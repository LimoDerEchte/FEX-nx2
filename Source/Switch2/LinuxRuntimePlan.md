# Stock glibc on Switch 2

**Hardware verified in FEX 0.8.4.** The Debian 12 amd64 rootfs passed
stock `true` (status 0) and the baseline `glibc` probe (status 42).
See [the active milestone](NextMilestone.md) for the handoff.

Run a stock x86-64 Linux PIE with the distribution's own `ld-linux-x86-64.so.2`
and libc from `/fex/rootfs`. Keep the current static guests and dynamic demo as
regression cases. The acceptance test includes a second program that uses
thread-local storage, allocates memory, opens `libm.so.6` with `dlopen`, resolves
`cos`, prints `glibc + TLS + dlopen = 42`, and exits with 42.

## What the current port can reuse

The existing ELF parser accepted this host's stock `/usr/bin/true`, the
glibc test PIE, and its matching interpreter in a local harness. It found
their entry points and mapped program header tables. The launcher already
maps the interpreter separately and passes `AT_PHDR`, `AT_BASE`, and
`AT_ENTRY`. Keep this path and let the stock guest interpreter resolve
libraries and apply relocations.

FEX's existing Linux frontend provides reference implementations for the
x86-64 `stat` layout, guest code invalidation, and ELF startup. Its syscalls
that call the Linux kernel directly need Horizon implementations here.

## Measured startup

A native run with the local x86-64 glibc 2.44 recorded 30 syscall entries for
`true` and 45 for the TLS and `dlopen` test. The latter printed 42 and
returned 42. These counts depend on the distribution, build, and loader
cache. The reproducible target should pin the binary, interpreter, and
libraries from a single x86-64 rootfs.

| Area | Observed behavior | Implementation target |
| --- | --- | --- |
| Library files | `openat`, `read`, `fstat`, `newfstatat`, `access`, and `close` | Use actual SD file metadata and return the guest x86-64 layout. Add the offset reads needed by file-backed mappings. |
| Address space | Initial file-backed private mapping, fixed file-backed overlays, anonymous zero fill, and subrange `mprotect` | Reserve an owned guest range, split mappings at page boundaries, and load the requested file offsets into that range. |
| Dynamic TLS | `arch_prctl(ARCH_SET_FS)` followed by guest FS-relative memory accesses | Update the FEX guest CPU frame's FS base. Leave the host thread pointer untouched. |
| libc heap and exit | `brk` expands the heap, then the program uses `exit_group` | Track a per-run heap with zeroed growth. Route process exit through the existing controlled return to the launcher. |
| Optional startup facilities | `rseq`, `set_robust_list`, `set_tid_address`, `getrandom`, `prlimit64` | Implement behavior when needed. Verify any fallback with the pinned runtime. |

On the measured host, forcing each optional call in the last row to return
`ENOSYS` individually still let the single-threaded test finish. Forcing
`fstat` or `arch_prctl` to return `ENOSYS` prevented startup. This experiment
did not test concurrent guests or combinations of those fallbacks.

## Runtime selection

The local CachyOS glibc 2.44 was useful for tracing but its interpreter
uses AVX and AVX-512 during early startup. The console trace reached
`ld-linux+0x29e89`, where the first instruction is VEX-encoded `vpxor`
and the following instruction stores a ZMM register with `vmovdqu8`.
Use the Debian 12 amd64 glibc 2.36 packages as the baseline for the next
console test. Keep the interpreter, libc, and libm from the same package.
The test source uses glibc 2.34 symbols. Build its object for baseline
x86-64 and link it with the matching Debian startup objects
(`STOCK_CRT_DIR`) and libc (`STOCK_LIB_DIR`). The CachyOS startup objects
carry an x86-64-v4 ISA requirement even when the C source is compiled
with `-march=x86-64`.

- [Debian 12 amd64 libc6 package](https://packages.debian.org/bookworm/amd64/libc6/download)
- [Debian 12 amd64 coreutils package](https://packages.debian.org/bookworm/amd64/coreutils/download)

## Delivery order

1. Add a per-run guest address-space implementation. Keep guest pointer
   values directly usable by FEX. A fixed mapping request may only replace
   a range already owned by the same guest. Validate rounding, partially
   overlapping ranges, file offsets, zero fill, and cleanup on failure.
   If an operation changes executable pages, invalidate FEX translations
   before those addresses are reused.
2. Share that mapping metadata between the executable-range query and
   syscall pointer validation. Use it for main and interpreter segments,
   DSO mappings, stack, and heap. Keep Linux protection flags distinct
   from the host's JIT publication memory.
3. Implement file metadata, TLS setup, heap management, and exit. Add a
   debug trace for unimplemented or failing guest syscalls, keyed by guest
   RIP and syscall number. Package a matched loader and libraries for local
   staging and compare the console trace to a native baseline.
4. Verify on Switch 2 in order. The copied stock `true` exits with zero.
   The glibc test prints and exits with 42. Launch both repeatedly, then run
   the embedded probe, the custom dynamic demo, and Star Catch again.

## Route to Big Picture

Stock glibc is a foundation for guest processes and threads, IPC, networking,
and a desktop display path. Keep 32-bit guest support as a separate
compatibility checkpoint if the selected Steam client needs it. Treat
Steam sign-in and controller navigation as a client milestone. Proton 11
game launch is a later milestone that also depends on the Linux container
runtime and graphics integration.

- [glibc shared-object segment mapping](https://github.com/bminor/glibc/blob/master/elf/dl-map-segments.h)
- [glibc x86-64 TLS](https://github.com/bminor/glibc/blob/master/sysdeps/x86_64/nptl/tls.h)
- [Valve Steam Linux Runtime and pressure-vessel](https://github.com/ValveSoftware/steam-runtime/blob/master/doc/reporting-steamlinuxruntime-bugs.md)
