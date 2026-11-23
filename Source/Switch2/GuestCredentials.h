// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace Switch2Guest {
constexpr uint32_t NoId = UINT32_MAX;
constexpr uint32_t CapLast = 40; // CAP_CHECKPOINT_RESTORE
constexpr uint64_t CapFull = (uint64_t {1} << (CapLast + 1)) - 1;
constexpr uint32_t CapChown = 0, CapDacOverride = 1, CapKill = 5, CapSetgid = 6, CapSetuid = 7, CapSetpcap = 8,
                   CapSysNice = 23, CapSysTime = 25, CapWakeAlarm = 35;

// include/uapi/linux/securebits.h
constexpr uint32_t SecbitNoRoot = 1u << 0, SecbitNoRootLocked = 1u << 1, SecbitNoSetuidFixup = 1u << 2,
                   SecbitNoSetuidFixupLocked = 1u << 3, SecbitKeepCaps = 1u << 4, SecbitKeepCapsLocked = 1u << 5,
                   SecbitNoCapAmbientRaise = 1u << 6, SecbitNoCapAmbientRaiseLocked = 1u << 7;

// Glue note: capset(2) carries two 32-bit words per set; the upper word must be masked to CapFull (mk_kernel_cap)
// before calling CapSet, and results are reported back truncated the same way.
struct Credentials {
  std::array<uint32_t, 4> Uid {1000, 1000, 1000, 1000}; // real, effective, saved, fs
  std::array<uint32_t, 4> Gid {1000, 1000, 1000, 1000};
  uint64_t CapEffective {}, CapPermitted {}, CapInheritable {}, CapBounding {CapFull}, CapAmbient {};
  uint32_t Securebits {};
  bool Capable(uint32_t Cap) const { return CapEffective >> Cap & 1; }
  bool Secure(uint32_t Bit) const { return Securebits & Bit; }
};

inline Credentials RootCredentials() {
  Credentials C;
  C.Uid = {0, 0, 0, 0};
  C.Gid = {0, 0, 0, 0};
  C.CapEffective = C.CapPermitted = CapFull;
  return C;
}

namespace Detail {
// CAP_CHOWN, DAC_OVERRIDE, DAC_READ_SEARCH, FOWNER, FSETID, LINUX_IMMUTABLE, MKNOD, MAC_OVERRIDE
constexpr uint64_t FsCaps = (1ull << 0) | (1ull << 1) | (1ull << 2) | (1ull << 3) | (1ull << 4) | (1ull << 9) |
                            (1ull << 27) | (1ull << 32);

inline bool IsOneOf(uint32_t V, const std::array<uint32_t, 4>& Ids, bool IncludeFs) {
  return V == Ids[0] || V == Ids[1] || V == Ids[2] || (IncludeFs && V == Ids[3]);
}
} // namespace Detail

// cap_task_fix_setuid for LSM_SETID_RE/ID/RES (setuid/setreuid/setresuid): cap_emulate_setxuid.
inline void FixCapsSetxuid(Credentials& C, const std::array<uint32_t, 4>& Old) {
  if (C.Secure(SecbitNoSetuidFixup)) return;
  const bool OldHadRoot = Old[0] == 0 || Old[1] == 0 || Old[2] == 0;
  const bool NewHasRoot = C.Uid[0] == 0 || C.Uid[1] == 0 || C.Uid[2] == 0;
  if (OldHadRoot && !NewHasRoot && !C.Secure(SecbitKeepCaps)) {
    C.CapPermitted = C.CapEffective = 0;
  }
  if (!NewHasRoot) C.CapAmbient = 0;
  if (Old[1] == 0 && C.Uid[1] != 0) {
    C.CapEffective = 0;
  } else if (Old[1] != 0 && C.Uid[1] == 0) {
    C.CapEffective = C.CapPermitted;
  }
}

// cap_task_fix_setuid for LSM_SETID_FS (setfsuid only): drop/raise the fs capability set.
inline void FixCapsFs(Credentials& C, const std::array<uint32_t, 4>& Old) {
  if (C.Secure(SecbitNoSetuidFixup)) return;
  if (Old[3] == 0 && C.Uid[3] != 0) {
    C.CapEffective &= ~Detail::FsCaps;
  } else if (Old[3] != 0 && C.Uid[3] == 0) {
    C.CapEffective |= C.CapPermitted & Detail::FsCaps;
  }
}

inline int64_t SetUid(Credentials& C, uint32_t Uid) {
  if (Uid == NoId) return -22;
  const auto Old = C.Uid;
  if (C.Capable(CapSetuid)) {
    C.Uid = {Uid, Uid, Uid, Uid};
  } else if (Uid == C.Uid[0] || Uid == C.Uid[2]) {
    C.Uid[1] = C.Uid[3] = Uid;
  } else {
    return -1;
  }
  FixCapsSetxuid(C, Old);
  return 0;
}

inline int64_t SetReUid(Credentials& C, uint32_t Real, uint32_t Effective) {
  const bool Privileged = C.Capable(CapSetuid);
  const auto Old = C.Uid;
  if (!Privileged) {
    if (Real != NoId && Real != Old[0] && Real != Old[1]) return -1;
    if (Effective != NoId && !Detail::IsOneOf(Effective, Old, false)) return -1;
  }
  if (Real != NoId) C.Uid[0] = Real;
  if (Effective != NoId) C.Uid[1] = Effective;
  if (Real != NoId || (Effective != NoId && Effective != Old[0])) C.Uid[2] = C.Uid[1];
  C.Uid[3] = C.Uid[1];
  FixCapsSetxuid(C, Old);
  return 0;
}

inline int64_t SetResUid(Credentials& C, uint32_t Real, uint32_t Effective, uint32_t Saved) {
  const auto Old = C.Uid;
  if (!C.Capable(CapSetuid)) {
    for (auto V : {Real, Effective, Saved}) {
      if (V != NoId && !Detail::IsOneOf(V, Old, false)) return -1;
    }
  }
  if (Real != NoId) C.Uid[0] = Real;
  if (Effective != NoId) C.Uid[1] = Effective;
  if (Saved != NoId) C.Uid[2] = Saved;
  C.Uid[3] = C.Uid[1];
  FixCapsSetxuid(C, Old);
  return 0;
}

// Returns the old fsuid; an unpermitted or invalid change is silently ignored.
inline int64_t SetFsUid(Credentials& C, uint32_t Uid) {
  const auto Old = C.Uid;
  if (Uid != NoId && (C.Capable(CapSetuid) || Detail::IsOneOf(Uid, Old, true))) {
    C.Uid[3] = Uid;
    FixCapsFs(C, Old);
  }
  return Old[3];
}

inline int64_t SetGid(Credentials& C, uint32_t Gid) {
  if (Gid == NoId) return -22;
  if (C.Capable(CapSetgid)) {
    C.Gid = {Gid, Gid, Gid, Gid};
  } else if (Gid == C.Gid[0] || Gid == C.Gid[2]) {
    C.Gid[1] = C.Gid[3] = Gid;
  } else {
    return -1;
  }
  return 0;
}

inline int64_t SetReGid(Credentials& C, uint32_t Real, uint32_t Effective) {
  const auto Old = C.Gid;
  if (!C.Capable(CapSetgid)) {
    if (Real != NoId && Real != Old[0] && Real != Old[1]) return -1;
    if (Effective != NoId && !Detail::IsOneOf(Effective, Old, false)) return -1;
  }
  if (Real != NoId) C.Gid[0] = Real;
  if (Effective != NoId) C.Gid[1] = Effective;
  if (Real != NoId || (Effective != NoId && Effective != Old[0])) C.Gid[2] = C.Gid[1];
  C.Gid[3] = C.Gid[1];
  return 0;
}

inline int64_t SetResGid(Credentials& C, uint32_t Real, uint32_t Effective, uint32_t Saved) {
  const auto Old = C.Gid;
  if (!C.Capable(CapSetgid)) {
    for (auto V : {Real, Effective, Saved}) {
      if (V != NoId && !Detail::IsOneOf(V, Old, false)) return -1;
    }
  }
  if (Real != NoId) C.Gid[0] = Real;
  if (Effective != NoId) C.Gid[1] = Effective;
  if (Saved != NoId) C.Gid[2] = Saved;
  C.Gid[3] = C.Gid[1];
  return 0;
}

inline int64_t SetFsGid(Credentials& C, uint32_t Gid) {
  const auto Old = C.Gid[3];
  if (Gid != NoId && (C.Capable(CapSetgid) || Detail::IsOneOf(Gid, C.Gid, true))) C.Gid[3] = Gid;
  return Old;
}

// execve capability transform (cap_bprm_creds_from_file / handle_privileged_root).
// SetUidOwner / SetGidOwner: owner of a set-uid / set-gid executable (nullopt if the bit is not set); honoured only
// without NoNewPrivs, then euid/suid/fsuid (egid/sgid/fsgid) become the owner. SetId: the exec changes uid or gid
// for any other reason. File capabilities are not modelled (has_fcap is always false).
// Any effective setid clears ambient. Root (euid or ruid 0, unless SECBIT_NOROOT) gets
// permitted = bounding | inheritable; effective follows only when euid is 0.
inline void ApplyExecCaps(Credentials& C, std::optional<uint32_t> SetUidOwner, bool NoNewPrivs, bool SetId = false,
                          std::optional<uint32_t> SetGidOwner = std::nullopt) {
  bool IsSetId = SetId && !NoNewPrivs;
  if (!NoNewPrivs) {
    if (SetUidOwner) {
      C.Uid[1] = C.Uid[2] = C.Uid[3] = *SetUidOwner;
      IsSetId = true;
    }
    if (SetGidOwner) {
      C.Gid[1] = C.Gid[2] = C.Gid[3] = *SetGidOwner;
      IsSetId = true;
    }
  }
  if (IsSetId) C.CapAmbient = 0;
  uint64_t Permitted = 0;
  bool Effective = false;
  if (!C.Secure(SecbitNoRoot)) {
    if (C.Uid[1] == 0 || C.Uid[0] == 0) Permitted = C.CapBounding | C.CapInheritable;
    if (C.Uid[1] == 0) Effective = true;
  }
  Permitted |= C.CapAmbient;
  C.CapPermitted = Permitted;
  C.CapEffective = Effective ? Permitted : C.CapAmbient;
  C.Securebits &= ~SecbitKeepCaps;
}

// capset(2) permission rules only (no pid/version ABI handling).
inline int64_t CapSet(Credentials& C, uint64_t Effective, uint64_t Permitted, uint64_t Inheritable) {
  if (Inheritable & ~(C.CapInheritable | C.CapBounding)) return -1;
  if (!C.Capable(CapSetpcap) && (Inheritable & ~(C.CapInheritable | C.CapPermitted))) return -1;
  if (Permitted & ~C.CapPermitted) return -1;
  if (Effective & ~Permitted) return -1;
  C.CapEffective = Effective;
  C.CapPermitted = Permitted;
  C.CapInheritable = Inheritable;
  C.CapAmbient &= Permitted & Inheritable;
  return 0;
}
} // namespace Switch2Guest
