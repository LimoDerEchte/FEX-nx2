# Milestone 4: containers and scheduling

Goal: run Steam's container runtime (pressure-vessel, which starts every
game and Proton through bubblewrap) and Chromium-style sandboxes
(steamwebhelper) in the guest. Add the scheduling calls that Wine, games
and thread pools use. Give every other system-administration call the
Linux error.

Groups from [RemainingSyscalls.md](RemainingSyscalls.md): scheduling,
namespaces and mounts, seccomp, `sethostname`, `ioprio_*`, `kcmp`, and the
calls answered with a Linux error. Graphics is out of scope.

## Process

1. The tasks below run **one after another**, each implemented by **one
   Sonnet agent**. Never run two tasks at once. Each agent gets this plan's
   "Global constraints" and its own task text as a brief file in
   `.superpowers/sdd/`, writes its report next to it and returns status,
   a one-line test summary and concerns.
2. After the last task, **one Sonnet agent reviews the whole milestone**
   (the full diff against the milestone's base commit). It writes one
   findings file, ranked by severity.
3. The **Opus orchestrator** fixes the review findings, writes the test
   suite (`LinuxRuntime/ContainerSuite.c` plus a tools script and an
   acceptance program, as in milestones 2 and 3), makes it pass natively
   (uid 1000 and namespace root), builds the console NRO and hands the user
   console runs to debug until acceptance passes. Then it reconciles the
   inventory and checkpoint and commits once on `nx2`.

## Global constraints

- Follow `AGENTS.md`: code in `Source/Switch2`, real Linux semantics,
  root causes, no commits by task agents, report host, native and console
  results separately.
- Guest threads run cooperatively on one host thread. Never block it.
  Validate every guest memory access (a console CPU fault kills the app).
- **Privilege model (Linux user namespaces).** Machine-wide operations
  always fail EPERM, even for guest root: `reboot`, `kexec_load`,
  `kexec_file_load`, `init_module`, `finit_module`, `delete_module`,
  `swapon`, `swapoff`, `acct`, `iopl`, `ioperm`, `vhangup`, `syslog`
  (Debian `dmesg_restrict=1`), `perf_event_open`, `bpf`. Operations scoped
  to a namespace (mount, hostname, chroot, pivot_root, namespace creation)
  need the capability in the user namespace that owns the target
  namespace, as on Linux. The initial user namespace's root keeps its
  current capabilities except those for machine-wide operations, which
  `capget` and `/proc/<pid>/status` must show as absent from the bounding
  set.
- ENOSYS (as a kernel built without the feature): `io_setup`,
  `io_destroy`, `io_getevents`, `io_submit`, `io_cancel`, `io_pgetevents`,
  `io_uring_setup`, `io_uring_enter`, `io_uring_register`, `rseq`,
  `add_key`, `request_key`, `keyctl`, `landlock_*`, `lsm_*`, `quotactl`,
  `quotactl_fd`, `open_tree`, `move_mount`, `fsopen`, `fsconfig`,
  `fsmount`, `fspick`, `mount_setattr`, `statmount`, `listmount`,
  `uretprobe`, `sysfs` and the obsolete entries. `ptrace` gives EPERM (future
  work, see RemainingSyscalls.md).
- Each task adds host tests in `LinuxRuntime/` with a `make check-guest-*`
  target, keeps every existing `check-guest-*` target and the `guest_nro`
  build passing with 0 warnings, and updates a new
  `Source/Switch2/ContainerInventory.md` (one row per syscall: number,
  name, Routed/Partial/Unrouted, Linux behavior and deliberate differences).
- Do not bump the launcher version; the orchestrator does that for console
  builds.

## Tasks

### Task 1: scheduling, small calls and Linux errors

- `sched_setparam`, `sched_getparam`, `sched_setscheduler`,
  `sched_getscheduler`, `sched_get_priority_max`, `sched_get_priority_min`,
  `sched_rr_get_interval`, `sched_setaffinity`, `sched_getaffinity` (review
  the existing handler), `sched_setattr`, `sched_getattr`: per-thread
  policy, priority, nice and CPU mask stored and reported with Linux errors
  (EINVAL, ESRCH, EPERM for real-time policies without CAP_SYS_NICE or
  RLIMIT_RTPRIO, `SCHED_RESET_ON_FORK`, the size handshake of
  `sched_attr`). The CPU count comes from the host core count the guest
  already reports (`/proc/cpuinfo`, `sysconf`); the mask is honored for
  reporting only, which is recorded as a deliberate difference.
- `ioprio_set`, `ioprio_get` (per process, group and user targets; classes
  RT need CAP_SYS_ADMIN or CAP_SYS_NICE as on Linux).
- `kcmp` (KCMP_FILE, KCMP_FILES, KCMP_FS, KCMP_VM, KCMP_SIGHAND, KCMP_IO,
  KCMP_SYSVSEM, KCMP_EPOLL_TFD) using the runtime's existing tables.
- Every EPERM and ENOSYS entry of the global constraints, with the
  capability bounding-set change.

### Task 2: user, UTS, IPC and PID namespaces

- A namespace object per kind with owner user namespace and reference
  counting; processes hold one of each.
- `unshare` and `clone`/`clone3` flags `CLONE_NEWUSER`, `CLONE_NEWUTS`,
  `CLONE_NEWIPC`, `CLONE_NEWPID`, `CLONE_NEWCGROUP` (empty cgroup view),
  `CLONE_NEWNET` (EPERM recorded as a deliberate difference unless the
  network can be isolated), plus the non-namespace flags `CLONE_FILES`,
  `CLONE_FS`, `CLONE_SYSVSEM`.
- User namespaces: `/proc/<pid>/uid_map`, `gid_map`, `setgroups` writes
  with Linux's rules (one write, `deny` before an unprivileged gid map),
  id translation in credentials, `stat` and socket credentials, overflow
  id 65534, full capabilities for the creator inside, capability checks
  against the owning namespace.
- UTS: `sethostname`, `setdomainname`, `uname` per namespace.
- IPC: System V and POSIX message queue tables per namespace.
- PID: nested pid numbers, pid 1 semantics (orphans reparent to it, it
  ignores signals without handlers, its exit kills the namespace),
  `getpid`/`getppid`/`wait` values, `NSpid` in `/proc/<pid>/status`.
- `setns` with `/proc/<pid>/ns/*` files and pidfds (nsfs `stat`
  identities, `ioctl` NS_GET_USERNS/NS_GET_PARENT/NS_GET_NSTYPE).

### Task 3: mount namespaces, mount, pivot_root and chroot

- A mount table per mount namespace (`CLONE_NEWNS`), copied on creation,
  with mount ids, parent ids, propagation flags (private, slave, shared
  accepted; propagation between namespaces may be recorded as a difference
  if not implemented).
- `mount`: bind mounts (`MS_BIND`, `MS_REC`) of files and directories,
  `tmpfs` (memory-backed, size limit, mode, uid, gid options), `proc`,
  `sysfs` (empty or minimal), `devpts`, `mqueue`, read-only and
  nosuid/nodev/noexec remounts (`MS_REMOUNT`, `MS_BIND|MS_REMOUNT`),
  `MS_MOVE`, `MS_PRIVATE`/`MS_SLAVE`/`MS_SHARED`/`MS_UNBINDABLE`. Linux
  errors for unknown types (ENODEV), missing paths, busy targets.
- Path resolution crosses mount points both ways (`..` at a mount root),
  enforces read-only (EROFS), nosuid, nodev and noexec.
- `umount2` with `MNT_DETACH`, `MNT_FORCE` (EPERM or accepted), `UMOUNT_NOFOLLOW`, `MNT_EXPIRE`.
- `pivot_root` with Linux's checks; `chroot` per process (Linux rules for
  `..` above the root and `/proc/<pid>/root`).
- `/proc/<pid>/mountinfo`, `/proc/<pid>/mounts` and `/proc/mounts`
  generated from the table.

### Task 4: seccomp

- `seccomp` and `prctl` `PR_SET_SECCOMP`/`PR_GET_SECCOMP`,
  `PR_SET_NO_NEW_PRIVS`/`PR_GET_NO_NEW_PRIVS` (filters need no_new_privs
  or CAP_SYS_ADMIN).
- Strict mode and filter mode: a classic BPF interpreter over
  `seccomp_data` (`nr`, `arch` = AUDIT_ARCH_X86_64, `instruction_pointer`,
  `args`), checked once at load (Linux's validator rules, 4096
  instructions, EINVAL/EFAULT/EACCES), run at the syscall dispatch entry
  for every guest syscall.
- Actions: KILL_PROCESS, KILL_THREAD, TRAP (SIGSYS with
  `si_call_addr`, `si_syscall`, `si_arch`), ERRNO, TRACE (ENOSYS: no
  tracer), LOG, ALLOW. USER_NOTIF and `SECCOMP_FILTER_FLAG_NEW_LISTENER`
  with a notification fd if feasible, else EINVAL recorded as a
  difference.
- Flags TSYNC, LOG, SPEC_ALLOW, WAIT_KILLABLE_RECV;
  `SECCOMP_GET_ACTION_AVAIL`, `SECCOMP_GET_NOTIF_SIZES`.
- Filters inherited by fork and clone, kept across execve; `Seccomp:`,
  `Seccomp_filters:` and `NoNewPrivs:` in `/proc/<pid>/status`.

## Test suite and acceptance (orchestrator)

- `LinuxRuntime/ContainerSuite.c` (TAP, like CommSuite.c) covering every
  routed call and every EPERM/ENOSYS entry; `ContainerTools.sh` with Debian
  `bubblewrap`, util-linux `unshare`/`nsenter`/`chrt`/`taskset`/`ionice`,
  `hostname`; `container-acceptance.elf` printing `Container acceptance
  results: suite 42, tools 42`.
- Tools cases at least: `bwrap --unshare-all --ro-bind / / --dev /dev
  --proc /proc --tmpfs /tmp --hostname box true`; `bwrap` with
  `--seccomp` from a compiled filter; `unshare -Urm` mount of a tmpfs;
  `nsenter` into a running namespace; `chrt`, `taskset`, `ionice`.
- Native: passes as uid 1000 and as namespace root. Console: the
  acceptance line plus process, communication and filesystem acceptance
  as regressions.
