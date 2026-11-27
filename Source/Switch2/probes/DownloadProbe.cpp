// SPDX-License-Identifier: MIT
// EXPERIMENT console probe (not part of the launcher): can the console fetch the FEX RootFS Ubuntu 24.04 image over plain HTTP,
// resume it with a Range request, and how fast are the network and the SD card? Uses the installer's own HTTP client
// (host/HttpClient.h) with the launcher's socket configuration. One result line per case on screen and in the SD log
// sdmc:/switch/fex-downloadprobe.log. Cases:
//   1 nifm address and DNS servers          2 name lookup of rootfs.fex-emu.gg
//   3 Range probe: status 206, Content-Range start and total size, no redirect
//   4 network only: the first 32 MiB discarded (throughput)
//   5 network to SD: the first 64 MiB written to sdmc:/fex/.download/probe.bin with XXH3-64 on the fly (throughput)
//   6 Range resume: bytes 32 MiB.. read again and compared with what case 5 wrote (XXH3-64 of a 16 MiB slice)
//   7 interrupted transfer: the connection is dropped after 8 MiB and the transfer resumes from the received offset
//   8 only with the sidecar sdmc:/switch/fex-downloadprobe.full: the whole image with resume, XXH3-64 must equal the index's
// The probe file is removed at the end. + exits.
#include <switch.h>

#include <sys/stat.h>

#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include "xxhash.h"

#include "HttpClient.h"

namespace {
constexpr const char* Version = "download-probe 1";
constexpr const char* LogPath = "sdmc:/switch/fex-downloadprobe.log";
constexpr const char* FullSidecar = "sdmc:/switch/fex-downloadprobe.full";
constexpr const char* ProbeFile = "sdmc:/fex/.download/probe.bin";
constexpr const char* Url = "http://rootfs.fex-emu.gg/Ubuntu_24_04/2026-08-11/Ubuntu_24_04.sqsh";
constexpr uint64_t ImageBytes = 525615104ull;
constexpr const char* ImageHash = "3517e0e5ea25a473";
constexpr uint64_t MiB = 1ull << 20;

FILE* Log;
int Fails;
PadState Pad;

void Out(const char* Format, ...) {
  char Line[300];
  va_list Args;
  va_start(Args, Format);
  std::vsnprintf(Line, sizeof(Line), Format, Args);
  va_end(Args);
  std::printf("%s\n", Line);
  consoleUpdate(nullptr);
  if (Log) {
    std::fprintf(Log, "%s\n", Line);
    std::fflush(Log);
  }
}

void Report(const char* Case, bool Pass, const char* Detail = "") {
  Out("%s %s %s", Pass ? "PASS" : "FAIL", Case, Detail);
  if (!Pass) ++Fails;
}

bool Quit() {
  padUpdate(&Pad);
  return padGetButtonsDown(&Pad) & HidNpadButton_Plus;
}

double Seconds() { return armTicksToNs(armGetSystemTick()) / 1e9; }

std::string Mbps(uint64_t Bytes, double Elapsed) {
  char Text[64];
  std::snprintf(Text, sizeof(Text), "%.1f MiB in %.1f s = %.2f MiB/s", Bytes / double(MiB), Elapsed, Elapsed > 0 ? Bytes / double(MiB) / Elapsed : 0.0);
  return Text;
}

std::string Hex(uint64_t Value) {
  char Text[20];
  std::snprintf(Text, sizeof(Text), "%016" PRIx64, Value);
  return Text;
}

// Streams bytes [Offset, Offset + Limit) (Limit 0: to the end) to Sink; stops after Limit bytes by cancelling the transfer.
const char* Fetch(uint64_t Offset, uint64_t Limit, const std::function<bool(const uint8_t*, size_t)>& Sink, Switch2Guest::Http::ResponseInfo& Info) {
  uint64_t Got = 0;
  bool Stopped = false;
  // Progress every 2 s (screen and log), so a slow transfer and a hang look different:
  // a hang prints no progress line after "started".
  const double Start = Seconds();
  double LastProgress = Start;
  if (Limit > 1) Out("   started: offset %" PRIu64 ", up to %" PRIu64 " bytes", Offset, Limit);
  const auto* Error = Switch2Guest::Http::Get(
      Url, Offset,
      [&](const uint8_t* Data, size_t Count) {
        const double Now = Seconds();
        if (Now - LastProgress >= 2.0) {
          LastProgress = Now;
          Out("   progress: %.1f MiB after %.0f s (%.2f MiB/s), last read %zu bytes", Got / double(MiB), Now - Start,
              Got / double(MiB) / (Now - Start), Count);
        }
        size_t Take = Count;
        if (Limit && Got + Take > Limit) Take = static_cast<size_t>(Limit - Got);
        if (Take && !Sink(Data, Take)) return false;
        Got += Take;
        if (Limit && Got >= Limit) {
          Stopped = true;
          return false;
        }
        return !Quit();
      },
      Info);
  if (Stopped && Error && std::strcmp(Error, "Cancelled") == 0) return nullptr;
  return Error;
}

void Run() {
  using namespace Switch2Guest;
  // 1. nifm
  u32 Address {};
  const bool Nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
  const Result AddressRc = Nifm ? nifmGetCurrentIpAddress(&Address) : 1;
  Out("nifm=%d ip_rc=%#x ip=%u.%u.%u.%u", Nifm, AddressRc, Address & 255, (Address >> 8) & 255, (Address >> 16) & 255, Address >> 24);
  if (!Nifm || R_FAILED(AddressRc) || !Address) {
    Report("1 network connection", false, "not connected");
    return;
  }
  Report("1 network connection", true);
  constexpr u32 SocketBuffer = 65536;
  const SocketInitConfig Config = {
    .tcp_tx_buf_size = 8 * SocketBuffer,
    .tcp_rx_buf_size = 8 * SocketBuffer,
    .tcp_tx_buf_max_size = 16 * SocketBuffer,
    .tcp_rx_buf_max_size = 16 * SocketBuffer,
    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,
    .sb_efficiency = 8,
    .num_bsd_sessions = 4,
    .bsd_service_type = BsdServiceType_User,
  };
  const Result SocketRc = socketInitialize(&Config);
  Out("socketInitialize (launcher config: 8x64 KiB tcp buffers) rc=%#x", SocketRc);
  if (R_FAILED(SocketRc)) {
    Report("1 socket service", false);
    return;
  }

  // 2. DNS
  {
    addrinfo Hints {};
    Hints.ai_family = AF_INET;
    Hints.ai_socktype = SOCK_STREAM;
    addrinfo* List {};
    const double Start = Seconds();
    const int Rc = getaddrinfo("rootfs.fex-emu.gg", "80", &Hints, &List);
    char Detail[96] = "";
    if (!Rc && List) {
      const auto* Ip = &reinterpret_cast<sockaddr_in*>(List->ai_addr)->sin_addr;
      char Text[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, Ip, Text, sizeof(Text));
      std::snprintf(Detail, sizeof(Detail), "-> %s in %.0f ms", Text, (Seconds() - Start) * 1000);
      freeaddrinfo(List);
    } else {
      std::snprintf(Detail, sizeof(Detail), "getaddrinfo rc=%d", Rc);
    }
    Report("2 DNS rootfs.fex-emu.gg", !Rc, Detail);
    if (Rc) return;
  }

  // 3. Range probe: bytes 0-.. limited to one byte, to read the head only.
  {
    Http::ResponseInfo Info;
    const auto* Error = Fetch(1000, 1, [](const uint8_t*, size_t) { return true; }, Info);
    char Detail[200];
    std::snprintf(Detail, sizeof(Detail), "status=%d range_start=%" PRIu64 " total=%" PRIu64 " (%s)", Info.Status, Info.RangeStart.value_or(0),
                  Info.TotalSize.value_or(0), Error ? Error : "ok");
    Report("3 Range request (no redirect, 206, Content-Range)", !Error && Info.Status == 206 && Info.RangeStart == 1000u && Info.TotalSize == ImageBytes, Detail);
    if (Error) return;
  }

  // 4. network only
  {
    Http::ResponseInfo Info;
    uint64_t Total = 0;
    const double Start = Seconds();
    const auto* Error = Fetch(0, 32 * MiB, [&](const uint8_t*, size_t Count) {
      Total += Count;
      return true;
    }, Info);
    const double Elapsed = Seconds() - Start;
    Report("4 network only, first 32 MiB discarded", !Error && Total == 32 * MiB, (Error ? Error : Mbps(Total, Elapsed).c_str()));
    Out("   status=%d content_length=%" PRIu64, Info.Status, Info.ContentLength.value_or(0));
  }

  // 4b. Throughput by socket buffer size (0.16.2: 256 KiB reached the 100 Mbit line). Speed is capped at
  // about SO_RCVBUF / round-trip time; 64 KiB gave about 3.3 MiB/s on a 100 Mbit line.
  for (const int Size : {65536, 262144, 524288, 1048576}) {
    TransferSocketBuffer = Size;
    Http::ResponseInfo Info;
    uint64_t Total = 0;
    const double Start = Seconds();
    const auto* Error = Fetch(0, 64 * MiB, [&](const uint8_t*, size_t Count) {
      Total += Count;
      return true;
    }, Info);
    const double Elapsed = Seconds() - Start;
    char Name[96];
    std::snprintf(Name, sizeof(Name), "4b SO_RCVBUF %d KiB (socket reports %d), 64 MiB", Size / 1024, LastTunedReceiveBuffer);
    Report(Name, !Error && Total == 64 * MiB, (Error ? Error : Mbps(Total, Elapsed).c_str()));
    if (Quit()) return;
  }
  TransferSocketBuffer = 262144; // The installer's value.

  // 5. network to SD with XXH3
  uint64_t PrefixHash = 0;
  mkdir("sdmc:/fex", 0777);
  mkdir("sdmc:/fex/.download", 0777);
  {
    auto* File = std::fopen(ProbeFile, "wb");
    if (!File) {
      Report("5 network to SD", false, "could not create the probe file");
      return;
    }
    auto* State = XXH3_createState();
    XXH3_64bits_reset(State);
    Http::ResponseInfo Info;
    uint64_t Total = 0;
    bool WriteFailed = false;
    const double Start = Seconds();
    const auto* Error = Fetch(0, 64 * MiB, [&](const uint8_t* Data, size_t Count) {
      XXH3_64bits_update(State, Data, Count);
      if (std::fwrite(Data, 1, Count, File) != Count) {
        WriteFailed = true;
        return false;
      }
      Total += Count;
      return true;
    }, Info);
    std::fclose(File);
    const double Elapsed = Seconds() - Start;
    PrefixHash = XXH3_64bits_digest(State);
    XXH3_freeState(State);
    Report("5 network to SD, first 64 MiB", !Error && !WriteFailed && Total == 64 * MiB, (Error ? Error : Mbps(Total, Elapsed).c_str()));
    Out("   XXH3-64 of the first 64 MiB = %s (host: head -c 67108864 Ubuntu_24_04.sqsh | xxhsum -H3)", Hex(PrefixHash).c_str());
  }

  // 6. Range resume: re-read 16 MiB at 32 MiB and compare with the file written in case 5.
  {
    auto* State = XXH3_createState();
    XXH3_64bits_reset(State);
    Http::ResponseInfo Info;
    uint64_t Total = 0;
    const double Start = Seconds();
    const auto* Error = Fetch(32 * MiB, 16 * MiB, [&](const uint8_t* Data, size_t Count) {
      XXH3_64bits_update(State, Data, Count);
      Total += Count;
      return true;
    }, Info);
    const double Elapsed = Seconds() - Start;
    const uint64_t Resumed = XXH3_64bits_digest(State);
    XXH3_freeState(State);
    // The same slice read back from the SD file.
    auto* Reader = std::fopen(ProbeFile, "rb");
    uint64_t FromFile = 0;
    bool ReadBack = false;
    if (Reader) {
      auto* Check = XXH3_createState();
      XXH3_64bits_reset(Check);
      std::fseek(Reader, static_cast<long>(32 * MiB), SEEK_SET);
      std::vector<uint8_t> Chunk(256 * 1024);
      uint64_t Left = 16 * MiB;
      while (Left) {
        const size_t Want = static_cast<size_t>(std::min<uint64_t>(Left, Chunk.size()));
        if (std::fread(Chunk.data(), 1, Want, Reader) != Want) break;
        XXH3_64bits_update(Check, Chunk.data(), Want);
        Left -= Want;
      }
      ReadBack = Left == 0;
      FromFile = XXH3_64bits_digest(Check);
      XXH3_freeState(Check);
      std::fclose(Reader);
    }
    char Detail[200];
    std::snprintf(Detail, sizeof(Detail), "status=%d range_start=%" PRIu64 " resumed=%s file=%s %s", Info.Status, Info.RangeStart.value_or(0),
                  Hex(Resumed).c_str(), Hex(FromFile).c_str(), Mbps(Total, Elapsed).c_str());
    Report("6 Range resume at 32 MiB equals the bytes already on the SD", !Error && ReadBack && Info.Status == 206 && Info.RangeStart == 32 * MiB &&
                                                                              Total == 16 * MiB && Resumed == FromFile, Detail);
  }

  // 7. interrupted transfer, resumed from the received offset (what the installer does after a stall).
  {
    Http::ResponseInfo Info;
    uint64_t Got = 0;
    const auto* First = Fetch(0, 8 * MiB, [&](const uint8_t*, size_t Count) {
      Got += Count;
      return true;
    }, Info);
    Info = {};
    uint64_t Second = 0;
    const auto* Error = Fetch(Got, 4 * MiB, [&](const uint8_t*, size_t Count) {
      Second += Count;
      return true;
    }, Info);
    char Detail[160];
    std::snprintf(Detail, sizeof(Detail), "first=%" PRIu64 " bytes then 206 at %" PRIu64 ", got %" PRIu64, Got, Info.RangeStart.value_or(0), Second);
    Report("7 dropped connection, resume from the received offset", !First && !Error && Info.Status == 206 && Info.RangeStart == Got && Second == 4 * MiB, Detail);
  }
  std::remove(ProbeFile);

  // 8. the whole image (sidecar)
  struct stat Sidecar {};
  if (stat(FullSidecar, &Sidecar) == 0) {
    Out("8 whole image (sidecar %s present): 501 MiB, takes minutes. + stops.", FullSidecar);
    auto* State = XXH3_createState();
    XXH3_64bits_reset(State);
    Http::ResponseInfo Info;
    uint64_t Total = 0;
    uint64_t Resumes = 0;
    const double Start = Seconds();
    const char* Error = nullptr;
    while (Total < ImageBytes) {
      Error = Fetch(Total, 0, [&](const uint8_t* Data, size_t Count) {
        XXH3_64bits_update(State, Data, Count);
        Total += Count;
        if ((Total & (8 * MiB - 1)) < Count) {
          std::printf("\r%.0f / %.0f MiB", Total / double(MiB), ImageBytes / double(MiB));
          consoleUpdate(nullptr);
        }
        return true;
      }, Info);
      if (!Error) break;
      if (std::strcmp(Error, "Cancelled") == 0) break;
      ++Resumes;
      Out("   interrupted at %" PRIu64 ": %s (resume %" PRIu64 ")", Total, Error, Resumes);
      if (Resumes > 20) break;
    }
    const double Elapsed = Seconds() - Start;
    const uint64_t Hash = XXH3_64bits_digest(State);
    XXH3_freeState(State);
    char Detail[240];
    std::snprintf(Detail, sizeof(Detail), "%s, XXH3-64 %s (expected %s), %" PRIu64 " resumes", Mbps(Total, Elapsed).c_str(), Hex(Hash).c_str(), ImageHash, Resumes);
    Report("8 whole image over HTTP", Total == ImageBytes && Hex(Hash) == ImageHash, Detail);
  } else {
    Out("8 whole image: skipped (create %s to run it)", FullSidecar);
  }
  socketExit();
}
} // namespace

int main() {
  consoleInit(nullptr);
  fsdevMountSdmc();
  Log = std::fopen(LogPath, "a");
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&Pad);
  if (Log) std::fprintf(Log, "\n===== new run =====\n");
  Out("%s. Log: %s", Version, Log ? LogPath : "NOT OPEN (SD write failed), screen only");
  Out("%s", Url);
  Run();
  Out("==== download-probe end fails=%d ====", Fails);
  Out("Press + to exit.");
  while (appletMainLoop()) {
    padUpdate(&Pad);
    if (padGetButtonsDown(&Pad) & HidNpadButton_Plus) break;
    consoleUpdate(nullptr);
  }
  if (Log) std::fclose(Log);
  consoleExit(nullptr);
  return 0;
}
