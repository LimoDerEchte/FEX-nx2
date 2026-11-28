// SPDX-License-Identifier: MIT
// The static part of the kernel's generated file systems (sysfs and /proc/sys): a table of directories, files and
// symlinks by absolute guest path. The runtime adds every node with its payload (a reader, a writer, a link target)
// once; device nodes that come and go (input devices) are added and removed while the runtime runs. A path below one
// of the tree's roots that has no node does not exist (ENOENT).
//
// Adding a file: Add("/sys/kernel/mm/transparent_hugepage/enabled", Kind::File, 0444, Payload). Missing parent
// directories below the root are created with mode 0555.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Switch2Guest::KernelTree {

enum class Kind : uint8_t { Directory, File, Link };

template<typename Payload>
class Tree {
public:
  struct Node {
    Kind Type {};
    uint32_t Mode {}; // permission bits only (0444, 0644, 0555, 0777, ...)
    Payload Data {};
  };

  // A directory the tree serves completely (for example "/sys" or "/proc/sys"). Nodes are only added below roots.
  void AddRoot(std::string_view Path, uint32_t Mode = 0555) {
    if (!IsRoot(Path)) Roots.emplace_back(Path);
    Nodes[std::string {Path}] = Node {Kind::Directory, Mode, Payload {}};
  }

  // True when Path is a root or lies below one: the tree decides whether it exists.
  bool Covers(std::string_view Path) const {
    return RootOf(Path) != nullptr;
  }

  // Adds or replaces a node. Returns false when Path is not below a root. Missing parents become directories (0555).
  bool Add(std::string_view Path, Kind Type, uint32_t Mode, Payload Data = {}) {
    const auto* Root = RootOf(Path);
    if (!Root || Path.size() == Root->size()) return false;
    for (size_t Slash = Path.find('/', Root->size() + 1); Slash != std::string_view::npos; Slash = Path.find('/', Slash + 1)) {
      const std::string Parent {Path.substr(0, Slash)};
      if (!Nodes.count(Parent)) Nodes[Parent] = Node {Kind::Directory, 0555, Payload {}};
    }
    Nodes[std::string {Path}] = Node {Type, Mode, std::move(Data)};
    return true;
  }

  // Removes a node and everything below it (a root stays).
  void Remove(std::string_view Path) {
    if (IsRoot(Path)) return;
    const std::string Prefix = std::string {Path} + "/";
    for (auto It = Nodes.lower_bound(Prefix); It != Nodes.end() && It->first.starts_with(Prefix);) It = Nodes.erase(It);
    Nodes.erase(std::string {Path});
  }

  const Node* Find(std::string_view Path) const {
    if (!Covers(Path)) return nullptr;
    const auto It = Nodes.find(Path);
    return It == Nodes.end() ? nullptr : &It->second;
  }
  Node* Find(std::string_view Path) {
    return const_cast<Node*>(std::as_const(*this).Find(Path));
  }

  bool IsDirectory(std::string_view Path) const {
    const auto* Found = Find(Path);
    return Found && Found->Type == Kind::Directory;
  }

  // The direct children of a directory, sorted by name.
  std::vector<std::pair<std::string, Kind>> Children(std::string_view Directory) const {
    std::vector<std::pair<std::string, Kind>> Out;
    if (!IsDirectory(Directory)) return Out;
    const std::string Prefix = std::string {Directory} + "/";
    for (auto It = Nodes.lower_bound(Prefix); It != Nodes.end() && It->first.starts_with(Prefix); ++It) {
      const std::string_view Name = std::string_view {It->first}.substr(Prefix.size());
      if (Name.find('/') == std::string_view::npos) Out.emplace_back(std::string {Name}, It->second.Type);
    }
    return Out;
  }

private:
  bool IsRoot(std::string_view Path) const {
    for (const auto& Root : Roots) {
      if (Root == Path) return true;
    }
    return false;
  }
  // The longest root Path lies in (equal to it, or below it at a '/').
  const std::string* RootOf(std::string_view Path) const {
    const std::string* Best {};
    for (const auto& Root : Roots) {
      const bool Inside = Path == Root || (Path.size() > Root.size() && Path.starts_with(Root) && Path[Root.size()] == '/');
      if (Inside && (!Best || Root.size() > Best->size())) Best = &Root;
    }
    return Best;
  }

  std::map<std::string, Node, std::less<>> Nodes;
  std::vector<std::string> Roots;
};

} // namespace Switch2Guest::KernelTree
