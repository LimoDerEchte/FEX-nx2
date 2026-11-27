// SPDX-License-Identifier: MIT
// The logic of the file picker without any drawing: locations, start folder, filtering and sorting, and a folder loader that
// reads a directory in small steps (never blocks the main thread). The picker screen (screens/filepicker.cpp) is built on it.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "PathUtil.h"
#include "Storage.h"

namespace ui {

struct BrowseEntry {
  std::string Name;
  bool IsDir = false;
  uint64_t Size = 0;
  int64_t Mtime = 0;
  bool Selectable = true;  // a file that does not match the accepted types is shown at 40% and skipped by D-pad navigation
};

struct BrowseOptions {
  std::vector<std::string> Accept;  // lower-case extensions with the dot; empty = every file matches
  bool FolderMode = false;          // files hidden
};

// Case-insensitive extension match: ".SQSH" matches ".sqsh"; "a.sqsh.sha256" does not match ".sqsh".
bool MatchesAccept(const std::string& Name, const std::vector<std::string>& Accept);
bool IsHiddenName(const std::string& Name);  // dot-files
// Folders first, then files; each A-Z case-insensitive (ties by the exact name).
void SortEntries(std::vector<BrowseEntry>& Entries);
// Turns a directory entry into a list entry; false when it is not shown (dot-file, or a file in folder mode).
bool MakeEntry(const DirEntry& D, const BrowseOptions& O, BrowseEntry& Out);

struct Location {
  std::string Id;     // "sd", "fex", "rootfs", "home"
  std::string Group;  // "Console" or "Linux"
  std::string Name;
  std::string Root;   // sdmc:/ , sdmc:/fex/ , / , /home/<user>/
};
// The four locations of the spec (microSD, FEX folder, RootFS, Home). HomeFolder from the backend.
std::vector<Location> DefaultLocations(const std::string& HomeFolder);
// Index of the most specific location whose root contains Path; the first when none does.
int LocationOf(const std::vector<Location>& L, const std::string& Path);
// The folder the picker opens: the folder of the setting's current value, else the location that fits the setting
// (Fallback: "sd", "fex", "rootfs" or "home"), else sdmc:/.
std::string StartFolder(Storage& S, const std::vector<Location>& L, const std::string& Current, bool FolderMode, const std::string& Fallback);

// Reads one folder in steps. Begin, then Step every frame until Done. Entries are sorted once the read is complete.
class FolderLoader {
public:
  void Begin(Storage& S, const std::string& Path, const BrowseOptions& O, uint64_t Now);
  // Reads up to MaxEntries entries (and stops early on Pending). Returns true when the folder is complete.
  bool Step(int MaxEntries);
  bool Done() const { return Finished; }
  bool Failed() const { return Error; }  // the folder could not be opened or read
  // The spinner replaces the list once the load has taken more than 150 ms.
  bool SpinnerVisible(uint64_t Now) const { return !Finished && Now - Started >= 150; }
  const std::string& Path() const { return Where; }
  std::vector<BrowseEntry>& Entries() { return List; }
  const std::vector<BrowseEntry>& Entries() const { return List; }
  int PendingSteps() const { return PendingCount; }
  void Rebase(uint64_t Now) { Started = Now; }  // the clock of the first frame (the screen is built before it has one)

private:
  std::unique_ptr<DirReader> Reader;
  BrowseOptions Opt;
  std::string Where;
  std::vector<BrowseEntry> List;
  uint64_t Started = 0;
  bool Finished = true, Error = false;
  int PendingCount = 0;
};

} // namespace ui
