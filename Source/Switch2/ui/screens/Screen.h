// SPDX-License-Identifier: MIT
// What every launcher screen offers to the app shell (Task 7): per-frame update with the raw input, draw, the Interaction (focus
// and scroll state live in it, so a screen kept alive keeps its focus), and navigation requests.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Backend.h"
#include "DrawContext.h"
#include "components/Modals.h"
#include "input/Interaction.h"

namespace ui {

// A file picker invocation: what to choose and where to start.
struct PickerSpec {
  std::string Title;                  // "Choose RootFS image"
  std::vector<std::string> Accept;    // lower-case extensions with the dot; empty = any file
  bool Folder = false;                // choose a folder: files hidden, "+ Use this folder"
  std::string Current;                // the setting's current value: the start folder is its folder
  std::string FallbackLocation = "fex";  // "sd", "fex", "rootfs" or "home" when Current is not usable
};

struct Request {
  enum class Kind : uint8_t {
    Back,             // return to the previous screen (Home from Settings)
    OpenSettings,
    OpenAbout,
    OpenFtp,
    LaunchShell,
    LaunchSteam,
    LaunchDesktop,
    OpenRootfsSetup,  // Reinstall = true from Settings > Reinstall RootFS
    OpenFilePicker,   // Picker + OnPicked(path)
    Exit,             // leave the app (Home B, first-run RootFS setup B)
    OpenHome,         // go to Home (Installation done, last shell tab closed, shell exit)
    StartInstall,     // RootFS setup + Install: show Installation with `Install`; the app calls InstallScreen::Run()
    DevAction,        // Settings > Developer: `Action` (+ `Path`), the app opens a program tab in the Shell screen
    StartFirstRun,    // after a wipe: every screen closes and first-run RootFS setup opens, as on a launch without a RootFS
  };
  Kind K = Kind::Back;
  std::string Action, Path;  // DevAction
  bool Reinstall = false;
  PickerSpec Picker;
  std::function<void(const std::string&)> OnPicked;
  InstallRequest Install;  // StartInstall
};

class Screen {
public:
  virtual ~Screen() = default;
  virtual void Update(uint64_t Now, float Scale, const std::vector<Event>& Raw) = 0;
  virtual void Draw(const DrawContext& C, uint64_t Now) = 0;
  virtual Interaction& Input() = 0;
  std::function<void(const Request&)> OnRequest;

protected:
  void Send(Request::Kind K) {
    if (OnRequest) {
      Request R;
      R.K = K;
      OnRequest(R);
    }
  }
};

// Opens modals on a screen's Interaction (one focus scope per modal; B, the scrim or Cancel closes the top one and the focus
// returns to what it was) and draws the open and fading ones.
class ModalHost {
public:
  explicit ModalHost(Interaction& I) : In(I) {}
  void Tick(uint64_t Now) { Clock = Now; }  // the time Hide() fades from; call every Update
  void Open(ModalBase& M) {
    Stack.push_back(&M);
    std::vector<Widget*> L;
    M.Layer(L);
    M.OnCancel = [this]() { Close(); };
    In.PushLayer(L, M.InitialFocus(), [this]() { Close(); });
  }
  void Close() {  // the top modal
    if (Stack.empty())
      return;
    Stack.back()->Hide(Clock);
    Stack.pop_back();
    In.PopLayer();
  }
  bool IsOpen() const { return !Stack.empty(); }
  ModalBase* Top() const { return Stack.empty() ? nullptr : Stack.back(); }
  // Draws every modal of All that is open or still fading, in the given order.
  void Draw(const Frame& F, std::initializer_list<ModalBase*> All) const {
    for (ModalBase* M : All)
      if (M->Visible(F.Now))
        M->Draw(F);
  }

private:
  Interaction& In;
  std::vector<ModalBase*> Stack;
  uint64_t Clock = 0;
};

} // namespace ui
