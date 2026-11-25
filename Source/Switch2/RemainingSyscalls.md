# Remaining x86-64 syscalls

Written 2026-11-24, after milestone 3. The x86-64 table in
`Source/Tools/LinuxEmulation/LinuxSyscalls/x64/SyscallsEnum.h` has 379
entries. [FilesystemInventory.md](FilesystemInventory.md),
[ProcessInventory.md](ProcessInventory.md) and
[CommunicationInventory.md](CommunicationInventory.md) track 289 of them.
The 90 below are in no inventory. Only `sched_getaffinity` (204) and
`getrandom` (318) have a handler in `GuestRuntime.cpp`; the other 88
return ENOSYS. Milestone 4 ([plan](Milestone4Plan.md)) implements or
answers all of them except `modify_ldt`, `set_thread_area` and
`get_thread_area`, which are in milestone 5 ([plan](Milestone5Plan.md)).
Milestone 6 ([plan](Milestone6Plan.md)) covers `/proc`, `/sys`, `/dev`
and input. `ptrace` stays EPERM (future work below).

| Group | Syscalls (number) | Milestone |
| --- | --- | --- |
| Scheduling and CPU affinity (11) | `sched_setparam` 142, `sched_getparam` 143, `sched_setscheduler` 144, `sched_getscheduler` 145, `sched_get_priority_max` 146, `sched_get_priority_min` 147, `sched_rr_get_interval` 148, `sched_setaffinity` 203, `sched_getaffinity` 204 (handler), `sched_setattr` 314, `sched_getattr` 315 | 4 |
| Asynchronous I/O (9) | `io_setup` 206, `io_destroy` 207, `io_getevents` 208, `io_submit` 209, `io_cancel` 210, `io_pgetevents` 333, `io_uring_setup` 425, `io_uring_enter` 426, `io_uring_register` 427 | 4 |
| Thread runtime and security (13) | `rseq` 334, `set_thread_area` 205, `get_thread_area` 211, `modify_ldt` 154, `getrandom` 318 (handler), `kcmp` 312, `seccomp` 317, `landlock_create_ruleset` 444, `landlock_add_rule` 445, `landlock_restrict_self` 446, `lsm_get_self_attr` 459, `lsm_set_self_attr` 460, `lsm_list_modules` 461 | 4; `set_thread_area`, `get_thread_area`, `modify_ldt` in 5 |
| Namespaces and mounts (17) | `unshare` 272, `setns` 308, `chroot` 161, `pivot_root` 155, `mount` 165, `umount2` 166, `open_tree` 428, `move_mount` 429, `fsopen` 430, `fsconfig` 431, `fsmount` 432, `fspick` 433, `mount_setattr` 442, `statmount` 457, `listmount` 458, `sysfs` 139, `vhangup` 153 || 4 |
| Keys (3) | `add_key` 248, `request_key` 249, `keyctl` 250 || 4 |
| I/O priority and quotas (4) | `ioprio_set` 251, `ioprio_get` 252, `quotactl` 179, `quotactl_fd` 443 || 4 |
| Tracing and profiling (4) | `ptrace` 101, `perf_event_open` 298, `bpf` 321, `uretprobe` 335 | 4 (Linux errors); `ptrace` future |
| System administration (14) | `syslog` 103, `reboot` 169, `sethostname` 170, `setdomainname` 171, `acct` 163, `swapon` 167, `swapoff` 168, `kexec_load` 246, `kexec_file_load` 320, `init_module` 175, `finit_module` 313, `delete_module` 176, `iopl` 172, `ioperm` 173 || 4 |
| Removed or never implemented in Linux (15) | `uselib` 134, `_sysctl` 156, `create_module` 174, `get_kernel_syms` 177, `query_module` 178, `nfsservctl` 180, `getpmsg` 181, `putpmsg` 182, `afs_syscall` 183, `tuxcall` 184, `security` 185, `vserver` 236, `epoll_ctl_old` 214, `epoll_wait_old` 215, `lookup_dcookie` 212 | 4 (ENOSYS, checked against Debian's kernel) |

## Future work

- **`ptrace`** returns EPERM until there is a way to debug on the Switch 2
  (it would serve gdb, strace and crash reporters). Revisit when console
  debugging access exists.
- **Network namespaces** (`CLONE_NEWNET`) give EPERM. bubblewrap's
  `--unshare-net` (Flatpak apps without network access, Chromium's
  namespace sandbox) needs a namespace with its own loopback device and an
  `AF_NETLINK` route socket to bring it up.
- **Graphics** is in no milestone; it needs its own architecture plan
  (shelved work on `shelf/graphics`).

## Open entries inside milestones 1–3

These are inventoried but not complete:

- **Process, Partial (18):** `clone`, `vfork`, `wait4`, `getrusage`,
  `sysinfo`, `times`, `personality`, `getpriority`, `setpriority`, `prctl`,
  `waitid`, `clone3`, `pidfd_getfd`, `restart_syscall`, `adjtimex`,
  `settimeofday`, `clock_nanosleep`, `clock_adjtime`.
- **Process, Unrouted (2):** `fanotify_init`, `fanotify_mark`.
- **Communication, Partial (8):** `mmap`, `mremap`, `mbind`,
  `process_madvise`, `set_mempolicy_home_node`, `shmat`, `mq_notify`,
  `socket`.
- **Communication, Unrouted (4):** `remap_file_pages`, `userfaultfd`,
  `memfd_secret`, `cachestat`.

The reasons for each are in the inventory rows. Besides syscalls,
`procfs` (`/proc/<pid>/stat`, `/proc/<pid>/fd`), `devfs` and ioctl
operations need their own inventory before coverage is complete.
