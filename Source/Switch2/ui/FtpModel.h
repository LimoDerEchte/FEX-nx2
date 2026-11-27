// SPDX-License-Identifier: MIT
// The FTP screen's model (no drawing): the stored credentials and their validation, the 1024..65535 port range, the first-run
// random password, the transfer-speed smoothing (1 Hz samples, 3 s window) and the text the screen shows. Keys live in
// sdmc:/fex/config/ui.json (ftp_enabled, ftp_user, ftp_pass, ftp_port, ftp_root, ftp_background); other keys are kept.
// No Vulkan or libnx types here.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "Backend.h"
#include "SettingsSchema.h"

namespace ui {

inline constexpr int FtpPortMin = 1024, FtpPortMax = 65535, FtpDefaultPort = 1337;
inline constexpr size_t FtpPasswordLength = 8;
inline constexpr const char* FtpDefaultUser = "switch";
inline constexpr const char* FtpDefaultRoot = "/";

// Each returns true when the value is acceptable; otherwise Why says what is wrong (shown in the HintBar).
bool ValidFtpUser(const std::string& V, std::string* Why = nullptr);       // 1..32 printable ASCII, no spaces
bool ValidFtpPassword(const std::string& V, std::string* Why = nullptr);   // 1..64 printable ASCII, no leading or trailing space
bool ParseFtpPort(const std::string& Text, int& Port, std::string* Why = nullptr);  // decimal digits only, 1024..65535
// Eight characters from letters and digits without look-alikes (no 0 O 1 l I). Rand returns uniform 32-bit values.
std::string RandomFtpPassword(const std::function<uint32_t()>& Rand);

struct FtpSettings {
  bool Enabled = false;
  std::string User = FtpDefaultUser;
  std::string Password;
  int Port = FtpDefaultPort;
  std::string Root = FtpDefaultRoot;
  bool Background = false;
  bool PasswordGenerated = false;  // Load made a new password: Save writes it

  // Reads ui.json. Missing or invalid values fall back to the defaults (a bad value is replaced, never half used); an empty
  // password becomes a random one. A corrupt file leaves the defaults and the screen's Save replaces it.
  static FtpSettings Load(Storage& S, const SettingsPaths& P, const std::function<uint32_t()>& Rand);
  // Re-reads ui.json and sets only the ftp_* keys. false and Error on a write failure.
  bool Save(Storage& S, const SettingsPaths& P, std::string& Error) const;
  FtpConfig ToConfig() const;
};

// Per-client and total speeds from the snapshots: a sample every second or later, speed = bytes over the samples of the last
// 3 s (at least two samples; one sample reads 0). Bytes counted into a client only count while that client is listed.
class FtpRates {
public:
  static constexpr uint64_t SampleMs = 1000, WindowMs = 3000;
  struct Speed {
    double DownBps = 0;  // bytes per second that came to the console
    double UpBps = 0;    // bytes per second that left it
  };
  void Reset();
  // Call every frame; samples when a second passed since the last sample.
  void Update(uint64_t NowMs, const FtpSnapshot& S);
  Speed Of(uint64_t ClientId) const;
  Speed Total() const { return TotalSpeed; }
  size_t SampleCount() const { return Samples.size(); }

private:
  struct Sample {
    uint64_t T = 0;
    uint64_t TotalIn = 0, TotalOut = 0;
    std::map<uint64_t, std::pair<uint64_t, uint64_t>> PerClient;
  };
  std::vector<Sample> Samples;
  std::map<uint64_t, Speed> ClientSpeed;
  Speed TotalSpeed;
};

// "12.4 MB/s", "0 B/s" (decimal units like the card sizes elsewhere: KB MB GB with one decimal from KB up).
std::string FormatRate(double BytesPerSecond);
// "Receiving name.bin · 61%", "Sending name.bin", "Idle". The percent shows only when the size is known.
std::string FtpActivityText(const FtpClientView& C);
// Hint bar text while running: "2 clients · ↓ 1.2 MB/s · ↑ 0 B/s".
std::string FtpSummaryText(size_t Clients, FtpRates::Speed Total);
// "ftp://192.168.1.42:1337", or "Server is off".
std::string FtpAddressText(const FtpSnapshot& S, bool Enabled);
// The password tile text: eight bullets, or the password.
std::string MaskedPassword(const std::string& Password, bool Show);

} // namespace ui
