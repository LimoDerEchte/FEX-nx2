// SPDX-License-Identifier: MIT
#include "../GuestEpoll.h"
#include <cstdio>
#include <cstdlib>
#include <map>

#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      std::exit(1);                                           \
    }                                                         \
  } while (0)

using namespace Switch2Guest;

std::map<int, uint32_t> Ready;
std::map<int, uint64_t> Generation;

static size_t Collect(EpollSet<int>& Set, EpollEvent* Out, size_t Max) {
  return Set.Collect(Out, Max, [](int K) { return Ready[K]; }, [](int K) { return Generation[K]; });
}

int main() {
  EpollSet<int> Set;
  EpollEvent Out[4] {};
  CHECK(Set.Add(1, EpollIn, 11) == 0);
  CHECK(Set.Add(1, EpollIn, 11) == -17);
  CHECK(Set.Modify(2, EpollIn, 0) == -2);
  CHECK(Set.Remove(2) == -2);

  // Level-triggered: reported on every call while ready.
  CHECK(Collect(Set, Out, 4) == 0);
  Ready[1] = EpollIn | EpollOut;
  CHECK(Collect(Set, Out, 4) == 1 && Out[0].Events == EpollIn && Out[0].Data == 11);
  CHECK(Collect(Set, Out, 4) == 1);

  // ERR and HUP are always reported, even when not requested.
  Ready[1] = EpollHup | EpollOut;
  CHECK(Collect(Set, Out, 4) == 1 && Out[0].Events == EpollHup);

  // Edge-triggered: once per state change.
  CHECK(Set.Add(2, EpollIn | EpollEt, 22) == 0);
  Ready[1] = 0;
  Ready[2] = EpollIn;
  Generation[2] = 5;
  CHECK(Collect(Set, Out, 4) == 1 && Out[0].Data == 22);
  CHECK(Collect(Set, Out, 4) == 0);
  Generation[2] = 6;
  CHECK(Collect(Set, Out, 4) == 1);
  CHECK(!Set.AnyReady([](int K) { return Ready[K]; }, [](int K) { return Generation[K]; }));
  // MOD re-checks a ready ET item.
  CHECK(Set.Modify(2, EpollIn | EpollEt, 23) == 0);
  CHECK(Collect(Set, Out, 4) == 1 && Out[0].Data == 23);

  // One-shot: once until modified.
  CHECK(Set.Add(3, EpollIn | EpollOneShot, 33) == 0);
  Ready[2] = 0;
  Ready[3] = EpollIn;
  CHECK(Collect(Set, Out, 4) == 1 && Out[0].Data == 33);
  CHECK(Collect(Set, Out, 4) == 0);
  CHECK(Set.Modify(3, EpollIn | EpollOneShot, 34) == 0);
  CHECK(Collect(Set, Out, 4) == 1 && Out[0].Data == 34);

  // Round-robin with Max 1 between two ready level-triggered items.
  EpollSet<int> Pair;
  Pair.Add(10, EpollIn, 10);
  Pair.Add(20, EpollIn, 20);
  Ready[10] = Ready[20] = EpollIn;
  CHECK(Collect(Pair, Out, 1) == 1 && Out[0].Data == 10);
  CHECK(Collect(Pair, Out, 1) == 1 && Out[0].Data == 20);
  CHECK(Collect(Pair, Out, 1) == 1 && Out[0].Data == 10);
  Pair.Forget(10);
  CHECK(!Pair.Contains(10) && Pair.Size() == 1);
  CHECK(Collect(Pair, Out, 4) == 1 && Out[0].Data == 20);
  Pair.ForgetIf([](int K) { return K == 20; });
  CHECK(Pair.Size() == 0 && Collect(Pair, Out, 4) == 0);

  // EPOLLEXCLUSIVE rules.
  CHECK(CheckEpollExclusive(EpollCtlAdd, EpollIn | EpollExclusive, false) == 0);
  CHECK(CheckEpollExclusive(EpollCtlMod, EpollIn | EpollExclusive, false) == -22);
  CHECK(CheckEpollExclusive(EpollCtlAdd, EpollIn | EpollExclusive | EpollOneShot, false) == -22);
  CHECK(CheckEpollExclusive(EpollCtlAdd, EpollIn | EpollExclusive, true) == -22);
  CHECK(CheckEpollExclusive(EpollCtlDel, EpollExclusive, true) == 0);
  CHECK(Set.Add(4, EpollIn | EpollExclusive, 0) == 0);
  CHECK(Set.Modify(4, EpollIn, 0) == -22);
  std::puts("guest epoll checks passed");
}
