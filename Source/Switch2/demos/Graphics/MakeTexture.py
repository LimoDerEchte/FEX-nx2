# SPDX-License-Identifier: MIT
"""Generate a 32 × 32 RGBA8 sprite for the guest texture fixture."""

from pathlib import Path
import sys


def pixel(x, y):
    distance = abs(2 * x - 31) + abs(2 * y - 31)
    if distance > 29:
        return 0, 0, 0, 0
    if distance > 23:
        return 255, 203, 87, 255
    if abs(x - y) < 3:
        return 255, 244, 187, 255
    return 55 + x * 4, 170 + y * 2, 253, 255


Path(sys.argv[1]).write_bytes(bytes(
    channel
    for y in range(32)
    for x in range(32)
    for channel in pixel(x, y)
))
