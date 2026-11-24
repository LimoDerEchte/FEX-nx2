// SPDX-License-Identifier: MIT
// "Setup Debian rootfs" launcher screen: downloads the packages named in the
// bundled manifest (/fex/setup/debian-rootfs.manifest, generated on the host
// from the Debian archive over HTTPS), checks each one's size and SHA-256,
// and unpacks it into /fex/rootfs through the guest filesystem (dpkg's first
// stage). The console has no TLS, so packages travel over plain HTTP; the
// manifest's hashes are what make that safe. Verified packages are cached in
// /fex/setup/cache so an interrupted setup resumes without downloading again.
#include "GuestRootfsSetup.h"
#include "GuestRuntime.h"
#include "HostNet.h"

#include <switch.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {
using namespace Switch2Guest;
using namespace Switch2Guest::RootfsSetup;

constexpr char ManifestPath[] = "sdmc:/fex/setup/debian-rootfs.manifest";
constexpr char CacheFolder[] = "sdmc:/fex/setup/cache";

bool ReadSdFile(const std::string& Path, std::vector<uint8_t>& Data) {
  auto* File = std::fopen(Path.c_str(), "rb");
  if (!File) return false;
  std::fseek(File, 0, SEEK_END);
  const long Size = std::ftell(File);
  std::fseek(File, 0, SEEK_SET);
  Data.resize(Size > 0 ? Size : 0);
  const bool Read = Data.empty() || std::fread(Data.data(), 1, Data.size(), File) == Data.size();
  std::fclose(File);
  return Read;
}

bool WriteSdFile(const std::string& Path, const std::vector<uint8_t>& Data) {
  const auto Temporary = Path + ".part";
  auto* File = std::fopen(Temporary.c_str(), "wb");
  if (!File) return false;
  const bool Written = Data.empty() || std::fwrite(Data.data(), 1, Data.size(), File) == Data.size();
  if (std::fclose(File) || !Written) return false;
  std::remove(Path.c_str());
  return std::rename(Temporary.c_str(), Path.c_str()) == 0;
}

std::string Sha256Hex(const std::vector<uint8_t>& Data) {
  uint8_t Digest[SHA256_HASH_SIZE];
  sha256CalculateHash(Digest, Data.data(), Data.size());
  std::string Text;
  char Byte[3];
  for (auto Value : Digest) {
    std::snprintf(Byte, sizeof(Byte), "%02x", Value);
    Text += Byte;
  }
  return Text;
}

// Called with bytes received so far; returns false to cancel.
using ProgressFn = std::function<bool(uint64_t)>;

// HTTP/1.1 GET over a plain TCP connection, following up to five http://
// redirects. Returns an error text, or nullptr.
const char* HttpGet(std::string UrlText, std::vector<uint8_t>& Body, const ProgressFn& Progress, int& Status) {
  for (int Redirects = 0; Redirects <= 5; ++Redirects) {
    const auto Location = ParseHttpUrl(UrlText);
    if (!Location) return "Unsupported redirect (not http://)";
    addrinfo Hints {};
    Hints.ai_family = AF_INET;
    Hints.ai_socktype = SOCK_STREAM;
    addrinfo* Addresses {};
    const auto Port = std::to_string(Location->Port);
    if (getaddrinfo(Location->Host.c_str(), Port.c_str(), &Hints, &Addresses) || !Addresses) return "Could not resolve the mirror's name";
    const int Socket = socket(AF_INET, SOCK_STREAM, 0);
    if (Socket < 0) {
      freeaddrinfo(Addresses);
      return "Could not open a socket";
    }
    timeval Timeout {30, 0};
    setsockopt(Socket, SOL_SOCKET, SO_RCVTIMEO, &Timeout, sizeof(Timeout));
    setsockopt(Socket, SOL_SOCKET, SO_SNDTIMEO, &Timeout, sizeof(Timeout));
    const int Connected = connect(Socket, Addresses->ai_addr, Addresses->ai_addrlen);
    freeaddrinfo(Addresses);
    if (Connected) {
      close(Socket);
      return "Could not connect to the mirror";
    }
    const auto Request = "GET " + Location->Path + " HTTP/1.1\r\nHost: " + Location->Host +
                         "\r\nUser-Agent: FEX-Switch2-rootfs-setup\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
    if (send(Socket, Request.data(), Request.size(), 0) != static_cast<ssize_t>(Request.size())) {
      close(Socket);
      return "Could not send the request";
    }
    std::vector<uint8_t> Response;
    std::vector<uint8_t> Chunk(64 * 1024);
    size_t HeadEnd = std::string::npos;
    for (;;) {
      const auto Received = recv(Socket, Chunk.data(), Chunk.size(), 0);
      if (Received < 0) {
        close(Socket);
        return "The download stalled or failed";
      }
      if (!Received) break;
      Response.insert(Response.end(), Chunk.begin(), Chunk.begin() + Received);
      if (HeadEnd == std::string::npos) {
        const std::string_view Text(reinterpret_cast<const char*>(Response.data()), Response.size());
        HeadEnd = Text.find("\r\n\r\n");
      }
      if (Progress && !Progress(HeadEnd == std::string::npos ? 0 : Response.size() - HeadEnd - 4)) {
        close(Socket);
        return "Cancelled";
      }
    }
    close(Socket);
    if (HeadEnd == std::string::npos) return "Malformed HTTP response";
    const auto Head = ParseResponseHead({reinterpret_cast<const char*>(Response.data()), HeadEnd + 2});
    Status = Head.Status;
    if (!Head.Status) return "Malformed HTTP response";
    if (Head.Status == 301 || Head.Status == 302 || Head.Status == 303 || Head.Status == 307 || Head.Status == 308) {
      if (Head.Location.starts_with("/")) UrlText = "http://" + Location->Host + Head.Location;
      else UrlText = Head.Location;
      continue;
    }
    if (Head.Status != 200) return "The mirror refused the download";
    Body.assign(Response.begin() + HeadEnd + 4, Response.end());
    if (Head.Chunked && !Dechunk(Body)) return "Malformed chunked response";
    if (!Head.Chunked && Head.ContentLength && *Head.ContentLength != Body.size()) return "The download was cut short";
    return nullptr;
  }
  return "Too many redirects";
}

class Screen {
public:
  explicit Screen(PadState& Pad) : Pad {Pad} {}
  std::vector<std::string> Log;
  std::string Status;

  void Draw() {
    consoleClear();
    std::puts("Setup Debian rootfs\n");
    const size_t First = Log.size() > 30 ? Log.size() - 30 : 0;
    for (size_t Index = First; Index < Log.size(); ++Index) std::puts(Log[Index].c_str());
    if (!Status.empty()) std::printf("\n%s\n", Status.c_str());
    consoleUpdate(nullptr);
  }

  void Add(std::string Line) {
    WriteLog(("Rootfs setup: " + Line).c_str());
    Log.push_back(std::move(Line));
    Draw();
  }

  bool CancelRequested() {
    padUpdate(&Pad);
    return padGetButtonsDown(&Pad) & HidNpadButton_B;
  }

  // Waits for A (true) or B (false).
  bool Confirm() {
    while (appletMainLoop()) {
      padUpdate(&Pad);
      const auto Buttons = padGetButtonsDown(&Pad);
      if (Buttons & HidNpadButton_A) return true;
      if (Buttons & (HidNpadButton_B | HidNpadButton_Plus)) return false;
      svcSleepThread(20'000'000);
    }
    return false;
  }

private:
  PadState& Pad;
};

std::string Mib(uint64_t Bytes) {
  char Text[32];
  std::snprintf(Text, sizeof(Text), "%.1f MiB", Bytes / 1048576.0);
  return Text;
}

// Fetches one package, from the cache if it is there and intact. Returns an
// error text, or nullptr.
const char* FetchPackage(const Manifest& List, const ManifestPackage& Package, std::vector<uint8_t>& Deb, Screen& View,
                         const std::string& Label) {
  const auto Cached = std::string {CacheFolder} + "/" + Package.Path.substr(Package.Path.rfind('/') + 1);
  if (ReadSdFile(Cached, Deb) && Deb.size() == Package.Size && Sha256Hex(Deb) == Package.Sha256) return nullptr;
  const char* Error {};
  for (const auto& Url : {PackageUrl(List, Package), SnapshotUrl(List, Package)}) {
    uint64_t LastShown {};
    int Status {};
    Error = HttpGet(Url, Deb,
                    [&](uint64_t Received) {
                      if (Received - LastShown >= 256 * 1024 || Received == Package.Size) {
                        LastShown = Received;
                        View.Status = Label + "  " + Mib(Received) + " / " + Mib(Package.Size) + "\nB Cancel";
                        View.Draw();
                      }
                      return !View.CancelRequested();
                    },
                    Status);
    if (Error && std::strcmp(Error, "Cancelled") == 0) return Error;
    if (!Error && (Deb.size() != Package.Size || Sha256Hex(Deb) != Package.Sha256)) Error = "Checksum mismatch";
    if (!Error) break;
    WriteLog(("Rootfs setup: " + Url + ": " + Error + " (HTTP " + std::to_string(Status) + ")").c_str());
  }
  if (Error) return Error;
  mkdir("sdmc:/fex/setup", 0777);
  mkdir(CacheFolder, 0777);
  if (!WriteSdFile(Cached, Deb)) WriteLog("Rootfs setup: could not cache a package; continuing.");
  return nullptr;
}

// Runs the whole setup after confirmation. Returns the result line.
std::string Install(Screen& View, const Manifest& List) {
  auto Writer = OpenGuestRootfsWriter();
  if (!Writer) return "Could not reserve memory for the guest filesystem.";
  if (const auto* Error = PrepareDpkgDatabase(*Writer)) return Error;
  std::vector<InstalledPackage> Installed;
  uint64_t Done {};
  for (size_t Index = 0; Index < List.Packages.size(); ++Index) {
    const auto& Package = List.Packages[Index];
    const auto Label = "[" + std::to_string(Index + 1) + "/" + std::to_string(List.Packages.size()) + "] " + Package.Name;
    View.Status = Label;
    View.Draw();
    std::vector<uint8_t> Deb;
    if (const auto* Error = FetchPackage(List, Package, Deb, View, Label)) {
      if (!Installed.empty()) Writer->WriteFile("/var/lib/dpkg/status", Bytes(StatusFile(Installed)), 0644, 0, 0, 0);
      return Package.Name + ": " + Error + (std::strcmp(Error, "Cancelled") ? ". Check the network and retry." : ".");
    }
    View.Status = Label + "  unpacking";
    View.Draw();
    InstalledPackage Record;
    std::string Detail;
    if (const auto* Error = UnpackDeb(Deb, *Writer, Record, Detail)) {
      if (!Installed.empty()) Writer->WriteFile("/var/lib/dpkg/status", Bytes(StatusFile(Installed)), 0644, 0, 0, 0);
      return Package.Name + ": " + Error + (Detail.empty() ? "" : " " + Detail);
    }
    Installed.push_back(std::move(Record));
    Done += Package.Size;
    View.Add(Label + " " + Package.Version + "  " + Mib(Done) + " / " + Mib(List.TotalSize()));
  }
  if (Writer->WriteFile("/var/lib/dpkg/status", Bytes(StatusFile(Installed)), 0644, 0, 0, 0) < 0)
    return "Could not write /var/lib/dpkg/status.";
  if (Writer->WriteFile("/etc/apt/sources.list", Bytes(SourcesList(List.Suite)), 0644, 0, 0, 0) < 0)
    return "Could not write /etc/apt/sources.list.";
  return "Unpacked " + std::to_string(Installed.size()) + " packages. Maintainer scripts have not run yet: "
         "run 'dpkg --configure -a' as root to finish.";
}
} // namespace

namespace Switch2Guest {
void RunRootfsSetup(PadState& Pad, const std::string& InodeLayoutError) {
  Screen View(Pad);
  std::vector<uint8_t> Text;
  Manifest List;
  std::string Problem;
  if (!InodeLayoutError.empty()) Problem = InodeLayoutError;
  else if (!ReadSdFile(ManifestPath, Text)) Problem = "Missing /fex/setup/debian-rootfs.manifest. Copy the full SD bundle.";
  else if (const auto* Error = ParseManifest({reinterpret_cast<const char*>(Text.data()), Text.size()}, List)) Problem = Error;
  if (!Problem.empty()) {
    View.Add(Problem);
    View.Status = "B Back";
    View.Draw();
    View.Confirm();
    return;
  }
  View.Add("Debian " + List.Suite + " (minbase), " + std::to_string(List.Packages.size()) + " packages, " +
           Mib(List.TotalSize()) + " download.");
  View.Add("Packages are unpacked into /fex/rootfs over existing files.");
  View.Add("Downloads use plain HTTP; every package is checked");
  View.Add("against the SHA-256 in the bundled manifest.");
  View.Status = "A Start    B Back";
  View.Draw();
  if (!View.Confirm()) return;

  u32 HostAddress = 0;
  const bool Nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
  std::string Result;
  if (!Nifm || R_FAILED(nifmGetCurrentIpAddress(&HostAddress)) || !HostAddress) {
    Result = "The console is not connected to a network.";
  } else if (!Switch2Guest::HostNet::Acquire()) {
    Result = "Could not start the network socket service.";
  } else {
    Result = Install(View, List);
    Switch2Guest::HostNet::Release();
  }
  if (Nifm) nifmExit();
  View.Add(Result);
  View.Status = "B Back";
  View.Draw();
  View.Confirm();
}
} // namespace Switch2Guest
