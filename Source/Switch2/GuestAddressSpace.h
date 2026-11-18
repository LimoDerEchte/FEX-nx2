// SPDX-License-Identifier: MIT
#pragma once

#include "GuestElf.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace Switch2Guest {
// Guest addresses are directly usable host pointers. This table tracks Linux
// page permissions and limits fixed overlays to allocations owned by the guest.
class GuestAddressSpace {
public:
  struct Range { uint64_t Start; size_t Size; uint8_t Flags; };

  static constexpr size_t MaxMapping = 32 * 1024 * 1024;
  static constexpr uint8_t Unmapped = 0x80;

  void Register(uint64_t Start, size_t Size, uint8_t Flags) {
    const auto Base = Start & ~(PageSize - 1);
    Regions.emplace_back(Base, RoundUp(Start - Base + Size), Flags);
  }

  uint64_t Allocate(size_t Size, uint8_t Flags) {
    if (!Size || Size > MaxMapping) {
      return 0;
    }
    const size_t Rounded = RoundUp(Size);
    if (Rounded > MaxTotal - OwnedBytes) {
      return 0;
    }
    auto* Memory = std::aligned_alloc(PageSize, Rounded);
    if (!Memory) {
      return 0;
    }
    std::memset(Memory, 0, Rounded);
    const auto Address = reinterpret_cast<uint64_t>(Memory);
    Regions.emplace_back(Address, Rounded, Flags);
    Regions.back().Storage.reset(Memory);
    OwnedBytes += Rounded;
    return Address;
  }

  bool Owns(uint64_t Start, size_t Size) const {
    for (const auto& Region : Regions) {
      if (Region.Storage && Region.Contains(Start, Size)) {
        return true;
      }
    }
    return false;
  }

  bool IsMapped(uint64_t Start, size_t Size) const {
    if (!Size || Size > UINT64_MAX - Start) return false;
    const uint64_t End = Start + Size;
    for (auto Address = Start; Address < End; Address += PageSize) {
      if (!Query(Address).Size) return false;
    }
    return true;
  }

  Range Query(uint64_t Address) const {
    // At a shared ELF boundary, the final segment supplies page permissions.
    for (auto It = Regions.rbegin(); It != Regions.rend(); ++It) {
      const auto& Region = *It;
      if (!Region.Contains(Address, 1)) continue;
      const auto Page = (Address - Region.Base) / PageSize;
      const auto Flags = Region.Pages[Page];
      if (Flags == Unmapped) return {};
      size_t First = Page;
      size_t End = Page + 1;
      while (First && Region.Pages[First - 1] == Flags) --First;
      while (End < Region.Pages.size() && Region.Pages[End] == Flags) ++End;
      uint64_t FirstAddress = Region.Base + First * PageSize;
      uint64_t EndAddress = Region.Base + End * PageSize;
      for (auto Above = Regions.rbegin(); Above != It; ++Above) {
        if (Above->Base > Address) {
          EndAddress = std::min(EndAddress, Above->Base);
        } else {
          FirstAddress = std::max(FirstAddress, Above->Base + Above->Size);
        }
      }
      return {FirstAddress, static_cast<size_t>(EndAddress - FirstAddress), Flags};
    }
    return {};
  }

  bool Set(uint64_t Start, size_t Size, uint8_t Flags, bool OwnedOnly = false) {
    if (!Size || Start % PageSize || Size % PageSize || Size > UINT64_MAX - Start) return false;
    if (OwnedOnly ? !Owns(Start, Size) : !IsMapped(Start, Size)) return false;
    for (auto& Region : Regions) {
      if (OwnedOnly && !Region.Storage) continue;
      const auto First = std::max(Start, Region.Base);
      const auto End = std::min(Start + Size, Region.Base + Region.Size);
      if (First >= End) continue;
      std::fill(Region.Pages.begin() + (First - Region.Base) / PageSize,
                Region.Pages.begin() + (End - Region.Base) / PageSize, Flags);
    }
    if (Flags == Unmapped) {
      std::erase_if(Regions, [&](const auto& Region) {
        if (!Region.Storage || !std::all_of(Region.Pages.begin(), Region.Pages.end(), [](uint8_t Page) { return Page == Unmapped; })) {
          return false;
        }
        OwnedBytes -= Region.Size;
        return true;
      });
    }
    return true;
  }

  static constexpr size_t RoundUp(size_t Size) { return (Size + PageSize - 1) & ~(PageSize - 1); }

private:
  static constexpr size_t MaxTotal = 96 * 1024 * 1024;
  struct Region {
    Region(uint64_t Base, size_t Size, uint8_t Flags)
      : Base {Base}
      , Size {Size}
      , Pages(Size / PageSize, Flags) {}

    bool Contains(uint64_t Start, size_t Length) const {
      return Start >= Base && Start - Base <= Size && Length <= Size - (Start - Base);
    }

    uint64_t Base;
    size_t Size;
    std::vector<uint8_t> Pages;
    std::unique_ptr<void, decltype(&std::free)> Storage {nullptr, &std::free};
  };
  std::vector<Region> Regions;
  size_t OwnedBytes {};
};
} // namespace Switch2Guest
