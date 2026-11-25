// SPDX-License-Identifier: MIT
// Linux seccomp for the Switch 2 guest runtime: the classic BPF validator (Linux bpf_check_classic with
// seccomp_check_filter), the interpreter over struct seccomp_data, the filter chain and the action precedence.
// The runtime glue (dispatch hook, prctl, status, signals) is in GuestRuntime.cpp; ContainerInventory.md lists
// the behavior and the deliberate differences. Host test: make check-guest-seccomp.
#pragma once
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace Switch2Guest::Seccomp {
// prctl(PR_GET_SECCOMP) results and the mode of a thread.
constexpr uint32_t ModeDisabled = 0, ModeStrict = 1, ModeFilter = 2;

// seccomp(2) operations and flags.
constexpr uint32_t OpSetModeStrict = 0, OpSetModeFilter = 1, OpGetActionAvail = 2, OpGetNotifSizes = 3;
constexpr uint32_t FlagTsync = 1, FlagLog = 2, FlagSpecAllow = 4, FlagNewListener = 8, FlagTsyncEsrch = 16, FlagWaitKillableRecv = 32;
constexpr uint32_t FlagMask = FlagTsync | FlagLog | FlagSpecAllow | FlagNewListener | FlagTsyncEsrch | FlagWaitKillableRecv;

// Return values of a filter. The action is compared as a signed 32 bit value: KILL_PROCESS sorts lowest.
constexpr uint32_t RetKillProcess = 0x80000000, RetKillThread = 0, RetTrap = 0x00030000, RetErrno = 0x00050000,
                   RetUserNotif = 0x7fc00000, RetTrace = 0x7ff00000, RetLog = 0x7ffc0000, RetAllow = 0x7fff0000,
                   RetActionFull = 0xffff0000, RetData = 0x0000ffff;
constexpr uint32_t MaxErrno = 4095;

constexpr size_t MaxProgram = 4096;     // BPF_MAXINSNS
constexpr size_t MaxPathInsns = 32768;  // MAX_INSNS_PER_PATH
constexpr size_t PerFilterCost = 4;     // seccomp_attach_filter charges every earlier filter 4 more

constexpr uint32_t AuditArchX8664 = 0xc000003e;

struct Insn {
  uint16_t Code;
  uint8_t Jt, Jf;
  uint32_t K;
};
static_assert(sizeof(Insn) == 8);

// struct seccomp_data.
struct Data {
  int32_t Nr;
  uint32_t Arch;
  uint64_t InstructionPointer;
  uint64_t Args[6];
};
static_assert(sizeof(Data) == 64);

constexpr int ErrInval = 22, ErrNoMem = 12;

namespace Op {
constexpr uint16_t LdW = 0x00, LdImm = 0x00, LdWAbs = 0x20, LdMem = 0x60, LdWLen = 0x80, LdxImm = 0x01, LdxMem = 0x61, LdxWLen = 0x81,
                   St = 0x02, Stx = 0x03, Ja = 0x05, Jeq = 0x15, Jgt = 0x25, Jge = 0x35, Jset = 0x45, RetK = 0x06, RetA = 0x16,
                   Tax = 0x07, Txa = 0x87;
constexpr uint16_t AluAdd = 0x04, AluSub = 0x14, AluMul = 0x24, AluDiv = 0x34, AluOr = 0x44, AluAnd = 0x54, AluLsh = 0x64,
                   AluRsh = 0x74, AluNeg = 0x84, AluMod = 0x94, AluXor = 0xa4;
constexpr uint16_t SrcX = 0x08;
} // namespace Op

constexpr size_t MemWords = 16;

inline bool IsConditional(uint16_t Code) {
  const auto Base = Code & ~Op::SrcX;
  return Base == Op::Jeq || Base == Op::Jgt || Base == Op::Jge || Base == Op::Jset;
}

// Linux bpf_check_classic plus seccomp_check_filter: true when the program is acceptable (EINVAL otherwise).
// Only these instructions load from seccomp_data (32 bit absolute, 4 byte aligned and in range); the length
// loads become constants; scratch memory is checked for uninitialized reads along every path.
inline bool Validate(std::span<const Insn> Program) {
  const size_t Count = Program.size();
  if (Count == 0 || Count > MaxProgram) return false;
  for (size_t Pc = 0; Pc < Count; ++Pc) {
    const auto& I = Program[Pc];
    switch (I.Code) {
    case Op::LdWAbs:
      if (I.K >= sizeof(Data) || (I.K & 3)) return false;
      break;
    case Op::LdImm: case Op::LdWLen: case Op::LdxImm: case Op::LdxWLen: case Op::RetK: case Op::RetA: case Op::Tax: case Op::Txa:
    case Op::AluNeg:
      break;
    case Op::LdMem: case Op::LdxMem: case Op::St: case Op::Stx:
      if (I.K >= MemWords) return false;
      break;
    case Op::AluDiv: case Op::AluMod:
      if (I.K == 0) return false;
      break;
    case Op::AluLsh: case Op::AluRsh:
      if (I.K >= 32) return false;
      break;
    case Op::AluAdd: case Op::AluSub: case Op::AluMul: case Op::AluOr: case Op::AluAnd: case Op::AluXor:
    case Op::AluAdd | Op::SrcX: case Op::AluSub | Op::SrcX: case Op::AluMul | Op::SrcX: case Op::AluDiv | Op::SrcX:
    case Op::AluOr | Op::SrcX: case Op::AluAnd | Op::SrcX: case Op::AluLsh | Op::SrcX: case Op::AluRsh | Op::SrcX:
    case Op::AluMod | Op::SrcX: case Op::AluXor | Op::SrcX:
      break;
    case Op::Ja:
      if (I.K >= Count - Pc - 1) return false;
      break;
    case Op::Jeq: case Op::Jgt: case Op::Jge: case Op::Jset:
    case Op::Jeq | Op::SrcX: case Op::Jgt | Op::SrcX: case Op::Jge | Op::SrcX: case Op::Jset | Op::SrcX:
      if (I.Jt >= Count - Pc - 1 || I.Jf >= Count - Pc - 1) return false;
      break;
    default: return false;
    }
  }
  if ((Program.back().Code & 7) != 6) return false; // BPF_RET
  // check_load_and_stores: a scratch cell must be written on every path before it is read.
  std::vector<uint16_t> Masks(Count, 0xffff);
  uint16_t Valid = 0;
  for (size_t Pc = 0; Pc < Count; ++Pc) {
    const auto& I = Program[Pc];
    Valid &= Masks[Pc];
    switch (I.Code) {
    case Op::St: case Op::Stx: Valid |= static_cast<uint16_t>(1u << I.K); break;
    case Op::LdMem: case Op::LdxMem:
      if (!(Valid & (1u << I.K))) return false;
      break;
    case Op::Ja:
      Masks[Pc + 1 + I.K] &= Valid;
      Valid = 0xffff;
      break;
    default:
      if (IsConditional(I.Code)) {
        Masks[Pc + 1 + I.Jt] &= Valid;
        Masks[Pc + 1 + I.Jf] &= Valid;
        Valid = 0xffff;
      }
      break;
    }
  }
  return true;
}

// Runs a validated program (Linux's classic BPF as converted for seccomp). A division or modulo by a zero X
// ends the program with return value 0 (KILL_THREAD).
inline uint32_t Run(std::span<const Insn> Program, const Data& Input) {
  uint8_t Bytes[sizeof(Data)];
  std::memcpy(Bytes, &Input, sizeof(Data));
  uint32_t A = 0, X = 0, M[MemWords] {};
  for (size_t Pc = 0; Pc < Program.size(); ++Pc) {
    const auto& I = Program[Pc];
    const uint32_t Src = (I.Code & Op::SrcX) ? X : I.K;
    switch (I.Code) {
    case Op::LdWAbs: std::memcpy(&A, Bytes + I.K, 4); break;
    case Op::LdImm: A = I.K; break;
    case Op::LdWLen: A = sizeof(Data); break;
    case Op::LdxImm: X = I.K; break;
    case Op::LdxWLen: X = sizeof(Data); break;
    case Op::LdMem: A = M[I.K]; break;
    case Op::LdxMem: X = M[I.K]; break;
    case Op::St: M[I.K] = A; break;
    case Op::Stx: M[I.K] = X; break;
    case Op::Tax: X = A; break;
    case Op::Txa: A = X; break;
    case Op::RetK: return I.K;
    case Op::RetA: return A;
    case Op::AluNeg: A = 0u - A; break;
    case Op::Ja: Pc += I.K; break;
    default: {
      const auto Base = static_cast<uint16_t>(I.Code & ~Op::SrcX);
      if (IsConditional(I.Code)) {
        const bool Taken = Base == Op::Jeq ? A == Src : Base == Op::Jgt ? A > Src : Base == Op::Jge ? A >= Src : (A & Src) != 0;
        Pc += Taken ? I.Jt : I.Jf;
        break;
      }
      switch (Base) {
      case Op::AluAdd: A += Src; break;
      case Op::AluSub: A -= Src; break;
      case Op::AluMul: A *= Src; break;
      case Op::AluDiv:
        if (!Src) return 0;
        A /= Src;
        break;
      case Op::AluMod:
        if (!Src) return 0;
        A %= Src;
        break;
      case Op::AluOr: A |= Src; break;
      case Op::AluAnd: A &= Src; break;
      case Op::AluXor: A ^= Src; break;
      case Op::AluLsh: A <<= (Src & 31); break;
      case Op::AluRsh: A >>= (Src & 31); break;
      default: return 0;
      }
      break;
    }
    }
  }
  return 0;
}

// One attached filter. Chains are immutable and shared between threads and children.
struct Filter {
  std::vector<Insn> Program;
  std::shared_ptr<const Filter> Prev;
  bool Log {}; // SECCOMP_FILTER_FLAG_LOG
  size_t Depth {1};
};
using Chain = std::shared_ptr<const Filter>;

// The seccomp state of one thread.
struct State {
  uint32_t Mode {ModeDisabled};
  Chain Filters;
};

inline size_t FilterCount(const Chain& C) {
  return C ? C->Depth : 0;
}

// seccomp_attach_filter: ENOMEM beyond MAX_INSNS_PER_PATH over the whole chain.
inline int Attach(State& S, std::vector<Insn> Program, bool Log) {
  size_t Total = Program.size();
  for (const Filter* Walk = S.Filters.get(); Walk; Walk = Walk->Prev.get()) Total += Walk->Program.size() + PerFilterCost;
  if (Total > MaxPathInsns) return -ErrNoMem;
  auto F = std::make_shared<Filter>();
  F->Program = std::move(Program);
  F->Prev = S.Filters;
  F->Log = Log;
  F->Depth = FilterCount(S.Filters) + 1;
  S.Filters = std::move(F);
  S.Mode = ModeFilter;
  return 0;
}

// is_ancestor: the chain Parent is a prefix of Child's chain (or null).
inline bool IsAncestor(const Chain& Parent, const Chain& Child) {
  if (!Parent) return true;
  for (const Filter* Walk = Child.get(); Walk; Walk = Walk->Prev.get()) {
    if (Walk == Parent.get()) return true;
  }
  return false;
}

// seccomp_can_sync_threads for one other thread: its mode is none or filter and its chain is a prefix of the caller's.
inline bool CanSync(const State& Other, const State& Caller) {
  return (Other.Mode == ModeDisabled || Other.Mode == ModeFilter) && IsAncestor(Other.Filters, Caller.Filters);
}

// Action precedence: the signed 32 bit value of the action bits, lowest wins.
constexpr int32_t ActionOrder(uint32_t Ret) {
  return static_cast<int32_t>(Ret & RetActionFull);
}

struct Decision {
  uint32_t Ret {RetAllow};
  const Filter* Match {}; // the filter whose result won (null: no filter)
};

// seccomp_run_filters: every filter of the chain, newest first, the lowest action wins (the first on a tie).
inline Decision Evaluate(const Chain& C, const Data& Input) {
  Decision Result;
  for (const Filter* Walk = C.get(); Walk; Walk = Walk->Prev.get()) {
    const uint32_t Ret = Run(Walk->Program, Input);
    if (ActionOrder(Ret) < ActionOrder(Result.Ret)) {
      Result.Ret = Ret;
      Result.Match = Walk;
    }
  }
  return Result;
}

// The syscalls of strict mode on x86-64: read, write, exit, rt_sigreturn.
constexpr bool StrictAllows(uint64_t Nr) {
  return Nr == 0 || Nr == 1 || Nr == 60 || Nr == 15;
}

// SECCOMP_GET_ACTION_AVAIL: the exact action values (the data bits must be zero). The runtime reports
// USER_NOTIF as unavailable: it has no notification fds (see ContainerInventory.md).
constexpr bool ActionKnown(uint32_t Action) {
  return Action == RetKillProcess || Action == RetKillThread || Action == RetTrap || Action == RetErrno ||
         Action == RetUserNotif || Action == RetTrace || Action == RetLog || Action == RetAllow;
}

// struct seccomp_notif_sizes of Linux 7.3 (seccomp_notif 80, seccomp_notif_resp 24, seccomp_data 64).
struct NotifSizes {
  uint16_t Notif {80}, NotifResp {24}, Data {64};
};

// The kernel's return value for an ERRNO action: the data bits, clamped to MAX_ERRNO, negated.
constexpr int64_t ErrnoResult(uint32_t Ret) {
  const uint32_t Value = Ret & RetData;
  return -static_cast<int64_t>(Value > MaxErrno ? MaxErrno : Value);
}
} // namespace Switch2Guest::Seccomp
