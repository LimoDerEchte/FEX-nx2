// SPDX-License-Identifier: MIT
// Host tests of GuestSched.h. Every expectation was checked against native Linux 7.3 (uid 1000 with RLIMIT_RTPRIO
// 0 and 50, and `unshare -r`); the cases are named after the calls that produced them.
#include "GuestSched.h"
#include "GuestCredentials.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(x)                                                     \
  do {                                                               \
    if (!(x)) {                                                      \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x);        \
      std::exit(1);                                                  \
    }                                                                \
  } while (0)

using namespace Switch2Guest;
using namespace Switch2Guest::Sched;

static Actor User(uint64_t Rtprio = 0, bool Owner = true) { return {false, Owner, Rtprio, 0}; }
static Actor Root() { return {true, true, 0, 0}; }
static Request Scheduler(int64_t Policy, uint32_t Priority, int32_t Nice) {
  Request R;
  R.Policy = Policy & ~int64_t {PolicyResetOnForkBit};
  R.Flags = Policy & PolicyResetOnForkBit ? FlagResetOnFork : 0;
  R.Priority = Priority;
  R.Nice = Nice;
  return R;
}
static Request Attribute(uint32_t Policy, uint32_t Flags, int32_t Nice, uint32_t Priority) {
  Request R;
  R.Policy = Flags & FlagKeepPolicy ? int64_t {-1} : int64_t {Policy};
  R.Flags = Flags;
  R.Nice = Nice;
  R.Priority = Priority;
  R.FromAttr = true;
  return R;
}

int main() {
  // sched_get_priority_max/min.
  CHECK(PriorityMax(Fifo) == 99 && PriorityMin(Fifo) == 1 && PriorityMax(Rr) == 99 && PriorityMin(Rr) == 1);
  for (int Policy : {Other, Batch, Idle, Deadline}) CHECK(PriorityMax(Policy) == 0 && PriorityMin(Policy) == 0);
  for (int Policy : {-1, 4, 7, 8, 1000}) CHECK(PriorityMax(Policy) == ErrInval && PriorityMin(Policy) == ErrInval);

  // sched_setscheduler/sched_setparam validation (native: ss(0,1,0) EINVAL, ss(0,0,1) EINVAL, ss(0,1,100) EINVAL).
  {
    State S;
    int32_t Nice = 0;
    CHECK(Apply(S, Nice, Scheduler(Other, 0, 0), User()) == 0);
    CHECK(Apply(S, Nice, Scheduler(Other, 1, 0), User()) == ErrInval);
    CHECK(Apply(S, Nice, Scheduler(Fifo, 0, 0), Root()) == ErrInval);
    CHECK(Apply(S, Nice, Scheduler(Fifo, 100, 0), Root()) == ErrInval);
    CHECK(Apply(S, Nice, Scheduler(4, 0, 0), User()) == ErrInval && Apply(S, Nice, Scheduler(7, 0, 0), User()) == ErrInval);
    CHECK(Apply(S, Nice, Scheduler(Deadline, 0, 0), Root()) == ErrInval); // zeroed deadline parameters
    CHECK(S.Policy == Other);
  }
  // Real-time policies need CAP_SYS_NICE or RLIMIT_RTPRIO (native: ss(0,1,1) EPERM at limit 0; ok at limit 50).
  {
    State S;
    int32_t Nice = 0;
    CHECK(Apply(S, Nice, Scheduler(Fifo, 1, 0), User(0)) == ErrPerm && S.Policy == Other);
    CHECK(Apply(S, Nice, Scheduler(Fifo, 10, 0), User(50)) == 0 && S.Policy == Fifo && S.Priority == 10);
    CHECK(Apply(S, Nice, Scheduler(Fifo, 51, 0), User(50)) == ErrPerm && S.Priority == 10);
    CHECK(Apply(S, Nice, Scheduler(-1, 20, 0), User(50)) == 0 && S.Priority == 20);       // sched_setparam raises within the limit
    CHECK(Apply(S, Nice, Scheduler(-1, 0, 0), User(50)) == ErrInval);                      // FIFO needs a priority
    CHECK(Apply(S, Nice, Scheduler(Rr, 20, 0), User(50)) == 0 && S.Policy == Rr);
    CHECK(Apply(S, Nice, Scheduler(Other, 0, 0), User(0)) == 0 && S.Policy == Other && S.Priority == 0);
    CHECK(Apply(S, Nice, Scheduler(Fifo, 99, 0), Root()) == 0 && S.Priority == 99);
    // Lowering below the old priority is allowed with the limit; raising past both is not.
    CHECK(Apply(S, Nice, Scheduler(Fifo, 60, 0), User(50)) == 0 && S.Priority == 60); // lowering from 99
    CHECK(Apply(S, Nice, Scheduler(Fifo, 70, 0), User(50)) == ErrPerm);
    // Another user's thread: EPERM without CAP_SYS_NICE.
    State T;
    CHECK(Apply(T, Nice, Scheduler(Other, 0, 0), User(0, false)) == ErrPerm);
    CHECK(Apply(T, Nice, Scheduler(Other, 0, 0), Root()) == 0);
  }
  // Nice rules (native: nice 5 then 3 EPERM, 5 then 8 ok; RLIMIT_NICE 0).
  {
    State S;
    int32_t Nice = 5;
    CHECK(Apply(S, Nice, Attribute(Other, 0, 3, 0), User()) == ErrPerm && Nice == 5);
    CHECK(Apply(S, Nice, Attribute(Other, 0, 5, 0), User()) == 0 && Nice == 5);
    CHECK(Apply(S, Nice, Attribute(Other, 0, 8, 0), User()) == 0 && Nice == 8);
    CHECK(Apply(S, Nice, Attribute(Batch, 0, 3, 0), User()) == ErrPerm && S.Policy == Other);
    Actor Limited = User();
    Limited.NiceLimit = 20 - (-5); // RLIMIT_NICE 25 allows nice down to -5
    CHECK(Apply(S, Nice, Attribute(Other, 0, -5, 0), Limited) == 0 && Nice == -5);
    CHECK(Apply(S, Nice, Attribute(Other, 0, -6, 0), Limited) == ErrPerm);
    CHECK(Apply(S, Nice, Attribute(Other, 0, -20, 0), Root()) == 0 && Nice == -20);
    CHECK(Apply(S, Nice, Attribute(Other, 0, 19, 0), User()) == 0 && Nice == 19);
  }
  // SCHED_IDLE keeps the nice value and leaving it needs CAP_SYS_NICE at nice 0 (native: idle then other EPERM).
  {
    State S;
    int32_t Nice = 5;
    CHECK(Apply(S, Nice, Attribute(Idle, 0, 0, 0), User()) == 0 && S.Policy == Idle && Nice == 5);
    CHECK(Apply(S, Nice, Attribute(Other, 0, 5, 0), User()) == ErrPerm && S.Policy == Idle);
    CHECK(Apply(S, Nice, Attribute(Other, 0, 5, 0), Root()) == 0 && S.Policy == Other);
  }
  // SCHED_RESET_ON_FORK: set freely, cleared only with CAP_SYS_NICE (native: reset then clear EPERM).
  {
    State S;
    int32_t Nice = 0;
    CHECK(Apply(S, Nice, Scheduler(Batch | PolicyResetOnForkBit, 0, 0), User()) == 0 && S.ResetOnFork && S.Policy == Batch);
    CHECK(Apply(S, Nice, Scheduler(Other, 0, 0), User()) == ErrPerm && S.ResetOnFork);
    CHECK(Apply(S, Nice, Scheduler(-1, 0, 0), User()) == 0 && S.ResetOnFork);           // setparam keeps the flag
    CHECK(Apply(S, Nice, Scheduler(Other, 0, 0), Root()) == 0 && !S.ResetOnFork);
    CHECK(Apply(S, Nice, Scheduler(Batch | 0x20000000, 0, 0), Root()) == ErrInval);       // unknown flag bit left in the policy
    CHECK(Apply(S, Nice, Scheduler(0x100, 0, 0), Root()) == ErrInval);
  }
  // sched_setattr flags.
  {
    State S;
    int32_t Nice = 0;
    CHECK(Apply(S, Nice, Attribute(Other, 0x80, 0, 0), User()) == ErrInval);
    CHECK(Apply(S, Nice, Attribute(Other, 0x10000000, 0, 0), User()) == ErrInval);
    CHECK(Apply(S, Nice, Attribute(Other, FlagReclaim, 0, 0), User()) == 0);             // accepted on a non-deadline policy
    CHECK(Apply(S, Nice, Attribute(Other, FlagDlOverrun, 0, 0), User()) == 0 && S.DlFlags == 0);
    // KEEP_POLICY ignores the policy field and uses the old policy for the priority rule.
    CHECK(Apply(S, Nice, Attribute(99, FlagKeepPolicy, 2, 0), User()) == 0 && Nice == 2 && S.Policy == Other);
    CHECK(Apply(S, Nice, Attribute(Fifo, FlagKeepPolicy, 3, 5), User()) == ErrInval);
    // KEEP_PARAMS validates and changes nothing but the flags.
    CHECK(Apply(S, Nice, Attribute(Rr, FlagKeepParams, 9, 5), User(50)) == 0 && S.Policy == Other && Nice == 2);
    CHECK(Apply(S, Nice, Attribute(Rr, FlagKeepParams, 9, 0), User(50)) == ErrInval);
    CHECK(Apply(S, Nice, Attribute(99, FlagKeepParams, 9, 0), User()) == ErrInval);
    CHECK(Apply(S, Nice, Attribute(99, FlagKeepParams | FlagKeepPolicy, 9, 0), User()) == 0);
    CHECK(Apply(S, Nice, Attribute(Other, FlagKeepParams | FlagResetOnFork, 9, 0), User()) == 0 && S.ResetOnFork && Nice == 2);
    CHECK(Apply(S, Nice, Attribute(Other, FlagKeepPolicy | FlagResetOnFork, 2, 0), User()) == 0 && S.ResetOnFork); // keep-policy keeps the stored flag
  }
  // Fair slice: clamped, 0 restores the default, setparam keeps it (native).
  {
    State S;
    int32_t Nice = 0;
    auto R = Attribute(Other, 0, 0, 0);
    R.Runtime = 500'000;
    CHECK(Apply(S, Nice, R, User()) == 0 && S.Slice == 500'000);
    CHECK(Apply(S, Nice, Scheduler(-1, 0, 0), User()) == 0 && S.Slice == 500'000);
    R.Runtime = 50'000;
    CHECK(Apply(S, Nice, R, User()) == 0 && S.Slice == MinSlice);
    R.Runtime = 200'000'000;
    CHECK(Apply(S, Nice, R, User()) == 0 && S.Slice == MaxSlice);
    R.Runtime = 0;
    CHECK(Apply(S, Nice, R, User()) == 0 && S.Slice == DefaultSlice);
    R = Attribute(Idle, 0, 0, 0);
    R.Runtime = 500'000;
    CHECK(Apply(S, Nice, R, User()) == 0 && S.Slice == DefaultSlice); // not a fair-class policy
  }
  // Utilization clamps.
  {
    State S;
    int32_t Nice = 0;
    auto R = Attribute(Other, FlagUtilMin | FlagUtilMax, 0, 0);
    R.UtilMin = 50;
    R.UtilMax = 40;
    CHECK(Apply(S, Nice, R, User()) == ErrInval);
    R.UtilMin = 0;
    R.UtilMax = 1025;
    CHECK(Apply(S, Nice, R, User()) == ErrInval);
    R.UtilMin = 10;
    R.UtilMax = 100;
    CHECK(Apply(S, Nice, R, User()) == 0 && S.UtilMin == 10 && S.UtilMax == 100);
    R = Attribute(Other, FlagUtilMin, 0, 0);
    R.UtilMin = 20;
    R.UtilMax = 5; // ignored: its flag is not set
    CHECK(Apply(S, Nice, R, User()) == 0 && S.UtilMin == 20 && S.UtilMax == 100);
    R.UtilMin = 1025;
    CHECK(Apply(S, Nice, R, User()) == ErrInval);
  }
  // SCHED_DEADLINE (native: EPERM unprivileged for valid parameters, EINVAL for bad ones).
  {
    State S;
    int32_t Nice = 3;
    auto R = Attribute(Deadline, 0, 0, 0);
    R.Runtime = 1'000'000;
    R.Deadline = 2'000'000;
    R.Period = 3'000'000;
    CHECK(Apply(S, Nice, R, User()) == ErrPerm);
    CHECK(Apply(S, Nice, R, Root()) == 0 && S.Policy == Deadline && S.Runtime == 1'000'000 && S.Period == 3'000'000 && Nice == 3);
    R.Period = 0;
    CHECK(Apply(S, Nice, R, Root()) == 0 && S.Period == 2'000'000);
    R.Runtime = 100;
    CHECK(Apply(S, Nice, R, Root()) == ErrInval);
    R.Runtime = 3'000'000;
    CHECK(Apply(S, Nice, R, Root()) == ErrInval); // runtime above the deadline
    R.Runtime = 1'000'000;
    R.Period = 1'000'000;
    CHECK(Apply(S, Nice, R, Root()) == ErrInval); // period below the deadline
    R.Period = 3'000'000;
    R.Priority = 1;
    CHECK(Apply(S, Nice, R, Root()) == ErrInval);
    R.Priority = 0;
    R.Flags = FlagReclaim;
    CHECK(Apply(S, Nice, R, Root()) == 0 && S.DlFlags == FlagReclaim);
    const auto A = Report(S, Nice, 56);
    CHECK(A.Policy == 6 && A.Runtime == 1'000'000 && A.Deadline == 2'000'000 && A.Period == 3'000'000 && A.Flags == FlagReclaim && A.Nice == 0);
    // sched_fork: EAGAIN unless reset-on-fork.
    State Child = S;
    int32_t ChildNice = Nice;
    CHECK(ForkInherit(Child, ChildNice) == ErrAgain);
    Child.ResetOnFork = true;
    CHECK(ForkInherit(Child, ChildNice) == 0 && Child.Policy == Other && ChildNice == 0 && !Child.ResetOnFork);
    // Leaving deadline clears its parameters.
    CHECK(Apply(S, Nice, Scheduler(Other, 0, 0), Root()) == 0 && S.Runtime == 0 && S.DlFlags == 0);
  }
  // Fork inheritance (native: child of FIFO inherits; with reset it is OTHER nice 0 and the flag clears).
  {
    State S;
    int32_t Nice = -2;
    CHECK(Apply(S, Nice, Scheduler(Fifo, 10, 0), Root()) == 0);
    State Child = S;
    int32_t ChildNice = Nice;
    CHECK(ForkInherit(Child, ChildNice) == 0 && Child.Policy == Fifo && Child.Priority == 10);
    S.ResetOnFork = true;
    Child = S;
    CHECK(ForkInherit(Child, ChildNice) == 0 && Child.Policy == Other && Child.Priority == 0 && ChildNice == 0 && !Child.ResetOnFork && S.ResetOnFork);
    State Fair;
    int32_t Neg = -3;
    Fair.ResetOnFork = true;
    Fair.Policy = Batch;
    CHECK(ForkInherit(Fair, Neg) == 0 && Fair.Policy == Batch && Neg == 0);
    int32_t Pos = 4;
    Fair.ResetOnFork = true;
    CHECK(ForkInherit(Fair, Pos) == 0 && Pos == 4);
  }
  // sched_rr_get_interval (native: RR 100 ms, FIFO 0, fair a whole jiffy count).
  {
    State S;
    CHECK(RrInterval(S) == 2'000'000);
    S.Policy = Rr;
    CHECK(RrInterval(S) == 100'000'000);
    S.Policy = Fifo;
    CHECK(RrInterval(S) == 0);
    S.Policy = Other;
    S.Slice = 500'000;
    CHECK(RrInterval(S) == 0);
  }
  // sched_attr size handshake (native: size 0 ok, 47 and 4097 E2BIG, 100 with zero tail ok, nonzero tail E2BIG).
  {
    uint8_t Raw[4200] {};
    Attr A;
    Attr Ours {};
    Ours.Policy = 3;
    Ours.Nice = 100;
    for (uint32_t Size : {0u, 48u, 52u, 56u, 100u, 4096u}) {
      std::memset(Raw, 0, sizeof(Raw));
      std::memcpy(Raw, &Ours, sizeof(Ours));
      CHECK(DecodeAttr(Raw, Size, A) == 0 && A.Policy == 3 && A.Nice == 19);
    }
    CHECK(DecodeAttr(Raw, 47, A) == Err2big && DecodeAttr(Raw, 4097, A) == Err2big);
    Raw[90] = 1;
    CHECK(DecodeAttr(Raw, 100, A) == Err2big && DecodeAttr(Raw, 56, A) == 0);
    Raw[90] = 0;
    Ours.Nice = -100;
    std::memcpy(Raw, &Ours, sizeof(Ours));
    CHECK(DecodeAttr(Raw, 56, A) == 0 && A.Nice == -20);
    // Utilization flags need the VER1 size.
    Ours.Flags = FlagUtilMin;
    std::memcpy(Raw, &Ours, sizeof(Ours));
    CHECK(DecodeAttr(Raw, 48, A) == ErrInval && DecodeAttr(Raw, 56, A) == 0);
  }
  // sched_getattr report (native: RT shows nice 0 and clamp 1024..1024, fair its slice, size min(usize, 56)).
  {
    State S;
    auto A = Report(S, -4, 100);
    CHECK(A.Size == 56 && A.Policy == 0 && A.Nice == -4 && A.Priority == 0 && A.Runtime == DefaultSlice && A.UtilMin == 0 && A.UtilMax == 1024);
    CHECK(Report(S, 0, 52).Size == 52 && Report(S, 0, 48).Size == 48);
    S.Policy = Fifo;
    S.Priority = 10;
    A = Report(S, -4, 56);
    CHECK(A.Nice == 0 && A.Priority == 10 && A.Runtime == 0 && A.UtilMin == 1024 && A.UtilMax == 1024);
    S.ResetOnFork = true;
    CHECK(Report(S, 0, 56).Flags == FlagResetOnFork);
  }
  // Affinity (native: len < 8 EINVAL, len 8..any returns 8 bytes, empty set EINVAL, a wider mask is clamped).
  {
    CHECK(AffinityBytes(0, 6) == ErrInval && AffinityBytes(7, 6) == ErrInval && AffinityBytes(9, 6) == ErrInval);
    CHECK(AffinityBytes(8, 6) == 8 && AffinityBytes(128, 6) == 8);
    CHECK(AffinityBytes(1, 6) == ErrInval); // not a multiple of sizeof(long)
    CHECK(AffinityReadBytes(0, 6) == 0 && AffinityReadBytes(3, 6) == 3 && AffinityReadBytes(1 << 20, 6) == 8);
    uint64_t Mask = AllCpus(6);
    CHECK(Mask == 0x3f);
    CHECK(AffinityApply(Mask, 0, 6) == ErrInval && Mask == 0x3f);
    CHECK(AffinityApply(Mask, uint64_t {1} << 63, 6) == ErrInval);
    CHECK(AffinityApply(Mask, 1, 6) == 0 && Mask == 1);
    CHECK(AffinityApply(Mask, ~uint64_t {0}, 6) == 0 && Mask == 0x3f);
    CHECK(AllCpus(64) == ~uint64_t {0} && MaskBytes(6) == 8 && MaskBytes(64) == 8);
  }

  // ioprio_check_cap (native: RT EPERM without a capability, NONE with a level EINVAL, classes 4 to 7 EINVAL).
  {
    using namespace Ioprio;
    const auto Value = [](uint32_t Class, uint32_t Data) { return (Class << ClassShift) | Data; };
    CHECK(Check(Value(ClassNone, 0), false) == 0 && Check(Value(ClassNone, 1), false) == ErrInval);
    CHECK(Check(Value(ClassNone, 8), false) == 0); // a hint without a level
    CHECK(Check(Value(ClassBe, 7), false) == 0 && Check(Value(ClassBe, 8), false) == 0);
    CHECK(Check(Value(ClassIdle, 0), false) == 0 && Check(Value(ClassIdle, 7), false) == 0);
    CHECK(Check(Value(ClassRt, 3), false) == ErrPerm && Check(Value(ClassRt, 3), true) == 0);
    for (uint32_t Class : {4u, 5u, 6u, 7u}) CHECK(Check(Value(Class, 0), true) == ErrInval);
    CHECK(Check(0xffffffffu, true) == ErrInval);
    CHECK(Stored(0x10000 | Value(ClassBe, 3)) == Value(ClassBe, 3)); // bits above the class are dropped
    CHECK(Stored(Value(ClassBe, 3) | (5 << 3)) == (Value(ClassBe, 3) | (5 << 3)));
    CHECK(Effective(0, -4) == Value(ClassBe, 3) && Effective(0, 0) == Value(ClassBe, 4) && Effective(0, 19) == Value(ClassBe, 7));
    CHECK(Effective(Value(ClassIdle, 2), 0) == Value(ClassIdle, 2));
    CHECK(Effective(0, -20) == Value(ClassBe, 0));
  }

  // kcmp ordering and the machine-administration calls.
  {
    using namespace Kcmp;
    CHECK(Order(5, 5) == 0 && Order(1, 2) == 1 && Order(2, 1) == 2 && Order(0, 7) == 1);
    CHECK(TypeCount == 8 && EpollTfd == 7);
    CHECK(Admin::Iopl(0) == 0 && Admin::Iopl(1) == ErrPerm && Admin::Iopl(3) == ErrPerm && Admin::Iopl(4) == ErrInval);
    CHECK(Admin::Ioperm(0, 0, 0) == ErrInval && Admin::Ioperm(0, 1, 1) == ErrPerm && Admin::Ioperm(0, 1, 0) == 0);
    CHECK(Admin::Ioperm(65535, 1, 0) == 0 && Admin::Ioperm(65535, 2, 0) == ErrInval && Admin::Ioperm(~uint64_t {0}, 2, 0) == ErrInval);
  }
  // Machine-wide capabilities are outside every set.
  {
    const auto Root = RootCredentials();
    for (uint32_t Cap : {16u, 17u, 20u, 22u, 26u, 34u, 38u, 39u}) {
      CHECK(!Root.Capable(Cap) && !(Root.CapPermitted >> Cap & 1) && !(Root.CapBounding >> Cap & 1) && (MachineWideCaps >> Cap & 1));
    }
    CHECK(Root.Capable(CapSysNice) && Root.Capable(21) && Root.Capable(18) && Root.Capable(12)); // SYS_ADMIN, SYS_CHROOT, NET_ADMIN stay
    CHECK(Credentials {}.CapBounding == CapInitialRoot);
  }
  std::printf("GuestSched: OK\n");
  return 0;
}
