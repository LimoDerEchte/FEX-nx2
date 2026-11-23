// SPDX-License-Identifier: MIT
#include "../GuestSignals.h"
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

static SigInfo Queued(uint32_t Signal, uint64_t Value) {
  auto Info = MakeSigInfo(Signal, SiQueue, 7, 1000);
  Info.Kill.Value = Value;
  return Info;
}

int main() {
  // Default actions, Linux signal(7).
  for (uint32_t S : {SigQuit, SigIll, SigTrap, SigAbrt, SigBus, SigFpe, SigSegv, SigXcpu, SigXfsz, SigSys})
    CHECK(DefaultAction(S) == SignalDefault::Core);
  for (uint32_t S : {SigChld, SigUrg, SigWinch}) CHECK(DefaultAction(S) == SignalDefault::Ignore);
  for (uint32_t S : {SigStop, SigTstp, SigTtin, SigTtou}) CHECK(DefaultAction(S) == SignalDefault::Stop);
  CHECK(DefaultAction(SigCont) == SignalDefault::Continue);
  for (uint32_t S : {SigHup, SigInt, SigKill, SigUsr1, SigUsr2, SigPipe, SigAlrm, SigTerm, SigStkflt, SigVtalrm, SigProf,
                     SigIo, SigPwr})
    CHECK(DefaultAction(S) == SignalDefault::Terminate);
  for (uint32_t S = SigRtMin; S <= SigMax; ++S) CHECK(DefaultAction(S) == SignalDefault::Terminate);

  // siginfo layout.
  auto Info = MakeSigInfo(SigUsr1, SiUser, 101, 1000);
  CHECK(reinterpret_cast<const int32_t*>(&Info)[0] == SigUsr1);
  CHECK(reinterpret_cast<const int32_t*>(&Info)[2] == SiUser);
  CHECK(reinterpret_cast<const uint32_t*>(&Info)[4] == 101);
  CHECK(reinterpret_cast<const uint32_t*>(&Info)[5] == 1000);

  // Standard signals collapse.
  SignalQueue Queue;
  CHECK(Queue.Enqueue(MakeSigInfo(SigUsr1, SiUser), 8));
  CHECK(Queue.Enqueue(MakeSigInfo(SigUsr1, SiUser), 8));
  CHECK(Queue.QueuedCount() == 1);
  CHECK(Queue.Set() == SignalBit(SigUsr1));

  // Realtime signals queue with their values, in order.
  CHECK(Queue.Enqueue(Queued(SigRtMin + 1, 7), 8));
  CHECK(Queue.Enqueue(Queued(SigRtMin + 1, 8), 8));
  CHECK(Queue.Enqueue(Queued(SigRtMin, 5), 8));
  CHECK(Queue.Enqueue(MakeSigInfo(SigInt, SiUser), 8));
  CHECK(Queue.QueuedCount() == 5);

  // Lowest number first: SIGINT, SIGUSR1, SIGRTMIN, then SIGRTMIN+1 FIFO.
  CHECK(Queue.Dequeue(~uint64_t {})->Signo == static_cast<int32_t>(SigInt));
  CHECK(Queue.Dequeue(~uint64_t {})->Signo == static_cast<int32_t>(SigUsr1));
  CHECK(Queue.Dequeue(~uint64_t {})->Kill.Value == 5);
  CHECK(Queue.Dequeue(~uint64_t {})->Kill.Value == 7);
  CHECK(Queue.Dequeue(~uint64_t {})->Kill.Value == 8);
  CHECK(!Queue.Dequeue(~uint64_t {}));

  // Dequeue honours the allowed set.
  CHECK(Queue.Enqueue(MakeSigInfo(SigUsr1, SiUser), 8));
  CHECK(Queue.Enqueue(MakeSigInfo(SigUsr2, SiUser), 8));
  CHECK(Queue.Dequeue(SignalBit(SigUsr2))->Signo == static_cast<int32_t>(SigUsr2));
  CHECK(!Queue.Dequeue(SignalBit(SigUsr2)));

  // Discard drops stop signals (SIGCONT generation).
  CHECK(Queue.Enqueue(MakeSigInfo(SigTstp, SiKernel), 8));
  CHECK(Queue.Enqueue(MakeSigInfo(SigTtin, SiKernel), 8));
  Queue.Discard(StopSet);
  CHECK(Queue.Set() == SignalBit(SigUsr1));

  // The realtime queue limit returns false; standard signals still queue.
  SignalQueue Limited;
  CHECK(Limited.Enqueue(Queued(SigRtMin, 1), 2));
  CHECK(Limited.Enqueue(Queued(SigRtMin, 2), 2));
  CHECK(!Limited.Enqueue(Queued(SigRtMin, 3), 2));
  CHECK(Limited.QueuedCount() == 2);

  CHECK((Unblockable & SignalBit(SigKill)) && (Unblockable & SignalBit(SigStop)));
  // signalfd_siginfo follows the siginfo layout of each signal.
  {
    auto Kill = MakeSigInfo(SigTerm, SiUser, 42, 1000);
    auto Out = ToSignalFdInfo(Kill);
    CHECK(Out.Signo == SigTerm && Out.Code == SiUser && Out.Pid == 42 && Out.Uid == 1000 && Out.Ptr == 0);
    SigInfo Timer;
    Timer.Signo = SigAlrm;
    Timer.Code = SiTimer;
    Timer.Timer.Tid = 3;
    Timer.Timer.Overrun = 5;
    Timer.Timer.Value = 0x1234;
    Out = ToSignalFdInfo(Timer);
    CHECK(Out.Tid == 3 && Out.Overrun == 5 && Out.Ptr == 0x1234 && Out.Int == 0x1234 && Out.Pid == 0);
    SigInfo Child;
    Child.Signo = SigChld;
    Child.Code = CldExited;
    Child.Child.Pid = 101;
    Child.Child.Uid = 1000;
    Child.Child.Status = 7;
    Child.Child.Utime = 2;
    Out = ToSignalFdInfo(Child);
    CHECK(Out.Pid == 101 && Out.Uid == 1000 && Out.Status == 7 && Out.Utime == 2 && Out.Code == CldExited);
    auto Queue = Queued(SigRtMin, 9);
    Queue.Code = SiQueue;
    Out = ToSignalFdInfo(Queue);
    CHECK(Out.Ptr == 9 && Out.Int == 9);
  }
  std::puts("guest signals: ok");
}
