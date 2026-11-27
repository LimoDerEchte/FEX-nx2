// SPDX-License-Identifier: MIT
// Fonts and icons linked into the binary (.incbin), so the NRO needs nothing on the SD card. UI_ASSET_DIR (a build define)
// is the absolute path of ui/assets. See assets/SOURCES.txt for versions and licenses.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ui {

enum class FontId : uint8_t {
  NunitoRegular,
  NunitoSemiBold,
  NunitoBold,
  NunitoExtraBold,
  MonoRegular,
  MonoSemiBold,
  MonoBold,
  NotoRegular,
  NotoMedium,
  Count
};

// Lucide icons on a 24-unit grid (stroke = currentColor).
enum class IconId : uint8_t {
  Gamepad,
  Monitor,
  TerminalSquare,  // Lucide square-terminal
  Settings,        // the gear
  ChevronRight,
  ChevronDown,
  Plus,
  X,
  Info,
  HardDrive,
  Package,
  ShoppingBag,
  Folder,
  House,
  SdCard,          // Lucide has no SD card icon: memory-stick
  ArrowUp,
  Pencil,
  Eye,
  EyeOff,
  Trash,
  CircleSlash,
  Transfer,        // Lucide arrow-up-down
  File,
  Count
};

struct Blob {
  const uint8_t* Data = nullptr;
  size_t Size = 0;  // icons: the data is also NUL-terminated (one byte past Size)
};

Blob GetFont(FontId Id);
Blob GetIcon(IconId Id);
const char* FontName(FontId Id);
const char* IconName(IconId Id);  // the Lucide file stem, for example "chevron-right"

// License texts and licenses.json, embedded in the NRO. The launcher writes them to sdmc:/fex/licenses/ at first run
// (LicenseStore) and the About page reads them from here. A license `file` in licenses.json is one of these names.
Blob GetLicensesJson();
// The built-in RootFS index (rootfs-index.json, the RootFS_links.json v1 format plus the fields the UI needs), see RootfsIndex.h.
Blob GetRootfsIndexJson();
size_t LicenseFileCount();
const char* LicenseFileName(size_t Index);
Blob LicenseFileData(size_t Index);
Blob FindLicenseFile(const char* Name);  // null Data when the name is not embedded

} // namespace ui
