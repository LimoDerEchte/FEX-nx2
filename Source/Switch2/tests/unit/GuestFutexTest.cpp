// SPDX-License-Identifier: MIT
#include "GuestFutex.h"
#include <cstdio>
#include <cstdlib>
#include <map>

#define CHECK(x)                                                     \
  do {                                                               \
    if (!(x)) {                                                      \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x);        \
      std::exit(1);                                                  \
    }                                                                \
  } while (0)

using namespace Switch2Guest;

static uint32_t Encode(uint32_t Op, uint32_t Cmp, uint32_t OpArg, uint32_t CmpArg, bool Shift = false) {
  return (Shift ? 0x80000000u : 0) | (Op << 28) | (Cmp << 24) | ((OpArg & 0xfff) << 12) | (CmpArg & 0xfff);
}

int main() {
  // Keys.
  CHECK(MakeFutexKey(0x1000, 5, true) == MakeFutexKey(0x1000, 9, true));
  CHECK(!(MakeFutexKey(0x1000, 5, false) == MakeFutexKey(0x1000, 9, false)));
  CHECK(MakeFutexKey(0x1000, 5, true).Pid == 0);
  CHECK(MakeFutexKey(0x1000, 5, false).Pid == 5);

  // WAKE_OP: five operations.
  uint32_t Word = 10, Old = 0;
  auto Op = DecodeWakeOp(Encode(0, 0, 7, 0));
  CHECK(ApplyWakeOp(Op, Word, Old) == 0 && Old == 10 && Word == 7);
  Op = DecodeWakeOp(Encode(1, 0, 5, 0));
  CHECK(ApplyWakeOp(Op, Word, Old) == 0 && Old == 7 && Word == 12);
  Op = DecodeWakeOp(Encode(2, 0, 3, 0));
  CHECK(ApplyWakeOp(Op, Word, Old) == 0 && Old == 12 && Word == 15);
  Op = DecodeWakeOp(Encode(3, 0, 6, 0));
  CHECK(ApplyWakeOp(Op, Word, Old) == 0 && Old == 15 && Word == 9);
  Op = DecodeWakeOp(Encode(4, 0, 1, 0));
  CHECK(ApplyWakeOp(Op, Word, Old) == 0 && Old == 9 && Word == 8);

  // Sign extension of 12-bit arguments.
  Op = DecodeWakeOp(Encode(0, 0, 0xfff, 0xfff));
  CHECK(Op.OpArg == -1 && Op.CmpArg == -1);
  Word = 0;
  CHECK(ApplyWakeOp(Op, Word, Old) == 0 && Word == 0xffffffffu);
  Op = DecodeWakeOp(Encode(0, 0, 0x800, 0x7ff));
  CHECK(Op.OpArg == -2048 && Op.CmpArg == 2047);

  // OPARG_SHIFT.
  Op = DecodeWakeOp(Encode(2, 0, 4, 0, true));
  CHECK(Op.Shift && Op.OpArg == 16);
  Word = 1;
  CHECK(ApplyWakeOp(Op, Word, Old) == 0 && Word == 17);
  Op = DecodeWakeOp(Encode(0, 0, 31, 0, true));
  CHECK(static_cast<uint32_t>(Op.OpArg) == 0x80000000u);
  Op = DecodeWakeOp(Encode(0, 0, 33, 0, true)); // 33 & 31 == 1
  CHECK(Op.OpArg == 2);

  // Six comparisons (signed).
  const auto Cmp = [](uint32_t C, int32_t Arg, uint32_t Value) {
    return CompareWakeOp(DecodeWakeOp(Encode(0, C, 0, static_cast<uint32_t>(Arg))), Value);
  };
  CHECK(Cmp(0, 5, 5) && !Cmp(0, 5, 6));
  CHECK(!Cmp(1, 5, 5) && Cmp(1, 5, 6));
  CHECK(Cmp(2, 5, 4) && !Cmp(2, 5, 5));
  CHECK(Cmp(3, 5, 5) && !Cmp(3, 5, 6));
  CHECK(Cmp(4, 5, 6) && !Cmp(4, 5, 5));
  CHECK(Cmp(5, 5, 5) && !Cmp(5, 5, 4));
  CHECK(Cmp(2, 0, 0xffffffffu)); // -1 < 0

  // Invalid op and cmp.
  Word = 3;
  CHECK(ApplyWakeOp(DecodeWakeOp(Encode(5, 0, 1, 0)), Word, Old) == -38 && Word == 3);
  CHECK(ApplyWakeOp(DecodeWakeOp(Encode(7, 0, 1, 0)), Word, Old) == -38);
  CHECK(ApplyWakeOp(DecodeWakeOp(Encode(0, 6, 1, 0)), Word, Old) == -38 && Word == 3);
  CHECK(ApplyWakeOp(DecodeWakeOp(Encode(0, 15, 1, 0)), Word, Old) == -38);

  // TryLockPi.
  uint32_t Pi = 0;
  CHECK(TryLockPi(Pi, 100, true) == PiLock::Acquired && Pi == 100);
  CHECK(TryLockPi(Pi, 100, true) == PiLock::Deadlock && Pi == 100);
  CHECK(TryLockPi(Pi, 101, true) == PiLock::MustWait && Pi == (100 | FutexWaiters));
  CHECK(TryLockPi(Pi, 100, true) == PiLock::Deadlock);
  Pi = 200;
  CHECK(TryLockPi(Pi, 101, false) == PiLock::NoOwner && Pi == 200);
  Pi = FutexOwnerDied;
  CHECK(TryLockPi(Pi, 101, true) == PiLock::Acquired && Pi == (FutexOwnerDied | 101));
  Pi = FutexOwnerDied | FutexWaiters;
  CHECK(TryLockPi(Pi, 102, false) == PiLock::Acquired && Pi == (FutexOwnerDied | FutexWaiters | 102));

  // FutexDeath.
  uint32_t Dead = 300 | FutexWaiters;
  bool Wake = false;
  CHECK(FutexDeath(Dead, 300, Wake) && Wake && Dead == (FutexWaiters | FutexOwnerDied));
  Dead = 300;
  CHECK(FutexDeath(Dead, 300, Wake) && !Wake && Dead == FutexOwnerDied);
  Dead = 301 | FutexWaiters;
  CHECK(!FutexDeath(Dead, 300, Wake) && Dead == (301 | FutexWaiters));
  Dead = 0;
  CHECK(!FutexDeath(Dead, 300, Wake));

  // Robust list. head: next, futex_offset, pending.
  std::map<uint64_t, uint64_t> Memory;
  const auto Read = [&](uint64_t Address, uint64_t& Out) {
    const auto It = Memory.find(Address);
    if (It == Memory.end()) return false;
    Out = It->second;
    return true;
  };
  constexpr uint64_t Head = 0x1000;
  Memory[Head + 8] = 0x40; // futex_offset
  Memory[Head + 16] = 0;
  // Empty circular list.
  Memory[Head] = Head;
  CHECK(WalkRobustList(Head, Read).empty());
  // Two entries, second marked PI; circular back to head.
  Memory[Head] = 0x2000;
  Memory[0x2000] = 0x3000 | 1;
  Memory[0x3000] = Head;
  auto Entries = WalkRobustList(Head, Read);
  CHECK(Entries.size() == 2);
  CHECK(Entries[0].WordAddress == 0x2040 && !Entries[0].Pi);
  CHECK(Entries[1].WordAddress == 0x3040 && Entries[1].Pi);
  // PI bit on the head's first pointer.
  Memory[Head] = 0x2000 | 1;
  Entries = WalkRobustList(Head, Read);
  CHECK(Entries.size() == 2 && Entries[0].Pi && Entries[1].Pi);
  Memory[Head] = 0x2000;
  // Pending entry after the list; a pending entry also on the list is not repeated.
  Memory[Head + 16] = 0x4000 | 1;
  Memory[0x4000] = 0x4000;
  Entries = WalkRobustList(Head, Read);
  CHECK(Entries.size() == 3 && Entries[2].WordAddress == 0x4040 && Entries[2].Pi);
  Memory[Head + 16] = 0x3000;
  Entries = WalkRobustList(Head, Read);
  CHECK(Entries.size() == 2 && Entries[0].WordAddress == 0x2040 && Entries[1].WordAddress == 0x3040);
  Memory[Head + 16] = 0;
  // Fault on a next pointer stops the walk after the entries already seen.
  Memory.erase(0x3000);
  Entries = WalkRobustList(Head, Read);
  CHECK(Entries.size() == 1 && Entries[0].WordAddress == 0x2040);
  // Fault on futex_offset: nothing.
  Memory.erase(Head + 8);
  CHECK(WalkRobustList(Head, Read).empty());
  Memory[Head + 8] = 0x40;
  // Fault on the head: nothing.
  CHECK(WalkRobustList(0x9000, Read).empty());
  // A cycle that never returns to the head stops at the 2048 limit.
  Memory[0x2000] = 0x2000;
  Entries = WalkRobustList(Head, Read);
  CHECK(Entries.size() == RobustListLimit);
  CHECK(RobustListLimit == 2048);
  std::puts("GuestFutex OK");
  return 0;
}
