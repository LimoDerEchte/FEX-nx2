#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Compares the theme dump of build/ui-text-test (--dump-theme) with the tables of the theming spec
# (docs/ui/fex-switch2-theming-spec.md): every launcher token, accent preset and shell color, dark and light.
# usage: CheckTheme.py DUMP.txt SPEC.md
import re
import sys


def parse_spec(path):
    exp = {}  # (mode, token) -> (rrggbb, alpha 0..255)
    section = None
    for line in open(path, encoding="utf-8"):
        if line.startswith("## "):
            section = line[3:].strip()
            continue
        if section is None or not line.startswith("|") or "---" in line:
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) >= 3 and re.match(r"(Cyan|Red|Violet|Blue)\b", cells[0]):  # the "Accent presets" table
            name = re.match(r"(Cyan|Red|Violet|Blue)", cells[0]).group(1).lower()
            for mode, cell in (("dark", cells[1]), ("light", cells[2])):
                exp[(mode, "preset_" + name)] = (re.search(r"#([0-9A-Fa-f]{6})", cell).group(1).upper(), 255)
        elif section == "Launcher tokens" and len(cells) >= 3:
            names = re.findall(r"`(\w+)`", cells[0])
            if not names:
                continue
            dark = re.findall(r"#([0-9A-Fa-f]{6})", cells[1])
            light = re.findall(r"#([0-9A-Fa-f]{6})", cells[2])
            if len(names) != len(dark) or len(names) != len(light):
                sys.exit("CheckTheme: cannot parse row: " + line)
            for mode, vals, cell in (("dark", dark, cells[1]), ("light", light, cells[2])):
                for name, v in zip(names, vals):
                    alpha = 255
                    m = re.search(r"@\s*(\d+)%", cell)
                    if m:
                        alpha = round(int(m.group(1)) * 255 / 100)
                    exp[(mode, name)] = (v.upper(), alpha)
        elif section == "Shell palette" and len(cells) >= 3:
            m = re.match(r"`(kde_\w+)`", cells[0])
            name = m.group(1) if m else None
            if not name:
                m = re.match(r"ANSI (green|blue|yellow|red)", cells[0])
                name = "ansi_" + m.group(1) if m else None
            if name:
                for mode, cell in (("dark", cells[1]), ("light", cells[2])):
                    exp[(mode, name)] = (re.search(r"#([0-9A-Fa-f]{6})", cell).group(1).upper(), 255)
    return exp


def parse_dump(path):
    got = {}
    for line in open(path):
        f = line.split()
        if len(f) == 4:
            got[(f[0], f[1])] = (f[2].upper(), int(f[3]))
    return got


def main():
    got, exp = parse_dump(sys.argv[1]), parse_spec(sys.argv[2])
    bad = 0
    for key, val in sorted(exp.items()):
        if got.get(key) != val:
            print("CheckTheme: %s %s: spec %s, theme %s" % (key[0], key[1], val, got.get(key)))
            bad += 1
    for key in sorted(set(got) - set(exp)):
        print("CheckTheme: %s %s is in the theme but not in the spec tables" % key)
        bad += 1
    if bad:
        sys.exit(1)
    print("CheckTheme: ok (%d values match the spec tables)" % len(exp))


main()
