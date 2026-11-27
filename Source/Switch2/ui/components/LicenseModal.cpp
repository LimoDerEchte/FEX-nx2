// SPDX-License-Identifier: MIT
#include "components/LicenseModal.h"
#include "components/Chrome.h"

#include <algorithm>

namespace ui {

constexpr float ScreenW = 1280, ScreenH = 720;

std::vector<std::string> WrapLines(const Frame& F, const TextStyle& St, const std::string& Text, float Width) {
  std::vector<std::string> Out;
  size_t Pos = 0;
  while (Pos <= Text.size()) {
    size_t Nl = Text.find('\n', Pos);
    if (Nl == std::string::npos)
      Nl = Text.size();
    std::string Para = Text.substr(Pos, Nl - Pos);
    if (!Para.empty() && Para.back() == '\r')
      Para.pop_back();
    Pos = Nl + 1;
    if (Para.empty()) {
      Out.emplace_back();
      continue;
    }
    // Keep leading indentation of the paragraph on its first line.
    size_t I = 0;
    std::string Cur;
    while (I < Para.size()) {
      size_t J = Para.find(' ', I + 1);
      if (J == std::string::npos)
        J = Para.size();
      const std::string Piece = Para.substr(I, J - I);  // a word with its leading space(s)
      const std::string Try = Cur + Piece;
      if (!Cur.empty() && F.Measure(St, Try) > Width) {
        Out.push_back(Cur);
        Cur = Piece.size() > 0 && Piece[0] == ' ' ? Piece.substr(1) : Piece;
      } else {
        Cur = Try;
      }
      // A single piece wider than the line: cut it by characters.
      while (F.Measure(St, Cur) > Width && Cur.size() > 1) {
        size_t Cut = Cur.size();
        while (Cut > 1 && F.Measure(St, Cur.substr(0, Cut)) > Width)
          --Cut;
        while (Cut > 1 && (static_cast<unsigned char>(Cur[Cut]) & 0xC0) == 0x80)
          --Cut;
        Out.push_back(Cur.substr(0, Cut));
        Cur = Cur.substr(Cut);
      }
      I = J;
    }
    Out.push_back(Cur);
    if (Nl == Text.size())
      break;
  }
  return Out;
}

// ---- LicenseModal -------------------------------------------------------------------------------------------------------

LicenseModal::LicenseModal() {
  Close.Text = "Close";
  Close.K = PillButton::Kind::Secondary;
  Close.Bounds.H = 48;
  Close.Fixed = true;
  Close.OnActivate = [this]() {
    if (OnCancel)
      OnCancel();
  };
  Read.Text = "Read license text";
  Read.K = PillButton::Kind::Secondary;
  Read.Bounds.H = 48;
  Read.Fixed = true;
  Read.OnActivate = [this]() {
    if (OnReadText)
      OnReadText();
  };
}

void LicenseModal::Open(const LicenseEntry& E, uint64_t Now) {
  Entry = E;
  HasText = LicenseTextAvailable(E);
  Read.Enabled = HasText;
  Show(Now);
}

void LicenseModal::Layer(std::vector<Widget*>& Out) {
  Out.push_back(&Scrim);
  Out.push_back(&Panel);
  if (HasText)
    Out.push_back(&Read);
  Out.push_back(&Close);
}

void LicenseModal::Draw(const Frame& Fr) {
  const Frame F = Fr.Fixed();
  const float K = Alpha.Value(F.Now);
  if (K <= 0.001f && !Opened)
    return;
  const Theme& T = F.T();
  const float S = F.S();
  const float W = 640, InnerW = W - 56;
  const float TitleH = F.Dc.F.LineHeightPx(cstyles::ModalTitle, S) / S, BodyH = F.Dc.F.LineHeightPx(styles::Body, S) / S;
  const float MonoH = F.Dc.F.LineHeightPx(styles::MonoUiSmall, S) / S;
  const std::vector<std::string> Note = WrapLines(F, styles::Body, Entry.Note, InnerW);
  const float BoxH = 14 + 2 * MonoH + 6 + 14;
  const float H = 28 + 32 + 14 + float(Note.size()) * (BodyH + 4) + 16 + BoxH + 20 + 48 + 28;
  Dialog = Rect {(ScreenW - W) / 2, std::round((ScreenH - H) / 2), W, H};
  Panel.Bounds = Dialog;
  Close.Layout(F);
  Close.Bounds.W = std::max(Close.Bounds.W, 120.0f);
  Close.FixedWidth = Close.Bounds.W;
  Read.Layout(F);
  const float By = Dialog.Y + H - 28 - 48;
  Close.Bounds.X = Dialog.X + W - 28 - Close.Bounds.W;
  Close.Bounds.Y = By;
  Read.Bounds.X = Close.Bounds.X - 12 - Read.Bounds.W;
  Read.Bounds.Y = By;
  Close.Fade = Read.Fade = K;
  DrawScrim(F, Dialog, K);
  // Title row: name 22/800 and the license chip in accent.
  const float ChipW = F.Measure(cstyles::Chip13, Entry.License) + 28;
  const float NameMax = InnerW - ChipW - 14;
  F.Text(cstyles::ModalTitle, F.Dc.F.Fit(cstyles::ModalTitle, Entry.Name, S, NameMax * S, Ellipsis::End), Dialog.X + 28, Dialog.Y + 28, 32, Opacity(T.Text, K));
  const float NameW = std::min(NameMax, F.Measure(cstyles::ModalTitle, Entry.Name));
  const float ChipX = Dialog.X + 28 + NameW + 14;
  const Rect Chip = F.Box(ChipX, Dialog.Y + 28, ChipW, 32);
  F.Dc.R.StrokeRoundRect(Chip, Chip.H / 2, std::max(1.0f, std::round(2 * S)), Opacity(T.Accent, K));
  F.Text(cstyles::Chip13, Entry.License, ChipX + ChipW / 2, Dialog.Y + 28, 32, Opacity(T.Accent, K), Align::Center);
  (void)TitleH;
  float Y = Dialog.Y + 28 + 32 + 14;
  for (const std::string& L : Note) {
    F.Text(styles::Body, L, Dialog.X + 28, Y, BodyH, Opacity(T.TextMuted, K));
    Y += BodyH + 4;
  }
  Y += 12;
  F.Dc.R.FillRoundRect(F.Box(Dialog.X + 28, Y, InnerW, BoxH), 12 * S, Opacity(T.LogBg, K));
  const float LabelW = F.Measure(styles::MonoUiSmall, "source  ");
  const std::string Text = HasText ? LicenseTextPath(Entry) : std::string("not bundled");
  auto Line = [&](const char* Label, const std::string& Value, float Ly) {
    F.Text(styles::MonoUiSmall, Label, Dialog.X + 28 + 16, Ly, MonoH, Opacity(T.TextDim, K));
    const std::string Fit = F.Dc.F.Fit(styles::MonoUiSmall, Value, S, (InnerW - 32 - LabelW) * S, Ellipsis::Middle);
    F.Text(styles::MonoUiSmall, Fit, Dialog.X + 28 + 16 + LabelW, Ly, MonoH, Opacity(T.TextField, K));
  };
  Line("source", Entry.Url.empty() ? std::string("-") : Entry.Url, Y + 14);
  Line("text", Text, Y + 14 + MonoH + 6);
  if (HasText)
    Read.Draw(F);
  Close.Draw(F);
}

// ---- FullTextView -------------------------------------------------------------------------------------------------------

FullTextView::FullTextView() {
  Close.Text = "Close";
  Close.K = PillButton::Kind::Secondary;
  Close.Bounds.H = 44;
  Close.Fixed = true;
  Close.OnActivate = [this]() {
    if (OnCancel)
      OnCancel();
  };
  Hints.SetHints({{Button::B, "Back"}, {Button::ZL, "Page up"}, {Button::ZR, "Page down"}});
  Hints.OnHint = [this](Button B) {
    if (OnHint)
      OnHint(B);
  };
  // The whole screen is the panel: a tap on it must not close the view (the scrim sits under it).
  Panel.Bounds = Rect {0, 0, ScreenW, ScreenH};
}

void FullTextView::Open(uint64_t Now) {
  Off = 0;
  WrapScale = 0;
  Lines.clear();
  Show(Now);
}

void FullTextView::Layer(std::vector<Widget*>& Out) {
  Out.push_back(&Scrim);
  Out.push_back(&Panel);
  Out.push_back(&Close);
  Out.push_back(&Hints);
}

void FullTextView::ScrollBy(float Dy) { Off = std::clamp(Off + Dy, 0.0f, MaxOffset()); }
void FullTextView::Page(int Dir) { ScrollBy(float(Dir) * (ViewH - 48)); }

void FullTextView::Draw(const Frame& Fr) {
  const Frame F = Fr.Fixed();
  const float K = Alpha.Value(F.Now);
  if (K <= 0.001f && !Opened)
    return;
  const Theme& T = F.T();
  const float S = F.S();
  LineH = F.Dc.F.LineHeightPx(styles::MonoLog, S) / S;
  if (WrapScale != S || Lines.empty()) {
    WrapScale = S;
    Lines = WrapLines(F, styles::MonoLog, Text, 1280 - 96);
  }
  const float ViewTop = 88, ViewBottom = 656;
  ViewH = ViewBottom - ViewTop - 40;  // 20 padding top and bottom
  ContentH = float(Lines.size()) * LineH;
  Off = std::clamp(Off, 0.0f, MaxOffset());
  const Size Sz = F.Dc.R.TargetSize();
  F.Dc.R.FillRect(Rect {0, 0, float(Sz.W), float(Sz.H)}, Opacity(T.Bg, K));
  // Header (88): title 28/800 with the subtitle, Close at the right.
  DrawScreenHeader(F, IconId::Info, Title, Subtitle, true, K, false);
  Close.Layout(F);
  Close.Bounds.W = std::max(Close.Bounds.W, 100.0f);
  Close.FixedWidth = Close.Bounds.W;
  Close.Bounds.X = chrome::HeaderRight - Close.Bounds.W;
  Close.Bounds.Y = chrome::HeaderCentre(44);
  Close.Fade = K;
  Close.Draw(F);
  F.Dc.R.PushClip(F.Dc.Du(0, ViewTop, 1280, ViewBottom - ViewTop));
  const size_t First = size_t(std::max(0.0f, std::floor((Off - 20) / LineH)));
  for (size_t I = First; I < Lines.size(); ++I) {
    const float Y = ViewTop + 20 + float(I) * LineH - Off;
    if (Y > ViewBottom)
      break;
    F.Text(styles::MonoLog, Lines[I], 48, Y, LineH, Opacity(T.TextField, K));
  }
  F.Dc.R.PopClip();
  // Scroll indicator.
  if (MaxOffset() > 0) {
    const float TrackH = ViewBottom - ViewTop - 16;
    const float ThumbH = std::max(32.0f, TrackH * ViewH / ContentH);
    const float ThumbY = ViewTop + 8 + (TrackH - ThumbH) * (Off / MaxOffset());
    F.Dc.R.FillRoundRect(F.Box(1280 - 14, ThumbY, 4, ThumbH), 2 * S, Opacity(T.Divider, K));
  }
  Hints.Fade = K;
  Hints.Draw(F);
}

} // namespace ui
