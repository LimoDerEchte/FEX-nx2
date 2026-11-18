// SPDX-License-Identifier: MIT
#include "GuestElf.h"
#include "GuestAddressSpace.h"
#include "GuestRootFS.h"
#include "GuestVideo.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/Switch2/GuestTrace.h>

#include <switch.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <string>
#include <vector>

bool RunSwitch2JitProbe();

namespace {
constexpr size_t GuestPageSize = Switch2Guest::PageSize;
constexpr uint64_t ExpectedExitStatus = 42;
constexpr uint64_t LinuxWriteSyscall = 1;
constexpr uint64_t LinuxExitSyscall = 60;
constexpr char DefaultFolder[] = "/fex/games";
constexpr char SettingsPath[] = "sdmc:/fex/fex-switch2.cfg";
constexpr char LogPath[] = "/fex/fex-switch2-guest.log";

FsFile LogFile {};
s64 LogOffset {};
bool LogOpen {};
bool LogWriteFailed {};
enum class LogLevel { Standard, Detailed, Trace };
LogLevel Logging {};
constexpr const char* LogLevelLabels[] {"Logging  Standard", "Logging  Detailed", "Logging  Trace"};
bool GraphicsActive {};

Result EnsureDirectory(FsFileSystem& SdCard, const char* Path) {
  const Result Status = fsFsCreateDirectory(&SdCard, Path);
  if (R_SUCCEEDED(Status)) {
    return Status;
  }

  FsDirEntryType Type {};
  return R_SUCCEEDED(fsFsGetEntryType(&SdCard, Path, &Type)) && Type == FsDirEntryType_Dir ? 0 : Status;
}

Result OpenLog() {
  FsFileSystem SdCard {};
  Result Status = fsOpenSdCardFileSystem(&SdCard);
  if (R_FAILED(Status)) {
    return Status;
  }

  FsDirEntryType Existing {};
  FsDirEntryType Legacy {};
  if (R_FAILED(fsFsGetEntryType(&SdCard, "/fex", &Existing)) &&
      R_SUCCEEDED(fsFsGetEntryType(&SdCard, "/switch/fex", &Legacy)) && Legacy == FsDirEntryType_Dir) {
    Status = fsFsRenameDirectory(&SdCard, "/switch/fex", "/fex");
    if (R_FAILED(Status)) {
      fsFsClose(&SdCard);
      return Status;
    }
  }

  for (const char* Path : {"/fex", DefaultFolder, Switch2Guest::Root, "/fex/rootfs/lib", "/fex/rootfs/lib64",
                           "/fex/rootfs/usr", "/fex/rootfs/usr/lib", "/fex/rootfs/etc", "/fex/rootfs/tmp",
                           "/fex/rootfs/dev", "/fex/rootfs/proc"}) {
    Status = EnsureDirectory(SdCard, Path);
    if (R_FAILED(Status)) {
      fsFsClose(&SdCard);
      return Status;
    }
  }

  constexpr u32 Mode = FsOpenMode_Write | FsOpenMode_Append;
  Status = fsFsOpenFile(&SdCard, LogPath, Mode, &LogFile);
  if (R_FAILED(Status)) {
    const Result CreateStatus = fsFsCreateFile(&SdCard, LogPath, 0, 0);
    Status = R_SUCCEEDED(CreateStatus) ? fsFsOpenFile(&SdCard, LogPath, Mode, &LogFile) : CreateStatus;
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

void WriteLog(const char* Text) {
  if (!LogOpen || LogWriteFailed) {
    return;
  }

  char Line[1024];
  const size_t Length = std::min(std::strlen(Text), sizeof(Line) - 1);
  std::memcpy(Line, Text, Length);
  Line[Length] = '\n';

  const Result Status = fsFileWrite(&LogFile, LogOffset, Line, Length + 1, FsWriteOption_Flush);
  if (R_SUCCEEDED(Status)) {
    LogOffset += Length + 1;
  } else {
    LogWriteFailed = true;
    std::printf("SD log write failed (0x%08x)\n", Status);
  }
}

void TraceGuest(const char* Stage, uint64_t RIP, uint64_t Detail) {
  char Message[160];
  std::snprintf(Message, sizeof(Message), "%s  guest RIP=%#llx  detail=%#llx", Stage,
                static_cast<unsigned long long>(RIP), static_cast<unsigned long long>(Detail));
  WriteLog(Message);
}

// The guest deliberately uses no external loader, libraries, or filesystem.
alignas(GuestPageSize) constexpr std::array<uint8_t, GuestPageSize> GuestCode {
  0x0f, 0x31,       // rdtsc
  0xbf, 7, 0, 0, 0, // mov edi, 7
  0x6b, 0xff, 6,    // imul edi, edi, 6
  0xb8, LinuxExitSyscall, 0, 0, 0, // mov eax, 60
  0x0f, 0x05,       // syscall
  0x0f, 0x0b,       // ud2
};

alignas(GuestPageSize) std::array<uint8_t, 64 * 1024> GuestStack;

void PrintStage(const char* Text);

class ProbeSyscalls final : public FEXCore::HLE::SyscallHandler {
public:
  ProbeSyscalls(std::span<const Switch2Guest::Segment> Segments, std::string Folder, std::string RootPath)
    : Folder {std::move(Folder)}
    , RootPath {std::move(RootPath)} {
    for (const auto& Segment : Segments) {
      Memory.Register(Segment.Start, Segment.Size, Segment.Flags);
    }
    Memory.Register(reinterpret_cast<uint64_t>(GuestStack.data()), GuestStack.size(), PF_R | PF_W);
    padInitializeDefault(&Input);
    padUpdate(&Input);
  }

  ~ProbeSyscalls() override {
    CloseVideo();
    for (auto& File : Files) {
      if (File && !File->Virtual) {
        fsFileClose(&File->Handle);
      }
    }
  }

  std::jmp_buf ReturnToHost;
  uint64_t Number {};
  uint64_t ExitStatus {};

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
      Number = Registers[REG_RAX];
      ExitStatus = Registers[REG_RDI] & 0xff;
      FlushGuestOutput();
      WriteLog("Guest requested exit.");
      std::longjmp(ReturnToHost, 1);
    }

    constexpr int64_t LinuxEnosys = -38;
    const auto A = Registers[REG_RDI], B = Registers[REG_RSI], C = Registers[REG_RDX];
    int64_t Result = LinuxEnosys;
    switch (Registers[REG_RAX]) {
    case 0: Result = ReadGuest(A, B, C); break;
    case LinuxWriteSyscall: Result = WriteGuest(A, B, C); break;
    case 3: Result = CloseGuest(A); break;
    case 5: Result = StatGuest(A, B); break;
    case 9: // mmap
      Result = MapGuest(Frame, A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]);
      break;
    case 10: Result = ProtectGuest(Frame, A, B, C); break;
    case 11: Result = UnmapGuest(Frame, A, B); break;
    case 12: Result = SetHeapEnd(A); break;
    case 17: Result = ReadAtGuest(A, B, C, Registers[REG_R10]); break;
    case 20: Result = WriteVectorGuest(A, B, C); break;
    case 21: Result = AccessGuest(A, B); break;
    case 158: Result = ArchPrctl(Frame, A, B); break;
    case 262: Result = StatAtGuest(A, B, C, Registers[REG_R10]); break;
    case 318: Result = RandomGuest(A, B, C); break;
    case FEX_NX2_PRESENT_RGBA: Result = PresentGuestFrame(A); break;
    case FEX_NX2_POLL_INPUT: Result = PollGameInput(); break;
    case FEX_NX2_TICKS_MS: Result = armTicksToNs(armGetSystemTick() - StartTick) / 1'000'000; break;
    case FEX_NX2_DELAY_MS: svcSleepThread(std::min<uint64_t>(A, 1000) * 1'000'000); Result = 0; break;
    case 257: // openat
      Result = OpenGuest(A, B, C);
      break;
    }
    if (Logging == LogLevel::Trace || (Logging == LogLevel::Detailed && Result < 0 && Result >= -4095)) {
      char Message[128];
      std::snprintf(Message, sizeof(Message), "Guest syscall %llu at %#llx returned %lld",
                    static_cast<unsigned long long>(Registers[REG_RAX]), static_cast<unsigned long long>(Frame->State.rip),
                    static_cast<long long>(Result));
      WriteLog(Message);
    }
    Registers[REG_RAX] = Result;
    Frame->State.rip += 2;
  }

  void PreCompile() override {
    if (Logging == LogLevel::Detailed) {
      WriteLog("FEXCore entered guest compilation.");
    }
  }

  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Address) override {
    const auto Range = Memory.Query(Address);
    if (Range.Flags & PF_X) {
      return {Range.Start, Range.Size, (Range.Flags & PF_W) != 0};
    }
    return {};
  }

  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }

private:
  static constexpr int64_t BadFile = -9;
  static constexpr int64_t Fault = -14;
  static constexpr int64_t Invalid = -22;
  struct OpenFile {
    FsFile Handle;
    s64 Offset;
    bool Virtual {};
    std::string_view Contents;
    uint64_t Inode;
  };
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
    return FD >= 3 && FD - 3 < Files.size() && Files[FD - 3] ? &*Files[FD - 3] : nullptr;
  }

  size_t Available(uint64_t Address, uint32_t Flags) const {
    const auto Range = Memory.Query(Address);
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

  int64_t StatGuest(uint64_t FD, uint64_t Output) {
    if (sizeof(LinuxStat) > Available(Output, PF_W)) return Fault;
    LinuxStat Info {};
    if (FD <= 2) {
      Info = MakeStat(0020000 | 0600, 0, FD + 1);
    } else if (auto* File = GetFile(FD)) {
      s64 Size = File->Contents.size();
      if (!File->Virtual && R_FAILED(fsFileGetSize(&File->Handle, &Size))) return -5;
      Info = MakeStat(0100000 | 0444, Size, File->Inode);
    } else {
      return BadFile;
    }
    std::memcpy(reinterpret_cast<void*>(Output), &Info, sizeof(Info));
    return 0;
  }

  int64_t StatAtGuest(uint64_t FD, uint64_t Address, uint64_t Output, uint64_t Flags) {
    constexpr uint64_t EmptyPath = 0x1000;
    constexpr uint64_t NoFollow = 0x100;
    if (Flags & ~(EmptyPath | NoFollow)) return Invalid;
    const auto Name = GuestString(Address);
    if (!Name) return Fault;
    if (Name->empty() && (Flags & EmptyPath)) return StatGuest(FD, Output);
    if (!IsCwd(FD) && !Name->starts_with('/')) return BadFile;
    if (sizeof(LinuxStat) > Available(Output, PF_W)) return Fault;
    const auto Path = Switch2Guest::ResolvePath(*Name, Folder, RootPath);
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
    const auto Info = MakeStat(Type == FsDirEntryType_Dir ? 0040555 : 0100444, Size, FileInode(*Path));
    std::memcpy(reinterpret_cast<void*>(Output), &Info, sizeof(Info));
    return 0;
  }

  int64_t AccessGuest(uint64_t Address, uint64_t Mode) {
    if ((Mode & ~7ULL) || (Mode & 2)) return -13;
    const auto Name = GuestString(Address);
    if (!Name) return Fault;
    const auto Path = Switch2Guest::ResolvePath(*Name, Folder, RootPath);
    if (!Path || Path->size() >= FS_MAX_PATH) return Invalid;
    FsFileSystem SdCard {};
    if (R_FAILED(fsOpenSdCardFileSystem(&SdCard))) return -5;
    FsDirEntryType Type {};
    const auto Status = fsFsGetEntryType(&SdCard, Path->c_str(), &Type);
    fsFsClose(&SdCard);
    return R_SUCCEEDED(Status) ? 0 : -2;
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
    if (FD != 1 && FD != 2) return BadFile;
    if (!Count) return 0;
    if (Count > 1024) return Invalid;
    if (Count * sizeof(IoVector) > Available(Address, PF_R)) return Fault;

    int64_t Total = 0;
    for (uint64_t Index = 0; Index < Count; ++Index) {
      IoVector Vector;
      std::memcpy(&Vector, reinterpret_cast<const void*>(Address + Index * sizeof(Vector)), sizeof(Vector));
      const auto Written = WriteGuest(FD, Vector.Base, Vector.Size);
      if (Written < 0) return Total ? Total : Written;
      Total += Written;
      if (static_cast<uint64_t>(Written) < Vector.Size) break;
    }
    return Total;
  }

  int64_t WriteGuest(uint64_t FD, uint64_t Address, uint64_t Count) {
    if (FD != 1 && FD != 2) {
      return BadFile;
    }
    Count = std::min<uint64_t>(Count, 4096);
    if (!Count) {
      return 0;
    }
    if (Count > Available(Address, PF_R)) {
      return Fault;
    }
    if (FD == 2 || Logging == LogLevel::Trace) {
      LogGuestOutput(reinterpret_cast<const char*>(Address), Count);
    }
    if (GraphicsActive) {
      // The console renderer shares the display with the game.
      return Count;
    }
    const size_t Written = std::fwrite(reinterpret_cast<const void*>(Address), 1, Count, stdout);
    std::fflush(stdout);
    consoleUpdate(nullptr);
    return Written;
  }

  int64_t OpenGuest(uint64_t Directory, uint64_t Address, uint64_t Flags) {
    constexpr uint64_t CloseOnExec = 02000000;
    if (!IsCwd(Directory) || (Flags & ~CloseOnExec)) {
      return Invalid;
    }
    const auto* Name = reinterpret_cast<const char*>(Address);
    const size_t Limit = std::min<size_t>(Available(Address, PF_R), FS_MAX_PATH);
    if (!Limit) {
      return Fault;
    }
    const auto* End = static_cast<const char*>(std::memchr(Name, 0, Limit));
    if (!End) {
      return Fault;
    }
    const std::string_view GuestPath {Name, static_cast<size_t>(End - Name)};
    const auto Path = Switch2Guest::ResolvePath(GuestPath, Folder, RootPath);
    if (!Path) {
      return Invalid;
    }

    const auto Slot = std::find_if(Files.begin(), Files.end(), [](const auto& File) { return !File; });
    if (Slot == Files.end()) {
      return -24; // EMFILE
    }
    if (GuestPath == "/dev/null" || GuestPath == "/proc/version" || GuestPath == "/proc/self/status") {
      const std::string_view Contents = GuestPath == "/proc/version" ? "Linux version 5.0.0 (FEX Switch 2 guest)\n" :
        GuestPath == "/proc/self/status" ? "Name:\tfex-guest\nThreads:\t1\n" : "";
      *Slot = OpenFile {{}, 0, true, Contents, FileInode(GuestPath)};
      return Slot - Files.begin() + 3;
    }
    if (Path->size() >= FS_MAX_PATH) {
      return -36; // ENAMETOOLONG
    }
    FsFileSystem SdCard {};
    if (R_FAILED(fsOpenSdCardFileSystem(&SdCard))) {
      return -5; // EIO
    }
    FsFile File {};
    const Result ResultCode = fsFsOpenFile(&SdCard, Path->c_str(), FsOpenMode_Read, &File);
    fsFsClose(&SdCard);
    if (Logging == LogLevel::Trace) {
      char Message[1024];
      std::snprintf(Message, sizeof(Message), "openat %s -> 0x%08x", Path->c_str(), ResultCode);
      WriteLog(Message);
    }
    if (R_FAILED(ResultCode)) {
      return -2; // ENOENT
    }
    *Slot = OpenFile {File, 0, false, {}, FileInode(*Path)};
    WriteLog(GuestPath.starts_with('/') ? "Guest opened a rootfs file." : "Guest opened a data file from its working directory.");
    return Slot - Files.begin() + 3;
  }

  int64_t ReadGuest(uint64_t FD, uint64_t Address, uint64_t Count) {
    if (FD != 0 && (FD < 3 || FD - 3 >= Files.size() || !Files[FD - 3])) {
      return BadFile;
    }
    if (!Count) {
      return 0;
    }
    Count = std::min<uint64_t>(Count, 65536);
    if (Count > Available(Address, PF_W)) {
      return Fault;
    }
    auto* Buffer = reinterpret_cast<char*>(Address);
    if (FD == 0) {
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
    auto& File = *GetFile(FD);
    const auto Read = ReadFileAt(File, Buffer, Count, File.Offset);
    if (Read > 0) File.Offset += Read;
    return Read;
  }

  static int64_t ReadFileAt(OpenFile& File, void* Buffer, size_t Count, uint64_t Offset) {
    if (Offset > INT64_MAX) return Invalid;
    if (File.Virtual) {
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
    if (FD < 3 || FD - 3 >= Files.size() || !Files[FD - 3]) {
      return BadFile;
    }
    if (!Files[FD - 3]->Virtual) {
      fsFileClose(&Files[FD - 3]->Handle);
    }
    Files[FD - 3].reset();
    return 0;
  }

  static uint8_t PageFlags(uint64_t Protection) {
    return ((Protection & 1) ? PF_R : 0) | ((Protection & 2) ? PF_W : 0) | ((Protection & 4) ? PF_X : 0);
  }

  void InvalidateExecutableRange(FEXCore::Core::CpuStateFrame* Frame, uint64_t Start, size_t Size, uint8_t NewFlags) {
    bool Invalidate = NewFlags & PF_X;
    for (size_t Offset = 0; !Invalidate && Offset < Size; Offset += GuestPageSize) {
      Invalidate = Memory.Query(Start + Offset).Flags & PF_X;
    }
    if (!Invalidate) return;
    auto* Thread = Frame->Thread;
    auto* Context = Thread->CTX;
    std::unique_lock Lock {Context->GetCodeInvalidationMutex()};
    Context->InvalidateCodeBuffersCodeRange(Start, Size);
    Context->InvalidateThreadCachedCodeRange(Thread, Start, Size);
  }

  bool OverlapsHeap(uint64_t Address, size_t Size) const {
    return HeapStart && Address < HeapStart + HeapLimit && HeapStart < Address + Size;
  }

  int64_t MapGuest(FEXCore::Core::CpuStateFrame* Frame, uint64_t Address, uint64_t Size, uint64_t Protection, uint64_t Flags,
                   uint64_t FD, uint64_t Offset) {
    constexpr uint64_t Private = 2, Fixed = 0x10, Anonymous = 0x20, DenyWrite = 0x800, Executable = 0x1000;
    const bool IsFixed = Flags & Fixed;
    const bool IsAnonymous = Flags & Anonymous;
    if (!Size || Size > Memory.MaxMapping || (Protection & ~7ULL) || !(Flags & Private) ||
        (Flags & ~(Private | Fixed | Anonymous | DenyWrite | Executable)) || Offset % GuestPageSize ||
        (IsAnonymous && (Offset || static_cast<uint32_t>(FD) != UINT32_MAX))) return Invalid;
    auto* File = IsAnonymous ? nullptr : GetFile(FD);
    if (!IsAnonymous && !File) return BadFile;
    const auto Rounded = Memory.RoundUp(Size);
    if (IsFixed && (Address % GuestPageSize || !Memory.Owns(Address, Rounded) || OverlapsHeap(Address, Rounded))) return Invalid;
    const auto Access = PageFlags(Protection);
    if (!IsFixed) {
      Address = Memory.Allocate(Size, Access);
      if (!Address) return -12;
      if (File && ReadFileAt(*File, reinterpret_cast<void*>(Address), Rounded, Offset) < 0) {
        Memory.Set(Address, Rounded, Memory.Unmapped, true);
        return -5;
      }
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
    Memory.Set(Address, Rounded, Access, true);
    return Address;
  }

  int64_t ProtectGuest(FEXCore::Core::CpuStateFrame* Frame, uint64_t Address, uint64_t Size, uint64_t Protection) {
    if ((Protection & ~7ULL) || Address % GuestPageSize || Size > Memory.MaxMapping) return Invalid;
    if (!Size) return 0;
    const auto Rounded = Memory.RoundUp(Size);
    if (!Memory.IsMapped(Address, Rounded)) return -12;
    const auto Access = PageFlags(Protection);
    InvalidateExecutableRange(Frame, Address, Rounded, Access);
    return Memory.Set(Address, Rounded, Access) ? 0 : Invalid;
  }

  int64_t UnmapGuest(FEXCore::Core::CpuStateFrame* Frame, uint64_t Address, uint64_t Size) {
    if (Address % GuestPageSize || !Size || Size > Memory.MaxMapping) return Invalid;
    const auto Rounded = Memory.RoundUp(Size);
    if (!Memory.Owns(Address, Rounded) || OverlapsHeap(Address, Rounded)) return Invalid;
    InvalidateExecutableRange(Frame, Address, Rounded, 0);
    return Memory.Set(Address, Rounded, Memory.Unmapped, true) ? 0 : Invalid;
  }

  int64_t SetHeapEnd(uint64_t Requested) {
    if (!HeapStart) {
      HeapStart = Memory.Allocate(HeapLimit, 0);
      HeapEnd = HeapStart;
    }
    if (!HeapStart || Requested < HeapStart || Requested - HeapStart > HeapLimit) return HeapEnd;
    const size_t OldSize = Memory.RoundUp(HeapEnd - HeapStart);
    const size_t NewSize = Memory.RoundUp(Requested - HeapStart);
    if (NewSize > OldSize) {
      std::memset(reinterpret_cast<void*>(HeapStart + OldSize), 0, NewSize - OldSize);
      Memory.Set(HeapStart + OldSize, NewSize - OldSize, PF_R | PF_W, true);
    } else if (NewSize < OldSize) {
      Memory.Set(HeapStart + NewSize, OldSize - NewSize, 0, true);
    }
    HeapEnd = Requested;
    return HeapEnd;
  }

  static constexpr size_t HeapLimit = 8 * 1024 * 1024;
  uint64_t HeapStart {};
  uint64_t HeapEnd {};
  Switch2Guest::GuestAddressSpace Memory;
  std::string Folder;
  std::string RootPath;
  std::string OutputLog;
  std::array<std::optional<OpenFile>, 16> Files {};
  PadState Input {};
  const u64 StartTick {armGetSystemTick()};
  Framebuffer Video {};
  bool Presented {};

};

void PrintStage(const char* Text) {
  WriteLog(Text);
  if (!GraphicsActive) {
    std::puts(Text);
    consoleUpdate(nullptr);
  }
}

std::optional<uint64_t> RunGuest(uint64_t Entry, std::span<const Switch2Guest::Segment> Segments,
                                 const std::string& Folder = "/", std::string_view Program = "probe", std::string_view Argument = {},
                                 const Switch2Guest::ElfImage* ProgramImage = nullptr, uint64_t InterpreterBase = 0,
                                 const std::string& RootPath = Switch2Guest::Root) {
  using namespace FEXCore;
  Config::Set(Config::CONFIG_IS64BIT_MODE, "1");
  Config::Set(Config::CONFIG_DISKCACHE, "0");
  Config::Set(Config::CONFIG_DISABLE_VIXL_INDIRECT_RUNTIME_CALLS, "1");

  HostFeatures Features {};
  Features.ProcessFeatures.Is64BitMode = true;
  Features.DCacheLineLog2 = 4;
  SignalDelegator Signals;
  auto Syscalls = fextl::make_unique<ProbeSyscalls>(Segments, Folder, RootPath);

  PrintStage("Creating FEXCore context...");
  auto Context = Context::Context::CreateNewContext(Features);
  Context->SetSignalDelegator(&Signals);
  Context->SetSyscallHandler(Syscalls.get());
  PrintStage("Initializing FEXCore dispatcher...");
  if (!Context->InitCore()) {
    return std::nullopt;
  }

  PrintStage("Creating guest thread...");
  std::array<Core::CPUState::gdt_segment, 32> GDT {};
  Core::CPUState State {};
  State.segment_arrays[Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = GDT.data();
  State.segment_arrays[Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = GDT.data();
  State.cs_idx = Core::CPUState::DEFAULT_USER_CS << 3;
  auto* CodeSegment = Core::CPUState::GetSegmentFromIndex(State, State.cs_idx);
  Core::CPUState::SetGDTLimit(CodeSegment, 0xF'FFFFU);
  CodeSegment->L = 1;
  State.rip = Entry;
  // Linux initial stack with argv, an empty environment, and AT_NULL.
  char* Strings = reinterpret_cast<char*>(GuestStack.data() + GuestStack.size());
  auto PushString = [&](std::string_view Text) {
    Strings -= Text.size() + 1;
    std::memcpy(Strings, Text.data(), Text.size());
    Strings[Text.size()] = 0;
    return reinterpret_cast<uint64_t>(Strings);
  };
  const auto ProgramAddress = PushString(Program);
  const auto ArgumentAddress = Argument.empty() ? 0 : PushString(Argument);
  std::vector<uint64_t> InitialStack {ArgumentAddress ? 2u : 1u, ProgramAddress};
  if (ArgumentAddress) {
    InitialStack.push_back(ArgumentAddress);
  }
  InitialStack.insert(InitialStack.end(), {0, 0}); // argv and envp terminators
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
  State.gregs[X86State::REG_RSP] = Stack;

  constexpr auto ReturnStackSize = Core::InternalThreadState::CALLRET_STACK_SIZE;
  std::unique_ptr<void, decltype(&std::free)> ReturnStack {
    std::aligned_alloc(GuestPageSize, ReturnStackSize), &std::free};
  if (!ReturnStack) {
    PrintStage("Could not allocate the guest return stack.");
    return std::nullopt;
  }
  std::memset(ReturnStack.get(), 0, ReturnStackSize);
  State.callret_sp = reinterpret_cast<uint64_t>(ReturnStack.get()) + ReturnStackSize - 16;
  auto* Thread = Context->CreateThread(&State);
  Thread->CallRetStackBase = ReturnStack.get();

  if (Logging == LogLevel::Trace) {
    TraceGuest("Program image", ProgramImage ? ProgramImage->Entry : Entry, ProgramImage ? ProgramImage->Base : Entry);
    TraceGuest("Interpreter start", Entry, InterpreterBase);
    FEXCore::Switch2::TraceHandler = TraceGuest;
  }
  PrintStage("Running x86-64 guest...");
  const bool GuestExited = setjmp(Syscalls->ReturnToHost) != 0;
  if (!GuestExited) {
    Context->ExecuteThread(Thread);
  }
  FEXCore::Switch2::TraceHandler = nullptr;
  Syscalls->CloseVideo();
  if (GuestExited) {
    char ResultText[96];
    std::snprintf(ResultText, sizeof(ResultText), "Guest syscall %llu, exit status %llu",
                  static_cast<unsigned long long>(Syscalls->Number), static_cast<unsigned long long>(Syscalls->ExitStatus));
    PrintStage(ResultText);
  } else {
    PrintStage("Dispatcher returned before the guest reached its syscall.");
  }
  PrintStage("Destroying guest thread...");
  Context->DestroyThread(Thread);
  return GuestExited ? std::optional {Syscalls->ExitStatus} : std::nullopt;
}

const char* ReadElf(const char* Path, Switch2Guest::ElfImage& Image) {
  FsFileSystem SdCard {};
  if (R_FAILED(fsOpenSdCardFileSystem(&SdCard))) {
    return "Could not open the SD card.";
  }
  FsFile File {};
  Result Status = fsFsOpenFile(&SdCard, Path, FsOpenMode_Read, &File);
  fsFsClose(&SdCard);
  if (R_FAILED(Status)) {
    return "Could not open the selected file.";
  }

  s64 Size {};
  Status = fsFileGetSize(&File, &Size);
  if (R_FAILED(Status) || Size <= 0 || Size > Switch2Guest::MaxElfSize) {
    fsFileClose(&File);
    return "Invalid or unreadable ELF size.";
  }
  std::unique_ptr<uint8_t, decltype(&std::free)> Bytes {
    static_cast<uint8_t*>(std::malloc(Size)), &std::free};
  if (!Bytes) {
    fsFileClose(&File);
    return "Could not allocate the ELF read buffer.";
  }
  u64 Read {};
  Status = fsFileRead(&File, 0, Bytes.get(), Size, FsReadOption_None, &Read);
  fsFileClose(&File);
  if (R_FAILED(Status) || Read != static_cast<u64>(Size)) {
    return "Could not read the complete ELF.";
  }

  return Switch2Guest::LoadElf({Bytes.get(), static_cast<size_t>(Size)}, Image);
}

struct DirectoryEntry {
  std::string Name;
  bool IsDirectory;
};

std::string DisplayName(std::string Text) {
  for (auto& Character : Text) {
    if (static_cast<unsigned char>(Character) < 32 || Character == 127) {
      Character = '?';
    }
  }
  constexpr size_t MaxWidth = 64;
  if (Text.size() > MaxWidth) {
    Text.resize(MaxWidth - 3);
    Text += "...";
  }
  return Text;
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

class Launcher {
public:
  void LoadSettings() {
    if (!fsdevGetDeviceFileSystem("sdmc")) {
      fsdevMountSdmc();
    }
    auto* File = std::fopen(SettingsPath, "r");
    if (!File) {
      if (errno == ENOENT) {
        SaveSettings();
      }
      return;
    }
    const int StoredLevel = std::fgetc(File);
    Logging = StoredLevel == '2' ? LogLevel::Trace : StoredLevel == '1' ? LogLevel::Detailed : LogLevel::Standard;
    if (std::fgetc(File) == '\n') {
      StartFolder = ReadSavedFolder(File, DefaultFolder);
    }
    std::fclose(File);
  }

  void Draw() const {
    consoleClear();
    switch (Page) {
    case Screen::Home:
      std::puts("FEX for Switch 2\n");
      if (!LogOpen) {
        std::puts("SD setup failed. Check /fex.");
      }
      DrawItem(0, "Browse ELF files");
      DrawItem(1, "Settings");
      if (!LastResult.empty()) {
        std::printf("\nLast run  %s\n", LastResult.c_str());
      }
      std::puts("\nA Select    + Exit");
      break;
    case Screen::Browse: {
      std::printf("Browse ELF files\nSD %s\n\n", DisplayName(Folder).c_str());
      const size_t First = Selected / VisibleRows * VisibleRows;
      for (size_t Index = First; Index < std::min(First + VisibleRows, Entries.size()); ++Index) {
        const auto& Entry = Entries[Index];
        const auto Label = (Entry.IsDirectory ? "[Folder] " : "") + DisplayName(Entry.Name);
        DrawItem(Index, Label.c_str());
      }
      if (Entries.empty()) {
        std::puts("This folder is empty.");
      }
      if (!Notice.empty()) {
        std::printf("\n%s\n", Notice.c_str());
      }
      std::puts("\nA Open / inspect    B Back    Y Home");
      break;
    }
    case Screen::Details:
      std::printf("Launch ELF\n\n%s\n\n", DisplayName(SelectedFile).c_str());
      std::puts(LoadError ? LoadError : Image.Interpreter.empty() ? "x86-64 Linux static PIE" : "x86-64 Linux PIE with an ELF interpreter");
      std::puts(LoadError ? "\nB Back" : "\nA Run    B Back");
      break;
    case Screen::Settings:
      std::puts("Settings\n");
      DrawItem(0, ("Start folder  " + DisplayName(StartFolder)).c_str());
      DrawItem(1, LogLevelLabels[static_cast<int>(Logging)]);
      DrawItem(2, "Diagnostics");
      if (!Notice.empty()) {
        std::printf("\n%s\n", Notice.c_str());
      }
      std::puts("\nA Change / open    B Back");
      break;
    case Screen::Diagnostics:
      std::puts("Diagnostics\n");
      DrawItem(0, "Embedded x86-64 guest");
      DrawItem(1, "ARM64 JIT publication");
      std::puts("\nA Run    B Back");
      break;
    case Screen::Result:
      break;
    }
  }

  bool Update(u64 Buttons) {
    switch (Page) {
    case Screen::Home:
      if (Buttons & HidNpadButton_A) {
        if (Selected == 0) {
          OpenFolder(LastFolder.empty() ? StartFolder : LastFolder);
        } else {
          Open(Screen::Settings);
        }
        return true;
      }
      return Move(Buttons, 2);
    case Screen::Browse:
      return UpdateBrowser(Buttons);
    case Screen::Details:
      if (Buttons & HidNpadButton_B) {
        Image = {};
        Interpreter = {};
        BackToBrowser();
        return true;
      }
      if ((Buttons & HidNpadButton_A) && !LoadError) {
        RunSelectedElf();
      }
      return false;
    case Screen::Settings:
      return UpdateSettings(Buttons);
    case Screen::Diagnostics:
      if (Buttons & HidNpadButton_B) {
        Open(Screen::Settings);
        return true;
      }
      if (Buttons & HidNpadButton_A) {
        RunDiagnostic();
      } else {
        return Move(Buttons, 2);
      }
      return false;
    case Screen::Result:
      if (Buttons & HidNpadButton_Y) {
        Open(Screen::Home);
        return true;
      }
      if (Buttons & HidNpadButton_B) {
        if (ResultFromElf) {
          BackToBrowser();
        } else {
          Open(Screen::Diagnostics);
        }
        return true;
      }
      if (ResultFromElf && (Buttons & HidNpadButton_A)) {
        InspectSelectedElf();
        if (LoadError) {
          return true;
        }
        RunSelectedElf();
      }
      return false;
    }
    return false;
  }

private:
  enum class Screen { Home, Browse, Details, Settings, Diagnostics, Result };
  static constexpr size_t VisibleRows = 12;

  static std::string ReadSavedFolder(FILE* File, const std::string& Fallback) {
    char Path[FS_MAX_PATH];
    if (!std::fgets(Path, sizeof(Path), File)) {
      return Fallback;
    }
    Path[strcspn(Path, "\r\n")] = 0;
    const std::string_view Saved {Path};
    if (Saved == "/switch/fex" || Saved.starts_with("/switch/fex/")) {
      return std::string {Saved.substr(sizeof("/switch") - 1)};
    }
    return Saved.starts_with('/') ? std::string {Saved} : Fallback;
  }

  void SaveSettings() {
    auto* File = std::fopen(SettingsPath, "w");
    if (!File) {
      Notice = "Could not save settings to the SD card.";
      return;
    }
    const int Written = std::fprintf(File, "%d\n%s\n", static_cast<int>(Logging), StartFolder.c_str());
    if (std::fclose(File) != 0 || Written < 0) {
      Notice = "Could not save settings to the SD card.";
    }
  }

  void DrawItem(size_t Index, const char* Label) const {
    std::printf("%s %s\n", Selected == Index ? ">" : " ", Label);
  }

  void Open(Screen Target) {
    Page = Target;
    Selected = 0;
    Notice.clear();
  }

  bool Move(u64 Buttons, size_t Count) {
    if (Count == 0) {
      return false;
    }
    if (Buttons & HidNpadButton_AnyUp) {
      Selected = Selected == 0 ? Count - 1 : Selected - 1;
      return true;
    }
    if (Buttons & HidNpadButton_AnyDown) {
      Selected = (Selected + 1) % Count;
      return true;
    }
    return false;
  }

  void OpenFolder(std::string Target) {
    Open(Screen::Browse);
    Entries.clear();
    if (R_FAILED(ReadDirectory(Target, Entries))) {
      Entries.clear();
      if (Target != "/" && R_SUCCEEDED(ReadDirectory("/", Entries))) {
        Notice = "Folder unavailable. Showing the SD card root.";
        Target = "/";
      } else {
        Entries.clear();
        Notice = "Could not read the SD card.";
      }
    }
    Folder = std::move(Target);
    LastFolder = Folder;
  }

  void BackToBrowser() {
    Open(Screen::Browse);
    Selected = std::min(BrowserSelection, Entries.empty() ? size_t {0} : Entries.size() - 1);
  }

  bool UpdateBrowser(u64 Buttons) {
    if (Buttons & HidNpadButton_Y) {
      Open(Screen::Home);
      return true;
    }
    if (Buttons & HidNpadButton_B) {
      if (Folder == "/") {
        Open(Screen::Home);
      } else {
        const auto Slash = Folder.rfind('/');
        OpenFolder(Slash == 0 ? "/" : Folder.substr(0, Slash));
      }
      return true;
    }
    if ((Buttons & HidNpadButton_A) && !Entries.empty()) {
      const auto& Entry = Entries[Selected];
      auto Path = Folder + (Folder == "/" ? "" : "/") + Entry.Name;
      if (Path.size() >= FS_MAX_PATH) {
        Notice = "Path is too long.";
      } else if (Entry.IsDirectory) {
        OpenFolder(std::move(Path));
      } else {
        SelectedFile = std::move(Path);
        BrowserSelection = Selected;
        InspectSelectedElf();
      }
      return true;
    }
    return Move(Buttons, Entries.size());
  }

  bool UpdateSettings(u64 Buttons) {
    if (Buttons & HidNpadButton_B) {
      Open(Screen::Home);
      return true;
    }
    if (!(Buttons & HidNpadButton_A)) {
      return Move(Buttons, 3);
    }
    Notice.clear();
    switch (Selected) {
    case 0:
      // Cycle between the default, the SD root, and the current browser folder.
      StartFolder = StartFolder == DefaultFolder ? "/" : StartFolder == "/" && Folder != "/" ? Folder : DefaultFolder;
      LastFolder.clear();
      SaveSettings();
      break;
    case 1:
      Logging = static_cast<LogLevel>((static_cast<int>(Logging) + 1) % 3);
      SaveSettings();
      break;
    case 2:
      Open(Screen::Diagnostics);
      break;
    }
    return true;
  }

  void InspectSelectedElf() {
    Image = {};
    Interpreter = {};
    LoadError = ReadElf(SelectedFile.c_str(), Image);
    SelectedRoot = Switch2Guest::Root;
    if (!LoadError && !Image.Interpreter.empty()) {
      const std::string Sidecar = "sdmc:" + SelectedFile + ".rootfs";
      if (auto* File = std::fopen(Sidecar.c_str(), "r")) {
        char Buffer[80] {};
        const bool Read = std::fgets(Buffer, sizeof(Buffer), File) != nullptr;
        const bool Complete = Read && (std::strchr(Buffer, '\n') || std::feof(File));
        std::fclose(File);
        Buffer[strcspn(Buffer, "\r\n")] = 0;
        const std::string_view Name {Buffer};
        if (!Complete || Name.empty() ||
            Name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != Name.npos) {
          LoadError = "Invalid .rootfs sidecar. Use a rootfs folder name.";
          Open(Screen::Details);
          return;
        }
        SelectedRoot += '/';
        SelectedRoot += Name;
      } else if (errno != ENOENT) {
        LoadError = "Could not read the .rootfs sidecar.";
        Open(Screen::Details);
        return;
      }
      const auto Path = Switch2Guest::ResolvePath(Image.Interpreter, "/", SelectedRoot);
      LoadError = Path ? ReadElf(Path->c_str(), Interpreter) : "Invalid interpreter path.";
      if (!LoadError && !Interpreter.Interpreter.empty()) {
        LoadError = "An ELF interpreter cannot have its own interpreter.";
      }
    }
    Open(Screen::Details);
  }

  void RunSelectedElf() {
    consoleClear();
    PrintStage(DisplayName(SelectedFile).c_str());
    std::string Argument;
    const auto ArgumentsPath = "sdmc:" + SelectedFile + ".args";
    if (auto* File = std::fopen(ArgumentsPath.c_str(), "r")) {
      char Line[FS_MAX_PATH];
      if (std::fgets(Line, sizeof(Line), File)) {
        Line[strcspn(Line, "\r\n")] = 0;
        Argument = Line;
      }
      std::fclose(File);
    }
    std::vector<Switch2Guest::Segment> Segments(Image.GetSegments().begin(), Image.GetSegments().end());
    const auto InterpreterSegments = Interpreter.GetSegments();
    Segments.insert(Segments.end(), InterpreterSegments.begin(), InterpreterSegments.end());
    if (Interpreter.Entry) {
      PrintStage(("Guest rootfs " + SelectedRoot).c_str());
      PrintStage(("Starting interpreter " + Image.Interpreter).c_str());
    }
    const auto Exit = RunGuest(Interpreter.Entry ? Interpreter.Entry : Image.Entry, Segments, Folder, SelectedFile, Argument,
                               &Image, Interpreter.Base, SelectedRoot);
    Image = {};
    Interpreter = {};
    LastResult = Exit ? "Exited with status " + std::to_string(*Exit) : "Guest did not reach exit.";
    PrintStage(LastResult.c_str());
    std::puts("\nA Retry    B Browser    Y Home");
    ResultFromElf = true;
    Page = Screen::Result;
  }

  void RunDiagnostic() {
    consoleClear();
    bool Passed;
    if (Selected == 0) {
      const Switch2Guest::Segment Segment {reinterpret_cast<uint64_t>(GuestCode.data()), GuestCode.size(), PF_R | PF_X};
      Passed = RunGuest(Segment.Start, {&Segment, 1}) == ExpectedExitStatus;
      LastResult = Passed ? "Embedded x86-64 guest passed." : "Embedded x86-64 guest failed.";
    } else {
      PrintStage("Running ARM64 JIT publication probe...");
      Passed = RunSwitch2JitProbe();
      LastResult = Passed ? "ARM64 JIT probe passed." : "ARM64 JIT probe failed.";
    }
    PrintStage(LastResult.c_str());
    std::puts("\nB Diagnostics    Y Home");
    ResultFromElf = false;
    Page = Screen::Result;
  }

  Screen Page {Screen::Home};
  size_t Selected {};
  size_t BrowserSelection {};
  std::string StartFolder {DefaultFolder};
  std::string LastFolder;
  std::string Folder {"/"};
  std::string SelectedFile;
  std::string SelectedRoot;
  std::string Notice;
  std::string LastResult;
  std::vector<DirectoryEntry> Entries;
  Switch2Guest::ElfImage Image;
  Switch2Guest::ElfImage Interpreter;
  const char* LoadError {};
  bool ResultFromElf {};
};
} // namespace

int main() {
  const Result LogStatus = OpenLog();
  WriteLog("--- FEX Switch 2 launcher 0.8.4 started ---");
  WriteLog("Initializing console...");
  consoleInit(nullptr);
  if (R_FAILED(LogStatus)) {
    std::printf("Could not open SD log %s (0x%08x)\n", LogPath, LogStatus);
  }
  WriteLog("Initializing FEXCore configuration...");
  FEXCore::Config::Initialize();
  LogMan::Msg::InstallHandler([](LogMan::DebugLevels, const char* Message) { PrintStage(Message); });
  LogMan::Throw::InstallHandler(PrintStage);

  WriteLog("Initializing controller input...");
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState Pad {};
  padInitializeDefault(&Pad);
  Launcher Menu;
  Menu.LoadSettings();
  Menu.Draw();

  while (appletMainLoop()) {
    padUpdate(&Pad);
    const auto Buttons = padGetButtonsDown(&Pad);
    if (Buttons & HidNpadButton_Plus) {
      break;
    }
    if (Menu.Update(Buttons)) {
      Menu.Draw();
    }
    consoleUpdate(nullptr);
  }

  WriteLog("Closing guest probe.");
  LogMan::Throw::UnInstallHandler();
  LogMan::Msg::UnInstallHandler();
  FEXCore::Config::Shutdown();
  consoleExit(nullptr);
  if (LogOpen) {
    fsFileClose(&LogFile);
  }
  return 0;
}
