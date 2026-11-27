// SPDX-License-Identifier: MIT
#include "screens/home.h"

#include "PathUtil.h"

namespace ui {

using namespace home_layout;

HomeScreen::HomeScreen(LauncherBackend& B) : Be(B) {
  Version = std::make_unique<VersionBlock>();
  Version->Fixed = true;
  Version->Bounds.X = HeaderSide;
  Version->Bounds.Y = HeaderTop + (HeaderH - 44) / 2;

  GearBtn = std::make_unique<IconButton>();
  GearBtn->Fixed = true;
  GearBtn->Bounds.X = 1280 - HeaderSide - 56;
  GearBtn->Bounds.Y = HeaderTop;
  GearBtn->OnActivate = [this]() { Send(Request::Kind::OpenSettings); };

  static const struct {
    IconId Icon;
    const char* Label;
    const char* Subtitle;
  } Spec[3] = {{IconId::Gamepad, "Launch Steam", "Big Picture, controller-ready"},
               {IconId::Monitor, "Launch Desktop", "Your configured desktop session"},
               {IconId::TerminalSquare, "Launch Shell", "Bash inside the x86 RootFS"}};
  for (int I = 0; I < 3; ++I) {
    auto C = std::make_unique<LaunchCard>();
    C->Fixed = true;
    C->Bounds.X = CardX[I];
    C->Bounds.Y = CardY;
    C->Icon = Spec[I].Icon;
    C->Label = Spec[I].Label;
    C->Subtitle = Spec[I].Subtitle;
    C->Group = 1;  // "last focused card" for the gear's Down and the pill's Up
    Cards.push_back(std::move(C));
  }
  Cards[0]->OnActivate = [this]() { Send(Request::Kind::LaunchSteam); };
  Cards[1]->OnActivate = [this]() { Send(Request::Kind::LaunchDesktop); };
  Cards[2]->OnActivate = [this]() { Send(Request::Kind::LaunchShell); };

  Pill_ = std::make_unique<FtpPill>();
  Pill_->Fixed = true;
  Pill_->Bounds.Y = CardY + CardH + PillGapBelowCards;
  Pill_->OnActivate = [this]() { Send(Request::Kind::OpenFtp); };

  Bar = std::make_unique<HintBar>();
  Bar->SetHints({{Button::A, "Launch"}, {Button::B, "Exit"}, {Button::X, "FTP"}, {Button::Plus, "Settings"}});
  Bar->OnHint = [this](Button Btn) {
    UiEvent E;
    E.Kind = UiEvent::Type::Button;
    E.Btn = Btn;
    Interact.Dispatch(E, Now_);
  };

  std::vector<Widget*> Root = {Version.get(), GearBtn.get(), Cards[0].get(), Cards[1].get(), Cards[2].get(), Pill_.get(), Bar.get()};
  Interact.SetRoot(Root);
  // Explicit navigation: Up from any card -> gear; gear Down -> the last focused card; Down from any card -> the FTP pill;
  // pill Up -> the last focused card.
  for (auto& C : Cards) {
    C->Over[int(Dir::Up)].Id = GearBtn->Id;
    C->Over[int(Dir::Down)].Id = Pill_->Id;
  }
  GearBtn->Over[int(Dir::Down)].Group = 1;
  Pill_->Over[int(Dir::Up)].Group = 1;
  Interact.OnBack = [this]() { Send(Request::Kind::Exit); };  // no confirmation (console feedback)
  Interact.OnButton = [this](Button Btn) {
    if (Btn == Button::X)
      Send(Request::Kind::OpenFtp);
    else if (Btn == Button::Plus)
      Send(Request::Kind::OpenSettings);
  };
  Refresh();
  // Card 1 is focused on first open; when it is disabled (Steam needs graphics) the first usable card is.
  for (int I = 0; I < 3; ++I)
    if (Cards[size_t(I)]->Enabled) {
      LastCard = I;
      Interact.SetFocus(Cards[size_t(I)]->Id, 0);
      break;
    }
}

HomeScreen::~HomeScreen() = default;

void HomeScreen::Refresh() {
  const Capability Cap[3] = {Be.Steam(), Be.Desktop(), Be.Shell()};
  for (int I = 0; I < 3; ++I) {
    static const char* const Normal[3] = {"Big Picture, controller-ready", "Your configured desktop session", "Bash inside the x86 RootFS"};
    Cards[size_t(I)]->Enabled = Cap[I].Available;
    Cards[size_t(I)]->Subtitle = Cap[I].Available || Cap[I].Reason.empty() ? Normal[I] : Cap[I].Reason;
  }
  // Left/Right move to the nearest enabled card; with none on that side focus stays (never jumps to the FTP pill).
  for (int I = 0; I < 3; ++I) {
    for (int Step : {-1, 1}) {
      int Target = I;
      for (int K = I + Step; K >= 0 && K < 3; K += Step)
        if (Cards[size_t(K)]->Enabled) {
          Target = K;
          break;
        }
      Cards[size_t(I)]->Over[int(Step < 0 ? Dir::Left : Dir::Right)].Id = Cards[size_t(Target)]->Id;
    }
  }
  const FtpInfo Ftp = Be.Ftp();
  Pill_->Running = Ftp.Running;
  Pill_->Clients = Ftp.Clients;
  Version->Version = Be.VersionText();
  const RootfsInfo R = Be.Rootfs();
  Bar->Segs.clear();
  if (R.Installed) {
    Bar->Segs.push_back({HintBar::Seg::Kind::Body, "RootFS"});
    Bar->Segs.push_back({HintBar::Seg::Kind::Mono, R.Name});
    Bar->Segs.push_back({HintBar::Seg::Kind::Body, "\xC2\xB7 " + FormatBytes(R.FreeBytes) + " free"});
  } else {
    Bar->Segs.push_back({HintBar::Seg::Kind::Body, "No RootFS installed \xC2\xB7 " + FormatBytes(R.FreeBytes) + " free"});
  }
}

void HomeScreen::Layout(const Frame& F) {
  Pill_->Bounds.W = Pill_->PreferredWidth(F);
  Pill_->Bounds.X = std::round((1280 - Pill_->Bounds.W) / 2);
}

void HomeScreen::Update(uint64_t Now, float Scale, const std::vector<Event>& Raw) {
  Now_ = Now;
  Host.Tick(Now);
  Interact.Update(Now, Scale, Raw);
  for (int I = 0; I < 3; ++I)
    if (Cards[size_t(I)]->Focused)
      LastCard = I;
  Refresh();
}

void HomeScreen::Draw(const DrawContext& C, uint64_t Now) {
  Now_ = Now;
  const Frame F {C, Now, 0};
  const Theme& T = C.T;
  Layout(F);
  Version->Draw(F);
  GearBtn->Draw(F);
  // Title block: 8 below the header, centred.
  const float TitleTop = HeaderTop + HeaderH + 8;
  const float TitleH = C.F.LineHeightPx(styles::Display, C.S) / C.S;
  F.Text(styles::Display, "FEX for Switch 2", 640, TitleTop, TitleH, T.Text, Align::Center);
  const float SubTop = TitleTop + TitleH + 6;
  F.Text(styles::BodyRegular, "Run x86 and x86-64 Linux software on your console", 640, SubTop, C.F.LineHeightPx(styles::BodyRegular, C.S) / C.S, T.TextMuted, Align::Center);
  TitleBottom = SubTop + C.F.LineHeightPx(styles::BodyRegular, C.S) / C.S;
  for (auto& Card : Cards)
    Card->Draw(F);
  Pill_->Draw(F);
  Bar->Draw(F);
  Interact.DrawOverlay(F);
}

} // namespace ui
