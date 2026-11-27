// SPDX-License-Identifier: MIT
#include <CodeEmitter/Emitter.h>
#include <FEXCore/Utils/Switch2/JitMemory.h>

#include <switch.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>

// The launcher UI sets this to show the probe's lines in a terminal tab; null = the text console (printf).
void (*Switch2JitProbeOutput)(const char* Line) = nullptr;

namespace {
void Say(const char* Format, ...) __attribute__((format(printf, 1, 2)));
void Say(const char* Format, ...) {
  char Line[160];
  va_list Args;
  va_start(Args, Format);
  std::vsnprintf(Line, sizeof(Line), Format, Args);
  va_end(Args);
  if (Switch2JitProbeOutput) Switch2JitProbeOutput(Line);
  else std::printf("%s", Line);
}

bool CheckResult(const char* Operation, Result Status) {
  if (R_SUCCEEDED(Status)) {
    return true;
  }

  Say("%s failed (0x%08x)\n", Operation, Status);
  return false;
}

bool EmitAndCheck(FEXCore::Switch2::JitMemory& Code, uint16_t Expected, bool Sparse) {
  if (!Sparse && !CheckResult("jitTransitionToWritable", Code.BeginWrite())) {
    return false;
  }

  ARMEmitter::Emitter Emitter;
  Emitter.SetBuffer(Code.Writable(), Code.Size(), Code.Executable());
  Emitter.nop();
  const size_t EntryOffset = Emitter.GetCursorOffset();
  const auto Entry = Emitter.GetExecutableCursorAddress<uint32_t (*)()>();
  Emitter.movz(ARMEmitter::Size::i32Bit, ARMEmitter::Reg::r0, Expected);
  Emitter.ret();

  const Result Status = Sparse ? Code.PublishRange(EntryOffset, Emitter.GetCursorOffset() - EntryOffset) : Code.Publish();
  if (!CheckResult(Sparse ? "kaihenJitPublishRanges" : "jitTransitionToExecutable", Status)) {
    return false;
  }

  const uint32_t Actual = Entry();
  Say("Expected %u, got %u\n", static_cast<unsigned>(Expected), Actual);
  return Actual == Expected;
}

} // namespace

bool RunSwitch2JitProbe() {
  for (const uint16_t Expected : {42, 43}) {
    FEXCore::Switch2::JitMemory Code;
    if (!CheckResult("jitCreate", Code.Create(0x1000))) {
      return false;
    }
    const bool Passed = EmitAndCheck(Code, 7, false) && EmitAndCheck(Code, Expected, true);
    if (!CheckResult("jitClose", Code.Close()) || !Passed) {
      return false;
    }
  }
  return true;
}

#ifndef FEX_SWITCH2_LAUNCHER
int main() {
  consoleInit(nullptr);

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState Pad {};
  padInitializeDefault(&Pad);

  std::puts("FEX ARM64 code emitter");
  std::puts("Press A to run the JIT probe, or + to exit.");

  while (appletMainLoop()) {
    padUpdate(&Pad);
    const auto Buttons = padGetButtonsDown(&Pad);
    if (Buttons & HidNpadButton_Plus) {
      break;
    }
    if (Buttons & HidNpadButton_A) {
      std::puts("Starting JIT probe...");
      consoleUpdate(nullptr);
      std::puts(RunSwitch2JitProbe() ? "JIT probe passed." : "JIT probe failed.");
    }
    consoleUpdate(nullptr);
  }

  consoleExit(nullptr);
  return 0;
}
#endif
