# Milestone 2 process and event syscall inventory

Numbers are Linux x86-64 ABI numbers. A routed syscall has a Switch 2
dispatcher handler: a `case` in the `switch (Registers[REG_RAX])` block of
`GuestSyscalls::HandleSyscall` in `GuestRuntime.cpp`, or the exit path
that runs before it. `Partial` means routed with known gaps named in
Notes. `Unrouted` returns ENOSYS. The historical audit counted 85
entries; this inventory adds the remaining x86-64 entries of these groups
so none is left untracked. It has 109 entries. Semantics and console
acceptance are tracked in [NextMilestone.md](../NextMilestone.md).

Started as the baseline before milestone 2 work; Tasks 3-15 updated the
Dispatch and Notes columns as they landed.

## Console verification still required

| Interface | Pending console evidence |
| --- | --- |
| Tasks 3-9 (preemption, signals, lifecycle, sessions, restart table, credentials, clocks) | Verified on console 0.13.13-diag: proc-suite 138/0/7 (uid 1000), proc-suite-root 143/0/2 |
| Timers, eventfd, signalfd, poll/select, epoll, inotify, limits (Tasks 10-15) | Verified on console 0.13.16/0.13.17-diag: process acceptance `suite 42, tools 42` (proc-suite 240/0/8), proc-suite-root 246/0/2 |
| Launcher tabs | Verify simultaneous user, root and developer sessions and process-group signals on their controlling ptys. See the Task 8b checkpoint. |
| `ps`, `top`, `pgrep`, `free`, `uptime` (0.17.13) | The files they read exist (see [Procfs files for procps](#procfs-files-for-procps)); the host suite checks the formats natively and the proc suite `procfs/` cases pass on Linux, but only the console shows that the runtime serves them: `top -bn1`, `ps aux`, `free`, `uptime`, `pgrep` |

## Procfs files for procps

The formats are in `runtime/process/GuestProcfs.h` (unit test `make check-guest-procfs`, field layout of Linux 5.15) and the
values come from the runtime's process table. Suite: `procfs/` cases of `tests/guest/ProcSuite.c` (native Linux and console).
The cause of `top`'s "Error, do this: mount -t proc proc /proc" (0.17.12) was `openat("/proc/self/stat")` returning ENOENT.

| File | Content |
| --- | --- |
| `/proc/<pid>/stat`, `/proc/<pid>/task/<tid>/stat` | 52 fields. pid, comm, state (R running or runnable, S sleeping, T stopped, Z zombie), ppid, pgrp, session, tty_nr (136:index of a pty), tpgid, utime (all CPU time: no user/system split), cutime, priority and nice, num_threads, starttime (clock ticks since console boot), vsize and rss (mapped and committed pages), signal masks (blocked, ignored, caught), exit_signal, policy and rt_priority. Not tracked (0): minflt/majflt, kstkesp/eip, wchan, guest_time, code, data and stack addresses, arg/env ranges. |
| `/proc/<pid>/statm` | size, resident from the address space; shared, text, data are 0 |
| `/proc/<pid>/status` | as before plus VmPeak, VmSize and VmRSS |
| `/proc/<pid>/cmdline`, `comm`, `cgroup` | the argv of the running image (empty for a zombie), the 15 character name, `0::/` |
| `/proc/<pid>/task`, `task/<tid>/{stat,statm,status,cmdline,comm,cgroup}` | the live threads of the process (the per-thread `status`, `statm` and `cmdline` are the process's) |
| `/proc/stat` | `cpu` and `cpuN` lines (busy ticks all counted as user time, split evenly over the CPUs; idle = uptime x CPUs - busy), `ctxt` (scheduler switches), `btime`, `processes`, `procs_running`, `procs_blocked`. No `intr`/`softirq` counts. |
| `/proc/uptime`, `/proc/loadavg`, `/proc/meminfo`, `/proc/cpuinfo` | uptime since console boot; load averages damped every 5 s from the runnable count sampled when the file is read (the kernel samples on every tick, so a long idle gap reads as that many samples of the current count); MemTotal and MemFree from the guest arena's commit limit and use, swap 0; cpuinfo is a generic x86-64 description |
| `/proc/sys/kernel/pid_max` | 32768 |

Not provided: `/proc/<pid>/environ`, `maps`, `smaps`, `io`, `sched`, `oom_score`, `wchan`, `/proc/vmstat`, `/proc/diskstats`,
`/proc/<pid>/fdinfo`.

## Deliberate differences from a stock kernel

| Interface | Behavior |
| --- | --- |
| `RLIMIT_NOFILE` | The hard limit cannot exceed 1024, the descriptor table size (Linux's `fs.nr_open` is 1048576): raising it above returns EPERM, also for root |
| Other resource limits | Stored, reported and inherited, but only NOFILE, NPROC, SIGPENDING and CPU are enforced. FSIZE (no SIGXFSZ/EFBIG), DATA, AS, RSS, MEMLOCK, LOCKS, MSGQUEUE and RTTIME never fail a call; STACK does not resize the stack of a later exec (always 1 MiB); CORE has no effect (no core files). A `setuid` past RLIMIT_NPROC does not make the next exec fail with EAGAIN |
| `/proc/<pid>/fd` | Directory and links exist (milestone 4); the rest of `/proc` is only what [Procfs files for procps](#procfs-files-for-procps) lists |
| `fanotify_init`, `fanotify_mark` | ENOSYS (-38). Linux returns EPERM without CAP_SYS_ADMIN; fanotify is not implemented |
| `eventfd` read with a bad buffer | EFAULT before the counter is consumed; Linux consumes the counter, then fails the copy |
| Edge-triggered epoll | An edge is a change of the item's readiness generation: per-socket and pipe change counters, eventfd/timerfd/inotify queue changes, any signal for signalfd, any I/O for terminals and nested epoll. Linux reports an edge per wakeup callback, so a write that does not change readiness can produce an extra event there (or here for terminals) |
| inotify sources | Events come from guest syscalls only. Changes by the FTP server, the rootfs setup or another program on the console do not notify; IN_UNMOUNT never occurs. Limits are fixed: 128 instances, 8192 watches per instance, 16384 queued events (`fs.inotify.*`) |
| Remaining `Partial` rows | Their Notes name the gap (wait4/getrusage/times without system time, vfork copying memory, clone sharing limits, prctl options, restart_syscall EINTR); none changed in Tasks 10-15 |
| Clocks | See [Clocks: deliberate differences](#clocks-deliberate-differences) |
| Process model | One worker runs all guest threads through `GuestSyscalls::RunThreads`. Syscalls and the timed watchdog return execution to the scheduler. Launcher tabs start separate session leaders in this worker, with a controlling pty and per-launch credentials and working directory. |
| Signal core dumps | Core-action signals terminate without a core file; WCOREDUMP is never set |
| Signal dequeue order | Lowest number first; Linux takes synchronous fault signals (SEGV, BUS, ILL, TRAP, FPE, SYS) first. No fault generates a signal yet |
| Interrupted socket timeouts | Socket waits with SO_RCVTIMEO/SO_SNDTIMEO restart under SA_RESTART; Linux returns EINTR |
| Vectored stream writes | `writev`/`pwritev2`/`sendmsg`-free vectored writes to pipes and sockets queue at most 1 MiB per call (one gathered copy per attempt); plain `write` and `send` queue every byte as Linux does |
| Orphan reaper | Without a subreaper, orphans go to a virtual init (pid 1) that reaps at once |

| Group | Routed | Partial | Unrouted |
| --- | ---: | ---: | ---: |
| Processes and sessions | 22 | 13 | 0 |
| Signals | 16 | 1 | 0 |
| Credentials and capabilities | 18 | 0 | 0 |
| Clocks and timers | 17 | 4 | 0 |
| Polling and watches | 16 | 0 | 2 |

## Interruption and restart

A caught signal ends a blocked call as Linux does. The runtime decides in
`InterruptWait` (`GuestRuntime.cpp`) from the wait kind, the syscall number
(`RestartableSyscall`) and `SA_RESTART`. A signal without a handler (stop,
continue, ignored) leaves the call blocked. Each row has a
`signal/restart_table_*` case; all pass natively.

| Call | Wait kind | With SA_RESTART | Without | Linux return |
| --- | --- | --- | --- | --- |
| `read` (pipe, socket, terminal) | IO / Terminal | Restarted | EINTR | -ERESTARTSYS |
| `write` (full pipe or socket) | IO | Restarted; after queued bytes, their count | EINTR, or the count | -ERESTARTSYS / count |
| `wait4`, `waitid` | IO | Restarted | EINTR | -ERESTARTSYS |
| `futex` FUTEX_WAIT without timeout | Futex | Restarted | EINTR | -ERESTARTSYS |
| `futex` with timeout | Futex | EINTR | EINTR | -ERESTART_RESTARTBLOCK |
| `flock`, `fcntl` F_SETLKW / F_OFD_SETLKW | Lock | Restarted | EINTR | -ERESTARTSYS |
| `accept`, `accept4`, Unix `connect` | IO | Restarted | EINTR | -ERESTARTSYS |
| `poll`, `ppoll`, `select`, `pselect6`, `epoll_wait`, `epoll_pwait(2)` | IO | EINTR | EINTR | -ERESTARTNOHAND / -EINTR |
| `nanosleep`, `clock_nanosleep` | Sleep | EINTR, relative sleeps store the time left | EINTR | -ERESTART_RESTARTBLOCK |
| `pause`, `rt_sigsuspend`, `rt_sigtimedwait` | Signal | EINTR | EINTR | -ERESTARTNOHAND / -EINTR |
| `vfork` parent | Vfork | Not interrupted (killable only) | same | TASK_KILLABLE |

A woken IO, terminal or lock wait reruns its syscall before a pending
signal is delivered (`RerunBeforeSignals`), as `pipe_read` and `do_wait`
rescan first. A stopped and continued sleep keeps its deadline, so it never
needs `restart_syscall` (219).

## Processes and sessions

| Number | Syscall | Dispatch | Notes |
| ---: | --- | --- | --- |
| 24 | `sched_yield` | Routed | Ends the thread's time slice; the scheduler picks the next ready thread. |
| 39 | `getpid` | Routed |  |
| 56 | `clone` | Partial | Threads need CLONE_VM|FS|FILES|SIGHAND|THREAD. Process clones support exit signals 0..64, CLONE_PARENT, CLONE_VFORK (with CLONE_VM), CLONE_PIDFD, CLONE_SETTLS, CLONE_PARENT_SETTID, CHILD_SETTID/CLEARTID and a new stack. Processes cannot share memory or handlers: CLONE_VM without VFORK and CLONE_SIGHAND return EINVAL. CLONE_FS, CLONE_FILES, CLONE_SYSVSEM and the namespace flags (user, UTS, IPC, PID, cgroup, mount placeholder) are supported since milestone 4 task 2; see [ContainerInventory.md](ContainerInventory.md). |
| 57 | `fork` | Routed |  |
| 58 | `vfork` | Partial | The child gets a copy of memory, not a shared one; the parent waits (killable only) until the child execs or exits, as in Linux. |
| 59 | `execve` | Routed | `#!` scripts as Linux binfmt_script: 256-byte line, one argument, four rewrites then ELOOP, setuid bits of scripts ignored. `/dev/fd/N` names given to interpreters of descriptor-relative scripts need a `/dev/fd` the runtime does not have yet. |
| 60 | `exit` | Routed | Handled before the dispatch switch. The last thread's exit ends the process with that status. |
| 61 | `wait4` | Partial | Exited, stopped (WUNTRACED) and continued (WCONTINUED) children, pid/pgid/any selectors, __WCLONE/__WALL. rusage reports only ru_utime (guest CPU time); __WNOTHREAD is accepted and has no effect (threads cannot fork separately). |
| 63 | `uname` | Routed |  |
| 97 | `getrlimit` | Routed | Per-process limits (`GuestProcess::Limits`), inherited by fork and kept by exec. Defaults: STACK 1 MiB (the loaded stack)/unlimited, CORE 0/unlimited, NPROC 32/32 (the thread table), NOFILE 1024/1024, MEMLOCK 8 MiB, SIGPENDING 4096, MSGQUEUE 819200, NICE 0, RTPRIO 0, the rest unlimited. Cases `proc/rlimit_*`. |
| 98 | `getrusage` | Partial | RUSAGE_SELF, CHILDREN and THREAD report ru_utime only; system time, faults and context switches are zero. |
| 99 | `sysinfo` | Partial | Loads and free RAM are zero; procs counts live guest threads. |
| 100 | `times` | Partial | User times in 100 Hz ticks; system times are zero. |
| 109 | `setpgid` | Routed | Linux rules: ESRCH, EPERM across sessions and for leaders, EACCES after the child's exec. |
| 110 | `getppid` | Routed |  |
| 111 | `getpgrp` | Routed |  |
| 112 | `setsid` | Routed | New session without a controlling terminal. |
| 121 | `getpgid` | Routed |  |
| 124 | `getsid` | Routed |  |
| 135 | `personality` | Partial | Value stored and reported; flags such as ADDR_NO_RANDOMIZE change nothing (no ASLR). |
| 140 | `getpriority` | Partial | Nice values stored and reported per process; PRIO_USER matches real uid. |
| 141 | `setpriority` | Partial | Linux permission rules (EPERM, EACCES with RLIMIT_NICE 0); the cooperative scheduler does not weight threads by nice. |
| 157 | `prctl` | Partial | PDEATHSIG (1/2), DUMPABLE (3/4), NAME (15/16), CHILD_SUBREAPER (36/37), NO_NEW_PRIVS (38/39), KEEPCAPS (7/8), CAPBSET_READ/DROP (23/24), CAP_AMBIENT (47). SECUREBITS (27/28) and other options return EINVAL. |
| 158 | `arch_prctl` | Routed |  |
| 160 | `setrlimit` | Routed | `do_prlimit` order: EFAULT, EINVAL (resource, soft above hard), EPERM for NOFILE above 1024 (nr_open) or a raised hard limit without CAP_SYS_RESOURCE. Enforced: NOFILE (allocation EMFILE, `dup2` EBADF, `F_DUPFD` EINVAL, `poll` EINVAL), NPROC (clone EAGAIN for non-root), SIGPENDING (queued RT signals, timers), CPU (SIGXCPU at the soft limit, which moves up a second each time; SIGKILL at the hard limit). See deliberate differences for the others. |
| 186 | `gettid` | Routed |  |
| 218 | `set_tid_address` | Routed |  |
| 231 | `exit_group` | Routed | Ends every thread; reparents children (subreaper or pid 1), sends PDEATHSIG, hangs up newly orphaned stopped groups and a led terminal, notifies the parent. |
| 247 | `waitid` | Partial | P_ALL, P_PID, P_PGID, P_PIDFD with WEXITED/WSTOPPED/WCONTINUED/WNOWAIT/WNOHANG; rusage as wait4. |
| 302 | `prlimit64` | Routed | As setrlimit; another pid or tid needs matching real/effective/saved uids and gids or CAP_SYS_RESOURCE (EPERM); missing pid ESRCH; the new value is read before the target is looked up. Case `proc/prlimit_other_pid`. |
| 309 | `getcpu` | Routed | Always CPU 0, node 0 (one host thread runs the guest). |
| 322 | `execveat` | Routed | AT_EMPTY_PATH needs a descriptor opened by path. |
| 434 | `pidfd_open` | Routed | PIDFD_NONBLOCK accepted; pidfds cannot be polled yet (Task 12). |
| 435 | `clone3` | Partial | As clone (flags per [ContainerInventory.md](ContainerInventory.md)); set_tid still returns EPERM/EINVAL (not implemented), CLONE_INTO_CGROUP EINVAL. |
| 438 | `pidfd_getfd` | Partial | Permission is same real and effective uid or CAP_SYS_PTRACE (no ptrace access model). |

## Signals

| Number | Syscall | Dispatch | Notes |
| ---: | --- | --- | --- |
| 13 | `rt_sigaction` | Routed |  |
| 14 | `rt_sigprocmask` | Routed |  |
| 15 | `rt_sigreturn` | Routed |  |
| 34 | `pause` | Routed |  |
| 62 | `kill` | Routed | Linux kill_something_info rules; a thread id names its process. Needs CAP_KILL or matching uid. |
| 127 | `rt_sigpending` | Routed | Thread and process pending sets. |
| 128 | `rt_sigtimedwait` | Routed |  |
| 129 | `rt_sigqueueinfo` | Routed | A thread id names its process. |
| 130 | `rt_sigsuspend` | Routed |  |
| 131 | `sigaltstack` | Routed |  |
| 200 | `tkill` | Routed |  |
| 219 | `restart_syscall` | Partial | Returns EINTR; interrupted timed sleeps are not restarted with the remaining time. |
| 234 | `tgkill` | Routed |  |
| 282 | `signalfd` | Routed | As signalfd4 with flags 0. |
| 289 | `signalfd4` | Routed | Size 8 (EINVAL), mask EFAULT, flags NONBLOCK/CLOEXEC (EINVAL); fd -1 creates, an existing signalfd updates its mask, another descriptor EINVAL. Reads dequeue the calling thread's and process's pending signals in the mask as `signalfd_siginfo` (layout per signal kind); SIGKILL and SIGSTOP are ignored in the mask. Poll readiness follows the polling thread. Cases `event/signalfd_*`. |
| 297 | `rt_tgsigqueueinfo` | Routed |  |
| 424 | `pidfd_send_signal` | Routed | Flags THREAD, THREAD_GROUP and PROCESS_GROUP. |

## Credentials and capabilities

| Number | Syscall | Dispatch | Notes |
| ---: | --- | --- | --- |
| 102 | `getuid` | Routed |  |
| 104 | `getgid` | Routed |  |
| 105 | `setuid` | Routed | Linux commoncap rules (`GuestCredentials.h`); capabilities follow the id changes (setxuid fixup, KEEPCAPS). |
| 106 | `setgid` | Routed | Linux commoncap rules (`GuestCredentials.h`); capabilities follow the id changes (setxuid fixup, KEEPCAPS). |
| 107 | `geteuid` | Routed |  |
| 108 | `getegid` | Routed |  |
| 113 | `setreuid` | Routed | Linux commoncap rules (`GuestCredentials.h`); capabilities follow the id changes (setxuid fixup, KEEPCAPS). |
| 114 | `setregid` | Routed | Linux commoncap rules (`GuestCredentials.h`); capabilities follow the id changes (setxuid fixup, KEEPCAPS). |
| 115 | `getgroups` | Routed |  |
| 116 | `setgroups` | Routed | Needs CAP_SETGID (EPERM). |
| 117 | `setresuid` | Routed |  |
| 118 | `getresuid` | Routed |  |
| 119 | `setresgid` | Routed |  |
| 120 | `getresgid` | Routed |  |
| 122 | `setfsuid` | Routed | Returns the old fsuid; capability fixup for fs caps. |
| 123 | `setfsgid` | Routed |  |
| 125 | `capget` | Routed | Versions 1, 2, 3; unknown version writes 0x20080522 and fails EINVAL (NULL data returns 0); pid < 0 EINVAL, missing pid ESRCH. |
| 126 | `capset` | Routed | Own pid only (EPERM otherwise); EPERM for raising permitted or inheritable beyond the Linux limits; bits above CAP_CHECKPOINT_RESTORE are dropped. |

## Clocks and timers

| Number | Syscall | Dispatch | Notes |
| ---: | --- | --- | --- |
| 35 | `nanosleep` | Routed |  |
| 36 | `getitimer` | Routed | ITIMER_REAL on the monotonic clock, VIRTUAL and PROF on process CPU time (no user/system split). Cases `timer/*itimer*`. |
| 37 | `alarm` | Routed | ITIMER_REAL; returns the seconds left rounded up. Kept across exec, not inherited by fork. Cases `timer/alarm_*`. |
| 38 | `setitimer` | Routed | EINVAL for `which` > 2 or `tv_usec` outside [0, 1e6); NULL new value disarms. Signals use SI_KERNEL. |
| 96 | `gettimeofday` | Routed | Writes the guest realtime clock and the stored timezone (`{0, 0}` until `settimeofday` stores one); NULL `tv` or `tz` allowed. Faults after writing `tv` leave it written, as Linux does. |
| 159 | `adjtimex` | Partial | See [Clocks: deliberate differences](#clocks-deliberate-differences): fields are stored and read back, the clock is never slewed. |
| 164 | `settimeofday` | Partial | Sets the guest realtime clock (CAP_SYS_TIME, EPERM). Errors in Linux order: EFAULT, EINVAL for `tv_usec >= 1000000` or negative seconds, EPERM, EINVAL for a timezone beyond +-15 h. The first-call clock warp by the timezone is not modelled. |
| 201 | `time` | Routed | Seconds of the guest realtime clock; EFAULT for a bad pointer. |
| 222 | `timer_create` | Routed | Ids start at 0 per process and are never reused (an id is consumed even when a later check fails). SIGEV_SIGNAL, SIGEV_NONE, SIGEV_THREAD (as SIGEV_SIGNAL), SIGEV_THREAD_ID for an own thread; MONOTONIC_RAW and coarse clocks EOPNOTSUPP; alarm clocks EPERM without CAP_WAKE_ALARM; CPU clocks of the process. Default sigevent: SIGALRM with the id as value. |
| 223 | `timer_settime` | Routed | Relative or TIMER_ABSTIME; re-arming makes a queued signal of the previous arming stale (dropped at dequeue). |
| 224 | `timer_gettime` | Routed |  |
| 225 | `timer_getoverrun` | Routed | Overrun of the last delivered signal. |
| 226 | `timer_delete` | Routed | A queued signal stays pending and is dropped at dequeue, as Linux does. Exec deletes every timer. |
| 227 | `clock_settime` | Routed | Only `CLOCK_REALTIME` is settable: other ids, including `CLOCK_PROCESS_CPUTIME_ID`, EINVAL; dynamic CPU clocks EPERM (EINVAL when the process is missing). Request errors (EFAULT, EINVAL) come before the CAP_SYS_TIME check (EPERM). Re-arms absolute REALTIME/TAI sleeps. |
| 228 | `clock_gettime` | Routed | REALTIME, TAI and the alarm clocks read the guest realtime base (sub-second); coarse clocks round down to 4 ms; MONOTONIC, MONOTONIC_RAW, BOOTTIME read the monotonic tick. Process CPU clocks (2, per-pid) sum the process's CPU time including the running quantum; thread clocks (3, per-tid) read the thread. PROF, VIRT and SCHED return the same value (no user/system split). EINVAL for ids 10, 12 and above, FD clocks, a missing pid, or a thread of another process. |
| 229 | `clock_getres` | Routed | `{0, 1}` for the hrtimer and SCHED CPU clocks, 4 ms for the coarse clocks and PROF/VIRT CPU clocks; NULL result allowed; EINVAL for an invalid id. |
| 230 | `clock_nanosleep` | Partial | `CLOCK_PROCESS_CPUTIME_ID` and process CPU clock ids sleep until the process's CPU time reaches the target (relative, or absolute with `TIMER_ABSTIME`); a sleeper with no other running thread waits forever, and the scheduler stops with its deadlock message. Static THREAD_CPUTIME, MONOTONIC_RAW and the coarse clocks return EOPNOTSUPP; per-thread CPU clock ids and a missing pid return EINVAL (Linux also allows another thread's clock). Alarm clocks return EPERM without CAP_WAKE_ALARM and otherwise sleep on the matching non-alarm clock. Absolute REALTIME/TAI sleeps follow `clock_settime`. |
| 283 | `timerfd_create` | Routed | REALTIME, MONOTONIC, BOOTTIME and the alarm clocks (EPERM without CAP_WAKE_ALARM); others EINVAL. Cases `timer/timerfd_*`. |
| 286 | `timerfd_settime` | Routed | TFD_TIMER_ABSTIME, TFD_TIMER_CANCEL_ON_SET (absolute REALTIME only; a clock set makes reads return ECANCELED). Flags beyond those two EINVAL. `TFD_IOC_SET_TICKS` ioctl supported. |
| 287 | `timerfd_gettime` | Routed |  |
| 305 | `clock_adjtime` | Partial | `CLOCK_REALTIME` behaves like `adjtimex`; invalid ids EINVAL; every other clock, CPU clocks included, EOPNOTSUPP as on Linux. |

## Polling and watches

| Number | Syscall | Dispatch | Notes |
| ---: | --- | --- | --- |
| 7 | `poll` | Routed | Readiness of every descriptor kind from `Readiness`: pipes, FIFOs and sockets (`StreamPoll`), regular files and directories (always IN/OUT), console input (interactive only), pidfd, eventfd, timerfd, signalfd, epoll and inotify. POLLNVAL for closed descriptors; nfds above RLIMIT_NOFILE EINVAL. Cases `poll/*`. |
| 23 | `select` | Routed | Timeout normalized and written back with the time left; negative nfds EINVAL, nfds above the table clamped; EBADF for a closed descriptor in a set. |
| 213 | `epoll_create` | Routed | size <= 0 EINVAL. Cases `epoll/*`. |
| 232 | `epoll_wait` | Routed | maxevents <= 0 or above INT_MAX/12 EINVAL; level, edge (EPOLLET), EPOLLONESHOT, round robin across waits; never restarted. |
| 233 | `epoll_ctl` | Routed | Linux check order (EFAULT, EBADF, EPERM for descriptors without poll, EINVAL, EPOLLEXCLUSIVE rules, ELOOP for cycles and nesting beyond 4). Items are keyed on (description, fd) and vanish when the description is destroyed. |
| 253 | `inotify_init` | Routed | Cases `inotify/*`. |
| 254 | `inotify_add_watch` | Routed | EINVAL for no event bits or MASK_ADD with MASK_CREATE, ENOTDIR for IN_ONLYDIR, EACCES without read permission, EEXIST for IN_MASK_CREATE on a watched inode; one wd per inode. |
| 255 | `inotify_rm_watch` | Routed | Queues IN_IGNORED; unknown wd EINVAL. |
| 270 | `pselect6` | Routed | Applies the signal mask for the wait (restored unless the call ends with EINTR, then after the handler); size other than 8 EINVAL; time left written back. |
| 271 | `ppoll` | Routed | As poll, with the timespec and signal mask of pselect6. |
| 281 | `epoll_pwait` | Routed | As epoll_wait with a signal mask for the wait. |
| 284 | `eventfd` | Routed | As eventfd2 with flags 0. |
| 290 | `eventfd2` | Routed | Counter, EFD_SEMAPHORE, NONBLOCK, CLOEXEC; reads and writes block; 8-byte buffers (EINVAL); writes of UINT64_MAX EINVAL. Cases `event/eventfd_*`. |
| 291 | `epoll_create1` | Routed | Flags other than EPOLL_CLOEXEC EINVAL. |
| 294 | `inotify_init1` | Routed | IN_NONBLOCK and IN_CLOEXEC; other flags EINVAL; at most 128 instances (EMFILE). |
| 300 | `fanotify_init` | Unrouted | Deliberate: ENOSYS (see deliberate differences). |
| 301 | `fanotify_mark` | Unrouted | Deliberate: ENOSYS. |
| 441 | `epoll_pwait2` | Routed | Timespec timeout (NULL waits forever, EINVAL for an invalid one, EFAULT) and a signal mask. |

## Clocks: deliberate differences

- The guest realtime clock is one process-global base sampled at startup: the Horizon RTC second (`time()`) plus real elapsed monotonic time (`Switch2Guest::RealtimeBase`). Sub-second readings are real elapsed time; the absolute value is as accurate as the RTC (+-1 s). `clock_gettime(CLOCK_REALTIME)`, `gettimeofday`, `time`, futex `FUTEX_CLOCK_REALTIME` timeouts, absolute REALTIME sleeps and file timestamps all read it, so they agree.
- `clock_settime`, `settimeofday` and `adjtimex ADJ_SETOFFSET` change the guest base only. The Horizon clock is untouched, nothing is persisted, and the next launch starts from the RTC again. CAP_SYS_TIME is required (EPERM otherwise).
- `adjtimex` / `clock_adjtime(CLOCK_REALTIME)`: the NTP discipline does not run. `modes == 0` reports `time` from the guest clock, `tick = 10000`, `precision = 1`, `tolerance = 32768000`, and returns `TIME_ERROR` (5) while `STA_UNSYNC` is set, which is the boot default (a Linux host with an NTP daemon reports `TIME_OK`). Write modes (`ADJ_OFFSET`, `ADJ_FREQUENCY`, `ADJ_MAXERROR`, `ADJ_ESTERROR`, `ADJ_STATUS`, `ADJ_TIMECONST`, `ADJ_TICK`, `ADJ_MICRO`, `ADJ_NANO`) store their fields (clamped as Linux does) and read them back without steering the clock or changing the apparent tick rate. `ADJ_SETOFFSET` steps the clock and `ADJ_TAI` sets the TAI offset. `adjtime(3)` single-shot slews are accepted and ignored, and `ADJ_OFFSET`/`ADJ_TIMECONST` read-back is the stored value, not Linux's PLL state.
- Alarm clocks: `clock_gettime`/`clock_getres` on `CLOCK_REALTIME_ALARM` and `CLOCK_BOOTTIME_ALARM` succeed as the plain clocks (a host without a wakeup RTC returns EINVAL). Alarm sleeps need CAP_WAKE_ALARM and then sleep on the plain clock: Horizon offers no wake-from-suspend and never suspends a running application.
- `CLOCK_BOOTTIME` equals `CLOCK_MONOTONIC` (no suspend time). Coarse clocks and PROF/VIRT CPU clocks report 4 ms resolution (CONFIG_HZ=250 on Debian kernels); native hosts may use 1 ms.
- CPU time is wall time of each guest thread's host dispatches (including the runtime's syscall work); there is no user/system split, so `PROF`, `VIRT` and `SCHED` agree.
- Absolute `CLOCK_REALTIME` and `CLOCK_TAI` sleeps are re-armed when the clock is set. Futex absolute realtime timeouts are converted once and are not re-armed.
- Per-thread CPU clocks of another thread cannot be slept on (EINVAL); readable ones work for any thread of the caller's process.

## Credentials: deliberate differences

- Authorization uses capability sets (`CAP_CHOWN`, `DAC_OVERRIDE`, `DAC_READ_SEARCH`, `FOWNER`, `FSETID`, `KILL`, `SETUID`, `SETGID`, `SETPCAP`, `SYS_ADMIN`, `SYS_NICE`, `SYS_PTRACE`, `WAKE_ALARM`) instead of euid 0. Other capabilities have no consumer yet.
- File capabilities (`security.capability`) are not modelled: exec never grants capabilities from a file, only the root rules and ambient set apply.
- `PR_GET_SECUREBITS`/`PR_SET_SECUREBITS` return EINVAL; the bits themselves exist (NOROOT, NO_SETUID_FIXUP, KEEP_CAPS, NO_CAP_AMBIENT_RAISE) but only KEEP_CAPS can be changed, through `PR_SET_KEEPCAPS`.
- `mknod` of device nodes returns EPERM for every caller, with or without `CAP_MKNOD` (no device nodes exist).
- User namespaces exist since milestone 4 task 2: capabilities are per user namespace, and the checks that name a namespace owner use it; every other capability check is against the initial namespace (see [ContainerInventory.md](ContainerInventory.md#namespaces)).
- Launch credentials: `<program>.user` and the Root shell item give uid 0 `RootCredentials()` (all capabilities); any other uid starts with no capabilities.
