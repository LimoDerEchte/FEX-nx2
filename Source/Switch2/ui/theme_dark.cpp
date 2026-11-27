// SPDX-License-Identifier: MIT
// The dark theme: every color of the theming spec ("Launcher tokens", "Accent presets", "Shell palette"). This file and
// theme_light.cpp are the only places a hex color literal may appear (checked by `make check-ui`).
#include "theme.h"

namespace ui {

const Theme& DarkThemeBase() {
  static const Theme T = [] {
    Theme T;
    T.Mode = ThemeMode::Dark;
    T.Preset = AccentPreset::Cyan;
    T.Bg = Hex(0x1B1B1F);
    T.Surface = Hex(0x2A2A30);
    T.SurfaceRaised = Hex(0x2C2C32);
    T.SurfaceInset = Hex(0x34343B);
    T.Field = Hex(0x26262B);
    T.LogBg = Hex(0x121215);
    T.Divider = Hex(0x34343A);
    T.Text = Hex(0xF2F2F4);
    T.TextMuted = Hex(0xA6A6AE);
    T.TextField = Hex(0xDCDCE2);
    T.TextDim = Hex(0x8A8A94);
    T.Accent = Hex(0x00C3E3);
    T.OnAccent = Hex(0x1B1B1F);
    T.HintDisc = Hex(0xF2F2F4);
    T.HintLetter = Hex(0x1B1B1F);
    T.ToggleOffTrack = Hex(0x4A4A52);
    T.ToggleOffKnob = Hex(0xC9C9D0);
    T.ToggleOnKnob = Hex(0xFFFFFF);
    T.RadioOff = Hex(0x6E6E78);
    T.ButtonSecondary = Hex(0x3A3A42);
    T.Scrim = Hex(0x000000, 158);  // 62%
    T.Ok = Hex(0x4ADE80);
    T.Warn = Hex(0xFBBF24);
    T.Error = Hex(0xF87171);
    T.KdeWindow = Hex(0x202326);
    T.KdeHeader = Hex(0x2A2E32);
    T.KdeBorder = Hex(0x141618);
    T.KdeView = Hex(0x232627);
    T.KdeText = Hex(0xFCFCFC);
    T.KdeTextInactive = Hex(0xBDC3C7);
    T.KdeFocus = Hex(0x3DAEE9);
    T.AnsiGreen = Hex(0x1CDC9A);
    T.AnsiBlue = Hex(0x1D99F3);
    T.AnsiYellow = Hex(0xFDBC4B);
    T.AnsiRed = Hex(0xED1515);
    T.AccentPresets[size_t(AccentPreset::Cyan)] = T.Accent;
    T.AccentPresets[size_t(AccentPreset::Red)] = Hex(0xE60012);
    T.AccentPresets[size_t(AccentPreset::Violet)] = Hex(0x7C5CFF);
    T.AccentPresets[size_t(AccentPreset::Blue)] = Hex(0x3DAEE9);
    return T;
  }();
  return T;
}

} // namespace ui
