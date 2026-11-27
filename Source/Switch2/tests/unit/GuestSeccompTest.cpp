// SPDX-License-Identifier: MIT
// Host tests of GuestSeccomp.h. Expectations were checked against native Linux 7.3 (small C programs as uid 1000
// with no_new_privs; the sequences are listed in ContainerInventory.md). The two compiled filters were produced by
// libseccomp 2.6 (seccomp_export_bpf) and their outcomes confirmed by installing them natively.
#include "GuestSeccomp.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                                \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      std::exit(1);                                            \
    }                                                          \
  } while (0)

using namespace Switch2Guest::Seccomp;

static const uint8_t AllowList[] = {32,0,0,0,4,0,0,0,21,0,0,24,62,0,0,192,32,0,0,0,0,0,0,0,53,0,0,1,0,0,0,64,21,0,0,21,255,255,255,255,21,0,19,0,0,0,0,0,21,0,18,0,1,0,0,0,21,0,17,0,3,0,0,0,21,0,16,0,9,0,0,0,21,0,15,0,12,0,0,0,21,0,14,0,15,0,0,0,21,0,13,0,39,0,0,0,21,0,0,1,101,0,0,0,6,0,0,0,0,0,3,0,21,0,10,0,231,0,0,0,21,0,0,1,179,1,0,0,6,0,0,0,38,0,5,0,21,0,0,6,1,1,0,0,32,0,0,0,36,0,0,0,84,0,0,0,0,0,0,0,21,0,0,3,0,0,0,0,32,0,0,0,32,0,0,0,84,0,0,0,3,0,0,0,21,0,1,0,0,0,0,0,6,0,0,0,1,0,5,0,6,0,0,0,0,0,255,127,6,0,0,0,0,0,0,0};
static const uint8_t DenyList[] = {32,0,0,0,4,0,0,0,21,0,0,12,62,0,0,192,32,0,0,0,0,0,0,0,53,0,0,1,0,0,0,64,21,0,0,9,255,255,255,255,21,0,0,1,169,0,0,0,6,0,0,0,0,0,0,128,21,0,0,4,41,0,0,0,32,0,0,0,20,0,0,0,21,0,0,2,0,0,0,0,32,0,0,0,16,0,0,0,21,0,1,0,16,0,0,0,6,0,0,0,0,0,255,127,6,0,0,0,13,0,5,0,6,0,0,0,0,0,0,0};

static std::vector<Insn> Load(const uint8_t* Bytes, size_t Size) {
  std::vector<Insn> Out(Size / sizeof(Insn));
  std::memcpy(Out.data(), Bytes, Size);
  return Out;
}
#define LOAD(a) Load(a, sizeof(a))

static Insn Stmt(uint16_t Code, uint32_t K) { return {Code, 0, 0, K}; }
static Insn Jump(uint16_t Code, uint32_t K, uint8_t Jt, uint8_t Jf) { return {Code, Jt, Jf, K}; }
static Insn Ret(uint32_t V) { return Stmt(Op::RetK, V); }

static Data Call(int32_t Nr, uint64_t A0 = 0, uint64_t A1 = 0, uint64_t A2 = 0) {
  Data D {};
  D.Nr = Nr;
  D.Arch = AuditArchX8664;
  D.InstructionPointer = 0x401000;
  D.Args[0] = A0;
  D.Args[1] = A1;
  D.Args[2] = A2;
  return D;
}

constexpr int Read = 0, Write = 1, Close = 3, Mmap = 9, Brk = 12, SigReturn = 15, Ptrace = 101, Getpid = 39, Getppid = 110,
              Reboot = 169, ExitGroup = 231, Openat = 257, Clone3 = 435, Socket = 41;

static bool Valid(std::vector<Insn> P) { return Validate(P); }

int main() {
  // ---- validator (native: seccomp(SET_MODE_FILTER) returns EINVAL for each rejected program)
  {
    CHECK(!Valid({}));
    CHECK(Valid({Ret(RetAllow)}));
    CHECK(!Valid({Stmt(Op::LdImm, 1)}));                                              // no final RET
    CHECK(!Valid({Jump(Op::Jeq, 0, 1, 0), Ret(0)}));                                  // jt beyond the program
    CHECK(!Valid({Jump(Op::Jeq, 0, 0, 1), Ret(0)}));
    CHECK(Valid({Jump(Op::Jeq, 0, 1, 0), Ret(0), Ret(1)}));
    CHECK(!Valid({Stmt(Op::Ja, 1), Ret(0)}));                                         // ja beyond
    CHECK(Valid({Stmt(Op::Ja, 1), Ret(0), Ret(1)}));
    CHECK(!Valid({Stmt(Op::LdMem, 0), Stmt(Op::RetA, 0)}));                           // uninitialized scratch read
    CHECK(Valid({Stmt(Op::St, 3), Stmt(Op::LdMem, 3), Stmt(Op::RetA, 0)}));
    CHECK(!Valid({Stmt(Op::St, 16), Ret(0)}));                                        // M[16]
    CHECK(Valid({Stmt(Op::St, 15), Ret(0)}));
    CHECK(!Valid({Stmt(Op::LdWAbs, 2), Ret(0)}));                                     // unaligned
    CHECK(!Valid({Stmt(Op::LdWAbs, 64), Ret(0)}));                                    // out of seccomp_data
    CHECK(Valid({Stmt(Op::LdWAbs, 60), Ret(0)}));
    CHECK(!Valid({Stmt(0x28, 0), Ret(0)}));                                           // BPF_LD|BPF_H|BPF_ABS
    CHECK(!Valid({Stmt(0x30, 0), Ret(0)}));                                           // BPF_LD|BPF_B|BPF_ABS
    CHECK(!Valid({Stmt(0x40, 0), Ret(0)}));                                           // BPF_LD|BPF_W|BPF_IND
    CHECK(!Valid({Stmt(0xb1, 0), Ret(0)}));                                           // BPF_LDX|BPF_B|BPF_MSH
    CHECK(!Valid({Stmt(Op::AluDiv, 0), Ret(0)}));                                     // divide by constant 0
    CHECK(!Valid({Stmt(Op::AluMod, 0), Ret(0)}));
    CHECK(Valid({Stmt(Op::AluDiv | Op::SrcX, 0), Ret(0)}));                           // by X: runtime
    CHECK(!Valid({Stmt(Op::AluLsh, 32), Ret(0)}));
    CHECK(Valid({Stmt(Op::AluLsh, 31), Ret(0)}));
    CHECK(!Valid({Stmt(Op::AluRsh, 32), Ret(0)}));
    CHECK(!Valid({Stmt(0x0e, 0)}));                                                   // BPF_RET|BPF_X
    CHECK(Valid({Stmt(Op::LdWLen, 0), Stmt(Op::RetA, 0)}));
    CHECK(!Valid({Stmt(0x25, 0)}));                                                   // old BPF_JMP|BPF_JGT class with no target room
    std::vector<Insn> Big(MaxProgram, Stmt(Op::LdImm, 0));
    Big.back() = Ret(RetAllow);
    CHECK(Valid(Big));
    Big.insert(Big.begin(), Stmt(Op::LdImm, 0));
    CHECK(!Valid(Big));
    // The scratch check follows every path: the join keeps only cells written on both.
    CHECK(!Valid({Stmt(Op::LdImm, 0), Jump(Op::Jeq, 0, 1, 0), Stmt(Op::St, 0), Stmt(Op::LdMem, 0), Ret(0)}));
    CHECK(Valid({Stmt(Op::St, 0), Jump(Op::Jeq, 0, 1, 0), Stmt(Op::LdImm, 0), Stmt(Op::LdMem, 0), Ret(0)}));
  }

  // ---- interpreter
  {
    const auto Run1 = [](std::vector<Insn> P, const Data& D = Call(0)) { CHECK(Validate(P)); return Run(P, D); };
    CHECK(Run1({Stmt(Op::LdWAbs, 0), Stmt(Op::RetA, 0)}, Call(1234)) == 1234);                          // nr
    CHECK(Run1({Stmt(Op::LdWAbs, 4), Stmt(Op::RetA, 0)}) == AuditArchX8664);                             // arch
    CHECK(Run1({Stmt(Op::LdWAbs, 8), Stmt(Op::RetA, 0)}) == 0x401000);                                   // ip low
    CHECK(Run1({Stmt(Op::LdWAbs, 12), Stmt(Op::RetA, 0)}) == 0);                                         // ip high
    CHECK(Run1({Stmt(Op::LdWAbs, 16), Stmt(Op::RetA, 0)}, Call(0, 0x1111222233334444ULL)) == 0x33334444);  // arg0 low
    CHECK(Run1({Stmt(Op::LdWAbs, 20), Stmt(Op::RetA, 0)}, Call(0, 0x1111222233334444ULL)) == 0x11112222);  // arg0 high
    CHECK(Run1({Stmt(Op::LdWAbs, 24), Stmt(Op::RetA, 0)}, Call(0, 0, 7)) == 7);                            // arg1 low
    CHECK(Run1({Stmt(Op::LdWLen, 0), Stmt(Op::RetA, 0)}) == 64);
    CHECK(Run1({Stmt(Op::LdxWLen, 0), Stmt(Op::Txa, 0), Stmt(Op::RetA, 0)}) == 64);
    CHECK(Run1({Stmt(Op::LdImm, 7), Stmt(Op::AluAdd, 5), Stmt(Op::RetA, 0)}) == 12);
    CHECK(Run1({Stmt(Op::LdImm, 7), Stmt(Op::AluSub, 9), Stmt(Op::RetA, 0)}) == 0xfffffffeu);
    CHECK(Run1({Stmt(Op::LdImm, 7), Stmt(Op::AluMul, 6), Stmt(Op::RetA, 0)}) == 42);
    CHECK(Run1({Stmt(Op::LdImm, 43), Stmt(Op::AluDiv, 6), Stmt(Op::RetA, 0)}) == 7);
    CHECK(Run1({Stmt(Op::LdImm, 43), Stmt(Op::AluMod, 6), Stmt(Op::RetA, 0)}) == 1);
    CHECK(Run1({Stmt(Op::LdImm, 0xf0), Stmt(Op::AluOr, 0x0f), Stmt(Op::AluAnd, 0x3c), Stmt(Op::AluXor, 0xff), Stmt(Op::RetA, 0)}) == 0xc3);
    CHECK(Run1({Stmt(Op::LdImm, 1), Stmt(Op::AluLsh, 31), Stmt(Op::AluRsh, 30), Stmt(Op::RetA, 0)}) == 2);
    CHECK(Run1({Stmt(Op::LdImm, 5), Stmt(Op::AluNeg, 0), Stmt(Op::RetA, 0)}) == 0xfffffffbu);
    CHECK(Run1({Stmt(Op::LdImm, 1), Stmt(Op::LdxImm, 33), Stmt(Op::AluLsh | Op::SrcX, 0), Stmt(Op::RetA, 0)}) == 2); // shift counts wrap at 32
    CHECK(Run1({Stmt(Op::LdImm, 5), Stmt(Op::LdxImm, 0), Stmt(Op::AluDiv | Op::SrcX, 0), Ret(RetAllow)}) == 0);  // native: killed (return 0)
    CHECK(Run1({Stmt(Op::LdImm, 5), Stmt(Op::LdxImm, 0), Stmt(Op::AluMod | Op::SrcX, 0), Ret(RetAllow)}) == 0);
    CHECK(Run1({Stmt(Op::LdImm, 9), Stmt(Op::St, 4), Stmt(Op::LdImm, 0), Stmt(Op::LdMem, 4), Stmt(Op::Tax, 0), Stmt(Op::Stx, 5),
                Stmt(Op::LdxMem, 5), Stmt(Op::Txa, 0), Stmt(Op::RetA, 0)}) == 9);
    // jumps compare unsigned
    CHECK(Run1({Stmt(Op::LdImm, 0x80000000), Jump(Op::Jgt, 1, 0, 1), Ret(10), Ret(20)}) == 10);
    CHECK(Run1({Stmt(Op::LdImm, 5), Jump(Op::Jge, 5, 0, 1), Ret(10), Ret(20)}) == 10);
    CHECK(Run1({Stmt(Op::LdImm, 5), Jump(Op::Jgt, 5, 0, 1), Ret(10), Ret(20)}) == 20);
    CHECK(Run1({Stmt(Op::LdImm, 5), Jump(Op::Jset, 4, 0, 1), Ret(10), Ret(20)}) == 10);
    CHECK(Run1({Stmt(Op::LdImm, 5), Stmt(Op::LdxImm, 5), Jump(Op::Jeq | Op::SrcX, 0, 0, 1), Ret(10), Ret(20)}) == 10);
    CHECK(Run1({Stmt(Op::Ja, 1), Ret(10), Ret(20)}) == 20);
    CHECK(Run1({Ret(0x12345678)}) == 0x12345678);
  }

  // ---- action precedence: lowest signed action wins, across the chain
  {
    CHECK(ActionOrder(RetKillProcess) < ActionOrder(RetKillThread));
    CHECK(ActionOrder(RetKillThread) < ActionOrder(RetTrap));
    CHECK(ActionOrder(RetTrap) < ActionOrder(RetErrno));
    CHECK(ActionOrder(RetErrno) < ActionOrder(RetUserNotif));
    CHECK(ActionOrder(RetUserNotif) < ActionOrder(RetTrace));
    CHECK(ActionOrder(RetTrace) < ActionOrder(RetLog));
    CHECK(ActionOrder(RetLog) < ActionOrder(RetAllow));
    State S;
    CHECK(Evaluate(S.Filters, Call(0)).Ret == RetAllow && !Evaluate(S.Filters, Call(0)).Match); // no filter
    const uint32_t Order[] {RetAllow, RetLog, RetTrace, RetUserNotif, RetErrno | 5, RetTrap | 9, RetKillThread, RetKillProcess};
    // Install the actions from the weakest to the strongest: each addition wins over every earlier one.
    for (size_t I = 0; I < 8; ++I) {
      CHECK(Attach(S, {Ret(Order[I])}, false) == 0);
      CHECK(Evaluate(S.Filters, Call(0)).Ret == Order[I]);
      if (I) CHECK(Evaluate(S.Filters, Call(0)).Match == S.Filters.get());
    }
    CHECK(FilterCount(S.Filters) == 8);
    // ... and in the other order the strongest stays.
    State T;
    for (size_t I = 8; I-- > 0;) {
      CHECK(Attach(T, {Ret(Order[I])}, false) == 0);
      CHECK(Evaluate(T.Filters, Call(0)).Ret == Order[7]);
    }
    // Equal actions: the newest filter's value (and data) is kept: two ERRNOs.
    State U;
    CHECK(Attach(U, {Ret(RetErrno | 1)}, false) == 0 && Attach(U, {Ret(RetErrno | 2)}, false) == 0);
    CHECK(Evaluate(U.Filters, Call(0)).Ret == (RetErrno | 2));
    // An unknown action (0x00010000) sorts between KILL_THREAD and TRAP: it beats ERRNO, loses to KILL_THREAD.
    State V;
    CHECK(Attach(V, {Ret(RetErrno | 1)}, false) == 0 && Attach(V, {Ret(0x00010000)}, false) == 0);
    CHECK(Evaluate(V.Filters, Call(0)).Ret == 0x00010000);
  }

  // ---- errno clamp, action list, sizes
  {
    CHECK(ErrnoResult(RetErrno | 0) == 0);
    CHECK(ErrnoResult(RetErrno | 13) == -13);
    CHECK(ErrnoResult(RetErrno | 4095) == -4095);
    CHECK(ErrnoResult(RetErrno | 4096) == -4095);
    CHECK(ErrnoResult(RetErrno | 5000) == -4095);
    CHECK(ErrnoResult(RetErrno | 0xffff) == -4095);
    for (const auto A : {RetKillProcess, RetKillThread, RetTrap, RetErrno, RetUserNotif, RetTrace, RetLog, RetAllow}) CHECK(ActionKnown(A));
    CHECK(!ActionKnown(0x00010000) && !ActionKnown(RetErrno | 5) && !ActionKnown(RetAllow | 1) && !ActionKnown(0x7fff0001));
    const NotifSizes Sizes;
    CHECK(Sizes.Notif == 80 && Sizes.NotifResp == 24 && Sizes.Data == 64);
    CHECK(StrictAllows(Read) && StrictAllows(Write) && StrictAllows(60) && StrictAllows(SigReturn));
    CHECK(!StrictAllows(ExitGroup) && !StrictAllows(Getppid) && !StrictAllows(Brk));
  }

  // ---- attach limits (native: seven 4096 instruction filters fit, the eighth is ENOMEM, then a 1 instruction filter fits)
  {
    State S;
    std::vector<Insn> Big(MaxProgram, Stmt(Op::LdImm, 0));
    Big.back() = Ret(RetAllow);
    for (int I = 0; I < 7; ++I) CHECK(Attach(S, Big, false) == 0);
    CHECK(Attach(S, Big, false) == -ErrNoMem);
    CHECK(FilterCount(S.Filters) == 7);
    CHECK(Attach(S, {Ret(RetAllow)}, false) == 0);
    CHECK(FilterCount(S.Filters) == 8); // native: Seccomp_filters 8
    CHECK(S.Mode == ModeFilter);
  }

  // ---- thread sync: the other thread's chain must be a prefix of the caller's
  {
    State Caller, Other;
    CHECK(CanSync(Other, Caller));
    CHECK(Attach(Caller, {Ret(RetAllow)}, false) == 0);
    CHECK(CanSync(Other, Caller)); // an unfiltered thread joins
    Other = Caller;
    CHECK(CanSync(Other, Caller));
    CHECK(Attach(Caller, {Ret(RetAllow)}, false) == 0);
    CHECK(CanSync(Other, Caller)); // older chain: a prefix
    State Stranger;
    CHECK(Attach(Stranger, {Ret(RetAllow)}, false) == 0);
    CHECK(!CanSync(Stranger, Caller)); // native: TSYNC returns the thread's id
    Stranger.Mode = ModeStrict;
    State Empty;
    CHECK(!CanSync(Stranger, Empty));
    Other.Mode = ModeStrict;
    CHECK(!CanSync(Other, Caller)); // a strict thread cannot sync
  }

  // ---- libseccomp allow list (default ERRNO(EPERM)); native outcomes noted
  {
    const auto P = LOAD(AllowList);
    CHECK(Validate(P));
    const auto Act = [&](Data D) { return Run(P, D); };
    CHECK(Act(Call(Read)) == RetAllow && Act(Call(Write)) == RetAllow && Act(Call(Getpid)) == RetAllow);
    CHECK(Act(Call(Getppid)) == (RetErrno | 1));                    // native: EPERM
    CHECK(Act(Call(Clone3)) == (RetErrno | 38));                    // native: ENOSYS
    CHECK(Act(Call(Ptrace)) == RetTrap);                            // native: SIGSYS
    CHECK(Act(Call(Openat, 0, 0, 0)) == RetAllow);                  // native: O_RDONLY allowed
    CHECK(Act(Call(Openat, 0, 0, 1)) == (RetErrno | 1));            // native: O_WRONLY EPERM
    CHECK(Act(Call(Openat, 0, 0, 2)) == (RetErrno | 1));            // native: O_RDWR EPERM
    Data Foreign = Call(Read);
    Foreign.Arch = 0x40000003; // AUDIT_ARCH_I386
    CHECK(Run(P, Foreign) != RetAllow);
    CHECK(Act(Call(Read | 0x40000000)) != RetAllow); // x32 numbers
  }

  // ---- libseccomp deny list (default ALLOW)
  {
    const auto P = LOAD(DenyList);
    CHECK(Validate(P));
    CHECK(Run(P, Call(Reboot)) == RetKillProcess);                  // native: SIGSYS, core
    CHECK(Run(P, Call(Socket, 16)) == (RetErrno | 13));             // native: AF_NETLINK EACCES
    CHECK(Run(P, Call(Socket, 1)) == RetAllow);                     // native: AF_UNIX works
    CHECK(Run(P, Call(Getpid)) == RetAllow);
    // Stacked with the allow list, the stricter action of the two applies.
    State S;
    CHECK(Attach(S, LOAD(AllowList), false) == 0 && Attach(S, LOAD(DenyList), false) == 0);
    CHECK(Evaluate(S.Filters, Call(Reboot)).Ret == RetKillProcess);
    CHECK(Evaluate(S.Filters, Call(Getppid)).Ret == (RetErrno | 1));
    CHECK(Evaluate(S.Filters, Call(Getpid)).Ret == RetAllow);
    CHECK(Evaluate(S.Filters, Call(Socket, 16)).Ret == (RetErrno | 13));
  }

  // ---- a Chromium style filter: arch check, then a dispatch on nr
  {
    std::vector<Insn> P {
      Stmt(Op::LdWAbs, 4), Jump(Op::Jeq, AuditArchX8664, 1, 0), Ret(RetKillProcess),
      Stmt(Op::LdWAbs, 0), Jump(Op::Jeq, Write, 4, 0), Jump(Op::Jeq, Brk, 3, 0), Jump(Op::Jeq, Mmap, 2, 0),
      Jump(Op::Jeq, Getppid, 2, 0), Ret(RetErrno | 38), Ret(RetAllow), Ret(RetTrap | 0x1234),
    };
    CHECK(Validate(P));
    CHECK(Run(P, Call(Write)) == RetAllow && Run(P, Call(Brk)) == RetAllow && Run(P, Call(Mmap)) == RetAllow);
    CHECK(Run(P, Call(Getppid)) == (RetTrap | 0x1234));
    CHECK(Run(P, Call(Read)) == (RetErrno | 38));
    Data Foreign = Call(Write);
    Foreign.Arch = 0x40000003;
    CHECK(Run(P, Foreign) == RetKillProcess);
  }

  std::puts("guest seccomp: ok");
  return 0;
}
