// SPDX-License-Identifier: MIT
// Settings and About (spec "Settings" and "Settings > About"). Rows come from the SettingsModel's schema table: header 88, sidebar
// 300, scrolling content with scroll-spy, smooth section scroll (200 ms), L/R sections, ZL/ZR page, Restore defaults confirm,
// disabled rows with a reason line. About is the last sidebar item and replaces the right pane with the app block and the
// license lists generated from licenses.json; A on a row opens the LicenseModal and from there the full text view.
// Persistence happens in Commit() (B): atomic writes, unknown keys kept.
#pragma once

#include <memory>

#include "Backend.h"
#include "LicenseStore.h"
#include "SettingsSchema.h"
#include "components/LicenseModal.h"
#include "components/Rows.h"
#include "screens/Screen.h"

namespace ui {

namespace settings_layout {
inline constexpr float HeaderH = 88, SidebarW = 300, ViewTop = 88, ViewH = 568;
inline constexpr float ContentX = 333, ContentW = 899;  // 300 + 1 rule + 32 padding .. 1280 - 48
inline constexpr float RowH = 74, SectionPadTop = 24, SectionHeaderH = 24, TopPad = 8, BottomPad = 48;
inline constexpr float SidebarItemH = 52, SidebarGap = 8, SidebarX = 32, SidebarItemW = 252;
} // namespace settings_layout

class SettingsScreen final : public Screen {
public:
  // `Licenses` null = the embedded licenses.json. The model must outlive the screen.
  SettingsScreen(LauncherBackend& B, SettingsModel& M, SettingsPaths Paths = {}, const std::vector<LicenseEntry>* Licenses = nullptr);
  ~SettingsScreen() override;

  void Update(uint64_t Now, float Scale, const std::vector<Event>& Raw) override;
  void Draw(const DrawContext& C, uint64_t Now) override;
  Interaction& Input() override { return Interact; }

  // Saves the settings (atomic). false and Status text on failure.
  bool Commit();
  // B: Commit, then Back. A failed save keeps the screen open with the error; B again leaves without saving.
  void Leave();

  // ---- for tests and the host tool ----
  int SectionCount() const { return int(Sections.size()); }                // without About
  int Selected() const { return Sel; }                                      // SectionCount() = About
  bool InAbout() const { return Sel == SectionCount(); }
  void SelectSection(int Index, uint64_t Now, bool Scroll = true);
  SettingsRow* RowFor(const std::string& Key);
  SidebarItem& Item(int I) { return *Items[size_t(I)]; }
  float SectionOffset(int I) const;  // scroll offset that puts section I at the top
  float ContentHeight() const;
  ScrollView& Scrolling() { return Scroll; }
  PillButton& RestoreButton() { return Restore; }
  HintBar& Hints() { return Bar; }
  ModalHost& Modals() { return Host; }
  PickerModal& Picker() { return PickerM; }
  ConfirmDialog& Confirm() { return ConfirmM; }
  LicenseModal& LicModal() { return LicM; }
  FullTextView& TextView() { return TextM; }
  LicenseRow* LicRow(int I) { return LicRows[size_t(I)].get(); }
  int LicenseRowCount() const { return int(LicRows.size()); }
  const std::string& StatusText() const { return Bar.Status; }
  void RefreshRows();               // re-reads the model: values, enabled state, reason lines
  void ApplyTheme();
  void OpenRestoreConfirm();
  bool SaveFailed() const { return SaveFailedOnce; }

private:
  struct RowInfo {
    const SettingDef* Def = nullptr;
    int Section = 0;
    std::unique_ptr<SettingsRow> Row;
    Control* Ctl = nullptr;
  };
  void BuildRows();
  void BuildAbout();
  void Layout();
  void SwitchPane(bool About);
  void SetRoot();
  void Press(RowInfo& R);
  void OpenPickerFor(RowInfo& R);
  void RequestPath(RowInfo& R);
  void SectionStep(int Dir, uint64_t Now);
  void PageStep(int Dir, uint64_t Now);
  void FocusFirstRowOf(int Section);
  void UpdateSpy();
  void UpdateAboutLayout(const Frame& F);
  void DrawAbout(const Frame& F);

  LauncherBackend& Be;
  SettingsModel& Model;
  SettingsPaths Paths;
  const std::vector<LicenseEntry>& Licenses;
  Interaction Interact;
  ModalHost Host {Interact};
  ScrollView Scroll;

  std::vector<std::string> Sections;
  std::vector<std::unique_ptr<SidebarItem>> Items;  // sections then About
  std::vector<RowInfo> Rows;
  std::vector<float> SectionStart, SectionHeaderY;
  float SettingsEnd = 0;
  std::vector<std::unique_ptr<LicenseRow>> LicRows;
  float AboutHeaderY[2] = {0, 0}, AboutBlockY = 0, AboutFootY = 0, AboutEnd = 0;
  std::vector<std::string> FootLines;
  float FootScale = 0;
  int FirstBundled = -1, FirstRuntime = -1;
  PillButton Restore;
  HintBar Bar;
  PickerModal PickerM;
  ConfirmDialog ConfirmM;
  LicenseModal LicM;
  FullTextView TextM;
  RowInfo* PickerRow = nullptr;
  Widget* PrevFocus = nullptr;
  int Sel = 0;
  bool AboutPane = false;
  float PaneOffset[2] = {0, 0};
  bool Holding = false;
  float HoldTarget = 0;
  bool SaveFailedOnce = false;
  bool RowsDirty = true;
  uint64_t Now_ = 0;
  int Frames = 0;
  std::string PendingConfirm;  // "restore" or "wipe"
  std::pair<std::string, std::string> DevPending;  // a developer action waiting for the picker's file (Update sends it)
};

} // namespace ui
