// SPDX-License-Identifier: MIT
// Linux host tool (make ui-host): renders a screen headless on the host Vulkan backend and writes a PNG.
//   fex-ui-host --headless --screen swatch|gallery|home|settings|about|filepicker --theme dark|light [--accent cyan|red|violet|blue] --size 1920x1080 --out file.png
// Screens other than swatch: --script "D,D,R,A" (button presses, one per 3 frames: U D L R A B X Y l r z Z P M; l r = L R, z Z = ZL ZR, P M = + -),
// --scroll N (du, gallery and settings), --modal NAME (gallery: picker|confirm|destructive; home: exit; settings/about: restore|wipe|enum|license|text),
// --section N (settings: select section N; about = last), --picker file|folder|any, (script key "." waits 3 frames), --path P (picker start value), --many N (N files in sdmc:/roms),
// --slow N (folders stall N frames), --frames N. The screens run on FakeBackend (host only).
// Task 6 screens: ftp (--ftp running|stopped|nonet|clients), rootfs (--reinstall, --space low, --modal reinstall), install (--install running|done|failed|cancel),
// shell (--shell prompt|tabs|exit, --modal exit). running/cancel installs capture the frame at pump 330 (about 5 s in).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "DrawContext.h"
#include "platform/Log.h"
#include "platform/host/HostPlatform.h"
#include "platform/host/PngWriter.h"
#include "backend/FakeBackend.h"
#include "screens/filepicker.h"
#include "screens/gallery.h"
#include "screens/ftp.h"
#include "screens/home.h"
#include "screens/install.h"
#include "screens/rootfs_setup.h"
#include "screens/shell.h"
#include "screens/settings.h"
#include "screens/swatch.h"

int main(int Argc, char** Argv) {
  const char* OutPath = nullptr;
  std::string Screen = "swatch";
  ui::ThemeMode Mode = ui::ThemeMode::Dark;
  ui::AccentPreset Accent = ui::AccentPreset::Cyan;
  int W = 1280, H = 720, Frames = 2;
  std::string Script;
  float ScrollTo = 0;
  std::string Modal, PickerKind = "file", PickerPath;
  int Section = -1, Many = 0, Slow = 0;
  std::string FtpMode = "clients", InstallMode = "running", ShellMode = "prompt", Space;
  bool Reinstall = false;
  for (int I = 1; I < Argc; ++I) {
    const std::string A = Argv[I];
    auto Next = [&]() -> const char* { return I + 1 < Argc ? Argv[++I] : ""; };
    if (A == "--headless") {
    } else if (A == "--out") {
      OutPath = Next();
    } else if (A == "--screen") {
      Screen = Next();
    } else if (A == "--theme") {
      const std::string V = Next();
      if (V == "dark")
        Mode = ui::ThemeMode::Dark;
      else if (V == "light")
        Mode = ui::ThemeMode::Light;
      else {
        std::fprintf(stderr, "bad --theme %s\n", V.c_str());
        return 2;
      }
    } else if (A == "--accent") {
      if (!ui::ParseAccent(Next(), Accent)) {
        std::fprintf(stderr, "bad --accent\n");
        return 2;
      }
    } else if (A == "--size") {
      if (std::sscanf(Next(), "%dx%d", &W, &H) != 2 || W <= 0 || H <= 0) {
        std::fprintf(stderr, "bad --size\n");
        return 2;
      }
    } else if (A == "--frames") {
      Frames = std::atoi(Next());
    } else if (A == "--script") {
      Script = Next();
    } else if (A == "--modal") {
      Modal = Next();
    } else if (A == "--section") {
      Section = std::atoi(Next());
    } else if (A == "--picker") {
      PickerKind = Next();
    } else if (A == "--path") {
      PickerPath = Next();
    } else if (A == "--many") {
      Many = std::atoi(Next());
    } else if (A == "--slow") {
      Slow = std::atoi(Next());
    } else if (A == "--ftp") {
      FtpMode = Next();
    } else if (A == "--install") {
      InstallMode = Next();
    } else if (A == "--shell") {
      ShellMode = Next();
    } else if (A == "--space") {
      Space = Next();
    } else if (A == "--reinstall") {
      Reinstall = true;
    } else if (A == "--scroll") {
      ScrollTo = float(std::atof(Next()));
    } else {
      std::fprintf(stderr, "unknown argument %s\n", A.c_str());
      return 2;
    }
  }
  static const char* const Known[] = {"swatch", "gallery", "home", "settings", "about", "filepicker", "ftp", "rootfs", "install", "shell"};
  if (std::find_if(std::begin(Known), std::end(Known), [&](const char* K) { return Screen == K; }) == std::end(Known)) {
    std::fprintf(stderr, "unknown --screen %s (known: swatch, gallery, home, settings, about, filepicker, ftp, rootfs, install, shell)\n", Screen.c_str());
    return 2;
  }

  ui::PlatformOptions Options;
  Options.Width = W;
  Options.Height = H;
  std::unique_ptr<ui::HostPlatform> P = ui::CreateHostPlatform(Options);
  if (!P) {
    ui::Log("FAIL create platform");
    return 1;
  }
  int Rc = 0;
  {
    ui::Renderer& R = P->GetRenderer();
    ui::Fonts Fonts(R);
    ui::Icons Icons(R);
    if (!Fonts.Ok()) {
      ui::Log("FAIL fonts");
      return 1;
    }
    ui::SetTheme(Mode, Accent);
    ui::FakeBackend Backend;
    ui::SettingsModel Model;
    std::unique_ptr<ui::Screen> Scr;
    ui::Gallery* Gal = nullptr;
    ui::SettingsScreen* Settings = nullptr;
    ui::ShellScreen* Shell = nullptr;
    std::vector<char> Keys;
    for (char Ch : Script)
      if (Ch != ',' && Ch != ' ')
        Keys.push_back(Ch);
    if (Screen != "swatch") {
      if (Many > 0)
        for (int K = 0; K < Many; ++K) {
          char Name[48];
          std::snprintf(Name, sizeof(Name), "sdmc:/roms/game-%05d.nes", K);
          Backend.Fs().AddFile(Name, uint64_t(40000 + K * 17), 1700000000 + K * 3600);
        }
      Backend.Fs().PendingFrames = Slow;
      if (Screen == "gallery") {
        auto G = std::make_unique<ui::Gallery>();
        Gal = G.get();
        Scr = std::move(G);
      } else if (Screen == "home") {
        auto H = std::make_unique<ui::HomeScreen>(Backend);
        Scr = std::move(H);
      } else if (Screen == "settings" || Screen == "about") {
        auto St = std::make_unique<ui::SettingsScreen>(Backend, Model);
        Settings = St.get();
        Scr = std::move(St);
      } else if (Screen == "ftp") {
        Backend.Address = FtpMode == "nonet" ? "" : "192.168.1.42";
        Backend.LastFtpConfig.Port = 1337;
        if (FtpMode == "clients" || FtpMode == "running") {
          Backend.FtpUp = true;
          Backend.Ftp_.Running = true;
        }
        if (FtpMode == "clients") {
          ui::FtpClientView A;
          A.Id = 1;
          A.Address = "192.168.1.20";
          A.User = "switch";
          A.LoggedIn = true;
          A.State = ui::FtpClientView::Act::Receiving;
          A.File = "/home/fex/Games/save-data.tar";
          A.Done = 61 << 20;
          A.Total = 100 << 20;
          ui::FtpClientView B2;
          B2.Id = 2;
          B2.Address = "192.168.1.31";
          B2.User = "switch";
          B2.LoggedIn = true;
          B2.State = ui::FtpClientView::Act::Sending;
          B2.File = "/usr/share/doc/readme.txt";
          B2.Done = 4 << 20;
          B2.Total = 90 << 20;
          ui::FtpClientView C3;
          C3.Id = 3;
          C3.Address = "192.168.1.55";
          C3.User = "switch";
          C3.LoggedIn = true;
          Backend.FtpClients = {A, B2, C3};
        }
        Scr = std::make_unique<ui::FtpScreen>(Backend, ui::SettingsPaths {}, []() { return 0x9E3779B9u; });
      } else if (Screen == "rootfs") {
        Backend.Root.Installed = Reinstall;
        if (Space == "low")
          Backend.Fs().SetFree("sdmc:/", 1200ull << 20, 127865454592ull);
        Scr = std::make_unique<ui::RootfsSetupScreen>(Backend, Reinstall);
      } else if (Screen == "install") {
        ui::InstallRequest Rq;
        Rq.Image = *ui::BuiltInRootfsIndex().Find("ubuntu", "24.04");
        Backend.PumpsPerStep = 80;
        if (InstallMode == "failed")
          Backend.InstallFailsAtStep = 3;
        Scr = std::make_unique<ui::InstallScreen>(Backend, Rq);
      } else if (Screen == "shell") {
        auto Sh = std::make_unique<ui::ShellScreen>(Backend, false);
        Shell = Sh.get();
        if (ShellMode == "tabs") {
          Sh->NewTab(false);
          Sh->NewTab(true);
          Sh->SelectTab(0);
        }
        Scr = std::move(Sh);
      } else {
        ui::PickerSpec Spec;
        Spec.Title = PickerKind == "folder" ? "Choose the start folder" : "Choose RootFS image";
        Spec.Accept = PickerKind == "folder" || PickerKind == "any" ? std::vector<std::string> {} : std::vector<std::string> {".sqsh", ".ero"};
        Spec.Folder = PickerKind == "folder";
        Spec.Current = PickerPath.empty() ? (PickerKind == "folder" ? "/fex/games" : "sdmc:/fex/rootfs/Ubuntu_24_04.sqsh") : PickerPath;
        Scr = std::make_unique<ui::FilePickerScreen>(Backend, Spec, [](const std::string&) {});
      }
      Frames = std::max(Frames, int(Keys.size()) * 3 + 12);
      if (Screen == "ftp" && FtpMode == "clients")
        Frames = std::max(Frames, 260);  // the speeds need two samples a second apart
    }
    auto WritePngNow = [&](const char* Path) -> bool {
      std::vector<uint8_t> Pixels;
      ui::Size Sz;
      if (!P->GetDisplay().ReadPixels(Pixels, Sz) || !ui::WritePng(Path, Pixels, Sz.W, Sz.H)) {
        ui::Log("FAIL write PNG %s", Path);
        return false;
      }
      ui::Log("step png: ok %s (%dx%d)", Path, Sz.W, Sz.H);
      return true;
    };
    // One frame: update with the events, draw, present. false when the frame could not be made.
    auto DoFrame = [&](int Frame, const std::vector<ui::Event>& Ev) -> bool {
      if (Scr) {
        const uint64_t Now = 1000 + uint64_t(Frame) * 16;
        if (Screen == "ftp" && Backend.FtpUp) {  // a running server: counters grow at 1.2 MB/s in, 0.4 MB/s out
          Backend.FtpTotalIn = uint64_t(Frame) * 20000;
          Backend.FtpTotalOut = uint64_t(Frame) * 6000;
          for (ui::FtpClientView& C : Backend.FtpClients) {
            if (C.State == ui::FtpClientView::Act::Receiving)
              C.BytesIn = uint64_t(Frame) * 20000;
            if (C.State == ui::FtpClientView::Act::Sending)
              C.BytesOut = uint64_t(Frame) * 6000;
          }
        }
        if (Frame == 1 && Shell && Backend.LastShell) {
          const std::string Esc = "\x1b";
          for (size_t K = 0; K < Backend.Shells.size(); ++K)
            Backend.Shells[K]->Feed(Esc + "]0;fex@fex: ~\a" + Esc + "[1;32mfex@fex" + Esc + "[0m:" + Esc + "[1;34m~/Games" + Esc + "[0m$ ls -l\n" +
                                     "total 12\n" + Esc + "[34mdrwxr-xr-x" + Esc + "[0m 2 fex fex 4096 Nov 25 doom\n" + "-rw-r--r-- 1 fex fex  220 Nov 25 notes.txt\n" +
                                     Esc + "[33mwarning:" + Esc + "[0m 3 files skipped\n" + Esc + "[31merror:" + Esc + "[0m nothing\n" + Esc + "[1;32mfex@fex" + Esc + "[0m:" + Esc + "[1;34m~/Games" + Esc +
                                     "[0m$ ");
        }
        if (Frame == 1 && !Modal.empty()) {
          if (Shell && Modal == "exit") {
            Backend.LastShell->Child = true;
            Shell->ExitToHome();
          } else if (Shell && Modal == "help") {
            Shell->OpenHelp();
          } else if (Screen == "rootfs" && Modal == "reinstall") {
            static_cast<ui::RootfsSetupScreen*>(Scr.get())->PressInstall();
          } else if (Screen == "ftp" && Modal == "disconnect") {
            static_cast<ui::FtpScreen*>(Scr.get())->AskDisconnect(0);
          }
        }
        if (Frame == 0 && Settings) {
          if (Screen == "about") {
            Settings->SelectSection(Settings->SectionCount(), Now, false);
            Settings->Input().SetFocus(Settings->Item(Settings->SectionCount()).Id, Now);
          }
          else if (Section >= 0) {
            Settings->SelectSection(Section, Now, true);
            Settings->Input().SetFocus(Settings->Item(Section).Id, Now);
          }
        }
        if (Frame == 1 && !Modal.empty()) {
          if (Gal) {
            if (Modal == "picker")
              Gal->OpenPicker(Now);
            else
              Gal->OpenConfirm(Modal == "destructive", Now);
          } else if (Settings) {
            if (Modal == "restore") {
              Settings->OpenRestoreConfirm();
            } else if (Modal == "wipe" || Modal == "enum") {
              if (ui::SettingsRow* R = Settings->RowFor(Modal == "wipe" ? "wipe_rootfs" : "theme"))
                Settings->Input().SetFocus(R->Id, Now);
              std::vector<ui::Event> A;
              ui::Event E;
              E.Kind = ui::Event::Type::ButtonDown;
              E.Btn = ui::Button::A;
              A.push_back(E);
              E.Kind = ui::Event::Type::ButtonUp;
              A.push_back(E);
              Settings->Update(Now, P->GetRenderer().Scale(), A);
            } else if (Modal == "license" || Modal == "text") {
              if (Settings->LicenseRowCount() > 1) {
                ui::LicenseRow* R = Settings->LicRow(1);
                Settings->Input().SetFocus(R->Id, Now);
                std::vector<ui::Event> A;
                ui::Event E;
                E.Kind = ui::Event::Type::ButtonDown;
                E.Btn = ui::Button::A;
                A.push_back(E);
                E.Kind = ui::Event::Type::ButtonUp;
                A.push_back(E);
                Settings->Update(Now, P->GetRenderer().Scale(), A);
                if (Modal == "text" && Settings->LicModal().ReadPill().Enabled)
                  Settings->LicModal().ReadPill().Activate(Now);
              }
            }
          }
        }
        if (Frame == 1 && ScrollTo > 0) {
          if (Gal)
            Gal->Scrolling().To(ScrollTo, Now, 0);
          else if (Settings)
            Settings->Scrolling().To(ScrollTo, Now, 0);
        }
        Scr->Update(Now, P->GetRenderer().Scale(), Ev);
      }
      ui::Display& D = P->GetDisplay();
      const ui::Theme& T = ui::CurrentTheme();
      if (!D.BeginFrame() || !R.BeginFrame(T.Bg)) {
        ui::Log("FAIL frame %d could not begin", Frame);
        return false;
      }
      ui::DrawContext C {R, Fonts, Icons, T, R.Scale()};
      if (Scr)
        Scr->Draw(C, 1000 + uint64_t(Frame) * 16);
      else
        ui::DrawSwatch(C);
      Fonts.EndFrame();
      Icons.EndFrame();
      if (!R.EndFrame() || !D.Present()) {
        ui::Log("FAIL frame %d could not present", Frame);
        return false;
      }
      return true;
    };
    auto KeyEvents = [&](int Frame) {
      std::vector<ui::Event> Ev;
      if (Frame >= 2 && (Frame - 2) % 3 == 0 && size_t((Frame - 2) / 3) < Keys.size()) {
        static const char* Names = "UDLRABXYlrzZPM";
        static const ui::Button Map[] = {ui::Button::Up, ui::Button::Down, ui::Button::Left, ui::Button::Right, ui::Button::A, ui::Button::B,
                                         ui::Button::X, ui::Button::Y, ui::Button::L, ui::Button::R, ui::Button::ZL, ui::Button::ZR,
                                         ui::Button::Plus, ui::Button::Minus};
        const char* At = std::strchr(Names, Keys[size_t((Frame - 2) / 3)]);
        if (At && *At) {
          ui::Event E;
          E.Kind = ui::Event::Type::ButtonDown;
          E.Btn = Map[At - Names];
          Ev.push_back(E);
          E.Kind = ui::Event::Type::ButtonUp;
          Ev.push_back(E);
        }
      }
      return Ev;
    };
    if (Screen == "install") {
      // The installer is cooperative: Run() blocks and every Pump draws a frame, as the app does. running/cancel capture at pump 330.
      auto* Ins = static_cast<ui::InstallScreen*>(Scr.get());
      int Fi = 0;
      bool Captured = false;
      bool Failed = false;
      Ins->PumpHook = [&]() {
        std::vector<ui::Event> Ev = KeyEvents(Fi);
        if (InstallMode == "cancel" && Fi == 300) {  // B opens "Cancel installation?"
          ui::Event E;
          E.Kind = ui::Event::Type::ButtonDown;
          E.Btn = ui::Button::B;
          Ev.push_back(E);
          E.Kind = ui::Event::Type::ButtonUp;
          Ev.push_back(E);
        }
        if (!DoFrame(Fi, Ev))
          Failed = true;
        ++Fi;
        if (Fi == 330 && (InstallMode == "running" || InstallMode == "cancel") && OutPath && !Captured) {
          Captured = true;
          if (!WritePngNow(OutPath))
            Failed = true;
        }
      };
      if (InstallMode == "cancel") {
        // After the capture the cancel is not confirmed: choose "Keep installing" so the run ends.
      }
      Ins->Run();
      Frames = 0;
      if (InstallMode == "done" || InstallMode == "failed") {
        for (int K = 0; K < 30 && !Failed; ++K)
          Failed = !DoFrame(Fi++, {});
        Frames = 0;
      }
      if (Failed)
        Rc = 1;
      if (Captured)
        OutPath = nullptr;
    }
    for (int Frame = 0; Frame < Frames && Rc == 0; ++Frame)
      if (!DoFrame(Frame, KeyEvents(Frame))) {
        Rc = 1;
        break;
      }
    const ui::FontsStats FS = Fonts.Stats();
    const ui::IconsStats IS = Icons.Stats();
    ui::Log("fonts: %d atlases, %d pages, %d glyphs; icons: %d cached on %d pages", FS.Atlases, FS.Pages, FS.Glyphs, IS.Cached, IS.Pages);
    if (OutPath && !WritePngNow(OutPath))
      Rc = 1;
  }
  P.reset();
  return Rc;
}
