#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Checks GuestRootfsSetup.h against Python's readers on the real manifest.

Downloads every manifest package over HTTPS into the cache, verifies its
size and SHA-256, unpacks all of them with the C++ test binary and compares
every data.tar entry (type, mode, size, content hash, link target, owner)
and the dpkg status and .list files.
"""
import argparse, hashlib, io, lzma, os, shutil, subprocess, sys, tarfile, urllib.request
from pathlib import Path

def ar_members(data):
    assert data[:8] == b"!<arch>\n"
    offset, members = 8, {}
    while offset + 60 <= len(data):
        header = data[offset:offset + 60]
        name = header[:16].decode().strip().rstrip("/")
        size = int(header[48:58].decode().strip())
        members[name] = data[offset + 60:offset + 60 + size]
        offset += 60 + size + (size & 1)
    return members

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("cache", type=Path)
    parser.add_argument("binary", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    archives, debs = {}, []
    for line in args.manifest.read_text().splitlines():
        fields = line.split()
        if fields and fields[0] == "archive":
            archives[fields[1]] = fields[2].replace("http://", "https://", 1)
        elif fields and fields[0] == "package":
            _, archive, path, size, sha, name, version = fields
            target = args.cache / Path(path).name
            if not target.is_file() or hashlib.sha256(target.read_bytes()).hexdigest() != sha:
                with urllib.request.urlopen(f"{archives[archive]}/{path}", timeout=120) as response:
                    target.write_bytes(response.read())
            data = target.read_bytes()
            if len(data) != int(size) or hashlib.sha256(data).hexdigest() != sha:
                sys.exit(f"Checksum mismatch for {path}")
            debs.append(target)
    shutil.rmtree(args.output, ignore_errors=True)
    args.output.mkdir(parents=True)
    subprocess.run([str(args.binary), str(args.output), str(args.manifest), *map(str, debs)], check=True)

    owners = {}
    for line in Path(str(args.output) + ".owners").read_text().splitlines():
        kind, path, *rest = line.split(" ")
        if kind == "owner":
            owners[path] = tuple(map(int, rest))
    expected, status_names = {}, []
    for deb in debs:  # later packages overwrite earlier ones, as on the console
        members = ar_members(deb.read_bytes())
        control = tarfile.open(fileobj=io.BytesIO(lzma.decompress(members["control.tar.xz"])))
        fields = control.extractfile("./control").read().decode()
        name = next(l.split(": ", 1)[1] for l in fields.splitlines() if l.startswith("Package: "))
        if "\nMulti-Arch: same" in "\n" + fields:
            name += ":amd64"
        status_names.append(name)
        listed = (args.output / f"var/lib/dpkg/info/{name}.list").read_text().splitlines()
        data = tarfile.open(fileobj=io.BytesIO(lzma.decompress(members["data.tar.xz"])))
        paths = ["/."]
        for member in data.getmembers():
            path = "/" + member.name.lstrip("./").rstrip("/") if member.name not in (".", "./") else "/"
            if path == "/":
                continue
            paths.append(path)
            if member.isdir() and path in expected:
                continue  # dpkg keeps the first package's directory metadata
            content = data.extractfile(member).read() if member.isfile() else None
            expected[path] = (member, content)
        if listed != paths:
            sys.exit(f"{name}.list differs from the data.tar listing")
    errors = 0
    for path, (member, content) in expected.items():
        host = args.output / path.lstrip("/")
        info = host.lstat()
        problem = None
        if member.isdir():
            problem = None if host.is_dir() else "not a directory"
            if not problem and (info.st_mode & 0o7777) != member.mode and not host.is_symlink():
                problem = f"mode {oct(info.st_mode & 0o7777)} != {oct(member.mode)}"
        elif member.issym():
            problem = None if host.is_symlink() and os.readlink(host) == member.linkname else "symlink differs"
        elif member.islnk():
            source = args.output / member.linkname.lstrip("./")
            problem = None if os.path.samefile(host, source) else "hard link differs"
        elif member.isfile():
            if host.read_bytes() != content:
                problem = "content differs"
            elif (info.st_mode & 0o7777) != member.mode:
                problem = f"mode {oct(info.st_mode & 0o7777)} != {oct(member.mode)}"
            elif int(info.st_mtime) != member.mtime:
                problem = "mtime differs"
        if not problem and not member.islnk() and owners.get(path) != (member.uid, member.gid):
            problem = f"owner {owners.get(path)} != {(member.uid, member.gid)}"
        if problem:
            errors += 1
            if errors <= 20:
                print(f"{path}: {problem}")
    status = (args.output / "var/lib/dpkg/status").read_text()
    for name in status_names:
        package = name.split(":")[0]
        if f"Package: {package}\nStatus: install ok unpacked\n" not in status:
            errors += 1
            print(f"status lacks {name}")
    if errors:
        sys.exit(f"{errors} mismatches")
    print(f"rootfs setup check: {len(debs)} packages, {len(expected)} paths match")

if __name__ == "__main__":
    main()
