# Multithreading SDK probe results (Task 0)

Probe: `make mt-probe` (EXPERIMENT, `probes/MultithreadProbe.cpp`), log `sdmc:/fex/mtprobe.log`. Plan:
[MultithreadingPlan.md](MultithreadingPlan.md).

## Run 1 (2026-11-27, mtprobe 1, log `.superpowers/sdd/mtprobe-1.log`)

- **Cores.** Process core mask `0x3f`: 6 cores. Threads created with cpuid -2 (process default core) all run on core 0 and
  never move: 6 such threads take 6x one thread's time. Threads pinned to cores 0..5 scale perfectly (601 ms alone, 602 ms
  for 6). The runtime must place guest threads on cores itself (run 2 checks whether an all-cores mask migrates).
- **Thread cap.** The resource-limit query returns -1 (not readable). 299 extra threads (256 KiB stacks, 16 us per
  create+start) were created, then `0x10801` (kernel: limit reached). Guest threads per app: a few hundred at most.
- **Wake latency between cores** (round trip, 20000 rounds): address arbiter (`svcWaitForAddress`/`svcSignalToAddress`)
  median 0.45 us, p99 1.8 us; uevent 5.7 / 6.8 us; mutex+condvar 9.9 / 13.8 us; busy-wait 0.38 / 1.1 us. Blocked guest
  threads should wait on the address arbiter (futex-like).
- **FEXCore in parallel.** 2..6 guest threads of one context ran correctly at once (results right). `tso_litmus` ran two
  threads of one context on cores 1 and 2 at the same time: real parallel execution works.
- **Stopping a running guest thread from another core** (poll word): 20/20, median 0.48 us to ExecuteThread return, rip
  restored inside the loop.
- **Cross-thread code invalidation:** 20/20. Patching guest code and invalidating (`GetCodeInvalidationMutex`,
  `InvalidateCodeBuffersCodeRange`, `InvalidateThreadCachedCodeRange`) while another core runs it: the other thread picks
  up the new code within 0.38 ms median (0.56 ms max).
- **TSO litmus** (message passing, 20M messages): TSO off sees 0.7-1.2M reorderings (3.4-6 %); TSO on sees 0. TSO on is
  80x slower on this store/load ping-pong (3.3 s vs 40 ms).
- **TSO cost (one thread):** compute and pointer chase unchanged; load+store loop 21 -> 171 ms (8x); vector TSO makes
  movups copies 5.3 -> 41.9 ms (8x); memcpy TSO makes `rep movsb` 8.6 -> 1729 ms (200x).
  Today the runtime runs every process with TSO on (FEX default `TSOEnabled=true`), single-threaded ones included.
- **Per FEX thread:** `CreateThread` 1.2 ms. Memory per thread not measurable this way (heap preallocated).
- **Host stack:** the exec_stack check crashed at its first size, 1 MiB (the launcher uses 4 MiB); the step was not logged.
  Run 2 logs every step on a 3 MiB..768 KiB ladder.

## Run 2 (2026-11-27, mtprobe 2, log `.superpowers/sdd/mtprobe-2.log`)

- **Core placement.** A thread created on the default core with core mask `0x3f` (`svcSetThreadCoreMask` before start,
  with or without an ideal core) starts on an idle core: 6 such threads ran on cores 1,2,3,4,5,0 and took 602 ms (601 ms
  alone). No pinning by the runtime is needed, only the all-cores mask.
- **FEXCore scales:** 1/2/4/6 guest threads of one context on separate cores: 361/363/363/362 ms, all results right.
- **exec_stack crash was a probe bug,** in runs 1 and 2: `mem_rmw` added 2 to the self-pointer `chase` then followed
  (a wild host load). Fixed in mtprobe 3.
- **TSO cost is measured with no host CPU features.** The runtime (and runs 1-2) create the context with empty
  `HostFeatures`: no LSE atomics, no RCPC. FEXCore then emits its slowest TSO form. Run 3 measures TSO with LSE, RCPC and
  LRCPC2 tried one by one.

## Run 3 (2026-11-27, mtprobe 3, log `.superpowers/sdd/mtprobe-3.log`)

- **Host stack:** every routine compiled and ran on 3 MiB, 2 MiB, 1 MiB, 512, 256 and 128 KiB host stacks. These are
  small blocks; the stack for real programs is decided in Task 1 (the launcher's 4 MiB stays until measured).
- **CPU features (no crash, so the A78C has them):** LSE atomics, RCPC (LDAPR) and LRCPC2 (LDAPUR/STLUR, `TSOImm9`; the
  litmus `[rsi+64]` accesses use the immediate form).
- **TSO cost by feature set** (load+store loop, TSO off -> on): none 21 -> 171 ms (8.1x); LSE 21 -> 171 ms (8.1x);
  LSE+RCPC 21 -> 22.9 ms (1.09x); LSE+RCPC+LRCPC2 21 -> 22.9 ms (1.09x). Compute, `rep movsb` and pointer chase are
  unchanged in every set. Vector TSO stays 8x (5.3 -> 42 ms) with every set; FEX's default keeps it off.
- **Litmus** with TSO on: 0 reorderings in every set. Its 3.3 s (vs 40 ms with TSO off) is the cost of two cores
  ping-ponging one cache line with ordered accesses, the worst case, not a per-access cost.
- **Conclusion:** with RCPC the TSO cost on ordinary memory code is about 9 %. The runtime passes no host features today,
  so every process currently pays the 8x form.
