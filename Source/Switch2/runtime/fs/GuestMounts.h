// SPDX-License-Identifier: MIT
#pragma once

// Pure logic of Linux mount namespaces (Milestone 4, task 3): mount flags and filesystem options, the mount
// table of one mount namespace (mount ids, parents, propagation state, locked flags), the decisions of mount(2)
// (bind, new mount, remount, move, propagation changes), umount2(2) and pivot_root(2), and the text of
// /proc/<pid>/mountinfo and /proc/<pid>/mounts. The runtime resolves paths (GuestRuntime.cpp: ResolveSdPath),
// holds one Table per mount namespace and calls these functions with resolved (mount, host path) pairs; this header
// is tested on the host (tests/unit/GuestMountsTest.cpp). The reference is fs/namespace.c and mm/shmem.c of Linux
// 7.3, checked against native runs under `unshare -rm` (see ContainerInventory.md).
//
// A position is a mount plus a host path (the path the file layer uses) inside that mount: Root is the host path of
// the mount's root, Mountpoint the host path of the covered directory in the parent mount. Bind mounts therefore
// share their source's host paths and so its inodes.

#include "GuestNamespaces.h"

#include <algorithm>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Switch2Guest::Mounts {
using Switch2Guest::Ns::ErrInval;
using Switch2Guest::Ns::ErrPerm;
constexpr int64_t ErrNoEnt = -2, ErrBusy = -16, ErrNotDir = -20, ErrAgain = -11, ErrLoop = -40, ErrNoDev = -19,
                  ErrRoFs = -30, ErrAcces = -13, ErrNoSpc = -28;

// mount(2) flags.
constexpr uint64_t MsRdonly = 1, MsNosuid = 2, MsNodev = 4, MsNoexec = 8, MsSynchronous = 16, MsRemount = 32,
                   MsMandlock = 64, MsDirsync = 128, MsNosymfollow = 256, MsNoatime = 1024, MsNodiratime = 2048,
                   MsBind = 4096, MsMove = 8192, MsRec = 16384, MsSilent = 32768, MsPosixacl = 1u << 16,
                   MsUnbindable = 1u << 17, MsPrivate = 1u << 18, MsSlave = 1u << 19, MsShared = 1u << 20,
                   MsRelatime = 1u << 21, MsKernmount = 1u << 22, MsIVersion = 1u << 23, MsStrictatime = 1u << 24,
                   MsLazytime = 1u << 25, MsNouser = 1u << 31, MsMgcVal = 0xC0ED0000u, MsMgcMsk = 0xffff0000u;
constexpr uint64_t MsPropagation = MsShared | MsPrivate | MsSlave | MsUnbindable;
// umount2 flags.
constexpr uint32_t MntForce = 1, MntDetach = 2, MntExpire = 4, UmountNofollow = 8;

// Per-mount flags (MNT_*), the ones mountinfo field 6 prints and remount with MS_BIND changes.
constexpr uint32_t MntNosuid = 1, MntNodev = 2, MntNoexec = 4, MntNoatime = 8, MntNodiratime = 16, MntRelatime = 32,
                   MntReadonly = 64, MntNosymfollow = 128;
constexpr uint32_t MntUserSettable = 0xff;
// Flags a mount namespace copy locked (MNT_LOCK_*): an unprivileged owner may not clear them.
constexpr uint32_t LockAtime = 1, LockReadonly = 2, LockNodev = 4, LockNosuid = 8, LockNoexec = 16;
// Superblock flags that mountinfo and /proc/mounts print.
constexpr uint32_t SbSynchronous = 1, SbDirsync = 2, SbMandlock = 4, SbLazytime = 8;

// ======================= superblocks and mounts =======================

struct Superblock {
  uint64_t Dev {};          // st_dev minor of an anonymous device (major 0); 0 keeps the file layer's own number
  std::string Type;         // "tmpfs", "proc", ...
  std::string Source;       // the device name shown in mountinfo
  std::string FsRoot;       // host path of the filesystem's root
  bool ReadOnly {};         // SB_RDONLY
  uint32_t Flags {};        // SbSynchronous | SbDirsync | SbMandlock | SbLazytime
  bool Pseudo {};           // creations fail with EPERM (sysfs, devpts, mqueue: empty here)
  // tmpfs and ramfs options; 0 means the default (no limit, mode 01777)
  uint64_t SizeBytes {}, NrInodes {};
  uint32_t Mode {01777}, Uid {}, Gid {};
  std::string Extra;        // other options shown as given (proc hidepid, devpts mode)
  std::shared_ptr<Ns::UserNs> Owner;  // s_user_ns: who may reconfigure it
  std::shared_ptr<Ns::PidNs> Pid;     // a proc superblock shows the processes as this PID namespace numbers them
  std::shared_ptr<void> Cleanup;      // the runtime hangs what must happen when the last user is gone here
  bool Memory() const { return Type == "tmpfs" || Type == "ramfs"; }
};

struct Mount {
  uint64_t Id {};
  std::shared_ptr<Mount> Parent;  // null for the table's top mount and for a detached mount
  std::string Mountpoint;         // host path of the covered directory, in the parent's coordinates
  std::string Root;               // host path of this mount's root
  std::shared_ptr<Superblock> Sb;
  uint32_t Flags {};              // MNT_*
  uint32_t Locks {};              // LOCK_*
  bool Locked {};                 // MNT_LOCKED: cannot be unmounted or moved (it hides what is below)
  uint64_t Group {};              // peer group: "shared:N"
  uint64_t Master {};             // "master:N" (slave of group N)
  bool Unbindable {};
  bool ExpiryMark {};

  bool ReadOnly() const { return (Flags & MntReadonly) || Sb->ReadOnly; }
};

using MountRef = std::shared_ptr<Mount>;

// calculate_f_flags: the mount's flags as statfs f_flags (ST_RDONLY 1, ST_NOSUID 2, ST_NODEV 4, ST_NOEXEC 8,
// ST_NOATIME 0x400, ST_NODIRATIME 0x800, ST_RELATIME 0x1000), for whatever file system the mount shows.
inline int64_t StatfsFlags(const Mount& M) {
  int64_t Flags {};
  if (M.ReadOnly()) Flags |= 1;
  if (M.Flags & MntNosuid) Flags |= 2;
  if (M.Flags & MntNodev) Flags |= 4;
  if (M.Flags & MntNoexec) Flags |= 8;
  if (M.Flags & MntNoatime) Flags |= 0x400;
  if (M.Flags & MntNodiratime) Flags |= 0x800;
  if (M.Flags & MntRelatime) Flags |= 0x1000;
  return Flags;
}

// A place: a mount and a host path inside it.
struct Place {
  MountRef M;
  std::string Path;
};

struct Ids {
  uint64_t NextMount {1};
  uint64_t NextDev {40};
  uint64_t NextGroup {1};
};

inline bool PathContains(std::string_view Parent, std::string_view Entry) {
  return Entry == Parent || (Entry.size() > Parent.size() && Entry.starts_with(Parent) && Entry[Parent.size()] == '/');
}

struct Table {
  std::vector<MountRef> List;  // every mount of the namespace, creation order (a parent before its children)

  bool Has(const Mount* M) const {
    return std::any_of(List.begin(), List.end(), [&](const MountRef& X) { return X.get() == M; });
  }
  MountRef Top() const { return List.empty() ? nullptr : List.front(); }
  // The topmost mount covering Path of Parent: the last one mounted there.
  MountRef ChildAt(const Mount* Parent, std::string_view Path) const {
    for (auto It = List.rbegin(); It != List.rend(); ++It) {
      if ((*It)->Parent.get() == Parent && (*It)->Mountpoint == Path) return *It;
    }
    return nullptr;
  }
  bool HasChildren(const Mount* M) const {
    return std::any_of(List.begin(), List.end(), [&](const MountRef& X) { return X->Parent.get() == M; });
  }
  // The mounts whose parent chain leads to Root, Root first.
  std::vector<MountRef> Subtree(const MountRef& Root) const {
    std::vector<MountRef> Out;
    for (const auto& X : List) {
      for (const Mount* P = X.get(); P; P = P->Parent.get()) {
        if (P == Root.get()) {
          Out.push_back(X);
          break;
        }
      }
    }
    return Out;
  }
};

// The mount reached when Path of mount M is stepped onto, following covering mounts: (M, Path) when nothing
// covers it. Stacked mounts are followed to the top.
inline Place Cross(const Table& T, Place At, bool* Crossed = nullptr) {
  for (;;) {
    auto Over = T.ChildAt(At.M.get(), At.Path);
    if (!Over) return At;
    if (Crossed) *Crossed = true;
    Over->ExpiryMark = false;
    At = {Over, Over->Root};
  }
}

// The path of position (M, Path) as seen from root place Root, or nullopt when Root is not above it. A position
// outside the table (a detached mount) is unreachable.
inline std::optional<std::string> VisiblePath(const Place& Root, const Mount* M, std::string_view Path) {
  std::string Below;  // the components under Path that deeper mounts contributed
  for (;;) {
    if (M == Root.M.get() && PathContains(Root.Path, Path)) {
      const std::string Full = std::string {Path.substr(Root.Path.size())} + Below;
      return Full.empty() ? std::string {"/"} : Full;
    }
    if (!M->Parent || !PathContains(M->Root, Path)) return std::nullopt;
    Below = std::string {Path.substr(M->Root.size())} + Below;
    Path = M->Mountpoint;
    M = M->Parent.get();
  }
}

// The path to show for something unreachable from the root, as getcwd does: "(unreachable)" and the path inside
// the topmost mount it can climb to.
inline std::string UnreachablePath(const Mount* M, std::string_view Path) {
  std::string Tail;
  for (;;) {
    if (PathContains(M->Root, Path)) Tail = std::string {Path.substr(M->Root.size())} + Tail;
    if (!M->Parent) break;
    Path = M->Mountpoint;
    M = M->Parent.get();
  }
  return "(unreachable)" + (Tail.empty() ? std::string {"/"} : Tail);
}

// ======================= filesystem options =======================

struct FsOptions {
  uint64_t SizeBytes {};  // tmpfs size (0 default)
  uint64_t NrInodes {};
  std::optional<uint32_t> Mode, Uid, Gid;
  std::string Extra;
};

inline std::optional<uint64_t> ParseNumber(std::string_view Text, unsigned Base = 10) {
  if (Text.empty()) return std::nullopt;
  uint64_t Value {};
  for (const char C : Text) {
    unsigned Digit;
    if (C >= '0' && C <= '9') Digit = C - '0';
    else if (Base == 16 && C >= 'a' && C <= 'f') Digit = C - 'a' + 10;
    else if (Base == 16 && C >= 'A' && C <= 'F') Digit = C - 'A' + 10;
    else return std::nullopt;
    if (Digit >= Base) return std::nullopt;
    if (Value > (UINT64_MAX - Digit) / Base) return std::nullopt;
    Value = Value * Base + Digit;
  }
  return Value;
}

// memparse: a number with an optional K, M or G suffix (powers of 1024).
inline std::optional<uint64_t> MemParse(std::string_view Text) {
  if (Text.empty()) return std::nullopt;
  unsigned Shift = 0;
  switch (Text.back()) {
  case 'k': case 'K': Shift = 10; break;
  case 'm': case 'M': Shift = 20; break;
  case 'g': case 'G': Shift = 30; break;
  default: break;
  }
  if (Shift) Text.remove_suffix(1);
  const auto Value = ParseNumber(Text);
  if (!Value || *Value > (UINT64_MAX >> Shift)) return std::nullopt;
  return *Value << Shift;
}

inline constexpr uint64_t PageSize = 4096;

// The data string of mount(2) for Type. MemTotal is the machine's memory (for size=N%). Mapping: ids are kernel
// ids already. Returns the options, or a negated errno (EINVAL for anything the filesystem rejects).
inline std::variant<int64_t, FsOptions> ParseFsOptions(std::string_view Type, std::string_view Data, uint64_t MemTotal) {
  FsOptions Out;
  while (!Data.empty()) {
    const auto Comma = Data.find(',');
    const auto Item = Data.substr(0, Comma);
    Data = Comma == Data.npos ? std::string_view {} : Data.substr(Comma + 1);
    if (Item.empty()) continue;
    const auto Equals = Item.find('=');
    const auto Key = Item.substr(0, Equals);
    const auto Value = Equals == Item.npos ? std::string_view {} : Item.substr(Equals + 1);
    const bool HasValue = Equals != Item.npos;
    if (Type == "tmpfs") {
      if (Key == "size" && HasValue) {
        if (Value.ends_with('%')) {
          const auto Percent = ParseNumber(Value.substr(0, Value.size() - 1));
          if (!Percent || *Percent > 100) return ErrInval;
          Out.SizeBytes = MemTotal / 100 * *Percent;
        } else if (const auto Bytes = MemParse(Value)) Out.SizeBytes = *Bytes;
        else return ErrInval;
        Out.SizeBytes = (Out.SizeBytes + PageSize - 1) / PageSize * PageSize;
      } else if (Key == "nr_blocks" && HasValue) {
        const auto Blocks = MemParse(Value);
        if (!Blocks || *Blocks > UINT64_MAX / PageSize) return ErrInval;
        Out.SizeBytes = *Blocks * PageSize;
      } else if (Key == "nr_inodes" && HasValue) {
        const auto Count = MemParse(Value);
        if (!Count) return ErrInval;
        Out.NrInodes = *Count;
      } else if (Key == "mode" && HasValue) {
        const auto Mode = ParseNumber(Value, 8);
        if (!Mode || *Mode & ~07777ULL) return ErrInval;
        Out.Mode = static_cast<uint32_t>(*Mode);
      } else if ((Key == "uid" || Key == "gid") && HasValue) {
        const auto Id = ParseNumber(Value);
        if (!Id || *Id >= 0xffffffffULL) return ErrInval;
        (Key == "uid" ? Out.Uid : Out.Gid) = static_cast<uint32_t>(*Id);
      } else if (Key == "huge" && HasValue) {
        if (Value != "never" && Value != "always" && Value != "within_size" && Value != "advise" && Value != "deny" && Value != "force")
          return ErrInval;
      } else if (Key == "mpol" && HasValue) {
        if (Value != "default" && Value != "local") return ErrInval;
      } else if ((Key == "inode32" || Key == "inode64" || Key == "noswap") && !HasValue) {
      } else return ErrInval;
    } else if (Type == "ramfs") {
      if (Key == "mode" && HasValue) {
        const auto Mode = ParseNumber(Value, 8);
        if (!Mode || *Mode & ~07777ULL) return ErrInval;
        Out.Mode = static_cast<uint32_t>(*Mode);
      } else return ErrInval;
    } else if (Type == "proc") {
      if (Key == "hidepid" && HasValue) {
        unsigned Level;
        if (Value == "off" || Value == "0") Level = 0;
        else if (Value == "noaccess" || Value == "1") Level = 1;
        else if (Value == "invisible" || Value == "2") Level = 2;
        else if (Value == "ptraceable" || Value == "4") Level = 4;
        else return ErrInval;
        if (Level) Out.Extra += ",hidepid=" + std::to_string(Level);
      } else if (Key == "gid" && HasValue) {
        const auto Id = ParseNumber(Value);
        if (!Id || *Id >= 0xffffffffULL) return ErrInval;
        Out.Gid = static_cast<uint32_t>(*Id);
        if (*Id) Out.Extra += ",gid=" + std::string {Value};
      } else if (Key == "subset" && HasValue && Value == "pid") {
        Out.Extra += ",subset=pid";
      } else return ErrInval;
    } else if (Type == "devpts") {
      if ((Key == "newinstance") && !HasValue) {
      } else if ((Key == "mode" || Key == "ptmxmode") && HasValue) {
        const auto Mode = ParseNumber(Value, 8);
        if (!Mode || *Mode & ~0777ULL) return ErrInval;
        if (Key == "mode") Out.Mode = static_cast<uint32_t>(*Mode);
      } else if ((Key == "uid" || Key == "gid" || Key == "max") && HasValue) {
        const auto Id = ParseNumber(Value);
        if (!Id || *Id >= 0xffffffffULL) return ErrInval;
        if (Key == "uid") Out.Uid = static_cast<uint32_t>(*Id);
        if (Key == "gid") Out.Gid = static_cast<uint32_t>(*Id);
      } else return ErrInval;
    } else {
      return ErrInval;  // sysfs and mqueue take no options
    }
  }
  return Out;
}

// The filesystems mount(2) can create. Others are ENODEV (a kernel built without them, or one that needs a block
// device).
inline bool KnownFsType(std::string_view Type) {
  return Type == "tmpfs" || Type == "ramfs" || Type == "proc" || Type == "sysfs" || Type == "devpts" || Type == "mqueue";
}

// ======================= mount flags =======================

struct MountFlags {
  uint32_t Mnt {};       // MNT_*
  uint32_t SbFlags {};   // SbSynchronous ...
  bool SbReadOnly {};
};

// path_mount: the per-mount and superblock flags a mount(2) flags word asks for (relatime unless noatime).
inline MountFlags FlagsFromMs(uint64_t Ms) {
  MountFlags F;
  if (!(Ms & MsNoatime)) F.Mnt |= MntRelatime;
  if (Ms & MsNosuid) F.Mnt |= MntNosuid;
  if (Ms & MsNodev) F.Mnt |= MntNodev;
  if (Ms & MsNoexec) F.Mnt |= MntNoexec;
  if (Ms & MsNoatime) F.Mnt |= MntNoatime;
  if (Ms & MsNodiratime) F.Mnt |= MntNodiratime;
  if (Ms & MsStrictatime) F.Mnt &= ~(MntRelatime | MntNoatime);
  if (Ms & MsRdonly) F.Mnt |= MntReadonly;
  if (Ms & MsNosymfollow) F.Mnt |= MntNosymfollow;
  F.SbReadOnly = Ms & MsRdonly;
  if (Ms & MsSynchronous) F.SbFlags |= SbSynchronous;
  if (Ms & MsDirsync) F.SbFlags |= SbDirsync;
  if (Ms & MsMandlock) F.SbFlags |= SbMandlock;
  if (Ms & MsLazytime) F.SbFlags |= SbLazytime;
  return F;
}

// can_change_locked_flags: whether a remount to Wanted keeps every locked flag.
inline bool CanChangeLocked(const Mount& M, uint32_t Wanted) {
  const uint32_t Have = M.Flags;
  if ((M.Locks & LockReadonly) && !(Wanted & MntReadonly)) return false;
  if ((M.Locks & LockNodev) && !(Wanted & MntNodev)) return false;
  if ((M.Locks & LockNosuid) && !(Wanted & MntNosuid)) return false;
  if ((M.Locks & LockNoexec) && !(Wanted & MntNoexec)) return false;
  constexpr uint32_t Atime = MntNoatime | MntNodiratime | MntRelatime;
  if ((M.Locks & LockAtime) && (Have & Atime) != (Wanted & Atime)) return false;
  return true;
}

// ======================= table operations =======================

// The capabilities the caller holds, as the checks need them.
struct Caller {
  Ns::Subject Who;
};

inline bool CapableOver(const Caller& C, const Ns::UserNs& Target, uint32_t Cap) { return Ns::NsCapable(C.Who, Target, Cap); }

inline MountRef MakeMount(Table& T, Ids& I, MountRef Parent, std::string Mountpoint, std::string Root, std::shared_ptr<Superblock> Sb, uint32_t Flags) {
  auto M = std::make_shared<Mount>();
  M->Id = I.NextMount++;
  M->Parent = std::move(Parent);
  M->Mountpoint = std::move(Mountpoint);
  M->Root = std::move(Root);
  M->Sb = std::move(Sb);
  M->Flags = Flags;
  T.List.push_back(M);
  return M;
}

// A copy of source mount X attached below Parent at Mountpoint (clone_mnt): a peer of a shared source, a slave of
// a slave, locks kept, the locked mark dropped.
inline MountRef CloneMount(Table& T, Ids& I, const Mount& X, MountRef Parent, std::string Mountpoint, std::string Root) {
  auto M = MakeMount(T, I, std::move(Parent), std::move(Mountpoint), std::move(Root), X.Sb, X.Flags);
  M->Locks = X.Locks;
  M->Group = X.Group;
  M->Master = X.Master;
  return M;
}

// ---- mount(2) with MS_BIND (do_loopback). Source and Target are resolved places; SrcDir/DstDir say whether each is
// a directory. Returns 0 or -errno; Out receives the new top mount.
inline int64_t Bind(Table& T, Ids& I, const Place& Source, bool SrcDir, const Place& Target, bool DstDir, bool Recursive, MountRef* Out = nullptr) {
  if (!T.Has(Target.M.get())) return ErrInval;                    // check_mnt(parent)
  if (Source.M->Unbindable) return ErrInval;
  if (!T.Has(Source.M.get())) return ErrInval;
  if (SrcDir != DstDir) return ErrNotDir;                        // graft_tree
  // Every mount covering a directory inside the source subtree is cloned too, except unbindable ones. The
  // subtree is taken before the new mount exists (the target may lie inside the source).
  const auto Tree = Recursive ? T.Subtree(Source.M) : std::vector<MountRef> {};
  auto Top = CloneMount(T, I, *Source.M, Target.M, Target.Path, Source.Path);
  if (Out) *Out = Top;
  std::vector<std::pair<MountRef, MountRef>> Map {{Source.M, Top}};
  for (const auto& X : Tree) {
    if (X == Source.M || X->Unbindable) continue;
    const auto Parent = std::find_if(Map.begin(), Map.end(), [&](const auto& P) { return P.first == X->Parent; });
    if (Parent == Map.end()) continue;
    if (X->Parent == Source.M && !PathContains(Source.Path, X->Mountpoint)) continue;
    Map.emplace_back(X, CloneMount(T, I, *X, Parent->second, X->Mountpoint, X->Root));
  }
  return 0;
}

// ---- mount(2) creating a new filesystem (do_new_mount). Sb is built by the caller; Target is the directory.
inline int64_t NewMount(Table& T, Ids& I, const Place& Target, bool DstDir, std::shared_ptr<Superblock> Sb, uint32_t MntFlags, MountRef* Out = nullptr) {
  if (!T.Has(Target.M.get())) return ErrInval;
  if (!DstDir) return ErrNotDir;
  auto M = MakeMount(T, I, Target.M, Target.Path, Sb->FsRoot, Sb, MntFlags);
  if (Out) *Out = M;
  return 0;
}

// The superblock a mount(2) of Type creates: Source is the device string (null becomes "none"), Opts the parsed
// data, Flags the mount flags. Owner is the user namespace that owns it (current's, or the pid, ipc or net
// namespace owner for proc, mqueue and sysfs).
inline std::shared_ptr<Superblock> MakeSuperblock(Ids& I, std::string_view Type, std::string_view Source, const FsOptions& Opts,
                                                  const MountFlags& Flags, std::shared_ptr<Ns::UserNs> Owner, std::string FsRoot,
                                                  uint32_t FsUid, uint32_t FsGid) {
  auto Sb = std::make_shared<Superblock>();
  Sb->Dev = I.NextDev++;
  Sb->Type = std::string {Type};
  Sb->Source = Source.empty() ? "none" : std::string {Source};
  Sb->FsRoot = std::move(FsRoot);
  Sb->ReadOnly = Flags.SbReadOnly;
  Sb->Flags = Flags.SbFlags;
  Sb->Owner = std::move(Owner);
  Sb->Pseudo = Type == "sysfs" || Type == "devpts" || Type == "mqueue";
  Sb->SizeBytes = Opts.SizeBytes;
  Sb->NrInodes = Opts.NrInodes;
  Sb->Mode = Opts.Mode.value_or(Type == "devpts" ? 0755 : 01777);
  Sb->Uid = Opts.Uid.value_or(FsUid);
  Sb->Gid = Opts.Gid.value_or(FsGid);
  Sb->Extra = Opts.Extra;
  return Sb;
}

// ---- mount(2) with MS_REMOUNT (do_remount). Place is the resolved mount root.
//  Bind: MS_BIND|MS_REMOUNT changes the per-mount flags only (do_reconfigure_mnt); otherwise the superblock is
//  reconfigured (needs CAP_SYS_ADMIN over its owner) with the options and ro state, and per-mount flags are set too.
// Writers: files open for writing on the mount (bind) or on the superblock (ro remount); they make ro EBUSY.
struct RemountArgs {
  bool Bind {};
  uint64_t Ms {};
  std::optional<FsOptions> Opts;  // data given (parsed) for a superblock remount
  bool SbCapable {};              // CAP_SYS_ADMIN over the superblock's user namespace
  bool Writers {};                // a file is open for writing on the mount (bind) or filesystem
  int64_t DataError {};           // the error parsing the data string (reported after the locked-flag check)
};
inline int64_t Remount(Table& T, const Place& At, const RemountArgs& A) {
  if (!T.Has(At.M.get())) return ErrInval;
  if (At.Path != At.M->Root) return ErrInval;                     // not a mount point
  const auto F = FlagsFromMs(A.Ms);
  Mount& M = *At.M;
  if (!CanChangeLocked(M, F.Mnt)) return ErrPerm;
  if (A.Bind) {
    if ((F.Mnt & MntReadonly) && !(M.Flags & MntReadonly) && A.Writers) return ErrBusy;
    M.Flags = (M.Flags & ~MntUserSettable) | (F.Mnt & MntUserSettable);
    return 0;
  }
  if (A.DataError) return A.DataError;
  if (!A.SbCapable) return ErrPerm;
  auto& Sb = *M.Sb;
  if (F.SbReadOnly && !Sb.ReadOnly && A.Writers) return ErrBusy;
  Sb.ReadOnly = F.SbReadOnly;
  Sb.Flags = F.SbFlags;
  if (A.Opts && Sb.Memory()) {
    if (A.Opts->SizeBytes) Sb.SizeBytes = A.Opts->SizeBytes;
    if (A.Opts->NrInodes) Sb.NrInodes = A.Opts->NrInodes;
  }
  M.Flags = (M.Flags & ~MntUserSettable) | (F.Mnt & MntUserSettable);
  if (F.SbReadOnly) M.Flags |= MntReadonly;
  else M.Flags &= ~MntReadonly;
  return 0;
}

// ---- mount(2) with MS_SHARED, MS_PRIVATE, MS_SLAVE or MS_UNBINDABLE (do_change_type). Exactly one flag.
inline int64_t ChangeType(Table& T, Ids& I, const Place& At, uint64_t Ms, bool Recursive) {
  const uint64_t Type = Ms & MsPropagation;
  if (!Type || (Type & (Type - 1))) return ErrInval;
  if (!T.Has(At.M.get()) || At.Path != At.M->Root) return ErrInval;
  std::vector<MountRef> Targets;
  if (Recursive) Targets = T.Subtree(At.M);
  else Targets.push_back(At.M);
  for (const auto& M : Targets) {
    switch (Type) {
    case MsShared:
      if (!M->Group) M->Group = I.NextGroup++;
      M->Unbindable = false;
      break;
    case MsPrivate:
      M->Group = 0;
      M->Master = 0;
      M->Unbindable = false;
      break;
    case MsSlave:
      if (M->Group) {
        // a slave of its former peers; alone in the group it is simply private
        const bool Peers = std::any_of(T.List.begin(), T.List.end(), [&](const MountRef& X) { return X != M && X->Group == M->Group; });
        if (Peers) M->Master = M->Group;
        M->Group = 0;
      }
      M->Unbindable = false;
      break;
    case MsUnbindable:
      M->Group = 0;
      M->Master = 0;
      M->Unbindable = true;
      break;
    default: break;
    }
  }
  return 0;
}

// ---- mount(2) with MS_MOVE (do_move_mount). Source is the old place (must be a mount root), Target the new
// mountpoint. SrcDir and DstDir say whether each is a directory.
inline int64_t Move(Table& T, const Place& Source, bool SrcDir, const Place& Target, bool DstDir) {
  Mount& Old = *Source.M;
  if (!T.Has(Target.M.get()) || !T.Has(&Old)) return ErrInval;
  if (Source.Path != Old.Root) return ErrInval;
  if (!Old.Parent) return ErrInval;                               // the top of the namespace
  if (Old.Locked) return ErrInval;
  if (SrcDir != DstDir) return ErrInval;
  if (Old.Parent->Group) return ErrInval;                         // moving out of a shared mount
  if (Target.M->Group) {
    for (const auto& X : T.Subtree(Source.M)) {
      if (X->Unbindable) return ErrInval;
    }
  }
  for (const Mount* P = Target.M.get(); P; P = P->Parent.get()) {
    if (P == &Old) return ErrLoop;                                // into itself or below
  }
  Old.Parent = Target.M;
  Old.Mountpoint = Target.Path;
  return 0;
}

// ---- umount2 (path_umount and do_umount). At is the resolved place; Busy says whether the mount has users other
// than the table (an open file, a working directory or root, a child mount). CapableForce: CAP_SYS_ADMIN over the
// superblock's owner. IsRoot: the caller's root mount. Removed gets the mounts taken out of the table.
struct UmountArgs {
  uint32_t Flags {};
  bool Capable {};         // CAP_SYS_ADMIN over the mount namespace's owner
  bool CapableForce {};
  bool IsRoot {};          // At.M is the caller's root mount
};
// Users of M: callable per mount; the caller answers it from its own state (files, working directories).
template <typename BusyFn>
inline int64_t Umount(Table& T, const Place& At, const UmountArgs& A, BusyFn&& Busy, std::vector<MountRef>* Removed = nullptr) {
  if (A.Flags & ~(MntForce | MntDetach | MntExpire | UmountNofollow)) return ErrInval;
  if (!A.Capable) return ErrPerm;
  Mount& M = *At.M;
  if (At.Path != M.Root) return ErrInval;
  if (!T.Has(&M)) return ErrInval;
  if (M.Locked) return ErrInval;
  if ((A.Flags & MntForce) && !A.CapableForce) return ErrPerm;
  const auto Children = [&](const MountRef& X) {
    std::vector<MountRef> Out;
    for (const auto& Y : T.List) {
      if (Y->Parent == X) Out.push_back(Y);
    }
    return Out;
  };
  const auto Detach = [&](const MountRef& X) {
    for (const auto& Y : T.Subtree(X)) {
      if (Removed) Removed->push_back(Y);
    }
    const auto Tree = T.Subtree(X);
    std::erase_if(T.List, [&](const MountRef& Y) { return std::find(Tree.begin(), Tree.end(), Y) != Tree.end(); });
    X->Parent = nullptr;
  };
  const MountRef Self = At.M;
  if (A.Flags & MntExpire) {
    if (A.IsRoot || (A.Flags & (MntForce | MntDetach))) return ErrInval;
    if (!Children(Self).empty() || Busy(Self)) return ErrBusy;
    if (!M.ExpiryMark) {
      M.ExpiryMark = true;
      return ErrAgain;
    }
  }
  if (A.Flags & MntDetach) {
    Detach(Self);
    return 0;
  }
  // shrink_submounts: expirable (marked) children that are not busy go first.
  for (const auto& C : Children(Self)) {
    if (C->ExpiryMark && Children(C).empty() && !Busy(C)) Detach(C);
  }
  if (A.IsRoot || !Children(Self).empty() || Busy(Self)) return ErrBusy;
  Detach(Self);
  return 0;
}

// ---- pivot_root (the body of pivot_root(2) after both paths were looked up as directories).
struct PivotResult {
  MountRef OldRoot, NewRoot;  // tasks whose root or working directory is OldRoot's root move to NewRoot's
};
inline std::variant<int64_t, PivotResult> PivotRoot(Table& T, const Place& Root, const Place& New, const Place& Old, bool Capable) {
  if (!Capable) return ErrPerm;
  Mount& RootM = *Root.M;
  Mount& NewM = *New.M;
  Mount& OldM = *Old.M;
  const auto Shared = [](const Mount* M) { return M && M->Group != 0; };
  if (Shared(&OldM) || Shared(NewM.Parent.get()) || Shared(RootM.Parent.get())) return ErrInval;
  if (!T.Has(&RootM) || !T.Has(&NewM)) return ErrInval;
  if (NewM.Locked) return ErrInval;
  if (&NewM == &RootM || &OldM == &RootM) return ErrBusy;
  if (Root.Path != RootM.Root) return ErrInval;                   // the root is not a mount point
  if (!RootM.Parent) return ErrInval;
  if (New.Path != NewM.Root) return ErrInval;
  if (!NewM.Parent) return ErrInval;
  // put_old must lie under new_root
  {
    const Mount* M = &OldM;
    std::string_view P = Old.Path;
    while (M != &NewM && M->Parent) {
      P = M->Mountpoint;
      M = M->Parent.get();
    }
    if (M != &NewM || !PathContains(New.Path, P)) return ErrInval;
  }
  // new_root must lie under the current root
  {
    const Mount* M = &NewM;
    std::string_view P = New.Path;
    while (M != &RootM && M->Parent) {
      P = M->Mountpoint;
      M = M->Parent.get();
    }
    if (M != &RootM || !PathContains(Root.Path, P)) return ErrInval;
  }
  const auto RootParent = RootM.Parent;
  const auto RootMountpoint = RootM.Mountpoint;
  if (RootM.Locked) {
    NewM.Locked = true;
    RootM.Locked = false;
  }
  RootM.Parent = Old.M;                                           // old root onto put_old
  RootM.Mountpoint = Old.Path;
  NewM.Parent = RootParent;                                       // new root where the old one was
  NewM.Mountpoint = RootMountpoint;
  return PivotResult {Root.M, New.M};
}

// ---- copy_mnt_ns: a copy of the table for a new mount namespace. Unprivileged: the new namespace is owned by a
// user namespace other than the old one's (the copies lock what a less privileged owner must not see through or
// relax, and shared mounts become slaves). Map receives old mount -> new mount.
inline std::shared_ptr<Table> CopyTable(const Table& Old, Ids& I, bool Unprivileged, std::vector<std::pair<MountRef, MountRef>>* Map = nullptr) {
  auto New = std::make_shared<Table>();
  std::vector<std::pair<MountRef, MountRef>> Local;
  auto& Pairs = Map ? *Map : Local;
  // A mount is copied after its parent: move_mount and pivot_root re-parent a mount below one made later in the
  // list, so the list order alone is not enough. The list order is kept wherever the parent is already copied.
  const auto Find = [&](const MountRef& X) -> MountRef {
    const auto P = std::find_if(Pairs.begin(), Pairs.end(), [&](const auto& E) { return E.first == X; });
    return P == Pairs.end() ? nullptr : P->second;
  };
  const auto InList = [&](const MountRef& X) { return std::find(Old.List.begin(), Old.List.end(), X) != Old.List.end(); };
  std::function<void(const MountRef&)> Copy = [&](const MountRef& X) {
    if (Find(X)) return;
    if (X->Parent && InList(X->Parent)) Copy(X->Parent);
    const MountRef Parent = X->Parent ? Find(X->Parent) : nullptr;
    auto M = CloneMount(*New, I, *X, Parent, X->Mountpoint, X->Root);
    M->Unbindable = X->Unbindable;
    M->Locked = X->Locked;
    if (Unprivileged) {
      M->Locks |= LockAtime;
      if (M->Flags & MntReadonly) M->Locks |= LockReadonly;
      if (M->Flags & MntNodev) M->Locks |= LockNodev;
      if (M->Flags & MntNosuid) M->Locks |= LockNosuid;
      if (M->Flags & MntNoexec) M->Locks |= LockNoexec;
      if (Parent) M->Locked = true;
      if (M->Group) {                                             // CL_SHARED_TO_SLAVE
        M->Master = M->Group;
        M->Group = 0;
      }
    }
    Pairs.emplace_back(X, M);
  };
  for (const auto& X : Old.List) Copy(X);
  return New;
}

// ======================= text =======================

inline std::string Escape(std::string_view Text) {
  std::string Out;
  for (const char C : Text) {
    if (C == ' ' || C == '\t' || C == '\n' || C == '\\') {
      char Buffer[8];
      std::snprintf(Buffer, sizeof(Buffer), "\\%03o", static_cast<unsigned char>(C));
      Out += Buffer;
    } else {
      Out += C;
    }
  }
  return Out;
}

inline std::string MountOptions(const Mount& M) {
  std::string Out = (M.Flags & MntReadonly) ? "ro" : "rw";
  if (M.Flags & MntNosuid) Out += ",nosuid";
  if (M.Flags & MntNodev) Out += ",nodev";
  if (M.Flags & MntNoexec) Out += ",noexec";
  if (M.Flags & MntNoatime) Out += ",noatime";
  if (M.Flags & MntNodiratime) Out += ",nodiratime";
  if (M.Flags & MntRelatime) Out += ",relatime";
  if (M.Flags & MntNosymfollow) Out += ",nosymfollow";
  return Out;
}

inline std::string SuperblockFlagText(const Superblock& Sb) {
  std::string Out;
  if (Sb.Flags & SbSynchronous) Out += ",sync";
  if (Sb.Flags & SbDirsync) Out += ",dirsync";
  if (Sb.Flags & SbMandlock) Out += ",mand";
  if (Sb.Flags & SbLazytime) Out += ",lazytime";
  return Out;
}

// The filesystem's own options (shmem_show_options and friends), with a leading comma each.
inline std::string FsOptionText(const Superblock& Sb) {
  std::string Out;
  char Buffer[64];
  if (Sb.Type == "tmpfs") {
    if (Sb.SizeBytes) {
      std::snprintf(Buffer, sizeof(Buffer), ",size=%lluk", static_cast<unsigned long long>(Sb.SizeBytes >> 10));
      Out += Buffer;
    }
    if (Sb.NrInodes) {
      std::snprintf(Buffer, sizeof(Buffer), ",nr_inodes=%llu", static_cast<unsigned long long>(Sb.NrInodes));
      Out += Buffer;
    }
    if (Sb.Mode != 01777) {
      std::snprintf(Buffer, sizeof(Buffer), ",mode=%03o", Sb.Mode);
      Out += Buffer;
    }
    if (Sb.Uid) Out += ",uid=" + std::to_string(Sb.Uid);
    if (Sb.Gid) Out += ",gid=" + std::to_string(Sb.Gid);
  } else if (Sb.Type == "ramfs") {
    if (Sb.Mode != 01777 && Sb.Mode != 0755) {
      std::snprintf(Buffer, sizeof(Buffer), ",mode=%o", Sb.Mode);
      Out += Buffer;
    }
  } else if (Sb.Type == "devpts") {
    Out += ",mode=600,ptmxmode=000";
  }
  Out += Sb.Extra;
  return Out;
}

// /proc/<pid>/mountinfo as read by a process whose root is Root.
inline std::optional<std::string> WhereMounted(const Place& Root, const MountRef& M) {
  if (M == Root.M && M->Root == Root.Path) return std::string {"/"};
  if (!M->Parent) return std::nullopt;
  return VisiblePath(Root, M->Parent.get(), M->Mountpoint);
}

inline std::string MountInfo(const Table& T, const Place& Root) {
  std::string Out;
  char Buffer[64];
  for (const auto& M : T.List) {
    const auto Visible = WhereMounted(Root, M);
    if (!Visible) continue;
    const std::string& Where = *Visible;
    const auto& Sb = *M->Sb;
    std::string Inside = "/";
    if (PathContains(Sb.FsRoot, M->Root) && M->Root.size() > Sb.FsRoot.size()) Inside = M->Root.substr(Sb.FsRoot.size());
    std::snprintf(Buffer, sizeof(Buffer), "%llu %llu 0:%llu ", static_cast<unsigned long long>(M->Id),
                  static_cast<unsigned long long>(M->Parent ? M->Parent->Id : 0), static_cast<unsigned long long>(Sb.Dev));
    Out += Buffer;
    Out += Escape(Inside) + " " + Escape(Where) + " " + MountOptions(*M);
    if (M->Group) Out += " shared:" + std::to_string(M->Group);
    if (M->Master) Out += " master:" + std::to_string(M->Master);
    if (M->Unbindable) Out += " unbindable";
    Out += " - " + Sb.Type + " " + Escape(Sb.Source) + " " + (Sb.ReadOnly ? "ro" : "rw") + SuperblockFlagText(Sb) + FsOptionText(Sb) + "\n";
  }
  return Out;
}

// /proc/<pid>/mounts.
inline std::string MountsText(const Table& T, const Place& Root) {
  std::string Out;
  for (const auto& M : T.List) {
    const auto Visible = WhereMounted(Root, M);
    if (!Visible) continue;
    const std::string& Where = *Visible;
    const auto& Sb = *M->Sb;
    Out += Escape(Sb.Source) + " " + Escape(Where) + " " + Sb.Type + " " + (M->ReadOnly() ? "ro" : "rw") + SuperblockFlagText(Sb);
    std::string Options = MountOptions(*M);
    Options.erase(0, 2);  // the leading ro or rw is printed above
    Out += Options;
    Out += FsOptionText(Sb) + " 0 0\n";
  }
  return Out;
}

// ======================= access rules =======================

// What a write through mount M hits: 0 when allowed, ErrRoFs for a read-only mount or filesystem, ErrPerm for the
// pseudo filesystems that take no creations.
inline int64_t WriteCheck(const Mount& M) {
  if (M.ReadOnly()) return ErrRoFs;
  return 0;
}
inline int64_t CreateCheck(const Mount& M) {
  if (M.Sb->Pseudo) return ErrPerm;
  return WriteCheck(M);
}
}  // namespace Switch2Guest::Mounts
