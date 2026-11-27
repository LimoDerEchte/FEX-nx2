// SPDX-License-Identifier: MIT
#include "components/Chrome.h"

namespace ui {

namespace {
constexpr TextStyle HeaderSub {"header_sub", FontId::NunitoRegular, 14};
} // namespace

void DrawFullRule(const Frame& F, float Y, float K) {
  const int Hair = HairlinePx(F.S());
  const float Py = std::round((Y - F.ScrollY) * F.S()) - float(Hair);
  F.Dc.R.Line(0, Py, float(F.Dc.R.TargetSize().W), Py, float(Hair), Opacity(F.T().Divider, K));
}

HeaderMetrics DrawScreenHeader(const Frame& F, IconId Icon, const std::string& Title, const std::string& Subtitle, bool HasSubtitle,
                               float K, bool WithIcon) {
  using namespace chrome;
  const Theme& T = F.T();
  const float S = F.S();
  F.Dc.R.FillRect(Rect {0, 0, float(F.Dc.R.TargetSize().W), std::round(HeaderH * S)}, Opacity(T.Bg, K));
  const float X = WithIcon ? TitleX : SideX;
  if (WithIcon)
    F.Dc.I.Draw(Icon, F.Dc.Du(SideX, HeaderCentre(IconSize), IconSize, IconSize), Opacity(T.Text, K), stroke::Gear);
  HeaderMetrics M;
  M.SubX = X;
  if (HasSubtitle || !Subtitle.empty()) {
    const float H1 = F.Dc.F.LineHeightPx(styles::TitleSettings, S) / S, H2 = F.Dc.F.LineHeightPx(HeaderSub, S) / S;
    const float Top = (HeaderH - (H1 + 2 + H2)) / 2;
    F.Text(styles::TitleSettings, Title, X, Top, H1, Opacity(T.Text, K));
    M.SubY = Top + H1 + 2;
    M.SubH = H2;
    if (!Subtitle.empty())
      F.Text(HeaderSub, Subtitle, X, M.SubY, H2, Opacity(T.TextMuted, K));
  } else {
    F.Text(styles::TitleSettings, Title, X, 0, HeaderH, Opacity(T.Text, K));
  }
  DrawFullRule(F, HeaderH, K);
  return M;
}

} // namespace ui
