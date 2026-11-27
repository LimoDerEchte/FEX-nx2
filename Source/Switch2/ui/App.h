// SPDX-License-Identifier: MIT
// The app shell: owns the platform-independent parts of one launcher run. The screen stack and the flow of the spec's navigation
// diagram (first run -> RootFS setup -> Installation -> Home; Home <-> Settings, FTP, Shell; file pickers on top), the 150 ms
// cross-fade between screens (the new screen is drawn over the old one with a rising opacity), per-screen state kept while a screen
// stays on the stack, dock/undock rescale (the display applies the new size at the start of a frame, every screen draws in design
// units with the new scale, focus and scroll live in the screens), the settings load and the licenses write at start, and the
// cooperative runs (RootFS install, guest sessions) that draw a frame from inside the long call. No Vulkan or libnx types here:
// the platform and the backend are chosen at link time.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Backend.h"
#include "DrawContext.h"
#include "SettingsSchema.h"
#include "platform/Platform.h"
#include "render/Fonts.h"
#include "render/Icons.h"
#include "screens/Screen.h"

namespace ui {

class HomeScreen;
class InstallScreen;
class SettingsScreen;

enum class ScreenKind : uint8_t { Home, Settings, Ftp, RootfsSetup, Install, Shell, Picker };
const char* ScreenKindName(ScreenKind K);

class App {
public:
  static constexpr uint64_t FadeMs = 150;

  // `Rand` feeds the first-run FTP password. The platform and the backend must outlive the app.
  App(Platform& P, LauncherBackend& B, std::function<uint32_t()> Rand = {});
  ~App();

  bool Ok() const { return FontsOk; }
  // One frame: console messages, backend tick, input, update, draw, present, then the deferred long runs (install, guest session).
  // false when the app ends: the console closes it, Exit was confirmed, or the display failed for good.
  bool Frame();
  int Run();  // Frame() until it returns false

  // Tests and the host tools.
  void SetClock(std::function<uint64_t()> C) { Clock = std::move(C); }
  int Depth() const { return int(Stack.size()); }
  ScreenKind TopKind() const { return Stack.back().K; }
  ScreenKind KindAt(int I) const { return Stack[size_t(I)].K; }
  Screen& Top() { return *Stack.back().S; }
  Screen& At(int I) { return *Stack[size_t(I)].S; }
  bool Fading(uint64_t Now) const { return FadeOut && Now < FadeStart + FadeMs; }
  bool QuitRequested() const { return Quit; }
  void Post(const Request& R) { Queue.push_back(R); }  // as if the top screen had sent it (tests)
  SettingsModel& Settings() { return Model; }
  Fonts& GetFonts() { return F; }
  uint64_t LastNow() const { return LastNow_; }
  int FramesDrawn() const { return Drawn; }
  // The external-session hand-off (Steam, Desktop): unset until graphics exists. When set it is called with "steam" or "desktop",
  // blocks until the session ends and returns; Home is drawn again with the same focus (it stayed on the stack).
  std::function<void(const std::string& Kind)> ExternalSession;

private:
  struct Entry {
    std::unique_ptr<Screen> S;
    ScreenKind K;
  };
  bool DoFrame(bool Nested);
  bool PumpFrame();
  void RunDeferred();
  void Handle(const Request& R, uint64_t Now);
  void Push(std::unique_ptr<Screen> S, ScreenKind K, uint64_t Now);
  void Pop(uint64_t Now);
  void GoHome(uint64_t Now);
  void StartFirstRun(uint64_t Now);
  void BeginFade(Screen* Out, uint64_t Now);
  void Wire(Screen& S);
  void ApplySettings();
  void DrawScreen(Screen& S, const DrawContext& C, uint64_t Now);

  Platform& P;
  LauncherBackend& Be;
  Renderer& R;
  Fonts F;
  Icons I;
  bool FontsOk = false;
  std::function<uint32_t()> Rand;
  std::function<uint64_t()> Clock;
  SettingsModel Model;
  SettingsPaths Paths;
  std::vector<Entry> Stack;
  std::vector<Request> Queue;           // requests from the screen being updated, handled after its Update returns
  std::unique_ptr<Screen> Retired;      // a popped screen kept alive until its fade ended
  Screen* FadeOut = nullptr;
  uint64_t FadeStart = 0;
  bool Quit = false;
  bool InRun = false, InHook = false;
  int FailedFrames = 0;
  uint64_t LastNow_ = 0, LastPump = 0;
  int Drawn = 0;
  std::vector<Event> Events;
};

} // namespace ui
