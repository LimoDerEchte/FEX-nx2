// SPDX-License-Identifier: MIT
// Shell (spec "Shell"): KDE-style tab strip (tabs, close buttons, new-tab button only), the terminal surface drawing a TerminalGrid in
// mono_term with the theme's Breeze palette (grid = floor(width / advance) x floor(height / line), about 138x25 at 1280x720), OSC 0/2
// tab titles, the session banner, and a HintBar (ZR/ZL new/close tab, B, X, Y, R3 Help) and the Help list (R3) of all bindings.
// The backend can disable new tabs during a launch that requires a dedicated runtime.
// D-pad = arrow keys, X selects one control key from the D-pad, A = Enter (a tap opens the keyboard), B = Escape.
// L/R switch tabs, ZR opens a new tab, ZL closes the current one. The right stick scrolls the back buffer (Shift+PgUp/PgDn on a keyboard),
// - exits to Home (a confirm while a process other than the shell runs), closing the last tab goes Home.
#pragma once

#include <memory>

#include "components/Chrome.h"
#include "components/Terminal.h"
#include "screens/Screen.h"

namespace ui {

namespace shell_layout {
inline constexpr float HintH = chrome::FooterH, HintY = chrome::FooterY, TermY = tabs::StripH, TermH = chrome::FooterY - tabs::StripH;  // the footer every screen has
inline constexpr size_t MaxBytesPerFrame = 64 * 1024;
} // namespace shell_layout

namespace control_keys {  // the X overlay (du): panel, button disc radius, distance of the four discs from the cross centre
inline constexpr Rect Panel {330, 175, 620, 370};
inline constexpr float DiscR = 30, Pitch = 92;
} // namespace control_keys

class ShellScreen final : public Screen {
public:
  // `Program` set: the first tab runs that session (a developer program) instead of a new shell; the tab stays after it ended
  // when the session has an ExitNote.
  ShellScreen(LauncherBackend& B, bool Root = false, std::unique_ptr<ShellSession> Program = {});
  ~ShellScreen() override;

  void Update(uint64_t Now, float Scale, const std::vector<Event>& Raw) override;
  void Draw(const DrawContext& C, uint64_t Now) override;
  Interaction& Input() override { return Interact; }

  struct Tab {
    std::unique_ptr<ShellSession> Session;
    TerminalGrid Grid;
    bool Root = false;
    bool Program = false;  // a developer program, not a shell
    bool Ended = false;    // the program ended and its tab shows the exit note
  };
  // ---- tests and the host tool ----
  int TabCount() const { return int(Tabs.size()); }
  int ActiveTab() const { return Active; }
  Tab& GetTab(int I) { return *Tabs[size_t(I)]; }
  TabStrip& Strip() { return Strip_; }
  void SetForeground(bool Value);
  TerminalView& Terminal() { return Term; }
  HintBar& Hints() { return Bar; }
  ConfirmDialog& Confirm() { return ConfirmM; }
  InfoDialog& Help() { return HelpM; }
  ModalHost& Modals() { return Host; }
  std::string TabTitle(int I) const;
  void SelectTab(int I);
  void NextTab(int Dir);
  void ShowStatus(std::string Message);  // replaces the button map in the footer for a few seconds
  bool NewTab(bool Root = false, std::unique_ptr<ShellSession> Program = {});
  void CloseTab(int I);              // confirms first while a process runs in the last tab
  void ExitToHome();                 // - : confirm while a process runs, else Home
  void OpenKeyboard();               // Y
  void OpenHelp();                   // R3: all the bindings
  void SendKey(Dir D);
  bool ControlKeysArmed() const { return ControlKeys; }
  void Pump();                       // reads session output into the grids (also done by Update)
  void FitGrids(const Frame& F);     // resizes every grid to the terminal area (Draw does it every frame)

private:
  void Banner(Tab& T);
  void DrawControlKeys(const Frame& F);
  void ShowMap();
  void SetControlKeys(bool Enabled);
  void SendKey(Key K, uint8_t Modifiers = 0, char32_t Text = 0);
  void RemoveTab(int I);
  void Sync();
  std::string DefaultTitle(const Tab& T) const;

  LauncherBackend& Be;
  Interaction Interact;
  ModalHost Host {Interact};
  TabStrip Strip_;
  TerminalView Term;
  HintBar Bar;
  ConfirmDialog ConfirmM;
  InfoDialog HelpM;
  std::vector<std::unique_ptr<Tab>> Tabs;
  int Active = 0;
  int PendingClose = -1;   // -1 none, -2 exit to Home
  int Cols = 0, Rows = 0;
  bool RootBuilt = false, ControlKeys = false;
  uint64_t Now_ = 0, StatusUntil = 0;
  float LineDu = 23.25f;
  float WheelRemainder = 0;
};

} // namespace ui
