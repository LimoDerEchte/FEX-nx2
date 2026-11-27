// SPDX-License-Identifier: MIT
// What the launcher's console files share: the thread-safe byte queue between a guest thread and the UI thread, the PrintStage sink,
// and the entry points of the RootFS install, and the live index fetch (each in its own file). The UI
// talks to all of it only through ui::LauncherBackend (LauncherBackend.cpp).
#pragma once

#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "Backend.h"

namespace Switch2Guest {

// Bytes from one thread to another. Push never blocks; Pop returns what is there.
class ByteQueue {
public:
  void Push(const char* Data, size_t Size) {
    std::lock_guard<std::mutex> Lock(Mutex);
    Bytes.append(Data, Size);
  }
  void Push(const std::string& Text) { Push(Text.data(), Text.size()); }
  size_t Pop(char* Out, size_t Max) {
    std::lock_guard<std::mutex> Lock(Mutex);
    const size_t Count = Bytes.size() < Max ? Bytes.size() : Max;
    std::memcpy(Out, Bytes.data(), Count);
    Bytes.erase(0, Count);
    return Count;
  }
  size_t Size() {
    std::lock_guard<std::mutex> Lock(Mutex);
    return Bytes.size();
  }

private:
  std::mutex Mutex;
  std::string Bytes;
};

// PrintStage lines (the launcher's progress messages and FEX's log messages) also go to this sink while it is set. Any thread calls it.
void SetStageSink(std::function<void(const char*)> Sink);

// The RootFS install (RootfsInstallHost.cpp): the installer pipeline with the guest filesystem writer, over the UI's events.
// Blocking and cooperative: Events.Pump() is called often, Events.Cancelled() is polled.
ui::InstallOutcome RunRootfsInstall(const ui::InstallRequest& Request, ui::InstallEvents& Events);
// Settings > Wipe RootFS on the Installation screen: step 0 counts the entries, step 1 removes them one by one with progress.
ui::InstallOutcome RunRootfsWipe(ui::InstallEvents& Events);
// Downloads the project's live RootFS_links.json; false with Error when it cannot (no network, HTTP error).
bool FetchLiveRootfsIndexText(std::string& Text, std::string& Error);

} // namespace Switch2Guest
