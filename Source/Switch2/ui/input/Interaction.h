// SPDX-License-Identifier: MIT
// Connects the platform events to the components: InputModes (modes, repeat, gestures) -> UI events -> FocusTree (spatial
// navigation, hover focuses, overrides) -> widget actions (A / tap / left click activates, B / right click goes back, Left/
// Right cycles, drags move sliders, scrolling). A stack of layers gives modals their own focus scope; closing one restores
// the focus under it. Also draws the focus ring and the mouse pointer. No Vulkan or libnx types.
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "components/Basic.h"
#include "input/Focus.h"
#include "input/InputModes.h"

namespace ui {

class Interaction {
public:
  Interaction();
  ~Interaction();
  Interaction(const Interaction&) = delete;
  Interaction& operator=(const Interaction&) = delete;

  // The base layer: the widgets of the screen (children included automatically). Focus and scroll survive changes.
  void SetRoot(const std::vector<Widget*>& Widgets);
  // A modal layer on top: the focus starts at InitialFocus (null = the first enabled), B/right click/scrim call OnBack.
  void PushLayer(const std::vector<Widget*>& Widgets, Widget* InitialFocus, std::function<void()> OnBack);
  void PopLayer();
  size_t LayerCount() const { return Layers.size(); }

  // Scrolling of the base layer's non-fixed widgets (Settings, Gallery). May stay null.
  ScrollView* Scroll = nullptr;
  std::function<void()> OnBack;                      // B on the base layer
  std::function<void(Button)> OnButton;              // X Y L R ZL ZR Plus Minus
  std::function<bool(float X, float Y, float Dy)> ScrollFilter;  // first chance at a scroll (LogView); true = handled
  std::function<void(float X, float Y)> OnLongPress;

  // One frame: scale (output pixels per du), raw events, the clock. Dispatches everything, ticks the widgets.
  void Update(uint64_t Now, float Scale, const std::vector<Event>& Raw);
  // Feeds one UI event as if InputModes produced it (tests).
  void Dispatch(const UiEvent& E, uint64_t Now);

  Mode GetMode() const { return Modes.GetMode(); }
  int FocusedId() const;
  Widget* FocusedWidget() const;
  bool SetFocus(int Id, uint64_t Now);
  Widget* HitWidget(float X, float Y, uint64_t Now) const;  // screen du
  float ScrollOffset(uint64_t Now) const { return Scroll ? Scroll->Offset(Now) : 0.0f; }
  Frame FrameFor(const Frame& Base, const Widget& W) const { return Frame {Base.Dc, Base.Now, W.Fixed ? 0.0f : ScrollOffset(Base.Now)}; }
  InputModes& GetModes() { return Modes; }
  const InputModes& GetModes() const { return Modes; }
  uint64_t Count(const char* What) const;  // counters for the probe/tests: "nav", "activate", "back"

  // The ring around the focused widget (hidden in touch mode) and the pointer arrow (mouse mode, hidden after 3 s idle).
  // Draw last, after the widgets and modals.
  void DrawOverlay(const Frame& F);

private:
  struct Layer {
    std::vector<Widget*> Roots;  // what SetRoot/PushLayer got; Flat is rebuilt from it each Update
    std::vector<Widget*> Flat;
    FocusTree Tree;
    std::function<void()> Back;
    bool Modal = false;
  };
  Layer& Top() { return Layers.back(); }
  const Layer& Top() const { return Layers.back(); }
  void Flatten(const std::vector<Widget*>& In, std::vector<Widget*>& Out);
  // Containers change their children between frames (HintBar::SetHints): rebuild every layer's Flat from its Roots and
  // drop a press whose widget left them.
  void Reflatten();
  void Rebuild(Layer& L, uint64_t Now);
  void ApplyFocus(Layer& L);
  void FocusChanged(Widget* W, uint64_t Now);
  Widget* ById(const Layer& L, int Id) const;
  void ClearPress();
  void DoScroll(float Dy, uint64_t Now);

  std::vector<Layer> Layers = std::vector<Layer>(1);
  InputModes Modes;
  FocusRing Ring;
  float LastScale = 0;
  int NextId = 1;
  Widget* PressedW = nullptr;
  float PtrX = 0, PtrY = 0;
  uint64_t NavCount = 0, ActivateCount = 0, BackCount = 0;

  struct PointerSprite;
  std::unique_ptr<PointerSprite> Sprite;
};

} // namespace ui
