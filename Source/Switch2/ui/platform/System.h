// SPDX-License-Identifier: MIT
// System services the UI needs: clock, free space, network address, battery, software keyboard.
// Task 1 console backend implements the clock only; the other calls return false ("not available") until the tasks that
// need them (Task 4 file picker, Task 6 FTP and Shell screens). No libnx types here.
#pragma once

#include <cstdint>
#include <string>

namespace ui {

struct WallTime {
  int Year = 0, Month = 0, Day = 0, Hour = 0, Minute = 0, Second = 0;
};

class System {
public:
  virtual ~System() = default;
  // Monotonic milliseconds since an arbitrary start.
  virtual uint64_t MonotonicMs() = 0;
  // Local wall clock. false when the console clock could not be read.
  virtual bool WallClock(WallTime& Out) = 0;
  virtual bool FreeSpace(const char* Path, uint64_t& FreeBytes, uint64_t& TotalBytes) = 0;
  virtual bool NetworkAddress(std::string& Dotted) = 0;
  virtual bool Battery(int& Percent, bool& Charging) = 0;
  // Software keyboard. false = unavailable or cancelled.
  virtual bool ShowKeyboard(const char* Header, std::string& Text) = 0;
  // false once the console asks the application to close (applet main loop). Call once per frame; the host backends return true.
  virtual bool KeepRunning() { return true; }
};

} // namespace ui
