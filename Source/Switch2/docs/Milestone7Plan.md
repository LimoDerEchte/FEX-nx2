# Milestone 7 (potential): non-PIE executables (low guest address window)

Status: deferred by the user on 2026-11-25 (after M5 and M6). Draft only; nothing implemented. Discuss the design with
the user before starting.

## Why

Ubuntu's `/usr/bin/python3.12` (and other distro binaries) is a non-PIE x86-64
executable (`ET_EXEC`, linked at `0x400000`). The launcher loads only `ET_DYN`
images (`runtime/GuestElf.cpp`: "Expected a little-endian x86-64
position-independent ELF.", execve returns ENOEXEC). Console log 0.17.0
(`.superpowers/sdd/ui-task7-console-0.17.0.log` line 3039): dash falls back to
running `/usr/bin/py3clean` as a shell script and every python3-* maintainer
script fails during `apt upgrade`.

Guest addresses are host pointers today. The console cannot map anything below
`0x8000000` (`docs/Milestone3Sdk.md`: `svcMapPhysicalMemory` there returns
`0xdc01`; it works only in the alias region, 128 GiB at `0x405f800000`).
A non-PIE image cannot be relocated: its code holds absolute addresses as
imm32/disp32 and its data holds absolute pointers, with no runtime relocations.

## Design

**Low window.** Guest addresses in `[0, 0x8000000)` (128 MiB, `WindowEnd`) are
translated: host = guest + `WindowDelta`. Every other guest address stays a host
pointer (identity), exactly as today. The console never hands out host addresses
below `0x8000000` (unmapped), so the two ranges cannot collide.

- **Backing.** The window is backed by a reserved host range of 128 MiB:
  - console: alias region (`svcMapPhysicalMemory`, 2 MiB granules, mapped on
    demand when the guest maps something in the window; permissions via
    `svcSetMemoryPermission`);
  - native Linux host builds: `mmap(PROT_NONE)` reservation, committed on demand.
  `WindowDelta` = host base of that range. Guest page 0 (null page) stays
  unmapped, so a null dereference still faults.
- **JIT.** In processes that use the window, every guest memory access
  translates its final address: `tst addr, #0xfffffffff8000000; add t, addr,
  delta; csel addr', addr, t, ne` (3 instructions). This covers loads, stores,
  TSO/LRCPC accesses, atomics, vector and SVE accesses, gathers, string ops
  (MemCpy/MemSet), prefetch/cache ops and any other op whose operand is a guest
  address. Instruction fetch in the frontend reads guest code through the
  window too. Processes without a window generate exactly today's code (no
  overhead for PIE-only guests).
- **Runtime.** Every host-side access to guest memory goes through one
  translation helper (`GuestHost(Address)`), including `Available()`, syscall
  argument reads and writes, signal frames, stack/auxv setup, the ELF loader,
  futex words, iovecs, sockaddrs, `/proc/self/mem`. Guest-visible addresses
  (`/proc/self/maps`, auxv, `mmap` results, brk) are always guest addresses.
- **Loader.** `LoadElf` accepts `ET_EXEC` whose segments lie inside the window;
  they are placed at their link addresses. brk starts after the image (in the
  window). `mmap` with `MAP_FIXED` or a hint inside the window, and `MAP_32BIT`,
  allocate in the window. A static non-PIE and a dynamic non-PIE (with
  `/lib64/ld-linux-x86-64.so.2`) both run.
- **Scope out.** 32-bit guests (M5), images linked outside the window (they
  stay ENOEXEC with an explicit log line).

## Tasks

One Sonnet agent per task, sequentially; then one Sonnet review of the whole
milestone; then the orchestrator fixes, writes the suite and runs the console.

### Task 1: JIT and frontend translation (FEXCore)

- Feasibility first: list every Arm64 backend op that dereferences a guest
  address (MemoryOps.cpp, AtomicOps.cpp, VectorOps.cpp, MiscOps.cpp, string
  ops, the dispatcher's own guest reads) and every FEXCore C++ path that reads
  guest memory (frontend decode, `AdjustAddrForSpecialRegion`, signal/x87 or
  thunk helpers). Record the list in this file under "Task 1 inventory".
- Add a per-context window setting (`WindowDelta`, 0 = off) that the JIT reads
  when it compiles a block. When it is non-zero, translate as above. When it is
  zero, emitted code is unchanged (verify by comparing code size of a fixed
  block set before/after).
- Setting the window on a live context flushes its code cache.
- Native test: a host test (FEXCore unit or `check-guest-*` style) that runs a
  tiny x86-64 block through the JIT on the Linux host with a window whose guest
  addresses are not mapped on the host (any missed translation segfaults):
  load, store, atomic add, `rep movsb`, a vector load, an indirect jump through
  a table in the window.

### Task 2: runtime, loader and memory map

- `GuestHost()` helper and a sweep of `runtime/` (GuestRuntime.cpp and the
  headers under memory/, ipc/, process/, fs/): every dereference of a guest
  address goes through it. Grep-based checklist recorded under "Task 2 sweep".
- Window backing (console alias region, Linux reservation), `GuestAddressSpace`
  support for window mappings (MAP_FIXED, hints, MAP_32BIT, munmap, mprotect,
  mremap inside the window), brk in the window, `/proc/self/maps` with guest
  addresses.
- `LoadElf` and the exec path: `ET_EXEC` in the window; the window is enabled on
  the process (Task 1 setting) when its image or interpreter needs it; fork
  keeps it; exec of a PIE image drops it.

### Task 3: tests and acceptance programs

- `tests/guest/NonPieSuite.c` built twice (`-no-pie -static`, `-no-pie`
  dynamic): absolute data pointers, a switch jump table, function pointers,
  `write` from `.rodata`, `read` into `.bss`, brk growth, `mmap(MAP_32BIT)`,
  fork + exec of a non-PIE child, exit 42. Staged like the other suites.
- `tests/guest/NonPieTools.sh` for the RootFS: `python3 -c 'print(42)'`,
  `py3compile` on a file, `python3 -m compileall`, exits 42.

## Acceptance (console)

- NonPieSuite static and dynamic: all checks pass, exit 42.
- In the Shell: `python3 -c 'import sys; print(sys.version)'` prints the
  version; `sudo apt install --reinstall python3-apt` runs its maintainer
  scripts without "Syntax error"; `sudo dpkg --configure -a` finishes.
- Every existing acceptance program still passes (PIE path unchanged).

## Related: isolated memory for fork children (noted 2026-11-27)

Processes made by `fork` share one memory view until `exec` and swap their private pages by copying
(`GuestAddressSpace::SavePrivatePages`/`RestorePrivatePages`) whenever another member of the group runs; only one of them
runs at a time. Multithreading Task 1 measured ~30 ms per swap with 256 MiB private memory (console 0.18.9,
`.superpowers/sdd/console-0.18.9.log`): a parent that polls its forked child stalls both.

The translation this milestone adds is the same mechanism a real fix needs: give a fork child its own per-process
delta (host = guest + delta for every guest address, not only the low window), copy the parent's private pages once into
a disjoint arena range at fork, and both processes run in parallel with no swaps. Same JIT hook (a per-context delta;
for a whole-space delta one `add` per access instead of the window's tst/add/csel), same `GuestHost()` runtime sweep.
Open points: arena space per child (the parent's mapped span must fit again in the 128 GiB alias region), shared
mappings (MAP_SHARED, shared file views) must map one backing at two host ranges, code caches cannot be shared between
deltas, and PIE-only processes keep identity addressing with no cost. Decided by the user on 2026-11-27: M7's translation is
designed as a general per-process delta from the start, covering both non-PIE images and fork children.
