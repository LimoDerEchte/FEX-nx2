// SPDX-License-Identifier: MIT
#include "GuestTimers.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      std::exit(1);                                           \
    }                                                         \
  } while (0)

using namespace Switch2Guest;

int main() {
  ArmedTimer Once;
  Once.Arm(100, 0);
  CHECK(Once.Expire(99) == 0 && Once.Armed());
  CHECK(Once.Expire(100) == 1 && !Once.Armed());
  CHECK(Once.Expire(1000) == 0);

  ArmedTimer Periodic;
  Periodic.Arm(100, 10);
  CHECK(Periodic.Expire(135) == 4);        // 100, 110, 120, 130
  CHECK(Periodic.Deadline == 140);         // keeps the original phase
  CHECK(Periodic.Expire(139) == 0);
  CHECK(Periodic.Expire(140) == 1 && Periodic.Deadline == 150);

  CHECK(Periodic.Remaining(149).ValueNs == 1 && Periodic.Remaining(149).IntervalNs == 10);
  CHECK(Periodic.Remaining(150).ValueNs == 1);   // due but not yet expired: still armed
  CHECK(Periodic.Remaining(160).ValueNs == 1);
  CHECK(Periodic.Remaining(120).ValueNs == 30);
  Periodic.Disarm();
  CHECK(Periodic.Remaining(0).ValueNs == 0 && Periodic.Remaining(0).IntervalNs == 0);

  ArmedTimer Huge;
  Huge.Arm(UINT64_MAX - 5, UINT64_MAX / 2);
  CHECK(Huge.Expire(UINT64_MAX) == 1 && Huge.Deadline == UINT64_MAX);

  CHECK(AlarmSecondsLeft(0) == 0);
  CHECK(AlarmSecondsLeft(1) == 1);
  CHECK(AlarmSecondsLeft(1'499'999'999) == 1);
  CHECK(AlarmSecondsLeft(1'500'000'000) == 2);
  CHECK(AlarmSecondsLeft(3'000'000'000) == 3);

  CHECK(FromTimeval(0, 1) == 1000);
  CHECK(FromTimeval(2, 500'000) == 2'500'000'000);
  CHECK(ToTimevalUp(1).Seconds == 0 && ToTimevalUp(1).Microseconds == 1);
  CHECK(ToTimevalUp(1'000'000'000).Seconds == 1 && ToTimevalUp(1'000'000'000).Microseconds == 0);
  CHECK(ToTimevalUp(1'000'000'001).Microseconds == 1);

  CHECK(ClampOverrun(UINT64_MAX) == INT_MAX);
  CHECK(ClampOverrun(7) == 7);
  std::puts("guest timer checks passed");
}
