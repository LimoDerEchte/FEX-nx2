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


def stage(source, output, probe, glibc_probe, process_probe, filesystem_probe=None, fs_suite=None,
          name_limit_probe=None, proc_suite=None, rootfs_manifest=None, process_acceptance=None, comm_suite=None, comm_acceptance=None, container_suite=None, container_acceptance=None):
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


    if filesystem_probe:
        tools = ("cat", "chmod", "cp", "df", "ln", "ls", "mkdir", "mktemp", "mv", "readlink", "rm", "stat", "touch")
        for executable in tools:
            candidates = (source / "bin" / executable, source / "usr/bin" / executable)
            file = next((candidate for candidate in candidates if candidate.is_file()), candidates[0])
            install(file, root / "bin" / executable)

        for name, arguments in (
            ("filesystem-probe.elf", None),
            ("filesystem-persist-write.elf", "--persist-write\n/.fex-persistence-01231\n"),
            ("filesystem-persist-read.elf", "--persist-read\n/.fex-persistence-01231\n"),
        ):
            install(filesystem_probe, root / "bin" / name)
            if arguments:
                (root / "bin" / (name + ".args")).write_text(arguments, encoding="utf-8")

        for phase, label in (("before-checkpoint", "rollback"), ("after-checkpoint", "commit")):
            for action in ("write", "read"):
                name = f"filesystem-recover-{label}-{action}.elf"
                install(filesystem_probe, root / "bin" / name)
                (root / "bin" / (name + ".args")).write_text(
                    f"--recover-{action}\n{phase}\n", encoding="utf-8"
                )
                if action == "write":
                    # Opts the launch into the interrupted journal move.
                    (root / "bin" / (name + ".recovery-test")).write_text("", encoding="utf-8")

        install(source / "bin/bash", root / "bin/filesystem-coreutils.elf")
        script = Path(__file__).with_name("CoreutilsFilesystemProbe.sh")
        shutil.copyfile(script, root / "bin/filesystem-coreutils.sh")
        (root / "bin/filesystem-coreutils.elf.args").write_text(
            "--noprofile\n--norc\n/bin/filesystem-coreutils.sh\n", encoding="utf-8"
        )
        install(source / "bin/bash", root / "bin/filesystem-acceptance.elf")
        acceptance = "--noprofile\n--norc\n/bin/filesystem-coreutils.sh\n/bin/filesystem-probe.elf\n"
        if fs_suite:
            acceptance += "/bin/fs-suite.elf\n"
        (root / "bin/filesystem-acceptance.elf.args").write_text(acceptance, encoding="utf-8")

    if fs_suite:
        # The suite runs as the default user and, through the launcher's
        # credential file, as root so cross-identity cases execute.
        install(fs_suite, root / "bin/fs-suite.elf")
        install(fs_suite, root / "bin/fs-suite-root.elf")
        (root / "bin/fs-suite-root.elf.user").write_text("0 0\n", encoding="utf-8")

    if proc_suite:
        # Like fs-suite: default user plus a root variant via the sidecar.
        install(proc_suite, root / "bin/proc-suite.elf")
        install(proc_suite, root / "bin/proc-suite-root.elf")
        (root / "bin/proc-suite-root.elf.user").write_text("0 0\n", encoding="utf-8")
        # Name every group so a stale filter on the SD card is replaced.
        groups = "".join(f"{group}/\n" for group in (
            "proc", "signal", "session", "cred", "clock", "timer", "event", "poll", "epoll", "inotify"))
        for name in ("proc-suite.elf.args", "proc-suite-root.elf.args"):
            (root / "bin" / name).write_text(groups, encoding="utf-8")

    if process_acceptance:
        # Debian tools for the milestone 2 acceptance script.
        for tool in ("bin/cat", "bin/date", "bin/grep", "bin/kill", "bin/mkdir", "bin/rm", "bin/touch",
                     "usr/bin/head", "usr/bin/seq", "usr/bin/tr", "usr/bin/wc", "usr/bin/inotifywait"):
            install(source / tool, root / tool)
        shutil.copyfile(Path(__file__).with_name("ProcessTools.sh"), root / "bin/process-tools.sh")
        install(process_acceptance, root / "bin/process-acceptance.elf")

    if comm_suite:
        # Memory and communication suite: default user plus a root variant.
        install(comm_suite, root / "bin/comm-suite.elf")
        install(comm_suite, root / "bin/comm-suite-root.elf")
        (root / "bin/comm-suite-root.elf.user").write_text("0 0\n", encoding="utf-8")

    if comm_acceptance:
        # Debian tools for the milestone 3 acceptance script.
        for tool in ("bin/cat", "bin/mkdir", "bin/rm", "bin/grep", "bin/sleep", "usr/bin/head", "usr/bin/seq",
                     "usr/bin/tr", "usr/bin/wc", "usr/bin/timeout", "usr/bin/sort",
                     "usr/bin/sha256sum", "usr/bin/mkfifo", "usr/bin/xz", "usr/bin/getent"):
            file = source / tool
            if not file.is_file():
                file = source / ("usr/bin" if tool.startswith("bin/") else "bin") / Path(tool).name
            install(file, root / tool)
        # Debian's nc is the netcat-openbsd alternative.
        install(source / "bin/nc.openbsd", root / "bin/nc")
        shutil.copyfile(Path(__file__).with_name("CommTools.sh"), root / "bin/comm-tools.sh")
        install(comm_acceptance, root / "bin/comm-acceptance.elf")

    if container_suite:
        # Container suite: default user plus a root variant.
        install(container_suite, root / "bin/container-suite.elf")
        install(container_suite, root / "bin/container-suite-root.elf")
        (root / "bin/container-suite-root.elf.user").write_text("0 0\n", encoding="utf-8")

    if container_acceptance:
        # Debian tools for the milestone 4 acceptance script.
        for tool in ("bin/cat", "bin/mkdir", "bin/rm", "bin/grep", "bin/sleep", "bin/mount", "bin/kill",
                     "bin/hostname", "bin/ls", "usr/bin/unshare", "usr/bin/nsenter", "usr/bin/chrt",
                     "usr/bin/taskset", "usr/bin/ionice", "usr/bin/bwrap", "usr/bin/wc", "usr/bin/head",
                     "usr/bin/id", "usr/bin/printf", "usr/bin/tr", "usr/bin/seq", "usr/bin/timeout"):
            file = source / tool
            if not file.is_file():
                file = source / ("usr/bin" if tool.startswith("bin/") else "bin") / Path(tool).name
            install(file, root / tool)
        shutil.copyfile(Path(__file__).with_name("ContainerTools.sh"), root / "bin/container-tools.sh")
        install(container_acceptance, root / "bin/container-acceptance.elf")
        # bubblewrap mounts its tmpfs on /tmp, which base-files would provide.
        (root / "tmp").mkdir(exist_ok=True)

    if name_limit_probe:
        install(name_limit_probe, root / "bin/name-limit-probe.elf")

    if rootfs_manifest:
        setup = output / "fex/setup"
        setup.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(rootfs_manifest, setup / "debian-rootfs.manifest")



if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Directory containing the unpacked Debian packages")
    parser.add_argument("output", type=Path, help="Directory in which to create fex/")
    parser.add_argument("probe", type=Path)
    parser.add_argument("glibc_probe", type=Path)
    parser.add_argument("process_probe", type=Path)
    parser.add_argument("--filesystem-probe", type=Path, help="Include filesystem and coreutils tests")
    parser.add_argument("--fs-suite", type=Path, help="Include the filesystem compatibility suite")
    parser.add_argument("--name-limit-probe", type=Path, help="Include the name length probe")
    parser.add_argument("--proc-suite", type=Path, help="Include the process and event compatibility suite")
    parser.add_argument("--rootfs-manifest", type=Path, help="Stage the Debian rootfs download manifest")
    parser.add_argument("--process-acceptance", type=Path, help="Include the process acceptance wrapper and tools")
    parser.add_argument("--comm-suite", type=Path, help="Include the memory and communication compatibility suite")
    parser.add_argument("--comm-acceptance", type=Path, help="Include the communication acceptance wrapper and tools")
    parser.add_argument("--container-suite", type=Path, help="Include the container and scheduling compatibility suite")
    parser.add_argument("--container-acceptance", type=Path, help="Include the container acceptance wrapper and tools")
    args = parser.parse_args()
    stage(
        args.source.resolve(), args.output.resolve(), args.probe.resolve(),
        args.glibc_probe.resolve(), args.process_probe.resolve(),
        args.filesystem_probe.resolve() if args.filesystem_probe else None,
        args.fs_suite.resolve() if args.fs_suite else None,
        args.name_limit_probe.resolve() if args.name_limit_probe else None,
        args.proc_suite.resolve() if args.proc_suite else None,
        args.rootfs_manifest.resolve() if args.rootfs_manifest else None,
        args.process_acceptance.resolve() if args.process_acceptance else None,
        args.comm_suite.resolve() if args.comm_suite else None,
        args.comm_acceptance.resolve() if args.comm_acceptance else None,
        args.container_suite.resolve() if args.container_suite else None,
        args.container_acceptance.resolve() if args.container_acceptance else None,
    )
