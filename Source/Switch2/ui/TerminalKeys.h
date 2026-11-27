// SPDX-License-Identifier: MIT
#pragma once

#include "TerminalGrid.h"
#include "platform/Input.h"

namespace ui {

std::string EncodeTerminalKey(const TerminalGrid& Grid, Key K, uint8_t Mods = 0, char32_t Text = 0);
std::string EncodeTerminalPaste(const TerminalGrid& Grid, std::string_view Text);

} // namespace ui
