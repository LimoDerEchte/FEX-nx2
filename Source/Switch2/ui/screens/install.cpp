// SPDX-License-Identifier: MIT
#include "screens/install.h"

#include <algorithm>
#include <cmath>

#include "PathUtil.h"

namespace ui {

using namespace install_layout;

namespace {
constexpr TextStyle StepName {"install_step", FontId::NunitoBold, 17};
constexpr TextStyle StepPct {"install_pct", FontId::NunitoExtraBold, 17};
constexpr TextStyle StepFoot {"install_foot", FontId::NunitoRegular, 14};
constexpr TextStyle Detail14 {"install_detail", FontId::NunitoRegular, 14};
constexpr TextStyle Sub16 {"install_sub", FontId::NunitoRegular, 16};
LogView::Kind KindOf(LogSpan::Role R) {
  switch (R) {
  case LogSpan::Role::Stamp: return LogView::Kind::Dim;
  case LogSpan::Role::Ok: return LogView::Kind::Ok;
  case LogSpan::Role::Warn: return LogView::Kind::Warn;
  case LogSpan::Role::Error: return LogView::Kind::Error;
  case LogSpan::Role::Text: return LogView::Kind::Normal;
  }
  return LogView::Kind::Normal;
}
} // namespace

// A focusable, invisible target so A reaches the screen when the install is done or failed.
class InstallScreen::Anchor final : public Widget {
public:
  Anchor() { Fixed = true; Hittable = false; Bounds = Rect {0, 0, 1, 1}; }
  void Draw(const Frame&) override {}
  bool ShowsRing() const override { return false; }
};

class InstallScreen::Sink final : public InstallEvents {
public:
  explicit Sink(InstallScreen& S) : Scr(S) {}
  void Step(int Index, const std::string& Title) override { Scr.Prog.SetStep(Index, Title); }
  void Progress(double Fraction, const std::string& Detail) override {
    Scr.Prog.SetFraction(Fraction, Scr.Now_);
    Scr.Detail = Detail;
    Scr.Bar_.SetValue(float(Scr.Prog.Fraction()), Scr.Now_);
  }
  void Log(InstallLogKind Kind, const std::string& Line) override {
    std::vector<LogView::Span> Parts;
    for (const LogSpan& P : InstallLogSpans(Scr.Now_ - Scr.StartMs, Kind, Line))
      Parts.push_back({KindOf(P.R), P.Text});
    Scr.Log_.AppendSpans(std::move(Parts));
  }
  void Pump() override {
    if (Scr.PumpHook)
      Scr.PumpHook();
  }
  bool Cancelled() override { return Scr.CancelFlag; }

private:
  InstallScreen& Scr;
};

InstallScreen::InstallScreen(LauncherBackend& B, InstallRequest R) : Be(B), Req(std::move(R)) {
  Events = std::make_unique<Sink>(*this);
  Act = std::make_unique<Anchor>();
  Version.Bounds.X = 48;
  Version.Bounds.Y = 32;
  Version.Version = Be.VersionText();
  if (Req.WipeOnly)
    WipeWhat = Be.WipeSummary();
  Log_.Bounds = Rect {ColX, LogTop, ColW, BottomY - ProgressH - 20 - LogTop};
  Bar_.Bounds = Rect {ColX, BottomY - ProgressH + 24 + 10, ColW, 12};

  CancelM.Title = "Cancel installation?";
  CancelM.OnSelect = [this](int I) {
    Host.Close();
    if (I == 1)
      CancelFlag = true;  // the installer notices at its next Pump and removes what it created
  };
  Act->OnActivate = [this]() {
    if (St == State::Done)
      Continue();
    else if (St == State::Failed)
      Retry();
  };
  Act->Enabled = false;
  Bar.OnHint = [this](Button Btn) {
    UiEvent E;
    E.Kind = UiEvent::Type::Button;
    E.Btn = Btn;
    Interact.Dispatch(E, Now_);
  };
  Interact.OnBack = [this]() {
    if (Req.WipeOnly && (St == State::Running || St == State::Idle))
      return;  // a wipe cannot be cancelled
    if (St == State::Running || St == State::Idle)
      AskCancel();
    else if (St == State::Failed)
      Send(Request::Kind::Back);
  };
  Interact.OnButton = [this](Button Btn) {
    if (Host.IsOpen())
      return;
    if (Btn == Button::ZL)
      Log_.ScrollBy(-Log_.Bounds.H * 0.8f);
    else if (Btn == Button::ZR)
      Log_.ScrollBy(Log_.Bounds.H * 0.8f);
  };
  Interact.ScrollFilter = [this](float, float, float Dy) {
    if (Host.IsOpen())
      return false;
    Log_.ScrollBy(Dy);
    return true;
  };
  std::vector<Widget*> L = {Act.get(), &Version, &Log_, &Bar_, &Bar};
  Interact.SetRoot(L);
  SetBar();
}

InstallScreen::~InstallScreen() = default;

std::string InstallScreen::Title() const {
  if (Req.WipeOnly) {
    switch (St) {
    case State::Done: return "RootFS wiped";
    case State::Failed: return "Wipe failed";
    default: return "Wiping the RootFS";
    }
  }
  switch (St) {
  case State::Done: return "RootFS installed";
  case State::Failed: return "Installation failed";
  default: return "Installing the RootFS";
  }
}

std::string InstallScreen::Subtitle() const {
  if (Req.WipeOnly)
    return "Removing " + WipeWhat;
  const RootfsImage& I = Req.Image;
  std::string S = (I.Name.empty() ? std::string("RootFS") : I.Name) + " \xC2\xB7 x86-64";
  const uint64_t Bytes = InstallDownloadBytes(I, !Req.LocalPath.empty(), Req.Extras);
  if (!Req.LocalPath.empty())
    S += " \xC2\xB7 from the SD card";
  else if (Bytes)
    S += " \xC2\xB7 " + FormatBytes(Bytes) + " to download";
  return S;
}

bool InstallScreen::CancelKeepsDownload() const { return Req.LocalPath.empty() && Prog.StepIndex() >= FirstStepAfterVerify; }

void InstallScreen::AskCancel() {
  if (Host.IsOpen())
    return;
  CancelM.Options = {"Keep installing", CancelKeepsDownload() ? "Cancel, keep the download" : "Cancel and remove files"};
  CancelM.Selected = 0;
  CancelM.Open(Now_);
  Host.Open(CancelM);
}

void InstallScreen::SetBar() {
  Bar.Segs.clear();
  if (Req.WipeOnly && St == State::Done) {
    Bar.SetHints({{Button::A, "Continue"}});
    Bar.Status = "Continue opens RootFS setup";
  } else if (Req.WipeOnly && (St == State::Running || St == State::Idle)) {
    Bar.SetHints({});
    Bar.Status = "Keep the console on until the wipe finishes";
  } else if (St == State::Done) {
    Bar.SetHints({{Button::A, "Continue"}});
    if (!Out.Warning.empty())
      Bar.Segs.push_back({HintBar::Seg::Kind::Warn, Out.Warning});
    else
      Bar.Status = "";
  } else if (St == State::Failed) {
    Bar.SetHints({{Button::A, "Retry"}, {Button::B, "Back"}});
    Bar.Segs.push_back({HintBar::Seg::Kind::Error, Out.Error});
  } else {
    Bar.SetHints({{Button::B, "Cancel"}});
    Bar.Status = "Keep the console on until installation finishes";
  }
  Act->Enabled = St == State::Done || St == State::Failed;
}

void InstallScreen::Run() {
  Wants = false;
  ++RunCount;
  St = State::Running;
  CancelFlag = false;
  Out = InstallOutcome();
  StartMs = Now_;
  Prog.Start(Now_, Req.WipeOnly ? 2 : InstallStepCount);
  Bar_.Reset(Now_);
  Detail.clear();
  Log_.Clear();  // a new run starts with a clean log
  SetBar();
  Finish(Req.WipeOnly ? Be.WipeRootfs(*Events) : Be.InstallRootfs(Req, *Events));
}

void InstallScreen::Finish(const InstallOutcome& O) {
  Out = O;
  if (O.Ok) {
    St = State::Done;
    Prog.Finish(Now_);
    Bar_.SetValue(1.0f, Now_);
    if (!O.Warning.empty())
      Events->Log(InstallLogKind::Warn, O.Warning);
    Events->Log(InstallLogKind::Ok, Req.WipeOnly ? "RootFS wiped" : "RootFS installed");
  } else if (O.Cancelled) {
    St = State::Cancelled;
    Send(Request::Kind::Back);
  } else {
    St = State::Failed;
    // The installer logs its own failure line; add the outcome only when it is not the last line already.
    const std::string Msg = O.Error.empty() ? (Req.WipeOnly ? "The wipe failed" : "The installation failed") : O.Error;
    if (Log_.LineCount() == 0 || Log_.Line(Log_.LineCount() - 1).find(Msg) == std::string::npos)
      Events->Log(InstallLogKind::Error, Msg);
  }
  SetBar();
  if (Act->Enabled)
    Interact.SetFocus(Act->Id, Now_);
}

void InstallScreen::Continue() {
  if (St == State::Done)
    Send(Req.WipeOnly ? Request::Kind::StartFirstRun : Request::Kind::OpenHome);  // after a wipe: first-run RootFS setup
}

void InstallScreen::Retry() {
  if (St != State::Failed)
    return;
  St = State::Idle;
  Wants = true;  // the app calls Run() again
  SetBar();
}

void InstallScreen::Update(uint64_t Now, float Scale, const std::vector<Event>& Raw) {
  Now_ = Now;
  Host.Tick(Now);
  Interact.Update(Now, Scale, Raw);
}

void InstallScreen::Draw(const DrawContext& C, uint64_t Now) {
  Now_ = Now;
  const Theme& T = C.T;
  const float S = C.S;
  const Frame F {C, Now, 0};
  Version.Draw(F);
  const float TitleH = C.F.LineHeightPx(styles::TitleInstall, S) / S;
  F.Text(styles::TitleInstall, Title(), ColX, TitleY, TitleH, St == State::Failed ? T.Error : T.Text);
  F.Text(Sub16, Subtitle(), ColX, TitleY + TitleH + 6, 24, T.TextMuted);
  Log_.Draw(F);

  // Progress block: step name left, percentage right, the bar, the footer.
  const float Top = BottomY - ProgressH + 4;
  std::string Step = Prog.StepTitle().empty() ? "Starting" : Prog.StepTitle();
  if (St == State::Done)
    Step = "Done";
  else if (St == State::Failed)
    Step = "Failed at: " + Step;
  const std::string Pct = std::to_string(Prog.Percent()) + "%";
  const float PctW = F.Measure(StepPct, Pct);
  const float StepMax = ColW - PctW - 24;
  const std::string Fit = C.F.Fit(StepName, Step, S, StepMax * S, Ellipsis::End);
  const float StepW = F.Measure(StepName, Fit);
  F.Text(StepName, Fit, ColX, Top, 24, St == State::Failed ? T.Error : T.Text);
  if (St == State::Running && !Detail.empty()) {
    const float Room = StepMax - StepW - 14;
    if (Room > 60)
      F.Text(Detail14, C.F.Fit(Detail14, Detail, S, Room * S, Ellipsis::End), ColX + StepW + 14, Top, 24, T.TextMuted);
  }
  F.Text(StepPct, Pct, ColX + ColW, Top, 24, T.Accent, Align::Right);
  Bar_.Bounds.Y = Top + 24 + 10;
  Bar_.Draw(F);
  F.Text(StepFoot, St == State::Failed ? Prog.StepText() : Prog.FooterText(Now), ColX, Bar_.Bounds.Y + 12 + 10, 20, T.TextMuted);

  Bar.Draw(F);
  Host.Draw(F, {&CancelM});
  Interact.DrawOverlay(F);
}

} // namespace ui
