// SPDX-License-Identifier: MIT
// PickerModal (scrim + 520 wide dialog, option rows, Cancel) and ConfirmDialog (same frame, 560 wide, Cancel focused by
// default, accent or error confirm). Fade 120 ms. B, a tap on the scrim, or Cancel closes with OnCancel.
#pragma once

#include <memory>

#include "components/Basic.h"

namespace ui {

// A hittable, invisible widget (scrim and dialog panel): the scrim cancels, the panel swallows taps.
class HitArea final : public Widget {
public:
  HitArea() { Focusable = false; Fixed = true; }
  void Draw(const Frame&) override {}
};

class ModalBase {
public:
  virtual ~ModalBase() = default;
  std::function<void()> OnCancel;

  void Show(uint64_t Now);
  void Hide(uint64_t Now);
  bool IsOpen() const { return Opened; }
  bool Visible(uint64_t Now) const { return Opened || Alpha.Value(Now) > 0.001f; }
  float FadeValue(uint64_t Now) const { return Alpha.Value(Now); }
  // The interactive widgets (scrim first, panel, then the controls) for Interaction::SetLayer.
  virtual void Layer(std::vector<Widget*>& Out) = 0;
  virtual Widget* InitialFocus() = 0;
  virtual void Draw(const Frame& F) = 0;

protected:
  ModalBase();
  void DrawScrim(const Frame& F, Rect DialogDu, float K);
  HitArea Scrim, Panel;
  Tween Alpha;
  bool Opened = false;
};

class PickerModal final : public ModalBase {
public:
  PickerModal();
  ~PickerModal() override;
  std::string Title;
  std::vector<std::string> Options;
  int Selected = 0;
  std::function<void(int)> OnSelect;

  void Open(uint64_t Now);  // builds the rows from Options and fades in
  void Pick(int Index, uint64_t Now);
  void Layer(std::vector<Widget*>& Out) override;
  Widget* InitialFocus() override;  // the current value's row
  void Draw(const Frame& F) override;

private:
  class Row;
  std::vector<std::unique_ptr<Row>> Rows;
  PillButton Cancel;
  Rect Dialog;
  void Layout(const Frame& F);
};

class ConfirmDialog final : public ModalBase {
public:
  ConfirmDialog();
  std::string Title, Message, ConfirmText = "Confirm";
  std::vector<std::string> Summary;  // optional lines in a `log_bg` box under the message (mono_log)
  bool Destructive = false;  // error fill instead of accent
  std::function<void()> OnConfirm;

  void Open(uint64_t Now);
  void Layer(std::vector<Widget*>& Out) override;
  Widget* InitialFocus() override;  // Cancel
  void Draw(const Frame& F) override;

private:
  PillButton Cancel, Confirm;
  Rect Dialog;
  std::vector<std::string> Lines;
};

// A list dialog with one Close button (the Shell's controls help): Title, then Rows of a mono key column and a body
// description. B, the scrim or Close closes it with OnCancel.
class InfoDialog final : public ModalBase {
public:
  InfoDialog();
  std::string Title;
  std::vector<std::pair<std::string, std::string>> Rows;

  void Open(uint64_t Now) { Show(Now); }
  void Layer(std::vector<Widget*>& Out) override;
  Widget* InitialFocus() override { return &Close; }
  void Draw(const Frame& F) override;

private:
  PillButton Close;
  Rect Dialog;
};

} // namespace ui
