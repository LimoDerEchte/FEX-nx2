// SPDX-License-Identifier: MIT
// One guest runtime that serves the launcher's terminal tabs (UI Task 8b). The launcher owns a GuestService: it adds sessions (each a
// program on its own pseudo-terminal whose master the launcher holds), the runtime thread starts them as new processes between
// scheduler passes, and everything that crosses threads is in this header: the master's two locked queues (HostPtyMaster), atomics
// per session and one locked list of launches. The UI thread never touches runtime state.
#pragma once

#include "GuestPty.h"
#include "GuestRuntime.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Switch2Guest {

// What to start. The runtime loads the program itself (through the guest file system, or from the card for a developer program), so
// the inode store has one owner.
struct SessionLaunch {
  std::string Program;                    // a guest path inside the RootFS ("/bin/bash"), or an SD path when OnCard
  bool OnCard {};                         // a developer program of the SD card (its interpreter still resolves in the RootFS)
  std::vector<std::string> Arguments;     // argv[1..]
  std::vector<std::string> Environment;   // the complete environment
  std::optional<std::pair<uint32_t, uint32_t>> Credentials;  // uid, gid; none = the default unprivileged user
  std::string Folder;                     // the launch folder (host side, as RunGuest took it)
  std::string WorkingDirectory;           // the initial working directory
  bool SingleThread {};                   // <program>.single-thread: the process's threads run one at a time
  bool Profile {};                        // EXPERIMENT <program>.profile: sample where guest code spends its time
  bool NoTso {};                          // EXPERIMENT <program>.no-tso: FEX translates without x86 memory ordering
};

// One session as the launcher and the runtime share it.
struct HostSession {
  enum Phase : int { Queued, Running, Ended };
  explicit HostSession(uint64_t SessionId) : Id(SessionId) {}
  const uint64_t Id;
  SessionLaunch Launch;                   // written before Add, read by the runtime thread only
  HostPtyMaster Master;
  std::atomic<int> State {Queued};
  std::atomic<bool> Sized {false};        // the UI published its initial grid before the program runs
  std::atomic<bool> CloseRequested {false};  // the UI dropped the tab: hang up
  std::atomic<bool> Busy {false};         // a foreground process other than the session's first process runs
  std::atomic<int> ExitCode {0};
  std::atomic<int> TermSignal {0};
  std::mutex Mutex;
  std::string Failure;                    // why the session never started
};

// A RootFS picker request shares the live inode store through the scheduler.
struct SessionDirectory {
  std::string Folder;
  std::vector<DirectoryEntry> Entries;
  Result Status {};
  std::atomic<bool> Ready {false};
};

class GuestService final : public GuestHost {
public:
  GuestService();
  ~GuestService() override;
  GuestService(const GuestService&) = delete;
  GuestService& operator=(const GuestService&) = delete;

  // ---- launcher thread ----
  // Queues a session. false when the runtime already decided to end (the caller joins this service and makes a new one).
  bool Add(const std::shared_ptr<HostSession>& Session);
  bool List(const std::shared_ptr<SessionDirectory>& Directory);
  // Starts the runtime thread (the first session was added). SingleBlock: the runtime-wide single-block JIT mode.
  bool Start(bool SingleBlock);
  // The runtime has ended (all sessions over, a stop, or a failure); Join then returns at once.
  bool Finished() const { return Done.load(); }
  // Asks the runtime to stop everything now (all processes end, the inode store is saved).
  void Stop() { StopFlag.store(true); }
  void Join();
  bool Running() const { return Started && !Joined; }

  // ---- runtime thread ----
  std::vector<std::shared_ptr<HostSession>> TakeLaunches();
  std::vector<std::shared_ptr<SessionDirectory>> TakeListings();
  // The runtime has no live session: ends it unless a launch arrived meanwhile. true = it ends (Add refuses from now on).
  bool TryFinish();
  // The runtime stops: Add refuses from now on; returns the launches that never started.
  std::vector<std::shared_ptr<HostSession>> Close();

  // GuestHost: the launcher draws on its own thread; the guest has no console here.
  void Output(const char*, size_t) override {}
  bool Stopped() override { return StopFlag.load(); }
  bool MainTick() override { return true; }

  bool SingleBlock {};

private:
  struct Worker;
  std::mutex Mutex;
  std::vector<std::shared_ptr<HostSession>> Pending;
  std::vector<std::shared_ptr<SessionDirectory>> Listings;
  bool Ending {};
  std::atomic<bool> StopFlag {false}, Done {false};
  bool Started {}, Joined {};
  std::unique_ptr<Worker> Impl;
  friend void RunGuestServiceThread(GuestService&);
};

} // namespace Switch2Guest
