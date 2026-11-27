// SPDX-License-Identifier: MIT
// SettingsRow and the controls it holds: Toggle, EnumChip, PathField, Slider (spec "Shared components").
#pragma once

#include <memory>

#include "components/Basic.h"

namespace ui {

// A control that sits at the right of a SettingsRow. Standalone it is a normal widget; inside a row it is not a focus stop
// (the row is) and the row forwards A, Left/Right and drags to it.
class Control : public Widget {
public:
  virtual float PreferredWidth(const Frame& F) = 0;
};

// Track 56x32 r999, knob 26, padding 3; "On"/"Off" label 36 wide right-aligned, gap 12. Knob slides 120 ms ease-out.
class Toggle final : public Control {
public:
  Toggle() { Bounds.W = 104; Bounds.H = 32; }
  bool On() const { return Value; }
  float KnobPos(uint64_t Now) const { return Knob.Value(Now); }  // 0 left .. 1 right, 120 ms
  void Set(bool V, uint64_t Now, bool Animate = true);
  std::function<void(bool)> OnChange;
  void Activate(uint64_t Now) override;
  void Draw(const Frame& F) override;
  float PreferredWidth(const Frame&) override { return 104; }
  float RingOutset() const override { return 6; }
  float RingStroke() const override { return 3; }
  float RingRadius() const override { return 16; }

private:
  bool Value = false;
  bool Init = false;
  Tween Knob;
};

// PillButton with padding 0 12 0 18: the value in accent 16/700, a chevron 18, gap 8. Left/Right cycle the values.
class EnumChip final : public Control {
public:
  EnumChip() { Bounds.H = 44; }
  std::vector<std::string> Values;
  int Index = 0;
  bool Wrap = true;  // false: Left on the first value and Right on the last are not consumed (the focus moves on)
  std::function<void(int)> OnChange;
  bool OnNav(Dir D, uint64_t Now) override;
  void Draw(const Frame& F) override;
  float PreferredWidth(const Frame& F) override;
  float RingOutset() const override { return 6; }
  float RingStroke() const override { return 3; }
  float RingRadius() const override { return 22; }
};

// Chip (max 340 wide, padding 8 12, r8, field bg, mono 13, middle ellipsis) + Browse pill, gap 10. A opens the picker.
class PathField final : public Control {
public:
  PathField() { Bounds.H = 44; Browse.Text = "Browse"; }
  std::string Path;
  void Draw(const Frame& F) override;
  float PreferredWidth(const Frame& F) override;

private:
  PillButton Browse;
  float ChipWidth(const Frame& F);
};

// Track 220 wide, value label 64 wide right-aligned (mono_ui 15/600), gap 14, filled part in accent.
class Slider final : public Control {
public:
  Slider() { Bounds.W = 298; Bounds.H = 32; }
  float Min = 0, Max = 100, Step = 1, Value = 0;
  bool LeaveAtEdge = false;  // true: Left at Min and Right at Max are not consumed (the focus moves on)
  std::function<std::string(float)> Format;  // default "%g"
  std::function<void(float)> OnChange;
  bool OnNav(Dir D, uint64_t Now) override;
  bool WantsDrag() const override { return true; }
  void OnDragTo(float X, float Y, uint64_t Now) override;
  void Activate(uint64_t) override {}
  void Draw(const Frame& F) override;
  float PreferredWidth(const Frame&) override { return 298; }
  float RingOutset() const override { return 6; }
  float RingStroke() const override { return 3; }
  float RingRadius() const override { return 16; }
  void SetValue(float V);

private:
  float Snap(float V) const;
};

// Min height 68, padding 10 0, 1 du bottom rule, gap 24; label row_label, description caption muted (gap 2). The whole row is
// the focus target; A acts on the control.
class SettingsRow final : public Widget {
public:
  SettingsRow() { Bounds.H = 74; }
  std::string Label, Description;
  // No rules between rows (removed in 0.17.13).
  static constexpr float BottomPad = 0;  // text block and control are centred in the full row (the rules that needed an offset are gone)
  void SetControl(std::unique_ptr<Control> C);
  Control* GetControl() { return Ctl.get(); }
  void Activate(uint64_t Now) override;
  bool OnNav(Dir D, uint64_t Now) override { return Ctl && Ctl->Enabled && Ctl->OnNav(D, Now); }
  bool WantsDrag() const override { return Ctl && Ctl->WantsDrag(); }
  void OnDragTo(float X, float Y, uint64_t Now) override {
    if (Ctl && Enabled)
      Ctl->OnDragTo(X, Y, Now);
  }
  void Tick(uint64_t Now) override {
    if (Ctl)
      Ctl->Tick(Now);
  }
  void Draw(const Frame& F) override;
  // 14 du left and right of the label and the control, 4 du inside the rules above and below.
  float RingOutset() const override { return 14; }
  float RingOutsetY() const override { return -4; }
  float RingStroke() const override { return 3; }
  float RingRadius() const override { return 12; }

private:
  std::unique_ptr<Control> Ctl;
};

} // namespace ui
