# Milestone 2 — Processes and Events Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement Linux process, session, signal, credential, clock, timer, polling and watch semantics in the Switch 2 guest runtime so stock Bash pipelines, job control, traps and Debian `sleep`, `timeout`, `kill` and `inotifywait` behave as on Linux through the normal launcher.

**Architecture:** The runtime is one host thread running a cooperative scheduler (`GuestSyscalls::RunThreads` in `GuestRuntime.cpp`) over FEXCore guest threads; processes share one host address range and swap private pages on switch. Milestone 2 adds (1) timed preemption through FEXCore's interrupt fault page, (2) a Linux signal model with process/thread pending queues, default actions and stop/continue, (3) sessions and terminal job control, and (4) a readiness core shared by poll/select/epoll, timer, event, signal and inotify descriptors. Pure logic lives in small headers (`Guest*.h`) with host unit tests; syscall glue stays in `GuestRuntime.cpp`.

**Tech Stack:** C++20, libnx/ounce-devkit (Horizon), FEXCore JIT, x86-64 Debian 12 guest userspace, C test programs linked against Debian glibc, GNU make.

## Global Constraints

- Switch 2 code lives in `Source/Switch2`; Linux and Windows builds keep working. FEXCore changes are guarded by `__SWITCH2__` or a runtime config flag that defaults off.
- Builds: `make -C Source/Switch2` (JIT probe); one launcher tree `Source/Switch2/build/guest`; bundle staged to `Source/Switch2/build/guest/sd`. One NRO per console run; per-launch test behavior goes in sidecar files (`<program>.<mode>`), never build options.
- Real semantics: Linux behavior through the normal entry point. No faked results. Anything incomplete is listed in `ProcessInventory.md` under "Deliberate differences" with the exact errno returned.
- Error order and errno follow host Linux 7.3 (the native reference on this machine).
- Evidence: report host build, native Linux and Switch 2 console results separately. Claim only what was observed.
- Commit on `nx2` once, after milestone console acceptance passes. Graphics stays on `shelf/graphics`.
- Keep `Source/Switch2/NextMilestone.md#restart-checkpoint` current after each verified slice (status, done-when, next step, blockers, exact commands with results, console artifact paths).
- Launcher version for the milestone console run starts at `0.13.0` (bump in `GuestProbe.cpp` `WriteLog("--- FEX Switch 2 launcher X started ---")` only when preparing a console build).
- Do not run r/w persistence/recovery console pairs unless a change touches the inode store or journal.

## Ownership

User direction: Opus (main session) implements complex functions; a Sonnet subagent does the outline and simpler features.

| Owner | Tasks |
| --- | --- |
| Sonnet subagent | 1, 2, 8, 9, 11, 12, 15, 16 (gap sweep for simple groups, Debian tooling) |
| Opus (main) | 3, 4, 5, 6, 7, 10, 13, 14, 16 (review of signal/session/epoll/inotify cases), 17, 18 |

Each Sonnet task is dispatched with this plan's Global Constraints, the task text, and the exact interfaces it consumes. Opus reviews every Sonnet diff before the next dependent task.

## Phases

1. **Outline** (Tasks 1–2): inventory, feature outline, suite harness.
2. **Implementation** (Tasks 3–14): each task adds a pure-logic header with a host unit test, syscall glue, and a native-verified suite case.
3. **Full test suite and acceptance** (Tasks 15–18): complete `ProcSuite.c`, Debian tool acceptance script, packaged native run, NRO, console checklist.

## File Structure

| File | Responsibility | Task |
| --- | --- | --- |
| `Source/Switch2/docs/ProcessInventory.md` | Milestone 2 syscall inventory, routing, deliberate differences, console evidence | 1 |
| `Source/Switch2/tests/guest/ProcSuite.c` | Milestone 2 suite (TAP, exit 42), groups `proc/ session/ signal/ cred/ clock/ timer/ poll/ epoll/ event/ inotify/` | 2, 15 |
| `Source/Switch2/tests/guest/ProcessTools.sh` | Debian tool acceptance: pipelines, traps, `sleep`, `timeout`, `kill`, `inotifywait` | 16 |
| `Source/Switch2/runtime/process/GuestPreempt.h` | Quantum bookkeeping, interrupt-page arming decisions | 3 |
| `Source/Switch2/runtime/process/GuestSignals.h` | Signal numbers, default actions, siginfo layout, pending queues | 4 |
| `Source/Switch2/runtime/process/GuestJobControl.h` | Wait-status encoding, wait selectors, pgrp/session rules | 5, 6 |
| `Source/Switch2/runtime/process/GuestCredentials.h` | set*id rules, capability sets and transitions | 8 |
| `Source/Switch2/runtime/process/GuestClocks.h` | Clock IDs, CPU clock encoding, realtime base, resolution | 9 |
| `Source/Switch2/runtime/process/GuestTimers.h` | Timer queue, interval/overrun math, itimer/alarm rounding | 10 |
| `Source/Switch2/runtime/ipc/GuestEventFd.h` | eventfd counter semantics | 11 |
| `Source/Switch2/runtime/ipc/GuestEpoll.h` | Interest list, ready list, ET/ONESHOT/EXCLUSIVE | 13 |
| `Source/Switch2/runtime/fs/GuestInotify.h` | Watch table, event encoding, queue limits, cookies | 14 |
| `Source/Switch2/tests/unit/Guest<Name>Test.cpp` | Host unit test per header | per task |
| `Source/Switch2/runtime/GuestRuntime.cpp` | Syscall glue, scheduler, terminal signal generation | 3–14 |
| `Source/Switch2/runtime/process/GuestTerminal.h` | ISIG character detection (`VINTR`, `VQUIT`, `VSUSP`), `TOSTOP` | 6 |
| `FEXCore/Source/Interface/Core/Core.cpp`, `FEXCore/include/FEXCore/Core/Context.h` | Expose `SetNeedsPendingInterruptFaultCheck` | 3 |
| `Source/Switch2/launcher/GuestProbe.cpp` | Exception handler routes interrupt-page faults to the runtime | 3 |
| `Source/Switch2/Makefile` | `check-guest-*` targets, `proc-suite`, `check-proc-suite-native`, staging | every task |
| `Source/Switch2/tools/StageDebian.py`, `FetchDebian.py` | Stage `proc-suite.elf`, `ProcessTools.sh`, `inotify-tools`, `procps` | 2, 16 |

## Host unit test pattern (used by every header task)

Existing pattern: `tests/unit/GuestFileWriteTest.cpp` + Makefile rule. Each new header gets:

```make
.PHONY: check-guest-signals
check-guest-signals: $(BUILD)/guest-signals-test
	$(BUILD)/guest-signals-test

$(BUILD)/guest-signals-test: tests/unit/GuestSignalsTest.cpp GuestSignals.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -O1 -g -Wall -Wextra -Werror -I. $< -o $@
```

(Check the existing `check-guest-file-write` rule for the exact `HOST_CXX`/flags variable names and copy them; do not invent new ones.) Tests use plain `assert`-style checks that print `FAIL <file>:<line> <expr>` and exit 1, matching `GuestFileWriteTest.cpp`.

## Native verification loop (used by every suite case)

Suite expectations are validated against the host kernel before guest code exists:

```bash
make -C Source/Switch2 check-proc-suite-native        # uid 1000 and namespace root, exit 42
Source/Switch2/build/proc-suite-host -l               # list names
Source/Switch2/build/proc-suite-host signal/           # run one group
```

The guest runtime only runs on the console, so each implementation task ends with an NRO compile (`cmake --build Source/Switch2/build/guest --target guest_nro`) and the case joins the console batch in Task 18.

---

## Phase 1 — Outline

### Task 1: Inventory and feature outline [Sonnet]

**Files:**
- Create: `Source/Switch2/docs/ProcessInventory.md`
- Modify: `Source/Switch2/NextMilestone.md` (add "Milestone 2 feature outline and acceptance" after the Milestone 1 one)

**Interfaces:**
- Produces: the authoritative entry list consumed by every later task and by Task 17 reconciliation.

- [ ] **Step 1: Build the routed set.** Extract every `case N:` in `GuestSyscalls::HandleSyscall` (`GuestRuntime.cpp`, the `switch (Registers[REG_RAX])` block):

```bash
cd Source/Switch2 && sed -n '/switch (Registers[REG_RAX]) {/,/if (Result == RetrySyscall/p' GuestRuntime.cpp | grep -o 'case [0-9]\+' | awk '{print $2}' | sort -n | uniq > /tmp/claude-routed.txt
```

(Use the scratchpad directory, not `/tmp`, when running as an agent.)

- [ ] **Step 2: Write `ProcessInventory.md`** with the header style of `FilesystemInventory.md`: intro, "Console verification still required", "Deliberate differences from a stock kernel", a group summary table, then one table per group (`| Number | Syscall | Dispatch | Notes |`). Dispatch values: `Routed`, `Partial` (routed but known gaps; Notes names them), `Unrouted`. Entries:

  - **Processes and sessions:** 24 sched_yield, 39 getpid, 56 clone, 57 fork, 58 vfork, 59 execve, 60 exit, 61 wait4, 63 uname, 97 getrlimit, 98 getrusage, 99 sysinfo, 100 times, 109 setpgid, 110 getppid, 111 getpgrp, 112 setsid, 121 getpgid, 124 getsid, 135 personality, 140 getpriority, 141 setpriority, 157 prctl, 158 arch_prctl, 160 setrlimit, 186 gettid, 218 set_tid_address, 231 exit_group, 247 waitid, 302 prlimit64, 309 getcpu, 322 execveat, 434 pidfd_open, 435 clone3, 438 pidfd_getfd
  - **Signals:** 13 rt_sigaction, 14 rt_sigprocmask, 15 rt_sigreturn, 34 pause, 62 kill, 127 rt_sigpending, 128 rt_sigtimedwait, 129 rt_sigqueueinfo, 130 rt_sigsuspend, 131 sigaltstack, 200 tkill, 219 restart_syscall, 234 tgkill, 282 signalfd, 289 signalfd4, 297 rt_tgsigqueueinfo, 424 pidfd_send_signal
  - **Credentials and capabilities:** 102 getuid, 104 getgid, 105 setuid, 106 setgid, 107 geteuid, 108 getegid, 113 setreuid, 114 setregid, 115 getgroups, 116 setgroups, 117 setresuid, 118 getresuid, 119 setresgid, 120 getresgid, 122 setfsuid, 123 setfsgid, 125 capget, 126 capset
  - **Clocks and timers:** 35 nanosleep, 36 getitimer, 37 alarm, 38 setitimer, 96 gettimeofday, 159 adjtimex, 164 settimeofday, 201 time, 222 timer_create, 223 timer_settime, 224 timer_gettime, 225 timer_getoverrun, 226 timer_delete, 227 clock_settime, 228 clock_gettime, 229 clock_getres, 230 clock_nanosleep, 283 timerfd_create, 286 timerfd_settime, 287 timerfd_gettime, 305 clock_adjtime
  - **Polling and watches:** 7 poll, 23 select, 213 epoll_create, 232 epoll_wait, 233 epoll_ctl, 253 inotify_init, 254 inotify_add_watch, 255 inotify_rm_watch, 270 pselect6, 271 ppoll, 281 epoll_pwait, 284 eventfd, 290 eventfd2, 291 epoll_create1, 294 inotify_init1, 300 fanotify_init, 301 fanotify_mark, 441 epoll_pwait2

  This list has 109 entries, more than the historical 85-entry audit. State that in the intro: "The historical audit counted 85 entries; this inventory adds the remaining x86-64 entries of these groups so none is left untracked." Mark each `Routed`/`Partial`/`Unrouted` from Step 1 plus these known partials (read the named handler to confirm before writing the note):
  - 62 `kill`: `SignalProcess` returns EOPNOTSUPP for pid <= 0 and for default dispositions.
  - 61 `wait4`: no WUNTRACED/WCONTINUED/__WALL; rusage only utime.
  - 56 `clone`: fork-style clone only with exit signal 17.
  - 97/302 `getrlimit`/`prlimit64`: setting returns EPERM.
  - 228 `clock_gettime`: realtime one-second resolution; CPU clocks return monotonic.
  - 230 `clock_nanosleep`: process CPU clock and root alarm sleeps return EOPNOTSUPP.
  - 7/23/270/271 poll family: review readiness per descriptor type.
  - 157 `prctl`: list implemented options from `GuestPrivilegeControl`.
  - 24 `sched_yield`: returns 0 without yielding.

- [ ] **Step 3: Add the feature outline** to `NextMilestone.md`, mirroring "Milestone 1 feature outline and acceptance":

| Group | Required behavior | Combined suite coverage |
| --- | --- | --- |
| Processes and sessions | fork/vfork/clone/clone3 lifecycles, exec variants, exit and exit_group, wait4/waitid selectors and states, reparenting, rusage and times, process groups and sessions, controlling terminal, pidfds, rlimits, priority, prctl subset | `proc/`, `session/` cases, Bash pipelines and job control |
| Signals | Process and thread pending sets, default actions (terminate, core, ignore, stop, continue), queued realtime signals with siginfo, masks, suspend/wait/timedwait, restart semantics, alternate stacks, signalfd, pidfd signals | `signal/` cases, Bash traps, `kill`, `timeout` |
| Credentials and capabilities | set*id transition rules, supplementary groups, filesystem IDs, capability sets and their transitions across set*id and exec | `cred/` cases run as uid 1000 and root |
| Clocks and timers | All clock IDs incl. CPU and per-pid CPU clocks, resolution, settable realtime for root, sleeps, alarm, itimers, POSIX timers with overrun, timerfd | `clock/`, `timer/` cases, `sleep`, `timeout` |
| Polling and watches | poll/select/pselect/ppoll readiness with signal masks, epoll (LT/ET/ONESHOT, nesting, description lifetime), eventfd, inotify events and watch lifetime | `poll/`, `epoll/`, `event/`, `inotify/` cases, `inotifywait` |

  Add the acceptance paragraph: one combined acceptance program (`/bin/process-acceptance.elf` wrapper running `proc-suite.elf` and `ProcessTools.sh`), console run plus an interactive job-control checklist.

- [ ] **Step 4: Verify** links and counts: `grep -c '^| [0-9]' Source/Switch2/docs/ProcessInventory.md` prints `109`.

- [ ] **Step 5: Update restart checkpoint** "Next" bullet to point at `Milestone2Plan.md` Task 2. No commit (commit at milestone end).

### Task 2: Suite harness `ProcSuite.c` [Sonnet]

**Files:**
- Create: `Source/Switch2/tests/guest/ProcSuite.c`
- Modify: `Source/Switch2/Makefile`, `Source/Switch2/tools/StageDebian.py`

**Interfaces:**
- Produces: `TEST(group, name)`, `CHECK(expr)`, `CHECK_ERRNO(call, errno)`, `SKIP(reason)`, `Fork(void (*child)(void))`, `WaitExit(pid_t)` returning the raw wait status, `Deadline(ms)` helper; binary names `proc-suite-host` (host) and `proc-suite-debian.elf` (staged as `/bin/proc-suite.elf`), root variant `/bin/proc-suite-root.elf` launched with a `proc-suite-root.elf.user` sidecar (`0 0`) exactly like `fs-suite-root.elf`.

- [ ] **Step 1: Copy the harness** (lines from the top of `FsSuite.c` through the end of the "Harness" section, including `TEST`, TAP output, pattern filters `-l`, `!pattern`, exit 42) into `ProcSuite.c`. Change the header comment to "Process and event compatibility suite". Drop filesystem-only helpers. Every test runs in a forked child of the harness so a test that kills, stops or changes credentials cannot affect the next test: the harness forks, the child calls the test and exits 0/1, the parent waits with a 10 s alarm and reports `not ok ... timeout` on expiry (kill the child's process group with SIGKILL first; the child calls `setpgid(0, 0)`).

- [ ] **Step 2: Add the first case** to prove the harness:

```c
TEST(proc, getpid_matches_gettid_in_main_thread) {
  CHECK(getpid() == syscall(SYS_gettid));
  CHECK(getppid() > 0);
}
```

- [ ] **Step 3: Makefile** — copy the `fs-suite`, `$(BUILD)/fs-suite.o`, `$(BUILD)/fs-suite-host`, `check-fs-suite-native` rules to `proc-suite` equivalents; add `$(BUILD)/proc-suite-debian.elf` to the `%-debian.elf` pattern list and to `filesystem-runtime`'s prerequisites and the `StageDebian.py` call as `--proc-suite`.

- [ ] **Step 4: StageDebian.py** — add `proc_suite=None` parameter staging `/bin/proc-suite.elf`, `/bin/proc-suite-root.elf` and `/bin/proc-suite-root.elf.user` containing `0 0` (follow the `fs_suite` code path exactly).

- [ ] **Step 5: Run**

```bash
make -C Source/Switch2 check-proc-suite-native
```
Expected: `1 passed, 0 failed, 0 skipped` twice (uid 1000 and namespace root), exit 0.

```bash
make -C Source/Switch2 filesystem-runtime && ls Source/Switch2/build/guest/sd/fex/rootfs/bin/proc-suite*
```
Expected: `proc-suite.elf proc-suite-root.elf proc-suite-root.elf.user`.

---

## Phase 2 — Implementation

### Task 3: Timed preemption (EXPERIMENT gate) [Opus]

Without preemption a CPU-bound guest never returns to `RunThreads`, so no signal, timer, terminal key or other process runs. Every later task depends on this.

**Files:**
- Create: `Source/Switch2/runtime/process/GuestPreempt.h`, `Source/Switch2/tests/unit/GuestPreemptTest.cpp`
- Modify: `FEXCore/include/FEXCore/Core/Context.h`, `FEXCore/Source/Interface/Core/Core.cpp`, `FEXCore/Source/Interface/Context/Context.h`, `Source/Switch2/runtime/GuestRuntime.cpp` (`InitializeProcessContext`, `RunThreads`, `Dispatch`), `Source/Switch2/launcher/GuestProbe.cpp` (`__libnx_exception_handler`)

**Interfaces:**
- Produces: `void GuestSyscalls::RequestPreempt()` (host timer thread), `bool HandleInterruptFault(ThreadExceptionDump*)` (exception handler hook, returns true when it consumed the fault), `GuestThread::RanSince` (monotonic ns at dispatch), constant `PreemptQuantumNs = 10'000'000`.
- Consumes: FEXCore `InternalThreadState::InterruptFaultPage` (page-aligned, stored to at every block entry when `Config.NeedsPendingInterruptFaultCheck`).

Design: enable `NeedsPendingInterruptFaultCheck` for Switch 2 contexts. A host watchdog thread (`threadCreate`, priority above the guest thread) wakes every quantum; if the running guest thread has run longer than a quantum **and** (another thread is ready, a timer is due, a signal is pending for it, or terminal input arrived) it makes the running thread's `InterruptFaultPage` read-only (`svcSetMemoryPermission(page, 0x1000, Perm_R)`). The next block-entry store faults. The exception handler recognises a fault address inside the current thread's interrupt page, restores `Perm_Rw`, and resumes the guest at a point where the guest state is consistent (block entry) through FEXCore's existing suspend path, which then returns to `RunThreads`.

- [ ] **Step 1: Spike A — fault delivery.** In a throwaway branch of the JIT probe (`JitProbe.cpp`), allocate a page from `aligned_alloc(0x1000, 0x1000)`, start a thread that after 50 ms calls `svcSetMemoryPermission` to `Perm_R`, loop storing to the page in the main thread, and in `__libnx_exception_handler` restore `Perm_Rw`, set a flag and return (libnx restores registers from `ThreadExceptionDump` on return). Log `preempt spike: fault delivered after N stores, resumed`. Label the NRO `0.13.0-exp1`. Console result needed (batch into the first console run). Pass: the log line appears and the probe exits normally.

- [ ] **Step 2: Spike B — resume path.** Read how FEX handles the interrupt page on Linux (`grep -rn InterruptFaultPage Source/Tools Source/Linux* FEXCore/Source`), specifically the code that spills static-register-allocated guest registers into `CpuStateFrame` and jumps to the dispatcher's thread-pause/return handler. Decide between:
  - (a) Translate `ThreadExceptionDump` into FEX's `ArchHelpers::Context` and call the same SignalDelegator helper, then set `Dump->pc` to the dispatcher's "return to host" address so `ExecuteThread` returns, landing at `Dispatch`'s `setjmp`. Mark `Current->Preempted = true` so `RunThreads` does not treat the return as `Dispatcher returned before the guest reached its syscall`.
  - (b) If (a) needs too much Linux-only machinery, set `Dump->pc` to a host trampoline that runs on the guest thread's host stack, copies SRA registers from the saved dump into `Frame->State` (mapping in `FEXCore/Source/Interface/Core/ArchHelpers/` SRA tables), sets `State.rip` from the block's entry RIP (FEX's block lookup by host PC) and `longjmp(ReturnToHost, 1)`.
  Record the decision and reasons in `ProcessInventory.md` under a "Preemption design" heading.

- [ ] **Step 3: Write `GuestPreempt.h` with its unit test first.** Pure decision logic:

```cpp
// GuestPreempt.h
#pragma once
#include <cstdint>
namespace Switch2Guest {
constexpr uint64_t PreemptQuantumNs = 10'000'000;
struct PreemptInputs {
  uint64_t Now, RunningSince;
  bool OtherReady, SignalPending, TimerDue, TerminalInput;
};
constexpr bool ShouldPreempt(const PreemptInputs& In) {
  if (In.Now - In.RunningSince < PreemptQuantumNs) return In.SignalPending; // signals preempt immediately
  return In.OtherReady || In.SignalPending || In.TimerDue || In.TerminalInput;
}
} // namespace Switch2Guest
```

```cpp
// tests/unit/GuestPreemptTest.cpp
#include "GuestPreempt.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)
using namespace Switch2Guest;
int main() {
  CHECK(!ShouldPreempt({PreemptQuantumNs - 1, 0, true, false, false, false}));
  CHECK(ShouldPreempt({PreemptQuantumNs, 0, true, false, false, false}));
  CHECK(ShouldPreempt({1, 0, false, true, false, false}));
  CHECK(!ShouldPreempt({PreemptQuantumNs * 5, 0, false, false, false, false}));
  CHECK(ShouldPreempt({PreemptQuantumNs, 0, false, false, true, false}));
  std::puts("guest preempt: ok");
}
```

Run: `make -C Source/Switch2 check-guest-preempt` → first FAIL (target missing), then add Makefile rule (pattern above) → `guest preempt: ok`.

- [ ] **Step 4: FEXCore switch.** Add to `FEXCore::Context::Context` a virtual `void SetNeedsPendingInterruptFaultCheck(bool)` implemented in `ContextImpl` as `Config.NeedsPendingInterruptFaultCheck = Value;` (must be called before `InitCore`). Call it from `InitializeProcessContext` before `InitCore()`. Linux/Windows builds never call it, so their behavior is unchanged. Verify `cmake --build Build` (host Linux FEX build, if configured) and the Switch 2 build both compile.

- [ ] **Step 5: Runtime glue.** Watchdog thread in `GuestSyscalls` constructor (stop in destructor). `std::atomic<uint64_t> RunningSince`, `std::atomic<FEXCore::Core::InternalThreadState*> RunningCore`; `RunThreads` sets them around `Dispatch`. Watchdog computes `PreemptInputs` from atomics the scheduler publishes (`OtherReadyHint`, `SignalPendingHint`, `NextTimerDeadline`, `TerminalHint` updated by the keyboard poll) — no access to non-atomic scheduler state from the watchdog. `RunThreads` also calls `PollTerminalInput()` once per scheduling pass when `Interactive`, not only when idle.

- [ ] **Step 6: `sched_yield` (24)** now returns `RetrySyscall`-style yield: set `rip += 2`, `RAX = 0`, `longjmp(ReturnToHost)` so other ready threads run (round-robin from the next index in `RunThreads`: keep a `NextIndex` cursor instead of always starting at 0 — fairness requirement).

- [ ] **Step 7: Suite cases** (native first):

```c
static volatile sig_atomic_t Spun;
TEST(proc, busy_child_is_killable) {
  pid_t Child = fork();
  if (!Child) { for (;;) Spun++; }
  usleep(50000);
  CHECK(kill(Child, SIGKILL) == 0);
  int Status; CHECK(waitpid(Child, &Status, 0) == Child);
  CHECK(WIFSIGNALED(Status) && WTERMSIG(Status) == SIGKILL);
}
TEST(proc, busy_threads_share_cpu) {
  // two threads increment separate counters for 200 ms; both must advance
}
TEST(signal, alarm_interrupts_busy_loop) {
  signal(SIGALRM, OnAlarm); alarm(1); while (!Alarmed) {} CHECK(Alarmed);
}
```

(`signal/alarm_interrupts_busy_loop` depends on Task 10; register it now, it is expected to fail on the console until Task 10.)

Run `make -C Source/Switch2 check-proc-suite-native` → all pass. Build NRO → compiles.

- [ ] **Step 8: Experiment gate.** Console run of `0.13.0-exp2` with `proc-suite.elf proc/busy_child_is_killable proc/busy_threads_share_cpu`. If it fails or regresses `fs-suite.elf`, revert Task 3 runtime changes per AGENTS.md and re-plan before any further task. Record the result in the restart checkpoint.

### Task 4: Signal model [Opus]

**Files:**
- Create: `Source/Switch2/runtime/process/GuestSignals.h`, `Source/Switch2/tests/unit/GuestSignalsTest.cpp`
- Modify: `GuestRuntime.cpp` (`GuestThread`, `GuestProcess`, `QueueGuestSignal`, `PendingCaughtSignal`, `SignalProcess`, `EnterSignalHandler`, `SignalAction`, `SignalMask`, case 127, `RunThreads` delivery, `TerminateForSignal`, `DeliverDefaultPipeSignal`)

**Interfaces:**
- Produces (header):

```cpp
namespace Switch2Guest {
enum class SignalDefault : uint8_t { Terminate, Core, Ignore, Stop, Continue };
constexpr uint32_t SigHup = 1, SigInt = 2, SigQuit = 3, SigIll = 4, SigTrap = 5, SigAbrt = 6, SigBus = 7, SigFpe = 8,
  SigKill = 9, SigUsr1 = 10, SigSegv = 11, SigUsr2 = 12, SigPipe = 13, SigAlrm = 14, SigTerm = 15, SigStkflt = 16,
  SigChld = 17, SigCont = 18, SigStop = 19, SigTstp = 20, SigTtin = 21, SigTtou = 22, SigUrg = 23, SigXcpu = 24,
  SigXfsz = 25, SigVtalrm = 26, SigProf = 27, SigWinch = 28, SigIo = 29, SigPwr = 30, SigSys = 31, SigRtMin = 32, SigMax = 64;
constexpr uint64_t SignalBit(uint32_t S) { return uint64_t {1} << (S - 1); }
constexpr uint64_t Unblockable = SignalBit(SigKill) | SignalBit(SigStop);
constexpr uint64_t StopSet = SignalBit(SigStop) | SignalBit(SigTstp) | SignalBit(SigTtin) | SignalBit(SigTtou);
SignalDefault DefaultAction(uint32_t Signal);

// Linux x86-64 siginfo_t, 128 bytes.
struct SigInfo {
  int32_t Signo, Errno, Code, Pad0;
  union {
    struct { uint32_t Pid, Uid; uint64_t Value; } Kill;          // SI_USER, SI_QUEUE, SI_TKILL
    struct { int32_t Tid, Overrun; uint64_t Value; } Timer;      // SI_TIMER
    struct { uint32_t Pid, Uid; int32_t Status, Pad; int64_t Utime, Stime; } Child; // CLD_*
    struct { uint64_t Address; } Fault;
    uint8_t Raw[112];
  };
};
static_assert(sizeof(SigInfo) == 128);
constexpr int32_t SiUser = 0, SiKernel = 0x80, SiQueue = -1, SiTimer = -2, SiTkill = -6;
constexpr int32_t CldExited = 1, CldKilled = 2, CldDumped = 3, CldTrapped = 4, CldStopped = 5, CldContinued = 6;

// Pending set: standard signals collapse to one entry, realtime signals queue (limit per RLIMIT_SIGPENDING).
class SignalQueue {
public:
  uint64_t Set() const;
  bool Enqueue(const SigInfo& Info, size_t Limit); // false = EAGAIN for rt queue full (standard: always true)
  std::optional<SigInfo> Dequeue(uint64_t Allowed); // lowest number first, standard before realtime
  void Discard(uint64_t Mask);                      // drop all instances (SIGCONT drops stops, stops drop SIGCONT, SIG_IGN drops)
  size_t QueuedCount() const;
private:
  std::vector<SigInfo> Entries;
};
}
```

- Produces (runtime): `GuestThread::Pending` (`SignalQueue`), `GuestProcess::SharedPending` (`SignalQueue`), `GuestProcess::State` (`enum class RunState { Running, Stopped }`), `GuestProcess::StopSignal`, `GuestProcess::ReportStop`, `GuestProcess::ReportContinue`; functions:
  - `int64_t SendSignal(GuestProcess& Target, GuestThread* Thread, SigInfo Info)` — thread-directed when `Thread`, else process-directed. Applies Linux `prepare_signal` rules: SIGCONT/stop interaction, ignored signals dropped unless blocked, SIGKILL wakes everything; picks a receiving thread (`complete_signal` order: current thread if not blocking, else first thread not blocking).
  - `bool CanSignal(const GuestProcess& Target, uint32_t Signal)` — `kill_ok_by_cred`: sender euid 0, or sender ruid/euid equals target ruid/suid; SIGCONT also allowed within the same session.
  - `uint32_t NextDeliverable(GuestThread&)` replacing `PendingCaughtSignal`, applying default actions inline (terminate whole process with all threads, core = terminate with 0x80 bit, stop = enter `Stopped` and notify parent, ignore = drop).
- Consumes: Task 3 preemption for asynchronous delivery.

- [ ] **Step 1: Unit tests first** (`GuestSignalsTest.cpp`): default action for all 64 signals (table above in Linux `sig_kernel_*` macros: Core = QUIT ILL TRAP ABRT BUS FPE SEGV XCPU XFSZ SYS; Ignore = CHLD URG WINCH; Stop = STOP TSTP TTIN TTOU; Continue = CONT; everything else Terminate incl. 32–64); queue collapses two SIGUSR1 into one; queues two SIGRTMIN+1 with distinct `Value`; `Dequeue` returns SIGINT before SIGRTMIN, and SIGRTMIN before SIGRTMIN+1 in FIFO per signal; `Discard(StopSet)` drops pending SIGTSTP; rt limit returns false at `Limit`. Run `make -C Source/Switch2 check-guest-signals` → fails to compile, implement header, → `guest signals: ok`.

- [ ] **Step 2: Replace pending bitmasks** (`PendingSignals`, `SignalSenders`) with `SignalQueue` on thread and process. `rt_sigpending` (127) returns `(Thread.Pending.Set() | Process.SharedPending.Set()) & Thread.SignalMask` and validates `sigsetsize == 8` (EINVAL) and the output buffer (EFAULT). Keep the `SuppressPipeSignal` path but route it through `SendSignal`, thread-directed, with `si_code = SI_USER`, `si_pid = 0`, `si_uid = 0` (Linux `send_sig(SIGPIPE, current, 0)` uses `SEND_SIG_NOINFO`). Add a suite case reading the handler's `si_code` natively to confirm before coding.

- [ ] **Step 3: Delivery.** `EnterSignalHandler` takes a `SigInfo` and copies all 128 bytes into `GuestSignalFrame::Info` (change `std::array<int32_t, 32> Info` to `SigInfo Info`), so `si_code`, `si_pid`, `si_uid`, `si_value`, `si_status` reach handlers. Honour `SA_SIGINFO` only for argument meaning (Linux always writes the frame). `SA_NODEFER`, `SA_RESETHAND`, `SA_ONSTACK`, `sa_mask` already handled — keep. `rt_sigaction`: reject SIGKILL/SIGSTOP with a handler or SIG_IGN (EINVAL) — current code rejects any `Input` for 9/19; Linux allows `SIG_DFL` for them; fix to reject only non-default. Setting SIG_IGN (or SIG_DFL for an Ignore-default signal) discards that signal from all pending queues of the process (`flush_sigqueue_mask`).

- [ ] **Step 4: Process-directed `kill`.** `SignalProcess(int32_t PID, uint32_t Signal)`:
  - `PID > 0`: target process; ESRCH if missing or a zombie (zombie = `Exited` but unreaped: Linux returns 0 for zombies with sig 0? It returns 0 because the task still exists; match: `kill(zombie, 0) == 0` and `kill(zombie, SIGTERM) == 0` with no effect).
  - `PID == 0`: every process in caller's group; `PID == -1`: every process the caller may signal except PID 1 (the virtual init from Task 5) and the caller itself; `PID < -1`: group `-PID`. Return EPERM only if every target failed `CanSignal`, ESRCH if none existed, else 0.
  - Remove the `EOPNOTSUPP` returns.

- [ ] **Step 5: `tkill` (200), `tgkill` (234)**: thread-directed `SiTkill`. tgkill: `tgid <= 0 || tid <= 0` EINVAL; thread not in tgid ESRCH. **`rt_sigqueueinfo` (129) / `rt_tgsigqueueinfo` (297)**: copy 128-byte siginfo; if sending to another process and `si_code >= 0` or `si_code == SI_TKILL`, EPERM; force `si_signo = sig`. **`pidfd_send_signal` (424)** after Task 5's pidfd.

- [ ] **Step 6: Waiting syscalls.**
  - `pause` (34): wait kind `SignalWait`; returns EINTR after a handler runs (`ERESTARTNOHAND` → EINTR because a handler ran).
  - `rt_sigsuspend` (130): save mask, install `*mask` minus unblockables, wait; on handler entry the frame's saved mask is the **old** mask (`saved_sigmask`), return EINTR.
  - `rt_sigtimedwait` (128): `set` minus unblockables; dequeue a matching pending signal immediately (thread first, then shared); else block with optional timeout (EAGAIN on expiry, EINVAL on bad timespec, EINTR if another unblocked caught signal arrives). Writes siginfo when `info` non-null.
  - Restart rules: add `WaitKind::SignalWait`; for interruptible waits implement Linux's `-ERESTARTSYS` semantics generically: a blocked syscall that gets a caught signal returns EINTR unless the handler has `SA_RESTART` **and** the syscall is restartable (read/write/wait4/futex-without-timeout/lock/accept/connect, not sleeps/poll/select/epoll/sigsuspend/pause/sigtimedwait). Encode per wait as `bool RestartOnSigaction`. Restart = rewind `rip` by 2 and restore `RAX` to the syscall number (keep `Frame->State.gregs[REG_RAX]` original in `GuestThread::SyscallNumber`).
  - `restart_syscall` (219): only reachable after a stop/continue of a `nanosleep`/`clock_nanosleep`/`futex` with a timeout (Linux `ERESTART_RESTARTBLOCK`); when a stopped thread with such a wait continues with no handler, resume the remaining wait. Return EINTR if there is no restart block.

- [ ] **Step 7: Default actions.** Terminate/core: mark every thread of the process exited (exit_group semantics, Task 5 helper `ExitProcess(Status, Signal)`), set `TerminatingSignal` (core adds `0x80` to the wait status only if `RLIMIT_CORE != 0`; Linux sets the core flag only when a dump was written — no dump is written here, so never set `0x80`; document in inventory). Stop: Task 6. Continue: Task 6.

- [ ] **Step 8: Suite cases** (`signal/` group; native first, each one assertion-focused):
  - `default_term_status` — child `raise(SIGTERM)` → `WIFSIGNALED && WTERMSIG == SIGTERM`.
  - `default_ignore_chld_winch_urg` — raise each, process continues.
  - `kill_errors` — `kill(pid, 65)` EINVAL; `kill(INT_MAX, 0)` ESRCH; uid 1000 → `kill(1, SIGTERM)`: EPERM natively; in the guest pid 1 does not exist → ESRCH. Use a forked child that `setresuid`s instead of pid 1 to keep it portable (only when root; skip otherwise).
  - `kill_process_group` — fork two children in a new group, `kill(-pgid, SIGUSR1)`, both handlers record.
  - `kill_zero_targets_group`, `kill_minus_one_skips_self`.
  - `siginfo_from_kill` — handler with `SA_SIGINFO` sees `si_code == SI_USER`, `si_pid == getppid-of-sender`, `si_uid == getuid()`.
  - `sigqueue_value_and_rt_order` — block SIGRTMIN..+1, `sigqueue` +1 (value 7), SIGRTMIN (5), +1 (8); unblock; handler order SIGRTMIN, +1(7), +1(8).
  - `standard_signal_collapses` — blocked SIGUSR1 sent twice delivers once.
  - `sigqueueinfo_eperm_positive_code` — `rt_sigqueueinfo(getppid(), SIGUSR1, &info{si_code=SI_USER})` → EPERM.
  - `tgkill_targets_thread` — thread with SIGUSR1 unblocked receives; `tgkill(getpid(), tid, SIGUSR1)`; handler records `gettid()`.
  - `tgkill_errors` — `tgkill(0, 1, 0)` EINVAL; `tgkill(getpid(), 999999, 0)` ESRCH.
  - `sigaction_kill_stop_rules` — `SIG_DFL` accepted for SIGKILL; handler → EINVAL; `sigsetsize 4` → EINVAL.
  - `ignore_discards_pending` — block USR1, raise, set SIG_IGN, set handler, unblock → handler not called.
  - `sigpending_reports_blocked`.
  - `sigsuspend_restores_mask` — handler reads mask (must include suspend mask), after return mask equals old mask, result EINTR.
  - `pause_returns_eintr`.
  - `sigtimedwait_dequeues` / `sigtimedwait_timeout_eagain` / `sigtimedwait_bad_timespec_einval`.
  - `restart_read_with_sa_restart` — pipe read in child thread blocked; SIGUSR1 handler with SA_RESTART; read continues and later returns data.
  - `no_restart_without_sa_restart` — same without SA_RESTART → EINTR.
  - `nanosleep_never_restarts` — already covered by `time/sleep_interrupted_by_signal` in FsSuite; re-home it here.
  - `sigaltstack_handler_runs_on_alt_stack`.
  - `sigpipe_default_kills_writer`, `sigpipe_ignored_epipe` (move from FsSuite if present; keep FsSuite copy if it is part of M1 acceptance).

Run `make -C Source/Switch2 check-guest-signals check-proc-suite-native` → pass; NRO builds.

### Task 5: Process lifecycle and wait [Opus]

**Files:**
- Create: `Source/Switch2/runtime/process/GuestJobControl.h` (wait part), `Source/Switch2/tests/unit/GuestJobControlTest.cpp`
- Modify: `GuestRuntime.cpp` (`RequestFork`, `ForkGuest`, `CloneThread`, `WaitChild`, `RetireProcess`, `ExecGuest`, `InstallExec`, exit path in `HandleSyscall`, `SystemInfo`, cases 56/57/58/61/98/100/135/140/141/157/247/309/322/434/435/438)

**Interfaces:**
- Produces (header):

```cpp
namespace Switch2Guest {
constexpr uint32_t WaitExitedStatus(uint32_t Code) { return (Code & 0xff) << 8; }
constexpr uint32_t WaitSignaledStatus(uint32_t Signal) { return Signal & 0x7f; }
constexpr uint32_t WaitStoppedStatus(uint32_t Signal) { return (Signal << 8) | 0x7f; }
constexpr uint32_t WaitContinuedStatus = 0xffff;
enum class WaitIdType : uint32_t { All = 0, Pid = 1, Pgid = 2, Pidfd = 3 };
constexpr uint32_t WNoHang = 1, WUntraced = 2, WStopped = 2, WExited = 4, WContinued = 8, WNoWait = 0x01000000,
  WNoThread = 0x20000000, WAll = 0x40000000, WClone = 0x80000000;
struct WaitSelector { WaitIdType Type; uint32_t Id; };
// wait4 pid argument to selector: >0 Pid, 0 own pgid (filled by caller), -1 All, < -1 Pgid.
WaitSelector SelectorFromWait4(int32_t Pid, uint32_t OwnGroup);
// Validation: wait4 accepts WNOHANG|WUNTRACED|WCONTINUED|__WNOTHREAD|__WALL|__WCLONE else EINVAL;
// waitid requires at least one of WEXITED|WSTOPPED|WCONTINUED and accepts WNOHANG|WNOWAIT|__W* too.
int64_t ValidateWait4Options(uint32_t Options);
int64_t ValidateWaitIdOptions(uint32_t Options);
}
```

- Produces (runtime): `void ExitProcess(GuestProcess&, uint32_t Status, uint32_t Signal)` — kills sibling threads, closes descriptors, reparents children to the reaper (nearest `ChildSubreaper` ancestor, else `ProcessID`'s parent placeholder: the launcher root process acts as init for its descendants; see note), sends `SIGCHLD` (or the clone exit signal) to the parent with `CLD_EXITED/KILLED` siginfo, auto-reaps when the parent's SIGCHLD action is `SIG_IGN` or has `SA_NOCLDWAIT`, wakes waiters (`++IOEpoch`). `GuestProcess::ExitSignal` (from clone flags), `GuestProcess::ChildSubreaper`, `GuestProcess::ParentDeathSignal`, `GuestProcess::Nice`, `GuestProcess::Name[16]`, `GuestProcess::ChildTicks` (accumulated reaped children CPU), `GuestProcess::StartNs`.
- Pidfd: an `OpenFile` with `PidfdTarget` (`uint32_t`) and readiness "readable when the process has exited" (consumed by Task 13).

Note on init: the launched program is PID 100 with parent 1, and no PID 1 exists in the guest. Orphans reparent to PID 100's... Linux reparents to the namespace init. Decision: orphans reparent to `ProcessID` (100) if it is alive and is a subreaper by default — **no**: that changes Linux semantics for the launched program. Instead orphans reparent to a virtual init (PID 1) owned by the runtime that auto-reaps (equivalent to systemd/init reaping). `getppid()` of an orphan returns 1, as on Linux. Record in `ProcessInventory.md`.

- [ ] **Step 1: Unit tests** for the header: statuses (`WaitExitedStatus(3) == 0x300`, `WaitStoppedStatus(SIGTSTP) == 0x147f`), selectors (`SelectorFromWait4(0, 77) == {Pgid, 77}`, `(-1) == {All}`, `(-5) == {Pgid,5}`), option validation (`ValidateWait4Options(WExited) == -22`, `ValidateWaitIdOptions(WNoHang) == -22`, `ValidateWaitIdOptions(WExited|WNoWait) == 0`). `make -C Source/Switch2 check-guest-job-control` fails → implement → passes.

- [ ] **Step 2: Unified wait.** Rewrite `WaitChild` as `WaitGeneric(WaitSelector, uint32_t Options, SigInfo* Info, uint64_t StatusAddr, uint64_t UsageAddr, bool IsWaitId)`:
  - Candidates: children whose `ParentPID == Task->PID` (and, Linux 4.7+, children of other threads of the same thread group — same here as threads share the process), filtered by selector; `__WCLONE` selects children whose exit signal is not SIGCHLD, default selects SIGCHLD children, `__WALL` both.
  - Reportable states in order checked per child: exited (if `WExited` / always for wait4), stopped with `ReportStop` pending (if `WUntraced`), continued with `ReportContinue` (if `WContinued`).
  - `WNOWAIT` leaves the state reportable. Without it: exited children are reaped (add their CPU ticks + their `ChildTicks` into the parent's `ChildTicks`), stop/continue flags are cleared.
  - wait4: status word; rusage full 144 bytes (`ru_utime` from child CPU ticks, `ru_stime` 0, `ru_maxrss` from `Memory` private size in KiB, others 0).
  - waitid: fills `siginfo` (`si_signo = SIGCHLD`, `si_code` CLD_*, `si_pid`, `si_uid` = child ruid, `si_status` = exit code or signal); with WNOHANG and nothing ready returns 0 and zeroes `si_pid`/`si_signo` (Linux clears the struct). `P_PIDFD` resolves the descriptor (EBADF if not a pidfd).
  - No matching child: ECHILD. Blocking: `WaitForIO()` with restart-on-SA_RESTART (Task 4).

- [ ] **Step 3: Exit.** `exit` (60) ends the thread; when the last thread exits the process exits. `exit_group` (231) calls `ExitProcess`. Existing logic in `HandleSyscall` top moves into `ExitProcess`. The launched root process exiting still ends the run (`ProcessExited`) — keep, but first send SIGHUP+SIGCONT to orphaned stopped groups (Task 6 hook) and let remaining processes be torn down by `RunThreads`' final loop.

- [ ] **Step 4: Fork family.**
  - `clone` (56) without `CLONE_VM`: accept exit signal any 0..64 (stored in `ExitSignal`), flags `CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID`, `CLONE_CHILD_CLEARTID`, `CLONE_SETTLS`, `CLONE_PIDFD` (0x1000, returns pidfd in `parent_tid` slot), `CLONE_PARENT` (parent = caller's parent), `CLONE_VFORK` alone or with `CLONE_VM` (vfork: run child on a copy as fork does today, block the parent with `WaitKind::VforkDone` until the child execs or exits). `CLONE_NEWNS`/`NEWPID`/… → EPERM for non-root, EINVAL `CLONE_SIGHAND` without `CLONE_VM`, EINVAL `CLONE_THREAD` without `CLONE_SIGHAND`. Unsupported combinations return EINVAL, never ENOSYS (record in inventory).
  - `vfork` (58) = clone(CLONE_VM|CLONE_VFORK|SIGCHLD).
  - `clone3` (435): read `struct clone_args` (size ≥ 64 = `CLONE_ARGS_SIZE_VER0`, larger sizes require trailing zeros else E2BIG; < 64 EINVAL), map to the same path; `exit_signal` must be ≤ 64 (EINVAL); `set_tid` non-zero → EPERM for non-root (no PID namespaces), `cgroup` flag EINVAL.
  - Thread clone (`CloneThread`) also honours `CLONE_PARENT_SETTID` already; add `CLONE_SYSVSEM` acceptance (0x40000 already accepted) and `CLONE_PIDFD` EINVAL with `CLONE_THREAD`.

- [ ] **Step 5: Exec.** `execveat` (322): dirfd-relative path, `AT_EMPTY_PATH` (exec the descriptor's inode), `AT_SYMLINK_NOFOLLOW` (ELOOP on symlink). On exec: reset caught handlers to SIG_DFL (keep SIG_IGN), clear alt stack, clear pending? (no: pending signals survive exec), keep mask, close `O_CLOEXEC` descriptors (check existing), reset `ParentDeathSignal` when set-id exec, update `Name` to the basename (first 15 chars). Release a vfork parent.

- [ ] **Step 6: Misc process calls.**
  - `getpriority`/`setpriority` (140/141): `PRIO_PROCESS/PGRP/USER`; nice -20..19 stored, return value of getpriority is `20 - nice` (raw syscall); lowering nice below current needs root (EACCES); other user's process EPERM; no match ESRCH; bad `which` EINVAL. Priority has no scheduling effect on the cooperative scheduler — inventory note.
  - `getrusage` (98): `RUSAGE_SELF` (0), `RUSAGE_CHILDREN` (-1), `RUSAGE_THREAD` (1), else EINVAL.
  - `times` (100): clock ticks at 100 Hz (`sysconf(_SC_CLK_TCK)` = 100 on x86-64); returns ticks since boot (monotonic ns / 10 ms); buffer may be NULL.
  - `personality` (135): store; `0xffffffff` queries; return previous value; accept `PER_LINUX` (0) and flag bits; `ADDR_NO_RANDOMIZE` has no effect (no ASLR) — note.
  - `prctl` (157) via `GuestPrivilegeControl`: add `PR_SET_PDEATHSIG` 1/`PR_GET_PDEATHSIG` 2, `PR_GET_DUMPABLE` 3/`PR_SET_DUMPABLE` 4, `PR_SET_NAME` 15/`PR_GET_NAME` 16, `PR_SET_CHILD_SUBREAPER` 36/`PR_GET_CHILD_SUBREAPER` 37, `PR_GET_NO_NEW_PRIVS` 39 (existing set 38), `PR_CAPBSET_READ` 23 (Task 8), `PR_SET_KEEPCAPS` 8/`PR_GET_KEEPCAPS` 7 (Task 8). Unknown options EINVAL.
  - `getcpu` (309): cpu 0 node 0; NULL pointers allowed.
  - `sysinfo` (99): `Processes` = live process count.
  - `pidfd_open` (434): flags 0 or `PIDFD_NONBLOCK` (O_NONBLOCK) else EINVAL; ESRCH if missing; EINVAL for a non-leader TID; returns a close-on-exec descriptor. `pidfd_getfd` (438): flags 0; needs `CanSignal`-like ptrace check — implement as same-uid or root (Linux uses PTRACE_MODE_ATTACH_REALCREDS; document); duplicates the target's descriptor with `FD_CLOEXEC`. `pidfd_send_signal` (424): flags 0; `info` NULL → `SI_USER`.
  - `uname` (63): release string `5.15.0` → report the host-reference release used for errno parity? Keep `5.15.0`; glibc only needs ≥ 3.2. No change.

- [ ] **Step 7: Suite cases** (`proc/` group):
  `fork_wait_exit_code`, `wait_echild_without_children`, `waitpid_specific_and_any`, `wait_pgid_selector`, `waitid_wexited_siginfo`, `waitid_wnowait_keeps_zombie`, `waitid_wnohang_zeroes_info`, `waitid_requires_state_flag_einval`, `wait4_rusage_utime_advances` (child spins 100 ms), `sigchld_siginfo_on_exit`, `sigchld_ignored_autoreaps` (wait → ECHILD), `sa_nocldwait_autoreaps`, `orphan_reparented_ppid_1` (grandchild sees `getppid() == 1` after parent exits; with `PR_SET_CHILD_SUBREAPER` sees the subreaper), `exit_group_kills_threads`, `vfork_parent_waits_for_exec`, `clone3_basic_and_size_errors`, `clone_pidfd_poll_exit`, `pidfd_open_errors`, `pidfd_send_signal_kills`, `pidfd_getfd_dup`, `execveat_empty_path`, `exec_resets_handlers_keeps_ignored`, `exec_keeps_pending_and_mask`, `pdeathsig_on_parent_exit`, `prctl_name_roundtrip`, `priority_rules` (raise ok, lower EACCES as uid 1000), `getrusage_children_after_wait`, `times_ticks_advance`, `getcpu_zero`, `sysinfo_counts_processes`.

  `make -C Source/Switch2 check-guest-job-control check-proc-suite-native` → pass; NRO builds.

### Task 6: Sessions and terminal job control [Opus]

**Files:**
- Modify: `Source/Switch2/runtime/process/GuestJobControl.h` (session rules), `tests/unit/GuestJobControlTest.cpp`, `GuestRuntime.cpp` (`SetProcessGroup`, `GetProcessGroup`, `TerminalIoctl`, `ReadTerminal`, terminal write path, `RunThreads` stop handling, cases 109/111/112/121/124), `GuestTerminal.h`

**Interfaces:**
- Produces (header):

```cpp
struct SessionView { uint32_t Pid, Group, Session; bool IsChild, ChildExecd; };
// setpgid(pid, pgid) rule set (Linux kernel/sys.c): returns 0 or -errno.
int64_t CheckSetpgid(const SessionView& Caller, const SessionView& Target, bool GroupExistsInSession, uint32_t NewGroup);
// setsid: EPERM if the caller already leads a process group.
int64_t CheckSetsid(const SessionView& Caller, bool GroupIdInUse);
// Terminal signal for a control character with ISIG set; 0 if none.
uint32_t TerminalSignalFor(uint8_t Byte, const uint8_t ControlChars[19], uint32_t LocalFlags);
```

- Produces (runtime): `GuestProcess::Session`; terminal state `ForegroundGroup`, `TerminalSession` (`0` = no controlling session), `GuestProcess::HasTerminal`; `void StopProcess(GuestProcess&, uint32_t Signal)`, `void ContinueProcess(GuestProcess&)`, `void SignalGroup(uint32_t Group, SigInfo)`, `bool IsOrphanedGroup(uint32_t Group)`.
- Consumes: Task 4 `SendSignal`, Task 5 `ExitProcess` hook.

- [ ] **Step 1: Unit tests** for `CheckSetpgid`: target not caller/child → ESRCH; child that exec'd → EACCES; target is session leader → EPERM; different session → EPERM; group not in session and `NewGroup != Target.Pid` → EPERM; negative pgid → EINVAL (pass as int via wrapper). `CheckSetsid`: group leader → EPERM. `TerminalSignalFor`: `0x03` with ISIG → SIGINT, `0x1a` → SIGTSTP, `0x1c` → SIGQUIT, without ISIG → 0, `_POSIX_VDISABLE` (0) never matches. Run → fail → implement → pass.

- [ ] **Step 2: Sessions.** Root process starts as its own session leader (`Session = Group = PID = 100`) holding the console terminal as controlling terminal (like a login shell). `setsid` (112): new session and group, drops controlling terminal (`HasTerminal = false`). `getsid` (124): pid 0 or existing → session; ESRCH. `getpgid` (121), `getpgrp` (111). Replace `SetProcessGroup` body with `CheckSetpgid`. Fork inherits `Session`, `HasTerminal`.

- [ ] **Step 3: Terminal ioctls.** `TIOCGPGRP` (0x540f): ENOTTY if the caller's session does not own the terminal. `TIOCSPGRP` (0x5410): ENOTTY if not controlling terminal; EPERM if group not in caller's session; ESRCH for no such group; background caller gets SIGTTOU unless blocked/ignored (then allowed). `TIOCGSID` (0x5429): real session ID (currently returns `ProcessID` for both). `TIOCSCTTY` (0x540e): session leader without a controlling terminal acquires it (EPERM otherwise; root with arg 1 steals). `TIOCNOTTY` (0x5422): leader releases → SIGHUP+SIGCONT to the foreground group. Verify case numbers against `asm-generic/ioctls.h`.

- [ ] **Step 4: Terminal signal generation.** In `PollTerminalInput`, for each byte with `ISIG` in `Terminal.Attributes.c_lflag`: if `TerminalSignalFor` returns a signal, do not queue the byte; flush input/output unless `NOFLSH`; `SignalGroup(ForegroundGroup, {Signo, SiKernel})`; echo `^C` when `ECHO|ECHOCTL`. This runs even while a guest is busy because Task 3 preempts on `TerminalHint`.

- [ ] **Step 5: Background access.** Reads from the controlling terminal by a process whose group is not `ForegroundGroup`: if SIGTTIN is ignored or blocked or the group is orphaned → EIO; else `SignalGroup(own, SIGTTIN)` and restart the read (ERESTARTSYS). Writes: only when `TOSTOP` is set, same rule with SIGTTOU (ignored/blocked → write proceeds). `TCSETS*` from background → SIGTTOU regardless of TOSTOP.

- [ ] **Step 6: Stop and continue.** `StopProcess`: all threads leave the run queue (`GuestProcess::State = Stopped`; `Ready()` returns false for threads of stopped processes), `ReportStop = true`, `StopSignal = sig`, notify parent with `SIGCHLD` `CLD_STOPPED` unless parent's SIGCHLD action has `SA_NOCLDSTOP`, wake waiters. Stop signals sent to an orphaned group (TSTP/TTIN/TTOU only) are discarded. `ContinueProcess`: on SIGCONT generation (not delivery) — resume, `ReportContinue = true`, `CLD_CONTINUED` to parent (unless SA_NOCLDSTOP), discard pending stop signals; SIGCONT itself is then delivered to a handler if one is installed. Interrupted sleeps follow Task 4 restart-block rules.

- [ ] **Step 7: Orphaned groups.** When a process exits or changes group, any group that becomes orphaned (no member has a parent in a different group of the same session) and has a stopped member gets SIGHUP then SIGCONT.

- [ ] **Step 8: Suite cases** (`session/` group; terminal-dependent cases skip when `isatty(0)` is false, and the interactive checklist in Task 18 covers them on the console):
  `setsid_new_session`, `setsid_eperm_group_leader`, `setpgid_rules` (each errno), `setpgid_after_exec_eacces`, `getsid_esrch`, `stop_and_continue_reported` (child `raise(SIGSTOP)`; parent `waitpid(WUNTRACED)` → `WIFSTOPPED/WSTOPSIG==SIGSTOP`; `kill(SIGCONT)`; `waitpid(WCONTINUED)` → `WIFCONTINUED`), `sigcont_discards_pending_stop`, `sa_nocldstop_suppresses`, `orphaned_stopped_group_gets_hup` (child forks grandchild in a new group, grandchild stops, child exits → grandchild handler sees SIGHUP then SIGCONT), `tstp_to_orphan_discarded`, `tiocsctty_and_tiocgpgrp` (uses `posix_openpt`? no pty here → skip unless `/dev/ptmx` works; inventory notes ptys are milestone-3/backlog), `background_read_ttin` (isatty-gated).

### Task 7: (reserved) Restart and stop integration review [Opus]

- [ ] **Step 1:** Walk every `WaitKind` and every `RetrySyscall` return in `GuestRuntime.cpp`; for each record in a table in `ProcessInventory.md` ("Interruption and restart") whether a caught signal yields EINTR, restart, or `restart_syscall`, compared with the Linux source of the same syscall. Fix mismatches.
- [ ] **Step 2:** Add `signal/restart_table_<syscall>` cases for read (pipe), write (full pipe), wait4, futex, flock, fcntl F_SETLKW, accept, connect (Unix), epoll_wait, poll, select, nanosleep, sigsuspend — each asserts the Linux result with and without SA_RESTART.
- [ ] **Step 3:** `make -C Source/Switch2 check-proc-suite-native` passes; NRO builds.

### Task 8: Credentials and capabilities [Sonnet]

**Files:**
- Create: `Source/Switch2/runtime/process/GuestCredentials.h`, `Source/Switch2/tests/unit/GuestCredentialsTest.cpp`
- Modify: `GuestRuntime.cpp` (`SetGuestIDs`, `SetFilesystemID`, `GuestGroups`, `GuestPrivilegeControl`, `InstallExec`, `GuestProcess`, cases 105/106/113/114/125/126)

**Interfaces:**
- Produces:

```cpp
namespace Switch2Guest {
constexpr uint32_t NoId = UINT32_MAX;
constexpr uint32_t CapLast = 40; // CAP_CHECKPOINT_RESTORE
constexpr uint64_t CapFull = (uint64_t {1} << (CapLast + 1)) - 1;
constexpr uint32_t CapChown = 0, CapDacOverride = 1, CapKill = 5, CapSetgid = 6, CapSetuid = 7, CapSetpcap = 8, CapSysNice = 23, CapSysTime = 25, CapWakeAlarm = 35;
struct Credentials {
  std::array<uint32_t, 4> Uid {1000, 1000, 1000, 1000}; // real, effective, saved, fs
  std::array<uint32_t, 4> Gid {1000, 1000, 1000, 1000};
  uint64_t CapEffective {}, CapPermitted {}, CapInheritable {}, CapBounding {CapFull}, CapAmbient {};
  bool KeepCaps {};
  bool Capable(uint32_t Cap) const { return CapEffective >> Cap & 1; }
};
Credentials RootCredentials();                      // all ids 0, eff=perm=CapFull
int64_t SetUid(Credentials&, uint32_t Uid);         // setuid(2)
int64_t SetReUid(Credentials&, uint32_t Real, uint32_t Effective);
int64_t SetResUid(Credentials&, uint32_t Real, uint32_t Effective, uint32_t Saved);
int64_t SetFsUid(Credentials&, uint32_t Uid);       // returns old fsuid, never an error
int64_t SetGid(Credentials&, uint32_t Gid);
int64_t SetReGid(Credentials&, uint32_t Real, uint32_t Effective);
int64_t SetResGid(Credentials&, uint32_t Real, uint32_t Effective, uint32_t Saved);
int64_t SetFsGid(Credentials&, uint32_t Gid);
void FixCapsAfterUidChange(Credentials&, const std::array<uint32_t, 4>& Old); // cap_emulate_setxuid
void ApplyExecCaps(Credentials&, bool SetUidRoot, bool NoNewPrivs);         // root-as-full-caps rules
int64_t CapSet(Credentials&, uint64_t Effective, uint64_t Permitted, uint64_t Inheritable);
}
```

Rules (Linux `kernel/sys.c`, `security/commoncap.c`):
- `setuid(u)`: `u == -1` EINVAL. If `CAP_SETUID`: ruid=euid=suid=fsuid=u. Else if `u == ruid || u == suid`: euid=fsuid=u. Else EPERM.
- `setreuid(r,e)`: unprivileged: `r` ∈ {-1, ruid, euid}; `e` ∈ {-1, ruid, euid, suid}, else EPERM. If `r != -1 || (e != -1 && e != old ruid)`: suid = new euid. fsuid = euid.
- `setresuid`: unprivileged each non -1 value ∈ {ruid, euid, suid}. fsuid = euid.
- `setfsuid(u)`: returns old fsuid; applies only if privileged or `u` ∈ {ruid, euid, suid, fsuid}.
- Group variants use `CAP_SETGID` identically.
- `FixCapsAfterUidChange`: if any old {r,e,s} was 0 and all new {r,e,s} are nonzero and not KeepCaps: clear permitted and effective (and ambient). If old euid 0 and new euid nonzero: clear effective. If old euid nonzero and new euid 0: effective = permitted. fsuid change from 0 → nonzero clears fs caps (CHOWN, DAC_OVERRIDE, DAC_READ_SEARCH, FOWNER, FSETID, LINUX_IMMUTABLE, MKNOD, MAC_OVERRIDE) from effective; nonzero → 0 sets them from permitted.
- `ApplyExecCaps`: if new euid 0 or ruid 0 (and not NoNewPrivs-blocked setuid): permitted = bounding | inheritable... use the "root is full caps" rule: `pP' = (pI & bset) | bset` → `bset`; effective = permitted if euid 0, else ambient only. Non-root: permitted = ambient, effective = ambient.
- `capset`: new inheritable ⊆ old (inheritable | permitted) unless CAP_SETPCAP (then ⊆ old inheritable | bounding); new permitted ⊆ old permitted; new effective ⊆ new permitted; violations EPERM.
- `capget`/`capset` ABI: header `{u32 version; i32 pid}`. Versions `0x19980330` (1 data struct), `0x20071026` and `0x20080522` (2 structs `{effective, permitted, inheritable}` u32 each). Unknown version: write `0x20080522` into header, return EINVAL (capget with NULL data returns 0 after writing). `pid` < 0 EINVAL; capget for another pid reads that process (ESRCH if missing); capset pid must be 0 or self (EPERM).
- Replace `UserIDs/GroupIDs` arrays in `GuestProcess` with `Credentials Creds` and update all users (`grep -n 'UserIDs\|GroupIDs' GuestRuntime.cpp`). Authorization checks that compared `UserIDs[1] == 0` must use `Creds.Capable(...)`: file DAC (CAP_DAC_OVERRIDE), chown (CAP_CHOWN), kill (CAP_KILL), set*id, `CAP_SYS_TIME` (Task 9), `CAP_WAKE_ALARM`, `CAP_SYS_NICE` (Task 5 priority). Keep `<program>.user` launch credentials: `0 0` → `RootCredentials()`.
- Also route `setuid` (105), `setgid` (106), `setreuid` (113), `setregid` (114), `capget` (125), `capset` (126); `PR_CAPBSET_READ`, `PR_CAPBSET_DROP` (needs CAP_SETPCAP, EPERM), `PR_SET_KEEPCAPS`, `PR_GET_KEEPCAPS`, `PR_CAP_AMBIENT` (47: IS_SET/RAISE/LOWER/CLEAR_ALL).

- [ ] **Step 1:** Unit tests covering each bullet (one CHECK per rule), e.g.:

```cpp
auto C = Credentials {};                // uid 1000
CHECK(SetUid(C, 0) == -1);              // EPERM
C.Uid = {1000, 1000, 2000, 1000};
CHECK(SetUid(C, 2000) == 0 && C.Uid[1] == 2000 && C.Uid[0] == 1000);
auto R = RootCredentials();
CHECK(SetResUid(R, 1000, 1000, 1000) == 0 && R.CapPermitted == 0 && R.CapEffective == 0);
auto K = RootCredentials(); K.KeepCaps = true;
CHECK(SetResUid(K, 1000, 1000, 1000) == 0 && K.CapPermitted == CapFull && K.CapEffective == 0);
CHECK(SetUid(C, NoId) == -22);
```

Run `make -C Source/Switch2 check-guest-credentials` → fail → implement → pass.

- [ ] **Step 2:** Glue + migrate `UserIDs/GroupIDs`. `make -C Source/Switch2 check-fs-suite-native` must still pass (unchanged), NRO builds.

- [ ] **Step 3:** Suite cases (`cred/`; root-only cases skip as uid 1000 and run in `proc-suite-root.elf`):
  `setuid_unprivileged_rules`, `setreuid_saved_update`, `setresuid_unprivileged`, `setfsuid_returns_old`, `setgid_family_mirrors_uid`, `setgroups_requires_cap` (EPERM as 1000), `getgroups_size_einval`, `root_drop_clears_caps`, `keepcaps_preserves_permitted`, `capget_versions_and_einval`, `capset_cannot_raise`, `capbset_read`, `ambient_raise_requires_permitted_inheritable`, `exec_as_root_gets_full_caps`, `kill_needs_matching_uid` (root child drops to 2000, signals a 3000 process → EPERM).

### Task 9: Clocks [Sonnet]

**Files:**
- Create: `Source/Switch2/runtime/process/GuestClocks.h`, `Source/Switch2/tests/unit/GuestClocksTest.cpp`
- Modify: `GuestRuntime.cpp` (`ClockGuest`, `RealTimeNs`, `SleepGuest`, cases 96/159/164/201/227/228/229/305)

**Interfaces:**
- Produces:

```cpp
namespace Switch2Guest {
enum class ClockKind { Realtime, Monotonic, Boottime, Tai, MonotonicRaw, RealtimeCoarse, MonotonicCoarse,
                       ProcessCpu, ThreadCpu, RealtimeAlarm, BoottimeAlarm, Invalid };
// Linux clockid decoding incl. dynamic CPU clocks: clockid < 0 encodes ~pid << 3 | (perthread 4) | type (0 PROF, 1 VIRT, 2 SCHED; 3 = FD -> EINVAL here).
struct DecodedClock { ClockKind Kind; uint32_t Pid; bool PerThread; };
DecodedClock DecodeClock(int32_t Id);
constexpr uint64_t CoarseResolutionNs = 4'000'000; // CONFIG_HZ=250 on Debian kernels
uint64_t ResolutionNs(ClockKind);                  // 1 for hrtimer clocks and CPU clocks, Coarse for coarse
struct RealtimeBase {                              // realtime = Base + (monotonic - MonoAtBase)
  int64_t BaseNs; uint64_t MonoAtBase;
  int64_t Now(uint64_t Mono) const { return BaseNs + static_cast<int64_t>(Mono - MonoAtBase); }
  void Set(int64_t Realtime, uint64_t Mono) { BaseNs = Realtime; MonoAtBase = Mono; }
};
constexpr int64_t TaiOffsetNs = 0; // adjtimex TAI offset default (Linux starts at 0 until set)
}
```

- Realtime: at runtime construction sample `time(nullptr)` (Horizon RTC, seconds) and `MonotonicNs()` once into `RealtimeBase`. This replaces `RealTimeNs()` everywhere (sleeps, futex absolute timeouts, file timestamps that use realtime — grep `time(nullptr)`). Sub-second precision is real elapsed time; absolute accuracy stays the RTC's ±1 s. Coarse clocks return the value rounded down to `CoarseResolutionNs`.
- CPU clocks: process = sum of `CpuTicks` of the process (live, including the running quantum); thread = per-`GuestThread` `CpuTicks` (add field, accumulate in `RunThreads`). `VIRT` and `PROF` types return the same (no stime split; inventory note). Per-pid clocks: ESRCH... Linux returns EINVAL for a nonexistent pid in `clock_gettime`; follow Linux 7.3 (verify natively in the suite).
- `clock_getres` (229): writes `{0, ResolutionNs}`; NULL output allowed; invalid id EINVAL.
- `gettimeofday` (96): timeval + optional timezone `{0,0}`; NULL tv allowed. `time` (201): returns seconds, writes if non-null (EFAULT on bad pointer).
- `settimeofday` (164) / `clock_settime` (227): only `CLOCK_REALTIME` settable (EINVAL for others, incl. MONOTONIC); needs `CAP_SYS_TIME` (EPERM); `tv_usec >= 1e6` EINVAL; sets the **guest** `RealtimeBase` (process-global for this launch, not persisted, does not change Horizon's clock — inventory deliberate difference). Absolute `TIMER_ABSTIME` realtime sleeps already converted stay as-is (Linux re-arms them on clock set — note as difference, or handle via Task 10's `CancelOnSet` list).
- `adjtimex` (159) / `clock_adjtime` (305): `modes == 0` read-only for any user returns `TIME_OK` (0) and fills `struct timex` (208 bytes: `time` from realtime, `tick = 10000`, `status = STA_UNSYNC` 0x40 → returns `TIME_ERROR` (5) as Linux does when unsynchronised; verify natively); any write mode needs CAP_SYS_TIME (EPERM); `ADJ_OFFSET_SS_READ`... for root accept `ADJ_SETOFFSET` (adjusts `RealtimeBase`) and `ADJ_TAI`; other write modes are accepted and their fields stored for read-back (frequency, maxerror, esterror, status, constant) without steering the clock. Inventory note.
- Fix carry-over: `clock_nanosleep` on `CLOCK_PROCESS_CPUTIME_ID`: Linux returns EINVAL? Check natively — Linux supports process CPU clock nanosleep (posix_cpu_nsleep) for the own process; thread CPU clock → EINVAL. Implement process CPU sleep as wait until the process's CPU time reaches the target (deadline checked in `Ready` from `CpuTicks`; while the sleeper blocks, other threads must consume CPU). Root alarm sleeps: implement as the corresponding non-alarm clock (Horizon offers no wake-from-suspend; console never suspends a running app) — remove the EOPNOTSUPP; inventory note.

- [ ] **Step 1:** Unit tests: `DecodeClock(0..11)` kinds; `DecodeClock(10)`/`DecodeClock(12)` invalid; `DecodeClock(~(100 << 3) | 2)` → ProcessCpu pid 100; `DecodeClock((~(5 << 3)) | 4 | 2)` → ThreadCpu pid 5 PerThread; type 3 invalid; `ResolutionNs(RealtimeCoarse) == 4'000'000`; `RealtimeBase` arithmetic. Fail → implement → pass (`make -C Source/Switch2 check-guest-clocks`).
- [ ] **Step 2:** Glue; update `FilesystemInventory.md` deliberate-difference row for `clock_nanosleep` and realtime resolution to point at `ProcessInventory.md`.
- [ ] **Step 3:** Suite cases (`clock/`): `all_ids_gettime`, `invalid_ids_einval`, `getres_values` (compare with native — coarse equals `sysconf`-independent 4 ms on Debian kernels; on the native host read the real value and assert `<= 10 ms`), `realtime_subsecond_advances` (two reads 20 ms apart differ by 15–100 ms), `realtime_matches_time` (`|time() - ts.tv_sec| <= 1`), `monotonic_never_decreases`, `process_cpu_advances_with_work`, `thread_cpu_per_thread`, `pid_cpu_clock_of_child`, `settime_eperm_unprivileged`, `settime_monotonic_einval`, `settimeofday_bad_usec_einval`, `adjtimex_readonly`, `cpu_nanosleep_process_clock`, `gettimeofday_null_tv_ok`.

### Task 10: Timers [Opus]

**Files:**
- Create: `Source/Switch2/runtime/process/GuestTimers.h`, `Source/Switch2/tests/unit/GuestTimersTest.cpp`
- Modify: `GuestRuntime.cpp` (timer table per process, scheduler expiry, cases 36/37/38/222–226/283/286/287, timerfd `OpenFile` kind, `ReadDescriptor`, poll readiness hook)

**Interfaces:**
- Produces:

```cpp
namespace Switch2Guest {
struct TimerSpec { uint64_t ValueNs, IntervalNs; };          // 0 value = disarmed
struct ArmedTimer {
  uint64_t Deadline {};     // in the timer's own clock domain (monotonic ns, or CPU ns)
  uint64_t IntervalNs {};
  uint64_t Overrun {};
  bool Armed() const { return Deadline != 0; }
  // Expire at Now: returns expirations since last call (>= 1) and re-arms (interval) or disarms.
  uint64_t Expire(uint64_t Now);
  TimerSpec Remaining(uint64_t Now) const;                    // it_value rounded up to >= 1ns while armed
};
// alarm(2): returns previous remaining seconds, rounded to nearest, at least 1 if any time remained.
uint32_t AlarmSecondsLeft(uint64_t RemainingNs);
// itimerval microsecond conversion: values are rounded up to the next microsecond (Linux timeval_to_ktime).
uint64_t FromTimeval(int64_t Sec, int64_t Usec);
// timer_getoverrun clamps at DELAYTIMER_MAX = INT_MAX.
int32_t ClampOverrun(uint64_t Overrun);
}
```

- Runtime model: each process owns `std::vector<PosixTimer>` (id, clock kind, `ArmedTimer`, `SigEvent {Notify, Signo, Value, Tid}`, `PendingSignal` flag for overrun accounting) plus three itimers (`ITIMER_REAL` monotonic, `ITIMER_VIRTUAL` and `ITIMER_PROF` on process CPU). Timerfds hold an `ArmedTimer` in their `OpenFile` (`TimerFd` struct: clock, ticks counter, `CancelOnSet`, `Canceled`). A scheduler step `FireTimers(Now)` runs at the top of each `RunThreads` pass and from `Ready()` evaluation; `NextTimerDeadline` (atomic) feeds Task 3's watchdog and the idle sleep (`svcSleepThread(min(8 ms, until next deadline))`).
- Signal notification: overrun accounting as Linux: while the previous timer signal is still pending (queued, not delivered), further expirations add to `Overrun` instead of queueing; `si_overrun` reported at delivery; `timer_getoverrun` returns the overrun of the last delivered signal.
- Syscalls:
  - `alarm` (37) → ITIMER_REAL seconds; `getitimer`/`setitimer` (36/38): which 0/1/2 else EINVAL; `tv_usec` outside [0, 1e6) EINVAL; setitimer NULL new value: Linux 7.3 treats as zero (verify native, there was a change in 6.x printing a warning); old value written.
  - `timer_create` (222): clocks REALTIME, MONOTONIC, BOOTTIME, TAI? (TAI supported since 3.10 → yes), PROCESS/THREAD CPU and per-pid CPU clocks, alarm clocks (EPERM without CAP_WAKE_ALARM); `sigevent` NULL → SIGEV_SIGNAL SIGALRM value=timer id; `SIGEV_THREAD_ID` (4) tid must be in the process (EINVAL); signo 1..64 (EINVAL); writes id (EFAULT → timer not created). ids per process from 0. Limit by `RLIMIT_SIGPENDING` → EAGAIN.
  - `timer_settime` (223): flags `TIMER_ABSTIME`; `it_value` validation EINVAL; returns old; unknown id EINVAL. Absolute realtime converted using `RealtimeBase`.
  - `timer_gettime` (224), `timer_getoverrun` (225), `timer_delete` (226) (removes pending timer signal).
  - Exec deletes POSIX timers and keeps ITIMER_REAL? Linux: exec keeps itimers and alarm; POSIX timers are deleted. Fork: child has no timers, no itimers (itimers are not inherited).
  - `timerfd_create` (283): clocks REALTIME, MONOTONIC, BOOTTIME, *_ALARM (EPERM without cap) else EINVAL; flags `TFD_CLOEXEC|TFD_NONBLOCK` else EINVAL. `timerfd_settime` (286): flags `TFD_TIMER_ABSTIME` (1) and `TFD_TIMER_CANCEL_ON_SET` (2, only with ABSTIME on REALTIME clocks) else EINVAL; EBADF/EINVAL for non-timerfd. `timerfd_gettime` (287). read: < 8 bytes EINVAL; 0 ticks → block / EAGAIN; returns u64 ticks and resets; `ECANCELED` once after a realtime clock set when CANCEL_ON_SET. `ioctl(TFD_IOC_SET_TICKS)` (0x40085400) sets ticks (EINVAL for 0). Poll: readable when ticks > 0 or canceled.
  - `clock_settime` (Task 9) walks CANCEL_ON_SET timerfds and marks them canceled; also re-evaluates absolute realtime POSIX timers and timerfds (Linux re-arms them) — implement by storing absolute realtime deadlines in realtime domain and converting at each `FireTimers`.

- [ ] **Step 1:** Unit tests: `ArmedTimer` one-shot expiry returns 1 and disarms; periodic with Now 3.5 intervals late returns 4 and next deadline aligned to the original phase (`Deadline + k*Interval`); `Remaining` never 0 while armed; `AlarmSecondsLeft(1)` == 1, `(1'499'999'999)` == 1, `(1'500'000'000)` == 2; `FromTimeval(0, 1)` == 1000; `ClampOverrun(UINT64_MAX)` == INT_MAX. Fail → implement → pass (`make -C Source/Switch2 check-guest-timers`).
- [ ] **Step 2:** Runtime glue as above; NRO builds.
- [ ] **Step 3:** Suite cases (`timer/`):
  `alarm_returns_remaining`, `alarm_zero_cancels`, `alarm_delivers_sigalrm`, `setitimer_real_periodic` (3 ticks in ~35 ms with 10 ms interval), `setitimer_bad_which_and_usec`, `itimer_virtual_counts_cpu` (busy loop), `itimer_not_inherited_by_fork`, `alarm_survives_exec` (exec helper `/bin/proc-suite.elf --alarm-left` prints remaining; helper mode in suite main), `posix_timer_default_sigevent` (SIGALRM, `si_value == timer id`, `si_code == SI_TIMER`), `posix_timer_thread_id`, `posix_timer_overrun` (10 ms periodic while signal blocked 100 ms → overrun ≥ 5), `posix_timer_errors`, `posix_timer_cpu_clock`, `timer_delete_drops_pending`, `timerfd_read_ticks`, `timerfd_nonblock_eagain`, `timerfd_short_read_einval`, `timerfd_abstime`, `timerfd_poll_readable`, `timerfd_set_ticks_ioctl`, `timerfd_cancel_on_set` (root only), `timerfd_errors`.

### Task 11: eventfd [Sonnet]

**Files:**
- Create: `Source/Switch2/runtime/ipc/GuestEventFd.h`, `Source/Switch2/tests/unit/GuestEventFdTest.cpp`
- Modify: `GuestRuntime.cpp` (`OpenFile` gets `std::shared_ptr<EventCounter> Event`, cases 284/290, `ReadDescriptor`, `WriteDescriptor`, stat/fdinfo shape, poll readiness)

**Interfaces:**
- Produces:

```cpp
namespace Switch2Guest {
constexpr uint64_t EventFdMax = UINT64_MAX - 1;
struct EventCounter {
  uint64_t Value {};
  bool Semaphore {};
  // read: -EAGAIN when Value == 0 (caller decides to block); else returns the value read and updates Value.
  int64_t Read(uint64_t& Out);
  // write: -EINVAL for UINT64_MAX; -EAGAIN when Value + Add would exceed EventFdMax; else adds.
  int64_t Write(uint64_t Add);
  bool Readable() const { return Value > 0; }
  bool Writable() const { return Value < EventFdMax; }
};
}
```

- Syscalls: `eventfd2(initval, flags)`: flags ⊆ `EFD_SEMAPHORE(1)|EFD_NONBLOCK(04000)|EFD_CLOEXEC(02000000)` else EINVAL. `eventfd(initval)` (284) = flags 0. read/write buffer < 8 → EINVAL; blocking via `WaitForIO()`; each successful read or write `++IOEpoch` to wake waiters. `fstat`: anon inode shape. Write `event/eventfd_stat_shape` first, read `st_mode`, `st_nlink`, `st_size` on the native host, and make the guest return the same values. `/proc/self/fdinfo` out of scope (procfs later).
- Poll readiness: `POLLIN` when Readable, `POLLOUT` when Writable, `POLLERR` when Value == UINT64_MAX (unreachable through write; ignore).

- [ ] **Step 1:** Unit tests: read returns value and zeroes; semaphore read returns 1 and decrements; write UINT64_MAX EINVAL; write that overflows EAGAIN; `Value = EventFdMax` write 0 ok. Fail → implement → pass (`make -C Source/Switch2 check-guest-eventfd`).
- [ ] **Step 2:** Glue. NRO builds.
- [ ] **Step 3:** Suite (`event/`): `eventfd_counter`, `eventfd_semaphore`, `eventfd_nonblock_eagain`, `eventfd_short_buffer_einval`, `eventfd_bad_flags_einval`, `eventfd_blocking_read_woken_by_thread`, `eventfd_poll_states`, `eventfd_fork_shares_counter`, `eventfd_stat_shape`.

### Task 12: signalfd [Sonnet]

**Files:**
- Modify: `Source/Switch2/runtime/process/GuestSignals.h` (add `SignalFdInfo` + conversion), `tests/unit/GuestSignalsTest.cpp`, `GuestRuntime.cpp` (cases 282/289, `OpenFile` `SignalFdMask`, read path, poll readiness)

**Interfaces:**
- Consumes: Task 4 `SignalQueue::Dequeue`, `SigInfo`.
- Produces:

```cpp
struct SignalFdInfo { // struct signalfd_siginfo, 128 bytes
  uint32_t Signo; int32_t Errno, Code; uint32_t Pid, Uid; int32_t Fd; uint32_t Tid, Band, Overrun, Trapno;
  int32_t Status, Int; uint64_t Ptr, Utime, Stime, Addr; uint16_t AddrLsb; uint8_t Pad[46];
};
static_assert(sizeof(SignalFdInfo) == 128);
SignalFdInfo ToSignalFdInfo(const SigInfo&);
```

- Syscalls: `signalfd4(fd, mask, sizemask, flags)`: `sizemask != 8` EINVAL; flags ⊆ `SFD_NONBLOCK|SFD_CLOEXEC` else EINVAL; `fd == -1` creates; else updates the mask of an existing signalfd (EINVAL if not a signalfd, EBADF if not open). `signalfd` (282) = flags 0. SIGKILL/SIGSTOP silently removed from the mask. read: count < 128 EINVAL; dequeues from the **reading thread's** pending queue then the process shared queue, only signals in the mask, as many records as fit; none → block (only if not nonblocking) or EAGAIN. Readiness: POLLIN when a matching signal is pending for the thread doing the poll (Linux uses the poller's pending set). Signals consumed by signalfd are not delivered to handlers; the usual pattern blocks them with `sigprocmask` first.

- [ ] **Step 1:** Unit test `ToSignalFdInfo` field mapping (kill: pid/uid; timer: overrun/tid/ptr; child: status/utime). Fail → implement → pass.
- [ ] **Step 2:** Glue. NRO builds.
- [ ] **Step 3:** Suite (`event/`): `signalfd_reads_blocked_signal`, `signalfd_multiple_records`, `signalfd_short_buffer_einval`, `signalfd_update_mask`, `signalfd_nonblock_eagain`, `signalfd_poll_readable`, `signalfd_ignores_kill_stop_in_mask`, `signalfd_sigchld_from_child` (classic pattern: block SIGCHLD, signalfd, fork/exit, read → `ssi_pid == child`, `ssi_status == code`).

### Task 13: Readiness core, poll family and epoll [Opus]

**Files:**
- Create: `Source/Switch2/runtime/ipc/GuestEpoll.h`, `Source/Switch2/tests/unit/GuestEpollTest.cpp`
- Modify: `GuestRuntime.cpp` (`PollGuest`, `PollGuestWithTimespec`, `SelectGuest`, new `Readiness(OpenFile&, GuestThread&)`, cases 7/23/213/232/233/270/271/281/291/441, `CloseGuest` / `OpenFile` destructor for epoll removal)

**Interfaces:**
- Produces: `uint32_t Readiness(const std::shared_ptr<OpenFile>&, const GuestThread& Poller)` — single source of `POLLIN|POLLOUT|POLLERR|POLLHUP|POLLRDHUP|POLLPRI|POLLRDNORM|POLLWRNORM` for every descriptor kind (regular file, directory, console/terminal, pipe/FIFO, socket, memfd, eventfd, timerfd, signalfd, pidfd, epoll (readable when its ready list is non-empty), inotify). `poll`, `select`, `epoll` all call it. Header:

```cpp
namespace Switch2Guest {
constexpr uint32_t EpollIn = 0x1, EpollPri = 0x2, EpollOut = 0x4, EpollErr = 0x8, EpollHup = 0x10, EpollRdNorm = 0x40,
  EpollRdBand = 0x80, EpollWrNorm = 0x100, EpollWrBand = 0x200, EpollMsg = 0x400, EpollRdHup = 0x2000,
  EpollExclusive = 1u << 28, EpollWakeup = 1u << 29, EpollOneShot = 1u << 30, EpollEt = 1u << 31;
struct __attribute__((packed)) EpollEvent { uint32_t Events; uint64_t Data; };
static_assert(sizeof(EpollEvent) == 12);
template <typename Key> class EpollSet {
public:
  int64_t Add(Key K, uint32_t Events, uint64_t Data);   // -EEXIST
  int64_t Modify(Key K, uint32_t Events, uint64_t Data); // -ENOENT; -EINVAL when EPOLLEXCLUSIVE involved
  int64_t Remove(Key K);                                 // -ENOENT
  // Collect up to Max ready events. Readiness(K) returns current poll mask.
  // LT: report while ready. ET: report only on a new readiness edge since last report (tracked by a generation
  // counter the caller bumps whenever the file's state changes). ONESHOT: disable after one report until Modify.
  template <typename ReadyFn, typename GenFn>
  size_t Collect(EpollEvent* Out, size_t Max, ReadyFn Readiness, GenFn Generation);
  bool Contains(Key K) const;
  void Forget(Key K);                                    // description closed: drop silently
private:
  struct Item { Key K; uint32_t Events; uint64_t Data; uint64_t SeenGeneration; bool Disabled; };
  std::vector<Item> Items;                               // insertion order; round-robin cursor for fairness
  size_t Cursor {};
};
}
```

- Interest key: Linux keys on (file description, fd number). Use `{std::weak_ptr<OpenFile>, int Fd}`; an item disappears when the description is destroyed (all dup'd fds closed) — not when one fd closes. Readiness per item uses the description.
- Edge triggering needs a per-description change generation: add `uint64_t Generation` to `OpenFile` and to `StreamSocket` (pipe/socket buffers), incremented on every state change that affects readiness (write into pipe, read out, shutdown, counter changes, new inotify event, timer tick, signal queued for signalfd readers). ET reports when `Generation != SeenGeneration` and the item is ready.
- Syscalls: `epoll_create(size)`: size <= 0 EINVAL. `epoll_create1(flags)`: flags ⊆ EPOLL_CLOEXEC else EINVAL. `epoll_ctl`: EBADF for bad epfd/fd; EINVAL epfd not epoll or `fd == epfd`; EPERM target doesn't support poll (regular files and directories on Linux → EPERM); EINVAL `EPOLLEXCLUSIVE` with MOD or with ONESHOT or on epoll target; ELOOP nested cycle or depth > 5 (`EP_MAX_NESTS` 4); EFAULT event pointer for ADD/MOD (DEL ignores it); `EPOLLERR|EPOLLHUP` always reported. `epoll_wait`/`epoll_pwait`/`epoll_pwait2`: maxevents <= 0 or > `INT_MAX / 12` EINVAL; EFAULT output; timeout ms / timespec (pwait2: NULL = infinite, invalid ts EINVAL); sigmask atomically installed for the wait (size 8 else EINVAL) and restored after — a caught signal delivered during the wait runs with the old mask saved in the frame (same as sigsuspend). Never restarted (EINTR).
- poll/ppoll: `nfds > RLIMIT_NOFILE` EINVAL; negative fd ignored (revents 0); closed fd → POLLNVAL; ppoll sigmask semantics like pwait; timeout written back remaining for ppoll? (Linux writes remaining time to the user timespec for ppoll/pselect6 — implement); select: `nfds < 0` EINVAL, EBADF for a set bit on a closed fd, writes back remaining timeout; pselect6 `sigdata` {ptr,size}.

- [ ] **Step 1:** Unit tests for `EpollSet` with a fake readiness map: add twice EEXIST; modify missing ENOENT; LT reports twice; ET reports once until generation bumps; ONESHOT reports once until Modify; Forget drops; `Collect` round-robins (with Max 1 and two ready items, alternates); EXCLUSIVE+MOD EINVAL. Fail → implement → pass (`make -C Source/Switch2 check-guest-epoll`).
- [ ] **Step 2:** Implement `Readiness()` and migrate poll/select to it (keep current behavior for existing types, verify with `check-fs-suite-native` cases that use poll and the M1 acceptance bundle natively).
- [ ] **Step 3:** epoll glue; NRO builds.
- [ ] **Step 4:** Suite (`poll/`, `epoll/`):
  `poll/pipe_states`, `poll/negative_fd_ignored`, `poll/closed_fd_pollnval`, `poll/hup_after_writer_close`, `poll/ppoll_sigmask_atomic` (signal blocked outside, unblocked in mask → EINTR and handler ran), `poll/ppoll_writes_remaining`, `poll/select_ebadf`, `poll/select_nfds_einval`, `poll/pselect6_sigmask`, `poll/regular_file_always_ready`;
  `epoll/create_errors`, `epoll/ctl_errors` (EEXIST, ENOENT, EPERM regular file, EINVAL self, ELOOP cycle), `epoll/level_triggered_repeats`, `epoll/edge_triggered_once`, `epoll/oneshot_rearm`, `epoll/data_roundtrip`, `epoll/close_dup_keeps_interest` (dup fd, close original, still reported), `epoll/close_last_removes`, `epoll/nested_epoll_readable`, `epoll/pwait_sigmask`, `epoll/pwait2_timespec`, `epoll/wait_timeout_zero`, `epoll/hup_err_always`, `epoll/eventfd_timerfd_signalfd_sources`, `epoll/fork_shares_instance`, `epoll/maxevents_einval`.

### Task 14: inotify (and fanotify decision) [Opus]

**Files:**
- Create: `Source/Switch2/runtime/fs/GuestInotify.h`, `Source/Switch2/tests/unit/GuestInotifyTest.cpp`
- Modify: `GuestRuntime.cpp` (cases 253/254/255/294/300/301, event emission hooks in filesystem handlers, `OpenFile` `Inotify` instance, read path, readiness)

**Interfaces:**
- Produces:

```cpp
namespace Switch2Guest {
constexpr uint32_t InAccess = 0x1, InModify = 0x2, InAttrib = 0x4, InCloseWrite = 0x8, InCloseNowrite = 0x10,
  InOpen = 0x20, InMovedFrom = 0x40, InMovedTo = 0x80, InCreate = 0x100, InDelete = 0x200, InDeleteSelf = 0x400,
  InMoveSelf = 0x800, InUnmount = 0x2000, InQOverflow = 0x4000, InIgnored = 0x8000, InOnlyDir = 0x01000000,
  InDontFollow = 0x02000000, InExclUnlink = 0x04000000, InMaskCreate = 0x10000000, InMaskAdd = 0x20000000,
  InIsDir = 0x40000000, InOneShot = 0x80000000, InAllEvents = 0xfff;
constexpr size_t InotifyMaxQueued = 16384, InotifyMaxWatches = 8192, InotifyMaxInstances = 128;
struct InotifyEvent { int32_t Wd; uint32_t Mask, Cookie; std::string Name; };
// Encoded size: 16 + name padded with NULs to a multiple of 16 (0 when no name).
size_t EncodedSize(const InotifyEvent&);
size_t Encode(const InotifyEvent&, uint8_t* Out);
class InotifyInstance {
public:
  // Returns wd or -errno. Same inode → same wd (mask replaced, or OR'd with IN_MASK_ADD; IN_MASK_CREATE → EEXIST).
  int64_t AddWatch(uint64_t Inode, uint32_t Mask, bool IsDirectory);
  int64_t RemoveWatch(int32_t Wd);                        // queues IN_IGNORED; -EINVAL unknown wd
  // Emit an event for a watched inode; Name empty for self events. Coalesces an identical event equal to the tail.
  void Emit(uint64_t Inode, uint32_t Mask, uint32_t Cookie, std::string_view Name, bool IsDirectory);
  void InodeGone(uint64_t Inode);                         // DELETE_SELF then IGNORED, removes the watch
  // Read as many whole events as fit; -EINVAL if the first does not fit; -EAGAIN when empty.
  int64_t Read(uint8_t* Out, size_t Count);
  size_t PendingBytes() const;                            // FIONREAD
  bool Readable() const;
private:
  struct Watch { int32_t Wd; uint64_t Inode; uint32_t Mask; bool Directory; };
  std::vector<Watch> Watches;
  std::deque<InotifyEvent> Queue;
  int32_t NextWd {1};
  bool Overflowed {};
};
}
```

- Events are delivered to watches on the inode itself (self events and file events on a watched file) and to watches on the **parent directory** (child events with the child name, `IN_ISDIR` for directories). Emission hook: `void NotifyFs(uint64_t ParentInode, std::string_view Name, uint64_t Inode, uint32_t Mask, bool IsDirectory, uint32_t Cookie = 0)` in `GuestSyscalls`, iterating all live inotify instances (`std::vector<std::weak_ptr<InotifyState>>`). Call sites (grep each handler in `GuestRuntime.cpp`):
  - create file (`OpenNamed` with O_CREAT creating), `MakeNode`, `CreateSymlink`, `LinkGuest`: `IN_CREATE` on parent.
  - `CreateDirectoryGuest`: `IN_CREATE|IN_ISDIR`.
  - `UnlinkGuest`, `RemoveDirectoryGuest`: `IN_DELETE` on parent; if link count reaches 0 (and no open descriptor keeps it, for files) → `InodeGone` on that inode (`IN_DELETE_SELF` + `IN_IGNORED`). Directories: on rmdir immediately.
  - `RenameSdEntry`: cookie from a global counter; `IN_MOVED_FROM` on old parent, `IN_MOVED_TO` on new parent, `IN_MOVE_SELF` on the moved inode; a replaced destination gets `InodeGone` semantics if its last link dropped. Exchange: two pairs.
  - writes (`WriteFileAt`, truncate, fallocate that changes size, copy/splice into files): `IN_MODIFY` on inode and parent(s) — parent name requires a path; use the description's `Path` for the parent and name.
  - `ReadFileAt`/`ReadDirents`: `IN_ACCESS`. Open: `IN_OPEN` (path-only opens excluded). Close of the **last** descriptor of a description: `IN_CLOSE_WRITE` if opened for writing else `IN_CLOSE_NOWRITE`.
  - chmod/chown/utimens/xattr set/remove/link count change: `IN_ATTRIB`.
  - Changes made through the FTP server (`GuestFileService`) go through the same handlers, so they also notify.
- Syscalls: `inotify_init1(flags)`: flags ⊆ `IN_NONBLOCK|IN_CLOEXEC` else EINVAL; instance limit EMFILE. `inotify_add_watch(fd, path, mask)`: EBADF/EINVAL non-inotify fd; mask with no event bits EINVAL; `IN_MASK_ADD|IN_MASK_CREATE` together EINVAL; path resolution with `IN_DONT_FOLLOW`; `IN_ONLYDIR` on non-directory ENOTDIR; read permission on the target required (EACCES); watch limit ENOSPC. `inotify_rm_watch`: EINVAL unknown wd. read: EINVAL short buffer, blocking/EAGAIN. `ioctl(FIONREAD)` = `PendingBytes()`. Readiness POLLIN.
- fanotify (300/301): Linux built without `CONFIG_FANOTIFY` returns ENOSYS; with it, `fanotify_init` needs `CAP_SYS_ADMIN` (EPERM). Decision: implement neither; return ENOSYS for both (kernel configuration without fanotify) and record as deliberate difference. Debian `inotifywait` does not use fanotify.

- [ ] **Step 1:** Unit tests: `EncodedSize` with name "a" → 32, empty → 16, 15-char name → 32, 16-char → 48 (16 + 32 because Linux pads `len` so that the name has at least one NUL: `len = roundup(strlen+1, 16)`); AddWatch same inode returns same wd and replaces mask; MASK_ADD ORs; MASK_CREATE EEXIST; Emit filtered by mask; identical tail coalesced; overflow at 16384 emits one `IN_Q_OVERFLOW` with wd -1; RemoveWatch queues IN_IGNORED; ONESHOT watch removed after first event (+IN_IGNORED); Read EINVAL for 15-byte buffer. Fail → implement → pass (`make -C Source/Switch2 check-guest-inotify`).
- [ ] **Step 2:** Glue + hooks; `make -C Source/Switch2 check-fs-suite-native` unchanged; NRO builds.
- [ ] **Step 3:** Suite (`inotify/`): `init1_flags`, `add_watch_errors`, `same_inode_same_wd`, `create_delete_in_dir`, `mkdir_isdir_flag`, `modify_and_close_write`, `open_access_close_nowrite`, `attrib_on_chmod`, `rename_cookie_pair`, `move_self`, `delete_self_then_ignored`, `rm_watch_ignored`, `oneshot`, `onlydir_enotdir`, `dont_follow_symlink`, `short_read_einval`, `fionread`, `nonblock_eagain`, `poll_readable`, `excl_unlink` (events after unlink of an open child are suppressed with IN_EXCL_UNLINK), `fanotify_enosys` (native: skip unless the call returns ENOSYS or EPERM; guest expects ENOSYS — record the native result in the skip message).

### Task 15: Resource limits and remaining process calls [Sonnet]

**Files:**
- Modify: `GuestRuntime.cpp` (`LimitGuest`, cases 97/160/302, descriptor allocation sites, `ForkGuest`, `InstallExec`), `FilesystemInventory.md` (RLIMIT_NOFILE row)

- Store `std::array<std::pair<uint64_t,uint64_t>, 16> Limits` per process, inherited by fork and exec. Defaults (Debian user session): `RLIMIT_STACK` 8 MiB soft / unlimited hard (MainStackSize soft stays what the loader uses), `RLIMIT_NOFILE` 1024/1024 (runtime table size caps the hard limit: raising hard above 1024 → EPERM even for root; inventory note), `RLIMIT_NPROC` = threads table size, `RLIMIT_SIGPENDING` 63503? use the native host value read during reference run? Use `Threads.size() * 64`? Choose Linux default for 16 GB RAM is ~62k; set 4096 and note), `RLIMIT_CORE` 0/unlimited, others unlimited. `setrlimit`/`prlimit64`: resource > 15 EINVAL; soft > hard EINVAL; raising hard needs `CAP_SYS_RESOURCE` (EPERM); `prlimit64` on another pid requires matching uids or the cap (EPERM), missing pid ESRCH. `RLIMIT_NOFILE` soft lowers the highest descriptor `open`/`dup`/`fcntl(F_DUPFD)`/`pipe`/`socket` may allocate (EMFILE; `dup2`/`dup3` above it EBADF; `F_DUPFD` arg ≥ limit EINVAL). `RLIMIT_NPROC` checked at fork for non-root (EAGAIN). `RLIMIT_SIGPENDING` used by Task 4 queues and Task 10 timers. `RLIMIT_CPU` soft → SIGXCPU each second past soft, SIGKILL at hard (uses process CPU ticks; checked in `FireTimers`).
- Suite (`proc/`): `rlimit_roundtrip`, `rlimit_soft_above_hard_einval`, `rlimit_raise_hard_eperm`, `rlimit_nofile_emfile`, `rlimit_inherited_by_fork_and_exec`, `prlimit_other_pid`, `rlimit_cpu_sigxcpu` (soft 1 s, busy loop, SIGXCPU handler).
- Run `make -C Source/Switch2 check-proc-suite-native check-fs-suite-native` → pass; NRO builds.

---

## Phase 3 — Full test suite and acceptance

### Task 16: Complete `ProcSuite.c` and Debian tool acceptance [Sonnet for simple groups and tooling; Opus for signal/session/epoll/inotify review]

**Files:**
- Modify: `tests/guest/ProcSuite.c`, `tools/FetchDebian.py` (add `inotify-tools`, `libinotifytools0`, `procps`, `libproc2-0`), `tools/StageDebian.py`, `Makefile`
- Create: `tests/guest/ProcessTools.sh`, `tests/guest/ProcessAcceptance.c` (wrapper like the filesystem acceptance ELF: runs suite then tools script, prints `suite N, tools N`, exits 0 when both are 42)

- [ ] **Step 1: Gap sweep.** For every `ProcessInventory.md` row, confirm at least one suite case names it (add `Cases` column). Add cases for rows without one. Every errno listed in Tasks 4–15 has a case.
- [ ] **Step 2: `ProcessTools.sh`** (bash, `set -u`, prints `ok`/`not ok` lines, exits 42 on success):

```bash
#!/bin/bash
# Debian tool acceptance for milestone 2. Usage: process-tools.sh <scratch-dir>
set -u
Fail=0
check() { if eval "$2"; then echo "ok $1"; else echo "not ok $1"; Fail=1; fi; }
Dir=${1:-/tmp/process-tools}; rm -rf "$Dir"; mkdir -p "$Dir"; cd "$Dir" || exit 1

check pipeline_three_stages '[ "$(seq 1 1000 | grep 7 | wc -l)" = 271 ]'
check pipeline_status '! (false | true; exit ${PIPESTATUS[0]})'
check subshell_and_command_subst '[ "$( (echo a; echo b) | tr -d "\n")" = ab ]'
check sleep_duration 'S=$(date +%s%N); sleep 0.3; E=$(date +%s%N); [ $(( (E-S)/1000000 )) -ge 290 ]'
check timeout_kills_sleep 'timeout 0.2 sleep 5; [ $? = 124 ]'
check timeout_signal_kill 'timeout -s KILL 0.2 sleep 5; [ $? = 137 ]'
check timeout_busy_loop 'timeout 0.3 bash -c "while :; do :; done"; [ $? = 124 ]'
check kill_builtin_term 'sleep 5 & P=$!; kill $P; wait $P; [ $? = 143 ]'
check kill_binary_list '/bin/kill -l | grep -q TERM'
check kill_binary_signal 'sleep 5 & P=$!; /bin/kill -USR1 $P; wait $P; [ $? = 138 ]'
check trap_exit '[ "$(bash -c "trap \"echo bye\" EXIT; true")" = bye ]'
check trap_signal '[ "$(bash -c "trap \"echo got\" USR1; kill -USR1 \$\$; echo after")" = "got
after" ]'
check trap_err_and_return '[ "$(bash -c "trap \"echo e\" ERR; false; true")" = e ]'
check wait_n_any 'sleep 0.2 & sleep 0.1 & wait -n; [ $? = 0 ]'
check background_job_completes '( sleep 0.1; echo done > bg.out ) & wait; [ "$(cat bg.out)" = done ]'
check job_control_script '[ "$(bash -m -c "sleep 5 & kill -STOP %1; jobs -l | grep -c Stopped; kill -CONT %1; kill %1; wait" 2>/dev/null | head -1)" = 1 ]'
check inotifywait_create '( sleep 0.2; touch watched/new ) & mkdir -p watched; [ "$(inotifywait -q -e create --format %f watched)" = new ]'
check inotifywait_timeout 'mkdir -p idle; inotifywait -q -t 1 idle; [ $? = 2 ]'
check inotifywait_monitor '( sleep 0.2; echo x > m/f; rm m/f ) & mkdir -p m; [ "$(timeout 2 inotifywait -q -m -e create,delete --format %e m | head -2 | tr "\n" " ")" = "CREATE DELETE " ]'
check ps_lists_self 'ps -o pid= -p $$ | grep -q $$'   # needs procfs; expected not ok until procfs milestone -> see below
exit $(( Fail ? 1 : 42 ))
```

  `ps` needs `/proc/<pid>/stat`, which the procfs reconciliation after milestone 3 owns. Do **not** fake it: drop the `ps_lists_self` line and record "`ps`/`pgrep` need procfs" in `ProcessInventory.md` console-pending table. If `/bin/kill` (procps) needs `/proc` for `-l` it does not; for signalling by pid it does not.

- [ ] **Step 3: Native runs.**

```bash
make -C Source/Switch2 check-proc-suite-native
make -C Source/Switch2 debian-sysroot filesystem-runtime
Source/Switch2/tools/RunPackagedNative.sh Source/Switch2/build/guest/sd/fex/rootfs /bin/process-acceptance.elf
Source/Switch2/tools/RunPackagedNative.sh --root Source/Switch2/build/guest/sd/fex/rootfs /bin/proc-suite-root.elf
```

Expected: suite `N passed, 0 failed, K skipped` (list skips with reasons) as uid 1000 and namespace root; acceptance prints `suite 42, tools 42` and exits 0; root suite exits 42. Also re-run M1: `make -C Source/Switch2 check-fs-suite-native` and the packaged filesystem acceptance (`ABI 42, coreutils 0, suite 42`).

### Task 17: Reconcile inventory and documentation [Opus]

- [ ] **Step 1:** Every `ProcessInventory.md` row is `Routed` with its cases, or listed in "Deliberate differences" with the exact errno. Group summary table counts match (`grep` count per group).
- [ ] **Step 2:** Move M2-related rows out of `FilesystemInventory.md` deliberate differences (`clock_nanosleep`, `RLIMIT_NOFILE`) with a pointer.
- [ ] **Step 3:** `README.md` Switch 2 section: one paragraph on the new suites and acceptance commands.
- [ ] **Step 4:** Restart checkpoint: status, verification commands with results, console artifact paths, next step "console run 0.13.0".

### Task 18: Console build and checklist [Opus]

- [ ] **Step 1:** Bump launcher banner to `0.13.0` in `GuestProbe.cpp`; `make -C Source/Switch2 filesystem-runtime`; `cmake --build Source/Switch2/build/guest --target guest_nro`; run all host checks:

```bash
make -C Source/Switch2 check-guest-inode-store check-guest-file-punch check-guest-file-write check-guest-memory check-guest-log check-guest-terminal check-guest-ftp check-guest-preempt check-guest-signals check-guest-job-control check-guest-credentials check-guest-clocks check-guest-timers check-guest-eventfd check-guest-epoll check-guest-inotify
```

- [ ] **Step 2: Hand off** artifacts and checklist (in the restart checkpoint, then to the user):
  1. Copy `Source/Switch2/build/guest/sd/fex` to SD `/fex` (merge) and `Source/Switch2/build/guest/fex-switch2-guest.nro` to the NRO location.
  2. Browse Linux rootfs → `/bin/process-acceptance.elf`. Expect `suite 42, tools 42`, exit 0.
  3. `/bin/proc-suite-root.elf`. Expect exit 42.
  4. `/bin/filesystem-acceptance.elf` (regression). Expect `ABI 42, coreutils 0, suite 42`.
  5. Interactive Bash (keyboard): `sleep 100` then Ctrl+C → prompt returns, `echo $?` prints `130`. `sleep 100` then Ctrl+Z → `[1]+ Stopped`; `bg` → `[1]+ sleep 100 &`; `jobs` shows Running; `fg` then Ctrl+C → prompt. `while :; do :; done` then Ctrl+C → prompt returns (preemption). `(while :; do :; done) &` then `echo hi` responds immediately, `kill %1` → `Terminated`. `cat` then Ctrl+Z, `kill -9 %1` → `Killed`. `trap 'echo caught' INT` then Ctrl+C → `caught`. `stty tostop; echo x &` → job stops with SIGTTOU.
  6. On failure copy `/fex/fex-switch2-guest.log`.
  Skip persistence/recovery pairs (no inode-store changes) unless Tasks touched `GuestInodeStore.h` or journal code.
- [ ] **Step 3:** After the user reports results: record them in the checkpoint; fix failures (systematic-debugging skill), bump patch version per console run; when all pass, commit once on `nx2`:

```bash
git add Source/Switch2 FEXCore/include/FEXCore/Core/Context.h FEXCore/Source/Interface/Core/Core.cpp FEXCore/Source/Interface/Context/Context.h
git commit -m "Implement process, signal and event syscalls on Switch 2"
```

(Check `git status --short` first; never add `build/` outputs.)

---

## Dependency order

```
1 → 2 → 3 (gate) → 4 → 5 → 6 → 7
                   4 → 12
              3 → 9 → 10
                   8 (after 4: kill permission uses Credentials)
              13 (after 10, 11, 12 for sources) → 14 → 15 → 16 → 17 → 18
              11 (after 2, independent of 4)
```

Parallel-safe Sonnet work while Opus does 3–7: Task 11 (eventfd header + test), Task 9 Step 1 (clock header + test), Task 8 Step 1 (credentials header + test). Runtime glue of those tasks waits for the Opus tasks touching the same `GuestRuntime.cpp` regions to land, to avoid conflicting edits.
