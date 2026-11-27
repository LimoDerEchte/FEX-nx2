// SPDX-License-Identifier: MIT
// LicenseModal (640 wide, same scrim and radius as PickerModal: name + license chip, obligation note, a log_bg box with the
// source and text path, Close) and FullTextView (the full-screen scrollable license text opened from it).
#pragma once

#include "LicenseStore.h"
#include "components/Modals.h"

namespace ui {

// Word wraps Text to Width du in the given style (explicit newlines kept; a word longer than the line is cut).
std::vector<std::string> WrapLines(const Frame& F, const TextStyle& St, const std::string& Text, float Width);

class LicenseModal final : public ModalBase {
public:
  LicenseModal();
  LicenseEntry Entry;
  bool HasText = false;           // the embedded text exists: "Read license text" is offered
  std::function<void()> OnReadText;
  void Open(const LicenseEntry& E, uint64_t Now);
  void Layer(std::vector<Widget*>& Out) override;
  Widget* InitialFocus() override { return &Close; }
  void Draw(const Frame& F) override;
  PillButton& ClosePill() { return Close; }
  PillButton& ReadPill() { return Read; }

private:
  PillButton Close, Read;
  Rect Dialog;
};

class FullTextView final : public ModalBase {
public:
  FullTextView();
  std::string Title, Subtitle, Text;
  std::function<void(Button)> OnHint;   // a tapped hint (B, ZL, ZR)
  void Open(uint64_t Now);
  void Layer(std::vector<Widget*>& Out) override;
  Widget* InitialFocus() override { return &Close; }
  void Draw(const Frame& F) override;
  void ScrollBy(float Dy);
  void Page(int Dir);                   // -1 up, +1 down
  float Offset() const { return Off; }
  float MaxOffset() const { return std::max(0.0f, ContentH - ViewH); }
  size_t LineCount() const { return Lines.size(); }

private:
  PillButton Close;
  HintBar Hints;
  std::vector<std::string> Lines;
  float WrapScale = 0;
  float Off = 0, ContentH = 0, ViewH = 568, LineH = 22;
};

} // namespace ui
