// SPDX-License-Identifier: MIT
// File picker (spec "File picker"): header with purpose and accepted types, sidebar of locations (microSD, FEX folder, RootFS,
// Home), path bar with focusable breadcrumbs, a virtualized list (folders first, A-Z, non-matching files at 40% and skipped),
// a spinner when a folder takes longer than 150 ms, folder mode with "+ Use this folder", `sdmc:/...` vs `/...` path forms.
#pragma once

#include <memory>

#include "Backend.h"
#include "FileBrowser.h"
#include "components/Rows.h"
#include "screens/Screen.h"

namespace ui {

namespace picker_layout {
inline constexpr float HeaderH = 88, SidebarW = 300, PathBarY = 102, PathBarH = 40, ListTop = 156, ListBottom = 656;
inline constexpr float ListX = 333, ListW = 899, RowH = 56, RowStep = 58;
inline constexpr int PoolSize = 12;  // ceil(500 / 58) + 3 rows are enough for any scroll position
} // namespace picker_layout

class FilePickerScreen final : public Screen {
public:
  // OnPicked(path) is called with the chosen file or folder (path forms `sdmc:/...` or `/...`), then the screen sends Back.
  FilePickerScreen(LauncherBackend& B, PickerSpec Spec, std::function<void(const std::string&)> OnPicked);
  ~FilePickerScreen() override;

  void Update(uint64_t Now, float Scale, const std::vector<Event>& Raw) override;
  void Draw(const DrawContext& C, uint64_t Now) override;
  Interaction& Input() override { return Interact; }

  // ---- for tests and the host tool ----
  const std::string& CurrentPath() const { return Path; }
  const std::vector<Location>& Locations() const { return Locs; }
  int CurrentLocation() const { return Loc; }
  const std::vector<BrowseEntry>& Entries() const { return Loader.Entries(); }
  bool Loading() const { return !Loader.Done(); }
  bool SpinnerShown(uint64_t Now) const { return Loader.SpinnerVisible(Now); }
  int CursorIndex() const { return Cursor; }
  int VisibleRows() const;          // rows bound to an entry
  int BoundRows() const { return VisibleRows(); }
  FileRow& Slot(int K) { return *Pool[size_t(K)]; }
  ScrollView& ListScroll() { return Scroll; }
  void Open(const std::string& Folder, uint64_t Now, const std::string& Prefer = {});
  void GoLocation(int I, uint64_t Now);
  void GoParent(uint64_t Now);
  void ActivateEntry(int Index, uint64_t Now);
  void Finish(const std::string& Path);
  bool MoveCursor(int Delta, uint64_t Now, bool Wrap = false);   // skips unselectable files; false when it cannot move; Wrap: past an end goes to the other
  void Page(int Dir, uint64_t Now);
  std::string FocusedPath() const;            // the path shown at the left of the HintBar
  HintBar& Hints() { return Bar; }
  const PickerSpec& Spec() const { return Pk; }
  float ListOffset(uint64_t Now) const { return Scroll.Offset(Now); }
  void Relayout(uint64_t Now);
  UpButton& Up() { return UpBtn; }
  int CrumbCount() const { return CrumbsShown; }
  Crumb& CrumbAt(int I) { return *Crumbs[size_t(I)]; }
  SidebarItem& LocItem(int I) { return *LocItems[size_t(I)]; }

private:
  void Rebuild();                 // crumbs and the hint labels after a folder change
  void SyncFocus(uint64_t Now);
  FileRow* SlotOf(int Index);
  bool Selectable(int Index) const;
  void UpdateHints();
  std::string Subtitle() const;

  LauncherBackend& Be;
  PickerSpec Pk;
  std::function<void(const std::string&)> Result;
  Interaction Interact;
  ScrollView Scroll;
  std::vector<Location> Locs;
  std::vector<std::unique_ptr<SidebarItem>> LocItems;
  UpButton UpBtn;
  std::vector<std::unique_ptr<Crumb>> Crumbs;
  int CrumbsShown = 0;
  std::vector<std::string> CrumbPaths;
  std::vector<std::unique_ptr<FileRow>> Pool;
  HintBar Bar;
  FolderLoader Loader;
  BrowseOptions Opt;
  std::string Path;
  std::string Prefer;
  int Loc = 0;
  int Cursor = 0;
  bool ReturnToList = false;  // the folder being loaded was opened from the list
  bool HintsOnFile = false;
  bool HintsAtRoot = false;
  bool Loaded = true;  // the current folder's load finished and the cursor and scroll range were set
  bool ClockSet = false;
  uint64_t Now_ = 0;
  bool CursorLed = false;
};

} // namespace ui
