# Guest-to-host multithreading (plan, 2026-11-26)

Status: plan only, not started. Replaces the sketch in [Milestone5Plan.md](Milestone5Plan.md) Task 0. Discuss with the user
before starting; recommended order: after UI Task 8 (ptys and tabs), before the 32-bit work of Milestone 5.

## Today

- `GuestSyscalls::RunThreads` (`runtime/GuestRuntime.cpp`) runs every guest thread of every guest process on **one host
  thread** (the launcher's 4 MiB guest worker). `Dispatch` calls `Context.ExecuteThread`; a syscall leaves the JIT through
  `longjmp(ReturnToHost)` into the scheduler. A watchdog thread (`runtime/process/GuestPreempt.h`, 10 ms quantum) sets the
  FEXCore poll word so CPU-bound code returns to the scheduler.
- A blocking syscall does not block: it records a wait (`WaitKind`: Futex, IO, Terminal, Lock, Sleep, Signal, Vfork, Net),
  the scheduler runs other threads, and the syscall runs again from its rip when the wait may be over (`SeccompRetryRip`).
- All runtime state is reached through members of the one `GuestSyscalls` object, mainly `Current` (the running thread slot)
  and `Task` (its process). Nothing is locked: only one guest thread runs at a time.
- Effect: a multithreaded guest uses one of the console's cores; every guest of every tab shares that core; x86 memory
  ordering holds trivially, so FEX's TSO emulation is off.

## Goal

Each guest thread (each process's main thread and every `clone`) runs its FEX core on its own host thread, in parallel on
the console's cores, with Linux semantics unchanged and every existing suite still passing.

## Design

1. **One runtime lock, the JIT outside it.** Guest code runs without any lock. A syscall (and every other entry into the
   runtime: preemption stop, signal delivery, exit) takes one runtime mutex for the handler and drops it before returning to
   guest code. The lock is never held across guest code or a blocking wait. This keeps the 17k lines of handlers correct
   unchanged (they still see one consistent state) while guest compute runs in parallel. Hot paths are split out later only
   where measurements show contention (candidates: futex, pipe and socket I/O).
2. **`Current`/`Task` per host thread.** They become `thread_local` (or a per-thread context passed down), set when the
   thread takes the runtime lock. `setjmp`/`longjmp(ReturnToHost)` stays, per host thread.
3. **Blocking keeps the retry model.** A syscall that must wait records its wait as today, then releases the runtime lock
   and blocks its own host thread on a per-thread wake object (Horizon event or condition variable) until a waker signals it
   (futex wake, pipe/socket/pty data or space, child exit, timer deadline, signal, terminal input), then retakes the lock and
   re-runs the syscall from its rip as today. No handler has to be rewritten into a blocking form.
4. **Wakers.** Every place that today makes a waiting thread runnable signals that thread's wake object instead.
   Host sockets: a network helper thread polls the host fds of waiting threads (`HostNet`) and wakes them (today the
   scheduler polls). Timers and deadlines: the wait uses a timeout (`svcWaitSynchronization`/condvar timed wait).
5. **Signals.** A thread running guest code is stopped through its FEXCore poll word (the watchdog's mechanism, per
   thread); a blocked thread through its wake object; delivery then happens in the runtime under the lock, as today.
6. **Stop-the-world for whole-process changes.** `exit_group`, `execve`, fatal signals, `fork` from a multithreaded process
   (copies only the caller), `munmap`/`mremap`/`mprotect` of code another thread may execute, and JIT code invalidation
   (self-modifying code, `InvalidateExecutableStores`) stop the process's other threads at safe points (poll word +
   acknowledgement) before changing memory or the code cache; FEXCore's `CodeInvalidationMutex` is used as FEX does on Linux.
7. **Memory model.** x86 TSO on ARM: FEX's TSO emulation (`TSOEnabled`, with `VectorTSOEnabled`, `MemcpySetTSOEnabled`,
   `HalfBarrierTSOEnabled`; the A78C has LRCPC) is enabled when a process gets its second thread, the way upstream FEX does
   it (mark memory shared, clear the code cache); single-threaded processes keep the fast path.
8. **Scheduler calls become real.** `sched_setaffinity` sets the host core mask (`svcSetThreadCoreMask`, within the cores
   the process may use); `getcpu` reports `svcGetCurrentProcessorNumber`; `sched_yield` yields the host thread; nice and
   `sched_setscheduler` map to host priorities in the allowed range (documented mapping). Inventory rows that say
   "cooperative" or "one host thread" are updated.
9. **The rest of the launcher.** The UI thread never touches runtime state except through the locked pty/session queues of
   UI Task 8b. The FTP service's own `GuestSyscalls` instance (`GuestFileService`) shares the SD inode store with the guest
   runtime: it must take the same runtime lock (or stay suspended while guests run, as today; decide in Task 1).
10. **Diagnosis switch.** A sidecar file (`<program>.single-thread`, per AGENTS.md: test behavior in sidecars, not build
    options) runs a launch with all guest threads on one host thread under the same lock, to compare behavior.

## Tasks (milestone process: sequential Sonnet implementers, one Sonnet review, then the orchestrator fixes and runs the console)

### Task 0: feasibility probe (EXPERIMENT NRO, report before anything else)

Answer on the console, with numbers, in `docs/MultithreadingSdk.md`; stop and report if a blocker shows:
- How many host threads the launcher may create (Horizon thread resource limit for an applet and for an application
  launch), host stack size each needs for FEXCore execution, TLS cost, creation time.
- Several `ExecuteThread` calls of one `FEXCore::Context` running in parallel on different cores (the JIT's block cache,
  `CodeInvalidationMutex`, allocator, `InternalThreadState` are per thread / locked as FEX expects).
- Stopping a thread running guest code via its poll word from another core, and latency.
- TSO cost: a memory-heavy benchmark with `TSOEnabled` off/on (and the vector/memcpy variants).
- Event wait/wake latency (`ueventSignal`/`svcWaitSynchronization` or condvar) between cores.

### Task 1: host thread per guest thread under one runtime lock

`thread_local` current thread/process; the runtime lock around every runtime entry; each guest thread on its own host
thread (created at `clone`/process start, joined at exit); blocked syscalls wait on their wake object (design 3) with every
existing waker converted (futex, pipes, Unix sockets, eventfd, signalfd, timerfd, inotify, mqueue, SysV IPC, wait4/waitid,
vfork, terminal/pty, timers); the network helper thread; the single-thread sidecar. All existing suites pass in both modes.

### Task 2: signals, exit, exec, fork and memory changes across threads

Poll-word interruption per thread; stop-the-world for exit_group/execve/fatal signals/fork/munmap/mprotect/code
invalidation; `pthread_cancel`; robust futex lists at thread death; `CLONE_CHILD_CLEARTID` wake; core-wide timers
(`ITIMER_PROF`/`SIGPROF` per thread CPU time where host thread times allow).

### Task 3: memory model, scheduler calls, contention

TSO switch-on at the second thread (design 7); affinity/getcpu/yield/priority (design 8); measure lock contention with the
suites and split the hottest paths if needed; `/proc/<pid>/task/<tid>/stat` and `/proc/stat` CPU times from host thread
times.

### Task 4: tests

`tests/guest/ThreadSuite.c` (native Linux first, then console): N spinning threads finish in about 1/N of one thread's time
up to the core count; futex ping-pong; mutex/condvar stress with TSO-sensitive message passing (fails with TSO off);
signal to a thread blocked in futex, read, nanosleep and poll; exit_group with threads blocked and running; fork and execve
from a threaded process; pthread_cancel; affinity round trip and enforcement (`getcpu` stays in the mask). Plus a real
workload timing: `xz -T4`, `sort --parallel=4`, a parallel `make -j4` in the RootFS. Process, comm, filesystem, container
and pty suites pass again, also with the single-thread sidecar.

## Done when

- Console: ThreadSuite passes; the parallel benchmarks scale with cores (numbers recorded); all acceptance programs pass;
  several shell tabs run CPU-bound programs at the same time without stalling each other or the UI.
- Inventories updated (`ProcessInventory.md` "Process model" row and every "cooperative"/"one host thread" remark).

## Risks

- FEXCore on this port may assume one executing thread somewhere (allocator, code cache, the Switch-specific code buffer):
  Task 0 must show parallel execution works before Task 1.
- No fault catching on the console (Milestone3Sdk.md): self-modifying code and memory changes cannot rely on mprotect
  faults; design 6 (stop-the-world at safe points) is the replacement and costs latency.
- One runtime lock can serialize syscall-heavy workloads; parallel compute still scales.
