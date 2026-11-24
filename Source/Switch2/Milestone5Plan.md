# Milestone 5: 32-bit guests

Goal: run 32-bit x86 Linux programs (i386 Debian binaries, 32-bit games,
Proton's and Wine's 32-bit processes) next to 64-bit ones. This needs
32-bit process start, the separate 32-bit syscall table with its
structure layouts, and the descriptor-table calls `modify_ldt`,
`set_thread_area` and `get_thread_area`. Graphics is out of scope.

Today the runtime forces 64-bit mode (`GuestRuntime.cpp`:
`Features.ProcessFeatures.Is64BitMode = true`,
`CONFIG_IS64BIT_MODE "1"`) and loads ELF64 only.

## Process

1. The tasks below run **one after another**, each implemented by **one
   Sonnet agent**. Never run two tasks at once. Each agent gets the
   "Global constraints" and its task text as a brief file in
   `.superpowers/sdd/`, writes its report next to it and returns status,
   a one-line test summary and concerns.
2. After the last task, **one Sonnet agent reviews the whole milestone**
   and writes one findings file, ranked by severity.
3. The **Opus orchestrator** fixes the findings, writes the test suite,
   makes it pass natively (an i386 build of the same suite on Linux), builds
   the console NRO, hands the user console runs to debug until acceptance
   passes, reconciles the inventory and checkpoint and commits once on
   `nx2`.

## Global constraints

- Follow `AGENTS.md` (real semantics, root causes, no commits by task
  agents, separate host, native and console evidence).
- One host thread runs all guest threads cooperatively; never block it.
  Validate every guest memory access.
- A 32-bit process's memory lies below 4 GiB of guest address space and
  must come from the guest arena (`GuestArena.h`); 64-bit processes are
  unchanged.
- 32-bit syscalls share the 64-bit handlers wherever the semantics match.
  Translation lives in one place (a new `GuestCompat32.h`), not copied
  into each handler.
- Each task adds host tests with a `make check-guest-*` target, keeps all
  existing checks and `guest_nro` passing with 0 warnings and updates a new
  `Source/Switch2/Compat32Inventory.md` (one row per i386 syscall number:
  name, shared handler or own, Routed/Partial/Unrouted, differences).
- Do not bump the launcher version.

## Tasks

### Task 1: feasibility and 32-bit process start

- Establish what this FEXCore checkout supports for 32-bit guests: a
  context with `Is64BitMode` false, 32-bit decoding and the syscall entry
  paths (`int 0x80`, `sysenter`, `syscall` in compatibility mode), and
  whether 32-bit and 64-bit code can share one process (far jumps between
  code segments 0x23 and 0x33, which Wine's new WoW64 mode uses). Write
  the findings into the plan before changing code; if mixed-mode is not
  supported, record it as a deliberate difference.
- ELF32 loading (`GuestElf.cpp`): `ld-linux.so.2` interpreter, PT_LOAD
  below 4 GiB, 32-bit auxv, stack layout and the 32-bit vDSO entry or
  AT_SYSINFO handling.
- `execve` picks the mode from the ELF class; fork keeps it.
- `personality(PER_LINUX32)` and `uname` machine (`i686` for 32-bit
  callers with `PER_LINUX32`, else `x86_64`).

### Task 2: the 32-bit syscall table, core calls

- The i386 number map to the shared handlers, with 32-bit argument
  widening (sign versus zero extension per argument) and 64-bit values
  split across register pairs (`pread64`, `ftruncate64`, `fadvise64_64`,
  `sync_file_range`, `fallocate`).
- Structure translation for files and memory: `stat64`, `statfs64`,
  `old_*` variants that i386 still has, `iovec`, `dirent`/`getdents`,
  `rlimit`, `rusage`, `timeval`/`timespec` in both 32-bit and `time64`
  forms (the `*_time64` syscalls 403–423), `mmap2` (page offset),
  `old_mmap`, `select`/`_newselect`, `old_readdir`.
- Process and credential calls including the 16-bit uid/gid variants.

### Task 3: signals, sockets, IPC and threads

- 32-bit signal frames (`sigaction`, `rt_sigaction`, `sigreturn`,
  `rt_sigreturn`, `siginfo`, `ucontext`, `sigaltstack`, FPU and XSAVE
  state layout), signal delivery and return.
- `socketcall` and `ipc` multiplexers plus the direct socket numbers;
  `msghdr`, `cmsghdr` (SCM_RIGHTS, SCM_CREDENTIALS), `mmsghdr`, socket
  options with 32-bit `timeval`.
- `epoll_event` (packed on x86-64 and i386 alike), `sigset` sizes,
  `futex` with 32-bit `robust_list`, `set_robust_list`, `clone` argument
  order for i386 and the TLS argument as a `user_desc`.
- Terminal and generic `ioctl` compatibility (same numbers, 32-bit
  pointer arguments).

### Task 4: descriptor tables

- `set_thread_area` and `get_thread_area` (GDT TLS entries 12–14,
  `entry_number = -1` allocation), `modify_ldt` (read, write, write_old,
  read_default; per-process LDT inherited by fork) for 32-bit and 64-bit
  callers.
- Segment register loads (`mov %gs`, `%fs`) resolving bases from GDT and
  LDT entries through FEXCore.
- `arch_prctl` (64-bit) unchanged; `ARCH_GET_CPUID`/`ARCH_SET_CPUID` as
  Linux.

### Task 5: i386 rootfs

- `FetchDebian.py`, `StageDebian.py` and the console rootfs setup
  (`GuestRootfsSetup.h`) add the i386 architecture (dpkg
  `--add-architecture i386` state) and install `libc6:i386` and the i386
  packages the test suite needs, alongside the 64-bit ones.
- `make check-rootfs-setup` covers the i386 packages.

## Test suite and acceptance (orchestrator)

- Build ProcSuite, CommSuite and the filesystem suite for i386 (`-m32`)
  where their cases apply, plus `Compat32Suite.c` for the i386-only paths
  (socketcall, ipc, time64 calls, LDT, signal frames).
- Tools: i386 `bash`, `coreutils` and `xz` from Debian.
- `compat32-acceptance.elf` printing `Compat32 acceptance results: suite
  42, tools 42`. Native: an i386 run on Linux, as uid 1000 and namespace
  root. Console: that line plus the 64-bit acceptance programs as
  regressions.
