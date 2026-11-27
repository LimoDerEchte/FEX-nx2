// SPDX-License-Identifier: MIT
// Installation (spec "Installation"): version block, title and subtitle, the LogView (colored line rules, 2,000 lines), and the
// progress block (step name, percentage in the accent, bar, "Step n of 7 · about 4 min left"). It runs the backend installer, which is
// cooperative: Run() blocks, and the installer's Pump() calls PumpHook so the app draws a frame and polls input (B opens
// "Cancel installation?"). The steps are the installer's real seven, not the spec's five. A cancel after the image's hash check
// passed keeps the downloaded image (/fex/.download); any other cancel removes everything the install created.
// Done: A Continue (Home). Failed: A Retry (NeedsRun() is true again; the app calls Run()), B Back to RootFS setup.
// WipeOnly (Settings > Wipe RootFS): the same screen runs the backend's WipeRootfs, 2 steps, titled "Wiping the RootFS"; it has no
// cancel; Done: A Continue (first-run RootFS setup, the other screens close); Failed: A Retry, B Back to Settings.
#pragma once

#include <memory>

#include "InstallModel.h"
#include "components/Progress.h"
#include "screens/Screen.h"

namespace ui {

namespace install_layout {
inline constexpr float ColX = 160, ColW = 960, TitleY = 96, LogTop = 193, ProgressH = 76, BottomY = 632;
} // namespace install_layout

class InstallScreen final : public Screen {
public:
  enum class State : uint8_t { Idle, Running, Done, Failed, Cancelled };

  InstallScreen(LauncherBackend& B, InstallRequest Req);
  ~InstallScreen() override;

  void Update(uint64_t Now, float Scale, const std::vector<Event>& Raw) override;
  void Draw(const DrawContext& C, uint64_t Now) override;
  Interaction& Input() override { return Interact; }

  // Runs the installer to its end (blocking, cooperative). The app sets PumpHook to "poll input, Update, Draw, present".
  void Run();
  std::function<void()> PumpHook;
  bool NeedsRun() const { return Wants; }

  // ---- state (tests and the host tool) ----
  State GetState() const { return St; }
  const InstallProgress& Progress() const { return Prog; }
  LogView& Log() { return Log_; }
  HintBar& Hints() { return Bar; }
  PickerModal& CancelPicker() { return CancelM; }
  ModalHost& Modals() { return Host; }
  const InstallOutcome& Outcome() const { return Out; }
  const InstallRequest& Request_() const { return Req; }
  std::string Title() const;
  std::string Subtitle() const;
  bool CancelKeepsDownload() const;       // the Cancel option text and result: the image already passed its hash check
  bool CancelRequested() const { return CancelFlag; }
  void Abort() { CancelFlag = true; }     // the app is closing: stop the installer like a confirmed cancel
  void AskCancel();                       // B while running
  void Continue();                        // A when done
  void Retry();                           // A when failed

private:
  class Sink;
  void Finish(const InstallOutcome& O);
  void SetBar();

  LauncherBackend& Be;
  InstallRequest Req;
  std::unique_ptr<Sink> Events;
  Interaction Interact;
  ModalHost Host {Interact};
  VersionBlock Version;
  LogView Log_;
  ProgressBar Bar_;
  HintBar Bar;
  PickerModal CancelM;
  class Anchor;
  std::unique_ptr<Anchor> Act;
  InstallProgress Prog;
  InstallOutcome Out;
  State St = State::Idle;
  bool CancelFlag = false;
  bool Wants = true;
  uint64_t Now_ = 0, StartMs = 0;
  std::string Detail;
  std::string WipeWhat;  // WipeOnly: the backend's WipeSummary, read once (the RootFS is gone afterwards)
  int RunCount = 0;
};

} // namespace ui
