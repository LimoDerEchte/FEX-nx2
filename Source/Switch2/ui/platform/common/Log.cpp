// SPDX-License-Identifier: MIT
#include "platform/Log.h"

#include <cstdarg>
#include <cstdio>

namespace ui {
namespace {
LogSink CurrentSink = nullptr;
}

void SetLogSink(LogSink Sink) { CurrentSink = Sink; }

void Log(const char* Format, ...) {
  char Line[512];
  va_list Args;
  va_start(Args, Format);
  std::vsnprintf(Line, sizeof(Line), Format, Args);
  va_end(Args);
  if (CurrentSink)
    CurrentSink(Line);
  else
    std::printf("%s\n", Line);
}

} // namespace ui
