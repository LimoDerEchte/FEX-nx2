// SPDX-License-Identifier: MIT
#include "screens/shell.h"

#include "TerminalKeys.h"

#include <algorithm>
#include <cmath>

namespace ui {

using namespace shell_layout;

namespace {
using Seg = HintBar::Seg;
Seg Disc(Button B) { return {Seg::Kind::Key, HintLetter(B)}; }
Seg Label(const char* Text) { return {Seg::Kind::Body, Text}; }
Seg Gap() { return {Seg::Kind::Gap, ""}; }
} // namespace

// Footer: only the main bindings, as button discs like the hints on the right (nothing is spelled with arrow characters, the
// UI font has none). Left: ZR new tab, ZL close tab, B Esc. Right (tappable): X arms or cancels the control keys, Y keyboard,
// R3 (right stick click) opens the list of every binding.
void ShellScreen::ShowMap() {
  Bar.Status.clear();
  // While the control keys are armed the overlay (DrawControlKeys) shows the four D-pad keys, so the footer keeps its normal map.
  Bar.Segs = {Disc(Button::ZR), Label("New tab"), Gap(), Disc(Button::ZL), Label("Close tab"), Gap(), Disc(Button::B), Label("Esc")};
  Bar.SetHints({{Button::X, ControlKeys ? "Cancel" : "Ctrl keys"}, {Button::Y, "Keyboard"}, {Button::StickR, "Help"}});
}

ShellScreen::ShellScreen(LauncherBackend& B, bool Root, std::unique_ptr<ShellSession> Program) : Be(B) {
  Term.Bounds = Rect {0, TermY, 1280, TermH};
  Bar.Bounds = Rect {0, HintY, 1280, HintH};
  ShowMap();
  Bar.OnHint = [this](Button Btn) {
    UiEvent E;
    E.Kind = UiEvent::Type::Button;
    E.Btn = Btn;
    Interact.Dispatch(E, Now_);
  };
  const Capability More = Be.MoreShells();
  Strip_.NewEnabled = More.Available;
  Strip_.OnSelect = [this](int I) { SelectTab(I); };
  Strip_.OnClose = [this](int I) { CloseTab(I); };
  Strip_.OnNew = [this]() { NewTab(false); };

  ConfirmM.Title = "Exit to FEX?";
  ConfirmM.Message = "A program is still running in the shell. Leaving stops it.";
  ConfirmM.ConfirmText = "Exit";
  ConfirmM.Destructive = true;
  ConfirmM.OnConfirm = [this]() {
    Host.Close();
    const int What = PendingClose;
    PendingClose = -1;
    if (What == -2) {
      Term.Grid = nullptr;
      Tabs.clear();
      Send(Request::Kind::OpenHome);
    } else if (What >= 0) {
      RemoveTab(What);
    }
  };

  Term.OnArrow = [this](Dir D, uint64_t) {
    SendKey(D);
    return true;
  };
  Term.OnActivate = [this]() {
    // A sends Enter; a tap (touch mode) opens the keyboard instead. While the control keys are armed a tap or A cancels them.
    if (ControlKeys)
      SetControlKeys(false);
    else if (Interact.GetMode() == Mode::Touch)
      OpenKeyboard();
    else
      SendKey(Key::Enter);
  };
  Interact.OnBack = [this]() {
    SetControlKeys(false);
    SendKey(Key::Escape);
  };
  HelpM.Title = "Shell controls";
  HelpM.Rows = {{"ZR / ZL", "New tab / close the current tab"},
                {"L / R", "Previous / next tab"},
                {"D-pad, left stick", "Arrow keys"},
                {"A / B", "Enter / Escape"},
                {"X, then D-pad Up", "Ctrl+C (interrupt)"},
                {"X, then D-pad Down", "Ctrl+Z (suspend)"},
                {"X, then D-pad Left", "Ctrl+D (end of input)"},
                {"X, then D-pad Right", "Tab (complete)"},
                {"Y", "Software keyboard (text, Enter, paste)"},
                {"Right stick, touch drag", "Scroll back; typing returns to the end"},
                {"Shift+PgUp / PgDn", "Scroll back a page (keyboard)"},
                {"Plus / Minus", "Settings / exit to FEX"},
                {"R3 or B", "Close this list"}};
  Interact.OnButton = [this](Button Btn) {
    if (Btn == Button::StickR) {  // click of the right stick: the list of all bindings, the same click closes it
      if (Host.Top() == &HelpM)
        Host.Close();
      else if (!Host.IsOpen())
        OpenHelp();
      return;
    }
    if (Host.IsOpen())
      return;
    switch (Btn) {
    case Button::L: NextTab(-1); break;
    case Button::R: NextTab(1); break;
    case Button::Y: OpenKeyboard(); break;
    case Button::Plus: SetControlKeys(false); Send(Request::Kind::OpenSettings); break;
    case Button::X:
      if (!Tabs.empty()) {
        SetControlKeys(!ControlKeys);
        Interact.SetFocus(Term.Id, Now_);
      }
      break;
    case Button::Minus: ExitToHome(); break;
    case Button::ZR: NewTab(false); break;
    case Button::ZL: CloseTab(Active); break;  // a shell at its prompt hangs up at once; a running program asks first
    default: break;
    }
  };
  Interact.ScrollFilter = [this](float, float, float Dy) {
    if (Host.IsOpen() || Tabs.empty())
      return false;
    // Dy in du, positive = towards newer text (as in the log views): the back buffer moves by whole lines.
    WheelRemainder += -Dy / LineDu;
    const int Lines = int(WheelRemainder);
    WheelRemainder -= float(Lines);
    if (Lines)
      Tabs[size_t(Active)]->Grid.ScrollView(Lines);
    return true;
  };

  NewTab(Root, std::move(Program));
  Sync();
}

ShellScreen::~ShellScreen() = default;

std::string ShellScreen::DefaultTitle(const Tab& T) const {
  const std::string Own = T.Session->Title();
  return !Own.empty() ? Own : T.Root ? "Root shell" : "Shell";
}

std::string ShellScreen::TabTitle(int I) const {
  const Tab& T = *Tabs[size_t(I)];
  return T.Grid.Title().empty() ? DefaultTitle(T) : T.Grid.Title();
}

void ShellScreen::Banner(Tab& T) {
  CellColor Focus, Inactive;
  Focus.K = CellColor::Kind::Focus;
  Inactive.K = CellColor::Kind::Inactive;
  T.Grid.PrintStyled("FEX-Emu " + Be.FexBuild() + " \xC2\xB7 x86-64 guest on ARM64", Focus, true);
  const std::string Name = Be.Rootfs().Name.empty() ? std::string("RootFS") : Be.Rootfs().Name;
  T.Grid.PrintStyled("RootFS " + Name + " mounted at /", Inactive, false);
}

void ShellScreen::ShowStatus(std::string Message) {
  Bar.Segs.clear();
  Bar.Status = std::move(Message);
  StatusUntil = Now_ + 5000;
}

bool ShellScreen::NewTab(bool Root, std::unique_ptr<ShellSession> Program) {
  std::string Err;
  if (const auto More = Be.MoreShells(); !Program && !Tabs.empty() && !More.Available) {
    ShowStatus(More.Reason);
    return false;
  }
  auto T = std::make_unique<Tab>();
  T->Root = Root;
  T->Program = Program != nullptr;
  T->Session = Program ? std::move(Program) : Be.StartShell(Root, Err);
  if (!T->Session) {
    ShowStatus("Could not start the shell: " + Err);
    if (Tabs.empty())
      Send(Request::Kind::Back);
    return false;
  }
  T->Grid.Resize(Cols > 0 ? Cols : 138, Rows > 0 ? Rows : 25);
  T->Session->Resize(T->Grid.Cols(), T->Grid.Rows());
  if (!T->Program)
    Banner(*T);
  Tabs.push_back(std::move(T));
  SetControlKeys(false);
  Active = int(Tabs.size()) - 1;
  Sync();
  return true;
}

void ShellScreen::Sync() {
  std::vector<std::string> Titles;
  for (int I = 0; I < int(Tabs.size()); ++I)
    Titles.push_back(TabTitle(I));
  const int Before = Strip_.Count();
  Strip_.SetTabs(Titles, Active);
  Strip_.NewEnabled = Be.MoreShells().Available;
  if (Before != Strip_.Count() || !RootBuilt) {
    RootBuilt = true;
    std::vector<Widget*> L = {&Strip_, &Term, &Bar};
    Interact.SetRoot(L);
    Interact.SetFocus(Term.Id, Now_);
  }
  Term.Grid = Tabs.empty() ? nullptr : &Tabs[size_t(Active)]->Grid;
}

void ShellScreen::SelectTab(int I) {
  if (I < 0 || I >= int(Tabs.size()))
    return;
  SetControlKeys(false);
  Active = I;
  Interact.SetFocus(Term.Id, Now_);
  Sync();
}

void ShellScreen::NextTab(int Dir) {
  if (Tabs.size() < 2)
    return;
  SelectTab((Active + Dir + int(Tabs.size())) % int(Tabs.size()));
}

void ShellScreen::RemoveTab(int I) {
  if (I < 0 || I >= int(Tabs.size()))
    return;
  if (PendingClose == I) {
    PendingClose = -1;
    Host.Close();
  } else if (PendingClose > I) {
    --PendingClose;
  }
  Term.Grid = nullptr;
  Tabs.erase(Tabs.begin() + I);
  SetControlKeys(false);
  if (Tabs.empty()) {
    Send(Request::Kind::OpenHome);
    return;
  }
  Active = std::min(Active > I ? Active - 1 : Active, int(Tabs.size()) - 1);
  Sync();
}

void ShellScreen::CloseTab(int I) {
  if (I < 0 || I >= int(Tabs.size()) || Host.IsOpen())
    return;
  if (Tabs[size_t(I)]->Session->ChildRunning()) {
    PendingClose = I;
    ConfirmM.Title = "Close this tab?";
    ConfirmM.Message = "A program is still running in it. Closing stops it.";
    ConfirmM.ConfirmText = "Close";
    ConfirmM.Open(Now_);
    Host.Open(ConfirmM);
    return;
  }
  RemoveTab(I);
}

void ShellScreen::ExitToHome() {
  if (Host.IsOpen())
    return;
  bool Running = false;
  for (auto& T : Tabs)
    Running |= T->Session->ChildRunning();
  if (Running) {
    PendingClose = -2;
    ConfirmM.Title = "Exit to FEX?";
    ConfirmM.Message = "A program is still running in the shell. Leaving stops it.";
    ConfirmM.ConfirmText = "Exit";
    ConfirmM.Open(Now_);
    Host.Open(ConfirmM);
    return;
  }
  Term.Grid = nullptr;
  Tabs.clear();
  Send(Request::Kind::OpenHome);
}

void ShellScreen::OpenHelp() {
  if (Host.IsOpen())
    return;
  SetControlKeys(false);
  HelpM.Open(Now_);
  Host.Open(HelpM);
}

void ShellScreen::OpenKeyboard() {
  if (Tabs.empty() || Host.IsOpen())
    return;
  SetControlKeys(false);
  std::string Text;
  if (Be.KeyboardText("Type into the shell", "", KeyboardKind::Text, Text) && !Text.empty()) {
    auto& T = *Tabs[size_t(Active)];
    T.Grid.ScrollToLive();
    T.Session->Write(EncodeTerminalPaste(T.Grid, Text));
  }
}

void ShellScreen::SetControlKeys(bool Enabled) {
  if (Enabled == ControlKeys)
    return;
  ControlKeys = Enabled;
  ShowMap();
}

void ShellScreen::SendKey(Dir D) {
  if (ControlKeys) {
    SetControlKeys(false);
    if (D == Dir::Right)
      SendKey(Key::Tab);
    else
      SendKey(Key::Text, Ctrl, D == Dir::Up ? U'C' : D == Dir::Down ? U'Z' : U'D');
    return;
  }
  SendKey(D == Dir::Up ? Key::Up : D == Dir::Down ? Key::Down : D == Dir::Right ? Key::Right : Key::Left);
}

void ShellScreen::SendKey(Key K, uint8_t Modifiers, char32_t Text) {
  if (Tabs.empty() || Host.IsOpen())
    return;
  auto& T = *Tabs[size_t(Active)];
  const std::string Bytes = EncodeTerminalKey(T.Grid, K, Modifiers, Text);
  if (!Bytes.empty()) {
    T.Grid.ScrollToLive();
    T.Session->Write(Bytes);
  }
}

void ShellScreen::Pump() {
  char Buf[4096];
  for (int I = 0; I < int(Tabs.size());) {
    Tab& T = *Tabs[size_t(I)];
    size_t Total = 0;
    for (size_t N; Total < MaxBytesPerFrame && (N = T.Session->Read(Buf, sizeof(Buf))) > 0;) {
      T.Grid.Feed(std::string_view(Buf, N));
      const std::string Reply = T.Grid.TakeReplies();
      if (!Reply.empty())
        T.Session->Write(Reply);
      T.Grid.TakeBell();  // BEL (Tab with several completions, PgUp unbound in readline): no flash, no sound
      Total += N;
    }
    if (!T.Session->Running() && !T.Session->ChildRunning()) {
      // Program exit notes and startup failures keep the tab visible. A normal shell exit closes it.
      const std::string Note = T.Ended ? std::string() : T.Session->ExitNote();
      if (T.Ended || !Note.empty()) {
        if (!T.Ended) {
          T.Ended = true;
          CellColor Inactive;
          Inactive.K = CellColor::Kind::Inactive;
          T.Grid.PrintStyled("", Inactive, false);
          T.Grid.PrintStyled(Note, Inactive, true);
        }
        ++I;
        continue;
      }
      RemoveTab(I);
      continue;
    }
    ++I;
  }
}

void ShellScreen::FitGrids(const Frame& F) {
  const TerminalSize Sz = Term.FitSize(F);
  LineDu = TerminalView::LinePitch(F.S()) / F.S();
  if (Sz.Cols == Cols && Sz.Rows == Rows)
    return;
  Cols = Sz.Cols;
  Rows = Sz.Rows;
  for (auto& T : Tabs) {
    T->Grid.Resize(Cols, Rows);
    T->Session->Resize(Cols, Rows);
  }
}

void ShellScreen::Update(uint64_t Now, float Scale, const std::vector<Event>& Raw) {
  Now_ = Now;
  Host.Tick(Now);
  Pump();
  Interact.Update(Now, Scale, Raw);
  for (const auto& E : Raw)
    if (E.Kind == Event::Type::KeyDown) {
      if ((E.Modifiers & Shift) && (E.KeyboardKey == Key::PageUp || E.KeyboardKey == Key::PageDown) && !Tabs.empty() && !Host.IsOpen()) {
        const int Page = Rows > 2 ? Rows - 2 : 1;  // Shift+PgUp/PgDn page through the back buffer, as in other terminals
        Tabs[size_t(Active)]->Grid.ScrollView(E.KeyboardKey == Key::PageUp ? Page : -Page);
        continue;
      }
      SendKey(E.KeyboardKey, E.Modifiers, E.Codepoint);
    }
  Strip_.NewEnabled = Be.MoreShells().Available;
  if (StatusUntil && Now >= StatusUntil) {
    StatusUntil = 0;
    ShowMap();
  }
  if (!Tabs.empty()) {
    bool Changed = false;
    for (int I = 0; I < int(Tabs.size()); ++I)
      Changed |= Tabs[size_t(I)]->Grid.TitleChanged();
    if (Changed)
      Sync();
  }
}

// X armed: the whole screen darkens; the four D-pad keys sit in its middle as a cross, each combination written on the
// outside of its key (above Up, below Down, left of Left, right of Right). No panel.
void ShellScreen::DrawControlKeys(const Frame& F) {
  const Theme& T = F.T();
  const float S = F.S();
  const Size Sz = F.Dc.R.TargetSize();
  F.Dc.R.FillRect(Rect {0, 0, float(Sz.W), float(Sz.H)}, T.Scrim);
  const Rect P = control_keys::Panel;
  const float Cx = P.X + P.W / 2, Cy = P.Y + P.H / 2;
  static constexpr TextStyle ArrowStyle {"ctrl_arrow", FontId::MonoSemiBold, 26};
  static constexpr TextStyle KeyStyle {"ctrl_label", FontId::NunitoExtraBold, 22};
  enum class Side { Above, Below, Left, Right };
  const auto Key = [&](Button B, const char* Label, float Dx, float Dy, Side Where) {
    const float X = Cx + Dx, Y = Cy + Dy, R = control_keys::DiscR;
    constexpr float Gap = 14, LineH = 28;
    F.Dc.R.FillCircle(std::round(X * S), std::round(Y * S), std::round(R * S), T.HintDisc);
    F.Text(ArrowStyle, HintLetter(B), X, Y - R, 2 * R, T.HintLetter, Align::Center);
    switch (Where) {
    case Side::Above: F.Text(KeyStyle, Label, X, Y - R - Gap - LineH, LineH, T.Text, Align::Center); break;
    case Side::Below: F.Text(KeyStyle, Label, X, Y + R + Gap, LineH, T.Text, Align::Center); break;
    case Side::Left: F.Text(KeyStyle, Label, X - R - Gap, Y - R, 2 * R, T.Text, Align::Right); break;
    case Side::Right: F.Text(KeyStyle, Label, X + R + Gap, Y - R, 2 * R, T.Text, Align::Left); break;
    }
  };
  const float D = control_keys::Pitch;
  Key(Button::Up, "Ctrl+C", 0, -D, Side::Above);
  Key(Button::Down, "Ctrl+Z", 0, D, Side::Below);
  Key(Button::Left, "Ctrl+D", -D, 0, Side::Left);
  Key(Button::Right, "Tab", D, 0, Side::Right);
}

void ShellScreen::Draw(const DrawContext& C, uint64_t Now) {
  Now_ = Now;
  const Theme& T = C.T;
  const Frame F {C, Now, 0};
  C.R.FillRect(Rect {0, 0, float(C.R.TargetSize().W), float(C.R.TargetSize().H)}, T.KdeWindow);
  FitGrids(F);
  Term.Draw(F);
  Strip_.Draw(F);
  Bar.Draw(F);
  if (ControlKeys)
    DrawControlKeys(F);
  Host.Draw(F, {&ConfirmM, &HelpM});
  Interact.DrawOverlay(F);
}

} // namespace ui
