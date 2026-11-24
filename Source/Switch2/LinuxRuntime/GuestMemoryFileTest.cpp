// SPDX-License-Identifier: MIT
#include "../GuestMemoryFile.h"

#include <cassert>
#include <cstdio>
#include <sys/mman.h>

using Switch2Guest::ArenaBackend;
using Switch2Guest::GuestArena;
using Switch2Guest::GuestMemoryFile;
using Switch2Guest::PageSize;

// Uncommitted arena memory is PROT_NONE: touching it kills the test.
struct HostBackend : ArenaBackend {
  bool Commit(uint64_t Address, size_t Size) override {
    return mprotect(reinterpret_cast<void*>(Address), Size, PROT_READ | PROT_WRITE) == 0;
  }
  bool Decommit(uint64_t Address, size_t Size) override {
    return madvise(reinterpret_cast<void*>(Address), Size, MADV_DONTNEED) == 0 &&
           mprotect(reinterpret_cast<void*>(Address), Size, PROT_NONE) == 0;
  }
};

static uint8_t At(const GuestMemoryFile& File, uint64_t Offset) {
  uint8_t Byte {};
  File.Read(Offset, &Byte, 1);
  return Byte;
}

static bool Zero(const GuestMemoryFile& File, uint64_t Begin, uint64_t End) {
  for (auto Offset = Begin; Offset < End; ++Offset) if (At(File, Offset)) return false;
  return true;
}

int main() {
  constexpr size_t G = GuestArena::Granule;
  constexpr size_t ArenaBytes = 3 * GuestMemoryFile::MaxFileSize;
  void* Map = mmap(nullptr, ArenaBytes + G, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  assert(Map != MAP_FAILED);
  const uint64_t ArenaBase = (reinterpret_cast<uint64_t>(Map) + G - 1) & ~(G - 1);
  HostBackend Backend;
  GuestArena Arena {Backend, ArenaBase, ArenaBytes, 64 * G};
  GuestArena::Current() = &Arena;
  // Truncation growth is sparse and counts no blocks.
  GuestMemoryFile File;
  assert(File.Resize(8 * PageSize) == 0);
  assert(File.AllocatedBytes() == 0);
  assert(!File.Seek(0, true) && File.Seek(0, false) == 0);

  // Writes allocate only the pages they touch.
  const char Data[] = "payload";
  assert(File.Write(2 * PageSize + 10, Data, sizeof(Data)) == 0);
  assert(File.AllocatedBytes() == PageSize);
  assert(File.Seek(0, true) == 2 * PageSize && File.Seek(2 * PageSize + 5, true) == 2 * PageSize + 5);
  assert(File.Seek(2 * PageSize, false) == 3 * PageSize);
  assert(File.Seek(7 * PageSize, false) == 7 * PageSize);

  // Full-page punch frees pages; partial pages stay allocated and zero.
  std::vector<char> Fill(4 * PageSize, 'x');
  assert(File.Write(PageSize, Fill.data(), Fill.size()) == 0);
  assert(File.AllocatedBytes() == 4 * PageSize);
  assert(File.Punch(PageSize + 100, 4 * PageSize + 100) == 0);
  assert(File.AllocatedBytes() == 2 * PageSize);
  assert(Zero(File, PageSize + 100, 4 * PageSize + 100));
  assert(At(File, PageSize + 99) == 'x');
  assert(At(File, 4 * PageSize + 100) == 'x');
  assert(File.Seek(PageSize, false) == 2 * PageSize && File.Seek(2 * PageSize, true) == 4 * PageSize);
  assert(File.Size == 8 * PageSize);

  // Punching beyond EOF and holes is harmless.
  assert(File.Punch(20 * PageSize, 30 * PageSize) == 0 && File.Size == 8 * PageSize);

  // A grow seal rejects any allocation past EOF, KEEP_SIZE included.
  File.Seals = GuestMemoryFile::SealGrow;
  const auto Allocated = File.AllocatedBytes();
  assert(File.Allocate(8 * PageSize, 10 * PageSize, true) == -1);
  assert(File.Allocate(8 * PageSize, 10 * PageSize, false) == -1);
  assert(File.AllocatedBytes() == Allocated);
  File.Seals = 0;
  // KEEP_SIZE preallocation beyond EOF counts blocks but keeps the size.
  // Truncation frees it.
  assert(File.Allocate(8 * PageSize, 10 * PageSize, true) == 0);
  assert(File.Size == 8 * PageSize && File.AllocatedBytes() == 4 * PageSize);
  assert(File.Resize(8 * PageSize) == 0 && File.AllocatedBytes() == 2 * PageSize);

  // Shrinking zeroes the partial tail so later growth reads zero.
  assert(File.Resize(PageSize + 50) == 0);
  assert(File.AllocatedBytes() == PageSize);
  assert(File.Resize(3 * PageSize) == 0 && Zero(File, PageSize + 50, 3 * PageSize));

  // A shared mapping can dirty bytes past EOF in the last page.
  assert(File.MarkAllocated(2 * PageSize, 3 * PageSize) == 0);
  reinterpret_cast<char*>(File.Base())[3 * PageSize - 1] = 'z';
  assert(File.Resize(3 * PageSize - 10) == 0);
  assert(File.Resize(3 * PageSize) == 0 && Zero(File, 3 * PageSize - 10, 3 * PageSize));

  // Seals.
  File.Seals = GuestMemoryFile::SealWrite;
  assert(File.Write(0, Data, 1) == -1 && File.Punch(0, PageSize) == -1);
  assert(File.Write(0, Data, 0) == 0);
  File.Seals = GuestMemoryFile::SealShrink;
  assert(File.Resize(PageSize) == -1);
  File.Seals = GuestMemoryFile::SealGrow;
  assert(File.Write(3 * PageSize, Data, 1) == -1);
  File.Seals = 0;

  // A zero-length write past EOF does not extend.
  assert(File.Write(10 * PageSize, Data, 0) == 0 && File.Size == 3 * PageSize);
  assert(File.Write(GuestMemoryFile::MaxFileSize, Data, 1) == -27);

  // Only present pages are committed; punching and truncation give them back.
  {
    GuestMemoryFile Sparse;
    const auto Before = Arena.Committed();
    assert(Sparse.Resize(64 * PageSize) == 0 && Arena.Committed() == Before);
    assert(Sparse.Write(40 * PageSize, Data, sizeof(Data)) == 0);
    assert(Arena.Committed() == Before + G && Arena.IsCommitted(Sparse.Base() + 40 * PageSize, PageSize));
    assert(!Arena.IsCommitted(Sparse.Base(), PageSize));
    char Back[sizeof(Data)] {};
    Sparse.Read(40 * PageSize, Back, sizeof(Back));
    assert(Back[0] == 'p' && At(Sparse, 3 * PageSize) == 0);
    assert(Sparse.Punch(40 * PageSize, 41 * PageSize) == 0 && Arena.Committed() == Before);
    assert(Zero(Sparse, 40 * PageSize, 41 * PageSize));
    assert(Sparse.Write(PageSize, Data, 3) == 0 && Sparse.Resize(0) == 0 && Arena.Committed() == Before);
    // A mapping pins the pages: they are zeroed but stay committed.
    assert(Sparse.Resize(8 * PageSize) == 0 && Sparse.Write(0, Data, 3) == 0);
    auto Mapping = Sparse.Storage;
    assert(Sparse.Punch(0, PageSize) == 0 && Arena.IsCommitted(Sparse.Base(), PageSize));
    assert(reinterpret_cast<char*>(Sparse.Base())[0] == 0);
    Mapping.reset();
    // Mapped pages beyond Present commit on demand, and fail cleanly when the arena is full.
    GuestMemoryFile Big;
    assert(Big.Resize(GuestMemoryFile::MaxFileSize) == 0);
    assert(Big.Allocate(0, 100 * G, true) == -28 && Big.AllocatedBytes() == 0);
    assert(Big.Allocate(0, 4 * G, true) == 0 && Big.AllocatedBytes() == 4 * G);
  }
  // The reservation follows the size: small files take MinReservation (the
  // old fixed 1 GiB made 200 of them need 200 GiB of address space), grow in
  // place when they can, and move (contents kept) when nothing maps them.
  {
    std::vector<GuestMemoryFile> Many(200);
    for (auto& Small : Many) assert(Small.Resize(PageSize) == 0 && Small.Capacity == GuestMemoryFile::MinReservation);
    Many.clear();
    GuestMemoryFile Growing;
    assert(Growing.Write(0, Data, sizeof(Data)) == 0);
    const auto Committed = Arena.Committed();
    assert(Growing.Resize(3 * G) == 0 && Growing.Capacity == 4 * G && Arena.Committed() == Committed);
    // Hold the address space after the reservation so it cannot grow in place.
    const auto Before = Growing.Base();
    const bool Blocked = Arena.ReserveAt(Before + Growing.Capacity, PageSize);
    assert(Blocked);
    auto Pin = Growing.Storage; // a mapping pins the storage: it cannot move
    assert(Growing.Resize(5 * G) == -12 && Growing.Base() == Before && Growing.Size == 3 * G);
    Pin.reset();
    assert(Growing.Resize(5 * G) == 0 && Growing.Capacity == 8 * G && Growing.Base() != Before);
    char Back[sizeof(Data)] {};
    Growing.Read(0, Back, sizeof(Back));
    assert(Back[0] == 'p' && Growing.AllocatedBytes() == PageSize && Arena.Committed() == Committed);
    Arena.Release(Before + 4 * G, PageSize);
  }
  std::puts("guest memory file checks passed");
}
