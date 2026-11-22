// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

namespace Switch2Guest {
template<size_t Capacity>
class GuestLogBuffer {
public:
  void Append(std::string_view Line) {
    const size_t Length = std::min(Line.size(), Capacity - 1);
    while (Used + Length + 1 > Capacity) {
      char Byte;
      do {
        Byte = Bytes[Head];
        Head = (Head + 1) % Capacity;
        --Used;
      } while (Used && Byte != '\n');
      ++Dropped;
    }
    const size_t Tail = (Head + Used) % Capacity;
    const size_t First = std::min(Length, Capacity - Tail);
    if (First) std::memcpy(Bytes.data() + Tail, Line.data(), First);
    if (First < Length) std::memcpy(Bytes.data(), Line.data() + First, Length - First);
    Bytes[(Tail + Length) % Capacity] = '\n';
    Used += Length + 1;
  }

  std::array<std::span<const char>, 2> Contents() const {
    const auto First = std::min(Used, Capacity - Head);
    return {std::span<const char> {Bytes.data() + Head, First}, std::span<const char> {Bytes.data(), Used - First}};
  }

  size_t DroppedLines() const { return Dropped; }
  bool Empty() const { return !Used; }

  void Clear() { Head = Used = Dropped = 0; }

private:
  static_assert(Capacity > 1);
  std::array<char, Capacity> Bytes;
  size_t Head {};
  size_t Used {};
  size_t Dropped {};
};
} // namespace Switch2Guest
