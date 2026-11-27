// SPDX-License-Identifier: MIT
#include "GuestFileWrite.h"

#include <array>
#include <cassert>
#include <optional>

int main() {
  using Switch2Guest::WriteGuestFileRange;
  constexpr size_t ChunkSize = 65536;
  const std::array<char, ChunkSize * 2 + 3> Bytes {};

  for (const auto FailAt : {-1, 0, 1, 2, 3}) {
    int Calls = 0;
    std::optional<uint64_t> Restored;
    const auto Count = WriteGuestFileRange(100, 120, 200, Bytes,
      [&](uint64_t Begin, uint64_t End) -> int64_t {
        assert(Begin == 100 && End == 200);
        return FailAt == 0 ? -5 : 0;
      },
      [&](uint64_t Offset, std::span<const char> Chunk) -> int64_t {
        assert(Offset == 200 + Calls * ChunkSize);
        assert(Chunk.data() == Bytes.data() + Calls * ChunkSize);
        assert(Chunk.size() == (Calls == 2 ? 3 : ChunkSize));
        return ++Calls == FailAt ? -28 : 0;
      },
      [&](uint64_t Length) { Restored = Length; });
    if (FailAt < 0) {
      assert(Count == static_cast<int64_t>(Bytes.size()) && Calls == 3 && !Restored);
    } else if (!FailAt) {
      assert(Count == -5 && Calls == 0 && Restored == 100);
    } else if (FailAt == 1) {
      assert(Count == -28 && Calls == 1 && Restored == 100);
    } else {
      assert(Count == (FailAt - 1) * static_cast<int64_t>(ChunkSize) && Calls == FailAt && Restored == 200 + static_cast<uint64_t>(Count));
    }
  }

  assert(WriteGuestFileRange(100, 100, 0, std::span(Bytes).first(5),
    [](uint64_t, uint64_t) -> int64_t { return 0; },
    [](uint64_t, std::span<const char>) -> int64_t { return -5; },
    [](uint64_t) { assert(false); }) == -5);
}
