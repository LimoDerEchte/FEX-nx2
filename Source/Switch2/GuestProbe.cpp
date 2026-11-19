// SPDX-License-Identifier: MIT
#include "GuestRuntime.h"
#include "GuestRootFS.h"
#include "GuestLogBuffer.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Utils/LogManager.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <elf.h>

bool RunSwitch2JitProbe();

namespace Switch2Guest {
constexpr size_t GuestPageSize = Switch2Guest::PageSize;
constexpr uint64_t ExpectedExitStatus = 42;
constexpr uint64_t LinuxExitSyscall = 60;
constexpr char DefaultFolder[] = "/fex/games";
constexpr char SettingsPath[] = "sdmc:/fex/fex-switch2.cfg";
constexpr char LogPath[] = "/fex/fex-switch2-guest.log";

FsFile LogFile {};
s64 LogOffset {};
bool LogOpen {};
bool LogWriteFailed {};
bool BufferingGuestLog {};
alignas(GuestPageSize) GuestLogBuffer<2 * 1024 * 1024> GuestLog;
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

  for (const char* Path : {"/fex", DefaultFolder, Switch2Guest::Root}) {
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

void WriteLog(const char* Text) {
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

void BeginGuestLog() {
  WriteLog("Guest run started. Runtime trace is buffered.");
  GuestLog.Clear();
  BufferingGuestLog = true;
}

void EndGuestLog() {
  if (!BufferingGuestLog) return;
  BufferingGuestLog = false;
  if (const auto Dropped = GuestLog.DroppedLines()) {
    char Message[96];
    const auto Length = std::snprintf(Message, sizeof(Message), "Guest log omitted %zu older lines.\n", Dropped);
    WriteLogBytes({Message, static_cast<size_t>(Length)});
  }
  for (auto Bytes : GuestLog.Contents()) {
    constexpr size_t MaxWrite = 64 * 1024;
    while (!Bytes.empty()) {
      const auto Count = std::min(Bytes.size(), MaxWrite);
      if (!WriteLogBytes(Bytes.first(Count))) break;
      Bytes = Bytes.subspan(Count);
    }
  }
  FlushLogFile();
  GuestLog.Clear();
}

// The guest deliberately uses no external loader, libraries, or filesystem.
alignas(GuestPageSize) constexpr std::array<uint8_t, GuestPageSize> GuestCode {
  // Exercise more unmatched returns than the host shadow stack could hold.
  0xb9, 0, 0, 5, 0,                       // mov ecx, 0x50000
  0x48, 0x8d, 0x15, 3, 0, 0, 0,          // loop: lea rdx, [rip + 3]
  0x52,                                  // push rdx
  0xeb, 6,                               // jmp target
  0xff, 0xc9,                            // resume: dec ecx
  0x75, 0xf2,                            // jnz loop
  0xeb, 1,                               // jmp done
  0xc3,                                  // target: ret
  0x0f, 0x31,                            // done: rdtsc
  0xbf, 7, 0, 0, 0, // mov edi, 7
  0x6b, 0xff, 6,    // imul edi, edi, 6
  0xb8, LinuxExitSyscall, 0, 0, 0, // mov eax, 60
  0x0f, 0x05,       // syscall
  0x0f, 0x0b,       // ud2
};

void PrintStage(const char* Text) {
  WriteLog(Text);
  if (!GraphicsActive) {
    std::puts(Text);
    consoleUpdate(nullptr);
  }
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
    static_cast<uint8_t*>(std::aligned_alloc(GuestPageSize, (Size + GuestPageSize - 1) & ~(GuestPageSize - 1))), &std::free};
  if (!Bytes) {
    fsFileClose(&File);
    return "Could not allocate the ELF read buffer.";
  }
  constexpr u64 ReadChunkSize = 64 * 1024;
  const auto FileSize = static_cast<u64>(Size);
  for (u64 Offset = 0; Offset < FileSize;) {
    const u64 Chunk = std::min(FileSize - Offset, ReadChunkSize);
    u64 Read {};
    Status = fsFileRead(&File, Offset, Bytes.get() + Offset, Chunk, FsReadOption_None, &Read);
    if (R_FAILED(Status) || Read != Chunk) {
      fsFileClose(&File);
      return "Could not read the complete ELF.";
    }
    Offset += Read;
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
      DrawItem(1, "Browse Linux rootfs");
      DrawItem(2, "Settings");
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
      std::puts(LoadError ? "\nB Back" : "\nA Run    X Run single-block    B Back");
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
        } else if (Selected == 1) {
          OpenFolder(Switch2Guest::Root);
        } else {
          Open(Screen::Settings);
        }
        return true;
      }
      return Move(Buttons, 3);
    case Screen::Browse:
      return UpdateBrowser(Buttons);
    case Screen::Details:
      if (Buttons & HidNpadButton_B) {
        Image = {};
        Interpreter = {};
        BackToBrowser();
        return true;
      }
      if ((Buttons & (HidNpadButton_A | HidNpadButton_X)) && !LoadError) {
        RunSelectedElf((Buttons & HidNpadButton_X) != 0);
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
    if (!LoadError && !Image.Interpreter.empty()) {
      const auto Path = Switch2Guest::ResolvePath(Image.Interpreter, "/");
      LoadError = Path ? ReadElf(Path->c_str(), Interpreter) : "Invalid interpreter path.";
      if (!LoadError && !Interpreter.Interpreter.empty()) {
        LoadError = "An ELF interpreter cannot have its own interpreter.";
      }
    }
    Open(Screen::Details);
  }

  void RunSelectedElf(bool SingleBlock = false) {
    consoleClear();
    PrintStage(DisplayName(SelectedFile).c_str());
    std::vector<std::string> Arguments;
    const auto ArgumentsPath = "sdmc:" + SelectedFile + ".args";
    if (auto* File = std::fopen(ArgumentsPath.c_str(), "r")) {
      char Line[FS_MAX_PATH + 2];
      bool Valid = true;
      while (std::fgets(Line, sizeof(Line), File)) {
        const size_t Length = strcspn(Line, "\r\n");
        if (Length >= FS_MAX_PATH || Arguments.size() == 32) {
          Valid = false;
          break;
        }
        Arguments.emplace_back(Line, Length);
      }
      Valid &= !std::ferror(File);
      std::fclose(File);
      if (!Valid) {
        PrintStage("Could not read arguments. Use up to 32 short lines, one per argument.");
        ShowElfResult("Invalid argument file.");
        return;
      }
    }
    std::vector<Switch2Guest::Segment> Segments(Image.GetSegments().begin(), Image.GetSegments().end());
    const auto InterpreterSegments = Interpreter.GetSegments();
    Segments.insert(Segments.end(), InterpreterSegments.begin(), InterpreterSegments.end());
    if (Interpreter.Entry) {
      PrintStage("Guest rootfs /fex/rootfs");
      PrintStage(("Starting interpreter " + Image.Interpreter).c_str());
    }
    const std::string_view Path {SelectedFile};
    const bool InsideRoot = Path.starts_with("/fex/rootfs/");
    const auto Program = InsideRoot ? Path.substr(sizeof(Switch2Guest::Root) - 1) : Path;
    const auto Exit = RunGuest(Interpreter.Entry ? Interpreter.Entry : Image.Entry, Segments,
                               InsideRoot ? Switch2Guest::Root : Folder, Program, Arguments,
                               &Image, Interpreter.Base, InsideRoot ? "/" : Folder, SingleBlock);
    ShowElfResult(Exit ? "Exited with status " + std::to_string(*Exit) : "Guest did not reach exit.");
  }

  void ShowElfResult(std::string Result) {
    Image = {};
    Interpreter = {};
    LastResult = std::move(Result);
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
  Switch2Guest::ElfImage Interpreter;
  const char* LoadError {};
  bool ResultFromElf {};
};
} // namespace Switch2Guest

extern "C" {
alignas(16) char __nx_exception_stack[16 * 1024];
uint64_t __nx_exception_stack_size = sizeof(__nx_exception_stack);
uint32_t __nx_exception_ignoredebug = 1;
extern char _start[];

void __libnx_exception_handler(ThreadExceptionDump* Dump) {
  static bool LoggingFault {};
  if (!Dump || LoggingFault) return;
  LoggingFault = true;
  Switch2Guest::EndGuestLog();
  char Line[512];
  const auto Base = reinterpret_cast<uint64_t>(_start);
  std::snprintf(Line, sizeof(Line),
                "Host fault desc=%#x esr=%#x far=%#llx pc=%#llx lr=%#llx sp=%#llx fp=%#llx nro_base=%#llx",
                Dump->error_desc, Dump->esr, static_cast<unsigned long long>(Dump->far.x),
                static_cast<unsigned long long>(Dump->pc.x), static_cast<unsigned long long>(Dump->lr.x),
                static_cast<unsigned long long>(Dump->sp.x), static_cast<unsigned long long>(Dump->fp.x),
                static_cast<unsigned long long>(Base));
  Switch2Guest::WriteLog(Line);
  MemoryInfo Stack {};
  u32 Page {};
  if (R_SUCCEEDED(svcQueryMemory(&Stack, &Page, Dump->sp.x))) {
    std::snprintf(Line, sizeof(Line), "Host fault stack region base=%#llx size=%#llx perm=%#x type=%#x",
                  static_cast<unsigned long long>(Stack.addr), static_cast<unsigned long long>(Stack.size), Stack.perm, Stack.type);
    Switch2Guest::WriteLog(Line);
  }
}
}

int main() {
  using namespace Switch2Guest;
  const Result LogStatus = OpenLog();
  WriteLog("--- FEX Switch 2 launcher 0.9.17 started ---");
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
