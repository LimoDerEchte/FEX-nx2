// SPDX-License-Identifier: MIT
#include "screens/rootfs_setup.h"
#include "components/Chrome.h"

#include <algorithm>
#include <cmath>

#include "PathUtil.h"

namespace ui {

using namespace setup_layout;

namespace {
const char* const SetupSections[4] = {"Base system", "Desktop environments", "Game stores", "Install options"};
const char* const SetupNotes[4] = {"Pick one", "Pick one \xC2\xB7 downloaded the first time you launch it", "Installed together with the RootFS", "Optional"};
constexpr int SectionCols[4] = {2, 5, 2, 2};
constexpr const char* NotYet = "Not available yet";
constexpr TextStyle Banner15 {"setup_banner", FontId::NunitoSemiBold, 15};
constexpr TextStyle Free15 {"setup_free", FontId::NunitoBold, 15};
constexpr TextStyle FreeNote {"setup_free_note", FontId::NunitoRegular, 15};
} // namespace

RootfsSetupScreen::RootfsSetupScreen(LauncherBackend& B, bool R, SettingsPaths P) : Be(B), Reinstall(R), Paths(std::move(P)) {
  Sel = RootfsSelection::Load(Be.GetStorage(), Paths, Be.RootfsImages());
  Be.GetStorage().FreeSpace("sdmc:/", Free, Total);
  Build();

  Bar.SetHints({{Button::B, Reinstall ? "Cancel" : "Exit"}, {Button::A, "Select"}, {Button::Plus, Reinstall ? "Reinstall" : "Install"}});
  Bar.OnHint = [this](Button Btn) {
    UiEvent E;
    E.Kind = UiEvent::Type::Button;
    E.Btn = Btn;
    Interact.Dispatch(E, Now_);
  };

  ConfirmM.Title = "Reinstall RootFS?";
  ConfirmM.ConfirmText = "Reinstall";
  ConfirmM.Destructive = false;
  ConfirmM.OnConfirm = [this]() {
    Host.Close();
    StartInstall();
  };

  std::vector<Widget*> L;
  for (auto& C : Cards)
    L.push_back(C.Card.get());
  L.push_back(&Bar);
  Interact.SetRoot(L);
  Interact.Scroll = &Scroll;
  Interact.OnBack = [this]() { Leave(); };
  Interact.OnButton = [this](Button Btn) {
    if (Btn == Button::Plus && !Host.IsOpen())
      PressInstall();
  };
  Layout();
  Refresh();
  // The first focus is the chosen base card.
  if (CardInfo* C = CardByKey(Sel.Base))
    Interact.SetFocus(C->Card->Id, 0);
}

RootfsSetupScreen::~RootfsSetupScreen() = default;

RootfsSetupScreen::CardInfo* RootfsSetupScreen::CardByKey(const std::string& Key) {
  for (CardInfo& C : Cards)
    if (C.Key == Key)
      return &C;
  return nullptr;
}

void RootfsSetupScreen::Build() {
  auto Add = [&](int Section, const std::string& Key, const std::string& Name, const std::string& Meta, IconId Icon, bool Real, bool Radio) {
    CardInfo C;
    C.Section = Section;
    C.Key = Key;
    C.Real = Real;
    C.Card = std::make_unique<OptionCard>(Section == 1, Radio ? OptionCard::Badge::Radio : OptionCard::Badge::Check);
    C.Card->Name = Name;
    C.Card->Meta = Real ? Meta : (Meta.empty() ? NotYet : Meta);
    C.Card->Icon = Icon;
    C.Card->Enabled = Real;
    Cards.push_back(std::move(C));
    CardInfo& Ref = Cards.back();
    Ref.Card->OnActivate = [this, &Ref]() { Select(Ref); };
  };
  Cards.reserve(16);
  // Base system: the index entries (Ubuntu 24.04) and the 26.04 the project has no image for yet.
  bool First = true;
  for (const RootfsImage& I : Be.RootfsImages().Images) {
    const bool Real = I.Selectable();
    std::string Meta = Real ? "Download " + FormatBytes(I.DownloadBytes) + " \xC2\xB7 installed about " + FormatBytes(I.InstalledBytes) : I.Reason;
    Add(0, RootfsSelection::KeyOf(I), I.Name, Meta, IconId::Package, Real, true);
    Cards.back().Image = &I;
    if (Real && First)
      Cards.back().Card->Tag = "Recommended";
    if (Real)
      First = false;
  }
  Add(0, "ubuntu-26.04", "Ubuntu 26.04", "", IconId::Package, false, true);
  Add(1, "none", "None", "Command line only", IconId::CircleSlash, true, true);
  Add(1, "kde", "KDE Plasma", "", IconId::Monitor, false, true);
  Add(1, "xfce", "Xfce", "", IconId::Monitor, false, true);
  Add(1, "lxqt", "LXQt", "", IconId::Monitor, false, true);
  Add(1, "gnome", "GNOME", "", IconId::Monitor, false, true);
  Add(2, "steam", "Steam", "", IconId::ShoppingBag, false, false);
  Add(2, "epic", "Epic Games", "", IconId::ShoppingBag, false, false);
  const RootfsImage* Img = nullptr;
  for (const RootfsImage& I : Be.RootfsImages().Images)
    if (I.Selectable()) {
      Img = &I;
      break;
    }
  std::string ExtrasMeta = "Test tools for the guest";
  if (Img && !Img->Extras.empty()) {
    ExtrasMeta.clear();
    for (const std::string& E : Img->Extras)
      ExtrasMeta += (ExtrasMeta.empty() ? "" : ", ") + E;
    ExtrasMeta += " \xC2\xB7 about " + FormatBytes(Img->ExtraDownloadBytes);
  }
  Add(3, "extras", "Extra packages", ExtrasMeta, IconId::Package, true, false);
  Add(3, "local-image", "Image from the SD card", "Choose a .sqsh file instead of downloading", IconId::HardDrive, true, false);
}

void RootfsSetupScreen::Layout() {
  float Y = ViewTop + Pad;
  BannerY = Y;
  if (Reinstall)
    Y += BannerH + SectionGap;
  for (int S = 0; S < 4; ++S) {
    SectionY[S] = Y;
    Y += 20 + 12;
    const int Cols = SectionCols[S];
    const float W = (ContentW - float(Cols - 1) * 24) / float(Cols);
    int N = 0;
    float H = 0;
    for (CardInfo& C : Cards)
      if (C.Section == S) {
        const int Col = N % Cols, Row = N / Cols;
        H = C.Card->Bounds.H;
        C.Card->Bounds.X = SideX + float(Col) * (W + 24);
        C.Card->Bounds.Y = Y + float(Row) * (H + 24);
        C.Card->Bounds.W = W;
        ++N;
      }
    const int Rows = (N + Cols - 1) / Cols;
    Y += float(Rows) * H + float(std::max(0, Rows - 1)) * 24 + SectionGap;
  }
  ContentEnd = Y - SectionGap + 24;
  Scroll.SetViewport(ViewTop, ViewH, ContentEnd - ViewTop);
}

const RootfsImage* RootfsSetupScreen::ChosenImage() const {
  for (const CardInfo& C : Cards)
    if (C.Section == 0 && C.Real && C.Image && C.Key == Sel.Base)
      return C.Image;
  return nullptr;
}

uint64_t RootfsSetupScreen::NeedBytes() const {
  const RootfsImage* I = ChosenImage();
  return I ? InstallSpaceNeeded(*I, !Local.empty(), Sel.Extras) : 0;
}
uint64_t RootfsSetupScreen::DownloadBytes() const {
  const RootfsImage* I = ChosenImage();
  return I ? InstallDownloadBytes(*I, !Local.empty(), Sel.Extras) : 0;
}

std::string RootfsSetupScreen::FooterText() const {
  if (!EnoughSpace())
    return "Not enough space \xC2\xB7 needs about " + FormatBytes(NeedBytes()) + ", " + FormatBytes(Free) + " free";
  return "Download now \xE2\x89\x88 " + FormatBytes(DownloadBytes()) + " \xC2\xB7 desktop on first launch \xE2\x89\x88 none";
}

void RootfsSetupScreen::Refresh() {
  for (CardInfo& C : Cards) {
    if (C.Section == 0 && C.Real)
      C.Card->Selected = C.Key == Sel.Base;
    else if (C.Section == 1)
      C.Card->Selected = C.Real && C.Key == Sel.Desktop;
    else if (C.Key == "extras")
      C.Card->Selected = Sel.Extras;
    else if (C.Key == "local-image") {
      C.Card->Selected = !Local.empty();
      C.Card->Meta = Local.empty() ? "Choose a .sqsh file instead of downloading" : Local;
    }
  }
  Bar.Segs.clear();
  HintBar::Seg G;
  G.K = EnoughSpace() ? HintBar::Seg::Kind::Body : HintBar::Seg::Kind::Warn;
  G.Text = FooterText();
  Bar.Segs.push_back(G);
  Bar.SetHintEnabled(Button::Plus, EnoughSpace() && ChosenImage() != nullptr);
}

void RootfsSetupScreen::SaveSelection() {
  std::string Err;
  if (!Sel.Save(Be.GetStorage(), Paths, Err))
    Bar.Segs.assign(1, HintBar::Seg {HintBar::Seg::Kind::Warn, "Could not save the choices: " + Err});
}

void RootfsSetupScreen::Select(CardInfo& C) {
  if (!C.Real || Host.IsOpen())
    return;
  if (C.Section == 0) {
    Sel.Base = C.Key;
  } else if (C.Section == 1) {
    Sel.Desktop = C.Key;
  } else if (C.Key == "extras") {
    Sel.Extras = !Sel.Extras;
  } else if (C.Key == "local-image") {
    if (!Local.empty()) {
      Local.clear();
    } else {
      Request Rq;
      Rq.K = Request::Kind::OpenFilePicker;
      Rq.Picker.Title = "Choose the RootFS image";
      Rq.Picker.Accept = {".sqsh"};
      Rq.Picker.Current = "sdmc:/fex/images/";
      Rq.Picker.FallbackLocation = "fex";
      Rq.OnPicked = [this](const std::string& Path) {
        if (IsSdPath(Path)) {
          Local = "/" + Path.substr(6);  // the installer reads SD files as "/fex/images/x.sqsh"
          Refresh();
        }
      };
      if (OnRequest)
        OnRequest(Rq);
    }
  }
  SaveSelection();
  Refresh();
}

InstallRequest RootfsSetupScreen::BuildRequest() const {
  InstallRequest R;
  if (const RootfsImage* I = ChosenImage())
    R.Image = *I;
  R.LocalPath = Local;
  R.Reinstall = Reinstall || Be.Rootfs().Installed;
  R.Extras = Sel.Extras;
  return R;
}

void RootfsSetupScreen::StartInstall() {
  Request Rq;
  Rq.K = Request::Kind::StartInstall;
  Rq.Install = BuildRequest();
  if (OnRequest)
    OnRequest(Rq);
}

void RootfsSetupScreen::PressInstall() {
  if (!EnoughSpace() || !ChosenImage() || Host.IsOpen())
    return;
  if (!Reinstall) {
    StartInstall();
    return;
  }
  const RootfsImage* I = ChosenImage();
  ConfirmM.Message = "This replaces the installed RootFS, including everything in your home folder. The old RootFS stays until the new image is downloaded and checked. It cannot be undone.";
  ConfirmM.Summary.clear();
  ConfirmM.Summary.push_back("Base      " + (I ? I->Name : std::string("-")));
  ConfirmM.Summary.push_back("Desktop   none");
  ConfirmM.Summary.push_back("Stores    none");
  ConfirmM.Summary.push_back("Download  " + FormatBytes(DownloadBytes()));
  ConfirmM.Open(Now_);
  Host.Open(ConfirmM);
}

void RootfsSetupScreen::Leave() { Send(Reinstall ? Request::Kind::Back : Request::Kind::Exit); }

void RootfsSetupScreen::Update(uint64_t Now, float Scale, const std::vector<Event>& Raw) {
  Now_ = Now;
  Host.Tick(Now);
  Interact.Update(Now, Scale, Raw);
}

void RootfsSetupScreen::Draw(const DrawContext& C, uint64_t Now) {
  Now_ = Now;
  const Theme& T = C.T;
  const float S = C.S;
  const Frame F {C, Now, 0};
  const Frame Fc {C, Now, Interact.ScrollOffset(Now)};

  C.R.PushClip(C.Du(0, ViewTop, 1280, ViewH));
  if (Reinstall) {
    const Rect B = Fc.Box(SideX, BannerY, ContentW, BannerH);
    C.R.FillRoundRect(B, 12 * S, Opacity(T.Warn, 0.10f));
    C.R.StrokeRoundRect(B, 12 * S, std::max(1.0f, std::round(2 * S)), T.Warn);
    const std::string Name = Be.Rootfs().Name.empty() ? "the installed RootFS" : Be.Rootfs().Name;
    Fc.Text(Banner15, "This replaces your current RootFS (" + Name + "). Everything in it, including your home folder, is deleted.", SideX + 20, BannerY, BannerH, T.Text);
  }
  for (int S2 = 0; S2 < 4; ++S2)
    DrawSectionHeader(Fc, SideX, SectionY[S2], ContentW, SetupSections[S2], SetupNotes[S2]);
  for (CardInfo& Ci : Cards) {
    const float Y = Ci.Card->Bounds.Y - Fc.ScrollY;
    if (Y + Ci.Card->Bounds.H > ViewTop && Y < ViewTop + ViewH)
      Ci.Card->Draw(Fc);
  }
  C.R.PopClip();

  // Header: hard drive icon, title over the subtitle, the free-space pill.
  DrawScreenHeader(F, IconId::HardDrive, "RootFS setup", Reinstall ? "Reinstall \xC2\xB7 replaces the current RootFS" : "First-run setup \xC2\xB7 installs the Linux system");
  const std::string FreeText = FormatBytes(Free) + " free";
  const float W1 = F.Measure(Free15, FreeText), W2 = F.Measure(FreeNote, "on microSD");
  const float PillW = 18 + W1 + 8 + W2 + 18;
  const float PillX = chrome::HeaderRight - PillW, PillY = chrome::HeaderCentre(40);
  C.R.FillRoundRect(C.Du(PillX, PillY, PillW, 40), std::round(20 * S), T.SurfaceRaised);
  F.Text(Free15, FreeText, PillX + 18, PillY, 40, EnoughSpace() ? T.Text : T.Warn);
  F.Text(FreeNote, "on microSD", PillX + 18 + W1 + 8, PillY, 40, T.TextMuted);
  Bar.Draw(F);
  Host.Draw(F, {&ConfirmM});
  Interact.DrawOverlay(F);
}

} // namespace ui
