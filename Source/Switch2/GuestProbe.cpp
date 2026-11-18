// SPDX-License-Identifier: MIT
#include "GuestElf.h"
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
constexpr char DefaultFolder[] = "/switch/fex/games";
constexpr char SettingsPath[] = "sdmc:/switch/fex/fex-switch2.cfg";
constexpr char LogPath[] = "/switch/fex/fex-switch2-guest.log";

FsFile LogFile {};
s64 LogOffset {};
bool LogOpen {};
bool LogWriteFailed {};
bool DetailedLogging {};
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

  for (const char* Path : {"/switch", "/switch/fex", DefaultFolder}) {
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

// The guest deliberately uses no external loader, libraries, or filesystem.
alignas(GuestPageSize) constexpr std::array<uint8_t, GuestPageSize> GuestCode {
  0xbf, 7, 0, 0, 0, // mov edi, 7
  0x6b, 0xff, 6,    // imul edi, edi, 6
  0xb8, LinuxExitSyscall, 0, 0, 0, // mov eax, 60
  0x0f, 0x05,       // syscall
  0x0f, 0x0b,       // ud2
};

alignas(16) std::array<uint8_t, 64 * 1024> GuestStack;

void PrintStage(const char* Text);

class ProbeSyscalls final : public FEXCore::HLE::SyscallHandler {
public:
  ProbeSyscalls(std::span<const Switch2Guest::Segment> Segments, std::string Folder)
    : Segments {Segments}
    , Folder {std::move(Folder)} {
    padInitializeDefault(&Input);
    padUpdate(&Input);
  }

  ~ProbeSyscalls() override {
    CloseVideo();
    for (auto& File : Files) {
      if (File) {
        fsFileClose(&File->Handle);
      }
    }
    for (const auto& Mapping : Mappings) {
      std::free(Mapping.Ptr);
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
    if (Registers[REG_RAX] == LinuxExitSyscall) {
      Number = LinuxExitSyscall;
      ExitStatus = Registers[REG_RDI] & 0xff;
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
    case 9: // mmap
      Result = MapGuest(A, B, C, Registers[REG_R10], Registers[REG_R8], Registers[REG_R9]);
      break;
    case 11: Result = UnmapGuest(A, B); break;
    case FEX_NX2_PRESENT_RGBA: Result = PresentGuestFrame(A); break;
    case FEX_NX2_POLL_INPUT: Result = PollGameInput(); break;
    case FEX_NX2_TICKS_MS: Result = armTicksToNs(armGetSystemTick() - StartTick) / 1'000'000; break;
    case FEX_NX2_DELAY_MS: svcSleepThread(std::min<uint64_t>(A, 1000) * 1'000'000); Result = 0; break;
    case 257: // openat
      Result = OpenGuest(A, B, C);
      break;
    }
    Registers[REG_RAX] = Result;
    Frame->State.rip += 2;
  }

  void PreCompile() override {
    if (DetailedLogging) {
      WriteLog("FEXCore entered guest compilation.");
    }
  }

  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Address) override {
    for (const auto& Segment : Segments) {
      if ((Segment.Flags & PF_X) && Address >= Segment.Start && Address - Segment.Start < Segment.Size) {
        return {Segment.Start, Segment.Size, (Segment.Flags & PF_W) != 0};
      }
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
  };
  struct Mapping {
    void* Ptr;
    size_t Size;
    uint32_t Flags;
  };

  size_t Available(uint64_t Address, uint32_t Flags) const {
    const auto InRange = [&](uint64_t Start, uint64_t Size, uint32_t Access) -> size_t {
      return (Access & Flags) == Flags && Address >= Start && Address - Start < Size ? Size - (Address - Start) : 0;
    };
    for (const auto& Segment : Segments) {
      if (const auto Size = InRange(Segment.Start, Segment.Size, Segment.Flags)) {
        return Size;
      }
    }
    if (const auto Size = InRange(reinterpret_cast<uint64_t>(GuestStack.data()), GuestStack.size(), PF_R | PF_W)) {
      return Size;
    }
    for (const auto& Map : Mappings) {
      if (const auto Size = InRange(reinterpret_cast<uint64_t>(Map.Ptr), Map.Size, Map.Flags)) {
        return Size;
      }
    }
    return 0;
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
    constexpr uint64_t CurrentDirectory = static_cast<uint64_t>(-100);
    constexpr uint64_t CloseOnExec = 02000000;
    if ((Directory != CurrentDirectory && Directory != static_cast<uint32_t>(CurrentDirectory)) || (Flags & ~CloseOnExec)) {
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
    const std::string_view Relative {Name, static_cast<size_t>(End - Name)};
    if (Relative.empty() || Relative.starts_with('/') || Relative.find_first_of(":\\") != Relative.npos) {
      return Invalid;
    }
    auto Remaining = Relative;
    while (!Remaining.empty()) {
      const auto Slash = Remaining.find('/');
      const auto Component = Remaining.substr(0, Slash);
      if (Component.empty() || Component == "." || Component == "..") {
        return Invalid;
      }
      if (Slash == Remaining.npos) {
        break;
      }
      Remaining.remove_prefix(Slash + 1);
    }

    const auto Slot = std::find_if(Files.begin(), Files.end(), [](const auto& File) { return !File; });
    if (Slot == Files.end()) {
      return -24; // EMFILE
    }
    const auto Path = Folder + (Folder == "/" ? "" : "/") + std::string {Relative};
    if (Path.size() >= FS_MAX_PATH) {
      return -36; // ENAMETOOLONG
    }
    FsFileSystem SdCard {};
    if (R_FAILED(fsOpenSdCardFileSystem(&SdCard))) {
      return -5; // EIO
    }
    FsFile File {};
    const Result ResultCode = fsFsOpenFile(&SdCard, Path.c_str(), FsOpenMode_Read, &File);
    fsFsClose(&SdCard);
    if (R_FAILED(ResultCode)) {
      return -2; // ENOENT
    }
    *Slot = OpenFile {File, 0};
    WriteLog("Guest opened a data file from its working directory.");
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
    auto& File = *Files[FD - 3];
    u64 Read {};
    if (R_FAILED(fsFileRead(&File.Handle, File.Offset, Buffer, Count, FsReadOption_None, &Read))) {
      return -5; // EIO
    }
    File.Offset += Read;
    return Read;
  }

  int64_t CloseGuest(uint64_t FD) {
    if (FD < 3 || FD - 3 >= Files.size() || !Files[FD - 3]) {
      return BadFile;
    }
    fsFileClose(&Files[FD - 3]->Handle);
    Files[FD - 3].reset();
    return 0;
  }

  int64_t MapGuest(uint64_t Address, uint64_t Size, uint64_t Protection, uint64_t Flags, uint64_t FD, uint64_t Offset) {
    constexpr uint64_t AnonymousPrivate = 0x22;
    if (Address || !Size || Size > 1024 * 1024 || (Protection & ~3ULL) ||
        Flags != AnonymousPrivate || static_cast<uint32_t>(FD) != UINT32_MAX || Offset) {
      return Invalid;
    }
    const auto Slot = std::find_if(Mappings.begin(), Mappings.end(), [](const auto& Map) { return !Map.Ptr; });
    if (Slot == Mappings.end()) {
      return -12; // ENOMEM
    }
    const auto Rounded = (Size + GuestPageSize - 1) & ~(GuestPageSize - 1);
    auto* Memory = std::aligned_alloc(GuestPageSize, Rounded);
    if (!Memory) {
      return -12; // ENOMEM
    }
    std::memset(Memory, 0, Rounded);
    *Slot = {Memory, Rounded, static_cast<uint32_t>(((Protection & 1) ? PF_R : 0) | ((Protection & 2) ? PF_W : 0))};
    return reinterpret_cast<int64_t>(Memory);
  }

  int64_t UnmapGuest(uint64_t Address, uint64_t Size) {
    for (auto& Map : Mappings) {
      if (Map.Ptr && Address == reinterpret_cast<uint64_t>(Map.Ptr) && Size && Size <= Map.Size && Map.Size - Size < GuestPageSize) {
        std::free(Map.Ptr);
        Map = {};
        return 0;
      }
    }
    return Invalid;
  }

  std::span<const Switch2Guest::Segment> Segments;
  std::string Folder;
  std::array<std::optional<OpenFile>, 16> Files {};
  std::array<Mapping, 8> Mappings {};
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
                                 const std::string& Folder = "/", std::string_view Program = "probe", std::string_view Argument = {}) {
  using namespace FEXCore;
  Config::Set(Config::CONFIG_IS64BIT_MODE, "1");
  Config::Set(Config::CONFIG_DISKCACHE, "0");
  Config::Set(Config::CONFIG_DISABLE_VIXL_INDIRECT_RUNTIME_CALLS, "1");

  HostFeatures Features {};
  Features.ProcessFeatures.Is64BitMode = true;
  Features.DCacheLineLog2 = 4;
  SignalDelegator Signals;
  auto Syscalls = fextl::make_unique<ProbeSyscalls>(Segments, Folder);

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
  const std::array<uint64_t, 7> InitialStack {ArgumentAddress ? 2u : 1u, ProgramAddress, ArgumentAddress};
  const auto Stack = (reinterpret_cast<uint64_t>(Strings) - sizeof(InitialStack)) & ~15ULL;
  std::memcpy(reinterpret_cast<void*>(Stack), InitialStack.data(), sizeof(InitialStack));
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

  PrintStage("Running x86-64 guest...");
  const bool GuestExited = setjmp(Syscalls->ReturnToHost) != 0;
  if (!GuestExited) {
    Context->ExecuteThread(Thread);
  }
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
    DetailedLogging = std::fgetc(File) == '1';
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
        std::puts("SD setup failed. Check /switch/fex.");
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
      std::puts(LoadError ? LoadError : "x86-64 Linux static PIE");
      std::puts(LoadError ? "\nB Back" : "\nA Run    B Back");
      break;
    case Screen::Settings:
      std::puts("Settings\n");
      DrawItem(0, ("Start folder  " + DisplayName(StartFolder)).c_str());
      DrawItem(1, DetailedLogging ? "Logging  Detailed" : "Logging  Standard");
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
    return Path[0] == '/' ? Path : Fallback;
  }

  void SaveSettings() {
    auto* File = std::fopen(SettingsPath, "w");
    if (!File) {
      Notice = "Could not save settings to the SD card.";
      return;
    }
    const int Written = std::fprintf(File, "%d\n%s\n", DetailedLogging, StartFolder.c_str());
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
      DetailedLogging = !DetailedLogging;
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
    LoadError = ReadElf(SelectedFile.c_str(), Image);
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
    const auto Exit = RunGuest(Image.Entry, Image.GetSegments(), Folder, SelectedFile, Argument);
    Image = {};
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
  std::string Notice;
  std::string LastResult;
  std::vector<DirectoryEntry> Entries;
  Switch2Guest::ElfImage Image;
  const char* LoadError {};
  bool ResultFromElf {};
};
} // namespace

int main() {
  const Result LogStatus = OpenLog();
  WriteLog("--- FEX Switch 2 launcher 0.6.0 started ---");
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
