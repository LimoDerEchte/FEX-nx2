// SPDX-License-Identifier: MIT
#pragma once

#include "GuestArena.h"
#include "GuestElf.h"

#include <algorithm>
#include <cstring>
#include <elf.h>
#include <optional>
#include <utility>
#include <vector>

namespace Switch2Guest {
// Guest addresses are directly usable host pointers. This table tracks Linux
// page permissions and limits fixed overlays to allocations owned by the guest.
//
// Owned memory comes from the GuestArena: Allocate reserves address space and
// commits pages only while they are accessible. A CPU fault ends the app, so
// no page that is not committed may ever be read or written. The table keeps
// the invariant that a page with any of R, W or X is committed, and Available
// reports zero bytes for anything else.
class GuestAddressSpace {
public:
  struct Range { uint64_t Start; size_t Size; uint8_t Flags; };
  enum class SetStatus { Ok, Invalid, NoMemory };
  enum class RemapStatus { Ok, Fault, Invalid, NoMemory };

  static constexpr uint8_t Unmapped = 0x80;
  // Page attributes that ride along with the protection bits.
  static constexpr uint16_t ProtMask = 0x07;
  static constexpr uint16_t Resident = 0x08;   // committed in this process's view (arena pages)
  static constexpr uint16_t DontFork = 0x10;   // MADV_DONTFORK: absent in the fork child
  static constexpr uint16_t WipeOnFork = 0x20; // MADV_WIPEONFORK: zero in the fork child
  static constexpr uint16_t Sealed = 0x40;     // mseal
  static constexpr uint16_t Locked = 0x100;    // mlock accounting

  GuestAddressSpace() = default;
  explicit GuestAddressSpace(GuestArena& Arena) : Arena {&Arena} {}

  // Largest mapping that could ever be committed.
  size_t MaxMapping() const { return Arena ? Arena->CommitLimit() : 0; }
  GuestArena* GetArena() const { return Arena; }

  void Register(uint64_t Start, size_t Size, uint8_t Flags) {
    const auto Base = Start & ~(PageSize - 1);
    Regions.emplace_back(Base, RoundUp(Start - Base + Size), Flags);
  }

  // Reserves address space and commits it unless the mapping is inaccessible.
  uint64_t Allocate(size_t Size, uint8_t Flags, bool Shared = false, size_t Alignment = PageSize) {
    if (!Size || !Arena || Size > Arena->VirtualSize()) return 0;
    const size_t Rounded = RoundUp(Size);
    if (Flags && Rounded > MaxMapping()) return 0;
    const auto Address = Arena->Reserve(Rounded, Alignment);
    if (!Address) return 0;
    if (Flags && !Arena->Commit(Address, Rounded)) {
      Arena->Release(Address, Rounded);
      return 0;
    }
    AddOwned(Address, Rounded, Flags, Shared);
    return Address;
  }

  // Reserves address space whose pages all start unmapped (the brk area above
  // the break). The region stays reserved while every page is unmapped, so
  // pages can be mapped inside it later; the reservation goes with the table.
  uint64_t ReserveUnmapped(size_t Size) {
    if (!Size || !Arena || Size > Arena->VirtualSize()) return 0;
    const size_t Rounded = RoundUp(Size);
    const auto Address = Arena->Reserve(Rounded, PageSize);
    if (!Address) return 0;
    AddOwned(Address, Rounded, 0, false);
    std::fill(Regions.back().Pages.begin(), Regions.back().Pages.end(), static_cast<uint16_t>(Unmapped));
    Regions.back().KeepEmpty = true;
    return Address;
  }

  // Maps a specific free range of the arena (MAP_FIXED outside owned memory).
  // Returns false when the range is not free arena address space.
  bool AllocateAt(uint64_t Address, size_t Size, uint8_t Flags, bool Shared = false) {
    if (!Size || !Arena || Address % PageSize || Size % PageSize) return false;
    if (Flags && Size > MaxMapping()) return false;
    if (!Arena->ReserveAt(Address, Size)) return false;
    if (Flags && !Arena->Commit(Address, Size)) {
      Arena->Release(Address, Size);
      return false;
    }
    AddOwned(Address, Size, Flags, Shared);
    return true;
  }

  // True when any page of the range is mapped.
  bool AnyMapped(uint64_t Start, size_t Size) const {
    for (const auto& Region : Regions) {
      const auto [First, End] = Overlap(Region, Start, Size);
      for (auto Page = First; Page < End; ++Page) {
        if (!(Region.Pages[Page] & Unmapped)) return true;
      }
    }
    return false;
  }

  // The maximal runs of mapped pages inside the range (holes skipped).
  std::vector<std::pair<uint64_t, size_t>> MappedRuns(uint64_t Start, size_t Size) const {
    std::vector<std::pair<uint64_t, size_t>> Runs;
    if (!Size || Size > UINT64_MAX - Start) return Runs;
    // Visit only pages some region holds: the range may span most of the address space.
    std::vector<uint64_t> Candidates;
    for (const auto& Region : Regions) {
      const auto [First, Last] = Overlap(Region, Start, Size);
      for (auto Page = First; Page < Last; ++Page) {
        if (!(Region.Pages[Page] & Unmapped)) Candidates.push_back(Region.Base + Page * PageSize);
      }
    }
    std::sort(Candidates.begin(), Candidates.end());
    Candidates.erase(std::unique(Candidates.begin(), Candidates.end()), Candidates.end());
    for (const auto Address : Candidates) {
      const auto Top = TopIndex(Address);
      if (Top == NoRegion || (Regions[Top].Pages[(Address - Regions[Top].Base) / PageSize] & Unmapped)) continue;
      if (!Runs.empty() && Runs.back().first + Runs.back().second == Address) Runs.back().second += PageSize;
      else Runs.emplace_back(Address, PageSize);
    }
    return Runs;
  }

  // Mapped pages in the range that have the attribute.
  size_t CountHave(uint64_t Start, size_t Size, uint16_t Bit) const {
    size_t Count {};
    for (const auto& Region : Regions) {
      const auto [First, End] = Overlap(Region, Start, Size);
      for (auto Page = First; Page < End; ++Page) Count += !(Region.Pages[Page] & Unmapped) && (Region.Pages[Page] & Bit);
    }
    return Count;
  }

  size_t MappedBytes() const {
    size_t Pages {};
    for (const auto& Region : Regions) {
      Pages += std::count_if(Region.Pages.begin(), Region.Pages.end(), [](uint16_t Byte) { return !(Byte & Unmapped); });
    }
    return Pages * PageSize;
  }

  // The storage object behind the page at Address (file-backed regions).
  std::shared_ptr<void> StorageAt(uint64_t Address) const {
    const auto Top = TopIndex(Address);
    return Top == NoRegion ? nullptr : Regions[Top].Storage;
  }

  bool RegionMayWrite(uint64_t Address) const {
    const auto Top = TopIndex(Address);
    return Top != NoRegion && Regions[Top].MayWrite;
  }

  // Whether a region of this process shares the storage.
  bool Maps(const std::shared_ptr<void>& Storage) const {
    return std::any_of(Regions.begin(), Regions.end(), [&](const auto& Region) { return Region.Storage == Storage; });
  }

  bool RegisterSharedFile(uint64_t Start, size_t Size, uint8_t Flags, std::shared_ptr<void> Storage, bool MayWrite = true) {
    if (!Storage || !Size || Start % PageSize || Size % PageSize || Size > UINT64_MAX - Start) return false;
    for (const auto& Region : Regions) {
      if (Start < Region.Base + Region.Size && Region.Base < Start + Size) return false;
    }
    Regions.emplace_back(Start, Size, Flags);
    Regions.back().Storage = std::move(Storage);
    Regions.back().Shared = true;
    Regions.back().FileBacked = true;
    Regions.back().MayWrite = MayWrite;
    return true;
  }

  bool HasWritableFileMapping(const std::shared_ptr<void>& Storage) const {
    return std::any_of(Regions.begin(), Regions.end(), [&](const auto& Region) {
      return Region.FileBacked && Region.MayWrite && Region.Storage == Storage;
    });
  }

  bool AllowsFileMappingWrite(uint64_t Start, size_t Size) const {
    return std::none_of(Regions.begin(), Regions.end(), [&](const auto& Region) {
      return Region.FileBacked && !Region.MayWrite && Start < Region.Base + Region.Size && Region.Base < Start + Size;
    });
  }

  // True when some mapped file-backed region (or one backed by Storage) covers the range.
  bool IsFileBacked(uint64_t Start, size_t Size) const {
    return std::any_of(Regions.begin(), Regions.end(), [&](const auto& Region) {
      return Region.FileBacked && Start < Region.Base + Region.Size && Region.Base < Start + Size;
    });
  }

  // Anonymous or file mappings that more than one view shares (MAP_SHARED).
  bool IsShared(uint64_t Start, size_t Size) const {
    return std::any_of(Regions.begin(), Regions.end(), [&](const auto& Region) {
      return Region.Shared && Start < Region.Base + Region.Size && Region.Base < Start + Size;
    });
  }

  // True when every page in the range is private anonymous memory the guest owns.
  bool IsPrivateAnonymous(uint64_t Start, size_t Size) const {
    return Owns(Start, Size) && !IsShared(Start, Size) && !IsFileBacked(Start, Size);
  }

  // Registered stack and ELF pages are owned by the process image. The
  // process detaches their pipe references before releasing that image.
  std::shared_ptr<const uint8_t> RetainPage(uint64_t Address, size_t Size, bool& Shared) const {
    if (!Size || Size > PageSize - Address % PageSize) return {};
    for (auto It = Regions.rbegin(); It != Regions.rend(); ++It) {
      if (!It->Contains(Address, Size)) continue;
      const auto Flags = It->Pages[(Address - It->Base) / PageSize];
      if ((Flags & Unmapped) || !Resident_(*It, Flags)) return {};
      Shared = It->Shared;
      return {It->Storage, reinterpret_cast<const uint8_t*>(Address)};
    }
    return {};
  }

  // Whether the guest owns every byte of the range, across adjacent allocations.
  bool Owns(uint64_t Start, size_t Size, bool ForOverlay = false) const {
    if (Start + Size < Start) return false;
    const auto End = Start + Size;
    auto Address = Start;
    do {
      bool Found {};
      for (const auto& Region : Regions) {
        if (!Region.Storage || (ForOverlay && Region.FileBacked) || !Region.Contains(Address, 0) ||
            (Address == Region.Base + Region.Size && Size)) {
          continue;
        }
        Address = std::min<uint64_t>(End, Region.Base + Region.Size);
        Found = true;
        break;
      }
      if (!Found) return false;
    } while (Address < End);
    return true;
  }

  bool IsMapped(uint64_t Start, size_t Size) const {
    if (!Size || Size > UINT64_MAX - Start) return false;
    const uint64_t End = Start + Size;
    for (auto Address = Start; Address < End;) {
      const auto Run = Query(Address);
      if (!Run.Size) return false;
      Address = Run.Start + Run.Size;
    }
    return true;
  }

  // The maximal run of pages around Address with the same protection.
  Range Query(uint64_t Address) const {
    const auto Page = Address & ~(PageSize - 1);
    const auto Top = TopIndex(Page);
    if (Top == NoRegion) return {};
    const auto Flags = Prot(Regions[Top].Pages[(Page - Regions[Top].Base) / PageSize]);
    if (Flags & Unmapped) return {};
    const auto Start = ScanBackward(Page, Flags);
    const auto End = ScanForward(Page, Flags);
    return {Start, static_cast<size_t>(End - Start), static_cast<uint8_t>(Flags)};
  }

  // Bytes from Address that are mapped with all of Need and committed.
  size_t Available(uint64_t Address, uint8_t Need) const {
    const auto Page = Address & ~(PageSize - 1);
    const auto Top = TopIndex(Page);
    if (Top == NoRegion) return 0;
    const auto& Region = Regions[Top];
    const auto Flags = Prot(Region.Pages[(Page - Region.Base) / PageSize]);
    if ((Flags & Unmapped) || (Flags & Need) != Need) return 0;
    if (Need && !Resident_(Region, Region.Pages[(Page - Region.Base) / PageSize])) return 0;
    return ScanForward(Page, Flags) - Address;
  }

  // Whether every page in the range is mapped and committed.
  bool IsCommitted(uint64_t Start, size_t Size) const {
    if (!Size || Size > UINT64_MAX - Start) return false;
    const auto End = RoundUp(Start + Size);
    for (auto Address = Start & ~(PageSize - 1); Address < End; Address += PageSize) {
      if (!PageCommitted(Address)) return false;
    }
    return true;
  }

  bool PageCommitted(uint64_t Address) const {
    const auto Top = TopIndex(Address);
    if (Top == NoRegion) return false;
    const auto Flags = Regions[Top].Pages[(Address - Regions[Top].Base) / PageSize];
    return !(Flags & Unmapped) && Resident_(Regions[Top], Flags);
  }

  // Protection changes. Adding access to an uncommitted page commits it
  // first; NoMemory leaves every page unchanged. Unmapped releases pages and
  // drops fully unmapped owned regions.
  SetStatus SetPages(uint64_t Start, size_t Size, uint8_t Flags, bool OwnedOnly = false) {
    if (!Size || Start % PageSize || Size % PageSize || Size > UINT64_MAX - Start) return SetStatus::Invalid;
    if (OwnedOnly ? !Owns(Start, Size) : !IsMapped(Start, Size)) return SetStatus::Invalid;
    if (Flags == Unmapped) {
      for (auto& Region : Regions) {
        if (OwnedOnly && !Region.Storage) continue;
        const auto [First, End] = Overlap(Region, Start, Size);
        if (First >= End) continue;
        ReleasePages(Region, First, End);
        for (auto Page = First; Page < End; ++Page) Region.Pages[Page] = Unmapped;
      }
      std::erase_if(Regions, [&](const auto& Region) {
        return Region.Storage && !Region.KeepEmpty && std::all_of(Region.Pages.begin(), Region.Pages.end(), [](uint16_t Page) { return Page & Unmapped; });
      });
      return SetStatus::Ok;
    }
    if (!CommitFor(Start, Size, Flags, OwnedOnly)) return SetStatus::NoMemory;
    for (auto& Region : Regions) {
      if (OwnedOnly && !Region.Storage) continue;
      const auto [First, End] = Overlap(Region, Start, Size);
      for (auto Page = First; Page < End; ++Page) {
        auto& Byte = Region.Pages[Page];
        if (Byte & Unmapped) {
          Byte = Flags | (Flags && Region.Arena ? Resident : 0);
        } else {
          Byte = (Byte & ~ProtMask) | Flags | (Flags && Region.Arena ? Resident : 0);
        }
      }
    }
    return SetStatus::Ok;
  }

  bool Set(uint64_t Start, size_t Size, uint8_t Flags, bool OwnedOnly = false) {
    return SetPages(Start, Size, Flags, OwnedOnly) == SetStatus::Ok;
  }

  // Gives a range of owned pages back to the arena and keeps them mapped
  // (inaccessible and zero on the next commit). Used by MADV_DONTNEED on
  // private anonymous memory and by brk shrinking.
  void Discard(uint64_t Start, size_t Size) {
    for (auto& Region : Regions) {
      if (!Region.Storage) continue;
      const auto [First, End] = Overlap(Region, Start, Size);
      if (First < End) ReleasePages(Region, First, End);
    }
  }

  // Zeroes committed pages of owned memory without changing the table.
  void Zero(uint64_t Start, size_t Size) {
    for (auto Address = Start & ~(PageSize - 1); Address < Start + Size; Address += PageSize) {
      if (PageCommitted(Address) && Owns(Address, PageSize)) std::memset(reinterpret_cast<void*>(Address), 0, PageSize);
    }
  }

  // Per-page attributes. Returns the pages that changed.
  size_t ChangeAttribute(uint64_t Start, size_t Size, uint16_t Bit, bool On) {
    size_t Changed {};
    for (auto& Region : Regions) {
      const auto [First, End] = Overlap(Region, Start, Size);
      for (auto Page = First; Page < End; ++Page) {
        auto& Byte = Region.Pages[Page];
        if (Byte & Unmapped) continue;
        if (static_cast<bool>(Byte & Bit) == On) continue;
        Byte = On ? Byte | Bit : Byte & ~Bit;
        ++Changed;
      }
    }
    return Changed;
  }

  bool AllHave(uint64_t Start, size_t Size, uint16_t Bit) const {
    return AllPages(Start, Size, [&](uint16_t Byte) { return Byte & Bit; });
  }
  bool AnyHave(uint64_t Start, size_t Size, uint16_t Bit) const {
    bool Any {};
    for (const auto& Region : Regions) {
      const auto [First, End] = Overlap(Region, Start, Size);
      for (auto Page = First; Page < End && !Any; ++Page) Any = !(Region.Pages[Page] & Unmapped) && (Region.Pages[Page] & Bit);
    }
    return Any;
  }

  size_t LockedBytes() const {
    size_t Pages {};
    for (const auto& Region : Regions) {
      Pages += std::count_if(Region.Pages.begin(), Region.Pages.end(),
                             [](uint16_t Byte) { return !(Byte & Unmapped) && (Byte & Locked); });
    }
    return Pages * PageSize;
  }

  // mremap of a single owned private mapping (Linux order: Fault when the old
  // range is not one mapping, then the request is placed). Sizes are page
  // multiples. FixedStart moves onto owned address space. NewAddress receives
  // the result. Shared and file-backed mappings shrink in place only.
  RemapStatus Remap(uint64_t OldStart, size_t OldSize, size_t NewSize, bool MayMove, bool DontUnmap,
                    std::optional<uint64_t> FixedStart, uint64_t& NewAddress) {
    const auto Run = Query(OldStart);
    if (!OldSize || OldStart % PageSize || OldSize % PageSize || OldSize > UINT64_MAX - OldStart || !Run.Size ||
        Run.Start > OldStart || Run.Start + Run.Size < OldStart + OldSize) {
      return RemapStatus::Fault;
    }
    const auto OldEnd = OldStart + OldSize;
    const auto First = TopIndex(OldStart);
    const uint16_t Head = Regions[First].Pages[(OldStart - Regions[First].Base) / PageSize];
    bool Movable = true, Extendable = true, AnyShared = false;
    for (const auto& Part : Regions) {
      if (!(OldStart < Part.Base + Part.Size && Part.Base < OldEnd)) continue;
      Extendable = Extendable && Part.Arena && Part.Storage && !Part.FileBacked;
      Movable = Movable && Extendable && !Part.Shared;
      AnyShared = AnyShared || Part.Shared;
    }
    // MREMAP_DONTUNMAP of a shared mapping is EINVAL here (Linux 7.3 allows it): one object cannot be mapped twice.
    if (DontUnmap && AnyShared) return RemapStatus::Invalid;
    if (!FixedStart && !DontUnmap && NewSize <= OldSize) {
      if (NewSize < OldSize && SetPages(OldStart + NewSize, OldSize - NewSize, Unmapped, true) != SetStatus::Ok) {
        // A registered image page cannot be partially unmapped by remapping.
        return RemapStatus::Invalid;
      }
      NewAddress = OldStart;
      return RemapStatus::Ok;
    }
    if (!FixedStart && !DontUnmap && NewSize > OldSize) {
      // Grow in place into unmapped pages of the same reservation, or into
      // free address space right after it.
      const auto Extra = NewSize - OldSize;
      const auto Last = TopIndex(OldEnd - 1);
      auto& Region = Regions[Last];
      if (Extendable && Extra <= Region.Size - (OldEnd - Region.Base)) {
        const auto FirstPage = (OldEnd - Region.Base) / PageSize;
        bool Free = true;
        for (auto Page = FirstPage; Page < FirstPage + Extra / PageSize; ++Page) Free = Free && (Region.Pages[Page] & Unmapped);
        if (Free) {
          if (!CommitFor(OldEnd, Extra, Head & ProtMask, true)) return RemapStatus::NoMemory;
          for (auto Page = FirstPage; Page < FirstPage + Extra / PageSize; ++Page) {
            Region.Pages[Page] = Inherited(Head) | (Head & ProtMask) | (Head & ProtMask ? Resident : 0);
          }
          NewAddress = OldStart;
          return RemapStatus::Ok;
        }
      }
      if (Extendable && Arena && Arena->ReserveAt(OldEnd, Extra)) {
        const bool SharedTail = Region.Shared;
        if ((Head & ProtMask) && !Arena->Commit(OldEnd, Extra)) {
          Arena->Release(OldEnd, Extra);
          return RemapStatus::NoMemory;
        }
        AddOwned(OldEnd, Extra, Head & ProtMask, SharedTail);
        Regions.back().Pages.assign(Extra / PageSize, Inherited(Head) | (Head & ProtMask) | (Head & ProtMask ? Resident : 0));
        NewAddress = OldStart;
        return RemapStatus::Ok;
      }
      if (!MayMove || !Movable) return RemapStatus::NoMemory;
    } else if (!Movable) {
      return RemapStatus::NoMemory;
    }

    // Move (or duplicate onto fresh address space) a private anonymous mapping.
    uint64_t Target = 0;
    bool FreshTarget {};
    if (FixedStart) {
      Target = *FixedStart;
      if (Target % PageSize) return RemapStatus::NoMemory;
      if (Target < OldEnd && OldStart < Target + NewSize) return RemapStatus::Invalid;
      if (!Owns(Target, NewSize, true)) {
        // Free arena address space is claimed outright; anything else (an
        // image page or space outside the arena) cannot be replaced.
        if (AnyMapped(Target, NewSize) || NewSize > UINT64_MAX - Target ||
            !AllocateAt(Target, NewSize, static_cast<uint8_t>(Head & ProtMask))) {
          return RemapStatus::NoMemory;
        }
        for (auto& Page : Regions.back().Pages) Page = Inherited(Head) | (Head & ProtMask) | (Head & ProtMask ? Resident : 0);
        FreshTarget = true;
      } else {
        // The target pages become fresh memory. Keep the reservation: only the
        // contents and the per-page attributes go.
        Discard(Target, NewSize);
        ChangeAttribute(Target, NewSize, DontFork | WipeOnFork | Locked, false);
      }
    }
    if (!FixedStart) {
      Target = Allocate(NewSize, Head & ProtMask, false);
      if (!Target) return RemapStatus::NoMemory;
      for (auto& Page : Regions.back().Pages) Page = Inherited(Head) | (Head & ProtMask) | (Head & ProtMask ? Resident : 0);
    } else if (!FreshTarget && SetPages(Target, NewSize, static_cast<uint8_t>(Head & ProtMask), true) != SetStatus::Ok) {
      // The target was discarded above: leaving its old protection on
      // uncommitted pages would let the next access fault the CPU. Linux has
      // already unmapped the target at this point, so leave a hole.
      SetPages(Target, NewSize, Unmapped, true);
      return RemapStatus::NoMemory;
    }
    const auto Copy = std::min(OldSize, NewSize);
    for (size_t Offset = 0; Offset < Copy; Offset += PageSize) {
      if (PageCommitted(OldStart + Offset) && PageCommitted(Target + Offset)) {
        std::memcpy(reinterpret_cast<void*>(Target + Offset), reinterpret_cast<const void*>(OldStart + Offset), PageSize);
      }
    }
    if (DontUnmap) {
      Zero(OldStart, OldSize);
    } else {
      SetPages(OldStart, OldSize, Unmapped, true);
    }
    NewAddress = Target;
    return RemapStatus::Ok;
  }

  // Copies between a process's memory and a buffer. Resident means the
  // process's bytes are live in memory; otherwise they sit in its saved
  // private pages. Stops at the first page that is unmapped, uncommitted or
  // lacks access, and returns the bytes done.
  size_t CopyFromProcess(uint64_t Address, void* Out, size_t Size, bool Live) const {
    size_t Done {};
    while (Done < Size) {
      const auto Here = Address + Done;
      const auto Chunk = std::min<size_t>(Size - Done, PageSize - Here % PageSize);
      const auto* Source = Access(Here, Chunk, PF_R, Live);
      if (!Source) break;
      std::memcpy(static_cast<uint8_t*>(Out) + Done, Source, Chunk);
      Done += Chunk;
    }
    return Done;
  }

  size_t CopyToProcess(uint64_t Address, const void* In, size_t Size, bool Live) {
    size_t Done {};
    while (Done < Size) {
      const auto Here = Address + Done;
      const auto Chunk = std::min<size_t>(Size - Done, PageSize - Here % PageSize);
      auto* Target = const_cast<uint8_t*>(Access(Here, Chunk, PF_W, Live, true));
      if (!Target) break;
      std::memcpy(Target, static_cast<const uint8_t*>(In) + Done, Chunk);
      Done += Chunk;
    }
    return Done;
  }

  // The address space a fork child starts with: DONTFORK pages are absent,
  // WIPEONFORK pages read zero, and nothing is locked. Take it after
  // SavePrivatePages so the child's saved copy exists.
  GuestAddressSpace ForkChild() const {
    GuestAddressSpace Child = *this;
    for (auto& Region : Child.Regions) {
      for (auto& Byte : Region.Pages) {
        if (Byte & Unmapped) continue;
        if (Byte & DontFork) {
          Byte = Unmapped;
          continue;
        }
        Byte &= ~Locked;
      }
    }
    for (auto& Saved : Child.Saved) {
      bool Wipes {};
      for (size_t Offset = 0; Offset < Saved.Size && !Wipes; Offset += PageSize) {
        Wipes = Child.PageHas(Saved.Start + Offset, WipeOnFork);
      }
      if (!Wipes) continue;
      // The bytes are shared with the parent's copy: unshare before wiping.
      std::shared_ptr<void> Copy {std::malloc(Saved.Size), &std::free};
      if (!Copy) continue;
      std::memcpy(Copy.get(), Saved.Bytes.get(), Saved.Size);
      for (size_t Offset = 0; Offset < Saved.Size; Offset += PageSize) {
        if (Child.PageHas(Saved.Start + Offset, WipeOnFork)) std::memset(static_cast<uint8_t*>(Copy.get()) + Offset, 0, PageSize);
      }
      Saved.Bytes = std::move(Copy);
    }
    // Pages gone from the child lose their saved bytes; keep them harmlessly
    // (the shared reservation still holds the committed pages).
    return Child;
  }

  // Horizon guest pointers are direct host addresses. Save private bytes at
  // process switches so fork retains virtual addresses and independent data.
  // Pages that are not committed hold nothing and are skipped.
  bool SavePrivatePages(bool (*Validate)(uint64_t, size_t) = nullptr) {
    std::vector<SavedRange> New;
    for (const auto& Region : Regions) {
      if (Region.Shared) continue;
      const auto NeedsSave = [&](size_t Page) {
        return !(Region.Pages[Page] & Unmapped) && Resident_(Region, Region.Pages[Page]);
      };
      for (size_t Page = 0; Page < Region.Pages.size();) {
        if (!NeedsSave(Page)) {
          ++Page;
          continue;
        }
        const size_t First = Page++;
        while (Page < Region.Pages.size() && NeedsSave(Page)) ++Page;
        const auto Start = Region.Base + First * PageSize;
        const size_t Size = (Page - First) * PageSize;
        if (Validate && !Validate(Start, Size)) return false;
        std::shared_ptr<void> Bytes {std::malloc(Size), &std::free};
        if (!Bytes) return false;
        std::memcpy(Bytes.get(), reinterpret_cast<const void*>(Start), Size);
        New.push_back({Start, Size, std::move(Bytes)});
      }
    }
    Saved = std::move(New);
    return true;
  }

  void RestorePrivatePages() const {
    for (const auto& Range : Saved) {
      std::memcpy(reinterpret_cast<void*>(Range.Start), Range.Bytes.get(), Range.Size);
    }
  }

  static constexpr size_t RoundUp(size_t Size) { return (Size + PageSize - 1) & ~(PageSize - 1); }

private:
  static constexpr size_t NoRegion = SIZE_MAX;
  struct Region {
    Region(uint64_t Base, size_t Size, uint8_t Flags)
      : Base {Base}
      , Size {Size}
      , Pages(Size / PageSize, Flags) {}

    bool Contains(uint64_t Start, size_t Length) const {
      return Start >= Base && Start - Base <= Size && Length <= Size - (Start - Base);
    }

    uint64_t Base;
    size_t Size;
    std::vector<uint16_t> Pages;
    std::shared_ptr<void> Storage;
    bool Shared {};
    bool FileBacked {};
    bool MayWrite {};
    bool Arena {}; // address space and commitment come from the GuestArena
    bool KeepEmpty {}; // stays reserved when every page is unmapped (ReserveUnmapped)
  };
  struct SavedRange {
    uint64_t Start;
    size_t Size;
    std::shared_ptr<void> Bytes;
  };

  static constexpr uint16_t Prot(uint16_t Byte) { return Byte & (ProtMask | Unmapped); }
  // Attributes a page keeps when its mapping grows or moves.
  static constexpr uint16_t Inherited(uint16_t Byte) { return Byte & (DontFork | WipeOnFork | Locked); }
  // Pages outside the arena (images, external storage) are always resident.
  static bool Resident_(const Region& Region, uint16_t Byte) { return !Region.Arena || (Byte & Resident); }

  void AddOwned(uint64_t Address, size_t Size, uint8_t Flags, bool Shared) {
    auto* Owner = Arena;
    Regions.emplace_back(Address, Size, Flags | (Flags ? Resident : 0));
    Regions.back().Storage = std::shared_ptr<void> {reinterpret_cast<void*>(Address),
                                                    [Owner, Address, Size](void*) { Owner->Release(Address, Size); }};
    Regions.back().Shared = Shared;
    Regions.back().Arena = true;
  }

  static std::pair<size_t, size_t> Overlap(const Region& Region, uint64_t Start, size_t Size) {
    const auto First = std::max(Start, Region.Base);
    const auto End = std::min(Start + Size, Region.Base + Region.Size);
    if (First >= End) return {0, 0};
    return {(First - Region.Base) / PageSize, (End - Region.Base) / PageSize};
  }

  template<typename Predicate>
  bool AllPages(uint64_t Start, size_t Size, Predicate Test) const {
    for (const auto& Region : Regions) {
      const auto [First, End] = Overlap(Region, Start, Size);
      for (auto Page = First; Page < End; ++Page) {
        if (!(Region.Pages[Page] & Unmapped) && !Test(Region.Pages[Page])) return false;
      }
    }
    return true;
  }

  bool PageHas(uint64_t Address, uint16_t Bit) const {
    const auto Top = TopIndex(Address);
    return Top != NoRegion && !(Regions[Top].Pages[(Address - Regions[Top].Base) / PageSize] & Unmapped) &&
           (Regions[Top].Pages[(Address - Regions[Top].Base) / PageSize] & Bit);
  }

  size_t TopIndex(uint64_t Address) const {
    for (size_t Index = Regions.size(); Index--;) {
      if (Regions[Index].Contains(Address, 1)) return Index;
    }
    return NoRegion;
  }

  uint64_t ScanForward(uint64_t Start, uint16_t Flags) const {
    auto Address = Start;
    for (;;) {
      const auto Top = TopIndex(Address);
      if (Top == NoRegion) break;
      const auto& Region = Regions[Top];
      if (Prot(Region.Pages[(Address - Region.Base) / PageSize]) != Flags) break;
      auto Limit = Region.Base + Region.Size;
      for (auto Above = Top + 1; Above < Regions.size(); ++Above) {
        if (Regions[Above].Base > Address) Limit = std::min(Limit, Regions[Above].Base);
      }
      auto Page = (Address - Region.Base) / PageSize;
      const auto EndPage = (Limit - Region.Base) / PageSize;
      while (Page < EndPage && Prot(Region.Pages[Page]) == Flags) ++Page;
      Address = Region.Base + Page * PageSize;
      if (Page < EndPage) break;
    }
    return Address;
  }

  uint64_t ScanBackward(uint64_t Start, uint16_t Flags) const {
    auto Address = Start;
    while (Address >= PageSize) {
      const auto Last = Address - PageSize;
      const auto Top = TopIndex(Last);
      if (Top == NoRegion) break;
      const auto& Region = Regions[Top];
      if (Prot(Region.Pages[(Last - Region.Base) / PageSize]) != Flags) break;
      auto Lower = Region.Base;
      for (auto Above = Top + 1; Above < Regions.size(); ++Above) {
        const auto AboveEnd = Regions[Above].Base + Regions[Above].Size;
        if (AboveEnd <= Last) Lower = std::max(Lower, AboveEnd);
      }
      auto Page = (Last - Region.Base) / PageSize;
      const auto LowPage = (Lower - Region.Base) / PageSize;
      while (Page > LowPage && Prot(Region.Pages[Page - 1]) == Flags) --Page;
      Address = Region.Base + Page * PageSize;
      if (Page > LowPage) break;
    }
    return Address;
  }

  // Pages the table still maps get backing before they gain access. A page
  // that another process view already committed is zeroed for this view.
  bool CommitFor(uint64_t Start, size_t Size, uint8_t Flags, bool OwnedOnly) {
    if (!Flags || !Arena) return true;
    struct Run { uint64_t Start; size_t Size; std::vector<std::pair<uint64_t, size_t>> Fresh; };
    std::vector<Run> Done;
    const auto Rollback = [&] {
      for (const auto& Run : Done) {
        for (const auto& [Address, Length] : Run.Fresh) Arena->Decommit(Address, Length);
      }
    };
    for (auto& Region : Regions) {
      if (!Region.Arena || (OwnedOnly && !Region.Storage)) continue;
      const auto [First, End] = Overlap(Region, Start, Size);
      for (auto Page = First; Page < End;) {
        if (Region.Pages[Page] & Resident) {
          ++Page;
          continue;
        }
        const auto RunFirst = Page;
        while (Page < End && !(Region.Pages[Page] & Resident)) ++Page;
        Run New {Region.Base + RunFirst * PageSize, (Page - RunFirst) * PageSize, {}};
        // Pages the arena already holds were committed through another view.
        std::vector<std::pair<uint64_t, bool>> Held;
        for (auto Address = New.Start; Address < New.Start + New.Size; Address += PageSize) {
          Held.emplace_back(Address, Arena->IsCommitted(Address, PageSize));
        }
        if (!Arena->Commit(New.Start, New.Size)) {
          Rollback();
          return false;
        }
        for (const auto& [Address, Was] : Held) {
          if (!Was) {
            if (!New.Fresh.empty() && New.Fresh.back().first + New.Fresh.back().second == Address) New.Fresh.back().second += PageSize;
            else New.Fresh.emplace_back(Address, PageSize);
          } else if (!Region.Shared) {
            std::memset(reinterpret_cast<void*>(Address), 0, PageSize);
          }
        }
        Done.push_back(std::move(New));
      }
    }
    return true;
  }

  // Clears committed pages in [First, End) of a region. The arena keeps the
  // memory when another process view still holds the reservation.
  void ReleasePages(Region& Region, size_t First, size_t End) {
    if (!Region.Arena) return;
    const bool Give = Region.Storage.use_count() == 1 && !Region.FileBacked;
    for (auto Page = First; Page < End;) {
      if (Region.Pages[Page] & Unmapped || !(Region.Pages[Page] & Resident)) {
        ++Page;
        continue;
      }
      const auto Run = Page;
      while (Page < End && !(Region.Pages[Page] & Unmapped) && (Region.Pages[Page] & Resident)) {
        Region.Pages[Page] &= ~Resident;
        ++Page;
      }
      if (Give && Arena) Arena->Decommit(Region.Base + Run * PageSize, (Page - Run) * PageSize);
    }
  }

  // The bytes of one page of this process's view, or null.
  const uint8_t* Access(uint64_t Address, size_t Chunk, uint8_t Need, bool Live, bool ForWrite = false) const {
    const auto Top = TopIndex(Address);
    if (Top == NoRegion) return nullptr;
    const auto& Region = Regions[Top];
    const auto Byte = Region.Pages[(Address - Region.Base) / PageSize];
    if ((Byte & Unmapped) || (Byte & Need) != Need || !Resident_(Region, Byte)) return nullptr;
    if (Live || Region.Shared) return reinterpret_cast<const uint8_t*>(Address);
    for (const auto& Range : Saved) {
      if (Address >= Range.Start && Address - Range.Start + Chunk <= Range.Size) {
        auto* Bytes = const_cast<SavedRange&>(Range).Bytes.get();
        if (ForWrite && Range.Bytes.use_count() > 1) {
          // Saved bytes are shared with a fork relative: take a private copy first.
          std::shared_ptr<void> Copy {std::malloc(Range.Size), &std::free};
          if (!Copy) return nullptr;
          std::memcpy(Copy.get(), Bytes, Range.Size);
          const_cast<SavedRange&>(Range).Bytes = std::move(Copy);
          Bytes = const_cast<SavedRange&>(Range).Bytes.get();
        }
        return static_cast<const uint8_t*>(Bytes) + (Address - Range.Start);
      }
    }
    return nullptr;
  }

  GuestArena* Arena {GuestArena::Current()};
  std::vector<Region> Regions;
  std::vector<SavedRange> Saved;
};
} // namespace Switch2Guest
