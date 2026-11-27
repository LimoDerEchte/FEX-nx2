// SPDX-License-Identifier: MIT
#include "InstallModel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

void InstallProgress::Start(uint64_t NowMs, int StepCount) {
  Steps = StepCount;
  StartMs = LastMs = NowMs;
  Frac = 0;
  Step = -1;
  Title.clear();
  Done = false;
}

void InstallProgress::SetStep(int Index, const std::string& T) {
  Step = Index;
  Title = T;
}

void InstallProgress::SetFraction(double F, uint64_t NowMs) {
  F = std::clamp(F, 0.0, 1.0);
  if (F < Frac)
    F = Frac;  // never backwards
  Frac = F;
  LastMs = std::max(LastMs, NowMs);
}

void InstallProgress::Finish(uint64_t NowMs) {
  Frac = 1;
  Done = true;
  LastMs = std::max(LastMs, NowMs);
}

int InstallProgress::Percent() const {
  if (Done)
    return 100;
  return std::min(99, int(std::floor(Frac * 100.0 + 1e-9)));
}

std::string InstallProgress::StepText() const {
  return "Step " + std::to_string(std::max(0, Step) + 1) + " of " + std::to_string(Steps);
}

bool InstallProgress::Eta(uint64_t NowMs, double& Seconds) const {
  if (Done || Frac <= 0 || NowMs < StartMs + EtaWarmupMs)
    return false;
  // The whole elapsed time, up to now (not only to the last report): a stall makes the estimate grow instead of freezing it.
  const double Elapsed = double(std::max(NowMs, LastMs) - StartMs) / 1000.0;
  Seconds = (1.0 - Frac) * Elapsed / Frac;
  return true;
}

std::string FormatEta(double S) {
  char B[48];
  if (S < 10)
    return "a few seconds";
  if (S < 60) {
    std::snprintf(B, sizeof(B), "about %d s", int(std::lround(S / 5.0) * 5));
    return B;
  }
  if (S < 3600) {
    std::snprintf(B, sizeof(B), "about %d min", int(std::lround(S / 60.0)));
    return B;
  }
  const int Min = int(std::lround(S / 60.0));
  std::snprintf(B, sizeof(B), "about %d h %02d min", Min / 60, Min % 60);
  return B;
}

std::string InstallProgress::EtaText(uint64_t NowMs) const {
  double S = 0;
  if (!Eta(NowMs, S))
    return "estimating\xE2\x80\xA6";
  return FormatEta(S) + " left";
}

std::string InstallProgress::FooterText(uint64_t NowMs) const {
  if (Done)
    return StepText();
  return StepText() + " \xC2\xB7 " + EtaText(NowMs);
}

std::string FormatStamp(uint64_t Ms) {
  char B[32];
  std::snprintf(B, sizeof(B), "[%02llu:%02llu.%02llu]", (unsigned long long)(Ms / 60000), (unsigned long long)((Ms / 1000) % 60), (unsigned long long)((Ms / 10) % 100));
  return B;
}

std::vector<LogSpan> InstallLogSpans(uint64_t Ms, InstallLogKind Kind, const std::string& Line) {
  std::vector<LogSpan> Out;
  Out.push_back({LogSpan::Role::Stamp, FormatStamp(Ms) + " "});
  switch (Kind) {
  case InstallLogKind::Ok: Out.push_back({LogSpan::Role::Ok, "ok "}); break;
  case InstallLogKind::Warn: Out.push_back({LogSpan::Role::Warn, "warn: "}); break;
  case InstallLogKind::Error: Out.push_back({LogSpan::Role::Error, "error: "}); break;
  case InstallLogKind::Info: break;
  }
  Out.push_back({LogSpan::Role::Text, Line});
  return Out;
}

uint64_t InstallSpaceNeeded(const RootfsImage& I, bool Local, bool Extras) {
  const uint64_t Installed = I.InstalledBytes ? I.InstalledBytes : I.DownloadBytes * 4;
  uint64_t Need = Installed + Installed / 20 + (Extras ? (64ull << 20) : 0);
  if (!Local)
    Need += I.DownloadBytes;
  return Need;
}

uint64_t InstallDownloadBytes(const RootfsImage& I, bool Local, bool Extras) {
  return (Local ? 0 : I.DownloadBytes) + (Extras ? I.ExtraDownloadBytes : 0);
}

} // namespace ui
