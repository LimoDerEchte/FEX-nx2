// SPDX-License-Identifier: MIT
#include "GuestElf.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
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

alignas(16) std::array<uint8_t, GuestPageSize> GuestStack;

void PrintStage(const char* Text);

class ProbeSyscalls final : public FEXCore::HLE::SyscallHandler {
public:
  explicit ProbeSyscalls(std::span<const Switch2Guest::Segment> Segments)
    : Segments {Segments} {}

  std::jmp_buf ReturnToHost;
  uint64_t Number {};
  uint64_t ExitStatus {};

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
    Registers[REG_RAX] = Registers[REG_RAX] == LinuxWriteSyscall ?
      WriteGuest(Registers[REG_RDI], Registers[REG_RSI], Registers[REG_RDX]) : LinuxEnosys;
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
  int64_t WriteGuest(uint64_t FD, uint64_t Address, uint64_t Count) {
    constexpr int64_t LinuxEbadf = -9;
    constexpr int64_t LinuxEfault = -14;
    if (FD != 1 && FD != 2) {
      return LinuxEbadf;
    }
    if (Count == 0) {
      return 0;
    }
    for (const auto& Segment : Segments) {
      if ((Segment.Flags & PF_R) && Address >= Segment.Start && Address - Segment.Start < Segment.Size &&
          Count <= Segment.Size - (Address - Segment.Start)) {
        const auto* Data = reinterpret_cast<const char*>(Address);
        const size_t Written = std::fwrite(Data, 1, std::min<uint64_t>(Count, 4096), stdout);
        std::fflush(stdout);
        WriteLog("Guest wrote to the console.");
        consoleUpdate(nullptr);
        return Written;
      }
    }
    return LinuxEfault;
  }

  std::span<const Switch2Guest::Segment> Segments;
};

void PrintStage(const char* Text) {
  WriteLog(Text);
  std::puts(Text);
  consoleUpdate(nullptr);
}

std::optional<uint64_t> RunGuest(uint64_t Entry, std::span<const Switch2Guest::Segment> Segments) {
  using namespace FEXCore;
  Config::Set(Config::CONFIG_IS64BIT_MODE, "1");
  Config::Set(Config::CONFIG_DISKCACHE, "0");
  Config::Set(Config::CONFIG_DISABLE_VIXL_INDIRECT_RUNTIME_CALLS, "1");

  HostFeatures Features {};
  Features.ProcessFeatures.Is64BitMode = true;
  Features.DCacheLineLog2 = 4;
  SignalDelegator Signals;
  auto Syscalls = fextl::make_unique<ProbeSyscalls>(Segments);

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
  // Empty Linux initial stack: argc, argv terminator, envp terminator, AT_NULL pair.
  std::fill(GuestStack.end() - 48, GuestStack.end(), 0);
  State.gregs[X86State::REG_RSP] = reinterpret_cast<uint64_t>(GuestStack.data() + GuestStack.size() - 48);
  auto* Thread = Context->CreateThread(&State);

  PrintStage("Running x86-64 guest...");
  const bool GuestExited = setjmp(Syscalls->ReturnToHost) != 0;
  if (!GuestExited) {
    Context->ExecuteThread(Thread);
  }
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
    const auto Exit = RunGuest(Image.Entry, Image.GetSegments());
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
  WriteLog("--- FEX Switch 2 launcher 0.3.1 started ---");
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
