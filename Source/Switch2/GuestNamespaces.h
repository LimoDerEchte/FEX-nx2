// SPDX-License-Identifier: MIT
#pragma once

// Pure logic of the Linux namespaces (Milestone 4, task 2): namespace objects with owner user namespace and
// reference counts (shared_ptr), id maps and their write rules, capability checks against the owning user
// namespace, pid translation between nested pid namespaces, the clone/unshare/setns decisions and the nsfs
// ioctl rules. The runtime stores one NsSet per process and does the memory access; this header can be tested
// on the host (LinuxRuntime/GuestNamespacesTest.cpp). The reference is kernel/user_namespace.c, kernel/nsproxy.c,
// kernel/pid_namespace.c and fs/nsfs.c of Linux 7.3, checked against native runs (see ContainerInventory.md).

#include "GuestCredentials.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Switch2Guest::Ns {
// Negated Linux errno values.
constexpr int64_t ErrPerm = -1, ErrSrch = -3, ErrNoMem = -12, ErrInval = -22, ErrNoSpc = -28, ErrNotty = -25,
                  ErrUsers = -87;

constexpr uint64_t CloneNewTime = 0x80, CloneNewNs = 0x20000, CloneNewCgroup = 0x02000000, CloneNewUts = 0x04000000,
                   CloneNewIpc = 0x08000000, CloneNewUser = 0x10000000, CloneNewPid = 0x20000000,
                   CloneNewNet = 0x40000000;
constexpr uint64_t AllNsFlags = CloneNewTime | CloneNewNs | CloneNewCgroup | CloneNewUts | CloneNewIpc | CloneNewUser |
                                CloneNewPid | CloneNewNet;
constexpr uint32_t CapSysChroot = 18, CapSysAdmin = 21, CapSetfcap = 31;
constexpr uint32_t MaxUserNsLevel = 32; // create_user_ns: a parent above this level gives EUSERS
constexpr uint32_t MaxPidNsLevel = 32;  // MAX_PID_NS_LEVEL: a deeper pid namespace gives ENOSPC
constexpr size_t MaxIdExtents = 340;    // UID_GID_MAP_MAX_EXTENTS
constexpr uint32_t OverflowId = 65534;  // overflowuid and overflowgid
constexpr uint32_t NameMax = 64;        // __NEW_UTS_LEN

enum class Kind : uint8_t { User, Uts, Ipc, Pid, Cgroup, Net, Mnt, Time };

constexpr uint64_t FlagOf(Kind K) {
  switch (K) {
  case Kind::User: return CloneNewUser;
  case Kind::Uts: return CloneNewUts;
  case Kind::Ipc: return CloneNewIpc;
  case Kind::Pid: return CloneNewPid;
  case Kind::Cgroup: return CloneNewCgroup;
  case Kind::Net: return CloneNewNet;
  case Kind::Mnt: return CloneNewNs;
  case Kind::Time: return CloneNewTime;
  }
  return 0;
}
constexpr const char* NameOf(Kind K) {
  switch (K) {
  case Kind::User: return "user";
  case Kind::Uts: return "uts";
  case Kind::Ipc: return "ipc";
  case Kind::Pid: return "pid";
  case Kind::Cgroup: return "cgroup";
  case Kind::Net: return "net";
  case Kind::Mnt: return "mnt";
  case Kind::Time: return "time";
  }
  return "";
}

// ======================= id maps =======================

// One extent: ids First.. of the namespace are kernel (initial namespace) ids Lower.. .
struct IdExtent {
  uint32_t First, Lower, Count;
};

class IdMap {
public:
  std::vector<IdExtent> Extents;
  bool Empty() const { return Extents.empty(); }
  static IdMap Identity() { return IdMap {{{0, 0, UINT32_MAX}}}; }

  // make_kuid / make_kgid: a namespace id to the kernel id.
  std::optional<uint32_t> ToKernel(uint32_t Id) const { return RangeToKernel(Id, 1); }
  // from_kuid: a kernel id to the namespace id.
  std::optional<uint32_t> FromKernel(uint32_t Kernel) const { return RangeFromKernel(Kernel, 1); }
  // map_id_range_down: the whole range lies in one extent.
  std::optional<uint32_t> RangeToKernel(uint32_t First, uint32_t Count) const {
    const uint64_t Last = uint64_t {First} + Count - 1;
    for (const auto& E : Extents) {
      if (First >= E.First && Last <= uint64_t {E.First} + E.Count - 1) return static_cast<uint32_t>(E.Lower + (First - E.First));
    }
    return std::nullopt;
  }
  std::optional<uint32_t> RangeFromKernel(uint32_t Lower, uint32_t Count) const {
    const uint64_t Last = uint64_t {Lower} + Count - 1;
    for (const auto& E : Extents) {
      if (Lower >= E.Lower && Last <= uint64_t {E.Lower} + E.Count - 1) return static_cast<uint32_t>(E.First + (Lower - E.Lower));
    }
    return std::nullopt;
  }
};

// ======================= namespace objects =======================

struct UserNs;

struct Namespace {
  Kind K;
  uint64_t Ino;
  Namespace(Kind K, uint64_t Ino) : K {K}, Ino {Ino} {}
  virtual ~Namespace() = default;
  Namespace(const Namespace&) = delete;
  Namespace& operator=(const Namespace&) = delete;
};

struct UserNs final : Namespace {
  std::shared_ptr<UserNs> Parent; // null for the initial namespace
  uint32_t Level {};
  uint32_t OwnerUid {}, OwnerGid {}; // kernel ids of the creator's euid and egid
  IdMap Uid, Gid;
  bool SetgroupsAllowed {true};
  bool ParentCouldSetfcap {};
  UserNs(uint64_t Ino) : Namespace {Kind::User, Ino} {}
};

// An owned namespace: its owner decides who may change it.
struct Owned : Namespace {
  std::shared_ptr<UserNs> Owner;
  Owned(Kind K, uint64_t Ino, std::shared_ptr<UserNs> Owner) : Namespace {K, Ino}, Owner {std::move(Owner)} {}
};

struct UtsNs final : Owned {
  std::string Hostname, Domainname;
  UtsNs(uint64_t Ino, std::shared_ptr<UserNs> Owner) : Owned {Kind::Uts, Ino, std::move(Owner)} {}
};

// The runtime hangs the System V tables and the POSIX message queue tree of the namespace on Store.
struct IpcNs final : Owned {
  std::shared_ptr<void> Store;
  IpcNs(uint64_t Ino, std::shared_ptr<UserNs> Owner) : Owned {Kind::Ipc, Ino, std::move(Owner)} {}
};

// CLONE_NEWCGROUP: the namespace root is the cgroup the creator was in; the guest has no cgroup hierarchy, so
// every namespace shows an empty view and the objects differ only in identity.
struct CgroupNs final : Owned {
  CgroupNs(uint64_t Ino, std::shared_ptr<UserNs> Owner) : Owned {Kind::Cgroup, Ino, std::move(Owner)} {}
};

// The network cannot be isolated (the guest uses the console's sockets): the one initial object.
struct NetNs final : Owned {
  NetNs(uint64_t Ino, std::shared_ptr<UserNs> Owner) : Owned {Kind::Net, Ino, std::move(Owner)} {}
};

// Seam for the mount namespace task: Table is the namespace's mount table (a copy is made on CLONE_NEWNS).
struct MntNs final : Owned {
  std::shared_ptr<void> Table;
  MntNs(uint64_t Ino, std::shared_ptr<UserNs> Owner) : Owned {Kind::Mnt, Ino, std::move(Owner)} {}
};

struct PidNs final : Owned {
  std::shared_ptr<PidNs> Parent;
  uint32_t Level {};
  uint32_t Init {};     // global id of the namespace's pid 1 (0 before the first task)
  bool Adding {true};   // false once the init died: no pid can be allocated (fork gives ENOMEM)
  uint32_t NextLocal {1};
  std::map<uint32_t, uint32_t> ToLocalMap, ToGlobalMap; // empty in the root namespace, where numbers are global ids
  PidNs(uint64_t Ino, std::shared_ptr<UserNs> Owner) : Owned {Kind::Pid, Ino, std::move(Owner)} {}

  // alloc_pid: the task gets a number here and in every ancestor below the root; returns the number here.
  uint32_t Allocate(uint32_t Global) {
    uint32_t Mine = Global;
    for (PidNs* N = this; N && N->Level > 0; N = N->Parent.get()) {
      const auto Local = N->NextLocal++;
      N->ToLocalMap[Global] = Local;
      N->ToGlobalMap[Local] = Global;
      if (N == this) Mine = Local;
      if (Local == 1 && N->Init == 0) N->Init = Global;
    }
    return Mine;
  }
  // A task that never started (its creation failed): its numbers go back.
  void Release(uint32_t Global) {
    for (PidNs* N = this; N && N->Level > 0; N = N->Parent.get()) {
      const auto It = N->ToLocalMap.find(Global);
      if (It == N->ToLocalMap.end()) continue;
      const auto Local = It->second;
      N->ToGlobalMap.erase(Local);
      N->ToLocalMap.erase(It);
      if (N->Init == Global) N->Init = 0;
      if (N->NextLocal == Local + 1) N->NextLocal = Local;
    }
  }
  // A task that is gone for good (reaped): its numbers leave the maps. The counter does not go back, so a number
  // is not handed out again right away, and Init stays so the namespace still knows who was its pid 1.
  void Retire(uint32_t Global) {
    for (PidNs* N = this; N && N->Level > 0; N = N->Parent.get()) {
      const auto It = N->ToLocalMap.find(Global);
      if (It == N->ToLocalMap.end()) continue;
      N->ToGlobalMap.erase(It->second);
      N->ToLocalMap.erase(It);
    }
  }
  // The task's number in this namespace, 0 when it is not visible here (pid_nr_ns).
  uint32_t ToLocal(uint32_t Global) const {
    if (Level == 0) return Global;
    const auto It = ToLocalMap.find(Global);
    return It == ToLocalMap.end() ? 0 : It->second;
  }
  // find_pid_ns: the global id of the task this namespace numbers Local; 0 when there is none.
  uint32_t ToGlobal(uint32_t Local) const {
    if (Level == 0) return Local;
    const auto It = ToGlobalMap.find(Local);
    return It == ToGlobalMap.end() ? 0 : It->second;
  }
  bool IsInitOf(uint32_t Global) const { return Level > 0 && Init == Global; }
  // True when Other is this namespace or one of its descendants.
  bool Contains(const PidNs& Other) const {
    for (const PidNs* N = &Other; N; N = N->Parent.get()) {
      if (N == this) return true;
    }
    return false;
  }
};

// What a process holds: one of each (the time namespace is not built). PidForChildren differs from Pid between
// unshare(CLONE_NEWPID) and the next fork.
struct NsSet {
  std::shared_ptr<UserNs> User;
  std::shared_ptr<UtsNs> Uts;
  std::shared_ptr<IpcNs> Ipc;
  std::shared_ptr<PidNs> Pid, PidForChildren;
  std::shared_ptr<CgroupNs> Cgroup;
  std::shared_ptr<NetNs> Net;
  std::shared_ptr<MntNs> Mnt;

  std::shared_ptr<Namespace> Get(Kind K) const {
    switch (K) {
    case Kind::User: return User;
    case Kind::Uts: return Uts;
    case Kind::Ipc: return Ipc;
    case Kind::Pid: return Pid;
    case Kind::Cgroup: return Cgroup;
    case Kind::Net: return Net;
    case Kind::Mnt: return Mnt;
    case Kind::Time: return nullptr;
    }
    return nullptr;
  }
};

// The inode numbers of the initial namespaces are those of a Linux boot (proc_ns_inum); the others count up.
class Registry {
public:
  Registry() {
    Initial.User = std::make_shared<UserNs>(4026531837ULL);
    Initial.User->Uid = IdMap::Identity();
    Initial.User->Gid = IdMap::Identity();
    Initial.Uts = std::make_shared<UtsNs>(4026531838ULL, Initial.User);
    Initial.Ipc = std::make_shared<IpcNs>(4026531839ULL, Initial.User);
    Initial.Pid = Initial.PidForChildren = std::make_shared<PidNs>(4026531836ULL, Initial.User);
    Initial.Cgroup = std::make_shared<CgroupNs>(4026531835ULL, Initial.User);
    Initial.Net = std::make_shared<NetNs>(4026531833ULL, Initial.User);
    Initial.Mnt = std::make_shared<MntNs>(4026531832ULL, Initial.User);
  }
  NsSet Initial;
  uint64_t NextIno() { return NextIno_++; }

private:
  uint64_t NextIno_ {4026532000ULL};
};

// ======================= capabilities and identities =======================

// The acting task as the namespace rules see it: its user namespace, its effective capabilities there and its
// effective kernel ids.
struct Subject {
  std::shared_ptr<UserNs> Ns;
  uint64_t CapEffective {};
  uint32_t Euid {}, Egid {};
};

// cap_capable: whether the subject holds Cap over the user namespace Target. A task has the capability in its
// own namespace when it is in its effective set, and has every capability in a child namespace its euid created.
inline bool NsCapable(const Subject& S, const UserNs& Target, uint32_t Cap) {
  const UserNs* T = &Target;
  for (;;) {
    if (T == S.Ns.get()) return S.CapEffective >> Cap & 1;
    if (T->Level <= S.Ns->Level) return false;
    if (T->Parent.get() == S.Ns.get() && T->OwnerUid == S.Euid) return true;
    T = T->Parent.get();
  }
}

inline std::optional<uint32_t> MakeKuid(const UserNs& Ns, uint32_t Uid) { return Ns.Uid.ToKernel(Uid); }
inline std::optional<uint32_t> MakeKgid(const UserNs& Ns, uint32_t Gid) { return Ns.Gid.ToKernel(Gid); }
// from_kuid_munged: ids with no mapping read as the overflow id.
inline uint32_t MungeUid(const UserNs& Ns, uint32_t Kuid) { return Ns.Uid.FromKernel(Kuid).value_or(OverflowId); }
inline uint32_t MungeGid(const UserNs& Ns, uint32_t Kgid) { return Ns.Gid.FromKernel(Kgid).value_or(OverflowId); }

// The kernel id the namespace calls root (make_kuid(ns, 0)), NoId when 0 is unmapped.
inline uint32_t RootKuid(const UserNs& Ns) { return Ns.Uid.ToKernel(0).value_or(NoId); }

// ======================= user namespaces =======================

// create_user_ns. Chrooted: the creator is inside a chroot (EPERM).
inline std::variant<int64_t, std::shared_ptr<UserNs>> CreateUserNs(Registry& R, const Subject& Creator, bool Chrooted) {
  if (Creator.Ns->Level > MaxUserNsLevel) return ErrUsers;
  if (Chrooted) return ErrPerm;
  if (!Creator.Ns->Uid.FromKernel(Creator.Euid) || !Creator.Ns->Gid.FromKernel(Creator.Egid)) return ErrPerm;
  auto New = std::make_shared<UserNs>(R.NextIno());
  New->Parent = Creator.Ns;
  New->Level = Creator.Ns->Level + 1;
  New->OwnerUid = Creator.Euid;
  New->OwnerGid = Creator.Egid;
  New->ParentCouldSetfcap = Creator.CapEffective >> CapSetfcap & 1;
  New->SetgroupsAllowed = Creator.Ns->SetgroupsAllowed; // create_user_ns: ns->flags = parent_ns->flags
  return New;
}

// set_cred_user_ns: the creator becomes the namespace's full-capability owner.
inline void EnterUserNs(Credentials& C, const std::shared_ptr<UserNs>& Ns) {
  C.CapPermitted = C.CapEffective = C.CapBounding = CapFull;
  C.CapInheritable = C.CapAmbient = 0;
  C.Securebits = 0;
  C.InitialNs = false;
  C.RootUid = RootKuid(*Ns);
}

struct ParsedExtent {
  uint32_t First, Lower, Count;
};

// The text of a uid_map or gid_map write ("first lower count" per line), nullopt for anything Linux rejects with
// EINVAL: a field that is not a plain decimal number below 2^32, a missing or extra field, a zero count, a range
// that wraps, overlap with an earlier line (in either column), or 340 lines already read.
inline std::optional<std::vector<ParsedExtent>> ParseIdMapText(std::string_view Text) {
  const auto IsSpace = [](char C) { return C == ' ' || C == '\t' || C == '\n' || C == '\v' || C == '\f' || C == '\r'; };
  // One line: three decimal numbers, each ended by a blank or the end of the line.
  const auto ParseLine = [&](std::string_view Line) -> std::optional<ParsedExtent> {
    size_t Pos = 0;
    const auto SkipSpaces = [&] {
      while (Pos < Line.size() && IsSpace(Line[Pos])) ++Pos;
    };
    const auto Number = [&]() -> std::optional<uint32_t> {
      SkipSpaces();
      if (Pos >= Line.size() || Line[Pos] < '0' || Line[Pos] > '9') return std::nullopt;
      uint64_t V {};
      while (Pos < Line.size() && Line[Pos] >= '0' && Line[Pos] <= '9') {
        V = V * 10 + static_cast<uint64_t>(Line[Pos++] - '0');
        if (V > UINT32_MAX) return std::nullopt;
      }
      if (Pos < Line.size() && !IsSpace(Line[Pos])) return std::nullopt;
      return static_cast<uint32_t>(V);
    };
    const auto First = Number();
    const auto Lower = First ? Number() : std::nullopt;
    const auto Count = Lower ? Number() : std::nullopt;
    if (!Count) return std::nullopt;
    SkipSpaces();
    if (Pos != Line.size()) return std::nullopt;
    return ParsedExtent {*First, *Lower, *Count};
  };
  std::vector<ParsedExtent> Out;
  size_t Pos = 0;
  while (Pos < Text.size()) {
    const size_t End = Text.find('\n', Pos);
    const size_t LineEnd = End == std::string_view::npos ? Text.size() : End;
    const auto Parsed = ParseLine(Text.substr(Pos, LineEnd - Pos));
    Pos = End == std::string_view::npos ? Text.size() : End + 1;
    if (!Parsed) return std::nullopt;
    const auto [First, Lower, Count] = *Parsed;
    if (!Count) return std::nullopt;
    if (static_cast<uint32_t>(First + Count) <= First || static_cast<uint32_t>(Lower + Count) <= Lower) return std::nullopt;
    if (Out.size() >= MaxIdExtents) return std::nullopt;
    for (const auto& E : Out) {
      if (First < uint64_t {E.First} + E.Count && E.First < uint64_t {First} + Count) return std::nullopt;
      if (Lower < uint64_t {E.Lower} + E.Count && E.Lower < uint64_t {Lower} + Count) return std::nullopt;
    }
    Out.push_back({First, Lower, Count});
  }
  return Out;
}

// new_idmap_permitted. Extents carry ids of the parent namespace in Lower. Opener is the credential the file was
// opened with (f_cred), Current the writing task.
inline bool IdMapPermitted(const UserNs& Target, bool Gid, const Subject& Opener, const Subject& Current,
                           const std::vector<ParsedExtent>& Extents) {
  const auto& Parent = *Target.Parent;
  if (!Gid) {
    // verify_root_map: mapping the parent's uid 0 needs CAP_SETFCAP.
    bool MapsRoot = false;
    for (const auto& E : Extents) MapsRoot |= E.Lower == 0;
    if (MapsRoot) {
      if (Opener.Ns.get() == &Target) {
        if (!Target.ParentCouldSetfcap) return false;
      } else if (!NsCapable(Opener, Parent, CapSetfcap)) {
        return false;
      }
    }
  }
  // Mapping one's own id is always allowed (gids only with setgroups denied).
  if (Extents.size() == 1 && Extents[0].Count == 1 && Target.OwnerUid == Opener.Euid) {
    const uint32_t Id = Extents[0].Lower;
    if (!Gid) {
      const auto Kuid = MakeKuid(Parent, Id);
      if (Kuid && *Kuid == Opener.Euid) return true;
    } else {
      const auto Kgid = MakeKgid(Parent, Id);
      if (!Target.SetgroupsAllowed && Kgid && *Kgid == Opener.Egid) return true;
    }
  }
  const uint32_t Cap = Gid ? CapSetgid : CapSetuid;
  return NsCapable(Current, Parent, Cap) && NsCapable(Opener, Parent, Cap);
}

// proc_uid_map_write / proc_gid_map_write and map_write. Returns the byte count or a negated errno.
//  - the opener must be in the namespace or its parent (EPERM);
//  - the write must start at offset 0 and be shorter than a page (EINVAL);
//  - one write only (EPERM), and it needs CAP_SYS_ADMIN over the namespace (EPERM);
//  - text errors EINVAL, an empty map EINVAL, then the permission rules (EPERM);
//  - every range must lie within one extent of the parent's map (EPERM).
inline int64_t WriteIdMap(UserNs& Target, bool Gid, const Subject& Opener, const Subject& Current, uint64_t Offset,
                          std::string_view Text) {
  UserNs* Parent = Target.Parent.get();
  if (!Parent || (Opener.Ns.get() != &Target && Opener.Ns.get() != Parent)) return ErrPerm;
  if (Offset != 0 || Text.size() >= 4096) return ErrInval;
  IdMap& Map = Gid ? Target.Gid : Target.Uid;
  if (!Map.Empty()) return ErrPerm;
  if (!NsCapable(Opener, Target, CapSysAdmin)) return ErrPerm;
  const auto Parsed = ParseIdMapText(Text);
  if (!Parsed || Parsed->empty()) return ErrInval;
  if (!IdMapPermitted(Target, Gid, Opener, Current, *Parsed)) return ErrPerm;
  IdMap New;
  const IdMap& ParentMap = Gid ? Parent->Gid : Parent->Uid;
  for (const auto& E : *Parsed) {
    const auto Kernel = ParentMap.RangeToKernel(E.Lower, E.Count);
    if (!Kernel) return ErrPerm;
    New.Extents.push_back({E.First, *Kernel, E.Count});
  }
  if (New.Extents.size() > 5) {
    std::sort(New.Extents.begin(), New.Extents.end(), [](const IdExtent& A, const IdExtent& B) { return A.First < B.First; });
  }
  Map = std::move(New);
  return static_cast<int64_t>(Text.size());
}

// proc_setgroups_write: "allow" or "deny", then optional whitespace. Opening the file for writing needs
// CAP_SYS_ADMIN over the namespace (EACCES, see OpenSetgroupsForWrite). Returns the byte count or -errno.
inline int64_t WriteSetgroups(UserNs& Target, uint64_t Offset, std::string_view Text) {
  if (Offset != 0 || Text.size() >= 8) return ErrInval;
  bool Allow;
  size_t Used;
  if (Text.starts_with("allow")) {
    Allow = true;
    Used = 5;
  } else if (Text.starts_with("deny")) {
    Allow = false;
    Used = 4;
  } else {
    return ErrInval;
  }
  for (size_t I = Used; I < Text.size(); ++I) {
    const char C = Text[I];
    if (C != ' ' && C != '\t' && C != '\n' && C != '\v' && C != '\f' && C != '\r') return ErrInval;
  }
  if (Allow) {
    if (!Target.SetgroupsAllowed) return ErrPerm;
  } else {
    if (!Target.Gid.Empty()) return ErrPerm;
    Target.SetgroupsAllowed = false;
  }
  return static_cast<int64_t>(Text.size());
}
inline bool OpenSetgroupsForWrite(const UserNs& Target, const Subject& Opener) { return NsCapable(Opener, Target, CapSysAdmin); }

// userns_may_setgroups: the setgroups syscall needs a written gid map and no "deny".
inline bool MaySetgroups(const UserNs& Ns) { return !Ns.Gid.Empty() && Ns.SetgroupsAllowed; }

// uid_m_show and gid_m_show: the map as the reader sees it. A reader in the namespace sees the parent's ids.
inline std::string RenderIdMap(const UserNs& Target, bool Gid, const UserNs& Reader) {
  const UserNs* Lower = &Reader;
  if (Lower == &Target && Target.Parent) Lower = Target.Parent.get();
  const IdMap& Map = Gid ? Target.Gid : Target.Uid;
  std::string Out;
  char Line[48];
  for (const auto& E : Map.Extents) {
    const auto Seen = (Gid ? Lower->Gid : Lower->Uid).FromKernel(E.Lower);
    std::snprintf(Line, sizeof(Line), "%10u %10u %10u\n", E.First, Seen.value_or(NoId), E.Count);
    Out += Line;
  }
  return Out;
}

inline std::string RenderSetgroups(const UserNs& Ns) { return Ns.SetgroupsAllowed ? "allow\n" : "deny\n"; }

// ======================= clone, unshare and setns =======================

struct Switch {
  NsSet Ns;                          // the namespaces after the call
  std::shared_ptr<UserNs> NewUser;   // set when CLONE_NEWUSER created one (the caller updates the credentials)
  Subject Who;                       // the subject after the change (full capabilities in the new user namespace)
  bool NewMnt {};                    // CLONE_NEWNS: the caller copies the mount table (Task 3)
};

// create_new_namespaces and the checks around it, shared by clone and unshare. Old is the process's set, Who the
// acting task. Flags holds CLONE_NEW* bits only. The mutual exclusions with CLONE_FS, CLONE_THREAD and
// CLONE_SYSVSEM are the caller's (they differ between clone and unshare).
//  - CLONE_NEWTIME: EINVAL (the kernel is built without time namespaces);
//  - CLONE_NEWUSER: create_user_ns errors, the creator gets full capabilities in it;
//  - every other namespace needs CAP_SYS_ADMIN over the (new) user namespace (EPERM);
//  - CLONE_NEWNET: EPERM always (the network cannot be isolated);
//  - CLONE_NEWPID: EINVAL while pid_for_children differs from the active pid namespace, ENOSPC past 32 levels.
inline std::variant<int64_t, Switch> BuildNamespaces(Registry& R, const NsSet& Old, const Subject& Who, uint64_t Flags, bool Chrooted) {
  Flags &= AllNsFlags;
  if (Flags & CloneNewTime) return ErrInval;
  Switch Out;
  Out.Ns = Old;
  Out.Who = Who;
  if (Flags & CloneNewUser) {
    auto Created = CreateUserNs(R, Who, Chrooted);
    if (auto* Error = std::get_if<int64_t>(&Created)) return *Error;
    Out.NewUser = std::get<std::shared_ptr<UserNs>>(Created);
    Out.Ns.User = Out.NewUser;
    Out.Who.Ns = Out.NewUser;
    Out.Who.CapEffective = CapFull;
  }
  const uint64_t Others = Flags & ~CloneNewUser;
  if (!Others) return Out;
  if (!NsCapable(Out.Who, *Out.Who.Ns, CapSysAdmin)) return ErrPerm;
  if (Flags & CloneNewNet) return ErrPerm;
  if (Flags & CloneNewPid) {
    if (Old.Pid != Old.PidForChildren) return ErrInval;
    if (Old.PidForChildren->Level >= MaxPidNsLevel) return ErrNoSpc;
  }
  if (Flags & CloneNewUts) {
    auto New = std::make_shared<UtsNs>(R.NextIno(), Out.Who.Ns);
    New->Hostname = Old.Uts->Hostname;
    New->Domainname = Old.Uts->Domainname;
    Out.Ns.Uts = std::move(New);
  }
  if (Flags & CloneNewIpc) Out.Ns.Ipc = std::make_shared<IpcNs>(R.NextIno(), Out.Who.Ns); // the runtime attaches the Store
  if (Flags & CloneNewCgroup) Out.Ns.Cgroup = std::make_shared<CgroupNs>(R.NextIno(), Out.Who.Ns);
  if (Flags & CloneNewNs) {
    Out.Ns.Mnt = std::make_shared<MntNs>(R.NextIno(), Out.Who.Ns);
    Out.Ns.Mnt->Table = Old.Mnt->Table; // task 3 replaces this by a copy
    Out.NewMnt = true;
  }
  if (Flags & CloneNewPid) {
    auto New = std::make_shared<PidNs>(R.NextIno(), Out.Who.Ns);
    New->Parent = Old.PidForChildren;
    New->Level = Old.PidForChildren->Level + 1;
    Out.Ns.PidForChildren = std::move(New);
  }
  return Out;
}

// The checks of one setns() installation (userns_install, utsns_install, ipcns_install, pidns_install,
// cgroupns_install, netns_install, mntns_install). Who is the subject, with the user namespace the earlier
// installations of the same call already set. SingleThread: the process has one thread. SharesFs: its
// filesystem state is shared with another process. Returns 0 or -errno.
inline int64_t CheckInstall(const NsSet& Old, const Subject& Who, const Namespace& Target, bool SingleThread, bool SharesFs) {
  switch (Target.K) {
  case Kind::User: {
    const auto& U = static_cast<const UserNs&>(Target);
    if (&U == Who.Ns.get()) return ErrInval;
    if (!SingleThread || SharesFs) return ErrInval;
    return NsCapable(Who, U, CapSysAdmin) ? 0 : ErrPerm;
  }
  case Kind::Pid: {
    const auto& P = static_cast<const PidNs&>(Target);
    if (!NsCapable(Who, *P.Owner, CapSysAdmin) || !NsCapable(Who, *Who.Ns, CapSysAdmin)) return ErrPerm;
    // Only the active namespace and its descendants may be entered.
    const PidNs* A = &P;
    while (A->Level > Old.Pid->Level) A = A->Parent.get();
    return A == Old.Pid.get() ? 0 : ErrInval;
  }
  case Kind::Mnt: {
    const auto& M = static_cast<const MntNs&>(Target);
    if (!NsCapable(Who, *M.Owner, CapSysAdmin) || !NsCapable(Who, *Who.Ns, CapSysChroot) || !NsCapable(Who, *Who.Ns, CapSysAdmin)) return ErrPerm;
    return SharesFs ? ErrInval : 0;
  }
  case Kind::Uts: case Kind::Ipc: case Kind::Cgroup: case Kind::Net: {
    const auto& O = static_cast<const Owned&>(Target);
    return NsCapable(Who, *O.Owner, CapSysAdmin) && NsCapable(Who, *Who.Ns, CapSysAdmin) ? 0 : ErrPerm;
  }
  case Kind::Time: return ErrInval;
  }
  return ErrInval;
}

// Installs Target into New (the result of a successful CheckInstall).
inline void Install(NsSet& New, const std::shared_ptr<Namespace>& Target) {
  switch (Target->K) {
  case Kind::User: New.User = std::static_pointer_cast<UserNs>(Target); break;
  case Kind::Uts: New.Uts = std::static_pointer_cast<UtsNs>(Target); break;
  case Kind::Ipc: New.Ipc = std::static_pointer_cast<IpcNs>(Target); break;
  case Kind::Pid: New.PidForChildren = std::static_pointer_cast<PidNs>(Target); break;
  case Kind::Cgroup: New.Cgroup = std::static_pointer_cast<CgroupNs>(Target); break;
  case Kind::Net: New.Net = std::static_pointer_cast<NetNs>(Target); break;
  case Kind::Mnt: New.Mnt = std::static_pointer_cast<MntNs>(Target); break;
  case Kind::Time: break;
  }
}

// ======================= nsfs =======================

// ioctl numbers of fs/nsfs.c (NSIO 0xb7).
constexpr uint32_t NsGetUserns = 0xb701, NsGetParent = 0xb702, NsGetNstype = 0xb703, NsGetOwnerUid = 0xb704;

// ns_get_owner: the user namespace that owns Target, when it is the caller's namespace or a descendant of it
// (the owner's chain must reach the caller's namespace), else EPERM. A user namespace is owned by its parent.
inline std::variant<int64_t, std::shared_ptr<UserNs>> NsOwner(const Namespace& Target, const UserNs& Mine) {
  std::shared_ptr<UserNs> Owner;
  if (Target.K == Kind::User) Owner = static_cast<const UserNs&>(Target).Parent;
  else if (Target.K != Kind::Time) Owner = static_cast<const Owned&>(Target).Owner;
  for (std::shared_ptr<UserNs> P = Owner;; P = P->Parent) {
    if (!P) return ErrPerm;
    if (P.get() == &Mine) return Owner;
  }
}

// NS_GET_PARENT: user and pid namespaces only (EINVAL otherwise); the parent must be the caller's namespace or
// one of its descendants (EPERM otherwise). Returns the parent object.
inline std::variant<int64_t, std::shared_ptr<Namespace>> NsParent(const Namespace& Target, const UserNs& MyUser, const PidNs& MyPid) {
  if (Target.K == Kind::User) {
    auto Owner = NsOwner(Target, MyUser);
    if (auto* Error = std::get_if<int64_t>(&Owner)) return *Error;
    return std::static_pointer_cast<Namespace>(std::get<std::shared_ptr<UserNs>>(Owner));
  }
  if (Target.K == Kind::Pid) {
    const auto& P = static_cast<const PidNs&>(Target);
    for (std::shared_ptr<PidNs> N = P.Parent;; N = N->Parent) {
      if (!N) return ErrPerm;
      if (N.get() == &MyPid) return std::static_pointer_cast<Namespace>(P.Parent);
    }
  }
  return ErrInval;
}

// The text of /proc/<pid>/ns/<name>: "user:[4026531837]". Name is one of the entries of NsLinkNames.
constexpr const char* NsLinkNames[] = {"cgroup", "ipc", "mnt", "net", "pid", "pid_for_children", "time", "time_for_children", "user", "uts"};

inline std::optional<std::string> NsLinkText(const NsSet& Ns, std::string_view Name) {
  const auto Format = [](const char* Type, uint64_t Ino) { return std::string(Type) + ":[" + std::to_string(Ino) + "]"; };
  if (Name == "user") return Format("user", Ns.User->Ino);
  if (Name == "uts") return Format("uts", Ns.Uts->Ino);
  if (Name == "ipc") return Format("ipc", Ns.Ipc->Ino);
  if (Name == "pid") return Format("pid", Ns.Pid->Ino);
  if (Name == "pid_for_children") return Format("pid", Ns.PidForChildren->Ino);
  if (Name == "cgroup") return Format("cgroup", Ns.Cgroup->Ino);
  if (Name == "net") return Format("net", Ns.Net->Ino);
  if (Name == "mnt") return Format("mnt", Ns.Mnt->Ino);
  // The time namespace is not built: the one initial object (4026531834) is shown, as on a kernel without time
  // namespace creation.
  if (Name == "time" || Name == "time_for_children") return Format("time", 4026531834ULL);
  return std::nullopt;
}

// The namespace object a /proc/<pid>/ns/<name> link opens; null for the time namespace (no object).
inline std::shared_ptr<Namespace> NsLinkTarget(const NsSet& Ns, std::string_view Name) {
  if (Name == "user") return Ns.User;
  if (Name == "uts") return Ns.Uts;
  if (Name == "ipc") return Ns.Ipc;
  if (Name == "pid") return Ns.Pid;
  if (Name == "pid_for_children") return Ns.PidForChildren;
  if (Name == "cgroup") return Ns.Cgroup;
  if (Name == "net") return Ns.Net;
  if (Name == "mnt") return Ns.Mnt;
  return nullptr;
}

// ======================= pid namespace signal rules =======================

// sig_task_ignored for a signal sent by a user (kill, tkill, pidfd_send_signal): the init of a pid namespace
// ignores signals that have the default action. SIGKILL and SIGSTOP get through only from outside the namespace
// (SenderVisible: the sender has a pid in the target's namespace).
inline bool PidInitDropsSignal(bool TargetIsPidInit, bool DefaultAction, bool SenderVisible, uint32_t Signal) {
  if (!TargetIsPidInit || !DefaultAction) return false;
  const bool Kernel = Signal == 9 || Signal == 19; // sig_kernel_only: SIGKILL, SIGSTOP
  return !(!SenderVisible && Kernel);
}

// ======================= uts =======================

// sethostname and setdomainname: capability over the namespace's owner first, then the length (an int: the
// syscall truncates its argument) in 0..64.
inline int64_t CheckSetName(const Subject& Who, const UtsNs& Ns, uint64_t LenArgument) {
  if (!NsCapable(Who, *Ns.Owner, CapSysAdmin)) return ErrPerm;
  const auto Len = static_cast<int32_t>(LenArgument);
  if (Len < 0 || Len > static_cast<int32_t>(NameMax)) return ErrInval;
  return Len;
}
} // namespace Switch2Guest::Ns
