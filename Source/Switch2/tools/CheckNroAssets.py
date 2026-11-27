#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Checks that every file under ui/assets (fonts, icons, licenses.json, license texts, the RootFS index) is embedded verbatim in the NRO,
# so the NRO is the only file the user copies. Usage: CheckNroAssets.py <nro> <ui/assets>
import os
import sys

nro = open(sys.argv[1], "rb").read()
root = sys.argv[2]
missing = []
count = 0
for folder, _, names in os.walk(root):
    for name in sorted(names):
        if not name.endswith((".ttf", ".svg", ".json", ".txt")):
            continue
        if name == "SOURCES.txt":
            continue  # provenance notes, not embedded
        path = os.path.join(folder, name)
        data = open(path, "rb").read()
        count += 1
        if data not in nro:
            missing.append(os.path.relpath(path, root))
if missing:
    print("check-nro-assets: NOT embedded in the NRO:", ", ".join(missing))
    sys.exit(1)
print("check-nro-assets: ok (%d asset files found byte for byte in %s, %d bytes)" % (count, os.path.basename(sys.argv[1]), len(nro)))
