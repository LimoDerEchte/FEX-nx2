// SPDX-License-Identifier: MIT
// Component gallery (1280x720 du): every shared component in every state (normal, focused, pressed, disabled), driven by
// the real input path. Header: accent and theme choosers (PickerModal). Content scrolls (focus scrolls into view; wheel, touch
// drag and right stick scroll). HintBar at the bottom. The console probe runs this page; `make ui-host --screen gallery`
// renders it headless.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "components/Controls.h"
#include "components/Modals.h"
#include "components/Progress.h"
#include "input/Interaction.h"
#include "screens/Screen.h"

namespace ui {

class Gallery final : public Screen {
public:
  Gallery();
  ~Gallery() override;

  // Input for one frame: the raw events, the clock and the output scale. Applies theme changes made on the page.
  void Update(uint64_t Now, float Scale, const std::vector<Event>& Raw) override;
  void Draw(const DrawContext& C, uint64_t Now) override;

  Interaction& Input() override { return Interact; }
  const std::string& LastAction() const { return Last; }
  void LastActionReset() { Last = "none"; }
  // For tests: widgets by role.
  PillButton* FirstPill();
  Slider* FirstSlider();
  EnumChip* FirstEnum();
  SettingsRow* RowFor(Control* C);
  struct Parts;
  void OpenPicker(uint64_t Now);
  void OpenConfirm(bool Destructive, uint64_t Now);
  void CloseModal(uint64_t Now);
  ModalBase* ActiveModal() { return Active; }
  ScrollView& Scrolling() { return Scroll; }
  std::string ExtraStatus;  // appended to the HintBar status (the probe adds its page hint)

private:
  void Build();
  void SetStatus(uint64_t Now);
  void OpenFor(ModalBase& M, uint64_t Now);

  Interaction Interact;
  ScrollView Scroll;
  std::unique_ptr<Parts> Pt;
  ModalBase* Active = nullptr;
  EnumChip* PickerFor = nullptr;
  std::string Last = "none";
  uint64_t LastLogAt = 0;
  int LogCount = 0;
  uint64_t Now_ = 0;
};

} // namespace ui
