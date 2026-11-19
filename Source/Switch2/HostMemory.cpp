// SPDX-License-Identifier: MIT
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/MathUtils.h>
#include <FEXCore/fextl/memory_resource.h>

extern "C" {
#include <switch/kernel/svc.h>
#include <switch/result.h>
}

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace fextl::pmr {
std::pmr::memory_resource* get_default_resource() {
  static default_resource Resource;
  return &Resource;
}
} // namespace fextl::pmr

namespace FEXCore::Allocator {
void* VirtualAlloc(size_t Size, bool Execute, bool Commit) {
  return VirtualAlloc(nullptr, Size, Execute, Commit);
}

void* VirtualAlloc(void* Base, size_t Size, bool Execute, bool) {
  if (Base || Execute || !Size) {
    return nullptr;
  }

  constexpr auto PageSize = FEXCore::Utils::FEX_PAGE_SIZE;
  if (Size > SIZE_MAX - (PageSize - 1)) {
    return nullptr;
  }
  const size_t RoundedSize = AlignUp(Size, PageSize);
  void* Ptr = std::aligned_alloc(PageSize, RoundedSize);
  if (Ptr) {
    std::memset(Ptr, 0, RoundedSize);
  }
  return Ptr;
}

bool VirtualProtect(void* Ptr, size_t Size, ProtectOptions Options) {
  // Horizon can change permissions on data pages but cannot make them executable.
  if ((Options & ProtectOptions::Exec) == ProtectOptions::Exec) {
    return false;
  }

  return R_SUCCEEDED(svcSetMemoryPermission(Ptr, Size, static_cast<uint32_t>(Options)));
}

void VirtualFree(void* Ptr, size_t Size) {
  if (!Ptr) return;

  // Heap allocations can contain guard pages with different permissions.
  // Restore those runs individually before handing the pages back to malloc.
  const auto Start = reinterpret_cast<uintptr_t>(Ptr);
  const auto End = Start + AlignUp(Size, FEXCore::Utils::FEX_PAGE_SIZE);
  for (auto Address = Start; Address < End;) {
    MemoryInfo Info {};
    u32 Page {};
    const auto Query = svcQueryMemory(&Info, &Page, Address);
    if (R_FAILED(Query) || Address < Info.addr || Address - Info.addr >= Info.size) {
      LogMan::Msg::EFmt("Could not query FEX heap permissions at {:#x}: {:#x}", Address, Query);
      std::abort();
    }
    const auto RunEnd = std::min(End, Info.addr + Info.size);
    if (Info.perm != Perm_Rw) {
      const auto Restore = svcSetMemoryPermission(reinterpret_cast<void*>(Address), RunEnd - Address, Perm_Rw);
      if (R_FAILED(Restore)) {
        LogMan::Msg::EFmt("Could not restore FEX heap permissions at {:#x}: {:#x}", Address, Restore);
        std::abort();
      }
    }
    Address = RunEnd;
  }
  std::free(Ptr);
}

void VirtualDontNeed(void* Ptr, size_t Size, bool) {
  if (Ptr && Size) {
    std::memset(Ptr, 0, Size);
  }
}
} // namespace FEXCore::Allocator
