// SPDX-License-Identifier: MIT
// Timed preemption decisions for the cooperative guest scheduler.
//
// A watchdog thread decides when the running guest thread must return to
// the scheduler. It then sets the poll request word in the thread's FEXCore
// interrupt page; generated code polls it at block entries and backward
// branches and returns from ExecuteThread.
#pragma once
#include <cstdint>

namespace Switch2Guest {
constexpr uint64_t PreemptQuantumNs = 10'000'000;

struct PreemptInputs {
  uint64_t Now;
  uint64_t RunningSince;
  bool OtherRunnable; // another guest thread is ready or waits for a deadline
  bool SignalPending; // a caught signal is pending for the running thread
  bool TimerDue;      // a guest deadline has passed
  bool TerminalInput; // the interactive terminal may have input to process
};

constexpr bool ShouldPreempt(const PreemptInputs& In) {
  // Signals for the running thread are delivered as soon as possible.
  if (In.SignalPending) return true;
  if (In.Now - In.RunningSince < PreemptQuantumNs) return false;
  return In.OtherRunnable || In.TimerDue || In.TerminalInput;
}

} // namespace Switch2Guest
