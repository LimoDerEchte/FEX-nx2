// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Switch2Guest {
inline constexpr std::string_view AccessAcl = "system.posix_acl_access";
inline constexpr std::string_view DefaultAcl = "system.posix_acl_default";

struct PosixAclLayout {
  size_t Owner {}, Group {}, Mask {}, Other {};
  bool Extended {};

  uint32_t Mode(std::string_view Bytes) const {
    return (uint32_t(static_cast<uint8_t>(Bytes[Owner])) << 6) |
           (uint32_t(static_cast<uint8_t>(Bytes[Mask ? Mask : Group])) << 3) |
           uint32_t(static_cast<uint8_t>(Bytes[Other]));
  }

  template<typename InGroup>
  bool Allows(std::string_view Bytes, uint32_t OwnerID, uint32_t GroupID,
              uint32_t UID, InGroup IsMember, unsigned Requested) const {
    const auto Permission = [&](size_t At) { return static_cast<uint8_t>(Bytes[At]); };
    const auto AllowsBits = [&](unsigned Bits) { return (Bits & Requested) == Requested; };
    if (UID == OwnerID) return AllowsBits(Permission(Owner));
    const unsigned MaskBits = Mask ? Permission(Mask) : 7;
    bool MatchedGroup = IsMember(GroupID);
    bool GroupAllows = MatchedGroup && AllowsBits(Permission(Group) & MaskBits);
    for (size_t At = 4; At + 8 <= Bytes.size(); At += 8) {
      const auto Tag = Permission(At);
      if (Tag != 2 && Tag != 8) continue;
      uint32_t ID {};
      for (unsigned Byte = 0; Byte < 4; ++Byte) ID |= uint32_t(Permission(At + 4 + Byte)) << (8 * Byte);
      if (Tag == 2 && ID == UID) return AllowsBits(Permission(At + 2) & MaskBits);
      if (Tag == 8 && IsMember(ID)) {
        MatchedGroup = true;
        GroupAllows |= AllowsBits(Permission(At + 2) & MaskBits);
      }
    }
    return MatchedGroup ? GroupAllows : AllowsBits(Permission(Other));
  }

  void ApplyMode(std::string& Bytes, uint32_t Mode, bool Restrict) const {
    const auto Update = [&](size_t At, unsigned Shift) {
      const auto Permission = (Mode >> Shift) & 7;
      Bytes[At] = Restrict ? static_cast<uint8_t>(Bytes[At]) & Permission : Permission;
    };
    Update(Owner, 6);
    Update(Mask ? Mask : Group, 3);
    Update(Other, 0);
  }
};

inline std::optional<PosixAclLayout> ParsePosixAcl(std::string_view Bytes) {
  const auto Word = [&](size_t At, unsigned Count) {
    uint32_t Value {};
    for (unsigned I = 0; I < Count; ++I) Value |= uint32_t(static_cast<uint8_t>(Bytes[At + I])) << (I * 8);
    return Value;
  };
  if (Bytes.size() < 28 || (Bytes.size() - 4) % 8 || Word(0, 4) != 2) return std::nullopt;
  PosixAclLayout Layout;
  uint32_t LastTag {}, LastID {};
  bool Named {};
  for (size_t At = 4; At < Bytes.size(); At += 8) {
    const auto Tag = Word(At, 2);
    const auto ID = Word(At + 4, 4);
    if (Tag < LastTag || Word(At + 2, 2) > 7) return std::nullopt;
    if (Tag == 2 || Tag == 8) {
      if (ID == UINT32_MAX || (Tag == LastTag && ID <= LastID)) return std::nullopt;
      Named = true;
    } else {
      if (ID != UINT32_MAX || Tag == LastTag) return std::nullopt;
      switch (Tag) {
      case 1: Layout.Owner = At + 2; break;
      case 4: Layout.Group = At + 2; break;
      case 16: Layout.Mask = At + 2; break;
      case 32: Layout.Other = At + 2; break;
      default: return std::nullopt;
      }
    }
    LastTag = Tag;
    LastID = ID;
  }
  if (!Layout.Owner || !Layout.Group || !Layout.Other || (Named && !Layout.Mask)) return std::nullopt;
  Layout.Extended = Layout.Mask != 0;
  return Layout;
}
} // namespace Switch2Guest
