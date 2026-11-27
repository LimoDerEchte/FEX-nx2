// SPDX-License-Identifier: MIT
// Theme tokens (theming spec): one Theme per mode, every color a semantic token. Screens and components read tokens only.
// A hex color literal outside theme_dark.cpp / theme_light.cpp fails `make check-ui`.
#pragma once

#include <cstdint>

#include "render/Renderer.h"

namespace ui {

// X(Field, spec_name): the launcher and shell tokens of the theming spec, in table order. Used by the dump and the tests.
#define UI_THEME_COLORS(X)                                                                                             \
  X(Bg, bg) X(Surface, surface) X(SurfaceRaised, surface_raised) X(SurfaceInset, surface_inset) X(Field, field)       \
  X(LogBg, log_bg) X(Divider, divider) X(Text, text) X(TextMuted, text_muted) X(TextField, text_field)                 \
  X(TextDim, text_dim) X(Accent, accent) X(OnAccent, on_accent) X(HintDisc, hint_disc) X(HintLetter, hint_letter)      \
  X(ToggleOffTrack, toggle_off_track) X(ToggleOffKnob, toggle_off_knob) X(ToggleOnKnob, toggle_on_knob)                \
  X(RadioOff, radio_off) X(ButtonSecondary, button_secondary) X(Scrim, scrim) X(Ok, ok) X(Warn, warn) X(Error, error) \
  X(KdeWindow, kde_window) X(KdeHeader, kde_header) X(KdeBorder, kde_border) X(KdeView, kde_view)                      \
  X(KdeText, kde_text) X(KdeTextInactive, kde_text_inactive) X(KdeFocus, kde_focus) X(AnsiGreen, ansi_green)           \
  X(AnsiBlue, ansi_blue) X(AnsiYellow, ansi_yellow) X(AnsiRed, ansi_red)

enum class ThemeMode : uint8_t { Dark, Light };
enum class AccentPreset : uint8_t { Cyan, Red, Violet, Blue, Count };

const char* AccentName(AccentPreset P);  // "cyan", "red", "violet", "blue" (the stored form, never a hex)
bool ParseAccent(const char* Name, AccentPreset& Out);

// Marks a token nobody set; the completeness test fails if it survives in a theme.
constexpr Color UnsetColor = Color {1, 2, 3, 4};
constexpr Color Hex(uint32_t Rgb, uint8_t Alpha = 255) { return Color {uint8_t(Rgb >> 16), uint8_t(Rgb >> 8), uint8_t(Rgb), Alpha}; }

struct Theme {
  ThemeMode Mode = ThemeMode::Dark;
  AccentPreset Preset = AccentPreset::Cyan;
#define UI_THEME_FIELD(Field, Name) Color Field = UnsetColor;
  UI_THEME_COLORS(UI_THEME_FIELD)
#undef UI_THEME_FIELD
  Color AccentPresets[size_t(AccentPreset::Count)] = {UnsetColor, UnsetColor, UnsetColor, UnsetColor};
};

// The two base themes (cyan accent), defined in theme_dark.cpp and theme_light.cpp.
const Theme& DarkThemeBase();
const Theme& LightThemeBase();

// A theme for (mode, accent preset): the base with Accent replaced by that preset's value. The reference stays valid for
// the whole run, so `g_theme` can point at it.
const Theme& GetTheme(ThemeMode Mode, AccentPreset Accent = AccentPreset::Cyan);

extern const Theme* g_theme;  // null until SetTheme; CurrentTheme() then returns dark, cyan
const Theme& CurrentTheme();
void SetTheme(ThemeMode Mode, AccentPreset Accent);

// WCAG relative luminance contrast ratio of two opaque colors.
double ContrastRatio(Color A, Color B);
// Source-over of Top (its alpha) on Bottom.
Color Blend(Color Top, Color Bottom);

} // namespace ui
