// SPDX-License-Identifier: MIT
// The backend bundle. CreatePlatform is defined by exactly one backend, chosen at link time (platform/switch2 for the
// console, platform/host for Linux). Applications and screens never see which one they got.
#pragma once

#include <memory>

#include "platform/Display.h"
#include "platform/Input.h"
#include "platform/System.h"
#include "render/Renderer.h"

namespace ui {

struct PlatformOptions {
  // Host headless backend only: the initial output size. The console ignores it (the window decides).
  int Width = 1280;
  int Height = 720;
};

class Platform {
public:
  virtual ~Platform() = default;
  virtual Display& GetDisplay() = 0;
  virtual Input& GetInput() = 0;
  virtual System& GetSystem() = 0;
  virtual Renderer& GetRenderer() = 0;
};

// Every init step is logged through ui::Log. Returns null (after logging "FAIL <step>: <reason>") on failure.
std::unique_ptr<Platform> CreatePlatform(const PlatformOptions& Options);

} // namespace ui
