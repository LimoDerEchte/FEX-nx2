// SPDX-License-Identifier: MIT
// The light theme: every color of the theming spec ("Launcher tokens", "Accent presets", "Shell palette"). This file and
// theme_dark.cpp are the only places a hex color literal may appear (checked by `make check-ui`).
#include "theme.h"

namespace ui {

const Theme& LightThemeBase() {
  static const Theme T = [] {
    Theme T;
    T.Mode = ThemeMode::Light;
    T.Preset = AccentPreset::Cyan;
    T.Bg = Hex(0xEBEBEF);
    T.Surface = Hex(0xFFFFFF);
    T.SurfaceRaised = Hex(0xFFFFFF);
    T.SurfaceInset = Hex(0xE4E4EA);
    T.Field = Hex(0xE4E4EA);
    T.LogBg = Hex(0xF7F7F9);
    T.Divider = Hex(0xD2D2D8);
    T.Text = Hex(0x1B1B1F);
    T.TextMuted = Hex(0x5E5E66);
    T.TextField = Hex(0x2E2E34);
    T.TextDim = Hex(0x6E6E78);
    T.Accent = Hex(0x006A84);
    T.OnAccent = Hex(0xFFFFFF);
    T.HintDisc = Hex(0x1B1B1F);
    T.HintLetter = Hex(0xEBEBEF);
    T.ToggleOffTrack = Hex(0x7C7C86);
    T.ToggleOffKnob = Hex(0xFFFFFF);
    T.ToggleOnKnob = Hex(0xFFFFFF);
    T.RadioOff = Hex(0x8A8A94);
    T.ButtonSecondary = Hex(0xDCDCE2);
    T.Scrim = Hex(0x000000, 89);  // 35%
    T.Ok = Hex(0x15803D);
    T.Warn = Hex(0xB45309);
    T.Error = Hex(0xB91C1C);
    T.KdeWindow = Hex(0xEFF0F1);
    T.KdeHeader = Hex(0xDEE0E2);
    T.KdeBorder = Hex(0xC4C9CD);
    T.KdeView = Hex(0xFCFCFC);
    T.KdeText = Hex(0x232629);
    T.KdeTextInactive = Hex(0x57626D);
    T.KdeFocus = Hex(0x1E80BD);
    T.AnsiGreen = Hex(0x0B7558);
    T.AnsiBlue = Hex(0x1A73C2);
    T.AnsiYellow = Hex(0xA15C00);
    T.AnsiRed = Hex(0xC0392B);
    T.AccentPresets[size_t(AccentPreset::Cyan)] = T.Accent;
    T.AccentPresets[size_t(AccentPreset::Red)] = Hex(0xC8102E);
    T.AccentPresets[size_t(AccentPreset::Violet)] = Hex(0x5B3FE0);
    T.AccentPresets[size_t(AccentPreset::Blue)] = Hex(0x1F6FB2);
    return T;
  }();
  return T;
}

} // namespace ui
