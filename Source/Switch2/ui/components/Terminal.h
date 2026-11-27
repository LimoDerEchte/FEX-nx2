// SPDX-License-Identifier: MIT
// Shell widgets: the KDE-style tab strip (only tabs, their close buttons and a new-tab button) and the terminal surface that
// draws a TerminalGrid in mono_term with the theme's Breeze palette.
#pragma once

#include <memory>

#include "TerminalGrid.h"
#include "components/Basic.h"

namespace ui {

namespace tabs {
inline constexpr float StripH = 42, PadX = 6, Gap = 2, ActiveH = 36, InactiveH = 34, BorderH = 1;
inline constexpr float LabelPadLeft = 14, LabelGap = 10, CloseSize = 24, PadRight = 6, MaxTabW = 240, NewSize = 32, NewGap = 4;
} // namespace tabs

class CloseButton final : public Widget {
public:
  CloseButton() { Focusable = false; Fixed = true; Bounds.W = Bounds.H = tabs::CloseSize; }
  void Draw(const Frame& F) override;
};

class TabItem final : public Widget {
public:
  TabItem() { Focusable = false; Fixed = true; }
  std::string Title;
  bool Active = false;
  CloseButton Close;
  void Children(std::vector<Widget*>& Out) override { Out.push_back(&Close); }
  void Draw(const Frame& F) override;
};

class NewTabButton final : public Widget {
public:
  NewTabButton() { Focusable = false; Fixed = true; Bounds.W = Bounds.H = tabs::NewSize; }
  void Draw(const Frame& F) override;
};

// Where the tabs go (du, strip-relative to the screen's top left): a pure function of the label widths, so tests need no fonts.
struct TabGeometry {
  std::vector<Rect> Tab, Close;
  Rect NewTab;
};
TabGeometry LayoutTabs(const std::vector<float>& LabelWidths, int Active);

class TabStrip final : public Widget {
public:
  TabStrip() { Focusable = false; Hittable = false; Fixed = true; Bounds = Rect {0, 0, 1280, tabs::StripH}; }
  std::function<void(int)> OnSelect, OnClose;
  std::function<void()> OnNew;
  bool NewEnabled = true;
  void SetTabs(const std::vector<std::string>& Titles, int Active);
  int Count() const { return int(Items.size()); }
  int Active() const { return ActiveIdx; }
  TabItem& Item(int I) { return *Items[size_t(I)]; }
  NewTabButton& NewBtn() { return New; }
  void Layout(const Frame& F);
  void Children(std::vector<Widget*>& Out) override {
    for (auto& I : Items)
      Out.push_back(I.get());
    Out.push_back(&New);
  }
  void Draw(const Frame& F) override;

private:
  std::vector<std::unique_ptr<TabItem>> Items;
  NewTabButton New;
  int ActiveIdx = 0;
};

// The 16 ANSI colors from the theme tokens (red, green, yellow, blue from the Breeze palette; the others derived from them).
struct AnsiPalette {
  Color C[16];
};
AnsiPalette MakeAnsiPalette(const Theme& T);

class TerminalView final : public Widget {
public:
  static constexpr float PadX = 18, PadY = 14;
  TerminalView() { Bounds = Rect {0, tabs::StripH, 1280, 656 - tabs::StripH}; }
  TerminalGrid* Grid = nullptr;
  // The line pitch in device pixels: the spec's line height 1.55 x 15 du x s, not rounded (the glyphs are placed on whole pixels).
  static float LinePitch(float S) { return styles::MonoTerm.LineHeight * styles::MonoTerm.Size * S; }
  // The grid size for the current bounds, fonts and scale (device pixel advance and line pitch).
  TerminalSize FitSize(const Frame& F) const;
  std::function<bool(Dir, uint64_t)> OnArrow;  // Up/Down/Left/Right go to the guest as arrow keys
  bool OnNav(Dir D, uint64_t Now) override { return OnArrow && OnArrow(D, Now); }
  void Draw(const Frame& F) override;
  bool ShowsRing() const override { return false; }
};

} // namespace ui
