// SPDX-License-Identifier: MIT
#include "../GuestPreempt.h"
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
  CHECK(!ShouldPreempt({Q - 1, 0, true, false, false, false}));
  CHECK(ShouldPreempt({Q, 0, true, false, false, false}));
  CHECK(ShouldPreempt({1, 0, false, true, false, false}));
  CHECK(!ShouldPreempt({Q * 5, 0, false, false, false, false}));
  CHECK(ShouldPreempt({Q, 0, false, false, true, false}));
  CHECK(ShouldPreempt({Q + 100, 100, false, false, false, true}));
  CHECK(!ShouldPreempt({Q + 99, 100, false, false, false, true}));

  std::puts("guest preempt: ok");
}
