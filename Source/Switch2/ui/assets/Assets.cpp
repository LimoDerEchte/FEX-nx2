// SPDX-License-Identifier: MIT
// Embedded fonts and icons (.incbin of the files under ui/assets). Same code for the console and the Linux host.
#include "assets/Assets.h"

#include <cstring>

#ifndef UI_ASSET_DIR
#error "UI_ASSET_DIR must be the absolute path of ui/assets"
#endif

#define UI_EMBED(Sym, File)                                                                                            \
  __asm__(".pushsection .rodata\n.balign 16\n.global ui_asset_" #Sym "_start\nui_asset_" #Sym "_start:\n.incbin \""    \
          UI_ASSET_DIR "/" File "\"\n.global ui_asset_" #Sym "_end\nui_asset_" #Sym "_end:\n.byte 0\n.popsection\n");   \
  extern "C" const unsigned char ui_asset_##Sym##_start[];                                                              \
  extern "C" const unsigned char ui_asset_##Sym##_end[];

UI_EMBED(font_NunitoRegular, "fonts/NunitoSans-Regular.ttf")
UI_EMBED(font_NunitoSemiBold, "fonts/NunitoSans-SemiBold.ttf")
UI_EMBED(font_NunitoBold, "fonts/NunitoSans-Bold.ttf")
UI_EMBED(font_NunitoExtraBold, "fonts/NunitoSans-ExtraBold.ttf")
UI_EMBED(font_MonoRegular, "fonts/JetBrainsMono-Regular.ttf")
UI_EMBED(font_MonoSemiBold, "fonts/JetBrainsMono-SemiBold.ttf")
UI_EMBED(font_MonoBold, "fonts/JetBrainsMono-Bold.ttf")
UI_EMBED(font_NotoRegular, "fonts/NotoSans-Regular.ttf")
UI_EMBED(font_NotoMedium, "fonts/NotoSans-Medium.ttf")
UI_EMBED(icon_Gamepad, "icons/gamepad.svg")
UI_EMBED(icon_Monitor, "icons/monitor.svg")
UI_EMBED(icon_TerminalSquare, "icons/square-terminal.svg")
UI_EMBED(icon_Settings, "icons/settings.svg")
UI_EMBED(icon_ChevronRight, "icons/chevron-right.svg")
UI_EMBED(icon_ChevronDown, "icons/chevron-down.svg")
UI_EMBED(icon_Plus, "icons/plus.svg")
UI_EMBED(icon_X, "icons/x.svg")
UI_EMBED(icon_Info, "icons/info.svg")
UI_EMBED(icon_HardDrive, "icons/hard-drive.svg")
UI_EMBED(icon_Package, "icons/package.svg")
UI_EMBED(icon_ShoppingBag, "icons/shopping-bag.svg")
UI_EMBED(icon_Folder, "icons/folder.svg")
UI_EMBED(icon_House, "icons/house.svg")
UI_EMBED(icon_SdCard, "icons/memory-stick.svg")
UI_EMBED(icon_ArrowUp, "icons/arrow-up.svg")
UI_EMBED(icon_Pencil, "icons/pencil.svg")
UI_EMBED(icon_Eye, "icons/eye.svg")
UI_EMBED(icon_EyeOff, "icons/eye-off.svg")
UI_EMBED(icon_Trash, "icons/trash.svg")
UI_EMBED(icon_CircleSlash, "icons/circle-slash.svg")
UI_EMBED(icon_Transfer, "icons/arrow-up-down.svg")
UI_EMBED(icon_File, "icons/file.svg")
UI_EMBED(licenses_json, "licenses.json")
UI_EMBED(lic_0, "licenses/FEX-Emu-MIT.txt")
UI_EMBED(lic_1, "licenses/FreeType-FTL.txt")
UI_EMBED(lic_2, "licenses/HarfBuzz-MIT.txt")
UI_EMBED(lic_3, "licenses/NanoSVG-LICENSE.txt")
UI_EMBED(lic_4, "licenses/LUCIDE-LICENSE.txt")
UI_EMBED(lic_5, "licenses/NunitoSans-OFL.txt")
UI_EMBED(lic_6, "licenses/JetBrainsMono-OFL.txt")
UI_EMBED(lic_7, "licenses/NotoSans-OFL.txt")
UI_EMBED(lic_8, "licenses/hbmenu-ISC.txt")
UI_EMBED(lic_9, "licenses/libnx-ISC.txt")
UI_EMBED(lic_10, "licenses/libounce-ISC.txt")
UI_EMBED(lic_11, "licenses/zlib-LICENSE.txt")
UI_EMBED(lic_12, "licenses/libpng-LICENSE.txt")
UI_EMBED(lic_13, "licenses/bzip2-LICENSE.txt")
UI_EMBED(lic_14, "licenses/Zstandard-BSD.txt")
UI_EMBED(lic_15, "licenses/xxHash-BSD2.txt")
UI_EMBED(lic_16, "licenses/XZ-Embedded-0BSD.txt")

namespace ui {
namespace {
struct Entry {
  const unsigned char* Start;
  const unsigned char* End;
  const char* Name;
};

const Entry Fonts[] = {
    {ui_asset_font_NunitoRegular_start, ui_asset_font_NunitoRegular_end, "Nunito Sans 400"},
    {ui_asset_font_NunitoSemiBold_start, ui_asset_font_NunitoSemiBold_end, "Nunito Sans 600"},
    {ui_asset_font_NunitoBold_start, ui_asset_font_NunitoBold_end, "Nunito Sans 700"},
    {ui_asset_font_NunitoExtraBold_start, ui_asset_font_NunitoExtraBold_end, "Nunito Sans 800"},
    {ui_asset_font_MonoRegular_start, ui_asset_font_MonoRegular_end, "JetBrains Mono 400"},
    {ui_asset_font_MonoSemiBold_start, ui_asset_font_MonoSemiBold_end, "JetBrains Mono 600"},
    {ui_asset_font_MonoBold_start, ui_asset_font_MonoBold_end, "JetBrains Mono 700"},
    {ui_asset_font_NotoRegular_start, ui_asset_font_NotoRegular_end, "Noto Sans 400"},
    {ui_asset_font_NotoMedium_start, ui_asset_font_NotoMedium_end, "Noto Sans 500"},
};
const Entry Icons[] = {
    {ui_asset_icon_Gamepad_start, ui_asset_icon_Gamepad_end, "gamepad"},
    {ui_asset_icon_Monitor_start, ui_asset_icon_Monitor_end, "monitor"},
    {ui_asset_icon_TerminalSquare_start, ui_asset_icon_TerminalSquare_end, "square-terminal"},
    {ui_asset_icon_Settings_start, ui_asset_icon_Settings_end, "settings"},
    {ui_asset_icon_ChevronRight_start, ui_asset_icon_ChevronRight_end, "chevron-right"},
    {ui_asset_icon_ChevronDown_start, ui_asset_icon_ChevronDown_end, "chevron-down"},
    {ui_asset_icon_Plus_start, ui_asset_icon_Plus_end, "plus"},
    {ui_asset_icon_X_start, ui_asset_icon_X_end, "x"},
    {ui_asset_icon_Info_start, ui_asset_icon_Info_end, "info"},
    {ui_asset_icon_HardDrive_start, ui_asset_icon_HardDrive_end, "hard-drive"},
    {ui_asset_icon_Package_start, ui_asset_icon_Package_end, "package"},
    {ui_asset_icon_ShoppingBag_start, ui_asset_icon_ShoppingBag_end, "shopping-bag"},
    {ui_asset_icon_Folder_start, ui_asset_icon_Folder_end, "folder"},
    {ui_asset_icon_House_start, ui_asset_icon_House_end, "house"},
    {ui_asset_icon_SdCard_start, ui_asset_icon_SdCard_end, "memory-stick"},
    {ui_asset_icon_ArrowUp_start, ui_asset_icon_ArrowUp_end, "arrow-up"},
    {ui_asset_icon_Pencil_start, ui_asset_icon_Pencil_end, "pencil"},
    {ui_asset_icon_Eye_start, ui_asset_icon_Eye_end, "eye"},
    {ui_asset_icon_EyeOff_start, ui_asset_icon_EyeOff_end, "eye-off"},
    {ui_asset_icon_Trash_start, ui_asset_icon_Trash_end, "trash"},
    {ui_asset_icon_CircleSlash_start, ui_asset_icon_CircleSlash_end, "circle-slash"},
    {ui_asset_icon_Transfer_start, ui_asset_icon_Transfer_end, "arrow-up-down"},
    {ui_asset_icon_File_start, ui_asset_icon_File_end, "file"},
};
const Entry LicenseFiles[] = {
    {ui_asset_lic_0_start, ui_asset_lic_0_end, "FEX-Emu-MIT.txt"},
    {ui_asset_lic_1_start, ui_asset_lic_1_end, "FreeType-FTL.txt"},
    {ui_asset_lic_2_start, ui_asset_lic_2_end, "HarfBuzz-MIT.txt"},
    {ui_asset_lic_3_start, ui_asset_lic_3_end, "NanoSVG-LICENSE.txt"},
    {ui_asset_lic_4_start, ui_asset_lic_4_end, "LUCIDE-LICENSE.txt"},
    {ui_asset_lic_5_start, ui_asset_lic_5_end, "NunitoSans-OFL.txt"},
    {ui_asset_lic_6_start, ui_asset_lic_6_end, "JetBrainsMono-OFL.txt"},
    {ui_asset_lic_7_start, ui_asset_lic_7_end, "NotoSans-OFL.txt"},
    {ui_asset_lic_8_start, ui_asset_lic_8_end, "hbmenu-ISC.txt"},
    {ui_asset_lic_9_start, ui_asset_lic_9_end, "libnx-ISC.txt"},
    {ui_asset_lic_10_start, ui_asset_lic_10_end, "libounce-ISC.txt"},
    {ui_asset_lic_11_start, ui_asset_lic_11_end, "zlib-LICENSE.txt"},
    {ui_asset_lic_12_start, ui_asset_lic_12_end, "libpng-LICENSE.txt"},
    {ui_asset_lic_13_start, ui_asset_lic_13_end, "bzip2-LICENSE.txt"},
    {ui_asset_lic_14_start, ui_asset_lic_14_end, "Zstandard-BSD.txt"},
    {ui_asset_lic_15_start, ui_asset_lic_15_end, "xxHash-BSD2.txt"},
    {ui_asset_lic_16_start, ui_asset_lic_16_end, "XZ-Embedded-0BSD.txt"},
};
static_assert(sizeof(Fonts) / sizeof(Fonts[0]) == size_t(FontId::Count), "font table out of sync with FontId");
static_assert(sizeof(Icons) / sizeof(Icons[0]) == size_t(IconId::Count), "icon table out of sync with IconId");

Blob ToBlob(const Entry& E) { return Blob {E.Start, size_t(E.End - E.Start)}; }
} // namespace

Blob GetFont(FontId Id) { return size_t(Id) < size_t(FontId::Count) ? ToBlob(Fonts[size_t(Id)]) : Blob {}; }
Blob GetIcon(IconId Id) { return size_t(Id) < size_t(IconId::Count) ? ToBlob(Icons[size_t(Id)]) : Blob {}; }
const char* FontName(FontId Id) { return size_t(Id) < size_t(FontId::Count) ? Fonts[size_t(Id)].Name : "?"; }
const char* IconName(IconId Id) { return size_t(Id) < size_t(IconId::Count) ? Icons[size_t(Id)].Name : "?"; }


Blob GetLicensesJson() { return Blob {ui_asset_licenses_json_start, size_t(ui_asset_licenses_json_end - ui_asset_licenses_json_start)}; }
size_t LicenseFileCount() { return sizeof(LicenseFiles) / sizeof(LicenseFiles[0]); }
const char* LicenseFileName(size_t Index) { return Index < LicenseFileCount() ? LicenseFiles[Index].Name : ""; }
Blob LicenseFileData(size_t Index) { return Index < LicenseFileCount() ? ToBlob(LicenseFiles[Index]) : Blob {}; }
Blob FindLicenseFile(const char* Name) {
  for (size_t I = 0; I < LicenseFileCount(); ++I)
    if (std::strcmp(LicenseFiles[I].Name, Name) == 0)
      return ToBlob(LicenseFiles[I]);
  return Blob {};
}

} // namespace ui
