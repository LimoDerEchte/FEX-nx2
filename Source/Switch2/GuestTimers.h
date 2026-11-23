// SPDX-License-Identifier: MIT
#pragma once
// Timer arithmetic shared by itimers, POSIX timers and timerfds, as Linux
// kernel/time/{itimer,posix-timers,alarmtimer}.c and fs/timerfd.c compute it.

#include <climits>
#include <cstdint>

namespace Switch2Guest {
struct TimerSpec {
  uint64_t ValueNs {}, IntervalNs {}; // a zero value means disarmed
};

struct ArmedTimer {
  uint64_t Deadline {}; // in the timer's clock domain (monotonic or CPU ns); 0 = disarmed
  uint64_t IntervalNs {};
  uint64_t Overrun {};

  bool Armed() const { return Deadline != 0; }

  void Arm(uint64_t Deadline_, uint64_t Interval) {
    Deadline = Deadline_ ? Deadline_ : 1;
    IntervalNs = Interval;
    Overrun = 0;
  }

  void Disarm() { Deadline = IntervalNs = Overrun = 0; }

  // Expiry at Now: the number of expirations since the last call (0 if the
  // deadline has not passed). A periodic timer keeps its phase: the next
  // deadline is the first Deadline + k * Interval after Now (hrtimer_forward).
  uint64_t Expire(uint64_t Now) {
    if (!Armed() || Now < Deadline) return 0;
    if (!IntervalNs) {
      Disarm();
      return 1;
    }
    const uint64_t Late = Now - Deadline;
    const uint64_t Count = Late / IntervalNs + 1;
    const uint64_t Step = Count > (UINT64_MAX - Deadline) / IntervalNs ? UINT64_MAX - Deadline : Count * IntervalNs;
    Deadline += Step;
    if (!Deadline) Deadline = UINT64_MAX;
    return Count;
  }

  // it_value is at least 1 ns while armed (common_timer_get), so an armed
  // timer never reads as disarmed.
  TimerSpec Remaining(uint64_t Now) const {
    if (!Armed()) return {};
    return {Deadline > Now ? Deadline - Now : 1, IntervalNs};
  }
};

// alarm(2) returns the previous remaining seconds, rounded to nearest, and
// at least 1 when any time remained (alarm_setitimer).
inline uint32_t AlarmSecondsLeft(uint64_t RemainingNs) {
  if (!RemainingNs) return 0;
  const uint64_t Seconds = RemainingNs / 1'000'000'000 + (RemainingNs % 1'000'000'000 >= 500'000'000);
  return Seconds ? static_cast<uint32_t>(Seconds > UINT32_MAX ? UINT32_MAX : Seconds) : 1;
}

// timeval to nanoseconds; microseconds convert exactly.
inline uint64_t FromTimeval(int64_t Seconds, int64_t Microseconds) {
  if (Seconds < 0) return 0;
  const uint64_t Limit = (UINT64_MAX - 999'999'000) / 1'000'000'000;
  if (static_cast<uint64_t>(Seconds) > Limit) return UINT64_MAX;
  return static_cast<uint64_t>(Seconds) * 1'000'000'000 + static_cast<uint64_t>(Microseconds) * 1000;
}

// Nanoseconds to a timeval, rounding up to the next microsecond as Linux
// getitimer does, so a running timer never reads as zero.
struct Timeval {
  int64_t Seconds, Microseconds;
};
inline Timeval ToTimevalUp(uint64_t Ns) {
  const uint64_t Micro = Ns / 1000 + (Ns % 1000 != 0);
  return {static_cast<int64_t>(Micro / 1'000'000), static_cast<int64_t>(Micro % 1'000'000)};
}

// timer_getoverrun and si_overrun clamp at DELAYTIMER_MAX.
inline int32_t ClampOverrun(uint64_t Overrun) {
  return Overrun > static_cast<uint64_t>(INT_MAX) ? INT_MAX : static_cast<int32_t>(Overrun);
}
} // namespace Switch2Guest
