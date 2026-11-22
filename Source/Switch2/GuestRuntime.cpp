// SPDX-License-Identifier: MIT
#include "GuestRuntime.h"
#include "GuestAddressSpace.h"
#include "GuestDirectoryRecovery.h"
#include "GuestFileWrite.h"
#include "GuestFilePunch.h"
#include "GuestInodeStore.h"
#include "GuestMemoryFile.h"
#include "GuestPosixAcl.h"
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
#include <bit>
#include <charconv>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <elf.h>
#include <map>
#include <set>

namespace Switch2Guest {
namespace {
// Initial real, effective, saved and filesystem IDs for the next launch.
std::optional<std::pair<uint32_t, uint32_t>> LaunchCredentials;
// Interrupt the next launch's journaled move into /.fex-recovery-test.
bool RecoveryTestLaunch {};
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
                           std::span<const std::string> Environment, const ElfImage* ProgramImage, uint64_t InterpreterBase,
                           std::array<uint32_t, 4> IDs = {1000, 1000, 1000, 1000}) {
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
      AT_RANDOM, reinterpret_cast<uint64_t>(Strings), AT_UID, IDs[0], AT_EUID, IDs[1], AT_GID, IDs[2], AT_EGID, IDs[3],
      AT_SECURE, IDs[0] != IDs[1] || IDs[2] != IDs[3]});
  }
  InitialStack.insert(InitialStack.end(), {AT_NULL, 0});
  const size_t StackBytes = InitialStack.size() * sizeof(uint64_t);
  const auto Stack = (reinterpret_cast<uint64_t>(Strings) - StackBytes) & ~15ULL;
  std::memcpy(reinterpret_cast<void*>(Stack), InitialStack.data(), StackBytes);
  return Stack;
}

class GuestFileService;

class GuestSyscalls final : public FEXCore::HLE::SyscallHandler {
  friend class GuestFileService;

public:
  GuestSyscalls(std::span<const Switch2Guest::Segment> Segments, std::string Folder, std::string RootPath, std::string WorkingDirectory,
                bool Interactive, bool Inspect = false)
    : RootPath {std::move(RootPath)}
    , LaunchFolder {Folder}
    , Interactive {Interactive} {
    LoadInodeStore();
    Task = &Processes.try_emplace(ProcessID).first->second;
    if (LaunchCredentials) {
      Task->UserIDs.fill(LaunchCredentials->first);
      Task->GroupIDs.fill(LaunchCredentials->second);
    }
    Task->HasDispatched = true;
    Task->Folder = std::move(Folder);
    Task->WorkingDirectory = std::move(WorkingDirectory);
    if (Inspect) return;
    for (const auto& Segment : Segments) {
      Task->Memory.Register(Segment.Start, Segment.Size, Segment.Flags);
    }
    for (size_t FD = 0; FD < 3; ++FD) {
      auto& File = Descriptors().Files[FD];
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

  const char* InspectElf(std::string_view Name, ElfImage& Image) {
    LinuxStat Info {};
    std::string Path;
    if (PathStat(CurrentDirectory, Name, Info, &Path) < 0 || Path.empty() || (Info.Mode & 0170000) != 0100000) {
      return "Could not resolve the guest ELF file.";
    }
    return ReadElf(FileBackingPath(Path).c_str(), Image, Info.Size, FileExtents(Info.Inode));
  }

  void TrackInitialExecutable(std::string_view Program) {
    LinuxStat Info {};
    if (!Program.empty() && !PathStat(CurrentDirectory, Program, Info) && (Info.Mode & 0170000) == 0100000) {
      Task->ExecutableInode = Info.Inode;
    }
  }

  // Directories the SD card holds natively, as opposed to overflow ones.
  bool HasNativeDirectory(const std::string& Path) const { return !IsOverflowDirectory(Path); }

  void AppendGuestDirectoryEntries(const std::string& Path, std::vector<DirectoryEntry>& Entries) const {
    const auto Prefix = Path + (Path.ends_with('/') ? "" : "/");
    for (auto It = SdInodes.lower_bound(Prefix); It != SdInodes.end() && It->first.starts_with(Prefix); ++It) {
      const auto Child = std::string_view(It->first).substr(Prefix.size());
      if (Child.find('/') != Child.npos) continue;
      if (IsOverflowDirectory(It->first)) {
        // An interrupted conversion can leave the empty native directory.
        std::erase_if(Entries, [&](const DirectoryEntry& Entry) { return Entry.Name == Child; });
        Entries.push_back({std::string(Child), true});
      } else if (IsVirtualEntry(It->first)) {
        Entries.push_back({std::string(Child), false, SymlinkAt(It->first) != nullptr,
                           static_cast<uint8_t>(SpecialNodeAt(It->first) >> 12)});
      }
    }
  }

  ~GuestSyscalls() override {
    if (SaveInodeStore() < 0) WriteLog("Failed to save guest inode metadata.");
    CloseVideo();
    // Queued socket descriptors can refer back to their receive socket.
    for (const auto& Entry : SocketRightsReceivers) {
      if (auto Socket = Entry.lock()) Socket->Rights.clear();
    }
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
        const auto Signal = PendingCaughtSignal(Thread, *Task);
        if (Signal) {
          constexpr uint64_t Restart = 0x10000000;
          if (Thread.Waiting == WaitKind::Lock && !(Task->SignalActions[Signal - 1][1] & Restart)) {
            auto& State = Thread.Core->CurrentFrame->State;
            State.gregs[FEXCore::X86State::REG_RAX] = -4; // EINTR
            State.rip += 2;
          }
          if (Thread.Waiting == WaitKind::Sleep) InterruptSleep(Thread);
          Thread.Waiting = WaitKind::None;
          Thread.PendingRecordLock.reset();
          if (!EnterSignalHandler(Thread.Core->CurrentFrame, Signal)) TerminateForSignal(11);
        }
        if (!Thread.Exited) Dispatch(*Thread.Core->CTX);
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
    case 2: Result = OpenGuest(CurrentDirectory, A, static_cast<uint32_t>(B), C); break;
    case 3: Result = CloseGuest(A); break;
    case 4: Result = StatAtGuest(CurrentDirectory, A, B, 0); break;
    case 6: Result = StatAtGuest(CurrentDirectory, A, B, 0x100); break; // AT_SYMLINK_NOFOLLOW
    case 5: Result = StatGuest(A, B); break;
    case 7: {
      const auto Milliseconds = static_cast<int32_t>(C);
      Result = PollGuest(A, B, Milliseconds < 0 ? std::nullopt : std::optional<uint64_t> {Milliseconds * 1'000'000ULL});
      break;
    }
    case 8: Result = SeekGuest(A, B, C); break;
    case 77: Result = TruncateDescriptor(A, B); break;
    case 319: Result = CreateMemoryFile(A, B); break;
    case 9:
      Result = MapGuest(Frame, A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]);
      break;
    case 10: Result = ProtectGuest(Frame, A, B, C); break;
    case 11: Result = UnmapGuest(Frame, A, B); break;
    case 12: Result = SetHeapEnd(A); break;
    case 13: Result = SignalAction(A, B, C, Registers[REG_R10]); break;
    case 14: Result = SignalMask(A, B, C, Registers[REG_R10]); break;
    case 15:
      if (ReturnGuestSignal(Frame)) std::longjmp(ReturnToHost, 1);
      Result = Fault;
      break;
    case 127: Result = B == 8 ? CopyOut(A, Current->PendingSignals & Current->SignalMask) : Invalid; break;
    case 16: Result = TerminalIoctl(A, B, C); break;
    case 17: Result = ReadGuest(A, B, C, Registers[REG_R10]); break;
    case 18: Result = WriteGuest(A, B, C, Registers[REG_R10]); break;
    case 19: Result = VectorIO(static_cast<uint32_t>(A), B, static_cast<uint32_t>(C), false); break;
    case 20: Result = VectorIO(static_cast<uint32_t>(A), B, static_cast<uint32_t>(C), true); break;
    case 21: Result = AccessGuest(CurrentDirectory, A, B, 0); break;
    case 22: Result = CreatePipe(A, 0); break;
    case 24: Result = 0; break;
    case 28: Result = AdviseGuest(A, B, C); break;
    case 32: Result = FileControl(A, 0, 0); break;
    case 33: Result = DuplicateDescriptor(A, B, false); break;
    case 40: Result = SendFile(A, B, C, Registers[REG_R10]); break;
    case 39: case 124: Result = Task->PID; break;
    case 111: Result = Task->Group; break;
    case 110: Result = Task->ParentPID; break;
    case 121: Result = GetProcessGroup(A); break;
    case 41: Result = CreateSocket(A, B, C); break;
    case 42: Result = ConnectSocket(A, B, C); break;
    case 43: Result = AcceptSocket(A, B, C, 0); break;
    case 44: case 45: Result = SocketTransfer(Registers[REG_RAX] == 44, A, B, C, Registers[REG_R10],
                                               Registers[REG_R8], Registers[REG_R9]); break;
    case 46: case 47: Result = SocketMessage(Registers[REG_RAX] == 46, A, B, C); break;
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
    case 62: Result = SignalProcess(A, B); break;
    case 73: Result = FlockGuest(A, B); break;
    case 74: case 75: Result = SyncDescriptor(A); break;
    case 76: Result = TruncatePath(A, B); break;
    case 78: Result = ReadDirents(A, B, C, false); break;
    case 79: Result = GetCwd(A, B); break;
    case 80: Result = ChangeDirectory(A); break;
    case 81: Result = ChangeDirectoryDescriptor(A); break;
    case 82: Result = RenameSdEntry(CurrentDirectory, A, CurrentDirectory, B, 0); break;
    case 83: Result = CreateDirectoryGuest(CurrentDirectory, A, B); break;
    case 84: Result = RemoveDirectoryGuest(CurrentDirectory, A); break;
    case 86: Result = LinkGuest(CurrentDirectory, A, CurrentDirectory, B, 0); break;
    case 265: Result = LinkGuest(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 88: Result = CreateSymlink(A, CurrentDirectory, B); break;
    case 89: Result = ReadSymlink(CurrentDirectory, A, B, C); break;
    case 266: Result = CreateSymlink(A, B, C); break;
    case 267: Result = ReadSymlink(A, B, C, Registers[REG_R10]); break;
    case 87: Result = UnlinkGuest(CurrentDirectory, A); break;
    case 85: Result = OpenGuest(CurrentDirectory, A, 01101, B); break; // creat
    case 90: Result = ChangeModeAt(CurrentDirectory, A, B, 0); break;
    case 91: Result = ChangeModeDescriptor(A, B); break;
    case 92: Result = ChangeOwner(CurrentDirectory, A, B, C, 0); break;
    case 93: Result = ChangeOwner(A, 0, B, C, 0x1000, true); break;
    case 94: Result = ChangeOwner(CurrentDirectory, A, B, C, 0x100); break;
    case 260: Result = ChangeOwner(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 95: Result = std::exchange(Task->CreationMask, A & 0777); break;
    case 97: Result = LimitGuest(A, B, 0); break;
    case 99: Result = SystemInfo(A); break;
    case 102: Result = Task->UserIDs[0]; break;
    case 104: Result = Task->GroupIDs[0]; break;
    case 107: Result = Task->UserIDs[1]; break;
    case 108: Result = Task->GroupIDs[1]; break;
    case 115: Result = GuestGroups(false, A, B); break;
    case 116: Result = GuestGroups(true, A, B); break;
    case 117: Result = SetGuestIDs(Task->UserIDs, A, B, C); break;
    case 118: Result = GetGuestIDs(Task->UserIDs, A, B, C); break;
    case 119: Result = SetGuestIDs(Task->GroupIDs, A, B, C); break;
    case 120: Result = GetGuestIDs(Task->GroupIDs, A, B, C); break;
    case 157: Result = GuestPrivilegeControl(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 122: Result = SetFilesystemID(Task->UserIDs, A); break;
    case 123: Result = SetFilesystemID(Task->GroupIDs, A); break;
    case 109: Result = SetProcessGroup(A, B); break;
    case 133: Result = MakeNode(CurrentDirectory, A, B); break;
    case 259: Result = MakeNode(A, B, C); break;
    case 132: Result = !A ? Fault : UpdateFileTimes(CurrentDirectory, A, B, 0, TimeInput::Seconds); break;
    case 235: Result = !A ? Fault : UpdateFileTimes(CurrentDirectory, A, B, 0, TimeInput::Microseconds); break;
    case 261: Result = UpdateFileTimes(A, B, C, 0, TimeInput::Microseconds); break;
    case 280: Result = UpdateFileTimes(A, B, C, Registers[REG_R10], TimeInput::Nanoseconds); break;
    case 131: Result = AltStack(A, B); break;
    case 137: Result = StatFilesystemPath(A, B); break;
    case 138: Result = StatFilesystemDescriptor(A, B); break;
    case 162: {
      if (SyncSdFilesystem() < 0) WriteLog("Guest sync could not flush the SD filesystem.");
      Result = 0;
      break;
    }
    case 306: Result = SyncFilesystemDescriptor(A); break;
    case 158: Result = ArchPrctl(Frame, A, B); break;
    case 187: Result = ReadaheadGuest(A, B, C); break;
    case 221: Result = AdviseFile(A, B, C, Registers[REG_R10]); break;
    case 186: Result = Current->TID; break;
    case 188: case 189: case 190:
    case 191: case 192: case 193:
    case 194: case 195: case 196:
    case 197: case 198: case 199:
      Result = LegacyXattr(Registers[REG_RAX], A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 463: case 464:
      Result = XattrWithArgs(Registers[REG_RAX] == 463, A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]); break;
    case 465: Result = Xattr(XattrOp::List, A, B, C, 0, Registers[REG_R10], Registers[REG_R8], 0); break;
    case 466: Result = Xattr(XattrOp::Remove, A, B, C, Registers[REG_R10], 0, 0, 0); break;
    case 202: Result = Futex(A, B, C, Registers[REG_R10], Registers[REG_R9]); break;
    case 204: Result = GetAffinity(A, B, C); break;
    case 217: Result = ReadDirents(A, B, C, true); break;
    case 218: Result = SetTidAddress(A); break;
    case 228: Result = ClockGuest(A, B); break;
    case 35: Result = SleepGuest(1, 0, A, B); break;
    case 230: Result = SleepGuest(A, B, C, Registers[REG_R10]); break;
    case 258: Result = CreateDirectoryGuest(A, B, C); break;
    case 263: Result = C == 0x200 ? RemoveDirectoryGuest(A, B) : C ? Invalid : UnlinkGuest(A, B); break;
    case 264: Result = RenameSdEntry(A, B, C, Registers[REG_R10], 0); break;
    case 316: Result = RenameSdEntry(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 262: Result = StatAtGuest(A, B, C, Registers[REG_R10]); break;
    case 268: Result = ChangeModeAt(A, B, C, 0); break;
    case 452: Result = ChangeModeAt(A, B, C, Registers[REG_R10]); break;
    case 269: Result = AccessGuest(A, B, C, 0); break;
    case 270: Result = SelectGuest(A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]); break;
    case 271: Result = PollGuestWithTimespec(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 275: Result = SpliceGuest(A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]); break;
    case 276: Result = SpliceGuest(A, 0, B, 0, C, Registers[REG_R10], true); break;
    case 277: Result = SyncFileRange(A, B, C, Registers[REG_R10]); break;
    case 285: Result = AllocateFileRange(A, B, C, Registers[REG_R10]); break;
    case 278: Result = VmspliceGuest(A, B, C, Registers[REG_R10]); break;
    case 288: Result = AcceptSocket(A, B, C, Registers[REG_R10]); break;
    case 292: Result = A == B || C & ~02000000ULL ? Invalid : DuplicateDescriptor(A, B, (C & 02000000) != 0); break;
    case 293: Result = CreatePipe(A, B); break;
    case 295: case 296: {
      const auto Offset = Registers[REG_R10];
      Result = VectorIO(static_cast<uint32_t>(A), B, static_cast<uint32_t>(C), Registers[REG_RAX] == 296, Offset);
      break;
    }
    case 326: Result = CopyFileRange(A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]); break;
    case 327: case 328: {
      const auto Offset = Registers[REG_R10];
      Result = VectorIO(static_cast<uint32_t>(A), B, static_cast<uint32_t>(C), Registers[REG_RAX] == 328,
                        Offset == UINT64_MAX ? std::nullopt : std::optional {Offset}, Registers[REG_R9]);
      break;
    }
    case 302: Result = A == 0 || A == Task->PID ? LimitGuest(B, Registers[REG_R10], C) : -3; break;
    case 318: Result = RandomGuest(A, B, C); break;
    case 332: Result = StatxGuest(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 436: Result = CloseRange(A, B, C); break;
    case 437: Result = OpenAt2(A, B, C, Registers[REG_R10]); break;
    case 136: Result = UstatGuest(A, B); break;
    case 303: Result = NameToHandle(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 304: Result = OpenByHandle(A, B, C); break;
    case 439: Result = AccessGuest(A, B, C, Registers[REG_R10]); break;
    case FEX_NX2_PRESENT_RGBA: Result = PresentGuestFrame(A); break;
    case FEX_NX2_POLL_INPUT: Result = PollGameInput(); break;
    case FEX_NX2_TICKS_MS: Result = armTicksToNs(armGetSystemTick() - StartTick) / 1'000'000; break;
    case FEX_NX2_DELAY_MS: svcSleepThread(std::min<uint64_t>(A, 1000) * 1'000'000); Result = 0; break;
    case 257: // openat
      Result = OpenGuest(static_cast<uint32_t>(A), B, static_cast<uint32_t>(C), Registers[REG_R10]);
      break;
    }
    if (Result == RetrySyscall || Task->PendingExec) {
      std::longjmp(ReturnToHost, 1);
    }
    constexpr uint64_t NoSignal = 0x4000;
    const auto Call = Registers[REG_RAX];
    const bool SuppressPipeSignal = (Call == 44 && (Registers[REG_R10] & NoSignal)) ||
                                    (Call == 46 && (C & NoSignal)) ||
                                    (Call == 328 && (Registers[REG_R9] & VectorNoSignal));
    if (Result == -32 && !SuppressPipeSignal) QueueGuestSignal(*Current, PipeSignal, Task->PID);
    if (const auto Signal = PendingCaughtSignal(*Current, *Task)) {
      Registers[REG_RAX] = Result;
      Frame->State.rip += 2;
      if (!EnterSignalHandler(Frame, Signal)) TerminateForSignal(11);
      std::longjmp(ReturnToHost, 1);
    }
    if (DeliverDefaultPipeSignal()) std::longjmp(ReturnToHost, 1);
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
  enum class WaitKind { None, Futex, IO, Terminal, Lock, Sleep };
  static constexpr size_t DescriptorCount = 1024; // Linux default RLIMIT_NOFILE and FD_SETSIZE
  struct OpenFile;
  struct DescriptorState {
    std::array<std::shared_ptr<OpenFile>, DescriptorCount> Files {};
    std::array<uint8_t, DescriptorCount> Flags {};
  };
  struct RecordLockWait {
    uint64_t Inode, Start, End;
    int16_t Type;
  };
  struct SavedGuestSignal {
    uint64_t FrameAddress;
    std::array<__uint128_t, 16> YMM;
  };
  struct GuestThread {
    FEXCore::Core::InternalThreadState* Core {};
    std::shared_ptr<DescriptorState> Descriptors;
    std::shared_ptr<OpenFile> PendingFifo;
    std::optional<RecordLockWait> PendingRecordLock;
    std::unique_ptr<void, decltype(&std::free)> ReturnStack {nullptr, &std::free};
    uint64_t ReturnStackTop {};
    uint64_t ClearTid {};
    uint64_t SignalMask {};
    uint64_t PendingSignals {};
    std::array<uint32_t, 64> SignalSenders {};
    std::vector<SavedGuestSignal> SignalFrames;
    std::array<uint64_t, 3> AltStack {0, 2, 0};
    uint64_t Deadline {};
    uint64_t SleepRemaining {}; // Guest timespec for the unslept time, or 0.
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
    Slot->Descriptors = Current && Current->PID == PID ? Current->Descriptors :
                                                        std::move(Processes.at(PID).PendingDescriptors);
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
    if (Thread.Waiting == WaitKind::Lock && PendingCaughtSignal(Thread, Processes.at(Thread.PID))) return true;
    const bool Expired = Thread.Deadline && MonotonicNs() >= Thread.Deadline;
    if (Thread.Waiting == WaitKind::Sleep) {
      if (!Expired) return PendingCaughtSignal(Thread, Processes.at(Thread.PID)) != 0;
      Thread.Core->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX] = 0;
      Thread.Deadline = 0;
      Thread.Waiting = WaitKind::None;
      return true;
    }
    if (Thread.Waiting == WaitKind::Futex && Expired) {
      Thread.Core->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX] = -110;
      Thread.Deadline = 0;
      Thread.Waiting = WaitKind::None;
      return true;
    }
    const bool InputAvailable = Thread.Waiting == WaitKind::Terminal && TerminalClosed;
    if ((Thread.Waiting == WaitKind::IO || Thread.Waiting == WaitKind::Terminal || Thread.Waiting == WaitKind::Lock) &&
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

  static uint64_t RealTimeNs() { return time(nullptr) * 1'000'000'000ULL; }

  // nanosleep and clock_nanosleep. The thread waits for a monotonic deadline
  // and the syscall completes from the scheduler. Absolute CLOCK_REALTIME and
  // CLOCK_TAI deadlines are converted once, so later clock changes do not
  // move them.
  int64_t SleepGuest(uint64_t Clock, uint64_t Flags, uint64_t Request, uint64_t Remaining) {
    constexpr uint64_t AbsoluteTime = 1; // TIMER_ABSTIME; Linux ignores other flag bits.
    // Error order follows Linux: unsupported clocks fail before the request
    // is read, alarm clocks after it is validated.
    const bool Alarm = Clock == 8 || Clock == 9; // REALTIME_ALARM, BOOTTIME_ALARM
    switch (Clock) {
      case 0: case 1: case 7: case 11: case 8: case 9: break; // REALTIME, MONOTONIC, BOOTTIME, TAI, alarms
      case 3: case 4: case 5: case 6: return -95; // THREAD_CPUTIME, MONOTONIC_RAW, coarse clocks
      case 2: return -95; // PROCESS_CPUTIME: not implemented, see FilesystemInventory.md.
      default: return Invalid;
    }
    if (Available(Request, PF_R) < 16) return Fault;
    uint64_t Duration {};
    if (!ReadTimespec(Request, Duration)) return Invalid;
    // CAP_WAKE_ALARM is root-only here; Horizon offers no RTC wakeup source.
    if (Alarm) return Task->UserIDs[1] ? -1 : -95;
    const auto Now = MonotonicNs();
    uint64_t Deadline {};
    if (Flags & AbsoluteTime) {
      const auto ClockNow = Clock == 0 || Clock == 11 ? RealTimeNs() : Now;
      if (Duration <= ClockNow) return 0;
      Deadline = AddDeadline(Now, Duration - ClockNow);
      Remaining = 0; // Absolute sleeps never report the unslept time.
    } else {
      if (!Duration) return 0;
      Deadline = AddDeadline(Now, Duration);
    }
    Current->Deadline = Deadline;
    Current->SleepRemaining = Remaining;
    Current->Waiting = WaitKind::Sleep;
    Current->Core->CurrentFrame->State.rip += 2;
    return RetrySyscall;
  }

  // A caught signal ends a sleep with EINTR, whether or not the handler has
  // SA_RESTART, and a relative sleep stores the time left.
  void InterruptSleep(GuestThread& Thread) {
    auto& Result = Thread.Core->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX];
    Result = -4;
    if (Thread.SleepRemaining) {
      const auto Now = MonotonicNs();
      const auto Left = Thread.Deadline > Now ? Thread.Deadline - Now : 0;
      const int64_t Time[] {static_cast<int64_t>(Left / 1'000'000'000), static_cast<int64_t>(Left % 1'000'000'000)};
      if (CopyOut(Thread.SleepRemaining, Time)) Result = Fault;
    }
    Thread.SleepRemaining = 0;
    Thread.Deadline = 0;
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
    Current->Descriptors.reset();
    Current->PendingFifo.reset();
    ++IOEpoch;
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
  static constexpr uint64_t NoDescriptor = UINT64_MAX;
  static constexpr uint32_t NoAtimeFlag = 01000000, DataSyncFlag = 010000, SyncFlag = 04000000 | DataSyncFlag;
  static constexpr uint32_t VectorNoSignal = 0x100; // RWF_NOSIGNAL
  enum class AppendMode { Descriptor, Force, Suppress };
  static constexpr uint32_t PathOnlyFlag = 010000000; // O_PATH
  enum class FileLock : uint8_t { None, Shared, Exclusive };
  using StreamSocket = Switch2Guest::StreamSocket<OpenFile>;
  struct FifoState {
    std::shared_ptr<StreamSocket> Reader {std::make_shared<StreamSocket>()};
    std::shared_ptr<StreamSocket> Writer {std::make_shared<StreamSocket>()};
    unsigned Readers {}, Writers {};
    uint64_t WriterGeneration {};

    FifoState() {
      Reader->Pipe = Writer->Pipe = true;
      Reader->Connect(Writer);
      Writer->Connect(Reader);
      Reader->ShutRead = true;
      Writer->ShutWrite = true;
    }
  };

  // Linux access mode 3 checks read and write permission at open but
  // grants neither read nor write access to the descriptor.
  static constexpr bool ReadAccess(uint64_t Flags) { return (Flags & 3) == 0 || (Flags & 3) == 2; }
  static constexpr bool WriteAccess(uint64_t Flags) { return (Flags & 3) == 1 || (Flags & 3) == 2; }

  struct OpenFile {
    ~OpenFile() {
      if (Fifo) {
        if (ReadAccess(StatusFlags) && --Fifo->Readers == 0) Fifo->Reader->ShutRead = true;
        if (WriteAccess(StatusFlags) && --Fifo->Writers == 0) Fifo->Writer->ShutWrite = true;
      }
    }
    ConsoleStream Console {};
    s64 Offset {};
    bool RealFile {};
    bool TemporaryLinkable {};
    bool TemporaryPublished {};
    bool RemovedDirectory {};
    bool DirectorySnapshot {};
    bool Pipe {};
    std::string_view Contents;
    std::string Path;
    uint64_t Inode {};
    uint32_t StatusFlags {};
    bool PathOnly() const { return StatusFlags & PathOnlyFlag; }
    FileLock Lock {};
    uint8_t ReadAdvice {};
    std::optional<std::vector<DirectoryEntry>> Directory;
    std::shared_ptr<StreamSocket> Socket;
    std::shared_ptr<FifoState> Fifo;
    bool FifoPartnerSeen {};
    uint64_t InitialWriterGeneration {};
    StreamSocket& WriteEndpoint() const { return *(Fifo ? Fifo->Writer : Socket); }
    bool Nonblocking() const { return StatusFlags & 04000; }
    std::shared_ptr<GuestMemoryFile> Memory;
  };
  using SignalTable = std::array<std::array<uint64_t, 4>, 64>;
  struct ExecImage {
    uint64_t Inode {};
    std::optional<uint32_t> EffectiveUID, EffectiveGID;
    ElfImage Program;
    ElfImage Interpreter;
    std::string Path;
    std::vector<std::string> Arguments;
    std::vector<std::string> Environment;
  };

  struct GuestProcess {
    GuestAddressSpace Memory;
    std::vector<std::weak_ptr<StreamSocket::PinnedPage>> PrivatePipePages;
    std::array<FEXCore::Core::CPUState::gdt_segment, 32> GDT {};
    // Only fork descendants can share a private host address view. Exec
    // builds a fresh address space with independently owned backing.
    std::shared_ptr<const uint8_t> ForkView {std::make_shared<const uint8_t>(0)};
    std::string Folder;
    std::string WorkingDirectory;
    bool RemovedWorkingDirectory {};
    uint32_t CreationMask {0022};
    bool NoNewPrivileges {};
    // Real, effective, saved and filesystem identities.
    std::array<uint32_t, 4> UserIDs {1000, 1000, 1000, 1000};
    std::array<uint32_t, 4> GroupIDs {1000, 1000, 1000, 1000};
    std::vector<uint32_t> SupplementaryGroups;
    std::shared_ptr<DescriptorState> PendingDescriptors {std::make_shared<DescriptorState>()};
    SignalTable SignalActions {};
    uint64_t HeapStart {};
    uint64_t HeapEnd {};
    uint64_t CpuTicks {};
    uint64_t ExecutableInode {};
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
    uint32_t TerminatingSignal {};
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

  void SuspendPipePages(GuestProcess& Process, uint64_t Address = 0, uint64_t Size = UINT64_MAX, bool Detach = false) {
    std::erase_if(Process.PrivatePipePages, [&](const auto& Weak) {
      const auto Pin = Weak.lock();
      if (!Pin) return true;
      const auto Start = reinterpret_cast<uint64_t>(Pin->Live);
      if (Start >= Address + Size || Address >= Start + Pin->Size) return false;
      Pin->Suspend();
      return Detach;
    });
  }

  bool ActivateProcess(uint32_t PID) {
    if (Task->PID != PID) {
      SuspendPipePages(*Task);
      if (!Task->Exited && Task->ForkView.use_count() > 1) {
        if (!Task->Memory.SavePrivatePages(ValidateSnapshotRange)) return false;
        Task->RestorePending = true;
      }
      Task = &Processes.at(PID);
      if (Task->RestorePending) {
        Task->Memory.RestorePrivatePages();
        Task->RestorePending = false;
      }
      for (const auto& Weak : Task->PrivatePipePages) {
        if (const auto Pin = Weak.lock()) Pin->Suspended = false;
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
    Process.PendingDescriptors.reset();
    for (auto It = FlockHolders.begin(); It != FlockHolders.end();) {
      std::erase_if(It->second, [](const auto& Weak) { return Weak.expired(); });
      if (It->second.empty()) It = FlockHolders.erase(It);
      else ++It;
    }
    SuspendPipePages(Process, 0, UINT64_MAX, true);
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
    Child.CreationMask = Task->CreationMask;
    Child.NoNewPrivileges = Task->NoNewPrivileges;
    Child.UserIDs = Task->UserIDs;
    Child.GroupIDs = Task->GroupIDs;
    Child.SupplementaryGroups = Task->SupplementaryGroups;
    Child.RemovedWorkingDirectory = Task->RemovedWorkingDirectory;
    Child.WorkingDirectory = Task->WorkingDirectory;
    Child.Memory = Task->Memory;
    Child.GDT = Task->GDT;
    Child.ForkView = Task->ForkView;
    Child.RestorePending = true;
    *Child.PendingDescriptors = Descriptors();
    Child.SignalActions = Task->SignalActions;
    Child.HeapStart = Task->HeapStart;
    Child.HeapEnd = Task->HeapEnd;
    // Keep the source process's ELF backing alive even if it execs or exits.
    Child.InheritedImages = Task->InheritedImages;
    Child.ExecutableInode = Task->ExecutableInode;
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
      if (Status) CopyOut(Status, Child.TerminatingSignal ? Child.TerminatingSignal : Child.Status << 8);
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
      const auto Text = GuestString(StringAddress, 32 * 4096, -7); // MAX_ARG_STRLEN
      if (!Text) return Text.Error;
      if (Text->size() + 1 > Budget) return -7;
      Budget -= Text->size() + 1;
      Strings.emplace_back(*Text);
    }
    return -7;
  }

  int64_t ExecGuest(uint64_t PathAddress, uint64_t Argv, uint64_t Envp) {
    const auto Name = GuestString(PathAddress);
    if (!Name) return Name.Error;
    std::string Path;
    LinuxStat Info {};
    if (const auto Status = PathStat(CurrentDirectory, *Name, Info, &Path); Status < 0) return Status;
    if ((Info.Mode & 0170000u) != 0100000u || !InodeAllows(Info.Inode, Info.Mode, 1)) return -13;
    if (ExecutableHasWriter(Info.Inode)) return -26;

    ExecImage Request;
    Request.Inode = Info.Inode;
    Request.Path = *Name;
    if (!Task->NoNewPrivileges) {
      if (Info.Mode & 04000) Request.EffectiveUID = Info.Uid;
      if ((Info.Mode & 02010) == 02010) Request.EffectiveGID = Info.Gid;
    }
    size_t Budget = 128 * 1024;
    if (const auto Result = CopyExecVector(Argv, Request.Arguments, Budget); Result < 0) return Result;
    if (const auto Result = CopyExecVector(Envp, Request.Environment, Budget); Result < 0) return Result;
    if (ReadElf(FileBackingPath(Path).c_str(), Request.Program, Info.Size, FileExtents(Info.Inode))) return -8;
    if (!Request.Program.Interpreter.empty()) {
      LinuxStat InterpreterInfo {};
      std::string Interpreter;
      if (const auto Error = PathStat(CurrentDirectory, Request.Program.Interpreter, InterpreterInfo, &Interpreter); Error) {
        return Error;
      }
      if ((InterpreterInfo.Mode & 0170000) != 0100000 || Interpreter.empty()) return -13;
      if (ReadElf(FileBackingPath(Interpreter).c_str(), Request.Interpreter, InterpreterInfo.Size, FileExtents(InterpreterInfo.Inode)) ||
          !Request.Interpreter.Interpreter.empty()) return -80; // ELIBBAD
    }
    Task->PendingExec = std::move(Request);
    return 0;
  }

  bool InstallExec() {
    const auto PID = Task->PID;
    auto* OldThread = Current->Core;
    FEXCore::Core::CPUState State {};
    const auto Mask = Current->SignalMask;
    const auto Pending = Current->PendingSignals;
    const auto SignalSenders = Current->SignalSenders;
    Task->PendingDescriptors = Current->Descriptors;

    // The dispatcher is process infrastructure. Keep its native mapping
    // published across exec, but clear all translations for the old image.
    OldThread->CTX->ClearCodeCache(OldThread, false);
    for (auto& Thread : Threads) {
      if (!Thread.Core || Thread.PID != PID) continue;
      Thread.Core->CTX->DestroyThread(Thread.Core);
      Thread = {};
    }
    Current = nullptr;
    SuspendPipePages(*Task, 0, UINT64_MAX, true);
    InitializeExecCPU(*Task, State);
    Task->Memory = {};
    Task->ForkView = std::make_shared<const uint8_t>(0);
    Task->RestorePending = false;
    Task->HeapStart = Task->HeapEnd = 0;
    Task->Image = std::make_shared<ExecImage>(std::move(*Task->PendingExec));
    Task->ExecutableInode = Task->Image->Inode;
    Task->PendingExec.reset();
    Task->InheritedImages.clear();
    for (size_t FD = 0; FD < Descriptors().Files.size(); ++FD) {
      if (Descriptors().Flags[FD]) CloseGuest(FD);
    }
    for (auto& Action : Task->SignalActions) {
      if (Action[0] != 1) Action = {};
    }
    ++IOEpoch;

    const auto& Image = *Task->Image;
    Task->UserIDs[1] = Image.EffectiveUID.value_or(Task->UserIDs[1]);
    Task->GroupIDs[1] = Image.EffectiveGID.value_or(Task->GroupIDs[1]);
    Task->UserIDs[2] = Task->UserIDs[3] = Task->UserIDs[1];
    Task->GroupIDs[2] = Task->GroupIDs[3] = Task->GroupIDs[1];
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
      BuildInitialStack(Base, Image.Path, Image.Arguments, Image.Environment, &Image.Program, Image.Interpreter.Base,
                        {Task->UserIDs[0], Task->UserIDs[1], Task->GroupIDs[0], Task->GroupIDs[1]});
    if (!AddThread(*Task->Context, State, PID, 0, PID)) return false;
    const auto Thread = std::find_if(Threads.begin(), Threads.end(), [PID](const auto& Entry) { return Entry.TID == PID; });
    Thread->SignalMask = Mask;
    Thread->PendingSignals = Pending;
    Thread->SignalSenders = SignalSenders;
    Task->HasDispatched = false;
    Task->TraceFirstDispatch = true;
    Task->StartupSyscallCheckpoints = Logging == LogLevel::Trace ? 12 : 0;
    WriteLog(("Guest exec " + Image.Path).c_str());
    return true;
  }

  bool IsUnlinked(uint64_t Inode) const {
    const auto Metadata = Inodes.find(Inode);
    return Metadata != Inodes.end() && Metadata->second.Unlinked;
  }

  enum class TimeInput { Seconds, Microseconds, Nanoseconds };

  static FileTimestamp WallClock() {
    struct timespec Now {};
    if (clock_gettime(CLOCK_REALTIME, &Now) != 0) Now.tv_sec = time(nullptr);
    return {Now.tv_sec, Now.tv_nsec};
  }

  void ImportTimestamps(FsFileSystem& SdCard, const std::string& Path, uint64_t Inode) {
    auto& Times = Inodes[Inode].Times;
    if (Times) return;
    FsTimeStampRaw Raw {};
    if (R_SUCCEEDED(fsFsGetFileTimeStampRaw(&SdCard, Path.c_str(), &Raw)) && Raw.is_valid) {
      Times = InodeTimestamps {
        {static_cast<int64_t>(Raw.accessed), 0}, {static_cast<int64_t>(Raw.modified), 0},
        {static_cast<int64_t>(Raw.modified), 0}};
    }
  }

  void MarkInodeChanged(uint64_t Inode, bool Modified = false) {
    const auto Now = WallClock();
    auto& Times = Inodes[Inode].Times;
    auto& Stamps = Times ? *Times : Times.emplace();
    Stamps.Change = Now;
    if (Modified) {
      Stamps.Modify = Now;
      InvalidateReadCache(Inode);
    }
    if (Inode >= InodeSnapshot::FirstInode) InodeStoreDirty = true;
  }

  void MarkFileDataChanged(uint64_t Inode) {
    auto& Record = Inodes[Inode];
    if (Record.Mode && Task->UserIDs[3]) {
      // An unprivileged writer loses setuid and executable setgid.
      const uint32_t Clear = 04000 | ((*Record.Mode & 0010) ? 02000 : 0);
      *Record.Mode &= ~Clear;
    }
    if (Inode >= InodeSnapshot::FirstInode) DirtyFileData.insert(Inode);
    MarkInodeChanged(Inode, true);
  }

  void MarkInodeAccessed(uint64_t Inode) {
    const auto Now = WallClock();
    auto& Times = Inodes[Inode].Times;
    auto& Stamps = Times ? *Times : Times.emplace();
    const auto AtOrBefore = [](FileTimestamp A, FileTimestamp B) {
      return A.Seconds < B.Seconds || (A.Seconds == B.Seconds && A.Nanoseconds <= B.Nanoseconds);
    };
    if (AtOrBefore(Stamps.Access, Stamps.Modify) || AtOrBefore(Stamps.Access, Stamps.Change) ||
        Stamps.Access.Seconds < Now.Seconds - 86400) {
      Stamps.Access = Now;
      if (Inode >= InodeSnapshot::FirstInode) InodeStoreDirty = true;
    }
  }

  struct LinuxStat {
    uint64_t Device, Inode, Links;
    uint32_t Mode, Uid, Gid, Pad;
    uint64_t Rdev;
    int64_t Size, BlockSize, Blocks;
    InodeTimestamps Times;
    int64_t Reserved[3];
  };
  static_assert(sizeof(LinuxStat) == 144);
  static constexpr uint64_t ProcDevice = 2, DevDevice = 3;

  static constexpr uint64_t CurrentDirectory = static_cast<uint64_t>(-100);
  static bool IsCwd(uint64_t FD) { return FD == CurrentDirectory || FD == static_cast<uint32_t>(CurrentDirectory); }
  DescriptorState& Descriptors() const {
    return Current ? *Current->Descriptors : *Task->PendingDescriptors;
  }

  template<typename Callback>
  void ForEachOpenFile(Callback Visit) {
    for (const auto& Thread : Threads) {
      if (!Thread.Descriptors) continue;
      for (const auto& File : Thread.Descriptors->Files) {
        if (File && !File->RealFile) Visit(*File);
      }
    }
    // A real file can be held only by a queued SCM_RIGHTS message.
    // Keep it visible to inode operations without extending its lifetime.
    for (const auto& Entry : DiskFiles) {
      if (const auto File = Entry.lock()) Visit(*File);
    }
  }

  OpenFile* GetFile(uint64_t FD) {
    return FD < Descriptors().Files.size() ? Descriptors().Files[FD].get() : nullptr;
  }

  bool ExecutableHasWriter(uint64_t Inode) {
    return std::any_of(DiskFiles.begin(), DiskFiles.end(), [Inode](const auto& Entry) {
      const auto File = Entry.lock();
      return File && File->Inode == Inode && !File->PathOnly() && WriteAccess(File->StatusFlags);
    });
  }

  bool InodeExecuting(uint64_t Inode) const {
    return Inode && std::any_of(Processes.begin(), Processes.end(), [Inode](const auto& Entry) {
      return !Entry.second.Exited && Entry.second.ExecutableInode == Inode;
    });
  }

  size_t Available(uint64_t Address, uint32_t Flags) const {
    const auto Range = Task->Memory.Query(Address);
    return (Range.Flags & Flags) == Flags && Range.Size ? Range.Size - (Address - Range.Start) : 0;
  }

  size_t AccessiblePrefix(uint64_t Address, size_t Count, uint32_t Flags) const {
    size_t Bytes {};
    while (Bytes < Count && Address <= UINT64_MAX - Bytes) {
      const auto Range = Available(Address + Bytes, Flags);
      if (!Range) break;
      Bytes += std::min(Range, Count - Bytes);
    }
    return Bytes;
  }

  // A NUL-terminated guest string, or the Linux error for it.
  struct GuestText {
    std::string_view Value;
    int64_t Error {};
    explicit operator bool() const { return !Error; }
    const std::string_view& operator*() const { return Value; }
    const std::string_view* operator->() const { return &Value; }
  };

  // Like getname() and strncpy_from_user: the NUL must appear within
  // Limit bytes (PATH_MAX for paths, counting the NUL). Unreadable memory
  // before the NUL or the limit is EFAULT; no NUL within the limit is
  // TooLong (ENAMETOOLONG for paths).
  GuestText GuestString(uint64_t Address, size_t Limit = GuestPathMax, int64_t TooLong = -36) const {
    const auto* Data = reinterpret_cast<const char*>(Address);
    const size_t Readable = std::min<size_t>(Available(Address, PF_R), Limit);
    const auto* End = Readable ? static_cast<const char*>(std::memchr(Data, 0, Readable)) : nullptr;
    if (End) return {std::string_view {Data, static_cast<size_t>(End - Data)}};
    return {{}, Readable == Limit ? TooLong : Fault};
  }

  static uint64_t VirtualInode(std::string_view Path) {
    uint64_t Hash = 14695981039346656037ULL;
    for (const auto Byte : Path) {
      Hash = (Hash ^ static_cast<uint8_t>(Byte)) * 1099511628211ULL;
    }
    return Hash;
  }

  uint64_t SdInode(std::string_view Path) {
    const auto Existing = SdInodes.find(Path);
    if (Existing != SdInodes.end()) return Existing->second;
    const auto Inode = NextSdInode++;
    SdInodes.emplace(Path, Inode);
    return Inode;
  }

  int64_t GuestPrivilegeControl(uint64_t Operation, uint64_t Value, uint64_t Arg3, uint64_t Arg4, uint64_t Arg5) {
    if (Operation != 38 && Operation != 39) return -38;
    if (Arg3 || Arg4 || Arg5) return Invalid;
    if (Operation == 39) return Value ? Invalid : Task->NoNewPrivileges;
    if (Value != 1) return Invalid;
    Task->NoNewPrivileges = true;
    return 0;
  }

  bool InGuestGroup(uint32_t Group, bool Real = false) const {
    const auto& Extra = Task->SupplementaryGroups;
    return Group == Task->GroupIDs[Real ? 0 : 3] || std::binary_search(Extra.begin(), Extra.end(), Group);
  }

  int64_t GuestGroups(bool Set, uint32_t Count, uint64_t Address) {
    auto& Groups = Task->SupplementaryGroups;
    if (Set && Task->UserIDs[1] != 0) return -1;
    if (!Set && !Count) return Groups.size();
    if (Count > INT32_MAX || (Set ? Count > 65536 : Count < Groups.size())) return Invalid;
    const size_t Bytes = sizeof(uint32_t) * (Set ? Count : Groups.size());
    if (Bytes && AccessiblePrefix(Address, Bytes, Set ? PF_R : PF_W) < Bytes) return Fault;
    if (Set) {
      std::vector<uint32_t> New(Count);
      if (Bytes) std::memcpy(New.data(), reinterpret_cast<const void*>(Address), Bytes);
      if (std::ranges::find(New, UINT32_MAX) != New.end()) return Invalid;
      std::ranges::sort(New);
      Groups.swap(New);
      return 0;
    }
    if (Bytes) std::memcpy(reinterpret_cast<void*>(Address), Groups.data(), Bytes);
    return Groups.size();
  }

  int64_t GetGuestIDs(const std::array<uint32_t, 4>& IDs, uint64_t Real, uint64_t Effective, uint64_t Saved) {
    if (CopyOut(Real, IDs[0]) || CopyOut(Effective, IDs[1]) || CopyOut(Saved, IDs[2])) return Fault;
    return 0;
  }

  int64_t SetGuestIDs(std::array<uint32_t, 4>& IDs, uint32_t Real, uint32_t Effective, uint32_t Saved) {
    const std::array Requested {Real, Effective, Saved};
    for (const auto Value : Requested) {
      if (Task->UserIDs[1] != 0 && Value != UINT32_MAX && std::find(IDs.begin(), IDs.begin() + 3, Value) == IDs.begin() + 3) return -1;
    }
    for (size_t I = 0; I < Requested.size(); ++I) {
      if (Requested[I] != UINT32_MAX) IDs[I] = Requested[I];
    }
    IDs[3] = IDs[1];
    return 0;
  }

  uint32_t SetFilesystemID(std::array<uint32_t, 4>& IDs, uint32_t Value) {
    const auto Previous = IDs[3];
    if (Value != UINT32_MAX && (Task->UserIDs[1] == 0 || std::find(IDs.begin(), IDs.end(), Value) != IDs.end())) IDs[3] = Value;
    return Previous;
  }

  std::pair<uint32_t, uint32_t> InodeOwner(uint64_t Inode) const {
    const auto Record = Inodes.find(Inode);
    if (Record != Inodes.end() && Record->second.Owner) return *Record->second.Owner;
    return {1000, 1000};
  }

  bool OwnsInodeOrRoot(uint64_t Inode) const {
    return !Task->UserIDs[3] || InodeOwner(Inode).first == Task->UserIDs[3];
  }

  bool InodeAllows(uint64_t Inode, uint32_t Mode, unsigned Requested, bool Real = false) const {
    const auto [UID, GID] = InodeOwner(Inode);
    const auto User = Task->UserIDs[Real ? 0 : 3];
    if (User == 0) return !(Requested & 1) || (Mode & 0170000) == 0040000 || (Mode & 0111);
    const auto Record = Inodes.find(Inode);
    if (Record != Inodes.end()) {
      const auto ACL = Record->second.Attributes.find(AccessAcl);
      if (ACL != Record->second.Attributes.end()) {
        if (const auto Layout = ParsePosixAcl(ACL->second)) {
          return Layout->Allows(ACL->second, UID, GID, User, [&](uint32_t ID) { return InGuestGroup(ID, Real); }, Requested);
        }
      }
    }
    const auto Shift = UID == User ? 6 : InGuestGroup(GID, Real) ? 3 : 0;
    return ((Mode >> Shift) & Requested) == Requested;
  }

  LinuxStat MakeStat(uint32_t Mode, int64_t Size, uint64_t Inode) {
    LinuxStat Info {};
    Info.Device = 1;
    Info.Inode = Inode;
    if (const auto Existing = Inodes.find(Inode); Existing != Inodes.end() && Existing->second.Times) {
      Info.Times = *Existing->second.Times;
    }
    Info.Links = InodeLinkCount(Inode);
    Info.Mode = Mode;
    const auto [UID, GID] = InodeOwner(Inode);
    Info.Uid = UID;
    Info.Gid = GID;
    Info.Size = Size;
    Info.BlockSize = GuestPageSize;
    Info.Blocks = Size / 512 + (Size % 512 != 0);
    return Info;
  }

  static int64_t DirectoryLinks(FsFileSystem& SdCard, const std::string& Path) {
    FsDir Directory {};
    const auto Opened = SdFileError(fsFsOpenDirectory(&SdCard, Path.c_str(), FsDirOpenMode_ReadDirs | FsDirOpenMode_NoFileSize, &Directory));
    if (Opened) return Opened;
    s64 Count {};
    const auto Status = SdFileError(fsDirGetEntryCount(&Directory, &Count));
    fsDirClose(&Directory);
    return Status ? Status : 2 + Count;
  }

  template<typename Operation>
  int64_t WithDiskFile(const OpenFile& File, uint32_t Mode, Operation Use) {
    const auto Node = Inodes.find(File.Inode);
    const auto Path = Node != Inodes.end() && Node->second.ManagedFile ? ObjectPath(Node->second.BackingObject.value_or(File.Inode)) : File.Path;
    FsFileSystem SdCard {};
    if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
    FsFile Native {};
    const auto Status = fsFsOpenFile(&SdCard, Path.c_str(), Mode, &Native);
    fsFsClose(&SdCard);
    if (R_FAILED(Status)) return SdFileError(Status);
    const auto Result = Use(Native);
    fsFileClose(&Native);
    return Result;
  }

  std::optional<uint64_t> LogicalSize(uint64_t Inode) const {
    const auto It = Inodes.find(Inode);
    return It == Inodes.end() ? std::nullopt : It->second.LogicalSize;
  }

  const PackedFileExtents* FileExtents(uint64_t Inode) const {
    const auto Entry = Inodes.find(Inode);
    return Entry != Inodes.end() && Entry->second.Extents ? &*Entry->second.Extents : nullptr;
  }

  size_t BoundDiskRead(uint64_t Inode, uint64_t Offset, size_t Count) const {
    const auto Size = LogicalSize(Inode);
    if (!Size) return Count;
    return Offset >= *Size ? 0 : std::min<uint64_t>(Count, *Size - Offset);
  }

  int64_t DiskSize(const OpenFile& File, s64& Size) {
    if (const auto Logical = LogicalSize(File.Inode)) {
      Size = *Logical;
      return 0;
    }
    return DiskAllocation(File, Size);
  }

  int64_t DiskAllocation(const OpenFile& File, s64& Size) {
    return WithDiskFile(File, FsOpenMode_Read, [&](FsFile& Native) {
      return SdFileError(fsFileGetSize(&Native, &Size));
    });
  }

  int64_t FlushDiskFile(const OpenFile& File) {
    const auto Error = WithDiskFile(File, FsOpenMode_Write, [](FsFile& Native) {
      return SdFileError(fsFileFlush(&Native));
    });
    if (!Error) DirtyFileData.erase(File.Inode);
    return Error;
  }

  static int64_t ZeroDiskRange(FsFile& Native, uint64_t Begin, uint64_t End) {
    const std::array<uint8_t, 4096> Zeros {};
    while (Begin < End) {
      const auto Count = std::min<uint64_t>(Zeros.size(), End - Begin);
      if (const auto Error = SdFileError(fsFileWrite(&Native, Begin, Zeros.data(), Count, FsWriteOption_None)); Error) return Error;
      Begin += Count;
    }
    return 0;
  }

  static int64_t ResizeNativeFile(FsFile& Native, uint64_t Size) {
    s64 Old {};
    if (const auto Error = SdFileError(fsFileGetSize(&Native, &Old)); Error) return Error;
    if (Size <= static_cast<uint64_t>(Old)) return SdFileError(fsFileSetSize(&Native, Size));
    if (const auto Error = ZeroDiskRange(Native, Old, Size); Error) {
      fsFileSetSize(&Native, Old);
      return Error;
    }
    return 0;
  }

  template<typename Overlay>
  int64_t ReplacePackedBacking(const OpenFile& File, const PackedFileExtents& Source,
                               PackedFileExtents Destination, uint64_t Size, Overlay Update,
                               uint64_t ShiftBoundary = INT64_MAX, int64_t SourceShift = 0) {
    if (!InodeStoreAvailable || NextSdInode == UINT64_MAX) return -5;
    auto& Record = Inodes.at(File.Inode);
    const auto PreviousObject = Record.BackingObject.value_or(File.Inode);
    const auto PreviouslyRetired = RetiredBackingObjects;
    const auto Candidate = NextSdInode++;

    // Reserve the object number in a durable snapshot before creating it.
    // Recovery can reclaim an abandoned candidate with the same rules as
    // any other retired backing.
    RetiredBackingObjects.insert(Candidate);
    InodeStoreDirty = true;
    FsFileSystem SdCard {};
    if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
    const auto Commit = [&] {
      if (const auto Error = SaveInodeStore(); Error) return Error;
      return SdFileError(fsFsCommit(&SdCard));
    };
    auto Error = Commit();
    const auto Path = ObjectPath(Candidate);
    if (!Error) Error = SdFileError(fsFsCreateFile(&SdCard, Path.c_str(), 0, 0));
    if (!Error) {
      FsFile Output {};
      Error = SdFileError(fsFsOpenFile(&SdCard, Path.c_str(), FsOpenMode_Write | FsOpenMode_Append, &Output));
      if (!Error) {
        Error = WithDiskFile(File, FsOpenMode_Read, [&](FsFile& Input) {
          std::array<char, 4096> Scratch;
          return Source.CopyTo(Destination, Scratch,
            [&](uint64_t Offset, std::span<char> Chunk) -> int64_t {
              u64 Count {};
              const auto ReadError = SdFileError(fsFileRead(&Input, Offset, Chunk.data(), Chunk.size(), FsReadOption_None, &Count));
              return ReadError ? ReadError : static_cast<int64_t>(Count);
            },
            [&](uint64_t Offset, std::span<const char> Chunk) {
              return SdFileError(fsFileWrite(&Output, Offset, Chunk.data(), Chunk.size(), FsWriteOption_None));
            }, Update, ShiftBoundary, SourceShift);
        });
        if (!Error) Error = SdFileError(fsFileFlush(&Output));
        fsFileClose(&Output);
      }
    }
    if (!Error) Error = SdFileError(fsFsCommit(&SdCard));
    if (!Error) {
      const auto OldBacking = Record.BackingObject;
      const auto OldSize = Record.LogicalSize;
      auto OldExtents = std::move(Record.Extents);
      Record.BackingObject = Candidate;
      Record.LogicalSize = Size;
      Record.Extents = std::move(Destination);
      RetiredBackingObjects.erase(Candidate);
      RetiredBackingObjects.insert(PreviousObject);
      InodeStoreDirty = true;
      Error = Commit();
      if (Error) {
        Record.BackingObject = OldBacking;
        Record.LogicalSize = OldSize;
        Record.Extents = std::move(OldExtents);
        RetiredBackingObjects.erase(PreviousObject);
        RetiredBackingObjects.insert(Candidate);
        // A failed commit can leave either checkpoint on disk. Keep both
        // backing objects and let startup choose the authoritative snapshot.
        InodeStoreAvailable = false;
      } else {
        // These objects were retired in both committed snapshots. The
        // current operation's previous object still belongs to one slot.
        std::vector<uint64_t> Removed;
        for (const auto Object : PreviouslyRetired) {
          const auto Delete = SdFileError(fsFsDeleteFile(&SdCard, ObjectPath(Object).c_str()));
          if (!Delete || Delete == -2) Removed.push_back(Object);
        }
        if (!Removed.empty() && R_SUCCEEDED(fsFsCommit(&SdCard))) {
          for (const auto Object : Removed) RetiredBackingObjects.erase(Object);
          InodeStoreDirty = true;
        }
      }
    }
    fsFsClose(&SdCard);
    if (!Error) InvalidateReadCache(File.Inode);
    return Error;
  }

  static PackedFileExtents AllocatePackedRange(const PackedFileExtents& Extents, uint64_t Begin, uint64_t End) {
    const auto FirstBlock = Begin - Begin % GuestPageSize;
    const auto AfterBlock = std::min<uint64_t>(INT64_MAX, End + (GuestPageSize - End % GuestPageSize) % GuestPageSize);
    return Extents.Allocate(FirstBlock, AfterBlock);
  }

  int64_t ResizeDiskInode(FsFile& Native, uint64_t Inode, uint64_t Size, bool Truncate = false) {
    const auto OldLogical = LogicalSize(Inode);
    if (!OldLogical) return ResizeNativeFile(Native, Size);
    if (Truncate && Size <= *OldLogical) {
      // Commit the shorter EOF before releasing backing bytes, so recovery
      // never exposes an EOF beyond the physical file after an interruption.
      auto& Record = Inodes.at(Inode);
      const bool PreviousDirty = InodeStoreDirty;
      Record.LogicalSize = Size;
      InodeStoreDirty = true;
      if (!Record.Unlinked) {
        if (const auto Error = SaveInodeStore(); Error) {
          Record.LogicalSize = OldLogical;
          InodeStoreDirty = PreviousDirty;
          return Error;
        }
      }
      s64 Allocated {};
      if (R_FAILED(fsFileGetSize(&Native, &Allocated)) ||
          (Size < static_cast<uint64_t>(Allocated) && R_FAILED(fsFileSetSize(&Native, Size)))) {
        WriteLog("Guest truncate changed logical EOF but could not release native backing space.");
      }
      return 0;
    }
    if (Size > *OldLogical) {
      s64 Allocated {};
      if (const auto Error = SdFileError(fsFileGetSize(&Native, &Allocated)); Error) return Error;
      if (Size > static_cast<uint64_t>(Allocated)) {
        if (const auto Error = ResizeNativeFile(Native, Size); Error) return Error;
      }
      if (const auto Error = ZeroDiskRange(Native, *OldLogical, Size); Error) return Error;
    }
    Inodes.at(Inode).LogicalSize = Size;
    InodeStoreDirty = true;
    return 0;
  }

  int64_t ResizeDiskFile(const OpenFile& File, uint64_t Size) {
    InvalidateReadCache(File.Inode);
    if (const auto* Extents = FileExtents(File.Inode)) {
      const auto OldSize = *LogicalSize(File.Inode);
      if (Size <= OldSize) {
        return ReplacePackedBacking(File, *Extents, Extents->Remove(Size, INT64_MAX), Size,
                                    [](uint64_t, std::span<char>) {});
      }
      Inodes.at(File.Inode).LogicalSize = Size;
      InodeStoreDirty = true;
      return 0;
    }
    s64 Visible {};
    if (const auto Error = DiskSize(File, Visible); Error) return Error;
    if (Size > static_cast<uint64_t>(Visible)) {
      if (const auto Error = EnsureLogicalDiskSize(File, Visible); Error) return Error;
      const auto Clear = WithDiskFile(File, FsOpenMode_Write | FsOpenMode_Append, [&](FsFile& Native) {
        s64 Allocated {};
        if (const auto Error = SdFileError(fsFileGetSize(&Native, &Allocated)); Error) return Error;
        return ZeroDiskRange(Native, Visible, std::min<uint64_t>(Size, Allocated));
      });
      if (Clear) return Clear;
      Inodes.at(File.Inode).LogicalSize = Size;
      InodeStoreDirty = true;
      return 0;
    }
    return WithDiskFile(File, FsOpenMode_Write | FsOpenMode_Append, [&](FsFile& Native) {
      return ResizeDiskInode(Native, File.Inode, Size, true);
    });
  }

  int64_t EnsureLogicalDiskSize(const OpenFile& File, uint64_t Visible) {
    if (LogicalSize(File.Inode)) return 0;
    const auto Existing = Inodes.find(File.Inode);
    if (Existing == Inodes.end() || !Existing->second.ManagedFile) {
      FsFileSystem SdCard {};
      if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
      const auto Promotion = PromoteSdFile(SdCard, File.Path);
      fsFsClose(&SdCard);
      if (Promotion) return Promotion;
    }
    auto& Record = Inodes.at(File.Inode);
    Record.LogicalSize = Visible;
    if (Record.Unlinked) return 0;
    const bool PreviousDirty = InodeStoreDirty;
    InodeStoreDirty = true;
    if (const auto Error = SaveInodeStore(); Error) {
      Record.LogicalSize.reset();
      InodeStoreDirty = PreviousDirty;
      return Error;
    }
    return 0;
  }

  int64_t FileStat(uint64_t FD, LinuxStat& Info) {
    if (auto* File = GetFile(FD)) {
      s64 Size = File->Memory ? File->Memory->Size : File->Contents.size();
      s64 Allocated {};
      if (File->RealFile) {
        if (const auto Error = DiskAllocation(*File, Allocated); Error) return Error;
        Size = LogicalSize(File->Inode).value_or(Allocated);
      }
      const auto Metadata = Inodes.find(File->Inode);
      const auto ProcTarget = ProcLink(File->Path);
      const auto* Target = !ProcTarget.empty() ? &ProcTarget :
                           Metadata != Inodes.end() && Metadata->second.Symlink ? &*Metadata->second.Symlink : nullptr;
      if (Target) Size = Target->size();
      const auto Special = Metadata == Inodes.end() ? 0 : Metadata->second.SpecialType;
      const auto Mode = Special ? Special : Target ? 0120777 : File->Directory ? 0040755 : File->Pipe ? 0010600 : File->Socket ? 0140777 :
                        File->Console != ConsoleStream::None ? 0020600 : File->Memory ? 0100777 : File->Path == "/dev/null" ? 0020666 : ProcFile(File->Path) ? 0100444 : RegularMode(File->Path);
      Info = MakeStat(ModeForInode(File->Inode, Mode), Size, File->Inode);
      const auto VirtualDir = File->Directory ? VirtualDirectory(File->Path) : std::string_view {};
      if (!VirtualDir.empty()) {
        Info.Device = VirtualDir == "/dev" ? DevDevice : ProcDevice;
        Info.Mode = 0040555;
      }
      if (ProcFile(File->Path) || !ProcTarget.empty()) Info.Device = ProcDevice;
      if (File->Path == "/dev/null" || File->Path == "/dev/tty") Info.Device = DevDevice;
      if (File->RealFile) Info.Blocks = Allocated / 512 + (Allocated % 512 != 0);
      if (File->Memory) Info.Blocks = File->Memory->AllocatedBytes() / 512;
      if (File->RemovedDirectory || IsUnlinked(File->Inode)) {
        Info.Links = 0;
      } else if (!VirtualDir.empty()) {
        Info.Links = 2;
      } else if (File->Directory) {
        int64_t Links = 2;
        if (!IsOverflowDirectory(File->Path)) {
          FsFileSystem SdCard {};
          if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
          Links = DirectoryLinks(SdCard, File->Path);
          fsFsClose(&SdCard);
          if (Links < 0) return Links;
        }
        Info.Links = Links + OverflowSubdirectories(File->Path);
      }
      if (File->Path == "/dev/null") Info.Rdev = 0x103;
      if (File->Console == ConsoleStream::Terminal) Info.Rdev = 0x500;
    } else {
      return BadFile;
    }
    return 0;
  }

  int64_t StatGuest(uint64_t FD, uint64_t Output) {
    LinuxStat Info {};
    const auto Result = FileStat(FD, Info);
    return Result ? Result : CopyOut(Output, Info);
  }

  int64_t StatAtGuest(uint64_t FD, uint64_t Address, uint64_t Output, uint64_t Flags) {
    LinuxStat Info {};
    const auto Result = GetStatAt(FD, Address, Flags, Info);
    return Result ? Result : CopyOut(Output, Info);
  }

  int64_t AccessGuest(uint64_t FD, uint64_t Address, uint64_t Mode, uint64_t Flags) {
    constexpr uint64_t EffectiveAccess = 0x200;
    constexpr uint64_t SupportedFlags = 0x100 | EffectiveAccess | 0x1000;
    if ((Mode & ~7ULL) || (Flags & ~SupportedFlags)) return Invalid;
    // The guest's real and effective credentials are identical.
    LinuxStat Info {};
    const auto Result = GetStatAt(FD, Address, Flags & ~EffectiveAccess, Info);
    if (Result < 0) return Result;
    return InodeAllows(Info.Inode, Info.Mode, Mode, !(Flags & EffectiveAccess)) ? 0 : -13;
  }

  static std::optional<std::string_view> ProcFile(std::string_view Name) {
    if (Name == "/proc/version") return "Linux version 5.0.0 (FEX Switch 2 guest)\n";
    if (!Name.starts_with("/proc/")) return std::nullopt;
    Name.remove_prefix(6);
    const auto Slash = Name.find('/');
    if (Slash == Name.npos) return std::nullopt;
    const auto Process = Name.substr(0, Slash);
    if (Process != "self" && !ProcPID(Process)) return std::nullopt;
    Name.remove_prefix(Slash + 1);
    if (Name == "status") return "Name:\tfex-guest\n";
    if (Name == "mountinfo") return "1 0 0:1 / / rw,relatime - msdos sdmc rw\n";
    if (Name == "mounts") return "sdmc / msdos rw,relatime 0 0\n";
    return std::nullopt;
  }

  static uint32_t ProcPID(std::string_view Name) {
    if (Name.empty() || Name.front() == '0') return 0;
    uint32_t PID {};
    const auto End = Name.data() + Name.size();
    const auto [Next, Error] = std::from_chars(Name.data(), End, PID);
    return Next == End && Error == std::errc {} ? PID : 0;
  }

  std::string ProcLink(std::string_view Path) const {
    if (VirtualMount(Path) != 1) return {};
    Path.remove_prefix(RootPath.size());
    if (Path == "/proc/self") return std::to_string(Task->PID);
    if (Path == "/proc/mounts") return "self/mounts";
    return {};
  }

  int64_t PathStat(uint64_t Directory, std::string_view Name, LinuxStat& Info, std::string* Resolved = nullptr, bool Follow = true) {
    FsFileSystem SdCard {};
    if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
    std::string Path;
    auto Error = ResolveSdPath(Directory, Name, SdCard, Path, Follow);
    if (!Error) {
      if (const auto Target = ProcLink(Path); !Target.empty()) {
        fsFsClose(&SdCard);
        Info = MakeStat(0120777, Target.size(), VirtualInode(std::string_view(Path).substr(RootPath.size())));
        Info.Device = ProcDevice;
        if (Resolved) *Resolved = std::move(Path);
        return 0;
      }
      if (const auto VirtualDir = VirtualDirectory(Path); !VirtualDir.empty()) {
        fsFsClose(&SdCard);
        Info = MakeStat(0040555, 0, VirtualInode(VirtualDir));
        Info.Device = VirtualDir == "/dev" ? DevDevice : ProcDevice;
        Info.Links = 2;
        if (Resolved) *Resolved = std::move(Path);
        return 0;
      }
      if (VirtualMount(Path)) {
        const auto GuestPath = std::string_view(Path).substr(RootPath.size());
        const bool Null = GuestPath == "/dev/null";
        const bool Device = Null || (Interactive && GuestPath == "/dev/tty");
        const auto Contents = ProcFile(GuestPath);
        if (Device || Contents) {
          fsFsClose(&SdCard);
          if (Name.ends_with('/')) return -20;
          Info = MakeStat(Device ? 0020666 : 0100444, Device ? 0 : Contents->size(), VirtualInode(GuestPath));
          Info.Device = Device ? DevDevice : ProcDevice;
          if (Device) Info.Rdev = Null ? 0x103 : 0x500;
          return 0;
        }
      }
      const auto* Target = SymlinkAt(Path);
      const auto Special = SpecialNodeAt(Path);
      if (Target || Special) {
        fsFsClose(&SdCard);
        if (Name.ends_with('/')) return -20;
        const auto Inode = SdInode(Path);
        Info = MakeStat(Special ? ModeForInode(Inode, Special) : 0120777, Target ? Target->size() : 0, Inode);
        if (Resolved) *Resolved = std::move(Path);
        return 0;
      }
    }
    const auto Backing = FileBackingPath(Path);
    const bool Overflow = !Error && IsOverflowDirectory(Path);
    FsDirEntryType Type {};
    if (!Error) Error = EntryType(SdCard, Path, Type);
    s64 Size {};
    int64_t Links = 0;
    if (!Error && Type == FsDirEntryType_File) {
      if (Name.ends_with('/')) {
        Error = -20;
      } else {
        FsFile File {};
        Error = SdFileError(fsFsOpenFile(&SdCard, Backing.c_str(), FsOpenMode_Read, &File));
        if (!Error) {
          Error = SdFileError(fsFileGetSize(&File, &Size));
          fsFileClose(&File);
        }
      }
    }
    if (!Error && Type == FsDirEntryType_Dir) {
      Links = Overflow ? 2 : DirectoryLinks(SdCard, Backing);
      if (Links < 0) Error = Links;
      else Links += OverflowSubdirectories(Path);
    }
    uint64_t Inode {};
    if (!Error) {
      Inode = SdInode(Path);
      if (!Overflow) ImportTimestamps(SdCard, Backing, Inode);
    }
    fsFsClose(&SdCard);
    if (Error) return Error;
    const auto Allocated = Size;
    Size = LogicalSize(Inode).value_or(Size);
    Info = MakeStat(ModeForInode(Inode, Type == FsDirEntryType_Dir ? 0040755 : RegularMode(Path)), Size, Inode);
    if (Type == FsDirEntryType_File) Info.Blocks = Allocated / 512 + (Allocated % 512 != 0);
    if (Links) Info.Links = Links;
    if (Resolved) *Resolved = std::move(Path);
    return 0;
  }

  uint32_t RegularMode(std::string_view Path) const {
    const auto Relative = Path.starts_with(RootPath) ? Path.substr(RootPath.size()) : Path;
    return Relative.starts_with("/bin/") || Relative.starts_with("/usr/bin/") ? 0100755 : 0100644;
  }

  uint32_t ModeForInode(uint64_t Inode, uint32_t Default) const {
    const auto Stored = Inodes.find(Inode);
    if (Stored == Inodes.end() || !Stored->second.Mode) return Default;
    return (Default & 0170000) | *Stored->second.Mode;
  }

  void SetInodeMode(uint64_t Inode, uint32_t Mode) {
    auto& Record = Inodes[Inode];
    if (Task->UserIDs[3] && !InGuestGroup(InodeOwner(Inode).second)) Mode &= ~02000U;
    Record.Mode = Mode & 07777;
    if (auto ACL = Record.Attributes.find(AccessAcl); ACL != Record.Attributes.end()) {
      if (const auto Layout = ParsePosixAcl(ACL->second)) Layout->ApplyMode(ACL->second, Mode, false);
    }
    MarkInodeChanged(Inode);
  }

  void InitializeInodeMode(uint64_t Inode, const std::string& Parent, uint32_t Mode, bool Directory) {
    auto& Record = Inodes[Inode];
    Record.Mode = Mode & 07777 & ~Task->CreationMask;
    const auto ParentInode = Inodes.find(SdInode(Parent));
    if (ParentInode == Inodes.end()) return;
    const auto Default = ParentInode->second.Attributes.find(DefaultAcl);
    if (Default == ParentInode->second.Attributes.end()) return;
    auto Bytes = Default->second;
    if (const auto Layout = ParsePosixAcl(Bytes)) {
      if (Directory) Record.Attributes.emplace(DefaultAcl, Bytes);
      Layout->ApplyMode(Bytes, Mode, true);
      Record.Mode = (Mode & 07000) | Layout->Mode(Bytes);
      if (Layout->Extended) Record.Attributes.emplace(AccessAcl, std::move(Bytes));
    }
  }

  static std::string ParentPath(std::string_view Path) {
    return std::string(Path.substr(0, std::max<size_t>(1, Path.rfind('/'))));
  }

  struct ParentTimestampRollback {
    uint64_t Inode;
    std::optional<InodeTimestamps> Times;
  };

  ParentTimestampRollback UpdateParentTimestamps(std::string_view Parent) {
    const auto Inode = SdInode(Parent);
    ParentTimestampRollback Previous {Inode, Inodes[Inode].Times};
    MarkInodeChanged(Inode, true);
    return Previous;
  }

  void RestoreParentTimestamps(const ParentTimestampRollback& Previous) {
    Inodes[Previous.Inode].Times = Previous.Times;
  }

  int64_t ChangeModeDescriptor(uint64_t FD, uint64_t Mode, bool AllowPath = false) {
    const auto* File = GetFile(FD);
    if (!File || (File->PathOnly() && !AllowPath)) return BadFile;
    const auto Metadata = Inodes.find(File->Inode);
    if (!VirtualDirectory(File->Path).empty()) return -95;
    if (!File->RealFile && !File->Directory && !File->Memory &&
        (Metadata == Inodes.end() || !Metadata->second.SpecialType)) return -95;
    if (!OwnsInodeOrRoot(File->Inode)) return -1;
    if (File->Memory && (File->Memory->Seals & GuestMemoryFile::SealExec) &&
        ((ModeForInode(File->Inode, 0100777) ^ Mode) & 0111)) return -1;
    SetInodeMode(File->Inode, Mode);
    return 0;
  }

  int64_t ChangeModeAt(uint64_t FD, uint64_t Address, uint64_t Mode, uint64_t Flags) {
    if (Flags & ~0x1100ULL) return Invalid;
    const auto Name = GuestString(Address);
    if (!Name) return Name.Error;
    if ((Flags & 0x1000) && Name->empty() && !IsCwd(FD)) return ChangeModeDescriptor(FD, Mode, true);
    LinuxStat Info {};
    std::string Path;
    const auto Error = PathStat(FD, Name->empty() && (Flags & 0x1000) ? "." : *Name, Info, &Path, !(Flags & 0x100));
    if (Error < 0) return Error;
    if (Path.empty() || !VirtualDirectory(Path).empty() || (Info.Mode & 0170000) == 0120000) return -95;
    if (!OwnsInodeOrRoot(Info.Inode)) return -1;
    SetInodeMode(Info.Inode, Mode);
    return 0;
  }

  int64_t ChangeOwner(uint64_t FD, uint64_t Address, uint32_t UID, uint32_t GID, uint64_t Flags, bool Descriptor = false) {
    constexpr uint32_t Unchanged = UINT32_MAX;
    if (Flags & ~0x1100ULL) return Invalid;
    LinuxStat Info {};
    int64_t Error;
    if (Descriptor) {
      const auto* File = GetFile(FD);
      if (!File || File->PathOnly()) return BadFile;
      Error = FileStat(FD, Info);
    } else {
      Error = GetStatAt(FD, Address, Flags, Info);
    }
    if (Error) return Error;
    const auto User = Task->UserIDs[3];
    if (User && (Info.Uid != User || (UID != Unchanged && UID != Info.Uid) ||
                 (GID != Unchanged && GID != Info.Gid && !InGuestGroup(GID)))) return -1;
    if (Info.Inode < InodeSnapshot::FirstInode && (Info.Mode & 0170000) != 0100000) return -1;
    if ((Info.Mode & 0170000) == 0100000) {
      const uint32_t Clear = 04000 | ((Info.Mode & 0010) ? 02000 : 0);
      Inodes[Info.Inode].Mode = Info.Mode & 07777 & ~Clear;
    }
    Inodes[Info.Inode].Owner.emplace(UID == Unchanged ? Info.Uid : UID, GID == Unchanged ? Info.Gid : GID);
    MarkInodeChanged(Info.Inode);
    return 0;
  }

  int64_t GetStatAt(uint64_t FD, uint64_t Address, uint64_t Flags, LinuxStat& Info) {
    if (Flags & ~0x1900ULL) return Invalid;
    constexpr uint64_t EmptyPath = 0x1000;
    const auto Name = !Address && (Flags & EmptyPath) ? GuestText {} : GuestString(Address);
    if (!Name) return Name.Error;
    if (Name->empty() && (Flags & EmptyPath)) {
      return IsCwd(FD) ? PathStat(FD, ".", Info) : FileStat(FD, Info);
    }
    return PathStat(FD, *Name, Info, nullptr, !(Flags & 0x100));
  }

  int64_t UpdateFileTimes(uint64_t FD, uint64_t Address, uint64_t Values, uint32_t Flags, TimeInput Format) {
    constexpr int64_t Now = 0x3fffffff, Omit = 0x3ffffffe;
    FileTimestamp Times[2] {{0, Now}, {0, Now}};
    if (Values) {
      if (Format == TimeInput::Seconds) {
        int64_t Seconds[2];
        if (AccessiblePrefix(Values, sizeof(Seconds), PF_R) != sizeof(Seconds)) return Fault;
        std::memcpy(Seconds, reinterpret_cast<const void*>(Values), sizeof(Seconds));
        Times[0] = {Seconds[0], 0};
        Times[1] = {Seconds[1], 0};
      } else {
        if (AccessiblePrefix(Values, sizeof(Times), PF_R) != sizeof(Times)) return Fault;
        std::memcpy(Times, reinterpret_cast<const void*>(Values), sizeof(Times));
        for (auto& Stamp : Times) {
          if (Format == TimeInput::Microseconds) {
            if (Stamp.Nanoseconds < 0 || Stamp.Nanoseconds >= 1'000'000) return Invalid;
            Stamp.Nanoseconds *= 1000;
          } else if (Stamp.Nanoseconds < 0 ||
                     (Stamp.Nanoseconds >= 1'000'000'000 && Stamp.Nanoseconds != Now && Stamp.Nanoseconds != Omit)) {
            return Invalid;
          }
        }
      }
    }
    if (Times[0].Nanoseconds == Omit && Times[1].Nanoseconds == Omit) return 0;
    if ((Flags & ~0x1100U) || (!Address && Flags)) return Invalid;

    if (!Address) {
      const auto* File = GetFile(FD);
      if (!File || File->PathOnly()) return BadFile;
    }
    LinuxStat Info {};
    const auto Error = Address ? GetStatAt(FD, Address, Flags, Info) : FileStat(FD, Info);
    if (Error < 0) return Error;
    if (!OwnsInodeOrRoot(Info.Inode)) {
      if (Times[0].Nanoseconds != Now || Times[1].Nanoseconds != Now) return -1;
      if (!InodeAllows(Info.Inode, Info.Mode, 2)) return -13;
    }
    const auto Type = Info.Mode & 0170000;
    if (Type != 0100000 && Type != 0040000 && Type != 0120000 && Type != 0010000 && Type != 0140000) return -95;

    auto Stamps = Info.Times;
    const auto Current = WallClock();
    if (Times[0].Nanoseconds != Omit) Stamps.Access = Times[0].Nanoseconds == Now ? Current : Times[0];
    if (Times[1].Nanoseconds != Omit) Stamps.Modify = Times[1].Nanoseconds == Now ? Current : Times[1];
    Stamps.Change = Current;
    Inodes[Info.Inode].Times = Stamps;
    if (Info.Inode >= InodeSnapshot::FirstInode) InodeStoreDirty = true;
    return 0;
  }

  int64_t StatxGuest(uint64_t FD, uint64_t Address, uint64_t Flags, uint64_t Mask, uint64_t Output) {
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
    if ((Mask & 0x80000000ULL) || (Flags & 0x6000ULL) == 0x6000) return Invalid;
    LinuxStat Info {};
    const auto Result = GetStatAt(FD, Address, Flags & ~0x6000ULL, Info);
    if (Result < 0) return Result;
    Statx Out {};
    Out.Access = {Info.Times.Access.Seconds, static_cast<uint32_t>(Info.Times.Access.Nanoseconds), 0};
    Out.Modify = {Info.Times.Modify.Seconds, static_cast<uint32_t>(Info.Times.Modify.Nanoseconds), 0};
    Out.Change = {Info.Times.Change.Seconds, static_cast<uint32_t>(Info.Times.Change.Nanoseconds), 0};
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
    return CopyOut(Output, Out);
  }

  int64_t ReadDirents(uint64_t FD, uint64_t Output, uint64_t Count, bool Is64BitRecord) {
    auto* File = GetFile(FD);
    if (!File || File->PathOnly()) return BadFile;
    if (!File->Directory) return -20;
    Count = static_cast<uint32_t>(Count);
    size_t Used {};
    const auto AvailableOutput = Available(Output, PF_W);
    auto* Bytes = reinterpret_cast<char*>(Output);
    if (File->RemovedDirectory) return -2;
    if (!File->DirectorySnapshot) {
      std::vector<DirectoryEntry> Entries { {".", true}, {"..", true} };
      const auto VirtualDir = VirtualDirectory(File->Path);
      if (VirtualDir == "/proc") {
        Entries.insert(Entries.end(), {{"self", false, true}, {"version", false}, {"mounts", false, true}});
        for (const auto& [PID, Process] : Processes) Entries.push_back({std::to_string(PID), true});
      } else if (VirtualDir.starts_with("/proc/")) {
        Entries.insert(Entries.end(), {{"status", false}, {"mountinfo", false}, {"mounts", false}});
      } else if (VirtualDir == "/dev") {
        Entries.push_back({"null", false, false, 2});
        if (Interactive) Entries.push_back({"tty", false, false, 2});
      } else if (IsOverflowDirectory(File->Path)) {
        // Only the store holds this directory's entries.
      } else if (const auto Status = ReadDirectory(File->Path, Entries); R_FAILED(Status)) {
        return SdFileError(Status);
      }
      if (File->Path == RootPath) {
        std::erase_if(Entries, [](const DirectoryEntry& Entry) { return Entry.Name == "dev" || Entry.Name == "proc"; });
        Entries.insert(Entries.end(), {{"dev", true}, {"proc", true}});
      }
      std::erase_if(Entries, [&](const DirectoryEntry& Entry) {
        return IsInodeStorePath(File->Path + "/" + Entry.Name);
      });
      if (VirtualDir.empty()) AppendGuestDirectoryEntries(File->Path, Entries);
      *File->Directory = std::move(Entries);
      File->DirectorySnapshot = true;
    }
    const auto& Entries = *File->Directory;
    while (static_cast<size_t>(File->Offset) < Entries.size()) {
      const auto& Entry = Entries[File->Offset];
      // Both LP64 records have two 64-bit words and a 16-bit length.
      // getdents64 puts d_type before the name. getdents puts it last.
      constexpr size_t FixedHeaderSize = 18;
      const size_t NameOffset = FixedHeaderSize + (Is64BitRecord ? 1 : 0);
      const uint16_t Length = (FixedHeaderSize + Entry.Name.size() + 2 + 7) & ~7;
      if (Length > Count - Used) {
        if (!Used) return Invalid;
        break;
      }
      if (Length > AvailableOutput - Used) {
        if (!Used) return Fault;
        break;
      }

      std::string Path = File->Path;
      if (Entry.Name == "..") {
        if (Path != RootPath && Path != LaunchFolder && Path != "/") Path.resize(std::max<size_t>(1, Path.rfind('/')));
      } else if (Entry.Name != ".") {
        if (!Path.ends_with('/')) Path += '/';
        Path += Entry.Name;
      }
      const auto Inode = VirtualMount(Path) ? VirtualInode(std::string_view(Path).substr(RootPath.size())) : SdInode(Path);
      const uint64_t Next = ++File->Offset;
      auto* EntryBytes = Bytes + Used;
      std::memset(EntryBytes, 0, Length);
      std::memcpy(EntryBytes, &Inode, 8);
      std::memcpy(EntryBytes + 8, &Next, 8);
      std::memcpy(EntryBytes + 16, &Length, 2);
      EntryBytes[Is64BitRecord ? FixedHeaderSize : Length - 1] = Entry.SpecialType ? Entry.SpecialType : Entry.IsDirectory ? 4 : Entry.IsSymlink ? 10 : 8;
      std::memcpy(EntryBytes + NameOffset, Entry.Name.c_str(), Entry.Name.size() + 1);
      Used += Length;
    }
    if (Used && !(File->StatusFlags & NoAtimeFlag)) MarkInodeAccessed(File->Inode);
    return Used;
  }

  int64_t SeekGuest(uint64_t FD, int64_t Offset, uint64_t Origin) {
    auto* File = GetFile(FD);
    if (!File || File->PathOnly()) return BadFile;
    if (File->Socket || File->Console != ConsoleStream::None) return -29;
    if (Origin > 4) return Invalid;
    if (File->Path == "/dev/null") return 0;
    int64_t Base {};
    if (Origin == 1) Base = File->Offset;
    else if (Origin >= 2) {
      int64_t Size;
      if (File->Directory) {
        Size = Origin == 2 ? File->Directory->size() : INT64_MAX;
      } else {
        if (Origin >= 3 && !File->RealFile && !File->Memory) return Invalid;
        LinuxStat Info {};
        const auto Result = FileStat(FD, Info);
        if (Result < 0) return Result;
        Size = Info.Size;
      }
      if (Origin >= 3) {
        // Memory files and packed SD files track holes. A dense SD file's
        // only hole is the implicit one at EOF.
        if (Offset < 0 || Offset >= Size) return -6; // ENXIO
        if (File->Memory) {
          const auto Found = File->Memory->Seek(Offset, Origin == 3);
          if (!Found) return -6;
          File->Offset = *Found;
          return *Found;
        }
        if (const auto* Extents = FileExtents(File->Inode)) {
          const auto Found = Extents->Seek(Offset, Size, Origin == 3);
          if (!Found) return -6;
          File->Offset = *Found;
          return *Found;
        }
        s64 DataEnd = Size;
        if (File->RealFile && LogicalSize(File->Inode)) {
          if (const auto Error = DiskAllocation(*File, DataEnd); Error) return Error;
          DataEnd = std::min<int64_t>(Size, DataEnd);
        }
        if (Origin == 3 && Offset >= DataEnd) return -6;
        Offset = Origin == 3 ? Offset : std::max(Offset, DataEnd);
      } else {
        Base = Size;
      }
    }
    int64_t Next {};
    if (__builtin_add_overflow(Base, Offset, &Next) || Next < 0) return Invalid;
    File->Offset = Next;
    if (File->Directory && !Next) File->DirectorySnapshot = false;
    return Next;
  }

  int64_t GetCwd(uint64_t Output, uint64_t Size) {
    if (Task->RemovedWorkingDirectory) return -2;
    const auto Bytes = Task->WorkingDirectory.size() + 1;
    if (Size < Bytes) return -34;
    if (Available(Output, PF_W) < Bytes) return Fault;
    std::memcpy(reinterpret_cast<void*>(Output), Task->WorkingDirectory.c_str(), Bytes);
    return Bytes;
  }

  int64_t ChangeDirectory(uint64_t Address) {
    const auto Name = GuestString(Address);
    if (!Name) return Name.Error;
    LinuxStat Info {};
    std::string Path;
    if (const auto Result = PathStat(CurrentDirectory, *Name, Info, &Path); Result < 0) return Result;
    if ((Info.Mode & 0170000) != 0040000) return -20;
    if (!InodeAllows(Info.Inode, Info.Mode, 1)) return -13;
    SetWorkingDirectory(std::move(Path));
    return 0;
  }

  int64_t ChangeDirectoryDescriptor(uint64_t FD) {
    const auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (!File->Directory) return -20;
    if (!InodeAllows(File->Inode, ModeForInode(File->Inode, 0040755), 1)) return -13;
    SetWorkingDirectory(File->Path);
    Task->RemovedWorkingDirectory = File->RemovedDirectory;
    return 0;
  }

  void SetWorkingDirectory(std::string Path) {
    Task->Folder = std::move(Path);
    Task->RemovedWorkingDirectory = false;
    Task->WorkingDirectory = GuestWorkingPath(Task->Folder);
  }

  std::string GuestWorkingPath(const std::string& Path) const {
    if (Path == RootPath) return "/";
    if (PathContains(RootPath, Path)) return Path.substr(RootPath.size());
    return Path;
  }

  static constexpr uint64_t ResolveNoXdev = 1, ResolveNoSymlinks = 4;
  static constexpr uint64_t ResolveBeneath = 8, ResolveInRoot = 16, ResolveCached = 32;

  unsigned VirtualMount(std::string_view Path) const {
    if (!Path.starts_with(RootPath)) return 0;
    Path.remove_prefix(RootPath.size());
    if (PathContains("/proc", Path)) return 1;
    if (PathContains("/dev", Path)) return 2;
    return 0;
  }

  std::string_view VirtualDirectory(std::string_view Path) const {
    if (!VirtualMount(Path)) return {};
    Path.remove_prefix(RootPath.size());
    if (Path == "/dev" || Path == "/proc") return Path;
    if (Path.starts_with("/proc/") && Processes.contains(ProcPID(Path.substr(6)))) return Path;
    return {};
  }

  int64_t ResolveSdPath(uint64_t FD, std::string_view Name, FsFileSystem& SdCard, std::string& Path, bool Follow = true,
                        uint64_t Resolve = 0) {
    if (Name.empty()) return -2;
    if (Name.find_first_of(":\\") != Name.npos) return Invalid;
    const bool Absolute = Name.starts_with('/');
    if (Absolute && (Resolve & ResolveBeneath)) return -18;
    if (Absolute && !(Resolve & ResolveInRoot)) {
      Path = RootPath;
    } else if (IsCwd(FD)) {
      if (Task->RemovedWorkingDirectory) return -2;
      Path = Task->Folder;
    } else {
      const auto* File = GetFile(FD);
      if (!File) return BadFile;
      if (!File->Directory) return -20;
      if (File->RemovedDirectory) return -2;
      Path = File->Path;
    }
    const std::string Scope = Resolve & (ResolveBeneath | ResolveInRoot) ? Path : "";
    return WalkSdPath(Name, SdCard, Path, Follow, 0, Resolve, Scope);
  }

  static constexpr std::string_view InodeObjects = InodeFiles::Objects;

  bool IsManagedFile(std::string_view Path) const {
    const auto Name = SdInodes.find(Path);
    if (Name == SdInodes.end()) return false;
    const auto Metadata = Inodes.find(Name->second);
    return Metadata != Inodes.end() && Metadata->second.ManagedFile;
  }

  uint32_t SpecialNodeAt(std::string_view Path) const {
    const auto Name = SdInodes.find(Path);
    if (Name == SdInodes.end()) return 0;
    const auto Metadata = Inodes.find(Name->second);
    return Metadata == Inodes.end() ? 0 : Metadata->second.SpecialType;
  }

  bool IsVirtualEntry(std::string_view Path) const {
    return SymlinkAt(Path) || IsManagedFile(Path) || SpecialNodeAt(Path);
  }

  // Horizon rejects SD paths longer than this many bytes (2002-6003),
  // measured on console 0.12.35 at every depth and for multibyte names.
  static constexpr size_t SdPathLimit = 257;
  // Linux PATH_MAX, including the terminating NUL.
  static constexpr size_t GuestPathMax = 4096;

  bool IsOverflowDirectory(std::string_view Path) const {
    const auto Name = SdInodes.find(Path);
    if (Name == SdInodes.end()) return false;
    const auto Metadata = Inodes.find(Name->second);
    return Metadata != Inodes.end() && Metadata->second.OverflowDirectory && !Metadata->second.Unlinked;
  }

  // Entries the SD card cannot hold natively: paths beyond SdPathLimit and
  // anything inside an overflow directory. All directories below an
  // overflow directory are overflow directories too, so the parent decides.
  bool NeedsOverflow(std::string_view Path) const {
    return Path.size() > SdPathLimit || IsOverflowDirectory(ParentPath(Path));
  }

  // Store-backed entries of any kind, including overflow directories.
  bool IsStoreEntry(std::string_view Path) const {
    return IsVirtualEntry(Path) || IsOverflowDirectory(Path);
  }

  // Type of a guest entry. Managed files are looked up through their object;
  // overflow directories and paths only the store can hold never reach the
  // SD card, so a missing overflow entry is ENOENT rather than a Horizon error.
  int64_t EntryType(FsFileSystem& SdCard, const std::string& Path, FsDirEntryType& Type) {
    if (IsOverflowDirectory(Path)) {
      Type = FsDirEntryType_Dir;
      return 0;
    }
    if (IsManagedFile(Path)) {
      return SdFileError(fsFsGetEntryType(&SdCard, FileBackingPath(Path).c_str(), &Type));
    }
    if (NeedsOverflow(Path)) return -2;
    return SdFileError(fsFsGetEntryType(&SdCard, Path.c_str(), &Type));
  }

  // Overflow subdirectories directly below Path, for st_nlink.
  int64_t OverflowSubdirectories(const std::string& Path) const {
    const auto Prefix = Path + (Path.ends_with('/') ? "" : "/");
    int64_t Count {};
    for (auto It = SdInodes.lower_bound(Prefix); It != SdInodes.end() && It->first.starts_with(Prefix); ++It) {
      if (std::string_view(It->first).substr(Prefix.size()).find('/') == std::string_view::npos &&
          IsOverflowDirectory(It->first)) ++Count;
    }
    return Count;
  }

  // Whether any store-backed entry exists below Path.
  bool HasStoreChildren(const std::string& Path) const {
    const auto Prefix = Path + "/";
    for (auto It = SdInodes.lower_bound(Prefix); It != SdInodes.end() && It->first.starts_with(Prefix); ++It) {
      if (IsStoreEntry(It->first)) return true;
    }
    return false;
  }

  static std::string ObjectPath(uint64_t Inode) {
    return std::string(InodeObjects) + "/" + std::to_string(Inode);
  }

  std::string FileBackingPath(std::string_view Path) const {
    if (!IsManagedFile(Path)) return std::string(Path);
    const auto Inode = SdInodes.find(Path)->second;
    return ObjectPath(Inodes.at(Inode).BackingObject.value_or(Inode));
  }

  uint64_t InodeLinkCount(uint64_t Inode) const {
    const auto Metadata = Inodes.find(Inode);
    if (Metadata == Inodes.end()) return 1;
    if (Metadata->second.Unlinked) return 0;
    if (!Metadata->second.ManagedFile && !Metadata->second.Symlink && !Metadata->second.SpecialType) return 1;
    return std::count_if(SdInodes.begin(), SdInodes.end(), [Inode](const auto& Name) { return Name.second == Inode; });
  }

  const std::string* SymlinkAt(std::string_view Path) const {
    const auto Name = SdInodes.find(Path);
    if (Name == SdInodes.end()) return nullptr;
    const auto Record = Inodes.find(Name->second);
    if (Record == Inodes.end() || !Record->second.Symlink) return nullptr;
    return &*Record->second.Symlink;
  }

  int64_t WalkSdPath(std::string_view Name, FsFileSystem& SdCard, std::string& Path, bool Follow, unsigned Depth,
                     uint64_t Resolve, std::string_view Scope) {
    if (Name.find_first_of(":\\") != Name.npos) return Invalid;
    const bool UnderRoot = Path == RootPath ||
                           (Path.starts_with(RootPath) && Path[RootPath.size()] == '/');
    const size_t Boundary = !Scope.empty() ? Scope.size() : UnderRoot ? RootPath.size() : LaunchFolder.size();
    while (!Name.empty()) {
      if (!Name.starts_with('/')) {
        const auto Inode = SdInode(Path);
        if (!InodeAllows(Inode, ModeForInode(Inode, 0040755), 1)) return -13;
      }
      const auto Slash = Name.find('/');
      const auto Component = Name.substr(0, Slash);
      if (Component == "..") {
        if (Path.size() > Boundary) {
          const auto ParentSize = std::max(Boundary, Path.rfind('/'));
          if ((Resolve & ResolveNoXdev) && VirtualMount(Path) != VirtualMount(std::string_view(Path).substr(0, ParentSize))) return -18;
          Path.resize(ParentSize);
        } else if (Resolve & ResolveBeneath) return -18;
      } else if (!Component.empty() && Component != ".") {
        // Traverse the intermediate entry before consuming a subsequent
        // component, including "." or "..". A missing or regular entry
        // cannot serve as the parent directory of another pathname.
        if (Component.size() > 255) return -36; // NAME_MAX
        const auto ParentSize = Path.size();
        if (!Path.ends_with('/')) Path += '/';
        Path += Component;
        if (Path.size() >= RootPath.size() + GuestPathMax) return -36;
        if (IsInodeStorePath(Path)) return -2;
        if ((Resolve & ResolveNoXdev) && VirtualMount(Path) != VirtualMount(std::string_view(Path).substr(0, ParentSize))) return -18;
        const auto SyntheticTarget = ProcLink(Path);
        const auto* Target = SyntheticTarget.empty() ? SymlinkAt(Path) : &SyntheticTarget;
        if (Target && (Follow || Slash != Name.npos)) {
          if (Depth == 40 || (Resolve & ResolveNoSymlinks)) return -40;
          auto Remaining = *Target;
          if (Slash != Name.npos) Remaining += Name.substr(Slash);
          if (Remaining.size() >= GuestPathMax) return -36;
          if (Target->starts_with('/')) {
            if (Resolve & ResolveBeneath) return -18;
            const std::string_view Restart = Resolve & ResolveInRoot ? Scope : std::string_view(RootPath);
            if ((Resolve & ResolveNoXdev) && VirtualMount(Path) != VirtualMount(Restart)) return -18;
            Path = Restart;
          } else Path.resize(ParentSize);
          return WalkSdPath(Remaining, SdCard, Path, Follow, Depth + 1, Resolve, Scope);
        }
        if (Slash != Name.npos && (Depth || Name.find_first_not_of('/', Slash) != Name.npos)) {
          if (IsManagedFile(Path) || SpecialNodeAt(Path)) return -20;
          const auto GuestPath = std::string_view(Path).substr(UnderRoot ? RootPath.size() : 0);
          if (UnderRoot && (ProcFile(GuestPath) || GuestPath == "/dev/null" ||
                            (Interactive && GuestPath == "/dev/tty"))) return -20;
          if (VirtualDirectory(Path).empty()) {
            FsDirEntryType Type {};
            if (const auto Error = EntryType(SdCard, Path, Type); Error < 0) return Error;
            if (Type != FsDirEntryType_Dir) return -20;
          }
        }
      }
      if (Slash == Name.npos) break;
      Name.remove_prefix(Slash + 1);
    }
    return 0;
  }

  bool CanModifyParent(std::string_view Path) {
    const auto Parent = Path.substr(0, std::max<size_t>(1, Path.rfind('/')));
    if (!VirtualDirectory(Parent).empty()) return false;
    const auto Inode = SdInode(Parent);
    return InodeAllows(Inode, ModeForInode(Inode, 0040755), 3);
  }

  bool CanRemoveStickyEntry(const std::string& Path) {
    const auto User = Task->UserIDs[3];
    if (!User) return true;
    const auto Parent = SdInode(ParentPath(Path));
    return !(ModeForInode(Parent, 0040755) & 01000) ||
           InodeOwner(Parent).first == User || InodeOwner(SdInode(Path)).first == User;
  }

  static bool PathContains(std::string_view Parent, std::string_view Entry) {
    return Entry == Parent || (Entry.starts_with(Parent) && Entry[Parent.size()] == '/');
  }

  void TrackRenamedSdEntry(std::string_view Old, std::string_view New, bool UpdateHandles = true) {
    const auto MovedPath = [&](std::string_view Path) {
      return std::string(New) + std::string(Path.substr(Old.size()));
    };
    decltype(SdInodes) Moved;
    for (auto It = SdInodes.lower_bound(Old); It != SdInodes.end() && It->first.starts_with(Old);) {
      if (PathContains(Old, It->first)) {
        Moved.emplace(MovedPath(It->first), It->second);
        It = SdInodes.erase(It);
      } else {
        ++It;
      }
    }
    SdInodes.merge(Moved);
    if (UpdateHandles) TrackRenamedHandles(Old, New);
  }

  void TrackRenamedHandles(std::string_view Old, std::string_view New) {
    const auto MovedPath = [&](std::string_view Path) {
      return std::string(New) + std::string(Path.substr(Old.size()));
    };
    ForEachOpenFile([&](OpenFile& File) {
      if ((File.Directory || File.RealFile) && !File.RemovedDirectory &&
          !IsUnlinked(File.Inode) && PathContains(Old, File.Path)) File.Path = MovedPath(File.Path);
    });
    for (auto& [PID, Process] : Processes) {
      if (!Process.RemovedWorkingDirectory && PathContains(Old, Process.Folder)) {
        Process.Folder = MovedPath(Process.Folder);
        Process.WorkingDirectory = GuestWorkingPath(Process.Folder);
      }
    }
  }

  int64_t RenameInodeEntries(const std::string& Old, const std::string& New, bool Exchange, bool Replace = true) {
    const auto Source = SdInodes.at(Old);
    const auto Existing = SdInodes.find(New);
    const auto PreviousName = Existing == SdInodes.end() ? 0 : Existing->second;
    const auto Destination = Replace ? PreviousName : 0;
    if (Source == Destination) return 0;

    auto& SourceRecord = Inodes.at(Source);
    auto* DestinationRecord = Destination ? &Inodes.at(Destination) : nullptr;
    const auto SourceTimes = SourceRecord.Times;
    const auto DestinationTimes = DestinationRecord ? DestinationRecord->Times : std::nullopt;
    const bool PreviousDirty = InodeStoreDirty;
    const bool LastDestinationLink = DestinationRecord && !Exchange && InodeLinkCount(Destination) == 1;
    if (Exchange) SdInodes[Old] = Destination;
    else SdInodes.erase(Old);
    SdInodes[New] = Source;
    MarkInodeChanged(Source);
    if (DestinationRecord) {
      DestinationRecord->Unlinked = LastDestinationLink;
      MarkInodeChanged(Destination);
    }

    const auto OldParent = ParentPath(Old);
    const auto NewParent = ParentPath(New);
    const auto OldParentTimes = UpdateParentTimestamps(OldParent);
    const auto NewParentTimes = OldParent != NewParent ?
      std::optional {UpdateParentTimestamps(NewParent)} : std::nullopt;
    if (const auto Error = SaveInodeStore(); Error < 0) {
      SdInodes[Old] = Source;
      if (PreviousName) SdInodes[New] = PreviousName;
      else SdInodes.erase(New);
      SourceRecord.Times = SourceTimes;
      if (DestinationRecord) {
        DestinationRecord->Times = DestinationTimes;
        DestinationRecord->Unlinked = false;
      }
      RestoreParentTimestamps(OldParentTimes);
      if (NewParentTimes) RestoreParentTimestamps(*NewParentTimes);
      InodeStoreDirty = PreviousDirty;
      return Error;
    }

    ForEachOpenFile([&](OpenFile& File) {
      if (!File.RealFile) return;
      if (File.Inode == Source && File.Path == Old) File.Path = New;
      else if (Exchange && File.Inode == Destination && File.Path == New) File.Path = Old;
    });
    return 0;
  }

  // Longest native path below Path, or Path's own length. Store-backed
  // entries are skipped because they never need an SD path.
  int64_t LongestNativePath(const std::string& Path) const {
    std::vector<DirectoryEntry> Entries;
    if (const auto Status = ReadDirectory(Path, Entries); R_FAILED(Status)) return SdFileError(Status);
    int64_t Longest = Path.size();
    for (const auto& Entry : Entries) {
      const auto Child = Path + "/" + Entry.Name;
      if (IsStoreEntry(Child)) continue;
      const auto Length = Entry.IsDirectory ? LongestNativePath(Child) : static_cast<int64_t>(Child.size());
      if (Length < 0) return Length;
      Longest = std::max(Longest, Length);
    }
    return Longest;
  }

  // Whether moving the native directory Old to New would leave a native
  // descendant beyond SdPathLimit.
  int64_t SubtreeExceedsLimit(const std::string& Old, const std::string& New) const {
    if (New.size() <= Old.size()) return 0;
    const auto Longest = LongestNativePath(Old);
    if (Longest < 0) return Longest;
    return static_cast<uint64_t>(Longest) - Old.size() + New.size() > SdPathLimit;
  }

  // Moves a native directory tree into the store: files become managed
  // objects through the journaled promotion, then each directory, deepest
  // first, becomes an overflow record before its empty native directory is
  // deleted. Every intermediate state is valid after a reset: store entries
  // may live in native directories, and a leftover empty native directory
  // is shadowed by its overflow record and removed on the next conversion.
  int64_t ConvertToOverflow(FsFileSystem& SdCard, const std::string& Path) {
    if (IsOverflowDirectory(Path)) return 0;
    if (!InodeStoreAvailable || InodeGeneration == UINT64_MAX) return -5;
    std::vector<DirectoryEntry> Entries;
    if (const auto Status = ReadDirectory(Path, Entries); R_FAILED(Status)) return SdFileError(Status);
    for (const auto& Entry : Entries) {
      const auto Child = Path + "/" + Entry.Name;
      int64_t Error {};
      if (IsOverflowDirectory(Child)) {
        Error = SdFileError(fsFsDeleteDirectory(&SdCard, Child.c_str()));
        if (Error == -2) Error = 0;
      } else {
        Error = Entry.IsDirectory ? ConvertToOverflow(SdCard, Child) : PromoteSdFile(SdCard, Child);
      }
      if (Error) return Error;
    }
    const auto Inode = SdInode(Path);
    ImportTimestamps(SdCard, Path, Inode);
    auto& Record = Inodes[Inode];
    const auto PreviousMode = Record.Mode;
    const bool PreviousDirty = InodeStoreDirty;
    if (!Record.Mode) Record.Mode = 0755;
    Record.OverflowDirectory = true;
    InodeStoreDirty = true;
    if (const auto Error = SaveInodeStore(); Error) {
      Record.OverflowDirectory = false;
      Record.Mode = PreviousMode;
      InodeStoreDirty = PreviousDirty;
      return Error;
    }
    const auto Removed = SdFileError(fsFsDeleteDirectory(&SdCard, Path.c_str()));
    if (Removed && Removed != -2) WriteLog("Converted overflow directory left its empty native directory behind.");
    return 0;
  }

  // Makes an existing entry store-backed so it can be renamed by name.
  int64_t MakeStoreBacked(FsFileSystem& SdCard, const std::string& Path, FsDirEntryType Type) {
    if (IsStoreEntry(Path)) return 0;
    return Type == FsDirEntryType_Dir ? ConvertToOverflow(SdCard, Path) : PromoteSdFile(SdCard, Path);
  }

  // Renames store-backed entries, moving everything below a directory with
  // it. A replaced destination loses its name, as unlink or rmdir would.
  int64_t RenameStoreEntries(const std::string& Old, const std::string& New, bool Exchange) {
    const auto Source = SdInode(Old);
    const auto Existing = SdInodes.find(New);
    const auto Destination = Existing != SdInodes.end() && IsStoreEntry(New) ? Existing->second : 0;
    if (Source == Destination && !Exchange) return 0; // Hard links to one inode.
    const bool DestinationDirectory = Destination && IsOverflowDirectory(New);
    const bool LastDestinationLink = Destination && !Exchange && InodeLinkCount(Destination) == 1;

    decltype(SdInodes) Names;
    for (const auto& [Path, Inode] : SdInodes) {
      if (PathContains(Old, Path)) Names.emplace(New + Path.substr(Old.size()), Inode);
      else if (PathContains(New, Path)) {
        if (Exchange) Names.emplace(Old + Path.substr(New.size()), Inode);
      } else Names.emplace(Path, Inode);
    }
    const auto SourceTimes = Inodes[Source].Times;
    const auto DestinationTimes = Destination ? Inodes[Destination].Times : std::nullopt;
    const bool PreviousDirty = InodeStoreDirty;
    SdInodes.swap(Names);
    MarkInodeChanged(Source);
    if (Destination) {
      Inodes[Destination].Unlinked = LastDestinationLink;
      MarkInodeChanged(Destination);
    }
    const auto OldParent = ParentPath(Old);
    const auto NewParent = ParentPath(New);
    const auto OldParentTimes = UpdateParentTimestamps(OldParent);
    const auto NewParentTimes = OldParent != NewParent ?
      std::optional {UpdateParentTimestamps(NewParent)} : std::nullopt;
    if (const auto Error = SaveInodeStore(); Error < 0) {
      SdInodes.swap(Names);
      Inodes[Source].Times = SourceTimes;
      if (Destination) {
        Inodes[Destination].Times = DestinationTimes;
        Inodes[Destination].Unlinked = false;
      }
      RestoreParentTimestamps(OldParentTimes);
      if (NewParentTimes) RestoreParentTimestamps(*NewParentTimes);
      InodeStoreDirty = PreviousDirty;
      return Error;
    }
    if (Exchange) {
      constexpr std::string_view Swap = "\x01exchange";
      TrackRenamedHandles(New, Swap);
      TrackRenamedHandles(Old, New);
      TrackRenamedHandles(Swap, Old);
    } else {
      if (DestinationDirectory) MarkDirectoryRemoved(New);
      TrackRenamedHandles(Old, New);
    }
    return 0;
  }

  // Renames where either side is, or must become, store-backed: overflow
  // directories, destinations the SD card cannot name, and native trees
  // that would exceed SdPathLimit at their new location.
  int64_t RenameThroughStore(FsFileSystem& SdCard, const std::string& OldPath, const std::string& NewPath,
                             FsDirEntryType OldType, uint64_t Flags) {
    constexpr uint64_t NoReplace = 1, Exchange = 2;
    if (OldPath == NewPath) return 0;
    FsDirEntryType NewType {FsDirEntryType_File};
    const bool NewLink = IsVirtualEntry(NewPath);
    const auto Target = NewLink ? 0 : EntryType(SdCard, NewPath, NewType);
    if (Target && Target != -2) return Target;
    const bool TargetExists = !Target;
    // do_renameat2 order: NOREPLACE, exchange target, then the ancestor
    // checks (source above target EINVAL, target above source ENOTEMPTY,
    // or EINVAL for exchange), then types in vfs_rename.
    if (TargetExists && (Flags & NoReplace)) return -17;
    if (!TargetExists && (Flags & Exchange)) return -2;
    if (PathContains(OldPath, NewPath)) return Invalid;
    if (TargetExists) {
      if (PathContains(NewPath, OldPath)) return Flags & Exchange ? Invalid : -39;
      if (!CanRemoveStickyEntry(NewPath)) return -1;
      if (!(Flags & Exchange)) {
        if (OldType != NewType) return OldType == FsDirEntryType_Dir ? -20 : -21;
        if (NewType == FsDirEntryType_Dir) {
          if (HasStoreChildren(NewPath)) return -39;
          if (!IsOverflowDirectory(NewPath)) {
            FsDir Directory {};
            auto Error = SdFileError(fsFsOpenDirectory(&SdCard, NewPath.c_str(), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &Directory));
            if (Error) return Error;
            s64 Count {};
            Error = SdFileError(fsDirGetEntryCount(&Directory, &Count));
            fsDirClose(&Directory);
            if (Error) return Error;
            if (Count) return -39;
          }
        }
      }
    }
    if (const auto Error = MakeStoreBacked(SdCard, OldPath, OldType); Error) return Error;
    if (TargetExists) {
      if (const auto Error = MakeStoreBacked(SdCard, NewPath, NewType); Error) return Error;
    }
    return RenameStoreEntries(OldPath, NewPath, Flags & Exchange);
  }

  int64_t RenameSdDirectories(FsFileSystem& SdCard, const std::string& Old, const std::string& New, bool Exchange, bool VirtualDestination = false) {
    if (!InodeStoreAvailable || InodeGeneration == UINT64_MAX) return -5;
    if (Old == New) return 0;
    if (PathContains(Old, New) || PathContains(New, Old)) return Invalid;
    int64_t Error {};
    if (!Exchange) {
      FsDir Directory {};
      Error = SdFileError(fsFsOpenDirectory(&SdCard, New.c_str(), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &Directory));
      if (Error) return Error;
      s64 Count {};
      Error = SdFileError(fsDirGetEntryCount(&Directory, &Count));
      fsDirClose(&Directory);
      if (Error) return Error;
      if (Count) return -39;
      const auto Prefix = New + "/";
      for (auto It = SdInodes.lower_bound(Prefix); It != SdInodes.end() && It->first.starts_with(Prefix); ++It) {
        if (IsStoreEntry(It->first)) return -39;
      }

    }

    const auto First = InodeSnapshot::FirstInode;
    const auto Intent = EncodeInodeSnapshot(InodeGeneration + 1, First + (VirtualDestination ? 4 : Exchange ? 3 : 2),
                                            {{Old, First}, {New, First + 1}}, {});
    if (Intent.empty()) return -28;
    Error = PublishInodeIntent(SdCard, DirectoryIntentPath, Intent);
    if (Error) return Error;
    if (!VirtualDestination) Error = SdFileError(fsFsRenameDirectory(&SdCard, New.c_str(), DirectoryBackupPath));
    if (!Error) Error = SdFileError(fsFsRenameDirectory(&SdCard, Old.c_str(), New.c_str()));

    uint64_t Destination {};
    if (!Error) {
      Destination = SdInode(New);
      const auto Source = SdInode(Old);
      const auto SourceTimes = Inodes[Source].Times;
      const auto DestinationTimes = Inodes[Destination].Times;
      const bool PreviousDirty = InodeStoreDirty;
      auto PreviousNames = SdInodes;
      if (Exchange) {
        TrackRenamedSdEntry(New, DirectoryBackupPath, false);
      } else {
        for (auto It = SdInodes.lower_bound(New); It != SdInodes.end() && It->first.starts_with(New);) {
          if (PathContains(New, It->first)) It = SdInodes.erase(It);
          else ++It;
        }
      }
      TrackRenamedSdEntry(Old, New, false);
      if (Exchange) {
        TrackRenamedSdEntry(DirectoryBackupPath, Old, false);
        MarkInodeChanged(Destination);
      }
      MarkInodeChanged(Source);
      const auto OldParent = ParentPath(Old);
      const auto NewParent = ParentPath(New);
      const auto OldParentTimes = UpdateParentTimestamps(OldParent);
      const auto NewParentTimes = OldParent != NewParent ?
        std::optional {UpdateParentTimestamps(NewParent)} : std::nullopt;
      Error = SaveInodeStore();
      if (Error) {
        SdInodes.swap(PreviousNames);
        Inodes[Source].Times = SourceTimes;
        Inodes[Destination].Times = DestinationTimes;
        RestoreParentTimestamps(OldParentTimes);
        if (NewParentTimes) RestoreParentTimestamps(*NewParentTimes);
        InodeStoreDirty = PreviousDirty;
      }
    }

    if (const auto Recovery = RecoverDirectoryRename(SdCard); Recovery) {
      InodeStoreAvailable = false;
      WriteLog("Guest directory rename could not finish recovery.");
      return Error ? Error : Recovery;
    }
    if (Error) return Error;
    if (Exchange) {
      TrackRenamedHandles(New, DirectoryBackupPath);
      TrackRenamedHandles(Old, New);
      TrackRenamedHandles(DirectoryBackupPath, Old);
    } else {
      ForEachOpenFile([&](OpenFile& File) {
        if (File.Directory && File.Inode == Destination) File.RemovedDirectory = true;
      });
      for (auto& [PID, Process] : Processes) {
        if (Process.Folder == New) Process.RemovedWorkingDirectory = true;
      }
      TrackRenamedHandles(Old, New);
    }
    return 0;
  }

  int64_t InterruptInodeTransaction(FsFileSystem& SdCard) {
    const auto Error = SdFileError(fsFsCommit(&SdCard));
    InodeStoreAvailable = false;
    WriteLog("Filesystem recovery probe left a committed journal intent for the next launch.");
    return Error ? Error : -4;
  }

  int64_t MoveNativeEntry(FsFileSystem& SdCard, const std::string& Old, const std::string& New, FsDirEntryType Type, bool Remove = false) {
    if (!InodeStoreAvailable || InodeGeneration == UINT64_MAX) return -5;
    const auto First = InodeSnapshot::FirstInode;
    const auto Intent = EncodeInodeSnapshot(InodeGeneration + 1, First + (Remove ? 7 : 5) + (Type == FsDirEntryType_File),
                                            {{Old, First}, {New, First + 1}}, {});
    if (Intent.empty()) return -28;
    auto Error = PublishInodeIntent(SdCard, DirectoryIntentPath, Intent);
    if (Error) return Error;

    const auto Source = SdInode(Old);
    ImportTimestamps(SdCard, Old, Source);
    const auto SourceTimes = Inodes[Source].Times;
    auto PreviousNames = SdInodes;
    const auto PreviousDirty = InodeStoreDirty;
    Error = SdFileError(Type == FsDirEntryType_Dir ? fsFsRenameDirectory(&SdCard, Old.c_str(), New.c_str()) :
                                                   fsFsRenameFile(&SdCard, Old.c_str(), New.c_str()));
    // Recovery acceptance: a launch opted in with a .recovery-test file stops
    // its probe move before or after the metadata checkpoint, as a power loss
    // would, and the next launch must roll the journal back or forward.
    const auto TestRoot = RootPath + "/.fex-recovery-test/";
    const bool TestMove = RecoveryTestLaunch && !Remove && Old == TestRoot + "source" && New.starts_with(TestRoot);
    if (!Error && TestMove && New == TestRoot + "before-checkpoint") {
      return InterruptInodeTransaction(SdCard);
    }
    if (!Error) {
      // A previously used but now absent name must not shadow the moved inode.
      const auto& Stale = Remove ? Old : New;
      for (auto It = SdInodes.lower_bound(Stale); It != SdInodes.end() && It->first.starts_with(Stale);) {
        if (PathContains(Stale, It->first)) It = SdInodes.erase(It);
        else ++It;
      }
      if (!Remove) TrackRenamedSdEntry(Old, New, false);
      MarkInodeChanged(Source);
      const auto OldParent = ParentPath(Old);
      const auto NewParent = Remove ? OldParent : ParentPath(New);
      const auto OldParentTimes = UpdateParentTimestamps(OldParent);
      const auto NewParentTimes = OldParent != NewParent ?
        std::optional {UpdateParentTimestamps(NewParent)} : std::nullopt;
      Error = SaveInodeStore();
      if (Error) {
        SdInodes.swap(PreviousNames);
        Inodes[Source].Times = SourceTimes;
        RestoreParentTimestamps(OldParentTimes);
        if (NewParentTimes) RestoreParentTimestamps(*NewParentTimes);
        InodeStoreDirty = PreviousDirty;
      }
    }
    if (!Error && TestMove && New == TestRoot + "after-checkpoint") {
      return InterruptInodeTransaction(SdCard);
    }
    if (const auto Recovery = RecoverDirectoryRename(SdCard); Recovery) {
      InodeStoreAvailable = false;
      WriteLog("Guest native move could not finish recovery.");
      return Error ? Error : Recovery;
    }
    if (!Error && !Remove) TrackRenamedHandles(Old, New);
    return Error;
  }

  int64_t RenameSdEntry(uint64_t OldFD, uint64_t OldAddress, uint64_t NewFD, uint64_t NewAddress, uint64_t Flags) {
    if (Flags & ~7ULL || (Flags & 2 && Flags & 5)) return Invalid;
    const auto OldName = GuestString(OldAddress), NewName = GuestString(NewAddress);
    if (!OldName) return OldName.Error;
    if (!NewName) return NewName.Error;

    FsFileSystem SdCard {};
    if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
    std::string OldPath, NewPath;
    const auto WithoutTrailingSlashes = [](std::string_view Name) {
      const auto Last = Name.find_last_not_of('/');
      return Last == Name.npos ? Name : Name.substr(0, Last + 1);
    };
    const auto OldComponentPath = WithoutTrailingSlashes(*OldName);
    const auto NewComponentPath = WithoutTrailingSlashes(*NewName);
    const auto DotName = [](std::string_view Path) {
      const auto Last = Path.substr(Path.rfind('/') + 1);
      return Last == "." || Last == "..";
    };
    auto Error = ResolveSdPath(OldFD, OldComponentPath, SdCard, OldPath, false);
    if (!Error) Error = ResolveSdPath(NewFD, NewComponentPath, SdCard, NewPath, false);
    if (!Error && (DotName(OldComponentPath) || DotName(NewComponentPath))) Error = -16;
    if (!Error && (OldPath == RootPath || OldPath == LaunchFolder || NewPath == RootPath || NewPath == LaunchFolder)) Error = -16;
    const bool OldLink = !Error && IsVirtualEntry(OldPath);
    const bool NewLink = !Error && IsVirtualEntry(NewPath);
    FsDirEntryType OldType {FsDirEntryType_File}, NewType {FsDirEntryType_File};
    if (!Error && !OldLink) Error = EntryType(SdCard, OldPath, OldType);
    if (!Error && ((OldType != FsDirEntryType_Dir && (OldName->ends_with('/') || NewName->ends_with('/'))) ||
                   (NewLink && NewName->ends_with('/')))) Error = -20;
    if (!Error && (!CanModifyParent(OldPath) || !CanModifyParent(NewPath))) Error = -13;
    if (!Error && !CanRemoveStickyEntry(OldPath)) Error = -1;
    if (!Error && NewLink && !CanRemoveStickyEntry(NewPath)) Error = -1;
    if (!Error && (Flags & 4)) Error = -1; // Whiteout requires privileges.
    if (!Error && OldPath != NewPath) {
      // Exchange also moves the destination to the source's location.
      FsDirEntryType Swapped {};
      bool Store = IsOverflowDirectory(OldPath) || IsOverflowDirectory(NewPath) || NeedsOverflow(NewPath);
      if (!Store && OldType == FsDirEntryType_Dir && !OldLink) {
        const auto Exceeds = SubtreeExceedsLimit(OldPath, NewPath);
        if (Exceeds < 0) Error = Exceeds;
        Store = Exceeds > 0;
      }
      if (!Error && !Store && (Flags & 2) && !NewLink && !EntryType(SdCard, NewPath, Swapped) &&
          Swapped == FsDirEntryType_Dir) {
        const auto Exceeds = SubtreeExceedsLimit(NewPath, OldPath);
        if (Exceeds < 0) Error = Exceeds;
        Store = Exceeds > 0;
      }
      if (!Error && Store) {
        Error = RenameThroughStore(SdCard, OldPath, NewPath, OldType, Flags);
        fsFsClose(&SdCard);
        return Error;
      }
    }
    if (!Error && OldLink && NewLink && !(Flags & 1)) {
      fsFsClose(&SdCard);
      return RenameInodeEntries(OldPath, NewPath, Flags & 2);
    }
    if (!Error && (OldPath != NewPath || Flags)) {
      if (OldType == FsDirEntryType_Dir && OldPath != NewPath && PathContains(OldPath, NewPath)) Error = Invalid;
      if (!Error) {
        const auto TargetError = NewLink ? 0 : EntryType(SdCard, NewPath, NewType);
        if (!TargetError) {
          if (Flags & 1) Error = -17;
          else if (!CanRemoveStickyEntry(NewPath)) Error = -1;
          else if (!(Flags & 2) && OldType != NewType) Error = OldType == FsDirEntryType_Dir ? -20 : -21;
          else if (OldType == FsDirEntryType_Dir || NewType == FsDirEntryType_Dir) {
            if (OldType == NewType) {
              Error = RenameSdDirectories(SdCard, OldPath, NewPath, Flags & 2);
            } else {
              const bool OldDirectory = OldType == FsDirEntryType_Dir;
              const auto& Directory = OldDirectory ? OldPath : NewPath;
              const auto& Other = OldDirectory ? NewPath : OldPath;
              if (PathContains(Directory, Other)) {
                Error = Invalid;
              } else {
                Error = PromoteSdFile(SdCard, Other);
                if (!Error) Error = RenameSdDirectories(SdCard, Directory, Other, true, true);
              }
            }
            fsFsClose(&SdCard);
            return Error;
          } else {
            Error = PromoteSdFile(SdCard, OldPath);
            if (!Error) Error = PromoteSdFile(SdCard, NewPath);
            if (!Error) {
              fsFsClose(&SdCard);
              return RenameInodeEntries(OldPath, NewPath, Flags & 2);
            }
          }
        } else if (TargetError != -2 || (Flags & 2)) {
          Error = TargetError;
        }
      }
      if (!Error) {
        if (OldLink) {
          fsFsClose(&SdCard);
          return RenameInodeEntries(OldPath, NewPath, false, false);
        }
        Error = MoveNativeEntry(SdCard, OldPath, NewPath, OldType);
      }
    }
    fsFsClose(&SdCard);
    return Error;
  }

  int64_t PromoteSdFile(FsFileSystem& SdCard, const std::string& Path) {
    if (IsVirtualEntry(Path)) return 0;
    if (!InodeStoreAvailable) return -5;
    const auto Inode = SdInode(Path);
    ImportTimestamps(SdCard, Path, Inode);
    const auto Storage = SdFileError(fsFsCreateDirectory(&SdCard, InodeObjects.data()));
    if (Storage && Storage != -17) return Storage;
    if (const auto Error = BeginPromotion(SdCard, Path, Inode); Error < 0) return Error;
    const auto Object = ObjectPath(Inode);
    auto Error = SdFileError(fsFsRenameFile(&SdCard, Path.c_str(), Object.c_str()));
    if (!Error) {
      auto& Record = Inodes[Inode];
      const auto OldMode = Record.Mode;
      const bool PreviousDirty = InodeStoreDirty;
      if (!Record.Mode) Record.Mode = RegularMode(Path) & 07777;
      Record.ManagedFile = true;
      InodeStoreDirty = true;
      Error = SaveInodeStore();
      if (Error) {
        const auto Rollback = SdFileError(fsFsRenameFile(&SdCard, Object.c_str(), Path.c_str()));
        if (Rollback) {
          InodeStoreAvailable = false;
          WriteLog("Failed to restore file after inode metadata checkpoint error.");
          return Error; // Preserve the recovery record.
        }
        Record.Mode = OldMode;
        Record.ManagedFile = false;
        InodeStoreDirty = PreviousDirty;
      }
    }
    if (const auto Cleanup = FinishInodeIntent(SdCard, PromotionPath); Cleanup) {
      InodeStoreAvailable = false;
      WriteLog("Guest inode promotion recovery record could not be cleared.");
      return Error ? Error : Cleanup;
    }
    return Error;
  }

  int64_t LinkGuest(uint64_t OldFD, uint64_t OldAddress, uint64_t NewFD, uint64_t NewAddress, uint64_t Flags) {
    constexpr uint64_t Follow = 0x400, Empty = 0x1000;
    if (Flags & ~(Follow | Empty)) return Invalid;
    const auto OldName = GuestString(OldAddress), NewName = GuestString(NewAddress);
    if (!OldName) return OldName.Error;
    if (!NewName) return NewName.Error;
    OpenFile* Temporary = nullptr;
    if (OldName->empty()) {
      if (!(Flags & Empty)) return -2;
      Temporary = GetFile(OldFD);
      if (!Temporary) return BadFile;
      if (!Temporary->TemporaryLinkable || (Temporary->TemporaryPublished && IsUnlinked(Temporary->Inode))) return -2;
    }

    FsFileSystem SdCard {};
    if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
    std::string OldPath, NewPath;
    auto Error = Temporary ? int64_t {0} : ResolveSdPath(OldFD, *OldName, SdCard, OldPath, Flags & Follow);
    if (!Error) Error = ResolveSdPath(NewFD, *NewName, SdCard, NewPath, false);
    if (!Error && NewName->ends_with('/')) Error = -2;
    if (!Error && !CanModifyParent(NewPath)) Error = -13;
    if (!Error && IsVirtualEntry(NewPath)) Error = -17;
    if (!Error) {
      FsDirEntryType Type {};
      const auto Target = EntryType(SdCard, NewPath, Type);
      if (Target != -2) Error = Target ? Target : -17;
    }
    if (!Error && !Temporary && OldName->ends_with('/') && IsVirtualEntry(OldPath)) Error = -20;
    if (!Error && !Temporary && !IsVirtualEntry(OldPath)) {
      FsDirEntryType Type {};
      Error = EntryType(SdCard, OldPath, Type);
      if (!Error && Type == FsDirEntryType_Dir) Error = -1;
      if (!Error && OldName->ends_with('/')) Error = -20;
      if (!Error) Error = PromoteSdFile(SdCard, OldPath);
    }
    if (!Error) {
      const auto Inode = Temporary ? Temporary->Inode : SdInode(OldPath);
      auto& Record = Inodes.at(Inode);
      const auto OldTimes = Record.Times;
      const bool WasUnlinked = Record.Unlinked;
      const auto PreviousDirty = InodeStoreDirty;
      SdInodes.emplace(NewPath, Inode);
      Record.Unlinked = false;
      MarkInodeChanged(Inode);
      const auto ParentTimes = UpdateParentTimestamps(ParentPath(NewPath));
      Error = SaveInodeStore();
      if (Error) {
        SdInodes.erase(NewPath);
        Record.Times = OldTimes;
        Record.Unlinked = WasUnlinked;
        RestoreParentTimestamps(ParentTimes);
        InodeStoreDirty = PreviousDirty;
      } else if (Temporary) {
        Temporary->TemporaryPublished = true;
        Temporary->Path = NewPath;
      }
    }
    fsFsClose(&SdCard);
    return Error;
  }

  int64_t MakeNode(uint64_t FD, uint64_t Address, uint32_t Mode) {
    const auto Type = Mode & 0170000;
    if (!Type || Type == 0100000) {
      const auto Opened = OpenGuest(FD, Address, 0300, Mode);
      if (Opened < 0) return Opened;
      return CloseGuest(Opened);
    }
    if (Type == 0020000 || Type == 0060000 || Type == 0040000) return -1;
    if (Type != 0010000 && Type != 0140000) return Invalid;

    const auto Name = GuestString(Address);
    if (!Name) return Name.Error;
    FsFileSystem SdCard {};
    if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
    std::string Path;
    auto Error = ResolveSdPath(FD, *Name, SdCard, Path, false);
    if (!Error && Name->ends_with('/')) Error = -2;
    if (!Error && !CanModifyParent(Path)) Error = -13;
    if (!Error && IsVirtualEntry(Path)) Error = -17;
    if (!Error) {
      FsDirEntryType ExistingType {};
      const auto Existing = EntryType(SdCard, Path, ExistingType);
      if (Existing != -2) Error = Existing ? Existing : -17;
    }
    fsFsClose(&SdCard);
    if (Error) return Error;
    InodeMetadata Record {};
    Record.SpecialType = Type;
    return CheckpointNewEntry(Path, Mode, false, std::move(Record));
  }

  int64_t OpenFifo(std::shared_ptr<OpenFile> File, uint64_t FD, bool CloseOnExec) {
    auto& Entry = FifoEndpoints[File->Inode];
    auto Fifo = Entry.lock();
    const auto Access = File->StatusFlags & 3;
    if (Access == 3) return Invalid; // fifo_open accepts only read, write or both.
    if (Access == 1 && File->Nonblocking() && (!Fifo || !Fifo->Readers)) return -6;

    if (Current->PendingFifo && Current->PendingFifo->Inode == File->Inode) {
      File = Current->PendingFifo;
    } else {
      if (!Fifo) Fifo = std::make_shared<FifoState>();
      Entry = Fifo;
      File->Pipe = true;
      File->Fifo = Fifo;
      File->Socket = Access == 1 ? Fifo->Writer : Fifo->Reader;
      File->InitialWriterGeneration = Fifo->WriterGeneration - (Fifo->Writers != 0);
      if (Access != 1) {
        ++Fifo->Readers;
        Fifo->Reader->ShutRead = false;
      }
      if (Access != 0) {
        if (!Fifo->Writers++) ++Fifo->WriterGeneration;
        Fifo->Writer->ShutWrite = false;
      }
      if (Fifo->Readers && Fifo->Writers) {
        for (auto& Thread : Threads) {
          if (Thread.PendingFifo && Thread.PendingFifo->Fifo == Fifo) {
            Thread.PendingFifo->FifoPartnerSeen = true;
          }
        }
      }
      ++IOEpoch;
    }
    if (!File->Nonblocking() && !File->FifoPartnerSeen && (!Fifo->Readers || !Fifo->Writers)) {
      Current->PendingFifo = std::move(File);
      return WaitForIO();
    }
    Current->PendingFifo.reset();
    Descriptors().Files[FD] = std::move(File);
    Descriptors().Flags[FD] = CloseOnExec;
    return FD;
  }

  int64_t CreateSymlink(uint64_t TargetAddress, uint64_t FD, uint64_t NameAddress) {
    const auto Target = GuestString(TargetAddress), Name = GuestString(NameAddress);
    if (!Target) return Target.Error;
    if (!Name) return Name.Error;
    if (Target->empty()) return -2;
    FsFileSystem SdCard {};
    if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
    std::string Path;
    auto Error = ResolveSdPath(FD, *Name, SdCard, Path, false);
    if (!Error && (Name->ends_with('/') || !CanModifyParent(Path))) Error = Name->ends_with('/') ? -2 : -13;
    if (!Error && IsVirtualEntry(Path)) Error = -17;
    if (!Error) {
      FsDirEntryType Type {};
      const auto Existing = EntryType(SdCard, Path, Type);
      if (Existing != -2) Error = Existing ? Existing : -17;
    }
    fsFsClose(&SdCard);
    if (Error) return Error;
    InodeMetadata Record {};
    Record.Symlink = *Target;
    return CheckpointNewEntry(Path, 0777, false, std::move(Record));
  }

  int64_t ReadSymlink(uint64_t FD, uint64_t Address, uint64_t Output, int32_t Capacity) {
    if (Capacity <= 0) return Invalid;
    const auto Name = GuestString(Address);
    if (!Name) return Name.Error;
    std::string SyntheticTarget;
    const std::string* Target = nullptr;
    if (Name->empty()) {
      const auto* File = GetFile(FD);
      if (!File) return IsCwd(FD) ? -2 : BadFile;
      SyntheticTarget = ProcLink(File->Path);
      const auto Metadata = Inodes.find(File->Inode);
      if (Metadata != Inodes.end() && Metadata->second.Symlink) Target = &*Metadata->second.Symlink;
    } else {
      LinuxStat Info {};
      std::string Path;
      if (const auto Error = PathStat(FD, *Name, Info, &Path, false); Error < 0) return Error;
      SyntheticTarget = ProcLink(Path);
      Target = SymlinkAt(Path);
    }
    if (!SyntheticTarget.empty()) Target = &SyntheticTarget;
    if (!Target) return Name->empty() ? -2 : Invalid;
    const auto Bytes = std::min<uint64_t>(Capacity, Target->size());
    if (Available(Output, PF_W) < Bytes) return Fault;
    std::memcpy(reinterpret_cast<void*>(Output), Target->data(), Bytes);
    return Bytes;
  }

  int64_t CheckpointNewEntry(const std::string& Path, uint32_t Mode, bool Directory, InodeMetadata Record = {}) {
    auto PreviousName = SdInodes.extract(Path);
    const auto PreviousDirty = InodeStoreDirty;
    const auto Inode = SdInode(Path);
    const auto Parent = ParentPath(Path);
    const auto Now = WallClock();
    Record.Times = InodeTimestamps {Now, Now, Now};
    uint32_t Group = Task->GroupIDs[3];
    const auto ParentInode = SdInode(Parent);
    if (ModeForInode(ParentInode, 0040755) & 02000) {
      Group = InodeOwner(ParentInode).second;
      if (Directory) Mode |= 02000;
    }
    Record.Owner.emplace(Task->UserIDs[3], Group);
    const bool Symlink = Record.Symlink.has_value();
    Inodes.emplace(Inode, std::move(Record));
    if (!Symlink) InitializeInodeMode(Inode, Parent, Mode, Directory);
    const auto ParentTimes = UpdateParentTimestamps(Parent);
    const auto Error = SaveInodeStore();
    if (Error) {
      SdInodes.erase(Path);
      Inodes.erase(Inode);
      if (PreviousName) SdInodes.insert(std::move(PreviousName));
      RestoreParentTimestamps(ParentTimes);
      InodeStoreDirty = PreviousDirty;
    }
    return Error;
  }

  int64_t CreateNativeEntry(FsFileSystem& SdCard, const std::string& Path, uint32_t Mode, bool Directory) {
    if (!InodeStoreAvailable || InodeGeneration == UINT64_MAX) return -5;
    FsDirEntryType StagingType {};
    const auto Staging = SdFileError(fsFsGetEntryType(&SdCard, DirectoryBackupPath, &StagingType));
    if (Staging != -2) return Staging ? Staging : -5;
    const auto First = InodeSnapshot::FirstInode;
    const auto Intent = EncodeInodeSnapshot(InodeGeneration + 1, First + (Directory ? 9 : 10),
                                            {{DirectoryBackupPath, First}, {Path, First + 1}}, {});
    if (Intent.empty()) return -28;
    auto Error = PublishInodeIntent(SdCard, DirectoryIntentPath, Intent);
    if (Error) return Error;

    Error = SdFileError(Directory ? fsFsCreateDirectory(&SdCard, DirectoryBackupPath) :
                                   fsFsCreateFile(&SdCard, DirectoryBackupPath, 0, 0));
    if (!Error) Error = CheckpointNewEntry(Path, Mode, Directory);
    if (const auto Recovery = RecoverDirectoryRename(SdCard); Recovery) {
      InodeStoreAvailable = false;
      WriteLog("Guest native creation could not finish recovery.");
      return Error ? Error : Recovery;
    }
    return Error;
  }

  // Creates a regular file the SD card cannot name, as a managed object.
  // The inode is first recorded as unlinked, like O_TMPFILE, so a reset
  // before the name is published leaves an object the next launch reclaims.
  int64_t CreateOverflowFile(FsFileSystem& SdCard, const std::string& Path, uint32_t Mode) {
    if (!InodeStoreAvailable || InodeGeneration == UINT64_MAX) return -5;
    const auto Storage = SdFileError(fsFsCreateDirectory(&SdCard, InodeObjects.data()));
    if (Storage && Storage != -17) return Storage;
    const auto Parent = ParentPath(Path);
    const auto ParentInode = SdInode(Parent);
    const auto Inode = NextSdInode++;
    auto& Record = Inodes[Inode];
    Record.ManagedFile = Record.Unlinked = true;
    uint32_t Group = Task->GroupIDs[3];
    if (ModeForInode(ParentInode, 0040755) & 02000) Group = InodeOwner(ParentInode).second;
    Record.Owner.emplace(Task->UserIDs[3], Group);
    InitializeInodeMode(Inode, Parent, Mode, false);
    const auto Now = WallClock();
    Record.Times = InodeTimestamps {Now, Now, Now};
    InodeStoreDirty = true;
    auto Error = SaveInodeStore();
    if (!Error) Error = SdFileError(fsFsCommit(&SdCard));
    if (!Error) Error = SdFileError(fsFsCreateFile(&SdCard, ObjectPath(Inode).c_str(), 0, 0));
    if (Error) return Error; // The unlinked record is reclaimed later.
    SdInodes[Path] = Inode;
    Inodes.at(Inode).Unlinked = false;
    const auto ParentTimes = UpdateParentTimestamps(Parent);
    InodeStoreDirty = true;
    Error = SaveInodeStore();
    if (Error) {
      SdInodes.erase(Path);
      Inodes.at(Inode).Unlinked = true;
      RestoreParentTimestamps(ParentTimes);
      InodeStoreDirty = true; // The unlinked record still needs saving.
    }
    return Error;
  }

  int64_t CreateDirectoryGuest(uint64_t FD, uint64_t Address, uint64_t Mode) {
    const auto Name = GuestString(Address);
    if (!Name) return Name.Error;
    FsFileSystem SdCard {};
    if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
    std::string Path;
    const auto Last = Name->find_last_not_of('/');
    const auto Pathname = Last == Name->npos ? *Name : Name->substr(0, Last + 1);
    auto Error = ResolveSdPath(FD, Pathname, SdCard, Path, false);
    if (!Error) {
      if (Path == RootPath || IsStoreEntry(Path)) Error = -17;
      else if (!CanModifyParent(Path)) Error = -13;
      else {
        FsDirEntryType Type {};
        Error = EntryType(SdCard, Path, Type);
        if (Error == -2 && NeedsOverflow(Path)) {
          if (!InodeStoreAvailable || InodeGeneration == UINT64_MAX) Error = -5;
          else {
            InodeMetadata Record {};
            Record.OverflowDirectory = true;
            Error = CheckpointNewEntry(Path, Mode, true, std::move(Record));
          }
        } else if (Error == -2) Error = CreateNativeEntry(SdCard, Path, Mode, true);
        else if (!Error) Error = -17;
      }
    }
    fsFsClose(&SdCard);
    return Error;
  }

  int64_t UnlinkInodeEntry(const std::string& Path) {
    const auto Inode = SdInodes.at(Path);
    auto& Record = Inodes.at(Inode);
    const auto OldTimes = Record.Times;
    const bool PreviousDirty = InodeStoreDirty;
    Record.Unlinked = InodeLinkCount(Inode) == 1;
    auto Name = SdInodes.extract(Path);
    MarkInodeChanged(Inode);
    const auto ParentTimes = UpdateParentTimestamps(ParentPath(Path));
    if (const auto Error = SaveInodeStore(); Error < 0) {
      SdInodes.insert(std::move(Name));
      Record.Unlinked = false;
      Record.Times = OldTimes;
      RestoreParentTimestamps(ParentTimes);
      InodeStoreDirty = PreviousDirty;
      return Error;
    }
    return 0;
  }

  int64_t UnlinkGuest(uint64_t FD, uint64_t Address) {
    const auto Name = GuestString(Address);
    if (!Name) return Name.Error;

    FsFileSystem SdCard {};
    if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
    std::string Path;
    auto Error = ResolveSdPath(FD, *Name, SdCard, Path, false);
    const bool Virtual = !Error && (SymlinkAt(Path) || SpecialNodeAt(Path));
    const bool Managed = !Error && IsManagedFile(Path);
    FsDirEntryType Type {};
    if (!Error && !Virtual) Error = EntryType(SdCard, Path, Type);
    if (!Error && !Virtual && Type == FsDirEntryType_Dir) Error = -21;
    if (!Error && Name->ends_with('/')) Error = -20;
    if (!Error && !CanModifyParent(Path)) Error = -13;
    if (!Error && !CanRemoveStickyEntry(Path)) Error = -1;
    if (!Error && (Managed || Virtual)) {
      fsFsClose(&SdCard);
      return UnlinkInodeEntry(Path);
    }
    if (!Error) {
      bool Live = false;
      const auto Inode = SdInode(Path);
      ForEachOpenFile([&](const OpenFile& File) { Live |= File.Inode == Inode && File.RealFile; });
      if (Live) {
        Error = PromoteSdFile(SdCard, Path);
        fsFsClose(&SdCard);
        return Error ? Error : UnlinkInodeEntry(Path);
      }
      Error = MoveNativeEntry(SdCard, Path, DirectoryBackupPath, FsDirEntryType_File, true);
    }
    fsFsClose(&SdCard);
    return Error;
  }

  int64_t RemoveDirectoryGuest(uint64_t FD, uint64_t Address) {
    const auto Name = GuestString(Address);
    if (!Name) return Name.Error;
    const auto Last = Name->find_last_not_of('/');
    if (Last != Name->npos) {
      const auto Slash = Name->rfind('/', Last);
      const auto Component = Name->substr(Slash == Name->npos ? 0 : Slash + 1,
                                           Last - (Slash == Name->npos ? 0 : Slash + 1) + 1);
      if (Component == ".") return Invalid;
      if (Component == "..") return -39;
    }

    FsFileSystem SdCard {};
    if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
    std::string Path;
    auto Pathname = *Name;
    if (Last != Name->npos) Pathname = Name->substr(0, Last + 1);
    auto Error = ResolveSdPath(FD, Pathname, SdCard, Path, false);
    if (!Error && IsVirtualEntry(Path)) Error = -20;
    if (!Error && (Path == RootPath || Path == "/")) Error = -16;
    FsDirEntryType Type {};
    if (!Error) Error = EntryType(SdCard, Path, Type);
    if (!Error && Type != FsDirEntryType_Dir) Error = -20;
    if (!Error && !CanModifyParent(Path)) Error = -13;
    if (!Error && !CanRemoveStickyEntry(Path)) Error = -1;
    if (!Error && IsOverflowDirectory(Path)) {
      Error = HasStoreChildren(Path) ? -39 : UnlinkInodeEntry(Path);
      fsFsClose(&SdCard);
      if (!Error) MarkDirectoryRemoved(Path);
      return Error;
    }
    if (!Error) {
      FsDir Directory {};
      auto Status = fsFsOpenDirectory(&SdCard, Path.c_str(), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &Directory);
      Error = SdFileError(Status);
      if (!Error) {
        s64 Entries {};
        Status = fsDirGetEntryCount(&Directory, &Entries);
        fsDirClose(&Directory);
        Error = SdFileError(Status);
        if (!Error && Entries) Error = -39;
      }
    }
    if (!Error) {
      const auto Prefix = Path + "/";
      for (auto It = SdInodes.lower_bound(Prefix); It != SdInodes.end() && It->first.starts_with(Prefix); ++It) {
        if (IsStoreEntry(It->first)) {
          Error = -39;
          break;
        }
      }
    }
    if (!Error) Error = MoveNativeEntry(SdCard, Path, DirectoryBackupPath, FsDirEntryType_Dir, true);
    fsFsClose(&SdCard);
    if (!Error) MarkDirectoryRemoved(Path);
    return Error;
  }

  void MarkDirectoryRemoved(const std::string& Path) {
    ForEachOpenFile([&](OpenFile& File) {
      if (File.Directory && File.Path == Path) File.RemovedDirectory = true;
    });
    for (auto& [PID, Process] : Processes) {
      if (Process.Folder == Path) Process.RemovedWorkingDirectory = true;
    }
  }

  int64_t DuplicateDescriptor(uint64_t Old, uint64_t New, bool CloseOnExec) {
    if (!GetFile(Old) || New >= Descriptors().Files.size()) return BadFile;
    if (Old == New) return New;
    if (GetFile(New)) CloseGuest(New);
    Descriptors().Files[New] = Descriptors().Files[Old];
    Descriptors().Flags[New] = CloseOnExec;
    return New;
  }

  int64_t FlockGuest(uint32_t FD, uint32_t Operation) {
    constexpr uint32_t Shared = 1, Exclusive = 2, Nonblocking = 4, Unlock = 8;
    const auto Kind = Operation & ~Nonblocking;
    if (Kind != Shared && Kind != Exclusive && Kind != Unlock) return Invalid;
    auto* File = GetFile(FD);
    if (!File || File->PathOnly()) return BadFile;

    const auto Requested = Kind == Shared ? FileLock::Shared :
                           Kind == Exclusive ? FileLock::Exclusive : FileLock::None;
    if (File->Lock == Requested) return 0;
    auto& Holders = FlockHolders[File->Inode];
    std::erase_if(Holders, [&](const auto& Weak) {
      const auto Holder = Weak.lock();
      return !Holder || Holder.get() == File;
    });
    if (File->Lock != FileLock::None) {
      // Linux does not guarantee atomic lock conversion.
      File->Lock = FileLock::None;
      ++IOEpoch;
    }
    if (Requested == FileLock::None) {
      if (Holders.empty()) FlockHolders.erase(File->Inode);
      return 0;
    }
    const bool Conflict = std::any_of(Holders.begin(), Holders.end(), [&](const auto& Weak) {
      const auto Holder = Weak.lock();
      return Holder && (Requested == FileLock::Exclusive || Holder->Lock == FileLock::Exclusive);
    });
    if (Conflict) return Operation & Nonblocking ? -11 : WaitForIO(WaitKind::Lock);
    File->Lock = Requested;
    Holders.emplace_back(Descriptors().Files[FD]);
    return 0;
  }

  struct ByteLock {
    uint64_t Start, End;
    int16_t Type;
    int32_t PID;
    std::weak_ptr<OpenFile> Description;
    std::weak_ptr<DescriptorState> Table;

    bool Expired() const { return PID == -1 ? Description.expired() : Table.expired(); }
  };

  bool RecordLockDeadlock(const ByteLock& Blocker) const {
    auto Owner = Blocker.Table.lock();
    // Follow the first conflicting lock for each sleeping owner, as Linux
    // does for POSIX locks. OFD locks do not participate in cycle detection.
    for (size_t Depth = 0; Owner && Depth < Threads.size(); ++Depth) {
      if (Owner == Current->Descriptors) return true;
      const auto Waiter = std::find_if(Threads.begin(), Threads.end(), [&](const auto& Thread) {
        return !Thread.Exited && Thread.Descriptors == Owner && Thread.PendingRecordLock.has_value();
      });
      if (Waiter == Threads.end()) break;
      const auto& Wait = *Waiter->PendingRecordLock;
      const auto Locks = RecordLocks.find(Wait.Inode);
      if (Locks == RecordLocks.end()) break;
      const auto Next = std::find_if(Locks->second.begin(), Locks->second.end(), [&](const ByteLock& Lock) {
        return !Lock.Expired() && Lock.Table.lock() != Owner && Wait.Start < Lock.End &&
               Lock.Start < Wait.End && (Wait.Type == 1 || Lock.Type == 1);
      });
      if (Next == Locks->second.end()) break;
      Owner = Next->Table.lock();
    }
    return false;
  }

  int64_t FileRecordLock(uint64_t FD, uint64_t Command, uint64_t Address) {
    Current->PendingRecordLock.reset();
    struct LinuxFlock {
      int16_t Type, Whence;
      int32_t Pad;
      int64_t Start, Length;
      int32_t PID, Tail;
    } Request {};
    static_assert(sizeof(LinuxFlock) == 32);
    constexpr int16_t ReadLock = 0, WriteLock = 1, Unlock = 2;
    const bool OFD = Command >= 36;
    const bool Query = Command == 5 || Command == 36;
    const bool Blocking = Command == 7 || Command == 38;
    if (AccessiblePrefix(Address, sizeof(Request), PF_R) != sizeof(Request)) return Fault;
    std::memcpy(&Request, reinterpret_cast<const void*>(Address), sizeof(Request));
    if (Request.Type < ReadLock || Request.Type > Unlock ||
        (Query && Request.Type == Unlock) || (OFD && Request.PID)) return Invalid;

    auto& File = *GetFile(FD);
    if (!Query && ((Request.Type == ReadLock && !ReadAccess(File.StatusFlags)) ||
                   (Request.Type == WriteLock && !WriteAccess(File.StatusFlags)))) return BadFile;
    int64_t Base {};
    if (Request.Whence == 1) Base = File.Offset;
    else if (Request.Whence == 2) {
      LinuxStat Info {};
      if (const auto Error = FileStat(FD, Info); Error) return Error;
      Base = Info.Size;
    } else if (Request.Whence != 0) return Invalid;

    int64_t Start {};
    if (__builtin_add_overflow(Base, Request.Start, &Start) || Start < 0) return Invalid;
    uint64_t End = uint64_t {1} << 63;
    if (Request.Length < 0) {
      End = Start;
      if (__builtin_add_overflow(Start, Request.Length, &Start) || Start < 0) return Invalid;
    } else if (Request.Length > 0) {
      if (static_cast<uint64_t>(Request.Length) > End - Start) return -75;
      End = static_cast<uint64_t>(Start) + Request.Length;
    }
    const auto Begin = static_cast<uint64_t>(Start);
    const auto Owned = [&](const ByteLock& Lock) {
      return OFD ? Lock.PID == -1 && Lock.Description.lock().get() == &File :
                   Lock.PID != -1 && Lock.Table.lock() == Current->Descriptors;
    };

    auto& Locks = RecordLocks[File.Inode];
    std::erase_if(Locks, [](const auto& Lock) { return Lock.Expired(); });
    const auto Conflict = std::find_if(Locks.begin(), Locks.end(), [&](const ByteLock& Lock) {
      return Begin < Lock.End && Lock.Start < End && !Owned(Lock) &&
             (Request.Type == WriteLock || (Request.Type == ReadLock && Lock.Type == WriteLock));
    });
    if (Query) {
      if (Conflict == Locks.end()) {
        Request.Type = Unlock;
      } else {
        Request.Type = Conflict->Type;
        Request.Whence = 0;
        Request.Start = Conflict->Start;
        Request.Length = Conflict->End == (uint64_t {1} << 63) ? 0 : Conflict->End - Conflict->Start;
        Request.PID = Conflict->PID;
      }
      if (Locks.empty()) RecordLocks.erase(File.Inode);
      if (AccessiblePrefix(Address, sizeof(Request), PF_W) != sizeof(Request)) return Fault;
      std::memcpy(reinterpret_cast<void*>(Address), &Request, sizeof(Request));
      return 0;
    }
    if (Conflict != Locks.end()) {
      if (!Blocking) return -11;
      if (!OFD) {
        if (RecordLockDeadlock(*Conflict)) return -35; // EDEADLK
        Current->PendingRecordLock = RecordLockWait {File.Inode, Begin, End, Request.Type};
      }
      return WaitForIO(WaitKind::Lock);
    }

    std::vector<ByteLock> Remaining;
    Remaining.reserve(Locks.size() + 2);
    for (const auto& Lock : Locks) {
      if (!Owned(Lock) || Begin >= Lock.End || Lock.Start >= End) {
        Remaining.push_back(Lock);
        continue;
      }
      if (Lock.Start < Begin) {
        auto Left = Lock;
        Left.End = Begin;
        Remaining.push_back(std::move(Left));
      }
      if (Lock.End > End) {
        auto Right = Lock;
        Right.Start = End;
        Remaining.push_back(std::move(Right));
      }
    }
    if (Request.Type != Unlock) {
      ByteLock Updated {Begin, End, Request.Type, OFD ? -1 : static_cast<int32_t>(Task->PID), {}, {}};
      if (OFD) Updated.Description = Descriptors().Files[FD];
      else Updated.Table = Current->Descriptors;
      std::erase_if(Remaining, [&](const ByteLock& Lock) {
        if (Lock.Type != Updated.Type || !Owned(Lock) || Lock.Start > Updated.End || Updated.Start > Lock.End) return false;
        Updated.Start = std::min(Updated.Start, Lock.Start);
        Updated.End = std::max(Updated.End, Lock.End);
        return true;
      });
      Remaining.push_back(std::move(Updated));
    }
    if (Remaining.empty()) RecordLocks.erase(File.Inode);
    else Locks.swap(Remaining);
    ++IOEpoch;
    return 0;
  }

  int64_t PipeCapacity(OpenFile& File, bool Set, uint32_t Argument) {
    constexpr uint32_t PipeMaximum = 1024 * 1024;
    constexpr uint32_t LinuxPage = 4096;
    if (!File.Pipe) return BadFile;
    if (!Set) return File.Socket->InputCapacity;
    if (Argument > INT32_MAX) return Invalid;
    if (Argument > PipeMaximum) return -1;

    const auto Capacity = std::bit_ceil(std::max(LinuxPage, Argument));
    const auto Peer = File.Socket->Peer.lock();
    if (File.Socket->UsedCapacity() > Capacity || (Peer && Peer->UsedCapacity() > Capacity)) return -16;
    File.Socket->InputCapacity = Capacity;
    if (Peer) Peer->InputCapacity = Capacity;
    ++IOEpoch;
    return Capacity;
  }

  int64_t FileControl(uint32_t FD, uint32_t Command, uint64_t Arg) {
    auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (Command == 1) return Descriptors().Flags[FD];
    if (Command == 2) {
      Descriptors().Flags[FD] = Arg & 1;
      return 0;
    }
    if (Command == 3) return File->StatusFlags;
    if (Command == 1031 || Command == 1032) return PipeCapacity(*File, Command == 1031, Arg);
    if (File->PathOnly() && Command != 0 && Command != 1030) return BadFile;
    if (Command == 4) {
      constexpr uint32_t MutableFlags = 04000 | 02000 | NoAtimeFlag; // O_NONBLOCK | O_APPEND
      if ((Arg & NoAtimeFlag) && !(File->StatusFlags & NoAtimeFlag) && !OwnsInodeOrRoot(File->Inode)) return -1;
      File->StatusFlags = (File->StatusFlags & ~MutableFlags) | (Arg & MutableFlags);
      if (File->Socket && !File->Fifo) File->Socket->Nonblocking = Arg & 04000;
      return 0;
    }
    if ((Command >= 5 && Command <= 7) || (Command >= 36 && Command <= 38)) return FileRecordLock(FD, Command, Arg);
    if (File->Memory && Command == 1034) return File->Memory->Seals; // F_GET_SEALS
    if (File->Memory && Command == 1033) { // F_ADD_SEALS
      constexpr auto SealWrite = GuestMemoryFile::SealWrite;
      if (Arg & ~(7ULL | SealWrite | GuestMemoryFile::SealFutureWrite | GuestMemoryFile::SealExec)) return Invalid;
      if (File->Memory->Seals & 1) return -1;
      if (!WriteAccess(File->StatusFlags)) return -1;
      if ((Arg & SealWrite) && !(File->Memory->Seals & SealWrite) &&
          std::any_of(Processes.begin(), Processes.end(), [&](const auto& Entry) {
            return !Entry.second.Exited && Entry.second.Memory.HasWritableFileMapping(File->Memory->Storage);
          })) return -16;
      File->Memory->Seals |= Arg;
      return 0;
    }
    if (Command == 0 || Command == 1030) {
      if (Arg >= Descriptors().Files.size()) return Invalid;
      const auto NewFD = FreeDescriptor(Arg);
      if (NewFD == NoDescriptor) return -24;
      Descriptors().Files[NewFD] = Descriptors().Files[FD];
      Descriptors().Flags[NewFD] = Command == 1030;
      return NewFD;
    }
    return Invalid;
  }

  uint64_t FreeDescriptor(uint64_t Start = 0) const {
    for (uint64_t FD = Start; FD < Descriptors().Files.size(); ++FD) {
      if (!Descriptors().Files[FD]) return FD;
    }
    return NoDescriptor;
  }

  int64_t CreateMemoryFile(uint64_t NameAddress, uint64_t Flags) {
    constexpr uint64_t CloseOnExec = 1, AllowSealing = 2, NoExecSeal = 8, Executable = 0x10;
    // MFD_HUGETLB is rejected as on kernels without hugetlbfs.
    if (Flags & ~(CloseOnExec | AllowSealing | NoExecSeal | Executable)) return Invalid;
    if ((Flags & NoExecSeal) && (Flags & Executable)) return Invalid;
    const auto Name = GuestString(NameAddress, 250, Invalid); // MFD_NAME_MAX_LEN 249
    if (!Name) return Name.Error;
    const auto FD = FreeDescriptor();
    if (FD == NoDescriptor) return -24;
    auto File = std::make_shared<OpenFile>();
    File->Path = "memfd:" + std::string {*Name};
    File->StatusFlags = 2;
    File->Inode = NextAnonymousInode++;
    File->Memory = std::make_shared<GuestMemoryFile>();
    // MFD_NOEXEC_SEAL implies MFD_ALLOW_SEALING, clears the execute bits
    // and seals them. Otherwise memfds are executable, as with the default
    // vm.memfd_noexec policy.
    File->Memory->Seals = (Flags & NoExecSeal) ? GuestMemoryFile::SealExec : (Flags & AllowSealing) ? 0u : 1u;
    if (Flags & NoExecSeal) Inodes[File->Inode].Mode = 0666;
    Descriptors().Files[FD] = std::move(File);
    Descriptors().Flags[FD] = (Flags & CloseOnExec) != 0;
    return FD;
  }

  static int64_t SdFileError(Result Status) {
    if (R_SUCCEEDED(Status)) return 0;
    if (R_MODULE(Status) == 2) {
      if (R_DESCRIPTION(Status) == 1) return -2; // PathNotFound
      if (R_DESCRIPTION(Status) == 2) return -17; // PathAlreadyExists
      if (R_DESCRIPTION(Status) == 6003) return -36; // TooLongPath; the runtime keeps such paths in the store
    }
    // Keep the Horizon result so unmapped failures can be classified.
    char Message[64];
    std::snprintf(Message, sizeof(Message), "SD result 2%03u-%04u (0x%08x) reported as EIO",
                  R_MODULE(Status), R_DESCRIPTION(Status), Status);
    WriteGuestOutput(Message); // Kept with the program output, not trimmed with the trace.
    return -5;
  }

  static constexpr const char* DirectoryIntentPath = InodeFiles::DirectoryIntent;
  static constexpr const char* DirectoryBackupPath = InodeFiles::DirectoryBackup;
  static constexpr const char* IntentStagingPath = InodeFiles::IntentStaging;
  static constexpr const char* PromotionPath = InodeFiles::Promotion;
  static constexpr const char* const* InodeStorePaths = InodeFiles::Stores;

  static bool IsInodeStorePath(std::string_view Path) {
    const auto Equal = [](std::string_view A, std::string_view B) {
      return std::ranges::equal(A, B, [](char Left, char Right) {
        if (Right >= 'A' && Right <= 'Z') Right += 'a' - 'A';
        return Left == Right;
      });
    };
    // The SD card compares names without case, so hide the whole metadata
    // folder under any spelling.
    const std::string_view Folder = InodeFiles::Directory;
    return Path.size() >= Folder.size() && Equal(Path.substr(0, Folder.size()), Folder) &&
           (Path.size() == Folder.size() || Path[Folder.size()] == '/');
  }

  static int64_t ReadInodeSlot(FsFileSystem& SdCard, const char* Path, std::vector<uint8_t>& Bytes) {
    FsFile File {};
    auto Error = SdFileError(fsFsOpenFile(&SdCard, Path, FsOpenMode_Read, &File));
    if (Error) return Error;
    s64 Size {};
    Error = SdFileError(fsFileGetSize(&File, &Size));
    if (!Error && (Size <= 0 || static_cast<uint64_t>(Size) > InodeSnapshot::MaxBytes)) Error = -5;
    if (!Error) {
      Bytes.resize(Size);
      u64 Read {};
      Error = SdFileError(fsFileRead(&File, 0, Bytes.data(), Bytes.size(), FsReadOption_None, &Read));
      if (!Error && Read != Bytes.size()) Error = -5;
      if (Error) Bytes.clear();
    }
    fsFileClose(&File);
    return Error;
  }

  static int64_t WriteInodeFile(FsFileSystem& SdCard, const char* Path, std::span<const uint8_t> Bytes) {
    FsFile File {};
    auto Error = SdFileError(fsFsOpenFile(&SdCard, Path, FsOpenMode_Write | FsOpenMode_Append, &File));
    if (Error) return Error;
    Error = SdFileError(fsFileSetSize(&File, Bytes.size()));
    if (!Error) Error = SdFileError(fsFileWrite(&File, 0, Bytes.data(), Bytes.size(), FsWriteOption_Flush));
    fsFileClose(&File);
    return Error;
  }

  static int64_t FinishInodeIntent(FsFileSystem& SdCard, const char* Path) {
    if (const auto Error = SdFileError(fsFsCommit(&SdCard)); Error) return Error;
    if (const auto Error = SdFileError(fsFsDeleteFile(&SdCard, Path)); Error) return Error;
    return SdFileError(fsFsCommit(&SdCard));
  }

  int64_t PublishInodeIntent(FsFileSystem& SdCard, const char* Destination, std::span<const uint8_t> Bytes) {
    auto Error = SdFileError(fsFsCreateFile(&SdCard, IntentStagingPath, 0, 0));
    if (Error) return Error;
    Error = WriteInodeFile(SdCard, IntentStagingPath, Bytes);
    if (!Error) Error = SdFileError(fsFsRenameFile(&SdCard, IntentStagingPath, Destination));
    if (Error) {
      fsFsDeleteFile(&SdCard, IntentStagingPath);
      return Error;
    }
    Error = SdFileError(fsFsCommit(&SdCard));
    if (Error) {
      // The intent may already be durable. Leave it for recovery, and do
      // not start the native mutation or another transaction in this run.
      InodeStoreAvailable = false;
    }
    return Error;
  }

  int64_t BeginPromotion(FsFileSystem& SdCard, const std::string& Path, uint64_t Inode) {
    if (InodeGeneration == UINT64_MAX) return -5;
    const auto Bytes = EncodeInodeSnapshot(InodeGeneration + 1, NextSdInode, {{Path, Inode}}, {});
    if (Bytes.empty()) return -28;
    return PublishInodeIntent(SdCard, PromotionPath, Bytes);
  }

  int64_t RecoverPromotion(FsFileSystem& SdCard) {
    std::vector<uint8_t> Bytes;
    const auto Error = ReadInodeSlot(SdCard, PromotionPath, Bytes);
    if (Error == -2) return 0;
    if (Error) return Error;
    const auto Intent = DecodeInodeSnapshot(Bytes);
    if (!Intent || Intent->Names.size() != 1 || !Intent->Records.empty()) return -5;
    // A later checkpoint can rename or delete the committed object. The
    // older recovery record must not undo that newer namespace state.
    if (InodeGeneration > Intent->Generation) return FinishInodeIntent(SdCard, PromotionPath);
    const auto& [Path, Inode] = *Intent->Names.begin();
    const auto Object = ObjectPath(Inode);
    FsDirEntryType Type {};
    const auto ObjectStatus = SdFileError(fsFsGetEntryType(&SdCard, Object.c_str(), &Type));
    if (ObjectStatus && ObjectStatus != -2) return ObjectStatus;
    if (!ObjectStatus && Type != FsDirEntryType_File) return -5;
    const auto OriginalStatus = SdFileError(fsFsGetEntryType(&SdCard, Path.c_str(), &Type));
    if (OriginalStatus && OriginalStatus != -2) return OriginalStatus;

    const auto Name = SdInodes.find(Path);
    const bool Committed = InodeGeneration >= Intent->Generation && Name != SdInodes.end() &&
                           Name->second == Inode && IsManagedFile(Path);
    if (Committed) {
      if (ObjectStatus || OriginalStatus != -2) return -5;
    } else if (!ObjectStatus && OriginalStatus == -2) {
      if (const auto Move = SdFileError(fsFsRenameFile(&SdCard, Object.c_str(), Path.c_str())); Move < 0) return Move;
    } else if (ObjectStatus != -2 || OriginalStatus || Type != FsDirEntryType_File) {
      return -5;
    }
    return FinishInodeIntent(SdCard, PromotionPath);
  }

  int64_t RecoverDirectoryRename(FsFileSystem& SdCard) {
    std::vector<uint8_t> Bytes;
    const auto Error = ReadInodeSlot(SdCard, DirectoryIntentPath, Bytes);
    if (Error == -2) return 0;
    if (Error) return Error;
    const auto Intent = DecodeInodeSnapshot(Bytes);
    if (!Intent || Intent->Names.size() != 2 || !Intent->Records.empty()) return -5;
    const auto First = InodeSnapshot::FirstInode;
    if (Intent->NextInode < First + 2 || Intent->NextInode > First + 10) return -5;
    const bool Exchange = Intent->NextInode == First + 3;
    const bool VirtualDestination = Intent->NextInode >= First + 4;
    const bool Create = Intent->NextInode >= First + 9;
    const bool Remove = !Create && Intent->NextInode >= First + 7;
    const bool File = Intent->NextInode == First + 6 || Intent->NextInode == First + 8 || Intent->NextInode == First + 10;
    const auto EntryType = File ? FsDirEntryType_File : FsDirEntryType_Dir;
    const std::string* Old = nullptr;
    const std::string* New = nullptr;
    for (const auto& [Path, Role] : Intent->Names) {
      if (Role == InodeSnapshot::FirstInode) Old = &Path;
      if (Role == InodeSnapshot::FirstInode + 1) New = &Path;
    }
    if (!Old || !New || PathContains(*Old, *New) || PathContains(*New, *Old)) return -5;

    const auto Check = [&](const char* Path) -> int64_t {
      FsDirEntryType Type {};
      const auto Status = SdFileError(fsFsGetEntryType(&SdCard, Path, &Type));
      return !Status && Type != EntryType ? -20 : Status;
    };
    const auto OldState = Check(Old->c_str());
    const auto NewState = Check(New->c_str());
    if (Remove && *New != DirectoryBackupPath) return -5;
    if (Create && *Old != DirectoryBackupPath) return -5;
    const auto Backup = Remove || Create ? -2 : Check(DirectoryBackupPath);
    if ((OldState && OldState != -2) || (NewState && NewState != -2) || (Backup && Backup != -2)) return -5;
    const auto Kind = Create ? DirectoryRenameKind::Create :
                      Remove ? DirectoryRenameKind::Remove :
                      VirtualDestination ? DirectoryRenameKind::ExchangeWithVirtual :
                      Exchange ? DirectoryRenameKind::Exchange : DirectoryRenameKind::Replace;
    const auto Plan = PlanDirectoryRecovery(Kind, InodeGeneration >= Intent->Generation,
                                            !OldState, !NewState, !Backup);
    if (Plan == DirectoryRecovery::Invalid) return -5;

    const auto Move = [&](const char* From, const char* To) {
      return SdFileError(EntryType == FsDirEntryType_File ? fsFsRenameFile(&SdCard, From, To) :
                                                           fsFsRenameDirectory(&SdCard, From, To));
    };
    int64_t RecoveryError {};
    switch (Plan) {
    case DirectoryRecovery::RestoreSource:
    case DirectoryRecovery::RestoreBoth:
      RecoveryError = Move(New->c_str(), Old->c_str());
      if (RecoveryError || Plan == DirectoryRecovery::RestoreSource) break;
      [[fallthrough]];
    case DirectoryRecovery::RestoreDestination:
      RecoveryError = Move(DirectoryBackupPath, New->c_str());
      break;
    case DirectoryRecovery::RemoveBackup:
      RecoveryError = SdFileError(File ? fsFsDeleteFile(&SdCard, DirectoryBackupPath) :
                                         fsFsDeleteDirectory(&SdCard, DirectoryBackupPath));
      break;
    case DirectoryRecovery::CompleteExchange:
      RecoveryError = Move(DirectoryBackupPath, Old->c_str());
      break;
    case DirectoryRecovery::Finish:
    case DirectoryRecovery::Invalid:
      break;
    }
    if (RecoveryError) return RecoveryError;
    return FinishInodeIntent(SdCard, DirectoryIntentPath);
  }

  void LoadInodeStore() {
    FsFileSystem SdCard {};
    if (R_FAILED(fsOpenSdCardFileSystem(&SdCard))) return;
    std::vector<uint8_t> First, Second;
    const auto A = ReadInodeSlot(SdCard, InodeStorePaths[0], First);
    const auto B = ReadInodeSlot(SdCard, InodeStorePaths[1], Second);
    auto Latest = DecodeInodeSnapshot(First);
    auto Older = DecodeInodeSnapshot(Second);
    if (Older && (!Latest || Older->Generation > Latest->Generation)) Latest.swap(Older);
    if (Latest) {
      InodeGeneration = Latest->Generation;
      NextSdInode = Latest->NextInode;
      SdInodes = std::move(Latest->Names);
      Inodes = std::move(Latest->Records);
      RetiredBackingObjects = std::move(Latest->RetiredBackingObjects);
      InodeStoreAvailable = true;
    } else if (A == -2 && B == -2) {
      InodeStoreAvailable = true;
    } else {
      WriteLog("Guest inode metadata could not be recovered.");
    }
    if (InodeStoreAvailable && RecoverDirectoryRename(SdCard) < 0) {
      InodeStoreAvailable = false;
      WriteLog("Guest directory rename could not be recovered.");
    }
    if (InodeStoreAvailable && RecoverPromotion(SdCard) < 0) {
      InodeStoreAvailable = false;
      WriteLog("Guest inode promotion could not be recovered.");
    }
    if (InodeStoreAvailable) {
      const auto Cleanup = SdFileError(fsFsDeleteFile(&SdCard, IntentStagingPath));
      if (Cleanup && Cleanup != -2) {
        InodeStoreAvailable = false;
        WriteLog("Guest inode intent staging file could not be removed.");
      }
    }
    if (InodeStoreAvailable) {
      // At startup there are no live guest inode references. Both slots
      // must record retirement before an object can be deleted.
      for (auto It = RetiredBackingObjects.begin(); It != RetiredBackingObjects.end();) {
        InodeStoreDirty = true;
        if (Older && Older->Generation != InodeGeneration && CanReclaimInode(*It, *Older)) {
          const auto Error = SdFileError(fsFsDeleteFile(&SdCard, ObjectPath(*It).c_str()));
          if (!Error || Error == -2) {
            It = RetiredBackingObjects.erase(It);
            continue;
          }
        }
        ++It;
      }
      if (ReclaimUnlinkedObjects(SdCard, Older ? &*Older : nullptr)) {
        WriteLog("Guest inode store could not commit unlinked backing cleanup.");
      }
      // A reset during ConvertToOverflow can leave the empty native
      // directory behind its overflow record. Remove it so a later rmdir of
      // the overflow directory cannot uncover it.
      for (const auto& [Path, Inode] : SdInodes) {
        if (Path.size() > SdPathLimit || !IsOverflowDirectory(Path)) continue;
        const auto Error = SdFileError(fsFsDeleteDirectory(&SdCard, Path.c_str()));
        if (Error && Error != -2) WriteLog("A native directory shadowed by an overflow directory could not be removed.");
      }
    }
    fsFsClose(&SdCard);
  }

  int64_t ReclaimUnlinkedObjects(FsFileSystem& SdCard, const InodeSnapshot* Older) {
    std::set<uint64_t> Live;
    for (const auto& Entry : DiskFiles) {
      if (const auto File = Entry.lock()) Live.insert(File->Inode);
    }
    std::vector<uint64_t> Removed;
    for (const auto& [Inode, Record] : Inodes) {
      if (!Record.ManagedFile || !Record.Unlinked || Live.contains(Inode)) continue;
      const auto Object = Record.BackingObject.value_or(Inode);
      InodeStoreDirty = true;
      if (Older && Older->Generation < InodeGeneration && CanReclaimInode(Object, *Older)) {
        const auto Error = SdFileError(fsFsDeleteFile(&SdCard, ObjectPath(Object).c_str()));
        if (!Error || Error == -2) Removed.push_back(Inode);
      }
    }
    if (Removed.empty()) return 0;
    if (const auto Error = SdFileError(fsFsCommit(&SdCard)); Error) return Error;
    for (const auto Inode : Removed) Inodes.erase(Inode);
    return 0;
  }

  int64_t SaveInodeStore() {
    if (!InodeStoreDirty) return 0;
    if (!InodeStoreAvailable || InodeGeneration == UINT64_MAX) return -5;
    const auto Generation = InodeGeneration + 1;
    const auto Bytes = EncodeInodeSnapshot(Generation, NextSdInode, SdInodes, Inodes, RetiredBackingObjects);
    if (Bytes.empty()) return -28;

    FsFileSystem SdCard {};
    if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
    const auto* Path = InodeStorePaths[Generation % 2];
    auto Error = WriteInodeFile(SdCard, Path, Bytes);
    if (Error == -2) {
      Error = SdFileError(fsFsCreateFile(&SdCard, Path, 0, 0));
      if (!Error) Error = WriteInodeFile(SdCard, Path, Bytes);
    }
    fsFsClose(&SdCard);
    if (!Error) {
      InodeGeneration = Generation;
      InodeStoreDirty = false;
    }
    return Error;
  }

  int64_t CommitSdInodeStore() {
    if (const auto Error = SaveInodeStore(); Error) return Error;
    FsFileSystem SdCard {};
    if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
    const auto Error = SdFileError(fsFsCommit(&SdCard));
    fsFsClose(&SdCard);
    return Error;
  }

  int64_t TruncatePath(uint64_t Address, uint64_t Size) {
    if (Size > INT64_MAX) return Invalid;
    const auto Name = GuestString(Address);
    if (!Name) return Name.Error;
    FsFileSystem SdCard {};
    if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
    std::string Path;
    auto Error = ResolveSdPath(CurrentDirectory, *Name, SdCard, Path);
    if (!Error && VirtualMount(Path)) {
      const auto GuestPath = std::string_view(Path).substr(RootPath.size());
      if (!VirtualDirectory(Path).empty()) Error = -21;
      else if (ProcFile(GuestPath)) Error = Name->ends_with('/') ? -20 : -13;
      else if (GuestPath == "/dev/null" || (Interactive && GuestPath == "/dev/tty")) {
        Error = Name->ends_with('/') ? -20 : Invalid;
      }
    }
    if (!Error && SpecialNodeAt(Path)) Error = Name->ends_with('/') ? -20 : Invalid;
    const auto Backing = FileBackingPath(Path);
    FsDirEntryType Type {};
    if (!Error) Error = EntryType(SdCard, Path, Type);
    if (!Error && Type == FsDirEntryType_Dir) Error = -21;
    if (!Error && Name->ends_with('/')) Error = -20;
    if (!Error) {
      const auto Inode = SdInode(Path);
      if (!InodeAllows(Inode, ModeForInode(Inode, RegularMode(Path)), 2)) Error = -13;
      else if (InodeExecuting(Inode)) Error = -26;
      else ImportTimestamps(SdCard, Backing, Inode);
    }
    fsFsClose(&SdCard);
    if (Error) return Error;
    OpenFile File;
    File.Path = std::move(Path);
    File.Inode = SdInode(File.Path);
    File.RealFile = true;
    Error = ResizeDiskFile(File, Size);
    if (!Error) MarkFileDataChanged(File.Inode);
    return Error;
  }

  int64_t TruncateDescriptor(uint64_t FD, uint64_t Size) {
    if (Size > INT64_MAX) return Invalid;
    auto* File = GetFile(FD);
    if (!File || File->PathOnly()) return BadFile;
    if (!WriteAccess(File->StatusFlags)) return Invalid;
    if (!File->Memory && !File->RealFile) return Invalid;
    const auto Error = File->Memory ? File->Memory->Resize(Size) : ResizeDiskFile(*File, Size);
    if (!Error) MarkFileDataChanged(File->Inode);
    return Error;
  }

  struct FileReadCache {
    uint64_t Inode {}, Offset {};
    size_t Size {};
    std::array<uint8_t, 64 * 1024> Buffer {};
  };

  int64_t PrefetchFile(OpenFile& File, uint64_t Offset, uint64_t Count) {
    Count = BoundDiskRead(File.Inode, Offset, Count);
    if (!File.RealFile || !Count) return 0;
    ReadCache.Size = 0;
    u64 Read {};
    const auto Error = ReadDiskData(File, ReadCache.Buffer.data(), std::min<uint64_t>(Count, ReadCache.Buffer.size()), Offset, Read);
    if (Error) return Error;
    ReadCache.Inode = File.Inode;
    ReadCache.Offset = Offset;
    ReadCache.Size = Read;
    return 0;
  }

  int64_t ReadaheadGuest(uint64_t FD, uint64_t Offset, uint64_t Count) {
    auto* File = GetFile(FD);
    if (!File || File->PathOnly() || !ReadAccess(File->StatusFlags)) return BadFile;
    if (Offset > INT64_MAX || (!File->RealFile && !File->Memory && !ProcFile(File->Path))) return Invalid;
    return PrefetchFile(*File, Offset, std::min<uint64_t>(Count, INT64_MAX - Offset));
  }

  int64_t AdviseFile(uint64_t FD, uint64_t Offset, uint64_t Count, uint32_t Advice) {
    auto* File = GetFile(FD);
    if (!File || File->PathOnly()) return BadFile;
    if (File->Pipe) return -29;
    if (Offset > INT64_MAX || Count > INT64_MAX || Advice > 5) return Invalid;
    if (!File->RealFile) return 0; // A memory-backed file has no SD cache to manage.
    switch (Advice) {
    case 0: case 1: case 2: case 5:
      File->ReadAdvice = Advice;
      break;
    case 3: // WILLNEED
      if (ReadAccess(File->StatusFlags)) PrefetchFile(*File, Offset, Count ? Count : ReadCache.Buffer.size());
      break;
    case 4: // DONTNEED
      if (ReadCache.Inode == File->Inode && ReadCache.Size &&
          Offset < ReadCache.Offset + ReadCache.Size && (!Count || ReadCache.Offset < Offset + Count)) {
        ReadCache.Size = 0;
      }
      break;
    }
    return 0;
  }

  void InvalidateReadCache(uint64_t Inode) {
    if (ReadCache.Inode == Inode) ReadCache.Size = 0;
  }

  int64_t AllocateFileRange(uint32_t FD, uint32_t Mode, uint64_t Offset, uint64_t Length) {
    constexpr uint64_t KeepSize = 1, PunchHole = 2, CollapseRange = 8, ZeroRange = 0x10, InsertRange = 0x20;
    constexpr uint32_t UnshareRange = 0x40;
    const auto* File = GetFile(FD);
    if (!File || File->PathOnly()) return BadFile;
    if (!Length || Offset > INT64_MAX || Length > INT64_MAX) return Invalid;
    const bool Shift = Mode == CollapseRange || Mode == InsertRange;
    const bool Unshare = (Mode & ~KeepSize) == UnshareRange;
    if (!Shift && !Unshare && (Mode & ~(KeepSize | ZeroRange)) && Mode != (KeepSize | PunchHole)) return -95;
    if (!WriteAccess(File->StatusFlags)) return BadFile;
    if (File->Pipe) return -29; // ESPIPE
    if (File->Directory) return -21;
    if (!File->RealFile && !File->Memory) return -19; // ENODEV
    if (Length > INT64_MAX - Offset) return -27;
    if (Unshare) return -95; // The SD and memfd backends do not support reflinks.

    const uint64_t End = Offset + Length;
    if (File->Memory) {
      if (Shift || (Mode & ZeroRange)) return -95;
      const auto Error = Mode & PunchHole ? File->Memory->Punch(Offset, End) :
                         File->Memory->Allocate(Offset, End, Mode & KeepSize);
      if (Error) return Error;
      MarkFileDataChanged(File->Inode);
      return 0;
    }

    s64 Visible {};
    if (const auto Error = DiskSize(*File, Visible); Error) return Error;
    if (Shift) {
      if (InodeExecuting(File->Inode)) return -26;
      const bool Insert = Mode == InsertRange;
      if (Offset % GuestPageSize || Length % GuestPageSize ||
          (Insert ? Offset : End) >= static_cast<uint64_t>(Visible)) return Invalid;
      if (Insert && Length > static_cast<uint64_t>(INT64_MAX - Visible)) return -27;
      const auto Publish = [&](const PackedFileExtents& Source) {
        if (Insert && !Source.Extents().empty() && Length > INT64_MAX - Source.Extents().back().End) return int64_t {-27};
        const auto Size = Insert ? Visible + Length : Visible - Length;
        return ReplacePackedBacking(*File, Source, Source.ShiftRange(Offset, Length, Insert), Size,
                                    [](uint64_t, std::span<char>) {}, Insert ? End : Offset,
                                    Insert ? -static_cast<int64_t>(Length) : static_cast<int64_t>(Length));
      };
      int64_t Error {};
      if (const auto* Extents = FileExtents(File->Inode)) {
        Error = Publish(*Extents);
      } else {
        s64 Allocated {};
        if (const auto Result = DiskAllocation(*File, Allocated); Result) return Result;
        const auto Dense = Allocated ? PackedFileExtents({{0, static_cast<uint64_t>(Allocated)}}) : PackedFileExtents({});
        if (const auto Result = EnsureLogicalDiskSize(*File, Visible); Result) return Result;
        Error = Publish(Dense);
      }
      if (!Error) MarkFileDataChanged(File->Inode);
      return Error;
    }
    if (const auto* Extents = FileExtents(File->Inode)) {
      auto Destination = *Extents;
      if (Mode & PunchHole) {
        if (Extents->Extents().empty() || Offset >= Extents->Extents().back().End) return 0;
        const auto Punch = PlanFilePunch(Extents->Extents().back().End, Offset, End, GuestPageSize);
        Destination = Destination.Remove(Punch.Reclaim.Begin, Punch.Reclaim.End);
      } else {
        Destination = AllocatePackedRange(Destination, Offset, End);
      }
      const auto Size = Mode & KeepSize ? static_cast<uint64_t>(Visible) : std::max<uint64_t>(Visible, End);
      const auto Error = ReplacePackedBacking(*File, *Extents, std::move(Destination), Size,
        [&](uint64_t Position, std::span<char> Chunk) {
          if (!(Mode & (PunchHole | ZeroRange))) return;
          const auto Begin = std::max(Position, Offset);
          const auto Limit = std::min(Position + Chunk.size(), End);
          if (Begin < Limit) std::memset(Chunk.data() + Begin - Position, 0, Limit - Begin);
        });
      if (!Error) MarkFileDataChanged(File->Inode);
      return Error;
    }
    uint64_t ClearEnd {};
    std::optional<uint64_t> ReclaimTail;
    if (Mode & PunchHole) {
      s64 Allocated {};
      if (const auto Error = DiskAllocation(*File, Allocated); Error) return Error;
      if (Offset >= static_cast<uint64_t>(Allocated)) return 0;
      const auto Punch = PlanFilePunch(Allocated, Offset, End, GuestPageSize);
      ClearEnd = Punch.ClearBefore.End;
      if (!Punch.Reclaim.Empty()) {
        if (Punch.Reclaim.End != static_cast<uint64_t>(Allocated)) {
          if (const auto Error = EnsureLogicalDiskSize(*File, Visible); Error) return Error;
          const PackedFileExtents Dense({{0, static_cast<uint64_t>(Allocated)}});
          const auto Error = ReplacePackedBacking(*File, Dense, Dense.Remove(Punch.Reclaim.Begin, Punch.Reclaim.End), Visible,
            [&](uint64_t Position, std::span<char> Chunk) {
              const auto Begin = std::max(Position, Offset);
              const auto Limit = std::min(Position + Chunk.size(), End);
              if (Begin < Limit) std::memset(Chunk.data() + Begin - Position, 0, Limit - Begin);
            });
          if (!Error) MarkFileDataChanged(File->Inode);
          return Error;
        }
        ReclaimTail = Punch.Reclaim.Begin;
      }
    }
    if (ReclaimTail || ((Mode & KeepSize) && !(Mode & PunchHole) && End > static_cast<uint64_t>(Visible))) {
      if (const auto Error = EnsureLogicalDiskSize(*File, Visible); Error) return Error;
    }
    InvalidateReadCache(File->Inode);
    const auto Error = WithDiskFile(*File, FsOpenMode_Write | FsOpenMode_Append, [&](FsFile& Native) {
      if (Mode & PunchHole) {
        if (const auto Clear = ZeroDiskRange(Native, Offset, ClearEnd); Clear) return Clear;
        return ReclaimTail ? SdFileError(fsFileSetSize(&Native, *ReclaimTail)) : int64_t {0};
      }
      if (Mode & ZeroRange) {
        s64 Allocated {};
        if (const auto Status = SdFileError(fsFileGetSize(&Native, &Allocated)); Status) return Status;
        auto Begin = std::min<uint64_t>(Offset, Allocated);
        const bool Grow = !(Mode & KeepSize) && End > static_cast<uint64_t>(Visible);
        if (Grow) Begin = std::min<uint64_t>(Begin, Visible);
        if (const auto Clear = ZeroDiskRange(Native, Begin, End); Clear) {
          if (End > static_cast<uint64_t>(Allocated) && R_FAILED(fsFileSetSize(&Native, Allocated))) {
            WriteLog("Guest zero-range could not restore native allocation after a write failure.");
          }
          return Clear;
        }
        if (Grow && LogicalSize(File->Inode)) {
          Inodes.at(File->Inode).LogicalSize = End;
          InodeStoreDirty = true;
        }
        return int64_t {0};
      }
      if (Mode & KeepSize) {
        s64 Allocated {};
        if (const auto Status = SdFileError(fsFileGetSize(&Native, &Allocated)); Status) return Status;
        if (End > static_cast<uint64_t>(Allocated)) {
          if (const auto Grow = ResizeNativeFile(Native, End); Grow) return Grow;
        }
      } else {
        if (End > static_cast<uint64_t>(Visible)) {
          if (const auto Grow = ResizeDiskInode(Native, File->Inode, End); Grow) return Grow;
        } else if (LogicalSize(File->Inode)) {
          s64 Allocated {};
          if (const auto Status = SdFileError(fsFileGetSize(&Native, &Allocated)); Status) return Status;
          if (End > static_cast<uint64_t>(Allocated)) {
            if (const auto Grow = ResizeNativeFile(Native, End); Grow) return Grow;
          }
        }
      }
      return int64_t {0};
    });
    if (Error) return Error;
    MarkFileDataChanged(File->Inode);
    return 0;
  }

  int64_t SyncFileRange(uint64_t FD, uint64_t Offset, uint64_t Length, uint32_t Flags) {
    const auto* File = GetFile(FD);
    if (!File || File->PathOnly()) return BadFile;
    if ((Flags & ~7U) || Offset > INT64_MAX || Length > INT64_MAX - Offset) return Invalid;
    if (DescriptorFilesystem(*File) == FilesystemType::Proc) return 0;
    if (!File->RealFile && !File->Memory && !File->Directory) return -29;
    if (!Flags || File->Memory) return 0;
    if (File->Directory) return SyncSdFilesystem();
    // Ounce exposes whole-file flush. This also waits for any prior write.
    if (!WriteAccess(File->StatusFlags) && !DirtyFileData.contains(File->Inode)) return 0;
    return FlushDiskFile(*File);
  }

  struct LinuxStatFS {
    int64_t Type, BlockSize;
    uint64_t Blocks, Free, Available, Files, FreeFiles;
    int32_t ID[2];
    int64_t NameLength, FragmentSize, Flags, Spare[4];
  };
  static_assert(sizeof(LinuxStatFS) == 120);

  int64_t StatSdFilesystem(uint64_t Output) {
    if (Available(Output, PF_W) < sizeof(LinuxStatFS)) return Fault;
    LinuxStatFS Info {};
    if (const auto Error = QuerySdFilesystem(Info); Error) return Error;
    return CopyOut(Output, Info);
  }

  int64_t QuerySdFilesystem(LinuxStatFS& Info) {
    FsFileSystem SdCard {};
    if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
    s64 Total {}, Free {};
    auto Error = SdFileError(fsFsGetTotalSpace(&SdCard, "/", &Total));
    if (!Error) Error = SdFileError(fsFsGetFreeSpace(&SdCard, "/", &Free));
    fsFsClose(&SdCard);
    if (Error) return Error;
    if (Total < 0 || Free < 0 || Free > Total) return -5;

    Info = {};
    Info.Type = 0x4d44; // Linux FAT-family magic for the SD backing store.
    Info.BlockSize = Info.FragmentSize = 512;
    Info.Blocks = Total / Info.FragmentSize;
    Info.Free = Info.Available = Free / Info.FragmentSize;
    Info.ID[0] = 1;
    Info.NameLength = 255;
    Info.Flags = 0x20; // ST_VALID
    return 0;
  }

  // ustat(2) reports free blocks and inodes for a mounted device number.
  int64_t UstatGuest(uint64_t Device, uint64_t Output) {
    struct LinuxUstat {
      int32_t FreeBlocks;
      uint64_t FreeInodes;
      char Name[6], Pack[6];
    } Result {};
    static_assert(sizeof(LinuxUstat) == 32);
    const auto Dev = static_cast<uint32_t>(Device);
    if (Dev == 1) {
      LinuxStatFS Info {};
      if (const auto Error = QuerySdFilesystem(Info); Error) return Error;
      Result.FreeBlocks = static_cast<int32_t>(Info.Free);
      Result.FreeInodes = Info.FreeFiles;
    } else if (Dev != ProcDevice && Dev != DevDevice) {
      return Invalid;
    }
    return CopyOut(Output, Result);
  }

  // File handles identify SD inodes by stable inode number, in the tmpfs
  // layout: generation, low inode word, high inode word. Inode numbers are
  // never reused, so the generation is zero. Like tmpfs, the SD export has
  // no parent decoding, so connectable handles are unsupported. Other
  // filesystems only encode AT_HANDLE_FID identifiers (FILEID_INO64_GEN),
  // as Linux does without export operations.
  static constexpr int32_t HandleInoGen = 1, HandleFid = 0x81, HandleInvalid = 255;
  static constexpr uint32_t HandleConnectable = 0x10000, HandleDirectory = 0x20000;

  int64_t NameToHandle(uint64_t FD, uint64_t Address, uint64_t Handle, uint64_t MountID, uint64_t RawFlags) {
    constexpr uint32_t UniqueMount = 1, Connectable = 2, Fid = 0x200, Follow = 0x400, EmptyPath = 0x1000;
    const auto Flags = static_cast<uint32_t>(RawFlags);
    if (Flags & ~(UniqueMount | Connectable | Fid | Follow | EmptyPath)) return Invalid;
    if ((Flags & Connectable) && (Flags & Fid)) return Invalid;
    const auto Name = GuestString(Address);
    if (!Name) return Name.Error;
    LinuxStat Info {};
    bool Decodable {};
    if (Name->empty() && (Flags & EmptyPath) && !IsCwd(FD)) {
      const auto* File = GetFile(FD);
      if (!File) return BadFile;
      if (const auto Error = FileStat(FD, Info); Error) return Error;
      Decodable = (File->RealFile || File->Fifo || (File->Directory && VirtualDirectory(File->Path).empty())) &&
                  File->Path.starts_with(RootPath) && !IsUnlinked(File->Inode) && !File->RemovedDirectory;
    } else {
      std::string Path;
      const auto Error = PathStat(FD, Name->empty() && (Flags & EmptyPath) ? "." : *Name, Info, &Path, Flags & Follow);
      if (Error) return Error;
      Decodable = Info.Device == 1 && Path.starts_with(RootPath) && !VirtualMount(Path);
    }
    if ((!Decodable && !(Flags & Fid)) || (Flags & Connectable)) return -95;
    struct { uint32_t Bytes; int32_t Type; } Header {};
    if (AccessiblePrefix(Handle, sizeof(Header), PF_R) != sizeof(Header)) return Fault;
    std::memcpy(&Header, reinterpret_cast<const void*>(Handle), sizeof(Header));
    if (Header.Bytes > 128) return Invalid;
    const uint32_t Needed = 12;
    std::array<uint32_t, 3> Words {static_cast<uint32_t>(Info.Inode), static_cast<uint32_t>(Info.Inode >> 32), 0};
    if (Decodable) Words = {0, Words[0], Words[1]};
    int64_t Result = 0;
    uint32_t Copied = Needed;
    if (Header.Bytes < Needed) {
      Header.Type = HandleInvalid;
      Copied = 0;
      Result = -75; // EOVERFLOW
    } else {
      Header.Type = Decodable ? HandleInoGen : HandleFid;
    }
    Header.Bytes = Needed;
    const bool Unique = Flags & UniqueMount;
    const auto MountBytes = Unique ? 8u : 4u;
    if (Available(MountID, PF_W) < MountBytes) Result = Fault;
    else if (Unique) CopyOut(MountID, uint64_t {1});
    else CopyOut(MountID, int32_t {1});
    if (Available(Handle, PF_W) < sizeof(Header) + Copied) return Fault;
    std::memcpy(reinterpret_cast<void*>(Handle), &Header, sizeof(Header));
    std::memcpy(reinterpret_cast<void*>(Handle + sizeof(Header)), Words.data(), Copied);
    return Result;
  }

  int64_t OpenByHandle(uint64_t MountFD, uint64_t Handle, uint64_t RawFlags) {
    bool SdMount = true;
    if (IsCwd(MountFD)) {
      SdMount = !VirtualMount(Task->Folder);
    } else {
      const auto* File = GetFile(MountFD);
      if (!File) return BadFile;
      SdMount = DescriptorFilesystem(*File) == FilesystemType::Sd && !VirtualMount(File->Path);
    }
    if (Task->UserIDs[1] != 0) return -1; // CAP_DAC_READ_SEARCH
    struct { uint32_t Bytes; int32_t Type; } Header {};
    if (AccessiblePrefix(Handle, sizeof(Header), PF_R) != sizeof(Header)) return Fault;
    std::memcpy(&Header, reinterpret_cast<const void*>(Handle), sizeof(Header));
    if (Header.Bytes > 128 || !Header.Bytes) return Invalid;
    if (Header.Type < 0 || (static_cast<uint32_t>(Header.Type) & 0xffff0000u & ~(HandleConnectable | HandleDirectory))) {
      return Invalid;
    }
    if (AccessiblePrefix(Handle + sizeof(Header), Header.Bytes, PF_R) != Header.Bytes) return Fault;
    std::array<uint32_t, 32> Words {};
    std::memcpy(Words.data(), reinterpret_cast<const void*>(Handle + sizeof(Header)), Header.Bytes);
    if (!SdMount || (Header.Type & 0xffff) != HandleInoGen || Header.Bytes < 12 || Words[0]) return -116; // ESTALE
    const auto Inode = Words[1] | static_cast<uint64_t>(Words[2]) << 32;
    if (IsUnlinked(Inode)) return -116;
    const auto Entry = std::find_if(SdInodes.begin(), SdInodes.end(), [&](const auto& Name) {
      return Name.second == Inode && std::string_view(Name.first).starts_with(RootPath) &&
             (Name.first.size() == RootPath.size() || Name.first[RootPath.size()] == '/') && !VirtualMount(Name.first);
    });
    if (Entry == SdInodes.end()) return -116;
    std::string Name = Entry->first.size() == RootPath.size() ? "/" : Entry->first.substr(RootPath.size());
    auto Flags = static_cast<uint32_t>(RawFlags) & LinuxValidOpenFlags;
    if (Flags & PathOnlyFlag) Flags &= PathOnlyFlag | 02000000 | 0200000 | 0400000;
    if (!ValidOpenFlags(Flags)) return Invalid;
    if ((Flags & 0300) == 0300) return -17; // The handle names an existing inode.
    // The handle names the inode itself, so a final symlink is never followed.
    return OpenNamed(CurrentDirectory, std::string_view(Name), Flags | 0400000, 0);
  }

  enum class FilesystemType : int64_t {
    Sd = 0x4d44, Proc = 0x9fa0, Tmpfs = 0x01021994, Devpts = 0x1cd1,
    Pipe = 0x50495045, Socket = 0x534f434b,
  };

  FilesystemType DescriptorFilesystem(const OpenFile& File) const {
    const auto VirtualDir = VirtualDirectory(File.Path);
    if (!VirtualDir.empty()) return VirtualDir == "/dev" ? FilesystemType::Tmpfs : FilesystemType::Proc;
    if (File.RealFile || File.Directory || File.Fifo) return FilesystemType::Sd;
    if (ProcFile(File.Path) || !ProcLink(File.Path).empty()) return FilesystemType::Proc;
    if (File.Memory || File.Path == "/dev/null" || File.Path == "/dev/tty") return FilesystemType::Tmpfs;
    if (File.PathOnly()) return FilesystemType::Sd;
    if (File.Pipe) return FilesystemType::Pipe;
    if (File.Socket) return FilesystemType::Socket;
    return FilesystemType::Devpts;
  }

  int64_t StatFilesystem(FilesystemType Type, uint64_t Output) {
    if (Type == FilesystemType::Sd) return StatSdFilesystem(Output);

    LinuxStatFS Info {};
    Info.Type = static_cast<int64_t>(Type);
    Info.BlockSize = Info.FragmentSize = GuestPageSize;
    Info.NameLength = 255;
    Info.Flags = 0x20; // ST_VALID
    if (Type == FilesystemType::Proc) Info.Flags |= 0xe; // NOSUID | NODEV | NOEXEC
    return CopyOut(Output, Info);
  }

  int64_t StatFilesystemPath(uint64_t Address, uint64_t Output) {
    const auto Name = GuestString(Address);
    if (!Name) return Name.Error;
    LinuxStat Info {};
    const auto Error = PathStat(CurrentDirectory, *Name, Info);
    if (Error) return Error;
    if (Info.Device == ProcDevice) return StatFilesystem(FilesystemType::Proc, Output);
    if (Info.Device == DevDevice) return StatFilesystem(FilesystemType::Tmpfs, Output);
    return StatSdFilesystem(Output);
  }

  int64_t StatFilesystemDescriptor(uint64_t FD, uint64_t Output) {
    const auto* File = GetFile(FD);
    if (!File) return BadFile;
    return StatFilesystem(DescriptorFilesystem(*File), Output);
  }

  int64_t SyncSdFilesystem() {
    // Dirty inodes outlive their last descriptor. Resolve ordinary files by
    // their current name and managed files by their stable backing object.
    for (const auto& [Path, Inode] : SdInodes) {
      if (!DirtyFileData.contains(Inode)) continue;
      OpenFile File {};
      File.Inode = Inode;
      File.Path = Path;
      if (const auto Error = FlushDiskFile(File); Error) return Error;
    }
    int64_t FlushError {};
    ForEachOpenFile([&](OpenFile& File) {
      if (!FlushError && File.RealFile && DirtyFileData.contains(File.Inode)) {
        FlushError = FlushDiskFile(File);
      }
    });
    if (FlushError) return FlushError;
    // Remaining entries have neither a name nor an open description.
    DirtyFileData.clear();
    if (const auto Error = SaveInodeStore(); Error < 0) return Error;
    FsFileSystem SdCard {};
    if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
    auto Error = SdFileError(fsFsCommit(&SdCard));
    if (!Error && InodeStoreAvailable && InodeGeneration && std::any_of(Inodes.begin(), Inodes.end(), [](const auto& Entry) {
          return Entry.second.ManagedFile && Entry.second.Unlinked;
        })) {
      std::vector<uint8_t> Bytes;
      const auto* OlderPath = InodeStorePaths[(InodeGeneration - 1) % 2];
      ReadInodeSlot(SdCard, OlderPath, Bytes);
      const auto Older = DecodeInodeSnapshot(Bytes);
      Error = ReclaimUnlinkedObjects(SdCard, Older ? &*Older : nullptr);
    }
    fsFsClose(&SdCard);
    return Error;
  }

  int64_t SyncFilesystemDescriptor(uint64_t FD) {
    const auto* File = GetFile(FD);
    if (!File || File->PathOnly()) return BadFile;
    // Synthetic filesystems have no persistent data to flush.
    return DescriptorFilesystem(*File) == FilesystemType::Sd ? SyncSdFilesystem() : 0;
  }

  int64_t SyncDescriptor(uint64_t FD) {
    auto* File = GetFile(FD);
    if (!File || File->PathOnly()) return BadFile;
    if (File->Memory) return 0;
    if (File->RealFile) {
      if (DirtyFileData.contains(File->Inode) || WriteAccess(File->StatusFlags)) {
        if (const auto Error = FlushDiskFile(*File); Error < 0) return Error;
      }
      return CommitSdInodeStore();
    }
    if (!File->Directory) return Invalid;
    if (!VirtualDirectory(File->Path).empty()) return Invalid;
    return SyncSdFilesystem();
  }

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
    File->Inode = NextAnonymousInode++;
    Descriptors().Files[FD] = std::move(File);
    Descriptors().Flags[FD] = (Flags & 02000000) != 0;
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
    Reader->Socket->Pipe = Writer->Socket->Pipe = true;
    Reader->StatusFlags = Flags & 04000;
    Writer->StatusFlags = Reader->StatusFlags | 1;
    Reader->Socket->Connect(Writer->Socket);
    Writer->Socket->Connect(Reader->Socket);
    const auto Inode = NextAnonymousInode;
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
    Socket.CopyInput(Buffer, Bytes);
    if (!Peek) {
      Socket.Consume(Bytes);
      ++IOEpoch;
    }
    return Bytes;
  }

  int64_t WriteSocket(StreamSocket& Socket, const char* Buffer, uint64_t Count, bool DontWait, bool Atomic = false, bool Merge = true) {
    if (!Socket.Connected) return -107;
    auto Peer = Socket.Peer.lock();
    if (!Peer || Socket.ShutWrite || Peer->ShutRead) return -32;
    const auto Bytes = std::min<uint64_t>(Count, Socket.WriteSpace(Merge, Count));
    if (!Bytes || (Atomic && Count <= StreamSocket::PipePage && Bytes < Count)) return Socket.Nonblocking || DontWait ? -11 : WaitForIO();
    const auto Written = Socket.Send({reinterpret_cast<const uint8_t*>(Buffer), Count}, {}, Merge);
    ++IOEpoch;
    return Written;
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

  struct LinuxMessage {
    uint64_t Name;
    uint32_t NameLength, NamePadding;
    uint64_t Vectors, VectorCount, Control, ControlLength;
    uint32_t Flags, FlagsPadding;
  };

  struct LinuxControl {
    uint64_t Length;
    int32_t Level, Type;
  };

  int64_t ReadMessageRights(const LinuxMessage& Message, StreamSocket::RightsMessage& Rights) {
    if (Message.ControlLength > 4096) return Invalid;
    if (Message.ControlLength > Available(Message.Control, PF_R)) return Fault;
    for (size_t Offset = 0; Message.ControlLength - Offset >= sizeof(LinuxControl);) {
      const auto* Data = reinterpret_cast<const char*>(Message.Control + Offset);
      LinuxControl Header {};
      std::memcpy(&Header, Data, sizeof(Header));
      if (Header.Length < sizeof(Header) || Header.Length > Message.ControlLength - Offset ||
          Header.Level != 1 || Header.Type != 1) return Invalid;
      const auto Count = (Header.Length - sizeof(Header)) / sizeof(int32_t);
      if (Count > 253 - Rights.Files.size()) return Invalid;
      for (size_t Index = 0; Index < Count; ++Index) {
        int32_t FD {};
        std::memcpy(&FD, Data + sizeof(Header) + Index * sizeof(FD), sizeof(FD));
        if (!GetFile(FD)) return BadFile;
        Rights.Files.push_back(Descriptors().Files[FD]);
      }
      const auto Next = Offset + ((Header.Length + 7) & ~7ULL);
      if (Next >= Message.ControlLength) break;
      Offset = Next;
    }
    return 0;
  }

  void WriteMessageRights(LinuxMessage& Message, const StreamSocket::RightsMessage& Rights, bool CloseOnExec) {
    uint64_t Written = 0;
    const auto Space = Message.ControlLength >= sizeof(LinuxControl) ? Message.ControlLength - sizeof(LinuxControl) : 0;
    for (const auto& File : Rights.Files) {
      const auto FD = FreeDescriptor();
      if (Written == Space / sizeof(int32_t) || FD == NoDescriptor) {
        Message.Flags |= 8; // MSG_CTRUNC
        break;
      }
      Descriptors().Files[FD] = File;
      Descriptors().Flags[FD] = CloseOnExec;
      CopyOut(Message.Control + sizeof(LinuxControl) + Written * sizeof(int32_t), static_cast<int32_t>(FD));
      ++Written;
    }
    Message.ControlLength = 0;
    if (Written) {
      const LinuxControl Header {sizeof(LinuxControl) + Written * sizeof(int32_t), 1, 1};
      CopyOut(Message.Control, Header);
      Message.ControlLength = std::min(Space + sizeof(LinuxControl), (Header.Length + 7) & ~uint64_t {7});
    }
  }

  int64_t SocketMessage(bool Send, uint64_t FD, uint64_t Address, uint64_t Flags) {
    constexpr uint64_t DontWait = 0x40, NoSignal = 0x4000, Peek = 2, CmsgCloexec = 0x40000000;
    if (Flags & ~(Send ? DontWait | NoSignal : DontWait | Peek | CmsgCloexec)) return -95;
    auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (!File->Socket || File->Pipe) return -88;
    auto& Socket = *File->Socket;
    if (!Socket.Connected) return -107;

    LinuxMessage Message {};
    if (Available(Address, Send ? PF_R : PF_R | PF_W) < sizeof(Message)) return Fault;
    std::memcpy(&Message, reinterpret_cast<const void*>(Address), sizeof(Message));
    if (Send && Message.Name) return -106;
    struct Vector { uint64_t Base, Length; };
    if (Message.VectorCount > 1024) return Invalid;
    if (Message.VectorCount && Available(Message.Vectors, PF_R) < Message.VectorCount * sizeof(Vector)) return Fault;
    std::vector<Vector> Vectors(Message.VectorCount);
    if (!Vectors.empty()) std::memcpy(Vectors.data(), reinterpret_cast<const void*>(Message.Vectors), Vectors.size() * sizeof(Vector));
    size_t Total = 0;
    for (auto& Vector : Vectors) {
      Vector.Length = std::min<uint64_t>(Vector.Length, StreamSocket::Capacity - Total);
      if (Vector.Length && Available(Vector.Base, Send ? PF_R : PF_W) < Vector.Length) return Fault;
      Total += Vector.Length;
    }

    if (Send) {
      if (!Total) return 0;
      auto Peer = Socket.Peer.lock();
      if (!Peer || Socket.ShutWrite || Peer->ShutRead) return -32;
      const auto Count = std::min(Total, Socket.WriteSpace());
      if (!Count) return Socket.Nonblocking || (Flags & DontWait) ? -11 : WaitForIO();
      StreamSocket::RightsMessage Rights {};
      if (Message.ControlLength) {
        if (const auto Status = ReadMessageRights(Message, Rights); Status < 0) return Status;
      }
      if (!Rights.Files.empty()) {
        if (std::any_of(Rights.Files.begin(), Rights.Files.end(), [](const auto& File) { return File->Socket != nullptr; })) {
          std::erase_if(SocketRightsReceivers, [](const auto& Entry) { return Entry.expired(); });
          if (std::none_of(SocketRightsReceivers.begin(), SocketRightsReceivers.end(),
                           [&](const auto& Entry) { return Entry.lock() == Peer; })) {
            SocketRightsReceivers.push_back(Peer);
          }
        }
      }
      auto Remaining = Count;
      for (const auto& Vector : Vectors) {
        const auto Length = std::min<uint64_t>(Remaining, Vector.Length);
        if (!Length) continue;
        const auto* Data = reinterpret_cast<const uint8_t*>(Vector.Base);
        Remaining -= Socket.Send({Data, Length}, std::move(Rights.Files));
      }
      ++IOEpoch;
      return Count;
    }

    if (Message.ControlLength && Available(Message.Control, PF_W) < Message.ControlLength) return Fault;
    if (Total && Socket.Input.empty() && !Socket.EndOfStream()) {
      return Socket.Nonblocking || (Flags & DontWait) ? -11 : WaitForIO();
    }
    // One ancillary message per receive. Keep the following message's
    // first byte queued so its rights remain associated with the stream.
    const auto Count = Socket.MessageBytes(Total);
    auto Input = Socket.Input.begin();
    auto Remaining = Count;
    for (const auto& Vector : Vectors) {
      const auto Length = std::min<uint64_t>(Remaining, Vector.Length);
      if (!Length) continue;
      std::copy_n(Input, Length, reinterpret_cast<uint8_t*>(Vector.Base));
      std::advance(Input, Length);
      Remaining -= Length;
    }
    Message.NameLength = 0;
    Message.Flags = 0;
    if (const auto* Rights = Socket.MessageRights(Count)) {
      WriteMessageRights(Message, *Rights, Flags & CmsgCloexec);
    } else {
      Message.ControlLength = 0;
    }
    if (!(Flags & Peek) && Count) {
      Socket.Consume(Count);
      ++IOEpoch;
    }
    CopyOut(Address, Message);
    return Count;
  }

  int64_t ShutdownSocket(uint64_t FD, uint64_t How) {
    auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (!File->Socket || File->Pipe) return -88;
    if (!File->Socket->Connected) return -107;
    if (How > 2) return Invalid;
    if (How != 1) {
      File->Socket->ShutRead = true;
      File->Socket->Consume(File->Socket->Input.size());
    }
    if (How != 0) File->Socket->ShutWrite = true;
    ++IOEpoch;
    return 0;
  }

  int64_t PollGuestWithTimespec(uint64_t Address, uint64_t Count, uint64_t Time, uint64_t Mask, uint64_t MaskSize) {
    if (Mask && MaskSize != sizeof(Current->SignalMask)) return Invalid;
    if (Mask && Available(Mask, PF_R) < MaskSize) return Fault;
    if (Mask) return -38; // Temporary masks require guest signal delivery.
    uint64_t Duration {};
    if (Time && Available(Time, PF_R) < 2 * sizeof(int64_t)) return Fault;
    if (Time && !ReadTimespec(Time, Duration)) return Invalid;
    return PollGuest(Address, Count, Time ? std::optional {Duration} : std::nullopt);
  }

  int64_t PollGuest(uint64_t Address, uint64_t Count, std::optional<uint64_t> Duration) {
    struct PollFD { int32_t FD; int16_t Events, Revents; };
    if (Count > Descriptors().Files.size()) return Invalid;
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
    if (ReadyCount || Duration == 0) return ReadyCount;
    if (Duration) {
      const auto Now = MonotonicNs();
      if (!Current->Deadline) Current->Deadline = AddDeadline(Now, *Duration);
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

  static constexpr uint32_t PipeSignal = 13;
  static constexpr uint64_t PipeSignalMask = uint64_t {1} << (PipeSignal - 1);

  void TerminateForSignal(uint32_t Signal) {
    Task->TerminatingSignal = Signal;
    Task->Exited = true;
    ExitThread();
    if (Task->PID == ProcessID) {
      Number = LinuxExitSyscall;
      ExitStatus = 128 + Signal;
      ProcessExited = true;
    }
  }

  bool DeliverDefaultPipeSignal() {
    const auto Disposition = Task->SignalActions[PipeSignal - 1][0];
    if (Disposition == 1) Current->PendingSignals &= ~PipeSignalMask;
    if (Disposition || !(Current->PendingSignals & ~Current->SignalMask & PipeSignalMask)) return false;
    Current->PendingSignals &= ~PipeSignalMask;
    TerminateForSignal(PipeSignal);
    return true;
  }

  struct GuestSignalFrame {
    uint64_t Restorer;
    struct {
      uint64_t Flags, Link;
      struct { uint64_t Pointer, Flags, Size; } Stack;
      struct { uint64_t Registers[23], FP, Reserved[8]; } Machine;
      uint64_t Mask[16];
    } Context;
    std::array<int32_t, 32> Info;
    uint64_t FPPadding;
    struct {
      uint16_t Control, Status, Tag, Opcode;
      uint64_t IP, DP;
      uint32_t MXCSR, MXCSRMask;
      uint64_t ST[8][2];
      uint64_t XMM[16][2];
      uint8_t Reserved[96];
    } FP;
    static_assert(sizeof(FP) == 512);
  };

  static constexpr auto SignalRegisters = [] {
    using namespace FEXCore::X86State;
    return std::array {REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
                       REG_RDI, REG_RSI, REG_RBP, REG_RBX, REG_RDX, REG_RAX, REG_RCX, REG_RSP};
  }();

  static uint32_t PendingCaughtSignal(const GuestThread& Thread, const GuestProcess& Process) {
    auto Pending = Thread.PendingSignals & ~Thread.SignalMask;
    while (Pending) {
      const auto Index = std::countr_zero(Pending);
      if (Process.SignalActions[Index][0] > 1) return Index + 1;
      Pending &= Pending - 1;
    }
    return 0;
  }

  static void QueueGuestSignal(GuestThread& Thread, uint32_t Signal, uint32_t Sender) {
    const uint64_t Mask = uint64_t {1} << (Signal - 1);
    if (!(Thread.PendingSignals & Mask)) Thread.SignalSenders[Signal - 1] = Sender;
    Thread.PendingSignals |= Mask;
  }

  int64_t SignalProcess(int32_t PID, uint32_t Signal) {
    if (Signal > 64) return Invalid;
    if (PID <= 0) return -95;
    const auto Target = Processes.find(PID);
    if (Target == Processes.end() || Target->second.Exited) return -3;
    if (!Signal) return 0;
    const auto Action = Target->second.SignalActions[Signal - 1][0];
    if (Action == 1) return 0;
    if (!Action) return -95; // Default process-directed dispositions are not implemented here.
    const uint64_t Mask = uint64_t {1} << (Signal - 1);
    GuestThread* Receiver {};
    for (auto& Thread : Threads) {
      if (!Thread.Core || Thread.Exited || Thread.PID != static_cast<uint32_t>(PID)) continue;
      Receiver = &Thread;
      if (!(Thread.SignalMask & Mask)) break;
    }
    if (!Receiver) return -3;
    QueueGuestSignal(*Receiver, Signal, Task->PID);
    return 0;
  }

  static constexpr auto X87SignalFlags = [] {
    using namespace FEXCore::X86State;
    return std::array {std::pair {X87FLAG_IE_LOC, 0}, std::pair {X87FLAG_C0_LOC, 8},
                       std::pair {X87FLAG_C1_LOC, 9}, std::pair {X87FLAG_C2_LOC, 10},
                       std::pair {X87FLAG_C3_LOC, 14}};
  }();

  bool EnterSignalHandler(CoreFrame* Frame, uint32_t Signal) {
    using namespace FEXCore::X86State;
    auto& State = Frame->State;
    const auto Action = Task->SignalActions[Signal - 1];
    const uint64_t Mask = uint64_t {1} << (Signal - 1);
    const auto SP = State.gregs[REG_RSP];
    const auto& Alt = Current->AltStack;
    const bool OnAlt = Alt[1] == 0 && SP >= Alt[0] && SP - Alt[0] < Alt[2];
    const bool UseAlt = (Action[1] & 0x08000000) && Alt[1] == 0 && !OnAlt;
    const auto Top = UseAlt ? Alt[0] + Alt[2] : SP;
    if (Top < sizeof(GuestSignalFrame) + 152 || !Action[2] ||
        !(Task->Memory.Query(Action[0]).Flags & PF_X)) return false;
    const auto Address = ((Top - 128 - sizeof(GuestSignalFrame)) & ~uint64_t {15}) - 8;
    GuestSignalFrame Guest {};
    Guest.Restorer = Action[2];
    Guest.Info[0] = Signal;
    Guest.Info[4] = Current->SignalSenders[Signal - 1];
    Guest.Info[5] = 1000;
    Guest.Context.Stack.Pointer = Alt[0];
    Guest.Context.Stack.Flags = OnAlt ? 1 : Alt[1];
    Guest.Context.Stack.Size = Alt[2];
    Guest.Context.Mask[0] = Current->SignalMask;
    auto& Context = Guest.Context.Machine;
    for (size_t I = 0; I < SignalRegisters.size(); ++I) Context.Registers[I] = State.gregs[SignalRegisters[I]];
    Context.Registers[16] = State.rip;
    Context.Registers[17] = Current->Core->CTX->ReconstructCompactedEFLAGS(Current->Core, false, nullptr, 0);
    Context.FP = Address + offsetof(GuestSignalFrame, FP);
    std::array<__uint128_t, 16> XMM {}, YMM {};
    Current->Core->CTX->ReconstructXMMRegisters(Current->Core, XMM.data(), YMM.data());
    std::memcpy(Guest.FP.XMM, XMM.data(), sizeof(XMM));
    Guest.FP.MXCSR = State.mxcsr;
    Guest.FP.MXCSRMask = 0xffc0;
    Guest.FP.Control = State.FCW;
    Guest.FP.Tag = State.AbridgedFTW;
    Guest.FP.IP = State.FIP;
    const auto TopRegister = State.flags[X87FLAG_TOP_LOC] & 7;
    Guest.FP.Status = TopRegister << 11;
    for (const auto [Flag, Bit] : X87SignalFlags) Guest.FP.Status |= (State.flags[Flag] & 1) << Bit;
    for (size_t I = 0; I < 8; ++I) std::memcpy(Guest.FP.ST[I], State.mm[(I + TopRegister) % 8], sizeof(State.mm[0]));
    if (CopyOut(Address, Guest)) return false;
    Current->SignalFrames.push_back({Address, YMM});
    Current->PendingSignals &= ~Mask;
    Current->SignalMask |= Action[3] | ((Action[1] & 0x40000000) ? 0 : Mask);
    Current->SignalMask &= ~((1ULL << 8) | (1ULL << 18));
    if (Action[1] & 0x80000000) Task->SignalActions[Signal - 1] = {};
    State.gregs[REG_RSP] = Address;
    State.gregs[REG_RDI] = Signal;
    State.gregs[REG_RSI] = Address + offsetof(GuestSignalFrame, Info);
    State.gregs[REG_RDX] = Address + offsetof(GuestSignalFrame, Context);
    State.gregs[REG_RAX] = 0;
    State.rip = Action[0];
    return true;
  }

  bool ReturnGuestSignal(CoreFrame* Frame) {
    using namespace FEXCore::X86State;
    if (Current->SignalFrames.empty()) return false;
    const auto& Saved = Current->SignalFrames.back();
    if (Frame->State.gregs[REG_RSP] != Saved.FrameAddress + 8 ||
        Available(Saved.FrameAddress, PF_R) < sizeof(GuestSignalFrame)) return false;
    GuestSignalFrame Guest {};
    std::memcpy(&Guest, reinterpret_cast<const void*>(Saved.FrameAddress), sizeof(Guest));
    auto& State = Frame->State;
    std::array<__uint128_t, 16> XMM {};
    std::memcpy(XMM.data(), Guest.FP.XMM, sizeof(XMM));
    Current->Core->CTX->SetXMMRegistersFromState(Current->Core, XMM.data(), Saved.YMM.data());
    State.mxcsr = Guest.FP.MXCSR & Guest.FP.MXCSRMask & 0xffc0;
    State.FCW = Guest.FP.Control;
    State.AbridgedFTW = Guest.FP.Tag;
    State.FIP = Guest.FP.IP;
    const auto TopRegister = (Guest.FP.Status >> 11) & 7;
    State.flags[X87FLAG_TOP_LOC] = TopRegister;
    for (const auto [Flag, Bit] : X87SignalFlags) State.flags[Flag] = (Guest.FP.Status >> Bit) & 1;
    for (size_t I = 0; I < 8; ++I) std::memcpy(State.mm[(I + TopRegister) % 8], Guest.FP.ST[I], sizeof(State.mm[0]));
    Current->SignalFrames.pop_back();
    const auto& Context = Guest.Context.Machine;
    for (size_t I = 0; I < SignalRegisters.size(); ++I) State.gregs[SignalRegisters[I]] = Context.Registers[I];
    State.rip = Context.Registers[16];
    Current->SignalMask = Guest.Context.Mask[0] & ~((1ULL << 8) | (1ULL << 18));
    Current->Core->CTX->SetFlagsFromCompactedEFLAGS(Current->Core, Context.Registers[17]);
    return true;
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
    if (Resource == 7) Limits[0] = Limits[1] = Descriptors().Files.size();
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

  enum class XattrOp { Set, Get, List, Remove };

  int64_t LegacyXattr(uint64_t Number, uint64_t Target, uint64_t Name, uint64_t Value, uint64_t Size, uint64_t Flags) {
    const auto Variant = (Number - 188) % 3;
    const auto Operation = static_cast<XattrOp>((Number - 188) / 3);
    const auto FD = Variant == 2 ? Target : CurrentDirectory;
    const auto Path = Variant == 2 ? 0 : Target;
    const uint64_t AtFlags = Variant == 2 ? 0x1000 : Variant == 1 ? 0x100 : 0;
    if (Operation == XattrOp::List) return Xattr(Operation, FD, Path, AtFlags, 0, Name, Value, 0);
    return Xattr(Operation, FD, Path, AtFlags, Name, Value, Size, Operation == XattrOp::Set ? Flags : 0);
  }

  int64_t XattrWithArgs(bool Setting, uint64_t FD, uint64_t Path, uint64_t AtFlags, uint64_t Name,
                        uint64_t Arguments, uint64_t Bytes) {
    struct XattrArguments { uint64_t Value; uint32_t Size, Flags; } Args {};
    if (Bytes < sizeof(Args)) return Invalid;
    if (Bytes > GuestPageSize) return -7;
    if (Available(Arguments, PF_R) < Bytes) return Fault;
    const auto* Data = reinterpret_cast<const uint8_t*>(Arguments);
    std::memcpy(&Args, Data, sizeof(Args));
    if (std::any_of(Data + sizeof(Args), Data + Bytes, [](uint8_t Byte) { return Byte != 0; })) return -7;
    return Xattr(Setting ? XattrOp::Set : XattrOp::Get, FD, Path, AtFlags, Name, Args.Value, Args.Size, Args.Flags);
  }

  int64_t Xattr(XattrOp Operation, uint32_t FD, uint64_t PathAddress, uint32_t AtFlags, uint64_t NameAddress,
                uint64_t Value, uint64_t Size, uint32_t Flags) {
    constexpr size_t MaxValue = 65536;
    constexpr uint64_t EmptyPath = 0x1000, NoFollow = 0x100;
    if (AtFlags & ~(EmptyPath | NoFollow)) return Invalid;
    if (Flags && (Operation != XattrOp::Set || (Flags & ~3U))) return Invalid;
    std::optional<std::string_view> Name;
    if (Operation != XattrOp::List) {
      constexpr size_t NameBytes = 256;
      const auto Count = std::min<size_t>(NameBytes, Available(NameAddress, PF_R));
      if (!Count) return Fault;
      const auto* Start = reinterpret_cast<const char*>(NameAddress);
      const auto* End = static_cast<const char*>(std::memchr(Start, 0, Count));
      if (!End) return Count == NameBytes ? -34 : Fault;
      if (End == Start) return -34;
      Name.emplace(Start, End - Start);
    }
    if (Operation == XattrOp::Set) {
      if (Size > MaxValue) return -7;
      if (Size && Available(Value, PF_R) < Size) return Fault;
    }
    LinuxStat Info {};
    int64_t Error;
    const bool Empty = !PathAddress ||
      (Available(PathAddress, PF_R) && *reinterpret_cast<const char*>(PathAddress) == 0);
    if (Empty && (AtFlags & EmptyPath)) {
      if (IsCwd(FD)) {
        Error = PathStat(FD, ".", Info);
      } else {
        const auto* File = GetFile(FD);
        if (!File || File->PathOnly()) return BadFile;
        Error = FileStat(FD, Info);
      }
    } else {
      Error = GetStatAt(FD, PathAddress, AtFlags, Info);
    }
    if (Error < 0) return Error;

    const auto Metadata = Inodes.find(Info.Inode);
    auto* Attributes = Metadata == Inodes.end() ? nullptr : &Metadata->second.Attributes;
    const bool Admin = Task->UserIDs[1] == 0;
    const auto Visible = [&](std::string_view Attribute) {
      return Admin || !Attribute.starts_with("trusted.");
    };
    if (Operation == XattrOp::List) {
      size_t Bytes = 0;
      if (Attributes) {
        for (const auto& [Name, Data] : *Attributes) {
          if (Visible(Name)) Bytes += Name.size() + 1;
        }
      }
      if (!Size || !Bytes) return Bytes;
      if (Size < Bytes) return -34;
      if (Bytes > MaxValue) return -7;
      if (Available(Value, PF_W) < Bytes) return Fault;
      auto* Output = reinterpret_cast<char*>(Value);
      for (const auto& [Name, Data] : *Attributes) {
        if (!Visible(Name)) continue;
        std::memcpy(Output, Name.c_str(), Name.size() + 1);
        Output += Name.size() + 1;
      }
      return Bytes;
    }

    const bool ACL = *Name == AccessAcl || *Name == DefaultAcl;
    const bool User = Name->starts_with("user.");
    const bool Trusted = Name->starts_with("trusted.");
    if (Trusted && !Admin) return Operation == XattrOp::Get ? -61 : -1;
    if (!ACL && !User && !Trusted) {
      if (Name->starts_with("security.")) return Operation == XattrOp::Get ? (*Name == "security." ? Invalid : -61) : -1;
      return -95;
    }
    if ((User && Name->size() == 5) || (Trusted && Name->size() == 8)) return Invalid;
    const auto Type = Info.Mode & 0170000;
    const bool Get = Operation == XattrOp::Get;
    const bool Owner = Info.Uid == Task->UserIDs[3] || Task->UserIDs[3] == 0;
    if (User && Type != 0100000 && Type != 0040000) return Get ? -61 : -1;
    if (ACL && Info.Device == ProcDevice) return -95;
    if (ACL && Type == 0120000) return -95;
    if (ACL && !Get && !Owner) return -1;
    if (User && !Get && Type == 0040000 && (Info.Mode & 01000) && !Owner) return -1;
    if (User && !InodeAllows(Info.Inode, Info.Mode, Get ? 4 : 2)) return -13;
    if (Info.Device == ProcDevice) return -95;
    if (*Name == DefaultAcl && Type != 0040000 && Get) return -61;

    const auto Existing = !Attributes ? XattrValues::iterator {} : Attributes->find(*Name);
    const bool Present = Attributes && Existing != Attributes->end();
    if (Operation == XattrOp::Set) {
      if (!ACL && (Flags & 1) && Present) return -17;
      if (!ACL && (Flags & 2) && !Present) return -61;
      const std::string_view Bytes {Size ? reinterpret_cast<const char*>(Value) : "", static_cast<size_t>(Size)};
      if (ACL) {
        if (!Bytes.empty()) {
          if (Bytes.size() < 4) return Invalid;
          if (Bytes.substr(0, 4) != std::string_view {"\x02\0\0\0", 4}) return -95;
        }
        if (Bytes.empty() || Bytes.size() == 4) {
          if (Present) Attributes->erase(Existing);
          MarkInodeChanged(Info.Inode);
          return 0;
        }
        const auto Layout = ParsePosixAcl(Bytes);
        if (!Layout) return Invalid;
        if (*Name == DefaultAcl && Type != 0040000) return -13;
        if (*Name == AccessAcl) {
          auto& Record = Inodes[Info.Inode];
          Record.Mode = (Info.Mode & 07000) | Layout->Mode(Bytes);
          if (Task->UserIDs[3] && !InGuestGroup(Info.Gid)) *Record.Mode &= ~02000;
          if (!Layout->Extended) {
            if (Present) Record.Attributes.erase(Existing);
            MarkInodeChanged(Info.Inode);
            return 0;
          }
        }
      }
      Inodes[Info.Inode].Attributes[std::string {*Name}] = Bytes;
      MarkInodeChanged(Info.Inode);
      return 0;
    }
    if (Operation == XattrOp::Remove) {
      if (!Present && !ACL) return -61;
      if (Present) Attributes->erase(Existing);
      MarkInodeChanged(Info.Inode);
      return 0;
    }
    if (!Present) return -61;
    const auto& Data = Existing->second;
    if (!Size || Data.empty()) return Data.size();
    if (Size < Data.size()) return -34;
    if (Available(Value, PF_W) < Data.size()) return Fault;
    std::memcpy(reinterpret_cast<void*>(Value), Data.data(), Data.size());
    return Data.size();
  }

  int64_t TerminalIoctl(uint64_t FD, uint64_t Command, uint64_t Argument) {
    const auto* File = GetFile(FD);
    if (!File || File->PathOnly()) return BadFile;
    if (Command == 0x5450 || Command == 0x5451) { // FIONCLEX / FIOCLEX
      Descriptors().Flags[FD] = Command == 0x5451;
      return 0;
    }
    if (Command == 0x5421) { // FIONBIO
      int32_t Enable {};
      if (Available(Argument, PF_R) < sizeof(Enable)) return Fault;
      std::memcpy(&Enable, reinterpret_cast<const void*>(Argument), sizeof(Enable));
      return FileControl(FD, 4, (File->StatusFlags & ~04000u) | (Enable ? 04000u : 0));
    }
    if (Command == 0x541b && File->Socket) { // FIONREAD
      if (File->Pipe && (File->StatusFlags & 3) == 1) {
        const auto Reader = File->Socket->Peer.lock();
        return CopyOut(Argument, static_cast<int32_t>(Reader ? Reader->Input.size() : 0));
      }
      return CopyOut(Argument, static_cast<int32_t>(File->Socket->Input.size()));
    }
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
    if (Count > Descriptors().Files.size()) return Invalid;
    using Mask = std::array<uint64_t, DescriptorCount / 64>;
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
    if (File.Fifo) {
      const auto Access = File.StatusFlags & 3;
      const auto& Fifo = *File.Fifo;
      int16_t Ready {};
      if (Access != 1) {
        if (!Fifo.Reader->Input.empty()) Ready |= Events & 1;
        if (!Fifo.Writers && Fifo.WriterGeneration != File.InitialWriterGeneration) Ready |= 0x10;
      }
      if (Access != 0) {
        if (!Fifo.Readers) Ready |= 8;
        else if (Fifo.Writer->WriteSpace(false)) Ready |= Events & 4;
      }
      return Ready;
    }
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
    // Files without a poll operation report both directions regardless of
    // the access mode. Terminal output is always writable.
    return (InputReady ? 1 : 0) | 4;
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
           ((Pressed & (HidNpadButton_B | HidNpadButton_Plus)) ? FEX_NX2_QUIT : 0) |
           ((Pressed & HidNpadButton_A) ? FEX_NX2_A : 0);
  }

  bool OpenVideo(unsigned Width, unsigned Height) {
    if (!GraphicsActive) {
      WriteLog("Opening the graphical display.");
      consoleExit(nullptr);
      Result Status = framebufferCreate(&Video, nwindowGetDefault(), Width, Height, PIXEL_FORMAT_RGBA_8888, 2);
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
        return false;
      }
      GraphicsActive = true;
      WriteLog("Graphical display initialized.");
    }

    return true;
  }


  int64_t PresentGuestFrame(uint64_t Address) {
    constexpr u32 Width = FEX_NX2_FRAME_WIDTH;
    constexpr u32 Height = FEX_NX2_FRAME_HEIGHT;
    constexpr u32 Scale = 4;
    if (Address % alignof(uint32_t) || Available(Address, PF_R) < Width * Height * sizeof(uint32_t)) {
      return Fault;
    }
    if (!OpenVideo(Width * Scale, Height * Scale)) return -5;

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
      WriteGuestOutput(OutputLog.c_str());
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

  int64_t TransferToPipe(StreamSocket& Sink, const char* Data, size_t Length, uint64_t Offset,
                         bool Nonblocking, const StreamSocket* SourcePipe = nullptr) {
    auto SourceBuffer = SourcePipe ? SourcePipe->PipeBuffers.cbegin() :
                                     std::deque<StreamSocket::PipeBuffer>::const_iterator {};
    size_t Written {};
    while (Written < Length) {
      size_t Boundary = StreamSocket::PipePage - ((Offset + Written) % StreamSocket::PipePage);
      std::shared_ptr<StreamSocket::PinnedPage> Pin;
      size_t Start {};
      if (SourcePipe) {
        Boundary = SourceBuffer->End - SourceBuffer->Start;
        Pin = SourceBuffer->Pin;
        Start = SourceBuffer->Start;
        ++SourceBuffer;
      }
      const auto Count = std::min(Length - Written, Boundary);
      const auto Result = Pin && Sink.WriteSpace(false) ?
        static_cast<int64_t>(Sink.SendPinned(std::move(Pin), Count, Start)) :
        WriteSocket(Sink, Data + Written, Count, Nonblocking || Written != 0, false, false);
      if (Result > 0) ++IOEpoch;
      if (Result <= 0) return Written ? static_cast<int64_t>(Written) : Result;
      Written += Result;
      if (static_cast<uint64_t>(Result) != Count) break;
    }
    return Written;
  }

  int64_t SpliceGuest(uint64_t InputFD, uint64_t InputOffset, uint64_t OutputFD, uint64_t OutputOffset,
                      uint64_t Length, uint64_t Flags, bool Duplicate = false) {
    if (!Length) return 0;
    if (Flags & ~0xfULL) return Invalid;
    auto* Input = GetFile(InputFD);
    auto* Output = GetFile(OutputFD);
    if (!Input || !Output || Input->PathOnly() || Output->PathOnly()) return BadFile;
    if (!ReadAccess(Input->StatusFlags) || !WriteAccess(Output->StatusFlags)) return BadFile;
    if (!Input->Pipe && !Output->Pipe) return Invalid;
    if (Duplicate && (!Input->Pipe || !Output->Pipe)) return Invalid;
    if (Input->Pipe && Output->Pipe && Input->Inode == Output->Inode) return Invalid;
    if ((InputOffset && Input->Socket) || (OutputOffset && Output->Socket)) return -29;
    if (Input->Directory || Output->Directory || (Output->StatusFlags & 02000)) return Invalid;
    if (!Input->RealFile && !Input->Memory && !Input->Socket) return Invalid;
    if (!Output->RealFile && !Output->Memory && !Output->Socket && Output->Path != "/dev/null") return Invalid;

    auto GetOffset = [&](uint64_t Address, const OpenFile& File, int64_t& Offset) {
      Offset = File.Offset;
      if (Address) {
        if (Available(Address, PF_R) < sizeof(Offset)) return Fault;
        std::memcpy(&Offset, reinterpret_cast<const void*>(Address), sizeof(Offset));
      }
      return Offset < 0 ? Invalid : int64_t {0};
    };
    int64_t In {}, Out {};
    if (const auto Error = GetOffset(InputOffset, *Input, In); Error) return Error;
    if (const auto Error = GetOffset(OutputOffset, *Output, Out); Error) return Error;
    const bool Nonblocking = Flags & 2;
    const auto Count = std::min<uint64_t>(Length, StreamSocket::Capacity);
    std::unique_ptr<char[]> Buffer {new (std::nothrow) char[Count]};
    if (!Buffer) return -12;

    // Peek at the input until the output has actually accepted bytes.
    const auto Read = Input->Socket ? ReadSocket(*Input->Socket, Buffer.get(), Count, Nonblocking || Input->Nonblocking(), true) :
                                      ReadFileAt(*Input, Buffer.get(), Count, In);
    if (Read < 0) return Read;
    if (!Read) {
      if ((InputOffset && CopyOut(InputOffset, In)) || (OutputOffset && CopyOut(OutputOffset, Out))) return Fault;
      return 0;
    }
    int64_t Written;
    if (Output->Pipe) {
      Written = TransferToPipe(Output->WriteEndpoint(), Buffer.get(), Read, In,
                               Nonblocking || Output->Nonblocking(), Input->Pipe ? Input->Socket.get() : nullptr);
    } else if (Output->Socket) {
      Written = WriteSocket(Output->WriteEndpoint(), Buffer.get(), Read, Nonblocking || Output->Nonblocking(), false, false);
    } else if (Output->Path == "/dev/null") {
      Written = Read;
    } else {
      Written = WriteFileAt(*Output, Buffer.get(), Read, Out);
    }
    if (Written <= 0) return Written;
    int64_t OffsetError {};
    if (!Duplicate) {
      if (Input->Socket) {
        Input->Socket->Consume(Written);
        ++IOEpoch;
      } else if (InputOffset) {
        In += Written;
        OffsetError = CopyOut(InputOffset, In);
      } else {
        Input->Offset += Written;
      }
    }
    if (Output->RealFile || Output->Memory) {
      if (OutputOffset) {
        Out += Written;
        OffsetError |= CopyOut(OutputOffset, Out);
      } else {
        Output->Offset += Written;
      }
    }
    return OffsetError ? Fault : Written;
  }

  int64_t VmspliceGuest(uint64_t FD, uint64_t Address, uint64_t Count, uint64_t Flags) {
    if (Flags & ~0xfULL) return Invalid;
    const auto* File = GetFile(FD);
    if (!File || !File->Pipe) return BadFile;
    if ((File->StatusFlags & 3) == 0) return VectorIO(FD, Address, Count, false, std::nullopt, 0, true, Flags & 2);
    struct IoVector { uint64_t Base, Size; };
    if (Count > 1024) return Invalid;
    if (AccessiblePrefix(Address, Count * sizeof(IoVector), PF_R) != Count * sizeof(IoVector)) return Fault;
    int64_t Written {};
    for (uint64_t Index = 0; Index < Count; ++Index) {
      IoVector Vector {};
      std::memcpy(&Vector, reinterpret_cast<const void*>(Address + Index * sizeof(Vector)), sizeof(Vector));
      while (Vector.Size) {
        const auto Boundary = StreamSocket::PipePage - Vector.Base % StreamSocket::PipePage;
        const auto Length = std::min<uint64_t>(Vector.Size, Boundary);
        const auto Bytes = AccessiblePrefix(Vector.Base, Length, PF_R);
        if (!Bytes) return Written ? Written : Fault;
        auto& Sink = File->WriteEndpoint();
        bool Shared {};
        auto Owner = Task->Memory.RetainPage(Vector.Base, Bytes, Shared);
        std::shared_ptr<StreamSocket::PinnedPage> Pin;
        if (Owner && Sink.WriteSpace(false)) {
          const auto* Live = Owner.get();
          Pin = std::make_shared<StreamSocket::PinnedPage>(StreamSocket::PinnedPage {
            Live, std::move(Owner), Bytes, false, Shared ? nullptr : std::make_unique<uint8_t[]>(Bytes)});
          if (!Shared) {
            std::erase_if(Task->PrivatePipePages, [](const auto& Weak) { return Weak.expired(); });
            Task->PrivatePipePages.emplace_back(Pin);
          }
        }
        const auto Result = Pin && Sink.WriteSpace(false) ?
          static_cast<int64_t>(Sink.SendPinned(std::move(Pin), Bytes)) :
          WriteSocket(Sink, reinterpret_cast<const char*>(Vector.Base), Bytes,
                      (Flags & 2) || File->Nonblocking() || Written, false, false);
        if (Result > 0) ++IOEpoch;
        if (Result <= 0) return Written ? Written : Result;
        Written += Result;
        if (static_cast<uint64_t>(Result) != Length) return Written;
        Vector.Base += Result;
        Vector.Size -= Result;
      }
    }
    return Written;
  }

  int64_t SendFile(uint64_t OutputFD, uint64_t InputFD, uint64_t OffsetAddress, uint64_t Length) {
    int64_t Offset {};
    if (OffsetAddress) {
      if (Available(OffsetAddress, PF_R) < sizeof(Offset)) return Fault;
      std::memcpy(&Offset, reinterpret_cast<const void*>(OffsetAddress), sizeof(Offset));
    }
    const auto Result = TransferSendFile(OutputFD, InputFD, OffsetAddress ? &Offset : nullptr, Length);
    if (Result != RetrySyscall && OffsetAddress && CopyOut(OffsetAddress, Offset)) return Fault;
    return Result;
  }

  int64_t TransferSendFile(uint64_t OutputFD, uint64_t InputFD, int64_t* Position, uint64_t Length) {
    auto* Input = GetFile(InputFD);
    if (!Input || Input->PathOnly() || !ReadAccess(Input->StatusFlags)) return BadFile;
    const auto Offset = Position ? *Position : Input->Offset;
    Length = std::min<uint64_t>(Length, 0x7ffff000);
    if (Offset < 0 || Length > static_cast<uint64_t>(INT64_MAX - Offset)) return Invalid;
    auto* Output = GetFile(OutputFD);
    if (!Output || Output->PathOnly() || !WriteAccess(Output->StatusFlags)) return BadFile;
    if ((!Input->RealFile && !Input->Memory) || (Output->StatusFlags & 02000)) return Invalid;
    if (!Output->RealFile && !Output->Memory && !Output->Socket &&
        Output->Console == ConsoleStream::None && Output->Path != "/dev/null") return Invalid;
    if (!Length) return 0;
    const auto Count = std::min<uint64_t>(Length, Output->RealFile || Output->Memory || Output->Pipe ? 65536 : 4096);
    std::unique_ptr<char[]> Buffer {new (std::nothrow) char[Count]};
    if (!Buffer) return -12;
    const auto Read = ReadFileAt(*Input, Buffer.get(), Count, Offset);
    if (Read <= 0) return Read;
    const auto Written = Output->Pipe ? TransferToPipe(Output->WriteEndpoint(), Buffer.get(), Read, Offset, Output->Nonblocking()) :
                                        WriteDescriptor(*Output, Buffer.get(), Read);
    if (Written <= 0) return Written;

    if (Position) *Position = Offset + Written;
    else if (Input != Output) Input->Offset = Offset + Written;
    return Written;
  }

  int64_t CopyFileRange(uint64_t InputFD, uint64_t InputOffset, uint64_t OutputFD, uint64_t OutputOffset,
                        uint64_t Length, uint64_t Flags) {
    auto* Input = GetFile(InputFD);
    auto* Output = GetFile(OutputFD);
    if (!Input || !Output || Input->PathOnly() || Output->PathOnly()) return BadFile;
    auto ReadOffset = [&](uint64_t Address, int64_t Current, int64_t& Offset) -> int64_t {
      if (!Address) {
        Offset = Current;
      } else {
        if (Available(Address, PF_R) < sizeof(Offset)) return Fault;
        std::memcpy(&Offset, reinterpret_cast<const void*>(Address), sizeof(Offset));
      }
      return 0;
    };
    int64_t StartIn {}, StartOut {};
    if (const auto Error = ReadOffset(InputOffset, Input->Offset, StartIn); Error) return Error;
    if (const auto Error = ReadOffset(OutputOffset, Output->Offset, StartOut); Error) return Error;
    if (Flags) return Invalid;
    if (!ReadAccess(Input->StatusFlags) || !WriteAccess(Output->StatusFlags) ||
        (Output->StatusFlags & 02000)) return BadFile;
    if (Input->Directory || Output->Directory) return -21;
    if ((!Input->Memory && !Input->RealFile) || (!Output->Memory && !Output->RealFile)) return Invalid;
    if (static_cast<bool>(Input->Memory) != static_cast<bool>(Output->Memory)) return -18;
    uint64_t End {};
    if (__builtin_add_overflow(static_cast<uint64_t>(StartIn), Length, &End) ||
        __builtin_add_overflow(static_cast<uint64_t>(StartOut), Length, &End)) return -75;
    if (StartIn < 0 || StartOut < 0) return Invalid;
    if (!Length) return 0;

    s64 Size {};
    if (Input->Memory) {
      Size = Input->Memory->Size;
    } else if (const auto Error = DiskSize(*Input, Size); Error) {
      return Error;
    }
    if (StartIn >= Size) return 0;
    const auto Remaining = std::min<uint64_t>(Length, Size - StartIn);
    if (Remaining > static_cast<uint64_t>(INT64_MAX - StartOut)) return -27;
    if (Input->Inode == Output->Inode) {
      const auto Distance = StartIn > StartOut ? StartIn - StartOut : StartOut - StartIn;
      if (static_cast<uint64_t>(Distance) < Remaining) return Invalid;
    }

    const auto Count = std::min<uint64_t>(Remaining, 65536);
    std::unique_ptr<char[]> Buffer {new (std::nothrow) char[Count]};
    if (!Buffer) return -12;
    const auto Bytes = ReadFileAt(*Input, Buffer.get(), Count, StartIn);
    if (Bytes <= 0) return Bytes;
    const auto Written = WriteFileAt(*Output, Buffer.get(), Bytes, StartOut);
    if (Written <= 0) return Written;
    StartIn += Written;
    StartOut += Written;
    int64_t InputError {}, OutputError {};
    if (InputOffset) InputError = CopyOut(InputOffset, StartIn);
    else Input->Offset = StartIn;
    if (OutputOffset) OutputError = CopyOut(OutputOffset, StartOut);
    else Output->Offset = StartOut;
    // Linux attempts both offset writebacks even if one faults after copying.
    return InputError || OutputError ? Fault : Written;
  }

  int64_t WriteFileAt(OpenFile& File, const void* Buffer, uint64_t Count, uint64_t Offset, AppendMode Append = AppendMode::Descriptor, bool Synchronous = false, int64_t* Position = nullptr) {
    if (Append == AppendMode::Force || (Append == AppendMode::Descriptor && (File.StatusFlags & 02000))) {
      if (File.Memory) {
        Offset = File.Memory->Size;
      } else {
        s64 Size {};
        if (const auto Error = DiskSize(File, Size); Error) return Error;
        Offset = Size;
      }
    }
    if (Offset > INT64_MAX || Count > INT64_MAX - Offset) return -27; // EFBIG
    if (File.Memory) {
      if (const auto Result = File.Memory->Write(Offset, Buffer, Count); Result < 0) return Result;
    } else if (const auto* Extents = FileExtents(File.Inode); Extents && Count) {
      const auto End = Offset + Count;
      const auto Visible = *LogicalSize(File.Inode);
      const auto Run = Extents->At(Offset, Count);
      if (End <= Visible && Run.Allocated && Run.Length == Count) {
        const auto WriteOption = Synchronous || (File.StatusFlags & DataSyncFlag) ? FsWriteOption_Flush : FsWriteOption_None;
        InvalidateReadCache(File.Inode);
        const auto Error = WithDiskFile(File, FsOpenMode_Write, [&](FsFile& Native) {
          return SdFileError(fsFileWrite(&Native, Run.BackingOffset, Buffer, Count, WriteOption));
        });
        if (Error) return Error;
      } else {
        const auto Error = ReplacePackedBacking(File, *Extents, AllocatePackedRange(*Extents, Offset, End), std::max(Visible, End),
          [&](uint64_t Position, std::span<char> Chunk) {
            const auto Begin = std::max(Position, Offset);
            const auto Limit = std::min(Position + Chunk.size(), End);
            if (Begin < Limit) {
              std::memcpy(Chunk.data() + Begin - Position, static_cast<const char*>(Buffer) + Begin - Offset, Limit - Begin);
            }
          });
        if (Error) return Error;
      }
    } else if (Count) {
      InvalidateReadCache(File.Inode);
      const auto WriteOption = Synchronous || (File.StatusFlags & DataSyncFlag) ? FsWriteOption_Flush : FsWriteOption_None;
      const auto Result = WithDiskFile(File, FsOpenMode_Write | FsOpenMode_Append, [&](FsFile& Native) {
        s64 Size {};
        if (const auto Error = SdFileError(fsFileGetSize(&Native, &Size)); Error) return Error;
        const auto Logical = LogicalSize(File.Inode);
        const auto Visible = Logical.value_or(Size);
        const auto Written = WriteGuestFileRange(Size, Visible, Offset, {static_cast<const char*>(Buffer), Count},
          [&](uint64_t Begin, uint64_t End) { return ZeroDiskRange(Native, Begin, End); },
          [&](uint64_t Position, std::span<const char> Chunk) {
            return SdFileError(fsFileWrite(&Native, Position, Chunk.data(), Chunk.size(), WriteOption));
          },
          [&](uint64_t Length) {
            if (R_FAILED(fsFileSetSize(&Native, Length))) {
              WriteLog("Guest write could not restore native allocation after an extending write failure.");
            }
          });
        if (Written < 0) return Written;
        Count = Written;
        if (Logical && Offset + Count > Visible) {
          Inodes.at(File.Inode).LogicalSize = Offset + Count;
          InodeStoreDirty = true;
        }
        return int64_t {0};
      });
      if (Result) return Result;
    }
    MarkFileDataChanged(File.Inode);
    if (File.RealFile && Count && (Synchronous || (File.StatusFlags & DataSyncFlag))) {
      if (const auto Error = CommitSdInodeStore(); Error < 0) return Error;
    }
    if (Position) *Position = Offset + Count;
    return Count;
  }

  static constexpr uint64_t MaxRWCount = 0x7ffff000, GuestUserLimit = 1ULL << 47;

  static bool IsGuestUserRange(uint64_t Address, uint64_t Count) {
    return Address < GuestUserLimit && Count <= GuestUserLimit - Address;
  }

  int64_t WriteGuest(uint64_t FD, uint64_t Address, uint64_t Count, std::optional<uint64_t> Offset = std::nullopt) {
    if (Offset && *Offset > INT64_MAX) return Invalid;
    auto* File = GetFile(FD);
    if (!File || File->PathOnly()) return BadFile;
    if (Offset && (File->Socket || File->Console != ConsoleStream::None)) return -29;
    if (!WriteAccess(File->StatusFlags)) return BadFile;
    if (!IsGuestUserRange(Address, Count)) return Fault;
    const auto Position = Offset.value_or(File->Offset);
    if (!File->Socket && Count > INT64_MAX - Position) return Invalid;
    if (File->Path == "/dev/null") return std::min(Count, MaxRWCount);
    if (!Count) return 0;
    const bool FileBacked = File->RealFile || File->Memory;
    if (!FileBacked && !File->Socket && File->Console == ConsoleStream::None) return BadFile;
    Count = std::min<uint64_t>(Count, FileBacked ? MaxRWCount : File->Pipe ? 65536 : 4096);
    const auto Accessible = AccessiblePrefix(Address, Count, PF_R);
    if (!Accessible || (Accessible != Count && !FileBacked)) return Fault;
    const auto* Buffer = reinterpret_cast<const char*>(Address);
    if (!Offset) return WriteDescriptor(*File, Buffer, Accessible);
    return WriteFileAt(*File, Buffer, Accessible, Position);
  }

  int64_t WriteDescriptor(OpenFile& Descriptor, const char* Buffer, uint64_t Count, AppendMode Append = AppendMode::Descriptor, bool Synchronous = false) {
    auto* File = &Descriptor;
    if (File->Path == "/dev/null") return Count;
    if (File->Socket) return WriteSocket(File->WriteEndpoint(), Buffer, Count, File->Nonblocking(), File->Pipe);
    if (File->Memory || File->RealFile) {
      int64_t End {};
      const auto Written = WriteFileAt(*File, Buffer, Count, File->Offset, Append, Synchronous, &End);
      if (Written > 0) File->Offset = End;
      return Written;
    }
    std::string Filtered;
    std::string_view Output {Buffer, static_cast<size_t>(Count)};
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

  static bool ValidOpenFlags(uint64_t Flags) {
    constexpr uint64_t DirectoryOnly = 0200000, TemporaryFile = 020000000, Create = 0100;
    constexpr uint64_t Allowed = 02000000 | DirectoryOnly | 0400000 | 04000 | 0400 |
      0100000 | Create | 0200 | 01000 | 02000 | PathOnlyFlag | TemporaryFile | SyncFlag | NoAtimeFlag | 3 |
      020000; // FASYNC is accepted and only takes effect through F_SETFL.
    return !(Flags & ~Allowed) && !((Flags & Create) && (Flags & DirectoryOnly)) &&
           (!(Flags & TemporaryFile) || ((Flags & DirectoryOnly) && (Flags & 3)));
  }

  int64_t OpenAt2(uint64_t Directory, uint64_t Address, uint64_t How, uint64_t Size) {
    struct OpenHow { uint64_t Flags, Mode, Resolve; } Args {};
    if (Size < sizeof(Args)) return Invalid;
    if (Size > GuestPageSize) return -7;
    if (AccessiblePrefix(How, Size, PF_R) != Size) return Fault;
    const auto* Data = reinterpret_cast<const uint8_t*>(How);
    std::memcpy(&Args, Data, sizeof(Args));
    if (std::any_of(Data + sizeof(Args), Data + Size, [](uint8_t Byte) { return Byte != 0; })) return -7;
    if ((Args.Resolve & ~0x3fULL) || (Args.Resolve & ResolveBeneath && Args.Resolve & ResolveInRoot)) return Invalid;
    if ((Args.Mode & ~07777ULL) || (Args.Mode && !(Args.Flags & (0100 | 020000000)))) return Invalid;
    if ((Args.Flags & PathOnlyFlag) && (Args.Flags & ~(PathOnlyFlag | 02000000 | 0200000 | 0400000))) return Invalid;
    if (!ValidOpenFlags(Args.Flags)) return Invalid;
    if (Args.Resolve & ResolveCached) {
      if (Args.Flags & (0100 | 01000 | 020000000)) return -11;
      const auto Name = GuestString(Address);
      if (!Name) return Name.Error;
      if (Name->empty()) return -2;
      const bool Absolute = Name->starts_with('/');
      if (Absolute && (Args.Resolve & ResolveBeneath)) return -18;
      if ((!Absolute || (Args.Resolve & ResolveInRoot)) && !IsCwd(Directory)) {
        const auto* File = GetFile(Directory);
        if (!File) return BadFile;
        if (!File->Directory) return -20;
      }
      return -11; // Native SD lookup requires a service call.
    }
    return OpenGuest(Directory, Address, Args.Flags, Args.Mode, Args.Resolve, true);
  }

  int64_t OpenTemporaryFile(uint64_t Directory, std::string_view Name, uint64_t Flags,
                            uint64_t CreationMode, uint64_t Resolve, uint64_t FD) {
    constexpr uint64_t CloseOnExec = 02000000, Exclusive = 0200, Create = 0100, NoFollow = 0400000, LargeFile = 0100000;
    constexpr uint64_t Preserved = 020000000 | 0200000 | NoFollow | 04000 | 02000 | SyncFlag | NoAtimeFlag | 3;
    if (!(Flags & 3) || (Flags & Create)) return Invalid;
    FsFileSystem SdCard {};
    if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
    std::string Path;
    auto Error = ResolveSdPath(Directory, Name, SdCard, Path, !(Flags & NoFollow), Resolve);
    FsDirEntryType Type {};
    if (!Error && VirtualMount(Path)) {
      const auto GuestPath = std::string_view(Path).substr(RootPath.size());
      const auto VirtualDir = VirtualDirectory(Path);
      if (VirtualDir == "/proc") Error = Task->UserIDs[3] ? -13 : -95;
      else if (VirtualDir.starts_with("/proc/")) Error = -1;
      else if (VirtualDir == "/dev") Error = -95;
      else if (!ProcLink(Path).empty() || ProcFile(GuestPath) || GuestPath == "/dev/null" ||
               (Interactive && GuestPath == "/dev/tty")) Error = -20;
    }
    if (!Error && IsVirtualEntry(Path)) Error = -20;
    if (!Error) Error = EntryType(SdCard, Path, Type);
    if (!Error && Type != FsDirEntryType_Dir) Error = -20;
    if (!Error) {
      const auto Inode = SdInode(Path);
      if (!InodeAllows(Inode, ModeForInode(Inode, 0040755), 3)) Error = -13;
    }
    if (!Error && !InodeStoreAvailable) Error = -5;
    if (!Error) {
      const auto Created = SdFileError(fsFsCreateDirectory(&SdCard, InodeObjects.data()));
      if (Created && Created != -17) Error = Created;
    }

    std::shared_ptr<OpenFile> File;
    if (!Error) {
      const auto Inode = NextSdInode++;
      auto& Record = Inodes[Inode];
      Record.ManagedFile = Record.Unlinked = true;
      const auto ParentInode = SdInode(Path);
      Record.Owner.emplace(Task->UserIDs[3], (ModeForInode(ParentInode, 0040755) & 02000) ?
                           InodeOwner(ParentInode).second : Task->GroupIDs[3]);
      InitializeInodeMode(Inode, Path, CreationMode, false);
      const auto Now = WallClock();
      Record.Times = InodeTimestamps {Now, Now, Now};
      InodeStoreDirty = true;
      // Record retirement first so a reset cannot strand an unnamed file.
      Error = SaveInodeStore();
      if (!Error) Error = SdFileError(fsFsCommit(&SdCard));
      if (!Error) {
        const auto Object = ObjectPath(Inode);
        Error = SdFileError(fsFsCreateFile(&SdCard, Object.c_str(), 0, 0));
        if (!Error) {
          File = std::make_shared<OpenFile>();
          const uint32_t Mode = FsOpenMode_Write | FsOpenMode_Append | ((Flags & 3) == 2 ? FsOpenMode_Read : 0);
          FsFile Native {};
          Error = SdFileError(fsFsOpenFile(&SdCard, Object.c_str(), Mode, &Native));
          if (!Error) {
            fsFileClose(&Native);
            File->RealFile = true;
            File->Inode = Inode;
            File->StatusFlags = (Flags & Preserved) | LargeFile;
            File->TemporaryLinkable = !(Flags & Exclusive);
          }
        }
      }
    }
    fsFsClose(&SdCard);
    if (Error) return Error;
    std::erase_if(DiskFiles, [](const auto& Entry) { return Entry.expired(); });
    DiskFiles.emplace_back(File);
    Descriptors().Files[FD] = std::move(File);
    Descriptors().Flags[FD] = (Flags & CloseOnExec) != 0;
    return FD;
  }

  // Legacy open and openat ignore bits outside VALID_OPEN_FLAGS; openat2 rejects them.
  static constexpr uint64_t LinuxValidOpenFlags = 3 | 0100 | 0200 | 0400 | 01000 | 02000 | 04000 | 04010000 |
    020000 | 040000 | 0100000 | 0200000 | 0400000 | 01000000 | 02000000 | 010000000 | 020000000;

  int64_t OpenGuest(uint64_t Directory, uint64_t Address, uint64_t Flags, uint64_t CreationMode, uint64_t Resolve = 0,
                    bool Strict = false) {
    if (!Strict) Flags &= LinuxValidOpenFlags;
    if (Flags & PathOnlyFlag) Flags &= PathOnlyFlag | 02000000 | 0200000 | 0400000;
    if (!ValidOpenFlags(Flags)) return Invalid;
    const auto Name = GuestString(Address);
    if (!Name) return Name.Error;
    return OpenNamed(Directory, *Name, Flags, CreationMode, Resolve);
  }

  // Opens a validated name. Flags have already been masked for O_PATH.
  int64_t OpenNamed(uint64_t Directory, std::optional<std::string_view> Name, uint64_t Flags, uint64_t CreationMode,
                    uint64_t Resolve = 0) {
    constexpr uint64_t CloseOnExec = 02000000, DirectoryOnly = 0200000, NoFollow = 0400000;
    constexpr uint64_t Nonblocking = 04000, LargeFile = 0100000;
    constexpr uint64_t Create = 0100, Exclusive = 0200, Truncate = 01000, Append = 02000;
    constexpr uint64_t TemporaryFile = 020000000;
    const bool PathOnly = (Flags & PathOnlyFlag) != 0;
    if (Name->empty()) return -2;
    if ((Resolve & ResolveBeneath) && Name->starts_with('/')) return -18;
    const auto FD = FreeDescriptor();
    if (FD == NoDescriptor) return -24;
    if (Flags & TemporaryFile) return OpenTemporaryFile(Directory, *Name, Flags, CreationMode, Resolve, FD);
    // A blocked FIFO open already resolved an inode. Retrying by name
    // could land on a different inode after another thread renames it.
    if (Current->PendingFifo) return OpenFifo(Current->PendingFifo, FD, Flags & CloseOnExec);
    auto File = std::make_shared<OpenFile>();
    File->StatusFlags = PathOnly ? Flags & (PathOnlyFlag | DirectoryOnly | NoFollow)
                                 : Flags & (Nonblocking | LargeFile | Append | SyncFlag | NoAtimeFlag | 3);
    {
      FsFileSystem SdCard {};
      if (const auto Status = fsOpenSdCardFileSystem(&SdCard); R_FAILED(Status)) return SdFileError(Status);
      if (const auto Error = ResolveSdPath(Directory, *Name, SdCard, File->Path, !(Flags & NoFollow) && !((Flags & Create) && (Flags & Exclusive)), Resolve); Error < 0) {
        fsFsClose(&SdCard);
        return Error;
      }
      if ((Flags & Create) && Name->ends_with('/')) {
        fsFsClose(&SdCard);
        return -21;
      }
      const auto& Path = File->Path;
      if (!ProcLink(Path).empty()) {
        fsFsClose(&SdCard);
        if (!PathOnly) return (Flags & Create) && (Flags & Exclusive) ? -17 : -40;
        if (Flags & DirectoryOnly) return -20;
        File->Inode = VirtualInode(std::string_view(Path).substr(RootPath.size()));
        Descriptors().Files[FD] = std::move(File);
        Descriptors().Flags[FD] = (Flags & CloseOnExec) != 0;
        return FD;
      }
      if (const auto VirtualDir = VirtualDirectory(Path); !VirtualDir.empty()) {
        fsFsClose(&SdCard);
        if ((Flags & (Create | Truncate)) || (Flags & 3)) return -21;
        File->Inode = VirtualInode(VirtualDir);
        if ((Flags & NoAtimeFlag) && !OwnsInodeOrRoot(File->Inode)) return -1;
        File->Directory.emplace();
        Descriptors().Files[FD] = std::move(File);
        Descriptors().Flags[FD] = (Flags & CloseOnExec) != 0;
        return FD;
      }
      if (VirtualMount(Path)) {
        const auto GuestPath = std::string_view(Path).substr(RootPath.size());
        const auto Contents = ProcFile(GuestPath);
        const bool Null = GuestPath == "/dev/null";
        const bool Tty = Interactive && GuestPath == "/dev/tty";
        if (Contents || Null || Tty) {
          fsFsClose(&SdCard);
          if ((Flags & DirectoryOnly) || Name->ends_with('/')) return -20;
          if ((Flags & Create) && (Flags & Exclusive)) return -17;
          if (!PathOnly && Contents && (Flags & 3)) return -13;
          File->Inode = VirtualInode(GuestPath);
          if ((Flags & NoAtimeFlag) && !OwnsInodeOrRoot(File->Inode)) return -1;
          File->Contents = Contents.value_or("");
          if (Tty) File->Console = ConsoleStream::Terminal;
          File->Path = GuestPath;
          Descriptors().Files[FD] = std::move(File);
          Descriptors().Flags[FD] = (Flags & CloseOnExec) != 0;
          return FD;
        }
      }
      if (SymlinkAt(Path)) {
        fsFsClose(&SdCard);
        if (!PathOnly) return (Flags & Create) && (Flags & Exclusive) ? -17 : -40;
        if (Flags & DirectoryOnly) return -20;
        File->Inode = SdInode(Path);
        Descriptors().Files[FD] = std::move(File);
        Descriptors().Flags[FD] = (Flags & CloseOnExec) != 0;
        return FD;
      }
      if (const auto Special = SpecialNodeAt(Path)) {
        fsFsClose(&SdCard);
        if (Flags & DirectoryOnly || Name->ends_with('/')) return -20;
        if ((Flags & Create) && (Flags & Exclusive)) return -17;
        File->Inode = SdInode(Path);
        if (PathOnly) {
          Descriptors().Files[FD] = std::move(File);
          Descriptors().Flags[FD] = (Flags & CloseOnExec) != 0;
          return FD;
        }
        if (Special != 0010000) return -6;
        const auto Access = Flags & 3;
        const uint32_t Required = (Access != 1 ? 4 : 0) | (Access != 0 ? 2 : 0);
        if (!InodeAllows(File->Inode, ModeForInode(File->Inode, Special), Required)) return -13;
        if ((Flags & NoAtimeFlag) && !OwnsInodeOrRoot(File->Inode)) return -1;
        return OpenFifo(std::move(File), FD, Flags & CloseOnExec);
      }
      FsDirEntryType Type {};
      auto Lookup = EntryType(SdCard, Path, Type);
      bool Created = false;
      if (Flags & Create) {
        if (Lookup == -2) {
          if (!CanModifyParent(Path)) {
            fsFsClose(&SdCard);
            return -13;
          }
          const auto Error = NeedsOverflow(Path) ? CreateOverflowFile(SdCard, Path, CreationMode) :
                                                   CreateNativeEntry(SdCard, Path, CreationMode, false);
          if (Error) {
            fsFsClose(&SdCard);
            return Error;
          }
          Lookup = 0;
          Type = FsDirEntryType_File;
          Created = true;
        } else if (!Lookup && (Flags & Exclusive)) {
          fsFsClose(&SdCard);
          return -17;
        }
      }
      // A created overflow file is managed, so look its object up afterwards.
      if (Lookup) {
        fsFsClose(&SdCard);
        return Lookup;
      }
      const auto Backing = FileBackingPath(Path);
      Result Status = 0;
      if (Type == FsDirEntryType_Dir && ((Flags & (Create | Truncate)) || (Flags & 3))) {
        fsFsClose(&SdCard);
        return -21;
      }
      if (!Created && !PathOnly) {
        const auto Inode = SdInode(Path);
        const auto Mode = ModeForInode(Inode, Type == FsDirEntryType_Dir ? 0040755 : RegularMode(Path));
        const auto Access = Flags & 3;
        const uint32_t Required = (Access != 1 ? 4 : 0) | (Access != 0 || (Flags & Truncate) ? 2 : 0);
        if (!InodeAllows(Inode, Mode, Required)) {
          fsFsClose(&SdCard);
          return -13;
        }
        if ((WriteAccess(Flags) || (Flags & Truncate)) && InodeExecuting(Inode)) {
          fsFsClose(&SdCard);
          return -26;
        }
        if ((Flags & NoAtimeFlag) && !OwnsInodeOrRoot(Inode)) {
          fsFsClose(&SdCard);
          return -1;
        }
      }
      if (R_SUCCEEDED(Status) && Type == FsDirEntryType_File && !(Flags & DirectoryOnly) && !Name->ends_with('/')) {
        uint32_t Mode = (Flags & 3) == 1 ? 0 : FsOpenMode_Read;
        if ((Flags & 3) || (Flags & Truncate)) Mode |= FsOpenMode_Write | FsOpenMode_Append;
        FsFile Native {};
        Status = fsFsOpenFile(&SdCard, Backing.c_str(), Mode, &Native);
        File->RealFile = R_SUCCEEDED(Status);
        if (File->RealFile) {
          int64_t Error {};
          if (Flags & Truncate) {
            File->Inode = SdInode(Path);
            File->Path = Path;
            Error = FileExtents(File->Inode) ? ResizeDiskFile(*File, 0) : ResizeDiskInode(Native, File->Inode, 0, true);
          }
          fsFileClose(&Native);
          if (Error) {
            fsFsClose(&SdCard);
            return Error;
          }
        }
      }
      if (R_SUCCEEDED(Status)) {
        const auto Inode = SdInode(Path);
        if (!IsOverflowDirectory(Path)) ImportTimestamps(SdCard, Backing, Inode);
        if (File->RealFile && (Flags & Truncate)) MarkFileDataChanged(Inode);
      }
      fsFsClose(&SdCard);
      if (Logging == LogLevel::Trace) {
        char Message[1024];
        std::snprintf(Message, sizeof(Message), "openat %s -> 0x%08x", Backing.c_str(), Status);
        WriteLog(Message);
      }
      if (R_FAILED(Status)) return SdFileError(Status);
      if (Type == FsDirEntryType_Dir) {
        File->Directory.emplace();
      } else if (!File->RealFile) {
        return -20;
      }
    }
    File->Inode = (File->RealFile || File->Directory) ? SdInode(File->Path) : VirtualInode(File->Path);
    if (File->RealFile) {
      std::erase_if(DiskFiles, [](const auto& Entry) { return Entry.expired(); });
      DiskFiles.emplace_back(File);
    }
    Descriptors().Files[FD] = std::move(File);
    Descriptors().Flags[FD] = (Flags & CloseOnExec) != 0;
    return FD;
  }

  int64_t VectorIO(uint64_t FD, uint64_t Address, uint64_t Count, bool Writing,
                   std::optional<uint64_t> Offset = std::nullopt, uint32_t Flags = 0,
                   bool PipeTransfer = false, bool Nonblocking = false) {
    constexpr uint32_t Hipri = 1, Sync = 0x6, Nowait = 8, Append = 0x10, NoAppend = 0x20;
    struct IoVector { uint64_t Base; uint64_t Size; };
    if (Offset && *Offset > INT64_MAX) return Invalid;
    auto* File = GetFile(FD);
    if (!File || File->PathOnly()) return BadFile;
    if (Offset && (File->Socket || File->Console != ConsoleStream::None)) return -29;
    if (!(Writing ? WriteAccess(File->StatusFlags) : ReadAccess(File->StatusFlags))) return BadFile;
    if (Count > 1024) return Invalid;
    if (!Count) return 0;
    const auto VectorBytes = Count * sizeof(IoVector);
    if (AccessiblePrefix(Address, VectorBytes, PF_R) != VectorBytes) return Fault;
    std::vector<IoVector> Vectors(Count);
    std::memcpy(Vectors.data(), reinterpret_cast<const void*>(Address), VectorBytes);
    size_t RequestedTotal = 0;
    for (const auto& Vector : Vectors) {
      if (Vector.Size > INT64_MAX) return Invalid;
      const auto Length = std::min<uint64_t>(Vector.Size, MaxRWCount);
      if (!IsGuestUserRange(Vector.Base, Length)) return Fault;
      RequestedTotal += std::min<size_t>(Length, MaxRWCount - RequestedTotal);
    }
    if (!RequestedTotal) return 0;
    if (!File->Socket && RequestedTotal > INT64_MAX - Offset.value_or(File->Offset)) return Invalid;
    const auto Supported = Hipri | Sync | Append | NoAppend | VectorNoSignal |
                           ((File->Socket || File->Path == "/dev/null") ? Nowait : 0);
    if (Flags & ~Supported) return -95;
    if ((Flags & Append) && (Flags & NoAppend)) return Invalid;
    const auto AppendBehavior = Flags & Append ? AppendMode::Force :
                                Flags & NoAppend ? AppendMode::Suppress : AppendMode::Descriptor;
    if (File->Directory) return Writing ? BadFile : -21;
    if (File->Path == "/dev/null") return Writing ? RequestedTotal : 0;
    const size_t Limit = File->Console != ConsoleStream::None ? (Writing ? 4096 : 65536) :
                         (File->Socket ? File->Socket->InputCapacity : MaxRWCount);
    size_t Total = 0;
    bool BufferFault = false;
    // One backend operation preserves pipe write atomicity and avoids
    // blocking again after a short read from a pipe or terminal.
    size_t Used = 0;
    for (auto& Vector : Vectors) {
      if (Total == Limit) break;
      const auto Requested = std::min<uint64_t>(Vector.Size, Limit - Total);
      if (!Requested) continue;
      Vector.Size = AccessiblePrefix(Vector.Base, Requested, Writing ? PF_R : PF_W);
      Total += Vector.Size;
      Vectors[Used++] = Vector;
      if (Vector.Size < Requested) {
        BufferFault = true;
        break;
      }
    }
    Vectors.resize(Used);
    if (!Total) return Writing ? Fault : ReadBufferFault(*File, Offset.value_or(File->Offset));
    std::unique_ptr<char[]> Buffer {new (std::nothrow) char[Total]};
    if (!Buffer) return -12;
    if (Writing) {
      if (!File->RealFile && !File->Memory && !File->Socket &&
          File->Console == ConsoleStream::None) return BadFile;
      if (BufferFault && File->Pipe) return Fault;
      size_t Copied = 0;
      for (const auto& Vector : Vectors) {
        if (!Vector.Size) break;
        std::memcpy(Buffer.get() + Copied, reinterpret_cast<const void*>(Vector.Base), Vector.Size);
        Copied += Vector.Size;
      }
      int64_t Written;
      if (File->Socket) {
        Written = WriteSocket(File->WriteEndpoint(), Buffer.get(), Total, (Flags & Nowait) || File->Nonblocking(), File->Pipe);
      } else if (!Offset) {
        Written = WriteDescriptor(*File, Buffer.get(), Total, AppendBehavior, Flags & Sync);
      } else {
        Written = WriteFileAt(*File, Buffer.get(), Total, *Offset, AppendBehavior, Flags & Sync);
      }
      return Written;
    }
    const auto Read = PipeTransfer || (File->Socket && (Flags & Nowait)) ?
                        ReadSocket(*File->Socket, Buffer.get(), Total, Nonblocking || (Flags & Nowait) || File->Nonblocking()) :
                      Offset ? ReadFileAt(*File, Buffer.get(), Total, *Offset) :
                               ReadDescriptor(*File, Buffer.get(), Total);
    if (Read <= 0) return Read;
    size_t Copied = 0;
    for (const auto& Vector : Vectors) {
      const auto Bytes = std::min<uint64_t>(Vector.Size, Read - Copied);
      if (!Bytes) break;
      std::memcpy(reinterpret_cast<void*>(Vector.Base), Buffer.get() + Copied, Bytes);
      Copied += Bytes;
    }
    return Read;
  }

  int64_t ReadBufferFault(const OpenFile& File, uint64_t Offset) {
    if (!File.RealFile && !File.Memory) return Fault;
    s64 Size {};
    if (File.Memory) Size = File.Memory->Size;
    else if (const auto Error = DiskSize(File, Size); Error) return Error;
    return Offset >= static_cast<uint64_t>(Size) ? 0 : Fault;
  }

  int64_t ReadGuest(uint64_t FD, uint64_t Address, uint64_t Count, std::optional<uint64_t> Offset = std::nullopt) {
    if (Offset && *Offset > INT64_MAX) return Invalid;
    auto* File = GetFile(FD);
    if (!File || File->PathOnly()) return BadFile;
    if (Offset && (File->Socket || File->Console != ConsoleStream::None)) return -29;
    if (!ReadAccess(File->StatusFlags)) return BadFile;
    if (!IsGuestUserRange(Address, Count)) return Fault;
    const auto Position = Offset.value_or(File->Offset);
    if (!File->Socket && Count > INT64_MAX - Position) return Invalid;
    if (File->Path == "/dev/null") return 0;
    if (File->Directory) return -21;
    if (!Count) return 0;
    Count = std::min<uint64_t>(Count, File->RealFile || File->Memory ? MaxRWCount : 65536);
    const auto Accessible = AccessiblePrefix(Address, Count, PF_W);
    if (!Accessible) return ReadBufferFault(*File, Position);
    if (Accessible != Count && !File->RealFile && !File->Memory) return Fault;
    auto* Buffer = reinterpret_cast<char*>(Address);
    return Offset ? ReadFileAt(*File, Buffer, Accessible, Position) : ReadDescriptor(*File, Buffer, Accessible);
  }

  int64_t ReadDescriptor(OpenFile& Descriptor, char* Buffer, uint64_t Count) {
    auto* File = &Descriptor;
    if (Interactive && (File->Console == ConsoleStream::Input || File->Console == ConsoleStream::Terminal)) {
      if (!ReadAccess(File->StatusFlags)) return BadFile;
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
    if (File->Socket) return ReadSocket(*File->Socket, Buffer, Count, File->Nonblocking());
    if (File->Directory) return -21;
    const auto Read = ReadFileAt(*File, Buffer, Count, File->Offset);
    if (Read > 0) File->Offset += Read;
    return Read;
  }

  int64_t ReadDiskData(OpenFile& File, void* Buffer, uint64_t Count, uint64_t Offset, u64& Read) {
    const auto Logical = LogicalSize(File.Inode);
    return WithDiskFile(File, FsOpenMode_Read, [&](FsFile& Native) {
      if (const auto* Extents = FileExtents(File.Inode)) {
        const auto Result = Extents->ReadBytes(Offset, {static_cast<char*>(Buffer), Count},
          [&](uint64_t Position, std::span<char> Chunk) -> int64_t {
            u64 Received {};
            const auto Error = SdFileError(fsFileRead(&Native, Position, Chunk.data(), Chunk.size(), FsReadOption_None, &Received));
            return Error ? Error : static_cast<int64_t>(Received);
          });
        if (Result < 0) return Result;
        Read = Result;
        return int64_t {0};
      }
      uint64_t PhysicalCount = Count;
      if (Logical) {
        s64 Allocated {};
        if (const auto Error = SdFileError(fsFileGetSize(&Native, &Allocated)); Error) return Error;
        PhysicalCount = Offset >= static_cast<uint64_t>(Allocated) ? 0 :
          std::min<uint64_t>(Count, static_cast<uint64_t>(Allocated) - Offset);
      }
      auto* Data = static_cast<char*>(Buffer);
      while (Read < PhysicalCount) {
        const auto Chunk = std::min<uint64_t>(PhysicalCount - Read, 65536);
        u64 Received {};
        const auto Error = SdFileError(fsFileRead(&Native, Offset + Read, Data + Read, Chunk, FsReadOption_None, &Received));
        if (Error && !Read && !Logical) {
          // Horizon rejects reads that start past EOF; Linux reads 0 bytes.
          s64 Size {};
          if (R_SUCCEEDED(fsFileGetSize(&Native, &Size)) && Offset >= static_cast<uint64_t>(Size)) return int64_t {0};
        }
        if (Error) return Read ? int64_t {0} : Error;
        Read += Received;
        if (Received < Chunk) break;
      }
      if (Logical && Read == PhysicalCount && PhysicalCount < Count) {
        std::memset(Data + PhysicalCount, 0, Count - PhysicalCount);
        Read = Count;
      }
      return int64_t {0};
    });
  }

  int64_t ReadFileAt(OpenFile& File, void* Buffer, size_t Count, uint64_t Offset) {
    if (Offset > INT64_MAX) return Invalid;
    if (File.Socket || File.Directory || File.Console != ConsoleStream::None) return -29;
    if (File.Memory) {
      if (Offset >= File.Memory->Size) return 0;
      const auto Read = std::min<size_t>(Count, File.Memory->Size - Offset);
      std::memmove(Buffer, static_cast<const char*>(File.Memory->Storage.get()) + Offset, Read);
      if (Read && !(File.StatusFlags & NoAtimeFlag)) MarkInodeAccessed(File.Inode);
      return Read;
    }
    if (!File.RealFile) {
      if (Offset >= File.Contents.size()) return 0;
      const auto Data = File.Contents.substr(Offset);
      const size_t Read = std::min(Data.size(), Count);
      std::memcpy(Buffer, Data.data(), Read);
      return Read;
    }
    Count = BoundDiskRead(File.Inode, Offset, Count);
    if (!Count) return 0;
    const auto Cached = [&] {
      if (ReadCache.Inode != File.Inode || Offset < ReadCache.Offset) return false;
      const auto Start = Offset - ReadCache.Offset;
      return Start < ReadCache.Size && Count <= ReadCache.Size - Start;
    };
    if (File.ReadAdvice == 2 && Count <= ReadCache.Buffer.size() && !Cached()) {
      PrefetchFile(File, Offset, std::min<uint64_t>(ReadCache.Buffer.size(), INT64_MAX - Offset));
    }
    if (Cached()) {
      std::memcpy(Buffer, ReadCache.Buffer.data() + (Offset - ReadCache.Offset), Count);
      if (File.ReadAdvice == 5) ReadCache.Size = 0;
      if (Count && !(File.StatusFlags & NoAtimeFlag)) MarkInodeAccessed(File.Inode);
      return Count;
    }
    u64 Read {};
    const auto Error = ReadDiskData(File, Buffer, Count, Offset, Read);
    if (Error < 0) return Error;
    if (Read && !(File.StatusFlags & NoAtimeFlag)) MarkInodeAccessed(File.Inode);
    return Read;
  }

  int64_t CloseRange(uint32_t First, uint32_t Last, uint32_t Flags) {
    constexpr uint32_t Unshare = 2, CloseOnExec = 4;
    if (First > Last || (Flags & ~(Unshare | CloseOnExec))) return Invalid;
    if ((Flags & Unshare) && !Current->Descriptors.unique()) {
      Current->Descriptors = std::make_shared<DescriptorState>(*Current->Descriptors);
    }
    auto& Table = Descriptors();
    for (size_t FD = First; FD < Table.Files.size() && FD <= Last; ++FD) {
      if (!Table.Files[FD]) continue;
      if (Flags & CloseOnExec) Table.Flags[FD] = 1;
      else CloseGuest(FD);
    }
    return 0;
  }

  int64_t CloseGuest(uint64_t FD) {
    if (!GetFile(FD)) return BadFile;
    const auto& File = Descriptors().Files[FD];
    const bool Socket = File->Socket != nullptr;
    const bool Locked = File->Lock != FileLock::None;
    const auto Inode = File->Inode;
    Descriptors().Files[FD].reset();
    Descriptors().Flags[FD] = 0;
    if (Locked) {
      const auto Holders = FlockHolders.find(Inode);
      if (Holders != FlockHolders.end()) {
        std::erase_if(Holders->second, [](const auto& Weak) { return Weak.expired(); });
        if (Holders->second.empty()) FlockHolders.erase(Holders);
      }
    }
    if (Socket) std::erase_if(SocketNames, [](const auto& Entry) { return Entry.second.expired(); });
    const auto RangeLocks = RecordLocks.find(Inode);
    if (RangeLocks != RecordLocks.end()) {
      const auto& ClosingTable = Current ? Current->Descriptors : Task->PendingDescriptors;
      std::erase_if(RangeLocks->second, [&](const ByteLock& Lock) {
        return Lock.Expired() || (Lock.PID != -1 && Lock.Table.lock() == ClosingTable);
      });
      if (RangeLocks->second.empty()) RecordLocks.erase(RangeLocks);
      ++IOEpoch;
    }
    if (Socket || Locked) ++IOEpoch;
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
    if (!Size || Size > Task->Memory.MaxMapping || (Protection & ~7ULL) || ((Flags & (Shared | Private)) != Private && (Flags & (Shared | Private)) != Shared) ||
        (Flags & ~(Shared | Private | Fixed | Anonymous | DenyWrite | Executable | NoReserve | Stack)) || Offset % GuestPageSize ||
        (IsAnonymous && (Offset || static_cast<uint32_t>(FD) != UINT32_MAX))) return Invalid;
    auto* File = IsAnonymous ? nullptr : GetFile(FD);
    if (!IsAnonymous && (!File || File->PathOnly())) return BadFile;
    const auto Rounded = Task->Memory.RoundUp(Size);
    if (IsFixed && (Address % GuestPageSize || !Task->Memory.Owns(Address, Rounded, true) || OverlapsHeap(Address, Rounded))) return Invalid;
    const auto Access = PageFlags(Protection);
    if (!IsAnonymous && (Flags & Shared)) {
      if (!File->Memory) return -19;
      auto& Memory = *File->Memory;
      if (!ReadAccess(File->StatusFlags)) return -13;
      const bool MayWrite = (File->StatusFlags & 3) == 2 && !Memory.WritesSealed();
      if ((Access & PF_W) && !MayWrite) return Memory.WritesSealed() ? -1 : -13;
      if (IsFixed || (Protection & 4) || Offset >= Memory.Size ||
          Rounded > GuestAddressSpace::RoundUp(Memory.Size) - Offset) return Invalid;
      const auto Base = reinterpret_cast<uint64_t>(Memory.Storage.get()) + Offset;
      if (!Task->Memory.RegisterSharedFile(Base, Rounded, Access, Memory.Storage, MayWrite)) return -12;
      // Direct access cannot fault per page, so the mapped range is allocated now.
      Memory.MarkAllocated(Offset, Offset + Rounded);
      return Base;
    }
    if (IsFixed) SuspendPipePages(*Task, Address, Rounded, true);
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
    if ((Access & PF_W) && !Task->Memory.AllowsFileMappingWrite(Address, Rounded)) return -13;
    InvalidateExecutableRange(Frame, Address, Rounded, Access);
    return Task->Memory.Set(Address, Rounded, Access) ? 0 : Invalid;
  }

  int64_t UnmapGuest(FEXCore::Core::CpuStateFrame* Frame, uint64_t Address, uint64_t Size) {
    if (Address % GuestPageSize || !Size || Size > Task->Memory.MaxMapping) return Invalid;
    const auto Rounded = Task->Memory.RoundUp(Size);
    if (!Task->Memory.Owns(Address, Rounded) || OverlapsHeap(Address, Rounded)) return Invalid;
    SuspendPipePages(*Task, Address, Rounded, true);
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
  std::string LaunchFolder;
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
  std::vector<std::weak_ptr<StreamSocket>> SocketRightsReceivers;
  uint64_t NextAnonymousInode {4};
  uint64_t NextSdInode {InodeSnapshot::FirstInode};
  InodeNames SdInodes;
  InodeRecords Inodes;
  std::set<uint64_t> RetiredBackingObjects;
  FileReadCache ReadCache;
  std::set<uint64_t> DirtyFileData;
  std::vector<std::weak_ptr<OpenFile>> DiskFiles;
  std::map<uint64_t, std::weak_ptr<FifoState>> FifoEndpoints;
  uint64_t InodeGeneration {};
  bool InodeStoreAvailable {};
  bool InodeStoreDirty {};
  std::map<uint64_t, std::vector<std::weak_ptr<OpenFile>>> FlockHolders;
  std::map<uint64_t, std::vector<ByteLock>> RecordLocks;
  std::map<uint32_t, GuestProcess> Processes;
  GuestProcess* Task {};
  uint32_t ForegroundGroup {ProcessID};
  PadState Input {};
  const u64 StartTick {armGetSystemTick()};
  Framebuffer Video {};
  bool Presented {};

};

// Linux file operations for the launcher's FTP server. Each call goes
// through the same syscall handler a guest program would reach, on a
// namespace of its own with no guest running, so permissions, the inode
// store and overflow paths all behave as they do for guests.
class GuestFileService final : public FtpFileSystem {
public:
  GuestFileService() {
    Syscalls = std::make_unique<GuestSyscalls>(std::span<const Segment> {}, Root, Root, "/", false, true);
    auto& Task = *Syscalls->Task;
    Service.Descriptors = Task.PendingDescriptors;
    Service.PID = Service.TID = GuestSyscalls::ProcessID;
    Syscalls->Current = &Service;
    // There is no user database to log in against. Transfers act with
    // root's access so every file can be reached, like copying files to
    // the SD card directly.
    Task.UserIDs.fill(0);
    Task.GroupIDs.fill(0);
    Task.SupplementaryGroups.clear();
    Buffer = Task.Memory.Allocate(BufferSize + 2 * PathBytes + StatBytes, PF_R | PF_W);
  }

  ~GuestFileService() override {
    Syscalls.reset(); // Saves the inode store while the service thread exists.
  }

  bool Ready() const { return Buffer != 0; }

  int64_t Stat(const std::string& Path, FtpEntry& Entry, bool NoFollow) override {
    if (const auto Error = SetPath(0, Path)) return Error;
    return StatAt(CurrentDirectory, PathAddress(0), NoFollow, Entry);
  }

  int64_t List(const std::string& Path, std::vector<FtpEntry>& Entries) override {
    constexpr uint64_t Directory = 0200000, CloseOnExec = 02000000;
    if (const auto Error = SetPath(0, Path)) return Error;
    const auto FD = Syscalls->OpenGuest(CurrentDirectory, PathAddress(0), Directory | CloseOnExec, 0);
    if (FD < 0) return FD;
    int64_t Error {};
    for (;;) {
      const auto Count = Syscalls->ReadDirents(FD, Buffer, BufferSize, true);
      if (Count <= 0) {
        Error = Count;
        break;
      }
      for (int64_t Offset = 0; Offset < Count;) {
        const auto* Record = reinterpret_cast<const uint8_t*>(Buffer + Offset);
        uint16_t Length;
        std::memcpy(&Length, Record + 16, sizeof(Length));
        const std::string Name(reinterpret_cast<const char*>(Record + 19));
        Offset += Length;
        if (Name == "." || Name == "..") continue;
        FtpEntry Entry {};
        if (SetPath(1, Name) || StatAt(FD, PathAddress(1), true, Entry)) continue; // Removed meanwhile.
        Entry.Name = Name;
        Entries.push_back(std::move(Entry));
      }
    }
    Syscalls->CloseGuest(FD);
    return Error;
  }

  int64_t OpenRead(const std::string& Path, uint64_t Offset) override {
    return Open(Path, 0, Offset);
  }

  int64_t OpenWrite(const std::string& Path, uint64_t Offset, bool Append) override {
    constexpr uint64_t WriteOnly = 1, Create = 0100, Truncate = 01000, AppendFlag = 02000;
    return Open(Path, WriteOnly | Create | (Append ? AppendFlag : Offset ? 0 : Truncate), Append ? 0 : Offset);
  }

  int64_t Read(int64_t Handle, void* Output, size_t Count) override {
    const auto Result = Syscalls->ReadGuest(Handle, Buffer, std::min(Count, BufferSize));
    if (Result > 0) std::memcpy(Output, reinterpret_cast<const void*>(Buffer), Result);
    return Result;
  }

  int64_t Write(int64_t Handle, const void* Input, size_t Count) override {
    Count = std::min(Count, BufferSize);
    std::memcpy(reinterpret_cast<void*>(Buffer), Input, Count);
    return Syscalls->WriteGuest(Handle, Buffer, Count);
  }

  int64_t Close(int64_t Handle) override { return Syscalls->CloseGuest(Handle); }

  int64_t EnterDirectory(const std::string& Path) override {
    constexpr uint64_t Search = 1;
    FtpEntry Entry {};
    if (const auto Error = Stat(Path, Entry, false)) return Error;
    if ((Entry.Mode & 0170000) != 0040000) return -20;
    return Syscalls->AccessGuest(CurrentDirectory, PathAddress(0), Search, 0);
  }

  int64_t MakeDirectory(const std::string& Path) override {
    if (const auto Error = SetPath(0, Path)) return Error;
    const auto Error = Syscalls->CreateDirectoryGuest(CurrentDirectory, PathAddress(0), 0777);
    if (Error) return Error;
    return Syscalls->ChangeOwner(CurrentDirectory, PathAddress(0), DefaultOwner, DefaultOwner, SymlinkNoFollow);
  }

  int64_t RemoveDirectory(const std::string& Path) override {
    if (const auto Error = SetPath(0, Path)) return Error;
    return Syscalls->RemoveDirectoryGuest(CurrentDirectory, PathAddress(0));
  }

  int64_t Remove(const std::string& Path) override {
    if (const auto Error = SetPath(0, Path)) return Error;
    return Syscalls->UnlinkGuest(CurrentDirectory, PathAddress(0));
  }

  int64_t Rename(const std::string& From, const std::string& To) override {
    if (const auto Error = SetPath(0, From)) return Error;
    if (const auto Error = SetPath(1, To)) return Error;
    return Syscalls->RenameSdEntry(CurrentDirectory, PathAddress(0), CurrentDirectory, PathAddress(1), 0);
  }

  int64_t SetModifyTime(const std::string& Path, int64_t Seconds) override {
    constexpr int64_t Omit = (1 << 30) - 2; // UTIME_OMIT
    if (const auto Error = SetPath(0, Path)) return Error;
    const int64_t Times[4] {0, Omit, Seconds, 0};
    std::memcpy(reinterpret_cast<void*>(StatAddress()), Times, sizeof(Times));
    return Syscalls->UpdateFileTimes(CurrentDirectory, PathAddress(0), StatAddress(), 0, GuestSyscalls::TimeInput::Nanoseconds);
  }

private:
  static constexpr size_t BufferSize = 256 * 1024;
  static constexpr size_t PathBytes = GuestSyscalls::GuestPathMax;
  static constexpr size_t StatBytes = 256;
  static constexpr uint64_t CurrentDirectory = GuestSyscalls::CurrentDirectory;
  static constexpr uint64_t SymlinkNoFollow = 0x100, EmptyPath = 0x1000;
  // Owner the runtime reports for SD entries without metadata, and the
  // identity guests run as by default.
  static constexpr uint32_t DefaultOwner = 1000;

  // Declared before Syscalls so it outlives the namespace that points to it.
  GuestSyscalls::GuestThread Service {};
  std::unique_ptr<GuestSyscalls> Syscalls;
  uint64_t Buffer {};

  uint64_t PathAddress(int Slot) const { return Buffer + BufferSize + Slot * PathBytes; }
  uint64_t StatAddress() const { return Buffer + BufferSize + 2 * PathBytes; }

  // Copies a path into guest memory. PATH_MAX counts the terminating NUL.
  int64_t SetPath(int Slot, const std::string& Path) const {
    if (Path.size() >= PathBytes) return -36;
    std::memcpy(reinterpret_cast<void*>(PathAddress(Slot)), Path.c_str(), Path.size() + 1);
    return 0;
  }

  int64_t StatAt(uint64_t Directory, uint64_t Name, bool NoFollow, FtpEntry& Entry) {
    const auto Error = Syscalls->StatAtGuest(Directory, Name, StatAddress(), NoFollow ? SymlinkNoFollow : 0);
    if (Error) return Error;
    // x86-64 struct stat.
    const auto* Info = reinterpret_cast<const uint8_t*>(StatAddress());
    auto Field = [&]<typename T>(size_t Offset, T& Value) { std::memcpy(&Value, Info + Offset, sizeof(T)); };
    Field(16, Entry.Links);
    Field(24, Entry.Mode);
    Field(28, Entry.User);
    Field(32, Entry.Group);
    Field(48, Entry.Size);
    Field(88, Entry.ModifySeconds);
    return 0;
  }

  int64_t Open(const std::string& Path, uint64_t Flags, uint64_t Offset) {
    constexpr uint64_t Nonblocking = 04000, CloseOnExec = 02000000;
    if (const auto Error = SetPath(0, Path)) return Error;
    // Transfers are for regular files; FIFOs and devices would block or
    // stream forever.
    FtpEntry Entry {};
    const auto Existing = StatAt(CurrentDirectory, PathAddress(0), false, Entry);
    if (!Existing && (Entry.Mode & 0170000) == 0040000) return -21;
    if (!Existing && (Entry.Mode & 0170000) != 0100000) return -22;
    const auto FD = Syscalls->OpenGuest(CurrentDirectory, PathAddress(0), Flags | Nonblocking | CloseOnExec, 0666);
    if (FD >= 0 && Existing == -2) {
      // New files belong to the default owner of SD files, not root.
      if (const auto Error = Syscalls->ChangeOwner(FD, 0, DefaultOwner, DefaultOwner, EmptyPath, true)) {
        Syscalls->CloseGuest(FD);
        return Error;
      }
    }
    if (FD < 0 || !Offset) return FD;
    const auto Seek = Syscalls->SeekGuest(FD, static_cast<int64_t>(Offset), 0);
    if (Seek < 0) {
      Syscalls->CloseGuest(FD);
      return Seek;
    }
    return FD;
  }
};

} // namespace

std::unique_ptr<FtpFileSystem> OpenGuestFileSystem() {
  auto Service = std::make_unique<GuestFileService>();
  if (!Service->Ready()) return nullptr;
  return Service;
}

void SetLaunchCredentials(std::optional<std::pair<uint32_t, uint32_t>> Credentials) {
  LaunchCredentials = Credentials;
}

void SetRecoveryTestLaunch(bool Enabled) {
  RecoveryTestLaunch = Enabled;
}

const char* ReadRootfsElf(std::string_view Name, ElfImage& Image, ElfImage& Interpreter) {
  auto Namespace = std::make_unique<GuestSyscalls>(std::span<const Segment> {}, Root, Root, "/", false, true);
  if (const auto* Error = Namespace->InspectElf(Name, Image)) return Error;
  if (!Image.Interpreter.empty()) return Namespace->InspectElf(Image.Interpreter, Interpreter);
  return nullptr;
}

Result ReadRootfsDirectory(const std::string& Folder, std::vector<DirectoryEntry>& Entries) {
  auto Namespace = std::make_unique<GuestSyscalls>(std::span<const Segment> {}, Root, Root, "/", false, true);
  // Overflow directories exist only in the inode store.
  const auto Status = Namespace->HasNativeDirectory(Folder) ? ReadDirectory(Folder, Entries) : 0;
  if (R_SUCCEEDED(Status)) Namespace->AppendGuestDirectoryEntries(Folder, Entries);
  return Status;
}

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
  if (ProgramImage) Syscalls->TrackInitialExecutable(Program);
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
