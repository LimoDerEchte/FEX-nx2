// SPDX-License-Identifier: MIT
// The console side of the RootFS installer (GuestRootfsInstall.h): the SD card paths, free space, plain HTTP over the socket
// service, the guest filesystem writer, the wipe of an installed RootFS, and the guest run of "dpkg --configure -a" for the extra
// packages. The Installation screen (ui/screens/install.cpp) drives it through ui::InstallEvents: the installer's log lines, steps
// and progress go to the screen, Pump() draws a frame, Cancelled() is the screen's cancel. A cancelled or failed install removes
// everything it created.
#include "GuestRootFS.h"
#include "GuestRootfsInstall.h"
#include "GuestRuntime.h"
#include "HostNet.h"
#include "LauncherHost.h"
#include "SdPath.h"

#include <switch.h>

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "RootfsIndex.h"

namespace {
using namespace Switch2Guest;
using namespace Switch2Guest::RootfsInstall;

// The wipe walks the trees itself: one SD call per entry, so other programs (the HOME menu) get the SD service between calls.
struct SdEntry {
  std::string Name;
  bool Folder {};
};

std::string SdError(const char* What, const std::string& Path, Result Code) {
  char Text[32];
  std::snprintf(Text, sizeof(Text), " (0x%08x)", Code);
  return std::string(What) + " " + Path + Text;
}

// The names in one folder, read to the end before anything in it is deleted.
bool ListSdFolder(FsFileSystem& Sd, const std::string& Path, std::vector<SdEntry>& Out, std::string& Error) {
  Out.clear();
  FsDir Dir {};
  Result Code = fsFsOpenDirectory(&Sd, SdPath(Path.c_str()), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles | FsDirOpenMode_NoFileSize, &Dir);
  if (R_FAILED(Code)) {
    Error = SdError("Could not open", Path, Code);
    return false;
  }
  std::vector<FsDirectoryEntry> Batch(32);
  for (;;) {
    s64 Count = 0;
    Code = fsDirRead(&Dir, &Count, Batch.size(), Batch.data());
    if (R_FAILED(Code) || Count <= 0) break;
    for (s64 Index = 0; Index < Count; ++Index)
      Out.push_back({Batch[size_t(Index)].name, Batch[size_t(Index)].type == FsDirEntryType_Dir});
  }
  fsDirClose(&Dir);
  if (R_FAILED(Code)) {
    Error = SdError("Could not read", Path, Code);
    return false;
  }
  return true;
}

struct SdWipe {
  FsFileSystem& Sd;
  const std::function<void(uint64_t, uint64_t)>& Tick;
  std::string& Error;
  uint64_t Total {}, Done {};

  void Report(uint64_t Value, uint64_t Of) {
    if (Tick) Tick(Value, Of);
  }
  bool Count(const std::string& Path) {
    std::vector<SdEntry> Entries;
    if (!ListSdFolder(Sd, Path, Entries, Error)) return false;
    for (const auto& Entry : Entries) {
      ++Total;
      if (Entry.Folder && !Count(Path + "/" + Entry.Name)) return false;
    }
    Report(Total, 0);
    return true;
  }
  // Removes what a folder holds (not the folder itself), deepest entries first.
  bool Empty(const std::string& Path) {
    std::vector<SdEntry> Entries;
    if (!ListSdFolder(Sd, Path, Entries, Error)) return false;
    for (const auto& Entry : Entries) {
      const std::string Child = Path + "/" + Entry.Name;
      if (Entry.Folder && !Empty(Child)) return false;
      const Result Code = Entry.Folder ? fsFsDeleteDirectory(&Sd, SdPath(Child.c_str())) : fsFsDeleteFile(&Sd, SdPath(Child.c_str()));
      if (R_FAILED(Code)) {
        Error = SdError("Could not delete", Child, Code);
        return false;
      }
      Report(++Done, std::max(Total, Done));
    }
    return true;
  }
};

// The folder exists afterwards (the launcher expects it), empty.
bool EnsureSdFolder(FsFileSystem& Sd, const char* Path, std::string& Error) {
  FsDirEntryType Type {};
  if (R_SUCCEEDED(fsFsGetEntryType(&Sd, SdPath(Path), &Type)) && Type == FsDirEntryType_Dir) return true;
  const Result Code = fsFsCreateDirectory(&Sd, SdPath(Path));
  if (R_FAILED(Code)) {
    Error = SdError("Could not create", Path, Code);
    return false;
  }
  return true;
}

// Runs the wipe on a worker thread while this (the UI) thread draws frames. Show gets the latest (done, total) before each frame.
bool WipeOnWorker(ui::InstallEvents& Events, std::string& Error, const std::function<void(uint64_t, uint64_t)>& Show) {
  std::atomic<bool> Finished {};
  std::atomic<uint64_t> Done {}, Total {};
  bool Ok {};
  std::thread Worker([&] {
    Ok = WipeGuestRootfs(Error, [&](uint64_t Value, uint64_t Of) {
      Done.store(Value, std::memory_order_relaxed);
      Total.store(Of, std::memory_order_relaxed);
    });
    Finished.store(true, std::memory_order_release);
  });
  while (!Finished.load(std::memory_order_acquire)) {
    Show(Done.load(std::memory_order_relaxed), Total.load(std::memory_order_relaxed));
    Events.Pump();
    svcSleepThread(16'000'000);
  }
  Worker.join();
  Show(Done.load(), Total.load());
  return Ok;
}

// The guest run of dpkg: its output and the launcher's stage lines go to the install log, and the UI draws frames while it runs.
class DpkgHost final : public GuestHost {
public:
  explicit DpkgHost(ui::InstallEvents& Events) : Events {Events} {}
  void Output(const char* Data, size_t Size) override { Lines.Push(Data, Size); }
  bool Stopped() override { return Stop.load(); }
  bool MainTick() override {
    Drain();
    Events.Pump();
    if (Events.Cancelled()) Stop.store(true);
    return !Stop.load();
  }
  // Moves finished lines to the log (escape sequences and carriage returns dropped).
  void Drain() {
    char Buffer[1024];
    for (size_t Count; (Count = Lines.Pop(Buffer, sizeof(Buffer))) > 0;) {
      for (size_t Index = 0; Index < Count; ++Index) {
        const char Byte = Buffer[Index];
        if (InEscape) {
          if (Byte >= '@' && Byte <= '~' && Byte != '[') InEscape = false;
        } else if (Byte == 27) {
          InEscape = true;
        } else if (Byte == '\n') {
          Flush();
        } else if (Byte != '\r') {
          Partial += Byte;
        }
      }
    }
  }
  void Flush() {
    if (!Partial.empty()) Events.Log(ui::InstallLogKind::Info, Partial);
    Partial.clear();
  }
  ByteQueue Lines;

private:
  ui::InstallEvents& Events;
  std::atomic<bool> Stop {false};
  std::string Partial;
  bool InEscape {};
};

// dpkg --configure -a in the guest as root (the extra packages are only unpacked by the installer).
bool RunDpkgConfigure(ui::InstallEvents& Events, std::string& Detail) {
  ElfImage Image, Interpreter;
  if (const auto* Error = ReadRootfsElf("/usr/bin/dpkg", Image, Interpreter)) {
    Detail = Error;
    return false;
  }
  std::vector<Segment> Segments(Image.GetSegments().begin(), Image.GetSegments().end());
  const auto InterpreterSegments = Interpreter.GetSegments();
  Segments.insert(Segments.end(), InterpreterSegments.begin(), InterpreterSegments.end());
  SetLaunchCredentials(std::pair<uint32_t, uint32_t> {0, 0});
  SetRecoveryTestLaunch(false);
  DpkgHost Host(Events);
  SetGuestHost(&Host);
  SetStageSink([&Host](const char* Text) {
    Host.Lines.Push(Text, std::strlen(Text));
    Host.Lines.Push("\n", 1);
  });
  const std::vector<std::string> Arguments {"--configure", "-a"};
  const auto Exit = RunGuest(Interpreter.Entry ? Interpreter.Entry : Image.Entry, Segments, Root, "/usr/bin/dpkg", Arguments, &Image,
                             Interpreter.Base, "/", false, false);
  SetStageSink(nullptr);
  SetGuestHost(nullptr);
  Host.Drain();
  Host.Flush();
  SetLaunchCredentials(std::nullopt);
  if (!Exit) {
    Detail = "dpkg did not reach exit";
    return false;
  }
  if (*Exit != 0) {
    Detail = "dpkg exited with status " + std::to_string(*Exit);
    return false;
  }
  return true;
}

class ConsoleEnv final : public InstallEnv {
public:
  explicit ConsoleEnv(ui::InstallEvents& Events) : Events {Events} {}

  bool Cancelled() override {
    if (Events.Cancelled()) Latched = true;
    return Latched;
  }
  void Pump() override { Events.Pump(); }
  void Log(LogKind Kind, const std::string& Line) override {
    const auto Mapped = Kind == LogKind::Ok ? ui::InstallLogKind::Ok : Kind == LogKind::Warn ? ui::InstallLogKind::Warn
                      : Kind == LogKind::Error ? ui::InstallLogKind::Error : ui::InstallLogKind::Info;
    WriteLog(("Rootfs install: " + Line).c_str());
    Events.Log(Mapped, Line);
  }
  void Step(int Index, const std::string& Title) override { Events.Step(Index, Title); }
  void Progress(double Fraction, const std::string& Detail) override { Events.Progress(Fraction, Detail); }
  uint64_t FreeBytes() override {
    FsFileSystem Sd {};
    s64 Free = 0;
    if (R_SUCCEEDED(fsOpenSdCardFileSystem(&Sd))) {
      if (R_FAILED(fsFsGetFreeSpace(&Sd, SdPath("/"), &Free))) Free = 0;
      fsFsClose(&Sd);
    }
    return Free > 0 ? static_cast<uint64_t>(Free) : 0;
  }
  std::string Native(const std::string& Logical) override { return "sdmc:" + Logical; }
  std::unique_ptr<RootfsWriter> OpenWriter() override { return OpenGuestRootfsWriter(); }
  bool RootfsPopulated() override {
    auto* Directory = opendir("sdmc:/fex/rootfs");
    if (!Directory) return false;
    bool Any = false;
    while (const auto* Entry = readdir(Directory)) {
      if (std::strcmp(Entry->d_name, ".") && std::strcmp(Entry->d_name, "..")) {
        Any = true;
        break;
      }
    }
    closedir(Directory);
    return Any;
  }
  bool WipeRootfs(const WipeTick& Tick, std::string& Error) override {
    return WipeOnWorker(Events, Error, [&](uint64_t Done, uint64_t Total) {
      if (Tick) Tick(Done, Total);
    });
  }
  std::vector<uint32_t> DnsServers() override {
    uint32_t Primary {}, Secondary {};
    if (!HostNet::DnsServers(Primary, Secondary)) return {};
    return {Primary, Secondary};
  }
  void RandomBytes(uint8_t* Data, size_t Size) override { randomGet(Data, Size); }
  bool ConfigureExtras(std::string& Detail) override { return RunDpkgConfigure(Events, Detail); }
  int64_t Now() override { return static_cast<int64_t>(std::time(nullptr)); }

private:
  ui::InstallEvents& Events;
  bool Latched {};
};

// The network for one call: nifm (reference counted) and the socket service. Online() is false without an address.
struct Network {
  Network() {
    Nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    u32 Address {};
    Online = Nifm && R_SUCCEEDED(nifmGetCurrentIpAddress(&Address)) && Address && HostNet::Acquire();
  }
  ~Network() {
    if (Online) HostNet::Release();
    if (Nifm) nifmExit();
  }
  bool Nifm {}, Online {};
};

} // namespace

namespace Switch2Guest {
bool WipeGuestRootfs(std::string& Error, const std::function<void(uint64_t, uint64_t)>& Tick) {
  // The "installed" marker goes first, so an interrupted wipe never leaves a RootFS that looks valid. Then the rootfs folder and its
  // inode metadata are emptied; the launcher keeps both folders present.
  std::remove("sdmc:/fex/config/rootfs.json");
  FsFileSystem Sd {};
  if (R_FAILED(fsOpenSdCardFileSystem(&Sd))) {
    Error = "Could not open the SD card";
    return false;
  }
  SdWipe Wipe {Sd, Tick, Error};
  bool Ok = EnsureSdFolder(Sd, "/fex/rootfs", Error) && EnsureSdFolder(Sd, "/fex/.inode", Error);
  Ok = Ok && Wipe.Count("/fex/rootfs") && Wipe.Count("/fex/.inode");
  Ok = Ok && Wipe.Empty("/fex/rootfs") && Wipe.Empty("/fex/.inode");
  fsFsClose(&Sd);
  return Ok;
}

ui::InstallOutcome RunRootfsWipe(ui::InstallEvents& Events) {
  ui::InstallOutcome Out;
  Events.Step(0, "Counting the files");
  Events.Log(ui::InstallLogKind::Info, "Counting the files of the RootFS");
  WriteLog("Rootfs wipe: counting");
  bool Removing = false;
  uint64_t Counted = 0;
  const bool Ok = WipeOnWorker(Events, Out.Error, [&](uint64_t Done, uint64_t Total) {
    if (!Total) {
      Counted = Done;
      Events.Progress(0, std::to_string(Done) + " found");
      return;
    }
    if (!Removing) {
      Removing = true;
      Counted = Total;
      const std::string Line = std::to_string(Total) + " files and folders to remove";
      Events.Log(ui::InstallLogKind::Info, Line);
      WriteLog(("Rootfs wipe: " + Line).c_str());
      Events.Step(1, "Removing the files");
    }
    Events.Progress(double(Done) / double(Total), std::to_string(Done) + " of " + std::to_string(Total));
  });
  if (!Ok) {
    if (Out.Error.empty()) Out.Error = "Could not wipe the RootFS";
    Events.Log(ui::InstallLogKind::Error, Out.Error);
    WriteLog(("Rootfs wipe: " + Out.Error).c_str());
    return Out;
  }
  if (!Removing) Events.Step(1, "Removing the files");  // nothing was there to remove
  const std::string Line = "Removed " + std::to_string(Counted) + " files and folders";
  Events.Log(ui::InstallLogKind::Info, Line);
  WriteLog(("Rootfs wipe: " + Line).c_str());
  Out.Ok = true;
  return Out;
}

bool FetchLiveRootfsIndexText(std::string& Text, std::string& Error) {
  Network Net;
  if (!Net.Online) {
    Error = "The console is not connected to a network.";
    return false;
  }
  struct Quiet final : ui::InstallEvents {
    void Step(int, const std::string&) override {}
    void Progress(double, const std::string&) override {}
    void Log(ui::InstallLogKind, const std::string&) override {}
    void Pump() override {}
    bool Cancelled() override { return false; }
  } Silent;
  ConsoleEnv Env(Silent);
  Text.clear();
  Http::ResponseInfo Info;
  const auto* Failure = Env.HttpGet(ui::LiveIndexUrl, 0, [&](const uint8_t* Data, size_t Count) {
    Text.append(reinterpret_cast<const char*>(Data), Count);
    return Text.size() < (4u << 20);
  }, Info);
  if (Failure) {
    Error = std::string("Could not read the online index: ") + Failure;
    Text.clear();
    return false;
  }
  return true;
}

ui::InstallOutcome RunRootfsInstall(const ui::InstallRequest& Request, ui::InstallEvents& Events) {
  ui::InstallOutcome Out;
  ConsoleEnv Env(Events);
  const ui::RootfsImage& Image = Request.Image;
  ImageSpec Spec;
  Spec.Name = Image.Name;
  Spec.FileName = Image.FileName;
  Spec.Url = Image.Url;
  Spec.Hash = Image.Hash;
  Spec.DownloadBytes = Image.DownloadBytes;
  Spec.InstalledBytes = Image.InstalledBytes;
  if (!Request.LocalPath.empty()) {
    // A card path, `sdmc:/fex/images/x.sqsh` or `/fex/images/x.sqsh` (ui/Backend.h); the installer wants `/fex/images/x.sqsh`.
    Spec.LocalPath = Request.LocalPath.starts_with("sdmc:") ? Request.LocalPath.substr(5) : Request.LocalPath;
  }
  InstallOptions Options;
  Options.Replace = true;
  Options.Extras = Request.Extras;
  Network Net;
  if (!Net.Online && Spec.LocalPath.empty()) {
    Out.Error = "The console is not connected to a network.";
    Events.Log(ui::InstallLogKind::Error, Out.Error);
    return Out;
  }
  Installer Run(Env, Spec, Options);
  const auto Result = Run.Run();
  Out.Ok = Result.Ok;
  Out.Cancelled = Result.Cancelled;
  Out.Error = Result.Error;
  Out.Warning = Result.Warning;
  return Out;
}
} // namespace Switch2Guest
