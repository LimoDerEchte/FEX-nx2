// SPDX-License-Identifier: MIT
#include "components/Rows.h"

#include <algorithm>
#include <cmath>

namespace ui {

// ---- SidebarItem --------------------------------------------------------------------------------------------------------

void SidebarItem::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  const float O = Op();
  if (Selected || Pressed)
    F.Dc.R.FillRoundRect(F.Box(Bounds), 12 * S, Opacity(Pressed && !Selected ? T.SurfaceInset : T.Surface, O));
  const Color Ink = Selected ? T.Accent : T.Text;
  float X = Bounds.X + 16;
  if (HasIcon) {
    F.Dc.I.Draw(Icon, F.Box(X, Bounds.Y + (Bounds.H - 20) / 2, 20, 20), Opacity(Ink, O), IconStroke);
    X += 20 + 12;
  }
  const std::string Fit = F.Dc.F.Fit(styles::RowLabel, Text, S, (Bounds.X + Bounds.W - 12 - X) * S, Ellipsis::End);
  F.Text(styles::RowLabel, Fit, X, Bounds.Y, Bounds.H, Opacity(Ink, O));
}

// ---- ActionPill ---------------------------------------------------------------------------------------------------------

float ActionPill::PreferredWidth(const Frame& F) { return 2 * 18 + (HasIcon ? 18 + 8 : 0) + F.Measure(cstyles::Pill, Text); }

void ActionPill::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float O = Op();
  const Rect B = F.Box(Bounds);
  F.Dc.R.FillRoundRect(B, B.H / 2, Opacity(Pressed ? T.SurfaceInset : T.SurfaceRaised, O));
  const Color Ink = Destructive ? T.Error : T.Text;
  const float TextW = F.Measure(cstyles::Pill, Text);
  float X = Bounds.X + (Bounds.W - TextW - (HasIcon ? 18 + 8 : 0)) / 2;
  if (HasIcon) {
    F.Dc.I.Draw(Icon, F.Box(X, Bounds.Y + (Bounds.H - 18) / 2, 18, 18), Opacity(Ink, O), stroke::Default);
    X += 18 + 8;
  }
  F.Text(cstyles::Pill, Text, X, Bounds.Y, Bounds.H, Opacity(Ink, O));
}

// ---- LicenseRow ---------------------------------------------------------------------------------------------------------

void LicenseRow::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  const float O = Op();
  if (Pressed)
    F.Dc.R.FillRoundRect(F.Box(Bounds), 12 * S, Opacity(T.SurfaceInset, O));
  // Right: chevron 20, 12 gap, chip 32 high (mono_ui 13/600, padding 0 14).
  const float ChipW = F.Measure(cstyles::Chip13, License) + 28;
  const float ChevX = Bounds.X + Bounds.W - 20 - 4;
  const float ChipX = ChevX - 12 - ChipW;
  F.Dc.I.Draw(IconId::ChevronRight, F.Box(ChevX, Bounds.Y + (Bounds.H - 20) / 2, 20, 20), Opacity(T.TextMuted, O), stroke::Chevron);
  const Rect Chip = F.Box(ChipX, Bounds.Y + (Bounds.H - 32) / 2, ChipW, 32);
  F.Dc.R.FillRoundRect(Chip, Chip.H / 2, Opacity(T.SurfaceRaised, O));
  F.Text(cstyles::Chip13, License, ChipX + ChipW / 2, Bounds.Y + (Bounds.H - 32) / 2, 32, Opacity(T.TextField, O), Align::Center);
  // Left: name 17/700 over role 14 muted.
  const float H1 = F.Dc.F.LineHeightPx(cstyles::Name17, S) / S, H2 = F.Dc.F.LineHeightPx(cstyles::Meta14, S) / S;
  const float Top = Bounds.Y + (Bounds.H - (H1 + 2 + H2)) / 2;
  const float Max = std::max(0.0f, ChipX - 16 - Bounds.X) * S;
  F.Text(cstyles::Name17, F.Dc.F.Fit(cstyles::Name17, Name, S, Max, Ellipsis::End), Bounds.X, Top, H1, Opacity(T.Text, O));
  F.Text(cstyles::Meta14, F.Dc.F.Fit(cstyles::Meta14, Role, S, Max, Ellipsis::End), Bounds.X, Top + H1 + 2, H2, Opacity(T.TextMuted, O));
}

// ---- FtpPill ------------------------------------------------------------------------------------------------------------

std::string FtpPill::StatusText() const {
  if (!Running)
    return "Off";
  return "Running \xC2\xB7 " + std::to_string(Clients) + (Clients == 1 ? " client" : " clients");
}

float FtpPill::PreferredWidth(const Frame& F) const {
  static constexpr TextStyle Status {"ftp_status", FontId::NunitoSemiBold, 16};
  return 18 + 22 + 10 + F.Measure(cstyles::Pill, "FTP server") + 14 + 1 + 14 + 8 + 8 + F.Measure(Status, StatusText()) + 22;
}

void FtpPill::Draw(const Frame& F) {
  static constexpr TextStyle Status {"ftp_status", FontId::NunitoSemiBold, 16};
  const Theme& T = F.T();
  const float S = F.S();
  const float O = Op();
  const Rect B = F.Box(Bounds);
  F.Dc.R.FillRoundRect(B, B.H / 2, Opacity(Pressed ? T.SurfaceInset : T.SurfaceRaised, O));
  const float Cy = Bounds.Y + Bounds.H / 2;
  float X = Bounds.X + 18;
  F.Dc.I.Draw(IconId::Transfer, F.Box(X, Cy - 11, 22, 22), Opacity(T.Text, O), stroke::Default);
  X += 22 + 10;
  const float W1 = F.Measure(cstyles::Pill, "FTP server");
  F.Text(cstyles::Pill, "FTP server", X, Bounds.Y, Bounds.H, Opacity(T.Text, O));
  X += W1 + 14;
  const int Hair = HairlinePx(S);
  const Rect Rule = F.Box(X, Cy - 10, 1, 20);
  F.Dc.R.FillRect(Rect {Rule.X, Rule.Y, float(Hair), Rule.H}, Opacity(T.Divider, O));
  X += 1 + 14;
  F.Dc.R.FillCircle(std::round((X + 4) * S), std::round((Cy - F.ScrollY) * S), std::round(4 * S), Opacity(Running ? T.Ok : T.RadioOff, O));
  X += 8 + 8;
  F.Text(Status, StatusText(), X, Bounds.Y, Bounds.H, Opacity(T.TextMuted, O));
}

// ---- FileRow ------------------------------------------------------------------------------------------------------------

void FileRow::Draw(const Frame& F) {
  if (!Bound)
    return;
  const Theme& T = F.T();
  const float S = F.S();
  const float O = Op();
  if (Focused || Pressed)
    F.Dc.R.FillRoundRect(F.Box(Bounds), 12 * S, Opacity(Pressed && !Focused ? T.SurfaceInset : T.Surface, O));
  const float Cy = Bounds.Y + Bounds.H / 2;
  const float IconX = Bounds.X + 16;
  if (IsDir) {
    // Folder: filled with surface_inset, muted outline.
    F.Dc.R.FillRoundRect(F.Box(IconX + 2, Cy - 6, 24, 15), 3 * S, Opacity(T.SurfaceInset, O));
    F.Dc.I.Draw(IconId::Folder, F.Box(IconX, Cy - 14, 28, 28), Opacity(T.TextMuted, O), stroke::Default);
  } else {
    F.Dc.I.Draw(IconId::File, F.Box(IconX, Cy - 14, 28, 28), Opacity(T.TextMuted, O), stroke::Default);
  }
  const float NameX = IconX + 28 + 12;
  const float SizeRight = Bounds.X + Bounds.W - 16 - 120 - 12;  // right edge of the size column
  const float DateX = Bounds.X + Bounds.W - 16 - 120;
  const float NameMax = std::max(0.0f, SizeRight - 110 - 12 - NameX);
  F.Text(cstyles::Name17, F.Dc.F.Fit(cstyles::Name17, Name, S, NameMax * S, Ellipsis::End), NameX, Bounds.Y, Bounds.H, Opacity(T.Text, O));
  F.Text(styles::MonoUi, SizeText, SizeRight, Bounds.Y, Bounds.H, Opacity(T.TextMuted, O), Align::Right);
  F.Text(cstyles::Meta14, DateText, DateX, Bounds.Y, Bounds.H, Opacity(T.TextMuted, O));
}

// ---- Crumb / UpButton ---------------------------------------------------------------------------------------------------

void Crumb::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  if (Pressed)
    F.Dc.R.FillRoundRect(F.Box(Bounds), 8 * S, Opacity(T.SurfaceInset, Op()));
  F.Text(cstyles::Crumb, Text, Bounds.X + Bounds.W / 2, Bounds.Y, Bounds.H, Opacity(Current ? T.Text : T.TextMuted, Op()), Align::Center);
}

void UpButton::Draw(const Frame& F) {
  const Theme& T = F.T();
  const Rect B = F.Box(Bounds);
  F.Dc.R.FillRoundRect(B, B.H / 2, Opacity(Pressed ? T.SurfaceInset : T.SurfaceRaised, Op()));
  F.Dc.I.Draw(IconId::ArrowUp, F.Box(Bounds.X + 11, Bounds.Y + 11, 18, 18), Opacity(T.Text, Op()), stroke::Default);
}

// ---- Spinner ------------------------------------------------------------------------------------------------------------

void DrawSpinner(const Frame& F, float Cx, float Cy, float Radius, uint64_t Now) {
  const float S = F.S();
  const float Head = float(Now % 900) / 900.0f * 8.0f;
  for (int I = 0; I < 8; ++I) {
    float Behind = Head - float(I);
    if (Behind < 0)
      Behind += 8.0f;
    const float K = 1.0f - Behind / 8.0f;  // 1 at the head, fading behind it
    const float A = 6.2831853f * float(I) / 8.0f;
    F.Dc.R.FillCircle(std::round((Cx + std::sin(A) * Radius) * S), std::round((Cy - std::cos(A) * Radius - F.ScrollY) * S), std::round(3 * S), Opacity(F.T().Accent, 0.25f + 0.75f * K));
  }
}

} // namespace ui
