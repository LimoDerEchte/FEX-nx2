// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

namespace Switch2Guest {
template<class Clear, class Write, class Restore>
int64_t WriteGuestFileRange(uint64_t Allocated, uint64_t Visible, uint64_t Offset, std::span<const char> Bytes,
                            Clear ClearGap, Write WriteChunk, Restore RestoreLength) {
  auto Error = ClearGap(std::min(Allocated, Visible), Offset);
  uint64_t Written = 0;
  while (!Error && Written < Bytes.size()) {
    const auto Chunk = Bytes.subspan(Written, std::min<size_t>(Bytes.size() - Written, 65536));
    Error = WriteChunk(Offset + Written, Chunk);
    if (!Error) Written += Chunk.size();
  }
  if (Error) {
    const auto KeptLength = Written ? std::max(Allocated, Offset + Written) : Allocated;
    if (Offset + Bytes.size() > KeptLength) RestoreLength(KeptLength);
    if (!Written) return Error;
  }
  return Written;
}
} // namespace Switch2Guest
