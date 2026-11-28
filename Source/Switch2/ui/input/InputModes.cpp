// SPDX-License-Identifier: MIT
#include "input/InputModes.h"

#include <algorithm>
#include <cmath>

namespace ui {

const char* ModeName(Mode M) { return M == Mode::Controller ? "controller" : M == Mode::Mouse ? "mouse" : "touch"; }

void InputModes::SetScale(float NewS) {
  if (NewS <= 0)
    return;
  const float Dx = PxX / S, Dy = PxY / S;
  S = NewS;
  PxX = Dx * S;
  PxY = Dy * S;
}

bool InputModes::PointerVisible() const { return Cur == Mode::Mouse && PointerEver && Now - PointerActive < tuning::PointerHideMs; }

void InputModes::SetMode(Mode M, std::vector<UiEvent>& Out) {
  if (M == Cur)
    return;
  Cur = M;
  if (M == Mode::Mouse)
    MoveAccum = 0;
  UiEvent E;
  E.Kind = UiEvent::Type::ModeChanged;
  E.NewMode = M;
  Out.push_back(E);
}

void InputModes::Emit(std::vector<UiEvent>& Out, UiEvent::Type K, float X, float Y, PointerSource Src) const {
  UiEvent E;
  E.Kind = K;
  E.X = X;
  E.Y = Y;
  E.Src = Src;
  Out.push_back(E);
}

void InputModes::StartRepeat(Dir D, int Source, uint64_t T, std::vector<UiEvent>& Out) {
  UiEvent E;
  E.Kind = UiEvent::Type::Nav;
  E.D = D;
  Out.push_back(E);
  R = Rep {true, D, Source, T + tuning::RepeatFirstMs};
}

void InputModes::StickChanged(uint64_t T, std::vector<UiEvent>& Out) {
  // The strongest left stick of any pad decides.
  float Bx = 0, By = 0, Best = 0, Ry = 0;
  for (auto& P : Stick) {
    const float M = P[0][0] * P[0][0] + P[0][1] * P[0][1];
    if (M > Best) {
      Best = M;
      Bx = P[0][0];
      By = P[0][1];
    }
    if (std::fabs(P[1][1]) > std::fabs(Ry))
      Ry = P[1][1];
  }
  RightY = Ry;
  if (std::fabs(Ry) > tuning::StickDeadzone)
    SetMode(Mode::Controller, Out);
  if (std::sqrt(Best) > tuning::StickDeadzone) {
    SetMode(Mode::Controller, Out);
    const Dir D = std::fabs(Bx) >= std::fabs(By) ? (Bx > 0 ? Dir::Right : Dir::Left) : (By > 0 ? Dir::Up : Dir::Down);
    if (!(R.Active && R.Source == 1 && R.D == D))
      StartRepeat(D, 1, T, Out);
  } else if (R.Active && R.Source == 1) {
    R.Active = false;
  }
}

void InputModes::Feed(const Event& E, std::vector<UiEvent>& Out) {
  using T = Event::Type;
  if (E.TimeMs > Now)
    Now = E.TimeMs;
  switch (E.Kind) {
  case T::ButtonDown: {
    SetMode(Mode::Controller, Out);
    switch (E.Btn) {
    case Button::Up: StartRepeat(Dir::Up, 0, Now, Out); break;
    case Button::Down: StartRepeat(Dir::Down, 0, Now, Out); break;
    case Button::Left: StartRepeat(Dir::Left, 0, Now, Out); break;
    case Button::Right: StartRepeat(Dir::Right, 0, Now, Out); break;
    case Button::Count: break;
    default: {
      UiEvent U;
      U.Kind = UiEvent::Type::Button;
      U.Btn = E.Btn;
      Out.push_back(U);
    }
    }
    break;
  }
  case T::ButtonUp: {
    const bool IsDir = E.Btn == Button::Up || E.Btn == Button::Down || E.Btn == Button::Left || E.Btn == Button::Right;
    const Dir D = E.Btn == Button::Up ? Dir::Up : E.Btn == Button::Down ? Dir::Down : E.Btn == Button::Left ? Dir::Left : Dir::Right;
    if (IsDir && R.Active && R.Source == 0 && R.D == D)
      R.Active = false;
    break;
  }
  case T::Stick:
    if (E.Pad < 16 && E.StickIndex < 2) {
      Stick[E.Pad][E.StickIndex][0] = E.X;
      Stick[E.Pad][E.StickIndex][1] = E.Y;
      StickChanged(Now, Out);
    }
    break;
  case T::PadCancel:
    for (auto& Pad : Stick)
      for (auto& Axes : Pad)
        Axes[0] = Axes[1] = 0;
    RightY = 0;
    R.Active = false;
    break;
  case T::Touch: {
    const float X = E.X / S, Y = E.Y / S;
    if (E.Phase == TouchPhase::Down) {
      if (TouchOn)
        break;  // one finger drives the UI
      SetMode(Mode::Touch, Out);
      TouchOn = true;
      TouchId = E.TouchId;
      DownX = LastX = X;
      DownY = LastY = Y;
      DownAt = Now;
      Dragging = LongFired = false;
      InertiaVel = 0;
      SampleCount = 0;
      Samples[SampleCount++] = Sample {Now, Y};
      Emit(Out, UiEvent::Type::PointerDown, X, Y, PointerSource::Touch);
    } else if (TouchOn && E.TouchId == TouchId) {
      if (E.Phase == TouchPhase::Move) {
        if (SampleCount == 8) {
          for (int I = 1; I < 8; ++I)
            Samples[I - 1] = Samples[I];
          --SampleCount;
        }
        Samples[SampleCount++] = Sample {Now, Y};
        if (!Dragging && !LongFired && std::hypot(X - DownX, Y - DownY) >= tuning::TapSlopDu)
          Dragging = true;
        if (Dragging) {
          UiEvent U;
          U.Kind = UiEvent::Type::Drag;
          U.Src = PointerSource::Touch;
          U.X = X;
          U.Y = Y;
          U.DX = X - LastX;
          U.DY = Y - LastY;
          Out.push_back(U);
        }
        LastX = X;
        LastY = Y;
      } else {  // Up
        UiEvent U;
        U.Kind = UiEvent::Type::PointerUp;
        U.Src = PointerSource::Touch;
        U.X = X;
        U.Y = Y;
        U.Tap = !Dragging && !LongFired;
        Out.push_back(U);
        if (Dragging) {
          // Velocity of the last 100 ms of finger movement.
          int First = SampleCount - 1;
          while (First > 0 && Samples[SampleCount - 1].T - Samples[First - 1].T <= 100)
            --First;
          const Sample& A = Samples[First];
          const Sample& B = Samples[SampleCount - 1];
          if (Now - B.T <= 100 && B.T > A.T)
            InertiaVel = (B.Y - A.Y) / float(B.T - A.T);
          if (std::fabs(InertiaVel) * 16.0f < 0.8f)
            InertiaVel = 0;
        }
        TouchOn = false;
        Dragging = false;
      }
    }
    break;
  }
  case T::MouseMove: {
    PxX = E.X;
    PxY = E.Y;
    PointerEver = true;
    if (Cur != Mode::Mouse) {
      if (Now - LastMoveAt > 250)
        MoveAccum = 0;
      MoveAccum += std::hypot(E.DX, E.DY) / S;
      LastMoveAt = Now;
      if (MoveAccum > tuning::MouseModeMotionDu)
        SetMode(Mode::Mouse, Out);
    }
    if (Cur == Mode::Mouse) {
      PointerActive = Now;
      UiEvent U;
      U.Kind = UiEvent::Type::PointerMove;
      U.Src = PointerSource::Mouse;
      U.X = PointerX();
      U.Y = PointerY();
      U.Held = LeftHeld;
      Out.push_back(U);
    }
    break;
  }
  case T::MouseCancel:
  case T::MouseButton: {
    const bool Cancel = E.Kind == T::MouseCancel;
    if (!Cancel && (E.Mouse == ui::MouseButton::Left || E.Mouse == ui::MouseButton::Right)) {
      SetMode(Mode::Mouse, Out);
      PointerEver = true;
      PointerActive = Now;
    }
    if (E.Mouse == ui::MouseButton::Left) {
      LeftHeld = !Cancel && E.Pressed;
      UiEvent U;
      U.Kind = LeftHeld ? UiEvent::Type::PointerDown : UiEvent::Type::PointerUp;
      U.Src = PointerSource::Mouse;
      U.X = PointerX();
      U.Y = PointerY();
      U.Tap = !Cancel && !LeftHeld;
      Out.push_back(U);
    } else if (!Cancel && E.Mouse == ui::MouseButton::Right && E.Pressed) {
      UiEvent U;  // right click = B
      U.Kind = UiEvent::Type::Button;
      U.Btn = Button::B;
      Out.push_back(U);
    }
    break;
  }
  case T::MouseWheel:
    if (E.DY != 0) {
      UiEvent U;
      U.Kind = UiEvent::Type::Scroll;
      U.DY = -E.DY * tuning::WheelDuPerUnit;  // wheel up (positive) moves the content back towards the start
      Out.push_back(U);
      if (Cur == Mode::Mouse)
        PointerActive = Now;
    }
    break;
  case T::KeyDown: break;
  case T::SizeChanged:
    break;
  }
}

void InputModes::Update(uint64_t NowMs, std::vector<UiEvent>& Out) {
  if (NowMs > Now)
    Now = NowMs;
  const uint64_t Dt = LastUpdate == 0 || Now < LastUpdate ? 0 : std::min<uint64_t>(Now - LastUpdate, 50);
  LastUpdate = Now;

  // Directional repeat: 400 ms after the press, then every 90 ms.
  if (R.Active) {
    int Guard = 0;
    while (R.Active && Now >= R.Next && Guard++ < 8) {
      UiEvent E;
      E.Kind = UiEvent::Type::Nav;
      E.D = R.D;
      E.Repeat = true;
      Out.push_back(E);
      R.Next += tuning::RepeatNextMs;
    }
    if (Guard >= 8)
      R.Next = Now + tuning::RepeatNextMs;
  }

  // Long press.
  if (TouchOn && !Dragging && !LongFired && Now - DownAt >= tuning::LongPressMs) {
    LongFired = true;
    UiEvent E;
    E.Kind = UiEvent::Type::LongPress;
    E.Src = PointerSource::Touch;
    E.X = LastX;
    E.Y = LastY;
    Out.push_back(E);
  }

  // Right stick scrolls.
  if (Dt && Cur == Mode::Controller && std::fabs(RightY) > tuning::StickScrollDead) {
    UiEvent E;
    E.Kind = UiEvent::Type::Scroll;
    E.DY = -RightY * tuning::StickScrollSpeed * float(Dt) / 1000.0f;
    Out.push_back(E);
  }

  // Inertia after a touch drag: the content keeps following the finger direction, slowing by 0.92 per 16 ms.
  if (!TouchOn && InertiaVel != 0 && Dt) {
    UiEvent E;
    E.Kind = UiEvent::Type::Scroll;
    E.DY = -InertiaVel * float(Dt);
    E.Inertia = true;
    Out.push_back(E);
    InertiaVel *= std::pow(tuning::InertiaFriction, float(Dt) / 16.0f);
    if (std::fabs(InertiaVel) * 16.0f < 0.2f)
      InertiaVel = 0;
  }
}

} // namespace ui
