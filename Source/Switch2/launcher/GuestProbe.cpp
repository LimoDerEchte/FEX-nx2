// SPDX-License-Identifier: MIT
#include "GuestRuntime.h"
#include "HostNet.h"
#include "SdPath.h"
#include "GuestRootFS.h"
#include "GuestFileExtents.h"
#include "GuestLogBuffer.h"
#include "LauncherBackend.h"
#include "LauncherHost.h"
#include "platform/Log.h"
#include "platform/Platform.h"
#include "App.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Utils/LogManager.h>

#include <arpa/inet.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>

namespace {
// The launcher's version: the banner, the Home screen's version block and the NACP (CMakeLists.txt) all show it.
constexpr char LauncherVersion[] = "0.18.25";
} // namespace

namespace Switch2Guest {
constexpr size_t GuestPageSize = Switch2Guest::PageSize;
constexpr char DefaultFolder[] = "/fex/games";
constexpr char LogPath[] = "/fex/fex-switch2-guest.log";

std::recursive_mutex LogMutex;
FsFile LogFile {};
s64 LogOffset {};
bool LogOpen {};
bool LogWriteFailed {};
bool BufferingGuestLog {};
alignas(GuestPageSize) GuestLogBuffer<2 * 1024 * 1024> GuestLog;
std::atomic<LogLevel> Logging {};
constexpr const char* LogLevelLabels[] {"Logging  Standard", "Logging  Detailed", "Logging  Trace"};
bool GraphicsActive {};

Result EnsureDirectory(FsFileSystem& SdCard, const char* Path) {
  const Result Status = fsFsCreateDirectory(&SdCard, SdPath(Path));
  if (R_SUCCEEDED(Status)) {
    return Status;
  }

  FsDirEntryType Type {};
  return R_SUCCEEDED(fsFsGetEntryType(&SdCard, SdPath(Path), &Type)) && Type == FsDirEntryType_Dir ? 0 : Status;
}

Result OpenLog() {
  FsFileSystem SdCard {};
  Result Status = fsOpenSdCardFileSystem(&SdCard);
  if (R_FAILED(Status)) {
    return Status;
  }

  FsDirEntryType Existing {};
  FsDirEntryType Legacy {};
  if (R_FAILED(fsFsGetEntryType(&SdCard, SdPath("/fex"), &Existing)) &&
      R_SUCCEEDED(fsFsGetEntryType(&SdCard, SdPath("/switch/fex"), &Legacy)) && Legacy == FsDirEntryType_Dir) {
    Status = fsFsRenameDirectory(&SdCard, SdPath("/switch/fex"), SdPath("/fex"));
    if (R_FAILED(Status)) {
      fsFsClose(&SdCard);
      return Status;
    }
  }

  for (const char* Path : {"/fex", DefaultFolder, Switch2Guest::Root, Switch2Guest::InodeFiles::Directory}) {
    Status = EnsureDirectory(SdCard, Path);
    if (R_FAILED(Status)) {
      fsFsClose(&SdCard);
      return Status;
    }
  }

  constexpr u32 Mode = FsOpenMode_Write | FsOpenMode_Append;
  Status = fsFsOpenFile(&SdCard, SdPath(LogPath), Mode, &LogFile);
  if (R_FAILED(Status)) {
    const Result CreateStatus = fsFsCreateFile(&SdCard, SdPath(LogPath), 0, 0);
    Status = R_SUCCEEDED(CreateStatus) ? fsFsOpenFile(&SdCard, SdPath(LogPath), Mode, &LogFile) : CreateStatus;
  }
  fsFsClose(&SdCard);
  if (R_FAILED(Status)) {
    return Status;
  }

  Status = fsFileGetSize(&LogFile, &LogOffset);
  LogOpen = R_SUCCEEDED(Status);
  if (!LogOpen) {
    fsFileClose(&LogFile);
  }
  return Status;
}

bool WriteLogBytes(std::span<const char> Bytes) {
  if (!LogOpen || LogWriteFailed) return false;
  const Result Status = fsFileWrite(&LogFile, LogOffset, Bytes.data(), Bytes.size(), FsWriteOption_None);
  if (R_SUCCEEDED(Status)) {
    LogOffset += Bytes.size();
    return true;
  }
  LogWriteFailed = true;
  std::printf("SD log write failed (0x%08x)\n", Status);
  return false;
}

void FlushLogFile() {
  if (!LogOpen || LogWriteFailed) return;
  const auto Status = fsFileFlush(&LogFile);
  if (R_FAILED(Status)) {
    LogWriteFailed = true;
    std::printf("SD log flush failed (0x%08x)\n", Status);
  }
}

// EXPERIMENT (multithreading scaling): time this host thread spent writing log lines (SD write and flush).
thread_local uint64_t LogWriteTicks {};
struct LogWriteTimer {
  uint64_t Start = armGetSystemTick();
  ~LogWriteTimer() { LogWriteTicks += armGetSystemTick() - Start; }
};

void WriteLog(const char* Text) {
  LogWriteTimer Timer;
  std::lock_guard Lock(LogMutex);
  if (!LogOpen || LogWriteFailed) return;
  constexpr size_t MaxLine = 1023;
  const size_t Length = std::min(std::strlen(Text), MaxLine);
  if (BufferingGuestLog) {
    GuestLog.Append({Text, Length});
    return;
  }
  char Line[MaxLine + 1];
  std::memcpy(Line, Text, Length);
  Line[Length] = '\n';
  if (WriteLogBytes({Line, Length + 1})) FlushLogFile();
}

void WriteGuestOutput(const char* Text) {
  std::lock_guard Lock(LogMutex);
  if (BufferingGuestLog && LogOpen && !LogWriteFailed) {
    // Program output goes to the file at once, marked "[out]", so it
    // survives a launcher crash that loses the buffered trace.
    constexpr size_t MaxLine = 1023;
    char Line[MaxLine + 8];
    const auto Length = std::snprintf(Line, sizeof(Line), "[out] %.*s\n", static_cast<int>(MaxLine), Text);
    if (Length > 0 && WriteLogBytes({Line, std::min<size_t>(Length, sizeof(Line) - 1)})) FlushLogFile();
  }
  WriteLog(Text);
}

void WriteRunCheckpoint(const char* Text) {
  std::lock_guard Lock(LogMutex);
  char Line[640];
  const auto Length = std::snprintf(Line, sizeof(Line), "%s\n", Text);
  if (Length > 0 && WriteLogBytes({Line, std::min<size_t>(Length, sizeof(Line) - 1)})) FlushLogFile();
}

// Standard log level: the runtime's trace is buffered in memory and written when the run ends (fast, but a crash that
// the fatal hooks do not see loses it). Detailed and Trace write each line to the SD at once: on HOS a process the system
// closes leaves nothing else behind.
void BeginGuestLog() {
  std::lock_guard Lock(LogMutex);
  const bool Buffer = Logging == LogLevel::Standard;
  WriteLog(Buffer ? "Guest run started. Runtime trace is buffered." : "Guest run started. Runtime trace is written at once.");
  GuestLog.Clear();
  BufferingGuestLog = Buffer;
}

template<size_t Capacity>
void WriteLogBuffer(const GuestLogBuffer<Capacity>& Buffer) {
  for (auto Bytes : Buffer.Contents()) {
    constexpr size_t MaxWrite = 64 * 1024;
    while (!Bytes.empty()) {
      const auto Count = std::min(Bytes.size(), MaxWrite);
      if (!WriteLogBytes(Bytes.first(Count))) return;
      Bytes = Bytes.subspan(Count);
    }
  }
}

void EndGuestLog() {
  std::lock_guard Lock(LogMutex);
  if (!BufferingGuestLog) return;
  BufferingGuestLog = false;
  const auto Dropped = GuestLog.DroppedLines();
  if (Dropped) {
    char Message[96];
    const auto Length = std::snprintf(Message, sizeof(Message), "Guest log omitted %zu older lines.\n", Dropped);
    WriteLogBytes({Message, static_cast<size_t>(Length)});
  }
  WriteLogBuffer(GuestLog);
  FlushLogFile();
  GuestLog.Clear();
}

// Fatal paths (CPU exception, abort, diagAbortWithResult) write the buffered guest trace and their line even when
// another thread holds LogMutex: with the guest worker running beside the UI (UI Task 8b) the lock is often taken when
// the other thread dies. Wait up to 200 ms for it, then write without it: the process ends anyway.
void WriteFatalLog(const char* Text) {
  static std::atomic<bool> Busy {};
  if (Busy.exchange(true)) return;
  std::unique_lock Lock(LogMutex, std::defer_lock);
  for (int Try = 0; Try < 200 && !Lock.try_lock(); ++Try) svcSleepThread(1'000'000);
  if (!LogOpen || LogWriteFailed) return;
  if (BufferingGuestLog) {
    BufferingGuestLog = false;
    if (const auto Dropped = GuestLog.DroppedLines()) {
      char Message[96];
      const auto Length = std::snprintf(Message, sizeof(Message), "Guest log omitted %zu older lines.\n", Dropped);
      WriteLogBytes({Message, static_cast<size_t>(Length)});
    }
    WriteLogBuffer(GuestLog);
  }
  if (!Lock.owns_lock()) WriteLogBytes(std::string_view {"Fatal log written without the log lock.\n"});
  WriteLogBytes({Text, std::strlen(Text)});
  FlushLogFile();
}

std::mutex StageMutex;
std::function<void(const char*)> StageSink;

void SetStageSink(std::function<void(const char*)> Sink) {
  std::lock_guard<std::mutex> Lock(StageMutex);
  StageSink = std::move(Sink);
}

// Progress and error lines: the SD log, and the terminal or install log that is open (the launcher has no text console any more).
void PrintStage(const char* Text) {
  WriteLog(Text);
  std::function<void(const char*)> Sink;
  {
    std::lock_guard<std::mutex> Lock(StageMutex);
    Sink = StageSink;
  }
  if (Sink) Sink(Text);
}

int64_t ReadFileHead(const char* Path, std::span<char> Buffer, uint64_t Size, const PackedFileExtents* Extents) {
  FsFileSystem SdCard {};
  if (R_FAILED(fsOpenSdCardFileSystem(&SdCard))) return -5;
  FsFile File {};
  const Result Status = fsFsOpenFile(&SdCard, SdPath(Path), FsOpenMode_Read, &File);
  fsFsClose(&SdCard);
  if (R_FAILED(Status)) return -5;
  const auto Wanted = static_cast<size_t>(std::min<uint64_t>(Size, Buffer.size()));
  const auto Read = [&](uint64_t Position, std::span<char> Chunk) -> int64_t {
    u64 Count {};
    const auto Result = fsFileRead(&File, Position, Chunk.data(), Chunk.size(), FsReadOption_None, &Count);
    return R_FAILED(Result) ? -5 : static_cast<int64_t>(Count);
  };
  int64_t Received {};
  if (Extents) {
    Received = Extents->ReadBytes(0, Buffer.first(Wanted), Read);
  } else {
    // Bytes past the stored data of a file extended by truncate read as zeros.
    s64 Physical {};
    if (R_FAILED(fsFileGetSize(&File, &Physical))) Physical = 0;
    const auto Stored = std::min<size_t>(Wanted, static_cast<size_t>(std::max<s64>(Physical, 0)));
    Received = Stored ? Read(0, Buffer.first(Stored)) : 0;
    if (Received == static_cast<int64_t>(Stored)) {
      std::memset(Buffer.data() + Stored, 0, Wanted - Stored);
      Received = Wanted;
    }
  }
  fsFileClose(&File);
  return Received < 0 ? Received : Received == static_cast<int64_t>(Wanted) ? Received : -5;
}

const char* ReadElf(const char* Path, Switch2Guest::ElfImage& Image, uint64_t Limit, const PackedFileExtents* Extents) {
  FsFileSystem SdCard {};
  if (R_FAILED(fsOpenSdCardFileSystem(&SdCard))) {
    return "Could not open the SD card.";
  }
  FsFile File {};
  Result Status = fsFsOpenFile(&SdCard, SdPath(Path), FsOpenMode_Read, &File);
  fsFsClose(&SdCard);
  if (R_FAILED(Status)) {
    return "Could not open the selected file.";
  }

  s64 Size {};
  Status = fsFileGetSize(&File, &Size);
  const auto PhysicalSize = Size;
  if (Limit != UINT64_MAX) Size = std::min<uint64_t>(Limit, Switch2Guest::MaxElfSize + 1);
  if (R_FAILED(Status) || Size <= 0 || Size > Switch2Guest::MaxElfSize) {
    fsFileClose(&File);
    return "Invalid or unreadable ELF size.";
  }
  std::unique_ptr<uint8_t, decltype(&std::free)> Bytes {
    static_cast<uint8_t*>(std::aligned_alloc(GuestPageSize, (Size + GuestPageSize - 1) & ~(GuestPageSize - 1))), &std::free};
  if (!Bytes) {
    fsFileClose(&File);
    return "Could not allocate the ELF read buffer.";
  }
  constexpr u64 ReadChunkSize = 64 * 1024;
  const auto FileSize = static_cast<u64>(Size);
  const auto DataSize = std::min<u64>(FileSize, PhysicalSize);
  if (Extents) {
    const auto Received = Extents->ReadBytes(0, {reinterpret_cast<char*>(Bytes.get()), static_cast<size_t>(Size)},
      [&](uint64_t Position, std::span<char> Chunk) -> int64_t {
        u64 Read {};
        const auto Result = fsFileRead(&File, Position, Chunk.data(), Chunk.size(), FsReadOption_None, &Read);
        return R_FAILED(Result) ? -5 : static_cast<int64_t>(Read);
      });
    if (Received != Size) {
      fsFileClose(&File);
      return "Could not read the complete ELF.";
    }
  } else {
    for (u64 Offset = 0; Offset < DataSize;) {
      const u64 Chunk = std::min(DataSize - Offset, ReadChunkSize);
      u64 Read {};
      Status = fsFileRead(&File, Offset, Bytes.get() + Offset, Chunk, FsReadOption_None, &Read);
      if (R_FAILED(Status) || Read != Chunk) {
        fsFileClose(&File);
        return "Could not read the complete ELF.";
      }
      Offset += Read;
    }
    std::memset(Bytes.get() + DataSize, 0, FileSize - DataSize);
  }
  fsFileClose(&File);

  const auto* Error = Switch2Guest::LoadElf({Bytes.get(), static_cast<size_t>(Size)}, Image);
  if (Error || Logging == LogLevel::Trace) {
    std::array<uint8_t, 32> Hash {};
    sha256CalculateHash(Hash.data(), Bytes.get(), Size);
    char Hex[65] {};
    for (size_t Index = 0; Index < Hash.size(); ++Index) {
      std::snprintf(Hex + Index * 2, 3, "%02x", Hash[Index]);
    }
    char Message[1024];
    std::snprintf(Message, sizeof(Message), "ELF SD file %s size=%lld sha256=%s",
                  Path, static_cast<long long>(Size), Hex);
    WriteLog(Message);
    if (Error) WriteLog(Error);
  }
  return Error;
}
} // namespace Switch2Guest

extern "C" {
alignas(16) char __nx_exception_stack[16 * 1024];
uint64_t __nx_exception_stack_size = sizeof(__nx_exception_stack);
uint32_t __nx_exception_ignoredebug = 1;
extern char _start[];

void __libnx_exception_handler(ThreadExceptionDump* Dump) {
  if (!Dump) return;
  char Line[512];
  const auto Base = reinterpret_cast<uint64_t>(_start);
  auto Length = std::snprintf(Line, sizeof(Line),
                "Host fault desc=%#x esr=%#x far=%#llx pc=%#llx lr=%#llx sp=%#llx fp=%#llx nro_base=%#llx\n",
                Dump->error_desc, Dump->esr, static_cast<unsigned long long>(Dump->far.x),
                static_cast<unsigned long long>(Dump->pc.x), static_cast<unsigned long long>(Dump->lr.x),
                static_cast<unsigned long long>(Dump->sp.x), static_cast<unsigned long long>(Dump->fp.x),
                static_cast<unsigned long long>(Base));
  MemoryInfo Stack {};
  u32 Page {};
  if (Length > 0 && static_cast<size_t>(Length) < sizeof(Line) && R_SUCCEEDED(svcQueryMemory(&Stack, &Page, Dump->sp.x))) {
    std::snprintf(Line + Length, sizeof(Line) - Length, "Host fault stack region base=%#llx size=%#llx perm=%#x type=%#x\n",
                  static_cast<unsigned long long>(Stack.addr), static_cast<unsigned long long>(Stack.size), Stack.perm, Stack.type);
  }
  Switch2Guest::WriteFatalLog(Line);
}

// abort() (newlib assert, libstdc++ __throw_* under -fno-exceptions, FEXCore's fatal paths): log where it came from,
// then abort. Linked with --wrap=abort.
[[noreturn]] void __real_abort();
[[noreturn]] void __wrap_abort() {
  char Line[160];
  std::snprintf(Line, sizeof(Line), "Host abort lr=%#llx nro_base=%#llx\n",
                static_cast<unsigned long long>(reinterpret_cast<uint64_t>(__builtin_return_address(0))),
                static_cast<unsigned long long>(reinterpret_cast<uint64_t>(_start)));
  Switch2Guest::WriteFatalLog(Line);
  __real_abort();
}

// libounce's weak diagAbortWithResult breaks into the kernel without a trace; log it first.
void diagAbortWithResult(Result Code) {
  char Line[160];
  std::snprintf(Line, sizeof(Line), "Host diagAbortWithResult result=%#x lr=%#llx nro_base=%#llx\n", Code,
                static_cast<unsigned long long>(reinterpret_cast<uint64_t>(__builtin_return_address(0))),
                static_cast<unsigned long long>(reinterpret_cast<uint64_t>(_start)));
  Switch2Guest::WriteFatalLog(Line);
  svcBreak(BreakReason_Panic, reinterpret_cast<uintptr_t>(&Code), sizeof(Code));
  __builtin_unreachable();
}
}

namespace {
void UiLogSink(const char* Line) { Switch2Guest::WriteLog(Line); }

// The UI could not start (no Vulkan, no window): show why on the libnx text console, so the user sees something, then wait for +.
void ShowStartFailure(Result LogStatus) {
  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState Pad {};
  padInitializeDefault(&Pad);
  std::printf("FEX for Switch 2 %s\n\nCould not start the graphical interface.\nSee %s on the SD card.\n", LauncherVersion, Switch2Guest::LogPath);
  if (R_FAILED(LogStatus)) std::printf("The SD log could not be opened (0x%08x).\n", LogStatus);
  std::puts("\nPress + to exit.");
  while (appletMainLoop()) {
    padUpdate(&Pad);
    if (padGetButtonsDown(&Pad) & HidNpadButton_Plus) break;
    consoleUpdate(nullptr);
  }
  consoleExit(nullptr);
}
} // namespace

// userAppInit (ui/platform/switch2/Switch2AppInit.cpp) makes the default NWindow follow the console resolution before Vulkan opens the surface.

int main() {
  using namespace Switch2Guest;
  const Result LogStatus = OpenLog();
  {
    char Banner[96];
    std::snprintf(Banner, sizeof(Banner), "--- FEX Switch 2 launcher %s started ---", LauncherVersion);
    WriteLog(Banner);
  }
  // The SD card as sdmc: for the settings, license and RootFS files the UI reads and writes through stdio.
  if (!fsdevGetDeviceFileSystem("sdmc")) fsdevMountSdmc();
  WriteLog("Initializing FEXCore configuration...");
  FEXCore::Config::Initialize();
  LogMan::Msg::InstallHandler([](LogMan::DebugLevels Level, const char* Message) {
    if (BufferingGuestLog && (Level == LogMan::ERROR || Level == LogMan::ASSERT)) {
      WriteRunCheckpoint(Message);
    }
    PrintStage(Message);
  });
  LogMan::Throw::InstallHandler(PrintStage);

  ui::SetLogSink(UiLogSink);
  WriteLog("Starting the graphical interface...");
  std::unique_ptr<ui::Platform> Platform = ui::CreatePlatform(ui::PlatformOptions {});
  if (!Platform) {
    WriteLog("FAIL create platform: see the steps above");
    ShowStartFailure(LogStatus);
  } else {
    {
      auto Backend = CreateLauncherBackend(LauncherVersion);
      ui::App App(*Platform, *Backend, []() { return static_cast<uint32_t>(randomGet64()); });
      if (!App.Ok()) {
        WriteLog("FAIL fonts did not load");
      } else {
        WriteLog("The graphical interface is running.");
        App.Run();
      }
      WriteLog("The graphical interface ended.");
    }
    Platform.reset();
  }

  WriteLog("Closing guest probe.");
  ShutdownGuestArena();
  LogMan::Throw::UnInstallHandler();
  LogMan::Msg::UnInstallHandler();
  FEXCore::Config::Shutdown();
  if (LogOpen) {
    fsFileClose(&LogFile);
  }
  return 0;
}
