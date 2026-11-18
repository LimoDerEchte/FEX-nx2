// SPDX-License-Identifier: MIT
#pragma once

// The umbrella SDK header defines a global Event type that conflicts with FEX.
extern "C" {
#include <switch/kernel/jit.h>
#include <switch/result.h>
#include <kaihen/protocol.h>
Result kaihenJitPublishRanges(Jit* Code, const KaihenJitRange* Ranges, size_t Count);
}

#include <cstdint>

namespace FEXCore::Switch2 {
class JitMemory {
public:
  JitMemory() = default;
  JitMemory(const JitMemory&) = delete;
  JitMemory& operator=(const JitMemory&) = delete;

  ~JitMemory() {
    if (Open) {
      Close();
    }
  }

  Result Create(size_t Size) {
    const Result Status = jitCreate(&Code, Size);
    Open = R_SUCCEEDED(Status);
    return Status;
  }

  Result BeginWrite() {
    return jitTransitionToWritable(&Code);
  }

  Result Publish() {
    return jitTransitionToExecutable(&Code);
  }

  Result PublishRange(size_t Offset, size_t Length) {
    const KaihenJitRange Range {Offset, Length};
    return kaihenJitPublishRanges(&Code, &Range, 1);
  }

  Result Close() {
    Open = false;
    return jitClose(&Code);
  }

  uint8_t* Writable() {
    return static_cast<uint8_t*>(jitGetRwAddr(&Code));
  }

  uint8_t* Executable() {
    return static_cast<uint8_t*>(jitGetRxAddr(&Code));
  }

  size_t Size() const {
    return Code.size;
  }

private:
  Jit Code {};
  bool Open {};
};
} // namespace FEXCore::Switch2
