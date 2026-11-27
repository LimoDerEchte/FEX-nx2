// SPDX-License-Identifier: MIT
// Pure futex logic: keys, FUTEX_WAKE_OP math, PI word transitions and the
// robust-list walk. No guest memory is touched here; callers pass readers.
#pragma once
#include <compare>
#include <cstdint>
#include <vector>

namespace Switch2Guest {

struct FutexKey {
  uint64_t Address {};
  uint32_t Pid {};
  auto operator<=>(const FutexKey&) const = default;
};

// Pid is 0 when the address lies in a shared mapping (every process sees the
// same host address, so the key is global); otherwise it is the owning
// process. Private and shared ops on private memory give the same key, like
// Linux (mm, address) keys.
inline FutexKey MakeFutexKey(uint64_t Address, uint32_t Pid, bool AddressIsShared) {
  return {Address, AddressIsShared ? 0u : Pid};
}

struct WakeOp {
  uint32_t Op {}, Cmp {};
  int32_t OpArg {}, CmpArg {};
  bool Shift {};
};

constexpr int32_t SignExtend12(uint32_t Value) { return static_cast<int32_t>((Value & 0xfff) << 20) >> 20; }

// Linux futex_atomic_op_inuser decoding. With the OPARG_SHIFT flag (bit 31)
// OpArg is already 1 << (arg & 31).
inline WakeOp DecodeWakeOp(uint32_t Val3) {
  WakeOp Result;
  Result.Op = (Val3 >> 28) & 7;
  Result.Cmp = (Val3 >> 24) & 15;
  Result.OpArg = SignExtend12(Val3 >> 12);
  Result.CmpArg = SignExtend12(Val3);
  Result.Shift = Val3 & 0x80000000u;
  if (Result.Shift) Result.OpArg = static_cast<int32_t>(1u << (static_cast<uint32_t>(Result.OpArg) & 31));
  return Result;
}

// -38 (ENOSYS) for Op > 4 or Cmp > 5, else 0. Old receives the prior word;
// Word is unchanged on error.
inline int ApplyWakeOp(const WakeOp& Op, uint32_t& Word, uint32_t& Old) {
  if (Op.Op > 4 || Op.Cmp > 5) return -38;
  Old = Word;
  const auto Arg = static_cast<uint32_t>(Op.OpArg);
  switch (Op.Op) {
  case 0: Word = Arg; break;
  case 1: Word = Old + Arg; break;
  case 2: Word = Old | Arg; break;
  case 3: Word = Old & ~Arg; break;
  default: Word = Old ^ Arg; break;
  }
  return 0;
}

inline bool CompareWakeOp(const WakeOp& Op, uint32_t Old) {
  const auto Value = static_cast<int32_t>(Old);
  switch (Op.Cmp) {
  case 0: return Value == Op.CmpArg;
  case 1: return Value != Op.CmpArg;
  case 2: return Value < Op.CmpArg;
  case 3: return Value <= Op.CmpArg;
  case 4: return Value > Op.CmpArg;
  case 5: return Value >= Op.CmpArg;
  default: return false;
  }
}

constexpr uint32_t FutexWaiters = 0x80000000, FutexOwnerDied = 0x40000000, FutexTidMask = 0x3fffffff;

// PI lock attempt on Word by Tid. MustWait sets FutexWaiters in Word. NoOwner
// (ESRCH) means the owner TID is not alive and OWNER_DIED is not set.
enum class PiLock { Acquired, Deadlock, MustWait, NoOwner };
inline PiLock TryLockPi(uint32_t& Word, uint32_t Tid, bool OwnerAlive) {
  const auto Owner = Word & FutexTidMask;
  if (!Owner) {
    Word = (Word & (FutexWaiters | FutexOwnerDied)) | Tid;
    return PiLock::Acquired;
  }
  if (Owner == Tid) return PiLock::Deadlock;
  if (!OwnerAlive) return PiLock::NoOwner;
  Word |= FutexWaiters;
  return PiLock::MustWait;
}

struct RobustEntry {
  uint64_t WordAddress {};
  bool Pi {};
};

constexpr size_t RobustListLimit = 2048;

// Linux exit_robust_list. Head is a robust_list_head {next, futex_offset,
// list_op_pending}. Read(Address, uint64_t& Out) returns false on a fault.
// Bit 0 of a list pointer marks a PI futex. Stops after RobustListLimit
// entries or on a fault; the pending entry follows the list.
template<typename Read64>
std::vector<RobustEntry> WalkRobustList(uint64_t Head, Read64 Read) {
  std::vector<RobustEntry> Result;
  uint64_t Next {}, Offset {}, Pending {};
  if (!Read(Head, Next) || !Read(Head + 8, Offset) || !Read(Head + 16, Pending)) return Result;
  uint64_t Entry = Next & ~uint64_t {1};
  bool Pi = Next & 1;
  const uint64_t PendingEntry = Pending & ~uint64_t {1};
  size_t Limit = RobustListLimit;
  bool Complete = true;
  while (Entry != Head) {
    uint64_t Following {};
    if (!Read(Entry, Following)) {
      Complete = false;
      break;
    }
    if (Entry != PendingEntry) Result.push_back({Entry + Offset, Pi});
    Entry = Following & ~uint64_t {1};
    Pi = Following & 1;
    if (!--Limit) break;
  }
  if (!Complete) return Result; // Linux returns without the pending entry
  if (PendingEntry) Result.push_back({PendingEntry + Offset, static_cast<bool>(Pending & 1)});
  return Result;
}

// Linux handle_futex_death: a word owned by Tid becomes
// (Word & FutexWaiters) | FutexOwnerDied. Wake reports whether a waiter
// exists. Returns true when the word changed.
inline bool FutexDeath(uint32_t& Word, uint32_t Tid, bool& Wake) {
  Wake = false;
  if ((Word & FutexTidMask) != Tid) return false;
  Wake = Word & FutexWaiters;
  Word = (Word & FutexWaiters) | FutexOwnerDied;
  return true;
}

} // namespace Switch2Guest
