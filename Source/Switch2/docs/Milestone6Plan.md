# Milestone 6: /proc, /sys, /dev and input

Goal: give desktop programs, Steam and games the kernel files they read
to learn about the system and themselves, the common `/dev` nodes and
ioctls, and controller and keyboard input through evdev. Graphics (DRM,
display, GPU ioctls) is out of scope.

## Process

1. The tasks below run **one after another**, each implemented by **one
   Sonnet agent**. Never run two tasks at once. Each agent gets the
   "Global constraints" and its task text as a brief file in
   `.superpowers/sdd/`, writes its report next to it and returns status,
   a one-line test summary and concerns.
2. After the last task, **one Sonnet agent reviews the whole milestone**
   and writes one findings file, ranked by severity.
3. The **Opus orchestrator** fixes the findings, writes the test suite,
   makes it pass natively (uid 1000 and namespace root), builds the
   console NRO, hands the user console runs to debug until acceptance
   passes, reconciles the inventory and checkpoint and commits once on
   `nx2`.

## Global constraints

- Follow `AGENTS.md` (real semantics, root causes, no commits by task
  agents, separate host, native and console evidence).
- Milestone 5 Task 0 gives each guest thread its own host thread; follow
  its locking and blocking-wait rules.
  Validate every guest memory access.
- Generated files reflect live runtime state at `read` time with Linux's
  formats (field order, spacing, units), `stat` results (size 0 for
  generated `/proc` files, `st_dev` of the proc mount) and permissions,
  including mount namespaces and `/proc` mounts from milestone 4.
- Values the console cannot know (hardware the guest does not see) use
  fixed, plausible values listed as deliberate differences.
- Each task adds host tests with a `make check-guest-*` target, keeps all
  existing checks and `guest_nro` passing with 0 warnings and updates a new
  `Source/Switch2/KernelFilesInventory.md` (one row per file or ioctl
  family: Routed/Partial/Unrouted, Linux behavior, differences).
- Do not bump the launcher version.

## Tasks

### Task 1: per-process /proc

- `/proc` readdir lists live pids; `/proc/self`, `/proc/thread-self`.
- `/proc/<pid>/`: `stat`, `statm`, `status` (all fields Linux prints),
  `cmdline`, `environ`, `comm` (writable), `maps`, `smaps`,
  `smaps_rollup`, `exe`, `cwd`, `root`, `fd/` and `fdinfo/` (pos, flags,
  mnt_id, eventfd and epoll and inotify extras), `task/<tid>/` with the
  per-thread files, `limits`, `cgroup`, `oom_score_adj` (writable),
  `auxv`, `io`, `wchan`, `sched`, `timerslack_ns`, `mem` (read through
  `process_vm_readv` rules), `ns/` (from milestone 4).
- Access checks for other processes' files follow Linux's ptrace access
  mode rules with the guest credentials.

### Task 2: system /proc and /sys

- `/proc/cpuinfo` (one block per reported CPU, flags matching the CPUID
  FEX exposes), `meminfo` (from the arena commit limit and use),
  `stat`, `uptime`, `loadavg`, `version`, `filesystems`, `swaps`,
  `partitions`, `cmdline`, `devices`, `misc`, `pressure/` (as Linux
  without PSI if absent).
- `/proc/sys/`: `kernel/` (`osrelease`, `ostype`, `hostname`
  (namespace-aware), `pid_max`, `random/boot_id`, `random/uuid`,
  `threads-max`, `yama/ptrace_scope`, `dmesg_restrict`), `vm/`
  (`overcommit_memory`, `max_map_count`, `mmap_min_addr`), `fs/`
  (`file-max`, `nr_open`, `inotify/*`, `mqueue/*`, `pipe-max-size`),
  `net/core/somaxconn`. Writes where Linux allows root to change behavior
  the runtime has (for example `pipe-max-size`), EACCES or EPERM
  otherwise.
- `/sys`: `devices/system/cpu/` (`online`, `possible`, `present`,
  `cpuN/topology/*`, `cpufreq` minimal), `class/power_supply/` battery
  from the console's charge state (psm service), `fs/cgroup/` (cgroup2
  root with the guest's processes, read-only), `kernel/mm/transparent_hugepage/enabled`.

### Task 3: /dev nodes and generic ioctls

- Character devices with correct `st_rdev`: `null`, `zero`, `full`,
  `random`, `urandom` (from the console RNG), `tty`, `console`, `ptmx`
  and `pts/` (reuse the terminal layer), `fd/`, `stdin`, `stdout`,
  `stderr`, `shm/`, `mqueue/`, `kmsg` (EPERM).
- Generic ioctls on every file kind: `FIONREAD`, `FIONBIO`, `FIOASYNC`,
  `FIOCLEX`, `FIONCLEX`, `FIGETBSZ`, `FS_IOC_GETFLAGS`/`SETFLAGS`,
  `FICLONE` (EOPNOTSUPP), `BLKGETSIZE64` (ENOTTY on non-block files),
  `RNDGETENTCNT` on random devices; ENOTTY for unknown requests.
- An ioctl inventory: every request the runtime answers, per file kind.

### Task 4: input devices

- `/dev/input/eventN` evdev devices for the Switch 2 controllers (each
  npad as a gamepad with Xbox-style button and axis codes, matching what
  SDL and Steam Input expect), the USB keyboard and a mouse when present;
  `/dev/input/by-id` and `by-path` links.
- Evdev semantics: `read` of `input_event` records with SYN_REPORT
  framing, `poll` readiness, EVIOCGVERSION, EVIOCGID, EVIOCGNAME,
  EVIOCGPHYS, EVIOCGUNIQ, EVIOCGPROP, EVIOCGBIT, EVIOCGKEY, EVIOCGABS,
  EVIOCSABS, EVIOCGRAB, EVIOCSCLOCKID, EVIOCGEFFECTS with rumble through
  EV_FF (`EVIOCSFF`, `EVIOCRMFF`, write of `EV_FF` play events) to the
  controllers' vibration.
- Hot-plug: device nodes appear and disappear as controllers connect;
  matching `/sys/class/input/` entries and udev-style data under
  `/run/udev/data/` so libudev, libinput and SDL find the devices
  (inotify events on `/dev/input` from the milestone 2 inotify layer).
- The launcher's own input handling must keep working while a guest
  reads controllers (document which device owns which button combination
  for leaving the guest).

## Test suite and acceptance (orchestrator)

- `tests/guest/KernelFilesSuite.c` checks formats and values against
  Linux parsers (glibc `get_nprocs`, `sysconf`, procps parsing), all
  generated files, `/dev` nodes and ioctls. `KernelFilesTools.sh` with
  Debian `procps` (`ps aux`, `top -bn1`, `pgrep`, `pmap`), `lsof` or
  `ls -l /proc/self/fd`, `lscpu`, `free`, `uptime`, `evtest` (scripted
  capture) and `python3` reading `/proc` where available.
- `kernelfiles-acceptance.elf` printing `Kernel files acceptance results:
  suite 42, tools 42`; a separate interactive controller checklist
  (evtest shows buttons, sticks, rumble on request, hot-plug).
- Native: uid 1000 and namespace root, except the input cases, which are
  console-only. Console: the acceptance line plus the earlier acceptance
  programs as regressions.
