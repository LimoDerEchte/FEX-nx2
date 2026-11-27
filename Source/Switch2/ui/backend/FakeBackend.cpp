// SPDX-License-Identifier: MIT
#include "backend/FakeBackend.h"

#include <algorithm>

#include "PathUtil.h"

namespace ui {

class FakeDirReader final : public DirReader {
public:
  FakeDirReader(std::vector<DirEntry> E, int Wait) : Entries(std::move(E)), Waiting(Wait) {}
  DirStep Next(DirEntry& Out) override {
    if (Waiting > 0) {
      --Waiting;
      return DirStep::Pending;
    }
    if (At >= Entries.size())
      return DirStep::End;
    Out = Entries[At++];
    return DirStep::Entry;
  }

private:
  std::vector<DirEntry> Entries;
  size_t At = 0;
  int Waiting;
};

FakeStorage::FakeStorage() {
  Nodes["sdmc:/"].IsDir = true;
  Nodes["/"].IsDir = true;
  Space["sdmc:/"] = {19756849561ull, 127865454592ull};
  Space["/"] = {12025908428ull, 21474836480ull};
}

void FakeStorage::AddDir(const std::string& Path) {
  const std::string N = NormalizePath(Path);
  if (Nodes.count(N) && Nodes[N].IsDir)
    return;
  if (!IsRootPath(N))
    AddDir(ParentPath(N));
  Nodes[N].IsDir = true;
}

void FakeStorage::AddFile(const std::string& Path, uint64_t Size, int64_t Mtime, const std::string& Data) {
  const std::string N = NormalizePath(Path);
  AddDir(ParentPath(N));
  Node& Nd = Nodes[N];
  Nd.IsDir = false;
  Nd.Size = Data.empty() ? Size : Data.size();
  Nd.Mtime = Mtime;
  Nd.Data = Data;
}

void FakeStorage::RemoveTree(const std::string& Path) {
  const std::string N = NormalizePath(Path);
  const std::string Prefix = N.back() == '/' ? N : N + "/";
  for (auto It = Nodes.begin(); It != Nodes.end();) {
    if (It->first == N || It->first.rfind(Prefix, 0) == 0)
      It = IsRootPath(It->first) ? std::next(It) : Nodes.erase(It);
    else
      ++It;
  }
}

bool FakeStorage::ReadFile(const std::string& Path, std::string& Out) {
  auto It = Nodes.find(NormalizePath(Path));
  if (It == Nodes.end() || It->second.IsDir)
    return false;
  Out = It->second.Data;
  return true;
}

bool FakeStorage::WriteFileAtomic(const std::string& Path, const std::string& Data, std::string& Error) {
  const std::string N = NormalizePath(Path);
  Ops.push_back("write " + N + ".tmp");
  if (FailWrites) {
    Error = "no space left";
    Ops.push_back("remove " + N + ".tmp");
    return false;
  }
  Ops.push_back("rename " + N + ".tmp " + N);
  AddFile(N, Data.size(), 1790000000, Data);
  return true;
}

bool FakeStorage::MakeDirs(const std::string& Path) {
  AddDir(Path);
  return true;
}
bool FakeStorage::IsDir(const std::string& Path) {
  auto It = Nodes.find(NormalizePath(Path));
  return It != Nodes.end() && It->second.IsDir;
}
bool FakeStorage::Exists(const std::string& Path) { return Nodes.count(NormalizePath(Path)) != 0; }

std::unique_ptr<DirReader> FakeStorage::OpenDir(const std::string& Path) {
  const std::string N = NormalizePath(Path);
  auto Self = Nodes.find(N);
  if (Self == Nodes.end() || !Self->second.IsDir)
    return nullptr;
  const std::string Prefix = N.back() == '/' ? N : N + "/";
  std::vector<DirEntry> Out;
  for (auto It = Nodes.lower_bound(Prefix); It != Nodes.end() && It->first.rfind(Prefix, 0) == 0; ++It) {
    const std::string Rest = It->first.substr(Prefix.size());
    if (Rest.empty() || Rest.find('/') != std::string::npos)
      continue;
    DirEntry E;
    E.Name = Rest;
    E.IsDir = It->second.IsDir;
    E.Size = It->second.Size;
    E.Mtime = It->second.Mtime;
    Out.push_back(std::move(E));
  }
  return std::make_unique<FakeDirReader>(std::move(Out), PendingFrames);
}

bool FakeStorage::FreeSpace(const std::string& Path, uint64_t& Free, uint64_t& Total) {
  auto It = Space.find(IsSdPath(Path) ? "sdmc:/" : "/");
  if (It == Space.end())
    return false;
  Free = It->second.first;
  Total = It->second.second;
  return true;
}

FakeBackend::FakeBackend() {
  // A believable card: the FEX folder, a RootFS image, and a RootFS tree with a home folder.
  Store.AddDir("sdmc:/fex/config");
  Store.AddDir("sdmc:/fex/licenses");
  Store.AddFile("sdmc:/fex/rootfs/Ubuntu_24_04.sqsh", 6558183424ull, 1790000000);
  Store.AddFile("sdmc:/fex/rootfs/Ubuntu_22_04.sqsh", 5200000000ull, 1760000000);
  Store.AddFile("sdmc:/fex/rootfs/Ubuntu_24_04.sqsh.sha256", 96, 1790000000);
  Store.AddFile("sdmc:/fex/rootfs/notes.txt", 1203, 1780000000);
  Store.AddFile("sdmc:/fex/rootfs/.hidden-cache", 10, 1780000000);
  Store.AddDir("sdmc:/fex/rootfs/old");
  Store.AddFile("sdmc:/switch/hbmenu.nro", 1850000, 1700000000);
  Store.AddFile("sdmc:/readme.txt", 400, 1700000000);
  Store.AddDir("sdmc:/roms");
  Store.AddDir("sdmc:/atmosphere");
  Store.AddDir("/fex/games");
  Store.AddDir("/fex/games/demo");
  Store.AddFile("/home/switch/notes.txt", 2048, 1785000000);
  Store.AddFile("/home/switch/.bashrc", 3771, 1785000000);
  Store.AddDir("/home/switch/Documents");
  Store.AddDir("/usr/bin");
  Store.AddFile("/usr/bin/startplasma-wayland", 14000, 1780000000);
  Store.AddFile("/usr/bin/bash", 1396520, 1780000000);
  Store.AddDir("/etc");
  Store.AddDir("/var");
}

InstallOutcome FakeBackend::WipeRootfs(InstallEvents& Events) {
  ++WipeCalls;
  InstallOutcome Out;
  constexpr uint64_t Entries = 400;
  Events.Step(0, "Counting the files");
  for (uint64_t Found = 100; Found <= Entries; Found += 100) {
    Events.Progress(0, std::to_string(Found) + " found");
    Events.Pump();
  }
  Root.Installed = false;  // the marker goes first
  Events.Step(1, "Removing the files");
  for (int Pump = 1; Pump <= PumpsPerStep; ++Pump) {
    const uint64_t Done = Entries * uint64_t(Pump) / uint64_t(PumpsPerStep);
    if (WipeFails && Pump * 2 > PumpsPerStep) {
      Out.Error = "Could not delete /fex/rootfs/usr (0x00000001)";
      Events.Log(InstallLogKind::Error, Out.Error);
      return Out;
    }
    Events.Progress(double(Done) / double(Entries), std::to_string(Done) + " of " + std::to_string(Entries));
    Events.Pump();
  }
  Store.RemoveTree("sdmc:/fex/rootfs");
  Events.Log(InstallLogKind::Info, "Removed " + std::to_string(Entries) + " files and folders");
  Out.Ok = true;
  return Out;
}

bool FakeBackend::FetchLiveRootfsIndex(std::string& Text, std::string& Error) {
  Text = LiveIndexText;
  if (Text.empty()) {
    Error = "could not reach rootfs.fex-emu.gg";
    return false;
  }
  return true;
}

bool FakeBackend::FtpStart(const FtpConfig& Config, std::string& Error) {
  if (!FtpStartError.empty()) {
    Error = FtpStartError;
    return false;
  }
  if (Address.empty()) {
    Error = "The console is not connected to a network";
    return false;
  }
  LastFtpConfig = Config;
  FtpUp = true;
  Ftp_.Running = true;
  return true;
}

void FakeBackend::FtpStop() {
  FtpUp = false;
  Ftp_.Running = false;
  Ftp_.Clients = 0;
}

FtpSnapshot FakeBackend::FtpState() {
  FtpSnapshot S;
  S.Running = FtpUp;
  S.Address = Address;
  S.Port = LastFtpConfig.Port;
  if (FtpUp) {
    S.Clients = FtpClients;
    S.TotalIn = FtpTotalIn;
    S.TotalOut = FtpTotalOut;
    Ftp_.Clients = int(FtpClients.size());
  }
  return S;
}

void FakeBackend::FtpDisconnect(uint64_t ClientId) {
  Disconnected.push_back(ClientId);
  FtpClients.erase(std::remove_if(FtpClients.begin(), FtpClients.end(), [&](const FtpClientView& C) { return C.Id == ClientId; }), FtpClients.end());
}

bool FakeBackend::KeyboardText(const std::string& Header, const std::string&, KeyboardKind, std::string& Out) {
  KeyboardHeaders.push_back(Header);
  if (KeyboardAnswers.empty())
    return false;
  Out = KeyboardAnswers.front();
  KeyboardAnswers.erase(KeyboardAnswers.begin());
  return true;
}

std::unique_ptr<ShellSession> FakeBackend::StartShell(bool, std::string& Error) {
  if (!ShellStartError.empty()) {
    Error = ShellStartError;
    return nullptr;
  }
  auto S = std::make_unique<FakeShell>();
  LastShell = S.get();
  Shells.push_back(S.get());
  S->Watch = &LastShell;
  return S;
}

std::unique_ptr<ShellSession> FakeBackend::StartDevSession(const std::string& Id, const std::string& Path, std::string& Error) {
  if (!DevAction(Id).Available) {
    Error = DevAction(Id).Reason;
    return nullptr;
  }
  DevStarted.emplace_back(Id, Path);
  auto S = std::make_unique<FakeShell>();
  S->TitleText = Id;
  S->Note = "Exited with status 42";
  S->Alive = false;
  LastShell = S.get();
  Shells.push_back(S.get());
  S->Watch = &LastShell;
  return S;
}

InstallOutcome FakeBackend::InstallRootfs(const InstallRequest& Request, InstallEvents& Events) {
  static const char* const Steps[] = {"Check free space", "Download", "Verify", "Extract the RootFS", "Set up the RootFS", "Extra packages", "Finish"};
  static const double Weights[] = {1, 30, 6, 40, 2, 20, 1};
  ++InstallCalls;
  LastRequest = Request;
  InstallOutcome Outcome;
  PartialRootfs = false;
  double Before = 0;
  // What the real installer's cleanup does (GuestRootfsInstall.h): a cancel removes the RootFS and the marker; the downloaded
  // image stays once its hash check passed (Verify finished), is removed on a cancel before that; a failure keeps the image.
  auto Cleanup = [&](bool Cancelled, int Step) {
    PartialRootfs = false;
    if (Cancelled && Request.LocalPath.empty() && Step < 3)
      DownloadKept = false;
    Root.Installed = false;
  };
  for (int Step = 0; Step < 7; ++Step) {
    if (Step == 1 && !Request.LocalPath.empty()) {
      Before += Weights[Step];
      continue;
    }
    Events.Step(Step, Step == 1 ? std::string("Download ") + Request.Image.FileName : std::string(Steps[Step]));
    if (Step == 3)
      PartialRootfs = true;
    Events.Log(InstallLogKind::Info, std::string("Step ") + std::to_string(Step + 1) + "/7  " + Steps[Step]);
    for (int Pump = 1; Pump <= PumpsPerStep; ++Pump) {
      Events.Progress((Before + Weights[Step] * Pump / PumpsPerStep) / 100.0, std::string(Steps[Step]) + " " + std::to_string(Pump));
      if (Step == 3 && Pump == PumpsPerStep / 2)
        Events.Log(InstallLogKind::Warn, "Skipped device node /dev/null");
      Events.Pump();
      if (Events.Cancelled()) {
        Outcome.Cancelled = true;
        Outcome.Error = "Cancelled";
        Events.Log(InstallLogKind::Info, "Cancelled: removing everything the install created");
        Cleanup(true, Step);
        return Outcome;
      }
    }
    if (Step == 1)
      DownloadKept = true;
    if (Step == InstallFailsAtStep) {
      Outcome.Error = std::string("The ") + Steps[Step] + " step failed";
      Events.Log(InstallLogKind::Error, Outcome.Error);
      Cleanup(false, Step);
      return Outcome;
    }
    Events.Log(InstallLogKind::Ok, std::string(Steps[Step]) + " done");
    Before += Weights[Step];
  }
  PartialRootfs = false;
  Root.Installed = true;
  Root.Name = Request.Image.FileName;
  Outcome.Ok = true;
  return Outcome;
}

} // namespace ui
