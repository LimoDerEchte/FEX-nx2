// SPDX-License-Identifier: MIT
// Home (spec "Home"): VersionBlock and gear in the header, title block, three launch cards (x = 164 / 500 / 836, gap 56), the FTP
// pill and the HintBar. Up from a card goes to the gear, gear Down goes back to the last card, Down from a card goes to the pill.
// Steam and Desktop are disabled with the backend's reason. B exits.
#pragma once

#include <memory>

#include "Backend.h"
#include "components/Rows.h"
#include "screens/Screen.h"

namespace ui {

// The exact numbers of the spec's Home layout (design units).
namespace home_layout {
inline constexpr float CardX[3] = {164, 500, 836};
inline constexpr float CardY = 235;       // "centred in the remaining space, about y 235"
inline constexpr float CardW = 280, CardH = 300, CardGap = 56;
inline constexpr float HeaderTop = 32, HeaderSide = 48, HeaderH = 56;
inline constexpr float PillGapBelowCards = 32;  // console: 56 put the pill's focus ring on the footer rule
inline constexpr float PillH = 52;
} // namespace home_layout

class HomeScreen final : public Screen {
public:
  explicit HomeScreen(LauncherBackend& B);
  ~HomeScreen() override;

  void Update(uint64_t Now, float Scale, const std::vector<Event>& Raw) override;
  void Draw(const DrawContext& C, uint64_t Now) override;
  Interaction& Input() override { return Interact; }

  // For tests and the host tool.
  LaunchCard& Card(int I) { return *Cards[size_t(I)]; }
  IconButton& Gear() { return *GearBtn; }
  FtpPill& Pill() { return *Pill_; }
  HintBar& Hints() { return *Bar; }
  ModalHost& Modals() { return Host; }
  void Refresh();                       // pulls RootFS, FTP and launch capabilities from the backend
  void Layout(const Frame& F);          // positions the pill (needs fonts) and the title block
  int LastCardFocused() const { return LastCard; }

private:
  LauncherBackend& Be;
  Interaction Interact;
  ModalHost Host {Interact};
  std::vector<std::unique_ptr<LaunchCard>> Cards;
  std::unique_ptr<IconButton> GearBtn;
  std::unique_ptr<VersionBlock> Version;
  std::unique_ptr<FtpPill> Pill_;
  std::unique_ptr<HintBar> Bar;
  int LastCard = 0;
  uint64_t Now_ = 0;
  float TitleBottom = 0;
};

} // namespace ui
