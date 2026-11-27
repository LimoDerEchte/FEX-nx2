// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>

#include "GuestElf.h"
#include "GuestFtp.h"
#include "GuestRootfsSetup.h"

#include <switch.h>

#include <memory>
#include <optional>
#include <span>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Switch2Guest {
enum class LogLevel { Standard, Detailed, Trace };
extern std::atomic<LogLevel> Logging;
extern bool GraphicsActive;

void WriteLog(const char* Text);
// EXPERIMENT (multithreading scaling): ticks this host thread spent in WriteLog.
extern thread_local uint64_t LogWriteTicks;
// Returns all guest arena memory and the loader's heap size; call once before the launcher exits.
void ShutdownGuestArena();
void WriteGuestOutput(const char* Text);
void PrintStage(const char* Text);
void WriteRunCheckpoint(const char* Text);
void BeginGuestLog();
void EndGuestLog();

struct DirectoryEntry {
  std::string Name;
  bool IsDirectory;
  bool IsSymlink {};
  uint8_t SpecialType {};
};
class PackedFileExtents;
const char* ReadElf(const char* Path, ElfImage& Image, uint64_t Limit = UINT64_MAX,
                    const PackedFileExtents* Extents = nullptr);
// Reads the first Buffer.size() bytes (fewer for a shorter file); returns the
// count, or a negative errno.
int64_t ReadFileHead(const char* Path, std::span<char> Buffer, uint64_t Size, const PackedFileExtents* Extents);
Result ReadDirectory(const std::string& Folder, std::vector<DirectoryEntry>& Entries);
const char* ReadRootfsElf(std::string_view Name, ElfImage& Image, ElfImage& Interpreter);
// Loads one ELF file named by a guest path (symlinks followed through the guest namespace: on Ubuntu /lib64 is a link into /usr/lib).
// Not for use while a guest runtime runs (it reads the inode store).
Result ReadRootfsDirectory(const std::string& Folder, std::vector<DirectoryEntry>& Entries);

// The launcher's side of guest runs: where the terminal output goes, where typed bytes come from, and when to stop. Without a host
// (null, the default) a run writes to the libnx text console and reads the controller, as the text launcher did. With a host,
// every run executes on its own worker thread and RunGuest keeps calling MainTick() on the calling thread, so the launcher can
// draw frames and poll input while the guest runs; the other calls come from the guest thread and must be thread-safe.
class GuestHost {
public:
  virtual ~GuestHost() = default;
  // The guest wrote to the terminal (an interactive run: as written, escape sequences included) or the line discipline echoed.
  virtual void Output(const char* Data, size_t Size) = 0;
  // Typed bytes (a key is one or more bytes; arrows are escape sequences). Returns the count copied, 0 = none.
  virtual size_t Input(char* Out, size_t Max) { (void)Out; (void)Max; return 0; }
  // true = end the run now (the user left the tab, the app is closing).
  virtual bool Stopped() { return false; }
  // The terminal size for TIOCGWINSZ (SIGWINCH goes to the foreground group when it changes).
  virtual void TerminalSize(int& Cols, int& Rows) { Cols = 80; Rows = 25; }
  // Whether a process other than the first one is alive (polled about 20 times a second).
  virtual void ChildProcesses(bool Alive) { (void)Alive; }
  // Calling thread, repeatedly while the guest runs: draw a frame and poll input. false = the app is closing.
  virtual bool MainTick() = 0;
};
void SetGuestHost(GuestHost* Host);

// Sets the user and group IDs of the next launched guest process, as a
// login or sudo would. std::nullopt restores the default unprivileged user.
void SetLaunchCredentials(std::optional<std::pair<uint32_t, uint32_t>> Credentials);

// Lets the next launch interrupt the filesystem recovery probe's journaled
// move into /.fex-recovery-test. Set from a "<program>.recovery-test" file.
void SetRecoveryTestLaunch(bool Enabled);
// Linux file operations on the guest root filesystem for the FTP server,
// with the same semantics guest programs see. Null if guest memory for the
// transfer buffers could not be reserved.
std::unique_ptr<FtpFileSystem> OpenGuestFileSystem();
// dpkg-style unpack operations on the guest root filesystem, acting as root,
// for the RootFS install. Null if guest memory
// could not be reserved. Creates through it skip the crash-safe intent
// protocol and journal frames are not flushed one by one: the install wipes
// the tree when it fails.
std::unique_ptr<RootfsSetup::RootfsWriter> OpenGuestRootfsWriter();
// Deletes /fex/rootfs, its inode metadata and the installed marker (/fex/config/rootfs.json), leaving empty folders. Nothing
// may be running in the guest. Backs the Settings "Wipe RootFS" action and a reinstall. It counts the entries first, then deletes
// them one SD call at a time (a single recursive delete holds the SD service for a minute and blocks the HOME menu). Tick gets
// (entries removed, total): total is 0 while counting, and the first value is the count so far. Tick may be called from any thread.
// Error names what failed.
bool WipeGuestRootfs(std::string& Error, const std::function<void(uint64_t Done, uint64_t Total)>& Tick = {});

std::optional<uint64_t> RunGuest(uint64_t Entry, std::span<const Segment> Segments, const std::string& Folder = "/",
                                 std::string_view Program = "probe", std::span<const std::string> Arguments = {},
                                 const ElfImage* ProgramImage = nullptr, uint64_t InterpreterBase = 0,
                                 const std::string& WorkingDirectory = "/", bool SingleBlock = false, bool Interactive = false);
} // namespace Switch2Guest
