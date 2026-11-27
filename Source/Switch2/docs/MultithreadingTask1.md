# Multithreading Task 1: a host thread per guest thread (implementation plan, 2026-11-27)

Parent plan: [MultithreadingPlan.md](MultithreadingPlan.md) (design, decisions, later tasks). Probe numbers:
[MultithreadingSdk.md](MultithreadingSdk.md). Everything here is in `runtime/GuestRuntime.cpp` unless a file is named.

Milestone process: Task 1A, then Task 1B, each by one Sonnet implementer, one after the other; then one Sonnet review of
both; then the orchestrator fixes findings, writes `tests/guest/ThreadSuite.c` and runs the console. The runtime cannot run
on the PC: the implementers verify by building (`cmake --build build/guest --target guest_nro`, 0 warnings) and by the host
checks (`make check-guest-<name>` for every header they touch, `make check-ui`). Do not commit; the orchestrator commits
after console acceptance.

## How the runtime works today (read before changing anything)

- `GuestSyscalls::RunThreads` is the scheduler. It runs on one host thread (`RunGuestServiceThread` for the launcher's
  tabs; `RunGuest` for the RootFS installer's dpkg run). It loops: `HostPoll` (launcher sessions), terminal input,
  `FireTimers`, `DrainClosedFiles`, `PollNet(0)`; then for every slot of `Threads` (32 `GuestThread`s): skip empty, destroy
  exited, skip `!Ready(Thread)`, `ActivateProcess(Thread.PID)` (sets `Task`), sets `Current = &Thread`, delivers signals,
  `Dispatch` (`setjmp(ReturnToHost)` + `Context.ExecuteThread`), then fork completion, process retirement, exec install.
- A syscall enters `HandleSyscall(Frame)` from generated code on the same host thread. It runs the handler and either
  returns to generated code, or leaves the JIT with `std::longjmp(ReturnToHost, 1)`: when the call must wait
  (`RetrySyscall`; the wait is recorded in `Current->Waiting` and the call reruns later from its rip), on exit, exec, fork,
  signal delivery, or when another thread of the process is ready.
- `Ready(Thread)` decides whether a waiting thread may run again. It looks at shared state only (futex wake sets
  `Waiting = None`, I/O waits compare `WaitAddress` with the global `IOEpoch`, deadlines compare with `MonotonicNs()`).
  It may finish a wait (side effects on the thread's registers and wait fields) and is called repeatedly.
- `Current` (running slot) and `Task` (its process) are plain members that all ~17k lines of handlers use.
- Processes made by `fork` share guest addresses until `exec` (`GuestProcess::ForkView` is shared). `ActivateProcess`
  swaps their private pages (`SavePrivatePages`/`RestorePrivatePages`) when `Task` changes inside such a group.
  Processes with an unshared `ForkView` live in separate address ranges of the guest arena and need no swap.
- A watchdog thread (`PreemptWatchdog`, global `PreemptControl Preempt`, `runtime/process/GuestPreempt.h`) sets the
  running thread's FEXCore poll word (`InterruptPollRequestOffset`) after a 10 ms quantum; generated code then returns
  from `ExecuteThread` and `ResumeAfterPreemption` rebuilds the guest rip from `InterruptPollTargetOffset`.
- The FTP server's `GuestFileService` owns a second `GuestSyscalls`; the launcher keeps FTP suspended while guest tabs
  run (user decision), so it never runs at the same time as guest threads. File-scope globals (`ClosedFiles`,
  `ClosedPtys`, `Preempt`, `TraceMappings`) are therefore only touched by one runtime at a time.

## Target model (Task 1B)

- **One runtime lock** (`RuntimeLock`, a libnx `Mutex`, non-recursive). Every read or write of runtime state happens
  with it held. Generated guest code runs **without** it. Handlers stay unchanged: they still see one consistent state.
- **Enter / leave.** A host thread that takes the lock for a guest thread calls `Enter(Slot)`: `Current = &Slot`,
  `Task = &Processes.at(Slot.PID)`, plus the memory view check (below). Before it releases the lock it calls
  `AfterRuntimeEntry()` (wakes and stop requests, below) and sets `Current = nullptr`. `Task` and `Current` are only valid
  while the lock is held; no code keeps them across an unlock.
- **A host thread per guest thread.** `AddThread` creates a libnx thread for the slot (stack `GuestHostStackSize` =
  4 MiB, priority `GuestThreadPriority` = 0x2d, one below the launcher UI's 0x2c; created on cpuid -2, then
  `svcSetThreadCoreMask(handle, -1, ProcessCoreMask)` before `threadStart`, with the process core mask from
  `svcGetInfo(InfoType_CoreMask)`; mtprobe 2 showed that this spreads threads over idle cores). The thread runs
  `GuestThreadMain(Slot)`.
- **The service thread** is the thread that calls `RunThreads` today. It no longer dispatches guest code. Its loop, under
  the lock: `HostPoll`, `PollTerminalInput` (interactive only), `FireTimers`, `DrainClosedFiles`, `PollNet(0)` when
  `NetWaiting()`, join finished guest host threads, `AfterRuntimeEntry()`; then it releases the lock and waits on
  `ServiceWake` until the next timer deadline, at most 8 ms (1 ms while `NetWaiting()`, so host sockets are polled at
  least every millisecond). It ends as today (`ProcessExited`, `HostStop`, no guest threads left outside service mode),
  then stops every guest host thread (below) before the cleanup that follows the loop today.
- **Waiting.** A guest host thread whose slot is not `Ready` blocks on its own wake word: it reads `Slot.WakeWord`,
  releases the lock, calls `svcWaitForAddress(&Slot.WakeWord, ArbitrationType_WaitIfEqual, Seen, Timeout)` with
  `Timeout` = time to `Slot.Deadline` (or -1 without one; 10 ms for a CPU-clock sleep, `CpuDeadline != 0`), then retakes
  the lock and checks `Ready` again. The address arbiter was the fastest wake in mtprobe 1 (0.45 us).
- **Waking.** `AfterRuntimeEntry()` runs before every unlock. For each slot with a host thread that is blocked
  (`HostState == Blocked`) and `Ready(Slot)` → `WakeSlot(Slot)`: `++WakeWord` (atomic), then
  `svcSignalToAddress(&WakeWord, SignalType_Signal, 0, 1)`. This is what `RefreshPreemptHints` does today (it calls
  `Ready` for every thread after every syscall), so no waker in the 17k lines of handlers needs to change.
- **Stop requests.** In the same pass, for each slot in guest code (`HostState == InGuest`) whose `NeedsStop(Slot)` is
  true, set its poll word (`InterruptPollRequestOffset` = 1). `NeedsStop`: the slot `Exited`; `HasActionableSignal`;
  its process `Stopped`, `Exited` or `PendingExec`; or a stop request on its process (`StopRequests > 0`, below).
- **Leaving guest code.** `HandleSyscall` takes the lock and calls `Enter` for the calling slot (found through a
  `thread_local GuestThread* HostSlot` the host thread set at start). At the end, where the handler today returns to
  generated code, it first checks `NeedsStop(*Current)`: true → `longjmp` to the host loop (the loop delivers signals,
  exits, stops); false → `AfterRuntimeEntry()`, `HostState = InGuest`, unlock, return. Every
  `std::longjmp(ReturnToHost, 1)` becomes `ReturnToHostLoop()`, which longjmps to `*HostReturn`, a `thread_local
  std::jmp_buf*` each host thread points at its own buffer; the lock stays held across the jump. The "another thread of
  the process is ready" jump at the end of `HandleSyscall` and `YieldRequested` go away: other threads run on their own
  host threads. `sched_yield` (today `YieldRequested`) becomes `svcSleepThread(-1)` after the unlock.
- **Preemption.** With more guest threads than cores, Horizon does not time-slice threads of one priority, so the
  watchdog stays: every `GuestPreemptCheckNs` = 5 ms it takes `Preempt.Lock` and sets the poll word of every slot that
  has been `InGuest` for at least `PreemptQuantumNs` (10 ms) since `Slot.GuestSince` and is not yet armed
  (`Slot.StopArmed`). A host thread whose `ExecuteThread` returned through a poll calls `ResumeAfterPreemption` (as
  today) and then yields with core migration (`svcSleepThread(-1)`) before it runs guest code again. The watchdog thread
  gets the all-cores mask too. `ShouldPreempt`'s other inputs (`OtherRunnable`, `TimerDue`, `TerminalInput`) are no longer
  needed; `GuestPreempt.h` and its test (`check-guest-preempt`) change to the per-thread rule.
- **Memory views (fork groups).** `ForkView` becomes `std::shared_ptr<MemoryView>` with `struct MemoryView { uint32_t
  Resident {}; };` (`Resident` = the PID whose private pages are in memory). `ActivateProcess(PID)` swaps only when
  `View->Resident != PID` and the resident process is live and still in the group; it then saves the resident process's
  pages, restores the new one's, suspends/unsuspends pipe pages as today, and sets `Resident = PID`. A swap needs every
  thread of the resident process out of guest code: `Enter` for a slot whose process is not resident calls
  `StopProcess(Resident)` (below) first. While a group holds more than one live process its processes therefore run one
  at a time; after `exec` a process has its own view and runs in parallel with the others.
- **Stopping a process's other threads** (`StopProcess(PID, Except)`): `++Process.StopRequests`; loop { set the poll word
  of every slot of `PID` other than `Except` that is `InGuest`; if none is `InGuest`, break; `Current`/`Task` saved,
  unlock, wait on `QuiesceWord` (arbiter, 1 ms timeout), lock, restore }; `--StopRequests` when the caller is done (a
  scoped helper). A host thread that returns from guest code to the lock signals `QuiesceWord` (all waiters). Used
  before `InstallExec` (the exec'ing thread stops its siblings, then their slots are destroyed by their own host threads,
  below), before `RetireProcess` (exit_group and fatal signals: memory is torn down), and for view swaps.
- **Slot ownership.** A slot's FEXCore thread (`Core`) is created by `AddThread` and destroyed only by the slot's own host
  thread, at the end of `GuestThreadMain`, under the lock (`DestroyThread`; then `HostState = Finished`, signal
  `ServiceWake`). Every place that destroys a `Core` or clears a slot today (`RunThreads`, `RetireProcess`, `InstallExec`,
  the `SpawnSession` and `CompleteFork` failure paths) instead marks the slot `Exited` and, for a slot that has no host
  thread yet, destroys it directly. The service thread joins `Finished` threads (`threadWaitForExit`, `threadClose`) and
  only then frees the slot (`Thread = {}`). `AddThread` takes only slots with no `Core` and `HostState == None`.
  `InstallExec` keeps the exec'ing slot's host thread: the new image's main thread reuses that host thread (the slot gets
  the new `Core`), the other slots of the process end.
- **CPU time.** `CpuTicks` of a slot add up as today, per host thread (`CurrentThreadTicks` is per host thread already).
  `QuantumStart`/`InDispatch` become per slot (`DispatchStartTicks`, `InGuest`); clocks that read a running thread's time
  (`ProcessCpuNs`, thread CPU clocks) add `svcGetInfo(InfoType_ThreadTickCount, Slot.HostHandle, -1) -
  DispatchStartTicks` for slots in guest code.
- **Single-thread switch** (design 10). A sidecar `<program>.single-thread` (any content) next to a launched program sets
  `SessionLaunch::SingleThread`; the process gets `GuestProcess::SerialGuest`, inherited by `fork`/`clone` children and
  kept across `exec`. In a `SerialGuest` process a slot may enter guest code only while no other slot of the same process
  is `InGuest` (otherwise it blocks as if not ready, and is woken when a sibling leaves guest code). This runs the
  process's threads one at a time on their host threads, for comparing behavior.
- **TSO** stays as today (FEX's `TSOEnabled` default, on for every process); Task 3 turns it off for single-threaded
  processes.

## Task 1A: preparation, no behavior change (Sonnet)

Mechanical changes that keep one host thread and today's behavior, so Task 1B only changes the scheduling.

1. **Jump buffer per host thread.** Replace the member `std::jmp_buf ReturnToHost` with a file-scope
   `thread_local std::jmp_buf* HostReturn {};` and `[[noreturn]] void ReturnToHostLoop()` (`std::longjmp(*HostReturn,
   1)`). `Dispatch` declares its own `std::jmp_buf` and points `HostReturn` at it around `ExecuteThread` (restore the
   previous value after). Replace every `std::longjmp(ReturnToHost, 1)` (8 sites: `grep -n 'longjmp(ReturnToHost'`).
2. **Runtime lock object.** Add `Mutex RuntimeLock {};` and `void LockRuntime()`/`void UnlockRuntime()` members (thin
   `mutexLock`/`mutexUnlock` wrappers). In 1A, `RunThreads` takes it once at its start and releases it at its end. Nothing else
   changes (the FTP `GuestFileService` is a separate instance and never runs together with guest tabs).
3. **Slot fields** (in `GuestThread`, unused in 1A except where stated): `Thread Host {}` (libnx), `bool HostCreated {}`,
   `enum class HostRun : uint8_t { None, Starting, Ready, Blocked, InGuest, Finished } HostState {}`,
   `std::atomic<int32_t> WakeWord {}` (make `GuestThread` movable where it is assigned `= {}`: replace those assignments
   by a `ResetSlot(GuestThread&)` helper that destroys and re-constructs in place, since `std::atomic` is not
   copyable), `uint64_t GuestSince {}`, `uint64_t DispatchStartTicks {}`, `std::atomic<bool> StopArmed {}`.
4. **Memory views.** `struct MemoryView { uint32_t Resident {}; };` and `ForkView` as `std::shared_ptr<MemoryView>`
   (5 uses: `grep -n ForkView`). Split `ActivateProcess` into the view swap (only when the view's `Resident` differs and
   the group has more than one live process: compare with `Resident` instead of `Task->PID`; move
   `SuspendPipePages`/unsuspend into the swap) and the `Task` assignment. Behavior for one host thread is unchanged.
   Set `Resident` where a process first runs (spawn, fork child, exec) and when `exec` gives the process a fresh view.
5. **Per-slot preemption fields.** `OpenPreemptWindow`/`ClosePreemptWindow` also set `Current->GuestSince` and
   `Current->DispatchStartTicks` and `Current->StopArmed`; `QuantumStart`/`InDispatch` readers (grep them) read the slot
   fields of the running slot instead. The watchdog keeps using `Preempt.Running` in 1A.
6. **Single-thread sidecar plumbing.** `launcher/LauncherBackend.cpp` `ReadSidecars`: `<program>.single-thread` exists →
   `P.SingleThread = true` (add `bool SingleThread {}` to `SessionLaunch` in `runtime/GuestService.h`). `SpawnSession`
   copies it to `GuestProcess::SerialGuest`; fork/clone children copy their parent's; exec keeps it. Log it once per
   session ("Guest session N: single-thread mode"). No scheduling effect in 1A.
7. **Constants.** `GuestHostStackSize = 4 MiB`, `GuestThreadPriority = 0x2d`, `GuestPreemptCheckNs = 5 ms` near
   `MainStackSize`.

Done when: `guest_nro` builds with 0 warnings; `make check-ui` and all `check-guest-*` pass; a diff review shows no
behavior change (the orchestrator runs the console suites after 1B, not after 1A).

## Task 1B: host threads (Sonnet)

Implement "Target model" above on top of 1A, in this order:

1. `GuestThreadMain(GuestThread& Slot)` (static trampoline + member): set `HostSlot`, own `std::jmp_buf` for `HostReturn`;
   `LockRuntime()`; loop {
   `Enter(Slot)` (may stop the resident process of its view, see 6);
   `Slot.Exited` or `ProcessExited`/`HostStop` → leave the loop;
   not `Ready(Slot)`, or `Task->Stopped`, or a `SerialGuest` sibling `InGuest` → block on `WakeWord` (Waiting above), continue;
   the body of today's `RunThreads` per-thread block (SetTidOnStart, first-dispatch trace, signal delivery,
   `Dispatch`, CPU ticks, startup checkpoints, `PendingFork` → `CompleteFork`, process exit → `StopProcess` +
   `RetireProcess`, `PendingExec` → `StopProcess` + `InstallExec`) }; at the end `DestroyThread(Slot.Core)`,
   `HostState = Finished`, `AfterRuntimeEntry()`, signal `ServiceWake`, unlock, return.
2. `Dispatch`: `HostState = InGuest`, `GuestSince`, `DispatchStartTicks`, `StopArmed = false`, `Current = nullptr`,
   unlock; `ExecuteThread`; lock; `Enter(Slot)`; `HostState = Ready`; signal `QuiesceWord`; then
   `ResumeAfterPreemption` and the yield. On the longjmp path the lock is already held and `Current` is set by
   `HandleSyscall`.
3. `HandleSyscall`: lock + `Enter` at the top (the slot from `HostSlot`, `HostState = Ready`, signal `QuiesceWord`); the
   normal return path as described in "Leaving guest code"; drop the ready-sibling longjmp and `RefreshPreemptHints`.
   `PreCompile`, `QueryGuestExecutableRange` and `LookupExecutableFileSection` are called from the JIT without the lock:
   `QueryGuestExecutableRange` takes the lock and reads the memory map of the process that owns the calling FEXCore
   thread (find the slot by `Core`), not `Task`.
4. `AddThread`: after `CreateThread`, create and start the host thread (constants from 1A); on failure undo the slot and
   return false (callers already handle false). `HostState = Starting` until the thread takes the lock.
5. `RunThreads` becomes the service loop described above; on exit it sets `HostStop`, marks every slot `Exited`, runs
   `AfterRuntimeEntry()` (stop + wake everyone), and joins every host thread (unlocking while it waits on `ServiceWake`)
   before today's cleanup.
6. `Enter`, `AfterRuntimeEntry`, `NeedsStop`, `WakeSlot`, `StopProcess` (+ scoped release), memory-view stop before a
   swap, `QuiesceWord`, `ServiceWake`, per-slot preemption in `PreemptWatchdog` and `GuestPreempt.h` (update
   `tests/unit/GuestPreemptTest.cpp`), CPU-time reads of running slots, `SerialGuest`.
7. Replace slot destruction everywhere per "Slot ownership".
8. `sched_yield`: unlock-time yield instead of `YieldRequested`.
9. `docs/ProcessInventory.md` "Process model" row and every "cooperative" / "one host thread" remark
   (`grep -rn 'cooperative\|one host thread' docs`): describe the new model; note `SerialGuest` and fork-group
   exclusivity.

Done when: `guest_nro` builds with 0 warnings; all host checks pass. The orchestrator then reviews, writes ThreadSuite and
runs the console (all suites, also with `.single-thread`, and CPU-bound threads scaling across cores).

## Risks for the implementers to keep in mind

- Never hold `Current`/`Task` or any reference into `Processes`/`Threads` state across an unlock, except the host
  thread's own slot reference (slots never move; they are freed only after their host thread was joined).
- Never call `DestroyThread` or free process memory while a slot of that process may be `InGuest`.
- Nothing may block with the lock held except short host calls that exist today (SD I/O, `PollNet(0)`).
- The lock is not recursive: code reached from a handler must not take it again (only the outermost entries do:
  `GuestThreadMain`, `Dispatch` after `ExecuteThread`, `HandleSyscall`, `QueryGuestExecutableRange`, the service loop).
