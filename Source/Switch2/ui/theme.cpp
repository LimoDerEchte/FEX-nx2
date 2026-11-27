// SPDX-License-Identifier: MIT
#include "theme.h"

#include <cmath>
#include <cstring>

namespace ui {

const Theme* g_theme = nullptr;

namespace {
Theme Built[2][size_t(AccentPreset::Count)];
bool BuiltReady = false;

void Build() {
  if (BuiltReady)
    return;
  for (int M = 0; M < 2; ++M) {
    const Theme& Base = M == 0 ? DarkThemeBase() : LightThemeBase();
    for (size_t P = 0; P < size_t(AccentPreset::Count); ++P) {
      Theme T = Base;
      T.Preset = AccentPreset(P);
      T.Accent = Base.AccentPresets[P];
      Built[M][P] = T;
    }
  }
  BuiltReady = true;
}

double Channel(uint8_t V) {
  const double C = V / 255.0;
  return C <= 0.03928 ? C / 12.92 : std::pow((C + 0.055) / 1.055, 2.4);
}
double Luminance(Color C) { return 0.2126 * Channel(C.R) + 0.7152 * Channel(C.G) + 0.0722 * Channel(C.B); }
} // namespace

const char* AccentName(AccentPreset P) {
  switch (P) {
  case AccentPreset::Cyan: return "cyan";
  case AccentPreset::Red: return "red";
  case AccentPreset::Violet: return "violet";
  case AccentPreset::Blue: return "blue";
  default: return "?";
  }
}

bool ParseAccent(const char* Name, AccentPreset& Out) {
  for (size_t P = 0; P < size_t(AccentPreset::Count); ++P)
    if (Name && std::strcmp(Name, AccentName(AccentPreset(P))) == 0) {
      Out = AccentPreset(P);
      return true;
    }
  return false;
}

const Theme& GetTheme(ThemeMode Mode, AccentPreset Accent) {
  Build();
  if (size_t(Accent) >= size_t(AccentPreset::Count))
    Accent = AccentPreset::Cyan;
  return Built[Mode == ThemeMode::Dark ? 0 : 1][size_t(Accent)];
}

const Theme& CurrentTheme() { return g_theme ? *g_theme : GetTheme(ThemeMode::Dark, AccentPreset::Cyan); }
void SetTheme(ThemeMode Mode, AccentPreset Accent) { g_theme = &GetTheme(Mode, Accent); }

double ContrastRatio(Color A, Color B) {
  double L1 = Luminance(A), L2 = Luminance(B);
  if (L1 < L2)
    std::swap(L1, L2);
  return (L1 + 0.05) / (L2 + 0.05);
}

Color Blend(Color Top, Color Bottom) {
  const double A = Top.A / 255.0;
  auto Mix = [&](uint8_t T, uint8_t B) { return uint8_t(std::lround(T * A + B * (1.0 - A))); };
  return Color {Mix(Top.R, Bottom.R), Mix(Top.G, Bottom.G), Mix(Top.B, Bottom.B), 255};
}

} // namespace ui
