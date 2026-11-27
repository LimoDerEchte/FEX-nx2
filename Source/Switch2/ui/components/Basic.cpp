// SPDX-License-Identifier: MIT
#include "components/Basic.h"

#include <algorithm>

#include "components/Chrome.h"

namespace ui {

// ---- FocusRing ----------------------------------------------------------------------------------------------------------

void FocusRing::Update(int FocusedId, bool Show, uint64_t Now) {
  if (!Show || FocusedId == NoFocus) {
    Id = NoFocus;
    Shown = false;
    A.Set(0);
    return;
  }
  if (FocusedId != Id || !Shown) {
    Id = FocusedId;
    Shown = true;
    A.Set(0);
    A.Go(1, Now, 100);  // fades in over 100 ms; the ring itself moves instantly
  }
}

void FocusRing::DrawNow(const Frame& F, const Widget& W, float Al) {
  if (Al <= 0)
    return;
  const float S = F.S();
  const float O = std::round(W.RingOutset() * S), Oy = std::round(W.RingOutsetY() * S);
  const Rect B = F.Box(W.Bounds);
  const Rect R {B.X - O, B.Y - Oy, B.W + 2 * O, B.H + 2 * Oy};
  const float Th = std::max(1.0f, std::round(W.RingStroke() * S));
  F.Dc.R.StrokeRoundRect(R, std::max(4.0f, W.RingRadius() + std::min(W.RingOutset(), W.RingOutsetY())) * S, Th, Opacity(F.T().Accent, Al));
}

// ---- VersionBlock -------------------------------------------------------------------------------------------------------

void VersionBlock::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  F.Dc.R.FillRoundRect(F.Box(Bounds.X, Bounds.Y, 44, 44), 12 * S, T.SurfaceRaised);
  F.Text(cstyles::MonoUi14, "FX", Bounds.X + 22, Bounds.Y, 44, T.Accent, Align::Center);
  // Two lines, gap 2, centred in the 44 du tile height.
  const float H1 = F.Dc.F.LineHeightPx(styles::Meta, S) / S, H2 = F.Dc.F.LineHeightPx(styles::MonoUi, S) / S;
  const float Top = Bounds.Y + (44 - (H1 + 2 + H2)) / 2;
  const float X = Bounds.X + 44 + 14;
  F.Text(styles::Meta, "Version", X, Top, H1, T.TextMuted);
  F.Text(styles::MonoUi, Version, X, Top + H1 + 2, H2, T.Text);
}

// ---- IconButton ---------------------------------------------------------------------------------------------------------

void IconButton::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  const Rect B = F.Box(Bounds);
  F.Dc.R.FillRoundRect(B, B.H / 2, Opacity(Pressed ? T.SurfaceInset : T.SurfaceRaised, Op()));
  const Rect I = F.Box(Bounds.X + (Bounds.W - 28) / 2, Bounds.Y + (Bounds.H - 28) / 2, 28, 28);
  (void)S;
  F.Dc.I.Draw(Icon, I, Opacity(T.Text, Op()), Stroke);
}

// ---- LaunchCard ---------------------------------------------------------------------------------------------------------

void LaunchCard::Retarget(uint64_t Now) {
  if (!ScaleInit) {
    Scale.Set(1);
    ScaleInit = true;
  }
  Scale.Go((Pressed || Pending) ? 0.97f : 1.0f, Now, 80);
}

void LaunchCard::Activate(uint64_t Now) {
  if (Pending)
    return;
  Pending = true;
  FireAt = Now + 80;  // the card is pressed for 80 ms, then it launches
  Retarget(Now);
}

void LaunchCard::Tick(uint64_t Now) {
  if (Pending && Now >= FireAt) {
    Pending = false;
    if (OnActivate)
      OnActivate();
  }
  Retarget(Now);
}

void LaunchCard::Draw(const Frame& F) {
  if (!ScaleInit)
    Retarget(F.Now);
  const Theme& T = F.T();
  const float S = F.S();
  const float K = Scale.Value(F.Now);
  const float Sk = S * K;
  const Rect B = F.Box(Bounds);
  const float Cx = B.X + B.W / 2, Cy = B.Y + B.H / 2;
  auto Part = [&](float Dx, float Dy, float W, float H) { return SnapRect(Rect {Cx + (Dx - Bounds.W / 2) * Sk, Cy + (Dy - Bounds.H / 2) * Sk, W * Sk, H * Sk}); };
  const float O = Op();
  F.Dc.R.FillRoundRect(Part(0, 0, Bounds.W, Bounds.H), 20 * Sk, Opacity(T.Surface, O));
  const float LabelH = F.Dc.F.LineHeightPx(styles::CardLabel, Sk) / Sk, SubH = F.Dc.F.LineHeightPx(styles::Caption, Sk) / Sk;
  const float Total = 120 + 28 + LabelH + 4 + SubH;
  const float Top = (Bounds.H - Total) / 2;
  F.Dc.R.FillRoundRect(Part((Bounds.W - 120) / 2, Top, 120, 120), 28 * Sk, Opacity(T.SurfaceInset, O));
  F.Dc.I.Draw(Icon, Part((Bounds.W - 64) / 2, Top + 28, 64, 64), Opacity(T.Text, O), stroke::Card);
  const float LabelBox = Top + 120 + 28, SubBox = LabelBox + LabelH + 4;
  auto Line = [&](const TextStyle& St, const std::string& Str, float BoxTop, float BoxH, Color C) {
    const float Base = F.Dc.F.BaselineInBox(St, Sk, Cy + (BoxTop - Bounds.H / 2) * Sk, BoxH * Sk);
    const std::string Fit = F.Dc.F.Fit(St, Str, Sk, (Bounds.W - 12) * Sk, Ellipsis::End);
    F.Dc.F.Draw(St, Fit, Sk, std::round(Cx), Base, C, Align::Center);
  };
  Line(styles::CardLabel, Label, LabelBox, LabelH, Opacity(T.Text, O));
  Line(styles::Caption, Subtitle, SubBox, SubH, Opacity(T.TextMuted, O));
}

// ---- PillButton ---------------------------------------------------------------------------------------------------------

void PillButton::Layout(const Frame& F) { Bounds.W = FixedWidth > 0 ? FixedWidth : std::max(Bounds.H, 2 * PadX + F.Measure(cstyles::Pill, Text)); }

void PillButton::Draw(const Frame& F) {
  Layout(F);
  const Theme& T = F.T();
  Color Fill = T.SurfaceRaised, Ink = T.Text;
  switch (K) {
  case Kind::Normal: Fill = Pressed ? T.SurfaceInset : T.SurfaceRaised; break;
  case Kind::Secondary: Fill = Pressed ? Mix(T.ButtonSecondary, T.Text, 0.12f) : T.ButtonSecondary; break;
  case Kind::Accent: Fill = Pressed ? Mix(T.Accent, T.Bg, 0.25f) : T.Accent; Ink = T.OnAccent; break;
  case Kind::Error: Fill = Pressed ? Mix(T.Error, T.Bg, 0.25f) : T.Error; Ink = T.OnAccent; break;
  }
  const Rect B = F.Box(Bounds);
  F.Dc.R.FillRoundRect(B, B.H / 2, Opacity(Fill, Op()));
  F.Text(cstyles::Pill, Text, Bounds.X + Bounds.W / 2, Bounds.Y, Bounds.H, Opacity(Ink, Op()), Align::Center);
}

// ---- HintBar ------------------------------------------------------------------------------------------------------------

const char* HintLetter(Button B) {
  switch (B) {
  case Button::A: return "A";
  case Button::B: return "B";
  case Button::X: return "X";
  case Button::Y: return "Y";
  case Button::L: return "L";
  case Button::R: return "R";
  case Button::ZL: return "ZL";
  case Button::ZR: return "ZR";
  case Button::Plus: return "+";
  case Button::Minus: return "-";
  case Button::StickL: return "L3";
  case Button::StickR: return "R3";
  case Button::Up: return "\xE2\x86\x91";
  case Button::Down: return "\xE2\x86\x93";
  case Button::Left: return "\xE2\x86\x90";
  case Button::Right: return "\xE2\x86\x92";
  default: return "?";
  }
}

void HintButton::Activate(uint64_t Now) {
  (void)Now;
  if (Owner && Owner->OnHint)
    Owner->OnHint(H.Btn);
}

void HintButton::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  const float Cy = Bounds.Y + 14;
  const Color Disc = Pressed ? Mix(T.HintDisc, T.Accent, 0.5f) : T.HintDisc;
  const float O = Op();
  F.Dc.R.FillCircle(std::round((Bounds.X + 14) * S), std::round((Cy - F.ScrollY) * S), std::round(14 * S), Opacity(Disc, O));
  F.Text(cstyles::HintLetter, HintLetter(H.Btn), Bounds.X + 14, Bounds.Y, 28, Opacity(T.HintLetter, O), Align::Center);
  F.Text(styles::Body, H.Label, Bounds.X + 28 + 10, Bounds.Y, 28, Opacity(Pressed ? T.Accent : T.Text, O));
}

void HintBar::SetHints(const std::vector<Hint>& Hints) {
  Buttons.clear();
  while (Pool.size() < Hints.size())
    Pool.push_back(std::make_unique<HintButton>());
  for (size_t I = 0; I < Hints.size(); ++I) {
    HintButton& B = *Pool[I];
    B.H = Hints[I];
    B.Owner = this;
    B.Enabled = true;
    B.Pressed = false;
    Buttons.push_back(&B);
  }
}

void HintBar::SetHintEnabled(Button B, bool Enabled) {
  for (HintButton* Btn : Buttons)
    if (Btn->H.Btn == B)
      Btn->Enabled = Enabled;
}

void HintBar::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  // Layout from the right edge (content inset 32 + 16): glyph 28 + 10 gap + label, 28 between hints.
  float X = Bounds.X + Bounds.W - 48;
  for (size_t I = Buttons.size(); I-- > 0;) {
    HintButton& B = *Buttons[I];
    const float W = 28 + 10 + F.Measure(styles::Body, B.H.Label);
    X -= W;
    B.Bounds = Rect {X, Bounds.Y + (Bounds.H - 28) / 2, W, 28};
    X -= 28;
  }
  DrawFullRule(F, Bounds.Y + float(HairlinePx(S)) / S);  // the footer's top rule, edge to edge like the header's
  const float Max = std::max(0.0f, X - (Bounds.X + 48) - 16);
  if (!Segs.empty()) {
    static constexpr TextStyle PathStyle {"hint_path", FontId::MonoSemiBold, 14};
    constexpr float SegGap = 8;  // between segments: text widths drop trailing spaces, so spacing is explicit
    float Sx = Bounds.X + 48, Left = Max;
    static constexpr TextStyle ArrowStyle {"hint_arrow", FontId::MonoSemiBold, 16};  // Nunito has no arrows
    for (const Seg& G : Segs) {
      if (G.K == Seg::Kind::Gap) {
        Sx += 14;
        Left -= 14;
        continue;
      }
      if (G.K == Seg::Kind::Key) {
        if (Left < 28)
          break;
        const bool Arrow = G.Text.size() == 3 && uint8_t(G.Text[0]) == 0xE2;  // U+2190..2193
        F.Dc.R.FillCircle(std::round((Sx + 14) * S), std::round((Bounds.Y + Bounds.H / 2 - F.ScrollY) * S), std::round(14 * S), T.HintDisc);
        F.Text(Arrow ? ArrowStyle : cstyles::HintLetter, G.Text, Sx + 14, Bounds.Y, Bounds.H, T.HintLetter, Align::Center);
        Sx += 28 + SegGap;
        Left -= 28 + SegGap;
        continue;
      }
      const TextStyle& St = G.K == Seg::Kind::Body || G.K == Seg::Kind::Warn || G.K == Seg::Kind::Error ? styles::Body : G.K == Seg::Kind::Mono ? styles::MonoUi : PathStyle;
      const Color C = G.K == Seg::Kind::Body ? T.TextMuted : G.K == Seg::Kind::Warn ? T.Warn : G.K == Seg::Kind::Error ? T.Error : G.K == Seg::Kind::Mono ? T.Text : T.TextField;
      const std::string Fit = F.Dc.F.Fit(St, G.Text, S, Left * S, G.K == Seg::Kind::Path ? Ellipsis::Middle : Ellipsis::End);
      const float W = F.Dc.F.Measure(St, Fit, S) / S;
      F.Text(St, Fit, Sx, Bounds.Y, Bounds.H, C);
      Sx += W + SegGap;
      Left -= W + SegGap;
    }
  } else if (!Status.empty()) {
    const std::string Fit = F.Dc.F.Fit(styles::Body, Status, S, Max * S, Ellipsis::End);
    F.Text(styles::Body, Fit, Bounds.X + 48, Bounds.Y, Bounds.H, T.TextMuted);
  }
  for (auto& B : Buttons)
    B->Draw(F);
}

} // namespace ui
