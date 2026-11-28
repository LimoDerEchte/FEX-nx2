// SPDX-License-Identifier: MIT
#include "GuestVmaList.h"

#include <cassert>

using namespace Switch2Guest;

static void TestEdits() {
  VmaList List;
  const Vma File {.Start = 0x1000, .End = 0x6000, .Prot = 1, .Shared = true, .Offset = 0x8000, .DevMinor = 1,
                  .Inode = 10, .Name = "/file", .Backing = Vma::File};
  List.Map(File);
  List.Unmap(0x1000, 0x1000);
  List.Unmap(0x5000, 0x1000);
  List.Unmap(0x3000, 0x1000);
  assert(List.Ranges().size() == 2);
  assert(!List.Find(0x1000) && !List.Find(0x3000) && !List.Find(0x5000));
  assert(List.Find(0x2000)->Offset == 0x9000 && List.Find(0x4000)->Offset == 0xb000);

  List.Map(File);
  List.Protect(0x2000, 0x2000, 3);
  assert(List.Ranges().size() == 3);
  assert(List.Find(0x2fff)->Prot == 3 && List.Find(0x2fff)->Offset == 0x9000);
  assert(List.Find(0x4000)->Prot == 1 && List.Find(0x4000)->Offset == 0xb000);
  List.Protect(0x2000, 0x2000, 1);
  assert(List.Ranges().size() == 1);
  List.Unmap(0x3000, 0x1000);
  List.Protect(0x2000, 0x3000, 3);
  assert(List.Ranges().size() == 4 && !List.Find(0x3000));

  List.Map({.Start = 0x2000, .End = 0x5000, .Prot = 3});
  assert(List.Ranges().size() == 3);
  assert(List.Find(0x1000)->Inode == 10 && List.Find(0x2000)->Inode == 0 && List.Find(0x5000)->Offset == 0xc000);
  List.Map({.Start = 0x5000, .End = 0x7000, .Prot = 3});
  assert(List.Ranges().size() == 2 && List.Find(0x6000)->Start == 0x2000);

  List.Map({.Start = 0x7000, .End = 0x8000, .Prot = 1, .DevMinor = 1, .Inode = 10});
  List.Map({.Start = 0x8000, .End = 0x9000, .Prot = 1, .Offset = 0x2000, .DevMinor = 1, .Inode = 10});
  assert(List.Find(0x7000)->End == 0x8000);
  List.Map({.Start = 0x8000, .End = 0x9000, .Prot = 1, .Offset = 0x1000, .DevMinor = 1, .Inode = 10});
  assert(List.Find(0x7000)->End == 0x9000);
  List.Map({.Start = 0x9000, .End = 0xa000, .Prot = 1, .Offset = 0x2000, .DevMinor = 2, .Inode = 10});
  assert(List.Find(0x7000)->End == 0x9000);
}

static void TestRemap() {
  VmaList List;
  List.Map({.Start = 0x3000, .End = 0x7000, .Prot = 3, .Offset = 0x2000, .Inode = 41, .Backing = Vma::File});
  List.Remap(0x3000, 0x4000, 0x3000, 0x2000);
  assert(List.Find(0x3000)->End == 0x5000 && !List.Find(0x5000));
  List.Remap(0x3000, 0x2000, 0x3000, 0x4000);
  assert(List.Find(0x3000)->End == 0x7000);
  List.Protect(0x5000, 0x2000, 1);
  List.Map({.Start = 0x1000, .End = 0x3000});
  List.Remap(0x4000, 0x3000, 0x1000, 0x3000);
  assert(List.Find(0x1000)->Offset == 0x3000 && List.Find(0x1000)->Prot == 3);
  assert(List.Find(0x2000)->Offset == 0x4000 && List.Find(0x2000)->Prot == 1);
  assert(!List.Find(0x4000) && List.Find(0x3000)->End == 0x4000);
  List.Remap(0x1000, 0x1000, 0x8000, 0x1000, true);
  assert(List.Find(0x1000)->Offset == 0x3000 && List.Find(0x8000)->Offset == 0x3000);
}

static std::string MapLine(std::string Prefix, std::string_view Name) {
  Prefix.resize(73, ' ');
  Prefix += Name;
  return Prefix + '\n';
}

static void TestMaps() {
  VmaList List;
  List.Map({.Start = 0x1000, .End = 0x2000});
  List.Map({.Start = 0x3000, .End = 0x4000, .Prot = 3, .Special = Vma::Heap});
  List.Map({.Start = 0x5000, .End = 0x6000, .Prot = 5, .Offset = 0x7000, .DevMajor = 0xab,
            .DevMinor = 0x123, .Inode = 42, .Name = "/path\nname", .Deleted = true});
  List.Map({.Start = 0x7000, .End = 0x8000, .Prot = 3, .Shared = true, .DevMinor = 1, .Inode = 135,
            .Name = "/memfd:name", .Backing = Vma::Shmem, .Deleted = true});
  assert(FormatMaps(List) == "00001000-00002000 ---p 00000000 00:00 0 \n" +
                              MapLine("00003000-00004000 rw-p 00000000 00:00 0 ", "[heap]") +
                              MapLine("00005000-00006000 r-xp 00007000 ab:123 42 ", "/path\\012name (deleted)") +
                              MapLine("00007000-00008000 rw-s 00000000 00:01 135 ", "/memfd:name (deleted)"));
}

static void TestSmaps() {
  VmaList List;
  List.Map({.Start = 0x1000, .End = 0x3000, .Prot = 3, .Inode = 1, .Backing = Vma::File, .Accounted = true});
  const auto Resident = [](const Vma& Range) { return Range.Start == 0x1000 ? 1 : (Range.End - Range.Start) / 4096; };
  const auto Sharing = [](const Vma& Range) { return Range.Inode == 4 ? 1 : 3; };
  assert(FormatSmaps(List, Resident, Sharing) ==
         "00001000-00003000 rw-p 00000000 00:00 1 \n"
         "Size:                  8 kB\n"
         "KernelPageSize:        4 kB\n"
         "MMUPageSize:           4 kB\n"
         "Rss:                   4 kB\n"
         "Pss:                   4 kB\n"
         "Shared_Clean:          0 kB\n"
         "Shared_Dirty:          0 kB\n"
         "Private_Clean:         0 kB\n"
         "Private_Dirty:         4 kB\n"
         "Referenced:            4 kB\n"
         "Anonymous:             4 kB\n"
         "LazyFree:              0 kB\n"
         "AnonHugePages:         0 kB\n"
         "ShmemPmdMapped:        0 kB\n"
         "FilePmdMapped:         0 kB\n"
         "Shared_Hugetlb:        0 kB\n"
         "Private_Hugetlb:       0 kB\n"
         "Swap:                  0 kB\n"
         "SwapPss:               0 kB\n"
         "Locked:                0 kB\n"
         "THPeligible:    0\n"
         "VmFlags: rd wr mr mw me ac \n");
  List.Map({.Start = 0x3000, .End = 0x4000, .Prot = 5, .Inode = 1, .Backing = Vma::File, .MainText = true});
  List.Map({.Start = 0x5000, .End = 0x8000, .Prot = 3, .Shared = true, .Inode = 2, .Backing = Vma::Shmem});
  List.Map({.Start = 0x9000, .End = 0xa000, .Prot = 1, .Shared = true, .Inode = 3, .Backing = Vma::File});
  List.Map({.Start = 0xa000, .End = 0xb000, .Prot = 1, .Shared = true, .Inode = 4, .Backing = Vma::Shmem});
  assert(List.Find(0x5000)->SameSharedObject(*List.Find(0x5000)));
  assert(!List.Find(0x5000)->SameSharedObject(*List.Find(0xa000)));
  const auto Total = CountVmas(List, Resident);
  assert(Total.Size == 32 * 1024 && Total.Rss == 28 * 1024);
  assert(Total.RssAnon == 4096 && Total.RssFile == 8192 && Total.RssShmem == 16384);
  assert(Total.Text == 4096 && Total.Data == 8192 && Total.Executable == 4096);
  assert(FormatSmapsRollup(List, Resident, Sharing) ==
         MapLine("00001000-0000b000 ---p 00000000 00:00 0 ", "[rollup]") +
         "Rss:                  28 kB\n"
         "Pss:                  17 kB\n"
         "Pss_Anon:              4 kB\n"
         "Pss_File:              5 kB\n"
         "Pss_Shmem:             7 kB\n"
         "Shared_Clean:          4 kB\n"
         "Shared_Dirty:         12 kB\n"
         "Private_Clean:         8 kB\n"
         "Private_Dirty:         4 kB\n"
         "Referenced:           28 kB\n"
         "Anonymous:             4 kB\n"
         "LazyFree:              0 kB\n"
         "AnonHugePages:         0 kB\n"
         "ShmemPmdMapped:        0 kB\n"
         "FilePmdMapped:         0 kB\n"
         "Shared_Hugetlb:        0 kB\n"
         "Private_Hugetlb:       0 kB\n"
         "Swap:                  0 kB\n"
         "SwapPss:               0 kB\n"
         "Locked:                0 kB\n");
}

static void TestSharing() {
  // Two mappings of the same file share pages only where their file ranges overlap.
  const Vma A {.Start = 0x1000, .End = 0x3000, .Shared = true, .Offset = 0, .DevMinor = 1, .Inode = 7,
               .Backing = Vma::File};
  Vma B = A;
  B.Offset = 0x1000;
  assert(A.SameSharedObject(B));
  B.Offset = 0x2000;
  assert(!A.SameSharedObject(B));
  // A shared anonymous object is told apart by its object address, not its inode.
  Vma C {.Start = 0x1000, .End = 0x2000, .Shared = true, .Inode = 5, .Backing = Vma::Shmem, .Object = 0x1000};
  Vma D = C;
  D.Object = 0x9000;
  assert(C.SameSharedObject(C) && !C.SameSharedObject(D));
}

static void TestProtectAccounting() {
  VmaList List;
  List.Map({.Start = 0x1000, .End = 0x4000, .Prot = 1});
  List.Protect(0x2000, 0x1000, 3);
  assert(!List.Find(0x1000)->Accounted && List.Find(0x2000)->Accounted);
  // Dropping write access keeps the charge, as Linux does.
  List.Protect(0x2000, 0x1000, 1);
  assert(List.Find(0x2000)->Accounted);
  List.Map({.Start = 0x8000, .End = 0x9000, .Prot = 1, .Shared = true, .Inode = 1, .Backing = Vma::Shmem});
  List.Protect(0x8000, 0x1000, 3);
  assert(!List.Find(0x8000)->Accounted);
}

int main() {
  TestSharing();
  TestProtectAccounting();
  TestEdits();
  TestRemap();
  TestMaps();
  TestSmaps();
}
