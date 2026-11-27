// SPDX-License-Identifier: MIT
// FocusRing, VersionBlock, IconButton (gear), LaunchCard, PillButton, HintBar (spec "Shared components").
#pragma once

#include <memory>

#include "components/Widget.h"
#include "platform/Input.h"

namespace ui {

// The ring around the focused widget: accent, outset/stroke/radius from the widget, fades in over 100 ms, moves instantly
// between targets, hidden in touch mode.
class FocusRing {
public:
  // Call every frame with the focused widget id (NoFocus = none) and whether the ring may show (not in touch mode).
  void Update(int FocusedId, bool Show, uint64_t Now);
  float Alpha(uint64_t Now) const { return A.Value(Now); }
  // Draws the ring around W (its Bounds, scrolled by F.ScrollY).
  void Draw(const Frame& F, const Widget& W) const { DrawNow(F, W, A.Value(F.Now)); }
  // The ring at a fixed opacity (gallery: a widget shown in its focused state).
  static void DrawNow(const Frame& F, const Widget& W, float Alpha);

private:
  int Id = NoFocus;
  bool Shown = false;
  Tween A;
};

// App tile 44x44 r12 + 14 gap + "Version" (meta, muted) over the version string (mono_ui 15/600). Not focusable.
class VersionBlock final : public Widget {
public:
  VersionBlock() { Focusable = false; Hittable = false; Bounds.W = 44 + 14 + 190; Bounds.H = 44; }
  std::string Version = "v0 · FEX-0";  // built from the build constants by the launcher ("v<app> · FEX-<fex build>")
  void Draw(const Frame& F) override;
};

// 56x56 circle with a 28 du icon. Ring outset 10, radius 38.
class IconButton final : public Widget {
public:
  IconButton() { Bounds.W = Bounds.H = 56; }
  IconId Icon = IconId::Settings;
  float Stroke = stroke::Gear;
  void Draw(const Frame& F) override;
  float RingRadius() const override { return 28; }
};

// 280x300 r20 card: 120x120 r28 icon tile, label and subtitle. Press scales to 0.97 over 80 ms, then launches.
class LaunchCard final : public Widget {
public:
  LaunchCard() { Bounds.W = 280; Bounds.H = 300; }
  IconId Icon = IconId::Gamepad;
  std::string Label, Subtitle;
  void Activate(uint64_t Now) override;
  void Tick(uint64_t Now) override;
  void Draw(const Frame& F) override;
  float RingRadius() const override { return 20; }
  float CurrentScale(uint64_t Now) const { return Scale.Value(Now); }  // 1 -> 0.97 over 80 ms

private:
  uint64_t FireAt = 0;
  bool Pending = false;
  Tween Scale;
  bool ScaleInit = false;
  void Retarget(uint64_t Now);
};

// Height 44 (or Height), radius 999, padding 0 18. Kinds: the pill on surface_raised, the modal's secondary (Cancel/Close),
// accent fill and error fill (ConfirmDialog).
class PillButton : public Widget {
public:
  enum class Kind : uint8_t { Normal, Secondary, Accent, Error };
  PillButton() { Bounds.H = 44; }
  std::string Text;
  Kind K = Kind::Normal;
  float PadX = 18;
  float FixedWidth = 0;  // 0 = text width + 2 PadX
  // Updates Bounds.W for the current text (needs the fonts and the scale).
  void Layout(const Frame& F);
  void Draw(const Frame& F) override;
  float RingOutset() const override { return 6; }
  float RingStroke() const override { return 3; }
  float RingRadius() const override { return Bounds.H / 2; }
};

struct Hint {
  Button Btn = Button::A;
  std::string Label;
};
const char* HintLetter(Button B);  // "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "+", "-"

class HintBar;
class HintButton final : public Widget {
public:
  HintButton() { Focusable = false; Fixed = true; Bounds.H = 28; }
  Hint H;
  void Activate(uint64_t Now) override;
  void Draw(const Frame& F) override;
  HintBar* Owner = nullptr;
};

// Height 64 (Shell 52), 1 du top rule, left status text, right hints (28 du glyph + 10 + label, gap 28). Hints are tappable.
class HintBar final : public Widget {
public:
  HintBar() { Focusable = false; Hittable = false; Fixed = true; Bounds = Rect {0, 656, 1280, 64}; }
  std::string Status;
  // Optional structured status instead of Status: segments drawn left to right. Body = body 16/600 muted, Mono = mono_ui 15
  // `text` (the RootFS image name), Path = mono_ui 14 `text_field` with a middle ellipsis (the file picker's full path).
  struct Seg {
    // Key = a 28 du button disc holding `Text` (a button letter, or an arrow for a D-pad direction), as the hints on the right;
    // Gap = 14 du of space.
    enum class Kind : uint8_t { Body, Mono, Path, Warn, Error, Key, Gap } K = Kind::Body;  // Warn/Error = Body in the `warn`/`error` color
    std::string Text;
  };
  std::vector<Seg> Segs;
  std::function<void(Button)> OnHint;
  void SetHints(const std::vector<Hint>& Hints);
  // A disabled hint is drawn at 40% and does nothing (for example + Install without enough space).
  void SetHintEnabled(Button B, bool Enabled);
  size_t HintCount() const { return Buttons.size(); }
  void Children(std::vector<Widget*>& Out) override {
    for (HintButton* B : Buttons)
      Out.push_back(B);
  }
  void Draw(const Frame& F) override;

private:
  // SetHints reuses buttons and never frees one while the bar lives: Interaction and a press in progress may still hold a
  // pointer to a button that is no longer shown (until its next Reflatten).
  std::vector<std::unique_ptr<HintButton>> Pool;
  std::vector<HintButton*> Buttons;
};

} // namespace ui
