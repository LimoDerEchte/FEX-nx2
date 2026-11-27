// SPDX-License-Identifier: MIT
// Milestone 3 SDK probe (memory and communication).
//
// Answers, on the console, the questions about Horizon and libounce that
// decide how milestone 3 implements mappings, faults, shared memory,
// futexes and sockets. Each check logs "BEGIN <name>" before it runs and
// "key=value" lines after, flushed to sdmc:/fex/m3probe.log, so a check that
// kills the application is the last BEGIN in the log. Risky checks (faults)
// run last.
#include <switch.h>
#include <switch2/memory/heap.h>
#include <switch/services/bsd.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csetjmp>
#include <fcntl.h>
#include <initializer_list>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
FILE* Log;
constexpr const char* LogPath = "sdmc:/fex/m3probe.log";
constexpr const char* Version = "m3probe 5";

void Out(const char* Format, ...) {
  char Line[512];
  va_list Arguments;
  va_start(Arguments, Format);
  std::vsnprintf(Line, sizeof(Line), Format, Arguments);
  va_end(Arguments);
  std::printf("%s\n", Line);
  consoleUpdate(nullptr);
  if (Log) {
    std::fprintf(Log, "%s\n", Line);
    std::fflush(Log);
    fsync(fileno(Log));
  }
}

void Begin(const char* Name) { Out("BEGIN %s", Name); }

const char* MemTypeName(u32 Type) {
  switch (Type & 0xff) {
  case 0x00: return "Unmapped";
  case 0x05: return "Heap";
  case 0x06: return "SharedMem";
  case 0x07: return "WeirdMappedMem";
  case 0x0B: return "MappedMemory";
  case 0x0D: return "TransferMemIsolated";
  case 0x0E: return "TransferMem";
  case 0x0F: return "ProcessMem";
  case 0x10: return "Reserved";
  case 0x14: return "CodeReadOnly";
  case 0x15: return "CodeWritable";
  default: return "other";
  }
}

void Query(const char* Label, uintptr_t Address) {
  MemoryInfo Info {};
  u32 Page {};
  const Result Status = svcQueryMemory(&Info, &Page, Address);
  Out("  %s addr=%#lx rc=%#x base=%#lx size=%#lx type=%#x(%s) perm=%#x attr=%#x", Label, Address, Status, Info.addr,
      Info.size, Info.type, MemTypeName(Info.type), Info.perm, Info.attr);
}

u64 Info(u32 Id, Handle Owner = CUR_PROCESS_HANDLE) {
  u64 Value {};
  const Result Status = svcGetInfo(&Value, Id, Owner, 0);
  return R_SUCCEEDED(Status) ? Value : ~0ULL;
}

// A free block inside [Base, Base + Size), or 0.
uintptr_t FreeIn(uintptr_t Base, u64 Size, u64 Want) {
  for (uintptr_t Address = Base; Address < Base + Size;) {
    MemoryInfo Info {};
    u32 Page {};
    if (R_FAILED(svcQueryMemory(&Info, &Page, Address))) return 0;
    const auto End = Info.addr + Info.size;
    if (Info.type == 0 && End - Address >= Want) return (Address + 0x1fffff) & ~uintptr_t {0x1fffff};
    if (End <= Address) return 0;
    Address = End;
  }
  return 0;
}

// ---- Fault capture -------------------------------------------------------
// libounce's __libnx_exception_entry sends the faulting thread to the
// handler and then to svcBreak: it cannot resume. This probe replaces the
// weak entry and edits the kernel's exception context to resume.

// Kernel exception context: x0-x8, lr, sp, pc, pstate, afsr0, afsr1, esr, far.
struct ExceptionContext {
  u64 X[9];
  u64 Lr, Sp, Pc;
  u32 Pstate, Afsr0, Afsr1, Esr;
  u64 Far;
};
static_assert(offsetof(ExceptionContext, Esr) == 0x6c && offsetof(ExceptionContext, Far) == 0x70);

enum ResumeMode : u32 { NotArmed = 0, SkipInstruction = 1, ReturnToLr = 2, RestorePermission = 3, SkipAndLog = 4 };
volatile u32 FaultMode;
volatile u32 FaultCount;
volatile u64 FaultType, FaultEsr, FaultFar, FaultPc;
volatile uintptr_t FaultFixAddress;
// x1 as the kernel passed it, and the context address used when x1 is 0.
volatile u64 EntryX1;
u64 ExceptionContextAddress;
bool Hinted(unsigned Svc) { return envIsSyscallHinted(Svc); }

bool VirtmemHeld;
void VirtLock() {
  virtmemLock();
  VirtmemHeld = true;
}
void VirtUnlock() {
  VirtmemHeld = false;
  virtmemUnlock();
}

// Unarmed exceptions inside a guarded check jump back to the check runner.
jmp_buf Guard;
volatile bool GuardActive;
[[noreturn]] void RecoverFromException() { longjmp(Guard, 1); }

} // namespace

extern "C" {
alignas(16) char __nx_exception_stack[16 * 1024];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
u32 __nx_exception_ignoredebug = 1;
// Set by the first instructions of the entry, before any stack use.
volatile u64 EntryRawX0, EntryRawX1, EntryRawSp;
volatile u32 EntrySeen;
// While Watching, the handler waits (up to 3 s) for the watcher thread to
// log the entry and set EntryGo before it returns.
volatile u32 Watching, EntryGo;

// Runs on the faulting thread. The entry saves the FP/SIMD registers, so
// the handler may log.
void __libnx_exception_entry();
extern char _start[];
// It runs in the kernel's user-exception state, so it makes no IPC calls
// except in SkipAndLog mode, which tests exactly that.
__attribute__((used)) u32 ProbeExceptionHandler(u64 Type, ExceptionContext* Context) {
  EntryX1 = reinterpret_cast<u64>(Context);
  if (Watching) {
    const u64 Deadline = armGetSystemTick() + armNsToTicks(3'000'000'000);
    while (!EntryGo && armGetSystemTick() < Deadline) asm volatile("yield" ::: "memory");
  }
  if (!Context) Context = reinterpret_cast<ExceptionContext*>(ExceptionContextAddress);
  if (!Context) return 0xf801;
  FaultType = Type;
  FaultEsr = Context->Esr;
  FaultFar = Context->Far;
  FaultPc = Context->Pc;
  FaultCount = FaultCount + 1;
  if (FaultMode == NotArmed) {
    if (!GuardActive) return 0xf801;
    GuardActive = false;
    Context->Pc = reinterpret_cast<u64>(&RecoverFromException);
    return 0;
  }
  switch (FaultMode) {
  case SkipInstruction: Context->Pc += 4; break;
  case SkipAndLog:
    Out("  handler logged type=%#lx esr=%#x far=%#lx pc=%#lx", Type, Context->Esr, Context->Far, Context->Pc);
    Context->Pc += 4;
    break;
  case ReturnToLr: Context->Pc = Context->Lr; break;
  case RestorePermission:
    if (R_FAILED(svcSetMemoryPermission(reinterpret_cast<void*>(FaultFixAddress), 0x1000, Perm_Rw))) return 0xf801;
    break;
  default: return 0xf801;
  }
  return 0;
}

// x0 = exception type, x1 = context. The kernel restores x0-x8, lr, sp and
// pc from the context, so x2-x8 are scratch; x9-x18 and x29 are saved. The
// kernel's entry sp is near the TLS context, so run on the exception stack.
asm(R"(
  .section .text.__libnx_exception_entry, "ax", %progbits
  .global __libnx_exception_entry
  .type __libnx_exception_entry, %function
  .align 2
__libnx_exception_entry:
  adrp x2, EntryRawX0
  str x0, [x2, #:lo12:EntryRawX0]
  adrp x2, EntryRawX1
  str x1, [x2, #:lo12:EntryRawX1]
  mov x3, sp
  adrp x2, EntryRawSp
  str x3, [x2, #:lo12:EntryRawSp]
  mov w3, #1
  adrp x2, EntrySeen
  str w3, [x2, #:lo12:EntrySeen]
  dmb ish
  mov x2, sp
  adrp x3, __nx_exception_stack
  add x3, x3, #:lo12:__nx_exception_stack
  adrp x4, __nx_exception_stack_size
  ldr x4, [x4, #:lo12:__nx_exception_stack_size]
  add x3, x3, x4
  mov sp, x3
  stp x2, x30, [sp, #-16]!
  stp x9, x10, [sp, #-16]!
  stp x11, x12, [sp, #-16]!
  stp x13, x14, [sp, #-16]!
  stp x15, x16, [sp, #-16]!
  stp x17, x18, [sp, #-16]!
  str x29, [sp, #-16]!
  sub sp, sp, #528
  stp q0, q1, [sp, #0]
  stp q2, q3, [sp, #32]
  stp q4, q5, [sp, #64]
  stp q6, q7, [sp, #96]
  stp q8, q9, [sp, #128]
  stp q10, q11, [sp, #160]
  stp q12, q13, [sp, #192]
  stp q14, q15, [sp, #224]
  stp q16, q17, [sp, #256]
  stp q18, q19, [sp, #288]
  stp q20, q21, [sp, #320]
  stp q22, q23, [sp, #352]
  stp q24, q25, [sp, #384]
  stp q26, q27, [sp, #416]
  stp q28, q29, [sp, #448]
  stp q30, q31, [sp, #480]
  mrs x5, fpcr
  mrs x6, fpsr
  add x7, sp, #512
  stp x5, x6, [x7]
  bl ProbeExceptionHandler
  add x7, sp, #512
  ldp x5, x6, [x7]
  msr fpcr, x5
  msr fpsr, x6
  ldp q0, q1, [sp, #0]
  ldp q2, q3, [sp, #32]
  ldp q4, q5, [sp, #64]
  ldp q6, q7, [sp, #96]
  ldp q8, q9, [sp, #128]
  ldp q10, q11, [sp, #160]
  ldp q12, q13, [sp, #192]
  ldp q14, q15, [sp, #224]
  ldp q16, q17, [sp, #256]
  ldp q18, q19, [sp, #288]
  ldp q20, q21, [sp, #320]
  ldp q22, q23, [sp, #352]
  ldp q24, q25, [sp, #384]
  ldp q26, q27, [sp, #416]
  ldp q28, q29, [sp, #448]
  ldp q30, q31, [sp, #480]
  add sp, sp, #528
  ldr x29, [sp], #16
  ldp x17, x18, [sp], #16
  ldp x15, x16, [sp], #16
  ldp x13, x14, [sp], #16
  ldp x11, x12, [sp], #16
  ldp x9, x10, [sp], #16
  ldp x2, x30, [sp], #16
  mov sp, x2
  bl svcReturnFromException
  b .
  .size __libnx_exception_entry, . - __libnx_exception_entry
  .text
)");
}

namespace {

// ---- Address space and memory ------------------------------------------

void CheckAddressSpace() {
  Begin("address_space");
  Out("  hos=%u.%u.%u", HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()), HOSVER_MICRO(hosversionGet()));
  Out("  is_application=%lu debugger=%lu program_id=%#lx", Info(InfoType_IsApplication), Info(InfoType_DebuggerAttached),
      Info(InfoType_ProgramId));
  Out("  aslr=%#lx+%#lx heap=%#lx+%#lx alias=%#lx+%#lx stack=%#lx+%#lx", Info(InfoType_AslrRegionAddress),
      Info(InfoType_AslrRegionSize), Info(InfoType_HeapRegionAddress), Info(InfoType_HeapRegionSize),
      Info(InfoType_AliasRegionAddress), Info(InfoType_AliasRegionSize), Info(InfoType_StackRegionAddress),
      Info(InfoType_StackRegionSize));
  Out("  address_space_size=%#lx alias_extra=%#lx", Info(36), Info(28));
  Out("  total_mem=%#lx used_mem=%#lx total_nonsys=%#lx used_nonsys=%#lx sysres_total=%#lx sysres_used=%#lx",
      Info(InfoType_TotalMemorySize), Info(InfoType_UsedMemorySize), Info(InfoType_TotalNonSystemMemorySize),
      Info(InfoType_UsedNonSystemMemorySize), Info(InfoType_SystemResourceSizeTotal), Info(InfoType_SystemResourceSizeUsed));
  Out("  free_threads=%lu heap_total=%#lx heap_free=%#lx", Info(InfoType_FreeThreadCount), heapGetTotalSize(),
      heapGetFreeSize());
  // Non-PIE x86-64 executables load at 0x400000; FEX uses guest addresses
  // as host pointers, so these must be mappable.
  for (const uintptr_t Address : {uintptr_t {0x10000}, uintptr_t {0x400000}, uintptr_t {0x10000000}, uintptr_t {0x555555554000},
                                  uintptr_t {0x7ff000000000}})
    Query("fixed", Address);
  // The whole map, coalesced by type.
  Out("  map:");
  for (uintptr_t Address = 0;;) {
    MemoryInfo Info {};
    u32 Page {};
    if (R_FAILED(svcQueryMemory(&Info, &Page, Address))) break;
    Out("    %#014lx-%#014lx %s perm=%#x", Info.addr, Info.addr + Info.size, MemTypeName(Info.type), Info.perm);
    if (Info.addr + Info.size <= Address || Info.addr + Info.size == 0) break;
    Address = Info.addr + Info.size;
  }
}

void CheckHeapLimits() {
  Begin("heap_limits");
  // Largest single block, then how much can be allocated in 16 MiB blocks.
  size_t Low = 0, High = size_t {8} << 30;
  while (High - Low > (1 << 20)) {
    const size_t Mid = (Low + High) / 2;
    void* Block = std::malloc(Mid);
    if (Block) {
      Low = Mid;
      std::free(Block);
    } else {
      High = Mid;
    }
  }
  Out("  largest_malloc=%zu MiB", Low >> 20);
  constexpr size_t Chunk = 16 << 20;
  void* Blocks[512] {};
  size_t Count = 0;
  while (Count < 512 && (Blocks[Count] = std::malloc(Chunk))) ++Count;
  Out("  total_in_16MiB_blocks=%zu MiB used_mem=%#lx heap_free=%#lx", Count * 16, Info(InfoType_UsedMemorySize),
      heapGetFreeSize());
  for (size_t Index = 0; Index < Count; ++Index) std::free(Blocks[Index]);
}

void CheckPermissions() {
  Begin("set_memory_permission");
  auto* Page = static_cast<u8*>(std::aligned_alloc(0x1000, 0x4000));
  std::memset(Page, 0x5a, 0x4000);
  Query("heap_page", reinterpret_cast<uintptr_t>(Page));
  for (const u32 Permission : {u32 {Perm_None}, u32 {Perm_R}, u32 {Perm_Rw}, u32 {Perm_Rx}, u32 {Perm_W}}) {
    const Result Status = svcSetMemoryPermission(Page + 0x1000, 0x1000, Permission);
    MemoryInfo Info {};
    u32 PageInfo {};
    svcQueryMemory(&Info, &PageInfo, reinterpret_cast<uintptr_t>(Page + 0x1000));
    Out("  perm=%#x rc=%#x now=%#x split_base=%#lx split_size=%#lx", Permission, Status, Info.perm, Info.addr, Info.size);
  }
  svcSetMemoryPermission(Page + 0x1000, 0x1000, Perm_Rw);
  // 2 MiB-aligned block: does a large region accept page granularity?
  auto* Large = static_cast<u8*>(std::aligned_alloc(0x200000, 0x400000));
  const Result Split = Large ? svcSetMemoryPermission(Large + 0x3000, 0x1000, Perm_None) : 0xffffffff;
  Out("  page_in_2MiB_block rc=%#x", Split);
  if (Large) {
    svcSetMemoryPermission(Large + 0x3000, 0x1000, Perm_Rw);
    std::free(Large);
  }
  std::free(Page);
}

void CheckMapMemory() {
  Begin("map_memory_alias");
  constexpr size_t Size = 0x10000;
  auto* Source = static_cast<u8*>(std::aligned_alloc(0x1000, Size));
  std::memset(Source, 0x11, Size);
  VirtLock();
  void* Destination = virtmemFindStack(Size, 0x1000);
  const Result Status = svcMapMemory(Destination, Source, Size);
  VirtUnlock();
  Out("  svcMapMemory rc=%#x dst=%p", Status, Destination);
  if (R_SUCCEEDED(Status)) {
    Query("source_after", reinterpret_cast<uintptr_t>(Source));
    Query("alias", reinterpret_cast<uintptr_t>(Destination));
    static_cast<u8*>(Destination)[0] = 0x22;
    Out("  alias_write_ok first_byte_via_alias=%#x", static_cast<u8*>(Destination)[0]);
    Out("  unmap rc=%#x source_byte=%#x", svcUnmapMemory(Destination, Source, Size), Source[0]);
  }
  std::free(Source);
}

void CheckPhysicalMemory() {
  Begin("map_physical_memory");
  constexpr size_t Size = 0x200000;
  VirtLock();
  const auto Target = FreeIn(Info(InfoType_AliasRegionAddress), Info(InfoType_AliasRegionSize), 2 * Size);
  const Result Status = Target ? svcMapPhysicalMemory(reinterpret_cast<void*>(Target), Size) : 0xffffffff;
  VirtUnlock();
  Out("  alias_target=%#lx rc=%#x", Target, Status);
  if (R_SUCCEEDED(Status)) {
    Query("physical", Target);
    std::memset(reinterpret_cast<void*>(Target), 0x33, Size);
    const Result Protect = svcSetMemoryPermission(reinterpret_cast<void*>(Target + 0x1000), 0x1000, Perm_None);
    Out("  write_ok set_permission_none rc=%#x", Protect);
    svcSetMemoryPermission(reinterpret_cast<void*>(Target + 0x1000), 0x1000, Perm_Rw);
    Out("  unmap rc=%#x", svcUnmapPhysicalMemory(reinterpret_cast<void*>(Target), Size));
  }
  // Fixed low addresses outside the alias region.
  for (const uintptr_t Address : {uintptr_t {0x400000}, uintptr_t {0x10000000}}) {
    const Result Low = svcMapPhysicalMemory(reinterpret_cast<void*>(Address), Size);
    Out("  fixed=%#lx rc=%#x", Address, Low);
    if (R_SUCCEEDED(Low)) svcUnmapPhysicalMemory(reinterpret_cast<void*>(Address), Size);
  }
}

void CheckSharedMemory() {
  Begin("shared_memory_two_views");
  constexpr size_t Size = 0x10000;
  if (!Hinted(0x50) || !Hinted(0x13)) {
    Out("  skipped: CreateSharedMemory/MapSharedMemory not hinted");
    return;
  }
  SharedMemory First {}, Second {};
  Result Status = shmemCreate(&First, Size, Perm_Rw, Perm_Rw);
  Out("  shmemCreate rc=%#x", Status);
  if (R_FAILED(Status)) return;
  Status = shmemMap(&First);
  Out("  map_first rc=%#x addr=%p", Status, shmemGetAddr(&First));
  shmemLoadRemote(&Second, First.handle, Size, Perm_Rw);
  const Result SecondStatus = shmemMap(&Second);
  Out("  map_second rc=%#x addr=%p", SecondStatus, shmemGetAddr(&Second));
  if (R_SUCCEEDED(Status) && R_SUCCEEDED(SecondStatus)) {
    static_cast<volatile u32*>(shmemGetAddr(&First))[0] = 0x12345678;
    Out("  coherent=%d", static_cast<volatile u32*>(shmemGetAddr(&Second))[0] == 0x12345678);
    Query("view", reinterpret_cast<uintptr_t>(shmemGetAddr(&Second)));
  }
  if (R_SUCCEEDED(SecondStatus)) svcUnmapSharedMemory(Second.handle, shmemGetAddr(&Second), Size);
  shmemClose(&First);
}

void CheckTransferMemory() {
  Begin("transfer_memory_self_map");
  constexpr size_t Size = 0x10000;
  if (!Hinted(0x15) || !Hinted(0x51)) {
    Out("  skipped: CreateTransferMemory/MapTransferMemory not hinted");
    return;
  }
  for (const ::Permission Access : {Perm_None, Perm_R, Perm_Rw}) {
    TransferMemory Owner {}, View {};
    const Result Status = tmemCreate(&Owner, Size, Access);
    if (R_FAILED(Status)) {
      Out("  owner_perm=%#x create rc=%#x", Access, Status);
      continue;
    }
    tmemLoadRemote(&View, Owner.handle, Size, Access);
    const Result Map = tmemMap(&View);
    Out("  owner_perm=%#x create rc=%#x self_map rc=%#x", Access, Status, Map);
    if (R_SUCCEEDED(Map)) {
      Query("view", reinterpret_cast<uintptr_t>(tmemGetAddr(&View)));
      Query("source", reinterpret_cast<uintptr_t>(Owner.src_addr));
      svcUnmapTransferMemory(View.handle, tmemGetAddr(&View), Size);
    }
    tmemClose(&Owner);
  }
}

void CheckProcessMemory() {
  Begin("map_own_process_memory");
  constexpr size_t Size = 0x10000;
  auto* Source = static_cast<u8*>(std::aligned_alloc(0x1000, Size));
  std::memset(Source, 0x44, Size);
  if (!Hinted(0x74)) {
    Out("  skipped: MapProcessMemory not hinted");
    std::free(Source);
    return;
  }
  const Handle Own = envGetOwnProcessHandle();
  VirtLock();
  void* Destination = virtmemFindAslr(Size, 0x1000);
  const Result Status = svcMapProcessMemory(Destination, Own, reinterpret_cast<u64>(Source), Size);
  VirtUnlock();
  Out("  own_handle=%#x rc=%#x", Own, Status);
  if (R_SUCCEEDED(Status)) {
    static_cast<u8*>(Destination)[0] = 0x55;
    Out("  coherent=%d", Source[0] == 0x55);
    Query("view", reinterpret_cast<uintptr_t>(Destination));
    svcUnmapProcessMemory(Destination, Own, reinterpret_cast<u64>(Source), Size);
  }
  std::free(Source);
}

void CheckCodeMemory() {
  Begin("code_memory_views");
  Jit Code {};
  const Result Status = jitCreate(&Code, 0x10000);
  Out("  jitCreate rc=%#x type=%d", Status, static_cast<int>(Code.type));
  if (R_FAILED(Status)) return;
  jitTransitionToWritable(&Code);
  static_cast<volatile u32*>(jitGetRwAddr(&Code))[0] = 0xdeadbeef;
  jitTransitionToExecutable(&Code);
  Out("  rw=%p rx=%p coherent=%d", jitGetRwAddr(&Code), jitGetRxAddr(&Code),
      static_cast<volatile u32*>(jitGetRxAddr(&Code))[0] == 0xdeadbeef);
  Query("rw", reinterpret_cast<uintptr_t>(jitGetRwAddr(&Code)));
  Query("rx", reinterpret_cast<uintptr_t>(jitGetRxAddr(&Code)));
  jitClose(&Code);
}

// ---- Threads and address arbiter ------------------------------------------

volatile u32 ArbiterWord;

void Worker(void* Argument) {
  auto* Counter = static_cast<volatile u32*>(Argument);
  *Counter = *Counter + 1;
  svcWaitForAddress(const_cast<u32*>(&ArbiterWord), ArbitrationType_WaitIfEqual, 0, 2'000'000'000);
}

void CheckThreadsAndArbiter() {
  Begin("threads_and_arbiter");
  const u64 Start = armGetSystemTick();
  const Result Timeout = svcWaitForAddress(const_cast<u32*>(&ArbiterWord), ArbitrationType_WaitIfEqual, 0, 1'000'000);
  Out("  wait_timeout rc=%#x us=%lu", Timeout, armTicksToNs(armGetSystemTick() - Start) / 1000);
  static Thread Threads[64];
  volatile u32 Started = 0;
  int Created = 0;
  Result Last = 0;
  for (; Created < 64; ++Created) {
    Last = threadCreate(&Threads[Created], Worker, const_cast<u32*>(&Started), nullptr, 0x4000, 0x2c, -2);
    if (R_FAILED(Last)) break;
    if (R_FAILED(Last = threadStart(&Threads[Created]))) {
      threadClose(&Threads[Created]);
      break;
    }
  }
  svcSleepThread(100'000'000);
  Out("  host_threads_created=%d last_rc=%#x started=%u free_threads=%lu", Created, Last, Started, Info(InfoType_FreeThreadCount));
  ArbiterWord = 1;
  const Result Signal = svcSignalToAddress(const_cast<u32*>(&ArbiterWord), SignalType_Signal, 0, -1);
  for (int Index = 0; Index < Created; ++Index) {
    threadWaitForExit(&Threads[Index]);
    threadClose(&Threads[Index]);
  }
  Out("  signal rc=%#x all_joined", Signal);
}

// ---- Sockets --------------------------------------------------------------

#define TRY(Label, Expression)                                                                         \
  do {                                                                                                 \
    errno = 0;                                                                                         \
    const long Returned = static_cast<long>(Expression);                                                  \
    Out("  %s -> %ld errno=%d(%s) bsd_errno=%d", Label, Returned, errno, std::strerror(errno), g_bsdErrno); \
  } while (0)

sockaddr_in Loopback(u16 Port) {
  sockaddr_in Address {};
  Address.sin_len = sizeof(Address);
  Address.sin_family = AF_INET;
  Address.sin_port = htons(Port);
  Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return Address;
}

u16 PortOf(int Fd) {
  sockaddr_in Address {};
  socklen_t Length = sizeof(Address);
  getsockname(Fd, reinterpret_cast<sockaddr*>(&Address), &Length);
  return ntohs(Address.sin_port);
}

bool SocketsUp;

void CheckSocketSetup() {
  Begin("socket_setup");
  u32 Address {};
  const bool Nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
  Out("  nifm=%d ip_rc=%#x ip=%#x", Nifm, Nifm ? nifmGetCurrentIpAddress(&Address) : 0, Address);
  SocketInitConfig Config = *socketGetDefaultInitConfig();
  Config.num_bsd_sessions = 8;
  Result Status = socketInitialize(&Config);
  Out("  socketInitialize sessions=8 rc=%#x", Status);
  if (R_FAILED(Status)) {
    Status = socketInitializeDefault();
    Out("  socketInitializeDefault rc=%#x", Status);
  }
  SocketsUp = R_SUCCEEDED(Status);
}

void CheckSocketFamilies() {
  Begin("socket_families");
  int Pair[2] {-1, -1};
  TRY("AF_UNIX stream", socket(AF_UNIX, SOCK_STREAM, 0));
  TRY("socketpair AF_UNIX", socketpair(AF_UNIX, SOCK_STREAM, 0, Pair));
  TRY("AF_INET6 stream", socket(AF_INET6, SOCK_STREAM, 0));
  TRY("AF_INET raw icmp", socket(AF_INET, SOCK_RAW, IPPROTO_ICMP));
  TRY("AF_INET dgram icmp", socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP));
  TRY("SOCK_NONBLOCK|SOCK_CLOEXEC", socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  TRY("SOCK_SEQPACKET", socket(AF_INET, SOCK_SEQPACKET, 0));
  // How many sockets one process may hold.
  int Fds[512];
  int Count = 0;
  while (Count < 512 && (Fds[Count] = socket(AF_INET, SOCK_DGRAM, 0)) >= 0) ++Count;
  Out("  max_sockets=%d errno=%d bsd_errno=%d", Count, errno, g_bsdErrno);
  for (int Index = 0; Index < Count; ++Index) close(Fds[Index]);
}

void CheckTcpLoopback() {
  Begin("tcp_loopback");
  const int Server = socket(AF_INET, SOCK_STREAM, 0);
  auto Address = Loopback(0);
  TRY("bind 127.0.0.1:0", bind(Server, reinterpret_cast<sockaddr*>(&Address), sizeof(Address)));
  TRY("listen", listen(Server, 4));
  const u16 Port = PortOf(Server);
  Out("  port=%u", Port);
  // Nonblocking connect and accept without a pending connection.
  TRY("fcntl server O_NONBLOCK", fcntl(Server, F_SETFL, O_NONBLOCK));
  TRY("accept empty", accept(Server, nullptr, nullptr));
  const int Client = socket(AF_INET, SOCK_STREAM, 0);
  TRY("fcntl client O_NONBLOCK", fcntl(Client, F_SETFL, O_NONBLOCK));
  TRY("fcntl F_GETFL", fcntl(Client, F_GETFL));
  Address = Loopback(Port);
  TRY("connect nonblocking", connect(Client, reinterpret_cast<sockaddr*>(&Address), sizeof(Address)));
  pollfd Wait {Client, POLLOUT, 0};
  TRY("poll POLLOUT", poll(&Wait, 1, 1000));
  Out("  revents=%#x", Wait.revents);
  int Error = -1;
  socklen_t Length = sizeof(Error);
  TRY("SO_ERROR", getsockopt(Client, SOL_SOCKET, SO_ERROR, &Error, &Length));
  Out("  so_error=%d", Error);
  pollfd Listen {Server, POLLIN, 0};
  TRY("poll listener", poll(&Listen, 1, 1000));
  const int Peer = accept(Server, nullptr, nullptr);
  Out("  accepted fd=%d errno=%d", Peer, errno);
  if (Peer < 0) return;
  char Buffer[64] {};
  TRY("recv empty MSG_DONTWAIT", recv(Peer, Buffer, sizeof(Buffer), MSG_DONTWAIT));
  TRY("send 5", send(Client, "hello", 5, 0));
  svcSleepThread(20'000'000);
  int Pending = -1;
  TRY("FIONREAD", ioctl(Peer, FIONREAD, &Pending));
  Out("  fionread=%d", Pending);
  TRY("recv MSG_PEEK", recv(Peer, Buffer, sizeof(Buffer), MSG_PEEK));
  TRY("recv", recv(Peer, Buffer, sizeof(Buffer), 0));
  // Scatter/gather through sendmsg/recvmsg.
  iovec Parts[2] {{const_cast<char*>("ab"), 2}, {const_cast<char*>("cde"), 3}};
  msghdr Message {};
  Message.msg_iov = Parts;
  Message.msg_iovlen = 2;
  TRY("sendmsg 2 iov", sendmsg(Client, &Message, 0));
  svcSleepThread(20'000'000);
  char Head[2] {}, Tail[8] {};
  iovec Into[2] {{Head, 2}, {Tail, 8}};
  msghdr Received {};
  Received.msg_iov = Into;
  Received.msg_iovlen = 2;
  TRY("recvmsg 2 iov", recvmsg(Peer, &Received, 0));
  Out("  recvmsg head=%.2s tail=%.8s", Head, Tail);
  // Descriptor passing over TCP (expected to fail; AF_UNIX stays in the runtime).
  alignas(cmsghdr) char Control[CMSG_SPACE(sizeof(int))] {};
  Message.msg_control = Control;
  Message.msg_controllen = sizeof(Control);
  auto* Header = CMSG_FIRSTHDR(&Message);
  Header->cmsg_level = SOL_SOCKET;
  Header->cmsg_type = SCM_RIGHTS;
  Header->cmsg_len = CMSG_LEN(sizeof(int));
  std::memcpy(CMSG_DATA(Header), &Client, sizeof(int));
  TRY("sendmsg SCM_RIGHTS", sendmsg(Client, &Message, 0));
  // Options.
  int Value = 1;
  TRY("TCP_NODELAY set", setsockopt(Client, IPPROTO_TCP, TCP_NODELAY, &Value, sizeof(Value)));
  TRY("SO_KEEPALIVE set", setsockopt(Client, SOL_SOCKET, SO_KEEPALIVE, &Value, sizeof(Value)));
  for (const int Option : {SO_SNDBUF, SO_RCVBUF, SO_TYPE}) {
    Value = 0;
    Length = sizeof(Value);
    getsockopt(Client, SOL_SOCKET, Option, &Value, &Length);
    Out("  getsockopt %#x = %d", Option, Value);
  }
  Value = 1 << 20;
  TRY("SO_RCVBUF 1MiB", setsockopt(Client, SOL_SOCKET, SO_RCVBUF, &Value, sizeof(Value)));
  Length = sizeof(Value);
  getsockopt(Client, SOL_SOCKET, SO_RCVBUF, &Value, &Length);
  Out("  SO_RCVBUF now=%d", Value);
  timeval Timeout {0, 100000};
  TRY("SO_RCVTIMEO", setsockopt(Peer, SOL_SOCKET, SO_RCVTIMEO, &Timeout, sizeof(Timeout)));
  TRY("SO_REUSEPORT", setsockopt(Client, SOL_SOCKET, SO_REUSEPORT, &(Value = 1), sizeof(Value)));
  // Throughput: 16 MiB through loopback in 64 KiB writes.
  TRY("client blocking", fcntl(Client, F_SETFL, 0));
  TRY("peer blocking", fcntl(Peer, F_SETFL, 0));
  static char Chunk[65536];
  size_t Sent = 0, Got = 0;
  const u64 Start = armGetSystemTick();
  while (Got < (16u << 20)) {
    if (Sent < (16u << 20)) {
      const auto Wrote = send(Client, Chunk, sizeof(Chunk), MSG_DONTWAIT);
      if (Wrote > 0) Sent += Wrote;
    }
    const auto Read = recv(Peer, Chunk, sizeof(Chunk), MSG_DONTWAIT);
    if (Read > 0) Got += Read;
    if (armTicksToNs(armGetSystemTick() - Start) > 20'000'000'000ULL) break;
  }
  const u64 Ms = armTicksToNs(armGetSystemTick() - Start) / 1'000'000;
  Out("  loopback_16MiB got=%zu ms=%lu MiB/s=%lu", Got, Ms, Ms ? (Got >> 20) * 1000 / Ms : 0);
  // Shutdown and peer teardown.
  TRY("shutdown SHUT_WR", shutdown(Client, SHUT_WR));
  TRY("peer recv after SHUT_WR", recv(Peer, Buffer, sizeof(Buffer), 0));
  TRY("close peer", close(Peer));
  svcSleepThread(20'000'000);
  TRY("send after peer close 1", send(Client, "x", 1, MSG_NOSIGNAL));
  svcSleepThread(20'000'000);
  TRY("send after peer close 2", send(Client, "x", 1, MSG_NOSIGNAL));
  TRY("recv after peer close", recv(Client, Buffer, sizeof(Buffer), MSG_DONTWAIT));
  close(Client);
  // Abortive close: SO_LINGER 0 gives the peer ECONNRESET.
  const int Abort = socket(AF_INET, SOCK_STREAM, 0);
  connect(Abort, reinterpret_cast<sockaddr*>(&Address), sizeof(Address));
  svcSleepThread(20'000'000);
  const int AbortPeer = accept(Server, nullptr, nullptr);
  linger Linger {1, 0};
  TRY("SO_LINGER 0", setsockopt(Abort, SOL_SOCKET, SO_LINGER, &Linger, sizeof(Linger)));
  close(Abort);
  svcSleepThread(20'000'000);
  TRY("recv after RST", recv(AbortPeer, Buffer, sizeof(Buffer), MSG_DONTWAIT));
  close(AbortPeer);
  // Errors.
  const int Second = socket(AF_INET, SOCK_STREAM, 0);
  Address = Loopback(Port);
  TRY("bind in use", bind(Second, reinterpret_cast<sockaddr*>(&Address), sizeof(Address)));
  close(Second);
  close(Server);
  const int Refused = socket(AF_INET, SOCK_STREAM, 0);
  Address = Loopback(Port);
  TRY("connect closed port", connect(Refused, reinterpret_cast<sockaddr*>(&Address), sizeof(Address)));
  TRY("send unconnected", send(socket(AF_INET, SOCK_STREAM, 0), "x", 1, 0));
  close(Refused);
  // Address layout: sockaddr_in has sin_len; does the service accept a
  // Linux-shaped address (no sin_len, family as u16)?
  const int Linux = socket(AF_INET, SOCK_STREAM, 0);
  struct {
    u16 Family, Port;
    u32 Address;
    u8 Zero[8];
  } LinuxAddress {AF_INET, 0, htonl(INADDR_LOOPBACK), {}};
  TRY("bind linux-shaped sockaddr", bind(Linux, reinterpret_cast<sockaddr*>(&LinuxAddress), sizeof(LinuxAddress)));
  close(Linux);
}

void CheckUdpLoopback() {
  Begin("udp_loopback");
  const int Receiver = socket(AF_INET, SOCK_DGRAM, 0);
  auto Address = Loopback(0);
  bind(Receiver, reinterpret_cast<sockaddr*>(&Address), sizeof(Address));
  const u16 Port = PortOf(Receiver);
  const int Sender = socket(AF_INET, SOCK_DGRAM, 0);
  Address = Loopback(Port);
  TRY("sendto", sendto(Sender, "datagram", 8, 0, reinterpret_cast<sockaddr*>(&Address), sizeof(Address)));
  char Buffer[64] {};
  sockaddr_in From {};
  socklen_t Length = sizeof(From);
  pollfd Wait {Receiver, POLLIN, 0};
  poll(&Wait, 1, 1000);
  TRY("recvfrom", recvfrom(Receiver, Buffer, sizeof(Buffer), 0, reinterpret_cast<sockaddr*>(&From), &Length));
  Out("  from_port_nonzero=%d len=%u", From.sin_port != 0, Length);
  TRY("sendto 4 bytes", sendto(Sender, "abcd", 4, 0, reinterpret_cast<sockaddr*>(&Address), sizeof(Address)));
  poll(&Wait, 1, 1000);
  TRY("recv truncating to 2", recv(Receiver, Buffer, 2, 0));
  TRY("recv next (truncated rest dropped?)", recv(Receiver, Buffer, sizeof(Buffer), MSG_DONTWAIT));
  TRY("large 60000 datagram", sendto(Sender, std::calloc(1, 60000), 60000, 0, reinterpret_cast<sockaddr*>(&Address), sizeof(Address)));
  close(Receiver);
  // Connected UDP to a closed port: ICMP unreachable as ECONNREFUSED?
  const int Connected = socket(AF_INET, SOCK_DGRAM, 0);
  connect(Connected, reinterpret_cast<sockaddr*>(&Address), sizeof(Address));
  TRY("send connected closed", send(Connected, "x", 1, 0));
  svcSleepThread(50'000'000);
  TRY("recv connected closed", recv(Connected, Buffer, sizeof(Buffer), MSG_DONTWAIT));
  close(Connected);
  close(Sender);
}

void CheckResolver() {
  Begin("resolver");
  for (const char* Name : {"localhost", "deb.debian.org"}) {
    addrinfo Hints {};
    Hints.ai_family = AF_INET;
    Hints.ai_socktype = SOCK_STREAM;
    addrinfo* Result = nullptr;
    const u64 Start = armGetSystemTick();
    const int Status = getaddrinfo(Name, "80", &Hints, &Result);
    char Text[INET_ADDRSTRLEN] = "-";
    if (!Status && Result) inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(Result->ai_addr)->sin_addr, Text, sizeof(Text));
    Out("  %s -> %d (%s) %s ms=%lu", Name, Status, Status ? gai_strerror(Status) : "ok", Text,
        armTicksToNs(armGetSystemTick() - Start) / 1'000'000);
    if (Result) freeaddrinfo(Result);
  }
  char Host[64] {};
  TRY("gethostname", gethostname(Host, sizeof(Host)));
  Out("  hostname=%s", Host);
}

// A socket call that blocks in a second host thread while the main thread
// continues: the runtime's scheduler runs on one host thread.
int BlockingServer = -1;
volatile int BlockingAccepted = -2;
void BlockingAccept(void*) { BlockingAccepted = accept(BlockingServer, nullptr, nullptr); }

void CheckBlockingInThread() {
  Begin("blocking_socket_in_thread");
  BlockingServer = socket(AF_INET, SOCK_STREAM, 0);
  auto Address = Loopback(0);
  bind(BlockingServer, reinterpret_cast<sockaddr*>(&Address), sizeof(Address));
  listen(BlockingServer, 1);
  const u16 Port = PortOf(BlockingServer);
  Thread Waiter {};
  Result Status = threadCreate(&Waiter, BlockingAccept, nullptr, nullptr, 0x8000, 0x2c, -2);
  if (R_SUCCEEDED(Status)) Status = threadStart(&Waiter);
  Out("  thread rc=%#x", Status);
  if (R_FAILED(Status)) return;
  svcSleepThread(200'000'000);
  const u64 Start = armGetSystemTick();
  int Ticks = 0;
  while (armTicksToNs(armGetSystemTick() - Start) < 100'000'000) {
    ++Ticks;
    svcSleepThread(1'000'000);
  }
  Out("  main_thread_ran_while_accept_blocked ticks=%d accepted=%d", Ticks, BlockingAccepted);
  const int Client = socket(AF_INET, SOCK_STREAM, 0);
  Address = Loopback(Port);
  TRY("connect", connect(Client, reinterpret_cast<sockaddr*>(&Address), sizeof(Address)));
  threadWaitForExit(&Waiter);
  threadClose(&Waiter);
  Out("  accepted=%d", BlockingAccepted);
  // Can another thread unblock a blocked accept by closing or shutting down?
  BlockingAccepted = -2;
  Status = threadCreate(&Waiter, BlockingAccept, nullptr, nullptr, 0x8000, 0x2c, -2);
  if (R_SUCCEEDED(Status)) threadStart(&Waiter);
  svcSleepThread(100'000'000);
  TRY("shutdown listener from main", shutdown(BlockingServer, SHUT_RDWR));
  svcSleepThread(200'000'000);
  Out("  after_shutdown accepted=%d", BlockingAccepted);
  if (BlockingAccepted == -2) {
    TRY("close listener from main", close(BlockingServer));
    svcSleepThread(200'000'000);
    Out("  after_close accepted=%d", BlockingAccepted);
  } else {
    close(BlockingServer);
  }
  if (BlockingAccepted != -2) {
    threadWaitForExit(&Waiter);
    threadClose(&Waiter);
  } else {
    Out("  waiter_still_blocked (left running)");
  }
  close(Client);
}

// ---- Faults (last: an unrecoverable fault ends the run) --------------------

// Higher priority than the main thread, so it preempts a spinning handler.
volatile u32 MainResumed;
void Watcher(void*) {
  Out("  watcher armed");
  for (int Step = 0; Step < 300 && !EntrySeen; ++Step) svcSleepThread(10'000'000);
  if (!EntrySeen) {
    Out("  watcher: entry not reached after 3 s (main thread hung outside the entry)");
    return;
  }
  Out("  watcher: entry reached x0=%#lx x1=%#lx sp=%#lx", EntryRawX0, EntryRawX1, EntryRawSp);
  for (int Step = 0; Step < 100 && FaultCount == 0; ++Step) svcSleepThread(10'000'000);
  Out("  watcher: handler ran=%u esr=%#lx far=%#lx pc=%#lx; releasing", FaultCount, FaultEsr, FaultFar, FaultPc);
  EntryGo = 1;
  for (int Step = 0; Step < 200 && !MainResumed; ++Step) svcSleepThread(10'000'000);
  Out("  watcher: main resumed=%u", MainResumed);
}

void CheckFaults() {
  Begin("fault_resume_read_none");
  if (!Hinted(0x28)) {
    Out("  skipped: svcReturnFromException (0x28) not hinted; faults cannot resume");
    return;
  }
  ExceptionContextAddress = Info(InfoType_UserExceptionContextAddress);
  auto* Page = static_cast<volatile u8*>(std::aligned_alloc(0x1000, 0x2000));
  Page[0] = 1;
  Out("  set_none rc=%#x", svcSetMemoryPermission(const_cast<u8*>(Page), 0x1000, Perm_None));
  Out("  entry=%p start=%p user_exception_context=%#lx", reinterpret_cast<void*>(&__libnx_exception_entry),
      reinterpret_cast<void*>(_start), ExceptionContextAddress);
  Out("  debugger_attached=%#lx core_mask=%#lx", Info(InfoType_DebuggerAttached, INVALID_HANDLE), Info(InfoType_CoreMask));
  static Thread WatchThread {};
  Watching = 1;
  Result WatchStatus = threadCreate(&WatchThread, Watcher, nullptr, nullptr, 0x8000, 0x2b, -2);
  if (R_SUCCEEDED(WatchStatus)) WatchStatus = threadStart(&WatchThread);
  Out("  watcher rc=%#x", WatchStatus);
  svcSleepThread(50'000'000);
  FaultCount = 0;
  FaultMode = SkipInstruction;
  const u8 Value = Page[0];
  FaultMode = NotArmed;
  MainResumed = 1;
  if (R_SUCCEEDED(WatchStatus)) {
    threadWaitForExit(&WatchThread);
    threadClose(&WatchThread);
  }
  Watching = 0;
  Out("  faults=%u type=%#lx esr=%#lx far=%#lx expected_far=%p value=%u entry_x1=%#lx", FaultCount, FaultType, FaultEsr,
      FaultFar, Page, Value, EntryX1);

  Begin("fault_preserves_registers");
  // x9-x18 and x19-x28 hold sentinels across a resumed load fault.
  FaultCount = 0;
  FaultMode = SkipInstruction;
  u64 Mismatch = 0;
  asm volatile(
    "mov x9, #9\n mov x10, #10\n mov x11, #11\n mov x12, #12\n mov x13, #13\n"
    "mov x14, #14\n mov x15, #15\n mov x16, #16\n mov x17, #17\n mov x19, #19\n"
    "mov x20, #20\n mov x21, #21\n mov x22, #22\n mov x23, #23\n mov x24, #24\n"
    "mov x25, #25\n mov x26, #26\n mov x27, #27\n mov x28, #28\n"
    "ldrb w8, [%1]\n"
    "mov %0, #0\n"
    "cmp x9, #9\n  cset x8, ne\n orr %0, %0, x8, lsl #9\n"
    "cmp x10, #10\n cset x8, ne\n orr %0, %0, x8, lsl #10\n"
    "cmp x11, #11\n cset x8, ne\n orr %0, %0, x8, lsl #11\n"
    "cmp x12, #12\n cset x8, ne\n orr %0, %0, x8, lsl #12\n"
    "cmp x13, #13\n cset x8, ne\n orr %0, %0, x8, lsl #13\n"
    "cmp x14, #14\n cset x8, ne\n orr %0, %0, x8, lsl #14\n"
    "cmp x15, #15\n cset x8, ne\n orr %0, %0, x8, lsl #15\n"
    "cmp x16, #16\n cset x8, ne\n orr %0, %0, x8, lsl #16\n"
    "cmp x17, #17\n cset x8, ne\n orr %0, %0, x8, lsl #17\n"
    "cmp x19, #19\n cset x8, ne\n orr %0, %0, x8, lsl #19\n"
    "cmp x20, #20\n cset x8, ne\n orr %0, %0, x8, lsl #20\n"
    "cmp x21, #21\n cset x8, ne\n orr %0, %0, x8, lsl #21\n"
    "cmp x22, #22\n cset x8, ne\n orr %0, %0, x8, lsl #22\n"
    "cmp x23, #23\n cset x8, ne\n orr %0, %0, x8, lsl #23\n"
    "cmp x24, #24\n cset x8, ne\n orr %0, %0, x8, lsl #24\n"
    "cmp x25, #25\n cset x8, ne\n orr %0, %0, x8, lsl #25\n"
    "cmp x26, #26\n cset x8, ne\n orr %0, %0, x8, lsl #26\n"
    "cmp x27, #27\n cset x8, ne\n orr %0, %0, x8, lsl #27\n"
    "cmp x28, #28\n cset x8, ne\n orr %0, %0, x8, lsl #28\n"
    : "=&r"(Mismatch)
    : "r"(Page)
    : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x19", "x20", "x21", "x22", "x23",
      "x24", "x25", "x26", "x27", "x28", "cc", "memory");
  FaultMode = NotArmed;
  Out("  faults=%u clobbered_mask=%#lx", FaultCount, Mismatch);

  Begin("fault_resume_write_readonly");
  svcSetMemoryPermission(const_cast<u8*>(Page), 0x1000, Perm_R);
  FaultCount = 0;
  FaultMode = SkipInstruction;
  Page[8] = 7;
  FaultMode = NotArmed;
  Out("  faults=%u esr=%#lx far=%#lx", FaultCount, FaultEsr, FaultFar);

  Begin("fault_fix_and_retry");
  FaultCount = 0;
  FaultFixAddress = reinterpret_cast<uintptr_t>(Page);
  FaultMode = RestorePermission;
  Page[16] = 9;
  FaultMode = NotArmed;
  Out("  faults=%u stored=%u", FaultCount, Page[16]);

  Begin("fault_latency");
  constexpr int Rounds = 1000;
  svcSetMemoryPermission(const_cast<u8*>(Page), 0x1000, Perm_None);
  FaultCount = 0;
  FaultMode = SkipInstruction;
  const u64 Start = armGetSystemTick();
  for (int Index = 0; Index < Rounds; ++Index) (void)Page[Index & 0xff];
  const u64 Ns = armTicksToNs(armGetSystemTick() - Start);
  FaultMode = NotArmed;
  Out("  faults=%u ns_per_fault=%lu", FaultCount, Ns / Rounds);
  svcSetMemoryPermission(const_cast<u8*>(Page), 0x1000, Perm_Rw);

  Begin("fault_unmapped");
  VirtLock();
  auto* Hole = static_cast<volatile u8*>(virtmemFindAslr(0x10000, 0x1000));
  VirtUnlock();
  FaultCount = 0;
  FaultMode = SkipInstruction;
  (void)Hole[0x100];
  FaultMode = NotArmed;
  Out("  faults=%u esr=%#lx far=%#lx expected=%p", FaultCount, FaultEsr, FaultFar, Hole + 0x100);

  Begin("fault_execute_heap");
  auto* Code = static_cast<u32*>(std::aligned_alloc(0x1000, 0x1000));
  Code[0] = 0xd65f03c0; // ret
  FaultCount = 0;
  FaultMode = ReturnToLr;
  reinterpret_cast<void (*)()>(Code)();
  FaultMode = NotArmed;
  Out("  faults=%u esr=%#lx far=%#lx pc=%#lx", FaultCount, FaultEsr, FaultFar, FaultPc);

  Begin("fault_in_second_thread");
  static Thread Faulter {};
  FaultCount = 0;
  FaultMode = SkipInstruction;
  svcSetMemoryPermission(const_cast<u8*>(Page), 0x1000, Perm_None);
  Result Status = threadCreate(&Faulter, [](void* Argument) { (void)static_cast<volatile u8*>(Argument)[0]; }, const_cast<u8*>(Page),
                               nullptr, 0x4000, 0x2c, -2);
  if (R_SUCCEEDED(Status)) Status = threadStart(&Faulter);
  if (R_SUCCEEDED(Status)) {
    threadWaitForExit(&Faulter);
    threadClose(&Faulter);
  }
  FaultMode = NotArmed;
  svcSetMemoryPermission(const_cast<u8*>(Page), 0x1000, Perm_Rw);
  Out("  thread rc=%#x faults=%u", Status, FaultCount);
  std::free(const_cast<u8*>(Page));
  std::free(Code);
}

// Last: logging from the handler makes IPC calls in the user-exception
// state, which may end the run.
void CheckFaultHandlerIpc() {
  Begin("fault_handler_ipc");
  if (!Hinted(0x28)) return Out("  skipped");
  auto* Page = static_cast<volatile u8*>(std::aligned_alloc(0x1000, 0x1000));
  svcSetMemoryPermission(const_cast<u8*>(Page), 0x1000, Perm_None);
  FaultCount = 0;
  FaultMode = SkipAndLog;
  (void)Page[0];
  FaultMode = NotArmed;
  svcSetMemoryPermission(const_cast<u8*>(Page), 0x1000, Perm_Rw);
  Out("  faults=%u", FaultCount);
  std::free(const_cast<u8*>(Page));
}

// Runs one check; an unexpected exception ends only that check.
void Guarded(void (*Check)()) {
  FaultMode = NotArmed;
  if (setjmp(Guard)) {
    Out("  EXCEPTION type=%#lx esr=%#lx far=%#lx pc=%#lx entry_x1=%#lx (check abandoned)", FaultType, FaultEsr, FaultFar,
        FaultPc, EntryX1);
    if (VirtmemHeld) VirtUnlock();
    return;
  }
  GuardActive = true;
  Check();
  GuardActive = false;
}

void CheckSvcHints() {
  Begin("svc_hints");
  static const struct { unsigned Id; const char* Name; } Calls[] = {
    {0x01, "SetHeapSize"}, {0x02, "SetMemoryPermission"}, {0x03, "SetMemoryAttribute"}, {0x04, "MapMemory"},
    {0x05, "UnmapMemory"}, {0x13, "MapSharedMemory"}, {0x14, "UnmapSharedMemory"}, {0x15, "CreateTransferMemory"},
    {0x26, "Break"}, {0x28, "ReturnFromException"}, {0x29, "GetInfo"},
    {0x2c, "MapPhysicalMemory"}, {0x2d, "UnmapPhysicalMemory"}, {0x34, "WaitForAddress"}, {0x35, "SignalToAddress"},
    {0x4b, "CreateCodeMemory"}, {0x4c, "ControlCodeMemory"}, {0x50, "CreateSharedMemory"}, {0x51, "MapTransferMemory"},
    {0x52, "UnmapTransferMemory"}, {0x73, "SetProcessMemoryPermission"}, {0x74, "MapProcessMemory"},
    {0x75, "UnmapProcessMemory"}, {0x77, "MapProcessCodeMemory"}, {0x80, "Unknown80"}};
  for (const auto& Call : Calls) Out("  svc %#04x %-26s hinted=%d", Call.Id, Call.Name, Hinted(Call.Id));
}

} // namespace

int main() {
  consoleInit(nullptr);
  fsdevMountSdmc();
  Log = std::fopen(LogPath, "w");
  Out("%s: milestone 3 SDK probe. Log: %s", Version, LogPath);
  Guarded(CheckSvcHints);
  Guarded(CheckAddressSpace);
  Guarded(CheckHeapLimits);
  Guarded(CheckPermissions);
  Guarded(CheckMapMemory);
  Guarded(CheckPhysicalMemory);
  Guarded(CheckSharedMemory);
  Guarded(CheckTransferMemory);
  Guarded(CheckProcessMemory);
  Guarded(CheckCodeMemory);
  Guarded(CheckThreadsAndArbiter);
  Guarded(CheckSocketSetup);
  if (SocketsUp) {
    Guarded(CheckSocketFamilies);
    Guarded(CheckTcpLoopback);
    Guarded(CheckUdpLoopback);
    Guarded(CheckResolver);
    Guarded(CheckBlockingInThread);
  }
  Guarded(CheckFaults);
  Guarded(CheckFaultHandlerIpc);
  Out("DONE. Press + to exit.");
  PadState Pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&Pad);
  while (appletMainLoop()) {
    padUpdate(&Pad);
    if (padGetButtonsDown(&Pad) & HidNpadButton_Plus) break;
    svcSleepThread(50'000'000);
  }
  if (SocketsUp) socketExit();
  if (Log) std::fclose(Log);
  consoleExit(nullptr);
  return 0;
}
