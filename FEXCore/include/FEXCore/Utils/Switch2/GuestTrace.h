// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace FEXCore::Switch2 {
using GuestTraceHandler = void (*)(const char* Stage, uint64_t GuestRIP, uint64_t Detail);
inline GuestTraceHandler TraceHandler {};

inline void TraceGuest(const char* Stage, uint64_t GuestRIP, uint64_t Detail = 0) {
  if (TraceHandler) {
    TraceHandler(Stage, GuestRIP, Detail);
  }
}
} // namespace FEXCore::Switch2
