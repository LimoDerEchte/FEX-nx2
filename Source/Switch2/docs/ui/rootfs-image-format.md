# RootFS images and the installer

Source of the images: the [FEX-Emu/RootFS](https://github.com/FEX-Emu/RootFS) project. Only **Ubuntu 24.04** is installable.
There is no Debian option (removed 2026-11-26; the project publishes no Debian image). Code (all under
`Source/Switch2`): `runtime/fs/GuestSquashfs.h` (reader), `runtime/fs/GuestRootfsInstall.h` (pipeline), `host/HttpClient.h`,
`launcher/RootfsInstallHost.cpp` (console side), `ui/RootfsIndex.{h,cpp}` and `ui/assets/rootfs-index.json` (index).

## Index

`ui/assets/rootfs-index.json` is embedded in the NRO (nothing is copied to the SD card). It has the project's
`RootFS_links.json` v1 format (`DistroMatch`, `DistroVersion`, `URL`, `Hash`, `Type`) plus `Name`, `DownloadSize`,
`InstalledSize`, `ExtraDownloadSize`, `Extras`, and `Available`/`Reason` (an entry that cannot be installed; none in the built-in index). `Hash` is the hex XXH3-64 of the whole file;
the project drops leading zeros (`cee02b6ea44829d` is valid). The console has no TLS, so every URL is used as `http://`
(`rootfs.fex-emu.gg` serves the same files over plain HTTP, 200, no redirect). "Check for newer images" downloads
`http://rootfs.fex-emu.gg/RootFS_links.json` and offers a squashfs entry of the same distro and version whose date folder is later;
an unreachable or unparsable live index falls back to the built-in one. Over plain HTTP the hash checks **integrity, not
authenticity**: someone who can change the traffic can change the image and the live index together.

Facts checked on 2026-11-25 for `Ubuntu_24_04/2026-08-11/Ubuntu_24_04.sqsh`: 525615104 bytes, XXH3-64 `3517e0e5ea25a473`
(`xxhsum -H3`), squashfs 4.0, zstd, block size 131072, 39528 inodes (26778 files, 9009 symlinks, 3741 folders, no device nodes, 5
hard-linked names), 1889068175 bytes of file data, one id (uid and gid 1000, the image builder), no extended attributes.

## Squashfs reader

Reads superblock, id table, inode table, directory table and fragment table; all inode types (basic and extended files,
directories, symlinks, devices, fifos, sockets); zstd metadata and data blocks; stored (uncompressed) blocks; sparse blocks
(size 0, arrive as zeros). gzip, xz, lzma, lzo and lz4 images fail with "Unsupported squashfs compression" and the name of the
compressor. An image with an xattr table is rejected rather than extracted without them. The image is read in bounded pieces
(64 KiB stdio buffer, one block at a time, a 16-block metadata cache); it is never held in memory. Names with `/`, `.`, `..` or NUL
are refused. A second directory entry naming an inode with more than one link becomes a hard link to the first path.

## Extraction

Entries stream in tree order into `RootfsWriter` (the guest filesystem service): folders, files (streamed, with owner, mode and
mtime), symlinks, hard links, FIFOs. Owners and modes are the image's (uid/gid 1000 for everything, as FEX ships it; setuid files
are already removed). Device nodes and sockets cannot be created by the runtime (it provides its own `/dev`); the Ubuntu image has
none. If an image has any they are counted, logged as warnings and skipped.

## The unbreak step

FEX's `Scripts/chroot.py break` (and `Scripts/Ubuntu/break_chroot.sh`) moves these into `/chroot/`: `/etc/{hosts,resolv.conf,
timezone,localtime,passwd,passwd-,group,group-,shadow,shadow-,gshadow,gshadow-,fstab,hostname,mtab,subuid,subgid,machine-id}`,
`boot home media mnt root srv tmp run`, `opt` when empty, `var/{tmp,run,lock}` and `var/lib/dbus/machine-id`; it deletes `/proc`,
`/sys`, `/dev`, `/usr/share/fex-emu` and `var/cache/apt`, and the image build removes setuid files and sudo. The launcher does what
`unbreak_chroot.sh` and `chroot.py unbreak` do, without a chroot and without mounts:

1. Every entry of `/chroot` is moved back to the same path under `/` (a rename; a folder that exists on both sides is merged entry
   by entry; `/chroot` is removed). On the Ubuntu 24.04 image: 31 moved, 5 merged (`etc`, `var`, `var/lib`, `var/lib/dbus`,
   `var/cache`), 0 kept, `/chroot` removed.
2. `/tmp` gets mode 1777; `/proc`, `/sys`, `/dev`, `/dev/pts` are created as mount points (the runtime provides the mounts).
3. Account files: the image's `passwd`, `group`, `shadow`, `gshadow` stay; if no user has uid 1000, user `fex` (uid/gid 1000, home
   `/home/fex`, shell `/bin/bash`) is added to all four and `/home/fex` is created. A missing file is created with `root` and `fex`.
4. `/etc/resolv.conf` is written from the console's DNS servers (falling back to 1.1.1.1 and 8.8.8.8) with the launcher's marker
   line, replacing the image's link into the missing `/run/systemd/resolve`. `/etc/hosts`, `/etc/hostname` and `/etc/machine-id`
   keep the image's files (created when missing; the machine id is also regenerated when it is shorter than 32 characters).
   `/etc/hosts` gains `127.0.1.1 fex-switch2` when no line names the runtime's host name (`uname -n` is always `fex-switch2`).

Differences from `unbreak_chroot.sh` and `chroot.py unbreak`:

| Script | Launcher |
|---|---|
| mounts `proc`, `sys`, `dev`, `dev/pts` and bind-mounts `/tmp` | creates the mount points only; the runtime mounts its own |
| mounts the host's aarch64 libraries (`lib/aarch64-linux-gnu`, `ld-linux-aarch64.so.1`) | not done: the console has no host libraries |
| copies `FEXInterpreter`, writes `/usr/share/fex-emu/Config.json` and `FEX_SERVERSOCKETPATH` in `/etc/environment` | not done (no FEXServer) |
| `unbreak_chroot.sh` moves `opt` back only if empty; `chroot.py` skips it when `/opt` does not exist | `/opt` is restored (an empty folder in the image) |
| `unbreak_chroot.sh` moves `var/cache/apt` and `var/lib/apt` back; the image has neither | whatever `/chroot` holds is moved back |
| an existing destination makes `mv` nest or overwrite | folders merge; a file that exists on both sides keeps the destination |
| runs `chroot` into the tree | no chroot |
| no account is added | user `fex` (1000) when uid 1000 has no entry |
| `/etc/resolv.conf` stays the image's | rewritten (see above) |
| only `/tmp` is chmod 1777 | same (`/var/tmp` stays as imaged, mode 0755) |

Ownership: everything in the FEX image is owned by uid/gid 1000 (including `/etc/shadow` and `/root`), the user who built it.
The installer re-owns it while extracting: uid 1000 and gid 1000 become 0 (`ExtractHooks::BuilderToRoot`), other ids stay, so the
system tree is root's as on Linux (sudo refuses a `/etc/sudoers.d` it does not own, and then ignores the NOPASSWD rule). Since
0.17.7; an older install keeps uid 1000 until it is reinstalled. `/home/fex` is created afterwards as 1000. The runtime's default
user is 1000 and starts in `/home/fex` (HOME too); root runs as uid 0 through the `.user` sidecar or the Root shell and starts in
`/root`.

## Extra packages

Packages our tests and tools need but the image lacks (checked against the image's dpkg status): `bubblewrap`
(`ContainerTools.sh`), `netcat-openbsd` (`CommTools.sh`: `nc`), `inotify-tools` (`ProcessTools.sh`: `inotifywait`), plus their
missing dependencies (for example `libinotifytools0`). The table `ExtraPackages` in `GuestRootfsInstall.h` is the single list.
`InRelease` of `noble`, `noble-updates` and `noble-security` is read from `http://archive.ubuntu.com/ubuntu`; `main` is scanned
first and `universe` only when a package is not found (about 22 MB of indexes in total). Each `Packages.xz` is verified against the
SHA-256 in `InRelease` and decoded as a stream (never fully in memory); each `.deb` is verified against the size and SHA-256 in
`Packages` and unpacked with the dpkg unpacker (`.tar.xz` and `.tar.zst` members); its control files go to `/var/lib/dpkg/info` and
its status paragraph (`install ok unpacked`) is appended to the image's `status` file. After the writer is closed, the launcher
runs `dpkg --configure -a` in the guest as root (this creates the `nc` alternative and runs `ldconfig`). If that fails, the install
still finishes, with a warning and `extras_configured: false` in `rootfs.json`.

Not done, on purpose: apt lists are not restored (the guest's own `apt-get update` fills them); the `InRelease` signature is not
checked (no GPG implementation), so over plain HTTP the chain Release -> Packages -> `.deb` protects integrity only, not
authenticity.

## Installer pipeline and files

Steps (weights in percent): 0 check free space (1), 1 download (30, skipped for a local image), 2 verify XXH3-64 (6), 3 extract
(40), 4 set up (unbreak and everyday setup, 2), 5 extra packages (20), 6 finish (1). Free space needed: the download size, the
installed size plus 5%, and 64 MiB for the extras. The image is stored as `/fex/.download/<name>.sqsh.part` (resumable with a Range
request, retried 4 times from the received offset, restarted when the server ignores ranges), renamed when its size matches and
removed after a verified install. A local `.sqsh` (the File picker; the text launcher uses the first file of `/fex/images`) is
installed in place and kept. `/fex/config/rootfs.json` is written last and is the only thing that makes a RootFS valid:

```json
{ "installed": true, "name": "Ubuntu 24.04", "image": "Ubuntu_24_04.sqsh", "url": "...", "hash": "3517e0e5ea25a473",
  "hash_kind": "xxh3-64", "verified": true, "installed_at": 1700000000, "extras": ["bubblewrap 0.9.0-1ubuntu0.3"], "extras_configured": true }
```

Cancel (B) or a failure removes the marker, deletes `/fex/rootfs` and `/fex/.inode_store` and the apt scratch files, and (cancel
only) the download. A failed download keeps the `.part` file for the next try; a hash mismatch deletes the image. Reinstall wipes
the existing RootFS first (`WipeGuestRootfs`, also behind Settings > Wipe RootFS). `ui.json` is not written: the settings schema
has no RootFS key.
