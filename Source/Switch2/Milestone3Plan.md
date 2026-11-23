# Milestone 3 — Memory and Communication Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement Linux virtual memory, futex, pipe, System V and POSIX IPC, Unix socket and TCP/UDP socket semantics in the Switch 2 guest runtime. Debian threaded tools and real Unix, TCP and UDP client/server pairs must work through the normal launcher.

**Architecture:**
- One host thread runs every guest thread cooperatively (`GuestSyscalls::RunThreads` in `GuestRuntime.cpp`). Guest addresses are host pointers. Processes made by `fork` share host addresses and swap private pages at process switches (`GuestAddressSpace::SavePrivatePages`).
- Milestone 3 replaces `aligned_alloc` guest memory with an arena. The arena reserves address space in the Horizon alias region and commits physical memory on demand with `svcMapPhysicalMemory`.
- Futex moves to a keyed wait table that works across processes.
- System V IPC, POSIX message queues and a `/dev/shm` tmpfs are added. AF_UNIX gains datagram and seqpacket sockets plus filesystem socket nodes.
- AF_INET TCP/UDP is backed by nonblocking host BSD sockets. The scheduler polls them.
- Pure logic lives in small headers (`Guest*.h`) with host unit tests. Syscall glue stays in `GuestRuntime.cpp`.

**Tech Stack:** C++20, libounce (libnx fork) at `/opt/devkitpro/libounce`, FEXCore JIT, Debian 12 x86-64 guest userspace, C test programs linked against Debian glibc, GNU make, CMake.

**Inputs:**
- [Milestone3Sdk.md](Milestone3Sdk.md) holds the SDK risks and console probe results (m3probe 1–5). Every decision below that cites "probe" refers to it.
- [Milestone2Plan.md](Milestone2Plan.md) is the format reference.
- [ProcessInventory.md](ProcessInventory.md) is the inventory format reference.

## Global Constraints

- **Scope.** Switch 2 code lives in `Source/Switch2`. The Linux and Windows builds keep working. Change FEXCore only if a task says so, guarded by `__SWITCH2__`.
- **No generated binaries.** Never commit generated binaries.
- **Builds.**
  - Launcher: `cmake --build Source/Switch2/build/guest --target guest_nro`.
  - Probes and host tests: `make -C Source/Switch2 <target>`.
  - Use one launcher tree, `Source/Switch2/build/guest`, with the SD bundle in `Source/Switch2/build/guest/sd`.
  - Ship one NRO per console run. Per-launch test behavior goes in sidecar files, never build options.
- **Real semantics.** Implement Linux behavior through the normal entry point. No faked results.
  - Anything incomplete goes in `CommunicationInventory.md` under "Deliberate differences", with the exact errno returned.
  - Error order and errno follow host Linux 7.3, the native reference on this machine.
- **Linux errno values used in this plan:**
  - EPERM 1, ENOENT 2, ESRCH 3, EINTR 4, EIO 5, E2BIG 7, EBADF 9, EAGAIN 11, ENOMEM 12, EACCES 13, EFAULT 14, EBUSY 16, EEXIST 17, ENODEV 19, EINVAL 22, ENFILE 23, EMFILE 24, EFBIG 27, ENOSPC 28, EPIPE 32, ERANGE 34, EDEADLK 35, ENAMETOOLONG 36, ENOSYS 38.
  - ENOMSG 42, EIDRM 43, ENOTSOCK 88, EDESTADDRREQ 89, EMSGSIZE 90, EPROTOTYPE 91, ENOPROTOOPT 92, EPROTONOSUPPORT 93, ESOCKTNOSUPPORT 94, EOPNOTSUPP 95, EAFNOSUPPORT 97, EADDRINUSE 98, EADDRNOTAVAIL 99.
  - ENETUNREACH 101, ECONNABORTED 103, ECONNRESET 104, ENOBUFS 105, EISCONN 106, ENOTCONN 107, ETIMEDOUT 110, ECONNREFUSED 111, EHOSTUNREACH 113, EALREADY 114, EINPROGRESS 115, EOWNERDEAD 130.
- **No CPU faults, ever.** m3probe 5 showed that a CPU fault in any host thread ends the app; user exception handlers never run.
  - Never set host page permission `Perm_None` or `Perm_R` on guest memory.
  - Never touch uncommitted arena memory from runtime code. Check `Available()` / `IsCommitted()` first.
  - Guest page protection lives only in the runtime's tables.
- **No double mapping.** The SDK cannot map one physical page at two addresses (SVCs 0x50, 0x51, 0x4b and 0x74 are not permitted). Every shared object has exactly one address, and a second mapping of the same object in one process fails with the errno given in its task.
- **Socket ABI.**
  - The host BSD socket ABI is FreeBSD-shaped: `SOL_SOCKET` 0xffff, `AF_INET6` 28, `MSG_DONTWAIT` 0x80, `sin_len`, newlib errno.
  - Never pass guest constants, addresses or errno through. Translate in `GuestInet.h`.
  - Read host errors from `g_bsdErrno` (Linux numbering, probe), not `errno`.
- **No commits until acceptance.** Do **not** commit during implementation or review. The milestone is committed once on `nx2` after console acceptance (AGENTS.md). Keep the work in the working tree; the orchestrator tracks progress in `.superpowers/sdd/progress.md`.
- **Restart checkpoint.** Keep `Source/Switch2/NextMilestone.md#restart-checkpoint` current after each work package: status, next step, blockers, exact commands with results.
- **Launcher version.** The first milestone 3 console build is `0.14.0`. Bump it in both `GuestProbe.cpp` (`WriteLog("--- FEX Switch 2 launcher X started ---")`) and `CMakeLists.txt` (nacptool line), only when preparing a console build.
- **Graphics.** Graphics stays on `shelf/graphics`.

## Execution model (token budget)

The user's direction is that Opus orchestrates. Implementation uses **at most 5 Sonnet subagents**, one per work package (WP). Review uses **at most 5 Sonnet subagents**, one per WP. Opus then fixes review findings, writes the guest suite, and debugs.

| Phase | Who | Content |
| --- | --- | --- |
| A. Implementation | Sonnet, 5 sequential subagents | WP1 → WP2 → WP3 → WP4 → WP5. All edit `GuestRuntime.cpp`, so they run **sequentially**, never in parallel. |
| B. Review | Sonnet, ≤5 subagents (parallel allowed: read-only) | One reviewer per WP with the checklist in [Phase B](#phase-b--review). Findings go to `.superpowers/sdd/m3-review-wpN.md`. |
| C. Final pass | Opus (main session) | Fix review findings, write `CommSuite.c` + `CommTools.sh` + acceptance wrapper, native reference run, console build and checklist, debugging. |

**Dispatch brief for each WP subagent:**
- Include this file's Global Constraints, the WP section verbatim, and the "Interfaces" blocks of earlier WPs.
- Tell it to read the files named in "Files" before editing.
- Tell it to report:
  1. the diff summary;
  2. the host-check command output;
  3. the `guest_nro` build result;
  4. every deviation from the plan, with its reason;
  5. inventory rows changed.

**Done-when for every WP:**
- `make -C Source/Switch2 <the WP's check targets> check-guest-memory` passes.
- `cmake --build Source/Switch2/build/guest --target guest_nro` builds without new warnings.
- The WP's inventory rows are updated.
- No console run is required inside a WP.

## Phases

1. **Phase A (WP1–WP5):** implementation with host unit tests for every new header.
2. **Phase B:** review.
3. **Phase C (Tasks C1–C5):** suite, tools acceptance, native reference, console build and checklist, inventory reconciliation.

## File Structure

| File | Responsibility | WP |
| --- | --- | --- |
| `Source/Switch2/CommunicationInventory.md` | Milestone 3 syscall inventory (all entries of the three groups), routing, deliberate differences, console evidence | 1 (create), 2–5 (rows) |
| `Source/Switch2/GuestArena.h` | Address-space reservation and commit policy over a backend interface (Horizon alias region on console, `mmap` in host tests) | 1 |
| `Source/Switch2/HostArena.cpp` | Horizon backend: `svcMapPhysicalMemory` / `svcUnmapPhysicalMemory` in the alias region, `virtmem` reservation | 1 |
| `Source/Switch2/GuestAddressSpace.h` | Uses `GuestArena`; commit tracking; `mremap`, `mseal`, `MADV_DONTFORK/WIPEONFORK` flags; cross-process byte access | 1 |
| `Source/Switch2/GuestMemoryFile.h` | memfd/tmpfs storage on an arena reservation instead of a fixed 32 MiB `aligned_alloc` | 1 |
| `Source/Switch2/GuestFutex.h` | Futex keys, WAKE_OP math, PI word transitions, robust-list walk, waitv parsing | 2 |
| `Source/Switch2/GuestSysvIpc.h` | SysV shm/sem/msg tables, permissions, limits, struct layouts | 3 |
| `Source/Switch2/GuestMqueue.h` | POSIX message queues | 3 |
| `Source/Switch2/GuestSocket.h` | Extend: datagram/seqpacket message boundaries, sender address, credentials, peer-close reset | 4 |
| `Source/Switch2/GuestInet.h` | Linux↔BSD translation: families, types, flags, sockaddr, socket options, cmsg, errno | 5 |
| `Source/Switch2/LinuxRuntime/Guest*Test.cpp` | Host unit test per header (`GuestArenaTest`, `GuestFutexTest`, `GuestSysvIpcTest`, `GuestMqueueTest`, `GuestSocketTest`, `GuestInetTest`) | per WP |
| `Source/Switch2/GuestRuntime.cpp` | Syscall glue, scheduler waits, host socket polling | 1–5 |
| `Source/Switch2/GuestProbe.cpp` | Network bring-up for guests (lazy `socketInitialize`, nifm DNS) | 5 |
| `Source/Switch2/Makefile` | `check-guest-*` targets; Phase C suite targets | every WP, C |
| `Source/Switch2/LinuxRuntime/CommSuite.c`, `CommTools.sh`, `CommAcceptance.c` | Phase C guest suite, Debian tool script, wrapper | C |
| `Source/Switch2/LinuxRuntime/FetchDebian.py`, `StageDebian.py` | Stage new packages and suite programs | C |

## Host unit test pattern (every header)

Copy the existing rule shape exactly, for example `check-guest-signals` in `Source/Switch2/Makefile`:

```make
.PHONY: check-guest-futex
check-guest-futex: $(BUILD)/guest-futex-test
	$<

$(BUILD)/guest-futex-test: LinuxRuntime/GuestFutexTest.cpp GuestFutex.h | $(BUILD)
	$(HOST_CXX) -std=c++20 -Wall -Wextra -Werror $< -o $@
```

Tests are plain `main()` programs with a `CHECK(expr)` macro that prints `FAIL <file>:<line> <expr>` and exits 1, as in `LinuxRuntime/GuestSignalsTest.cpp`. Headers must compile on the host: no libnx includes in headers. Put Horizon calls behind a backend interface or in `.cpp` files.

## Runtime conventions every WP must follow

These are read from `GuestRuntime.cpp`; reuse them, do not invent new ones.

- **Syscall dispatch.**
  - Syscalls dispatch in `switch (Registers[REG_RAX])` in `GuestSyscalls::HandleSyscall` (around line 476).
  - Arguments: `A`=rdi, `B`=rsi, `C`=rdx, `Registers[REG_R10]`, `Registers[REG_R8]`, `Registers[REG_R9]`.
  - Return negative Linux errno.
- **Constants and pointer checks.**
  - Constants: `BadFile` (-9), `Fault` (-14), `Invalid` (-22), `LinuxEnosys` (-38).
  - `Available(Address, PF_R|PF_W)` returns the readable/writable byte count from `Address`. Use it before every guest pointer access.
  - `CopyOut(Address, Value)` writes to guest memory.
- **Blocking.**
  - Set `Current->Waiting = WaitKind::X`, plus `Current->Deadline` (monotonic ns) if timed. Then return `RetrySyscall` (the call re-runs), or wake by setting the thread's RAX and `Waiting = WaitKind::None` (as `WakeFutex` does).
  - `WaitForIO()` waits for `IOEpoch` to change. Bump `++IOEpoch` whenever readiness may have changed.
  - `Ready(GuestThread&)` decides wakeups.
  - The idle path in `RunThreads` (around line 400) sleeps up to 8 ms. Its "blocked with no remaining producer" check must learn about every new wait kind that an external event can end.
- **Signals and restart.**
  - Interrupting waits: `InterruptWait` and `RestartableSyscall` decide EINTR vs restart. Add every new blocking syscall to `RestartableSyscall` with Linux's rule (comment the rule).
  - SIGPIPE: `HandleSyscall` sends SIGPIPE for `-32` unless suppressed (`SuppressPipeSignal`, around line 730). Extend the suppression for `sendto`/`sendmsg`/`sendmmsg` with `MSG_NOSIGNAL` (0x4000).
- **Processes and descriptors.**
  - `Task` is the current `GuestProcess`, `Current` the current `GuestThread`, `Processes` the map by PID.
  - `ActivateProcess` switches processes.
  - `Descriptors().Files[FD]` holds `std::shared_ptr<OpenFile>`. Use `FreeDescriptor()` and `InstallSocket(...)`.
  - The `OpenFile` destructor is the place for last-close cleanup.
- **Memory.** `GuestPageSize` is 4096. `PageFlags(Protection)` converts PROT bits to `PF_R/PF_W/PF_X`.

## Phase A — Implementation

### WP1: Memory — arena, mapping calls and limits [Sonnet subagent 1]

**Why:**
- Guest memory today is `aligned_alloc`, capped at `MaxTotal` 96 MiB and `MaxMapping` 32 MiB. Every byte is committed at map time; the brk heap is a fixed 8 MiB (`HeapLimit`); a memfd reserves 32 MiB.
- The console has about 9.1 GiB, and `svcMapPhysicalMemory` works in the 128 GiB alias region (probe).
- Linux programs reserve large `PROT_NONE` ranges and commit parts later (glibc arenas, thread stacks with guard pages, JIT and GC heaps).

**Files:**
- Create: `Source/Switch2/GuestArena.h`, `Source/Switch2/HostArena.cpp`, `Source/Switch2/LinuxRuntime/GuestArenaTest.cpp`, `Source/Switch2/CommunicationInventory.md`
- Modify: `Source/Switch2/GuestAddressSpace.h`, `Source/Switch2/GuestMemoryFile.h`, `Source/Switch2/GuestRuntime.cpp` (`MapGuest` around 10050, `ProtectGuest`, `UnmapGuest`, `SetHeapEnd`, `AdviseGuest` around 8467, `ActivateProcess` around 1600, the dispatch switch), `Source/Switch2/CMakeLists.txt` (add `HostArena.cpp` to the guest NRO sources), `Source/Switch2/Makefile`, `Source/Switch2/LinuxRuntime/AddressSpaceTest.cpp`, `Source/Switch2/LinuxRuntime/GuestMemoryFileTest.cpp`

**Interfaces (produced):**

```cpp
// GuestArena.h
namespace Switch2Guest {
struct ArenaBackend {                       // Horizon: HostArena.cpp; host tests: mmap
  virtual ~ArenaBackend() = default;
  virtual bool Commit(uint64_t Address, size_t Size) = 0;   // map zeroed memory, RW
  virtual bool Decommit(uint64_t Address, size_t Size) = 0; // return memory
};
class GuestArena {
public:
  static constexpr size_t Granule = 2 * 1024 * 1024; // commit unit (probe verified 2 MiB)
  GuestArena(ArenaBackend& Backend, uint64_t Base, size_t Size, size_t CommitLimit);
  uint64_t Reserve(size_t Size, size_t Alignment = 4096);   // 0 on failure; no commit
  bool Release(uint64_t Address, size_t Size);              // decommits and frees VA
  bool Commit(uint64_t Address, size_t Size);               // page range; commits covering granules
  void Decommit(uint64_t Address, size_t Size);             // page range; drops granules with no committed page
  bool IsCommitted(uint64_t Address, size_t Size) const;
  size_t Committed() const;                                 // bytes in committed granules
  size_t CommitLimit() const;
};
std::unique_ptr<ArenaBackend> MakeHostArenaBackend(uint64_t& Base, size_t& Size); // HostArena.cpp
}
```

`GuestAddressSpace` gains `bool IsCommitted(uint64_t, size_t) const`, `bool Remap(...)` (see Step 6) and `size_t CopyFromProcess(uint64_t Address, void* Out, size_t Size, bool Resident) const`. It also gains a matching `CopyToProcess`, which reads or writes the live bytes when `Resident` and the saved copy otherwise. The futex and process_vm code in later WPs use these.

- [ ] **Step 1: Create the inventory.**
  - Write `CommunicationInventory.md` in the format of `ProcessInventory.md`: a header paragraph, a "Console verification still required" table, a "Deliberate differences" table, and a table per group with columns `Nr | Name | Dispatch | Notes`.
  - Rows (x86-64 numbers):
    - **Virtual memory:** 9 mmap, 10 mprotect, 11 munmap, 12 brk, 25 mremap, 26 msync, 27 mincore, 28 madvise, 149 mlock, 150 munlock, 151 mlockall, 152 munlockall, 216 remap_file_pages, 237 mbind, 238 set_mempolicy, 239 get_mempolicy, 256 migrate_pages, 279 move_pages, 310 process_vm_readv, 311 process_vm_writev, 319 memfd_create, 323 userfaultfd, 324 membarrier, 325 mlock2, 329 pkey_mprotect, 330 pkey_alloc, 331 pkey_free, 440 process_madvise, 447 memfd_secret, 448 process_mrelease, 450 set_mempolicy_home_node, 451 cachestat, 453 map_shadow_stack, 462 mseal.
    - **Pipes and IPC:** 22 pipe, 293 pipe2, 202 futex, 273 set_robust_list, 274 get_robust_list, 449 futex_waitv, 454 futex_wake, 455 futex_wait, 456 futex_requeue, 29 shmget, 30 shmat, 31 shmctl, 67 shmdt, 64 semget, 65 semop, 66 semctl, 220 semtimedop, 68 msgget, 69 msgsnd, 70 msgrcv, 71 msgctl, 240 mq_open, 241 mq_unlink, 242 mq_timedsend, 243 mq_timedreceive, 244 mq_notify, 245 mq_getsetattr.
    - **Sockets:** 41 socket, 42 connect, 43 accept, 288 accept4, 44 sendto, 45 recvfrom, 46 sendmsg, 47 recvmsg, 307 sendmmsg, 299 recvmmsg, 48 shutdown, 49 bind, 50 listen, 51 getsockname, 52 getpeername, 53 socketpair, 54 setsockopt, 55 getsockopt.
  - Set Dispatch from the current switch: routed today are 9 10 11 12 28 319 22 293 202 41 42 43 288 44 45 46 47 48 49 50 53. Everything else is Unrouted.
- [ ] **Step 2: Write `GuestArenaTest.cpp` (failing).** Use a fake backend that records Commit/Decommit calls over a host `mmap(PROT_NONE)` region, with `mprotect` RW on commit. Cases:
  1. Reserve returns page-aligned, non-overlapping ranges inside `[Base, Base+Size)`.
  2. Commit of one page commits exactly its 2 MiB granule. A second page in the same granule does not call the backend again.
  3. Decommit of one page keeps the granule while another committed page shares it, and drops it when the last page goes.
  4. Commit beyond `CommitLimit` fails and changes nothing.
  5. Release decommits and allows reuse of the VA.
  6. `IsCommitted` is false for reserved-only pages.
  7. Reserve with 2 MiB alignment.
  Add a `check-guest-arena` target. Run it and see it fail to compile.
- [ ] **Step 3: Implement `GuestArena.h`.**
  - Track VA with a sorted free list (first-fit).
  - Track commit with a per-page bitmap and a per-granule committed-page count.
  - `Commit` zero-fills **only the newly committed pages** (`std::memset`). Do not assume the kernel zero-fills.
  - Run `check-guest-arena` until it passes.
- [ ] **Step 4: Implement `HostArena.cpp`.**
  - Base and size come from `svcGetInfo(InfoType_AliasRegionAddress/Size)`. Reserve the whole alias region except the first 1 GiB, with `virtmemAddReservation`.
  - `Commit` calls `svcMapPhysicalMemory(Address, Size)` on 2 MiB-aligned granules. `Decommit` calls `svcUnmapPhysicalMemory`. Log a failure with the Result.
  - `CommitLimit` = `TotalMemorySize - UsedMemorySize` at startup minus a 1 GiB reserve for FEXCore, the JIT and libnx, capped at 6 GiB.
  - Add the file to the NRO sources in `CMakeLists.txt`.
- [ ] **Step 5: Move `GuestAddressSpace` onto the arena.**
  - `Allocate` reserves and commits only if `Flags != 0` (a `PROT_NONE` mapping stays uncommitted). `Region::Storage` keeps a deleter that calls `Arena.Release`.
  - `Set(...)` that adds R or W to an uncommitted page commits it first. If the commit fails, the call returns false and the caller returns ENOMEM (-12).
  - `MaxMapping` becomes the arena commit limit, and `MaxTotal` is removed. Keep `MaxMapping` as a function `MaxMapping()`, since callers use it as a size cap.
  - `SavePrivatePages` / `RestorePrivatePages` skip pages that are not committed. Reading them would fault and end the app.
  - `RetainPage` and every `memcpy` from guest memory in the runtime must only touch committed pages. `Available()` must report 0 for uncommitted pages even if the guest flags say readable; assert this in a test.
  - Update `AddressSpaceTest.cpp` to construct with the fake backend.
- [ ] **Step 6: `brk`.**
  - Reserve `HeapLimit` = 1 GiB of VA at first `brk` and commit as the break grows; decommit whole granules above the new break when it shrinks.
  - Linux checks `RLIMIT_DATA`: keep returning the old break on failure (Linux brk semantics).
- [ ] **Step 7: memfd and tmpfs storage.**
  - `GuestMemoryFile::Reserve` reserves `MaxFileSize` = 1 GiB of arena VA on first use and commits pages as `Present` grows.
  - Punch and truncate decommit fully free granules.
  - Update `GuestMemoryFileTest.cpp`.
- [ ] **Step 8: `mmap` flags (case 9, `MapGuest`).** Accept and implement:
  - `MAP_FIXED_NOREPLACE` 0x100000 → EEXIST (-17) if any page in range is mapped.
  - `MAP_POPULATE` 0x8000 and `MAP_NONBLOCK` 0x10000 → commit at map time (already true).
  - `MAP_LOCKED` 0x2000 → count toward mlock accounting (Step 12).
  - `MAP_GROWSDOWN` 0x100 → accepted, no growth (deliberate difference).
  - `MAP_HUGETLB` 0x40000 → ENOMEM (no hugetlb pool, as Linux with `nr_hugepages=0`).
  - `MAP_SHARED_VALIDATE` 3 → like MAP_SHARED, but unknown flags → EOPNOTSUPP (-95). `MAP_SYNC` 0x80000 with it → EOPNOTSUPP.
  - `MAP_UNINITIALIZED` 0x4000000 → ignored.
  - `MAP_32BIT` 0x40 → ENOMEM. Arena addresses are above 4 GiB; deliberate difference.
  - Non-fixed hints are ignored (Linux permits). A size larger than the arena limit → ENOMEM.
- [ ] **Step 9: Writable file-backed `MAP_SHARED` on SD files.**
  - Today it returns ENODEV (-19) when the descriptor is writable.
  - Implement a "shared file view" with a clean snapshot. Map: fill the view from the file, keep a private copy of the bytes as written to disk, and register the view in `SharedFileViews` with `MayWrite`.
  - Writeback compares view vs snapshot per page and writes changed pages with the existing SD write path (`SdRetry`), then updates the snapshot.
  - Writeback runs on `msync(MS_SYNC|MS_ASYNC)`, `munmap`, `fsync`/`fdatasync` of that inode, process exit and exec.
  - `read`/`write` on the inode keep using `RefreshSharedFileViews` (around line 2529). Write back first, so a `write` after a store does not lose the store.
  - Only one view per (inode, offset range) per process. A second overlapping `MAP_SHARED` of the same file in the same process → ENOMEM (deliberate; single-view rule).
- [ ] **Step 10: `mremap` (case 25).**
  - Flags: `MREMAP_MAYMOVE` 1, `MREMAP_FIXED` 2, `MREMAP_DONTUNMAP` 4.
  - Error order as Linux:
    1. Unaligned old address, or flags other than these → EINVAL.
    2. FIXED without MAYMOVE → EINVAL.
    3. DONTUNMAP without MAYMOVE, or with `old_size != new_size` → EINVAL.
    4. New size 0 → EINVAL.
    5. Old range not one mapping → EFAULT.
  - Shrink: unmap the tail.
  - Grow: grow in place if the following pages are free in the same reservation. Otherwise MAYMOVE → reserve new, copy committed bytes, unmap old (keep old with DONTUNMAP, now zero-filled anonymous). Without MAYMOVE → ENOMEM.
  - Shared mappings (anonymous shared, memfd, SysV shm) cannot move (single view) → ENOMEM when growth needs a move (deliberate).
  - Call `SuspendPipePages` and `InvalidateExecutableRange` on the old range first. Update `RecordMapping`.
- [ ] **Step 11: `msync` (26), `mincore` (27), `madvise` review (28).**
  - **msync.**
    - Unaligned address → EINVAL; flags other than `MS_ASYNC` 1, `MS_INVALIDATE` 2, `MS_SYNC` 4 → EINVAL; ASYNC+SYNC together → EINVAL.
    - Unmapped page in range → ENOMEM.
    - Write back shared file views in range (Step 9).
  - **mincore.**
    - Unaligned → EINVAL; unmapped → ENOMEM; vector not writable → EFAULT.
    - Write 1 for committed pages, 0 for reserved-only.
  - **madvise.**
    - Accept: NORMAL 0, RANDOM 1, SEQUENTIAL 2, WILLNEED 3, DONTNEED 4, FREE 8, REMOVE 9, DONTFORK 10, DOFORK 11, MERGEABLE 12, UNMERGEABLE 13, HUGEPAGE 14, NOHUGEPAGE 15, DONTDUMP 16, DODUMP 17, WIPEONFORK 18, KEEPONFORK 19, COLD 20, PAGEOUT 21, POPULATE_READ 22, POPULATE_WRITE 23, COLLAPSE 25.
    - DONTNEED and FREE on private anonymous pages zero them and decommit whole free granules. On shared anonymous/memfd they keep contents (Linux).
    - REMOVE punches memfd/shm pages; on private mappings → EINVAL.
    - DONTFORK / WIPEONFORK set per-page flags in `GuestAddressSpace`. The fork path (`CompleteFork`) must leave DONTFORK pages unmapped in the child and zero WIPEONFORK pages in the child. WIPEONFORK on non-private-anonymous memory → EINVAL.
    - POPULATE_READ / WRITE → ENOMEM on unmapped pages, EFAULT on PROT_NONE (POPULATE_WRITE also on read-only).
    - Unknown advice → EINVAL. Unmapped range → ENOMEM, after the advice validation.
- [ ] **Step 12: mlock family (149, 150, 151, 152, 325).**
  - All pages are resident, so locking only does accounting.
  - Unmapped page → ENOMEM.
  - Locked bytes over `RLIMIT_MEMLOCK` without `CAP_IPC_LOCK` → ENOMEM for mlock/mlock2; EPERM if the limit is 0.
  - `mlock2` flags: only `MLOCK_ONFAULT` 1, else EINVAL.
  - `mlockall` flags: `MCL_CURRENT` 1 | `MCL_FUTURE` 2 | `MCL_ONFAULT` 4. Zero, or ONFAULT alone → EINVAL. `MCL_FUTURE` makes later maps count.
  - `munlockall` → 0. Keep a per-process locked-bytes counter. Reset on exec; the fork child starts unlocked (Linux).
- [ ] **Step 13: `process_vm_readv` (310) / `process_vm_writev` (311).**
  - Arguments: `(pid, local_iov, liovcnt, remote_iov, riovcnt, flags)`.
  - Errors in order:
    1. flags ≠ 0 → EINVAL;
    2. iov counts > 1024 → EINVAL;
    3. local iov array unreadable → EFAULT;
    4. no such pid → ESRCH;
    5. no ptrace access → EPERM;
    6. first remote page unmapped → EFAULT.
  - For ptrace access, reuse the rule from milestone 2's `kill`/`pidfd` (`GuestCredentials.h`): same real/effective/saved UID and GID, or `CAP_SYS_PTRACE`.
  - Partial transfer returns the bytes done.
  - Target memory: use `CopyFromProcess` / `CopyToProcess` with `Resident = (Target is Task or !Target->RestorePending)`. Write to a page with PF_X → `InvalidateExecutableRange` on the target.
- [ ] **Step 14: `membarrier` (324).**
  - Commands: QUERY 0 → mask of supported commands. Support GLOBAL 1, GLOBAL_EXPEDITED 2, REGISTER_GLOBAL_EXPEDITED 4, PRIVATE_EXPEDITED 8, REGISTER_PRIVATE_EXPEDITED 16, PRIVATE_EXPEDITED_SYNC_CORE 32, REGISTER_..._SYNC_CORE 64, PRIVATE_EXPEDITED_RSEQ 128, REGISTER_..._RSEQ 256, GET_REGISTRATIONS 512.
  - The expedited forms without registration → EPERM. Track registrations per process.
  - Barriers return 0: guest threads run on one host thread, so the barrier holds by construction.
  - `flags` must be 0, except `MEMBARRIER_CMD_FLAG_CPU` 1 with the RSEQ command → EINVAL otherwise.
- [ ] **Step 15: Remaining VM calls.** Use the semantics of a Linux kernel on this CPU model:
  - `pkey_alloc` (330) → ENOSPC: FEX's CPUID reports no PKU.
  - `pkey_mprotect` (329): pkey -1 → mprotect; other keys → EINVAL. `pkey_free` (331) → EINVAL.
  - `mseal` (462):
    - flags ≠ 0 or unaligned → EINVAL; unmapped → ENOMEM.
    - Sets a sealed bit per page. Later `mprotect`, `munmap`, `mremap`, `mmap MAP_FIXED` over it and destructive `madvise` (DONTNEED, FREE, REMOVE on private anonymous memory) → EPERM.
  - `map_shadow_stack` (453) → EOPNOTSUPP: no CET shadow stack.
  - `memfd_secret` (447) → ENOSYS, as Linux without `secretmem.enable`.
  - `userfaultfd` (323) → ENOSYS. Needs fault delivery; deliberate difference.
  - Single-node NUMA:
    - `set_mempolicy` (238): MPOL_DEFAULT 0, PREFERRED 1, BIND 2, INTERLEAVE 3, LOCAL 4, PREFERRED_MANY 5 with nodemask ⊆ {0} → 0. Store per thread. A mode out of range, or a mask naming other nodes → EINVAL.
    - `get_mempolicy` (239) returns the stored mode. `MPOL_F_NODE` 1 | `MPOL_F_ADDR` 2 → node 0.
    - `mbind` (237) validates range (EFAULT unmapped, EINVAL unaligned) and mode → 0.
    - `set_mempolicy_home_node` (450) → 0 with home_node 0, EINVAL otherwise.
    - `migrate_pages` (256) → 0 pages not moved.
    - `move_pages` (279): status array entries 0 for mapped pages, -EFAULT (-14) per unmapped page. Return 0.
  - `process_madvise` (440):
    - pidfd ok and advice ∈ {COLD, PAGEOUT, WILLNEED, COLLAPSE}; other advice → EINVAL.
    - flags ≠ 0 → EINVAL.
    - Returns total iov bytes.
  - `process_mrelease` (448): pidfd of a process that is not exiting → EINVAL.
  - `cachestat` (451) → ENOSYS (deliberate; rarely used).
  - `remap_file_pages` (216) → ENOSYS (deliberate).
- [ ] **Step 16: Self-modifying code.**
  - `InvalidateExecutableRange` already runs on mmap/mprotect/munmap.
  - Add it to every runtime write into guest memory that can hit PF_X pages: `read`/`pread`/`readv` into exec pages, `process_vm_writev`, and `recv*` into exec pages.
  - Record in the inventory: direct guest stores into RWX pages are not detected; FEX's `mtrack` needs write faults. Name `SMCChecks=full` as the workaround.
- [ ] **Step 17: Run all checks.** Run `make -C Source/Switch2 check-guest-arena check-guest-memory` and `cmake --build Source/Switch2/build/guest --target guest_nro`. Update the inventory rows and add these deliberate differences: no SIGSEGV for guest-PROT_NONE access, MAP_32BIT, MAP_GROWSDOWN, double shared mapping, non-moving shared mremap, userfaultfd, cachestat, remap_file_pages, mlock accounting only.

### WP2: Futex, robust lists and threads [Sonnet subagent 2]

**Why:**
- Today `Futex(Address, Operation, Value, Timeout, Mask)` supports only WAIT 0, WAKE 1, WAIT_BITSET 9 and WAKE_BITSET 10. It keys waiters by `(Task->PID, Address)`, so a futex in shared memory cannot wake another process.
- glibc uses REQUEUE/CMP_REQUEUE (older condvars), WAKE_OP, PI mutexes (`PTHREAD_PRIO_INHERIT`) and robust mutexes (`PTHREAD_MUTEX_ROBUST`, `set_robust_list` at every thread start).

**Files:**
- Create: `Source/Switch2/GuestFutex.h`, `Source/Switch2/LinuxRuntime/GuestFutexTest.cpp`
- Modify: `Source/Switch2/GuestRuntime.cpp` (`Futex` around 1336, `WakeFutex` around 1311, `ExitThread` around 1320, `Ready` around 1023, the exec path, `GuestThread` struct, dispatch), `Source/Switch2/Makefile`, `Source/Switch2/CommunicationInventory.md`

**Interfaces (produced):**

```cpp
// GuestFutex.h
namespace Switch2Guest {
struct FutexKey { uint64_t Address; uint32_t Pid; auto operator<=>(const FutexKey&) const = default; };
// Pid = 0 when Address lies in a Shared region (key is global); otherwise the
// owning process PID. Private and shared ops on private memory produce the
// same key, as Linux (mm, address) keys do.
FutexKey MakeFutexKey(uint64_t Address, uint32_t Pid, bool AddressIsShared);

struct WakeOp { uint32_t Op, Cmp; int32_t OpArg, CmpArg; bool Shift; };
WakeOp DecodeWakeOp(uint32_t Val3);
// Returns -38 (ENOSYS) for Op > 4 or Cmp > 5, else 0; Old receives the prior word.
int ApplyWakeOp(const WakeOp&, uint32_t& Word, uint32_t& Old);
bool CompareWakeOp(const WakeOp&, uint32_t Old);

constexpr uint32_t FutexWaiters = 0x80000000, FutexOwnerDied = 0x40000000, FutexTidMask = 0x3fffffff;
// PI lock attempt on Word by Tid. Result: Acquired, Deadlock (EDEADLK), MustWait
// (Word updated with FutexWaiters), NoOwner (ESRCH, owner TID not alive; caller
// passes OwnerAlive).
enum class PiLock { Acquired, Deadlock, MustWait, NoOwner };
PiLock TryLockPi(uint32_t& Word, uint32_t Tid, bool OwnerAlive);

struct RobustEntry { uint64_t WordAddress; bool Pi; };
// Walks a robust list given a reader (returns false on fault). Stops after
// 2048 entries (ROBUST_LIST_LIMIT) or on a fault, as Linux exit_robust_list.
template<typename Read64>
std::vector<RobustEntry> WalkRobustList(uint64_t Head, Read64 Read);
// Linux handle_futex_death: if (Word & FutexTidMask) == Tid, the new word is
// (Word & FutexWaiters) | FutexOwnerDied and Wake reports whether to wake one.
bool FutexDeath(uint32_t& Word, uint32_t Tid, bool& Wake);
}
```

`GuestThread` gains `FutexKey WaitKey`, `uint32_t WaitBitset`, `bool WaitPi`, `std::vector<FutexKey> WaitKeys` (futex_waitv), `uint64_t RobustHead`, `size_t RobustLength`.

- [ ] **Step 1: Failing host tests in `GuestFutexTest.cpp`.**
  - WAKE_OP decoding of all 5 ops and 6 compares.
  - Sign extension of 12-bit args. The `FUTEX_OP_OPARG_SHIFT` flag (bit 31 of val3 >> 28 & 8) gives `1 << (arg & 31)`.
  - Invalid op/cmp → -38.
  - TryLockPi:
    - 0 → Tid;
    - Tid itself → Deadlock;
    - other live owner → MustWait with the waiters bit set;
    - dead owner without OWNER_DIED → NoOwner;
    - `OWNER_DIED|0` → Acquired keeping OWNER_DIED.
  - FutexDeath with matching and non-matching TID.
  - Robust list:
    - circular list stops at the head;
    - the 2048 limit;
    - the pending entry (`list_op_pending`) is processed after the list;
    - bit 0 of an entry pointer marks PI.
  - Add `check-guest-futex`.
- [ ] **Step 2: Implement `GuestFutex.h`** until the tests pass.
- [ ] **Step 3: Signature and keys.**
  - Change dispatch case 202 to `Futex(A, B, C, Registers[REG_R10], Registers[REG_R8], static_cast<uint32_t>(Registers[REG_R9]))`, that is `(uaddr, op, val, timeout_or_val2, uaddr2, val3)`.
  - Replace `WaitAddress`/PID matching in `WakeFutex` with `FutexKey` matching. Shared = `Task->Memory` reports the address inside a `Shared` region (add `bool IsShared(uint64_t) const` to `GuestAddressSpace`).
  - Since the key can be global, waking scans all threads, not only `Task->PID`.
- [ ] **Step 4: Commands.** `cmd = op & 0x7f`; `FUTEX_PRIVATE_FLAG` 128; `FUTEX_CLOCK_REALTIME` 256 (valid only with WAIT_BITSET, WAIT_REQUEUE_PI, LOCK_PI2 → else ENOSYS, as Linux).
  - **REQUEUE 3 / CMP_REQUEUE 4.**
    - `val` and `val2` (as int) < 0 → EINVAL.
    - CMP checks `*uaddr == val3`, else EAGAIN.
    - Wake up to `val` waiters on key1; move up to `val2` remaining waiters to key2 (FIFO kept).
    - Return woken + requeued.
    - uaddr2 misaligned → EINVAL; uaddr2 unmapped → EFAULT.
  - **WAKE_OP 5.**
    - Read-modify-write `*uaddr2` with `ApplyWakeOp`. uaddr2 not writable → EFAULT.
    - Wake `val` waiters on uaddr; if `CompareWakeOp(old)`, wake `val2` waiters on uaddr2.
    - Return the sum.
  - **LOCK_PI 6 / LOCK_PI2 13 / TRYLOCK_PI 8 / UNLOCK_PI 7.**
    - Use `TryLockPi`; OwnerAlive = a live thread with that TID exists.
    - Timeout: LOCK_PI is absolute CLOCK_REALTIME; LOCK_PI2 is absolute MONOTONIC unless the CLOCK_REALTIME flag is set.
    - TRYLOCK_PI with MustWait → EAGAIN, and clear the waiters bit it set if nobody waits.
    - UNLOCK_PI: `(word & TidMask) != Tid` → EPERM. With waiters: hand off to the first waiter (FIFO). Set the word to `waiterTid | (more waiters ? FutexWaiters : 0)` while keeping OWNER_DIED, and wake that waiter with RAX 0. Without waiters: word = 0.
    - No priority inheritance: deliberate difference (cooperative single host thread).
  - **WAIT_REQUEUE_PI 11 / CMP_REQUEUE_PI 12.**
    - CMP_REQUEUE_PI requires `val == 1` → else EINVAL; `uaddr == uaddr2` → EINVAL.
    - The waiter woken by requeue acquires the PI futex at uaddr2 through the UNLOCK_PI handoff rule.
    - A WAIT_REQUEUE_PI waiter woken by a plain WAKE returns EAGAIN.
  - Unknown commands → ENOSYS.
- [ ] **Step 5: Robust lists.**
  - `set_robust_list` (273): `len != 24` → EINVAL. Store head per thread.
  - `get_robust_list` (274): pid 0 = self. Other pid: ESRCH if absent, EPERM without ptrace access (same rule as WP1 Step 13). Head/len pointers unwritable → EFAULT.
  - In `ExitThread` (before the `ClearTid` write) and in exec (`InstallExec`, before the image is replaced), walk the list with `WalkRobustList`. Read through `Available`; stop on fault.
    - Apply `FutexDeath` to each entry. Wake one waiter when needed; PI entries hand off as UNLOCK_PI does.
    - Do it for every thread of the process on `exit_group` and on fatal signals.
  - A fork child's robust list head is copied from the forking thread (Linux).
- [ ] **Step 6: futex2.**
  - **`futex_waitv` (449):** `(waiters, nr, flags, timeout, clockid)`.
    - `flags != 0` → EINVAL; `nr == 0 || nr > 128` → EINVAL.
    - Each `{u64 val, u64 uaddr, u32 flags, u32 reserved}`: reserved ≠ 0 → EINVAL. Flags must be `FUTEX2_SIZE_U32` 2, optionally | `FUTEX2_PRIVATE` 128, else EINVAL. `val` above 32 bits → EINVAL.
    - Any word mismatch → EAGAIN.
    - Timeout is absolute in clockid (MONOTONIC 1 or REALTIME 0, else EINVAL).
    - Block on all keys. The wake returns the index of the futex that woke it.
  - **`futex_wake` (454):** `(uaddr, mask, nr, flags)`.
  - **`futex_wait` (455):** `(uaddr, val, mask, flags, timeout, clockid)`.
  - **`futex_requeue` (456):** `(waiters[2], flags, nr_wake, nr_requeue)`.
  - For all three, flags other than SIZE_U32 / PRIVATE → EINVAL, as Linux 6.7.
- [ ] **Step 7: Restart rules.**
  - FUTEX_WAIT with a relative timeout restarts via `restart_syscall` semantics. Linux returns `-ERESTART_RESTARTBLOCK` → EINTR when the handler lacks SA_RESTART; otherwise it restarts with the remaining time.
  - LOCK_PI restarts (`-ERESTARTNOINTR`). futex_waitv → EINTR.
  - Encode in `RestartableSyscall` with comments.
- [ ] **Step 8: Run all checks.** Run `make -C Source/Switch2 check-guest-futex check-guest-memory` and the `guest_nro` build. Update the inventory and add these deliberate differences: no priority inheritance or boosting; PI waiters are FIFO.

### WP3: Pipes, System V IPC, POSIX message queues, /dev/shm [Sonnet subagent 3]

**Files:**
- Create: `Source/Switch2/GuestSysvIpc.h`, `Source/Switch2/GuestMqueue.h`, `Source/Switch2/LinuxRuntime/GuestSysvIpcTest.cpp`, `Source/Switch2/LinuxRuntime/GuestMqueueTest.cpp`
- Modify: `Source/Switch2/GuestRuntime.cpp` (`CreatePipe` around 7153, `OpenGuest` path lookup for `/dev/shm`, the `OpenFile` destructor, the process exit and exec paths, `CompleteFork`, `Ready`, dispatch), `Source/Switch2/Makefile`, `Source/Switch2/CommunicationInventory.md`

**Interfaces (produced):**

```cpp
// GuestSysvIpc.h — pure tables; the runtime owns waiting and memory.
namespace Switch2Guest {
struct IpcPerm { int32_t Key; uint32_t Uid, Gid, Cuid, Cgid; uint16_t Mode; uint16_t Seq; };
struct IpcCred { uint32_t Uid, Gid; std::span<const uint32_t> Groups; bool CapIpcOwner; };
// Linux ipcperms(): owner/creator bits, then group, then other; CAP_IPC_OWNER bypasses.
bool IpcAllowed(const IpcPerm&, const IpcCred&, uint16_t Requested /* 4 read, 2 write */);
template<typename Object> class IpcTable {   // one per kind
public:
  // shmget/semget/msgget lookup: returns id >= 0 or -errno (ENOENT, EEXIST, EACCES, ENOSPC, EINVAL via Check).
  int64_t Get(int32_t Key, int Flags, const IpcCred&, auto&& Create, auto&& Check);
  Object* Find(int32_t Id);                  // nullptr → EINVAL (or EIDRM for a removed id's seq)
  void Remove(int32_t Id);
  int32_t MaxIndex() const;
};
struct SemOp { uint16_t Num; int16_t Op; int16_t Flags; };
// Linux perform_atomic_semop: returns 0 (applied), 1 (must block), or -errno (EFBIG, ERANGE).
int TrySemOps(std::span<uint16_t> Values, std::span<const SemOp>, std::vector<int16_t>& UndoDelta, uint32_t Pid);
struct Message { int64_t Type; std::vector<uint8_t> Text; };
// msgrcv type selection: 0 first; >0 first of type (or not of type with MSG_EXCEPT); <0 lowest type <= |type|.
std::optional<size_t> SelectMessage(const std::deque<Message>&, int64_t Type, bool Except);
}
// GuestMqueue.h
namespace Switch2Guest {
struct MqAttr { int64_t Flags, MaxMsg, MsgSize, CurMsgs; };
class MessageQueue {
public:
  int64_t Send(std::span<const uint8_t>, uint32_t Priority);   // -EMSGSIZE, -EAGAIN when full
  int64_t Receive(std::span<uint8_t>, uint32_t& Priority);     // -EMSGSIZE if buffer < MsgSize, -EAGAIN when empty
  // highest priority first, FIFO within a priority
};
}
```

- [ ] **Step 1: Measure layouts natively.**
  - Write a throwaway C file in `.superpowers/sdd/scratch` that prints `sizeof` and `offsetof` for `struct shmid_ds`, `semid_ds`, `msqid_ds`, `ipc_perm`, `shminfo`, `shm_info`, `seminfo`, `msginfo`, `mq_attr`, `sembuf`. Build it with `$(HOST_X86_CC)`.
  - Copy the numbers into `static_assert`s in the headers. Linux x86-64 `ipc64_perm` is 48 bytes and `shmid64_ds` is 112 bytes; verify the rest from the output.
- [ ] **Step 2: Failing host tests.**
  - **`IpcAllowed`:** owner, group, other, CAP_IPC_OWNER.
  - **`IpcTable::Get`:**
    - `IPC_PRIVATE` always creates;
    - `IPC_CREAT|IPC_EXCL` on an existing key → EEXIST;
    - a missing key without CREAT → ENOENT;
    - permission check on an existing key → EACCES;
    - ids encode `seq * 32768 + index` (Linux `ipc_buildid` with IPCMNI 32768);
    - a stale id → EINVAL.
  - **`TrySemOps`:**
    - all-or-nothing;
    - op 0 waits for zero;
    - negative op blocks when the value would go below 0;
    - `IPC_NOWAIT` → EAGAIN (caller);
    - value > SEMVMX 32767 → ERANGE;
    - `sem_num >= nsems` → EFBIG;
    - SEM_UNDO accumulates the negated delta.
  - **`SelectMessage`:** all type modes.
  - **`MessageQueue`:** priority order, EMSGSIZE both ways, full/empty.
  - Add the `check-guest-sysv-ipc` and `check-guest-mqueue` targets.
- [ ] **Step 3: Implement both headers** until the tests pass.
- [ ] **Step 4: SysV shared memory (29, 30, 31, 67).**
  - **shmget(key, size, flags):**
    - size < SHMMIN 1 or > SHMMAX → EINVAL;
    - existing segment smaller than size → EINVAL;
    - `SHM_HUGETLB` 04000 → ENOMEM (no pool);
    - total pages over SHMALL → ENOSPC.
    - Storage: a `GuestMemoryFile` (WP1 arena-backed).
  - **shmat(id, addr, flags):**
    - Flags: `SHM_RDONLY` 010000, `SHM_RND` 020000, `SHM_REMAP` 040000, `SHM_EXEC` 0100000.
    - The segment has **one** address. `addr == 0` → that address.
    - `addr != 0` → after SHM_RND rounding to SHMLBA 4096 it must equal that address, else EINVAL (deliberate difference).
    - Already attached in this process → ENOMEM (deliberate difference; single view).
    - Register as a Shared region in `Task->Memory`. `shm_nattch++`; set `shm_atime`, `shm_lpid`.
  - **shmdt(addr):** the address is not a segment start → EINVAL. `nattch--`, `shm_dtime`.
  - **shmctl:**
    - Strip `IPC_64` 0x100 from cmd.
    - Commands: IPC_STAT 2, IPC_SET 1 (uid, gid, mode low 9 bits; owner/creator or CAP_SYS_ADMIN else EPERM), IPC_RMID 0 (mark destroyed, key → IPC_PRIVATE, free at nattch 0), IPC_INFO 3, SHM_INFO 14, SHM_STAT 13, SHM_STAT_ANY 15, SHM_LOCK 11, SHM_UNLOCK 12.
    - SHM_LOCK/SHM_UNLOCK need CAP_IPC_LOCK, or the owner within RLIMIT_MEMLOCK.
    - Struct copy-out uses the native layout from Step 1.
  - **Lifetime:**
    - fork: the child inherits attachments, `nattch++` each.
    - exec and exit: detach all.
    - The namespace is per runtime and is lost when the launcher exits (as after a reboot).
- [ ] **Step 5: SysV semaphores (64, 65, 66, 220).**
  - **semget:** `nsems` > SEMMSL 32000 → EINVAL; an existing set with fewer sems → EINVAL.
  - **semop / semtimedop:**
    - `nsops` > SEMOPM 500 → E2BIG; `nsops == 0` → EINVAL.
    - Blocking adds `WaitKind::Semaphore`. Wake check: re-run `TrySemOps` for every waiter after any change, Linux `update_queue` order (FIFO).
    - A removed set → waiters get EIDRM.
    - `semtimedop` timeout is relative; on expiry → EAGAIN (Linux returns EAGAIN on timeout).
    - Interrupted → EINTR, never restarted (Linux).
  - **semctl:**
    - Commands: IPC_RMID, IPC_SET, IPC_STAT, IPC_INFO, SEM_INFO 19, SEM_STAT 18, SEM_STAT_ANY 20, GETPID 11, GETVAL 12, GETALL 13, GETNCNT 14, GETZCNT 15, SETVAL 16, SETALL 17.
    - SETVAL/SETALL range check against SEMVMX → ERANGE; they clear the undo entries for those sems.
  - **SEM_UNDO:** per-process undo list, applied at process exit (clamp at 0 and SEMVMX as Linux `exit_sem`). Not inherited by fork unless `CLONE_SYSVSEM`.
- [ ] **Step 6: SysV message queues (68, 69, 70, 71).**
  - **Limits:** MSGMAX 8192, MSGMNB 16384, MSGMNI 32000.
  - **msgsnd:**
    - `msgsz > msg_qbytes` or `> MSGMAX`, or `mtype < 1` → EINVAL.
    - Blocks while `qbytes + msgsz > msg_qbytes` or the message count would exceed `msg_qbytes` (Linux `msg_fits_inqueue`).
    - `IPC_NOWAIT` → EAGAIN.
  - **msgrcv:**
    - Flags: `MSG_NOERROR` 010000 truncates, otherwise E2BIG when the text is longer than msgsz.
    - `MSG_EXCEPT` 020000.
    - `MSG_COPY` 040000 requires IPC_NOWAIT and no MSG_EXCEPT, else EINVAL. It copies the message at index `msgtyp`, or ENOMSG.
    - `IPC_NOWAIT` with nothing to take → ENOMSG.
    - Blocking → `WaitKind::MessageQueue`; a removed queue → EIDRM.
  - **msgctl:** IPC_STAT, IPC_SET (raising `msg_qbytes` above MSGMNB needs CAP_SYS_RESOURCE else EPERM), IPC_RMID, IPC_INFO, MSG_INFO 12, MSG_STAT 11, MSG_STAT_ANY 13.
- [ ] **Step 7: POSIX message queues (240–245).**
  - **mq_open(name, oflag, mode, attr).** glibc has already stripped the leading `/`.
    - Name errors: empty → ENOENT; contains `/` → EACCES; longer than 255 → ENAMETOOLONG.
    - `O_CREAT` checks attr when non-null: `mq_maxmsg` must be 1..10 and `mq_msgsize` 1..8192 without CAP_SYS_RESOURCE, else EINVAL. The defaults are 10 and 8192.
    - `O_CREAT|O_EXCL` on an existing queue → EEXIST.
    - Permission check uses the queue mode and creds → EACCES.
    - More than 256 queues → ENOSPC.
    - Bytes over RLIMIT_MSGQUEUE (819200) → EMFILE.
  - The descriptor is an `OpenFile` with a `std::shared_ptr<MqueueState>`. Poll: readable when non-empty, writable when not full. `fstat` mode is the queue mode.
  - **mq_timedsend / mq_timedreceive:**
    - absolute CLOCK_REALTIME timeout; ETIMEDOUT on expiry; EINVAL for a bad timespec (only checked when the call would block);
    - priority ≥ 32768 → EINVAL;
    - a descriptor opened without the right access → EBADF.
  - **mq_notify:**
    - `SIGEV_SIGNAL` 0 / `SIGEV_NONE` 1; registration conflicts → EBUSY; NULL unregisters.
    - Notify only on the empty→non-empty transition when no receiver is blocked, then auto-unregister (Linux).
    - Signal siginfo: `si_code` SI_MESGQ -3, `si_pid`/`si_uid` of the sender, `si_value` from sigevent.
    - `SIGEV_THREAD` 2 reaches the kernel as a netlink cookie (glibc). Return EINVAL for `sigev_notify` 2 and record it (glibc's `mq_notify(SIGEV_THREAD)` needs AF_NETLINK).
  - **mq_getsetattr:** only O_NONBLOCK settable. **mq_unlink:** ENOENT if missing; the queue lives until its last descriptor closes.
  - No `/dev/mqueue` filesystem view: deliberate difference.
- [ ] **Step 8: `/dev/shm` tmpfs.**
  - glibc `shm_open` and `sem_open` use `/dev/shm/<name>`. Add a memory-backed directory at `/dev/shm`: a map name → (`GuestMemoryFile`, inode, mode, uid, gid, times). It is visible through `open`/`openat`, `O_CREAT|O_EXCL`, `unlink`, `ftruncate`, `fstat`/`stat`, `getdents64`, `rename` within the directory, `fchmod`/`fchown`, and `mmap MAP_SHARED`.
  - Reuse the memfd mapping path, which already maps `File->Memory` shared.
  - Directory mode 01777 root:root; the sticky bit rule applies to unlink and rename.
  - Contents are lost when the launcher exits (tmpfs).
  - Find where `IsVirtualEntry`/`VirtualDirectory` (around 4786–4831) route `/dev`, and add `/dev/shm` there.
- [ ] **Step 9: Pipes.**
  - `pipe2` with `O_DIRECT` 040000: packet mode. Each write of ≤ PIPE_BUF 4096 bytes is one packet; larger writes split into 4096-byte packets. A read returns at most one packet and discards the excess (Linux).
  - `O_NOTIFICATION_PIPE` 0x80 → EINVAL. Deliberate: no watch queues.
  - `F_SETPIPE_SZ` above `/proc/sys/fs/pipe-max-size` (1048576) without CAP_SYS_RESOURCE → EPERM. Check it is already so in `PipeCapacity` (around 6198); fix if not.
  - FIONREAD on pipes already works; keep it.
- [ ] **Step 10: Run all checks.** Run `make -C Source/Switch2 check-guest-sysv-ipc check-guest-mqueue check-guest-memory` and the `guest_nro` build. Update the inventory and add these deliberate differences: shmat address, double attach, no /dev/mqueue, SIGEV_THREAD, IPC objects not persistent across launcher restarts.

### WP4: AF_UNIX completion [Sonnet subagent 4]

**Why:** The BSD service rejects AF_UNIX, socketpair and SEQPACKET (probe), so all Unix sockets stay in the runtime. Today `CheckSocket` accepts only `(AF_UNIX 1, SOCK_STREAM 1, protocol 0)`. Names live in `SocketNames` (a string map) instead of filesystem nodes. `getsockname`, `getpeername`, `getsockopt`, `setsockopt`, `sendmmsg` and `recvmmsg` are unrouted.

**Files:**
- Create: `Source/Switch2/LinuxRuntime/GuestSocketTest.cpp`
- Modify: `Source/Switch2/GuestSocket.h`, `Source/Switch2/GuestRuntime.cpp` (`CheckSocket`, `CreateSocket`, `SocketPair`, `BindSocket`, `ConnectSocket`, `AcceptSocket`, `SocketTransfer`, `SocketMessage`, `ShutdownSocket` around 7145–7520; `SocketNames` around 10182; `MakeNode` around 5632; `PollGuest` socket readiness; epoll item readiness; `fstat` of sockets), `Source/Switch2/Makefile`, `Source/Switch2/CommunicationInventory.md`

**Interfaces (produced):**

```cpp
// GuestSocket.h additions
enum class SocketKind : uint8_t { Stream, Datagram, SeqPacket };
struct SocketCred { int32_t Pid; uint32_t Uid, Gid; };
// StreamSocket keeps its name for compatibility; add:
//   SocketKind Kind;
//   std::deque<Record> Records;   // Datagram/SeqPacket: {size_t Bytes; std::string Source; SocketCred Cred; bool Eor;}
//   std::string Name;             // bound address bytes after sun_family (abstract names start with '\0')
//   std::optional<SocketCred> PeerCred;  // SO_PEERCRED, set at connect()/socketpair()
//   bool PassCred;                // SO_PASSCRED
//   bool ResetPending;            // peer closed with unread data → ECONNRESET once (stream/seqpacket)
//   size_t SendBuffer, ReceiveBuffer;    // SO_SNDBUF/SO_RCVBUF values as Linux reports them (doubled)
// New member functions:
//   size_t SendRecord(std::span<const uint8_t>, std::string Source, SocketCred, std::vector<std::shared_ptr<File>> Files, bool Eor);
//   size_t ReceiveRecord(std::span<uint8_t>, bool Peek, size_t& FullLength, std::string* Source, SocketCred*);
```

- [ ] **Step 1: Failing host tests in `GuestSocketTest.cpp`.** Instantiate `StreamSocket<int>`. Cases:
  1. Datagram boundaries are kept.
  2. Receive into a short buffer truncates and reports the full length (MSG_TRUNC); the rest of that record is discarded.
  3. SEQPACKET keeps boundaries and EOR.
  4. Rights attach to their record and come out with it.
  5. Datagram queue capacity counts records: Linux `max_dgram_qlen` 512 → the next send blocks or EAGAIN.
  6. Peer close with unread input sets `ResetPending` for stream.
  7. `Poll` for a datagram socket: writable when the peer queue has room.
  Add `check-guest-socket`.
- [ ] **Step 2: Implement the `GuestSocket.h` additions** until the tests pass.
- [ ] **Step 3: Socket creation.**
  - `CheckSocket(Domain, Type, Protocol)` for AF_UNIX 1: types STREAM 1, DGRAM 2, SEQPACKET 5; flags SOCK_NONBLOCK 04000, SOCK_CLOEXEC 02000000.
  - Other types → ESOCKTNOSUPPORT (-94); protocol ≠ 0 → EPROTONOSUPPORT (-93).
  - Families: AF_INET 2 goes to WP5. AF_INET6 10, AF_NETLINK 16 and AF_PACKET 17 → EAFNOSUPPORT (-97), deliberate (the host service rejects IPv6, probe; no netlink).
  - `socketpair` supports all three AF_UNIX types and sets PeerCred on both ends.
- [ ] **Step 4: Filesystem socket names.**
  - `bind` with a path name (first byte ≠ 0) creates an `S_IFSOCK` node through the `MakeNode` path, with mode `0777 & ~umask` (Linux `unix_bind`).
  - An existing path → EADDRINUSE (-98). A missing directory → ENOENT. Parent not writable → EACCES.
  - Key the binding table by **inode** for path names and by bytes for abstract names: change `SocketNames` to `std::map<std::string, ...>` for abstract names plus `std::map<uint64_t, ...>` for inodes.
  - `connect`/`sendto` to a path:
    - resolve the path (follow symlinks);
    - a missing path → ENOENT;
    - not a socket node → ECONNREFUSED (-111);
    - no write permission on the node → EACCES;
    - a node with no live binding (stale) → ECONNREFUSED.
  - Unlinking the node does not affect live sockets (Linux).
  - **Autobind:** `bind` with addrlen == 2, or `connect`/`sendto` on an unbound socket with SO_PASSCRED, gets an abstract name `\0` + 5 lowercase hex digits (Linux `unix_autobind`).
  - **Name lengths:** `sun_path` up to 108 bytes. A path name is cut at the first NUL. getsockname returns `offsetof(sun_path) + strlen + 1` for path names, and the exact length for abstract names.
- [ ] **Step 5: Datagram and seqpacket semantics.**
  - **DGRAM:**
    - `connect` sets the default peer; connecting to AF_UNSPEC 0 disconnects.
    - `sendto` with an address works on unconnected sockets; without an address and without a peer → ENOTCONN (-107). With an address on a connected socket, the address wins (Linux).
    - A receiver that is not reading and is full → EAGAIN or block. A receiver that is gone → ECONNREFUSED.
    - Records larger than SO_SNDBUF → EMSGSIZE (-90).
  - **SEQPACKET:** connection-oriented like STREAM with record boundaries; `MSG_EOR` accepted. Reading zero after peer shutdown gives EOF (0).
  - `recvfrom`/`recvmsg` return the sender's name (`msg_namelen`). An unnamed sender gives namelen 2.
- [ ] **Step 6: `getsockname` (51) / `getpeername` (52).**
  - EBADF / ENOTSOCK (-88) first.
  - Unconnected for getpeername → ENOTCONN.
  - `addrlen` pointer unreadable/unwritable → EFAULT. A negative length → EINVAL.
  - Copy at most `*addrlen` bytes and store the full length.
- [ ] **Step 7: `setsockopt` (54) / `getsockopt` (55)** for AF_UNIX at `SOL_SOCKET` 1.
  - Options: SO_TYPE 3, SO_ERROR 4, SO_DOMAIN 39, SO_PROTOCOL 38, SO_ACCEPTCONN 30, SO_PEERCRED 17 (`struct ucred {pid, uid, gid}`), SO_PASSCRED 16, SO_PASSSEC 34 (store), SO_SNDBUF 7 / SO_RCVBUF 8, SO_SNDBUFFORCE 32 / SO_RCVBUFFORCE 33 (need CAP_NET_ADMIN → EPERM), SO_RCVTIMEO_OLD 20 / SO_SNDTIMEO_OLD 21, SO_REUSEADDR 2 (store), SO_KEEPALIVE 9 (store), SO_LINGER 13 (store, `struct linger` 8 bytes), SO_RCVLOWAT 18 (honor in readiness and reads), SO_PEEK_OFF 42 (honor for MSG_PEEK on stream/dgram), SO_PEERGROUPS 59 (ERANGE with the needed length when the buffer is small), SO_PEERPIDFD 77.
  - Buffer values: SNDBUF/RCVBUF store `max(val*2, 4608)` for SNDBUF and `max(val*2, 2304)` for RCVBUF, capped at `net.core.wmem_max` / `rmem_max` 212992 (Linux `sock_setsockopt`). Getsockopt returns the stored value.
  - `SO_RCVTIMEO`/`SO_SNDTIMEO`: check whether milestone 2 already handles them (`grep -n "SO_RCVTIMEO\|RcvTimeout" GuestRuntime.cpp`) and reuse that.
  - Errors: unknown option → ENOPROTOOPT (-92); optlen < 4 for int options → EINVAL; bad pointers → EFAULT. Other levels on AF_UNIX → EOPNOTSUPP (-95) for set, ENOPROTOOPT for get.
- [ ] **Step 8: Messages.**
  - **sendmsg/recvmsg cmsg:**
    - SCM_RIGHTS (exists) for all three types.
    - SCM_CREDENTIALS 2 (`struct ucred`): the sender may send its own pid/uid/gid; others need CAP_SYS_ADMIN/CAP_SETUID/CAP_SETGID → EPERM.
    - The receiver gets SCM_CREDENTIALS on each message when SO_PASSCRED is set, filled with the sender's creds when the sender sent none.
  - **Flags:**
    - Send: MSG_DONTWAIT 0x40, MSG_NOSIGNAL 0x4000, MSG_EOR 0x80, MSG_MORE 0x8000 (accepted).
    - Receive: MSG_PEEK 2, MSG_TRUNC 0x20 (returns the full record length for dgram/seqpacket; for stream discards bytes), MSG_WAITALL 0x100 (stream: block until full, EOF, error or signal), MSG_CMSG_CLOEXEC, MSG_DONTWAIT, MSG_ERRQUEUE 0x2000 (→ EAGAIN).
    - Unknown flags → EOPNOTSUPP as today.
    - Output `msg_flags`: MSG_TRUNC, MSG_CTRUNC, MSG_EOR.
  - Lift the 64 KiB per-call stream cap (`StreamSocket::Capacity`) only for the iov total; queue capacity stays.
  - **`sendmmsg` (307) / `recvmmsg` (299):**
    - vlen capped at UIO_MAXIOV 1024.
    - Each entry's `msg_len` is written.
    - Stop at the first error after ≥1 success and return the count; the error is returned on the next call (Linux keeps it in `sk_err` for recvmmsg). For sendmmsg, return the count.
    - recvmmsg: `MSG_WAITFORONE` 0x10000; the timeout is checked between messages only (Linux behavior).
- [ ] **Step 9: Teardown and readiness.**
  - A stream peer closing with unread data in **its** receive queue makes the other end's next `recv` return ECONNRESET (-104), then 0 (Linux `unix_release_sock`).
  - POLLHUP when both directions are shut. POLLRDHUP 0x2000 on peer write shutdown.
  - Listening socket close: queued connections are reset (their connect side sees ECONNRESET on read).
  - `fstat` of a socket: `S_IFSOCK|0777`.
  - Bump `IOEpoch` and the socket `Changes` counter in every path that changes readiness.
- [ ] **Step 10: Run all checks.** Run `make -C Source/Switch2 check-guest-socket check-guest-memory` and the `guest_nro` build. Update the inventory.

### WP5: AF_INET TCP and UDP over host BSD sockets [Sonnet subagent 5]

**Probe facts (m3probe 2):**
- The service accepts 8 sessions and allows 126 sockets per process.
- Loopback TCP gives about 400 MiB/s.
- A UDP datagram of 60000 bytes fails with EMSGSIZE.
- `g_bsdErrno` uses Linux numbers.
- A Linux-shaped `sockaddr_in` is rejected with EADDRNOTAVAIL.
- The resolver works.
- A blocking `accept` in a helper host thread does not block the main thread, and `shutdown` from the main thread wakes it.
- Raw ICMP is allowed (`SOCK_RAW` + `IPPROTO_ICMP` returned a socket). ICMP datagram sockets → EPROTONOSUPPORT. `SOCK_NONBLOCK|SOCK_CLOEXEC` bits are accepted.

**Design decision:** host sockets are always **nonblocking**. A blocking guest call becomes `WaitKind::Net` with the host fd and events. The scheduler polls host sockets with `poll(…, 0)` on every pass that has Net waiters, and uses `poll(…, timeout)` in place of the idle `svcSleepThread`. No helper threads; they could be added later for DNS only.

**Files:**
- Create: `Source/Switch2/GuestInet.h`, `Source/Switch2/LinuxRuntime/GuestInetTest.cpp`
- Modify:
  - `Source/Switch2/GuestRuntime.cpp`: socket handlers, `OpenFile` (host fd member and destructor), `Ready`, the `RunThreads` idle path around line 400, `PollGuest`, epoll readiness, `select`, the `FileControl` `O_NONBLOCK` path, `ioctl` FIONREAD, SIGPIPE suppression.
  - `Source/Switch2/GuestProbe.cpp`: network bring-up.
  - `Source/Switch2/GuestRootfsSetup.h` or the launcher start path: `/etc/resolv.conf`.
  - `Source/Switch2/Makefile`, `Source/Switch2/CommunicationInventory.md`.

**Interfaces (produced):**

```cpp
// GuestInet.h — host-testable: uses only fixed numeric constants, no libnx headers.
namespace Switch2Guest::Inet {
// Linux values
constexpr int LinuxSolSocket = 1, LinuxAfInet = 2, LinuxIpprotoTcp = 6, LinuxIpprotoIp = 0, LinuxIpprotoUdp = 17;
// BSD values used by libounce (sys/socket.h): verify each against /opt/devkitpro/libounce/include and
// the devkitA64 newlib headers in Step 1 and static_assert them in GuestInet.cpp-free code via a table.
constexpr int BsdSolSocket = 0xffff;
struct OptionMap { int LinuxLevel, LinuxName, BsdLevel, BsdName; enum Kind : uint8_t { Int, Bool, Linger, Timeval, Ip4 } Type; };
std::optional<OptionMap> MapOption(int LinuxLevel, int LinuxName);   // nullopt → ENOPROTOOPT
std::optional<int> MapSendFlags(uint64_t Linux, uint64_t& Unsupported); // Linux MSG_* → BSD MSG_*
std::optional<int> MapReceiveFlags(uint64_t Linux, uint64_t& Unsupported);
uint64_t LinuxMessageFlags(int Bsd);                                  // msg_flags back to Linux
// sockaddr: Linux {u16 family; u16 port_be; u32 addr_be; u8 zero[8]} (16 bytes) ↔ BSD {u8 len; u8 family; ...}
int64_t ToBsdAddress(std::span<const uint8_t> Linux, std::array<uint8_t, 16>& Bsd);   // -EINVAL short, -EAFNOSUPPORT family
size_t ToLinuxAddress(std::span<const uint8_t> Bsd, std::array<uint8_t, 16>& Linux);
int LinuxErrno(int BsdErrno);   // identity for g_bsdErrno (probe: Linux numbering); table for newlib errno fallbacks
}
```

- [ ] **Step 1: Constants table.**
  - Read `/opt/devkitpro/libounce/include/sys/socket.h` (or the newlib `sys/socket.h` it includes), `netinet/in.h`, `netinet/tcp.h`.
  - Record the BSD values for: SO_REUSEADDR, SO_KEEPALIVE, SO_BROADCAST, SO_LINGER, SO_SNDBUF, SO_RCVBUF, SO_SNDLOWAT, SO_RCVLOWAT, SO_SNDTIMEO, SO_RCVTIMEO, SO_ERROR, SO_TYPE, SO_REUSEPORT, SO_OOBINLINE; IP_TOS, IP_TTL, IP_MULTICAST_IF, IP_MULTICAST_TTL, IP_MULTICAST_LOOP, IP_ADD_MEMBERSHIP, IP_DROP_MEMBERSHIP; TCP_NODELAY, TCP_MAXSEG, TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT (if present); MSG_OOB, MSG_PEEK, MSG_DONTROUTE, MSG_WAITALL, MSG_DONTWAIT, MSG_TRUNC, MSG_CTRUNC, MSG_EOR, MSG_NOSIGNAL (if present).
  - Linux values: SOL_SOCKET options as in WP4 plus SO_BROADCAST 6, SO_OOBINLINE 10, SO_REUSEPORT 15, SO_SNDLOWAT 19, SO_BINDTODEVICE 25, SO_TIMESTAMP 29, SO_RCVTIMEO_NEW 66, SO_SNDTIMEO_NEW 67, SO_INCOMING_CPU 49, SO_BUSY_POLL 46; IPPROTO_IP options IP_TOS 1, IP_TTL 2, IP_HDRINCL 3, IP_OPTIONS 4, IP_RECVERR 11, IP_MTU_DISCOVER 10, IP_MULTICAST_IF 32, IP_MULTICAST_TTL 33, IP_MULTICAST_LOOP 34, IP_ADD_MEMBERSHIP 35, IP_DROP_MEMBERSHIP 36; TCP options TCP_NODELAY 1, TCP_MAXSEG 2, TCP_CORK 3, TCP_KEEPIDLE 4, TCP_KEEPINTVL 5, TCP_KEEPCNT 6, TCP_SYNCNT 7, TCP_LINGER2 8, TCP_DEFER_ACCEPT 9, TCP_WINDOW_CLAMP 10, TCP_INFO 11, TCP_QUICKACK 12, TCP_CONGESTION 13, TCP_USER_TIMEOUT 18, TCP_FASTOPEN 23, TCP_NOTSENT_LOWAT 25.
  - Options the host lacks are emulated where Linux semantics allow storing them: SO_BINDTODEVICE (empty string only, else ENODEV), SO_TIMESTAMP, TCP_CORK, TCP_QUICKACK, TCP_DEFER_ACCEPT, TCP_USER_TIMEOUT, TCP_NOTSENT_LOWAT, TCP_CONGESTION (getsockopt "cubic"; set accepts "cubic" or "reno", else ENOENT).
  - `TCP_INFO` returns a zeroed `struct tcp_info` (232 bytes) with `tcpi_state` from connection state. Record it as partial in the inventory.
- [ ] **Step 2: Failing `GuestInetTest.cpp`.**
  - Address round trip, short address → EINVAL, family 10 → EAFNOSUPPORT.
  - The BSD address has `sin_len` 16 and family 2 at byte 1.
  - Every mapped option maps both ways.
  - Unknown option → nullopt.
  - Flag translation: Linux MSG_DONTWAIT 0x40 → BSD 0x80; MSG_NOSIGNAL 0x4000 is handled by the runtime (never passed to the host); unsupported bits reported.
  - Add `check-guest-inet`.
- [ ] **Step 3: Implement `GuestInet.h`** until the tests pass.
- [ ] **Step 4: Network bring-up.**
  - On the first AF_INET `socket()` in a launcher run, call `socketInitialize` with the FTP screen's config (`GuestProbe.cpp` around line 380), except `num_bsd_sessions = 4`.
  - Keep it initialized until the launcher exits.
  - If `nifmGetCurrentIpAddress` fails, AF_INET still works for 127.0.0.1. Other destinations return ENETUNREACH (-101) from the host.
  - Expose a runtime call `bool EnsureNetwork()` that returns false only if `socketInitialize` fails. `socket(AF_INET)` then returns EAFNOSUPPORT (-97) and logs it once.
  - The FTP server path must keep working: share one init/exit refcount between the FTP screen and the guest.
- [ ] **Step 5: `/etc/resolv.conf`.**
  - At launch, when nifm reports a connection, read DNS servers with `nifmGetCurrentIpConfigInfo`.
  - Write `/etc/resolv.conf` in the guest rootfs as a real file: `nameserver <addr>` lines; `options timeout:2 attempts:2`.
  - Overwrite only if the file is absent or starts with the marker line `# Generated by FEX Switch 2 launcher`.
  - Without network, leave the file unchanged. Ensure `/etc/hosts` has `127.0.0.1 localhost` (create only if absent).
  - Log the servers to the launcher log.
- [ ] **Step 6: Socket object.**
  - Add `int HostSocket = -1` and `InetState` (family, type, protocol, nonblocking, bound/listening/connecting/connected flags, `PendingError`, stored emulated options, readiness generation) to `OpenFile`, or to a `std::shared_ptr<InetSocket>` member mirroring `Socket`.
  - Close the host fd in the destructor (last reference). Shared descriptors across fork are one `OpenFile`, so they close once.
  - **`socket(AF_INET, type, proto)`:**
    - Types: STREAM 1 (proto 0 or 6), DGRAM 2 (proto 0 or 17).
    - RAW 3: allowed only with CAP_NET_RAW → else EPERM; proto ICMP 1 maps to the host RAW+ICMP, other protocols → EPROTONOSUPPORT.
    - Datagram ICMP ("ping socket") → EPROTONOSUPPORT, which Linux also returns when `ping_group_range` excludes the group.
    - Create the host socket nonblocking.
    - A host EMFILE/ENFILE (126 limit) → ENFILE (-23), recorded as a deliberate difference.
- [ ] **Step 7: Calls.** Every guest pointer is checked with `Available` before host I/O, and data goes through bounded copies (max 64 KiB per host call).
  - **`bind`:**
    - Translate the address.
    - Port < 1024 without CAP_NET_BIND_SERVICE → EACCES (Linux, `ip_unprivileged_port_start` 1024).
    - INADDR_ANY and loopback pass through. Host errno → Linux.
  - **`listen`:** backlog capped at 4096.
  - **`connect`:**
    - Host connect is nonblocking. EINPROGRESS on a blocking guest socket → wait `Net` for POLLOUT, then read `SO_ERROR` and return it (0 or -errno).
    - A nonblocking guest gets EINPROGRESS (-115). A second connect while in progress → EALREADY (-114). Connect when connected → EISCONN (-106).
    - UDP connect to AF_UNSPEC disconnects.
    - The Linux-shaped sockaddr was rejected by the host (probe), so the translation is mandatory.
  - **`accept` / `accept4`:** host accept on the nonblocking listener; EAGAIN → wait `Net` POLLIN (blocking) or return EAGAIN. Accept4 flags: SOCK_NONBLOCK and SOCK_CLOEXEC only. Peer address translated and copied as in WP4 Step 6.
  - **`sendto` / `send` / `sendmsg` / `sendmmsg`:**
    - The TCP stream rules of `WriteSocket` apply: a blocking send loops until every byte is queued, a signal interrupts it, or an error comes.
    - Host EAGAIN → wait POLLOUT.
    - EPIPE → SIGPIPE unless MSG_NOSIGNAL.
    - `sendmsg` with cmsg on AF_INET: IP_TOS/IP_TTL cmsgs → EINVAL unless mapped. SCM_RIGHTS on TCP → EINVAL (Linux: `cmsg` level SOL_SOCKET type 1 on inet → EINVAL).
  - **`recvfrom` / `recv` / `recvmsg` / `recvmmsg`:**
    - MSG_PEEK, MSG_TRUNC (UDP: return the full datagram length; host support permitting, else record partial), MSG_WAITALL (loop in runtime), MSG_DONTWAIT.
    - UDP over 65507 bytes → EMSGSIZE.
    - Peer address on UDP is translated.
  - **`shutdown`:** host shutdown, then wake Net waiters (`++IOEpoch`).
  - **`getsockname` / `getpeername`:** translate.
  - **`getsockopt` / `setsockopt`:** `MapOption`, then a host call or the emulated store. SO_ERROR returns and clears `PendingError` first, then the host SO_ERROR.
  - **ioctl FIONREAD 0x541b:** host FIONREAD. **SIOCOUTQ 0x5411:** host has none → return 0, recorded as partial.
  - **fcntl O_NONBLOCK:** changes only the guest flag. The host fd stays nonblocking.
- [ ] **Step 8: Readiness.**
  - `PollGuest`, `select` and epoll ask inet descriptors through one function, `int16_t InetPoll(OpenFile&, int16_t Events)`. It calls host `poll` with timeout 0 on that fd and translates POLLIN 1, POLLPRI 2, POLLOUT 4, POLLERR 8, POLLHUP 0x10 and POLLRDHUP 0x2000 (BSD has no RDHUP: derive it from POLLIN with FIONREAD 0 on a TCP socket).
  - **Blocking waits on inet sockets** (read, write, accept, connect, poll, select and epoll with inet members) set `WaitKind::Net` with the list of host fds and events.
    - `Ready()` for Net polls them with timeout 0, at most once per scheduler pass. Cache the result per pass using a pass counter.
    - Any readiness → ready (the call re-runs).
  - **Idle loop** (`RunThreads` around line 400): when any thread waits `Net`, replace `svcSleepThread(t)` with `poll(hostfds, n, t_ms)` (t ≤ 8 ms, as today). Count Net waiters as producers in the "blocked with no remaining producer" check.
  - **Edge-triggered epoll:** bump the item generation when `InetPoll` readiness bits change between polls (as other kinds do, see "Edge-triggered epoll" in `ProcessInventory.md`).
  - **CPU-bound guests:** the preemption window (`RefreshPreemptHints`) already returns to the scheduler every quantum. Net polling happens there, so a busy guest thread cannot starve network readiness.
- [ ] **Step 9: Errors.**
  - Use `g_bsdErrno` (Linux numbers, probe) after every failed host call. Pass it through `Inet::LinuxErrno`.
  - Never read `errno` for host socket results.
- [ ] **Step 10: Run all checks.** Run `make -C Source/Switch2 check-guest-inet check-guest-socket check-guest-memory` and the `guest_nro` build. Update the inventory and add these deliberate differences: no AF_INET6, AF_NETLINK or AF_PACKET (EAFNOSUPPORT); the 126 host sockets limit (ENFILE); TCP_INFO zeroed; SIOCOUTQ 0; no `SO_BINDTODEVICE` other than empty; ping sockets.

## Phase B — Review

At most 5 Sonnet reviewers, one per WP. They run after all of Phase A, are read-only and may run in parallel. Each gets:
- this plan's Global Constraints and Runtime conventions;
- its WP text;
- `git diff` limited to the WP's files.

Output to `.superpowers/sdd/m3-review-wpN.md`, one line per finding: `path:line: severity: problem. fix.` Severities: `blocker` (wrong Linux semantics, crash or fault risk, data loss), `major` (missing step, wrong errno or order), `minor`.

Checklist for every reviewer:
1. Every plan step is implemented or listed as a deviation with a reason.
2. **Fault safety.** No runtime read or write of guest memory without `Available`/`IsCommitted`. No `Perm_None`/`Perm_R` on guest pages. No host pointer arithmetic overflow (`Size > UINT64_MAX - Start` checks).
3. **Errno and order.** Values and order match the plan and Linux 7.3. Spot-check against `man 2 <call>` and kernel sources the reviewer knows.
4. **Blocking.**
   - Every new wait kind is woken somewhere.
   - The idle loop counts it as a producer when an external event (network, another process) can end it.
   - `RestartableSyscall` has the right rule.
   - No host call blocks the runtime thread (WP5: no blocking BSD call; WP1: no unbounded `svcMapPhysicalMemory` loops).
5. **Lifetime.**
   - Fork, exec, exit and close paths release or inherit state (shm attachments, undo lists, robust lists, host sockets, mq descriptors, shared file views).
   - No `shared_ptr` cycles.
6. **Header tests.** Host tests cover the header behavior named in the WP. `make -C Source/Switch2 <check targets>` passes. The `guest_nro` build passes.
7. **Inventory.** Rows and deliberate differences match the code.

Reviewer-specific focus:
- **WP1:** arena granule accounting under split mappings, `SavePrivatePages` skipping uncommitted pages, writeback ordering with `RefreshSharedFileViews`, mremap error order.
- **WP2:** shared-key correctness across fork, wake/requeue counts, PI handoff word values, robust walk limits and fault handling.
- **WP3:** IPC id encoding and stale ids, SEM_UNDO at exit, msg queue byte accounting, mq notify rules, `/dev/shm` permissions and sticky bit.
- **WP4:** record boundaries with rights, autobind, inode-keyed names, ECONNRESET rule, sockopt buffer math.
- **WP5:** translation completeness, no blocking host calls, readiness polling cost per pass, SIGPIPE, network init refcount with FTP.

The orchestrator (Opus) merges the five files into `.superpowers/sdd/m3-review.md`, then starts Phase C.

## Phase C — Final pass, suite and acceptance [Opus]

### Task C1: Apply review findings

- [ ] Fix every `blocker` and `major`, or argue it wrong in `m3-review.md` with evidence.
- [ ] Re-run all host checks and the `guest_nro` build.

### Task C2: Guest suite `CommSuite.c`

**Files:** Create `Source/Switch2/LinuxRuntime/CommSuite.c`. Copy the harness from `ProcSuite.c` lines 1–340: the TAP output, a forked child per test, the 10 s timeout, exit 42 on success, and the `TEST`/`CHECK`/`CHECK_ERRNO`/`SKIP` macros. Add Makefile targets `comm-suite` and `check-comm-suite-native`, built and linked like `proc-suite`.

Cases, by group (each name is `group/name`). Every case runs natively first: `make -C Source/Switch2 check-comm-suite-native`, as uid 1000 and namespace root.

- **mem/:**
  - `mmap_anon_zero`, `mmap_prot_none_reserve_then_commit` (reserve 1 GiB PROT_NONE, mprotect 1 MiB RW, write, read back);
  - `mmap_fixed_noreplace_eexist`, `mmap_hugetlb_enomem`, `mmap_shared_validate_unknown`;
  - `mprotect_unmapped_enomem`, `syscall_into_prot_none_efault` (read() into a PROT_NONE page → EFAULT), `munmap_partial_split`;
  - `mremap_shrink`, `mremap_grow_inplace`, `mremap_maymove_copies`, `mremap_fixed`, `mremap_dontunmap`, `mremap_errors`;
  - `msync_flags`, `msync_writeback_file` (MAP_SHARED on a file in `./scratch`, store, msync, read via read()), `munmap_writeback_file`, `write_then_mapping_sees_it`;
  - `mincore_basic`;
  - `madvise_dontneed_zeroes_private`, `madvise_dontneed_keeps_shared`, `madvise_dontfork`, `madvise_wipeonfork`, `madvise_invalid`;
  - `mlock_rlimit`, `mlockall_flags`;
  - `brk_grow_shrink_64mib`;
  - `memfd_large_sparse` (512 MiB ftruncate, touch 2 pages);
  - `process_vm_readv_child`, `process_vm_writev_child`, `process_vm_eperm` (uid 1000 against a setuid-root-like target: skip when not root);
  - `membarrier_query_register`;
  - `pkey_alloc_enospc`, `mseal_blocks_munmap`, `numa_single_node`, `userfaultfd_enosys`.
- **futex/:**
  - `wait_wake`, `wait_timeout`, `wait_bitset_realtime_abs`;
  - `requeue`, `cmp_requeue_eagain`, `wake_op_all_ops`;
  - `shared_across_fork` (MAP_SHARED anon word, child waits, parent wakes);
  - `lock_pi_unlock_handoff`, `trylock_pi_eagain`, `lock_pi_deadlock`, `unlock_pi_eperm`;
  - `robust_owner_died` (thread holds a robust mutex and exits; `pthread_mutex_lock` returns EOWNERDEAD);
  - `robust_list_len_einval`, `get_robust_list_self`;
  - `waitv_index`, `waitv_eagain`;
  - `futex2_wait_wake`.
- **thread/:**
  - `pthread_mutex_condvar_8_threads`, `pthread_rwlock`, `pthread_barrier`, `pthread_spin`;
  - `sem_open_named` (needs `/dev/shm`), `sem_init_pshared_fork`;
  - `shm_open_mmap_fork`, `dev_shm_sticky`.
- **ipc/:**
  - `shm_create_attach_fork`, `shm_rmid_lifetime`, `shm_double_attach_enomem` (guest-only expectation, see below), `shm_stat_info`;
  - `sem_basic`, `sem_undo_on_exit`, `semtimedop_eagain`, `sem_rmid_eidrm`, `sem_setall_erange`;
  - `msg_types`, `msg_noerror_e2big`, `msg_copy`, `msg_qbytes_block`;
  - `mq_send_receive_priority`, `mq_timedreceive_timeout`, `mq_notify_signal`, `mq_attr_limits`, `mq_unlink_lifetime`;
  - `pipe_direct_packets`, `pipe_setsz_eperm`.
- **unix/:**
  - `stream_path_bind_connect`, `bind_eaddrinuse`, `connect_stale_econnrefused`, `abstract_names`, `autobind`;
  - `dgram_boundaries`, `dgram_trunc`, `dgram_connect_default_peer`, `seqpacket_eor`;
  - `getsockname_getpeername`, `sockopt_basic`, `so_peercred`, `scm_credentials`, `scm_rights_dgram`;
  - `sendmmsg_recvmmsg`, `msg_waitall`, `peer_close_econnreset`, `rdhup_poll`, `epoll_et_socket`.
- **inet/** (127.0.0.1 only, so the suite needs no internet):
  - `tcp_connect_accept`, `tcp_nonblocking_connect_einprogress`, `tcp_so_error_refused`, `tcp_eaddrinuse`, `tcp_bind_privileged_port_eacces` (uid 1000);
  - `tcp_shutdown_eof`, `tcp_epipe_sigpipe`, `tcp_msg_nosignal`, `tcp_peek_fionread`;
  - `tcp_poll_epoll_select`, `tcp_epoll_et`, `tcp_large_transfer_16mib`;
  - `udp_boundaries`, `udp_connected_refused`, `udp_msg_trunc`;
  - `sockopt_map` (SO_REUSEADDR, SO_KEEPALIVE, TCP_NODELAY, SO_RCVBUF round trip);
  - `inet6_eafnosupport` (guest expectation; native Linux supports IPv6, so mark this a guest-only case).
  - `blocking_accept_does_not_stall_other_threads`: a thread blocks in accept while the main thread computes and then connects.

Guest-only expectations (deliberate differences) are selected at run time by `access("/proc/self/fex-switch2", F_OK) == 0`, or another marker the runtime already exposes. Check `ProcSuite.c` for how milestone 2 detected the guest and reuse it. The native run must then pass every case.

### Task C3: Debian tools acceptance

- [ ] **Add packages.** Add to `FetchDebian.py` `PACKAGES`: `"libc-bin"` (`getent`), `"netcat-openbsd"`, `"libbsd0"`, `"libmd0"`, `"xz-utils"`, `"liblzma5"`. Stage them with `StageDebian.py`.
- [ ] **Write `LinuxRuntime/CommTools.sh`** (Bash, TAP-like, exit 42 on success) with these checks:
  1. `xz -T4 -c` of 32 MiB of `/dev/urandom`-seeded data, then decompress and `cmp`: threaded futex workload.
  2. `sort --parallel=4 -S 64M` on 200k lines against `sort` single-threaded.
  3. `nc -lU ./s.sock` server and `nc -U` client round trip, plus `nc -lu`/`nc -u` UDP on 127.0.0.1, plus TCP `nc -l 127.0.0.1 <port>` / `nc 127.0.0.1 <port>` with a 4 MiB payload checked with `cmp`.
  4. `getent hosts localhost` gives 127.0.0.1.
  5. `getent ahostsv4 deb.debian.org` gives at least one address. Skip with a reason when `/etc/resolv.conf` has no nameserver.
  6. A Bash `coproc` and a named pipe (`mkfifo`) round trip.
- [ ] **Write `LinuxRuntime/CommAcceptance.c`** modeled on `ProcessAcceptance.c`. It runs `/bin/comm-suite.elf` and then `CommTools.sh`, and prints `Communication acceptance results: suite N, tools N`.
- [ ] Wire `filesystem-runtime` and `StageDebian.py` to stage `comm-suite.elf`, `comm-acceptance.elf` and `comm-tools.sh`.
- [ ] Run the packaged native reference: `LinuxRuntime/RunPackagedNative.sh build/guest/sd/fex/rootfs /bin/comm-acceptance.elf`. Expected: `suite 42, tools 42`.

### Task C4: Console build and checklist

- [ ] Bump the launcher to `0.14.0` in `GuestProbe.cpp` and `CMakeLists.txt`.
- [ ] Build: `cmake --build Source/Switch2/build/guest --target guest_nro`, then stage the SD bundle.
- [ ] Hand off:
  - the NRO path and the SD bundle path;
  - the program to run (`/bin/comm-acceptance.elf` via the launcher's usual program sidecar);
  - the expected `Communication acceptance results: suite 42, tools 42`;
  - an interactive checklist:
    - `nc -l 127.0.0.1 9000 &` then `nc 127.0.0.1 9000` in Bash;
    - `getent ahostsv4 deb.debian.org`;
    - `xz -T4`;
    - Ctrl+C on a blocked `nc -l`;
    - a TCP connection from a PC to a guest `nc -l 0.0.0.0 9000` (console IP from the FTP screen).
  - Also run `/bin/process-acceptance.elf` and `/bin/fs-suite.elf`, expecting `suite 42, tools 42` and the fs-suite pass count, to catch regressions from the arena change.
- [ ] Debug console failures until acceptance passes. Report host, native and console results separately.

### Task C5: Reconcile and commit

- [ ] Reconcile `CommunicationInventory.md`, record the console evidence, and update `NextMilestone.md` (status, commands with results, artifacts).
- [ ] Commit the milestone once on `nx2` after console acceptance.

## Dependency order

```
WP1 (arena, inventory) ─► WP2 (futex keys use IsShared) ─► WP3 (shm/mq/devshm use arena + GuestMemoryFile)
      ─► WP4 (unix sockets; uses MakeNode, creds) ─► WP5 (inet; shares socket glue with WP4)
      ─► Phase B (5 reviewers, parallel) ─► C1 ─► C2 ─► C3 ─► C4 (console) ─► C5 (commit)
```

## Risks to watch

| Risk | Evidence | Mitigation in plan |
| --- | --- | --- |
| `svcMapPhysicalMemory` granularity below 2 MiB is unverified | Probe mapped 2 MiB only | Commit in 2 MiB granules (WP1). A console failure in C4 shows up as `map` errors in the launcher log. |
| Commit failures under memory pressure | 9.1 GiB total, 4.5 GiB reachable after fragmentation (probe) | `CommitLimit` capped at 6 GiB minus a 1 GiB reserve; ENOMEM to the guest, never a crash |
| Runtime touches uncommitted memory → app killed | m3probe 5: faults are never delivered | `Available` reports 0 for uncommitted pages; reviewer checklist item 2 |
| Network readiness polling cost | One host `poll(…,0)` per pass with Net waiters | Cache per pass; measure in `tcp_large_transfer_16mib` (expect ≥ 50 MiB/s on console) |
| IPC buffers in alias-region memory | Unverified that BSD/FS IPC accept buffers in physical-alias Heap memory | C4 runs fs-suite and the network checks with guest buffers in arena memory. If they fail, stage I/O through a runtime bounce buffer. |
