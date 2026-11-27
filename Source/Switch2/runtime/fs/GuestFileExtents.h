// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Switch2Guest {
struct FileExtent {
  uint64_t Begin, End;
  bool operator==(const FileExtent&) const = default;
};

// Extents are stored consecutively in backing order. Logical gaps consume
// no backing bytes, and allocation beyond EOF may still have an extent.
class PackedFileExtents {
public:
  explicit PackedFileExtents(std::vector<FileExtent> Extents)
    : Data {std::move(Extents)} {}

  std::span<const FileExtent> Extents() const { return Data; }

  bool Valid() const {
    uint64_t PreviousEnd = 0;
    for (const auto& Extent : Data) {
      if (Extent.Begin < PreviousEnd || Extent.Begin >= Extent.End || Extent.End > INT64_MAX) return false;
      PreviousEnd = Extent.End;
    }
    return true;
  }

  struct Run {
    uint64_t BackingOffset, Length;
    bool Allocated;
  };

  Run At(uint64_t Offset, uint64_t Limit) const {
    uint64_t BackingOffset = 0;
    for (const auto& Extent : Data) {
      if (Offset < Extent.Begin) return {BackingOffset, std::min(Limit, Extent.Begin - Offset), false};
      if (Offset < Extent.End) return {BackingOffset + Offset - Extent.Begin, std::min(Limit, Extent.End - Offset), true};
      BackingOffset += Extent.End - Extent.Begin;
    }
    return {BackingOffset, Limit, false};
  }

  std::optional<uint64_t> Seek(uint64_t Offset, uint64_t Size, bool DataWanted) const {
    if (Offset >= Size) return std::nullopt;
    while (Offset < Size) {
      const auto Run = At(Offset, Size - Offset);
      if (Run.Allocated == DataWanted) return Offset;
      Offset += Run.Length;
    }
    return DataWanted ? std::nullopt : std::optional {Size};
  }

  template<class Read>
  int64_t ReadBytes(uint64_t Offset, std::span<char> Buffer, Read ReadBacking) const {
    size_t Done = 0;
    while (Done < Buffer.size()) {
      const auto Run = At(Offset + Done, Buffer.size() - Done);
      if (Run.Allocated) {
        const auto Chunk = Buffer.subspan(Done, std::min<uint64_t>(Run.Length, 65536));
        const auto Received = ReadBacking(Run.BackingOffset, Chunk);
        if (Received < 0) return Done ? static_cast<int64_t>(Done) : Received;
        if (Received != static_cast<int64_t>(Chunk.size())) {
          if (Received) return Done + Received;
          return Done ? static_cast<int64_t>(Done) : -5;
        }
        Done += Received;
      } else {
        std::memset(Buffer.data() + Done, 0, Run.Length);
        Done += Run.Length;
      }
    }
    return Done;
  }

  template<class Read, class Write, class Overlay>
  int64_t CopyTo(const PackedFileExtents& Destination, std::span<char> Buffer,
                 Read ReadBacking, Write WriteBacking, Overlay Update,
                 uint64_t ShiftBoundary = INT64_MAX, int64_t SourceShift = 0) const {
    if (Buffer.empty()) return -22;
    uint64_t BackingOffset = 0;
    for (const auto& Extent : Destination.Data) {
      for (auto Logical = Extent.Begin; Logical < Extent.End;) {
        const auto Limit = Logical < ShiftBoundary ? std::min(Extent.End, ShiftBoundary) : Extent.End;
        const auto Chunk = Buffer.first(std::min<uint64_t>(Buffer.size(), Limit - Logical));
        const auto Source = Logical < ShiftBoundary ? Logical : static_cast<uint64_t>(static_cast<int64_t>(Logical) + SourceShift);
        const auto Received = ReadBytes(Source, Chunk, ReadBacking);
        if (Received < 0) return Received;
        if (static_cast<uint64_t>(Received) != Chunk.size()) return -5;
        Update(Logical, Chunk);
        if (const auto Error = WriteBacking(BackingOffset, Chunk); Error) return Error;
        BackingOffset += Chunk.size();
        Logical += Chunk.size();
      }
    }
    return 0;
  }

  PackedFileExtents ShiftRange(uint64_t Offset, uint64_t Length, bool Insert) const {
    std::vector<FileExtent> Shifted;
    const auto Tail = Insert ? Offset : Offset + Length;
    for (const auto& Extent : Data) {
      if (Extent.Begin < Offset) Shifted.push_back({Extent.Begin, std::min(Extent.End, Offset)});
      if (Extent.End > Tail) {
        const auto Begin = std::max(Tail, Extent.Begin);
        if (Insert) Shifted.push_back({Begin + Length, Extent.End + Length});
        else Shifted.push_back({Begin - Length, Extent.End - Length});
      }
    }
    return PackedFileExtents {std::move(Shifted)};
  }

  PackedFileExtents Allocate(uint64_t Begin, uint64_t End) const {
    if (Begin >= End) return *this;
    std::vector<FileExtent> Allocated;
    auto Next = Data.begin();
    for (; Next != Data.end() && Next->End < Begin; ++Next) {
      Allocated.push_back(*Next);
    }
    for (; Next != Data.end() && Next->Begin <= End; ++Next) {
      Begin = std::min(Begin, Next->Begin);
      End = std::max(End, Next->End);
    }
    Allocated.push_back({Begin, End});
    Allocated.insert(Allocated.end(), Next, Data.end());
    return PackedFileExtents {std::move(Allocated)};
  }

  PackedFileExtents Remove(uint64_t Begin, uint64_t End) const {
    std::vector<FileExtent> Remaining;
    for (const auto& Extent : Data) {
      if (Extent.End <= Begin || Extent.Begin >= End || Begin >= End) {
        Remaining.push_back(Extent);
        continue;
      }
      if (Extent.Begin < Begin) Remaining.push_back({Extent.Begin, Begin});
      if (Extent.End > End) Remaining.push_back({End, Extent.End});
    }
    return PackedFileExtents {std::move(Remaining)};
  }

private:
  std::vector<FileExtent> Data;
};
} // namespace Switch2Guest
