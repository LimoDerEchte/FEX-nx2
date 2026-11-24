# Milestone 3 SDK risks and probe

Milestone 3 covers memory and communication: virtual memory, futex, pipes,
IPC and sockets. This file records what the SDK
(`/opt/devkitpro/libounce`, a libnx fork) decides for milestone 3. It also
covers the EXPERIMENT probe that answers the open questions on the console.
Fill in the **Console result** column from `sdmc:/fex/m3probe.log`.

## Static findings (from headers, `libounce.a` and the runtime)

1. **libounce cannot resume after a CPU fault.**
   - `__libnx_exception_entry` (in `exception.o`, weak) copies the
     context to `__nx_exceptiondump`.
   - It then sets pc to `__libnx_exception_returnentry`, which calls
     `__libnx_exception_handler` and then `svcBreak`.
   - So guard pages, `PROT_NONE` traps, guest SIGSEGV/SIGBUS delivery, and
     write-fault tracking for MAP_SHARED and fork pages need our own
     strong `__libnx_exception_entry`. That entry must edit the kernel
     context and call `svcReturnFromException(0)`.
   - The probe replaces the entry and measures this.
2. **Guest addresses are host pointers.**
   - `GuestAddressSpace` takes guest memory from `aligned_alloc`.
     Limits are `MaxTotal` 96 MiB and `MaxMapping` 32 MiB.
   - MAP_FIXED works only inside regions the runtime already owns.
   - Non-PIE x86-64 binaries (0x400000) and hint addresses
     (0x5555…, 0x7ff…) need a free virtual range there. The probe dumps
     the full map and the region bases.
   - Candidate fixed-address mechanisms: `svcMapPhysicalMemory`
     (alias region only), `svcMapMemory` (stack region), and code memory.
3. **MAP_SHARED needs one physical page at two addresses.**
   - Today `fork` copies private pages at each process switch
     (`SavePrivatePages`). Shared regions are skipped, so they stay
     shared in a single address space.
   - That stops working for file-backed writable MAP_SHARED, memfd
     mapped twice, SysV shm attached twice, and `mremap` of a shared
     region.
   - Candidates: `shmemCreate` + map twice, transfer memory mapped by its
     owner, `svcMapProcessMemory` on the own process handle,
     `svcMapMemory`. The probe tests each one for success, coherence and
     the resulting MemType.
4. **mprotect granularity.**
   - `svcSetMemoryPermission` works on heap memory only. It may refuse
     some permissions (W-only, RX on heap) or block sizes.
   - mimalloc may also back large blocks with 2 MiB pages.
   - The probe tries every permission on one page and one page inside a
     2 MiB block.
5. **Futex.**
   - The runtime implements futex in its cooperative scheduler
     (`Futex`, syscall 202: WAIT/WAKE/BITSET only).
   - Milestone 3 adds REQUEUE, CMP_REQUEUE, WAKE_OP, PI ops and robust
     lists. All of these can stay inside the scheduler.
   - The kernel arbiter (`svcWaitForAddress`/`svcSignalToAddress`) is
     32-bit and per process, plus 64-bit on [19.0.0+]. It matters only
     if guest threads ever run on more than one host thread.
   - The probe measures arbiter timeouts and the host thread cap.
6. **Sockets come from FreeBSD's ABI, not Linux's.**
   - In `sys/socket.h`, `SOL_SOCKET` is `0xffff` (Linux: 1) and
     `AF_INET6` is 28 (Linux: 10).
   - `SO_*`, `MSG_*` (`MSG_DONTWAIT` 0x80, Linux 0x40) and the
     `SOCK_NONBLOCK`/`SOCK_CLOEXEC` bits differ.
   - `sockaddr` has `sin_len`, and errno uses newlib numbers.
   - So every inet syscall needs a translation layer for constants,
     addresses, options, cmsg and errno. Do not pass guest values
     through.
   - The probe binds a Linux-shaped `sockaddr_in` to show what the
     service does with one.
7. **Blocking BSD calls block the host thread.**
   - The guest scheduler runs on one host thread. A blocking
     `accept`/`recv`/`connect`/`poll` would stop every guest process.
   - So host sockets must stay nonblocking. Blocking guest calls become
     scheduler waits, polled with `poll(…, 0)` from `NextTimerDeadline` and
     the preemption loop.
   - Another option is a helper host thread per blocking call. The probe
     checks that a blocked `accept` in a second host thread leaves the
     main thread running, and whether `shutdown`/`close` from the main
     thread wakes it.
8. **AF_UNIX, socketpair, SCM_RIGHTS.**
   - libounce exports `socketpair`, `sendmsg` and `recvmsg`, but the BSD
     service is expected to reject AF_UNIX.
   - The runtime already implements AF_UNIX stream sockets itself
     (`CheckSocket` accepts domain 1, type 1 only).
   - Milestone 3 extends that in-runtime implementation with SOCK_DGRAM,
     SOCK_SEQPACKET, abstract names, SCM_RIGHTS/SCM_CREDENTIALS and
     SO_PEERCRED. TCP descriptor passing is not possible. The probe
     confirms both.
9. **DNS.**
   - Guest glibc resolves names itself: `/etc/resolv.conf` plus UDP/TCP
     to port 53. Host `getaddrinfo` (sfdnsres) is not on that path.
   - So the runtime must write a `resolv.conf` from nifm's DNS servers
     (`nifmGetCurrentIpConfigInfo`). UDP to the outside network must work.
   - The probe resolves `deb.debian.org` and reports the elapsed time,
     which shows network health.
10. **Wildcard bind works.**
    - Horizon on Switch 2 allows binding 0.0.0.0 (console-verified
      2026-11-24 with `BindProbe.cpp`: TCP and UDP, port 0/40001/1337, with
      and without SO_REUSEADDR, loopback, console IP and LAN peers). The
      earlier "forbidden" note was wrong.
    - Guest INADDR_ANY passes straight to the host. See WP5 `bind` in
      [Milestone3Plan.md](Milestone3Plan.md).
11. **Resource limits.**
    - `socketInitialize` has a fixed session count; the launcher uses 3.
      Each session serves one blocking call at a time.
    - The service has a socket table limit and fixed TCP/UDP buffer
      sizes.
    - The probe measures how many sockets one process can open and
      whether 8 sessions are accepted. It also reports default buffer
      sizes and 16 MiB loopback throughput.
12. **Memory budget.**
    - The probe reports `TotalMemorySize`/`UsedMemorySize`, the largest
      single `malloc`, and the total reachable in 16 MiB blocks.
    - These bound guest `MaxTotal` and `RLIMIT_AS`/`overcommit`
      behaviour.

## Probe

- Source: `Source/Switch2/Milestone3Probe.cpp` (EXPERIMENT; not part of the
  launcher).
- Build: `make -C Source/Switch2 m3-probe`.
- Artifact: `Source/Switch2/build/fex-switch2-m3probe.nro`.
- Output: `sdmc:/fex/m3probe.log`, version line `m3probe 5`.
- The probe logs `BEGIN <check>` before each check and flushes after
  every line.
- An unexpected exception inside a check logs
  `EXCEPTION type esr far pc` and the probe moves on to the next check.
- If the app still dies, the last `BEGIN` names the check that killed
  it. Fault checks run last.

| Check | Question | Console result |
| --- | --- | --- |
| address_space | Region bases and sizes, memory totals, `0x400000`/`0x5555…`/`0x7ff…` state, full map | |
| heap_limits | Largest malloc, total reachable | |
| set_memory_permission | Which perms work on one heap page, split behaviour, page inside a 2 MiB block | |
| map_memory_alias | `svcMapMemory` alias works and stays coherent; source state while mapped | |
| map_physical_memory | Physical mapping in alias region; at `0x400000`/`0x10000000`; mprotect on it | |
| shared_memory_two_views | One shmem mapped twice, coherent | |
| transfer_memory_self_map | Owner can map its own tmem (perm 0/R/RW) | |
| map_own_process_memory | `svcMapProcessMemory` on own handle | |
| code_memory_views | jit RW/RX views coherent | |
| threads_and_arbiter | Arbiter timeout accuracy; host thread cap | |
| socket_setup | nifm IP; 8 BSD sessions accepted | |
| socket_families | AF_UNIX, socketpair, AF_INET6, raw/ICMP, flag bits, SEQPACKET, socket cap | |
| tcp_loopback | Nonblocking connect/accept, SO_ERROR, FIONREAD, PEEK, sendmsg iov, SCM_RIGHTS on TCP, options, throughput, shutdown, EPIPE/ECONNRESET, EADDRINUSE, ECONNREFUSED, Linux-shaped sockaddr | |
| udp_loopback | Datagram boundaries, truncation, 60000-byte datagram, connected UDP to closed port | |
| resolver | localhost and deb.debian.org through sfdnsres; hostname | |
| blocking_socket_in_thread | Main thread runs while accept blocks; shutdown/close wakes it | |
| fault_resume_read_none | Own exception entry resumes after a PROT_NONE read; ESR/FAR | |
| fault_preserves_registers | x9–x28 survive a resumed fault (mask 0) | |
| fault_resume_write_readonly | Write to R page; ESR WnR bit | |
| fault_fix_and_retry | Handler raises the permission and the faulting store is retried | |
| fault_latency | Cost per fault round trip | |
| fault_unmapped | Unmapped address fault is delivered | |
| fault_execute_heap | Instruction abort on RW heap delivered and resumable | |
| fault_in_second_thread | Faults on another host thread reach the handler | |
| fault_handler_ipc | Handler can log (file IPC) inside the exception state | |

## Console results

**m3probe 1 (2026-11-23, HOS 20.0.0).** The probe died at
`shared_memory_two_views` before `shmemCreate` logged a result. The
suspected cause is that SVC 0x50 is not permitted, and an unpermitted SVC
raises an exception. m3probe 2 logs the SVC hints, skips unhinted calls,
and abandons a check on an unexpected exception instead of crashing.

Results observed so far:

- **Address space.**
  - The address space is 39-bit (`0x8000000000`). Everything at and
    above `0x8000000000` is Reserved.
  - Linux's default mmap and PIE ranges (`0x5555…`, `0x7f…`) do not
    exist. Guest addresses cannot be host pointers there.
  - Guest mmap placement must stay below 512 GiB. This works only for
    guests that accept any address; PIE and `mmap(NULL)` do.
- **Fixed low addresses.**
  - `0x0`–`0x8000000` is Unmapped, but `svcMapPhysicalMemory` there and
    at `0x10000000` returns `0xdc01` (InvalidMemoryRegion).
  - Physical mapping works only in the alias region (`0x405f800000`,
    128 GiB). Non-PIE executables at `0x400000` cannot load at their
    link address.
- **Memory.**
  - TotalMemorySize is 0x23a000000 (9.1 GiB). The heap region is
    12 GiB, of which libounce manages 8.8 GiB.
  - The largest single `malloc` is 8191 MiB. A total of 4544 MiB was
    reachable in 16 MiB blocks, after the 8 GiB probe had fragmented
    the heap.
  - The runtime's `MaxTotal` of 96 MiB is far below what is available.
- **Page permissions.**
  - `svcSetMemoryPermission` on heap pages accepts none, R and RW at 4 KiB
    granularity, also inside 2 MiB blocks. The kernel splits the
    block.
  - It refuses RX and W-only with `0xd801`. PROT_EXEC guest pages are
    fine because FEX translates them; PROT_WRITE alone must map to RW.
- **svcMapMemory.**
  - It works into the stack region and the alias is coherent.
  - While mapped, the source becomes perm 0 with attr 0x1 (locked). It
    is a move, not a second view, so it is no use for MAP_SHARED.
- **svcMapPhysicalMemory (alias region).**
  - It maps 2 MiB as Heap-type memory and `svcSetMemoryPermission` works
    on it.
  - It is a candidate for a guest-owned arena at stable addresses.

**m3probe 2 and 3 (2026-11-23).** Every memory, thread and socket check
completed (results above and in the check table). Both runs died in
`fault_resume_read_none` right after the page was set to none. m3probe 3
logged no `handler entered` line.

- **The loader forwards faults.** The loader (`kaihen/game/main`, an
  nx-hbloader build) has the standard trampoline. While the NRO runs, its
  `__libnx_exception_entry` branches to the NRO entry (`g_nroAddr`). Our
  `_start` is at `0x8000000` and its crt0 branches to our strong entry at
  `0x8000584`.
- **The SDK entry never runs handler code in the exception state.** It
  copies the context, points sp/pc at a return entry and calls
  `svcReturnFromException(0)` at once. The handler runs afterwards as
  normal code. m3probe 3 logged (file IPC) inside the exception state.
- **Open causes:** IPC in the exception state, `svcReturnFromException`
  (0x28) not permitted, or a null x1 context, which made the old handler
  return `0xf801`.
- **m3probe 4** logs the hints for 0x26/0x28/0x29. It skips the fault
  checks if 0x28 is not hinted. The handler makes no calls, records x1
  and falls back to `UserExceptionContextAddress` when x1 is 0. A new
  last check, `fault_handler_ipc`, logs from inside the handler.

**m3probe 4 (2026-11-23).** SVCs 0x26, 0x28 and 0x29 are hinted. The
run still died in `fault_resume_read_none` with no `faults=` line, though
the handler made no calls. So the cause is not IPC in the exception state
and not a missing 0x28 permission. The remaining causes are:

- the kernel does not deliver user exceptions to this process
  (user exception handlers disabled, or the process is terminated first);
- the entry or the return from it fails.

The loader (`game/main`) and `hen/main` use no debug SVCs, so no debugger
takes the exception.

**m3probe 5** tells these apart with a watcher thread at priority 0x2b.

- The entry's first instructions store x0/x1/sp and set `EntrySeen`
  before any stack use.
- The handler spins (up to 3 s) until the watcher logs the entry and
  releases it.
- Log lines `watcher: entry reached`, then `handler ran`, then
  `main resumed=1` mark each stage.
- If the log ends at `watcher armed`, the kernel kills the process
  without entering user code.

**m3probe 5 (2026-11-23): user exceptions are not delivered.** The log
ends at `watcher rc=0` after `debugger_attached=0 core_mask=0x3f`. The
watcher thread at priority 0x2b was alive and polling every 10 ms. It
never logged `entry reached`, so the process was terminated before the
entry's first instruction ran.

- On this console (HOS 20.0.0, kaihen loader), a CPU fault in any thread
  ends the whole app. No user code runs.
- No debugger is attached.
- `svcReturnFromException` is hinted but unusable.

Milestone 3 consequences:

- **No fault-driven features.** Guest SIGSEGV/SIGBUS from real loads and
  stores, stack guard pages, `PROT_NONE` traps, write-fault copy-on-write
  for `fork`, and dirty tracking for shared files all need fault
  delivery. None of them can work.
- **Never set host `Perm_None`/`Perm_R` on guest memory.** Guest
  protection stays in the runtime's own page table (`Task->Memory`). Host
  pages stay RW.
- Syscalls that take guest buffers check guest flags and return `EFAULT`.
- A guest load or store to a guest-`PROT_NONE` or unmapped page cannot
  raise SIGSEGV. Track this as a known gap: GC write barriers, JVM
  implicit null checks and guard-page stack probing.
- **fork keeps eager private-page copies** (`SavePrivatePages`).
- **MAP_SHARED file writeback** must write the whole range on `msync`,
  `munmap` and exit, because dirty pages cannot be found.
- **Self-modifying code.** FEX's default `SMCChecks=mtrack` depends on
  write faults. The runtime invalidates only on mmap/mprotect/munmap
  (`InvalidateExecutableRange`). Guests that write to RWX pages without
  calling mprotect (JITs) need `SMCChecks=full` or a per-application
  setting. Track this.
- FEXCore's own guard pages (`VirtualProtect`) stay as they are. A hit
  ends the app, which it would on Linux as well.

## Decisions the results drive

- **Fault resume fails (observed, m3probe 5):** keep the explicit
  `Available()` checks. Record guest SIGSEGV as a tracked gap.
- **One aliasing mechanism is coherent and works on heap pages:** use it
  for MAP_SHARED/memfd/SysV shm.
- **No aliasing mechanism works:** keep the single-view model and reject
  double mappings explicitly.
- **Fixed low addresses are mappable:** support non-PIE executables.
  **Not mappable:** keep PIE-only and record it.
- **Linux-shaped sockaddr is rejected or misread:** confirms the
  translation layer is needed. It is needed in any case.
- **Blocked accept in a second host thread leaves the main thread
  running, and shutdown wakes it:** helper-thread blocking is possible.
  **Otherwise:** nonblocking host sockets plus scheduler polling only.
- The measured socket cap and memory totals become the runtime's
  RLIMIT_NOFILE share for sockets and the `MaxTotal` value.
