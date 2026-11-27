// SPDX-License-Identifier: MIT
// EXPERIMENT (multithreading milestone, Task 0): can guest threads run on their own host threads on the console?
// docs/MultithreadingPlan.md lists the checks. Runs every check in order, writes sdmc:/fex/mtprobe.log (flushed per line)
// and shows the same lines on screen. Each check logs "check <name>: start" first, so a crash names the check it died in
// (the console delivers no fault to user code: a crash ends the app). The risky stack-size check runs last.
#include "MtBlobs.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/LogManager.h>

#include <switch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <csetjmp>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <vector>

namespace {
constexpr const char* LogPath = "sdmc:/fex/mtprobe.log";
constexpr const char* Version = "mtprobe 3";
// Run 3 repeats only what runs 1 and 2 left open; `sdmc:/fex/mtprobe.full` (a sidecar file, any content) runs every check.
constexpr const char* FullSidecar = "sdmc:/fex/mtprobe.full";
constexpr int GuestPriority = 0x2c;
constexpr size_t ExecStack = 4 << 20;  // the launcher's guest worker stack (runtime/GuestRuntime.cpp CompilerStackSize)

FILE* Log {};
Mutex SayLock {};

void Say(const char* Format, ...) __attribute__((format(printf, 1, 2)));
void Say(const char* Format, ...) {
  char Line[512];
  va_list Args;
  va_start(Args, Format);
  std::vsnprintf(Line, sizeof(Line), Format, Args);
  va_end(Args);
  mutexLock(&SayLock);
  if (Log) {
    std::fputs(Line, Log);
    std::fputc('\n', Log);
    std::fflush(Log);
  }
  std::printf("%s\n", Line);
  mutexUnlock(&SayLock);
}

uint64_t NowNs() {
  return armTicksToNs(armGetSystemTick());
}
double Ms(uint64_t Ns) {
  return double(Ns) / 1e6;
}

// ---- Host threads ----------------------------------------------------------------------------------------------------

struct HostThread {
  Thread Handle {};
  bool Created {};
};

template<typename Fn>
struct Thunk {
  Fn Body;
  static void Entry(void* Arg) {
    static_cast<Thunk*>(Arg)->Body();
  }
};

// Runs Body on a new host thread (cpuid -2: the default core). The caller keeps the returned objects alive until Join.
template<typename Fn>
struct Spawned {
  Thunk<Fn> Work;
  HostThread T;
};
// Mask != 0: svcSetThreadCoreMask(Ideal, Mask) before the thread starts (Ideal -1: no ideal core).
template<typename Fn>
std::unique_ptr<Spawned<Fn>> Spawn(Fn Body, size_t Stack, int Core, Result* Status = nullptr, int Ideal = 0, uint32_t Mask = 0) {
  auto S = std::make_unique<Spawned<Fn>>(Spawned<Fn> {Thunk<Fn> {std::move(Body)}, {}});
  Result R = threadCreate(&S->T.Handle, &Thunk<Fn>::Entry, &S->Work, nullptr, Stack, GuestPriority, Core);
  if (R_SUCCEEDED(R) && Mask) {
    const Result M = svcSetThreadCoreMask(S->T.Handle.handle, Ideal, Mask);
    if (R_FAILED(M)) Say("  svcSetThreadCoreMask(ideal %d, mask %#x) failed: %#x", Ideal, Mask, M);
  }
  if (R_SUCCEEDED(R)) R = threadStart(&S->T.Handle);
  S->T.Created = R_SUCCEEDED(R);
  if (Status) *Status = R;
  if (!S->T.Created) return nullptr;
  return S;
}
template<typename Fn>
void Join(std::unique_ptr<Spawned<Fn>>& S) {
  if (!S) return;
  threadWaitForExit(&S->T.Handle);
  threadClose(&S->T.Handle);
  S.reset();
}

// ---- FEXCore ---------------------------------------------------------------------------------------------------------

std::atomic_ref<uint64_t> PollWord(FEXCore::Core::InternalThreadState* Thread, size_t Offset) {
  return std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(Thread->InterruptFaultPage + Offset));
}

uint8_t* CodeBase {};
constexpr size_t CodeSize = 0x10000;

// One guest thread: its FEX state, call/return stack, guest stack and the jump back to the host at exit().
struct GuestRun {
  FEXCore::Context::Context* Ctx {};
  FEXCore::Core::InternalThreadState* Core {};
  std::unique_ptr<uint8_t, decltype(&std::free)> ReturnStack {nullptr, &std::free};
  std::unique_ptr<uint8_t, decltype(&std::free)> Stack {nullptr, &std::free};
  uint64_t ReturnStackTop {};
  std::jmp_buf Jump {};
  uint64_t Result {};
  bool Exited {};       // exit() reached
  std::atomic<uint64_t> EndNs {};  // when ExecuteThread was left (read by the main thread)
  uint64_t StopRip {};  // after a poll stop: the guest rip it stopped at
};

std::array<std::atomic<GuestRun*>, 64> Runs {};

GuestRun* FindRun(FEXCore::Core::InternalThreadState* Thread) {
  for (auto& Slot : Runs)
    if (auto* R = Slot.load(std::memory_order_acquire); R && R->Core == Thread) return R;
  return nullptr;
}

struct ProbeHandler final : FEXCore::HLE::SyscallHandler {
  void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override {
    auto* R = FindRun(Frame->Thread);
    if (!R) {
      Say("FAIL syscall from an unknown thread, rax=%" PRIu64, Frame->State.gregs[FEXCore::X86State::REG_RAX]);
      return;
    }
    R->Result = Frame->State.gregs[FEXCore::X86State::REG_RDI];
    R->Exited = true;
    std::longjmp(R->Jump, 1);
  }
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Address) override {
    const auto Base = reinterpret_cast<uint64_t>(CodeBase);
    if (Address >= Base && Address < Base + CodeSize) return {Base, CodeSize, true};
    return {};
  }
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
};

ProbeHandler Handler;
FEXCore::SignalDelegator Delegator;

struct TsoMode {
  const char* Name;
  bool Tso, Vector, Memcpy;
};
constexpr TsoMode TsoOff {"tso-off", false, false, false};
constexpr TsoMode TsoOn {"tso-on", true, false, false};
constexpr TsoMode TsoVector {"tso-on+vector", true, true, false};
constexpr TsoMode TsoMemcpy {"tso-on+memcpy", true, false, true};

// The host CPU features FEXCore may use. The runtime passes none today; the A78C has LSE atomics (Armv8.1) and LDAPR (RCPC,
// Armv8.3); LRCPC2 (TSOImm9: LDAPUR/STLUR, Armv8.4) is uncertain. Feature ID registers cannot be read safely here, so each
// set is tried in order of risk: an instruction the CPU lacks ends the app, and the log names the set.
struct FeatureSet {
  const char* Name;
  bool Atomics, Rcpc, TsoImm9;
};
constexpr FeatureSet NoFeatures {"no features (today)", false, false, false};
FeatureSet Features = NoFeatures;

fextl::unique_ptr<FEXCore::Context::Context> MakeContext(const TsoMode& Mode) {
  using namespace FEXCore;
  Config::Set(Config::CONFIG_IS64BIT_MODE, "1");
  Config::Set(Config::CONFIG_DISKCACHE, "0");
  Config::Set(Config::CONFIG_MULTIBLOCK, "1");
  Config::Set(Config::CONFIG_MAXINST, "64");
  Config::Set(Config::CONFIG_DISABLE_VIXL_INDIRECT_RUNTIME_CALLS, "1");
  Config::Set(Config::CONFIG_TSOENABLED, Mode.Tso ? "1" : "0");
  Config::Set(Config::CONFIG_VECTORTSOENABLED, Mode.Vector ? "1" : "0");
  Config::Set(Config::CONFIG_MEMCPYSETTSOENABLED, Mode.Memcpy ? "1" : "0");
  HostFeatures Host {};
  Host.ProcessFeatures.Is64BitMode = true;
  Host.DCacheLineLog2 = 4;
  Host.SupportsAtomics = Features.Atomics;
  Host.SupportsRCPC = Features.Rcpc;
  Host.SupportsTSOImm9 = Features.TsoImm9;
  auto Ctx = Context::Context::CreateNewContext(Host);
  Ctx->SetSignalDelegator(&Delegator);
  Ctx->SetSyscallHandler(&Handler);
  Ctx->SetPollPendingInterrupt(true);
  if (!Ctx->InitCore()) {
    Say("FAIL InitCore (%s)", Mode.Name);
    return nullptr;
  }
  return Ctx;
}

std::array<FEXCore::Core::CPUState::gdt_segment, 32> Gdt {};

std::unique_ptr<GuestRun> NewGuest(FEXCore::Context::Context& Ctx) {
  using CPUState = FEXCore::Core::CPUState;
  auto R = std::make_unique<GuestRun>();
  R->Ctx = &Ctx;
  constexpr auto Size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
  R->ReturnStack.reset(static_cast<uint8_t*>(std::aligned_alloc(0x1000, Size)));
  R->Stack.reset(static_cast<uint8_t*>(std::aligned_alloc(0x1000, 0x10000)));
  if (!R->ReturnStack || !R->Stack) return nullptr;
  std::memset(R->ReturnStack.get(), 0, Size);
  R->ReturnStackTop = reinterpret_cast<uint64_t>(R->ReturnStack.get()) + Size - 16;
  CPUState State {};
  State.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_GDT] = Gdt.data();
  State.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_LDT] = Gdt.data();
  State.cs_idx = CPUState::DEFAULT_USER_CS << 3;
  auto* Code = CPUState::GetSegmentFromIndex(State, State.cs_idx);
  CPUState::SetGDTLimit(Code, 0xF'FFFFU);
  Code->L = 1;
  State.callret_sp = R->ReturnStackTop;
  R->Core = Ctx.CreateThread(&State);
  if (!R->Core) return nullptr;
  R->Core->CallRetStackBase = R->ReturnStack.get();
  for (auto& Slot : Runs) {
    GuestRun* Empty = nullptr;
    if (Slot.compare_exchange_strong(Empty, R.get())) return R;
  }
  Say("FAIL more than %zu guest threads", Runs.size());
  return nullptr;
}

void FreeGuest(std::unique_ptr<GuestRun>& R) {
  if (!R) return;
  for (auto& Slot : Runs) {
    GuestRun* Mine = R.get();
    Slot.compare_exchange_strong(Mine, nullptr);
  }
  R->Ctx->DestroyThread(R->Core);
  R.reset();
}

// Runs the routine at blob offset Entry on the calling host thread until it exits or a poll stop returns.
void Execute(GuestRun& R, size_t Entry, uint64_t Rdi, uint64_t Rsi = 0, uint64_t Rdx = 0) {
  using namespace FEXCore::X86State;
  auto* Frame = R.Core->CurrentFrame;
  Frame->State.rip = reinterpret_cast<uint64_t>(CodeBase) + Entry;
  Frame->State.gregs[REG_RDI] = Rdi;
  Frame->State.gregs[REG_RSI] = Rsi;
  Frame->State.gregs[REG_RDX] = Rdx;
  Frame->State.gregs[REG_RSP] = reinterpret_cast<uint64_t>(R.Stack.get()) + 0x10000 - 64;
  Frame->State.callret_sp = R.ReturnStackTop;
  Frame->InSyscallInfo = 0;
  R.Exited = false;
  R.StopRip = 0;
  PollWord(R.Core, FEXCore::Core::InterruptPollTargetOffset).store(0, std::memory_order_relaxed);
  if (setjmp(R.Jump) == 0) {
    R.Ctx->ExecuteThread(R.Core);
    // Returned without exit(): a poll stop.
    const auto Target = PollWord(R.Core, FEXCore::Core::InterruptPollTargetOffset).load(std::memory_order_relaxed);
    R.StopRip = Target ? R.Ctx->RestoreRIPFromHostPC(R.Core, Target) : 0;
  }
  R.EndNs = NowNs();
  PollWord(R.Core, FEXCore::Core::InterruptPollRequestOffset).store(0, std::memory_order_relaxed);
}

uint64_t Lcg(uint64_t Steps, uint64_t Seed) {
  uint64_t X = Seed;
  for (uint64_t I = 0; I < Steps; ++I) X = X * 6364136223846793005ull + 1442695040888963407ull;
  return X;
}

void Percentiles(const char* What, std::vector<uint64_t>& Ns) {
  if (Ns.empty()) return;
  std::sort(Ns.begin(), Ns.end());
  Say("  %s: n=%zu min %.2f us, median %.2f us, p99 %.2f us, max %.2f us", What, Ns.size(), Ns.front() / 1e3, Ns[Ns.size() / 2] / 1e3,
      Ns[Ns.size() * 99 / 100] / 1e3, Ns.back() / 1e3);
}

// ---- Checks ----------------------------------------------------------------------------------------------------------

uint64_t CoreMask {};
std::vector<int> Cores;

void CheckInfo() {
  Say("check info: start");
  svcGetInfo(&CoreMask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  for (int C = 0; C < 64; ++C)
    if (CoreMask & (1ull << C)) Cores.push_back(C);
  uint64_t Total {}, Used {};
  svcGetInfo(&Total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&Used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  s32 Priority {};
  svcGetThreadPriority(&Priority, CUR_THREAD_HANDLE);
  Say("  core mask %#" PRIx64 " (%zu cores), main thread on core %u priority %#x, memory used %" PRIu64 " / %" PRIu64 " MiB", CoreMask,
      Cores.size(), svcGetCurrentProcessorNumber(), Priority, Used >> 20, Total >> 20);
  Say("check info: INFO");
}

void CheckThreadLimit() {
  Say("check thread_limit: start");
  Handle Limit {};
  uint64_t LimitHandle {};
  s64 Max = -1, Current = -1;
  if (R_SUCCEEDED(svcGetInfo(&LimitHandle, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0)) && LimitHandle) {
    Limit = static_cast<Handle>(LimitHandle);
    svcGetResourceLimitLimitValue(&Max, Limit, LimitableResource_Threads);
    svcGetResourceLimitCurrentValue(&Current, Limit, LimitableResource_Threads);
  }
  Say("  resource limit: threads %" PRId64 " max, %" PRId64 " in use (handle %#" PRIx64 ")", Max, Current, LimitHandle);
  UEvent Go;
  ueventCreate(&Go, false);
  struct Parked {
    Thread T;
  };
  std::vector<Parked> Made;
  Made.reserve(1024);
  Result Failure = 0;
  const uint64_t Start = NowNs();
  for (int I = 0; I < 1024; ++I) {
    Parked P {};
    Result R = threadCreate(&P.T, [](void* Arg) { waitSingle(waiterForUEvent(static_cast<UEvent*>(Arg)), -1); }, &Go, nullptr, 256 << 10,
                            GuestPriority, -2);
    if (R_SUCCEEDED(R)) {
      R = threadStart(&P.T);
      if (R_FAILED(R)) threadClose(&P.T);
    }
    if (R_FAILED(R)) {
      Failure = R;
      break;
    }
    Made.push_back(P);
  }
  const uint64_t Elapsed = NowNs() - Start;
  s64 Peak = -1;
  if (Limit) svcGetResourceLimitCurrentValue(&Peak, Limit, LimitableResource_Threads);
  ueventSignal(&Go);
  for (auto& P : Made) {
    threadWaitForExit(&P.T);
    threadClose(&P.T);
  }
  Say("  created %zu threads with 256 KiB stacks (%s %#x), %.1f us per create+start, threads in use at the peak %" PRId64,
      Made.size(), Failure ? "stopped by" : "cap reached, no failure", Failure, Made.empty() ? 0.0 : Elapsed / 1e3 / Made.size(), Peak);
  Say("check thread_limit: INFO");
}

uint64_t NativeWork(uint64_t Steps, uint64_t Seed) {
  volatile uint64_t X = Seed;
  for (uint64_t I = 0; I < Steps; ++I) X = X * 6364136223846793005ull + 1442695040888963407ull;
  return X;
}

void CheckCoreScaling(bool Full) {
  Say("check core_scaling: start");
  constexpr uint64_t Steps = 100'000'000;
  double Single = 0;
  enum class Place { Default, Pinned, MaskNoIdeal, MaskIdeal };
  struct Mode {
    Place P;
    const char* Name;
  };
  std::vector<Mode> Modes {{Place::MaskNoIdeal, "all-cores mask, no ideal core"}, {Place::MaskIdeal, "all-cores mask, ideal core i"}};
  if (Full) Modes.insert(Modes.begin(), {{Place::Default, "default core (-2)"}, {Place::Pinned, "pinned to core i"}});
  for (const auto& M : Modes) {
    for (size_t N : {size_t {1}, size_t {2}, size_t {4}, Cores.size()}) {
      std::atomic<bool> Go {false};
      std::vector<uint32_t> StartCore(N), MidCore(N), EndCore(N);
      using Body = std::function<void()>;
      std::vector<std::unique_ptr<Spawned<Body>>> Threads;
      for (size_t I = 0; I < N; ++I) {
        const int Core = M.P == Place::Pinned ? Cores[I % Cores.size()] : -2;
        const int Ideal = M.P == Place::MaskIdeal ? Cores[I % Cores.size()] : -1;
        const uint32_t Mask = M.P == Place::MaskNoIdeal || M.P == Place::MaskIdeal ? static_cast<uint32_t>(CoreMask) : 0;
        Threads.push_back(Spawn<Body>([&, I] {
          while (!Go.load()) svcSleepThread(0);
          StartCore[I] = svcGetCurrentProcessorNumber();
          NativeWork(Steps / 2, I + 1);
          MidCore[I] = svcGetCurrentProcessorNumber();
          NativeWork(Steps / 2, I + 1);
          EndCore[I] = svcGetCurrentProcessorNumber();
        }, 64 << 10, Core, nullptr, Ideal, Mask));
      }
      const uint64_t Start = NowNs();
      Go.store(true);
      for (auto& T : Threads) Join(T);
      const uint64_t Elapsed = NowNs() - Start;
      if (N == 1 && !Single) Single = Ms(Elapsed);
      char Where[160] {};
      for (size_t I = 0; I < N; ++I)
        std::snprintf(Where + std::strlen(Where), sizeof(Where) - std::strlen(Where), "%u>%u>%u ", StartCore[I], MidCore[I], EndCore[I]);
      Say("  %s, %zu thread(s), each the same work: %.1f ms (one thread alone %.1f ms = perfect scaling), start>mid>end cores %s", M.Name, N,
          Ms(Elapsed), Single, Where);
    }
  }
  Say("check core_scaling: INFO");
}

void CheckWakeLatency() {
  Say("check wake_latency: start");
  constexpr int Rounds = 20000;
  const int CoreA = Cores.size() > 1 ? Cores[1] : -2, CoreB = Cores.size() > 2 ? Cores[2] : -2;
  using Body = std::function<void()>;
  // Each method: the pong thread answers every ping; the ping thread (main side) records round trips.
  auto Run = [&](const char* Name, auto Ping, auto Pong) {
    std::vector<uint64_t> Trips;
    Trips.reserve(Rounds);
    std::atomic<bool> Done {false};
    auto Ponger = Spawn<Body>([&] {
      for (int I = 0; I < Rounds; ++I) Pong();
      Done.store(true);
    }, 64 << 10, CoreB);
    auto Pinger = Spawn<Body>([&] {
      for (int I = 0; I < Rounds; ++I) {
        const uint64_t T0 = NowNs();
        Ping();
        Trips.push_back(NowNs() - T0);
      }
    }, 64 << 10, CoreA);
    Join(Pinger);
    Join(Ponger);
    Percentiles(Name, Trips);
  };
  {
    std::atomic<int> Turn {0};
    Run("spin (atomic busy wait)", [&] {
      Turn.store(1, std::memory_order_release);
      while (Turn.load(std::memory_order_acquire) == 1) {}
    }, [&] {
      while (Turn.load(std::memory_order_acquire) == 0) {}
      Turn.store(0, std::memory_order_release);
    });
  }
  {
    Mutex M {};
    CondVar C {};
    int Turn = 0;
    Run("mutex + condvar", [&] {
      mutexLock(&M);
      Turn = 1;
      condvarWakeAll(&C);
      while (Turn == 1) condvarWait(&C, &M);
      mutexUnlock(&M);
    }, [&] {
      mutexLock(&M);
      while (Turn == 0) condvarWait(&C, &M);
      Turn = 0;
      condvarWakeAll(&C);
      mutexUnlock(&M);
    });
  }
  {
    UEvent ToPong, ToPing;
    ueventCreate(&ToPong, true);
    ueventCreate(&ToPing, true);
    Run("uevent", [&] {
      ueventSignal(&ToPong);
      waitSingle(waiterForUEvent(&ToPing), -1);
    }, [&] {
      waitSingle(waiterForUEvent(&ToPong), -1);
      ueventSignal(&ToPing);
    });
  }
  {
    alignas(64) static std::atomic<s32> Turn {0};
    Run("address arbiter (futex-like)", [&] {
      Turn.store(1);
      svcSignalToAddress(&Turn, SignalType_Signal, 0, 1);
      while (Turn.load() == 1) svcWaitForAddress(&Turn, ArbitrationType_WaitIfEqual, 1, -1);
    }, [&] {
      while (Turn.load() == 0) svcWaitForAddress(&Turn, ArbitrationType_WaitIfEqual, 0, -1);
      Turn.store(0);
      svcSignalToAddress(&Turn, SignalType_Signal, 0, 1);
    });
  }
  Say("check wake_latency: INFO");
}

bool CheckJitParallel() {
  Say("check jit_parallel: start");
  auto Ctx = MakeContext(TsoOn);
  if (!Ctx) return false;
  constexpr uint64_t Steps = 100'000'000;
  bool Ok = true;
  double Single = 0;
  for (size_t N : {size_t {1}, size_t {2}, size_t {4}, Cores.size()}) {
    if (N > Cores.size()) continue;
    std::vector<std::unique_ptr<GuestRun>> Guests;
    for (size_t I = 0; I < N; ++I) Guests.push_back(NewGuest(*Ctx));
    if (std::any_of(Guests.begin(), Guests.end(), [](auto& G) { return !G; })) {
      Say("FAIL could not create %zu guest threads", N);
      return false;
    }
    std::atomic<bool> Go {false};
    using Body = std::function<void()>;
    std::vector<std::unique_ptr<Spawned<Body>>> Threads;
    for (size_t I = 0; I < N; ++I) {
      Threads.push_back(Spawn<Body>([&, I] {
        while (!Go.load()) svcSleepThread(0);
        Execute(*Guests[I], MtBlobs::spin, Steps, I + 1);
      }, ExecStack, Cores[I % Cores.size()]));
      if (!Threads.back()) {
        Say("FAIL could not start host thread %zu", I);
        Go.store(true);
        return false;
      }
    }
    const uint64_t Start = NowNs();
    Go.store(true);
    for (auto& T : Threads) Join(T);
    const uint64_t Elapsed = NowNs() - Start;
    if (N == 1) Single = Ms(Elapsed);
    size_t Right = 0;
    for (size_t I = 0; I < N; ++I) Right += Guests[I]->Exited && Guests[I]->Result == Lcg(Steps, I + 1);
    Ok &= Right == N;
    Say("  %zu guest thread(s) in one context, pinned to separate cores: %.1f ms (one alone %.1f ms), %zu/%zu results right", N, Ms(Elapsed), Single, Right, N);
    for (auto& G : Guests) FreeGuest(G);
  }
  Say("check jit_parallel: %s", Ok ? "PASS" : "FAIL");
  return Ok;
}

void CheckPollStop() {
  Say("check poll_stop: start");
  auto Ctx = MakeContext(TsoOn);
  if (!Ctx) return;
  auto G = NewGuest(*Ctx);
  if (!G) return;
  std::vector<uint64_t> Latency;
  int Good = 0;
  constexpr int Rounds = 20;
  const int Core = Cores.size() > 1 ? Cores[1] : -2;
  for (int I = 0; I < Rounds; ++I) {
    std::atomic<bool> Started {false};
    using Body = std::function<void()>;
    auto T = Spawn<Body>([&] {
      Started.store(true);
      Execute(*G, MtBlobs::forever, 0);
    }, ExecStack, Core);
    while (!Started.load()) svcSleepThread(100'000);
    svcSleepThread(30'000'000);
    const uint64_t Asked = NowNs();
    PollWord(G->Core, FEXCore::Core::InterruptPollRequestOffset).store(1, std::memory_order_release);
    bool Returned = false;
    for (int Wait = 0; Wait < 2000 && !Returned; ++Wait) {
      Returned = G->EndNs >= Asked;
      if (!Returned) svcSleepThread(100'000);
    }
    if (!Returned) {
      Say("  round %d: no return 200 ms after the poll request: giving up (the thread is still running guest code)", I);
      Say("check poll_stop: FAIL");
      // The thread still runs guest code: keep its context and state alive (leaked) so it does not run on freed memory.
      (void)T.release();
      (void)G.release();
      (void)Ctx.release();
      return;
    }
    Join(T);
    Latency.push_back(G->EndNs - Asked);
    const auto Base = reinterpret_cast<uint64_t>(CodeBase);
    Good += !G->Exited && G->StopRip >= Base + MtBlobs::forever && G->StopRip < Base + MtBlobs::smc;
  }
  Percentiles("request to ExecuteThread return", Latency);
  Say("  %d/%d stops restored a rip inside the loop", Good, Rounds);
  FreeGuest(G);
  Say("check poll_stop: %s", Good == Rounds ? "PASS" : "FAIL");
}

bool CheckSmcCrossThread() {
  Say("check smc_cross_thread: start");
  auto Ctx = MakeContext(TsoOn);
  if (!Ctx) return false;
  auto G = NewGuest(*Ctx);
  if (!G) return false;
  const uint64_t Target = reinterpret_cast<uint64_t>(CodeBase) + MtBlobs::smc_target;
  auto SetImmediate = [&](uint32_t Value) {
    std::memcpy(CodeBase + MtBlobs::smc_target + 1, &Value, 4);
  };
  auto Invalidate = [&] {
    std::unique_lock Lock {Ctx->GetCodeInvalidationMutex()};
    Ctx->InvalidateCodeBuffersCodeRange(Target, 6);
    Ctx->InvalidateThreadCachedCodeRange(G->Core, Target, 6);
  };
  constexpr int Rounds = 20;
  int Passed = 0;
  std::vector<uint64_t> Latency;
  const int Core = Cores.size() > 1 ? Cores[1] : -2;
  for (int I = 0; I < Rounds; ++I) {
    SetImmediate(0x11111111);
    Invalidate();
    alignas(64) static std::atomic<uint64_t> Progress;
    Progress.store(0);
    using Body = std::function<void()>;
    auto T = Spawn<Body>([&] { Execute(*G, MtBlobs::smc, 0x22222222, reinterpret_cast<uint64_t>(&Progress)); }, ExecStack, Core);
    for (int Wait = 0; Wait < 2000 && Progress.load() < 10000; ++Wait) svcSleepThread(100'000);
    if (Progress.load() < 10000) {
      Say("  round %d: the guest loop made no progress (%" PRIu64 " calls)", I, Progress.load());
      break;
    }
    const uint64_t Patched = NowNs();
    SetImmediate(0x22222222);
    Invalidate();
    bool Exited = false;
    for (int Wait = 0; Wait < 20000 && !Exited; ++Wait) {
      Exited = G->EndNs >= Patched;
      if (!Exited) svcSleepThread(100'000);
    }
    if (!Exited) {
      Say("  round %d: still running old code 2 s after the invalidation; stopping it through the poll word", I);
      PollWord(G->Core, FEXCore::Core::InterruptPollRequestOffset).store(1, std::memory_order_release);
      for (int Wait = 0; Wait < 2000 && G->EndNs < Patched; ++Wait) svcSleepThread(100'000);
      if (G->EndNs < Patched) {
        Say("check smc_cross_thread: FAIL (the thread did not stop either)");
        (void)T.release();
        (void)G.release();
        (void)Ctx.release();
        return false;
      }
      Join(T);
      break;
    }
    Join(T);
    if (G->Exited) {
      ++Passed;
      Latency.push_back(G->EndNs - Patched);
    }
  }
  Percentiles("patch+invalidate to exit", Latency);
  Say("  %d/%d rounds saw the new code", Passed, Rounds);
  FreeGuest(G);
  Say("check smc_cross_thread: %s", Passed == Rounds ? "PASS" : "FAIL");
  return Passed == Rounds;
}

void CheckTsoLitmus(std::initializer_list<TsoMode> Modes = {TsoOff, TsoOn}, int Reps = 3) {
  Say("check tso_litmus: start (%s)", Features.Name);
  constexpr uint64_t Count = 20'000'000;
  bool OnClean = true;
  for (const TsoMode& Mode : Modes) {
    auto Ctx = MakeContext(Mode);
    if (!Ctx) return;
    for (int Rep = 0; Rep < Reps; ++Rep) {
      auto Writer = NewGuest(*Ctx), Reader = NewGuest(*Ctx);
      if (!Writer || !Reader) return;
      alignas(64) static uint64_t Shared[16];
      std::memset(Shared, 0, sizeof(Shared));
      using Body = std::function<void()>;
      const int CoreA = Cores.size() > 1 ? Cores[1] : -2, CoreB = Cores.size() > 2 ? Cores[2] : -2;
      const uint64_t Start = NowNs();
      auto R = Spawn<Body>([&] { Execute(*Reader, MtBlobs::mp_reader, Count, reinterpret_cast<uint64_t>(Shared)); }, ExecStack, CoreB);
      auto W = Spawn<Body>([&] { Execute(*Writer, MtBlobs::mp_writer, Count, reinterpret_cast<uint64_t>(Shared)); }, ExecStack, CoreA);
      Join(W);
      Join(R);
      Say("  %s run %d: %" PRIu64 " reorderings seen (flag newer than data) in %" PRIu64 " messages, %.1f ms", Mode.Name, Rep,
          Reader->Result, Count, Ms(NowNs() - Start));
      if (Mode.Tso && Reader->Result) OnClean = false;
      FreeGuest(Writer);
      FreeGuest(Reader);
    }
  }
  Say("check tso_litmus: %s (tso-on must see 0; tso-off may or may not show reordering)", OnClean ? "PASS" : "FAIL");
}

void CheckTsoCost(std::initializer_list<TsoMode> Modes = {TsoOff, TsoOn, TsoVector, TsoMemcpy}) {
  Say("check tso_cost: start (%s)", Features.Name);
  constexpr size_t Bytes = 8 << 20;
  auto* Buffer = static_cast<uint64_t*>(std::aligned_alloc(0x1000, Bytes * 2));
  if (!Buffer) return;
  // A random cycle through the first 8 MiB for the pointer chase (one pointer per 64-byte line).
  const size_t Lines = Bytes / 64;
  std::vector<uint32_t> Order(Lines);
  for (size_t I = 0; I < Lines; ++I) Order[I] = I;
  uint64_t Rng = 12345;
  for (size_t I = Lines - 1; I > 0; --I) {
    Rng = Rng * 6364136223846793005ull + 1;
    std::swap(Order[I], Order[(Rng >> 33) % (I + 1)]);
  }
  auto PrepareChase = [&] {
    for (size_t I = 0; I < Lines; ++I) Buffer[Order[I] * 8] = reinterpret_cast<uint64_t>(&Buffer[Order[(I + 1) % Lines] * 8]);
  };
  struct Bench {
    const char* Name;
    size_t Entry;
    uint64_t Rdi, Rdx;
  };
  const Bench Benches[] = {
    {"compute (spin 50M)", MtBlobs::spin, 50'000'000, 0},
    {"load+store (mem_rmw 8 MiB x10)", MtBlobs::mem_rmw, 10, Bytes / 8},
    {"vector copy (mem_vec 4 MiB x10)", MtBlobs::mem_vec, 10, Bytes / 2 / 8},
    {"rep movsb (mem_copy 8 MiB x10)", MtBlobs::mem_copy, 10, Bytes},
    {"pointer chase (5M loads)", MtBlobs::chase, 5'000'000, 0},
  };
  for (const TsoMode& Mode : Modes) {
    auto Ctx = MakeContext(Mode);
    if (!Ctx) break;
    auto G = NewGuest(*Ctx);
    if (!G) break;
    char Line[400];
    int Used = std::snprintf(Line, sizeof(Line), "  %-14s", Mode.Name);
    using Body = std::function<void()>;
    for (const auto& B : Benches) {
      uint64_t Elapsed = 0;
      auto T = Spawn<Body>([&] {
        std::memset(Buffer, 0, Bytes * 2);
        if (B.Entry == MtBlobs::chase) PrepareChase();
        const uint64_t Rsi = B.Entry == MtBlobs::spin ? 1 : B.Entry == MtBlobs::chase ? reinterpret_cast<uint64_t>(&Buffer[Order[0] * 8])
                                                                                       : reinterpret_cast<uint64_t>(Buffer);
        Execute(*G, B.Entry, 1, Rsi, B.Rdx);  // warm up: compile
        if (B.Entry == MtBlobs::chase) PrepareChase();
        const uint64_t Start = NowNs();
        Execute(*G, B.Entry, B.Rdi, Rsi, B.Rdx);
        Elapsed = NowNs() - Start;
      }, ExecStack, -2);
      Join(T);
      Used += std::snprintf(Line + Used, sizeof(Line) - Used, " | %s %.1f ms", B.Name, Ms(Elapsed));
    }
    Say("%s", Line);
    FreeGuest(G);
  }
  std::free(Buffer);
  Say("check tso_cost: INFO");
}

void CheckThreadMemory() {
  Say("check thread_memory: start");
  auto Ctx = MakeContext(TsoOn);
  if (!Ctx) return;
  constexpr int Count = 8;
  uint64_t UsedBefore {}, UsedAfter {};
  svcGetInfo(&UsedBefore, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  const auto HeapBefore = mallinfo().uordblks;
  const uint64_t Start = NowNs();
  std::vector<std::unique_ptr<GuestRun>> Guests;
  for (int I = 0; I < Count; ++I) Guests.push_back(NewGuest(*Ctx));
  const uint64_t Elapsed = NowNs() - Start;
  svcGetInfo(&UsedAfter, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  const auto HeapAfter = mallinfo().uordblks;
  Say("  %d FEX threads: %.1f us per CreateThread (with the probe's 4 MiB call/return stack and 64 KiB guest stack), newlib heap +%lld KiB "
      "per thread, process memory +%lld KiB per thread",
      Count, Elapsed / 1e3 / Count, (long long)(HeapAfter - HeapBefore) / 1024 / Count, (long long)(UsedAfter - UsedBefore) / 1024 / Count);
  for (auto& G : Guests) FreeGuest(G);
  Say("check thread_memory: INFO");
}

// Last: a host stack that is too small for the FEX compiler crashes the app (no fault delivery on the console).
void CheckExecStack() {
  Say("check exec_stack: start (a crash here means the size and step logged last are too small)");
  for (size_t Stack : {size_t {3} << 20, size_t {2} << 20, size_t {1} << 20, size_t {512} << 10, size_t {256} << 10, size_t {128} << 10}) {
    Say("  %zu KiB host stack: new context", Stack >> 10);
    auto Ctx = MakeContext(TsoOn);
    if (!Ctx) return;
    auto G = NewGuest(*Ctx);
    if (!G) return;
    auto* Buffer = static_cast<uint64_t*>(std::aligned_alloc(0x1000, 0x20000));
    std::memset(Buffer, 0, 0x20000);
    bool Ok = true;
    const auto Data = reinterpret_cast<uint64_t>(Buffer);
    using Body = std::function<void()>;
    auto T = Spawn<Body>([&] {
      Say("    spin");
      Execute(*G, MtBlobs::spin, 1000, 1);
      Ok &= G->Exited && G->Result == Lcg(1000, 1);
      Say("    mem_rmw");
      Execute(*G, MtBlobs::mem_rmw, 2, Data, 1024);
      Say("    mem_vec");
      Execute(*G, MtBlobs::mem_vec, 2, Data, 1024);
      Say("    mem_copy");
      Execute(*G, MtBlobs::mem_copy, 2, Data, 4096);
      Say("    chase");
      Buffer[0] = Data;  // after mem_rmw, which adds to every qword (runs 1 and 2 chased Buffer+2 and crashed: a probe bug)
      Execute(*G, MtBlobs::chase, 100, Data);
      Ok &= G->Exited;
    }, Stack, Cores.size() > 1 ? Cores[1] : -2);
    if (!T) {
      Say("  could not start the thread");
      return;
    }
    Join(T);
    std::free(Buffer);
    FreeGuest(G);
    Say("  %zu KiB: %s", Stack >> 10, Ok ? "ok" : "wrong results");
  }
  Say("check exec_stack: INFO");
}

} // namespace

int main() {
  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState Pad {};
  padInitializeDefault(&Pad);
  mutexInit(&SayLock);
  if (!fsdevGetDeviceFileSystem("sdmc")) fsdevMountSdmc();
  Log = std::fopen(LogPath, "w");
  Say("--- FEX Switch 2 %s (EXPERIMENT, multithreading Task 0) ---", Version);
  Say("Log: %s. Runs every check, then waits for + to exit.", LogPath);
  consoleUpdate(nullptr);

  FEXCore::Config::Initialize();
  LogMan::Msg::InstallHandler([](LogMan::DebugLevels, const char* Message) { Say("FEX: %s", Message); });
  LogMan::Throw::InstallHandler([](const char* Message) { Say("FEX throw: %s", Message); });

  CodeBase = static_cast<uint8_t*>(std::aligned_alloc(0x1000, CodeSize));
  std::memset(CodeBase, 0xcc, CodeSize);
  std::memcpy(CodeBase, MtBlobs::Code, sizeof(MtBlobs::Code));

  FILE* Sidecar = std::fopen(FullSidecar, "r");
  const bool Full = Sidecar != nullptr;
  if (Sidecar) std::fclose(Sidecar);
  Say("Mode: %s", Full ? "every check (mtprobe.full present)" : "run 3: stack ladder, then TSO cost per CPU feature set");
  CheckInfo();
  consoleUpdate(nullptr);
  if (Full) {
    CheckThreadLimit();
    CheckCoreScaling(true);
    CheckWakeLatency();
    if (!CheckJitParallel()) Say("STOP: guest threads do not run in parallel in one context.");
    else {
      CheckPollStop();
      if (!CheckSmcCrossThread()) Say("STOP after smc_cross_thread.");
      else {
        CheckTsoLitmus();
        CheckTsoCost();
        CheckThreadMemory();
        CheckExecStack();
      }
    }
  } else {
    CheckExecStack();
    consoleUpdate(nullptr);
    // Feature ladder, least risky first. Each set: TSO off/on cost, then the litmus with TSO on (must stay 0).
    for (const FeatureSet& Set : {FeatureSet {"LSE atomics", true, false, false}, FeatureSet {"LSE atomics + RCPC (LDAPR)", true, true, false},
                                  FeatureSet {"LSE atomics + RCPC + LRCPC2 (TSOImm9)", true, true, true}}) {
      Say("feature set: %s (a crash right after this line means the CPU lacks it)", Set.Name);
      Features = Set;
      CheckTsoCost({TsoOff, TsoOn, TsoVector});
      consoleUpdate(nullptr);
      CheckTsoLitmus({TsoOn}, 1);
      consoleUpdate(nullptr);
    }
  }
  Say("--- mtprobe finished. Press + to exit. ---");
  while (appletMainLoop()) {
    padUpdate(&Pad);
    if (padGetButtonsDown(&Pad) & HidNpadButton_Plus) break;
    consoleUpdate(nullptr);
  }
  LogMan::Throw::UnInstallHandler();
  LogMan::Msg::UnInstallHandler();
  FEXCore::Config::Shutdown();
  if (Log) std::fclose(Log);
  consoleExit(nullptr);
  return 0;
}
