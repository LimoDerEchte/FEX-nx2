// SPDX-License-Identifier: MIT
// Linux host backend: the same Vulkan renderer on a headless offscreen image (RADV on the dev PC), scripted input and fixed
// System values. Used by `make check-ui`, the host probe and screenshot tests. There is no host window yet (the plan's
// optional window is not implemented; the host renders headless and writes PNG).
#pragma once

#include <deque>
#include <memory>

#include "platform/Platform.h"

namespace ui {

// Input fed by the test or probe: Push events, Poll returns them in order. SizeChanged events come from the display.
class HostScriptedInput final : public Input {
public:
  explicit HostScriptedInput(Display& D) : D(D), LastSerial(D.SizeSerial()) {}
  void Push(const Event& E) { Queue.push_back(E); }
  void Poll(std::vector<Event>& Out) override;
  bool HasMouse() const override { return true; }
  bool HasWheel() const override { return true; }

private:
  Display& D;
  uint32_t LastSerial;
  std::deque<Event> Queue;
};

// Fixed values: wall clock 2026-01-01 12:00:00, 12 GiB free of 64 GiB, 192.168.1.50, battery 80%, no keyboard.
class HostSystem final : public System {
public:
  uint64_t MonotonicMs() override;
  bool WallClock(WallTime& Out) override;
  bool FreeSpace(const char*, uint64_t& FreeBytes, uint64_t& TotalBytes) override;
  bool NetworkAddress(std::string& Dotted) override;
  bool Battery(int& Percent, bool& Charging) override;
  bool ShowKeyboard(const char*, std::string&) override { return false; }
};

class HostPlatform : public Platform {
public:
  // Resizes the headless output; applied at the next Display::BeginFrame (a SizeChanged event follows).
  virtual void SetSize(int Width, int Height) = 0;
  virtual HostScriptedInput& ScriptedInput() = 0;
};

std::unique_ptr<HostPlatform> CreateHostPlatform(const PlatformOptions& Options);

} // namespace ui
