// SPDX-License-Identifier: MIT
#include "../GuestMemoryFile.h"

#include <cassert>
#include <cstdio>

using Switch2Guest::GuestMemoryFile;
using Switch2Guest::PageSize;

static bool Zero(const GuestMemoryFile& File, uint64_t Begin, uint64_t End) {
  const auto* Bytes = static_cast<const uint8_t*>(File.Storage.get());
  for (auto Offset = Begin; Offset < End; ++Offset) if (Bytes[Offset]) return false;
  return true;
}

int main() {
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
  assert(static_cast<char*>(File.Storage.get())[PageSize + 99] == 'x');
  assert(static_cast<char*>(File.Storage.get())[4 * PageSize + 100] == 'x');
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
  static_cast<char*>(File.Storage.get())[3 * PageSize - 1] = 'z';
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
  assert(File.Write(Switch2Guest::GuestAddressSpace::MaxMapping, Data, 1) == -27);
  std::puts("guest memory file checks passed");
}
