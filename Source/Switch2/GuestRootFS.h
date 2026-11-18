// SPDX-License-Identifier: MIT
#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace Switch2Guest {
inline constexpr char Root[] = "/fex/rootfs";

// Absolute Linux paths live inside the rootfs. Relative paths retain the ELF's
// working directory. Reject traversal and device aliases before calling FS.
inline std::optional<std::string> ResolvePath(std::string_view Path, std::string_view Folder, std::string_view RootPath = Root) {
  if (Path.empty() || Path.find_first_of(":\\") != Path.npos) {
    return std::nullopt;
  }
  const auto Mount = Path.starts_with('/') ? RootPath : Folder;
  if (Path.starts_with('/')) {
    Path.remove_prefix(1);
  }
  std::string Resolved {Mount};
  while (!Path.empty()) {
    const auto Slash = Path.find('/');
    const auto Part = Path.substr(0, Slash);
    if (Part == "..") {
      return std::nullopt;
    }
    if (!Part.empty() && Part != ".") {
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
