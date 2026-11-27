// SPDX-License-Identifier: MIT
#include "GuestInotify.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      std::exit(1);                                           \
    }                                                         \
  } while (0)

using namespace Switch2Guest;

static InotifyEvent Next(InotifyInstance& I) {
  uint8_t Buffer[64];
  // The smallest buffer that takes the first event reads exactly one.
  int64_t Size = -22;
  for (size_t Limit = InotifyHeaderSize; Size == -22 && Limit <= sizeof(Buffer); Limit += InotifyHeaderSize) {
    Size = I.Read(Buffer, Limit);
  }
  InotifyEvent Event {};
  if (Size < 16) return {0, 0, 0, "<none>"};
  std::memcpy(&Event.Wd, Buffer, 4);
  std::memcpy(&Event.Mask, Buffer + 4, 4);
  std::memcpy(&Event.Cookie, Buffer + 8, 4);
  uint32_t Length;
  std::memcpy(&Length, Buffer + 12, 4);
  Event.Name = Length ? reinterpret_cast<const char*>(Buffer + 16) : "";
  if (Size != static_cast<int64_t>(16 + Length)) return {0, 0, 0, "<partial>"};
  return Event;
}

int main() {
  CHECK(EncodedSize({1, 0, 0, ""}) == 16);
  CHECK(EncodedSize({1, 0, 0, "a"}) == 32);
  CHECK(EncodedSize({1, 0, 0, std::string(15, 'x')}) == 32);
  CHECK(EncodedSize({1, 0, 0, std::string(16, 'x')}) == 48);

  InotifyInstance I;
  const auto Dir = I.AddWatch(10, InCreate | InDelete, true);
  CHECK(Dir == 1);
  CHECK(I.AddWatch(10, InModify, true) == 1);                  // same inode, mask replaced
  I.Emit(10, InCreate, 0, "f", false);
  CHECK(!I.Readable());                                         // filtered: no IN_CREATE any more
  CHECK(I.AddWatch(10, InCreate | InMaskAdd, true) == 1);      // OR'd
  CHECK(I.AddWatch(10, InCreate | InMaskCreate, true) == -17);
  I.Emit(10, InCreate, 0, "f", false);
  I.Emit(10, InCreate, 0, "f", false);                         // identical tail coalesced
  I.Emit(10, InModify, 0, "d", true);
  CHECK(I.Queued() == 2);
  CHECK(I.PendingBytes() == 64);
  uint8_t Small[15];
  CHECK(I.Read(Small, sizeof(Small)) == -22);
  const auto First = Next(I);
  CHECK(First.Wd == 1 && First.Mask == InCreate && First.Name == "f");
  const auto Second = Next(I);
  CHECK(Second.Mask == (InModify | InIsDir) && Second.Name == "d");
  CHECK(I.Read(Small, sizeof(Small)) == -11);

  CHECK(I.RemoveWatch(1) == 0);
  CHECK(I.RemoveWatch(1) == -22);
  const auto Ignored = Next(I);
  CHECK(Ignored.Wd == 1 && Ignored.Mask == InIgnored && Ignored.Name.empty());

  // One-shot: the first event ends the watch with IN_IGNORED.
  const auto Once = I.AddWatch(20, InOpen | InOneShot, false);
  CHECK(Once == 2);
  I.Emit(20, InOpen, 0, "", false);
  I.Emit(20, InOpen, 0, "", false);
  CHECK(Next(I).Mask == InOpen && Next(I).Mask == InIgnored && !I.Readable() && !I.Watching(20));

  // Self deletion.
  CHECK(I.AddWatch(30, InDeleteSelf, false) == 3);
  I.InodeGone(30);
  CHECK(Next(I).Mask == InDeleteSelf && Next(I).Mask == InIgnored && !I.Watching(30));

  // IN_EXCL_UNLINK suppresses events about unlinked children.
  I.AddWatch(40, InModify | InExclUnlink, true);
  I.Emit(40, InModify, 0, "gone", false, true);
  CHECK(!I.Readable());

  // Overflow: one IN_Q_OVERFLOW with wd -1 once the queue is full.
  InotifyInstance Full;
  Full.AddWatch(1, InModify, true);
  for (size_t Index = 0; Index < InotifyMaxQueued + 5; ++Index) Full.Emit(1, InModify, 0, std::to_string(Index), false);
  CHECK(Full.Queued() == InotifyMaxQueued + 1);
  std::puts("guest inotify checks passed");
}
