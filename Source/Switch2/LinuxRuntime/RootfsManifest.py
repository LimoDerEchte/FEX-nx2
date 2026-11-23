#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate the Debian 12 minbase download manifest shipped to the console.

The console downloads over plain HTTP, so it verifies each .deb against the
size and SHA-256 recorded here, which are read from HTTPS-fetched indexes.
"""

import argparse
import datetime
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from FetchDebian import compare_versions, stanzas  # noqa: E402

ARCHIVE_IDS = {
    "https://deb.debian.org/debian": ("debian", "http://deb.debian.org/debian"),
    "https://security.debian.org/debian-security": ("security", "http://security.debian.org/debian-security"),
}


def names(field):
    """Yield one list of alternative package names per dependency."""
    for dependency in (field or "").split(","):
        alternatives = [re.split(r"[ (:\[<]", item.strip(), maxsplit=1)[0] for item in dependency.split("|")]
        alternatives = [name for name in alternatives if name]
        if alternatives:
            yield alternatives


def load(cache):
    packages = {}
    for base, _, fields in stanzas(cache):
        if fields.get("Architecture") not in ("amd64", "all"):
            continue
        name = fields["Package"]
        current = packages.get(name)
        if not current or compare_versions(fields["Version"], current["Version"]) > 0:
            packages[name] = dict(fields, _base=base)
    providers = {}
    for name, fields in packages.items():
        for provided in names(fields.get("Provides")):
            providers.setdefault(provided[0], set()).add(name)
    return packages, providers


def select(packages, providers):
    roots = sorted(name for name, f in packages.items()
                   if f.get("Priority") == "required" or f.get("Essential") == "yes")
    if "apt" not in roots:
        roots.append("apt")
    chosen, edges, queue = set(), {}, list(roots)

    def resolve(alternatives):
        # debootstrap installs usr-is-merged; usrmerge would convert the
        # rootfs layout from a maintainer script and pulls in perl.
        if "usr-is-merged" in alternatives and "usr-is-merged" in packages:
            return "usr-is-merged"
        for name in alternatives:
            if name in chosen:
                return name
            for provider in sorted(providers.get(name, ())):
                if provider in chosen:
                    return provider
        for name in alternatives:
            if name in packages:
                return name
            if providers.get(name):
                return sorted(providers[name])[0]
        raise SystemExit(f"Unresolvable dependency: {' | '.join(alternatives)}")

    while queue:
        name = queue.pop(0)
        if name in chosen:
            continue
        if name not in packages:
            raise SystemExit(f"Package not found: {name}")
        chosen.add(name)
        edges[name] = set()
        fields = packages[name]
        for key in ("Pre-Depends", "Depends"):
            for alternatives in names(fields.get(key)):
                target = resolve(alternatives)
                if target != name:
                    edges[name].add(target)
                    queue.append(target)
    return chosen, edges


def order(chosen, edges):
    """Dependencies first; cycles are broken by taking the smallest name."""
    remaining = {name: {d for d in edges[name] if d in chosen} for name in chosen}
    result = []
    while remaining:
        ready = sorted(name for name, deps in remaining.items() if not deps)
        pick = ready[0] if ready else min(remaining)
        result.append(pick)
        del remaining[pick]
        for deps in remaining.values():
            deps.discard(pick)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--cache", type=Path, default=Path("build/rootfs-manifest-cache"))
    args = parser.parse_args()
    cache = args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    packages, providers = load(cache)
    chosen, edges = select(packages, providers)
    lines = [
        "# FEX Switch 2 Debian rootfs manifest",
        "suite bookworm",
        "snapshot " + datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ"),
        "archive debian http://deb.debian.org/debian",
        "archive security http://security.debian.org/debian-security",
    ]
    total = 0
    for name in order(chosen, edges):
        fields = packages[name]
        total += int(fields["Size"])
        lines.append(f"package {ARCHIVE_IDS[fields['_base']][0]} {fields['Filename']} {fields['Size']} "
                     f"{fields['SHA256']} {name} {fields['Version']}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(chosen)} packages, {total / 1048576:.1f} MiB")


if __name__ == "__main__":
    main()
