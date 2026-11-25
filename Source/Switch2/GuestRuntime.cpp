// SPDX-License-Identifier: MIT
#include "GuestRuntime.h"
#include "GuestAddressSpace.h"
#include "GuestClocks.h"
#include "GuestCredentials.h"
#include "GuestDirectoryRecovery.h"
#include "GuestEpoll.h"
#include "GuestEventFd.h"
#include "GuestFileWrite.h"
#include "GuestFutex.h"
#include "GuestInet.h"
#include "GuestFilePunch.h"
#include "GuestInodeStore.h"
#include "GuestInotify.h"
#include "GuestJobControl.h"
#include "GuestMemoryFile.h"
#include "GuestMounts.h"
#include "GuestMqueue.h"
#include "GuestNamespaces.h"
#include "GuestPreempt.h"
#include "GuestScript.h"
#include "GuestSeccomp.h"
#include "GuestSignals.h"
#include "GuestPosixAcl.h"
#include "GuestRootFS.h"
#include "GuestSched.h"
#include "GuestSocket.h"
#include "GuestSysvIpc.h"
#include "GuestTerminal.h"
#include "GuestTimers.h"
#include "GuestVideo.h"
#include "HostNet.h"

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
#include <atomic>
#include <variant>
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
// Capability numbers (linux/capability.h) beyond those in GuestCredentials.h.
constexpr uint32_t CapDacReadSearch = 2, CapFowner = 3, CapFsetid = 4, CapNetBindService = 10, CapNetAdmin = 12, CapNetRaw = 13, CapIpcLock = 14, CapIpcOwner = 15, CapSysPtrace = 19, CapSysAdmin = 21,
                   CapSysResource = 24, CapCheckpointRestore = 40;
namespace SockOpt = Switch2Guest::SockOpt;
constexpr size_t GuestPageSize = PageSize;

// Guest memory arena: one reservation in the Horizon alias region, created on
// first use and kept for the life of the app (HostArena.cpp backend).
std::unique_ptr<ArenaBackend> GuestArenaBackend;
std::unique_ptr<GuestArena> GuestArenaInstance;
bool EnsureGuestArena() {
  if (GuestArena::Current()) return true;
  uint64_t Base {};
  size_t Size {};
  GuestArenaBackend = MakeHostArenaBackend(Base, Size);
  if (!GuestArenaBackend) {
    WriteLog("Guest arena: the alias region could not be reserved.");
    return false;
  }
  WriteLog(HostHeapReport().c_str());
  GuestArenaInstance = std::make_unique<GuestArena>(*GuestArenaBackend, Base, Size, HostArenaCommitLimit());
  GuestArena::Current() = GuestArenaInstance.get();
  char Message[160];
  std::snprintf(Message, sizeof(Message), "Guest arena: %#llx+%#zx, commit limit %zu MiB",
                static_cast<unsigned long long>(Base), Size, GuestArenaInstance->CommitLimit() >> 20);
  WriteLog(Message);
  return true;
}

} // namespace (reopened below)
void ShutdownGuestArena() {
  if (GuestArenaInstance) {
    GuestArenaInstance->Decommit(GuestArenaInstance->BaseAddress(), GuestArenaInstance->VirtualSize());
    if (GuestArenaInstance->Committed()) WriteLog("Guest arena: memory still committed at exit.");
  }
  HostHeapRestore();
}
namespace {

// SD calls sometimes fail with kernel InvalidMemoryState (2001-0106,
// 0xd401) before the filesystem service acts: seen in console runs 0.13.5
// (rootfs setup) and 0.13.11 (fs-suite), with no cause found. The call had
// no effect, so it is retried up to two more times; each retry is logged.
// The log names the calling code as an offset into the NRO (resolve with addr2line on the .elf) to find
// which call and buffer the kernel rejects.
// Retry lines go to the log file at once (WriteRunCheckpoint): the buffered
// trace is trimmed and lost on a crash (0.15.11). Each line also holds the
// memory state of every pointer argument.
extern "C" char _start[];
template<typename Value>
void SdLogPointer(char*& Out, const char* End, const Value& Argument) {
  if constexpr (std::is_pointer_v<Value>) {
    if (Out >= End) return;
    MemoryInfo Info {};
    u32 Page {};
    const auto Address = reinterpret_cast<uintptr_t>(Argument);
    const auto Status = svcQueryMemory(&Info, &Page, Address);
    const auto Written = std::snprintf(Out, End - Out, " [%#lx: %s type %#x attr %#x perm %#x]", static_cast<unsigned long>(Address),
                                       R_SUCCEEDED(Status) ? "ok" : "query failed", Info.type, Info.attr, Info.perm);
    if (Written > 0) Out += std::min<ptrdiff_t>(Written, End - Out);
  }
}
template<typename Function, typename... Arguments>
[[gnu::noinline]] Result SdRetry(Function Call, Arguments... Values) {
  constexpr Result InvalidMemoryState = MAKERESULT(Module_Kernel, KernelError_InvalidMemoryState);
  Result Status = Call(Values...);
  for (int Retry = 1; Retry <= 2 && Status == InvalidMemoryState; ++Retry) {
    char Message[512];
    char* Out = Message;
    const char* End = Message + sizeof(Message);
    Out += std::snprintf(Out, End - Out, "SD result 2001-0106, retry %d of 2 (caller nro+%#llx)", Retry,
                         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(__builtin_return_address(0)) -
                                                         reinterpret_cast<uintptr_t>(_start)));
    (SdLogPointer(Out, End, Values), ...);
    WriteRunCheckpoint(Message);
    Status = Call(Values...);
  }
  if (Status == InvalidMemoryState) WriteRunCheckpoint("SD result 2001-0106 persisted after 2 retries.");
  return Status;
}

// Guest memory lives in the arena in the alias region, committed with svcMapPhysicalMemory. The fs service
// maps an IPC buffer from the caller's memory and rejects a page state it does not take (kernel
// InvalidMemoryState, 2001-0106), so file data of the guest goes through a host heap buffer in bounded chunks.
constexpr size_t SdBounceBytes = 64 * 1024;
std::vector<char>& SdBounce() {
  static std::vector<char> Buffer(SdBounceBytes);
  return Buffer;
}

Result SdWriteGuest(FsFile* File, s64 Offset, const void* Data, u64 Count, u32 Option) {
  auto& Bounce = SdBounce();
  for (u64 Done = 0; Done < Count;) {
    const auto Chunk = std::min<u64>(Count - Done, Bounce.size());
    std::memcpy(Bounce.data(), static_cast<const char*>(Data) + Done, Chunk);
    const bool Last = Done + Chunk == Count;
    if (const auto Status = SdRetry(fsFileWrite, File, Offset + static_cast<s64>(Done), Bounce.data(), Chunk, Last ? Option : FsWriteOption_None); R_FAILED(Status)) return Status;
    Done += Chunk;
  }
  return 0;
}

// Reads up to Count bytes at Offset into Data. Received is the byte count; a short chunk ends the read.
Result SdReadGuest(FsFile* File, s64 Offset, void* Data, u64 Count, u64* Received) {
  auto& Bounce = SdBounce();
  *Received = 0;
  while (*Received < Count) {
    const auto Chunk = std::min<u64>(Count - *Received, Bounce.size());
    u64 Got {};
    if (const auto Status = SdRetry(fsFileRead, File, Offset + static_cast<s64>(*Received), Bounce.data(), Chunk, FsReadOption_None, &Got); R_FAILED(Status)) {
      return Status;
    }
    std::memcpy(static_cast<char*>(Data) + *Received, Bounce.data(), Got);
    *Received += Got;
    if (Got < Chunk) break;
  }
  return 0;
}
constexpr size_t MainStackSize = 1024 * 1024;
constexpr uint64_t LinuxWriteSyscall = 1;
constexpr uint64_t LinuxExitSyscall = 60;

// Timed preemption (GuestPreempt.h). The scheduler publishes the running
// guest thread; a watchdog thread sets the poll request word in its FEXCore
// interrupt page when it must yield. Generated code polls that word at block
// entries and backward branches and returns from ExecuteThread.
struct PreemptControl {
  Mutex Lock {};
  // Guarded by Lock. Running changes only on the scheduler thread.
  FEXCore::Core::InternalThreadState* Running {};
  uint64_t RunningSince {};
  uint64_t NextDeadline {};
  PreemptInputs Hints {};
  bool Armed {};
  std::atomic<bool> StopWatchdog {};
  uint64_t Switches {}, Unresolved {};
  Thread Watchdog {};
  bool WatchdogStarted {};
};
PreemptControl Preempt;

// Descriptions of watchable files that were destroyed (Linux __fput calls
// fsnotify_close); the runtime turns them into inotify close events.
struct ClosedFile {
  uint64_t Inode;
  std::string Path;
  bool Write, Directory;
};
std::vector<ClosedFile> ClosedFiles;

std::atomic_ref<uint64_t> PollWord(FEXCore::Core::InternalThreadState* Thread, size_t Offset) {
  return std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(Thread->InterruptFaultPage + Offset));
}

void PreemptWatchdog(void*) {
  while (!Preempt.StopWatchdog.load()) {
    svcSleepThread(PreemptQuantumNs / 2);
    mutexLock(&Preempt.Lock);
    if (Preempt.Running && !Preempt.Armed) {
      auto Inputs = Preempt.Hints;
      Inputs.Now = armTicksToNs(armGetSystemTick());
      Inputs.RunningSince = Preempt.RunningSince;
      Inputs.TimerDue = Inputs.TimerDue || (Preempt.NextDeadline && Inputs.Now >= Preempt.NextDeadline);
      if (ShouldPreempt(Inputs)) {
        PollWord(Preempt.Running, FEXCore::Core::InterruptPollRequestOffset).store(1, std::memory_order_release);
        Preempt.Armed = true;
      }
    }
    mutexUnlock(&Preempt.Lock);
  }
}

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
    EnsureGuestArena();
    LoadInodeStore();
    CleanStaleTmpfs();
    Task = &Processes.try_emplace(ProcessID).first->second;
    Task->Ns = NsRegistry.Initial;
    Task->Ns.Uts->Hostname = "fex-switch2";
    Task->Ns.Mnt->Table = BuildInitialMounts();
    Task->Ns.Ipc->Store = std::make_shared<IpcStore>();
    if (LaunchCredentials) {
      Task->Creds = Switch2Guest::LaunchCredentialsFor(LaunchCredentials->first, LaunchCredentials->second);
    }
    Task->HasDispatched = true;
    Task->Fs->Folder = std::move(Folder);
    Task->Fs->WorkingDirectory = std::move(WorkingDirectory);
    Task->Fs->Root = this->RootPath;
    Task->Fs->RootMount = Task->Fs->FolderMount = MntTable().ChildAt(MntTable().Top().get(), this->RootPath);
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
    StartPreemptWatchdog();
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
    std::string Resolved;
    if (!Program.empty() && !PathStat(CurrentDirectory, Program, Info, &Resolved) && (Info.Mode & 0170000) == 0100000) {
      Task->ExecutableInode = Info.Inode;
      Task->ExecutablePath = std::move(Resolved);
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

  void StartPreemptWatchdog() {
    if (Preempt.WatchdogStarted) return;
    Preempt.Switches = Preempt.Unresolved = 0;
    Preempt.StopWatchdog = false;
    s32 Priority {};
    svcGetThreadPriority(&Priority, CUR_THREAD_HANDLE);
    // A higher priority on the guest's core lets the watchdog run while the
    // guest computes; it sleeps between checks.
    const auto Status = threadCreate(&Preempt.Watchdog, PreemptWatchdog, nullptr, nullptr, 0x4000, Priority - 1, -2);
    Preempt.WatchdogStarted = R_SUCCEEDED(Status) && R_SUCCEEDED(threadStart(&Preempt.Watchdog));
    StartedWatchdog = Preempt.WatchdogStarted;
    if (!Preempt.WatchdogStarted) {
      if (R_SUCCEEDED(Status)) threadClose(&Preempt.Watchdog);
      WriteLog("Could not start the guest preemption watchdog.");
    }
  }

  void StopPreemptWatchdog() {
    // Inspect-only instances never start the watchdog and must not stop the
    // running guest's.
    if (!StartedWatchdog) return;
    StartedWatchdog = false;
    Preempt.StopWatchdog = true;
    threadWaitForExit(&Preempt.Watchdog);
    threadClose(&Preempt.Watchdog);
    Preempt.WatchdogStarted = false;
  }

  ~GuestSyscalls() override {
    StopPreemptWatchdog();
    FlushAllTmpfs();
    if (SaveInodeStore() < 0) WriteLog("Failed to save guest inode metadata.");
    CloseVideo();
    // Queued socket descriptors can refer back to their receive socket.
    for (const auto& Entry : SocketRightsReceivers) {
      if (auto Socket = Entry.lock()) {
        Socket->Rights.clear();
        for (auto& Queued : Socket->Records) Queued.Files.clear();
      }
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
      // Preemption can return a busy thread every quantum; keep the
      // terminal responsive while guests compute.
      if (Interactive) PollTerminalInput();
      FireTimers();
      if (!ClosedFiles.empty()) DrainClosedFiles();
      // Host sockets raise events on their own: poll them once per pass while a thread waits on one.
      if (NetWaiting()) PollNet(0);
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
          CopyOut(Current->SetTidOnStart, OwnTid(*Current));
          Current->SetTidOnStart = 0;
        }
        const auto Before = CurrentThreadTicks();
        QuantumStart = Before;
        InDispatch = true;
        if (Task->TraceFirstDispatch && Logging == LogLevel::Trace) {
          LogFirstDispatch(*Thread.Core->CurrentFrame);
          FirstDispatchCompileCheckpoints = 11;
        }
        Task->TraceFirstDispatch = false;
        // Also drops unblocked ignored signals, as Linux get_signal does.
        if (Thread.RerunBeforeSignals) Thread.RerunBeforeSignals = false;
        else if (DeliverableSignals(Thread)) DeliverSignals(Thread);
        // A dropped stop signal can leave a sleep or futex wait unfinished.
        if (!Thread.Exited && !Task->Stopped && Thread.Waiting == WaitKind::None) Dispatch(*Thread.Core->CTX);
        FirstDispatchCompileCheckpoints = 0;
        const auto Used = CurrentThreadTicks() - Before;
        InDispatch = false;
        Task->CpuTicks += Used;
        Thread.CpuTicks += Used;
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
          if (Task->AutoReap) Task->Reaped = true;
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
        const auto TimerDue = NextTimerDeadline(false);
        if (!TimerDue && std::none_of(Threads.begin(), Threads.end(), [](const auto& Thread) {
              return Thread.Deadline || Thread.Waiting == WaitKind::Terminal || Thread.Waiting == WaitKind::Net;
            })) {
          PrintStage("Guest threads are blocked with no remaining producer.");
          break;
        }
        if (Interactive) PollTerminalInput();
        // Sleep until the next deadline, at most 8 ms.
        uint64_t Wake = TimerDue;
        for (const auto& Thread : Threads) {
          if (Thread.Core && !Thread.Exited && Thread.Deadline) Wake = Wake ? std::min(Wake, Thread.Deadline) : Thread.Deadline;
        }
        const auto Now = MonotonicNs();
        const auto Nanoseconds = Wake ? std::clamp<int64_t>(static_cast<int64_t>(Wake - std::min(Wake, Now)), 0, 8'000'000) : 8'000'000;
        // A thread that waits on a host socket sleeps in the host's poll instead, so the socket can end the sleep.
        if (NetWaiting() && Nanoseconds >= 1'000'000) PollNet(static_cast<int>(Nanoseconds / 1'000'000));
        else svcSleepThread(Nanoseconds);
      }
      if (DispatchFailed) break;
    }
    for (auto& Thread : Threads) {
      if (Thread.Core) Thread.Core->CTX->DestroyThread(Thread.Core);
      Thread = {};
    }
    Current = nullptr;
    FlushGuestOutput();
    char Summary[96];
    std::snprintf(Summary, sizeof(Summary), "Guest preemption: %llu switches, %llu unresolved",
                  static_cast<unsigned long long>(Preempt.Switches), static_cast<unsigned long long>(Preempt.Unresolved));
    WriteLog(Summary);
    return ProcessExited ? std::optional {ExitStatus} : std::nullopt;
  }

  uint64_t Number {};
  bool YieldRequested {};
  bool StartedWatchdog {};

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
    // Seccomp runs first, for every syscall (also those that would return ENOSYS and the exits). A value
    // replaces the syscall's result.
    const auto Denied = SeccompGate(Frame);
    if (!Denied && (Registers[REG_RAX] == LinuxExitSyscall || Registers[REG_RAX] == 231)) {
      // exit ends the thread and, with the last thread, the process;
      // exit_group ends the process.
      if (Registers[REG_RAX] == LinuxExitSyscall) ExitThread();
      if (Registers[REG_RAX] == 231 || std::none_of(Threads.begin(), Threads.end(), [&](const auto& Thread) {
            return Thread.Core && Thread.PID == Task->PID && !Thread.Exited;
          })) {
        ExitProcess(Registers[REG_RDI] & 0xff, 0);
      }
      std::longjmp(ReturnToHost, 1);
    }

    constexpr int64_t LinuxEnosys = -38;
    const auto A = Registers[REG_RDI], B = Registers[REG_RSI], C = Registers[REG_RDX];
    const uint64_t Arguments[] {A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]};
    std::optional<FsHook> Hook;
    if (!Denied && InotifyWatching()) Hook = BeforeFs(Registers[REG_RAX], Arguments);
    int64_t Result = Denied.value_or(LinuxEnosys);
    if (!Denied) switch (Registers[REG_RAX]) {
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
    case 29: Result = ShmGet(A, B, C); break;
    case 30: Result = ShmAttachGuest(A, B, C); break;
    case 31: Result = ShmControl(A, B, C); break;
    case 67: Result = ShmDetachGuest(Frame, A); break;
    case 64: Result = SemGet(A, B, C); break;
    case 65: Result = SemOperate(A, B, C, 0); break;
    case 66: Result = SemControl(A, B, C, Registers[REG_R10]); break;
    case 220: Result = SemOperate(A, B, C, Registers[REG_R10]); break;
    case 68: Result = MsgGet(A, B); break;
    case 69: Result = MsgSend(A, B, C, Registers[REG_R10]); break;
    case 70: Result = MsgReceive(A, B, C, static_cast<int64_t>(Registers[REG_R10]), Registers[REG_R8]); break;
    case 71: Result = MsgControl(A, B, C); break;
    case 240: Result = MqueueOpen(A, B, C, Registers[REG_R10]); break;
    case 241: Result = MqueueUnlink(A); break;
    case 242: Result = MqueueSend(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 243: Result = MqueueReceive(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 244: Result = MqueueNotifyRegister(A, B); break;
    case 245: Result = MqueueAttributes(A, B, C); break;
    case 9:
      Result = MapGuest(Frame, A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]);
      break;
    case 10: Result = ProtectGuest(Frame, A, B, C); break;
    case 11: Result = UnmapGuest(Frame, A, B); break;
    case 12: Result = SetHeapEnd(A); break;
    case 25: Result = RemapGuest(Frame, A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 26: Result = SyncMemory(A, B, C); break;
    case 27: Result = MincoreMemory(A, B, C); break;
    case 149: Result = LockMemory(A, B, false); break;
    case 150: Result = LockMemory(A, B, true); break;
    case 151: Result = LockAll(A); break;
    case 152: Result = UnlockAll(); break;
    case 325: Result = (C & ~1ULL) ? Invalid : LockMemory(A, B, false); break; // mlock2: only MLOCK_ONFAULT
    // remap_file_pages (216), userfaultfd (323), memfd_secret (447) and cachestat (451)
    // stay ENOSYS: see CommunicationInventory.md.
    case 237: Result = BindMemory(A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]); break;
    case 238: Result = SetMemoryPolicy(A, B, C); break;
    case 239: Result = GetMemoryPolicy(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 256: Result = MigratePages(A, B, C, Registers[REG_R10]); break;
    case 279: Result = MovePages(A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]); break;
    case 310: case 311:
      Result = ProcessVm(Registers[REG_RAX] == 311, A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]);
      break;
    case 324: Result = Membarrier(A, B, C); break;
    case 329: // pkey_mprotect: no protection keys, so only pkey -1 (plain mprotect) is valid
      Result = static_cast<int32_t>(Registers[REG_R10]) == -1 ? ProtectGuest(Frame, A, B, C) : Invalid;
      break;
    case 330: Result = A || (B & ~uint64_t {3}) ? Invalid : -28; break; // pkey_alloc: no PKU in CPUID, ENOSPC
    case 331: Result = Invalid; break; // pkey_free: no key is allocated
    case 440: Result = ProcessAdvise(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 448: Result = ProcessRelease(A, B); break;
    case 450: Result = SetHomeNode(A, B, C, Registers[REG_R10]); break;
    case 453: Result = NotSupported; break; // map_shadow_stack: no CET
    case 462: Result = SealMemory(A, B, C); break;
    case 13: Result = SignalAction(A, B, C, Registers[REG_R10]); break;
    case 14: Result = SignalMask(A, B, C, Registers[REG_R10]); break;
    case 15:
      if (ReturnGuestSignal(Frame)) std::longjmp(ReturnToHost, 1);
      Result = Fault;
      break;
    case 127: Result = PendingSignalsGuest(A, B); break;
    case 34: Result = PauseGuest(); break;
    case 128: Result = TimedWaitSignal(A, B, C, Registers[REG_R10]); break;
    case 129: Result = QueueSignalInfo(A, A, B, C, false); break;
    case 130: Result = SuspendGuest(A, B); break;
    case 200: Result = static_cast<int32_t>(A) > 0 ? SignalThread(-1, PidIn(A), B, MakeSigInfo(B, SiTkill, Task->PID, Task->Creds.Uid[0])) : Invalid; break;
    case 219: Result = -4; break; // restart_syscall: no restart block without stopped sleeps (Task 6).
    case 234: Result = TkillGuest(A, B, C); break;
    case 297: Result = QueueSignalInfo(A, B, C, Registers[REG_R10], true); break;
    case 16: Result = GetFile(A) && GetFile(A)->NsObject ? NsIoctl(*GetFile(A), B, C) : TerminalIoctl(A, B, C); break;
    case 17: Result = ReadGuest(A, B, C, Registers[REG_R10]); break;
    case 18: Result = WriteGuest(A, B, C, Registers[REG_R10]); break;
    case 19: Result = VectorIO(static_cast<uint32_t>(A), B, static_cast<uint32_t>(C), false); break;
    case 20: Result = VectorIO(static_cast<uint32_t>(A), B, static_cast<uint32_t>(C), true); break;
    case 21: Result = AccessGuest(CurrentDirectory, A, B, 0); break;
    case 22: Result = CreatePipe(A, 0); break;
    case 24: Result = 0; YieldRequested = true; break;
    case 28: Result = AdviseGuest(A, B, C); break;
    case 32: Result = FileControl(A, 0, 0); break;
    case 33: Result = DuplicateDescriptor(A, B, false); break;
    case 40: Result = SendFile(A, B, C, Registers[REG_R10]); break;
    case 39: Result = PidOut(Task->PID); break;
    case 124: Result = GetSession(A); break;
    case 112: Result = SetSession(); break;
    case 111: Result = PidOut(Task->Group); break;
    case 110: Result = PidOut(Task->ParentPID); break;
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
    case 51: case 52: Result = SocketAddressCall(Registers[REG_RAX] == 52, A, B, C); break;
    case 53: Result = SocketPair(A, B, C, Registers[REG_R10]); break;
    case 54: Result = SetSocketOptionCall(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 55: Result = GetSocketOptionCall(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 56: Result = CloneGuest(Frame, A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 57: Result = RequestClone({SigChld}); break;
    case 58: Result = RequestClone({CloneVm | CloneVfork | SigChld}); break;
    case 435: Result = Clone3(Frame, A, B); break;
    case 59: Result = ExecGuest(CurrentDirectory, A, B, C, 0); break;
    case 322: Result = ExecGuest(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 434: Result = PidfdOpen(A, B); break;
    case 424: Result = PidfdSendSignal(A, B, C, Registers[REG_R10]); break;
    case 438: Result = PidfdGetFd(A, B, C); break;
    case 61: Result = Wait4(A, B, C, Registers[REG_R10]); break;
    case 247: Result = WaitId(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 98: Result = ResourceUsage(A, B); break;
    case 100: Result = ProcessTimes(A); break;
    case 135: Result = SetPersonality(A); break;
    case 140: Result = GetPriority(A, B); break;
    case 141: Result = SetPriority(A, B, C); break;
    case 309: Result = (A && CopyOut(A, uint32_t {})) || (B && CopyOut(B, uint32_t {})) ? Fault : 0; break; // getcpu: CPU 0, node 0
    case 63: Result = UnameGuest(A); break;
    case 170: Result = SetUtsName(false, A, B); break;
    case 171: Result = SetUtsName(true, A, B); break;
    case 272: Result = UnshareGuest(A); break;
    case 308: Result = SetNsGuest(A, B); break;
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
    case 95: Result = std::exchange(Task->Fs->CreationMask, A & 0777); break;
    case 155: Result = PivotRootGuest(A, B); break;
    case 161: Result = ChrootGuest(A); break;
    case 165: Result = MountGuest(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 166: Result = UmountGuest(A, B); break;
    case 97: Result = PrLimit(0, A, 0, B); break;
    case 160: Result = PrLimit(0, A, B, 0); break;
    case 99: Result = SystemInfo(A); break;
    case 102: Result = UidOut(Task->Creds.Uid[0]); break;
    case 104: Result = GidOut(Task->Creds.Gid[0]); break;
    case 107: Result = UidOut(Task->Creds.Uid[1]); break;
    case 108: Result = GidOut(Task->Creds.Gid[1]); break;
    case 115: Result = GuestGroups(false, A, B); break;
    case 116: Result = GuestGroups(true, A, B); break;
    case 105: Result = SetIds(0, A, 0, 0); break;
    case 106: Result = SetIds(1, A, 0, 0); break;
    case 113: Result = SetIds(2, A, B, 0); break;
    case 114: Result = SetIds(3, A, B, 0); break;
    case 125: Result = GetCapabilities(A, B); break;
    case 126: Result = SetCapabilities(A, B); break;
    case 117: Result = SetIds(4, A, B, C); break;
    case 118: Result = GetGuestIDs(false, A, B, C); break;
    case 119: Result = SetIds(5, A, B, C); break;
    case 120: Result = GetGuestIDs(true, A, B, C); break;
    case 317: Result = SeccompCall(static_cast<uint32_t>(A), static_cast<uint32_t>(B), C); break;
    case 157: Result = GuestPrivilegeControl(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 122: Result = SetIds(6, A, 0, 0); break;
    case 123: Result = SetIds(7, A, 0, 0); break;
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
    case 186: Result = PidOut(Current->TID); break;
    case 188: case 189: case 190:
    case 191: case 192: case 193:
    case 194: case 195: case 196:
    case 197: case 198: case 199:
      Result = LegacyXattr(Registers[REG_RAX], A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 463: case 464:
      Result = XattrWithArgs(Registers[REG_RAX] == 463, A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]); break;
    case 465: Result = Xattr(XattrOp::List, A, B, C, 0, Registers[REG_R10], Registers[REG_R8], 0); break;
    case 466: Result = Xattr(XattrOp::Remove, A, B, C, Registers[REG_R10], 0, 0, 0); break;
    case 202: Result = Futex(A, B, C, Registers[REG_R10], Registers[REG_R8], static_cast<uint32_t>(Registers[REG_R9])); break;
    case 273: Result = SetRobustList(A, B); break;
    case 274: Result = GetRobustList(A, B, C); break;
    case 449: Result = FutexWaitv(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 454: Result = Futex2Wake(A, B, C, Registers[REG_R10]); break;
    case 455: Result = Futex2Wait(A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]); break;
    case 456: Result = Futex2Requeue(A, B, C, Registers[REG_R10]); break;
    case 204: Result = GetAffinity(A, B, C); break;
    case 203: Result = SetAffinity(A, B, C); break;
    case 142: Result = SchedSetScheduler(A, 0, B, true); break;
    case 143: Result = SchedGetParam(A, B); break;
    case 144: Result = SchedSetScheduler(A, B, C, false); break;
    case 145: Result = SchedGetScheduler(A); break;
    case 146: Result = Switch2Guest::Sched::PriorityMax(static_cast<int32_t>(A)); break;
    case 147: Result = Switch2Guest::Sched::PriorityMin(static_cast<int32_t>(A)); break;
    case 148: Result = SchedRrInterval(A, B); break;
    case 314: Result = SchedSetAttr(A, B, C); break;
    case 315: Result = SchedGetAttr(A, B, C, Registers[REG_R10]); break;
    case 251: Result = IoprioSet(A, B, C); break;
    case 252: Result = IoprioGet(A, B); break;
    case 312: Result = Kcmp(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    // Machine-wide operations: EPERM for everyone, root included (the guest does not own the machine). Their
    // capabilities are absent from every set (MachineWideCaps). ptrace 101 is EPERM until it is implemented;
    // syslog 103 is dmesg_restrict; the rest are vhangup 153, acct 163, swapon 167, swapoff 168, reboot 169,
    // init_module 175, delete_module 176, kexec_load 246, finit_module 313, kexec_file_load 320 and bpf 321.
    case 101: case 103: case 153: case 163: case 167: case 168: case 169: case 175: case 176: case 246: case 313:
    case 320: case 321: Result = -1; break;
    case 172: Result = Switch2Guest::Admin::Iopl(static_cast<uint32_t>(A)); break;
    case 173: Result = Switch2Guest::Admin::Ioperm(A, B, C); break;
    case 298: Result = Available(A, PF_R) < 8 ? Fault : -1; break; // perf_event_open: attr is read first, then EPERM
    // Features the emulated kernel is built without, and obsolete calls: ENOSYS, as for any unknown number.
    case 134: case 139: case 156: case 174: case 177: case 178: case 179: case 180: case 181: case 182: case 183:
    case 184: case 185: case 206: case 207: case 208: case 209: case 210: case 212: case 214: case 215: case 236:
    case 248: case 249: case 250: case 333: case 334: case 335: case 425: case 426: case 427: case 428: case 429:
    case 430: case 431: case 432: case 433: case 442: case 443: case 444: case 445: case 446: case 457: case 458:
    case 459: case 460: case 461: Result = LinuxEnosys; break;
    case 217: Result = ReadDirents(A, B, C, true); break;
    case 218: Result = SetTidAddress(A); break;
    case 228: Result = ClockGuest(A, B); break;
    case 229: Result = ClockResolution(A, B); break;
    case 227: Result = ClockSetTime(A, B); break;
    case 96: Result = GetTimeOfDay(A, B); break;
    case 164: Result = SetTimeOfDay(A, B); break;
    case 201: Result = TimeGuest(A); break;
    case 159: Result = AdjustTime(A); break;
    case 305: Result = ClockAdjustTime(A, B); break;
    case 35: Result = SleepGuest(1, 0, A, B); break;
    case 36: Result = GetITimer(A, B); break;
    case 37: Result = AlarmGuest(static_cast<uint32_t>(A)); break;
    case 38: Result = SetITimer(A, B, C); break;
    case 222: Result = TimerCreate(A, B, C); break;
    case 223: Result = TimerSetTime(A, B, C, Registers[REG_R10]); break;
    case 224: Result = TimerGetTime(A, B); break;
    case 225: Result = TimerGetOverrun(A); break;
    case 226: Result = TimerDelete(A); break;
    case 283: Result = TimerFdCreate(A, B); break;
    case 286: Result = TimerFdSetTime(A, B, C, Registers[REG_R10]); break;
    case 287: Result = TimerFdGetTime(A, B); break;
    case 284: Result = EventFdCreate(A, 0); break;
    case 290: Result = EventFdCreate(A, B); break;
    case 282: Result = SignalFdCreate(A, B, C, 0); break;
    case 289: Result = SignalFdCreate(A, B, C, Registers[REG_R10]); break;
    case 230: Result = SleepGuest(A, B, C, Registers[REG_R10]); break;
    case 258: Result = CreateDirectoryGuest(A, B, C); break;
    case 263: Result = C == 0x200 ? RemoveDirectoryGuest(A, B) : C ? Invalid : UnlinkGuest(A, B); break;
    case 264: Result = RenameSdEntry(A, B, C, Registers[REG_R10], 0); break;
    case 316: Result = RenameSdEntry(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 262: Result = StatAtGuest(A, B, C, Registers[REG_R10]); break;
    case 268: Result = ChangeModeAt(A, B, C, 0); break;
    case 452: Result = ChangeModeAt(A, B, C, Registers[REG_R10]); break;
    case 269: Result = AccessGuest(A, B, C, 0); break;
    case 253: Result = InotifyInit(0); break;
    case 294: Result = InotifyInit(A); break;
    case 254: Result = InotifyAddWatch(A, B, static_cast<uint32_t>(C)); break;
    case 255: Result = InotifyRemoveWatch(A, B); break;
    case 23: Result = SelectTimeval(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 213: Result = static_cast<int32_t>(A) <= 0 ? Invalid : EpollCreate(0); break;
    case 291: Result = EpollCreate(A); break;
    case 233: Result = EpollCtl(A, B, C, Registers[REG_R10]); break;
    case 232: Result = EpollWait(A, B, C, static_cast<int32_t>(Registers[REG_R10])); break;
    case 281: Result = EpollPwait(A, B, C, static_cast<int32_t>(Registers[REG_R10]), Registers[REG_R8], Registers[REG_R9]); break;
    case 441: Result = EpollPwait2(A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]); break;
    case 270: Result = SelectGuest(A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]); break;
    case 271: Result = PollGuestWithTimespec(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 275: Result = SpliceGuest(A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]); break;
    case 276: Result = SpliceGuest(A, 0, B, 0, C, Registers[REG_R10], true); break;
    case 277: Result = SyncFileRange(A, B, C, Registers[REG_R10]); break;
    case 285: Result = AllocateFileRange(A, B, C, Registers[REG_R10]); break;
    case 278: Result = VmspliceGuest(A, B, C, Registers[REG_R10]); break;
    case 288: Result = AcceptSocket(A, B, C, Registers[REG_R10]); break;
    case 299: Result = ReceiveMultiple(A, B, C, Registers[REG_R10], Registers[REG_R8]); break;
    case 307: Result = SendMultiple(A, B, C, Registers[REG_R10]); break;
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
    case 302: Result = PrLimit(A, B, C, Registers[REG_R10]); break;
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
    if (!Denied && Result > 0) {
      // Bytes the runtime stored into guest memory may land on translated code.
      switch (Registers[REG_RAX]) {
      case 0: case 17: case 45: InvalidateExecutableStores(B, Result); break;
      case 19: case 295: case 327: InvalidateExecutableIovecs(B, C, Result); break;
      case 47:
        if (uint64_t Message[4] {}; Available(B, PF_R) >= sizeof(Message)) {
          std::memcpy(Message, reinterpret_cast<const void*>(B), sizeof(Message));
          InvalidateExecutableIovecs(Message[2], Message[3], Result);
        }
        break;
      case 299: // recvmmsg: each message's iovecs, msg_len bytes
        for (int64_t Index = 0; Index < std::min<int64_t>(Result, UioMaxIov); ++Index) {
          const auto Entry = B + Index * MultiMessageSize;
          uint64_t Message[4] {};
          uint32_t Length {};
          if (Available(Entry, PF_R) < MultiMessageSize) break;
          std::memcpy(Message, reinterpret_cast<const void*>(Entry), sizeof(Message));
          std::memcpy(&Length, reinterpret_cast<const void*>(Entry + sizeof(LinuxMessage)), sizeof(Length));
          InvalidateExecutableIovecs(Message[2], Message[3], Length);
        }
        break;
      default: break;
      }
    }
    if (!Denied && Result >= 0 && Result != RetrySyscall) AfterFs(Hook, Registers[REG_RAX], Arguments, Result);
    if (!ClosedFiles.empty()) DrainClosedFiles();
    if (Result == RetrySyscall || Task->PendingExec) {
      if (Result == RetrySyscall) Current->SeccompRetryRip = Frame->State.rip;
      std::longjmp(ReturnToHost, 1);
    }
    if (Current->SuspendMask && MaskedWait(Registers[REG_RAX]) && Result != -4) {
      Current->SignalMask = *std::exchange(Current->SuspendMask, std::nullopt);
    }
    constexpr uint64_t NoSignal = 0x4000;
    const auto Call = Registers[REG_RAX];
    const bool SuppressPipeSignal = (Call == 44 && (Registers[REG_R10] & NoSignal)) ||
                                    (Call == 46 && (C & NoSignal)) || (Call == 307 && (Registers[REG_R10] & NoSignal)) ||
                                    (Call == 328 && (Registers[REG_R9] & VectorNoSignal));
    // Linux send_sig(SIGPIPE, current, 0): SI_USER from the writer itself.
    if (Result == -32 && !SuppressPipeSignal) {
      SendSignal(*Task, Current, MakeSigInfo(SigPipe, SiUser, Task->PID, Task->Creds.Uid[0]));
    }
    Current->Deadline = 0;
    Current->TimedSocketWait = false;
    Current->WaitSignals = 0;
    if (const auto Due = NextTimerDeadline(); Due && MonotonicNs() >= Due) FireTimers();
    if (HasActionableSignal(*Current)) {
      Registers[REG_RAX] = Result;
      Frame->State.rip += 2;
      DeliverSignals(*Current);
      std::longjmp(ReturnToHost, 1);
    }
    if (DeliverableSignals(*Current)) DeliverSignals(*Current); // only ignored ones: dropped
    if (Logging == LogLevel::Trace || (Logging == LogLevel::Detailed && Result < 0 && Result >= -4095)) {
      char Message[160];
      std::snprintf(Message, sizeof(Message), "Guest syscall %llu at %#llx returned %lld",
                    static_cast<unsigned long long>(Registers[REG_RAX]), static_cast<unsigned long long>(Frame->State.rip),
                    static_cast<long long>(Result));
      std::string Line = Message;
      Line += FailedPathDetail(Registers[REG_RAX], Registers);
      WriteLog(Line.c_str());
    }
    if (Task->StartupSyscallCheckpoints) {
      Task->LastStartupSyscall = Registers[REG_RAX] + 1;
      Registers[REG_RAX] = Result;
      Frame->State.rip += 2;
      std::longjmp(ReturnToHost, 1);
    }
    Registers[REG_RAX] = Result;
    Frame->State.rip += 2;
    if (std::exchange(YieldRequested, false) || std::any_of(Threads.begin(), Threads.end(), [&](auto& Thread) {
          return &Thread != Current && Thread.Core && !Thread.Exited &&
                 Thread.PID == Current->PID && Ready(Thread);
        })) {
      std::longjmp(ReturnToHost, 1);
    }
    RefreshPreemptHints();
  }

  void PreCompile() override {
    if (Logging == LogLevel::Trace) {
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
  // Net: like IO, but the event can come from outside (a host socket), so the scheduler polls the host.
  enum class WaitKind { None, Futex, IO, Terminal, Lock, Sleep, Signal, Vfork, Net };
  static constexpr size_t DescriptorCount = 1024; // Linux default RLIMIT_NOFILE and FD_SETSIZE
  // The thread table; also the RLIMIT_NPROC default. Slots allocate their
  // state on use. A shell pipeline under command substitution and timeout
  // needs about ten processes; 8 was too few for the Debian tool checks.
  static constexpr size_t GuestThreadCount = 32;
  static constexpr uint64_t SignalPendingDefault = 4096; // RLIMIT_SIGPENDING default
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
  // The clock a timer counts in. Deadlines are kept in the clock's own
  // domain, so setting the realtime clock moves realtime timers, as Linux
  // hrtimers on the realtime base move. CPU clocks name a process or thread.
  enum class TimerDomain : uint8_t { Monotonic, Realtime, Tai, ProcessCpu, ThreadCpu };
  struct TimerClock {
    TimerDomain Domain {};
    uint32_t Owner {}; // pid or tid of a CPU clock
  };

  // timerfd_create: expirations accumulate in Ticks until read.
  struct TimerFdState {
    TimerClock Clock;
    Switch2Guest::ArmedTimer Timer;
    uint64_t Ticks {};
    uint64_t Changes {}; // counts changes to Ticks, for edge-triggered epoll
    bool CancelOnSet {}; // TFD_TIMER_CANCEL_ON_SET on an absolute realtime timer
    bool Canceled {};    // the realtime clock was set since; read reports ECANCELED once
  };

  // timer_create. Notify is sigev_notify (SIGEV_SIGNAL 0, SIGEV_NONE 1,
  // SIGEV_THREAD 2 which the kernel treats as SIGEV_SIGNAL, SIGEV_THREAD_ID 4).
  struct PosixTimer {
    int32_t Id {};
    TimerClock Clock;
    Switch2Guest::ArmedTimer Timer;
    int32_t Notify {};
    uint32_t Signal {};
    uint64_t Value {};
    uint32_t Tid {};
    int32_t OverrunLast {}; // si_overrun of the last delivered signal
    uint32_t Sequence {};   // bumped by timer_settime; older queued signals are stale
  };

  // An epoll interest: Linux keys items on the (file description, fd) pair.
  // The item goes away when the description is destroyed, not when one of
  // its descriptors closes.
  struct EpollKey {
    std::weak_ptr<OpenFile> File;
    const OpenFile* Raw {};
    int32_t Fd {};
    bool operator==(const EpollKey& Other) const { return Raw == Other.Raw && Fd == Other.Fd; }
  };
  struct EpollState {
    Switch2Guest::EpollSet<EpollKey> Set;
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
    SignalQueue Pending;
    // rt_sigsuspend's saved mask, restored through the first handler frame.
    std::optional<uint64_t> SuspendMask;
    // rt_sigtimedwait's set while it waits (Linux real_blocked).
    uint64_t WaitSignals {};
    uint64_t CpuTicks {};
    bool RerunBeforeSignals {}; // see Ready()
    // Bytes a blocking stream write already queued before it waited for
    // space; the rerun continues after them (Linux pipe_write and
    // unix_stream_sendmsg loop inside one call).
    uint64_t StreamProgress {};
    // sendmmsg/recvmmsg: messages already moved (the call resumes there after a wait); recvmmsg: the
    // end of its timeout. TimedSocketWait: the wait runs under SO_RCVTIMEO or SO_SNDTIMEO, so a signal ends it with EINTR.
    uint64_t BatchDone {};
    uint64_t MultiMessageEnd {};
    // sendmmsg/recvmmsg: msg_len of the message in progress. A signal that
    // ends a half-done stream message counts it with its bytes (Linux).
    uint64_t BatchEntry {};
    bool TimedSocketWait {};
    std::vector<SavedGuestSignal> SignalFrames;
    std::array<uint64_t, 3> AltStack {0, 2, 0};
    uint64_t Deadline {};
    uint64_t SleepRemaining {}; // Guest timespec for the unslept time, or 0.
    // A CPU-clock sleep ends when process CpuClockPid has used CpuDeadline ns of CPU time.
    uint64_t CpuDeadline {};
    uint32_t CpuClockPid {};
    // An absolute REALTIME/TAI sleep keeps its target so a clock set re-arms Deadline.
    uint64_t RealtimeTarget {};
    bool RealtimeTai {};
    uint64_t WaitAddress {};
    // Futex wait state (WaitKind::Futex). WaitSeq orders waiters FIFO.
    FutexKey WaitKey {};
    uint32_t WaitBitset {};
    bool WaitPi {};            // queued on a PI futex (LOCK_PI, or requeued by CMP_REQUEUE_PI)
    bool WaitRequeuePi {};     // FUTEX_WAIT_REQUEUE_PI: waits on WaitKey for a requeue to WaitTarget
    FutexKey WaitTarget {};
    std::vector<FutexKey> WaitKeys; // futex_waitv
    uint64_t WaitSeq {};
    enum class FutexInterrupt : uint8_t { Linux, Restart, Eintr, Again } WaitInterrupt {};
    uint64_t RobustHead {};
    size_t RobustLength {};
    uint32_t TID {};
    uint32_t PID {};
    uint64_t SetTidOnStart {};
    WaitKind Waiting {};
    bool Exited {};
    // Blocked in a System V IPC call (IpcWaitKind 1 semop, 2 msgsnd, 3 msgrcv) on IpcWaitId. The
    // rerun tells a removed object (EIDRM) from a stale id (EINVAL).
    bool IpcBlocked {};
    uint8_t IpcWaitKind {};
    int32_t IpcWaitId {};
    uint16_t SemWaitNum {};
    bool SemWaitZero {};
    const void* MqWait {}; // the queue a mq_timedreceive is blocked on
    uint32_t MemoryPolicy {};        // set_mempolicy mode (single NUMA node)
    uint64_t MemoryPolicyNodes {};
    // Scheduling attributes (sched_setattr and friends), CPU mask (reporting only), I/O priority and the
    // identity of its io_context (0: none yet) for kcmp. The nice value lives in the process.
    Switch2Guest::Sched::State Sched;
    // Seccomp mode and filter chain (per thread, as in Linux). SeccompRetryRip is the rip of a syscall that
    // blocked and will run again: its filter already allowed it.
    Switch2Guest::Seccomp::State Seccomp;
    uint64_t SeccompRetryRip {};
    uint64_t CpuMask {~uint64_t {0}};
    uint16_t IoPriority {};
    uint64_t IoContext {};
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
    if (Current) {
      Slot->Sched = Current->Sched;
      Slot->CpuMask = Current->CpuMask;
      Slot->IoPriority = Current->IoPriority;
      Slot->IoContext = Current->IoContext;
      Slot->Seccomp = Current->Seccomp; // fork and clone inherit the filters
    }
    return true;
  }

  void Dispatch(ContextType& Context) {
    auto* Frame = Current->Core->CurrentFrame;
    // The syscall trampoline spills the guest registers. Its host call/return
    // cache is dispensable and may contain entries invalidated by another guest thread.
    Frame->State.callret_sp = Current->ReturnStackTop;
    Frame->InSyscallInfo = 0;
    OpenPreemptWindow();
    if (setjmp(ReturnToHost) == 0) {
      Context.ExecuteThread(Current->Core);
      if (!ResumeAfterPreemption()) {
        DispatchFailed = true;
        PrintStage("Dispatcher returned before the guest reached its syscall.");
      }
    }
    ClosePreemptWindow();
  }

  // ExecuteThread returned through a preemption poll: the stop handler has
  // spilled the guest registers, and the poll stored the host address of the
  // entry point or backward-branch target it stopped at. OpenPreemptWindow
  // clears that word, so any other stop leaves it 0. Rebuild the guest RIP.
  bool ResumeAfterPreemption() {
    auto* Thread = Current->Core;
    const auto Target = PollWord(Thread, FEXCore::Core::InterruptPollTargetOffset).load(std::memory_order_relaxed);
    if (!Target) return false;
    const auto RIP = Thread->CTX->RestoreRIPFromHostPC(Thread, Target);
    if (!RIP) {
      ++Preempt.Unresolved;
      return false;
    }
    Thread->CurrentFrame->State.rip = RIP;
    ++Preempt.Switches;
    if (Logging == LogLevel::Trace) {
      char Message[96];
      std::snprintf(Message, sizeof(Message), "Preempted tid=%u at rip=%#llx", Current->TID, static_cast<unsigned long long>(RIP));
      WriteRunCheckpoint(Message);
    }
    return true;
  }

  // Publishes the thread about to run and what the watchdog needs to decide
  // whether it must yield.
  void OpenPreemptWindow() {
    PollWord(Current->Core, FEXCore::Core::InterruptPollTargetOffset).store(0, std::memory_order_relaxed);
    mutexLock(&Preempt.Lock);
    Preempt.Running = Current->Core;
    Preempt.RunningSince = MonotonicNs();
    mutexUnlock(&Preempt.Lock);
    RefreshPreemptHints();
  }

  // Ready() may complete another thread's wait; that thread runs on the next
  // scheduler pass anyway. Called again after every syscall, since a syscall
  // can make another thread or process ready.
  void RefreshPreemptHints() {
    bool Other {};
    uint64_t NextDeadline {};
    for (auto& Thread : Threads) {
      if (&Thread == Current || !Thread.Core || Thread.Exited) continue;
      if (Ready(Thread)) Other = true;
      if (Thread.Deadline) NextDeadline = NextDeadline ? std::min(NextDeadline, Thread.Deadline) : Thread.Deadline;
    }
    if (const auto Timer = NextTimerDeadline()) NextDeadline = NextDeadline ? std::min(NextDeadline, Timer) : Timer;
    mutexLock(&Preempt.Lock);
    Preempt.NextDeadline = NextDeadline;
    Preempt.Hints = {};
    Preempt.Hints.OtherRunnable = Other;
    // The console's line discipline turns Ctrl+C, Ctrl+Z and Ctrl+\ into
    // signals whether or not a thread reads the terminal, so a busy loop
    // with no reader (a shell's own `while :; do :; done`) must still return
    // to the scheduler to poll the keyboard.
    Preempt.Hints.TerminalInput = Interactive;
    mutexUnlock(&Preempt.Lock);
  }

  void ClosePreemptWindow() {
    mutexLock(&Preempt.Lock);
    if (Preempt.Running) PollWord(Preempt.Running, FEXCore::Core::InterruptPollRequestOffset).store(0, std::memory_order_relaxed);
    Preempt.Armed = false;
    Preempt.Running = nullptr;
    mutexUnlock(&Preempt.Lock);
  }

  static uint64_t MonotonicNs() { return armTicksToNs(armGetSystemTick()); }

  static uint64_t CurrentThreadTicks() {
    u64 Ticks {};
    svcGetInfo(&Ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, TickCountInfo_Total);
    return Ticks;
  }

  struct GuestProcess;
  // CPU time the running thread has used in its current quantum; the scheduler only adds it to
  // the stored ticks once the guest returns.
  uint64_t LiveTicks() const { return InDispatch ? CurrentThreadTicks() - QuantumStart : 0; }
  uint64_t ProcessTicks(const GuestProcess& Process) const { return Process.CpuTicks + (&Process == Task ? LiveTicks() : 0); }
  uint64_t ThreadTicks(const GuestThread& Thread) const { return Thread.CpuTicks + (&Thread == Current ? LiveTicks() : 0); }
  uint64_t ProcessCpuNs(const GuestProcess& Process) const { return armTicksToNs(ProcessTicks(Process)); }
  uint64_t ThreadCpuNs(const GuestThread& Thread) const { return armTicksToNs(ThreadTicks(Thread)); }

  bool Ready(GuestThread& Thread) {
    // A stopped process runs again on SIGCONT; SIGKILL still ends it.
    const auto& Owner = Processes.at(Thread.PID);
    const bool Killed = (Thread.Pending.Set() | Owner.SharedPending.Set()) & SignalBit(SigKill);
    if (Owner.Stopped) return Killed;
    if (Thread.Waiting == WaitKind::Vfork) {
      // Only SIGKILL ends the wait early (TASK_KILLABLE).
      const auto* Child = FindProcess(Thread.WaitAddress);
      if (Child && !Child->Exited && !Child->Image && !Killed) return false;
      Thread.Waiting = WaitKind::None;
      return true;
    }
    if (Thread.Waiting == WaitKind::None) return true;
    const bool Expired = Thread.Deadline && MonotonicNs() >= Thread.Deadline;
    // Like Linux do_wait or pipe_read, a woken blocking call rescans before
    // it checks for signals: the syscall reruns first and a pending signal
    // is delivered when it completes or blocks again.
    if ((Thread.Waiting == WaitKind::IO || Thread.Waiting == WaitKind::Net || Thread.Waiting == WaitKind::Terminal || Thread.Waiting == WaitKind::Lock) &&
        Thread.WaitAddress != IOEpoch && HasActionableSignal(Thread)) {
      Thread.Waiting = WaitKind::None;
      Thread.RerunBeforeSignals = true;
      return true;
    }
    if (HasActionableSignal(Thread)) return true;
    if (Thread.Waiting == WaitKind::Signal) {
      // rt_sigtimedwait reruns to take a signal from its set or time out.
      const auto& Process = Processes.at(Thread.PID);
      if (!Expired && !((Thread.Pending.Set() | Process.SharedPending.Set()) & Thread.WaitSignals)) return false;
      Thread.Waiting = WaitKind::None;
      return true;
    }
    if (Thread.Waiting == WaitKind::Sleep) {
      // A CPU-clock sleep ends when the clock's process has used enough CPU time. A process that
      // is gone ends it too: its clock can no longer advance.
      const bool CpuDone = Thread.CpuDeadline && [&] {
        const auto* Target = FindProcess(Thread.CpuClockPid);
        return !Target || ProcessCpuNs(*Target) >= Thread.CpuDeadline;
      }();
      if (!Expired && !CpuDone) return false;
      Thread.Core->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX] = 0;
      Thread.Deadline = 0;
      Thread.CpuDeadline = 0;
      Thread.RealtimeTarget = 0;
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
    if ((Thread.Waiting == WaitKind::IO || Thread.Waiting == WaitKind::Net || Thread.Waiting == WaitKind::Terminal || Thread.Waiting == WaitKind::Lock) &&
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

  // Guest CLOCK_REALTIME: the Horizon RTC second sampled once at startup plus real elapsed time
  // (see Switch2Guest::RealtimeBase). Everything that reads wall time goes through here.
  uint64_t RealTimeNs() const {
    const auto Now = Realtime.Now(MonotonicNs());
    return Now > 0 ? static_cast<uint64_t>(Now) : 0;
  }
  uint64_t TaiNs() const {
    const auto Now = Realtime.TaiNow(MonotonicNs());
    return Now > 0 ? static_cast<uint64_t>(Now) : 0;
  }

  // The guest realtime clock was set (clock_settime, settimeofday, adjtimex ADJ_SETOFFSET).
  // Absolute REALTIME/TAI sleeps are re-armed against the new time, as Linux does when the clock is set.
  // Realtime timers keep realtime deadlines and need no re-arming; TFD_TIMER_CANCEL_ON_SET timerfds
  // become readable and report ECANCELED once (timerfd_clock_was_set).
  void RealtimeClockSet() {
    const auto Mono = MonotonicNs();
    for (auto& Thread : Threads) {
      if (!Thread.Core || Thread.Exited || Thread.Waiting != WaitKind::Sleep || !Thread.RealtimeTarget) continue;
      const auto ClockNow = Thread.RealtimeTai ? TaiNs() : RealTimeNs();
      Thread.Deadline = Thread.RealtimeTarget <= ClockNow ? Mono : AddDeadline(Mono, Thread.RealtimeTarget - ClockNow);
    }
    for (const auto& Weak : TimerFds) {
      const auto State = Weak.lock();
      if (!State || !State->CancelOnSet || State->Canceled) continue;
      State->Canceled = true;
      ++State->Ticks;
      ++State->Changes;
      ++IOEpoch;
    }
  }


  // The process or thread a CPU clock id names, or null (EINVAL). Linux pid_for_clock: pid 0 is the
  // caller; a per-thread clock must name a thread of the caller's process; a process clock must name a
  // process, and clock_gettime also accepts the caller's own thread id.
  GuestProcess* CpuClockProcess(const Switch2Guest::DecodedClock& Clock, bool Reading) {
    const auto Pid = Clock.Pid ? PidIn(Clock.Pid) : 0; // the clock id carries a number of the caller's PID namespace
    if (!Pid || Pid == Task->PID) return Task;
    if (Reading && Pid == Current->TID) return Task;
    return FindProcess(Pid);
  }
  const GuestThread* CpuClockThread(const Switch2Guest::DecodedClock& Clock) const {
    if (!Clock.Pid) return Current;
    for (const auto& Thread : Threads) {
      if (Thread.Core && !Thread.Exited && Thread.TID == PidIn(Clock.Pid) && Thread.PID == Task->PID) return &Thread;
    }
    return nullptr;
  }
  bool CpuClockValid(const Switch2Guest::DecodedClock& Clock) {
    return Clock.PerThread ? CpuClockThread(Clock) != nullptr : CpuClockProcess(Clock, false) != nullptr;
  }

  // Reads any readable clock in nanoseconds; returns 0 or a negative errno.
  int64_t ReadClock(const Switch2Guest::DecodedClock& Clock, uint64_t& Ns) {
    using Switch2Guest::ClockKind;
    switch (Clock.Kind) {
    case ClockKind::Realtime: case ClockKind::RealtimeAlarm: Ns = RealTimeNs(); return 0;
    case ClockKind::RealtimeCoarse: Ns = Switch2Guest::RoundCoarse(RealTimeNs()); return 0;
    case ClockKind::Tai: Ns = TaiNs(); return 0;
    // Horizon has no suspend to count, so BOOTTIME is MONOTONIC.
    case ClockKind::Monotonic: case ClockKind::MonotonicRaw: case ClockKind::Boottime: case ClockKind::BoottimeAlarm:
      Ns = MonotonicNs();
      return 0;
    case ClockKind::MonotonicCoarse: Ns = Switch2Guest::RoundCoarse(MonotonicNs()); return 0;
    case ClockKind::ProcessCpu: {
      const auto* Process = CpuClockProcess(Clock, true);
      if (!Process) return Invalid;
      Ns = ProcessCpuNs(*Process);
      return 0;
    }
    case ClockKind::ThreadCpu: {
      const auto* Thread = CpuClockThread(Clock);
      if (!Thread) return Invalid;
      Ns = ThreadCpuNs(*Thread);
      return 0;
    }
    case ClockKind::Invalid: break;
    }
    return Invalid;
  }

  // nanosleep and clock_nanosleep. The thread waits for a monotonic deadline
  // and the syscall completes from the scheduler. Absolute CLOCK_REALTIME and
  // CLOCK_TAI deadlines remember their target, so RealtimeClockSet re-arms them.
  // CLOCK_PROCESS_CPUTIME_ID sleeps wait until the process's CPU time reaches the target.
  int64_t SleepGuest(uint64_t ClockId, uint64_t Flags, uint64_t Request, uint64_t Remaining) {
    using Switch2Guest::ClockKind;
    constexpr uint64_t AbsoluteTime = 1; // TIMER_ABSTIME; Linux ignores other flag bits.
    const auto Clock = Switch2Guest::DecodeClock(static_cast<int32_t>(ClockId));
    // Error order follows Linux: clocks without a sleep fail before the request is read; the
    // alarm capability and CPU clock targets are checked after the request is validated.
    switch (Clock.Kind) {
      case ClockKind::Realtime: case ClockKind::Monotonic: case ClockKind::Boottime: case ClockKind::Tai:
      case ClockKind::RealtimeAlarm: case ClockKind::BoottimeAlarm: case ClockKind::ProcessCpu: break;
      // Static THREAD_CPUTIME, MONOTONIC_RAW and the coarse clocks have no nsleep. Dynamic
      // per-thread CPU clocks do, and fail after the request is read.
      case ClockKind::ThreadCpu:
        if (static_cast<int32_t>(ClockId) >= 0) return -95;
        break;
      case ClockKind::MonotonicRaw: case ClockKind::RealtimeCoarse: case ClockKind::MonotonicCoarse: return -95;
      case ClockKind::Invalid: return Invalid;
    }
    if (Available(Request, PF_R) < 16) return Fault;
    uint64_t Duration {};
    if (!ReadTimespec(Request, Duration)) return Invalid;
    // CAP_WAKE_ALARM gates the alarm clocks. Horizon has no RTC wakeup source and never suspends a
    // running application, so a permitted alarm sleep is the matching non-alarm sleep.
    if ((Clock.Kind == ClockKind::RealtimeAlarm || Clock.Kind == ClockKind::BoottimeAlarm) &&
        !Task->Creds.Capable(Switch2Guest::CapWakeAlarm)) return -1;
    const auto Now = MonotonicNs();
    uint64_t Deadline {};
    if (Clock.Kind == ClockKind::ProcessCpu || Clock.Kind == ClockKind::ThreadCpu) {
      // Per-thread CPU clocks cannot be slept on here (Linux allows other threads' clocks; not tracked).
      if (Clock.PerThread) return Invalid;
      const auto* Target = CpuClockProcess(Clock, false);
      if (!Target) return Invalid;
      const auto CpuNow = ProcessCpuNs(*Target);
      uint64_t CpuDeadline {};
      if (Flags & AbsoluteTime) {
        if (Duration <= CpuNow) return 0;
        CpuDeadline = Duration;
        Remaining = 0;
      } else {
        if (!Duration) return 0;
        CpuDeadline = AddDeadline(CpuNow, Duration);
      }
      Current->CpuDeadline = CpuDeadline;
      Current->CpuClockPid = Target->PID;
      Current->SleepRemaining = Remaining;
      Current->Waiting = WaitKind::Sleep;
      Current->Core->CurrentFrame->State.rip += 2;
      return RetrySyscall;
    }
    if (Flags & AbsoluteTime) {
      const bool OnRealtime = Clock.Kind == ClockKind::Realtime || Clock.Kind == ClockKind::RealtimeAlarm;
      const bool OnTai = Clock.Kind == ClockKind::Tai;
      const auto ClockNow = OnRealtime ? RealTimeNs() : OnTai ? TaiNs() : Now;
      if (Duration <= ClockNow) return 0;
      Deadline = AddDeadline(Now, Duration - ClockNow);
      Current->RealtimeTarget = OnRealtime || OnTai ? Duration : 0;
      Current->RealtimeTai = OnTai;
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
      uint64_t Left {};
      if (Thread.CpuDeadline) {
        // The unslept CPU time of a CPU-clock sleep.
        const auto* Target = FindProcess(Thread.CpuClockPid);
        const auto CpuNow = Target ? ProcessCpuNs(*Target) : Thread.CpuDeadline;
        Left = Thread.CpuDeadline > CpuNow ? Thread.CpuDeadline - CpuNow : 0;
      } else {
        const auto Now = MonotonicNs();
        Left = Thread.Deadline > Now ? Thread.Deadline - Now : 0;
      }
      const int64_t Time[] {static_cast<int64_t>(Left / 1'000'000'000), static_cast<int64_t>(Left % 1'000'000'000)};
      if (CopyOut(Thread.SleepRemaining, Time)) Result = Fault;
    }
    Thread.SleepRemaining = 0;
    Thread.Deadline = 0;
    Thread.CpuDeadline = 0;
    Thread.RealtimeTarget = 0;
  }

  int64_t CloneThread(CoreFrame* Frame, uint64_t Flags, uint64_t Stack, uint64_t ParentTid, uint64_t ChildTid, uint64_t TLS) {
    constexpr uint64_t Shared = 0x10f00; // VM, FS, FILES, SIGHAND, THREAD
    constexpr uint64_t SetTLS = 0x80000, ParentSetTid = 0x100000, ChildClearTid = 0x200000, ChildSetTid = 0x1000000;
    // Threads must share memory, filesystem state, descriptors and handlers.
    if ((Flags & Shared) != Shared || Flags & ~(Shared | SetTLS | ParentSetTid | ChildClearTid | ChildSetTid | 0x40000ULL)) return Invalid;
    // A thread shares its process's PID namespace: not after unshare(CLONE_NEWPID), and not once the init is gone.
    if (Task->Ns.Pid != Task->Ns.PidForChildren) return Invalid;
    for (const auto* N = Task->Ns.Pid.get(); N; N = N->Parent.get()) {
      if (!N->Adding) return -12;
    }
    if (NprocExceeded()) return -11;
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
    if (Current->Sched.Policy == Switch2Guest::Sched::Deadline && !Current->Sched.ResetOnFork) return -11; // sched_fork: EAGAIN
    if (!AddThread(*Frame->Thread->CTX, State, TID, Flags & ChildClearTid ? ChildTid : 0, Task->PID)) return -11;
    ++NextTid;
    const uint32_t Local = Task->Ns.Pid->Allocate(TID); // the thread's number in its namespace (and the ancestors')
    {
      // The new thread resets its policy when asked to; nice is process-wide here, so it stays.
      auto& Born = *std::find_if(Threads.begin(), Threads.end(), [TID](const auto& Entry) { return Entry.Core && Entry.TID == TID; });
      int32_t Unused = Task->Nice;
      Switch2Guest::Sched::ForkInherit(Born.Sched, Unused);
      if (Born.IoContext && !(Flags & CloneIo)) Born.IoContext = ++NextIoContext; // copy_io: CLONE_IO shares it
    }
    if (Flags & ParentSetTid) std::memcpy(reinterpret_cast<void*>(ParentTid), &Local, sizeof(Local));
    if (Flags & ChildSetTid) std::memcpy(reinterpret_cast<void*>(ChildTid), &Local, sizeof(Local));
    return Local;
  }

  int64_t SetTidAddress(uint64_t Address) {
    Current->ClearTid = Address;
    return PidOut(Current->TID);
  }

  // ---- Futex ----
  // Waiters are keyed by (address, pid), pid 0 for shared mappings, so a
  // shared futex wakes waiters in any process. WaitSeq keeps queues FIFO.
  uint64_t FutexSequence {};

  FutexKey KeyFor(uint64_t Address, bool Private = false) const {
    return MakeFutexKey(Address, Task->PID, !Private && Task->Memory.IsShared(Address, sizeof(uint32_t)));
  }

  bool LoadWord(uint64_t Address, uint32_t& Word) const {
    if (Available(Address, PF_R) < sizeof(Word)) return false;
    std::memcpy(&Word, reinterpret_cast<const void*>(Address), sizeof(Word));
    return true;
  }

  void StoreWord(uint64_t Address, uint32_t Word) { std::memcpy(reinterpret_cast<void*>(Address), &Word, sizeof(Word)); }

  // The owner number in a futex word is the thread's number in its PID namespace (gettid there).
  bool ThreadAlive(uint32_t TID) const {
    const auto Global = PidIn(TID);
    return std::any_of(Threads.begin(), Threads.end(), [Global](const auto& Thread) { return Thread.Core && !Thread.Exited && Thread.TID == Global; });
  }

  static bool WaitsOn(const GuestThread& Thread, const FutexKey& Key, size_t* Index = nullptr) {
    if (Thread.WaitKeys.empty()) {
      if (Index) *Index = 0;
      return Thread.WaitKey == Key;
    }
    for (size_t I = 0; I < Thread.WaitKeys.size(); ++I) {
      if (Thread.WaitKeys[I] != Key) continue;
      if (Index) *Index = I;
      return true;
    }
    return false;
  }

  // Blocked futex waiters on Key (PI or plain queue), oldest first.
  std::vector<GuestThread*> FutexQueue(const FutexKey& Key, bool Pi) {
    std::vector<GuestThread*> Queue;
    for (auto& Thread : Threads) {
      if (Thread.Core && !Thread.Exited && Thread.Waiting == WaitKind::Futex && Thread.WaitPi == Pi && WaitsOn(Thread, Key)) {
        Queue.push_back(&Thread);
      }
    }
    std::sort(Queue.begin(), Queue.end(), [](const auto* L, const auto* R) { return L->WaitSeq < R->WaitSeq; });
    return Queue;
  }

  void WakeThread(GuestThread& Thread, int64_t Result) {
    Thread.Core->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX] = Result;
    Thread.Waiting = WaitKind::None;
    Thread.Deadline = 0;
    Thread.WaitPi = false;
    Thread.WaitRequeuePi = false;
  }

  // Linux futex_wake: a count of zero or less still wakes one waiter.
  uint32_t WakeKey(const FutexKey& Key, int32_t Count, uint32_t Bitset) {
    const uint32_t Limit = Count > 0 ? static_cast<uint32_t>(Count) : 1;
    uint32_t Woken {};
    for (auto* Thread : FutexQueue(Key, false)) {
      if (!(Thread->WaitBitset & Bitset)) continue;
      size_t Index {};
      WaitsOn(*Thread, Key, &Index);
      // A FUTEX_WAIT_REQUEUE_PI waiter woken by a plain wake returns EAGAIN.
      WakeThread(*Thread, Thread->WaitRequeuePi ? -11 : static_cast<int64_t>(Index));
      if (++Woken >= Limit) break;
    }
    return Woken;
  }

  uint32_t WakeFutex(uint64_t Address, int32_t Count, uint32_t Bitset) { return WakeKey(KeyFor(Address), Count, Bitset); }

  // UNLOCK_PI handoff: the first PI waiter becomes the owner. Word is the
  // futex word being released.
  bool HandOffPi(const FutexKey& Key, uint32_t& Word) {
    const auto Queue = FutexQueue(Key, true);
    if (Queue.empty()) return false;
    auto* Waiter = Queue.front();
    Word = OwnTid(*Waiter) | (Word & FutexOwnerDied) | (Queue.size() > 1 ? FutexWaiters : 0);
    WakeThread(*Waiter, 0);
    return true;
  }

  // Absolute or relative timeout. Returns the error, or 0 with Deadline set
  // (0 = none) and Expired true when an absolute time already passed.
  int64_t FutexDeadline(uint64_t Timeout, bool Absolute, bool Realtime, uint64_t& Deadline, bool& Expired) {
    Deadline = 0;
    Expired = false;
    if (!Timeout) return 0;
    uint64_t Time {};
    if (!ReadTimespec(Timeout, Time)) return Available(Timeout, PF_R) < 16 ? Fault : Invalid;
    const auto Now = MonotonicNs();
    if (!Absolute) {
      Time = AddDeadline(Now, Time);
    } else if (Realtime) {
      const auto RealTime = RealTimeNs();
      if (Time <= RealTime) Expired = true;
      else Time = AddDeadline(Now, Time - RealTime);
    }
    if (Time <= Now) Expired = true;
    Deadline = Time;
    return 0;
  }

  // Parks the current thread. The syscall completes through WakeThread or Ready.
  int64_t BlockFutex(const FutexKey& Key, uint32_t Bitset, uint64_t Deadline, bool Pi, GuestThread::FutexInterrupt Interrupt) {
    Current->Waiting = WaitKind::Futex;
    Current->WaitKey = Key;
    Current->WaitKeys.clear();
    Current->WaitBitset = Bitset;
    Current->WaitPi = Pi;
    Current->WaitRequeuePi = false;
    Current->WaitSeq = ++FutexSequence;
    Current->WaitInterrupt = Interrupt;
    Current->Deadline = Deadline;
    Current->Core->CurrentFrame->State.rip += 2;
    return RetrySyscall;
  }

  int64_t FutexWait(uint64_t Address, bool Private, uint32_t Value, uint32_t Bitset, uint64_t Timeout, bool Absolute, bool Realtime) {
    uint64_t Deadline {};
    bool Expired {};
    if (const auto Error = FutexDeadline(Timeout, Absolute, Realtime, Deadline, Expired)) return Error;
    uint32_t Word {};
    if (!LoadWord(Address, Word)) return Fault;
    if (Word != Value) return -11;
    if (Expired) return -110;
    return BlockFutex(KeyFor(Address, Private), Bitset, Deadline, false, GuestThread::FutexInterrupt::Linux);
  }

  // FUTEX_REQUEUE / CMP_REQUEUE core: wake NrWake waiters of K1, then move up
  // to NrRequeue more to K2 behind K2's existing waiters (FIFO kept).
  int64_t RequeueFutex(const FutexKey& K1, const FutexKey& K2, int32_t NrWake, int32_t NrRequeue) {
    const auto Queue = FutexQueue(K1, false);
    int64_t Woken {}, Moved {};
    for (auto* Thread : Queue) {
      if (Woken >= NrWake && Moved >= NrRequeue) break;
      // futex_requeue checks each waiter it reaches: a WAIT_REQUEUE_PI waiter
      // ends the call with EINVAL after the earlier ones were handled.
      if (Thread->WaitRequeuePi) return Invalid;
      if (Woken < NrWake) {
        size_t Index {};
        WaitsOn(*Thread, K1, &Index);
        WakeThread(*Thread, static_cast<int64_t>(Index));
        ++Woken;
      } else if (Moved < NrRequeue) {
        if (Thread->WaitKeys.empty()) Thread->WaitKey = K2;
        else std::replace(Thread->WaitKeys.begin(), Thread->WaitKeys.end(), K1, K2);
        Thread->WaitSeq = ++FutexSequence;
        ++Moved;
      } else {
        break;
      }
    }
    return Woken + Moved;
  }

  // Futex word of a PI futex accessed read-write, or the Linux error.
  int64_t PiWordAccess(uint64_t Address) const {
    if (Address & 3) return Invalid;
    return Available(Address, PF_R | PF_W) < sizeof(uint32_t) ? Fault : 0;
  }

  int64_t LockPi(uint64_t Address, bool Private, bool Try, uint64_t Timeout, bool Realtime) {
    uint64_t Deadline {};
    bool Expired {};
    if (const auto Error = FutexDeadline(Timeout, true, Realtime, Deadline, Expired)) return Error;
    uint32_t Word {};
    if (!LoadWord(Address, Word) || Available(Address, PF_W) < sizeof(Word)) return Fault;
    const uint32_t Original = Word;
    const auto Key = KeyFor(Address, Private);
    switch (TryLockPi(Word, OwnTid(*Current), ThreadAlive(Word & FutexTidMask))) {
    case PiLock::Acquired: StoreWord(Address, Word); return 0;
    case PiLock::Deadlock: return -35;
    case PiLock::NoOwner: return -3;
    case PiLock::MustWait: break;
    }
    if (Try) {
      // Undo the waiters bit when nobody else waits.
      if (FutexQueue(Key, true).empty()) Word = Original;
      StoreWord(Address, Word);
      return -11;
    }
    StoreWord(Address, Word);
    if (Expired) return -110;
    // No priority inheritance: the owner is never boosted (waiters are FIFO).
    return BlockFutex(Key, UINT32_MAX, Deadline, true, GuestThread::FutexInterrupt::Restart);
  }

  int64_t UnlockPi(uint64_t Address, bool Private) {
    if (const auto Error = PiWordAccess(Address)) return Error;
    uint32_t Word {};
    LoadWord(Address, Word);
    if ((Word & FutexTidMask) != OwnTid(*Current)) return -1;
    if (!HandOffPi(KeyFor(Address, Private), Word)) Word = 0;
    StoreWord(Address, Word);
    return 0;
  }

  // CMP_REQUEUE_PI: the first waiter takes the PI futex at Address2 if it is
  // free, otherwise it and up to NrRequeue - 1 more queue on it as PI waiters.
  int64_t CmpRequeuePi(uint64_t Address, uint64_t Address2, bool Private, int32_t NrWake, int32_t NrRequeue, uint32_t Expected) {
    if (NrWake != 1 || NrRequeue < 0 || Address == Address2) return Invalid;
    if (const auto Error = PiWordAccess(Address2)) return Error;
    uint32_t Word {}, PiWord {};
    if (!LoadWord(Address, Word)) return Fault;
    if (Word != Expected) return -11;
    LoadWord(Address2, PiWord);
    const auto K1 = KeyFor(Address, Private), K2 = KeyFor(Address2, Private);
    const auto Queue = FutexQueue(K1, false);
    // Linux checks the top waiter before trying the lock, the rest as it reaches them.
    if (!Queue.empty() && !Queue.front()->WaitRequeuePi) return Invalid;
    // futex_requeue with requeue_pi: the top waiter takes the nr_wake slot
    // whether it acquires the lock (woken) or not (requeued), and up to
    // NrRequeue more are requeued (verified natively: 1 + nr_requeue move).
    int64_t Woken {}, Moved {};
    bool First = true;
    for (auto* Thread : Queue) {
      bool Extra = true;
      if (First) {
        First = false;
        Extra = false;
        const auto Owner = PiWord & FutexTidMask;
        const auto Lock = TryLockPi(PiWord, OwnTid(*Thread), ThreadAlive(Owner));
        if (Lock == PiLock::Deadlock) return -35;
        if (Lock == PiLock::NoOwner) return -3;
        if (Lock == PiLock::Acquired) {
          WakeThread(*Thread, 0);
          ++Woken;
          continue;
        }
      }
      if (Extra && Moved - (Woken ? 0 : 1) >= NrRequeue) break;
      if (!Thread->WaitRequeuePi) {
        // Earlier waiters stay moved, as in Linux.
        if (Moved) StoreWord(Address2, PiWord | FutexWaiters);
        else StoreWord(Address2, PiWord);
        return Invalid;
      }
      Thread->WaitKey = K2;
      Thread->WaitPi = true;
      Thread->WaitRequeuePi = false;
      // Requeued onto the rtmutex: a signal now ends the wait with EAGAIN.
      Thread->WaitInterrupt = GuestThread::FutexInterrupt::Again;
      Thread->WaitSeq = ++FutexSequence;
      ++Moved;
    }
    if (Moved) PiWord |= FutexWaiters;
    StoreWord(Address2, PiWord);
    return Woken + Moved;
  }

  int64_t Futex(uint64_t Address, uint64_t Operation, uint32_t Value, uint64_t Timeout, uint64_t Address2, uint32_t Value3) {
    constexpr uint32_t PrivateFlag = 128, ClockRealtime = 256;
    const auto Op = static_cast<uint32_t>(Operation);
    const auto Command = Op & ~(PrivateFlag | ClockRealtime);
    const bool Private = Op & PrivateFlag, Realtime = Op & ClockRealtime;
    // FUTEX_CLOCK_REALTIME only exists for WAIT_BITSET, WAIT_REQUEUE_PI and LOCK_PI2.
    if (Realtime && Command != 9 && Command != 11 && Command != 13) return -38;
    if (Command > 13) return -38;
    // For REQUEUE, CMP_REQUEUE and WAKE_OP the timeout slot carries val2.
    const auto Second = static_cast<int32_t>(Timeout);
    if (Address & 3) return Invalid;
    if (Available(Address, PF_R) < sizeof(uint32_t)) return Fault;
    const auto TwoAddresses = [&](bool Writable) -> int64_t {
      if (Address2 & 3) return Invalid;
      return Available(Address2, Writable ? PF_R | PF_W : PF_R) < sizeof(uint32_t) ? Fault : 0;
    };
    switch (Command) {
    case 0: return FutexWait(Address, Private, Value, UINT32_MAX, Timeout, false, false);
    case 9:
      if (!Value3) return Invalid;
      return FutexWait(Address, Private, Value, Value3, Timeout, true, Realtime);
    case 1: return WakeKey(KeyFor(Address, Private), static_cast<int32_t>(Value), UINT32_MAX);
    case 10:
      if (!Value3) return Invalid;
      return WakeKey(KeyFor(Address, Private), static_cast<int32_t>(Value), Value3);
    case 3: case 4: {
      if (static_cast<int32_t>(Value) < 0 || Second < 0) return Invalid;
      if (const auto Error = TwoAddresses(false)) return Error;
      if (Command == 4) {
        uint32_t Word {};
        LoadWord(Address, Word);
        if (Word != Value3) return -11;
      }
      return RequeueFutex(KeyFor(Address, Private), KeyFor(Address2, Private), static_cast<int32_t>(Value), Second);
    }
    case 5: {
      if (const auto Error = TwoAddresses(true)) return Error;
      uint32_t Word {}, Old {};
      LoadWord(Address2, Word);
      const auto Decoded = DecodeWakeOp(Value3);
      if (ApplyWakeOp(Decoded, Word, Old) < 0) return -38;
      StoreWord(Address2, Word);
      int64_t Woken = WakeKey(KeyFor(Address, Private), static_cast<int32_t>(Value), UINT32_MAX);
      if (CompareWakeOp(Decoded, Old)) Woken += WakeKey(KeyFor(Address2, Private), Second, UINT32_MAX);
      return Woken;
    }
    case 6: return LockPi(Address, Private, false, Timeout, true);
    case 13: return LockPi(Address, Private, false, Timeout, Realtime);
    case 8: return LockPi(Address, Private, true, 0, false);
    case 7: return UnlockPi(Address, Private);
    case 11: {
      // WAIT_REQUEUE_PI: wait on a plain futex for CMP_REQUEUE_PI to move
      // this thread to the PI futex at Address2.
      if (Address == Address2) return Invalid;
      if (const auto Error = TwoAddresses(true)) return Error;
      uint64_t Deadline {};
      bool Expired {};
      if (const auto Error = FutexDeadline(Timeout, true, Realtime, Deadline, Expired)) return Error;
      uint32_t Word {};
      LoadWord(Address, Word);
      if (Word != Value) return -11;
      if (Expired) return -110;
      BlockFutex(KeyFor(Address, Private), UINT32_MAX, Deadline, false, GuestThread::FutexInterrupt::Restart);
      Current->WaitRequeuePi = true;
      Current->WaitTarget = KeyFor(Address2, Private);
      return RetrySyscall;
    }
    default: // 12
      if (const auto Error = TwoAddresses(true)) return Error;
      return CmpRequeuePi(Address, Address2, Private, static_cast<int32_t>(Value), Second, Value3);
    }
  }

  // futex2 flags: SIZE_U32 (required) and PRIVATE. Anything else is EINVAL (Linux 6.7).
  static int64_t Futex2Flags(uint32_t Flags, bool& Private) {
    constexpr uint32_t SizeU32 = 2, PrivateFlag = 128;
    if ((Flags & ~(SizeU32 | PrivateFlag)) || !(Flags & SizeU32)) return Invalid;
    Private = Flags & PrivateFlag;
    return 0;
  }

  // futex_waitv / futex_requeue entry: {u64 val, u64 uaddr, u32 flags, u32 reserved}.
  struct Futex2Entry {
    uint64_t Value, Address;
    uint32_t Flags, Reserved;
  };

  static int64_t ParseFutex2Entry(const Futex2Entry& Entry, bool& Private) {
    if (Entry.Reserved || Entry.Value > UINT32_MAX) return Invalid;
    return Futex2Flags(Entry.Flags, Private);
  }

  static bool Futex2ClockValid(uint64_t Clock) { return Clock == 0 || Clock == 1; }

  int64_t Futex2Wake(uint64_t Address, uint64_t Mask, uint64_t Count, uint64_t Flags) {
    bool Private {};
    if (Flags > UINT32_MAX || Futex2Flags(static_cast<uint32_t>(Flags), Private)) return Invalid;
    if (!Mask || Mask > UINT32_MAX) return Invalid;
    if (Address & 3) return Invalid;
    if (Available(Address, PF_R) < sizeof(uint32_t)) return Fault;
    return WakeKey(KeyFor(Address, Private), static_cast<int32_t>(Count), static_cast<uint32_t>(Mask));
  }

  int64_t Futex2Wait(uint64_t Address, uint64_t Value, uint64_t Mask, uint64_t Flags, uint64_t Timeout, uint64_t Clock) {
    bool Private {};
    if (Flags > UINT32_MAX || Futex2Flags(static_cast<uint32_t>(Flags), Private)) return Invalid;
    if (Value > UINT32_MAX || !Mask || Mask > UINT32_MAX) return Invalid;
    if (Timeout && !Futex2ClockValid(Clock)) return Invalid;
    if (Address & 3) return Invalid;
    if (Available(Address, PF_R) < sizeof(uint32_t)) return Fault;
    return FutexWait(Address, Private, static_cast<uint32_t>(Value), static_cast<uint32_t>(Mask), Timeout, true, Clock == 0);
  }

  int64_t FutexWaitv(uint64_t Waiters, uint64_t Count, uint64_t Flags, uint64_t Timeout, uint64_t Clock) {
    if (Flags || !Count || Count > 128) return Invalid;
    if (Timeout && !Futex2ClockValid(Clock)) return Invalid;
    uint64_t Deadline {};
    bool Expired {};
    if (const auto Error = FutexDeadline(Timeout, true, Clock == 0, Deadline, Expired)) return Error;
    if (AccessiblePrefix(Waiters, Count * sizeof(Futex2Entry), PF_R) < Count * sizeof(Futex2Entry)) return Fault;
    std::vector<Futex2Entry> Entries(Count);
    std::memcpy(Entries.data(), reinterpret_cast<const void*>(Waiters), Count * sizeof(Futex2Entry));
    std::vector<FutexKey> Keys;
    for (const auto& Entry : Entries) {
      bool Private {};
      if (const auto Error = ParseFutex2Entry(Entry, Private)) return Error;
      Keys.push_back(KeyFor(Entry.Address, Private));
    }
    for (const auto& Entry : Entries) {
      if (Entry.Address & 3) return Invalid;
      uint32_t Word {};
      if (!LoadWord(Entry.Address, Word)) return Fault;
      if (Word != Entry.Value) return -11;
    }
    if (Expired) return -110;
    BlockFutex(Keys.front(), UINT32_MAX, Deadline, false, GuestThread::FutexInterrupt::Eintr);
    Current->WaitKeys = std::move(Keys);
    return RetrySyscall;
  }

  int64_t Futex2Requeue(uint64_t Waiters, uint64_t Flags, uint64_t NrWake, uint64_t NrRequeue) {
    if (Flags) return Invalid;
    if (static_cast<int32_t>(NrWake) < 0 || static_cast<int32_t>(NrRequeue) < 0) return Invalid;
    Futex2Entry Entries[2];
    if (AccessiblePrefix(Waiters, sizeof(Entries), PF_R) < sizeof(Entries)) return Fault;
    std::memcpy(Entries, reinterpret_cast<const void*>(Waiters), sizeof(Entries));
    bool Private[2] {};
    for (int I = 0; I < 2; ++I) {
      if (const auto Error = ParseFutex2Entry(Entries[I], Private[I])) return Error;
      if (Entries[I].Address & 3) return Invalid;
      if (Available(Entries[I].Address, PF_R) < sizeof(uint32_t)) return Fault;
    }
    uint32_t Word {};
    LoadWord(Entries[0].Address, Word);
    if (Word != Entries[0].Value) return -11;
    return RequeueFutex(KeyFor(Entries[0].Address, Private[0]), KeyFor(Entries[1].Address, Private[1]), static_cast<int32_t>(NrWake),
                        static_cast<int32_t>(NrRequeue));
  }

  // ---- Robust lists ----
  int64_t SetRobustList(uint64_t Head, uint64_t Length) {
    if (Length != 24) return Invalid; // sizeof(struct robust_list_head)
    Current->RobustHead = Head;
    Current->RobustLength = Length;
    return 0;
  }

  int64_t GetRobustList(uint64_t Pid, uint64_t HeadOut, uint64_t LengthOut) {
    const GuestThread* Target = Current;
    if (Pid) {
      Target = nullptr;
      for (const auto& Thread : Threads) {
        if (Thread.Core && !Thread.Exited && Thread.TID == PidIn(Pid)) Target = &Thread;
      }
      if (!Target) return -3;
      if (Target->PID != Task->PID) {
        const auto* Owner = FindProcess(Target->PID);
        if (!Owner || !MayPtrace(*Owner)) return -1;
      }
    }
    if (Available(LengthOut, PF_W) < sizeof(uint64_t) || Available(HeadOut, PF_W) < sizeof(uint64_t)) return Fault;
    const uint64_t Length = 24, Head = Target->RobustHead;
    CopyOut(LengthOut, Length);
    CopyOut(HeadOut, Head);
    return 0;
  }

  // Linux exit_robust_list: marks every robust futex the thread still owns as
  // OWNER_DIED and wakes or hands off to a waiter. Reads are bounded and
  // checked against the page table, so a bad list ends the walk quietly.
  void RunRobustList(GuestThread& Thread) {
    const auto Head = std::exchange(Thread.RobustHead, 0);
    if (!Head) return;
    const auto Read = [this](uint64_t Address, uint64_t& Out) {
      if (Address > UINT64_MAX - 8 || Available(Address, PF_R) < sizeof(Out)) return false;
      std::memcpy(&Out, reinterpret_cast<const void*>(Address), sizeof(Out));
      return true;
    };
    for (const auto& Entry : WalkRobustList(Head, Read)) {
      if ((Entry.WordAddress & 3) || Available(Entry.WordAddress, PF_R | PF_W) < sizeof(uint32_t)) continue;
      uint32_t Word {};
      LoadWord(Entry.WordAddress, Word);
      bool Wake {};
      if (!FutexDeath(Word, OwnTid(Thread), Wake)) continue;
      const auto Key = KeyFor(Entry.WordAddress);
      if (Entry.Pi && Wake) HandOffPi(Key, Word);
      StoreWord(Entry.WordAddress, Word);
      if (!Entry.Pi && Wake) WakeKey(Key, 1, UINT32_MAX);
    }
  }

  // mm_release: robust futexes first, then clear_child_tid.
  void ReleaseThreadMemory(GuestThread& Thread) {
    RunRobustList(Thread);
    const uint32_t Zero {};
    if (Thread.ClearTid && Available(Thread.ClearTid, PF_W) >= sizeof(Zero)) {
      std::memcpy(reinterpret_cast<void*>(Thread.ClearTid), &Zero, sizeof(Zero));
      WakeFutex(Thread.ClearTid, 1, UINT32_MAX);
    }
    Thread.ClearTid = 0;
  }

  void ExitThread() {
    ReleaseThreadMemory(*Current);
    Current->Exited = true;
    Current->Descriptors.reset();
    Current->PendingFifo.reset();
    ++IOEpoch;
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

  // ---- System V IPC and POSIX message queues ----
  struct ShmSegment {
    IpcPerm Perm;
    uint64_t Size {};
    std::shared_ptr<void> Storage; // arena block; the segment has exactly one address
    int32_t Cpid {}, Lpid {};
    int64_t Atime {}, Dtime {}, Ctime {};
    bool Destroyed {}; // IPC_RMID: the key is gone, the segment dies with its last attachment
    bool Locked {};
  };
  struct SemSet {
    IpcPerm Perm;
    std::vector<uint16_t> Values;
    std::vector<uint32_t> Pids;
    int64_t Otime {}, Ctime {};
  };
  struct MsgQueue {
    IpcPerm Perm;
    std::deque<Switch2Guest::Message> Messages;
    uint64_t Bytes {}, Qbytes {Switch2Guest::MsgMnb};
    int32_t Lspid {}, Lrpid {};
    int64_t Stime {}, Rtime {}, Ctime {};
  };
  // A segment mapped into one process. Dead records (unmapped, or the process
  // exited) are dropped lazily; the live ones are the segment's nattch.
  struct ShmAttach {
    std::shared_ptr<void> Storage;
    uint64_t Address {};
    size_t Size {};
  };
  using SemUndoList = std::map<int32_t, std::vector<int16_t>>; // by semaphore set id
  // Per-user queue accounting shared by every queue, for RLIMIT_MSGQUEUE and
  // fs.mqueue.queues_max. Queues release their share when they are destroyed.
  struct MqueueBook {
    std::map<uint32_t, uint64_t> Bytes;
    size_t Queues {};
  };
  struct MqueueState {
    MqueueState(int64_t MaxMsg, int64_t MsgSize) : Queue {MaxMsg, MsgSize} {}
    ~MqueueState() {
      if (!Book) return;
      Book->Bytes[ChargedUid] -= Charged;
      --Book->Queues;
    }
    Switch2Guest::MessageQueue Queue;
    std::shared_ptr<MqueueBook> Book;
    uint32_t Uid {}, Gid {}, ChargedUid {};
    uint64_t Charged {}, Inode {}, Changes {};
    uint16_t Mode {};
    std::string Name;
    // mq_notify registration. Owner is the registering process, 0 when none.
    uint32_t NotifyOwner {}, NotifySignal {};
    int32_t NotifyKind {};
    uint64_t NotifyValue {};
  };

  struct InotifyState {
    Switch2Guest::InotifyInstance Instance;
  };

  struct MqueueEntry {
    uint64_t Inode;
    std::weak_ptr<MqueueState> State;
  };
  // What an IPC namespace owns: the System V tables and the POSIX message queue tree (/dev/mqueue).
  struct IpcStore {
    IpcTable<ShmSegment> ShmTable {Switch2Guest::ShmMni};
    IpcTable<SemSet> SemTable {Switch2Guest::SemMni};
    IpcTable<MsgQueue> MsgTable {Switch2Guest::MsgMni};
    std::map<std::string, std::shared_ptr<MqueueState>, std::less<>> MqueueNames;
    std::vector<MqueueEntry> AllMqueues; // named or unlinked-but-open
    std::shared_ptr<MqueueBook> MqueueAccounts {std::make_shared<MqueueBook>()};
  };
  static IpcStore& IpcOf(const GuestProcess& Process) { return *static_cast<IpcStore*>(Process.Ns.Ipc->Store.get()); }
  // The tables of the running process's IPC namespace.
  IpcStore& Ipc() const { return IpcOf(*Task); }

  // A guest AF_INET descriptor: the guest-visible state (GuestInet.h) and the host socket.
  // The host descriptor closes with the last reference to the open file description.
  struct InetSocket : Switch2Guest::Inet::State {
    int HostFd {-1};
    ~InetSocket() {
      if (HostFd >= 0) HostNet::Close(HostFd);
    }
  };

  struct OpenFile {
    ~OpenFile() {
      if (NotifyClose) ClosedFiles.push_back({Inode, Path, WriteAccess(StatusFlags), Directory.has_value()});
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
    std::string Contents; // the text of a /proc file, generated when it is opened
    std::string Path;
    Switch2Guest::Mounts::MountRef Mount; // the mount the file was opened in (null for files that belong to none)
    uint64_t Inode {};
    uint32_t StatusFlags {};
    bool PathOnly() const { return StatusFlags & PathOnlyFlag; }
    FileLock Lock {};
    uint8_t ReadAdvice {};
    std::optional<std::vector<DirectoryEntry>> Directory;
    std::shared_ptr<StreamSocket> Socket;
    std::shared_ptr<InetSocket> Inet; // AF_INET; Socket stays empty
    std::shared_ptr<FifoState> Fifo;
    // Devices and /proc files keep their guest name in Path; this is the path the open resolved (through
    // mounts), which /proc/<pid>/fd/<n> prints.
    std::string LinkPath;
    uint32_t PidfdTarget {}; // pidfd_open / CLONE_PIDFD: the process it refers to.
    bool FifoPartnerSeen {};
    uint64_t InitialWriterGeneration {};
    StreamSocket& WriteEndpoint() const { return *(Fifo ? Fifo->Writer : Socket); }
    bool Nonblocking() const { return StatusFlags & 04000; }
    std::shared_ptr<GuestMemoryFile> Memory;
    // Descriptors on Linux's shared anonymous inode (anon_inode:[name]).
    const char* AnonName {};
    std::shared_ptr<TimerFdState> TimerFd;
    std::shared_ptr<Switch2Guest::EventCounter> Event;
    std::optional<uint64_t> SignalFdMask; // signalfd: the signals it reads
    std::shared_ptr<EpollState> Epoll;
    std::shared_ptr<InotifyState> Inotify;
    std::shared_ptr<MqueueState> Mqueue; // mq_open descriptor
    bool NotifyClose {}; // an open regular file or directory: closing it is an inotify event
    // An nsfs file (/proc/<pid>/ns/<name> or an NS_GET_* result): the namespace it names.
    std::shared_ptr<Switch2Guest::Ns::Namespace> NsObject;
    // An open /proc/<pid>/uid_map (0), gid_map (1) or setgroups (2) for writing: the process it belongs to and the
    // credentials the file was opened with (file_ns_capable uses those, not the writer's).
    struct ProcWriter {
      uint32_t PID;
      int Kind;
      Switch2Guest::Ns::Subject Opener;
    };
    std::optional<ProcWriter> ProcWrite;
  };
  // ================= System V IPC (shm, sem, msg) =================

  IpcCred IpcCredentials() const {
    return {Task->Creds.Uid[1], Task->Creds.Gid[1], Task->SupplementaryGroups, NsCapableOver(*Task->Ns.Ipc->Owner, CapIpcOwner)};
  }

  int64_t NowSeconds() const { return static_cast<int64_t>(RealTimeNs() / 1'000'000'000); }

  template<typename T>
  bool ReadStruct(uint64_t Address, T& Out) const {
    if (AccessiblePrefix(Address, sizeof(T), PF_R) < sizeof(T)) return false;
    std::memcpy(&Out, reinterpret_cast<const void*>(Address), sizeof(T));
    return true;
  }

  template<typename T>
  bool WriteStruct(uint64_t Address, const T& Value) {
    if (AccessiblePrefix(Address, sizeof(T), PF_W) < sizeof(T)) return false;
    std::memcpy(reinterpret_cast<void*>(Address), &Value, sizeof(T));
    return true;
  }

  // ipcctl_obtain_check: the owner, the creator or CAP_SYS_ADMIN.
  bool IpcOwner(const IpcPerm& Perm) const {
    return Task->Creds.Uid[1] == Perm.Uid || Task->Creds.Uid[1] == Perm.Cuid || NsCapableOver(*Task->Ns.Ipc->Owner, CapSysAdmin);
  }

  // A blocking IPC call parks the thread until IOEpoch changes and reruns.
  // Removed is the EIDRM check of that rerun: the object of the earlier call
  // is gone. Returns the first-call or rerun flag and clears the marker.
  bool IpcRerun(uint8_t Kind, int32_t Id) {
    const bool Rerun = Current->IpcBlocked && Current->IpcWaitKind == Kind && Current->IpcWaitId == Id;
    Current->IpcBlocked = false;
    return Rerun;
  }

  int64_t BlockIpc(uint8_t Kind, int32_t Id) {
    Current->IpcBlocked = true;
    Current->IpcWaitKind = Kind;
    Current->IpcWaitId = Id;
    return WaitForIO();
  }

  // ---- shared memory ----

  size_t ShmAttachCount(const ShmSegment& Segment) const {
    size_t Count {};
    for (const auto& [PID, Process] : Processes) {
      if (Process.Exited) continue;
      for (const auto& Attach : Process.ShmAttached) {
        if (Attach.Storage == Segment.Storage && Process.Memory.AnyMapped(Attach.Address, Attach.Size)) ++Count;
      }
    }
    return Count;
  }

  // Frees destroyed segments whose last mapping is gone (exit, exec, munmap).
  void ReapShm() {
    std::vector<int32_t> Dead;
    for (const auto& [Index, Segment] : Ipc().ShmTable.All()) {
      if (Segment.Destroyed && !ShmAttachCount(Segment)) Dead.push_back(Segment.Perm.Seq * Switch2Guest::IpcMni + Index);
    }
    for (const auto Id : Dead) Ipc().ShmTable.Remove(Id);
  }

  static uint64_t PagesOf(uint64_t Size) { return Size / GuestPageSize + (Size % GuestPageSize != 0); }

  int64_t ShmGet(uint64_t KeyArgument, uint64_t Size, uint64_t Flags) {
    ReapShm();
    return Ipc().ShmTable.Get(static_cast<int32_t>(KeyArgument), static_cast<int32_t>(Flags), IpcCredentials(),
      [&](ShmSegment& New) -> int64_t {
        if (Size < Switch2Guest::ShmMin || Size > Switch2Guest::ShmMax) return Invalid;
        uint64_t Used {};
        for (const auto& [Index, Segment] : Ipc().ShmTable.All()) Used += PagesOf(Segment.Size);
        if (Used > Switch2Guest::ShmMax - PagesOf(Size)) return -28; // SHMALL
        if (Flags & 04000) { // SHM_HUGETLB: no huge page pool
          return Task->Creds.Capable(CapIpcLock) || InGuestGroup(0) ? -12 : -1;
        }
        if (Size > Task->Memory.MaxMapping()) return -12;
        New.Storage = AllocateArenaBlock(PagesOf(Size) * GuestPageSize);
        if (!New.Storage) return -12;
        New.Size = Size;
        New.Cpid = Task->PID;
        New.Ctime = NowSeconds();
        return 0;
      },
      [&](const ShmSegment& Old) -> int64_t { return Old.Size < Size ? Invalid : 0; });
  }

  int64_t ShmAttachGuest(uint64_t IdArgument, uint64_t Address, uint64_t Flags) {
    constexpr uint64_t ReadOnly = 010000, Round = 020000, Remap = 040000, Execute = 0100000;
    ReapShm();
    // SHMLBA is one page on x86-64.
    const bool Fixed = Address != 0;
    if (Fixed) {
      if (Address & (GuestPageSize - 1)) {
        if (!(Flags & Round)) return Invalid;
        Address &= ~(GuestPageSize - 1);
      }
      if (!Address) return Invalid;
    } else if (Flags & Remap) {
      return Invalid;
    }
    auto* Segment = Ipc().ShmTable.Find(static_cast<int32_t>(IdArgument));
    if (!Segment) return Invalid;
    const uint16_t Wanted = (Flags & ReadOnly ? 4 : 6) | (Flags & Execute ? 1 : 0);
    if (!IpcAllowed(Segment->Perm, IpcCredentials(), Wanted)) return -13;
    // One address per segment: the SDK cannot map a physical page twice.
    const auto Base = reinterpret_cast<uint64_t>(Segment->Storage.get());
    if (Fixed && Address != Base) return Invalid;
    for (const auto& Attach : Task->ShmAttached) {
      if (Attach.Storage == Segment->Storage && Task->Memory.AnyMapped(Attach.Address, Attach.Size)) return -12;
    }
    const size_t Rounded = PagesOf(Segment->Size) * GuestPageSize;
    const uint8_t Access = PF_R | (Flags & ReadOnly ? 0 : PF_W) | (Flags & Execute ? PF_X : 0);
    if (!Task->Memory.RegisterSharedFile(Base, Rounded, Access, Segment->Storage, !(Flags & ReadOnly))) return -12;
    std::erase_if(Task->ShmAttached, [&](const ShmAttach& Old) { return !Task->Memory.AnyMapped(Old.Address, Old.Size); });
    Task->ShmAttached.push_back({Segment->Storage, Base, Rounded});
    Segment->Atime = NowSeconds();
    Segment->Lpid = Task->PID;
    return Base;
  }

  int64_t ShmDetachGuest(FEXCore::Core::CpuStateFrame* Frame, uint64_t Address) {
    auto Found = std::find_if(Task->ShmAttached.begin(), Task->ShmAttached.end(), [&](const ShmAttach& Attach) {
      return Attach.Address == Address && Task->Memory.AnyMapped(Attach.Address, Attach.Size);
    });
    if (Found == Task->ShmAttached.end()) return Invalid;
    const auto Storage = Found->Storage;
    const auto Size = Found->Size;
    if (const auto Error = UnmapGuest(Frame, Address, Size); Error < 0) return Error;
    Task->ShmAttached.erase(Found);
    for (auto& [Index, Segment] : Ipc().ShmTable.All()) {
      if (Segment.Storage != Storage) continue;
      Segment.Dtime = NowSeconds();
      Segment.Lpid = Task->PID;
    }
    ReapShm();
    return 0;
  }

  LinuxIpc64Perm ShmPermOut(const ShmSegment& Segment) const {
    return Switch2Guest::ToLinux(Segment.Perm, (Segment.Destroyed ? 01000 : 0) | (Segment.Locked ? 02000 : 0));
  }

  int64_t ShmControl(uint64_t IdArgument, uint64_t Command, uint64_t Buffer) {
    constexpr int IpcRmid = 0, IpcSet = 1, IpcStat = 2, IpcInfo = 3, ShmLock = 11, ShmUnlock = 12, ShmStat = 13, ShmInfo = 14,
                  ShmStatAny = 15;
    ReapShm();
    const auto Id = static_cast<int32_t>(IdArgument);
    const int Cmd = static_cast<int32_t>(Command) & ~0x100;
    const auto MaxIndex = [&] { return static_cast<int64_t>(std::max(0, Ipc().ShmTable.MaxIndex())); };
    switch (Cmd) {
    case IpcInfo: {
      if (Id < 0) return Invalid;
      const Switch2Guest::LinuxShminfo Info {Switch2Guest::ShmMax, Switch2Guest::ShmMin, Switch2Guest::ShmMni, Switch2Guest::ShmSeg,
                                              Switch2Guest::ShmMax, {}};
      if (!WriteStruct(Buffer, Info)) return Fault;
      return MaxIndex();
    }
    case ShmInfo: {
      if (Id < 0) return Invalid;
      uint64_t Pages {};
      for (const auto& [Index, Segment] : Ipc().ShmTable.All()) Pages += PagesOf(Segment.Size);
      const Switch2Guest::LinuxShmInfo Info {static_cast<int32_t>(Ipc().ShmTable.Count()), 0, Pages, Pages, 0, 0, 0};
      if (!WriteStruct(Buffer, Info)) return Fault;
      return MaxIndex();
    }
    case IpcStat: case ShmStat: case ShmStatAny: {
      auto* Segment = Cmd == IpcStat ? Ipc().ShmTable.Find(Id) : Ipc().ShmTable.FindIndex(Id);
      if (!Segment) return Invalid;
      if (Cmd != ShmStatAny && !IpcAllowed(Segment->Perm, IpcCredentials(), 4)) return -13;
      Switch2Guest::LinuxShmidDs Out {};
      Out.Perm = ShmPermOut(*Segment);
      Out.SegSz = Segment->Size;
      Out.Atime = Segment->Atime;
      Out.Dtime = Segment->Dtime;
      Out.Ctime = Segment->Ctime;
      Out.Cpid = Segment->Cpid;
      Out.Lpid = Segment->Lpid;
      Out.Nattch = ShmAttachCount(*Segment);
      if (!WriteStruct(Buffer, Out)) return Fault;
      return Cmd == IpcStat ? 0 : Segment->Perm.Seq * Switch2Guest::IpcMni + Id;
    }
    case IpcSet: {
      Switch2Guest::LinuxShmidDs In {};
      if (!ReadStruct(Buffer, In)) return Fault;
      auto* Segment = Ipc().ShmTable.Find(Id);
      if (!Segment) return Invalid;
      if (!IpcOwner(Segment->Perm)) return -1;
      Segment->Perm.Uid = In.Perm.Uid;
      Segment->Perm.Gid = In.Perm.Gid;
      Segment->Perm.Mode = (Segment->Perm.Mode & ~0777) | (In.Perm.Mode & 0777);
      Segment->Ctime = NowSeconds();
      return 0;
    }
    case IpcRmid: {
      auto* Segment = Ipc().ShmTable.Find(Id);
      if (!Segment) return Invalid;
      if (!IpcOwner(Segment->Perm)) return -1;
      Segment->Destroyed = true;
      Segment->Perm.Key = Switch2Guest::IpcPrivate;
      ReapShm();
      return 0;
    }
    case ShmLock: case ShmUnlock: {
      auto* Segment = Ipc().ShmTable.Find(Id);
      if (!Segment) return Invalid;
      if (!Task->Creds.Capable(CapIpcLock)) {
        const auto Euid = Task->Creds.Uid[1];
        if (Euid != Segment->Perm.Uid && Euid != Segment->Perm.Cuid) return -1;
        if (Cmd == ShmLock && !Task->Limits[ResourceMemlock][0]) return -1;
      }
      if (Cmd == ShmLock && !Segment->Locked) {
        if (!Task->Creds.Capable(CapIpcLock)) {
          uint64_t Locked {};
          for (const auto& [Index, Other] : Ipc().ShmTable.All()) {
            if (Other.Locked && Other.Perm.Uid == Segment->Perm.Uid) Locked += PagesOf(Other.Size) * GuestPageSize;
          }
          if (Locked + PagesOf(Segment->Size) * GuestPageSize > Task->Limits[ResourceMemlock][0]) return -12;
        }
        Segment->Locked = true;
      } else if (Cmd == ShmUnlock) {
        Segment->Locked = false;
      }
      return 0;
    }
    default: return Invalid;
    }
  }

  // ---- semaphores ----

  int64_t SemGet(uint64_t KeyArgument, uint64_t Count, uint64_t Flags) {
    const auto Number = static_cast<int32_t>(Count);
    // ksys_semget checks nsems before any lookup.
    if (Number < 0 || Number > Switch2Guest::SemMsl) return Invalid;
    return Ipc().SemTable.Get(static_cast<int32_t>(KeyArgument), static_cast<int32_t>(Flags), IpcCredentials(),
      [&](SemSet& New) -> int64_t {
        if (Number <= 0 || Number > Switch2Guest::SemMsl) return Invalid;
        int64_t Total {};
        for (const auto& [Index, Set] : Ipc().SemTable.All()) Total += Set.Values.size();
        if (Total + Number > Switch2Guest::SemMns) return -28;
        New.Values.assign(Number, 0);
        New.Pids.assign(Number, 0);
        New.Ctime = NowSeconds();
        return 0;
      },
      [&](const SemSet& Old) -> int64_t { return Number > static_cast<int64_t>(Old.Values.size()) ? Invalid : 0; });
  }

  // The first op of a call that cannot go ahead, replayed on a copy of the
  // values: the semaphore a waiter is counted on (GETNCNT / GETZCNT).
  static void FindBlockingOp(std::vector<uint16_t> Values, std::span<const SemOp> Ops, uint16_t& Number, bool& ForZero) {
    for (const auto& Op : Ops) {
      int32_t Next = Values[Op.Num];
      const bool Zero = !Op.Op;
      if ((Zero && Next) || Next + Op.Op < 0) {
        Number = Op.Num;
        ForZero = Zero;
        return;
      }
      Values[Op.Num] = static_cast<uint16_t>(Next + Op.Op);
    }
  }

  int64_t SemOperate(uint64_t IdArgument, uint64_t OpsAddress, uint64_t Count, uint64_t Timeout) {
    const auto Id = static_cast<int32_t>(IdArgument);
    const bool Rerun = IpcRerun(1, Id);
    if (Count < 1 || Id < 0) return Invalid;
    if (Count > static_cast<uint64_t>(Switch2Guest::SemOpm)) return -7;
    if (AccessiblePrefix(OpsAddress, Count * sizeof(Switch2Guest::LinuxSembuf), PF_R) < Count * sizeof(Switch2Guest::LinuxSembuf)) return Fault;
    std::vector<SemOp> Ops(Count);
    for (size_t Index = 0; Index < Count; ++Index) {
      Switch2Guest::LinuxSembuf Raw {};
      std::memcpy(&Raw, reinterpret_cast<const void*>(OpsAddress + Index * sizeof(Raw)), sizeof(Raw));
      Ops[Index] = {Raw.Num, Raw.Op, Raw.Flags};
    }
    uint64_t Relative {};
    if (Timeout) {
      if (Available(Timeout, PF_R) < 2 * sizeof(int64_t)) return Fault;
      if (!ReadTimespec(Timeout, Relative)) return Invalid;
    }
    auto* Set = Ipc().SemTable.Find(Id);
    if (!Set) return Rerun ? -43 : Invalid; // EIDRM for a waiter whose set was removed
    const bool Alter = std::any_of(Ops.begin(), Ops.end(), [](const SemOp& Op) { return Op.Op != 0; });
    const bool Undoes = std::any_of(Ops.begin(), Ops.end(), [](const SemOp& Op) { return Op.Flags & Switch2Guest::SemUndo; });
    for (const auto& Op : Ops) {
      if (Op.Num >= Set->Values.size()) return -27; // EFBIG, before the permission check
    }
    if (!IpcAllowed(Set->Perm, IpcCredentials(), Alter ? 2 : 4)) return -13;
    std::vector<int16_t> Scratch;
    auto& Undo = Undoes ? (*Task->SemUndo)[Id] : Scratch;
    const auto Result = Switch2Guest::TrySemOps(Set->Values, Ops, Undo, Task->PID, Set->Pids);
    if (Result < 0) return Result;
    if (!Result) {
      if (Alter) Set->Otime = NowSeconds();
      ++IOEpoch; // waiters on other semaphores of the set may now proceed
      return 0;
    }
    if (Timeout) {
      const auto Now = MonotonicNs();
      if (!Current->Deadline) Current->Deadline = AddDeadline(Now, Relative);
      if (Current->Deadline <= Now) return -11; // Linux semtimedop: EAGAIN on expiry
    }
    FindBlockingOp(Set->Values, Ops, Current->SemWaitNum, Current->SemWaitZero);
    return BlockIpc(1, Id);
  }

  size_t SemWaiters(int32_t Id, uint16_t Number, bool ForZero) const {
    size_t Count {};
    for (const auto& Thread : Threads) {
      if (Thread.Core && !Thread.Exited && Thread.IpcBlocked && Thread.IpcWaitKind == 1 && Thread.IpcWaitId == Id &&
          Thread.SemWaitNum == Number && Thread.SemWaitZero == ForZero) ++Count;
    }
    return Count;
  }

  // SETVAL and SETALL cancel the adjustments for the semaphores they set, in every process.
  void ClearSemUndo(int32_t Id, std::optional<size_t> Number) {
    for (auto& [PID, Process] : Processes) {
      const auto Entry = Process.SemUndo->find(Id);
      if (Entry == Process.SemUndo->end()) continue;
      if (!Number) Entry->second.assign(Entry->second.size(), 0);
      else if (*Number < Entry->second.size()) Entry->second[*Number] = 0;
    }
  }

  int64_t SemControl(uint64_t IdArgument, uint64_t Number, uint64_t Command, uint64_t Argument) {
    constexpr int IpcRmid = 0, IpcSet = 1, IpcStat = 2, IpcInfo = 3, GetPid = 11, GetVal = 12, GetAll = 13, GetNcnt = 14, GetZcnt = 15,
                  SetVal = 16, SetAll = 17, SemStat = 18, SemInfo = 19, SemStatAny = 20;
    const auto Id = static_cast<int32_t>(IdArgument);
    const int Cmd = static_cast<int32_t>(Command) & ~0x100;
    const auto Num = static_cast<int32_t>(Number);
    const auto MaxIndex = [&] { return static_cast<int64_t>(std::max(0, Ipc().SemTable.MaxIndex())); };
    if (Id < 0) return Invalid;
    switch (Cmd) {
    case IpcInfo: case SemInfo: {
      Switch2Guest::LinuxSeminfo Info {Switch2Guest::SemMns, Switch2Guest::SemMni, Switch2Guest::SemMns, Switch2Guest::SemMns,
                                        Switch2Guest::SemMsl, Switch2Guest::SemOpm, Switch2Guest::SemOpm, Switch2Guest::SemUsz,
                                        Switch2Guest::SemVmx, Switch2Guest::SemAem};
      if (Cmd == SemInfo) {
        int32_t Total {};
        for (const auto& [Index, Set] : Ipc().SemTable.All()) Total += static_cast<int32_t>(Set.Values.size());
        Info.Semusz = static_cast<int32_t>(Ipc().SemTable.Count());
        Info.Semaem = Total;
      }
      if (!WriteStruct(Argument, Info)) return Fault;
      return MaxIndex();
    }
    case IpcStat: case SemStat: case SemStatAny: {
      auto* Set = Cmd == IpcStat ? Ipc().SemTable.Find(Id) : Ipc().SemTable.FindIndex(Id);
      if (!Set) return Invalid;
      if (Cmd != SemStatAny && !IpcAllowed(Set->Perm, IpcCredentials(), 4)) return -13;
      Switch2Guest::LinuxSemidDs Out {};
      Out.Perm = Switch2Guest::ToLinux(Set->Perm);
      Out.Otime = Set->Otime;
      Out.Ctime = Set->Ctime;
      Out.Nsems = Set->Values.size();
      if (!WriteStruct(Argument, Out)) return Fault;
      return Cmd == IpcStat ? 0 : Set->Perm.Seq * Switch2Guest::IpcMni + Id;
    }
    case IpcSet: {
      Switch2Guest::LinuxSemidDs In {};
      if (!ReadStruct(Argument, In)) return Fault;
      auto* Set = Ipc().SemTable.Find(Id);
      if (!Set) return Invalid;
      if (!IpcOwner(Set->Perm)) return -1;
      Set->Perm.Uid = In.Perm.Uid;
      Set->Perm.Gid = In.Perm.Gid;
      Set->Perm.Mode = (Set->Perm.Mode & ~0777) | (In.Perm.Mode & 0777);
      Set->Ctime = NowSeconds();
      return 0;
    }
    case IpcRmid: {
      auto* Set = Ipc().SemTable.Find(Id);
      if (!Set) return Invalid;
      if (!IpcOwner(Set->Perm)) return -1;
      for (auto& [PID, Process] : Processes) Process.SemUndo->erase(Id);
      Ipc().SemTable.Remove(Id);
      ++IOEpoch; // blocked callers fail with EIDRM
      return 0;
    }
    default: break;
    }
    if (Cmd == SetVal) {
      // semctl_setval: semnum < 0 and the value range come before the lookup.
      const auto Value = static_cast<int32_t>(Argument);
      if (Num < 0) return Invalid;
      if (Value < 0 || Value > Switch2Guest::SemVmx) return -34;
    }
    auto* Set = Ipc().SemTable.Find(Id);
    if (!Set) return Invalid;
    switch (Cmd) {
    case GetAll: {
      if (!IpcAllowed(Set->Perm, IpcCredentials(), 4)) return -13;
      const auto Bytes = Set->Values.size() * sizeof(uint16_t);
      if (AccessiblePrefix(Argument, Bytes, PF_W) < Bytes) return Fault;
      std::memcpy(reinterpret_cast<void*>(Argument), Set->Values.data(), Bytes);
      return 0;
    }
    case SetAll: {
      if (!IpcAllowed(Set->Perm, IpcCredentials(), 2)) return -13;
      const auto Bytes = Set->Values.size() * sizeof(uint16_t);
      if (AccessiblePrefix(Argument, Bytes, PF_R) < Bytes) return Fault;
      std::vector<uint16_t> Values(Set->Values.size());
      std::memcpy(Values.data(), reinterpret_cast<const void*>(Argument), Bytes);
      if (std::any_of(Values.begin(), Values.end(), [](uint16_t Value) { return Value > Switch2Guest::SemVmx; })) return -34;
      Set->Values = std::move(Values);
      std::fill(Set->Pids.begin(), Set->Pids.end(), Task->PID);
      ClearSemUndo(Id, std::nullopt);
      Set->Ctime = NowSeconds();
      ++IOEpoch;
      return 0;
    }
    default: break;
    }
    if (Num < 0 || static_cast<size_t>(Num) >= Set->Values.size()) return Invalid;
    const auto Write = Cmd == SetVal;
    switch (Cmd) {
    case GetPid: case GetVal: case GetNcnt: case GetZcnt: case SetVal:
      if (!IpcAllowed(Set->Perm, IpcCredentials(), Write ? 2 : 4)) return -13;
      break;
    default: return Invalid;
    }
    switch (Cmd) {
    case GetPid: return Set->Pids[Num];
    case GetVal: return Set->Values[Num];
    case GetNcnt: return SemWaiters(Id, Num, false);
    case GetZcnt: return SemWaiters(Id, Num, true);
    default: break;
    }
    const auto Value = static_cast<int32_t>(Argument);
    Set->Values[Num] = static_cast<uint16_t>(Value);
    Set->Pids[Num] = Task->PID;
    ClearSemUndo(Id, static_cast<size_t>(Num));
    Set->Ctime = NowSeconds();
    ++IOEpoch;
    return 0;
  }

  // ---- message queues ----

  int64_t MsgGet(uint64_t KeyArgument, uint64_t Flags) {
    return Ipc().MsgTable.Get(static_cast<int32_t>(KeyArgument), static_cast<int32_t>(Flags), IpcCredentials(),
      [&](MsgQueue& New) -> int64_t {
        New.Ctime = NowSeconds();
        return 0;
      },
      [](const MsgQueue&) -> int64_t { return 0; });
  }

  int64_t MsgSend(uint64_t IdArgument, uint64_t Address, uint64_t Size, uint64_t Flags) {
    const auto Id = static_cast<int32_t>(IdArgument);
    const bool Rerun = IpcRerun(2, Id);
    if (Size > static_cast<uint64_t>(Switch2Guest::MsgMax) || static_cast<int64_t>(Size) < 0 || Id < 0) return Invalid;
    int64_t Type {};
    if (!ReadStruct(Address, Type)) return Fault;
    if (Type < 1) return Invalid;
    if (AccessiblePrefix(Address + sizeof(Type), Size, PF_R) < Size) return Fault;
    auto* Queue = Ipc().MsgTable.Find(Id);
    if (!Queue) return Rerun ? -43 : Invalid;
    if (!IpcAllowed(Queue->Perm, IpcCredentials(), 2)) return -13;
    // msg_fits_inqueue: the bytes and the message count both stay within msg_qbytes.
    if (Size + Queue->Bytes > Queue->Qbytes || 1 + Queue->Messages.size() > Queue->Qbytes) {
      if (Flags & Switch2Guest::IpcNowait) return -11;
      return BlockIpc(2, Id);
    }
    Switch2Guest::Message Entry {Type, std::vector<uint8_t>(Size)};
    if (Size) std::memcpy(Entry.Text.data(), reinterpret_cast<const void*>(Address + sizeof(Type)), Size);
    Queue->Messages.push_back(std::move(Entry));
    Queue->Bytes += Size;
    Queue->Lspid = Task->PID;
    Queue->Stime = NowSeconds();
    ++IOEpoch;
    return 0;
  }

  int64_t MsgReceive(uint64_t IdArgument, uint64_t Address, uint64_t Size, int64_t Type, uint64_t Flags) {
    constexpr uint64_t NoError = 010000, Except = 020000, Copy = 040000;
    const auto Id = static_cast<int32_t>(IdArgument);
    const bool Rerun = IpcRerun(3, Id);
    if (Id < 0 || static_cast<int64_t>(Size) < 0) return Invalid;
    if ((Flags & Copy) && ((Flags & Except) || !(Flags & Switch2Guest::IpcNowait))) return Invalid;
    auto* Queue = Ipc().MsgTable.Find(Id);
    if (!Queue) return Rerun ? -43 : Invalid;
    if (!IpcAllowed(Queue->Perm, IpcCredentials(), 4)) return -13;
    std::optional<size_t> Chosen;
    if (Flags & Copy) {
      // MSG_COPY: msgtyp is the index of the message to copy.
      if (Type >= 0 && static_cast<uint64_t>(Type) < Queue->Messages.size()) Chosen = static_cast<size_t>(Type);
    } else {
      Chosen = Switch2Guest::SelectMessage(Queue->Messages, Type, Flags & Except);
    }
    if (!Chosen) {
      if (Flags & Switch2Guest::IpcNowait) return -42; // ENOMSG
      return BlockIpc(3, Id);
    }
    const auto& Entry = Queue->Messages[*Chosen];
    if (Size < Entry.Text.size() && !(Flags & NoError)) return -7; // E2BIG; the message stays queued
    const auto Copied = std::min<uint64_t>(Size, Entry.Text.size());
    // do_msgrcv unlinks the message before store_msg: on EFAULT it is lost.
    const bool Writable = AccessiblePrefix(Address, sizeof(int64_t) + Copied, PF_W) >= sizeof(int64_t) + Copied;
    if (Writable) {
      std::memcpy(reinterpret_cast<void*>(Address), &Entry.Type, sizeof(int64_t));
      if (Copied) std::memcpy(reinterpret_cast<void*>(Address + sizeof(int64_t)), Entry.Text.data(), Copied);
      InvalidateExecutableStores(Address, sizeof(int64_t) + Copied);
    }
    if (!(Flags & Copy)) {
      Queue->Bytes -= Entry.Text.size();
      Queue->Messages.erase(Queue->Messages.begin() + *Chosen);
      Queue->Lrpid = Task->PID;
      Queue->Rtime = NowSeconds();
      ++IOEpoch;
    }
    return Writable ? static_cast<int64_t>(Copied) : Fault;
  }

  int64_t MsgControl(uint64_t IdArgument, uint64_t Command, uint64_t Buffer) {
    constexpr int IpcRmid = 0, IpcSet = 1, IpcStat = 2, IpcInfo = 3, MsgStat = 11, MsgInfo = 12, MsgStatAny = 13;
    const auto Id = static_cast<int32_t>(IdArgument);
    const int Cmd = static_cast<int32_t>(Command) & ~0x100;
    const auto MaxIndex = [&] { return static_cast<int64_t>(std::max(0, Ipc().MsgTable.MaxIndex())); };
    switch (Cmd) {
    case IpcInfo: case MsgInfo: {
      if (Id < 0) return Invalid;
      Switch2Guest::LinuxMsginfo Info {Switch2Guest::MsgPool, Switch2Guest::MsgMnb, Switch2Guest::MsgMax, Switch2Guest::MsgMnb,
                                        Switch2Guest::MsgMni, Switch2Guest::MsgSsz, Switch2Guest::MsgMnb,
                                        static_cast<uint16_t>(Switch2Guest::MsgSeg), 0};
      if (Cmd == MsgInfo) {
        int32_t Messages {}, Bytes {};
        for (const auto& [Index, Queue] : Ipc().MsgTable.All()) {
          Messages += static_cast<int32_t>(Queue.Messages.size());
          Bytes += static_cast<int32_t>(Queue.Bytes);
        }
        Info.Msgpool = static_cast<int32_t>(Ipc().MsgTable.Count());
        Info.Msgmap = Messages;
        Info.Msgtql = Bytes;
      }
      if (!WriteStruct(Buffer, Info)) return Fault;
      return MaxIndex();
    }
    case IpcStat: case MsgStat: case MsgStatAny: {
      if (Id < 0) return Invalid;
      auto* Queue = Cmd == IpcStat ? Ipc().MsgTable.Find(Id) : Ipc().MsgTable.FindIndex(Id);
      if (!Queue) return Invalid;
      if (Cmd != MsgStatAny && !IpcAllowed(Queue->Perm, IpcCredentials(), 4)) return -13;
      Switch2Guest::LinuxMsqidDs Out {};
      Out.Perm = Switch2Guest::ToLinux(Queue->Perm);
      Out.Stime = Queue->Stime;
      Out.Rtime = Queue->Rtime;
      Out.Ctime = Queue->Ctime;
      Out.Cbytes = Queue->Bytes;
      Out.Qnum = Queue->Messages.size();
      Out.Qbytes = Queue->Qbytes;
      Out.Lspid = Queue->Lspid;
      Out.Lrpid = Queue->Lrpid;
      if (!WriteStruct(Buffer, Out)) return Fault;
      return Cmd == IpcStat ? 0 : Queue->Perm.Seq * Switch2Guest::IpcMni + Id;
    }
    case IpcSet: {
      if (Id < 0) return Invalid;
      Switch2Guest::LinuxMsqidDs In {};
      if (!ReadStruct(Buffer, In)) return Fault;
      auto* Queue = Ipc().MsgTable.Find(Id);
      if (!Queue) return Invalid;
      if (!IpcOwner(Queue->Perm)) return -1;
      if (In.Qbytes > static_cast<uint64_t>(Switch2Guest::MsgMnb) && !Task->Creds.Capable(CapSysResource)) return -1;
      Queue->Qbytes = In.Qbytes;
      Queue->Perm.Uid = In.Perm.Uid;
      Queue->Perm.Gid = In.Perm.Gid;
      Queue->Perm.Mode = (Queue->Perm.Mode & ~0777) | (In.Perm.Mode & 0777);
      Queue->Ctime = NowSeconds();
      ++IOEpoch; // a larger limit may release blocked senders
      return 0;
    }
    case IpcRmid: {
      if (Id < 0) return Invalid;
      auto* Queue = Ipc().MsgTable.Find(Id);
      if (!Queue) return Invalid;
      if (!IpcOwner(Queue->Perm)) return -1;
      Ipc().MsgTable.Remove(Id);
      ++IOEpoch; // blocked senders and receivers fail with EIDRM
      return 0;
    }
    default: return Invalid;
    }
  }

  // exit_sem: applies the process's SEM_UNDO adjustments (when it is the last sharer of the list) to the
  // semaphore sets of its IPC namespace and starts a fresh list. Also run when the process leaves the namespace.
  void ExitSem(GuestProcess& Process) {
    if (Process.SemUndo.use_count() == 1) {
      for (const auto& [Id, Delta] : *Process.SemUndo) {
        if (auto* Set = IpcOf(Process).SemTable.Find(Id)) {
          Switch2Guest::ApplySemUndo(Set->Values, Delta);
          Set->Otime = NowSeconds();
        }
      }
    }
    Process.SemUndo = std::make_shared<SemUndoList>();
    ++IOEpoch;
  }

  // exit_sem and shmdt of a dying process; also drops its mq_notify registrations.
  void ExitSysvIpc(GuestProcess& Process) {
    ExitSem(Process);
    Process.ShmAttached.clear();
    for (const auto& Entry : IpcOf(Process).AllMqueues) {
      if (const auto Queue = Entry.State.lock(); Queue && Queue->NotifyOwner == Process.PID) Queue->NotifyOwner = 0;
    }
    PruneMqueues();
    ++IOEpoch;
  }

  // ================= POSIX message queues =================

  // Forgets queues that are gone (unlinked and closed), with their inode records.
  void PruneMqueues() {
    std::erase_if(Ipc().AllMqueues, [&](const auto& Entry) {
      if (!Entry.State.expired()) return false;
      Inodes.erase(Entry.Inode);
      return true;
    });
  }

  static constexpr int32_t SiMesgq = -3;

  OpenFile* MqueueFile(uint64_t FD) {
    auto* File = GetFile(FD);
    return File && File->Mqueue ? File : nullptr;
  }

  // mq_open. glibc has already removed the leading '/' from the name.
  int64_t MqueueOpen(uint64_t NameAddress, uint64_t OpenFlags, uint64_t Mode, uint64_t AttributeAddress) {
    constexpr uint64_t Create = 0100, Exclusive = 0200;
    const auto Name = GuestString(NameAddress, Switch2Guest::MqNameMax + 1, -36);
    if (!Name) return Name.Error;
    if ((OpenFlags & 3) == 3) return Invalid;
    if (Name->empty()) return -2;
    if (Name->find('/') != std::string_view::npos || *Name == "." || *Name == "..") return -13;
    MqAttr Attr {0, Switch2Guest::MqDefaultMaxMsg, Switch2Guest::MqDefaultMsgSize, 0};
    bool HasAttr {};
    if ((OpenFlags & Create) && AttributeAddress) {
      std::array<int64_t, 8> Raw {};
      if (!ReadStruct(AttributeAddress, Raw)) return Fault;
      Attr = {Raw[0], Raw[1], Raw[2], Raw[3]};
      HasAttr = true;
    }
    const auto FD = FreeDescriptor();
    if (FD == NoDescriptor) return -24;
    std::shared_ptr<MqueueState> State;
    if (const auto Existing = Ipc().MqueueNames.find(*Name); Existing != Ipc().MqueueNames.end()) {
      if ((OpenFlags & Create) && (OpenFlags & Exclusive)) return -17;
      State = Existing->second;
      const auto Access = OpenFlags & 3;
      const unsigned Required = (Access != 1 ? 4 : 0) | (Access != 0 ? 2 : 0);
      if (!InodeAllows(State->Inode, 0100000 | State->Mode, Required)) return -13;
    } else {
      if (!(OpenFlags & Create)) return -2;
      const bool Privileged = Task->Creds.Capable(CapSysResource);
      if (HasAttr) {
        if (const auto Error = Switch2Guest::CheckMqAttr(Attr, Privileged)) return Error;
      }
      if (Ipc().MqueueAccounts->Queues >= 1024 || (Ipc().MqueueAccounts->Queues >= Switch2Guest::MqQueuesMax && !Privileged)) return -28;
      const auto Charged = Switch2Guest::MqChargedBytes(Attr.MaxMsg, Attr.MsgSize);
      const auto Owner = Task->Creds.Uid[0];
      if (!Privileged && Ipc().MqueueAccounts->Bytes[Owner] + Charged > Task->Limits[ResourceMsgqueue][0]) return -24;
      State = std::make_shared<MqueueState>(Attr.MaxMsg, Attr.MsgSize);
      State->Book = Ipc().MqueueAccounts;
      State->ChargedUid = Owner;
      State->Charged = Charged;
      State->Uid = Task->Creds.Uid[1];
      State->Gid = Task->Creds.Gid[1];
      State->Mode = static_cast<uint16_t>(Mode & 0777 & ~Task->Fs->CreationMask);
      State->Name = std::string(*Name);
      State->Inode = NextAnonymousInode++;
      ++Ipc().MqueueAccounts->Queues;
      Ipc().MqueueAccounts->Bytes[Owner] += Charged;
      auto& Record = Inodes[State->Inode];
      Record.Mode = State->Mode;
      Record.Owner.emplace(State->Uid, State->Gid);
      const auto Now = WallClock();
      Record.Times = InodeTimestamps {Now, Now, Now};
      Ipc().MqueueNames.emplace(State->Name, State);
      PruneMqueues();
      Ipc().AllMqueues.push_back({State->Inode, State});
    }
    auto File = std::make_shared<OpenFile>();
    File->StatusFlags = static_cast<uint32_t>(OpenFlags & (3 | 04000));
    File->Inode = State->Inode;
    File->Mqueue = std::move(State);
    Descriptors().Files[FD] = std::move(File);
    Descriptors().Flags[FD] = 1; // mqueue descriptors are always close-on-exec
    return FD;
  }

  int64_t MqueueUnlink(uint64_t NameAddress) {
    const auto Name = GuestString(NameAddress, Switch2Guest::MqNameMax + 1, -36);
    if (!Name) return Name.Error;
    if (Name->empty()) return Invalid;
    if (Name->find('/') != std::string_view::npos || *Name == "." || *Name == "..") return -13;
    const auto Found = Ipc().MqueueNames.find(*Name);
    if (Found == Ipc().MqueueNames.end()) return -2;
    // The queue directory is sticky (01777): only the owner of the queue or of
    // the directory (root) may remove an entry.
    if (Found->second->Uid != Task->Creds.Uid[3] && !Task->Creds.Capable(CapFowner)) return -1;
    Found->second->Name.clear();
    Ipc().MqueueNames.erase(Found); // the queue lives until its last descriptor closes
    ++IOEpoch;
    return 0;
  }

  // mq_notify's one-shot notification: only for the empty to non-empty
  // transition, and only when no receiver is blocked (it takes the message).
  void NotifyMqueue(MqueueState& State) {
    if (!State.NotifyOwner) return;
    for (const auto& Thread : Threads) {
      if (Thread.Core && !Thread.Exited && Thread.MqWait == &State) return;
    }
    const auto Owner = State.NotifyOwner;
    State.NotifyOwner = 0;
    auto* Target = FindProcess(Owner);
    if (!Target || Target->Exited || State.NotifyKind != SigevSignal || !State.NotifySignal) return;
    auto Info = MakeSigInfo(State.NotifySignal, SiMesgq, Task->PID, Task->Creds.Uid[0]);
    Info.Kill.Value = State.NotifyValue;
    SendSignal(*Target, nullptr, Info);
  }

  int64_t MqueueSend(uint64_t FD, uint64_t Address, uint64_t Length, uint64_t Priority, uint64_t Timeout) {
    if (static_cast<uint32_t>(Priority) >= Switch2Guest::MqPrioMax) return Invalid;
    uint64_t Deadline {};
    bool Expired {};
    // The timeout is parsed whether or not the call blocks (native Linux 7.3).
    if (const auto Error = FutexDeadline(Timeout, true, true, Deadline, Expired)) return Error;
    auto* File = MqueueFile(FD);
    if (!File || !WriteAccess(File->StatusFlags)) return BadFile;
    auto& State = *File->Mqueue;
    if (Length > static_cast<uint64_t>(State.Queue.MessageSize())) return -90;
    if (AccessiblePrefix(Address, Length, PF_R) < Length) return Fault;
    if (State.Queue.Full()) {
      if (File->Nonblocking()) return -11;
      if (Expired) return -110;
      Current->Deadline = Deadline;
      return WaitForIO();
    }
    const bool WasEmpty = State.Queue.Empty();
    State.Queue.Send({reinterpret_cast<const uint8_t*>(Address), static_cast<size_t>(Length)}, static_cast<uint32_t>(Priority));
    ++State.Changes;
    ++IOEpoch;
    if (WasEmpty) NotifyMqueue(State);
    return 0;
  }

  int64_t MqueueReceive(uint64_t FD, uint64_t Address, uint64_t Length, uint64_t PriorityAddress, uint64_t Timeout) {
    Current->MqWait = nullptr;
    uint64_t Deadline {};
    bool Expired {};
    if (const auto Error = FutexDeadline(Timeout, true, true, Deadline, Expired)) return Error;
    auto* File = MqueueFile(FD);
    if (!File || !ReadAccess(File->StatusFlags)) return BadFile;
    auto& State = *File->Mqueue;
    if (Length < static_cast<uint64_t>(State.Queue.MessageSize())) return -90;
    if (State.Queue.Empty()) {
      if (File->Nonblocking()) return -11;
      if (Expired) return -110;
      Current->Deadline = Deadline;
      Current->MqWait = &State;
      return WaitForIO();
    }
    const auto Front = State.Queue.FrontLength();
    if (AccessiblePrefix(Address, Front, PF_W) < Front) return Fault;
    if (PriorityAddress && Available(PriorityAddress, PF_W) < sizeof(uint32_t)) return Fault;
    uint32_t Priority {};
    const auto Received = State.Queue.Receive({reinterpret_cast<uint8_t*>(Address), static_cast<size_t>(Length)}, Priority);
    if (PriorityAddress) CopyOut(PriorityAddress, Priority);
    InvalidateExecutableStores(Address, Received);
    ++State.Changes;
    ++IOEpoch;
    return Received;
  }

  int64_t MqueueNotifyRegister(uint64_t FD, uint64_t EventAddress) {
    std::array<uint8_t, 64> Event {};
    if (EventAddress && !ReadStruct(EventAddress, Event)) return Fault;
    uint64_t Value {};
    int32_t Signal {}, Kind {};
    std::memcpy(&Value, Event.data(), sizeof(Value));
    std::memcpy(&Signal, Event.data() + 8, sizeof(Signal));
    std::memcpy(&Kind, Event.data() + 12, sizeof(Kind));
    // SIGEV_THREAD reaches the kernel as a netlink cookie from glibc, which
    // needs AF_NETLINK: not available, so it is rejected like a bad kind.
    if (EventAddress && Kind != SigevNone && Kind != SigevSignal) return Invalid;
    // do_mq_notify checks the signal before the descriptor (verified natively: 1 to 64).
    if (EventAddress && Kind == SigevSignal && (Signal < 1 || Signal > 64)) return Invalid;
    auto* File = MqueueFile(FD);
    if (!File) return BadFile;
    auto& State = *File->Mqueue;
    if (!EventAddress) {
      if (State.NotifyOwner == Task->PID) State.NotifyOwner = 0;
      return 0;
    }
    if (State.NotifyOwner) return -16;
    State.NotifyOwner = Task->PID;
    State.NotifyKind = Kind;
    State.NotifySignal = static_cast<uint32_t>(Signal);
    State.NotifyValue = Value;
    return 0;
  }

  int64_t MqueueAttributes(uint64_t FD, uint64_t NewAddress, uint64_t OldAddress) {
    std::array<int64_t, 8> New {};
    if (NewAddress && !ReadStruct(NewAddress, New)) return Fault;
    if (NewAddress && (New[0] & ~int64_t {04000})) return Invalid;
    auto* File = MqueueFile(FD);
    if (!File) return BadFile;
    auto& State = *File->Mqueue;
    std::array<int64_t, 8> Old {};
    Old = {int64_t {File->StatusFlags & 04000}, State.Queue.Maximum(), State.Queue.MessageSize(),
           static_cast<int64_t>(State.Queue.Pending())};
    if (NewAddress) File->StatusFlags = (File->StatusFlags & ~04000U) | (New[0] & 04000);
    if (OldAddress && !WriteStruct(OldAddress, Old)) return Fault;
    return 0;
  }

  // read(2) on a queue descriptor returns its status line.
  int64_t ReadMqueue(OpenFile& File, char* Buffer, uint64_t Count, uint64_t Position) {
    const auto& State = *File.Mqueue;
    const uint64_t Size = State.Queue.QueuedBytes(); // QSIZE: the sum of the queued message lengths
    const bool Signal = State.NotifyOwner && State.NotifyKind == SigevSignal;
    char Line[96];
    const int Length = std::snprintf(Line, sizeof(Line), "QSIZE:%-10lu NOTIFY:%-5d SIGNO:%-5d NOTIFY_PID:%-6d\n",
                                     static_cast<unsigned long>(Size), State.NotifyOwner ? State.NotifyKind : 0,
                                     Signal ? static_cast<int>(State.NotifySignal) : 0, static_cast<int>(State.NotifyOwner));
    if (Position >= static_cast<uint64_t>(Length)) return 0;
    const auto Bytes = std::min<uint64_t>(Count, Length - Position);
    std::memcpy(Buffer, Line + Position, Bytes);
    return Bytes;
  }

  uint32_t MqueueEvents(const OpenFile& File) const {
    const auto& Queue = File.Mqueue->Queue;
    return (Queue.Empty() ? 0 : 1) | (Queue.Full() ? 0 : 4); // mq_poll: EPOLLIN when messages wait, EPOLLOUT while there is room
  }

  // ================= /dev/shm (tmpfs) =================
  //
  // A flat memory-backed directory: name -> memory file. Mode, owner and
  // times live in the shared inode table like every other anonymous inode.
  // Descriptors reuse the memfd read, write, truncate and mmap paths.
  struct ShmFile {
    std::shared_ptr<GuestMemoryFile> Memory;
    uint64_t Inode {};
  };
  std::map<std::string, ShmFile, std::less<>> ShmFiles;

  static constexpr std::string_view ShmDirectory = "/dev/shm";
  static uint64_t ShmDirectoryInode() { return VirtualInode(ShmDirectory); }

  // The entry name of a resolved SD path like <root>/dev/shm/name.
  std::optional<std::string_view> ShmEntryName(std::string_view Path) const {
    if (!Path.starts_with(RootPath)) return std::nullopt;
    Path.remove_prefix(RootPath.size());
    if (!Path.starts_with("/dev/shm/")) return std::nullopt;
    Path.remove_prefix(9);
    if (Path.empty() || Path.find('/') != Path.npos) return std::nullopt;
    return Path;
  }

  uint64_t ShmLinkCount(uint64_t Inode) const {
    return std::count_if(ShmFiles.begin(), ShmFiles.end(), [Inode](const auto& Entry) { return Entry.second.Inode == Inode; });
  }

  bool IsShmInode(uint64_t Inode) const {
    return std::any_of(ShmFiles.begin(), ShmFiles.end(), [Inode](const auto& Entry) { return Entry.second.Inode == Inode; });
  }

  // The directory is 01777 root:root: removing or replacing an entry needs
  // ownership of the entry or CAP_FOWNER.
  bool CanRemoveShmEntry(uint64_t Inode) const {
    return Task->Creds.Capable(CapFowner) || InodeOwner(Inode).first == Task->Creds.Uid[3] || Task->Creds.Uid[3] == 0;
  }

  void ShmDirectoryChanged() { MarkInodeChanged(ShmDirectoryInode(), true); }

  // After a name leaves the directory: the inode is unlinked once no name remains.
  void ShmNameDropped(uint64_t Inode) {
    if (!ShmLinkCount(Inode)) Inodes[Inode].Unlinked = true;
    MarkInodeChanged(Inode);
  }

  int64_t OpenShmFile(OpenFile& File, std::string_view Name, uint64_t Flags, uint64_t CreationMode, bool TrailingSlash) {
    constexpr uint64_t Create = 0100, Exclusive = 0200, Truncate = 01000, DirectoryOnly = 0200000;
    const bool PathOnly = (Flags & PathOnlyFlag) != 0;
    auto Entry = ShmFiles.find(Name);
    if (Entry == ShmFiles.end()) {
      if (!(Flags & Create)) return -2;
      if (TrailingSlash) return -21;
      if (Name.size() > 255) return -36;
      ShmFile Created;
      Created.Memory = std::make_shared<GuestMemoryFile>();
      Created.Memory->Seals = 1; // F_SEAL_SEAL: only memfd_create(MFD_ALLOW_SEALING) files can be sealed
      Created.Inode = NextAnonymousInode++;
      auto& Record = Inodes[Created.Inode];
      Record.Mode = CreationMode & 07777 & ~Task->Fs->CreationMask;
      Record.Owner.emplace(Task->Creds.Uid[3], Task->Creds.Gid[3]);
      const auto Now = WallClock();
      Record.Times = InodeTimestamps {Now, Now, Now};
      Entry = ShmFiles.emplace(std::string(Name), std::move(Created)).first;
      ShmDirectoryChanged();
    } else {
      if ((Flags & Create) && (Flags & Exclusive)) return -17;
      if ((Flags & DirectoryOnly) || TrailingSlash) return -20;
      const auto Inode = Entry->second.Inode;
      if (!PathOnly) {
        const auto Access = Flags & 3;
        const unsigned Required = (Access != 1 ? 4 : 0) | (Access != 0 || (Flags & Truncate) ? 2 : 0);
        if (!InodeAllows(Inode, ModeForInode(Inode, 0100000), Required)) return -13;
        if ((Flags & NoAtimeFlag) && !OwnsInodeOrRoot(Inode)) return -1;
      }
    }
    const auto& Node = Entry->second;
    if ((Flags & Truncate) && !PathOnly && Node.Memory->Size) {
      if (const auto Error = Node.Memory->Resize(0)) return Error;
      MarkFileDataChanged(Node.Inode);
    }
    File.Memory = Node.Memory;
    File.Inode = Node.Inode;
    File.Path = std::string(ShmDirectory) + "/" + std::string(Name);
    return 0;
  }

  int64_t UnlinkShmFile(std::string_view Name, bool TrailingSlash) {
    const auto Entry = ShmFiles.find(Name);
    if (Entry == ShmFiles.end()) return -2;
    if (TrailingSlash) return -20;
    const auto Inode = Entry->second.Inode;
    if (!CanRemoveShmEntry(Inode)) return -1;
    ShmFiles.erase(Entry);
    ShmNameDropped(Inode);
    ShmDirectoryChanged();
    ++IOEpoch;
    return 0;
  }

  int64_t RenameShmFile(std::optional<std::string_view> Old, std::optional<std::string_view> New, bool OldSlash, bool NewSlash,
                        uint64_t Flags) {
    constexpr uint64_t NoReplace = 1, Exchange = 2, Whiteout = 4;
    if (!Old || !New) return -18; // a rename between /dev/shm and another filesystem
    const auto Source = ShmFiles.find(*Old);
    if (Source == ShmFiles.end()) return -2;
    if (OldSlash || NewSlash) return -20;
    if (Flags & Whiteout) return -1;
    if (New->size() > 255) return -36;
    const auto Target = ShmFiles.find(*New);
    if ((Flags & Exchange) && Target == ShmFiles.end()) return -2;
    if (Target != ShmFiles.end()) {
      if (Flags & NoReplace) return -17;
      if (Target->second.Inode == Source->second.Inode) return 0; // two names of one file: nothing happens
    }
    if (!CanRemoveShmEntry(Source->second.Inode)) return -1;
    if (Target != ShmFiles.end() && !CanRemoveShmEntry(Target->second.Inode)) return -1;
    if (Flags & Exchange) {
      std::swap(Source->second, Target->second);
    } else {
      auto Moved = std::move(Source->second);
      const auto Replaced = Target == ShmFiles.end() ? 0 : Target->second.Inode;
      ShmFiles.erase(Source);
      ShmFiles.insert_or_assign(std::string(*New), std::move(Moved));
      if (Replaced) ShmNameDropped(Replaced);
    }
    ShmDirectoryChanged();
    return 0;
  }

  int64_t LinkShmFile(std::optional<std::string_view> Old, std::optional<std::string_view> New, bool NewSlash) {
    if (!Old || !New) return -18;
    const auto Source = ShmFiles.find(*Old);
    if (Source == ShmFiles.end()) return -2;
    if (NewSlash) return -2;
    if (New->size() > 255) return -36;
    if (ShmFiles.contains(*New)) return -17;
    const auto Node = Source->second;
    ShmFiles.emplace(std::string(*New), Node);
    Inodes[Node.Inode].Unlinked = false;
    MarkInodeChanged(Node.Inode);
    ShmDirectoryChanged();
    return 0;
  }

  int64_t TruncateShmFile(std::string_view Name, bool TrailingSlash, uint64_t Size) {
    const auto Entry = ShmFiles.find(Name);
    if (Entry == ShmFiles.end()) return -2;
    if (TrailingSlash) return -20;
    const auto Inode = Entry->second.Inode;
    if (!InodeAllows(Inode, ModeForInode(Inode, 0100000), 2)) return -13;
    if (const auto Error = Entry->second.Memory->Resize(Size)) return Error;
    MarkFileDataChanged(Inode);
    return 0;
  }

  using SignalTable = std::array<std::array<uint64_t, 4>, 64>;
  struct ExecImage {
    uint64_t Inode {};
    std::optional<uint32_t> EffectiveUID, EffectiveGID;
    ElfImage Program;
    ElfImage Interpreter;
    std::string Path;        // the name execve was given (comm, AT_EXECFN)
    std::string ProgramPath; // the ELF that runs; differs for interpreter scripts
    std::string ResolvedPath; // the SD path ProgramPath resolved to
    std::vector<std::string> Arguments;
    std::vector<std::string> Environment;
  };

  static constexpr size_t ResourceCount = 16; // RLIM_NLIMITS
  static constexpr uint64_t Infinity = UINT64_MAX;
  static constexpr size_t ResourceCpu = 0, ResourceData = 2, ResourceStack = 3, ResourceCore = 4, ResourceNproc = 6, ResourceNofile = 7,
                          ResourceMemlock = 8, ResourceSigpending = 11, ResourceMsgqueue = 12, ResourceNice = 13,
                          ResourceRtprio = 14;
  // The limits of the launched program: Linux's INIT_RLIMITS with the
  // runtime's capacities (descriptor table, thread table, signal queue).
  static std::array<std::array<uint64_t, 2>, ResourceCount> DefaultLimits() {
    std::array<std::array<uint64_t, 2>, ResourceCount> Limits;
    Limits.fill({Infinity, Infinity});
    Limits[ResourceStack] = {MainStackSize, Infinity};
    Limits[ResourceCore] = {0, Infinity};
    Limits[ResourceNproc] = {GuestThreadCount, GuestThreadCount};
    Limits[ResourceNofile] = {DescriptorCount, DescriptorCount};
    Limits[ResourceMemlock] = {8 << 20, 8 << 20};
    Limits[ResourceSigpending] = {SignalPendingDefault, SignalPendingDefault};
    Limits[ResourceMsgqueue] = {819200, 819200};
    Limits[ResourceNice] = {0, 0};
    Limits[ResourceRtprio] = {0, 0};
    return Limits;
  }

  struct FsState {
    std::string Folder;
    std::string WorkingDirectory;
    bool RemovedWorkingDirectory {};
    uint32_t CreationMask {0022};
    // Where the root (chroot, pivot_root) and the working directory sit in the mount namespace: Root and Folder are
    // host paths inside RootMount and FolderMount.
    std::string Root;
    Switch2Guest::Mounts::MountRef RootMount, FolderMount;
  };
  struct GuestProcess {
    GuestAddressSpace Memory;
    std::vector<std::weak_ptr<StreamSocket::PinnedPage>> PrivatePipePages;
    std::array<FEXCore::Core::CPUState::gdt_segment, 32> GDT {};
    // Only fork descendants can share a private host address view. Exec
    // builds a fresh address space with independently owned backing.
    std::shared_ptr<const uint8_t> ForkView {std::make_shared<const uint8_t>(0)};
    // The directory state (cwd, umask) a CLONE_FS child shares with its parent.
    std::shared_ptr<FsState> Fs {std::make_shared<FsState>()};
    bool NoNewPrivileges {};
    // Real, effective, saved and filesystem identities.
    // Capability sets and securebits ride along with the identities (Switch2Guest::Credentials).
    Switch2Guest::Credentials Creds;
    // The namespaces the process lives in: user, UTS, IPC, PID (Pid is where its ids are allocated and
    // PidForChildren where its children's are), cgroup, net and mount. Shared with the parent unless a CLONE_NEW*
    // flag or setns changed them. Creds hold kernel (initial namespace) ids; Ns.User translates them.
    Switch2Guest::Ns::NsSet Ns;
    std::vector<uint32_t> SupplementaryGroups;
    std::shared_ptr<DescriptorState> PendingDescriptors {std::make_shared<DescriptorState>()};
    SignalTable SignalActions {};
    SignalQueue SharedPending;
    uint64_t HeapStart {};
    uint64_t HeapEnd {};
    bool LockFuture {};              // mlockall(MCL_FUTURE); a fork child starts unlocked
    uint32_t MembarrierRegistered {}; // membarrier registrations (command bits); inherited by fork, cleared by exec
    std::vector<ShmAttach> ShmAttached; // System V segments mapped here; inherited by fork, dropped by exec and exit
    // SEM_UNDO adjustments. Shared with a CLONE_SYSVSEM child; applied when the last sharer exits.
    std::shared_ptr<SemUndoList> SemUndo {std::make_shared<SemUndoList>()};
    uint64_t CpuTicks {};
    uint64_t ExecutableInode {};
    std::string ExecutablePath; // the SD path of the ELF that runs, as /proc/<pid>/exe names it
    uint32_t PID {ProcessID};
    uint32_t ParentPID {1};
    uint32_t Group {ProcessID};
    // The launched program leads its own session and holds the console as
    // its controlling terminal, like a login shell.
    uint32_t Session {ProcessID};
    bool HasTerminal {true};
    uint32_t ExitSignal {17}; // Sent to the parent on exit (clone's CSIGNAL).
    uint32_t ParentDeathSignal {};
    bool ChildSubreaper {};
    bool Dumpable {true};
    int32_t Nice {};
    uint32_t Personality {};
    std::array<char, 16> Name {};
    uint64_t ChildTicks {}; // CPU ticks of reaped descendants.
    // ITIMER_REAL (monotonic ns), ITIMER_VIRTUAL and ITIMER_PROF (process
    // CPU ns). Kept across exec, not inherited by fork.
    std::array<Switch2Guest::ArmedTimer, 3> ITimers {};
    // POSIX timers (timer_create); deleted by exec, not inherited by fork.
    std::vector<PosixTimer> Timers;
    int32_t NextTimerId {};
    // Soft and hard resource limits (RLIMIT_*), inherited by fork and kept by exec.
    std::array<std::array<uint64_t, 2>, ResourceCount> Limits {DefaultLimits()};
    // Job control: a stopped process does not run; the parent may collect
    // one stop or continue report through wait.
    bool Stopped {};
    uint32_t StopSignal {};
    bool ReportStop {};
    bool ReportContinue {};
    // Exited with no parent left to wait: retired then dropped (Reaped).
    bool AutoReap {};
    bool Reaped {};
    bool PidRetired {}; // its numbers have left the PID namespaces' maps
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

  // ================= Namespaces (Milestone 4, task 2) =================

  // ksys_unshare: validates every flag first, then changes the process's namespaces, descriptor table, directory
  // state and System V semaphore undo list, so a failure changes nothing. The checks of the namespace flags are
  // BuildNamespaces (new user namespace first, capabilities against it).
  int64_t UnshareGuest(uint64_t Flags) {
    namespace Ns = Switch2Guest::Ns;
    constexpr uint64_t Known = CloneThread_ | CloneFs | Ns::CloneNewNs | CloneSighand | CloneVm | CloneFiles | CloneSysvsem |
                               Ns::CloneNewUts | Ns::CloneNewIpc | Ns::CloneNewNet | Ns::CloneNewUser | Ns::CloneNewPid |
                               Ns::CloneNewCgroup | Ns::CloneNewTime;
    if (Flags & ~Known) return Invalid;
    size_t Threads {};
    for (const auto& Thread : this->Threads) Threads += Thread.Core && !Thread.Exited && Thread.PID == Task->PID;
    const bool Multithreaded = Threads > 1;
    // CLONE_NEWNS implies CLONE_FS; CLONE_NEWUSER implies CLONE_THREAD and CLONE_FS, so it needs one thread.
    if (Flags & Ns::CloneNewNs) Flags |= CloneFs;
    if (Flags & Ns::CloneNewUser) Flags |= CloneThread_ | CloneFs;
    if ((Flags & CloneThread_) && Multithreaded) return Invalid;
    if ((Flags & CloneVm) && Multithreaded) return Invalid;
    if (Flags & Ns::CloneNewIpc) Flags |= CloneSysvsem;
    const auto Wanted = Flags & Ns::AllNsFlags;
    std::optional<Ns::Switch> Plan;
    if (Wanted) {
      auto Built = Ns::BuildNamespaces(NsRegistry, Task->Ns, SubjectOf(*Task), Wanted, IsChrooted());
      if (const auto* Error = std::get_if<int64_t>(&Built)) return *Error;
      Plan = std::move(std::get<Ns::Switch>(Built));
    }
    if (Flags & CloneSysvsem) ExitSem(*Task); // CLONE_SYSVSEM is equivalent to sys_exit() for the undo list
    if ((Flags & CloneFs) && Task->Fs.use_count() > 1) Task->Fs = std::make_shared<FsState>(*Task->Fs);
    if ((Flags & CloneFiles) && !Current->Descriptors.unique()) Current->Descriptors = std::make_shared<DescriptorState>(*Current->Descriptors);
    if (Plan) {
      const auto Old = Task->Ns;
      Task->Ns = Plan->Ns;
      if (Wanted & Ns::CloneNewIpc) Task->Ns.Ipc->Store = std::make_shared<IpcStore>();
      if (Plan->NewUser) Ns::EnterUserNs(Task->Creds, Plan->NewUser);
      if (Plan->NewMnt) OnMountNamespaceCreated(*Task->Ns.Mnt, *Old.Mnt, *Task->Fs);
    }
    ++IOEpoch;
    return 0;
  }

  // setns(fd, nstype): fd is an nsfs file (nstype 0 or its kind) or a pidfd (nstype a mask of CLONE_NEW* flags,
  // user namespace first). Every namespace is checked before any is installed.
  int64_t SetNsGuest(uint64_t FDArgument, uint64_t Type) {
    namespace Ns = Switch2Guest::Ns;
    const auto* File = GetFile(FDArgument);
    if (!File) return BadFile;
    size_t Threads {};
    for (const auto& Thread : this->Threads) Threads += Thread.Core && !Thread.Exited && Thread.PID == Task->PID;
    const bool Single = Threads <= 1;
    const bool SharesFs = Task->Fs.use_count() > 1;
    std::vector<std::shared_ptr<Ns::Namespace>> Targets;
    if (File->NsObject) {
      if (Type && Type != Ns::FlagOf(File->NsObject->K)) return Invalid;
      Targets.push_back(File->NsObject);
    } else if (File->PidfdTarget) {
      constexpr uint64_t Valid = Ns::CloneNewNs | Ns::CloneNewCgroup | Ns::CloneNewIpc | Ns::CloneNewPid | Ns::CloneNewNet |
                                 Ns::CloneNewUser | Ns::CloneNewUts | Ns::CloneNewTime;
      if (!Type || (Type & ~Valid)) return Invalid;
      const auto* Target = FindProcess(File->PidfdTarget);
      if (!Target || Target->Exited) return -3;
      if (Type & Ns::CloneNewTime) return Invalid;
      for (const auto Kind : {Ns::Kind::User, Ns::Kind::Mnt, Ns::Kind::Uts, Ns::Kind::Ipc, Ns::Kind::Pid, Ns::Kind::Net, Ns::Kind::Cgroup}) {
        if (!(Type & Ns::FlagOf(Kind))) continue;
        // pid namespaces come from pid_for_children, as nsproxy holds them
        Targets.push_back(Kind == Ns::Kind::Pid ? std::static_pointer_cast<Ns::Namespace>(Target->Ns.PidForChildren) : Target->Ns.Get(Kind));
      }
    } else {
      return Invalid;
    }
    auto Who = SubjectOf(*Task);
    auto NewNs = Task->Ns;
    std::shared_ptr<Ns::UserNs> NewUser;
    for (const auto& Target : Targets) {
      if (const auto Error = Ns::CheckInstall(Task->Ns, Who, *Target, Single, SharesFs)) return Error;
      if (Target->K == Ns::Kind::User) {
        NewUser = std::static_pointer_cast<Ns::UserNs>(Target);
        Who.Ns = NewUser;
        Who.CapEffective = Switch2Guest::CapFull; // the later installations are checked in the new namespace
      }
      Ns::Install(NewNs, Target);
    }
    // All checks passed: commit.
    if (NewNs.Ipc != Task->Ns.Ipc) ExitSem(*Task); // ipcns_install: ditch the old namespace's undo state
    const auto OldMnt = Task->Ns.Mnt;
    Task->Ns = NewNs;
    if (NewUser) Ns::EnterUserNs(Task->Creds, NewUser);
    if (NewNs.Mnt != OldMnt) OnMountNamespaceEntered(*Task->Ns.Mnt);
    ++IOEpoch;
    return 0;
  }

  // A descriptor for a namespace object (open_related_ns), close-on-exec.
  int64_t OpenNsFile(std::shared_ptr<Switch2Guest::Ns::Namespace> Object) {
    const auto FD = FreeDescriptor();
    if (FD == NoDescriptor) return -24;
    auto File = std::make_shared<OpenFile>();
    File->Inode = Object->Ino;
    File->StatusFlags = 0;
    File->Path = std::string {"nsfs:["} + std::to_string(Object->Ino) + "]";
    File->NsObject = std::move(Object);
    Descriptors().Files[FD] = std::move(File);
    Descriptors().Flags[FD] = 1;
    return FD;
  }

  // The ioctls of fs/nsfs.c on a namespace file.
  int64_t NsIoctl(const OpenFile& File, uint64_t Request, uint64_t Argument) {
    namespace Ns = Switch2Guest::Ns;
    const auto& Object = *File.NsObject;
    switch (static_cast<uint32_t>(Request)) {
    case Ns::NsGetUserns: {
      auto Owner = Ns::NsOwner(Object, *Task->Ns.User);
      if (const auto* Error = std::get_if<int64_t>(&Owner)) return *Error;
      return OpenNsFile(std::get<std::shared_ptr<Ns::UserNs>>(Owner));
    }
    case Ns::NsGetParent: {
      auto Parent = Ns::NsParent(Object, *Task->Ns.User, *Task->Ns.Pid);
      if (const auto* Error = std::get_if<int64_t>(&Parent)) return *Error;
      return OpenNsFile(std::get<std::shared_ptr<Ns::Namespace>>(Parent));
    }
    case Ns::NsGetNstype: return static_cast<int64_t>(Ns::FlagOf(Object.K));
    case Ns::NsGetOwnerUid: {
      if (Object.K != Ns::Kind::User) return Invalid;
      const uint32_t Owner = UidOut(static_cast<const Ns::UserNs&>(Object).OwnerUid);
      return CopyOut(Argument, Owner) ? Fault : 0;
    }
    default: return -25; // ENOTTY
    }
  }

  // ================= Mount namespaces (Milestone 4, task 3) =================
  //
  // Each mount namespace owns a Switch2Guest::Mounts::Table (GuestMounts.h). A position is a mount plus a host path
  // inside it; a bind mount reuses the host paths of its source, so inodes are shared and every file operation keeps
  // working on host paths. ResolveSdPath is the one chokepoint that turns a guest name into such a position: it
  // crosses mount points in both directions, stops `..` at the process root, restarts absolute symlinks at the
  // process root and reports the mount the result lies in (LastMount, LastParentMount, LastCrossed).

  using MountTable = Switch2Guest::Mounts::Table;
  using MountRef = Switch2Guest::Mounts::MountRef;
  using MountPlace = Switch2Guest::Mounts::Place;

  static MountTable& TableOf(const Switch2Guest::Ns::MntNs& Namespace) { return *static_cast<MountTable*>(Namespace.Table.get()); }
  MountTable& MntTable() const { return TableOf(*Task->Ns.Mnt); }
  Switch2Guest::Mounts::Ids MountIds;

  // The initial table: a hidden top mount, the SD card root on it, and the virtual /proc, /dev and /dev/shm.
  std::shared_ptr<void> BuildInitialMounts() {
    namespace Mn = Switch2Guest::Mounts;
    auto Table = std::make_shared<MountTable>();
    const auto Owner = NsRegistry.Initial.User;
    const auto Make = [&](const char* Type, const char* Source, uint64_t Dev, std::string Root) {
      auto Sb = std::make_shared<Mn::Superblock>();
      Sb->Dev = Dev;
      Sb->Type = Type;
      Sb->Source = Source;
      Sb->FsRoot = std::move(Root);
      Sb->Owner = Owner;
      return Sb;
    };
    const auto Sd = Make("msdos", "sdmc", 1, RootPath);
    const auto Top = Mn::MakeMount(*Table, MountIds, nullptr, "", RootPath, Sd, 0);
    const auto Slash = Mn::MakeMount(*Table, MountIds, Top, RootPath, RootPath, Sd, Mn::MntRelatime);
    constexpr uint32_t Hardened = Mn::MntNosuid | Mn::MntNodev | Mn::MntNoexec | Mn::MntRelatime;
    auto ProcSb = Make("proc", "proc", ProcDevice, RootPath + "/proc");
    ProcSb->Pid = NsRegistry.Initial.Pid;
    Mn::MakeMount(*Table, MountIds, Slash, RootPath + "/proc", RootPath + "/proc", ProcSb, Hardened);
    const auto Dev = Mn::MakeMount(*Table, MountIds, Slash, RootPath + "/dev", RootPath + "/dev", Make("devtmpfs", "devtmpfs", DevDevice, RootPath + "/dev"),
                                   Mn::MntNosuid | Mn::MntRelatime);
    Mn::MakeMount(*Table, MountIds, Dev, RootPath + "/dev/shm", RootPath + "/dev/shm", Make("tmpfs", "tmpfs", ShmDevice, RootPath + "/dev/shm"),
                  Mn::MntNosuid | Mn::MntNodev | Mn::MntRelatime);
    return Table;
  }

  // The place that is a process's root or working directory.
  static MountPlace RootOf(const GuestProcess& P) { return {P.Fs->RootMount, P.Fs->Root}; }
  static MountPlace FolderOf(const GuestProcess& P) { return {P.Fs->FolderMount ? P.Fs->FolderMount : P.Fs->RootMount, P.Fs->Folder}; }

  // current_chrooted: the root is not the root of the mount namespace.
  bool IsChrooted() const {
    const auto& Table = MntTable();
    const auto Top = Table.Top();
    const auto Place = Switch2Guest::Mounts::Cross(Table, {Top, Top->Root});
    return Task->Fs->RootMount != Place.M || Task->Fs->Root != Place.Path;
  }

  // A write below a read-only mount or file system (EROFS), or a creation on a file system that takes none (EPERM).
  static int64_t MountDenies(const MountRef& M, bool Creating) {
    if (!M) return 0;
    return Creating ? Switch2Guest::Mounts::CreateCheck(*M) : Switch2Guest::Mounts::WriteCheck(*M);
  }

  // Whether the walk found nothing at Path: a name that can be created (mkdir, mknod, symlink, link, creat).
  bool NameIsFree(FsFileSystem& SdCard, const std::string& Path) {
    if (LastCrossed || IsVirtualEntry(Path) || IsStoreEntry(Path) || VirtualMount(Path)) return false;
    FsDirEntryType Type {};
    return EntryType(SdCard, Path, Type) == -2;
  }

  // The working directory as getcwd prints it: relative to the root, "(unreachable)" in front when the root is not
  // above it. A directory outside every mount (the launch folder) keeps its stored name.
  std::string WorkingDirectoryText(const GuestProcess& P) const {
    namespace Mn = Switch2Guest::Mounts;
    const auto Where = FolderOf(P);
    if (!Where.M || !Mn::PathContains(Where.M->Root, Where.Path)) return P.Fs->WorkingDirectory;
    if (const auto Visible = Mn::VisiblePath(RootOf(P), Where.M.get(), Where.Path)) return *Visible;
    return Mn::UnreachablePath(Where.M.get(), Where.Path);
  }

  // The text of a /proc/<pid>/root or cwd link as the running process reads it (d_path: no "(unreachable)").
  std::string ProcRootText(const GuestProcess& P, bool Cwd) const {
    namespace Mn = Switch2Guest::Mounts;
    const auto Where = Cwd ? FolderOf(P) : RootOf(P);
    if (!Where.M || !Mn::PathContains(Where.M->Root, Where.Path)) return Cwd ? P.Fs->WorkingDirectory : std::string {"/"};
    if (const auto Visible = Mn::VisiblePath(RootOf(*Task), Where.M.get(), Where.Path)) return *Visible;
    auto Text = Mn::UnreachablePath(Where.M.get(), Where.Path);
    return Text.substr(std::string_view {"(unreachable)"}.size());
  }

  // Every mount table of a live mount namespace (a table is shared by the processes in its namespace).
  template <typename Callback>
  void ForEachMountTable(Callback Visit) {
    std::set<MountTable*> Seen;
    for (auto& [PID, Process] : Processes) {
      if (!Process.Ns.Mnt || !Process.Ns.Mnt->Table) continue;
      auto* Table = &TableOf(*Process.Ns.Mnt);
      if (Seen.insert(Table).second) Visit(*Table);
    }
  }

  // A directory that moved under a new name: mounts and roots recorded below it follow.
  void TrackRenamedMounts(std::string_view Old, std::string_view New) {
    const auto Moved = [&](std::string& Path) {
      if (PathContains(Old, Path)) Path = std::string(New) + Path.substr(Old.size());
    };
    ForEachMountTable([&](MountTable& Table) {
      for (auto& M : Table.List) {
        Moved(M->Mountpoint);
        Moved(M->Root);
      }
    });
    for (auto& [PID, Process] : Processes) Moved(Process.Fs->Root);
  }

  // The tmpfs-like file systems keep their files in a hidden folder on the SD card. It is emptied when the file
  // system's last user goes away (a Superblock destructor queues it), when the main process exits, and, after a
  // crash, once at runtime start.
  // The folder name is kept short: a guest path inside it is as long on the SD card as the same path in the rootfs
  // ("/fex/.t/NN" is not longer than "/fex/rootfs"), so the path limits hold the same way.
  static constexpr std::string_view TmpfsArea = "/fex/.tmp";
  static constexpr std::string_view OldTmpfsAreas[] = {"/fex/.t", "/fex/.tmpfs"}; // earlier builds' folder names
  std::vector<std::string> PendingTmpfsDeletes;
  std::set<std::string> TmpfsFolders; // every folder this run created and has not deleted yet

  // Every name in the folder goes with one inode-store save: all links of a stored inode live in the same file
  // system (link(2) gives EXDEV across mounts), so each stored inode here loses its last name.
  void DeleteTmpfsTree(FsFileSystem& SdCard, const std::string& Folder) {
    bool Changed {};
    for (auto It = SdInodes.lower_bound(Folder); It != SdInodes.end() && It->first.starts_with(Folder);) {
      if (!PathContains(Folder, It->first)) {
        ++It;
        continue;
      }
      const auto Inode = It->second;
      const auto Record = Inodes.find(Inode);
      if (Record != Inodes.end() && (Record->second.ManagedFile || Record->second.Symlink || Record->second.SpecialType)) {
        Record->second.Unlinked = true; // retires a managed file's object
        MarkInodeChanged(Inode);
      } else if (Record != Inodes.end()) {
        Inodes.erase(Record); // a directory or native file: its only name goes
      }
      Changed = true;
      It = SdInodes.erase(It);
    }
    if (Changed) {
      InodeStoreDirty = true;
      if (SaveInodeStore() < 0) WriteLog("Guest inode metadata could not be saved after a tmpfs folder was removed.");
    }
    SdRetry(fsFsDeleteDirectoryRecursively, &SdCard, Folder.c_str());
  }

  void FlushTmpfsDeletes() {
    if (PendingTmpfsDeletes.empty()) return;
    FsFileSystem SdCard {};
    if (R_FAILED(SdRetry(fsOpenSdCardFileSystem, &SdCard))) return;
    auto Queue = std::move(PendingTmpfsDeletes);
    PendingTmpfsDeletes.clear();
    for (const auto& Folder : Queue) {
      TmpfsFolders.erase(Folder);
      DeleteTmpfsTree(SdCard, Folder);
    }
    fsFsClose(&SdCard);
  }

  // The runtime ends with the main process, so every folder of this run goes, whoever still holds the file system.
  void FlushAllTmpfs() {
    for (const auto& Folder : TmpfsFolders) PendingTmpfsDeletes.push_back(Folder);
    FlushTmpfsDeletes();
  }

  // A crashed run leaves its folders behind; nothing of an earlier run is alive now.
  void CleanStaleTmpfs() {
    FsFileSystem SdCard {};
    if (R_FAILED(SdRetry(fsOpenSdCardFileSystem, &SdCard))) return;
    DeleteTmpfsTree(SdCard, std::string(TmpfsArea));
    for (const auto Old : OldTmpfsAreas) DeleteTmpfsTree(SdCard, std::string(Old));
    fsFsClose(&SdCard);
  }

  // The folder that holds a new memory file system: created empty with the superblock's mode and owner.
  int64_t CreateTmpfsFolder(Switch2Guest::Mounts::Superblock& Sb) {
    FsFileSystem SdCard {};
    if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
    auto Error = SdFileError(SdRetry(fsFsCreateDirectory, &SdCard, TmpfsArea.data()));
    if (Error == -17) Error = 0;
    if (!Error) Error = CreateNativeEntry(SdCard, Sb.FsRoot, Sb.Mode, true);
    fsFsClose(&SdCard);
    if (Error) return Error;
    const auto Inode = SdInode(Sb.FsRoot);
    auto& Record = Inodes[Inode];
    Record.Mode = Sb.Mode & 07777;
    Record.Owner.emplace(Sb.Uid, Sb.Gid);
    MarkInodeChanged(Inode);
    return SaveInodeStore();
  }

  // An existing guest path as mount, umount and pivot_root look it up: the walk result and whether it is a directory.
  struct MountLookup {
    int64_t Error {};
    MountPlace At;
    bool Directory {};
    std::string Name;
  };
  MountLookup LookupForMount(std::string_view Name, bool Follow = true) {
    MountLookup Out;
    FsFileSystem SdCard {};
    if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) {
      Out.Error = SdFileError(Status);
      return Out;
    }
    std::string Path;
    Out.Error = ResolveSdPath(CurrentDirectory, Name, SdCard, Path, Follow);
    if (!Out.Error) {
      if (!Switch2Guest::Mounts::PathContains(LastMount->Root, Path)) Out.Error = -22; // outside every mount (the launch folder)
      else if (!VirtualDirectory(Path).empty()) Out.Directory = true;
      else if (ShmEntryName(Path)) Out.Error = ShmFiles.contains(*ShmEntryName(Path)) ? 0 : -2;
      else if (VirtualMount(Path)) {
        const auto GuestPath = std::string_view(Path).substr(RootPath.size());
        if (!(ProcFile(GuestPath) || ProcNs(GuestPath) || !ProcLink(Path).empty() ||
              CharDevAt(GuestPath) != CharDev::None)) Out.Error = -2;
      } else if (IsVirtualEntry(Path)) {
        // symlinks cannot be the result of a following walk; files made by the inode store are not directories
      } else {
        FsDirEntryType Type {};
        Out.Error = EntryType(SdCard, Path, Type);
        Out.Directory = !Out.Error && Type == FsDirEntryType_Dir;
      }
    }
    fsFsClose(&SdCard);
    Out.At = {LastMount, std::move(Path)};
    Out.Name = std::string {Name};
    return Out;
  }

  // Whether the mount (or with Sb: its file system) has a file open for writing.
  bool MountHasWriters(const MountRef& M, bool WholeFilesystem) {
    bool Found = false;
    ForEachOpenFile([&](const OpenFile& File) {
      if (!File.Mount || File.Directory || File.PathOnly() || !WriteAccess(File.StatusFlags)) return;
      if (WholeFilesystem ? File.Mount->Sb == M->Sb : File.Mount == M) Found = true;
    });
    return Found;
  }

  // Users of a mount other than the table: working directories, roots and open files.
  bool MountBusy(const MountRef& M) {
    for (const auto& [PID, Process] : Processes) {
      if (Process.Exited || Process.Reaped) continue;
      if (Process.Fs->RootMount == M || Process.Fs->FolderMount == M) return true;
    }
    bool Found = false;
    ForEachOpenFile([&](const OpenFile& File) { Found |= File.Mount == M; });
    return Found;
  }

  // mount(2) argument text: type and source are strings (PATH_MAX), data is one page.
  struct MountText {
    int64_t Error {};
    bool Present {};
    std::string Value;
  };
  MountText MountString(uint64_t Address, size_t Limit, bool Page) const {
    MountText Out;
    if (!Address) return Out;
    Out.Present = true;
    const auto* Data = reinterpret_cast<const char*>(Address);
    const size_t Readable = std::min<size_t>(Available(Address, PF_R), Limit);
    if (!Readable) {
      Out.Error = Fault;
      return Out;
    }
    const auto* End = static_cast<const char*>(std::memchr(Data, 0, Readable));
    if (!End && !Page && Readable == Limit) Out.Error = -36;
    else if (!End && !Page && Readable < Limit) Out.Error = Fault;
    Out.Value.assign(Data, End ? End - Data : Readable);
    return Out;
  }

  // The user namespace that owns a new file system of Type: proc follows the PID namespace, mqueue the IPC
  // namespace, sysfs the network namespace, the others the caller's namespace (fc->user_ns).
  std::shared_ptr<Switch2Guest::Ns::UserNs> FsOwner(std::string_view Type) const {
    if (Type == "proc") return Task->Ns.Pid->Owner;
    if (Type == "mqueue") return Task->Ns.Ipc->Owner;
    if (Type == "sysfs") return Task->Ns.Net->Owner;
    return Task->Ns.User;
  }

  // mount(2): path_mount. The decision functions are in GuestMounts.h.
  int64_t MountGuest(uint64_t SourceAddress, uint64_t TargetAddress, uint64_t TypeAddress, uint64_t RawFlags, uint64_t DataAddress) {
    namespace Mn = Switch2Guest::Mounts;
    const auto Type = MountString(TypeAddress, GuestPathMax, false);
    if (Type.Error) return Type.Error;
    const auto Source = MountString(SourceAddress, GuestPathMax, false);
    if (Source.Error) return Source.Error;
    const auto Data = MountString(DataAddress, 4096, true);
    if (Data.Error) return Data.Error;
    const auto Target = GuestString(TargetAddress);
    if (!Target) return Target.Error;
    uint64_t Flags = RawFlags;
    if ((Flags & Mn::MsMgcMsk) == Mn::MsMgcVal) Flags &= ~static_cast<uint64_t>(Mn::MsMgcMsk);
    auto Lookup = LookupForMount(*Target);
    if (Lookup.Error) return Lookup.Error;
    if (Flags & Mn::MsNouser) return Invalid;
    auto& Table = MntTable();
    if (!NsCapableOver(*Task->Ns.Mnt->Owner, CapSysAdmin)) return -1;
    const auto Sd = Mn::FlagsFromMs(Flags);
    ++IOEpoch;
    if (Flags & Mn::MsRemount) {
      Mn::RemountArgs Args;
      Args.Bind = Flags & Mn::MsBind;
      Args.Ms = Flags;
      Args.SbCapable = Lookup.At.M && Lookup.At.M->Sb->Owner && NsCapableOver(*Lookup.At.M->Sb->Owner, CapSysAdmin);
      if (!Args.Bind) {
        auto Parsed = Mn::ParseFsOptions(Lookup.At.M->Sb->Type, Data.Value, MemoryTotal());
        if (const auto* Error = std::get_if<int64_t>(&Parsed)) Args.DataError = *Error;
        else Args.Opts = std::get<Mn::FsOptions>(Parsed);
      }
      Args.Writers = MountHasWriters(Lookup.At.M, !Args.Bind);
      return Mn::Remount(Table, Lookup.At, Args);
    }
    if (Flags & Mn::MsBind) {
      if (!SourceAddress || !Source.Value.size()) return Invalid;
      auto From = LookupForMount(Source.Value);
      if (From.Error) return From.Error;
      return Mn::Bind(Table, MountIds, From.At, From.Directory, Lookup.At, Lookup.Directory, Flags & Mn::MsRec);
    }
    if (Flags & Mn::MsPropagation) return Mn::ChangeType(Table, MountIds, Lookup.At, Flags, Flags & Mn::MsRec);
    if (Flags & Mn::MsMove) {
      if (!SourceAddress || !Source.Value.size()) return Invalid;
      auto From = LookupForMount(Source.Value);
      if (From.Error) return From.Error;
      return Mn::Move(Table, From.At, From.Directory, Lookup.At, Lookup.Directory);
    }
    // do_new_mount
    if (!Type.Present) return Invalid;
    if (!Mn::KnownFsType(Type.Value)) return -19; // ENODEV
    auto Parsed = Mn::ParseFsOptions(Type.Value, Data.Value, MemoryTotal());
    if (const auto* Error = std::get_if<int64_t>(&Parsed)) return *Error;
    auto Options = std::get<Mn::FsOptions>(Parsed);
    const auto Owner = FsOwner(Type.Value);
    if (!NsCapableOver(*Owner, CapSysAdmin)) return -1;
    // uid= and gid= name ids in the caller's namespace
    if (Options.Uid) {
      const auto Kuid = KuidIn(*Options.Uid);
      if (!Kuid) return Invalid;
      Options.Uid = *Kuid;
    }
    if (Options.Gid) {
      const auto Kgid = KgidIn(*Options.Gid);
      if (!Kgid) return Invalid;
      Options.Gid = *Kgid;
    }
    if (!Lookup.Directory) return -20;
    FlushTmpfsDeletes();
    std::string FsRoot = Type.Value == "proc" ? RootPath + "/proc" : std::string(TmpfsArea) + "/" + std::to_string(MountIds.NextDev);
    auto Sb = Mn::MakeSuperblock(MountIds, Type.Value, Source.Value, Options, Sd, Owner, FsRoot, Task->Creds.Uid[3], Task->Creds.Gid[3]);
    if (Type.Value == "proc") Sb->Pid = Task->Ns.Pid; // proc_init_fs_context: the mounter's active PID namespace
    if (Type.Value != "proc") {
      TmpfsFolders.insert(FsRoot);
      if (const auto Error = CreateTmpfsFolder(*Sb)) {
        char Message[160];
        std::snprintf(Message, sizeof(Message), "mount: creating the %s folder %s failed with %lld.", Type.Value.c_str(), FsRoot.c_str(),
                      static_cast<long long>(Error));
        WriteLog(Message);
        PendingTmpfsDeletes.push_back(FsRoot); // the folder may exist already
        FlushTmpfsDeletes();
        return Error;
      }
      // The folder goes when the last user of the file system does.
      Sb->Cleanup = std::shared_ptr<void>(new int, [this, Folder = FsRoot](int* Value) {
        delete Value;
        PendingTmpfsDeletes.push_back(Folder);
      });
    }
    return Mn::NewMount(Table, MountIds, Lookup.At, Lookup.Directory, Sb, Sd.Mnt);
  }

  uint64_t MemoryTotal() const {
    if (const auto* Arena = Task->Memory.GetArena()) return Arena->CommitLimit();
    return 2ULL << 30;
  }

  // umount2(2): can_umount and do_umount.
  int64_t UmountGuest(uint64_t TargetAddress, uint64_t RawFlags) {
    namespace Mn = Switch2Guest::Mounts;
    const auto Flags = static_cast<uint32_t>(RawFlags);
    if (Flags & ~(Mn::MntForce | Mn::MntDetach | Mn::MntExpire | Mn::UmountNofollow)) return Invalid;
    const auto Target = GuestString(TargetAddress);
    if (!Target) return Target.Error;
    const auto Lookup = LookupForMount(*Target, !(Flags & Mn::UmountNofollow));
    if (Lookup.Error) return Lookup.Error;
    Mn::UmountArgs Args;
    Args.Flags = Flags;
    Args.Capable = NsCapableOver(*Task->Ns.Mnt->Owner, CapSysAdmin);
    Args.CapableForce = Lookup.At.M->Sb->Owner && NsCapableOver(*Lookup.At.M->Sb->Owner, CapSysAdmin);
    Args.IsRoot = Lookup.At.M == Task->Fs->RootMount;
    const auto Result = Mn::Umount(MntTable(), Lookup.At, Args, [&](const MountRef& M) { return MountBusy(M); });
    if (Result == 0) {
      ++IOEpoch;
      FlushTmpfsDeletes();
    }
    return Result;
  }

  // pivot_root(2).
  int64_t PivotRootGuest(uint64_t NewAddress, uint64_t OldAddress) {
    namespace Mn = Switch2Guest::Mounts;
    const auto NewName = GuestString(NewAddress);
    if (!NewName) return NewName.Error;
    const auto OldName = GuestString(OldAddress);
    if (!OldName) return OldName.Error;
    const bool Capable = NsCapableOver(*Task->Ns.Mnt->Owner, CapSysAdmin);
    if (!Capable) return -1;
    const auto New = LookupForMount(*NewName);
    if (New.Error) return New.Error;
    if (!New.Directory) return -20;
    const auto Old = LookupForMount(*OldName);
    if (Old.Error) return Old.Error;
    if (!Old.Directory) return -20;
    const auto OldRoot = RootOf(*Task);
    auto Result = Mn::PivotRoot(MntTable(), OldRoot, New.At, Old.At, Capable);
    if (const auto* Error = std::get_if<int64_t>(&Result)) return *Error;
    // chroot_fs_refs: every process whose root or working directory was the old root moves to the new one.
    for (auto& [PID, Process] : Processes) {
      auto& Fs = *Process.Fs;
      if (Fs.RootMount == OldRoot.M && Fs.Root == OldRoot.Path) {
        Fs.RootMount = New.At.M;
        Fs.Root = New.At.Path;
      }
      if (Fs.FolderMount == OldRoot.M && Fs.Folder == OldRoot.Path) {
        Fs.FolderMount = New.At.M;
        Fs.Folder = New.At.Path;
        Fs.WorkingDirectory = GuestWorkingPath(Fs.Folder);
      }
    }
    ++IOEpoch;
    return 0;
  }

  // chroot(2): lookup, search permission, then CAP_SYS_CHROOT in the caller's user namespace.
  int64_t ChrootGuest(uint64_t Address) {
    const auto Name = GuestString(Address);
    if (!Name) return Name.Error;
    const auto Lookup = LookupForMount(*Name);
    if (Lookup.Error) return Lookup.Error;
    if (!Lookup.Directory) return -20;
    const auto Inode = SdInode(Lookup.At.Path);
    if (VirtualDirectory(Lookup.At.Path).empty() && !InodeAllows(Inode, ModeForInode(Inode, 0040755), 1)) return -13;
    if (!NsCapableOver(*Task->Ns.User, Switch2Guest::Ns::CapSysChroot)) return -1;
    Task->Fs->Root = Lookup.At.Path;
    Task->Fs->RootMount = Lookup.At.M;
    ++IOEpoch;
    return 0;
  }

  // A new mount namespace (unshare, clone): a copy of the table. Fs is the directory state of the process that owns
  // the namespace; its root and working directory are the same places in the copy.
  void OnMountNamespaceCreated(Switch2Guest::Ns::MntNs& Created, const Switch2Guest::Ns::MntNs& From, FsState& Fs) {
    std::vector<std::pair<MountRef, MountRef>> Map;
    Created.Table = Switch2Guest::Mounts::CopyTable(TableOf(From), MountIds, Created.Owner != From.Owner, &Map);
    const auto Follow = [&](MountRef& M) {
      const auto Found = std::find_if(Map.begin(), Map.end(), [&](const auto& E) { return E.first == M; });
      if (Found != Map.end()) M = Found->second;
    };
    Follow(Fs.RootMount);
    Follow(Fs.FolderMount);
  }

  // setns: the process's root and working directory become the root of the namespace.
  void OnMountNamespaceEntered(const Switch2Guest::Ns::MntNs& Entered) {
    const auto& Table = TableOf(Entered);
    const auto Top = Table.Top();
    const auto Place = Switch2Guest::Mounts::Cross(Table, {Top, Top->Root});
    Task->Fs->Root = Task->Fs->Folder = Place.Path;
    Task->Fs->RootMount = Task->Fs->FolderMount = Place.M;
    Task->Fs->RemovedWorkingDirectory = false;
    Task->Fs->WorkingDirectory = GuestWorkingPath(Task->Fs->Folder);
  }

  // Decisions live in GuestNamespaces.h. The runtime keeps kernel ids and global process numbers; these helpers
  // translate at the system call boundary: ids through the caller's user namespace (id maps, overflow id 65534),
  // process numbers through its PID namespace (0 when the process is not visible there).

  static Switch2Guest::Ns::Subject SubjectOf(const GuestProcess& Process) {
    return {Process.Ns.User, Process.Creds.CapEffective, Process.Creds.Uid[1], Process.Creds.Gid[1]};
  }

  // ns_capable(Target, Cap) for the running process.
  bool NsCapableOver(const Switch2Guest::Ns::UserNs& Target, uint32_t Cap) const {
    return Switch2Guest::Ns::NsCapable(SubjectOf(*Task), Target, Cap);
  }

  // The fields of the credentials that follow the user namespace's id map: the kernel id that is root there.
  static void SyncCreds(GuestProcess& Process) {
    Process.Creds.InitialNs = Process.Ns.User->Level == 0;
    Process.Creds.RootUid = Switch2Guest::Ns::RootKuid(*Process.Ns.User);
  }

  // from_kuid_munged and from_kgid_munged for the running process.
  uint32_t UidOut(uint32_t Kuid) const { return Switch2Guest::Ns::MungeUid(*Task->Ns.User, Kuid); }
  uint32_t GidOut(uint32_t Kgid) const { return Switch2Guest::Ns::MungeGid(*Task->Ns.User, Kgid); }
  // make_kuid and make_kgid of a system call argument: -1 ("unchanged") passes, an unmapped id is nullopt.
  std::optional<uint32_t> KuidIn(uint32_t Uid) const {
    return Uid == Switch2Guest::NoId ? std::optional<uint32_t> {Uid} : Switch2Guest::Ns::MakeKuid(*Task->Ns.User, Uid);
  }
  std::optional<uint32_t> KgidIn(uint32_t Gid) const {
    return Gid == Switch2Guest::NoId ? std::optional<uint32_t> {Gid} : Switch2Guest::Ns::MakeKgid(*Task->Ns.User, Gid);
  }

  static constexpr uint32_t NoPid = UINT32_MAX; // matches no process or thread

  // pid_vnr: the number the running process sees for a process or thread, 0 when it cannot see it.
  uint32_t PidOut(uint32_t Global) const { return Task->Ns.Pid->ToLocal(Global); }
  // find_vpid: the global number of the process or thread the running process calls Local, NoPid when none.
  uint32_t PidIn(uint64_t Local) const {
    const auto Global = Task->Ns.Pid->ToGlobal(static_cast<uint32_t>(Local));
    return Global ? Global : NoPid;
  }
  // A thread's number in its own PID namespace: what it stores in futex words and sees from gettid.
  uint32_t OwnTid(const GuestThread& Thread) const {
    const auto Owner = Processes.find(Thread.PID);
    return Owner == Processes.end() ? Thread.TID : Owner->second.Ns.Pid->ToLocal(Thread.TID);
  }

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
    // Interrupt-page checks let the watchdog preempt CPU-bound guest code.
    Process.Context->SetPollPendingInterrupt(true);
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
      std::erase_if(Processes, [&](const auto& Item) { return Item.second.Reaped && &Item.second != Task; });
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
    FlushViewsMappedBy(Process.Memory);
    Process.Memory = {};
    if (!SharedFileViews.empty()) PruneSharedFileViews();
    Process.ForkView.reset();
    Process.InheritedImages.clear();
    Process.Image.reset();
    Process.Context.reset();
    ++IOEpoch;
  }

  GuestProcess* FindProcess(uint32_t PID) {
    const auto It = Processes.find(PID);
    return It == Processes.end() || It->second.Reaped ? nullptr : &It->second;
  }

  int64_t GetProcessGroup(uint32_t PID) {
    if (!PID) return PidOut(Task->Group);
    const auto* Process = FindProcess(PidIn(PID));
    return Process ? static_cast<int64_t>(PidOut(Process->Group)) : -3;
  }

  int64_t GetSession(uint32_t PID) {
    if (!PID) return PidOut(Task->Session);
    const auto* Process = FindProcess(PidIn(PID));
    return Process ? static_cast<int64_t>(PidOut(Process->Session)) : -3;
  }

  bool GroupInSession(uint32_t Group, uint32_t Session) const {
    return std::any_of(Processes.begin(), Processes.end(), [&](const auto& Item) {
      return !Item.second.Reaped && Item.second.Group == Group && Item.second.Session == Session;
    });
  }

  int64_t SetProcessGroup(uint64_t PIDArgument, uint64_t GroupArgument) {
    if (static_cast<int32_t>(GroupArgument) < 0) return Invalid;
    // Both arguments are numbers of the caller's PID namespace (a group that has no member there matches nothing).
    const uint32_t PID = PIDArgument ? PidIn(PIDArgument) : Task->PID;
    const uint32_t Group = GroupArgument ? PidIn(GroupArgument) : PID;
    auto* Target = FindProcess(PID);
    if (!Target) return -3;
    const SessionView Caller {Task->PID, Task->Group, Task->Session, false, false};
    const SessionView View {Target->PID, Target->Group, Target->Session, Target->ParentPID == Task->PID, Target->Image != nullptr};
    if (const auto Error = CheckSetpgid(Caller, View, GroupInSession(Group, Task->Session), Group)) return Error;
    Target->Group = Group;
    return 0;
  }

  // setsid: a new session and group led by the caller, without a
  // controlling terminal.
  int64_t SetSession() {
    const bool InUse = std::any_of(Processes.begin(), Processes.end(),
                                   [&](const auto& Item) { return !Item.second.Reaped && Item.second.Group == Task->PID; });
    if (const auto Error = CheckSetsid({Task->PID, Task->Group, Task->Session, false, false}, InUse)) return Error;
    Task->Session = Task->Group = Task->PID;
    Task->HasTerminal = false;
    return PidOut(Task->PID);
  }


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

  static constexpr uint64_t CloneVm = 0x100, CloneFs = 0x200, CloneFiles = 0x400, CloneSighand = 0x800, ClonePidfd = 0x1000,
                            ClonePtrace = 0x2000, CloneVfork = 0x4000, CloneParent = 0x8000, CloneThread_ = 0x10000,
                            CloneSysvsem = 0x40000, CloneSettls = 0x80000, CloneParentSettid = 0x100000,
                            CloneChildCleartid = 0x200000, CloneDetached = 0x400000, CloneUntraced = 0x800000,
                            CloneChildSettid = 0x1000000, CloneIo = 0x80000000ULL, CloneClearSighand = 0x100000000ULL,
                            CloneIntoCgroup = 0x200000000ULL;
  static constexpr uint64_t CloneNamespaces = 0x20000 | 0x02000000 | 0x04000000 | 0x08000000 | 0x10000000 | 0x20000000 | 0x40000000;

  // A process clone (fork, vfork, clone or clone3 without CLONE_THREAD).
  // Flags may carry the exit signal in the low byte (clone, fork, vfork).
  struct ForkRequest {
    uint64_t Flags;
    uint64_t ChildTid {};
    uint64_t ParentTid {};
    uint64_t PidfdAddress {};
    uint64_t Stack {};
    uint64_t TLS {};
    uint32_t ExitSignal {SigChld};
  };
  std::optional<ForkRequest> PendingFork;

  int64_t CloneGuest(CoreFrame* Frame, uint64_t Flags, uint64_t Stack, uint64_t ParentTid, uint64_t ChildTid, uint64_t TLS) {
    if ((Flags & ClonePidfd) && (Flags & CloneThread_)) return Invalid;
    if (Flags & CloneThread_) {
      if (!(Flags & CloneSighand)) return Invalid;
      return CloneThread(Frame, Flags, Stack, ParentTid, ChildTid, TLS);
    }
    // clone(2) passes the pidfd slot in parent_tid; it excludes PARENT_SETTID
    // and CLONE_DETACHED.
    if ((Flags & ClonePidfd) && (Flags & (CloneParentSettid | CloneDetached))) return Invalid;
    return RequestClone({Flags & ~0xffULL, ChildTid, ParentTid, Flags & ClonePidfd ? ParentTid : 0, Stack, TLS,
                         static_cast<uint32_t>(Flags & 0xff)});
  }

  // struct clone_args, CLONE_ARGS_SIZE_VER2.
  struct CloneArgs {
    uint64_t Flags, Pidfd, ChildTid, ParentTid, ExitSignal, Stack, StackSize, TLS, SetTid, SetTidSize, Cgroup;
  };
  static_assert(sizeof(CloneArgs) == 88);

  int64_t Clone3(CoreFrame* Frame, uint64_t Address, uint64_t Size) {
    if (Size < 64) return Invalid;
    if (Size > 4096) return -7; // E2BIG
    CloneArgs Args {};
    if (Available(Address, PF_R) < Size) return Fault;
    std::memcpy(&Args, reinterpret_cast<const void*>(Address), std::min<uint64_t>(Size, sizeof(Args)));
    for (uint64_t Offset = sizeof(Args); Offset < Size; ++Offset) {
      if (reinterpret_cast<const uint8_t*>(Address)[Offset]) return -7;
    }
    if ((Args.ExitSignal & ~0xffULL) || Args.ExitSignal > SigMax) return Invalid;
    if (Args.Flags & ~(~0xffULL & (0xffffffffULL | CloneClearSighand | CloneIntoCgroup))) return Invalid;
    if (Args.Flags & 0xff) return Invalid; // the exit signal has its own field
    if ((Args.Flags & CloneDetached) || ((Args.Flags & CloneClearSighand) && (Args.Flags & CloneSighand))) return Invalid;
    // No cgroups; set_tid needs PID namespaces, which the runtime lacks.
    if (Args.Flags & CloneIntoCgroup) return Invalid;
    if (Args.SetTidSize > 32 || (Args.SetTid && !Args.SetTidSize) || (!Args.SetTid && Args.SetTidSize)) return Invalid;
    if (Args.SetTidSize) {
      return Task->Creds.Capable(CapSysAdmin) || Task->Creds.Capable(CapCheckpointRestore) ? Invalid : -1;
    }
    if (!Args.Stack != !Args.StackSize) return Invalid;
    const auto Top = Args.Stack ? Args.Stack + Args.StackSize : 0;
    if ((Args.Flags & ClonePidfd) && (Args.Flags & CloneThread_)) return Invalid;
    if (Args.Flags & CloneThread_) {
      if (!(Args.Flags & CloneSighand)) return Invalid;
      return CloneThread(Frame, Args.Flags, Top, Args.ParentTid, Args.ChildTid, Args.TLS);
    }
    return RequestClone({Args.Flags, Args.ChildTid, Args.ParentTid, Args.Flags & ClonePidfd ? Args.Pidfd : 0, Top, Args.TLS,
                         static_cast<uint32_t>(Args.ExitSignal)});
  }

  // Linux copy_process checks for a process clone. Processes cannot share
  // memory (except through vfork, which copies), filesystem state or
  // descriptor tables here, and there are no namespaces: those return EINVAL
  // (namespaces EPERM without privilege). See ProcessInventory.md.
  int64_t RequestClone(ForkRequest Request) {
    auto& Flags = Request.Flags;
    if (Flags & 0xff) {
      Request.ExitSignal = Flags & 0xff;
      Flags &= ~0xffULL;
    }
    if (Request.ExitSignal > SigMax) return Invalid;
    namespace Ns = Switch2Guest::Ns;
    // copy_process: the mutual exclusions come first, then the namespace creation (EPERM without the
    // capability), then CLONE_NEWIPC with CLONE_SYSVSEM.
    if ((Flags & Ns::CloneNewNs) && (Flags & CloneFs)) return Invalid;
    if ((Flags & Ns::CloneNewUser) && (Flags & CloneFs)) return Invalid;
    if ((Flags & CloneSighand) && !(Flags & CloneVm)) return Invalid;
    if ((Flags & CloneParent) && Task->Ns.Pid->IsInitOf(Task->PID)) return Invalid; // the init of a PID namespace has no siblings
    if (Flags & CloneNamespaces) {
      // A dry run of the namespace changes: the same errors, nothing kept. (Inside a chroot CLONE_NEWUSER is EPERM.)
      const auto Plan = Ns::BuildNamespaces(NsRegistry, Task->Ns, SubjectOf(*Task), Flags & CloneNamespaces, IsChrooted());
      if (const auto* Error = std::get_if<int64_t>(&Plan)) return *Error;
      if ((Flags & Ns::CloneNewIpc) && (Flags & CloneSysvsem)) return Invalid;
    }
    // alloc_pid: a PID namespace whose init is gone (or any of its ancestors) hands out no more numbers.
    if (!(Flags & Ns::CloneNewPid)) {
      for (const Ns::PidNs* N = Task->Ns.PidForChildren.get(); N; N = N->Parent.get()) {
        if (!N->Adding) return -12;
      }
    }
    if ((Flags & CloneVm) && !(Flags & CloneVfork)) return Invalid;
    if (Flags & CloneSighand) return Invalid; // shared signal handlers between processes need a shared address space
    if ((Flags & CloneParentSettid) && Available(Request.ParentTid, PF_W) < sizeof(uint32_t)) return Fault;
    if ((Flags & (CloneChildSettid | CloneChildCleartid)) && Available(Request.ChildTid, PF_W) < sizeof(uint32_t)) return Fault;
    if ((Flags & ClonePidfd) && Available(Request.PidfdAddress, PF_W) < sizeof(int32_t)) return Fault;
    if ((Flags & CloneSettls) && !IsGuestUserRange(Request.TLS, 1)) return -1;
    if (NprocExceeded()) return -11;
    if (Processes.size() >= Threads.size()) return -11;
    if (Current->Sched.Policy == Switch2Guest::Sched::Deadline && !Current->Sched.ResetOnFork) return -11; // sched_fork: EAGAIN
    PendingFork = Request;
    return RetrySyscall;
  }

  void CompleteFork() {
    const auto Request = *PendingFork;
    PendingFork.reset();
    auto* Frame = Current->Core->CurrentFrame;
    const auto Result = ForkGuest(Frame, Request); // the child's global process number, or an error
    Frame->State.gregs[FEXCore::X86State::REG_RAX] = Result > 0 ? PidOut(static_cast<uint32_t>(Result)) : Result;
    Frame->State.rip += 2;
    if (Result > 0 && (Request.Flags & CloneVfork)) {
      // The parent sleeps until the child execs or exits (vfork_done).
      Current->Waiting = WaitKind::Vfork;
      Current->WaitAddress = Result;
    }
  }

  int64_t ForkGuest(CoreFrame* Frame, const ForkRequest& Request) {
    const auto Flags = Request.Flags, ChildTid = Request.ChildTid;
    constexpr uint64_t ChildSetTid = CloneChildSettid, ChildClearTid = CloneChildCleartid;
    ProcessCheckpoint("Fork request, saving parent memory");
    if (!Task->Memory.SavePrivatePages(ValidateSnapshotRange)) return -11;
    ProcessCheckpoint("Fork request, parent memory saved");

    // The child's namespaces: the parent's, with the CLONE_NEW* ones replaced. It lives in the parent's
    // pid_for_children (a new namespace under CLONE_NEWPID).
    namespace Ns = Switch2Guest::Ns;
    Ns::NsSet ChildNs = Task->Ns;
    std::shared_ptr<Ns::UserNs> NewUser;
    bool NewMountNamespace = false;
    if (Flags & CloneNamespaces) {
      auto Plan = Ns::BuildNamespaces(NsRegistry, Task->Ns, SubjectOf(*Task), Flags & CloneNamespaces, IsChrooted());
      if (const auto* Error = std::get_if<int64_t>(&Plan)) return *Error;
      auto& Chosen = std::get<Ns::Switch>(Plan);
      ChildNs = Chosen.Ns;
      NewUser = Chosen.NewUser;
      NewMountNamespace = Chosen.NewMnt;
      if (Flags & Ns::CloneNewIpc) ChildNs.Ipc->Store = std::make_shared<IpcStore>();
    }
    ChildNs.Pid = ChildNs.PidForChildren;

    RetireReapedPids();
    const auto PID = NextTid++;
    auto& Child = Processes.try_emplace(PID).first->second;
    Child.PID = PID;
    Child.Ns = ChildNs;
    ChildNs.Pid->Allocate(PID); // the child's number in its namespace and every ancestor
    Child.ParentPID = Flags & CloneParent ? Task->ParentPID : Task->PID;
    Child.Group = Task->Group;
    Child.Session = Task->Session;
    Child.HasTerminal = Task->HasTerminal;
    Child.ExitSignal = Flags & CloneParent ? Task->ExitSignal : Request.ExitSignal;
    Child.Dumpable = Task->Dumpable;
    Child.Nice = Task->Nice;
    Child.Personality = Task->Personality;
    Child.Name = Task->Name;
    if (Flags & CloneFs) Child.Fs = Task->Fs;
    else *Child.Fs = *Task->Fs;
    if (NewMountNamespace) OnMountNamespaceCreated(*ChildNs.Mnt, *Task->Ns.Mnt, *Child.Fs);
    Child.NoNewPrivileges = Task->NoNewPrivileges;
    Child.Limits = Task->Limits;
    Child.Creds = Task->Creds;
    Child.SupplementaryGroups = Task->SupplementaryGroups;
    if (NewUser) Ns::EnterUserNs(Child.Creds, NewUser);
    Child.Memory = Task->Memory.ForkChild();
    Child.MembarrierRegistered = Task->MembarrierRegistered;
    // Segments stay attached in the child. The undo list is shared only under CLONE_SYSVSEM.
    Child.ShmAttached = Task->ShmAttached;
    if (Flags & 0x40000) Child.SemUndo = Task->SemUndo;
    Child.GDT = Task->GDT;
    Child.ForkView = Task->ForkView;
    Child.RestorePending = true;
    if (Flags & CloneFiles) Child.PendingDescriptors = Current->Descriptors; // one table, two processes
    else *Child.PendingDescriptors = Descriptors();
    Child.SignalActions = Task->SignalActions;
    if (Flags & CloneClearSighand) {
      for (auto& Action : Child.SignalActions) {
        if (Action[0] != SigIgnore) Action = {};
      }
    }
    Child.HeapStart = Task->HeapStart;
    Child.HeapEnd = Task->HeapEnd;
    // Keep the source process's ELF backing alive even if it execs or exits.
    Child.InheritedImages = Task->InheritedImages;
    Child.ExecutableInode = Task->ExecutableInode;
    Child.ExecutablePath = Task->ExecutablePath;
    if (Task->Image) Child.InheritedImages.push_back(Task->Image);
    FEXCore::Core::CPUState State {};
    std::memcpy(&State, &Frame->State, sizeof(State));
    State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = Child.GDT.data();
    State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = Child.GDT.data();
    State.rip += 2;
    State.gregs[FEXCore::X86State::REG_RAX] = 0;
    if (Request.Stack) State.gregs[FEXCore::X86State::REG_RSP] = Request.Stack;
    if (Flags & CloneSettls) State.fs_cached = Request.TLS;
    ProcessCheckpoint("Fork request, creating child FEX context");
    if (!InitializeProcessContext(Child) ||
        !AddThread(*Child.Context, State, PID, Flags & ChildClearTid ? ChildTid : 0, PID)) {
      ChildNs.Pid->Release(PID);
      Processes.erase(PID);
      return -11;
    }
    const auto Thread = std::find_if(Threads.begin(), Threads.end(), [PID](const auto& Entry) { return Entry.TID == PID; });
    if (Flags & ChildSetTid) Thread->SetTidOnStart = ChildTid;
    Thread->AltStack = Current->AltStack; // fork keeps sigaltstack; new threads do not
    Switch2Guest::Sched::ForkInherit(Thread->Sched, Child.Nice);
    if (Thread->IoContext && !(Flags & CloneIo)) Thread->IoContext = ++NextIoContext;
    if (Flags & CloneParentSettid) CopyOut(Request.ParentTid, PidOut(PID)); // the number in the parent's namespace
    if (Flags & ClonePidfd) {
      // Installed after the child copied the descriptor table, as in Linux.
      const auto Pidfd = OpenPidfd(PID, 0);
      if (Pidfd < 0) {
        Thread->Core->CTX->DestroyThread(Thread->Core);
        *Thread = {};
        ChildNs.Pid->Release(PID);
        Processes.erase(PID); // owns the context the thread used
        return Pidfd;
      }
      CopyOut(Request.PidfdAddress, static_cast<int32_t>(Pidfd));
    }
    Child.TraceFirstDispatch = true;
    Child.StartupSyscallCheckpoints = Logging == LogLevel::Trace ? 6 : 0;
    ProcessCheckpoint("Fork request, child ready");
    return PID;
  }

  static uint64_t TicksToMicroseconds(uint64_t Ticks) { return armTicksToNs(Ticks) / 1000; }

  // struct rusage: only ru_utime is measured (guest CPU time); the
  // cooperative runtime has no system time, page faults or context switches.
  int64_t CopyUsage(uint64_t Address, uint64_t Ticks) {
    std::array<uint64_t, 18> Usage {};
    const auto Microseconds = TicksToMicroseconds(Ticks);
    Usage[0] = Microseconds / 1'000'000;
    Usage[1] = Microseconds % 1'000'000;
    return CopyOut(Address, Usage);
  }

  struct WaitReport {
    uint32_t PID, Uid, Status;
    int32_t Code, Value; // si_code and si_status
    uint64_t Ticks;      // the child's CPU time plus its waited children's
  };

  // Linux do_wait for wait4 and waitid. Reports one exited, stopped or
  // continued child matching Selector; reaps exited children unless WNOWAIT.
  std::variant<int64_t, WaitReport> WaitGeneric(WaitSelector Selector, uint32_t Options, bool IsWaitId) {
    bool HasChild {};
    for (auto It = Processes.begin(); It != Processes.end(); ++It) {
      auto& Child = It->second;
      if (Child.Reaped || Child.ParentPID != Task->PID) continue;
      if (Selector.Type == WaitIdType::Pid && Child.PID != Selector.Id) continue;
      if (Selector.Type == WaitIdType::Pgid && Child.Group != Selector.Id) continue;
      // __WCLONE selects children whose exit signal is not SIGCHLD; __WALL both.
      if (!(Options & WAll) && ((Options & WClone) != 0) != (Child.ExitSignal != SigChld)) continue;
      HasChild = true;
      const bool Keep = Options & WNoWait;
      if (Child.Exited && (!IsWaitId || (Options & WExited))) {
        const WaitReport Report {Child.PID, Child.Creds.Uid[0],
                                 Child.TerminatingSignal ? WaitSignaledStatus(Child.TerminatingSignal) : WaitExitedStatus(Child.Status),
                                 Child.TerminatingSignal ? CldKilled : CldExited,
                                 static_cast<int32_t>(Child.TerminatingSignal ? Child.TerminatingSignal : Child.Status),
                                 Child.CpuTicks + Child.ChildTicks};
        if (!Keep) {
          Task->ChildTicks += Child.CpuTicks + Child.ChildTicks;
          Processes.erase(It);
        }
        return Report;
      }
      if (Child.Stopped && Child.ReportStop && (Options & WUntraced)) {
        if (!Keep) Child.ReportStop = false;
        return WaitReport {Child.PID, Child.Creds.Uid[0], WaitStoppedStatus(Child.StopSignal), CldStopped,
                           static_cast<int32_t>(Child.StopSignal), Child.CpuTicks + Child.ChildTicks};
      }
      if (Child.ReportContinue && (Options & WContinued)) {
        if (!Keep) Child.ReportContinue = false;
        return WaitReport {Child.PID, Child.Creds.Uid[0], WaitContinuedStatus, CldContinued, static_cast<int32_t>(SigCont),
                           Child.CpuTicks + Child.ChildTicks};
      }
    }
    if (!HasChild) return int64_t {-10};
    if (Options & WNoHang) return int64_t {0};
    return WaitForIO();
  }

  int64_t Wait4(uint64_t PID, uint64_t Status, uint64_t Options, uint64_t Usage) {
    if (const auto Error = ValidateWait4Options(Options)) return Error;
    if (static_cast<int32_t>(PID) == INT32_MIN) return -3;
    if (Status && Available(Status, PF_W) < 4) return Fault;
    if (Usage && Available(Usage, PF_W) < 144) return Fault;
    // wait4 always reports exited children.
    auto Selector = SelectorFromWait4(static_cast<int32_t>(PID), Task->Group);
    // A pid or group number names a process of the caller's PID namespace (the caller's own group is global).
    if (Selector.Type == WaitIdType::Pid || (Selector.Type == WaitIdType::Pgid && static_cast<int32_t>(PID) != 0)) {
      Selector.Id = PidIn(Selector.Id);
    }
    const auto Result = WaitGeneric(Selector, Options | WExited, false);
    if (const auto* Error = std::get_if<int64_t>(&Result)) return *Error;
    const auto& Report = std::get<WaitReport>(Result);
    if (Status) CopyOut(Status, Report.Status);
    if (Usage) CopyUsage(Usage, Report.Ticks);
    return PidOut(Report.PID);
  }

  // waitid writes si_signo, si_errno, si_code, si_pid, si_uid and si_status.
  static constexpr size_t WaitIdInfoBytes = offsetof(SigInfo, Child) + 12;
  int64_t WaitId(uint64_t Type, uint64_t ID, uint64_t InfoAddress, uint64_t Options, uint64_t Usage) {
    if (const auto Error = ValidateWaitIdOptions(Options)) return Error;
    WaitSelector Selector {};
    switch (Type) {
    case 0: Selector = {WaitIdType::All, 0}; break;
    case 1:
      if (static_cast<int32_t>(ID) <= 0) return Invalid;
      Selector = {WaitIdType::Pid, PidIn(ID)};
      break;
    case 2:
      if (static_cast<int32_t>(ID) < 0) return Invalid;
      Selector = {WaitIdType::Pgid, ID ? PidIn(ID) : Task->Group};
      break;
    case 3: {
      const auto* File = GetFile(ID);
      if (!File || !File->PidfdTarget) return BadFile;
      Selector = {WaitIdType::Pid, File->PidfdTarget};
      break;
    }
    default: return Invalid;
    }
    if (InfoAddress && Available(InfoAddress, PF_W) < sizeof(SigInfo)) return Fault;
    if (Usage && Available(Usage, PF_W) < 144) return Fault;
    const auto Result = WaitGeneric(Selector, Options, true);
    if (const auto* Error = std::get_if<int64_t>(&Result)) {
      // WNOHANG with nothing to report clears the reported fields.
      if (*Error == 0 && InfoAddress) std::memset(reinterpret_cast<void*>(InfoAddress), 0, WaitIdInfoBytes);
      return *Error;
    }
    const auto& Report = std::get<WaitReport>(Result);
    if (InfoAddress) {
      SigInfo Info = MakeSigInfo(SigChld, Report.Code, PidOut(Report.PID), UidOut(Report.Uid));
      Info.Child.Status = Report.Value;
      std::memcpy(reinterpret_cast<void*>(InfoAddress), &Info, WaitIdInfoBytes);
    }
    if (Usage) CopyUsage(Usage, Report.Ticks);
    return 0;
  }

  // Linux do_notify_parent / do_notify_parent_cldstop. Returns true when the
  // exited child must be reaped at once (SIGCHLD ignored or SA_NOCLDWAIT).
  bool NotifyParent(GuestProcess& Child, int32_t Code, int32_t Value) {
    auto* Parent = FindProcess(Child.ParentPID);
    if (!Parent || Parent->Exited) return Code == CldExited || Code == CldKilled;
    const auto& Action = Parent->SignalActions[SigChld - 1];
    uint32_t Signal = SigChld;
    bool AutoReap {};
    if (Code == CldStopped || Code == CldContinued) {
      if (Action[1] & SaNoCldStop) return false;
    } else {
      Signal = Child.ExitSignal;
      if (Signal == SigChld && (Action[0] == SigIgnore || (Action[1] & SaNoCldWait))) {
        AutoReap = true;
        if (Action[0] == SigIgnore) Signal = 0;
      }
    }
    if (Signal && Signal <= SigMax) {
      auto Info = MakeSigInfo(Signal, Code, Child.PID, Child.Creds.Uid[0]);
      Info.Child.Status = Value;
      Info.Child.Utime = static_cast<int64_t>(TicksToMicroseconds(Child.CpuTicks) / 10'000); // clock ticks
      SendSignal(*Parent, nullptr, Info);
    }
    ++IOEpoch;
    return AutoReap;
  }

  // The nearest living ancestor marked as child subreaper, else the
  // runtime's virtual init (pid 1), which reaps at once.
  uint32_t ReaperFor(const GuestProcess& Process) {
    for (auto PID = Process.ParentPID; PID > 1;) {
      const auto* Ancestor = FindProcess(PID);
      if (!Ancestor) break;
      // find_new_reaper looks only through ancestors in the same PID namespace.
      if (Ancestor->Ns.Pid != Process.Ns.Pid) break;
      if (Ancestor->ChildSubreaper && !Ancestor->Exited) return PID;
      PID = Ancestor->ParentPID;
    }
    // Then the namespace's init (pid 1 there), else the runtime's virtual init.
    const auto& PidNs = *Process.Ns.Pid;
    if (PidNs.Level > 0 && PidNs.Init != Process.PID) {
      if (const auto* Init = FindProcess(PidNs.Init); Init && !Init->Exited) return PidNs.Init;
    }
    return 1;
  }

  // zap_pid_ns_processes: the init of a PID namespace is exiting. The namespace takes no more numbers and every
  // process in it (and in namespaces below) gets SIGKILL. Linux holds the init's exit until they are all
  // reaped; here its parent is told at once (see ContainerInventory.md).
  void ZapPidNamespace(const GuestProcess& Init) {
    auto& PidNs = *Init.Ns.Pid;
    PidNs.Adding = false;
    for (auto& [ID, Member] : Processes) {
      if (Member.Reaped || Member.Exited || &Member == &Init || !PidNs.Contains(*Member.Ns.Pid)) continue;
      SendSignal(Member, nullptr, MakeSigInfo(SigKill, SiKernel));
    }
  }

  // Linux will_become_orphaned_pgrp: no living member has a living parent
  // in another group of the same session.
  bool IsOrphanedGroup(uint32_t Group) {
    for (const auto& [ID, Member] : Processes) {
      if (Member.Reaped || Member.Exited || Member.Group != Group) continue;
      const auto* Parent = FindProcess(Member.ParentPID);
      if (Parent && !Parent->Exited && Parent->Group != Group && Parent->Session == Member.Session) return false;
    }
    return true;
  }

  bool GroupHasStopped(uint32_t Group) const {
    return std::any_of(Processes.begin(), Processes.end(), [&](const auto& Item) {
      return !Item.second.Reaped && !Item.second.Exited && Item.second.Group == Group && Item.second.Stopped;
    });
  }

  // kill_pgrp with kernel privilege.
  void SignalGroup(uint32_t Group, uint32_t Signal) {
    for (auto& [ID, Member] : Processes) {
      if (!Member.Reaped && !Member.Exited && Member.Group == Group) SendSignal(Member, nullptr, MakeSigInfo(Signal, SiKernel));
    }
  }

  // kill_orphaned_pgrp: a group that just became orphaned and has stopped
  // members gets SIGHUP then SIGCONT.
  void HangUpOrphanedGroup(uint32_t Group) {
    if (!IsOrphanedGroup(Group) || !GroupHasStopped(Group)) return;
    SignalGroup(Group, SigHup);
    SignalGroup(Group, SigCont);
  }

  // Linux do_exit/exit_notify for the current process: ends every thread,
  // reparents children, hangs up newly orphaned stopped groups, gives up a
  // controlling terminal it leads and notifies the parent.
  void ExitProcess(uint32_t Status, uint32_t Signal) {
    auto& Process = *Task;
    Process.Status = Status & 0xff;
    Process.TerminatingSignal = Signal;
    Process.Exited = true;
    Process.Stopped = false;
    if (Process.Ns.Pid->IsInitOf(Process.PID)) ZapPidNamespace(Process);
    if (!Current->Exited) ExitThread();
    ExitSysvIpc(Process);
    for (auto& Thread : Threads) {
      if (!Thread.Core || Thread.Exited || Thread.PID != Process.PID) continue;
      ReleaseThreadMemory(Thread); // robust futexes and clear_child_tid, as do_exit does per thread
      Thread.Exited = true;
      Thread.Descriptors.reset();
      Thread.PendingFifo.reset();
    }
    if (Process.Session == Process.PID && TerminalSession == Process.Session) {
      if (ForegroundGroup) SignalGroup(ForegroundGroup, SigHup);
      for (auto& [ID, Member] : Processes) {
        if (Member.Session == Process.Session) Member.HasTerminal = false;
      }
      TerminalSession = 0;
      ForegroundGroup = 0;
    }
    const auto Reaper = ReaperFor(Process);
    for (auto& [ID, Child] : Processes) {
      if (Child.Reaped || Child.ParentPID != Process.PID) continue;
      Child.ParentPID = Reaper;
      Child.ExitSignal = SigChld; // reparent_leader: the new parent gets SIGCHLD
      if (Child.ParentDeathSignal && !Child.Exited) {
        SendSignal(Child, nullptr, MakeSigInfo(Child.ParentDeathSignal, SiUser, Process.PID, Process.Creds.Uid[0]));
      }
      if (Child.Exited) {
        if (Reaper == 1 || NotifyParent(Child, Child.TerminatingSignal ? CldKilled : CldExited,
                                        Child.TerminatingSignal ? Child.TerminatingSignal : Child.Status)) {
          Child.Reaped = true; // Zombies were retired when they exited.
        }
      } else if (Child.Group != Process.Group && Child.Session == Process.Session) {
        HangUpOrphanedGroup(Child.Group);
      }
    }
    if (const auto* Parent = FindProcess(Process.ParentPID);
        Parent && Parent->Group != Process.Group && Parent->Session == Process.Session) {
      HangUpOrphanedGroup(Process.Group);
    }
    if (Process.PID == ProcessID) {
      Number = LinuxExitSyscall;
      ExitStatus = Signal ? 128 + Signal : Process.Status;
      FlushAllTmpfs();
      ProcessExited = true;
    } else {
      Process.AutoReap = Process.ParentPID == 1 ||
        NotifyParent(Process, Signal ? CldKilled : CldExited, static_cast<int32_t>(Signal ? Signal : Process.Status));
    }
    ++IOEpoch;
  }

  // A reaped process has no pid any more (free_pid): its numbers leave the namespaces' maps. This runs before the
  // next fork rather than at the reap, because wait still reports the child's number after reaping it.
  void RetireReapedPids() {
    for (auto& [PID, Process] : Processes) {
      if (!Process.Reaped || Process.PidRetired) continue;
      Process.PidRetired = true;
      Process.Ns.Pid->Retire(PID);
    }
  }

  // Default stop action (do_signal_stop) for the current process.
  void StopProcess(GuestProcess& Process, uint32_t Signal) {
    Process.Stopped = true;
    Process.StopSignal = Signal;
    Process.ReportStop = true;
    Process.ReportContinue = false;
    NotifyParent(Process, CldStopped, static_cast<int32_t>(Signal));
    ++IOEpoch;
  }

  // SIGCONT generation resumes a stopped process (prepare_signal).
  void ContinueProcess(GuestProcess& Process) {
    if (!Process.Stopped) return;
    Process.Stopped = false;
    Process.ReportStop = false;
    Process.ReportContinue = true;
    NotifyParent(Process, CldContinued, static_cast<int32_t>(SigCont));
    ++IOEpoch;
  }

  int64_t OpenPidfd(uint32_t PID, uint32_t Flags) {
    const auto FD = FreeDescriptor();
    if (FD == NoDescriptor) return -24;
    auto File = std::make_shared<OpenFile>();
    File->PidfdTarget = PID;
    File->StatusFlags = 2 | (Flags & 04000);
    File->Path = "anon_inode:[pidfd]";
    File->Inode = NextAnonymousInode++;
    Descriptors().Files[FD] = std::move(File);
    Descriptors().Flags[FD] = 1; // pidfds are always close-on-exec
    return FD;
  }

  int64_t PidfdOpen(uint64_t PID, uint64_t Flags) {
    if (Flags & ~uint64_t {04000} || static_cast<int32_t>(PID) <= 0) return Invalid;
    const auto Global = PidIn(PID);
    if (!FindProcess(Global)) return ProcessOfId(Global) ? Invalid : -3; // a non-leader thread: EINVAL
    return OpenPidfd(Global, Flags);
  }

  // Flags: PIDFD_SIGNAL_THREAD 1, PIDFD_SIGNAL_THREAD_GROUP 2,
  // PIDFD_SIGNAL_PROCESS_GROUP 4; at most one.
  int64_t PidfdSendSignal(uint64_t FD, uint64_t Signal, uint64_t InfoAddress, uint64_t Flags) {
    if ((Flags & ~uint64_t {7}) || std::popcount(Flags) > 1) return Invalid;
    const auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (!File->PidfdTarget) return Invalid;
    auto* Target = FindProcess(File->PidfdTarget);
    if (!Target) return -3;
    SigInfo Info = MakeSigInfo(Signal, SiUser, Task->PID, Task->Creds.Uid[0]);
    if (InfoAddress) {
      if (Available(InfoAddress, PF_R) < sizeof(Info)) return Fault;
      std::memcpy(&Info, reinterpret_cast<const void*>(InfoAddress), sizeof(Info));
      if (static_cast<uint32_t>(Info.Signo) != Signal) return Invalid;
      if (Target != Task && (Info.Code >= 0 || Info.Code == SiTkill)) return -1;
    }
    if (Flags == 1) {
      // A process pidfd names its leader thread.
      auto* Leader = LeaderThread(Target->PID);
      return Leader ? SignalOne(*Target, Leader, Signal, Info) : -3;
    }
    if (Flags == 4) return SignalProcessGroup(Target->Group, Signal, Info);
    return SignalOne(*Target, nullptr, Signal, Info);
  }

  // pidfd_getfd. Linux requires PTRACE_MODE_ATTACH_REALCREDS; without ptrace
  // this is "same real and effective uid as the target, or root".
  int64_t PidfdGetFd(uint64_t FD, uint64_t TargetFD, uint64_t Flags) {
    if (Flags) return Invalid;
    const auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (!File->PidfdTarget) return BadFile;
    const auto* Target = FindProcess(File->PidfdTarget);
    if (!Target || Target->Exited) return -3;
    const auto& Mine = Task->Creds.Uid;
    if (!Task->Creds.Capable(CapSysPtrace) && (Target->Creds.Uid[0] != Mine[0] || Target->Creds.Uid[1] != Mine[0])) return -1;
    const GuestThread* Owner = LeaderThread(Target->PID);
    if (!Owner || !Owner->Descriptors || TargetFD >= Owner->Descriptors->Files.size()) return BadFile;
    auto Shared = Owner->Descriptors->Files[TargetFD];
    if (!Shared) return BadFile;
    const auto New = FreeDescriptor();
    if (New == NoDescriptor) return -24;
    Descriptors().Files[New] = std::move(Shared);
    Descriptors().Flags[New] = 1;
    return New;
  }

  int64_t ResourceUsage(uint64_t Who, uint64_t Address) {
    uint64_t Ticks {};
    switch (static_cast<int32_t>(Who)) {
    case 0: Ticks = ProcessTicks(*Task); break;  // RUSAGE_SELF
    case -1: Ticks = Task->ChildTicks; break;    // RUSAGE_CHILDREN
    case 1: Ticks = ThreadTicks(*Current); break; // RUSAGE_THREAD
    default: return Invalid;
    }
    return CopyUsage(Address, Ticks) ? Fault : 0;
  }

  // times(2): clock ticks at USER_HZ 100; the result counts ticks since boot.
  int64_t ProcessTimes(uint64_t Address) {
    const auto Ticks = [](uint64_t CpuTicks) { return static_cast<int64_t>(armTicksToNs(CpuTicks) / 10'000'000); };
    const int64_t Times[4] {Ticks(ProcessTicks(*Task)), 0, Ticks(Task->ChildTicks), 0};
    if (Address && CopyOut(Address, Times)) return Fault;
    return static_cast<int64_t>(MonotonicNs() / 10'000'000);
  }

  // personality(2): the value is stored and reported; ADDR_NO_RANDOMIZE and
  // the other flags change nothing because the runtime has no ASLR.
  int64_t SetPersonality(uint64_t Value) {
    const auto Old = Task->Personality;
    if (static_cast<uint32_t>(Value) != UINT32_MAX) Task->Personality = static_cast<uint32_t>(Value);
    return Old;
  }

  // getpriority/setpriority. Nice values are stored and reported; the
  // cooperative scheduler does not weight threads by them.
  template<typename F>
  int64_t ForPriorityTargets(uint64_t Which, uint64_t Who, F&& Visit) {
    if (Which > 2) return Invalid;
    const auto ID = static_cast<uint32_t>(Who);
    // A pid or group is a number of the caller's PID namespace and a user an id of its user namespace.
    const auto User = Which == 2 ? (Who ? KuidIn(ID) : std::optional<uint32_t> {Task->Creds.Uid[0]}) : std::optional<uint32_t> {0};
    if (!User) return -3;
    bool Found {};
    for (auto& [PID, Process] : Processes) {
      if (Process.Reaped || Process.Exited) continue;
      const bool Match = Which == 0 ? Process.PID == (ID ? PidIn(ID) : Task->PID)
                       : Which == 1 ? Process.Group == (ID ? PidIn(ID) : Task->Group)
                                    : Process.Creds.Uid[0] == *User;
      if (!Match) continue;
      Found = true;
      Visit(Process);
    }
    return Found ? 0 : -3;
  }

  int64_t GetPriority(uint64_t Which, uint64_t Who) {
    int32_t Best = 20;
    const auto Result = ForPriorityTargets(Which, Who, [&](GuestProcess& Process) { Best = std::min(Best, Process.Nice); });
    return Result ? Result : 20 - Best;
  }

  int64_t SetPriority(uint64_t Which, uint64_t Who, uint64_t Value) {
    const auto Nice = std::clamp(static_cast<int32_t>(Value), -20, 19);
    int64_t Error = -3;
    const auto Result = ForPriorityTargets(Which, Who, [&](GuestProcess& Process) {
      const auto& Mine = Task->Creds.Uid;
      const bool Nicer = Task->Creds.Capable(Switch2Guest::CapSysNice);
      if (!Nicer && Process.Creds.Uid[0] != Mine[1] && Process.Creds.Uid[1] != Mine[1]) {
        Error = -1;
      } else if (Nice < Process.Nice && !Nicer) {
        Error = -13; // RLIMIT_NICE is 0: lowering needs CAP_SYS_NICE.
      } else {
        if (Error == -3) Error = 0;
        Process.Nice = Nice;
      }
    });
    return Result ? Result : Error;
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

  // execve and execveat (AT_EMPTY_PATH 0x1000, AT_SYMLINK_NOFOLLOW 0x100).
  int64_t ExecGuest(uint64_t Directory, uint64_t PathAddress, uint64_t Argv, uint64_t Envp, uint64_t Flags) {
    if (Flags & ~uint64_t {0x1100}) return Invalid;
    const auto Text = GuestString(PathAddress);
    if (!Text) return Text.Error;
    std::string Name {*Text};
    std::string Path;
    LinuxStat Info {};
    // The name a script's interpreter receives, as Linux builds it: a
    // descriptor-relative exec names the file through /dev/fd.
    const auto DescriptorNumber = static_cast<int32_t>(Directory);
    const bool ThroughDescriptor = DescriptorNumber != -100 && (Name.empty() || Name.front() != '/');
    std::string ScriptName = !ThroughDescriptor ? Name :
                             "/dev/fd/" + std::to_string(DescriptorNumber) + (Name.empty() ? "" : "/" + Name);
    const bool ScriptNameInaccessible = ThroughDescriptor && DescriptorNumber >= 0 &&
      static_cast<size_t>(DescriptorNumber) < Descriptors().Flags.size() && Descriptors().Flags[DescriptorNumber];
    if (Name.empty() && (Flags & 0x1000)) {
      const auto* File = GetFile(Directory);
      if (!File) return BadFile;
      // Descriptors opened by path keep the SD path; the guest name drops the root.
      std::string_view Opened = File->Path;
      if (Opened.starts_with(RootPath)) Opened.remove_prefix(RootPath.size());
      if (Opened.empty() || Opened.front() != '/') return -13;
      Name = Opened;
      Directory = CurrentDirectory;
    }
    ExecImage Request;
    Request.Path = Name;
    size_t Budget = 128 * 1024;
    if (const auto Result = CopyExecVector(Argv, Request.Arguments, Budget); Result < 0) return Result;
    if (const auto Result = CopyExecVector(Envp, Request.Environment, Budget); Result < 0) return Result;

    // binfmt_script: a "#!" file runs its interpreter, which may itself be a
    // script, up to ScriptMaxRewrites times.
    std::string Program = Name;
    bool NosuidMount = false; // bprm_fill_uid: set-id bits of a file on a nosuid mount are ignored
    bool FollowLinks = !(Flags & 0x100);
    for (int Rewrites = 0;; ++Rewrites) {
      if (const auto Status = PathStat(Directory, Program, Info, &Path, FollowLinks); Status < 0) return Status;
      if ((Info.Mode & 0170000u) == 0120000u) return -40; // ELOOP: AT_SYMLINK_NOFOLLOW on a link
      if ((Info.Mode & 0170000u) != 0100000u || !InodeAllows(Info.Inode, Info.Mode, 1)) return -13;
      if (StatMount && (StatMount->Flags & Switch2Guest::Mounts::MntNoexec)) return -13; // do_open_execat: path_noexec
      NosuidMount = StatMount && (StatMount->Flags & Switch2Guest::Mounts::MntNosuid);
      if (ExecutableHasWriter(Info.Inode)) return -26;
      std::array<char, ScriptHeadBytes> Head {};
      const auto HeadSize = ReadFileHead(FileBackingPath(Path).c_str(), Head, Info.Size, FileExtents(Info.Inode));
      if (HeadSize < 0) return HeadSize;
      ScriptLine Line;
      const auto Kind = ParseScriptLine({Head.data(), static_cast<size_t>(HeadSize)}, Line);
      if (Kind == ScriptParse::NotScript) break;
      if (Kind == ScriptParse::Invalid) return -8;
      // The interpreter could not open a close-on-exec descriptor's script.
      if (Rewrites == 0 && ScriptNameInaccessible) return -2;
      if (Rewrites == ScriptMaxRewrites) return -40;
      const size_t Added = Line.Interpreter.size() + 1 + (Line.Argument ? Line.Argument->size() + 1 : 0) + ScriptName.size() + 1;
      if (Added > Budget) return -7;
      Budget -= Added;
      Request.Arguments = ScriptArguments(Line, ScriptName, Request.Arguments);
      ScriptName = Program = Line.Interpreter;
      Directory = CurrentDirectory;
      FollowLinks = true;
    }

    // Credentials and /proc/self/exe follow the ELF that runs; Linux ignores
    // set-user-ID bits on scripts.
    Request.Inode = Info.Inode;
    Request.ProgramPath = Program;
    Request.ResolvedPath = Path;
    // bprm_fill_uid: set-id bits are ignored when the user namespace has no mapping for the file's owner or group.
    if (!Task->NoNewPrivileges && !NosuidMount && InodeMapped(Info.Inode, true)) {
      if (Info.Mode & 04000) Request.EffectiveUID = Info.Uid;
      if ((Info.Mode & 02010) == 02010) Request.EffectiveGID = Info.Gid;
    }
    if (ReadElf(FileBackingPath(Path).c_str(), Request.Program, Info.Size, FileExtents(Info.Inode))) return -8;
    if (!Request.Program.Interpreter.empty()) {
      LinuxStat InterpreterInfo {};
      std::string Interpreter;
      if (const auto Error = PathStat(CurrentDirectory, Request.Program.Interpreter, InterpreterInfo, &Interpreter); Error) {
        return Error;
      }
      if ((InterpreterInfo.Mode & 0170000) != 0100000 || Interpreter.empty()) return -13;
      if (StatMount && (StatMount->Flags & Switch2Guest::Mounts::MntNoexec)) return -13;
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
    auto Pending = std::move(Current->Pending);
    const auto KeptSched = Current->Sched;
    const auto KeptCpuMask = Current->CpuMask;
    const auto KeptIoPriority = Current->IoPriority;
    const auto KeptIoContext = Current->IoContext;
    const auto KeptSeccomp = Current->Seccomp; // filters survive execve
    // unshare_files at exec: a descriptor table shared with another process (CLONE_FILES) becomes the
    // caller's own before the close-on-exec descriptors go, so the other process keeps them.
    const auto SharedWithOther = [&] {
      for (const auto& Thread : Threads) {
        if (Thread.Core && Thread.PID != PID && Thread.Descriptors == Current->Descriptors) return true;
      }
      for (const auto& [OtherPID, Other] : Processes) {
        if (OtherPID != PID && Other.PendingDescriptors == Current->Descriptors) return true;
      }
      return false;
    };
    Task->PendingDescriptors = SharedWithOther() ? std::make_shared<DescriptorState>(*Current->Descriptors) : Current->Descriptors;
    // exec_mm_release: every thread's robust futexes and clear_child_tid run against the old image.
    for (auto& Thread : Threads) {
      if (Thread.Core && !Thread.Exited && Thread.PID == PID) ReleaseThreadMemory(Thread);
    }

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
    FlushViewsMappedBy(Task->Memory);
    Task->Memory = {};
    Task->LockFuture = false;
    Task->MembarrierRegistered = 0;
    Task->ShmAttached.clear(); // exec detaches every segment; SEM_UNDO adjustments survive it
    Task->ForkView = std::make_shared<const uint8_t>(0);
    Task->RestorePending = false;
    Task->HeapStart = Task->HeapEnd = 0;
    Task->Image = std::make_shared<ExecImage>(std::move(*Task->PendingExec));
    Task->ExecutableInode = Task->Image->Inode;
    Task->ExecutablePath = Task->Image->ResolvedPath;
    {
      const auto& Path = Task->Image->Path;
      const auto Base = Path.substr(Path.find_last_of('/') == std::string::npos ? 0 : Path.find_last_of('/') + 1);
      Task->Name = {};
      std::memcpy(Task->Name.data(), Base.data(), std::min<size_t>(Base.size(), Task->Name.size() - 1));
    }
    Task->PendingExec.reset();
    Task->InheritedImages.clear();
    for (size_t FD = 0; FD < Descriptors().Files.size(); ++FD) {
      if (Descriptors().Flags[FD]) CloseGuest(FD);
    }
    for (auto& Action : Task->SignalActions) {
      if (Action[0] != 1) Action = {};
    }
    DeleteProcessTimers(*Task);
    ++IOEpoch;

    const auto& Image = *Task->Image;
    if (Image.EffectiveUID || Image.EffectiveGID) Task->ParentDeathSignal = 0;
    SyncCreds(*Task);
    Switch2Guest::ApplyExecCaps(Task->Creds, Image.EffectiveUID, Task->NoNewPrivileges, false, Image.EffectiveGID);
    Task->Creds.Uid[2] = Task->Creds.Uid[3] = Task->Creds.Uid[1];
    Task->Creds.Gid[2] = Task->Creds.Gid[3] = Task->Creds.Gid[1];
    for (const auto* ELF : {&Image.Program, &Image.Interpreter}) {
      for (const auto& Segment : ELF->GetSegments()) {
        Task->Memory.Register(Segment.Start, Segment.Size, Segment.Flags);
        RecordMapping(Segment.Start, Segment.Size, Segment.Start - ELF->Base,
                      ELF == &Image.Program ? Image.ProgramPath : Image.Program.Interpreter);
      }
    }
    State.rip = Image.Interpreter.Entry ? Image.Interpreter.Entry : Image.Program.Entry;
    const auto Base = CreateStack(MainStackSize);
    if (!Base) return false;
    State.gregs[FEXCore::X86State::REG_RSP] =
      BuildInitialStack(Base, Image.Path, Image.Arguments, Image.Environment, &Image.Program, Image.Interpreter.Base,
                        Switch2Guest::AuxvIds(Task->Creds));
    if (!AddThread(*Task->Context, State, PID, 0, PID)) return false;
    const auto Thread = std::find_if(Threads.begin(), Threads.end(), [PID](const auto& Entry) { return Entry.TID == PID; });
    Thread->SignalMask = Mask;
    Thread->Pending = std::move(Pending);
    Thread->Sched = KeptSched; // exec keeps the scheduling policy, CPU mask and I/O priority
    Thread->CpuMask = KeptCpuMask;
    Thread->IoPriority = KeptIoPriority;
    Thread->IoContext = KeptIoContext;
    Thread->Seccomp = KeptSeccomp;
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

  // File times use the guest realtime clock, so they agree with clock_gettime(CLOCK_REALTIME).
  FileTimestamp WallClock() const {
    const auto Now = RealTimeNs();
    return {static_cast<int64_t>(Now / 1'000'000'000), static_cast<int64_t>(Now % 1'000'000'000)};
  }

  void ImportTimestamps(FsFileSystem& SdCard, const std::string& Path, uint64_t Inode) {
    auto& Times = Inodes[Inode].Times;
    if (Times) return;
    FsTimeStampRaw Raw {};
    if (R_SUCCEEDED(SdRetry(fsFsGetFileTimeStampRaw, &SdCard, Path.c_str(), &Raw)) && Raw.is_valid) {
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

  int64_t FillSharedFileView(OpenFile& File, void* Storage, size_t Size, uint64_t Offset) {
    // Page-cache coherence does not access the file, so atime stays.
    const auto Flags = File.StatusFlags;
    File.StatusFlags |= NoAtimeFlag;
    const auto Read = ReadFileAt(File, Storage, Size, Offset);
    File.StatusFlags = Flags;
    if (Read < 0) return Read;
    // Bytes past EOF read as zero (Linux would raise SIGBUS past the last page).
    std::memset(static_cast<uint8_t*>(Storage) + Read, 0, Size - Read);
    return 0;
  }

  // Drops views whose mappings are gone, releasing their open files.
  void PruneSharedFileViews() {
    std::erase_if(SharedFileViews, [](const auto& Entry) { return Entry.second.Storage.expired(); });
  }

  // Refreshes every view of the file after its data changed. A writable
  // view keeps the pages it has stored to (those differ from Snapshot).
  void RefreshSharedFileViews(uint64_t Inode) {
    if (ViewSyncDepth) return;
    ++ViewSyncDepth;
    const auto [First, Last] = SharedFileViews.equal_range(Inode);
    for (auto It = First; It != Last;) {
      const auto Storage = It->second.Storage.lock();
      if (!Storage) {
        It = SharedFileViews.erase(It);
        continue;
      }
      auto& View = It->second;
      if (!View.MayWrite) {
        if (FillSharedFileView(*View.File, Storage.get(), View.Size, View.Offset) < 0) {
          WriteLog("Could not refresh a shared file mapping after a write.");
        }
      } else {
        std::vector<uint8_t> Fresh(View.Size);
        if (FillSharedFileView(*View.File, Fresh.data(), View.Size, View.Offset) < 0) {
          WriteLog("Could not refresh a shared file mapping after a write.");
        } else {
          auto* Bytes = static_cast<uint8_t*>(Storage.get());
          for (size_t Page = 0; Page < View.Size; Page += GuestPageSize) {
            if (std::memcmp(Bytes + Page, View.Snapshot.data() + Page, GuestPageSize)) continue;
            std::memcpy(Bytes + Page, Fresh.data() + Page, GuestPageSize);
            std::memcpy(View.Snapshot.data() + Page, Fresh.data() + Page, GuestPageSize);
          }
        }
      }
      ++It;
    }
    --ViewSyncDepth;
  }

  void MarkFileDataChanged(uint64_t Inode) {
    auto& Record = Inodes[Inode];
    if (Record.Mode && !CapWrtInode(CapFsetid, Inode)) {
      // An unprivileged writer loses setuid and executable setgid.
      const uint32_t Clear = 04000 | ((*Record.Mode & 0010) ? 02000 : 0);
      *Record.Mode &= ~Clear;
    }
    if (Inode >= InodeSnapshot::FirstInode) DirtyFileData.insert(Inode);
    MarkInodeChanged(Inode, true);
    if (!ViewSyncDepth && SharedFileViews.contains(Inode)) RefreshSharedFileViews(Inode);
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
  static constexpr uint64_t ProcDevice = 2, DevDevice = 3, ShmDevice = 4, AnonDevice = 13; // anon_inodefs is 0:13 on Linux

  // ---- Timers: itimers, POSIX timers and timerfds ----

  static constexpr int32_t SigevSignal = 0, SigevNone = 1, SigevThread = 2, SigevThreadId = 4;
  static constexpr uint64_t TimerAbsolute = 1; // TIMER_ABSTIME, TFD_TIMER_ABSTIME
  std::vector<std::weak_ptr<TimerFdState>> TimerFds;
  uint64_t SignalChanges {}; // counts signal queue changes, for edge-triggered epoll on signalfd

  // Current time in a timer's clock domain; nullopt when the clock's process or thread is gone.
  std::optional<uint64_t> TimerNow(const TimerClock& Clock) {
    switch (Clock.Domain) {
    case TimerDomain::Monotonic: return MonotonicNs();
    case TimerDomain::Realtime: return RealTimeNs();
    case TimerDomain::Tai: return TaiNs();
    case TimerDomain::ProcessCpu: {
      const auto* Process = FindProcess(Clock.Owner);
      if (!Process || Process->Exited) return std::nullopt;
      return ProcessCpuNs(*Process);
    }
    case TimerDomain::ThreadCpu:
      for (const auto& Thread : Threads) {
        if (Thread.Core && !Thread.Exited && Thread.TID == Clock.Owner) return ThreadCpuNs(Thread);
      }
      return std::nullopt;
    }
    return std::nullopt;
  }

  // The timer clock for a clock id accepted by timer_create or timerfd_create.
  TimerClock TimerClockFor(const Switch2Guest::DecodedClock& Clock) {
    using Switch2Guest::ClockKind;
    switch (Clock.Kind) {
    case ClockKind::Realtime: case ClockKind::RealtimeAlarm: return {TimerDomain::Realtime, 0};
    case ClockKind::Tai: return {TimerDomain::Tai, 0};
    case ClockKind::ProcessCpu: return {TimerDomain::ProcessCpu, CpuClockProcess(Clock, false)->PID};
    case ClockKind::ThreadCpu: return {TimerDomain::ThreadCpu, CpuClockThread(Clock)->TID};
    default: return {TimerDomain::Monotonic, 0};
    }
  }

  // The monotonic time at which a deadline in Clock's domain is reached at
  // the earliest. CPU time advances no faster than real time.
  std::optional<uint64_t> MonotonicDeadline(const TimerClock& Clock, uint64_t Deadline, uint64_t Mono) {
    const auto Now = TimerNow(Clock);
    if (!Now) return std::nullopt;
    return Deadline <= *Now ? Mono : AddDeadline(Mono, Deadline - *Now);
  }

  // The earliest monotonic time an armed timer may expire, or 0. Without
  // IncludeCpu, only timers that expire while every guest thread waits.
  uint64_t NextTimerDeadline(bool IncludeCpu = true) {
    const auto Mono = MonotonicNs();
    uint64_t Next {};
    auto Consider = [&](const TimerClock& Clock, const Switch2Guest::ArmedTimer& Timer) {
      if (!Timer.Armed()) return;
      if (!IncludeCpu && (Clock.Domain == TimerDomain::ProcessCpu || Clock.Domain == TimerDomain::ThreadCpu)) return;
      if (const auto At = MonotonicDeadline(Clock, Timer.Deadline, Mono)) Next = Next ? std::min(Next, *At) : *At;
    };
    for (auto& [PID, Process] : Processes) {
      if (Process.Exited || Process.Reaped) continue;
      Consider({TimerDomain::Monotonic, 0}, Process.ITimers[0]);
      Consider({TimerDomain::ProcessCpu, PID}, Process.ITimers[1]);
      Consider({TimerDomain::ProcessCpu, PID}, Process.ITimers[2]);
      for (const auto& Timer : Process.Timers) Consider(Timer.Clock, Timer.Timer);
      if (const auto Limit = CpuLimitDeadline(Process); Limit && IncludeCpu) {
        if (const auto At = MonotonicDeadline({TimerDomain::ProcessCpu, PID}, *Limit, Mono)) Next = Next ? std::min(Next, *At) : *At;
      }
    }
    for (const auto& Weak : TimerFds) {
      if (const auto State = Weak.lock()) Consider(State->Clock, State->Timer);
    }
    return Next;
  }

  // A thread of Process with the given tid, or null.
  GuestThread* ProcessThread(const GuestProcess& Process, uint32_t TID) {
    for (auto& Thread : Threads) {
      if (Thread.Core && !Thread.Exited && Thread.PID == Process.PID && Thread.TID == TID) return &Thread;
    }
    return nullptr;
  }

  static bool IsTimerSignal(const SigInfo& Info, int32_t Id) {
    return Info.Code == SiTimer && Info.Timer.Tid == Id;
  }

  // A queued timer signal carries its arming's sequence in si_sys_private
  // (offset 32), which Linux clears before the signal reaches the guest.
  static constexpr size_t TimerSequenceOffset = 32 - offsetof(SigInfo, Kill);
  static uint32_t TimerSequence(const SigInfo& Info) {
    uint32_t Sequence;
    std::memcpy(&Sequence, Info.Raw + TimerSequenceOffset, sizeof(Sequence));
    return Sequence;
  }
  static void SetTimerSequence(SigInfo& Info, uint32_t Sequence) {
    std::memcpy(Info.Raw + TimerSequenceOffset, &Sequence, sizeof(Sequence));
  }

  // Linux posix_timer_fn/send_sigqueue: while the timer's previous signal is
  // still queued, further expirations only add to its overrun.
  void QueueTimerSignal(GuestProcess& Process, PosixTimer& Timer, uint64_t Count) {
    if (Timer.Notify == SigevNone) return;
    GuestThread* Thread {};
    if (Timer.Notify == SigevThreadId) {
      Thread = ProcessThread(Process, Timer.Tid);
      if (!Thread) return; // the target thread exited
    }
    auto& Queue = Thread ? Thread->Pending : Process.SharedPending;
    if (auto* Queued = Queue.Find([&](const SigInfo& Info) { return IsTimerSignal(Info, Timer.Id); })) {
      // A stale signal from an earlier arming is reused for this one.
      const auto Total = TimerSequence(*Queued) == Timer.Sequence ? static_cast<uint64_t>(Queued->Timer.Overrun) + Count : Count - 1;
      Queued->Timer.Overrun = Switch2Guest::ClampOverrun(Total);
      Queued->Timer.Value = Timer.Value;
      SetTimerSequence(*Queued, Timer.Sequence);
      return;
    }
    SigInfo Info;
    Info.Signo = static_cast<int32_t>(Timer.Signal);
    Info.Code = SiTimer;
    Info.Timer.Tid = Timer.Id;
    Info.Timer.Overrun = Switch2Guest::ClampOverrun(Count - 1);
    Info.Timer.Value = Timer.Value;
    SetTimerSequence(Info, Timer.Sequence);
    SendSignal(Process, Thread, Info);
  }

  // Expires every due timer. Runs on each scheduler pass and after syscalls
  // once a deadline has passed.
  void FireTimers() {
    constexpr uint32_t ITimerSignals[] {SigAlrm, SigVtalrm, SigProf};
    for (auto& [PID, Process] : Processes) {
      if (Process.Exited || Process.Reaped) continue;
      for (size_t Which = 0; Which < Process.ITimers.size(); ++Which) {
        auto& Timer = Process.ITimers[Which];
        if (!Timer.Armed()) continue;
        const auto Now = Which ? ProcessCpuNs(Process) : MonotonicNs();
        // it_real_fn and the CPU itimers send with SEND_SIG_PRIV: SI_KERNEL.
        if (Timer.Expire(Now)) SendSignal(Process, nullptr, MakeSigInfo(ITimerSignals[Which], SiKernel));
      }
      for (auto& Timer : Process.Timers) {
        if (!Timer.Timer.Armed()) continue;
        const auto Now = TimerNow(Timer.Clock);
        if (!Now) continue;
        if (const auto Count = Timer.Timer.Expire(*Now)) QueueTimerSignal(Process, Timer, Count);
      }
      CheckCpuLimit(Process);
    }
    std::erase_if(TimerFds, [&](const auto& Weak) {
      const auto State = Weak.lock();
      if (!State) return true;
      if (!State->Timer.Armed()) return false;
      const auto Now = TimerNow(State->Clock);
      if (!Now) return false;
      if (const auto Count = State->Timer.Expire(*Now)) {
        State->Ticks += Count;
        ++State->Changes;
        ++IOEpoch;
      }
      return false;
    });
  }

  // posixtimer_deliver_signal: a timer signal from a deleted or re-armed
  // timer stays pending until dequeued, then is dropped. A live one records
  // its overrun for timer_getoverrun.
  bool TakeTimerSignal(SigInfo& Info) {
    if (Info.Code != SiTimer) return true;
    for (auto& Timer : Task->Timers) {
      if (Timer.Id != Info.Timer.Tid) continue;
      if (TimerSequence(Info) != Timer.Sequence) return false;
      Timer.OverrunLast = Info.Timer.Overrun;
      SetTimerSequence(Info, 0);
      return true;
    }
    return false;
  }

  // Dequeues the next signal in Allowed for Thread of the current process:
  // its own queue first, then the process queue.
  std::optional<SigInfo> TakeSignal(GuestThread& Thread, uint64_t Allowed) {
    for (;;) {
      auto Info = Thread.Pending.Dequeue(Allowed);
      if (!Info) Info = Task->SharedPending.Dequeue(Allowed);
      if (Info) ++SignalChanges;
      if (!Info || TakeTimerSignal(*Info)) return Info;
    }
  }

  // The sender in si_pid and si_uid, as the receiver's PID and user namespaces number it (the runtime builds
  // them with the global process number and the kernel id): 0 when the sender is not visible to the receiver,
  // 65534 for an unmapped uid. Linux does this when it queues the signal (the receiver's namespaces then).
  static void TranslateSigInfo(SigInfo& Info, const GuestProcess& Receiver) {
    if (Info.Code <= 0 && Info.Code != SiTimer) {
      Info.Kill.Pid = Receiver.Ns.Pid->ToLocal(Info.Kill.Pid);
      Info.Kill.Uid = Switch2Guest::Ns::MungeUid(*Receiver.Ns.User, Info.Kill.Uid);
    } else if (Info.Signo == static_cast<int32_t>(SigChld) && Info.Code >= CldExited && Info.Code <= CldContinued) {
      Info.Child.Pid = Receiver.Ns.Pid->ToLocal(Info.Child.Pid);
      Info.Child.Uid = Switch2Guest::Ns::MungeUid(*Receiver.Ns.User, Info.Child.Uid);
    }
  }
  bool RawSigInfo {}; // rt_sigqueueinfo: the sender's si_pid and si_uid are the caller's own words

  struct GuestTimeval {
    int64_t Seconds, Microseconds;
  };
  struct GuestITimerval {
    GuestTimeval Interval, Value;
  };
  struct GuestTimespec {
    int64_t Seconds, Nanoseconds;
  };
  struct GuestITimerspec {
    GuestTimespec Interval, Value;
  };

  static GuestTimeval ToGuestTimeval(uint64_t Ns) {
    const auto Value = Switch2Guest::ToTimevalUp(Ns);
    return {Value.Seconds, Value.Microseconds};
  }
  static GuestTimespec ToGuestTimespec(uint64_t Ns) {
    return {static_cast<int64_t>(Ns / 1'000'000'000), static_cast<int64_t>(Ns % 1'000'000'000)};
  }
  static bool ValidTimespec(const GuestTimespec& Time) {
    return Time.Seconds >= 0 && Time.Nanoseconds >= 0 && Time.Nanoseconds < 1'000'000'000;
  }
  static uint64_t TimespecNs(const GuestTimespec& Time) {
    return Time.Seconds > static_cast<int64_t>((MaxDeadline - Time.Nanoseconds) / 1'000'000'000)
      ? MaxDeadline : Time.Seconds * 1'000'000'000ULL + Time.Nanoseconds;
  }

  // Now in the domain of ITIMER Which of the current process.
  uint64_t ITimerNow(size_t Which) { return Which ? ProcessCpuNs(*Task) : MonotonicNs(); }

  GuestITimerval ReadITimer(size_t Which) {
    const auto Left = Task->ITimers[Which].Remaining(ITimerNow(Which));
    return {ToGuestTimeval(Left.IntervalNs), ToGuestTimeval(Left.ValueNs)};
  }

  int64_t GetITimer(uint64_t Which, uint64_t Address) {
    if (Which > 2) return Invalid;
    return CopyOut(Address, ReadITimer(Which));
  }

  // setitimer: a null new value disarms the timer, as Linux 7.3 does.
  int64_t SetITimer(uint64_t Which, uint64_t NewAddress, uint64_t OldAddress) {
    GuestITimerval New {};
    if (NewAddress) {
      if (Available(NewAddress, PF_R) < sizeof(New)) return Fault;
      std::memcpy(&New, reinterpret_cast<const void*>(NewAddress), sizeof(New));
    }
    if (Which > 2) return Invalid;
    auto Valid = [](const GuestTimeval& Time) {
      return Time.Seconds >= 0 && Time.Microseconds >= 0 && Time.Microseconds < 1'000'000;
    };
    if (!Valid(New.Value) || !Valid(New.Interval)) return Invalid;
    const auto Old = ReadITimer(Which);
    auto& Timer = Task->ITimers[Which];
    const auto Value = Switch2Guest::FromTimeval(New.Value.Seconds, New.Value.Microseconds);
    if (Value) Timer.Arm(AddDeadline(ITimerNow(Which), Value), Switch2Guest::FromTimeval(New.Interval.Seconds, New.Interval.Microseconds));
    else Timer.Disarm();
    return OldAddress ? CopyOut(OldAddress, Old) : 0;
  }

  int64_t AlarmGuest(uint32_t Seconds) {
    auto& Timer = Task->ITimers[0];
    const auto Now = MonotonicNs();
    const auto Left = Timer.Remaining(Now).ValueNs;
    if (Seconds) Timer.Arm(AddDeadline(Now, Seconds * 1'000'000'000ULL), 0);
    else Timer.Disarm();
    return Switch2Guest::AlarmSecondsLeft(Left);
  }

  // Linux clockid_to_kclock and kc->timer_create for timer_create.
  static int64_t CheckTimerClock(const Switch2Guest::DecodedClock& Clock) {
    using Switch2Guest::ClockKind;
    switch (Clock.Kind) {
    case ClockKind::Invalid: return Invalid;
    case ClockKind::MonotonicRaw: case ClockKind::RealtimeCoarse: case ClockKind::MonotonicCoarse: return NotSupported;
    default: break;
    }
    return 0;
  }

  int64_t TimerCreate(uint64_t ClockId, uint64_t EventAddress, uint64_t IdAddress) {
    struct GuestSigevent {
      uint64_t Value;
      int32_t Signal, Notify, Tid;
      uint8_t Pad[44];
    };
    static_assert(sizeof(GuestSigevent) == 64);
    GuestSigevent Event {};
    if (EventAddress) {
      if (Available(EventAddress, PF_R) < sizeof(Event)) return Fault;
      std::memcpy(&Event, reinterpret_cast<const void*>(EventAddress), sizeof(Event));
    }
    using Switch2Guest::ClockKind;
    const auto Clock = Switch2Guest::DecodeClock(static_cast<int32_t>(ClockId));
    if (const auto Error = CheckTimerClock(Clock)) return Error;
    if (Task->Timers.size() >= Task->Limits[ResourceSigpending][0]) return -11;
    // The id is allocated before the remaining checks and is not reused.
    const auto Id = Task->NextTimerId;
    Task->NextTimerId = Id == INT32_MAX ? 0 : Id + 1;
    PosixTimer Timer;
    Timer.Id = Id;
    if (EventAddress) {
      // good_sigevent
      switch (Event.Notify) {
      case SigevThreadId:
        if (!ProcessThread(*Task, static_cast<uint32_t>(Event.Tid))) return Invalid;
        [[fallthrough]];
      case SigevSignal: case SigevThread:
        if (Event.Signal <= 0 || Event.Signal > static_cast<int32_t>(SigMax)) return Invalid;
        [[fallthrough]];
      case SigevNone: break;
      default: return Invalid;
      }
      Timer.Notify = Event.Notify == SigevThread ? SigevSignal : Event.Notify;
      Timer.Signal = static_cast<uint32_t>(Event.Signal);
      Timer.Value = Event.Value;
      Timer.Tid = static_cast<uint32_t>(Event.Tid);
    } else {
      Timer.Notify = SigevSignal;
      Timer.Signal = SigAlrm;
      Timer.Value = static_cast<uint32_t>(Id);
    }
    if (CopyOut(IdAddress, Id)) return Fault;
    // kc->timer_create: alarm clocks need CAP_WAKE_ALARM; CPU clocks a live target.
    if ((Clock.Kind == ClockKind::RealtimeAlarm || Clock.Kind == ClockKind::BoottimeAlarm) &&
        !Task->Creds.Capable(Switch2Guest::CapWakeAlarm)) return -1;
    if ((Clock.Kind == ClockKind::ProcessCpu || Clock.Kind == ClockKind::ThreadCpu) && !CpuClockValid(Clock)) return Invalid;
    Timer.Clock = TimerClockFor(Clock);
    Task->Timers.push_back(Timer);
    return 0;
  }

  PosixTimer* FindTimer(uint64_t Id) {
    for (auto& Timer : Task->Timers) {
      if (static_cast<uint64_t>(static_cast<uint32_t>(Timer.Id)) == (Id & 0xffffffff)) return &Timer;
    }
    return nullptr;
  }

  GuestITimerspec TimerValue(const TimerClock& Clock, const Switch2Guest::ArmedTimer& Timer) {
    const auto Now = TimerNow(Clock);
    const auto Left = Timer.Remaining(Now.value_or(Timer.Deadline));
    return {ToGuestTimespec(Left.IntervalNs), ToGuestTimespec(Left.ValueNs)};
  }

  // Arms Timer from a validated itimerspec; absolute values are times on the clock.
  void ArmTimer(const TimerClock& Clock, Switch2Guest::ArmedTimer& Timer, const GuestITimerspec& New, bool Absolute) {
    const auto Value = TimespecNs(New.Value);
    if (!Value) {
      Timer.Disarm();
      return;
    }
    const auto Now = TimerNow(Clock).value_or(0);
    Timer.Arm(Absolute ? Value : AddDeadline(Now, Value), TimespecNs(New.Interval));
  }

  int64_t TimerSetTime(uint64_t Id, uint64_t Flags, uint64_t NewAddress, uint64_t OldAddress) {
    if (!NewAddress) return Invalid;
    GuestITimerspec New {};
    if (Available(NewAddress, PF_R) < sizeof(New)) return Fault;
    std::memcpy(&New, reinterpret_cast<const void*>(NewAddress), sizeof(New));
    if (!ValidTimespec(New.Interval) || !ValidTimespec(New.Value)) return Invalid;
    auto* Timer = FindTimer(Id);
    if (!Timer) return Invalid;
    const auto Old = TimerValue(Timer->Clock, Timer->Timer);
    // A signal queued by the previous arming is stale and never delivered.
    ++Timer->Sequence;
    ArmTimer(Timer->Clock, Timer->Timer, New, Flags & TimerAbsolute);
    return OldAddress ? CopyOut(OldAddress, Old) : 0;
  }

  int64_t TimerGetTime(uint64_t Id, uint64_t Address) {
    const auto* Timer = FindTimer(Id);
    if (!Timer) return Invalid;
    return CopyOut(Address, TimerValue(Timer->Clock, Timer->Timer));
  }

  int64_t TimerGetOverrun(uint64_t Id) {
    const auto* Timer = FindTimer(Id);
    return Timer ? Timer->OverrunLast : Invalid;
  }

  int64_t TimerDelete(uint64_t Id) {
    auto* Timer = FindTimer(Id);
    if (!Timer) return Invalid;
    Task->Timers.erase(Task->Timers.begin() + (Timer - Task->Timers.data()));
    return 0;
  }

  // exec deletes the POSIX timers (exit_itimers); itimers stay. Their
  // queued signals turn stale.
  void DeleteProcessTimers(GuestProcess& Process) { Process.Timers.clear(); }

  static constexpr uint64_t AnonCloseOnExec = 02000000, AnonNonblock = 04000;

  // A descriptor on the shared anonymous inode, like anon_inode_getfd.
  int64_t OpenAnonymous(const char* Name, uint64_t Flags, uint32_t Access, std::shared_ptr<OpenFile> File) {
    const auto FD = FreeDescriptor();
    if (FD == NoDescriptor) return -24;
    if (!AnonInode) AnonInode = NextAnonymousInode++;
    File->AnonName = Name;
    File->Path = std::string("anon_inode:") + Name;
    File->Inode = AnonInode;
    File->StatusFlags = Access | (Flags & AnonNonblock);
    Descriptors().Files[FD] = std::move(File);
    Descriptors().Flags[FD] = (Flags & AnonCloseOnExec) ? 1 : 0;
    return FD;
  }

  int64_t TimerFdCreate(uint64_t ClockId, uint64_t Flags) {
    using Switch2Guest::ClockKind;
    const auto Clock = Switch2Guest::DecodeClock(static_cast<int32_t>(ClockId));
    const bool Allowed = static_cast<int32_t>(ClockId) >= 0 &&
      (Clock.Kind == ClockKind::Realtime || Clock.Kind == ClockKind::Monotonic || Clock.Kind == ClockKind::Boottime ||
       Clock.Kind == ClockKind::RealtimeAlarm || Clock.Kind == ClockKind::BoottimeAlarm);
    if ((Flags & ~(AnonCloseOnExec | AnonNonblock)) || !Allowed) return Invalid;
    if ((Clock.Kind == ClockKind::RealtimeAlarm || Clock.Kind == ClockKind::BoottimeAlarm) &&
        !Task->Creds.Capable(Switch2Guest::CapWakeAlarm)) return -1;
    auto File = std::make_shared<OpenFile>();
    File->TimerFd = std::make_shared<TimerFdState>();
    File->TimerFd->Clock = TimerClockFor(Clock);
    const auto State = File->TimerFd;
    const auto FD = OpenAnonymous("[timerfd]", Flags, 2, std::move(File));
    if (FD >= 0) TimerFds.push_back(State);
    return FD;
  }

  // timerfd_fget: EBADF for a closed descriptor, EINVAL for another kind.
  int64_t TimerFdOf(uint64_t FD, TimerFdState*& State) {
    auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (!File->TimerFd) return Invalid;
    State = File->TimerFd.get();
    return 0;
  }

  int64_t TimerFdSetTime(uint64_t FD, uint64_t Flags, uint64_t NewAddress, uint64_t OldAddress) {
    constexpr uint64_t CancelOnSet = 2;
    GuestITimerspec New {};
    if (Available(NewAddress, PF_R) < sizeof(New)) return Fault;
    std::memcpy(&New, reinterpret_cast<const void*>(NewAddress), sizeof(New));
    if ((Flags & ~(TimerAbsolute | CancelOnSet)) || !ValidTimespec(New.Interval) || !ValidTimespec(New.Value)) return Invalid;
    TimerFdState* State {};
    if (const auto Error = TimerFdOf(FD, State)) return Error;
    const auto Old = TimerValue(State->Clock, State->Timer);
    // timerfd_setup_cancel: only absolute realtime timers may cancel; other flags are ignored.
    State->CancelOnSet = State->Clock.Domain == TimerDomain::Realtime && (Flags & TimerAbsolute) && (Flags & CancelOnSet);
    State->Canceled = false;
    State->Ticks = 0;
    ++State->Changes;
    ArmTimer(State->Clock, State->Timer, New, Flags & TimerAbsolute);
    ++IOEpoch;
    return OldAddress ? CopyOut(OldAddress, Old) : 0;
  }

  int64_t TimerFdGetTime(uint64_t FD, uint64_t Address) {
    TimerFdState* State {};
    if (const auto Error = TimerFdOf(FD, State)) return Error;
    return CopyOut(Address, TimerValue(State->Clock, State->Timer));
  }

  int64_t ReadTimerFd(OpenFile& File, char* Buffer, uint64_t Count) {
    auto& State = *File.TimerFd;
    if (Count < sizeof(uint64_t)) return Invalid;
    if (!State.Ticks) return File.Nonblocking() ? -11 : WaitForIO();
    ++State.Changes;
    if (State.Canceled) {
      State.Canceled = false;
      State.Ticks = 0;
      return -125; // ECANCELED
    }
    const auto Ticks = std::exchange(State.Ticks, 0);
    std::memcpy(Buffer, &Ticks, sizeof(Ticks));
    return sizeof(Ticks);
  }

  // eventfd and eventfd2. EFD_SEMAPHORE is 1; the others are O_ flags.
  int64_t EventFdCreate(uint64_t Initial, uint64_t Flags) {
    constexpr uint64_t Semaphore = 1;
    if (Flags & ~(Semaphore | AnonNonblock | AnonCloseOnExec)) return Invalid;
    auto File = std::make_shared<OpenFile>();
    File->Event = std::make_shared<Switch2Guest::EventCounter>();
    File->Event->Value = static_cast<uint32_t>(Initial);
    File->Event->Semaphore = Flags & Semaphore;
    return OpenAnonymous("[eventfd]", Flags, 2, std::move(File));
  }

  int64_t ReadEventFd(OpenFile& File, char* Buffer, uint64_t Count) {
    if (Count < sizeof(uint64_t)) return Invalid;
    uint64_t Value {};
    if (File.Event->Read(Value) < 0) return File.Nonblocking() ? -11 : WaitForIO();
    std::memcpy(Buffer, &Value, sizeof(Value));
    ++IOEpoch; // a writer may have room now
    return sizeof(Value);
  }

  int64_t WriteEventFd(OpenFile& File, const char* Buffer, uint64_t Count) {
    if (Count < sizeof(uint64_t)) return Invalid;
    uint64_t Add {};
    std::memcpy(&Add, Buffer, sizeof(Add));
    const auto Result = File.Event->Write(Add);
    if (Result == -11) return File.Nonblocking() ? -11 : WaitForIO();
    if (Result < 0) return Result;
    if (Add) ++IOEpoch;
    return sizeof(Add);
  }

  // signalfd and signalfd4. SIGKILL and SIGSTOP are silently left out.
  int64_t SignalFdCreate(uint64_t FD, uint64_t MaskAddress, uint64_t MaskSize, uint64_t Flags) {
    if (MaskSize != sizeof(uint64_t)) return Invalid;
    uint64_t Mask {};
    if (Available(MaskAddress, PF_R) < sizeof(Mask)) return Fault;
    std::memcpy(&Mask, reinterpret_cast<const void*>(MaskAddress), sizeof(Mask));
    if (Flags & ~(AnonNonblock | AnonCloseOnExec)) return Invalid;
    Mask &= ~Unblockable;
    if (static_cast<int32_t>(FD) == -1) {
      auto File = std::make_shared<OpenFile>();
      File->SignalFdMask = Mask;
      return OpenAnonymous("[signalfd]", Flags, 2, std::move(File));
    }
    auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (!File->SignalFdMask) return Invalid;
    File->SignalFdMask = Mask;
    ++IOEpoch;
    return static_cast<int64_t>(static_cast<int32_t>(FD));
  }

  // Dequeues signals in the mask, blocked or not, from the reading thread's
  // queue and then the process queue (signalfd_dequeue). Only the first
  // record waits.
  int64_t ReadSignalFd(OpenFile& File, char* Buffer, uint64_t Count) {
    using Switch2Guest::SignalFdInfo;
    if (Count < sizeof(SignalFdInfo)) return Invalid;
    uint64_t Done {};
    while (Count - Done >= sizeof(SignalFdInfo)) {
      const auto Info = TakeSignal(*Current, *File.SignalFdMask);
      if (!Info) break;
      const auto Record = Switch2Guest::ToSignalFdInfo(*Info);
      std::memcpy(Buffer + Done, &Record, sizeof(Record));
      Done += sizeof(Record);
    }
    if (Done) return static_cast<int64_t>(Done);
    return File.Nonblocking() ? -11 : WaitForIO();
  }

  // Anonymous descriptors (timerfd, eventfd, signalfd, epoll, inotify).
  int64_t ReadAnonymous(OpenFile& File, char* Buffer, uint64_t Count) {
    if (File.TimerFd) return ReadTimerFd(File, Buffer, Count);
    if (File.Event) return ReadEventFd(File, Buffer, Count);
    if (File.SignalFdMask) return ReadSignalFd(File, Buffer, Count);
    if (File.Inotify) {
      const auto Result = File.Inotify->Instance.Read(reinterpret_cast<uint8_t*>(Buffer), Count);
      if (Result == -11 && !File.Nonblocking()) return WaitForIO();
      return Result;
    }
    return Invalid;
  }

  int64_t WriteAnonymous(OpenFile& File, const char* Buffer, uint64_t Count) {
    if (File.Event) return WriteEventFd(File, Buffer, Count);
    return Invalid;
  }

  int64_t AnonymousIoctl(OpenFile& File, uint64_t Command, uint64_t Argument) {
    constexpr uint64_t SetTicks = 0x40085400; // TFD_IOC_SET_TICKS
    if (File.TimerFd && Command == SetTicks) {
      uint64_t Ticks {};
      if (Available(Argument, PF_R) < sizeof(Ticks)) return Fault;
      std::memcpy(&Ticks, reinterpret_cast<const void*>(Argument), sizeof(Ticks));
      if (!Ticks) return Invalid;
      File.TimerFd->Ticks = Ticks;
      ++File.TimerFd->Changes;
      ++IOEpoch;
      return 0;
    }
    if (File.Epoll) return Invalid; // ep_eventpoll_ioctl
    if (File.Inotify && Command == 0x541b) { // FIONREAD
      return CopyOut(Argument, static_cast<int32_t>(File.Inotify->Instance.PendingBytes()));
    }
    return -25;
  }

  // Poll mask of an anonymous descriptor.
  uint16_t AnonymousEvents(const OpenFile& File) const {
    if (File.TimerFd) return File.TimerFd->Ticks ? 1 : 0;
    if (File.Event) return (File.Event->Readable() ? 1 : 0) | (File.Event->Writable() ? 4 : 0);
    if (File.Inotify) return File.Inotify->Instance.Readable() ? 1 : 0;
    if (File.Epoll) {
      const auto Ready = File.Epoll->Set.AnyReady(
        [&](const EpollKey& Key) { const auto Target = Key.File.lock(); return Target ? Readiness(*Target) : 0; },
        [&](const EpollKey& Key) { const auto Target = Key.File.lock(); return Target ? ReadinessGeneration(*Target) : 0; });
      return Ready ? 1 : 0;
    }
    if (File.SignalFdMask) {
      // signalfd_poll: signals pending for the polling thread.
      if (!Current) return 0;
      return ((Current->Pending.Set() | Task->SharedPending.Set()) & *File.SignalFdMask) ? 1 : 0;
    }
    return 0;
  }

  LinuxStat AnonymousStat() const {
    LinuxStat Info {};
    Info.Device = AnonDevice;
    Info.Inode = AnonInode;
    Info.Links = 1;
    Info.Mode = 0600; // anon_inode: no file type bits
    Info.BlockSize = GuestPageSize;
    return Info;
  }

  // ---- epoll ----

  static constexpr unsigned EpollMaxNests = 4; // EP_MAX_NESTS
  std::vector<std::weak_ptr<EpollState>> EpollInstances;

  int64_t EpollCreate(uint64_t Flags) {
    if (Flags & ~AnonCloseOnExec) return Invalid;
    auto File = std::make_shared<OpenFile>();
    File->Epoll = std::make_shared<EpollState>();
    std::erase_if(EpollInstances, [](const auto& Weak) { return Weak.expired(); });
    EpollInstances.push_back(File->Epoll);
    return OpenAnonymous("[eventpoll]", Flags, 2, std::move(File));
  }

  // file_can_poll: the descriptor kinds with a poll operation.
  static bool CanPoll(const OpenFile& File) {
    return File.Socket || File.Inet || File.Console != ConsoleStream::None || File.AnonName || File.PidfdTarget;
  }

  static void ForgetClosed(EpollState& State) {
    State.Set.ForgetIf([](const EpollKey& Key) { return Key.File.expired(); });
  }

  // ep_loop_check_proc: the deepest nesting below From, or UINT_MAX when Into
  // is reachable (a cycle).
  unsigned EpollDepthBelow(const EpollState& From, const EpollState& Into, unsigned Depth) const {
    unsigned Result = Depth;
    From.Set.ForEachKey([&](const EpollKey& Key) {
      if (Result == UINT_MAX) return;
      const auto Target = Key.File.lock();
      if (!Target || !Target->Epoll) return;
      if (Target->Epoll.get() == &Into) {
        Result = UINT_MAX;
        return;
      }
      Result = std::max(Result, EpollDepthBelow(*Target->Epoll, Into, Depth + 1));
    });
    return Result;
  }

  // ep_get_upwards_depth_proc: how deeply State is nested in other instances.
  unsigned EpollDepthAbove(const EpollState& State, unsigned Depth) const {
    unsigned Result = Depth;
    for (const auto& Weak : EpollInstances) {
      const auto Parent = Weak.lock();
      if (!Parent || Parent.get() == &State) continue;
      bool Contains {};
      Parent->Set.ForEachKey([&](const EpollKey& Key) {
        const auto Target = Key.File.lock();
        Contains |= Target && Target->Epoll.get() == &State;
      });
      if (Contains && Depth < EpollMaxNests + 1) Result = std::max(Result, EpollDepthAbove(*Parent, Depth + 1));
    }
    return Result;
  }

  int64_t EpollCtl(uint64_t EpFd, uint64_t OpArgument, uint64_t FD, uint64_t EventAddress) {
    using namespace Switch2Guest;
    const auto Op = static_cast<uint32_t>(OpArgument);
    EpollEvent Event {};
    if (Op != EpollCtlDel) {
      if (Available(EventAddress, PF_R) < sizeof(Event)) return Fault;
      std::memcpy(&Event, reinterpret_cast<const void*>(EventAddress), sizeof(Event));
    }
    auto* Epoll = GetFile(EpFd);
    if (!Epoll || Epoll->PathOnly()) return BadFile;
    auto* Target = GetFile(FD);
    if (!Target || Target->PathOnly()) return BadFile;
    if (!CanPoll(*Target)) return -1;
    if (Epoll == Target || !Epoll->Epoll) return Invalid;
    if (Op != EpollCtlDel) {
      if (const auto Error = CheckEpollExclusive(Op, Event.Events, Target->Epoll != nullptr)) return Error;
    }
    auto& State = *Epoll->Epoll;
    if (Op == EpollCtlAdd && Target->Epoll) {
      const auto Below = EpollDepthBelow(*Target->Epoll, State, 0);
      if (Below == UINT_MAX || Below > EpollMaxNests || Below + 1 + EpollDepthAbove(State, 0) > EpollMaxNests) return -40;
    }
    ForgetClosed(State);
    const EpollKey Key {Descriptors().Files[FD], Target, static_cast<int32_t>(FD)};
    int64_t Result = Invalid;
    switch (Op) {
    case EpollCtlAdd: Result = State.Set.Add(Key, Event.Events, Event.Data); break;
    case EpollCtlDel: Result = State.Set.Remove(Key); break;
    case EpollCtlMod: Result = State.Set.Modify(Key, Event.Events, Event.Data); break;
    }
    if (!Result) ++IOEpoch; // a waiter may have a newly ready item
    return Result;
  }

  int64_t EpollWait(uint64_t EpFd, uint64_t Events, uint64_t MaxArgument, int32_t Milliseconds) {
    return EpollWaitCore(EpFd, Events, MaxArgument,
                         Milliseconds < 0 ? std::nullopt : std::optional<uint64_t> {Milliseconds * 1'000'000ULL});
  }

  int64_t EpollPwait(uint64_t EpFd, uint64_t Events, uint64_t MaxArgument, int32_t Milliseconds, uint64_t Mask, uint64_t MaskSize) {
    std::optional<uint64_t> WaitMask;
    if (const auto Error = ReadWaitMask(Mask, MaskSize, WaitMask)) return Error;
    InstallWaitMask(WaitMask);
    return EpollWait(EpFd, Events, MaxArgument, Milliseconds);
  }

  int64_t EpollPwait2(uint64_t EpFd, uint64_t Events, uint64_t MaxArgument, uint64_t Timeout, uint64_t Mask, uint64_t MaskSize) {
    uint64_t Duration {};
    if (Timeout && Available(Timeout, PF_R) < 16) return Fault;
    if (Timeout && !ReadTimespec(Timeout, Duration)) return Invalid;
    std::optional<uint64_t> WaitMask;
    if (const auto Error = ReadWaitMask(Mask, MaskSize, WaitMask)) return Error;
    InstallWaitMask(WaitMask);
    return EpollWaitCore(EpFd, Events, MaxArgument, Timeout ? std::optional {Duration} : std::nullopt);
  }

  // do_epoll_wait and ep_poll. Never restarted after a handler.
  int64_t EpollWaitCore(uint64_t EpFd, uint64_t Events, uint64_t MaxArgument, std::optional<uint64_t> Duration) {
    using Switch2Guest::EpollEvent;
    const auto Max = static_cast<int32_t>(MaxArgument);
    if (Max <= 0 || Max > static_cast<int32_t>(INT32_MAX / sizeof(EpollEvent))) return Invalid;
    if (!IsGuestUserRange(Events, Max * sizeof(EpollEvent))) return Fault;
    const auto* File = GetFile(EpFd);
    if (!File || File->PathOnly()) return BadFile;
    if (!File->Epoll) return Invalid;
    const auto State = File->Epoll;
    ForgetClosed(*State);
    if (Interactive) PollTerminalInput();
    std::vector<EpollEvent> Ready(std::min<size_t>(Max, std::max<size_t>(State->Set.Size(), 1)));
    const auto Count = State->Set.Collect(
      Ready.data(), Ready.size(),
      [&](const EpollKey& Key) { const auto Target = Key.File.lock(); return Target ? Readiness(*Target) : 0; },
      [&](const EpollKey& Key) { const auto Target = Key.File.lock(); return Target ? ReadinessGeneration(*Target) : 0; });
    if (Count) {
      const auto Bytes = Count * sizeof(EpollEvent);
      if (AccessiblePrefix(Events, Bytes, PF_W) != Bytes) return Fault;
      std::memcpy(reinterpret_cast<void*>(Events), Ready.data(), Bytes);
      return static_cast<int64_t>(Count);
    }
    if (Duration == 0) return 0;
    if (Duration) {
      const auto Now = MonotonicNs();
      if (!Current->Deadline) Current->Deadline = AddDeadline(Now, *Duration);
      if (Current->Deadline <= Now) return 0;
    }
    return BlockPoll();
  }

  // ---- inotify ----
  // Events come from the guest syscalls that change or use files (BeforeFs
  // and AfterFs around the dispatcher, ClosedFiles for closes). Changes made
  // by the FTP server or the rootfs setup do not notify.

  std::vector<std::weak_ptr<InotifyState>> InotifyInstances;
  uint32_t NextMoveCookie {};

  int64_t InotifyInit(uint64_t Flags) {
    if (Flags & ~(AnonNonblock | AnonCloseOnExec)) return Invalid;
    std::erase_if(InotifyInstances, [](const auto& Weak) { return Weak.expired(); });
    if (InotifyInstances.size() >= Switch2Guest::InotifyMaxInstances) return -24;
    auto File = std::make_shared<OpenFile>();
    File->Inotify = std::make_shared<InotifyState>();
    InotifyInstances.push_back(File->Inotify);
    return OpenAnonymous("inotify", Flags, 0, std::move(File));
  }

  int64_t InotifyAddWatch(uint64_t FD, uint64_t Address, uint32_t Mask) {
    using namespace Switch2Guest;
    constexpr uint32_t AllBits = InAllEvents | InUnmount | InQOverflow | InIgnored | InOnlyDir | InDontFollow | InExclUnlink |
                                 InMaskAdd | InMaskCreate | InIsDir | InOneShot;
    if ((Mask & InMaskAdd) && (Mask & InMaskCreate)) return Invalid;
    if (!(Mask & AllBits)) return Invalid;
    auto* File = GetFile(FD);
    if (!File || File->PathOnly()) return BadFile;
    if (!File->Inotify) return Invalid;
    const auto Name = GuestString(Address);
    if (!Name) return Name.Error;
    LinuxStat Info {};
    if (const auto Error = PathStat(CurrentDirectory, *Name, Info, nullptr, !(Mask & InDontFollow))) return Error;
    const bool Directory = (Info.Mode & 0170000) == 0040000;
    if ((Mask & InOnlyDir) && !Directory) return -20;
    if (!InodeAllows(Info.Inode, Info.Mode, 4)) return -13;
    return File->Inotify->Instance.AddWatch(Info.Inode, Mask, Directory);
  }

  int64_t InotifyRemoveWatch(uint64_t FD, uint64_t Wd) {
    auto* File = GetFile(FD);
    if (!File || File->PathOnly()) return BadFile;
    if (!File->Inotify) return Invalid;
    const auto Result = File->Inotify->Instance.RemoveWatch(static_cast<int32_t>(Wd));
    if (!Result) ++IOEpoch;
    return Result;
  }

  bool InotifyWatching() {
    std::erase_if(InotifyInstances, [](const auto& Weak) { return Weak.expired(); });
    return std::any_of(InotifyInstances.begin(), InotifyInstances.end(), [](const auto& Weak) {
      const auto State = Weak.lock();
      return State && State->Instance.HasWatches();
    });
  }

  template<typename Fn>
  void ForEachInotify(Fn Visit) {
    for (const auto& Weak : InotifyInstances) {
      const auto State = Weak.lock();
      if (!State) continue;
      const auto Before = State->Instance.Changes();
      Visit(State->Instance);
      if (State->Instance.Changes() != Before) ++IOEpoch;
    }
  }

  void InotifyEmit(uint64_t Inode, uint32_t Mask, uint32_t Cookie, std::string_view Name, bool Directory, bool Unlinked = false) {
    if (!Inode) return;
    ForEachInotify([&](auto& Instance) { Instance.Emit(Inode, Mask, Cookie, Name, Directory, Unlinked); });
  }

  uint64_t KnownInode(std::string_view Path) const {
    const auto It = SdInodes.find(Path);
    return It == SdInodes.end() ? 0 : It->second;
  }

  static std::string_view BaseName(std::string_view Path) { return Path.substr(Path.rfind('/') + 1); }

  // A watched directory's child event, named.
  void NotifyChild(const std::string& Path, uint32_t Mask, uint32_t Cookie, bool Directory, bool Unlinked = false) {
    if (Path.empty() || Path == RootPath) return;
    InotifyEmit(KnownInode(ParentPath(Path)), Mask, Cookie, BaseName(Path), Directory, Unlinked);
  }

  // An event on a file: its directory's watches with the name, then its own
  // (fsnotify_handle_event handles the parent mark first).
  void NotifyPath(const std::string& Path, uint64_t Inode, uint32_t Mask, bool Directory, bool Unlinked = false) {
    NotifyChild(Path, Mask, 0, Directory, Unlinked);
    InotifyEmit(Inode, Mask, 0, {}, Directory, Unlinked);
  }

  void NotifyFile(const OpenFile& File, uint32_t Mask) {
    if (!File.NotifyClose) return; // only regular files and directories opened by path
    NotifyPath(File.Path, File.Inode, Mask, File.Directory.has_value(), IsUnlinked(File.Inode));
  }

  bool InodeOpen(uint64_t Inode) {
    bool Open {};
    ForEachOpenFile([&](const OpenFile& File) { Open |= File.Inode == Inode && File.NotifyClose; });
    return Open;
  }

  // The inode is gone (last link and last description): IN_DELETE_SELF.
  // EndInodeWatches then ends its watches with IN_IGNORED.
  void InodeDeleted(uint64_t Inode, bool Directory) {
    InotifyEmit(Inode, Switch2Guest::InDeleteSelf, 0, {}, Directory);
  }
  void EndInodeWatches(uint64_t Inode) {
    ForEachInotify([&](auto& Instance) { Instance.DropInode(Inode); });
  }

  void DrainClosedFiles() {
    auto Closed = std::move(ClosedFiles);
    ClosedFiles.clear();
    if (!InotifyWatching()) return;
    for (const auto& File : Closed) {
      const bool Unlinked = IsUnlinked(File.Inode);
      NotifyPath(File.Path, File.Inode, File.Write ? Switch2Guest::InCloseWrite : Switch2Guest::InCloseNowrite,
                 File.Directory, Unlinked);
      if (Unlinked && !InodeOpen(File.Inode)) {
        InodeDeleted(File.Inode, File.Directory);
        EndInodeWatches(File.Inode);
      }
    }
  }

  struct FsSubject {
    std::string Path;
    uint64_t Inode {};
    uint64_t Links {};
    bool Exists {}, Directory {};
  };
  struct FsHook {
    FsSubject First, Second;
    uint64_t Flags {};
  };

  FsSubject Subject(uint64_t FD, uint64_t Address, bool Follow) {
    FsSubject Result;
    if (!Address) return Result;
    const auto Name = GuestString(Address);
    if (!Name || Name->empty()) return Result;
    std::string_view Pathname = *Name;
    while (Pathname.size() > 1 && Pathname.ends_with('/')) Pathname.remove_suffix(1);
    LinuxStat Info {};
    if (!PathStat(FD, Pathname, Info, &Result.Path, Follow)) {
      Result.Exists = true;
      Result.Inode = Info.Inode;
      Result.Links = Info.Links;
      Result.Directory = (Info.Mode & 0170000) == 0040000;
      return Result;
    }
    Result.Path.clear();
    FsFileSystem SdCard {};
    if (R_SUCCEEDED(SdRetry(fsOpenSdCardFileSystem, &SdCard))) {
      if (ResolveSdPath(FD, Pathname, SdCard, Result.Path, false)) Result.Path.clear();
      fsFsClose(&SdCard);
    }
    return Result;
  }

  // Captures what path-based calls refer to before they run.
  std::optional<FsHook> BeforeFs(uint64_t Number, const uint64_t* Args) {
    constexpr uint64_t NoFollow = 0x100, FollowLink = 0x400, OpenNoFollow = 0400000;
    const auto Cwd = CurrentDirectory;
    FsHook Hook;
    switch (Number) {
    case 2: Hook.First = Subject(Cwd, Args[0], !(Args[1] & OpenNoFollow)); Hook.Flags = Args[1]; break;
    case 85: Hook.First = Subject(Cwd, Args[0], true); Hook.Flags = 01101; break;
    case 257: Hook.First = Subject(Args[0], Args[1], !(Args[2] & OpenNoFollow)); Hook.Flags = Args[2]; break;
    case 437: {
      uint64_t How {};
      if (Available(Args[2], PF_R) >= sizeof(How)) std::memcpy(&How, reinterpret_cast<const void*>(Args[2]), sizeof(How));
      Hook.First = Subject(Args[0], Args[1], !(How & OpenNoFollow));
      Hook.Flags = How;
      break;
    }
    case 83: case 133: case 84: case 87: case 90: case 92: case 132: case 235: case 76: case 188: case 197:
      Hook.First = Subject(Cwd, Args[0], Number != 83 && Number != 133 && Number != 84 && Number != 87);
      break;
    case 94: case 189: case 198: Hook.First = Subject(Cwd, Args[0], false); break;
    case 258: case 259: case 268: Hook.First = Subject(Args[0], Args[1], Number == 268); break;
    case 263: Hook.First = Subject(Args[0], Args[1], false); Hook.Flags = Args[2]; break;
    case 452: Hook.First = Subject(Args[0], Args[1], !(Args[3] & NoFollow)); break;
    case 260: Hook.First = Subject(Args[0], Args[1], !(Args[4] & NoFollow)); break;
    case 261: Hook.First = Subject(Args[0], Args[1], true); break;
    case 280: if (Args[1]) Hook.First = Subject(Args[0], Args[1], !(Args[3] & NoFollow)); break;
    case 463: case 466: Hook.First = Subject(Args[0], Args[1], !(Args[2] & NoFollow)); break;
    case 88: Hook.First = Subject(Cwd, Args[1], false); break;
    case 266: Hook.First = Subject(Args[1], Args[2], false); break;
    case 86: Hook.First = Subject(Cwd, Args[0], false); Hook.Second = Subject(Cwd, Args[1], false); break;
    case 265: Hook.First = Subject(Args[0], Args[1], Args[4] & FollowLink); Hook.Second = Subject(Args[2], Args[3], false); break;
    case 82: Hook.First = Subject(Cwd, Args[0], false); Hook.Second = Subject(Cwd, Args[1], false); break;
    case 264: Hook.First = Subject(Args[0], Args[1], false); Hook.Second = Subject(Args[2], Args[3], false); break;
    case 316:
      Hook.First = Subject(Args[0], Args[1], false);
      Hook.Second = Subject(Args[2], Args[3], false);
      Hook.Flags = Args[4];
      break;
    default: return std::nullopt;
    }
    return Hook;
  }

  void NotifyRemoved(const FsSubject& Removed) {
    using namespace Switch2Guest;
    if (!Removed.Exists) return;
    // vfs_unlink: the link count changed (IN_ATTRIB); d_delete_notify: the
    // inode's IN_DELETE_SELF and IN_IGNORED once it is gone, then the
    // parent's IN_DELETE.
    const bool Gone = Removed.Directory || (Removed.Links <= 1 && !InodeOpen(Removed.Inode));
    if (!Removed.Directory) InotifyEmit(Removed.Inode, InAttrib, 0, {}, false);
    if (Gone) {
      InodeDeleted(Removed.Inode, Removed.Directory);
      EndInodeWatches(Removed.Inode);
    }
    NotifyChild(Removed.Path, InDelete, 0, Removed.Directory);
  }

  // fsnotify_move.
  void NotifyMove(const FsSubject& From, const FsSubject& To, const FsSubject* Replaced) {
    using namespace Switch2Guest;
    const auto Cookie = ++NextMoveCookie;
    NotifyChild(From.Path, InMovedFrom, Cookie, From.Directory);
    NotifyChild(To.Path, InMovedTo, Cookie, From.Directory);
    if (Replaced) InotifyEmit(Replaced->Inode, InAttrib, 0, {}, Replaced->Directory);
    InotifyEmit(From.Inode, InMoveSelf, 0, {}, From.Directory);
  }

  // Emits the events of a completed syscall.
  void AfterFs(const std::optional<FsHook>& Hook, uint64_t Number, const uint64_t* Args, int64_t Result) {
    using namespace Switch2Guest;
    const bool Open = Number == 2 || Number == 85 || Number == 257 || Number == 437;
    if (Open) {
      // Closing a regular file or directory is an event even if the watch comes later.
      auto* File = GetFile(Result);
      if (File && !File->PathOnly() && (File->RealFile || File->Directory) && File->Path.starts_with('/')) File->NotifyClose = true;
    }
    if (!InotifyWatching()) return;
    auto FileAt = [&](uint64_t FD) -> const OpenFile* { return GetFile(FD); };
    auto Attrib = [&](const FsSubject& Subject) {
      if (Subject.Exists) NotifyPath(Subject.Path, Subject.Inode, InAttrib, Subject.Directory);
    };
    auto Access = [&](uint64_t FD) { if (const auto* File = FileAt(FD); File && File->RealFile) NotifyFile(*File, InAccess); };
    auto Modify = [&](uint64_t FD) { if (const auto* File = FileAt(FD); File && File->RealFile) NotifyFile(*File, InModify); };
    switch (Number) {
    case 0: case 17: case 19: case 295: case 327: if (Result > 0) Access(Args[0]); return;
    case 1: case 18: case 20: case 296: case 328: if (Result > 0) Modify(Args[0]); return;
    case 78: case 217: if (const auto* File = FileAt(Args[0]); File) NotifyFile(*File, InAccess); return;
    case 77: case 285: Modify(Args[0]); return;
    case 40: if (Result > 0) { Access(Args[1]); Modify(Args[0]); } return;
    case 275: case 326: if (Result > 0) { Access(Args[0]); Modify(Args[2]); } return;
    case 91: case 93: case 190: case 199:
      if (const auto* File = FileAt(Args[0]); File) NotifyFile(*File, InAttrib);
      return;
    case 280:
      if (!Args[1]) {
        if (const auto* File = FileAt(Args[0]); File) NotifyFile(*File, InAttrib);
        return;
      }
      break;
    default: break;
    }
    if (!Hook) return;
    const auto& First = Hook->First;
    switch (Number) {
    case 2: case 85: case 257: case 437: {
      const auto* File = GetFile(Result);
      if (!File) return;
      if (!First.Exists && !File->Path.empty()) NotifyChild(File->Path, InCreate, 0, false);
      else if (First.Exists && (Hook->Flags & 01000) && File->RealFile) NotifyFile(*File, InModify);
      if (!File->PathOnly()) NotifyFile(*File, InOpen);
      return;
    }
    case 83: case 258: NotifyChild(First.Path, InCreate, 0, true); return;
    case 133: case 259: case 88: case 266: NotifyChild(First.Path, InCreate, 0, false); return;
    case 86: case 265:
      InotifyEmit(First.Inode, InAttrib, 0, {}, First.Directory);
      NotifyChild(Hook->Second.Path, InCreate, 0, First.Directory);
      return;
    case 87: case 84: NotifyRemoved(First); return;
    case 263: NotifyRemoved(First); return;
    case 82: case 264: case 316: {
      const auto& Second = Hook->Second;
      if (!First.Exists) return;
      constexpr uint64_t Exchange = 2;
      if (Hook->Flags & Exchange) {
        NotifyMove(First, Second, nullptr);
        NotifyMove(Second, First, nullptr);
        return;
      }
      const bool Replaced = Second.Exists && Second.Inode != First.Inode;
      NotifyMove(First, Second, Replaced ? &Second : nullptr);
      if (Replaced && (Second.Directory || (Second.Links <= 1 && !InodeOpen(Second.Inode)))) {
        InodeDeleted(Second.Inode, Second.Directory);
        EndInodeWatches(Second.Inode);
      }
      return;
    }
    case 76: if (First.Exists) NotifyPath(First.Path, First.Inode, InModify, false); return;
    default: Attrib(First); return; // chmod, chown, utimes and xattr changes
    }
  }

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

  // Bytes readable or writable from Address. Pages that are not committed
  // report zero: touching one would fault the CPU and end the app.
  size_t Available(uint64_t Address, uint32_t Flags) const {
    return Task->Memory.Available(Address, static_cast<uint8_t>(Flags));
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
  // The path argument of a failed path system call for the detailed log (" path=..."), with the PID namespace
  // view for /proc paths and the source, type and flags for mount.
  std::string FailedPathDetail(uint64_t Number, const decltype(CoreFrame::State.gregs)& Registers) const {
    int Index = -1;
    switch (Number) {
    case 2: case 4: case 6: case 21: case 83: case 84: case 87: case 89: case 90: case 92: case 133: case 155: case 161: case 166:
      Index = 0; break;
    case 165: case 257: case 258: case 262: case 263: case 267: case 268: case 269: case 280: case 332: case 437: case 439:
      Index = 1; break;
    default: return {};
    }
    static constexpr int ArgRegs[] = {FEXCore::X86State::REG_RDI, FEXCore::X86State::REG_RSI};
    const auto Path = GuestString(Registers[ArgRegs[Index]]);
    if (!Path) return " path=<unreadable>";
    std::string Out = " path=" + std::string(Path->substr(0, 200));
    if (Number == 165) {
      const auto Source = GuestString(Registers[FEXCore::X86State::REG_RDI]);
      const auto Type = GuestString(Registers[FEXCore::X86State::REG_RDX]);
      char Flags[32];
      std::snprintf(Flags, sizeof(Flags), " flags=%#llx", static_cast<unsigned long long>(Registers[FEXCore::X86State::REG_R10]));
      Out += " source=" + (Source ? std::string(Source->substr(0, 80)) : std::string("<null>"));
      Out += " type=" + (Type ? std::string(Type->substr(0, 32)) : std::string("<null>"));
      Out += Flags;
    }
    if (Path->starts_with("/proc")) {
      char View[160];
      std::snprintf(View, sizeof(View), " procview level=%u self=%u task-level=%u task-pid=%u global=%u",
                    ViewPid().Level, ViewOut(Task->PID), Task->Ns.Pid->Level, PidOut(Task->PID), Task->PID);
      Out += View;
    }
    return Out;
  }

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

  // capget(2)/capset(2) header handling (kernel/capability.c cap_validate_magic).
  // Returns the number of 32-bit words per set (1 or 2), or a negative errno.
  int64_t CapabilityWords(uint64_t Header) {
    constexpr uint32_t Version1 = 0x19980330, Version2 = 0x20071026, Version3 = 0x20080522;
    uint32_t Version {};
    if (Available(Header, PF_R) < 8) return Fault;
    std::memcpy(&Version, reinterpret_cast<const void*>(Header), sizeof(Version));
    if (Version == Version1) return 1;
    if (Version == Version2 || Version == Version3) return 2;
    if (CopyOut(Header, Version3)) return Fault; // only the version word is written
    return Invalid;
  }

  int64_t GetCapabilities(uint64_t Header, uint64_t Data) {
    const auto Words = CapabilityWords(Header);
    if (Words < 0) return !Data && Words == Invalid ? 0 : Words;
    if (!Data) return 0;
    int32_t PID {};
    std::memcpy(&PID, reinterpret_cast<const void*>(Header + 4), sizeof(PID));
    if (PID < 0) return Invalid;
    const GuestProcess* Target = Task;
    if (PID && static_cast<uint32_t>(PID) != PidOut(Task->PID) && static_cast<uint32_t>(PID) != PidOut(Current->TID)) {
      Target = ProcessOfId(PidIn(PID));
      if (!Target || Target->Exited) return -3;
    }
    const auto& C = Target->Creds;
    std::array<uint32_t, 6> Out {}; // {effective, permitted, inheritable} per word
    for (int Word = 0; Word < Words; ++Word) {
      Out[Word * 3 + 0] = static_cast<uint32_t>(C.CapEffective >> (32 * Word));
      Out[Word * 3 + 1] = static_cast<uint32_t>(C.CapPermitted >> (32 * Word));
      Out[Word * 3 + 2] = static_cast<uint32_t>(C.CapInheritable >> (32 * Word));
    }
    const size_t Bytes = Words * 3 * sizeof(uint32_t);
    if (Available(Data, PF_W) < Bytes) return Fault;
    std::memcpy(reinterpret_cast<void*>(Data), Out.data(), Bytes);
    return 0;
  }

  int64_t SetCapabilities(uint64_t Header, uint64_t Data) {
    const auto Words = CapabilityWords(Header);
    if (Words < 0) return Words;
    int32_t PID {};
    std::memcpy(&PID, reinterpret_cast<const void*>(Header + 4), sizeof(PID));
    if (PID && static_cast<uint32_t>(PID) != PidOut(Task->PID) && static_cast<uint32_t>(PID) != PidOut(Current->TID)) return -1;
    const size_t Bytes = Words * 3 * sizeof(uint32_t);
    if (!Data || Available(Data, PF_R) < Bytes) return Fault;
    std::array<uint32_t, 6> In {};
    std::memcpy(In.data(), reinterpret_cast<const void*>(Data), Bytes);
    // mk_kernel_cap: bits above CAP_LAST_CAP are dropped, not rejected.
    const auto Gather = [&](int Kind) {
      return (static_cast<uint64_t>(In[3 + Kind]) << 32 | In[Kind]) & Switch2Guest::CapFull;
    };
    return Switch2Guest::CapSet(Task->Creds, Gather(0), Gather(1), Gather(2));
  }

  // The seccomp filter of the current thread, run at syscall entry. Returns the result that replaces the
  // syscall (ERRNO, TRACE and USER_NOTIF without a listener, TRAP), or nothing to run it. Kill actions end the
  // thread or process here.
  std::optional<int64_t> SeccompGate(CoreFrame* Frame) {
    namespace Sc = Switch2Guest::Seccomp;
    using namespace FEXCore::X86State;
    auto& State = Current->Seccomp;
    auto& Registers = Frame->State.gregs;
    // A syscall that blocked runs again from its instruction: its filter has allowed it already.
    if (const auto Rip = std::exchange(Current->SeccompRetryRip, 0); Rip && Rip == Frame->State.rip) return std::nullopt;
    if (State.Mode == Sc::ModeDisabled) return std::nullopt;
    const uint64_t Number = Registers[REG_RAX];
    if (State.Mode == Sc::ModeStrict) {
      if (Sc::StrictAllows(Number)) return std::nullopt;
      SeccompKill(SigKill, false); // __secure_computing_strict: do_exit(SIGKILL)
    }
    Sc::Data Input {};
    Input.Nr = static_cast<int32_t>(Number);
    Input.Arch = Sc::AuditArchX8664;
    Input.InstructionPointer = Frame->State.rip + 2; // after the syscall instruction
    Input.Args[0] = Registers[REG_RDI];
    Input.Args[1] = Registers[REG_RSI];
    Input.Args[2] = Registers[REG_RDX];
    Input.Args[3] = Registers[REG_R10];
    Input.Args[4] = Registers[REG_R8];
    Input.Args[5] = Registers[REG_R9];
    const auto Ret = Sc::Evaluate(State.Filters, Input).Ret;
    switch (static_cast<uint32_t>(Ret & Sc::RetActionFull)) {
    case Sc::RetAllow:
    case Sc::RetLog: return std::nullopt; // LOG: no audit log to write to
    case Sc::RetErrno: return Sc::ErrnoResult(Ret);
    case Sc::RetTrace: // no tracer is attached: the syscall fails with ENOSYS
    case Sc::RetUserNotif: return -38; // no notification listener exists (see ContainerInventory.md)
    case Sc::RetTrap: {
      // force_sig_seccomp: SIGSYS is unblocked and its ignored or caught-while-blocked action is reset first.
      SigInfo Info;
      Info.Signo = static_cast<int32_t>(SigSys);
      Info.Errno = static_cast<int32_t>(Ret & Sc::RetData);
      Info.Code = 1; // SYS_SECCOMP
      const uint64_t CallAddress = Input.InstructionPointer;
      const int32_t SyscallNumber = Input.Nr;
      std::memcpy(Info.Raw, &CallAddress, sizeof(CallAddress));
      std::memcpy(Info.Raw + 8, &SyscallNumber, sizeof(SyscallNumber));
      std::memcpy(Info.Raw + 12, &Input.Arch, sizeof(Input.Arch));
      auto& Action = Task->SignalActions[SigSys - 1];
      if (Action[0] == SigIgnore || (Current->SignalMask & SignalBit(SigSys))) {
        Action = {};
        Current->SignalMask &= ~SignalBit(SigSys);
      }
      SendSignal(*Task, Current, Info);
      return static_cast<int64_t>(Number); // syscall_rollback: the handler sees the syscall number in rax
    }
    case Sc::RetKillThread: SeccompKill(SigSys, false); break;
    default: SeccompKill(SigSys, true); break; // KILL_PROCESS and every unknown action
    }
    return std::nullopt;
  }

  // The thread dies of Signal (SIGSYS for a seccomp kill, SIGKILL for strict mode), taking its process with it
  // when it is the last thread or when Process is set. Never returns.
  void SeccompKill(uint32_t Signal, bool Process) {
    const bool Last = std::none_of(Threads.begin(), Threads.end(), [&](const auto& Thread) {
      return Thread.Core && &Thread != Current && Thread.PID == Task->PID && !Thread.Exited;
    });
    if (Process || Last) ExitProcess(0, Signal);
    else ExitThread();
    std::longjmp(ReturnToHost, 1);
  }

  // seccomp(2) and prctl(PR_SET_SECCOMP). USER_NOTIF notification fds are not implemented: the flag
  // SECCOMP_FILTER_FLAG_NEW_LISTENER (and WAIT_KILLABLE_RECV, which needs it) is EINVAL, as on a kernel without it.
  int64_t SeccompCall(uint32_t Operation, uint32_t Flags, uint64_t Arguments) {
    namespace Sc = Switch2Guest::Seccomp;
    auto& State = Current->Seccomp;
    switch (Operation) {
    case Sc::OpSetModeStrict:
      if (Flags || Arguments) return Invalid;
      if (State.Mode != Sc::ModeDisabled && State.Mode != Sc::ModeStrict) return Invalid;
      State.Mode = Sc::ModeStrict;
      return 0;
    case Sc::OpSetModeFilter: return SeccompSetFilter(Flags, Arguments);
    case Sc::OpGetActionAvail: {
      if (Flags) return Invalid;
      uint32_t Action {};
      if (Available(Arguments, PF_R) < sizeof(Action)) return Fault;
      std::memcpy(&Action, reinterpret_cast<const void*>(Arguments), sizeof(Action));
      return Sc::ActionKnown(Action) && Action != Sc::RetUserNotif ? 0 : -95; // EOPNOTSUPP
    }
    case Sc::OpGetNotifSizes: return Flags ? Invalid : CopyOut(Arguments, Sc::NotifSizes {});
    default: return Invalid;
    }
  }

  int64_t SeccompSetFilter(uint32_t Flags, uint64_t Address) {
    namespace Sc = Switch2Guest::Seccomp;
    auto& State = Current->Seccomp;
    constexpr uint32_t Supported = Sc::FlagTsync | Sc::FlagLog | Sc::FlagSpecAllow | Sc::FlagTsyncEsrch;
    if (Flags & ~Supported) return Invalid;
    // struct sock_fprog: u16 len, pad, pointer.
    if (Available(Address, PF_R) < 16) return Fault;
    uint16_t Length {};
    uint64_t Pointer {};
    std::memcpy(&Length, reinterpret_cast<const void*>(Address), sizeof(Length));
    std::memcpy(&Pointer, reinterpret_cast<const void*>(Address + 8), sizeof(Pointer));
    if (!Length || Length > Sc::MaxProgram) return Invalid;
    if (!Task->NoNewPrivileges && !Task->Creds.OwnCapable(CapSysAdmin)) return -13; // EACCES
    if (Available(Pointer, PF_R) < Length * sizeof(Sc::Insn)) return Fault;
    std::vector<Sc::Insn> Program(Length);
    std::memcpy(Program.data(), reinterpret_cast<const void*>(Pointer), Length * sizeof(Sc::Insn));
    if (!Sc::Validate(Program)) return Invalid;
    if (State.Mode == Sc::ModeStrict) return Invalid; // seccomp_may_assign_mode
    std::vector<GuestThread*> Others;
    if (Flags & Sc::FlagTsync) {
      for (auto& Thread : Threads) {
        if (Thread.Core && !Thread.Exited && Thread.PID == Task->PID && &Thread != Current) Others.push_back(&Thread);
      }
      std::sort(Others.begin(), Others.end(), [](const auto* L, const auto* R) { return L->TID < R->TID; });
      for (const auto* Thread : Others) {
        if (!Sc::CanSync(Thread->Seccomp, State)) return (Flags & Sc::FlagTsyncEsrch) ? -3 : static_cast<int64_t>(PidOut(Thread->TID));
      }
    }
    if (const auto Error = Sc::Attach(State, std::move(Program), Flags & Sc::FlagLog)) return Error;
    for (auto* Thread : Others) Thread->Seccomp = State;
    return 0;
  }

  int64_t GuestPrivilegeControl(uint64_t Operation, uint64_t Value, uint64_t Arg3, uint64_t Arg4, uint64_t Arg5) {
    switch (Operation) {
    case 1: // PR_SET_PDEATHSIG
      if (Value > SigMax) return Invalid;
      Task->ParentDeathSignal = Value;
      return 0;
    case 2: return CopyOut(Value, static_cast<int32_t>(Task->ParentDeathSignal)) ? Fault : 0;
    case 3: return Task->Dumpable; // PR_GET_DUMPABLE
    case 4:
      if (Value > 1) return Invalid;
      Task->Dumpable = Value;
      return 0;
    case 15: { // PR_SET_NAME: up to 15 bytes, always terminated
      std::array<char, 16> Name {};
      const auto Size = AccessiblePrefix(Value, Name.size() - 1, PF_R);
      if (!Size) return Fault;
      std::memcpy(Name.data(), reinterpret_cast<const void*>(Value), Size);
      Name[std::strlen(Name.data())] = 0;
      Task->Name = Name;
      return 0;
    }
    case 16: return CopyOut(Value, Task->Name) ? Fault : 0;
    // cap_task_prctl ignores the unused arguments of the keep-caps options;
    // glibc's variadic prctl leaves whatever was in those registers.
    case 7: // PR_GET_KEEPCAPS
      return Task->Creds.Secure(Switch2Guest::SecbitKeepCaps) ? 1 : 0;
    case 8: // PR_SET_KEEPCAPS
      if (Value > 1) return Invalid;
      if (Task->Creds.Secure(Switch2Guest::SecbitKeepCapsLocked)) return -1;
      if (Value) Task->Creds.Securebits |= Switch2Guest::SecbitKeepCaps;
      else Task->Creds.Securebits &= ~Switch2Guest::SecbitKeepCaps;
      return 0;
    case 23: // PR_CAPBSET_READ
      if (Value > Switch2Guest::CapLast) return Invalid;
      return Task->Creds.CapBounding >> Value & 1;
    case 24: // PR_CAPBSET_DROP
      if (!Task->Creds.OwnCapable(Switch2Guest::CapSetpcap)) return -1;
      if (Value > Switch2Guest::CapLast) return Invalid;
      Task->Creds.CapBounding &= ~(uint64_t {1} << Value);
      return 0;
    case 47: { // PR_CAP_AMBIENT: Value is the sub-operation, Arg3 the capability
      auto& C = Task->Creds;
      if (Value == 4) { // PR_CAP_AMBIENT_CLEAR_ALL
        if (Arg3 || Arg4 || Arg5) return Invalid;
        C.CapAmbient = 0;
        return 0;
      }
      if (Arg4 || Arg5 || Arg3 > Switch2Guest::CapLast) return Invalid;
      const uint64_t Bit = uint64_t {1} << Arg3;
      if (Value == 1) return (C.CapAmbient & Bit) ? 1 : 0; // IS_SET
      if (Value == 2) { // RAISE
        if (!(C.CapPermitted & Bit) || !(C.CapInheritable & Bit) || C.Secure(Switch2Guest::SecbitNoCapAmbientRaise)) return -1;
        C.CapAmbient |= Bit;
        return 0;
      }
      if (Value == 3) { // LOWER
        C.CapAmbient &= ~Bit;
        return 0;
      }
      return Invalid;
    }
    case 36: Task->ChildSubreaper = Value != 0; return 0;
    case 37: return CopyOut(Value, static_cast<int32_t>(Task->ChildSubreaper)) ? Fault : 0;
    case 21: return Current->Seccomp.Mode; // PR_GET_SECCOMP: the other arguments are ignored
    case 22: // PR_SET_SECCOMP: strict ignores arg3, filter takes the struct sock_fprog
      if (Value == Switch2Guest::Seccomp::ModeStrict) return SeccompCall(Switch2Guest::Seccomp::OpSetModeStrict, 0, 0);
      if (Value == Switch2Guest::Seccomp::ModeFilter) return SeccompCall(Switch2Guest::Seccomp::OpSetModeFilter, 0, Arg3);
      return Invalid;
    case 38: case 39: break;
    default: return Invalid;
    }
    if (Arg3 || Arg4 || Arg5) return Invalid;
    if (Operation == 39) return Value ? Invalid : Task->NoNewPrivileges;
    if (Value != 1) return Invalid;
    Task->NoNewPrivileges = true;
    return 0;
  }

  bool InGuestGroup(uint32_t Group, bool Real = false) const {
    const auto& Extra = Task->SupplementaryGroups;
    return Group == Task->Creds.Gid[Real ? 0 : 3] || std::binary_search(Extra.begin(), Extra.end(), Group);
  }

  int64_t GuestGroups(bool Set, uint32_t Count, uint64_t Address) {
    auto& Groups = Task->SupplementaryGroups;
    // may_setgroups: CAP_SETGID in the caller's namespace, and no "deny" or missing gid map there.
    if (Set && (!Task->Creds.OwnCapable(Switch2Guest::CapSetgid) || !Switch2Guest::Ns::MaySetgroups(*Task->Ns.User))) return -1;
    if (!Set && !Count) return Groups.size();
    if (Count > INT32_MAX || (Set ? Count > 65536 : Count < Groups.size())) return Invalid;
    const size_t Bytes = sizeof(uint32_t) * (Set ? Count : Groups.size());
    if (Bytes && AccessiblePrefix(Address, Bytes, Set ? PF_R : PF_W) < Bytes) return Fault;
    if (Set) {
      std::vector<uint32_t> New(Count);
      if (Bytes) std::memcpy(New.data(), reinterpret_cast<const void*>(Address), Bytes);
      if (std::ranges::find(New, UINT32_MAX) != New.end()) return Invalid;
      for (auto& Group : New) {
        const auto Kernel = KgidIn(Group);
        if (!Kernel) return Invalid;
        Group = *Kernel;
      }
      std::ranges::sort(New);
      Groups.swap(New);
      return 0;
    }
    if (Bytes) {
      auto* Out = reinterpret_cast<uint32_t*>(Address);
      for (size_t Index = 0; Index < Groups.size(); ++Index) {
        const uint32_t Value = GidOut(Groups[Index]);
        std::memcpy(Out + Index, &Value, sizeof(Value));
      }
    }
    return Groups.size();
  }

  int64_t GetGuestIDs(bool Group, uint64_t Real, uint64_t Effective, uint64_t Saved) {
    const auto& IDs = Group ? Task->Creds.Gid : Task->Creds.Uid;
    const auto Out = [&](uint32_t Id) { return Group ? GidOut(Id) : UidOut(Id); };
    if (CopyOut(Real, Out(IDs[0])) || CopyOut(Effective, Out(IDs[1])) || CopyOut(Saved, Out(IDs[2]))) return Fault;
    return 0;
  }

  // setuid (0), setgid (1), setreuid (2), setregid (3), setresuid (4), setresgid (5), setfsuid (6), setfsgid (7).
  // The arguments are ids of the caller's user namespace: each becomes a kernel id first (EINVAL when unmapped,
  // except setfsuid and setfsgid, which then change nothing and return the current id).
  int64_t SetIds(int Which, uint64_t A, uint64_t B, uint64_t C) {
    SyncCreds(*Task);
    auto& Creds = Task->Creds;
    const bool Group = Which & 1;
    const auto To = [&](uint64_t V) { return Group ? KgidIn(static_cast<uint32_t>(V)) : KuidIn(static_cast<uint32_t>(V)); };
    if (Which >= 6) {
      const auto Kernel = To(A);
      const auto Old = Group ? Creds.Gid[3] : Creds.Uid[3];
      if (!Kernel) return Group ? GidOut(Old) : UidOut(Old);
      Group ? Switch2Guest::SetFsGid(Creds, *Kernel) : Switch2Guest::SetFsUid(Creds, *Kernel);
      return Group ? GidOut(Old) : UidOut(Old);
    }
    const auto K1 = To(A);
    const auto K2 = Which >= 2 ? To(B) : std::optional<uint32_t> {0};
    const auto K3 = Which >= 4 ? To(C) : std::optional<uint32_t> {0};
    if (!K1 || !K2 || !K3) return Invalid;
    switch (Which) {
    case 0: return Switch2Guest::SetUid(Creds, *K1);
    case 1: return Switch2Guest::SetGid(Creds, *K1);
    case 2: return Switch2Guest::SetReUid(Creds, *K1, *K2);
    case 3: return Switch2Guest::SetReGid(Creds, *K1, *K2);
    case 4: return Switch2Guest::SetResUid(Creds, *K1, *K2, *K3);
    default: return Switch2Guest::SetResGid(Creds, *K1, *K2, *K3);
    }
  }

  // True when the credential holds Cap; access(2) and friends use the real
  // ids, where Linux's override_creds keeps capabilities only for real root.
  bool HasCap(uint32_t Cap, bool Real, uint64_t Inode) const {
    const auto& Creds = Task->Creds;
    bool Has;
    if (!Real || (Creds.Uid[0] == Creds.Uid[1] && Creds.Gid[0] == Creds.Gid[1])) Has = Creds.OwnCapable(Cap);
    else Has = Creds.Uid[0] == Switch2Guest::Ns::RootKuid(*Task->Ns.User) && (Creds.CapPermitted >> Cap & 1);
    return Has && InodeMapped(Inode, true);
  }

  // privileged_wrt_inode_uidgid: the inode's owner (and group) have ids in the caller's user namespace.
  bool InodeMapped(uint64_t Inode, bool NeedGroup) const {
    const auto [Uid, Gid] = InodeOwner(Inode);
    return Task->Ns.User->Uid.FromKernel(Uid) && (!NeedGroup || Task->Ns.User->Gid.FromKernel(Gid));
  }

  // capable_wrt_inode_uidgid: the capability in the caller's own user namespace over an inode whose owner and
  // group it maps. In the initial namespace every id maps, so this is capable().
  bool CapWrtInode(uint32_t Cap, uint64_t Inode) const { return Task->Creds.OwnCapable(Cap) && InodeMapped(Inode, true); }

  std::pair<uint32_t, uint32_t> InodeOwner(uint64_t Inode) const {
    const auto Record = Inodes.find(Inode);
    if (Record != Inodes.end() && Record->second.Owner) return *Record->second.Owner;
    return {1000, 1000};
  }

  bool OwnsInodeOrRoot(uint64_t Inode) const {
    // inode_owner_or_capable: the owner, or CAP_FOWNER in a user namespace that maps the owner.
    return InodeOwner(Inode).first == Task->Creds.Uid[3] || (Task->Creds.OwnCapable(CapFowner) && InodeMapped(Inode, false));
  }

  bool InodeAllows(uint64_t Inode, uint32_t Mode, unsigned Requested, bool Real = false) const {
    const auto [UID, GID] = InodeOwner(Inode);
    const auto User = Task->Creds.Uid[Real ? 0 : 3];
    const bool Directory = (Mode & 0170000) == 0040000;
    // generic_permission: CAP_DAC_OVERRIDE / CAP_DAC_READ_SEARCH grant access
    // whatever the mode bits and ACL say (execute needs some x bit on files).
    const auto Override = [&] {
      if (HasCap(Switch2Guest::CapDacOverride, Real, Inode) && (!(Requested & 1) || Directory || (Mode & 0111))) return true;
      return HasCap(CapDacReadSearch, Real, Inode) && ((Requested == 4) || (Directory && !(Requested & 2)));
    };
    if (Override()) return true;
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
    const auto Opened = SdFileError(SdRetry(fsFsOpenDirectory, &SdCard, Path.c_str(), FsDirOpenMode_ReadDirs | FsDirOpenMode_NoFileSize, &Directory));
    if (Opened) return Opened;
    s64 Count {};
    const auto Status = SdFileError(SdRetry(fsDirGetEntryCount, &Directory, &Count));
    fsDirClose(&Directory);
    return Status ? Status : 2 + Count;
  }

  template<typename Operation>
  int64_t WithDiskFile(const OpenFile& File, uint32_t Mode, Operation Use) {
    const auto Node = Inodes.find(File.Inode);
    const auto Path = Node != Inodes.end() && Node->second.ManagedFile ? ObjectPath(Node->second.BackingObject.value_or(File.Inode)) : File.Path;
    FsFileSystem SdCard {};
    if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
    FsFile Native {};
    const auto Status = SdRetry(fsFsOpenFile, &SdCard, Path.c_str(), Mode, &Native);
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
      return SdFileError(SdRetry(fsFileGetSize, &Native, &Size));
    });
  }

  int64_t FlushDiskFile(const OpenFile& File) {
    const auto Error = WithDiskFile(File, FsOpenMode_Write, [](FsFile& Native) {
      return SdFileError(SdRetry(fsFileFlush, &Native));
    });
    if (!Error) DirtyFileData.erase(File.Inode);
    return Error;
  }

  static int64_t ZeroDiskRange(FsFile& Native, uint64_t Begin, uint64_t End) {
    const std::array<uint8_t, 4096> Zeros {};
    while (Begin < End) {
      const auto Count = std::min<uint64_t>(Zeros.size(), End - Begin);
      if (const auto Error = SdFileError(SdRetry(fsFileWrite, &Native, Begin, Zeros.data(), Count, FsWriteOption_None)); Error) return Error;
      Begin += Count;
    }
    return 0;
  }

  static int64_t ResizeNativeFile(FsFile& Native, uint64_t Size) {
    s64 Old {};
    if (const auto Error = SdFileError(SdRetry(fsFileGetSize, &Native, &Old)); Error) return Error;
    if (Size <= static_cast<uint64_t>(Old)) return SdFileError(SdRetry(fsFileSetSize, &Native, Size));
    if (const auto Error = ZeroDiskRange(Native, Old, Size); Error) {
      SdRetry(fsFileSetSize, &Native, Old);
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
    if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
    const auto Commit = [&] {
      if (const auto Error = SaveInodeStore(); Error) return Error;
      return SdFileError(SdRetry(fsFsCommit, &SdCard));
    };
    auto Error = Commit();
    const auto Path = ObjectPath(Candidate);
    if (!Error) Error = SdFileError(SdRetry(fsFsCreateFile, &SdCard, Path.c_str(), 0, 0));
    if (!Error) {
      FsFile Output {};
      Error = SdFileError(SdRetry(fsFsOpenFile, &SdCard, Path.c_str(), FsOpenMode_Write | FsOpenMode_Append, &Output));
      if (!Error) {
        Error = WithDiskFile(File, FsOpenMode_Read, [&](FsFile& Input) {
          std::array<char, 4096> Scratch;
          return Source.CopyTo(Destination, Scratch,
            [&](uint64_t Offset, std::span<char> Chunk) -> int64_t {
              u64 Count {};
              const auto ReadError = SdFileError(SdRetry(fsFileRead, &Input, Offset, Chunk.data(), Chunk.size(), FsReadOption_None, &Count));
              return ReadError ? ReadError : static_cast<int64_t>(Count);
            },
            [&](uint64_t Offset, std::span<const char> Chunk) {
              return SdFileError(SdRetry(fsFileWrite, &Output, Offset, Chunk.data(), Chunk.size(), FsWriteOption_None));
            }, Update, ShiftBoundary, SourceShift);
        });
        if (!Error) Error = SdFileError(SdRetry(fsFileFlush, &Output));
        fsFileClose(&Output);
      }
    }
    if (!Error) Error = SdFileError(SdRetry(fsFsCommit, &SdCard));
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
          const auto Delete = SdFileError(SdRetry(fsFsDeleteFile, &SdCard, ObjectPath(Object).c_str()));
          if (!Delete || Delete == -2) Removed.push_back(Object);
        }
        if (!Removed.empty() && R_SUCCEEDED(SdRetry(fsFsCommit, &SdCard))) {
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
      if (R_FAILED(SdRetry(fsFileGetSize, &Native, &Allocated)) ||
          (Size < static_cast<uint64_t>(Allocated) && R_FAILED(SdRetry(fsFileSetSize, &Native, Size)))) {
        WriteLog("Guest truncate changed logical EOF but could not release native backing space.");
      }
      return 0;
    }
    if (Size > *OldLogical) {
      s64 Allocated {};
      if (const auto Error = SdFileError(SdRetry(fsFileGetSize, &Native, &Allocated)); Error) return Error;
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
        if (const auto Error = SdFileError(SdRetry(fsFileGetSize, &Native, &Allocated)); Error) return Error;
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
      if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
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
    const auto* Opened = GetFile(FD);
    const ProcViewScope View {*this, Opened ? Opened->Mount : nullptr};
    const auto Result = FileStatUnmounted(FD, Info);
    StatMount = Opened ? Opened->Mount : nullptr;
    if (!Result) ApplyMountDevice(Info, StatMount);
    return Result;
  }

  int64_t FileStatUnmounted(uint64_t FD, LinuxStat& Info) {
    if (const auto* Anonymous = GetFile(FD); Anonymous && Anonymous->AnonName) {
      Info = AnonymousStat();
      return 0;
    }
    if (const auto* Namespace = GetFile(FD); Namespace && Namespace->NsObject) {
      // nsfs: a read-only file owned by kernel root, identified by the namespace's inode number.
      Info = MakeStat(0100444, 0, Namespace->NsObject->Ino);
      Info.Device = NsfsDevice;
      Info.Uid = Info.Gid = 0;
      Info.Links = 1;
      return 0;
    }
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
      const auto Mode = Special ? Special : Target ? 0120777 : File->Directory ? 0040755 : File->Pipe ? 0010600 : File->Socket || File->Inet ? 0140777 :
                        File->Console != ConsoleStream::None ? 0020600 : File->Memory ? 0100777 : CharDevAt(File->Path) != CharDev::None ? 0020666 : ProcFile(File->Path) ? ProcFileMode(File->Path) : RegularMode(File->Path);
      Info = MakeStat(ModeForInode(File->Inode, Mode), Size, File->Inode);
      const auto VirtualDir = File->Directory ? VirtualDirectory(File->Path) : std::string_view {};
      if (VirtualDir == ShmDirectory) {
        ShmDirectoryStat(Info);
      } else if (!VirtualDir.empty()) {
        Info.Device = VirtualDir == "/dev" ? DevDevice : ProcDevice;
        Info.Mode = 0040555;
      }
      if (File->Memory && File->Path.starts_with("/dev/shm/")) Info.Device = ShmDevice;
      if (ProcFile(File->Path) || !ProcTarget.empty()) Info.Device = ProcDevice;
      if (CharDevAt(File->Path) != CharDev::None) Info.Device = DevDevice;
      if (File->RealFile) Info.Blocks = Allocated / 512 + (Allocated % 512 != 0);
      if (File->Memory) Info.Blocks = File->Memory->AllocatedBytes() / 512;
      if (File->Mqueue) Info.Size = 80; // mqueue inodes report FILENT_SIZE whatever they hold
      if (File->RemovedDirectory || IsUnlinked(File->Inode)) {
        Info.Links = 0;
      } else if (!VirtualDir.empty()) {
        Info.Links = 2;
      } else if (File->Directory) {
        int64_t Links = 2;
        if (!IsOverflowDirectory(File->Path)) {
          FsFileSystem SdCard {};
          if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
          Links = DirectoryLinks(SdCard, File->Path);
          fsFsClose(&SdCard);
          if (Links < 0) return Links;
        }
        Info.Links = Links + OverflowSubdirectories(File->Path);
      }
      if (CharDevAt(File->Path) != CharDev::None) Info.Rdev = CharDevRdev(CharDevAt(File->Path));
      if (File->Console == ConsoleStream::Terminal) Info.Rdev = 0x500;
    } else {
      return BadFile;
    }
    return 0;
  }

  // The owner as the caller's user namespace names it (from_kuid_munged).
  void MungeOwner(LinuxStat& Info) const {
    Info.Uid = UidOut(Info.Uid);
    Info.Gid = GidOut(Info.Gid);
  }

  int64_t StatGuest(uint64_t FD, uint64_t Output) {
    LinuxStat Info {};
    const auto Result = FileStat(FD, Info);
    if (Result) return Result;
    MungeOwner(Info);
    return CopyOut(Output, Info);
  }

  int64_t StatAtGuest(uint64_t FD, uint64_t Address, uint64_t Output, uint64_t Flags) {
    LinuxStat Info {};
    const auto Result = GetStatAt(FD, Address, Flags, Info);
    if (Result) return Result;
    MungeOwner(Info);
    return CopyOut(Output, Info);
  }

  int64_t AccessGuest(uint64_t FD, uint64_t Address, uint64_t Mode, uint64_t Flags) {
    constexpr uint64_t EffectiveAccess = 0x200;
    constexpr uint64_t SupportedFlags = 0x100 | EffectiveAccess | 0x1000;
    if ((Mode & ~7ULL) || (Flags & ~SupportedFlags)) return Invalid;
    // The guest's real and effective credentials are identical.
    LinuxStat Info {};
    const auto Result = GetStatAt(FD, Address, Flags & ~EffectiveAccess, Info);
    if (Result < 0) return Result;
    // do_faccessat: asking for write access to a file, directory or link below a read-only mount is EROFS
    const auto Type = Info.Mode & 0170000;
    if ((Mode & 2) && (Type == 0100000 || Type == 0040000 || Type == 0120000)) {
      if (const auto Denied = MountDenies(StatMount, false)) return Denied;
    }
    return InodeAllows(Info.Inode, Info.Mode, Mode, !(Flags & EffectiveAccess)) ? 0 : -13;
  }

  // A process of /proc: "/proc/<pid>/<rest>" with the pid as the caller's PID namespace numbers it ("self" is the
  // caller). A process the caller cannot see (or that is gone) has no entry.
  struct ProcEntry {
    const GuestProcess* Process {};
    std::string_view Rest;
  };

  // The PID namespace a proc mount shows (proc_pid_ns of the superblock): the mount the last path walk ended in, or
  // the mount of the open file a caller names with ProcViewScope. Anything else is read through the caller's own.
  mutable MountRef ProcViewMount;
  struct ProcViewScope {
    const GuestSyscalls& Owner;
    MountRef Saved;
    ProcViewScope(const GuestSyscalls& Owner, const MountRef& Mount) : Owner {Owner}, Saved {std::move(Owner.ProcViewMount)} {
      Owner.ProcViewMount = Mount;
    }
    ~ProcViewScope() { Owner.ProcViewMount = std::move(Saved); }
  };
  const Switch2Guest::Ns::PidNs& ViewPid() const {
    const auto& Mount = ProcViewMount ? ProcViewMount : LastMount;
    if (Mount && Mount->Sb && Mount->Sb->Pid) return *Mount->Sb->Pid;
    return *Task->Ns.Pid;
  }
  uint32_t ViewOut(uint32_t Global) const { return ViewPid().ToLocal(Global); }

  std::optional<ProcEntry> ProcOf(std::string_view Name) const {
    if (!Name.starts_with("/proc/")) return std::nullopt;
    Name.remove_prefix(6);
    const auto Slash = Name.find('/');
    const auto Who = Name.substr(0, Slash);
    const GuestProcess* Process {};
    if (Who == "self") {
      if (!ViewOut(Task->PID)) return std::nullopt; // the caller is outside the namespace this mount shows
      Process = Task;
    } else {
      const auto Number = ProcPID(Who);
      if (!Number) return std::nullopt;
      const auto Found = Processes.find(ViewPid().ToGlobal(Number));
      if (Found == Processes.end() || Found->second.Reaped) return std::nullopt;
      Process = &Found->second;
    }
    return ProcEntry {Process, Slash == Name.npos ? std::string_view {} : Name.substr(Slash + 1)};
  }

  // The character devices of /dev. They are real device nodes: st_rdev, mode 0666 and the read/write rules of
  // drivers/char/mem.c. /dev/tty exists always (it can be bind-mounted) but opens only for a process with a
  // controlling terminal, which here means an interactive run.
  enum class CharDev { None, Null, Zero, Full, Random, Urandom, Tty };
  static CharDev CharDevAt(std::string_view GuestPath) {
    if (GuestPath == "/dev/null") return CharDev::Null;
    if (GuestPath == "/dev/zero") return CharDev::Zero;
    if (GuestPath == "/dev/full") return CharDev::Full;
    if (GuestPath == "/dev/random") return CharDev::Random;
    if (GuestPath == "/dev/urandom") return CharDev::Urandom;
    if (GuestPath == "/dev/tty") return CharDev::Tty;
    return CharDev::None;
  }
  // The data devices (everything but the terminal).
  static bool IsDataDev(std::string_view GuestPath) {
    const auto Kind = CharDevAt(GuestPath);
    return Kind != CharDev::None && Kind != CharDev::Tty;
  }
  static uint64_t CharDevRdev(CharDev Kind) {
    switch (Kind) {
    case CharDev::Null: return 0x103;    // 1:3
    case CharDev::Zero: return 0x105;    // 1:5
    case CharDev::Full: return 0x107;    // 1:7
    case CharDev::Random: return 0x108;  // 1:8
    case CharDev::Urandom: return 0x109; // 1:9
    case CharDev::Tty: return 0x500;     // 5:0
    default: return 0;
    }
  }
  // read_null, read_zero (also /dev/full) and the random devices' read.
  static int64_t ReadCharDev(CharDev Kind, void* Buffer, uint64_t Count) {
    auto* Bytes = static_cast<char*>(Buffer);
    if (Kind == CharDev::Null) return 0;
    if (Kind == CharDev::Random || Kind == CharDev::Urandom) {
      for (uint64_t Done = 0; Done < Count;) {
        const auto Chunk = std::min<uint64_t>(Count - Done, 65536);
        randomGet(Bytes + Done, Chunk);
        Done += Chunk;
      }
    } else {
      std::memset(Bytes, 0, Count);
    }
    return static_cast<int64_t>(Count);
  }
  // write_null (also zero and the random devices, which mix the data in) and write_full.
  static int64_t WriteCharDev(CharDev Kind, uint64_t Count) { return Kind == CharDev::Full ? -28 : static_cast<int64_t>(Count); }

  // The pid numbers of Id from the caller's namespace down to Of's own, as NStgid, NSpid and NSpgid print them (0
  // where the process or group is not visible at that level).
  std::string PidChain(uint32_t Id, const GuestProcess& Of) const {
    std::vector<const Switch2Guest::Ns::PidNs*> Chain;
    for (const auto* N = Of.Ns.Pid.get(); N && N->Level >= ViewPid().Level; N = N->Parent.get()) Chain.push_back(N);
    std::string Out;
    for (auto It = Chain.rbegin(); It != Chain.rend(); ++It) Out += (It == Chain.rbegin() ? "" : "\t") + std::to_string((*It)->ToLocal(Id));
    return Out;
  }

  // /proc/<pid>/status as the caller reads it: ids and numbers in the caller's namespaces.
  std::string ProcStatus(const GuestProcess& P) const {
    char Line[160];
    std::string Out;
    const auto Add = [&](const char* Format, auto... Values) {
      std::snprintf(Line, sizeof(Line), Format, Values...);
      Out += Line;
    };
    const char* Comm = P.Name[0] ? P.Name.data() : "fex-guest";
    Add("Name:\t%s\nUmask:\t%04o\n", Comm, P.Fs->CreationMask);
    Add("State:\t%s\n", P.Exited ? "Z (zombie)" : P.Stopped ? "T (stopped)" : "R (running)");
    size_t Count {};
    for (const auto& Thread : Threads) Count += Thread.Core && !Thread.Exited && Thread.PID == P.PID;
    Add("Tgid:\t%u\nNgid:\t0\nPid:\t%u\nPPid:\t%u\nTracerPid:\t0\n", ViewOut(P.PID), ViewOut(P.PID), ViewOut(P.ParentPID));
    Add("Uid:\t%u\t%u\t%u\t%u\n", UidOut(P.Creds.Uid[0]), UidOut(P.Creds.Uid[1]), UidOut(P.Creds.Uid[2]), UidOut(P.Creds.Uid[3]));
    Add("Gid:\t%u\t%u\t%u\t%u\n", GidOut(P.Creds.Gid[0]), GidOut(P.Creds.Gid[1]), GidOut(P.Creds.Gid[2]), GidOut(P.Creds.Gid[3]));
    Out += "Groups:\t";
    for (const auto Group : P.SupplementaryGroups) Out += std::to_string(GidOut(Group)) + " ";
    Out += "\n";
    Out += "NStgid:\t" + PidChain(P.PID, P) + "\nNSpid:\t" + PidChain(P.PID, P) + "\nNSpgid:\t" + PidChain(P.Group, P) +
           "\nNSsid:\t" + PidChain(P.Session, P) + "\n";
    Add("Threads:\t%zu\n", Count);
    Add("CapInh:\t%016llx\nCapPrm:\t%016llx\nCapEff:\t%016llx\nCapBnd:\t%016llx\nCapAmb:\t%016llx\n",
        static_cast<unsigned long long>(P.Creds.CapInheritable), static_cast<unsigned long long>(P.Creds.CapPermitted),
        static_cast<unsigned long long>(P.Creds.CapEffective), static_cast<unsigned long long>(P.Creds.CapBounding),
        static_cast<unsigned long long>(P.Creds.CapAmbient));
    // Seccomp state of the thread group leader (the main thread), or of any live thread.
    const GuestThread* Leader {};
    for (const auto& Thread : Threads) {
      if (!Thread.Core || Thread.Exited || Thread.PID != P.PID) continue;
      if (!Leader || Thread.TID == P.PID) Leader = &Thread;
    }
    Add("NoNewPrivs:\t%d\nSeccomp:\t%u\nSeccomp_filters:\t%zu\n", P.NoNewPrivileges ? 1 : 0, Leader ? Leader->Seccomp.Mode : 0,
        Leader ? Switch2Guest::Seccomp::FilterCount(Leader->Seccomp.Filters) : size_t {});
    return Out;
  }

  // The part of /proc/sys the runtime serves (Milestone 6 does the rest): the ids the kernel shows for an unmapped
  // id, the namespace limits, and the identity strings of the caller's UTS namespace.
  static constexpr std::string_view UnameRelease = "5.15.0";
  static constexpr uint32_t MaxNamespaces = 126662; // the default of the checking host (it follows the memory size)
  static constexpr std::array<std::string_view, 7> NamespaceLimits {"cgroup", "ipc", "mnt", "net", "pid", "user", "uts"};

  static bool ProcSysDirectory(std::string_view Name) { return Name == "/proc/sys" || Name == "/proc/sys/kernel" || Name == "/proc/sys/user"; }

  static std::vector<DirectoryEntry> ProcSysEntries(std::string_view Directory) {
    std::vector<DirectoryEntry> Entries;
    if (Directory == "/proc/sys") {
      Entries.insert(Entries.end(), {{"kernel", true}, {"user", true}});
    } else if (Directory == "/proc/sys/kernel") {
      for (const auto* Name : {"domainname", "hostname", "osrelease", "ostype", "overflowgid", "overflowuid"}) Entries.push_back({Name, false});
    } else {
      for (const auto Name : NamespaceLimits) Entries.push_back({"max_" + std::string(Name) + "_namespaces", false});
    }
    return Entries;
  }

  std::optional<std::string> ProcSysFile(std::string_view Name) const {
    if (!Name.starts_with("/proc/sys/")) return std::nullopt;
    Name.remove_prefix(10);
    if (Name.starts_with("kernel/")) {
      Name.remove_prefix(7);
      if (Name == "overflowuid" || Name == "overflowgid") return std::to_string(Switch2Guest::Ns::OverflowId) + "\n";
      if (Name == "hostname") return Task->Ns.Uts->Hostname + "\n";
      if (Name == "domainname") return Task->Ns.Uts->Domainname + "\n";
      if (Name == "ostype") return std::string {"Linux\n"};
      if (Name == "osrelease") return std::string(UnameRelease) + "\n";
    } else if (Name.starts_with("user/max_") && Name.ends_with("_namespaces")) {
      const auto Which = Name.substr(9, Name.size() - 9 - 11);
      if (std::find(NamespaceLimits.begin(), NamespaceLimits.end(), Which) != NamespaceLimits.end()) return std::to_string(MaxNamespaces) + "\n";
    }
    return std::nullopt;
  }

  // Mode of a proc file: the /proc/sys tunables are 0644, the rest 0444.
  static uint32_t ProcFileMode(std::string_view Name) { return Name.starts_with("/proc/sys/") ? 0100644 : 0100444; }

  std::optional<std::string> ProcFile(std::string_view Name) const {
    if (const auto Sys = ProcSysFile(Name)) return Sys;
    if (Name == "/proc/version") return std::string {"Linux version 5.0.0 (FEX Switch 2 guest)\n"};
    const auto Entry = ProcOf(Name);
    if (!Entry) return std::nullopt;
    const auto& Rest = Entry->Rest;
    if (Rest == "status") return ProcStatus(*Entry->Process);
    if (Rest == "uid_map" || Rest == "gid_map") return Switch2Guest::Ns::RenderIdMap(*Entry->Process->Ns.User, Rest == "gid_map", *Task->Ns.User);
    if (Rest == "setgroups") return Switch2Guest::Ns::RenderSetgroups(*Entry->Process->Ns.User);
    if (Rest == "mountinfo") return Switch2Guest::Mounts::MountInfo(TableOf(*Entry->Process->Ns.Mnt), RootOf(*Entry->Process));
    if (Rest == "mounts") return Switch2Guest::Mounts::MountsText(TableOf(*Entry->Process->Ns.Mnt), RootOf(*Entry->Process));
    return std::nullopt;
  }

  // Opening /proc/<pid>/{uid_map,gid_map,setgroups} for writing: the files are 0644 owned by the process's
  // owner (CAP_DAC_OVERRIDE over that owner works); setgroups also needs CAP_SYS_ADMIN over the namespace.
  std::optional<OpenFile::ProcWriter> OpenProcWriter(std::string_view GuestPath) const {
    const auto Entry = ProcOf(GuestPath);
    if (!Entry) return std::nullopt;
    int Kind;
    if (Entry->Rest == "uid_map") Kind = 0;
    else if (Entry->Rest == "gid_map") Kind = 1;
    else if (Entry->Rest == "setgroups") Kind = 2;
    else return std::nullopt;
    const auto& Target = *Entry->Process;
    const auto& Mine = Task->Creds;
    const bool Owner = Mine.Uid[3] == Target.Creds.Uid[1];
    const bool Override = Mine.OwnCapable(Switch2Guest::CapDacOverride) && Task->Ns.User->Uid.FromKernel(Target.Creds.Uid[1]) &&
                          Task->Ns.User->Gid.FromKernel(Target.Creds.Gid[1]);
    if (!Owner && !Override) return std::nullopt;
    const auto Subject = SubjectOf(*Task);
    if (Kind == 2 && !Switch2Guest::Ns::OpenSetgroupsForWrite(*Target.Ns.User, Subject)) return std::nullopt;
    return OpenFile::ProcWriter {Target.PID, Kind, Subject};
  }

  // proc_uid_map_write, proc_gid_map_write and proc_setgroups_write.
  int64_t WriteProcNamespaceFile(OpenFile& File, uint64_t Address, uint64_t Count) {
    namespace Ns = Switch2Guest::Ns;
    if (Count >= 4096) return Invalid;
    if (Count && AccessiblePrefix(Address, Count, PF_R) < Count) return Fault;
    auto* Target = FindProcess(File.ProcWrite->PID);
    if (!Target) return -3;
    const std::string_view Text {reinterpret_cast<const char*>(Address), static_cast<size_t>(Count)};
    auto& UserNs = *Target->Ns.User;
    const auto Result = File.ProcWrite->Kind == 2 ? Ns::WriteSetgroups(UserNs, File.Offset, Text)
                                                  : Ns::WriteIdMap(UserNs, File.ProcWrite->Kind == 1, File.ProcWrite->Opener, SubjectOf(*Task), File.Offset, Text);
    if (Result > 0) File.Offset += Result;
    // Every process of the namespace now has a root id (or lost it): RootUid follows the id map.
    if (Result > 0 && File.ProcWrite->Kind != 2) {
      for (auto& [PID, Process] : Processes) {
        if (Process.Ns.User.get() == &UserNs) SyncCreds(Process);
      }
    }
    return Result;
  }

  // /proc/<pid>/ns/<name>: the namespace object, null for anything else. Opening and reading the link need
  // the right to look at the process (ptrace_may_access with PTRACE_MODE_READ_FSCREDS).
  struct ProcNsLink {
    std::shared_ptr<Switch2Guest::Ns::Namespace> Object;
    std::string Text;
    const GuestProcess* Owner {};
  };

  std::optional<ProcNsLink> ProcNs(std::string_view GuestPath) const {
    const auto Entry = ProcOf(GuestPath);
    if (!Entry || !Entry->Rest.starts_with("ns/")) return std::nullopt;
    const auto Name = Entry->Rest.substr(3);
    if (Name == "time" || Name == "time_for_children") return std::nullopt; // the kernel is built without time namespaces
    auto Object = Switch2Guest::Ns::NsLinkTarget(Entry->Process->Ns, Name);
    if (!Object) return std::nullopt;
    return ProcNsLink {std::move(Object), *Switch2Guest::Ns::NsLinkText(Entry->Process->Ns, Name), Entry->Process};
  }

  static constexpr uint64_t NsfsDevice = 7; // the nsfs pseudo filesystem, as 0:7 on the checking host

  // /proc/<pid> and /proc/<pid>/ns exist for a process the caller sees.
  bool IsProcDirectory(std::string_view GuestPath) const {
    if (GuestPath == "/proc/self") return false; // the symlink itself
    if (ProcSysDirectory(GuestPath)) return true;
    const auto Entry = ProcOf(GuestPath);
    return Entry && (Entry->Rest.empty() || Entry->Rest == "ns" || Entry->Rest == "fd");
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
    if (Path == "/proc/self") {
      const auto Number = ViewOut(Task->PID);
      return Number ? std::to_string(Number) : std::string {};
    }
    if (Path == "/proc/mounts") return "self/mounts";
    if (const auto Entry = ProcOf(Path); Entry && Entry->Rest == "exe") return ProcExeText(*Entry->Process);
    if (const auto Entry = ProcOf(Path); Entry && Entry->Rest.starts_with("fd/")) {
      const auto Digits = Entry->Rest.substr(3);
      uint64_t Number {};
      const auto [Next, Error] = std::from_chars(Digits.data(), Digits.data() + Digits.size(), Number);
      const bool Canonical = !Digits.empty() && (Digits == "0" || Digits.front() != '0');
      return Canonical && Error == std::errc {} && Next == Digits.data() + Digits.size() ? ProcFdText(*Entry->Process, Number) : std::string {};
    }
    if (const auto Entry = ProcOf(Path); Entry && (Entry->Rest == "root" || Entry->Rest == "cwd")) {
      return ProcRootText(*Entry->Process, Entry->Rest == "cwd");
    }
    return {};
  }

  // /proc/<pid>/exe: the file the process runs, as a path below the root of the reader's view of the file system
  // (empty when the process has no executable). A file outside the process's root is named from the top.
  // The descriptor table of a process: the caller's own, or the one of P's first live thread.
  const DescriptorState* DescriptorsOf(const GuestProcess& P) const {
    if (&P == Task) return &Descriptors();
    for (const auto& Thread : Threads) {
      if (Thread.PID == P.PID && !Thread.Exited && Thread.Descriptors) return Thread.Descriptors.get();
    }
    return nullptr;
  }

  // /proc/<pid>/fd/<n>: what the descriptor refers to, as proc_fd_link prints it (a path below the reader's root,
  // "pipe:[ino]", "socket:[ino]", or the anon_inode name). Empty when the descriptor is not open.
  std::string ProcFdText(const GuestProcess& P, uint64_t Number) const {
    const auto* Table = DescriptorsOf(P);
    if (!Table || Number >= Table->Files.size() || !Table->Files[Number]) return {};
    const auto& File = *Table->Files[Number];
    if (File.Pipe || (File.Fifo && File.Path.empty())) return "pipe:[" + std::to_string(File.Inode) + "]";
    if ((File.Socket || File.Inet) && File.Path.empty()) return "socket:[" + std::to_string(File.Inode) + "]";
    std::string_view Path = File.LinkPath.empty() ? std::string_view {File.Path} : std::string_view {File.LinkPath};
    if (Path.empty() || !Path.starts_with('/')) return std::string {Path.empty() ? "anon_inode:[unknown]" : Path};
    // d_path through the mount the file was opened in, relative to the reader's root (tmpfs and bind mounts live
    // in other SD folders than the path that reaches them).
    namespace Mn = Switch2Guest::Mounts;
    if (File.Mount && Mn::PathContains(File.Mount->Root, Path)) {
      if (const auto Visible = Mn::VisiblePath(RootOf(*Task), File.Mount.get(), Path)) return *Visible;
      auto Text = Mn::UnreachablePath(File.Mount.get(), Path);
      return Text.substr(std::string_view {"(unreachable)"}.size());
    }
    const std::string_view Root = P.Fs->Root;
    for (const auto Prefix : {Root, std::string_view {RootPath}}) {
      if (!Prefix.empty() && PathContains(Prefix, Path)) {
        Path.remove_prefix(Prefix.size());
        return Path.empty() ? std::string {"/"} : std::string {Path};
      }
    }
    return std::string {Path};
  }

  std::string ProcExeText(const GuestProcess& P) const {
    std::string_view Path = P.ExecutablePath;
    if (Path.empty()) return {};
    const std::string_view Root = P.Fs->Root;
    for (const auto Prefix : {Root, std::string_view {RootPath}}) {
      if (!Prefix.empty() && PathContains(Prefix, Path)) {
        Path.remove_prefix(Prefix.size());
        return Path.empty() ? std::string {"/"} : std::string {Path};
      }
    }
    return std::string {Path};
  }

  // The mount the last PathStat or FileStat found its file in (null when it belongs to none).
  MountRef StatMount;

  // st_dev of a file system made by mount(2): its own anonymous device (proc keeps the one the file layer reports).
  static void ApplyMountDevice(LinuxStat& Info, const MountRef& M) {
    if (M && M->Sb->Dev >= 40 && M->Sb->Type != "proc") Info.Device = M->Sb->Dev;
  }

  int64_t PathStat(uint64_t Directory, std::string_view Name, LinuxStat& Info, std::string* Resolved = nullptr, bool Follow = true) {
    const auto Result = PathStatUnmounted(Directory, Name, Info, Resolved, Follow);
    StatMount = LastMount;
    if (!Result) ApplyMountDevice(Info, StatMount);
    return Result;
  }

  int64_t PathStatUnmounted(uint64_t Directory, std::string_view Name, LinuxStat& Info, std::string* Resolved, bool Follow) {
    FsFileSystem SdCard {};
    if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
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
      if (const auto Shm = ShmEntryName(Path)) {
        if (const auto Entry = ShmFiles.find(*Shm); Entry != ShmFiles.end()) {
          fsFsClose(&SdCard);
          if (Name.ends_with('/')) return -20;
          Info = ShmFileStat(Entry->second);
          if (Resolved) *Resolved = std::move(Path);
          return 0;
        }
      }
      if (const auto VirtualDir = VirtualDirectory(Path); !VirtualDir.empty()) {
        fsFsClose(&SdCard);
        Info = MakeStat(0040555, 0, VirtualInode(VirtualDir));
        Info.Device = VirtualDir == "/dev" ? DevDevice : ProcDevice;
        if (VirtualDir == ShmDirectory) ShmDirectoryStat(Info);
        Info.Links = 2;
        if (Resolved) *Resolved = std::move(Path);
        return 0;
      }
      if (VirtualMount(Path)) {
        const auto GuestPath = std::string_view(Path).substr(RootPath.size());
        if (const auto Link = ProcNs(GuestPath)) {
          // A namespace link: a symlink to "user:[4026531837]"; followed, the nsfs file of that namespace.
          fsFsClose(&SdCard);
          if (Name.ends_with('/')) return -20;
          if (!Follow) {
            Info = MakeStat(0120777, Link->Text.size(), VirtualInode(GuestPath));
            Info.Device = ProcDevice;
          } else {
            Info = MakeStat(0100444, 0, Link->Object->Ino);
            Info.Device = NsfsDevice;
            Info.Uid = Info.Gid = 0;
            Info.Links = 1;
          }
          if (Resolved) *Resolved = std::move(Path);
          return 0;
        }
        const auto Kind = CharDevAt(GuestPath);
        const bool Device = Kind != CharDev::None;
        const auto Contents = ProcFile(GuestPath);
        if (Device || Contents) {
          fsFsClose(&SdCard);
          if (Name.ends_with('/')) return -20;
          Info = MakeStat(Device ? 0020666 : ProcFileMode(GuestPath), Device ? 0 : Contents->size(), VirtualInode(GuestPath));
          Info.Device = Device ? DevDevice : ProcDevice;
          if (Device) Info.Rdev = CharDevRdev(Kind);
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
        Error = SdFileError(SdRetry(fsFsOpenFile, &SdCard, Backing.c_str(), FsOpenMode_Read, &File));
        if (!Error) {
          Error = SdFileError(SdRetry(fsFileGetSize, &File, &Size));
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

  LinuxStat ShmFileStat(const ShmFile& Node) {
    auto Info = MakeStat(ModeForInode(Node.Inode, 0100666), Node.Memory->Size, Node.Inode);
    Info.Blocks = Node.Memory->AllocatedBytes() / 512;
    Info.Device = ShmDevice;
    return Info;
  }

  // Shared by stat on the directory and on an open descriptor of it.
  void ShmDirectoryStat(LinuxStat& Info) const {
    Info.Mode = 0041777;
    Info.Uid = Info.Gid = 0;
    Info.Device = ShmDevice;
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
    if (!CapWrtInode(CapFsetid, Inode) && !InGuestGroup(InodeOwner(Inode).second)) Mode &= ~02000U;
    Record.Mode = Mode & 07777;
    if (auto ACL = Record.Attributes.find(AccessAcl); ACL != Record.Attributes.end()) {
      if (const auto Layout = ParsePosixAcl(ACL->second)) Layout->ApplyMode(ACL->second, Mode, false);
    }
    MarkInodeChanged(Inode);
  }

  void InitializeInodeMode(uint64_t Inode, const std::string& Parent, uint32_t Mode, bool Directory) {
    auto& Record = Inodes[Inode];
    Record.Mode = Mode & 07777 & ~Task->Fs->CreationMask;
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
    if (const auto Denied = MountDenies(File->Mount, false)) return Denied;
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
    if (const auto Denied = MountDenies(StatMount, false)) return Denied;
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
    if (const auto Denied = MountDenies(StatMount, false)) return Denied;
    const auto User = Task->Creds.Uid[3];
    // chown(2) takes ids of the caller's user namespace; an id it does not map is EINVAL.
    const auto NewUid = KuidIn(UID), NewGid = KgidIn(GID);
    if (!NewUid || !NewGid) return Invalid;
    UID = *NewUid;
    GID = *NewGid;
    if (!CapWrtInode(Switch2Guest::CapChown, Info.Inode) && (Info.Uid != User || (UID != Unchanged && UID != Info.Uid) ||
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
    if (const auto Denied = MountDenies(StatMount, false)) return Denied;
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
    Out.Uid = UidOut(Info.Uid);
    Out.Gid = GidOut(Info.Gid);
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
        Entries.insert(Entries.end(), {{"self", false, true}, {"version", false}, {"mounts", false, true}, {"sys", true}});
        // The processes the caller sees, numbered in its PID namespace.
        const ProcViewScope View {*this, File->Mount};
        for (const auto& [PID, Process] : Processes) {
          if (const auto Number = ViewOut(PID); Number && !Process.Reaped) Entries.push_back({std::to_string(Number), true});
        }
      } else if (VirtualDir.ends_with("/ns")) {
        for (const auto* Name : Switch2Guest::Ns::NsLinkNames) {
          const std::string_view View = Name;
          if (View != "time" && View != "time_for_children") Entries.push_back({Name, false, true});
        }
      } else if (VirtualDir.ends_with("/fd") && VirtualDir.starts_with("/proc/")) {
        const ProcViewScope View {*this, File->Mount};
        if (const auto Entry = ProcOf(VirtualDir)) {
          if (const auto* Table = DescriptorsOf(*Entry->Process)) {
            for (size_t Index = 0; Index < Table->Files.size(); ++Index) {
              if (Table->Files[Index]) Entries.push_back({std::to_string(Index), false, true});
            }
          }
        }
      } else if (ProcSysDirectory(VirtualDir)) {
        Entries = ProcSysEntries(VirtualDir);
        Entries.insert(Entries.begin(), {{".", true}, {"..", true}});
      } else if (VirtualDir.starts_with("/proc/")) {
        Entries.insert(Entries.end(), {{"status", false}, {"mountinfo", false}, {"mounts", false}, {"uid_map", false}, {"gid_map", false},
                                       {"setgroups", false}, {"ns", true}, {"fd", true}, {"root", false, true}, {"cwd", false, true}, {"exe", false, true}});
      } else if (VirtualDir == "/dev") {
        for (const auto* Name : {"null", "zero", "full", "random", "urandom", "tty"}) Entries.push_back({Name, false, false, 2});
        Entries.push_back({"shm", true});
      } else if (VirtualDir == ShmDirectory) {
        for (const auto& [Name, Node] : ShmFiles) Entries.push_back({Name, false});
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
      const auto ShmEntry = ShmEntryName(Path);
      const auto Inode = ShmEntry && ShmFiles.contains(*ShmEntry) ? ShmFiles.find(*ShmEntry)->second.Inode :
                         VirtualMount(Path) ? VirtualInode(std::string_view(Path).substr(RootPath.size())) : SdInode(Path);
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
    if (File->Socket || File->Inet || File->Console != ConsoleStream::None) return -29;
    if (Origin > 4) return Invalid;
    if (const auto Kind = CharDevAt(File->Path); Kind == CharDev::Null || Kind == CharDev::Zero || Kind == CharDev::Full) {
      File->Offset = 0; // null_lseek: the position is always 0
      return 0;
    } else if (Kind == CharDev::Random || Kind == CharDev::Urandom) {
      return File->Offset; // noop_llseek
    }
    if (File->AnonName) return File->Offset; // noop_llseek
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
    if (Task->Fs->RemovedWorkingDirectory) return -2;
    const auto Text = WorkingDirectoryText(*Task);
    const auto Bytes = Text.size() + 1;
    if (Size < Bytes) return -34;
    if (Available(Output, PF_W) < Bytes) return Fault;
    std::memcpy(reinterpret_cast<void*>(Output), Text.c_str(), Bytes);
    return Bytes;
  }

  int64_t ChangeDirectory(uint64_t Address) {
    const auto Name = GuestString(Address);
    if (!Name) return Name.Error;
    LinuxStat Info {};
    std::string Path;
    if (const auto Result = PathStat(CurrentDirectory, *Name, Info, &Path); Result < 0) return Result;
    const auto Mount = LastMount;
    if ((Info.Mode & 0170000) != 0040000) return -20;
    if (!InodeAllows(Info.Inode, Info.Mode, 1)) return -13;
    SetWorkingDirectory(std::move(Path), Mount);
    return 0;
  }

  int64_t ChangeDirectoryDescriptor(uint64_t FD) {
    const auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (!File->Directory) return -20;
    if (!InodeAllows(File->Inode, ModeForInode(File->Inode, 0040755), 1)) return -13;
    SetWorkingDirectory(File->Path, File->Mount);
    Task->Fs->RemovedWorkingDirectory = File->RemovedDirectory;
    return 0;
  }

  void SetWorkingDirectory(std::string Path, MountRef Mount) {
    Task->Fs->FolderMount = Mount ? std::move(Mount) : Task->Fs->RootMount;
    Task->Fs->Folder = std::move(Path);
    Task->Fs->RemovedWorkingDirectory = false;
    Task->Fs->WorkingDirectory = GuestWorkingPath(Task->Fs->Folder);
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
    if (Path == "/dev" || Path == "/proc" || Path == ShmDirectory) return Path;
    if (IsProcDirectory(Path)) return Path;
    return {};
  }

  // The path walk keeps its position as a host path plus the mount it is in (GuestMounts.h). The result of the
  // last walk: LastMount is the mount the final path lies in, LastCrossed says the last component stepped onto a
  // mount point (rmdir, unlink and rename refuse those with EBUSY; the walk has already moved onto the mounted root).
  MountRef WalkMount, ScopeMount, LastMount, LastParentMount;
  bool LastCrossed {};

  int64_t ResolveSdPath(uint64_t FD, std::string_view Name, FsFileSystem& SdCard, std::string& Path, bool Follow = true,
                        uint64_t Resolve = 0) {
    LastMount = LastParentMount = nullptr;
    LastCrossed = false;
    if (Name.empty()) return -2;
    const bool Absolute = Name.starts_with('/');
    if (Absolute && (Resolve & ResolveBeneath)) return -18;
    if (Absolute && !(Resolve & ResolveInRoot)) {
      Path = Task->Fs->Root;
      WalkMount = Task->Fs->RootMount;
    } else if (IsCwd(FD)) {
      if (Task->Fs->RemovedWorkingDirectory) return -2;
      Path = Task->Fs->Folder;
      WalkMount = Task->Fs->FolderMount ? Task->Fs->FolderMount : Task->Fs->RootMount;
    } else {
      const auto* File = GetFile(FD);
      if (!File) return BadFile;
      if (!File->Directory) return -20;
      if (File->RemovedDirectory) return -2;
      Path = File->Path;
      WalkMount = File->Mount ? File->Mount : Task->Fs->RootMount;
    }
    const std::string Scope = Resolve & (ResolveBeneath | ResolveInRoot) ? Path : "";
    ScopeMount = WalkMount;
    LastParentMount = WalkMount;
    const auto Result = WalkSdPath(Name, SdCard, Path, Follow, 0, Resolve, Scope);
    LastMount = WalkMount;
    return Result;
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

  // Linux allows every byte but '/' and NUL in a name; FAT and exFAT reject
  // control characters and " * : < > ? \ |, and Horizon treats ':' as a
  // mount separator.
  static bool SdNameUnsafe(std::string_view Path) {
    const auto Name = Path.substr(Path.rfind('/') + 1);
    return std::any_of(Name.begin(), Name.end(), [](char C) {
      return static_cast<uint8_t>(C) < 0x20 || std::strchr("\"*:<>?\\|", C) != nullptr;
    });
  }

  // Entries the SD card cannot hold natively: paths beyond SdPathLimit,
  // names with characters the SD card rejects, and anything inside an
  // overflow directory. All directories below an overflow directory are
  // overflow directories too, so the parent decides.
  bool NeedsOverflow(std::string_view Path) const {
    return Path.size() > SdPathLimit || SdNameUnsafe(Path) || IsOverflowDirectory(ParentPath(Path));
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
      return SdFileError(SdRetry(fsFsGetEntryType, &SdCard, FileBackingPath(Path).c_str(), &Type));
    }
    if (NeedsOverflow(Path)) return -2;
    return SdFileError(SdRetry(fsFsGetEntryType, &SdCard, Path.c_str(), &Type));
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
    if (const auto Names = ShmLinkCount(Inode)) return Names;
    if (!Metadata->second.ManagedFile && !Metadata->second.Symlink && !Metadata->second.SpecialType) return 1;
    return std::count_if(SdInodes.begin(), SdInodes.end(), [Inode](const auto& Name) { return Name.second == Inode; });
  }

  // A link is stored as a small SD file plus its inode record. When the SD card has a folder at that path
  // instead (replaced outside the launcher, e.g. by copying a folder from a PC), the record is stale: it is
  // dropped and the folder is used. Returns true when it dropped one.
  bool DropStaleSymlink(FsFileSystem& SdCard, const std::string& Path) {
    FsDirEntryType Type {};
    if (EntryType(SdCard, Path, Type) < 0 || Type != FsDirEntryType_Dir) return false;
    const auto Name = SdInodes.find(Path);
    if (Name == SdInodes.end()) return false;
    Inodes.erase(Name->second);
    SdInodes.erase(Name);
    InodeStoreDirty = true;
    WriteLog(("Inode store: " + Path + " is a folder on the SD card; dropped its stale link record.").c_str());
    return true;
  }

  const std::string* SymlinkAt(std::string_view Path) const {
    const auto Name = SdInodes.find(Path);
    if (Name == SdInodes.end()) return nullptr;
    const auto Record = Inodes.find(Name->second);
    if (Record == Inodes.end() || !Record->second.Symlink) return nullptr;
    return &*Record->second.Symlink;
  }

  // Steps the walk onto whatever is mounted at (WalkMount, Path): the topmost mount takes over. NO_XDEV refuses.
  int64_t FollowMounts(std::string& Path, uint64_t Resolve) {
    if (!Switch2Guest::Mounts::PathContains(WalkMount->Root, Path)) return 0;
    bool Crossed = false;
    auto At = Switch2Guest::Mounts::Cross(MntTable(), {WalkMount, Path}, &Crossed);
    if (!Crossed) return 0;
    if (Resolve & ResolveNoXdev) return -18;
    WalkMount = std::move(At.M);
    Path = std::move(At.Path);
    LastCrossed = true;
    return 0;
  }

  int64_t WalkSdPath(std::string_view Name, FsFileSystem& SdCard, std::string& Path, bool Follow, unsigned Depth,
                     uint64_t Resolve, std::string_view Scope) {
    const auto UnderRoot = [&] { return PathContains(RootPath, Path); };
    const auto InMounts = [&] { return Switch2Guest::Mounts::PathContains(WalkMount->Root, Path); };
    const auto AtBoundary = [&] {
      if (!Scope.empty()) return Path == Scope && WalkMount == ScopeMount;
      return Path == Task->Fs->Root && WalkMount == Task->Fs->RootMount;
    };
    while (!Name.empty()) {
      if (!Name.starts_with('/')) {
        const auto Inode = SdInode(Path);
        if (!InodeAllows(Inode, ModeForInode(Inode, 0040755), 1)) return -13;
      }
      const auto Slash = Name.find('/');
      const auto Component = Name.substr(0, Slash);
      // Only a real component moves the walk off a mount point: "//" and "/." after it keep the crossing (rmdir of
      // "/mnt//" is EBUSY as well).
      if (!Component.empty() && Component != ".") LastCrossed = false;
      if (Component == "..") {
        if (!InMounts()) {
          // outside every mount (the launch folder): clamp at the folder
          if (Path.size() > LaunchFolder.size()) Path.resize(std::max(LaunchFolder.size(), Path.rfind('/')));
          else if (Resolve & ResolveBeneath) return -18;
        } else {
          // follow_dotdot: stay at the root, leave a mount at its root, else go to the parent directory
          for (;;) {
            if (AtBoundary()) {
              if (Resolve & ResolveBeneath) return -18;
              break;
            }
            if (Path != WalkMount->Root) {
              Path.resize(std::max<size_t>(1, Path.rfind('/')));
              break;
            }
            if (!WalkMount->Parent) break;
            if (Resolve & ResolveNoXdev) return -18;
            Path = WalkMount->Mountpoint;
            WalkMount = WalkMount->Parent;
          }
          if (const auto Error = FollowMounts(Path, Resolve)) return Error;
          LastCrossed = false;
          LastParentMount = WalkMount;
        }
      } else if (!Component.empty() && Component != ".") {
        // Traverse the intermediate entry before consuming a subsequent
        // component, including "." or "..". A missing or regular entry
        // cannot serve as the parent directory of another pathname.
        if (Component.size() > 255) return -36; // NAME_MAX
        const auto ParentSize = Path.size();
        LastParentMount = WalkMount;
        if (!Path.ends_with('/')) Path += '/';
        Path += Component;
        if (Path.size() >= RootPath.size() + GuestPathMax) return -36;
        if (IsInodeStorePath(Path)) return -2;
        if (const auto Error = FollowMounts(Path, Resolve)) return Error;
        const bool Entered = LastCrossed;
        // The pid in /proc/self and /proc/<pid>/... is the one the proc mount being walked numbers it by (the
        // mount of a PID namespace the caller is not in, such as the initial /proc seen from a new namespace).
        std::optional<ProcViewScope> ProcView;
        ProcView.emplace(*this, WalkMount);
        const auto GuestPathNow = UnderRoot() ? std::string_view(Path).substr(RootPath.size()) : std::string_view {};
        if (!Entered && Depth < 40 && (Follow || Slash != Name.npos) && !GuestPathNow.empty()) {
          // /proc/<pid>/root and /proc/<pid>/cwd are magic links: following one jumps to that directory.
          if (const auto Entry = ProcOf(GuestPathNow); Entry && (Entry->Rest == "root" || Entry->Rest == "cwd")) {
            if (Resolve & 2) return -40; // RESOLVE_NO_MAGICLINKS
            if (Resolve & (ResolveNoSymlinks | ResolveBeneath | ResolveInRoot)) return Resolve & ResolveNoSymlinks ? -40 : -18;
            if (!MayPtrace(*Entry->Process)) return -13;
            const bool Cwd = Entry->Rest == "cwd";
            if (Cwd && Entry->Process->Fs->RemovedWorkingDirectory) return -2;
            if (Resolve & ResolveNoXdev) return -18;
            Path = Cwd ? Entry->Process->Fs->Folder : Entry->Process->Fs->Root;
            WalkMount = Cwd && Entry->Process->Fs->FolderMount ? Entry->Process->Fs->FolderMount : Entry->Process->Fs->RootMount;
            if (Slash == Name.npos) break;
            Name.remove_prefix(Slash + 1);
            continue;
          }
        }
        // The scope stays open for the directory check below too: "/proc/<pid>" after a /proc/self link is
        // looked up in the walked mount's PID namespace, not the caller's.
        const auto SyntheticTarget = ProcLink(Path);
        const auto* Target = SyntheticTarget.empty() ? SymlinkAt(Path) : &SyntheticTarget;
        if (SyntheticTarget.empty() && Target && DropStaleSymlink(SdCard, Path)) Target = nullptr;
        if (Target && (Follow || Slash != Name.npos)) {
          if (Depth == 40 || (Resolve & ResolveNoSymlinks)) return -40;
          auto Remaining = *Target;
          if (Slash != Name.npos) Remaining += Name.substr(Slash);
          if (Remaining.size() >= GuestPathMax) return -36;
          if (Target->starts_with('/')) {
            if (Resolve & ResolveBeneath) return -18;
            // An absolute link restarts at the process root (or at the scope with RESOLVE_IN_ROOT).
            const auto RestartMount = Resolve & ResolveInRoot ? ScopeMount : Task->Fs->RootMount;
            const std::string_view Restart = Resolve & ResolveInRoot ? Scope : std::string_view(Task->Fs->Root);
            if ((Resolve & ResolveNoXdev) && WalkMount != RestartMount) return -18;
            Path = Restart;
            WalkMount = RestartMount;
          } else {
            Path.resize(ParentSize);
            // the link lived in the parent's mount: nothing to restore, the walk entered no mount for a link
          }
          ProcView.reset();
          return WalkSdPath(Remaining, SdCard, Path, Follow, Depth + 1, Resolve, Scope);
        }
        if (Slash != Name.npos && (Depth || Name.find_first_not_of('/', Slash) != Name.npos)) {
          if (IsManagedFile(Path) || SpecialNodeAt(Path)) return -20;
          if (const auto Shm = ShmEntryName(Path); Shm && ShmFiles.contains(*Shm)) return -20;
          const auto GuestPath = std::string_view(Path).substr(UnderRoot() ? RootPath.size() : 0);
          if (UnderRoot() && (ProcFile(GuestPath) || CharDevAt(GuestPath) != CharDev::None)) return -20;
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
    const auto User = Task->Creds.Uid[3];
    if (CapWrtInode(CapFowner, SdInode(Path))) return true;
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
      if (!Process.Fs->RemovedWorkingDirectory && PathContains(Old, Process.Fs->Folder)) {
        Process.Fs->Folder = MovedPath(Process.Fs->Folder);
        Process.Fs->WorkingDirectory = GuestWorkingPath(Process.Fs->Folder);
      }
    }
    TrackRenamedMounts(Old, New);
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
        Error = SdFileError(SdRetry(fsFsDeleteDirectory, &SdCard, Child.c_str()));
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
    const auto Removed = SdFileError(SdRetry(fsFsDeleteDirectory, &SdCard, Path.c_str()));
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
            auto Error = SdFileError(SdRetry(fsFsOpenDirectory, &SdCard, NewPath.c_str(), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &Directory));
            if (Error) return Error;
            s64 Count {};
            Error = SdFileError(SdRetry(fsDirGetEntryCount, &Directory, &Count));
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
      Error = SdFileError(SdRetry(fsFsOpenDirectory, &SdCard, New.c_str(), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &Directory));
      if (Error) return Error;
      s64 Count {};
      Error = SdFileError(SdRetry(fsDirGetEntryCount, &Directory, &Count));
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
    if (!VirtualDestination) Error = SdFileError(SdRetry(fsFsRenameDirectory, &SdCard, New.c_str(), DirectoryBackupPath));
    if (!Error) Error = SdFileError(SdRetry(fsFsRenameDirectory, &SdCard, Old.c_str(), New.c_str()));

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
        if (Process.Fs->Folder == New) Process.Fs->RemovedWorkingDirectory = true;
      }
      TrackRenamedHandles(Old, New);
    }
    return 0;
  }

  int64_t InterruptInodeTransaction(FsFileSystem& SdCard) {
    const auto Error = SdFileError(SdRetry(fsFsCommit, &SdCard));
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
    Error = SdFileError(Type == FsDirEntryType_Dir ? SdRetry(fsFsRenameDirectory, &SdCard, Old.c_str(), New.c_str()) :
                                                   SdRetry(fsFsRenameFile, &SdCard, Old.c_str(), New.c_str()));
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
      char Message[160];
      std::snprintf(Message, sizeof(Message), "Guest native move could not finish recovery (move %lld, recovery %lld, %zu names, %zu inodes).",
                    static_cast<long long>(Error), static_cast<long long>(Recovery), SdInodes.size(), Inodes.size());
      WriteLog(Message);
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
    if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
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
    const auto OldParentMount = LastParentMount;
    const bool OldMounted = LastCrossed;
    if (!Error) Error = ResolveSdPath(NewFD, NewComponentPath, SdCard, NewPath, false);
    const auto NewParentMount = LastParentMount;
    const bool NewMounted = LastCrossed;
    // Both names must lie on one mount (EXDEV), which must be writable (EROFS) before the names are looked at.
    if (!Error && OldParentMount != NewParentMount) Error = -18;
    if (!Error) Error = MountDenies(NewParentMount, false);
    if (!Error && (DotName(OldComponentPath) || DotName(NewComponentPath))) Error = -16;
    if (!Error && (OldMounted || NewMounted)) {
      // a mount point cannot be renamed or replaced (a missing source is ENOENT first, as Linux)
      FsDirEntryType Probe {};
      if (OldMounted || IsVirtualEntry(OldPath) || EntryType(SdCard, OldPath, Probe) == 0) Error = -16;
    }
    if (!Error && (OldPath == RootPath || OldPath == LaunchFolder || NewPath == RootPath || NewPath == LaunchFolder)) Error = -16;
    if (!Error && (VirtualDirectory(OldPath) == ShmDirectory || VirtualDirectory(NewPath) == ShmDirectory)) Error = -16;
    if (!Error && (ShmEntryName(OldPath) || ShmEntryName(NewPath))) {
      fsFsClose(&SdCard);
      return RenameShmFile(ShmEntryName(OldPath), ShmEntryName(NewPath), OldName->ends_with('/'), NewName->ends_with('/'), Flags);
    }
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
    const auto Storage = SdFileError(SdRetry(fsFsCreateDirectory, &SdCard, InodeObjects.data()));
    if (Storage && Storage != -17) return Storage;
    if (const auto Error = BeginPromotion(SdCard, Path, Inode); Error < 0) return Error;
    const auto Object = ObjectPath(Inode);
    auto Error = SdFileError(SdRetry(fsFsRenameFile, &SdCard, Path.c_str(), Object.c_str()));
    if (!Error) {
      auto& Record = Inodes[Inode];
      const auto OldMode = Record.Mode;
      const bool PreviousDirty = InodeStoreDirty;
      if (!Record.Mode) Record.Mode = RegularMode(Path) & 07777;
      Record.ManagedFile = true;
      InodeStoreDirty = true;
      Error = SaveInodeStore();
      if (Error) {
        const auto Rollback = SdFileError(SdRetry(fsFsRenameFile, &SdCard, Object.c_str(), Path.c_str()));
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
    if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
    std::string OldPath, NewPath;
    auto Error = Temporary ? int64_t {0} : ResolveSdPath(OldFD, *OldName, SdCard, OldPath, Flags & Follow);
    const auto OldMount = LastMount;
    if (!Error) Error = ResolveSdPath(NewFD, *NewName, SdCard, NewPath, false);
    const auto NewMount = LastParentMount;
    const bool NewEntered = LastCrossed;
    // A new name on a read-only mount fails first, then a name on another mount (EXDEV), as filename_create does.
    if (!Error && !NewName->ends_with('/')) {
      if (const auto Denied = MountDenies(NewMount, true); Denied && NameIsFree(SdCard, NewPath)) Error = Denied;
    }
    if (!Error && !Temporary && !NewEntered && OldMount != NewMount && NameIsFree(SdCard, NewPath)) Error = -18;
    if (!Error && !Temporary && (ShmEntryName(OldPath) || ShmEntryName(NewPath))) {
      fsFsClose(&SdCard);
      return LinkShmFile(ShmEntryName(OldPath), ShmEntryName(NewPath), NewName->ends_with('/'));
    }
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
    return MakeSpecialNode(FD, *Name, Mode);
  }

  // Creates a FIFO or socket node at Name (mknod, and bind() for AF_UNIX). The resolved path
  // comes back in Created.
  int64_t MakeSpecialNode(uint64_t FD, std::string_view NodeName, uint32_t Mode, std::string* Created = nullptr) {
    const auto Type = Mode & 0170000;
    const auto* Name = &NodeName;
    FsFileSystem SdCard {};
    if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
    std::string Path;
    auto Error = ResolveSdPath(FD, *Name, SdCard, Path, false);
    const auto ParentMount = LastParentMount;
    if (!Error && Name->ends_with('/')) Error = -2;
    if (!Error) {
      if (const auto Denied = MountDenies(ParentMount, true); Denied && NameIsFree(SdCard, Path)) Error = Denied;
    }
    if (!Error && !CanModifyParent(Path)) Error = -13;
    if (!Error && (IsVirtualEntry(Path) || LastCrossed)) Error = -17;
    if (!Error) {
      FsDirEntryType ExistingType {};
      const auto Existing = EntryType(SdCard, Path, ExistingType);
      if (Existing != -2) Error = Existing ? Existing : -17;
    }
    fsFsClose(&SdCard);
    if (Error) return Error;
    InodeMetadata Record {};
    Record.SpecialType = Type;
    if (Created) *Created = Path;
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
    if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
    std::string Path;
    auto Error = ResolveSdPath(FD, *Name, SdCard, Path, false);
    const auto ParentMount = LastParentMount;
    if (!Error && !Name->ends_with('/')) {
      if (const auto Denied = MountDenies(ParentMount, true); Denied && NameIsFree(SdCard, Path)) Error = Denied;
    }
    if (!Error && (Name->ends_with('/') || !CanModifyParent(Path))) Error = Name->ends_with('/') ? -2 : -13;
    if (!Error && (IsVirtualEntry(Path) || LastCrossed)) Error = -17;
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
      // proc_pid_readlink: another process's exe needs ptrace_may_access (read).
      if (!SyntheticTarget.empty() && VirtualMount(Path)) {
        if (const auto Entry = ProcOf(std::string_view(Path).substr(RootPath.size())); Entry && (Entry->Rest == "exe" || Entry->Rest.starts_with("fd/")) && !MayPtrace(*Entry->Process)) return -13;
      }
      Target = SymlinkAt(Path);
      if (SyntheticTarget.empty() && VirtualMount(Path)) {
        if (const auto Link = ProcNs(std::string_view(Path).substr(RootPath.size()))) {
          if (!MayPtrace(*Link->Owner)) return -13;
          SyntheticTarget = Link->Text;
        }
      }
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
    uint32_t Group = Task->Creds.Gid[3];
    const auto ParentInode = SdInode(Parent);
    if (ModeForInode(ParentInode, 0040755) & 02000) {
      Group = InodeOwner(ParentInode).second;
      if (Directory) Mode |= 02000;
    }
    Record.Owner.emplace(Task->Creds.Uid[3], Group);
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

  // Startup, after journal recovery: every operation publishes its intent
  // before it creates or fills the staging entry and clears the intent only
  // after the entry is gone, so a staging entry with no intent is left over
  // from a crash. It blocks every later create, rename and remove. Move it
  // into the orphans folder (kept, not deleted) and log it.
  int64_t SetAsideOrphanBackup(FsFileSystem& SdCard) {
    FsDirEntryType Type {};
    const auto State = SdFileError(SdRetry(fsFsGetEntryType, &SdCard, DirectoryBackupPath, &Type));
    if (State == -2) return 0;
    if (State) return State;
    auto Error = SdFileError(SdRetry(fsFsCreateDirectory, &SdCard, InodeFiles::Orphans));
    if (Error && Error != -17) return Error;
    char Target[FS_MAX_PATH];
    for (unsigned Index = 0;; ++Index) {
      if (Index == 1000) return -17;
      std::snprintf(Target, sizeof(Target), "%s/dir-backup-%llu-%u", InodeFiles::Orphans,
                    static_cast<unsigned long long>(InodeGeneration), Index);
      FsDirEntryType Existing {};
      if (SdFileError(SdRetry(fsFsGetEntryType, &SdCard, Target, &Existing)) == -2) break;
    }
    Error = SdFileError(Type == FsDirEntryType_Dir ? SdRetry(fsFsRenameDirectory, &SdCard, DirectoryBackupPath, Target) :
                                                     SdRetry(fsFsRenameFile, &SdCard, DirectoryBackupPath, Target));
    if (Error) return Error;
    Error = SdFileError(SdRetry(fsFsCommit, &SdCard));
    char Message[FS_MAX_PATH + 96];
    std::snprintf(Message, sizeof(Message), "Guest staging entry without a journal intent moved to %s.", Target);
    WriteLog(Message);
    return Error;
  }

  int64_t CreateNativeEntry(FsFileSystem& SdCard, const std::string& Path, uint32_t Mode, bool Directory) {
    if (!InodeStoreAvailable || InodeGeneration == UINT64_MAX) {
      static bool Logged;
      if (!Logged) WriteLog("Guest native creation refused: the inode store is unavailable.");
      Logged = true;
      return -5;
    }
    FsDirEntryType StagingType {};
    const auto Staging = SdFileError(SdRetry(fsFsGetEntryType, &SdCard, DirectoryBackupPath, &StagingType));
    if (Staging != -2) {
      WriteLog("Guest native creation refused: the staging entry /fex/.inode_store/dir-backup exists.");
      return Staging ? Staging : -5;
    }
    const auto First = InodeSnapshot::FirstInode;
    const auto Intent = EncodeInodeSnapshot(InodeGeneration + 1, First + (Directory ? 9 : 10),
                                            {{DirectoryBackupPath, First}, {Path, First + 1}}, {});
    if (Intent.empty()) return -28;
    auto Error = PublishInodeIntent(SdCard, DirectoryIntentPath, Intent);
    if (Error) return Error;

    Error = SdFileError(Directory ? SdRetry(fsFsCreateDirectory, &SdCard, DirectoryBackupPath) :
                                             SdRetry(fsFsCreateFile, &SdCard, DirectoryBackupPath, 0, 0));
    if (!Error) Error = CheckpointNewEntry(Path, Mode, Directory);
    if (const auto Recovery = RecoverDirectoryRename(SdCard); Recovery) {
      InodeStoreAvailable = false;
      char Message[200];
      std::snprintf(Message, sizeof(Message), "Guest native creation could not finish recovery (create %lld, recovery %lld) for %s.",
                    static_cast<long long>(Error), static_cast<long long>(Recovery), Path.c_str());
      WriteLog(Message);
      return Error ? Error : Recovery;
    }
    return Error;
  }

  // Creates a regular file the SD card cannot name, as a managed object.
  // The inode is first recorded as unlinked, like O_TMPFILE, so a reset
  // before the name is published leaves an object the next launch reclaims.
  int64_t CreateOverflowFile(FsFileSystem& SdCard, const std::string& Path, uint32_t Mode) {
    if (!InodeStoreAvailable || InodeGeneration == UINT64_MAX) return -5;
    const auto Storage = SdFileError(SdRetry(fsFsCreateDirectory, &SdCard, InodeObjects.data()));
    if (Storage && Storage != -17) return Storage;
    const auto Parent = ParentPath(Path);
    const auto ParentInode = SdInode(Parent);
    const auto Inode = NextSdInode++;
    auto& Record = Inodes[Inode];
    Record.ManagedFile = Record.Unlinked = true;
    uint32_t Group = Task->Creds.Gid[3];
    if (ModeForInode(ParentInode, 0040755) & 02000) Group = InodeOwner(ParentInode).second;
    Record.Owner.emplace(Task->Creds.Uid[3], Group);
    InitializeInodeMode(Inode, Parent, Mode, false);
    const auto Now = WallClock();
    Record.Times = InodeTimestamps {Now, Now, Now};
    InodeStoreDirty = true;
    auto Error = SaveInodeStore();
    if (!Error) Error = SdFileError(SdRetry(fsFsCommit, &SdCard));
    if (!Error) Error = SdFileError(SdRetry(fsFsCreateFile, &SdCard, ObjectPath(Inode).c_str(), 0, 0));
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
    if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
    std::string Path;
    const auto Last = Name->find_last_not_of('/');
    const auto Pathname = Last == Name->npos ? *Name : Name->substr(0, Last + 1);
    auto Error = ResolveSdPath(FD, Pathname, SdCard, Path, false);
    const auto ParentMount = LastParentMount;
    const bool Entered = LastCrossed;
    if (!Error) {
      if (Path == RootPath || IsStoreEntry(Path) || Entered) Error = -17;
      else if (const auto Denied = MountDenies(ParentMount, true); Denied && NameIsFree(SdCard, Path)) Error = Denied;
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
    if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
    std::string Path;
    auto Error = ResolveSdPath(FD, *Name, SdCard, Path, false);
    const bool Mounted = LastCrossed;
    if (!Error) Error = MountDenies(LastParentMount, false);
    if (!Error) {
      if (const auto Shm = ShmEntryName(Path)) {
        fsFsClose(&SdCard);
        return UnlinkShmFile(*Shm, Name->ends_with('/'));
      }
    }
    const bool Virtual = !Error && (SymlinkAt(Path) || SpecialNodeAt(Path));
    const bool Managed = !Error && IsManagedFile(Path);
    FsDirEntryType Type {};
    if (!Error && !Virtual) Error = EntryType(SdCard, Path, Type);
    if (!Error && !Virtual && Type == FsDirEntryType_Dir) Error = -21;
    if (!Error && Name->ends_with('/')) Error = -20;
    if (!Error && Mounted) Error = -16; // a mount point (a file bound over)
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
    if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
    std::string Path;
    auto Pathname = *Name;
    if (Last != Name->npos) Pathname = Name->substr(0, Last + 1);
    auto Error = ResolveSdPath(FD, Pathname, SdCard, Path, false);
    const bool Mounted = LastCrossed;
    if (!Error) Error = MountDenies(LastParentMount, false);
    if (!Error && IsVirtualEntry(Path)) Error = -20;
    // /dev/shm holds only regular files: rmdir of one is ENOTDIR.
    if (!Error) {
      if (const auto Shm = ShmEntryName(Path)) Error = ShmFiles.contains(*Shm) ? -20 : -2;
    }
    if (!Error && (Path == RootPath || Path == "/" || Mounted)) Error = -16;
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
      auto Status = SdRetry(fsFsOpenDirectory, &SdCard, Path.c_str(), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &Directory);
      Error = SdFileError(Status);
      if (!Error) {
        s64 Entries {};
        Status = SdRetry(fsDirGetEntryCount, &Directory, &Entries);
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
      if (Process.Fs->Folder == Path) Process.Fs->RemovedWorkingDirectory = true;
    }
  }

  int64_t DuplicateDescriptor(uint64_t Old, uint64_t New, bool CloseOnExec) {
    if (!GetFile(Old) || New >= DescriptorLimit()) return BadFile;
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
    constexpr uint64_t PipeMaximum = 1024 * 1024; // /proc/sys/fs/pipe-max-size
    constexpr uint32_t LinuxPage = 4096;
    if (!File.Pipe) return BadFile;
    if (!Set) return File.Socket->InputCapacity;
    // round_pipe_size: above 2^31 is EINVAL; a rounded size over pipe-max-size
    // needs CAP_SYS_RESOURCE (checked after rounding, so 1048577 is EPERM).
    if (Argument > (1u << 31)) return Invalid;
    const uint64_t Capacity = std::bit_ceil<uint64_t>(std::max(LinuxPage, Argument));
    if (Capacity > PipeMaximum && !Task->Creds.Capable(CapSysResource)) return -1;

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
      // O_NONBLOCK | O_APPEND | O_NOATIME, and O_DIRECT on a pipe (packet mode for later writes)
      const uint32_t MutableFlags = 04000 | 02000 | NoAtimeFlag | (File->Pipe ? DirectFlag : 0);
      if ((Arg & NoAtimeFlag) && !(File->StatusFlags & NoAtimeFlag) && !OwnsInodeOrRoot(File->Inode)) return -1;
      File->StatusFlags = (File->StatusFlags & ~MutableFlags) | (Arg & MutableFlags);
      if (File->Socket && !File->Fifo) File->Socket->Nonblocking = Arg & 04000;
      if (File->Inet) File->Inet->Nonblocking = Arg & 04000; // the host descriptor stays nonblocking
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
      if (Arg >= DescriptorLimit()) return Invalid;
      const auto NewFD = FreeDescriptor(Arg);
      if (NewFD == NoDescriptor) return -24;
      Descriptors().Files[NewFD] = Descriptors().Files[FD];
      Descriptors().Flags[NewFD] = Command == 1030;
      return NewFD;
    }
    return Invalid;
  }

  // RLIMIT_NOFILE: descriptors at or above the soft limit are not allocated.
  uint64_t DescriptorLimit() const {
    return std::min<uint64_t>(Task->Limits[ResourceNofile][0], Descriptors().Files.size());
  }

  uint64_t FreeDescriptor(uint64_t Start = 0) const {
    for (uint64_t FD = Start; FD < DescriptorLimit(); ++FD) {
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
    auto Error = SdFileError(SdRetry(fsFsOpenFile, &SdCard, Path, FsOpenMode_Read, &File));
    if (Error) return Error;
    s64 Size {};
    Error = SdFileError(SdRetry(fsFileGetSize, &File, &Size));
    if (!Error && (Size <= 0 || static_cast<uint64_t>(Size) > InodeSnapshot::MaxBytes)) Error = -5;
    if (!Error) {
      Bytes.resize(Size);
      u64 Read {};
      Error = SdFileError(SdRetry(fsFileRead, &File, 0, Bytes.data(), Bytes.size(), FsReadOption_None, &Read));
      if (!Error && Read != Bytes.size()) Error = -5;
      if (Error) Bytes.clear();
    }
    fsFileClose(&File);
    return Error;
  }

  static int64_t WriteInodeFile(FsFileSystem& SdCard, const char* Path, std::span<const uint8_t> Bytes) {
    FsFile File {};
    auto Error = SdFileError(SdRetry(fsFsOpenFile, &SdCard, Path, FsOpenMode_Write | FsOpenMode_Append, &File));
    if (Error) return Error;
    Error = SdFileError(SdRetry(fsFileSetSize, &File, Bytes.size()));
    if (!Error) Error = SdFileError(SdRetry(fsFileWrite, &File, 0, Bytes.data(), Bytes.size(), FsWriteOption_Flush));
    fsFileClose(&File);
    return Error;
  }

  static int64_t FinishInodeIntent(FsFileSystem& SdCard, const char* Path) {
    if (const auto Error = SdFileError(SdRetry(fsFsCommit, &SdCard)); Error) return Error;
    if (const auto Error = SdFileError(SdRetry(fsFsDeleteFile, &SdCard, Path)); Error) return Error;
    return SdFileError(SdRetry(fsFsCommit, &SdCard));
  }

  int64_t PublishInodeIntent(FsFileSystem& SdCard, const char* Destination, std::span<const uint8_t> Bytes) {
    auto Error = SdFileError(SdRetry(fsFsCreateFile, &SdCard, IntentStagingPath, 0, 0));
    if (Error) return Error;
    Error = WriteInodeFile(SdCard, IntentStagingPath, Bytes);
    if (!Error) Error = SdFileError(SdRetry(fsFsRenameFile, &SdCard, IntentStagingPath, Destination));
    if (Error) {
      SdRetry(fsFsDeleteFile, &SdCard, IntentStagingPath);
      return Error;
    }
    Error = SdFileError(SdRetry(fsFsCommit, &SdCard));
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
    const auto ObjectStatus = SdFileError(SdRetry(fsFsGetEntryType, &SdCard, Object.c_str(), &Type));
    if (ObjectStatus && ObjectStatus != -2) return ObjectStatus;
    if (!ObjectStatus && Type != FsDirEntryType_File) return -5;
    const auto OriginalStatus = SdFileError(SdRetry(fsFsGetEntryType, &SdCard, Path.c_str(), &Type));
    if (OriginalStatus && OriginalStatus != -2) return OriginalStatus;

    const auto Name = SdInodes.find(Path);
    const bool Committed = InodeGeneration >= Intent->Generation && Name != SdInodes.end() &&
                           Name->second == Inode && IsManagedFile(Path);
    if (Committed) {
      if (ObjectStatus || OriginalStatus != -2) return -5;
    } else if (!ObjectStatus && OriginalStatus == -2) {
      if (const auto Move = SdFileError(SdRetry(fsFsRenameFile, &SdCard, Object.c_str(), Path.c_str())); Move < 0) return Move;
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
      const auto Status = SdFileError(SdRetry(fsFsGetEntryType, &SdCard, Path, &Type));
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
      return SdFileError(EntryType == FsDirEntryType_File ? SdRetry(fsFsRenameFile, &SdCard, From, To) :
                                                                              SdRetry(fsFsRenameDirectory, &SdCard, From, To));
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
      RecoveryError = SdFileError(File ? SdRetry(fsFsDeleteFile, &SdCard, DirectoryBackupPath) :
                                         SdRetry(fsFsDeleteDirectory, &SdCard, DirectoryBackupPath));
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
    if (R_FAILED(SdRetry(fsOpenSdCardFileSystem, &SdCard))) return;
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
    if (InodeStoreAvailable && SetAsideOrphanBackup(SdCard) < 0) {
      InodeStoreAvailable = false;
      WriteLog("Guest staging entry without a journal intent could not be set aside.");
    }
    if (InodeStoreAvailable && RecoverPromotion(SdCard) < 0) {
      InodeStoreAvailable = false;
      WriteLog("Guest inode promotion could not be recovered.");
    }
    if (InodeStoreAvailable) {
      const auto Cleanup = SdFileError(SdRetry(fsFsDeleteFile, &SdCard, IntentStagingPath));
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
          const auto Error = SdFileError(SdRetry(fsFsDeleteFile, &SdCard, ObjectPath(*It).c_str()));
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
        if (Path.size() > SdPathLimit || SdNameUnsafe(Path) || !IsOverflowDirectory(Path)) continue;
        const auto Error = SdFileError(SdRetry(fsFsDeleteDirectory, &SdCard, Path.c_str()));
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
        const auto Error = SdFileError(SdRetry(fsFsDeleteFile, &SdCard, ObjectPath(Object).c_str()));
        if (!Error || Error == -2) Removed.push_back(Inode);
      }
    }
    if (Removed.empty()) return 0;
    if (const auto Error = SdFileError(SdRetry(fsFsCommit, &SdCard)); Error) return Error;
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
    if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
    const auto* Path = InodeStorePaths[Generation % 2];
    auto Error = WriteInodeFile(SdCard, Path, Bytes);
    if (Error == -2) {
      Error = SdFileError(SdRetry(fsFsCreateFile, &SdCard, Path, 0, 0));
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
    if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
    const auto Error = SdFileError(SdRetry(fsFsCommit, &SdCard));
    fsFsClose(&SdCard);
    return Error;
  }

  int64_t TruncatePath(uint64_t Address, uint64_t Size) {
    if (Size > INT64_MAX) return Invalid;
    const auto Name = GuestString(Address);
    if (!Name) return Name.Error;
    FsFileSystem SdCard {};
    if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
    std::string Path;
    auto Error = ResolveSdPath(CurrentDirectory, *Name, SdCard, Path);
    const auto FileMount = LastMount;
    if (!Error) {
      if (const auto Shm = ShmEntryName(Path)) {
        fsFsClose(&SdCard);
        return TruncateShmFile(*Shm, Name->ends_with('/'), Size);
      }
    }
    if (!Error && VirtualMount(Path)) {
      const auto GuestPath = std::string_view(Path).substr(RootPath.size());
      if (!VirtualDirectory(Path).empty()) Error = -21;
      else if (ProcFile(GuestPath)) Error = Name->ends_with('/') ? -20 : -13;
      else if (CharDevAt(GuestPath) != CharDev::None) {
        Error = Name->ends_with('/') ? -20 : Invalid;
      }
    }
    if (!Error && SpecialNodeAt(Path)) Error = Name->ends_with('/') ? -20 : Invalid;
    const auto Backing = FileBackingPath(Path);
    FsDirEntryType Type {};
    if (!Error) Error = EntryType(SdCard, Path, Type);
    if (!Error && Type == FsDirEntryType_Dir) Error = -21;
    if (!Error && Name->ends_with('/')) Error = -20;
    if (!Error) Error = MountDenies(FileMount, false);
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
    const ProcViewScope View {*this, File->Mount};
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
        return ReclaimTail ? SdFileError(SdRetry(fsFileSetSize, &Native, *ReclaimTail)) : int64_t {0};
      }
      if (Mode & ZeroRange) {
        s64 Allocated {};
        if (const auto Status = SdFileError(SdRetry(fsFileGetSize, &Native, &Allocated)); Status) return Status;
        auto Begin = std::min<uint64_t>(Offset, Allocated);
        const bool Grow = !(Mode & KeepSize) && End > static_cast<uint64_t>(Visible);
        if (Grow) Begin = std::min<uint64_t>(Begin, Visible);
        if (const auto Clear = ZeroDiskRange(Native, Begin, End); Clear) {
          if (End > static_cast<uint64_t>(Allocated) && R_FAILED(SdRetry(fsFileSetSize, &Native, Allocated))) {
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
        if (const auto Status = SdFileError(SdRetry(fsFileGetSize, &Native, &Allocated)); Status) return Status;
        if (End > static_cast<uint64_t>(Allocated)) {
          if (const auto Grow = ResizeNativeFile(Native, End); Grow) return Grow;
        }
      } else {
        if (End > static_cast<uint64_t>(Visible)) {
          if (const auto Grow = ResizeDiskInode(Native, File->Inode, End); Grow) return Grow;
        } else if (LogicalSize(File->Inode)) {
          s64 Allocated {};
          if (const auto Status = SdFileError(SdRetry(fsFileGetSize, &Native, &Allocated)); Status) return Status;
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

  // statfs of the SD card; a bind mount or remount of a directory of it shows its own mount's flags.
  int64_t StatSdFilesystem(uint64_t Output, const Switch2Guest::Mounts::Mount* Mount = nullptr) {
    if (Available(Output, PF_W) < sizeof(LinuxStatFS)) return Fault;
    LinuxStatFS Info {};
    if (const auto Error = QuerySdFilesystem(Info); Error) return Error;
    if (Mount) Info.Flags |= Switch2Guest::Mounts::StatfsFlags(*Mount);
    return CopyOut(Output, Info);
  }

  int64_t QuerySdFilesystem(LinuxStatFS& Info) {
    FsFileSystem SdCard {};
    if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
    s64 Total {}, Free {};
    auto Error = SdFileError(SdRetry(fsFsGetTotalSpace, &SdCard, "/", &Total));
    if (!Error) Error = SdFileError(SdRetry(fsFsGetFreeSpace, &SdCard, "/", &Free));
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
    // The mount the name was found in (StatMount of the lookup above); 1 for files that belong to none.
    const uint64_t ReportedMount = StatMount ? StatMount->Id : 1;
    if (Available(MountID, PF_W) < MountBytes) Result = Fault;
    else if (Unique) CopyOut(MountID, ReportedMount);
    else CopyOut(MountID, static_cast<int32_t>(ReportedMount));
    if (Available(Handle, PF_W) < sizeof(Header) + Copied) return Fault;
    std::memcpy(reinterpret_cast<void*>(Handle), &Header, sizeof(Header));
    std::memcpy(reinterpret_cast<void*>(Handle + sizeof(Header)), Words.data(), Copied);
    return Result;
  }

  int64_t OpenByHandle(uint64_t MountFD, uint64_t Handle, uint64_t RawFlags) {
    bool SdMount = true;
    if (IsCwd(MountFD)) {
      SdMount = !VirtualMount(Task->Fs->Folder);
    } else {
      const auto* File = GetFile(MountFD);
      if (!File) return BadFile;
      SdMount = DescriptorFilesystem(*File) == FilesystemType::Sd && !VirtualMount(File->Path);
    }
    if (!Task->Creds.Capable(CapDacReadSearch)) return -1;
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
    const ProcViewScope View {*this, File.Mount};
    const auto VirtualDir = VirtualDirectory(File.Path);
    if (!VirtualDir.empty()) return VirtualDir == "/dev" ? FilesystemType::Tmpfs : FilesystemType::Proc;
    if (File.RealFile || File.Directory || File.Fifo) return FilesystemType::Sd;
    if (File.NsObject || ProcFile(File.Path) || !ProcLink(File.Path).empty()) return FilesystemType::Proc;
    if (File.Memory || CharDevAt(File.Path) != CharDev::None) return FilesystemType::Tmpfs;
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
    if (StatMount && StatMount->Sb->Dev >= 40 && StatMount->Sb->Type != "proc") return StatMountedFilesystem(*StatMount, Output);
    if (Info.Device == ProcDevice) return StatFilesystem(FilesystemType::Proc, Output);
    if (Info.Device == DevDevice || Info.Device == ShmDevice) return StatFilesystem(FilesystemType::Tmpfs, Output);
    return StatSdFilesystem(Output, StatMount.get());
  }

  // statfs of a file system made by mount(2): its magic number, its limits and the mount's flags (ST_RDONLY 1,
  // ST_NOSUID 2, ST_NODEV 4, ST_NOEXEC 8, ST_NOATIME 0x400, ST_NODIRATIME 0x800, ST_RELATIME 0x1000).
  int64_t StatMountedFilesystem(const Switch2Guest::Mounts::Mount& M, uint64_t Output) {
    namespace Mn = Switch2Guest::Mounts;
    LinuxStatFS Info {};
    const auto& Type = M.Sb->Type;
    Info.Type = Type == "tmpfs" ? 0x01021994 : Type == "ramfs" ? 0x858458f6 : Type == "sysfs" ? 0x62656572 : Type == "devpts" ? 0x1cd1 : 0x19800202;
    Info.BlockSize = Info.FragmentSize = GuestPageSize;
    Info.NameLength = 255;
    Info.ID[0] = static_cast<int32_t>(M.Sb->Dev);
    const uint64_t Blocks = M.Sb->SizeBytes ? M.Sb->SizeBytes / GuestPageSize : (MemoryTotal() / 2) / GuestPageSize;
    Info.Blocks = Info.Free = Info.Available = M.Sb->Pseudo ? 0 : Blocks;
    Info.Files = Info.FreeFiles = M.Sb->NrInodes ? M.Sb->NrInodes : Blocks;
    Info.Flags = 0x20;
    Info.Flags |= Switch2Guest::Mounts::StatfsFlags(M);
    return CopyOut(Output, Info);
  }

  int64_t StatFilesystemDescriptor(uint64_t FD, uint64_t Output) {
    const auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (File->Mount && File->Mount->Sb->Dev >= 40 && File->Mount->Sb->Type != "proc") return StatMountedFilesystem(*File->Mount, Output);
    if (File->Mount && DescriptorFilesystem(*File) == FilesystemType::Sd) return StatSdFilesystem(Output, File->Mount.get());
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
    if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
    auto Error = SdFileError(SdRetry(fsFsCommit, &SdCard));
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
    if (DescriptorFilesystem(*File) != FilesystemType::Sd) return 0;
    if (const auto Error = FlushSharedFileViews(0); Error < 0) return Error;
    return SyncSdFilesystem();
  }

  int64_t SyncDescriptor(uint64_t FD) {
    auto* File = GetFile(FD);
    if (!File || File->PathOnly()) return BadFile;
    if (File->Memory) return 0;
    if (File->RealFile) {
      if (const auto Error = FlushSharedFileViews(File->Inode); Error < 0) return Error;
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
  static constexpr uint32_t DirectFlag = 040000; // O_DIRECT: packet mode on a pipe

  // ===== Sockets =====
  // AF_UNIX descriptors keep their state in OpenFile::Socket (the same type
  // pipes use). The syscall front ends below (LookupSocket, the address,
  // option and message decoders) are family neutral: they check the
  // descriptor, decode guest structures and then call the Unix back end
  // (BindUnix, ConnectUnix, SendUnix, ReceiveUnix ...). A second family adds
  // its own OpenFile member and one branch per front end at the places
  // marked "family hook".
  using SocketKind = Switch2Guest::SocketKind;
  using SocketCred = Switch2Guest::SocketCred;

  static constexpr uint64_t MsgOob = 1, MsgPeek = 2, MsgTrunc = 0x20, MsgDontWait = 0x40, MsgEor = 0x80, MsgWaitAll = 0x100,
                            MsgErrQueue = 0x2000, MsgWaitForOne = 0x10000, MsgCmsgCloexec = 0x40000000,
                            MsgCmsgCompat = 0x80000000;
  static constexpr uint32_t MsgCtrunc = 8;
  static constexpr int64_t NotSocket = -88, NotConnected = -107;
  static constexpr size_t SocketAddressMax = 110; // sizeof(struct sockaddr_un)
  static constexpr size_t UioMaxIov = 1024;

  static std::shared_ptr<OpenFile> SocketFile(uint64_t Flags, SocketKind Kind = SocketKind::Stream) {
    auto File = std::make_shared<OpenFile>();
    File->Socket = std::make_shared<StreamSocket>();
    File->Socket->Kind = Kind;
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

  // The descriptor of a socket call: EBADF, or ENOTSOCK for anything that is
  // not a socket. AF_INET descriptors (OpenFile::Inet) are accepted too; each front end branches on them.
  OpenFile* LookupSocket(uint64_t FD, int64_t& Error) {
    auto* File = GetFile(FD);
    if (!File) {
      Error = BadFile;
      return nullptr;
    }
    if ((!File->Socket && !File->Inet) || File->Pipe) {
      Error = NotSocket;
      return nullptr;
    }
    return File;
  }

  // SO_PEERCRED and SO_PEERGROUPS use the effective ids; SCM_CREDENTIALS the real ones.
  SocketCred PeerCredentials() const { return {static_cast<int32_t>(Task->PID), Task->Creds.Uid[1], Task->Creds.Gid[1]}; }
  SocketCred MessageCredentials() const { return {static_cast<int32_t>(Task->PID), Task->Creds.Uid[0], Task->Creds.Gid[0]}; }

  void SetPeerCredentials(StreamSocket& Socket) const {
    Socket.PeerCred = PeerCredentials();
    Socket.PeerGroups = Task->SupplementaryGroups;
  }

  // sock_create for AF_UNIX: unix_create checks the protocol (0 or PF_UNIX) and then the type
  // (SOCK_RAW is a datagram socket). AF_INET sockets are made by CreateInetSocket; socketpair() of AF_INET is EOPNOTSUPP.
  static int64_t CheckSocket(uint64_t Domain, uint64_t Type, uint64_t Protocol, SocketKind& Kind, bool Pair = false) {
    if (Type & ~(0xfULL | SocketFlags)) return Invalid;
    // __sock_create: a family outside 0..NPROTO-1 (46) is EAFNOSUPPORT, then
    // a type at or above SOCK_MAX (11) is EINVAL.
    if (static_cast<uint32_t>(Domain) < 46 && (Type & 0xf) >= 11) return Invalid;
    if (static_cast<int32_t>(Domain) != 1) return static_cast<int32_t>(Domain) == 2 && Pair ? -95 : -97;
    // Deliberate: AF_INET6, AF_NETLINK and AF_PACKET are not offered (EAFNOSUPPORT above).
    const auto Number = static_cast<int32_t>(Protocol);
    if (Number != 0 && Number != 1) return -93;
    switch (Type & 0xf) {
    case 1: Kind = SocketKind::Stream; return 0;
    case 2: case 3: Kind = SocketKind::Datagram; return 0;
    case 5: Kind = SocketKind::SeqPacket; return 0;
    default: return -94;
    }
  }

  int64_t CreateSocket(uint64_t Domain, uint64_t Type, uint64_t Protocol) {
    if (static_cast<int32_t>(Domain) == Switch2Guest::Inet::LinuxAfInet) {
      if (Type & ~(0xfULL | SocketFlags)) return Invalid;
      if ((Type & 0xf) >= 11) return Invalid; // SOCK_MAX
      return CreateInetSocket(Type, Protocol);
    }
    SocketKind Kind {};
    if (const auto Result = CheckSocket(Domain, Type, Protocol, Kind); Result) return Result;
    const auto FD = FreeDescriptor();
    if (FD == NoDescriptor) return -24;
    InstallSocket(FD, SocketFile(Type, Kind), Type);
    return FD;
  }

  // pipe2 takes O_CLOEXEC, O_NONBLOCK and O_DIRECT (packet mode). Linux also
  // accepts O_NOTIFICATION_PIPE when built with watch queues; this runtime
  // has none, so that flag is EINVAL like any other unknown bit.
  int64_t CreatePipe(uint64_t Output, uint64_t Flags) {
    if (Flags & ~(SocketFlags | DirectFlag)) return Invalid;
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
    // O_DIRECT applies to the write end only: it flags each write as a packet.
    Writer->StatusFlags = Reader->StatusFlags | 1 | (Flags & DirectFlag);
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
    SocketKind Kind {};
    if (const auto Result = CheckSocket(Domain, Type, Protocol, Kind, true); Result) return Result;
    if (Available(Output, PF_W) < 8) return Fault;
    const auto First = FreeDescriptor();
    if (First == NoDescriptor) return -24;
    const auto Second = FreeDescriptor(First + 1);
    if (Second == NoDescriptor) return -24;
    auto A = SocketFile(Type, Kind);
    auto B = SocketFile(Type, Kind);
    A->Socket->Connect(B->Socket);
    B->Socket->Connect(A->Socket);
    SetPeerCredentials(*A->Socket);
    SetPeerCredentials(*B->Socket);
    InstallSocket(First, std::move(A), Type);
    InstallSocket(Second, std::move(B), Type);
    const uint32_t FDs[] {static_cast<uint32_t>(First), static_cast<uint32_t>(Second)};
    std::memcpy(reinterpret_cast<void*>(Output), FDs, sizeof(FDs));
    return 0;
  }

  // ----- AF_UNIX names -----

  // A sockaddr_un read from the guest. Name holds the bytes after sun_family:
  // a path (cut at its first NUL) or an abstract name that starts with NUL.
  struct UnixAddress {
    std::string Name;
    bool Abstract {};
    bool Autobind {}; // bind() with just the family
    bool Unspec {};   // AF_UNSPEC: a datagram connect() with it disconnects
  };

  // move_addr_to_kernel plus unix_validate_addr. AllowUnspec is for datagram connect().
  int64_t ReadUnixAddress(uint64_t Address, uint64_t Length, UnixAddress& Out, bool AllowAutobind, bool AllowUnspec) {
    const auto Size = static_cast<int32_t>(Length);
    if (Size < 0 || Size > 128) return Invalid;
    if (Size && Available(Address, PF_R) < static_cast<size_t>(Size)) return Fault;
    uint16_t Family {};
    if (Size >= 2) std::memcpy(&Family, reinterpret_cast<const void*>(Address), sizeof(Family));
    if (AllowUnspec && Size >= 2 && Family == 0) {
      Out.Unspec = true;
      return 0;
    }
    if (Size < 2 || Family != 1) return Invalid;
    if (Size == 2) {
      if (!AllowAutobind) return Invalid;
      Out.Autobind = true;
      return 0;
    }
    if (static_cast<size_t>(Size) > SocketAddressMax) return Invalid;
    const auto* Path = reinterpret_cast<const char*>(Address) + 2;
    Out.Abstract = Path[0] == 0;
    Out.Name.assign(Path, Out.Abstract ? static_cast<size_t>(Size) - 2 : strnlen(Path, static_cast<size_t>(Size) - 2));
    return 0;
  }

  // sockaddr_un bytes for a name: sun_family and, for a bound socket, the name
  // (a path adds its NUL, abstract names are exact). Unnamed: just the family.
  static std::vector<uint8_t> SocketAddressBytes(const std::string& Name, bool Named) {
    std::vector<uint8_t> Bytes {1, 0};
    if (Named) {
      Bytes.insert(Bytes.end(), Name.begin(), Name.end());
      if (!Name.empty() && Name[0] != '\0') Bytes.push_back(0);
    }
    return Bytes;
  }

  // The name a socket reports as its peer's: live when the peer is there, else
  // what it was at connect() time.
  static std::vector<uint8_t> PeerAddressBytes(const StreamSocket& Socket) {
    if (const auto Peer = Socket.Peer.lock()) return SocketAddressBytes(Peer->Name, Peer->Bound);
    return SocketAddressBytes(Socket.PeerName, Socket.PeerNamed);
  }

  // Copies an address to the guest: at most *Length bytes, then *Length is the
  // full size (move_addr_to_user).
  int64_t WriteSocketAddress(uint64_t Address, uint64_t Length, std::span<const uint8_t> Bytes) {
    if (Available(Length, PF_R | PF_W) < sizeof(uint32_t)) return Fault;
    uint32_t Space {};
    std::memcpy(&Space, reinterpret_cast<const void*>(Length), sizeof(Space));
    if (static_cast<int32_t>(Space) < 0) return Invalid;
    const auto Copy = std::min<size_t>(Space, Bytes.size());
    if (Copy && Available(Address, PF_W) < Copy) return Fault;
    if (Copy) std::memcpy(reinterpret_cast<void*>(Address), Bytes.data(), Copy);
    return CopyOut(Length, static_cast<uint32_t>(Bytes.size()));
  }

  // Checks the (address, length) pair of accept and recvfrom before they act.
  int64_t CheckSocketAddress(uint64_t Address, uint64_t Length) {
    if (!Address) return 0;
    if (Available(Length, PF_R | PF_W) < 4) return Fault;
    uint32_t Space {};
    std::memcpy(&Space, reinterpret_cast<const void*>(Length), 4);
    if (static_cast<int32_t>(Space) < 0) return Invalid;
    return Available(Address, PF_W) >= std::min(Space, 2u) ? 0 : Fault;
  }

  // ----- Binding tables -----
  // A path name is the S_IFSOCK node it created, keyed by that node's inode, so the table follows
  // the filesystem object (an unlinked node keeps its live socket, a new node at the same path
  // does not find it). Abstract names are keyed by name and type: Linux hashes the type in.
  using BindingKey = std::pair<std::string, SocketKind>;

  std::shared_ptr<StreamSocket> BoundAbstract(const std::string& Name, SocketKind Kind) const {
    const auto Entry = AbstractSockets.find({Name, Kind});
    return Entry == AbstractSockets.end() ? nullptr : Entry->second.lock();
  }

  // unix_autobind: NUL and five lowercase hex digits, the first free one.
  int64_t AutobindUnix(const std::shared_ptr<StreamSocket>& Socket) {
    if (Socket->Bound) return 0;
    for (uint32_t Tries = 0; Tries <= 0xfffff; ++Tries) {
      auto Name = Switch2Guest::AutobindName(AutobindSequence++ & 0xfffff);
      if (BoundAbstract(Name, Socket->Kind)) continue;
      AbstractSockets[{Name, Socket->Kind}] = Socket;
      Socket->Name = std::move(Name);
      Socket->Bound = true;
      return 0;
    }
    return -98;
  }

  // Resolves a name to the bound socket of the given type, with unix_find_other's errors: a path that
  // is missing is ENOENT (or what the walk reports), one that is not a socket node ECONNREFUSED, no write
  // permission EACCES, a node nobody is bound to anymore ECONNREFUSED, the wrong type EPROTOTYPE.
  int64_t FindUnixSocket(const UnixAddress& Address, SocketKind Kind, std::shared_ptr<StreamSocket>& Out) {
    Out.reset();
    if (Address.Abstract) {
      Out = BoundAbstract(Address.Name, Kind);
      return Out ? 0 : -111;
    }
    LinuxStat Info {};
    if (const auto Error = PathStat(CurrentDirectory, Address.Name, Info)) return Error;
    if ((Info.Mode & 0170000) != 0140000) return -111;
    if (!InodeAllows(Info.Inode, Info.Mode, 2)) return -13;
    const auto Entry = PathSockets.find(Info.Inode);
    Out = Entry == PathSockets.end() ? nullptr : Entry->second.lock();
    if (!Out) return -111;
    if (Out->Kind != Kind) {
      Out.reset();
      return -91;
    }
    return 0;
  }

  int64_t BindSocket(uint64_t FD, uint64_t Address, uint64_t Length) {
    int64_t Error {};
    auto* File = LookupSocket(FD, Error);
    if (!File) return Error;
    if (File->Inet) return BindInet(*File->Inet, Address, Length);
    return BindUnix(*File, Address, Length);
  }

  int64_t BindUnix(OpenFile& File, uint64_t Address, uint64_t Length) {
    auto Socket = File.Socket;
    UnixAddress Name;
    if (const auto Result = ReadUnixAddress(Address, Length, Name, true, false); Result) return Result;
    if (Name.Autobind) return Socket->Bound ? Invalid : AutobindUnix(Socket);
    if (Name.Abstract) {
      if (Socket->Bound) return Invalid;
      auto& Binding = AbstractSockets[{Name.Name, Socket->Kind}];
      if (!Binding.expired()) return -98;
      Binding = Socket;
      Socket->Name = Name.Name;
      Socket->Bound = true;
      return 0;
    }
    // unix_bind_bsd looks the path up first (an existing one is EADDRINUSE),
    // then refuses a socket that already has a name, then makes the node.
    LinuxStat Existing {};
    if (const auto Error = PathStat(CurrentDirectory, Name.Name, Existing, nullptr, false); Error != -2) {
      if (!Error) return -98;
      return Error;
    }
    if (Socket->Bound) return Invalid;
    std::string Created;
    if (const auto Error = MakeSpecialNode(CurrentDirectory, Name.Name, 0140777, &Created); Error) return Error == -17 ? -98 : Error;
    PathSockets[SdInode(Created)] = Socket;
    Socket->Name = Name.Name;
    Socket->Bound = true;
    return 0;
  }

  int64_t ListenSocket(uint64_t FD, int32_t Backlog) {
    int64_t Error {};
    auto* File = LookupSocket(FD, Error);
    if (!File) return Error;
    if (File->Inet) return ListenInet(*File->Inet, Backlog);
    auto& Socket = *File->Socket;
    if (Socket.Kind == SocketKind::Datagram) return -95;
    if (!Socket.Bound || Socket.Connected) return Invalid;
    Socket.Listening = true;
    SetPeerCredentials(Socket); // a listener reports the credentials it listened with
    // __sys_listen: a backlog above net.core.somaxconn (4096), including a
    // negative one read as unsigned, is capped to it.
    constexpr uint32_t SoMaxConn = 4096;
    Socket.Backlog = std::min(static_cast<uint32_t>(Backlog), SoMaxConn);
    return 0;
  }

  int64_t ConnectSocket(uint64_t FD, uint64_t Address, uint64_t Length) {
    int64_t Error {};
    auto* File = LookupSocket(FD, Error);
    if (!File) return Error;
    if (File->Inet) return ConnectInet(*File->Inet, Address, Length);
    return ConnectUnix(*File, Address, Length);
  }

  int64_t ConnectUnix(OpenFile& File, uint64_t Address, uint64_t Length) {
    auto& Socket = *File.Socket;
    const bool Datagram = Socket.Kind == SocketKind::Datagram;
    if (!Datagram) {
      if (Socket.Connected) return -106;
      if (Socket.Listening) return Invalid;
    }
    UnixAddress Name;
    if (const auto Result = ReadUnixAddress(Address, Length, Name, false, Datagram); Result) return Result;
    if (Name.Unspec) {
      Socket.Connected = false;
      Socket.Peer.reset();
      ++IOEpoch;
      return 0;
    }
    if (Socket.PassCred && !Socket.Bound) AutobindUnix(File.Socket);
    std::shared_ptr<StreamSocket> Target;
    if (const auto Result = FindUnixSocket(Name, Socket.Kind, Target); Result) return Result;
    if (Datagram) {
      Socket.Connect(Target);
      Socket.PeerName = Target->Name;
      Socket.PeerNamed = Target->Bound;
      ++IOEpoch;
      return 0;
    }
    if (!Target->Listening) return -111;
    // unix_recvq_full: the queue is full once it holds more than the backlog.
    if (Target->Pending.size() > Target->Backlog) return SocketWait(Socket.Nonblocking, Socket.SndTimeoutNs);
    auto Connection = std::make_shared<StreamSocket>();
    Connection->Kind = Socket.Kind;
    Connection->Embryo = true;
    Connection->Name = Target->Name; // the accepted socket carries the listener's name
    Connection->Bound = true;
    Connection->PeerName = Socket.Name;
    Connection->PeerNamed = Socket.Bound;
    Connection->PeerCred = PeerCredentials();
    Connection->PeerGroups = Task->SupplementaryGroups;
    Connection->Connect(File.Socket);
    Socket.Connect(Connection);
    Socket.PeerName = Target->Name;
    Socket.PeerNamed = true;
    Socket.PeerCred = Target->PeerCred;
    Socket.PeerGroups = Target->PeerGroups;
    Target->Pending.push_back(std::move(Connection));
    ++IOEpoch;
    return 0;
  }

  int64_t AcceptSocket(uint64_t FD, uint64_t Address, uint64_t Length, uint64_t Flags) {
    if (Flags & ~SocketFlags) return Invalid;
    int64_t Error {};
    auto* File = LookupSocket(FD, Error);
    if (!File) return Error;
    if (File->Inet) return AcceptInet(*File, Address, Length, Flags);
    auto& Socket = *File->Socket;
    if (Socket.Kind == SocketKind::Datagram) return -95;
    if (!Socket.Listening) return Invalid;
    if (const auto Result = CheckSocketAddress(Address, Length); Result) return Result;
    if (Socket.Pending.empty()) return SocketWait(Socket.Nonblocking, Socket.RcvTimeoutNs);
    const auto NewFD = FreeDescriptor();
    if (NewFD == NoDescriptor) return -24;
    auto Connection = std::make_shared<OpenFile>();
    Connection->Socket = std::move(Socket.Pending.front());
    Socket.Pending.pop_front();
    Connection->Socket->Embryo = false;
    // unix_sock_inherit_flags: the accepted socket takes the listener's
    // SO_PASSCRED and SO_PASSSEC.
    Connection->Socket->PassCred = Socket.PassCred;
    Connection->Socket->PassSec = Socket.PassSec;
    SetSocketFlags(*Connection, Flags);
    InstallSocket(NewFD, std::move(Connection), Flags);
    if (Address) WriteSocketAddress(Address, Length, PeerAddressBytes(*GetFile(NewFD)->Socket));
    ++IOEpoch;
    return NewFD;
  }

  // getsockname (51) and getpeername (52).
  int64_t SocketAddressCall(bool Peer, uint64_t FD, uint64_t Address, uint64_t Length) {
    int64_t Error {};
    auto* File = LookupSocket(FD, Error);
    if (!File) return Error;
    if (File->Inet) return InetAddressCall(Peer, *File->Inet, Address, Length);
    const auto& Socket = *File->Socket;
    if (Peer && (!Socket.Connected || Socket.Listening)) return NotConnected;
    const auto Bytes = Peer ? PeerAddressBytes(Socket) : SocketAddressBytes(Socket.Name, Socket.Bound);
    return WriteSocketAddress(Address, Length, Bytes);
  }

  // ----- Waiting -----

  // A socket call that cannot go on yet: EAGAIN for nonblocking calls and once
  // SO_RCVTIMEO or SO_SNDTIMEO has run out, else sleep until I/O changes. A
  // signal interrupts a timed wait with EINTR whatever SA_RESTART says
  // (sock_intr_errno).
  int64_t SocketWait(bool NoWait, uint64_t TimeoutNs, WaitKind Kind = WaitKind::IO) {
    if (NoWait) return -11;
    if (TimeoutNs) {
      const auto Now = MonotonicNs();
      if (!Current->Deadline) Current->Deadline = AddDeadline(Now, TimeoutNs);
      else if (Now >= Current->Deadline) return -11;
      Current->TimedSocketWait = true;
    }
    return WaitForIO(Kind);
  }

  // ----- Stream reads and writes (also used for pipes) -----

  // sock_error: a pending error (ECONNRESET from a peer that closed with unread data, or one a
  // recvmmsg stored) is reported once, when there is nothing left to read.
  static int32_t TakeSocketError(StreamSocket& Socket) {
    if (Socket.ResetPending) {
      Socket.ResetPending = false;
      return 104;
    }
    return std::exchange(Socket.PendingError, 0);
  }

  int64_t ReadSocket(StreamSocket& Socket, char* Buffer, uint64_t Count, bool DontWait, bool Peek = false) {
    if (!Socket.Pipe) {
      SocketIo Io;
      Io.Vectors.push_back({reinterpret_cast<uint64_t>(Buffer), Count});
      Io.Total = Count;
      SocketResult Result;
      return ReceiveUnix(Socket, Io, (DontWait ? MsgDontWait : 0) | (Peek ? MsgPeek : 0), Result);
    }
    if (!Socket.Connected) return NotConnected;
    if (Socket.ShutRead) return 0;
    if (Socket.Input.empty()) {
      if (Socket.EndOfStream()) return 0;
      return Socket.Nonblocking || DontWait ? -11 : WaitForIO();
    }
    // A packet pipe (O_DIRECT writer) gives at most one packet per read.
    const auto Bytes = Socket.ReadLimit(Count);
    Socket.CopyInput(Buffer, Bytes);
    if (!Peek) {
      Socket.ConsumeRead(Bytes);
      ++IOEpoch;
    }
    return Bytes;
  }

  // Files (SCM_RIGHTS) ride with the first bytes this call queues.
  int64_t WriteSocket(StreamSocket& Socket, const char* Buffer, uint64_t Count, bool DontWait, bool Atomic = false, bool Merge = true,
                      bool Packet = false, std::vector<std::shared_ptr<OpenFile>> Files = {}) {
    if (!Socket.Pipe && Socket.Kind != SocketKind::Stream) {
      // write, writev and the like on a datagram or seqpacket socket send one record.
      SocketIo Io;
      Io.Vectors.push_back({reinterpret_cast<uint64_t>(Buffer), Count});
      Io.Total = Count;
      SendControl Control;
      return SendRecordUnix(Socket, Io, DontWait ? MsgDontWait : 0, Control, Socket.Nonblocking || DontWait, false);
    }
    if (!Socket.Connected) return NotConnected;
    // Packet (O_DIRECT) buffers never merge; Send forces the same.
    if (Packet) Merge = false;
    const bool Blocking = !Socket.Nonblocking && !DontWait;
    // A blocking write returns only when every byte is queued, when the
    // reader goes away after some bytes (their count), or on a signal
    // (InterruptWait reports the count).
    const uint64_t Done = Blocking ? std::exchange(Current->StreamProgress, 0) : 0;
    auto Peer = Socket.Peer.lock();
    if (!Peer || Socket.ShutWrite || Peer->ShutRead) return Done ? static_cast<int64_t>(Done) : -32;
    if (Count == Done) return Done;
    const auto Remaining = Count - Done;
    const auto Bytes = std::min<uint64_t>(Remaining, Socket.WriteSpace(Merge, Remaining));
    if (!Bytes || (Atomic && !Done && Count <= StreamSocket::PipePage && Bytes < Count)) {
      if (!Blocking) return -11;
      // SO_SNDTIMEO ends the wait with what was queued so far (or EAGAIN).
      const auto Wait = SocketWait(false, Socket.SndTimeoutNs);
      if (Wait == -11) return Done ? static_cast<int64_t>(Done) : -11;
      Current->StreamProgress = Done;
      return Wait;
    }
    std::optional<SocketCred> Cred;
    if (!Socket.Pipe) Cred = MessageCredentials();
    const auto Written = Socket.Send({reinterpret_cast<const uint8_t*>(Buffer) + Done, Remaining}, Done ? std::vector<std::shared_ptr<OpenFile>> {} : std::move(Files),
                                     Merge, Packet, Cred);
    ++IOEpoch;
    if (Blocking && Done + Written < Count) {
      Current->StreamProgress = Done + Written;
      return WaitForIO();
    }
    return Done + Written;
  }

  // ----- Message decoding shared by every socket family -----

  struct SocketVector {
    uint64_t Base, Length;
  };

  struct SocketIo {
    std::vector<SocketVector> Vectors;
    size_t Total {};
    uint64_t Name {}, NameLength {};       // sendmsg: the destination; recvmsg: where the sender goes
    uint64_t Control {}, ControlLength {}; // ancillary data buffer
  };

  struct SocketResult {
    bool HasSource {};
    std::string Source;
    uint32_t Flags {};        // msg_flags: MSG_TRUNC, MSG_CTRUNC
    uint64_t ControlUsed {};  // bytes written to the control buffer
    bool HasInet {};          // an AF_INET sender address (Linux sockaddr_in) is in Inet
    std::array<uint8_t, 16> Inet {};
  };

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

  // iovec array of a message: UIO_MAXIOV is EMSGSIZE, a bad array or buffer EFAULT. The total is
  // clipped to MAX_RW_COUNT as Linux does.
  int64_t ReadVectors(const LinuxMessage& Message, bool Writing, SocketIo& Io) {
    if (Message.VectorCount > UioMaxIov) return -90;
    if (Message.VectorCount && Available(Message.Vectors, PF_R) < Message.VectorCount * sizeof(SocketVector)) return Fault;
    Io.Vectors.resize(Message.VectorCount);
    if (!Io.Vectors.empty()) std::memcpy(Io.Vectors.data(), reinterpret_cast<const void*>(Message.Vectors), Io.Vectors.size() * sizeof(SocketVector));
    Io.Total = 0;
    for (auto& Vector : Io.Vectors) {
      Vector.Length = std::min<uint64_t>(Vector.Length, MaxRWCount - Io.Total);
      if (Vector.Length && Available(Vector.Base, Writing ? PF_W : PF_R) < Vector.Length) return Fault;
      Io.Total += Vector.Length;
    }
    return 0;
  }

  // The bytes of a message to send: the guest buffer itself when it is one vector, else a copy.
  struct GatheredBytes {
    const char* Data {};
    std::unique_ptr<char[]> Copy;
  };

  int64_t Gather(const SocketIo& Io, GatheredBytes& Out) {
    if (Io.Vectors.size() == 1 || !Io.Total) {
      Out.Data = Io.Vectors.empty() ? "" : reinterpret_cast<const char*>(Io.Vectors[0].Base);
      return 0;
    }
    Out.Copy.reset(new (std::nothrow) char[Io.Total]);
    if (!Out.Copy) return -12;
    size_t Offset {};
    for (const auto& Vector : Io.Vectors) {
      if (!Vector.Length) continue;
      std::memcpy(Out.Copy.get() + Offset, reinterpret_cast<const void*>(Vector.Base), Vector.Length);
      Offset += Vector.Length;
    }
    Out.Data = Out.Copy.get();
    return 0;
  }

  // Copies Count bytes of Data into the vectors, after Skip bytes already filled.
  static void Scatter(const std::vector<SocketVector>& Vectors, size_t Skip, const uint8_t* Data, size_t Count) {
    for (const auto& Vector : Vectors) {
      if (!Count) break;
      if (Skip >= Vector.Length) {
        Skip -= Vector.Length;
        continue;
      }
      const auto Step = std::min<size_t>(Count, Vector.Length - Skip);
      std::memcpy(reinterpret_cast<uint8_t*>(Vector.Base) + Skip, Data, Step);
      Data += Step;
      Count -= Step;
      Skip = 0;
    }
  }

  // ----- Ancillary data -----

  struct SendControl {
    std::vector<std::shared_ptr<OpenFile>> Files;
    std::optional<SocketCred> Cred;
  };

  static bool InGuestIds(uint32_t Id, const std::array<uint32_t, 4>& Ids) { return Id == Ids[0] || Id == Ids[1] || Id == Ids[2]; }

  // __scm_send: SCM_RIGHTS and SCM_CREDENTIALS at SOL_SOCKET; other levels are skipped, other
  // types are EINVAL. A process may only claim its own credentials without the capability.
  int64_t ReadControl(const SocketIo& Io, SendControl& Out) {
    if (Io.ControlLength > 20480) return -105; // net.core.optmem_max
    if (Io.ControlLength > Available(Io.Control, PF_R)) return Fault;
    constexpr size_t Alignment = 8;
    for (size_t Offset = 0; Io.ControlLength - Offset >= sizeof(LinuxControl);) {
      const auto* Data = reinterpret_cast<const char*>(Io.Control + Offset);
      LinuxControl Header {};
      std::memcpy(&Header, Data, sizeof(Header));
      if (Header.Length < sizeof(Header) || Header.Length > Io.ControlLength - Offset) return Invalid;
      if (Header.Level == 1) {
        if (Header.Type == 1) {
          const auto Count = (Header.Length - sizeof(Header)) / sizeof(int32_t);
          if (Count > 253 - Out.Files.size()) return Invalid;
          for (size_t Index = 0; Index < Count; ++Index) {
            int32_t FD {};
            std::memcpy(&FD, Data + sizeof(Header) + Index * sizeof(FD), sizeof(FD));
            if (!GetFile(FD)) return BadFile;
            Out.Files.push_back(Descriptors().Files[FD]);
          }
        } else if (Header.Type == 2) {
          if (Header.Length != sizeof(Header) + sizeof(int32_t) * 3) return Invalid;
          int32_t Parts[3];
          std::memcpy(Parts, Data + sizeof(Header), sizeof(Parts));
          // scm_check_creds: the claimed pid, uid and gid are of the caller's namespaces.
          const auto& Creds = Task->Creds;
          const auto ClaimedUid = KuidIn(static_cast<uint32_t>(Parts[1])), ClaimedGid = KgidIn(static_cast<uint32_t>(Parts[2]));
          if (Parts[0] != static_cast<int32_t>(PidOut(Task->PID)) && !NsCapableOver(*Task->Ns.Pid->Owner, CapSysAdmin)) return -1;
          if (!ClaimedUid || !ClaimedGid) return -1;
          if (!InGuestIds(*ClaimedUid, Creds.Uid) && !Creds.OwnCapable(Switch2Guest::CapSetuid)) return -1;
          if (!InGuestIds(*ClaimedGid, Creds.Gid) && !Creds.OwnCapable(Switch2Guest::CapSetgid)) return -1;
          const auto Claimed = Parts[0] > 0 ? PidIn(static_cast<uint32_t>(Parts[0])) : 0u;
          Out.Cred = SocketCred {static_cast<int32_t>(Claimed == NoPid ? 0 : Claimed), *ClaimedUid, *ClaimedGid};
        } else {
          return Invalid;
        }
      }
      const auto Next = Offset + ((Header.Length + Alignment - 1) & ~(Alignment - 1));
      if (Io.ControlLength - std::min<uint64_t>(Next, Io.ControlLength) < sizeof(LinuxControl)) break;
      Offset = Next;
    }
    return 0;
  }

  // put_cmsg: appends one control message, truncating it (and setting MSG_CTRUNC) when it does not fit.
  struct ControlWriter {
    uint64_t Address, Space, Used {};
    uint32_t* Flags;
  };

  void PutControl(ControlWriter& Writer, int32_t Level, int32_t Type, const void* Data, size_t Size) {
    const auto Left = Writer.Space - Writer.Used;
    if (Left < sizeof(LinuxControl)) {
      *Writer.Flags |= MsgCtrunc;
      return;
    }
    auto Length = sizeof(LinuxControl) + Size;
    if (Length > Left) {
      *Writer.Flags |= MsgCtrunc;
      Length = Left;
    }
    const LinuxControl Header {Length, Level, Type};
    std::memcpy(reinterpret_cast<void*>(Writer.Address + Writer.Used), &Header, sizeof(Header));
    std::memcpy(reinterpret_cast<void*>(Writer.Address + Writer.Used + sizeof(Header)), Data, Length - sizeof(Header));
    Writer.Used += std::min<uint64_t>((Length + 7) & ~uint64_t {7}, Left);
  }

  // scm_detach_fds: installs the descriptors that fit and puts them in one SCM_RIGHTS message; the
  // rest are dropped with MSG_CTRUNC.
  void PutRights(ControlWriter& Writer, const std::vector<std::shared_ptr<OpenFile>>& Files, bool CloseOnExec) {
    if (Files.empty()) return;
    const auto Left = Writer.Space - Writer.Used;
    const auto Fit = Left > sizeof(LinuxControl) ? (Left - sizeof(LinuxControl)) / sizeof(int32_t) : 0;
    std::vector<int32_t> Installed;
    for (const auto& File : Files) {
      const auto FD = FreeDescriptor();
      if (Installed.size() == Fit || FD == NoDescriptor) {
        *Writer.Flags |= MsgCtrunc;
        break;
      }
      Descriptors().Files[FD] = File;
      Descriptors().Flags[FD] = CloseOnExec;
      Installed.push_back(static_cast<int32_t>(FD));
    }
    if (!Installed.empty()) PutControl(Writer, 1, 1, Installed.data(), Installed.size() * sizeof(int32_t));
  }

  // Registers a socket that is about to hold a descriptor of another socket: queued descriptors can
  // refer back to their own queue, which the destructor of the runtime breaks.
  void NoteRightsReceiver(const std::vector<std::shared_ptr<OpenFile>>& Files, const std::shared_ptr<StreamSocket>& Receiver) {
    if (!std::any_of(Files.begin(), Files.end(), [](const auto& File) { return File->Socket != nullptr; })) return;
    std::erase_if(SocketRightsReceivers, [](const auto& Entry) { return Entry.expired(); });
    if (std::none_of(SocketRightsReceivers.begin(), SocketRightsReceivers.end(), [&](const auto& Entry) { return Entry.lock() == Receiver; })) {
      SocketRightsReceivers.push_back(Receiver);
    }
  }

  // ----- Sending -----

  // The front end of send, sendto and sendmsg for AF_UNIX. Family hook: an inet socket sends here.
  int64_t SendUnix(StreamSocket& Socket, SocketIo& Io, uint64_t Flags) {
    if (Flags & MsgOob) return -95; // Deliberate: no out-of-band data on AF_UNIX stream sockets
    SendControl Control;
    if (Io.ControlLength) {
      if (const auto Result = ReadControl(Io, Control); Result < 0) return Result;
    }
    const bool DontWait = Socket.Nonblocking || (Flags & MsgDontWait);
    const bool HasName = Io.Name && static_cast<int32_t>(Io.NameLength) != 0;
    if (Socket.Kind == SocketKind::Stream) {
      // unix_stream_sendmsg: an address is never wanted.
      if (HasName) return Socket.Connected ? -106 : -95;
      if (!Socket.Connected) return NotConnected;
      GatheredBytes Bytes;
      if (const auto Result = Gather(Io, Bytes); Result) return Result;
      if (!Control.Files.empty()) NoteRightsReceiver(Control.Files, Socket.Peer.lock());
      return WriteSocket(Socket, Bytes.Data, Io.Total, Flags & MsgDontWait, false, true, false, std::move(Control.Files));
    }
    return SendRecordUnix(Socket, Io, Flags, Control, DontWait, HasName);
  }

  // unix_dgram_sendmsg (also used by seqpacket, which ignores any address). Errors follow its
  // order: no peer, message size, sender memory, then the lookup of the destination.
  int64_t SendRecordUnix(StreamSocket& Socket, SocketIo& Io, uint64_t Flags, SendControl& Control, bool DontWait, bool HasName) {
    const bool SeqPacket = Socket.Kind == SocketKind::SeqPacket;
    UnixAddress Name;
    if (SeqPacket) {
      if (const auto Error = TakeSocketError(Socket)) return -Error;
      if (!Socket.Connected) return NotConnected;
      HasName = false;
    }
    if (HasName) {
      if (const auto Result = ReadUnixAddress(Io.Name, Io.NameLength, Name, false, false); Result) return Result;
    } else if (!Socket.Connected) {
      return NotConnected;
    }
    if (Socket.ShutWrite) return -32;
    // A connected datagram socket whose peer is gone: ECONNREFUSED once, then it is unconnected.
    if (!HasName && !Socket.Peer.lock()) {
      if (SeqPacket) return -32;
      Socket.Connected = false;
      ++IOEpoch;
      return -111;
    }
    if (Socket.PassCred && !Socket.Bound) AutobindUnix(Socket.shared_from_this());
    if (Io.Total > Socket.MaxRecord()) return -90;
    if (!Socket.SenderHasRoom()) return SocketWait(DontWait, Socket.SndTimeoutNs);
    std::shared_ptr<StreamSocket> Target;
    if (HasName) {
      if (const auto Result = FindUnixSocket(Name, Socket.Kind, Target); Result) return Result;
      // unix_may_send: a datagram socket connected to another peer accepts data from that peer only.
      if (Socket.Kind == SocketKind::Datagram && Target->Connected) {
        if (const auto Other = Target->Peer.lock(); Other && Other.get() != &Socket) return -1;
      }
    }
    GatheredBytes Bytes;
    if (const auto Result = Gather(Io, Bytes); Result) return Result;
    const auto Cred = Control.Cred ? *Control.Cred : MessageCredentials();
    const auto Source = Socket.Bound ? Socket.Name : std::string {};
    const auto Receiver = Target ? Target : Socket.Peer.lock();
    auto Files = Control.Files;
    switch (Socket.TrySend({reinterpret_cast<const uint8_t*>(Bytes.Data), Io.Total}, Source, Cred, std::move(Files), Flags & MsgEor, Target)) {
    case StreamSocket::SendStatus::Ok:
      NoteRightsReceiver(Control.Files, Receiver);
      ++IOEpoch;
      return Io.Total;
    case StreamSocket::SendStatus::Refused:
      if (!HasName) {
        Socket.Connected = false;
        ++IOEpoch;
      }
      return SeqPacket ? -32 : -111;
    case StreamSocket::SendStatus::Shutdown: return -32;
    case StreamSocket::SendStatus::Full: return SocketWait(DontWait, Socket.SndTimeoutNs);
    }
    return Invalid;
  }

  // ----- Receiving -----

  // Writes the ancillary data of one received message: credentials when SO_PASSCRED is on, then rights.
  void FinishControl(StreamSocket& Socket, const SocketIo& Io, SocketResult& Out, const SocketCred& Cred,
                     const std::vector<std::shared_ptr<OpenFile>>& Files, uint64_t Flags, bool HadRights) {
    if (!Io.ControlLength) {
      // A receive without a control buffer drops what it would have carried.
      if (Socket.PassCred || HadRights) Out.Flags |= MsgCtrunc;
      return;
    }
    ControlWriter Writer {Io.Control, Io.ControlLength, 0, &Out.Flags};
    if (Socket.PassCred) {
      // The sender's credentials in the receiver's namespaces (0 and 65534 when it is not visible or mapped).
      const int32_t Parts[3] {static_cast<int32_t>(PidOut(static_cast<uint32_t>(Cred.Pid))), static_cast<int32_t>(Cred.Uid == Switch2Guest::NoId ? Cred.Uid : UidOut(Cred.Uid)),
                              static_cast<int32_t>(Cred.Gid == Switch2Guest::NoId ? Cred.Gid : GidOut(Cred.Gid))};
      PutControl(Writer, 1, 2, Parts, sizeof(Parts));
    }
    PutRights(Writer, Files, Flags & MsgCmsgCloexec);
    Out.ControlUsed = Writer.Used;
  }

  // The receive half of recv, recvfrom, recvmsg and read for AF_UNIX. Returns the byte count, 0 at
  // EOF, or a negative errno; RetrySyscall after it queued a wait. Family hook: inet sockets receive elsewhere.
  int64_t ReceiveUnix(StreamSocket& Socket, SocketIo& Io, uint64_t Flags, SocketResult& Out) {
    if (Flags & MsgErrQueue) return -11;
    if (Flags & MsgOob) return Socket.Kind == SocketKind::Stream ? Invalid : -95;
    const bool Peek = Flags & MsgPeek;
    const bool DontWait = Socket.Nonblocking || (Flags & MsgDontWait);
    if (Socket.Kind != SocketKind::Stream) return ReceiveRecordUnix(Socket, Io, Flags, DontWait, Peek, Out);

    // unix_stream_read_generic.
    if (!Socket.Connected || Socket.Listening) return Invalid;
    const bool Blocking = !DontWait;
    const uint64_t Done = Blocking ? std::exchange(Current->StreamProgress, 0) : 0;
    const bool WaitAll = Flags & MsgWaitAll;
    const uint64_t Want = Io.Total - Done;
    if (!Want) return Done;
    // The bytes the call waits for: SO_RCVLOWAT, or all of them with MSG_WAITALL.
    const uint64_t Target = std::min<uint64_t>(WaitAll ? Io.Total : static_cast<uint64_t>(Socket.RcvLowat), Io.Total);
    const uint64_t Skip = Peek && Socket.PeekOff >= 0 ? static_cast<uint64_t>(Socket.PeekOff) : 0;
    const uint64_t Available = Socket.Input.size() > Skip ? Socket.Input.size() - Skip : 0;
    if (!Available) {
      if (const auto Error = TakeSocketError(Socket)) return Done ? static_cast<int64_t>(Done) : -Error;
      if (Socket.ShutRead || Socket.EndOfStream()) return Done;
      if (Done >= Target && Done) return Done;
      const auto Wait = SocketWait(DontWait, Socket.RcvTimeoutNs);
      if (Wait == -11) return Done ? static_cast<int64_t>(Done) : -11;
      Current->StreamProgress = Done;
      return Wait;
    }
    // One ancillary message per receive: a run of bytes sent with other credentials, and the bytes
    // after a message that carried rights, wait for the next call.
    const uint64_t Count = Skip ? std::min(Want, Available) : Socket.MessageBytes(Want);
    if (Flags & MsgTrunc) {
      // Stream MSG_TRUNC discards bytes instead of copying them.
    } else if (Count) {
      std::vector<uint8_t> Chunk(Count);
      std::copy_n(Socket.Input.begin() + Skip, Count, Chunk.begin());
      Scatter(Io.Vectors, Done, Chunk.data(), Count);
    }
    const bool HadRights = !Skip && Socket.MessageRights(Count) != nullptr;
    const auto Cred = Socket.FrontCred();
    std::vector<std::shared_ptr<OpenFile>> Files;
    if (HadRights) Files = Socket.Rights.front().Files;
    if (!Peek) {
      Socket.Consume(Count);
      if (Socket.PeekOff > 0) Socket.PeekOff = static_cast<int32_t>(std::max<int64_t>(0, Socket.PeekOff - static_cast<int64_t>(Count)));
      ++IOEpoch;
    } else if (Socket.PeekOff >= 0) {
      Socket.PeekOff += static_cast<int32_t>(Count);
    }
    Out.Flags = 0;
    FinishControl(Socket, Io, Out, Cred, Files, Flags, HadRights);
    if (Socket.PassCred || HadRights) return Done + Count; // a message with ancillary data ends the call
    if (Peek || Done + Count >= Target || DontWait) return Done + Count;
    // The stream ended or failed with the queue now empty: nothing more will arrive, the copied bytes are the result.
    if (Socket.Input.empty() && (Socket.EndOfStream() || Socket.ResetPending || Socket.PendingError)) return Done + Count;
    // Below the target: keep what was copied, wait for the rest.
    const auto Wait = SocketWait(false, Socket.RcvTimeoutNs);
    if (Wait == -11) return Done + Count;
    Current->StreamProgress = Done + Count;
    return Wait;
  }

  // unix_dgram_recvmsg (seqpacket goes through it too).
  int64_t ReceiveRecordUnix(StreamSocket& Socket, SocketIo& Io, uint64_t Flags, bool DontWait, bool Peek, SocketResult& Out) {
    if (Socket.Kind == SocketKind::SeqPacket && (!Socket.Connected || Socket.Listening)) return NotConnected;
    if (Socket.Records.empty()) {
      if (const auto Error = TakeSocketError(Socket)) return -Error;
      if (Socket.ShutRead || (Socket.Kind == SocketKind::SeqPacket && Socket.EndOfStream())) return 0;
      return SocketWait(DontWait, Socket.RcvTimeoutNs);
    }
    size_t Largest {};
    for (const auto& Queued : Socket.Records) Largest = std::max(Largest, Queued.Bytes);
    std::vector<uint8_t> Chunk(std::min<size_t>(Io.Total, Largest));
    size_t Full {};
    std::string Source;
    SocketCred Cred;
    std::vector<std::shared_ptr<OpenFile>> Files;
    const bool Skipping = Peek && Socket.PeekOff >= 0;
    const auto Copied = Socket.ReceiveRecord(Chunk, Peek, Full, &Source, &Cred, &Files, Skipping ? static_cast<size_t>(Socket.PeekOff) : 0);
    Scatter(Io.Vectors, 0, Chunk.data(), Copied);
    if (Peek) {
      if (Socket.PeekOff >= 0) Socket.PeekOff += static_cast<int32_t>(Copied);
    } else {
      if (Socket.PeekOff > 0) Socket.PeekOff = static_cast<int32_t>(std::max<int64_t>(0, Socket.PeekOff - static_cast<int64_t>(Full)));
      ++IOEpoch;
    }
    Out.Flags = 0;
    Out.HasSource = !Source.empty();
    Out.Source = std::move(Source);
    if (Full > Copied) Out.Flags |= static_cast<uint32_t>(MsgTrunc);
    FinishControl(Socket, Io, Out, Cred, Files, Flags, !Files.empty());
    return (Flags & MsgTrunc) ? static_cast<int64_t>(Full) : static_cast<int64_t>(Copied);
  }

  // ===== AF_INET over host BSD sockets (GuestInet.h translates, HostNet.cpp calls) =====
  // Host sockets are nonblocking for good and one host thread runs every
  // guest thread, so a guest call that would block records what it waits for
  // (InetSocket::WaitEvents) and sleeps as WaitKind::Net. PollNet polls the
  // host between scheduler passes and in place of the idle sleep, and ends the
  // wait (++IOEpoch) when a socket becomes ready for what a call waits for.

  // The service starts with the first AF_INET socket and stays up until the launcher exits.
  static bool EnsureNetwork() {
    static bool Up {}, Failed {};
    if (Up) return true;
    if (Failed) return false;
    if (HostNet::Acquire()) return Up = true;
    Failed = true;
    WriteLog("Guest network: the socket service did not start; AF_INET sockets are unavailable.");
    return false;
  }

  static std::array<uint8_t, 16> BsdAddress(uint32_t Address, uint16_t Port) {
    std::array<uint8_t, 16> Bsd {};
    Bsd[0] = 16;
    Bsd[1] = Switch2Guest::Inet::BsdAfInet;
    Bsd[2] = static_cast<uint8_t>(Port >> 8);
    Bsd[3] = static_cast<uint8_t>(Port);
    std::memcpy(&Bsd[4], &Address, sizeof(Address));
    return Bsd;
  }

  static void ParseBsdAddress(const std::array<uint8_t, 16>& Bsd, uint32_t& Address, uint16_t& Port) {
    Port = static_cast<uint16_t>(Bsd[2] << 8 | Bsd[3]);
    std::memcpy(&Address, &Bsd[4], sizeof(Address));
  }

  static std::vector<uint8_t> LinuxInetAddress(uint32_t Address, uint16_t Port) {
    std::vector<uint8_t> Bytes(16);
    Bytes[0] = Switch2Guest::Inet::LinuxAfInet;
    Bytes[2] = static_cast<uint8_t>(Port >> 8);
    Bytes[3] = static_cast<uint8_t>(Port);
    std::memcpy(&Bytes[4], &Address, sizeof(Address));
    return Bytes;
  }

  struct InetDestination {
    std::array<uint8_t, 16> Bsd {}; // what the host is given
    uint32_t Ip {};                 // as the guest wrote it (INADDR_ANY resolved for connect and sendto)
    uint16_t Port {};
    bool Unspec {};
  };

  // move_addr_to_kernel and the checks of inet_bind/connect/udp_sendmsg. AllowUnspec: a datagram
  // connect() with AF_UNSPEC dissolves the association. UnspecIsInet: udp_sendmsg takes AF_UNSPEC as AF_INET.
  int64_t ReadInetAddress(uint64_t Address, uint64_t Length, InetDestination& Out, bool AllowUnspec, bool UnspecIsInet = false) {
    const auto Size = static_cast<int32_t>(Length);
    if (Size < 0 || Size > 128) return Invalid;
    if (Size && Available(Address, PF_R) < static_cast<size_t>(Size)) return Fault;
    uint8_t Raw[128] {};
    if (Size) std::memcpy(Raw, reinterpret_cast<const void*>(Address), static_cast<size_t>(Size));
    const uint16_t Family = Size >= 2 ? static_cast<uint16_t>(Raw[0] | Raw[1] << 8) : 0xffff;
    if (AllowUnspec && Size >= 2 && Family == 0) {
      Out.Unspec = true;
      return 0;
    }
    if (UnspecIsInet && Size >= 16 && Family == 0) Raw[0] = Switch2Guest::Inet::LinuxAfInet;
    if (const auto Result = Switch2Guest::Inet::ToBsdAddress({Raw, static_cast<size_t>(std::max(Size, 0))}, Out.Bsd); Result) return Result;
    ParseBsdAddress(Out.Bsd, Out.Ip, Out.Port);
    return 0;
  }

  // A connect or sendto destination: INADDR_ANY means this host (Linux connects to loopback).
  void ResolveInetDestination(const InetSocket&, InetDestination& Out) {
    if (Out.Ip == 0) Out.Ip = Switch2Guest::Inet::LoopbackAddress;
    Out.Bsd = BsdAddress(Out.Ip, Out.Port);
  }

  // The host side of getsockname after a connect or an implicit bind.
  void NoteInetLocalName(InetSocket& Socket, bool KeepAddress) const {
    std::array<uint8_t, 16> Bsd {};
    if (HostNet::GetName(Socket.HostFd, false, Bsd) < 0) return;
    uint32_t Ip {};
    uint16_t Port {};
    ParseBsdAddress(Bsd, Ip, Port);
    Socket.GuestPort = Port;
    if (!KeepAddress) Socket.GuestAddress = Ip;
    Socket.Bound = true;
  }

  // The implicit bind of listen() and of a datagram send on an unbound socket. Linux binds INADDR_ANY
  // and a free port; the host takes INADDR_ANY as is.
  int64_t AutobindInet(InetSocket& Socket) {
    if (Socket.Bound) return 0;
    if (const auto Error = HostNet::Bind(Socket.HostFd, BsdAddress(0, 0)); Error < 0) return Error;
    NoteInetLocalName(Socket, true);
    Socket.GuestAddress = 0;
    return 0;
  }

  // sock_create for AF_INET (inet_create). SOCK_RAW needs CAP_NET_RAW and, as the host allows only
  // ICMP, protocol 1; datagram ICMP sockets (ping sockets) are not offered.
  int64_t CreateInetSocket(uint64_t Type, uint64_t Protocol) {
    const auto Number = static_cast<int32_t>(Protocol);
    uint8_t Kind {};
    switch (Type & 0xf) {
    case 1:
      if (Number != 0 && Number != Switch2Guest::Inet::LinuxIpprotoTcp) return -93;
      Kind = 1;
      break;
    case 2:
      if (Number != 0 && Number != Switch2Guest::Inet::LinuxIpprotoUdp) return -93;
      Kind = 2;
      break;
    case 3:
      if (!Task->Creds.Capable(CapNetRaw)) return -1;
      if (Number != Switch2Guest::Inet::LinuxIpprotoIcmp) return -93;
      Kind = 3;
      break;
    default: return -94;
    }
    if (!EnsureNetwork()) return -97;
    const auto FD = FreeDescriptor();
    if (FD == NoDescriptor) return -24;
    const auto HostFd = HostNet::Socket(Kind, Kind == 3 ? Switch2Guest::Inet::LinuxIpprotoIcmp : 0);
    // The service holds 126 sockets per process; Linux's EMFILE/ENFILE for that is ENFILE here.
    if (HostFd < 0) return HostFd == -24 ? -23 : HostFd;
    auto Socket = std::make_shared<InetSocket>();
    Socket->HostFd = HostFd;
    Socket->Type = Kind;
    Socket->Protocol = Kind == 1 ? Switch2Guest::Inet::LinuxIpprotoTcp : Kind == 2 ? Switch2Guest::Inet::LinuxIpprotoUdp : Switch2Guest::Inet::LinuxIpprotoIcmp;
    Socket->Nonblocking = Type & 04000;
    auto File = std::make_shared<OpenFile>();
    File->Inet = Socket;
    File->StatusFlags = 2 | (Type & 04000);
    std::erase_if(LiveInetSockets, [](const auto& Weak) { return Weak.expired(); });
    LiveInetSockets.push_back(Socket);
    InstallSocket(FD, std::move(File), Type);
    return FD;
  }

  // ----- bind, listen, connect, accept -----

  int64_t BindInet(InetSocket& Socket, uint64_t Address, uint64_t Length) {
    InetDestination Name;
    if (const auto Result = ReadInetAddress(Address, Length, Name, false); Result) return Result;
    // ip_unprivileged_port_start is 1024. __inet_bind checks it before the socket state.
    if (Name.Port && Name.Port < 1024 && !Task->Creds.Capable(CapNetBindService)) return -13;
    if (Socket.Bound) return Invalid;
    if (const auto Error = HostNet::Bind(Socket.HostFd, BsdAddress(Name.Ip, Name.Port)); Error < 0) return Error;
    NoteInetLocalName(Socket, true);
    Socket.GuestAddress = Name.Ip;
    ++IOEpoch;
    return 0;
  }

  int64_t ListenInet(InetSocket& Socket, int32_t Backlog) {
    if (Socket.Type != 1) return -95;
    if (Socket.Connected || Socket.Connecting) return Invalid;
    if (const auto Error = AutobindInet(Socket); Error < 0) return Error;
    constexpr uint32_t SoMaxConn = 4096;
    const auto Capped = static_cast<int>(std::min(static_cast<uint32_t>(Backlog), SoMaxConn));
    if (const auto Error = HostNet::Listen(Socket.HostFd, Capped); Error < 0) return Error;
    Socket.Listening = true;
    ++IOEpoch;
    return 0;
  }

  // The outcome of a host connect in flight: 0 completed, a negative errno failed, 1 still going.
  // A decided outcome is kept until the next connect() acknowledges it, as Linux keeps SS_CONNECTING:
  // that connect() returns 0 (or the connect error), and only then is the socket SS_CONNECTED.
  int64_t InetConnectProgress(InetSocket& Socket) const {
    HostNet::PollEntry Entry {Socket.HostFd, Switch2Guest::Inet::BsdPollOut, 0};
    if (const auto Result = HostNet::Poll(&Entry, 1, 0); Result < 0) return Result;
    using namespace Switch2Guest::Inet;
    if (!(Entry.Revents & (BsdPollOut | BsdPollErr | BsdPollHup))) return 1;
    int32_t Error {};
    size_t Size = sizeof(Error);
    HostNet::GetOption(Socket.HostFd, BsdSolSocket, BsdSoError, &Error, Size);
    Socket.Connecting = false;
    Socket.ConnectAck = true;
    if (Error) {
      Socket.ConnectError = LinuxErrno(Error);
      Socket.PendingError = Socket.ConnectError; // sk_err: the next SO_ERROR reports it once
      return -Socket.ConnectError;
    }
    // The error was taken already (a getsockopt of the guest): Linux reports ECONNABORTED.
    if ((Entry.Revents & (BsdPollErr | BsdPollHup)) && !(Entry.Revents & BsdPollOut)) {
      Socket.ConnectError = 103;
      return -103;
    }
    Socket.ConnectError = 0;
    Socket.Connected = true;
    NoteInetLocalName(Socket, Socket.GuestAddress != 0);
    ++IOEpoch;
    return 0;
  }

  // A decided connect reported by a call other than connect(): the error is taken (sk_err cleared).
  static int64_t TakeConnectError(InetSocket& Socket, int64_t Progress) {
    if (Progress < 0) Socket.PendingError = 0;
    return Progress;
  }

  // connect() on a socket whose nonblocking connect has been decided (__inet_stream_connect, SS_CONNECTING).
  static int64_t AcknowledgeConnect(InetSocket& Socket) {
    Socket.ConnectAck = false;
    const auto Error = std::exchange(Socket.ConnectError, 0);
    if (!Error) return 0;
    // sock_error: the pending error once, ECONNABORTED when it was read already.
    const auto Pending = std::exchange(Socket.PendingError, 0);
    return -(Pending ? Pending : 103);
  }

  int64_t ConnectInet(InetSocket& Socket, uint64_t Address, uint64_t Length) {
    const bool Stream = Socket.Type == 1;
    if (Stream && Socket.Connecting) {
      if (InetConnectProgress(Socket) != 1) return AcknowledgeConnect(Socket);
      if (Socket.Nonblocking) return -114;
      Socket.WaitEvents |= 4;
      const auto Wait = SocketWait(false, Socket.SndTimeoutNs, WaitKind::Net);
      return Wait == -11 ? -115 : Wait;
    }
    if (Stream && Socket.ConnectAck) return AcknowledgeConnect(Socket);
    InetDestination Target;
    if (const auto Result = ReadInetAddress(Address, Length, Target, !Stream); Result) return Result;
    if (Target.Unspec) {
      // Dissolve the association; the host drops a datagram socket's peer for an address of family 0.
      std::array<uint8_t, 16> None {};
      None[0] = 16;
      if (const auto Error = HostNet::Connect(Socket.HostFd, None); Error < 0 && Error != -97) return Error;
      Socket.Connected = false;
      Socket.PeerAddress = 0;
      Socket.PeerPort = 0;
      ++IOEpoch;
      return 0;
    }
    if (Stream) {
      if (Socket.Connected || Socket.Listening) return -106;
    }
    ResolveInetDestination(Socket, Target);
    const auto Error = HostNet::Connect(Socket.HostFd, Target.Bsd);
    if (Error < 0 && Error != -115) return Error;
    Socket.PeerAddress = Target.Ip;
    Socket.PeerPort = Target.Port;
    if (Error == 0) {
      Socket.Connected = true;
      NoteInetLocalName(Socket, Socket.GuestAddress != 0);
      ++IOEpoch;
      return 0;
    }
    Socket.Connecting = true;
    if (Socket.Nonblocking) return -115;
    Socket.WaitEvents |= 4;
    const auto Wait = SocketWait(false, Socket.SndTimeoutNs, WaitKind::Net);
    return Wait == -11 ? -115 : Wait;
  }

  int64_t AcceptInet(OpenFile& File, uint64_t Address, uint64_t Length, uint64_t Flags) {
    auto& Socket = *File.Inet;
    if (Socket.Type != 1) return -95;
    if (!Socket.Listening) return Invalid;
    if (const auto Result = CheckSocketAddress(Address, Length); Result) return Result;
    const auto NewFD = FreeDescriptor();
    if (NewFD == NoDescriptor) return -24;
    std::array<uint8_t, 16> Peer {};
    const auto HostFd = HostNet::Accept(Socket.HostFd, Peer);
    if (HostFd == -11) {
      if (Socket.Nonblocking) return -11;
      Socket.WaitEvents |= 1;
      return SocketWait(false, Socket.RcvTimeoutNs, WaitKind::Net);
    }
    if (HostFd < 0) return HostFd == -24 ? -23 : HostFd;
    auto Accepted = std::make_shared<InetSocket>();
    static_cast<Switch2Guest::SocketOptions&>(*Accepted) = Socket; // sk_clone_lock: the options carry over
    Accepted->Extra = Socket.Extra;
    Accepted->HostFd = HostFd;
    Accepted->Type = 1;
    Accepted->Protocol = Switch2Guest::Inet::LinuxIpprotoTcp;
    Accepted->Nonblocking = Flags & 04000;
    Accepted->Connected = true;
    ParseBsdAddress(Peer, Accepted->PeerAddress, Accepted->PeerPort);
    NoteInetLocalName(*Accepted, false);
    auto Connection = std::make_shared<OpenFile>();
    Connection->Inet = Accepted;
    Connection->StatusFlags = 2 | (Flags & 04000);
    std::erase_if(LiveInetSockets, [](const auto& Weak) { return Weak.expired(); });
    LiveInetSockets.push_back(Accepted);
    InstallSocket(NewFD, std::move(Connection), Flags);
    if (Address) WriteSocketAddress(Address, Length, LinuxInetAddress(Accepted->PeerAddress, Accepted->PeerPort));
    ++IOEpoch;
    return NewFD;
  }

  int64_t InetAddressCall(bool Peer, InetSocket& Socket, uint64_t Address, uint64_t Length) {
    if (Peer) {
      // inet_getname: no peer in CLOSE, SYN_SENT and LISTEN (and an unconnected datagram socket).
      if (!Socket.Connected) return NotConnected;
      if (Socket.Type == 1) {
        std::array<uint8_t, 16> Check {};
        if (const auto Error = HostNet::GetName(Socket.HostFd, true, Check); Error < 0) return Error;
      }
      return WriteSocketAddress(Address, Length, LinuxInetAddress(Socket.PeerAddress, Socket.PeerPort));
    }
    if (!Socket.Bound) return WriteSocketAddress(Address, Length, LinuxInetAddress(0, 0));
    return WriteSocketAddress(Address, Length, LinuxInetAddress(Socket.GuestAddress, Socket.GuestPort));
  }

  int64_t ShutdownInet(InetSocket& Socket, uint64_t How) {
    if (static_cast<int32_t>(How) < 0 || How > 2) return Invalid;
    if (Socket.Type != 1 && !Socket.Connected) return NotConnected;
    if (const auto Error = HostNet::Shutdown(Socket.HostFd, static_cast<int>(How)); Error < 0) return Error;
    if (How != 1) Socket.ShutRead = true;
    if (How != 0) Socket.ShutWrite = true;
    ++IOEpoch;
    return 0;
  }

  // ----- sending -----

  // The bytes a datagram or stream call sends. Control messages are validated as scm_send does and
  // dropped: Linux accepts SCM_RIGHTS, SCM_CREDENTIALS and IP cmsgs on inet sockets and sends none
  // of them (the IP_TTL and IP_TOS cmsgs of a datagram are accepted but not applied).
  int64_t SendInet(InetSocket& Socket, SocketIo& Io, uint64_t Flags) {
    using namespace Switch2Guest::Inet;
    if (Io.ControlLength) {
      SendControl Control;
      if (const auto Result = ReadControl(Io, Control); Result < 0) return Result;
    }
    // Bits with no host equivalent are dropped: Linux ignores unknown MSG_* bits on send too.
    uint64_t Dropped {};
    const auto HostFlags = *MapSendFlags(Flags, Dropped);
    const bool DontWait = Socket.Nonblocking || (Flags & MsgDontWait);
    if (Socket.Type != 1) return SendInetDatagram(Socket, Io, Flags, HostFlags, DontWait);

    if (Socket.Connecting) {
      const auto Progress = TakeConnectError(Socket, InetConnectProgress(Socket));
      if (Progress < 0) return Progress;
      if (Progress == 1) {
        if (DontWait) return -11;
        Socket.WaitEvents |= 4;
        return SocketWait(false, Socket.SndTimeoutNs, WaitKind::Net);
      }
    }
    if (!Socket.Connected || Socket.ShutWrite) return -32;
    GatheredBytes Bytes;
    if (const auto Result = Gather(Io, Bytes); Result) return Result;
    uint64_t Done = DontWait ? 0 : std::exchange(Current->StreamProgress, 0);
    if (const auto Error = TakeSocketPendingInet(Socket); Error && !Done) return -Error;
    while (Done < Io.Total) {
      const auto Chunk = std::min<uint64_t>(Io.Total - Done, 65536);
      const auto Sent = HostNet::Send(Socket.HostFd, Bytes.Data + Done, Chunk, HostFlags, nullptr);
      if (Sent > 0) {
        Done += static_cast<uint64_t>(Sent);
        continue;
      }
      if (Sent == -11 || Sent == 0) {
        if (DontWait) return Done ? static_cast<int64_t>(Done) : -11;
        Socket.WaitEvents |= 4;
        const auto Wait = SocketWait(false, Socket.SndTimeoutNs, WaitKind::Net);
        if (Wait == -11) return Done ? static_cast<int64_t>(Done) : -11;
        Current->StreamProgress = Done;
        return Wait;
      }
      // A failed send after some bytes: report the bytes, the error comes with the next call.
      if (Done) {
        Socket.PendingError = static_cast<int32_t>(-Sent);
        return static_cast<int64_t>(Done);
      }
      return Sent;
    }
    return static_cast<int64_t>(Done);
  }

  static int32_t TakeSocketPendingInet(InetSocket& Socket) { return std::exchange(Socket.PendingError, 0); }

  // udp_sendmsg and raw_sendmsg.
  int64_t SendInetDatagram(InetSocket& Socket, SocketIo& Io, uint64_t Flags, int HostFlags, bool DontWait) {
    using namespace Switch2Guest::Inet;
    if (Flags & MsgOob) return -95;
    InetDestination Target;
    const bool HasName = Io.Name && static_cast<int32_t>(Io.NameLength) != 0;
    if (HasName) {
      if (const auto Result = ReadInetAddress(Io.Name, Io.NameLength, Target, false, true); Result) return Result;
      if (Target.Port == 0 && Socket.Type == 2) return -22;
    } else if (!Socket.Connected) {
      return -89;
    }
    if (Io.Total > 65507) return -90;
    if (const auto Error = TakeSocketPendingInet(Socket)) return -Error;
    bool UseName = HasName;
    if (HasName) {
      ResolveInetDestination(Socket, Target);
      // The host refuses an address on a connected datagram socket; one that names the peer is the same as none.
      if (Socket.Connected) {
        if (Target.Ip != Socket.PeerAddress || Target.Port != Socket.PeerPort) return -106;
        UseName = false;
      }
    }
    if (!Socket.Bound) {
      if (const auto Error = AutobindInet(Socket); Error < 0) return Error;
    }
    GatheredBytes Bytes;
    if (const auto Result = Gather(Io, Bytes); Result) return Result;
    const auto Sent = HostNet::Send(Socket.HostFd, Bytes.Data, Io.Total, HostFlags, UseName ? &Target.Bsd : nullptr);
    if (Sent >= 0) return static_cast<int64_t>(Io.Total);
    if (Sent == -11) {
      if (DontWait) return -11;
      Socket.WaitEvents |= 4;
      return SocketWait(false, Socket.SndTimeoutNs, WaitKind::Net);
    }
    return Sent;
  }

  // ----- receiving -----

  std::vector<uint8_t> NetScratch;

  // recv, recvfrom, recvmsg and read on an inet socket: the byte count, 0 at EOF, a negative errno,
  // or RetrySyscall after a wait was queued. A datagram goes through a 64 KiB buffer so MSG_TRUNC
  // reports the full length and a short buffer truncates as Linux does.
  int64_t ReceiveInet(InetSocket& Socket, SocketIo& Io, uint64_t Flags, SocketResult& Out) {
    using namespace Switch2Guest::Inet;
    Out.Flags = 0;
    if (Flags & MsgErrQueue) return -11;
    // Bits with no host equivalent are dropped: Linux ignores unknown MSG_* bits on receive too.
    uint64_t Dropped {};
    const auto HostFlags = *MapReceiveFlags(Flags, Dropped);
    const bool Peek = Flags & MsgPeek;
    const bool DontWait = Socket.Nonblocking || (Flags & MsgDontWait);
    if (NetScratch.empty()) NetScratch.resize(65536);
    if (Socket.Type != 1) {
      if (Flags & MsgOob) return -95;
      if (const auto Error = TakeSocketPendingInet(Socket)) return -Error;
      std::array<uint8_t, 16> From {};
      bool HasFrom {};
      const auto Got = HostNet::Receive(Socket.HostFd, NetScratch.data(), NetScratch.size(), HostFlags, &From, HasFrom);
      if (Got == -11) {
        if (DontWait) return -11;
        Socket.WaitEvents |= 1;
        return SocketWait(false, Socket.RcvTimeoutNs, WaitKind::Net);
      }
      if (Got < 0) return Got;
      const auto Copied = std::min<size_t>(Io.Total, static_cast<size_t>(Got));
      Scatter(Io.Vectors, 0, NetScratch.data(), Copied);
      if (static_cast<size_t>(Got) > Io.Total) Out.Flags |= static_cast<uint32_t>(MsgTrunc);
      if (HasFrom) {
        std::array<uint8_t, 16> Linux {};
        if (ToLinuxAddress(From, Linux)) {
          Out.HasInet = true;
          Out.Inet = Linux;
        }
      }
      return (Flags & MsgTrunc) ? Got : static_cast<int64_t>(Copied);
    }

    if (Socket.Listening) return NotConnected;
    if (!Io.Total && Socket.Connected) return 0;
    if (Socket.Connecting) {
      const auto Progress = TakeConnectError(Socket, InetConnectProgress(Socket));
      if (Progress < 0) return Progress;
      if (Progress == 1) {
        if (DontWait) return -11;
        Socket.WaitEvents |= 1;
        return SocketWait(false, Socket.RcvTimeoutNs, WaitKind::Net);
      }
    }
    if (!Socket.Connected && !Socket.ShutRead) return NotConnected;
    const bool Blocking = !DontWait;
    uint64_t Done = Blocking ? std::exchange(Current->StreamProgress, 0) : 0;
    const bool WaitAll = Flags & MsgWaitAll;
    // The bytes the call waits for: SO_RCVLOWAT, or all of them with MSG_WAITALL.
    const uint64_t Target = std::min<uint64_t>(WaitAll ? Io.Total : static_cast<uint64_t>(Socket.RcvLowat), Io.Total);
    if (Done >= Io.Total) return static_cast<int64_t>(Done);
    if (const auto Error = TakeSocketPendingInet(Socket); Error && !Done) return -Error;
    bool Eof {}, Unused {};
    while (Done < Io.Total) {
      const auto Chunk = std::min<uint64_t>(Io.Total - Done, NetScratch.size());
      const auto Got = HostNet::Receive(Socket.HostFd, NetScratch.data(), Chunk, HostFlags, nullptr, Unused);
      if (Got > 0) {
        if (!(Flags & MsgTrunc)) Scatter(Io.Vectors, Done, NetScratch.data(), static_cast<size_t>(Got));
        Done += static_cast<uint64_t>(Got);
        if (Peek || static_cast<uint64_t>(Got) < Chunk) break; // nothing more queued
        continue;
      }
      Eof = Got == 0;
      if (Got == 0) break; // the peer shut its end down (EOF)
      if (Got == -11) break;
      if (Done) {
        Socket.PendingError = static_cast<int32_t>(-Got);
        return static_cast<int64_t>(Done);
      }
      return Got;
    }
    if (Done && (Done >= Target || DontWait || Peek || Eof)) return static_cast<int64_t>(Done);
    if (Eof) return static_cast<int64_t>(Done);
    if (DontWait) return Done ? static_cast<int64_t>(Done) : -11;
    // Below the target: keep what was copied and wait for the rest.
    Socket.WaitEvents |= 1;
    const auto Wait = SocketWait(false, Socket.RcvTimeoutNs, WaitKind::Net);
    if (Wait == -11) return Done ? static_cast<int64_t>(Done) : -11;
    Current->StreamProgress = Done;
    return Wait;
  }

  // read(2) and write(2) on an inet socket.
  int64_t ReadInet(OpenFile& File, char* Buffer, uint64_t Count, uint64_t Flags = 0) {
    SocketIo Io;
    Io.Vectors.push_back({reinterpret_cast<uint64_t>(Buffer), Count});
    Io.Total = Count;
    SocketResult Result;
    return ReceiveInet(*File.Inet, Io, Flags | (File.Nonblocking() ? MsgDontWait : 0), Result);
  }

  int64_t WriteInet(OpenFile& File, const char* Buffer, uint64_t Count, uint64_t Flags = 0) {
    SocketIo Io;
    Io.Vectors.push_back({reinterpret_cast<uint64_t>(Buffer), Count});
    Io.Total = Count;
    return SendInet(*File.Inet, Io, Flags | (File.Nonblocking() ? MsgDontWait : 0));
  }

  // ----- options -----

  // setsockopt for an inet socket. SOL_SOCKET options the shared store handles (and the host mirrors),
  // SOL_IP and SOL_TCP options forwarded to the host or kept here.
  int64_t SetInetOption(InetSocket& Socket, uint64_t Level, uint64_t Name, uint64_t Value, int32_t OptLen) {
    using namespace Switch2Guest::Inet;
    const auto Lvl = static_cast<int32_t>(Level);
    const auto Opt = static_cast<int>(static_cast<int32_t>(Name));
    if (Lvl != LinuxSolSocket && Lvl != LinuxIpprotoIp && !(Lvl == LinuxIpprotoTcp && Socket.Type == 1)) return -92;
    uint8_t Raw[256] {};
    const auto Size = static_cast<size_t>(std::min<int32_t>(OptLen, sizeof(Raw)));
    if (Lvl == LinuxSolSocket) {
      // SO_BINDTODEVICE names are read up to IFNAMSIZ - 1.
      const auto Need = Opt == 25 ? std::min<size_t>(Size, 15) : Switch2Guest::RequiredSetSize(Opt);
      if (Opt != 25 && static_cast<size_t>(OptLen) < Need) return Invalid;
      const auto Read = Opt == 25 ? Need : std::min<size_t>(Need, sizeof(Raw));
      if (Read && Available(Value, PF_R) < Read) return Fault;
      if (Read) std::memcpy(Raw, reinterpret_cast<const void*>(Value), Read);
      if (const auto Result = StoreEmulated(Socket.Extra, Lvl, Opt, Raw, Read); Result != Unhandled) return Result;
      const auto Result = Switch2Guest::SetSocketOption(Socket, Opt, Raw, static_cast<size_t>(OptLen), Task->Creds.Capable(CapNetAdmin));
      if (Result != 0) return Result;
      ++IOEpoch;
      return ForwardInetOption(Socket, Lvl, Opt, Raw, Read);
    }
    if (Size && Available(Value, PF_R) < Size) return Fault;
    if (Size) std::memcpy(Raw, reinterpret_cast<const void*>(Value), Size);
    if (Lvl == LinuxIpprotoIp && (Opt == 1 || Opt == 2)) {
      // ip_setsockopt reads an int, or one byte when optlen is 1 to 3 (0 when
      // optlen is 0). IP_TTL needs optlen >= 1 and -1 or 1..255 (-1 is the
      // default, 64); IP_TOS keeps the low byte. Verified natively.
      int32_t Int = Size >= sizeof(int32_t) ? 0 : Size ? Raw[0] : 0;
      if (Size >= sizeof(int32_t)) std::memcpy(&Int, Raw, sizeof(Int));
      if (Opt == 2) {
        if (!Size || (Int != -1 && (Int < 1 || Int > 255))) return Invalid;
        if (Int == -1) Int = 64;
      } else {
        Int &= 0xff;
      }
      uint8_t Normal[sizeof(int32_t)];
      std::memcpy(Normal, &Int, sizeof(Int));
      return ForwardInetOption(Socket, Lvl, Opt, Normal, sizeof(Normal));
    }
    if (const auto Mapped = MapOption(Lvl, Opt)) {
      if (Mapped->Type == OptionKind::Int || Mapped->Type == OptionKind::Bool) {
        if (static_cast<size_t>(OptLen) < sizeof(int32_t)) return Invalid;
        int32_t Int {};
        std::memcpy(&Int, Raw, sizeof(Int));
        if (const auto Range = CheckOptionRange(Lvl, Opt, Int); Range) return Range;
      }
      return ForwardInetOption(Socket, Lvl, Opt, Raw, Size);
    }
    if (const auto Result = StoreEmulated(Socket.Extra, Lvl, Opt, Raw, Size); Result != Unhandled) return Result;
    return -92;
  }

  // The host half of setsockopt: Raw holds the Linux value.
  int64_t ForwardInetOption(InetSocket& Socket, int Level, int Name, const uint8_t* Raw, size_t Size) {
    using namespace Switch2Guest::Inet;
    const auto Mapped = MapOption(Level, Name);
    if (!Mapped || (Name == SockOpt::Error && Level == LinuxSolSocket)) return 0;
    int32_t Int {};
    if (Size >= sizeof(Int)) std::memcpy(&Int, Raw, sizeof(Int));
    int64_t Error {};
    switch (Mapped->Type) {
    case OptionKind::Bool: {
      const int32_t Flag = Int != 0;
      Error = HostNet::SetOption(Socket.HostFd, Mapped->BsdLevel, Mapped->BsdName, &Flag, sizeof(Flag));
      break;
    }
    case OptionKind::Int:
      Error = HostNet::SetOption(Socket.HostFd, Mapped->BsdLevel, Mapped->BsdName, &Int, sizeof(Int));
      // Buffer sizes are advice to the host; it may clamp or refuse them.
      if (Level == LinuxSolSocket && (Name == SockOpt::SndBuf || Name == SockOpt::RcvBuf)) Error = 0;
      break;
    case OptionKind::Linger: {
      int32_t OnOff {}, Seconds {};
      std::memcpy(&OnOff, Raw, sizeof(OnOff));
      std::memcpy(&Seconds, Raw + 4, sizeof(Seconds));
      const int32_t Linger[2] {OnOff != 0, Seconds};
      Error = HostNet::SetOption(Socket.HostFd, Mapped->BsdLevel, Mapped->BsdName, Linger, sizeof(Linger));
      break;
    }
    case OptionKind::Byte: {
      // IP_MULTICAST_TTL and IP_MULTICAST_LOOP take an int or a byte; the host wants a byte.
      const uint8_t Byte = Size >= sizeof(int32_t) ? static_cast<uint8_t>(Int) : Size >= 1 ? Raw[0] : 0;
      if (Size < 1) return Invalid;
      Error = HostNet::SetOption(Socket.HostFd, Mapped->BsdLevel, Mapped->BsdName, &Byte, 1);
      break;
    }
    case OptionKind::Ip4: {
      // IP_MULTICAST_IF takes an in_addr, or an ip_mreqn whose imr_address is the interface.
      if (Size < 4) return Invalid;
      const uint8_t* Address = Size >= 12 ? Raw + 4 : Raw;
      Error = HostNet::SetOption(Socket.HostFd, Mapped->BsdLevel, Mapped->BsdName, Address, 4);
      break;
    }
    case OptionKind::Membership:
      // ip_mreq (8 bytes) or ip_mreqn (12): group, then the interface address.
      if (Size < 8) return Invalid;
      Error = HostNet::SetOption(Socket.HostFd, Mapped->BsdLevel, Mapped->BsdName, Raw, 8);
      break;
    case OptionKind::Timeval: break;
    }
    return Error < 0 ? Error : 0;
  }

  // getsockopt for an inet socket; Option gets the value bytes.
  int64_t GetInetOption(InetSocket& Socket, int Level, int Name, int32_t Space, Switch2Guest::OptionValue& Option, std::vector<uint8_t>& Large) {
    using namespace Switch2Guest::Inet;
    auto PutInt = [&](int32_t Int) {
      std::memcpy(Option.Data.data(), &Int, sizeof(Int));
      Option.Size = sizeof(Int);
    };
    if (Level == LinuxSolSocket) {
      switch (Name) {
      case SockOpt::Type: PutInt(Socket.Type); return 0;
      case SockOpt::Domain: PutInt(LinuxAfInet); return 0;
      case SockOpt::Protocol: PutInt(Socket.Protocol); return 0;
      case SockOpt::AcceptConn: PutInt(Socket.Listening); return 0;
      case SockOpt::PeerCred: {
        // An inet socket has no peer credentials: pid 0, uid and gid -1.
        const int32_t Parts[3] {0, -1, -1};
        std::memcpy(Option.Data.data(), Parts, sizeof(Parts));
        Option.Size = sizeof(Parts);
        return 0;
      }
      case SockOpt::PeerGroups: case SockOpt::PeerPidfd: return -61; // ENODATA
      case SockOpt::Error: {
        if (const auto Pending = TakeSocketPendingInet(Socket)) {
          PutInt(Pending);
          return 0;
        }
        int32_t Error {};
        size_t Size = sizeof(Error);
        // A connect in flight that finished reports its outcome here (the nonblocking connect idiom).
        if (Socket.Connecting) {
          const auto Progress = TakeConnectError(Socket, InetConnectProgress(Socket));
          if (Progress < 0) {
            PutInt(static_cast<int32_t>(-Progress));
            return 0;
          }
        }
        if (HostNet::GetOption(Socket.HostFd, BsdSolSocket, BsdSoError, &Error, Size) == 0 && Error) Error = LinuxErrno(Error);
        PutInt(Error);
        return 0;
      }
      default: break;
      }
      if (std::vector<uint8_t> Stored; LoadEmulated(Socket.Extra, Level, Name, Stored)) {
        Large = std::move(Stored);
        return 0;
      }
      return Switch2Guest::GetSocketOption(Socket, Name, Option);
    }
    if (Level == LinuxIpprotoTcp && Name == 11) { // TCP_INFO
      Large = TcpInfo(Socket);
      return 0;
    }
    if (const auto Mapped = MapOption(Level, Name)) {
      switch (Mapped->Type) {
      case OptionKind::Membership: return -92;
      case OptionKind::Byte: {
        uint8_t Byte {};
        size_t Size = 1;
        if (const auto Error = HostNet::GetOption(Socket.HostFd, Mapped->BsdLevel, Mapped->BsdName, &Byte, Size); Error < 0) return Error;
        // ip_getsockopt returns an int unless the caller gave less than one.
        if (Space >= static_cast<int32_t>(sizeof(int32_t))) PutInt(Byte);
        else {
          Option.Data[0] = Byte;
          Option.Size = 1;
        }
        return 0;
      }
      case OptionKind::Ip4: {
        size_t Size = 4;
        if (const auto Error = HostNet::GetOption(Socket.HostFd, Mapped->BsdLevel, Mapped->BsdName, Option.Data.data(), Size); Error < 0) return Error;
        Option.Size = 4;
        return 0;
      }
      case OptionKind::Linger: return -92;
      default: {
        int32_t Int {};
        size_t Size = sizeof(Int);
        if (const auto Error = HostNet::GetOption(Socket.HostFd, Mapped->BsdLevel, Mapped->BsdName, &Int, Size); Error < 0) return Error;
        if (Mapped->Type == OptionKind::Bool) Int = Int != 0;
        PutInt(Int);
        return 0;
      }
      }
    }
    if (std::vector<uint8_t> Stored; LoadEmulated(Socket.Extra, Level, Name, Stored)) {
      Large = std::move(Stored);
      return 0;
    }
    return -92;
  }

  int64_t GetInetOptionCall(InetSocket& Socket, uint64_t Level, uint64_t Name, uint64_t Value, uint64_t Length) {
    using namespace Switch2Guest::Inet;
    if (Available(Length, PF_R | PF_W) < sizeof(int32_t)) return Fault;
    int32_t Space {};
    std::memcpy(&Space, reinterpret_cast<const void*>(Length), sizeof(Space));
    if (Space < 0) return Invalid;
    const auto Lvl = static_cast<int32_t>(Level);
    // ip_getsockopt and tcp_getsockopt: a level the protocol does not have is EOPNOTSUPP.
    if (Lvl != LinuxSolSocket && Lvl != LinuxIpprotoIp && !(Lvl == LinuxIpprotoTcp && Socket.Type == 1)) return -95;
    Switch2Guest::OptionValue Option;
    std::vector<uint8_t> Large;
    if (const auto Result = GetInetOption(Socket, Lvl, static_cast<int>(static_cast<int32_t>(Name)), Space, Option, Large); Result) return Result;
    const uint8_t* Data = Large.empty() ? Option.Data.data() : Large.data();
    const size_t Size = Large.empty() ? Option.Size : Large.size();
    const auto Copy = std::min<size_t>(static_cast<size_t>(Space), Size);
    if (Copy && Available(Value, PF_W) < Copy) return Fault;
    if (Copy) std::memcpy(reinterpret_cast<void*>(Value), Data, Copy);
    return CopyOut(Length, static_cast<uint32_t>(Copy));
  }

  // ----- readiness -----

  // Linux poll bits of an inet socket, from a host poll with timeout 0. Updates what the scheduler
  // compares (LastEvents, Generation) and records what a caller that finds nothing ready waits for.
  int16_t InetReadiness(InetSocket& Socket, uint32_t Requested) const {
    using namespace Switch2Guest::Inet;
    HostNet::PollEntry Entry {Socket.HostFd, static_cast<int16_t>(BsdPollIn | BsdPollPri | BsdPollOut), 0};
    if (HostNet::Poll(&Entry, 1, 0) < 0) return 8;
    int16_t Events = FromBsdPoll(Entry.Revents);
    if (Socket.Listening) Events &= static_cast<int16_t>(~4);
    // LastEvents holds host-derived bits only, as PollNet compares them.
    if (Socket.LastEvents != Events) {
      Socket.LastEvents = Events;
      ++Socket.Generation;
    }
    // A connect in flight that the host finished: decide it now (the host may flag a refusal with POLLHUP
    // alone), so a refusal shows as Linux's POLLERR|POLLHUP|POLLIN|POLLOUT and a success as POLLOUT.
    if (Socket.Connecting && (Entry.Revents & (BsdPollOut | BsdPollErr | BsdPollHup))) InetConnectProgress(Socket);
    // The host marks a socket error (ICMP unreachable on a connected datagram socket) as readable without
    // POLLERR; Linux's sk_err shows as POLLERR. Take it into PendingError, which SO_ERROR and recv report once.
    if (!Socket.Listening && !Socket.PendingError && (Entry.Revents & (BsdPollIn | BsdPollErr | BsdPollHup)) &&
        (Socket.Type != 1 || (Entry.Revents & (BsdPollErr | BsdPollHup)))) {
      int32_t Error {};
      size_t Size = sizeof(Error);
      if (HostNet::GetOption(Socket.HostFd, BsdSolSocket, BsdSoError, &Error, Size) == 0 && Error) Socket.PendingError = LinuxErrno(Error);
    }
    int16_t Shown = Events;
    if (Socket.PendingError) Shown |= 8;
    if (Socket.ConnectAck && Socket.ConnectError) {
      Shown |= 1 | 4 | 0x10; // tcp_poll of a closed socket
      if (Socket.PendingError) Shown |= 8;
    }
    // BSD has no POLLRDHUP: a readable stream with nothing to read has seen the peer's FIN.
    if ((Requested & 0x2000) && (Events & 1) && Socket.Type == 1 && !Socket.Listening && HostNet::BytesPending(Socket.HostFd) == 0) {
      Events |= 0x2000;
    }
    // Waiters on bits that are not ready yet: the scheduler's poll ends their wait when one comes up.
    Socket.WaitEvents |= static_cast<int16_t>(Requested & 7 & ~Shown);
    return Shown | (Events & 0x2000);
  }

  // The scheduler's poll of every live inet socket, at most TimeoutMs long. Ends the wait of a
  // thread (++IOEpoch) when a socket's readiness changed or reached what a blocked call waits for.
  bool PollNet(int TimeoutMs) {
    using namespace Switch2Guest::Inet;
    std::erase_if(LiveInetSockets, [](const auto& Weak) { return Weak.expired(); });
    if (LiveInetSockets.empty()) {
      if (TimeoutMs > 0) svcSleepThread(static_cast<int64_t>(TimeoutMs) * 1'000'000);
      return false;
    }
    std::vector<std::shared_ptr<InetSocket>> Sockets;
    std::vector<HostNet::PollEntry> Entries;
    for (const auto& Weak : LiveInetSockets) {
      auto Socket = Weak.lock();
      if (!Socket) continue;
      Entries.push_back({Socket->HostFd, static_cast<int16_t>(BsdPollIn | BsdPollPri | BsdPollOut), 0});
      Sockets.push_back(std::move(Socket));
    }
    const auto Ready = HostNet::Poll(Entries.data(), Entries.size(), TimeoutMs);
    if (Ready < 0 && TimeoutMs > 0) svcSleepThread(static_cast<int64_t>(TimeoutMs) * 1'000'000);
    if (Ready <= 0) return false;
    bool Changed {};
    for (size_t Index = 0; Index < Sockets.size(); ++Index) {
      auto& Socket = *Sockets[Index];
      int16_t Events = FromBsdPoll(Entries[Index].Revents);
      if (Socket.Listening) Events &= static_cast<int16_t>(~4);
      const bool Wanted = (Events & Socket.WaitEvents) != 0;
      if (Events != Socket.LastEvents || Wanted) {
        if (Events != Socket.LastEvents) ++Socket.Generation;
        Socket.LastEvents = Events;
        Socket.WaitEvents = 0;
        Changed = true;
      }
    }
    if (Changed) ++IOEpoch;
    return Changed;
  }

  bool NetWaiting() const {
    return std::any_of(Threads.begin(), Threads.end(), [&](const auto& Thread) {
      return Thread.Core && !Thread.Exited && (Thread.Waiting == WaitKind::Net || (Thread.Waiting == WaitKind::Terminal && !LiveInetSockets.empty()));
    });
  }

  // ----- Syscall front ends -----

  // sendto (44) and recvfrom (45).
  int64_t SocketTransfer(bool Write, uint64_t FD, uint64_t Address, uint64_t Count, uint64_t Flags,
                         uint64_t PeerAddress, uint64_t PeerLength) {
    int64_t Error {};
    auto* File = LookupSocket(FD, Error);
    if (!File) return Error;
    Flags &= 0xffffffff;
    if (!Write) {
      if (const auto Result = CheckSocketAddress(PeerAddress, PeerLength); Result) return Result;
    }
    Count = std::min<uint64_t>(Count, MaxRWCount);
    if (Count && Available(Address, Write ? PF_R : PF_W) < Count) return Fault;
    SocketIo Io;
    Io.Vectors.push_back({Address, Count});
    Io.Total = Count;
    if (Write) {
      Io.Name = PeerAddress;
      Io.NameLength = PeerAddress ? PeerLength : 0;
      return File->Inet ? SendInet(*File->Inet, Io, Flags) : SendUnix(*File->Socket, Io, Flags);
    }
    SocketResult Result;
    const auto Status = File->Inet ? ReceiveInet(*File->Inet, Io, Flags, Result) : ReceiveUnix(*File->Socket, Io, Flags, Result);
    if (Status >= 0 && PeerAddress) {
      // Sockets that are not bound report no address (a zero length).
      if (Result.HasInet) WriteSocketAddress(PeerAddress, PeerLength, std::vector<uint8_t>(Result.Inet.begin(), Result.Inet.end()));
      else WriteSocketAddress(PeerAddress, PeerLength, Result.HasSource ? SocketAddressBytes(Result.Source, true) : std::vector<uint8_t> {});
    }
    return Status;
  }

  // One message of sendmsg, recvmsg, sendmmsg and recvmmsg.
  int64_t SocketMessage(bool Send, uint64_t FD, uint64_t Address, uint64_t Flags) {
    Flags &= 0xffffffff;
    if (Flags & MsgCmsgCompat) return Invalid;
    int64_t Error {};
    auto* File = LookupSocket(FD, Error);
    if (!File) return Error;
    LinuxMessage Message {};
    if (Available(Address, Send ? PF_R : PF_R | PF_W) < sizeof(Message)) return Fault;
    std::memcpy(&Message, reinterpret_cast<const void*>(Address), sizeof(Message));
    SocketIo Io;
    if (const auto Result = ReadVectors(Message, !Send, Io); Result) return Result;
    Io.Name = Message.Name;
    Io.NameLength = Message.Name ? Message.NameLength : 0;
    Io.Control = Message.Control;
    Io.ControlLength = Message.ControlLength;
    if (Send) return File->Inet ? SendInet(*File->Inet, Io, Flags) : SendUnix(*File->Socket, Io, Flags);

    if (Message.ControlLength && Available(Message.Control, PF_W) < Message.ControlLength) return Fault;
    if (Message.Name && static_cast<int32_t>(Message.NameLength) < 0) return Invalid;
    if (Message.Name && Message.NameLength && Available(Message.Name, PF_W) < std::min<uint32_t>(Message.NameLength, SocketAddressMax)) return Fault;
    SocketResult Result;
    const auto Status = File->Inet ? ReceiveInet(*File->Inet, Io, Flags, Result) : ReceiveUnix(*File->Socket, Io, Flags, Result);
    if (Status == RetrySyscall || Status < 0) return Status;
    const uint32_t NameSpace = Message.Name ? Message.NameLength : 0;
    Message.NameLength = 0;
    // The check above covers SocketAddressMax bytes; a Unix path name of the
    // full 108 bytes plus its NUL is longer, so check the real count here.
    const auto PutName = [&](const uint8_t* Data, size_t Size) {
      const auto Count = std::min<size_t>(NameSpace, Size);
      if (Count && Available(Message.Name, PF_W) < Count) return false;
      std::memcpy(reinterpret_cast<void*>(Message.Name), Data, Count);
      Message.NameLength = static_cast<uint32_t>(Size);
      return true;
    };
    if (Result.HasInet && Message.Name) {
      if (!PutName(Result.Inet.data(), Result.Inet.size())) return Fault;
    } else if (Result.HasSource && Message.Name) {
      const auto Bytes = SocketAddressBytes(Result.Source, true);
      if (!PutName(reinterpret_cast<const uint8_t*>(Bytes.data()), Bytes.size())) return Fault;
    }
    Message.Flags = Result.Flags;
    Message.ControlLength = Result.ControlUsed;
    CopyOut(Address, Message);
    return Status;
  }

  // sendmmsg (307) and recvmmsg (299). Each entry is a message and a 32-bit msg_len.
  static constexpr uint64_t MultiMessageSize = sizeof(LinuxMessage) + 8;

  int64_t SendMultiple(uint64_t FD, uint64_t Vector, uint64_t Count, uint64_t Flags) {
    Flags &= 0xffffffff;
    if (Flags & MsgCmsgCompat) return Invalid;
    int64_t Error {};
    if (!LookupSocket(FD, Error)) return Error;
    Count = std::min<uint64_t>(static_cast<uint32_t>(Count), UioMaxIov);
    // After a wait the call resumes at the message that blocked; a stream
    // message continues after the bytes it already queued (StreamProgress).
    uint64_t Done = Current->BatchDone;
    int64_t Failure {};
    auto Finish = [&](int64_t Result) {
      Current->BatchDone = 0;
      Current->BatchEntry = 0;
      return Result;
    };
    for (; Done < Count; ++Done) {
      const auto Entry = Vector + Done * MultiMessageSize;
      if (Available(Entry, PF_R | PF_W) < MultiMessageSize) {
        Failure = Fault;
        break;
      }
      Current->BatchEntry = Entry + sizeof(LinuxMessage);
      const auto Result = SocketMessage(true, FD, Entry, Flags);
      if (Result == RetrySyscall) {
        // Linux sleeps inside the call until the message is queued.
        Current->BatchDone = Done;
        return Result;
      }
      if (Result < 0) {
        Failure = Result;
        break;
      }
      CopyOut(Entry + sizeof(LinuxMessage), static_cast<uint32_t>(Result));
      // Each message gets the whole SO_SNDTIMEO.
      Current->Deadline = 0;
      Current->TimedSocketWait = false;
    }
    return Finish(Done ? static_cast<int64_t>(Done) : Failure);
  }

  int64_t ReceiveMultiple(uint64_t FD, uint64_t Vector, uint64_t Count, uint64_t Flags, uint64_t Timeout) {
    Flags &= 0xffffffff;
    uint64_t Duration {};
    if (Timeout) {
      if (Available(Timeout, PF_R) < 2 * sizeof(int64_t)) return Fault;
      if (!ReadTimespec(Timeout, Duration)) return Invalid;
    }
    if (Flags & MsgCmsgCompat) return Invalid;
    int64_t Error {};
    auto* File = LookupSocket(FD, Error);
    if (!File) return Error;
    Count = std::min<uint64_t>(static_cast<uint32_t>(Count), UioMaxIov);
    // After a wait the call resumes at the message that blocked.
    uint64_t Done = Current->BatchDone;
    if (Timeout && !Current->MultiMessageEnd) Current->MultiMessageEnd = AddDeadline(MonotonicNs(), Duration) | 1;
    auto Finish = [&](int64_t Result) {
      Current->BatchDone = 0;
      Current->BatchEntry = 0;
      Current->MultiMessageEnd = 0;
      return Result;
    };
    for (; Done < Count; ++Done) {
      const auto Entry = Vector + Done * MultiMessageSize;
      if (Available(Entry, PF_R | PF_W) < MultiMessageSize) return Finish(Done ? static_cast<int64_t>(Done) : Fault);
      Current->BatchEntry = Entry + sizeof(LinuxMessage);
      const bool OneIsEnough = (Flags & MsgWaitForOne) && Done;
      const auto Result = SocketMessage(false, FD, Entry, OneIsEnough ? Flags | MsgDontWait : Flags);
      if (Result == RetrySyscall) {
        Current->BatchDone = Done;
        return Result;
      }
      if (Result < 0) {
        // Messages already received are returned; the error comes with the next call (sk_err).
        if (!Done) return Finish(Result);
        if (Result != -11) (File->Inet ? File->Inet->PendingError : File->Socket->PendingError) = static_cast<int32_t>(-Result);
        return Finish(Done);
      }
      CopyOut(Entry + sizeof(LinuxMessage), static_cast<uint32_t>(Result));
      // Each message gets the whole SO_RCVTIMEO.
      Current->Deadline = 0;
      Current->TimedSocketWait = false;
      // The timeout is only looked at between messages.
      if (Current->MultiMessageEnd && MonotonicNs() >= Current->MultiMessageEnd && Done + 1 < Count) return Finish(Done + 1);
    }
    return Finish(Done);
  }

  // ----- Socket options -----

  // setsockopt (54).
  int64_t SetSocketOptionCall(uint64_t FD, uint64_t Level, uint64_t Name, uint64_t Value, uint64_t Length) {
    const auto OptLen = static_cast<int32_t>(Length);
    if (OptLen < 0) return Invalid;
    int64_t Error {};
    auto* File = LookupSocket(FD, Error);
    if (!File) return Error;
    if (File->Inet) return SetInetOption(*File->Inet, Level, Name, Value, OptLen);
    if (static_cast<int32_t>(Level) != 1) return -95;
    const auto Option = static_cast<int>(static_cast<int32_t>(Name));
    const auto Need = Switch2Guest::RequiredSetSize(Option);
    if (static_cast<size_t>(OptLen) < Need) return Invalid;
    uint8_t Raw[16] {};
    if (Available(Value, PF_R) < Need) return Fault;
    std::memcpy(Raw, reinterpret_cast<const void*>(Value), Need);
    const auto Result = Switch2Guest::SetSocketOption(*File->Socket, Option, Raw, OptLen, Task->Creds.Capable(CapNetAdmin));
    if (Result == 0) ++IOEpoch;
    return Result;
  }

  // getsockopt (55). Options of a family come before the shared SOL_SOCKET ones.
  int64_t GetSocketOptionCall(uint64_t FD, uint64_t Level, uint64_t Name, uint64_t Value, uint64_t Length) {
    int64_t Error {};
    auto* File = LookupSocket(FD, Error);
    if (!File) return Error;
    if (File->Inet) return GetInetOptionCall(*File->Inet, Level, Name, Value, Length);
    if (static_cast<int32_t>(Level) != 1) return -95;
    if (Available(Length, PF_R | PF_W) < sizeof(int32_t)) return Fault;
    int32_t Space {};
    std::memcpy(&Space, reinterpret_cast<const void*>(Length), sizeof(Space));
    if (Space < 0) return Invalid;
    auto& Socket = *File->Socket;
    Switch2Guest::OptionValue Option;
    auto PutInt = [&](int32_t Int) {
      std::memcpy(Option.Data.data(), &Int, sizeof(Int));
      Option.Size = sizeof(Int);
    };
    std::vector<uint32_t> Groups;
    switch (static_cast<int>(static_cast<int32_t>(Name))) {
    case SockOpt::Type: PutInt(Socket.Kind == SocketKind::Stream ? 1 : Socket.Kind == SocketKind::Datagram ? 2 : 5); break;
    case SockOpt::Domain: PutInt(1); break;
    case SockOpt::Protocol: PutInt(0); break;
    case SockOpt::AcceptConn: PutInt(Socket.Listening); break;
    case SockOpt::Error: PutInt(TakeSocketError(Socket)); break;
    case SockOpt::PeerCred: {
      const auto Cred = Socket.PeerCred.value_or(SocketCred {});
      // The peer in the caller's namespaces. No peer reads as pid 0 and the overflow ids (Linux: -1 and the
      // unmapped 65534 for an unconnected socket).
      const int32_t Parts[3] {static_cast<int32_t>(PidOut(static_cast<uint32_t>(Cred.Pid))), static_cast<int32_t>(Cred.Uid == Switch2Guest::NoId ? Cred.Uid : UidOut(Cred.Uid)),
                              static_cast<int32_t>(Cred.Gid == Switch2Guest::NoId ? Cred.Gid : GidOut(Cred.Gid))};
      std::memcpy(Option.Data.data(), Parts, sizeof(Parts));
      Option.Size = sizeof(Parts);
      break;
    }
    case SockOpt::PeerGroups: {
      if (!Socket.PeerCred) return -61; // ENODATA
      const uint32_t Needed = static_cast<uint32_t>(Socket.PeerGroups.size() * sizeof(uint32_t));
      if (static_cast<uint32_t>(Space) < Needed) {
        CopyOut(Length, Needed);
        return -34;
      }
      if (Needed && Available(Value, PF_W) < Needed) return Fault;
      if (Needed) std::memcpy(reinterpret_cast<void*>(Value), Socket.PeerGroups.data(), Needed);
      return CopyOut(Length, Needed);
    }
    case SockOpt::PeerPidfd: {
      if (!Socket.PeerCred) return -61;
      if (Space < 4) return Invalid;
      const auto* Process = FindProcess(static_cast<uint32_t>(Socket.PeerCred->Pid));
      if (!Process || Process->Reaped) return -3;
      const auto FD = OpenPidfd(static_cast<uint32_t>(Socket.PeerCred->Pid), 0);
      if (FD < 0) return FD;
      PutInt(static_cast<int32_t>(FD));
      break;
    }
    default:
      if (const auto Result = Switch2Guest::GetSocketOption(Socket, static_cast<int>(static_cast<int32_t>(Name)), Option); Result) return Result;
      break;
    }
    const auto Copy = std::min<size_t>(static_cast<size_t>(Space), Option.Size);
    if (Copy && Available(Value, PF_W) < Copy) return Fault;
    if (Copy) std::memcpy(reinterpret_cast<void*>(Value), Option.Data.data(), Copy);
    return CopyOut(Length, static_cast<uint32_t>(Copy));
  }

  // ----- shutdown -----

  int64_t ShutdownSocket(uint64_t FD, uint64_t How) {
    int64_t Error {};
    auto* File = LookupSocket(FD, Error);
    if (!File) return Error;
    if (File->Inet) return ShutdownInet(*File->Inet, How);
    auto& Socket = *File->Socket;
    if (static_cast<int32_t>(How) < 0 || How > 2) return Invalid;
    // unix_shutdown works on sockets that were never connected.
    if (How != 1) {
      // Queued bytes and records stay readable (verified natively); once they
      // are read a receive returns 0 and peer writes fail with EPIPE.
      Socket.ShutRead = true;
    }
    if (How != 0) Socket.ShutWrite = true;
    ++Socket.Changes;
    ++IOEpoch;
    return 0;
  }

  // ppoll, pselect6, epoll_pwait and epoll_pwait2 install a signal mask for
  // the wait, as rt_sigsuspend does. HandleSyscall restores the caller's mask
  // unless the call ends with EINTR; then the handler frame carries it
  // (restore_saved_sigmask_unless).
  int64_t ReadWaitMask(uint64_t Address, uint64_t Size, std::optional<uint64_t>& Mask) {
    if (!Address) return 0;
    if (Size != sizeof(uint64_t)) return Invalid;
    uint64_t Value {};
    if (Available(Address, PF_R) < sizeof(Value)) return Fault;
    std::memcpy(&Value, reinterpret_cast<const void*>(Address), sizeof(Value));
    Mask = Value;
    return 0;
  }

  void InstallWaitMask(std::optional<uint64_t> Mask) {
    if (!Mask || Current->SuspendMask) return;
    Current->SuspendMask = Current->SignalMask;
    Current->SignalMask = *Mask & ~Unblockable;
  }

  static bool MaskedWait(uint64_t Number) { return Number == 270 || Number == 271 || Number == 281 || Number == 441; }

  // Blocks a poll-type call: EINTR at once when a signal would interrupt it
  // (the wait mask may have unblocked one), else wait for any I/O change.
  int64_t BlockPoll() {
    if (HasActionableSignal(*Current)) return -4;
    // With AF_INET sockets about, the scheduler polls the host while the call waits.
    std::erase_if(LiveInetSockets, [](const auto& Weak) { return Weak.expired(); });
    return WaitForIO(Interactive ? WaitKind::Terminal : LiveInetSockets.empty() ? WaitKind::IO : WaitKind::Net);
  }

  // Linux poll_select_finish: timed calls write back the time left.
  void WriteRemaining(uint64_t Address, uint64_t Duration, bool Timeval) {
    const auto Now = MonotonicNs();
    const auto Left = !Current->Deadline ? Duration : Current->Deadline > Now ? Current->Deadline - Now : 0;
    if (Timeval) CopyOut(Address, GuestTimeval {static_cast<int64_t>(Left / 1'000'000'000), static_cast<int64_t>(Left % 1'000'000'000 / 1000)});
    else CopyOut(Address, ToGuestTimespec(Left));
  }

  int64_t PollGuestWithTimespec(uint64_t Address, uint64_t Count, uint64_t Time, uint64_t Mask, uint64_t MaskSize) {
    uint64_t Duration {};
    if (Time && Available(Time, PF_R) < 2 * sizeof(int64_t)) return Fault;
    if (Time && !ReadTimespec(Time, Duration)) return Invalid;
    std::optional<uint64_t> WaitMask;
    if (const auto Error = ReadWaitMask(Mask, MaskSize, WaitMask)) return Error;
    InstallWaitMask(WaitMask);
    const auto Result = PollGuest(Address, Count, Time ? std::optional {Duration} : std::nullopt);
    if (Result != RetrySyscall && Time) WriteRemaining(Time, Duration, false);
    return Result;
  }

  int64_t PollGuest(uint64_t Address, uint64_t Count, std::optional<uint64_t> Duration) {
    struct PollFD { int32_t FD; int16_t Events, Revents; };
    if (Count > Task->Limits[ResourceNofile][0]) return Invalid; // poll: nfds above RLIMIT_NOFILE
    if (Count > Descriptors().Files.size()) return -12;
    if (Count && Available(Address, PF_R | PF_W) < Count * sizeof(PollFD)) return Fault;
    if (Interactive) PollTerminalInput();
    int64_t ReadyCount {};
    for (size_t Index = 0; Index < Count; ++Index) {
      auto* Position = reinterpret_cast<void*>(Address + Index * sizeof(PollFD));
      PollFD Entry {};
      std::memcpy(&Entry, Position, sizeof(Entry));
      Entry.Revents = 0;
      if (Entry.FD >= 0) {
        const auto* File = GetFile(Entry.FD);
        // POLLERR and POLLHUP are always reported; a closed descriptor is POLLNVAL.
        if (!File) Entry.Revents = 0x20;
        else Entry.Revents = static_cast<int16_t>(Readiness(*File, static_cast<uint16_t>(Entry.Events)) & (static_cast<uint16_t>(Entry.Events) | 0x18));
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
    return BlockPoll();
  }

  // The poll mask of any descriptor: poll, select and epoll all use it.
  // Requested matters only for sockets, which compute some bits on demand.
  uint32_t Readiness(const OpenFile& File, uint32_t Requested = 0x2007) const {
    uint32_t Mask {};
    if (File.Socket) Mask = static_cast<uint16_t>(StreamPoll(File, static_cast<int16_t>(Requested & 0x2007)));
    else if (File.Inet) Mask = static_cast<uint16_t>(InetReadiness(*File.Inet, Requested & 0x2007));
    else if (File.AnonName || File.PidfdTarget || File.Mqueue) return DescriptorEvents(File);
    else if (File.Console == ConsoleStream::Input && !Interactive) return 0;
    else Mask = DescriptorEvents(File);
    // pipe_poll, unix_poll and DEFAULT_POLLMASK add the normal-data bits.
    if (Mask & 1) Mask |= 0x40;
    if (Mask & 4) Mask |= 0x100;
    return Mask;
  }

  // Changes whenever File's readiness may have changed (edge-triggered epoll).
  uint64_t ReadinessGeneration(const OpenFile& File) const {
    auto Mix = [](uint64_t Seed, uint64_t Value) { return (Seed ^ (Value + 0x9e3779b97f4a7c15ULL + (Seed << 6) + (Seed >> 2))) * 0xff51afd7ed558ccdULL; };
    if (File.Socket) {
      const auto& Socket = *File.Socket;
      const auto Other = Socket.Peer.lock();
      uint64_t Value = Mix(Socket.Changes, Other ? Other->Changes : UINT64_MAX);
      Value = Mix(Value, Socket.ShutRead | Socket.ShutWrite << 1 | Socket.Connected << 2 | Socket.Listening << 3 |
                         (Other && Other->ShutRead) << 4 | (Other && Other->ShutWrite) << 5);
      Value = Mix(Value, Socket.Pending.size());
      Value = Mix(Mix(Value, Socket.Records.size()), Socket.Wmem | static_cast<uint64_t>(Socket.ResetPending) << 40);
      if (File.Fifo) Value = Mix(Mix(Mix(Value, File.Fifo->Readers), File.Fifo->Writers), File.Fifo->WriterGeneration);
      return Value;
    }
    if (File.Inet) {
      // The poll that notices a change also bumps Generation.
      InetReadiness(*File.Inet, 0x2007);
      return Mix(File.Inet->Generation, File.Inet->Connected | File.Inet->Listening << 1 | File.Inet->Connecting << 2);
    }
    if (File.TimerFd) return File.TimerFd->Changes;
    if (File.Mqueue) return File.Mqueue->Changes;
    if (File.Event) return File.Event->Changes;
    if (File.SignalFdMask) return SignalChanges;
    if (File.Inotify) return File.Inotify->Instance.Changes();
    if (File.PidfdTarget) {
      const auto It = Processes.find(File.PidfdTarget);
      return It == Processes.end() ? 3 : It->second.Exited + 2 * It->second.Reaped;
    }
    // Terminals and nested epoll instances: any I/O change counts.
    return IOEpoch;
  }

  // select (timeval) and pselect6 (timespec and a signal mask).
  int64_t SelectTimeval(uint64_t Count, uint64_t Read, uint64_t Write, uint64_t Except, uint64_t Timeout) {
    uint64_t Duration {};
    if (Timeout) {
      GuestTimeval Value {};
      if (Available(Timeout, PF_R) < sizeof(Value)) return Fault;
      std::memcpy(&Value, reinterpret_cast<const void*>(Timeout), sizeof(Value));
      // poll_select_set_timeout after normalizing the microseconds.
      const GuestTimespec Time {Value.Seconds + Value.Microseconds / 1'000'000, Value.Microseconds % 1'000'000 * 1000};
      if (!ValidTimespec(Time)) return Invalid;
      Duration = TimespecNs(Time);
    }
    const auto Result = SelectCore(Count, Read, Write, Except, Timeout ? std::optional {Duration} : std::nullopt);
    if (Result != RetrySyscall && Timeout) WriteRemaining(Timeout, Duration, true);
    return Result;
  }

  int64_t SelectGuest(uint64_t Count, uint64_t Read, uint64_t Write, uint64_t Except, uint64_t Timeout, uint64_t SigData) {
    uint64_t Duration {};
    if (Timeout && !ReadTimespec(Timeout, Duration)) return Available(Timeout, PF_R) < 16 ? Fault : Invalid;
    std::optional<uint64_t> WaitMask;
    if (SigData) {
      uint64_t SignalMaskArgs[2];
      if (Available(SigData, PF_R) < sizeof(SignalMaskArgs)) return Fault;
      std::memcpy(SignalMaskArgs, reinterpret_cast<const void*>(SigData), sizeof(SignalMaskArgs));
      if (const auto Error = ReadWaitMask(SignalMaskArgs[0], SignalMaskArgs[1], WaitMask)) return Error;
    }
    InstallWaitMask(WaitMask);
    const auto Result = SelectCore(Count, Read, Write, Except, Timeout ? std::optional {Duration} : std::nullopt);
    if (Result != RetrySyscall && Timeout) WriteRemaining(Timeout, Duration, false);
    return Result;
  }

  // core_sys_select: n above the descriptor table is clamped, negative n is EINVAL.
  int64_t SelectCore(uint64_t CountArgument, uint64_t Read, uint64_t Write, uint64_t Except, std::optional<uint64_t> Duration) {
    if (static_cast<int32_t>(CountArgument) < 0) return Invalid;
    const size_t Count = std::min<size_t>(static_cast<uint32_t>(CountArgument), Descriptors().Files.size());
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
    if (Duration && !Current->Deadline) Current->Deadline = AddDeadline(MonotonicNs(), *Duration);
    if (Interactive) PollTerminalInput();
    // POLLIN_SET, POLLOUT_SET and POLLEX_SET.
    constexpr uint32_t SelectBits[] {0x1 | 0x8 | 0x10 | 0x40 | 0x80, 0x4 | 0x8 | 0x100 | 0x200, 0x2};
    std::array<Mask, 3> ReadySets {};
    int64_t ReadyCount {};
    for (size_t FD = 0; FD < Count; ++FD) {
      const auto Word = FD / 64;
      const auto Bit = 1ULL << (FD % 64);
      if (!((Requested[0][Word] | Requested[1][Word] | Requested[2][Word]) & Bit)) continue;
      const auto* File = GetFile(FD);
      if (!File) return BadFile;
      const auto Events = Readiness(*File, 0x7);
      for (size_t Set = 0; Set < ReadySets.size(); ++Set) {
        if ((Requested[Set][Word] & Bit) && (Events & SelectBits[Set])) {
          ReadySets[Set][Word] |= Bit;
          ++ReadyCount;
        }
      }
    }
    const auto Now = MonotonicNs();
    if (ReadyCount || (Duration && Now >= Current->Deadline) || TerminalClosed) {
      for (size_t Set = 0; Set < ReadySets.size(); ++Set) {
        if (Addresses[Set] && Size) std::memcpy(reinterpret_cast<void*>(Addresses[Set]), ReadySets[Set].data(), Size);
      }
      return ReadyCount;
    }
    return BlockPoll();
  }

  template<typename T>
  int64_t CopyOut(uint64_t Address, const T& Value) {
    if (Available(Address, PF_W) < sizeof(Value)) return Fault;
    std::memcpy(reinterpret_cast<void*>(Address), &Value, sizeof(Value));
    return 0;
  }

  // A default terminate or core action kills every thread of the current
  // process (Linux do_group_exit). No core file is written, so the wait
  // status never carries the core flag (WCOREDUMP).
  void TerminateForSignal(uint32_t Signal) {
    ExitProcess(0, Signal);
  }


  struct GuestSignalFrame {
    uint64_t Restorer;
    struct {
      uint64_t Flags, Link;
      struct { uint64_t Pointer, Flags, Size; } Stack;
      struct { uint64_t Registers[23], FP, Reserved[8]; } Machine;
      uint64_t Mask[16];
    } Context;
    SigInfo Info;
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

  static constexpr uint64_t SaNoCldStop = 0x1, SaNoCldWait = 0x2, SaSigInfo = 0x4, SaOnStack = 0x08000000, SaRestart = 0x10000000, SaNoDefer = 0x40000000,
                            SaResetHand = 0x80000000;
  static constexpr uint64_t SigDefault = 0, SigIgnore = 1;

  // Linux sig_handler_ignored: SIG_IGN, or SIG_DFL with an ignore default
  // (SIGCONT's continue happens when it is sent).
  static bool SignalIgnored(const GuestProcess& Process, uint32_t Signal) {
    const auto Handler = Process.SignalActions[Signal - 1][0];
    if (Handler == SigIgnore) return true;
    const auto Default = DefaultAction(Signal);
    return Handler == SigDefault && (Default == SignalDefault::Ignore || Default == SignalDefault::Continue);
  }

  // Signals the thread may take now: unblocked and not reserved by
  // rt_sigtimedwait.
  uint64_t DeliverableSignals(const GuestThread& Thread) const {
    const auto& Process = Processes.at(Thread.PID);
    return (Thread.Pending.Set() | Process.SharedPending.Set()) & ~(Thread.SignalMask | Thread.WaitSignals);
  }

  // True when delivery would run a handler or a default action.
  bool HasActionableSignal(const GuestThread& Thread) const {
    const auto& Process = Processes.at(Thread.PID);
    for (auto Pending = DeliverableSignals(Thread); Pending; Pending &= Pending - 1) {
      if (!SignalIgnored(Process, std::countr_zero(Pending) + 1)) return true;
    }
    return false;
  }

  // Linux complete_signal's reference thread for a process-directed signal.
  GuestThread* LeaderThread(uint32_t PID) {
    GuestThread* First {};
    for (auto& Thread : Threads) {
      if (!Thread.Core || Thread.Exited || Thread.PID != PID) continue;
      if (Thread.TID == PID) return &Thread;
      if (!First) First = &Thread;
    }
    return First;
  }

  // Linux prepare_signal and __send_signal_locked. Thread is the target for
  // a thread-directed signal, else nullptr. Returns 0 or -EAGAIN.
  int64_t SendSignal(GuestProcess& Target, GuestThread* Thread, const SigInfo& Info) {
    const auto Signal = static_cast<uint32_t>(Info.Signo);
    const auto Bit = SignalBit(Signal);
    auto Discard = [&](uint64_t Mask) {
      Target.SharedPending.Discard(Mask);
      for (auto& Entry : Threads) {
        if (Entry.Core && Entry.PID == Target.PID) Entry.Pending.Discard(Mask);
      }
    };
    if (StopSet & Bit) Discard(SignalBit(SigCont));
    if (Signal == SigCont) {
      Discard(StopSet);
      ContinueProcess(Target);
    }
    // A blocked signal is queued even when ignored: unblocking may follow
    // a change of disposition.
    const auto* Reference = Thread ? Thread : LeaderThread(Target.PID);
    // blocked | real_blocked: rt_sigtimedwait's set is not part of the mask.
    const bool Blocked = Reference && (Reference->SignalMask & Bit);
    if (!Blocked && SignalIgnored(Target, Signal)) return 0;
    // The init of a PID namespace (SIGNAL_UNKILLABLE) ignores signals that have the default action; a user's
    // SIGKILL and SIGSTOP get through only from outside the namespace.
    if (UserSender && Target.Ns.Pid->IsInitOf(Target.PID) && !Blocked && Target.SignalActions[Signal - 1][0] == SigDefault &&
        Switch2Guest::Ns::PidInitDropsSignal(true, true, Target.Ns.Pid->ToLocal(UserSender->PID) != 0, Signal)) {
      return 0;
    }
    auto& Queue = Thread ? Thread->Pending : Target.SharedPending;
    SigInfo Queued = Info;
    if (!RawSigInfo) TranslateSigInfo(Queued, Target);
    if (!Queue.Enqueue(Queued, Target.Limits[ResourceSigpending][0])) return -11;
    ++IOEpoch; // signalfd readers and pollers
    ++SignalChanges;
    return 0;
  }

  // The process whose system call is signalling (kill, tkill, pidfd_send_signal, ...); null for signals the
  // runtime raises itself.
  const GuestProcess* UserSender {};

  // Linux kill_ok_by_cred. SIGCONT may always reach the caller's session.
  bool CanSignal(const GuestProcess& Target, uint32_t Signal) const {
    if (&Target == Task) return true;
    if (Signal == SigCont && Target.Session == Task->Session) return true;
    const auto& From = Task->Creds.Uid;
    const auto& To = Target.Creds.Uid;
    if (NsCapableOver(*Target.Ns.User, Switch2Guest::CapKill)) return true;
    return From[1] == To[0] || From[1] == To[2] || From[0] == To[0] || From[0] == To[2];
  }

  // check_kill_permission, then send. A zombie accepts and drops signals.
  int64_t SignalOne(GuestProcess& Target, GuestThread* Thread, uint32_t Signal, const SigInfo& Info) {
    if (Signal > SigMax) return Invalid;
    if (!CanSignal(Target, Signal)) return -1;
    if (!Signal || Target.Exited) return 0;
    const auto Saved = std::exchange(UserSender, Task);
    const auto Result = SendSignal(Target, Thread, Info);
    UserSender = Saved;
    return Result;
  }

  // kill(2), Linux kill_something_info.
  int64_t SignalProcess(int32_t PID, uint32_t Signal) {
    const auto Info = MakeSigInfo(Signal, SiUser, Task->PID, Task->Creds.Uid[0]);
    if (PID > 0) {
      auto* Target = ProcessOfId(PidIn(PID));
      return Target ? SignalOne(*Target, nullptr, Signal, Info) : -3;
    }
    if (PID == INT32_MIN) return -3;
    if (PID == -1) {
      int64_t Result {};
      size_t Count {};
      for (auto& [ID, Target] : Processes) {
        // Only processes the caller sees, and not its namespace's init (number 1).
        if (PidOut(ID) <= 1 || &Target == Task || Target.Reaped) continue;
        ++Count;
        if (const auto Error = SignalOne(Target, nullptr, Signal, Info); Error != -1) Result = Error;
      }
      return Count ? Result : -3;
    }
    return SignalProcessGroup(PID ? PidIn(static_cast<uint32_t>(-PID)) : Task->Group, Signal, Info);
  }

  // __kill_pgrp_info: success if any member was signalled.
  int64_t SignalProcessGroup(uint32_t Group, uint32_t Signal, const SigInfo& Info) {
    bool Success {};
    int64_t Result = -3;
    for (auto& [ID, Target] : Processes) {
      if (Target.Reaped || Target.Group != Group) continue;
      Result = SignalOne(Target, nullptr, Signal, Info);
      Success |= !Result;
    }
    return Success ? 0 : Result;
  }

  // tkill (TGID -1) and tgkill: Linux do_send_specific.
  int64_t SignalThread(int64_t TGID, uint64_t TID, uint32_t Signal, const SigInfo& Info) {
    for (auto& Thread : Threads) {
      if (!Thread.Core || Thread.Exited || Thread.TID != TID) continue;
      if (TGID > 0 && Thread.PID != TGID) break;
      return SignalOne(Processes.at(Thread.PID), &Thread, Signal, Info);
    }
    return -3;
  }

  int64_t TkillGuest(uint64_t TGID, uint64_t TID, uint64_t Signal) {
    if (static_cast<int32_t>(TGID) <= 0 || static_cast<int32_t>(TID) <= 0) return Invalid;
    return SignalThread(PidIn(TGID), PidIn(TID), Signal, MakeSigInfo(Signal, SiTkill, Task->PID, Task->Creds.Uid[0]));
  }

  // rt_sigqueueinfo and rt_tgsigqueueinfo. Only a thread signalling itself
  // may claim a kernel or kill() origin.
  int64_t QueueSignalInfo(uint64_t TGID, uint64_t TID, uint64_t Signal, uint64_t Address, bool ThreadDirected) {
    if (ThreadDirected && (static_cast<int32_t>(TGID) <= 0 || static_cast<int32_t>(TID) <= 0)) return Invalid;
    SigInfo Info;
    if (Available(Address, PF_R) < sizeof(Info)) return Fault;
    std::memcpy(&Info, reinterpret_cast<const void*>(Address), sizeof(Info));
    if ((Info.Code >= 0 || Info.Code == SiTkill) && static_cast<uint32_t>(TID) != PidOut(Current->TID)) return -1;
    Info.Signo = static_cast<int32_t>(Signal);
    const auto Saved = std::exchange(RawSigInfo, true);
    int64_t Result;
    if (ThreadDirected) {
      Result = SignalThread(PidIn(TGID), PidIn(TID), Signal, Info);
    } else {
      const auto PID = static_cast<int32_t>(TGID);
      auto* Target = PID > 0 ? ProcessOfId(PidIn(PID)) : nullptr;
      Result = Target ? SignalOne(*Target, nullptr, Signal, Info) : -3;
    }
    RawSigInfo = Saved;
    return Result;
  }

  // kill_pid_info: a thread ID names its thread group.
  GuestProcess* ProcessOfId(uint32_t ID) {
    if (auto* Process = FindProcess(ID)) return Process;
    for (const auto& Thread : Threads) {
      if (Thread.Core && !Thread.Exited && Thread.TID == ID) return FindProcess(Thread.PID);
    }
    return nullptr;
  }

  int64_t PendingSignalsGuest(uint64_t Address, uint64_t Size) {
    if (Size > sizeof(uint64_t)) return Invalid;
    const auto Pending = (Current->Pending.Set() | Task->SharedPending.Set()) & Current->SignalMask;
    if (Available(Address, PF_W) < Size) return Fault;
    std::memcpy(reinterpret_cast<void*>(Address), &Pending, Size);
    return 0;
  }

  // pause and rt_sigsuspend wait until delivery interrupts them (EINTR).
  int64_t PauseGuest() {
    Current->Waiting = WaitKind::Signal;
    return RetrySyscall;
  }

  int64_t SuspendGuest(uint64_t Address, uint64_t Size) {
    if (Size != sizeof(uint64_t)) return Invalid;
    uint64_t Mask {};
    if (Available(Address, PF_R) < sizeof(Mask)) return Fault;
    std::memcpy(&Mask, reinterpret_cast<const void*>(Address), sizeof(Mask));
    if (!Current->SuspendMask) {
      Current->SuspendMask = Current->SignalMask;
      Current->SignalMask = Mask & ~Unblockable;
    }
    return PauseGuest();
  }

  int64_t TimedWaitSignal(uint64_t SetAddress, uint64_t InfoAddress, uint64_t Timeout, uint64_t Size) {
    if (Size != sizeof(uint64_t)) return Invalid;
    uint64_t Set {};
    if (Available(SetAddress, PF_R) < sizeof(Set)) return Fault;
    std::memcpy(&Set, reinterpret_cast<const void*>(SetAddress), sizeof(Set));
    Set &= ~Unblockable;
    uint64_t Duration {};
    if (Timeout && Available(Timeout, PF_R) < 2 * sizeof(int64_t)) return Fault;
    if (Timeout && !ReadTimespec(Timeout, Duration)) return Invalid;
    auto Info = TakeSignal(*Current, Set);
    if (Info) {
      Current->WaitSignals = 0;
      if (InfoAddress && CopyOut(InfoAddress, *Info)) return Fault;
      return Info->Signo;
    }
    if (Timeout) {
      const auto Now = MonotonicNs();
      if (!Current->Deadline) Current->Deadline = AddDeadline(Now, Duration);
      if (Current->Deadline <= Now) return -11;
    }
    Current->WaitSignals = Set;
    Current->Waiting = WaitKind::Signal;
    return RetrySyscall;
  }

  // Syscalls Linux restarts after an SA_RESTART handler (-ERESTARTSYS).
  // Sleeps, polls and signal waits return EINTR instead. futex_waitv (449)
  // is listed here for the same reason; futex waits are interrupted through
  // the WaitKind::Futex case of InterruptWait, which applies the rule above.
  static bool RestartableSyscall(uint64_t Number) {
    switch (Number) {
    case 7: case 23: case 34: case 35: case 128: case 130: case 230: case 232: case 270: case 271: case 281: case 441: case 449:
      return false;
    // semop and semtimedop never restart; msgsnd and msgrcv return -ERESTARTNOHAND, which a handler turns into EINTR.
    // mq_timedsend and mq_timedreceive wait with -ERESTARTSYS and restart under SA_RESTART.
    case 65: case 220: case 69: case 70:
      return false;
    default: return true;
    }
  }

  // Ends the wait of a thread about to run a handler for Signal.
  void InterruptWait(GuestThread& Thread, uint32_t Signal) {
    using namespace FEXCore::X86State;
    auto& State = Thread.Core->CurrentFrame->State;
    const bool Restart = Task->SignalActions[Signal - 1][1] & SaRestart;
    // A wait that ends here without running the syscall again (EINTR, a partial count) must not leave the "its
    // filter already allowed it" marker behind, or the next syscall at the same rip would skip its filter.
    struct StaleRetry {
      GuestThread& Thread;
      decltype(State) Registers;
      ~StaleRetry() {
        if (Thread.SeccompRetryRip && Thread.SeccompRetryRip != Registers.rip) Thread.SeccompRetryRip = 0;
      }
    } const Stale {Thread, State};
    switch (Thread.Waiting) {
    case WaitKind::None: return;
    case WaitKind::Sleep: InterruptSleep(Thread); break;
    case WaitKind::Vfork: break; // vfork already returned the child's pid
    case WaitKind::Futex:
      // The wait already advanced rip; RAX still holds the syscall number.
      // LOCK_PI and WAIT_REQUEUE_PI before the requeue restart unconditionally
      // (-ERESTARTNOINTR, verified natively); a WAIT_REQUEUE_PI waiter already
      // requeued to the PI futex returns EAGAIN (rt_mutex_wait_proxy_lock's
      // EINTR becomes EWOULDBLOCK); futex_waitv returns EINTR. A plain wait
      // restarts under SA_RESTART unless it is timed: a timed wait uses a
      // restart block, which a handler turns into EINTR.
      if (Thread.WaitInterrupt == GuestThread::FutexInterrupt::Again) {
        State.gregs[REG_RAX] = -11;
      } else if (Thread.WaitInterrupt == GuestThread::FutexInterrupt::Restart ||
          (Thread.WaitInterrupt == GuestThread::FutexInterrupt::Linux && Restart && !Thread.Deadline)) {
        State.rip -= 2;
      } else {
        State.gregs[REG_RAX] = -4;
      }
      break;
    default:
      // sendmmsg and recvmmsg return the messages they already moved. A
      // stream message half done counts too, with its byte count in msg_len
      // (its sendmsg/recvmsg returned the partial count).
      if (Thread.BatchEntry && (Thread.BatchDone || Thread.StreamProgress)) {
        auto Messages = std::exchange(Thread.BatchDone, 0);
        if (const auto Partial = std::exchange(Thread.StreamProgress, 0)) {
          const auto Length = static_cast<uint32_t>(Partial);
          if (Available(Thread.BatchEntry, PF_W) >= sizeof(Length)) {
            std::memcpy(reinterpret_cast<void*>(Thread.BatchEntry), &Length, sizeof(Length));
            ++Messages;
          }
        }
        State.gregs[REG_RAX] = Messages ? Messages : static_cast<uint64_t>(Fault);
        State.rip += 2;
        break;
      }
      // A stream write that queued bytes returns their count (pipe_write).
      if (Thread.StreamProgress) {
        State.gregs[REG_RAX] = std::exchange(Thread.StreamProgress, 0);
        State.rip += 2;
        break;
      }
      // The syscall instruction has not completed: rerun or fail it.
      if (!Restart || !RestartableSyscall(State.gregs[REG_RAX]) || Thread.TimedSocketWait) {
        State.gregs[REG_RAX] = -4;
        State.rip += 2;
      }
      break;
    }
    Thread.Waiting = WaitKind::None;
    Thread.Deadline = 0;
    Thread.TimedSocketWait = false;
    Thread.BatchDone = 0;
    Thread.BatchEntry = 0;
    Thread.MultiMessageEnd = 0;
    Thread.WaitSignals = 0;
    Thread.PendingRecordLock.reset();
    Thread.IpcBlocked = false;
    Thread.MqWait = nullptr;
  }

  // Linux get_signal for the current thread: drops ignored signals, runs
  // default actions and stacks one frame per caught signal, lowest first,
  // like the kernel's exit-to-user loop.
  void DeliverSignals(GuestThread& Thread) {
    while (!Thread.Exited) {
      // A stopped process only takes SIGKILL.
      const auto Allowed = DeliverableSignals(Thread) & (Task->Stopped ? SignalBit(SigKill) : ~uint64_t {});
      auto Info = TakeSignal(Thread, Allowed);
      if (!Info) return;
      const auto Signal = static_cast<uint32_t>(Info->Signo);
      if (SignalIgnored(*Task, Signal)) continue;
      if (Task->SignalActions[Signal - 1][0] == SigDefault) {
        if (DefaultAction(Signal) == SignalDefault::Stop) {
          // Terminal stops to an orphaned group are discarded (get_signal).
          if (Signal != SigStop && IsOrphanedGroup(Task->Group)) continue;
          StopProcess(*Task, Signal);
          return;
        }
        // Terminate or core; ignore and continue never reach here.
        TerminateForSignal(Signal);
        return;
      }
      InterruptWait(Thread, Signal);
      if (!EnterSignalHandler(Thread.Core->CurrentFrame, *Info)) {
        // Linux force_sigsegv.
        TerminateForSignal(SigSegv);
        return;
      }
    }
  }

  static constexpr auto X87SignalFlags = [] {
    using namespace FEXCore::X86State;
    return std::array {std::pair {X87FLAG_IE_LOC, 0}, std::pair {X87FLAG_C0_LOC, 8},
                       std::pair {X87FLAG_C1_LOC, 9}, std::pair {X87FLAG_C2_LOC, 10},
                       std::pair {X87FLAG_C3_LOC, 14}};
  }();

  bool EnterSignalHandler(CoreFrame* Frame, const SigInfo& Info) {
    using namespace FEXCore::X86State;
    auto& State = Frame->State;
    const auto Signal = static_cast<uint32_t>(Info.Signo);
    const auto Action = Task->SignalActions[Signal - 1];
    const uint64_t Mask = SignalBit(Signal);
    const auto SP = State.gregs[REG_RSP];
    const auto& Alt = Current->AltStack;
    const bool OnAlt = Alt[1] == 0 && SP >= Alt[0] && SP - Alt[0] < Alt[2];
    const bool UseAlt = (Action[1] & SaOnStack) && Alt[1] == 0 && !OnAlt;
    const auto Top = UseAlt ? Alt[0] + Alt[2] : SP;
    if (Top < sizeof(GuestSignalFrame) + 152 || !Action[2] ||
        !(Task->Memory.Query(Action[0]).Flags & PF_X)) return false;
    const auto Address = ((Top - 128 - sizeof(GuestSignalFrame)) & ~uint64_t {15}) - 8;
    GuestSignalFrame Guest {};
    Guest.Restorer = Action[2];
    Guest.Info = Info;
    Guest.Context.Stack.Pointer = Alt[0];
    Guest.Context.Stack.Flags = OnAlt ? 1 : Alt[1];
    Guest.Context.Stack.Size = Alt[2];
    // rt_sigsuspend's caller mask returns with the first frame.
    Guest.Context.Mask[0] = Current->SuspendMask.value_or(Current->SignalMask);
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
    Current->SuspendMask.reset();
    Current->SignalMask |= Action[3] | ((Action[1] & SaNoDefer) ? 0 : Mask);
    Current->SignalMask &= ~Unblockable;
    if (Action[1] & SaResetHand) Task->SignalActions[Signal - 1][0] = SigDefault;
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
    New[3] &= ~Unblockable; // sigdelsetmask(SIGKILL, SIGSTOP)
    Action = New;
    // Linux do_sigaction: a newly ignored signal leaves every pending queue.
    if (Input && SignalIgnored(*Task, Number)) {
      Task->SharedPending.Discard(SignalBit(Number));
      for (auto& Thread : Threads) {
        if (Thread.Core && Thread.PID == Task->PID) Thread.Pending.Discard(SignalBit(Number));
      }
    }
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

  // uname: the host and domain name of the caller's UTS namespace, zero padded to 64 bytes as Linux copies them.
  int64_t UnameGuest(uint64_t Address) {
    char Names[6][65] {"Linux", "", "", "#1 FEX Switch 2", "x86_64", ""};
    std::memcpy(Names[2], UnameRelease.data(), UnameRelease.size());
    const auto& Uts = *Task->Ns.Uts;
    std::memcpy(Names[1], Uts.Hostname.data(), std::min<size_t>(Uts.Hostname.size(), 64));
    std::memcpy(Names[5], Uts.Domainname.data(), std::min<size_t>(Uts.Domainname.size(), 64));
    return CopyOut(Address, Names);
  }

  // sethostname (170) and setdomainname (171): CAP_SYS_ADMIN over the namespace's owner, then a length of at most
  // 64 (the argument is an int), then the copy (EFAULT).
  int64_t SetUtsName(bool Domain, uint64_t Address, uint64_t Length) {
    auto& Uts = *Task->Ns.Uts;
    const auto Size = Switch2Guest::Ns::CheckSetName(SubjectOf(*Task), Uts, Length);
    if (Size < 0) return Size;
    if (Size && AccessiblePrefix(Address, static_cast<size_t>(Size), PF_R) < static_cast<size_t>(Size)) return Fault;
    (Domain ? Uts.Domainname : Uts.Hostname).assign(Size ? reinterpret_cast<const char*>(Address) : "", static_cast<size_t>(Size));
    return 0;
  }

  // copy_process: a non-root user with RLIMIT_NPROC tasks or more cannot
  // create another without CAP_SYS_RESOURCE or CAP_SYS_ADMIN.
  bool NprocExceeded() const {
    const auto Uid = Task->Creds.Uid[0];
    if (!Uid || Task->Creds.Capable(CapSysResource) || Task->Creds.Capable(CapSysAdmin)) return false;
    uint64_t Count {};
    for (const auto& Thread : Threads) {
      if (!Thread.Core || Thread.Exited) continue;
      const auto Owner = Processes.find(Thread.PID);
      Count += Owner != Processes.end() && Owner->second.Creds.Uid[0] == Uid;
    }
    return Count >= Task->Limits[ResourceNproc][0];
  }

  // getrlimit (97), setrlimit (160) and prlimit64 (302).
  int64_t PrLimit(uint64_t Pid, uint64_t Resource, uint64_t Input, uint64_t Output) {
    std::array<uint64_t, 2> New {};
    if (Input) {
      if (Available(Input, PF_R) < sizeof(New)) return Fault;
      std::memcpy(New.data(), reinterpret_cast<const void*>(Input), sizeof(New));
    }
    GuestProcess* Target = Task;
    const auto Id = Pid ? PidIn(static_cast<uint32_t>(Pid)) : 0;
    if (Id && Id != Task->PID) {
      auto It = Processes.find(Id);
      if (It == Processes.end()) {
        const auto Thread = std::find_if(Threads.begin(), Threads.end(), [&](const auto& Entry) { return Entry.Core && Entry.TID == Id; });
        if (Thread != Threads.end()) It = Processes.find(Thread->PID);
      }
      if (It == Processes.end() || It->second.Reaped) return -3;
      Target = &It->second;
      // check_prlimit_permission: all three ids match, or CAP_SYS_RESOURCE.
      const auto& Mine = Task->Creds;
      const auto& Theirs = Target->Creds;
      const bool Same = Theirs.Uid[0] == Mine.Uid[0] && Theirs.Uid[1] == Mine.Uid[0] && Theirs.Uid[2] == Mine.Uid[0] &&
                        Theirs.Gid[0] == Mine.Gid[0] && Theirs.Gid[1] == Mine.Gid[0] && Theirs.Gid[2] == Mine.Gid[0];
      if (!Same && !Mine.Capable(CapSysResource)) return -1;
    }
    // do_prlimit.
    if (Resource >= ResourceCount) return Invalid;
    auto& Limit = Target->Limits[Resource];
    const auto Old = Limit;
    if (Input) {
      if (New[0] > New[1]) return Invalid;
      // sysctl nr_open is the descriptor table size.
      if (Resource == ResourceNofile && New[1] > DescriptorCount) return -1;
      if (New[1] > Limit[1] && !Task->Creds.Capable(CapSysResource)) return -1;
      Limit = New;
    }
    if (Output && CopyOut(Output, Old)) return Fault;
    return 0;
  }

  // RLIMIT_CPU (check_process_timers): SIGXCPU once the soft limit is
  // reached, raising the soft limit by a second each time; SIGKILL at the
  // hard limit.
  static std::optional<uint64_t> CpuLimitDeadline(const GuestProcess& Process) {
    const auto [Soft, Hard] = Process.Limits[ResourceCpu];
    if (Soft == Infinity && Hard == Infinity) return std::nullopt;
    auto Seconds = [](uint64_t Value) { return Value >= UINT64_MAX / 1'000'000'000 ? UINT64_MAX : Value * 1'000'000'000; };
    return std::min(Seconds(Soft), Seconds(Hard));
  }

  void CheckCpuLimit(GuestProcess& Process) {
    const auto Deadline = CpuLimitDeadline(Process);
    if (!Deadline) return;
    const auto Now = ProcessCpuNs(Process);
    auto& [Soft, Hard] = Process.Limits[ResourceCpu];
    if (Hard != Infinity && Now >= Hard * 1'000'000'000) {
      SendSignal(Process, nullptr, MakeSigInfo(SigKill, SiKernel));
      return;
    }
    if (Soft != Infinity && Now >= Soft * 1'000'000'000) {
      if (Soft < Hard) ++Soft;
      SendSignal(Process, nullptr, MakeSigInfo(SigXcpu, SiKernel));
    }
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
    if (const auto* Arena = Task->Memory.GetArena()) {
      Data.TotalRAM = Arena->CommitLimit();
      Data.FreeRAM = Arena->CommitLimit() - Arena->Committed();
    }
    // Linux reports nr_threads.
    Data.Processes = std::count_if(Threads.begin(), Threads.end(), [](const auto& Thread) { return Thread.Core && !Thread.Exited; });
    Data.MemUnit = 1;
    return CopyOut(Address, Data);
  }

  // ---- Scheduling, I/O priority and kcmp (Milestone 4). The decisions are in GuestSched.h. ----

  // The CPUs the guest sees: the host cores this process may run on (the console reports mask 0x3f).
  static uint32_t HostCpuCount() {
    static const uint32_t Count = [] {
      uint64_t Mask {};
      if (R_FAILED(svcGetInfo(&Mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || !Mask) return uint32_t {6};
      return static_cast<uint32_t>(std::clamp(std::popcount(Mask), 1, 64));
    }();
    return Count;
  }

  // find_process_by_pid: 0 is the caller, otherwise a thread ID.
  GuestThread* SchedTarget(int32_t Pid) {
    if (Pid == 0) return Current;
    if (Pid < 0) return nullptr;
    const auto Global = PidIn(static_cast<uint32_t>(Pid));
    for (auto& Thread : Threads) {
      if (Thread.Core && !Thread.Exited && Thread.TID == Global) return &Thread;
    }
    return nullptr;
  }

  Switch2Guest::Sched::Actor SchedActor(const GuestProcess& Target) const {
    Switch2Guest::Sched::Actor Who;
    Who.CapNice = Task->Creds.Capable(CapSysNice);
    Who.SameOwner = Task->Creds.Uid[1] == Target.Creds.Uid[1] || Task->Creds.Uid[1] == Target.Creds.Uid[0];
    Who.RtprioLimit = Target.Limits[ResourceRtprio][0];
    Who.NiceLimit = Target.Limits[ResourceNice][0];
    return Who;
  }

  // sched_setscheduler (Param false) and sched_setparam (Param true).
  int64_t SchedSetScheduler(uint64_t PidArgument, uint64_t PolicyArgument, uint64_t ParamAddress, bool Param) {
    using namespace Switch2Guest::Sched;
    const auto Pid = static_cast<int32_t>(PidArgument);
    if (!ParamAddress || Pid < 0) return Invalid;
    int32_t Priority {};
    if (Available(ParamAddress, PF_R) < sizeof(Priority)) return Fault;
    std::memcpy(&Priority, reinterpret_cast<const void*>(ParamAddress), sizeof(Priority));
    Request Change;
    if (!Param) {
      const auto Policy = static_cast<int32_t>(PolicyArgument);
      if (Policy < 0) return Invalid;
      Change.Policy = Policy & ~PolicyResetOnForkBit;
      Change.Flags = Policy & PolicyResetOnForkBit ? FlagResetOnFork : 0;
    }
    Change.Priority = static_cast<uint32_t>(Priority);
    auto* Thread = SchedTarget(Pid);
    auto* Owner = Thread ? FindProcess(Thread->PID) : nullptr;
    if (!Owner) return -3;
    Change.Nice = Owner->Nice;
    return Apply(Thread->Sched, Owner->Nice, Change, SchedActor(*Owner));
  }

  int64_t SchedGetScheduler(uint64_t PidArgument) {
    const auto Pid = static_cast<int32_t>(PidArgument);
    if (Pid < 0) return Invalid;
    const auto* Thread = SchedTarget(Pid);
    if (!Thread) return -3;
    return Thread->Sched.Policy | (Thread->Sched.ResetOnFork ? Switch2Guest::Sched::PolicyResetOnForkBit : 0);
  }

  int64_t SchedGetParam(uint64_t PidArgument, uint64_t ParamAddress) {
    const auto Pid = static_cast<int32_t>(PidArgument);
    if (!ParamAddress || Pid < 0) return Invalid;
    const auto* Thread = SchedTarget(Pid);
    if (!Thread) return -3;
    const int32_t Priority = Switch2Guest::Sched::IsRt(Thread->Sched.Policy) ? static_cast<int32_t>(Thread->Sched.Priority) : 0;
    return CopyOut(ParamAddress, Priority) ? Fault : 0;
  }

  int64_t SchedRrInterval(uint64_t PidArgument, uint64_t Address) {
    const auto Pid = static_cast<int32_t>(PidArgument);
    if (Pid < 0) return Invalid;
    const auto* Thread = SchedTarget(Pid);
    if (!Thread) return -3;
    const auto Ns = Switch2Guest::Sched::RrInterval(Thread->Sched);
    const int64_t Time[] {static_cast<int64_t>(Ns / 1'000'000'000), static_cast<int64_t>(Ns % 1'000'000'000)};
    return CopyOut(Address, Time) ? Fault : 0;
  }

  int64_t SchedSetAttr(uint64_t PidArgument, uint64_t Address, uint64_t Flags) {
    using namespace Switch2Guest::Sched;
    const auto Pid = static_cast<int32_t>(PidArgument);
    if (!Address || Pid < 0 || Flags) return Invalid;
    uint32_t Size {};
    if (Available(Address, PF_R) < sizeof(Size)) return Fault;
    std::memcpy(&Size, reinterpret_cast<const void*>(Address), sizeof(Size));
    const auto Used = Size ? Size : AttrSizeVer0;
    Attr Decoded;
    if (Used < AttrSizeVer0 || Used > AttrSizeMax) {
      CopyOut(Address, static_cast<uint32_t>(sizeof(Attr))); // sched_copy_attr tells the caller our size
      return Err2big;
    }
    if (Available(Address, PF_R) < Used) return Fault;
    std::vector<uint8_t> Raw(Used);
    std::memcpy(Raw.data(), reinterpret_cast<const void*>(Address), Used);
    if (const auto Error = DecodeAttr(Raw.data(), Size, Decoded)) {
      if (Error == Err2big) CopyOut(Address, static_cast<uint32_t>(sizeof(Attr)));
      return Error;
    }
    if (static_cast<int32_t>(Decoded.Policy) < 0) return Invalid;
    Request Change;
    Change.Policy = Decoded.Flags & FlagKeepPolicy ? int64_t {-1} : int64_t {Decoded.Policy};
    Change.Flags = Decoded.Flags >> 32 ? 0x80000000u : static_cast<uint32_t>(Decoded.Flags); // unknown bits: EINVAL
    Change.Nice = Decoded.Nice;
    Change.Priority = Decoded.Priority;
    Change.Runtime = Decoded.Runtime;
    Change.Deadline = Decoded.Deadline;
    Change.Period = Decoded.Period;
    Change.UtilMin = Decoded.UtilMin;
    Change.UtilMax = Decoded.UtilMax;
    Change.FromAttr = true;
    auto* Thread = SchedTarget(Pid);
    auto* Owner = Thread ? FindProcess(Thread->PID) : nullptr;
    if (!Owner) return -3;
    return Apply(Thread->Sched, Owner->Nice, Change, SchedActor(*Owner));
  }

  int64_t SchedGetAttr(uint64_t PidArgument, uint64_t Address, uint64_t Usize, uint64_t Flags) {
    using namespace Switch2Guest::Sched;
    const auto Pid = static_cast<int32_t>(PidArgument);
    if (!Address || Pid < 0 || Usize > AttrSizeMax || Usize < AttrSizeVer0 || Flags) return Invalid;
    const auto* Thread = SchedTarget(Pid);
    auto* Owner = Thread ? FindProcess(Thread->PID) : nullptr;
    if (!Owner) return -3;
    const auto Reported = Report(Thread->Sched, Owner->Nice, static_cast<uint32_t>(Usize));
    if (Available(Address, PF_W) < Usize) return Fault;
    std::vector<uint8_t> Out(Usize, 0); // the part past our structure reads as zero
    std::memcpy(Out.data(), &Reported, std::min<size_t>(Usize, sizeof(Reported)));
    std::memcpy(reinterpret_cast<void*>(Address), Out.data(), Out.size());
    return 0;
  }

  int64_t GetAffinity(uint64_t Tid, uint64_t Size, uint64_t Address) {
    using namespace Switch2Guest::Sched;
    const auto Count = HostCpuCount();
    const auto Bytes = AffinityBytes(Size, Count);
    if (Bytes < 0) return Bytes;
    const auto* Thread = SchedTarget(static_cast<int32_t>(Tid));
    if (!Thread) return -3;
    const uint64_t Mask = Thread->CpuMask & AllCpus(Count);
    if (Available(Address, PF_W) < static_cast<size_t>(Bytes)) return Fault;
    std::memcpy(reinterpret_cast<void*>(Address), &Mask, Bytes);
    return Bytes;
  }

  // The mask is stored and reported; the host scheduler places threads (deliberate difference).
  int64_t SetAffinity(uint64_t Tid, uint64_t Size, uint64_t Address) {
    using namespace Switch2Guest::Sched;
    const auto Count = HostCpuCount();
    const auto Read = AffinityReadBytes(Size, Count);
    uint64_t Requested {};
    if (Read) {
      if (Available(Address, PF_R) < Read) return Fault;
      std::memcpy(&Requested, reinterpret_cast<const void*>(Address), Read);
    }
    auto* Thread = SchedTarget(static_cast<int32_t>(Tid));
    auto* Owner = Thread ? FindProcess(Thread->PID) : nullptr;
    if (!Owner) return -3;
    if (!SchedActor(*Owner).SameOwner && !Task->Creds.Capable(CapSysNice)) return -1;
    return AffinityApply(Thread->CpuMask, Requested, Count);
  }

  // ioprio_set and ioprio_get targets. A process target is one thread; a group or user target is every thread in it.
  template<typename Visit>
  int64_t ForIoprioTargets(uint64_t WhichArgument, uint64_t WhoArgument, Visit&& Each) {
    using namespace Switch2Guest::Ioprio;
    const auto Which = static_cast<int32_t>(WhichArgument);
    const auto Who = static_cast<int32_t>(WhoArgument);
    if (Which == WhoProcess) {
      auto* Thread = SchedTarget(Who);
      return Thread ? Each(*Thread, true) : -3;
    }
    if (Which != WhoPgrp && Which != WhoUser) return Invalid;
    int64_t Result = -3;
    // A group is a number of the caller's PID namespace, a user an id of its user namespace.
    const auto WantedUser = Which == WhoUser ? (Who ? KuidIn(static_cast<uint32_t>(Who)) : std::optional<uint32_t> {Task->Creds.Uid[0]}) : std::optional<uint32_t> {0};
    if (!WantedUser) return -3;
    const auto Wanted = Which == WhoPgrp ? (Who ? PidIn(static_cast<uint32_t>(Who)) : Task->Group) : *WantedUser;
    for (auto& Thread : Threads) {
      if (!Thread.Core || Thread.Exited) continue;
      const auto* Owner = FindProcess(Thread.PID);
      if (!Owner || (Which == WhoPgrp ? Owner->Group : Owner->Creds.Uid[0]) != Wanted) continue;
      Result = Each(Thread, false);
      if (Result < 0) break;
    }
    return Result;
  }

  int64_t IoprioSet(uint64_t Which, uint64_t Who, uint64_t Value) {
    const auto Packed = static_cast<uint32_t>(Value);
    const bool Privileged = Task->Creds.Capable(CapSysNice) || Task->Creds.Capable(CapSysAdmin);
    if (const auto Error = Switch2Guest::Ioprio::Check(Packed, Privileged)) return Error;
    return ForIoprioTargets(Which, Who, [&](GuestThread& Thread, bool) -> int64_t {
      const auto* Owner = FindProcess(Thread.PID);
      if (!Owner) return -3;
      const auto& Mine = Task->Creds;
      if (Owner->Creds.Uid[0] != Mine.Uid[1] && Owner->Creds.Uid[0] != Mine.Uid[0] && !Mine.Capable(CapSysNice)) return -1;
      Thread.IoPriority = Switch2Guest::Ioprio::Stored(Packed);
      if (!Thread.IoContext) Thread.IoContext = ++NextIoContext;
      return 0;
    });
  }

  // ioprio_get: one thread's own value; for a group or user the best (numerically least) of its threads, where a
  // thread without a class counts as best-effort at the level its nice value gives.
  int64_t IoprioGet(uint64_t Which, uint64_t Who) {
    int64_t Best = -1;
    const auto Result = ForIoprioTargets(Which, Who, [&](GuestThread& Thread, bool Single) -> int64_t {
      const auto* Owner = FindProcess(Thread.PID);
      if (!Owner) return -3;
      if (Single) {
        Best = Thread.IoPriority;
        return 0;
      }
      const auto Value = Switch2Guest::Ioprio::Effective(Thread.IoPriority, Owner->Nice);
      Best = Best < 0 ? Value : std::min<int64_t>(Best, Value);
      return 0;
    });
    return Result < 0 ? Result : Best;
  }

  // kcmp: the order of two kernel objects of one kind, as seen from two threads. Objects are named by the runtime
  // table that holds them (descriptor table, address space, signal table, ...).
  int64_t Kcmp(uint64_t Pid1, uint64_t Pid2, uint64_t TypeArgument, uint64_t Index1, uint64_t Index2) {
    using namespace Switch2Guest::Kcmp;
    const auto Find = [&](uint64_t Pid) -> GuestThread* {
      const auto Id = static_cast<int32_t>(Pid);
      if (Id <= 0) return nullptr;
      const auto Global = PidIn(static_cast<uint32_t>(Id));
      for (auto& Thread : Threads) {
        if (Thread.Core && !Thread.Exited && Thread.TID == Global) return &Thread;
      }
      return nullptr;
    };
    auto* First = Find(Pid1);
    auto* Second = Find(Pid2);
    if (!First || !Second) return Switch2Guest::ErrSrch;
    auto* Owner1 = FindProcess(First->PID);
    auto* Owner2 = FindProcess(Second->PID);
    if (!Owner1 || !Owner2) return Switch2Guest::ErrSrch;
    // ptrace_may_access(PTRACE_MODE_READ_REALCREDS) on both tasks.
    for (const auto* Owner : {Owner1, Owner2}) {
      if (Owner != Task && !MayPtrace(*Owner)) return Switch2Guest::ErrPerm;
    }
    const auto Pointer = [](const auto* Object) { return reinterpret_cast<uint64_t>(Object); };
    const auto FileIn = [](const GuestThread& Thread, uint64_t FD) -> const OpenFile* {
      return Thread.Descriptors && FD < Thread.Descriptors->Files.size() ? Thread.Descriptors->Files[FD].get() : nullptr;
    };
    switch (static_cast<int32_t>(TypeArgument) < 0 ? static_cast<uint32_t>(TypeCount) : static_cast<uint32_t>(TypeArgument)) {
    case File: {
      const auto* Left = FileIn(*First, Index1);
      const auto* Right = FileIn(*Second, Index2);
      return Left && Right ? Order(Pointer(Left), Pointer(Right)) : Switch2Guest::ErrBadf;
    }
    case Vm: return Order(Pointer(&Owner1->Memory), Pointer(&Owner2->Memory));
    case Files: return Order(Pointer(First->Descriptors.get()), Pointer(Second->Descriptors.get()));
    case Fs: return Order(Pointer(Owner1->Fs.get()), Pointer(Owner2->Fs.get())); // threads share their process's directory state
    case Sighand: return Order(Pointer(&Owner1->SignalActions), Pointer(&Owner2->SignalActions));
    case Io: return Order(First->IoContext, Second->IoContext);
    case Sysvsem: return Order(Pointer(Owner1->SemUndo.get()), Pointer(Owner2->SemUndo.get()));
    case EpollTfd: {
      // idx1 is a descriptor of the first task; idx2 points to {efd, tfd, toff} naming an epoll item of the second.
      uint32_t Slot[3] {};
      if (Available(Index2, PF_R) < sizeof(Slot)) return Fault;
      std::memcpy(Slot, reinterpret_cast<const void*>(Index2), sizeof(Slot));
      const auto* File = FileIn(*First, Index1);
      const auto* Epoll = FileIn(*Second, Slot[0]);
      if (!File || !Epoll) return Switch2Guest::ErrBadf;
      if (!Epoll->Epoll) return Switch2Guest::ErrInval;
      ForgetClosed(*Epoll->Epoll);
      uint32_t Skip = Slot[2];
      const OpenFile* Found {};
      Epoll->Epoll->Set.ForEachKey([&](const EpollKey& Key) {
        if (Found || Key.Fd != static_cast<int32_t>(Slot[1])) return;
        if (Skip) --Skip;
        else Found = Key.Raw;
      });
      return Found ? Order(Pointer(File), Pointer(Found)) : Switch2Guest::ErrNoent;
    }
    default: return Switch2Guest::ErrInval;
    }
  }

  static constexpr int64_t NotPermitted = -1;
  static constexpr int64_t NotSupported = -95;

  int64_t ClockGuest(uint64_t ClockId, uint64_t Address) {
    uint64_t Ns {};
    if (const auto Error = ReadClock(Switch2Guest::DecodeClock(static_cast<int32_t>(ClockId)), Ns)) return Error;
    const int64_t Time[] {static_cast<int64_t>(Ns / 1'000'000'000), static_cast<int64_t>(Ns % 1'000'000'000)};
    return CopyOut(Address, Time);
  }

  int64_t ClockResolution(uint64_t ClockId, uint64_t Address) {
    const auto Clock = Switch2Guest::DecodeClock(static_cast<int32_t>(ClockId));
    if (Clock.Kind == Switch2Guest::ClockKind::Invalid) return Invalid;
    if ((Clock.Kind == Switch2Guest::ClockKind::ProcessCpu || Clock.Kind == Switch2Guest::ClockKind::ThreadCpu) &&
        static_cast<int32_t>(ClockId) < 0 && !CpuClockValid(Clock)) return Invalid;
    if (!Address) return 0;
    const int64_t Time[] {0, static_cast<int64_t>(Switch2Guest::ResolutionNs(Clock))};
    return CopyOut(Address, Time);
  }

  int64_t GetTimeOfDay(uint64_t TimeValue, uint64_t Timezone) {
    if (TimeValue) {
      const auto Ns = RealTimeNs();
      const int64_t Value[] {static_cast<int64_t>(Ns / 1'000'000'000), static_cast<int64_t>(Ns % 1'000'000'000 / 1000)};
      if (CopyOut(TimeValue, Value)) return Fault;
    }
    if (Timezone) {
      const int32_t Zone[] {TimezoneMinutesWest, TimezoneDstTime};
      if (CopyOut(Timezone, Zone)) return Fault;
    }
    return 0;
  }

  int64_t TimeGuest(uint64_t Address) {
    const int64_t Seconds = RealTimeNs() / 1'000'000'000;
    if (Address && CopyOut(Address, Seconds)) return Fault;
    return Seconds;
  }

  // Sets the guest realtime clock to Seconds.Nanoseconds. Linux do_sys_settimeofday64 checks the time
  // is valid, then CAP_SYS_TIME.
  static constexpr int64_t MaxClockSeconds = INT64_MAX / 1'000'000'000;
  int64_t SetRealtime(int64_t Seconds, int64_t Nanoseconds) {
    if (Seconds < 0 || Seconds > MaxClockSeconds || Nanoseconds < 0 || Nanoseconds >= 1'000'000'000) return Invalid;
    if (!Task->Creds.Capable(Switch2Guest::CapSysTime)) return NotPermitted;
    Realtime.Set(Seconds * 1'000'000'000 + Nanoseconds, MonotonicNs());
    RealtimeClockSet();
    return 0;
  }

  int64_t SetTimeOfDay(uint64_t TimeValue, uint64_t Timezone) {
    int64_t Value[2] {};
    int32_t Zone[2] {};
    if (TimeValue) {
      if (Available(TimeValue, PF_R) < sizeof(Value)) return Fault;
      std::memcpy(Value, reinterpret_cast<const void*>(TimeValue), sizeof(Value));
      if (static_cast<uint64_t>(Value[1]) >= 1'000'000) return Invalid;
    }
    if (Timezone) {
      if (Available(Timezone, PF_R) < sizeof(Zone)) return Fault;
      std::memcpy(Zone, reinterpret_cast<const void*>(Timezone), sizeof(Zone));
    }
    if (TimeValue && (Value[0] < 0 || Value[0] > MaxClockSeconds)) return Invalid;
    if (!Task->Creds.Capable(Switch2Guest::CapSysTime)) return NotPermitted;
    if (Timezone) {
      if (Zone[0] > 15 * 60 || Zone[0] < -15 * 60) return Invalid;
      TimezoneMinutesWest = Zone[0];
      TimezoneDstTime = Zone[1];
      // Linux also warps the clock by the zone on the first settimeofday(NULL, tz) after boot;
      // not modelled (ProcessInventory.md).
    }
    return TimeValue ? SetRealtime(Value[0], Value[1] * 1000) : 0;
  }

  int64_t ClockSetTime(uint64_t ClockId, uint64_t Address) {
    const auto Clock = Switch2Guest::DecodeClock(static_cast<int32_t>(ClockId));
    // Dynamic CPU clocks validate their target, then can never be set. Every other clock but
    // CLOCK_REALTIME has no set operation (the static CPU clock ids included).
    if (static_cast<int32_t>(ClockId) < 0 && Clock.Kind != Switch2Guest::ClockKind::Invalid) return CpuClockValid(Clock) ? NotPermitted : Invalid;
    if (Clock.Kind != Switch2Guest::ClockKind::Realtime) return Invalid;
    int64_t Time[2] {};
    if (Available(Address, PF_R) < sizeof(Time)) return Fault;
    std::memcpy(Time, reinterpret_cast<const void*>(Address), sizeof(Time));
    return SetRealtime(Time[0], Time[1]);
  }

  // struct timex on x86-64 (208 bytes).
  struct GuestTimex {
    uint32_t Modes;
    int64_t Offset, Frequency, MaxError, EstError;
    int32_t Status;
    int64_t Constant, Precision, Tolerance, TimeSeconds, TimeSub, Tick, PpsFrequency, Jitter;
    int32_t Shift;
    int64_t Stability, JitterCount, CalibrationCount, ErrorCount, StabilityCount;
    int32_t Tai;
    int32_t Padding[11];
  };
  static_assert(sizeof(GuestTimex) == 208);

  // adjtimex and clock_adjtime(CLOCK_REALTIME). The validation and error order follow Linux
  // (timekeeping_validate_timex); the NTP discipline itself is not run: write modes store their
  // fields for read-back without steering the clock, except ADJ_SETOFFSET and ADJ_TAI.
  int64_t AdjustTime(uint64_t Address) {
    constexpr uint32_t AdjOffset = 0x1, AdjFrequency = 0x2, AdjMaxError = 0x4, AdjEstError = 0x8, AdjStatus = 0x10,
                       AdjTimeConstant = 0x20, AdjTai = 0x80, AdjSetOffset = 0x100, AdjMicro = 0x1000, AdjNano = 0x2000,
                       AdjTick = 0x4000, AdjAdjtime = 0x8000, AdjSingleShot = 0x1, AdjReadOnly = 0x2000;
    constexpr int32_t StaNano = 0x2000, StaReadOnly = 0xff00, StaUnsync = 0x40;
    constexpr int64_t MaxFrequency = 500 * 65536, MaxPhase = 500'000'000;
    GuestTimex Request {};
    if (Available(Address, PF_R) < sizeof(Request)) return Fault;
    std::memcpy(&Request, reinterpret_cast<const void*>(Address), sizeof(Request));
    const bool CanSet = Task->Creds.Capable(Switch2Guest::CapSysTime);
    const auto Modes = Request.Modes;
    if (Modes & AdjAdjtime) {
      if (!(Modes & AdjSingleShot)) return Invalid;
      if (!(Modes & AdjReadOnly) && !CanSet) return NotPermitted;
    } else {
      if (Modes && !CanSet) return NotPermitted;
      if ((Modes & AdjTick) && (Request.Tick < 900'000 / 100 || Request.Tick > 1'100'000 / 100)) return Invalid;
    }
    int64_t Delta {};
    if (Modes & AdjSetOffset) {
      if (!CanSet) return NotPermitted;
      const int64_t Sub = Request.TimeSub;
      if (Sub < 0 || Sub >= ((Modes & AdjNano) ? 1'000'000'000 : 1'000'000)) return Invalid;
      const int64_t Seconds = Request.TimeSeconds;
      if (Seconds > MaxClockSeconds || Seconds < -MaxClockSeconds) return Invalid;
      Delta = Seconds * 1'000'000'000 + ((Modes & AdjNano) ? Sub : Sub * 1000);
      if (Realtime.Now(MonotonicNs()) + Delta < 0) return Invalid;
    }
    if ((Modes & AdjTai) && Request.Constant < 0) return Invalid;
    if (Modes & AdjSetOffset) {
      Realtime.SetOffset(Delta, MonotonicNs());
      RealtimeClockSet();
    }
    // adjtime(3) single-shot slews are accepted without slewing.
    if (!(Modes & AdjAdjtime)) {
      if (Modes & AdjStatus) Ntp.Status = (Ntp.Status & StaReadOnly) | (Request.Status & ~StaReadOnly);
      if (Modes & AdjNano) Ntp.Status |= StaNano;
      if (Modes & AdjMicro) Ntp.Status &= ~StaNano;
      if (Modes & AdjOffset) Ntp.Offset = std::clamp<int64_t>(Request.Offset, -MaxPhase / ((Ntp.Status & StaNano) ? 1 : 1000), MaxPhase / ((Ntp.Status & StaNano) ? 1 : 1000));
      if (Modes & AdjFrequency) Ntp.Frequency = std::clamp<int64_t>(Request.Frequency, -MaxFrequency, MaxFrequency);
      if (Modes & AdjMaxError) Ntp.MaxError = Request.MaxError;
      if (Modes & AdjEstError) Ntp.EstError = Request.EstError;
      if (Modes & AdjTimeConstant) Ntp.Constant = std::clamp<int64_t>(Request.Constant, 0, 10);
      if (Modes & AdjTick) Ntp.Tick = Request.Tick;
      if (Modes & AdjTai) Realtime.TaiOffsetNs = Request.Constant * 1'000'000'000;
    }
    GuestTimex Reply {};
    Reply.Modes = Modes;
    const auto Now = static_cast<uint64_t>(std::max<int64_t>(Realtime.Now(MonotonicNs()), 0));
    Reply.TimeSeconds = static_cast<int64_t>(Now / 1'000'000'000);
    Reply.TimeSub = static_cast<int64_t>(Now % 1'000'000'000) / ((Ntp.Status & StaNano) ? 1 : 1000);
    Reply.Offset = Ntp.Offset;
    Reply.Frequency = Ntp.Frequency;
    Reply.MaxError = Ntp.MaxError;
    Reply.EstError = Ntp.EstError;
    Reply.Status = Ntp.Status;
    Reply.Constant = Ntp.Constant;
    Reply.Precision = 1;
    Reply.Tolerance = MaxFrequency / 1;
    Reply.Tick = Ntp.Tick;
    Reply.Tai = static_cast<int32_t>(Realtime.TaiOffsetNs / 1'000'000'000);
    if (CopyOut(Address, Reply)) return Fault;
    return (Ntp.Status & StaUnsync) ? 5 : 0; // TIME_ERROR while unsynchronised, else TIME_OK
  }

  int64_t ClockAdjustTime(uint64_t ClockId, uint64_t Address) {
    const auto Clock = Switch2Guest::DecodeClock(static_cast<int32_t>(ClockId));
    if (Clock.Kind == Switch2Guest::ClockKind::Invalid) return Invalid;
    if (Clock.Kind != Switch2Guest::ClockKind::Realtime) return NotSupported;
    return AdjustTime(Address);
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
    if (Operation == XattrOp::Set || Operation == XattrOp::Remove) {
      if (const auto Denied = MountDenies(StatMount, false)) return Denied;
    }

    const auto Metadata = Inodes.find(Info.Inode);
    auto* Attributes = Metadata == Inodes.end() ? nullptr : &Metadata->second.Attributes;
    const bool Admin = Task->Creds.Capable(CapSysAdmin);
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
    const bool Owner = Info.Uid == Task->Creds.Uid[3] || (Task->Creds.OwnCapable(CapFowner) && InodeMapped(Info.Inode, false));
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
          if (Task->Creds.Uid[3] && !InGuestGroup(Info.Gid)) *Record.Mode &= ~02000;
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
    if (File->AnonName) return AnonymousIoctl(*const_cast<OpenFile*>(File), Command, Argument);
    if (File->Inet && (Command == 0x541b || Command == 0x5411)) { // FIONREAD (SIOCINQ) and SIOCOUTQ
      // Deliberate: the host reports no send queue, so SIOCOUTQ is 0.
      if (Command == 0x5411) return CopyOut(Argument, int32_t {0});
      if (File->Inet->Listening) return Invalid;
      if (File->Inet->Type != 1) {
        // udp_ioctl: the size of the next datagram. The host counts every queued byte, so peek the next one.
        using namespace Switch2Guest::Inet;
        if (NetScratch.empty()) NetScratch.resize(65536);
        bool HasFrom {};
        const auto Got = HostNet::Receive(File->Inet->HostFd, NetScratch.data(), NetScratch.size(), BsdMsgPeek | BsdMsgDontWait, nullptr, HasFrom);
        // A socket error the peek took is kept for the next SO_ERROR or receive.
        if (Got < 0 && Got != -11 && !File->Inet->PendingError) File->Inet->PendingError = static_cast<int32_t>(-Got);
        return CopyOut(Argument, static_cast<int32_t>(Got > 0 ? Got : 0));
      }
      const auto Bytes = HostNet::BytesPending(File->Inet->HostFd);
      if (Bytes < 0) return Bytes;
      return CopyOut(Argument, static_cast<int32_t>(Bytes));
    }
    if (Command == 0x541b && File->Socket) { // FIONREAD
      if (File->Pipe && (File->StatusFlags & 3) == 1) {
        const auto Reader = File->Socket->Peer.lock();
        return CopyOut(Argument, static_cast<int32_t>(Reader ? Reader->Input.size() : 0));
      }
      const auto& Queue = *File->Socket;
      // A datagram or seqpacket socket reports the size of the next record.
      if (Queue.Kind != Switch2Guest::SocketKind::Stream) {
        return CopyOut(Argument, static_cast<int32_t>(Queue.Records.empty() ? 0 : Queue.Records.front().Bytes));
      }
      return CopyOut(Argument, static_cast<int32_t>(Queue.Input.size()));
    }
    if (!Interactive || File->Console == ConsoleStream::None) return -25;
    switch (Command) {
    case 0x5401: return CopyOut(Argument, Terminal.Attributes); // TCGETS
    case 0x5402: case 0x5403: case 0x5404: {
      if (const auto Access = TerminalAccess(SigTtou)) return Access;
      TerminalAttributes New;
      if (Available(Argument, PF_R) < sizeof(New)) return Fault;
      std::memcpy(&New, reinterpret_cast<const void*>(Argument), sizeof(New));
      Terminal.SetAttributes(New, Command == 0x5404);
      return 0;
    }
    case 0x540b: // TCFLSH
      if (Argument > 2) return Invalid;
      if (const auto Access = TerminalAccess(SigTtou)) return Access;
      if (Argument != 1) Terminal.Clear();
      return 0;
    case 0x540f: // TIOCGPGRP
      if (!ControlsTerminal()) return -25;
      return CopyOut(Argument, ForegroundGroup);
    case 0x5429: // TIOCGSID
      if (!ControlsTerminal() || !TerminalSession) return -25;
      return CopyOut(Argument, TerminalSession);
    case 0x5410: { // TIOCSPGRP
      if (!ControlsTerminal()) return -25;
      if (const auto Access = TerminalAccess(SigTtou)) return Access;
      int32_t Group {};
      if (Available(Argument, PF_R) < sizeof(Group)) return Fault;
      std::memcpy(&Group, reinterpret_cast<const void*>(Argument), sizeof(Group));
      if (Group < 0) return Invalid;
      const bool Exists = std::any_of(Processes.begin(), Processes.end(), [&](const auto& Item) {
        return !Item.second.Reaped && Item.second.Group == static_cast<uint32_t>(Group);
      });
      if (!Exists) return -3;
      if (!GroupInSession(Group, Task->Session)) return -1;
      ForegroundGroup = Group;
      return 0;
    }
    case 0x540e: { // TIOCSCTTY
      const bool Leader = Task->Session == Task->PID;
      if (Leader && Task->HasTerminal && TerminalSession == Task->Session) return 0;
      if (!Leader || Task->HasTerminal) return -1;
      if (TerminalSession) {
        if (Argument != 1 || !Task->Creds.Capable(CapSysAdmin)) return -1;
        for (auto& [ID, Member] : Processes) {
          if (Member.Session == TerminalSession) Member.HasTerminal = false;
        }
      }
      TerminalSession = Task->Session;
      ForegroundGroup = Task->Group;
      Task->HasTerminal = true;
      return 0;
    }
    case 0x5422: // TIOCNOTTY
      if (!ControlsTerminal()) return -25;
      if (Task->Session == Task->PID) {
        // disassociate_ctty: the foreground group gets SIGHUP and SIGCONT.
        if (ForegroundGroup) {
          SignalGroup(ForegroundGroup, SigHup);
          SignalGroup(ForegroundGroup, SigCont);
        }
        for (auto& [ID, Member] : Processes) {
          if (Member.Session == Task->Session) Member.HasTerminal = false;
        }
        TerminalSession = 0;
        ForegroundGroup = 0;
      }
      Task->HasTerminal = false;
      return 0;
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

  bool ControlsTerminal() const {
    return Interactive && Task->HasTerminal && TerminalSession && Task->Session == TerminalSession;
  }

  // Linux __tty_check_change and job_control for the console: a process in
  // a background group that uses its controlling terminal gets Signal for
  // its group and retries; EIO when SIGTTIN is ignored or blocked or the
  // group is orphaned. An ignored or blocked SIGTTOU permits the access.
  int64_t TerminalAccess(uint32_t Signal) {
    if (!ControlsTerminal() || !ForegroundGroup || Task->Group == ForegroundGroup) return 0;
    const bool Ignored = Task->SignalActions[Signal - 1][0] == SigIgnore || (Current->SignalMask & SignalBit(Signal));
    if (Ignored) return Signal == SigTtin ? -5 : 0;
    if (IsOrphanedGroup(Task->Group)) return -5;
    SignalGroup(Task->Group, Signal);
    return WaitForIO();
  }

  void FeedTerminalByte(char Byte) {
    ++IOEpoch;
    const auto& Attributes = Terminal.Attributes;
    if (const auto Signal = TerminalSignalFor(static_cast<uint8_t>(Byte), Attributes.Characters.data(), Attributes.Local)) {
      // n_tty isig: flush unless NOFLSH, echo ^X with ECHO|ECHOCTL, signal
      // the foreground group.
      constexpr uint32_t NoFlush = 0x80, Echo = 0x8, EchoControl = 0x200;
      if (!(Attributes.Local & NoFlush)) Terminal.Clear();
      if ((Attributes.Local & (Echo | EchoControl)) == (Echo | EchoControl)) {
        const char Text[] {'^', static_cast<char>(Byte ^ 0x40)};
        std::fwrite(Text, 1, sizeof(Text), stdout);
        consoleUpdate(nullptr);
      }
      if (ForegroundGroup) SignalGroup(ForegroundGroup, Signal);
      return;
    }
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
        if (!Fifo.Readers) Ready |= 8 | (Events & 4); // pipe_poll: POLLERR, and the buffer takes writes
        else if (Fifo.Writer->WriteSpace(false)) Ready |= Events & 4;
      }
      return Ready;
    }
    const bool WriteEnd = (File.StatusFlags & 3) == 1;
    // pipe_poll: no readers is POLLERR; the buffer still takes writes.
    if (WriteEnd && File.Socket->Peer.expired()) return 8 | (Events & 4);
    if (!WriteEnd && File.Socket->Peer.expired()) {
      return 0x10 | (File.Socket->Input.empty() ? 0 : Events & 1);
    }
    return File.Socket->Poll(Events & (WriteEnd ? 4 : 1));
  }

  uint16_t DescriptorEvents(const OpenFile& File) const {
    if (File.Socket) return StreamPoll(File, 7);
    if (File.Inet) return static_cast<uint16_t>(InetReadiness(*File.Inet, 7));
    if (File.Mqueue) return MqueueEvents(File);
    if (File.AnonName) return AnonymousEvents(File);
    if (File.PidfdTarget) {
      // pidfd_poll: readable once the process exited, hung up once reaped.
      const auto It = Processes.find(File.PidfdTarget);
      if (It == Processes.end() || It->second.Reaped) return 1 | 0x10;
      return It->second.Exited ? 1 : 0;
    }
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
    // Only stream sockets can be read through a splice: a record socket has no byte stream to peek and consume.
    if (Input->Socket && !Input->Pipe && Input->Socket->Kind != Switch2Guest::SocketKind::Stream) return Invalid;
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
    // A store through a shared mapping must not be lost to this write.
    if (!SharedFileViews.empty() && File.RealFile && HasWritableView(File.Inode)) FlushSharedFileViews(File.Inode);
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
          return SdFileError(SdWriteGuest(&Native, Run.BackingOffset, Buffer, Count, WriteOption));
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
        if (const auto Error = SdFileError(SdRetry(fsFileGetSize, &Native, &Size)); Error) return Error;
        const auto Logical = LogicalSize(File.Inode);
        const auto Visible = Logical.value_or(Size);
        const auto Written = WriteGuestFileRange(Size, Visible, Offset, {static_cast<const char*>(Buffer), Count},
          [&](uint64_t Begin, uint64_t End) { return ZeroDiskRange(Native, Begin, End); },
          [&](uint64_t Position, std::span<const char> Chunk) {
            return SdFileError(SdWriteGuest(&Native, Position, Chunk.data(), Chunk.size(), WriteOption));
          },
          [&](uint64_t Length) {
            if (R_FAILED(SdRetry(fsFileSetSize, &Native, Length))) {
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
  static constexpr size_t VectorStreamWriteLimit = 1 << 20;

  static bool IsGuestUserRange(uint64_t Address, uint64_t Count) {
    return Address < GuestUserLimit && Count <= GuestUserLimit - Address;
  }

  int64_t WriteGuest(uint64_t FD, uint64_t Address, uint64_t Count, std::optional<uint64_t> Offset = std::nullopt) {
    if (Offset && *Offset > INT64_MAX) return Invalid;
    auto* File = GetFile(FD);
    if (!File || File->PathOnly()) return BadFile;
    if (Offset && (File->Socket || File->Inet || File->Console != ConsoleStream::None || File->AnonName)) return -29;
    if (!WriteAccess(File->StatusFlags)) return BadFile;
    if (!IsGuestUserRange(Address, Count)) return Fault;
    if (File->ProcWrite) return Offset ? -29 : WriteProcNamespaceFile(*File, Address, Count);
    if (File->AnonName) {
      Count = std::min<uint64_t>(Count, 65536);
      if (Count && AccessiblePrefix(Address, Count, PF_R) != Count) return Fault;
      return WriteAnonymous(*File, reinterpret_cast<const char*>(Address), Count);
    }
    const auto Position = Offset.value_or(File->Offset);
    if (!File->Socket && !File->Inet && Count > INT64_MAX - Position) return Invalid;
    if (IsDataDev(File->Path)) return WriteCharDev(CharDevAt(File->Path), std::min(Count, MaxRWCount));
    if (!Count) return 0;
    const bool FileBacked = File->RealFile || File->Memory;
    if (!FileBacked && !File->Socket && !File->Inet && File->Console == ConsoleStream::None) return BadFile;
    Count = std::min<uint64_t>(Count, FileBacked || File->Socket || File->Inet ? MaxRWCount : 4096);
    const auto Accessible = AccessiblePrefix(Address, Count, PF_R);
    if (!Accessible || (Accessible != Count && !FileBacked)) return Fault;
    const auto* Buffer = reinterpret_cast<const char*>(Address);
    constexpr uint32_t TerminalStop = 0x100; // TOSTOP
    if (Interactive && File->Console != ConsoleStream::None && (Terminal.Attributes.Local & TerminalStop)) {
      if (const auto Access = TerminalAccess(SigTtou)) return Access;
    }
    if (!Offset) return WriteDescriptor(*File, Buffer, Accessible);
    return WriteFileAt(*File, Buffer, Accessible, Position);
  }

  int64_t WriteDescriptor(OpenFile& Descriptor, const char* Buffer, uint64_t Count, AppendMode Append = AppendMode::Descriptor, bool Synchronous = false) {
    auto* File = &Descriptor;
    if (IsDataDev(File->Path)) return WriteCharDev(CharDevAt(File->Path), Count);
    if (File->Mqueue) return Invalid; // mqueue files have no write operation
    if (File->Inet) return WriteInet(*File, Buffer, Count);
    if (File->Socket) {
      return WriteSocket(File->WriteEndpoint(), Buffer, Count, File->Nonblocking(), File->Pipe, true, File->Pipe && (File->StatusFlags & DirectFlag));
    }
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
    // Detailed and Trace logs keep the program output, so a console run's
    // results (such as the suites' TAP lines) reach the downloaded log.
    if (File->Console == ConsoleStream::Error || Logging != LogLevel::Standard) {
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
    if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
    std::string Path;
    auto Error = ResolveSdPath(Directory, Name, SdCard, Path, !(Flags & NoFollow), Resolve);
    FsDirEntryType Type {};
    if (!Error) Error = MountDenies(LastMount, true);
    if (!Error && VirtualMount(Path)) {
      const auto GuestPath = std::string_view(Path).substr(RootPath.size());
      const auto VirtualDir = VirtualDirectory(Path);
      if (VirtualDir == "/proc") Error = Task->Creds.Capable(Switch2Guest::CapDacOverride) ? -95 : -13;
      else if (VirtualDir.starts_with("/proc/")) Error = -1;
      else if (VirtualDir == "/dev" || VirtualDir == ShmDirectory) Error = -95;
      else if (!ProcLink(Path).empty() || ProcFile(GuestPath) || CharDevAt(GuestPath) != CharDev::None) Error = -20;
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
      const auto Created = SdFileError(SdRetry(fsFsCreateDirectory, &SdCard, InodeObjects.data()));
      if (Created && Created != -17) Error = Created;
    }

    std::shared_ptr<OpenFile> File;
    if (!Error) {
      const auto Inode = NextSdInode++;
      auto& Record = Inodes[Inode];
      Record.ManagedFile = Record.Unlinked = true;
      const auto ParentInode = SdInode(Path);
      Record.Owner.emplace(Task->Creds.Uid[3], (ModeForInode(ParentInode, 0040755) & 02000) ?
                           InodeOwner(ParentInode).second : Task->Creds.Gid[3]);
      InitializeInodeMode(Inode, Path, CreationMode, false);
      const auto Now = WallClock();
      Record.Times = InodeTimestamps {Now, Now, Now};
      InodeStoreDirty = true;
      // Record retirement first so a reset cannot strand an unnamed file.
      Error = SaveInodeStore();
      if (!Error) Error = SdFileError(SdRetry(fsFsCommit, &SdCard));
      if (!Error) {
        const auto Object = ObjectPath(Inode);
        Error = SdFileError(SdRetry(fsFsCreateFile, &SdCard, Object.c_str(), 0, 0));
        if (!Error) {
          File = std::make_shared<OpenFile>();
          const uint32_t Mode = FsOpenMode_Write | FsOpenMode_Append | ((Flags & 3) == 2 ? FsOpenMode_Read : 0);
          FsFile Native {};
          Error = SdFileError(SdRetry(fsFsOpenFile, &SdCard, Object.c_str(), Mode, &Native));
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
      if (const auto Status = SdRetry(fsOpenSdCardFileSystem, &SdCard); R_FAILED(Status)) return SdFileError(Status);
      if (const auto Error = ResolveSdPath(Directory, *Name, SdCard, File->Path, !(Flags & NoFollow) && !((Flags & Create) && (Flags & Exclusive)), Resolve); Error < 0) {
        fsFsClose(&SdCard);
        return Error;
      }
      File->Mount = LastMount;
      const auto OpenParentMount = LastParentMount;
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
      if (const auto Shm = ShmEntryName(Path)) {
        fsFsClose(&SdCard);
        const auto Error = OpenShmFile(*File, *Shm, Flags, CreationMode, Name->ends_with('/'));
        if (Error) return Error;
        Descriptors().Files[FD] = std::move(File);
        Descriptors().Flags[FD] = (Flags & CloseOnExec) != 0;
        return FD;
      }
      if (VirtualMount(Path)) {
        const auto GuestPath = std::string_view(Path).substr(RootPath.size());
        if (const auto Link = ProcNs(GuestPath)) {
          // An nsfs file: read-only, for setns and the NS_GET_* ioctls. The process must be inspectable.
          fsFsClose(&SdCard);
          if ((Flags & DirectoryOnly) || Name->ends_with('/')) return -20;
          if ((Flags & Create) && (Flags & Exclusive)) return -17;
          if (!PathOnly && (Flags & 3)) return -13;
          if (!MayPtrace(*Link->Owner)) return -13;
          File->NsObject = Link->Object;
          File->Inode = Link->Object->Ino;
          File->Path = GuestPath;
          Descriptors().Files[FD] = std::move(File);
          Descriptors().Flags[FD] = (Flags & CloseOnExec) != 0;
          return FD;
        }
        const auto Contents = ProcFile(GuestPath);
        const auto Kind = CharDevAt(GuestPath);
        const bool Device = Kind != CharDev::None;
        const bool Tty = Kind == CharDev::Tty && Interactive;
        if (Contents || Device) {
          fsFsClose(&SdCard);
          if ((Flags & DirectoryOnly) || Name->ends_with('/')) return -20;
          // proc_mountinfo_open and proc_mounts_open: another user's mount table needs ptrace_may_access (read)
          if (Contents && !PathOnly) {
            if (const auto Entry = ProcOf(GuestPath); Entry && (Entry->Rest == "mountinfo" || Entry->Rest == "mounts") && !MayPtrace(*Entry->Process)) return -13;
          }
          // may_open_dev: a device node on a nodev mount cannot be opened
          if (Device && !PathOnly && File->Mount && (File->Mount->Flags & Switch2Guest::Mounts::MntNodev)) return -13;
          // tty_open: without a controlling terminal the open fails
          if (Kind == CharDev::Tty && !Tty && !PathOnly) return -6;
          if ((Flags & Create) && (Flags & Exclusive)) return -17;
          if (!PathOnly && Contents && (Flags & 3)) {
            const auto Writer = OpenProcWriter(GuestPath);
            if (!Writer) return -13;
            File->ProcWrite = *Writer;
          }
          File->Inode = VirtualInode(GuestPath);
          if ((Flags & NoAtimeFlag) && !OwnsInodeOrRoot(File->Inode)) return -1;
          File->Contents = Contents.value_or(std::string {});
          if (Tty) File->Console = ConsoleStream::Terminal;
          File->LinkPath = Path;
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
          if (const auto Denied = MountDenies(OpenParentMount, true)) {
            fsFsClose(&SdCard);
            return Denied;
          }
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
      if (!Created && !PathOnly && Type == FsDirEntryType_File && (WriteAccess(Flags) || (Flags & Truncate))) {
        // get_write_access: a regular file cannot be opened for writing below a read-only mount or file system
        if (const auto Denied = MountDenies(File->Mount, false)) {
          fsFsClose(&SdCard);
          return Denied;
        }
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
        Status = SdRetry(fsFsOpenFile, &SdCard, Backing.c_str(), Mode, &Native);
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
    if (Offset && (File->Socket || File->Inet || File->Console != ConsoleStream::None)) return -29;
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
    if (!File->Socket && !File->Inet && RequestedTotal > INT64_MAX - Offset.value_or(File->Offset)) return Invalid;
    const auto Supported = Hipri | Sync | Append | NoAppend | VectorNoSignal |
                           ((File->Socket || File->Inet || IsDataDev(File->Path)) ? Nowait : 0);
    if (Flags & ~Supported) return -95;
    if ((Flags & Append) && (Flags & NoAppend)) return Invalid;
    const auto AppendBehavior = Flags & Append ? AppendMode::Force :
                                Flags & NoAppend ? AppendMode::Suppress : AppendMode::Descriptor;
    if (File->Directory) return Writing ? BadFile : -21;
    if (const auto Kind = CharDevAt(File->Path); Writing && IsDataDev(File->Path)) return WriteCharDev(Kind, RequestedTotal);
    else if (Kind == CharDev::Null) return 0;
    const size_t Limit = File->Console != ConsoleStream::None ? (Writing ? 4096 : 65536) :
                         // Vectored stream writes are gathered into one copy per
                         // attempt, so they are bounded to keep that copy small.
                         (File->Socket ? (Writing ? VectorStreamWriteLimit : File->Socket->InputCapacity) :
                          File->Inet ? (Writing ? VectorStreamWriteLimit : 65536) : IsDataDev(File->Path) ? 65536 : MaxRWCount);
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
      if (!File->RealFile && !File->Memory && !File->Socket && !File->Inet &&
          File->Console == ConsoleStream::None) return BadFile;
      if (BufferFault && File->Pipe) return Fault;
      size_t Copied = 0;
      for (const auto& Vector : Vectors) {
        if (!Vector.Size) break;
        std::memcpy(Buffer.get() + Copied, reinterpret_cast<const void*>(Vector.Base), Vector.Size);
        Copied += Vector.Size;
      }
      int64_t Written;
      if (File->Inet) {
        Written = WriteInet(*File, Buffer.get(), Total, (Flags & Nowait) ? MsgDontWait : 0);
      } else if (File->Socket) {
        Written = WriteSocket(File->WriteEndpoint(), Buffer.get(), Total, (Flags & Nowait) || File->Nonblocking(), File->Pipe, true,
                              File->Pipe && (File->StatusFlags & DirectFlag));
      } else if (!Offset) {
        Written = WriteDescriptor(*File, Buffer.get(), Total, AppendBehavior, Flags & Sync);
      } else {
        Written = WriteFileAt(*File, Buffer.get(), Total, *Offset, AppendBehavior, Flags & Sync);
      }
      return Written;
    }
    const auto Read = File->Inet && (Flags & Nowait) ? ReadInet(*File, Buffer.get(), Total, MsgDontWait) :
                      PipeTransfer || (File->Socket && (Flags & Nowait)) ?
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
    if (Offset && (File->Socket || File->Inet || File->Console != ConsoleStream::None || File->AnonName)) return -29;
    if (!ReadAccess(File->StatusFlags)) return BadFile;
    if (!IsGuestUserRange(Address, Count)) return Fault;
    if (File->AnonName) {
      // Anonymous descriptors check the size themselves; a zero count is not special.
      Count = std::min<uint64_t>(Count, 65536);
      if (Count && AccessiblePrefix(Address, Count, PF_W) != Count) return Fault;
      return ReadAnonymous(*File, reinterpret_cast<char*>(Address), Count);
    }
    const auto Position = Offset.value_or(File->Offset);
    if (!File->Socket && !File->Inet && Count > INT64_MAX - Position) return Invalid;
    if (File->Path == "/dev/null") return 0;
    if (File->Directory) return -21;
    if (!Count) return 0;
    const bool Device = IsDataDev(File->Path);
    Count = std::min<uint64_t>(Count, File->RealFile || File->Memory || Device ? MaxRWCount : 65536);
    const auto Accessible = AccessiblePrefix(Address, Count, PF_W);
    if (!Accessible) return ReadBufferFault(*File, Position);
    if (Accessible != Count && !File->RealFile && !File->Memory && !Device) return Fault; // a device read stops at the fault
    auto* Buffer = reinterpret_cast<char*>(Address);
    return Offset ? ReadFileAt(*File, Buffer, Accessible, Position) : ReadDescriptor(*File, Buffer, Accessible);
  }

  int64_t ReadDescriptor(OpenFile& Descriptor, char* Buffer, uint64_t Count) {
    auto* File = &Descriptor;
    if (IsDataDev(File->Path)) return ReadCharDev(CharDevAt(File->Path), Buffer, Count);
    if (Interactive && (File->Console == ConsoleStream::Input || File->Console == ConsoleStream::Terminal)) {
      if (!ReadAccess(File->StatusFlags)) return BadFile;
      if (const auto Access = TerminalAccess(SigTtin)) return Access;
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
    if (File->Inet) return ReadInet(*File, Buffer, Count);
    if (File->Mqueue) {
      const auto Bytes = ReadMqueue(*File, Buffer, Count, File->Offset);
      if (Bytes > 0) File->Offset += Bytes;
      return Bytes;
    }
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
            const auto Error = SdFileError(SdReadGuest(&Native, Position, Chunk.data(), Chunk.size(), &Received));
            return Error ? Error : static_cast<int64_t>(Received);
          });
        if (Result < 0) return Result;
        Read = Result;
        return int64_t {0};
      }
      uint64_t PhysicalCount = Count;
      if (Logical) {
        s64 Allocated {};
        if (const auto Error = SdFileError(SdRetry(fsFileGetSize, &Native, &Allocated)); Error) return Error;
        PhysicalCount = Offset >= static_cast<uint64_t>(Allocated) ? 0 :
          std::min<uint64_t>(Count, static_cast<uint64_t>(Allocated) - Offset);
      }
      auto* Data = static_cast<char*>(Buffer);
      while (Read < PhysicalCount) {
        const auto Chunk = std::min<uint64_t>(PhysicalCount - Read, 65536);
        u64 Received {};
        const auto Status = SdReadGuest(&Native, Offset + Read, Data + Read, Chunk, &Received);
        if (R_FAILED(Status) && !Read && !Logical) {
          // Horizon rejects reads that start past EOF; Linux reads 0 bytes.
          s64 Size {};
          if (R_SUCCEEDED(SdRetry(fsFileGetSize, &Native, &Size)) && Offset >= static_cast<uint64_t>(Size)) return int64_t {0};
        }
        const auto Error = SdFileError(Status);
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
    if (IsDataDev(File.Path)) return ReadCharDev(CharDevAt(File.Path), Buffer, Count);
    if (File.Socket || File.Inet || File.Directory || File.Console != ConsoleStream::None) return -29;
    // Stores made through a shared mapping must be visible to the read.
    if (!SharedFileViews.empty() && File.RealFile && HasWritableView(File.Inode)) FlushSharedFileViews(File.Inode);
    if (File.Memory) {
      if (Offset >= File.Memory->Size) return 0;
      const auto Read = std::min<size_t>(Count, File.Memory->Size - Offset);
      File.Memory->Read(Offset, Buffer, Read);
      if (Read && !(File.StatusFlags & NoAtimeFlag)) MarkInodeAccessed(File.Inode);
      return Read;
    }
    if (!File.RealFile) {
      if (Offset >= File.Contents.size()) return 0;
      const auto Data = std::string_view(File.Contents).substr(Offset);
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
    if (!SharedFileViews.empty()) PruneSharedFileViews();
    const auto& File = Descriptors().Files[FD];
    const bool Socket = File->Socket != nullptr || File->Inet != nullptr;
    const bool Locked = File->Lock != FileLock::None;
    const auto Inode = File->Inode;
    // mqueue_flush_file: any close by the registered process drops its mq_notify registration.
    if (File->Mqueue && File->Mqueue->NotifyOwner == Task->PID) File->Mqueue->NotifyOwner = 0;
    Descriptors().Files[FD].reset();
    Descriptors().Flags[FD] = 0;
    if (Locked) {
      const auto Holders = FlockHolders.find(Inode);
      if (Holders != FlockHolders.end()) {
        std::erase_if(Holders->second, [](const auto& Weak) { return Weak.expired(); });
        if (Holders->second.empty()) FlockHolders.erase(Holders);
      }
    }
    if (Socket) {
      std::erase_if(PathSockets, [](const auto& Entry) { return Entry.second.expired(); });
      std::erase_if(AbstractSockets, [](const auto& Entry) { return Entry.second.expired(); });
    }
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

  // Drops the translations of executable pages that change or are rewritten.
  // Owner is the process whose memory changed (the current one by default).
  void InvalidateExecutableRange(FEXCore::Core::CpuStateFrame* Frame, uint64_t Start, size_t Size, uint8_t NewFlags,
                                 const GuestProcess* Owner = nullptr) {
    if (!Owner) Owner = Task;
    bool Invalidate = NewFlags & PF_X;
    for (auto Address = Start; !Invalidate && Address - Start < Size;) {
      const auto Run = Owner->Memory.Query(Address);
      Invalidate = Run.Flags & PF_X;
      Address = Run.Size ? Run.Start + Run.Size : (Address & ~(GuestPageSize - 1)) + GuestPageSize;
    }
    if (!Invalidate) return;
    // Each process has its own FEX context and code buffers.
    auto* Context = Owner->Context ? Owner->Context.get() : Frame->Thread->CTX;
    std::unique_lock Lock {Context->GetCodeInvalidationMutex()};
    Context->InvalidateCodeBuffersCodeRange(Start, Size);
    for (const auto& Guest : Threads) {
      if (Guest.Core && Guest.PID == Owner->PID) Context->InvalidateThreadCachedCodeRange(Guest.Core, Start, Size);
    }
  }

  // A runtime store into guest memory (read, recv, process_vm_writev) may land
  // on translated code. Direct guest stores into RWX pages are not detected.
  void InvalidateExecutableStores(uint64_t Start, size_t Size) {
    if (Current && Current->Core && Size) InvalidateExecutableRange(Current->Core->CurrentFrame, Start, Size, 0);
  }

  // Stores a read-like syscall made through a guest iovec array.
  void InvalidateExecutableIovecs(uint64_t Array, uint64_t Count, uint64_t Bytes) {
    for (uint64_t Index = 0; Index < std::min<uint64_t>(Count, 1024) && Bytes; ++Index) {
      uint64_t Entry[2] {};
      if (Available(Array + Index * 16, PF_R) < 16) return;
      std::memcpy(Entry, reinterpret_cast<const void*>(Array + Index * 16), 16);
      const auto Part = std::min<uint64_t>(Entry[1], Bytes);
      InvalidateExecutableStores(Entry[0], Part);
      Bytes -= Part;
    }
  }

  // The pages below the current break. The reservation above the break is
  // unmapped address space, as on Linux, and takes ordinary fixed mappings.
  bool OverlapsHeap(uint64_t Address, size_t Size) const {
    const auto End = Task->HeapStart + Task->Memory.RoundUp(Task->HeapEnd - Task->HeapStart);
    return Task->HeapStart && Address < End && Task->HeapStart < Address + Size;
  }

  // Whether the range touches the brk reservation (which is private memory).
  bool OverlapsHeapReservation(uint64_t Address, size_t Size) const {
    return Task->HeapStart && Address < Task->HeapStart + HeapLimit && Task->HeapStart < Address + Size;
  }

  // Arena memory for a shared file view: reserved and committed together.
  std::shared_ptr<void> AllocateArenaBlock(size_t Size) {
    auto* Arena = Task->Memory.GetArena();
    if (!Arena || Size > Task->Memory.MaxMapping()) return {};
    const auto Address = Arena->Reserve(Size);
    if (!Address) return {};
    if (!Arena->Commit(Address, Size)) {
      Arena->Release(Address, Size);
      return {};
    }
    return {reinterpret_cast<void*>(Address), [Arena, Address, Size](void*) { Arena->Release(Address, Size); }};
  }

  // can_do_mlock and the RLIMIT_MEMLOCK check for NewBytes more locked bytes.
  // Returns 0, -1 (EPERM, limit 0) or Over.
  int64_t LockCheck(size_t NewBytes, int64_t Over) const {
    if (Task->Creds.Capable(CapIpcLock)) return 0;
    const auto Limit = Task->Limits[ResourceMemlock][0];
    if (!Limit) return -1;
    return Task->Memory.LockedBytes() + NewBytes > Limit ? Over : 0;
  }

  // ---- Shared file views ---------------------------------------------------

  // Stores made through a writable view are found by comparing it with Snapshot, the bytes as
  // last read from or written to the file, because dirty pages cannot be
  // found without faults. Flush writes the changed pages through the normal
  // file write path, clipped at end of file.
  int64_t FlushSharedFileViews(uint64_t Inode, uint64_t Start = 0, uint64_t Length = UINT64_MAX) {
    if (SharedFileViews.empty() || ViewSyncDepth) return 0;
    int64_t Result {};
    bool Wrote {};
    std::vector<uint64_t> Touched;
    ++ViewSyncDepth;
    for (auto& [Key, View] : SharedFileViews) {
      if ((Inode && Key != Inode) || !View.MayWrite) continue;
      const auto Storage = View.Storage.lock();
      if (!Storage) continue;
      const auto Base = reinterpret_cast<uint64_t>(Storage.get());
      if (Base >= Start + Length && Start + Length >= Start) continue;
      if (Base + View.Size <= Start) continue;
      auto* Bytes = static_cast<uint8_t*>(Storage.get());
      const auto First = Start > Base ? (Start - Base) / GuestPageSize : 0;
      const auto Limit = Start + Length < Start || Start + Length - Base >= View.Size ? View.Size / GuestPageSize
                                                                                       : (Start + Length - Base + GuestPageSize - 1) / GuestPageSize;
      s64 FileSize {};
      if (const auto Error = DiskSize(*View.File, FileSize); Error) {
        Result = Result ? Result : Error;
        continue;
      }
      for (auto Page = First; Page < Limit;) {
        if (!std::memcmp(Bytes + Page * GuestPageSize, View.Snapshot.data() + Page * GuestPageSize, GuestPageSize)) {
          ++Page;
          continue;
        }
        const auto Run = Page;
        while (Page < Limit && std::memcmp(Bytes + Page * GuestPageSize, View.Snapshot.data() + Page * GuestPageSize, GuestPageSize)) ++Page;
        const auto FileOffset = View.Offset + Run * GuestPageSize;
        if (FileOffset < static_cast<uint64_t>(FileSize)) {
          const auto Count = std::min<uint64_t>((Page - Run) * GuestPageSize, FileSize - FileOffset);
          const auto Written = WriteFileAt(*View.File, Bytes + Run * GuestPageSize, Count, FileOffset, AppendMode::Suppress);
          if (Written < 0) {
            Result = Result ? Result : Written;
            continue;
          }
          Wrote = Wrote || Written > 0;
          if (static_cast<uint64_t>(Written) < Count) {
            // Short write: only whole pages written count as clean; the
            // rest stays dirty for the next flush, and the caller sees EIO.
            const auto Clean = static_cast<uint64_t>(Written) / GuestPageSize;
            std::memcpy(View.Snapshot.data() + Run * GuestPageSize, Bytes + Run * GuestPageSize, Clean * GuestPageSize);
            Result = Result ? Result : -5;
            continue;
          }
        }
        std::memcpy(View.Snapshot.data() + Run * GuestPageSize, Bytes + Run * GuestPageSize, (Page - Run) * GuestPageSize);
      }
      Touched.push_back(Key);
    }
    --ViewSyncDepth;
    if (Wrote) {
      for (const auto Key : Touched) RefreshSharedFileViews(Key);
    }
    return Result;
  }

  // Writes back every writable view the process maps (exit and exec).
  void FlushViewsMappedBy(const GuestAddressSpace& Memory) {
    if (SharedFileViews.empty()) return;
    std::set<uint64_t> Inodes;
    for (const auto& [Inode, View] : SharedFileViews) {
      const auto Storage = View.Storage.lock();
      if (View.MayWrite && Storage && Memory.Maps(Storage)) Inodes.insert(Inode);
    }
    for (const auto Inode : Inodes) FlushSharedFileViews(Inode);
  }

  bool HasWritableView(uint64_t Inode) const {
    const auto [First, Last] = SharedFileViews.equal_range(Inode);
    return std::any_of(First, Last, [](const auto& Entry) { return Entry.second.MayWrite; });
  }

  // ---- mmap, mprotect, munmap, brk ------------------------------------------

  int64_t MapGuest(FEXCore::Core::CpuStateFrame* Frame, uint64_t Address, uint64_t Size, uint64_t Protection, uint64_t Flags,
                   uint64_t FD, uint64_t Offset) {
    constexpr uint64_t Shared = 1, Private = 2, TypeMask = 3, SharedValidate = 3, Fixed = 0x10, Anonymous = 0x20, Bits32 = 0x40,
                       GrowsDown = 0x100, DenyWrite = 0x800, Executable = 0x1000, LockedMap = 0x2000, NoReserve = 0x4000,
                       Populate = 0x8000, NonBlock = 0x10000, Stack = 0x20000, HugeTlb = 0x40000, SyncMap = 0x80000,
                       FixedNoReplace = 0x100000, Uninitialized = 0x4000000;
    constexpr uint64_t Known = TypeMask | Fixed | Anonymous | Bits32 | GrowsDown | DenyWrite | Executable | LockedMap | NoReserve |
                               Populate | NonBlock | Stack | HugeTlb | SyncMap | FixedNoReplace | Uninitialized;
    if (!Size || Offset % GuestPageSize || (Protection & ~15ULL)) return Invalid;
    if (Size > UINT64_MAX - GuestPageSize) return -12;
    const bool IsFixed = Flags & (Fixed | FixedNoReplace);
    const bool IsAnonymous = Flags & Anonymous;
    const uint64_t Type = Flags & TypeMask;
    if (Type == 0) return Invalid;
    // File mappings only: an anonymous MAP_SHARED_VALIDATE is EINVAL below.
    if (Type == SharedValidate && !IsAnonymous && (Flags & ~Known)) return NotSupported;
    if (Type == SharedValidate && !IsAnonymous && (Flags & SyncMap)) return NotSupported;
    const bool IsShared = Type == Shared || Type == SharedValidate;
    (void)Private;
    if (IsFixed && Address % GuestPageSize) return Invalid;
    // No hugetlb pool, as Linux with nr_hugepages=0; arena addresses are above 4 GiB.
    if (Flags & (HugeTlb | Bits32)) return -12;
    const auto Rounded = Task->Memory.RoundUp(Size);
    const auto Access = PageFlags(Protection);
    const auto* Arena = Task->Memory.GetArena();
    if (!Arena || Rounded > Arena->VirtualSize() || (Access && Rounded > Task->Memory.MaxMapping())) return -12;
    auto* File = IsAnonymous ? nullptr : GetFile(FD);
    if (!IsAnonymous && (!File || File->PathOnly())) return BadFile;
    // do_mmap: a file on a noexec mount cannot be mapped executable.
    if (File && (Protection & 4) && File->Mount && (File->Mount->Flags & Switch2Guest::Mounts::MntNoexec)) return -1;
    if (File && Offset + Rounded < Offset) return -75;
    if (IsFixed && OverlapsHeap(Address, Rounded)) return Invalid;
    // The brk reservation is private: a shared mapping cannot be placed in it.
    if (IsFixed && IsShared && OverlapsHeapReservation(Address, Rounded)) return -12;
    if (IsFixed && Task->Memory.AnyHave(Address, Rounded, GuestAddressSpace::Sealed)) return -1;
    if ((Flags & FixedNoReplace) && Task->Memory.AnyMapped(Address, Rounded)) return -17;
    const bool WantLock = (Flags & LockedMap) || Task->LockFuture;
    if (WantLock) {
      // MAP_LOCKED fails can_do_mlock (EPERM) with a zero limit; a mapping
      // locked only by MCL_FUTURE fails mlock_future_ok (EAGAIN). Verified natively.
      auto Error = LockCheck(Rounded, -11);
      if (Error == -1 && !(Flags & LockedMap)) Error = -11;
      if (Error) return Error;
    }
    // Linux 7.3 has no anonymous MAP_SHARED_VALIDATE (EINVAL, verified natively).
    if (Type == SharedValidate && IsAnonymous) return Invalid;
    const auto Lock = [&](uint64_t Start) {
      if (WantLock) Task->Memory.ChangeAttribute(Start, Rounded, GuestAddressSpace::Locked, true);
    };

    if (!IsAnonymous && IsShared && !File->Memory) {
      // A shared view of an SD file: filled from the file, written back at
      // msync, munmap, fsync, exit and exec (dirty pages cannot be found).
      if (!File->RealFile) return -19;
      const auto Mode = File->StatusFlags & 3;
      if (Mode == 1 || ((Protection & 2) && Mode != 2)) return -13;
      if (IsFixed || (Protection & 4)) return Invalid;
      const bool MayWrite = Mode == 2;
      if (MayWrite || HasWritableView(File->Inode)) {
        const auto [First, Last] = SharedFileViews.equal_range(File->Inode);
        for (auto It = First; It != Last; ++It) {
          const auto Storage = It->second.Storage.lock();
          if ((MayWrite || It->second.MayWrite) && Storage && Task->Memory.Maps(Storage) &&
              It->second.Offset < Offset + Rounded && Offset < It->second.Offset + It->second.Size) {
            return -12; // one view per file range in a process
          }
        }
      }
      auto Storage = AllocateArenaBlock(Rounded);
      if (!Storage) return -12;
      if (const auto Error = FillSharedFileView(*File, Storage.get(), Rounded, Offset); Error) return Error;
      const auto Base = reinterpret_cast<uint64_t>(Storage.get());
      std::vector<uint8_t> Snapshot;
      if (MayWrite) Snapshot.assign(static_cast<uint8_t*>(Storage.get()), static_cast<uint8_t*>(Storage.get()) + Rounded);
      if (!Task->Memory.RegisterSharedFile(Base, Rounded, Access, Storage, MayWrite)) return -12;
      PruneSharedFileViews();
      SharedFileViews.emplace(File->Inode, SharedFileView {Storage, Descriptors().Files[FD], Offset, Rounded, MayWrite, std::move(Snapshot)});
      Lock(Base);
      RecordMapping(Base, Rounded, Offset, File->Path);
      return Base;
    }
    if (!IsAnonymous && IsShared) {
      auto& Memory = *File->Memory;
      if (!ReadAccess(File->StatusFlags)) return -13;
      const bool MayWrite = (File->StatusFlags & 3) == 2 && !Memory.WritesSealed();
      if ((Access & PF_W) && !MayWrite) return Memory.WritesSealed() ? -1 : -13;
      if (IsFixed || (Protection & 4) || Offset >= Memory.Size ||
          Rounded > GuestAddressSpace::RoundUp(Memory.Size) - Offset) return Invalid;
      // Direct access cannot fault per page, so the mapped range is allocated now.
      if (Memory.MarkAllocated(Offset, Offset + Rounded)) return -12;
      const auto Base = Memory.Base() + Offset;
      if (!Task->Memory.RegisterSharedFile(Base, Rounded, Access, Memory.Storage, MayWrite)) return -12;
      MappedMemoryFiles[Memory.Storage.get()] = File->Memory;
      Lock(Base);
      return Base;
    }

    // Private file contents and anonymous memory live in the arena. A private
    // file mapping is filled while accessible, then given its protection.
    const auto Initial = File ? static_cast<uint8_t>(PF_R | PF_W) : Access;
    const auto Fill = [&](uint64_t Start) -> int64_t {
      if (!File) return 0;
      if (ReadFileAt(*File, reinterpret_cast<void*>(Start), Rounded, Offset) < 0) return -5;
      return 0;
    };
    if (!IsFixed) {
      Address = Task->Memory.Allocate(Rounded, Initial, IsShared);
      if (!Address) return -12;
      if (Fill(Address)) {
        Task->Memory.Set(Address, Rounded, Task->Memory.Unmapped, true);
        return -5;
      }
      if (Initial != Access) Task->Memory.Set(Address, Rounded, Access, true);
      Lock(Address);
      RecordMapping(Address, Rounded, Offset, File ? std::string_view(File->Path) : std::string_view {});
      return Address;
    }

    if (!Task->Memory.Owns(Address, Rounded, true)) {
      // Free arena address space can be claimed outright. Anything else (a
      // fixed address outside the arena) cannot be provided: deliberate difference.
      if (Task->Memory.AnyMapped(Address, Rounded) || !Task->Memory.AllocateAt(Address, Rounded, Initial, IsShared)) return -12;
      if (Fill(Address)) {
        Task->Memory.Set(Address, Rounded, Task->Memory.Unmapped, true);
        return -5;
      }
      if (Initial != Access) Task->Memory.Set(Address, Rounded, Access, true);
      Lock(Address);
      RecordMapping(Address, Rounded, Offset, File ? std::string_view(File->Path) : std::string_view {});
      return Address;
    }
    // Replacing part of a mapping of the other sharing kind would keep the old kind.
    if (Task->Memory.IsShared(Address, Rounded) != IsShared && Task->Memory.AnyMapped(Address, Rounded)) return -12;
    SuspendPipePages(*Task, Address, Rounded, true);
    InvalidateExecutableRange(Frame, Address, Rounded, Access);
    Task->Memory.Discard(Address, Rounded);
    Task->Memory.ChangeAttribute(Address, Rounded, GuestAddressSpace::DontFork | GuestAddressSpace::WipeOnFork | GuestAddressSpace::Locked, false);
    if (Task->Memory.SetPages(Address, Rounded, Initial, true) != GuestAddressSpace::SetStatus::Ok) {
      // Discard left the old protection on uncommitted pages; an access would
      // fault the CPU. Linux has already unmapped the old mapping here.
      Task->Memory.Set(Address, Rounded, Task->Memory.Unmapped, true);
      RecordMapping(Address, Rounded, 0, {});
      if (!SharedFileViews.empty()) PruneSharedFileViews();
      return -12;
    }
    if (IsShared) Task->Memory.Zero(Address, Rounded);
    if (Fill(Address)) return -5;
    if (Initial != Access) Task->Memory.Set(Address, Rounded, Access, true);
    Lock(Address);
    RecordMapping(Address, Rounded, Offset, File ? std::string_view(File->Path) : std::string_view {});
    return Address;
  }

  int64_t ProtectGuest(FEXCore::Core::CpuStateFrame* Frame, uint64_t Address, uint64_t Size, uint64_t Protection) {
    if ((Protection & ~15ULL) || Address % GuestPageSize) return Invalid;
    if (!Size) return 0;
    if (Size > UINT64_MAX - GuestPageSize || Address + Size < Address) return -12;
    const auto Rounded = Task->Memory.RoundUp(Size);
    if (!Task->Memory.IsMapped(Address, Rounded)) return -12;
    if (Task->Memory.AnyHave(Address, Rounded, GuestAddressSpace::Sealed)) return -1;
    const auto Access = PageFlags(Protection);
    if ((Access & PF_W) && !Task->Memory.AllowsFileMappingWrite(Address, Rounded)) return -13;
    InvalidateExecutableRange(Frame, Address, Rounded, Access);
    switch (Task->Memory.SetPages(Address, Rounded, Access)) {
    case GuestAddressSpace::SetStatus::Ok: return 0;
    case GuestAddressSpace::SetStatus::NoMemory: return -12;
    default: return Invalid;
    }
  }

  int64_t UnmapGuest(FEXCore::Core::CpuStateFrame* Frame, uint64_t Address, uint64_t Size) {
    if (Address % GuestPageSize || !Size || Size > GuestUserLimit || Address > GuestUserLimit) return Invalid;
    const auto Rounded = Task->Memory.RoundUp(Size);
    if (!Task->Memory.AnyMapped(Address, Rounded)) return 0; // unmapping nothing succeeds
    // Holes inside the range are skipped, as on Linux. Registered image pages
    // cannot be given back: EINVAL.
    const auto Runs = Task->Memory.MappedRuns(Address, Rounded);
    if (OverlapsHeap(Address, Rounded)) return Invalid;
    for (const auto& [Start, Length] : Runs) {
      if (!Task->Memory.Owns(Start, Length)) return Invalid;
    }
    if (Task->Memory.AnyHave(Address, Rounded, GuestAddressSpace::Sealed)) return -1;
    SuspendPipePages(*Task, Address, Rounded, true);
    InvalidateExecutableRange(Frame, Address, Rounded, 0);
    FlushSharedFileViews(0, Address, Rounded);
    for (const auto& [Start, Length] : Runs) {
      if (!Task->Memory.Set(Start, Length, Task->Memory.Unmapped, true)) return Invalid;
    }
    RecordMapping(Address, Rounded, 0, {});
    if (!SharedFileViews.empty()) PruneSharedFileViews();
    return 0;
  }

  // brk. The heap reserves HeapLimit of address space; pages above the break
  // are unmapped and are mapped (committed) as the break grows. RLIMIT_DATA
  // failures and growth into another mapping return the old break, as Linux does.
  int64_t SetHeapEnd(uint64_t Requested) {
    if (!Task->HeapStart) {
      Task->HeapStart = Task->Memory.ReserveUnmapped(HeapLimit);
      Task->HeapEnd = Task->HeapStart;
    }
    if (!Task->HeapStart || Requested < Task->HeapStart || Requested - Task->HeapStart > HeapLimit ||
        Requested - Task->HeapStart > Task->Limits[ResourceData][0]) return Task->HeapEnd;
    const size_t OldSize = Task->Memory.RoundUp(Task->HeapEnd - Task->HeapStart);
    const size_t NewSize = Task->Memory.RoundUp(Requested - Task->HeapStart);
    if (NewSize > OldSize) {
      // Linux keeps one page between the new break and the next mapping.
      const size_t Probe = std::min<size_t>(NewSize - OldSize + GuestPageSize, HeapLimit - OldSize);
      if (Task->Memory.AnyMapped(Task->HeapStart + OldSize, Probe)) return Task->HeapEnd;
      if (Task->Memory.SetPages(Task->HeapStart + OldSize, NewSize - OldSize, PF_R | PF_W, true) != GuestAddressSpace::SetStatus::Ok) {
        return Task->HeapEnd;
      }
      if (Task->LockFuture) Task->Memory.ChangeAttribute(Task->HeapStart + OldSize, NewSize - OldSize, GuestAddressSpace::Locked, true);
    } else if (NewSize < OldSize) {
      // The pages above the new break are unmapped and go back to the arena;
      // regrown memory reads zero.
      Task->Memory.Set(Task->HeapStart + NewSize, OldSize - NewSize, GuestAddressSpace::Unmapped, true);
    }
    Task->HeapEnd = Requested;
    return Task->HeapEnd;
  }

  // ---- mremap, msync, mincore, madvise ---------------------------------------

  int64_t RemapGuest(FEXCore::Core::CpuStateFrame* Frame, uint64_t OldAddress, uint64_t OldSize, uint64_t NewSize, uint64_t Flags,
                     uint64_t NewAddress) {
    constexpr uint64_t MayMove = 1, FixedMove = 2, DontUnmap = 4;
    if (OldAddress % GuestPageSize || (Flags & ~(MayMove | FixedMove | DontUnmap))) return Invalid;
    if ((Flags & FixedMove) && !(Flags & MayMove)) return Invalid;
    if ((Flags & DontUnmap) && (!(Flags & MayMove) || OldSize != NewSize)) return Invalid;
    if (!NewSize) return Invalid;
    if (NewSize > UINT64_MAX - GuestPageSize) return Invalid; // PAGE_ALIGN wraps to 0
    if (OldSize > UINT64_MAX - GuestPageSize) return -12;
    const auto OldRounded = Task->Memory.RoundUp(OldSize), NewRounded = Task->Memory.RoundUp(NewSize);
    constexpr uint64_t TaskSize = (1ull << 47) - 4096; // x86-64 TASK_SIZE_MAX; a longer mapping cannot exist: EINVAL
    if (NewRounded > TaskSize) return Invalid;
    if ((Flags & FixedMove) && NewAddress % GuestPageSize) return Invalid;
    if (!OldRounded) return Invalid; // duplicating a shared mapping needs two views
    if (OldAddress + OldRounded < OldAddress) return Invalid;
    if (!Task->Memory.IsMapped(OldAddress, OldRounded)) return Fault;
    if (Task->Memory.AnyHave(OldAddress, OldRounded, GuestAddressSpace::Sealed) ||
        ((Flags & FixedMove) && Task->Memory.AnyHave(NewAddress, NewRounded, GuestAddressSpace::Sealed))) return -1;
    if (OverlapsHeap(OldAddress, OldRounded) || ((Flags & FixedMove) && OverlapsHeap(NewAddress, NewRounded))) return Invalid;
    SuspendPipePages(*Task, OldAddress, OldRounded, true);
    InvalidateExecutableRange(Frame, OldAddress, OldRounded, 0);
    FlushSharedFileViews(0, OldAddress, OldRounded);
    uint64_t Result {};
    switch (Task->Memory.Remap(OldAddress, OldRounded, NewRounded, Flags & MayMove, Flags & DontUnmap,
                               (Flags & FixedMove) ? std::optional {NewAddress} : std::nullopt, Result)) {
    case GuestAddressSpace::RemapStatus::Ok: break;
    case GuestAddressSpace::RemapStatus::Fault: return Fault;
    case GuestAddressSpace::RemapStatus::Invalid: return Invalid;
    case GuestAddressSpace::RemapStatus::NoMemory: return -12;
    }
    InvalidateExecutableRange(Frame, Result, NewRounded, 0);
    RecordMapping(OldAddress, OldRounded, 0, {});
    RecordMapping(Result, NewRounded, 0, {});
    if (!SharedFileViews.empty()) PruneSharedFileViews();
    return Result;
  }

  int64_t SyncMemory(uint64_t Address, uint64_t Size, uint64_t Flags) {
    constexpr uint64_t Async = 1, Invalidate = 2, SyncNow = 4;
    if ((Flags & ~(Async | Invalidate | SyncNow)) || ((Flags & Async) && (Flags & SyncNow)) || Address % GuestPageSize) return Invalid;
    if (Size > UINT64_MAX - GuestPageSize || Address + Size < Address) return -12;
    if (!Size) return 0;
    const auto Rounded = Task->Memory.RoundUp(Size);
    if (!Task->Memory.IsMapped(Address, Rounded)) return -12;
    if ((Flags & Invalidate) && Task->Memory.AnyHave(Address, Rounded, GuestAddressSpace::Locked)) return -16;
    return FlushSharedFileViews(0, Address, Rounded);
  }

  int64_t MincoreMemory(uint64_t Address, uint64_t Size, uint64_t Vector) {
    if (Address % GuestPageSize) return Invalid;
    if (Size > UINT64_MAX - GuestPageSize || Address + Size < Address) return -12;
    if (!Size) return 0;
    const auto Rounded = Task->Memory.RoundUp(Size);
    if (!Task->Memory.IsMapped(Address, Rounded)) return -12;
    const auto Pages = Rounded / GuestPageSize;
    if (AccessiblePrefix(Vector, Pages, PF_W) < Pages) return Fault;
    auto* Out = reinterpret_cast<uint8_t*>(Vector);
    for (size_t Page = 0; Page < Pages; ++Page) Out[Page] = Task->Memory.PageCommitted(Address + Page * GuestPageSize) ? 1 : 0;
    return 0;
  }

  // The MADV_REMOVE target of one page range: a memfd or anonymous shared memory.
  int64_t RemoveMemory(uint64_t Address, uint64_t Size) {
    if (!Task->Memory.IsShared(Address, Size)) return Invalid;
    if (Task->Memory.AnyHave(Address, Size, GuestAddressSpace::Locked)) return Invalid;
    for (auto Page = Address; Page < Address + Size; Page += GuestPageSize) {
      const auto Storage = Task->Memory.StorageAt(Page);
      const auto Known = MappedMemoryFiles.find(Storage.get());
      if (Known != MappedMemoryFiles.end()) {
        const auto File = Known->second.lock();
        if (!File) return Invalid;
        if (!Task->Memory.RegionMayWrite(Page)) return -13;
        if (const auto Error = File->Punch(Page - File->Base(), Page - File->Base() + GuestPageSize); Error) return Error;
      } else if (Task->Memory.IsFileBacked(Page, GuestPageSize)) {
        return NotSupported; // an SD file: no hole punching through a mapping
      } else {
        Task->Memory.Zero(Page, GuestPageSize); // anonymous shared memory
      }
    }
    return 0;
  }

  int64_t AdviseGuest(uint64_t Address, uint64_t Size, uint64_t Advice) {
    constexpr uint64_t DontNeed = 4, Free = 8, Remove = 9, DontFork = 10, DoFork = 11, WipeOnFork = 18, KeepOnFork = 19,
                       PopulateRead = 22, PopulateWrite = 23;
    switch (Advice) {
    case 0: case 1: case 2: case 3: case DontNeed: case Free: case Remove: case DontFork: case DoFork: case 12: case 13:
    case 14: case 15: case 16: case 17: case WipeOnFork: case KeepOnFork: case 20: case 21: case PopulateRead: case PopulateWrite:
    case 25: break;
    default: return Invalid;
    }
    if (Address % GuestPageSize) return Invalid;
    if (Size > UINT64_MAX - GuestPageSize) return Invalid;
    const auto Rounded = Task->Memory.RoundUp(Size);
    if (Address + Rounded < Address) return Invalid;
    if (!Rounded) return 0;
    if (!Task->Memory.IsMapped(Address, Rounded)) return -12;
    const bool Private = Task->Memory.IsPrivateAnonymous(Address, Rounded);
    switch (Advice) {
    case DontNeed: case Free:
      // madvise_dontneed_free_valid_vma: both refuse VM_LOCKED (verified natively).
      if (Task->Memory.AnyHave(Address, Rounded, GuestAddressSpace::Locked)) return Invalid;
      if (Advice == Free && !Private) return Invalid;
      if (!Private) return 0; // shared and file mappings keep their contents
      if (Task->Memory.AnyHave(Address, Rounded, GuestAddressSpace::Sealed)) return -1;
      // Accessible pages cannot fault back in, so they are zeroed in place.
      // Inaccessible pages give their memory back to the arena.
      for (auto Run = Address; Run < Address + Rounded;) {
        const auto Range = Task->Memory.Query(Run);
        const auto End = std::min<uint64_t>(Address + Rounded, Range.Start + Range.Size);
        if (!Range.Flags) Task->Memory.Discard(Run, End - Run);
        else std::memset(reinterpret_cast<void*>(Run), 0, End - Run);
        Run = End;
      }
      return 0;
    case Remove: return RemoveMemory(Address, Rounded);
    case DontFork: Task->Memory.ChangeAttribute(Address, Rounded, GuestAddressSpace::DontFork, true); return 0;
    case DoFork: Task->Memory.ChangeAttribute(Address, Rounded, GuestAddressSpace::DontFork, false); return 0;
    case WipeOnFork:
      if (!Private) return Invalid;
      Task->Memory.ChangeAttribute(Address, Rounded, GuestAddressSpace::WipeOnFork, true);
      return 0;
    case KeepOnFork: Task->Memory.ChangeAttribute(Address, Rounded, GuestAddressSpace::WipeOnFork, false); return 0;
    case PopulateRead: case PopulateWrite: {
      const uint8_t Need = Advice == PopulateWrite ? PF_W : PF_R;
      for (auto Run = Address; Run < Address + Rounded;) {
        const auto Range = Task->Memory.Query(Run);
        if ((Range.Flags & Need) != Need) return Fault;
        Run = Range.Start + Range.Size;
      }
      return 0;
    }
    default: return 0; // hints: pages stay resident
    }
  }

  // ---- mlock family -----------------------------------------------------------

  int64_t LockMemory(uint64_t Address, uint64_t Size, bool Unlock) {
    Size += Address % GuestPageSize;
    Address &= ~(GuestPageSize - 1);
    if (Size > UINT64_MAX - GuestPageSize) return -12;
    const auto Rounded = Task->Memory.RoundUp(Size);
    if (!Unlock) {
      const auto Fresh = Rounded - Task->Memory.CountHave(Address, Rounded, GuestAddressSpace::Locked) * GuestPageSize;
      if (const auto Error = LockCheck(Rounded ? Fresh : 0, -12); Error) return Error;
    }
    if (!Rounded) return 0;
    if (Address + Rounded < Address || !Task->Memory.IsMapped(Address, Rounded)) return -12;
    Task->Memory.ChangeAttribute(Address, Rounded, GuestAddressSpace::Locked, !Unlock);
    return 0;
  }

  int64_t LockAll(uint64_t Flags) {
    constexpr uint64_t Current = 1, Future = 2, OnFault = 4;
    if ((Flags & ~(Current | Future | OnFault)) || !(Flags & (Current | Future))) return Invalid;
    if (!Task->Creds.Capable(CapIpcLock) && !Task->Limits[ResourceMemlock][0]) return -1;
    if (Flags & Current) {
      if (!Task->Creds.Capable(CapIpcLock) && Task->Memory.MappedBytes() > Task->Limits[ResourceMemlock][0]) return -12;
      Task->Memory.ChangeAttribute(0, UINT64_MAX / 2, GuestAddressSpace::Locked, true);
    }
    if (Flags & Future) Task->LockFuture = true;
    return 0;
  }

  int64_t UnlockAll() {
    Task->Memory.ChangeAttribute(0, UINT64_MAX / 2, GuestAddressSpace::Locked, false);
    Task->LockFuture = false;
    return 0;
  }

  // ---- process_vm_readv / process_vm_writev ----------------------------------

  // Reads the iovec array at Address (count already limited to 1024).
  int64_t ReadIovecs(uint64_t Address, uint64_t Count, std::vector<std::array<uint64_t, 2>>& Out) {
    if (!Count) return 0;
    if (AccessiblePrefix(Address, Count * 16, PF_R) < Count * 16) return Fault;
    Out.resize(Count);
    std::memcpy(Out.data(), reinterpret_cast<const void*>(Address), Count * 16);
    return 0;
  }

  // ptrace_may_access(PTRACE_MODE_ATTACH_REALCREDS): the caller's filesystem
  // identity matches every identity of the target, or it holds CAP_SYS_PTRACE.
  bool MayPtrace(const GuestProcess& Target) const {
    if (Target.PID == Task->PID) return true;
    if (NsCapableOver(*Target.Ns.User, CapSysPtrace)) return true;
    const auto Matches = [](uint32_t Id, const std::array<uint32_t, 4>& Ids) { return Id == Ids[0] && Id == Ids[1] && Id == Ids[2]; };
    return Matches(Task->Creds.Uid[3], Target.Creds.Uid) && Matches(Task->Creds.Gid[3], Target.Creds.Gid) && Target.Dumpable;
  }

  int64_t ProcessVm(bool Writing, uint64_t PID, uint64_t LocalAddress, uint64_t LocalCount, uint64_t RemoteAddress,
                    uint64_t RemoteCount, uint64_t Flags) {
    constexpr uint64_t MaxVectors = 1024;
    if (Flags) return Invalid;
    if (LocalCount > MaxVectors || RemoteCount > MaxVectors) return Invalid;
    std::vector<std::array<uint64_t, 2>> Local, Remote;
    if (const auto Error = ReadIovecs(LocalAddress, LocalCount, Local); Error) return Error;
    if (const auto Error = ReadIovecs(RemoteAddress, RemoteCount, Remote); Error) return Error;
    uint64_t Wanted {};
    for (const auto& Entry : Local) {
      if (Entry[1] > INT64_MAX || Wanted > INT64_MAX - Entry[1]) return Invalid;
      Wanted += Entry[1];
    }
    for (const auto& Entry : Remote) {
      if (Entry[1] > INT64_MAX) return Invalid;
    }
    if (!Wanted) return 0;
    auto* Target = FindProcess(PidIn(PID));
    if (!Target || Target->Exited) return -3;
    if (!MayPtrace(*Target)) return -1;
    const bool Live = Target == Task || !Target->RestorePending;
    size_t LocalIndex {}, LocalUsed {};
    int64_t Total {};
    std::vector<uint8_t> Buffer(GuestPageSize);
    for (const auto& Entry : Remote) {
      uint64_t Done {};
      while (Done < Entry[1]) {
        while (LocalIndex < Local.size() && LocalUsed == Local[LocalIndex][1]) {
          ++LocalIndex;
          LocalUsed = 0;
        }
        if (LocalIndex == Local.size()) return Total;
        const auto Chunk = std::min<uint64_t>({Entry[1] - Done, Local[LocalIndex][1] - LocalUsed, GuestPageSize});
        const auto Here = Local[LocalIndex][0] + LocalUsed, There = Entry[0] + Done;
        if (Writing) {
          if (Available(Here, PF_R) < Chunk) return Total ? Total : Fault;
          if (Target->Memory.CopyToProcess(There, reinterpret_cast<const void*>(Here), Chunk, Live) < Chunk) return Total ? Total : Fault;
          // The target's translations of its PF_X pages go stale too.
          if (Current && Current->Core) InvalidateExecutableRange(Current->Core->CurrentFrame, There, Chunk, 0, Target);
        } else {
          if (Available(Here, PF_W) < Chunk) return Total ? Total : Fault;
          if (Target->Memory.CopyFromProcess(There, Buffer.data(), Chunk, Live) < Chunk) return Total ? Total : Fault;
          std::memcpy(reinterpret_cast<void*>(Here), Buffer.data(), Chunk);
          InvalidateExecutableStores(Here, Chunk);
        }
        Done += Chunk;
        LocalUsed += Chunk;
        Total += Chunk;
      }
    }
    return Total;
  }

  // ---- membarrier --------------------------------------------------------------

  int64_t Membarrier(uint64_t Command, uint64_t Flags, uint64_t CpuId) {
    constexpr uint64_t Query = 0, Global = 1, GlobalExpedited = 2, RegisterGlobal = 4, Private = 8, RegisterPrivate = 16,
                       PrivateSyncCore = 32, RegisterSyncCore = 64, PrivateRseq = 128, RegisterRseq = 256, Registrations = 512;
    constexpr uint64_t Supported = 0x3ff;
    if (Command == PrivateRseq) {
      if (Flags && Flags != 1) return Invalid;
      if (Flags == 1 && static_cast<int32_t>(CpuId) != 0) return Invalid; // one CPU
    } else if (Flags) {
      return Invalid;
    }
    switch (Command) {
    case Query: return Supported;
    case Global: case GlobalExpedited: return 0;
    case RegisterGlobal: Task->MembarrierRegistered |= RegisterGlobal; return 0;
    case RegisterPrivate: Task->MembarrierRegistered |= RegisterPrivate; return 0;
    case RegisterSyncCore: Task->MembarrierRegistered |= RegisterSyncCore; return 0;
    case RegisterRseq: Task->MembarrierRegistered |= RegisterRseq; return 0;
    case Private: case PrivateSyncCore: case PrivateRseq:
      // Guest threads share one host thread, so every barrier already holds.
      // The registrations are kept as the REGISTER_* command bits, which GET_REGISTRATIONS reports.
      return (Task->MembarrierRegistered & (Command << 1)) ? 0 : -1;
    case Registrations: return Task->MembarrierRegistered;
    default: return Invalid;
    }
  }

  // ---- NUMA policy (one node) --------------------------------------------------

  // Reads a nodemask for set_mempolicy and mbind. Only node 0 exists.
  int64_t ReadNodeMask(uint64_t Address, uint64_t MaxNode, uint64_t& Nodes) {
    Nodes = 0;
    const auto Bits = MaxNode - 1; // get_nodes drops one from maxnode (0 wraps and is rejected)
    if (!Bits || !Address) return 0;
    if (Bits > GuestPageSize * 8) return Invalid;
    const auto Words = (Bits + 63) / 64;
    std::vector<uint64_t> Mask(Words);
    if (AccessiblePrefix(Address, Words * 8, PF_R) < Words * 8) return Fault;
    std::memcpy(Mask.data(), reinterpret_cast<const void*>(Address), Words * 8);
    if (Bits % 64) Mask.back() &= (uint64_t {1} << (Bits % 64)) - 1;
    for (size_t Index = 1; Index < Mask.size(); ++Index) {
      if (Mask[Index]) return Invalid;
    }
    Nodes = Mask[0];
    return Nodes & ~uint64_t {1} ? Invalid : 0;
  }

  // Validates a memory policy mode with its nodemask (mpol_new).
  int64_t CheckMemoryPolicy(uint64_t Mode, uint64_t Nodes, uint32_t& Out) {
    constexpr uint64_t Flags = (1 << 15) | (1 << 14) | (1 << 13);
    const auto Plain = Mode & ~Flags;
    if (Plain > 6) return Invalid;
    if ((Mode & (1 << 15)) && (Mode & (1 << 14))) return Invalid;
    const bool NeedsNodes = Plain == 2 || Plain == 3 || Plain == 5 || Plain == 6;
    if (NeedsNodes && !Nodes) return Invalid;
    if ((Plain == 0 || Plain == 4) && Nodes) return Invalid;
    Out = static_cast<uint32_t>(Plain);
    return 0;
  }

  int64_t SetMemoryPolicy(uint64_t Mode, uint64_t NodeMask, uint64_t MaxNode) {
    uint64_t Nodes {};
    if (const auto Error = ReadNodeMask(NodeMask, MaxNode, Nodes); Error) return Error;
    uint32_t Plain {};
    if (const auto Error = CheckMemoryPolicy(Mode, Nodes, Plain); Error) return Error;
    Current->MemoryPolicy = Plain;
    Current->MemoryPolicyNodes = Nodes;
    return 0;
  }

  int64_t GetMemoryPolicy(uint64_t ModeOut, uint64_t NodeMask, uint64_t MaxNode, uint64_t Address, uint64_t Flags) {
    constexpr uint64_t NodeFlag = 1, AddressFlag = 2, MemsAllowed = 4;
    // kernel_get_mempolicy: maxnode below nr_node_ids (1) with a mask is EINVAL.
    if (NodeMask && MaxNode < 1) return Invalid;
    if (Flags & ~(NodeFlag | AddressFlag | MemsAllowed)) return Invalid;
    if ((Flags & MemsAllowed) && (Flags & (NodeFlag | AddressFlag))) return Invalid;
    if ((Flags & AddressFlag) && !Task->Memory.IsMapped(Address & ~(GuestPageSize - 1), GuestPageSize)) return Fault;
    const bool Addressed = Flags & AddressFlag; // a mapping's own policy is the default
    const uint32_t Mode = Addressed ? 0 : Current->MemoryPolicy;
    const uint64_t Nodes = (Flags & MemsAllowed) ? 1 : Addressed ? 0 : Current->MemoryPolicyNodes;
    if (ModeOut && !(Flags & MemsAllowed)) {
      const int32_t Value = (Flags & NodeFlag) ? 0 : static_cast<int32_t>(Mode);
      if (const auto Error = CopyOut(ModeOut, Value); Error) return Error;
    }
    if (NodeMask) {
      // copy_nodes_to_user: ALIGN(maxnode - 1, 64) / 8 bytes; past the one
      // word of nodes the rest is cleared, and more than a page is EINVAL.
      const uint64_t Bytes = (MaxNode - 1 + 63) / 64 * 8;
      if (Bytes > GuestPageSize) return Invalid;
      if (Bytes) {
        std::vector<uint64_t> Mask(Bytes / 8);
        Mask[0] = Nodes;
        if (AccessiblePrefix(NodeMask, Bytes, PF_W) < Bytes) return Fault;
        std::memcpy(reinterpret_cast<void*>(NodeMask), Mask.data(), Bytes);
      }
    }
    return 0;
  }

  int64_t BindMemory(uint64_t Address, uint64_t Size, uint64_t Mode, uint64_t NodeMask, uint64_t MaxNode, uint64_t Flags) {
    constexpr uint64_t Valid = 1 | 2 | 4;
    if (Flags & ~Valid) return Invalid;
    if ((Flags & 4) && !Task->Creds.Capable(CapSysNice)) return -1;
    if (Address % GuestPageSize) return Invalid;
    if (Size > UINT64_MAX - GuestPageSize || Address + Task->Memory.RoundUp(Size) < Address) return Invalid;
    const auto Rounded = Task->Memory.RoundUp(Size);
    if (!Rounded) return 0;
    uint64_t Nodes {};
    if (const auto Error = ReadNodeMask(NodeMask, MaxNode, Nodes); Error) return Error;
    uint32_t Plain {};
    if (const auto Error = CheckMemoryPolicy(Mode, Nodes, Plain); Error) return Error;
    return Task->Memory.IsMapped(Address, Rounded) ? 0 : Fault;
  }

  int64_t SetHomeNode(uint64_t Address, uint64_t Size, uint64_t Node, uint64_t Flags) {
    if (Flags || Address % GuestPageSize) return Invalid;
    if (Node != 0) return Invalid;
    if (Size > UINT64_MAX - GuestPageSize || Address + Task->Memory.RoundUp(Size) < Address) return Invalid;
    const auto Rounded = Task->Memory.RoundUp(Size);
    if (!Rounded) return 0;
    return Task->Memory.IsMapped(Address, Rounded) ? 0 : -12;
  }

  // Resolves a pid argument that may be 0 (the caller) for migrate_pages and move_pages.
  int64_t NumaTarget(uint64_t PID, GuestProcess*& Target) {
    Target = PID ? FindProcess(PidIn(PID)) : Task;
    if (!Target || Target->Exited) return -3;
    if (Target != Task && !MayPtrace(*Target) && !Task->Creds.Capable(CapSysNice)) return -1;
    return 0;
  }

  int64_t MigratePages(uint64_t PID, uint64_t MaxNode, uint64_t OldNodes, uint64_t NewNodes) {
    uint64_t Old {}, New {};
    if (const auto Error = ReadNodeMask(OldNodes, MaxNode, Old); Error == Fault) return Error;
    if (const auto Error = ReadNodeMask(NewNodes, MaxNode, New); Error) return Error;
    GuestProcess* Target {};
    if (const auto Error = NumaTarget(PID, Target); Error) return Error;
    return 0; // no pages needed to move: there is one node
  }

  int64_t MovePages(uint64_t PID, uint64_t Count, uint64_t Pages, uint64_t Nodes, uint64_t Status, uint64_t Flags) {
    if (Flags & ~uint64_t {6}) return Invalid;
    if ((Flags & 4) && !Task->Creds.Capable(CapSysNice)) return -1;
    GuestProcess* Target {};
    if (const auto Error = NumaTarget(PID, Target); Error) return Error;
    if (Count > INT32_MAX) return Invalid;
    if (Count && (AccessiblePrefix(Pages, Count * 8, PF_R) < Count * 8 || AccessiblePrefix(Status, Count * 4, PF_W) < Count * 4 ||
                  (Nodes && AccessiblePrefix(Nodes, Count * 4, PF_R) < Count * 4))) return Fault;
    for (uint64_t Index = 0; Index < Count; ++Index) {
      uint64_t Page {};
      std::memcpy(&Page, reinterpret_cast<const void*>(Pages + Index * 8), 8);
      int32_t Node {};
      if (Nodes) std::memcpy(&Node, reinterpret_cast<const void*>(Nodes + Index * 4), 4);
      int32_t Result = 0;
      if (!Target->Memory.IsMapped(Page & ~(GuestPageSize - 1), GuestPageSize)) Result = -14;
      else if (Nodes && Node != 0) Result = -19;
      std::memcpy(reinterpret_cast<void*>(Status + Index * 4), &Result, 4);
    }
    return 0;
  }

  // ---- process_madvise, process_mrelease, mseal, pkeys ---------------------------

  int64_t ProcessAdvise(uint64_t FD, uint64_t Vectors, uint64_t Count, uint64_t Advice, uint64_t Flags) {
    if (Flags) return Invalid;
    // import_iovec runs before the pidfd lookup: a negative length is EINVAL,
    // one past the user address space EFAULT, and the total is clamped to MAX_RW_COUNT.
    if (Count > 1024) return Invalid;
    std::vector<std::array<uint64_t, 2>> Parts;
    if (const auto Error = ReadIovecs(Vectors, Count, Parts); Error) return Error;
    uint64_t Total {};
    for (auto& Part : Parts) {
      if (Part[1] > INT64_MAX) return Invalid;
      if (Part[1] > GuestUserLimit || Part[0] > GuestUserLimit - Part[1]) return Fault;
      Part[1] = std::min<uint64_t>(Part[1], MaxRWCount - Total);
      Total += Part[1];
    }
    const auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (!File->PidfdTarget) return BadFile;
    auto* Target = FindProcess(File->PidfdTarget);
    if (!Target || Target->Exited) return -3;
    if (Target != Task) {
      if (Advice != 3 && Advice != 20 && Advice != 21 && Advice != 25) return Invalid;
      if (!MayPtrace(*Target)) return -1;
      if (!Task->Creds.Capable(CapSysNice)) return -1;
    } else if (Advice != 3 && Advice != 20 && Advice != 21 && Advice != 25) {
      return Invalid;
    }
    int64_t Done {};
    for (const auto& Part : Parts) {
      if (Part[0] % GuestPageSize) return Done ? Done : Invalid;
      const auto Rounded = Task->Memory.RoundUp(Part[1]);
      if (Rounded && !Target->Memory.IsMapped(Part[0], Rounded)) return Done ? Done : -12;
      Done += Part[1];
    }
    return Done;
  }

  int64_t ProcessRelease(uint64_t FD, uint64_t Flags) {
    if (Flags) return Invalid;
    const auto* File = GetFile(FD);
    if (!File) return BadFile;
    if (!File->PidfdTarget) return BadFile;
    const auto* Target = FindProcess(File->PidfdTarget);
    if (!Target) return -3;
    return Target->Exited ? 0 : Invalid; // only a process that is exiting may be released
  }

  int64_t SealMemory(uint64_t Address, uint64_t Size, uint64_t Flags) {
    if (Flags || Address % GuestPageSize) return Invalid;
    if (Size > UINT64_MAX - GuestPageSize) return Invalid;
    const auto Rounded = Task->Memory.RoundUp(Size);
    if (Address + Rounded < Address) return Invalid;
    if (!Rounded) return 0;
    if (!Task->Memory.IsMapped(Address, Rounded)) return -12;
    Task->Memory.ChangeAttribute(Address, Rounded, GuestAddressSpace::Sealed, true);
    return 0;
  }

  static constexpr size_t HeapLimit = 1024 * 1024 * 1024; // brk address space; committed as the break grows
  std::string RootPath;
  std::string LaunchFolder;
  std::array<GuestThread, GuestThreadCount> Threads {};
  GuestThread* Current {};
  std::jmp_buf ReturnToHost;
  uint64_t ExitStatus {};
  mutable uint64_t IOEpoch {};
  uint32_t NextTid {ProcessID + 1};
  uint64_t NextIoContext {};
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
  // Bound AF_UNIX sockets: path names by the inode of their S_IFSOCK node, abstract names by name and type.
  std::map<uint64_t, std::weak_ptr<StreamSocket>> PathSockets;
  std::map<std::pair<std::string, Switch2Guest::SocketKind>, std::weak_ptr<StreamSocket>> AbstractSockets;
  uint32_t AutobindSequence {};
  std::vector<std::weak_ptr<StreamSocket>> SocketRightsReceivers;
  std::vector<std::weak_ptr<InetSocket>> LiveInetSockets; // the sockets PollNet watches
  uint64_t NextAnonymousInode {4};
  uint64_t AnonInode {}; // the shared anonymous inode, allocated on first use
  uint64_t NextSdInode {InodeSnapshot::FirstInode};
  InodeNames SdInodes;
  InodeRecords Inodes;
  std::set<uint64_t> RetiredBackingObjects;
  FileReadCache ReadCache;
  std::set<uint64_t> DirtyFileData;
  std::vector<std::weak_ptr<OpenFile>> DiskFiles;
  // Read-only MAP_SHARED views of SD files. Each holds its open file, as a
  // Linux mapping does, and is refreshed when the file's data changes, so it
  // stays coherent with writes through any descriptor.
  struct SharedFileView {
    std::weak_ptr<void> Storage;
    std::shared_ptr<OpenFile> File;
    uint64_t Offset {};
    size_t Size {};
    bool MayWrite {};
    std::vector<uint8_t> Snapshot; // MayWrite views: the bytes as last read from or written to the file
  };
  std::multimap<uint64_t, SharedFileView> SharedFileViews;
  uint32_t ViewSyncDepth {}; // nonzero while views are flushed or refreshed
  // memfd storage by mapped address, for MADV_REMOVE.
  std::map<const void*, std::weak_ptr<GuestMemoryFile>> MappedMemoryFiles;
  // System V IPC objects. The namespace lives as long as the launcher (no persistence across restarts).
  // The namespace registry: the initial namespaces and the inode numbers of the others.
  Switch2Guest::Ns::Registry NsRegistry;
  std::map<uint64_t, std::weak_ptr<FifoState>> FifoEndpoints;
  uint64_t InodeGeneration {};
  bool InodeStoreAvailable {};
  bool InodeStoreDirty {};
  std::map<uint64_t, std::vector<std::weak_ptr<OpenFile>>> FlockHolders;
  std::map<uint64_t, std::vector<ByteLock>> RecordLocks;
  std::map<uint32_t, GuestProcess> Processes;
  GuestProcess* Task {};
  uint32_t ForegroundGroup {ProcessID};
  uint32_t TerminalSession {ProcessID}; // 0: no session controls the console.
  PadState Input {};
  const u64 StartTick {armGetSystemTick()};
  // Guest CLOCK_REALTIME: the Horizon RTC second at startup, then real elapsed time. Process-global
  // for this launch; clock_settime and friends change it, never Horizon's clock, and it is not persisted.
  Switch2Guest::RealtimeBase Realtime {static_cast<int64_t>(time(nullptr)) * 1'000'000'000, MonotonicNs()};
  // The running quantum of the guest thread: its thread tick count when the dispatch began.
  uint64_t QuantumStart {};
  bool InDispatch {};
  // settimeofday's timezone (gettimeofday reports it) and the NTP state adjtimex reads and writes.
  int32_t TimezoneMinutesWest {}, TimezoneDstTime {};
  struct NtpState {
    int64_t Offset {}, Frequency {}, MaxError {16'000'000}, EstError {16'000'000}, Constant {2}, Tick {10'000};
    int32_t Status {0x40}; // STA_UNSYNC, as a kernel boots without an NTP daemon
  } Ntp;
  Framebuffer Video {};
  bool Presented {};

};

// Linux file operations for the launcher's FTP server. Each call goes
// through the same syscall handler a guest program would reach, on a
// namespace of its own with no guest running, so permissions, the inode
// store and overflow paths all behave as they do for guests.
class GuestFileService final : public FtpFileSystem, public RootfsSetup::RootfsWriter {
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
    Task.Creds = Switch2Guest::RootCredentials();
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

  // RootfsWriter: dpkg's unpack operations through the guest syscalls, so
  // owners, modes, links and long names land exactly as a guest would
  // create them. The service acts as root.
  int64_t MakeDirectory(const std::string& Path, uint32_t Mode, uint32_t Uid, uint32_t Gid) override {
    FtpEntry Entry {};
    const auto Existing = Stat(Path, Entry, true);
    if (!Existing && (Entry.Mode & 0170000) == 0120000) {
      // dpkg keeps a symlink to a directory (e.g. a merged /usr layout).
      if (!Stat(Path, Entry, false) && (Entry.Mode & 0170000) == 0040000) return 0;
      return -17;
    }
    if (!Existing && (Entry.Mode & 0170000) != 0040000) return -17;
    // Like dpkg, an existing directory keeps its owner and mode (this also
    // covers mount points such as /dev and /proc).
    if (!Existing) return 0;
    if (Existing != -2) return Existing;
    if (const auto Error = SetPath(0, Path)) return Error;
    if (const auto Error = Syscalls->CreateDirectoryGuest(CurrentDirectory, PathAddress(0), 0700)) return Error;
    return SetOwnerAndMode(Path, Uid, Gid, Mode);
  }

  int64_t WriteFile(const std::string& Path, std::span<const uint8_t> Data, uint32_t Mode, uint32_t Uid, uint32_t Gid,
                    int64_t ModifyTime) override {
    constexpr uint64_t WriteOnly = 1, Create = 0100, Truncate = 01000, NoFollow = 0400000, CloseOnExec = 02000000;
    const auto Temporary = Path + ".dpkg-new";
    if (const auto Error = SetPath(0, Temporary)) return Error;
    const auto FD = Syscalls->OpenGuest(CurrentDirectory, PathAddress(0), WriteOnly | Create | Truncate | NoFollow | CloseOnExec, 0600);
    if (FD < 0) return FD;
    for (size_t Offset = 0; Offset < Data.size();) {
      const auto Written = Write(FD, Data.data() + Offset, Data.size() - Offset);
      if (Written <= 0) {
        Syscalls->CloseGuest(FD);
        return Written ? Written : -5;
      }
      Offset += Written;
    }
    if (const auto Error = Syscalls->CloseGuest(FD)) return Error;
    if (const auto Error = SetOwnerAndMode(Temporary, Uid, Gid, Mode)) return Error;
    if (const auto Error = SetModifyTime(Temporary, ModifyTime)) return Error;
    return Rename(Temporary, Path);
  }

  int64_t Symlink(const std::string& Target, const std::string& Path, uint32_t Uid, uint32_t Gid) override {
    if (const auto Error = RemoveNonDirectory(Path)) return Error;
    if (const auto Error = SetPath(0, Path)) return Error;
    if (const auto Error = SetPath(1, Target)) return Error;
    if (const auto Error = Syscalls->CreateSymlink(PathAddress(1), CurrentDirectory, PathAddress(0))) return Error;
    return Syscalls->ChangeOwner(CurrentDirectory, PathAddress(0), Uid, Gid, SymlinkNoFollow);
  }

  int64_t HardLink(const std::string& Existing, const std::string& Path) override {
    if (const auto Error = RemoveNonDirectory(Path)) return Error;
    if (const auto Error = SetPath(0, Existing)) return Error;
    if (const auto Error = SetPath(1, Path)) return Error;
    return Syscalls->LinkGuest(CurrentDirectory, PathAddress(0), CurrentDirectory, PathAddress(1), 0);
  }

  int64_t MakeNode(const std::string& Path, uint32_t Mode, uint64_t Device, uint32_t Uid, uint32_t Gid) override {
    // The runtime has no device nodes (mknod of a device returns EPERM);
    // FIFOs work.
    if (Device) return -1;
    if (const auto Error = RemoveNonDirectory(Path)) return Error;
    if (const auto Error = SetPath(0, Path)) return Error;
    if (const auto Error = Syscalls->MakeNode(CurrentDirectory, PathAddress(0), Mode)) return Error;
    return SetOwnerAndMode(Path, Uid, Gid, Mode & 07777);
  }

private:
  // chown first: it clears set-user-ID and set-group-ID bits.
  int64_t SetOwnerAndMode(const std::string& Path, uint32_t Uid, uint32_t Gid, uint32_t Mode) {
    if (const auto Error = SetPath(0, Path)) return Error;
    if (const auto Error = Syscalls->ChangeOwner(CurrentDirectory, PathAddress(0), Uid, Gid, SymlinkNoFollow)) return Error;
    return Syscalls->ChangeModeAt(CurrentDirectory, PathAddress(0), Mode & 07777, 0);
  }

  int64_t RemoveNonDirectory(const std::string& Path) {
    FtpEntry Entry {};
    const auto Existing = Stat(Path, Entry, true);
    if (Existing == -2) return 0;
    if (Existing) return Existing;
    if ((Entry.Mode & 0170000) == 0040000) return -21;
    return Remove(Path);
  }

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

std::unique_ptr<RootfsSetup::RootfsWriter> OpenGuestRootfsWriter() {
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

// Network files of the guest rootfs: /etc/resolv.conf from the connection's DNS servers (rewritten only
// when it is absent or carries our marker line, and only with a connection) and /etc/hosts with a
// localhost line (only when absent). Once per distinct content per launcher run.
static void PrepareNetworkFiles() {
  static constexpr std::string_view Marker = "# Generated by FEX Switch 2 launcher\n";
  static std::string LastResolv;
  static bool HostsChecked {};
  uint32_t Primary {}, Secondary {};
  std::string Resolv;
  if (HostNet::DnsServers(Primary, Secondary) && (Primary || Secondary)) {
    auto Text = [](uint32_t Address) {
      const auto* Part = reinterpret_cast<const uint8_t*>(&Address);
      return std::to_string(Part[0]) + "." + std::to_string(Part[1]) + "." + std::to_string(Part[2]) + "." + std::to_string(Part[3]);
    };
    Resolv = std::string(Marker);
    if (Primary) Resolv += "nameserver " + Text(Primary) + "\n";
    if (Secondary) Resolv += "nameserver " + Text(Secondary) + "\n";
    Resolv += "options timeout:2 attempts:2\n";
  }
  const bool NeedResolv = !Resolv.empty() && Resolv != LastResolv;
  if (!NeedResolv && HostsChecked) return;
  GuestFileService Service;
  if (!Service.Ready()) return;
  FtpEntry Entry {};
  if (NeedResolv) {
    LastResolv = Resolv;
    bool Replace = Service.Stat("/etc/resolv.conf", Entry, false) < 0;
    if (!Replace && (Entry.Mode & 0170000) == 0100000) {
      char Head[64] {};
      const auto Handle = Service.OpenRead("/etc/resolv.conf", 0);
      if (Handle >= 0) {
        const auto Got = Service.Read(Handle, Head, Marker.size());
        Service.Close(Handle);
        Replace = Got == static_cast<int64_t>(Marker.size()) && std::string_view(Head, Marker.size()) == Marker;
      }
    }
    if (Replace) {
      const auto Error = Service.WriteFile("/etc/resolv.conf", RootfsSetup::Bytes(Resolv), 0644, 0, 0, static_cast<int64_t>(std::time(nullptr)));
      char Message[200];
      std::snprintf(Message, sizeof(Message), "Guest network: /etc/resolv.conf %s (nameservers %s %s)",
                    Error ? "not written" : "written", Resolv.substr(Marker.size(), Resolv.find('\n', Marker.size()) - Marker.size()).c_str(),
                    Secondary ? "+1" : "");
      WriteLog(Message);
    } else {
      WriteLog("Guest network: /etc/resolv.conf is not ours; left unchanged.");
    }
  }
  if (!HostsChecked) {
    HostsChecked = true;
    if (Service.Stat("/etc/hosts", Entry, true) == -2) {
      constexpr std::string_view Hosts = "127.0.0.1\tlocalhost\n";
      Service.WriteFile("/etc/hosts", RootfsSetup::Bytes(Hosts), 0644, 0, 0, static_cast<int64_t>(std::time(nullptr)));
    }
  }
}

static std::optional<uint64_t> RunGuestOnCurrentThread(uint64_t Entry, std::span<const Segment> Segments, const std::string& Folder,
                                 std::string_view Program, std::span<const std::string> Arguments,
                                 const ElfImage* ProgramImage, uint64_t InterpreterBase, const std::string& WorkingDirectory, bool SingleBlock, bool Interactive) {
  PrepareNetworkFiles();
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
  // Debian's login PATHs (login.defs ENV_SUPATH / ENV_PATH): root also
  // searches the sbin directories, where dpkg expects ldconfig and
  // start-stop-daemon.
  const bool Root = LaunchCredentials && LaunchCredentials->first == 0;
  std::vector<std::string> Environment {Root ? "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
                                             : "PATH=/usr/local/bin:/usr/bin:/bin",
                                        Root ? "HOME=/root" : "HOME=/", "LANG=C", Interactive ? "TERM=ansi" : "TERM=dumb"};
  if (Interactive) {
    Environment.insert(Environment.end(), {"HISTFILE=", "TERMINFO=/lib/terminfo", "PS1=\\[\\e[36m\\]fex\\[\\e[0m\\]:\\w\\$ "});
  }
  // The auxiliary vector carries the launch identity (AT_EUID of a root run is 0), as an exec would.
  const auto LaunchIds = Switch2Guest::AuxvIds(LaunchCredentials ? Switch2Guest::LaunchCredentialsFor(LaunchCredentials->first, LaunchCredentials->second)
                                                                 : Switch2Guest::Credentials {});
  State.gregs[X86State::REG_RSP] = BuildInitialStack(StackBase, Program, Argv, Environment, ProgramImage, InterpreterBase, LaunchIds);

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
  Result Status = SdRetry(fsOpenSdCardFileSystem, &SdCard);
  if (R_FAILED(Status)) {
    return Status;
  }
  FsDir Directory {};
  Status = SdRetry(fsFsOpenDirectory, &SdCard, Folder.c_str(), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles | FsDirOpenMode_NoFileSize, &Directory);
  fsFsClose(&SdCard);
  if (R_FAILED(Status)) {
    return Status;
  }

  std::array<FsDirectoryEntry, 8> Batch;
  for (;;) {
    s64 Count {};
    Status = SdRetry(fsDirRead, &Directory, &Count, Batch.size(), Batch.data());
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

