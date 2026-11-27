# Milestone 4 container and scheduling syscall inventory

Numbers are Linux x86-64 ABI numbers. A routed syscall has a Switch 2
dispatcher handler: a `case` in the `switch (Registers[REG_RAX])` block of
`GuestSyscalls::HandleSyscall` in `GuestRuntime.cpp`. `Partial` means
routed with known gaps named in Notes. `Unrouted` returns ENOSYS. Format
follows [ProcessInventory.md](ProcessInventory.md). The pure decisions of
the scheduling, I/O priority, kcmp and machine-administration calls are in
`GuestSched.h` (host test: `make check-guest-sched`); expected results were
taken from a native Linux 7.3 run as uid 1000 (with `RLIMIT_RTPRIO` 0 and
50) and as `unshare -r` root. Task 1 of 4: scheduling, small calls and
Linux errors. Task 2 of 4: user, UTS, IPC, PID and cgroup namespaces; their
decisions are in `GuestNamespaces.h` (host test: `make check-guest-namespaces`,
expected results from native Linux 7.3 runs of small C programs as uid 1000 and
under `unshare -r`). Task 3 of 4: mounts, `chroot` and `pivot_root` (`GuestMounts.h`, `make check-guest-mounts`, see
[Mounts](#mounts)). Task 4 of 4: seccomp (`GuestSeccomp.h`, `make check-guest-seccomp`, see [Seccomp](#seccomp)). Later tasks add the remaining rows.

## Console verification

Launcher 0.15.12 on the console (2026-11-25, log `.superpowers/sdd/m4-console13.log`): `/bin/container-acceptance.elf` gave `Container acceptance results: suite 42, tools 42`, with container suite 54 passed, 0 failed, 0 skipped as uid 1000 and as root, and ContainerTools.sh all ok. Launcher 0.15.11 gave the same result for container acceptance and also `/bin/process-acceptance.elf` `suite 42, tools 42`. The 0.15.12 regressions passed: comm `suite 42, tools 42` (comm suite 124/0/1) and filesystem `ABI 42, coreutils 0, suite 42` (fs suite 81/0/2).

## Privilege model

The guest runs as a user namespace of a machine it does not own. Operations
on the whole machine fail EPERM for every guest user, root included; their
capabilities (`MachineWideCaps` in `GuestCredentials.h`: CAP_SYS_MODULE 16,
CAP_SYS_RAWIO 17, CAP_SYS_PACCT 20, CAP_SYS_BOOT 22, CAP_SYS_TTY_CONFIG 26,
CAP_SYSLOG 34, CAP_PERFMON 38, CAP_BPF 39) are missing from the bounding set
of every process and from root's effective and permitted sets, so `capget`,
`prctl(PR_CAPBSET_READ)` and `/proc/<pid>/status` (procfs milestone) show them
absent. Root keeps every other capability (`CapInitialRoot`, 33 of 41).
Namespace-scoped operations (mount, hostname, chroot, namespaces) need the
capability in the namespace that owns the target; tasks 2 and 3 add them.

**Known difference: owner of `/`.** Files without an owner record, `/`
included, report the default user 1000:1000. On Linux `/` belongs to kernel
root, so a user namespace that does not map root sees it as 65534.
Giving `/` the owner 0:0 (tried in 0.15.2 to 0.15.7) made `/` unwritable
for uid 1000, which the launcher's rootfs programs need, because they start
in `/`. It was reverted. `ns/user_map_root` checks the owner of `/` on
Linux only. A real fix needs the launcher to start programs in the user's
home folder first.

## Scheduling and CPU affinity

| Number | Syscall | Dispatch | Notes |
| ---: | --- | --- | --- |
| 142 | `sched_setparam` | Routed | `sched_setscheduler` with the policy kept (`GuestSched.h` `Apply`). pid 0 is the caller, otherwise a thread ID. EINVAL for a null param or negative pid, EFAULT, ESRCH, EPERM and EINVAL as Linux (a real-time priority outside 1..99 or 0 on FIFO/RR, nonzero on a fair policy). Raising the priority needs `RLIMIT_RTPRIO` or CAP_SYS_NICE. |
| 143 | `sched_getparam` | Routed | `sched_priority` of the real-time policies, 0 otherwise; null param EINVAL, bad pointer EFAULT. |
| 144 | `sched_setscheduler` | Routed | Policies OTHER, FIFO, RR, BATCH, IDLE, DEADLINE; ISO (4) and EXT (7) are EINVAL (not built). `SCHED_RESET_ON_FORK` (0x40000000) is stored; clearing it needs CAP_SYS_NICE. FIFO/RR need CAP_SYS_NICE, or a nonzero soft `RLIMIT_RTPRIO` that covers the priority (lowering is always allowed); DEADLINE needs CAP_SYS_NICE. A target of another user needs CAP_SYS_NICE. Leaving SCHED_IDLE needs the right to have the current nice. Nice is kept. |
| 145 | `sched_getscheduler` | Routed | Policy, with 0x40000000 when reset-on-fork is set; negative pid EINVAL, unknown thread ESRCH. |
| 146 | `sched_get_priority_max` | Routed | 99 for FIFO/RR, 0 for OTHER/BATCH/IDLE/DEADLINE, EINVAL otherwise (including ISO and EXT). |
| 147 | `sched_get_priority_min` | Routed | 1 for FIFO/RR, 0 for the others, EINVAL otherwise. |
| 148 | `sched_rr_get_interval` | Routed | RR 100 ms, FIFO and DEADLINE 0, the fair policies their slice truncated to whole milliseconds (default slice 2.8 ms). Negative pid EINVAL, bad pointer EFAULT. |
| 203 | `sched_setaffinity` | Partial | Mask stored per thread and reported by `sched_getaffinity`; **never enforced** (the host scheduler places threads). The mask is limited to the CPUs that exist (`svcGetInfo` core mask, 6 on the console); an empty result is EINVAL; a length below 8 is zero extended, above 8 only 8 bytes are read; a thread of another user needs CAP_SYS_NICE (EPERM); inherited by fork and clone, kept by exec. |
| 204 | `sched_getaffinity` | Partial | Returns 8 bytes of the stored mask; length below the CPU count in bytes or not a multiple of 8 is EINVAL; bytes past 8 are not written. Reporting only (see `sched_setaffinity`). |
| 314 | `sched_setattr` | Routed | `struct sched_attr` size handshake: size 0 means 48, a size below 48 or above 4096 or nonzero bytes past our 56 give E2BIG with `size` rewritten to 56; flags above 0x7f, SUGOV and a nonzero `flags` argument are EINVAL; `KEEP_POLICY` ignores the policy field; `KEEP_PARAMS` validates and changes only the flags and clamps (as Linux 7.3); `sched_nice` clamps to -20..19; fair `sched_runtime` is the slice (clamped to 0.1..100 ms, 0 is the default); utilization clamps are stored and range checked (EINVAL above 1024, min above max, size below 56). SCHED_DEADLINE parameters are validated (`__checkparam_dl`) and stored but **not admission controlled** (no EBUSY) and never run as deadline tasks. |
| 315 | `sched_getattr` | Routed | Size min(usize, 56) reported; usize below 48, above 4096, a nonzero flags argument or null pointer EINVAL; bytes past 56 read as zero. Real-time policies report nice 0 and clamp 1024..1024; fair policies report nice and the slice as `sched_runtime`; deadline parameters for SCHED_DEADLINE. |

## I/O priority and kcmp

| Number | Syscall | Dispatch | Notes |
| ---: | --- | --- | --- |
| 251 | `ioprio_set` | Routed | `which` PROCESS (a thread), PGRP, USER; other EINVAL. The class is `(value >> 13) & 7`: NONE needs level 0 (hint bits allowed), BE and IDLE take any data, RT needs CAP_SYS_NICE or CAP_SYS_ADMIN (EPERM), classes 4..7 EINVAL. A target owned by another user needs CAP_SYS_NICE (EPERM); ESRCH when nothing matches. The value is stored (and created lazily as an io_context for `kcmp`); **it changes no I/O scheduling**. |
| 252 | `ioprio_get` | Routed | A process target returns that thread's stored value (0, class NONE, when never set); a group or user target returns the numerically least of its threads, a thread with class NONE counting as best-effort at level `(nice + 20) / 5`. ESRCH when nothing matches, EINVAL for another `which`. Inherited by fork and clone, kept by exec. |
| 312 | `kcmp` | Routed | Types FILE, VM, FILES, FS, SIGHAND, IO, SYSVSEM, EPOLL_TFD; type above 7 EINVAL (after ESRCH). Result 0 equal, 1 below, 2 above (a consistent order of the runtime objects, not Linux's obfuscated pointers). Both threads need ptrace-read access (EPERM). FILE and EPOLL_TFD: EBADF for a bad descriptor, EPOLL_TFD EFAULT, EINVAL for a non-epoll `efd`, ENOENT when the epoll set has no item at that descriptor and offset. FILES: the descriptor table; threads of one process share FS, VM, SIGHAND and SYSVSEM, processes never do (the runtime has no CLONE_FS/FILES/SIGHAND for processes, and a vfork child copies its address space), so a CLONE_VM vfork child compares unequal on VM where Linux says equal. IO: threads with no io_context compare equal; a fork child of a thread that had one gets its own. |

## Namespaces

Every process holds one object of each kind (`Namespace` in `GuestNamespaces.h`): user, UTS, IPC, PID (two: the one
its ids are allocated in and `pid_for_children`), cgroup, net and mount. An object has an inode number (the nsfs
identity: the initial ones are those of a Linux boot, 4026531837 for the user namespace; new ones count up from
4026532000), an owner user namespace and a reference count (`shared_ptr`: the processes, descendant namespaces and
open nsfs files hold it). `Credentials` hold kernel ids and capabilities of the process's user namespace; all
translation to and from the guest's ids happens at the system call boundary.

| Number | Syscall | Dispatch | Notes |
| ---: | --- | --- | --- |
| 272 | `unshare` | Routed | Flags outside THREAD, FS, NEWNS, SIGHAND, VM, FILES, SYSVSEM, NEWUTS, NEWIPC, NEWNET, NEWUSER, NEWPID, NEWCGROUP, NEWTIME: EINVAL. NEWNS implies FS; NEWUSER implies THREAD and FS and fails EINVAL in a process with more than one thread (so does THREAD or VM with several threads); NEWIPC implies SYSVSEM. A new user namespace is made first (EPERM when the euid or egid has no mapping in the caller's namespace, as inside a fresh namespace before `uid_map`; EPERM in a chroot; EUSERS past 33 levels), then every other namespace needs CAP_SYS_ADMIN over it (EPERM). NEWNET is EPERM for everyone. NEWTIME is EINVAL. NEWPID twice without a fork in between is EINVAL, ENOSPC past 32 levels. Every check runs before anything changes. CLONE_SYSVSEM and NEWIPC apply and drop the process's SEM_UNDO list (`exit_sem`), CLONE_FS gives the process its own directory state, CLONE_FILES gives the calling thread its own descriptor table. NEWNS copies the mount table (see [Mounts](#mounts)). |
| 308 | `setns` | Routed | `fd` is an nsfs file (`/proc/<pid>/ns/*`, `NS_GET_USERNS` results) or a pidfd. nsfs: `nstype` 0 or the file's kind, else EINVAL; EBADF for a bad fd, EINVAL for any other file. pidfd: `nstype` a non-empty mask of CLONE_NEW* (EINVAL for unknown bits and for NEWTIME), ESRCH for an exited process; the user namespace is installed first and the others are checked against it; nothing changes unless all checks pass. User namespace: EINVAL for the one you are in, for a multithreaded caller or one that shares its directory state (CLONE_FS) with another process, else CAP_SYS_ADMIN over it (EPERM). UTS, IPC, cgroup, net: CAP_SYS_ADMIN over the namespace's owner and over your own (EPERM). PID: the same, then only the active namespace or a descendant (EINVAL), and it becomes `pid_for_children`. Mount: also CAP_SYS_CHROOT, EINVAL when the directory state is shared. Entering an IPC namespace drops the SEM_UNDO list. |
| 56 | `clone` | Partial | Namespace flags: NEWNS with FS and NEWUSER with FS are EINVAL; CLONE_PARENT by a PID namespace's init is EINVAL; threads cannot carry any NEW* flag (EINVAL; Linux allows UTS, IPC, net and cgroup for threads, which here would need per-thread namespaces); a dead PID namespace (its init exited) gives ENOMEM. Then the same checks as `unshare` (EPERM before the EINVAL for NEWIPC with SYSVSEM). NEWTIME is EINVAL (it is the exit-signal byte of `clone`; `clone3` rejects it). The child lives in the parent's `pid_for_children` (a new namespace under NEWPID, where it is pid 1 and has the namespace's init rules); `CLONE_PARENT_SETTID` and the return value are numbers in the parent's PID namespace, `CLONE_CHILD_SETTID` the number in the child's own. CLONE_FILES gives the child the parent's descriptor table object (exec makes a private copy first), CLONE_FS the directory state (cwd, umask; `chdir` and `umask` of either change both), CLONE_SYSVSEM the SEM_UNDO list. CLONE_VM without VFORK and CLONE_SIGHAND stay EINVAL (processes cannot share memory or handlers). |
| 57 | `fork` | Partial | As `clone` with SIGCHLD; see the process rows in [ProcessInventory.md](ProcessInventory.md). |
| 58 | `vfork` | Partial | As `clone` with CLONE_VM, CLONE_VFORK and SIGCHLD. |
| 435 | `clone3` | Partial | As `clone`; NEWTIME (flag 0x80) is EINVAL like every flag in the exit-signal byte, `set_tid` stays EPERM/EINVAL (not implemented). |
| 170 | `sethostname` | Routed | Per UTS namespace. CAP_SYS_ADMIN over the namespace's owner first (EPERM), then the length as an `int` in 0..64 (EINVAL; 0x100000001 counts as 1), then EFAULT. Names may contain any bytes. |
| 171 | `setdomainname` | Routed | As `sethostname` for the NIS domain name (default empty, Linux shows `(none)`). |
| 63 | `uname` | Routed | The host and domain name of the caller's UTS namespace, zero padded to 64 bytes. |
| 16 | `ioctl` (nsfs) | Partial | On an nsfs file: `NS_GET_USERNS` (the owner, which must be your user namespace or a descendant of it, else EPERM; user namespaces are owned by their parent), `NS_GET_PARENT` (user and PID namespaces, EINVAL otherwise; the parent must be yours or below, else EPERM), `NS_GET_NSTYPE` (the CLONE_NEW* value), `NS_GET_OWNER_UID` (user namespaces only, EINVAL otherwise; the owner's uid in your namespace, 65534 if unmapped); anything else ENOTTY. The new descriptor is close-on-exec. `NS_GET_MNTNS_ID`, `NS_GET_PID_FROM_PIDNS` and friends are ENOTTY. Other ioctls are as before. |

### Identities in a user namespace

A process's `Credentials` hold kernel ids. `getuid`, `geteuid`, `getresuid`, the gid forms, `getgroups`, `stat` and
`statx` owners, `SO_PEERCRED`, `SCM_CREDENTIALS` (send and receive), `si_uid` of signals and `waitid`, `Uid:` and
`Gid:` of `/proc/<pid>/status` and the ids of `uid_map` show ids as `from_kuid_munged` gives them in the reader's
namespace: the mapped id, or 65534 when none. Calls that take ids (`setuid`, `setreuid`, `setresuid`, the gid forms,
`setgroups`, `chown`, `fchown`, `lchown`, `fchownat`, `getpriority` and `ioprio_get` with a user, `SCM_CREDENTIALS`
claims) turn them into kernel ids with `make_kuid` and fail EINVAL (SCM_CREDENTIALS: EPERM) for an unmapped id;
`setfsuid` and `setfsgid` ignore it and return the old id. Capabilities:

- a process created by `CLONE_NEWUSER` or `unshare` has every capability (`CapFull`, bounding set included, ambient
  and inheritable empty, securebits cleared) in the new namespace and none outside it; a process's capability is
  checked as `capable()` (the initial namespace) unless the call names a namespace. The calls that name one:
  `setuid`/`setgid` families, `setgroups`, `capset` and the bounding set (the caller's own namespace);
  `sethostname`, `setdomainname`, IPC ownership (`IPC_RMID`, `IPC_SET`, queue and set access: the namespace that
  owns the IPC namespace); `kill` and friends (`CAP_KILL` over the target's namespace); `unshare`, `clone`, `setns`
  (see above); the file permission checks use `capable_wrt_inode_uidgid` (the capability in the caller's own
  namespace, and the inode's owner and group mapped in it) for `CAP_DAC_OVERRIDE`, `CAP_DAC_READ_SEARCH`,
  `CAP_FOWNER`, `CAP_FSETID` and `CAP_CHOWN` (the sticky-directory checks of `/dev/shm` and `/dev/mqueue` still ask the initial namespace); `/proc/<pid>/ns/*` and `ptrace`-style access use the target's namespace.
- the "root" of a namespace is the kernel id it maps to 0 (`Credentials::RootUid`): exec, `setuid` and the capability
  fixups give root's treatment to that id and to nobody when 0 is unmapped. A set-id executable is ignored when its
  owner or group has no mapping.
- machine-wide operations stay EPERM whatever the namespace: their capabilities (`MachineWideCaps`) are checked
  against the initial namespace and are never in any initial-namespace set. Inside a child namespace `capget` and
  `/proc/<pid>/status` show the full set (as Linux does), the operations still fail.
- `/proc/<pid>/uid_map`, `gid_map`: a reader sees extents translated into its own namespace (a reader inside the
  namespace sees the parent's ids). One write per file (EPERM afterwards), at offset 0 and shorter than 4096 bytes
  (EINVAL), by a task that opened it in the namespace or its parent, with CAP_SYS_ADMIN over the namespace; lines of
  three decimal numbers, a zero count, an overlap or more than 340 extents are EINVAL; an empty map is EINVAL. A map
  that is not the writer's own single id needs CAP_SETUID (CAP_SETGID) over the parent in both the writer and the
  opener, and mapping the parent's uid 0 needs CAP_SETFCAP; a gid map of one id needs `setgroups` denied first; the
  ranges must lie in extents of the parent's map. Files are 0644 owned by the process's owner; opening
  `setgroups` for writing needs CAP_SYS_ADMIN over the namespace (EACCES). `setgroups` takes `allow` or `deny` and
  whitespace; `allow` after `deny` and `deny` after a gid map are EPERM; the `setgroups` syscall needs a gid map and no
  `deny`.

### PID namespaces

Process and thread ids stay global in the runtime; each PID namespace holds the numbers of its tasks and its
ancestors' (`PidNs::Allocate`, first number 1). Everything that takes or returns a pid goes through the caller's
namespace: `getpid`, `gettid`, `getppid`, `getpgrp`, `getpgid`, `getsid`, `setsid`, `setpgid`, `kill` (pid, group and
-1, which skips pid 1 and invisible processes), `tkill`, `tgkill`, `rt_sigqueueinfo`, `rt_tgsigqueueinfo`,
`pidfd_open`, `wait4`, `waitid`, `clone` (return value and `parent_tid`), `set_tid_address`, `getpriority`,
`setpriority`, `ioprio_*`, `sched_*` with a pid, `kcmp`, `prlimit64`, `capget`, `capset`, `process_vm_*`,
`migrate_pages`, CPU clock ids, futex owner words (PI futexes and robust lists use the thread's number in its own
namespace), `si_pid` of signals and `waitid`, `SO_PEERCRED` and `SCM_CREDENTIALS`, `/proc` directory names and
`/proc/self`. A process the caller cannot see is ESRCH and reads as 0 (`getppid` of a namespace's init, `getpgrp`
and `getsid` outside the namespace). The init (pid 1) of a namespace:

- is the reaper of the namespace's orphans (a child subreaper inside the namespace goes first, ancestors outside it
  are never used) and is the only process that gets them;
- ignores signals that have the default action from its own namespace, and SIGKILL and SIGSTOP too; from an
  ancestor namespace SIGKILL and SIGSTOP are delivered and the rest still ignored; a handler or SIG_IGN is normal;
- may not `clone(CLONE_PARENT)` (EINVAL);
- when it exits, kills every process in the namespace and below it with SIGKILL and the namespace takes no more
  numbers (`fork`, `clone` and thread creation fail ENOMEM, also for a process that unshared into it).

`/proc/<pid>/status` shows `Tgid`, `Pid`, `PPid` in the reader's namespace and `NStgid`, `NSpid`, `NSpgid`, `NSsid`
as the chain from the reader's namespace down to the process's (0 where a group or session is outside a level).
`/proc/<pid>/ns/{user,uts,ipc,pid,pid_for_children,cgroup,net,mnt}` are links reading `user:[4026531837]`; `stat`
on one follows it to an nsfs file (mode 0444, kernel root, device 0:7, the namespace's inode number); `open` needs
`ptrace`-read access to the process (EACCES) and is read-only; `time` links are absent.

### IPC and other namespaces

An IPC namespace owns its System V tables (shared memory, semaphores, queues, with their limits and keys) and its
POSIX message queue tree (names, accounting); a new one starts empty, a process's SEM_UNDO list applies to the
namespace it had when it was made. A UTS namespace copies the host and domain name of its creator. A cgroup
namespace is an object only (the guest has no cgroup hierarchy; there is no `/proc/self/cgroup`). The network
namespace is the single initial one: `CLONE_NEWNET` is EPERM for everyone, `setns` to a net namespace file succeeds
and changes nothing.

## Mounts

Task 3 of 4: mount namespaces, `mount`, `umount2`, `pivot_root` and `chroot`. The decisions (flags, option parsing,
the table of one namespace, bind, remount, move, propagation changes, umount, pivot_root, copy on `CLONE_NEWNS`,
`mountinfo` and `mounts` text) are in `GuestMounts.h` (host test: `make check-guest-mounts`). Expected results were
taken from native Linux 7.3 runs of small C programs under `unshare -rm` (uid 1000; scratch programs, not in the
repository): bind and tmpfs mounts with every error and read-only case, remount rules including locked flags in a
copied namespace, `MS_MOVE`, propagation, `umount2` flags (`MNT_EXPIRE` gave EAGAIN once, then 0), `pivot_root`
errors, `chroot` and `..` above the root, `mountinfo` and `mounts` text.

### How a path is resolved

A position is a mount plus a host path inside it (the path the file layer already uses). `ResolveSdPath` and
`WalkSdPath` in `GuestRuntime.cpp` are the only place a guest name becomes a position, so every path-taking system
call goes through them:

- the walk starts at the process root (absolute names, `Fs->Root`/`Fs->RootMount`), the working directory
  (`Fs->Folder`/`FolderMount`) or a directory descriptor (`OpenFile::Path`/`Mount`);
- stepping onto a mount point continues in the topmost mount covering it (`Mounts::Cross`; stacked mounts are
  followed to the top); `..` at the root of a mount continues at its mount point, and `..` at the process root (or
  at the `RESOLVE_BENEATH`/`RESOLVE_IN_ROOT` scope) stays;
- absolute symlinks restart at the process root, `/proc/<pid>/root` and `cwd` jump to that process's root and
  directory (ptrace-read access needed, EACCES; `RESOLVE_NO_MAGICLINKS` ELOOP; BENEATH/IN_ROOT EXDEV);
- the result is a host path (so a bind mount shares inodes with its source) plus the mount it lies in
  (`LastMount`), the mount of its parent directory (`LastParentMount`, what `mkdir`, `unlink`, `rename` need) and
  whether the last component was a mount point (`LastCrossed`);
- `RESOLVE_NO_XDEV` fails on every mount crossing, either way.

Path entry points checked (all reach `ResolveSdPath`, directly or through `PathStat`/`GetStatAt`):
`open`, `openat`, `openat2`, `creat`, `O_TMPFILE`, `open_by_handle_at`; `stat`, `lstat`, `newfstatat`, `statx`,
`access`, `faccessat`, `faccessat2`, `readlink`, `readlinkat`, `statfs`, `chdir`, `execve`, `execveat` (script and ELF
interpreters), `truncate`, `chmod`, `fchmodat`, `chown`, `lchown`, `fchownat`, `utime`, `utimes`, `futimesat`,
`utimensat`, the xattr family, `name_to_handle_at`, `inotify_add_watch`, `mkdir`, `mkdirat`, `mknod`, `mknodat` and
`bind` of an `AF_UNIX` socket, `symlink`, `symlinkat`, `link`, `linkat`, `unlink`, `unlinkat`, `rmdir`, `rename`,
`renameat`, `renameat2`, `connect` to an `AF_UNIX` path, and the four new calls. Descriptor-relative calls start at
the descriptor's mount. Not covered: `execveat(AT_EMPTY_PATH)` and `open_by_handle_at` rebuild a guest name from the
descriptor's host path relative to the initial root (so they ignore `chroot` and mounts elsewhere).

Read-only and access flags (checked against the mount, and the file system for a read-only superblock):
read-only gives EROFS for `open` of a regular file for writing or with `O_TRUNC`, for a missing `O_CREAT` name, for
`O_TMPFILE`, `mkdir`, `mknod`, `symlink`, `link` (new name), `unlink` and `rmdir` (also for a name that does not
exist, as Linux), `rename`, `truncate`, `chmod`, `chown`, `utimensat`, `setxattr`, `removexattr` and `access(W_OK)`;
an existing name wins (EEXIST) over EROFS for the creating calls, and `link` or `rename` across two mounts is EXDEV
(after EROFS for `link`, before it for `rename`). `nosuid` makes `execve` ignore set-user-ID and set-group-ID bits,
`noexec` makes `execve` EACCES and `mmap(PROT_EXEC)` EPERM, `nodev` makes opening `/dev/null` and `/dev/tty`
EACCES. `rmdir` or `rename` of a mount point and `unlink` of a bound file are EBUSY. Sysfs, devpts and mqueue
mounts take no creations (EPERM).

### System calls

| Number | Syscall | Dispatch | Notes |
| ---: | --- | --- | --- |
| 165 | `mount` | Routed | Flags as `path_mount`: `MS_REMOUNT` first, then `MS_BIND` (`MS_REC`), then one of `MS_SHARED`, `MS_PRIVATE`, `MS_SLAVE`, `MS_UNBINDABLE` (more than one or none of the others EINVAL, `MS_REC` recursive), then `MS_MOVE`, else a new file system. The target is looked up first (ENOENT, following symlinks), then `MS_NOUSER` EINVAL, then CAP_SYS_ADMIN over the user namespace that owns the mount namespace (EPERM). **Bind**: a missing source ENOENT, no source EINVAL, an unbindable source EINVAL, a directory onto a file or a file onto a directory ENOTDIR; flags other than `MS_REC` are ignored on creation (a read-only bind needs `MS_REMOUNT|MS_BIND|MS_RDONLY`); the copy shares its source's peer group and keeps its flags and locks; `MS_REC` also copies the covering mounts below, skipping unbindable ones. **Remount**: EINVAL unless the target is a mount point; locked flags (see below) EPERM; `MS_BIND|MS_REMOUNT` replaces the per-mount flags (nosuid, nodev, noexec, noatime, nodiratime, relatime, ro, nosymfollow; unspecified flags are cleared, relatime is not default when `MS_NOATIME`) and makes read-only EBUSY while a file is open for writing on the mount; plain `MS_REMOUNT` reconfigures the file system (CAP_SYS_ADMIN over its owner, EPERM; data errors EINVAL; ro EBUSY with writers; `sync`, `dirsync`, `mand`, `lazytime`; tmpfs `size=` and `nr_inodes=`) and replaces the per-mount flags too. **Move**: source must be a mount root with a parent, not locked, not under a shared parent (EINVAL), a directory onto a directory (EINVAL otherwise), ELOOP into itself or below. **New file system**: no type EINVAL, unknown ENODEV, bad option EINVAL, EPERM without CAP_SYS_ADMIN over the file system's owner (proc: the PID namespace's, mqueue: the IPC namespace's, sysfs: the network namespace's, others: yours), target not a directory ENOTDIR; `MS_RDONLY` sets the mount and the file system read-only. Types: `tmpfs`, `ramfs`, `proc`, `sysfs`, `devpts`, `mqueue`. `tmpfs` options `size`, `nr_blocks`, `nr_inodes`, `mode`, `uid`, `gid` (ids of the caller's namespace, unmapped EINVAL), `huge`, `mpol=default|local`, `inode32`, `inode64`, `noswap`; empty items skipped, the last duplicate wins, sizes round up to pages. The magic `0xC0ED0000` in the high flag bits is dropped. |
| 166 | `umount2` | Routed | Flags outside `MNT_FORCE`, `MNT_DETACH`, `MNT_EXPIRE`, `UMOUNT_NOFOLLOW` EINVAL before the lookup; then ENOENT; CAP_SYS_ADMIN over the namespace's owner (EPERM); not a mount root, not in this namespace, or locked EINVAL; `MNT_FORCE` needs CAP_SYS_ADMIN over the file system's owner (EPERM) and is otherwise accepted without effect. `MNT_EXPIRE` with `MNT_FORCE`, `MNT_DETACH` or on your root mount EINVAL, busy EBUSY, the first call on an idle mount EAGAIN and marks it (using the mount clears the mark), the next one unmounts it. `MNT_DETACH` removes the mount and everything below at once (open files, working directories and roots keep the detached mount alive; `getcwd` then prints `(unreachable)`). Otherwise EBUSY while the mount has children (marked idle ones go first), open files, a working directory or a root in it, or it is the caller's root. |
| 155 | `pivot_root` | Routed | CAP_SYS_ADMIN over the namespace's owner first (EPERM), then both names must be existing directories (ENOENT, ENOTDIR). Then `pivot_root`'s checks in order (all EINVAL unless noted): put_old, the parent of new_root or the parent of the root mount shared; new_root or the root not in this namespace; new_root locked; new_root or put_old on the root mount (EBUSY); the root not a mount root, or the root mount without a parent; new_root not a mount root or without a parent; put_old not under new_root; new_root not under the root. The old root mount moves onto put_old, the new one takes its place, and every process whose root or working directory was the old root now has the new root. `pivot_root(".", ".")` works. |
| 161 | `chroot` | Routed | ENOENT, ENOTDIR, then search permission (EACCES), then CAP_SYS_CHROOT in your own user namespace (EPERM). The root of every process sharing the directory state (`CLONE_FS`) changes; the working directory does not. `..` at the root stays, from outside the root it walks up as usual; a symlink and `/proc/<pid>/root` read as the process's root. `unshare(CLONE_NEWUSER)` and `clone(CLONE_NEWUSER)` are EPERM while chrooted (`current_chrooted`: the root is not the root of the mount namespace). |

### Mount namespaces and the table

Each `MntNs` owns a `Mounts::Table` (`MntNs::Table`). The initial table has a hidden top mount (no parent, never
printed; it makes `pivot_root` of the first root possible and stops `..` at the SD root) and, below it, `/` (SD
card, `msdos`/`sdmc`, device 0:1), `/proc` (proc), `/dev` (devtmpfs) and `/dev/shm` (tmpfs); the last three are mounts
of the same host paths the file layer already serves, so a bind or move of `/proc` into a new root works and a new
`proc` mount shows the live process tree. All are private. `CLONE_NEWNS` (clone or unshare) copies the table
(`CopyTable`, new mount ids) and moves the process's root and working directory onto the copies; when the new
namespace is owned by another user namespace the copies lock what a less privileged owner must not relax or see
through (every mount except the top is locked: no umount, move or pivot; read-only, nodev, nosuid, noexec and the
atime flags may not be cleared) and shared mounts become slaves. `setns` of a mount namespace (the checks of
task 2) puts the process's root and working directory at the root of the namespace. A mount namespace dies with
its last process and descriptor.

`/proc/<pid>/mountinfo` and `/proc/<pid>/mounts` (and `/proc/mounts`, a link to `self/mounts`) print the target
process's namespace from its root: `id parent major:minor root mountpoint options [shared:N] [master:N]
[unbindable] - fstype source superoptions`, mount points reachable from the root only, names escaped (`\040`),
`size=`, `nr_inodes=`, a non-default `mode=`, non-zero `uid=` and `gid=` for tmpfs, ro/rw from the mount (mountinfo
field 6) and from the file system (the last field; `/proc/mounts` merges them). `/proc/<pid>/root` and `cwd` are
links (the process's root, as the reader sees it).

### tmpfs and the other memory file systems

tmpfs and ramfs keep their files in a hidden folder of the SD card (`/fex/.tmp/<device>`), made at mount time with
the options' mode, uid and gid and removed when the last mount, file or directory using the file system is gone
(and for every stale folder at the first mount of a run). Everything the file layer does for the rootfs (inode
store, overflow names, xattrs, links, FIFOs) works there. `sysfs`, `devpts` and `mqueue` are empty file systems of
the same kind (no `/sys` tree, no pty nodes, no queue files). The root file system's `/dev/pts` is the one built-in devpts instance (mode 0755, statfs type 0x1cd1) with working pseudo-terminals: see [TerminalInventory.md](TerminalInventory.md). A devpts mounted by the guest (bwrap `--dev`) is still an empty file system of its own.

### Deliberate differences and open items

| Interface | Behavior |
| --- | --- |
| tmpfs is SD-backed | Not memory-backed (the file layer is path based; `GuestMemoryFile` has no directories). Contents are discarded with the file system, so a tmpfs mount is as non-persistent as on Linux, but it uses SD card space and its speed |
| tmpfs `size=` and `nr_inodes=` | Parsed, rounded, shown in `mountinfo`, `statfs` and changed by remount, **never enforced** (no ENOSPC from them); incomplete |
| Propagation | Peer groups (`shared:N`), slaves (`master:N`) and unbindable mounts are tracked and enforce the EINVAL rules (pivot_root, move, bind of an unbindable mount, copy to a less privileged namespace), but a mount or umount does not propagate to peers or slaves; initial mounts are private (Linux with systemd starts shared) |
| `/proc`, `/dev`, `/dev/shm` mounts | The default mounts are real table entries, but unmounting them in the initial namespace does not hide the virtual trees (the file layer serves `/proc` and `/dev` by path); mounting `proc` somewhere else (and binding them into a new root) works. A second `proc` mount reports the same `st_dev` as the first |
| `mount_too_revealing` | A `proc` or `sysfs` mount in a child user namespace is not refused for lack of a fully visible existing mount (Linux gives EPERM) |
| File systems | Only the six above; every other name is ENODEV (as a kernel built without it; block-device file systems have no device to mount). The new mount API (`open_tree`, `move_mount`, `fsopen`, `fsconfig`, `fsmount`, `fspick`, `mount_setattr`, `statmount`, `listmount`) stays ENOSYS |
| Writers and busy | A remount to read-only is EBUSY while a file is open for writing on the mount (or file system); an unlinked-but-open file on a read-only remount and mappings of a file do not count as users for `umount` |
| Mount ids and devices | Ids start at 1 and count up per run; a new file system gets its own anonymous device (`st_dev`, `statfs` id) from 0:40; bind mounts share their source's device; `statx` still lacks `stx_mnt_id`; `name_to_handle_at` reports the mount id |
| `rename` of a directory containing mounts | Allowed (the mounts follow the new name); Linux allows it too. Renaming or removing the host path a bind mount shares (the source) is not refused |
| Directory entries | `getdents` derives the `..` entry's inode from the host path's parent, so `..` of a mounted root reports the host parent rather than the mount point's directory; mount points are not marked specially |
| `chroot` and handles | `execveat(AT_EMPTY_PATH)` and `open_by_handle_at` build names relative to the initial root |
| `/proc/<pid>/fd`, `exe`, `maps` | Not present (the procfs milestone) |
| `/proc/sys` | Only `kernel/{overflowuid,overflowgid,hostname,domainname,ostype,osrelease}` and `user/max_{cgroup,ipc,mnt,net,pid,user,uts}_namespaces` (126662, the checking host's default) are served, read only (mode 0644, writes EACCES; `sethostname` is the way to change the name); the rest is Milestone 6. No `max_time_namespaces` (no time namespaces) |
| `/dev` devices | `null`, `zero`, `full`, `random`, `urandom` (1:3, 1:5, 1:7, 1:8, 1:9, mode 0666) and `tty` (5:0). `/dev/tty` always exists (it can be bind-mounted) and opens the process's controlling terminal (the console in an interactive run, or a pty slave), else ENXIO as natively without one. `random` never blocks. Not done: `splice` into `zero`/`full`/`random`, the `ioctl`s of the random devices (RNDGETENTCNT etc.), `/dev/console`, a working `devpts` for a guest mount (bwrap `--dev` makes its own empty `pts` mount and the `ptmx` symlink, which succeed, but no pty can be opened through them). `/dev/ptmx`, `/dev/pts/ptmx` and `/dev/pts/<n>` work in the root file system, see [TerminalInventory.md](TerminalInventory.md) |
| PID namespaces of `/proc` | Each `proc` superblock keeps the PID namespace of its mounter (the initial one for the default mount); listing, `/proc/<pid>` lookup, `/proc/self` and the `NS*` lines of `status` use it, not the reader's. Thread ids of extra threads stay in the namespace maps until it is freed (only reaped processes are pruned, at the next fork) |

## Calls answered with EPERM

| Number | Syscall | Dispatch | Notes |
| ---: | --- | --- | --- |
| 101 | `ptrace` | Routed | EPERM for every request, root included (future work: needs a way to debug on the console). |
| 103 | `syslog` | Routed | EPERM for every type, root included (Debian `dmesg_restrict=1`; CAP_SYSLOG is absent). |
| 153 | `vhangup` | Routed | EPERM (CAP_SYS_TTY_CONFIG is absent). |
| 163 | `acct` | Routed | EPERM (CAP_SYS_PACCT is absent). |
| 167 | `swapon` | Routed | EPERM (machine-wide). |
| 168 | `swapoff` | Routed | EPERM (machine-wide). |
| 169 | `reboot` | Routed | EPERM before the magic numbers are looked at (CAP_SYS_BOOT is absent). |
| 172 | `iopl` | Routed | Level 0 changes nothing and succeeds (as Linux); 1 to 3 EPERM (CAP_SYS_RAWIO is absent); above 3 EINVAL. |
| 173 | `ioperm` | Routed | A bad range EINVAL; clearing permission bits succeeds as on Linux; setting them EPERM. |
| 175 | `init_module` | Routed | EPERM (CAP_SYS_MODULE is absent). |
| 176 | `delete_module` | Routed | EPERM. |
| 246 | `kexec_load` | Routed | EPERM (CAP_SYS_BOOT is absent). |
| 298 | `perf_event_open` | Routed | EFAULT when `attr` is unreadable, else EPERM (CAP_PERFMON/CAP_SYS_ADMIN paranoid level; the guest has no PMU access). |
| 313 | `finit_module` | Routed | EPERM. |
| 320 | `kexec_file_load` | Routed | EPERM. |
| 321 | `bpf` | Routed | EPERM for every command (unprivileged BPF is disabled and CAP_BPF is absent). |

## Calls answered with ENOSYS

A kernel built without the feature, and the obsolete entries, return ENOSYS.
The dispatcher lists them explicitly (one `case` group) so they read as
decisions and not as forgotten calls.

| Number | Syscall | Dispatch | Notes |
| ---: | --- | --- | --- |
| 134 | `uselib` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 139 | `sysfs` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 156 | `_sysctl` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 174 | `create_module` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 177 | `get_kernel_syms` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 178 | `query_module` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 179 | `quotactl` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 180 | `nfsservctl` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 181 | `getpmsg` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 182 | `putpmsg` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 183 | `afs_syscall` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 184 | `tuxcall` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 185 | `security` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 206 | `io_setup` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 207 | `io_destroy` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 208 | `io_getevents` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 209 | `io_submit` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 210 | `io_cancel` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 212 | `lookup_dcookie` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 214 | `epoll_ctl_old` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 215 | `epoll_wait_old` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 236 | `vserver` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 248 | `add_key` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 249 | `request_key` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 250 | `keyctl` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 333 | `io_pgetevents` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 334 | `rseq` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 335 | `uretprobe` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 425 | `io_uring_setup` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 426 | `io_uring_enter` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 427 | `io_uring_register` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 428 | `open_tree` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 429 | `move_mount` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 430 | `fsopen` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 431 | `fsconfig` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 432 | `fsmount` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 433 | `fspick` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 442 | `mount_setattr` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 443 | `quotactl_fd` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 444 | `landlock_create_ruleset` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 445 | `landlock_add_rule` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 446 | `landlock_restrict_self` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 457 | `statmount` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 458 | `listmount` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 459 | `lsm_get_self_attr` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 460 | `lsm_set_self_attr` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |
| 461 | `lsm_list_modules` | Unrouted | ENOSYS (as a kernel without the feature; explicit case). |

## Seccomp

The validator, interpreter, filter chain and action precedence are in `GuestSeccomp.h` (host test:
`make check-guest-seccomp`; two filters compiled by libseccomp 2.6 plus hand-written Chromium-style ones, expected
values from native Linux 7.3 as uid 1000 with no_new_privs). The glue is `SeccompGate` (run at the top of
`HandleSyscall`, before the exit calls and the `switch`, so every syscall is filtered including ENOSYS ones),
`SeccompCall` and `GuestPrivilegeControl` in `GuestRuntime.cpp`. State is per thread (mode plus a shared immutable
chain); fork, clone and clone3 copy it, `execve` keeps it.

| Number | Syscall | Dispatch | Notes |
| ---: | --- | --- | --- |
| 317 | `seccomp` | Partial | `SET_MODE_STRICT` (flags and args must be 0), `SET_MODE_FILTER`, `GET_ACTION_AVAIL`, `GET_NOTIF_SIZES` (80, 24, 64). Unknown operation EINVAL. `SET_MODE_FILTER` check order as native: flags (EINVAL), `struct sock_fprog` copy (EFAULT), length 0 or above 4096 (EINVAL), no_new_privs or CAP_SYS_ADMIN in the caller's user namespace (EACCES), instruction copy (EFAULT), validator (EINVAL), strict mode already set (EINVAL), TSYNC conflict (the conflicting thread's id, ESRCH with `TSYNC_ESRCH`), chain over 32768 instructions (ENOMEM; each earlier filter counts 4 more). Flags TSYNC, LOG, SPEC_ALLOW, TSYNC_ESRCH accepted; NEW_LISTENER and WAIT_KILLABLE_RECV are EINVAL (see differences) |
| 157 | `prctl` seccomp options | Routed | `PR_GET_SECCOMP` (0, 1, 2; other arguments ignored), `PR_SET_SECCOMP` mode 1 (strict, arg3 ignored) and 2 (filter, arg3 the `sock_fprog`), other modes EINVAL. `PR_SET_NO_NEW_PRIVS` and `PR_GET_NO_NEW_PRIVS` are from task 1 |
| - | `/proc/<pid>/status` | Routed | `NoNewPrivs:`, `Seccomp:` and `Seccomp_filters:` in Linux's order; the values come from the main thread |

Validator (Linux `bpf_check_classic` + `seccomp_check_filter`): 1 to 4096 instructions; only `LD W ABS` reads
`seccomp_data` (offset below 64, 4 byte aligned), `LD W LEN` and `LDX W LEN` give 64; `LD/LDX IMM`, `MEM`, `ST`, `STX`,
`TAX`, `TXA`, all ALU operations (`DIV`/`MOD` by constant 0 and shifts by 32 or more rejected), `JA`, `JEQ`, `JGT`,
`JGE`, `JSET` (K and X), `RET K` and `RET A` are allowed; half or byte loads, `IND`, `MSH`, `RET X` are EINVAL; scratch
indexes below 16; every jump stays inside the program; the last instruction is a RET; a scratch cell must be written
on every path before it is read. At run time a division or modulo by X = 0 returns 0 (KILL_THREAD) and shifts by X use
the low 5 bits.

| Action | Behavior |
| --- | --- |
| `KILL_PROCESS`, and any unknown action | The process dies of SIGSYS (no core file is written) |
| `KILL_THREAD` | The thread exits; the last thread of a process takes the process down with SIGSYS (native single thread run: killed by SIGSYS) |
| `TRAP` | SIGSYS to the thread: `si_code` SYS_SECCOMP (1), `si_errno` the 16 data bits, `si_call_addr` the address after the syscall instruction, `si_syscall`, `si_arch` 0xc000003e. A blocked or ignored SIGSYS is unblocked and reset to default first, so the process dies. The handler's saved `rax` is the syscall number (native); the syscall does not run |
| `ERRNO` | Returns the data bits as errno, clamped to 4095; data 0 returns 0 |
| `USER_NOTIF` | ENOSYS (no listener, as native without `NEW_LISTENER`) |
| `TRACE` | ENOSYS (no tracer, as native) |
| `LOG` | Allowed; nothing is logged (no audit log) |
| `ALLOW` | Runs the syscall |

Across a chain every filter runs and the lowest action by signed 32 bit value wins (KILL_PROCESS, KILL_THREAD, TRAP,
ERRNO, USER_NOTIF, TRACE, LOG, ALLOW); on a tie the newest filter's value. Strict mode allows `read`, `write`, `exit`
and `rt_sigreturn`; any other syscall (`exit_group` too) kills the thread with SIGKILL (the process when it is the last
thread). A syscall that blocks and runs again is filtered once.

## Deliberate differences

| Interface | Behavior |
| --- | --- |
| Scheduling is bookkeeping | Policy, priority, nice, slice, deadline parameters and clamps are stored per thread and reported with Linux's validation, but the runtime and the Horizon scheduler (which runs each guest thread's host thread) ignore them: a SCHED_FIFO thread does not preempt, SCHED_DEADLINE has no admission control (no EBUSY) and no deadline guarantee |
| Nice is per process | Linux keeps nice per thread. `setpriority`, `sched_setattr` and `sched_setscheduler` change the whole process; a `SCHED_RESET_ON_FORK` thread clone leaves nice alone (a fork child resets it) |
| CPU affinity is reporting only | The mask is stored per thread and `sched_getaffinity` returns it, clamped to the 6 (console `svcGetInfo` core mask) CPUs. Threads are never pinned. More than 64 CPUs are not modeled (one 8 byte word) |
| Policies ISO and EXT | EINVAL: the emulated kernel is built without them (the host kernel the checks ran on has SCHED_EXT) |
| Fair slice | `sched_runtime` of a SCHED_OTHER/BATCH thread is the slice (default 2.8 ms, the 0.7 ms base slice scaled for 8 CPUs; Linux 7.3 on the checking host reported 1.6 ms). `sched_rr_get_interval` truncates it to whole milliseconds (HZ 1000) |
| `ioprio` has no effect | Values are stored and reported (`ioprio_get` default 0, NONE). The best of a group counts a NONE thread as best-effort at `(nice + 20) / 5`, which matched one native run; a second native run with a different nice gave a result this rule does not predict (see the task report) |
| `kcmp` object identity | Order is by runtime object address, so `1` and `2` differ from Linux's obfuscated order; only equality and antisymmetry are Linux's. A `CLONE_VM` vfork child has its own address space object (compares unequal on VM) |
| Process model limits | `kcmp` SIGHAND and VM treat all threads of a process as sharing, and different processes as never sharing; FS, FILES and SYSVSEM compare the real shared objects (CLONE_FS, CLONE_FILES, CLONE_SYSVSEM) |
| Namespaces are per process | Linux keeps them per thread: `unshare(CLONE_NEWUTS)`, `NEWIPC`, `NEWPID`, `NEWCGROUP` and `setns` in a multithreaded process change every thread here, and a new thread clone cannot carry a CLONE_NEW* flag (EINVAL). `CLONE_NEWUSER` and `setns` of a user namespace need one thread, as on Linux |
| Network namespace | No isolation is possible: `CLONE_NEWNET` is EPERM, also for namespace root and together with `CLONE_NEWUSER` |
| Time namespace | The emulated kernel is built without it: `CLONE_NEWTIME` is EINVAL and `/proc/<pid>/ns/time` is absent |
| Init exit | Linux holds the exit of a PID namespace's init until every process in the namespace is reaped; here the init's parent hears of its exit at once and the others die at their next scheduling (their zombies are reaped by the runtime's virtual init) |
| Capability sets after `CLONE_NEWUSER` | Full, as Linux; the machine-wide capabilities show in a child namespace's sets (they are checked against the initial namespace, where they are absent) |
| IPC object ids | `ipc_perm` uids and gids of System V objects are kernel ids in `IPC_STAT` output (Linux translates them into the caller's namespace) |
| `/proc` | One tree for the caller's PID namespace (mount `proc` in a new root to see another one); `/proc/<pid>/status` has a fixed field set (no memory or signal fields); the files are snapshots taken at `open` |
| `clone3` `set_tid` | Not implemented (EPERM without CAP_SYS_ADMIN or CAP_CHECKPOINT_RESTORE, else EINVAL) |
| `iopl`, `ioperm` | Not an unconditional EPERM: the calls that change nothing (iopl level 0, clearing ioperm bits, range errors) return what Linux returns; every request that would grant I/O privilege is EPERM |
| `perf_event_open` argument order | Only the first 8 bytes of `attr` are checked (EFAULT) before EPERM; Linux validates `attr->size` (E2BIG/EINVAL) first |
| Seccomp notification fds | `SECCOMP_FILTER_FLAG_NEW_LISTENER` and `WAIT_KILLABLE_RECV` return EINVAL (the notification ioctls and the supervisor wait are not implemented). A filter may still return `USER_NOTIF`: the syscall gets ENOSYS, as on Linux for a filter without a listener. `GET_ACTION_AVAIL(USER_NOTIF)` therefore reports EOPNOTSUPP (native: 0). `GET_NOTIF_SIZES` returns Linux 7.3's sizes |
| Seccomp logging and core files | `SECCOMP_FILTER_FLAG_LOG` is stored; no action is logged. A SIGSYS kill sets no core-dump flag in the wait status (native: core dumped) |
| Seccomp no_new_privs | The flag is per process (Linux: per thread); TSYNC therefore changes nothing for it |

