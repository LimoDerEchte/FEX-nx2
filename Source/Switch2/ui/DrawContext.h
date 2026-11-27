// SPDX-License-Identifier: MIT
// What a screen draws with: the 2D layer, text, icons, the active theme and the scale. Screens use only this (no Vulkan,
// no libnx).
#pragma once

#include "render/Fonts.h"
#include "render/Icons.h"
#include "render/Renderer.h"
#include "theme.h"

namespace ui {

struct DrawContext {
  Renderer& R;
  Fonts& F;
  Icons& I;
  const Theme& T;
  float S = 1.0f;  // output height / 720

  // A design-unit rect as a snapped device rect.
  Rect Du(float X, float Y, float W, float H) const { return SnapRect(Rect {X * S, Y * S, W * S, H * S}); }
};

} // namespace ui
