// SPDX-License-Identifier: MIT
// The built-in RootFS index (rootfs-index.json), embedded on its own so the console launcher can use it without the fonts and
// icons of Assets.cpp. Same code for the console and the Linux host.
#include "assets/Assets.h"

#ifndef UI_ASSET_DIR
#error "UI_ASSET_DIR must be the absolute path of ui/assets"
#endif

__asm__(".pushsection .rodata\n.balign 16\n.global ui_asset_rootfs_index_start\nui_asset_rootfs_index_start:\n.incbin \"" UI_ASSET_DIR
        "/rootfs-index.json\"\n.global ui_asset_rootfs_index_end\nui_asset_rootfs_index_end:\n.byte 0\n.popsection\n");
extern "C" const unsigned char ui_asset_rootfs_index_start[];
extern "C" const unsigned char ui_asset_rootfs_index_end[];

namespace ui {
Blob GetRootfsIndexJson() { return Blob {ui_asset_rootfs_index_start, size_t(ui_asset_rootfs_index_end - ui_asset_rootfs_index_start)}; }
} // namespace ui
