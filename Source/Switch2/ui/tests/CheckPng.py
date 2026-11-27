#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Checks pixels of a PNG written by the host UI probe (8-bit RGBA, filter 0 per row).
# usage: CheckPng.py FILE.png WxH [eq:X,Y,R,G,B] [ne:X,Y,R,G,B] [any:X0,Y0,X1,Y1,R,G,B] ...
#   eq   pixel equals the color            ne   pixel differs from the color
#   any  some pixel in [X0,X1) x [Y0,Y1) differs from the color (drawn content, such as text)
import struct
import sys
import zlib


def load(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, w = 8, b"", None
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            assert (depth, ctype) == (8, 6), "expected 8-bit RGBA"
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * 4 + 1
    rows = []
    for y in range(h):
        assert raw[y * stride] == 0, "filter 0 expected"
        rows.append(raw[y * stride + 1:(y + 1) * stride])
    return w, h, rows


def px(rows, x, y):
    return tuple(rows[y][x * 4:x * 4 + 3])


def main():
    path, size = sys.argv[1], sys.argv[2]
    w, h, rows = load(path)
    fails = 0
    if size != "%dx%d" % (w, h):
        print("FAIL size: expected %s got %dx%d" % (size, w, h))
        fails += 1
    n = 0
    for arg in sys.argv[3:]:
        kind, vals = arg.split(":")
        v = [int(t) for t in vals.split(",")]
        n += 1
        if kind in ("eq", "ne"):
            got = px(rows, v[0], v[1])
            want = tuple(v[2:5])
            ok = (got == want) == (kind == "eq")
            if not ok:
                print("FAIL %s: pixel (%d,%d) is %s, want %s %s" % (arg, v[0], v[1], got, "==" if kind == "eq" else "!=", want))
                fails += 1
        elif kind == "any":
            want = tuple(v[4:7])
            found = any(px(rows, x, y) != want for y in range(v[1], v[3]) for x in range(v[0], v[2]))
            if not found:
                print("FAIL %s: region has only %s" % (arg, want))
                fails += 1
        else:
            print("bad check", arg)
            return 2
    print("%s: %d checks, %d failures" % (path, n, fails))
    return 1 if fails else 0


sys.exit(main())
