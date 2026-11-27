// SPDX-License-Identifier: MIT
// EXPERIMENT UI probe core: draws and reads input only through ui::Platform (Display, Input, System, Renderer), so the
// console and the host run the same code. The mains (UiProbeSwitch2Main.cpp, UiProbeHostMain.cpp) choose the backend at
// link time.
#pragma once

#include <functional>

#include "platform/Platform.h"

namespace probe {

constexpr const char* Version = "ui-probe 3";

struct Options {
  int MaxFrames = 0;          // 0 = run until + is pressed
  uint64_t FixedFrameMs = 0;  // nonzero: animation time = frame * FixedFrameMs (deterministic host screenshots)
  std::function<void(int Frame, ui::Platform&)> OnFrame;  // before each frame (host: scripted resize)
};

// Returns 0 on a clean exit, 1 on a failure (logged as "FAIL <step>: ...").
int Run(ui::Platform& P, const Options& O);

} // namespace probe
