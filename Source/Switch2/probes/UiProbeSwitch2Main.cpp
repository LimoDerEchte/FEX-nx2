// SPDX-License-Identifier: MIT
// EXPERIMENT console probe main (make ui-probe -> build/fex-switch2-uiprobe.nro). Log: sdmc:/switch/fex-uiprobe.log
// (append, one header per run, flushed per line, "ok" or "FAIL <step>: <VkResult or rc>").
#include <switch.h>

#include <cstdio>
#include <cstdlib>

#include "UiProbe.h"
#include "platform/Log.h"

namespace {
constexpr const char* LogPath = "sdmc:/switch/fex-uiprobe.log";
FILE* LogFile;

void Sink(const char* Line) {
  std::printf("%s\n", Line);
  if (LogFile) {
    std::fprintf(LogFile, "%s\n", Line);
    std::fflush(LogFile);
  }
}

// Vulkan or the window failed: show the failure on the normal text console so the user sees it, then wait for +.
void ShowFailureAndWait(const char* Reason) {
  ui::Log("falling back to the text console to show the failure");
  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState Pad;
  padInitializeDefault(&Pad);
  std::printf("FEX UI probe: FAIL\n%s\n\nSee sdmc:/switch/fex-uiprobe.log\nPress + to exit.\n", Reason);
  while (appletMainLoop()) {
    padUpdate(&Pad);
    if (padGetButtonsDown(&Pad) & HidNpadButton_Plus)
      break;
    consoleUpdate(nullptr);
  }
  consoleExit(nullptr);
}
} // namespace

int main(int, char**) {
  LogFile = std::fopen(LogPath, "a");
  ui::SetLogSink(Sink);
  ui::Log("==== %s start (build %s %s) log %s ====", probe::Version, __DATE__, __TIME__, LogFile ? "open ok" : "FAIL open (stdout only)");

  ui::PlatformOptions Options;
  std::unique_ptr<ui::Platform> Platform = ui::CreatePlatform(Options);
  int Rc = 1;
  if (!Platform) {
    ui::Log("FAIL create platform: see the step above");
    Platform.reset();
    ShowFailureAndWait("Could not start Vulkan / the window. The failing step is the last FAIL line in the log.");
  } else {
    Rc = probe::Run(*Platform, probe::Options {});
    ui::Log("step shutdown: releasing platform");
    Platform.reset();
    ui::Log("step shutdown: ok");
  }
  ui::Log("==== %s end rc=%d ====", probe::Version, Rc);
  if (LogFile)
    std::fclose(LogFile);
  return Rc;
}
