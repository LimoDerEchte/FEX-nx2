// SPDX-License-Identifier: MIT
#pragma once

#include "GuestAddressSpace.h"
#include "GuestFilePunch.h"

#include <optional>

namespace Switch2Guest {
// tmpfs-style memory file. The backing is one fixed reservation so guest
// mappings can keep direct pointers. Page presence follows Linux shmem:
// writes, fallocate and mappings allocate pages, while holes, punched pages
// and pages freed by truncation read as zero and count no blocks.
struct GuestMemoryFile {
  static constexpr uint32_t SealShrink = 2, SealGrow = 4, SealWrite = 8, SealFutureWrite = 16, SealExec = 32;
  // Largest file size. The file reserves arena address space for its size
  // (a power of two, at least MinReservation) on first use and commits pages
  // only as they become present. A larger size grows the reservation in place
  // or moves the file when nothing maps or references it.
  static constexpr size_t MaxFileSize = 1024 * 1024 * 1024;
  static constexpr size_t MaxPages = MaxFileSize / PageSize;
  static constexpr size_t MinReservation = 2 * GuestArena::Granule;
  std::shared_ptr<void> Storage;
  size_t Capacity {}; // reserved bytes at Base()
  std::shared_ptr<size_t> Reserved; // the reservation size the storage releases
  size_t Size {};
  uint32_t Seals {};
  std::vector<bool> Present;
  size_t HighWater {}; // Bytes at and above this offset are zero.
  GuestArena* Arena {GuestArena::Current()};

  bool WritesSealed() const { return Seals & (SealWrite | SealFutureWrite); }

  int64_t Reserve(size_t End, bool Grows = true) {
    if (Grows && End > Size && (Seals & SealGrow)) return -1;
    if (End > MaxFileSize) return -27;
    if (!End || End <= Capacity) return 0;
    if (!Arena) return -12;
    size_t Wanted = MinReservation;
    while (Wanted < End) Wanted *= 2;
    if (Storage && Arena->ReserveAt(Base() + Capacity, Wanted - Capacity)) {
      // Grown in place: the storage's deleter releases the whole reservation.
      *Reserved = Wanted;
      Capacity = Wanted;
      return 0;
    }
    // A mapping or pipe holds direct pointers into the storage: it cannot move.
    if (Storage && Storage.use_count() > 1) return -12;
    const auto Address = Arena->Reserve(Wanted, GuestArena::Granule);
    if (!Address) return -12;
    auto* Owner = Arena;
    auto Cell = std::make_shared<size_t>(Wanted);
    std::shared_ptr<void> Moved {reinterpret_cast<void*>(Address), [Owner, Cell](void* Pointer) {
      Owner->Release(reinterpret_cast<uint64_t>(Pointer), *Cell);
    }};
    // Copy the present pages over; the old reservation goes with Storage.
    for (size_t Page = 0; Page < Present.size() && Page * PageSize < Capacity;) {
      if (!Present[Page]) {
        ++Page;
        continue;
      }
      const auto Run = Page;
      while (Page < Present.size() && Present[Page]) ++Page;
      const auto Offset = Run * PageSize, Bytes = (Page - Run) * PageSize;
      if (!Arena->Commit(Address + Offset, Bytes)) return -12; // Moved releases what was committed
      std::memcpy(reinterpret_cast<void*>(Address + Offset), reinterpret_cast<const void*>(Base() + Offset), Bytes);
    }
    Storage = std::move(Moved);
    Reserved = std::move(Cell);
    Capacity = Wanted;
    return 0;
  }

  // Commits the pages covering [Begin, End) and counts them as allocated.
  // -28 (ENOSPC) when the arena has no memory left, as a full tmpfs would.
  int64_t MarkAllocated(uint64_t Begin, uint64_t End) {
    End = std::min<uint64_t>(End, MaxFileSize);
    if (Begin >= End) return 0;
    const auto FirstPage = Begin / PageSize, EndPage = GuestAddressSpace::RoundUp(End) / PageSize;
    if (Present.empty()) Present.resize(MaxPages);
    bool Missing {};
    for (auto Page = FirstPage; Page < EndPage && !Missing; ++Page) Missing = !Present[Page];
    if (Missing && !Arena->Commit(Base() + FirstPage * PageSize, (EndPage - FirstPage) * PageSize)) return -28;
    HighWater = std::max<size_t>(HighWater, EndPage * PageSize);
    for (auto Page = FirstPage; Page < EndPage; ++Page) Present[Page] = true;
    return 0;
  }

  uint64_t Base() const { return reinterpret_cast<uint64_t>(Storage.get()); }

  // Reads Count bytes at Offset inside the file's range; holes read zero.
  void Read(uint64_t Offset, void* Buffer, size_t Count) const {
    auto* Out = static_cast<uint8_t*>(Buffer);
    for (size_t Done = 0; Done < Count;) {
      const auto Here = Offset + Done;
      const auto Chunk = std::min<size_t>(Count - Done, PageSize - Here % PageSize);
      if (PagePresent(Here / PageSize)) std::memcpy(Out + Done, reinterpret_cast<const void*>(Base() + Here), Chunk);
      else std::memset(Out + Done, 0, Chunk);
      Done += Chunk;
    }
  }

  uint64_t AllocatedBytes() const {
    return static_cast<uint64_t>(std::count(Present.begin(), Present.end(), true)) * PageSize;
  }

  bool PagePresent(uint64_t Page) const { return Page < Present.size() && Present[Page]; }

  // SEEK_DATA and SEEK_HOLE over [Offset, Size), with the implicit hole at EOF.
  std::optional<uint64_t> Seek(uint64_t Offset, bool DataWanted) const {
    if (Offset >= Size) return std::nullopt;
    for (auto Page = Offset / PageSize; Page * PageSize < Size; ++Page) {
      if (PagePresent(Page) == DataWanted) return std::max<uint64_t>(Offset, Page * PageSize);
    }
    return DataWanted ? std::nullopt : std::optional<uint64_t> {Size};
  }

  int64_t Resize(size_t NewSize) {
    if (NewSize < Size && (Seals & SealShrink)) return -1;
    if (const auto Error = Reserve(NewSize); Error) return Error;
    if (!Storage) {
      Size = NewSize;
      return 0;
    }
    if (NewSize <= Size) {
      // Linux zeroes the partial page and frees every page beyond the new EOF,
      // including pages preallocated beyond the old EOF.
      const auto Kept = GuestAddressSpace::RoundUp(NewSize);
      if (HighWater > NewSize) ZeroPresent(NewSize, HighWater);
      HighWater = std::min(HighWater, Kept);
      Reclaim(Kept / PageSize, Present.size());
    } else {
      // A shared mapping can store past EOF inside the last page.
      const auto Tail = std::min(GuestAddressSpace::RoundUp(Size), NewSize);
      ZeroPresent(Size, Tail);
    }
    Size = NewSize;
    return 0;
  }

  int64_t Allocate(uint64_t Begin, uint64_t End, bool KeepSize) {
    // shmem_fallocate checks the grow seal against EOF even with KEEP_SIZE.
    if (const auto Error = Reserve(End); Error) return Error;
    if (const auto Error = MarkAllocated(Begin, End); Error) return Error;
    if (!KeepSize && End > Size) Size = End;
    return 0;
  }

  int64_t Write(uint64_t Offset, const void* Buffer, uint64_t Count) {
    if (!Count) return 0;
    if (WritesSealed()) return -1;
    if (Offset + Count > Size) {
      if (const auto Error = Reserve(Offset + Count); Error) return Error;
    } else if (const auto Error = Reserve(Offset + Count, false); Error) {
      return Error;
    }
    if (const auto Error = MarkAllocated(Offset, Offset + Count); Error) return Error;
    if (Offset + Count > Size) Size = Offset + Count;
    std::memmove(reinterpret_cast<void*>(Base() + Offset), Buffer, Count);
    return 0;
  }

  int64_t Punch(uint64_t Begin, uint64_t End) {
    if (WritesSealed()) return -1;
    if (!Storage) return 0;
    const auto Plan = PlanFilePunch(MaxFileSize, Begin, End, PageSize);
    // Partial pages stay allocated. Shared mappings keep their pointers and
    // observe the zeroed bytes, matching a refault after the punch.
    for (const auto& Range : {Plan.ClearBefore, Plan.Reclaim, Plan.ClearAfter}) {
      const auto Limit = std::min<uint64_t>(Range.End, HighWater);
      if (Range.Begin < Limit) ZeroPresent(Range.Begin, Limit);
    }
    Reclaim(Plan.Reclaim.Begin / PageSize, std::min<size_t>(Plan.Reclaim.End / PageSize, Present.size()));
    return 0;
  }

  std::shared_ptr<void> Retain(size_t Offset, size_t Length) const {
    if (!Length || Offset >= Size || Length > Size - Offset) return {};
    return {Storage, static_cast<uint8_t*>(Storage.get()) + Offset};
  }

private:
  // Zeroes the committed pages inside [Begin, End). Pages that were never
  // committed hold nothing and must not be touched.
  void ZeroPresent(uint64_t Begin, uint64_t End) {
    for (auto Offset = Begin; Offset < End;) {
      const auto Chunk = std::min<uint64_t>(End - Offset, PageSize - Offset % PageSize);
      if (Arena->IsCommitted(Base() + Offset, 1)) std::memset(reinterpret_cast<void*>(Base() + Offset), 0, Chunk);
      Offset += Chunk;
    }
  }

  // Frees pages [First, End). A page that a mapping or pipe still references
  // stays committed (zeroed) because a stray access would fault the CPU; the
  // arena returns whole granules once the last user lets go.
  void Reclaim(size_t First, size_t End) {
    if (!Storage || Present.empty()) return;
    const bool Give = Storage.use_count() == 1;
    for (auto Page = First; Page < End && Page < Present.size(); ++Page) {
      if (!Present[Page]) continue;
      Present[Page] = false;
      if (Give) Arena->Decommit(Base() + Page * PageSize, PageSize);
      else ZeroPresent(Page * PageSize, (Page + 1) * PageSize);
    }
  }
};
} // namespace Switch2Guest
