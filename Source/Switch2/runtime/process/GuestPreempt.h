// SPDX-License-Identifier: MIT
// Timed preemption decisions for the guest threads.
//
// Every guest thread runs on its own host thread, but Horizon does not time-slice host threads of one priority: with
// more guest threads than cores a CPU-bound thread would hold its core forever. A watchdog thread therefore decides
// per thread when it must give its core back. It then sets the poll request word in the thread's FEXCore interrupt
// page; generated code polls it at block entries and backward branches and returns from ExecuteThread.
#pragma once
#include <cstdint>

namespace Switch2Guest {
constexpr uint64_t PreemptQuantumNs = 10'000'000;

// A thread that has been in guest code for a whole quantum since it entered (Now and RunningSince are the same clock).
// Other threads, timers and terminal input need no input here: they have host threads or the service thread of their own.
constexpr bool ShouldPreempt(uint64_t Now, uint64_t RunningSince) {
  return Now - RunningSince >= PreemptQuantumNs;
}

} // namespace Switch2Guest
