// SPDX-License-Identifier: MIT
// RootFS setup (spec "RootFS setup"): opens on first run (no RootFS) and from Settings > Reinstall RootFS. Header with the free-space
// pill, sections of cards (base system, desktop environments, game stores, install options), a footer with the live download size
// and the hints. Only what the installer can really do is selectable: Ubuntu 24.04 from the FEX RootFS SquashFS, "None" for the
// desktop, the extra packages and an image file from the SD card; every other card is drawn disabled with "Not available yet".
// + Install starts Installation at once on a first run; in reinstall mode it asks "Reinstall RootFS?" first (Cancel focused).
#pragma once

#include <memory>

#include "InstallModel.h"
#include "RootfsSelection.h"
#include "components/Tiles.h"
#include "screens/Screen.h"

namespace ui {

namespace setup_layout {
inline constexpr float HeaderH = 88, ViewTop = 88, ViewH = 568, SideX = 48, ContentW = 1184, SectionGap = 24, Pad = 20;
inline constexpr float BannerH = 56;
} // namespace setup_layout

class RootfsSetupScreen final : public Screen {
public:
  RootfsSetupScreen(LauncherBackend& B, bool Reinstall, SettingsPaths Paths = {});
  ~RootfsSetupScreen() override;

  void Update(uint64_t Now, float Scale, const std::vector<Event>& Raw) override;
  void Draw(const DrawContext& C, uint64_t Now) override;
  Interaction& Input() override { return Interact; }

  struct CardInfo {
    std::unique_ptr<OptionCard> Card;
    int Section = 0;        // 0 base, 1 desktop, 2 stores, 3 options
    std::string Key;        // "ubuntu-24.04", "none", "extras", "local-image"
    bool Real = false;      // selectable
    const RootfsImage* Image = nullptr;
  };
  // ---- tests and the host tool ----
  bool ReinstallMode() const { return Reinstall; }
  int CardCount() const { return int(Cards.size()); }
  CardInfo& Card(int I) { return Cards[size_t(I)]; }
  CardInfo* CardByKey(const std::string& Key);
  const RootfsSelection& Selection() const { return Sel; }
  HintBar& Hints() { return Bar; }
  ConfirmDialog& Confirm() { return ConfirmM; }
  ModalHost& Modals() { return Host; }
  ScrollView& Scrolling() { return Scroll; }
  uint64_t FreeBytes() const { return Free; }
  uint64_t NeedBytes() const;
  uint64_t DownloadBytes() const;
  bool EnoughSpace() const { return Free >= NeedBytes(); }
  std::string FooterText() const;
  const std::string& LocalImage() const { return Local; }
  InstallRequest BuildRequest() const;
  void Select(CardInfo& C);       // A on a card
  void PressInstall();            // + Install / + Reinstall
  void Leave();                   // B

private:
  void Build();
  void Layout();
  void Refresh();
  void SaveSelection();
  void StartInstall();
  const RootfsImage* ChosenImage() const;

  LauncherBackend& Be;
  bool Reinstall;
  SettingsPaths Paths;
  RootfsSelection Sel;
  Interaction Interact;
  ModalHost Host {Interact};
  ScrollView Scroll;
  std::vector<CardInfo> Cards;
  HintBar Bar;
  ConfirmDialog ConfirmM;
  std::string Local;
  uint64_t Free = 0, Total = 0;
  float SectionY[4] = {0, 0, 0, 0}, ContentEnd = 0, BannerY = 0;
  uint64_t Now_ = 0;
};

} // namespace ui
