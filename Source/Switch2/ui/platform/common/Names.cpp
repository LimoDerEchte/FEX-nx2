// SPDX-License-Identifier: MIT
#include "platform/Input.h"

namespace ui {

const char* ButtonName(Button B) {
  static const char* const Names[] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Plus", "Minus", "Up", "Down", "Left", "Right", "StickL", "StickR"};
  return size_t(B) < size_t(Button::Count) ? Names[size_t(B)] : "?";
}

const char* MouseButtonName(MouseButton B) {
  static const char* const Names[] = {"Left", "Right", "Middle", "Back", "Forward"};
  return size_t(B) < size_t(MouseButton::Count) ? Names[size_t(B)] : "?";
}

} // namespace ui
