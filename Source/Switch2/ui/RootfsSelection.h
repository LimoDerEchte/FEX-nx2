// SPDX-License-Identifier: MIT
// What the RootFS setup screen remembers: base system, desktop, stores and the extras choice, in ui.json (rootfs_base, rootfs_desktop,
// rootfs_stores, rootfs_extras). The installer owns sdmc:/fex/config/rootfs.json (its "installed" marker, deleted at the start of
// every install), so the choices cannot live there. No Vulkan or libnx types here.
#pragma once

#include <string>
#include <vector>

#include "RootfsIndex.h"
#include "SettingsSchema.h"

namespace ui {

struct RootfsSelection {
  std::string Base;                 // "ubuntu-24.04"
  std::string Desktop = "none";     // only "none" exists until graphics does
  std::vector<std::string> Stores;  // none exist yet
  bool Extras = true;               // the extra Ubuntu packages (InstallRequest::Extras)

  // The selection key of an image: DistroMatch + "-" + DistroVersion.
  static std::string KeyOf(const RootfsImage& I) { return I.DistroMatch + "-" + I.DistroVersion; }
  // Reads ui.json; a missing or unknown base falls back to the first selectable image of the index, other values to defaults.
  static RootfsSelection Load(Storage& S, const SettingsPaths& P, const RootfsIndex& Index);
  bool Save(Storage& S, const SettingsPaths& P, std::string& Error) const;
};

} // namespace ui
