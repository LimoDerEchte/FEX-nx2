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
using InodeRecords = std::map<uint64_t, InodeMetadata, std::less<>>;

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

namespace InodeSnapshotFormat {
// The fields of one metadata record (shared by snapshots and journal
// frames). False when the record is invalid or the output grew too large.
inline bool PutRecordFields(std::vector<uint8_t>& Bytes, const InodeMetadata& Record) {
  if (Record.OverflowDirectory && (Record.ManagedFile || Record.Symlink || Record.SpecialType)) return false;
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
    if (!Record.ManagedFile || !Record.LogicalSize || !Record.Extents->Valid()) return false;
    PutWord(Bytes, Record.Extents->Extents().size());
    for (const auto& Extent : Record.Extents->Extents()) {
      PutWord(Bytes, Extent.Begin);
      PutWord(Bytes, Extent.End);
      if (Bytes.size() > InodeSnapshot::MaxBytes) return false;
    }
  }
  PutWord(Bytes, Record.Attributes.size());
  for (const auto& [Name, Value] : Record.Attributes) {
    PutString(Bytes, Name);
    PutString(Bytes, Value);
    if (Bytes.size() > InodeSnapshot::MaxBytes) return false;
  }
  return Bytes.size() <= InodeSnapshot::MaxBytes;
}
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
    if (!PutRecordFields(Bytes, Record)) return {};
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

namespace InodeSnapshotFormat {
// Reads the fields PutRecordFields wrote into a fresh Record. Owners and
// BackingObjects, when given, check backing objects against the names of a
// whole snapshot.
inline bool ReadRecordFields(Reader& Input, InodeMetadata& Record, uint64_t NextInode, const std::map<uint64_t, bool>* Owners,
                             std::map<uint64_t, bool>* BackingObjects) {
  uint64_t Fields {}, AttributeCount {};
  // Symlink, managed file, special node and overflow directory exclude each other.
  if (!Input.Word(Fields) || Fields > 1023 || std::popcount(Fields & (28 | 512)) > 1) return false;
  Record.ManagedFile = (Fields & 8) != 0;
  Record.OverflowDirectory = (Fields & 512) != 0;
  if (Fields & 1) {
    uint64_t Mode {};
    if (!Input.Word(Mode) || Mode > 07777) return false;
    Record.Mode = Mode;
  }
  if (Fields & 2) {
    InodeTimestamps Times {};
    if (!Input.Time(Times.Access) || !Input.Time(Times.Modify) || !Input.Time(Times.Change)) return false;
    Record.Times = Times;
  }
  if (Fields & 4) {
    std::string Target;
    if (!Input.String(Target, 4095) || Target.empty() || Target.find('\0') != Target.npos) return false;
    Record.Symlink = std::move(Target);
  }
  if (Fields & 16) {
    uint64_t Type {};
    if (!Input.Word(Type) || (Type != 0010000 && Type != 0140000)) return false;
    Record.SpecialType = Type;
  }
  if (Fields & 32) {
    uint64_t Size {};
    if (!Record.ManagedFile || !Input.Word(Size) || Size > INT64_MAX) return false;
    Record.LogicalSize = Size;
  }
  if (Fields & 64) {
    uint64_t UID {}, GID {};
    if (!Input.Word(UID) || !Input.Word(GID) || UID >= UINT32_MAX || GID >= UINT32_MAX) return false;
    Record.Owner.emplace(UID, GID);
  }
  if (Fields & 128) {
    uint64_t Object {};
    if (!Record.ManagedFile || !Input.Word(Object) || Object < InodeSnapshot::FirstInode ||
        Object >= NextInode || (Owners && Owners->contains(Object)) || (BackingObjects && !BackingObjects->try_emplace(Object, true).second)) {
      return false;
    }
    Record.BackingObject = Object;
  }
  if (Fields & 256) {
    uint64_t ExtentCount {};
    if (!Record.ManagedFile || !Record.LogicalSize || !Input.Word(ExtentCount) ||
        ExtentCount > Input.Bytes.size() / 16) return false;
    std::vector<FileExtent> Extents;
    Extents.reserve(ExtentCount);
    for (uint64_t I = 0; I < ExtentCount; ++I) {
      FileExtent Extent {};
      if (!Input.Word(Extent.Begin) || !Input.Word(Extent.End)) return false;
      Extents.push_back(Extent);
    }
    Record.Extents.emplace(std::move(Extents));
    if (!Record.Extents->Valid()) return false;
  }
  if (!Input.Word(AttributeCount) || AttributeCount > Input.Bytes.size() / 16) return false;
  for (uint64_t Attribute = 0; Attribute < AttributeCount; ++Attribute) {
    std::string Name, Value;
    if (!Input.String(Name, 255) || Name.empty() || Name.find('\0') != Name.npos ||
        !Input.String(Value, 65536) || !Record.Attributes.try_emplace(std::move(Name), std::move(Value)).second) {
      return false;
    }
  }
  return true;
}
} // namespace InodeSnapshotFormat

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
    uint64_t Inode {};
    if (!Input.Word(Inode)) return std::nullopt;
    const auto Owner = Owners.find(Inode);
    if (Owner == Owners.end() || Owner->second) return std::nullopt;
    Owner->second = true;
    if (!ReadRecordFields(Input, Snapshot.Records[Inode], Snapshot.NextInode, &Owners, &BackingObjects)) return std::nullopt;
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
// ---------------------------------------------------------------------------
// Write-ahead journal.
//
// Saving the store used to encode the whole snapshot (every name and record)
// for each metadata change, so N changes cost O(N^2) bytes written to the SD
// card: the Ubuntu RootFS extraction (about 40k entries) slowed to a stop.
// Now a save appends one journal frame with only the names, records and
// retired objects that changed since the previous save, as SQLite's WAL and
// ext4's journal do. A full snapshot (a checkpoint) is written into the older
// of the two store slots when the journal passes JournalCheckpointBytes, when
// a change was not recorded per key, at the end of a guest session, and at
// load after a crash; the journal is then emptied. Loading decodes the newest
// checkpoint and replays the frames that follow it, and the result is
// encoded and decoded as a snapshot again, so a replayed store passes the
// same checks as a checkpoint.

inline constexpr uint64_t JournalCheckpointBytes = 4 * 1024 * 1024;

// A map that records every key changed through it since ClearTouched, so a
// save can write only those keys. Lookups give const iterators: a change has
// to go through operator[], at, Edit, emplace, erase, extract, insert or
// merge, which record the key. With KeepOriginals the value a key had before
// its first change is kept (a save needs the old inode of a name). swap and
// assignment from another map record nothing per key; they mark the whole
// map changed, and the next save writes a checkpoint.
template<typename Key, typename Value, bool KeepOriginals = false>
class TrackedMap {
public:
  using Base = std::map<Key, Value, std::less<>>;
  using const_iterator = typename Base::const_iterator;
  using iterator = const_iterator;
  using node_type = typename Base::node_type;
  using value_type = typename Base::value_type;
  using key_type = Key;
  using mapped_type = Value;

  TrackedMap() = default;
  TrackedMap(const TrackedMap&) = default;
  TrackedMap(TrackedMap&&) = default;
  TrackedMap& operator=(const TrackedMap& Other) {
    Items = Other.Items;
    All = true;
    return *this;
  }
  TrackedMap& operator=(TrackedMap&& Other) {
    Items = std::move(Other.Items);
    All = true;
    return *this;
  }

  // Replaces the contents without recording a change (loading the store).
  void Reset(Base Contents) {
    Items = std::move(Contents);
    ClearTouched();
  }
  const Base& Map() const { return Items; }

  const_iterator begin() const { return Items.begin(); }
  const_iterator end() const { return Items.end(); }
  size_t size() const { return Items.size(); }
  bool empty() const { return Items.empty(); }
  template<typename K>
  const_iterator find(const K& Wanted) const {
    return Items.find(Wanted);
  }
  template<typename K>
  const_iterator lower_bound(const K& Wanted) const {
    return Items.lower_bound(Wanted);
  }
  template<typename K>
  bool contains(const K& Wanted) const {
    return Items.contains(Wanted);
  }
  const Value& at(const Key& Wanted) const { return Items.at(Wanted); }

  Value& operator[](const Key& Wanted) {
    Touch(Wanted);
    return Items[Wanted];
  }
  Value& at(const Key& Wanted) {
    Touch(Wanted);
    return Items.at(Wanted);
  }
  // Mutable access through an iterator from find or lower_bound.
  Value& Edit(const_iterator Position) {
    Touch(Position->first);
    return const_cast<Value&>(Position->second);
  }
  template<typename... Arguments>
  std::pair<const_iterator, bool> emplace(Arguments&&... Values) {
    auto Result = Items.emplace(std::forward<Arguments>(Values)...);
    if (Result.second) TouchAbsent(Result.first->first);
    return {Result.first, Result.second};
  }
  const_iterator erase(const_iterator Position) {
    Touch(Position->first);
    return Items.erase(Position);
  }
  template<typename K>
  size_t erase(const K& Wanted) {
    const auto Position = Items.find(Wanted);
    if (Position == Items.end()) return 0;
    Touch(Position->first);
    Items.erase(Position);
    return 1;
  }
  template<typename K>
  node_type extract(const K& Wanted) {
    const auto Position = Items.find(Wanted);
    if (Position == Items.end()) return {};
    Touch(Position->first);
    return Items.extract(Position);
  }
  void insert(node_type&& Node) {
    if (Node.empty()) return;
    Touch(Node.key());
    Items.insert(std::move(Node));
  }
  // Moves every entry of Other whose key is not here yet (std::map::merge).
  void merge(TrackedMap& Other) {
    for (const auto& Entry : Other.Items) {
      if (!Items.contains(Entry.first)) Touch(Entry.first);
    }
    Items.merge(Other.Items);
    Other.All = true;
  }
  void swap(TrackedMap& Other) {
    Items.swap(Other.Items);
    All = Other.All = true;
  }

  bool AllTouched() const { return All; }
  const std::set<Key, std::less<>>& Touched() const { return TouchedKeys; }
  // The value each touched key had before its first change (KeepOriginals).
  const std::map<Key, std::optional<Value>, std::less<>>& Originals() const { return OriginalValues; }
  void ClearTouched() {
    TouchedKeys.clear();
    OriginalValues.clear();
    All = false;
  }

private:
  void Touch(const Key& Changed) {
    if (All || !TouchedKeys.insert(Changed).second) return;
    if constexpr (KeepOriginals) {
      const auto Position = Items.find(Changed);
      OriginalValues.emplace(Changed, Position == Items.end() ? std::nullopt : std::optional<Value> {Position->second});
    }
  }
  void TouchAbsent(const Key& Changed) {
    if (All || !TouchedKeys.insert(Changed).second) return;
    if constexpr (KeepOriginals) OriginalValues.emplace(Changed, std::nullopt);
  }

  Base Items;
  std::set<Key, std::less<>> TouchedKeys;
  std::map<Key, std::optional<Value>, std::less<>> OriginalValues;
  bool All {};
};

using TrackedNames = TrackedMap<std::string, uint64_t, true>;
using TrackedRecords = TrackedMap<uint64_t, InodeMetadata>;

// One journal frame: the store at Generation, given the store at
// Generation - 1. Records are written only for inodes that have a name and
// are not unlinked (as in a snapshot). Retired objects are the snapshot's
// retired set: the explicit retired backing objects plus the object of every
// unlinked managed file.
struct InodeJournalFrame {
  uint64_t Generation {}, NextInode {};
  std::vector<std::pair<std::string, uint64_t>> NamePuts;
  std::vector<std::string> NameRemovals;
  std::vector<std::pair<uint64_t, InodeMetadata>> RecordPuts;
  std::vector<uint64_t> RecordRemovals;
  std::vector<uint64_t> RetiredAdds, RetiredRemovals;
};

// What the store files hold, in the terms the writer needs to find out
// which frame entries a change produces.
struct InodeJournalView {
  std::map<uint64_t, int64_t> Links;          // names per inode
  std::set<uint64_t> Records;                 // inodes written as records
  std::map<uint64_t, uint64_t> UnlinkedObjects; // unlinked managed record -> its object (in-memory keys)
  std::map<uint64_t, int64_t> ObjectRefs;     // object -> unlinked records naming it
  std::set<uint64_t> Explicit;                // explicit retired backing objects
  std::set<uint64_t> Retired;                 // the retired set as written

  void Rebuild(const std::map<std::string, uint64_t, std::less<>>& Names, const std::map<uint64_t, InodeMetadata, std::less<>>& Metadata,
               const std::set<uint64_t>& RetiredBackingObjects) {
    *this = {};
    for (const auto& Entry : Names) ++Links[Entry.second];
    for (const auto& [Inode, Record] : Metadata) {
      if (!Record.Unlinked && Links.contains(Inode)) Records.insert(Inode);
      if (Record.ManagedFile && Record.Unlinked) {
        const auto Object = Record.BackingObject.value_or(Inode);
        UnlinkedObjects.emplace(Inode, Object);
        ++ObjectRefs[Object];
      }
    }
    Explicit = RetiredBackingObjects;
    Retired = Explicit;
    for (const auto& Entry : ObjectRefs) Retired.insert(Entry.first);
  }
};

// A frame plus the view changes it implies, applied once the frame is on
// the SD card.
struct InodeJournalChange {
  InodeJournalFrame Frame;
  std::map<uint64_t, int64_t> LinkChanges, ObjectRefChanges;
  std::map<uint64_t, std::optional<uint64_t>> UnlinkedChanges;
  std::set<uint64_t> Explicit;

  void ApplyTo(InodeJournalView& View) const {
    const auto Add = [](std::map<uint64_t, int64_t>& Counts, uint64_t Key, int64_t Change) {
      auto& Count = Counts[Key];
      Count += Change;
      if (!Count) Counts.erase(Key);
    };
    for (const auto& [Inode, Change] : LinkChanges) Add(View.Links, Inode, Change);
    for (const auto& [Object, Change] : ObjectRefChanges) Add(View.ObjectRefs, Object, Change);
    for (const auto& [Inode, Object] : UnlinkedChanges) {
      if (Object) View.UnlinkedObjects[Inode] = *Object;
      else View.UnlinkedObjects.erase(Inode);
    }
    for (const auto& Entry : Frame.RecordPuts) View.Records.insert(Entry.first);
    for (const auto Inode : Frame.RecordRemovals) View.Records.erase(Inode);
    for (const auto Object : Frame.RetiredAdds) View.Retired.insert(Object);
    for (const auto Object : Frame.RetiredRemovals) View.Retired.erase(Object);
    View.Explicit = Explicit;
  }
};

// The frame for the changes recorded in Names and Records since the last
// save, against View (what the files hold).
inline InodeJournalChange CollectJournalChange(uint64_t Generation, uint64_t NextInode, const TrackedNames& Names,
                                               const TrackedRecords& Records, const std::set<uint64_t>& RetiredBackingObjects,
                                               const InodeJournalView& View) {
  InodeJournalChange Change;
  auto& Frame = Change.Frame;
  Frame.Generation = Generation;
  Frame.NextInode = NextInode;
  std::set<uint64_t> Inodes;
  for (const auto& [Path, Original] : Names.Originals()) {
    const auto Position = Names.find(Path);
    const auto Current = Position == Names.end() ? std::nullopt : std::optional<uint64_t> {Position->second};
    if (Current == Original) continue;
    if (Original) {
      --Change.LinkChanges[*Original];
      Inodes.insert(*Original);
    }
    if (Current) {
      ++Change.LinkChanges[*Current];
      Inodes.insert(*Current);
      Frame.NamePuts.emplace_back(Path, *Current);
    } else {
      Frame.NameRemovals.push_back(Path);
    }
  }
  for (const auto Inode : Records.Touched()) Inodes.insert(Inode);

  std::set<uint64_t> Objects;
  for (const auto Inode : Inodes) {
    if (Inode < InodeSnapshot::FirstInode) continue;
    int64_t Links = 0;
    if (const auto Count = View.Links.find(Inode); Count != View.Links.end()) Links += Count->second;
    if (const auto Count = Change.LinkChanges.find(Inode); Count != Change.LinkChanges.end()) Links += Count->second;
    const auto Record = Records.find(Inode);
    const bool Exists = Record != Records.end();
    if (Exists && !Record->second.Unlinked && Links > 0) Frame.RecordPuts.emplace_back(Inode, Record->second);
    else if (View.Records.contains(Inode)) Frame.RecordRemovals.push_back(Inode);

    std::optional<uint64_t> Now;
    if (Exists && Record->second.ManagedFile && Record->second.Unlinked) Now = Record->second.BackingObject.value_or(Inode);
    const auto Before = View.UnlinkedObjects.find(Inode);
    const auto Was = Before == View.UnlinkedObjects.end() ? std::nullopt : std::optional<uint64_t> {Before->second};
    if (Now == Was) continue;
    Change.UnlinkedChanges[Inode] = Now;
    if (Was) {
      --Change.ObjectRefChanges[*Was];
      Objects.insert(*Was);
    }
    if (Now) {
      ++Change.ObjectRefChanges[*Now];
      Objects.insert(*Now);
    }
  }
  Change.Explicit = RetiredBackingObjects;
  for (const auto Object : RetiredBackingObjects) {
    if (!View.Explicit.contains(Object)) Objects.insert(Object);
  }
  for (const auto Object : View.Explicit) {
    if (!RetiredBackingObjects.contains(Object)) Objects.insert(Object);
  }
  for (const auto Object : Objects) {
    int64_t Refs = 0;
    if (const auto Count = View.ObjectRefs.find(Object); Count != View.ObjectRefs.end()) Refs += Count->second;
    if (const auto Count = Change.ObjectRefChanges.find(Object); Count != Change.ObjectRefChanges.end()) Refs += Count->second;
    const bool Present = RetiredBackingObjects.contains(Object) || Refs > 0;
    const bool Written = View.Retired.contains(Object);
    if (Present && !Written) Frame.RetiredAdds.push_back(Object);
    if (!Present && Written) Frame.RetiredRemovals.push_back(Object);
  }
  return Change;
}

namespace InodeJournalFormat {
constexpr uint64_t Magic = 0x01004c4e524a5846; // FXJRNL, 0, version 1.
constexpr size_t HeaderBytes = 16, TrailerBytes = 8;
} // namespace InodeJournalFormat

// Magic, body length, body, checksum of magic, length and body. Empty if a
// record cannot be encoded.
inline std::vector<uint8_t> EncodeJournalFrame(const InodeJournalFrame& Frame) {
  using namespace InodeSnapshotFormat;
  std::vector<uint8_t> Bytes;
  PutWord(Bytes, InodeJournalFormat::Magic);
  PutWord(Bytes, 0); // Body length, filled in below.
  PutWord(Bytes, Frame.Generation);
  PutWord(Bytes, Frame.NextInode);
  PutWord(Bytes, Frame.NamePuts.size());
  for (const auto& [Path, Inode] : Frame.NamePuts) {
    PutString(Bytes, Path);
    PutWord(Bytes, Inode);
  }
  PutWord(Bytes, Frame.NameRemovals.size());
  for (const auto& Path : Frame.NameRemovals) PutString(Bytes, Path);
  PutWord(Bytes, Frame.RecordPuts.size());
  for (const auto& [Inode, Record] : Frame.RecordPuts) {
    PutWord(Bytes, Inode);
    if (!PutRecordFields(Bytes, Record)) return {};
  }
  for (const auto* List : {&Frame.RecordRemovals, &Frame.RetiredAdds, &Frame.RetiredRemovals}) {
    PutWord(Bytes, List->size());
    for (const auto Value : *List) PutWord(Bytes, Value);
  }
  if (Bytes.size() > InodeSnapshot::MaxBytes) return {};
  const uint64_t Body = Bytes.size() - InodeJournalFormat::HeaderBytes;
  for (unsigned I = 0; I < 8; ++I) Bytes[8 + I] = static_cast<uint8_t>(Body >> (I * 8));
  PutWord(Bytes, Checksum(Bytes));
  return Bytes;
}

struct DecodedJournal {
  std::vector<InodeJournalFrame> Frames;
  std::vector<size_t> Ends; // Byte offset after each frame.
};

// Reads frames until the end or the first frame that is torn or fails its
// checksum (an append interrupted by a crash).
inline DecodedJournal DecodeJournal(std::span<const uint8_t> Bytes) {
  using namespace InodeSnapshotFormat;
  DecodedJournal Journal;
  size_t Offset = 0;
  while (Bytes.size() - Offset >= InodeJournalFormat::HeaderBytes + InodeJournalFormat::TrailerBytes) {
    Reader Header {Bytes.subspan(Offset, InodeJournalFormat::HeaderBytes)};
    uint64_t FrameMagic {}, Body {};
    Header.Word(FrameMagic);
    Header.Word(Body);
    const auto Left = Bytes.size() - Offset - InodeJournalFormat::HeaderBytes - InodeJournalFormat::TrailerBytes;
    if (FrameMagic != InodeJournalFormat::Magic || Body > Left) break;
    const auto Covered = Bytes.subspan(Offset, InodeJournalFormat::HeaderBytes + Body);
    Reader Tail {Bytes.subspan(Offset + Covered.size(), InodeJournalFormat::TrailerBytes)};
    uint64_t Written {};
    Tail.Word(Written);
    if (Checksum(Covered) != Written) break;

    Reader Input {Covered.subspan(InodeJournalFormat::HeaderBytes)};
    InodeJournalFrame Frame;
    uint64_t Count {};
    const auto CountOk = [&](size_t Smallest) { return Input.Word(Count) && Count <= Input.Bytes.size() / Smallest; };
    bool Ok = Input.Word(Frame.Generation) && Input.Word(Frame.NextInode) && Frame.NextInode >= InodeSnapshot::FirstInode && CountOk(16);
    for (uint64_t I = 0; Ok && I < Count; ++I) {
      std::string Path;
      uint64_t Inode {};
      Ok = Input.String(Path, InodeSnapshot::MaxNameBytes) && Path.starts_with('/') && Path.find('\0') == Path.npos &&
           Input.Word(Inode) && Inode >= InodeSnapshot::FirstInode && Inode < Frame.NextInode;
      if (Ok) Frame.NamePuts.emplace_back(std::move(Path), Inode);
    }
    Ok = Ok && CountOk(8);
    for (uint64_t I = 0; Ok && I < Count; ++I) {
      std::string Path;
      Ok = Input.String(Path, InodeSnapshot::MaxNameBytes) && Path.starts_with('/');
      if (Ok) Frame.NameRemovals.push_back(std::move(Path));
    }
    Ok = Ok && CountOk(16);
    for (uint64_t I = 0; Ok && I < Count; ++I) {
      uint64_t Inode {};
      InodeMetadata Record;
      Ok = Input.Word(Inode) && Inode >= InodeSnapshot::FirstInode && Inode < Frame.NextInode &&
           ReadRecordFields(Input, Record, Frame.NextInode, nullptr, nullptr);
      if (Ok) Frame.RecordPuts.emplace_back(Inode, std::move(Record));
    }
    for (auto* List : {&Frame.RecordRemovals, &Frame.RetiredAdds, &Frame.RetiredRemovals}) {
      Ok = Ok && CountOk(8);
      for (uint64_t I = 0; Ok && I < Count; ++I) {
        uint64_t Value {};
        Ok = Input.Word(Value) && Value >= InodeSnapshot::FirstInode;
        if (Ok) List->push_back(Value);
      }
    }
    if (!Ok || !Input.Bytes.empty()) break;
    Offset += Covered.size() + InodeJournalFormat::TrailerBytes;
    Journal.Frames.push_back(std::move(Frame));
    Journal.Ends.push_back(Offset);
  }
  return Journal;
}

struct ReplayedStore {
  std::optional<InodeSnapshot> Snapshot; // Empty if the replayed store fails the snapshot checks.
  size_t Applied {};                     // Frames applied after the checkpoint.
  size_t Consumed {};                    // Leading frames skipped or applied; the rest are never replayed.
};

// Applies the frames that follow Base. Frames at or below Base's generation
// (left over from before the checkpoint) are skipped; replay stops at the
// first gap in generations.
inline ReplayedStore ReplayJournal(const InodeSnapshot& Base, std::span<const InodeJournalFrame> Frames) {
  ReplayedStore Result;
  auto Names = Base.Names;
  InodeRecords Linked;
  auto Retired = Base.RetiredBackingObjects;
  for (const auto& [Inode, Record] : Base.Records) {
    if (Record.ManagedFile && Record.Unlinked) Retired.insert(Record.BackingObject.value_or(Inode));
    else Linked.emplace(Inode, Record);
  }
  uint64_t Generation = Base.Generation, NextInode = Base.NextInode;
  for (const auto& Frame : Frames) {
    if (!Result.Applied && Frame.Generation <= Generation) {
      ++Result.Consumed;
      continue;
    }
    if (Frame.Generation != Generation + 1) break;
    ++Result.Consumed;
    for (const auto& [Path, Inode] : Frame.NamePuts) Names[Path] = Inode;
    for (const auto& Path : Frame.NameRemovals) Names.erase(Path);
    for (const auto& [Inode, Record] : Frame.RecordPuts) Linked[Inode] = Record;
    for (const auto Inode : Frame.RecordRemovals) Linked.erase(Inode);
    for (const auto Object : Frame.RetiredAdds) Retired.insert(Object);
    for (const auto Object : Frame.RetiredRemovals) Retired.erase(Object);
    Generation = Frame.Generation;
    NextInode = Frame.NextInode;
    ++Result.Applied;
  }
  if (!Result.Applied) {
    Result.Snapshot = Base;
    return Result;
  }
  const auto Bytes = EncodeInodeSnapshot(Generation, NextInode, Names, Linked, Retired);
  if (!Bytes.empty()) Result.Snapshot = DecodeInodeSnapshot(Bytes);
  return Result;
}
} // namespace Switch2Guest
