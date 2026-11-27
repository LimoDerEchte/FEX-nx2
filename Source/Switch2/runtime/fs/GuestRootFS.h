// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace Switch2Guest {
inline constexpr char Root[] = "/fex/rootfs";
// The default user's home inside the RootFS (user fex, uid 1000, added by the RootFS install). Launches as that user start here,
// like a login: the system folders belong to root.
inline constexpr char DefaultHome[] = "/home/fex";

// Inode metadata and its recovery journal live in one hidden folder so they
// do not clutter /fex. Launchers before 0.15.1 used /fex/.inode; its
// contents are not migrated.
namespace InodeFiles {
inline constexpr char Directory[] = "/fex/.inode";
inline constexpr char Objects[] = "/fex/.inode/objects";
inline constexpr char DirectoryIntent[] = "/fex/.inode/dir-intent";
inline constexpr char DirectoryBackup[] = "/fex/.inode/dir-backup";
inline constexpr char IntentStaging[] = "/fex/.inode/intent-stage";
inline constexpr char Promotion[] = "/fex/.inode/promotion";
// A staging entry left without its journal intent is moved here, never
// deleted, so a launch that crashed mid-operation cannot lose data.
inline constexpr char Orphans[] = "/fex/.inode/orphans";
inline constexpr const char* Stores[] {"/fex/.inode/store-0", "/fex/.inode/store-1"};
// Frames appended since the newest checkpoint in Stores (GuestInodeStore.h).
inline constexpr char Journal[] = "/fex/.inode/journal";
} // namespace InodeFiles

// Absolute Linux paths live inside the rootfs. Relative paths retain the ELF's
// working directory. Absolute Linux paths clamp parent traversal at the root.
inline std::optional<std::string> ResolvePath(std::string_view Path, std::string_view Folder, std::string_view RootPath = Root) {
  if (Path.empty() || Path.find_first_of(":\\") != Path.npos) {
    return std::nullopt;
  }
  const bool Absolute = Path.starts_with('/');
  const auto Mount = Absolute ? RootPath : Folder;
  if (Path.starts_with('/')) {
    Path.remove_prefix(1);
  }
  std::string Resolved {Mount};
  while (!Path.empty()) {
    const auto Slash = Path.find('/');
    const auto Part = Path.substr(0, Slash);
    if (Part == "..") {
      if (!Absolute) return std::nullopt;
      if (Resolved.size() > Mount.size()) Resolved.resize(std::max(Mount.size(), Resolved.rfind('/')));
    } else if (!Part.empty() && Part != ".") {
      if (!Resolved.ends_with('/')) {
        Resolved += '/';
      }
      Resolved += Part;
    }
    if (Slash == Path.npos) {
      break;
    }
    Path.remove_prefix(Slash + 1);
  }
  return Resolved;
}
} // namespace Switch2Guest
