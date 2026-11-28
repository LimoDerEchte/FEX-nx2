# Milestone 5: /proc, /sys, /dev and input — implementation plan

Status: plan written 2026-11-27. Not started. Order of milestones (user, 2026-11-27): M5 this plan, M6 non-PIE
executables, M7 Wayland compositor, M8 graphics acceleration, M9 32-bit guests.

Goal: give desktop programs, Steam and games the kernel files they read to learn about the system and themselves, the
common `/dev` nodes and ioctls, and controller, keyboard and mouse input through evdev. Graphics (DRM, display, GPU
ioctls) is out of scope.

This plan is written so that each task can be handed to an implementer **who has never seen this project** (Codex). Read
sections 1 to 3 completely before starting any task; they are part of every task.

---

## 1. Who does what (user decision 2026-11-27)

| Task | Owner | Why |
|---|---|---|
| 1. Kernel file tree and `/sys` mount | Opus (Claude) | Rewires the path walk, `stat`, `open`, `getdents` and `readlink` for all virtual files; it can only be verified on the console. |
| 2. Memory map records and `maps`/`smaps` | Codex | Self-contained data structure with host unit tests, plus calls at the listed memory handlers. |
| 3. Per-process `/proc` files | Codex | Formatting from existing state, one generator per file. |
| 4. System `/proc` files and `/proc/sys` | Codex | Formatting from existing state. |
| 5. `/sys` tree | Codex | Formatting plus two console services (psm). |
| 6. `/dev` nodes and generic ioctls | Codex | Small, listed behaviors. |
| 7. Evdev model (pure, host-tested) | Codex | Self-contained header with unit tests; no console code. |
| 8. Input integration (HID feed, hot-plug, rumble) | Opus (Claude) | Threads, launcher UI ownership of input, console-only behavior. |

Then (Claude): one whole-milestone review, fixes, the guest test suite `KernelFilesSuite.c`, console runs and the commit.
Implementers never commit, never bump the launcher version and never edit `NextMilestone.md`.

Tasks run **in order, one at a time**: 1, 2, 3, 4, 5, 6, 7, 8. Each task starts from the tree the previous one left.

**Update 2026-11-28 (user).** Codex is not used: it was tried on Task 2 and stopped. Sonnet agents implement, grouped
to save tokens:
- Agent A: Task 2 (it also reviews Codex's partial work);
- Agent B: Tasks 3 and 4;
- Agent C: Tasks 5, 6 and 7.

Task 8 is Claude's.

**Report.** Each implementer writes `.superpowers/sdd/m5-task-<N>-report.md` (repository root `.superpowers/`,
git-ignored): files changed, what was done, commands run with their output summary, deviations from this plan, open
questions. If a task needs a design decision this plan does not make, **stop and write the question in the report**
instead of guessing.

---

## 2. Project context for an implementer with no history

**What this is.** A fork of FEX (an x86-64 emulator for ARM64) that runs unmodified x86-64 Linux programs on the
Nintendo Switch 2 (Horizon OS, not Linux). There is no Linux kernel on the console: every Linux system call the guest
program makes is implemented in our own runtime. The guest's root file system (Debian or Ubuntu) lives on the SD card.

**Repository layout** (repository root `/home/limo/Documents/FEX`, branch `nx2`; all our code is under `Source/Switch2`):

- `Source/Switch2/runtime/GuestRuntime.cpp` (~19,800 lines): the syscall implementation. One class, `GuestSyscalls`,
  holds all runtime state. Syscall handlers are its member functions.
- `Source/Switch2/runtime/process/`, `runtime/fs/`, `runtime/ipc/`, `runtime/memory/`: small **pure** headers
  (namespace `Switch2Guest`, no Horizon calls) that hold logic and formatting, each with a host unit test in
  `Source/Switch2/tests/unit/<Name>Test.cpp`. Example: `runtime/process/GuestProcfs.h` formats `/proc/<pid>/stat`,
  `/proc/meminfo`, `/proc/stat`, `/proc/cpuinfo`; its test is `tests/unit/GuestProcfsTest.cpp`.
- `Source/Switch2/ui/`: the launcher UI (SDL2). `ui/platform/switch2/Switch2Platform.cpp` reads controllers,
  keyboard and mouse with the libnx-style `hid` API.
- `Source/Switch2/tests/guest/`: x86-64 C test programs run inside the guest (written by Claude for this milestone).
- `Source/Switch2/docs/`: plans and inventories (`ProcessInventory.md`, `FilesystemInventory.md`,
  `ContainerInventory.md`, ...).

**Linux version we imitate.** `uname -r` returns `5.15.0` (`UnameRelease` in `GuestRuntime.cpp`). Every generated file
uses the Linux 5.15 format (field order, names, spacing). The reference is the Linux 5.15 source: `fs/proc/array.c`,
`fs/proc/base.c`, `fs/proc/task_mmu.c`, `fs/proc/meminfo.c`, `arch/x86/kernel/cpu/proc.c`, `drivers/input/evdev.c`, etc.

**Errors.** Handlers return `int64_t`: the result, or a negative Linux errno (`-2` ENOENT, `-13` EACCES, `-1` EPERM,
`-22` EINVAL, `-25` ENOTTY, `-95` EOPNOTSUPP, `-14` EFAULT, `-9` EBADF, `-11` EAGAIN, `-19` ENODEV, `-30` EROFS).
Some have names in the class (`Invalid`, `Fault`, `BadFile`).

**Guest memory.** Guest addresses are host pointers. Never dereference a guest pointer before checking it:
`Available(Address, PF_R or PF_W)` returns how many bytes are accessible; `CopyOut(Address, Value)` writes a value
and returns 0 or `Fault`. Follow how nearby handlers do it.

**Threads and locking.** Every guest thread runs on its own host thread. All syscall handlers run while holding one
runtime mutex (`RuntimeLock`), so handler code sees consistent state and needs no extra locking **between handlers**.
Rules:
- Never block (sleep, wait on I/O) inside a handler. A call that must wait records a wait and returns through the
  existing wait mechanism (see how `read` on a pipe waits); ask in the report if you need this.
- Code that runs on **another** host thread (the launcher UI thread, a service thread) must not touch `GuestSyscalls`
  state; it hands data over through its own mutex-protected structure (Task 8 defines one for input).
- `MapLock` guards the guest memory maps for FEX's compiler; memory handlers already take it where needed. Keep the
  existing pattern when you add records in Task 2.

**Processes and namespaces.**
- `Processes` is a `std::map<uint32_t, GuestProcess>` keyed by global PID. `Threads` is an array of `GuestThread`
  slots; a live slot has `Core` set.
- `Task` is the calling process, `Current` the calling thread.
- PID namespaces: `/proc` shows numbers in the namespace of the proc mount; use `ViewOut(GlobalPid)` for every pid or
  tid you print, and skip entries that return 0.
- Credentials: `Process.Creds` (`Uid[4]`, `Gid[4]` as real, effective, saved, fs). `MayPtrace(Target)` implements
  Linux's `ptrace_may_access` for the caller.

**How virtual files work** (after Task 1, done 2026-11-27; read this before Tasks 2-6):
- `/proc`, `/dev` and `/sys` are virtual. `VirtualMount(Path)` returns 1 for `/proc`, 2 for `/dev`, 3 for `/sys`.
- **The kernel tree** (`runtime/process/GuestKernelTree.h`, filled in `GuestSyscalls::BuildKernelFiles`) holds every
  node of **`/sys` and `/proc/sys`**: directories, files (`KernelFileData::Read`, a `std::function<std::string()>`
  that returns the contents at open time, and an optional `Write`, `std::function<int64_t(std::string_view)>`) and
  symlinks (`KernelFileData::Target`).
  - To add a file there, add one `File(path, mode, reader)` call in `BuildKernelFiles`, or `T.Add(path, Kind::Link,
    0777, {{}, {}, "target"})` for a link.
  - Parents are created automatically. Directory listings, `stat`, `open`, `readlink`, `statfs` and the path walk
    all use the tree. Nothing else needs to change.
  - A writable file has mode 0644 (root writes) or 0666 and a `Write` that returns the bytes consumed or a negative
    errno. The open checks the mode and returns EACCES when there is no writer.
  - Device nodes that come and go (Task 8) use `KernelFiles().Add` and `KernelFiles().Remove` at run time.
- **Per-process files** (`/proc/<pid>/...` and `/proc/<pid>/task/<tid>/...`):
  1. Add the name to `IsProcessFile`.
  2. Add a branch to `ProcessFile(Process, Thread, Name)` returning the contents.
  3. Add the name to the two per-process listings in `getdents64` (search for `{"cgroup", false}`).
  4. Files that need a ptrace check at open go next to the `mountinfo` check in the open path (search for
     `proc_mountinfo_open`).
  5. Their modes come from `ProcFileMode`.
- **System `/proc` files** (`/proc/cpuinfo` etc.): `ProcSystemFile(Name)` and `ProcFile(Name)`, plus the `/proc`
  listing in `getdents64` (search for `{"cpuinfo", false}`).
- `ProcLink(Path)` gives `/proc` symlink targets (`/proc/self`, `exe`, `cwd`, `root`, `fd/<n>`).
- `CharDevAt(Path)` names the `/dev` character devices.
- Contents are generated at `open` and stored in `OpenFile::Contents`; `read` works on that snapshot. `stat` reports
  size 0 and `SEEK_END` is EINVAL, as on Linux.

**Building and testing.**
- Host unit tests: `make -C Source/Switch2 check-guest-<name>` (each has a target in `Source/Switch2/Makefile`; copy
  the `check-guest-procfs` pattern, which builds `tests/unit/GuestProcfsTest.cpp` with `-Wall -Wextra -Werror`).
- Console build: `cmake --build Source/Switch2/build/guest --target guest_nro`. It must build with **0 warnings**. It
  takes several minutes. Do not create another build directory.
- Your editor's clang diagnostics on `GuestRuntime.cpp` (`'SdPath.h' file not found` etc.) are false: that file only
  builds through the console toolchain. Trust the `cmake --build` result.
- You cannot run the console build. The guest test suite and console runs are Claude's job after you finish.

**Style.**
- C++20, 2-space indent, `PascalCase` names (functions, variables, members), 120-column lines.
- Comments are full sentences that say what Linux does and what we do. Name the Linux function you imitate
  (`// proc_pid_status`).
- Every value the console cannot know is a **fixed, plausible value listed as a deliberate difference** in the
  inventory, never invented per run.
- No faked behavior: a file either has its real content, or its fixed values are documented as deliberate differences.

---

## 3. Global constraints (every task)

1. Follow `AGENTS.md` at the repository root (real semantics, root causes, no commits, no version bump).
2. Pure logic and formatting go into a `Switch2Guest` header with a host unit test and a `check-guest-*` make target.
   `GuestRuntime.cpp` only gathers state and calls the header.
3. Generated files reflect live state at the time of `open`.
4. Use Linux 5.15 formats exactly: field order, spacing, tabs, units, trailing newline.
5. `stat` on a generated `/proc` or `/sys` file returns size 0, `st_dev` of its mount and the mode Linux uses (0444
   read-only, 0644 writable, 0200 write-only). Directories 0555, symlinks 0777.
6. Access to another process's files follows Linux:
   - `environ`, `auxv`, `mem`, `maps`, `smaps`, `smaps_rollup`, `fd/`, `fdinfo/`, `io`, `wchan`, `limits` need
     `MayPtrace(Target)` (EACCES otherwise; `maps` and `smaps` check at open).
   - `stat`, `status`, `statm`, `cmdline`, `comm` are readable by all.
7. Writes to files Linux lets root change are allowed only where the runtime has the behavior. Otherwise:
   - EACCES for non-root, by mode;
   - EPERM or EINVAL for root, as Linux does for that file.
8. Every task updates `Source/Switch2/docs/KernelFilesInventory.md` (Task 1 creates it). It has one row per file,
   directory or ioctl family, with these columns: path, status (Routed / Partial / Unrouted), Linux behavior,
   deliberate differences.
9. All existing `check-guest-*` targets still pass. `guest_nro` builds with 0 warnings.

---

## 4. Tasks

### Task 1 (Opus): kernel file tree and the `/sys` mount

**Implemented 2026-11-27 (host checks and `guest_nro` pass; console pending).** As built:
- the kernel tree serves `/sys` and `/proc/sys` (see section 2);
- per-process files stay in `ProcessFile`, with the steps listed in section 2;
- `/sys` (sysfs, device 0:21) and `/sys/fs/cgroup` (cgroup2, device 0:26, read-only) are mounted, and
  `mount -t sysfs/cgroup2` shows the same tree;
- generated files `stat` with size 0, and `SEEK_END` on them is EINVAL;
- `/proc/<pid>/mem` and `task/<tid>/mem` exist;
- `docs/KernelFilesInventory.md` is created.

The original task text follows.

Replace the scattered `/proc` and `/dev` checks with one lookup, so later tasks add a file by adding one entry.

- `runtime/process/GuestKernelTree.h` (pure): `struct KernelNode { Kind (Directory, File, Link, CharDevice);
  Mode; Rdev; Writable; }` and a path-pattern table. Pattern components are literal names, `<pid>`, `<tid>`, `<fd>`
  and `<n>` (a decimal index). Directory listings come from the table plus dynamic children (pids, tids, fds, pty
  numbers, input devices).
- `GuestRuntime.cpp`: one `KernelLookup(GuestPath)` used by the path walk, `stat`/`statx`, `open`, `getdents64`,
  `readlink`, `access`, `statfs` and `inotify_add_watch` for every virtual path.
  - It returns the node plus a generator, `std::function<std::optional<std::string>()>` for a file or `std::string()`
    for a link, bound to the process and thread the path names.
  - Writable files also get a writer, `int64_t(std::string_view Data)`.
- Existing behavior stays identical except one deliberate fix: generated files `stat` as size 0. This includes ns
  links, the uid_map writers, pty nodes, the ptrace check on `mountinfo` and `nodev`.
- Add the virtual `/sys` mount: `VirtualMount` returns 3, device `SysDevice` (next free small number), and the initial
  mount table gets these mounts. Bubblewrap's own `sysfs` mounts must show the same tree.
  - `sysfs` at `/sys`, flags `nosuid,nodev,noexec,relatime`.
  - `cgroup2` at `/sys/fs/cgroup`, read-only, `nosuid,nodev,noexec,relatime`.
- Add `/proc/<pid>/mem`:
  - Open requires `MayPtrace` in attach mode.
  - `pread` and `pwrite` follow `process_vm_readv` and `process_vm_writev` (fork groups whose private pages are
    swapped out must be read from the saved copy).
  - It is EIO beyond the mapped pages.
- Creates `docs/KernelFilesInventory.md` with every file served today.
- Verify: `check-guest-procfs`, a new `check-guest-kernel-tree`, `guest_nro`, then the console final pass (proc,
  container and pty suites unchanged).

### Task 2 (Codex): memory map records, `maps`, `smaps`, `smaps_rollup`, `statm`

Today each process's memory is tracked as page ranges with flags (`runtime/memory/GuestAddressSpace.h`, class
`GuestAddressSpace`, per-page flags), but not **which file** a range maps. `/proc/<pid>/maps` needs that.

1. New pure header `runtime/memory/GuestVmaList.h`:
   - `struct Vma { uint64_t Start, End; uint8_t Prot /*PROT_READ=1, WRITE=2, EXEC=4*/; bool Shared; uint64_t Offset;
     uint32_t DevMajor, DevMinor; uint64_t Inode; std::string Name; uint8_t Special /*None, Heap, Stack, Vvar, Vdso*/; }`.
   - `class VmaList` with `Map(Vma)`, `Unmap(Start, Length)`, `Protect(Start, Length, Prot)`,
     `Remap(OldStart, OldLength, NewStart, NewLength)` and `Find(Address)`.
   - Ranges never overlap. `Map` over an existing range replaces it. `Unmap` and `Protect` split ranges as Linux
     splits VMAs.
   - Adjacent anonymous ranges with equal properties merge, as Linux `vma_merge` does for anonymous private memory;
     file ranges merge only when their offsets are contiguous.
   - Unit test `tests/unit/GuestVmaListTest.cpp`, target `check-guest-vma-list`: splits at both ends, a hole in the
     middle, protect of part, remap grow/shrink/move, merge rules, a `Map` replacing several ranges.
2. Formatting in the same header:
   - **`FormatMaps(const VmaList&)`**, Linux `show_map_vma`:
     - Prefix `"%08lx-%08lx %c%c%c%c %08llx %02x:%02x %lu "`, with flags `r`/`-`, `w`/`-`, `x`/`-` and `s` (shared)
       or `p` (private).
     - When there is a name, pad with spaces to column 73 (`25 + 8*6 - 1` characters) and append it.
     - Names: the file path, `[heap]`, `[stack]`, `[vvar]` or `[vdso]`; nothing for anonymous memory.
     - A deleted file gets `" (deleted)"`. A memfd is named `/memfd:<name> (deleted)`.
   - **`FormatSmaps`**, Linux 5.15 `show_smap`: for each range the maps line, then these fields in this order:

     ```
     Size, KernelPageSize, MMUPageSize, Rss, Pss, Shared_Clean, Shared_Dirty, Private_Clean, Private_Dirty,
     Referenced, Anonymous, LazyFree, AnonHugePages, ShmemPmdMapped, FilePmdMapped, Shared_Hugetlb,
     Private_Hugetlb, Swap, SwapPss, Locked
     ```

     - Each field is the name with its colon, padded to 16 characters, then `%8lu kB`.
     - Then `THPeligible:    0` and the `VmFlags:` line (`rd wr ex sh mr mw me ms gd ac`, each followed by a
       space, as `show_smap_vma_flags`).
     - **Accounting rule (decided 2026-11-28, answer to the Task 2 report question).** The runtime has no per-page
       write history and cannot get one: there are no write faults on the console (`docs/Milestone3Sdk.md`), and
       instrumenting guest stores is out of scope. Do **not** add dirty tracking. Derive every field from the
       range's kind and its committed pages (`GuestAddressSpace::PageCommitted`):

       | Range | Counted as |
       |---|---|
       | private, writable (anonymous, `[heap]`, `[stack]`, writable private file mapping) | `Private_Dirty`; `Anonymous` = the same pages |
       | private, read-only (code and read-only data of files) | `Private_Clean` |
       | shared (`MAP_SHARED` file or memfd, System V shm), writable | `Shared_Dirty` when another process maps the same object, else `Private_Dirty` |
       | shared, read-only | `Shared_Clean` when another process maps the same object, else `Private_Clean` |

       - `Rss` is the committed pages.
       - `Pss` is `Rss` for private ranges. For a shared range it is `Rss` divided by the number of live processes
         whose `VmaList` maps the same object: the same device and inode, or the same System V shm id. Linux shifts
         the counts left by 12 (`PSS_SHIFT`) and divides per page; do the same, rounding as `smaps_account` does.
       - `Referenced` = `Rss`. `LazyFree`, `AnonHugePages`, the `*PmdMapped` and `*_Hugetlb` fields, `Swap`,
         `SwapPss` and `Locked` are 0.
       - List "dirty state follows the mapping's write permission, not actual writes" as a deliberate difference in
         `KernelFilesInventory.md`.
   - **`FormatSmapsRollup`**, Linux `show_smaps_rollup`:
     - First line `"%08lx-%08lx ---p 00000000 00:00 0"` padded to column 73, then `[rollup]`, spanning the lowest
       start to the highest end.
     - Then `Rss`, `Pss`, `Pss_Anon`, `Pss_File`, `Pss_Shmem`, `Shared_Clean` ... `Locked`, the same field list as
       smaps without `Size`, `KernelPageSize`, `MMUPageSize`, `THPeligible` and `VmFlags`.
3. Keep one `VmaList` per address space (per process; a `fork` child copies it; `execve` resets it). Update it in:
   - `MapGuest`, `UnmapGuest`, `ProtectGuest`, `RemapGuest`;
   - `ShmAttachGuest`, `ShmDetachGuest`;
   - the `brk` handler (the `[heap]` range);
   - the ELF loader (`runtime/GuestElf.cpp`: each `PT_LOAD` segment with the file's path, device and inode, and the
     interpreter's segments);
   - the initial stack (`[stack]`, the range the loader sets up);
   - FEX's own internal allocations must **not** appear (only guest-requested mappings).
4. Serve `/proc/<pid>/maps`, `smaps`, `smaps_rollup` (ptrace read check at open) and the same files under
   `task/<tid>/`.
5. Make `/proc/<pid>/statm` and the `Vm*`/`Rss*` lines Task 3 needs use the `VmaList` totals.
6. Inventory rows.

Done when: `check-guest-vma-list` and `check-guest-procfs` pass, and `guest_nro` builds with 0 warnings.

### Task 3 (Codex): per-process `/proc` files

Serve these under `/proc/<pid>/` and, where Linux has them per thread, `/proc/<pid>/task/<tid>/`. Pure formatting goes
in `runtime/process/GuestProcfs.h` with new cases in `tests/unit/GuestProcfsTest.cpp`.

- **`status`.** Complete it to Linux 5.15 `proc_pid_status`. Lines in this order:

  ```
  Name, Umask, State, Tgid, Ngid, Pid, PPid, TracerPid, Uid, Gid, FDSize, Groups, NStgid, NSpid, NSpgid, NSsid,
  VmPeak, VmSize, VmLck, VmPin, VmHWM, VmRSS, RssAnon, RssFile, RssShmem, VmData, VmStk, VmExe, VmLib, VmPTE,
  VmSwap, HugetlbPages, CoreDumping, THP_enabled, Threads, SigQ, SigPnd, ShdPnd, SigBlk, SigIgn, SigCgt, CapInh,
  CapPrm, CapEff, CapBnd, CapAmb, NoNewPrivs, Seccomp, Seccomp_filters, Speculation_Store_Bypass,
  Cpus_allowed, Cpus_allowed_list, Mems_allowed, Mems_allowed_list,
  voluntary_ctxt_switches, nonvoluntary_ctxt_switches
  ```

  - Keep the existing values. Read `ProcStatus` first and only add what is missing.
  - Vm values come from Task 2. Peak and HWM are the highest values seen since `exec`; track them.
  - `Speculation_Store_Bypass:` is `thread vulnerable`.
  - `Cpus_allowed` comes from the thread's affinity mask (`GuestThread::CpuMask`), `Mems_allowed` is `1` and its list
    is `0`.
- **`cmdline`.** Keep. **`environ`**: the environment strings the process was started with, NUL-separated, as stored at
  `execve` (find where `cmdline` gets its strings and store the environment the same way).
- **`comm`.** Make it writable by the process itself and by others in its thread group: the write sets the name
  (truncated to 15 bytes, newline stripped), as `comm_write`. It is mode 0644.
- **`auxv`.** The auxiliary vector the loader placed on the stack, as 16-byte pairs up to and including `AT_NULL`.
- **`limits`.** Linux `proc_pid_limits`:
  - Header `"%-25s %-20s %-20s %-10s\n"` with `Limit`, `Soft Limit`, `Hard Limit`, `Units`.
  - Then 16 rows, names as Linux: `Max cpu time` seconds, `Max file size` bytes, `Max data size` bytes, ... `Max
    realtime timeout` us.
  - Each value is `%-20s` "unlimited" or `%-20lu`, then the units as `%-10s` or nothing.
  - Values come from `Process.Limits`.
- **`fd/` and `fdinfo/<fd>`.**
  - Base lines `pos:\t%lli\nflags:\t0%o\nmnt_id:\t%i\nino:\t%lu\n`. The flags include `O_CLOEXEC` (02000000) when
    the descriptor has it.
  - eventfd adds `eventfd-count: %16llx\n`.
  - epoll adds one line per watched fd: `tfd: %8d events: %8x data: %16llx  pos:%lli ino:%lx sdev:%x\n`.
  - inotify adds per watch `inotify wd:%x ino:%lx sdev:%x mask:%x ignored_mask:0\n`. The fhandle part is a
    deliberate difference.
  - signalfd adds `sigmask:\t%016llx\n`.
  - timerfd adds:

    ```
    clockid: %d
    ticks: %llu
    settime flags: 0%o
    it_value: (%llu, %llu)
    it_interval: (%llu, %llu)
    ```

  - `fdinfo` of another process needs `MayPtrace`.
- **`oom_score_adj`.** Read/write, -1000..1000:
  - Lowering it below the current minimum needs CAP_SYS_RESOURCE (EACCES otherwise);
  - it is shared by the thread group and inherited on fork;
  - `oom_adj` is the legacy scaled view;
  - `oom_score` reads `0` (deliberate: the console has no OOM killer).
- **`io`.**

  ```
  rchar, wchar, syscr, syscw, read_bytes, write_bytes, cancelled_write_bytes
  ```

  - Linux format `%s: %llu`.
  - Count `rchar`/`wchar`/`syscr`/`syscw` in the read/write family handlers, per thread, summed for the process.
  - `read_bytes` and `write_bytes` are 0 (deliberate: no block layer).
- **`wchan`.** `0` while running. While waiting it is the Linux symbol name for the wait kind: `futex_wait_queue_me`
  for a futex, `do_wait` for wait4, `pipe_read`, `hrtimer_nanosleep`, `do_sys_poll`, `ep_poll`, `n_tty_read` (map
  each `WaitKind` once, in a table).
- **`sched`.** Linux `proc_sched_show_task` header and the fields `se.exec_start`, `se.sum_exec_runtime`,
  `nr_switches`, `nr_voluntary_switches`, `nr_involuntary_switches`, `se.load.weight`, `policy`, `prio`,
  `clock-delta`.
  - Values come from thread CPU time (`ThreadCpuNs`), the policy and the nice value stored by `sched_setscheduler` and
    `setpriority`.
- **`timerslack_ns`.** Read/write (default 50000; per thread; `prctl(PR_SET_TIMERSLACK)` and this file share the value).
- **`cgroup`.** Keep (`0::/`). **`ns/`**, **`uid_map`** etc.: keep.
- **Also serve** `loginuid` (`4294967295`), `sessionid` (`4294967295`), `personality` (`%08x`, from the stored
  personality), `stack` (EACCES: needs CAP_SYS_ADMIN and is empty), `syscall` (`running` or the syscall number and
  six arguments while blocked in one, ptrace check), `attr/current` (EINVAL: no LSM) and `mountstats` (one line per
  mount: `device <src> mounted on <path> with fstype <type>`).
- **`task/<tid>/`** gets `stat`, `statm`, `status`, `comm`, `cmdline`, `environ`, `auxv`, `maps`, `smaps`, `io`,
  `sched`, `wchan`, `fd`, `fdinfo`, `ns`, `cgroup`, `limits` and `children` (space-separated child pids of that thread).

Done when: `check-guest-procfs` covers each new formatter (exact expected strings), and `guest_nro` builds with 0
warnings. Inventory rows.

### Task 4 (Codex): system `/proc` files and `/proc/sys`

Formatting in `runtime/process/GuestProcfs.h` with unit tests. The `/proc/sys` nodes are added in `BuildKernelFiles` (section 2); pure value logic (parsing writes, limits) goes in a new `runtime/process/GuestSysctl.h` with `check-guest-sysctl`.

- **`/proc/cpuinfo`.** Use the x86 format of `arch/x86/kernel/cpu/proc.c`, one block per CPU from `HostCpuCount()`,
  with these lines:

  ```
  processor, vendor_id, cpu family, model, model name, stepping, microcode, cpu MHz, cache size, physical id,
  siblings, core id, cpu cores, apicid, initial apicid, fpu, fpu_exception, cpuid level, wp, flags, bugs,
  bogomips, clflush size, cache_alignment, address sizes, power management
  ```

  Tabs as Linux. Values come from the CPUID that FEX exposes to the guest, through
  `Context->RunCPUIDFunction(Leaf, Subleaf)` (`FEXCore/include/FEXCore/Core/Context.h`):
  - vendor from leaf 0;
  - family, model and stepping from leaf 1;
  - the model name from 0x80000002..4;
  - `flags`: every set bit of leaves 1, 7, 0x80000001, 0xd (sub 1) and 0x80000008 that Linux names in
    `arch/x86/include/asm/cpufeatures.h`, in Linux's order (put the name table in the header).
  - Fixed values: `cpu MHz` 1000.000, `bogomips` 2000.00, `cache size` 512 KB, `address sizes` `48 bits physical,
    48 bits virtual`. List them as deliberate differences.
- **`/proc/meminfo`.** All Linux 5.15 fields in order, adding the missing ones:
  - `MemTotal`, `MemFree`, `MemAvailable` from the guest arena (existing).
  - `Cached` and `Shmem` from tmpfs and `/dev/shm` pages.
  - `AnonPages` from Task 2 totals.
  - `Hugepagesize: 2048 kB`. The remaining fields are 0.
- **`/proc/version`.** `Linux version 5.15.0 (fex@switch2) (gcc version 12.2.0) #1 SMP PREEMPT` plus a fixed date.
  - Today it says 5.0.0, which disagrees with `uname`. It must match `UnameRelease` and `uname -v`. Find where
    `uname` builds its version string and use the same constant.
- **Other files:**
  - `/proc/filesystems`: `nodev\tsysfs`, `nodev\ttmpfs`, `nodev\tproc`, ... for every type `mount` accepts, plus
    `\tvfat` for the card.
  - `/proc/swaps`: the header only.
  - `/proc/partitions`: the header and an empty line.
  - `/proc/cmdline`: `BOOT_IMAGE=/fex root=sdmc\n`, a deliberate difference.
  - `/proc/devices`: the character majors we serve (1 mem, 4 tty, 5 /dev/tty, 5 /dev/console, 5 /dev/ptmx, 13 input,
    136 pts) and an empty block section.
  - `/proc/misc`: empty.
  - `/proc/pressure/` must not exist (ENOENT, as Linux without PSI).
  - `/proc/self/mounts` and `/proc/mounts`: keep.
- **`/proc/sys/`.** Read-only unless stated. Writable means: root can write, the value changes behavior, and invalid
  values give EINVAL.

  | Path | Value |
  |---|---|
  | `kernel/osrelease`, `ostype`, `hostname`, `domainname` | keep (namespace-aware, already served) |
  | `kernel/pid_max` | 32768 |
  | `kernel/threads-max` | the slot count limit |
  | `kernel/random/boot_id` | a UUID made once per launcher start from the console RNG |
  | `kernel/random/uuid` | a new UUID per read |
  | `kernel/random/entropy_avail` | 256 |
  | `kernel/random/poolsize` | 256 |
  | `kernel/yama/ptrace_scope` | 1 |
  | `kernel/dmesg_restrict` | 1 |
  | `kernel/kptr_restrict` | 1 |
  | `kernel/perf_event_paranoid` | 2 |
  | `kernel/unprivileged_userns_clone` | 1 |
  | `kernel/ngroups_max` | 65536 |
  | `kernel/cap_last_cap` | 40 |
  | `kernel/shmmax`, `shmall`, `shmmni`, `msgmax`, `msgmnb`, `msgmni`, `sem` | from the System V IPC limits in `runtime/ipc/GuestSysvIpc.h` (writable where Linux allows) |
  | `vm/overcommit_memory` | 0 |
  | `vm/max_map_count` | 65530 |
  | `vm/mmap_min_addr` | 65536 |
  | `vm/swappiness` | 60 |
  | `fs/file-max` | 9223372036854775807 |
  | `fs/nr_open` | 1048576 |
  | `fs/pipe-max-size` | 1048576, writable if the pipe code uses a limit |
  | `fs/inotify/max_user_watches`, `max_user_instances`, `max_queued_events` | the inotify layer's limits |
  | `fs/mqueue/msg_max`, `msgsize_max`, `queues_max` | the mqueue layer's limits |
  | `net/core/somaxconn` | 4096 |
  | `user/max_*_namespaces` | keep |

  Directory listings list exactly these.

Done when: unit tests cover cpuinfo (with a fake CPUID input), meminfo field order, the sysctl table and writes. Then
`guest_nro`. Inventory rows.

### Task 5 (Codex): the `/sys` tree

The nodes are added in `BuildKernelFiles` (section 2). Pure formatting (topology lists, cpu masks, battery text, cgroup files) goes in `runtime/process/GuestSysfs.h` with a unit test (`check-guest-sysfs`).

- **`/sys/devices/system/cpu/`:**
  - `online`, `possible`, `present` (`0-5`, from `HostCpuCount()`), `kernel_max`, `offline` (empty line).
  - `cpuN/` (with `cpuN/online` 1, except that cpu0 has none, as on x86).
  - `cpuN/topology/`: `core_id`, `physical_package_id` 0, `die_id` 0, `core_cpus_list`, `thread_siblings_list`,
    `core_siblings_list`, `package_cpus_list`, and `thread_siblings`/`core_cpus` masks.
  - `cpuN/cpufreq/`: `scaling_cur_freq`, `cpuinfo_max_freq`, `cpuinfo_min_freq` (fixed, deliberate),
    `scaling_governor` `performance`.
  - `/sys/devices/system/node/node0/` with `cpulist`, and `/sys/devices/system/node/online` (`0`).
- **`/sys/class/power_supply/`:**
  - `BAT0` and `ADP1` are symlinks into `/sys/devices/platform/...`.
  - `BAT0/`: `type` `Battery`, `present` 1, `status`, `capacity`, `capacity_level`, `technology` `Li-ion` and `uevent`.
    `status` is `Charging`, `Discharging`, `Full` or `Not charging`; `capacity` comes from
    `psmGetBatteryChargePercentage`.
  - `ADP1/`: `type` `Mains` and `online` (from `psmGetChargerType` != None).
  - Initialize `psm` once, lazily, on first use. If `psmInitialize` fails, the power supplies are absent (no fake
    battery); say so in the report.
  - These calls are console-only: put them in `GuestRuntime.cpp` behind a small function. The pure header formats
    from plain values.
- **`/sys/fs/cgroup/`** (cgroup2 root, read-only mount from Task 1):
  - `cgroup.procs`: pids in the reader's PID namespace.
  - `cgroup.threads`: tids.
  - `cgroup.controllers` and `cgroup.subtree_control`: empty, a deliberate difference (no controllers).
  - `cgroup.max.depth` and `cgroup.max.descendants`: `max`.
  - `cgroup.stat`: `nr_descendants 0\nnr_dying_descendants 0\n`.
  - Writes fail with EROFS (read-only mount).
- **`/sys/kernel/mm/transparent_hugepage/enabled`** `always [madvise] never`, and `defrag` likewise.
- **`/sys/class/dmi/id/`** (symlink to `/sys/devices/virtual/dmi/id`):
  - `sys_vendor` `Nintendo`, `product_name` `Switch 2`, `board_vendor` `Nintendo`, `bios_vendor` `Nintendo`,
    `chassis_type` `30` (tablet), `product_uuid` (mode 0400, root only).
  - All are deliberate fixed values.
- **`/sys/kernel/`:** `osrelease` is not in sysfs (do not add); add `/sys/kernel/random/boot_id` only if Linux has it
  (it does not; skip).
- **`/sys/class/`**, **`/sys/bus/`** and **`/sys/block`:** directories exist and are empty unless listed here.
  `/sys/class/input` is Task 8.

Done when: `check-guest-sysfs` passes and `guest_nro` builds. Inventory rows.

### Task 6 (Codex): `/dev` nodes and generic ioctls

- **Nodes.** Each is a character device with the right `st_rdev`, owner and mode:
  - `console` (5:1, root:tty 0600): opens the launcher's console terminal, the same object as `/dev/tty` in the
    first tab; find how `/dev/tty` opens it.
  - `kmsg` (1:11, 0644): open for read gives EPERM without CAP_SYSLOG (dmesg_restrict=1); a write appends nothing
    and returns the length.
  - `fd` is a symlink to `/proc/self/fd`; `stdin`, `stdout`, `stderr` are symlinks to `/proc/self/fd/0..2`.
  - `mqueue/` is the POSIX message queue file system (find the mqueue layer, `runtime/ipc/GuestMqueue.h`).
  - The existing nodes (`null`, `zero`, `full`, `random`, `urandom`, `tty`, `ptmx`, `pts/`, `shm/`) stay.
  - `random` and `urandom` already read from the console RNG; check it.
- **Generic ioctls** in the ioctl entry (`TerminalIoctl` in `GuestRuntime.cpp` is the general ioctl entry despite its
  name; rename it to `Ioctl` and move the terminal part into its own function):

  | Ioctl | Behavior |
  |---|---|
  | `FIOCLEX` / `FIONCLEX` | keep |
  | `FIONBIO` | keep |
  | `FIOASYNC` (0x5452) | sets or clears `O_ASYNC`, as `fcntl(F_SETFL)` does; if SIGIO delivery is not implemented, list it as Partial |
  | `FIONREAD` (0x541b) on a regular file | size minus position; on a directory ENOTTY |
  | `FIOQSIZE` (0x5460) | file size for regular files and directories |
  | `FIGETBSZ` (2) | 4096 |
  | `FS_IOC_GETFLAGS` (0x80086601) | 0 |
  | `FS_IOC_SETFLAGS` (0x40086602) | 0 is accepted; any other bit is EOPNOTSUPP, a deliberate difference |
  | `FICLONE` (0x40049409) and `FICLONERANGE` (0x4020940d) | EOPNOTSUPP on the same mount, EXDEV across mounts |
  | `BLKGETSIZE64` (0x80081272) | ENOTTY on non-block files |
  | `RNDGETENTCNT` (0x80045200) | on random devices returns 256 |
  | `RNDADDTOENTCNT`, `RNDADDENTROPY`, `RNDZAPENTCNT`, `RNDCLEARPOOL` | EPERM without CAP_SYS_ADMIN, else 0 |
  | `TCGETS` on non-terminals | ENOTTY (check it already is) |

  Unknown requests give ENOTTY. Validate the argument pointer before writing (EFAULT).
- **Ioctl inventory.** A section in `KernelFilesInventory.md` listing every request the runtime answers, per file kind
  (terminal, socket, inet, eventfd/anon inode, regular file, random device, evdev from Task 8).

Done when: unit-testable parts are tested (a `check-guest-ioctl` target for any pure helper), and `guest_nro` builds.
Inventory rows.

### Task 7 (Codex): pure evdev model `runtime/process/GuestEvdev.h`

No console code. Everything here is host-tested in `tests/unit/GuestEvdevTest.cpp` (`check-guest-evdev`).

- **Layouts** (x86-64):
  - `input_event`: 24 bytes `{int64 sec; int64 usec; uint16 type; uint16 code; int32 value}`.
  - `input_id`: 4×uint16 `{bustype, vendor, product, version}`.
  - `input_absinfo`: 6×int32 `{value, minimum, maximum, fuzz, flat, resolution}`.
  - `ff_effect`: 48 bytes (Linux `include/uapi/linux/input.h`); only `FF_RUMBLE` is supported
    (`strong_magnitude`, `weak_magnitude` u16, `replay.length` ms).
- **`struct EvdevDevice`:**
  - `Name`, `Phys`, `Uniq`, `Id`;
  - property bits;
  - capability bitmaps per event type (`EV_SYN`, `EV_KEY`, `EV_REL`, `EV_ABS`, `EV_MSC`, `EV_FF`, `EV_LED`, `EV_REP`);
  - current key state bitmap;
  - absinfo per axis;
  - the maximum effects count (16 for gamepads, 0 otherwise).
- **Factory functions** that return complete devices:
  - **`MakeGamepad(Index)`.** Xbox 360 identity (bustype USB 0x03, vendor 0x045e, product 0x028e, version 0x0114, name
    `Microsoft X-Box 360 pad`, phys `switch2/npad<Index>`). Keys: `BTN_SOUTH`, `BTN_EAST`, `BTN_NORTH`, `BTN_WEST`,
    `BTN_TL`, `BTN_TR`, `BTN_SELECT`, `BTN_START`, `BTN_MODE`, `BTN_THUMBL`, `BTN_THUMBR`. Axes:
    - `ABS_X`, `ABS_Y`, `ABS_RX`, `ABS_RY`: −32768..32767, fuzz 16, flat 128;
    - `ABS_Z`, `ABS_RZ`: 0..255;
    - `ABS_HAT0X`, `ABS_HAT0Y`: −1..1.

    Plus `EV_FF` with `FF_RUMBLE`, `FF_PERIODIC`, `FF_SINE`, `FF_GAIN` as xpad reports (only rumble plays). The
    identity is an open decision; see section 5.
  - **`MakeKeyboard()`.** USB keyboard: `EV_KEY` for every key in the HID-usage table below, `EV_LED`
    (num/caps/scroll), `EV_REP`, `EV_MSC`/`MSC_SCAN`.
  - **`MakeMouse()`.** `REL_X`, `REL_Y`, `REL_WHEEL`, `REL_HWHEEL`, `BTN_LEFT`, `BTN_RIGHT`, `BTN_MIDDLE`.
- **`HidUsageToKey(uint8_t Usage)`.** USB HID keyboard usage to Linux `KEY_*`, the `hid_keyboard[]` table of
  `drivers/hid/hid-input.c`.
- **`class EvdevClient`** (one per open file, Linux `evdev_client`):
  - **Queue.** Holds `input_event`s; its capacity is the next power of two ≥ max(64, 8 × events per packet).
  - **`Push(events of one packet ending in SYN_REPORT)`.**
    - Respects the client's event mask (`EVIOCSMASK`).
    - On overflow: drop the queued events, enqueue `SYN_DROPPED` then a `SYN_REPORT`, as `__pass_event`.
    - Packets are delivered whole.
  - **`Read(Buffer, Size)`.**
    - Size < 24 → EINVAL.
    - Empty queue: EAGAIN when non-blocking; otherwise the caller waits.
    - Returns whole events only.
  - **Clock.** Timestamps use the client's clock (`EVIOCSCLOCKID`: REALTIME default, MONOTONIC, BOOTTIME; others
    EINVAL).
  - **Grab.** `EVIOCGRAB` makes this client the only receiver; a second grab gives EBUSY. `EVIOCREVOKE` makes reads
    and polls fail with ENODEV.
- **`int64_t EvdevIoctl(EvdevDevice&, EvdevClient&, uint32_t Request, ArgBuffer)`.** It works on a byte buffer (the
  runtime copies in and out) and decodes the `_IOC` size field. Requests:
  - `EVIOCGVERSION` (0x010001), `EVIOCGID`, `EVIOCGREP`/`EVIOCSREP`.
  - `EVIOCGNAME`/`GPHYS`/`GUNIQ(len)` return the bytes copied including the NUL, at most len.
  - `EVIOCGPROP`, `EVIOCGKEY`, `EVIOCGLED`, `EVIOCGSND`, `EVIOCGSW` and `EVIOCGBIT(type, len)` return
    min(len, bitmap bytes in longs).
  - `EVIOCGABS(axis)` / `EVIOCSABS(axis)`.
  - `EVIOCSFF`: assigns an id when `id == -1`, EINVAL for unsupported types, ENOSPC when full.
  - `EVIOCRMFF`, `EVIOCGEFFECTS`, `EVIOCGRAB`, `EVIOCREVOKE`, `EVIOCGMASK`, `EVIOCSMASK`, `EVIOCSCLOCKID`.
  - Unknown requests give EINVAL (as evdev does for unknown 'E' ioctls) and other types ENOTTY.
- **`Write(Buffer)`** of `input_event`s:
  - `EV_FF` code=effect id, value=play count → a `RumbleRequest {Strong, Weak, DurationMs}` (or stop when value is 0)
    returned to the caller;
  - `EV_FF`/`FF_GAIN` sets the gain;
  - other events are accepted and ignored, as evdev forwards them to drivers.
- **State.** Each `Push` updates the key bitmap and abs values, so `EVIOCGKEY` and `EVIOCGABS` report the current state.

Done when: `check-guest-evdev` covers framing, overflow with SYN_DROPPED, masks, each ioctl's size rules, the effects
table, rumble write decoding and the HID table (spot checks: usage 0x04 → `KEY_A` 30, 0x28 → `KEY_ENTER` 28,
0xE1 → `KEY_LEFTSHIFT` 42). No `GuestRuntime.cpp` changes in this task.

### Task 8 (Opus): input integration

- A mutex-protected `GuestInputFeed`:
  - The launcher UI thread is the only `hid` reader. It writes pad states (per npad: buttons, sticks, style,
    connected), keyboard reports, mouse deltas and buttons into the feed.
  - Each packet is stamped with monotonic time.
  - The runtime takes packets out under its own lock and turns them into evdev packets (Task 7).
  - Rumble requests go back the same way (`hidSendVibrationValues` on the UI thread).
- `/dev/input/eventN` (13:64+N, root:input 0660, with the group created in the RootFS setup if missing) and
  `/dev/input/by-id`, `/dev/input/by-path` links.
- Hot-plug: nodes appear and disappear as npads connect. Inotify events on `/dev/input` come from the milestone 2
  inotify layer, so SDL's inotify fallback finds them. Open files of a removed device get ENODEV.
- `/sys/class/input/eventN` and `inputN` with `name`, `id/*`, `capabilities/*`, `uevent` and `dev`, so libudev
  enumeration and SDL's sysfs probing work.
- No `/run/udev/control` (programs then use the inotify fallback). Static `/run/udev/data/c13:<minor>` entries
  (`E:ID_INPUT=1`, `E:ID_INPUT_JOYSTICK=1` / `KEYBOARD` / `MOUSE`) if libudev needs them.
- `poll`/`epoll`/`select` readiness and blocking reads through the existing wait mechanism.
- Input ownership between the launcher UI and the guest (see section 5).

---

## 5. Decisions (user, 2026-11-28)

1. **Gamepad identity is a launcher setting**, default Xbox 360 (`045e:028e`, xpad); the other choice is Switch Pro
   Controller (`057e:2009`, hid-nintendo). Task 7: `MakeGamepad(Index, Identity)` builds either. Task 8: the setting
   row.
2. **Buttons follow position, not label**, for both identities. The Switch's right face button (labeled A) is
   `BTN_EAST`, bottom (B) `BTN_SOUTH`, top (X) `BTN_NORTH`, left (Y) `BTN_WEST`. A game's "south/A" action is
   therefore on the Switch's B.
3. **Gamepad ownership.** While a guest process has an evdev gamepad open and its tab is in front, presses go to the
   guest only. Holding `+` and `−` together for one second returns control to the launcher.
4. **Keyboard and mouse** follow the same rule as the gamepad.

The questions as originally asked:

1. **Gamepad identity.**
   - Xbox 360 (`045e:028e`, recommended): SDL, Steam and most games know it without a mapping.
   - Nintendo Switch Pro Controller (`057e:2009`, hid-nintendo layout): true to the hardware, but SDL and Steam may
     then look for `hidraw`, which we do not serve.
2. **Who gets the controller while a guest reads it.**
   - Recommended: while the guest tab is in front and a process has an evdev gamepad open, buttons go to the guest
     only, except a launcher combination (for example `+` and `−` held for one second) that returns control to the
     launcher.
   - Alternative: both the terminal and the guest receive every press.
3. **Keyboard and mouse.** The same rule as the gamepad, or always shared with the terminal.

---

## 6. Test suite and acceptance (Claude, after Task 8)

- `tests/guest/KernelFilesSuite.c`, added to `FINAL_PASS_SUITES` in `tools/StageDebian.py`. It:
  - checks every generated file against Linux parsers (glibc `get_nprocs`, `sysconf`, procps-style parsing);
  - checks the `/dev` nodes and ioctls;
  - checks the evdev ioctls on a device when one is present (skipped otherwise);
  - follows the suite output rules in `AGENTS.md`.
- `KernelFilesTools.sh` with Debian `procps` (`ps aux`, `top -bn1`, `pgrep`, `pmap`), `ls -l /proc/self/fd`, `lscpu`,
  `free`, `uptime`, `evtest` and `python3` reading `/proc`.
- An interactive controller checklist on the console: `evtest` shows buttons and sticks, rumble on request, hot-plug,
  and the launcher combination works.
- Native runs: uid 1000 and namespace root, except the input cases. Console: final pass suite clean plus the
  checklist.
