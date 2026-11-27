// SPDX-License-Identifier: MIT
// The RootFS image index. The built-in index (assets/rootfs-index.json, embedded in the NRO) has the RootFS_links.json "v1"
// format of the FEX RootFS project (DistroMatch, DistroVersion, URL, Hash, Type) plus the fields the UI needs: Name,
// DownloadSize, InstalledSize, ExtraDownloadSize, Extras, and Available/Reason for an entry that cannot be installed (drawn as a
// disabled card). "Check for newer images" parses the project's live RootFS_links.json with the same parser and offers a newer
// entry of the same distro and version. The console has no TLS: every URL is used as http:// (HttpUrl). Hash is the hex XXH3-64
// of the whole image file (leading zeros may be dropped). Over plain HTTP the hash checks integrity, not authenticity.
// No Vulkan or libnx types here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ui {

inline constexpr const char* LiveIndexUrl = "http://rootfs.fex-emu.gg/RootFS_links.json";

struct RootfsImage {
  std::string Key;            // the index key, "Ubuntu 24.04 (SquashFS)"
  std::string Name;           // "Ubuntu 24.04"
  std::string DistroMatch;    // "ubuntu"
  std::string DistroVersion;  // "24.04"
  std::string Url;            // http://...
  std::string Hash;           // hex XXH3-64
  std::string Type;           // "squashfs" or "erofs"
  std::string FileName;       // last path component of the URL
  uint64_t DownloadBytes = 0;  // 0 = unknown (learned from the server)
  uint64_t InstalledBytes = 0;
  uint64_t ExtraDownloadBytes = 0;
  std::vector<std::string> Extras;
  bool Available = true;
  std::string Reason;         // shown when not available
  bool Selectable() const { return Available && Type == "squashfs" && !Url.empty(); }
};

struct RootfsIndex {
  std::vector<RootfsImage> Images;
  const RootfsImage* Find(const std::string& DistroMatch, const std::string& DistroVersion) const;
};

// "https://host/x" -> "http://host/x"; anything else unchanged.
std::string HttpUrl(const std::string& Url);
std::string UrlFileName(const std::string& Url);

// Parses an index (built-in or live). Entries of an unknown Type are kept (not selectable); an entry with a malformed hash or URL
// is skipped. False with an error when the document is not a v1 index.
bool ParseRootfsIndex(const std::string& JsonText, RootfsIndex& Out, std::string& Error);

// The embedded index; the process-wide copy parsed once. Never empty: a broken embedded file fails the host tests.
const RootfsIndex& BuiltInRootfsIndex();

// The index the UI shows: the built-in one. `LiveText` is the downloaded RootFS_links.json (empty when the fetch failed or was
// not asked for); a live entry that is newer than the built-in entry of the same distro and version is offered through
// FindNewer, and a failed or unparsable live index falls back to the built-in one (Note says why).
struct IndexChoice {
  RootfsIndex Index;
  bool LiveUsed = false;
  std::string Note;
};
IndexChoice ChooseIndex(const std::string& LiveText);

// A live squashfs entry newer than Current (same DistroMatch and DistroVersion, a different URL whose date folder sorts
// later, valid hash, the same URL host as Current). The sizes of Current carry over (the download size is learned from the server).
bool FindNewer(const RootfsImage& Current, const RootfsIndex& Live, RootfsImage& Out);

} // namespace ui
