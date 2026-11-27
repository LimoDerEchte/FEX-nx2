#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Checks the RootFS image installer against host tools.

Builds fixture squashfs images with the host's mksquashfs (zstd, gzip and xz), extracts the zstd one with the installer's
extraction code and compares the result with the fixture sources (content, modes, owners, links, hard links, sparse
files, fragments, long names) and with unsquashfs; then runs the pipeline scenarios (download resume, cancel cleanup,
hash mismatch, extras from a fake Ubuntu archive).

Usage: CheckRootfsInstall.py TEST_BINARY WORK_DIR
"""
import base64, hashlib, io, lzma, os, secrets, shutil, stat, subprocess, sys, tarfile
from pathlib import Path


HOST_UID, HOST_GID = os.getuid(), os.getgid()


def run(*args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def build_tree(root: Path):
    """Returns {path: (type, mode, uid, gid)} for the entries whose attributes the check pins down."""
    expected = {}
    pseudo = []

    def add(path, type_, mode, uid=HOST_UID, gid=HOST_GID):
        expected["/" + path] = (type_, mode, uid, gid)
        pseudo.append(f"{path} m {mode:o} {uid} {gid}")

    def mkdir(path, mode=0o755, uid=HOST_UID, gid=HOST_GID):
        (root / path).mkdir(parents=True, exist_ok=True)
        add(path, "d", mode, uid, gid)

    def write(path, data: bytes, mode=0o644, uid=HOST_UID, gid=HOST_GID):
        (root / path).parent.mkdir(parents=True, exist_ok=True)
        (root / path).write_bytes(data)
        add(path, "f", mode, uid, gid)

    rnd = bytearray()
    state = 12345
    for _ in range(900 * 1024):
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        rnd.append((state >> 16) & 0xFF)
    rnd = bytes(rnd)
    mkdir("usr/bin")
    mkdir("usr/lib/x86_64-linux-gnu")
    mkdir("usr/share/doc")
    mkdir("etc/apt")
    mkdir("var/lib/dpkg")
    mkdir("var/lib/dbus")
    mkdir("var/cache")
    mkdir("root", 0o700, 0, 0)
    write("usr/bin/hello", b"hello\n", 0o755)
    os.link(root / "usr/bin/hello", root / "usr/bin/hello-hard")
    write("usr/bin/setuid-probe", b"#!/bin/sh\n", 0o4755, 12, 34)
    # The FEX images own everything as uid/gid 1000 (the image builder); the installer re-owns those to root.
    mkdir("etc/sudoers.d", 0o755, 1000, 1000)
    write("etc/sudoers.d/README", b"# sudoers.d\n", 0o440, 1000, 1000)
    write("usr/lib/big.bin", rnd, 0o644)                          # many blocks plus a fragment tail
    write("usr/lib/block-aligned.bin", rnd[:131072 * 3], 0o644)                # exact multiple of the block size
    write("usr/lib/tail.bin", rnd[:131072 + 100], 0o644)                       # one block and a 100 byte tail
    write("usr/lib/empty", b"", 0o644)
    for index in range(30):
        write(f"usr/lib/frag{index:02d}", rnd[index * 50:index * 50 + 40 + index], 0o644)  # many small files share fragments
    sparse = root / "usr/lib/sparse.bin"
    with open(sparse, "wb") as f:
        f.truncate(1 << 20)
        f.seek((1 << 20) - 10)
        f.write(b"endofsparse")
    add("usr/lib/sparse.bin", "f", 0o644)
    long_name = "n" * 255
    write(f"usr/share/doc/{long_name}", b"long name\n", 0o644)
    os.symlink("usr/bin", root / "bin")
    add("bin", "l", 0o777)
    os.symlink("/usr/lib/big.bin", root / "usr/lib/link-abs")
    add("usr/lib/link-abs", "l", 0o777, 5, 6)
    os.symlink("../lib/big.bin", root / "usr/lib/link-rel")
    add("usr/lib/link-rel", "l", 0o777)
    os.symlink("x" * 300 + "/target", root / "usr/lib/link-long")
    add("usr/lib/link-long", "l", 0o777)
    mkdir("tmp", 0o1777)
    mkdir("run")
    os.mkfifo(root / "run/fifo")
    add("run/fifo", "p", 0o644)
    mkdir("dev")
    pseudo.append("dev/null c 666 0 0 1 3")
    pseudo.append("dev/sda b 660 0 6 8 0")
    # An FEX style broken chroot, for the pipeline scenarios.
    mkdir("chroot")
    for d in ("boot", "home", "media", "mnt", "opt", "srv", "tmp", "run", "var", "var/tmp", "var/lib", "var/lib/dbus", "var/cache", "etc"):
        mkdir("chroot/" + d)
    mkdir("chroot/root", 0o700)
    write("chroot/root/.bash_history", b"ls\n", 0o600)
    for name, body in {
        "hosts": "127.0.0.1\tlocalhost\n", "timezone": "America/New_York\n", "fstab": "# fstab\n", "hostname": "localhost.localdomain\n",
        "subuid": "", "subgid": "", "machine-id": "9f7ea62a2fe34a88bd981a0ba5e130c6\n",
        "passwd": "root:x:0:0:root:/root:/bin/bash\nnobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin\n",
        "passwd-": "root:x:0:0:root:/root:/bin/bash\n", "group": "root:x:0:\nnogroup:x:65534:\n", "group-": "root:x:0:\n",
        "shadow": "root:*:19000:0:99999:7:::\n", "shadow-": "root:*:19000:0:99999:7:::\n", "gshadow": "root:*::\n", "gshadow-": "root:*::\n",
    }.items():
        write("chroot/etc/" + name, body.encode(), 0o640 if "shadow" in name else 0o644)
    os.symlink("../run/systemd/resolve/stub-resolv.conf", root / "chroot/etc/resolv.conf")
    add("chroot/etc/resolv.conf", "l", 0o777)
    os.symlink("/usr/share/zoneinfo/America/New_York", root / "chroot/etc/localtime")
    add("chroot/etc/localtime", "l", 0o777)
    os.symlink("../proc/self/mounts", root / "chroot/etc/mtab")
    add("chroot/etc/mtab", "l", 0o777)
    write("chroot/var/lib/dbus/machine-id", b"9f7ea62a2fe34a88bd981a0ba5e130c6\n")
    os.symlink("/run", root / "chroot/var/run")
    add("chroot/var/run", "l", 0o777)
    os.symlink("/run/lock", root / "chroot/var/lock")
    add("chroot/var/lock", "l", 0o777)
    os.symlink("/dev/shm", root / "chroot/run/shm")
    add("chroot/run/shm", "l", 0o777)
    write("chroot/run/adduser", b"", 0o644)
    # dpkg status of the image (extras are resolved against it).
    status = (
        "Package: libc6\nStatus: install ok installed\nArchitecture: amd64\nVersion: 2.39-0ubuntu8\n\n"
        "Package: base-files\nStatus: install ok installed\nArchitecture: amd64\nVersion: 13ubuntu10\n\n"
        # A removed package dpkg still remembers (its conffiles stay): installing it replaces this entry.
        "Package: sudo\nStatus: deinstall ok config-files\nPriority: important\nArchitecture: amd64\nVersion: 1.9.15p5-3ubuntu5\n"
        "Conffiles:\n /etc/sudoers 2b4c0bf4e2f5c4f1d2e7b1b0b6e0c1aa\n\n"
        # Listed as installed, but the image lacks their files (su, mount): the extras unpack them anyway.
        "Package: util-linux\nStatus: install ok installed\nArchitecture: amd64\nVersion: 2.39.3-9ubuntu6.5\n\n"
        "Package: mount\nStatus: install ok installed\nArchitecture: amd64\nVersion: 2.39.3-9ubuntu6.5\n\n"
    )
    write("var/lib/dpkg/status", status.encode())
    return expected, "\n".join(pseudo) + "\n"


def mksquashfs(src: Path, out: Path, pseudo: str, comp="zstd", extra=()):
    pf = out.with_suffix(".pf")
    pf.write_text(pseudo)
    out.unlink(missing_ok=True)
    run("mksquashfs", src, out, "-comp", comp, "-noappend", "-quiet", "-no-progress", "-all-time", "1700000000", "-pf", pf, *extra)


def tree_sha(path: Path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_extraction(binary, work, source: Path, expected, image: Path):
    out = work / "extracted"
    run(binary, "extract", image, out)
    meta = {}
    for line in Path(str(out) + ".meta").read_text().splitlines():
        path, kind, mode, uid, gid, detail = line.split("|", 5)
        meta[path] = (kind, int(mode, 8), int(uid), int(gid), detail)
    problems = []
    for path, (kind, mode, uid, gid) in expected.items():
        got = meta.get(path)
        if got is None:
            problems.append(f"missing {path}")
            continue
        if got[0] != kind:
            problems.append(f"{path}: type {got[0]} != {kind}")
        if kind != "l" and (got[1] != mode or got[2] != uid or got[3] != gid) and not (kind == "p"):
            problems.append(f"{path}: mode/owner {got[1]:o} {got[2]}/{got[3]} != {mode:o} {uid}/{gid}")
        if kind == "p" and (got[1] != mode or got[2] != uid):
            problems.append(f"{path}: fifo attrs")
    # Content of every regular file, symlink targets, hard links, sizes (sparse files included).
    inodes = {}
    for dirpath, dirnames, filenames in os.walk(source):
        for name in dirnames + filenames:
            src = Path(dirpath) / name
            rel = "/" + str(src.relative_to(source))
            dst = out / rel[1:]
            if src.is_symlink():
                if os.readlink(dst) != os.readlink(src):
                    problems.append(f"{rel}: symlink target differs")
            elif src.is_file():
                if dst.stat().st_size != src.stat().st_size or tree_sha(dst) != tree_sha(src):
                    problems.append(f"{rel}: content differs")
                inodes.setdefault((src.stat().st_ino), []).append(rel)
    for names in inodes.values():
        if len(names) > 1:
            if len({(out / n[1:]).stat().st_ino for n in names}) != 1:
                problems.append(f"hard links not preserved: {names}")
    summary = dict(line.split() for line in Path(str(out) + ".summary").read_text().splitlines())
    if int(summary["devices"]) != 2:
        problems.append(f"devices skipped {summary['devices']} != 2")
    if int(summary["hardlinks"]) != 1:
        problems.append(f"hard links {summary['hardlinks']} != 1")
    # Same image through unsquashfs: identical file contents and names.
    ref = work / "unsquashfs"
    shutil.rmtree(ref, ignore_errors=True)
    subprocess.run(["unsquashfs", "-q", "-no-xattrs", "-d", str(ref), str(image)], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    a = sorted(str(p.relative_to(ref)) for p in ref.rglob("*") if not (p.is_char_device() or p.is_block_device()))
    b = sorted(str(p.relative_to(out)) for p in out.rglob("*"))
    if a != b:
        problems.append(f"names differ from unsquashfs: {sorted(set(a) ^ set(b))[:5]}")
    for rel in a:
        p, q = ref / rel, out / rel
        if p.is_symlink():
            if os.readlink(p) != os.readlink(q):
                problems.append(f"{rel}: unsquashfs link differs")
        elif p.is_file() and tree_sha(p) != tree_sha(q):
            problems.append(f"{rel}: unsquashfs content differs")
    return problems


def make_deb(path: Path, name, version, files, depends="", provides=""):
    """files: {path: (bytes|link target, mode, kind)}."""
    control = f"Package: {name}\nVersion: {version}\nArchitecture: amd64\nMaintainer: Test <t@example.com>\nDescription: fixture {name}\n"
    if depends:
        control += f"Depends: {depends}\n"
    if provides:
        control += f"Provides: {provides}\n"
    def tar_bytes(entries):
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w", format=tarfile.GNU_FORMAT) as tar:
            for member, (data, mode, kind) in entries.items():
                info = tarfile.TarInfo(member)
                info.mode = mode
                info.mtime = 1700000000
                info.uname = info.gname = "root"
                if kind == "d":
                    info.type = tarfile.DIRTYPE
                    tar.addfile(info)
                elif kind == "l":
                    info.type = tarfile.SYMTYPE
                    info.linkname = data
                    tar.addfile(info)
                else:
                    info.size = len(data)
                    tar.addfile(info, io.BytesIO(data))
        return buf.getvalue()
    md5 = "".join(f"{hashlib.md5(d).hexdigest()}  {p[2:]}\n" for p, (d, m, k) in files.items() if k == "f")
    control_tar = tar_bytes({"./control": (control.encode(), 0o644, "f"), "./md5sums": (md5.encode(), 0o644, "f")})
    data_tar = tar_bytes(files)
    zst = subprocess.run(["zstd", "-q", "-c"], input=data_tar, stdout=subprocess.PIPE, check=True).stdout  # no content size in the frame
    xz = lzma.compress(control_tar, format=lzma.FORMAT_XZ)
    def member(name, data):
        header = f"{name + '/':<16}{1700000000:<12}{0:<6}{0:<6}{100644:<8}{len(data):<10}`\n".encode()
        return header + data + (b"\n" if len(data) % 2 else b"")
    deb = b"!<arch>\n" + member("debian-binary", b"2.0\n") + member("control.tar.xz", xz) + member("data.tar.zst", zst)
    path.write_bytes(deb)
    return control


def _probably_prime(n: int) -> bool:
    if n < 4:
        return n in (2, 3)
    for p in (3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % p == 0:
            return n == p
    d, r = n - 1, 0
    while d % 2 == 0:
        d, r = d // 2, r + 1
    for _ in range(40):
        x = pow(secrets.randbelow(n - 3) + 2, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(r - 1):
            x = pow(x, 2, n)
            if x == n - 1:
                break
        else:
            return False
    return True


def make_rsa_key(bits=2048):
    """A throwaway RSA key (e = 65537) for the fixture archive: (n, d)."""
    def prime():
        while True:
            p = secrets.randbits(bits // 2) | (3 << (bits // 2 - 2)) | 1
            if (p - 1) % 65537 and _probably_prime(p):
                return p
    p, q = prime(), prime()
    while q == p:
        q = prime()
    return p * q, pow(65537, -1, (p - 1) * (q - 1))


SHA512_DIGEST_INFO = bytes.fromhex("3051300d060960864801650304020305000440")


def clearsign(text: str, n: int, d: int) -> str:
    """An OpenPGP clear-signed message like the Ubuntu archive's InRelease: v4 signature, canonical text, RSA, SHA-512."""
    lines = text[:-1].split("\n") if text.endswith("\n") else text.split("\n")
    canonical = "\r\n".join(line.rstrip(" \t") for line in lines).encode()
    created = b"\x05\x02" + (1714057881).to_bytes(4, "big")  # signature creation time subpacket
    hashed = bytes([4, 0x01, 1, 10]) + len(created).to_bytes(2, "big") + created
    digest = hashlib.sha512(canonical + hashed + b"\x04\xff" + len(hashed).to_bytes(4, "big")).digest()
    k = (n.bit_length() + 7) // 8
    encoded = b"\x00\x01" + b"\xff" * (k - 3 - len(SHA512_DIGEST_INFO) - len(digest)) + b"\x00" + SHA512_DIGEST_INFO + digest
    signature = pow(int.from_bytes(encoded, "big"), d, n)
    body = hashed + b"\x00\x00" + digest[:2] + signature.bit_length().to_bytes(2, "big") + signature.to_bytes((signature.bit_length() + 7) // 8, "big")
    packet = bytes([0x89]) + len(body).to_bytes(2, "big") + body  # old format, tag 2, two-byte length (as GnuPG writes it)
    armor = base64.b64encode(packet).decode()
    escaped = "\n".join("- " + line if line.startswith("-") else line for line in lines)
    return ("-----BEGIN PGP SIGNED MESSAGE-----\nHash: SHA512\n\n" + escaped + "\n-----BEGIN PGP SIGNATURE-----\n\n" +
            "\n".join(armor[i:i + 64] for i in range(0, len(armor), 64)) + "\n-----END PGP SIGNATURE-----\n")


def build_archive(work: Path):
    apt = work / "image-dir" / "apt"
    shutil.rmtree(apt, ignore_errors=True)
    apt.mkdir(parents=True)
    pool = {
        ("main", "pool/main/b/bubblewrap/bubblewrap_0.9.0-1_amd64.deb"): ("bubblewrap", "0.9.0-1", {
            "./usr": (b"", 0o755, "d"), "./usr/bin": (b"", 0o755, "d"), "./usr/bin/bwrap": (b"bwrap\n", 0o755, "f")}, "libc6 (>= 2.34)", ""),
        ("main", "pool/main/n/netcat-openbsd/netcat-openbsd_1.226-1_amd64.deb"): ("netcat-openbsd", "1.226-1", {
            "./bin": (b"", 0o755, "d"), "./bin/nc.openbsd": (b"nc\n", 0o755, "f")}, "libc6 (>= 2.34), libbsd0", "netcat"),
        ("universe", "pool/universe/i/inotify-tools/inotify-tools_4.23.9.0-2_amd64.deb"): ("inotify-tools", "4.23.9.0-2", {
            "./usr": (b"", 0o755, "d"), "./usr/bin": (b"", 0o755, "d"), "./usr/bin/inotifywait": (b"inotifywait\n", 0o755, "f")},
            "libc6 (>= 2.34), libinotifytools0 (= 4.23.9.0-2)", ""),
        ("universe", "pool/universe/i/inotify-tools/libinotifytools0_4.23.9.0-2_amd64.deb"): ("libinotifytools0", "4.23.9.0-2", {
            "./usr": (b"", 0o755, "d"), "./usr/lib": (b"", 0o755, "d"), "./usr/lib/x86_64-linux-gnu": (b"", 0o755, "d"),
            "./usr/lib/x86_64-linux-gnu/libinotifytools.so.0.4.1": (b"lib\n", 0o644, "f"),
            "./usr/lib/x86_64-linux-gnu/libinotifytools.so.0": ("libinotifytools.so.0.4.1", 0o777, "l")}, "libc6 (>= 2.34)", ""),
    }
    # Everyday tools (one file each is enough for the pipeline).
    for component, name, version, path in (("main", "sudo", "1.9.15p5-3ubuntu5", "./usr/bin/sudo"),
                                           ("main", "nano", "7.2-2build1", "./usr/bin/nano"),
                                           ("main", "wget", "1.21.4-1ubuntu4", "./usr/bin/wget"),
                                           ("main", "whiptail", "0.52.24-2ubuntu2", "./usr/bin/whiptail"),
                                           ("main", "bsdutils", "1:2.39.3-9ubuntu6.6", "./usr/bin/script"),
                                           ("main", "util-linux", "2.39.3-9ubuntu6.6", "./usr/bin/su"),
                                           ("main", "mount", "2.39.3-9ubuntu6.6", "./usr/bin/mount")):
        url = f"pool/{component}/{name[0]}/{name}/{name}_{version}_amd64.deb"
        pool[(component, url)] = (name, version, {"./usr": (b"", 0o755, "d"), "./usr/bin": (b"", 0o755, "d"),
                                                  path: (name.encode() + b"\n", 0o755, "f")}, "libc6 (>= 2.34)", "")
    # libbsd0 is a netcat dependency the image lacks: it must be pulled in from main.
    pool[("main", "pool/main/libb/libbsd/libbsd0_0.12.1-1_amd64.deb")] = ("libbsd0", "0.12.1-1", {
        "./usr": (b"", 0o755, "d"), "./usr/lib": (b"", 0o755, "d"), "./usr/lib/x86_64-linux-gnu": (b"", 0o755, "d"),
        "./usr/lib/x86_64-linux-gnu/libbsd.so.0": (b"bsd\n", 0o644, "f")}, "libc6 (>= 2.34)", "")
    index = {"main": "", "universe": ""}
    urls = []
    for (component, url), (name, version, files, depends, provides) in pool.items():
        deb = apt / Path(url).name
        control = make_deb(deb, name, version, files, depends, provides)
        data = deb.read_bytes()
        index[component] += control + f"Filename: {url}\nSize: {len(data)}\nSHA256: {hashlib.sha256(data).hexdigest()}\n\n"
        urls.append(f"{url} {deb.name}")
    # A tampered copy of bubblewrap with the same size (a flipped byte near the end).
    original = (apt / "bubblewrap_0.9.0-1_amd64.deb").read_bytes()
    tampered = bytearray(original)
    tampered[-5] ^= 0xFF
    (apt / "tampered.deb").write_bytes(bytes(tampered))
    (apt / "urls.txt").write_text("\n".join(urls) + "\n")
    # An old version in "noble" and the same package newer in "noble-updates": the later suite wins. The older one has no .deb.
    key_n, key_d = make_rsa_key()
    older = "Package: bubblewrap\nVersion: 0.1-old\nArchitecture: amd64\nFilename: pool/main/b/bubblewrap/old.deb\nSize: 1\nSHA256: " + "0" * 64 + "\n\n"
    for suite in ("noble", "noble-updates", "noble-security"):
        entries = []
        for component in ("main", "universe"):
            text = (older if suite == "noble" and component == "main" else "") + (index[component] if suite == "noble-updates" else "")
            if suite == "noble" and component == "main":
                text = older + index[component]  # noble holds the same packages; noble-updates repeats them, the later suite wins
            elif suite == "noble":
                text = index[component]
            data = lzma.compress(text.encode(), format=lzma.FORMAT_XZ)
            fixture = apt / f"{suite}.{component}.Packages.xz"
            fixture.write_bytes(data)
            entries.append(f" {hashlib.sha256(data).hexdigest()} {len(data)} {component}/binary-amd64/Packages.xz")
        release = "Origin: Ubuntu\nSuite: " + suite + "\nSHA256:\n" + "\n".join(entries) + "\n"
        (apt / f"{suite}.InRelease").write_text(clearsign(release, key_n, key_d))
        if suite == "noble-updates":
            # The same index with a different hash but the original signature (a network attacker without the key).
            signed = clearsign(release, key_n, key_d)
            (apt / "tampered.InRelease").write_text(signed.replace(entries[0][1:65], "0" * 64, 1))
            # Valid hashes, signed by another key.
            other_n, other_d = make_rsa_key()
            (apt / "otherkey.InRelease").write_text(clearsign(release, other_n, other_d))
    (apt / "archive-key.n").write_bytes(key_n.to_bytes((key_n.bit_length() + 7) // 8, "big"))
    return apt


def main():
    binary, work = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    run(binary)  # unit checks
    source = work / "source"
    source.mkdir()
    expected, pseudo = build_tree(source)
    image_dir = work / "image-dir"
    image_dir.mkdir()
    image = image_dir / "image.sqsh"
    # The image root as the FEX images have it: 0755, owned by the builder (1000), whatever the host user is.
    mksquashfs(source, image, pseudo, extra=("-root-mode", "755", "-root-uid", "1000", "-root-gid", "1000"))
    problems = check_extraction(binary, work, source, expected, image)
    # Other compressors are rejected with a clear message.
    small = work / "small"
    small.mkdir()
    (small / "a").write_text("a")
    for comp in ("gzip", "xz"):
        mksquashfs(small, image_dir / f"{comp}.sqsh", "", comp=comp)
    build_archive(work)
    if problems:
        for line in problems:
            print("FAIL", line)
        sys.exit(1)
    print("ok extraction matches the fixture sources and unsquashfs")
    run(binary, "scenarios", image_dir)
    print("rootfs install checks passed")


if __name__ == "__main__":
    main()
