// SPDX-License-Identifier: MIT
// Diagnostic log shared by the UI platform and render layers. Every init step of a backend goes through Log, so a
// failure on the console can be diagnosed from the SD log alone. The sink is set by the application (the probe appends to
// a file and flushes per line). No Vulkan or libnx types here.
#pragma once

namespace ui {

using LogSink = void (*)(const char* Line);

// A null sink restores the default (stdout).
void SetLogSink(LogSink Sink);
void Log(const char* Format, ...) __attribute__((format(printf, 1, 2)));

} // namespace ui
