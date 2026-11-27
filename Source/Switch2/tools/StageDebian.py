#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Stage an SD-card smoke-test tree from unpacked Debian 12 amd64 packages."""

import argparse
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess

# Guest-side shell tools live in tests/guest next to the suites.
GUEST_TESTS = Path(__file__).resolve().parent.parent / "tests" / "guest"


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
    # Job-control tools for milestone 2. The sysroot has no dash, so Bash
    # (which runs in POSIX mode as sh) provides /bin/sh.
    install(source / "bin/sleep", root / "bin/sleep")
    install(source / "usr/bin/timeout", root / "usr/bin/timeout")
    install(source / "bin/bash", root / "bin/sh")
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


HOME_SCRATCH = "/home/fex"   # the default user's home folder in the RootFS
ROOT_SCRATCH = "/root"       # root's home folder
PERSIST_DIR = "/home/fex/.fex-persistence-01231"


def stage_games(source, output, programs):
    """Stage the test programs into output/fex/games.

    `programs` maps a role (thread_ipc, filesystem_probe, ...) to a built ELF.
    The RootFS provides every interpreter, library and tool: only Debian's bash
    binary is copied, as the ELF the wrapper programs run, without libraries.
    """
    games = output / "fex/games"
    games.mkdir(parents=True, exist_ok=True)

    def elf(role, name, args=None, user=None):
        file = programs.get(role)
        if not file:
            return
        shutil.copy(file, games / name)
        sidecars(name, args, user)

    def sidecars(name, args=None, user=None):
        if args:
            (games / (name + ".args")).write_text("".join(a + "\n" for a in args), encoding="utf-8")
        if user:
            (games / (name + ".user")).write_text(user + "\n", encoding="utf-8")

    def script(name, file):
        shutil.copyfile(GUEST_TESTS / file, games / name)

    def bash(name, args):
        file = source / "bin/bash"
        if not file.is_file():
            raise SystemExit(f"Missing Debian file: {file}")
        shutil.copy(file, games / name)
        sidecars(name, args)

    for role, name in (("thread_ipc", "thread-ipc.elf"), ("glibc_probe", "glibc-probe.elf"),
                       ("process_probe", "process-probe.elf")):
        elf(role, name)

    if programs.get("filesystem_probe"):
        elf("filesystem_probe", "filesystem-probe.elf", ["-d", HOME_SCRATCH])
        elf("filesystem_probe", "filesystem-persist-write.elf", ["--persist-write", PERSIST_DIR])
        elf("filesystem_probe", "filesystem-persist-read.elf", ["--persist-read", PERSIST_DIR])
        # The runtime interrupts the journaled move in /.fex-recovery-test, a
        # root-owned folder, so these probes run as root. The .recovery-test
        # sidecar opts the write launch into the interruption.
        for phase, label in (("before-checkpoint", "rollback"), ("after-checkpoint", "commit")):
            for action in ("write", "read"):
                name = f"filesystem-recover-{label}-{action}.elf"
                elf("filesystem_probe", name, [f"--recover-{action}", phase], "0 0")
                if action == "write":
                    (games / (name + ".recovery-test")).write_text("", encoding="utf-8")
        script("filesystem-coreutils.sh", "CoreutilsFilesystemProbe.sh")
        bash("filesystem-coreutils.elf",
             ["--noprofile", "--norc", "./filesystem-coreutils.sh", "-d", HOME_SCRATCH])
        acceptance = ["--noprofile", "--norc", "./filesystem-coreutils.sh", "-d", HOME_SCRATCH,
                      "./filesystem-probe.elf"]
        if programs.get("fs_suite"):
            acceptance.append("./fs-suite.elf")
        bash("filesystem-acceptance.elf", acceptance)

    # Each suite runs as the default user and, through the .user sidecar, as
    # root so the cross-identity cases execute. Scratch goes to the home
    # folder of the identity. Filters name every group so a stale filter on
    # the SD card is replaced.
    proc_groups = [f"{group}/" for group in (
        "proc", "signal", "session", "cred", "clock", "timer", "event", "poll", "epoll", "inotify")]
    for role, base, groups in (("fs_suite", "fs-suite", []), ("proc_suite", "proc-suite", proc_groups),
                               ("comm_suite", "comm-suite", []), ("container_suite", "container-suite", []),
                               ("pty_suite", "pty-suite", ["pty/"])):
        elf(role, base + ".elf", ["-d", HOME_SCRATCH, *groups])
        elf(role, base + "-root.elf", ["-d", ROOT_SCRATCH, *groups], "0 0")

    if programs.get("process_acceptance"):
        script("process-tools.sh", "ProcessTools.sh")
        elf("process_acceptance", "process-acceptance.elf")
    if programs.get("comm_acceptance"):
        script("comm-tools.sh", "CommTools.sh")
        elf("comm_acceptance", "comm-acceptance.elf")
    if programs.get("container_acceptance"):
        script("container-tools.sh", "ContainerTools.sh")
        elf("container_acceptance", "container-acceptance.elf")
    elf("name_limit_probe", "name-limit-probe.elf", ["-d", HOME_SCRATCH])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", type=Path, help="Directory containing the unpacked Debian packages")
    parser.add_argument("output", type=Path, help="Directory in which to create fex/")
    parser.add_argument("probe", type=Path)
    parser.add_argument("glibc_probe", type=Path)
    parser.add_argument("process_probe", type=Path)
    parser.add_argument("--games", action="store_true",
                        help="Stage only the test programs into fex/games (no RootFS files)")
    parser.add_argument("--filesystem-probe", type=Path, help="Include filesystem and coreutils tests (--games only)")
    parser.add_argument("--fs-suite", type=Path, help="Include the filesystem compatibility suite (--games only)")
    parser.add_argument("--name-limit-probe", type=Path, help="Include the name length probe (--games only)")
    parser.add_argument("--proc-suite", type=Path, help="Include the process and event compatibility suite (--games only)")
    parser.add_argument("--process-acceptance", type=Path, help="Include the process acceptance wrapper and tools (--games only)")
    parser.add_argument("--comm-suite", type=Path, help="Include the memory and communication compatibility suite (--games only)")
    parser.add_argument("--comm-acceptance", type=Path, help="Include the communication acceptance wrapper and tools (--games only)")
    parser.add_argument("--container-suite", type=Path, help="Include the container and scheduling compatibility suite (--games only)")
    parser.add_argument("--pty-suite", type=Path, help="Include the pseudo-terminal compatibility suite (--games only)")
    parser.add_argument("--container-acceptance", type=Path, help="Include the container acceptance wrapper and tools (--games only)")
    args = parser.parse_args()
    if args.games:
        programs = {"thread_ipc": args.probe, "glibc_probe": args.glibc_probe, "process_probe": args.process_probe}
        programs["filesystem_probe"] = args.filesystem_probe
        programs["fs_suite"] = args.fs_suite
        programs["name_limit_probe"] = args.name_limit_probe
        programs["proc_suite"] = args.proc_suite
        programs["process_acceptance"] = args.process_acceptance
        programs["comm_suite"] = args.comm_suite
        programs["comm_acceptance"] = args.comm_acceptance
        programs["container_suite"] = args.container_suite
        programs["pty_suite"] = args.pty_suite
        programs["container_acceptance"] = args.container_acceptance
        programs = {role: file.resolve() for role, file in programs.items() if file}
        stage_games(args.source.resolve(), args.output.resolve(), programs)
    else:
        stage(args.source.resolve(), args.output.resolve(), args.probe.resolve(),
              args.glibc_probe.resolve(), args.process_probe.resolve())
