// SPDX-License-Identifier: MIT
// Turns the raw platform event stream into UI events in design units: input mode (controller, mouse, touch), directional
// repeat (400 ms, then 90 ms; stick deadzone 0.5), pointer visibility (24 du arrow, hidden after 3 s idle), touch gestures
// (tap < 12 du, long press 500 ms, drag, inertia 0.92 per 16 ms) and wheel/right-stick scroll. All times come from event
// times and Update(now), so tests drive it with a fake clock. No Vulkan or libnx types.
#pragma once

#include <cstdint>
#include <vector>

#include "input/Dir.h"
#include "platform/Input.h"

namespace ui {

enum class Mode : uint8_t { Controller, Mouse, Touch };
const char* ModeName(Mode M);
enum class PointerSource : uint8_t { Mouse, Touch };

namespace tuning {
inline constexpr uint32_t RepeatFirstMs = 400, RepeatNextMs = 90;
inline constexpr float StickDeadzone = 0.5f;      // direction and mode
inline constexpr float StickScrollDead = 0.2f;    // right stick scroll
inline constexpr float StickScrollSpeed = 900.0f; // du per second at full deflection
inline constexpr float MouseModeMotionDu = 2.0f;
inline constexpr float PointerSizeDu = 24.0f;
inline constexpr uint32_t PointerHideMs = 3000;
inline constexpr float TapSlopDu = 12.0f;
inline constexpr uint32_t LongPressMs = 500;
inline constexpr float InertiaFriction = 0.92f;   // per 16 ms
inline constexpr float WheelDuPerUnit = 2.4f;     // a raw wheel unit; console: 12 scrolled about 5x too far
inline constexpr float MinHitDu = 44.0f;
} // namespace tuning

struct UiEvent {
  enum class Type : uint8_t {
    Nav,          // D, Repeat
    Button,       // Btn: A B X Y L R ZL ZR Plus Minus (controller buttons; Right click is reported as B)
    PointerMove,  // Src, X, Y (du), Held (left button down)
    PointerDown,  // Src, X, Y
    PointerUp,    // Src, X, Y, Tap (a valid tap or click: not dragged, not a long press)
    Drag,         // Src (touch), X, Y, DX, DY (du since the previous Drag/Down), after the tap slop is exceeded
    Scroll,       // DY: change of the scroll offset in du (positive = towards the end of the content), Inertia
    LongPress,    // Src (touch), X, Y
    ModeChanged,  // NewMode
  };
  Type Kind = Type::Nav;
  Dir D = Dir::Up;
  Button Btn = Button::A;
  PointerSource Src = PointerSource::Mouse;
  float X = 0, Y = 0, DX = 0, DY = 0;
  bool Held = false, Tap = false, Repeat = false, Inertia = false;
  Mode NewMode = Mode::Controller;
};

class InputModes {
public:
  // Output pixels per du (Display::Scale()); call again after a size change. The pointer keeps its design position.
  void SetScale(float S);

  void Feed(const Event& E, std::vector<UiEvent>& Out);
  // Time effects: repeats, long press, inertia, stick scroll, pointer hide.
  void Update(uint64_t NowMs, std::vector<UiEvent>& Out);

  Mode GetMode() const { return Cur; }
  bool PointerVisible() const;
  float PointerX() const { return PxX / S; }  // du
  float PointerY() const { return PxY / S; }
  bool LeftDown() const { return LeftHeld; }

private:
  void SetMode(Mode M, std::vector<UiEvent>& Out);
  void StartRepeat(Dir D, int Source, uint64_t T, std::vector<UiEvent>& Out);
  void StickChanged(uint64_t T, std::vector<UiEvent>& Out);
  void Emit(std::vector<UiEvent>& Out, UiEvent::Type K, float X, float Y, PointerSource Src) const;

  float S = 1.0f;
  Mode Cur = Mode::Controller;
  uint64_t Now = 0;
  uint64_t LastUpdate = 0;

  // Repeat.
  struct Rep {
    bool Active = false;
    Dir D = Dir::Up;
    int Source = 0;  // 0 d-pad, 1 stick
    uint64_t Next = 0;
  } R;
  float Stick[16][2][2] {};  // pad, stick, x/y
  float RightY = 0;

  // Pointer (output pixels).
  float PxX = 640, PxY = 360;
  uint64_t PointerActive = 0;
  bool PointerEver = false;
  float MoveAccum = 0;
  uint64_t LastMoveAt = 0;
  bool LeftHeld = false;

  // Touch.
  bool TouchOn = false;
  uint32_t TouchId = 0;
  float DownX = 0, DownY = 0, LastX = 0, LastY = 0;  // du
  uint64_t DownAt = 0;
  bool Dragging = false, LongFired = false;
  struct Sample {
    uint64_t T;
    float Y;
  } Samples[8] {};
  int SampleCount = 0;
  float InertiaVel = 0;  // du per ms (finger direction)
};

} // namespace ui
