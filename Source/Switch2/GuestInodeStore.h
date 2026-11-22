// SPDX-License-Identifier: MIT
#pragma once

#include "GuestFileExtents.h"

#include <bit>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Switch2Guest {
struct FileTimestamp {
  int64_t Seconds, Nanoseconds;
};
struct InodeTimestamps {
  FileTimestamp Access, Modify, Change;
};
using XattrValues = std::map<std::string, std::string, std::less<>>;
struct InodeMetadata {
  std::optional<uint32_t> Mode;
  std::optional<std::pair<uint32_t, uint32_t>> Owner;
  std::optional<InodeTimestamps> Times;
  XattrValues Attributes;
  std::optional<std::string> Symlink;
  bool Unlinked {};
  bool ManagedFile {};
  // A directory that exists only in the store, used where Horizon cannot
  // hold the path (see SdPathLimit). Everything below it is store-backed.
  bool OverflowDirectory {};
  std::optional<uint64_t> LogicalSize;
  std::optional<uint64_t> BackingObject;
  std::optional<PackedFileExtents> Extents;
  uint32_t SpecialType {};
};

using InodeNames = std::map<std::string, uint64_t, std::less<>>;
using InodeRecords = std::map<uint64_t, InodeMetadata>;

struct InodeSnapshot {
  static constexpr uint64_t FirstInode = uint64_t {1} << 32;
  // Names are SD paths: a guest path below PATH_MAX plus its SD prefix.
  static constexpr size_t MaxNameBytes = 8191;
  static constexpr size_t MaxBytes = 32 * 1024 * 1024;
  uint64_t Generation {};
  uint64_t NextInode {FirstInode};
  InodeNames Names;
  InodeRecords Records;
  std::set<uint64_t> RetiredBackingObjects;
};

namespace InodeSnapshotFormat {
constexpr uint64_t Magic = 0x0145444f4e495846; // FXINODE followed by version 1.

inline void PutWord(std::vector<uint8_t>& Bytes, uint64_t Value) {
  for (unsigned Shift = 0; Shift < 64; Shift += 8) Bytes.push_back(Value >> Shift);
}

inline void PutString(std::vector<uint8_t>& Bytes, std::string_view String) {
  PutWord(Bytes, String.size());
  Bytes.insert(Bytes.end(), String.begin(), String.end());
}

inline uint64_t Checksum(std::span<const uint8_t> Bytes) {
  uint64_t Hash = 14695981039346656037ULL;
  for (const auto Byte : Bytes) Hash = (Hash ^ Byte) * 1099511628211ULL;
  return Hash;
}

struct Reader {
  std::span<const uint8_t> Bytes;

  bool Word(uint64_t& Value) {
    if (Bytes.size() < 8) return false;
    Value = 0;
    for (unsigned I = 0; I < 8; ++I) Value |= uint64_t {Bytes[I]} << (I * 8);
    Bytes = Bytes.subspan(8);
    return true;
  }

  bool String(std::string& Output, size_t Limit) {
    uint64_t Size {};
    if (!Word(Size) || Size > Limit || Size > Bytes.size()) return false;
    Output.assign(reinterpret_cast<const char*>(Bytes.data()), Size);
    Bytes = Bytes.subspan(Size);
    return true;
  }

  bool Time(FileTimestamp& Time) {
    uint64_t Seconds {}, Nanoseconds {};
    if (!Word(Seconds) || !Word(Nanoseconds) || Nanoseconds >= 1'000'000'000) return false;
    Time = {std::bit_cast<int64_t>(Seconds), static_cast<int64_t>(Nanoseconds)};
    return true;
  }
};
} // namespace InodeSnapshotFormat

inline std::vector<uint8_t> EncodeInodeSnapshot(uint64_t Generation, uint64_t NextInode,
                                               const InodeNames& Names, const InodeRecords& Metadata,
                                               const std::set<uint64_t>& RetiredBackingObjects = {}) {
  using namespace InodeSnapshotFormat;
  std::vector<uint8_t> Bytes;
  PutWord(Bytes, Magic);
  PutWord(Bytes, Generation);
  PutWord(Bytes, NextInode);
  PutWord(Bytes, Names.size());
  // Namespace entries and metadata are separate so hard links can share a
  // record. Unlinked objects and runtime-only inode numbers are omitted.
  std::map<uint64_t, const InodeMetadata*> Linked;
  std::set<uint64_t> NamedInodes;
  for (const auto& [Name, Inode] : Names) {
    NamedInodes.insert(Inode);
    PutString(Bytes, Name);
    PutWord(Bytes, Inode);
    if (Bytes.size() > InodeSnapshot::MaxBytes) return {};
    if (const auto It = Metadata.find(Inode); It != Metadata.end() && !It->second.Unlinked) {
      Linked.try_emplace(Inode, &It->second);
    }
  }
  PutWord(Bytes, Linked.size());
  std::set<uint64_t> LiveBackingObjects;
  for (const auto& [Inode, MetadataEntry] : Linked) {
    const auto& Record = *MetadataEntry;
    if (Record.BackingObject &&
        (!Record.ManagedFile || *Record.BackingObject < InodeSnapshot::FirstInode ||
         *Record.BackingObject >= NextInode || NamedInodes.contains(*Record.BackingObject))) return {};
    if (Record.ManagedFile && !LiveBackingObjects.insert(Record.BackingObject.value_or(Inode)).second) return {};
    PutWord(Bytes, Inode);
    if (Record.OverflowDirectory && (Record.ManagedFile || Record.Symlink || Record.SpecialType)) return {};
    PutWord(Bytes, (Record.Mode ? 1 : 0) | (Record.Times ? 2 : 0) | (Record.Symlink ? 4 : 0) | (Record.ManagedFile ? 8 : 0) | (Record.SpecialType ? 16 : 0) |
                   (Record.LogicalSize ? 32 : 0) | (Record.Owner ? 64 : 0) | (Record.BackingObject ? 128 : 0) | (Record.Extents ? 256 : 0) |
                   (Record.OverflowDirectory ? 512 : 0));
    if (Record.Mode) PutWord(Bytes, *Record.Mode);
    if (Record.Times) {
      for (const auto Time : {Record.Times->Access, Record.Times->Modify, Record.Times->Change}) {
        PutWord(Bytes, std::bit_cast<uint64_t>(Time.Seconds));
        PutWord(Bytes, Time.Nanoseconds);
      }
    }
    if (Record.Symlink) PutString(Bytes, *Record.Symlink);
    if (Record.SpecialType) PutWord(Bytes, Record.SpecialType);
    if (Record.LogicalSize) PutWord(Bytes, *Record.LogicalSize);
    if (Record.Owner) {
      PutWord(Bytes, Record.Owner->first);
      PutWord(Bytes, Record.Owner->second);
    }
    if (Record.BackingObject) PutWord(Bytes, *Record.BackingObject);
    if (Record.Extents) {
      if (!Record.ManagedFile || !Record.LogicalSize || !Record.Extents->Valid()) return {};
      PutWord(Bytes, Record.Extents->Extents().size());
      for (const auto& Extent : Record.Extents->Extents()) {
        PutWord(Bytes, Extent.Begin);
        PutWord(Bytes, Extent.End);
        if (Bytes.size() > InodeSnapshot::MaxBytes) return {};
      }
    }
    PutWord(Bytes, Record.Attributes.size());
    for (const auto& [Name, Value] : Record.Attributes) {
      PutString(Bytes, Name);
      PutString(Bytes, Value);
      if (Bytes.size() > InodeSnapshot::MaxBytes) return {};
    }
    if (Bytes.size() > InodeSnapshot::MaxBytes) return {};
  }
  // The optional trailer records objects whose last name was removed.
  // Older snapshots lack this trailer and remain readable.
  auto Retired = RetiredBackingObjects;
  for (const auto& [Inode, Record] : Metadata) {
    if (Record.ManagedFile && Record.Unlinked) Retired.insert(Record.BackingObject.value_or(Inode));
  }
  if (!Retired.empty()) {
    PutWord(Bytes, Retired.size());
    for (const auto Object : Retired) {
      if (Object < InodeSnapshot::FirstInode || Object >= NextInode || LiveBackingObjects.contains(Object)) return {};
      if (NamedInodes.contains(Object)) {
        const auto Entry = Linked.find(Object);
        if (Entry == Linked.end() || !Entry->second->BackingObject) return {};
      }
      PutWord(Bytes, Object);
      if (Bytes.size() > InodeSnapshot::MaxBytes) return {};
    }
  }
  PutWord(Bytes, Checksum(Bytes));
  if (Bytes.size() > InodeSnapshot::MaxBytes) return {};
  return Bytes;
}

inline std::optional<InodeSnapshot> DecodeInodeSnapshot(std::span<const uint8_t> Bytes) {
  using namespace InodeSnapshotFormat;
  if (Bytes.size() < 48 || Bytes.size() > InodeSnapshot::MaxBytes) return std::nullopt;
  Reader Tail {Bytes.last(8)};
  uint64_t WrittenChecksum {};
  Tail.Word(WrittenChecksum);
  Reader Input {Bytes.first(Bytes.size() - 8)};
  if (Checksum(Input.Bytes) != WrittenChecksum) return std::nullopt;

  InodeSnapshot Snapshot;
  uint64_t Version {}, NameCount {}, RecordCount {};
  if (!Input.Word(Version) || Version != Magic || !Input.Word(Snapshot.Generation) ||
      !Input.Word(Snapshot.NextInode) || Snapshot.NextInode < InodeSnapshot::FirstInode ||
      !Input.Word(NameCount) || NameCount > Input.Bytes.size() / 16) return std::nullopt;

  std::map<uint64_t, bool> Owners;
  for (uint64_t Index = 0; Index < NameCount; ++Index) {
    std::string Path;
    uint64_t Inode {};
    if (!Input.String(Path, InodeSnapshot::MaxNameBytes) || !Path.starts_with('/') || Path.find('\0') != Path.npos ||
        !Input.Word(Inode) || Inode < InodeSnapshot::FirstInode || Inode >= Snapshot.NextInode ||
        !Snapshot.Names.try_emplace(std::move(Path), Inode).second) return std::nullopt;
    Owners.try_emplace(Inode, false);
  }
  if (!Input.Word(RecordCount) || RecordCount > Owners.size()) return std::nullopt;
  std::map<uint64_t, bool> BackingObjects;
  for (uint64_t Index = 0; Index < RecordCount; ++Index) {
    uint64_t Inode {}, Fields {}, AttributeCount {};
    // Symlink, managed file, special node and overflow directory exclude each other.
    if (!Input.Word(Inode) || !Input.Word(Fields) || Fields > 1023 || std::popcount(Fields & (28 | 512)) > 1) return std::nullopt;
    const auto Owner = Owners.find(Inode);
    if (Owner == Owners.end() || Owner->second) return std::nullopt;
    Owner->second = true;
    auto& Record = Snapshot.Records[Inode];
    Record.ManagedFile = (Fields & 8) != 0;
    Record.OverflowDirectory = (Fields & 512) != 0;
    if (Fields & 1) {
      uint64_t Mode {};
      if (!Input.Word(Mode) || Mode > 07777) return std::nullopt;
      Record.Mode = Mode;
    }
    if (Fields & 2) {
      InodeTimestamps Times {};
      if (!Input.Time(Times.Access) || !Input.Time(Times.Modify) || !Input.Time(Times.Change)) return std::nullopt;
      Record.Times = Times;
    }
    if (Fields & 4) {
      std::string Target;
      if (!Input.String(Target, 4095) || Target.empty() || Target.find('\0') != Target.npos) return std::nullopt;
      Record.Symlink = std::move(Target);
    }
    if (Fields & 16) {
      uint64_t Type {};
      if (!Input.Word(Type) || (Type != 0010000 && Type != 0140000)) return std::nullopt;
      Record.SpecialType = Type;
    }
    if (Fields & 32) {
      uint64_t Size {};
      if (!Record.ManagedFile || !Input.Word(Size) || Size > INT64_MAX) return std::nullopt;
      Record.LogicalSize = Size;
    }
    if (Fields & 64) {
      uint64_t UID {}, GID {};
      if (!Input.Word(UID) || !Input.Word(GID) || UID >= UINT32_MAX || GID >= UINT32_MAX) return std::nullopt;
      Record.Owner.emplace(UID, GID);
    }
    if (Fields & 128) {
      uint64_t Object {};
      if (!Record.ManagedFile || !Input.Word(Object) || Object < InodeSnapshot::FirstInode ||
          Object >= Snapshot.NextInode || Owners.contains(Object) || !BackingObjects.try_emplace(Object, true).second) {
        return std::nullopt;
      }
      Record.BackingObject = Object;
    }
    if (Fields & 256) {
      uint64_t ExtentCount {};
      if (!Record.ManagedFile || !Record.LogicalSize || !Input.Word(ExtentCount) ||
          ExtentCount > Input.Bytes.size() / 16) return std::nullopt;
      std::vector<FileExtent> Extents;
      Extents.reserve(ExtentCount);
      for (uint64_t I = 0; I < ExtentCount; ++I) {
        FileExtent Extent {};
        if (!Input.Word(Extent.Begin) || !Input.Word(Extent.End)) return std::nullopt;
        Extents.push_back(Extent);
      }
      Record.Extents.emplace(std::move(Extents));
      if (!Record.Extents->Valid()) return std::nullopt;
    }
    if (!Input.Word(AttributeCount) || AttributeCount > Input.Bytes.size() / 16) return std::nullopt;
    for (uint64_t Attribute = 0; Attribute < AttributeCount; ++Attribute) {
      std::string Name, Value;
      if (!Input.String(Name, 255) || Name.empty() || Name.find('\0') != Name.npos ||
          !Input.String(Value, 65536) || !Record.Attributes.try_emplace(std::move(Name), std::move(Value)).second) {
        return std::nullopt;
      }
    }
  }
  if (!Input.Bytes.empty()) {
    uint64_t RetiredCount {};
    if (!Input.Word(RetiredCount) || RetiredCount != Input.Bytes.size() / 8) return std::nullopt;
    for (uint64_t Index = 0; Index < RetiredCount; ++Index) {
      uint64_t Inode {};
      if (!Input.Word(Inode) || Inode < InodeSnapshot::FirstInode || Inode >= Snapshot.NextInode ||
          BackingObjects.contains(Inode)) return std::nullopt;
      if (Owners.contains(Inode)) {
        const auto Record = Snapshot.Records.find(Inode);
        if (Record == Snapshot.Records.end() || !Record->second.BackingObject ||
            !Snapshot.RetiredBackingObjects.insert(Inode).second) return std::nullopt;
        continue;
      }
      InodeMetadata Record;
      Record.Unlinked = true;
      Record.ManagedFile = true;
      if (!Snapshot.Records.try_emplace(Inode, std::move(Record)).second) return std::nullopt;
    }
    if (!Input.Bytes.empty()) return std::nullopt;
  }
  return Snapshot;
}

inline bool CanReclaimInode(uint64_t Inode, const InodeSnapshot& Older) {
  if (Older.RetiredBackingObjects.contains(Inode)) return true;
  const auto Entry = Older.Records.find(Inode);
  return Entry != Older.Records.end() && Entry->second.ManagedFile && Entry->second.Unlinked;
}

inline std::optional<InodeSnapshot> RecoverInodeSnapshot(std::span<const uint8_t> First,
                                                         std::span<const uint8_t> Second) {
  auto A = DecodeInodeSnapshot(First);
  auto B = DecodeInodeSnapshot(Second);
  if (B && (!A || B->Generation > A->Generation)) return B;
  return A;
}
} // namespace Switch2Guest
