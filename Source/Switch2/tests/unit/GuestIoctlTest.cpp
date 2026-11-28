// SPDX-License-Identifier: MIT
#include "GuestIoctl.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(Cond) do { if (!(Cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #Cond); std::exit(1); } } while (0)

using namespace Switch2Guest::Ioctl;

int main() {
  // Request numbers (asm-generic/ioctls.h, linux/fs.h, linux/random.h).
  CHECK(FS_IOC_GETFLAGS == ((2u << 30) | (8u << 16) | ('f' << 8) | 1));
  CHECK(FS_IOC_SETFLAGS == ((1u << 30) | (8u << 16) | ('f' << 8) | 2));
  CHECK(FICLONE == ((1u << 30) | (4u << 16) | (0x94 << 8) | 9));
  CHECK(FICLONERANGE == ((1u << 30) | (32u << 16) | (0x94 << 8) | 13));
  CHECK(RNDGETENTCNT == ((2u << 30) | (4u << 16) | ('R' << 8) | 0));
  CHECK(RNDADDTOENTCNT == ((1u << 30) | (4u << 16) | ('R' << 8) | 1));
  CHECK(RNDADDENTROPY == ((1u << 30) | (8u << 16) | ('R' << 8) | 3));
  CHECK(RNDZAPENTCNT == (('R' << 8) | 4));
  CHECK(RNDCLEARPOOL == (('R' << 8) | 6));

  // Inode kinds.
  CHECK(KindOfMode(0100644) == Kind::Regular);
  CHECK(KindOfMode(0040755) == Kind::Directory);
  CHECK(KindOfMode(0020666) == Kind::Other && KindOfMode(0120777) == Kind::Other && KindOfMode(0010600) == Kind::Other);

  // FIONREAD, FIOQSIZE, flags.
  CHECK(FionreadApplies(Kind::Regular) && !FionreadApplies(Kind::Directory) && !FionreadApplies(Kind::Other));
  CHECK(FionreadValue(100, 40) == 60 && FionreadValue(0, 5) == -5);
  CHECK(FioqsizeApplies(Kind::Regular) && FioqsizeApplies(Kind::Directory) && !FioqsizeApplies(Kind::Other));
  CHECK(FileFlagsApply(Kind::Directory) && !FileFlagsApply(Kind::Other));
  CHECK(SetFlagsResult(0, true) == 0);
  CHECK(SetFlagsResult(0x10, true) == Eopnotsupp);
  CHECK(SetFlagsResult(0, false) == Eperm);
  CHECK(BlockSize == 4096);

  // Clone: another mount first, then write access, then the file kinds, then no reflink support.
  CHECK(CloneResult(false, false, Kind::Regular, Kind::Regular) == Exdev);
  CHECK(CloneResult(true, false, Kind::Regular, Kind::Regular) == Ebadf);
  CHECK(CloneResult(true, true, Kind::Directory, Kind::Regular) == Eisdir);
  CHECK(CloneResult(true, true, Kind::Other, Kind::Regular) == Einval);
  CHECK(CloneResult(true, true, Kind::Regular, Kind::Regular) == Eopnotsupp);

  // Random devices.
  CHECK(RandomEntropyCount == 256);
  CHECK(IsRandomPrivileged(RNDADDTOENTCNT) && IsRandomPrivileged(RNDADDENTROPY) && IsRandomPrivileged(RNDZAPENTCNT) && IsRandomPrivileged(RNDCLEARPOOL));
  CHECK(!IsRandomPrivileged(RNDGETENTCNT) && !IsRandomPrivileged(FIONREAD));
  CHECK(RandomPrivilegedResult(true) == 0 && RandomPrivilegedResult(false) == Eperm);

  std::puts("guest-ioctl: ok");
  return 0;
}
