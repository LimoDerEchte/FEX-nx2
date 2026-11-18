// SPDX-License-Identifier: MIT
#pragma once

// Fork-specific guest syscall. RDI points to a tightly packed RGBA8 frame.
#define FEX_NX2_PRESENT_RGBA 0x46580001
#define FEX_NX2_FRAME_WIDTH 320
#define FEX_NX2_FRAME_HEIGHT 180

// Poll returns this controller bitmask. Delay accepts milliseconds in RDI.
#define FEX_NX2_POLL_INPUT 0x46580002
#define FEX_NX2_TICKS_MS 0x46580003
#define FEX_NX2_DELAY_MS 0x46580004
#define FEX_NX2_UP 1
#define FEX_NX2_DOWN 2
#define FEX_NX2_LEFT 4
#define FEX_NX2_RIGHT 8
#define FEX_NX2_QUIT 16
