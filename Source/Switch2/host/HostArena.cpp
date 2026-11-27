// SPDX-License-Identifier: MIT
// Horizon backend for GuestArena: guest memory lives in the alias region,
// where svcMapPhysicalMemory maps zeroed RW memory at fixed addresses
// (m3probe: 2 MiB granules, Heap-type, never faulted).
#include "GuestArena.h"
#include "GuestRuntime.h"

#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace Switch2Guest {
namespace {
constexpr uint64_t SkippedBytes = 1ULL << 30; // leave the first GiB of the alias region to the SDK
constexpr size_t SystemMargin = 64ULL << 20;  // headroom for kernel and service allocations
constexpr size_t MaxCommit = 6ULL << 30;
// The loader hands the NRO almost all memory as heap (console 0.14.0:
// TotalMemorySize 0x23a000000, UsedMemorySize 0x239c28000 at start), which left
// the arena a commit limit of 0. The launcher keeps this much heap for
// FEXCore, the JIT, libnx, FTP and rootfs setup; the rest goes to the arena.
constexpr uint64_t HeapKeep = 2ULL << 30;
constexpr uint64_t HeapGranule = 0x200000;

struct {
  bool Override;
  Result Status;
  uint64_t Original;  // heap size the loader set, restored at exit (0: not shrunk)
  uint64_t Kept;      // heap size after the shrink
} HeapState;

uint64_t Info(InfoType Type) {
  uint64_t Value {};
  svcGetInfo(&Value, Type, CUR_PROCESS_HANDLE, 0);
  return Value;
}

struct HorizonBackend final : ArenaBackend {
  bool Commit(uint64_t Address, size_t Size) override {
    const auto Status = svcMapPhysicalMemory(reinterpret_cast<void*>(Address), Size);
    if (R_FAILED(Status)) {
      char Message[96];
      std::snprintf(Message, sizeof(Message), "Arena commit %#llx+%#zx failed: %#x", static_cast<unsigned long long>(Address), Size, Status);
      WriteLog(Message);
      return false;
    }
    return true;
  }
  bool Decommit(uint64_t Address, size_t Size) override {
    const auto Status = svcUnmapPhysicalMemory(reinterpret_cast<void*>(Address), Size);
    if (R_FAILED(Status)) {
      char Message[96];
      std::snprintf(Message, sizeof(Message), "Arena decommit %#llx+%#zx failed: %#x", static_cast<unsigned long long>(Address), Size, Status);
      WriteLog(Message);
      return false;
    }
    return true;
  }
};
} // namespace

size_t HostArenaCommitLimit() {
  const auto Total = Info(InfoType_TotalMemorySize), Used = Info(InfoType_UsedMemorySize);
  const uint64_t Free = Total > Used ? Total - Used : 0;
  return static_cast<size_t>(std::min<uint64_t>(Free > SystemMargin ? Free - SystemMargin : 0, MaxCommit));
}

std::string HostHeapReport() {
  char Message[192];
  std::snprintf(Message, sizeof(Message), "Heap: loader override=%d shrink rc=%#x, %llu MiB -> %llu MiB; total %llu MiB, used %llu MiB",
                HeapState.Override, HeapState.Status, static_cast<unsigned long long>(HeapState.Original >> 20),
                static_cast<unsigned long long>(HeapState.Kept >> 20),
                static_cast<unsigned long long>(Info(InfoType_TotalMemorySize) >> 20),
                static_cast<unsigned long long>(Info(InfoType_UsedMemorySize) >> 20));
  return Message;
}

// The loader reuses its heap for the next NRO, so the heap it set must be back
// before the launcher returns. Call after the arena has returned its memory.
bool HostHeapRestore() {
  if (!HeapState.Original) return true;
  void* Out {};
  const auto Status = svcSetHeapSize(&Out, HeapState.Original);
  if (R_FAILED(Status)) {
    char Message[96];
    std::snprintf(Message, sizeof(Message), "Heap restore to %#llx failed: %#x", static_cast<unsigned long long>(HeapState.Original), Status);
    WriteLog(Message);
    return false;
  }
  HeapState.Original = 0;
  return true;
}

std::unique_ptr<ArenaBackend> MakeHostArenaBackend(uint64_t& Base, size_t& Size) {
  const auto Alias = Info(InfoType_AliasRegionAddress), AliasSize = Info(InfoType_AliasRegionSize);
  const uint64_t Start = (Alias + SkippedBytes + GuestArena::Granule - 1) & ~(GuestArena::Granule - 1);
  if (!Alias || AliasSize <= SkippedBytes + GuestArena::Granule) return nullptr;
  const size_t Length = (Alias + AliasSize - Start) & ~(GuestArena::Granule - 1);
  virtmemLock();
  const auto* Reservation = virtmemAddReservation(reinterpret_cast<void*>(Start), Length);
  virtmemUnlock();
  if (!Reservation) return nullptr;
  Base = Start;
  Size = Length;
  return std::make_unique<HorizonBackend>();
}
} // namespace Switch2Guest

extern "C" {
extern char* fake_heap_start;
extern char* fake_heap_end;

// Replaces libounce's weak __libnx_initheap (same contract: set
// fake_heap_start/end before malloc starts). It keeps HeapKeep of the heap and
// returns the rest to the kernel so the guest arena can commit it.
void __libnx_initheap() {
  using namespace Switch2Guest;
  const uint64_t HeapBase = Info(InfoType_HeapRegionAddress);
  uint64_t Addr {}, Size {};
  if (envHasHeapOverride()) {
    HeapState.Override = true;
    Addr = reinterpret_cast<uint64_t>(envGetHeapOverrideAddr());
    Size = envGetHeapOverrideSize();
    // Shrink only when the override is the top of the heap, so freeing its
    // end frees nothing the loader still uses.
    MemoryInfo Above {};
    u32 Page {};
    const bool AtTop = Addr >= HeapBase && Size > HeapKeep &&
                       R_SUCCEEDED(svcQueryMemory(&Above, &Page, Addr + Size)) && (Above.type & 0xff) != MemType_Heap;
    if (AtTop) {
      const uint64_t Offset = Addr - HeapBase;
      const uint64_t Target = (Offset + HeapKeep + HeapGranule - 1) & ~(HeapGranule - 1);
      void* Out {};
      HeapState.Status = svcSetHeapSize(&Out, Target);
      if (R_SUCCEEDED(HeapState.Status)) {
        HeapState.Original = Offset + Size;
        Size = Target - Offset;
      }
    }
  } else {
    const uint64_t Total = Info(InfoType_TotalMemorySize), Used = Info(InfoType_UsedMemorySize);
    Size = Total > Used + HeapGranule ? (Total - Used - HeapGranule) & ~(HeapGranule - 1) : 0;
    Size = std::min<uint64_t>(Size ? Size : 0x20000000, HeapKeep);
    void* Out {};
    HeapState.Status = svcSetHeapSize(&Out, Size);
    if (R_FAILED(HeapState.Status)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
    Addr = reinterpret_cast<uint64_t>(Out);
  }
  HeapState.Kept = Size;
  fake_heap_start = reinterpret_cast<char*>(Addr);
  fake_heap_end = reinterpret_cast<char*>(Addr + Size);
}
}
