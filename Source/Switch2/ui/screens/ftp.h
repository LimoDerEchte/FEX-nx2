// SPDX-License-Identifier: MIT
// FTP server screen (spec "FTP server"): header with the server Toggle, `ftp://<ip>:<port>` (or "Server is off"), five credential
// tiles (user, password with an eye, port, folder, keep running in apps), the connected clients with real counters from the
// server (speeds refresh at 1 Hz, smoothed over 3 s), a disconnect confirm, and a HintBar. Y starts or stops the server from
// anywhere (on the Password tile it shows or hides the password). The settings live in ui.json (ftp_*); the first run makes a
// random 8-character password. Editing a credential while the server runs restarts it (the clients reconnect).
#pragma once

#include <memory>

#include "FtpModel.h"
#include "components/Tiles.h"
#include "screens/Screen.h"

namespace ui {

namespace ftp_layout {
inline constexpr float HeaderH = 88, ViewTop = 88, ViewH = 568;
inline constexpr float SideX = 48, ContentW = 1184;
inline constexpr float AddrLabelY = 120, AddrY = 148, TileY = 222, TileH = 84, TileW = 224, TileGap = 16;
inline constexpr float ClientsHeaderY = 342, FirstRowY = 370, RowH = 72, BottomPad = 48;
inline constexpr int TileGroup = 1001, RowGroup = 1002;
} // namespace ftp_layout

class FtpScreen final : public Screen {
public:
  enum TileId { User = 0, Password = 1, Port = 2, Folder = 3, Background = 4 };

  // `Rand` makes the first-run password (the console passes a random source; tests pass a fixed one).
  FtpScreen(LauncherBackend& B, SettingsPaths Paths = {}, std::function<uint32_t()> Rand = {});
  ~FtpScreen() override;

  void Update(uint64_t Now, float Scale, const std::vector<Event>& Raw) override;
  void Draw(const DrawContext& C, uint64_t Now) override;
  Interaction& Input() override { return Interact; }

  // ---- state (tests and the host tool) ----
  const FtpSettings& Settings() const { return Cfg; }
  const FtpSnapshot& Snapshot() const { return Snap; }
  const FtpRates& Rates() const { return Rates_; }
  bool PasswordShown() const { return ShowSecret; }
  Toggle& HeaderToggle() { return Header; }
  CredentialTile& Tile(int I) { return *Tiles[size_t(I)]; }
  int ClientRowCount() const { return int(Rows.size()); }
  ClientRow& Row(int I) { return *Rows[size_t(I)]; }
  HintBar& Hints() { return Bar; }
  ConfirmDialog& Confirm() { return ConfirmM; }
  ModalHost& Modals() { return Host; }
  ScrollView& Scrolling() { return Scroll; }
  std::string AddressText() const;
  const std::string& StatusText() const { return Bar.Status; }
  // Actions (also what the buttons do).
  void SetRunning(bool On);          // Y and the header toggle
  void ToggleRunning() { SetRunning(!Snap.Running); }
  void EditTile(int I);              // A on a tile
  void ToggleSecret();
  void AskDisconnect(int RowIndex);  // A on a client row

private:
  void Save();
  void Restart();                    // after a credential change while running
  void Refresh(uint64_t Now);
  void RebuildRows();
  void Layout();
  void RefreshTexts();
  void SetStatus(const std::string& S, bool Sticky = false);
  void BuildRoot();

  LauncherBackend& Be;
  SettingsPaths Paths;
  FtpSettings Cfg;
  FtpSnapshot Snap;
  FtpRates Rates_;
  Interaction Interact;
  ModalHost Host {Interact};
  ScrollView Scroll;
  Toggle Header;
  std::vector<std::unique_ptr<CredentialTile>> Tiles;
  std::vector<std::unique_ptr<ClientRow>> Rows;
  std::vector<uint64_t> RowIds;
  HintBar Bar;
  ConfirmDialog ConfirmM;
  uint64_t DisconnectId = 0;
  bool ShowSecret = false;
  bool Dirty = true;
  uint64_t Now_ = 0;
  std::string Error;          // the last start failure, shown in the HintBar until the next start
  std::string Message;        // a short notice (a refused value), shown for 6 s
  uint64_t MessageUntil = 0;
  float ContentEnd = 0;
};

} // namespace ui
