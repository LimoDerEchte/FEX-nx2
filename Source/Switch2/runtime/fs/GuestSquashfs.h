// SPDX-License-Identifier: MIT
// Reader for squashfs 4.0 images (the FEX RootFS project's ".sqsh" files): superblock, id table, inode table,
// directory table and fragments, with zstd decompression. It never mounts anything and never holds the image in memory:
// it reads bounded chunks through an ImageSource (an SD file on the console, a host file in tests), walks the tree in
// order (a directory before its children) and streams file data one block at a time. Other compressors (gzip, xz,
// lzma, lzo, lz4) are rejected with a clear error. The images FEX publishes store no extended attributes; an image
// that has an xattr table is rejected rather than extracted with the attributes silently dropped.
// Portable (no libnx), so it is tested natively.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ThirdParty/zstd/zstd.h"

namespace Switch2Guest::Squashfs {

class ImageSource {
public:
  virtual ~ImageSource() = default;
  virtual uint64_t Size() const = 0;
  // Reads exactly Size bytes at Offset. False on a short read or error.
  virtual bool Read(uint64_t Offset, void* Buffer, size_t Size) = 0;
};

// stdio-backed source (SD card files on the console, regular files on the host).
class StdioImageSource final : public ImageSource {
public:
  explicit StdioImageSource(const std::string& Path) {
    File = std::fopen(Path.c_str(), "rb");
    if (!File) return;
    std::setvbuf(File, nullptr, _IOFBF, 64 * 1024);
    std::fseek(File, 0, SEEK_END);
    const auto End = std::ftell(File);
    Total = End > 0 ? static_cast<uint64_t>(End) : 0;
  }
  ~StdioImageSource() override {
    if (File) std::fclose(File);
  }
  bool Opened() const { return File != nullptr; }
  uint64_t Size() const override { return Total; }
  bool Read(uint64_t Offset, void* Buffer, size_t Count) override {
    if (!File || Offset + Count > Total) return false;
    if (std::fseek(File, static_cast<long>(Offset), SEEK_SET)) return false;
    return Count == 0 || std::fread(Buffer, 1, Count, File) == Count;
  }

private:
  std::FILE* File {};
  uint64_t Total {};
};

enum class Type { Directory, File, Symlink, CharDevice, BlockDevice, Fifo, Socket };

struct Entry {
  std::string Path;           // absolute, "/" for the root
  Type Kind {};
  uint32_t Mode {};           // permission bits including setuid/setgid/sticky (07777)
  uint32_t Uid {}, Gid {};
  uint32_t ModifyTime {};
  uint64_t Size {};           // file size
  uint32_t Links {1};
  uint32_t InodeNumber {};
  std::string LinkTarget;     // symlinks
  uint32_t Major {}, Minor {};
  uint64_t Device {};         // Linux dev_t of a device node
  std::string HardLinkTo;     // non-empty: this path is another name of that earlier entry
  // File data location.
  uint64_t StartBlock {};
  uint32_t Fragment {0xFFFFFFFF}, FragmentOffset {};
  std::vector<uint32_t> BlockSizes;
  // Directory listing location.
  uint64_t DirectoryBlock {};
  uint32_t DirectoryOffset {};
};

struct Superblock {
  uint32_t Inodes {}, BlockSize {}, Fragments {};
  uint16_t Compression {}, BlockLog {}, Flags {}, IdCount {};
  uint64_t RootInode {}, BytesUsed {}, IdTable {}, XattrTable {}, InodeTable {}, DirectoryTable {}, FragmentTable {};
};

inline const char* CompressionName(uint16_t Id) {
  switch (Id) {
  case 1: return "gzip";
  case 2: return "lzma";
  case 3: return "lzo";
  case 4: return "xz";
  case 5: return "lz4";
  case 6: return "zstd";
  default: return "unknown";
  }
}

class Reader {
public:
  using VisitFn = std::function<const char*(const Entry&)>;
  using DataFn = std::function<const char*(const uint8_t*, size_t)>;

  explicit Reader(ImageSource& Source) : Source {Source} {}
  const Superblock& Info() const { return Super; }
  // Detail names the compressor for the "unsupported" error.
  std::string Detail;

  // Reads and checks the superblock and the id and fragment tables. Returns an error text, or nullptr.
  const char* Open() {
    uint8_t Raw[96];
    if (Source.Size() < sizeof(Raw) || !Source.Read(0, Raw, sizeof(Raw))) return "The image is too small to be a squashfs file system";
    if (U32(Raw) != 0x73717368) return "Not a squashfs image (bad magic)";
    Super.Inodes = U32(Raw + 4);
    Super.BlockSize = U32(Raw + 12);
    Super.Fragments = U32(Raw + 16);
    Super.Compression = U16(Raw + 20);
    Super.BlockLog = U16(Raw + 22);
    Super.Flags = U16(Raw + 24);
    Super.IdCount = U16(Raw + 26);
    if (U16(Raw + 28) != 4 || U16(Raw + 30) != 0) return "Unsupported squashfs version (only 4.0)";
    Super.RootInode = U64(Raw + 32);
    Super.BytesUsed = U64(Raw + 40);
    Super.IdTable = U64(Raw + 48);
    Super.XattrTable = U64(Raw + 56);
    Super.InodeTable = U64(Raw + 64);
    Super.DirectoryTable = U64(Raw + 72);
    Super.FragmentTable = U64(Raw + 80);
    if (Super.Compression != 6) {
      Detail = std::string {"The image is compressed with "} + CompressionName(Super.Compression) + "; only zstd is supported";
      return "Unsupported squashfs compression";
    }
    if (Super.BlockSize < 4096 || Super.BlockSize > (1u << 20) || (Super.BlockSize & (Super.BlockSize - 1)) ||
        (1u << Super.BlockLog) != Super.BlockSize)
      return "Corrupt squashfs superblock (block size)";
    if (Super.XattrTable != ~uint64_t {0}) return "The image has extended attributes, which are not supported";
    if (Super.BytesUsed > Source.Size()) return "The squashfs image is truncated";
    if (Super.InodeTable >= Super.BytesUsed || Super.DirectoryTable >= Super.BytesUsed) return "Corrupt squashfs superblock (tables)";
    // Id table: an index of metadata block pointers.
    Ids.clear();
    const uint32_t IdBlocks = (Super.IdCount + 1023) / 1024;
    std::vector<uint8_t> Index(size_t {IdBlocks} * 8);
    if (IdBlocks && !Source.Read(Super.IdTable, Index.data(), Index.size())) return "Could not read the squashfs id table";
    for (uint32_t Block = 0; Block < IdBlocks; ++Block) {
      std::vector<uint8_t> Data;
      if (const auto* Error = ReadMetadataBlock(U64(Index.data() + Block * 8ull), Data, nullptr)) return Error;
      for (size_t Offset = 0; Offset + 4 <= Data.size() && Ids.size() < Super.IdCount; Offset += 4) Ids.push_back(U32(Data.data() + Offset));
    }
    if (Ids.size() != Super.IdCount) return "Corrupt squashfs id table";
    FragmentIndex.clear();
    if (Super.Fragments) {
      const uint32_t FragmentBlocks = (Super.Fragments + 511) / 512;
      FragmentIndex.resize(FragmentBlocks);
      std::vector<uint8_t> FragmentPointers(size_t {FragmentBlocks} * 8);
      if (!Source.Read(Super.FragmentTable, FragmentPointers.data(), FragmentPointers.size())) return "Could not read the squashfs fragment table";
      for (uint32_t Block = 0; Block < FragmentBlocks; ++Block) FragmentIndex[Block] = U64(FragmentPointers.data() + Block * 8ull);
    }
    return nullptr;
  }

  // Visits every inode in tree order: a directory comes before its children, children in name order. Hard links (a later
  // directory entry naming an inode that has more than one link) get HardLinkTo set to the first path.
  const char* Walk(const VisitFn& Visit) {
    LinkedPaths.clear();
    Entry Root;
    Root.Path = "/";
    if (const auto* Error = ReadInode(Super.RootInode >> 16, Super.RootInode & 0xffff, Root)) return Error;
    if (Root.Kind != Type::Directory) return "The squashfs root is not a directory";
    if (const auto* Error = Visit(Root)) return Error;
    return WalkDirectory(Root, 0, Visit);
  }

  // Streams a regular file's data, one block at a time (sparse blocks arrive as zeros).
  const char* ReadFile(const Entry& File, const DataFn& Sink) {
    const uint64_t Block = Super.BlockSize;
    uint64_t Remaining = File.Size;
    uint64_t Position = File.StartBlock;
    std::vector<uint8_t> Compressed, Data;
    for (size_t Index = 0; Index < File.BlockSizes.size(); ++Index) {
      const uint64_t Expected = std::min<uint64_t>(Block, Remaining);
      if (!Expected) return "Corrupt squashfs file (too many blocks)";
      const uint32_t Word = File.BlockSizes[Index];
      const uint32_t Stored = Word & 0xffffff;
      if (!Stored) {
        Data.assign(Expected, 0);
      } else {
        if (const auto* Error = ReadDataBlock(Position, Word, Compressed, Data)) return Error;
        if (Data.size() < Expected) return "Corrupt squashfs data block (short)";
        Data.resize(Expected);
      }
      Position += Stored;
      Remaining -= Expected;
      if (const auto* Error = Sink(Data.data(), Data.size())) return Error;
    }
    if (Remaining) {
      if (File.Fragment == 0xFFFFFFFF || Remaining >= Block) return "Corrupt squashfs file (missing tail)";
      const uint8_t* FragmentData {};
      if (const auto* Error = LoadFragment(File.Fragment, FragmentData)) return Error;
      if (uint64_t {File.FragmentOffset} + Remaining > FragmentBytes.size()) return "Corrupt squashfs fragment";
      if (const auto* Error = Sink(FragmentBytes.data() + File.FragmentOffset, Remaining)) return Error;
    }
    return nullptr;
  }

  // Sum of regular file sizes (needs a walk; callers that want the installed size run Walk once).
  static uint64_t RoundUp(uint64_t Value, uint64_t Unit) { return (Value + Unit - 1) / Unit * Unit; }

private:
  static uint16_t U16(const uint8_t* P) { return static_cast<uint16_t>(P[0] | P[1] << 8); }
  static uint32_t U32(const uint8_t* P) { return P[0] | P[1] << 8 | P[2] << 16 | static_cast<uint32_t>(P[3]) << 24; }
  static uint64_t U64(const uint8_t* P) { return U32(P) | static_cast<uint64_t>(U32(P + 4)) << 32; }

  static const char* Decompress(const uint8_t* In, size_t InSize, std::vector<uint8_t>& Out, size_t Capacity) {
    Out.resize(Capacity);
    const auto Result = ZSTD_decompress(Out.data(), Capacity, In, InSize);
    if (ZSTD_isError(Result)) return "Corrupt zstd data in the squashfs image";
    Out.resize(Result);
    return nullptr;
  }

  // Reads the metadata block at the absolute offset. Next receives the offset of the following block.
  const char* ReadMetadataBlock(uint64_t Offset, std::vector<uint8_t>& Data, uint64_t* Next) {
    uint8_t Header[2];
    if (Offset + 2 > Super.BytesUsed || !Source.Read(Offset, Header, 2)) return "Could not read squashfs metadata";
    const uint16_t Word = U16(Header);
    const size_t Size = Word & 0x7fff;
    if (!Size || Size > 8192 || Offset + 2 + Size > Super.BytesUsed) return "Corrupt squashfs metadata block";
    Scratch.resize(Size);
    if (!Source.Read(Offset + 2, Scratch.data(), Size)) return "Could not read squashfs metadata";
    if (Word & 0x8000) {
      Data = Scratch;
    } else if (const auto* Error = Decompress(Scratch.data(), Size, Data, 8192)) {
      return Error;
    }
    if (Next) *Next = Offset + 2 + Size;
    return nullptr;
  }

  // A cursor over a metadata table that continues into the next block like the kernel's squashfs_read_metadata.
  struct Cursor {
    uint64_t Table {};   // absolute offset of the table's first block
    uint64_t Block {};   // block offset relative to Table
    uint32_t Offset {};  // offset in the decompressed block
  };

  struct CachedBlock {
    uint64_t Key {};
    uint64_t Next {};
    std::vector<uint8_t> Data;
  };

  const char* Block(uint64_t Absolute, const CachedBlock*& Out) {
    for (auto& Item : Cache) {
      if (Item.Key == Absolute && !Item.Data.empty()) {
        Out = &Item;
        return nullptr;
      }
    }
    auto& Slot = Cache[CacheNext++ % Cache.size()];
    Slot.Key = Absolute;
    if (const auto* Error = ReadMetadataBlock(Absolute, Slot.Data, &Slot.Next)) {
      Slot.Data.clear();
      return Error;
    }
    Out = &Slot;
    return nullptr;
  }

  const char* ReadMeta(Cursor& At, void* Output, size_t Count) {
    auto* Bytes = static_cast<uint8_t*>(Output);
    while (Count) {
      const CachedBlock* Item {};
      if (const auto* Error = Block(At.Table + At.Block, Item)) return Error;
      if (At.Offset >= Item->Data.size()) {
        // Exactly at the end of this block: continue in the next one.
        if (At.Offset > Item->Data.size()) return "Corrupt squashfs metadata offset";
        At.Block = Item->Next - At.Table;
        At.Offset = 0;
        const CachedBlock* Following {};
        if (const auto* Error = Block(At.Table + At.Block, Following)) return Error;
        Item = Following;
      }
      const size_t Take = std::min<size_t>(Count, Item->Data.size() - At.Offset);
      std::memcpy(Bytes, Item->Data.data() + At.Offset, Take);
      Bytes += Take;
      Count -= Take;
      At.Offset += static_cast<uint32_t>(Take);
    }
    return nullptr;
  }

  // Reads and decompresses one data block (or fragment block) to its full length.
  const char* ReadDataBlock(uint64_t Offset, uint32_t Word, std::vector<uint8_t>& Compressed, std::vector<uint8_t>& Data) {
    const size_t Stored = Word & 0xffffff;
    if (Stored > Super.BlockSize || Offset + Stored > Super.BytesUsed) return "Corrupt squashfs data block";
    Compressed.resize(Stored);
    if (!Source.Read(Offset, Compressed.data(), Stored)) return "Could not read squashfs file data";
    if (Word & (1u << 24)) {
      Data = Compressed;
      return nullptr;
    }
    return Decompress(Compressed.data(), Stored, Data, Super.BlockSize);
  }

  const char* LoadFragment(uint32_t Index, const uint8_t*& Data) {
    if (Index >= Super.Fragments) return "Corrupt squashfs fragment index";
    if (FragmentLoaded == Index) {
      Data = FragmentBytes.data();
      return nullptr;
    }
    FragmentLoaded = 0xFFFFFFFF;
    std::vector<uint8_t> Metadata;
    if (const auto* Error = ReadMetadataBlock(FragmentIndex[Index / 512], Metadata, nullptr)) return Error;
    const size_t At = size_t {Index % 512} * 16;
    if (At + 16 > Metadata.size()) return "Corrupt squashfs fragment table";
    std::vector<uint8_t> Compressed;
    if (const auto* Error = ReadDataBlock(U64(Metadata.data() + At), U32(Metadata.data() + At + 8), Compressed, FragmentBytes)) return Error;
    FragmentLoaded = Index;
    Data = FragmentBytes.data();
    return nullptr;
  }

  const char* ReadInode(uint64_t Block, uint32_t Offset, Entry& Out) {
    Cursor At {Super.InodeTable, Block, Offset};
    uint8_t Header[16];
    if (const auto* Error = ReadMeta(At, Header, sizeof(Header))) return Error;
    const uint16_t Kind = U16(Header);
    Out.Mode = U16(Header + 2) & 07777;
    const uint16_t UidIndex = U16(Header + 4), GidIndex = U16(Header + 6);
    if (UidIndex >= Ids.size() || GidIndex >= Ids.size()) return "Corrupt squashfs inode (owner index)";
    Out.Uid = Ids[UidIndex];
    Out.Gid = Ids[GidIndex];
    Out.ModifyTime = U32(Header + 8);
    Out.InodeNumber = U32(Header + 12);
    Out.Links = 1;
    uint8_t Body[40];
    switch (Kind) {
    case 1: // directory
      if (const auto* Error = ReadMeta(At, Body, 16)) return Error;
      Out.Kind = Type::Directory;
      Out.DirectoryBlock = U32(Body);
      Out.Links = U32(Body + 4);
      Out.Size = U16(Body + 8);
      Out.DirectoryOffset = U16(Body + 10);
      return nullptr;
    case 8: // extended directory
      if (const auto* Error = ReadMeta(At, Body, 24)) return Error;
      Out.Kind = Type::Directory;
      Out.Links = U32(Body);
      Out.Size = U32(Body + 4);
      Out.DirectoryBlock = U32(Body + 8);
      Out.DirectoryOffset = U16(Body + 18);
      return nullptr;
    case 2:   // file
    case 9: { // extended file
      Out.Kind = Type::File;
      uint32_t Words = 0;
      if (Kind == 2) {
        if (const auto* Error = ReadMeta(At, Body, 16)) return Error;
        Out.StartBlock = U32(Body);
        Out.Fragment = U32(Body + 4);
        Out.FragmentOffset = U32(Body + 8);
        Out.Size = U32(Body + 12);
      } else {
        if (const auto* Error = ReadMeta(At, Body, 40)) return Error;
        Out.StartBlock = U64(Body);
        Out.Size = U64(Body + 8);
        Out.Links = U32(Body + 24);
        Out.Fragment = U32(Body + 28);
        Out.FragmentOffset = U32(Body + 32);
        (void)Words;
      }
      const uint64_t Full = Out.Size / Super.BlockSize;
      const uint64_t Count = Out.Fragment == 0xFFFFFFFF ? (Out.Size + Super.BlockSize - 1) / Super.BlockSize : Full;
      if (Count > (1u << 24)) return "Corrupt squashfs inode (block count)";
      // Read the list in chunks: a corrupt count fails at the end of the metadata instead of allocating it all up front.
      Out.BlockSizes.clear();
      std::vector<uint8_t> Raw(static_cast<size_t>(std::min<uint64_t>(Count, 4096)) * 4);
      for (uint64_t Done = 0; Done < Count;) {
        const size_t Chunk = static_cast<size_t>(std::min<uint64_t>(Count - Done, 4096));
        if (const auto* Error = ReadMeta(At, Raw.data(), Chunk * 4)) return Error;
        for (size_t Index = 0; Index < Chunk; ++Index) Out.BlockSizes.push_back(U32(Raw.data() + Index * 4));
        Done += Chunk;
      }
      return nullptr;
    }
    case 3:
    case 10: { // symlink
      if (const auto* Error = ReadMeta(At, Body, 8)) return Error;
      Out.Kind = Type::Symlink;
      Out.Links = U32(Body);
      const uint32_t Length = U32(Body + 4);
      if (Length > 4096) return "Corrupt squashfs symlink";
      Out.LinkTarget.resize(Length);
      if (Length)
        if (const auto* Error = ReadMeta(At, Out.LinkTarget.data(), Length)) return Error;
      Out.Size = Length;
      return nullptr;
    }
    case 4:
    case 5:
    case 11:
    case 12: { // block and character devices
      if (const auto* Error = ReadMeta(At, Body, 8)) return Error;
      Out.Kind = (Kind == 4 || Kind == 11) ? Type::BlockDevice : Type::CharDevice;
      Out.Links = U32(Body);
      const uint32_t Rdev = U32(Body + 4);
      // Linux new_decode_dev.
      Out.Major = (Rdev & 0xfff00) >> 8;
      Out.Minor = (Rdev & 0xff) | ((Rdev >> 12) & 0xfff00);
      Out.Device = (uint64_t {Out.Major & 0xfff} << 8) | (Out.Minor & 0xff) | (uint64_t {Out.Minor & ~0xffu} << 12);
      return nullptr;
    }
    case 6:
    case 7:
    case 13:
    case 14: { // fifo and socket
      if (const auto* Error = ReadMeta(At, Body, 4)) return Error;
      Out.Kind = (Kind == 6 || Kind == 13) ? Type::Fifo : Type::Socket;
      Out.Links = U32(Body);
      return nullptr;
    }
    default: return "Unsupported squashfs inode type";
    }
  }

  struct DirectoryItem {
    std::string Name;
    uint64_t Block;
    uint32_t Offset;
  };

  const char* WalkDirectory(const Entry& Directory, int Depth, const VisitFn& Visit) {
    if (Depth > 64) return "The squashfs tree is nested too deeply";
    if (Directory.Size < 3) return nullptr;
    // The directory's listing: headers of up to 256 entries each.
    std::vector<DirectoryItem> Items;
    Cursor At {Super.DirectoryTable, Directory.DirectoryBlock, Directory.DirectoryOffset};
    uint64_t Left = Directory.Size - 3;
    while (Left) {
      uint8_t Header[12];
      if (Left < sizeof(Header)) return "Corrupt squashfs directory";
      if (const auto* Error = ReadMeta(At, Header, sizeof(Header))) return Error;
      Left -= sizeof(Header);
      const uint32_t Count = U32(Header) + 1;
      const uint32_t Start = U32(Header + 4);
      if (Count > 256) return "Corrupt squashfs directory header";
      for (uint32_t Index = 0; Index < Count; ++Index) {
        uint8_t Raw[8];
        if (Left < sizeof(Raw)) return "Corrupt squashfs directory";
        if (const auto* Error = ReadMeta(At, Raw, sizeof(Raw))) return Error;
        const size_t NameSize = size_t {U16(Raw + 6)} + 1;
        Left -= sizeof(Raw);
        if (Left < NameSize || NameSize > 256) return "Corrupt squashfs directory entry";
        DirectoryItem Item;
        Item.Name.resize(NameSize);
        if (const auto* Error = ReadMeta(At, Item.Name.data(), NameSize)) return Error;
        Left -= NameSize;
        if (Item.Name.empty() || Item.Name == "." || Item.Name == ".." || Item.Name.find('/') != std::string::npos ||
            Item.Name.find('\0') != std::string::npos)
          return "Unsafe name in the squashfs directory";
        Item.Block = Start;
        Item.Offset = U16(Raw);
        Items.push_back(std::move(Item));
      }
    }
    for (const auto& Item : Items) {
      Entry Child;
      Child.Path = (Directory.Path == "/" ? "" : Directory.Path) + "/" + Item.Name;
      if (const auto* Error = ReadInode(Item.Block, Item.Offset, Child)) return Error;
      if (Child.Kind != Type::Directory && Child.Links > 1) {
        const auto [Found, Inserted] = LinkedPaths.try_emplace(Child.InodeNumber, Child.Path);
        if (!Inserted) Child.HardLinkTo = Found->second;
      }
      if (const auto* Error = Visit(Child)) return Error;
      if (Child.Kind == Type::Directory) {
        if (const auto* Error = WalkDirectory(Child, Depth + 1, Visit)) return Error;
      }
    }
    return nullptr;
  }

  ImageSource& Source;
  Superblock Super {};
  std::vector<uint32_t> Ids;
  std::vector<uint64_t> FragmentIndex;
  std::vector<uint8_t> Scratch;
  std::vector<CachedBlock> Cache = std::vector<CachedBlock>(16);
  size_t CacheNext {};
  std::vector<uint8_t> FragmentBytes;
  uint32_t FragmentLoaded {0xFFFFFFFF};
  std::map<uint32_t, std::string> LinkedPaths;
};

} // namespace Switch2Guest::Squashfs
