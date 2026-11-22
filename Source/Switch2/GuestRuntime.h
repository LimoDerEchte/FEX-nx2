// SPDX-License-Identifier: MIT
#pragma once

#include "GuestElf.h"
#include "GuestFtp.h"

#include <switch.h>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Switch2Guest {
enum class LogLevel { Standard, Detailed, Trace };
extern LogLevel Logging;
extern bool GraphicsActive;

void WriteLog(const char* Text);
void WriteGuestOutput(const char* Text);
void PrintStage(const char* Text);
void WriteRunCheckpoint(const char* Text);
void BeginGuestLog();
void EndGuestLog();

struct DirectoryEntry {
  std::string Name;
  bool IsDirectory;
  bool IsSymlink {};
  uint8_t SpecialType {};
};
class PackedFileExtents;
const char* ReadElf(const char* Path, ElfImage& Image, uint64_t Limit = UINT64_MAX,
                    const PackedFileExtents* Extents = nullptr);
Result ReadDirectory(const std::string& Folder, std::vector<DirectoryEntry>& Entries);
const char* ReadRootfsElf(std::string_view Name, ElfImage& Image, ElfImage& Interpreter);
Result ReadRootfsDirectory(const std::string& Folder, std::vector<DirectoryEntry>& Entries);

// Sets the user and group IDs of the next launched guest process, as a
// login or sudo would. std::nullopt restores the default unprivileged user.
void SetLaunchCredentials(std::optional<std::pair<uint32_t, uint32_t>> Credentials);

// Lets the next launch interrupt the filesystem recovery probe's journaled
// move into /.fex-recovery-test. Set from a "<program>.recovery-test" file.
void SetRecoveryTestLaunch(bool Enabled);
// Linux file operations on the guest root filesystem for the FTP server,
// with the same semantics guest programs see. Null if guest memory for the
// transfer buffers could not be reserved.
std::unique_ptr<FtpFileSystem> OpenGuestFileSystem();

std::optional<uint64_t> RunGuest(uint64_t Entry, std::span<const Segment> Segments, const std::string& Folder = "/",
                                 std::string_view Program = "probe", std::span<const std::string> Arguments = {},
                                 const ElfImage* ProgramImage = nullptr, uint64_t InterpreterBase = 0,
                                 const std::string& WorkingDirectory = "/", bool SingleBlock = false, bool Interactive = false);
} // namespace Switch2Guest
