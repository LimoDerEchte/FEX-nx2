// SPDX-License-Identifier: ISC
// Copyright 2026 nx-hbmenu Switch 2 port contributors (adapted from ounce-hbmenu switch2/ouncevk_platform.c).
//
// libnx's default __appInit runs, then userAppInit: it makes the default NWindow follow the console's resolution
// (1920x1080 handheld, the TV output setting docked, 3840x2160 at 4K) before the Vulkan surface is created. OunceVK opens
// the GPU when the Vulkan instance is created, and libounce's __appExit closes it.
//
// Link this file into every program that uses the Switch 2 backend. A program that already defines userAppInit (the
// launcher, when the UI is integrated in Task 7) calls ui::Switch2PrepareWindow() from it instead and leaves this file out.
#include <switch.h>

#include "platform/switch2/Switch2Platform.h"

namespace ui {
bool WindowPrepared = false;
void Switch2PrepareWindow() {
  nwindowSetModeDimensions(nwindowGetDefault(), 0, 0, 0, 0);
  WindowPrepared = true;
}
} // namespace ui

extern "C" void userAppInit(void) { ui::Switch2PrepareWindow(); }
