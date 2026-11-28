// SPDX-License-Identifier: MIT
// The console implementation of ui::LauncherBackend (ui/Backend.h) over the existing runtime. The screens call only the interface:
//  - storage: PosixStorage on sdmc:, with the RootFS listed through the guest filesystem layer (its view, not the raw SD names);
//  - the FTP server (GuestFtp.h) steps once per frame from Tick(); it is stopped while a guest runs or the installer works, because
//    the guest runtime and the FTP file service both own the inode store;
//  - the RootFS install (RootfsInstallHost.cpp);
//  - terminal tabs share one guest worker, with a host-owned pty master for each tab.
#include "LauncherBackend.h"

#include <switch.h>

#include <arpa/inet.h>
#include <sys/stat.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>

#include "GuestFtp.h"
#include "GuestInputFeed.h"
#include "GuestRootFS.h"
#include "GuestService.h"
#include "HostNet.h"
#include "LauncherHost.h"
#include "PathUtil.h"
#include "RootfsIndex.h"
#include "RootfsState.h"
#include "Storage.h"

#ifndef FEX_SWITCH2_FEX_BUILD
#define FEX_SWITCH2_FEX_BUILD "unknown"
#endif

namespace Switch2Guest {
namespace {

// ---- storage -----------------------------------------------------------------------------------------------------------------

class VectorDirReader final : public ui::DirReader {
public:
  explicit VectorDirReader(std::shared_ptr<SessionDirectory> Listing) : Listing(std::move(Listing)) {}
  ui::DirStep Next(ui::DirEntry& Out) override {
    if (!Listing->Ready.load()) return ui::DirStep::Pending;
    if (Failed() || At >= Listing->Entries.size()) return ui::DirStep::End;
    const auto& Entry = Listing->Entries[At++];
    Out = {};
    Out.Name = Entry.Name;
    Out.IsDir = Entry.IsDirectory;
    return ui::DirStep::Entry;
  }
  bool Failed() const override { return Listing->Ready.load() && R_FAILED(Listing->Status); }

private:
  std::shared_ptr<SessionDirectory> Listing;
  size_t At {};
};

// sdmc:/... is the card through stdio; /... is the inside of the RootFS, listed the way a guest program sees it.
class ConsoleStorage final : public ui::PosixStorage {
public:
  ConsoleStorage() : PosixStorage("sdmc:", "sdmc:/fex/rootfs") {}
  GuestService* Service {};
  std::unique_ptr<ui::DirReader> OpenDir(const std::string& Path) override {
    const std::string Normal = ui::NormalizePath(Path);
    if (!ui::IsRootfsPath(Normal)) return PosixStorage::OpenDir(Path);
    auto Listing = std::make_shared<SessionDirectory>();
    Listing->Folder = Normal == "/" ? std::string(Root) : std::string(Root) + Normal;
    if (Service) {
      if (!Service->List(Listing)) return nullptr;
    } else {
      Listing->Status = ReadRootfsDirectory(Listing->Folder, Listing->Entries);
      Listing->Ready.store(true);
    }
    return std::make_unique<VectorDirReader>(std::move(Listing));
  }
};

// ---- sessions ----------------------------------------------------------------------------------------------------------------

struct SessionData {
  std::shared_ptr<HostSession> Host;
  std::string Title;
};

std::string ParentOf(const std::string& Path) {
  const auto Slash = Path.rfind('/');
  return Slash == std::string::npos || Slash == 0 ? "/" : Path.substr(0, Slash);
}

// Settings > Developer: sidecar files of a program, as the old launcher read them. Returns an error text or "".
std::string ReadSidecars(SessionLaunch& P, bool& Recovery, std::string& Notice) {
  const std::string Base = "sdmc:" + (P.OnCard ? P.Program : std::string(Root) + P.Program);
  if (auto* File = std::fopen((Base + ".args").c_str(), "r")) {
    char Line[FS_MAX_PATH + 2];
    bool Valid = true;
    while (std::fgets(Line, sizeof(Line), File)) {
      const size_t Length = strcspn(Line, "\r\n");
      if (Length >= FS_MAX_PATH || P.Arguments.size() == 32) {
        Valid = false;
        break;
      }
      P.Arguments.emplace_back(Line, Length);
    }
    Valid &= !std::ferror(File);
    std::fclose(File);
    if (!Valid) return "Could not read arguments. Use up to 32 short lines, one per argument.";
  }
  if (auto* File = std::fopen((Base + ".user").c_str(), "r")) {
    unsigned long User {}, Group {};
    char Extra {};
    const bool Parsed = std::fscanf(File, "%lu %lu %c", &User, &Group, &Extra) == 2 && User < UINT32_MAX && Group < UINT32_MAX;
    std::fclose(File);
    if (!Parsed) return "Invalid user file. Use one line: uid gid.";
    P.Credentials.emplace(User, Group);
    Notice += "Running as uid " + std::to_string(User) + ", gid " + std::to_string(Group) + "\r\n";
  }
  if (auto* File = std::fopen((Base + ".single-thread").c_str(), "r")) {
    std::fclose(File);
    P.SingleThread = true;
    Notice += "Single-thread mode: the program's threads run one at a time.\r\n";
  }
  if (auto* File = std::fopen((Base + ".no-tso").c_str(), "r")) {
    std::fclose(File);
    P.NoTso = true;
    Notice += "Experiment: x86 memory ordering (TSO) is off; threaded programs may misbehave.\r\n";
  }
  if (auto* File = std::fopen((Base + ".profile").c_str(), "r")) {
    std::fclose(File);
    P.Profile = true;
    Notice += "Experiment: guest code is sampled every 10 ms; the profile goes to the log.\r\n";
  }
  if (auto* File = std::fopen((Base + ".recovery-test").c_str(), "r")) {
    std::fclose(File);
    Recovery = true;
    Notice += "Recovery test: the probe's journaled move will be interrupted.\r\n";
  }
  return {};
}

// ---- the backend -------------------------------------------------------------------------------------------------------------

class ConsoleBackend final : public ui::LauncherBackend {
public:
  explicit ConsoleBackend(std::string Version) : VersionLabel(std::move(Version)) {
    NifmOk = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
  }
  ~ConsoleBackend() override {
    if (Service) {
      Service->Stop();
      FinishService(false);
    }
    FtpStop();
    if (NifmOk) nifmExit();
  }

  ui::Storage& GetStorage() override { return Card; }
  std::string VersionText() override { return "v" + VersionLabel + " \xC2\xB7 FEX-" + FexBuild(); }
  std::string FexBuild() override { return FEX_SWITCH2_FEX_BUILD; }

  ui::RootfsInfo Rootfs() override {
    const uint64_t Now = armTicksToNs(armGetSystemTick());
    if (!RootfsValid || Now - RootfsAt > 2'000'000'000ull) {
      RootfsCache = ui::DetectRootfs(Card);
      RootfsAt = Now;
      RootfsValid = true;
    }
    return RootfsCache;
  }
  ui::FtpInfo Ftp() override {
    ui::FtpInfo Info;
    Info.Running = Server.has_value();
    Info.Clients = Server ? int(Server->Connections()) : 0;
    return Info;
  }
  ui::Capability Steam() override { return {false, "Needs graphics support \xC2\xB7 not available yet"}; }
  ui::Capability Desktop() override { return {false, "Needs graphics support \xC2\xB7 not available yet"}; }
  ui::Capability Shell() override {
    if (!Rootfs().Installed) return {false, "No RootFS installed"};
    return {};
  }
  ui::Capability Reinstall() override {
    if (!Rootfs().Installed) return {false, "No RootFS installed"};
    if (Service) return {false, "Close the guest tabs before changing the RootFS."};
    return {};
  }
  ui::Capability Wipe() override { return Reinstall(); }
  std::string WipeSummary() override {
    const std::string Name = Rootfs().Name.empty() ? "the RootFS" : Rootfs().Name;
    return Name + ", the desktops and game stores installed in it, and your home folder";
  }
  ui::InstallOutcome WipeRootfs(ui::InstallEvents& Events) override {
    if (Service) return {false, false, "Close the guest tabs before changing the RootFS.", {}};
    if (InFtpStep) return {false, false, "Wait for the FTP transfer to end, or stop the FTP server first.", {}};
    SuspendFtp Quiet(*this);
    auto Outcome = RunRootfsWipe(Events);
    RootfsValid = false;
    return Outcome;
  }
  std::string HomeFolder() override { return Card.IsDir(DefaultHome) ? std::string(DefaultHome) + "/" : ""; }

  const ui::RootfsIndex& RootfsImages() override { return ui::BuiltInRootfsIndex(); }
  bool FetchLiveRootfsIndex(std::string& Text, std::string& Error) override { return FetchLiveRootfsIndexText(Text, Error); }
  ui::InstallOutcome InstallRootfs(const ui::InstallRequest& Request, ui::InstallEvents& Events) override {
    if (Service) return {false, false, "Close the guest tabs before changing the RootFS.", {}};
    if (InFtpStep) return {false, false, "Wait for the FTP transfer to end, or stop the FTP server first.", {}};
    SuspendFtp Quiet(*this);
    auto Outcome = RunRootfsInstall(Request, Events);
    RootfsValid = false;
    return Outcome;
  }

  // ---- FTP
  std::string NetworkAddress() override {
    u32 Address = 0;
    if (!NifmOk || R_FAILED(nifmGetCurrentIpAddress(&Address)) || !Address) return {};
    char Text[INET_ADDRSTRLEN] {};
    const in_addr Host {Address};
    return inet_ntop(AF_INET, &Host, Text, sizeof(Text)) ? Text : "";
  }
  bool FtpStart(const ui::FtpConfig& Config, std::string& Error) override {
    if (InFtpStep) {
      // Called from a frame drawn inside a transfer: the running server is on the stack. Restart it after Step returns.
      FtpStop();
      PendingStart = Config;
      return true;
    }
    if (Service) {
      Error = "FTP is suspended while guest tabs are running.";
      return false;
    }
    FtpStop();
    u32 Address = 0;
    if (!NifmOk || R_FAILED(nifmGetCurrentIpAddress(&Address)) || !Address) {
      Error = "The console is not connected to a network.";
      return false;
    }
    if (!HostNet::Acquire()) {
      Error = "Could not start the network socket service.";
      return false;
    }
    NetHeld = true;
    Files = OpenGuestFileSystem();
    if (!Files) {
      Error = "Could not reserve transfer memory.";
      ReleaseNet();
      return false;
    }
    Server.emplace(*Files, Config.Password);
    Server->SetUser(Config.User);
    Server->SetStartDirectory(Config.Root);
    Server->Trace = [](const std::string& Line) { WriteLog(Line.c_str()); };
    Server->Idle = [this]() {
      if (Hook) Hook();
    };
    const int Failure = Server->Start(Config.Port, Address);
    const std::string Attempt = "FTP server port " + std::to_string(Config.Port) + ": " +
                                (Failure ? std::string(Server->FailedCall) + " failed, " + std::strerror(-Failure) : "listening");
    WriteLog(Attempt.c_str());
    if (Failure) {
      Error = std::string("Could not open a listening socket (") + Server->FailedCall + "): " + std::strerror(-Failure);
      Server.reset();
      Files.reset();
      ReleaseNet();
      return false;
    }
    Running = Config;
    return true;
  }
  void FtpStop() override {
    if (InFtpStep) {
      // Called from a frame drawn inside a transfer: the server and its file system are on the stack. Abort the
      // transfers now and stop after Step returns (Tick).
      for (const auto& C : Server->Clients()) Server->Disconnect(C.Id);
      StopPending = true;
      PendingStart.reset();
      return;
    }
    if (!Server && !Files) return;
    Server.reset();
    Files.reset();  // saves the guest inode store
    ReleaseNet();
    WriteLog("FTP server stopped.");
  }
  ui::FtpSnapshot FtpState() override {
    ui::FtpSnapshot Snap;
    Snap.Running = Server.has_value() && (!StopPending || PendingStart);
    Snap.Address = NetworkAddress();
    if (!Server) return Snap;
    Snap.Port = Server->Port();
    for (const auto& C : Server->Clients()) {
      ui::FtpClientView V;
      V.Id = C.Id;
      V.Address = C.Address;
      V.User = C.User;
      V.LoggedIn = C.LoggedIn;
      V.State = C.State == FtpClientInfo::Activity::Receiving ? ui::FtpClientView::Act::Receiving
              : C.State == FtpClientInfo::Activity::Sending   ? ui::FtpClientView::Act::Sending
                                                              : ui::FtpClientView::Act::Idle;
      V.File = C.File;
      V.Done = C.Done;
      V.Total = C.Total;
      V.BytesIn = C.BytesIn;
      V.BytesOut = C.BytesOut;
      Snap.Clients.push_back(std::move(V));
    }
    Snap.TotalIn = Server->TotalIn();
    Snap.TotalOut = Server->TotalOut();
    return Snap;
  }
  void FtpDisconnect(uint64_t ClientId) override {
    if (Server) Server->Disconnect(ClientId);
  }
  void Tick() override {
    if (Service && Service->Finished()) FinishService();
    if (!Server || InFtpStep) return;
    InFtpStep = true;
    Server->Step(0);
    InFtpStep = false;
    if (std::exchange(StopPending, false)) {
      FtpStop();
      if (const auto Config = std::exchange(PendingStart, std::nullopt)) {
        std::string Error;
        if (!FtpStart(*Config, Error)) WriteLog(("FTP server did not restart: " + Error).c_str());
      }
    }
  }
  void SetFrameHook(std::function<void()> H) override { Hook = std::move(H); }

  bool KeyboardText(const std::string& Header, const std::string& Initial, ui::KeyboardKind Kind, std::string& Out) override {
    SwkbdConfig Config {};
    if (R_FAILED(swkbdCreate(&Config, 0))) {
      WriteLog("Could not open the Horizon keyboard.");
      return false;
    }
    swkbdConfigMakePresetDefault(&Config);
    swkbdConfigSetHeaderText(&Config, Header.c_str());
    swkbdConfigSetInitialText(&Config, Initial.c_str());
    swkbdConfigSetStringLenMax(&Config, 512);
    swkbdConfigSetReturnButtonFlag(&Config, false);
    if (Kind == ui::KeyboardKind::Number) swkbdConfigSetType(&Config, SwkbdType_NumPad);
    if (Kind == ui::KeyboardKind::Password) swkbdConfigSetPasswordFlag(&Config, 1);
    std::array<char, 2049> Text {};
    const Result Status = swkbdShow(&Config, Text.data(), Text.size());
    swkbdClose(&Config);
    if (R_FAILED(Status)) return false;
    Out.assign(Text.data(), strnlen(Text.data(), Text.size()));
    return true;
  }

  // ---- settings of the launcher itself
  void ApplySetting(const std::string& Key, const std::string& Value) override {
    if (Key == "gamepad_identity") {
      InputFeed().SetGamepadIdentity(Value == "switch_pro" ? Evdev::GamepadIdentity::SwitchPro :
                                                            Evdev::GamepadIdentity::Xbox360);
    }
    if (Key == "log_level") Logging = Value == "trace" ? LogLevel::Trace : Value == "detailed" ? LogLevel::Detailed : LogLevel::Standard;
  }

  // ---- developer actions
  ui::Capability DevAction(const std::string& Id) override {
    if (Service && (Exclusive || Id == "run_elf_single")) return {false, "This action needs its own guest runtime. Close the guest tabs first."};
    return {};
  }

  ui::Capability MoreShells() override {
    if (Service && !Service->Finished() && Exclusive)
      return {false, "This launch needs its own guest runtime."};
    return {};
  }

  int OpenSession(const ui::SessionSpec& Spec, std::string& Error) override {
    using K = ui::SessionSpec::Kind;
    const bool Shell = Spec.K == K::Shell || Spec.K == K::RootShell;
    const bool Single = Spec.Id == "run_elf_single";
    const bool FinalPass = Spec.Id == "final_pass";
    if (!Shell && !Single && !FinalPass && Spec.Id != "run_elf") {
      Error = "Unknown action " + Spec.Id;
      return 0;
    }
    // A guest runtime owns the inode store, so the FTP server must stop first; inside a transfer it cannot yet.
    if (InFtpStep && !Service) {
      Error = "Wait for the FTP transfer to end, or stop the FTP server first.";
      return 0;
    }

    const int Id = NextId++;
    auto Host = std::make_shared<HostSession>(Id);
    auto& Launch = Host->Launch;
    bool Recovery = false;
    std::string Title;
    std::string Notice;
    if (Shell) {
      Launch.Program = "/bin/bash";
      Launch.Arguments = {"-l"};
      if (Spec.K == K::RootShell) Launch.Credentials.emplace(0, 0);
    } else {
      const std::string Path = FinalPass ? "sdmc:/fex/games/final-pass-suite.elf" : Spec.Path;
      Launch.OnCard = Path.starts_with("sdmc:");
      Launch.Program = Launch.OnCard ? Path.substr(5) : Path;
      Title = ui::BaseName(Path);
      Error = ReadSidecars(Launch, Recovery, Notice);
      if (!Error.empty()) {
        Host->Failure = Error;
        Host->State.store(HostSession::Ended);
        Sessions.emplace(Id, SessionData {Host, Title});
        return Id;
      }
    }
    const bool AsRoot = Launch.Credentials && Launch.Credentials->first == 0;
    const char* Home = AsRoot ? "/root" : DefaultHome;
    struct stat Info {};
    const bool HasHome = !stat((std::string("sdmc:") + Root + Home).c_str(), &Info) && S_ISDIR(Info.st_mode);
    Launch.WorkingDirectory = Launch.OnCard ? ParentOf(Launch.Program) : Shell || HasHome ? Home : "/";
    Launch.Folder = Launch.OnCard ? Launch.WorkingDirectory : std::string(Root) + (Launch.WorkingDirectory == "/" ? "" : Launch.WorkingDirectory);
    Launch.Environment = {
      AsRoot ? "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" : "PATH=/usr/local/bin:/usr/bin:/bin",
      std::string("HOME=") + (Shell || AsRoot || HasHome ? Home : "/"), "LANG=C", Shell ? "TERM=xterm-256color" : "TERM=dumb"};
    if (Shell) {
      Launch.Environment.insert(Launch.Environment.end(), {
        AsRoot ? "USER=root" : "USER=fex", AsRoot ? "LOGNAME=root" : "LOGNAME=fex", "SHELL=/bin/bash"});
    }
    Host->Master.Output(Notice.data(), Notice.size());
    if (Service && Service->Finished()) FinishService();
    if (Service && (Exclusive || Single || Recovery)) {
      Error = "This launch needs its own guest runtime. Close the guest tabs first.";
      return 0;
    }
    if (Service && !Service->Add(Host)) FinishService();
    if (!Service) {
      if (Server) ResumeFtp = Running;
      FtpStop();
      Service = std::make_unique<GuestService>();
      Card.Service = Service.get();
      Exclusive = Single || Recovery;
      SetRecoveryTestLaunch(Recovery);
      Service->Add(Host);
      if (!Service->Start(Single)) {
        FinishService();
        Error = "Could not start the guest runtime thread.";
        return 0;
      }
    }
    Sessions.emplace(Id, SessionData {Host, Title});
    return Id;
  }
  size_t SessionRead(int Id, char* Out, size_t Max) override {
    auto It = Sessions.find(Id);
    return It == Sessions.end() ? 0 : It->second.Host->Master.Read(Out, Max);
  }
  void SessionWrite(int Id, std::string_view Input) override {
    auto It = Sessions.find(Id);
    if (It != Sessions.end()) It->second.Host->Master.Write(Input.data(), Input.size());
  }
  void SessionResize(int Id, int Cols, int Rows) override {
    auto It = Sessions.find(Id);
    if (It == Sessions.end()) return;
    auto& Host = *It->second.Host;
    Host.Master.SetSize(Cols, Rows);
    Host.Sized.store(true);
  }
  void SessionForeground(int Id, bool Value) override {
    if (Service && Sessions.contains(Id)) Service->SetForeground(Id, Value);
  }
  void SessionClose(int Id) override {
    auto It = Sessions.find(Id);
    if (It == Sessions.end()) return;
    SessionForeground(Id, false);
    It->second.Host->CloseRequested.store(true);
    Sessions.erase(It);
  }
  ui::SessionInfo SessionState(int Id) override {
    ui::SessionInfo Info;
    auto It = Sessions.find(Id);
    if (It == Sessions.end()) return Info;
    auto& Host = *It->second.Host;
    Info.Title = It->second.Title;
    Info.Running = Host.State.load() != HostSession::Ended || Host.Master.PendingOutput() != 0;
    Info.ChildRunning = Host.State.load() == HostSession::Running && (!Info.Title.empty() || Host.Busy.load());
    if (!Info.Running) {
      std::lock_guard<std::mutex> Lock(Host.Mutex);
      Info.ExitNote = Host.Failure;
      if (Info.ExitNote.empty() && !Info.Title.empty()) {
        const int Signal = Host.TermSignal.load();
        Info.ExitNote = Signal ? "Terminated by signal " + std::to_string(Signal) : "Exited with status " + std::to_string(Host.ExitCode.load());
      }
    }
    return Info;
  }

private:
  // The FTP server is off while a guest runs or the installer works (both own the inode store); it comes back after.
  struct SuspendFtp {
    explicit SuspendFtp(ConsoleBackend& B) : Be(B) {
      if (Be.Server) {
        Resume = Be.Running;
        Be.FtpStop();
        Was = true;
      }
    }
    ~SuspendFtp() {
      if (Was) {
        std::string Error;
        if (!Be.FtpStart(Resume, Error)) WriteLog(("FTP server did not restart: " + Error).c_str());
      }
    }
    ConsoleBackend& Be;
    ui::FtpConfig Resume;
    bool Was = false;
  };

  void ReleaseNet() {
    if (NetHeld) HostNet::Release();
    NetHeld = false;
  }

  void FinishService(bool RestoreFtp = true) {
    Service->Join();
    Service.reset();
    Card.Service = nullptr;
    Exclusive = false;
    SetRecoveryTestLaunch(false);
    if (RestoreFtp && ResumeFtp) {
      const auto Config = std::exchange(ResumeFtp, std::nullopt);
      std::string Error;
      if (!FtpStart(*Config, Error)) WriteLog(("FTP server did not restart: " + Error).c_str());
    }
  }

  std::string VersionLabel;
  ConsoleStorage Card;
  bool NifmOk {};
  ui::RootfsInfo RootfsCache;
  uint64_t RootfsAt {};
  bool RootfsValid {};
  std::unique_ptr<FtpFileSystem> Files;
  std::optional<FtpServer> Server;
  ui::FtpConfig Running;
  bool NetHeld {}, InFtpStep {}, StopPending {};
  std::optional<ui::FtpConfig> PendingStart;  // FtpStart called inside a transfer: started by Tick after Step returns
  std::function<void()> Hook;
  std::map<int, SessionData> Sessions;
  std::unique_ptr<GuestService> Service;
  std::optional<ui::FtpConfig> ResumeFtp;
  bool Exclusive {};
  int NextId {1};
};

} // namespace

std::unique_ptr<ui::LauncherBackend> CreateLauncherBackend(const char* LauncherVersion) {
  return std::make_unique<ConsoleBackend>(LauncherVersion);
}

} // namespace Switch2Guest
