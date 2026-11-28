// SPDX-License-Identifier: MIT
// What the screens ask of the launcher: versions, storage, RootFS and FTP state, which launch targets work, and the RootFS wipe.
// The console implementation (launcher/LauncherBackend.cpp, Task 7) sits over the real runtime; FakeBackend (ui/backend, host
// only) serves tests and headless screenshots. No Vulkan or libnx types here.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "RootfsIndex.h"
#include "Storage.h"

namespace ui {

// A feature either works or is drawn disabled with the reason (no faked behavior).
struct Capability {
  bool Available = true;
  std::string Reason;  // shown as the muted reason line when not available
};

struct RootfsInfo {
  bool Installed = false;
  std::string Name;         // for example "Ubuntu_24_04.sqsh"
  uint64_t FreeBytes = 0;   // free space of the SD card
};

struct FtpInfo {
  bool Running = false;
  int Clients = 0;
};

// ---- RootFS installer (Task 5) -------------------------------------------------------------------------------------------
// One RootFS install, behind the backend. The call blocks, but it is cooperative: the installer calls Pump() at least once per
// received chunk and per extracted entry, and the caller draws a frame there (so the Installation screen stays live and B can
// cancel). Steps, in order: 0 Check free space, 1 Download (skipped for a local image), 2 Verify, 3 Extract the RootFS,
// 4 Set up the RootFS, 5 Extra packages, 6 Finish. Progress is the weighted overall fraction 0..1.
struct InstallRequest {
  RootfsImage Image;          // from RootfsImages() (or a newer live entry)
  std::string LocalPath;      // set: install this .sqsh from the SD card ("/fex/images/x.sqsh") instead of downloading
  bool Reinstall = false;     // wipe an existing RootFS first
  bool WipeOnly = false;      // Settings > Wipe RootFS: the screen runs WipeRootfs (2 steps) instead of an install; Image is unused
  bool Extras = true;         // the extra Ubuntu packages our tests need
};

enum class InstallLogKind { Info, Ok, Warn, Error };

class InstallEvents {
public:
  virtual ~InstallEvents() = default;
  virtual void Step(int Index, const std::string& Title) = 0;
  virtual void Progress(double Fraction, const std::string& Detail) = 0;
  virtual void Log(InstallLogKind Kind, const std::string& Line) = 0;
  virtual void Pump() = 0;
  virtual bool Cancelled() = 0;
};

struct InstallOutcome {
  bool Ok = false;
  bool Cancelled = false;   // cancelled by the user: everything the install created is removed
  std::string Error;
  std::string Warning;      // finished, but something needs attention (for example the extra packages are not configured)
};

// ---- FTP server (Task 6, over runtime/GuestFtp.h) ----------------------------------------------------------------------------
struct FtpConfig {
  std::string User = "switch";   // only this user name logs in
  std::string Password;
  uint16_t Port = 1337;          // 1024..65535
  std::string Root = "/";        // the folder clients see first, a RootFS path
  bool Background = false;       // keep serving while a guest program runs (launcher screens and the shell always do)
};

// One connected client, from the server's own counters. BytesIn went to the console (uploads), BytesOut left it (downloads).
struct FtpClientView {
  uint64_t Id = 0;
  std::string Address;           // dotted IPv4
  std::string User;
  bool LoggedIn = false;
  enum class Act : uint8_t { Idle, Receiving, Sending } State = Act::Idle;
  std::string File;
  uint64_t Done = 0, Total = 0;  // current transfer; Total 0 = unknown
  uint64_t BytesIn = 0, BytesOut = 0;
};

struct FtpSnapshot {
  bool Running = false;
  std::string Address;           // the console's address, "" when the console has no network
  uint16_t Port = 0;
  std::vector<FtpClientView> Clients;
  uint64_t TotalIn = 0, TotalOut = 0;  // all clients since the server started
  std::string Error;             // why the last start failed ("" otherwise)
};

enum class KeyboardKind : uint8_t { Text, Password, Number };

// ---- Shell (Task 6) -----------------------------------------------------------------------------------------------------------
// One guest shell: the screen reads its output bytes and writes the keys. Non-blocking.
class ShellSession {
public:
  virtual ~ShellSession() = default;
  virtual size_t Read(char* Out, size_t Max) = 0;     // output bytes since the last call, 0 = none
  virtual void Write(std::string_view Input) = 0;      // keys, already encoded as the guest expects them
  virtual bool Running() = 0;                          // false once the shell exited
  virtual bool ChildRunning() = 0;                     // a foreground process other than the shell itself is running
  virtual void SetForeground(bool Value) { (void)Value; }
  virtual void Resize(int Cols, int Rows) { (void)Cols; (void)Rows; }  // the terminal grid changed (TIOCGWINSZ)
  // The tab title until the program sets one with OSC 0/2 ("" = the screen's default, "Shell" or "Root shell").
  virtual std::string Title() { return {}; }
  // A program exit or session failure returns a note that keeps the tab visible until closed. A normal shell exit returns "".
  virtual std::string ExitNote() { return {}; }
};

// ---- Shell sessions keyed by an id. The console backend uses an independent pty per tab and one shared guest worker.
struct SessionSpec {
  enum class Kind : uint8_t { Shell, RootShell, Program } K = Kind::Shell;
  std::string Id;    // Program: the developer action ("run_elf", "run_elf_single")
  std::string Path;  // Program: the file for the ELF runs
};
struct SessionInfo {
  bool Running = false;       // live session or queued output to drain
  bool ChildRunning = false;  // a process other than the shell itself is alive in it
  std::string Title;          // "" = the default
  std::string ExitNote;       // set once a program ended ("Exited with status 42")
};

class LauncherBackend;
class ShellSession;

class LauncherBackend {
public:
  virtual ~LauncherBackend() = default;
  virtual Storage& GetStorage() = 0;
  // "v<app> · FEX-<fex build>" from the build constants.
  virtual std::string VersionText() = 0;
  virtual RootfsInfo Rootfs() = 0;
  virtual FtpInfo Ftp() = 0;
  virtual Capability Steam() = 0;
  virtual Capability Desktop() = 0;
  virtual Capability Shell() = 0;
  // Settings > Paths. Reinstall opens RootFS setup (Task 5/6); Wipe deletes the RootFS, desktops, stores and home folder.
  virtual Capability Reinstall() = 0;
  virtual Capability Wipe() = 0;
  virtual std::string WipeSummary() = 0;  // "Ubuntu_24_04.sqsh, 6.1 GB, ..." for the confirm dialog
  // Runs on the Installation screen like an install (cooperative, Pump per frame): step 0 "Counting the files", step 1 "Remove the
  // files" with the fraction removed. It cannot be cancelled: a half-wiped RootFS is no use either way.
  virtual InstallOutcome WipeRootfs(InstallEvents& Events) = 0;
  // The Linux home folder, shown as a picker location ("/home/switch/").
  virtual std::string HomeFolder() = 0;

  // The RootFS images: the built-in index. FetchLiveRootfsIndex downloads the project's RootFS_links.json (blocking, with Pump
  // calls through Events); an empty Text and an Error mean the built-in index stays. ChooseIndex(Text) merges a newer entry.
  virtual const RootfsIndex& RootfsImages() = 0;
  virtual bool FetchLiveRootfsIndex(std::string& Text, std::string& Error) = 0;
  virtual InstallOutcome InstallRootfs(const InstallRequest& Request, InstallEvents& Events) = 0;

  // ---- FTP server. Start fails with a message (port in use, no network ...). State is read once per frame.
  virtual std::string NetworkAddress() = 0;                                     // "" when there is no network
  virtual bool FtpStart(const FtpConfig& Config, std::string& Error) = 0;
  virtual void FtpStop() = 0;
  virtual FtpSnapshot FtpState() = 0;
  virtual void FtpDisconnect(uint64_t ClientId) = 0;
  // The software keyboard (text, password) or number pad. false = cancelled.
  virtual bool KeyboardText(const std::string& Header, const std::string& Initial, KeyboardKind Kind, std::string& Out) = 0;

  // ---- Shell. MoreShells reports whether the current runtime can accept another tab.
  virtual std::unique_ptr<ShellSession> StartShell(bool Root, std::string& Error);
  virtual Capability MoreShells() = 0;
  virtual std::string FexBuild() = 0;                                           // "2606"

  // ---- App shell (Task 7)
  // Called once per frame by the app: the FTP server's non-blocking step.
  virtual void Tick() {}
  // A function that draws a frame; the backend may call it from inside a long call (an FTP transfer). Cleared with {} when the app ends.
  virtual void SetFrameHook(std::function<void()> Hook) { (void)Hook; }
  // The settings the launcher itself uses ("log_level", "start_folder"): called for every schema key after the settings load and
  // after Settings saved them.
  virtual void ApplySetting(const std::string& Key, const std::string& Value) { (void)Key; (void)Value; }
  // Developer actions of Settings > Developer: "root_shell" (opened by the app itself), "run_elf", "run_elf_single",
  // "final_pass" (sdmc:/fex/games/final-pass-suite.elf, listed only when that file exists).
  virtual Capability DevAction(const std::string& Id) { (void)Id; return Capability {}; }
  // Starts a developer program asynchronously and returns its terminal tab handle.
  virtual std::unique_ptr<ShellSession> StartDevSession(const std::string& Id, const std::string& Path, std::string& Error);

  // OpenSession queues a launch and returns its id (> 0), or 0 with Error. Read drains the pty master, Write queues input,
  // The first Resize supplies the initial pty size and releases the queued launch. Later calls update the live size. Close
  // invalidates the id and hangs up the terminal.
  virtual int OpenSession(const SessionSpec& Spec, std::string& Error) { (void)Spec; Error = "not available"; return 0; }
  virtual size_t SessionRead(int Id, char* Out, size_t Max) { (void)Id; (void)Out; (void)Max; return 0; }
  virtual void SessionWrite(int Id, std::string_view Input) { (void)Id; (void)Input; }
  virtual void SessionResize(int Id, int Cols, int Rows) { (void)Id; (void)Cols; (void)Rows; }
  virtual void SessionForeground(int Id, bool Value) { (void)Id; (void)Value; }
  virtual void SessionClose(int Id) { (void)Id; }
  virtual SessionInfo SessionState(int Id) { (void)Id; return SessionInfo {}; }

};

// The per-tab handle over the id API: every call goes to the backend with the session's id; closing the handle closes the session.
class IdSession final : public ShellSession {
public:
  IdSession(LauncherBackend& B, int SessionId) : Be(B), Id(SessionId) {}
  ~IdSession() override { Be.SessionClose(Id); }
  size_t Read(char* Out, size_t Max) override { return Be.SessionRead(Id, Out, Max); }
  void Write(std::string_view Input) override { Be.SessionWrite(Id, Input); }
  bool Running() override { return Be.SessionState(Id).Running; }
  bool ChildRunning() override { return Be.SessionState(Id).ChildRunning; }
  void SetForeground(bool Value) override { Be.SessionForeground(Id, Value); }
  void Resize(int Cols, int Rows) override { Be.SessionResize(Id, Cols, Rows); }
  std::string Title() override { return Be.SessionState(Id).Title; }
  std::string ExitNote() override { return Be.SessionState(Id).ExitNote; }
  int SessionId() const { return Id; }

private:
  LauncherBackend& Be;
  int Id;
};

inline std::unique_ptr<ShellSession> LauncherBackend::StartShell(bool Root, std::string& Error) {
  SessionSpec Spec;
  Spec.K = Root ? SessionSpec::Kind::RootShell : SessionSpec::Kind::Shell;
  const int Id = OpenSession(Spec, Error);
  return Id > 0 ? std::make_unique<IdSession>(*this, Id) : nullptr;
}

inline std::unique_ptr<ShellSession> LauncherBackend::StartDevSession(const std::string& Id, const std::string& Path, std::string& Error) {
  SessionSpec Spec;
  Spec.K = SessionSpec::Kind::Program;
  Spec.Id = Id;
  Spec.Path = Path;
  const int Sid = OpenSession(Spec, Error);
  return Sid > 0 ? std::make_unique<IdSession>(*this, Sid) : nullptr;
}

} // namespace ui
