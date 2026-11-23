// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

namespace Switch2Guest {
enum class ClockKind { Realtime, Monotonic, Boottime, Tai, MonotonicRaw, RealtimeCoarse, MonotonicCoarse,
                       ProcessCpu, ThreadCpu, RealtimeAlarm, BoottimeAlarm, Invalid };

// Linux clockid decoding, including dynamic CPU clocks (include/linux/posix-timers.h):
//   MAKE_PROCESS_CPUCLOCK(pid, clock) = (~pid << 3) | clock; per-thread adds bit 4.
//   type 0 PROF, 1 VIRT, 2 SCHED; 3 (FD clocks) is invalid here. Pid 0 means the caller.
struct DecodedClock {
  ClockKind Kind;
  uint32_t Pid;
  bool PerThread;
  uint8_t CpuType; // 0 PROF, 1 VIRT, 2 SCHED; meaningful for ProcessCpu/ThreadCpu only
};

constexpr DecodedClock DecodeClock(int32_t Id) {
  if (Id < 0) {
    if ((Id & 3) == 3) return {ClockKind::Invalid, 0, false, 0};
    const bool PerThread = Id & 4;
    const uint32_t Pid = ~static_cast<uint32_t>(Id >> 3);
    return {PerThread ? ClockKind::ThreadCpu : ClockKind::ProcessCpu, Pid, PerThread, static_cast<uint8_t>(Id & 3)};
  }
  switch (Id) {
  case 0: return {ClockKind::Realtime, 0, false, 0};
  case 1: return {ClockKind::Monotonic, 0, false, 0};
  case 2: return {ClockKind::ProcessCpu, 0, false, 2}; // CLOCK_PROCESS_CPUTIME_ID is SCHED
  case 3: return {ClockKind::ThreadCpu, 0, true, 2}; // CLOCK_THREAD_CPUTIME_ID is SCHED
  case 4: return {ClockKind::MonotonicRaw, 0, false, 0};
  case 5: return {ClockKind::RealtimeCoarse, 0, false, 0};
  case 6: return {ClockKind::MonotonicCoarse, 0, false, 0};
  case 7: return {ClockKind::Boottime, 0, false, 0};
  // Deliberate difference: Linux returns EINVAL for the alarm clocks without a wakeup-capable RTC. The Switch 2 guest
  // has none either, but glue maps them to their non-alarm clocks for gettime/getres (accepted, tracked divergence).
  case 8: return {ClockKind::RealtimeAlarm, 0, false, 0};
  case 9: return {ClockKind::BoottimeAlarm, 0, false, 0};
  case 11: return {ClockKind::Tai, 0, false, 0};
  default: return {ClockKind::Invalid, 0, false, 0};
  }
}

constexpr uint64_t CoarseResolutionNs = 4'000'000; // CONFIG_HZ=250 on Debian kernels

constexpr uint64_t ResolutionNs(ClockKind Kind) {
  switch (Kind) {
  case ClockKind::RealtimeCoarse:
  case ClockKind::MonotonicCoarse: return CoarseResolutionNs;
  case ClockKind::Invalid: return 0;
  default: return 1;
  }
}

// posix-cpu-timers.c: PROF/VIRT clocks tick at 1/HZ (CONFIG_HZ=250); SCHED is ns precision.
constexpr uint64_t ResolutionNs(const DecodedClock& D) {
  if ((D.Kind == ClockKind::ProcessCpu || D.Kind == ClockKind::ThreadCpu)) return D.CpuType == 2 ? 1 : CoarseResolutionNs;
  return ResolutionNs(D.Kind);
}

constexpr uint64_t RoundCoarse(uint64_t Ns) { return Ns / CoarseResolutionNs * CoarseResolutionNs; }

struct RealtimeBase { // realtime = Base + (monotonic - MonoAtBase)
  int64_t BaseNs {};
  uint64_t MonoAtBase {};
  int64_t TaiOffsetNs {}; // CLOCK_TAI = realtime + offset (adjtimex ADJ_TAI; starts at 0)
  int64_t Now(uint64_t Mono) const { return BaseNs + static_cast<int64_t>(Mono - MonoAtBase); }
  int64_t TaiNow(uint64_t Mono) const { return Now(Mono) + TaiOffsetNs; }
  // ADJ_SETOFFSET: step realtime by DeltaNs as of Mono.
  void SetOffset(int64_t DeltaNs, uint64_t Mono) { Set(Now(Mono) + DeltaNs, Mono); }
  void Set(int64_t Realtime, uint64_t Mono) {
    BaseNs = Realtime;
    MonoAtBase = Mono;
  }
};
} // namespace Switch2Guest
