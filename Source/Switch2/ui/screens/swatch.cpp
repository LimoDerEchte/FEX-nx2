// SPDX-License-Identifier: MIT
#include "screens/swatch.h"

#include <cstdio>
#include <string>

namespace ui {
namespace {

struct TokenRef {
  const char* Name;
  Color Theme::* Field;
};

const TokenRef Tokens[] = {
#define UI_TOKEN_REF(Field, Name) {#Name, &Theme::Field},
    UI_THEME_COLORS(UI_TOKEN_REF)
#undef UI_TOKEN_REF
};

std::string HexOf(Color C) {
  char Buf[16];
  if (C.A == 255)
    std::snprintf(Buf, sizeof(Buf), "#%02X%02X%02X", C.R, C.G, C.B);
  else
    std::snprintf(Buf, sizeof(Buf), "#%02X%02X%02X %d%%", C.R, C.G, C.B, int(std::lround(C.A * 100.0 / 255.0)));
  return Buf;
}

void Chip(const DrawContext& C, float X, float Y, float Size, Color Fill) {
  const int Hair = HairlinePx(C.S);
  const Rect R = C.Du(X, Y, Size, Size);
  C.R.FillRoundRect(R, 4 * C.S, Fill);
  C.R.StrokeRoundRect(R, 4 * C.S, float(Hair), C.T.TextDim);
}

} // namespace

void DrawSwatch(const DrawContext& C) {
  const Theme& T = C.T;
  const float S = C.S;
  const int Hair = HairlinePx(S);
  Fonts& F = C.F;

  // Header.
  const std::string Title = std::string("Swatch · ") + (T.Mode == ThemeMode::Dark ? "dark" : "light") + " · " + AccentName(T.Preset);
  F.Draw(styles::TitleSettings, Title, S, std::round(32 * S), F.BaselineInBox(styles::TitleSettings, S, 16 * S, 44 * S), T.Text);
  F.Draw(styles::Caption, "FEX Switch 2 launcher UI · theme tokens, text styles, icons", S, std::round(1248 * S), F.BaselineInBox(styles::Caption, S, 16 * S, 44 * S),
         T.TextMuted, Align::Right);
  C.R.Line(32 * S, std::round(70 * S), 1248 * S, std::round(70 * S), float(Hair), T.Divider);

  // Text styles (left column).
  float Y = 84;
  for (const TextStyle& St : styles::All) {
    const char* Sample = St.Font >= FontId::MonoRegular && St.Font <= FontId::MonoBold ? "ftp://192.168.1.50:5000" : St.Upper ? "Appearance" : "FEX Switch 2";
    const float Adv = std::round(St.Size * 1.28f) + 3;
    const float Base = std::round((Y + St.Size * 1.0f) * S);
    F.Draw(St, Sample, S, std::round(32 * S), Base, T.Text);
    char Label[96];
    std::snprintf(Label, sizeof(Label), "%s · %s · %g du", St.Name, FontName(St.Font), double(St.Size));
    const std::string Fitted = F.Fit(styles::Meta, Label, S, 250 * S, Ellipsis::End);
    F.Draw(styles::Meta, Fitted, S, std::round(372 * S), Base, T.TextMuted);
    Y += Adv;
  }

  // Tokens (right two columns).
  const size_t Count = sizeof(Tokens) / sizeof(Tokens[0]);
  const size_t PerColumn = (Count + 1) / 2;
  for (size_t I = 0; I < Count; ++I) {
    const float X = 640 + float(I / PerColumn) * 304, Row = 84 + float(I % PerColumn) * 22;
    const Color Value = T.*(Tokens[I].Field);
    Chip(C, X, Row, 16, Value);
    const float Base = F.BaselineInBox(styles::MonoUiSmall, S, Row * S, 16 * S);
    F.Draw(styles::MonoUiSmall, Tokens[I].Name, S, std::round((X + 24) * S), Base, T.Text);
    F.Draw(styles::MonoUiSmall, HexOf(Value), S, std::round((X + 164) * S), Base, T.TextMuted);
  }
  // Accent presets.
  {
    const float Row = 84 + float(PerColumn) * 22 + 10;
    F.Draw(styles::Meta, "accent presets", S, std::round(640 * S), F.BaselineInBox(styles::Meta, S, Row * S, 16 * S), T.TextMuted);
    for (size_t P = 0; P < size_t(AccentPreset::Count); ++P) {
      const float X = 760 + float(P) * 120;
      Chip(C, X, Row, 16, T.AccentPresets[P]);
      F.Draw(styles::MonoUiSmall, AccentName(AccentPreset(P)), S, std::round((X + 24) * S), F.BaselineInBox(styles::MonoUiSmall, S, Row * S, 16 * S), T.Text);
    }
  }

  // Icons.
  C.R.Line(32 * S, std::round(530 * S), 1248 * S, std::round(530 * S), float(Hair), T.Divider);
  for (int I = 0; I < int(IconId::Count); ++I) {
    const float Cx = 32 + 76 + float(I % 8) * 152, Top = 542 + float(I / 8) * 56;
    const Rect Box = C.Du(Cx - 14, Top, 28, 28);
    C.I.Draw(IconId(I), Box, T.Text);
    F.Draw(styles::Meta, IconName(IconId(I)), S, std::round(Cx * S), F.BaselineInBox(styles::Meta, S, (Top + 30) * S, 16 * S), T.TextMuted, Align::Center);
  }
  // The same icons in the accent at the spec stroke widths (gear 1.8, card 1.5, chevron 2.3, close 2.6).
  const float StrokeX = 960 + 12;
  C.I.Draw(IconId::Settings, C.Du(StrokeX, 458, 28, 28), T.Accent, stroke::Gear);
  C.I.Draw(IconId::Gamepad, C.Du(StrokeX + 44, 458, 28, 28), T.Accent, stroke::Card);
  C.I.Draw(IconId::ChevronRight, C.Du(StrokeX + 88, 458, 28, 28), T.Accent, stroke::Chevron);
  C.I.Draw(IconId::X, C.Du(StrokeX + 132, 458, 28, 28), T.Accent, stroke::Close);
}

} // namespace ui
