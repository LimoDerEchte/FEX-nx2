// SPDX-License-Identifier: MIT
#pragma once

#include <memory>

#include "Backend.h"

namespace Switch2Guest {

// The console implementation of ui::LauncherBackend over the real runtime: SD storage, RootFS state, FTP server, RootFS install,
// shell and developer sessions, the software keyboard. LauncherVersion is "0.17.20".
std::unique_ptr<ui::LauncherBackend> CreateLauncherBackend(const char* LauncherVersion);

} // namespace Switch2Guest
