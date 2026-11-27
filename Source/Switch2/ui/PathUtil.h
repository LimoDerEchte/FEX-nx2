// SPDX-License-Identifier: MIT
// Path forms of the UI: `sdmc:/...` (microSD card) and `/...` (inside the RootFS). Both always display and are stored in
// this form, so the side is never ambiguous.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ui {

inline constexpr const char* SdPrefix = "sdmc:/";

inline bool IsSdPath(const std::string& P) { return P.rfind(SdPrefix, 0) == 0; }
inline bool IsRootfsPath(const std::string& P) { return !P.empty() && P[0] == '/'; }
inline bool IsValidPath(const std::string& P) { return IsSdPath(P) || IsRootfsPath(P); }

// Collapses "//", "." and ".." (never above the root) and removes a trailing slash except on a root. A path in neither form is
// returned unchanged.
std::string NormalizePath(const std::string& P);
// The folder that contains P; a root is its own parent.
std::string ParentPath(const std::string& P);
inline bool IsRootPath(const std::string& P) { return P == "sdmc:/" || P == "/"; }
std::string JoinPath(const std::string& Dir, const std::string& Name);
std::string BaseName(const std::string& P);

struct PathCrumb {
  std::string Label;  // "microSD", "RootFS" for the roots, else the folder name
  std::string Path;   // the folder this crumb opens (a root keeps its slash)
};
// Root first: "sdmc:/fex" -> microSD, fex.
std::vector<PathCrumb> Breadcrumbs(const std::string& P);

// Human readable sizes and dates: "1.5 KB", "18.4 GB"; "2026-11-25".
std::string FormatBytes(uint64_t Bytes);
std::string FormatDate(int64_t UnixSeconds);  // empty when 0

} // namespace ui
