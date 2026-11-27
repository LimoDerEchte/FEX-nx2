// SPDX-License-Identifier: MIT
#include "screens/ftp.h"
#include "components/Chrome.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace ui {

using namespace ftp_layout;

namespace {
constexpr TextStyle AddrStyle {"ftp_address", FontId::MonoSemiBold, 34, -0.5f};
constexpr TextStyle ConnectLabel {"ftp_connect", FontId::NunitoSemiBold, 16};
constexpr TextStyle Empty16 {"ftp_empty", FontId::NunitoRegular, 16};

std::function<uint32_t()> DefaultRand() {
  auto Gen = std::make_shared<std::random_device>();
  return [Gen]() { return uint32_t((*Gen)()); };
}
} // namespace

FtpScreen::FtpScreen(LauncherBackend& B, SettingsPaths P, std::function<uint32_t()> Rand) : Be(B), Paths(std::move(P)) {
  Cfg = FtpSettings::Load(Be.GetStorage(), Paths, Rand ? Rand : DefaultRand());
  Snap = Be.FtpState();

  Header.Fixed = true;
  Header.Bounds = Rect {chrome::HeaderRight - 104, chrome::HeaderCentre(32), 104, 32};
  Header.Set(Snap.Running, 0, false);
  Header.OnChange = [this](bool On) { SetRunning(On); };

  static const char* const Labels[5] = {"Username", "Password", "Port", "Folder", "Keep running in apps"};
  for (int I = 0; I < 5; ++I) {
    auto T = std::make_unique<CredentialTile>(I == Password ? CredentialTile::Kind::Password : I == Background ? CredentialTile::Kind::Switch : CredentialTile::Kind::Text);
    T->Label = Labels[I];
    T->Group = TileGroup;
    T->Bounds = Rect {SideX + float(I) * (TileW + TileGap), TileY, TileW, TileH};
    T->OnActivate = [this, I]() { EditTile(I); };
    Tiles.push_back(std::move(T));
  }
  Tiles[Password]->OnEye = [this]() { ToggleSecret(); };

  Bar.SetHints({{Button::Y, "Start/Stop"}, {Button::B, "Back"}, {Button::A, "Edit"}});
  Bar.OnHint = [this](Button Btn) {
    UiEvent E;
    E.Kind = UiEvent::Type::Button;
    E.Btn = Btn;
    Interact.Dispatch(E, Now_);
  };

  ConfirmM.Title = "Disconnect?";
  ConfirmM.ConfirmText = "Disconnect";
  ConfirmM.Destructive = true;
  ConfirmM.OnConfirm = [this]() {
    Host.Close();
    if (DisconnectId)
      Be.FtpDisconnect(DisconnectId);
    DisconnectId = 0;
  };

  BuildRoot();
  Interact.Scroll = &Scroll;
  Interact.OnBack = [this]() { Send(Request::Kind::Back); };
  Interact.OnButton = [this](Button Btn) {
    if (Btn != Button::Y || Host.IsOpen())
      return;
    Widget* F = Interact.FocusedWidget();
    if (F == Tiles[Password].get())
      ToggleSecret();
    else
      ToggleRunning();
  };
  Interact.SetFocus(Header.Id, 0);
  Layout();
  RefreshTexts();
  if (Cfg.PasswordGenerated)
    Save();  // the first run keeps the password it shows
}

FtpScreen::~FtpScreen() = default;

void FtpScreen::BuildRoot() {
  std::vector<Widget*> L;
  L.push_back(&Header);
  for (auto& T : Tiles)
    L.push_back(T.get());
  for (auto& R : Rows)
    L.push_back(R.get());
  L.push_back(&Bar);
  Interact.SetRoot(L);
  // Up from any tile reaches the header toggle (it lies outside the 45 degree cone); the toggle's Down goes to the tiles and the
  // tiles' Down to the clients (the last focused of each group).
  for (auto& T : Tiles) {
    T->Over[int(Dir::Up)].Id = Header.Id;
    if (!Rows.empty())
      T->Over[int(Dir::Down)].Group = RowGroup;
  }
  Header.Over[int(Dir::Down)].Group = TileGroup;
}

std::string FtpScreen::AddressText() const { return FtpAddressText(Snap, true); }

void FtpScreen::SetStatus(const std::string& S, bool) {
  Message = S;
  MessageUntil = Now_ + 6000;
  Bar.Status = S;
}

void FtpScreen::Save() {
  std::string Err;
  if (!Cfg.Save(Be.GetStorage(), Paths, Err))
    SetStatus("Could not save the FTP settings: " + Err);
}

void FtpScreen::SetRunning(bool On) {
  Cfg.Enabled = On;
  Error.clear();
  if (On) {
    std::string Err;
    if (!Be.FtpStart(Cfg.ToConfig(), Err)) {
      Cfg.Enabled = false;
      Error = "Could not start the FTP server: " + Err;
    }
  } else {
    Be.FtpStop();
  }
  Save();
  Rates_.Reset();
  Snap = Be.FtpState();
  Header.Set(Snap.Running, Now_, true);
  Dirty = true;
  RefreshTexts();
}

void FtpScreen::Restart() {
  if (!Snap.Running)
    return;
  Be.FtpStop();
  std::string Err;
  if (!Be.FtpStart(Cfg.ToConfig(), Err)) {
    Cfg.Enabled = false;
    Error = "Could not restart the FTP server: " + Err;
    Save();
  }
  Rates_.Reset();
  Snap = Be.FtpState();
  Header.Set(Snap.Running, Now_, true);
  Dirty = true;
}

void FtpScreen::ToggleSecret() {
  ShowSecret = !ShowSecret;
  RefreshTexts();
}

void FtpScreen::EditTile(int I) {
  if (Host.IsOpen())
    return;
  std::string Out, Why;
  switch (I) {
  case User:
    if (Be.KeyboardText("FTP user name", Cfg.User, KeyboardKind::Text, Out)) {
      if (ValidFtpUser(Out, &Why)) {
        Cfg.User = Out;
        Save();
        Restart();
        SetStatus("");
      } else {
        SetStatus(Why);
      }
    }
    break;
  case Password:
    if (Be.KeyboardText("FTP password", Cfg.Password, KeyboardKind::Password, Out)) {
      if (ValidFtpPassword(Out, &Why)) {
        Cfg.Password = Out;
        Save();
        Restart();
        SetStatus("");
      } else {
        SetStatus(Why);
      }
    }
    break;
  case Port: {
    int P = 0;
    if (Be.KeyboardText("FTP port (1024-65535)", std::to_string(Cfg.Port), KeyboardKind::Number, Out)) {
      if (ParseFtpPort(Out, P, &Why)) {
        Cfg.Port = P;
        Save();
        Restart();
        SetStatus("");
      } else {
        SetStatus(Why);
      }
    }
    break;
  }
  case Folder: {
    Request Rq;
    Rq.K = Request::Kind::OpenFilePicker;
    Rq.Picker.Title = "Choose the FTP folder";
    Rq.Picker.Folder = true;
    Rq.Picker.Current = Cfg.Root;
    Rq.Picker.FallbackLocation = "rootfs";
    Rq.OnPicked = [this](const std::string& Path) {
      if (!Path.empty() && Path[0] == '/') {
        Cfg.Root = Path;
        Save();
        Restart();
        Dirty = true;
      }
    };
    if (OnRequest)
      OnRequest(Rq);
    break;
  }
  case Background:
    Cfg.Background = !Cfg.Background;
    Save();
    if (Snap.Running)
      Restart();
    Dirty = true;
    break;
  }
  RefreshTexts();
}

void FtpScreen::AskDisconnect(int RowIndex) {
  if (Host.IsOpen() || RowIndex < 0 || RowIndex >= int(Rows.size()))
    return;
  ClientRow& R = *Rows[size_t(RowIndex)];
  DisconnectId = R.ClientId;
  ConfirmM.Title = "Disconnect " + R.Name + "?";
  ConfirmM.Message = R.Address + " is dropped. It can connect again with the password.";
  ConfirmM.Open(Now_);
  Host.Open(ConfirmM);
}

void FtpScreen::RefreshTexts() {
  Tiles[User]->Value = Cfg.User;
  Tiles[Password]->Value = MaskedPassword(Cfg.Password, ShowSecret);
  Tiles[Password]->ShowSecret = ShowSecret;
  Tiles[Port]->Value = std::to_string(Cfg.Port);
  Tiles[Folder]->Value = Cfg.Root;
  Tiles[Background]->SetOn(Cfg.Background, Now_);
  const bool Run = Snap.Running;
  Bar.Segs.clear();
  if (!Message.empty() && Now_ < MessageUntil)
    Bar.Status = Message;
  else if (!Error.empty())
    Bar.Status = Error;
  else if (Run) {
    // The arrows come from JetBrains Mono (Nunito Sans has none): "N clients · ↓ speed · ↑ speed".
    Bar.Status = FtpSummaryText(Snap.Clients.size(), Rates_.Total());
    using K = HintBar::Seg::Kind;
    Bar.Segs = {{K::Body, std::to_string(Snap.Clients.size()) + (Snap.Clients.size() == 1 ? " client \xC2\xB7" : " clients \xC2\xB7")},
                {K::Mono, "\xE2\x86\x93"}, {K::Body, FormatRate(Rates_.Total().DownBps) + " \xC2\xB7"},
                {K::Mono, "\xE2\x86\x91"}, {K::Body, FormatRate(Rates_.Total().UpBps)}};
  }
  else
    Bar.Status = "Passwords travel unencrypted; use trusted networks only";
}

void FtpScreen::RebuildRows() {
  std::vector<uint64_t> Ids;
  for (const FtpClientView& C : Snap.Clients)
    Ids.push_back(C.Id);
  if (Ids != RowIds) {
    // Keep the widgets of clients that stay (so the focus survives), make new ones, drop the rest.
    std::vector<std::unique_ptr<ClientRow>> New;
    for (uint64_t Id : Ids) {
      auto It = std::find_if(Rows.begin(), Rows.end(), [Id](const auto& R) { return R && R->ClientId == Id; });
      if (It != Rows.end()) {
        New.push_back(std::move(*It));
      } else {
        auto R = std::make_unique<ClientRow>();
        R->ClientId = Id;
        R->Group = RowGroup;
        ClientRow* P = R.get();
        R->OnActivate = [this, P]() {
          for (size_t K = 0; K < Rows.size(); ++K)
            if (Rows[K].get() == P)
              AskDisconnect(int(K));
        };
        New.push_back(std::move(R));
      }
    }
    Rows = std::move(New);
    RowIds = Ids;
    BuildRoot();  // a focused row that is gone leaves the focus on the first enabled widget
  }
  for (size_t I = 0; I < Snap.Clients.size(); ++I) {
    const FtpClientView& C = Snap.Clients[I];
    ClientRow& R = *Rows[I];
    const FtpRates::Speed Sp = Rates_.Of(C.Id);
    R.Name = C.LoggedIn ? C.User : "Not signed in";
    R.Address = C.Address;
    R.Activity = FtpActivityText(C);
    R.Down = FormatRate(Sp.DownBps);
    R.Up = FormatRate(Sp.UpBps);
    R.DownActive = Sp.DownBps >= 1;
    R.UpActive = Sp.UpBps >= 1;
  }
  Layout();
}

void FtpScreen::Layout() {
  float Y = FirstRowY;
  for (auto& R : Rows) {
    R->Bounds = Rect {SideX, Y, ContentW, RowH};
    Y += RowH;
  }
  if (Rows.empty())
    Y += 40;
  ContentEnd = Y + BottomPad;
  Scroll.SetViewport(ViewTop, ViewH, ContentEnd - ViewTop);
  for (auto& T : Tiles)
    T->Layout();
}

void FtpScreen::Update(uint64_t Now, float Scale, const std::vector<Event>& Raw) {
  Now_ = Now;
  Host.Tick(Now);
  Snap = Be.FtpState();
  if (Snap.Running != Header.On() && !Header.Pressed)
    Header.Set(Snap.Running, Now, true);  // the server stopped (or started) on its own
  if (Snap.Running)
    Rates_.Update(Now, Snap);
  else
    Rates_.Reset();
  RebuildRows();
  RefreshTexts();
  Interact.Update(Now, Scale, Raw);
}

void FtpScreen::Draw(const DrawContext& C, uint64_t Now) {
  Now_ = Now;
  const Theme& T = C.T;
  const Frame F {C, Now, 0};
  const Frame Fc {C, Now, Interact.ScrollOffset(Now)};

  // Content, clipped to the view below the header.
  C.R.PushClip(C.Du(0, ViewTop, 1280, ViewH));
  Fc.Text(ConnectLabel, "Connect from your computer", SideX, AddrLabelY, 22, T.TextMuted);
  const std::string Addr = FtpAddressText(Snap, Snap.Running);
  const bool On = Snap.Running;
  Fc.Text(AddrStyle, Addr, SideX, AddrY, 46, On ? T.Text : T.TextMuted);
  for (auto& Tl : Tiles)
    Tl->Draw(Fc);
  DrawSectionHeader(Fc, SideX, ClientsHeaderY, ContentW, "Connected clients", On ? "" : "");
  if (!On) {
    Fc.Text(Empty16, "Turn the server on to accept connections.", SideX, FirstRowY, 40, T.TextMuted);
  } else if (Rows.empty()) {
    Fc.Text(Empty16, "No clients yet. Connect with an FTP client and the password above.", SideX, FirstRowY, 40, T.TextMuted);
  } else {
    for (auto& R : Rows) {
      const float Y = R->Bounds.Y - Fc.ScrollY;
      if (Y + R->Bounds.H > ViewTop && Y < ViewTop + ViewH)
        R->Draw(Fc);
    }
  }
  C.R.PopClip();

  // Header: transfer icon, title, the server toggle; rule.
  DrawScreenHeader(F, IconId::Transfer, "FTP server");
  Header.Draw(F);
  Bar.Draw(F);
  Host.Draw(F, {&ConfirmM});
  Interact.DrawOverlay(F);
}

} // namespace ui
