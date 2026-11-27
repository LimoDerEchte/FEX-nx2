// SPDX-License-Identifier: MIT
// 2D layer interface: rects, rounded rects, rings, circles, lines, textured quads, clip and textures. All coordinates are
// device pixels (callers multiply design units by Display::Scale()); callers snap edges and hairlines to device pixels
// (Snap, HairlinePx). Shapes are antialiased in the shader at half-pixel coverage, so a shape on integer edges is exact.
// No Vulkan or libnx types here.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "platform/Display.h"

namespace ui {

struct Color {
  uint8_t R = 0, G = 0, B = 0, A = 255;
  static constexpr Color Rgb(uint8_t R, uint8_t G, uint8_t B, uint8_t A = 255) { return Color {R, G, B, A}; }
  constexpr Color WithAlpha(uint8_t Alpha) const { return Color {R, G, B, Alpha}; }
};

struct Rect {
  float X = 0, Y = 0, W = 0, H = 0;
};

inline float Snap(float V) { return std::round(V); }
inline Rect SnapRect(Rect R) {
  float X0 = Snap(R.X), Y0 = Snap(R.Y), X1 = Snap(R.X + R.W), Y1 = Snap(R.Y + R.H);
  return Rect {X0, Y0, X1 - X0, Y1 - Y0};
}

using TextureId = int32_t;
constexpr TextureId NoTexture = -1;

enum class TextureFormat : uint8_t {
  Rgba8,  // 4 bytes per texel, straight alpha
  Alpha8, // 1 byte per texel coverage, sampled as (1, 1, 1, value) and multiplied by the tint (glyph atlases)
};
enum class TextureFilter : uint8_t { Nearest, Linear };

class Renderer {
public:
  virtual ~Renderer() = default;

  // Output size and scale of the frame being drawn.
  virtual Size TargetSize() const = 0;
  float Scale() const { return ScaleForHeight(TargetSize().H); }

  // Call Display::BeginFrame first, then BeginFrame, draw, EndFrame (records, submits), then Display::Present.
  // BeginFrame false = the frame cannot be drawn; the draw calls become no-ops and EndFrame returns false.
  virtual bool BeginFrame(Color Clear) = 0;
  virtual bool EndFrame() = 0;

  // Everything drawn after this call is multiplied by Opacity (0..1) until it is set back to 1. The app shell uses it to cross-fade
  // a new screen over the old one. Colors keep their own alpha; the product is what reaches the blend.
  void SetOpacity(float V) { Opacity_ = V < 0 ? 0 : V > 1 ? 1 : V; }
  float Opacity() const { return Opacity_; }
  Color WithOpacity(Color C) const { return Opacity_ >= 1.0f ? C : C.WithAlpha(uint8_t(std::lround(float(C.A) * Opacity_))); }

  // Filled rect and filled rounded rect. Radius is clamped to half the shorter side.
  virtual void FillRect(Rect R, Color C) = 0;
  virtual void FillRoundRect(Rect R, float Radius, Color C) = 0;
  // Outline drawn inside R: a ring of Thickness along the rounded rect edge.
  virtual void StrokeRoundRect(Rect R, float Radius, float Thickness, Color C) = 0;
  void FillCircle(float Cx, float Cy, float Radius, Color C) { FillRoundRect(Rect {Cx - Radius, Cy - Radius, 2 * Radius, 2 * Radius}, Radius, C); }
  void Ring(float Cx, float Cy, float Radius, float Thickness, Color C) {
    StrokeRoundRect(Rect {Cx - Radius, Cy - Radius, 2 * Radius, 2 * Radius}, Radius, Thickness, C);
  }
  // Horizontal or vertical line, snapped to device pixels (thickness: max(1, round)). Returns false for any other angle
  // (diagonal lines are not implemented; the spec needs only hairlines and dividers).
  virtual bool Line(float X0, float Y0, float X1, float Y1, float Thickness, Color C) = 0;

  // Textured quad: Src in texels of the texture, Dst in device pixels, Tint multiplies the texel.
  virtual void Image(TextureId Tex, Rect Dst, Rect Src, Color Tint) = 0;

  // Scissor clip, intersected with the current clip, rounded outward to whole pixels.
  virtual void PushClip(Rect R) = 0;
  virtual void PopClip() = 0;

  // Textures. NoTexture on failure. UpdateTexture copies Pixels (row stride in bytes, 0 = tightly packed) and applies it
  // before the next frame's draws. DestroyTexture is safe at any time (the GPU is done with it first).
  virtual TextureId CreateTexture(int W, int H, TextureFormat Format, TextureFilter Filter) = 0;
  virtual bool UpdateTexture(TextureId Tex, int X, int Y, int W, int H, const void* Pixels, size_t StrideBytes) = 0;
  virtual void DestroyTexture(TextureId Tex) = 0;

private:
  float Opacity_ = 1.0f;
};

} // namespace ui
