// SPDX-License-Identifier: MIT
#pragma once

// Fork-specific guest syscall. RDI points to a tightly packed RGBA8 frame.
#define FEX_NX2_PRESENT_RGBA 0x46580001
#define FEX_NX2_FRAME_WIDTH 320
#define FEX_NX2_FRAME_HEIGHT 180
