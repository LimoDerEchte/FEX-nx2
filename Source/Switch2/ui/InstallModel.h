// SPDX-License-Identifier: MIT
// The Installation screen's model (no drawing): the overall percentage (the installer reports a fraction already weighted over its
// 7 steps; the bar never moves back), the ETA from the average rate over the whole elapsed time ("estimating..." for the first 3 s), step texts,
// the log line format and the free-space estimate the RootFS setup screen shows. No Vulkan or libnx types here.
#pragma once

#include <cstdint>
#include <string>

#include "Backend.h"

namespace ui {

// The installer's steps (runtime/fs/GuestRootfsInstall.h): 0 Check free space, 1 Download, 2 Verify, 3 Extract the RootFS,
// 4 Set up the RootFS, 5 Extra packages, 6 Finish. The spec shows five; the real count is shown.
inline constexpr int InstallStepCount = 7;
// Step index from which the image's hash check has passed: a cancel keeps the downloaded image from here on.
inline constexpr int FirstStepAfterVerify = 3;

class InstallProgress {
public:
  static constexpr uint64_t EtaWarmupMs = 3000;

  void Start(uint64_t NowMs, int StepCount = InstallStepCount);
  void SetStep(int Index, const std::string& Title);
  void SetFraction(double Fraction, uint64_t NowMs);
  void Finish(uint64_t NowMs);  // bar to 100 %

  double Fraction() const { return Frac; }
  int Percent() const;                       // floor(100 x fraction); 100 only when finished
  int StepIndex() const { return Step; }     // -1 before the first step
  const std::string& StepTitle() const { return Title; }
  std::string StepText() const;              // "Step 4 of 7" (of the count Start was given)
  // Seconds left: the remaining fraction at the average rate since Start (the fraction the installer reports is weighted by the
  // predicted time of each step, the extraction's by bytes and entries). false (estimating) for the first 3 s or while nothing moved.
  bool Eta(uint64_t NowMs, double& Seconds) const;
  // "estimating…" or "about 4 min left".
  std::string EtaText(uint64_t NowMs) const;
  // "Step 4 of 7 · about 4 min left".
  std::string FooterText(uint64_t NowMs) const;
  bool Finished() const { return Done; }

private:
  uint64_t StartMs = 0, LastMs = 0;
  double Frac = 0;
  int Step = -1;
  int Steps = InstallStepCount;
  std::string Title;
  bool Done = false;
};

// "a few seconds", "about 40 s", "about 4 min", "about 1 h 05 min".
std::string FormatEta(double Seconds);
// "[mm:ss.cc]" from elapsed milliseconds (minutes keep counting past 99).
std::string FormatStamp(uint64_t ElapsedMs);

// A log line as the LogView shows it: the dim stamp, then (per kind) "ok " / "warn: " / "error: " in the kind's color, then the text.
struct LogSpan {
  enum class Role : uint8_t { Stamp, Ok, Warn, Error, Text } R = Role::Text;
  std::string Text;
};
std::vector<LogSpan> InstallLogSpans(uint64_t ElapsedMs, InstallLogKind Kind, const std::string& Line);

// Free space the install needs: the same formula as the installer (installed size + 5 %, plus the download unless a local image
// is used, plus 64 MiB of working room for the extra packages).
uint64_t InstallSpaceNeeded(const RootfsImage& Image, bool LocalImage, bool Extras);
// What the install downloads: the image (unless local) and the extra packages' indexes and archives.
uint64_t InstallDownloadBytes(const RootfsImage& Image, bool LocalImage, bool Extras);

} // namespace ui
