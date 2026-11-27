// SPDX-License-Identifier: MIT
// Linux signal semantics for the Switch 2 guest runtime: signal numbers,
// default actions, the x86-64 siginfo layout and pending-signal queues.
#pragma once
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace Switch2Guest {
enum class SignalDefault : uint8_t { Terminate, Core, Ignore, Stop, Continue };

constexpr uint32_t SigHup = 1, SigInt = 2, SigQuit = 3, SigIll = 4, SigTrap = 5, SigAbrt = 6, SigBus = 7, SigFpe = 8,
                   SigKill = 9, SigUsr1 = 10, SigSegv = 11, SigUsr2 = 12, SigPipe = 13, SigAlrm = 14, SigTerm = 15,
                   SigStkflt = 16, SigChld = 17, SigCont = 18, SigStop = 19, SigTstp = 20, SigTtin = 21, SigTtou = 22,
                   SigUrg = 23, SigXcpu = 24, SigXfsz = 25, SigVtalrm = 26, SigProf = 27, SigWinch = 28, SigIo = 29,
                   SigPwr = 30, SigSys = 31, SigRtMin = 32, SigMax = 64;

constexpr uint64_t SignalBit(uint32_t Signal) {
  return uint64_t {1} << (Signal - 1);
}
constexpr uint64_t Unblockable = SignalBit(SigKill) | SignalBit(SigStop);
constexpr uint64_t StopSet = SignalBit(SigStop) | SignalBit(SigTstp) | SignalBit(SigTtin) | SignalBit(SigTtou);
constexpr bool IsRealtime(uint32_t Signal) {
  return Signal >= SigRtMin;
}

// Linux sig_kernel_coredump/ignore/stop and SIGCONT; every other signal,
// including all realtime signals, terminates.
constexpr SignalDefault DefaultAction(uint32_t Signal) {
  switch (Signal) {
  case SigQuit:
  case SigIll:
  case SigTrap:
  case SigAbrt:
  case SigBus:
  case SigFpe:
  case SigSegv:
  case SigXcpu:
  case SigXfsz:
  case SigSys: return SignalDefault::Core;
  case SigChld:
  case SigUrg:
  case SigWinch: return SignalDefault::Ignore;
  case SigStop:
  case SigTstp:
  case SigTtin:
  case SigTtou: return SignalDefault::Stop;
  case SigCont: return SignalDefault::Continue;
  default: return SignalDefault::Terminate;
  }
}

// x86-64 siginfo_t, 128 bytes. The union starts at offset 16.
struct SigInfo {
  int32_t Signo {}, Errno {}, Code {}, Pad0 {};
  union {
    struct {
      uint32_t Pid, Uid;
      uint64_t Value;
    } Kill; // SI_USER, SI_QUEUE, SI_TKILL, SI_KERNEL
    struct {
      int32_t Tid, Overrun;
      uint64_t Value;
    } Timer; // SI_TIMER
    struct {
      uint32_t Pid, Uid;
      int32_t Status, Pad;
      int64_t Utime, Stime;
    } Child; // CLD_*
    struct {
      uint64_t Address;
    } Fault;
    uint8_t Raw[112];
  };
  SigInfo() {
    std::memset(Raw, 0, sizeof(Raw));
  }
};
static_assert(sizeof(SigInfo) == 128);
static_assert(offsetof(SigInfo, Kill) == 16);

constexpr int32_t SiUser = 0, SiKernel = 0x80, SiQueue = -1, SiTimer = -2, SiTkill = -6;
constexpr int32_t CldExited = 1, CldKilled = 2, CldDumped = 3, CldTrapped = 4, CldStopped = 5, CldContinued = 6;

inline SigInfo MakeSigInfo(uint32_t Signal, int32_t Code, uint32_t Pid = 0, uint32_t Uid = 0) {
  SigInfo Info;
  Info.Signo = static_cast<int32_t>(Signal);
  Info.Code = Code;
  Info.Kill.Pid = Pid;
  Info.Kill.Uid = Uid;
  return Info;
}

// struct signalfd_siginfo, 128 bytes.
struct SignalFdInfo {
  uint32_t Signo;
  int32_t Errno, Code;
  uint32_t Pid, Uid;
  int32_t Fd;
  uint32_t Tid, Band, Overrun, Trapno;
  int32_t Status, Int;
  uint64_t Ptr, Utime, Stime, Addr;
  uint16_t AddrLsb, Pad2;
  int32_t Syscall;
  uint64_t CallAddr;
  uint32_t Arch;
  uint8_t Pad[28];
};
static_assert(sizeof(SignalFdInfo) == 128);

// Linux signalfd_copyinfo with siginfo_layout: the fields that the signal's
// layout defines, everything else zero.
inline SignalFdInfo ToSignalFdInfo(const SigInfo& Info) {
  SignalFdInfo Out {};
  Out.Signo = static_cast<uint32_t>(Info.Signo);
  Out.Errno = Info.Errno;
  Out.Code = Info.Code;
  const auto Signal = static_cast<uint32_t>(Info.Signo);
  if (Info.Code > SiUser && Info.Code < SiKernel) {
    if (Signal == SigChld) {
      Out.Pid = Info.Child.Pid;
      Out.Uid = Info.Child.Uid;
      Out.Status = Info.Child.Status;
      Out.Utime = static_cast<uint64_t>(Info.Child.Utime);
      Out.Stime = static_cast<uint64_t>(Info.Child.Stime);
    } else if (Signal == SigSegv || Signal == SigBus || Signal == SigIll || Signal == SigFpe || Signal == SigTrap) {
      Out.Addr = Info.Fault.Address;
    } else {
      Out.Pid = Info.Kill.Pid;
      Out.Uid = Info.Kill.Uid;
    }
  } else if (Info.Code == SiTimer) {
    Out.Tid = static_cast<uint32_t>(Info.Timer.Tid);
    Out.Overrun = static_cast<uint32_t>(Info.Timer.Overrun);
    Out.Ptr = Info.Timer.Value;
    Out.Int = static_cast<int32_t>(Info.Timer.Value);
  } else {
    Out.Pid = Info.Kill.Pid;
    Out.Uid = Info.Kill.Uid;
    if (Info.Code < 0) { // SIL_RT
      Out.Ptr = Info.Kill.Value;
      Out.Int = static_cast<int32_t>(Info.Kill.Value);
    }
  }
  return Out;
}

// A pending set. A standard signal has at most one pending instance (later
// ones are dropped, as in Linux legacy_queue); realtime signals queue in
// order up to Limit entries (RLIMIT_SIGPENDING).
class SignalQueue {
public:
  uint64_t Set() const {
    uint64_t Bits {};
    for (const auto& Entry : Entries) Bits |= SignalBit(static_cast<uint32_t>(Entry.Signo));
    return Bits;
  }

  // Returns false when a realtime signal could not be queued (EAGAIN for
  // sigqueue). A duplicate standard signal is accepted and dropped.
  bool Enqueue(const SigInfo& Info, size_t Limit) {
    const auto Signal = static_cast<uint32_t>(Info.Signo);
    if (!IsRealtime(Signal)) {
      if (Set() & SignalBit(Signal)) return true;
    } else if (Entries.size() >= Limit) {
      return false;
    }
    Entries.push_back(Info);
    return true;
  }

  // Removes the lowest-numbered pending signal in Allowed, oldest first.
  std::optional<SigInfo> Dequeue(uint64_t Allowed) {
    const auto Pending = Set() & Allowed;
    if (!Pending) return std::nullopt;
    const auto Signal = static_cast<int32_t>(std::countr_zero(Pending) + 1);
    const auto It = std::find_if(Entries.begin(), Entries.end(), [&](const SigInfo& Entry) { return Entry.Signo == Signal; });
    auto Info = *It;
    Entries.erase(It);
    return Info;
  }

  // Drops every pending instance of the signals in Mask.
  void Discard(uint64_t Mask) {
    std::erase_if(Entries, [&](const SigInfo& Entry) { return Mask & SignalBit(static_cast<uint32_t>(Entry.Signo)); });
  }

  size_t QueuedCount() const {
    return Entries.size();
  }

  // The first pending entry matching Match, or null.
  template<typename Fn>
  SigInfo* Find(Fn Match) {
    const auto It = std::find_if(Entries.begin(), Entries.end(), Match);
    return It == Entries.end() ? nullptr : &*It;
  }

  // Drops every pending entry matching Match.
  template<typename Fn>
  void DiscardIf(Fn Match) {
    std::erase_if(Entries, Match);
  }

private:
  std::vector<SigInfo> Entries;
};
} // namespace Switch2Guest
