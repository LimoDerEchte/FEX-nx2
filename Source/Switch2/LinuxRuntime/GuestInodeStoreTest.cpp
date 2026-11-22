// SPDX-License-Identifier: MIT
#include "../GuestDirectoryRecovery.h"
#include "../GuestInodeStore.h"
#include "../GuestPosixAcl.h"

#include <cassert>

int main() {
  using namespace Switch2Guest;
  // Every possible presence combination is tested for each transaction
  // kind, both before and after the namespace checkpoint.
  using Kind = DirectoryRenameKind;
  using Plan = DirectoryRecovery;
  constexpr auto I = Plan::Invalid, F = Plan::Finish;
  constexpr Plan Expected[5][2][8] {
    {{I, I, I, Plan::RestoreBoth, I, Plan::RestoreDestination, F, I},
     {I, I, F, Plan::RemoveBackup, I, I, I, I}},
    {{I, I, I, Plan::RestoreBoth, I, Plan::RestoreDestination, F, I},
     {I, I, I, Plan::CompleteExchange, I, I, F, I}},
    {{I, I, Plan::RestoreSource, I, F, I, I, I},
     {I, I, F, I, I, I, I, I}},
    {{I, I, Plan::RestoreSource, I, F, I, I, I},
     {F, I, Plan::RemoveBackup, I, I, I, I, I}},
    {{F, I, I, I, Plan::RemoveBackup, I, I, I},
     {I, I, F, I, Plan::RestoreDestination, I, I, I}},
  };
  for (unsigned K = 0; K < 5; ++K) {
    for (unsigned Committed = 0; Committed < 2; ++Committed) {
      for (unsigned State = 0; State < 8; ++State) {
        assert(PlanDirectoryRecovery(static_cast<Kind>(K), Committed, State & 4, State & 2, State & 1) ==
               Expected[K][Committed][State]);
      }
    }
  }

  const char RawACL[] = {
    2, 0, 0, 0,
    1, 0, 7, 0, -1, -1, -1, -1,
    2, 0, 4, 0, 42, 0, 0, 0,
    4, 0, 5, 0, -1, -1, -1, -1,
    16, 0, 5, 0, -1, -1, -1, -1,
    32, 0, 1, 0, -1, -1, -1, -1,
  };
  std::string ACL(RawACL, sizeof(RawACL));
  const auto Parsed = ParsePosixAcl(ACL);
  assert(Parsed && Parsed->Extended && Parsed->Mode(ACL) == 0751);
  const auto Group = [](uint32_t ID) { return ID == 10; };
  assert(Parsed->Allows(ACL, 20, 10, 42, Group, 4));
  assert(!Parsed->Allows(ACL, 20, 10, 42, Group, 1));
  assert(Parsed->Allows(ACL, 20, 10, 43, Group, 1));
  Parsed->ApplyMode(ACL, 0666, true);
  assert(Parsed->Mode(ACL) == 0640);
  Parsed->ApplyMode(ACL, 0600, false);
  assert(Parsed->Mode(ACL) == 0600 && ACL[14] == 4);
  ACL[30] = 8;
  assert(!ParsePosixAcl(ACL));

  const auto First = InodeSnapshot::FirstInode;
  InodeNames Names {{"/fex/rootfs/one", First}, {"/fex/rootfs/link", First}};
  InodeRecords Records;
  Records[First].Mode = 0640;
  Records[First].ManagedFile = true;
  Records[First].Times = InodeTimestamps {{-15, 123}, {4, 999999999}, {6, 444}};
  Records[First].Attributes.emplace("user.empty", "");
  Records[First].Attributes.emplace("user.binary", std::string("a\0\xff", 3));
  Records[First + 1].Unlinked = true;
  Names.emplace("/fex/rootfs/symbolic", First + 2);
  Records[First + 2].Symlink = "../somewhere";
  Names.emplace("/fex/rootfs/fifo", First + 3);
  Records[First + 3].SpecialType = 0010000;

  const auto Older = EncodeInodeSnapshot(3, First + 4, Names, Records);
  Records[First].Mode = 0600;
  Records[First].LogicalSize = 0;
  Records[First].Owner.emplace(20, 10);
  const auto Newer = EncodeInodeSnapshot(4, First + 4, Names, Records);
  const auto Loaded = RecoverInodeSnapshot(Older, Newer);
  assert(Loaded && Loaded->Generation == 4 && Loaded->NextInode == First + 4);
  assert(Loaded->Names == Names && Loaded->Records.size() == 3);
  assert(Loaded->Records.at(First + 2).Symlink == "../somewhere");
  assert(Loaded->Records.at(First + 3).SpecialType == 0010000);
  const auto& Record = Loaded->Records.at(First);
  assert(Record.Mode == 0600 && Record.ManagedFile && Record.LogicalSize == 0);
  assert(!DecodeInodeSnapshot(Older)->Records.at(First).LogicalSize);
  assert(Record.Owner && Record.Owner->first == 20 && Record.Owner->second == 10);
  assert(Record.Times && Record.Times->Access.Seconds == -15 && Record.Times->Modify.Nanoseconds == 999999999);
  assert(Record.Attributes == Records.at(First).Attributes);
  for (size_t Size = 0; Size < Newer.size(); ++Size) {
    const auto Recovered = RecoverInodeSnapshot(Older, std::span(Newer).first(Size));
    assert(Recovered && Recovered->Generation == 3);
  }
  for (size_t I = 0; I < Newer.size(); ++I) {
    auto Corrupted = Newer;
    Corrupted[I] ^= 0x80;
    assert(!DecodeInodeSnapshot(Corrupted));
  }
  assert(!RecoverInodeSnapshot({}, {}));

  {
    // Overflow directories round-trip, accept names beyond Horizon's SD path
    // limit, and exclude the other entry kinds.
    InodeNames LongNames {{"/fex/rootfs/" + std::string(300, 'd'), First}};
    InodeRecords LongRecords;
    LongRecords[First].OverflowDirectory = true;
    LongRecords[First].Mode = 0750;
    const auto Encoded = EncodeInodeSnapshot(1, First + 1, LongNames, LongRecords);
    const auto Decoded = DecodeInodeSnapshot(Encoded);
    assert(Decoded && Decoded->Names == LongNames && Decoded->Records.at(First).OverflowDirectory);
    assert(!Decoded->Records.at(First).ManagedFile && Decoded->Records.at(First).Mode == 0750);
    LongRecords[First].ManagedFile = true;
    assert(EncodeInodeSnapshot(1, First + 1, LongNames, LongRecords).empty());
    LongRecords[First].ManagedFile = false;
    LongNames = {{"/fex/rootfs/" + std::string(InodeSnapshot::MaxNameBytes, 'n'), First}};
    assert(!DecodeInodeSnapshot(EncodeInodeSnapshot(1, First + 1, LongNames, LongRecords)));
  }

  auto ReplacedRecords = Records;
  ReplacedRecords.at(First).BackingObject = First + 4;
  const auto Replaced = EncodeInodeSnapshot(5, First + 5, Names, ReplacedRecords, {First});
  const auto Replacement = RecoverInodeSnapshot(Newer, Replaced);
  assert(Replacement && Replacement->Names == Names);
  assert(Replacement->Records.at(First).BackingObject == First + 4);
  assert(CanReclaimInode(First, *Replacement));
  assert(!CanReclaimInode(First, *Loaded));
  assert(!CanReclaimInode(First + 4, *Replacement));
  const auto Reserved = DecodeInodeSnapshot(EncodeInodeSnapshot(5, First + 5, Names, Records, {First + 4}));
  assert(Reserved && Reserved->Names == Names);
  assert(CanReclaimInode(First + 4, *Reserved));
  assert(!CanReclaimInode(First, *Reserved));
  assert(EncodeInodeSnapshot(5, First + 5, Names, ReplacedRecords, {First + 4}).empty());
  auto AliasedRecords = ReplacedRecords;
  auto& Aliased = AliasedRecords.at(First + 2);
  Aliased.Symlink.reset();
  Aliased.ManagedFile = true;
  Aliased.BackingObject = First + 4;
  assert(EncodeInodeSnapshot(5, First + 5, Names, AliasedRecords).empty());
  Aliased.BackingObject = First + 3;
  assert(EncodeInodeSnapshot(5, First + 5, Names, AliasedRecords).empty());
  assert(RecoverInodeSnapshot(Newer, std::span(Replaced).first(Replaced.size() - 1))->Generation == 4);

  auto& SparseRecord = ReplacedRecords.at(First);
  SparseRecord.LogicalSize = 16384;
  SparseRecord.Extents = PackedFileExtents({{0, 12288}, {16384, 20480}}).Remove(4096, 8192);
  const auto SparseBytes = EncodeInodeSnapshot(6, First + 5, Names, ReplacedRecords, {First});
  const auto Sparse = DecodeInodeSnapshot(SparseBytes);
  assert(Sparse && Sparse->Names == Names && Sparse->Records.at(First).LogicalSize == 16384);
  const auto& Extents = *Sparse->Records.at(First).Extents;
  assert(Extents.Extents().size() == 3);
  assert(Extents.At(0, 16384).Allocated && Extents.At(0, 16384).Length == 4096);
  assert(!Extents.At(4096, 8192).Allocated && Extents.At(4096, 8192).Length == 4096);
  assert(Extents.At(8192, 8192).Allocated && Extents.At(8192, 8192).BackingOffset == 4096);
  assert(Extents.At(16384, 4096).Allocated && Extents.At(16384, 4096).BackingOffset == 8192);
  assert(!Extents.At(20480, 4096).Allocated && Extents.At(20480, 4096).Length == 4096);
  assert(Extents.Seek(4096, 16384, true) == 8192);
  assert(Extents.Seek(4096, 16384, false) == 4096);
  assert(Extents.Seek(8192, 16384, false) == 12288);
  assert(!Extents.Seek(12288, 16384, true));
  assert(!Extents.Seek(16384, 16384, false));
  std::array<char, 4098> AcrossHole;
  assert(Extents.ReadBytes(4095, AcrossHole, [](uint64_t Position, std::span<char> Chunk) -> int64_t {
    assert(Chunk.size() == 1 && (Position == 4095 || Position == 4096));
    Chunk.front() = Position == 4095 ? 'a' : 'b';
    return 1;
  }) == 4098);
  assert(AcrossHole.front() == 'a' && AcrossHole.back() == 'b');
  for (auto Byte : std::span(AcrossHole).subspan(1, 4096)) assert(Byte == 0);
  assert(Extents.ReadBytes(4095, AcrossHole, [](uint64_t, std::span<char>) -> int64_t { return -5; }) == -5);

  const PackedFileExtents Original({{0, 8}, {12, 16}});
  const auto Punched = Original.Remove(4, 8);
  const std::string Existing = "abcdefghmnop";
  std::string ReplacementData;
  std::array<char, 3> ChunkBuffer;
  auto ReadBacking = [&](uint64_t Position, std::span<char> Chunk) -> int64_t {
    assert(Position + Chunk.size() <= Existing.size());
    std::memcpy(Chunk.data(), Existing.data() + Position, Chunk.size());
    return Chunk.size();
  };
  auto Overlay = [](uint64_t Logical, std::span<char> Chunk) {
    if (Logical == 3) Chunk.front() = 0;
    if (Logical == 12) Chunk.front() = 0;
  };
  const auto Shifted = Original.ShiftRange(4, 2, true);
  assert(Shifted.Valid() && Shifted.Seek(4, 18, true) == 6);
  const auto Collapsed = Original.ShiftRange(2, 2, false);
  assert(Collapsed.Valid() && Collapsed.Seek(6, 14, true) == 10);
  const auto AppendChunk = [&](uint64_t Position, std::span<const char> Chunk) -> int64_t {
    assert(Position == ReplacementData.size());
    ReplacementData.append(Chunk.data(), Chunk.size());
    return 0;
  };
  const auto Unchanged = [](uint64_t, std::span<char>) {};
  assert(Original.CopyTo(Shifted, ChunkBuffer, ReadBacking, AppendChunk, Unchanged, 6, -2) == 0);
  assert(ReplacementData == Existing);
  ReplacementData.clear();
  assert(Original.CopyTo(Collapsed, ChunkBuffer, ReadBacking, AppendChunk, Unchanged, 2, 2) == 0);
  assert(ReplacementData == "abefghmnop");
  ReplacementData.clear();
  assert(Original.CopyTo(Punched, ChunkBuffer, ReadBacking,
    [&](uint64_t Position, std::span<const char> Chunk) -> int64_t {
      assert(Position == ReplacementData.size());
      ReplacementData.append(Chunk.data(), Chunk.size());
      return 0;
    }, Overlay) == 0);
  assert(ReplacementData == std::string("abc\0\0nop", 8));
  const auto Filled = Punched.Allocate(4, 12);
  assert(Filled.Extents().size() == 1 && (Filled.Extents().front() == FileExtent {0, 16}));
  ReplacementData.clear();
  assert(Punched.CopyTo(Filled, ChunkBuffer,
    [&](uint64_t Position, std::span<char> Chunk) -> int64_t {
      const std::string Retained = "abcdmnop";
      assert(Position + Chunk.size() <= Retained.size());
      std::memcpy(Chunk.data(), Retained.data() + Position, Chunk.size());
      return Chunk.size();
    },
    [&](uint64_t Position, std::span<const char> Chunk) -> int64_t {
      assert(Position == ReplacementData.size());
      ReplacementData.append(Chunk.data(), Chunk.size());
      return 0;
    }, [](uint64_t, std::span<char>) {}) == 0);
  assert(ReplacementData == std::string("abcd") + std::string(8, '\0') + "mnop");
  const auto Disjoint = Filled.Allocate(20, 24);
  assert(Disjoint.Extents().size() == 2 && (Disjoint.Extents().back() == FileExtent {20, 24}));
  unsigned Writes = 0;
  assert(Original.CopyTo(Punched, ChunkBuffer, ReadBacking,
    [&](uint64_t, std::span<const char>) -> int64_t { return ++Writes == 2 ? -28 : 0; }, Overlay) == -28);
  assert(Writes == 2);
  assert(Extents.Remove(0, 20480).Extents().empty());
  SparseRecord.Extents = PackedFileExtents({{0, 8192}, {4096, 12288}});
  assert(EncodeInodeSnapshot(6, First + 5, Names, ReplacedRecords).empty());

  auto RetiredNames = Names;
  RetiredNames.erase("/fex/rootfs/one");
  RetiredNames.erase("/fex/rootfs/link");
  Records[First].Unlinked = true;
  const auto RetireBytes = EncodeInodeSnapshot(5, First + 4, RetiredNames, Records);
  const auto Retirement = DecodeInodeSnapshot(RetireBytes);
  assert(Retirement && Retirement->Records.at(First).Unlinked);
  assert(!CanReclaimInode(First, *Loaded));
  assert(CanReclaimInode(First, *Retirement));

  const auto IntentBytes = EncodeInodeSnapshot(5, First + 3, {{"/fex/rootfs/one", First}}, {});
  const auto Intent = DecodeInodeSnapshot(IntentBytes);
  assert(Intent && Intent->Generation == 5 && Intent->Names.size() == 1 && Intent->Records.empty());
  assert(Intent->Names.at("/fex/rootfs/one") == First);
  assert(!DecodeInodeSnapshot(std::span(IntentBytes).first(IntentBytes.size() - 1)));
}
