// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

namespace Switch2Guest {
inline constexpr char Root[] = "/fex/rootfs";

// Inode metadata and its recovery journal live in one hidden folder so they
// do not clutter /fex. Each name also has the flat path used before 0.12.33.
namespace InodeFiles {
inline constexpr char Directory[] = "/fex/.inode";
inline constexpr char Objects[] = "/fex/.inode/objects";
inline constexpr char DirectoryIntent[] = "/fex/.inode/dir-intent";
inline constexpr char DirectoryBackup[] = "/fex/.inode/dir-backup";
inline constexpr char IntentStaging[] = "/fex/.inode/intent-stage";
inline constexpr char Promotion[] = "/fex/.inode/promotion";
inline constexpr const char* Stores[] {"/fex/.inode/store-0", "/fex/.inode/store-1"};
struct Move { const char* Legacy; const char* Current; };
inline constexpr Move LegacyMoves[] {
  {"/fex/.inode-store-0", Stores[0]}, {"/fex/.inode-store-1", Stores[1]},
  {"/fex/.inode-objects", Objects}, {"/fex/.inode-dir-intent", DirectoryIntent},
  {"/fex/.inode-dir-backup", DirectoryBackup}, {"/fex/.inode-intent-stage", IntentStaging},
  {"/fex/.inode-promotion", Promotion},
};
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
