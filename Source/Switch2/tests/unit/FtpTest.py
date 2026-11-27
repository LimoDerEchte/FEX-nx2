#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Drives an FTP server built from GuestFtp.h with the standard ftplib client.

Usage: FtpTest.py host port password [expected-uid]
       FtpTest.py --server <ftp-server-host binary>

Any user name is accepted. The server must serve a writable root
directory. With expected-uid, files and directories the test creates
must report that owner (1000 on Switch 2). With --server, the
host build is started on a temporary directory and stopped afterwards. The test works below
/ftp-test, crosses Horizon's 257-byte SD path limit with deep directories
and 255-byte names, and checks transfers, resume, listings, renames,
timestamps and Linux error replies. Exits 0 when every check passes.
"""
import ftplib
import io
import re
import subprocess
import sys
import tempfile


def with_host_server(binary):
    with tempfile.TemporaryDirectory() as root:
        server = subprocess.Popen([binary, root, "secret"], stdout=subprocess.PIPE, text=True)
        try:
            port = int(server.stdout.readline().split()[1])
            return run("127.0.0.1", port, "secret", "fex", None)
        finally:
            server.kill()
            server.wait()


def main():
    if sys.argv[1] == "--server":
        return with_host_server(sys.argv[2])
    return run(sys.argv[1], int(sys.argv[2]), sys.argv[3], "fex", sys.argv[4] if len(sys.argv) > 4 else None)


def run(host, port, password, user, expected_uid):
    failures = []

    def check(condition, label):
        print(("ok   " if condition else "FAIL ") + label)
        if not condition:
            failures.append(label)

    def error_of(call, *args):
        try:
            call(*args)
        except ftplib.error_perm as error:
            return str(error)
        return ""

    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=60)
    check(error_of(ftp.login, user, password + "x").startswith("530"), "wrong password is rejected")
    ftp.close()
    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=60)
    ftp.login(user, password)
    ftp.set_pasv(True)
    check(ftp.pwd() == "/", "starts in /")
    # Desktop clients open extra connections for transfers.
    second = ftplib.FTP()
    second.connect(host, port, timeout=60)
    second.login(user, password)
    check(second.pwd() == "/" and ftp.pwd() == "/", "two sessions at once")
    second.quit()
    ftp.mkd("/ftp-test")
    ftp.cwd("/ftp-test")
    check(ftp.pwd() == "/ftp-test", "cwd changes directory")

    # Deep directories: five 200-byte levels put the path past 1000 bytes.
    deep = "/ftp-test"
    for level in range(5):
        deep += "/" + chr(ord("a") + level) * 200
        ftp.mkd(deep)
    ftp.cwd(deep)
    check(ftp.pwd() == deep, "cwd into a 1 KiB deep directory")
    name = "f" * 255
    payload = bytes(range(256)) * 4096 + b"tail"
    ftp.storbinary("STOR " + name, io.BytesIO(payload))
    check(ftp.size(name) == len(payload), "SIZE of a deep 255-byte name")
    received = io.BytesIO()
    ftp.retrbinary("RETR " + name, received.write)
    check(received.getvalue() == payload, "RETR returns the uploaded bytes")
    received = io.BytesIO()
    ftp.retrbinary("RETR " + name, received.write, rest=1000)
    check(received.getvalue() == payload[1000:], "REST resumes a download")
    ftp.storbinary("APPE " + name, io.BytesIO(b"more"))
    check(ftp.size(name) == len(payload) + 4, "APPE appends")
    ftp.storbinary("STOR " + name, io.BytesIO(b"short"))
    check(ftp.size(name) == 5, "STOR replaces and truncates")

    listing = []
    ftp.retrlines("LIST", listing.append)
    check(len(listing) == 1 and listing[0].startswith("-") and listing[0].endswith(name), "LIST shows the file")
    fields = listing[0].split(None, 8)
    check(len(fields) == 9 and fields[8] == name and re.fullmatch(r"\d\d:\d\d", fields[7]) is not None,
          "LIST has the ls -l fields (recent file: month, day, time, name)")
    names = ftp.nlst()
    check(names == [name], "NLST names the file")
    facts = list(ftp.mlsd())
    check(len(facts) == 1 and facts[0][0] == name and facts[0][1]["type"] == "file" and facts[0][1]["size"] == "5",
          "MLSD reports type and size")
    if expected_uid is not None:
        check(facts[0][1].get("unix.uid") == expected_uid, f"new files belong to uid {expected_uid}")
        parent = [fact for entry, fact in ftp.mlsd("..") if entry == deep.rsplit("/", 1)[1]]
        check(len(parent) == 1 and parent[0].get("unix.uid") == expected_uid, f"new directories belong to uid {expected_uid}")

    check(ftp.sendcmd("MFMT 20200102030405 " + name).startswith("213"), "MFMT sets the modification time")
    check(ftp.sendcmd("MDTM " + name) == "213 20200102030405", "MDTM reads it back")
    listing = []
    ftp.retrlines("LIST", listing.append)
    fields = listing[0].split(None, 8) if len(listing) == 1 else []
    check(len(fields) == 9 and fields[5:8] == ["Jan", "2", "2020"] and fields[8] == name,
          "LIST shows the year for an old file and keeps the name intact")

    # Rename across the limit in both directions.
    ftp.rename(name, "/ftp-test/shallow")
    check(ftp.size("/ftp-test/shallow") == 5, "rename a deep file to a shallow path")
    ftp.rename("/ftp-test/shallow", deep + "/" + "g" * 255)
    check(ftp.size("g" * 255) == 5, "rename a shallow file to a deep path")
    ftp.mkd("/ftp-test/tree")
    ftp.storbinary("STOR /ftp-test/tree/inner", io.BytesIO(b"inner"))
    ftp.rename("/ftp-test/tree", deep + "/tree")
    received = io.BytesIO()
    ftp.retrbinary("RETR tree/inner", received.write)
    check(received.getvalue() == b"inner", "rename a directory tree below the limit")

    # Linux errors come back as 550 replies.
    check("No such file" in error_of(ftp.size, "missing"), "missing file is ENOENT")
    check("Is a directory" in error_of(ftp.delete, "tree"), "DELE of a directory is EISDIR")
    check("not empty" in error_of(ftp.rmd, "tree"), "RMD of a full directory is ENOTEMPTY")
    check("File exists" in error_of(ftp.mkd, "tree"), "MKD of an existing name is EEXIST")
    check("Not a directory" in error_of(ftp.cwd, "g" * 255), "CWD into a file is ENOTDIR")
    check("too long" in error_of(ftp.mkd, "n" * 256), "a 256-byte name is ENAMETOOLONG")
    check(error_of(ftp.sendcmd, "PORT 127,0,0,1,1,1").startswith("502"), "active mode is refused")

    # Clean up everything, deepest first.
    ftp.delete("tree/inner")
    ftp.rmd("tree")
    ftp.delete("g" * 255)
    ftp.cwd("/ftp-test")
    while deep != "/ftp-test":
        ftp.rmd(deep)
        deep = deep.rsplit("/", 1)[0]
    ftp.cwd("..")
    ftp.rmd("/ftp-test")
    check(ftp.nlst("/") == [] or "ftp-test" not in ftp.nlst("/"), "everything was removed")
    ftp.quit()

    print(f"ftp test: {len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
