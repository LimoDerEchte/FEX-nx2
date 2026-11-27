# Switch 2 milestone handoff

## Restart checkpoint

### 0.17.17 console (2026-11-27): FEX gets the console's CPU features (accepted, committed)
Console (log `.superpowers/sdd/console-0.17.17.log`): perl array loop 1m43.8s -> 1m38.3s, xz -9 1m8.6s -> 1m7.3s;
comm 124/0/1, fs 81/0/2, pty 43/0/0, proc 241/2/6, container 53/1/0. The 3 failures (`proc/rlimit_inherited_by_fork_and_exec`,
`timer/alarm_survives_exec`, `seccomp/filter_inherited_and_kept_across_exec`) are not from this change: they re-exec
`/proc/self/exe`, which names the card path `/fex/games/<suite>.elf`, and the guest cannot reach the card by absolute
path (execve ENOENT). They passed at M4, when the suites ran from the RootFS `/bin`. Next: that fix, then Task 1.
Previous entry text:
`InitializeProcessContext` (`runtime/GuestRuntime.cpp`) passes `SupportsAtomics`, `SupportsRCPC`, `SupportsTSOImm9`
(LSE, LDAPR, LDAPUR/STLUR; mtprobe 3 ran all three). TSO stays on for every process until multithreading Task 1
(user decision: TSO from the second thread on). Host: guest_nro 0 warnings, filesystem-runtime ok; codegen runs only on
the console. NRO `.superpowers/sdd/fex-switch2-guest-0.17.17.nro`. Console: suites (proc, comm, fs, pty, container,
thread-ipc) plus a speed comparison with 0.17.16 (perl array loop, xz). Then its own commit, then Task 1.

### Multithreading Task 0: probe runs 1-3 done, Task 0 complete (results `docs/MultithreadingSdk.md`, logs `.superpowers/sdd/mtprobe-{1,2,3}.log`); TSO default decision with the user pending (2026-11-27)
EXPERIMENT `make mt-probe` -> `build/guest/fex-switch2-mtprobe.nro` (copy `.superpowers/sdd/fex-switch2-mtprobe-1.nro`), sources
`probes/MultithreadProbe.cpp`, `probes/MultithreadBlobs.S` (x86 test code, assembled on the PC by
`probes/MakeMultithreadBlobs.py`), CMake target `mtprobe_nro`. Log `sdmc:/fex/mtprobe.log`. Checks in order: info,
thread_limit, core_scaling, wake_latency, jit_parallel (gate), poll_stop, smc_cross_thread (gate), tso_litmus, tso_cost,
thread_memory, exec_stack (last; may crash by design). Run 1 log `.superpowers/sdd/mtprobe-1.log`: gates pass, crash in exec_stack at 1 MiB. Probe 2 (`.superpowers/sdd/fex-switch2-mtprobe-2.nro`) re-runs core placement (all-cores mask), pinned jit_parallel and a logged stack ladder; sidecar `sdmc:/fex/mtprobe.full` runs every check. Decisions and plan: `docs/MultithreadingPlan.md`. Next: results into
`docs/MultithreadingSdk.md`, TSO default decision with the user, then Task 1. Host: probe and guest_nro build, 0 warnings;
nothing of it runs on the host.

### 0.17.16 console (2026-11-27): launcher UI milestone accepted and committed. Console: SD-card image install
(hash matches, old RootFS removed only after verify), broken card image fails with the RootFS kept, FTP stop during an
upload (426, deferred stop, no crash), closed tab kills its `nohup sleep`; no fault lines. Log
`.superpowers/sdd/console-0.17.16.log`. After the run: the failed-install log line no longer says "removing" when the
RootFS was not touched (log text only). Next: multithreading milestone (`docs/MultithreadingPlan.md`, discuss with the
user first), then M5; performance backlog.

#### 0.17.16 whole-milestone review fixes. Review `.superpowers/sdd/ui-review-report.md` (1 Critical,
3 Important, 7 Minor). Fixed: (C1) "Image from the SD card" built `/fex/rootfs/fex/images/x.sqsh` (`launcher/RootfsInstallHost.cpp`
now keeps the card path); (I3, C1) the installer removes the old RootFS only after the new image is downloaded, verified and
opens (`RunSteps`/`RemoveOld` in `runtime/fs/GuestRootfsInstall.h`; wipe first only when the download does not fit next to
it); a card image with another name than the index file installs unverified with a warning; (I1) FTP stop/start/tab
open/install from a frame drawn inside a transfer freed the running server: now deferred to `Tick` after `Step`, transfers
aborted, tab open/install refused while a transfer runs (`launcher/LauncherBackend.cpp`); (I2) live index entries only from
the built-in host, note says not authenticated, index sizes over 1 TiB rejected; (M1) background jobs of a closed tab are
killed 2 s after close even when the leader exited first (`ServicePoll`); (M2) xz truncation check; (M3) downloads capped
at the expected size (64 MiB without one); (M6) squashfs block list read in chunks; (M5) `ReportMemory` removed;
(M7) specs moved to `Source/Switch2/docs/ui`, plan marked historical, `ThirdParty/nanosvg/LICENSE.txt`. Host: check-ui ok,
30/30 check-guest-* ok, pty-suite 43/0/0, proc-suite 249/0/7 + 253/0/3 + 6/0/0, check-rootfs-install ok (3 new cases),
filesystem-runtime ok, guest_nro 0 warnings. NRO `.superpowers/sdd/fex-switch2-guest-0.17.16.nro`.

### 0.17.15 console (2026-11-26): UI Task 8 accepted. pty-suite and pty-suite-root 43/0/0; X overlay and Settings/About fixes approved; apt-get update did not crash (backlog). Log `.superpowers/sdd/console-0.17.15.log`. Next: whole launcher UI milestone review (one Sonnet agent), orchestrator fixes, console acceptance, one commit. — fixes from the 0.17.13 console run (UI Task 8e, host part)

0.17.14 console (log `.superpowers/sdd/console-0.17.14.log`): X no longer crashes; PgUp/PgDn worked already in 0.17.13
(misread finding); RootFS ETA deferred by the user. pty `isig_signals_to_foreground_group` root cause: test race, not the
runtime: the parent typed ^C ^\\ ^Z ^C before the child ran once (signal trace: shared pending 0x80006), and standard
signals coalesce; the native run hid it (child on another CPU). Test now waits for the child's acknowledgement after each
signal; experiment logging removed. X overlay redrawn as the user asked: no panel, scrim, the four keys, labels outside
(above Up, below Down, left of Left, right of Right). apt-get update still closes the app (HOS error screen; the console runs
stock HOS, not Atmosphère) while apt "store"s a Packages file, with no fault/abort line: 0.17.15 writes the runtime trace
to the SD at once in Detailed/Trace (was buffered and lost) and logs process memory every 5 s (EXPERIMENT).
Proc suite: `clock/monotonic_never_decreases` assumed BOOTTIME == MONOTONIC (false on a host that has slept); now checks
BOOTTIME >= MONOTONIC. Checks: check-ui ok, all check-guest-* ok, pty-suite 43/0/0, proc-suite 253/0/3, guest_nro 0
warnings, filesystem-runtime ok. NRO `.superpowers/sdd/fex-switch2-guest-0.17.15.nro`.

Orchestrator part (2026-11-26): (1) X crash fixed: `HintBar::SetHints` freed its `HintButton`s while `Interaction` layers
and `PressedW` still pointed at them (use-after-free in `Interaction::Update`, reproduced with
`build/fex-ui-host --headless --screen shell --script X`). `HintBar` now keeps a pool of buttons for its lifetime and
`Interaction` re-flattens every layer from its roots before and after dispatch (`Reflatten`, drops a stale press). (9) apt
crash: no trace in the log because `__libnx_exception_handler` gave up when another thread held `LogMutex` (guest worker
beside the UI since 8b). New `WriteFatalLog` waits up to 200 ms, then writes without the lock, flushing the buffered guest
trace; `abort` (`--wrap=abort`) and `diagAbortWithResult` log `Host abort lr=` / `Host diagAbortWithResult` first. Cause
of the crash itself still unknown: next log. (12) EXPERIMENT logging for `pty/isig_signals_to_foreground_group`
(Detailed/Trace): `Guest pty signal target` (mask, wait set, pending, handler, frames) per member after each pty signal,
then up to 64 `Guest signal handler|return|dropped` lines. Remove once the cause is found. Checks after these changes:
`check-ui` ok, all `check-guest-*` ok, pty-suite 43/0/0, proc-suite ok, guest_nro 0 warnings, `filesystem-runtime` ok.

0.17.13 console (log `.superpowers/sdd/console-0.17.13.log`): X on the Shell crashed the app, `sudo apt-get update` crashed, PgUp/PgDn
still wrong in `less`, `pty/isig_signals_to_foreground_group` failed again (42/1). Report `.superpowers/sdd/ui-task8e-report.md`,
NRO `.superpowers/sdd/fex-switch2-guest-0.17.14.nro`. Done in 8e: X overlay (cross of the four D-pad keys with their combinations),
Settings row centring, no lines in the About license list / before About, "Read license text" button style, RootFS install ETA
(average rate, extraction weighted by bytes and entries), Run ELF picker starts in `sdmc:/fex/games`, list wrap-around, the HID
keyboard mapping as host code with tests. Handed to the orchestrator: X crash (cause found, see the report), apt crash, isig test.
Console to retest: see the report's checklist.

### 0.17.13 pending — fixes from the 0.17.12 console run (UI Task 8d)

0.17.12 console (log `.superpowers/sdd/console-0.17.12.log`): the 8b CLI tests passed. Findings fixed in 0.17.13, report
`.superpowers/sdd/ui-task8d-report.md` (cause, fix, test per finding; console checklist there), NRO
`.superpowers/sdd/fex-switch2-guest-0.17.13.nro`:
1. Shell footer arrows drew as missing glyphs (the UI font has none): the footer is button discs now.
2. Controller tab management: ZR new tab, ZL close tab, L/R switch; right stick scrolls back; R3 opens the list of all bindings.
3. PgUp/PgDn were encoded as `CSI 4~`/`5~` (End/PgUp) instead of `5~`/`6~`; the "flash" was the 120 ms BEL overlay (bash rings
   BEL on Tab and unbound keys): removed.
4. `/proc/<pid>/stat`, `statm`, `cmdline`, `comm`, `cgroup`, `task/`, `/proc/stat|uptime|loadavg|meminfo|cpuinfo`,
   `pid_max` (`runtime/process/GuestProcfs.h`): `top` and `ps` read them; `docs/ProcessInventory.md`.
5. RootFS install extras: `bsdutils` (script; not `bsdextrautils`), `util-linux` (su), `mount` (mount/umount): a package whose
   file is missing is unpacked although dpkg lists it as installed. Needs a RootFS reinstall.
6. apt sources: deb only (no deb-src), plain archive URIs (no mirror method). Needs a RootFS reinstall.
7. Settings row rules removed.
8. pty-suite on the console: termios (the guest ELF is built with the host's glibc 2.42+ headers where `B38400` is 38400 but
   runs on Debian 12's glibc 2.36, whose `cfgetospeed` returns the code 0017: test compares speeds now) and TIOCSPGRP
   (pgid 1 is the virtual init: EPERM, not ESRCH) fixed; `isig_signals_to_foreground_group` still open: the second ^C (typed
   line pending) raises no SIGINT on the console only. EXPERIMENT: 0.17.13 logs every terminal signal typed at a pty master
   ("Guest pty N: signal S for foreground group G: K process(es) signalled", Detailed/Trace log). Read it in the next run.
Host 2026-11-26: `check-ui` ok, 30 `check-guest-*` ok (new `check-guest-procfs`), `check-rootfs-install` ok,
`check-pty-suite-native` 43/0/0 (host glibc and Debian 12 glibc), `check-proc-suite-native` 249/0/7 and 253/0/3 (+ Debian
glibc `procfs/` 6/0/0), `guest_nro` 0 warnings, `filesystem-runtime` exit 0. Console: not run.

### 0.17.12 pending — UI Task 8b + 8c

8b artifacts: `build/guest/fex-switch2-guest.nro`, report
`.superpowers/sdd/ui-task8b-report.md`; console run pending.

UI Task 8c (terminal emulation, keys) host work done, console run pending:
`ui/TerminalGrid.*` xterm emulator, `ui/TerminalKeys.*`, Shell footer and
X-then-D-pad control keys; details in `docs/TerminalInventory.md`, report
`.superpowers/sdd/ui-task8c-report.md`, console checklist there. Host rerun
on 2026-11-26: `make check-ui` ok (ui-terminal-test 219 checks, 0 failures),
`check-pty-suite-native` 43/0/0, `check-proc-suite-native` 247/0/3,
`guest_nro` 0 warnings, `filesystem-runtime` exit 0. These supersede the
blocked 8b host results below.


Task 8b is implemented in the working tree. Release version remains 0.17.10
until the host gate passes. The launcher shares one runtime across user,
root and developer tabs. Each tab uses its own pty. Games ELF interpreters
resolve in the installed RootFS guest namespace.

Host: `cmake --build Source/Switch2/build/guest --target guest_nro` built
`guest_nro` with 0 warnings. `make -C Source/Switch2 filesystem-runtime`
exited 0. The 29 `check-guest-*` targets returned 28 passes and one FTP
failure (`listen failed: Operation not permitted`). `make -C Source/Switch2
check-ui` stopped at `VK_ERROR_INITIALIZATION_FAILED (-3)`. The app
test passed (ui-app-test: 88 checks, 0 failures).
Native: `make -C Source/Switch2 check-pty-suite-native` 41/2/0,
`make -C Source/Switch2 check-proc-suite-native` 233/10/7. No console run.

Rerun outside the sandbox (2026-11-26): all 29 `check-guest-*` pass,
`check-ui` 0 failures, pty-suite 43/0/0, proc-suite 247/0/3. 0.17.11 (8b only)
was never run on the console; 0.17.12 (8b + 8c) replaces it:
`build/guest/fex-switch2-guest.nro` (copy
`.superpowers/sdd/fex-switch2-guest-0.17.12.nro`), games staged in
`build/guest/sd/fex/games`, 0 build warnings. Next: console run with the 8b
checklist below plus `.superpowers/sdd/ui-task8c-report.md` "Console checklist".
See `.superpowers/sdd/ui-task8b-report.md` for the full check list.

#### Console checklist

1. Copy `Source/Switch2/build/guest/fex-switch2-guest.nro` to the card. Copy `Source/Switch2/build/guest/sd/fex/games/` to `sdmc:/fex/games/`, including sidecars. Use the installed Ubuntu RootFS.
2. Start FTP, open Shell, then open another shell with the tab strip's +. Run `for i in 1 2 3 4 5; do echo "$i $(tty)"; sleep 2; done` in each tab. Each tab should print its own distinct `/dev/pts/N` and keep progressing while focus is on the other tab. Confirm typing only reaches the active tab and the UI stays responsive. Check `stty size` before and after a display size change.
3. In a user tab check `id -u` → 1000, `pwd` → `/home/fex`, `echo "$TERM"` → `xterm-256color`, and `sudo true; echo $?` → 0. Run `sudo apt-get update` on a connected console. Check that it gets no pty or `posix_openpt` error.
4. In one shell run `sleep 30 &`, `jobs`, and `fg` in order. Send Ctrl+Z and check for a stopped job, then run `bg` and check that it resumes. The other tab should remain unaffected. Task 8c supplies physical Ctrl+Z input. The current console can test the same VSUSP path by running `stty susp x` before `fg`, sending `x` from the software keyboard while the job is foreground, then restoring `stty susp '^Z'` after regaining the prompt.
5. Run `script -qc true /dev/null; echo $?` and expect 0. In **+ Settings > Developer**, open a Root shell next to the user shell. Check `id -u` → 0 and `pwd` → `/root`. Switch back and recheck the user credentials and terminal.
6. In **+ Settings > Developer > Run ELF**, choose `sdmc:/fex/games/pty-suite.elf` beside a shell. Expect startup through the installed Ubuntu interpreter, 43 passed, 0 failed, 0 skipped, and a retained `Exited with status 42` tab. Repeat with `pty-suite-root.elf` to verify the UID/GID sidecar. Check that a single-block or recovery launch is refused while an ordinary session is active.
7. Close a running tab and confirm the other survives. Run `exit` in Bash and confirm its tab closes. Close all remaining tabs and expect Home, guest shutdown, and FTP restoration. Run again and exit the app using a normal platform shutdown with tabs still open. Confirm guest shutdown and inode-store persistence by checking `sdmc:/fex/fex-switch2-guest.log` and reopening the launcher.

- **Test staging (2026-11-26, console not run; launcher unchanged at 0.17.10).** `make -C Source/Switch2 filesystem-runtime` now deletes `build/guest/sd/fex` and stages only `fex/games/` (test ELFs, sidecars, helper scripts); it never writes `fex/rootfs` or `fex/setup`, so copying `build/guest/sd/fex` onto the card cannot overwrite the Ubuntu RootFS. Programs run from Settings > Developer > Run ELF (working folder = the games folder) and are named without `/bin/`: use `filesystem-acceptance.elf`, `process-acceptance.elf`, `comm-acceptance.elf`, `container-acceptance.elf`, `*-suite.elf`, `*-suite-root.elf` where older entries below say `/bin/<name>`. Scratch goes to `/home/fex` or `/root` through `-d DIR` in the `.args` sidecars; the recovery probes run as root (`.user 0 0`) because the runtime's journal test folder is `/.fex-recovery-test`. The full list with expected results is in the README section "Test programs on the console". `make debian-runtime` still stages the old Debian developer bundle in `build/debian-runtime`. `tools/RunPackagedNative.sh` needs a RootFS folder; the staged bundle no longer has one.
- **Status (2026-11-24).** Milestone 3 (memory and communication) is
  **complete** and committed on `nx2` as launcher **0.14.2**. Console
  acceptance on 0.14.2: `/bin/comm-acceptance.elf` `Communication
  acceptance results: suite 42, tools 42` (comm-suite 124 passed, 0 failed,
  1 skipped); `/bin/comm-suite-root.elf` 124/0/1, exit 42;
  `/bin/filesystem-acceptance.elf` `ABI 42, coreutils 0, suite 42`. On
  0.14.1: `/bin/process-acceptance.elf` `suite 42, tools 42`, interactive
  Bash with `nc -l 0.0.0.0 40002` reached from a LAN PC. The launcher
  replaces `__libnx_initheap` (HostArena.cpp): it shrinks the loader heap to
  2 GiB so the guest arena gets a 6144 MiB commit limit, and gives the heap
  back at exit. SD data reads and writes bounce through a 64 KiB heap
  buffer. Horizon allows binding 0.0.0.0 (EXPERIMENT `BindProbe.cpp`,
  `make bind-probe`), so INADDR_ANY passes through. Native: comm-suite
  122/0/3 as uid 1000 and namespace root; packaged comm-acceptance
  `suite 42, tools 42`. Host: the 8 `check-guest-*` targets of milestone 3
  pass. Evidence per interface: [CommunicationInventory.md](docs/CommunicationInventory.md).
- **FTP (2026-11-24, launcher 0.14.3).** LIST printed `%b %d %Y %H:%M`
  (one field too many), so clients took the time as part of the name.
  `GuestFtp.h` `ListTime` prints `ls -l` dates (`%b %e %H:%M` within six
  months, else `%b %e  %Y`). Host: `make -C Source/Switch2 check-guest-ftp`
  0 failed. Console 0.14.3: FileZilla lists, browses, downloads and uploads
  with correct names and paths.
- **0.17.9 (2026-11-26, console not run): RootFS `/` owner.** 0.17.7 console: `/etc` and `/etc/sudoers.d` are 0:0, but `/` stayed 1000:1000: extraction skipped the image's root entry (the folder exists), so it had no owner record (default 1000). Extraction now applies the image root's owner (re-owned, 1000 to 0) and mode to `/` through new `RootfsWriter::SetOwner` (console: `ChangeOwner`). Fixture root pinned to 0755 1000:1000 (`-root-mode/-root-uid/-root-gid`); install_full checks `/` is 0:0 0755 (failed before). Existing installs: `chown 0:0 /` from the Root shell, or reinstall. sudo now fails with "unable to allocate pty: No such file or directory": no ptys yet, UI Task 8a. Host: `make check-rootfs-install` ok, guest_nro 0 warnings. ELF `.superpowers/sdd/fex-switch2-guest-0.17.9.elf`.
- **0.17.10 (2026-11-26, console not run): UI Task 8a, runtime ptys.** `/dev/ptmx`, `/dev/pts/N` with Linux semantics (tty object shared with the console terminal, `TtyMaster` interface for 8b); details and Partial items in `docs/TerminalInventory.md`, report `.superpowers/sdd/ui-task8a-report.md`. New `tests/guest/PtySuite.c`: 43/0/0 native (uid 1000 and unshare-root), staged as `/bin/pty-suite.elf` and `/bin/pty-suite-root.elf` (+ `.user 0 0`). Host: 29/29 check-guest-* (new check-guest-pty), check-ui, proc and container suites native ok; guest_nro 0 warnings. Runtime wiring console-only, unverified (agent's least sure spots: pty waits in `WaitKind::IO`, stat/readlink of pty nodes through the SD path code). ELF `.superpowers/sdd/fex-switch2-guest-0.17.10.elf`. Next: 8b.
- **0.17.10 console (2026-11-26, log `.superpowers/sdd/console-0.17.10.log`):** both runs from `sdmc:/fex/games` failed before start with "Could not open the selected file.": `Inspect` (launcher/LauncherBackend.cpp) reads a games program's interpreter by raw SD path (`ResolvePath` + `ReadElf`), but Ubuntu's `/lib64/ld-linux-x86-64.so.2` is a symlink chain kept in the inode store. The old Debian bundle staged a real ld-linux there, which hid it. Fix (interpreter through the guest namespace, like `ReadRootfsElf`) handed to the running 8b agent, which owns those files. Pty suite, sudo, acceptance: not run yet.
- **Multithreading plan 2026-11-26:** guest threads still run on one host thread (cooperative scheduler + 10 ms watchdog preemption). Full plan `docs/MultithreadingPlan.md` (one runtime lock with the JIT outside it, host thread per guest thread, retry-model waits on per-thread wake objects, stop-the-world for whole-process changes, TSO at the second thread; Task 0 console feasibility probe first). Not started; proposed after UI Task 8, before Milestone 5's 32-bit work.
- **UI Task 8 (ptys + tabs) started 2026-11-26,** split into 8a runtime ptys + `tests/guest/PtySuite.c` (Sonnet, brief `.superpowers/sdd/ui-task8a-brief.md`), 8b concurrent runtime + one Bash per tab on host-owned masters, 8c terminal emulation; then the whole-UI review. **8a done (host side, console not run, launcher still 0.17.9):** per-terminal `GuestTty` + devpts ptys (`runtime/process/GuestPty.h`, wiring in `GuestRuntime.cpp`), `tests/guest/PtySuite.c` (43 cases pass natively as uid 1000 and `unshare -r` root; staged as `/bin/pty-suite.elf` and `-root.elf` by `make filesystem-runtime`), `docs/TerminalInventory.md`, host test `make check-guest-pty`. Report `.superpowers/sdd/ui-task8a-report.md`. Console checklist: run `/bin/pty-suite.elf` and `/bin/pty-suite-root.elf` (expect 43 passed, exit 42), then `sudo true` in Bash (no pty error).
- **0.17.8 (2026-11-26, console not run): UI fixes.** (1) Settings sidebar: the rule above About spans the whole sidebar (x 0 to `SidebarW`, `ui/screens/settings.cpp`). (2) Wipe steps read "Counting the files" / "Removing the files". (3) After a wipe the app no longer exits (left over from the old Settings wipe): Done shows A Continue, which sends `Request::Kind::StartFirstRun`; `App::StartFirstRun` closes every screen and opens first-run RootFS setup (B there exits, as on a launch without a RootFS). Reinstall is unchanged (Continue goes Home). New `TestWipeFlow` in `ui/tests/AppTest.cpp`. (4) Debian 12 removed (user decision: not planned): built-in index entry, card, tests, docs (`docs/ui/rootfs-image-format.md`, `docs/LauncherUiPlan.md`); the `Available`/`Reason` index fields stay, tested with a synthetic entry. The Debian developer sysroot tools (`tools/FetchDebian.py`, `make debian-sysroot`) are a separate thing and stay. Host: `make check-ui` ok, guest_nro 0 warnings. ELF `.superpowers/sdd/fex-switch2-guest-0.17.8.elf`.
- **0.17.7 (2026-11-26, console not run): root-owned RootFS, hosts, login start folder.** 0.17.5 shell: `sudo true` said "/etc/sudoers.d is owned by uid 1000, should be 0", "unable to resolve host fex-switch2", "a password is required". Cause: the FEX image owns all 39,528 entries as 1000/1000 (`unsquashfs -lln`) and the installer kept image owners, so sudo ignored `/etc/sudoers.d/fex` (NOPASSWD rule, already written by setup) and asked for a password. Fix: `ExtractHooks::BuilderToRoot` (installer sets it) maps uid/gid `ImageBuilderId` 1000 to 0 at extraction; other ids stay; `/home/fex` stays 1000. `/etc/hosts` gains `127.0.1.1\tfex-switch2` when no line names `GuestHostName` (`HostsNames`). Because the system tree is now root's, runs as the default user start in `/home/fex` with HOME=/home/fex, and root runs start in `/root` (`RunProgram` in `launcher/LauncherBackend.cpp`, env in `RunGuest`); the suites make their scratch folders in the working directory. Old installs keep uid 1000 until reinstalled. nano/wget printing nothing in the log: user says they work; the log only captures stderr of shell children (not a bug to fix now). Tests: fixture gains `etc/sudoers.d` + README owned 1000 (`tools/CheckRootfsInstall.py`); install_full checks they become 0/0, setuid-probe stays 12/34, /home/fex 1000, hosts line; failed before the fix. Host: `make check-rootfs-install` ok, `make check-ui` ok, 28/28 check-guest-* ok, guest_nro 0 warnings. ELF `.superpowers/sdd/fex-switch2-guest-0.17.7.elf`.
- **0.17.6 (2026-11-26, console not run): RootFS wipe one entry at a time, with progress.** 0.17.5 console: the new object store works (user), extras configured. Wipe: `fsFsDeleteDirectoryRecursively` held the SD service about a minute, so HOME/hbmenu froze. `WipeGuestRootfs` (`launcher/RootfsInstallHost.cpp`) now removes the marker first, counts every entry of `/fex/rootfs` and `/fex/.inode` (`SdWipe::Count`, fsDirRead), then deletes deepest-first with one `fsFsDeleteFile`/`fsFsDeleteDirectory` per entry, on a worker thread (`WipeOnWorker`) while the UI thread pumps frames. Settings > Wipe RootFS opens the Installation screen with `InstallRequest::WipeOnly` ("Wiping the RootFS", step 1/2 "Count the files", 2/2 "Remove the files" with "n of m", no cancel; Done: A Exit; Failed: Retry/Back); backend `WipeRootfs(InstallEvents&)` returns an `InstallOutcome` (`RunRootfsWipe`). A reinstall uses the same delete inside its step 1 ("Counting files: n", "Removed n of m"), title unchanged. Host: `make check-ui` ok (new wipe-mode test in Task6Test), `make check-rootfs-install` ok, guest_nro 0 warnings. ELF `.superpowers/sdd/fex-switch2-guest-0.17.6.elf`. The 0.17.5 shell issues are fixed in 0.17.7.
- **0.17.5 (2026-11-25, console not run): object store layout and extras status.** (1) Inode objects move from one flat `/fex/.inode/objects/<inode>` folder to `objects/<inode>>16 hex>/<(inode>>8)&0xff hex>/<inode>` (`ObjectFolder`/`ObjectPath`/`EnsureObjectFolder` in `runtime/GuestRuntime.cpp`; leaf folders cached per session). Reason: the user saw single small files in `/usr/share` take seconds late in a RootFS install; exFAT creates/looks up names by scanning the whole folder (0.16.12 log: 28,047 promotion intents, objects all in one folder). Unconfirmed as the cause until a console install log shows the time drop. No migration (user decision): a store with files directly in `objects/` is detected at load (`FlatObjectLayout`), logged ("old flat layout ... reinstall the RootFS") and the inode store is unavailable, so old RootFS installs must be reinstalled. (2) 0.17.2 console: extras configure failed with dpkg "parsing file '/var/lib/dpkg/status' near line 18573 package 'sudo': mixed non-coinstallable and coinstallable package instances present": the image keeps a non-installed `sudo` entry and the installer appended a second one. `WithoutReplacedEntries` (`runtime/fs/GuestRootfsInstall.h`) drops the image's entry for each unpacked package (dpkg instance rules: one per name, or one per arch for Multi-Arch: same) before appending; fixture status gains a `deinstall ok config-files` sudo entry and the test checks one sudo entry remains (failed before the fix, passes after). Host: `make check-rootfs-install` ok, 28/28 check-guest-* ok, guest_nro 0 warnings; the object layout is console-only code (host checks do not cover it). ELF `.superpowers/sdd/fex-switch2-guest-0.17.5.elf`.
- **0.17.4 (2026-11-25, console not run): reinstall fixes.** 0.17.2 console: Reinstall showed no progress screen; log stopped at "Removing the existing RootFS" (the recursive SD delete ran on the UI thread, no frames). `ConsoleEnv::WipeRootfs` (`launcher/RootfsInstallHost.cpp`) now deletes on a worker thread and pumps the UI every 16 ms; the step shows "Removing the existing RootFS". The wipe now runs before the free-space check (user request), so the old RootFS's space counts. Host: `make check-rootfs-install` ok, guest_nro 0 warnings. ELF `.superpowers/sdd/fex-switch2-guest-0.17.4.elf`. Open: Settings > Wipe RootFS still deletes on the UI thread (frozen until done).
- **0.17.3 (2026-11-25, console not run): UI fixes from the user's 0.17.1 notes.** Shell footer is the standard footer (`chrome::FooterY/FooterH`, terminal ends at 656); path chip as tall as Browse; focus ring clipped to the list (`Widget::RingClip`, file picker and Settings rows); file picker rows bound to slot `Index % PoolSize` (focus no longer falls to the sidebar when the list scrolls to the last entry; `TestPickerDownToLast`); Settings rows 74 du with the text centred 4 du above the middle, a rule above each section's first row, rules from the sidebar rule to the screen edge (also the About rule). Host: `make check-ui` ok, check-nro-assets ok, guest_nro 0 warnings. ELF `.superpowers/sdd/fex-switch2-guest-0.17.3.elf`.
- **Non-PIE executables (python3) deferred to a potential Milestone 7** (user, 2026-11-25): draft plan `docs/Milestone7Plan.md`, nothing implemented. Tracked gap: Ubuntu's python3.12 and other ET_EXEC binaries return ENOEXEC; python3-* maintainer scripts fail in `apt upgrade`.
- **0.17.2 (2026-11-25, console not run): 0.17.1 + signed archive index (item d).** ELF `.superpowers/sdd/fex-switch2-guest-0.17.2.elf`.
- **0.17.1 (2026-11-25, superseded by 0.17.2 before a console run): fixes from the 0.17.0 console run.** 0.17.0 console: UI works; log `.superpowers/sdd/ui-task7-console-0.17.0.log`. Fixed: Home FTP pill 24 du higher; Settings/About row focus ring wider and inside the row rules (`RingOutsetY`); file picker stale rows after a folder change (rows unbound in `Open`, `Loaded` flag instead of "was not done this frame", which also fixes the missing scroll range and auto-scroll); picker B = parent folder (closes at a root), X = cancel; one header/footer component (`ui/components/Chrome.{h,cpp}`: `DrawScreenHeader`, `DrawFullRule`, `chrome::` layout) used by Settings, FTP, RootFS setup, picker, license view, gallery; header and footer rules edge to edge; Home B exits without a dialog; footer segments 8 du apart; wheel scroll 12 -> 2.4 du per unit. Removed: Debian developer setup (`launcher/RootfsSetupHost.cpp`, manifest/URL code of `GuestRootfsSetup.h`, `tests/unit/GuestRootfsSetupTest.cpp`, `tools/CheckRootfsSetup.py`, `tools/RootfsManifest.py`, `check-rootfs-setup`, `/fex/setup` staging) and the embedded-guest and JIT-probe developer rows. RootFS install: extras + sudo, nano, wget, whiptail (git is in the image); `/etc/sudoers.d/fex` (NOPASSWD, fex has no password); apt archive URI -> `mirror+http://mirrors.ubuntu.com/mirrors.txt` (security pocket unchanged). Host: `make check-ui` ok, `make check-rootfs-install` ok, 28/28 `check-guest-*` ok, `check-nro-assets` ok, guest_nro 0 warnings. ELF `.superpowers/sdd/fex-switch2-guest-0.17.1.elf`. **Open from the 0.17.0 log:** (a) python3 does not run: Ubuntu's `/usr/bin/python3.12` is non-PIE (ET_EXEC at 0x400000), `LoadElf` returns ENOEXEC, dash then runs py3clean/py3compile as sh scripts -> python3-* maintainer scripts fail during `apt upgrade`. Needs fixed low guest addresses (docs/Milestone3Sdk.md: not mappable on the console) -> guest/host address translation; milestone-sized, not planned yet. (b) apt "posix_openpt (2: ENOENT)": no /dev/ptmx until UI Task 8. (c) apt lock EACCES as user fex is correct (use sudo). (d) Fixed in 0.17.2: the extras' `InRelease` is now verified (`runtime/fs/OpenPgp.h`: SHA-512, RSA e=65537, cleartext framework, v4 signature packets) against the embedded Ubuntu Archive Automatic Signing Key (2018), fingerprint F6ECB3762474EDA9D21B7022871920D1991BC93C (`runtime/fs/UbuntuArchiveKey.h`); only the signed text is parsed. Checked by hand against the live noble and noble-updates InRelease (valid, ~8 ms each; one flipped byte rejected). Fixture archive is signed with a throwaway RSA-2048 key (`tools/CheckRootfsInstall.py`); `install_extras` passes, tampered / other-key / wrong-key InRelease are refused. Remaining: no Valid-Until in Ubuntu's InRelease, so an attacker can replay an older genuine index (old but genuine packages). Still open: the RootFS image itself is checked only by XXH3-64 (not cryptographic) over HTTP, as FEX's RootFS_links.json provides it. The machine-id RNG finding does not apply on the console (`RootfsInstallHost.cpp` uses libnx `randomGet`). Next: user's 0.17.2 console run (same checklist as 0.17.1), then RESUME below.
- **0.17.0 (2026-11-25, console run, see 0.17.1): UI Tasks 6 and 7 in.** New Vulkan launcher UI replaces the text menu (reports `.superpowers/sdd/ui-task6-report.md`, `ui-task7-report.md`; console checklist draft in the Task 7 report). Host: `make check-ui` ok (70 PNGs), `make check-nro-assets` ok (51 embedded files), all `check-guest-*` and `check-rootfs-install` pass, guest_nro 0 warnings. ELF `.superpowers/sdd/fex-switch2-guest-0.17.0.elf`. **PAUSED (user, token limit).** RESUME: (1) collect the user's 0.17.0 console result; (2) UI Task 8 (ptys + shell tabs, LauncherUiPlan.md) via one Sonnet agent; (3) one Sonnet review of the whole UI diff (minor list in progress.md); (4) orchestrator fixes, console acceptance, one commit.
- **0.16.12 console accepted for UI Task 5 (2026-11-25).** Log `.superpowers/sdd/ui-task5-console-0.16.12.log`: Ubuntu 24.04 install complete (reused the kept image, verify ok, 26773 files / 3741 folders / 9009 links / 5 hard links, Unbreak, setup incl. `/dev/pts`, 4 extra packages unpacked and configured, step 7/7). SD time for the tree still 497 s (journal appends 83 s); further speed work deferred. apt/dpkg pty message: not fixed at the user's request (no pseudo-terminals, ContainerInventory.md). **Next: UI Task 6** (one Sonnet agent, report `.superpowers/sdd/ui-task6-report.md`), then Task 7, one Sonnet review, orchestrator fixes and console acceptance, one commit.
- **0.16.12 (2026-11-25, console not run).** 0.16.11 console install (log `.superpowers/sdd/ui-task5-console-0.16.11.log`): extraction completed (26773 files, 3741 folders, 9009 links, 5 hard links; no 0xd401: the SdPath fix holds), Unbreak ok, then `Could not create a mount point: /dev/pts (error 13)`: `/dev` in the RootFS is the runtime's virtual directory. SD time still 504 s for the whole tree: `WriteInodeFile` 27911 x 2 (68 s) and RecoverDirectoryRename 55830 (12 s) came from `MoveNativeEntry` (the writer writes `<name>.dpkg-new` and renames it), journal appends 42206 / 74.5 s without flush (the write itself costs 1.8 ms), SdWriteGuest 52852 / 77 s, EntryType 159059 / 27 s (was 608k / 102 s for 60% of the tree). Fixes: (1) `MoveNativeEntry` in BulkCreates mode skips the intent, the recovery and the full copy of `SdInodes` kept for rollback (a copy of every name per rename). (2) `/dev/pts` is a virtual empty directory (`PtsDirectory`: listed in `/dev`, mode 0755, statfs type devpts 0x1cd1, creating in it EOPNOTSUPP like `/dev`); the installer's MakeDirectory keeps an existing directory. `DescriptorFilesystem` now reports tmpfs for `/dev/shm` too (was proc). Open: no pseudo-terminals (`/dev/ptmx`), which apt/dpkg in Bash report; tracked in ContainerInventory.md. Open (normal mode): `MoveNativeEntry` copies all names per rename for rollback. Host: all `check-guest-*` and `check-rootfs-install` pass (they do not exercise the /dev code); guest_nro 0 warnings. ELF `.superpowers/sdd/fex-switch2-guest-0.16.12.elf`.
- **0.16.11 (2026-11-25, console not run): RootFS writer skips the create protocol (user decision).** `GuestSyscalls::BulkCreates`, set by `OpenGuestRootfsWriter` (RootFS install and Debian rootfs setup): `CreateNativeEntry` creates the entry under its final name, then `CheckpointNewEntry`; on a store error the entry is deleted again. No intent file, staging entry, rename or recovery (0.16.8: about 53 s of 405 s SD time). Journal frames are written without FsWriteOption_Flush (0.16.8: 42 s); the session-end checkpoint writes the whole store. A crash mid-install can leave native entries without records (default metadata) or lose unflushed frames; the install marker is absent then and the next install wipes the tree. Revisit if crashes show up. Normal guest programs keep the full protocol. Host: all `check-guest-*` and `check-rootfs-install` pass; guest_nro 0 warnings. ELF `.superpowers/sdd/fex-switch2-guest-0.16.11.elf`.
- **0.16.10 (2026-11-25, console not run): SD directory cache.** Path walks checked every parent folder with fsFsGetEntryType (0.16.8: 608k calls, 102 s, 25 per entry). `SdDirectories` in GuestRuntime.cpp holds native paths known to be directories; `EntryType` answers from it and adds every directory it sees. `SdRetry` drops a path and everything below it on fsFsDeleteDirectory, fsFsDeleteDirectoryRecursively, fsFsCleanDirectoryRecursively (contents only) and fsFsRenameDirectory (source); `LoadInodeStore` clears it at session start (the launcher deletes folders directly); 65536-entry cap. Only one thread does SD work at a time (the launcher joins the guest worker), so no lock. Known gap: a folder removed by another FTP server while a session runs stays cached; the call on the path itself still fails with ENOENT. Not changed: the crash-safe create protocol (intent file + staging entry + rename: about 53 s of 405 s) and the flushed journal append (42 s). Also (user request): a cancel after the hash check keeps `/fex/.download/<image>` (`Installer::Verified`); the next install reuses it. `install_cancel_cleanup` checks both cases and the reuse. Host: all `check-guest-*` and `check-rootfs-install` pass; guest_nro 0 warnings. ELF `.superpowers/sdd/fex-switch2-guest-0.16.10.elf`.
- **0.16.9 (2026-11-25, console not run): root cause of SD 0xd401.** 0.16.8 console install (log `.superpowers/sdd/ui-task5-console-0.16.8.log`) failed on the same entry as 0.16.6 (`/usr/share/icons/Humanity/mimes/22/text-x-perl.svg`, a symlink, so its EntryType goes to `/fex/.inode/objects/<inode>`), after the same number of SD calls (1362145 against 1362161), and all three retries at 1/10/100 ms failed: deterministic. Cause: libounce `fsFsGetEntryType` (and the other path calls) hands the caller's pointer to the kernel as an IPC pointer buffer of FS_MAX_PATH (0x301) bytes without copying (`mov w0, #0x3010000` before `svcSendSyncRequest`). The failing path string was at 0x65f690fd80, 640 bytes before the page boundary 0x65f6910000, so the kernel's read ran past the string into the next page and returned InvalidMemoryState. Fix: `host/SdPath.h` (`SdPath` copies a path into a 0x301-byte buffer; `SdArgument` picks parameters by the fs function's own types, so `const void*` data buffers are passed through). `SdRetry` now copies all path parameters, and the launcher's direct fs calls (`GuestProbe.cpp`, `RootfsInstallHost.cpp`) wrap paths in `SdPath`. The 0.16.7 delayed-retry experiment is reverted (no retry helped); 0xd401 is still logged with caller and pointer states. This is probably also the cause of the earlier intermittent 0xd401 (M2 SD retry hook, M4 sparse-exec EIO). Perf sites from 0.16.8: EntryType 608102 calls / 102 s (25 per entry), SdWriteGuest 41045 / 60 s, journal append 42 s, `WriteInodeFile` 2 x 22 s (store slot writes), RecoverDirectoryRename 35650 / 7.7 s. Host: all `check-guest-*` and `check-rootfs-install` pass; guest_nro 0 warnings; 16 `strncpy` before `fsFsGetEntryType` call sites in the ELF. ELF `.superpowers/sdd/fex-switch2-guest-0.16.9.elf`.
- **0.16.8 (2026-11-25, console not run).** Store directory back to `/fex/.inode` (was `/fex/.inode_store` since the old `/fex/.inode` broke). Root cause of both "corrupted" folders, confirmed on the console over OunceShell's FTP (192.168.178.29:1337): Horizon creates `NetLock_Arany_=Class_Gold=_Főtanúsítvány.crt` but never lists it, so its folder looks empty and RMD or recursive delete fails. `DELE` by exact name worked (250), then `RMD` worked for `/fex/rootfs/usr/share/ca-certificates/mozilla` and for `/fex/.inode/dir-backup` (an old staged copy of a mozilla folder). The non-ASCII overflow routing in 0.16.7 is the fix. Host: all `check-guest-*` and `check-rootfs-install` pass; guest_nro 0 warnings. ELF `.superpowers/sdd/fex-switch2-guest-0.16.8.elf`.
- **0.16.7 (2026-11-25, console not run).** 0.16.6 console install (log `.superpowers/sdd/ui-task5-console-0.16.6.log`): failed after about 24k of 39528 entries with SD 0xd401 on fsFsGetEntryType (`EntryType`, nro+0x1b824; all 3 immediate tries failed), then `Could not delete /fex/rootfs (0x00001002)`. Perf: 1.36M SD calls / 407 s (about 57 per entry, 0.3 ms each); journal 23962 frames / 47 s (12%); 3 checkpoints / 0.26 s. The inode store is no longer the main cost. Afterwards `/fex/rootfs/usr/share/ca-certificates/mozilla` cannot be listed by our FTP or OunceShell's FTP (the user cannot take the card out to check it on a PC). That folder holds `NetLock_Arany_=Class_Gold=_Főtanúsítvány.crt`, one of only 2 non-ASCII names in the image (valid NFC UTF-8); the other is a symlink, stored as an object. Fixes: `SdNameUnsafe` sends names with any byte >= 0x80 to the overflow store. Diagnostics (EXPERIMENT): `Perf: SD site nro+...` (top 15 call sites by count); 0xd401 retried after 1/10/100 ms with a `cleared on retry N` line. Host: all `check-guest-*` and `check-rootfs-install` pass; guest_nro 0 warnings. ELF `.superpowers/sdd/fex-switch2-guest-0.16.7.elf`.
- **0.16.6 (2026-11-25, console not run): inode store write-ahead journal.** Replaces the full-snapshot save on every metadata change (O(N^2) for N changes; stalled the Ubuntu extraction, slows small-file FTP and shell work). Design (GuestInodeStore.h "Write-ahead journal", after SQLite WAL / ext4 jbd2; research notes in this conversation: UMSDOS, posixovl, pseudo, WSL DrvFs, git fan-out): `SdInodes`/`Inodes` are `TrackedMap`s that record every key changed through them (lookups return const iterators, so the compiler finds every write; `Edit()` for writes through an iterator; swap and whole-map assignment force a checkpoint). `SaveInodeStore` appends one checksummed frame to `/fex/.inode_store/journal` with only the changed names, linked records and retired objects (retired objects at object level, so in-memory and decoded representations agree), and advances the generation, so the intent protocols are unchanged. `WriteCheckpoint` writes the full snapshot into the slot that does not hold the newest one and empties the journal: when the journal passes 4 MiB, when a map was replaced wholesale, at the end of each guest session, and at load after a crash (after intent recovery). Load: newest slot + replay of the frames that follow (stale leading frames skipped, stop at a torn frame or gap, journal cut after the last consumed frame); the replayed store is encoded and decoded as a snapshot again, so it passes the same checks. Unlinked-object reclaim keeps "both slots retire it" (now: newest and the other checkpoint). The 0.16.4/0.16.5 bulk-writer deferral is removed. No migration (user resets `/fex`). Host: new journal model test in `check-guest-inode-store` (40 seeds x 400 random ops: create, managed file, hard link, rename via extract/insert, unlink with managed retire, chmod/chown/utimens, xattr via Edit, reclaim, backing-object replacement, runtime-only inodes, swap, subtree merge; replay equals memory after every save; a journal cut at a random byte replays to the last complete save; a frame for one create in a 3000-entry store is under 200 bytes); mutation check: dropping retired adds, name removals or record-touch collection fails the test (dropping record removals does not: the snapshot encoder already ignores unnamed records). All 28 `check-guest-*`, `check-rootfs-install` pass; guest_nro 0 warnings. Perf counters (0.16.3 EXPERIMENT) still log `Perf: ... Inode journal: N frames ...; M checkpoints ...` at session end. ELF `.superpowers/sdd/fex-switch2-guest-0.16.6.elf`. Console checklist: reset `/fex`, banner `launcher 0.16.6`, Ubuntu install (expect steady extraction), Bash checks and timing, log.
- **0.16.5 (2026-11-25, console not run).** 0.16.4 console (log `.superpowers/sdd/ui-task5-console-0.16.4.log`): install failed at once, `Could not write to the RootFS: /Unknown_Packages (error 2)`. Cause: native creation publishes an intent for generation + 1, creates the staging entry, checkpoints, then `RecoverDirectoryRename` finishes or rolls back by comparing the in-memory `InodeGeneration`; the deferred save did not advance it, so every create was rolled back. 0.16.5: a deferred save advances `InodeGeneration` in memory and writes nothing; the store stays dirty and the writer's final save writes the next generation. guest_nro 0 warnings. ELF `.superpowers/sdd/fex-switch2-guest-0.16.5.elf`.
- **0.16.4 (2026-11-25, console not run).** The 0.16.2 Ubuntu install slowed to a near stop during extraction. Cause (from code): every created entry goes through `CheckpointNewEntry`, which calls `SaveInodeStore`, which encodes and rewrites the whole, growing store, so N entries cost O(N^2) SD writes. Fix: `OpenGuestRootfsWriter` (RootFS install and Debian setup) sets `GuestSyscalls::DeferInodeSaves`; the store is saved once when the writer is destroyed (before `dpkg --configure -a` runs the guest, and before the cancel/failure cleanup wipes). A crash before that save loses only this run's records; both flows start over by wiping. The FTP server and guest programs still save per operation (the general fix waits for the 0.16.3 `Perf:` numbers). Keeps the 0.16.3 perf counters. guest_nro 0 warnings; `check-rootfs-install` passes. ELF `.superpowers/sdd/fex-switch2-guest-0.16.4.elf`.
- **0.16.3 (2026-11-25, diagnostic EXPERIMENT, console not run).** 0.16.2 console FTP: `FTP STOR 335352744 bytes in 14.03 s (22.80 MiB/s), file system 13.72 s` (log `.superpowers/sdd/ui-task5-console-0.16.2.log`): the guest file layer, not the network, is the FTP bottleneck. User: small files are much slower, and Bash has been slower since milestone 3. Suspects from the code: `SaveInodeStore` encodes and rewrites the whole inode snapshot (all names and records) to the SD on every metadata change; `MarkInodeAccessed` uses `Inodes[Inode]`, so every read file gains a record (the store grows with every file read) and marks the store dirty; guest file data moves through the SD in 64 KiB bounce chunks. 0.16.3 measures, without changing behavior: SD calls through `SdRetry` (count, time), inode store saves (count, KiB, time, encode time), names and records, and the 20 syscalls with the most total time, logged unbuffered as `Perf:` lines when the guest session ends. ELF `.superpowers/sdd/fex-switch2-guest-0.16.3.elf`. Revert after the cause is found.
- **0.16.2 (2026-11-25).** Console probe on 0.16.1 (log `.superpowers/sdd/ui-task5-probe-0.16.1.log`): all 8 cases pass, whole image 501.3 MiB in 150.4 s = 3.33 MiB/s, XXH3 matches; the 0.16.0 stall is gone. Socket-buffer sweep (log `.superpowers/sdd/ui-task5-probe-0.16.2.log`, 64 MiB each, 100 Mbit line): SO_RCVBUF 64 KiB 3.34 MiB/s, 256 KiB 11.92, 512 KiB 11.59, 1 MiB 11.62. `TransferSocketBuffer` default is now 256 KiB (FTP data sockets and the HTTP client). Launcher 0.16.2 built, 0 warnings; `check-guest-ftp`, `check-rootfs-install` pass. ELF `.superpowers/sdd/fex-switch2-guest-0.16.2.elf`. Console next: FTP speed (RETR/STOR log lines) and the Task 5 install checklist with banner 0.16.2.
- **0.16.1 (2026-11-25, console not run).** On 0.16.0 the download probe sat for several minutes between PASS 3 and PASS 4 (32 MiB; no progress output then). The user reports OunceShell's FTP server is much faster than ours. OunceShell ftpd (`switch2/third_party/ftpd/ftp.c`, `ftp_set_socket_options`) sets SO_RCVBUF/SO_SNDBUF 64 KiB and TCP_NODELAY on each data socket; we set neither. 0.16.1: new `host/SocketTuning.h` (`TuneTransferSocket`) applied to the FTP PASV listener and accepted data socket and to the HTTP client socket before connect. FTP logs one line per RETR/STOR with bytes, time, MiB/s and the time spent in the guest file layer (to tell network from file-layer cost). The download probe prints `started` and a `progress:` line every 2 s. Host: `check-guest-ftp`, `check-rootfs-install`, `check-ui` pass; guest_nro 0 warnings. ELF `.superpowers/sdd/fex-switch2-guest-0.16.1.elf`. EXPERIMENT until the console shows a speed-up.
- **UI Task 5 console run pending (launcher 0.16.0, 2026-11-25).** Tasks 4 and 4b (SD root `sdmc:/fex`; settings = real project features only: Reinstall/Wipe RootFS, Start folder, Theme, Accent, Log level; ui.json only) and Task 5 (FEX Ubuntu 24.04 SquashFS installer in the text launcher, Home item 6; download probe `build/fex-switch2-downloadprobe.nro`) are done and uncommitted. Host: `check-ui`, `check-rootfs-install` and all `check-*` pass except `check-proc-suite-native` (`clock/monotonic_never_decreases`, host suspend, pre-existing). Native: real image installed on host (39502 entries match unsquashfs except the 5 setup files), extras install, CommTools.sh passes in that tree. Console checklist: `.superpowers/sdd/ui-task5-report.md` "Console checklist" (banner is `launcher 0.16.0`, not 0.15.12). Logs to save: `.superpowers/sdd/ui-task5-probe.log`, `.superpowers/sdd/ui-task5-console.log`; ELF `.superpowers/sdd/fex-switch2-guest-0.16.0.elf`. Install wipes `/fex/rootfs` and `/fex/.inode_store`. Open for the fix pass: Wipe RootFS copy mentions desktops and game stores; Task 7 wires LauncherBackend (RootfsImages, FetchLiveRootfsIndex, InstallRootfs, WipeRootfs). Next after the console run: Task 6.
- **RESTART HERE: Launcher UI (2026-11-25).** Plan: `Source/Switch2/docs/LauncherUiPlan.md`. Task reports: `.superpowers/sdd/ui-task{1,1.5,2,3}-report.md`. Nothing has been committed since M4 (`c772c11e9`, pushed to `origin` = github.com/LimoDerEchte/FEX-nx2, a public fork; `upstream` = FEX-Emu/FEX). The folder restructure and the UI work are uncommitted on `nx2`; per AGENTS.md they are committed once, after UI acceptance.
  - Done: Task 1 (platform abstraction plus Vulkan renderer; console probe 2 passed). Task 1.5 (restructure). Task 2 (fonts, icons, theme; swatch PNGs in `Source/Switch2/build/ui-check/`). Task 3 (input, focus, components, Joy-Con mouse through Nmouse/PadPointer).
  - Console probe 3 (log `.superpowers/sdd/ui-probe3.log`, partly run by the user, everything tried worked): HarfBuzz kerning yes; Nmouse found both Joy-Cons (types 0x2, 0x4); the right Joy-Con drove the pointer; the stick wheel works (about 12-33 raw per frame); the user raised the gain to 1.56. Not yet checked on the console: the full gallery checklist, ZL/ZR-as-click, the 3 s release, a USB mouse. Input problems are to be debugged in later stages (user, 2026-11-25).
  - Minor items for the final review: (1) the host gallery PNG shows a focus ring on the "normal" icon-button demo as well as on "focused"; check whether that is the live focus ring. (2) Pointer gain default 1.0: the user used 1.56; pick a default after more console use. (3) Dark `toggle_off_track` has about 2:1 contrast (spec value kept). (4) No glyph fallback font. (5) FreeType/HarfBuzz license texts and `licenses.json` are due in Task 4 (About). (6) LicenseModal moved to Task 4.
  - **NEXT STEP: dispatch Task 4** (Home, Settings, About, File picker) to one fresh Sonnet agent, with the same prompt shape as Tasks 2 and 3. Requirements: AGENTS.md, plan sections Architecture, Global constraints, File structure and Task 4, and the specs in `docs/ui/`. Context: read the Task 1-3 reports and the `ui/` headers first. Settings rows come from the schema, and only settings the runtime honors are enabled. NRO-only: embed licenses. No commit, no version bump. Keep `guest_nro`, all `check-*`, `check-ui` and `ui-probe` green. Report to `.superpowers/sdd/ui-task4-report.md`. Task 4 is host-tested; it needs a console run only if the report says so. Then Task 5 (RootFS: FEX Ubuntu 24.04 SquashFS, zstd squashfs reader, unbreak step, extra packages, download probe; console-gated), then Tasks 6 and 7, one Sonnet review, the orchestrator's fixes, console acceptance and one commit.
- **UI Task 5 (RootFS images and installer) implemented, console run pending (2026-11-25).** Report and the console checklist: `.superpowers/sdd/ui-task5-report.md`. Code: `runtime/fs/GuestSquashfs.h`, `runtime/fs/GuestRootfsInstall.h`, `host/HttpClient.h`, `launcher/RootfsInstallHost.cpp` (home menu "Install Ubuntu 24.04 RootFS"), `ui/RootfsIndex.*` + `ui/assets/rootfs-index.json`, `ThirdParty/zstd`, `probes/DownloadProbe.cpp` (`make download-probe`, EXPERIMENT), format notes `docs/ui/rootfs-image-format.md`. Host: `make check-rootfs-install` (fixture squashfs vs unsquashfs, pipeline scenarios), real Ubuntu 24.04 image extracted natively and `CommTools.sh`/`ProcessTools.sh` run in it. Console order: (1) `make download-probe` NRO, (2) the launcher NRO, install from the home menu. Next after the console passes: Task 6.
- **UI Task 1 passed on the console (2026-11-25).** The Vulkan probe ran at 2560x1440 docked and 1920x1080 handheld; dock and undock work; touch, buttons, FreeType and the clock work; the Joy-Con mouse is not reported (tracked in the plan). Log: `.superpowers/sdd/ui-probe2.log`. Next: Task 2 (fonts, icons, theme).
- **Launcher UI, 2026-11-25 update.** UI probe 1 (SDL2) failed on the console: SDL fell back to `offscreen`, then "Could not find a valid EGL device" (log `.superpowers/sdd/ui-probe1.log`). User decision: render with Vulkan as ounce-hbmenu does, behind a platform abstraction (`ui/platform/` Display, Input and System; `ui/render/` Renderer). Plan updated: `docs/LauncherUiPlan.md`, Task 1 redone. Task 1.5 restructure done (report `.superpowers/sdd/ui-task1.5-report.md`): the 33 check targets pass, guest_nro has 0 warnings, the filesystem-runtime file list is unchanged. That task also fixed the HostDiskCache path in `FEXCore/Source/CMakeLists.txt`.
- **Launcher UI started (2026-11-25).** Plan [LauncherUiPlan.md](docs/LauncherUiPlan.md), Task 1 (SDL2 on libounce plus `make ui-probe`) is with a Sonnet agent; its report goes to `.superpowers/sdd/ui-task1-report.md`. User decisions: RootFS images come from the FEX RootFS project (`http://rootfs.fex-emu.gg/RootFS_links.json`, plain HTTP works). Ubuntu 24.04 SquashFS (zstd, XXH3 hash) is the only selectable RootFS. FEX has no Debian image, so the Debian 12 card is disabled and the in-app Debian setup is dropped. Extra packages are limited to what the tests and tools need; audio and graphics come through the FEX layer. Task 5 has been rewritten to match.
- **Milestone 4 accepted (2026-11-25, launcher 0.15.12).** Console log `.superpowers/sdd/m4-console13.log`: comm `suite 42, tools 42`, filesystem `ABI 42, coreutils 0, suite 42`, container `suite 42, tools 42` (process `suite 42, tools 42` on 0.15.11). No SD 0xd401 in this run, so the comm-tools crash and the sparse-exec EIO of 0.15.11 did not reproduce. Both stay open as intermittent: the SD error is open since 0.13.5, and the comm crash happened at the same spot as the 0.14.2 hang. For the commit, the exec log line and the retry backoff were removed (no evidence); the unbuffered `SdRetry` lines with pointer memory state stay as a diagnostic. Next: the launcher UI plan.
- **0.15.12 (2026-11-25, diagnostic EXPERIMENT, console not run).** Correction to the 0.15.11 notes: the `SdRetry` retry lines go through `WriteLog`, which is buffered during a guest run, and that run's log shows `Guest log omitted 37863 older lines`. The missing retry lines therefore prove nothing: the sparse-exec read most likely went through `SdRetry` and failed all three tries. Every SD file call in `GuestRuntime.cpp` uses `SdRetry`, and guest file data uses the host-heap bounce buffer. All guest work runs on one host worker thread, so concurrent IPC on the bounce buffer is ruled out. 0.15.12 changes: `SdRetry` writes each retry with `WriteRunCheckpoint`, which is unbuffered and survives trimming and crashes. Each retry line holds the caller (`nro+0x...`, resolve with `.superpowers/sdd/fex-switch2-guest-0.15.12.elf`) and `svcQueryMemory` type, attr and perm for every pointer argument. Retries now back off 1 ms, then 20 ms. A failure on the last try logs `SD result 2001-0106 persisted after 2 retries.` Each guest exec also logs `exec pid N: <program> <argv1>` unbuffered, so a crash shows the last program started. The `WriteRunCheckpoint` line buffer grew from 256 to 640 bytes. Revert the exec line once the comm crash is found. Console checklist: (1) banner `launcher 0.15.12` (copy only the NRO; the SD bundle is unchanged); (2) `/bin/comm-acceptance.elf`, expect `Communication acceptance results: suite 42, tools 42`; if it crashes, the last `exec pid` line names the step; (3) `/bin/filesystem-acceptance.elf`, expect `ABI 42, coreutils 0, suite 42`; on an SD 0xd401, the retry lines name the call and buffer state.
- **0.15.11 console (2026-11-24), NEXT STEPS.** Log `.superpowers/sdd/m4-console12.log`; ELF for symbolization `.superpowers/sdd/fex-switch2-guest-0.15.11.elf`.
  - PASSED: `/bin/container-acceptance.elf` `suite 42, tools 42` (container suite 54/0/0 as uid 1000 and as root). `/bin/process-acceptance.elf` `suite 42, tools 42` (proc-suite 243 passed, 0 failed, 7 skipped).
  - FAILED 1, comm: `/bin/comm-acceptance.elf` crashed after `# comm-suite summary: 124 passed, 0 failed, 1 skipped` (log line ~3607), during `comm-tools.sh`. The buffered runtime trace was lost: no `Host fault` line and no syscall trace, and the next line is a new launcher start. M3 0.14.2 once hung at the same point (the xz test in `CommTools.sh`). Next: rerun alone; if it reproduces, run `comm-tools.sh` steps one at a time (root shell: `bash --noprofile --norc /bin/comm-tools.sh ./x`) to find the step. Check whether the 0.15.x changes (fd links, LinkPath, DropStaleSymlink, setgroups inheritance) touch it; they look unrelated.
  - FAILED 2, filesystem: `ABI 1, coreutils 0, suite 42`. The only ABI failure is the probe case `sparse-exec`: `SD result 2001-0106 (0x0000d401) reported as EIO` and then `libc.so.6: failed to map segment from shared object` (mmap EIO). This is the same intermittent kernel InvalidMemoryState as the unshare/nsenter failures in 0.15.9 (open since 0.13.5). There are NO `SD result 2001-0106, retry` lines here, so the failing SD call does not go through `SdRetry` (the `caller nro+...` diagnostic therefore did not fire). Next: find the guest mmap/file-backed mapping path (FileExtents / mapping of file pages, `GuestFileExtents.h`, mmap in `GuestRuntime.cpp`) and which libnx fs call it uses without `SdRetry`; wrap it or log its caller, then trace the cause (a buffer in a memory state the fs IPC rejects; host-heap bounce buffers are already used for read/write).
  - When comm and filesystem pass: update `ContainerInventory.md` "Console verification still required", then commit milestone 4 once on `nx2` (the `wip/m4` branch holds the pre-revert state; delete it after the commit if it is not needed).
  - SD card state for these runs: prepackaged rootfs copied over a setup rootfs; `chown 1000:1000 /` was done in the root shell (old 0:0 record from 0.15.2-0.15.7).
- **0.15.11 (2026-11-24, console not run):** 0.15.10 console: suite 54/0/0; the bwrap `/dev/null` bind now works; 5 bwrap checks fail with `setting up gid map: Operation not permitted`. bwrap with `--dev` maps 0 inside, then unshares a second user namespace and writes `gid_map` "1000 0 1", which Linux allows because the child inherits setgroups "deny" (`create_user_ns`: `ns->flags = parent_ns->flags`). `CreateUserNs` did not inherit it; fixed, host test in `GuestNamespacesTest.cpp`. No SD 0xd401 in this run; unshare and nsenter passed. Log `.superpowers/sdd/m4-console11.log`.
- **0.15.10 (2026-11-24, console not run):** 0.15.9 console (after `chown 1000:1000 /` in the root shell, because builds 0.15.2 to 0.15.7 had saved owner 0:0 for `/`): container suite 54/0/0 as uid 1000 and as root; acceptance `suite 42, tools 1`. Tool failures: 6 bwrap checks (`Unable to find "/dev/null" in mount table`: the `/proc/self/fd` link of a bound device printed its guest name, not the mount point); `unshare` exec and an `nsenter` library mmap got SD result 2001-0106 (0xd401) on all retries, reported as EIO (open since 0.13.5, cause unknown); `nsenter_same_namespace` failed as a consequence. 0.15.10: device and /proc opens keep their resolved path (`OpenFile::LinkPath`) for the fd link; as a diagnostic, the SD retry log names the calling code (`caller nro+0x...`; the matching ELF is saved as `.superpowers/sdd/fex-switch2-guest-0.15.10.elf`). Log: `.superpowers/sdd/m4-console10.log`. Console checklist: banner 0.15.10; `/bin/container-acceptance.elf` -> `suite 42, tools 42`; then the regressions.
- **0.15.9 (2026-11-24, console not run):** 0.15.8 console: `/fex/rootfs/bin/container-acceptance.elf` gave "Could not resolve the guest ELF file." The user had copied the prepackaged rootfs over a rootfs from the setup: the inode store still held link records for `/bin` (and others) while the SD card now had real folders there. 0.15.9 drops a link record when the SD entry is a folder (`DropStaleSymlink` in the path walk, logged as "Inode store: ... dropped its stale link record."). Console checklist as for 0.15.8, banner `launcher 0.15.9`; expect that log line for `/fex/rootfs/bin` on the first launch.
- **Partial revert (0.15.8, 2026-11-24, console not run):** the user reported that 0.15.7 broke the file structure and the rootfs setup. Everything as of 0.15.7 is saved on branch `wip/m4` (4e9f3ddeb). 0.15.8 keeps the milestone 4 runtime: namespaces, mounts, seccomp, scheduling, the `/proc` view and `/proc/<pid>/fd` fixes, failed-path logging, `.inode_store` and orphan set-aside, and the `/fex/.tmp` tmpfs folders. It reverts to the HEAD (8d822c1b8) behavior: the rootfs setup (unpack only; the debootstrap second stage and merged /usr are on `wip/m4`), the test layout in `rootfs/bin` with absolute paths, and the cwd of the acceptance programs. It removes the launch-time base folder repair and the root owner of `/` (a known difference, see ContainerInventory.md). The staged rootfs gets an empty `tmp` folder for bwrap. Native packaged (uid 1000): container `suite 42, tools 1` (userns refused inside the chroot, expected); container suite-root 42; comm `42, 42`; filesystem `ABI 42, coreutils 0, suite 42`; process `suite 1, tools 42` (only `clock/monotonic_never_decreases`, because the host was suspended). The host checks pass and `guest_nro` has 0 warnings. Console checklist: 1. copy the NRO and `build/guest/sd/fex/`; on the SD card, delete `/fex/games/tests` and any rootfs from the 0.15.7 setup, and use the prepackaged rootfs; 2. banner `launcher 0.15.8`; 3. `/bin/container-acceptance.elf` -> `suite 42, tools 42`; 4. `/bin/container-suite-root.elf` -> exit 42; 5. regressions `/bin/process-acceptance.elf`, `/bin/comm-acceptance.elf`, `/bin/filesystem-acceptance.elf` as listed below. Then commit milestone 4.
- **Milestone 4: console run pending (0.15.6, 2026-11-24).** 0.15.5 console: repair created `/tmp`, `/var/tmp` failed ENOENT (`/var` missing in the console rootfs); uid 1000 suite still EACCES on its scratch mkdir (cause open; 0.15.6 prints cwd, uid, owner and mode); bwrap now fails at `readlink(/proc/self/fd/N)` (0.15.6 adds `/proc/<pid>/fd/` links and listing, mount-aware paths). Repair continues past failures and logs each. 0.15.4 console: container-suite-root 54/0/0; tools all ok except 6 bwrap cases (`/tmp` missing in the console rootfs); uid 1000 suite bailed (scratch in `/` is EACCES since `/` is root-owned). 0.15.5: launcher recreates missing `/tmp` (1777), `/var/tmp` (1777), `/run` (0755) at rootfs launch (`RepairBaseDirectories`, logged); acceptance programs and the filesystem acceptance script work in a fresh `/tmp/<name>-acceptance.XXXXXX`; packaged native runner provides `/tmp` and the launcher char devices. Packaged native 0.15.5 (uid 1000): container `suite 42, tools 1` (userns refused in the chroot, expected), process `suite 1, tools 42` (only `clock/monotonic_never_decreases`: host was suspended), comm `42, 42`, filesystem `ABI 42, coreutils 0, suite 42`. 0.15.3 console (old suite binary, SD bundle not copied): `/proc/self/...` ENOENT from a process in a nested PID namespace (fixed in 0.15.4: the walk ended its `ProcViewScope` before the `/proc/<pid>` directory check); bwrap `mount("tmpfs", "/tmp")` ENOENT from the target lookup (open). tmpfs backing folder renamed `/fex/.tmp` (old `/fex/.t`, `/fex/.tmpfs` removed at start). 0.15.2 console: suite 53/1 (`ns/pid_init_numbers`) as uid 1000 and root; tools: bwrap `Failed to mount tmpfs: ENOENT`, `unshare -Urpf --mount-proc` `/proc/self/uid_map` ENOENT. 0.15.3 adds diagnostics: failed path syscalls log `path=` (mount also source/type/flags, /proc paths the PID-namespace view), tmpfs folder creation failures, and the pid test child prints its values. Debugging is done by the orchestrator, not subagents. 0.15.1 console: container suite 50/4 (uid 1000) and 50/4 (root), tools 8 failures; fixes in `.superpowers/sdd/m4-console1-report.md`. 0.15.2 also sets aside a staging `dir-backup` with no intent into `/fex/.inode_store/orphans/`, logs refused creates, and (EXPERIMENT, this build only) tries to delete the old `/fex/.inode`. 0.15.0 console: every create failed EIO (corrupt `/fex/.inode`); 0.15.1 moves the inode store to `/fex/.inode_store` with no migration. Tasks 1-4,
  review and fixes done (uncommitted). Suite `tests/guest/ContainerSuite.c`
  (54 cases), `ContainerTools.sh` (20 checks), `ContainerAcceptance.c`;
  report `.superpowers/sdd/m4-suite-report.md`. Native: container-suite
  50/0/4 as uid 1000 and namespace root; tools 20/20. Packaged native:
  33/0/21 (17 skips: `unshare(CLONE_NEWUSER)` refused inside the native
  chroot), tools 14/20 for the same reason, so the console is the first
  evidence for namespaces. All `check-guest-*`, `check-comm-suite-native`,
  `guest_nro` 0 warnings pass. Artifacts: `build/guest/fex-switch2-guest.nro`
  and `build/guest/sd/fex/`. Console checklist: 1. banner `launcher 0.15.7`;
  2. `/bin/container-acceptance.elf` -> `Container acceptance results:
  suite 42, tools 42`; 3. `/bin/container-suite-root.elf` -> exit 42;
  4. regressions `/bin/process-acceptance.elf` (`suite 42, tools 42`),
  `/bin/comm-acceptance.elf` (`Communication acceptance results: suite 42,
  tools 42`), `/bin/filesystem-acceptance.elf` (`ABI 42, coreutils 0,
  suite 42`). Then commit milestone 4 once.
- **After milestone 4 commits:** the launcher UI plan
  [LauncherUiPlan.md](docs/LauncherUiPlan.md) (spec from the user,
  `~/Downloads/fex-switch2-ui-spec.md` + theming spec; Task 1 copies them
  to `docs/ui/`). Backend: SDL2 2.28.5 switch port rebuilt for libounce +
  switch2 EGL/GLES2. Task 1 is a console-gated probe.
- **Next.** Milestone 4, containers and scheduling
  ([Milestone4Plan.md](docs/Milestone4Plan.md)): namespaces, mounts,
  `pivot_root`/`chroot`, seccomp, `sched_*`, and Linux errors for the
  machine-wide calls. Then milestone 5, a host thread per guest thread (Task 0) and 32-bit guests
  ([Milestone5Plan.md](docs/Milestone5Plan.md)), and milestone 6, `/proc`,
  `/sys`, `/dev` and input ([Milestone6Plan.md](docs/Milestone6Plan.md)). The
  90 uninventoried syscalls and the open rows of milestones 1–3 are in
  [RemainingSyscalls.md](docs/RemainingSyscalls.md). Process for each
  milestone (AGENTS.md): sequential Sonnet task agents, one Sonnet review
  of the whole milestone, then Opus writes the suite and runs console
  debugging. `ptrace` is future work; graphics is in no milestone.
- **Status (2026-11-23).** Milestone 2 (processes and events) is
  **complete** and committed on `nx2`. Console acceptance on 0.13.17-diag:
  `/bin/process-acceptance.elf` `suite 42, tools 42` (proc-suite 240 passed,
  0 failed, 8 skipped), interactive Bash checklist including Ctrl+C on a
  foreground busy loop; 0.13.16-diag: proc-suite-root 246/0/2,
  filesystem-acceptance `ABI 42, coreutils 0, suite 42`. Native: proc-suite
  241/0/7 (uid 1000), 245/0/3 (namespace root); fs-suite 81/0/2.
  The commit is **0.13.18**: the EXPERIMENT SD diagnostics
  (`SdBufferError`, `SdPathError`) are removed and every Result-returning
  libnx SD call goes through `SdRetry`, which retries `2001-0106`
  (InvalidMemoryState, 0xd401) up to two times and logs each retry (the
  user's decision; the cause stays unknown). 0.13.18 passed the host checks
  and builds; it has not had its own console run.
- **Milestone 3 history.** Milestone 3, memory and communication (virtual memory, pipes
  and IPC, sockets; see
  [Active milestone sequence](#active-milestone-sequence--linux-guest-kernel-compatibility)).
  The plan is [Milestone3Plan.md](docs/Milestone3Plan.md) (written
  2026-11-23): 5 Sonnet work packages, 5 Sonnet reviewers, then an Opus
  final pass with the suite and console run. Phase A: WP1 (memory arena,
  mapping calls, limits) is implemented in the working tree with host checks
  (`make -C Source/Switch2 check-guest-arena check-guest-memory`) and the
  `guest_nro` build passing; no console run yet. WP2 (futex, robust lists, futex2) is implemented in the
  working tree (`make -C Source/Switch2 check-guest-futex` and `guest_nro`
  passing, no console run). WP3 (pipes, System V IPC, POSIX message queues, `/dev/shm`) is implemented in the
  working tree (`make -C Source/Switch2 check-guest-sysv-ipc check-guest-mqueue check-guest-pipe` and `guest_nro`
  passing, no console run). WP4 (AF_UNIX stream, datagram and seqpacket, filesystem socket
  nodes, getsockname/getpeername, socket options, sendmmsg/recvmmsg) is implemented in the working tree
  (`make -C Source/Switch2 check-guest-socket` and `guest_nro` passing, no console run).
  WP5 (AF_INET TCP and UDP over nonblocking host BSD sockets: `GuestInet.h`,
  `HostNet.cpp`, `WaitKind::Net`, `/etc/resolv.conf` from nifm) is implemented in the working tree
  (`make -C Source/Switch2 check-guest-inet check-guest-socket check-guest-memory` and `guest_nro`
  passing, no console run); INADDR_ANY passes straight to the host (0.0.0.0 bind allowed, console-verified 2026-11-24 via BindProbe.cpp)
  (Milestone3Sdk.md item 10). Phase B review done 2026-11-24: merged
  `.superpowers/sdd/m3-review.md` (3 blocker, 13 major, 46 minor; per WP in
  `m3-review-wpN.md`). Next: Phase C1 (Opus) fixes, blockers first (all three
  are CPU-fault paths: mremap grow-in-place overflow, MAP_FIXED/Remap FIXED
  commit failure leaving R/W uncommitted pages, recvmsg Unix name write one
  byte past the check), then C2 CommSuite.c, C3, C4 console build 0.14.0.
  C1 done (`.superpowers/sdd/m3-c1-report.md`). C2+C3 done 2026-11-24
  (Sonnet, `.superpowers/sdd/m3-c2c3-report.md`): `tests/guest/CommSuite.c`
  (125 cases), `CommTools.sh`, `CommAcceptance.c`, staged by
  `make -C Source/Switch2 filesystem-runtime`. Guest-unverified native-adjusted
  asserts: second connect after nonblocking connect (0 then EISCONN),
  abstract names per socket type, `mq_open("/")` ENOENT, oversized mremap
  EINVAL. MREMAP_DONTUNMAP on shared anon stays EINVAL on guest (inventory
  row 25). **C4: console 0.14.0 failed: "Guest arena ... commit limit 0 MiB", "Could not allocate the guest stack" (loader heap override held all memory). Fixed in 0.14.1: HostArena.cpp overrides `__libnx_initheap`, shrinks the loader heap to 2 GiB, restores it at exit (`ShutdownGuestArena`); log line `Heap: loader override=...`. Console run of 0.14.1 pending.**
  **Console 0.14.1 ran: 9 failures (inet connect/poll/FIONREAD, membarrier registrations, mremap EINVAL, unix dgram EPERM, unix MSG_WAITALL at EOF, SD InvalidMemoryState in xz). Fixed in 0.14.2 (see .superpowers/sdd/m3-console1-report.md); console rerun of comm-acceptance, comm-suite-root and process/filesystem acceptance pending.**
  Verified 2026-11-24 (orchestrator): 8 `check-guest-*` exit 0;
  `guest_nro` 0 warnings; packaged native `/bin/comm-acceptance.elf` →
  `Communication acceptance results: suite 42, tools 42`;
  `/bin/process-acceptance.elf` → `suite 42, tools 42`; `--root
  /bin/comm-suite-root.elf` 122/0/3, exit 42.
  Artifacts: `Source/Switch2/build/guest/fex-switch2-guest.nro` and
  `Source/Switch2/build/guest/sd/fex` (merge onto SD `/fex`).
  Console 0.14.1 (2026-11-24): heap fix OK (`Heap: ... 9090 MiB -> 2049 MiB`,
  commit limit 6144 MiB); process `suite 42, tools 42`; filesystem `ABI 42,
  coreutils 0, suite 42`; Bash/nc PC test OK; FTP not yet checked. comm:
  suite 116/8/1, tools `xz_threads_roundtrip` SD 0xd401 EIO. 0.14.2 fixes all
  nine (`.superpowers/sdd/m3-console1-report.md`); SD data read/write now
  bounce through a 64 KiB heap buffer.
  **Console checklist 0.14.2** (copy NRO and merge `sd/fex`; only changed
  areas): 1. Banner `launcher 0.14.2`. 2. `/bin/comm-acceptance.elf` →
  `Communication acceptance results: suite 42, tools 42`. 3.
  `/bin/comm-suite-root.elf` → exit 42. 4. `/bin/filesystem-acceptance.elf`
  → `ABI 42, coreutils 0, suite 42` (SD path changed). 5. FTP on 1337:
  connect, list, upload.
  Bind probe result (`build/fex-switch2-bindprobe.nro`, 2026-11-24): Horizon
  allows 0.0.0.0 bind; the console-address redirect was removed. C5 after console pass:
  reconcile inventory/checkpoint, commit on `nx2`.
  Orchestrator ledger and per-WP reports and concerns:
  `.superpowers/sdd/progress.md`, `.superpowers/sdd/m3-wp1..5-report.md`.
  Verified 2026-11-24 (orchestrator rerun): `make -C Source/Switch2
  check-guest-inet check-guest-socket check-guest-sysv-ipc check-guest-mqueue check-guest-pipe
  check-guest-futex check-guest-arena check-guest-memory` exit 0. Rows
  and deliberate differences are in
  [CommunicationInventory.md](docs/CommunicationInventory.md).
  SDK risks and the EXPERIMENT
  probe (`make m3-probe`, `build/fex-switch2-m3probe.nro`, log
  `sdmc:/fex/m3probe.log`) are in [Milestone3Sdk.md](docs/Milestone3Sdk.md);
  its console results feed the plan. procfs (`/proc/<pid>/stat`,
  `/fd` for `ps`/`pgrep` and `event/anon_inode_names`), devfs, ioctls and
  the 137 entries outside the three groups are reconciled after milestone 3.
  Graphics stays on `shelf/graphics`. Open minors: rootfs setup should
  configure base-passwd first; Task 5/6/8 minors in
  `.superpowers/sdd/progress.md`.
- **History.** Milestone 2 console runs 0.13.1-0.13.17 follow.

- **Status (2026-11-22).** Milestone 1 (filesystem) is **complete** and
  committed on `nx2`. Console acceptance passed across 0.12.36–0.12.40:
  combined acceptance `ABI 42, coreutils 0, suite 42` and the root suite
  (0.12.37), the persistence and both recovery pairs (0.12.36), and the
  FTP test, 0 failed (0.12.40). Builds 0.12.38–0.12.40 changed only the
  FTP server. Graphics stays shelved on `shelf/graphics` (snapshot
  `c54852c9b`) until milestones 2 and 3 are done.
- **Console 0.13.14/0.13.15-diag (2026-11-23).** proc-suite-root and
  filesystem-acceptance passed. process-acceptance: suite 236/4/7 (the four
  `/proc/self/fd/N` anon-name readlinks: the guest has no `/proc/<pid>/fd`),
  tools 1 (`inotifywait_monitor`: fork EAGAIN, 9 live processes against the
  8-slot thread table); 0.13.14 then crashed the launcher and lost the
  buffered output. **0.13.15-diag** writes program output to the log at once
  (`[out]` lines). **0.13.16-diag:** thread table 32 (RLIMIT_NPROC 32); the
  anon-name checks moved to `event/anon_inode_names`, which skips without
  `/proc/<pid>/fd`; the suite runs in its own `./proc-suite.XXXXXX`
  directory (a root run left root-owned `ino-*` directories that broke the
  user run); `RunPackagedNative.sh` cleans up through the id-mapped
  namespace (it had left `rootfs/scratch` in the staged bundle). Native:
  241/0/7 and 245/0/3; packaged root suite then acceptance, twice:
  `suite 42, tools 42`. The 0.13.14 crash is unexplained; rerun to see
  whether it recurs.
- **Console 0.13.16-diag (2026-11-23).** proc-suite 240/0/8, proc-suite-root
  246/0/2, filesystem-acceptance `ABI 42, coreutils 0, suite 42`; tools 1:
  `inotifywait_monitor` (the trace shows inotifywait exec'd after the
  writer subshell started; on the console its startup outran `sleep 0.2`).
  Interactive Bash passed except Ctrl+C on a foreground `while :; do :;
  done`: preemption counted terminal input only when another thread waited
  on the terminal, so the shell's own loop was never preempted and the key
  never read. **0.13.17-diag:** terminal input always allows preemption in
  interactive runs; `inotifywait_create`/`_monitor` act only after
  inotifywait reports "Watches established." Native tools script exits 42.
- **Next (2026-11-23): console run of 0.13.17-diag** (same checklist). Milestone 2,
  processes and events ([Milestone2Plan.md](docs/Milestone2Plan.md), inventory in
  [ProcessInventory.md](docs/ProcessInventory.md), ledger in
  `.superpowers/sdd/progress.md`, git-ignored). Tasks 1-9 were
  console-verified on 0.13.13-diag (proc-suite 138/0/7, proc-suite-root
  143/0/2, fs-suite 81/0/2, fs-suite-root 83/0/0). Tasks 10-18 are now
  implemented in the working tree, **not yet console-verified**:
  timers/alarm/itimers/timerfd (10), eventfd (11), signalfd (12), poll,
  select, ppoll/pselect6 signal masks and epoll (13), inotify with
  filesystem hooks, fanotify ENOSYS (14), per-process rlimits (15),
  `ProcessTools.sh` + `process-acceptance.elf` and the Debian packages grep,
  procps, inotify-tools (16), inventory/README (17), banner 0.13.14-diag (18).
  Nothing of milestone 2 is committed; it commits once after console
  acceptance.
  **Verification (2026-11-23, 0.13.14):**
  - Host: `cmake --build Source/Switch2/build/guest --target guest_nro`
    builds; all 16 `check-guest-*` targets of Task 18 pass;
    `make check-rootfs-setup` 93 packages, 6326 paths match.
  - Native: `make check-proc-suite-native` 240 passed, 0 failed, 7 skipped
    (uid 1000) and 244/0/3 (namespace root); `make check-fs-suite-native`
    81/0/2. Packaged (`RunPackagedNative.sh build/guest/sd/fex/rootfs ...`):
    `/bin/process-acceptance.elf` prints `suite 42, tools 42`, exit 0;
    `--root /bin/proc-suite-root.elf` 244/0/3, exit 42;
    `/bin/filesystem-acceptance.elf` (with its .args) `ABI 42, coreutils 0,
    suite 42`. `RunPackagedNative.sh` now starts the program in its own
    session and not as the namespace init, as the launcher does (orphaned
    process groups need init outside the session).
  - Console: pending. Artifacts: `Source/Switch2/build/guest/fex-switch2-guest.nro`
    and `Source/Switch2/build/guest/sd/fex` (merge onto SD `/fex`). The
    staged `proc-suite*.elf.args` now name all ten groups (written by
    `StageDebian.py`).
  **Console checklist:** 1. `/bin/process-acceptance.elf` → `Process
  acceptance results: suite 42, tools 42`. 2. `/bin/proc-suite-root.elf` →
  exit 42. 3. `/bin/filesystem-acceptance.elf` → `ABI 42, coreutils 0,
  suite 42`. 4. Interactive Bash: Ctrl+C on `sleep 100` (`echo $?` 130),
  Ctrl+Z/`bg`/`jobs`/`fg`, Ctrl+C on `while :; do :; done`,
  `(while :; do :; done) &` then `echo hi`, `cat` + Ctrl+Z + `kill -9 %1`,
  `trap 'echo caught' INT`, `stty tostop; echo x &`. 5. On failure send
  `/fex/fex-switch2-guest.log`. Persistence/recovery pairs skipped (no
  inode-store or journal changes).
  Open: the **EXPERIMENT** SD diagnostics (`SdBufferError`, `SdPathError`,
  0.13.12-diag) stay until `SD result 2001-0106` (seen in 0.13.5 and
  0.13.11, both after `SaveInodeStore` with the large post-Debian inode
  store) recurs and is explained, or are reverted before the milestone
  commit. Rootfs setup follow-up: configure base-passwd first (debootstrap
  order); a fresh `dpkg --configure -a` needs a second run today.
  Task 5/6 minors and Task 8 minor (pidfd_getfd ptrace check) are in the
  ledger. `make debian-sysroot` now unpacks into a staging directory, so
  re-running it works.
- **Console 0.13.5 rootfs setup (2026-11-22, failed).** Packages 1-18
  unpacked (tzdata fell back to snapshot.debian.org after security.debian.org
  stalled). diffutils stopped at `/usr/share/locale/id/LC_MESSAGES/diffutils.mo`:
  `SD result 2001-0106 (0x0000d401)`, kernel InvalidMemoryState, then "Guest
  native move could not finish recovery" (the `.dpkg-new` rename path,
  `MoveNativeEntry`). The log does not say which SD call failed.
  **Experiment 0.13.6-diag:** `SdBufferError` logs the call, buffer address,
  size and `svcQueryMemory` type/attr/perm of the buffer's first and last
  page for every `fsFileRead`/`fsFileWrite`; `SdPathError` names the call and
  paths for the inode-file and rename calls in that path; the recovery line
  reports both error codes and the store's name and inode counts. Revert or
  keep once the cause is known. Console: run the setup again (it resumes from
  the cache) and send the log.
  Also new in this build: a **Root shell** launcher item (Bash with uid and
  gid 0, the same launch credentials a `.user` sidecar sets), because the
  console has no other way to run `dpkg --configure -a` as root.
  Launch PATH now follows Debian's login.defs: root gets
  `/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin` and
  `HOME=/root`, other users `/usr/local/bin:/usr/bin:/bin` (was `/bin:/usr/bin`
  for everyone, so dpkg as root could not find `ldconfig` and
  `start-stop-daemon` in `/sbin`).
  Console 0.13.6-diag Root shell (22:24): uid 0 launch worked, but dpkg
  still reported `ldconfig`/`start-stop-daemon` missing from PATH; the shell's
  own output is not in the log, so the copied NRO most likely predates the
  PATH change (built 22:20). **0.13.7-diag** carries both, and stops logging
  routine PathNotFound from the inode-slot probes.
  Console 0.13.7-diag (22:28): root PATH works; `dpkg --configure -a` ran
  the maintainer scripts but every `#!` file failed in the runtime's execve
  ("Expected a little-endian x86-64 ... ELF"), so only `#!/bin/sh` scripts
  worked, through the shell's ENOEXEC fallback. debconf's
  `#!/usr/bin/perl -w` frontend was then read by sh ("use: not found"),
  debconf and libc6 postinst failed and the rest stopped on dependencies.
  **0.13.8-diag:** execve implements binfmt_script (`GuestScript.h`,
  `make check-guest-script` passes; ProcSuite `proc/exec_interpreter_script`
  natively passes, full native suite 95 passed, 1 skipped). Console:
  proc-suite + proc-suite-root 0 failed, then Root shell `dpkg --configure -a`.
  Console 0.13.8-diag (22:38): perl and debconf scripts now run; all but
  base-files and bash configured. base-files' postinst `chown root:root`
  failed because base-passwd (which creates /etc/passwd) was configured after
  it; debootstrap installs base-passwd first. ldconfig printed "Cannot mmap
  file" for every library: MAP_SHARED of an SD file returned ENODEV.
  **0.13.9-diag:** read-only MAP_SHARED of SD files (coherent views,
  FsSuite `io/shared_readonly_file_mapping`, native 81 passed, 2 skipped).
  Follow-up: the setup should configure base-passwd first as debootstrap
  does.
  Console 0.13.9-diag (user report, 2026-11-22): fs-suite and
  fs-suite-root pass; Root shell `dpkg --configure -a` completes. The
  downloaded log on the host was still the 0.13.8-diag run, so this result is
  the user's observation only.
  **0.13.10:** experiment reverted (SdBufferError/SdPathError wrappers; the
  2001-0106 failure did not recur, so there is no evidence for it). The
  recovery-failure log line keeps both error codes. Open: the 2001-0106
  (InvalidMemoryState) failure from 0.13.5 is unexplained; if it recurs,
  rebuild the experiment.
- **Task 7 (2026-11-22, native done, console pending).** Restart review:
  `ProcessInventory.md` "Interruption and restart" table; 12
  `signal/restart_table_*` cases (epoll skips until Task 13). Fixes found:
  blocking pipe/socket writes now queue every byte (`StreamProgress`;
  a signal after queued bytes returns their count, `proc/blocking_pipe_write_is_complete`,
  `signal/interrupted_pipe_write_returns_count`); Unix listen queues
  backlog + 1 and caps at somaxconn 4096 (was clamp 1..16,
  `proc/unix_listen_queues_backlog_plus_one`). `make check-proc-suite-native`:
  110 passed, 0 failed, 1 skipped (uid 1000 and namespace root). NRO 0.13.10
  builds. Next: Task 8 glue (credentials).
- **Tasks 8 + 9 (2026-11-22, native done).** Credentials/capabilities glue
  (Sonnet, `.superpowers/sdd/task-8-report.md`) and clocks glue (Sonnet,
  `task-9-report.md`). Native `check-proc-suite-native`: uid 1000 140 passed,
  0 failed, 6 skipped; namespace root 144 passed, 0 failed, 2 skipped.
  `check-fs-suite-native` 81 passed, 2 skipped. Step-1 headers for Tasks
  10/13/14 (`GuestTimers.h`, `GuestEpoll.h`, `GuestInotify.h`) pass their
  host checks.
- **Console 0.13.11 (staged, pending).** Covers Tasks 7-9. The proc-suite
  `.args` sidecars now select `proc/ signal/ session/ cred/ clock/`
  (alarm busy loop still excluded until Task 10). Artifacts:
  `Source/Switch2/build/guest/fex-switch2-guest.nro`,
  `Source/Switch2/build/guest/sd/fex`.
- **Console 0.13.11 (2026-11-22, 23:21).** fs-suite-root exit 42,
  filesystem-acceptance exit 0, Root shell `id`/`dpkg --audit`/`date`/`sleep`
  and user `touch`/`rm` ran (screen output not in the log). fs-suite (uid
  1000) exited 1 after `SD result 2001-0106 (0x0000d401)` and "Guest native
  creation could not finish recovery" (second occurrence after 0.13.5's
  diffutils rename; both paths end in `SaveInodeStore`). proc-suite and
  proc-suite-root exited 1; the log has no TAP lines because Detailed logging
  did not keep program stdout. Every run also logs `SD result 2002-3005`
  (unexplained, runs still pass).
  **0.13.12-diag:** Detailed logs now keep program output; EXPERIMENT: SD
  buffer/path diagnostics are back (`SdBufferError`, `SdPathError`, error
  codes in the creation-recovery line) until 2001-0106 is explained.
- **Console 0.13.12-diag (23:28).** fs-suite 81 passed, 0 failed, 2 skipped
  (2001-0106 did not recur); proc-suite 137/1/7 (`clock/all_ids_gettime`:
  pid 1 is the runtime's virtual init, no process to read); proc-suite-root
  141/2/2 (same, plus `cred/keepcaps_preserves_permitted`: PR_GET_KEEPCAPS and
  PR_SET_KEEPCAPS rejected the garbage unused arguments glibc's variadic
  prctl passes; Linux cap_task_prctl ignores them). `2002-3005` is Horizon's
  out-of-range read past EOF, already mapped to a 0-byte read; it no longer
  logs. **0.13.13-diag:** both fixes; the clock case reads the parent's CPU
  clock. Native 140/0/6 and 144/0/2. SD diagnostics experiment still in.
- **Console 0.13.13-diag (2026-11-22, passed).** proc-suite 138/0/7,
  proc-suite-root 143/0/2, fs-suite 81/0/2, fs-suite-root 83/0/0;
  2001-0106 did not recur (user report and log).
- **Console 0.13.4 rootfs setup (2026-11-22, failed).** Packages 1-11
  unpacked (the `:` names work). base-files stopped at `/dev` (error 95):
  the setup chowned and chmodded the existing virtual `/dev`. **0.13.5:**
  like dpkg, an existing directory keeps its owner and mode; only new
  directories get the package's metadata. `make check-rootfs-setup` still
  matches 93 packages, 6326 paths. Console checklist: 0.13.4's with banner
  `0.13.5`; the setup resumes from the 11 cached packages.
- **Console 0.13.3 rootfs setup (2026-11-22, failed).** Packages 1-2
  unpacked; package 3 stopped at `/var/lib/dpkg/info/gcc-12-base:amd64.list`
  ("Could not write the dpkg database"). Cause: the runtime returned EINVAL
  for any name containing `:` or `\` (`ResolveSdPath`, `WalkSdPath`), though
  Linux allows every byte but `/` and NUL. **0.13.4:** names with characters
  FAT or Horizon reject (control characters, `" * : < > ? \ |`) now go to the
  inode store like over-long paths (`SdNameUnsafe` in `NeedsOverflow`).
  New fs-suite case `path/sd_reserved_characters` (files, directory and
  children, symlink, hard link, renames store<->native both ways, ENOENT for
  missing reserved names); native fs-suite 80 passed, 2 skipped. Console
  checklist: 0.13.3's list with banner `0.13.4`, plus `/bin/fs-suite.elf`
  and `/bin/fs-suite-root.elf` with 0 failed. The setup resumes from the
  two cached packages.
- **0.13.3 rebuild (Task 5/6 review fixes, rootfs setup).** The Opus review
  of Tasks 5/6 (`.superpowers/sdd/task-5-6-review.md`) found C2: a dropped
  orphaned-group stop signal dispatched a thread with an unfinished sleep or
  futex wait; threads now dispatch only with no wait pending. I1 (CLONE_PIDFD
  EMFILE use-after-free), I2 (wait4/waitid rusage now carries the child's CPU
  time), I3 (pidfds poll readable only once the process exited, POLLHUP once
  reaped) are fixed, as are the reparent exit signal and a stale foreground
  group. New cases `proc/pidfd_not_readable_while_running`,
  `proc/wait4_rusage_reports_child_time`; native 94 passed, 1 skipped, both
  users. Open review minors are in the ledger.
  New launcher item **Setup Debian rootfs** (`RootfsSetupHost.cpp`,
  `GuestRootfsSetup.h`, XZ Embedded v2024-12-30 in `ThirdParty/xz-embedded`):
  reads `/fex/setup/debian-rootfs.manifest` (from `make rootfs-manifest`,
  `tools/RootfsManifest.py`: bookworm minbase = required + essential +
  apt with dependencies, 93 packages, 32.0 MiB, usr-is-merged preferred over
  usrmerge), downloads each package over HTTP from deb.debian.org or
  security.debian.org (snapshot.debian.org fallback), checks size and SHA-256,
  caches it in `/fex/setup/cache`, and unpacks it through the guest
  filesystem as root (dpkg's .dpkg-new + rename, owners, modes, mtimes,
  symlinks, hard links), writing `/var/lib/dpkg/status` (install ok unpacked),
  `info/*.list` and control files, and `/etc/apt/sources.list`. Incomplete,
  tracked: maintainer scripts do not run (`dpkg --configure -a` from the guest
  later); device nodes are refused; the manifest is trusted from HTTPS on the
  host without checking the Release signature. Host: `make check-rootfs-setup`
  unpacked all 93 packages, 6326 paths match Python's ar/tar readers (type,
  mode, size, content, mtime, owner, link target, dpkg .list and status).
  Console checklist for 0.13.3:
  1. Copy `Source/Switch2/build/guest/sd/fex` over SD `/fex` (it now includes
     `/fex/setup/debian-rootfs.manifest`) and run
     `Source/Switch2/build/guest/fex-switch2-guest.nro` (banner `0.13.3`).
  2. `/bin/proc-suite.elf` and `/bin/proc-suite-root.elf`: 0 failed (95
     cases, terminal ioctl case passes or skips).
  3. `/bin/filesystem-acceptance.elf`: `ABI 42, coreutils 0, suite 42`.
  4. Home > Setup Debian rootfs, A: 93 lines `[n/93] <package> <version>`,
     then `Unpacked 93 packages...`. Run it a second time: it reuses the cache
     and finishes without downloading.
  5. Bash shell: `ls -l /usr/bin/apt /bin/dash /etc/apt/sources.list`,
     `head -3 /var/lib/dpkg/status`, `/usr/bin/sleep 1; echo $?` (0).
- **Console 0.13.2 (2026-11-22).** Filesystem acceptance passed.
  proc-suite 82 passed, 9 failed; root 83 passed, 8 failed; 421/418
  preemption switches, 0 unresolved. Causes, fixed in 0.13.3:
  - `waitid` copied 24 siginfo bytes, dropping `si_status` (waitid_wexited_siginfo,
    clone_pidfd_poll_exit). It now writes the 28 bytes Linux writes.
  - `getsid`/`getpgid` returned ESRCH as an unsigned value (getsid_esrch).
  - `execveat(AT_EMPTY_PATH)` used the SD path of the descriptor.
  - getrlimit reported RLIMIT_NICE unlimited while setpriority enforced 0
    (priority_rules as uid 1000); it now reports 0.
  - A SIGCHLD handler interrupted `waitpid` although the child had already
    exited (sa_nocldstop_suppresses). A woken IO, terminal or lock wait now
    reruns its syscall before a pending signal is delivered, as Linux rescans
    before checking signals.
  - The bundle had no `/bin/sh` or `/bin/sleep` (exec_*, setpgid_after_exec).
    StageDebian now installs `sleep`, `/usr/bin/timeout`, and Bash as
    `/bin/sh` (the sysroot has no dash). `inotifywait` and procps `kill` are
    still missing from the sysroot (Task 17 staging).
  Console checklist: same as 0.13.2 below with banner `0.13.3`; expect 0
  failed in both suites.
- **0.13.2 (Tasks 5 and 6, Task 4 review fixes).** `GuestJobControl.h`
  (wait statuses, wait option checks, wait4 selector, setpgid/setsid rules,
  ISIG characters) with `check-guest-job-control`. Runtime:
  - Lifecycle: `ExitProcess` (Linux do_exit/exit_notify) ends every thread,
    reparents children to the nearest `PR_SET_CHILD_SUBREAPER` ancestor or a
    virtual init (pid 1) that reaps at once, sends `PR_SET_PDEATHSIG`, hangs
    up newly orphaned groups with stopped members (SIGHUP then SIGCONT), hangs
    up the foreground group when a terminal-owning session leader exits, and
    notifies the parent (`CLD_EXITED`/`CLD_KILLED`, exit signal; autoreap for
    SIG_IGN or SA_NOCLDWAIT). The last thread's `exit` ends the process.
  - Generic wait for `wait4` and `waitid` (P_ALL/PID/PGID/PIDFD, WEXITED,
    WSTOPPED/WUNTRACED, WCONTINUED, WNOWAIT, WNOHANG, __WCLONE/__WALL),
    ru_utime from per-process and per-thread CPU ticks, `getrusage`, `times`.
  - Job control: default stop actions stop the process (`CLD_STOPPED` unless
    SA_NOCLDSTOP), discarded for an orphaned group except SIGSTOP; SIGCONT
    generation resumes it (`CLD_CONTINUED`); a stopped process only reacts to
    SIGKILL. Sessions: `setsid`, `getsid`, Linux `setpgid` rules, the SIGCONT
    same-session permission. The console is the controlling terminal of the
    root session: TIOCGPGRP/TIOCSPGRP/TIOCGSID/TIOCSCTTY/TIOCNOTTY, background
    reads get SIGTTIN, background TCSETS*/TCFLSH/TIOCSPGRP and TOSTOP writes get
    SIGTTOU (EIO for an orphaned group or an ignored SIGTTIN), and ^C ^\ ^Z
    signal the foreground group with echo and flush.
  - Fork family: `clone` flag checks (EINVAL for sharing that separate
    processes cannot do, EPERM/EINVAL for namespaces), CLONE_PARENT, VFORK
    (parent waits until exec or exit), PIDFD, SETTLS, PARENT_SETTID, new
    stack; `vfork`, `clone3`, `pidfd_open`, `pidfd_send_signal` (THREAD,
    THREAD_GROUP, PROCESS_GROUP flags), `pidfd_getfd`, `execveat`.
  - `prctl` PDEATHSIG, DUMPABLE, NAME, CHILD_SUBREAPER (others EINVAL),
    `getpriority`/`setpriority` (Linux permission rules, nice stored only),
    `personality`, `getcpu`, `sysinfo` thread count; exec sets the name.
  - Task 4 review fixes: unblocked ignored signals are dropped on return to
    user (I1); `kill`/`rt_sigqueueinfo` with a non-leader thread id signal
    its process (I3); sigtimedwait's set no longer counts as blocked (M3);
    SA_RESETHAND resets only the handler (M4); tkill truncates to pid_t (M5);
    stored sa_mask drops SIGKILL/SIGSTOP (M7); fork keeps sigaltstack (M8);
    the restart and pause cases wait for the main thread to block (I4).
    The inventory is updated (I2); M6 and M9 are tracked there.
  Host: `make -C Source/Switch2 check-guest-job-control check-guest-signals
  check-guest-preempt check-guest-credentials check-guest-clocks
  check-guest-eventfd` all ok; `check-proc-suite-native` 92 passed, 1 skipped
  (no tty) as uid 1000 and namespace root. NRO 0.13.2 built, bundle staged;
  both proc-suite `.args` select `proc/`, `signal/`, `session/` and exclude
  `signal/alarm_interrupts_busy_loop` (Task 10). Console checklist:
  1. Copy `Source/Switch2/build/guest/sd/fex` over SD `/fex` and run
     `Source/Switch2/build/guest/fex-switch2-guest.nro` (banner `0.13.2`).
  2. Run `/bin/proc-suite.elf`. Expect `92 passed, 0 failed` with
     `session/terminal_job_control_ioctls` passing or skipped (93 cases).
  3. Run `/bin/proc-suite-root.elf`. Same expectation as root.
  4. Run `/bin/filesystem-acceptance.elf`. Expect `ABI 42, coreutils 0,
     suite 42`.
  5. In the interactive Bash (needs a keyboard that sends ^Z and ^C): `sleep 30 &`, `jobs` (Running), `kill %1`,
     `jobs` (Terminated); `sleep 30`, then ^Z (`Stopped`), `bg`, `fg`, ^C
     (prompt returns); `trap 'echo hup' HUP; kill -HUP $$` prints `hup`.
  6. The log ends with `Guest preemption: N switches, 0 unresolved`.
- **Console 0.13.0-exp1 (2026-11-22, failed).** Both `/bin/proc-suite.elf`
  and `/bin/filesystem-acceptance.elf` crashed the app. The log stops right
  after the forked child's first compiled block (pid 101), before its first
  syscall, with no `Host fault` line. That is the first point where a second
  thread exists, so the first preemption. exp1 write-protected the interrupt
  page from a watchdog and handled the fault in a `--wrap`ped
  `__libnx_exception_entry` (C code and SVCs in exception state, then
  `svcReturnFromException` into the dispatcher stop handler). The failure
  bypassed libnx's logging handler, so that path was removed rather than
  debugged blind.
- **Console 0.13.1 (2026-11-22).** Filesystem acceptance passed. proc-suite
  and proc-suite-root (`proc/`, `signal/` minus `kill_zombie_succeeds`): 38
  passed, 1 failed each. The failure is `signal/alarm_interrupts_busy_loop`
  (timeout): `alarm` is Task 10. All Task 3 and Task 4 cases passed,
  including `busy_child_is_killable` and both preempted-loop cases.
  `Guest preemption: 796 switches, 0 unresolved` (root run 799).
- **0.13.1 (Task 4 signal model).** `GuestSignals.h`
  (default actions, 128-byte siginfo, `SignalQueue`: standard signals
  collapse, realtime signals queue in order) with `check-guest-signals`.
  Runtime: per-thread `Pending` and per-process `SharedPending` queues;
  `SendSignal` (Linux prepare_signal: ignored-and-unblocked signals dropped,
  SIGCONT/stop discard each other), `CanSignal` (kill_ok_by_cred; CAP_KILL =
  euid 0 until Task 8, SIGCONT session rule waits for Task 6), `kill` for
  every pid form, `tkill`, `tgkill`, `rt_sigqueueinfo`,
  `rt_tgsigqueueinfo`, `rt_sigpending` (sizes up to 8), `pause`,
  `rt_sigsuspend`, `rt_sigtimedwait`, `restart_syscall` (EINTR: no restart
  blocks yet). `DeliverSignals` stacks one frame per caught signal like the
  kernel's exit-to-user loop, passes the full siginfo, runs terminate/core
  defaults on every thread of the process (wait status never has the core
  flag: no dump is written), and ends waits with Linux restart rules
  (`SA_RESTART` reruns read/write/wait4/lock/accept/connect and untimed futex
  waits; sleeps, polls and signal waits give EINTR). getrlimit reports
  RLIMIT_SIGPENDING 4096, the queue limit. Incomplete, tracked: default stop
  actions leave the signal pending (Task 6). Native probes overrode the plan:
  SIGPIPE carries `si_pid` = writer, `si_uid` = its real uid; Linux rejects
  any `sigaction` act for SIGKILL/SIGSTOP, SIG_DFL included; realtime
  handlers that block only their own signal run +1(7), +1(8), RTMIN(5).
  Host: `make -C Source/Switch2 check-guest-signals` ok;
  `check-proc-suite-native` 40 passed as uid 1000 and namespace root. NRO
  0.13.1 built; bundle staged; both proc-suite `.args` select `proc/`,
  `signal/` and exclude `signal/kill_zombie_succeeds` (needs waitid, Task 5).
- **Console 0.13.0-exp4 (2026-11-22).** Filesystem acceptance passed.
  proc-suite: `busy_threads_share_cpu`, `preempted_register_loop_is_exact`
  and `getpid_matches_gettid_in_main_thread` ok; `preempted_call_loop_is_exact`
  hit the 10 s harness timeout, but its child later ran `exit_group(0)`, so
  its result check passed and only the 30M-call loop was too slow. The count
  is now 3M. `Guest preemption: 1892 switches, 0 unresolved`. The harness's
  SIGKILL got EOPNOTSUPP (fixed by Task 4).
- **0.13.0-exp4 (Task 3 review fixes).** The Task 3 review
  found two resume-RIP bugs that exp3's tests could not see:
  - An entry-point poll resumed at the multiblock head (`GetGuestBlockEntry`),
    so a poll at a call-return entry re-ran the code before the call.
  - Back-edge targets map through RIP entries, which existed only at
    instructions with side effects, so a register-only loop resumed at an
    earlier instruction.
  Fixes: every poll stores the host address of its resume point (an
  `EntryLabel` bound at each entry point, or the branch target), resolved only
  through `RestoreRIPFromHostPC`. `GenerateIR` records a RIP entry for every
  instruction when `PollPendingInterrupt` is on. `OpenPreemptWindow` clears
  the target word, so a stop that is not a poll is never resumed. The hints
  use `Ready()` and are refreshed after every syscall. Only the instance that
  started the watchdog stops it. New suite cases
  `proc/preempted_register_loop_is_exact` and
  `proc/preempted_call_loop_is_exact` pass natively (6/6 with the others).
  `check-guest-preempt` and `check-guest-signals` pass. NRO and bundle are
  built, and the staged `proc-suite.elf.args` selects the two new cases,
  `busy_threads_share_cpu` and `getpid_matches_gettid_in_main_thread`.
- **Console 0.13.0-exp3 (2026-11-22, passed).** `/bin/proc-suite.elf`
  (`.args`: `proc/busy_threads_share_cpu`,
  `proc/getpid_matches_gettid_in_main_thread`) exited 42 and the log reported
  `Guest preemption: 22 switches, 0 unresolved`. Filesystem acceptance passed
  on exp2 with the same preemption runtime. The Task 3 gate is passed.
- **Console 0.13.0-exp2 (2026-11-22).** `/bin/filesystem-acceptance.elf`
  passed (`ABI 42, coreutils 0, suite 42`) with preemption active.
  `/bin/proc-suite.elf` hung: the log shows one preemption (`Preempted
  tid=101 at rip=0x5740524353`, a block entry), the child resumed and
  completed six syscalls, then nothing. Cause: `CondJump`
  (`FEXCore/Source/Interface/Core/JIT/BranchOps.cpp`) emitted conditional
  backward branches directly, without a poll, so a loop closed by a
  conditional jump (`while (!Stop) n++;`) never saw the request.
- **0.13.0-exp3.** With polls enabled, a conditional backward branch is
  emitted as "inverted branch over; poll; `b` target", so every back-edge
  polls. NRO built; the bundle is unchanged since exp1. Run the exp2
  checklist below with banner `0.13.0-exp3`.
- **Experiment 0.13.0-exp2 (Task 3, preemption gate).** No exceptions:
  with `Context::SetPollPendingInterrupt(true)` the JIT loads the word at
  `InterruptPollRequestOffset` (64) of `InterruptFaultPage` at every block
  entry and before every backward branch. When it is nonzero it stores the
  branch target's host address (0 at a block entry) at
  `InterruptPollTargetOffset` (72), restores `sp` from
  `ReturningStackLocation` and jumps to `Pointers.ThreadStopHandlerSpillSRA`,
  the same exit FEX's HLT path uses, so `ExecuteThread` returns.
  `Dispatch` rebuilds the guest RIP (`RestoreRIPFromHostPC` or
  `GetGuestBlockEntry`). The watchdog thread (`PreemptWatchdog`) only writes
  that word, after a 10 ms quantum when another thread could run.
  `sched_yield` yields. Host: `make -C Source/Switch2 check-guest-preempt`
  passed; `check-proc-suite-native` 4 passed as uid 1000 and namespace root;
  NRO built. The FEX Linux build was not checked (this checkout lacks the
  `External/jemalloc_glibc` and `External/rpmalloc` submodules). Console
  checklist:
  1. Copy `Source/Switch2/build/guest/sd/fex` over SD `/fex` and run
     `Source/Switch2/build/guest/fex-switch2-guest.nro` (banner
     `0.13.0-exp2`).
  2. Run `/bin/proc-suite.elf` (its staged `.args` selects
     `proc/busy_threads_share_cpu` and `proc/getpid_matches_gettid_in_main_thread`).
     Expect `ok` for both and `2 passed, 0 failed`. Without preemption
     `busy_threads_share_cpu` hangs.
  3. Run `/bin/filesystem-acceptance.elf`. Expect `ABI 42, coreutils 0,
     suite 42`.
  4. The log must contain `Guest preemption: N switches, 0 unresolved`
     with N > 0 after step 2, and `Preempted tid=... at rip=...` lines.
- **Console 0.12.40 (2026-11-22).** The user reported that all FTP tests
  pass on the console.
- **Console 0.12.36 (log of 2026-11-22).** Acceptance printed `ABI 42,
  coreutils 0, suite 1`; the root suite had 80 passed, 1 failed. The only
  failure in both: `path/long_paths` line 1927, a 4096-byte path returned
  EFAULT instead of ENAMETOOLONG. `path/name_too_long`,
  `path/long_path_renames`, and the persistence and both recovery pairs
  (42 each) passed, so the overflow store works on the SD card. The FTP
  server failed twice with `Could not listen on port 5000: Address
  already in use`: an always-on FTP sysmodule holds 5000. The user also
  pointed out that the rootfs has no accounts, so `root` logins made no
  sense. `SD result 2002-3005` in `io/positional_io_keeps_offset` is a
  pread past EOF that the runtime turns into a 0-byte read; the test
  passes.
- **Fixed for 0.12.37.**
  - `GuestString` follows `getname`/`strncpy_from_user`: EFAULT only when
    the bytes are unreadable, otherwise the caller's error when no NUL
    appears within the limit: ENAMETOOLONG within PATH_MAX for paths,
    E2BIG within MAX_ARG_STRLEN (128 KiB) for exec arguments (these failed
    with EFAULT above 4095 bytes), EINVAL within 250 bytes for memfd
    names. Suite: a 4095-byte path is ENOENT, not ENAMETOOLONG.
  - FTP: port 21, falling back to any free port if taken. Any user name;
    the on-screen password is the only credential. The service acts with
    root's access and chowns what it creates to uid 1000 (the default
    owner of SD entries) through the real chown handler.
- **Verification (host, 2026-11-22, 0.12.37).** Host checks including
  `check-guest-ftp` passed; the FTP test also passed with an ownership
  expectation. `check-fs-suite-native`: 79 passed, 0 failed, 2 skipped as
  uid 1000 and root. Packaged bundle: `ABI 42, coreutils 0, suite 42`,
  exit 0; root suite 42. NRO 0.12.37 built.
- **Console 0.12.37 (2026-11-22).** Acceptance and the root suite passed
  (the user reported the tests succeed). The FTP screen showed `Could not
  open a listening socket: Permission denied`: Horizon refuses binding the
  privileged port 21, and 0.12.37 only fell back on EADDRINUSE.
- **Fixed for 0.12.38.** The FTP server tries 1337 (OunceShell's port
  under kaiHEN), 2121, then a system-chosen port, logging each attempt as
  `FTP server port N: ...` with the failing call. Passive data sockets
  fall back from a system-chosen port to 50000–50999. Host FTP test
  passes; NRO 0.12.38 built.
- **0.12.39.** Following OunceShell's ftpd (github.com/kurumuz/ounceshell,
  `switch2/third_party/ftpd/ftp.c`): the listener binds the console's
  address from `nifmGetCurrentIpAddress` instead of `INADDR_ANY` (the
  likely EACCES cause), the socket driver uses ftpd's buffer config, and
  the screen refuses to start without a network address. Attempts log as
  `FTP server <ip>:<port>: ...`. Host FTP test passes; NRO 0.12.39 built.
  0.12.38 was not run on the console.
- **Console 0.12.39 (2026-11-22).** The FTP server listened on
  192.168.178.29:1337 (the nifm address bind fixed EACCES). `FtpTest.py`
  passed the wrong-password check, then its reconnect got `421 Another
  client is connected`: the server accepted new connections before
  noticing the closed one, and served only one client.
- **Fixed for 0.12.40.** Up to 8 sessions with per-session state; hang-ups
  are processed before new connections are accepted. `FtpTest.py` also
  opens two sessions at once. Host FTP test passes; NRO 0.12.40 built.
- **Console 0.12.35 (log of 2026-11-22).** Name-limit probe, longest name
  per depth (SD directory path bytes in parentheses): 235 (22), 194 (63),
  153 (104), 112 (145), 71 (186), 30 (227). Name plus path is always 257
  bytes, two-byte characters count as two bytes, and creating the depth-6
  directory failed. The error is `SD result 2002-6003`.
- **Implemented for 0.12.36.**
  - Horizon 2002-6003 maps to ENAMETOOLONG; guest path limits follow
    PATH_MAX 4096 instead of Horizon's 769-byte `FS_MAX_PATH`.
  - Overflow store (`GuestRuntime.cpp`, `GuestInodeStore.h` flag 512).
    Directories whose SD path would exceed 257 bytes, or whose parent is
    an overflow directory, are `OverflowDirectory` records; files there
    are managed objects created like `O_TMPFILE` (unlinked record, object,
    then the name). All lookups go through `EntryType`, so a missing
    overflow entry is ENOENT. Listing, `st_nlink`, rmdir (ENOTEMPTY from
    store children) and the launcher's rootfs browser include them.
  - Renames involving an overflow directory, an overflow destination, or
    a native directory whose deepest native path would exceed the limit
    at its new name go through `RenameThroughStore`: both sides are made
    store-backed (files promoted, directories converted deepest first by
    `ConvertToOverflow`), then every name below the source is moved in
    one store save. Error order follows `do_renameat2`. A reset during
    conversion leaves an empty native directory behind its overflow
    record; the next load deletes it.
  - FTP server (`GuestFtp.h`, launcher **FTP server**): port 5000, passive
    mode, password shown on screen, `root` = uid 0, other names =
    uid 1000. `GuestFileService` calls the guest syscall handlers (open,
    read, write, getdents64, newfstatat, mkdirat, unlinkat, renameat,
    utimensat, faccessat2) on its own namespace. Commands and replies are
    logged as `FTP >` / `FTP <` lines (passwords masked).
  - Suite: `path/long_paths`, `path/long_path_renames` (81 cases). The
    persistence pair now also checks an overflow directory chain, a
    255-byte name and a converted native tree.
- **Verification (host, 2026-11-22, 0.12.36).**
  `make -C Source/Switch2 check-guest-inode-store check-guest-file-punch check-guest-file-write check-guest-memory check-guest-log check-guest-terminal check-guest-ftp`
  passed (FTP protocol test: 25 checks, 0 failed).
  `make -C Source/Switch2 check-fs-suite-native`: 79 passed, 0 failed,
  2 skipped as uid 1000 and as namespace root.
  `make -C Source/Switch2 filesystem-runtime` staged `build/guest/sd`.
  `tools/RunPackagedNative.sh build/guest/sd/fex/rootfs /bin/bash --noprofile --norc /bin/filesystem-coreutils.sh /bin/filesystem-probe.elf /bin/fs-suite.elf`
  printed `ABI 42, coreutils 0, suite 42` and exited 0; with
  `--root /bin/fs-suite-root.elf` it exited 42. Persist write and read
  run natively in the bundle returned 42 each.
  `cmake --build Source/Switch2/build/guest --target guest_nro` built
  the 0.12.36 NRO. The overflow store and the console FTP backend have
  only been built for the console, not run there.
- **Console 0.12.32.** Acceptance hung because `nanosleep` and
  `clock_nanosleep` were unrouted; fixed in 0.12.33 and confirmed.
- **Implemented for 0.12.33.**
  - `nanosleep` (35) and `clock_nanosleep` (230): REALTIME, MONOTONIC,
    BOOTTIME and TAI, relative and TIMER_ABSTIME. A caught signal ends the
    sleep with EINTR (even with SA_RESTART) and relative sleeps store the
    time left. Error order matches host Linux 7.3: EOPNOTSUPP for thread
    CPU, raw and coarse clocks; EINVAL for unknown clocks; alarm clocks
    validate, then EPERM for non-root. Gaps: process CPU clock and root
    alarm sleeps return EOPNOTSUPP.
  - The log keeps guest stdout/stderr in a separate 512 KiB buffer and
    repeats it after a trimmed trace.
  - Inode metadata moved from flat `/fex/.inode-*` entries into
    `/fex/.inode/` (`store-0`, `store-1`, `objects`, `dir-intent`,
    `dir-backup`, `intent-stage`, `promotion`). The launcher renames
    legacy entries on startup and refuses guest launches if an entry
    exists at both paths or a rename fails.
  - One NRO for everything: the recovery interruption is enabled per
    launch by a `<program>.recovery-test` file (staged for the two recovery
    write probes) instead of the `FEX_SWITCH2_RECOVERY_TEST` build option.
  - Suite: `time/nanosleep_rules`, `time/sleep_interrupted_by_signal`.
- **Verification (host, 2026-11-22, 0.12.34).** Same commands as below:
  host checks passed, native suite 77/0/2 as uid 1000 and namespace
  root, packaged bundle `ABI 42, coreutils 0, suite 42`, packaged root
  suite exit 42, NRO built.
- **Earlier in milestone 1.** memfd page tracking (hole punching frees
  pages, `st_blocks`, `SEEK_DATA`/`SEEK_HOLE`, KEEP_SIZE, truncation),
  `MFD_NOEXEC_SEAL`/`MFD_EXEC`/`F_SEAL_EXEC`, access mode 3, legacy `open`
  ignoring unknown flags, regular files polling readable and writable,
  `ustat`, `name_to_handle_at`, `open_by_handle_at` (all 102 inventory
  entries routed; see
  [deliberate differences](docs/FilesystemInventory.md#deliberate-differences-from-a-stock-kernel)),
  and `<program>.user` launch credentials.
- **Test suite.** `tests/guest/FsSuite.c` has 79 named cases, each in
  its own scratch directory, with TAP output and exit 42.
  `tools/RunPackagedNative.sh` runs a staged bundle on the host
  kernel in user, mount and PID namespaces.
- **Build layout.** One tree: `build/guest` (configure per README). The
  bundle stages to `build/guest/sd` (`make -C Source/Switch2
  filesystem-runtime`), mirroring the SD root.
- **Verification (host, 2026-11-22, 0.12.33).**
  `make -C Source/Switch2 check-guest-inode-store check-guest-file-punch check-guest-file-write check-guest-memory check-guest-log check-guest-terminal`
  passed. `make -C Source/Switch2 check-fs-suite-native`: 77 passed,
  0 failed, 2 skipped as uid 1000 and as namespace root (the skips,
  `path/handles` and `xattr/trusted_namespace`, need init-namespace
  capabilities). `tools/RunPackagedNative.sh build/guest/sd/fex/rootfs /bin/bash --noprofile --norc /bin/filesystem-coreutils.sh /bin/filesystem-probe.elf /bin/fs-suite.elf`
  printed `ABI 42, coreutils 0, suite 42`; with `--root /bin/fs-suite-root.elf`
  it exited 42. `cmake --build Source/Switch2/build/guest --target guest_nro`
  built the 0.12.33 NRO.
- **Debian sysroot.** `make -C Source/Switch2 debian-sysroot` downloads
  and checks Debian 12 packages into `build/debian-sysroot` (default
  `DEBIAN_ROOT`).

### Last console checklist (0.12.40, passed)

Artifact: `Source/Switch2/build/guest/fex-switch2-guest.nro` (the bundle
is unchanged from 0.12.37). Acceptance and the root suite passed on
0.12.37 and nothing they cover changed, so only FTP is checked.

1. Open **FTP server** with the console on the PC's network. Expect
   `ftp://<ip>:1337` (or 2121 / another port), `User any name` and a
   six-digit password.
2. On the PC run `python3 Source/Switch2/tests/unit/FtpTest.py <ip>
   <port> <password> 1000`. Expect `ftp test: 0 failed`.
3. Press B to stop the server.

On failure, copy `/fex/fex-switch2-guest.log`; the `FTP server <ip>:<port>`
lines name the socket call that failed, and sessions appear as `FTP >`
and `FTP <` lines.

The sections below preserve the detailed acceptance scope and historical
evidence. Use this checkpoint for current status and the next action.

Interactive Bash 0.11.5 passed on Switch 2. The user confirmed the
result screen, repeat launches in the same NRO, and consecutive external
commands in one shell. The baseline is committed on `nx2` as `f4f9c0d13`.

## Next milestone — finish filesystem compatibility (groups 1–5)

SD truncate growth now preserves an unallocated tail through the
managed inode's logical EOF. Both pathname and descriptor truncation
share the backing path. The probe verifies unchanged block counts,
zero reads and hole seeking. Native acceptance returned ABI 42 and
Debian coreutils 0, and both persistence phases returned 42.
Extending SD writes clear the gap once and attempt to restore the old
physical length if the gap or payload write fails. The native probe
also checks a write into a sparse tail and the zero bytes on either
side of it. The development NRO built and packaged native acceptance
returned ABI 42 and Debian coreutils 0. SD write failures and rollback
have yet to be exercised on console.
The restart probe now also checks a truncate-grown file after hard
linking and renaming it, including inode identity, zero allocation,
hole seeking and EOF. The refreshed development bundle is under
`Source/Switch2/build/filesystem-runtime-m1/fex`. The exact packaged
ABI ELF ran with the bundled Debian loader and libc and returned 42
in combined acceptance and both persistence phases. Debian coreutils
returned 0 in the combined run. No console result has been recorded
for the refreshed bundle.

The last console-confirmed build is
0.12.29. The 0.12.31 recovery checklist is recorded below.

| Group | Status (2026-11-22) |
| --- | --- |
| 1. Descriptors and I/O | Implemented. Console: `fcntl` deadlock, `flock` interruption and restart. |
| 2. Paths and links | Implemented, including file handles. Console: interrupted SD rename recovery at both checkpoints. |
| 3. Metadata and permissions | Implemented. Console: cross-identity cases through `fs-suite-root.elf`. |
| 4. Allocation and transfer | Implemented, including memfd hole punching. Console: `splice`, `tee`, `vmsplice`. |
| 5. Extended attributes | Implemented. Console: `trusted.*` visibility through `fs-suite-root.elf`. |

Reuse the combined ABI and Debian coreutils acceptance program. Add cases
for the remaining behavior and use restart phases for persistence and
recovery. Reconcile the syscall and flag inventory before declaring the
milestone complete. Commit on `nx2` after hardware acceptance.
The [current syscall inventory](docs/FilesystemInventory.md) records ABI
numbers and dispatcher routing. It includes three unrouted legacy
and handle-based candidates whose milestone assignment still needs
reconciliation with the historical baseline.

## Active milestone sequence — Linux guest kernel compatibility

Graphics is shelved until these milestones are complete. Work on them in
order on `nx2`.

| Milestone | Groups | Audited entries | Initially routed | Initially missing |
| --- | --- | ---: | ---: | ---: |
| 1 | File descriptors and I/O, paths and links, metadata and permissions, file allocation and transfer, extended attributes | 98 | 28 | 70 |
| 2 | Processes and sessions, signals, credentials and capabilities, clocks and timers, polling and watches | 85 | 24 | 61 |
| 3 | Virtual memory, pipes and IPC, sockets | 60 | 22 | 38 |

Count each existing handler as needing a semantics review. Track syscall
variants, flags, Linux errors, shared state and resource lifetime, and limits
imposed by the host. Test successes and errors with the same x86-64 probe on
native Debian and in the normal NRO launcher. Keep probe cases small and give
failures names that identify the failing operation and errno. Record host
build and reference results separately from console results. At each completed
milestone, hand off the NRO and probe for joint console debugging. Commit the
milestone after its acceptance checks pass.

1. **Filesystem.** Give guest files stable identities independent of paths.
   Support descriptor-relative access, rename and unlink with live handles,
   access checks, links, timestamps, persistent metadata and xattrs, file
   allocation and transfer. Verify persistence across launcher restarts,
   error handling, and ordinary Debian `ls`, `cp -a`, `mv`, `rm`, `ln`,
   `mkdir`, `stat`, `touch`, `chmod` and `df` against scratch data.
2. **Processes and events.** Verify process and session lifecycle, signal
   delivery and return, credential changes and authorization, clocks, timers,
   and readiness and watch lifetimes. Exercise stock Bash pipelines, terminal
   job control, traps, and Debian `sleep`, `timeout`, `kill` and
   `inotifywait`.
3. **Memory and communication.** Verify mapping protections and sharing,
   futex behavior and robust lists, pipes and queues, socket options and
   addresses, descriptor passing and peer teardown. Exercise Debian threaded
   applications and real Unix, TCP and UDP client/server pairs.

The initial inventory covers 243 of the 380 x86-64 syscall entries in this
checkout. Track the other 137 individually (129 initially missing), including
scheduling, mount and namespace APIs, async I/O and privileged or legacy
interfaces. The three groups of milestones alone cannot establish full kernel
coverage. Reconcile remaining entries and kernel-visible interfaces such as
`procfs`, `devfs` and ioctl operations before reporting 100%. Validate
privilege failures under the intended guest credentials.

### Recovery diagnostic 0.12.31

The last console-confirmed build is 0.12.29. Combined acceptance returned
0 (ABI 42, Debian coreutils 0) and both persistence phases returned 42
across a clean launcher restart. Since that run, work added pipe page
slots and pinned pages, SIGPIPE and lock interruption/restart, filesystem
credentials and ownership checks, sparse-tail persistence, and SD journal
durability and recovery.

The 0.12.31 diagnostic NRO is build-verified at
`Source/Switch2/build/guest-filesystem-recovery/fex-switch2-guest.nro`
and contains the 0.12.31 launcher banner. The staged bundle is at
`Source/Switch2/build/filesystem-runtime-m1/fex`. The 0.12.31 console
run was requested to verify recovery ordering on the SD backend.
The 90-second response window elapsed without a console result.
Merge the bundle into SD `/fex`,
launch the diagnostic NRO, and use **Browse Linux rootfs**:

1. Run `/bin/filesystem-acceptance.elf`. Expect ABI 42, coreutils 0,
   and overall exit 0.
2. Run `/bin/filesystem-recover-rollback-write.elf`, fully reopen the NRO,
   then run `/bin/filesystem-recover-rollback-read.elf`. Expect 42 each.
3. Run `/bin/filesystem-recover-commit-write.elf`, fully reopen the NRO,
   then run `/bin/filesystem-recover-commit-read.elf`. Expect 42 each.
4. Run `/bin/filesystem-persist-write.elf`, fully reopen the NRO,
   then run `/bin/filesystem-persist-read.elf`. Expect 42 each.

The recovery build uses `FEX_SWITCH2_RECOVERY_TEST=ON` and deliberately
interrupts journal transactions at two checkpoints. Keep trace logs from
`/fex/fex-switch2-guest.log` if a phase fails. Give the user the complete
console checklist and the requested 90-second response window. Keep builds
under `Source/Switch2/build/` and use the combined acceptance program for
the final run. Work on `nx2` and commit at the completed milestone boundary.

Development builds continue separately while console recovery results
are pending. Range sync now accepts procfs files and directories and
decodes flags as a 32-bit Linux argument. The updated Debian ELF and
development NRO built, and native combined acceptance returned ABI 42
and Debian coreutils 0. The newer development bundle is under
`Source/Switch2/build/filesystem-runtime-dev/fex`.

O_TMPFILE now validates the resolved directory before checking SD
metadata availability, reports ENOTDIR for a retained final symlink
or virtual file, and follows a directory symlink with a trailing slash.
Temporary files on the synthetic devfs are unsupported. The
development NRO built and native packaged acceptance returned ABI 42
and Debian coreutils 0.

Openat2 cached lookup now validates flags, the pathname buffer and
the starting descriptor before returning EAGAIN for an uncached
lookup. Creation and truncation requests still return EAGAIN before
pathname lookup. Packaged native acceptance returned ABI 42 and
Debian coreutils 0, and the development NRO built.

Timestamp updates now authorize explicit timestamps by inode ownership
and current-time requests by ownership or write permission. The ABI
probe covers current-time updates on a read-only owned file, two
UTIME_OMIT values on a missing name, and denial on a foreign inode on
the native reference. Packaged native acceptance returned ABI 42 and
Debian coreutils 0, and the development NRO built.

Timestamp syscalls now copy the caller's time values before checking
pathname flags and treat two UTIME_OMIT values as a no-op even with an
invalid descriptor or flags. The ABI probe checks that precedence,
fault reporting and the 32-bit flags argument. Packaged native
acceptance returned ABI 42 and Debian coreutils 0. The development
NRO built at the existing version.

Pathname truncate now classifies the resolved virtual target, including
aliases and trailing slash errors. The ABI probe also verifies sparse
growth through a relative symlink while an existing descriptor retains
the same inode. The updated development NRO built and packaged native
acceptance returned ABI 42 and Debian coreutils 0.

The packaged ABI probe now executes a copy of true after growing its
logical EOF without increasing its allocated blocks. Native combined
acceptance returned ABI 42 and Debian coreutils 0. The case still
needs guest execution on Switch 2. The development launcher now merges
managed inode entries into rootfs listings and resolves the selected ELF
and its interpreter through the guest namespace, including logical EOF.
The development NRO and host checks passed. Browser selection and
execution of a managed ELF still need console verification.

Memfd growth checks now return the seal error before the runtime
allocation limit. The packaged probe covers scalar and vector writes
across sealed EOF, unchanged data and offsets after rejection, and
successful in-range writes. Native acceptance returned ABI 42 and
Debian coreutils 0. The development NRO built at the existing version.

### Milestone 1 feature outline and acceptance

| Group | Required behavior | Combined suite coverage |
| --- | --- | --- |
| Descriptors and I/O | Open descriptions, descriptor sharing and inheritance, standard and positioned I/O, vectored flags, offsets, directory cookies, locks, sync and filesystem statistics | ELF ABI cases, fork and thread cases, stock Debian utilities |
| Paths and links | Namespace traversal, descriptor-relative lookup, scoped open, symlinks, hard links, directory and special-node lifecycle, rename and unlink with open handles, crash recovery | ELF namespace cases, stock utilities, restart and interrupted transaction checks |
| Metadata and permissions | Stable inode identity, mode, ownership, ACL authorization, timestamps, descriptor and pathname stat variants | ELF metadata and access cases, stock utilities, restart checks |
| Allocation and transfer | Sparse extents, allocation modes, growth and truncation, cache advice, file and pipe transfers with correct offsets, errors and partial progress | ELF range and transfer cases, large and sparse copies |
| Extended attributes | All legacy and descriptor-relative variants, namespace permissions, flags, binary values, ACL inheritance and persistence | ELF attribute cases, stock archival copy, restart checks |

Use one combined acceptance run for the ELF and Debian utility checks,
with restart phases for persistence and interruption recovery. Report
host execution and Switch 2 execution separately. Batch console checks
until hardware evidence is required, then allow the requested 90-second
response window before continuing. Increase the NRO version only when
preparing a build for a new physical Switch 2 run.

### Milestone 2 feature outline and acceptance

| Group | Required behavior | Combined suite coverage |
| --- | --- | --- |
| Processes and sessions | fork/vfork/clone/clone3 lifecycles, exec variants, exit and exit_group, wait4/waitid selectors and states, reparenting, rusage and times, process groups and sessions, controlling terminal, pidfds, rlimits, priority, prctl subset | `proc/`, `session/` cases, Bash pipelines and job control |
| Signals | Process and thread pending sets, default actions (terminate, core, ignore, stop, continue), queued realtime signals with siginfo, masks, suspend/wait/timedwait, restart semantics, alternate stacks, signalfd, pidfd signals | `signal/` cases, Bash traps, `kill`, `timeout` |
| Credentials and capabilities | set*id transition rules, supplementary groups, filesystem IDs, capability sets and their transitions across set*id and exec | `cred/` cases run as uid 1000 and root |
| Clocks and timers | All clock IDs incl. CPU and per-pid CPU clocks, resolution, settable realtime for root, sleeps, alarm, itimers, POSIX timers with overrun, timerfd | `clock/`, `timer/` cases, `sleep`, `timeout` |
| Polling and watches | poll/select/pselect/ppoll readiness with signal masks, epoll (LT/ET/ONESHOT, nesting, description lifetime), eventfd, inotify events and watch lifetime | `poll/`, `epoll/`, `event/`, `inotify/` cases, `inotifywait` |

Use one combined acceptance program: `/bin/process-acceptance.elf` is a
wrapper that runs `proc-suite.elf` and `ProcessTools.sh`. Pair the
console run with an interactive job-control checklist. Report host
execution and Switch 2 execution separately, and track every syscall in
[ProcessInventory.md](docs/ProcessInventory.md).

### Filesystem implementation in progress

Vectored I/O now imports the full descriptor array and validates signed
lengths before checking v2 flags. Zero-length vectors return zero before
flag validation. Read-side priority, sync and append flags follow the
native Linux result. File vectors use the Linux maximum transfer count
and the probe checks a 96 KiB memfd round trip in single positioned
calls. Socket and console limits and the remaining flag and fault cases
still need reconciliation.

Vectored I/O now checks guest address ranges and offset overflow before
dispatch, returns zero for an unmapped destination at regular-file EOF,
and handles `/dev/null` without touching the vector buffers. The
expanded native acceptance suite returned ABI 42 and Debian coreutils 0.

Scalar and positioned I/O on the null device now check user address
ranges without touching buffer pages. The ABI probe checks unmapped
buffers, positioned offset overflow, and the Linux transfer cap.
The development NRO built and packaged native acceptance returned
ABI 42 and Debian coreutils 0.

Scalar reads now share address and position checks, and use the same
regular-file EOF handling as vectored reads when the buffer is unmapped.
Regression cases cover both sides of EOF, offset overflow, and zero-byte
requests with an invalid address. Packaged native acceptance returned
ABI 42 and Debian coreutils 0. The development NRO built.

Scalar and positioned writes now share guest address and offset
validation. Regular-file writes use the Linux maximum transfer count.
The packaged probe covers invalid-address and overflow precedence,
single 160 KiB writes, positioned write offsets and copied content.
Native acceptance returned ABI 42 and Debian coreutils 0. The guest
NRO built. Large SD writes still need console verification.

Regular-file scalar reads now use the Linux transfer cap. SD reads
use bounded 64 KiB service calls and return bytes already transferred
if a later call fails. The ABI probe checks full-size 160 KiB scalar
and positioned reads and their offsets. The development NRO built and
packaged native acceptance returned ABI 42 and Debian coreutils 0.

SD payload writes now use bounded 64 KiB service calls. If a later
chunk fails, offset updates and return counts reflect completed
chunks, and cleanup attempts to keep the original backing plus
successful extension. The native large-transfer cases passed and the
development NRO built. SD partial-failure behavior remains unverified
on hardware.

`make -C Source/Switch2 check-guest-file-write` now exercises the same
write loop used by the SD runtime. It injects errors during gap
clearing and at each payload chunk, and checks the reported byte count
and retained backing length. The new test, four existing host checks,
and the development NRO build passed. Native packaged acceptance
returned ABI 42 and Debian coreutils 0.

Vectored I/O now honors RWF_NOWAIT for pipes, sockets and the null
device, and RWF_NOSIGNAL suppresses SIGPIPE on pwritev2. The ABI probe
checks blocking pipe descriptors with empty, ready and full queues,
as well as EPIPE with a signal handler installed. Native acceptance
returned ABI 42 and Debian coreutils 0. The guest NRO built at the
existing version. SD and memfd RWF_NOWAIT remain unsupported.

Existing-inode open and F_SETFL now enforce O_NOATIME authorization
using guest filesystem credentials. chmod clears setgid when an
unprivileged owner is outside the inode's group. The native suite
checks O_NOATIME rejection and verifies that a rejected F_SETFL leaves
other flags unchanged. Native acceptance returned ABI 42 and Debian
coreutils 0, and the guest NRO built at the existing version.

The standalone Switch 2 probe and development guest NRO built without a
version bump. Host VMA, inode store, log and terminal tests passed.
Native Linux combined acceptance passed with ABI 42 and staged Debian
coreutils 0. Both native persistence phases returned 42. These checks
add no console results.

POSIX ACL xattrs now handle empty and header-only values as removals,
apply the ACL-specific create and replace rules, distinguish unsupported
versions from malformed entries, and enforce the inode type restrictions.
Legacy descriptor xattrs reject O_PATH descriptors. O_TMPFILE accepts
O_TRUNC without retaining it in F_GETFL and retains O_NOFOLLOW.
The expanded combined suite passed on native Linux (ABI 42, Debian
coreutils 0). The standalone probe and development guest NRO built at
the existing version. Console validation remains pending.

The path walker now detects crossings into and out of the virtual
`/dev` and `/proc` mounts with RESOLVE_NO_XDEV, including symlink
targets and paths that cross a mount before backtracking. Open and stat
resolve aliases of the existing virtual files. RESOLVE_NO_SYMLINKS
rejects traversal through `/proc/self` and `/proc/mounts`. The expanded
native suite passed (ABI 42, Debian coreutils 0), and the development
NRO rebuilt at the existing version. Virtual directory descriptors
and the remaining procfs link behavior still need work.

Pathname statfs now reports the mount reached by pathname resolution.
Aliases of existing virtual files report the same filesystem type as
direct paths, and stat and fstat agree on their device identity. The
expanded native acceptance suite returned ABI 42 and Debian coreutils 0.
The development guest NRO built at the existing version.

The synthetic `/dev`, `/proc`, and `/proc/self` directories now
support open, stat, enumeration, and descriptor-relative traversal of
the existing virtual entries. The reference probe checks procfs
directory identity, enumeration, and RESOLVE_NO_XDEV from within a
mount. Native acceptance returned ABI 42 and Debian coreutils 0.
The development guest NRO built at the existing version. Procfs
symlink identity and on-console behavior still require verification.

`/proc/self` and `/proc/mounts` now resolve as symlinks through the
normal path walker. The procfs directory exposes numeric guest PIDs,
and the acceptance probe covers link identity, readlink, O_NOFOLLOW,
and O_PATH with empty-path readlinkat. Native acceptance returned ABI
42 and Debian coreutils 0. The guest NRO built at the existing version.
Full procfs contents and console behavior remain outside these checks.

SD hole punching now zeroes partial blocks without dropping their
allocation. A punch that covers the allocated tail reclaims only full
blocks and preserves logical EOF. The ABI probe checks partial punches
inside a file and at EOF, including SEEK_DATA after a partial punch.
The development NRO built at the existing version. Native combined
acceptance returned ABI 42 and Debian coreutils 0, and both persistence
phases returned 42. Full interior block reclamation and memfd hole
punching remain open.

Memfd write seals now enforce descriptor write and hole-punch
authorization, distinguish existing shared mappings from mappings
created after F_SEAL_FUTURE_WRITE, and reject F_SEAL_WRITE while a
potentially writable shared mapping exists. The combined native suite
passed (ABI 42, Debian coreutils 0), the host address-space test covers
mapping authorization, and the development NRO built at the existing
version. Unsealed memfd deallocation and console checks remain open.

Memfd partial-page punches zero the selected bytes in place while
preserving EOF and the allocation. The ABI probe verifies the result
through both an existing shared mapping and a positioned read. Full
pages and reservations beyond EOF still need reclaimable backing.
Native acceptance returned ABI 42 and Debian coreutils 0. The guest
NRO built at the existing version.

Partial punches also clear the requested bytes beyond logical EOF
within the last mapped page. The ABI probe checks both sides of EOF
through a shared mapping and confirms the logical size stays fixed.
Packaged native acceptance returned ABI 42 and Debian coreutils 0.

Copy-file-range now loads both explicit offsets before validating
flags and ranges, and distinguishes unsigned range overflow from a
negative offset. The expanded native suite returned ABI 42 and Debian
coreutils 0. The guest NRO built at the existing version.

File advice now distinguishes pipes from sockets for fadvise and
accepts readahead on the already-resident synthetic proc files.
The native probe covers these descriptor types, invalid advice and
offsets, and confirms that readahead preserves the descriptor offset.
Native acceptance returned ABI 42 and Debian coreutils 0. The guest
NRO built at the existing version.

SD ZERO_RANGE now zeroes the range and any newly exposed gap before
publishing the new logical EOF. On a write error it attempts to restore
the original backing length. KEEP_SIZE on an unnamed managed file
uses its existing backing, and publication preserves the reservation
and visible size. The expanded native acceptance suite returned ABI 42
and Debian coreutils 0. The guest NRO built at the existing version.

Sendfile now validates the input descriptor and position before the
output descriptor, including overflow of the requested input range.
The ABI probe checks EBADF and EINVAL precedence and verifies that
rejected calls leave the explicit input offset unchanged. Native
acceptance returned ABI 42 and Debian coreutils 0. The development
NRO built at the existing version.

Ordinary files now use Ounce SD handles for writes, positioned writes, create
and exclusive create, append, pathname and descriptor truncation, and flush.
The SD pathname walk checks intermediate components before handling `..`.
Directory creation, empty-directory removal, `fchdir`, `readv` and both
directory-record layouts are wired. `pread64` validates offsets and descriptor
types before handling a zero-byte request. Vectored reads and writes use
one backing operation, and the positioned variants and v2 offset `-1` are
routed. `RWF_APPEND`, `RWF_NOAPPEND`, `RWF_DSYNC`, and `RWF_SYNC` are
wired for writes. `O_SYNC` and `O_DSYNC` flush SD writes, and `O_NOATIME`
preserves access times on reads. The reference checks flags, append
override and access-time preservation.
Other v2 flags and Linux transfer-size and fault-order parity require
additional coverage. These changes are build-verified. The Debian probe
checks positioned vector contents, append and offset behavior on native Linux.
`flock` uses open-description ownership, cooperative blocking on conflict,
and weak holder references so closing the final reference drops the lock.
The Debian reference probe checks shared holders, nonblocking conversion,
and lock ownership through duplicated descriptors. Console verification and
signal interruption of blocking calls remain pending. The `fcntl` lock
commands now handle process and open-description byte ranges, conflict
queries, split unlocks and close-time release. The Debian probe checks
same-inode conflicts between the two ownership models. Deadlock reporting,
interruption, and console behavior remain pending. Pipe capacity can be
queried and changed on either endpoint. The ioctl path now also supports
`FIONREAD`, `FIONBIO`, `FIOCLEX` and `FIONCLEX`. The reference checks
capacity, nonblocking writes, bytes pending and poll readiness. Linux pipe
page-slot accounting still differs from the guest byte queue.

SD inode numbers are assigned for the lifetime of a launcher run and moved
with directory subtrees on successful native rename. Rename to an unused name
updates open descriptor paths and working directories. Removed directory
handles stay detached even when their former name is recreated. Directory
iteration builds its snapshot on demand and refreshes it on rewind. The
Debian probe checks these invariants on the reference system. Native SD
rename with live handles is still hardware-unverified. Regular-file unlink
uses the native SD delete operation, drops the name's inode association only
on success, and marks the former inode unlinked for descriptor stat. The
reference probe checks open-handle reads and writes after unlink and reuse
of the name. Whether Ounce permits deletion with live handles still needs
hardware evidence. Mode, timestamps, attributes and unlink state now share
one inode record. The launcher now saves linked inode numbers and metadata
to alternating checksummed SD snapshots under `/fex`. It loads the newest
valid generation, saves at regular-file sync and launcher teardown, and
keeps runtime-only and unlinked objects out of the snapshots. The host
format tests verify binary values, shared inode numbers and recovery after
truncation or corruption. On-device restart recovery is still unverified.
A native namespace mutation and its metadata checkpoint are separate
operations, so power-loss consistency across rename/unlink still needs a
transactional namespace.

Guest symlinks now live in persisted inode records and participate in lookup,
directory listing, stat, readlink, rename to unused names, and unlink.
Relative targets are resolved from the containing directory, absolute targets
from the guest root, and loops return ELOOP. The Debian reference checks
nofollow, truncated readlink, link rename, and loops. `O_PATH` now preserves
descriptor flags, rejects I/O, and can retain a symlink for `fstat` and
empty-path `readlinkat` after unlink. The Debian reference verifies those
operations. Remaining work includes virtual device targets, additional path
descriptor and symlink edge cases, and hardware verification.

Hard links share an inode and one SD backing object. First link promotion
moves an ordinary SD file into the reserved object directory and checkpoints
the linked names, with rollback on a reported checkpoint failure. Open,
stat, truncate and guest exec resolve the backing path independently of the
link name. The reference probe checks write visibility through aliases.
Native promotion while an SD handle is open and on-device persistence remain
unverified.

Object promotion now writes a checksummed recovery record before the SD move.
Startup uses the checkpoint generation and observed file locations to retain a
committed object or roll back an uncommitted move. Ambiguous recovery keeps
the record and disables metadata writes. On-device write ordering and startup
recovery still need verification. Replacement and exchange for two entries
already in the metadata namespace now use one checkpoint. The native Linux
reference verifies exchange and replacement of symlinks and regular files.
Regular SD files promote under their original names before an exchange or
replacement, so the final rename changes both names in one checkpoint.
Directory replacement stages the empty destination under a recovery record,
moves the source, and commits the inode namespace before removing the
displaced backing directory. Recovery rolls back an uncommitted move or
finishes committed cleanup. The reference checks nonempty rejection,
detached descriptors, and stable source identity. Two-directory exchange
uses the same intent with an exchange marker and finishes the staged native
move only after the swapped namespace is durable. The probe verifies exchanged
subtrees and descriptor-relative cleanup. Mixed directory and nondirectory
exchange first promotes the nondirectory when needed, so recovery has only
one native directory move to reconcile with the namespace checkpoint. The
reference checks both operand orders with a symlink and retained directory
descriptor. The recovery planner used by the launcher is tested across all
48 combinations of transaction kind, checkpoint state, and native path
presence. Physical SD write ordering and on-console transaction verification
remain pending. A replaced
managed backing object is retained for live descriptors and older snapshots.
Managed unlink also checkpoints the name removal while retaining the backing
file, so live handles can survive the last link. Object retirement is saved in both alternating snapshots before the launcher
can reclaim an object on startup. Recovery keeps objects referenced by the
fallback snapshot. Existing objects that predate recorded retirement are
retained. The host codec tests cover retirement and fallback retention.
On-device reclamation and crash recovery remain pending. Unlink of a live ordinary file also attempts promotion
before removing the name. If native promotion fails with a generic SD error
while handles are live, the runtime flushes the handles, retries the move
with native handles closed, and reopens the same guest file descriptions
without changing their offsets or flags. It attempts rollback if reopening
fails and retains the recovery record if rollback cannot finish. The native
SD rename and retry paths still require hardware verification.
Creation uses a per-process umask inherited at fork. `chmod`, `fchmod`,
`fchmodat` and `fchmodat2` share inode-level mode state, and the current
fixed guest credentials are used for owner access checks on open, traverse,
create, remove, rename and truncate. The Debian probe checks umask, mode
updates and denied opens. The mode state is currently in memory.
All sixteen xattr calls route to the same inode state, including the modern
descriptor-relative ABI. User attributes support binary values, create and
replace modes, listing, size queries, and removal. The native reference probe
verifies legacy and modern API interoperability. POSIX ACL xattrs now
validate their binary layout, update access-mode bits, persist with inode
attributes and propagate default ACLs on creation. The reference checks
inheritance, its interaction with umask, chmod mask updates and malformed
entries. Named-user and group authorization with multiple guest credentials,
security namespaces, and console persistence remain open.
The inode timestamp state imports available SD times, supports `utime`,
`utimes`, `futimesat` and `utimensat`, and is visible through both stat ABIs.
The native probe verifies subsecond round trips and `UTIME_OMIT`. Timestamp
state is saved with inode snapshots. Path and descriptor stat now compute
directory link counts from native child directories. Successful namespace
mutations update the affected parents' modification and change times. Parent
times currently enter the next checkpoint for operations that checkpoint
names inside the syscall. The reference checks subdirectory link counts and
parent mtime. On-console verification remains pending.
The chown family is routed under the current single unprivileged identity,
including checks for forbidden changes and clearing applicable set-ID bits.
Multiple credentials and privileged changes depend on the later credential
group. All 98 audited group 1–5 syscall numbers now have routes. This
is a dispatch count. Existing partial paths still need the semantics and
acceptance work tracked above and below. `mknod` and `mknodat` create
regular files and persistent FIFO and socket inode types. Named FIFO opens
share live pipe endpoints and support blocking rendezvous, nonblocking
opens and open handles after unlink. The reference verifies regular-node
creation, FIFO I/O and writer ENXIO without a reader. FIFO state now
retains buffered data while either direction has an open description and
tracks writer generations for reader hangup polling. The probe checks reopen
and poll transitions. Privileged device creation, integration with Unix
socket binding, SIGPIPE delivery, and hardware verification remain pending. `openat2`
parses the extensible ABI and enforces beneath, in-root and no-symlink
constraints during traversal. Cache-only lookup reports EAGAIN because
the SD path walk requires native service calls. The reference probe
covers scope escapes, symlink traversal and extended argument validation.
Additional flag and virtual mount cases remain pending. `O_TMPFILE` now
creates a nameless backing object, records retirement before creating it,
and allows publication with `linkat(AT_EMPTY_PATH)` unless opened with
`O_EXCL`. The reference checks link counts, publication, identity and the
exclusive restriction. Hardware and remaining temporary-file flag parity
are unverified. Descriptor tables
are now shared by guest threads, copied on fork, and retained from the
calling thread on exec. `close_range` supports closing, close-on-exec and
unsharing. The native test checks a private thread's flag changes and
closure while the caller retains its descriptor and shares the file offset.
`readahead` and `fadvise64` now use a bounded SD read cache. Writes and
truncation invalidate the cached inode across all descriptors. Positional
prefetch is synchronous because the current backend uses Ounce filesystem
IPC. The reference probe checks offset preservation and read-after-write
coherence across opens. Device coverage and console verification remain open. The runtime now routes `copy_file_range` between files of the same backing
type and `sendfile` to writable file, pipe and socket descriptors. Transfers
can return short counts and update offsets by the bytes actually written.
The reference probe checks explicit and implicit offsets, overlapping-copy
rejection, and sending file data to a pipe. Console verification and the
remaining allocation operations are pending. `splice` and `tee` peek until
the output accepts bytes, and `vmsplice` shares the vectored transfer path.
The reference exercises file-to-pipe splice, nonblocking tee, duplication
without input consumption, and vectored pipe writes. Guest page pinning
and complete Linux pipe buffer accounting remain pending. `fallocate`
extends backing allocations and supports SD zero-range writes. Unsupported
hole-punch and extent operations return errors. `sync_file_range` uses the
native whole-file flush because Ounce does not expose a range flush. The
reference checks extension, zero fill from growth and sync. The SD error translation covers
path-not-found and path-exists and still needs good coverage of backing
store failures. `statfs` and `fstatfs` report native SD capacity and free
space, and `syncfs` flushes live writable files and the metadata checkpoint
before committing the SD filesystem. The reference probe checks path/fd
agreement, capacity invariants, sync and an invalid descriptor. Device and
memory filesystem statistics and the native SD commit result still need
verification. The raw SD operations are a first backend slice, and group
1–5 completion requires the full ABI and real application verification.

`tests/guest/FilesystemProbe.c` creates a unique scratch directory and
covers positioned and vectored I/O, append, shared offsets after dup, file
growth, both getdents layouts, relative operations after directory rename,
link identity, open-after-unlink, mode, timestamps and xattrs. Its Debian
ELF builds with `make -C Source/Switch2 filesystem-probe
DEBIAN_ROOT=/path/to/debian-sysroot` and exited 42 with the Debian 12
glibc 2.36 loader on native Linux. It advertises x86-64-baseline.
`tests/guest/CoreutilsFilesystemProbe.sh` passed on native Linux using the
Debian 9.7-3 coreutils binaries and the staged Debian 12 coreutils 9.1-1
binaries using the bundled Debian loader and Bash. The guest exposes the
SD-backed root mount in `/proc/self/mountinfo` and `/proc/mounts` for `df`.

Build the acceptance bundle with `make -C Source/Switch2 filesystem-runtime
DEBIAN_ROOT=/path/to/debian-sysroot`. Provide unpacked Debian 12 coreutils,
Bash, glibc and its startup objects, ncurses terminfo, and their library
dependencies. The output defaults to `Source/Switch2/build/filesystem-runtime`.
Its rootfs includes `/bin/filesystem-probe.elf`,
`/bin/filesystem-coreutils.elf`, and the two persistence launch entries
`/bin/filesystem-persist-write.elf` and `/bin/filesystem-persist-read.elf`.
The persistence entries use the same probe with distinct arguments. The
Bash entry runs the coreutils script through the normal guest exec path.
Expand the probe matrix to remaining cases.

The Ounce JIT probe and default guest launcher built. The latter used the
configuration without optional graphics dependencies. The address-space,
terminal, and log regression checks passed. Console results are recorded
below.

### First filesystem console run

The 0.12.12 hardware log shows the coreutils suite exited 0 and both
persistence runs exited 42, including the read after restarting the NRO.
The full filesystem probe exited 1. Reopening a file with a live writable
SD handle repeatedly produced native result `0x00000e02`. The same
restriction affected path stat, attribute operations and directory rename.
The guest also observed nonzero bytes after file growth and sparse writes.

0.12.13 keeps guest open descriptions and offsets but opens native SD
handles only for individual operations. It writes zeros explicitly for
growth and sparse gaps. This removes the native handle conflict without
closing the guest descriptors. The filesystem launcher is built under
`Source/Switch2/build/guest-filesystem/`. The user confirmed that both
the full filesystem probe and the Debian coreutils suite passed on
0.12.13. The cross-launch persistence pass was on 0.12.12.

This completes the console smoke-test gate. Full groups 1–5 sign-off
still requires the remaining Linux-behavior audit and on-console
transaction recovery verification.

### Filesystem work after the smoke tests

0.12.14 adds POSIX blocking record-lock cycle detection. Each blocked
request retains its inode and byte range so the runtime can follow live
conflicting owners and return `EDEADLK` when the new request closes a cycle.
Open-file-description locks do not participate in POSIX cycle detection.
The combined filesystem probe now forks two lock owners and requires one
`EDEADLK` and one successful acquisition, with the successful waiter
waking after the other owner releases its lock. The expanded Debian probe
exited 42 on native Linux. The Ounce NRO and staged Debian bundle rebuilt,
and host inode-store, address-space, terminal and logging checks passed.
The new lock contention case has not been run on Switch 2. Interruption
of the blocking calls still depends on guest signal delivery.

0.12.15 extends `statfs`, `fstatfs` and `syncfs` to the existing
synthetic proc files, device nodes, memfds, anonymous pipes and sockets.
Device and proc path lookups match the filesystem type reported on their
descriptors. Named FIFOs remain on the SD filesystem. `fstatfs` supports
`O_PATH`; `syncfs` rejects it with EBADF. Synthetic filesystems report
filesystem identity and page size without inventing storage capacity.
The expanded probe exited 42 on native Linux and the NRO and Debian
bundle built. Console verification is pending.

0.12.16 matches Linux descriptor validation order for `fallocate`
and `sync_file_range`, including bad and path-only descriptors, invalid
ranges and pipe ESPIPE. The expanded Debian probe exited 42 on native
Linux. The Ounce NRO and staged bundle rebuilt. Extent operations and
console verification remain open.

0.12.17 handles `copy_file_range` offset writeback faults after the
transfer. The runtime reads both offsets before copying, then attempts
both writebacks when bytes moved. The probe checks EOF, zero-length
copies, and a read-only offset that returns EFAULT while retaining the
copied bytes and updating the writable offset. The expanded probe
exited 42 on native Linux. The staged bundle and NRO built. Console
verification remains pending.

0.12.18 extends offset writeback behavior to `sendfile` and
`splice`. The probe verifies that a writeback fault leaves bytes in the
destination pipe, and that a zero-length splice skips descriptor
validation. The expanded probe exited 42 on native Linux. The staged
bundle and NRO built. Console verification is pending.

0.12.19 returns EISDIR for zero-length reads on directories. The Debian
probe also checks memfd append changes through duplicated descriptors,
including positioned writes and their effect on the shared offset.
The expanded probe exited 42 on native Linux. The staged bundle and NRO
built. Console verification remains pending.

0.12.20 adds `SEEK_DATA` and `SEEK_HOLE` for dense files. The only
hole is at EOF because the current backings allocate and zero-fill
growth. The probe checks both seek origins in a written extent and
confirms that EOF and negative offsets return ENXIO without changing
the current offset. The expanded probe exited 42 on native Linux,
and the Ounce NRO and staged bundle built. Console verification and
sparse extent support remain open.

0.12.21 makes both getdents variants honor the 32-bit count ABI and
update directory access time after partial batches. The combined probe
reads a single record, resumes from its offset cookie, and verifies EOF
for both layouts. It exited 42 on native Linux. The staged bundle and
NRO built. Console verification remains pending.

0.12.22 supports null pathname pointers with AT_EMPTY_PATH in the shared
stat lookup, validates statx sync flags before lookup, and copies stat
results only after successful lookup. Descriptor stat now reports the
same /dev/null device number as pathname stat. The expanded Debian probe
exited 42 on native Linux and the Ounce NRO and staged bundle built.
Console verification remains pending.

0.12.23 enforces directory resolution when a symlink target ends in a
slash. It also honors the signed 32-bit readlink buffer length and
reports ENOENT for an empty path on a valid non-symlink descriptor.
The expanded Debian probe exited 42 on native Linux and the NRO and
staged bundle built. Console verification is pending.

0.12.24 rejects trailing slashes on synthetic nondirectory nodes for
stat and hard-link sources, and returns EISDIR when open with O_CREAT
targets a pathname ending in a slash. The probe covers a named FIFO
and a missing path. It exited 42 on native Linux, and the NRO and
staged bundle built. Console verification is pending.

0.12.25 checkpoints parent directory timestamps with hard-link creation,
managed unlink, and transactional rename. A failed checkpoint restores
the previous parent timestamps along with the namespace change. The NRO
build and host inode-store checks passed. Console recovery remains open,
as do native mutations that still lack a namespace transaction.

0.12.26 checkpoints renames of managed regular files, symlinks and
special nodes to unused names, with namespace and timestamp rollback
on a reported checkpoint error. The persistence probe now renames a
staged symlink and checks that only its final name survives. Both
persistence phases exited 42 on native Linux, and the Ounce NRO,
staged bundle and host inode-store check passed. Native SD renames
to unused names and console recovery verification remain open.

0.12.27 adds recovery intents for native file and directory moves to
unused names. It reuses the existing single-move recovery states to
restore an uncommitted move or retain a committed one. The Ounce NRO
and host inode-store tests passed. Interrupted console recovery
remains unverified.

0.12.28 applies the namespace journal to ordinary native unlink and
empty-directory removal. The runtime first moves the entry into reserved
storage, then checkpoints removal of the name and its cached descendants.
Recovery restores an uncommitted removal or deletes the committed backup.
The host recovery planner exercises all 64 presence and commit states,
and the Ounce launcher built. Physical recovery and SD write ordering
remain unverified. Native creation durability remains open.

The current development build stages new SD files and directories under a recovery intent,
checkpoints initial mode, inherited ACLs and timestamps, then publishes
the native entry. Recovery discards an uncommitted staging entry or
publishes a committed one. The host planner checks all 80 states and
the NRO builds. Interruption and physical write ordering still need
console verification.

Symlink and special-node creation now checkpoint the new inode and
its parent timestamps using the same metadata path as native creation.
A failed checkpoint restores the prior name and timestamps. The two
native Linux persistence phases also check a FIFO's type and mode
after reopening. Both exited 42. The NRO and host inode-store and
address-space tests passed. Switch 2 persistence remains to be checked
with the updated bundle.

The staged rootfs now provides `/bin/filesystem-acceptance.elf`, a
Bash launch entry that runs the ABI probe and Debian utility checks and
requires statuses 42 and 0 respectively. The combined script exited 0 on native Linux
with the Debian-linked probe and the host's coreutils. Keep the separate
persistence entries for the relaunch boundary. The combined entry does
not yet cover all items in the feature outline. Sparse extents, transfer
and permission coverage, and interrupted console recovery remain open.

Timestamp updates now support persistent FIFO and socket inodes.
The probe checks FIFO timestamp round trips and distinguishes the
descriptor-only utimensat form from explicit AT_EMPTY_PATH on an
O_PATH descriptor. The combined native Linux suite passed, and the
NRO built at the existing version. Console verification is pending.

Unprivileged file writes, truncation and allocation now clear setuid
and clear setgid when group execute is set. The combined suite checks
that these operations preserve both bits under privileged guest
filesystem credentials. The privileged branch still needs a run.
The combined suite also checks
positioned writes, descriptor truncation, allocation inside the current
size, and truncation on open. It passed on native Linux, and the NRO and
Debian bundle built at the existing version. Console verification is pending.

The truncate probe now covers named FIFOs, trailing slashes, directories
and negative lengths on invalid descriptors. Runtime validation matches
those native Linux outcomes. The combined suite passed on native Linux
and the NRO rebuilt at the existing version.

Regular-file and memfd scalar I/O now transfers the accessible
prefix when a buffer crosses into an inaccessible page. Positioned
operations keep descriptor offsets unchanged. Vectored I/O can also
walk adjacent accessible mappings. The combined native Linux suite
passed, and the NRO rebuilt. Pipe and terminal fault behavior and
on-console verification remain open.

Memfd fallocate now reserves backing memory with KEEP_SIZE without
changing EOF and enforces F_SEAL_GROW for reservations past EOF.
The combined probe checks deferred growth, zero-filled gaps and seal
errors. It passed on native Linux, and the NRO rebuilt at the existing
version. SD reservation beyond EOF and actual hole deallocation remain
open.

SD zero-range allocation now performs size checks, growth and clearing
through one native handle. It attempts to restore the old EOF if clearing
fails after growth. The combined reference suite covers in-file KEEP_SIZE
zeroing, zeroing across EOF and unchanged descriptor offsets. Native
Linux passed and the NRO built. The Ounce SDK exposes range clearing for
supported files and storage, but its public file API exposes no separate
reserve-space operation. SD reservations past EOF and deallocation still
require a backing representation with separate allocation and logical size.

SD KEEP_SIZE now promotes the file into managed storage when necessary
and checkpoints logical EOF before extending the physical backing. Guest
stat, reads, read-ahead and exec honor logical EOF, and append uses it
to select the write offset. The inode codec reads snapshots predating
this field. The expanded combined reference suite checks reservation
past EOF and passed on native Linux. The host codec check and NRO build
passed. Physical allocation reporting, reclaim on shrink, hole punching
and Switch 2 persistence verification remain open.

Path and descriptor stat now report the managed backing length in
512-byte blocks independently of logical EOF. A shrinking truncate
checkpoints EOF before releasing the backing tail. Failed post-checkpoint
reclamation is logged and can leave the allocation in place. The
combined reference suite checks allocation visibility and release; the
two persistence phases now check reserved space and EOF across reopen.
All passed on native Linux, and the NRO built at the existing version.
Physical recovery and hole punching remain open.

The combined acceptance probe now verifies byte-for-byte copies over
multiple transfer calls, including a nonzero explicit output offset,
implicit input advancement, EOF, and the destination size. The staged
bundle built and the combined suite passed on native Linux. The current
memfd backing is a single allocation shared with memory mappings and the
compositor, so releasing individual pages requires a backing change.

The combined runner now prints both results even after an ABI failure.
The ELF logs each group it enters and summarizes assertion failures.
Native verification covered both the successful run and a deliberately
wrong ABI exit code, which returned acceptance status 1 while still
running coreutils. The staged bundle was rebuilt.

The combined suite covers the error for an unsupported system xattr
and an empty user xattr suffix. The persistence phases pass a binary
xattr with embedded zero bytes through a renamed symlink and hard-link
alias. The combined suite and both persistence phases passed on native
Linux, and the Ounce NRO rebuilt at the existing version.

Filesystem sync now tracks dirty SD data by inode after the last
descriptor closes. It resolves the current name after rename and flushes
live unlinked files through their backing objects. The combined suite
checks sync after close and rename, plus fsync on a read-only reopen.
Native Linux passed and the NRO built. SD durability needs console data.

### 0.12.29 console verification

The combined suite passed on native Linux, including range sync through a
read-only reopen. The 0.12.29 NRO and updated Debian bundle built.
The user confirmed the combined acceptance run and both persistence
phases passed on Switch 2, including the read after restarting the NRO.
This verifies the cases exercised by the suite, including normal
namespace updates, reserved space and logical EOF across relaunch.

Full sign-off still requires sparse extents and hole punching, remaining
allocation and transfer semantics, permission and xattr interactions,
and interrupted SD recovery on hardware. Blocking-lock interruption
and SIGPIPE depend on guest signal delivery. The combined suite needs
coverage for these remaining cases before the milestone commit.

Pipe capacity now accounts for occupied buffer pages. Reading part of
a full page does not free a slot, and splice output uses nonmergeable
buffers. The combined acceptance probe checks partial drain on a
single-page pipe. The development NRO and staged suite built. Exact
splice page boundaries, vmsplice pinning, signal delivery, credentials
and sparse extents remain open.

The combined probe now checks transfer page boundaries, live shared and
private vmsplice pages, stack-backed buffers, and fork isolation while
a parent page remains queued. The staged bundle built. The new fork
case still requires runtime verification.

The persistence probe now checks a punched trailing SD extent across
reopen, including preserved EOF, zero reads and hole seeking. Both
phases passed on native Linux. The updated Switch 2 persistence case
and interior sparse extents remain open.

The recovery diagnostic build enables `FEX_SWITCH2_RECOVERY_TEST`
and leaves a native rename intent in place before or after the inode
checkpoint. The same filesystem probe has write and read modes for
each boundary. The staged `filesystem-recover-rollback-*.elf` and
`filesystem-recover-commit-*.elf` entries select these modes. Restart
between each write and read. This exercises startup recovery on SD
with a controlled interruption. Physical power-loss ordering still
requires hardware evidence.

SD and memfd now share a hole-punch planner that separates whole
blocks from partial blocks on either side. The host
`check-guest-file-punch` target covers interior, tail, unaligned and
out-of-range requests. The planner identifies the exact interior range
to reclaim. Moving that range to reclaimable backing remains open.

The inode snapshot can now record a replacement backing object
independently of the guest inode number. The host codec test checks
hard-link identity, fallback to the old generation after a torn
snapshot, and rejection of a retirement record for a live backing.
Startup reclaims retired objects only when both snapshots record
their retirement. The next runtime step is to construct and publish
replacement objects for interior punches.

The snapshot codec now round-trips ordered logical extents for a packed
backing object. Host tests verify that removing an interior extent
preserves both sides, maps them to compact backing offsets, and rejects
overlapping ranges. Packed reads now map allocated runs, fill holes
with zeros, and serve the same logical view to ELF loading. Hole and
data seeks use the extent table. The host tests cover reads across
an interior hole, seeks and backing read errors. The development NRO
built. Packed object creation, mutation and publication remain open.

The packed extent copier now streams retained ranges into consecutive
backing offsets with bounded scratch space and can apply changes to
the retained bytes as it copies. The host test checks the exact
compacted payload after an interior punch, along with stopping on an
injected write error. The development NRO built. The copier still
needs to be connected to SD object creation and checkpointing.
The development NRO and six host checks passed. Native packaged
acceptance returned ABI 42 and Debian coreutils 0.

## Shelved graphics milestone

The Wayland compositor bridge, native Vulkan probes, static Vulkan thunks
and their hardware history (0.12.1–0.12.12) are on branch
`shelf/graphics`. Resume after kernel coverage is complete, using FEX's
native Vulkan layer. Steam Big Picture remains the long-term goal.

## Backlog for future milestone selection

- **apt-get update closed the app on the console (0.17.13, 0.17.14; HOS error screen, no fault/abort line).** Both times
  while apt was "store"-ing a large Packages file (6.7 / 15 MB). Did not recur on 0.17.15 (2026-11-26). Evidence tools in
  place: fatal hooks (`WriteFatalLog`, `--wrap=abort`, `diagAbortWithResult`), the runtime trace written at once in
  Detailed/Trace. The 5 s memory report experiment (`ReportMemory`) was removed in 0.17.16 (no recurrence). If it happens
  again: Detailed log + HOS error code; bring the memory report back (`HostHeapReport()` every 5 s in `RunThreads`).
- **Full arm64 feature test on the console (user, 2026-11-27).** For performance work: probe every arm64 feature the
  console's A78C offers (by executing each instruction class in a probe, as mtprobe 3 did for LSE/RCPC/LRCPC2, since the ID
  registers cannot be read safely from EL0), then use the result for both builds: the compiler flags of the NRO itself
  (`-march`/`-mcpu` for the launcher, runtime and FEXCore host code) and the `HostFeatures` FEXCore generates guest code
  for (today the runtime passes none). Known so far: LSE, RCPC, LRCPC2 present (mtprobe 3).
- **Launcher review leftovers (0.17.16).** InRelease freshness (no Valid-Until / signature time check: replay of an older
  signed index is possible). The live RootFS index is plain HTTP (no TLS on the console): newer entries are limited to the
  built-in entry's host and marked not authenticated; a signed index would close it. The 0.16.3 perf counters
  (`LogPerfStats`, EXPERIMENT) stay for the performance work (Tab completion, apt, RootFS ETA).

2. **Desktop IPC foundation.** Fold the subset required by Wayland into
   milestone 3. Keep broader descriptor, file-backed shared memory, and
   event-loop compatibility here. Prove bidirectional shared writes
   after the original descriptor closes.
3. **Guest session manager.** Add preemption, signal delivery, background
   jobs, and session teardown. Verify a CPU-bound background job can run
   alongside Bash foreground commands and be stopped or killed.
4. **Steam networking foundation.** Add guest IPv4 TCP/UDP, DNS, socket
   options, and readiness handling. Verify stock Debian `curl` resolves a
   hostname and completes an HTTPS download.

## Completed milestone — interactive Bash terminal

The 0.9.17 Bash smoke test passed on Switch 2 in Trace mode. The Debian
runtime and IPC baseline is committed on `nx2` as `bc32e7ccc`. The
current milestone is an interactive terminal. Guest subprocess launch
and cross-process IPC follow it.

## Interactive subprocess build 0.11.0

The earlier tty build returned exit successfully but left the app's
screen black. The launcher now tears down the terminal renderer and
reinitializes the native console on the launcher thread before drawing
the result.

The 0.11.0 process path implements Bash's observed `clone` fork variant,
separate process IDs and groups, inherited descriptor references,
private versus shared memory views, `execve` with the guest argument and
environment vectors, close-on-exec, and `wait4` reaping. A child runs its
real fork continuation before loading a new ELF. Only one guest process
runs at a time. Switches restore private bytes to the original addresses;
shared mappings retain their bytes. Horizon does not enforce guest memory
isolation. Process and pthread waits use the same cooperative scheduler.
Terminal input also gives that scheduler a chance to run other guests.

Build and host verification passed. The host address-space test checked
that fork clones restore private bytes, preserve shared bytes, and can
unmap pages independently. The new Debian-linked process probe passed
on native Linux. Hardware verification remains required:

1. Launch **Bash shell**. Run `ls -la /bin`, then `echo returned:$?`.
   Verify the stock Debian listing, the prompt returning, and status 0.
2. Enter `exit 42`. Verify the launcher result screen stays visible
   and B returns to the home menu.
3. Run `process-probe.elf` from the updated SD bundle and expect
   `fork + pipe + execve + waitpid passed` and exit 42. Run it twice.
4. Rerun the pthread/IPC and noninteractive Bash probes with Trace.

The first hardware run of 0.11.0 closed the application on `ls`.
Horizon did not deliver the native exception record, so the only
persisted record ended before the trace buffer began. Version 0.11.1
moves fork snapshot and child context setup to the scheduler, after
the JIT dispatch has unwound. It validates native permissions before
copying private regions and persists sparse lifecycle checkpoints at
scheduler boundaries. Compiler and syscall trace lines remain buffered.
Use **Trace** and run a single `ls`; the last `Process checkpoint`
should distinguish snapshot creation, child restoration/dispatch,
exec replacement, and child retirement. Use `process-probe.elf` to
verify the same path without Bash.

The 0.11.1 hardware checkpoints reached child dispatch and successful
exec image preparation for `ls`; no child exit was recorded. The
0.11.2 scheduler now separates fork-shared native addresses from
independent exec address spaces. Only views that can overwrite one
another are snapshotted on a process switch; an inactive parent
still restores its saved bytes after its child execs. The diagnostic
logs first dispatch of the replacement and the first 12 completed
child syscalls. Rerun `ls` with Trace. Also compare
`exec /bin/ls` from a fresh shell, which replaces Bash directly and
isolates the exec path from fork.

Both 0.11.2 hardware runs reached the first replacement-image dispatch
and closed the application before the first syscall completion. Direct
`exec /bin/ls` reproduced the failure independently of fork. Version
0.11.3 retains the process's native FEX dispatcher across exec, clears
old translations, and initializes the new thread's CPU and segment descriptor
table with the same defaults as a fresh launch. Trace saves up to six
compiler checkpoints on the first replacement dispatch. The NRO build
and host terminal, log-buffer, and address-space checks passed. On
hardware, retest direct exec in Trace first, then test plain `ls` in a
fresh Bash session if direct exec succeeds.

On 0.11.3 hardware, direct `exec /bin/ls` printed the listing and
returned status 0. The launcher logged the result but the display went
black. Plain `ls` completed in a separate shell. The same shell then
crashed on `ls -la /bin`. Its log ends at PID 102's first fork
continuation dispatch, before an exec request for that command.

Version 0.11.4 keeps the initialized console renderer when the shell
returns, resets its text attributes, and submits the result screen.
Trace records the guest entry and stack ranges, native dispatcher
permissions, and up to six compiler and six syscall checkpoints for
each fork continuation. The process probe now exercises two sequential
children in one guest run. The native Debian probe ended with status 42
and the host terminal, log-buffer, and address-space checks passed.
On Switch 2, compare `ls -la /bin` as the first command with
`ls` followed by `ls -la /bin` in a fresh shell. Retest both
`exec /bin/ls` and `exit 42` for a visible result screen.

On 0.11.4 hardware, `ls -la /bin` reached child exit when it was
the first command. After a prior `ls`, PID 102 reached successful
x86 decode and IR generation with an executable native dispatcher,
then stopped before the dispatch of its first compiled block.
Direct `exec /bin/ls` returned status 0 and B worked from the blank
result screen. A subsequent guest launch in the same NRO closed the
application before a new buffered trace was saved.

Version 0.11.5 restores non-writable heap runs to read/write before
returning virtual allocations to the host allocator. The old free
path ignored permission failures across mixed-permission ranges.
This corrects a cleanup hazard that could affect later use of a
compiler guard page's backing; its connection to the hardware crash
remains to be verified. Native JIT close failures and FEX errors now
persist in the log. The startup trace covers worker and context
lifetime and the initial block through ARM64 emission and publication.
The launcher draws the result as a regular page after the worker
returns. The ARM64 diagnostic now creates, publishes, executes, and
closes JIT memory twice to exercise mapping reuse directly.

General asynchronous guest signals, stop/continue notifications,
background job control, scripts selected through shebang, and writable
file redirection are still pending. The 0.11.0 implementation targets
the real foreground fork/exec/wait path used by Bash. Do not treat the
remaining Bash/Linux compatibility work as verified.

## Terminal build 0.10.1

- **Bash shell** is the third home-menu item. It starts the stock
  Debian `/bin/bash --noprofile --norc -i`. The rootfs browser keeps
  using the existing `bash.args` smoke test.
- The guest terminal uses libounce USB HID keyboard input with a US
  layout and key repeat. It provides the terminal ioctls, descriptor
  duplication, and input readiness calls used by Bash/readline.
  ANSI foreground and background colors use the native console.
- Press both sticks together for a one-line Horizon keyboard. Confirming
  enters the line and submits Enter. Canceling leaves the shell input
  alone. Plus closes terminal input.
- The updated `build/debian-runtime.zip` includes Debian's ANSI terminfo
  at `/fex/rootfs/lib/terminfo/a/ansi`. Copy that file alone if the
  existing rootfs has the verified Debian files.

Hardware on 0.10.0 showed that Bash accepted the `ls` command and
rendered the cyan prompt. The preceding `2004h` was readline's DEC
private bracketed-paste control leaking through the console parser.
0.10.1 consumes private mode controls across split writes while
retaining ANSI color and cursor sequences. It also implements
`pipe`/`pipe2` for Bash's process-group channel.

A native trace of the same Debian Bash running `ls` then showed
`clone(CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID | SIGCHLD)` with no
child stack, process-group setup in parent and child, child
`execve("/bin/ls", ...)`, and parent `wait4`. This clone requests
an independent process with a private address-space view. The
existing pthread clone shares the address space and descriptor table,
so it cannot implement this call. Implement process isolation and
descriptor inheritance before enabling that clone variant; then add
exec and reaping and verify that `ls` returns to the same Bash prompt.

Host terminal, log ring, and VMA tests passed. The stock Bash PTY
reference passed color, arrow-key history, and exit 42 with the staged
ANSI terminfo. The NRO is built. Switch 2 hardware checks for 0.10.1 are pending:

1. Start **Bash shell** from the third home-menu item. Type `pwd` and
   `echo hello` on a physical USB keyboard. Test Backspace and Up.
2. Run `printf '\033[31mred\033[0m\n'` and verify red output and
   normal color afterward.
3. Open the Horizon keyboard with both stick clicks, enter
   `echo controller`, and confirm. Test cancellation separately.
4. Type `exit 42` and verify the launcher displays 42. Repeat the
   noninteractive Bash and thread/IPC probes with Trace.

The terminal launches a real interactive Bash. This phase covers
builtins and line editing. Running external programs, general
job control, and asynchronous terminal signal delivery need the
guest process and signal work listed below.

## Previous milestone — Debian tools and guest IPC

Branch `nx2`. Version 0.8.4 passed on Switch 2 with Debian 12 amd64 glibc
2.36. Debian `true` exited with status 0 and the glibc TLS and `dlopen`
probe printed its result and exited with status 42. Star Catch and the
custom dynamic ELF demo passed on earlier hardware runs.

## First hardware pass — launcher 0.9.0

The launcher uses one SD filesystem at `/fex/rootfs`. Browse Linux rootfs
launches the original Debian executables directly from `/bin`, with Linux
working directory `/`. Select executables in `/fex/games` through Browse
ELF files. Each optional `.args` sidecar supplies one argument per line.
The prepared bundle puts the correct interpreter and dependencies at
their Linux paths. The bundle uses the matching Debian packages.

The runtime owns the main stack and each FEX thread's state and return
stack. Guest pthreads share the address space and descriptor table and
are scheduled cooperatively at syscall boundaries. The glibc clone
fallback sets the child stack and TLS. Futex wait/wake and child-TID
clearing support thread joins. Unix-domain stream sockets support
socketpair, bind/listen/connect/accept, poll, partial transfers and
half-close. Anonymous MAP_SHARED mappings are visible to every thread
in the guest. Directory enumeration and metadata support the Debian ls
smoke test. Bash uses a noninteractive command with builtins.

### Build and host verification

- Run `make -C Source/Switch2` for the standalone JIT probe.
- Run `cmake --build Source/Switch2/build/guest --target guest_nro -j 8`
  for `Source/Switch2/build/guest/fex-switch2-guest.nro`.
- Run `make -C Source/Switch2 check-guest-memory` for the host VMA test.
- Run `make -C Source/Switch2 debian-runtime DEBIAN_ROOT=/path/to/unpacked/debian`
  to stage the test SD tree. The initial 0.9.0 bundle uses Debian Bookworm
  amd64 coreutils 9.1, Bash 5.2.15 and glibc 2.36. The five binaries
  below reproduced their expected output and exit status with the staged
  glibc loader on an x86-64 Linux host.
- The prepared SD archive is `Source/Switch2/build/debian-runtime.zip`.
  Extract it at the SD root so its top-level `fex/` folder lands there.
  Copy the NRO separately.

### Switch 2 verification

The first 0.9.0 hardware run passed `thread-ipc.elf`. Stock Debian `ls`
printed the directory and then reported `write error: Bad file descriptor`.
The runtime treated stdout specially and returned EBADF when coreutils
closed it. Version 0.9.1 uses the guest descriptor table for stdin,
stdout, and stderr, including close and descriptor reuse.

On 0.9.1, stock Debian `ls` passed on hardware. Bash still closed the
software. The final two compiled-block addresses had the same distance
and page offsets as in 0.9.0, even after disabling the unchecked JIT
shadow call/return stack. The crash remains reproducible at the same
relative locations.

Version 0.9.2 builds successfully. Its Trace log includes the backing
ELF or library filename, the offset, and the first x86 bytes on each
compiled-block dispatch line. The ELF launch screen offers **X Run
single-block** to test whether compiling one x86 basic block at a time
changes Bash's behavior. **A** retains normal compilation. Use the
existing SD bundle. If Bash still fails in single-block mode, capture
the last few `Dispatch compiled block` and syscall lines.

The 0.9.2 trace identifies the last compiled dispatch as
`ld-linux-x86-64.so.2+0x20c0`. The bytes match Debian glibc's loader
debugger rendezvous initialization. This identifies the last newly
compiled block and leaves open whether the fault occurs in that block
or a cached successor. Version 0.9.3 logs the JIT mode and records the
block's emitted ARM64 bytes and entry RDI/RSI in Trace. Test Bash with
**X** first. On failure preserve the `JIT mode`, `Compile RDI/RSI`,
and `Loader ARM64` lines and the final dispatch lines.


The 0.9.3 single-block trace passes the debugger initialization and
stops after compiling `ld-linux-x86-64.so.2+0x1d963`. This block in
`dl_main` reads the main executable's link map and follows its
`DT_STRTAB` entry while preparing the audit list. Stock Bash contains a
valid entry. The Switch loader's initial image was checked on the host:
`PT_DYNAMIC` is at Bash base + `0x12b5e8`, and its `DT_STRTAB` entry at
base + `0x12b678` holds the unrelocated value `0x141e0`. Native x86-64
GDB with the staged glibc loader reached the same block and observed
that entry, with its value relocated to Bash base + `0x141e0`.

Version 0.9.4 captures a checked `Loader link_map` snapshot and emitted
ARM64 bytes for that block. Run Bash with **X** and Trace, then send
the snapshot and final dispatch lines if it closes. If the snapshot
matches native Linux, inspect the generated loads and cached successor
path. If the snapshot differs, trace the setup of the main link map
before audit processing. A last-compiled address alone cannot identify
the exact faulting instruction.

The 0.9.4 Switch trace confirms a valid main map and correct dynamic
table base at `+0x1d963`, with `l_info[DT_STRTAB]` already zero. The
generated block loads through that zero pointer. Its AArch64 dump is
consistent with the guest's pointer loads.

Version 0.9.5 traces the preceding dynamic tag scan and prints the raw
tags, bypass flag, and populated slots. Native comparison used a
temporary copy of the stock Bash ELF with its PT_INTERP pointed at the
staged loader and started directly by Linux. At `+0x1cbac` its bypass
flag was zero, raw tags were `DT_NEEDED=1` and `DT_STRTAB=5`, and the
slot was initially zero. It reached `+0x1cbb9` and `+0x1cbfb`, populated
the slot by `+0x1cc38`, and retained it at `+0x1cdd2` and `+0x1d963`.
Native Bash exited 42. For the next hardware run use **X** and Trace
and collect every `Loader link_map` line in the latest Bash run.
This distinguishes a skipped scan, missing raw tag, failed write, or
a later overwrite.

The 0.9.5 hardware log shows dl_main enters the scan with the correct
dynamic table address and a zero bypass flag. The raw first tag reads
zero instead of `DT_NEEDED=1`, so the scan immediately stops. The word
at the expected `DT_STRTAB` offset also differs from the stock ELF.
The loader is reacting to incorrect guest bytes before any library
mappings are made. The snapshot alone leaves open whether the bytes
were wrong at SD ingestion or changed during guest execution.

Version 0.9.6 reads ELF files in bounded, page-aligned SD chunks and
validates that PT_DYNAMIC lies fully inside the file-backed image. It
logs the initial tag and the first observed change to the initial
dynamic bytes. Use the existing SD bundle and launch Bash with
**X** and Trace. Compare `Initial program PT_DYNAMIC`, any
`Program PT_DYNAMIC first change`, and the `Loader link_map` checkpoints.
The full hardware log is available locally when copied to
`/home/limo/fex-switch2-guest.log`.

The 0.9.6 hardware log confirms that the first tag is zero before guest
execution. Duplicating the CR in each of the 17 CRLF sequences in the
stock Bash ELF reproduces both corrupt words exactly: zero at file
offset `0x12b5e8` and `0x6ffffef500` at `0x12b678`. This points to text
conversion during file handling. The stock Debian Bash is 1,265,648 bytes,
SHA-256 `55b89ab22bee4792a210f493a53fb066accd5d30b69837c28d98be5ff863efcf`.
Replace the staged SD files using direct extraction or binary transfer
for the entire bundle, including libraries. Version 0.9.7 validates
the ELF dynamic table and logs each executable's raw SD size and
SHA-256 in Trace or on load failure. The host ELF check accepted the
unchanged fixtures and rejected the reproduced corrupt Bash.
Re-run Bash in normal mode after copying the verified bundle.

Both 0.9.7 hardware runs now hash the stock Bash and glibc ELF
interpreter correctly. Dynamic linking succeeds and Bash starts. The
remaining failure is later and differs by JIT mode:

- **A** (multiblock) stops during IR generation for libc file offset
  `0x5c400`, in the formatted-output implementation.
- **X** (single block) stops during ARM64 generation for libc file offset
  `0xa9de0`, in a vectorized string scan.

Version 0.9.8 registers the libounce native exception hook and uses a
separate exception stack to append the ARM64 PC, ESR, fault address,
LR, SP, and NRO base to the SD log. Compile-entry lines also carry
their file offsets. The build and host VMA test passed. Run Bash with
**A** and Trace, then use any `Host fault` line to map the faulting native
instruction against the matching ELF build. Keep the current SD files.

On the next hardware run, Bash printed `/bin` and exited with status
1. This reaches the builtin command and narrows the failure to the
executable-permission test. The native Debian trace confirms that
`[[ -x /bin/ls ]]` uses `faccessat2(AT_FDCWD, "/bin/ls", X_OK, AT_EACCESS)`.
Version 0.9.9 adds faccessat and faccessat2 using guest file metadata.
Retest with the current SD bundle and look for the success line and
exit 42.

The 0.9.9 run again stops in libc `+0x5c400` during IR generation,
before the guest reaches `pwd` or `faccessat2`. No native exception
record was persisted. Version 0.9.10 bounds Switch 2 translations
to 64 guest instructions per block and runs dynamic guests on a
4 MiB native worker stack, while the launcher waits. These changes
test compiler resource pressure as a possible cause. Additional Trace
checkpoints separate IR lowering from optimization and ARM64 emission
from publication. First verify `bin/true` on the worker, then run Bash
with **A** and Trace against the same SD files.

On 0.9.10, stock `true` exits 0 on the native worker. Bash passes
faccessat2, prints `Debian Bash builtins passed`, then stops compiling
`bash+0xbcbf0` during block registration before reaching exit_group.
No native fault record was written. Version 0.9.11 avoids allocating
the unused L2 cache on Switch 2, saving 12 MiB per guest thread, and
adds native heap snapshots to Trace. The build and host memory checks
passed. Retest Bash with **A** and Trace. If it stops again, retain the
last native heap snapshot and the end of the run.

The 0.9.11 run regressed. It stopped inside an anonymous 12 KiB
`mmap` during loader startup. The last heap snapshot shows about
8.7 GiB free, which gives no support to heap exhaustion. The
0.9.12 diagnostic build reverses the L2 allocation and heap-sampling
experiment and restores the 0.9.10 allocation layout. It records
separate entry and completion points for the mmap host allocation,
zeroing, and VMA registration. This tests the newly observed stopping
point while retaining the native exception hook and compiler-stage
checkpoints for the original late crash. Treat the crash location as
an observation until a specific failing operation is confirmed.

The 0.9.12 log reaches the return from aligned_alloc for the 12 KiB
mapping, then stops before the post-memset checkpoint. The linked SDK
memset uses DC ZVA for zero fills when DCZID_EL0 advertises a supported
block size. Version 0.9.13 checks the full allocated range with
svcQueryMemory, reports its address and DCZID_EL0, and initializes
mmap allocations using ordinary stores. This isolates an invalid
mapping from a fault specific to the SDK zeroing path. The change is
diagnostic. The build and host VMA test passed.

On 0.9.13, the kernel reports all mmap allocations writable,
including the 12 KiB range that previously stopped. DCZID_EL0 is
`0x4`, and ordinary stores clear that range. Bash then reaches command
parsing and stops in ARM64 emission for `bash+0x39200`. The SDK
zero-fill path remains a candidate, since compiler and allocator code
also calls memset. Version 0.9.14 restores the standard VMA
initialization path and uses a Switch-launcher-only linker wrapper
to run all linked memset calls through ordinary stores. Its host
comparison against standard memset passed across sizes, alignments
and fill bytes, and the ARM64 disassembly confirms it emits stores.
Test with the current SD files, **A**, and Trace. This tests one
shared cause across compilation and guest memory setup.

The 0.9.14 store-only build reproduces the 0.9.13 stopping point
exactly at `JIT begin` for `bash+0x39200`. The linker wrapper did
not resolve this failure and was reverted. Version 0.9.15 traces
emitter setup, scratch-buffer allocation, and each IR operation for
that one block. This should narrow the deterministic failure to the
host buffer/allocator path, an opcode handler, or end-of-block
finalization. Retest with **A** and Trace, then inspect the final
emission checkpoint.

The 0.9.15 run stopped again at the earlier 12 KiB mmap before it
could reach the instrumented Bash block. Version 0.9.16 restores
ordinary stores for guest mmap initialization while keeping the
targeted emitter trace. Settings → Diagnostics → Native zero-fill
comparison runs the SDK memset and ordinary stores independently
of guest execution on the same aligned 12 KiB allocation. The
native exception hook is also enabled when a debugger is present.
Run Bash with **A** and Trace first, then run the native probe
separately. This gives a guest result and a control experiment.

The 0.9.16 native comparison passed all 64 iterations. Bash still
stopped after entering the 12 KiB mmap even with ordinary stores,
so SDK memset alone cannot explain the gap. The next control is to
run the same Bash and NRO in Standard logging, which removes the
flushed syscall and compiler trace writes without changing guest
configuration. There is no need to rebuild or change SD files
for this comparison. If Standard also stops, use a focused mmap
trace that logs allocation return, kernel mapping attributes and
initialization on the guest worker, and compare with the same
allocation sequence on a native worker.

Bash passed on the console under Standard logging with 0.9.16.
This supplies a control with the same guest, build, and JIT settings,
and isolates the reproducible failure to a difference in the Trace
path. Version 0.9.17 removes SD writes from active guest execution.
It captures the latest 2 MiB in a fixed ring buffer, writes from the
launcher after the worker returns, and flushes once per batch. The
native exception hook attempts a best-effort dump. A forced process
termination that bypasses the hook can still lose the in-memory
trace. The temporary memory zeroing and per-op emitter diagnostics
were removed. The worker and the block limit stay the same for this
comparison. Retest stock Bash with **A** and Trace, checking exit 42
and the complete log, then repeat the IPC regression.

Switch 2 verification: Bash passed with Trace on 0.9.17. The buffered
trace removed the earlier crash seen with synchronous SD writes.

| Launcher path | Expected result |
| --- | --- |
| Browse Linux rootfs → `bin/true` | Exit 0 |
| Browse Linux rootfs → `bin/ls` | `-lna /bin` lists `bash`, `ls`, and `true`. Exit 0 |
| Browse Linux rootfs → `bin/bash` | Prints `/bin` and `Debian Bash builtins passed`. Exit 42 |
| Browse ELF files → `glibc-probe.elf` | Prints `glibc + TLS + dlopen = 42`. Exit 42 |
| Browse ELF files → `thread-ipc.elf` | Prints `pthread + Unix sockets + shared mmap = 42`. Exit 42. Repeat to check cleanup |

Also rerun the embedded x86-64 diagnostic and Star Catch. Capture the
version, output, and status here when results arrive. If a test fails,
enable Trace and preserve the latest run from `/fex/fex-switch2-guest.log`.


## Remaining milestone work after the hardware pass

1. Verify scheduling under contention and add guest preemption for
   threads that execute long CPU-bound sections between syscalls.
2. Split per-process state and keep descriptor and mapping ownership
   correct when a second guest image runs. Extend shared-memory backing
   to independent guests so each can map the same bytes.
3. Bring up one guest subprocess path based on glibc `posix_spawn` and
   `waitpid`. Trace the required clone or clone3 variant, `execve`,
   descriptor inheritance, and exit behavior before implementing general
   fork or copy-on-write. Use stock Bash to invoke a separate program as
   a follow-up to the builtin smoke test.
4. Run a glibc-linked graphical parent and a separate helper. The parent
   exchanges a Unix socket message with the helper, displays a response
   that changes with controller input, and observes EOF and the helper's
   exit status. Repeat the test to verify cleanup and rerun the earlier
   fixtures.

Guest programs currently execute in the NRO address space. Linux signal
dispositions are stored for compatibility with startup calls. Signal
delivery, an interactive terminal, and Internet sockets require separate
runtime work. Steam Big Picture remains the long-term target.
