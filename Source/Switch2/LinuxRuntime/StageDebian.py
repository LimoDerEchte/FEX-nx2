#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Stage an SD-card smoke-test tree from unpacked Debian 12 amd64 packages."""

import argparse
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess


def elf_info(path, *options):
    return subprocess.check_output(["readelf", *options, path], text=True)


def stage(source, output, probe, glibc_probe, process_probe):
    root = output / "fex/rootfs"
    library_dirs = ["lib/x86_64-linux-gnu", "usr/lib/x86_64-linux-gnu"]
    installed = set()

    def install(file, path):
        if path in installed:
            return
        if not file.is_file():
            raise SystemExit(f"Missing Debian file: {file}")
        installed.add(path)
        destination = path
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(file, destination)
        metadata = elf_info(file, "-hWld")
        if "Advanced Micro Devices X86-64" not in metadata:
            raise SystemExit(f"Expected an amd64 ELF: {file}")
        interpreter = re.search(r"Requesting program interpreter: (/[^\]]+)", metadata)
        if interpreter:
            name = PurePosixPath(interpreter[1]).name
            install(source / library_dirs[0] / name, root / interpreter[1].lstrip("/"))
        for library in re.findall(r"Shared library: \[([^\]]+)\]", metadata):
            for directory in library_dirs:
                library_file = source / directory / library
                if library_file.is_file():
                    install(library_file, root / directory / library)
                    break
            else:
                raise SystemExit(f"Missing Debian dependency {library} for {file}")

    for executable in ("ls", "bash"):
        install(source / "bin" / executable, root / "bin" / executable)
    install(source / "bin/true", root / "bin/true")
    install(probe, output / "fex/games/thread-ipc.elf")
    install(glibc_probe, output / "fex/games/glibc-probe.elf")
    install(process_probe, output / "fex/games/process-probe.elf")
    install(source / library_dirs[0] / "libm.so.6", root / library_dirs[0] / "libm.so.6")

    ansi = Path("lib/terminfo/a/ansi")
    if not (source / ansi).is_file():
        raise SystemExit("Missing Debian ANSI terminfo. Extract the ncurses-base package.")
    (root / ansi).parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source / ansi, root / ansi)

    (root / "bin/ls.args").write_text("-lna\n/bin\n", encoding="utf-8")
    (root / "bin/bash.args").write_text(
        "--noprofile\n--norc\n-c\n"
        "cd /bin && pwd && [[ -x /bin/ls ]] && printf '%s\\n' 'Debian Bash builtins passed' && exit 42; exit 1\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Directory containing the unpacked Debian packages")
    parser.add_argument("output", type=Path, help="Directory in which to create fex/")
    parser.add_argument("probe", type=Path)
    parser.add_argument("glibc_probe", type=Path)
    parser.add_argument("process_probe", type=Path)
    args = parser.parse_args()
    stage(args.source.resolve(), args.output.resolve(), args.probe.resolve(), args.glibc_probe.resolve(), args.process_probe.resolve())
