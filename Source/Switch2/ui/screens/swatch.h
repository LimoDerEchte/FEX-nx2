// SPDX-License-Identifier: MIT
#pragma once

#include "DrawContext.h"

namespace ui {

// Test screen (1280x720 du): every theme token with its hex, every text style, every icon, the accent presets. Used for
// the dark and light screenshots and for checking the mockup colors.
void DrawSwatch(const DrawContext& C);

} // namespace ui
