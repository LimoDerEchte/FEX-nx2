// SPDX-License-Identifier: MIT
// libounce passes every fs path to the SD service as an IPC pointer buffer
// of FS_MAX_PATH (0x301) bytes taken straight from the caller's pointer
// (fsFsGetEntryType: no copy, descriptor size 0x301). The kernel reads all
// 0x301 bytes, so a path string that ends less than 0x301 bytes before an
// unmapped or non-IPC page fails with 2001-0106 (0xd401, InvalidMemoryState)
// every time, whatever its length. The RootFS install hit it at the same
// entry on every run (a heap string 640 bytes before a page boundary).
// SdPath copies a path into a full-size buffer first.
#pragma once

#include <cstring>
#include <switch.h>

namespace Switch2Guest {

struct SdPath {
  char Bytes[FS_MAX_PATH] {};
  explicit SdPath(const char* Path) {
    std::strncpy(Bytes, Path, sizeof(Bytes) - 1);
  }
  operator const char*() const {
    return Bytes;
  }
};

// Argument storage for an fs call, by the called function's parameter type:
// const char* parameters (paths) become an SdPath, everything else is
// passed through. Data buffers are const void*, so they are never copied.
template<typename Value>
struct SdArgument {
  Value Held;
  explicit SdArgument(Value Argument) : Held(Argument) {}
  Value Get() const {
    return Held;
  }
};

template<>
struct SdArgument<const char*> {
  SdPath Held;
  explicit SdArgument(const char* Argument) : Held(Argument) {}
  const char* Get() const {
    return Held;
  }
};

} // namespace Switch2Guest
