// SPDX-License-Identifier: MIT
#include "components/Terminal.h"

#include <algorithm>
#include <cmath>

namespace ui {

// ---- Tabs ---------------------------------------------------------------------------------------------------------------

TabGeometry LayoutTabs(const std::vector<float>& LabelW, int Active) {
  using namespace tabs;
  TabGeometry G;
  float X = PadX;
  const float Bottom = StripH - BorderH;  // tabs sit on the border, bottom-aligned
  for (size_t I = 0; I < LabelW.size(); ++I) {
    const float H = int(I) == Active ? ActiveH : InactiveH;
    const float W = std::min(MaxTabW, LabelPadLeft + LabelW[I] + LabelGap + CloseSize + PadRight);
    G.Tab.push_back(Rect {X, Bottom - H, W, H});
    G.Close.push_back(Rect {X + W - PadRight - CloseSize, Bottom - H + (H - CloseSize) / 2, CloseSize, CloseSize});
    X += W + Gap;
  }
  const float NewX = LabelW.empty() ? PadX : X - Gap + NewGap;
  G.NewTab = Rect {NewX, Bottom - ActiveH + (ActiveH - NewSize) / 2, NewSize, NewSize};
  return G;
}

void CloseButton::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  if (Pressed)
    F.Dc.R.FillRoundRect(F.Box(Bounds), 4 * S, Opacity(T.KdeTextInactive, 0.2f));
  F.Dc.I.Draw(IconId::X, F.Box(Bounds.X + 6, Bounds.Y + 6, 12, 12), T.KdeTextInactive, stroke::Close);
}

void TabItem::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  const Rect B = F.Box(Bounds);
  if (Active) {
    F.Dc.R.FillRoundRect(B, 4 * S, T.KdeView);
    F.Dc.R.FillRect(Rect {B.X, B.Y + B.H / 2, B.W, B.H - B.H / 2}, T.KdeView);  // square bottom corners
    const float Bar = std::max(1.0f, std::round(2 * S));
    F.Dc.R.PushClip(Rect {B.X, B.Y, B.W, std::round(4 * S)});
    F.Dc.R.FillRoundRect(Rect {B.X, B.Y, B.W, std::round(8 * S)}, 4 * S, T.KdeFocus);
    F.Dc.R.PopClip();
    F.Dc.R.FillRect(Rect {B.X, B.Y + Bar, B.W, std::round(4 * S) - Bar}, T.KdeView);
  } else if (Pressed) {
    F.Dc.R.FillRoundRect(B, 4 * S, Opacity(T.KdeView, 0.5f));
  }
  const float TextW = Bounds.W - tabs::LabelPadLeft - tabs::LabelGap - tabs::CloseSize - tabs::PadRight;
  const std::string Fit = F.Dc.F.Fit(styles::KdeTab, Title, S, std::max(0.0f, TextW) * S, Ellipsis::End);
  F.Text(styles::KdeTab, Fit, Bounds.X + tabs::LabelPadLeft, Bounds.Y, Bounds.H, Active ? T.KdeText : T.KdeTextInactive);
  Close.Draw(F);
}

void NewTabButton::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  if (Pressed)
    F.Dc.R.FillRoundRect(F.Box(Bounds), 4 * S, Opacity(T.KdeTextInactive, 0.2f));
  F.Dc.I.Draw(IconId::Plus, F.Box(Bounds.X + 8, Bounds.Y + 8, 16, 16), Opacity(T.KdeTextInactive, Op()), stroke::Default);
}

void TabStrip::SetTabs(const std::vector<std::string>& Titles, int Active) {
  if (int(Titles.size()) != int(Items.size())) {
    Items.clear();
    for (size_t I = 0; I < Titles.size(); ++I) {
      auto T = std::make_unique<TabItem>();
      const int Idx = int(I);
      T->OnActivate = [this, Idx]() {
        if (OnSelect)
          OnSelect(Idx);
      };
      T->Close.OnActivate = [this, Idx]() {
        if (OnClose)
          OnClose(Idx);
      };
      Items.push_back(std::move(T));
    }
  }
  New.OnActivate = [this]() {
    if (NewEnabled && OnNew)
      OnNew();
  };
  ActiveIdx = Active;
  for (size_t I = 0; I < Items.size(); ++I) {
    Items[I]->Title = Titles[I];
    Items[I]->Active = int(I) == Active;
  }
}

void TabStrip::Layout(const Frame& F) {
  std::vector<float> W;
  for (auto& I : Items)
    W.push_back(F.Measure(styles::KdeTab, I->Title));
  const TabGeometry G = LayoutTabs(W, ActiveIdx);
  for (size_t I = 0; I < Items.size(); ++I) {
    Items[I]->Bounds = G.Tab[I];
    Items[I]->Close.Bounds = G.Close[I];
  }
  New.Bounds = G.NewTab;
  New.Enabled = NewEnabled;
}

void TabStrip::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  Layout(F);
  F.Dc.R.FillRect(F.Box(0, 0, Bounds.W, tabs::StripH - tabs::BorderH), T.KdeHeader);
  const int Hair = HairlinePx(S);
  F.Dc.R.FillRect(Rect {0, std::round((tabs::StripH - tabs::BorderH) * S), std::round(Bounds.W * S), float(Hair)}, T.KdeBorder);
  for (auto& I : Items)
    I->Draw(F);
  New.Draw(F);
}

// ---- Terminal -------------------------------------------------------------------------------------------------------------

AnsiPalette MakeAnsiPalette(const Theme& T) {
  AnsiPalette P;
  P.C[0] = T.KdeBorder;
  P.C[1] = T.AnsiRed;
  P.C[2] = T.AnsiGreen;
  P.C[3] = T.AnsiYellow;
  P.C[4] = T.AnsiBlue;
  P.C[5] = Mix(T.AnsiRed, T.AnsiBlue, 0.5f);
  P.C[6] = Mix(T.AnsiGreen, T.AnsiBlue, 0.5f);
  P.C[7] = T.KdeTextInactive;
  P.C[8] = Mix(T.KdeBorder, T.KdeText, 0.45f);
  for (int I = 1; I <= 6; ++I)
    P.C[8 + I] = Mix(P.C[I], T.KdeText, 0.15f);  // bright = the same hue, a little lighter
  P.C[15] = T.KdeText;
  return P;
}

TerminalSize TerminalView::FitSize(const Frame& F) const {
  const float S = F.S();
  return ComputeTerminalSize((Bounds.W - 2 * PadX) * S, (Bounds.H - 2 * PadY) * S, F.Dc.F.CellWidth(styles::MonoTerm, S), LinePitch(S));
}

void TerminalView::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  const Rect B = F.Box(Bounds);
  F.Dc.R.FillRect(B, T.KdeView);
  if (!Grid)
    return;
  const AnsiPalette Pal = MakeAnsiPalette(T);
  const float Adv = F.Dc.F.CellWidth(styles::MonoTerm, S);
  const float Line = LinePitch(S);
  const float X0 = std::round(B.X + PadX * S), Y0 = std::round(B.Y + PadY * S);
  F.Dc.R.PushClip(SnapRect(Rect {X0, Y0, B.W - 2 * PadX * S, B.H - 2 * PadY * S}));
  auto Resolve = [&](const CellColor& C, bool Bold, bool Fg, Color& Out) {
    switch (C.K) {
    case CellColor::Kind::Default: Out = Fg ? T.KdeText : T.KdeView; return false;
    case CellColor::Kind::Ansi: Out = Pal.C[std::min<int>(15, C.Index + (Bold && Fg && C.Index < 8 ? 8 : 0))]; return true;
    case CellColor::Kind::Rgb: Out = Color::Rgb(C.R, C.G, C.B); return true;
    case CellColor::Kind::Focus: Out = T.KdeFocus; return true;
    case CellColor::Kind::Inactive: Out = T.KdeTextInactive; return true;
    }
    return false;
  };
  for (int Row = 0; Row < Grid->Rows(); ++Row) {
    const float Y = std::round(Y0 + float(Row) * Line);
    if (Y > B.Y + B.H)
      break;
    int Col = 0;
    while (Col < Grid->Cols()) {
      const Cell& First = Grid->ViewAt(Col, Row);
      int End = Col + 1;
      while (End < Grid->Cols() && First.Width == 1 && First.Ch < 128 &&
             Grid->ViewAt(End, Row).Width == 1 && Grid->ViewAt(End, Row).Ch < 128 &&
             Grid->ViewAt(End, Row).SameStyle(First))
        ++End;
      End = std::max(End, Col + First.Width);
      Color Fg, Bg;
      Resolve(First.Fg, First.Bold, true, Fg);
      bool HasBg = Resolve(First.Bg, false, false, Bg);
      if (First.Reverse) {
        std::swap(Fg, Bg);
        Fg = First.Bg.K == CellColor::Kind::Default ? T.KdeView : Fg;
        if (First.Fg.K == CellColor::Kind::Default)
          Bg = T.KdeText;
        HasBg = true;
      }
      const float Xa = std::round(X0 + float(Col) * Adv), Xb = std::round(X0 + float(End) * Adv);
      if (HasBg)
        F.Dc.R.FillRect(SnapRect(Rect {Xa, Y, Xb - Xa, std::round(Y0 + float(Row + 1) * Line) - Y}), Bg);
      if (!First.Hidden && (!First.Blink || (F.Now / 530) % 2 == 0)) {
        std::string Text;
        for (int C = Col; C < End; ++C) {
          const Cell& Glyph = Grid->ViewAt(C, Row);
          if (Glyph.Width)
            AppendTerminalUtf8(Text, Glyph.Ch);
        }
        if (First.Dim)
          Fg = Mix(Bg, Fg, 0.6f);
        if (Text.find_first_not_of(' ') != std::string::npos) {
          TextStyle St = First.Bold ? styles::MonoTermBold : styles::MonoTerm;
          St.Italic = First.Italic;
          const float Base = F.Dc.F.BaselineInBox(St, S, Y, Line);
          F.Dc.R.PushClip(SnapRect(Rect {Xa, Y, Xb - Xa, std::round(Y0 + float(Row + 1) * Line) - Y}));  // a run never spills into the next run's cells
          if (First.Width == 2)
            F.Dc.F.Draw(St, Text, S, (Xa + Xb) / 2, Base, Fg, Align::Center);
          else
            F.Dc.F.Draw(St, Text, S, Xa, Base, Fg);
          F.Dc.R.PopClip();
        }
        const float Stroke = std::max(1.0f, std::round(S));
        if (First.Underline)
          F.Dc.R.FillRect(SnapRect(Rect {Xa, Y + Line - std::round(2 * S), Xb - Xa, Stroke}), Fg);
        if (First.Strike)
          F.Dc.R.FillRect(SnapRect(Rect {Xa, Y + Line / 2, Xb - Xa, Stroke}), Fg);
      }
      Col = End;
    }
  }
  // Block cursor 9x18 du, blinking at 530 ms, only on the live screen.
  if (Grid->CursorVisible() && Grid->ScrollOffset() == 0 && (!Grid->CursorBlinks() || (F.Now / 530) % 2 == 0)) {
    const float Cw = std::round(Adv), Ch = std::round(18 * S);
    const float Cx = std::round(X0 + float(Grid->CursorCol()) * Adv), Cy = Y0 + float(Grid->CursorRow()) * Line + (Line - Ch) / 2;
    F.Dc.R.FillRect(SnapRect(Rect {Cx, Cy, Cw, Ch}), T.KdeText);
  }
  F.Dc.R.PopClip();
}

} // namespace ui
