// SPDX-License-Identifier: MIT
// Base of every shared component: one design-unit rect, the states (focused, pressed, disabled), activate/navigate/drag
// hooks and Draw. Components draw through Frame (DrawContext + clock + scroll offset) and use theme tokens only.
#pragma once

#include <cmath>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "Anim.h"
#include "DrawContext.h"
#include "input/Focus.h"

namespace ui {

struct Frame {
  const DrawContext& Dc;
  uint64_t Now = 0;
  float ScrollY = 0;  // du the scrolled content moved up

  float S() const { return Dc.S; }
  const Theme& T() const { return Dc.T; }
  Rect Box(float X, float Y, float W, float H) const { return Dc.Du(X, Y - ScrollY, W, H); }
  Rect Box(Rect R) const { return Box(R.X, R.Y, R.W, R.H); }
  Frame Fixed() const { return Frame {Dc, Now, 0}; }
  // Draws one line of text anchored at X (du; left/center/right per Align), vertically centred in the box Top..Top+H (du).
  float Text(const TextStyle& St, std::string_view Str, float X, float Top, float H, Color C, Align A = Align::Left) const {
    return Dc.F.Draw(St, Str, Dc.S, std::round(X * Dc.S), Dc.F.BaselineInBox(St, Dc.S, (Top - ScrollY) * Dc.S, H * Dc.S), C, A);
  }
  float Measure(const TextStyle& St, std::string_view Str) const { return Dc.F.Measure(St, Str, Dc.S) / Dc.S; }  // du
};

inline Color Opacity(Color C, float K) { return C.WithAlpha(uint8_t(std::lround(float(C.A) * Clamp01(K)))); }
inline Color Mix(Color A, Color B, float T) {
  T = Clamp01(T);
  auto L = [&](uint8_t X, uint8_t Y) { return uint8_t(std::lround(float(X) + (float(Y) - float(X)) * T)); };
  return Color {L(A.R, B.R), L(A.G, B.G), L(A.B, B.B), L(A.A, B.A)};
}
constexpr float DisabledOpacity = 0.4f;

// Spec styles that the typography table lists only by weight in prose.
namespace cstyles {
inline constexpr TextStyle Pill {"pill", FontId::NunitoBold, 16};                // PillButton body 15-16/700
inline constexpr TextStyle HintLetter {"hint_letter", FontId::NunitoExtraBold, 14};
inline constexpr TextStyle ModalTitle {"modal_title", FontId::NunitoExtraBold, 22};
inline constexpr TextStyle MonoUi14 {"mono_ui_14", FontId::MonoSemiBold, 14};   // "FX" in the app tile
inline constexpr TextStyle Value18 {"value_18", FontId::NunitoBold, 16};        // EnumChip value 16/700
inline constexpr TextStyle Crumb {"crumb", FontId::NunitoSemiBold, 16};         // file picker breadcrumbs 16/600
inline constexpr TextStyle Name17 {"name_17", FontId::NunitoBold, 17};          // file names, license names 17/700
inline constexpr TextStyle Chip13 {"chip_13", FontId::MonoSemiBold, 13};        // license chip 13/600
inline constexpr TextStyle Group13 {"group_13", FontId::NunitoBold, 13, 0.6f, 0, true};  // picker location group labels
inline constexpr TextStyle Meta14 {"meta_14", FontId::NunitoSemiBold, 14};
} // namespace cstyles

class Widget {
public:
  virtual ~Widget() = default;

  Rect Bounds;                 // du, in the unscrolled layout
  bool Visible = true;
  bool Enabled = true;         // disabled: drawn at 40%, skipped by navigation, no pointer
  bool Focusable = true;       // a stop of the focus tree
  bool Hittable = true;        // receives pointer hits (a hint, scrim or panel is hittable but not focusable)
  bool Fixed = false;          // not moved by the scroll offset (header, HintBar, modals)
  bool Focused = false;        // set by Interaction
  bool Pressed = false;        // set by Interaction (pointer down on it)
  int Id = NoFocus;
  int Group = NoFocus;
  Override Over[4];            // explicit navigation overrides (Dir index)
  std::function<void()> OnActivate;

  virtual void Activate(uint64_t Now) {
    (void)Now;
    if (OnActivate)
      OnActivate();
  }
  // Left/Right (or any direction) inside a focused control, for example cycling an enum. true = consumed.
  virtual bool OnNav(Dir D, uint64_t Now) {
    (void)D;
    (void)Now;
    return false;
  }
  virtual bool WantsDrag() const { return false; }
  // Pointer pressed or dragged while pressed: X in du (screen). Only for WantsDrag widgets.
  virtual void OnDragTo(float X, float Y, uint64_t Now) {
    (void)X;
    (void)Y;
    (void)Now;
  }
  virtual void Tick(uint64_t Now) { (void)Now; }
  virtual void Children(std::vector<Widget*>& Out) { (void)Out; }
  virtual void Draw(const Frame& F) = 0;

  // The focus ring: outset, stroke and the target's corner radius (du).
  virtual float RingOutset() const { return 10; }
  virtual float RingOutsetY() const { return RingOutset(); }  // vertical outset when it differs (list rows: inside the rules)
  virtual float RingStroke() const { return 4; }
  virtual float RingRadius() const { return 0; }
  virtual bool ShowsRing() const { return true; }  // false: focused, but no ring (the terminal surface)
  Rect RingClip {};  // du, screen space; H > 0: the ring is clipped to it (rows of a list that scrolls under the header/footer)

  float Fade = 1.0f;           // group opacity (modal fade)
  float Op() const { return Fade * (Enabled ? 1.0f : DisabledOpacity); }
};

} // namespace ui
