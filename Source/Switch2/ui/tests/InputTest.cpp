// SPDX-License-Identifier: MIT
// Host tests for Task 3 (make check-ui): input modes, repeat timing, gestures, spatial navigation, overrides, scroll-into-view,
// hit targets, component animations and the gallery driven through the real Interaction path. Fake clock, fake backends: no GPU.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "DrawContext.h"
#include "components/Controls.h"
#include "components/Modals.h"
#include "components/Progress.h"
#include "input/Focus.h"
#include "input/InputModes.h"
#include "input/Interaction.h"
#include "platform/Log.h"
#include "platform/fake/FakePlatform.h"
#include "screens/gallery.h"

namespace {
int Failures = 0, Checks = 0;

#define CHECK(Cond)                                                          \
  do {                                                                       \
    ++Checks;                                                                \
    if (!(Cond)) {                                                           \
      ++Failures;                                                            \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond);            \
    }                                                                        \
  } while (0)

using namespace ui;

Event Btn(Button B, bool Down, uint64_t T = 0) {
  Event E;
  E.Kind = Down ? Event::Type::ButtonDown : Event::Type::ButtonUp;
  E.Btn = B;
  E.TimeMs = T;
  return E;
}
Event StickEv(int Idx, float X, float Y, uint64_t T = 0) {
  Event E;
  E.Kind = Event::Type::Stick;
  E.StickIndex = uint8_t(Idx);
  E.X = X;
  E.Y = Y;
  E.TimeMs = T;
  return E;
}
Event TouchEv(TouchPhase P, float X, float Y, uint64_t T, uint32_t Id = 1) {
  Event E;
  E.Kind = Event::Type::Touch;
  E.Phase = P;
  E.X = X;
  E.Y = Y;
  E.TouchId = Id;
  E.TimeMs = T;
  return E;
}
Event MoveEv(float X, float Y, float Dx, float Dy, uint64_t T) {
  Event E;
  E.Kind = Event::Type::MouseMove;
  E.X = X;
  E.Y = Y;
  E.DX = Dx;
  E.DY = Dy;
  E.TimeMs = T;
  return E;
}
Event MouseBtnEv(MouseButton B, bool Down, uint64_t T) {
  Event E;
  E.Kind = Event::Type::MouseButton;
  E.Mouse = B;
  E.Pressed = Down;
  E.TimeMs = T;
  return E;
}
Event WheelEv(float Dy, uint64_t T) {
  Event E;
  E.Kind = Event::Type::MouseWheel;
  E.DY = Dy;
  E.TimeMs = T;
  return E;
}

int Count(const std::vector<UiEvent>& V, UiEvent::Type K) {
  int N = 0;
  for (const UiEvent& E : V)
    N += E.Kind == K;
  return N;
}
int CountNav(const std::vector<UiEvent>& V, Dir D) {
  int N = 0;
  for (const UiEvent& E : V)
    N += E.Kind == UiEvent::Type::Nav && E.D == D;
  return N;
}

// ---- InputModes ------------------------------------------------------------------------------------------------------------

void TestRepeat() {
  InputModes M;
  M.SetScale(1.0f);
  std::vector<UiEvent> Out;
  M.Feed(Btn(Button::Down, true, 1000), Out);
  CHECK(CountNav(Out, Dir::Down) == 1);  // the press itself
  Out.clear();
  M.Update(1399, Out);
  CHECK(Out.empty());  // first repeat only after 400 ms
  M.Update(1400, Out);
  CHECK(CountNav(Out, Dir::Down) == 1);
  Out.clear();
  M.Update(1489, Out);
  CHECK(Out.empty());
  M.Update(1490, Out);
  CHECK(CountNav(Out, Dir::Down) == 1);  // then every 90 ms
  Out.clear();
  M.Update(1580, Out);
  CHECK(CountNav(Out, Dir::Down) == 1);
  Out.clear();
  M.Feed(Btn(Button::Down, false, 1590), Out);
  M.Update(2000, Out);
  CHECK(CountNav(Out, Dir::Down) == 0);  // released: no more repeats

  // A frame that took 300 ms catches up, but with a bound.
  Out.clear();
  M.Feed(Btn(Button::Left, true, 5000), Out);
  Out.clear();
  M.Update(5400 + 270, Out);
  CHECK(CountNav(Out, Dir::Left) == 4);  // 400, 490, 580, 670

  // Stick: the same timing, 0.5 deadzone.
  InputModes St;
  St.SetScale(1.0f);
  Out.clear();
  St.Feed(StickEv(0, 0.4f, 0, 100), Out);
  CHECK(Out.empty());  // inside the deadzone: nothing, not even a mode change
  CHECK(St.GetMode() == Mode::Controller);
  St.Feed(StickEv(0, 0.6f, 0.1f, 200), Out);
  CHECK(CountNav(Out, Dir::Right) == 1);
  Out.clear();
  St.Update(599, Out);
  CHECK(Out.empty());
  St.Update(600, Out);
  CHECK(CountNav(Out, Dir::Right) == 1);
  Out.clear();
  St.Feed(StickEv(0, 0.0f, -0.9f, 610), Out);  // pushed down: a new direction starts a new press
  CHECK(CountNav(Out, Dir::Down) == 1);
  St.Feed(StickEv(0, 0, 0, 620), Out);
  Out.clear();
  St.Update(5000, Out);
  CHECK(Out.empty());
  St.Feed(StickEv(0, 0.8f, 0, 5010), Out);
  CHECK(CountNav(Out, Dir::Right) == 1);
  Out.clear();
  St.Feed(Event {.Kind = Event::Type::PadCancel, .TimeMs = 5020}, Out);
  St.Update(6000, Out);
  CHECK(Out.empty());
}

void TestModes() {
  InputModes M;
  M.SetScale(2.0f);  // 1 du = 2 px
  std::vector<UiEvent> Out;
  CHECK(M.GetMode() == Mode::Controller);
  // Motion of 2 du or less does not enter mouse mode; more does.
  M.Feed(MoveEv(100, 100, 2, 0, 100), Out);  // 1 du
  CHECK(M.GetMode() == Mode::Controller && !M.PointerVisible());
  M.Feed(MoveEv(102, 100, 2, 0, 110), Out);  // total 2 du: not past it
  CHECK(M.GetMode() == Mode::Controller);
  M.Feed(MoveEv(104, 100, 2, 0, 120), Out);  // 3 du
  CHECK(M.GetMode() == Mode::Mouse && M.PointerVisible());
  CHECK(Count(Out, UiEvent::Type::ModeChanged) == 1);
  CHECK(Count(Out, UiEvent::Type::PointerMove) == 1);
  CHECK(std::fabs(M.PointerX() - 52.0f) < 0.01f);  // output px to du
  // The pointer hides after 3 s idle but the mode stays.
  Out.clear();
  M.Update(120 + 2999, Out);
  CHECK(M.PointerVisible());
  M.Update(120 + 3000, Out);
  CHECK(!M.PointerVisible() && M.GetMode() == Mode::Mouse);
  // Any button: controller mode.
  M.Feed(Btn(Button::X, true, 4000), Out);
  CHECK(M.GetMode() == Mode::Controller);
  // Touch down: touch mode.
  M.Feed(TouchEv(TouchPhase::Down, 200, 200, 4100), Out);
  CHECK(M.GetMode() == Mode::Touch);
  M.Feed(TouchEv(TouchPhase::Up, 200, 200, 4150), Out);
  // A click enters mouse mode at once (no motion needed).
  Out.clear();
  M.Feed(MouseBtnEv(MouseButton::Left, true, 4200), Out);
  CHECK(M.GetMode() == Mode::Mouse);
  CHECK(Count(Out, UiEvent::Type::PointerDown) == 1);
  M.Feed(MouseBtnEv(MouseButton::Left, false, 4210), Out);
  M.Feed(MouseBtnEv(MouseButton::Left, true, 4220), Out);
  Event Cancel = MouseBtnEv(MouseButton::Left, false, 4230);
  Cancel.Kind = Event::Type::MouseCancel;
  M.Feed(Cancel, Out);
  CHECK(Out.back().Kind == UiEvent::Type::PointerUp && !Out.back().Tap);
  // Right click = B.
  Out.clear();
  M.Feed(MouseBtnEv(MouseButton::Right, true, 4300), Out);
  bool SawB = false;
  for (const UiEvent& E : Out)
    SawB |= E.Kind == UiEvent::Type::Button && E.Btn == Button::B;
  CHECK(SawB);
  // The wheel scrolls (up = back towards the start).
  Out.clear();
  M.Feed(WheelEv(2, 4400), Out);
  CHECK(Out.size() == 1 && Out[0].Kind == UiEvent::Type::Scroll && Out[0].DY < 0);
  // The pointer is 24 du.
  CHECK(tuning::PointerSizeDu == 24.0f && tuning::PointerHideMs == 3000);
}

void TestGestures() {
  // Tap: moved < 12 du is a tap; 12 du or more is a drag.
  {
    InputModes M;
    M.SetScale(1.0f);
    std::vector<UiEvent> Out;
    M.Feed(TouchEv(TouchPhase::Down, 100, 100, 1000), Out);
    M.Feed(TouchEv(TouchPhase::Move, 100, 111.5f, 1050), Out);
    CHECK(Count(Out, UiEvent::Type::Drag) == 0);
    M.Feed(TouchEv(TouchPhase::Up, 100, 111.5f, 1100), Out);
    CHECK(Out.back().Kind == UiEvent::Type::PointerUp && Out.back().Tap);
  }
  {
    InputModes M;
    M.SetScale(2.0f);  // 12 du = 24 px
    std::vector<UiEvent> Out;
    M.Feed(TouchEv(TouchPhase::Down, 100, 100, 1000), Out);
    M.Feed(TouchEv(TouchPhase::Move, 100, 124, 1050), Out);  // exactly 12 du
    CHECK(Count(Out, UiEvent::Type::Drag) == 1);
    CHECK(std::fabs(Out.back().DY - 12.0f) < 0.01f);  // the whole distance since the press is delivered
    M.Feed(TouchEv(TouchPhase::Move, 100, 164, 1500), Out);  // slow, then stops: no inertia
    M.Feed(TouchEv(TouchPhase::Up, 100, 164, 1900), Out);
    CHECK(!Out.back().Tap);
    Out.clear();
    M.Update(1950, Out);
    M.Update(2000, Out);
    CHECK(Count(Out, UiEvent::Type::Scroll) == 0);
  }
  // Long press: 500 ms without moving; the release is then not a tap.
  {
    InputModes M;
    M.SetScale(1.0f);
    std::vector<UiEvent> Out;
    M.Feed(TouchEv(TouchPhase::Down, 50, 50, 1000), Out);
    M.Update(1499, Out);
    CHECK(Count(Out, UiEvent::Type::LongPress) == 0);
    M.Update(1500, Out);
    CHECK(Count(Out, UiEvent::Type::LongPress) == 1);
    M.Update(1600, Out);
    CHECK(Count(Out, UiEvent::Type::LongPress) == 1);  // once
    M.Feed(TouchEv(TouchPhase::Up, 50, 50, 1650), Out);
    CHECK(!Out.back().Tap);
  }
  // Inertia: a fast flick keeps scrolling and decays by 0.92 per 16 ms.
  {
    InputModes M;
    M.SetScale(1.0f);
    std::vector<UiEvent> Out;
    M.Feed(TouchEv(TouchPhase::Down, 100, 500, 1000), Out);
    for (int I = 1; I <= 5; ++I)
      M.Feed(TouchEv(TouchPhase::Move, 100, 500 - 20.0f * float(I), 1000 + 16 * uint64_t(I)), Out);
    M.Feed(TouchEv(TouchPhase::Up, 100, 400, 1085), Out);
    Out.clear();
    M.Update(1085, Out);  // sets the clock
    Out.clear();
    M.Update(1101, Out);
    float First = 0;
    for (const UiEvent& E : Out)
      if (E.Kind == UiEvent::Type::Scroll && E.Inertia)
        First = E.DY;
    CHECK(First > 0);  // the finger moved up, so the content scrolls on towards the end
    Out.clear();
    M.Update(1117, Out);
    float Second = 0;
    for (const UiEvent& E : Out)
      if (E.Kind == UiEvent::Type::Scroll && E.Inertia)
        Second = E.DY;
    CHECK(std::fabs(Second / First - 0.92f) < 0.01f);
    // It dies out.
    int Guard = 0;
    uint64_t T = 1117;
    while (Guard++ < 600) {
      Out.clear();
      T += 16;
      M.Update(T, Out);
      if (Count(Out, UiEvent::Type::Scroll) == 0)
        break;
    }
    CHECK(Guard < 600);
  }
  // Right stick scrolls in controller mode: up scrolls back.
  {
    InputModes M;
    M.SetScale(1.0f);
    std::vector<UiEvent> Out;
    M.Feed(StickEv(1, 0, 1.0f, 100), Out);
    M.Update(100, Out);
    Out.clear();
    M.Update(116, Out);
    CHECK(Out.size() == 1 && Out[0].Kind == UiEvent::Type::Scroll && Out[0].DY < 0);
  }
}

// ---- Focus -----------------------------------------------------------------------------------------------------------------

FocusNode Node(int Id, float X, float Y, float W = 40, float H = 40, bool Enabled = true) {
  FocusNode N;
  N.Id = Id;
  N.R = Rect {X, Y, W, H};
  N.Enabled = Enabled;
  return N;
}

void TestFocus() {
  // 3x3 grid, 100 du apart.
  FocusTree T;
  T.Begin();
  for (int R = 0; R < 3; ++R)
    for (int C = 0; C < 3; ++C)
      T.Add(Node(1 + R * 3 + C, 100.0f * float(C), 100.0f * float(R)));
  T.End();
  CHECK(T.Focused() == 1);  // the first enabled
  T.SetFocus(5);
  CHECK(T.Navigate(Dir::Up) == 2);
  CHECK(T.Navigate(Dir::Down) == 5);
  CHECK(T.Navigate(Dir::Left) == 4);
  CHECK(T.Navigate(Dir::Right) == 5);
  T.SetFocus(1);
  CHECK(T.Navigate(Dir::Up) == NoFocus && T.Focused() == 1);  // nothing above: stays
  CHECK(T.Navigate(Dir::Left) == NoFocus);

  // Disabled nodes are skipped: with 6 disabled, Right from 5 goes to a diagonal on the edge of the cone; the tie (3 vs 9)
  // is broken by the lower id.
  FocusTree D;
  D.Begin();
  for (int R = 0; R < 3; ++R)
    for (int C = 0; C < 3; ++C)
      D.Add(Node(1 + R * 3 + C, 100.0f * float(C), 100.0f * float(R), 40, 40, 1 + R * 3 + C != 6));
  D.End();
  D.SetFocus(5);
  CHECK(D.Navigate(Dir::Right) == 3);
  D.SetFocus(5);
  CHECK(D.SetFocus(6) == false);  // a disabled node cannot be focused

  // Cone: a node more sideways than ahead is not a candidate.
  FocusTree K;
  K.Begin();
  K.Add(Node(1, 0, 0));
  K.Add(Node(2, 100, 200));  // right 100, down 200: outside the right cone, inside the down cone
  K.End();
  K.SetFocus(1);
  CHECK(K.Navigate(Dir::Right) == NoFocus);
  CHECK(K.Navigate(Dir::Down) == 2);

  // The nearest wins; a closer node beats a farther one in the same direction; equal distance: smaller sideways offset.
  FocusTree N;
  N.Begin();
  N.Add(Node(1, 0, 0));
  N.Add(Node(2, 150, 0));
  N.Add(Node(3, 300, 0));
  N.Add(Node(4, 150, 40));  // as far as 2 plus a bit
  N.End();
  N.SetFocus(1);
  CHECK(N.Navigate(Dir::Right) == 2);
  FocusTree Tie;
  Tie.Begin();
  Tie.Add(Node(1, 0, 0));
  Tie.Add(Node(7, 100, -30));  // dist 104.4
  Tie.Add(Node(5, 100, 30));   // dist 104.4, same sideways: lower id
  Tie.Add(Node(9, 104, 0));    // straight ahead, 104: the smaller sideways offset wins the tie within 0.5 du
  Tie.End();
  Tie.SetFocus(1);
  CHECK(Tie.Navigate(Dir::Right) == 9);

  // Explicit overrides: an id, and "the last focused of a group". Up from any card goes to the gear; the gear's Down goes
  // back to the last focused card.
  FocusTree O;
  O.Begin();
  FocusNode Gear = Node(10, 600, 0, 56, 56);
  Gear.Over[int(Dir::Down)].Group = 7;
  O.Add(Gear);
  for (int I = 0; I < 3; ++I) {
    FocusNode C = Node(20 + I, 164.0f + 336.0f * float(I), 200, 280, 300);
    C.Group = 7;
    C.Over[int(Dir::Up)].Id = 10;
    O.Add(C);
  }
  O.End();
  O.SetFocus(21);
  CHECK(O.Navigate(Dir::Up) == 10);
  CHECK(O.Navigate(Dir::Down) == 21);  // the last focused card, not the geometrically nearest
  O.SetFocus(22);
  O.Navigate(Dir::Up);
  CHECK(O.Navigate(Dir::Down) == 22);
  // A disabled override target falls back to spatial navigation.
  FocusTree O2;
  O2.Begin();
  FocusNode A = Node(1, 0, 100);
  A.Over[int(Dir::Up)].Id = 2;
  O2.Add(A);
  O2.Add(Node(2, 500, 0, 40, 40, false));
  O2.Add(Node(3, 0, 0));
  O2.End();
  O2.SetFocus(1);
  CHECK(O2.Navigate(Dir::Up) == 3);

  // The focus survives a rebuild; it moves to the first enabled node when its node disappears.
  FocusTree R;
  R.Begin();
  R.Add(Node(1, 0, 0));
  R.Add(Node(2, 100, 0));
  R.End();
  R.SetFocus(2);
  R.Begin();
  R.Add(Node(1, 0, 0));
  R.Add(Node(2, 100, 0));
  R.End();
  CHECK(R.Focused() == 2);
  R.Begin();
  R.Add(Node(1, 0, 0));
  R.End();
  CHECK(R.Focused() == 1);

  // Hit targets are never smaller than 44x44 du, even for a 24 du visual.
  const Rect H = HitRect(Rect {100, 100, 24, 24});
  CHECK(H.W == 44 && H.H == 44 && H.X == 90 && H.Y == 90);
  const Rect Big = HitRect(Rect {0, 0, 280, 300});
  CHECK(Big.W == 280 && Big.H == 300);
  FocusTree Hit;
  Hit.Begin();
  Hit.Add(Node(1, 100, 100, 24, 24));
  Hit.End();
  CHECK(Hit.HitTest(91, 91) == 1 && Hit.HitTest(133, 133) == 1);
  CHECK(Hit.HitTest(89, 100) == NoFocus && Hit.HitTest(134, 100) == NoFocus);
}

void TestScroll() {
  ScrollView V;
  V.SetViewport(72, 584, 1648);
  CHECK(V.MaxOffset() == 1648 - 584);
  // A target below the viewport: scroll by the smallest amount with a 24 du margin, 150 ms ease-out.
  V.EnsureVisible(Rect {0, 700, 100, 50}, 1000);
  CHECK(V.Target() == 700 + 50 + 24 - 72 - 584);
  CHECK(V.Offset(1000) == 0);
  const float Mid = V.Offset(1075);
  CHECK(Mid > 0 && Mid < V.Target());
  CHECK(Mid > V.Target() * 0.5f);  // ease-out: past halfway at half time
  CHECK(V.Offset(1150) == V.Target());
  // A target above: back to the top, clamped to 0.
  V.EnsureVisible(Rect {0, 80, 100, 50}, 2000);
  CHECK(V.Target() == 0);
  // Already visible: no scroll.
  V.EnsureVisible(Rect {0, 300, 100, 50}, 3000);
  CHECK(V.Target() == 0);
  // A target in the margin band: the margin is kept.
  V.EnsureVisible(Rect {0, 640, 100, 10}, 4000);  // bottom 650 + 24 > 656
  CHECK(V.Target() == 650 + 24 - 72 - 584);
  // Direct changes (wheel, drag) are immediate and clamped.
  V.By(1e6f, 5000);
  CHECK(V.Offset(5000) == V.MaxOffset());
  V.By(-1e6f, 5001);
  CHECK(V.Offset(5001) == 0);
}

// ---- Components ------------------------------------------------------------------------------------------------------------

void TestComponents() {
  // ProgressBar: eases over 200 ms and never moves backwards.
  ProgressBar B;
  B.SetValue(0.5f, 1000);
  CHECK(B.Displayed(1000) == 0);
  const float Mid = B.Displayed(1100);
  CHECK(Mid > 0 && Mid < 0.5f);
  CHECK(B.Displayed(1200) == 0.5f);
  B.SetValue(0.3f, 1300);  // backwards: ignored
  CHECK(B.Target() == 0.5f && B.Displayed(1500) == 0.5f);
  B.SetValue(0.4f, 1300);
  CHECK(B.Target() == 0.5f);
  // While easing to 0.8 the displayed value only ever increases.
  B.SetValue(0.8f, 2000);
  float Prev = B.Displayed(2000);
  bool Monotonic = true;
  for (uint64_t T = 2000; T <= 2300; T += 7) {
    const float V = B.Displayed(T);
    Monotonic &= V >= Prev;
    Prev = V;
  }
  CHECK(Monotonic && Prev == 0.8f);
  B.SetValue(2.0f, 3000);
  CHECK(B.Target() == 1.0f);
  B.Reset(4000);
  CHECK(B.Target() == 0 && B.Displayed(4000) == 0);

  // LogView: a ring buffer of 2,000 lines.
  LogView L;
  for (int I = 0; I < 2500; ++I)
    L.Append("line " + std::to_string(I));
  CHECK(L.LineCount() == 2000 && L.Dropped() == 500);
  CHECK(L.Line(0) == "line 500" && L.Line(1999) == "line 2499");
  CHECK(LogView::MaxLines == 2000);

  // FocusRing: fades in over 100 ms, moves instantly, hidden in touch mode.
  FocusRing R;
  R.Update(5, true, 1000);
  CHECK(R.Alpha(1000) == 0);
  CHECK(R.Alpha(1050) > 0 && R.Alpha(1050) < 1);
  CHECK(R.Alpha(1100) == 1);
  R.Update(5, true, 1200);
  CHECK(R.Alpha(1200) == 1);  // same target: no new fade
  R.Update(6, true, 1300);  // a new target starts a new fade (the ring itself is at the new place at once)
  CHECK(R.Alpha(1300) == 0 && R.Alpha(1400) == 1);
  R.Update(6, false, 1500);  // touch mode
  CHECK(R.Alpha(1500) == 0 && R.Alpha(1700) == 0);
  R.Update(6, true, 1800);
  CHECK(R.Alpha(1800) == 0 && R.Alpha(1900) == 1);

  // Toggle: the knob slides for 120 ms.
  Toggle T;
  T.Set(false, 0);
  T.Set(true, 1000);
  CHECK(T.KnobPos(1000) == 0 && T.KnobPos(1060) > 0 && T.KnobPos(1060) < 1 && T.KnobPos(1120) == 1);
  bool Seen = false;
  T.OnChange = [&](bool On) { Seen = On; };
  T.Activate(2000);
  CHECK(!T.On() && !Seen);

  // LaunchCard: pressed scales to 0.97 over 80 ms, then launches (once).
  LaunchCard C;
  int Launched = 0;
  C.OnActivate = [&]() { ++Launched; };
  C.Tick(1000);
  CHECK(C.CurrentScale(1000) == 1.0f);
  C.Activate(1000);
  CHECK(C.CurrentScale(1040) < 1.0f && C.CurrentScale(1040) > 0.97f);
  CHECK(std::fabs(C.CurrentScale(1080) - 0.97f) < 1e-5f);
  C.Tick(1079);
  CHECK(Launched == 0);
  C.Tick(1080);
  CHECK(Launched == 1);
  C.Tick(1200);
  CHECK(Launched == 1 && C.CurrentScale(1300) == 1.0f);

  // Pressed by the pointer: 0.97 while down, back on release.
  LaunchCard C2;
  C2.Tick(0);
  C2.Pressed = true;
  C2.Tick(100);
  CHECK(std::fabs(C2.CurrentScale(180) - 0.97f) < 1e-5f);
  C2.Pressed = false;
  C2.Tick(300);
  CHECK(C2.CurrentScale(380) == 1.0f);

  // Modal fade: 120 ms.
  ConfirmDialog D;
  D.Show(1000);
  CHECK(D.FadeValue(1000) == 0 && D.FadeValue(1060) > 0 && D.FadeValue(1060) < 1 && D.FadeValue(1120) == 1);
  D.Hide(2000);
  CHECK(D.IsOpen() == false && D.Visible(2060) && !D.Visible(2120));

  // EnumChip cycles with Left/Right (wraps); Slider steps and clamps.
  EnumChip E;
  E.Values = {"a", "b", "c"};
  E.Index = 2;
  int Changed = -1;
  E.OnChange = [&](int I) { Changed = I; };
  CHECK(E.OnNav(Dir::Right, 0) && E.Index == 0 && Changed == 0);
  CHECK(E.OnNav(Dir::Left, 0) && E.Index == 2);
  CHECK(!E.OnNav(Dir::Up, 0));
  Slider S;
  S.Min = 0; S.Max = 100; S.Step = 25; S.SetValue(50);
  S.Bounds = Rect {100, 0, 298, 32};
  CHECK(S.OnNav(Dir::Right, 0) && S.Value == 75);
  CHECK(S.OnNav(Dir::Right, 0) && S.OnNav(Dir::Right, 0) && S.Value == 100);
  S.OnDragTo(100 + 110, 10, 0);
  CHECK(S.Value == 50);
  S.OnDragTo(-500, 10, 0);
  CHECK(S.Value == 0);
}

// ---- Rig with a fake renderer for components that measure text -------------------------------------------------------------

struct Rig {
  FakeDisplay D {Size {1280, 720}};
  FakeRenderer R {D};
  Fonts F {R};
  Icons I {R};
  Rig() { SetTheme(ThemeMode::Dark, AccentPreset::Cyan); }
  DrawContext Ctx() { return DrawContext {R, F, I, CurrentTheme(), 1.0f}; }
};

int CountFill(const FakeRenderer& R, const char* WH) {
  int N = 0;
  for (const std::string& C : R.Calls)
    if (C.rfind("FillRect", 0) == 0 && C.find(WH) != std::string::npos)
      ++N;
  return N;
}

void TestCursorBlink() {
  Rig G;
  LogView L;
  L.Bounds = Rect {0, 0, 400, 200};
  L.Append("hello");
  auto CursorAt = [&](uint64_t Now) {
    G.R.Calls.clear();
    const DrawContext C = G.Ctx();
    L.Draw(Frame {C, Now, 0});
    return CountFill(G.R, " 8.000000 15.000000 ");
  };
  CHECK(CursorAt(100) == 1);   // on for 530 ms
  CHECK(CursorAt(529) == 1);
  CHECK(CursorAt(530) == 0);   // then off for 530 ms
  CHECK(CursorAt(1059) == 0);
  CHECK(CursorAt(1060) == 1);
}

// ---- Gallery through the real input path -------------------------------------------------------------------------------------

struct GalRig : Rig {
  Gallery Gal;
  uint64_t Now = 1000;
  void Step(const std::vector<Event>& Ev = {}, uint64_t Advance = 16) {
    Now += Advance;
    Gal.Update(Now, 1.0f, Ev);
    R.Calls.clear();
    const DrawContext C = Ctx();
    Gal.Draw(C, Now);
  }
  void Run(uint64_t Ms) {
    for (uint64_t T = 0; T < Ms; T += 16)
      Step();
  }
  void Press(Button B) {
    Step({Btn(B, true, Now)});
    Step({Btn(B, false, Now)});
  }
  Widget* Focused() { return Gal.Input().FocusedWidget(); }
};

void TestGallery() {
  GalRig G;
  G.Step();
  Interaction& In = G.Gal.Input();
    CHECK(In.GetMode() == Mode::Controller);
  CHECK(G.Focused() != nullptr);

  // D-pad moves the focus; Down from the gear is the group override (the first card, as none was focused before).
  Widget* Gear = G.Focused();
  G.Press(Button::Down);
  Widget* Card = G.Focused();
  CHECK(Card != Gear && dynamic_cast<LaunchCard*>(Card) != nullptr);
  G.Press(Button::Up);  // Up from a card: the gear
  CHECK(G.Focused() == Gear);
  G.Press(Button::Down);
  CHECK(G.Focused() == Card);  // the last focused card
  // A launches after the 80 ms press.
  G.Press(Button::A);
  CHECK(G.Gal.LastAction() == "none");
  G.Run(100);
  CHECK(G.Gal.LastAction() == "card 1 launched");
  // Disabled widgets are skipped: the Store card and the Disabled pill/row are never focused while walking Right/Down.
  for (int I = 0; I < 40; ++I) {
    G.Press(I % 3 == 0 ? Button::Right : Button::Down);
    CHECK(G.Focused() == nullptr || G.Focused()->Enabled);
  }

  // Focus scrolls into view with the margin.
  G.Run(300);
  Widget* W = G.Focused();
  const float Off = G.Gal.Scrolling().Offset(G.Now);
  if (W && !W->Fixed) {
    const float Y0 = W->Bounds.Y - Off, Y1 = Y0 + W->Bounds.H;
    CHECK(Y0 >= 72 + 24 - 1 || Off == 0);
    CHECK(Y1 <= 656 - 24 + 1 || Off == G.Gal.Scrolling().MaxOffset());
  }

  // Mouse: hover focuses the widget under the pointer; left click activates; right click goes back.
  GalRig M;
  M.Step();
  M.Gal.Scrolling().To(0, M.Now, 0);
  PillButton* Pill = M.Gal.FirstPill();
  M.Gal.Scrolling().To(Pill->Bounds.Y - 200, M.Now, 0);
  M.Step();
  const float Off2 = M.Gal.Scrolling().Offset(M.Now);
  const float Px = Pill->Bounds.X + 20, Py = Pill->Bounds.Y - Off2 + 20;
  M.Step({MoveEv(Px - 30, Py, 30, 0, M.Now)});
  M.Step({MoveEv(Px, Py, 30, 0, M.Now)});
  CHECK(M.Gal.Input().GetMode() == Mode::Mouse);
  CHECK(M.Focused() == Pill);  // hover focuses
  M.Step({MouseBtnEv(MouseButton::Left, true, M.Now)});
  CHECK(Pill->Pressed);
  M.Step({MouseBtnEv(MouseButton::Left, false, M.Now)});
  CHECK(!Pill->Pressed && M.Gal.LastAction() == "pill Normal");
  M.Step({MouseBtnEv(MouseButton::Right, true, M.Now)});
  CHECK(M.Gal.LastAction().rfind("B on the base page", 0) == 0);
  // The wheel scrolls the page.
  const float Before = M.Gal.Scrolling().Offset(M.Now);
  M.Step({WheelEv(-3, M.Now)});
  CHECK(M.Gal.Scrolling().Offset(M.Now) > Before);
  // A click outside any widget does nothing.
  M.Step({MoveEv(5, 300, 40, 0, M.Now)});

  // Touch: tap activates; the ring is hidden; a drag scrolls and cancels the tap; touch down on a slider drags it.
  GalRig T;
  T.Step();
  T.Gal.Scrolling().To(T.Gal.FirstPill()->Bounds.Y - 200, T.Now, 0);
  T.Step();
  const float TOff = T.Gal.Scrolling().Offset(T.Now);
  PillButton* TP = T.Gal.FirstPill();
  const float Tx = TP->Bounds.X + 30, Ty = TP->Bounds.Y - TOff + 20;
  T.Step({TouchEv(TouchPhase::Down, Tx, Ty, T.Now)});
  CHECK(T.Gal.Input().GetMode() == Mode::Touch && TP->Pressed);
  T.Step({TouchEv(TouchPhase::Up, Tx, Ty, T.Now)});
  CHECK(!TP->Pressed && T.Gal.LastAction() == "pill Normal");
  T.Gal.LastActionReset();
  const float TBefore = T.Gal.Scrolling().Offset(T.Now);
  T.Step({TouchEv(TouchPhase::Down, Tx, Ty, T.Now)});
  T.Step({TouchEv(TouchPhase::Move, Tx, Ty - 100, T.Now)});
  T.Step({TouchEv(TouchPhase::Up, Tx, Ty - 100, T.Now + 400)}, 400);  // slow release: no inertia
  CHECK(T.Gal.Scrolling().Offset(T.Now) > TBefore + 90);
  CHECK(T.Gal.LastAction() == "none" && !TP->Pressed);  // the drag was not a tap
  // Slider drag.
  Slider* Sl = T.Gal.FirstSlider();
  T.Gal.Scrolling().To(Sl->Bounds.Y - 200, T.Now, 0);
  T.Step();
  const float SOff = T.Gal.Scrolling().Offset(T.Now);
  const float Sy = Sl->Bounds.Y - SOff + 16;
  T.Step({TouchEv(TouchPhase::Down, Sl->Bounds.X + 22, Sy, T.Now)});
  T.Step({TouchEv(TouchPhase::Move, Sl->Bounds.X + 110, Sy, T.Now)});
  CHECK(std::fabs(Sl->Value - 50) < 1e-3f);
  T.Step({TouchEv(TouchPhase::Move, Sl->Bounds.X + 44, Sy + 2, T.Now)});
  CHECK(std::fabs(Sl->Value - 20) < 1e-3f);
  T.Step({TouchEv(TouchPhase::Up, Sl->Bounds.X + 44, Sy + 2, T.Now)});

  // Left/Right on the focused row cycles the enum without opening the picker.
  GalRig E;
  E.Step();
  EnumChip* Chip = E.Gal.FirstEnum();
  Chip->Index = 0;
  const int Idx = Chip->Index;
  // Focus the row holding the chip with the mouse (hover), then use the d-pad.
  Widget* Row = E.Gal.RowFor(Chip);
  CHECK(Row != nullptr);
  E.Gal.Input().SetFocus(Row->Id, E.Now);
  E.Step();
  E.Press(Button::Right);
  CHECK(Chip->Index == (Idx + 1) % 4);
  CHECK(E.Gal.ActiveModal() == nullptr);
  E.Press(Button::A);  // A opens the picker
  CHECK(E.Gal.ActiveModal() != nullptr);
  E.Run(200);
  // The picker opens with the focus on the current value; Down moves; A selects and closes; the focus returns to the row.
  Widget* Cur = E.Focused();
  CHECK(Cur != nullptr && Cur != Row);
  E.Press(Button::Down);
  CHECK(E.Focused() != Cur);
  const int Wanted = (Chip->Index + 1) % 4;
  E.Press(Button::A);
  CHECK(E.Gal.ActiveModal() == nullptr);
  CHECK(Chip->Index == Wanted);
  E.Run(200);
  CHECK(E.Focused() == Row);

  // B cancels a modal; so does a tap on the scrim; ConfirmDialog starts on Cancel.
  GalRig Cf;
  Cf.Step();
  Cf.Gal.OpenConfirm(false, Cf.Now);
  Cf.Run(200);
  CHECK(Cf.Gal.ActiveModal() != nullptr);
  CHECK(Cf.Focused() != nullptr && static_cast<ModalBase*>(Cf.Gal.ActiveModal())->InitialFocus() == Cf.Focused());
  Cf.Press(Button::B);
  CHECK(Cf.Gal.ActiveModal() == nullptr);
  Cf.Run(200);
  Cf.Gal.OpenConfirm(true, Cf.Now);
  Cf.Run(200);
  Cf.Step({TouchEv(TouchPhase::Down, 20, 300, Cf.Now)});
  Cf.Step({TouchEv(TouchPhase::Up, 20, 300, Cf.Now)});
  CHECK(Cf.Gal.ActiveModal() == nullptr);  // the scrim tap cancelled
  // A tap inside the dialog panel does not.
  Cf.Run(200);
  Cf.Gal.OpenPicker(Cf.Now);
  Cf.Run(200);
  Cf.Step({TouchEv(TouchPhase::Down, 640 - 240, 200, Cf.Now)});
  Cf.Step({TouchEv(TouchPhase::Up, 640 - 240, 200, Cf.Now)});
  CHECK(Cf.Gal.ActiveModal() != nullptr);
  // Confirm with A on the confirm button.
  Cf.Gal.CloseModal(Cf.Now);
  Cf.Run(200);
  Cf.Gal.OpenConfirm(false, Cf.Now);
  Cf.Run(200);
  Cf.Press(Button::Left);  // Cancel -> Confirm is to its right; Left from Cancel is nothing: Right goes to nothing too
  Cf.Press(Button::Right);
  Cf.Gal.CloseModal(Cf.Now);

  // Touch mode hides the ring; a button press brings it back.
  GalRig Hd;
  Hd.Step();
  Hd.Step({TouchEv(TouchPhase::Down, 5, 300, Hd.Now)});
  Hd.Step({TouchEv(TouchPhase::Up, 5, 300, Hd.Now)});
  CHECK(Hd.Gal.Input().GetMode() == Mode::Touch);
  Hd.Press(Button::Down);
  CHECK(Hd.Gal.Input().GetMode() == Mode::Controller);

  // The status line is built from real counters.
  CHECK(In.Count("nav") > 0 && In.Count("activate") > 0);
}

} // namespace

int main() {
  SetLogSink([](const char*) {});
  TestRepeat();
  TestModes();
  TestGestures();
  TestFocus();
  TestScroll();
  TestComponents();
  TestCursorBlink();
  TestGallery();
  std::printf("ui-input-test: %d checks, %d failures\n", Checks, Failures);
  return Failures ? 1 : 0;
}
