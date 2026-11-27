// SPDX-License-Identifier: MIT
#include "FtpModel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "Json.h"

namespace ui {

namespace {
bool Printable(const std::string& V) {
  for (unsigned char C : V)
    if (C < 0x20 || C > 0x7E)
      return false;
  return true;
}
void Why_(std::string* W, const char* Text) {
  if (W)
    *W = Text;
}
} // namespace

bool ValidFtpUser(const std::string& V, std::string* Why) {
  if (V.empty()) {
    Why_(Why, "The user name cannot be empty");
    return false;
  }
  if (V.size() > 32) {
    Why_(Why, "The user name is limited to 32 characters");
    return false;
  }
  if (!Printable(V) || V.find(' ') != std::string::npos) {
    Why_(Why, "The user name may use letters, digits and symbols, no spaces");
    return false;
  }
  return true;
}

bool ValidFtpPassword(const std::string& V, std::string* Why) {
  if (V.empty()) {
    Why_(Why, "The password cannot be empty");
    return false;
  }
  if (V.size() > 64) {
    Why_(Why, "The password is limited to 64 characters");
    return false;
  }
  if (!Printable(V) || V.front() == ' ' || V.back() == ' ') {
    Why_(Why, "The password may use letters, digits and symbols, no space at the ends");
    return false;
  }
  return true;
}

bool ParseFtpPort(const std::string& Text, int& Port, std::string* Why) {
  if (Text.empty() || Text.size() > 5 || !std::all_of(Text.begin(), Text.end(), [](unsigned char C) { return C >= '0' && C <= '9'; })) {
    Why_(Why, "Enter the port as a number from 1024 to 65535");
    return false;
  }
  const int V = std::atoi(Text.c_str());
  if (V < FtpPortMin || V > FtpPortMax) {
    Why_(Why, "The port must be from 1024 to 65535");
    return false;
  }
  Port = V;
  return true;
}

std::string RandomFtpPassword(const std::function<uint32_t()>& Rand) {
  static const char Alphabet[] = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  constexpr uint32_t N = sizeof(Alphabet) - 1;
  const uint32_t Limit = 0xFFFFFFFFu - (0xFFFFFFFFu % N);  // rejection sampling: no modulo bias
  std::string Out;
  while (Out.size() < FtpPasswordLength) {
    const uint32_t R = Rand();
    if (R < Limit)
      Out.push_back(Alphabet[R % N]);
  }
  return Out;
}

// ---- Settings -----------------------------------------------------------------------------------------------------------

FtpSettings FtpSettings::Load(Storage& S, const SettingsPaths& P, const std::function<uint32_t()>& Rand) {
  FtpSettings F;
  std::string Text;
  Json J;
  std::string Err;
  if (S.ReadFile(P.UiJson, Text) && Json::Parse(Text, J, Err) && J.IsObject()) {
    if (const Json* V = J.Find("ftp_enabled"))
      F.Enabled = V->AsBool(false);
    if (const Json* V = J.Find("ftp_user"))
      if (V->IsString() && ValidFtpUser(V->AsString()))
        F.User = V->AsString();
    if (const Json* V = J.Find("ftp_pass"))
      if (V->IsString() && ValidFtpPassword(V->AsString()))
        F.Password = V->AsString();
    if (const Json* V = J.Find("ftp_port")) {
      int Port = 0;
      if ((V->IsNumber() || V->IsString()) && ParseFtpPort(V->AsString(), Port))
        F.Port = Port;
    }
    if (const Json* V = J.Find("ftp_root"))
      if (V->IsString() && !V->AsString().empty() && V->AsString()[0] == '/')
        F.Root = V->AsString();
    if (const Json* V = J.Find("ftp_background"))
      F.Background = V->AsBool(false);
  }
  if (F.Password.empty()) {
    F.Password = RandomFtpPassword(Rand);
    F.PasswordGenerated = true;
  }
  return F;
}

bool FtpSettings::Save(Storage& S, const SettingsPaths& P, std::string& Error) const {
  Json J = Json::Object();
  std::string Text, Err;
  if (S.ReadFile(P.UiJson, Text)) {
    Json Old;
    if (Json::Parse(Text, Old, Err) && Old.IsObject())
      J = std::move(Old);
    else
      S.WriteFileAtomic(P.UiJson + ".bad", Text, Err);  // a corrupt ui.json is kept aside, as the settings screen does
  }
  if (!J.Find("version"))
    J.Set("version", Json::Int(1));
  J.Set("ftp_enabled", Json::Bool(Enabled));
  J.Set("ftp_user", Json::String(User));
  J.Set("ftp_pass", Json::String(Password));
  J.Set("ftp_port", Json::Int(Port));
  J.Set("ftp_root", Json::String(Root));
  J.Set("ftp_background", Json::Bool(Background));
  if (!S.WriteFileAtomic(P.UiJson, J.Dump(), Err)) {
    Error = "ui.json: " + Err;
    return false;
  }
  return true;
}

FtpConfig FtpSettings::ToConfig() const {
  FtpConfig C;
  C.User = User;
  C.Password = Password;
  C.Port = uint16_t(Port);
  C.Root = Root;
  C.Background = Background;
  return C;
}

// ---- Rates --------------------------------------------------------------------------------------------------------------

void FtpRates::Reset() {
  Samples.clear();
  ClientSpeed.clear();
  TotalSpeed = {};
}

void FtpRates::Update(uint64_t NowMs, const FtpSnapshot& S) {
  if (!Samples.empty() && NowMs < Samples.back().T + SampleMs)
    return;
  Sample Cur;
  Cur.T = NowMs;
  Cur.TotalIn = S.TotalIn;
  Cur.TotalOut = S.TotalOut;
  for (const FtpClientView& C : S.Clients)
    Cur.PerClient[C.Id] = {C.BytesIn, C.BytesOut};
  Samples.push_back(std::move(Cur));
  while (Samples.size() > 1 && Samples.back().T - Samples[1].T >= WindowMs)
    Samples.erase(Samples.begin());  // keep the oldest sample at or just beyond the window start
  ClientSpeed.clear();
  TotalSpeed = {};
  if (Samples.size() < 2)
    return;
  const Sample& First = Samples.front();
  const Sample& Last = Samples.back();
  const double Dt = double(Last.T - First.T) / 1000.0;
  if (Dt <= 0)
    return;
  auto Delta = [](uint64_t B, uint64_t A) { return B >= A ? double(B - A) : 0.0; };
  TotalSpeed.DownBps = Delta(Last.TotalIn, First.TotalIn) / Dt;
  TotalSpeed.UpBps = Delta(Last.TotalOut, First.TotalOut) / Dt;
  for (const auto& [Id, Now] : Last.PerClient) {
    auto Old = First.PerClient.find(Id);
    const std::pair<uint64_t, uint64_t> Base = Old == First.PerClient.end() ? std::pair<uint64_t, uint64_t> {0, 0} : Old->second;
    ClientSpeed[Id] = {Delta(Now.first, Base.first) / Dt, Delta(Now.second, Base.second) / Dt};
  }
}

FtpRates::Speed FtpRates::Of(uint64_t Id) const {
  auto It = ClientSpeed.find(Id);
  return It == ClientSpeed.end() ? Speed {} : It->second;
}

// ---- Text ---------------------------------------------------------------------------------------------------------------

std::string FormatRate(double Bps) {
  char B[40];
  if (Bps < 1000)
    std::snprintf(B, sizeof(B), "%d B/s", int(std::lround(Bps)));
  else if (Bps < 1e6)
    std::snprintf(B, sizeof(B), "%.1f KB/s", Bps / 1e3);
  else if (Bps < 1e9)
    std::snprintf(B, sizeof(B), "%.1f MB/s", Bps / 1e6);
  else
    std::snprintf(B, sizeof(B), "%.1f GB/s", Bps / 1e9);
  return B;
}

std::string FtpActivityText(const FtpClientView& C) {
  if (C.State == FtpClientView::Act::Idle)
    return C.LoggedIn ? "Idle" : "Not logged in";
  const std::string Name = C.File.empty() ? "" : C.File.substr(C.File.rfind('/') == std::string::npos ? 0 : C.File.rfind('/') + 1);
  std::string Out = std::string(C.State == FtpClientView::Act::Receiving ? "Receiving " : "Sending ") + Name;
  if (C.Total > 0) {
    const int Pct = int(std::min<uint64_t>(100, C.Done * 100 / C.Total));
    Out += " \xC2\xB7 " + std::to_string(Pct) + "%";
  }
  return Out;
}

std::string FtpSummaryText(size_t Clients, FtpRates::Speed Total) {
  return std::to_string(Clients) + (Clients == 1 ? " client" : " clients") + " \xC2\xB7 \xE2\x86\x93 " + FormatRate(Total.DownBps) + " \xC2\xB7 \xE2\x86\x91 " + FormatRate(Total.UpBps);
}

std::string FtpAddressText(const FtpSnapshot& S, bool Enabled) {
  if (!Enabled || !S.Running)
    return "Server is off";
  return "ftp://" + S.Address + ":" + std::to_string(S.Port);
}

std::string MaskedPassword(const std::string& Password, bool Show) {
  if (Show)
    return Password;
  std::string Out;
  for (size_t I = 0; I < Password.size(); ++I)
    Out += "\xE2\x80\xA2";
  return Out;
}

} // namespace ui
