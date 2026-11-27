// SPDX-License-Identifier: MIT
// USB HID keyboard usage (page 7) -> UI key event, for a US layout. No libnx types, so the host tests drive the same code the
// Switch 2 keyboard path uses (Switch2Platform.cpp reads the usages from the SDK and calls this).
#pragma once

#include "platform/Input.h"

namespace ui {

namespace hid_usage {
inline constexpr int A = 4, Z = 29, D1 = 30, D0 = 39, Return = 40, Escape = 41, Backspace = 42, Tab = 43, Space = 44, Slash = 56, F1 = 58, F12 = 69,
                     Insert = 73, Home = 74, PageUp = 75, Delete = 76, End = 77, PageDown = 78, Right = 79, Left = 80, Down = 81, Up = 82,
                     KeypadDivide = 84, KeypadDot = 99, Backslash = 100, KeypadEquals = 103;
}

// Fills E (Kind, KeyboardKey, Modifiers, Codepoint) for one key press. false: the usage has no mapping.
inline bool MapHidKeyboard(int Usage, bool ShiftHeld, bool AltHeld, bool CtrlHeld, bool CapsLock, Event& E) {
  using namespace hid_usage;
  E.Kind = Event::Type::KeyDown;
  E.Modifiers = uint8_t((ShiftHeld ? Shift : 0) | (AltHeld ? Alt : 0) | (CtrlHeld ? Ctrl : 0));
  const bool Upper = ShiftHeld != CapsLock;
  if (Usage >= A && Usage <= Z) {
    E.Codepoint = (Upper ? U'A' : U'a') + (Usage - A);
  } else if (Usage >= D1 && Usage <= D0) {
    E.Codepoint = (ShiftHeld ? U"!@#$%^&*()" : U"1234567890")[Usage - D1];
  } else if (Usage >= Return && Usage <= Tab) {
    constexpr Key Keys[] = {Key::Enter, Key::Escape, Key::Backspace, Key::Tab};
    E.KeyboardKey = Keys[Usage - Return];
  } else if (Usage >= Space && Usage <= Slash) {
    constexpr auto Plain = U" -=[]\\#;'`,./";
    constexpr auto ShiftedText = U" _+{}|~:\"~<>?";
    E.Codepoint = (ShiftHeld ? ShiftedText : Plain)[Usage - Space];
  } else if (Usage >= F1 && Usage <= F12) {
    E.KeyboardKey = Key(int(Key::F1) + Usage - F1);
  } else if (Usage >= Insert && Usage <= Up) {
    constexpr Key Keys[] = {Key::Insert, Key::Home, Key::PageUp, Key::Delete, Key::End,
                            Key::PageDown, Key::Right, Key::Left, Key::Down, Key::Up};
    E.KeyboardKey = Keys[Usage - Insert];
  } else if (Usage >= KeypadDivide && Usage <= KeypadDot) {
    constexpr Key Keys[] = {Key::KeypadDivide, Key::KeypadMultiply, Key::KeypadSubtract, Key::KeypadAdd, Key::KeypadEnter,
                            Key::Keypad1, Key::Keypad2, Key::Keypad3, Key::Keypad4, Key::Keypad5,
                            Key::Keypad6, Key::Keypad7, Key::Keypad8, Key::Keypad9, Key::Keypad0, Key::KeypadDot};
    E.KeyboardKey = Keys[Usage - KeypadDivide];
  } else if (Usage == KeypadEquals) {
    E.KeyboardKey = Key::KeypadEquals;
  } else if (Usage == Backslash) {
    E.Codepoint = ShiftHeld ? U'|' : U'\\';
  } else {
    return false;
  }
  return true;
}

} // namespace ui
