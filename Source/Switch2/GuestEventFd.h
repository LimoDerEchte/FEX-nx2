// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace Switch2Guest {
constexpr uint64_t EventFdMax = UINT64_MAX - 1;

struct EventCounter {
  uint64_t Value {};
  bool Semaphore {};
  uint64_t Changes {}; // counts reads and writes, for edge-triggered epoll

  // Returns 0 and stores the value read in Out, or -EAGAIN when Value == 0 (caller decides whether to block).
  // Semaphore mode returns 1 and decrements; otherwise returns Value and zeroes it.
  int64_t Read(uint64_t& Out) {
    if (!Value) return -11;
    if (Semaphore) {
      Out = 1;
      --Value;
    } else {
      Out = Value;
      Value = 0;
    }
    ++Changes;
    return 0;
  }

  // -EINVAL for UINT64_MAX; -EAGAIN when Value + Add would exceed EventFdMax; else adds and returns 0.
  int64_t Write(uint64_t Add) {
    if (Add == UINT64_MAX) return -22;
    if (Add > EventFdMax - Value) return -11;
    Value += Add;
    ++Changes;
    return 0;
  }

  bool Readable() const { return Value > 0; }
  bool Writable() const { return Value < EventFdMax; }
};
} // namespace Switch2Guest
