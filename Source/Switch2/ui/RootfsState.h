// SPDX-License-Identifier: MIT
// Whether a RootFS is installed, decided from the files on the card (the installer's marker, or a populated /fex/rootfs such as the
// developer bundle's Debian tree). The app starts in RootFS setup (first run) when it is not. No Vulkan or libnx types here.
#pragma once

#include "Backend.h"
#include "Storage.h"

namespace ui {

inline constexpr const char* RootfsMarkerPath = "sdmc:/fex/config/rootfs.json";
inline constexpr const char* RootfsFolder = "sdmc:/fex/rootfs";

// Installed, the name for the Settings and Reinstall texts ("Ubuntu 24.04", or "RootFS" for a tree without a marker) and the free
// space of the card. An empty card gives Installed = false.
RootfsInfo DetectRootfs(Storage& S);

} // namespace ui
