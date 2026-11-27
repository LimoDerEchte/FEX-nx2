// SPDX-License-Identifier: MIT
#include "components/Modals.h"

#include <algorithm>
#include <sstream>

namespace ui {

constexpr float ScreenW = 1280, ScreenH = 720;

ModalBase::ModalBase() {
  Scrim.Bounds = Rect {0, 0, ScreenW, ScreenH};
  Scrim.OnActivate = [this]() {
    if (OnCancel)
      OnCancel();
  };
  Panel.OnActivate = []() {};
  Alpha.Set(0);
}

void ModalBase::Show(uint64_t Now) {
  Opened = true;
  Alpha.Go(1, Now, 120);
}
void ModalBase::Hide(uint64_t Now) {
  Opened = false;
  Alpha.Go(0, Now, 120);
}

void ModalBase::DrawScrim(const Frame& F, Rect DialogDu, float K) {
  const Theme& T = F.T();
  const float S = F.S();
  const Size Sz = F.Dc.R.TargetSize();
  F.Dc.R.FillRect(Rect {0, 0, float(Sz.W), float(Sz.H)}, Opacity(T.Scrim, K));
  F.Dc.R.FillRoundRect(F.Box(DialogDu), 24 * S, Opacity(T.Surface, K));
}

// ---- PickerModal --------------------------------------------------------------------------------------------------------

class PickerModal::Row final : public Widget {
public:
  PickerModal* Owner = nullptr;
  int Index = 0;
  std::string Text;
  bool Selected = false;
  void Activate(uint64_t Now) override { Owner->Pick(Index, Now); }
  float RingOutset() const override { return 4; }
  float RingStroke() const override { return 3; }
  float RingRadius() const override { return 12; }
  void Draw(const Frame& F) override {
    const Theme& T = F.T();
    const float S = F.S();
    const float O = Op();
    const Rect B = F.Box(Bounds);
    if (Selected || Pressed)
      F.Dc.R.FillRoundRect(B, 12 * S, Opacity(T.SurfaceInset, O));
    const float Cy = Bounds.Y + Bounds.H / 2;
    const Rect Radio = F.Box(Bounds.X + 16, Cy - 11, 22, 22);
    F.Dc.R.StrokeRoundRect(Radio, Radio.H / 2, std::max(1.0f, std::round(2 * S)), Opacity(Selected ? T.Accent : T.RadioOff, O));
    if (Selected)
      F.Dc.R.FillCircle(Radio.X + Radio.W / 2, Radio.Y + Radio.H / 2, std::round(5 * S), Opacity(T.Accent, O));
    F.Text(styles::RowLabel, Text, Bounds.X + 16 + 22 + 14, Bounds.Y, Bounds.H, Opacity(T.Text, O));
  }
};

PickerModal::PickerModal() {
  Cancel.Text = "Cancel";
  Cancel.K = PillButton::Kind::Secondary;
  Cancel.Bounds.H = 48;
  Cancel.Fixed = true;
  Cancel.OnActivate = [this]() {
    if (OnCancel)
      OnCancel();
  };
}
PickerModal::~PickerModal() = default;

void PickerModal::Open(uint64_t Now) {
  Rows.clear();
  for (size_t I = 0; I < Options.size(); ++I) {
    auto R = std::make_unique<Row>();
    R->Owner = this;
    R->Index = int(I);
    R->Text = Options[I];
    R->Selected = int(I) == Selected;
    R->Fixed = true;
    Rows.push_back(std::move(R));
  }
  Show(Now);
}

void PickerModal::Pick(int Index, uint64_t Now) {
  Selected = Index;
  if (OnSelect)
    OnSelect(Index);
  (void)Now;
}

void PickerModal::Layer(std::vector<Widget*>& Out) {
  Out.push_back(&Scrim);
  Out.push_back(&Panel);
  for (auto& R : Rows)
    Out.push_back(R.get());
  Out.push_back(&Cancel);
}

Widget* PickerModal::InitialFocus() {
  for (auto& R : Rows)
    if (R->Index == Selected)
      return R.get();
  return Rows.empty() ? static_cast<Widget*>(&Cancel) : Rows[0].get();
}

void PickerModal::Layout(const Frame& F) {
  const float S = F.S();
  const float TitleH = F.Dc.F.LineHeightPx(cstyles::ModalTitle, S) / S;
  const float N = float(Rows.size());
  const float H = 28 + TitleH + 12 + N * 56 + std::max(0.0f, N - 1) * 8 + 12 + 48 + 28;
  Dialog = Rect {(ScreenW - 520) / 2, std::round((ScreenH - H) / 2), 520, H};
  float Y = Dialog.Y + 28 + TitleH + 12;
  for (auto& R : Rows) {
    R->Bounds = Rect {Dialog.X + 28, Y, 464, 56};
    Y += 56 + 8;
  }
  Y += -8 + 12;
  Cancel.FixedWidth = 464;
  Cancel.Bounds.X = Dialog.X + 28;
  Cancel.Bounds.Y = Y;
  Panel.Bounds = Dialog;
}

void PickerModal::Draw(const Frame& Fr) {
  const Frame F = Fr.Fixed();
  const float K = Alpha.Value(F.Now);
  if (K <= 0.001f && !Opened)
    return;
  Layout(F);
  for (auto& R : Rows) {
    R->Fade = K;
    R->Selected = R->Index == Selected;
  }
  Cancel.Fade = K;
  DrawScrim(F, Dialog, K);
  F.Text(cstyles::ModalTitle, Title, Dialog.X + 28, Dialog.Y + 28, F.Dc.F.LineHeightPx(cstyles::ModalTitle, F.S()) / F.S(), Opacity(F.T().Text, K));
  for (auto& R : Rows)
    R->Draw(F);
  Cancel.Draw(F);
}

// ---- ConfirmDialog ------------------------------------------------------------------------------------------------------

ConfirmDialog::ConfirmDialog() {
  Cancel.Text = "Cancel";
  Cancel.K = PillButton::Kind::Secondary;
  Cancel.Bounds.H = Confirm.Bounds.H = 48;
  Cancel.Fixed = Confirm.Fixed = true;
  Cancel.OnActivate = [this]() {
    if (OnCancel)
      OnCancel();
  };
  Confirm.OnActivate = [this]() {
    if (OnConfirm)
      OnConfirm();
  };
}

void ConfirmDialog::Open(uint64_t Now) {
  Confirm.Text = ConfirmText;
  Confirm.K = Destructive ? PillButton::Kind::Error : PillButton::Kind::Accent;
  Show(Now);
}

void ConfirmDialog::Layer(std::vector<Widget*>& Out) {
  Out.push_back(&Scrim);
  Out.push_back(&Panel);
  Out.push_back(&Confirm);
  Out.push_back(&Cancel);  // Cancel last: the default focus and the last stop
}

Widget* ConfirmDialog::InitialFocus() { return &Cancel; }

void ConfirmDialog::Draw(const Frame& Fr) {
  const Frame F = Fr.Fixed();
  const float K = Alpha.Value(F.Now);
  if (K <= 0.001f && !Opened)
    return;
  const float S = F.S();
  const float W = 560, TextW = W - 56;
  // Word wrap the message to the dialog's text width.
  Lines.clear();
  {
    std::istringstream In(Message);
    std::string Word, Cur;
    while (In >> Word) {
      const std::string Try = Cur.empty() ? Word : Cur + " " + Word;
      if (!Cur.empty() && F.Measure(styles::Body, Try) > TextW) {
        Lines.push_back(Cur);
        Cur = Word;
      } else {
        Cur = Try;
      }
    }
    if (!Cur.empty())
      Lines.push_back(Cur);
  }
  const float TitleH = F.Dc.F.LineHeightPx(cstyles::ModalTitle, S) / S, BodyH = F.Dc.F.LineHeightPx(styles::Body, S) / S;
  const float SumLineH = F.Dc.F.LineHeightPx(styles::MonoLog, S) / S;
  const float SumH = Summary.empty() ? 0 : float(Summary.size()) * SumLineH + 28 + 16;
  const float H = 28 + TitleH + 12 + float(Lines.size()) * (BodyH + 4) + SumH + 20 + 48 + 28;
  Dialog = Rect {(ScreenW - W) / 2, std::round((ScreenH - H) / 2), W, H};
  Panel.Bounds = Dialog;
  Confirm.Layout(F);
  Cancel.Layout(F);
  Cancel.Bounds.W = std::max(Cancel.Bounds.W, 120.0f);
  Confirm.Bounds.W = std::max(Confirm.Bounds.W, 120.0f);
  const float By = Dialog.Y + H - 28 - 48;
  Confirm.Bounds.X = Dialog.X + W - 28 - Confirm.Bounds.W;
  Confirm.Bounds.Y = By;
  Cancel.Bounds.X = Confirm.Bounds.X - 12 - Cancel.Bounds.W;
  Cancel.Bounds.Y = By;
  Confirm.FixedWidth = Confirm.Bounds.W;
  Cancel.FixedWidth = Cancel.Bounds.W;
  Confirm.Fade = Cancel.Fade = K;
  DrawScrim(F, Dialog, K);
  F.Text(cstyles::ModalTitle, Title, Dialog.X + 28, Dialog.Y + 28, TitleH, Opacity(F.T().Text, K));
  float Y = Dialog.Y + 28 + TitleH + 12;
  for (const std::string& L : Lines) {
    F.Text(styles::Body, L, Dialog.X + 28, Y, BodyH, Opacity(F.T().TextMuted, K));
    Y += BodyH + 4;
  }
  if (!Summary.empty()) {
    Y += 4;
    const Rect Box = F.Box(Dialog.X + 28, Y, TextW, float(Summary.size()) * SumLineH + 28);
    F.Dc.R.FillRoundRect(Box, 12 * S, Opacity(F.T().LogBg, K));
    F.Dc.R.StrokeRoundRect(Box, 12 * S, float(HairlinePx(S)), Opacity(F.T().Divider, K));
    for (size_t I = 0; I < Summary.size(); ++I)
      F.Text(styles::MonoLog, F.Dc.F.Fit(styles::MonoLog, Summary[I], S, (TextW - 32) * S, Ellipsis::End), Dialog.X + 28 + 16, Y + 14 + float(I) * SumLineH, SumLineH, Opacity(F.T().TextField, K));
  }
  Cancel.Draw(F);
  Confirm.Draw(F);
}

// ---- InfoDialog ---------------------------------------------------------------------------------------------------------

InfoDialog::InfoDialog() {
  Close.Text = "Close";
  Close.K = PillButton::Kind::Accent;
  Close.Bounds.H = 48;
  Close.Fixed = true;
  Close.OnActivate = [this]() {
    if (OnCancel)
      OnCancel();
  };
}

void InfoDialog::Layer(std::vector<Widget*>& Out) {
  Out.push_back(&Scrim);
  Out.push_back(&Panel);
  Out.push_back(&Close);
}

void InfoDialog::Draw(const Frame& Fr) {
  const Frame F = Fr.Fixed();
  const float K = Alpha.Value(F.Now);
  if (K <= 0.001f && !Opened)
    return;
  const float S = F.S();
  const float W = 760;
  const float TitleH = F.Dc.F.LineHeightPx(cstyles::ModalTitle, S) / S, BodyH = F.Dc.F.LineHeightPx(styles::Body, S) / S;
  const float RowH = BodyH + 6;
  const float H = 28 + TitleH + 14 + float(Rows.size()) * RowH + 20 + 48 + 28;
  Dialog = Rect {(ScreenW - W) / 2, std::round((ScreenH - H) / 2), W, H};
  Panel.Bounds = Dialog;
  Close.Layout(F);
  Close.Bounds.W = std::max(Close.Bounds.W, 120.0f);
  Close.FixedWidth = Close.Bounds.W;
  Close.Bounds.X = Dialog.X + W - 28 - Close.Bounds.W;
  Close.Bounds.Y = Dialog.Y + H - 28 - 48;
  Close.Fade = K;
  DrawScrim(F, Dialog, K);
  F.Text(cstyles::ModalTitle, Title, Dialog.X + 28, Dialog.Y + 28, TitleH, Opacity(F.T().Text, K));
  float Y = Dialog.Y + 28 + TitleH + 14;
  for (const auto& [Keys, What] : Rows) {
    F.Text(styles::MonoUi, F.Dc.F.Fit(styles::MonoUi, Keys, S, 250 * S, Ellipsis::End), Dialog.X + 28, Y, RowH, Opacity(F.T().Text, K));
    F.Text(styles::Body, What, Dialog.X + 28 + 270, Y, RowH, Opacity(F.T().TextMuted, K));
    Y += RowH;
  }
  Close.Draw(F);
}

} // namespace ui
