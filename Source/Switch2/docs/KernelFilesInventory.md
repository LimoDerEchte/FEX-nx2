# Kernel files inventory (/proc, /sys, /dev)

Milestone 5 ([plan](Milestone5Plan.md)). One row per file, directory or ioctl family. Status: **Routed** (Linux
behavior), **Partial** (served, with the differences listed), **Unrouted** (ENOENT or ENOTTY today).

Where the code lives (`runtime/GuestRuntime.cpp` unless noted):
- **Kernel tree** (`runtime/process/GuestKernelTree.h`, built in `BuildKernelFiles`): every node of `/sys` and
  `/proc/sys`.
- **Per-process files:** `ProcessFile` / `IsProcessFile` and the `/proc/<pid>` listing in `getdents64`.
- **System `/proc` files:** `ProcSystemFile`, `ProcFile`.
- **Links:** `ProcLink`.
- **Devices:** `CharDevAt`.

All generated files `stat` with size 0, `st_dev` of their file system (proc 0:2, sysfs 0:21, cgroup2 0:26) and
seek only with `SEEK_SET`/`SEEK_CUR` (`SEEK_END` is EINVAL, as `seq_lseek`).

## Mounts

| Path | Status | Linux behavior | Differences |
|---|---|---|---|
| `/proc` (proc) | Routed | `nosuid,nodev,noexec,relatime`; new mounts keep the mounter's PID namespace | |
| `/sys` (sysfs) | Partial | `nosuid,nodev,noexec,relatime`; `mount -t sysfs` shows the same tree | Only the nodes listed below exist |
| `/sys/fs/cgroup` (cgroup2) | Partial | cgroup2 root; `mount -t cgroup2` shows the same tree | Read-only (EROFS on write): no controllers |
| `/dev` (devtmpfs), `/dev/shm` (tmpfs), `/dev/pts` (devpts) | Routed | | |

## /proc

| Path | Status | Linux behavior | Differences |
|---|---|---|---|
| `/proc` listing | Routed | `self`, `version`, `mounts`, `sys`, `stat`, `uptime`, `loadavg`, `meminfo`, `cpuinfo`, `filesystems`, `swaps`, `partitions`, `cmdline`, `devices`, `misc`, the pids of the mount's PID namespace | Other Linux files (`interrupts`, `diskstats`, `modules`, ...) do not exist. `/proc/pressure/` does not exist, as Linux without PSI |
| `/proc/self`, `/proc/mounts` | Routed | links | `thread-self`: Task 3 |
| `/proc/<pid>/{stat,cmdline,cgroup}` and `task/<tid>/` | Routed | `cgroup` is `0::/` | |
| `/proc/<pid>/status` and `task/<tid>/status` | Partial | Every line of Linux 5.15 `proc_pid_status` in order (`Name` ... `nonvoluntary_ctxt_switches`); per task for `task/<tid>` | `VmLck` and `VmPin`, `VmPTE`, `VmSwap`, `HugetlbPages`, `CoreDumping` and `THP_enabled` are 0 (no `mlock` page tracking, no page tables, no swap). `VmHWM` is sampled when a `/proc` file is read and before an unmap of 1 MiB or more (no page-fault hook), so it can miss a short peak. `VmPeak` is the largest VMA total since exec. `TracerPid` is 0 (no ptrace). `Speculation_Store_Bypass` is `thread vulnerable`. `Mems_allowed` is `1` / list `0`. Context switch counts are waits entered (a rerun that sleeps again counts again) and preemptions of the thread; `State` is `R` or `S` from the wait state (never `D`) |
| `/proc/<pid>/comm` and `task/<tid>/comm` | Routed | 0644; a write by the process's own thread group sets the name (15 bytes, text up to a newline, as `comm_write`); a write by another process is accepted and ignored | One name per process (the runtime does not name threads separately) |
| `/proc/<pid>/environ` | Routed | 0400; reads the original environment byte range from process memory, including in-place edits and across fork; ptrace check at open | Changes to the location of the process's libc environment array do not change the original range. |
| `/proc/<pid>/auxv` | Routed | 0400; the auxiliary vector the loader placed on the stack, 16-byte pairs up to `AT_NULL`; ptrace check at open | |
| `/proc/<pid>/limits` and `task/<tid>/limits` | Routed | Header and 16 rows of `proc_pid_limits`; values from the process's `RLIMIT_*` table | Readable by everyone, as Linux (the plan listed a ptrace check; Linux has none for this file) |
| `/proc/<pid>/io` and `task/<tid>/io` | Partial | 0400; ptrace check at open; `rchar`, `wchar`, `syscr`, `syscw` count the `read`, `pread64`, `readv`, `preadv`, `preadv2` and write families (and `sendfile`, `copy_file_range` as both), per thread and for the process | `read_bytes`, `write_bytes`, `cancelled_write_bytes` are always 0 (no block layer). Socket `recv`/`send` do not count, as on Linux. The process total never goes down when a thread exits |
| `/proc/<pid>/oom_score_adj`, `oom_adj` | Routed | 0644; -1000..1000 (`oom_adj` -17..15, scaled as `oom_adjust_write`); shared by the thread group, inherited by `fork`, kept by `exec`; lowering below the value an administrator set needs `CAP_SYS_RESOURCE` (EACCES) | The value has no effect: there is no OOM killer |
| `/proc/<pid>/oom_score` | Partial | 0444 | Always `0` (no OOM killer) |
| `/proc/<pid>/wchan` and `task/<tid>/wchan` | Partial | 0444; `0` while running, else the kernel function name; ptrace check at open | The name comes from the blocked system call (`do_wait`, `do_sys_poll`, `core_sys_select`, `ep_poll`, `hrtimer_nanosleep`, `futex_wait_queue_me`, `inet_csk_accept`) or from the kind of wait (`pipe_read`, `n_tty_read`, `locks_lock_inode_wait`, `do_sigtimedwait`, `wait_for_vfork_done`, `sk_wait_data`); other sleeps read as the kind's name |
| `/proc/<pid>/sched` and `task/<tid>/sched` | Partial | 0644; header, 57 dashes and `se.exec_start`, `se.sum_exec_runtime`, `nr_switches`, `nr_voluntary_switches`, `nr_involuntary_switches`, `se.load.weight`, `policy`, `prio`, `clock-delta` | Other CFS fields are not printed. `se.exec_start` is the thread's last guest entry. A write is EACCES (no statistics to reset) |
| `/proc/<pid>/timerslack_ns` and `task/<tid>/timerslack_ns` | Partial | 0644; per thread, default 50000, shared with `prctl(PR_SET_TIMERSLACK/PR_GET_TIMERSLACK)`; `0` restores the default; another task needs `CAP_SYS_NICE` (EPERM) | The runtime's timers do not use the slack: the value is only stored and read back. Inherited by `fork` and `clone` |
| `/proc/<pid>/{loginuid,sessionid}` | Partial | `4294967295` (no audit) | `loginuid` is 0644 but a write is EACCES (nothing records it) |
| `/proc/<pid>/personality` | Routed | 0400, `%08x` of the value `personality(2)` stored; ptrace check at open | |
| `/proc/<pid>/stack` | Partial | 0400; ptrace check at open; read gives EACCES without `CAP_SYS_ADMIN` | Empty for an administrator (no kernel stack to show) |
| `/proc/<pid>/syscall` and `task/<tid>/syscall` | Partial | 0400; ptrace check at open; a blocked call shows its number, six arguments, stack pointer and counter in hex | A task that is not blocked reads `running`, including the reader itself (Linux shows the reader's own `read`) |
| `/proc/<pid>/attr/current` | Partial | 0666; the open succeeds and a read fails with EINVAL (no security module) | The other `attr/` files do not exist |
| `/proc/<pid>/mountstats` | Partial | 0400; one `device <source> mounted on <path> with fstype <type>` line per mount of the process's namespace, from its root; ptrace check at open | No per-file-system statistics lines |
| `/proc/<pid>/children`, `task/<tid>/children` | Partial | Space-separated child pids as the caller numbers them | The runtime does not record which thread forked a child: the main thread lists all children, other threads none |
| `/proc/<pid>/fd/`, `task/<tid>/fd/` | Routed | links | `task/<tid>/{fd,fdinfo,ns,attr}` name the process's own objects |
| `/proc/<pid>/fdinfo/<fd>` | Partial | 0400; ptrace check at open; `pos`, `flags` (with `O_CLOEXEC` from the descriptor and `O_LARGEFILE` for opened paths), `mnt_id`, `ino`; eventfd `eventfd-count`; epoll one `tfd:` line per watch; inotify one `inotify wd:` line per watch; signalfd `sigmask`; timerfd `clockid`, `ticks`, `settime flags`, `it_value`, `it_interval` | The `inotify` line has no file handle part (the runtime has no file handles). `sdev` is the mount's device number (0 where the file system has none), `mnt_id` is 0 for files with no mount. `pos` of epoll targets is 0 for files with no offset |
| `/proc/<pid>/statm` and `task/<tid>/statm` | Routed | Size, resident, shared, text and data counts from guest VMAs | Residency and file versus anonymous accounting follow the same rules as `smaps` |
| `/proc/<pid>/maps` and `task/<tid>/maps` | Routed | Address, protection, sharing, file offset and identity, and mapping name. Ptrace read check at open | Contents are snapshotted at open. No `[vdso]` or `[vvar]` ranges (the runtime has none). `mlock`, `madvise` and `mseal` flags (`lo`, `dc`, `dd`, `wf`, `sl`) are not shown. A System V segment shows its shm id as the inode. An anonymous shared mapping shows `/dev/zero (deleted)` with a made-up inode. The device is 0:N with N the runtime's device number |
| `/proc/<pid>/{smaps,smaps_rollup}` and `task/<tid>/` | Partial | Committed-page accounting, with PSS for shared backing objects. Ptrace read check at open | Dirty state follows the mapping's write permission, not actual writes. PSS divides by the number of live processes mapping the object. All resident pages count as referenced. Lazy-free, huge-page, swap and locked counts are zero. Contents are snapshotted at open |
| `/proc/<pid>/{mountinfo,mounts}` | Routed | ptrace read check at open | |
| `/proc/<pid>/{uid_map,gid_map,setgroups}` | Routed | writable as Linux | |
| `/proc/<pid>/ns/*` | Routed | nsfs links | no time namespaces |
| `/proc/<pid>/{exe,cwd,root}` | Routed | links | |
| `/proc/<pid>/mem`, `task/<tid>/mem` | Routed | 0600; open needs ptrace attach access; `pread`/`pwrite` at the address; EIO at the first unmapped page; 0 bytes after the process exited | |
| `/proc/{stat,uptime,loadavg}` | Routed | | `loadavg` is sampled when read |
| `/proc/meminfo` | Partial | Every line of Linux 5.15 `meminfo_proc_show`, in order. `MemTotal`, `MemFree`, `MemAvailable` from the guest arena; `AnonPages` the anonymous pages of every live process (VMA accounting); `Shmem` and `Cached` the pages of `/dev/shm` files; `CommitLimit` the arena size; `Committed_AS` the arena in use; `Hugepagesize` 2048 kB | Every other field is 0 (`Mapped`, `Buffers`, `Active`, `Slab`, ...). tmpfs mounts on the SD card are SD files, so they add nothing to `Shmem` |
| `/proc/cpuinfo` | Partial | One x86 block per CPU (`HostCpuCount()`) with the lines of `arch/x86/kernel/cpu/proc.c`. Vendor, family, model, stepping, model name, `cpuid level` and `flags` come from the CPUID FEX shows the guest (leaves 0, 1, 7, 0xd.1, 0x80000001, 0x80000008, 0x80000002..4) | Fixed: `microcode` 0x0, `cpu MHz` 1000.000, `bogomips` 2000.00, `cache size` 512 KB, `address sizes` 48/48, one package whose cores are the CPUs, empty `bugs`. `flags` lists the bits that have a name in the table in `GuestProcfs.h` (a subset of `cpufeatures.h`); the synthetic flags Linux adds (`constant_tsc`, `rep_good`, `nopl`, `cpuid`, `tsc_known_freq`, ...) are not printed |
| `/proc/version` | Routed | `Linux version 5.15.0 (fex@switch2) (gcc version 12.2.0) #1 SMP PREEMPT FEX Switch 2 Mon Jan 5 00:00:00 UTC 2026`, the release and version `uname` returns | Fixed build text |
| `/proc/filesystems` | Partial | `nodev` lines for every type `mount(2)` creates plus `devtmpfs`, and `vfat` for the card | Only the types the runtime serves |
| `/proc/swaps`, `/proc/partitions` | Routed | header only (no swap, no block devices) | |
| `/proc/cmdline` | Partial | `BOOT_IMAGE=/fex root=sdmc` | Fixed: the console has no boot command line |
| `/proc/devices` | Partial | Character majors 1 mem, 4 tty, 5 `/dev/tty`, `/dev/console`, `/dev/ptmx`, 13 input, 136 pts, and an empty block section | Lists the majors Task 6 and 8 serve, whether or not a node exists yet |
| `/proc/misc` | Routed | empty | |
| `/proc/sys/kernel/{overflowuid,overflowgid}` | Partial | 0644 | Writes are EACCES (the runtime has no setting to change) |
| `/proc/sys/kernel/{hostname,domainname}` | Routed | 0644; a write sets the caller's UTS namespace name as `sethostname` does (`CAP_SYS_ADMIN` over the namespace's owner, text up to a newline, at most 64 bytes) | |
| `/proc/sys/kernel/{ostype,osrelease}` | Routed | 0444 | |
| `/proc/sys/kernel/{pid_max,threads-max,ngroups_max,cap_last_cap}` | Partial | 32768; the thread table size (32); 65536; 40 | No writers: the runtime's tables are fixed |
| `/proc/sys/kernel/random/{boot_id,uuid,entropy_avail,poolsize}` | Routed | `boot_id` one UUID per launcher start, `uuid` a new random UUID per read, both from the console RNG; 256, 256 | `entropy_avail` and `poolsize` are fixed |
| `/proc/sys/kernel/yama/ptrace_scope` | Partial | `0` | The plan said `1`, but the runtime applies classic ptrace rules only (same uid or `CAP_SYS_PTRACE`), so `1` would claim a restriction it does not enforce. No writer |
| `/proc/sys/kernel/{dmesg_restrict,kptr_restrict,perf_event_paranoid,unprivileged_userns_clone}` | Partial | 1, 1, 2, 1 | No writers (`perf_event_open` is always EPERM, there is no `dmesg`) |
| `/proc/sys/kernel/{shmmax,shmall,shmmni,msgmax,msgmnb,msgmni,sem}` | Partial | The System V IPC limits of `GuestSysvIpc.h` in Linux's formats (`sem` is `SEMMSL SEMMNS SEMOPM SEMMNI`) | 0644 as Linux, but no writers: the limits are compile-time constants |
| `/proc/sys/vm/{overcommit_memory,max_map_count,mmap_min_addr,swappiness}` | Partial | 0, 65530, 65536, 60 | No writers. The runtime does not enforce `max_map_count` |
| `/proc/sys/fs/{file-max,nr_open}` | Partial | 9223372036854775807, 1048576 | No writers |
| `/proc/sys/fs/pipe-max-size` | Partial | 1048576, the limit `F_SETPIPE_SZ` uses | No writer |
| `/proc/sys/fs/inotify/{max_user_watches,max_user_instances,max_queued_events}` | Partial | The inotify layer's limits (8192, 128, 16384) | No writers |
| `/proc/sys/fs/mqueue/{msg_max,msgsize_max,queues_max}` | Partial | The mqueue layer's defaults (10, 8192, 256) | No writers |
| `/proc/sys/net/core/somaxconn` | Partial | 4096 | No writer |
| `/proc/sys/user/max_*_namespaces` | Partial | 0644 | Fixed 126662; writes EACCES |

## /sys

Formatting is in `runtime/process/GuestSysfs.h` (`check-guest-sysfs`); the nodes are added in `BuildSysfs`.

| Path | Status | Linux behavior | Differences |
|---|---|---|---|
| `/sys/{block,bus,class,dev,devices,firmware,fs,kernel,module,power}` | Partial | top-level directories | `block`, `bus`, `firmware`, `module`, `power` stay empty. The input subsystem adds character-device links below `dev/char`. |
| `/sys/devices/system/cpu/{online,possible,present,offline,kernel_max}` | Routed | `0-5` (`HostCpuCount()`), empty line, `5` | |
| `/sys/devices/system/cpu/cpuN/online` | Routed | `1`, absent for cpu0 as on x86 | Writable by mode (0644) but no writer: the cores cannot be taken offline (EACCES) |
| `/sys/devices/system/cpu/cpuN/topology/*` | Partial | `core_id` N, `physical_package_id` 0, `die_id` 0, `core_cpus[_list]` and `thread_siblings[_list]` the cpu itself, `core_siblings[_list]` and `package_cpus[_list]` all cpus, masks in `%*pb` form | Fixed topology: one core per cpu, one package, no SMT. The console reports no cache or cluster layout |
| `/sys/devices/system/cpu/cpuN/cpufreq/{scaling_cur_freq,cpuinfo_max_freq,cpuinfo_min_freq,scaling_governor}` | Partial | kHz values, `performance` | Fixed 1000000 / 500000 kHz (the 1000 MHz of `/proc/cpuinfo`): a guest cannot read the console's clocks. The governor is 0644 without a writer |
| `/sys/devices/system/node/{online,possible,has_cpu,has_memory,has_normal_memory}`, `node0/{cpulist,cpumap,distance,cpuN}`, `cpu/cpuN/node0` | Routed | `0`; `0-5`, `3f`, `10`; links between node and cpus | One node. `node0` has no `meminfo`, `numastat` or `vmstat` |
| `/sys/class/power_supply/{BAT0,ADP1}` | Partial | Symlinks into `/sys/devices/platform/switch2-battery/power_supply/BAT0` and `.../switch2-charger/power_supply/ADP1` | Present only when `psmInitialize` succeeds (initialized once, on first use of the tree); otherwise there are no power supplies (no fake battery) |
| `BAT0/{type,present,technology,status,capacity,capacity_level,uevent}` | Partial | `Battery`, `1`, `Li-ion`, `Charging`/`Discharging`/`Full`/`Not charging`, 0..100 (`psmGetBatteryChargePercentage`), `Critical`/`Low`/`Normal`/`High`/`Full`, `POWER_SUPPLY_*` lines | Read at each open. `status` follows `psmGetChargerType` and the percentage: no charger is Discharging, an under-powered or unsupported charger is Not charging. `capacity_level` thresholds (100 Full, >=80 High, >=16 Normal, >=6 Low, else Critical) are ours; Linux lets each driver choose. No `voltage_now`, `current_now`, `energy_*` |
| `ADP1/{type,online,uevent}` | Partial | `Mains`, `1` when `psmGetChargerType` is not Unconnected | Read at each open |
| `/sys/fs/cgroup/cgroup.procs`, `cgroup.threads` | Routed | pids and tids of every live process, ascending, in the PID namespace of the mount being read | Every process is in the root group. Mode 0644 as Linux; a write fails with EROFS (read-only mount) |
| `/sys/fs/cgroup/{cgroup.controllers,cgroup.subtree_control,cgroup.max.depth,cgroup.max.descendants,cgroup.stat}` | Partial | empty line, empty line, `max`, `max`, `nr_descendants 0` / `nr_dying_descendants 0` | No controllers (deliberate). Writes: EROFS |
| `/sys/kernel/mm/transparent_hugepage/{enabled,defrag}` | Partial | `always [madvise] never`, `always defer defer+madvise [madvise] never` | Fixed; mode 0644 without a writer (EACCES): the runtime has no huge pages |
| `/sys/class/dmi/id` -> `/sys/devices/virtual/dmi/id/{sys_vendor,product_name,board_vendor,bios_vendor,chassis_type,product_uuid}` | Partial | `Nintendo`, `Switch 2`, `Nintendo`, `Nintendo`, `30` (tablet), a UUID (0400, root only) | All fixed (the console has no DMI tables); `product_uuid` is a constant, not a device id |
| `/sys/kernel/osrelease`, `/sys/kernel/random/boot_id` | Unrouted | Do not exist in Linux sysfs | Not added on purpose |

## /dev

| Path | Status | Linux behavior | Differences |
|---|---|---|---|
| `null` 1:3, `zero` 1:5, `full` 1:7, `random` 1:8, `urandom` 1:9 | Routed | 0666 | |
| `tty` 5:0, `ptmx` 5:2, `pts/` | Routed | see `TerminalInventory.md` | |
| `shm/` | Routed | tmpfs | |
| `console` 5:1 | Routed | root:tty 0600; opens the launcher's console terminal (the object the standard streams of an interactive run use; ioctls act on it) | A non-root caller gets EACCES (no CAP_DAC_OVERRIDE). Opening it does not make it a controlling terminal |
| `kmsg` 1:11 | Partial | 0644; a write returns the length and appends nothing; a read open without CAP_SYSLOG is EPERM (`kernel.dmesg_restrict` 1) | There is no kernel log: a read (with CAP_SYSLOG) always fails with EAGAIN, even for a blocking file. A write-only open by a non-root caller is EACCES by mode |
| `fd`, `stdin`, `stdout`, `stderr` | Routed | Symlinks to `/proc/self/fd`, `/proc/self/fd/0`, `/proc/self/fd/1`, `/proc/self/fd/2` | `st_dev` is the devtmpfs |
| `mqueue/` | Partial | The POSIX message queue file system: sticky world-writable directory, one entry per queue of the caller's IPC namespace. `open` (with `O_CREAT` creating a queue of default attributes), `stat`, `unlink` and `getdents` work; the descriptor is the `mq_open` descriptor (reads show `QSIZE:` text) | `rename`, `link`, `truncate`, `chmod` and `utimes` on a queue do not work by path. `st_dev` is the devtmpfs, not an mqueue device. |
| `input/` | Routed | Input device nodes and persistent links | See the input section below. |

## Ioctls

The entry is `Ioctl` (the `ioctl` syscall): `FIOCLEX`/`FIONCLEX`/`FIONBIO` for every file, then the requests of the
file's kind, then `FileIoctl` (regular files and random devices, decisions in `runtime/process/GuestIoctl.h`,
`check-guest-ioctl`), then the terminal requests (`TtyIoctl`, see `TerminalInventory.md`). Anything else is ENOTTY. An
argument pointer is checked before it is written (EFAULT).

| File kind | Requests | Status | Differences |
|---|---|---|---|
| Every file | `FIOCLEX` 0x5451, `FIONCLEX` 0x5450, `FIONBIO` 0x5421 | Routed | `FIONBIO` is `fcntl(F_SETFL, O_NONBLOCK)` |
| Pipes, sockets, inotify, terminals | `FIOASYNC` 0x5452 | Partial | Sets and clears `O_ASYNC` (shown by `F_GETFL`, set by `F_SETFL` too). SIGIO is not sent: there is no `F_SETOWN`. Other files (regular files, eventfd, devices) answer ENOTTY, as files without a `fasync` operation do |
| Regular files | `FIONREAD` 0x541b | Routed | Size minus position (generated `/proc`/`/sys` files report size 0, as Linux). Directories, devices and links: ENOTTY |
| Regular files, directories | `FIOQSIZE` 0x5460 | Routed | The bytes the inode occupies (`i_blocks` times 512, `inode_get_bytes`), not `st_size`; the plan's "file size" read as Linux's behavior |
| Every file | `FIGETBSZ` 2 | Partial | 4096 for every file system |
| Regular files, directories | `FS_IOC_GETFLAGS` 0x80086601 | Partial | Always 0 (no inode flags anywhere) |
| Regular files, directories | `FS_IOC_SETFLAGS` 0x40086602 | Partial | 0 is accepted for the owner or CAP_FOWNER (else EPERM); any other bit is EOPNOTSUPP (deliberate) |
| Regular files | `FICLONE` 0x40049409, `FICLONERANGE` 0x4020940d | Partial | EBADF for a bad source, EXDEV across mounts, EBADF if the destination is not open for writing, EISDIR for a directory, else EOPNOTSUPP (no reflinks) |
| Regular files | `BLKGETSIZE64` 0x80081272 | Routed | ENOTTY: there are no block devices |
| `/dev/random`, `/dev/urandom` | `RNDGETENTCNT` 0x80045200 | Routed | 256 |
| `/dev/random`, `/dev/urandom` | `RNDADDTOENTCNT` 0x40045201, `RNDADDENTROPY` 0x40085203, `RNDZAPENTCNT` 0x5204, `RNDCLEARPOOL` 0x5206 | Partial | EPERM without CAP_SYS_ADMIN, else 0. The data is validated and discarded: the console RNG has no pool |
| Terminals (pty, console, `/dev/tty`) | `TCGETS`, `TCSETS[WF]`, `TCGETS2`, `TCSETS[WF]2`, `TCSBRK`, `TCXONC`, `TCFLSH`, `TIOCGPGRP`, `TIOCSPGRP`, `TIOCGSID`, `TIOCSCTTY`, `TIOCNOTTY`, `TIOCGWINSZ`, `TIOCSWINSZ`, `FIONREAD`, `TIOCOUTQ`, `TIOCGETD`, `TIOCSETD`, `TIOCPKT`, `TIOCGPTN`, `TIOCSPTLCK`, `TIOCGPTLCK`, `TIOCGPTPEER` and the others of `TtyIoctl` | Routed | See `TerminalInventory.md`. On a file that is not a terminal every one of them is ENOTTY |
| Sockets | `FIONREAD` (`SIOCINQ`) 0x541b | Routed | Stream bytes queued, next record size for datagram and seqpacket; a pipe's reader side counts its queue |
| Internet sockets | `FIONREAD` 0x541b, `SIOCOUTQ` 0x5411 | Partial | `SIOCOUTQ` is 0 (the host reports no send queue); a listening socket is EINVAL |
| timerfd | `TFD_IOC_SET_TICKS` 0x40085400 | Routed | |
| epoll | every request | Routed | EINVAL (`ep_eventpoll_ioctl`) |
| inotify | `FIONREAD` 0x541b | Routed | Bytes of queued events |
| Namespace files (`/proc/<pid>/ns/*`) | `NS_GET_USERNS`, `NS_GET_PARENT`, `NS_GET_NSTYPE`, `NS_GET_OWNER_UID` | Routed | See `ContainerInventory.md` |
| `/dev/input/eventN` | `EVIOC*` | Partial | Identity, capabilities, live state, repeat settings, masks, clocks, grab/revoke and force-feedback effects. See the model limits below. |
| Everything else | unknown request | Routed | ENOTTY |

The pure evdev model (`GuestEvdev.h`): `EvdevIoctl` implements `EVIOCGVERSION`, `GID`, `GREP`/`SREP`, `GNAME`/`GPHYS`/`GUNIQ`,
`GPROP`, `GKEY`, `GLED`, `GSND`, `GSW`, `GBIT`, `GABS`/`SABS`, `SFF`, `RMFF`, `GEFFECTS`, `GRAB`, `REVOKE`, `GMASK`/`SMASK` and
`SCLOCKID` on byte buffers. Differences from `evdev.c`: a request of another ioctl type is ENOTTY (Linux says EINVAL); an
unknown `'E'` request is EINVAL; `EVIOCGKEYCODE`/`SKEYCODE` and `EVIOCGMTSLOTS` are not implemented (EINVAL); `EVIOCSFF`
accepts only `FF_RUMBLE`, although `FF_PERIODIC`, `FF_SINE` and `FF_GAIN` are advertised in the `EV_FF` bitmap (as xpad shows
them); `input_estimate_events_per_packet` is approximated (one per axis, one per other event type present); axis values are
not fuzz-filtered; the ring is as `evdev_client` (one slot unused, `SYN_DROPPED` plus the newest event after overflow).

## Input

Task 8a adds the UI-thread HID feed for gamepads, keyboards and mice. It queues timestamped state changes for evdev
translation, sends force-feedback requests back to the console, and exposes per-class guest ownership. Holding
`+` and `−` together for one second returns ownership to the launcher until a guest reopens the device.
Ownership only controls whether the launcher acts on input. Evdev delivers to every open client, including readers
in background tabs, unless a client grabs the device.
Settings > Developer > Gamepad identity persists the Xbox 360 or Switch Pro identity. The default is Xbox 360.
Changing the setting reconnects every connected pad. A physical controller style change also reconnects the device.

The handheld controller uses an available slot among the eight gamepad slots. Horizon handles Home and Capture outside
the npad state. Nmouse supplies vertical scrolling, and the legacy mouse path supplies both wheel axes.

| Path | Status | Linux behavior | Differences |
|---|---|---|---|
| `/dev/input` | Routed | Directory of input character devices, root:root 0755, as are by-id and by-path | Keyboard and mouse nodes are always present. Pad nodes follow their Horizon connections. Input nodes and discovery entries use their connection time. Static input and udev directories use runtime startup time. |
| `/dev/input/eventN` | Partial | `root:input` 0660, major 13, minor 64 + N. Each open description has its own queue. Blocking and nonblocking reads, `poll`, `epoll` and `select` follow evdev. Unplug unlinks the node, wakes waiters and leaves existing descriptions with ENODEV and HUP/ERR. | The RootFS account database supplies the GID. Gamepads also use the seat ACL below. |
| `/dev/input/eventN` access ACL | Partial | systemd-logind applies `uaccess` to joystick nodes, granting the active seat user's UID read and write. `system.posix_acl_access` exposes that entry. Tab changes update access for new opens and leave existing descriptions usable. Keyboards and mice use mode 0660 alone. | The front terminal tab's login user is the sole seat user. There is no logind daemon. The runtime manages this ACL and supports reading and listing it. |
| `/dev/input/eventN` data and ownership | Partial | Complete input events, clock selection, exclusive delivery to the grabber, and force-feedback play and stop. The sine, triangle and square periodic effects drive both rumble motors as in ff-memless. | Loss in the shared 256-change feed causes `SYN_DROPPED` for live clients and state is restored at the oldest retained change. BOOTTIME equals MONOTONIC. Keyboard repeat settings are stored without generating repeats. LED writes and SIGIO delivery are unsupported. The UI applies the final rumble command per effect per frame and samples gain when playback starts. A foreground tab's open captures its input class for the launcher until the release chord, tab switch, close or device removal. |
| `/sys/class/input/{inputM,eventN}` | Routed | Relative links to the input device and its evdev child | Devices live under `/sys/devices/virtual/input/inputM`. Input indices increase independently of reusable evdev minors. |
| `/sys/devices/virtual/input/inputM/{name,phys,uniq,properties}` | Routed | Driver identity strings and the input property bitmap | `phys` matches EVIOCGPHYS, e.g. `switch2/npad0`. Horizon exposes no guest USB topology or serial for these logical devices. |
| `/sys/devices/virtual/input/inputM/id/{bustype,vendor,product,version}` | Routed | Four-digit lowercase hex fields | The same IDs as EVIOCGID. |
| `/sys/devices/virtual/input/inputM/capabilities/{ev,key,rel,abs,msc,led,snd,ff,sw}` | Routed | Linux `input_print_bitmap` with x86-64 word width | Uses the guest evdev capability bits, including the smaller force-feedback set of the modeled pads. |
| `/sys/devices/virtual/input/inputM/{modalias,uevent}` | Partial | Linux input modalias and uevent environment | Readable environment only. Replaying uevents through sysfs is unsupported. |
| `/sys/devices/virtual/input/inputM/eventN/{dev,uevent}` | Partial | `13:<64+N>` and the event device's `MAJOR`, `MINOR` and `DEVNAME` | Replaying uevents through sysfs is unsupported. |
| `/sys/devices/virtual/input/inputM/{subsystem,eventN/subsystem,eventN/device}`, `/sys/dev/char/13:<64+N>` | Routed | Links to the input class, the parent input device and the evdev device by number | All links resolve within the generated sysfs tree. |
| `/dev/input/by-id/usb-<serial>-event-<class>` | Partial | Persistent links to `../eventN` using `60-persistent-input.rules` names | Serial derives from the emulated vendor/product IDs, plus the npad slot for controllers. |
| `/dev/input/by-path/platform-switch2-<slot>-event-<class>` | Partial | Persistent links to `../eventN` by physical path | The path derives from the logical console slot in `phys`. |
| `/run/udev/data/c13:<64+N>` | Partial | libudev device database with devlinks, input class, serial, path and initialization time | Read-only virtual mount under the RootFS `/run`. Joystick event records carry upstream seat and uaccess tags. The runtime applies the seat ACL directly. No hwdb, tag index or daemon. |
| `/run/udev/control` | Unrouted | udev daemon control socket | The runtime lacks udev control and AF_NETLINK. |
| inotify watch on `/dev/input` | Routed | Named `IN_CREATE` on arrival and `IN_ATTRIB` then `IN_DELETE` on removal | Discovery metadata is published with the node and removed with it. The scheduler may coalesce transient idle connections between snapshots. |

SDL releases that fall back when the udev monitor fails use inotify on `/dev/input`. Releases such as SDL 2.26.5
require `SDL_JOYSTICK_DISABLE_UDEV=1` to select that backend. Libudev can look up the static data for live devices.
The libinput path backend can use this data. Its udev-seat backend needs an AF_NETLINK monitor.
