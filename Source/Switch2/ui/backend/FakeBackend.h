// SPDX-License-Identifier: MIT
// HOST ONLY: FakeBackend and FakeStorage serve the host tests and the headless screenshots. The console never links this.
// FakeStorage is an in-memory tree (sdmc:/ and / are two trees); folders can stall for a number of frames to exercise the
// loading spinner, and writes are recorded so tests can check the temp+rename order.
#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "Backend.h"

namespace ui {

class FakeStorage final : public Storage {
public:
  FakeStorage();
  // Test setup.
  void AddDir(const std::string& Path);
  void AddFile(const std::string& Path, uint64_t Size, int64_t Mtime = 0, const std::string& Data = {});
  void RemoveTree(const std::string& Path);
  void SetFree(const std::string& Root, uint64_t Free, uint64_t Total) { Space[Root] = {Free, Total}; }
  int PendingFrames = 0;      // each OpenDir stalls for this many Next calls before it yields its first entry
  bool FailWrites = false;    // WriteFileAtomic fails (full card)
  std::vector<std::string> Ops;  // "write <tmp>", "rename <from> <to>", ...

  bool ReadFile(const std::string& Path, std::string& Out) override;
  bool WriteFileAtomic(const std::string& Path, const std::string& Data, std::string& Error) override;
  bool MakeDirs(const std::string& Path) override;
  bool IsDir(const std::string& Path) override;
  bool Exists(const std::string& Path) override;
  std::unique_ptr<DirReader> OpenDir(const std::string& Path) override;
  bool FreeSpace(const std::string& Path, uint64_t& FreeBytes, uint64_t& TotalBytes) override;

private:
  struct Node {
    bool IsDir = false;
    uint64_t Size = 0;
    int64_t Mtime = 0;
    std::string Data;
  };
  std::map<std::string, Node> Nodes;  // normalized path -> node
  std::map<std::string, std::pair<uint64_t, uint64_t>> Space;
  friend class FakeDirReader;
};

// A scripted shell: Feed() queues output for Read(), Written holds the keys the screen sent.
class FakeShell final : public ShellSession {
public:
  ~FakeShell() override {
    if (Watch && *Watch == this)
      *Watch = nullptr;
  }
  FakeShell** Watch = nullptr;  // cleared when the shell is destroyed (the screen owns it)
  size_t Read(char* Out, size_t Max) override {
    const size_t N = std::min(Max, Pending.size());
    for (size_t I = 0; I < N; ++I)
      Out[I] = Pending[I];
    Pending.erase(0, N);
    return N;
  }
  void Write(std::string_view In) override { Written.append(In); }
  bool Running() override { return Alive; }
  bool ChildRunning() override { return Child; }
  void SetForeground(bool Value) override { Foreground = Value; }
  void Resize(int C, int R) override { Cols = C; Rows = R; }
  int Cols = 0, Rows = 0;
  std::string Title() override { return TitleText; }
  std::string ExitNote() override { return Note; }
  std::string TitleText, Note;
  void Feed(const std::string& Bytes) { Pending += Bytes; }
  std::string Pending, Written;
  bool Alive = true, Child = false, Foreground = false;
};

class FakeBackend : public LauncherBackend {
public:
  FakeBackend();
  Storage& GetStorage() override { return Store; }
  FakeStorage& Fs() { return Store; }
  std::string VersionText() override { return Version; }
  RootfsInfo Rootfs() override { return Root; }
  FtpInfo Ftp() override { return Ftp_; }
  Capability Steam() override { return SteamCap; }
  Capability Desktop() override { return DesktopCap; }
  Capability Shell() override { return ShellCap; }
  Capability Reinstall() override { return ReinstallCap; }
  Capability Wipe() override { return WipeCap; }
  std::string WipeSummary() override { return "Ubuntu_24_04.sqsh (6.1 GB), the KDE Plasma desktop and your home folder (1.2 GB)"; }
  InstallOutcome WipeRootfs(InstallEvents& Events) override;
  std::string HomeFolder() override { return "/home/switch/"; }
  std::string NetworkAddress() override { return Address; }
  bool FtpStart(const FtpConfig& Config, std::string& Error) override;
  void FtpStop() override;
  FtpSnapshot FtpState() override;
  void FtpDisconnect(uint64_t ClientId) override;
  bool KeyboardText(const std::string& Header, const std::string& Initial, KeyboardKind Kind, std::string& Out) override;
  std::unique_ptr<ShellSession> StartShell(bool Root, std::string& Error) override;
  Capability MoreShells() override { return MoreShellsCap; }
  std::string FexBuild() override { return "2606"; }
  void Tick() override { ++Ticks; }
  void ApplySetting(const std::string& Key, const std::string& Value) override { Applied[Key] = Value; }
  Capability DevAction(const std::string& Id) override {
    auto It = DevCaps.find(Id);
    return It == DevCaps.end() ? Capability {} : It->second;
  }
  std::unique_ptr<ShellSession> StartDevSession(const std::string& Id, const std::string& Path, std::string& Error) override;
  const RootfsIndex& RootfsImages() override { return BuiltInRootfsIndex(); }
  bool FetchLiveRootfsIndex(std::string& Text, std::string& Error) override;
  // A scripted install: seven steps with PumpsPerStep Pump calls each; honors Cancelled() and InstallFailsAtStep.
  InstallOutcome InstallRootfs(const InstallRequest& Request, InstallEvents& Events) override;

  // State the tests change.
  std::string Version = "v0.16.0 \xC2\xB7 FEX-2606";
  RootfsInfo Root {true, "Ubuntu_24_04.sqsh", 19756849561ull};
  FtpInfo Ftp_ {true, 2};
  Capability SteamCap {false, "Needs graphics support \xC2\xB7 not available yet"};
  Capability DesktopCap {false, "Needs graphics support \xC2\xB7 not available yet"};
  Capability ShellCap {true, ""};
  Capability ReinstallCap {true, ""};
  Capability WipeCap {true, ""};
  int WipeCalls = 0;
  bool WipeFails = false;
  // RootFS installer script.
  std::string LiveIndexText;          // what FetchLiveRootfsIndex returns; empty fails ("offline")
  int PumpsPerStep = 4;
  int InstallFailsAtStep = -1;        // fail in this step (0..6)
  int InstallCalls = 0;
  bool PartialRootfs = false;         // files of a running install exist (removed by a cancel or failure)
  bool DownloadKept = false;          // the downloaded image is on the card
  InstallRequest LastRequest;
  // FTP and shell scripts.
  std::string Address = "192.168.1.42";
  bool FtpUp = false;
  FtpConfig LastFtpConfig;
  std::string FtpStartError;                 // set: FtpStart fails with it
  std::vector<FtpClientView> FtpClients;     // what FtpState reports while running
  uint64_t FtpTotalIn = 0, FtpTotalOut = 0;
  std::vector<uint64_t> Disconnected;
  std::vector<std::string> KeyboardAnswers;  // consumed in order by KeyboardText; empty list = cancelled
  std::vector<std::string> KeyboardHeaders;
  FakeShell* LastShell = nullptr;            // owned by the screen once started
  std::string ShellStartError;
  Capability MoreShellsCap;
  std::vector<FakeShell*> Shells;           // every shell started (owned by the screen)
  // App shell (Task 7).
  int Ticks = 0;
  std::map<std::string, std::string> Applied;   // ApplySetting calls (last value per key)
  std::map<std::string, Capability> DevCaps;    // developer actions that are not available
  std::vector<std::pair<std::string, std::string>> DevStarted;  // StartDevSession (Id, Path)

private:
  FakeStorage Store;
};

} // namespace ui
