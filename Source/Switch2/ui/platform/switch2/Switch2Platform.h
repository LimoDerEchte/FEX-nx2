// SPDX-License-Identifier: MIT
// Console backend entry points shared by the backend files. Applications use only platform/Platform.h.
#pragma once

namespace ui {

// Makes the default NWindow follow the console resolution. Call it from userAppInit (see Switch2AppInit.cpp).
void Switch2PrepareWindow();
// Set by Switch2PrepareWindow; CreatePlatform logs it so a missing userAppInit shows in the log.
extern bool WindowPrepared;

} // namespace ui
