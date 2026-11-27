// SPDX-License-Identifier: MIT
#include "components/Tiles.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {
constexpr TextStyle Mono18 {"tile_value", FontId::MonoSemiBold, 18};
constexpr TextStyle Label14 {"tile_label", FontId::NunitoSemiBold, 14};
constexpr TextStyle Bold18 {"tile_switch", FontId::NunitoBold, 18};
constexpr TextStyle Name18 {"card_name_18", FontId::NunitoBold, 18};
constexpr TextStyle Name17 {"card_name_17", FontId::NunitoBold, 17};
constexpr TextStyle Meta13 {"card_meta_13", FontId::NunitoRegular, 13};
constexpr TextStyle Meta14 {"card_meta_14", FontId::NunitoRegular, 14};
constexpr TextStyle Tag12 {"card_tag", FontId::NunitoBold, 12};
constexpr TextStyle Act16 {"client_activity", FontId::NunitoSemiBold, 16};
constexpr TextStyle Speed16On {"client_speed_on", FontId::MonoSemiBold, 16};
constexpr TextStyle Speed16Off {"client_speed_off", FontId::MonoRegular, 16};
constexpr TextStyle Addr14 {"client_addr", FontId::MonoRegular, 14};
} // namespace

// ---- EyeButton / CredentialTile -----------------------------------------------------------------------------------------------

void EyeButton::Draw(const Frame& F) {
  const Theme& T = F.T();
  const Color Ink = Pressed ? T.Text : T.TextMuted;
  F.Dc.I.Draw(Shown ? IconId::EyeOff : IconId::Eye, F.Box(Bounds.X + 11, Bounds.Y + 11, 22, 22), Opacity(Ink, Op()), stroke::Default);
}

void CredentialTile::SetOn(bool V, uint64_t Now) {
  if (!KnobInit) {
    KnobInit = true;
    On = V;
    Knob.Set(V ? 1.0f : 0.0f);
    return;
  }
  if (V == On && Knob.To == (V ? 1.0f : 0.0f))
    return;
  On = V;
  Knob.Go(V ? 1.0f : 0.0f, Now, 120);
}

void CredentialTile::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  const float O = Op();
  if (!KnobInit)
    SetOn(On, F.Now);
  const Rect B = F.Box(Bounds);
  F.Dc.R.FillRoundRect(B, 16 * S, Opacity(Pressed ? T.SurfaceInset : T.Surface, O));
  const float LabelH = F.Dc.F.LineHeightPx(Label14, S) / S;
  const float ValueH = F.Dc.F.LineHeightPx(Mono18, S) / S;
  F.Text(Label14, Label, Bounds.X + 18, Bounds.Y + 14, LabelH, Opacity(T.TextMuted, O));
  if (K != Kind::Switch)
    F.Dc.I.Draw(IconId::Pencil, F.Box(Bounds.X + Bounds.W - 18 - 14, Bounds.Y + 14, 14, 14), Opacity(T.TextMuted, O), stroke::Default);
  const float ValueY = Bounds.Y + Bounds.H - 14 - ValueH;
  if (K == Kind::Switch) {
    const float KnobPos = Knob.Value(F.Now);
    F.Text(Bold18, On ? "On" : "Off", Bounds.X + 18, ValueY, ValueH, Opacity(T.Text, O));
    const float Tx = Bounds.X + Bounds.W - 18 - 48, Ty = Bounds.Y + Bounds.H - 14 - 28 + (ValueH - 28) / 2 - 0;  // 48x28 track
    const Rect Tr = F.Box(Tx, Ty, 48, 28);
    F.Dc.R.FillRoundRect(Tr, Tr.H / 2, Opacity(Mix(T.ToggleOffTrack, T.Accent, KnobPos), O));
    const Rect Kn = F.Box(Tx + 3 + KnobPos * (48 - 6 - 22), Ty + 3, 22, 22);
    F.Dc.R.FillRoundRect(Kn, Kn.H / 2, Opacity(Mix(T.ToggleOffKnob, T.ToggleOnKnob, KnobPos), O));
    return;
  }
  float Right = Bounds.X + Bounds.W - 18;
  if (K == Kind::Password) {
    Layout();
    Eye.Shown = ShowSecret;
    Eye.Draw(F);
    Right -= 22 + 8;
  }
  const std::string Fit = F.Dc.F.Fit(Mono18, Value, S, (Right - (Bounds.X + 18)) * S, Ellipsis::End);
  F.Text(Mono18, Fit, Bounds.X + 18, ValueY, ValueH, Opacity(T.Text, O));
}

// ---- ClientRow ------------------------------------------------------------------------------------------------------------

void ClientRow::Columns(float X, float W, float Out[4][2]) {
  const float Gap = 24, Fixed = 140;
  const float Fr = (W - 3 * Gap - 2 * Fixed) / 5.0f;
  Out[0][0] = X;
  Out[0][1] = Fr * 2;
  Out[1][0] = X + Fr * 2 + Gap;
  Out[1][1] = Fr * 3;
  Out[2][0] = Out[1][0] + Out[1][1] + Gap;
  Out[2][1] = Fixed;
  Out[3][0] = Out[2][0] + Fixed + Gap;
  Out[3][1] = Fixed;
}

void ClientRow::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  const float O = Op();
  const int Hair = HairlinePx(S);
  if (Pressed)
    F.Dc.R.FillRoundRect(F.Box(Bounds), 12 * S, Opacity(T.SurfaceInset, O));
  float Col[4][2];
  Columns(Bounds.X, Bounds.W, Col);
  const float H1 = F.Dc.F.LineHeightPx(Name18, S) / S, H2 = F.Dc.F.LineHeightPx(Addr14, S) / S;
  const float Top = Bounds.Y + (Bounds.H - (H1 + 2 + H2)) / 2;
  F.Text(Name18, F.Dc.F.Fit(Name18, Name, S, Col[0][1] * S, Ellipsis::End), Col[0][0], Top, H1, Opacity(T.Text, O));
  F.Text(Addr14, Address, Col[0][0], Top + H1 + 2, H2, Opacity(T.TextMuted, O));
  F.Text(Act16, F.Dc.F.Fit(Act16, Activity, S, Col[1][1] * S, Ellipsis::Middle), Col[1][0], Bounds.Y, Bounds.H, Opacity(T.Text, O));
  F.Text(DownActive ? Speed16On : Speed16Off, Down, Col[2][0] + Col[2][1], Bounds.Y, Bounds.H, Opacity(DownActive ? T.Text : T.TextMuted, O), Align::Right);
  F.Text(UpActive ? Speed16On : Speed16Off, Up, Col[3][0] + Col[3][1], Bounds.Y, Bounds.H, Opacity(UpActive ? T.Text : T.TextMuted, O), Align::Right);
  const float Y = std::round((Bounds.Y + Bounds.H - F.ScrollY) * S) - float(Hair);
  F.Dc.R.Line(std::round(Bounds.X * S), Y, std::round((Bounds.X + Bounds.W) * S), Y, float(Hair), T.Divider);
}

// ---- OptionCard -----------------------------------------------------------------------------------------------------------

void OptionCard::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  const float O = Op();
  const Rect Box = F.Box(Bounds);
  F.Dc.R.FillRoundRect(Box, 16 * S, Opacity(Pressed ? T.SurfaceInset : T.Surface, O));
  // Badge: 24 du. Radio = 2 du ring (accent when selected, radio_off otherwise) + 12 dot; check on = accent disc + check; off = ring.
  const float BadgeSize = 24;
  const float Bx = Bounds.X + Bounds.W - 20 - BadgeSize / 2, By = Tall ? Bounds.Y + 16 + BadgeSize / 2 : Bounds.Y + Bounds.H / 2;
  const float Cx = std::round(Bx * S), Cy = std::round((By - F.ScrollY) * S), R = std::round(BadgeSize / 2 * S);
  const float Ring = std::max(1.0f, std::round(2 * S));
  if (B == Badge::Radio) {
    F.Dc.R.Ring(Cx, Cy, R, Ring, Opacity(Selected ? T.Accent : T.RadioOff, O));
    if (Selected)
      F.Dc.R.FillCircle(Cx, Cy, std::round(6 * S), Opacity(T.Accent, O));
  } else if (Selected) {
    F.Dc.R.FillCircle(Cx, Cy, R, Opacity(T.Accent, O));
    // A check mark from two short strokes drawn as small squares along the path (no diagonal lines in the 2D layer).
    const float U = std::max(1.0f, std::round(2 * S));
    const float Pts[][2] = {{-5, 0}, {-4, 1}, {-3, 2}, {-2, 3}, {-1, 4}, {0, 3}, {1, 2}, {2, 1}, {3, 0}, {4, -1}, {5, -2}, {6, -3}};
    for (const auto& P : Pts)
      F.Dc.R.FillRect(SnapRect(Rect {Cx + P[0] * S - U / 2, Cy + (P[1] - 1) * S - U / 2, U, U}), Opacity(T.OnAccent, O));
  } else {
    F.Dc.R.Ring(Cx, Cy, R, Ring, Opacity(T.RadioOff, O));
  }
  const float RightLimit = Bounds.X + Bounds.W - 20 - BadgeSize - 12;
  if (!Tall) {
    F.Dc.R.FillRoundRect(F.Box(Bounds.X + 18, Bounds.Y + (Bounds.H - 52) / 2, 52, 52), 12 * S, Opacity(T.SurfaceInset, O));
    F.Dc.I.Draw(Icon, F.Box(Bounds.X + 18 + 14, Bounds.Y + (Bounds.H - 24) / 2, 24, 24), Opacity(T.Text, O), stroke::Card);
    const float X = Bounds.X + 18 + 52 + 16;
    const float H1 = F.Dc.F.LineHeightPx(Name18, S) / S, H2 = F.Dc.F.LineHeightPx(Meta14, S) / S;
    const float Top = Bounds.Y + (Bounds.H - (H1 + 4 + H2)) / 2;
    float NameW = F.Measure(Name18, Name);
    const std::string Fit = F.Dc.F.Fit(Name18, Name, S, (RightLimit - X - (Tag.empty() ? 0 : 120)) * S, Ellipsis::End);
    NameW = F.Measure(Name18, Fit);
    F.Text(Name18, Fit, X, Top, H1, Opacity(T.Text, O));
    if (!Tag.empty()) {
      const float TW = F.Measure(Tag12, Tag) + 20;
      const Rect Tg = F.Box(X + NameW + 10, Top + (H1 - 22) / 2, TW, 22);
      F.Dc.R.FillRoundRect(Tg, Tg.H / 2, Opacity(Mix(T.Surface, T.Accent, 0.22f), O));
      F.Text(Tag12, Tag, X + NameW + 10 + TW / 2, Top + (H1 - 22) / 2, 22, Opacity(T.Accent, O), Align::Center);
    }
    F.Text(Meta14, F.Dc.F.Fit(Meta14, Meta, S, (RightLimit - X) * S, Ellipsis::End), X, Top + H1 + 4, H2, Opacity(T.TextMuted, O));
  } else {
    F.Dc.R.FillRoundRect(F.Box(Bounds.X + 16, Bounds.Y + 16, 44, 44), 12 * S, Opacity(T.SurfaceInset, O));
    F.Dc.I.Draw(Icon, F.Box(Bounds.X + 16 + 10, Bounds.Y + 16 + 10, 24, 24), Opacity(T.Text, O), stroke::Card);
    const float H1 = F.Dc.F.LineHeightPx(Name17, S) / S, H2 = F.Dc.F.LineHeightPx(Meta13, S) / S;
    const float W = Bounds.W - 32;
    const float Y2 = Bounds.Y + Bounds.H - 14 - H2;
    F.Text(Name17, F.Dc.F.Fit(Name17, Name, S, W * S, Ellipsis::End), Bounds.X + 16, Y2 - 2 - H1, H1, Opacity(T.Text, O));
    F.Text(Meta13, F.Dc.F.Fit(Meta13, Meta, S, W * S, Ellipsis::End), Bounds.X + 16, Y2, H2, Opacity(T.TextMuted, O));
  }
}

void DrawSectionHeader(const Frame& F, float X, float Y, float W, const std::string& Label, const std::string& Note) {
  const Theme& T = F.T();
  F.Text(styles::Section, Label, X, Y, 20, T.TextMuted);
  if (!Note.empty())
    F.Text(Meta14, Note, X + W, Y, 20, T.TextMuted, Align::Right);
}

} // namespace ui
