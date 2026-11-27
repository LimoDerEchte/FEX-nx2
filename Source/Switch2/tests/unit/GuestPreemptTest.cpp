// SPDX-License-Identifier: MIT
#include "GuestPreempt.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(x)                                                     \
  do {                                                               \
    if (!(x)) {                                                      \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x);        \
      std::exit(1);                                                  \
    }                                                                \
  } while (0)

using namespace Switch2Guest;

int main() {
  constexpr uint64_t Q = PreemptQuantumNs;
  CHECK(!ShouldPreempt(Q - 1, 0));
  CHECK(ShouldPreempt(Q, 0));
  CHECK(ShouldPreempt(Q * 5, 0));
  // The quantum counts from when this thread entered guest code, not from zero.
  CHECK(!ShouldPreempt(Q + 99, 100));
  CHECK(ShouldPreempt(Q + 100, 100));
  CHECK(!ShouldPreempt(100, 100));

  std::puts("guest preempt: ok");
}
