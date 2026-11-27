// SPDX-License-Identifier: MIT
#include "Interface/Core/DiskCache.h"

namespace FEXCore::DiskCache {
// The Switch 2 backend currently keeps translated blocks in memory.
void DiskCache::Init(FEXCore::Context::ContextImpl*) {}

std::optional<CodeHitData> DiskCache::Lookup(Core::InternalThreadState*, std::optional<ExecutableFileSectionInfo>, uint64_t,
                                             std::optional<uint64_t>& GuestCodeKey) {
  GuestCodeKey.reset();
  return std::nullopt;
}

void DiskCache::Validate(uint64_t, const CodeHitData&, const CPU::CPUBackend::CompiledCode&, std::optional<ExecutableFileSectionInfo>) {}

bool DiskCache::Store(Core::InternalThreadState*, std::optional<ExecutableFileSectionInfo>, uint64_t, uint64_t, std::span<const uint8_t>,
                      const CPU::CPUBackend::CompiledCode&, std::span<const FEXCore::CPU::Relocation>,
                      const Frontend::Decoder::DecodedBlockInformation*) {
  return false;
}
} // namespace FEXCore::DiskCache
