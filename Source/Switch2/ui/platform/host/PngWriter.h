// SPDX-License-Identifier: MIT
// Host only: 8-bit RGBA PNG writer (zlib), used by the headless screenshot tools.
#pragma once

#include <cstdint>
#include <vector>

namespace ui {

// Tightly packed RGBA8, filter type 0 on every row.
bool WritePng(const char* Path, const std::vector<uint8_t>& Rgba, int W, int H);

} // namespace ui
