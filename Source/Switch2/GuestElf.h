// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>

namespace Switch2Guest {
constexpr size_t PageSize = 4096;
constexpr size_t MaxElfSize = 16 * 1024 * 1024;

struct Segment {
  uint64_t Start;
  uint64_t Size;
  uint32_t Flags;
};

struct ElfImage {
  std::unique_ptr<uint8_t, decltype(&std::free)> Storage {nullptr, &std::free};
  std::array<Segment, 16> Segments {};
  size_t SegmentCount {};
  uint64_t Entry {};

  std::span<const Segment> GetSegments() const {
    return {Segments.data(), SegmentCount};
  }
};

// Loads a relocation-free static PIE. Returns an error message on failure.
const char* LoadElf(std::span<const uint8_t> File, ElfImage& Image);
} // namespace Switch2Guest
