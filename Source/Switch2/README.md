# FEX on Switch 2

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
to select, B to go back, and Plus to exit. On startup the launcher creates
`/switch/fex/games`. The browser defaults to that folder and remembers the
last folder while the launcher is open.

**Settings** lets you select the starting folder and choose standard or
detailed logging. Settings are saved to `/switch/fex/fex-switch2.cfg`. Diagnostics
offers the embedded x86-64 guest and the ARM64 JIT publication probe.

Build the sample with `make -C Source/Switch2 hello-elf`. Copy
`Source/Switch2/build/fex-hello.elf` to `/switch/fex/games`, then choose
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
`/switch/fex/fex-switch2-guest.log` and flushes each line.
If the software closes, inspect the latest section beginning with
`--- FEX Switch 2 launcher 0.4.0 started ---`. A crash before `main()` or
failure to open the SD card can leave the file unchanged.

## SD Maze

Build with `make -C Source/Switch2 maze-elf`. Copy the entire
`Source/Switch2/build/sd-maze/` folder into `/switch/fex/games/` on the SD
card and launch `sd-maze.elf` from the 0.4.0 launcher. Use the D-pad to
move and find the exit marked E. Press B or Plus to leave the game.
Either outcome returns an exit status of 0 to the launcher.

The launcher uses the ELF's parent folder as the guest working directory.
An optional `<executable>.args` sidecar supplies one argument from its
first line. The maze reads `data/maze.txt` relative to its working
directory. The guest runtime supports read-only relative `openat`,
`read`, `close`, private anonymous memory allocation and release, and
controller-backed stdin. Other Linux ABI operations remain outside this
probe's scope.

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
