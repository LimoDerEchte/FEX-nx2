// SPDX-License-Identifier: MIT
#pragma once

// Pure logic of the scheduling, I/O priority, kcmp and machine-administration syscalls (Milestone 4, task 1).
// The runtime owns the per-thread storage and memory access; this header holds the Linux decisions so they
// can be tested on the host (LinuxRuntime/GuestSchedTest.cpp). The reference is kernel/sched/syscalls.c and
// block/ioprio.c of Linux 7.3, checked against a native run (see ContainerInventory.md).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Switch2Guest {
// Negated Linux errno values.
constexpr int64_t ErrPerm = -1, ErrNoent = -2, ErrSrch = -3, Err2big = -7, ErrBadf = -9, ErrAgain = -11, ErrInval = -22;
}

namespace Switch2Guest::Sched {
// Policies. SCHED_ISO (4) and SCHED_EXT (7) do not exist in the emulated kernel (EINVAL).
constexpr int32_t Other = 0, Fifo = 1, Rr = 2, Batch = 3, Idle = 5, Deadline = 6;
constexpr uint32_t PolicyResetOnForkBit = 0x40000000;
constexpr uint32_t FlagResetOnFork = 0x01, FlagReclaim = 0x02, FlagDlOverrun = 0x04, FlagKeepPolicy = 0x08, FlagKeepParams = 0x10,
                   FlagUtilMin = 0x20, FlagUtilMax = 0x40, FlagAll = 0x7f;
constexpr uint32_t UtilScale = 1024, AttrSizeVer0 = 48, AttrSizeVer1 = 56, AttrSizeMax = 4096;
constexpr uint32_t MaxRtPriority = 99;
// EEVDF base slice (0.7 ms scaled by 1 + log2 of the CPU count) and its sched_attr clamp.
constexpr uint64_t DefaultSlice = 2'800'000, MinSlice = 100'000, MaxSlice = 100'000'000;
constexpr uint64_t RrTimeslice = 100'000'000; // RR_TIMESLICE, 100 ms
constexpr uint64_t JiffyNs = 1'000'000;       // sched_rr_get_interval reports whole jiffies (HZ 1000)

constexpr bool ValidPolicy(int64_t Policy) {
  return Policy == Other || Policy == Fifo || Policy == Rr || Policy == Batch || Policy == Idle || Policy == Deadline;
}
constexpr bool IsRt(int64_t Policy) { return Policy == Fifo || Policy == Rr; }
constexpr bool IsFair(int64_t Policy) { return Policy == Other || Policy == Batch; }

// sched_get_priority_max and _min.
inline int64_t PriorityMax(int64_t Policy) { return IsRt(Policy) ? MaxRtPriority : ValidPolicy(Policy) ? 0 : ErrInval; }
inline int64_t PriorityMin(int64_t Policy) { return IsRt(Policy) ? 1 : ValidPolicy(Policy) ? 0 : ErrInval; }

// What one thread has stored. The nice value lives in the process (the runtime shares it between threads).
struct State {
  int32_t Policy {Other};
  bool ResetOnFork {};
  uint32_t Priority {};            // rt_priority; 0 outside FIFO and RR
  uint64_t Slice {DefaultSlice};   // the fair class's sched_runtime
  uint64_t Runtime {}, Deadline {}, Period {}; // SCHED_DEADLINE parameters
  uint32_t DlFlags {};             // FlagReclaim, FlagDlOverrun
  uint32_t UtilMin {}, UtilMax {UtilScale};
  bool UtilMinSet {}, UtilMaxSet {};
};

// One change request: sched_setscheduler, sched_setparam or sched_setattr.
struct Request {
  int64_t Policy {-1};   // -1 keeps the policy (sched_setparam, SCHED_FLAG_KEEP_POLICY)
  uint32_t Flags {};     // SCHED_FLAG_*; FlagResetOnFork carries sched_setscheduler's policy bit
  int32_t Nice {};
  uint32_t Priority {};
  uint64_t Runtime {}, Deadline {}, Period {};
  uint32_t UtilMin {}, UtilMax {};
  bool FromAttr {};      // sched_setattr: sched_runtime sets the fair slice
};

// Who asks and which limits of the target apply.
struct Actor {
  bool CapNice {};         // CAP_SYS_NICE
  bool SameOwner {};       // check_same_owner: the caller's euid is the target's euid or uid
  uint64_t RtprioLimit {}; // the target's RLIMIT_RTPRIO soft limit
  uint64_t NiceLimit {};   // the target's RLIMIT_NICE soft limit (20 - nice)
};

inline bool CanNice(const Actor& Who, int32_t Nice) { return Who.CapNice || static_cast<int64_t>(20) - Nice <= static_cast<int64_t>(Who.NiceLimit); }

// __checkparam_dl.
inline bool CheckDeadline(const Request& R) {
  constexpr uint64_t HighBit = uint64_t {1} << 63;
  if (!R.Deadline || R.Runtime < 1024) return false;
  if ((R.Deadline & HighBit) || (R.Period & HighBit)) return false;
  const auto Period = R.Period ? R.Period : R.Deadline;
  return Period >= R.Deadline && R.Deadline >= R.Runtime;
}

// __sched_setscheduler for a user caller: validates and applies. Nice is the process's value and changes only for
// a fair-class policy. Returns 0 or a negative errno and leaves State and Nice untouched on failure.
inline int64_t Apply(State& S, int32_t& Nice, const Request& R, const Actor& Who) {
  int64_t Policy = R.Policy;
  bool Reset = R.Flags & FlagResetOnFork;
  if (Policy < 0) {
    Policy = S.Policy;
    Reset = S.ResetOnFork;
  } else if (!ValidPolicy(Policy)) {
    return ErrInval;
  }
  if (R.Flags & ~FlagAll) return ErrInval; // includes SCHED_FLAG_SUGOV, which only the kernel may set
  if (R.Priority > MaxRtPriority) return ErrInval;
  if (Policy == Deadline && !CheckDeadline(R)) return ErrInval;
  if (IsRt(Policy) != (R.Priority != 0)) return ErrInval;
  if (R.Flags & (FlagUtilMin | FlagUtilMax)) {
    const bool Min = R.Flags & FlagUtilMin, Max = R.Flags & FlagUtilMax;
    if ((Min && R.UtilMin > UtilScale) || (Max && R.UtilMax > UtilScale) || (Min && Max && R.UtilMin > R.UtilMax)) return ErrInval;
  }
  if (!Who.CapNice) {
    if (IsFair(Policy) && R.Nice < Nice && !CanNice(Who, R.Nice)) return ErrPerm;
    if (IsRt(Policy)) {
      if (Policy != S.Policy && !Who.RtprioLimit) return ErrPerm;
      if (R.Priority > S.Priority && R.Priority > Who.RtprioLimit) return ErrPerm;
    }
    if (Policy == Deadline) return ErrPerm;
    // SCHED_IDLE counts as nice 20: leaving it needs the right to have the current nice.
    if (S.Policy == Idle && Policy != Idle && !CanNice(Who, Nice)) return ErrPerm;
    if (!Who.SameOwner) return ErrPerm;
    if (S.ResetOnFork && !Reset) return ErrPerm;
  }
  S.ResetOnFork = Reset;
  // SCHED_FLAG_KEEP_PARAMS: everything is validated, but only the flags and clamps change (native 7.3).
  if (!(R.Flags & FlagKeepParams)) {
    S.Policy = static_cast<int32_t>(Policy);
    S.Priority = 0;
    if (Policy == Deadline) {
      S.Runtime = R.Runtime;
      S.Deadline = R.Deadline;
      S.Period = R.Period ? R.Period : R.Deadline;
      S.DlFlags = R.Flags & (FlagReclaim | FlagDlOverrun);
    } else {
      S.Runtime = S.Deadline = S.Period = 0;
      S.DlFlags = 0;
      if (IsRt(Policy)) {
        S.Priority = R.Priority;
      } else if (IsFair(Policy)) {
        Nice = R.Nice;
        if (R.FromAttr) S.Slice = R.Runtime ? std::clamp(R.Runtime, MinSlice, MaxSlice) : DefaultSlice;
      }
    }
  }
  if (R.Flags & FlagUtilMin) {
    S.UtilMin = R.UtilMin;
    S.UtilMinSet = true;
  }
  if (R.Flags & FlagUtilMax) {
    S.UtilMax = R.UtilMax;
    S.UtilMaxSet = true;
  }
  return 0;
}

// sched_fork: what a new task inherits. A SCHED_DEADLINE parent that did not ask for reset-on-fork cannot fork
// (EAGAIN); reset-on-fork turns a real-time or deadline child into SCHED_OTHER nice 0 and a negative nice into 0.
inline int64_t ForkInherit(State& Child, int32_t& Nice) {
  if (Child.ResetOnFork) {
    if (Child.Policy == Deadline || IsRt(Child.Policy)) {
      Child = State {};
      Nice = 0;
    } else if (Nice < 0) {
      Nice = 0;
    }
    Child.ResetOnFork = false;
    return 0;
  }
  return Child.Policy == Deadline ? ErrAgain : 0;
}

// sched_rr_get_interval, in nanoseconds: RR has the fixed slice, FIFO and deadline none, the fair classes their
// slice truncated to a jiffy.
inline uint64_t RrInterval(const State& S) {
  if (S.Policy == Rr) return RrTimeslice;
  if (S.Policy == Fifo || S.Policy == Deadline) return 0;
  return S.Slice / JiffyNs * JiffyNs;
}

// struct sched_attr (SCHED_ATTR_SIZE_VER1).
struct Attr {
  uint32_t Size {}, Policy {};
  uint64_t Flags {};
  int32_t Nice {};
  uint32_t Priority {};
  uint64_t Runtime {}, Deadline {}, Period {};
  uint32_t UtilMin {}, UtilMax {};
};
static_assert(sizeof(Attr) == AttrSizeVer1);

// sched_copy_attr on the bytes the caller passed: Size (already read, 0 means VER0) bytes at Bytes. E2BIG when the
// size is outside [VER0, one page] or nonzero bytes follow our structure; the runtime then writes sizeof(Attr) back.
inline int64_t DecodeAttr(const uint8_t* Bytes, uint32_t Size, Attr& Out) {
  if (!Size) Size = AttrSizeVer0;
  if (Size < AttrSizeVer0 || Size > AttrSizeMax) return Err2big;
  Out = {};
  std::memcpy(&Out, Bytes, std::min<size_t>(Size, sizeof(Out)));
  for (uint32_t Index = sizeof(Out); Index < Size; ++Index) {
    if (Bytes[Index]) return Err2big;
  }
  Out.Size = Size;
  if ((Out.Flags & (FlagUtilMin | FlagUtilMax)) && Size < AttrSizeVer1) return ErrInval;
  Out.Nice = std::clamp<int32_t>(Out.Nice, -20, 19);
  return 0;
}

// sched_getattr: the report for State and the process's nice, Usize being the caller's buffer size.
inline Attr Report(const State& S, int32_t Nice, uint32_t Usize) {
  Attr A;
  A.Size = std::min<uint32_t>(Usize, sizeof(Attr));
  A.Policy = static_cast<uint32_t>(S.Policy);
  A.Flags = (S.ResetOnFork ? FlagResetOnFork : 0) | (S.Policy == Deadline ? S.DlFlags : 0);
  if (S.Policy == Deadline) {
    A.Runtime = S.Runtime;
    A.Deadline = S.Deadline;
    A.Period = S.Period;
  } else if (IsRt(S.Policy)) {
    A.Priority = S.Priority;
  } else {
    A.Nice = Nice;
    A.Runtime = S.Slice;
  }
  A.UtilMin = S.UtilMinSet ? S.UtilMin : IsRt(S.Policy) ? UtilScale : 0;
  A.UtilMax = S.UtilMaxSet ? S.UtilMax : UtilScale;
  return A;
}

// ---- CPU affinity (mask of at most 64 CPUs) ----
inline uint64_t AllCpus(uint32_t Count) { return Count >= 64 ? ~uint64_t {0} : (uint64_t {1} << Count) - 1; }
inline size_t MaskBytes(uint32_t Count) { return (Count + 63) / 64 * 8; }

// sched_getaffinity: the number of bytes to copy, or EINVAL for a buffer that is too small or not a multiple of
// sizeof(long).
inline int64_t AffinityBytes(uint64_t Len, uint32_t Count) {
  if (Len < (Count + 7) / 8 || (Len & 7)) return ErrInval;
  return static_cast<int64_t>(std::min<uint64_t>(Len, MaskBytes(Count)));
}
// get_user_cpu_mask: the bytes the call reads, a shorter buffer being zero extended.
inline size_t AffinityReadBytes(uint64_t Len, uint32_t Count) { return static_cast<size_t>(std::min<uint64_t>(Len, MaskBytes(Count))); }
// sched_setaffinity: the new mask is the request limited to the CPUs that exist; none left is EINVAL.
inline int64_t AffinityApply(uint64_t& Mask, uint64_t Requested, uint32_t Count) {
  const auto New = Requested & AllCpus(Count);
  if (!New) return ErrInval;
  Mask = New;
  return 0;
}
} // namespace Switch2Guest::Sched

namespace Switch2Guest::Ioprio {
constexpr uint32_t ClassNone = 0, ClassRt = 1, ClassBe = 2, ClassIdle = 3;
constexpr int32_t WhoProcess = 1, WhoPgrp = 2, WhoUser = 3;
constexpr uint32_t ClassShift = 13, DataMask = 0x1fff;

// ioprio_check_cap. Returns 0, EPERM or EINVAL; the real-time class needs CAP_SYS_NICE or CAP_SYS_ADMIN.
inline int64_t Check(uint32_t Value, bool CapNiceOrAdmin) {
  const auto Class = (Value >> ClassShift) & 7;
  const auto Level = Value & 7;
  switch (Class) {
  case ClassRt:
    if (!CapNiceOrAdmin) return ErrPerm;
    return 0;
  case ClassBe:
  case ClassIdle: return 0;
  case ClassNone: return Level ? ErrInval : 0;
  default: return ErrInval;
  }
}
// The value a thread stores: the class bits and the 13 data bits.
inline uint16_t Stored(uint32_t Value) { return static_cast<uint16_t>(((Value >> ClassShift) & 7) << ClassShift | (Value & DataMask)); }
// A thread without an explicit class counts as best-effort at the level derived from its nice value when the
// best of a group is taken.
inline uint16_t Effective(uint16_t Value, int32_t Nice) {
  if ((Value >> ClassShift) != ClassNone) return Value;
  return static_cast<uint16_t>(ClassBe << ClassShift | std::clamp((Nice + 20) / 5, 0, 7));
}
} // namespace Switch2Guest::Ioprio

namespace Switch2Guest::Kcmp {
enum Type : uint32_t { File, Vm, Files, Fs, Sighand, Io, Sysvsem, EpollTfd, TypeCount };
// kcmp_ptr: 0 equal, 1 first below second, 2 first above second. Linux obfuscates the pointers; any consistent
// total order serves.
inline int64_t Order(uint64_t First, uint64_t Second) { return First == Second ? 0 : First < Second ? 1 : 2; }
} // namespace Switch2Guest::Kcmp

namespace Switch2Guest::Admin {
// iopl: level 0 changes nothing; 1 to 3 raise the I/O privilege level, which needs CAP_SYS_RAWIO, absent here.
inline int64_t Iopl(uint32_t Level) { return Level > 3 ? ErrInval : Level == 0 ? 0 : ErrPerm; }
// ioperm: a bad range is EINVAL; clearing permission bits is allowed, setting them needs CAP_SYS_RAWIO.
inline int64_t Ioperm(uint64_t From, uint64_t Num, uint64_t TurnOn) {
  if (From + Num <= From || From + Num > 65536) return ErrInval;
  return TurnOn ? ErrPerm : 0;
}
} // namespace Switch2Guest::Admin
