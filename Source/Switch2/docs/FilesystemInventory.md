# Milestone 1 filesystem syscall inventory

Numbers are Linux x86-64 ABI numbers. A routed syscall has a Switch 2
dispatcher handler. Semantics and console acceptance are tracked in
[NextMilestone.md](../NextMilestone.md). The inventory has 102 entries:
the historical 98-entry baseline plus the legacy and handle-based calls
reconciled into it (`ustat`, `name_to_handle_at`, `open_by_handle_at`)
and `fchmodat2`.

Every entry is routed and implemented. The [filesystem suite](../tests/guest/FsSuite.c)
and the ABI probe cover each group; both pass on the native reference.
Console acceptance is tracked in the handoff.

## Console verification still required

| Interface | Pending console evidence |
| --- | --- |
| `fcntl`, `flock` | Deadlock reporting, interruption and restart |
| `splice`, `tee`, `vmsplice` | Transfer boundaries, pinning and writeback |
| Ownership, modes and xattrs | Cross-identity, setgid, sticky and `trusted.*` cases (suite run as root) |
| `name_to_handle_at`, `open_by_handle_at` | Root round trip and ESTALE after unlink |
| Rename and sync | Recovery at both journal checkpoints |

## Deliberate differences from a stock kernel

| Interface | Behavior |
| --- | --- |
| `O_DIRECT` | EINVAL at open, as on a filesystem without direct I/O |
| `MFD_HUGETLB` | EINVAL, as on a kernel without hugetlbfs |
| `RWF_NOWAIT` on files | EOPNOTSUPP, as on a filesystem without `FMODE_NOWAIT` |
| `FALLOC_FL_UNSHARE_RANGE`, `FALLOC_FL_WRITE_ZEROES` | EOPNOTSUPP; the backends have no reflinks or zeroing offload |
| File handles | SD inodes only, tmpfs layout, not connectable; handles are valid while the inode is linked. Other filesystems encode `AT_HANDLE_FID` only |
| Mount IDs | Every mount reports ID 1 |
| memfd mappings | A shared mapping allocates its whole range when mapped, not per fault |
| SD file shared mappings | Only read-only descriptors (no PROT_WRITE, PROT_EXEC or MAP_FIXED); writable descriptors return ENODEV. The view is refreshed after every data change to stay coherent; access past EOF reads zero instead of raising SIGBUS |
| procfs | No `/proc/<pid>/fd` magic links yet (procfs is reconciled after milestone 3) |
| Name and path length | Linux limits: NAME_MAX 255, PATH_MAX 4096. Horizon caps the whole SD path at 257 bytes (console 0.12.35, 2002-6003), so longer entries live in the inode store: directories as overflow records, files as managed objects. Names with characters FAT or Horizon reject (control characters and `" * : < > ? \ |`) live in the store the same way (0.13.4; before, they returned EINVAL). Moving a native directory where its tree would not fit converts the tree first; the conversion is per entry and each step survives a reset, but a reset can leave a tree partly converted at its old name. Such renames read the source tree once to measure it |
| `RLIMIT_NOFILE` | Settable and enforced since milestone 2; the hard limit is capped at 1024. See [ProcessInventory.md](ProcessInventory.md#deliberate-differences-from-a-stock-kernel) |
| Clocks and `clock_nanosleep` | Realtime has sub-second resolution from one guest base (RTC second at launch plus elapsed time), which file timestamps share; process CPU clock sleeps and alarm sleeps are implemented. Differences are tracked in [ProcessInventory.md](ProcessInventory.md#clocks-deliberate-differences) |

| Group | Routed | Unrouted |
| --- | ---: | ---: |
| Descriptors and I/O | 27 | 0 |
| Paths and links | 23 | 0 |
| Metadata and permissions | 24 | 0 |
| Allocation and transfer | 12 | 0 |
| Extended attributes | 16 | 0 |

## Descriptors and I/O

| Number | Syscall | Dispatch |
| ---: | --- | --- |
| 0 | `read` | Routed |
| 1 | `write` | Routed |
| 2 | `open` | Routed |
| 3 | `close` | Routed |
| 8 | `lseek` | Routed |
| 16 | `ioctl` | Routed |
| 17 | `pread64` | Routed |
| 18 | `pwrite64` | Routed |
| 19 | `readv` | Routed |
| 20 | `writev` | Routed |
| 32 | `dup` | Routed |
| 33 | `dup2` | Routed |
| 72 | `fcntl` | Routed |
| 73 | `flock` | Routed |
| 74 | `fsync` | Routed |
| 75 | `fdatasync` | Routed |
| 78 | `getdents` | Routed |
| 162 | `sync` | Routed |
| 217 | `getdents64` | Routed |
| 257 | `openat` | Routed |
| 292 | `dup3` | Routed |
| 295 | `preadv` | Routed |
| 296 | `pwritev` | Routed |
| 306 | `syncfs` | Routed |
| 327 | `preadv2` | Routed |
| 328 | `pwritev2` | Routed |
| 436 | `close_range` | Routed |

## Paths and links

| Number | Syscall | Dispatch |
| ---: | --- | --- |
| 79 | `getcwd` | Routed |
| 80 | `chdir` | Routed |
| 81 | `fchdir` | Routed |
| 82 | `rename` | Routed |
| 83 | `mkdir` | Routed |
| 84 | `rmdir` | Routed |
| 85 | `creat` | Routed |
| 86 | `link` | Routed |
| 87 | `unlink` | Routed |
| 88 | `symlink` | Routed |
| 89 | `readlink` | Routed |
| 133 | `mknod` | Routed |
| 258 | `mkdirat` | Routed |
| 259 | `mknodat` | Routed |
| 263 | `unlinkat` | Routed |
| 264 | `renameat` | Routed |
| 265 | `linkat` | Routed |
| 266 | `symlinkat` | Routed |
| 267 | `readlinkat` | Routed |
| 303 | `name_to_handle_at` | Routed |
| 304 | `open_by_handle_at` | Routed |
| 316 | `renameat2` | Routed |
| 437 | `openat2` | Routed |

## Metadata and permissions

| Number | Syscall | Dispatch |
| ---: | --- | --- |
| 4 | `stat` | Routed |
| 5 | `fstat` | Routed |
| 6 | `lstat` | Routed |
| 21 | `access` | Routed |
| 90 | `chmod` | Routed |
| 91 | `fchmod` | Routed |
| 92 | `chown` | Routed |
| 93 | `fchown` | Routed |
| 94 | `lchown` | Routed |
| 95 | `umask` | Routed |
| 132 | `utime` | Routed |
| 136 | `ustat` | Routed |
| 137 | `statfs` | Routed |
| 138 | `fstatfs` | Routed |
| 235 | `utimes` | Routed |
| 260 | `fchownat` | Routed |
| 261 | `futimesat` | Routed |
| 262 | `newfstatat` | Routed |
| 268 | `fchmodat` | Routed |
| 269 | `faccessat` | Routed |
| 280 | `utimensat` | Routed |
| 332 | `statx` | Routed |
| 439 | `faccessat2` | Routed |
| 452 | `fchmodat2` | Routed |

## Allocation and transfer

| Number | Syscall | Dispatch |
| ---: | --- | --- |
| 40 | `sendfile` | Routed |
| 76 | `truncate` | Routed |
| 77 | `ftruncate` | Routed |
| 187 | `readahead` | Routed |
| 221 | `fadvise64` | Routed |
| 275 | `splice` | Routed |
| 276 | `tee` | Routed |
| 277 | `sync_file_range` | Routed |
| 278 | `vmsplice` | Routed |
| 285 | `fallocate` | Routed |
| 319 | `memfd_create` | Routed |
| 326 | `copy_file_range` | Routed |

## Extended attributes

| Number | Syscall | Dispatch |
| ---: | --- | --- |
| 188 | `setxattr` | Routed |
| 189 | `lsetxattr` | Routed |
| 190 | `fsetxattr` | Routed |
| 191 | `getxattr` | Routed |
| 192 | `lgetxattr` | Routed |
| 193 | `fgetxattr` | Routed |
| 194 | `listxattr` | Routed |
| 195 | `llistxattr` | Routed |
| 196 | `flistxattr` | Routed |
| 197 | `removexattr` | Routed |
| 198 | `lremovexattr` | Routed |
| 199 | `fremovexattr` | Routed |
| 463 | `setxattrat` | Routed |
| 464 | `getxattrat` | Routed |
| 465 | `listxattrat` | Routed |
| 466 | `removexattrat` | Routed |
