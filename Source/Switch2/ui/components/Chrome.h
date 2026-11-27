// SPDX-License-Identifier: MIT
// The screen frame every full-screen page shares: a header band (icon, title, optional subtitle, a rule edge to edge), the
// body between header and footer, and the footer (the HintBar, whose rule also runs edge to edge). Screens place their own
// right-hand header controls at HeaderRight and vertically centre them with HeaderCentre.
#pragma once

#include <string>

#include "components/Widget.h"

namespace ui {

namespace chrome {
inline constexpr float HeaderH = 88;            // header band height (du)
inline constexpr float FooterY = 656, FooterH = 64;
inline constexpr float BodyTop = HeaderH, BodyH = FooterY - HeaderH;
inline constexpr float SideX = 48;              // left/right content inset of header and footer
inline constexpr float HeaderRight = 1280 - SideX;
inline constexpr float IconSize = 32, IconGap = 14;
inline constexpr float TitleX = SideX + IconSize + IconGap;
// Top of a control of height H centred in the header.
constexpr float HeaderCentre(float H) { return (HeaderH - H) / 2; }
} // namespace chrome

// Where the header put its text; a screen with a custom subtitle (mixed styles) draws it at SubX/SubY/SubH.
struct HeaderMetrics {
  float SubX = chrome::TitleX, SubY = 0, SubH = 0;
};

// Draws the header: background, icon, title (centred, or over the subtitle when HasSubtitle), and the bottom rule across the
// whole width. Subtitle text is drawn when not empty; with HasSubtitle and an empty Subtitle the caller draws it from the
// returned metrics. Opacity fades the whole header (modals).
HeaderMetrics DrawScreenHeader(const Frame& F, IconId Icon, const std::string& Title, const std::string& Subtitle = {},
                               bool HasSubtitle = false, float Opacity = 1.0f, bool WithIcon = true);

// A horizontal 1 px rule across the whole output width at design-unit Y (the rule's bottom edge sits on Y).
void DrawFullRule(const Frame& F, float Y, float Opacity = 1.0f);

} // namespace ui
