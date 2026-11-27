// SPDX-License-Identifier: MIT
// EXPERIMENT host probe main (make ui-probe-host): the same probe on the headless Vulkan host backend, writing a PNG.
//   fex-uiprobe-host --headless --out probe.png [--size 1920x1080] [--frames 3] [--resize-at N --resize-to WxH] [--log file]
// Scripted input: --press-plus-at N exits at frame N (to test the exit path).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "UiProbe.h"
#include "platform/Log.h"
#include "platform/host/HostPlatform.h"
#include "platform/host/PngWriter.h"

namespace {
FILE* LogFile;

void Sink(const char* Line) {
  std::printf("%s\n", Line);
  if (LogFile) {
    std::fprintf(LogFile, "%s\n", Line);
    std::fflush(LogFile);
  }
}

bool ParseSize(const char* S, int& W, int& H) { return std::sscanf(S, "%dx%d", &W, &H) == 2 && W > 0 && H > 0; }

} // namespace

int main(int Argc, char** Argv) {
  const char* OutPath = nullptr;
  const char* LogPath = nullptr;
  int W = 1280, H = 720, Frames = 3, ResizeAt = -1, ResizeW = 0, ResizeH = 0, PlusAt = -1;
  for (int I = 1; I < Argc; ++I) {
    std::string A = Argv[I];
    auto Next = [&]() -> const char* { return I + 1 < Argc ? Argv[++I] : ""; };
    if (A == "--headless") {
    } else if (A == "--out") OutPath = Next();
    else if (A == "--log") LogPath = Next();
    else if (A == "--size") { if (!ParseSize(Next(), W, H)) { std::fprintf(stderr, "bad --size\n"); return 2; } }
    else if (A == "--frames") Frames = std::atoi(Next());
    else if (A == "--resize-at") ResizeAt = std::atoi(Next());
    else if (A == "--resize-to") { if (!ParseSize(Next(), ResizeW, ResizeH)) { std::fprintf(stderr, "bad --resize-to\n"); return 2; } }
    else if (A == "--press-plus-at") PlusAt = std::atoi(Next());
    else { std::fprintf(stderr, "unknown argument %s\n", A.c_str()); return 2; }
  }
  if (LogPath)
    LogFile = std::fopen(LogPath, "w");
  ui::SetLogSink(Sink);
  ui::Log("==== %s host start ====", probe::Version);

  ui::PlatformOptions Options;
  Options.Width = W;
  Options.Height = H;
  std::unique_ptr<ui::HostPlatform> P = ui::CreateHostPlatform(Options);
  if (!P) {
    ui::Log("FAIL create platform");
    return 1;
  }
  probe::Options O;
  O.MaxFrames = Frames;
  O.FixedFrameMs = 16;
  O.OnFrame = [&](int Frame, ui::Platform&) {
    if (Frame == ResizeAt && ResizeW > 0) {
      ui::Log("host: resize to %dx%d", ResizeW, ResizeH);
      P->SetSize(ResizeW, ResizeH);
    }
    if (Frame == PlusAt) {
      ui::Event E;
      E.Kind = ui::Event::Type::ButtonDown;
      E.Btn = ui::Button::Plus;
      P->ScriptedInput().Push(E);
    }
  };
  int Rc = probe::Run(*P, O);
  if (Rc == 0 && OutPath) {
    std::vector<uint8_t> Pixels;
    ui::Size Sz;
    if (!P->GetDisplay().ReadPixels(Pixels, Sz) || !ui::WritePng(OutPath, Pixels, Sz.W, Sz.H)) {
      ui::Log("FAIL write PNG %s", OutPath);
      Rc = 1;
    } else {
      ui::Log("step png: ok %s (%dx%d)", OutPath, Sz.W, Sz.H);
    }
  }
  P.reset();
  ui::Log("==== %s host end rc=%d ====", probe::Version, Rc);
  if (LogFile)
    std::fclose(LogFile);
  return Rc;
}
