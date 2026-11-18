# SPDX-License-Identifier: MIT
"""Stage matching x86-64 glibc files from the development machine."""

from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys


def command(*args):
    return subprocess.check_output(args, text=True)


output, true_binary, probe_binary, compiler, library_dir = sys.argv[1:]
library_root = Path(library_dir) if library_dir else None
root = Path(output)
for binary, name in [(true_binary, "true.elf"), (probe_binary, "glibc-probe.elf")]:
    headers = command("readelf", "-hWl", binary)
    match = re.search(r"Requesting program interpreter: (/[^\]]+)", headers)
    if "Advanced Micro Devices X86-64" not in headers or not match:
        raise SystemExit(f"Expected a dynamic x86-64 Linux ELF: {binary}")
    guest_interpreter = PurePosixPath(match[1])
    interpreter = library_root / guest_interpreter.name if library_root else Path(str(guest_interpreter))
    target = root / "rootfs" / guest_interpreter.relative_to("/")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(interpreter, target)

    destination = root / "games" / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(binary, destination)

for name in ("libc.so.6", "libm.so.6"):
    library = library_root / name if library_root else Path(command(compiler, "-print-file-name=" + name).strip()).resolve()
    if not library.is_file():
        raise SystemExit(f"No matching development libc library: {name}")
    for folder in ("lib", "usr/lib", "lib/x86_64-linux-gnu", "usr/lib/x86_64-linux-gnu"):
        target = root / "rootfs" / folder / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(library, target)
