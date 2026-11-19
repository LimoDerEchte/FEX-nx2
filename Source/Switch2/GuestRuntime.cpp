// SPDX-License-Identifier: MIT
#include "GuestRuntime.h"
#include "GuestAddressSpace.h"
#include "GuestRootFS.h"
#include "GuestSocket.h"
#include "GuestTerminal.h"
#include "GuestVideo.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/Switch2/GuestTrace.h>

#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <elf.h>
#include <map>

namespace Switch2Guest {
namespace {
constexpr size_t GuestPageSize = PageSize;
constexpr size_t MainStackSize = 1024 * 1024;
constexpr uint64_t LinuxWriteSyscall = 1;
constexpr uint64_t LinuxExitSyscall = 60;

struct TraceMapping {
  uint64_t Start;
  size_t Size;
  uint64_t Offset;
  std::string Path;
};
std::vector<TraceMapping> TraceMappings;

const TraceMapping* FindTraceMapping(uint64_t RIP) {
  for (auto It = TraceMappings.rbegin(); It != TraceMappings.rend(); ++It) {
    if (RIP >= It->Start && RIP - It->Start < It->Size) return &*It;
  }
  return nullptr;
}

void RecordMapping(uint64_t Start, size_t Size, uint64_t Offset, std::string_view Path) {
  if (Logging == LogLevel::Trace) TraceMappings.push_back({Start, Size, Offset, std::string {Path}});
}

unsigned FirstDispatchCompileCheckpoints {};

void TraceGuest(const char* Stage, uint64_t RIP, uint64_t Detail) {
  char Message[384];
  const auto Length = std::snprintf(Message, sizeof(Message), "%s  guest RIP=%#llx  detail=%#llx", Stage,
                static_cast<unsigned long long>(RIP), static_cast<unsigned long long>(Detail));
  if (std::strcmp(Stage, "Compile entry") == 0 || std::strcmp(Stage, "Dispatch compiled block") == 0) {
    const auto* Mapping = FindTraceMapping(RIP);
    if (Mapping && !Mapping->Path.empty()) {
      const auto Offset = RIP - Mapping->Start;
      const auto* Bytes = reinterpret_cast<const uint8_t*>(RIP);
      char Hex[33] {};
      for (size_t Index = 0; Index < std::min<size_t>(16, Mapping->Size - Offset); ++Index) {
        std::snprintf(Hex + Index * 2, 3, "%02x", Bytes[Index]);
      }
      const auto Name = std::string_view(Mapping->Path).substr(Mapping->Path.find_last_of('/') + 1);
      std::snprintf(Message + Length, sizeof(Message) - Length, "  %.*s+%#llx  x86=%s",
                    static_cast<int>(std::min<size_t>(Name.size(), 120)), Name.data(),
                    static_cast<unsigned long long>(Mapping->Offset + Offset), Hex);
    }
  }
  WriteLog(Message);
  if (FirstDispatchCompileCheckpoints) {
    WriteRunCheckpoint(Message);
    --FirstDispatchCompileCheckpoints;
  }
}

uint64_t BuildInitialStack(uint64_t StackBase, std::string_view Program, std::span<const std::string> Arguments,
                           std::span<const std::string> Environment, const ElfImage* ProgramImage, uint64_t InterpreterBase) {
  char* Strings = reinterpret_cast<char*>(StackBase + MainStackSize);
  auto PushString = [&](std::string_view Text) {
    Strings -= Text.size() + 1;
    std::memcpy(Strings, Text.data(), Text.size());
    Strings[Text.size()] = 0;
    return reinterpret_cast<uint64_t>(Strings);
  };
  const auto ProgramAddress = PushString(Program);
  std::vector<uint64_t> InitialStack {Arguments.size()};
  for (const auto& Argument : Arguments) InitialStack.push_back(PushString(Argument));
  InitialStack.push_back(0);
  for (const auto& Variable : Environment) InitialStack.push_back(PushString(Variable));
  InitialStack.push_back(0);
  if (ProgramImage) {
    Strings -= 16;
    randomGet(Strings, 16);
    InitialStack.insert(InitialStack.end(), {
      AT_PHDR, ProgramImage->ProgramHeaders, AT_PHENT, sizeof(Elf64_Phdr), AT_PHNUM, ProgramImage->ProgramHeaderCount,
      AT_PAGESZ, GuestPageSize, AT_BASE, InterpreterBase, AT_ENTRY, ProgramImage->Entry, AT_EXECFN, ProgramAddress,
      AT_RANDOM, reinterpret_cast<uint64_t>(Strings), AT_UID, 1000, AT_EUID, 1000, AT_GID, 1000, AT_EGID, 1000, AT_SECURE, 0});
  }
  InitialStack.insert(InitialStack.end(), {AT_NULL, 0});
  const size_t StackBytes = InitialStack.size() * sizeof(uint64_t);
  const auto Stack = (reinterpret_cast<uint64_t>(Strings) - StackBytes) & ~15ULL;
  std::memcpy(reinterpret_cast<void*>(Stack), InitialStack.data(), StackBytes);
  return Stack;
}

class GuestSyscalls final : public FEXCore::HLE::SyscallHandler {
public:
  GuestSyscalls(std::span<const Switch2Guest::Segment> Segments, std::string Folder, std::string RootPath, std::string WorkingDirectory, bool Interactive)
    : RootPath {std::move(RootPath)}
    , Interactive {Interactive} {
    Task = &Processes.try_emplace(ProcessID).first->second;
    Task->HasDispatched = true;
    Task->Folder = std::move(Folder);
    Task->WorkingDirectory = std::move(WorkingDirectory);
    for (const auto& Segment : Segments) {
      Task->Memory.Register(Segment.Start, Segment.Size, Segment.Flags);
    }
    for (size_t FD = 0; FD < 3; ++FD) {
      auto& File = Task->Files[FD];
      File = std::make_shared<OpenFile>();
      File->Console = FD == 0 ? ConsoleStream::Input : FD == 1 ? ConsoleStream::Output : ConsoleStream::Error;
      File->StatusFlags = FD == 0 ? 0 : 1;
      File->Inode = FD + 1;
    }
    padInitializeDefault(&Input);
    padUpdate(&Input);
    if (Interactive) {
      hidInitializeKeyboard();
      hidGetKeyboardStates(&LastKeyboard, 1);
    }
  }

  ~GuestSyscalls() override {
    CloseVideo();
  }

  uint64_t CreateStack(size_t Size) { return Task->Memory.Allocate(Size, PF_R | PF_W); }
  bool InitMain(FEXCore::Core::CPUState& State) {
    InitializeExecCPU(*Task, State);
    Task->TraceFirstDispatch = true;
    return InitializeProcessContext(*Task);
  }
  bool AddMainThread(FEXCore::Core::CPUState& State) {
    return AddThread(*Task->Context, State, ProcessID, 0, ProcessID);
  }

  std::optional<uint64_t> RunThreads() {
    while (!ProcessExited) {
      bool Ran = false;
      for (size_t Index = 0; Index < Threads.size() && !ProcessExited; ++Index) {
        auto& Thread = Threads[Index];
        if (!Thread.Core) continue;
        if (Thread.Exited) {
          Thread.Core->CTX->DestroyThread(Thread.Core);
          Thread = {};
          continue;
        }
        if (!Ready(Thread)) continue;
        if (!ActivateProcess(Thread.PID)) {
          PrintStage("Could not save the guest process address space.");
          DispatchFailed = true;
          break;
        }
        Current = &Thread;
        if (Current->SetTidOnStart) {
          CopyOut(Current->SetTidOnStart, Current->TID);
          Current->SetTidOnStart = 0;
        }
        const auto Before = CurrentThreadTicks();
        if (Task->TraceFirstDispatch && Logging == LogLevel::Trace) {
          LogFirstDispatch(*Thread.Core->CurrentFrame);
          FirstDispatchCompileCheckpoints = 11;
        }
        Task->TraceFirstDispatch = false;
        Dispatch(*Thread.Core->CTX);
        FirstDispatchCompileCheckpoints = 0;
        Task->CpuTicks += CurrentThreadTicks() - Before;
        if (Task->StartupSyscallCheckpoints && Task->LastStartupSyscall) {
          char Message[112];
          std::snprintf(Message, sizeof(Message), "%s syscall %llu completed",
                        Task->Image ? "Replacement" : "Fork continuation",
                        static_cast<unsigned long long>(Task->LastStartupSyscall - 1));
          ProcessCheckpoint(Message);
          Task->LastStartupSyscall = 0;
          --Task->StartupSyscallCheckpoints;
        }
        if (PendingFork) CompleteFork();
        if (Task->Exited && Task->PID != ProcessID) {
          ProcessCheckpoint("Child exit, retiring process");
          RetireProcess(Task->PID);
          ProcessCheckpoint("Child exit, process retired");
        } else if (Task->PendingExec) {
          ProcessCheckpoint("Exec request, replacing guest image");
          if (!InstallExec()) DispatchFailed = true;
          else ProcessCheckpoint("Exec request, replacement ready");
        }
        Ran = true;
      }
      if (!Ran && !ProcessExited) {
        if (!std::any_of(Threads.begin(), Threads.end(), [](const auto& Thread) { return Thread.Core; })) break;
        if (!appletMainLoop()) break;
        if (std::none_of(Threads.begin(), Threads.end(), [](const auto& Thread) {
              return Thread.Deadline || Thread.Waiting == WaitKind::Terminal;
            })) {
          PrintStage("Guest threads are blocked with no remaining producer.");
          break;
        }
        if (Interactive) PollTerminalInput();
        svcSleepThread(8'000'000);
      }
      if (DispatchFailed) break;
    }
    for (auto& Thread : Threads) {
      if (Thread.Core) Thread.Core->CTX->DestroyThread(Thread.Core);
      Thread = {};
    }
    Current = nullptr;
    FlushGuestOutput();
    return ProcessExited ? std::optional {ExitStatus} : std::nullopt;
  }

  uint64_t Number {};

  void CloseVideo() {
    if (GraphicsActive) {
      framebufferClose(&Video);
      GraphicsActive = false;
      consoleInit(nullptr);
    }
  }

  void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override {
    using namespace FEXCore::X86State;
    auto& Registers = Frame->State.gregs;
    if (Logging == LogLevel::Trace) {
      char Message[224];
      std::snprintf(Message, sizeof(Message), "Syscall %llu enter rip=%#llx args=%#llx,%#llx,%#llx,%#llx,%#llx,%#llx",
                    static_cast<unsigned long long>(Registers[REG_RAX]), static_cast<unsigned long long>(Frame->State.rip),
                    static_cast<unsigned long long>(Registers[REG_RDI]), static_cast<unsigned long long>(Registers[REG_RSI]),
                    static_cast<unsigned long long>(Registers[REG_RDX]), static_cast<unsigned long long>(Registers[REG_R10]),
                    static_cast<unsigned long long>(Registers[REG_R8]), static_cast<unsigned long long>(Registers[REG_R9]));
      WriteLog(Message);
    }
    if (Registers[REG_RAX] == LinuxExitSyscall || Registers[REG_RAX] == 231) {
      const auto ExitCall = Registers[REG_RAX];
      ExitThread();
      Task->Exited = ExitCall == 231 || std::none_of(Threads.begin(), Threads.end(), [&](const auto& Thread) {
        return Thread.Core && Thread.PID == Task->PID && !Thread.Exited;
      });
      if (Task->Exited) Task->Status = Registers[REG_RDI] & 0xff;
      if (Task->Exited && Task->PID == ProcessID) {
        Number = ExitCall;
        ExitStatus = Task->Status;
        ProcessExited = true;
      }
      std::longjmp(ReturnToHost, 1);
    }

    constexpr int64_t LinuxEnosys = -38;
    const auto A = Registers[REG_RDI], B = Registers[REG_RSI], C = Registers[REG_RDX];
    int64_t Result = LinuxEnosys;
    switch (Registers[REG_RAX]) {
    case 0: Result = ReadGuest(A, B, C); break;
    case LinuxWriteSyscall: Result = WriteGuest(A, B, C); break;
    case 2: Result = OpenGuest(CurrentDirectory, A, B); break;
    case 3: Result = CloseGuest(A); break;
    case 4: case 6: Result = StatAtGuest(CurrentDirectory, A, B, 0); break;
    case 5: Result = StatGuest(A, B); break;
    case 7: Result = PollGuest(A, B, C); break;
    case 8: Result = SeekGuest(A, B, C); break;
    case 9:
      Result = MapGuest(Frame, A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]);
      break;
    case 10: Result = ProtectGuest(Frame, A, B, C); break;
    case 11: Result = UnmapGuest(Frame, A, B); break;
    case 12: Result = SetHeapEnd(A); break;
    case 13: Result = SignalAction(A, B, C, Registers[REG_R10]); break;
    case 14: Result = SignalMask(A, B, C, Registers[REG_R10]); break;
    case 16: Result = TerminalIoctl(A, B, C); break;
    case 17: Result = ReadAtGuest(A, B, C, Registers[REG_R10]); break;
    case 20: Result = WriteVectorGuest(A, B, C); break;
    case 21: Result = AccessGuest(CurrentDirectory, A, B, 0); break;
    case 22: Result = CreatePipe(A, 0); break;
    case 24: Result = 0; break;
    case 28: Result = AdviseGuest(A, B, C); break;
    case 32: Result = FileControl(A, 0, 0); break;
    case 33: Result = DuplicateDescriptor(A, B, false); break;
    case 39: case 124: Result = Task->PID; break;
    case 111: Result = Task->Group; break;
    case 110: Result = Task->ParentPID; break;
    case 121: Result = GetProcessGroup(A); break;
    case 41: Result = CreateSocket(A, B, C); break;
    case 42: Result = ConnectSocket(A, B, C); break;
    case 43: Result = AcceptSocket(A, B, C, 0); break;
    case 44: case 45: Result = SocketTransfer(Registers[REG_RAX] == 44, A, B, C, Registers[REG_R10],
                                               Registers[REG_R8], Registers[REG_R9]); break;
    case 48: Result = ShutdownSocket(A, B); break;
    case 49: Result = BindSocket(A, B, C); break;
    case 50: Result = ListenSocket(A, B); break;
    case 53: Result = SocketPair(A, B, C, Registers[REG_R10]); break;
    case 56: Result = A & 0x10000 ? CloneThread(Frame, A, B, C, Registers[REG_R10], Registers[REG_R8]) :
                                     B == 0 ? RequestFork(A, Registers[REG_R10]) : -38; break;
    case 57: Result = RequestFork(17, 0); break;
    case 59: Result = ExecGuest(A, B, C); break;
    case 61: Result = WaitChild(A, B, C, Registers[REG_R10]); break;
    case 63: Result = UnameGuest(A); break;
    case 72: Result = FileControl(A, B, C); break;
    case 79: Result = GetCwd(A, B); break;
    case 80: Result = ChangeDirectory(A); break;
    case 97: Result = LimitGuest(A, B, 0); break;
    case 99: Result = SystemInfo(A); break;
    case 102: case 104: case 107: case 108: Result = 1000; break;
    case 109: Result = SetProcessGroup(A, B); break;
    case 131: Result = AltStack(A, B); break;
    case 158: Result = ArchPrctl(Frame, A, B); break;
    case 186: Result = Current->TID; break;
    case 191: case 192: Result = GetAttribute(A); break;
    case 193: Result = (GetFile(A)) ? -61 : BadFile; break;
    case 202: Result = Futex(A, B, C, Registers[REG_R10], Registers[REG_R9]); break;
    case 204: Result = GetAffinity(A, B, C); break;
    case 217: Result = ReadDirents(A, B, C); break;
    case 218: Result = SetTidAddress(A); break;
    case 228: Result = ClockGuest(A, B); break;
    case 262: Result = StatAtGuest(A, B, C, Registers[REG_R10]); break;
    case 269: Result = AccessGuest(A, B, C, 0); break;
    case 270: Result = SelectGuest(A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]); break;
    case 288: Result = AcceptSocket(A, B, C, Registers[REG_R10]); break;
    case 292: Result = A == B || C & ~02000000ULL ? Invalid : DuplicateDescriptor(A, B, (C & 02000000) != 0); break;
    case 293: Result = CreatePipe(A, B); break;
    case 302: Result = A == 0 || A == Task->PID ? LimitGuest(B, Registers[REG_R10], C) : -3; break;
    case 318: Result = RandomGuest(A, B, C); break;
    case 332: Result = StatxGuest(A, B, C, Registers[REG_R8]); break;
    case 439: Result = AccessGuest(A, B, C, Registers[REG_R10]); break;
    case FEX_NX2_PRESENT_RGBA: Result = PresentGuestFrame(A); break;
    case FEX_NX2_POLL_INPUT: Result = PollGameInput(); break;
    case FEX_NX2_TICKS_MS: Result = armTicksToNs(armGetSystemTick() - StartTick) / 1'000'000; break;
    case FEX_NX2_DELAY_MS: svcSleepThread(std::min<uint64_t>(A, 1000) * 1'000'000); Result = 0; break;
    case 257: // openat
      Result = OpenGuest(A, B, C);
      break;
    }
    if (Result == RetrySyscall || Task->PendingExec) {
      std::longjmp(ReturnToHost, 1);
    }
    Current->Deadline = 0;
    if (Logging == LogLevel::Trace || (Logging == LogLevel::Detailed && Result < 0 && Result >= -4095)) {
      char Message[128];
      std::snprintf(Message, sizeof(Message), "Guest syscall %llu at %#llx returned %lld",
                    static_cast<unsigned long long>(Registers[REG_RAX]), static_cast<unsigned long long>(Frame->State.rip),
                    static_cast<long long>(Result));
      WriteLog(Message);
    }
    if (Task->StartupSyscallCheckpoints) {
      Task->LastStartupSyscall = Registers[REG_RAX] + 1;
      Registers[REG_RAX] = Result;
      Frame->State.rip += 2;
      std::longjmp(ReturnToHost, 1);
    }
    Registers[REG_RAX] = Result;
    Frame->State.rip += 2;
    if (std::any_of(Threads.begin(), Threads.end(), [&](auto& Thread) {
          return &Thread != Current && Thread.Core && !Thread.Exited &&
                 Thread.PID == Current->PID && Ready(Thread);
        })) {
      std::longjmp(ReturnToHost, 1);
    }
  }

  void PreCompile() override {
    if (Logging == LogLevel::Detailed) {
      WriteLog("FEXCore entered guest compilation.");
    }
  }

  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Address) override {
    const auto Range = Task->Memory.Query(Address);
    if (Range.Flags & PF_X) {
      return {Range.Start, Range.Size, (Range.Flags & PF_W) != 0};
    }
    return {};
  }

  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }

private:
  static constexpr uint32_t ProcessID = 100;
  static constexpr int64_t RetrySyscall = INT64_MIN;
  using ContextType = FEXCore::Context::Context;
  using CoreFrame = FEXCore::Core::CpuStateFrame;
  enum class WaitKind { None, Futex, IO, Terminal };
  struct GuestThread {
    FEXCore::Core::InternalThreadState* Core {};
    std::unique_ptr<void, decltype(&std::free)> ReturnStack {nullptr, &std::free};
    uint64_t ReturnStackTop {};
    uint64_t ClearTid {};
    uint64_t SignalMask {};
    std::array<uint64_t, 3> AltStack {0, 2, 0};
    uint64_t Deadline {};
    uint64_t WaitAddress {};
    uint32_t WaitMask {};
    uint32_t TID {};
    uint32_t PID {};
    uint64_t SetTidOnStart {};
    WaitKind Waiting {};
    bool Exited {};
  };

  bool AddThread(ContextType& Context, FEXCore::Core::CPUState& State, uint32_t TID, uint64_t ClearTid, uint32_t PID) {
    const auto Slot = std::find_if(Threads.begin(), Threads.end(), [](const auto& Thread) { return !Thread.Core; });
    if (Slot == Threads.end()) return false;
    constexpr auto Size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
    Slot->ReturnStack.reset(std::aligned_alloc(GuestPageSize, Size));
    if (!Slot->ReturnStack) return false;
    std::memset(Slot->ReturnStack.get(), 0, Size);
    Slot->ReturnStackTop = reinterpret_cast<uint64_t>(Slot->ReturnStack.get()) + Size - 16;
    State.callret_sp = Slot->ReturnStackTop;
    Slot->Core = Context.CreateThread(&State);
    Slot->Core->CallRetStackBase = Slot->ReturnStack.get();
    Slot->TID = TID;
    Slot->PID = PID;
    Slot->ClearTid = ClearTid;
    Slot->SignalMask = Current ? Current->SignalMask : 0;
    return true;
  }

  void Dispatch(ContextType& Context) {
    auto* Frame = Current->Core->CurrentFrame;
    // The syscall trampoline spills the guest registers. Its host call/return
    // cache is dispensable and may contain entries invalidated by another guest thread.
    Frame->State.callret_sp = Current->ReturnStackTop;
    Frame->InSyscallInfo = 0;
    if (setjmp(ReturnToHost) == 0) {
      Context.ExecuteThread(Current->Core);
      DispatchFailed = true;
      PrintStage("Dispatcher returned before the guest reached its syscall.");
    }
  }

  static uint64_t MonotonicNs() { return armTicksToNs(armGetSystemTick()); }

  static uint64_t CurrentThreadTicks() {
    u64 Ticks {};
    svcGetInfo(&Ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, TickCountInfo_Total);
    return Ticks;
  }

  bool Ready(GuestThread& Thread) {
    if (Thread.Waiting == WaitKind::None) return true;
    const bool Expired = Thread.Deadline && MonotonicNs() >= Thread.Deadline;
    if (Thread.Waiting == WaitKind::Futex && Expired) {
      Thread.Core->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX] = -110;
      Thread.Deadline = 0;
      Thread.Waiting = WaitKind::None;
      return true;
    }
    const bool InputAvailable = Thread.Waiting == WaitKind::Terminal && TerminalClosed;
    if ((Thread.Waiting == WaitKind::IO || Thread.Waiting == WaitKind::Terminal) &&
        (Expired || InputAvailable || Thread.WaitAddress != IOEpoch)) {
      Thread.Waiting = WaitKind::None;
      return true;
    }
    return false;
  }

  int64_t WaitForIO(WaitKind Kind = WaitKind::IO) {
    Current->Waiting = Kind;
    Current->WaitAddress = IOEpoch;
    return RetrySyscall;
  }

  static constexpr uint64_t MaxDeadline = UINT64_MAX - 1;
  static uint64_t AddDeadline(uint64_t Base, uint64_t Duration) {
    return Base + std::min(Duration, MaxDeadline - Base);
  }

  bool ReadTimespec(uint64_t Address, uint64_t& Duration) const {
    int64_t Time[2];
    if (Available(Address, PF_R) < sizeof(Time)) return false;
    std::memcpy(Time, reinterpret_cast<const void*>(Address), sizeof(Time));
    if (Time[0] < 0 || Time[1] < 0 || Time[1] >= 1'000'000'000) return false;
    Duration = Time[0] > static_cast<int64_t>((MaxDeadline - Time[1]) / 1'000'000'000)
      ? MaxDeadline : Time[0] * 1'000'000'000ULL + Time[1];
    return true;
  }

  int64_t CloneThread(CoreFrame* Frame, uint64_t Flags, uint64_t Stack, uint64_t ParentTid, uint64_t ChildTid, uint64_t TLS) {
    constexpr uint64_t Shared = 0x10f00; // VM, FS, FILES, SIGHAND, THREAD
    constexpr uint64_t SetTLS = 0x80000, ParentSetTid = 0x100000, ChildClearTid = 0x200000, ChildSetTid = 0x1000000;
    if ((Flags & Shared) != Shared || Flags & ~(Shared | SetTLS | ParentSetTid | ChildClearTid | ChildSetTid | 0x40000ULL)) return -38;
    if (Stack < 16 || Available(Stack - 16, PF_W) < 16 ||
        ((Flags & ParentSetTid) && Available(ParentTid, PF_W) < 4) ||
        ((Flags & (ChildClearTid | ChildSetTid)) && Available(ChildTid, PF_W) < 4)) return Fault;
    FEXCore::Core::CPUState State {};
    std::memcpy(&State, &Frame->State, sizeof(State));
    State.rip += 2;
    State.gregs[FEXCore::X86State::REG_RAX] = 0;
    State.gregs[FEXCore::X86State::REG_RSP] = Stack;
    if (Flags & SetTLS) State.fs_cached = TLS;
    const auto TID = NextTid;
    if (!AddThread(*Frame->Thread->CTX, State, TID, Flags & ChildClearTid ? ChildTid : 0, Task->PID)) return -11;
    ++NextTid;
    if (Flags & ParentSetTid) std::memcpy(reinterpret_cast<void*>(ParentTid), &TID, sizeof(TID));
    if (Flags & ChildSetTid) std::memcpy(reinterpret_cast<void*>(ChildTid), &TID, sizeof(TID));
    return TID;
  }

  int64_t SetTidAddress(uint64_t Address) {
    Current->ClearTid = Address;
    return Current->TID;
  }

  uint32_t WakeFutex(uint64_t Address, uint32_t Count, uint32_t Mask) {
    uint32_t Woken {};
    for (auto& Thread : Threads) {
      if (Woken == Count) break;
      if (Thread.PID != Task->PID || Thread.Waiting != WaitKind::Futex || Thread.WaitAddress != Address || !(Thread.WaitMask & Mask)) continue;
      Thread.Core->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX] = 0;
      Thread.Waiting = WaitKind::None;
      Thread.Deadline = 0;
      ++Woken;
    }
    return Woken;
  }

  void ExitThread() {
    const uint32_t Zero {};
    if (Current->ClearTid && Available(Current->ClearTid, PF_W) >= sizeof(Zero)) {
      std::memcpy(reinterpret_cast<void*>(Current->ClearTid), &Zero, sizeof(Zero));
      WakeFutex(Current->ClearTid, 1, UINT32_MAX);
    }
    Current->Exited = true;
  }

  int64_t Futex(uint64_t Address, uint64_t Operation, uint32_t Value, uint64_t Timeout, uint32_t Mask) {
    if ((Address & 3) || (Operation & ~0x1ffULL)) return Invalid;
    if (Available(Address, PF_R) < 4) return Fault;
    const auto Command = Operation & 0x7f;
    if (Command == 0 || Command == 1) Mask = UINT32_MAX;
    if (!Mask || ((Operation & 0x100) && Command != 9)) return Invalid;
    if (Command == 1 || Command == 10) return WakeFutex(Address, Value, Mask);
    if (Command != 0 && Command != 9) return -38;
    uint32_t Word {};
    std::memcpy(&Word, reinterpret_cast<const void*>(Address), sizeof(Word));
    if (Word != Value) return -11;
    if (Timeout) {
      uint64_t Time {};
      if (!ReadTimespec(Timeout, Time)) return Available(Timeout, PF_R) < 16 ? Fault : Invalid;
      const auto Now = MonotonicNs();
      if (Command == 0) Time = AddDeadline(Now, Time);
      if (Command == 9 && (Operation & 0x100)) {
        const uint64_t RealTime = time(nullptr) * 1'000'000'000ULL;
        if (Time <= RealTime) return -110;
        Time = AddDeadline(Now, Time - RealTime);
      }
      if (Time <= Now) return -110;
      Current->Deadline = Time;
    }
    Current->Waiting = WaitKind::Futex;
    Current->WaitAddress = Address;
    Current->WaitMask = Mask;
    Current->Core->CurrentFrame->State.rip += 2;
    return RetrySyscall;
  }

  static constexpr int64_t BadFile = -9;
  static constexpr int64_t Fault = -14;
  static constexpr int64_t Invalid = -22;
  enum class ConsoleStream { None, Input, Output, Error, Terminal };
  static constexpr size_t DescriptorCount = 256;
  static constexpr uint64_t NoDescriptor = UINT64_MAX;
  struct OpenFile {
    ~OpenFile() {
      if (RealFile) fsFileClose(&Handle);
    }
    ConsoleStream Console {};
    FsFile Handle {};
    s64 Offset {};
    bool RealFile {};
    bool Pipe {};
    std::string_view Contents;
    std::string Path;
    uint64_t Inode {};
    uint32_t StatusFlags {};
    std::optional<std::vector<DirectoryEntry>> Directory;
    std::shared_ptr<Switch2Guest::StreamSocket> Socket;
  };
  using DescriptorTable = std::array<std::shared_ptr<OpenFile>, DescriptorCount>;
  using SignalTable = std::array<std::array<uint64_t, 4>, 64>;
  struct ExecImage {
    ElfImage Program;
    ElfImage Interpreter;
    std::string Path;
    std::vector<std::string> Arguments;
    std::vector<std::string> Environment;
  };

  struct GuestProcess {
    GuestAddressSpace Memory;
    std::array<FEXCore::Core::CPUState::gdt_segment, 32> GDT {};
    // Only fork descendants can share a private host address view. Exec
    // builds a fresh address space with independently owned backing.
    std::shared_ptr<const uint8_t> ForkView {std::make_shared<const uint8_t>(0)};
    std::string Folder;
    std::string WorkingDirectory;
    DescriptorTable Files {};
    std::array<uint8_t, DescriptorCount> DescriptorFlags {};
    SignalTable SignalActions {};
    uint64_t HeapStart {};
    uint64_t HeapEnd {};
    uint64_t CpuTicks {};
    uint32_t PID {ProcessID};
    uint32_t ParentPID {1};
    uint32_t Group {ProcessID};
    bool Exited {};
    bool HasDispatched {};
    bool RestorePending {};
    unsigned StartupSyscallCheckpoints {};
    bool TraceFirstDispatch {};
    uint64_t LastStartupSyscall {};
    uint32_t Status {};
    std::optional<ExecImage> PendingExec;
    std::shared_ptr<ExecImage> Image;
    std::vector<std::shared_ptr<ExecImage>> InheritedImages;
    FEXCore::SignalDelegator Signals;
    fextl::unique_ptr<ContextType> Context;
  };

  static void InitializeExecCPU(GuestProcess& Process, FEXCore::Core::CPUState& State) {
    using CPUState = FEXCore::Core::CPUState;
    Process.GDT = {};
    State.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_GDT] = Process.GDT.data();
    State.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_LDT] = Process.GDT.data();
    State.cs_idx = CPUState::DEFAULT_USER_CS << 3;
    auto* Code = CPUState::GetSegmentFromIndex(State, State.cs_idx);
    CPUState::SetGDTLimit(Code, 0xF'FFFFU);
    Code->L = 1;
  }

  bool InitializeProcessContext(GuestProcess& Process) {
    FEXCore::HostFeatures Features {};
    Features.ProcessFeatures.Is64BitMode = true;
    Features.DCacheLineLog2 = 4;
    Process.Context = ContextType::CreateNewContext(Features);
    Process.Context->SetSignalDelegator(&Process.Signals);
    Process.Context->SetSyscallHandler(this);
    return Process.Context->InitCore();
  }

  static bool ValidateSnapshotRange(uint64_t Start, size_t Size) {
    const auto End = Start + Size;
    for (auto Address = Start; Address < End;) {
      MemoryInfo Info {};
      u32 Page {};
      const auto Status = svcQueryMemory(&Info, &Page, Address);
      if (R_FAILED(Status) || (Info.perm & Perm_Rw) != Perm_Rw ||
          Address < Info.addr || Address - Info.addr >= Info.size) {
        char Message[176];
        std::snprintf(Message, sizeof(Message), "Fork snapshot range %#llx size=%#zx inaccessible at %#llx perm=%#x result=%#x",
                      static_cast<unsigned long long>(Start), Size, static_cast<unsigned long long>(Address), Info.perm, Status);
        WriteRunCheckpoint(Message);
        return false;
      }
      Address = Info.addr + Info.size;
    }
    return true;
  }

  bool ActivateProcess(uint32_t PID) {
    if (Task->PID != PID) {
      if (!Task->Exited && Task->ForkView.use_count() > 1) {
        if (!Task->Memory.SavePrivatePages(ValidateSnapshotRange)) return false;
        Task->RestorePending = true;
      }
      Task = &Processes.at(PID);
      if (Task->RestorePending) {
        Task->Memory.RestorePrivatePages();
        Task->RestorePending = false;
      }
    }
    if (!Task->HasDispatched) {
      if (Task->Image) ProcessCheckpoint("Replacement image entering dispatch");
      else if (PID == ProcessID) ProcessCheckpoint("Initial image entering dispatch");
      else ProcessCheckpoint("Child private memory restored, entering dispatch");
      Task->HasDispatched = true;
    }
    return true;
  }

  void RetireProcess(uint32_t PID) {
    for (auto& Thread : Threads) {
      if (!Thread.Core || Thread.PID != PID) continue;
      Thread.Core->CTX->DestroyThread(Thread.Core);
      Thread = {};
    }
    auto& Process = Processes.at(PID);
    Process.Files = {};
    Process.Memory = {};
    Process.ForkView.reset();
    Process.InheritedImages.clear();
    Process.Image.reset();
    Process.Context.reset();
    ++IOEpoch;
  }

  int64_t GetProcessGroup(uint32_t PID) const {
    if (!PID) return Task->Group;
    const auto It = Processes.find(PID);
    return It == Processes.end() ? -3 : It->second.Group;
  }

  int64_t SetProcessGroup(uint32_t PID, uint32_t Group) {
    if (!PID) PID = Task->PID;
    if (!Group) Group = PID;
    const auto It = Processes.find(PID);
    if (It == Processes.end() || (PID != Task->PID && It->second.ParentPID != Task->PID)) return -3;
    if (PID != Task->PID && It->second.Image) return -13;
    if (Group != PID && std::none_of(Processes.begin(), Processes.end(),
                                     [&](const auto& Item) { return Item.second.Group == Group; })) return -1;
    It->second.Group = Group;
    return 0;
  }

  struct ForkRequest {
    uint64_t Flags;
    uint64_t ChildTid;
  };

  std::optional<ForkRequest> PendingFork;

  void ProcessCheckpoint(const char* Name) const {
    if (Logging != LogLevel::Trace) return;
    char Message[192];
    std::snprintf(Message, sizeof(Message), "Process checkpoint pid=%u %s", Task->PID, Name);
    WriteRunCheckpoint(Message);
  }

  void LogFirstDispatch(const CoreFrame& Frame) const {
    MemoryInfo Info {};
    u32 Page {};
    const auto Dispatcher = Frame.Pointers.DispatcherLoopTop;
    const auto Status = svcQueryMemory(&Info, &Page, Dispatcher);
    const auto RIP = Frame.State.rip;
    const auto RSP = Frame.State.gregs[FEXCore::X86State::REG_RSP];
    char Message[160];
    std::snprintf(Message, sizeof(Message),
                  "Dispatch rip=%#llx guest-x=%zu rsp=%#llx guest-rw=%zu dispatcher=%#llx perm=%#x result=%#x",
                  static_cast<unsigned long long>(RIP), Available(RIP, PF_X),
                  static_cast<unsigned long long>(RSP), Available(RSP, PF_R | PF_W),
                  static_cast<unsigned long long>(Dispatcher), Info.perm, Status);
    ProcessCheckpoint(Message);
  }

  int64_t RequestFork(uint64_t Flags, uint64_t ChildTid) {
    constexpr uint64_t ChildSetTid = 0x1000000, ChildClearTid = 0x200000;
    if ((Flags & 0xff) != 17 || (Flags & ~(ChildSetTid | ChildClearTid | 0xffULL))) return -38;
    if ((Flags & (ChildSetTid | ChildClearTid)) && Available(ChildTid, PF_W) < sizeof(uint32_t)) return Fault;
    if (Processes.size() >= Threads.size()) return -11;
    PendingFork = ForkRequest {Flags, ChildTid};
    return RetrySyscall;
  }

  void CompleteFork() {
    const auto [Flags, ChildTid] = *PendingFork;
    PendingFork.reset();
    auto* Frame = Current->Core->CurrentFrame;
    Frame->State.gregs[FEXCore::X86State::REG_RAX] = ForkGuest(Frame, Flags, ChildTid);
    Frame->State.rip += 2;
  }

  int64_t ForkGuest(CoreFrame* Frame, uint64_t Flags, uint64_t ChildTid) {
    constexpr uint64_t ChildSetTid = 0x1000000, ChildClearTid = 0x200000;
    ProcessCheckpoint("Fork request, saving parent memory");
    if (!Task->Memory.SavePrivatePages(ValidateSnapshotRange)) return -11;
    ProcessCheckpoint("Fork request, parent memory saved");

    const auto PID = NextTid++;
    auto& Child = Processes.try_emplace(PID).first->second;
    Child.PID = PID;
    Child.ParentPID = Task->PID;
    Child.Group = Task->Group;
    Child.Folder = Task->Folder;
    Child.WorkingDirectory = Task->WorkingDirectory;
    Child.Memory = Task->Memory;
    Child.GDT = Task->GDT;
    Child.ForkView = Task->ForkView;
    Child.RestorePending = true;
    Child.Files = Task->Files;
    Child.DescriptorFlags = Task->DescriptorFlags;
    Child.SignalActions = Task->SignalActions;
    Child.HeapStart = Task->HeapStart;
    Child.HeapEnd = Task->HeapEnd;
    // Keep the source process's ELF backing alive even if it execs or exits.
    Child.InheritedImages = Task->InheritedImages;
    if (Task->Image) Child.InheritedImages.push_back(Task->Image);
    FEXCore::Core::CPUState State {};
    std::memcpy(&State, &Frame->State, sizeof(State));
    State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = Child.GDT.data();
    State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = Child.GDT.data();
    State.rip += 2;
    State.gregs[FEXCore::X86State::REG_RAX] = 0;
    ProcessCheckpoint("Fork request, creating child FEX context");
    if (!InitializeProcessContext(Child) ||
        !AddThread(*Child.Context, State, PID, Flags & ChildClearTid ? ChildTid : 0, PID)) {
      Processes.erase(PID);
      return -11;
    }
    const auto Thread = std::find_if(Threads.begin(), Threads.end(), [PID](const auto& Entry) { return Entry.TID == PID; });
    if (Flags & ChildSetTid) Thread->SetTidOnStart = ChildTid;
    Child.TraceFirstDispatch = true;
    Child.StartupSyscallCheckpoints = Logging == LogLevel::Trace ? 6 : 0;
    ProcessCheckpoint("Fork request, child ready");
    return PID;
  }

  int64_t WaitChild(int32_t PID, uint64_t Status, uint32_t Flags, uint64_t Usage) {
    if (Flags & ~(1u | 2u | 8u)) return Invalid;
    if (Status && Available(Status, PF_W) < 4) return Fault;
    if (Usage && Available(Usage, PF_W) < 144) return Fault;
    bool HasChild {};
    for (auto It = Processes.begin(); It != Processes.end(); ++It) {
      const auto& Child = It->second;
      if (Child.ParentPID != Task->PID) continue;
      if (PID > 0 && Child.PID != static_cast<uint32_t>(PID)) continue;
      if (PID == 0 && Child.Group != Task->Group) continue;
      if (PID < -1 && Child.Group != static_cast<uint32_t>(-static_cast<int64_t>(PID))) continue;
      HasChild = true;
      if (!Child.Exited) continue;
      if (Status) CopyOut(Status, Child.Status << 8);
      if (Usage) {
        std::array<uint64_t, 18> Accounting {};
        const auto Microseconds = armTicksToNs(Child.CpuTicks) / 1000;
        Accounting[0] = Microseconds / 1'000'000;
        Accounting[1] = Microseconds % 1'000'000;
        CopyOut(Usage, Accounting);
      }
      const auto Reaped = Child.PID;
      Processes.erase(It);
      return Reaped;
    }
    if (!HasChild) return -10;
    if (Flags & 1) return 0;
    return WaitForIO();
  }

  int64_t CopyExecVector(uint64_t Address, std::vector<std::string>& Strings, size_t& Budget) {
    if (!Address) return 0;
    for (size_t Index = 0; Index < 256; ++Index) {
      const uint64_t Slot = Address + Index * sizeof(uint64_t);
      uint64_t StringAddress {};
      if (Available(Slot, PF_R) < sizeof(StringAddress)) return Fault;
      std::memcpy(&StringAddress, reinterpret_cast<const void*>(Slot), sizeof(StringAddress));
      if (!StringAddress) return 0;
      const auto Text = GuestString(StringAddress);
      if (!Text) return Fault;
      if (Text->size() + 1 > Budget) return -7;
      Budget -= Text->size() + 1;
      Strings.emplace_back(*Text);
    }
    return -7;
  }

  int64_t ExecGuest(uint64_t PathAddress, uint64_t Argv, uint64_t Envp) {
    const auto Name = GuestString(PathAddress);
    if (!Name) return Fault;
    const auto Path = ResolveGuestPath(CurrentDirectory, *Name);
    if (!Path) return -2;
    LinuxStat Info {};
    if (const auto Status = PathStat(CurrentDirectory, *Name, Info); Status < 0) return Status;
    if ((Info.Mode & 0170000u) != 0100000u || !(Info.Mode & 0111)) return -13;

    ExecImage Request;
    Request.Path = *Name;
    size_t Budget = 128 * 1024;
    if (const auto Result = CopyExecVector(Argv, Request.Arguments, Budget); Result < 0) return Result;
    if (const auto Result = CopyExecVector(Envp, Request.Environment, Budget); Result < 0) return Result;
    if (ReadElf(Path->c_str(), Request.Program)) return -8;
    if (!Request.Program.Interpreter.empty()) {
      const auto Interpreter = ResolvePath(Request.Program.Interpreter, "/", RootPath);
      if (!Interpreter || ReadElf(Interpreter->c_str(), Request.Interpreter)) return -2;
      if (!Request.Interpreter.Interpreter.empty()) return -8;
    }
    Task->PendingExec = std::move(Request);
    return 0;
  }

  bool InstallExec() {
    const auto PID = Task->PID;
    auto* OldThread = Current->Core;
    FEXCore::Core::CPUState State {};
    const auto Mask = Current->SignalMask;

    // The dispatcher is process infrastructure. Keep its native mapping
    // published across exec, but clear all translations for the old image.
    OldThread->CTX->ClearCodeCache(OldThread, false);
    for (auto& Thread : Threads) {
      if (!Thread.Core || Thread.PID != PID) continue;
      Thread.Core->CTX->DestroyThread(Thread.Core);
      Thread = {};
    }
    Current = nullptr;
    InitializeExecCPU(*Task, State);
    Task->Memory = {};
    Task->ForkView = std::make_shared<const uint8_t>(0);
    Task->RestorePending = false;
    Task->HeapStart = Task->HeapEnd = 0;
    Task->Image = std::make_shared<ExecImage>(std::move(*Task->PendingExec));
    Task->PendingExec.reset();
    Task->InheritedImages.clear();
    for (size_t FD = 0; FD < Task->Files.size(); ++FD) {
      if (Task->DescriptorFlags[FD]) CloseGuest(FD);
    }
    for (auto& Action : Task->SignalActions) {
      if (Action[0] != 1) Action = {};
    }
    ++IOEpoch;

    const auto& Image = *Task->Image;
    for (const auto* ELF : {&Image.Program, &Image.Interpreter}) {
      for (const auto& Segment : ELF->GetSegments()) {
        Task->Memory.Register(Segment.Start, Segment.Size, Segment.Flags);
        RecordMapping(Segment.Start, Segment.Size, Segment.Start - ELF->Base,
                      ELF == &Image.Program ? Image.Path : Image.Program.Interpreter);
      }
    }
    State.rip = Image.Interpreter.Entry ? Image.Interpreter.Entry : Image.Program.Entry;
    const auto Base = CreateStack(MainStackSize);
    if (!Base) return false;
    State.gregs[FEXCore::X86State::REG_RSP] =
      BuildInitialStack(Base, Image.Path, Image.Arguments, Image.Environment, &Image.Program, Image.Interpreter.Base);
    if (!AddThread(*Task->Context, State, PID, 0, PID)) return false;
    const auto Thread = std::find_if(Threads.begin(), Threads.end(), [PID](const auto& Entry) { return Entry.TID == PID; });
    Thread->SignalMask = Mask;
    Task->HasDispatched = false;
    Task->TraceFirstDispatch = true;
    Task->StartupSyscallCheckpoints = Logging == LogLevel::Trace ? 12 : 0;
    WriteLog(("Guest exec " + Image.Path).c_str());
    return true;
  }

  struct LinuxStat {
    uint64_t Device, Inode, Links;
    uint32_t Mode, Uid, Gid, Pad;
    uint64_t Rdev;
    int64_t Size, BlockSize, Blocks;
    uint64_t Times[6];
    int64_t Reserved[3];
  };
  static_assert(sizeof(LinuxStat) == 144);

  static constexpr uint64_t CurrentDirectory = static_cast<uint64_t>(-100);
  static bool IsCwd(uint64_t FD) { return FD == CurrentDirectory || FD == static_cast<uint32_t>(CurrentDirectory); }
  OpenFile* GetFile(uint64_t FD) {
    return FD < Task->Files.size() ? Task->Files[FD].get() : nullptr;
  }

  size_t Available(uint64_t Address, uint32_t Flags) const {
    const auto Range = Task->Memory.Query(Address);
    return (Range.Flags & Flags) == Flags && Range.Size ? Range.Size - (Address - Range.Start) : 0;
  }

  std::optional<std::string_view> GuestString(uint64_t Address) const {
    const auto* Data = reinterpret_cast<const char*>(Address);
    const size_t Limit = std::min<size_t>(Available(Address, PF_R), FS_MAX_PATH);
    if (!Limit) return std::nullopt;
    const auto* End = static_cast<const char*>(std::memchr(Data, 0, Limit));
    if (!End) return std::nullopt;
    return std::string_view {Data, static_cast<size_t>(End - Data)};
  }

  static uint64_t FileInode(std::string_view Path) {
    uint64_t Hash = 14695981039346656037ULL;
    for (const auto Byte : Path) {
      Hash = (Hash ^ static_cast<uint8_t>(Byte)) * 1099511628211ULL;
    }
    return Hash;
  }

  static LinuxStat MakeStat(uint32_t Mode, int64_t Size, uint64_t Inode) {
    LinuxStat Info {};
    Info.Device = 1;
    Info.Inode = Inode;
    Info.Links = 1;
    Info.Mode = Mode;
    Info.Size = Size;
    Info.BlockSize = GuestPageSize;
    Info.Blocks = (Size + 511) / 512;
    return Info;
  }

  int64_t FileStat(uint64_t FD, LinuxStat& Info) {
    if (auto* File = GetFile(FD)) {
      s64 Size = File->Contents.size();
      if (File->RealFile && R_FAILED(fsFileGetSize(&File->Handle, &Size))) return -5;
      const auto Mode = File->Directory ? 0040555 : File->Pipe ? 0010600 : File->Socket ? 0140777 :
                        File->Console != ConsoleStream::None ? 0020600 : File->Path == "/dev/null" ? 0020666 : RegularMode(File->Path);
      Info = MakeStat(Mode, Size, File->Inode);
      if (File->Console == ConsoleStream::Terminal) Info.Rdev = 0x500;
    } else {
      return BadFile;
    }
    return 0;
  }

  int64_t StatGuest(uint64_t FD, uint64_t Output) {
    if (sizeof(LinuxStat) > Available(Output, PF_W)) return Fault;
    LinuxStat Info {};
    const auto Result = FileStat(FD, Info);
    if (!Result) std::memcpy(reinterpret_cast<void*>(Output), &Info, sizeof(Info));
    return Result;
  }

  int64_t StatAtGuest(uint64_t FD, uint64_t Address, uint64_t Output, uint64_t Flags) {
    if (sizeof(LinuxStat) > Available(Output, PF_W)) return Fault;
    LinuxStat Info {};
    const auto Result = GetStatAt(FD, Address, Flags, Info);
    if (!Result) std::memcpy(reinterpret_cast<void*>(Output), &Info, sizeof(Info));
    return Result;
  }

  int64_t AccessGuest(uint64_t FD, uint64_t Address, uint64_t Mode, uint64_t Flags) {
    constexpr uint64_t EffectiveAccess = 0x200;
    constexpr uint64_t SupportedFlags = 0x100 | EffectiveAccess | 0x1000;
    if ((Mode & ~7ULL) || (Flags & ~SupportedFlags)) return Invalid;
    // The guest's real and effective credentials are identical.
    LinuxStat Info {};
    const auto Result = GetStatAt(FD, Address, Flags & ~EffectiveAccess, Info);
    if (Result < 0) return Result;
    return (Mode & (Info.Mode & 7)) == Mode ? 0 : -13;
  }

  std::optional<std::string> ResolveGuestPath(uint64_t Directory, std::string_view Path) {
    if (Path.starts_with('/')) return Switch2Guest::ResolvePath(Path, "/", RootPath);
    auto* File = IsCwd(Directory) ? nullptr : GetFile(Directory);
    if (!IsCwd(Directory) && (!File || !File->Directory)) return std::nullopt;
    const auto& Base = File ? File->Path : Task->Folder;
    if (Base == RootPath || (Base.starts_with(RootPath) && Base[RootPath.size()] == '/')) {
      const std::string GuestPath = Base.substr(RootPath.size()) + "/" + std::string(Path);
      return Switch2Guest::ResolvePath(GuestPath, "/", RootPath);
    }
    return Switch2Guest::ResolvePath(Path, Base, RootPath);
  }

  int64_t PathStat(uint64_t Directory, std::string_view Name, LinuxStat& Info) {
    if (!Name.starts_with('/') && !IsCwd(Directory)) {
      const auto* File = GetFile(Directory);
      if (!File) return BadFile;
      if (!File->Directory) return -20;
    }
    if (Name.empty()) return -2;
    if (Name == "/dev/null" || (Interactive && Name == "/dev/tty")) {
      Info = MakeStat(0020666, 0, FileInode(Name));
      Info.Rdev = Name == "/dev/null" ? 0x103 : 0x500;
      return 0;
    }
    const auto Path = ResolveGuestPath(Directory, Name);
    if (!Path || Path->size() >= FS_MAX_PATH) return Invalid;
    FsFileSystem SdCard {};
    if (R_FAILED(fsOpenSdCardFileSystem(&SdCard))) return -5;
    FsDirEntryType Type {};
    const Result Status = fsFsGetEntryType(&SdCard, Path->c_str(), &Type);
    s64 Size {};
    bool Readable = R_SUCCEEDED(Status);
    if (Readable && Type == FsDirEntryType_File) {
      FsFile File {};
      Readable = R_SUCCEEDED(fsFsOpenFile(&SdCard, Path->c_str(), FsOpenMode_Read, &File));
      if (Readable) {
        Readable = R_SUCCEEDED(fsFileGetSize(&File, &Size));
        fsFileClose(&File);
      }
    }
    fsFsClose(&SdCard);
    if (!Readable) return -2;
    if (Name.ends_with('/') && Type != FsDirEntryType_Dir) return -20;
    Info = MakeStat(Type == FsDirEntryType_Dir ? 0040555 : RegularMode(*Path), Size, FileInode(*Path));
    return 0;
  }

  uint32_t RegularMode(std::string_view Path) const {
    const auto Relative = Path.starts_with(RootPath) ? Path.substr(RootPath.size()) : Path;
    return Relative.starts_with("/bin/") || Relative.starts_with("/usr/bin/") ? 0100555 : 0100444;
  }

  int64_t GetStatAt(uint64_t FD, uint64_t Address, uint64_t Flags, LinuxStat& Info) {
    if (Flags & ~0x1900ULL) return Invalid;
    const auto Name = GuestString(Address);
    if (!Name) return Fault;
    if (Name->empty() && (Flags & 0x1000)) {
      return IsCwd(FD) ? PathStat(FD, ".", Info) : FileStat(FD, Info);
    }
    return PathStat(FD, *Name, Info);
  }

  int64_t StatxGuest(uint64_t FD, uint64_t Address, uint64_t Flags, uint64_t Output) {
    // x86-64 Linux statx, including the fixed tail reserved for newer kernels.
    struct Timestamp { int64_t Seconds; uint32_t Nanoseconds, Reserved; };
    struct Statx {
      uint32_t Mask, BlockSize;
      uint64_t Attributes;
      uint32_t Links, Uid, Gid;
      uint16_t Mode, Padding;
      uint64_t Inode, Size, Blocks, AttributesMask;
      Timestamp Access, Birth, Change, Modify;
      uint32_t RdevMajor, RdevMinor, DevMajor, DevMinor;
      uint64_t Reserved[14];
    };
    static_assert(sizeof(Statx) == 256);
    if (Available(Output, PF_W) < sizeof(Statx)) return Fault;
    LinuxStat Info {};
    const auto Result = GetStatAt(FD, Address, Flags & ~0x6000ULL, Info);
    if (Result < 0) return Result;
    Statx Out {};
    Out.Mask = 0x7ff;
    Out.BlockSize = Info.BlockSize;
    Out.Links = Info.Links;
    Out.Uid = Info.Uid;
    Out.Gid = Info.Gid;
    Out.Mode = Info.Mode;
    Out.Inode = Info.Inode;
    Out.Size = Info.Size;
    Out.Blocks = Info.Blocks;
    Out.RdevMajor = Info.Rdev >> 8;
    Out.RdevMinor = Info.Rdev & 255;
    Out.DevMinor = Info.Device;
    std::memcpy(reinterpret_cast<void*>(Output), &Out, sizeof(Out));
    return 0;
  }

  int64_t ReadDirents(uint64_t FD, uint64_t Output, uint64_t Count) {
    auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (!File->Directory) return -20;
    Count = std::min<uint64_t>(Count, INT32_MAX);
    if (Count > Available(Output, PF_W)) return Fault;
    size_t Used {};
    auto* Bytes = reinterpret_cast<char*>(Output);
    const auto& Entries = *File->Directory;
    while (static_cast<size_t>(File->Offset) < Entries.size()) {
      const auto& Entry = Entries[File->Offset];
      constexpr size_t HeaderSize = 19;
      const uint16_t Length = (HeaderSize + Entry.Name.size() + 1 + 7) & ~7;
      if (Length > Count - Used) return Used ? static_cast<int64_t>(Used) : Invalid;
      const auto Path = Switch2Guest::ResolvePath(Entry.Name, File->Path, RootPath);
      const auto Inode = FileInode(Path ? std::string_view(*Path) : std::string_view(File->Path));
      const uint64_t Next = ++File->Offset;
      auto* EntryBytes = Bytes + Used;
      std::memset(EntryBytes, 0, Length);
      std::memcpy(EntryBytes, &Inode, 8);
      std::memcpy(EntryBytes + 8, &Next, 8);
      std::memcpy(EntryBytes + 16, &Length, 2);
      EntryBytes[18] = Entry.IsDirectory ? 4 : 8;
      std::memcpy(EntryBytes + HeaderSize, Entry.Name.data(), Entry.Name.size());
      Used += Length;
    }
    return Used;
  }

  int64_t SeekGuest(uint64_t FD, int64_t Offset, uint64_t Origin) {
    auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (File->Socket || File->Console != ConsoleStream::None) return -29;
    int64_t Base {};
    if (Origin == 1) Base = File->Offset;
    else if (Origin == 2) {
      if (File->Directory) Base = File->Directory->size();
      else {
        LinuxStat Info {};
        const auto Result = FileStat(FD, Info);
        if (Result < 0) return Result;
        Base = Info.Size;
      }
    } else if (Origin != 0) return Invalid;
    int64_t Next {};
    if (__builtin_add_overflow(Base, Offset, &Next) || Next < 0) return Invalid;
    File->Offset = Next;
    return Next;
  }

  int64_t GetCwd(uint64_t Output, uint64_t Size) {
    const auto Bytes = Task->WorkingDirectory.size() + 1;
    if (Size < Bytes) return -34;
    if (Available(Output, PF_W) < Bytes) return Fault;
    std::memcpy(reinterpret_cast<void*>(Output), Task->WorkingDirectory.c_str(), Bytes);
    return Bytes;
  }

  int64_t ChangeDirectory(uint64_t Address) {
    const auto Name = GuestString(Address);
    if (!Name) return Fault;
    LinuxStat Info {};
    if (const auto Result = PathStat(CurrentDirectory, *Name, Info); Result < 0) return Result;
    if ((Info.Mode & 0170000) != 0040000) return -20;
    Task->Folder = *ResolveGuestPath(CurrentDirectory, *Name);
    if (Task->Folder == RootPath) Task->WorkingDirectory = "/";
    else if (Task->Folder.starts_with(RootPath) && Task->Folder[RootPath.size()] == '/') Task->WorkingDirectory = Task->Folder.substr(RootPath.size());
    else Task->WorkingDirectory = Task->Folder;
    return 0;
  }

  int64_t DuplicateDescriptor(uint64_t Old, uint64_t New, bool CloseOnExec) {
    if (!GetFile(Old) || New >= Task->Files.size()) return BadFile;
    if (Old == New) return New;
    if (GetFile(New)) CloseGuest(New);
    Task->Files[New] = Task->Files[Old];
    Task->DescriptorFlags[New] = CloseOnExec;
    return New;
  }

  int64_t FileControl(uint64_t FD, uint64_t Command, uint64_t Arg) {
    auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (Command == 1) return Task->DescriptorFlags[FD];
    if (Command == 2) {
      Task->DescriptorFlags[FD] = Arg & 1;
      return 0;
    }
    if (Command == 3) return File->StatusFlags;
    if (Command == 4) {
      File->StatusFlags = (File->StatusFlags & ~04000u) | (Arg & 04000);
      if (File->Socket) File->Socket->Nonblocking = Arg & 04000;
      return 0;
    }
    if (Command == 0 || Command == 1030) {
      if (Arg >= Task->Files.size()) return Invalid;
      const auto NewFD = FreeDescriptor(Arg);
      if (NewFD == NoDescriptor) return -24;
      Task->Files[NewFD] = Task->Files[FD];
      Task->DescriptorFlags[NewFD] = Command == 1030;
      return NewFD;
    }
    return Invalid;
  }

  uint64_t FreeDescriptor(uint64_t Start = 0) const {
    for (uint64_t FD = Start; FD < Task->Files.size(); ++FD) {
      if (!Task->Files[FD]) return FD;
    }
    return NoDescriptor;
  }

  using StreamSocket = Switch2Guest::StreamSocket;
  static constexpr uint32_t SocketFlags = 04000 | 02000000;

  static std::shared_ptr<OpenFile> SocketFile(uint64_t Flags) {
    auto File = std::make_shared<OpenFile>();
    File->Socket = std::make_shared<StreamSocket>();
    SetSocketFlags(*File, Flags);
    return File;
  }

  static void SetSocketFlags(OpenFile& File, uint64_t Flags) {
    File.Socket->Nonblocking = Flags & 04000;
    File.StatusFlags = 2 | (Flags & 04000);
  }

  void InstallSocket(uint64_t FD, std::shared_ptr<OpenFile> File, uint64_t Flags) {
    File->Inode = NextSocketInode++;
    Task->Files[FD] = std::move(File);
    Task->DescriptorFlags[FD] = (Flags & 02000000) != 0;
  }

  static int64_t CheckSocket(uint64_t Domain, uint64_t Type, uint64_t Protocol) {
    if (Domain != 1) return -97;
    if ((Type & ~SocketFlags) != 1) return -94;
    return Protocol ? -93 : 0;
  }

  int64_t CreateSocket(uint64_t Domain, uint64_t Type, uint64_t Protocol) {
    if (const auto Result = CheckSocket(Domain, Type, Protocol); Result) return Result;
    const auto FD = FreeDescriptor();
    if (FD == NoDescriptor) return -24;
    InstallSocket(FD, SocketFile(Type), Type);
    return FD;
  }

  int64_t CreatePipe(uint64_t Output, uint64_t Flags) {
    if (Flags & ~SocketFlags) return Invalid;
    if (Available(Output, PF_W) < 8) return Fault;
    const auto ReadFD = FreeDescriptor();
    if (ReadFD == NoDescriptor) return -24;
    const auto WriteFD = FreeDescriptor(ReadFD + 1);
    if (WriteFD == NoDescriptor) return -24;

    auto Reader = SocketFile(Flags);
    auto Writer = SocketFile(Flags);
    Reader->Pipe = Writer->Pipe = true;
    Reader->StatusFlags = Flags & 04000;
    Writer->StatusFlags = Reader->StatusFlags | 1;
    Reader->Socket->Connect(Writer->Socket);
    Writer->Socket->Connect(Reader->Socket);
    const auto Inode = NextSocketInode;
    InstallSocket(ReadFD, std::move(Reader), Flags);
    InstallSocket(WriteFD, std::move(Writer), Flags);
    GetFile(WriteFD)->Inode = Inode;
    const uint32_t FDs[] {static_cast<uint32_t>(ReadFD), static_cast<uint32_t>(WriteFD)};
    return CopyOut(Output, FDs);
  }

  int64_t SocketPair(uint64_t Domain, uint64_t Type, uint64_t Protocol, uint64_t Output) {
    if (const auto Result = CheckSocket(Domain, Type, Protocol); Result) return Result;
    if (Available(Output, PF_W) < 8) return Fault;
    const auto First = FreeDescriptor();
    if (First == NoDescriptor) return -24;
    const auto Second = FreeDescriptor(First + 1);
    if (Second == NoDescriptor) return -24;
    auto A = SocketFile(Type);
    auto B = SocketFile(Type);
    A->Socket->Connect(B->Socket);
    B->Socket->Connect(A->Socket);
    InstallSocket(First, std::move(A), Type);
    InstallSocket(Second, std::move(B), Type);
    const uint32_t FDs[] {static_cast<uint32_t>(First), static_cast<uint32_t>(Second)};
    std::memcpy(reinterpret_cast<void*>(Output), FDs, sizeof(FDs));
    return 0;
  }

  int64_t SocketName(uint64_t Address, uint64_t Length, std::string& Name) {
    if (Length <= 2 || Length > 110) return Invalid;
    if (Available(Address, PF_R) < Length) return Fault;
    const auto* Bytes = reinterpret_cast<const char*>(Address);
    uint16_t Family {};
    std::memcpy(&Family, Bytes, sizeof(Family));
    if (Family != 1) return -97;
    const auto* Path = Bytes + 2;
    Name.assign(Path, Path[0] ? strnlen(Path, Length - 2) : Length - 2);
    return 0;
  }

  int64_t BindSocket(uint64_t FD, uint64_t Address, uint64_t Length) {
    auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (!File->Socket || File->Pipe) return -88;
    if (File->Socket->Connected || File->Socket->Listening || File->Socket->Bound) return Invalid;
    std::string Name;
    if (const auto Result = SocketName(Address, Length, Name); Result) return Result;
    auto& Binding = SocketNames[Name];
    if (!Binding.expired()) return -98;
    Binding = File->Socket;
    File->Socket->Bound = true;
    return 0;
  }

  int64_t ListenSocket(uint64_t FD, int32_t Backlog) {
    auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (!File->Socket || File->Pipe) return -88;
    if (!File->Socket->Bound || File->Socket->Connected) return Invalid;
    File->Socket->Listening = true;
    File->Socket->Backlog = std::clamp(Backlog, 1, 16);
    return 0;
  }

  int64_t ConnectSocket(uint64_t FD, uint64_t Address, uint64_t Length) {
    auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (!File->Socket || File->Pipe) return -88;
    if (File->Socket->Connected) return -106;
    if (File->Socket->Listening) return Invalid;
    std::string Name;
    if (const auto Result = SocketName(Address, Length, Name); Result) return Result;
    const auto Entry = SocketNames.find(Name);
    auto Listener = Entry == SocketNames.end() ? nullptr : Entry->second.lock();
    if (!Listener) return -2;
    if (!Listener->Listening) return -111;
    if (Listener->Pending.size() >= Listener->Backlog) {
      return File->Socket->Nonblocking ? -11 : WaitForIO();
    }
    auto Connection = std::make_shared<StreamSocket>();
    Connection->Connect(File->Socket);
    File->Socket->Connect(Connection);
    Listener->Pending.push_back(std::move(Connection));
    ++IOEpoch;
    return 0;
  }

  int64_t CheckSocketAddress(uint64_t Address, uint64_t Length) {
    if (!Address) return 0;
    if (Available(Length, PF_R | PF_W) < 4) return Fault;
    uint32_t Space {};
    std::memcpy(&Space, reinterpret_cast<const void*>(Length), 4);
    return Available(Address, PF_W) >= std::min(Space, 2u) ? 0 : Fault;
  }

  static void WriteUnnamedAddress(uint64_t Address, uint64_t Length) {
    if (!Address) return;
    uint32_t Space {};
    std::memcpy(&Space, reinterpret_cast<const void*>(Length), 4);
    const uint16_t Family = 1;
    std::memcpy(reinterpret_cast<void*>(Address), &Family, std::min(Space, 2u));
    Space = 2;
    std::memcpy(reinterpret_cast<void*>(Length), &Space, 4);
  }

  int64_t AcceptSocket(uint64_t FD, uint64_t Address, uint64_t Length, uint64_t Flags) {
    if (Flags & ~SocketFlags) return Invalid;
    auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (!File->Socket || File->Pipe) return -88;
    if (!File->Socket->Listening) return Invalid;
    if (const auto Result = CheckSocketAddress(Address, Length); Result) return Result;
    if (File->Socket->Pending.empty()) return File->Socket->Nonblocking ? -11 : WaitForIO();
    const auto NewFD = FreeDescriptor();
    if (NewFD == NoDescriptor) return -24;
    auto Connection = std::make_shared<OpenFile>();
    Connection->Socket = std::move(File->Socket->Pending.front());
    File->Socket->Pending.pop_front();
    SetSocketFlags(*Connection, Flags);
    InstallSocket(NewFD, std::move(Connection), Flags);
    WriteUnnamedAddress(Address, Length);
    ++IOEpoch;
    return NewFD;
  }

  int64_t ReadSocket(StreamSocket& Socket, char* Buffer, uint64_t Count, bool DontWait, bool Peek = false) {
    if (!Socket.Connected) return -107;
    if (Socket.ShutRead) return 0;
    if (Socket.Input.empty()) {
      if (Socket.EndOfStream()) return 0;
      return Socket.Nonblocking || DontWait ? -11 : WaitForIO();
    }
    const auto Bytes = std::min<uint64_t>(Count, Socket.Input.size());
    std::copy_n(Socket.Input.begin(), Bytes, Buffer);
    if (!Peek) {
      Socket.Input.erase(Socket.Input.begin(), Socket.Input.begin() + Bytes);
      ++IOEpoch;
    }
    return Bytes;
  }

  int64_t WriteSocket(StreamSocket& Socket, const char* Buffer, uint64_t Count, bool DontWait, bool Atomic = false) {
    if (!Socket.Connected) return -107;
    auto Peer = Socket.Peer.lock();
    if (!Peer || Socket.ShutWrite || Peer->ShutRead) return -32;
    const auto Bytes = std::min<uint64_t>(Count, StreamSocket::Capacity - Peer->Input.size());
    if (!Bytes || (Atomic && Bytes < Count)) return Socket.Nonblocking || DontWait ? -11 : WaitForIO();
    Peer->Input.insert(Peer->Input.end(), Buffer, Buffer + Bytes);
    ++IOEpoch;
    return Bytes;
  }

  int64_t SocketTransfer(bool Write, uint64_t FD, uint64_t Address, uint64_t Count, uint64_t Flags,
                         uint64_t PeerAddress, uint64_t PeerLength) {
    if (Flags & ~(Write ? 0x4040ULL : 0x42ULL)) return -95;
    auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (!File->Socket || File->Pipe) return -88;
    if (!Write) {
      if (const auto Result = CheckSocketAddress(PeerAddress, PeerLength); Result) return Result;
    } else if (PeerAddress) {
      return -106;
    }
    Count = std::min<uint64_t>(Count, StreamSocket::Capacity);
    if (!Count) return 0;
    if (Available(Address, Write ? PF_R : PF_W) < Count) return Fault;
    auto* Buffer = reinterpret_cast<char*>(Address);
    if (Write) return WriteSocket(*File->Socket, Buffer, Count, Flags & 0x40);
    const auto Result = ReadSocket(*File->Socket, Buffer, Count, Flags & 0x40, Flags & 2);
    if (Result >= 0) WriteUnnamedAddress(PeerAddress, PeerLength);
    return Result;
  }

  int64_t ShutdownSocket(uint64_t FD, uint64_t How) {
    auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (!File->Socket || File->Pipe) return -88;
    if (!File->Socket->Connected) return -107;
    if (How > 2) return Invalid;
    if (How != 1) {
      File->Socket->ShutRead = true;
      File->Socket->Input.clear();
    }
    if (How != 0) File->Socket->ShutWrite = true;
    ++IOEpoch;
    return 0;
  }

  int64_t PollGuest(uint64_t Address, uint64_t Count, int32_t Milliseconds) {
    struct PollFD { int32_t FD; int16_t Events, Revents; };
    if (Count > Task->Files.size()) return Invalid;
    if (Count && Available(Address, PF_R | PF_W) < Count * sizeof(PollFD)) return Fault;
    int64_t ReadyCount {};
    for (size_t Index = 0; Index < Count; ++Index) {
      auto* Position = reinterpret_cast<void*>(Address + Index * sizeof(PollFD));
      PollFD Entry {};
      std::memcpy(&Entry, Position, sizeof(Entry));
      Entry.Revents = 0;
      if (Entry.FD >= 0) {
        const auto* File = GetFile(Entry.FD);
        if (File && File->Socket) Entry.Revents = StreamPoll(*File, Entry.Events);
        else if (!File) Entry.Revents = 0x20;
        else if (File->Console != ConsoleStream::Input) {
          Entry.Revents = Entry.Events & (File->Console == ConsoleStream::None ? 5 : 4);
        }
      }
      ReadyCount += Entry.Revents != 0;
      std::memcpy(Position, &Entry, sizeof(Entry));
    }
    if (ReadyCount || !Milliseconds) return ReadyCount;
    if (Milliseconds > 0) {
      const auto Now = MonotonicNs();
      if (!Current->Deadline) Current->Deadline = AddDeadline(Now, Milliseconds * 1'000'000ULL);
      if (Current->Deadline <= Now) return 0;
    }
    return WaitForIO();
  }

  template<typename T>
  int64_t CopyOut(uint64_t Address, const T& Value) {
    if (Available(Address, PF_W) < sizeof(Value)) return Fault;
    std::memcpy(reinterpret_cast<void*>(Address), &Value, sizeof(Value));
    return 0;
  }

  int64_t SignalAction(uint64_t Number, uint64_t Input, uint64_t Output, uint64_t Size) {
    if (Size != 8 || !Number || Number > Task->SignalActions.size() || (Input && (Number == 9 || Number == 19))) return Invalid;
    auto& Action = Task->SignalActions[Number - 1];
    auto New = Action;
    if (Input) {
      if (Available(Input, PF_R) < sizeof(New)) return Fault;
      std::memcpy(New.data(), reinterpret_cast<const void*>(Input), sizeof(New));
    }
    if (Output && CopyOut(Output, Action)) return Fault;
    Action = New;
    return 0;
  }

  int64_t SignalMask(uint64_t How, uint64_t Input, uint64_t Output, uint64_t Size) {
    if (Size != 8 || (Input && How > 2)) return Invalid;
    auto& Mask = Current->SignalMask;
    uint64_t New {};
    if (Input) {
      if (Available(Input, PF_R) < sizeof(New)) return Fault;
      std::memcpy(&New, reinterpret_cast<const void*>(Input), sizeof(New));
      if (How == 0) New |= Mask;
      if (How == 1) New = Mask & ~New;
      New &= ~((1ULL << 8) | (1ULL << 18));
    }
    if (Output && CopyOut(Output, Mask)) return Fault;
    if (Input) Mask = New;
    return 0;
  }

  int64_t AltStack(uint64_t Input, uint64_t Output) {
    auto& Stack = Current->AltStack;
    auto New = Stack;
    if (Input) {
      if (Available(Input, PF_R) < sizeof(New)) return Fault;
      std::memcpy(New.data(), reinterpret_cast<const void*>(Input), sizeof(New));
      New[1] &= UINT32_MAX;
      if (New[1] != 0 && New[1] != 2) return Invalid;
      if (New[1] == 0 && New[2] < 2048) return -12;
      if (New[1] == 2) New = {0, 2, 0};
    }
    if (Output && CopyOut(Output, Stack)) return Fault;
    Stack = New;
    return 0;
  }

  int64_t UnameGuest(uint64_t Address) {
    const char Names[6][65] {"Linux", "fex-switch2", "5.15.0", "#1 FEX Switch 2", "x86_64", ""};
    return CopyOut(Address, Names);
  }

  int64_t LimitGuest(uint64_t Resource, uint64_t Output, uint64_t Input) {
    if (Resource > 15) return Invalid;
    if (Input) return -1;
    if (!Output) return 0;
    constexpr uint64_t Unlimited = UINT64_MAX;
    uint64_t Limits[2] {Unlimited, Unlimited};
    if (Resource == 3) Limits[0] = MainStackSize;
    if (Resource == 7) Limits[0] = Limits[1] = Task->Files.size();
    return CopyOut(Output, Limits);
  }

  int64_t SystemInfo(uint64_t Address) {
    // Linux x86-64 sysinfo has 64-bit counts and a fixed 112-byte layout.
    struct Info {
      int64_t Uptime;
      uint64_t Loads[3], TotalRAM, FreeRAM, SharedRAM, BufferRAM, TotalSwap, FreeSwap;
      uint16_t Processes, Padding;
      uint64_t TotalHigh, FreeHigh;
      uint32_t MemUnit;
    };
    static_assert(sizeof(Info) == 112);
    Info Data {};
    Data.Uptime = MonotonicNs() / 1'000'000'000;
    Data.TotalRAM = MemoryLimit;
    Data.Processes = 1;
    Data.MemUnit = 1;
    return CopyOut(Address, Data);
  }

  int64_t GetAffinity(uint64_t Tid, uint64_t Size, uint64_t Address) {
    if (Tid && !std::any_of(Threads.begin(), Threads.end(), [&](const auto& Thread) { return Thread.Core && Thread.TID == Tid; })) return -3;
    if (Size < 8) return Invalid;
    const auto Result = CopyOut(Address, uint64_t {1});
    return Result ? Result : 8;
  }

  int64_t ClockGuest(uint64_t Clock, uint64_t Address) {
    uint64_t Ns = MonotonicNs();
    if (Clock == 0 || Clock == 5) Ns = time(nullptr) * 1'000'000'000ULL;
    else if (Clock != 1 && Clock != 4 && Clock != 6 && Clock != 7) return Invalid;
    const uint64_t Time[] {Ns / 1'000'000'000, Ns % 1'000'000'000};
    return CopyOut(Address, Time);
  }

  int64_t AdviseGuest(uint64_t Address, uint64_t Size, uint64_t Advice) {
    if (Address % GuestPageSize || Size > Task->Memory.MaxMapping) return Invalid;
    if (Size && !Task->Memory.IsMapped(Address, Task->Memory.RoundUp(Size))) return -12;
    // These are hints. Pages remain valid for Linux's MADV_FREE contract.
    if (Advice <= 3 || Advice == 8 || Advice == 14 || Advice == 15) return 0;
    if (Advice != 4 || !Size) return Advice == 4 ? 0 : Invalid;
    if (!Task->Memory.Owns(Address, Task->Memory.RoundUp(Size))) return Invalid;
    // pthread caches its stack after calling MADV_DONTNEED on its unused pages.
    if (Available(Address, PF_W) < Task->Memory.RoundUp(Size)) return Invalid;
    std::memset(reinterpret_cast<void*>(Address), 0, Task->Memory.RoundUp(Size));
    return 0;
  }

  int64_t GetAttribute(uint64_t Address) {
    const auto Name = GuestString(Address);
    if (!Name) return Fault;
    LinuxStat Info {};
    const auto Status = PathStat(CurrentDirectory, *Name, Info);
    return Status ? Status : -61;
  }

  int64_t TerminalIoctl(uint64_t FD, uint64_t Command, uint64_t Argument) {
    const auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (!Interactive || File->Console == ConsoleStream::None) return -25;
    switch (Command) {
    case 0x5401: return CopyOut(Argument, Terminal.Attributes); // TCGETS
    case 0x5402: case 0x5403: case 0x5404: {
      TerminalAttributes New;
      if (Available(Argument, PF_R) < sizeof(New)) return Fault;
      std::memcpy(&New, reinterpret_cast<const void*>(Argument), sizeof(New));
      Terminal.SetAttributes(New, Command == 0x5404);
      return 0;
    }
    case 0x540b: // TCFLSH
      if (Argument > 2) return Invalid;
      if (Argument != 1) Terminal.Clear();
      return 0;
    case 0x540f: return CopyOut(Argument, ForegroundGroup);
    case 0x5429: return CopyOut(Argument, ProcessID); // TIOCGPGRP, TIOCGSID
    case 0x5410: {
      uint32_t Group {};
      if (Available(Argument, PF_R) < sizeof(Group)) return Fault;
      std::memcpy(&Group, reinterpret_cast<const void*>(Argument), sizeof(Group));
      if (std::none_of(Processes.begin(), Processes.end(), [&](const auto& Item) { return Item.second.Group == Group; })) return -1;
      ForegroundGroup = Group;
      return 0;
    }
    case 0x5413: {
      const auto* Console = consoleGetDefault();
      const uint16_t Size[] {static_cast<uint16_t>(Console->windowHeight), static_cast<uint16_t>(Console->windowWidth), 0, 0};
      return CopyOut(Argument, Size);
    }
    case 0x5414: { // TIOCSWINSZ, the physical console has a fixed geometry.
      return Available(Argument, PF_R) >= 8 ? 0 : Fault;
    }
    case 0x541b: return CopyOut(Argument, static_cast<uint32_t>(Terminal.Pending()));
    default: return -25;
    }
  }

  void FeedTerminalByte(char Byte) {
    ++IOEpoch;
    const auto Echo = Terminal.Feed(Byte);
    if (!Echo.empty()) {
      std::fwrite(Echo.data(), 1, Echo.size(), stdout);
      consoleUpdate(nullptr);
    }
  }

  void FeedTerminalKey(unsigned Key, uint64_t Modifiers) {
    const auto Encoded = EncodeTerminalKey(Key, Modifiers & HidKeyboardModifier_Shift, Modifiers & HidKeyboardModifier_Control,
                                           Modifiers & (HidKeyboardModifier_LeftAlt | HidKeyboardModifier_RightAlt),
                                           Modifiers & HidKeyboardModifier_CapsLock);
    for (size_t Index = 0; Index < Encoded.Length; ++Index) FeedTerminalByte(Encoded.Bytes[Index]);
  }

  void OpenLineKeyboard() {
    SwkbdConfig Config {};
    const auto CreateStatus = swkbdCreate(&Config, 0);
    if (R_FAILED(CreateStatus)) {
      WriteLog("Could not open the Horizon keyboard.");
      return;
    }
    swkbdConfigMakePresetDefault(&Config);
    swkbdConfigSetHeaderText(&Config, "Bash input");
    swkbdConfigSetOkButtonText(&Config, "Send");
    swkbdConfigSetReturnButtonFlag(&Config, false);
    swkbdConfigSetStringLenMax(&Config, 512);
    std::array<char, 2049> Line {};
    const auto Status = swkbdShow(&Config, Line.data(), Line.size());
    swkbdClose(&Config);
    if (R_SUCCEEDED(Status)) {
      for (const auto Byte : std::string_view(Line.data(), strnlen(Line.data(), Line.size()))) {
        if (static_cast<unsigned char>(Byte) >= 32 && Byte != 127) FeedTerminalByte(Byte);
      }
      FeedTerminalByte('\r');
    }
    consoleUpdate(nullptr);
  }

  bool PollTerminalInput() {
    if (TerminalClosed) return false;
    if (!appletMainLoop()) {
      TerminalClosed = true;
      return false;
    }
    padUpdate(&Input);
    const auto Held = padGetButtons(&Input);
    const auto Pressed = padGetButtonsDown(&Input);
    constexpr auto Sticks = HidNpadButton_StickL | HidNpadButton_StickR;
    if (Pressed & HidNpadButton_Plus) {
      TerminalClosed = true;
      return false;
    }
    if ((Held & Sticks) == Sticks && (Pressed & Sticks)) OpenLineKeyboard();

    std::array<HidKeyboardState, 8> Samples {};
    const auto Count = hidGetKeyboardStates(Samples.data(), Samples.size());
    if (!Count) return true;
    const auto Now = MonotonicNs();
    for (size_t Index = Count; Index > 0; --Index) {
      const auto& State = Samples[Index - 1];
      if (State.sampling_number <= LastKeyboard.sampling_number) continue;
      for (unsigned Key = HidKeyboardKey_A; Key <= HidKeyboardKey_Backslash; ++Key) {
        const auto Usage = static_cast<HidKeyboardKey>(Key);
        if (!hidKeyboardStateGetKey(&State, Usage) || hidKeyboardStateGetKey(&LastKeyboard, Usage)) continue;
        FeedTerminalKey(Key, State.modifiers);
        RepeatKey = Usage;
        NextRepeat = Now + 400'000'000;
      }
      LastKeyboard = State;
    }
    if (RepeatKey && Now >= NextRepeat && hidKeyboardStateGetKey(&LastKeyboard, RepeatKey)) {
      FeedTerminalKey(RepeatKey, LastKeyboard.modifiers);
      NextRepeat = Now + 45'000'000;
    }
    return true;
  }

  int64_t ReadTerminal(char* Output, size_t Count, bool Nonblocking) {
    PollTerminalInput();
    if (Terminal.TakeEof()) return 0;
    const auto& Attr = Terminal.Attributes;
    const bool Canonical = Attr.Local & 2;
    const auto Minimum = Canonical ? size_t {1} : std::min<size_t>(Count, Attr.Characters[6]);
    const uint64_t Timeout = Canonical ? 0 : Attr.Characters[5] * 100'000'000ULL;
    const auto Pending = Terminal.Pending();
    if (TerminalClosed || (Pending >= Minimum && (Minimum || Pending || !Timeout))) return Terminal.Read(Output, Count);
    if (Nonblocking) return Pending ? static_cast<int64_t>(Terminal.Read(Output, Count)) : -11;
    if (Timeout && (!Minimum || Pending)) {
      const auto Now = MonotonicNs();
      if (!Current->Deadline) Current->Deadline = AddDeadline(Now, Timeout);
      if (Now >= Current->Deadline) return Terminal.Read(Output, Count);
    }
    return WaitForIO(WaitKind::Terminal);
  }

  int64_t SelectGuest(uint64_t Count, uint64_t Read, uint64_t Write, uint64_t Except, uint64_t Timeout, uint64_t SigData) {
    if (Count > Task->Files.size()) return Invalid;
    using Mask = std::array<uint64_t, 4>;
    static_assert(std::tuple_size_v<Mask> * 64 == DescriptorCount);
    const auto Size = (Count + 63) / 64 * sizeof(uint64_t);
    const uint64_t Addresses[] {Read, Write, Except};
    std::array<Mask, 3> Requested {};
    for (size_t Set = 0; Set < Requested.size(); ++Set) {
      if (!Addresses[Set] || !Size) continue;
      if (Available(Addresses[Set], PF_R | PF_W) < Size) return Fault;
      std::memcpy(Requested[Set].data(), reinterpret_cast<const void*>(Addresses[Set]), Size);
    }
    if (SigData) {
      uint64_t SignalMaskArgs[2];
      if (Available(SigData, PF_R) < sizeof(SignalMaskArgs)) return Fault;
      std::memcpy(SignalMaskArgs, reinterpret_cast<const void*>(SigData), sizeof(SignalMaskArgs));
      if (SignalMaskArgs[0] && SignalMaskArgs[1] != 8) return Invalid;
      if (SignalMaskArgs[0] && Available(SignalMaskArgs[0], PF_R) < 8) return Fault;
    }
    uint64_t TimeoutNs {};
    if (Timeout && !ReadTimespec(Timeout, TimeoutNs)) return Available(Timeout, PF_R) < 16 ? Fault : Invalid;
    if (Timeout && !Current->Deadline) Current->Deadline = AddDeadline(MonotonicNs(), TimeoutNs);
    {
      if (Interactive) PollTerminalInput();
      std::array<Mask, 3> ReadySets {};
      int64_t ReadyCount {};
      for (size_t FD = 0; FD < Count; ++FD) {
        const auto Word = FD / 64;
        const auto Bit = 1ULL << (FD % 64);
        const auto* File = GetFile(FD);
        if (!File && ((Requested[0][Word] | Requested[1][Word] | Requested[2][Word]) & Bit)) return BadFile;
        if (!File) continue;
        const auto Events = DescriptorEvents(*File);
        for (size_t Set = 0; Set < ReadySets.size(); ++Set) {
          const uint16_t MaskBits[] {0x19, 0x1c, 2};
          if ((Requested[Set][Word] & Bit) && (Events & MaskBits[Set])) {
            ReadySets[Set][Word] |= Bit;
            ++ReadyCount;
          }
        }
      }
      const auto Now = MonotonicNs();
      if (ReadyCount || (Timeout && Now >= Current->Deadline) || TerminalClosed) {
        for (size_t Set = 0; Set < ReadySets.size(); ++Set) {
          if (Addresses[Set] && Size) std::memcpy(reinterpret_cast<void*>(Addresses[Set]), ReadySets[Set].data(), Size);
        }
        return ReadyCount;
      }
      return WaitForIO(Interactive ? WaitKind::Terminal : WaitKind::IO);
    }
  }

  static int16_t StreamPoll(const OpenFile& File, int16_t Events) {
    if (!File.Pipe) return File.Socket->Poll(Events);
    const bool WriteEnd = (File.StatusFlags & 3) == 1;
    if (WriteEnd && File.Socket->Peer.expired()) return 8; // POLLERR
    if (!WriteEnd && File.Socket->Peer.expired()) {
      return 0x10 | (File.Socket->Input.empty() ? 0 : Events & 1);
    }
    return File.Socket->Poll(Events & (WriteEnd ? 4 : 1));
  }

  uint16_t DescriptorEvents(const OpenFile& File) const {
    if (File.Socket) return StreamPoll(File, 7);
    const bool InputReady = File.Console == ConsoleStream::None || Terminal.Readable() || TerminalClosed;
    const bool CanRead = (File.StatusFlags & 3) != 1;
    const bool CanWrite = (File.StatusFlags & 3) != 0;
    return (InputReady && CanRead ? 1 : 0) | (CanWrite ? 4 : 0);
  }

  int64_t ArchPrctl(FEXCore::Core::CpuStateFrame* Frame, uint64_t Code, uint64_t Value) {
    switch (Code) {
    case 0x1001: Frame->State.gs_cached = Value; return 0;
    case 0x1002: Frame->State.fs_cached = Value; return 0;
    case 0x1003:
    case 0x1004: {
      if (sizeof(uint64_t) > Available(Value, PF_W)) return Fault;
      const auto Base = Code == 0x1003 ? Frame->State.fs_cached : Frame->State.gs_cached;
      std::memcpy(reinterpret_cast<void*>(Value), &Base, sizeof(Base));
      return 0;
    }
    default: return Invalid;
    }
  }

  int64_t RandomGuest(uint64_t Address, uint64_t Count, uint64_t Flags) {
    if (Flags & ~3ULL) return Invalid;
    Count = std::min<uint64_t>(Count, 65536);
    if (!Count) return 0;
    if (Count > Available(Address, PF_W)) return Fault;
    randomGet(reinterpret_cast<void*>(Address), Count);
    return Count;
  }

  int64_t PollGameInput() {
    if (!appletMainLoop()) {
      return FEX_NX2_QUIT;
    }
    padUpdate(&Input);
    const auto Held = padGetButtons(&Input);
    const auto Pressed = padGetButtonsDown(&Input);
    return ((Held & HidNpadButton_AnyUp) ? FEX_NX2_UP : 0) |
           ((Held & HidNpadButton_AnyDown) ? FEX_NX2_DOWN : 0) |
           ((Held & HidNpadButton_AnyLeft) ? FEX_NX2_LEFT : 0) |
           ((Held & HidNpadButton_AnyRight) ? FEX_NX2_RIGHT : 0) |
           ((Pressed & (HidNpadButton_B | HidNpadButton_Plus)) ? FEX_NX2_QUIT : 0);
  }

  int64_t PresentGuestFrame(uint64_t Address) {
    constexpr u32 Width = FEX_NX2_FRAME_WIDTH;
    constexpr u32 Height = FEX_NX2_FRAME_HEIGHT;
    constexpr u32 Scale = 4;
    if (Address % alignof(uint32_t) || Available(Address, PF_R) < Width * Height * sizeof(uint32_t)) {
      return Fault;
    }
    if (!GraphicsActive) {
      WriteLog("Opening the graphical display.");
      consoleExit(nullptr);
      Result Status = framebufferCreate(&Video, nwindowGetDefault(), Width * Scale, Height * Scale, PIXEL_FORMAT_RGBA_8888, 2);
      if (R_SUCCEEDED(Status)) {
        Status = framebufferMakeLinear(&Video);
        if (R_FAILED(Status)) {
          framebufferClose(&Video);
        }
      }
      if (R_FAILED(Status)) {
        consoleInit(nullptr);
        char Message[96];
        std::snprintf(Message, sizeof(Message), "Could not open the graphical display (0x%08x).", Status);
        PrintStage(Message);
        return -5; // EIO
      }
      GraphicsActive = true;
      WriteLog("Graphical display initialized.");
    }

    u32 Stride {};
    auto* Destination = static_cast<uint8_t*>(framebufferBegin(&Video, &Stride));
    if (!Destination) {
      WriteLog("Could not dequeue a graphical framebuffer.");
      return -5;
    }
    const auto* Source = reinterpret_cast<const uint32_t*>(Address);
    constexpr size_t RowBytes = Width * Scale * sizeof(uint32_t);
    for (u32 Y = 0; Y < Height; ++Y) {
      auto* FirstRow = Destination + Y * Scale * Stride;
      auto* Row = reinterpret_cast<uint32_t*>(FirstRow);
      for (u32 X = 0; X < Width; ++X) {
        std::fill_n(Row + X * Scale, Scale, Source[Y * Width + X]);
      }
      for (u32 Copy = 1; Copy < Scale; ++Copy) {
        std::memcpy(FirstRow + Copy * Stride, FirstRow, RowBytes);
      }
    }
    framebufferEnd(&Video);
    if (!Presented) {
      Presented = true;
      WriteLog("Presented the first guest frame.");
    }
    return 0;
  }

  void FlushGuestOutput() {
    if (!OutputLog.empty()) {
      WriteLog(OutputLog.c_str());
      OutputLog.clear();
    }
  }

  void LogGuestOutput(const char* Text, size_t Count) {
    for (size_t Index = 0; Index < Count; ++Index) {
      const auto Byte = static_cast<unsigned char>(Text[Index]);
      if (Byte == '\n' || OutputLog.size() == 960) {
        FlushGuestOutput();
      }
      if (Byte >= 32 && Byte != 127) {
        OutputLog += static_cast<char>(Byte);
      } else if (Byte == '\t') {
        OutputLog += ' ';
      }
    }
  }

  int64_t WriteVectorGuest(uint64_t FD, uint64_t Address, uint64_t Count) {
    struct IoVector { uint64_t Base; uint64_t Size; };
    if (!GetFile(FD)) return BadFile;
    if (!Count) return 0;
    if (Count > 1024) return Invalid;
    if (Count * sizeof(IoVector) > Available(Address, PF_R)) return Fault;

    int64_t Total = 0;
    for (uint64_t Index = 0; Index < Count; ++Index) {
      IoVector Vector;
      std::memcpy(&Vector, reinterpret_cast<const void*>(Address + Index * sizeof(Vector)), sizeof(Vector));
      const auto Written = WriteGuest(FD, Vector.Base, Vector.Size);
      if (Written < 0) {
        if (Total && Written == RetrySyscall) Current->Waiting = WaitKind::None;
        return Total ? Total : Written;
      }
      Total += Written;
      if (static_cast<uint64_t>(Written) < Vector.Size) break;
    }
    return Total;
  }

  int64_t WriteGuest(uint64_t FD, uint64_t Address, uint64_t Count) {
    auto* File = GetFile(FD);
    if (!File || (File->StatusFlags & 3) == 0) return BadFile;
    if (!File->Socket && File->Console == ConsoleStream::None) return BadFile;
    Count = std::min<uint64_t>(Count, 4096);
    if (!Count) {
      return 0;
    }
    if (Count > Available(Address, PF_R)) {
      return Fault;
    }
    if (File->Socket) return WriteSocket(*File->Socket, reinterpret_cast<const char*>(Address), Count, false, File->Pipe);
    std::string Filtered;
    std::string_view Output {reinterpret_cast<const char*>(Address), static_cast<size_t>(Count)};
    if (Interactive) {
      Filtered = TerminalOutput.Filter(Output);
      Output = Filtered;
    }
    if (File->Console == ConsoleStream::Error || Logging == LogLevel::Trace) {
      LogGuestOutput(Output.data(), Output.size());
    }
    if (GraphicsActive) {
      // The console renderer shares the display with the game.
      return Count;
    }
    const size_t Written = Output.empty() ? 0 : std::fwrite(Output.data(), 1, Output.size(), stdout);
    std::fflush(stdout);
    consoleUpdate(nullptr);
    return Written == Output.size() ? static_cast<int64_t>(Count) : -5;
  }

  int64_t OpenGuest(uint64_t Directory, uint64_t Address, uint64_t Flags) {
    constexpr uint64_t CloseOnExec = 02000000, DirectoryOnly = 0200000, NoFollow = 0400000;
    constexpr uint64_t Nonblocking = 04000, NoControllingTerminal = 0400, LargeFile = 0100000;
    const auto Name = GuestString(Address);
    if (!Name) return Fault;
    if (Name->empty()) return -2;
    const bool OpenTerminal = Interactive && *Name == "/dev/tty";
    const uint64_t Allowed = CloseOnExec | DirectoryOnly | NoFollow | Nonblocking | NoControllingTerminal | LargeFile | (OpenTerminal ? 3 : 0);
    if (Flags & ~Allowed) return Flags & 3 ? -30 : Invalid;
    if (OpenTerminal && (Flags & DirectoryOnly)) return -20;
    const auto FD = FreeDescriptor();
    if (FD == NoDescriptor) return -24;
    auto File = std::make_shared<OpenFile>();
    File->StatusFlags = Flags & (Nonblocking | LargeFile | 3);
    if (OpenTerminal) {
      File->Console = ConsoleStream::Terminal;
      File->Path = *Name;
    } else if (*Name == "/dev/null" || *Name == "/proc/version" || *Name == "/proc/self/status") {
      if (Flags & DirectoryOnly) return -20;
      File->Contents = *Name == "/proc/version" ? "Linux version 5.0.0 (FEX Switch 2 guest)\n" :
        *Name == "/proc/self/status" ? "Name:\tfex-guest\n" : "";
      File->Path = *Name;
    } else {
      const auto Path = ResolveGuestPath(Directory, *Name);
      if (!Path || Path->size() >= FS_MAX_PATH) return Invalid;
      File->Path = *Path;
      FsFileSystem SdCard {};
      if (R_FAILED(fsOpenSdCardFileSystem(&SdCard))) return -5;
      FsDirEntryType Type {};
      auto Status = fsFsGetEntryType(&SdCard, Path->c_str(), &Type);
      if (R_SUCCEEDED(Status) && Type == FsDirEntryType_File && !(Flags & DirectoryOnly) && !Name->ends_with('/')) {
        Status = fsFsOpenFile(&SdCard, Path->c_str(), FsOpenMode_Read, &File->Handle);
        File->RealFile = R_SUCCEEDED(Status);
      }
      fsFsClose(&SdCard);
      if (Logging == LogLevel::Trace) {
        char Message[1024];
        std::snprintf(Message, sizeof(Message), "openat %s -> 0x%08x", Path->c_str(), Status);
        WriteLog(Message);
      }
      if (R_FAILED(Status)) return -2;
      if (Type == FsDirEntryType_Dir) {
        auto& Entries = File->Directory.emplace();
        if (R_FAILED(ReadDirectory(*Path, Entries))) return -5;
        Entries.insert(Entries.begin(), {{".", true}, {"..", true}});
      } else if (!File->RealFile) {
        return -20;
      }
    }
    File->Inode = FileInode(File->Path);
    Task->Files[FD] = std::move(File);
    Task->DescriptorFlags[FD] = (Flags & CloseOnExec) != 0;
    return FD;
  }

  int64_t ReadGuest(uint64_t FD, uint64_t Address, uint64_t Count) {
    auto* File = GetFile(FD);
    if (!File || (File->StatusFlags & 3) == 1) return BadFile;
    if (!Count) {
      return 0;
    }
    Count = std::min<uint64_t>(Count, 65536);
    if (Count > Available(Address, PF_W)) {
      return Fault;
    }
    auto* Buffer = reinterpret_cast<char*>(Address);
    if (Interactive && (File->Console == ConsoleStream::Input || File->Console == ConsoleStream::Terminal)) {
      if ((File->StatusFlags & 3) == 1) return BadFile;
      return ReadTerminal(Buffer, Count, File->StatusFlags & 04000);
    }
    if (File->Console == ConsoleStream::Input) {
      while (appletMainLoop()) {
        padUpdate(&Input);
        const auto Buttons = padGetButtonsDown(&Input);
        constexpr std::array Controls {
          std::pair {u64 {HidNpadButton_AnyUp}, 'w'}, std::pair {u64 {HidNpadButton_AnyLeft}, 'a'},
          std::pair {u64 {HidNpadButton_AnyDown}, 's'}, std::pair {u64 {HidNpadButton_AnyRight}, 'd'},
          std::pair {u64 {HidNpadButton_B | HidNpadButton_Plus}, 'q'}};
        for (const auto& [Button, Key] : Controls) {
          if (Buttons & Button) {
            *Buffer = Key;
            return 1;
          }
        }
        if (GraphicsActive) {
          svcSleepThread(10'000'000);
        } else {
          consoleUpdate(nullptr);
        }
      }
      return 0;
    }
    if (File->Socket) return ReadSocket(*File->Socket, Buffer, Count, false);
    if (File->Directory) return -21;
    const auto Read = ReadFileAt(*File, Buffer, Count, File->Offset);
    if (Read > 0) File->Offset += Read;
    return Read;
  }

  static int64_t ReadFileAt(OpenFile& File, void* Buffer, size_t Count, uint64_t Offset) {
    if (Offset > INT64_MAX) return Invalid;
    if (File.Socket || File.Directory || File.Console != ConsoleStream::None) return -29;
    if (!File.RealFile) {
      if (Offset >= File.Contents.size()) return 0;
      const auto Data = File.Contents.substr(Offset);
      const size_t Read = std::min(Data.size(), Count);
      std::memcpy(Buffer, Data.data(), Read);
      return Read;
    }
    u64 Read {};
    return R_SUCCEEDED(fsFileRead(&File.Handle, Offset, Buffer, Count, FsReadOption_None, &Read)) ? static_cast<int64_t>(Read) : -5;
  }

  int64_t ReadAtGuest(uint64_t FD, uint64_t Address, uint64_t Count, uint64_t Offset) {
    auto* File = GetFile(FD);
    if (!File) return BadFile;
    Count = std::min<uint64_t>(Count, 65536);
    if (!Count) return 0;
    if (Count > Available(Address, PF_W)) return Fault;
    return ReadFileAt(*File, reinterpret_cast<void*>(Address), Count, Offset);
  }

  int64_t CloseGuest(uint64_t FD) {
    if (!GetFile(FD)) return BadFile;
    const auto& File = Task->Files[FD];
    const bool Socket = File->Socket != nullptr;
    Task->Files[FD].reset();
    Task->DescriptorFlags[FD] = 0;
    if (Socket) {
      std::erase_if(SocketNames, [](const auto& Entry) { return Entry.second.expired(); });
      ++IOEpoch;
    }
    return 0;
  }

  static uint8_t PageFlags(uint64_t Protection) {
    return ((Protection & 1) ? PF_R : 0) | ((Protection & 2) ? PF_W : 0) | ((Protection & 4) ? PF_X : 0);
  }

  void InvalidateExecutableRange(FEXCore::Core::CpuStateFrame* Frame, uint64_t Start, size_t Size, uint8_t NewFlags) {
    bool Invalidate = NewFlags & PF_X;
    for (size_t Offset = 0; !Invalidate && Offset < Size; Offset += GuestPageSize) {
      Invalidate = Task->Memory.Query(Start + Offset).Flags & PF_X;
    }
    if (!Invalidate) return;
    auto* Thread = Frame->Thread;
    auto* Context = Thread->CTX;
    std::unique_lock Lock {Context->GetCodeInvalidationMutex()};
    Context->InvalidateCodeBuffersCodeRange(Start, Size);
    for (const auto& Guest : Threads) {
      if (Guest.Core && Guest.PID == Task->PID) Context->InvalidateThreadCachedCodeRange(Guest.Core, Start, Size);
    }
  }

  bool OverlapsHeap(uint64_t Address, size_t Size) const {
    return Task->HeapStart && Address < Task->HeapStart + HeapLimit && Task->HeapStart < Address + Size;
  }

  int64_t MapGuest(FEXCore::Core::CpuStateFrame* Frame, uint64_t Address, uint64_t Size, uint64_t Protection, uint64_t Flags,
                   uint64_t FD, uint64_t Offset) {
    constexpr uint64_t Shared = 1, Private = 2, Fixed = 0x10, Anonymous = 0x20, DenyWrite = 0x800,
                       Executable = 0x1000, NoReserve = 0x4000, Stack = 0x20000;
    const bool IsFixed = Flags & Fixed;
    const bool IsAnonymous = Flags & Anonymous;
    if (!Size || Size > Task->Memory.MaxMapping || (Protection & ~7ULL) || ((Flags & (Shared | Private)) != Private && !(IsAnonymous && (Flags & (Shared | Private)) == Shared)) ||
        (Flags & ~(Shared | Private | Fixed | Anonymous | DenyWrite | Executable | NoReserve | Stack)) || Offset % GuestPageSize ||
        (IsAnonymous && (Offset || static_cast<uint32_t>(FD) != UINT32_MAX))) return Invalid;
    auto* File = IsAnonymous ? nullptr : GetFile(FD);
    if (!IsAnonymous && !File) return BadFile;
    const auto Rounded = Task->Memory.RoundUp(Size);
    if (IsFixed && (Address % GuestPageSize || !Task->Memory.Owns(Address, Rounded) || OverlapsHeap(Address, Rounded))) return Invalid;
    const auto Access = PageFlags(Protection);
    if (!IsFixed) {
      Address = Task->Memory.Allocate(Size, Access, (Flags & Shared) != 0);
      if (!Address) return -12;
      if (File && ReadFileAt(*File, reinterpret_cast<void*>(Address), Rounded, Offset) < 0) {
        Task->Memory.Set(Address, Rounded, Task->Memory.Unmapped, true);
        return -5;
      }
      RecordMapping(Address, Rounded, Offset, File ? std::string_view(File->Path) : std::string_view {});
      return Address;
    }

    std::unique_ptr<void, decltype(&std::free)> Copy {nullptr, &std::free};
    if (File) {
      Copy.reset(std::calloc(1, Rounded));
      if (!Copy) return -12;
      if (ReadFileAt(*File, Copy.get(), Rounded, Offset) < 0) return -5;
    }
    InvalidateExecutableRange(Frame, Address, Rounded, Access);
    if (Copy) {
      std::memcpy(reinterpret_cast<void*>(Address), Copy.get(), Rounded);
    } else {
      std::memset(reinterpret_cast<void*>(Address), 0, Rounded);
    }
    Task->Memory.Set(Address, Rounded, Access, true);
    RecordMapping(Address, Rounded, Offset, File ? std::string_view(File->Path) : std::string_view {});
    return Address;
  }

  int64_t ProtectGuest(FEXCore::Core::CpuStateFrame* Frame, uint64_t Address, uint64_t Size, uint64_t Protection) {
    if ((Protection & ~7ULL) || Address % GuestPageSize || Size > Task->Memory.MaxMapping) return Invalid;
    if (!Size) return 0;
    const auto Rounded = Task->Memory.RoundUp(Size);
    if (!Task->Memory.IsMapped(Address, Rounded)) return -12;
    const auto Access = PageFlags(Protection);
    InvalidateExecutableRange(Frame, Address, Rounded, Access);
    return Task->Memory.Set(Address, Rounded, Access) ? 0 : Invalid;
  }

  int64_t UnmapGuest(FEXCore::Core::CpuStateFrame* Frame, uint64_t Address, uint64_t Size) {
    if (Address % GuestPageSize || !Size || Size > Task->Memory.MaxMapping) return Invalid;
    const auto Rounded = Task->Memory.RoundUp(Size);
    if (!Task->Memory.Owns(Address, Rounded) || OverlapsHeap(Address, Rounded)) return Invalid;
    InvalidateExecutableRange(Frame, Address, Rounded, 0);
    if (!Task->Memory.Set(Address, Rounded, Task->Memory.Unmapped, true)) return Invalid;
    RecordMapping(Address, Rounded, 0, {});
    return 0;
  }

  int64_t SetHeapEnd(uint64_t Requested) {
    if (!Task->HeapStart) {
      Task->HeapStart = Task->Memory.Allocate(HeapLimit, 0);
      Task->HeapEnd = Task->HeapStart;
    }
    if (!Task->HeapStart || Requested < Task->HeapStart || Requested - Task->HeapStart > HeapLimit) return Task->HeapEnd;
    const size_t OldSize = Task->Memory.RoundUp(Task->HeapEnd - Task->HeapStart);
    const size_t NewSize = Task->Memory.RoundUp(Requested - Task->HeapStart);
    if (NewSize > OldSize) {
      std::memset(reinterpret_cast<void*>(Task->HeapStart + OldSize), 0, NewSize - OldSize);
      Task->Memory.Set(Task->HeapStart + OldSize, NewSize - OldSize, PF_R | PF_W, true);
    } else if (NewSize < OldSize) {
      Task->Memory.Set(Task->HeapStart + NewSize, OldSize - NewSize, 0, true);
    }
    Task->HeapEnd = Requested;
    return Task->HeapEnd;
  }

  static constexpr size_t MemoryLimit = 96 * 1024 * 1024;
  static constexpr size_t HeapLimit = 8 * 1024 * 1024;
  std::string RootPath;
  std::array<GuestThread, 8> Threads {};
  GuestThread* Current {};
  std::jmp_buf ReturnToHost;
  uint64_t ExitStatus {};
  uint64_t IOEpoch {};
  uint32_t NextTid {ProcessID + 1};
  bool ProcessExited {};
  bool DispatchFailed {};
  std::string OutputLog;
  const bool Interactive;
  GuestTerminalInput Terminal;
  GuestConsoleOutput TerminalOutput;
  HidKeyboardState LastKeyboard {};
  HidKeyboardKey RepeatKey {};
  uint64_t NextRepeat {};
  bool TerminalClosed {};
  std::map<std::string, std::weak_ptr<StreamSocket>> SocketNames;
  uint64_t NextSocketInode {4};
  std::map<uint32_t, GuestProcess> Processes;
  GuestProcess* Task {};
  uint32_t ForegroundGroup {ProcessID};
  PadState Input {};
  const u64 StartTick {armGetSystemTick()};
  Framebuffer Video {};
  bool Presented {};

};

} // namespace

static std::optional<uint64_t> RunGuestOnCurrentThread(uint64_t Entry, std::span<const Segment> Segments, const std::string& Folder,
                                 std::string_view Program, std::span<const std::string> Arguments,
                                 const ElfImage* ProgramImage, uint64_t InterpreterBase, const std::string& WorkingDirectory, bool SingleBlock, bool Interactive) {
  using namespace FEXCore;
  Config::Set(Config::CONFIG_IS64BIT_MODE, "1");
  Config::Set(Config::CONFIG_DISKCACHE, "0");
  Config::Set(Config::CONFIG_MULTIBLOCK, SingleBlock ? "0" : "1");
  Config::Set(Config::CONFIG_MAXINST, "64");
  Config::Set(Config::CONFIG_DISABLE_VIXL_INDIRECT_RUNTIME_CALLS, "1");

  if (Logging == LogLevel::Trace) WriteRunCheckpoint("Launcher entered guest runtime");
  auto Syscalls = fextl::make_unique<GuestSyscalls>(Segments, Folder, Root, WorkingDirectory, Interactive);
  if (Logging == LogLevel::Trace) WriteRunCheckpoint("Launcher initializing guest context");
  PrintStage("Creating FEXCore context and dispatcher...");
  Core::CPUState State {};
  if (!Syscalls->InitMain(State)) return std::nullopt;

  PrintStage("Creating guest thread...");
  State.rip = Entry;
  const auto StackBase = Syscalls->CreateStack(MainStackSize);
  if (!StackBase) {
    PrintStage("Could not allocate the guest stack.");
    return std::nullopt;
  }
  std::vector<std::string> Argv {std::string {Program}};
  Argv.insert(Argv.end(), Arguments.begin(), Arguments.end());
  std::vector<std::string> Environment {"PATH=/bin:/usr/bin", "HOME=/", "LANG=C", Interactive ? "TERM=ansi" : "TERM=dumb"};
  if (Interactive) {
    Environment.insert(Environment.end(), {"HISTFILE=", "TERMINFO=/lib/terminfo", "PS1=\\[\\e[36m\\]fex\\[\\e[0m\\]:\\w\\$ "});
  }
  State.gregs[X86State::REG_RSP] = BuildInitialStack(StackBase, Program, Argv, Environment, ProgramImage, InterpreterBase);

  if (!Syscalls->AddMainThread(State)) {
    PrintStage("Could not allocate the guest return stack.");
    return std::nullopt;
  }
  if (Logging == LogLevel::Trace) WriteRunCheckpoint("Launcher created guest thread");

  PrintStage(SingleBlock ? "JIT mode: single block" : "JIT mode: multiblock");
  if (Logging == LogLevel::Trace) {
    for (size_t Index = 0; Index < Segments.size(); ++Index) {
      const auto& Segment = Segments[Index];
      const bool IsInterpreter = ProgramImage && Index >= ProgramImage->GetSegments().size();
      const auto Base = IsInterpreter ? InterpreterBase : ProgramImage ? ProgramImage->Base : Entry;
      RecordMapping(Segment.Start, Segment.Size, Segment.Start - Base,
                    IsInterpreter ? std::string_view(ProgramImage->Interpreter) : Program);
    }
    TraceGuest("Program image", ProgramImage ? ProgramImage->Entry : Entry, ProgramImage ? ProgramImage->Base : Entry);
    TraceGuest("Interpreter start", Entry, InterpreterBase);
    FEXCore::Switch2::TraceHandler = TraceGuest;
  }
  PrintStage("Running x86-64 guest...");
  const auto Exit = Syscalls->RunThreads();
  FEXCore::Switch2::TraceHandler = nullptr;
  TraceMappings.clear();
  Syscalls->CloseVideo();
  if (Exit) {
    char ResultText[96];
    std::snprintf(ResultText, sizeof(ResultText), "Guest syscall %llu, exit status %llu",
                  static_cast<unsigned long long>(Syscalls->Number), static_cast<unsigned long long>(*Exit));
    WriteLog(ResultText);
  }
  Syscalls.reset();
  if (Logging == LogLevel::Trace) WriteRunCheckpoint("Launcher destroyed guest contexts");
  return Exit;
}

std::optional<uint64_t> RunGuest(uint64_t Entry, std::span<const Segment> Segments, const std::string& Folder,
                                 std::string_view Program, std::span<const std::string> Arguments,
                                 const ElfImage* ProgramImage, uint64_t InterpreterBase, const std::string& WorkingDirectory, bool SingleBlock, bool Interactive) {
  BeginGuestLog();
  struct LogScope {
    ~LogScope() { EndGuestLog(); }
  } GuestLogScope;
  auto Run = [&] {
    return RunGuestOnCurrentThread(Entry, Segments, Folder, Program, Arguments, ProgramImage, InterpreterBase, WorkingDirectory, SingleBlock, Interactive);
  };
  if (!InterpreterBase) return Run();

  // Dynamic applications exercise substantially more compiler code. Give
  // FEX a stack independent of the launching homebrew environment.
  constexpr size_t CompilerStackSize = 4 * 1024 * 1024;
  std::optional<uint64_t> Exit;
  auto RunOnWorker = [&] { Exit = Run(); };
  using Work = decltype(RunOnWorker);
  Thread Worker {};
  if (Logging == LogLevel::Trace) WriteRunCheckpoint("Launcher creating guest worker");
  auto Status = threadCreate(&Worker, [](void* Arg) { (*static_cast<Work*>(Arg))(); },
                             &RunOnWorker, nullptr, CompilerStackSize, 0x2c, -2);
  if (R_SUCCEEDED(Status)) {
    Status = threadStart(&Worker);
    if (R_SUCCEEDED(Status)) {
      // The worker refers to the launcher's ELF storage and argument list.
      // A failed join would leave those references live.
      if (R_FAILED(threadWaitForExit(&Worker))) {
        WriteLog("Could not join the compiler thread.");
        std::abort();
      }
    }
    const auto CloseStatus = threadClose(&Worker);
    if (R_FAILED(CloseStatus)) {
      char Message[96];
      std::snprintf(Message, sizeof(Message), "Could not close the guest worker (0x%08x).", CloseStatus);
      WriteRunCheckpoint(Message);
    } else if (Logging == LogLevel::Trace) {
      WriteRunCheckpoint("Launcher closed guest worker");
    }
  }
  if (R_FAILED(Status)) {
    char Message[112];
    std::snprintf(Message, sizeof(Message), "Could not start the compiler thread (0x%08x).", Status);
    PrintStage(Message);
  }
  return Exit;
}

Result ReadDirectory(const std::string& Folder, std::vector<DirectoryEntry>& Entries) {
  FsFileSystem SdCard {};
  Result Status = fsOpenSdCardFileSystem(&SdCard);
  if (R_FAILED(Status)) {
    return Status;
  }
  FsDir Directory {};
  Status = fsFsOpenDirectory(&SdCard, Folder.c_str(), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles | FsDirOpenMode_NoFileSize, &Directory);
  fsFsClose(&SdCard);
  if (R_FAILED(Status)) {
    return Status;
  }

  std::array<FsDirectoryEntry, 8> Batch;
  for (;;) {
    s64 Count {};
    Status = fsDirRead(&Directory, &Count, Batch.size(), Batch.data());
    if (R_FAILED(Status) || Count == 0) {
      break;
    }
    for (s64 Index = 0; Index < Count; ++Index) {
      auto& Entry = Batch[Index];
      Entries.push_back({std::string {Entry.name, strnlen(Entry.name, sizeof(Entry.name))}, Entry.type == FsDirEntryType_Dir});
    }
  }
  fsDirClose(&Directory);
  std::sort(Entries.begin(), Entries.end(), [](const auto& A, const auto& B) {
    return A.IsDirectory != B.IsDirectory ? A.IsDirectory : A.Name < B.Name;
  });
  return Status;
}

} // namespace Switch2Guest
