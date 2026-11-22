#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Download and unpack the Debian 12 amd64 packages used by the guest tests.

Builds a sysroot suitable for DEBIAN_ROOT. Package indexes come from the
bookworm, bookworm-updates and bookworm-security archives; each .deb is
checked against the index SHA-256 before extraction. Maintainer scripts
are never run.
"""

import argparse
import hashlib
import io
import lzma
from pathlib import Path
import tarfile
import urllib.request

ARCHIVES = (
    ("https://deb.debian.org/debian", "bookworm"),
    ("https://deb.debian.org/debian", "bookworm-updates"),
    ("https://security.debian.org/debian-security", "bookworm-security"),
)
PACKAGES = (
    "libc6", "libc6-dev", "libgcc-s1", "bash", "coreutils", "libtinfo6", "ncurses-base",
    "libacl1", "libattr1", "libselinux1", "libpcre2-8-0", "libgmp10", "libcap2",
)


def fetch(url):
    with urllib.request.urlopen(url, timeout=120) as response:
        return response.read()


def _order(char):
    if char == "~":
        return -1
    if char.isalpha():
        return ord(char)
    return ord(char) + 256 if char else 0


def _compare_part(left, right):
    while left or right:
        prefix_left = len(left) - len(left.lstrip("~+-.:abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"))
        prefix_right = len(right) - len(right.lstrip("~+-.:abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"))
        a, left = left[:prefix_left], left[prefix_left:]
        b, right = right[:prefix_right], right[prefix_right:]
        for index in range(max(len(a), len(b))):
            difference = _order(a[index] if index < len(a) else "") - _order(b[index] if index < len(b) else "")
            if difference:
                return difference
        digits_left = len(left) - len(left.lstrip("0123456789"))
        digits_right = len(right) - len(right.lstrip("0123456789"))
        number_left, left = int(left[:digits_left] or 0), left[digits_left:]
        number_right, right = int(right[:digits_right] or 0), right[digits_right:]
        if number_left != number_right:
            return number_left - number_right
    return 0


def compare_versions(left, right):
    """Debian policy version ordering (epoch, upstream, revision)."""
    def split(version):
        epoch, _, rest = version.rpartition(":") if ":" in version else ("0", "", version)
        upstream, _, revision = rest.rpartition("-") if "-" in rest else (rest, "", "0")
        return int(epoch or 0), upstream, revision
    a, b = split(left), split(right)
    return (a[0] - b[0]) or _compare_part(a[1], b[1]) or _compare_part(a[2], b[2])


def index(cache):
    found = {}
    for base, suite in ARCHIVES:
        name = cache / f"{suite}.Packages.xz"
        if not name.is_file():
            name.write_bytes(fetch(f"{base}/dists/{suite}/main/binary-amd64/Packages.xz"))
        for stanza in lzma.decompress(name.read_bytes()).decode().split("\n\n"):
            fields = dict(line.split(": ", 1) for line in stanza.splitlines() if ": " in line and not line.startswith(" "))
            package = fields.get("Package")
            if package in PACKAGES:
                current = found.get(package)
                if not current or compare_versions(fields["Version"], current[3]) > 0:
                    found[package] = (base, fields["Filename"], fields["SHA256"], fields["Version"])
    missing = set(PACKAGES) - set(found)
    if missing:
        raise SystemExit(f"Packages not found in the bookworm indexes: {sorted(missing)}")
    return found


def unpack(deb, root):
    # A .deb is an ar archive holding control and data tarballs.
    if not deb.startswith(b"!<arch>\n"):
        raise SystemExit("Not a Debian package")
    offset = 8
    while offset + 60 <= len(deb):
        name = deb[offset:offset + 16].decode().strip().rstrip("/")
        size = int(deb[offset + 48:offset + 58].decode().strip())
        body = deb[offset + 60:offset + 60 + size]
        if name.startswith("data.tar"):
            with tarfile.open(fileobj=io.BytesIO(body)) as archive:
                archive.extractall(root, filter="tar")
            return
        offset += 60 + size + (size & 1)
    raise SystemExit("Package has no data archive")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="Sysroot directory to create")
    parser.add_argument("--cache", type=Path, help="Directory for indexes and packages")
    args = parser.parse_args()
    cache = (args.cache or args.output.with_name(args.output.name + "-cache")).resolve()
    cache.mkdir(parents=True, exist_ok=True)
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    for package, (base, filename, digest, _) in sorted(index(cache).items()):
        local = cache / Path(filename).name
        if not local.is_file() or hashlib.sha256(local.read_bytes()).hexdigest() != digest:
            local.write_bytes(fetch(f"{base}/{filename}"))
        deb = local.read_bytes()
        if hashlib.sha256(deb).hexdigest() != digest:
            raise SystemExit(f"Checksum mismatch for {filename}")
        unpack(deb, root)
        print(f"Unpacked {Path(filename).name}")


if __name__ == "__main__":
    main()
