// SPDX-License-Identifier: MIT
// Host tests for the app shell (make check-ui): the flow of the spec's navigation diagram on FakeBackend (first run -> RootFS setup ->
// Installation -> Home -> Settings -> back with the focus restored), the launcher reaching RootFS setup with an empty SD card
// (real files in a temp folder, no marker), the cross-fade, dock/undock inside one frame, the cooperative runs (install, retry, shell,
// developer program), settings applied to the backend, and the console closing the app. Fake platform, fake clock: no GPU.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "App.h"
#include "RootfsState.h"
#include "backend/FakeBackend.h"
#include "platform/Log.h"
#include "platform/fake/FakePlatform.h"
#include "screens/filepicker.h"
#include "screens/home.h"
#include "screens/install.h"
#include "screens/settings.h"
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

// A fake clock that moves 16 ms every time the app reads it (one read per frame or pump).
struct Rig {
  FakePlatform P;
  uint64_t T = 1000;
  explicit Rig(Size S = Size {1280, 720}) : P(S) { SetTheme(ThemeMode::Dark, AccentPreset::Cyan); }
  std::function<uint64_t()> Clock() {
    return [this]() { return T += 16; };
  }
};

struct Session {
  Rig R;
  FakeBackend B;
  std::unique_ptr<App> A;
  explicit Session(bool Installed = true, Size S = Size {1280, 720}) : R(S) {
    B.Root.Installed = Installed;
    A = std::make_unique<App>(R.P, B);
    A->SetClock(R.Clock());
  }
  bool Frames(int N) {
    bool Live = true;
    for (int I = 0; I < N && Live; ++I)
      Live = A->Frame();
    return Live;
  }
  void Press(Button Btn_) {
    R.P.In.Push(Btn(Btn_, true));
    Frames(1);
    R.P.In.Push(Btn(Btn_, false));
    Frames(1);
    Frames(12);  // the fade (150 ms = 10 frames at 16 ms) and Home's 80 ms card delay
  }
  int Focus() { return A->Top().Input().FocusedId(); }
};

bool Has(const std::vector<std::string>& Calls, const std::string& Needle) {
  for (const std::string& C : Calls)
    if (C.find(Needle) != std::string::npos)
      return true;
  return false;
}

// ---- first run: setup -> install -> Home -> Settings -> back, focus restored ---------------------------------------------------

void TestFirstRunFlow() {
  Session S(false);
  CHECK(S.A->Ok());
  CHECK(S.A->Depth() == 1 && S.A->TopKind() == ScreenKind::RootfsSetup);
  CHECK(!S.A->Fading(S.A->LastNow()));  // no fade into the first screen
  S.B.PumpsPerStep = 3;
  S.Frames(3);
  S.Press(Button::Plus);  // + Install (first run: no confirmation)
  CHECK(S.B.InstallCalls == 1);
  CHECK(S.A->Depth() == 2 && S.A->TopKind() == ScreenKind::Install);
  auto& Ins = static_cast<InstallScreen&>(S.A->Top());
  CHECK(Ins.GetState() == InstallScreen::State::Done);
  CHECK(!Ins.NeedsRun());
  S.Press(Button::A);  // Continue
  CHECK(S.A->Depth() == 1 && S.A->TopKind() == ScreenKind::Home);  // the setup and install screens are gone
  CHECK(S.B.InstallCalls == 1);

  auto* Home = &S.A->Top();
  const int HomeFocus = S.Focus();
  CHECK(HomeFocus > 0);
  S.Press(Button::Plus);  // Settings
  CHECK(S.A->Depth() == 2 && S.A->TopKind() == ScreenKind::Settings);
  CHECK(S.A->KindAt(0) == ScreenKind::Home && &S.A->At(0) == Home);
  S.Press(Button::B);  // Back: the settings are saved and Home returns as it was
  CHECK(S.A->Depth() == 1 && S.A->TopKind() == ScreenKind::Home);
  CHECK(&S.A->Top() == Home);
  CHECK(S.Focus() == HomeFocus);
}

// Settings > Wipe RootFS: the Installation screen runs the wipe; Continue closes every screen and opens first-run RootFS setup.
void TestWipeFlow() {
  Session S(true);
  S.B.PumpsPerStep = 3;
  S.Press(Button::Plus);  // Settings
  CHECK(S.A->Depth() == 2 && S.A->TopKind() == ScreenKind::Settings);
  Request R;
  R.K = Request::Kind::StartInstall;
  R.Install.WipeOnly = true;
  S.A->Post(R);  // what the confirmed wipe sends
  S.Frames(12);
  CHECK(S.A->Depth() == 3 && S.A->TopKind() == ScreenKind::Install);
  CHECK(S.B.WipeCalls == 1 && S.B.InstallCalls == 0 && !S.B.Root.Installed);
  CHECK(static_cast<InstallScreen&>(S.A->Top()).GetState() == InstallScreen::State::Done);
  S.Press(Button::A);  // Continue
  CHECK(!S.A->QuitRequested() && S.A->Depth() == 1 && S.A->TopKind() == ScreenKind::RootfsSetup);
  S.Press(Button::B);  // first-run setup: B leaves the app
  CHECK(S.A->QuitRequested());
}

void TestInstallRetry() {
  Session S(false);
  S.B.PumpsPerStep = 2;
  S.B.InstallFailsAtStep = 3;
  S.Press(Button::Plus);
  auto& Ins = static_cast<InstallScreen&>(S.A->Top());
  CHECK(S.B.InstallCalls == 1 && Ins.GetState() == InstallScreen::State::Failed);
  S.B.InstallFailsAtStep = -1;
  S.Press(Button::A);  // Retry: the app runs the installer again
  CHECK(S.B.InstallCalls == 2);
  CHECK(static_cast<InstallScreen&>(S.A->Top()).GetState() == InstallScreen::State::Done);
}

// ---- the launcher with an empty SD card ------------------------------------------------------------------------------------------

class CardBackend final : public FakeBackend {
public:
  explicit CardBackend(const std::string& Root) : Card(Root, Root + "/fex/rootfs") {}
  Storage& GetStorage() override { return Card; }
  RootfsInfo Rootfs() override { return DetectRootfs(Card); }
  PosixStorage Card;
};

std::string MakeTemp(const char* Tag) {
  std::string Dir = std::string("/tmp/fex-ui-app-") + Tag + "-XXXXXX";
  std::vector<char> Buf(Dir.begin(), Dir.end());
  Buf.push_back(0);
  return mkdtemp(Buf.data()) ? std::string(Buf.data()) : std::string();
}

bool Exists(const std::string& P) {
  struct stat St;
  return stat(P.c_str(), &St) == 0;
}

void TestEmptyCard() {
  const std::string Dir = MakeTemp("empty");
  CHECK(!Dir.empty());
  {
    Rig R;
    CardBackend B(Dir);
    CHECK(!DetectRootfs(B.Card).Installed);  // nothing on the card
    App A(R.P, B);
    A.SetClock(R.Clock());
    for (int I = 0; I < 5; ++I)
      CHECK(A.Frame());
    CHECK(A.Depth() == 1 && A.TopKind() == ScreenKind::RootfsSetup);  // first run: RootFS setup, nothing else
    CHECK(Exists(Dir + "/fex/licenses/licenses.json"));            // the licenses were written out at start
    CHECK(!Exists(Dir + "/fex/config/ui.json"));                   // no settings written without a change
    CHECK(A.FramesDrawn() >= 5);
  }
  // The same card with the installer's marker: Home.
  mkdir((Dir + "/fex/config").c_str(), 0777);
  FILE* F = std::fopen((Dir + "/fex/config/rootfs.json").c_str(), "w");
  std::fputs("{\"installed\": true, \"name\": \"Ubuntu 24.04\"}\n", F);
  std::fclose(F);
  {
    Rig R;
    CardBackend B(Dir);
    CHECK(DetectRootfs(B.Card).Installed && DetectRootfs(B.Card).Name == "Ubuntu 24.04");
    App A(R.P, B);
    CHECK(A.TopKind() == ScreenKind::Home);
  }
  // A marker that says not installed, and a developer bundle tree without a marker.
  F = std::fopen((Dir + "/fex/config/rootfs.json").c_str(), "w");
  std::fputs("{\"installed\": false}\n", F);
  std::fclose(F);
  {
    CardBackend B(Dir);
    CHECK(!DetectRootfs(B.Card).Installed);
  }
  std::remove((Dir + "/fex/config/rootfs.json").c_str());
  mkdir((Dir + "/fex/rootfs").c_str(), 0777);
  mkdir((Dir + "/fex/rootfs/bin").c_str(), 0777);
  {
    CardBackend B(Dir);
    CHECK(DetectRootfs(B.Card).Installed);
  }
  std::string Cmd = "rm -rf '" + Dir + "'";
  CHECK(std::system(Cmd.c_str()) == 0);
}

// ---- cross-fade, dock/undock ----------------------------------------------------------------------------------------------------

void TestFadeAndRescale() {
  Session S(true);
  S.Frames(2);
  auto& Rend = S.R.P.Rend;
  const auto Focus0 = S.Focus();
  S.R.P.In.Push(Btn(Button::Plus, true));
  S.Frames(1);
  S.R.P.In.Push(Btn(Button::Plus, false));
  S.Frames(1);
  CHECK(S.A->TopKind() == ScreenKind::Settings);
  CHECK(S.A->Fading(S.A->LastNow()));
  Rend.Calls.clear();
  S.Frames(1);
  CHECK(Has(Rend.Calls, " op="));  // the new screen is drawn over the old one with a rising opacity
  S.Frames(12);
  CHECK(!S.A->Fading(S.A->LastNow()));
  Rend.Calls.clear();
  S.Frames(1);
  CHECK(!Has(Rend.Calls, " op="));
  (void)Focus0;

  // Dock: the next frame already draws at 1920x1080 and keeps the state of the screen (focus, scroll).
  auto& Set = static_cast<SettingsScreen&>(S.A->Top());
  Set.Scrolling().To(200, S.A->LastNow(), 0);
  S.Frames(2);
  const int Focus = S.Focus();
  const float Scroll = Set.Scrolling().Target();
  S.R.P.Disp.SetSize(Size {1920, 1080});
  Rend.Calls.clear();
  S.Frames(1);
  CHECK(S.R.P.Rend.Scale() == 1.5f);
  CHECK(Has(Rend.Calls, "FillRect 0.000000 0.000000 1920.000000 1080.000000"));  // the very first frame after the change is at the new size
  CHECK(S.Focus() == Focus);
  CHECK(Set.Scrolling().Target() == Scroll);
  S.R.P.Disp.SetSize(Size {3840, 2160});
  S.Frames(1);
  CHECK(S.R.P.Rend.Scale() == 3.0f);
  S.R.P.Disp.SetSize(Size {1280, 720});
  S.Frames(1);
  CHECK(S.R.P.Rend.Scale() == 1.0f && S.Focus() == Focus && Set.Scrolling().Target() == Scroll);
}

// ---- shell, developer program, settings applied -----------------------------------------------------------------------------------

void TestShellAndDev() {
  Session S(true);
  CHECK(S.B.Applied.count("log_level") && S.B.Applied["log_level"] == "standard");  // applied once at start
  CHECK(S.B.Applied.count("start_folder") && S.B.Applied["start_folder"] == "/fex/games");
  S.Frames(2);
  S.Press(Button::A);  // the Shell card (first enabled card)
  CHECK(S.A->TopKind() == ScreenKind::Shell);
  auto& Tabs = static_cast<ShellScreen&>(S.A->Top());
  CHECK(Tabs.NewTab() && Tabs.NewTab());
  CHECK(Tabs.TabCount() == 3);
  const int Ticks = S.B.Ticks;
  S.Frames(4);
  CHECK(S.B.Ticks == Ticks + 4 && Tabs.TabCount() == 3);
  CHECK(!S.B.Shells[0]->Foreground && !S.B.Shells[1]->Foreground && S.B.Shells[2]->Foreground);
  S.Press(Button::L);
  CHECK(!S.B.Shells[0]->Foreground && S.B.Shells[1]->Foreground && !S.B.Shells[2]->Foreground);
  S.Press(Button::R);
  CHECK(!S.B.Shells[0]->Foreground && !S.B.Shells[1]->Foreground && S.B.Shells[2]->Foreground);
  S.Press(Button::Plus);
  CHECK(S.A->TopKind() == ScreenKind::Settings);
  CHECK(!S.B.Shells[0]->Foreground && !S.B.Shells[1]->Foreground && !S.B.Shells[2]->Foreground);
  S.Press(Button::B);
  CHECK(S.A->TopKind() == ScreenKind::Shell && S.B.Shells[2]->Foreground);
  S.Press(Button::A);
  CHECK(S.B.Shells[0]->Written.empty() && S.B.Shells[1]->Written.empty() && S.B.Shells[2]->Written == "\r");
  S.B.Shells[1]->Feed("\x1b]2;Second\aonly second\x1b[5n");
  S.Frames(1);
  CHECK(Tabs.TabTitle(0) == "Shell" && Tabs.TabTitle(1) == "Second" && Tabs.TabTitle(2) == "Shell");
  CHECK(S.B.Shells[0]->Written.empty() && S.B.Shells[1]->Written == "\x1b[0n" && S.B.Shells[2]->Written == "\r");
  CHECK(Tabs.GetTab(0).Grid.RowText(2).find("only second") == std::string::npos &&
        Tabs.GetTab(1).Grid.RowText(2).find("only second") != std::string::npos &&
        Tabs.GetTab(2).Grid.RowText(2).find("only second") == std::string::npos);
  for (int I = 0; I < 3; ++I)
    CHECK(S.B.Shells[I]->Cols == Tabs.GetTab(I).Grid.Cols() && S.B.Shells[I]->Rows == Tabs.GetTab(I).Grid.Rows());
  Tabs.CloseTab(1);
  S.Frames(1);
  CHECK(Tabs.TabCount() == 2 && S.A->TopKind() == ScreenKind::Shell);
  Tabs.CloseTab(1);
  Tabs.CloseTab(0);
  S.Frames(1);
  CHECK(S.A->TopKind() == ScreenKind::Home && S.A->Depth() == 1);

  // A developer program: its tab stays after the program ended and shows the exit note.
  Request Rq;
  Rq.K = Request::Kind::DevAction;
  Rq.Action = "run_elf";
  Rq.Path = "/bin/comm-acceptance.elf";
  S.Press(Button::A);
  CHECK(S.A->TopKind() == ScreenKind::Shell);
  S.Press(Button::Plus);
  CHECK(S.A->TopKind() == ScreenKind::Settings);
  S.A->Post(Rq);
  S.Frames(30);
  CHECK(S.B.DevStarted.size() == 1 && S.B.DevStarted[0].first == "run_elf" && S.B.DevStarted[0].second == "/bin/comm-acceptance.elf");
  CHECK(S.A->TopKind() == ScreenKind::Shell);
  auto& Sh = static_cast<ShellScreen&>(S.A->Top());
  CHECK(Sh.TabCount() == 2);
  CHECK(Sh.TabTitle(0) == "Shell" && Sh.TabTitle(1) == "run_elf");
  bool Note = false;
  for (int Row = 0; Row < Sh.GetTab(1).Grid.Rows(); ++Row)
    Note |= Sh.GetTab(1).Grid.RowText(Row).find("Exited with status 42") != std::string::npos;
  CHECK(Note);
  S.Press(Button::Minus);  // - exits to Home (nothing runs any more: no confirmation)
  CHECK(S.A->TopKind() == ScreenKind::Home && S.A->Depth() == 1);

  // An unavailable developer action starts nothing.
  S.B.DevCaps["unavailable_action"] = Capability {false, "Not available"};
  Request Bad;
  Bad.K = Request::Kind::DevAction;
  Bad.Action = "unavailable_action";
  S.A->Post(Bad);
  S.Frames(5);
  CHECK(S.B.DevStarted.size() == 1 && S.A->TopKind() == ScreenKind::Home);

  // Root shell.
  Request Root;
  Root.K = Request::Kind::DevAction;
  Root.Action = "root_shell";
  S.A->Post(Root);
  S.Frames(30);
  CHECK(S.A->TopKind() == ScreenKind::Shell);
  S.B.LastShell->Alive = false;
  S.Frames(2);
  CHECK(S.A->TopKind() == ScreenKind::Home);
}

void TestSettingsApplied() {
  Session S(true);
  S.Frames(2);
  S.A->Settings().Set("log_level", "trace");
  S.Press(Button::Plus);
  CHECK(S.A->TopKind() == ScreenKind::Settings);
  S.Press(Button::B);
  CHECK(S.A->TopKind() == ScreenKind::Home);
  CHECK(S.B.Applied["log_level"] == "trace");
  // The picker for a developer program: the choice reaches the app as a DevAction once Settings is on top again.
  S.Press(Button::Plus);
  auto& Set = static_cast<SettingsScreen&>(S.A->Top());
  CHECK(Set.RowFor("run_elf") != nullptr && Set.RowFor("root_shell") != nullptr && Set.RowFor("debian_setup") == nullptr &&
        Set.RowFor("embedded_guest") == nullptr && Set.RowFor("jit_probe") == nullptr);
  Set.Input().SetFocus(Set.RowFor("run_elf")->Id, S.A->LastNow());
  // The row is an action pill: A on its control opens the picker.
  S.R.P.In.Push(Btn(Button::A, true));
  S.Frames(1);
  S.R.P.In.Push(Btn(Button::A, false));
  S.Frames(14);
  CHECK(S.A->TopKind() == ScreenKind::Picker && S.A->Depth() == 3);
  auto& Pick = static_cast<FilePickerScreen&>(S.A->Top());
  CHECK(Pick.Spec().Title == "Choose an ELF file" && !Pick.Spec().Folder);
  CHECK(Pick.Spec().Current == "sdmc:/fex/games");  // Run ELF opens in the folder of the test programs
  Pick.Finish("sdmc:/fex/games/probe.elf");
  S.Frames(30);
  CHECK(S.B.DevStarted.size() == 1 && S.B.DevStarted[0].first == "run_elf" && S.B.DevStarted[0].second == "sdmc:/fex/games/probe.elf");
  CHECK(S.A->TopKind() == ScreenKind::Shell);
}

// Final Pass Suite: listed only while sdmc:/fex/games/final-pass-suite.elf exists; Run starts it without a picker.
void TestFinalPassRow() {
  Session S(true);
  S.Frames(2);
  S.Press(Button::Plus);
  CHECK(S.A->TopKind() == ScreenKind::Settings);
  CHECK(static_cast<SettingsScreen&>(S.A->Top()).RowFor("final_pass") == nullptr);
  S.Press(Button::B);
  S.B.Fs().AddFile("sdmc:/fex/games/final-pass-suite.elf", 100);
  S.Press(Button::Plus);
  auto& Set = static_cast<SettingsScreen&>(S.A->Top());
  CHECK(Set.RowFor("final_pass") != nullptr);
  Set.Input().SetFocus(Set.RowFor("final_pass")->Id, S.A->LastNow());
  S.R.P.In.Push(Btn(Button::A, true));
  S.Frames(1);
  S.R.P.In.Push(Btn(Button::A, false));
  S.Frames(30);
  CHECK(S.B.DevStarted.size() == 1 && S.B.DevStarted[0].first == "final_pass" && S.B.DevStarted[0].second.empty());
  CHECK(S.A->TopKind() == ScreenKind::Shell);
}

void TestQuit() {
  {
    Session S(true);
    S.Frames(2);
    Request Rq;
    Rq.K = Request::Kind::Exit;
    S.A->Post(Rq);
    CHECK(!S.Frames(3));  // Exit confirmed: the loop ends
    CHECK(S.A->QuitRequested());
  }
  {
    Session S(true);
    S.Frames(2);
    S.R.P.Sys.Running = false;  // the console closes the app
    CHECK(!S.A->Frame());
  }
  {
    Session S(true);
    S.Frames(2);
    CHECK(S.B.Ticks >= 2);  // the backend ticks once per frame
  }
}

} // namespace

int main() {
  SetLogSink([](const char*) {});
  TestFirstRunFlow();
  TestWipeFlow();
  TestInstallRetry();
  TestEmptyCard();
  TestFadeAndRescale();
  TestShellAndDev();
  TestSettingsApplied();
  TestFinalPassRow();
  TestQuit();
  std::printf("ui-app-test: %d checks, %d failures\n", Checks, Failures);
  return Failures ? 1 : 0;
}
