// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstdint>

namespace Switch2Guest {
struct FilePunchRange {
  uint64_t Begin, End;

  bool Empty() const { return Begin == End; }
};

struct FilePunch {
  FilePunchRange ClearBefore;
  FilePunchRange Reclaim;
  FilePunchRange ClearAfter;
};

inline FilePunch PlanFilePunch(uint64_t Allocated, uint64_t Begin, uint64_t End, uint64_t BlockSize) {
  const auto Limit = std::min(Allocated, End);
  Begin = std::min(Begin, Limit);
  const auto Leading = (BlockSize - Begin % BlockSize) % BlockSize;
  const auto FirstFull = Begin + std::min(Leading, Limit - Begin);
  const auto AfterFull = std::min(Allocated, End - End % BlockSize);
  if (FirstFull >= AfterFull) return {{Begin, Limit}, {Limit, Limit}, {Limit, Limit}};
  return {{Begin, FirstFull}, {FirstFull, AfterFull}, {AfterFull, Limit}};
}
} // namespace Switch2Guest
