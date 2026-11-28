// SPDX-License-Identifier: MIT
// One event stream for buttons, sticks, touch, mouse, wheel, keyboard and output size changes. No libnx types here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ui {

enum class Button : uint8_t {
  A, B, X, Y, L, R, ZL, ZR, Plus, Minus, Up, Down, Left, Right, StickL, StickR, Count
};
const char* ButtonName(Button B);

enum class TouchPhase : uint8_t { Down, Move, Up };

enum class MouseButton : uint8_t { Left, Right, Middle, Back, Forward, Count };
const char* MouseButtonName(MouseButton B);

enum class Key : uint8_t {
  Text, Enter, Backspace, Tab, Escape, Up, Down, Right, Left, Home, End,
  Insert, Delete, PageUp, PageDown, F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
  Keypad0, Keypad1, Keypad2, Keypad3, Keypad4, Keypad5, Keypad6, Keypad7, Keypad8, Keypad9,
  KeypadDot, KeypadDivide, KeypadMultiply, KeypadSubtract, KeypadAdd, KeypadEnter, KeypadEquals
};

enum KeyModifier : uint8_t { Shift = 1, Alt = 2, Ctrl = 4 };

struct Event {
  enum class Type : uint8_t {
    ButtonDown,  // Pad, Btn
    ButtonUp,    // Pad, Btn
    Stick,       // Pad, StickIndex (0 left, 1 right), X and Y in -1..1 (up is +Y)
    Touch,       // TouchId, Phase, X and Y in output pixels
    MouseMove,   // X and Y: pointer position in output pixels (clamped to the output), DX and DY: movement in output pixels
    MouseButton, // Mouse, Pressed
    MouseWheel,  // DX and DY: wheel delta as the SDK reports it (positive Y = up; Joy-Con: the stick)
    KeyDown,    // KeyboardKey, Modifiers, Codepoint (for Key::Text)
    SizeChanged, // W and H: the new output size in pixels
    MouseCancel, // Mouse: clear a held button without completing a click
    PadCancel,   // Clear held sticks and controller navigation repeats.
  };
  Type Kind = Type::ButtonDown;
  uint64_t TimeMs = 0;
  uint32_t Pad = 0;        // controller id: 0..7 = player 1..8, 8 = handheld
  Button Btn = Button::A;
  uint8_t StickIndex = 0;
  uint32_t TouchId = 0;
  TouchPhase Phase = TouchPhase::Down;
  ui::MouseButton Mouse = ui::MouseButton::Left;
  bool Pressed = false;
  float X = 0, Y = 0, DX = 0, DY = 0;
  int W = 0, H = 0;
  Key KeyboardKey = Key::Text;
  uint8_t Modifiers = 0;
  char32_t Codepoint = 0;
};

class Input {
public:
  virtual ~Input() = default;
  // Appends the events since the last call, oldest first. Never blocks.
  virtual void Poll(std::vector<Event>& Out) = 0;
  // false when the backend has no mouse (or wheel); the probe reports it instead of inventing values.
  virtual bool HasMouse() const { return false; }
  virtual bool HasWheel() const { return false; }
  // Pointer speed: output pixels (at 1280 px output width) per raw sensor count. Console Joy-Con / USB mouse only.
  virtual float PointerGain() const { return 1.0f; }
  virtual void SetPointerGain(float) {}
  // One line for the probe's Mouse readout (Nmouse init result, active device, raw sums). Empty when the backend has none.
  virtual std::string MouseStatus() const { return {}; }
};

} // namespace ui
