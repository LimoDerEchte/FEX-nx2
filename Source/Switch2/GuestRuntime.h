// SPDX-License-Identifier: MIT
#pragma once

#include "GuestElf.h"

#include <switch.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Switch2Guest {
enum class LogLevel { Standard, Detailed, Trace };
extern LogLevel Logging;
extern bool GraphicsActive;

void WriteLog(const char* Text);
void PrintStage(const char* Text);
void WriteRunCheckpoint(const char* Text);
void BeginGuestLog();
void EndGuestLog();

struct DirectoryEntry {
  std::string Name;
  bool IsDirectory;
};
const char* ReadElf(const char* Path, ElfImage& Image);
Result ReadDirectory(const std::string& Folder, std::vector<DirectoryEntry>& Entries);

std::optional<uint64_t> RunGuest(uint64_t Entry, std::span<const Segment> Segments, const std::string& Folder = "/",
                                 std::string_view Program = "probe", std::span<const std::string> Arguments = {},
                                 const ElfImage* ProgramImage = nullptr, uint64_t InterpreterBase = 0,
                                 const std::string& WorkingDirectory = "/", bool SingleBlock = false, bool Interactive = false);
} // namespace Switch2Guest
