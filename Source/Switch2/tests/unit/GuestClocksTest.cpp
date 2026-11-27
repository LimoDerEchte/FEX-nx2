// SPDX-License-Identifier: MIT
#include "GuestClocks.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#define CHECK(Cond) do { if (!(Cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #Cond); std::exit(1); } } while (0)

using namespace Switch2Guest;

int main() {
  const ClockKind Kinds[] = {ClockKind::Realtime, ClockKind::Monotonic, ClockKind::ProcessCpu, ClockKind::ThreadCpu,
                             ClockKind::MonotonicRaw, ClockKind::RealtimeCoarse, ClockKind::MonotonicCoarse,
                             ClockKind::Boottime, ClockKind::RealtimeAlarm, ClockKind::BoottimeAlarm,
                             ClockKind::Invalid, ClockKind::Tai};
  for (int32_t Id = 0; Id < 12; ++Id) {
    const auto D = DecodeClock(Id);
    CHECK(D.Kind == Kinds[Id]);
    CHECK(D.Pid == 0 && D.PerThread == (Id == 3));
  }
  CHECK(DecodeClock(10).Kind == ClockKind::Invalid);
  CHECK(DecodeClock(12).Kind == ClockKind::Invalid);
  CHECK(DecodeClock(INT32_MAX).Kind == ClockKind::Invalid);

  // Linux: MAKE_PROCESS_CPUCLOCK(pid, clock) = (~pid << 3) | clock; thread adds bit 4.
  // The brief's literal ~(100 << 3) | 2 does not follow this macro (low 3 bits are already 7), so it is not used.
  auto Process = [](int32_t Pid, int32_t Type) { return static_cast<int32_t>((~static_cast<uint32_t>(Pid) << 3) | Type); };
  auto Thread = [&](int32_t Tid, int32_t Type) { return Process(Tid, Type) | 4; };
  for (int Type = 0; Type < 3; ++Type) {
    auto P = DecodeClock(Process(100, Type));
    CHECK(P.Kind == ClockKind::ProcessCpu && P.Pid == 100 && !P.PerThread);
    auto T = DecodeClock(Thread(5, Type));
    CHECK(T.Kind == ClockKind::ThreadCpu && T.Pid == 5 && T.PerThread);
  }
  // pid 0 = calling process / thread; CLOCK_PROCESS_CPUTIME_ID style negative ids
  CHECK(DecodeClock(Process(0, 2)).Kind == ClockKind::ProcessCpu && DecodeClock(Process(0, 2)).Pid == 0);
  // type 3 (FD clocks) and -1 are invalid
  CHECK(DecodeClock(Process(100, 3)).Kind == ClockKind::Invalid);
  CHECK(DecodeClock(Thread(5, 3)).Kind == ClockKind::Invalid);
  CHECK(DecodeClock(-1).Kind == ClockKind::Invalid);
  {
    const auto M = DecodeClock(INT32_MIN); // finding 10
    CHECK(M.Kind == ClockKind::ProcessCpu && M.Pid == 0x0FFFFFFF && M.CpuType == 0 && !M.PerThread);
  }
  // finding 8: CPU clock type kept; resolution per type
  for (int Type = 0; Type < 3; ++Type) {
    CHECK(DecodeClock(Process(100, Type)).CpuType == Type && DecodeClock(Thread(5, Type)).CpuType == Type);
    CHECK(ResolutionNs(DecodeClock(Process(100, Type))) == (Type == 2 ? 1u : 4'000'000u));
    CHECK(ResolutionNs(DecodeClock(Thread(5, Type))) == (Type == 2 ? 1u : 4'000'000u));
  }
  CHECK(DecodeClock(2).CpuType == 2 && DecodeClock(3).CpuType == 2);
  CHECK(ResolutionNs(DecodeClock(2)) == 1 && ResolutionNs(DecodeClock(3)) == 1);
  CHECK(ResolutionNs(DecodeClock(0)) == 1 && ResolutionNs(DecodeClock(5)) == 4'000'000 && ResolutionNs(DecodeClock(10)) == 0);

  CHECK(CoarseResolutionNs == 4'000'000);
  CHECK(ResolutionNs(ClockKind::RealtimeCoarse) == 4'000'000);
  CHECK(ResolutionNs(ClockKind::MonotonicCoarse) == 4'000'000);
  for (auto K : {ClockKind::Realtime, ClockKind::Monotonic, ClockKind::Boottime, ClockKind::Tai, ClockKind::MonotonicRaw,
                 ClockKind::ProcessCpu, ClockKind::ThreadCpu, ClockKind::RealtimeAlarm, ClockKind::BoottimeAlarm})
    CHECK(ResolutionNs(K) == 1);

  RealtimeBase B {1'000'000'000'000, 500};
  CHECK(B.Now(500) == 1'000'000'000'000);
  CHECK(B.Now(1500) == 1'000'000'001'000);
  B.Set(42, 10'000);
  CHECK(B.Now(10'000) == 42 && B.Now(10'007) == 49);
  CHECK(B.TaiOffsetNs == 0 && B.TaiNow(10'007) == 49);
  B.TaiOffsetNs = 37'000'000'000;
  CHECK(B.TaiNow(10'007) == 37'000'000'049);
  B.SetOffset(-9, 10'007); // ADJ_SETOFFSET: realtime moves by delta now
  CHECK(B.Now(10'007) == 40 && B.Now(10'008) == 41);
  RealtimeBase Z; // finding 11: value-initialised
  CHECK(Z.BaseNs == 0 && Z.MonoAtBase == 0 && Z.TaiOffsetNs == 0);
  CHECK(RoundCoarse(0) == 0 && RoundCoarse(3'999'999) == 0 && RoundCoarse(4'000'000) == 4'000'000 && RoundCoarse(9'123'456) == 8'000'000);
  std::puts("GuestClocks: OK");
}
