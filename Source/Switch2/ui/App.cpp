// SPDX-License-Identifier: MIT
#include "App.h"

#include <unistd.h>

#include <algorithm>
#include <random>

#include "LicenseStore.h"
#include "RootfsState.h"
#include "platform/Log.h"
#include "screens/filepicker.h"
#include "screens/ftp.h"
#include "screens/home.h"
#include "screens/install.h"
#include "screens/rootfs_setup.h"
#include "screens/settings.h"
#include "screens/shell.h"

namespace ui {

const char* ScreenKindName(ScreenKind K) {
  switch (K) {
  case ScreenKind::Home: return "home";
  case ScreenKind::Settings: return "settings";
  case ScreenKind::Ftp: return "ftp";
  case ScreenKind::RootfsSetup: return "rootfs";
  case ScreenKind::Install: return "install";
  case ScreenKind::Shell: return "shell";
  case ScreenKind::Picker: return "filepicker";
  }
  return "?";
}

App::App(Platform& Pl, LauncherBackend& B, std::function<uint32_t()> RandFn) : P(Pl), Be(B), R(Pl.GetRenderer()), F(Pl.GetRenderer()), I(Pl.GetRenderer()), Rand(std::move(RandFn)) {
  FontsOk = F.Ok();
  if (!Rand) {
    auto Dev = std::make_shared<std::random_device>();
    Rand = [Dev]() { return uint32_t((*Dev)()); };
  }
  Clock = [this]() { return P.GetSystem().MonotonicMs(); };
  std::string Warning;
  if (!Model.Load(Be.GetStorage(), Paths, Warning))
    Log("settings: %s", Warning.c_str());
  ApplySettings();
  // The license texts go to the card the first time (the NRO is the only file the user copies).
  int Written = 0;
  std::string Error;
  if (!EnsureLicensesWritten(Be.GetStorage(), LicensesDir, Written, Error))
    Log("licenses: %s", Error.c_str());
  else if (Written)
    Log("licenses: wrote %d files to %s", Written, LicensesDir);
  Be.SetFrameHook([this]() {
    if (InHook) return;
    InHook = true;
    PumpFrame();
    InHook = false;
  });
  // Start: Home when a RootFS is installed, RootFS setup (first run) when not.
  const uint64_t Now = Clock();
  if (Be.Rootfs().Installed)
    Push(std::make_unique<HomeScreen>(Be), ScreenKind::Home, Now);
  else
    Push(std::make_unique<RootfsSetupScreen>(Be, false, Paths), ScreenKind::RootfsSetup, Now);
  FadeOut = nullptr;  // no fade into the first screen
}

App::~App() { Be.SetFrameHook({}); }

void App::ApplySettings() {
  for (const SettingDef& D : Model.Schema())
    if (D.Store == StoreKind::App)
      Be.ApplySetting(D.Key, Model.Get(D.Key));
  SetTheme(ResolveThemeMode(Model, ThemeMode::Dark), ResolveAccent(Model));
}

void App::Wire(Screen& S) {
  S.OnRequest = [this](const Request& Rq) { Queue.push_back(Rq); };
}

void App::BeginFade(Screen* Out, uint64_t Now) {
  FadeOut = Out;
  FadeStart = Now;
}

void App::Push(std::unique_ptr<Screen> S, ScreenKind K, uint64_t Now) {
  Screen* Old = Stack.empty() ? nullptr : Stack.back().S.get();
  Wire(*S);
  Stack.push_back({std::move(S), K});
  BeginFade(Old, Now);
  Retired.reset();
}

void App::Pop(uint64_t Now) {
  if (Stack.size() < 2)
    return;
  const ScreenKind Was = Stack.back().K;
  Retired = std::move(Stack.back().S);
  Stack.pop_back();
  BeginFade(Retired.get(), Now);
  if (Was == ScreenKind::Settings)
    ApplySettings();
  if (Stack.back().K == ScreenKind::Home)
    static_cast<HomeScreen&>(*Stack.back().S).Refresh();
}

void App::StartFirstRun(uint64_t Now) {
  Retired = Stack.empty() ? nullptr : std::move(Stack.back().S);
  Stack.clear();
  auto S = std::make_unique<RootfsSetupScreen>(Be, false, Paths);
  Wire(*S);
  Stack.push_back({std::move(S), ScreenKind::RootfsSetup});
  BeginFade(Retired.get(), Now);
}

void App::GoHome(uint64_t Now) {
  if (!Stack.empty() && Stack.front().K == ScreenKind::Home) {
    if (Stack.size() > 1) {
      Retired = std::move(Stack.back().S);
      Stack.pop_back();
      BeginFade(Retired.get(), Now);
      Stack.resize(1);
    }
    static_cast<HomeScreen&>(*Stack.front().S).Refresh();
    return;
  }
  // First run (or a RootFS that was wiped and installed again): Home does not exist yet.
  Retired = Stack.empty() ? nullptr : std::move(Stack.back().S);
  Stack.clear();
  auto H = std::make_unique<HomeScreen>(Be);
  Wire(*H);
  Stack.push_back({std::move(H), ScreenKind::Home});
  BeginFade(Retired.get(), Now);
}

void App::Handle(const Request& Rq, uint64_t Now) {
  using K = Request::Kind;
  switch (Rq.K) {
  case K::Back: Pop(Now); break;
  case K::OpenSettings: Push(std::make_unique<SettingsScreen>(Be, Model, Paths), ScreenKind::Settings, Now); break;
  case K::OpenAbout: break;  // About is a pane of Settings
  case K::OpenFtp: Push(std::make_unique<FtpScreen>(Be, Paths, Rand), ScreenKind::Ftp, Now); break;
  case K::LaunchShell:
  case K::LaunchSteam:
  case K::LaunchDesktop:
    if (Rq.K == K::LaunchShell) {
      auto Sh = std::make_unique<ShellScreen>(Be, false);
      if (Sh->TabCount() == 0) {
        Log("shell: could not start");
        break;
      }
      Push(std::move(Sh), ScreenKind::Shell, Now);
    } else if (ExternalSession) {
      ExternalSession(Rq.K == K::LaunchSteam ? "steam" : "desktop");
    }
    break;
  case K::OpenRootfsSetup: Push(std::make_unique<RootfsSetupScreen>(Be, Rq.Reinstall, Paths), ScreenKind::RootfsSetup, Now); break;
  case K::OpenFilePicker: Push(std::make_unique<FilePickerScreen>(Be, Rq.Picker, Rq.OnPicked), ScreenKind::Picker, Now); break;
  case K::Exit: Quit = true; break;
  case K::OpenHome: GoHome(Now); break;
  case K::StartFirstRun: StartFirstRun(Now); break;
  case K::StartInstall: Push(std::make_unique<InstallScreen>(Be, Rq.Install), ScreenKind::Install, Now); break;
  case K::DevAction: {
    const auto Existing = std::find_if(Stack.begin(), Stack.end(), [](const Entry& E) { return E.K == ScreenKind::Shell; });
    if (Existing != Stack.end()) {
      auto& Shell = static_cast<ShellScreen&>(*Existing->S);
      const bool Root = Rq.Action == "root_shell";
      std::string Error;
      auto Program = Root ? nullptr : Be.StartDevSession(Rq.Action, Rq.Path, Error);
      const bool Opened = (Root || Program) && Shell.NewTab(Root, std::move(Program));
      if (!Opened && !Error.empty()) Shell.ShowStatus(Error);
      const size_t Depth = size_t(Existing - Stack.begin()) + 1;
      while (Stack.size() > Depth) Pop(Now);
      break;
    }
    if (Rq.Action == "root_shell") {
      auto Sh = std::make_unique<ShellScreen>(Be, true);
      if (Sh->TabCount() == 0)
        break;
      Push(std::move(Sh), ScreenKind::Shell, Now);
      break;
    }
    std::string Error;
    std::unique_ptr<ShellSession> S = Be.StartDevSession(Rq.Action, Rq.Path, Error);
    if (!S) {
      Log("developer action %s: %s", Rq.Action.c_str(), Error.c_str());
      break;
    }
    auto Sh = std::make_unique<ShellScreen>(Be, false, std::move(S));
    Push(std::move(Sh), ScreenKind::Shell, Now);
    break;
  }
  }
}

void App::DrawScreen(Screen& S, const DrawContext& C, uint64_t Now) {
  // Every screen starts on the page background; the cross-fade blends it too.
  const Size Sz = R.TargetSize();
  R.FillRect(Rect {0, 0, float(Sz.W), float(Sz.H)}, C.T.Bg);
  S.Draw(C, Now);
}

bool App::DoFrame(bool Nested) {
  (void)Nested;
  if (!P.GetSystem().KeepRunning()) {
    Quit = true;
    return false;
  }
  Be.Tick();
  const uint64_t Now = Clock();
  LastNow_ = Now;
  Display& D = P.GetDisplay();
  const Theme& T0 = CurrentTheme();
  // Display first: it applies a pending dock/undock size, so the scale this frame is the new one.
  if (!D.BeginFrame() || !R.BeginFrame(T0.Bg)) {
    if (++FailedFrames >= 3000) {
      Log("FAIL 3000 consecutive frames could not begin");
      Quit = true;
      return false;
    }
    usleep(2000);
    return true;
  }
  FailedFrames = 0;
  Events.clear();
  P.GetInput().Poll(Events);
  Screen* Before = Stack.back().S.get();
  for (auto& Entry : Stack) {
    if (Entry.K == ScreenKind::Shell && Entry.S.get() != Before)
      Entry.S->Update(Now, R.Scale(), {});
  }
  Stack.back().S->Update(Now, R.Scale(), Events);
  // Requests act after the update returned (a screen may be replaced by its own request).
  while (!Queue.empty()) {
    std::vector<Request> Todo;
    Todo.swap(Queue);
    for (const Request& Rq : Todo)
      Handle(Rq, Now);
  }
  if (Stack.back().S.get() != Before)
    Stack.back().S->Update(Now, R.Scale(), {});  // the new top gets its first update before its first draw
  for (auto& Entry : Stack) {
    if (Entry.K == ScreenKind::Shell)
      static_cast<ShellScreen&>(*Entry.S).SetForeground(&Entry == &Stack.back());
  }
  const Theme& T = CurrentTheme();
  DrawContext C {R, F, I, T, R.Scale()};
  if (FadeOut && Now >= FadeStart + FadeMs) {
    FadeOut = nullptr;
    Retired.reset();
  }
  if (FadeOut) {
    DrawScreen(*FadeOut, C, Now);
    R.SetOpacity(std::clamp(float(Now - FadeStart) / float(FadeMs), 0.0f, 1.0f));
    DrawScreen(*Stack.back().S, C, Now);
    R.SetOpacity(1.0f);
  } else {
    DrawScreen(*Stack.back().S, C, Now);
  }
  F.EndFrame();
  I.EndFrame();
  const bool Ended = R.EndFrame();
  const bool Shown = Ended && D.Present();
  if (Shown)
    ++Drawn;
  return !Quit;
}

// A frame from inside a long call (the installer, a guest session). Cheap calls in between are skipped: one frame per vsync is all
// the display can show, and the caller's own work must not wait for every call.
bool App::PumpFrame() {
  const uint64_t Now = Clock();
  if (LastPump && Now >= LastPump && Now - LastPump < 12)
    return !Quit;
  LastPump = Now;
  return DoFrame(true);
}

void App::RunDeferred() {
  if (InRun)
    return;
  InRun = true;
  // The Installation screen runs the installer as soon as it is on top (and again after Retry).
  if (Stack.back().K == ScreenKind::Install) {
    auto& Ins = static_cast<InstallScreen&>(*Stack.back().S);
    if (Ins.NeedsRun()) {
      Ins.PumpHook = [this, &Ins]() {
        if (!PumpFrame())
          Ins.Abort();
      };
      Ins.Run();
      Ins.PumpHook = nullptr;
    }
  }
  InRun = false;
}

bool App::Frame() {
  const bool Live = DoFrame(false);
  if (Live)
    RunDeferred();
  return Live && !Quit;
}

int App::Run() {
  while (Frame()) {
  }
  return 0;
}

} // namespace ui
