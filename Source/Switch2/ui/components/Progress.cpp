// SPDX-License-Identifier: MIT
#include "components/Progress.h"

#include <algorithm>

namespace ui {

void ProgressBar::SetValue(float V, uint64_t Now) {
  V = Clamp01(V);
  if (V <= Fill.To)
    return;
  Fill.Go(V, Now, 200);
}

void ProgressBar::Reset(uint64_t Now) {
  (void)Now;
  Fill.Set(0);
}

void ProgressBar::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float O = Op();
  const Rect B = F.Box(Bounds);
  F.Dc.R.FillRoundRect(B, B.H / 2, Opacity(T.Divider, O));
  const float V = Clamp01(Fill.Value(F.Now));
  if (V > 0) {
    Rect R = B;
    R.W = std::max(B.H, std::round(B.W * V));
    F.Dc.R.FillRoundRect(R, R.H / 2, Opacity(T.Accent, O));
  }
}

void LogView::Append(std::string Line, Kind K) {
  Lines.push_back(Entry {std::move(Line), K, {}});
  if (Lines.size() > MaxLines) {
    Lines.pop_front();
    ++DroppedLines;
  }
  if (FromBottom > 0)
    FromBottom += LineHeightDu;  // the view stays where the reader is
}

void LogView::AppendSpans(std::vector<Span> Parts) {
  std::string Text;
  for (const Span& P : Parts)
    Text += P.Text;
  Append(std::move(Text), Kind::Normal);
  Lines.back().Parts = std::move(Parts);
}

void LogView::ScrollBy(float Dy) {
  const float Max = std::max(0.0f, float(Lines.size()) * LineHeightDu - Bounds.H + 32);
  FromBottom = std::clamp(FromBottom - Dy, 0.0f, Max);
}

void LogView::Draw(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  const int Hair = HairlinePx(S);
  const Rect B = F.Box(Bounds);
  F.Dc.R.FillRoundRect(B, 16 * S, T.LogBg);
  F.Dc.R.StrokeRoundRect(B, 16 * S, float(Hair), T.Divider);
  const float LinePx = F.Dc.F.LineHeightPx(styles::MonoLog, S);
  LineHeightDu = LinePx / S;
  const Rect In = SnapRect(Rect {B.X + 20 * S, B.Y + 16 * S, B.W - 40 * S, B.H - 32 * S});
  F.Dc.R.PushClip(In);
  const float Bottom = In.Y + In.H + FromBottom * S;  // device y of the bottom of the last line
  const int Count = int(Lines.size());
  for (int I = Count - 1; I >= 0; --I) {
    const float LineBottom = Bottom - float(Count - 1 - I) * LinePx;
    const float LineTop = LineBottom - LinePx;
    if (LineTop >= In.Y + In.H)
      continue;
    if (LineBottom <= In.Y)
      break;
    const Entry& E = Lines[size_t(I)];
    auto ColorOf = [&](Kind K) { return K == Kind::Dim ? T.TextDim : K == Kind::Ok ? T.Ok : K == Kind::Warn ? T.Warn : K == Kind::Error ? T.Error : T.TextField; };
    const float Base = F.Dc.F.BaselineInBox(styles::MonoLog, S, LineTop, LinePx);
    float W = 0;
    if (E.Parts.empty()) {
      W = F.Dc.F.Draw(styles::MonoLog, E.Text, S, In.X, Base, ColorOf(E.K));
    } else {
      for (const Span& P : E.Parts)
        W += F.Dc.F.Draw(styles::MonoLog, P.Text, S, In.X + W, Base, ColorOf(P.K));
    }
    if (I == Count - 1 && ((F.Now / 530) % 2) == 0) {  // block cursor 8x15 du, blink 530 ms
      const float Cur = std::round(15 * S);
      F.Dc.R.FillRect(SnapRect(Rect {In.X + W + std::round(2 * S), LineTop + (LinePx - Cur) / 2, std::round(8 * S), Cur}), T.TextField);
    }
  }
  if (Count == 0 && ((F.Now / 530) % 2) == 0) {
    const float Cur = std::round(15 * S);
    F.Dc.R.FillRect(SnapRect(Rect {In.X, In.Y + In.H - LinePx + (LinePx - Cur) / 2, std::round(8 * S), Cur}), T.TextField);
  }
  F.Dc.R.PopClip();
}

} // namespace ui
