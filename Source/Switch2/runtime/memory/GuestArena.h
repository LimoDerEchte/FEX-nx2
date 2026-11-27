// SPDX-License-Identifier: MIT
#pragma once

#include "GuestElf.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Switch2Guest {
// Physical memory behind the arena. The Horizon backend (HostArena.cpp) maps
// and unmaps physical memory in the alias region; host tests use mmap.
struct ArenaBackend {
  virtual ~ArenaBackend() = default;
  virtual bool Commit(uint64_t Address, size_t Size) = 0;   // map zeroed memory, RW
  virtual bool Decommit(uint64_t Address, size_t Size) = 0; // return memory
};

// Guest address space reservation. Guest addresses are host pointers into one
// reserved range. Address space is reserved first-fit and costs nothing;
// pages are committed on demand. The backend commits whole granules, so the
// arena counts committed pages per granule and drops a granule with its last
// page. Memory is never touched unless it is committed: a CPU fault ends the
// app, so callers must check IsCommitted first.
class GuestArena {
public:
  static constexpr size_t Granule = 2 * 1024 * 1024; // commit unit (probe verified 2 MiB)

  GuestArena(ArenaBackend& Backend, uint64_t Base, size_t Size, size_t CommitLimit)
    : Backend {Backend}
    , Base {Base}
    , Size {Size & ~(Granule - 1)}
    , Limit {CommitLimit}
    , Pages(this->Size / PageSize)
    , GranulePages(this->Size / Granule)
    , Held(this->Size / Granule) {
    if (this->Size) Free.emplace(Base, this->Size);
  }

  GuestArena(const GuestArena&) = delete;
  GuestArena& operator=(const GuestArena&) = delete;

  // The arena that address spaces and memory files use unless given one.
  static GuestArena*& Current() {
    static GuestArena* Arena {};
    return Arena;
  }

  uint64_t Reserve(size_t Bytes, size_t Alignment = PageSize) {
    if (!Bytes || Bytes > Size) return 0;
    Bytes = RoundUp(Bytes);
    Alignment = std::max<size_t>(Alignment, PageSize);
    for (auto It = Free.begin(); It != Free.end(); ++It) {
      const uint64_t Start = (It->first + Alignment - 1) & ~(Alignment - 1);
      const uint64_t End = It->first + It->second;
      if (Start >= End || End - Start < Bytes) continue;
      Take(It, Start, Bytes);
      return Start;
    }
    return 0;
  }

  // Claims a specific free range (growing a mapping in place).
  bool ReserveAt(uint64_t Address, size_t Bytes) {
    if (!Bytes || Address % PageSize || !Contains(Address, RoundUp(Bytes))) return false;
    Bytes = RoundUp(Bytes);
    auto It = Free.upper_bound(Address);
    if (It == Free.begin()) return false;
    --It;
    if (It->first + It->second < Address + Bytes) return false;
    Take(It, Address, Bytes);
    return true;
  }

  bool Release(uint64_t Address, size_t Bytes) {
    if (!Bytes || Address % PageSize || !Contains(Address, RoundUp(Bytes))) return false;
    Bytes = RoundUp(Bytes);
    auto Next = Free.lower_bound(Address);
    if (Next != Free.end() && Next->first < Address + Bytes) return false;
    if (Next != Free.begin()) {
      const auto Previous = std::prev(Next);
      if (Previous->first + Previous->second > Address) return false;
    }
    Decommit(Address, Bytes);
    auto Start = Address;
    auto Length = Bytes;
    if (Next != Free.end() && Next->first == Address + Bytes) {
      Length += Next->second;
      Next = Free.erase(Next);
    }
    if (Next != Free.begin()) {
      const auto Previous = std::prev(Next);
      if (Previous->first + Previous->second == Address) {
        Start = Previous->first;
        Length += Previous->second;
        Free.erase(Previous);
      }
    }
    Free.emplace(Start, Length);
    return true;
  }

  bool Contains(uint64_t Address, size_t Bytes) const {
    return Address >= Base && Address - Base <= Size && Bytes <= Size - (Address - Base);
  }

  // Commits the pages covering [Address, Address + Bytes). New pages read zero.
  bool Commit(uint64_t Address, size_t Bytes) {
    if (!Bytes || !Contains(Address, RoundDown(Address + Bytes - 1) + PageSize - Address)) return false;
    const auto First = PageIndex(RoundDown(Address));
    const auto End = PageIndex(RoundDown(Address + Bytes - 1)) + 1;
    // Granules that gain their first page.
    std::vector<size_t> Fresh;
    for (auto G = First / PagesPerGranule; G <= (End - 1) / PagesPerGranule; ++G) {
      if (GranulePages[G] || Held[G]) continue;
      bool Gains {};
      for (auto Page = std::max(First, G * PagesPerGranule); Page < std::min(End, (G + 1) * PagesPerGranule); ++Page) {
        if (!Pages[Page]) {
          Gains = true;
          break;
        }
      }
      if (Gains) Fresh.push_back(G);
    }
    if (Fresh.size() * Granule > Limit || Committed() > Limit - Fresh.size() * Granule) return false;
    for (size_t Done = 0; Done < Fresh.size(); ++Done) {
      if (!Backend.Commit(Base + Fresh[Done] * Granule, Granule)) {
        for (size_t Undo = 0; Undo < Done; ++Undo) Backend.Decommit(Base + Fresh[Undo] * Granule, Granule);
        return false;
      }
    }
    CommittedGranules += Fresh.size();
    for (auto Page = First; Page < End;) {
      if (Pages[Page]) {
        ++Page;
        continue;
      }
      const auto Run = Page;
      while (Page < End && !Pages[Page]) {
        Pages[Page] = true;
        Held[Page / PagesPerGranule] = false; // a granule the backend kept is in use again
        ++GranulePages[Page / PagesPerGranule];
        ++Page;
      }
      std::memset(reinterpret_cast<void*>(Base + Run * PageSize), 0, (Page - Run) * PageSize);
    }
    return true;
  }

  // Drops the pages covering the range. A granule is returned when its last page goes.
  void Decommit(uint64_t Address, size_t Bytes) {
    if (!Bytes) return;
    const auto Start = std::max(RoundDown(Address), Base);
    const auto Stop = std::min(RoundUp(Address + Bytes), Base + Size);
    if (Start >= Stop) return;
    for (auto Page = PageIndex(Start); Page < PageIndex(Stop); ++Page) {
      if (!Pages[Page]) continue;
      Pages[Page] = false;
      const auto G = Page / PagesPerGranule;
      if (--GranulePages[G] == 0) {
        // A granule the backend failed to unmap stays mapped and counted;
        // the next commit reuses it and the next release retries.
        if (Backend.Decommit(Base + G * Granule, Granule)) --CommittedGranules;
        else Held[G] = true;
      }
    }
  }

  bool IsCommitted(uint64_t Address, size_t Bytes) const {
    if (!Bytes || Address + Bytes < Address) return false;
    const auto Start = RoundDown(Address), Stop = RoundUp(Address + Bytes);
    if (!Contains(Start, Stop - Start)) return false;
    for (auto Page = PageIndex(Start); Page < PageIndex(Stop); ++Page) {
      if (!Pages[Page]) return false;
    }
    return true;
  }

  size_t Committed() const { return CommittedGranules * Granule; }
  size_t CommitLimit() const { return Limit; }
  uint64_t BaseAddress() const { return Base; }
  size_t VirtualSize() const { return Size; }

private:
  static constexpr size_t PagesPerGranule = Granule / PageSize;
  static constexpr uint64_t RoundDown(uint64_t Value) { return Value & ~(PageSize - 1); }
  static constexpr uint64_t RoundUp(uint64_t Value) { return (Value + PageSize - 1) & ~(PageSize - 1); }
  size_t PageIndex(uint64_t Address) const { return (Address - Base) / PageSize; }

  void Take(std::map<uint64_t, size_t>::iterator It, uint64_t Start, size_t Bytes) {
    const auto BlockStart = It->first;
    const auto BlockEnd = It->first + It->second;
    Free.erase(It);
    if (Start > BlockStart) Free.emplace(BlockStart, Start - BlockStart);
    if (Start + Bytes < BlockEnd) Free.emplace(Start + Bytes, BlockEnd - (Start + Bytes));
  }

  ArenaBackend& Backend;
  uint64_t Base;
  size_t Size;
  size_t Limit;
  size_t CommittedGranules {};
  std::map<uint64_t, size_t> Free;  // start -> size, address ordered
  std::vector<bool> Pages;          // committed
  std::vector<uint16_t> GranulePages; // committed pages per granule
  std::vector<bool> Held;            // empty granules the backend could not decommit (still mapped)
};

// HostArena.cpp (Horizon) and the host tests supply a backend. The Horizon
// factory reserves the alias region minus its first GiB and reports its range.
std::unique_ptr<ArenaBackend> MakeHostArenaBackend(uint64_t& Base, size_t& Size);
// TotalMemorySize - UsedMemorySize when the arena is created, minus 64 MiB; at most 6 GiB.
// HostArena.cpp shrinks the loader's heap to 2 GiB at startup so this is not 0.
size_t HostArenaCommitLimit();
// One log line: whether the loader's heap was shrunk, sizes, and memory totals.
std::string HostHeapReport();
// Gives the loader its heap size back; the arena must have returned its memory first.
bool HostHeapRestore();
} // namespace Switch2Guest
