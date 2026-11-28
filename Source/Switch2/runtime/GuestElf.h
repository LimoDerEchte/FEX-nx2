// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>

namespace Switch2Guest {
constexpr size_t PageSize = 4096;
constexpr size_t MaxElfSize = 16 * 1024 * 1024;

struct Segment {
  uint64_t Start;
  uint64_t Size;
  uint32_t Flags;
  uint64_t Offset {};
  uint64_t FileSize {};
};

struct ElfImage {
  std::unique_ptr<uint8_t, decltype(&std::free)> Storage {nullptr, &std::free};
  std::array<Segment, 16> Segments {};
  size_t SegmentCount {};
  uint64_t Entry {};
  uint64_t Base {};
  uint64_t ProgramHeaders {};
  uint16_t ProgramHeaderCount {};
  std::string Interpreter;
  std::string Path;
  uint64_t Inode {};
  uint64_t Device {};

  std::span<const Segment> GetSegments() const {
    return {Segments.data(), SegmentCount};
  }
};

// Loads an x86-64 position-independent image and records its interpreter. Returns an error message on failure.
const char* LoadElf(std::span<const uint8_t> File, ElfImage& Image);
} // namespace Switch2Guest
