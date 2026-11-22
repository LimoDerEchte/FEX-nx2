// SPDX-License-Identifier: MIT
#pragma once

#include "GuestElf.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace Switch2Guest {
// Guest addresses are directly usable host pointers. This table tracks Linux
// page permissions and limits fixed overlays to allocations owned by the guest.
class GuestAddressSpace {
public:
  struct Range { uint64_t Start; size_t Size; uint8_t Flags; };

  static constexpr size_t MaxMapping = 32 * 1024 * 1024;
  static constexpr uint8_t Unmapped = 0x80;

  void Register(uint64_t Start, size_t Size, uint8_t Flags) {
    const auto Base = Start & ~(PageSize - 1);
    Regions.emplace_back(Base, RoundUp(Start - Base + Size), Flags);
  }

  uint64_t Allocate(size_t Size, uint8_t Flags, bool Shared = false) {
    if (!Size || Size > MaxMapping) {
      return 0;
    }
    const size_t Rounded = RoundUp(Size);
    if (Rounded > MaxTotal - OwnedBytes) {
      return 0;
    }
    auto* Memory = std::aligned_alloc(PageSize, Rounded);
    if (!Memory) {
      return 0;
    }
    std::memset(Memory, 0, Rounded);
    const auto Address = reinterpret_cast<uint64_t>(Memory);
    Regions.emplace_back(Address, Rounded, Flags);
    Regions.back().Storage = std::shared_ptr<void> {Memory, &std::free};
    Regions.back().Shared = Shared;
    OwnedBytes += Rounded;
    return Address;
  }

  bool RegisterSharedFile(uint64_t Start, size_t Size, uint8_t Flags, std::shared_ptr<void> Storage, bool MayWrite = true) {
    if (!Storage || !Size || Start % PageSize || Size % PageSize || Size > UINT64_MAX - Start ||
        Size > MaxTotal - OwnedBytes) return false;
    for (const auto& Region : Regions) {
      if (Start < Region.Base + Region.Size && Region.Base < Start + Size) return false;
    }
    Regions.emplace_back(Start, Size, Flags);
    Regions.back().Storage = std::move(Storage);
    Regions.back().Shared = true;
    Regions.back().FileBacked = true;
    Regions.back().MayWrite = MayWrite;
    OwnedBytes += Size;
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

  // Registered stack and ELF pages are owned by the process image. The
  // process detaches their pipe references before releasing that image.
  std::shared_ptr<const uint8_t> RetainPage(uint64_t Address, size_t Size, bool& Shared) const {
    if (!Size || Size > PageSize - Address % PageSize) return {};
    for (auto It = Regions.rbegin(); It != Regions.rend(); ++It) {
      if (!It->Contains(Address, Size)) continue;
      if (It->Pages[(Address - It->Base) / PageSize] == Unmapped) return {};
      Shared = It->Shared;
      return {It->Storage, reinterpret_cast<const uint8_t*>(Address)};
    }
    return {};
  }

  bool Owns(uint64_t Start, size_t Size, bool ForOverlay = false) const {
    for (const auto& Region : Regions) {
      if (Region.Storage && (!ForOverlay || !Region.FileBacked) && Region.Contains(Start, Size)) {
        return true;
      }
    }
    return false;
  }

  bool IsMapped(uint64_t Start, size_t Size) const {
    if (!Size || Size > UINT64_MAX - Start) return false;
    const uint64_t End = Start + Size;
    for (auto Address = Start; Address < End; Address += PageSize) {
      if (!Query(Address).Size) return false;
    }
    return true;
  }

  Range Query(uint64_t Address) const {
    // At a shared ELF boundary, the final segment supplies page permissions.
    for (auto It = Regions.rbegin(); It != Regions.rend(); ++It) {
      const auto& Region = *It;
      if (!Region.Contains(Address, 1)) continue;
      const auto Page = (Address - Region.Base) / PageSize;
      const auto Flags = Region.Pages[Page];
      if (Flags == Unmapped) return {};
      size_t First = Page;
      size_t End = Page + 1;
      while (First && Region.Pages[First - 1] == Flags) --First;
      while (End < Region.Pages.size() && Region.Pages[End] == Flags) ++End;
      uint64_t FirstAddress = Region.Base + First * PageSize;
      uint64_t EndAddress = Region.Base + End * PageSize;
      for (auto Above = Regions.rbegin(); Above != It; ++Above) {
        if (Above->Base > Address) {
          EndAddress = std::min(EndAddress, Above->Base);
        } else {
          FirstAddress = std::max(FirstAddress, Above->Base + Above->Size);
        }
      }
      return {FirstAddress, static_cast<size_t>(EndAddress - FirstAddress), Flags};
    }
    return {};
  }

  bool Set(uint64_t Start, size_t Size, uint8_t Flags, bool OwnedOnly = false) {
    if (!Size || Start % PageSize || Size % PageSize || Size > UINT64_MAX - Start) return false;
    if (OwnedOnly ? !Owns(Start, Size) : !IsMapped(Start, Size)) return false;
    for (auto& Region : Regions) {
      if (OwnedOnly && !Region.Storage) continue;
      const auto First = std::max(Start, Region.Base);
      const auto End = std::min(Start + Size, Region.Base + Region.Size);
      if (First >= End) continue;
      std::fill(Region.Pages.begin() + (First - Region.Base) / PageSize,
                Region.Pages.begin() + (End - Region.Base) / PageSize, Flags);
    }
    if (Flags == Unmapped) {
      std::erase_if(Regions, [&](const auto& Region) {
        if (!Region.Storage || !std::all_of(Region.Pages.begin(), Region.Pages.end(), [](uint8_t Page) { return Page == Unmapped; })) {
          return false;
        }
        OwnedBytes -= Region.Size;
        return true;
      });
    }
    return true;
  }

  // Horizon guest pointers are direct host addresses. Save private bytes at
  // process switches so fork retains virtual addresses and independent data.
  bool SavePrivatePages(bool (*Validate)(uint64_t, size_t) = nullptr) {
    std::vector<SavedRange> New;
    for (const auto& Region : Regions) {
      if (Region.Shared) continue;
      const auto NeedsSave = [&](size_t Page) {
        return Region.Pages[Page] != Unmapped;
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
  static constexpr size_t MaxTotal = 96 * 1024 * 1024;
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
    std::vector<uint8_t> Pages;
    std::shared_ptr<void> Storage;
    bool Shared {};
    bool FileBacked {};
    bool MayWrite {};
  };
  struct SavedRange {
    uint64_t Start;
    size_t Size;
    std::shared_ptr<void> Bytes;
  };
  std::vector<Region> Regions;
  std::vector<SavedRange> Saved;
  size_t OwnedBytes {};
};
} // namespace Switch2Guest
