// SPDX-License-Identifier: MIT
#include <FEXCore/Utils/Allocator.h>
#include <FEXCore/Utils/MathUtils.h>
#include <FEXCore/fextl/memory_resource.h>

extern "C" {
#include <switch/kernel/svc.h>
#include <switch/result.h>
}

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
  if (Ptr) {
    // Restore any guard pages before returning the region to the heap.
    VirtualProtect(Ptr, AlignUp(Size, FEXCore::Utils::FEX_PAGE_SIZE), ProtectOptions::Read | ProtectOptions::Write);
    std::free(Ptr);
  }
}

void VirtualDontNeed(void* Ptr, size_t Size, bool) {
  if (Ptr && Size) {
    std::memset(Ptr, 0, Size);
  }
}
} // namespace FEXCore::Allocator
