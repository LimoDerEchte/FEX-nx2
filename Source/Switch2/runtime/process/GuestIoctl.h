// SPDX-License-Identifier: MIT
// The pure part of the generic ioctls (fs/ioctl.c, drivers/char/random.c): request numbers and the decisions that need
// no runtime state. The runtime reads and writes the guest argument and asks these helpers what to answer.
#pragma once

#include <cstdint>

namespace Switch2Guest::Ioctl {

// include/uapi/asm-generic/ioctls.h and linux/fs.h
inline constexpr uint64_t FIOCLEX = 0x5451, FIONCLEX = 0x5450, FIONBIO = 0x5421, FIOASYNC = 0x5452, FIONREAD = 0x541b, FIOQSIZE = 0x5460,
                          FIGETBSZ = 2, FS_IOC_GETFLAGS = 0x80086601, FS_IOC_SETFLAGS = 0x40086602, FICLONE = 0x40049409,
                          FICLONERANGE = 0x4020940d;
// linux/random.h
inline constexpr uint64_t RNDGETENTCNT = 0x80045200, RNDADDTOENTCNT = 0x40045201, RNDADDENTROPY = 0x40085203, RNDZAPENTCNT = 0x5204,
                          RNDCLEARPOOL = 0x5206;

inline constexpr int64_t Enotty = -25, Eperm = -1, Eopnotsupp = -95, Exdev = -18, Ebadf = -9, Eisdir = -21, Einval = -22;
inline constexpr int32_t RandomEntropyCount = 256; // the poolsize /proc/sys/kernel/random/entropy_avail shows
inline constexpr int32_t BlockSize = 4096;         // FIGETBSZ: the block size of every file system the runtime has

// The inode kinds the generic requests tell apart.
enum class Kind { Regular, Directory, Other };
inline Kind KindOfMode(uint32_t Mode) {
  const uint32_t Type = Mode & 0170000;
  return Type == 0100000 ? Kind::Regular : Type == 0040000 ? Kind::Directory : Kind::Other;
}

// FIONREAD (file_ioctl): regular files report size - position; every other kind goes to the file's own ioctl (no answer
// here, so ENOTTY).
inline bool FionreadApplies(Kind K) { return K == Kind::Regular; }
inline int32_t FionreadValue(int64_t Size, int64_t Position) { return static_cast<int32_t>(Size - Position); }

// FIOQSIZE: the bytes the inode occupies, for regular files and directories only (ENOTTY for the rest).
inline bool FioqsizeApplies(Kind K) { return K != Kind::Other; }

// FS_IOC_GETFLAGS and FS_IOC_SETFLAGS exist on regular files and directories.
inline bool FileFlagsApply(Kind K) { return K != Kind::Other; }
// FS_IOC_SETFLAGS: the file systems the runtime offers keep no inode flags, so 0 is accepted and any other bit is
// EOPNOTSUPP (deliberate difference). Only the owner or CAP_FOWNER may change flags (EPERM).
inline int64_t SetFlagsResult(uint64_t Flags, bool OwnerOrCapable) {
  if (!OwnerOrCapable) return Eperm;
  return Flags == 0 ? 0 : Eopnotsupp;
}

// FICLONE and FICLONERANGE (ioctl_file_clone, vfs_clone_file_range): another mount is EXDEV, a destination not open for
// writing EBADF, a directory EISDIR; same-mount regular files fail with EOPNOTSUPP (no reflink support).
inline int64_t CloneResult(bool SameMount, bool DestWritable, Kind Source, Kind Destination) {
  if (!SameMount) return Exdev;
  if (!DestWritable) return Ebadf;
  if (Source == Kind::Directory || Destination == Kind::Directory) return Eisdir;
  if (Source != Kind::Regular || Destination != Kind::Regular) return Einval;
  return Eopnotsupp;
}

// The random devices' requests. Returns the answer (0 or a negative errno) for a request that needs no argument beyond
// the capability check; RNDGETENTCNT is answered by the runtime with RandomEntropyCount.
inline bool IsRandomPrivileged(uint64_t Request) {
  return Request == RNDADDTOENTCNT || Request == RNDADDENTROPY || Request == RNDZAPENTCNT || Request == RNDCLEARPOOL;
}
inline int64_t RandomPrivilegedResult(bool CapSysAdmin) { return CapSysAdmin ? 0 : Eperm; }

} // namespace Switch2Guest::Ioctl
