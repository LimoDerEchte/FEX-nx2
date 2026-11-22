# FEX on Switch 2

## Continue a milestone in Codex or Claude Code

Open the other tool from the same repository root and worktree after the
first agent stops. Both use [AGENTS.md](../../AGENTS.md) for shared project
guidance. Claude Code imports it through [CLAUDE.md](../../CLAUDE.md).
The worktree contains the edits and the
[restart checkpoint](NextMilestone.md#restart-checkpoint) tracks what to do
next. Keep that checkpoint current during implementation so it is available
even when a usage limit interrupts the session.

When you're ready to resume, use the same prompt in either tool.

> Resume the Switch 2 milestone from the restart checkpoint in
> Source/Switch2/NextMilestone.md. Check the branch and worktree, preserve
> existing edits, and continue the next open step.

If you switch machines, transfer the in-progress source changes, including
untracked sources, and the handoff together. Rebuild local artifacts under
`Source/Switch2/build/` as needed. Use the recorded diagnostic pair for a
pending console run.

## x86-64 guest probe

Build with Clang, devkitA64, libounce, and kaiHEN. Initialize the dependencies
and configure the Switch 2 build from the repository root.

```sh
git submodule update --init External/fmt External/unordered_dense External/xxhash
cmake -S Source/Switch2 -B Source/Switch2/build/guest \
  -DCMAKE_TOOLCHAIN_FILE=Toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build Source/Switch2/build/guest --target guest_nro --parallel
```

Set `DEVKITPRO` if the devkit is installed outside `/opt/devkitpro`. The
toolchain uses Clang with emulated TLS for FEXCore and the devkitA64 linker
driver with libounce's startup code for the NRO.

## Using the launcher

Copy `Source/Switch2/build/guest/fex-switch2-guest.nro` to the SD card and
launch it from hbmenu under kaiHEN. Use the D-pad or stick to navigate, A
to select, B to go back, and Plus to exit. If only the old `/switch/fex` folder exists, the launcher moves it to
`/fex` on startup. If both folders exist, the launcher uses `/fex`. It creates
`/fex/games`. The browser defaults to that folder and remembers the
last folder while the launcher is open.

**FTP server** serves the guest root filesystem in passive mode while
its screen is open; B stops it. Like OunceShell's ftpd, it takes the
console's address from nifm and listens on that address (Horizon refuses
`INADDR_ANY` listeners) on port 1337, else 2121, else a free port; the
screen shows which.
The rootfs has no user database, so any user name is accepted and the
six-digit password on screen is the only credential. Transfers act with
root's access through the same file syscalls as guest programs, so paths
longer than the SD card can hold (see below) work; new files and folders
are owned by uid 1000, the default owner of files copied to the SD card. It supports RETR, STOR,
APPE, REST, LIST, NLST, MLSD, MLST, SIZE, MDTM, MFMT, MKD, RMD, DELE and
RNFR/RNTO. `make -C Source/Switch2 check-guest-ftp` runs the protocol
test against a host build; `LinuxRuntime/FtpTest.py <address> <port>
<password> 1000` runs the same test against the console and checks that
new entries belong to uid 1000.

**Settings** lets you select the starting folder and choose standard or
detailed logging, or Trace for syscall and JIT compilation checkpoints. Settings are saved to `/fex/fex-switch2.cfg`. Diagnostics
offers the embedded x86-64 guest and the ARM64 JIT publication probe.

Build the sample with `make -C Source/Switch2 hello-elf`. Copy
`Source/Switch2/build/fex-hello.elf` to `/fex/games`, then choose
**Browse ELF files**, select the sample, and press **A** on its launch
screen. A successful run prints `Hello from an x86-64 ELF on Switch 2!`
and reports exit status 42. On the result screen, A retries, B goes
back to the browser, and Y goes home.

The loader accepts a relocation-free x86-64 Linux static PIE (ET_DYN with
no interpreter, dynamic metadata, or TLS). The sample uses RIP-relative
addressing and makes raw `write` and `exit` syscalls. Guest writes go to
the console and can read buffers within readable ELF segments. This
probe supplies a minimal initial stack and a limited syscall interface.

The launcher appends checkpoints and FEXCore messages to
`/fex/fex-switch2-guest.log`. During a guest run it keeps the latest 2 MiB
in memory and writes it in batches when the guest returns. The native
exception handler also attempts to save this buffer if Horizon calls it.
If the software closes, inspect the latest section beginning with
`--- FEX Switch 2 launcher 0.12.32 started ---`. A crash before `main()` or
failure to open the SD card can leave the file unchanged.

## SD Maze

Build with `make -C Source/Switch2 maze-elf`. Copy the entire
`Source/Switch2/build/sd-maze/` folder into `/fex/games/` on the SD
card and launch `sd-maze.elf` from the 0.4.0 launcher. Use the D-pad to
move and find the exit marked E. Press B or Plus to leave the game.
Either outcome returns an exit status of 0 to the launcher.

The launcher uses the ELF's parent folder as the guest working directory.
An optional `<executable>.args` sidecar supplies one argument per line. The maze reads `data/maze.txt` relative to its working
directory. The guest runtime supports read-only relative `openat`,
`read`, `close`, private anonymous memory allocation and release, and
controller-backed stdin. Other Linux ABI operations remain outside this
probe's scope.

## Texture demo

Build with `make -C Source/Switch2 texture-demo`. Copy the entire
`Source/Switch2/build/texture-demo/` folder into `/fex/games/` and
launch `texture-demo.elf` from the 0.5.0 launcher. A colored diamond should
appear over a full-screen checkerboard. Move it with the D-pad or stick,
then press B or Plus to return to the launcher with exit status 0.

The x86-64 guest opens and reads `data/sprite.rgba` using its guest
working directory, checks that it contains exactly 32 × 32 RGBA8 pixels,
and draws it into a 320 × 180 frame. The Switch 2 launcher presents the
frame at 1280 × 720 through libounce. The graphics interface is a
fork-specific frame presentation syscall. A Linux graphics API bridge
will be a separate milestone.

## Star Catch and SDL2

Build with `make -C Source/Switch2 star-catch`. The build uses installed
SDL2 development headers. Copy the `build/star-catch/` folder into
`/fex/games/` and launch its ELF from the 0.6.0 launcher.

Move left or right to catch five falling stars. Three missed stars end the
round. Press B or Plus to return to the launcher. The blue paddle, score
markers, and lives are drawn by the game. It reads its star texture from
`data/star.rgba`.

The x86-64 guest links a small SDL2 source-compatibility library for the
APIs used by this game. It supports one 320 × 180 window, software rectangle
drawing and RGBA texture uploads and copies, directional key events and held
state, a quit event, millisecond timing, and read-only SDL file streams.
Existing dynamically linked SDL2 games, audio, and accelerated renderers
require additional runtime work.

## Linux rootfs and dynamic ELF test

The SD card layout is `/fex/games`, `/fex/rootfs`, and the launcher log and
config in `/fex`. The launcher creates the two top-level data directories.
Absolute guest paths resolve under the rootfs. Launches from within the rootfs
start in Linux `/`. Launches from the games directory start beside the ELF. Read-only `/proc/version`, `/proc/self/status`,
and `/dev/null` are supplied by the launcher.

Build with `make -C Source/Switch2 dynamic-demo`. Copy the contents of
`build/dynamic-demo/` into `/fex/`, preserving `games/` and `rootfs/`.
Launch `dynamic-demo.elf` with the 0.7.0 launcher. The ELF's `PT_INTERP`
selects `/lib/ld-fex-demo-x86-64.so.2` in the rootfs. That interpreter
reads the `DT_NEEDED` library, resolves the demo's PLT relocation, and
hands control to the program. The dynamically linked function prints a
guest kernel version and a hello message, then the program exits with 42.

This rootfs contains a test interpreter and library. The interpreter
supports the demo's single dependency and x86-64 PLT relocations.
The launcher accepts position-independent x86-64 program images and
supplies the Linux auxiliary vector. Distribution loaders and general
Linux applications still need file-backed mappings, broader syscall and
TLS coverage, and other runtime work.

## Debian 12 runtime smoke tests

Stage the official Debian 12 amd64 `bash`, `coreutils`, `libc6`,
`libc6-dev`, `libtinfo6`, `libselinux1`, `libpcre2-8-0`, and `ncurses-base` packages by
extracting their data into a common development directory. From the
repository root, run:

```sh
make -C Source/Switch2 debian-runtime DEBIAN_ROOT=/path/to/unpacked/debian
```

Copy `Source/Switch2/build/debian-runtime/fex/` to the SD root. On the
launcher, choose **Browse Linux rootfs** → **bin** and run `true`, `ls`,
and `bash`. The staged sidecars pass `-lna /bin` to ls and a builtin-only
command to Bash. Expect exit codes 0, 0, and 42. Under **Browse ELF files**
run `glibc-probe.elf` and `thread-ipc.elf`. Both should print a success
message and exit with status 42. Each executable uses the same
`/fex/rootfs` tree and its ELF interpreter. The staging script follows
package symlinks when copying libraries to the SD card.

See [the active milestone](NextMilestone.md) for hardware results and the
next runtime work. [The glibc bring-up notes](LinuxRuntimePlan.md) record
the prior 0.8.4 tests.

## Interactive Bash terminal

Choose **Bash shell**, the third item on the home menu, to open the
stock Debian Bash in interactive mode. Connect a physical USB keyboard
and use the US layout. Arrow keys, Backspace, Enter, and common control
keys are translated for readline. Press both stick buttons together to
open Horizon's keyboard. **Send** submits one line. **Plus** closes
terminal input, and `exit` returns to the launcher with a guest exit status.

The shell uses the rootfs's `/lib/terminfo/a/ansi` entry and libounce's
ANSI console colors. Try `printf '\033[31mred\033[0m\n'`. Session
history lives in memory. The 0.11.5 build adds the foreground fork, exec, and wait path used by
Bash for external commands. Test with `ls -la /bin` and `exit 42`. The
new `process-probe.elf` checks private/shared fork mappings, inherited
pipes, close-on-exec, and exit status. General signal delivery and job
control remain open.

## Filesystem compatibility suite

`LinuxRuntime/FsSuite.c` checks common behavior and edge cases for
descriptors and I/O, paths and links, metadata and permissions, allocation
and transfer, and extended attributes. Each test runs in its own scratch
directory and prints a TAP line; failures name the source line, result and
errno. The suite exits 42 when nothing fails.

```sh
make -C Source/Switch2 debian-sysroot        # Debian 12 packages into build/debian-sysroot
make -C Source/Switch2 check-fs-suite-native # host kernel reference, as you and as namespace root
make -C Source/Switch2 filesystem-runtime   # stages the SD bundle in build/guest/sd
Source/Switch2/LinuxRuntime/RunPackagedNative.sh Source/Switch2/build/guest/sd/fex/rootfs [--root] /bin/fs-suite.elf
```

Pass name prefixes to select tests (`io/`, `path/rename`) or `!prefix` to
exclude them, and `-l` to list them. The bundle stages `/bin/fs-suite.elf`
(default user) and `/bin/fs-suite-root.elf`, which runs as root through
the launcher's credential file so cross-identity cases execute.
`/bin/filesystem-acceptance.elf` runs the ABI probe, coreutils and the
suite together.

A `<program>.user` file next to an ELF holds `uid gid` for its launch,
like starting the program from a root shell. Without it, guests run as
uid 1000. An empty `<program>.recovery-test` file lets that launch
interrupt the recovery probe's journaled move into `/.fex-recovery-test`,
so the next launch must roll it back or forward; the bundle stages it for
the two `filesystem-recover-*-write.elf` probes.

Guest inode metadata (ownership, modes, links, xattrs) and its recovery
journal live in `/fex/.inode/`. Horizon limits a whole SD path to 257
bytes, so entries whose path is longer, and everything below them, are
kept there as well: directories as overflow records and files as objects
in `/fex/.inode/objects`. They do not appear under `/fex/rootfs` on the
SD card; use the FTP server to copy them. Launchers before 0.12.33 kept them as
`/fex/.inode-*`; the launcher moves those on startup and refuses to run
guests if an entry exists in both places.

## Graphics (shelved)

The Wayland compositor bridge and Vulkan probes are shelved on branch
`shelf/graphics` until kernel syscall coverage is complete. Resume them
there using FEX's native Vulkan layer.

## JIT diagnostics

Build the standalone ARM64 emitter probe with `make -C Source/Switch2`.
The output is `Source/Switch2/build/fex-switch2-jit.nro`. Press A to run it.
It publishes a function returning 7 and then updates just the entry range
to return 42. The original full-buffer probe passed on hardware. The standalone
range-publication variant has not been run independently.

## Port milestones

1. Hardware verified. The embedded x86-64 probe passed, and version 0.2.0
   loaded the SD-card ELF, handled its console write, and reported exit
   status 42.
2. Expand the loader, memory management, and syscall layer for a selected
   offline Linux game. Validate graphics, input, and audio independently
   before testing gameplay.
3. Evaluate Steam once the game path works. It needs 32-bit and 64-bit guest
   support and a wider Linux ABI, including threading, networking, IPC, and
   process management. Windows games also need a working Wine or Proton stack.

## Desktop and Windows guests

The launcher can remain the entry point for single Linux games and a future
desktop session. Wine's virtual desktop is a candidate for running Windows
applications in one managed display surface. It would require a working Wine
userspace, Windows executable launch profiles, graphics presentation, input,
audio, and the Linux ABI that Wine expects. The launcher UI does not provide
those runtime capabilities yet.
