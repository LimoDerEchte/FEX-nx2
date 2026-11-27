// SPDX-License-Identifier: MIT
// Host tests for Task 6 (make check-ui): the FTP state model (start/stop, credential validation, 1024..65535 port range, first-run
// password, ui.json keys, speeds and their 3 s smoothing), the FTP, RootFS setup, Installation and Shell screens against
// FakeBackend (install progress weighting, ETA with its 10 s window and "estimating..." for the first 3 s, cancel cleanup and the
// kept download), the terminal cell grid (emulation and the ~138x25 size at 1280x720) and the tab strip layout. Fake clock and
// renderer: no GPU.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "FtpModel.h"
#include "InstallModel.h"
#include "Json.h"
#include "PathUtil.h"
#include "RootfsSelection.h"
#include "TerminalGrid.h"
#include "backend/FakeBackend.h"
#include "platform/Log.h"
#include "platform/fake/FakePlatform.h"
#include "screens/ftp.h"
#include "screens/install.h"
#include "screens/rootfs_setup.h"
#include "screens/shell.h"

namespace {
int Failures = 0, Checks = 0;

#define CHECK(Cond)                                                          \
  do {                                                                       \
    ++Checks;                                                                \
    if (!(Cond)) {                                                           \
      ++Failures;                                                            \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond);            \
    }                                                                        \
  } while (0)

using namespace ui;

Event Btn(Button B, bool Down) {
  Event E;
  E.Kind = Down ? Event::Type::ButtonDown : Event::Type::ButtonUp;
  E.Btn = B;
  return E;
}

struct Rig {
  FakeDisplay D {Size {1280, 720}};
  FakeRenderer R {D};
  Fonts F {R};
  Icons I {R};
  uint64_t Now = 1000;
  Rig() { SetTheme(ThemeMode::Dark, AccentPreset::Cyan); }
  DrawContext Ctx() { return DrawContext {R, F, I, CurrentTheme(), 1.0f}; }
};

template <class S> struct ScreenRig : Rig {
  std::unique_ptr<S> Scr;
  std::vector<Request> Reqs;
  void Hook() {
    Scr->OnRequest = [this](const Request& R) { Reqs.push_back(R); };
  }
  void Step(const std::vector<Event>& Ev = {}, uint64_t Advance = 16) {
    Now += Advance;
    Scr->Update(Now, 1.0f, Ev);
    R.Calls.clear();
    const DrawContext C = Ctx();
    Scr->Draw(C, Now);
  }
  void Run(uint64_t Ms) {
    for (uint64_t T = 0; T < Ms; T += 16)
      Step();
  }
  void Press(Button B) {
    Step({Btn(B, true)});
    Step({Btn(B, false)});
    Step();
  }
  Widget* Focused() { return Scr->Input().FocusedWidget(); }
  bool Sent(Request::Kind K) const {
    for (const Request& Rq : Reqs)
      if (Rq.K == K)
        return true;
    return false;
  }
};

// The fixed random source of the tests: the first password is "aaaaaaaa"-like and known.
uint32_t Counter = 0;
uint32_t FixedRand() { return Counter++ * 7919u; }

bool HintEnabled(HintBar& H, Button B) {
  std::vector<Widget*> W;
  H.Children(W);
  for (Widget* X : W)
    if (static_cast<HintButton*>(X)->H.Btn == B)
      return X->Enabled;
  return false;
}

bool Has(const std::string& S, const std::string& Part) { return S.find(Part) != std::string::npos; }

// ---- FTP model ------------------------------------------------------------------------------------------------------------

void TestFtpValidation() {
  std::string Why;
  CHECK(ValidFtpUser("switch") && ValidFtpUser("a") && ValidFtpUser(std::string(32, 'x')));
  CHECK(!ValidFtpUser("", &Why) && !Why.empty());
  CHECK(!ValidFtpUser(std::string(33, 'x')) && !ValidFtpUser("two words") && !ValidFtpUser("tab\t") && !ValidFtpUser("caf\xC3\xA9"));
  CHECK(ValidFtpPassword("Passw0rd!") && ValidFtpPassword("with space inside") && ValidFtpPassword(std::string(64, 'p')));
  CHECK(!ValidFtpPassword("") && !ValidFtpPassword(" lead") && !ValidFtpPassword("trail ") && !ValidFtpPassword(std::string(65, 'p')) && !ValidFtpPassword("n\xC3\xB6"));
  int P = 0;
  CHECK(ParseFtpPort("1024", P) && P == 1024);
  CHECK(ParseFtpPort("65535", P) && P == 65535);
  CHECK(ParseFtpPort("1337", P) && P == 1337);
  CHECK(!ParseFtpPort("1023", P, &Why) && Has(Why, "1024"));
  CHECK(!ParseFtpPort("65536", P) && !ParseFtpPort("0", P) && !ParseFtpPort("21", P));
  CHECK(!ParseFtpPort("", P) && !ParseFtpPort("abc", P) && !ParseFtpPort("12 3", P) && !ParseFtpPort("+8080", P) && !ParseFtpPort("-1", P) && !ParseFtpPort("123456", P));
  CHECK(FtpPortMin == 1024 && FtpPortMax == 65535);
  // Random password: eight characters, no look-alikes, rejection sampling reads the source again when it would bias.
  uint32_t K = 0;
  const std::string A = RandomFtpPassword([&]() { return K += 0x01234567u; });
  CHECK(A.size() == 8 && ValidFtpPassword(A));
  bool Clean = true;
  for (char C : A)
    Clean &= std::string("0O1lI").find(C) == std::string::npos && std::isalnum(static_cast<unsigned char>(C));
  CHECK(Clean);
  int Calls = 0;
  const std::string B = RandomFtpPassword([&]() { return ++Calls == 1 ? 0xFFFFFFFFu : 5u; });
  CHECK(B.size() == 8 && Calls == 9);  // the first value is above the unbiased limit and is skipped
}

void TestFtpSettings() {
  FakeStorage S;
  SettingsPaths Paths;
  Counter = 1;
  FtpSettings F = FtpSettings::Load(S, Paths, FixedRand);
  CHECK(F.PasswordGenerated && F.Password.size() == 8 && F.User == "switch" && F.Port == 1337 && F.Root == "/" && !F.Enabled && !F.Background);
  std::string Err;
  F.Enabled = true;
  F.Port = 2121;
  F.Root = "/home/fex";
  F.User = "me";
  F.Background = true;
  CHECK(F.Save(S, Paths, Err));
  std::string Text;
  CHECK(S.ReadFile(Paths.UiJson, Text));
  Json J;
  CHECK(Json::Parse(Text, J, Err));
  CHECK(J.Find("ftp_enabled")->AsBool() && J.Find("ftp_user")->AsString() == "me" && J.Find("ftp_port")->AsInt() == 2121 &&
        J.Find("ftp_root")->AsString() == "/home/fex" && J.Find("ftp_background")->AsBool() && J.Find("ftp_pass")->AsString() == F.Password);
  const FtpSettings G = FtpSettings::Load(S, Paths, FixedRand);
  CHECK(!G.PasswordGenerated && G.Password == F.Password && G.Port == 2121 && G.User == "me" && G.Enabled && G.Background && G.Root == "/home/fex");
  // Other keys stay; invalid stored values are replaced by the defaults, never half used.
  S.AddFile("sdmc:/fex/config/ui.json", 0, 0, "{\"theme\":\"light\",\"ftp_port\":80,\"ftp_user\":\"bad user\",\"ftp_pass\":\"\",\"ftp_root\":\"nope\"}");
  Counter = 5;
  FtpSettings H = FtpSettings::Load(S, Paths, FixedRand);
  CHECK(H.Port == 1337 && H.User == "switch" && H.Root == "/" && H.PasswordGenerated);
  CHECK(H.Save(S, Paths, Err));
  S.ReadFile(Paths.UiJson, Text);
  Json::Parse(Text, J, Err);
  CHECK(J.Find("theme") && J.Find("theme")->AsString() == "light" && J.Find("ftp_port")->AsInt() == 1337);
  // A corrupt file is kept aside and replaced.
  S.AddFile("sdmc:/fex/config/ui.json", 0, 0, "{not json");
  CHECK(H.Save(S, Paths, Err) && S.Exists("sdmc:/fex/config/ui.json.bad"));
  const FtpConfig C = H.ToConfig();
  CHECK(C.Port == 1337 && C.User == "switch" && C.Password == H.Password);
}

void TestFtpRates() {
  FtpRates R;
  FtpSnapshot S;
  S.Running = true;
  FtpClientView A;
  A.Id = 7;
  S.Clients = {A};
  R.Update(1000, S);
  CHECK(R.SampleCount() == 1 && R.Total().DownBps == 0 && R.Of(7).UpBps == 0);  // one sample: no speed yet
  S.Clients[0].BytesIn = 1000000;
  S.TotalIn = 1000000;
  R.Update(1500, S);  // under a second since the last sample: not a sample
  CHECK(R.SampleCount() == 1);
  R.Update(2000, S);
  CHECK(R.SampleCount() == 2 && std::fabs(R.Total().DownBps - 1000000) < 1 && std::fabs(R.Of(7).DownBps - 1000000) < 1 && R.Of(7).UpBps == 0);
  // A burst is spread over the 3 s window: 0 / 1 MB / 1 MB / 4 MB over three seconds = 1.0, then it decays when the burst leaves.
  S.Clients[0].BytesIn = 1000000;
  S.TotalIn = 1000000;
  R.Update(3000, S);
  R.Update(4000, S);
  CHECK(std::fabs(R.Total().DownBps - 1000000.0 / 3.0) < 1);  // window 1000..4000: the first MB is still in it, over 3 s
  S.Clients[0].BytesIn = 4000000;
  S.TotalIn = 4000000;
  R.Update(5000, S);
  CHECK(R.SampleCount() == 4 && std::fabs(R.Total().DownBps - 1000000) < 1);  // 3 MB over the 3 s window 2000..5000
  // A client that left drops out of the per-client speeds; a new one counts from its first sample.
  S.Clients.clear();
  FtpClientView N;
  N.Id = 9;
  N.BytesOut = 500;
  S.Clients = {N};
  R.Update(6000, S);
  CHECK(R.Of(7).DownBps == 0 && R.Of(9).UpBps > 0);
  R.Reset();
  CHECK(R.SampleCount() == 0 && R.Total().DownBps == 0);
  CHECK(FormatRate(0) == "0 B/s" && FormatRate(999) == "999 B/s" && FormatRate(1500) == "1.5 KB/s" && FormatRate(12400000) == "12.4 MB/s" && FormatRate(2.5e9) == "2.5 GB/s");
  FtpClientView V;
  V.LoggedIn = true;
  CHECK(FtpActivityText(V) == "Idle");
  V.State = FtpClientView::Act::Receiving;
  V.File = "/home/fex/a.tar";
  V.Done = 61;
  V.Total = 100;
  CHECK(FtpActivityText(V) == "Receiving a.tar \xC2\xB7 61%");
  V.State = FtpClientView::Act::Sending;
  V.Total = 0;
  CHECK(FtpActivityText(V) == "Sending a.tar");
  V.State = FtpClientView::Act::Idle;
  V.LoggedIn = false;
  CHECK(FtpActivityText(V) == "Not logged in");
  CHECK(FtpSummaryText(1, {1500, 0}) == "1 client \xC2\xB7 \xE2\x86\x93 1.5 KB/s \xC2\xB7 \xE2\x86\x91 0 B/s");
  FtpSnapshot Snap;
  Snap.Address = "10.0.0.2";
  Snap.Port = 1337;
  CHECK(FtpAddressText(Snap, true) == "Server is off");
  Snap.Running = true;
  CHECK(FtpAddressText(Snap, true) == "ftp://10.0.0.2:1337" && FtpAddressText(Snap, false) == "Server is off");
  CHECK(MaskedPassword("abc", false) == "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" && MaskedPassword("abc", true) == "abc");
}

// ---- FTP screen -----------------------------------------------------------------------------------------------------------

struct FtpRig : ScreenRig<FtpScreen> {
  FakeBackend B;
  FtpRig() {
    Counter = 1;
    Scr = std::make_unique<FtpScreen>(B, SettingsPaths {}, FixedRand);
    Hook();
    Step();
  }
};

void TestFtpScreen() {
  FtpRig T;
  FtpScreen& S = *T.Scr;
  CHECK(T.Focused() == &S.HeaderToggle());
  CHECK(!S.Snapshot().Running && S.AddressText() == "Server is off" && !S.PasswordShown());
  CHECK(S.Tile(FtpScreen::User).Value == "switch" && S.Tile(FtpScreen::Port).Value == "1337" && S.Tile(FtpScreen::Folder).Value == "/");
  CHECK(S.Tile(FtpScreen::Password).Value.size() == 8 * 3);  // eight bullets (3 bytes each)
  CHECK(S.Settings().Password.size() == 8);
  CHECK(Has(S.StatusText(), "unencrypted"));
  // The first-run password was saved at once.
  std::string Text;
  CHECK(T.B.Fs().ReadFile("sdmc:/fex/config/ui.json", Text) && Has(Text, S.Settings().Password));

  // Y starts the server (anywhere but the Password tile), Y again stops it.
  T.Press(Button::Y);
  CHECK(S.Snapshot().Running && T.B.FtpUp && S.HeaderToggle().On() && S.AddressText() == "ftp://192.168.1.42:1337");
  CHECK(T.B.LastFtpConfig.User == "switch" && T.B.LastFtpConfig.Password == S.Settings().Password && T.B.LastFtpConfig.Port == 1337);
  T.B.Fs().ReadFile("sdmc:/fex/config/ui.json", Text);
  CHECK(Has(Text, "\"ftp_enabled\": true"));
  CHECK(Has(S.StatusText(), "0 clients"));
  T.Press(Button::Y);
  CHECK(!S.Snapshot().Running && !T.B.FtpUp && !S.HeaderToggle().On());
  T.B.Fs().ReadFile("sdmc:/fex/config/ui.json", Text);
  CHECK(Has(Text, "\"ftp_enabled\": false"));

  // The header toggle does the same through the focus path (A on the focused toggle).
  T.Press(Button::A);
  CHECK(S.Snapshot().Running);
  T.Press(Button::A);
  CHECK(!S.Snapshot().Running);

  // No network: start fails, the toggle stays off, the reason is in the HintBar.
  T.B.Address.clear();
  T.Press(Button::Y);
  CHECK(!S.Snapshot().Running && !S.HeaderToggle().On() && Has(S.StatusText(), "not connected to a network") && !S.Settings().Enabled);
  T.B.Address = "192.168.1.42";
  T.B.FtpStartError = "bind: address in use";
  T.Press(Button::Y);
  CHECK(!S.Snapshot().Running && Has(S.StatusText(), "address in use"));
  T.B.FtpStartError.clear();
  T.Press(Button::Y);
  CHECK(S.Snapshot().Running && !Has(S.StatusText(), "Could not"));

  // Navigation: Down from the toggle reaches the tiles; Left/Right move between tiles; Y on Password toggles the eye, not the server.
  T.Press(Button::Down);
  CHECK(T.Focused() == &S.Tile(FtpScreen::User));
  T.Press(Button::Right);
  CHECK(T.Focused() == &S.Tile(FtpScreen::Password));
  T.Press(Button::Y);
  CHECK(S.PasswordShown() && S.Snapshot().Running && S.Tile(FtpScreen::Password).Value == S.Settings().Password);
  T.Press(Button::Y);
  CHECK(!S.PasswordShown() && S.Tile(FtpScreen::Password).Value.size() == 24);
  T.Press(Button::Up);
  CHECK(T.Focused() == &S.HeaderToggle());

  // Editing: valid values apply and restart the running server; invalid ones are refused with the reason and change nothing.
  T.Press(Button::Down);  // the last focused tile (Password)
  CHECK(T.Focused() == &S.Tile(FtpScreen::Password));
  T.Press(Button::Left);
  T.B.KeyboardAnswers = {"newuser"};
  T.Press(Button::A);
  CHECK(S.Settings().User == "newuser" && T.B.LastFtpConfig.User == "newuser" && S.Tile(FtpScreen::User).Value == "newuser");
  T.Press(Button::Right);
  T.B.KeyboardAnswers = {"bad pass word "};
  T.Press(Button::A);
  CHECK(S.Settings().Password != "bad pass word " && Has(S.StatusText(), "no space at the ends"));
  T.B.KeyboardAnswers = {"S3cret!x"};
  T.Press(Button::A);
  CHECK(S.Settings().Password == "S3cret!x" && T.B.LastFtpConfig.Password == "S3cret!x");
  T.Press(Button::Right);
  T.B.KeyboardAnswers = {"80"};
  T.Press(Button::A);
  CHECK(S.Settings().Port == 1337 && Has(S.StatusText(), "1024 to 65535"));
  T.B.KeyboardAnswers = {"abc"};
  T.Press(Button::A);
  CHECK(S.Settings().Port == 1337);
  T.B.KeyboardAnswers = {"65535"};
  T.Press(Button::A);
  CHECK(S.Settings().Port == 65535 && T.B.LastFtpConfig.Port == 65535 && S.AddressText() == "ftp://192.168.1.42:65535");
  T.B.KeyboardAnswers = {};  // cancelled: nothing changes
  T.Press(Button::A);
  CHECK(S.Settings().Port == 65535);
  CHECK(T.B.KeyboardHeaders.size() >= 5);
  T.B.Fs().ReadFile("sdmc:/fex/config/ui.json", Text);
  CHECK(Has(Text, "\"ftp_port\": 65535") && Has(Text, "\"ftp_user\": \"newuser\""));

  // Folder tile asks for a folder picker over the RootFS; the result is stored.
  T.Press(Button::Right);
  T.Reqs.clear();
  T.Press(Button::A);
  CHECK(T.Reqs.size() == 1 && T.Reqs[0].K == Request::Kind::OpenFilePicker && T.Reqs[0].Picker.Folder && T.Reqs[0].Picker.FallbackLocation == "rootfs");
  if (!T.Reqs.empty() && T.Reqs[0].OnPicked)
    T.Reqs[0].OnPicked("/home/fex");
  CHECK(S.Settings().Root == "/home/fex" && T.B.LastFtpConfig.Root == "/home/fex");
  // Keep running in apps flips on A.
  T.Press(Button::Right);
  T.Press(Button::A);
  CHECK(S.Settings().Background && S.Tile(FtpScreen::Background).On);
  T.Press(Button::A);
  CHECK(!S.Settings().Background);

  // B goes back.
  T.Reqs.clear();
  T.Press(Button::B);
  CHECK(T.Sent(Request::Kind::Back));
}

void TestFtpClients() {
  FtpRig T;
  FtpScreen& S = *T.Scr;
  T.Press(Button::Y);
  FtpClientView A;
  A.Id = 1;
  A.Address = "192.168.1.20";
  A.User = "switch";
  A.LoggedIn = true;
  A.State = FtpClientView::Act::Receiving;
  A.File = "/home/fex/save.tar";
  A.Done = 61;
  A.Total = 100;
  FtpClientView B2;
  B2.Id = 2;
  B2.Address = "192.168.1.31";
  B2.User = "switch";
  B2.LoggedIn = true;
  T.B.FtpClients = {A, B2};
  T.Step();
  CHECK(S.ClientRowCount() == 2 && S.Row(0).Name == "switch" && S.Row(0).Address == "192.168.1.20" && S.Row(0).Activity == "Receiving save.tar \xC2\xB7 61%" && S.Row(1).Activity == "Idle");
  CHECK(Has(S.StatusText(), "2 clients"));
  // Speeds come from the counters over one second samples: 2 MB/s in for client 1.
  for (int Sec = 0; Sec < 5; ++Sec) {
    T.B.FtpClients[0].BytesIn += 2000000;
    T.B.FtpTotalIn += 2000000;
    T.Run(1000);
  }
  CHECK(S.Row(0).Down == "2.0 MB/s" && S.Row(0).DownActive && S.Row(0).Up == "0 B/s" && !S.Row(0).UpActive && S.Row(1).Down == "0 B/s" && !S.Row(1).DownActive);
  CHECK(Has(S.StatusText(), "2.0 MB/s"));
  // A client that connects later gets a row; the list scrolls (viewport shorter than the content with many clients).
  for (uint64_t Id = 3; Id < 9; ++Id) {
    FtpClientView C;
    C.Id = Id;
    C.Address = "10.0.0." + std::to_string(Id);
    C.User = "switch";
    C.LoggedIn = true;
    T.B.FtpClients.push_back(C);
  }
  T.Step();
  CHECK(S.ClientRowCount() == 8 && S.Scrolling().MaxOffset() > 0);
  // Down from the toggle reaches the tiles, Down again the client rows; A asks, Cancel is the default, Disconnect drops it.
  T.Press(Button::Down);
  T.Press(Button::Down);
  CHECK(T.Focused() == &S.Row(0));
  T.Press(Button::A);
  CHECK(S.Modals().IsOpen() && S.Confirm().IsOpen() && T.Focused() != &S.Row(0));
  T.Press(Button::A);  // Cancel (the default focus)
  CHECK(!S.Modals().IsOpen() && T.B.Disconnected.empty() && S.ClientRowCount() == 8);
  T.Run(300);
  T.Press(Button::A);
  CHECK(S.Modals().IsOpen());
  T.Run(200);
  T.Press(Button::Right);  // Cancel -> Disconnect
  T.Press(Button::A);
  T.Step();
  CHECK(T.B.Disconnected.size() == 1 && T.B.Disconnected[0] == 1 && S.ClientRowCount() == 7);
  // Stopped: the list is replaced by the muted line.
  T.Press(Button::B);
  T.Reqs.clear();
  T.Press(Button::Y);
  CHECK(!S.Snapshot().Running && S.ClientRowCount() == 0);
}

// ---- Install model --------------------------------------------------------------------------------------------------------

void TestInstallModel() {
  InstallProgress P;
  P.Start(10000);
  CHECK(P.Percent() == 0 && P.StepText() == "Step 1 of 7" && P.EtaText(10000) == "estimating\xE2\x80\xA6");
  P.SetStep(1, "Download x");
  P.SetFraction(0.01, 10100);
  CHECK(P.StepText() == "Step 2 of 7" && P.Percent() == 1);
  // 1 % per second since the start (10 s elapsed at 11 % -> the first sample was at 10100 ms, fraction 0.01).
  // Until 3 s have passed: estimating.
  for (int T = 1; T <= 29; ++T)
    P.SetFraction(0.01 + 0.01 * T / 10.0, 10100 + uint64_t(T) * 100);  // 0.1 % every 100 ms
  CHECK(P.EtaText(12900) == "estimating\xE2\x80\xA6");
  P.SetFraction(0.04, 13100);
  double Eta = 0;
  // 4 % in 3.1 s: 96 % left at 0.04 / 3.1 per second = 74.4 s.
  CHECK(P.Eta(13100, Eta) && std::fabs(Eta - 74.4) < 1.0);
  CHECK(P.EtaText(13100) == "about 1 min left");
  CHECK(P.FooterText(13100) == "Step 2 of 7 \xC2\xB7 about 1 min left");
  // The bar never moves back; a lower value is ignored.
  P.SetFraction(0.02, 13200);
  CHECK(P.Fraction() >= 0.04);
  // The ETA is the average rate over the whole time: a burst does not move it much (a 10 s window would have).
  uint64_t T = 13200;
  double F = P.Fraction();
  const double Before = (1.0 - F) * 3.2 / F;
  for (int I = 0; I < 100; ++I) {  // 10 s at 3x the speed
    T += 100;
    F += 0.003;
    P.SetFraction(F, T);
  }
  CHECK(P.Eta(T, Eta));
  const double Elapsed = double(T - 10000) / 1000.0;
  CHECK(std::fabs(Eta - (1.0 - P.Fraction()) * Elapsed / P.Fraction()) < 1e-6);
  const double Window = (1.0 - P.Fraction()) / 0.03;  // what the last 10 s alone would say
  CHECK(Eta > Window && Eta < Before);
  // A stall: the estimate grows with the time, it does not freeze.
  double Later = 0;
  CHECK(P.Eta(T + 20000, Later) && Later > Eta);
  P.Finish(T + 12000);
  CHECK(P.Percent() == 100 && P.Finished() && P.Fraction() == 1.0 && P.FooterText(T + 12000) == "Step 2 of 7");
  CHECK(FormatEta(3) == "a few seconds" && FormatEta(40) == "about 40 s" && FormatEta(125) == "about 2 min" && FormatEta(3900) == "about 1 h 05 min");
  CHECK(FormatStamp(0) == "[00:00.00]" && FormatStamp(61230) == "[01:01.23]" && FormatStamp(6000000) == "[100:00.00]");
  // Log line format: the stamp, then the kind prefix, then the text.
  auto Spans = InstallLogSpans(1500, InstallLogKind::Warn, "Skipped device node");
  CHECK(Spans.size() == 3 && Spans[0].R == LogSpan::Role::Stamp && Spans[0].Text == "[00:01.50] " && Spans[1].R == LogSpan::Role::Warn && Spans[1].Text == "warn: " && Spans[2].Text == "Skipped device node");
  Spans = InstallLogSpans(0, InstallLogKind::Ok, "x");
  CHECK(Spans[1].R == LogSpan::Role::Ok && Spans[1].Text == "ok ");
  Spans = InstallLogSpans(0, InstallLogKind::Error, "x");
  CHECK(Spans[1].R == LogSpan::Role::Error && Spans[1].Text == "error: ");
  Spans = InstallLogSpans(0, InstallLogKind::Info, "x");
  CHECK(Spans.size() == 2);
  // Space: the installer's formula.
  RootfsImage I = *BuiltInRootfsIndex().Find("ubuntu", "24.04");
  CHECK(InstallSpaceNeeded(I, false, true) == I.InstalledBytes + I.InstalledBytes / 20 + (64ull << 20) + I.DownloadBytes);
  CHECK(InstallSpaceNeeded(I, true, false) == I.InstalledBytes + I.InstalledBytes / 20);
  CHECK(InstallDownloadBytes(I, false, true) == I.DownloadBytes + I.ExtraDownloadBytes && InstallDownloadBytes(I, true, false) == 0);
}

// ---- Install screen -------------------------------------------------------------------------------------------------------

struct InstallRig : ScreenRig<InstallScreen> {
  FakeBackend B;
  std::vector<Event> Inject;     // events fed at pump number InjectAt
  int Pumps = 0, InjectAt = -1;
  std::vector<int> StepPercent;  // percent at every step change
  explicit InstallRig(bool Local = false, bool Wipe = false) {
    InstallRequest Rq;
    Rq.WipeOnly = Wipe;
    if (!Wipe)
      Rq.Image = *BuiltInRootfsIndex().Find("ubuntu", "24.04");
    if (Local)
      Rq.LocalPath = "/fex/images/Ubuntu_24_04.sqsh";
    B.PumpsPerStep = 10;
    B.Root.Installed = Wipe;
    Scr = std::make_unique<InstallScreen>(B, Rq);
    Hook();
    Step();  // the app updates the screen before it runs the installer
    Scr->PumpHook = [this]() {
      std::vector<Event> Ev;
      if (Pumps == InjectAt)
        Ev = Inject;
      ++Pumps;
      Step(Ev, 100);
    };
  }
};

void TestInstallScreen() {
  // A complete run: seven steps, weighted percentage, steps and log.
  {
    InstallRig T;
    InstallScreen& S = *T.Scr;
    CHECK(S.NeedsRun() && S.GetState() == InstallScreen::State::Idle);
    CHECK(S.Hints().HintCount() == 1);
    int LastPct = -1;
    bool Mono = true;
    int Boundaries = 0;
    T.Scr->PumpHook = [&]() {
      ++T.Pumps;
      T.Step({}, 100);
      Mono &= S.Progress().Percent() >= LastPct;
      LastPct = S.Progress().Percent();
    };
    S.Run();
    CHECK(!S.NeedsRun() && S.GetState() == InstallScreen::State::Done && S.Outcome().Ok);
    CHECK(Mono && S.Progress().Percent() == 100 && S.Progress().Finished());
    CHECK(S.Progress().StepIndex() == 6 && S.Progress().StepText() == "Step 7 of 7" && S.Progress().StepTitle() == "Finish");
    CHECK(T.Pumps == 70);
    // Weighting: the fake reports the installer's weights 1/30/6/40/2/20/1 of 100; a step boundary lands on its cumulative weight.
    FakeBackend B2;
    struct Rec : InstallEvents {
      std::vector<double> AtStepEnd;
      double Last = 0;
      int Pending = -1;
      void Step(int I, const std::string&) override {
        if (Pending >= 0 && I != Pending)
          AtStepEnd.push_back(Last);
        Pending = I;
      }
      void Progress(double F, const std::string&) override { Last = F; }
      void Log(InstallLogKind, const std::string&) override {}
      void Pump() override {}
      bool Cancelled() override { return false; }
    } R;
    InstallRequest Rq;
    Rq.Image = *BuiltInRootfsIndex().Find("ubuntu", "24.04");
    B2.PumpsPerStep = 3;
    B2.InstallRootfs(Rq, R);
    const double Want[] = {0.01, 0.31, 0.37, 0.77, 0.79, 0.99};
    bool Ok = R.AtStepEnd.size() == 6;
    for (size_t I = 0; Ok && I < 6; ++I)
      Ok = std::fabs(R.AtStepEnd[I] - Want[I]) < 1e-9;
    CHECK(Ok);
    (void)Boundaries;
    // The log: stamps, ok / warn prefixes, in order; the cursor view keeps up to 2,000 lines.
    CHECK(S.Log().LineCount() > 10);
    bool SawWarn = false, SawOk = false, SawStep = false;
    for (size_t I = 0; I < S.Log().LineCount(); ++I) {
      const std::string& L = S.Log().Line(I);
      SawWarn |= Has(L, "] warn: Skipped device node");
      SawOk |= Has(L, "] ok Check free space done");
      SawStep |= Has(L, "] Step 4/7  Extract the RootFS");
    }
    CHECK(SawWarn && SawOk && SawStep);
    CHECK(S.Title() == "RootFS installed" && S.Hints().HintCount() == 1);
    // A on Done goes Home.
    T.Press(Button::A);
    CHECK(T.Sent(Request::Kind::OpenHome));
  }
  // Cancel at every step leaves no partial RootFS (the fake mirrors the installer's cleanup); the image stays only after Verify.
  for (int At : {2, 13, 22, 34, 44, 62}) {
    InstallRig T;
    T.InjectAt = At;
    T.Inject = {Btn(Button::B, true), Btn(Button::B, false)};
    InstallScreen& S = *T.Scr;
    // B opens the confirm; one pump later choose "Cancel and remove files" (Down, A).
    int Pump = 0;
    bool Opened = false;
    std::string Label;
    S.PumpHook = [&]() {
      std::vector<Event> Ev;
      if (Pump == At)
        Ev = {Btn(Button::B, true), Btn(Button::B, false)};
      if (Pump == At + 3) {
        Opened = S.Modals().IsOpen();
        Label = S.CancelPicker().Options.size() == 2 ? S.CancelPicker().Options[1] : "";
        Ev = {Btn(Button::Down, true), Btn(Button::Down, false)};
      }
      if (Pump == At + 6)
        Ev = {Btn(Button::A, true), Btn(Button::A, false)};
      ++Pump;
      T.Step(Ev, 100);
    };
    S.Run();
    const int Step = S.Progress().StepIndex();
    CHECK(Opened);
    CHECK(S.Outcome().Cancelled && !S.Outcome().Ok && S.GetState() == InstallScreen::State::Cancelled);
    CHECK(!T.B.PartialRootfs && !T.B.Rootfs().Installed);
    const bool Verified = Step >= FirstStepAfterVerify;
    CHECK(T.B.DownloadKept == Verified);
    CHECK(Label == (Verified ? "Cancel, keep the download" : "Cancel and remove files"));
    CHECK(T.Sent(Request::Kind::Back));
    CHECK(S.CancelRequested());
  }
  // "Keep installing" continues to the end.
  {
    InstallRig T;
    InstallScreen& S = *T.Scr;
    int Pump = 0;
    S.PumpHook = [&]() {
      std::vector<Event> Ev;
      if (Pump == 5)
        Ev = {Btn(Button::B, true), Btn(Button::B, false)};
      if (Pump == 8)
        Ev = {Btn(Button::A, true), Btn(Button::A, false)};  // the first option, "Keep installing"
      ++Pump;
      T.Step(Ev, 100);
    };
    S.Run();
    CHECK(S.Outcome().Ok && !S.CancelRequested() && T.B.Rootfs().Installed);
  }
  // A local image has no download step and nothing downloaded to keep.
  {
    InstallRig T(true);
    InstallScreen& S = *T.Scr;
    int Pump = 0;
    S.PumpHook = [&]() {
      std::vector<Event> Ev;
      if (Pump == 3)
        Ev = {Btn(Button::B, true), Btn(Button::B, false)};
      if (Pump == 6)
        Ev = {Btn(Button::Down, true), Btn(Button::Down, false)};
      if (Pump == 9)
        Ev = {Btn(Button::A, true), Btn(Button::A, false)};
      ++Pump;
      T.Step(Ev, 100);
    };
    S.Run();
    CHECK(S.Outcome().Cancelled && !T.B.DownloadKept && !T.B.PartialRootfs);
    CHECK(S.Subtitle().find("from the SD card") != std::string::npos);
  }
  // Failure: title, hints, error line, then Retry runs again; B goes back.
  {
    InstallRig T;
    T.B.InstallFailsAtStep = 3;
    InstallScreen& S = *T.Scr;
    S.PumpHook = [&]() { T.Step({}, 100); };
    S.Run();
    T.Step();
    CHECK(S.GetState() == InstallScreen::State::Failed && S.Title() == "Installation failed" && !S.Outcome().Error.empty());
    CHECK(S.Hints().HintCount() == 2 && !T.B.PartialRootfs && T.B.DownloadKept);  // a failure keeps the verified image
    bool SawErr = false;
    for (size_t I = 0; I < S.Log().LineCount(); ++I)
      SawErr |= Has(S.Log().Line(I), "] error: ");
    CHECK(SawErr);
    CHECK(S.Progress().Percent() < 100);
    T.Press(Button::A);
    CHECK(S.NeedsRun() && S.GetState() == InstallScreen::State::Idle);
    T.B.InstallFailsAtStep = -1;
    S.PumpHook = [&]() { T.Step({}, 100); };
    S.Run();
    CHECK(S.GetState() == InstallScreen::State::Done && S.Log().LineCount() > 10);
    // B in the failed state returns to RootFS setup.
    InstallRig U;
    U.B.InstallFailsAtStep = 0;
    U.Scr->PumpHook = [&]() { U.Step({}, 100); };
    U.Scr->Run();
    U.Step();
    U.Reqs.clear();
    U.Press(Button::B);
    CHECK(U.Sent(Request::Kind::Back));
  }
  // Wipe mode (Settings > Wipe RootFS): its own title, two steps, no cancel, progress from the removed count; A on Done exits.
  {
    InstallRig T(false, true);
    InstallScreen& S = *T.Scr;
    CHECK(S.Title() == "Wiping the RootFS" && Has(S.Subtitle(), "Removing ") && S.Hints().HintCount() == 0);
    int LastPct = -1;
    bool Mono = true, AskedCancel = false;
    S.PumpHook = [&]() {
      std::vector<Event> Ev;
      if (T.Pumps++ == 6) {  // B while removing does nothing
        Event E;
        E.Kind = Event::Type::ButtonDown;
        E.Btn = Button::B;
        Ev.push_back(E);
        E.Kind = Event::Type::ButtonUp;
        Ev.push_back(E);
      }
      T.Step(Ev, 100);
      AskedCancel |= S.Modals().IsOpen();
      Mono &= S.Progress().Percent() >= LastPct;
      LastPct = S.Progress().Percent();
    };
    S.Run();
    CHECK(S.GetState() == InstallScreen::State::Done && S.Outcome().Ok && !AskedCancel && !S.Outcome().Cancelled);
    CHECK(Mono && T.B.WipeCalls == 1 && T.B.InstallCalls == 0 && !T.B.Root.Installed);
    CHECK(S.Progress().StepText() == "Step 2 of 2" && S.Progress().StepTitle() == "Removing the files" && S.Title() == "RootFS wiped");
    bool SawRemoved = false;
    for (size_t I = 0; I < S.Log().LineCount(); ++I)
      SawRemoved |= Has(S.Log().Line(I), "Removed 400 files and folders");
    CHECK(SawRemoved);
    T.Press(Button::A);
    CHECK(T.Sent(Request::Kind::StartFirstRun));
    // A failing wipe: "Wipe failed", the error, Retry; B goes back to Settings.
    InstallRig F(false, true);
    F.B.WipeFails = true;
    F.Scr->PumpHook = [&]() { F.Step({}, 100); };
    F.Scr->Run();
    F.Step();
    CHECK(F.Scr->GetState() == InstallScreen::State::Failed && F.Scr->Title() == "Wipe failed" && Has(F.Scr->Outcome().Error, "Could not delete"));
    CHECK(F.Scr->Hints().HintCount() == 2);
    F.Reqs.clear();
    F.Press(Button::B);
    CHECK(F.Sent(Request::Kind::Back));
  }
  // The ETA reads "estimating" while less than 3 s of samples exist and then a number (fake clock: 100 ms per pump).
  {
    InstallRig T;
    InstallScreen& S = *T.Scr;
    std::string Early, Late;
    int Pump = 0;
    S.PumpHook = [&]() {
      T.Step({}, 100);
      if (Pump == 20)
        Early = S.Progress().EtaText(T.Now);
      if (Pump == 45)
        Late = S.Progress().EtaText(T.Now);
      ++Pump;
    };
    S.Run();
    CHECK(Early == "estimating\xE2\x80\xA6" && Late != Early && Has(Late, "left"));
  }
}

// ---- RootFS setup ---------------------------------------------------------------------------------------------------------

struct SetupRig : ScreenRig<RootfsSetupScreen> {
  FakeBackend B;
  explicit SetupRig(bool Reinstall, bool Low = false) {
    B.Root.Installed = Reinstall;
    if (Low)
      B.Fs().SetFree("sdmc:/", 1200ull << 20, 127865454592ull);
    Scr = std::make_unique<RootfsSetupScreen>(B, Reinstall);
    Hook();
    Step();
  }
};

void TestRootfsSetup() {
  SetupRig T(false);
  RootfsSetupScreen& S = *T.Scr;
  auto* Ubuntu = S.CardByKey("ubuntu-24.04");
  auto* Next = S.CardByKey("ubuntu-26.04");  // the disabled card beside it
  CHECK(Ubuntu && Next && Ubuntu->Card->Enabled && Ubuntu->Card->Selected && Ubuntu->Card->Tag == "Recommended");
  CHECK(!S.CardByKey("debian-12"));
  for (const char* K : {"ubuntu-26.04", "kde", "xfce", "lxqt", "gnome", "steam", "epic"}) {
    auto* C = S.CardByKey(K);
    CHECK(C && !C->Card->Enabled && !C->Card->Selected && C->Card->Meta == "Not available yet");
  }
  CHECK(S.CardByKey("none")->Card->Selected && S.CardByKey("none")->Card->Enabled && S.CardByKey("extras")->Card->Selected && !S.CardByKey("local-image")->Card->Selected);
  CHECK(Ubuntu->Card->Bounds.H == 88 && S.CardByKey("none")->Card->Bounds.H == 120 && Ubuntu->Card->Meta.find("Download 501 MB") != std::string::npos);
  // Cards: two columns for the base system, five for the desktops (gap 24).
  CHECK(std::fabs(Ubuntu->Card->Bounds.W - 580) < 0.1 && std::fabs(S.CardByKey("none")->Card->Bounds.W - (1184 - 4 * 24) / 5.0f) < 0.1);
  CHECK(Ubuntu->Card->Bounds.X == 48 && std::fabs(Next->Card->Bounds.X - (48 + 580 + 24)) < 0.1);
  const RootfsImage& Img = *BuiltInRootfsIndex().Find("ubuntu", "24.04");
  CHECK(S.DownloadBytes() == Img.DownloadBytes + Img.ExtraDownloadBytes && S.EnoughSpace() && S.FreeBytes() == 19756849561ull);
  CHECK(S.FooterText() == "Download now \xE2\x89\x88 " + FormatBytes(S.DownloadBytes()) + " \xC2\xB7 desktop on first launch \xE2\x89\x88 none");
  CHECK(!S.ReinstallMode() && S.Hints().HintCount() == 3);
  // A on a disabled card does nothing and the D-pad skips it.
  S.Select(*Next);
  CHECK(S.Selection().Base == "ubuntu-24.04");
  CHECK(T.Focused() == Ubuntu->Card.get());
  T.Press(Button::Right);
  CHECK(T.Focused() != Next->Card.get());
  // Extras toggle changes the footer and persists in ui.json.
  S.Select(*S.CardByKey("extras"));
  CHECK(!S.Selection().Extras && S.DownloadBytes() == Img.DownloadBytes);
  std::string Text;
  CHECK(T.B.Fs().ReadFile("sdmc:/fex/config/ui.json", Text) && Has(Text, "\"rootfs_extras\": false") && Has(Text, "\"rootfs_base\": \"ubuntu-24.04\""));
  S.Select(*S.CardByKey("extras"));
  // First run: + Install starts at once with the chosen image.
  T.Reqs.clear();
  T.Press(Button::Plus);
  CHECK(T.Reqs.size() == 1 && T.Reqs[0].K == Request::Kind::StartInstall && T.Reqs[0].Install.Image.FileName == "Ubuntu_24_04.sqsh" && T.Reqs[0].Install.Extras &&
        !T.Reqs[0].Install.Reinstall && T.Reqs[0].Install.LocalPath.empty());
  // B exits the app on a first run.
  T.Reqs.clear();
  T.Press(Button::B);
  CHECK(T.Sent(Request::Kind::Exit));
  // A local image: the picker request, the stored path, a smaller download; A again clears it.
  T.Reqs.clear();
  S.Select(*S.CardByKey("local-image"));
  CHECK(T.Reqs.size() == 1 && T.Reqs[0].K == Request::Kind::OpenFilePicker && T.Reqs[0].Picker.Accept.size() == 1 && T.Reqs[0].Picker.Accept[0] == ".sqsh");
  T.Reqs[0].OnPicked("sdmc:/fex/images/Ubuntu_24_04.sqsh");
  CHECK(S.LocalImage() == "/fex/images/Ubuntu_24_04.sqsh" && S.CardByKey("local-image")->Card->Selected && S.DownloadBytes() == Img.ExtraDownloadBytes);
  CHECK(S.BuildRequest().LocalPath == "/fex/images/Ubuntu_24_04.sqsh");
  S.Select(*S.CardByKey("local-image"));
  CHECK(S.LocalImage().empty());

  // Not enough space: warn text, + Install disabled.
  SetupRig L(false, true);
  CHECK(!L.Scr->EnoughSpace() && Has(L.Scr->FooterText(), "Not enough space") && !HintEnabled(L.Scr->Hints(), Button::Plus));
  L.Reqs.clear();
  L.Press(Button::Plus);
  CHECK(L.Reqs.empty());
  CHECK(HintEnabled(S.Hints(), Button::Plus));
  // A pointer tap on the disabled hint does nothing either (disabled widgets get no pointer).
}


void TestRootfsReinstall() {
  SetupRig T(true);
  RootfsSetupScreen& S = *T.Scr;
  CHECK(S.ReinstallMode() && S.Hints().HintCount() == 3);
  // Cards sit lower than on a first run (the warn banner).
  SetupRig First(false);
  CHECK(S.CardByKey("ubuntu-24.04")->Card->Bounds.Y > First.Scr->CardByKey("ubuntu-24.04")->Card->Bounds.Y + 50);
  // + Reinstall asks first; Cancel is focused; B or Cancel closes without starting.
  T.Reqs.clear();
  T.Press(Button::Plus);
  CHECK(S.Modals().IsOpen() && S.Confirm().IsOpen() && T.Reqs.empty());
  CHECK(S.Confirm().Title == "Reinstall RootFS?" && S.Confirm().Summary.size() == 4 && Has(S.Confirm().Summary[0], "Ubuntu 24.04") && Has(S.Confirm().Summary[3], "MB"));
  T.Run(300);
  T.Press(Button::A);  // Cancel
  CHECK(!S.Modals().IsOpen() && T.Reqs.empty());
  T.Run(300);
  T.Press(Button::Plus);
  T.Press(Button::B);
  CHECK(!S.Modals().IsOpen() && T.Reqs.empty());
  T.Run(300);
  T.Press(Button::Plus);
  T.Run(200);
  T.Press(Button::Right);
  T.Press(Button::A);  // Reinstall
  CHECK(T.Reqs.size() == 1 && T.Reqs[0].K == Request::Kind::StartInstall && T.Reqs[0].Install.Reinstall);
  // B cancels the whole screen back to Settings.
  T.Reqs.clear();
  T.Run(300);
  T.Press(Button::B);
  CHECK(T.Sent(Request::Kind::Back));
}

// ---- Terminal grid --------------------------------------------------------------------------------------------------------

void TestTerminalGrid() {
  TerminalGrid G(20, 5);
  G.Feed("hello\r\nworld");
  CHECK(G.RowText(0) == "hello" && G.RowText(1) == "world" && G.CursorRow() == 1 && G.CursorCol() == 5);
  // With newline mode set, LF returns to the first column. The PTY supplies CR separately by default.
  G.Reset();
  G.Feed("\x1b[20hab\ncd");
  CHECK(G.RowText(0) == "ab" && G.RowText(1) == "cd");
  G.Reset();
  G.Feed("ab\ncd");
  CHECK(G.RowText(0) == "ab" && G.RowText(1) == "  cd");
  // Wrap at the right edge (deferred), BS, TAB.
  G.Reset();
  G.Feed(std::string(20, 'x') + "y");
  CHECK(G.RowText(0) == std::string(20, 'x') && G.RowText(1) == "y");
  G.Reset();
  G.Feed("abc\bX\tT");
  CHECK(G.RowText(0) == "abX     T");
  // Cursor movement and positioning, erase.
  G.Reset();
  G.Feed("\x1b[3;4HQ\x1b[1;1HP\x1b[2BR\x1b[2D+");
  CHECK(G.At(3, 2).Ch == U'Q' && G.At(0, 0).Ch == U'P');
  G.Reset();
  G.Feed("abcdef\x1b[3D\x1b[K");
  CHECK(G.RowText(0) == "abc");
  G.Feed("\x1b[2J");
  CHECK(G.RowText(0).empty());
  G.Feed("\x1b[HAAA\r\nBBB\x1b[1;2H\x1b[0J");
  CHECK(G.RowText(0) == "A" && G.RowText(1).empty());
  // Colors: 16, bold, 256, truecolor, reset; reverse.
  G.Reset();
  G.Feed("\x1b[31mr\x1b[1;32mg\x1b[0md\x1b[38;5;196mx\x1b[38;2;1;2;3mt\x1b[48;5;21mb\x1b[7mv");
  CHECK(G.At(0, 0).Fg.K == CellColor::Kind::Ansi && G.At(0, 0).Fg.Index == 1);
  CHECK(G.At(1, 0).Fg.Index == 2 && G.At(1, 0).Bold);
  CHECK(G.At(2, 0).Fg.K == CellColor::Kind::Default && !G.At(2, 0).Bold);
  CHECK(G.At(3, 0).Fg.K == CellColor::Kind::Rgb && G.At(3, 0).Fg.R == 255 && G.At(3, 0).Fg.G == 0);
  CHECK(G.At(4, 0).Fg.R == 1 && G.At(4, 0).Fg.G == 2 && G.At(4, 0).Fg.B == 3);
  CHECK(G.At(5, 0).Bg.K == CellColor::Kind::Rgb && G.At(6, 0).Reverse);
  // Scrolling and the back buffer.
  TerminalGrid H(10, 3);
  for (int I = 1; I <= 6; ++I)
    H.Feed("line" + std::to_string(I) + "\r\n");
  CHECK(H.RowText(0) == "line5" || H.RowText(0) == "line4");
  CHECK(H.ScrollbackLines() >= 3);
  const size_t Back = H.ScrollbackLines();
  H.ScrollView(2);
  CHECK(H.ScrollOffset() == 2 && H.ViewAt(0, 0).Ch != U' ');
  H.ScrollView(100);
  CHECK(H.ScrollOffset() == int(Back));
  H.ScrollView(-100);
  CHECK(H.ScrollOffset() == 0);
  H.ScrollView(1);
  H.Feed("more");  // output returns to the live screen
  CHECK(H.ScrollOffset() == 0);
  // The back buffer is a ring of 2,000 lines.
  TerminalGrid Big(10, 2);
  for (int I = 0; I < 2500; ++I)
    Big.Feed("x\r\n");
  CHECK(Big.ScrollbackLines() == TerminalGrid::MaxScrollback);
  // Insert / delete lines, scroll region, reverse index.
  TerminalGrid R(8, 4);
  R.Feed("1\r\n2\r\n3\r\n4\x1b[2;3r\x1b[2;1H\x1b[M");
  CHECK(R.RowText(0) == "1" && R.RowText(1) == "3" && R.RowText(2).empty() && R.RowText(3) == "4");
  R.Feed("\x1b[2;1H\x1bM");
  CHECK(R.RowText(1).empty());
  // UTF-8, titles (BEL and ST terminated), private modes ignored, cursor visibility, alternate screen.
  TerminalGrid U(20, 3);
  U.Feed("caf\xC3\xA9 \xE2\x82\xAC");
  CHECK(U.RowText(0) == "caf\xC3\xA9 \xE2\x82\xAC" && U.At(3, 0).Ch == 0xE9);
  U.Feed("\x1b]0;fex@fex: ~\a");
  CHECK(U.Title() == "fex@fex: ~" && U.TitleChanged() && !U.TitleChanged());
  U.Feed("\x1b]2;vim\x1b\\");
  CHECK(U.Title() == "vim" && U.TitleChanged());
  U.Feed("\x1b[?2004h\x1b[?1h");
  CHECK(U.RowText(0) == "caf\xC3\xA9 \xE2\x82\xAC");
  U.Feed("\x1b[?25l");
  CHECK(!U.CursorVisible());
  U.Feed("\x1b[?25h");
  CHECK(U.CursorVisible());
  U.Feed("\x1b[?1049h");
  CHECK(U.AltScreen() && U.RowText(0).empty());
  U.Feed("alt\x1b[?1049l");
  CHECK(!U.AltScreen() && U.RowText(0) == "caf\xC3\xA9 \xE2\x82\xAC");
  // A split escape sequence across Feed calls works.
  U.Reset();
  U.Feed("\x1b[3");
  U.Feed("1mz");
  CHECK(U.At(0, 0).Fg.Index == 1);
  // Resize keeps the content and the cursor row inside the grid.
  TerminalGrid Rz(10, 6);
  Rz.Feed("a\r\nb\r\nc\r\nd\r\ne\r\nf");
  Rz.Resize(12, 3);
  CHECK(Rz.Cols() == 12 && Rz.Rows() == 3 && Rz.CursorRow() < 3 && Rz.RowText(Rz.CursorRow()) == "f" && Rz.ScrollbackLines() >= 3);
  // The size of the grid for an area.
  const TerminalSize Sz = ComputeTerminalSize(1244, 598, 9.0f, 23.25f);
  CHECK(Sz.Cols == 138 && Sz.Rows == 25);
  CHECK(ComputeTerminalSize(0, 0, 9, 23).Cols == 1 && ComputeTerminalSize(100, 100, 0, 0).Rows == 1);
}

// ---- Tabs ---------------------------------------------------------------------------------------------------------------

void TestTabLayout() {
  using namespace tabs;
  const TabGeometry G = LayoutTabs({60, 100, 40}, 1);
  CHECK(G.Tab.size() == 3 && G.Close.size() == 3);
  // Bottom-aligned on the 1 du border of the 42 du strip: the tabs end at y 41; active 36 high, inactive 34.
  CHECK(G.Tab[1].Y + G.Tab[1].H == 41 && G.Tab[1].H == 36 && G.Tab[0].H == 34 && G.Tab[0].Y + G.Tab[0].H == 41 && G.Tab[2].H == 34);
  // Padding 6, gap 2; the width is 14 + label + 10 + 24 close + 6.
  CHECK(G.Tab[0].X == 6 && G.Tab[0].W == 14 + 60 + 10 + 24 + 6);
  CHECK(G.Tab[1].X == G.Tab[0].X + G.Tab[0].W + 2 && G.Tab[2].X == G.Tab[1].X + G.Tab[1].W + 2);
  // The close button 24x24 sits at the right end, vertically centred, 6 from the edge.
  CHECK(G.Close[1].W == 24 && G.Close[1].H == 24 && G.Close[1].X + 24 + 6 == G.Tab[1].X + G.Tab[1].W && G.Close[1].Y == G.Tab[1].Y + 6);
  // The new-tab button 32x32 is 4 after the last tab.
  CHECK(G.NewTab.W == 32 && G.NewTab.H == 32 && G.NewTab.X == G.Tab[2].X + G.Tab[2].W + 4);
  // Tabs never grow past the maximum; labels are cut.
  CHECK(LayoutTabs({1000}, 0).Tab[0].W == MaxTabW);
  // An empty strip puts the button at the left padding.
  CHECK(LayoutTabs({}, 0).NewTab.X == 6);
  // Hit areas are at least 44x44 even for the 24 du close button and the 32 du button.
  CHECK(HitRect(G.Close[0]).W >= 44 && HitRect(G.Close[0]).H >= 44 && HitRect(G.NewTab).W >= 44);
}

// ---- Shell screen ---------------------------------------------------------------------------------------------------------

struct ShellRig : ScreenRig<ShellScreen> {
  FakeBackend B;
  explicit ShellRig(bool Multi = false, bool Root = false) {
    if (!Multi)
      B.MoreShellsCap = Capability {false, "This launch needs its own guest runtime."};
    Scr = std::make_unique<ShellScreen>(B, Root);
    Hook();
    Step();
  }
  FakeShell& Sh(int I = 0) { return *B.Shells[size_t(I)]; }
};

void TestShellScreen() {
  ShellRig T;
  ShellScreen& S = *T.Scr;
  CHECK(S.TabCount() == 1 && T.B.Shells.size() == 1 && T.Focused() == &S.Terminal());
  // Size at 1280x720: about 138x25 (the terminal area is 1280x626 with padding 14 18).
  CHECK(S.GetTab(0).Grid.Cols() == 138 && S.GetTab(0).Grid.Rows() == 25);
  CHECK(T.Sh().Cols == 138 && T.Sh().Rows == 25);
  // The banner: two lines on open.
  CHECK(Has(S.GetTab(0).Grid.RowText(0), "FEX-Emu 2606") && Has(S.GetTab(0).Grid.RowText(0), "x86-64 guest on ARM64") && Has(S.GetTab(0).Grid.RowText(1), "RootFS Ubuntu_24_04.sqsh mounted at /"));
  CHECK(S.GetTab(0).Grid.At(0, 0).Fg.K == CellColor::Kind::Focus && S.GetTab(0).Grid.At(0, 0).Bold && S.GetTab(0).Grid.At(0, 1).Fg.K == CellColor::Kind::Inactive);
  // Tab label: a default until the shell sets the title (OSC 0/2).
  CHECK(S.TabTitle(0) == "Shell" && S.Strip().Item(0).Title == "Shell");
  T.Sh().Feed("\x1b]0;fex@fex: ~/Games\a$ ");
  T.Step();
  CHECK(S.TabTitle(0) == "fex@fex: ~/Games" && S.Strip().Item(0).Title == "fex@fex: ~/Games" && Has(S.GetTab(0).Grid.RowText(2), "$"));
  // Keys: D-pad = arrows, A = Enter, B = Escape, Y = software keyboard text.
  T.Press(Button::Up);
  T.Press(Button::Down);
  T.Press(Button::Left);
  T.Press(Button::Right);
  T.Press(Button::A);
  T.Press(Button::B);
  CHECK(T.Sh().Written == "\x1b[A\x1b[B\x1b[D\x1b[C\r\x1b");
  T.B.KeyboardAnswers = {"ls -l"};
  T.Press(Button::Y);
  CHECK(Has(T.Sh().Written, "ls -l"));
  T.Sh().Written.clear();
  T.Sh().Feed("\x1b[?1;2004h\x1b[5n");
  T.Step();
  T.Press(Button::Up);
  T.Press(Button::X);
  T.Press(Button::Up);
  T.Press(Button::X);
  T.Press(Button::Down);
  T.Press(Button::X);
  T.Press(Button::Left);
  T.Press(Button::X);
  T.Press(Button::Right);
  T.B.KeyboardAnswers = {"echo test"};
  T.Press(Button::Y);
  Event Hardware;
  Hardware.Kind = Event::Type::KeyDown;
  Hardware.KeyboardKey = Key::Text;
  Hardware.Modifiers = Ctrl;
  Hardware.Codepoint = U'L';
  T.Step({Hardware});
  CHECK(T.Sh().Written == "\x1b[0n\x1bOA\003\032\004\t\x1b[200~echo test\x1b[201~\014");
  CHECK(T.Reqs.empty());
  // Dedicated runtime: the new-tab button is disabled with the backend's reason; L/R do nothing with one tab.
  CHECK(!S.Strip().NewEnabled && !S.NewTab() && T.B.Shells.size() == 1 && T.B.MoreShells().Reason == "This launch needs its own guest runtime.");
  T.Press(Button::R);
  CHECK(S.ActiveTab() == 0);
  // Output scrolls the back buffer; Shift+PgUp/PgDn page through it (ZL/ZR are close/new tab now).
  std::string Lots;
  for (int I = 0; I < 60; ++I)
    Lots += "line " + std::to_string(I) + "\r\n";
  T.Sh().Feed(Lots);
  T.Step();
  CHECK(S.GetTab(0).Grid.ScrollbackLines() > 30);
  Event Page;
  Page.Kind = Event::Type::KeyDown;
  Page.KeyboardKey = Key::PageUp;
  Page.Modifiers = Shift;
  T.Step({Page});
  CHECK(S.GetTab(0).Grid.ScrollOffset() == 23);
  Page.KeyboardKey = Key::PageDown;
  T.Step({Page});
  CHECK(S.GetTab(0).Grid.ScrollOffset() == 0);
  Page.Modifiers = 0;  // plain PgUp/PgDn go to the program as CSI 5 ~ / 6 ~
  T.Sh().Written.clear();
  Page.KeyboardKey = Key::PageUp;
  T.Step({Page});
  Page.KeyboardKey = Key::PageDown;
  T.Step({Page});
  CHECK(T.Sh().Written == "\x1b[5~\x1b[6~");
  // ZR / ZL on a single-tab dedicated runtime: no new tab (with the reason), ZL closes the only tab and goes Home later.
  // - exits to Home at once while only the shell runs.
  T.Press(Button::Minus);
  CHECK(T.Sent(Request::Kind::OpenHome));
}

void TestShellExit() {
  // A running program asks first.
  ShellRig T;
  ShellScreen& S = *T.Scr;
  T.Sh().Child = true;
  T.Press(Button::Minus);
  CHECK(S.Modals().IsOpen() && T.Reqs.empty() && S.Confirm().Title == "Exit to FEX?");
  T.Run(300);
  T.Press(Button::A);  // Cancel
  CHECK(!S.Modals().IsOpen() && T.Reqs.empty() && S.TabCount() == 1);
  T.Run(300);
  T.Press(Button::Minus);
  T.Run(200);
  T.Press(Button::Right);
  T.Press(Button::A);  // Exit
  CHECK(T.Sent(Request::Kind::OpenHome));
  // The shell exiting by itself closes the last tab and goes Home, after its last output was shown.
  ShellRig U;
  U.Sh().Feed("bye\r\n");
  U.Sh().Alive = false;
  U.Step();
  CHECK(U.Sent(Request::Kind::OpenHome));
  // Closing the only tab by its button goes Home (no child running).
  ShellRig V;
  V.Scr->CloseTab(0);
  CHECK(V.Sent(Request::Kind::OpenHome) && V.Scr->TabCount() == 0);
  // A shell that cannot start goes back with the reason.
  FakeBackend B;
  B.ShellStartError = "no RootFS";
  ScreenRig<ShellScreen> W;
  W.Scr = std::make_unique<ShellScreen>(B, false);
  CHECK(W.Scr->TabCount() == 0);
}

void TestShellTabs() {
  ShellRig T(true);
  ShellScreen& S = *T.Scr;
  CHECK(S.Strip().NewEnabled);
  CHECK(S.NewTab(true) && S.TabCount() == 2 && S.ActiveTab() == 1 && S.GetTab(1).Root && S.TabTitle(1) == "Root shell");
  CHECK(S.Strip().Count() == 2 && S.Strip().Item(1).Active && !S.Strip().Item(0).Active);
  T.Press(Button::L);
  CHECK(S.ActiveTab() == 0 && S.Strip().Item(0).Active);
  T.Press(Button::L);  // wraps
  CHECK(S.ActiveTab() == 1);
  T.Press(Button::R);
  CHECK(S.ActiveTab() == 0);
  // ZR opens a new tab, ZL closes the current one (at its prompt at once, asks while a program runs).
  T.Press(Button::ZR);
  CHECK(S.TabCount() == 3 && S.ActiveTab() == 2);
  T.Press(Button::ZL);
  CHECK(S.TabCount() == 2 && S.ActiveTab() == 1);
  T.Press(Button::R);
  CHECK(S.ActiveTab() == 0);
  T.Sh(0).Child = true;
  T.Press(Button::ZL);
  CHECK(S.Modals().IsOpen() && S.TabCount() == 2);
  T.Run(300);
  T.Press(Button::A);  // Cancel
  CHECK(!S.Modals().IsOpen() && S.TabCount() == 2);
  T.Sh(0).Child = false;
  T.Run(300);
  // The footer shows the map as button discs, no arrow characters in text; X swaps it for the control keys.
  {
    int Discs = 0;
    bool Arrow = false;
    for (const auto& G : S.Hints().Segs) {
      Discs += G.K == HintBar::Seg::Kind::Key;
      Arrow |= G.K != HintBar::Seg::Kind::Key && G.Text.find("\xE2\x86") != std::string::npos;
    }
    CHECK(Discs == 3 && !Arrow && S.Hints().Segs[0].Text == "ZR" && S.Hints().HintCount() == 3);
    // X arms the control keys: the overlay (not the footer) shows them; the footer keeps its map.
    CHECK(!S.ControlKeysArmed());
    T.Press(Button::X);
    CHECK(S.ControlKeysArmed() && S.Hints().Segs.size() == 8 && S.Hints().Segs[0].Text == "ZR");
    T.Run(50);
    T.Press(Button::X);
    CHECK(!S.ControlKeysArmed());
  }
  // R3 (right stick click) opens the list of all bindings; R3 again or B closes it. Keys do not reach the shell meanwhile.
  {
    T.Sh(0).Written.clear();
    CHECK(!S.Modals().IsOpen());
    T.Press(Button::StickR);
    CHECK(S.Modals().IsOpen() && S.Modals().Top() == &S.Help() && S.Help().Rows.size() >= 10);
    T.Run(300);
    T.Press(Button::Up);
    T.Press(Button::ZL);  // no tab closes behind the list
    CHECK(S.TabCount() == 2 && T.Sh(0).Written.empty());
    T.Press(Button::StickR);
    CHECK(!S.Modals().IsOpen());
    T.Run(300);
    T.Press(Button::StickR);
    CHECK(S.Modals().IsOpen());
    T.Run(300);
    T.Press(Button::B);
    CHECK(!S.Modals().IsOpen() && T.Sh(0).Written.empty());
    T.Run(300);
    S.Hints().OnHint(Button::StickR);  // the footer's Help hint opens it too (tap)
    CHECK(S.Modals().IsOpen());
    T.Press(Button::StickR);
    CHECK(!S.Modals().IsOpen());
    T.Run(300);
  }
  // Keys go to the active tab only.
  T.Press(Button::A);
  CHECK(T.Sh(0).Written == "\r" && T.Sh(1).Written.empty());
  // Tab strip widgets: a tap on a tab selects it, on its close button closes it.
  S.Strip().Item(1).OnActivate();
  CHECK(S.ActiveTab() == 1);
  S.Strip().Item(1).Close.OnActivate();
  CHECK(S.TabCount() == 1 && S.ActiveTab() == 0 && T.Reqs.empty());
  // Closing a tab with a running program asks first.
  S.NewTab(false);
  T.B.Shells.back()->Child = true;
  S.CloseTab(1);
  CHECK(S.Modals().IsOpen() && S.TabCount() == 2);
  T.Run(300);
  T.Press(Button::A);  // Cancel
  CHECK(S.TabCount() == 2);
  T.Run(300);
  S.CloseTab(1);
  T.Run(200);
  T.Press(Button::Right);
  T.Press(Button::A);
  CHECK(S.TabCount() == 1 && T.Reqs.empty());
  // The size at other outputs: the same area at 1080p still holds about 138 columns.
  {
    FakeDisplay D {Size {1920, 1080}};
    FakeRenderer R {D};
    Fonts F {R};
    Icons I {R};
    DrawContext C {R, F, I, CurrentTheme(), 1.5f};
    const Frame Fr {C, 0, 0};
    const TerminalSize Sz = S.Terminal().FitSize(Fr);
    CHECK(Sz.Cols >= 136 && Sz.Cols <= 140 && Sz.Rows >= 24 && Sz.Rows <= 26);
  }
}

} // namespace

int main() {
  SetLogSink([](const char*) {});
  TestFtpValidation();
  TestFtpSettings();
  TestFtpRates();
  TestFtpScreen();
  TestFtpClients();
  TestInstallModel();
  TestInstallScreen();
  TestRootfsSetup();
  TestRootfsReinstall();
  TestTerminalGrid();
  TestTabLayout();
  TestShellScreen();
  TestShellExit();
  TestShellTabs();
  std::printf("ui-task6-test: %d checks, %d failures\n", Checks, Failures);
  return Failures ? 1 : 0;
}
