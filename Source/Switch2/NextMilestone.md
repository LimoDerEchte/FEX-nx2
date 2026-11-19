# Switch 2 milestone handoff

Interactive Bash 0.11.5 passed on Switch 2. The user confirmed the
result screen, repeat launches in the same NRO, and consecutive external
commands in one shell. The next milestone is pending selection.

The shortest path to Wayland is guest Unix descriptor passing and
file-backed shared mappings, followed by a native software compositor
and a Debian x86-64 client. A desktop session and Steam remain later
targets. The runtime and terminal limitations below still apply.

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
