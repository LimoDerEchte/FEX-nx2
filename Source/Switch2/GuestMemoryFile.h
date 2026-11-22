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
  static constexpr size_t MaxPages = GuestAddressSpace::MaxMapping / PageSize;
  std::shared_ptr<void> Storage;
  size_t Size {};
  uint32_t Seals {};
  std::vector<bool> Present;
  size_t HighWater {}; // Bytes at and above this offset are zero.

  bool WritesSealed() const { return Seals & (SealWrite | SealFutureWrite); }

  int64_t Reserve(size_t End, bool Grows = true) {
    if (Grows && End > Size && (Seals & SealGrow)) return -1;
    if (End > GuestAddressSpace::MaxMapping) return -27;
    if (End && !Storage) {
      Storage = std::shared_ptr<void> {
        std::aligned_alloc(PageSize, GuestAddressSpace::MaxMapping), &std::free};
      if (!Storage) return -12;
      std::memset(Storage.get(), 0, GuestAddressSpace::MaxMapping);
    }
    return 0;
  }

  void MarkAllocated(uint64_t Begin, uint64_t End) {
    End = std::min<uint64_t>(End, GuestAddressSpace::MaxMapping);
    if (Begin >= End) return;
    HighWater = std::max<size_t>(HighWater, GuestAddressSpace::RoundUp(End));
    if (Present.empty()) Present.resize(MaxPages);
    for (auto Page = Begin / PageSize; Page < GuestAddressSpace::RoundUp(End) / PageSize; ++Page) Present[Page] = true;
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
    auto* Data = static_cast<uint8_t*>(Storage.get());
    if (NewSize <= Size) {
      // Linux zeroes the partial page and frees every page beyond the new EOF,
      // including pages preallocated beyond the old EOF.
      const auto Kept = GuestAddressSpace::RoundUp(NewSize);
      if (HighWater > NewSize) std::memset(Data + NewSize, 0, HighWater - NewSize);
      HighWater = std::min(HighWater, Kept);
      for (auto Page = Kept / PageSize; Page < Present.size(); ++Page) Present[Page] = false;
    } else {
      // A shared mapping can store past EOF inside the last page.
      const auto Tail = std::min(GuestAddressSpace::RoundUp(Size), NewSize);
      std::memset(Data + Size, 0, Tail - Size);
    }
    Size = NewSize;
    return 0;
  }

  int64_t Allocate(uint64_t Begin, uint64_t End, bool KeepSize) {
    // shmem_fallocate checks the grow seal against EOF even with KEEP_SIZE.
    if (const auto Error = Reserve(End); Error) return Error;
    if (!KeepSize && End > Size) Size = End;
    MarkAllocated(Begin, End);
    return 0;
  }

  int64_t Write(uint64_t Offset, const void* Buffer, uint64_t Count) {
    if (!Count) return 0;
    if (WritesSealed()) return -1;
    if (Offset + Count > Size) {
      if (const auto Error = Reserve(Offset + Count); Error) return Error;
      Size = Offset + Count;
    }
    MarkAllocated(Offset, Offset + Count);
    std::memmove(static_cast<char*>(Storage.get()) + Offset, Buffer, Count);
    return 0;
  }

  int64_t Punch(uint64_t Begin, uint64_t End) {
    if (WritesSealed()) return -1;
    if (!Storage) return 0;
    const auto Plan = PlanFilePunch(GuestAddressSpace::MaxMapping, Begin, End, PageSize);
    auto* Bytes = static_cast<uint8_t*>(Storage.get());
    // Partial pages stay allocated. Shared mappings keep their pointers and
    // observe the zeroed bytes, matching a refault after the punch.
    for (const auto& Range : {Plan.ClearBefore, Plan.Reclaim, Plan.ClearAfter}) {
      const auto Limit = std::min<uint64_t>(Range.End, HighWater);
      if (Range.Begin < Limit) std::memset(Bytes + Range.Begin, 0, Limit - Range.Begin);
    }
    for (auto Page = Plan.Reclaim.Begin / PageSize; Page < Plan.Reclaim.End / PageSize && Page < Present.size(); ++Page) {
      Present[Page] = false;
    }
    return 0;
  }

  std::shared_ptr<void> Retain(size_t Offset, size_t Length) const {
    if (!Length || Offset >= Size || Length > Size - Offset) return {};
    return {Storage, static_cast<uint8_t*>(Storage.get()) + Offset};
  }
};
} // namespace Switch2Guest
