// SPDX-License-Identifier: MIT
#include "components/Controls.h"

#include <algorithm>
#include <cstdio>

namespace ui {

// ---- Toggle -------------------------------------------------------------------------------------------------------------

void Toggle::Set(bool V, uint64_t Now, bool Animate) {
  if (!Init) {
    Init = true;
    Value = V;
    Knob.Set(V ? 1.0f : 0.0f);
    return;
  }
  if (V == Value && Knob.To == (V ? 1.0f : 0.0f))
    return;
  Value = V;
  if (Animate)
    Knob.Go(V ? 1.0f : 0.0f, Now, 120);
  else
    Knob.Set(V ? 1.0f : 0.0f);
}

void Toggle::Activate(uint64_t Now) {
  Set(!Value, Now);
  if (OnChange)
    OnChange(Value);
}

void Toggle::Draw(const Frame& F) {
  if (!Init)
    Set(Value, F.Now);
  const Theme& T = F.T();
  const float S = F.S();
  const float O = Op();
  const float K = Knob.Value(F.Now);
  const float TrackX = Bounds.X + Bounds.W - 56, TrackY = Bounds.Y + (Bounds.H - 32) / 2;
  F.Text(styles::Body, Value ? "On" : "Off", TrackX - 12, Bounds.Y, Bounds.H, Opacity(T.TextMuted, O), Align::Right);
  Color Track = Mix(T.ToggleOffTrack, T.Accent, K);
  if (Pressed)
    Track = Mix(Track, T.Text, 0.12f);
  const Rect TR = F.Box(TrackX, TrackY, 56, 32);
  F.Dc.R.FillRoundRect(TR, TR.H / 2, Opacity(Track, O));
  const float KnobX = TrackX + 3 + K * (56 - 6 - 26);
  const Rect KR = F.Box(KnobX, TrackY + 3, 26, 26);
  (void)S;
  F.Dc.R.FillRoundRect(KR, KR.H / 2, Opacity(Mix(T.ToggleOffKnob, T.ToggleOnKnob, K), O));
}

// ---- EnumChip -----------------------------------------------------------------------------------------------------------

bool EnumChip::OnNav(Dir D, uint64_t) {
  if ((D != Dir::Left && D != Dir::Right) || Values.empty())
    return false;
  const int N = int(Values.size());
  if (!Wrap && ((D == Dir::Left && Index <= 0) || (D == Dir::Right && Index >= N - 1)))
    return false;
  Index = (Index + (D == Dir::Right ? 1 : N - 1)) % N;
  if (OnChange)
    OnChange(Index);
  return true;
}

float EnumChip::PreferredWidth(const Frame& F) {
  float W = 0;
  for (const std::string& V : Values)
    W = std::max(W, F.Measure(cstyles::Pill, V));  // wide enough for the longest value: no jumping when cycling
  return 18 + W + 8 + 18 + 12;
}

void EnumChip::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float O = Op();
  const Rect B = F.Box(Bounds);
  F.Dc.R.FillRoundRect(B, B.H / 2, Opacity(Pressed ? T.SurfaceInset : T.SurfaceRaised, O));
  const std::string& V = Index >= 0 && Index < int(Values.size()) ? Values[size_t(Index)] : std::string();
  F.Text(cstyles::Pill, V, Bounds.X + 18, Bounds.Y, Bounds.H, Opacity(T.Accent, O));
  F.Dc.I.Draw(IconId::ChevronDown, F.Box(Bounds.X + Bounds.W - 12 - 18, Bounds.Y + (Bounds.H - 18) / 2, 18, 18), Opacity(T.TextMuted, O), stroke::Chevron);
}

// ---- PathField ----------------------------------------------------------------------------------------------------------

float PathField::ChipWidth(const Frame& F) { return std::min(340.0f, F.Measure(styles::MonoUiSmall, Path) + 24); }

float PathField::PreferredWidth(const Frame& F) {
  Browse.Layout(F);
  return ChipWidth(F) + 10 + Browse.Bounds.W;
}

void PathField::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  const float O = Op();
  const float ChipW = ChipWidth(F);
  const float ChipH = Browse.Bounds.H;  // as tall as the Browse pill beside it
  const float ChipY = Bounds.Y + (Bounds.H - ChipH) / 2;
  F.Dc.R.FillRoundRect(F.Box(Bounds.X, ChipY, ChipW, ChipH), 8 * S, Opacity(T.Field, O));
  const std::string Fit = F.Dc.F.Fit(styles::MonoUiSmall, Path, S, (ChipW - 24) * S, Ellipsis::Middle);
  F.Text(styles::MonoUiSmall, Fit, Bounds.X + 12, ChipY, ChipH, Opacity(T.TextField, O));
  Browse.Layout(F);
  Browse.Bounds.X = Bounds.X + ChipW + 10;
  Browse.Bounds.Y = Bounds.Y + (Bounds.H - Browse.Bounds.H) / 2;
  Browse.Enabled = Enabled;
  Browse.Pressed = Pressed;
  Browse.Draw(F);
}

// ---- Slider -------------------------------------------------------------------------------------------------------------

float Slider::Snap(float V) const {
  V = std::clamp(V, Min, Max);
  if (Step > 0)
    V = Min + std::round((V - Min) / Step) * Step;
  return std::clamp(V, Min, Max);
}

void Slider::SetValue(float V) { Value = Snap(V); }

bool Slider::OnNav(Dir D, uint64_t) {
  if (D != Dir::Left && D != Dir::Right)
    return false;
  const float Old = Value;
  Value = Snap(Value + (D == Dir::Right ? Step : -Step));
  if (Value != Old && OnChange)
    OnChange(Value);
  return !(LeaveAtEdge && Value == Old);
}

void Slider::OnDragTo(float X, float, uint64_t) {
  const float Old = Value;
  Value = Snap(Min + (X - Bounds.X) / 220.0f * (Max - Min));
  if (Value != Old && OnChange)
    OnChange(Value);
}

void Slider::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  const float O = Op();
  const float Frac = Max > Min ? (Value - Min) / (Max - Min) : 0;
  const float Cy = Bounds.Y + Bounds.H / 2;
  const Rect Track = F.Box(Bounds.X, Cy - 3, 220, 6);
  F.Dc.R.FillRoundRect(Track, Track.H / 2, Opacity(T.Divider, O));
  const Rect Fill = F.Box(Bounds.X, Cy - 3, std::max(6.0f, 220 * Frac), 6);
  F.Dc.R.FillRoundRect(Fill, Fill.H / 2, Opacity(T.Accent, O));
  const float KnobD = Pressed ? 24.0f : 20.0f;
  const Rect Knob = F.Box(Bounds.X + 220 * Frac - KnobD / 2, Cy - KnobD / 2, KnobD, KnobD);
  F.Dc.R.FillRoundRect(Knob, Knob.H / 2, Opacity(T.ToggleOnKnob, O));
  F.Dc.R.StrokeRoundRect(Knob, Knob.H / 2, std::max(1.0f, std::round(2 * S)), Opacity(T.Accent, O));
  char Buf[48];
  std::snprintf(Buf, sizeof(Buf), "%g", double(Value));
  F.Text(styles::MonoUi, Format ? Format(Value) : std::string(Buf), Bounds.X + 220 + 14 + 64, Bounds.Y, Bounds.H, Opacity(T.Text, O), Align::Right);
}

// ---- SettingsRow --------------------------------------------------------------------------------------------------------

void SettingsRow::SetControl(std::unique_ptr<Control> C) {
  Ctl = std::move(C);
  if (Ctl) {
    Ctl->Focusable = false;
    Ctl->Hittable = false;
  }
}

void SettingsRow::Activate(uint64_t Now) {
  if (Ctl && Ctl->Enabled)
    Ctl->Activate(Now);
  if (OnActivate)
    OnActivate();
}

void SettingsRow::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  const float O = Op();
  if (Pressed)
    F.Dc.R.FillRoundRect(F.Box(Bounds), 12 * S, Opacity(T.SurfaceInset, O));
  const float LabelH = F.Dc.F.LineHeightPx(styles::RowLabel, S) / S, DescH = F.Dc.F.LineHeightPx(styles::Caption, S) / S;
  const bool HasDesc = !Description.empty();
  const float Block = LabelH + (HasDesc ? 2 + DescH : 0);
  const float Top = Bounds.Y + (Bounds.H - BottomPad - Block) / 2;
  float CtlW = 0;
  if (Ctl) {
    CtlW = Ctl->PreferredWidth(F);
    Ctl->Bounds.W = CtlW;
    Ctl->Bounds.X = Bounds.X + Bounds.W - CtlW;
    Ctl->Bounds.Y = Bounds.Y + (Bounds.H - BottomPad - Ctl->Bounds.H) / 2;
    Ctl->Enabled = Enabled;
    Ctl->Pressed = Pressed;
    Ctl->Focused = Focused;
  }
  const float TextMax = std::max(0.0f, Bounds.W - CtlW - 24) * S;
  F.Text(styles::RowLabel, F.Dc.F.Fit(styles::RowLabel, Label, S, TextMax, Ellipsis::End), Bounds.X, Top, LabelH, Opacity(T.Text, O));
  if (HasDesc)
    F.Text(styles::Caption, F.Dc.F.Fit(styles::Caption, Description, S, TextMax, Ellipsis::End), Bounds.X, Top + LabelH + 2, DescH, Opacity(T.TextMuted, O));
  if (Ctl)
    Ctl->Draw(F);
}

} // namespace ui
