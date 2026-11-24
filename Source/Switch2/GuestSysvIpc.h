// SPDX-License-Identifier: MIT
#pragma once

// System V IPC tables. Pure bookkeeping: the runtime owns waiting, guest
// memory and process state. Layouts and limits follow Linux 7.3 x86-64
// (measured on the host: struct sizes below are the kernel's ipc64 layouts).

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Switch2Guest {
constexpr int32_t IpcMni = 32768; // IPCMNI: ids encode seq * IpcMni + index
constexpr int IpcCreat = 01000, IpcExcl = 02000, IpcNowait = 04000;
constexpr int32_t IpcPrivate = 0;

// Limits (host Linux defaults).
constexpr uint64_t ShmMax = UINT64_MAX - (uint64_t {1} << 24); // SHMMAX, also SHMALL (pages)
constexpr uint64_t ShmMin = 1, ShmMni = 4096, ShmSeg = 4096;
constexpr int32_t SemMsl = 32000, SemOpm = 500, SemMni = 32000, SemVmx = 32767, SemAem = 32767, SemUsz = 20;
constexpr int32_t SemMns = 1024000000;
constexpr int32_t MsgMax = 8192, MsgMnb = 16384, MsgMni = 32000, MsgSsz = 16, MsgSeg = 65535;
constexpr int32_t MsgPool = MsgMni * MsgMnb / 1024;

// ipc64_perm on x86-64: 48 bytes.
struct IpcPerm {
  int32_t Key {};
  uint32_t Uid {}, Gid {}, Cuid {}, Cgid {};
  uint16_t Mode {};
  uint16_t Seq {};
};

struct IpcCred {
  uint32_t Uid, Gid; // effective ids
  std::span<const uint32_t> Groups;
  bool CapIpcOwner;
};

// Linux ipcperms(): owner or creator bits, then group (either gid), then
// other. Requested holds the wanted bits as one octal digit (4 read,
// 2 write, 1 execute). CAP_IPC_OWNER bypasses the check.
inline bool IpcAllowed(const IpcPerm& Perm, const IpcCred& Cred, uint16_t Requested) {
  uint16_t Granted = Perm.Mode;
  const auto InGroup = [&](uint32_t Gid) {
    return Gid == Cred.Gid || std::find(Cred.Groups.begin(), Cred.Groups.end(), Gid) != Cred.Groups.end();
  };
  if (Cred.Uid == Perm.Cuid || Cred.Uid == Perm.Uid) Granted >>= 6;
  else if (InGroup(Perm.Cgid) || InGroup(Perm.Gid)) Granted >>= 3;
  if ((Requested & ~Granted & 07) && !Cred.CapIpcOwner) return false;
  return true;
}

// One table per object kind. Object needs a public `IpcPerm Perm` member and
// must be default constructible. Objects keep stable addresses.
template<typename Object>
class IpcTable {
public:
  explicit IpcTable(size_t Limit = IpcMni) : Limit {Limit} {}

  // shmget/semget/msgget lookup. Returns the id, or -errno: ENOENT (no key
  // and no IPC_CREAT), EEXIST (IPC_CREAT|IPC_EXCL on an existing key),
  // EACCES (permission on an existing key, checked after Check), ENOSPC
  // (table full) and whatever Create or Check return. Create(Object&) fills a fresh object whose Perm
  // is already set; Check(const Object&) validates an existing one.
  int64_t Get(int32_t Key, int Flags, const IpcCred& Cred, auto&& Create, auto&& Check) {
    if (Key != IpcPrivate) {
      if (auto* Existing = FindKey(Key)) {
        if ((Flags & IpcCreat) && (Flags & IpcExcl)) return -17;
        // ipcget_public: more_checks (size EINVAL) before ipc_check_perms (EACCES).
        if (const int64_t Error = Check(std::as_const(*Existing)); Error) return Error;
        const uint16_t Mode = static_cast<uint16_t>(Flags) & 0777;
        if (!IpcAllowed(Existing->Perm, Cred, ((Mode >> 6) | (Mode >> 3) | Mode) & 07)) return -13;
        return IdOf(*Existing);
      }
      if (!(Flags & IpcCreat)) return -2;
    }
    Object Fresh {};
    Fresh.Perm.Key = Key;
    Fresh.Perm.Uid = Fresh.Perm.Cuid = Cred.Uid;
    Fresh.Perm.Gid = Fresh.Perm.Cgid = Cred.Gid;
    Fresh.Perm.Mode = static_cast<uint16_t>(Flags) & 0777;
    if (const int64_t Error = Create(Fresh); Error) return Error;
    if (Items.size() >= Limit) return -28;
    const auto Index = AllocateIndex();
    if (Index < 0) return -28;
    Fresh.Perm.Seq = CurrentSeq;
    auto& Slot = Items.emplace(Index, std::move(Fresh)).first->second;
    return IdOf(Slot);
  }

  // The object an id names, or nullptr (a stale sequence or free index).
  Object* Find(int32_t Id) {
    if (Id < 0) return nullptr;
    const auto It = Items.find(Id % IpcMni);
    if (It == Items.end() || It->second.Perm.Seq != static_cast<uint16_t>(Id / IpcMni)) return nullptr;
    return &It->second;
  }

  // The object at a table index (SHM_STAT and friends), or nullptr.
  Object* FindIndex(int32_t Index) {
    const auto It = Items.find(Index);
    return It == Items.end() ? nullptr : &It->second;
  }

  Object* FindKey(int32_t Key) {
    if (Key == IpcPrivate) return nullptr;
    for (auto& [Index, Item] : Items) {
      if (Item.Perm.Key == Key) return &Item;
    }
    return nullptr;
  }

  void Remove(int32_t Id) {
    if (Find(Id)) Items.erase(Id % IpcMni);
  }

  // The id of an object that lives in this table.
  int32_t IdOf(const Object& Item) const {
    for (const auto& [Index, Slot] : Items) {
      if (&Slot == &Item) return Item.Perm.Seq * IpcMni + Index;
    }
    return -1;
  }

  // Highest index in use, or -1 when the table is empty.
  int32_t MaxIndex() const { return Items.empty() ? -1 : Items.rbegin()->first; }
  size_t Count() const { return Items.size(); }
  auto& All() { return Items; }
  const auto& All() const { return Items; }

private:
  // Linux ipc_idr_alloc: cyclic over [0, max(in_use * 3 / 2, 64)); the
  // sequence number advances when the cursor wraps.
  int32_t AllocateIndex() {
    const int32_t Bound = static_cast<int32_t>(std::min<size_t>(std::max<size_t>(Items.size() * 3 / 2, 64), IpcMni));
    int32_t Index = -1;
    for (int32_t Candidate = LastIndex + 1; Candidate < Bound && Index < 0; ++Candidate) {
      if (!Items.contains(Candidate)) Index = Candidate;
    }
    for (int32_t Candidate = 0; Candidate <= LastIndex && Candidate < Bound && Index < 0; ++Candidate) {
      if (!Items.contains(Candidate)) Index = Candidate;
    }
    if (Index < 0) return -1;
    if (Index <= LastIndex) {
      ++Sequence;
      if (Sequence >= static_cast<uint32_t>(INT32_MAX / IpcMni)) Sequence = 0;
    }
    CurrentSeq = static_cast<uint16_t>(Sequence);
    LastIndex = Index;
    return Index;
  }

  std::map<int32_t, Object> Items;
  size_t Limit;
  int32_t LastIndex {-1};
  uint32_t Sequence {};
  uint16_t CurrentSeq {};
};

// ---- Semaphores -----------------------------------------------------------
constexpr int16_t SemUndo = 0x1000;

struct SemOp {
  uint16_t Num;
  int16_t Op;
  int16_t Flags;
};

// Linux perform_atomic_semop over the semaphore values of one set. Ops apply
// in order and all take effect or none do. Returns 0 (applied), 1 (the call
// must block), or -errno: EFBIG (a semaphore number out of range), ERANGE
// (a value above SEMVMX, or an undo adjustment out of range) and EAGAIN when
// a blocking op carries IPC_NOWAIT. UndoDelta is the caller's adjustment for
// this set (one entry per semaphore, grown on demand); Pids, when given,
// receives the last pid per semaphore for the applied ops.
inline int TrySemOps(std::span<uint16_t> Values, std::span<const SemOp> Ops, std::vector<int16_t>& UndoDelta, uint32_t Pid,
                     std::span<uint32_t> Pids = {}) {
  for (const auto& Op : Ops) {
    if (Op.Num >= Values.size()) return -27;
  }
  const bool Undoes = std::any_of(Ops.begin(), Ops.end(), [](const SemOp& Op) { return Op.Flags & SemUndo; });
  if (Undoes && UndoDelta.size() < Values.size()) UndoDelta.resize(Values.size());
  std::vector<uint16_t> Original(Values.begin(), Values.end());
  std::vector<int16_t> OriginalUndo = UndoDelta;
  const auto Roll = [&] {
    std::copy(Original.begin(), Original.end(), Values.begin());
    UndoDelta = OriginalUndo;
  };
  for (const auto& Op : Ops) {
    auto& Value = Values[Op.Num];
    if (!Op.Op && Value) {
      Roll();
      return (Op.Flags & IpcNowait) ? -11 : 1;
    }
    const int32_t Result = static_cast<int32_t>(Value) + Op.Op;
    if (Result < 0) {
      Roll();
      return (Op.Flags & IpcNowait) ? -11 : 1;
    }
    if (Result > SemVmx) {
      Roll();
      return -34;
    }
    if (Op.Flags & SemUndo) {
      const int32_t Undo = static_cast<int32_t>(UndoDelta[Op.Num]) - Op.Op;
      if (Undo < -SemAem - 1 || Undo > SemAem) {
        Roll();
        return -34;
      }
      UndoDelta[Op.Num] = static_cast<int16_t>(Undo);
    }
    Value = static_cast<uint16_t>(Result);
  }
  for (const auto& Op : Ops) {
    if (Op.Num < Pids.size()) Pids[Op.Num] = Pid;
  }
  return 0;
}

// exit_sem: applies the adjustments of a dying process, clamping each value
// to [0, SEMVMX] (Linux skips the part that would leave the range).
inline void ApplySemUndo(std::span<uint16_t> Values, std::span<const int16_t> UndoDelta) {
  for (size_t Index = 0; Index < Values.size() && Index < UndoDelta.size(); ++Index) {
    int32_t Next = static_cast<int32_t>(Values[Index]) + UndoDelta[Index];
    Next = std::clamp<int32_t>(Next, 0, SemVmx);
    Values[Index] = static_cast<uint16_t>(Next);
  }
}

// ---- Messages -------------------------------------------------------------
struct Message {
  int64_t Type;
  std::vector<uint8_t> Text;
};

// msgrcv type selection: 0 takes the first message; a positive type the
// first of that type (or the first not of that type with MSG_EXCEPT); a
// negative type the message with the lowest type that is <= |type| (the
// first one among equals). MSG_EXCEPT is ignored for types <= 0.
inline std::optional<size_t> SelectMessage(const std::deque<Message>& Queue, int64_t Type, bool Except) {
  if (Queue.empty()) return std::nullopt;
  if (Type == 0) return 0;
  if (Type < 0) {
    const int64_t Limit = Type == INT64_MIN ? INT64_MAX : -Type;
    std::optional<size_t> Best;
    for (size_t Index = 0; Index < Queue.size(); ++Index) {
      if (Queue[Index].Type > Limit) continue;
      if (!Best || Queue[Index].Type < Queue[*Best].Type) Best = Index;
    }
    return Best;
  }
  for (size_t Index = 0; Index < Queue.size(); ++Index) {
    if (Except ? Queue[Index].Type != Type : Queue[Index].Type == Type) return Index;
  }
  return std::nullopt;
}

// ---- Guest struct layouts (x86-64 kernel ipc64) ----------------------------
struct LinuxIpc64Perm {
  int32_t Key;
  uint32_t Uid, Gid, Cuid, Cgid;
  uint32_t Mode; // __kernel_mode_t; only the low 16 bits are used
  uint16_t Seq;
  uint16_t Pad2;
  uint64_t Unused1, Unused2;
};
static_assert(sizeof(LinuxIpc64Perm) == 48);
static_assert(offsetof(LinuxIpc64Perm, Mode) == 20);
static_assert(offsetof(LinuxIpc64Perm, Seq) == 24);

inline LinuxIpc64Perm ToLinux(const IpcPerm& Perm, uint32_t ExtraMode = 0) {
  LinuxIpc64Perm Out {};
  Out.Key = Perm.Key;
  Out.Uid = Perm.Uid;
  Out.Gid = Perm.Gid;
  Out.Cuid = Perm.Cuid;
  Out.Cgid = Perm.Cgid;
  Out.Mode = Perm.Mode | ExtraMode;
  Out.Seq = Perm.Seq;
  return Out;
}

struct LinuxShmidDs {
  LinuxIpc64Perm Perm;
  uint64_t SegSz;
  int64_t Atime, Dtime, Ctime;
  int32_t Cpid, Lpid;
  uint64_t Nattch;
  uint64_t Unused5, Unused6;
};
static_assert(sizeof(LinuxShmidDs) == 112);
static_assert(offsetof(LinuxShmidDs, SegSz) == 48 && offsetof(LinuxShmidDs, Atime) == 56 &&
              offsetof(LinuxShmidDs, Cpid) == 80 && offsetof(LinuxShmidDs, Nattch) == 88);

struct LinuxSemidDs {
  LinuxIpc64Perm Perm;
  int64_t Otime;
  uint64_t Unused1;
  int64_t Ctime;
  uint64_t Unused2;
  uint64_t Nsems;
  uint64_t Unused3, Unused4;
};
static_assert(sizeof(LinuxSemidDs) == 104);
static_assert(offsetof(LinuxSemidDs, Otime) == 48 && offsetof(LinuxSemidDs, Ctime) == 64 &&
              offsetof(LinuxSemidDs, Nsems) == 80);

struct LinuxMsqidDs {
  LinuxIpc64Perm Perm;
  int64_t Stime, Rtime, Ctime;
  uint64_t Cbytes, Qnum, Qbytes;
  int32_t Lspid, Lrpid;
  uint64_t Unused4, Unused5;
};
static_assert(sizeof(LinuxMsqidDs) == 120);
static_assert(offsetof(LinuxMsqidDs, Stime) == 48 && offsetof(LinuxMsqidDs, Cbytes) == 72 &&
              offsetof(LinuxMsqidDs, Qnum) == 80 && offsetof(LinuxMsqidDs, Qbytes) == 88 &&
              offsetof(LinuxMsqidDs, Lspid) == 96 && offsetof(LinuxMsqidDs, Lrpid) == 100);

struct LinuxShminfo { // IPC_INFO
  uint64_t Shmmax, Shmmin, Shmmni, Shmseg, Shmall, Unused[4];
};
static_assert(sizeof(LinuxShminfo) == 72);

struct LinuxShmInfo { // SHM_INFO
  int32_t UsedIds;
  uint32_t Pad;
  uint64_t ShmTot, ShmRss, ShmSwp, SwapAttempts, SwapSuccesses;
};
static_assert(sizeof(LinuxShmInfo) == 48 && offsetof(LinuxShmInfo, ShmTot) == 8);

struct LinuxSeminfo { // IPC_INFO and SEM_INFO
  int32_t Semmap, Semmni, Semmns, Semmnu, Semmsl, Semopm, Semume, Semusz, Semvmx, Semaem;
};
static_assert(sizeof(LinuxSeminfo) == 40);

struct LinuxMsginfo { // IPC_INFO and MSG_INFO
  int32_t Msgpool, Msgmap, Msgmax, Msgmnb, Msgmni, Msgssz, Msgtql;
  uint16_t Msgseg;
  uint16_t Pad;
};
static_assert(sizeof(LinuxMsginfo) == 32);

struct LinuxSembuf {
  uint16_t Num;
  int16_t Op;
  int16_t Flags;
};
static_assert(sizeof(LinuxSembuf) == 6);
} // namespace Switch2Guest
