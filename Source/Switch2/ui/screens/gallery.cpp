// SPDX-License-Identifier: MIT
#include "screens/gallery.h"
#include "components/Chrome.h"

#include <cstdarg>
#include <cstdio>

namespace ui {

namespace {
constexpr float ViewTop = chrome::HeaderH, ViewH = chrome::BodyH, ContentBottom = 1806;
const char* const AccentNames[] = {"Cyan", "Red", "Violet", "Blue"};

std::string Fmt(const char* Format, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* Format, ...) {
  char Buf[256];
  va_list Args;
  va_start(Args, Format);
  std::vsnprintf(Buf, sizeof(Buf), Format, Args);
  va_end(Args);
  return Buf;
}

struct Label {
  std::string Text;
  float X, Y;
  bool Section;
};
} // namespace

struct Gallery::Parts {
  std::vector<std::unique_ptr<Widget>> Owned;
  std::vector<Widget*> Scrolled;       // drawn inside the scroll viewport
  std::vector<Widget*> StaticFocus;    // shown with the ring drawn (the focused state)
  std::vector<Widget*> Registered;     // the base layer
  std::vector<Label> Labels;
  HintBar Hints;
  EnumChip* AccentChip = nullptr;
  EnumChip* ThemeChip = nullptr;
  VersionBlock* Version = nullptr;
  PickerModal Picker;
  ConfirmDialog Confirm;
  ProgressBar* Bar = nullptr;
  ProgressBar* Bar2 = nullptr;
  LogView* Log = nullptr;
  Toggle* FirstToggle = nullptr;
  Slider* FirstSlider = nullptr;
  EnumChip* FirstEnum = nullptr;
  LaunchCard* FirstCard = nullptr;
  IconButton* FirstGear = nullptr;
  PillButton* FirstPill = nullptr;
  PillButton* LogPill = nullptr;
  std::string LogInfo;
  ThemeMode Mode = ThemeMode::Dark;
  AccentPreset Accent = AccentPreset::Cyan;

  template <class T> T* Make(bool InScroll = true) {
    auto W = std::make_unique<T>();
    T* Raw = W.get();
    Owned.push_back(std::move(W));
    if (InScroll) {
      Raw->Fixed = false;
      Scrolled.push_back(Raw);
    } else {
      Raw->Fixed = true;
    }
    Registered.push_back(Raw);
    return Raw;
  }
};

Gallery::Gallery() : Pt(std::make_unique<Parts>()) {
  Pt->Mode = CurrentTheme().Mode;
  Pt->Accent = CurrentTheme().Preset;
  Build();
}
Gallery::~Gallery() = default;

void Gallery::Build() {
  Parts& P = *Pt;
  auto Section = [&](const char* Text, float Y) { P.Labels.push_back({Text, 48, Y, true}); };
  auto Cap = [&](const std::string& Text, float X, float Y) { P.Labels.push_back({Text, X, Y, false}); };
  auto Hold = [&](Widget* W) {  // a state demo: drawn but not part of navigation
    P.Registered.pop_back();
    W->Focusable = false;
    W->Hittable = false;
  };

  // Header (fixed): the theme and accent choosers.
  P.AccentChip = P.Make<EnumChip>(false);
  P.ThemeChip = P.Make<EnumChip>(false);
  P.AccentChip->Values = {"Cyan", "Red", "Violet", "Blue"};
  P.ThemeChip->Values = {"Dark", "Light"};
  P.AccentChip->OnChange = [&P](int I) {
    P.Accent = AccentPreset(I);
    SetTheme(P.Mode, P.Accent);
  };
  P.ThemeChip->OnChange = [&P](int I) {
    P.Mode = I == 0 ? ThemeMode::Dark : ThemeMode::Light;
    SetTheme(P.Mode, P.Accent);
  };
  P.AccentChip->OnActivate = [this]() { PickerFor = Pt->AccentChip; OpenPicker(Now_); };
  P.ThemeChip->OnActivate = [this]() { PickerFor = Pt->ThemeChip; OpenPicker(Now_); };

  // Section 1: icon buttons and the version block.
  Section("Icon button · version block", 84);
  const char* const States[] = {"normal", "focused", "pressed", "disabled"};
  P.FirstGear = P.Make<IconButton>();
  for (int I = 0; I < 4; ++I) {
    IconButton* B = I == 0 ? P.FirstGear : P.Make<IconButton>();
    B->Bounds.X = 58 + 90 * float(I);
    B->Bounds.Y = 124;
    B->OnActivate = [this]() { Last = "gear activated"; };
    if (I == 1) { B->Focused = true; P.StaticFocus.push_back(B); Hold(B); }
    if (I == 2) { B->Pressed = true; Hold(B); }
    if (I == 3) B->Enabled = false;
    Cap(States[I], B->Bounds.X + 28, 190);
  }
  P.Version = P.Make<VersionBlock>();
  P.Version->Bounds.X = 470;
  P.Version->Bounds.Y = 130;
  P.Version->Version = "v0 · FEX-demo";
  Hold(P.Version);

  // Section 2: launch cards.
  Section("Launch card · 280x300", 218);
  for (int I = 0; I < 4; ++I) {
    LaunchCard* C = P.Make<LaunchCard>();
    if (I == 0) P.FirstCard = C;
    C->Bounds.X = 48 + 304 * float(I);
    C->Bounds.Y = 254;
    C->Icon = I == 0 ? IconId::Gamepad : I == 1 ? IconId::Monitor : I == 2 ? IconId::TerminalSquare : IconId::Package;
    C->Label = I == 0 ? "Launch Steam" : I == 1 ? "Launch Desktop" : I == 2 ? "Launch Shell" : "Launch Store";
    C->Subtitle = I == 3 ? "Not available yet" : I == 0 ? "Big Picture, controller-ready" : I == 1 ? "Your configured desktop session" : "Bash inside the x86 RootFS";
    C->OnActivate = [this, I]() { Last = Fmt("card %d launched", I + 1); };
    if (I == 1) { C->Focused = true; P.StaticFocus.push_back(C); Hold(C); }
    if (I == 2) { C->Pressed = true; Hold(C); }
    if (I == 3) C->Enabled = false;
    Cap(States[I], C->Bounds.X + 140, 564);
  }
  // Up from a card goes to the gear (override); gear Down goes back to the last focused card (group).
  P.FirstGear->Over[int(Dir::Down)].Group = 1;
  for (Widget* W : P.Scrolled)
    if (auto* C = dynamic_cast<LaunchCard*>(W))
      C->Group = 1;

  // Section 3: pill buttons.
  Section("Pill button · kinds and states", 600);
  struct PillSpec { const char* Text; PillButton::Kind K; int State; };
  const PillSpec Pills[] = {{"Normal", PillButton::Kind::Normal, 0}, {"Focused", PillButton::Kind::Normal, 1}, {"Pressed", PillButton::Kind::Normal, 2},
                            {"Disabled", PillButton::Kind::Normal, 3}, {"Cancel", PillButton::Kind::Secondary, 0}, {"Confirm", PillButton::Kind::Accent, 0},
                            {"Delete", PillButton::Kind::Error, 0}};
  float Px = 58;
  for (const PillSpec& S : Pills) {
    PillButton* B = P.Make<PillButton>();
    if (!P.FirstPill) P.FirstPill = B;
    B->Text = S.Text;
    B->K = S.K;
    B->Bounds.X = Px;
    B->Bounds.Y = 640;
    B->Bounds.W = 2 * 18 + 90;
    Px += 2 * 18 + 90 + 24;
    B->FixedWidth = 126;
    B->Bounds.W = 126;
    B->OnActivate = [this, T = std::string(S.Text)]() { Last = "pill " + T; };
    if (S.State == 1) { B->Focused = true; P.StaticFocus.push_back(B); Hold(B); }
    if (S.State == 2) { B->Pressed = true; Hold(B); }
    if (S.State == 3) B->Enabled = false;
  }
  Cap("normal · focused · pressed · disabled · secondary · accent · error", 58, 696);

  // Section 4: settings rows with their controls.
  Section("Settings row · toggle · enum chip · path field · slider", 730);
  const char* const RowStates[] = {"normal", "normal", "normal", "normal", "disabled", "pressed", "focused"};
  for (int I = 0; I < 7; ++I) {
    SettingsRow* Row = P.Make<SettingsRow>();
    Row->Bounds = Rect {48, 762 + 68 * float(I), 1184, 68};
    switch (I) {
    case 0: {
      Row->Label = "Toggle";
      Row->Description = "A or a tap flips it; the knob slides for 120 ms";
      auto T = std::make_unique<Toggle>();
      if (I == 0) P.FirstToggle = T.get();
      T->OnChange = [this](bool On) { Last = On ? "toggle on" : "toggle off"; };
      Row->SetControl(std::move(T));
      break;
    }
    case 1: case 6: {
      Row->Label = "Enum chip";
      Row->Description = "Left and Right cycle the value; A opens the picker";
      auto C = std::make_unique<EnumChip>();
      C->Values = {"Auto", "Low", "Medium", "High"};
      C->Index = 2;
      EnumChip* Raw = C.get();
      if (I == 1) P.FirstEnum = Raw;
      C->OnChange = [this](int V) { Last = Fmt("enum -> %d", V); };
      C->OnActivate = [this, Raw]() { PickerFor = Raw; OpenPicker(Now_); };
      Row->SetControl(std::move(C));
      break;
    }
    case 2: {
      Row->Label = "Path field";
      Row->Description = "Chip with a middle ellipsis and a Browse pill";
      auto C = std::make_unique<PathField>();
      C->Path = "/home/limo/.local/share/FEX-Emu/RootFS/Ubuntu_24_04_amd64_long_name.sqsh";
      C->OnActivate = [this]() { Last = "path field: file picker opens here (Task 4)"; };
      Row->SetControl(std::move(C));
      break;
    }
    case 3: {
      Row->Label = "Slider";
      Row->Description = "Left and Right step; hold to repeat; drag with a finger or the mouse";
      auto C = std::make_unique<Slider>();
      C->Min = 0; C->Max = 100; C->Step = 5; C->SetValue(50);
      C->Format = [](float V) { return Fmt("%g%%", double(V)); };
      C->OnChange = [this](float V) { Last = Fmt("slider %g", double(V)); };
      P.FirstSlider = C.get();
      Row->SetControl(std::move(C));
      break;
    }
    default: {
      Row->Label = I == 4 ? "Disabled row" : I == 5 ? "Pressed row" : "Focused row";
      Row->Description = I == 4 ? "Not available yet" : "State demo";
      auto C = std::make_unique<Toggle>();
      Row->SetControl(std::move(C));
    }
    }
    if (I == 4) Row->Enabled = false;
    if (I == 5) { Row->Pressed = true; Hold(Row); }
    if (I == 6) { Row->Focused = true; P.StaticFocus.push_back(Row); Hold(Row); }
    (void)RowStates;
  }

  // Section 5: progress bar and log view.
  Section("Progress bar · log view", 1264);
  P.Bar = P.Make<ProgressBar>();
  P.Bar->Bounds = Rect {48, 1300, 1184, 12};
  P.Bar->Focusable = false;
  P.Registered.pop_back();
  P.Bar2 = P.Make<ProgressBar>();
  P.Bar2->Bounds = Rect {48, 1324, 1184, 12};
  P.Registered.pop_back();
  P.Bar2->Enabled = false;
  const char* const BarPills[] = {"Advance 15%", "Try backwards (ignored)", "Reset", "Add 500 lines"};
  float Bx = 58;
  for (int I = 0; I < 4; ++I) {
    PillButton* B = P.Make<PillButton>();
    B->Text = BarPills[I];
    B->Bounds.X = Bx;
    B->Bounds.Y = 1352;
    B->FixedWidth = I == 1 ? 230 : 160;
    Bx += B->FixedWidth + 16;
    if (I == 3) P.LogPill = B;
    B->OnActivate = [this, I]() {
      Parts& Q = *Pt;
      if (I == 0) { Q.Bar->SetValue(Q.Bar->Target() + 0.15f, Now_); Last = Fmt("progress target %.2f", double(Q.Bar->Target())); }
      if (I == 1) { Q.Bar->SetValue(Q.Bar->Target() - 0.2f, Now_); Last = Fmt("progress stays %.2f (never backwards)", double(Q.Bar->Target())); }
      if (I == 2) { Q.Bar->Reset(Now_); Last = "progress reset"; }
      if (I == 3) { for (int K = 0; K < 500; ++K) Q.Log->Append(Fmt("bulk line %d", K), K % 50 == 0 ? LogView::Kind::Warn : LogView::Kind::Normal); Last = "added 500 log lines"; }
    };
  }
  P.Bar->SetValue(0.35f, 0);
  P.Log = P.Make<LogView>();
  P.Log->Bounds = Rect {48, 1412, 1184, 230};
  P.Registered.pop_back();
  for (int I = 0; I < 6; ++I)
    P.Log->Append(Fmt("[%02d:00:%02d] demo line %d (the ring buffer keeps %zu lines)", 12, I, I, LogView::MaxLines), I == 2 ? LogView::Kind::Ok : I == 4 ? LogView::Kind::Dim : LogView::Kind::Normal);

  // Section 6: modals.
  Section("Modals · picker, confirm, destructive confirm", 1694);
  const char* const ModalPills[] = {"Open picker", "Open confirm", "Open destructive confirm"};
  float Mx = 58;
  for (int I = 0; I < 3; ++I) {
    PillButton* B = P.Make<PillButton>();
    B->Text = ModalPills[I];
    B->Bounds.X = Mx;
    B->Bounds.Y = 1722;
    B->FixedWidth = I == 2 ? 260 : 170;
    Mx += B->FixedWidth + 16;
    B->OnActivate = [this, I]() {
      if (I == 0) { PickerFor = nullptr; OpenPicker(Now_); }
      else OpenConfirm(I == 2, Now_);
    };
  }

  // HintBar.
  P.Hints.SetHints({{Button::A, "Activate"}, {Button::B, "Back"}, {Button::ZL, "Page up"}, {Button::ZR, "Page down"}});
  P.Hints.OnHint = [this](Button B) {
    UiEvent E;
    E.Kind = UiEvent::Type::Button;
    E.Btn = B;
    Interact.Dispatch(E, Now_);
  };

  Scroll.SetViewport(ViewTop, ViewH, ContentBottom - ViewTop);
  Interact.Scroll = &Scroll;
  std::vector<Widget*> Root = P.Registered;
  Root.push_back(&P.Hints);
  Interact.SetRoot(Root);
  // Ids exist now: Up from any card goes to the gear (explicit override).
  for (Widget* W : P.Scrolled)
    if (auto* C = dynamic_cast<LaunchCard*>(W))
      C->Over[int(Dir::Up)].Id = P.FirstGear->Id;
  Interact.SetFocus(P.FirstGear->Id, 0);

  Interact.OnBack = [this]() { Last = "B on the base page (Home would exit)"; };
  Interact.OnButton = [this](Button B) {
    if (B == Button::ZL || B == Button::ZR) {
      const float Page = ViewH - 48;
      Scroll.To(Scroll.Target() + (B == Button::ZR ? Page : -Page), Now_, 200);
      Last = B == Button::ZR ? "page down" : "page up";
    } else {
      Last = std::string("button ") + ButtonName(B);
    }
  };
  Interact.ScrollFilter = [this](float X, float Y, float Dy) {
    const Rect L = Pt->Log->Bounds;
    const float Off = Scroll.Offset(Now_);
    if (X >= L.X && X < L.X + L.W && Y >= L.Y - Off && Y < L.Y - Off + L.H && Y >= ViewTop && Y < ViewTop + ViewH) {
      Pt->Log->ScrollBy(Dy);
      return true;
    }
    return false;
  };
}

PillButton* Gallery::FirstPill() { return Pt->FirstPill; }
Slider* Gallery::FirstSlider() { return Pt->FirstSlider; }
EnumChip* Gallery::FirstEnum() { return Pt->FirstEnum; }
SettingsRow* Gallery::RowFor(Control* C) {
  for (Widget* W : Pt->Scrolled)
    if (auto* R = dynamic_cast<SettingsRow*>(W))
      if (R->GetControl() == C)
        return R;
  return nullptr;
}

void Gallery::OpenFor(ModalBase& M, uint64_t Now) {
  if (Active)
    return;
  Active = &M;
  std::vector<Widget*> L;
  M.Layer(L);
  M.OnCancel = [this]() { CloseModal(Now_); };
  Interact.PushLayer(L, M.InitialFocus(), [this]() { CloseModal(Now_); });
  (void)Now;
}

void Gallery::OpenPicker(uint64_t Now) {
  Parts& P = *Pt;
  if (Active)
    return;
  EnumChip* Chip = PickerFor;
  if (Chip) {
    P.Picker.Title = Chip == P.AccentChip ? "Accent color" : Chip == P.ThemeChip ? "Theme" : "Choose a value";
    P.Picker.Options = Chip->Values;
    P.Picker.Selected = Chip->Index;
  } else {
    P.Picker.Title = "PickerModal";
    P.Picker.Options = {"Option one", "Option two", "Option three"};
    P.Picker.Selected = 1;
  }
  P.Picker.OnSelect = [this, Chip](int I) {
    if (Chip) {
      Chip->Index = I;
      if (Chip->OnChange)
        Chip->OnChange(I);
    }
    Last = Fmt("picker selected %d", I);
    CloseModal(Now_);
  };
  P.Picker.Open(Now);
  OpenFor(P.Picker, Now);
}

void Gallery::OpenConfirm(bool Destructive, uint64_t Now) {
  Parts& P = *Pt;
  if (Active)
    return;
  P.Confirm.Title = Destructive ? "Delete the RootFS?" : "Restore all settings?";
  P.Confirm.Message = Destructive ? "This removes the installed Ubuntu 24.04 image from the SD card. This cannot be undone." : "Every setting returns to its default. Your RootFS stays installed.";
  P.Confirm.ConfirmText = Destructive ? "Delete" : "Restore";
  P.Confirm.Destructive = Destructive;
  P.Confirm.OnConfirm = [this, Destructive]() {
    Last = Destructive ? "confirmed: destructive" : "confirmed";
    CloseModal(Now_);
  };
  P.Confirm.Open(Now);
  OpenFor(P.Confirm, Now);
}

void Gallery::CloseModal(uint64_t Now) {
  if (!Active)
    return;
  Active->Hide(Now);
  Active = nullptr;
  Interact.PopLayer();
}

void Gallery::Update(uint64_t Now, float Scale, const std::vector<Event>& Raw) {
  Now_ = Now;
  Parts& P = *Pt;
  Interact.Update(Now, Scale, Raw);
  for (auto& W : P.Owned)
    W->Tick(Now);
  // Demo log: one line every 700 ms while the page is open.
  if (LastLogAt == 0)
    LastLogAt = Now;
  while (Now - LastLogAt >= 700) {
    LastLogAt += 700;
    ++LogCount;
    P.Log->Append(Fmt("[12:01:%02d] demo log line %d", LogCount % 60, LogCount), LogCount % 7 == 0 ? LogView::Kind::Warn : LogCount % 5 == 0 ? LogView::Kind::Ok : LogView::Kind::Normal);
  }
  P.LogInfo = Fmt("%zu lines kept, %llu dropped", P.Log->LineCount(), (unsigned long long)P.Log->Dropped());
  SetStatus(Now);
}

void Gallery::SetStatus(uint64_t Now) {
  (void)Now;
  Parts& P = *Pt;
  P.Hints.Status = Fmt("%s · focus %d · nav %llu · activate %llu · %s%s%s", ModeName(Interact.GetMode()), Interact.FocusedId(), (unsigned long long)Interact.Count("nav"),
                       (unsigned long long)Interact.Count("activate"), Last.c_str(), ExtraStatus.empty() ? "" : " · ", ExtraStatus.c_str());
}

void Gallery::Draw(const DrawContext& C, uint64_t Now) {
  Now_ = Now;
  Parts& P = *Pt;
  const Theme& T = C.T;
  const float S = C.S;
  const Frame F {C, Now, 0};
  const Frame Fc = Frame {C, Now, Interact.ScrollOffset(Now)};

  // Scrolled content, clipped to the viewport.
  C.R.PushClip(C.Du(0, ViewTop, 1280, ViewH));
  for (const Label& L : P.Labels) {
    if (L.Section)
      Fc.Text(styles::Section, L.Text, L.X, L.Y, 20, T.TextMuted);
    else
      Fc.Text(styles::Meta, L.Text, L.X, L.Y, 16, T.TextMuted, L.X == 58 ? Align::Left : Align::Center);
  }
  for (Widget* W : P.Scrolled) {
    if (W == P.Log) {
      P.Log->Draw(Fc);
      Fc.Text(styles::Meta, P.LogInfo, 48, 1648, 16, T.TextMuted);
    } else {
      W->Draw(Fc);
    }
  }
  for (Widget* W : P.StaticFocus)
    if (!W->Fixed)
      FocusRing::DrawNow(Fc, *W, 1.0f);
  C.R.PopClip();

  // Header and HintBar over the content.
  C.R.FillRect(Rect {0, std::round(656 * S), float(C.R.TargetSize().W), float(C.R.TargetSize().H) - std::round(656 * S)}, T.Bg);
  DrawScreenHeader(F, IconId::Info, "Component gallery", {}, false, 1.0f, false);
  P.ThemeChip->Bounds.W = P.ThemeChip->PreferredWidth(F);
  P.ThemeChip->Bounds.X = 1232 - P.ThemeChip->Bounds.W;
  P.ThemeChip->Bounds.Y = 22;
  P.AccentChip->Bounds.W = P.AccentChip->PreferredWidth(F);
  P.AccentChip->Bounds.X = P.ThemeChip->Bounds.X - 14 - P.AccentChip->Bounds.W;
  P.AccentChip->Bounds.Y = 22;
  P.ThemeChip->Index = P.Mode == ThemeMode::Dark ? 0 : 1;
  P.AccentChip->Index = int(P.Accent);
  P.ThemeChip->Draw(F);
  P.AccentChip->Draw(F);
  P.Hints.Draw(F);

  if (Active && Active->Visible(Now))
    Active->Draw(F);
  else if (!Active && (P.Picker.Visible(Now) || P.Confirm.Visible(Now))) {
    if (P.Picker.Visible(Now))
      P.Picker.Draw(F);
    if (P.Confirm.Visible(Now))
      P.Confirm.Draw(F);
  }
  Interact.DrawOverlay(F);
}

} // namespace ui
