// SPDX-License-Identifier: MIT
// Icons: Lucide SVG (24-unit grid) rasterized with nanosvg at the target pixel size, cached per (icon, size, stroke width).
// The cached texels are white with coverage alpha, so the color is the draw tint: a theme switch re-rasterizes nothing.
// Uses only Renderer.h. The Renderer must outlive the Icons object.
#pragma once

#include <memory>

#include "assets/Assets.h"
#include "render/Renderer.h"

namespace ui {

// Stroke widths of the spec (on the 24-unit grid).
namespace stroke {
inline constexpr float Default = 2.0f;
inline constexpr float Gear = 1.8f;
inline constexpr float Card = 1.5f;
inline constexpr float Chevron = 2.3f;  // 2.2 to 2.4
inline constexpr float Close = 2.6f;
} // namespace stroke

struct IconsStats {
  int Pages = 0;
  int Rasterized = 0;  // icons rasterized since start (a repeat draw of the same key adds nothing)
  int Cached = 0;
};

class Icons {
public:
  explicit Icons(Renderer& R);
  ~Icons();
  Icons(const Icons&) = delete;
  Icons& operator=(const Icons&) = delete;

  // Draws the icon into Dst (device px, the icon is square: its size is round(min(W, H)), centered in Dst) with the color
  // as tint. Returns false when the icon could not be rasterized.
  bool Draw(IconId Id, Rect Dst, Color Tint, float StrokeWidth = stroke::Default);

  // Uploads icons added this frame. Call before Renderer::EndFrame().
  void EndFrame();
  IconsStats Stats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> P;
};

} // namespace ui
