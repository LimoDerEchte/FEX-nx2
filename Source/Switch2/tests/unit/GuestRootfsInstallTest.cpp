// SPDX-License-Identifier: MIT
// Host tests for the RootFS image installer (runtime/fs/GuestRootfsInstall.h, GuestSquashfs.h, host/HttpClient.h).
//
//   guest-rootfs-install-test                      unit checks
//   guest-rootfs-install-test extract IMG OUT      extracts a squashfs image into OUT with the installer's extraction
//                                                   code (OUT.meta records owners and modes, devices are listed)
//   guest-rootfs-install-test scenarios DIR        pipeline scenarios with the fixtures tools/CheckRootfsInstall.py wrote
//   guest-rootfs-install-test install IMG SDROOT [HASH] [--extras]
//                                                   a full install of a real image into SDROOT/fex/rootfs
#include "GuestRootfsInstall.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

using namespace Switch2Guest;
using namespace Switch2Guest::RootfsInstall;

#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      std::exit(1);                                           \
    }                                                         \
  } while (0)

static void RemoveAll(const std::string& Path) {
  struct stat Info;
  if (lstat(Path.c_str(), &Info)) return;
  if (S_ISDIR(Info.st_mode)) {
    chmod(Path.c_str(), 0700);
    if (auto* Directory = opendir(Path.c_str())) {
      while (const auto* Entry = readdir(Directory)) {
        const std::string Name = Entry->d_name;
        if (Name != "." && Name != "..") RemoveAll(Path + "/" + Name);
      }
      closedir(Directory);
    }
    rmdir(Path.c_str());
  } else {
    unlink(Path.c_str());
  }
}

static void MakeAll(const std::string& Path) {
  for (size_t Slash = 1; Slash != std::string::npos; Slash = Path.find('/', Slash + 1)) mkdir(Path.substr(0, Slash).c_str(), 0777);
  mkdir(Path.c_str(), 0777);
}

static std::string ReadText(const std::string& Path) {
  std::ifstream File(Path, std::ios::binary);
  return {std::istreambuf_iterator<char>(File), {}};
}

static bool Exists(const std::string& Path) {
  struct stat Info;
  return !lstat(Path.c_str(), &Info);
}

// A RootfsWriter over a host directory. Ownership and the full mode (setuid, sticky, restrictive directories) are kept in a
// table instead of applied, so the test runs unprivileged and can always clean up.
static bool ApplyModes = false; // the "install" mode applies real modes so the tree can be run natively

class HostWriter final : public RootfsWriter {
public:
  struct Meta {
    uint32_t Mode {}, Uid {}, Gid {};
  };
  HostWriter(std::string Base, std::map<std::string, Meta>& Table) : Base {std::move(Base)}, Table {Table} {}
  std::vector<std::string> Nodes;

  int64_t MakeDirectory(const std::string& Path, uint32_t Mode, uint32_t Uid, uint32_t Gid) override {
    struct stat Info;
    if (!lstat(Full(Path).c_str(), &Info)) {
      if (S_ISLNK(Info.st_mode) && !stat(Full(Path).c_str(), &Info) && S_ISDIR(Info.st_mode)) return 0;
      return S_ISDIR(Info.st_mode) ? 0 : -EEXIST;
    }
    if (mkdir(Full(Path).c_str(), 0700)) return -errno;
    Table[Path] = {Mode & 07777, Uid, Gid};
    if (ApplyModes) chmod(Full(Path).c_str(), (Mode & 07777) | 0700);
    return 0;
  }
  int64_t WriteFile(const std::string& Path, std::span<const uint8_t> Data, uint32_t Mode, uint32_t Uid, uint32_t Gid, int64_t) override {
    const auto Temporary = Full(Path) + ".dpkg-new";
    auto* File = std::fopen(Temporary.c_str(), "wb");
    if (!File) return -errno;
    if (!Data.empty() && std::fwrite(Data.data(), 1, Data.size(), File) != Data.size()) {
      std::fclose(File);
      return -EIO;
    }
    std::fclose(File);
    chmod(Temporary.c_str(), ApplyModes ? ((Mode & 07777) | 0600) : 0600);
    if (rename(Temporary.c_str(), Full(Path).c_str())) return -errno;
    Table[Path] = {Mode & 07777, Uid, Gid};
    return 0;
  }
  int64_t Symlink(const std::string& Target, const std::string& Path, uint32_t Uid, uint32_t Gid) override {
    unlink(Full(Path).c_str());
    if (symlink(Target.c_str(), Full(Path).c_str())) return -errno;
    Table[Path] = {0777, Uid, Gid};
    return 0;
  }
  int64_t HardLink(const std::string& Existing, const std::string& Path) override {
    unlink(Full(Path).c_str());
    if (link(Full(Existing).c_str(), Full(Path).c_str())) return -errno;
    Table[Path] = Table[Existing];
    return 0;
  }
  int64_t MakeNode(const std::string& Path, uint32_t Mode, uint64_t Device, uint32_t Uid, uint32_t Gid) override {
    if (Device) {
      Nodes.push_back(Path);
      return -1; // the runtime has no device nodes
    }
    unlink(Full(Path).c_str());
    if (mkfifo(Full(Path).c_str(), 0600)) return -errno;
    Table[Path] = {Mode & 07777, Uid, Gid};
    return 0;
  }

  int64_t BeginFile(const std::string& Path, uint32_t Mode, uint32_t Uid, uint32_t Gid, int64_t) override {
    unlink(Full(Path).c_str());
    Stream = std::fopen(Full(Path).c_str(), "wb");
    if (!Stream) return -errno;
    StreamPath = Path;
    Table[Path] = {Mode & 07777, Uid, Gid};
    return 0;
  }
  int64_t AppendFile(std::span<const uint8_t> Data) override {
    return !Data.empty() && std::fwrite(Data.data(), 1, Data.size(), Stream) != Data.size() ? -EIO : 0;
  }
  int64_t EndFile() override {
    const bool Bad = std::fclose(Stream);
    Stream = nullptr;
    chmod(Full(StreamPath).c_str(), ApplyModes ? (Table[StreamPath].Mode | 0600) : 0600);
    return Bad ? -EIO : 0;
  }

  int64_t TreeStat(const std::string& Path, TreeEntry& Out) override {
    struct stat Info;
    if (lstat(Full(Path).c_str(), &Info)) return -errno;
    const auto Found = Table.find(Path);
    Out.Mode = (Info.st_mode & 0170000) | (Found != Table.end() ? Found->second.Mode : (Info.st_mode & 07777));
    Out.Uid = Found != Table.end() ? Found->second.Uid : 0;
    Out.Gid = Found != Table.end() ? Found->second.Gid : 0;
    Out.Size = static_cast<uint64_t>(Info.st_size);
    return 0;
  }
  int64_t TreeList(const std::string& Path, std::vector<std::string>& Names) override {
    auto* Directory = opendir(Full(Path).c_str());
    if (!Directory) return -errno;
    while (const auto* Entry = readdir(Directory)) {
      const std::string Name = Entry->d_name;
      if (Name != "." && Name != "..") Names.push_back(Name);
    }
    closedir(Directory);
    return 0;
  }
  int64_t TreeRename(const std::string& From, const std::string& To) override {
    if (rename(Full(From).c_str(), Full(To).c_str())) return -errno;
    std::vector<std::pair<std::string, Meta>> Moved;
    for (auto Item = Table.begin(); Item != Table.end();) {
      if (Item->first == From || Item->first.starts_with(From + "/")) {
        Moved.emplace_back(To + Item->first.substr(From.size()), Item->second);
        Item = Table.erase(Item);
      } else {
        ++Item;
      }
    }
    for (auto& [Key, Value] : Moved) Table[Key] = Value;
    return 0;
  }
  int64_t TreeRemove(const std::string& Path) override {
    struct stat Info;
    if (lstat(Full(Path).c_str(), &Info)) return -errno;
    if ((S_ISDIR(Info.st_mode) ? rmdir(Full(Path).c_str()) : unlink(Full(Path).c_str()))) return -errno;
    Table.erase(Path);
    return 0;
  }
  int64_t SetOwner(const std::string& Path, uint32_t Uid, uint32_t Gid) override {
    struct stat Info;
    if (lstat(Full(Path).c_str(), &Info)) return -errno;
    auto& Entry = Table[Path];
    if (!Entry.Mode) Entry.Mode = Info.st_mode & 07777;
    Entry.Uid = Uid;
    Entry.Gid = Gid;
    return 0;
  }
  int64_t SetMode(const std::string& Path, uint32_t Mode) override {
    struct stat Info;
    if (lstat(Full(Path).c_str(), &Info)) return -errno;
    Table[Path].Mode = Mode & 07777;
    if (ApplyModes) chmod(Full(Path).c_str(), S_ISDIR(Info.st_mode) ? ((Mode & 07777) | 0700) : ((Mode & 07777) | 0600));
    return 0;
  }
  int64_t ReadWholeFile(const std::string& Path, std::vector<uint8_t>& Data) override {
    auto* File = std::fopen(Full(Path).c_str(), "rb");
    if (!File) return -errno;
    Data.clear();
    uint8_t Chunk[4096];
    size_t Count;
    while ((Count = std::fread(Chunk, 1, sizeof(Chunk), File)) > 0) Data.insert(Data.end(), Chunk, Chunk + Count);
    std::fclose(File);
    return 0;
  }

  std::string Full(const std::string& Path) const { return Base + Path; }

private:
  std::string Base;
  std::map<std::string, Meta>& Table;
  std::FILE* Stream {};
  std::string StreamPath;
};

// Lines "path|type|mode|uid|gid|detail" for every entry below Base, sorted.
static void WriteMeta(const std::string& Base, const std::map<std::string, HostWriter::Meta>& Table, const std::string& Output) {
  std::vector<std::string> Lines;
  std::function<void(const std::string&)> Walk = [&](const std::string& Path) {
    auto* Directory = opendir((Base + Path).c_str());
    if (!Directory) return;
    std::vector<std::string> Names;
    while (const auto* Entry = readdir(Directory)) {
      const std::string Name = Entry->d_name;
      if (Name != "." && Name != "..") Names.push_back(Name);
    }
    closedir(Directory);
    for (const auto& Name : Names) {
      const auto Child = (Path == "/" ? "" : Path) + "/" + Name;
      struct stat Info;
      lstat((Base + Child).c_str(), &Info);
      const auto Found = Table.find(Child);
      char Line[600];
      const char* Type = S_ISDIR(Info.st_mode) ? "d" : S_ISLNK(Info.st_mode) ? "l" : S_ISFIFO(Info.st_mode) ? "p" : "f";
      std::string Detail;
      if (S_ISLNK(Info.st_mode)) {
        char Target[4200] {};
        const auto Length = readlink((Base + Child).c_str(), Target, sizeof(Target) - 1);
        Detail = std::string(Target, Length > 0 ? Length : 0);
      } else if (S_ISREG(Info.st_mode)) {
        Detail = std::to_string(Info.st_size) + " ino" + std::to_string(Info.st_ino) + " nlink" + std::to_string(Info.st_nlink);
      }
      std::snprintf(Line, sizeof(Line), "%s|%s|%o|%u|%u|", Child.c_str(), Type, Found == Table.end() ? 0 : Found->second.Mode,
                    Found == Table.end() ? 0 : Found->second.Uid, Found == Table.end() ? 0 : Found->second.Gid);
      Lines.push_back(std::string(Line) + Detail);
      if (S_ISDIR(Info.st_mode)) Walk(Child);
    }
  };
  Walk("/");
  std::sort(Lines.begin(), Lines.end());
  std::ofstream File(Output);
  for (const auto& Line : Lines) File << Line << "\n";
}

// ---------------------------------------------------------------------------

class TestEnv : public InstallEnv {
public:
  explicit TestEnv(std::string Root) : Root {std::move(Root)} {
    MakeAll(this->Root + "/fex/rootfs");
  }
  std::string Root;
  std::map<std::string, HostWriter::Meta> Table;
  std::vector<std::pair<LogKind, std::string>> Logs;
  std::vector<std::string> Steps;
  uint64_t Free = 1ull << 40;
  int CancelAfterPumps = -1; // cancel once this many Pump calls happened (-1: never)
  int Pumps {};
  std::string ServedFile;                       // fixture to serve for the image URL
  bool SupportRange = true;
  int64_t FailAfter = -1;                       // abort the first transfer after this many body bytes
  bool Failed {};
  std::vector<uint64_t> Offsets;
  std::map<std::string, std::string> Archive;   // URL -> fixture file
  std::vector<uint32_t> Dns;
  std::vector<uint8_t> ArchiveKey;               // the fixture archive's signing key (modulus); empty: the Ubuntu key
  double LastFraction {};
  bool Monotonic = true;

  bool Cancelled() override { return CancelAfterPumps >= 0 && Pumps >= CancelAfterPumps; }
  std::vector<std::span<const uint8_t>> ArchiveKeys() override {
    return ArchiveKey.empty() ? InstallEnv::ArchiveKeys() : std::vector<std::span<const uint8_t>> {std::span<const uint8_t>(ArchiveKey)};
  }
  void Pump() override { ++Pumps; }
  void Log(LogKind Kind, const std::string& Line) override { Logs.emplace_back(Kind, Line); }
  void Step(int, const std::string& Title) override { Steps.push_back(Title); }
  void Progress(double Fraction, const std::string&) override {
    if (Fraction + 1e-9 < LastFraction) Monotonic = false;
    LastFraction = Fraction;
  }
  uint64_t FreeBytes() override { return Free; }
  std::string Native(const std::string& Logical) override { return Root + Logical; }
  std::unique_ptr<RootfsWriter> OpenWriter() override { return std::make_unique<HostWriter>(Root + "/fex/rootfs", Table); }
  bool RootfsPopulated() override {
    std::vector<std::string> Names;
    HostWriter Writer(Root + "/fex/rootfs", Table);
    return Writer.TreeList("/", Names) == 0 && !Names.empty();
  }
  bool WipeRootfs(const WipeTick&, std::string&) override {
    RemoveAll(Root + "/fex/rootfs");
    MakeAll(Root + "/fex/rootfs");
    Table.clear();
    return true;
  }
  std::vector<uint32_t> DnsServers() override { return Dns; }
  const char* HttpGet(const std::string& Url, uint64_t Offset, const Http::BodyFn& Body, Http::ResponseInfo& Info, const Http::HeadFn& OnHead) override {
    Offsets.push_back(Offset);
    std::string Path;
    if (Url.ends_with(".sqsh")) Path = ServedFile;
    else if (Archive.count(Url)) Path = Archive[Url];
    Info = {};
    if (Path.empty()) {
      Info.Status = 404;
      return "The server refused the request";
    }
    auto* File = std::fopen(Path.c_str(), "rb");
    if (!File) {
      Info.Status = 404;
      return "The server refused the request";
    }
    std::fseek(File, 0, SEEK_END);
    const uint64_t Total = static_cast<uint64_t>(std::ftell(File));
    const bool Ranged = Offset && SupportRange;
    Info.Status = Ranged ? 206 : 200;
    if (Ranged) Info.RangeStart = Offset;
    const uint64_t Start = Ranged ? Offset : 0;
    std::fseek(File, static_cast<long>(Start), SEEK_SET);
    Info.ContentLength = Total - Start;
    if (OnHead && !OnHead(Info)) {
      std::fclose(File);
      return "Cancelled";
    }
    std::vector<uint8_t> Chunk(32 * 1024);
    uint64_t Sent = 0;
    for (;;) {
      const auto Count = std::fread(Chunk.data(), 1, Chunk.size(), File);
      if (!Count) break;
      if (FailAfter >= 0 && !Failed && Sent + Count > static_cast<uint64_t>(FailAfter)) {
        Failed = true;
        const auto Part = static_cast<size_t>(FailAfter - Sent);
        if (Part) Body(Chunk.data(), Part);
        std::fclose(File);
        return "The connection stalled or failed";
      }
      Sent += Count;
      if (!Body(Chunk.data(), Count)) {
        std::fclose(File);
        return "Cancelled";
      }
    }
    std::fclose(File);
    (void)Total;
    return nullptr;
  }
  bool ConfigureExtras(std::string& Detail) override {
    ConfigureCalled = true;
    Detail = "test";
    return true;
  }
  bool ConfigureCalled {};
  int64_t Now() override { return 1700000000; }
};

static bool HasLog(const TestEnv& Env, LogKind Kind, std::string_view Part) {
  for (const auto& [K, Line] : Env.Logs)
    if (K == Kind && Line.find(Part) != std::string::npos) return true;
  return false;
}

// ---------------------------------------------------------------------------
// Unit checks that need no fixtures.

static std::string Sha512Hex(std::string_view Text) {
  OpenPgp::Sha512 Hash;
  Hash.Update(Text);
  std::string Out;
  char Byte[3];
  for (auto Value : Hash.Final()) {
    std::snprintf(Byte, sizeof(Byte), "%02x", Value);
    Out += Byte;
  }
  return Out;
}

static void HashChecks() {
  // FIPS 180-4 examples; 112 bytes crosses a block boundary with the length.
  CHECK(Sha512Hex("abc") == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
  CHECK(Sha512Hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
        "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");
  CHECK(Sha256Of(RootfsSetup::Bytes("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(Sha256Of({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  std::string Long(1000, 'a');
  Sha256 Streaming;
  for (size_t Offset = 0; Offset < Long.size(); Offset += 37)
    Streaming.Update(reinterpret_cast<const uint8_t*>(Long.data()) + Offset, std::min<size_t>(37, Long.size() - Offset));
  CHECK(Streaming.Hex() == Sha256Of(RootfsSetup::Bytes(Long)));
  CHECK(ParseHash64("cee02b6ea44829d") == 0x0cee02b6ea44829dull); // the index drops leading zeros
  CHECK(ParseHash64("3517e0e5ea25a473") == 0x3517e0e5ea25a473ull);
  CHECK(!ParseHash64("") && !ParseHash64("xyz") && !ParseHash64("12345678901234567"));
  CHECK(Hex64(0x0cee02b6ea44829dull) == "0cee02b6ea44829d");
  // XXH3 of a file streamed in chunks equals the one-shot value.
  const std::string Path = "/tmp/guest-rootfs-install-hash.bin";
  {
    std::ofstream File(Path, std::ios::binary);
    std::string Data(700000, 'x');
    for (size_t Index = 0; Index < Data.size(); ++Index) Data[Index] = static_cast<char>(Index * 31 + (Index >> 8));
    File << Data;
  }
  const auto Text = ReadText(Path);
  uint64_t Hash {};
  CHECK(HashFileXxh3(Path, Hash, [](uint64_t) { return true; }));
  CHECK(Hash == XXH3_64bits(Text.data(), Text.size()));
  uint64_t Cancelled {};
  CHECK(!HashFileXxh3(Path, Cancelled, [](uint64_t Done) { return Done < 300000; }));
  unlink(Path.c_str());
}

static void HttpChecks() {
  const auto Head = Http::ParseHead("HTTP/1.1 206 Partial Content\r\nContent-Length: 100\r\nContent-Range: bytes 500-599/1000\r\nTransfer-Encoding: chunked\r\n");
  CHECK(Head.Status == 206 && Head.ContentLength == 100u && Head.RangeStart == 500u && Head.TotalSize == 1000u && Head.Chunked);
  CHECK(Http::ParseHead("HTTP/1.1 302 Found\r\nLocation: http://x/y\r\n").Location == "http://x/y");
  CHECK(Http::ParseHead("garbage").Status == 0);
  const auto Url = Http::ParseUrl("http://rootfs.fex-emu.gg:8080/Ubuntu_24_04/x.sqsh");
  CHECK(Url && Url->Host == "rootfs.fex-emu.gg" && Url->Port == 8080 && Url->Path == "/Ubuntu_24_04/x.sqsh");
  CHECK(!Http::ParseUrl("https://rootfs.fex-emu.gg/x") && !Http::ParseUrl("http://") && !Http::ParseUrl("http://h:0/"));
  // Chunk framing split at awkward places.
  const std::string Wire = "4\r\nWiki\r\n5;x=1\r\npedia\r\nE\r\n in\r\n\r\nchunks.\r\n0\r\n\r\n";
  for (size_t Step : {size_t {1}, size_t {2}, size_t {5}, Wire.size()}) {
    Http::Dechunker Unchunk;
    std::string Out;
    bool Good = true;
    for (size_t Offset = 0; Offset < Wire.size() && Good; Offset += Step)
      Good = Unchunk.Feed(reinterpret_cast<const uint8_t*>(Wire.data()) + Offset, std::min(Step, Wire.size() - Offset),
                          [&](const uint8_t* Data, size_t Size) {
                            Out.append(reinterpret_cast<const char*>(Data), Size);
                            return true;
                          });
    CHECK(Good && Unchunk.Finished() && Out == "Wikipedia in\r\n\r\nchunks.");
  }
  Http::Dechunker Bad;
  CHECK(!Bad.Feed(reinterpret_cast<const uint8_t*>("zz\r\n"), 4, [](const uint8_t*, size_t) { return true; }));
}

static void DependsChecks() {
  const auto Groups = ParseDepends("libc6 (>= 2.34), libcap2:amd64, libselinux1 | libfoo (<< 3), awk");
  CHECK(Groups.size() == 4);
  CHECK(Groups[0] == std::vector<std::string> {"libc6"} && Groups[1] == std::vector<std::string> {"libcap2"});
  CHECK((Groups[2] == std::vector<std::string> {"libselinux1", "libfoo"}) && Groups[3] == std::vector<std::string> {"awk"});
  CHECK(ParseDepends("").empty());
  const auto Hashes = ParseReleaseHashes("Origin: Ubuntu\nSHA256:\n " + std::string(64, 'a') + "   1234 main/binary-amd64/Packages.xz\n " +
                                         std::string(64, 'b') + " 55 main/Contents-amd64\nSize:\n junk\n");
  CHECK(Hashes.size() == 2 && Hashes.at("main/binary-amd64/Packages.xz").second == 1234 && Hashes.at("main/binary-amd64/Packages.xz").first == std::string(64, 'a'));
  std::vector<std::string> Seen;
  StanzaScanner Scanner([&](const std::string& Paragraph) { Seen.push_back(Paragraph); });
  const std::string Text = "Package: a\nVersion: 1\n\nPackage: b\nDepends: x\n\n\nPackage: c\nVersion: 3";
  for (char C : Text) Scanner.Feed(reinterpret_cast<const uint8_t*>(&C), 1);
  Scanner.Finish();
  CHECK(Seen.size() == 3 && Seen[0] == "Package: a\nVersion: 1\n" && Seen[2] == "Package: c\nVersion: 3");
  const auto Names = InstalledNames("Package: a\nStatus: install ok installed\nProvides: v1, v2 (= 1)\n\nPackage: b\nStatus: deinstall ok config-files\n\n");
  CHECK(Names.count("a") && Names.count("v1") && Names.count("v2") && !Names.count("b"));
}

// A broken chroot like FEX's images: etc files, folders and the apt state sit under /chroot.
static void BuildBrokenTree(HostWriter& Writer) {
  CHECK(Writer.MakeDirectory("/usr", 0755, 1000, 1000) == 0);
  CHECK(Writer.MakeDirectory("/usr/bin", 0755, 1000, 1000) == 0);
  CHECK(Writer.Symlink("usr/bin", "/bin", 1000, 1000) == 0);
  CHECK(Writer.MakeDirectory("/etc", 0755, 1000, 1000) == 0);
  CHECK(Writer.MakeDirectory("/etc/apt", 0755, 1000, 1000) == 0);
  CHECK(Writer.WriteFile("/etc/os-release", RootfsSetup::Bytes("NAME=Ubuntu\n"), 0644, 1000, 1000, 0) == 0);
  CHECK(Writer.MakeDirectory("/var", 0755, 1000, 1000) == 0);
  CHECK(Writer.MakeDirectory("/var/lib", 0755, 1000, 1000) == 0);
  CHECK(Writer.MakeDirectory("/var/lib/dbus", 0755, 1000, 1000) == 0); // exists in the main tree: merged, not replaced
  CHECK(Writer.MakeDirectory("/var/cache", 0755, 1000, 1000) == 0);
  CHECK(Writer.MakeDirectory("/chroot", 0755, 1000, 1000) == 0);
  for (const char* Path : {"/chroot/etc", "/chroot/boot", "/chroot/home", "/chroot/media", "/chroot/mnt", "/chroot/opt", "/chroot/root",
                           "/chroot/srv", "/chroot/tmp", "/chroot/run", "/chroot/run/lock", "/chroot/var", "/chroot/var/tmp", "/chroot/var/lib",
                           "/chroot/var/lib/dbus", "/chroot/var/cache"})
    CHECK(Writer.MakeDirectory(Path, std::string_view(Path) == "/chroot/root" ? 0700 : 0755, 1000, 1000) == 0);
  CHECK(Writer.SetMode("/chroot/tmp", 0664) == 0); // "for some reason /tmp ends up being 0664"
  for (const char* Name : {"hosts", "timezone", "fstab", "hostname", "subuid", "subgid", "machine-id", "passwd-", "group-", "shadow-", "gshadow-"})
    CHECK(Writer.WriteFile(std::string("/chroot/etc/") + Name, RootfsSetup::Bytes(std::string(Name) + "\n"), 0644, 1000, 1000, 0) == 0);
  CHECK(Writer.WriteFile("/chroot/etc/hosts", RootfsSetup::Bytes("127.0.0.1\tlocalhost\n"), 0644, 1000, 1000, 0) == 0);
  CHECK(Writer.WriteFile("/chroot/etc/passwd", RootfsSetup::Bytes("root:x:0:0:root:/root:/bin/bash\nnobody:x:65534:65534::/nonexistent:/usr/sbin/nologin\n"), 0644, 1000, 1000, 0) == 0);
  CHECK(Writer.WriteFile("/chroot/etc/group", RootfsSetup::Bytes("root:x:0:\nnogroup:x:65534:\n"), 0644, 1000, 1000, 0) == 0);
  CHECK(Writer.WriteFile("/chroot/etc/shadow", RootfsSetup::Bytes("root:*:19000:0:99999:7:::\n"), 0640, 1000, 1000, 0) == 0);
  CHECK(Writer.WriteFile("/chroot/etc/gshadow", RootfsSetup::Bytes("root:*::\n"), 0640, 1000, 1000, 0) == 0);
  CHECK(Writer.Symlink("../run/systemd/resolve/stub-resolv.conf", "/chroot/etc/resolv.conf", 1000, 1000) == 0);
  CHECK(Writer.Symlink("/usr/share/zoneinfo/America/New_York", "/chroot/etc/localtime", 1000, 1000) == 0);
  CHECK(Writer.Symlink("../proc/self/mounts", "/chroot/etc/mtab", 1000, 1000) == 0);
  CHECK(Writer.WriteFile("/chroot/var/lib/dbus/machine-id", RootfsSetup::Bytes("0123456789abcdef0123456789abcdef\n"), 0644, 1000, 1000, 0) == 0);
  CHECK(Writer.Symlink("/dev/shm", "/chroot/run/shm", 1000, 1000) == 0);
  CHECK(Writer.Symlink("/run", "/chroot/var/run", 1000, 1000) == 0);
  CHECK(Writer.Symlink("/run/lock", "/chroot/var/lock", 1000, 1000) == 0);
  CHECK(Writer.WriteFile("/chroot/root/.bashrc", RootfsSetup::Bytes("# root\n"), 0644, 1000, 1000, 0) == 0);
}

static void UnbreakChecks() {
  const std::string Base = "/tmp/guest-rootfs-install-unbreak";
  RemoveAll(Base);
  MakeAll(Base);
  std::map<std::string, HostWriter::Meta> Table;
  {
    HostWriter Writer(Base, Table);
    BuildBrokenTree(Writer);
    UnbreakStats Stats;
    std::string Detail;
    CHECK(!Unbreak(Writer, Stats, Detail));
    CHECK(Stats.Moved > 10 && Stats.Merged >= 1);
    CHECK(!Exists(Base + "/chroot"));
    for (const char* Path : {"/etc/hosts", "/etc/passwd", "/etc/passwd-", "/etc/group", "/etc/shadow", "/etc/gshadow", "/etc/fstab", "/etc/hostname",
                             "/etc/subuid", "/etc/subgid", "/etc/machine-id", "/etc/timezone", "/home", "/root/.bashrc", "/run/lock", "/var/tmp",
                             "/var/lib/dbus/machine-id", "/boot", "/media", "/mnt", "/opt", "/srv", "/tmp"})
      CHECK(Exists(Base + Path));
    struct stat Info;
    CHECK(!lstat((Base + "/etc/resolv.conf").c_str(), &Info) && S_ISLNK(Info.st_mode)); // moved as a link
    CHECK(!lstat((Base + "/var/run").c_str(), &Info) && S_ISLNK(Info.st_mode));
    CHECK(Table["/root"].Mode == 0700 && Table["/etc/shadow"].Mode == 0640); // modes travel with the entries
    CHECK(ReadText(Base + "/var/lib/dbus/machine-id") == "0123456789abcdef0123456789abcdef\n");
    // /etc/os-release was not in /chroot and stays.
    CHECK(Exists(Base + "/etc/os-release"));
    // Not a broken chroot any more: a second run does nothing.
    UnbreakStats Again;
    CHECK(!Unbreak(Writer, Again, Detail) && Again.Moved == 0);

    TestEnv Env("/tmp/guest-rootfs-install-unbreak-env");
    SetupStats Setup;
    CHECK(!EverydaySetup(Writer, {0x0100000a, 0}, Env, Setup, Detail)); // 10.0.0.1
    CHECK(Table["/tmp"].Mode == 01777);
    for (const char* Path : {"/proc", "/sys", "/dev", "/dev/pts", "/home/fex"}) CHECK(Exists(Base + Path));
    CHECK(Table["/home/fex"].Uid == 1000 && Table["/home/fex"].Gid == 1000);
    // The symlink resolv.conf was replaced by our file.
    CHECK(!lstat((Base + "/etc/resolv.conf").c_str(), &Info) && S_ISREG(Info.st_mode));
    const auto Resolv = ReadText(Base + "/etc/resolv.conf");
    CHECK(Resolv.starts_with(ResolvMarker) && Resolv.find("nameserver 10.0.0.1\n") != std::string::npos && Resolv.find("1.1.1.1") == std::string::npos);
    const auto Passwd = ReadText(Base + "/etc/passwd");
    CHECK(Passwd.starts_with("root:x:0:0:root:/root:/bin/bash\n") && Passwd.find("fex:x:1000:1000:fex:/home/fex:/bin/bash\n") != std::string::npos);
    CHECK(ReadText(Base + "/etc/group").find("fex:x:1000:\n") != std::string::npos);
    CHECK(ReadText(Base + "/etc/shadow").find("fex:*:") != std::string::npos);
    CHECK(ReadText(Base + "/etc/gshadow").find("fex:!::") != std::string::npos);
    CHECK(ReadText(Base + "/etc/hosts") == "127.0.0.1\tlocalhost\n127.0.1.1\tfex-switch2\n");  // the image's lines stay, the host name is added
    CHECK(ReadText(Base + "/etc/machine-id").size() == 33);             // the fixture's was too short: regenerated
    CHECK(ReadText(Base + "/etc/sudoers.d/fex").ends_with("fex ALL=(ALL:ALL) NOPASSWD: ALL\n") && Table["/etc/sudoers.d/fex"].Mode == 0440);
  }
  RemoveAll(Base);
}

static void AptMirrorChecks() {
  // The image's file: deb-src stanzas, three suites, comments. An earlier setup's mirror URI is put back as well.
  const std::string Sources = "## comment\nTypes: deb deb-src\nURIs: http://archive.ubuntu.com/ubuntu/\nSuites: noble noble-updates noble-backports\n"
                              "Components: main universe restricted multiverse\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n\n"
                              "Types: deb deb-src\nURIs: http://security.ubuntu.com/ubuntu/\nSuites: noble-security\nComponents: main universe restricted multiverse\n";
  const auto Out = NormalizeAptSources(Sources);
  CHECK(Out == "## comment\nTypes: deb\nURIs: http://archive.ubuntu.com/ubuntu/\nSuites: noble noble-updates noble-backports\n"
               "Components: main universe restricted multiverse\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n\n"
               "Types: deb\nURIs: http://security.ubuntu.com/ubuntu/\nSuites: noble-security\nComponents: main universe restricted multiverse\n");
  CHECK(NormalizeAptSources(Out) == Out);  // a second setup changes nothing
  CHECK(NormalizeAptSources("Types: deb\nURIs: mirror+http://mirrors.ubuntu.com/mirrors.txt\nSuites: noble\n") ==
        "Types: deb\nURIs: http://archive.ubuntu.com/ubuntu/\nSuites: noble\n");
  CHECK(NormalizeAptSources("Types: deb\nURIs: https://ppa.launchpadcontent.net/kisak/kisak-mesa/ubuntu/\nSuites: noble\n") ==
        "Types: deb\nURIs: https://ppa.launchpadcontent.net/kisak/kisak-mesa/ubuntu/\nSuites: noble\n");  // other sources keep their URI
}

static void SetupOnEmptyTree() {
  // An image without any account files: setup creates them.
  const std::string Base = "/tmp/guest-rootfs-install-empty";
  RemoveAll(Base);
  MakeAll(Base);
  std::map<std::string, HostWriter::Meta> Table;
  HostWriter Writer(Base, Table);
  CHECK(Writer.MakeDirectory("/etc", 0755, 0, 0) == 0);
  TestEnv Env("/tmp/guest-rootfs-install-empty-env");
  SetupStats Setup;
  std::string Detail;
  CHECK(!EverydaySetup(Writer, {}, Env, Setup, Detail));
  CHECK(ReadText(Base + "/etc/passwd") == "root:x:0:0:root:/root:/bin/bash\nfex:x:1000:1000:fex:/home/fex:/bin/bash\n");
  CHECK(ReadText(Base + "/etc/group") == "root:x:0:\nfex:x:1000:\n");
  CHECK(Table["/etc/shadow"].Mode == 0640 && Table["/etc/gshadow"].Mode == 0640);
  CHECK(ReadText(Base + "/etc/resolv.conf").find("nameserver 1.1.1.1\nnameserver 8.8.8.8\n") != std::string::npos);
  CHECK(ReadText(Base + "/etc/hosts").starts_with("127.0.0.1\tlocalhost\n"));
  CHECK(ReadText(Base + "/etc/hostname") == "fex-switch2\n");
  CHECK(ReadText(Base + "/etc/machine-id").size() == 33);
  CHECK(Table["/tmp"].Mode == 01777 && Exists(Base + "/tmp"));
  // passwd that has a uid-1000 user keeps it.
  CHECK(Writer.WriteFile("/etc/passwd", RootfsSetup::Bytes("root:x:0:0::/root:/bin/sh\nlimo:x:1000:1000::/home/limo:/bin/bash\n"), 0644, 0, 0, 0) == 0);
  SetupStats Again;
  CHECK(!EverydaySetup(Writer, {}, Env, Again, Detail));
  CHECK(ReadText(Base + "/etc/passwd").find("fex:") == std::string::npos);
  RemoveAll(Base);
  RemoveAll("/tmp/guest-rootfs-install-empty-env");
}

// ---------------------------------------------------------------------------
// Extraction of a real squashfs fixture.

static int Extract(const std::string& Image, const std::string& Output) {
  RemoveAll(Output);
  MakeAll(Output);
  Squashfs::StdioImageSource Source(Image);
  CHECK(Source.Opened());
  Squashfs::Reader Reader(Source);
  const auto* Error = Reader.Open();
  if (Error) {
    std::printf("open: %s %s\n", Error, Reader.Detail.c_str());
    return 1;
  }
  ImageSize Size;
  CHECK(!MeasureImage(Reader, Size, {}));
  std::map<std::string, HostWriter::Meta> Table;
  HostWriter Writer(Output, Table);
  ExtractStats Stats;
  std::string Detail;
  ExtractHooks Hooks;
  Error = ExtractSquashfs(Reader, Writer, Hooks, Stats, Detail);
  if (Error) {
    std::printf("extract: %s %s\n", Error, Detail.c_str());
    return 1;
  }
  CHECK(Stats.Bytes == Size.Bytes);
  WriteMeta(Output, Table, Output + ".meta");
  std::ofstream Summary(Output + ".summary");
  Summary << "directories " << Stats.Directories << "\nfiles " << Stats.Files << "\nhardlinks " << Stats.HardLinks << "\nsymlinks " << Stats.Symlinks
          << "\nfifos " << Stats.Fifos << "\nbytes " << Stats.Bytes << "\ndevices " << Stats.SkippedDevices << "\n";
  return 0;
}

// Rejects what it should: wrong magic, other compressors, truncation.
static void RejectChecks(const std::string& Directory) {
  for (const char* Name : {"gzip.sqsh", "xz.sqsh"}) {
    Squashfs::StdioImageSource Source(Directory + "/" + Name);
    CHECK(Source.Opened());
    Squashfs::Reader Reader(Source);
    const auto* Error = Reader.Open();
    CHECK(Error && std::string(Error) == "Unsupported squashfs compression");
    CHECK(Reader.Detail.find("only zstd") != std::string::npos);
    CHECK(Reader.Detail.find(std::string_view(Name).substr(0, std::strlen(Name) - 5)) != std::string::npos);
  }
  {
    Squashfs::StdioImageSource Source(Directory + "/image.sqsh");
    auto Bytes = ReadText(Directory + "/image.sqsh");
    std::ofstream(Directory + "/truncated.sqsh", std::ios::binary).write(Bytes.data(), static_cast<std::streamsize>(Bytes.size() / 2));
    Squashfs::StdioImageSource Short(Directory + "/truncated.sqsh");
    Squashfs::Reader Reader(Short);
    const auto* Error = Reader.Open();
    CHECK(Error && std::string(Error).find("truncated") != std::string::npos);
    Bytes[0] = 'X';
    std::ofstream(Directory + "/magic.sqsh", std::ios::binary).write(Bytes.data(), static_cast<std::streamsize>(Bytes.size()));
    Squashfs::StdioImageSource Bad(Directory + "/magic.sqsh");
    Squashfs::Reader BadReader(Bad);
    CHECK(BadReader.Open() && std::string(BadReader.Open()).find("magic") != std::string::npos);
    // Flip bytes inside the data area: extraction must fail with an error, not crash.
    auto Corrupt = ReadText(Directory + "/image.sqsh");
    // Squashfs stores incompressible file data raw (no checksum), so damage the compressed inode table.
    Squashfs::StdioImageSource Good(Directory + "/image.sqsh");
    Squashfs::Reader GoodReader(Good);
    CHECK(!GoodReader.Open());
    const auto Table = static_cast<size_t>(GoodReader.Info().InodeTable);
    for (size_t Offset = Table + 2; Offset < Table + 40; ++Offset) Corrupt[Offset] = static_cast<char>(Corrupt[Offset] ^ 0x5a);
    std::ofstream(Directory + "/corrupt.sqsh", std::ios::binary).write(Corrupt.data(), static_cast<std::streamsize>(Corrupt.size()));
    Squashfs::StdioImageSource Garbled(Directory + "/corrupt.sqsh");
    Squashfs::Reader GarbledReader(Garbled);
    if (!GarbledReader.Open()) {
      ImageSize Size;
      std::map<std::string, HostWriter::Meta> Table;
      RemoveAll(Directory + "/corrupt-out");
      MakeAll(Directory + "/corrupt-out");
      HostWriter Writer(Directory + "/corrupt-out", Table);
      ExtractStats Stats;
      std::string Detail;
      const auto* Result = ExtractSquashfs(GarbledReader, Writer, {}, Stats, Detail);
      (void)Size;
      CHECK(Result != nullptr);
    }
  }
}

// ---------------------------------------------------------------------------
// Pipeline scenarios.

static ImageSpec SpecFor(const std::string& Image) {
  ImageSpec Spec;
  Spec.Name = "Test RootFS";
  Spec.FileName = "Test.sqsh";
  Spec.Url = "http://rootfs.test/Test/Test.sqsh";
  uint64_t Hash {};
  CHECK(HashFileXxh3(Image, Hash, [](uint64_t) { return true; }));
  Spec.Hash = Hex64(Hash);
  struct stat Info;
  CHECK(!stat(Image.c_str(), &Info));
  Spec.DownloadBytes = static_cast<uint64_t>(Info.st_size);
  Spec.InstalledBytes = 4 << 20;
  return Spec;
}

// Serves the fixture archive tools/CheckRootfsInstall.py wrote: InRelease and Packages.xz per suite, and the .deb files
// listed in apt/urls.txt ("<path under the archive> <file>").
static void ServeArchive(TestEnv& Env, const std::string& Directory) {
  const std::string Base = ArchiveBase;
  for (const auto* Suite : ArchiveSuites) {
    Env.Archive[Base + "/dists/" + Suite + "/InRelease"] = Directory + "/apt/" + Suite + ".InRelease";
    for (const auto* Component : ArchiveComponents)
      Env.Archive[Base + "/dists/" + Suite + "/" + Component + "/binary-amd64/Packages.xz"] = Directory + "/apt/" + Suite + "." + Component + ".Packages.xz";
  }
  std::ifstream Key(Directory + "/apt/archive-key.n", std::ios::binary);
  Env.ArchiveKey.assign(std::istreambuf_iterator<char>(Key), std::istreambuf_iterator<char>());
  CHECK(!Env.ArchiveKey.empty());
  std::ifstream List(Directory + "/apt/urls.txt");
  std::string Url, File;
  while (List >> Url >> File) Env.Archive[Base + "/" + Url] = Directory + "/apt/" + File;
}

static int Scenarios(const std::string& Directory) {
  const std::string Image = Directory + "/image.sqsh";
  const std::string Work = "/tmp/guest-rootfs-install-sd";
  const auto Fresh = [&](TestEnv& Env) {
    Env.ServedFile = Image;
  };
  const auto Clean = [&] { RemoveAll(Work); };

  // 1. A full install: everything lands, the marker is last, the download is removed.
  {
    Clean();
    TestEnv Env(Work);
    Fresh(Env);
    Env.Dns = {0x0100000a};
    auto Spec = SpecFor(Image);
    InstallOptions Options;
    Options.Extras = false;
    Installer Run(Env, Spec, Options);
    const auto Result = Run.Run();
    CHECK(Result.Ok);
    CHECK(Env.Monotonic && Env.LastFraction > 0.999);
    CHECK(Exists(Work + "/fex/config/rootfs.json"));
    CHECK(!Exists(Work + "/fex/.download/Test.sqsh") && !Exists(Work + "/fex/.download/Test.sqsh.part"));
    CHECK(!Exists(Work + "/fex/rootfs/chroot"));
    CHECK(Exists(Work + "/fex/rootfs/home/fex") && Exists(Work + "/fex/rootfs/etc/passwd") && Exists(Work + "/fex/rootfs/usr/bin/hello"));
    CHECK(ReadText(Work + "/fex/rootfs/usr/bin/hello") == "hello\n");
    const auto Marker = ReadText(Work + "/fex/config/rootfs.json");
    CHECK(Marker.find("\"installed\": true") != std::string::npos && Marker.find("\"hash_kind\": \"xxh3-64\"") != std::string::npos);
    CHECK(Marker.find("\"verified\": true") != std::string::npos);
    CHECK(Env.Steps.size() == 7);
    CHECK(Env.Offsets.size() == 1 && Env.Offsets[0] == 0);
    CHECK(HasLog(Env, LogKind::Ok, "XXH3-64"));
    CHECK(Run.Extracted.SkippedDevices >= 1); // the fixture has device nodes the runtime cannot create
    // The image builder's uid/gid 1000 become root; other owners stay; the default user's home stays 1000.
    CHECK(Env.Table["/etc/sudoers.d"].Uid == 0 && Env.Table["/etc/sudoers.d"].Gid == 0);
    // The root folder gets the image root's owner (re-owned) and mode too: it is not created, it already exists.
    CHECK(Env.Table.count("/") && Env.Table["/"].Uid == 0 && Env.Table["/"].Gid == 0 && Env.Table["/"].Mode == 0755);
    CHECK(Env.Table["/etc/sudoers.d/README"].Uid == 0 && Env.Table["/etc/sudoers.d/README"].Gid == 0 && Env.Table["/etc/sudoers.d/README"].Mode == 0440);
    CHECK(Env.Table["/usr/bin/setuid-probe"].Uid == 12 && Env.Table["/usr/bin/setuid-probe"].Gid == 34);
    CHECK(Env.Table["/home/fex"].Uid == 1000 && Env.Table["/home/fex"].Gid == 1000);
    // /etc/hosts resolves the runtime's host name (sudo looks it up).
    CHECK(ReadText(Work + "/fex/rootfs/etc/hosts").find("127.0.1.1\tfex-switch2\n") != std::string::npos);
    std::puts("ok install_full");
  }
  // 2. Wrong hash: no marker, rootfs wiped, bad download removed.
  {
    Clean();
    TestEnv Env(Work);
    Fresh(Env);
    auto Spec = SpecFor(Image);
    Spec.Hash = "deadbeef";
    InstallOptions Options;
    Options.Extras = false;
    const auto Result = Installer(Env, Spec, Options).Run();
    CHECK(!Result.Ok && !Result.Cancelled && Result.Error.find("XXH3 hash") != std::string::npos);
    CHECK(!Exists(Work + "/fex/config/rootfs.json") && !Env.RootfsPopulated() && !Exists(Work + "/fex/.download/Test.sqsh"));
    std::puts("ok install_bad_hash");
  }
  // 3. Resume: a .part with the first third is continued with a Range request, and the result verifies.
  {
    Clean();
    TestEnv Env(Work);
    Fresh(Env);
    auto Spec = SpecFor(Image);
    const auto Bytes = ReadText(Image);
    MakeAll(Work + "/fex/.download");
    const size_t Have = Bytes.size() / 3;
    std::ofstream(Work + "/fex/.download/Test.sqsh.part", std::ios::binary).write(Bytes.data(), static_cast<std::streamsize>(Have));
    InstallOptions Options;
    Options.Extras = false;
    const auto Result = Installer(Env, Spec, Options).Run();
    CHECK(Result.Ok);
    CHECK(Env.Offsets.size() == 1 && Env.Offsets[0] == Have);
    std::puts("ok install_resume");
  }
  // 4. The server drops the connection once: the installer retries from the received offset.
  {
    Clean();
    TestEnv Env(Work);
    Fresh(Env);
    Env.FailAfter = 40000;
    auto Spec = SpecFor(Image);
    InstallOptions Options;
    Options.Extras = false;
    const auto Result = Installer(Env, Spec, Options).Run();
    CHECK(Result.Ok);
    CHECK(Env.Offsets.size() == 2 && Env.Offsets[0] == 0 && Env.Offsets[1] == 40000);
    CHECK(HasLog(Env, LogKind::Warn, "Download interrupted"));
    std::puts("ok install_retry_offset");
  }
  // 5. A server without Range support: the partial file is started over, no corruption.
  {
    Clean();
    TestEnv Env(Work);
    Fresh(Env);
    Env.SupportRange = false;
    auto Spec = SpecFor(Image);
    const auto Bytes = ReadText(Image);
    MakeAll(Work + "/fex/.download");
    std::ofstream(Work + "/fex/.download/Test.sqsh.part", std::ios::binary).write(Bytes.data(), 5000);
    InstallOptions Options;
    Options.Extras = false;
    const auto Result = Installer(Env, Spec, Options).Run();
    CHECK(Result.Ok);
    CHECK(HasLog(Env, LogKind::Warn, "does not support resuming"));
    std::puts("ok install_no_range");
  }
  // 6. Cancel: no rootfs or marker stays; the download stays only once it passed its hash check.
  {
    Clean();
    TestEnv Env(Work);
    Fresh(Env);
    auto Spec = SpecFor(Image);
    // Count pumps of a full run first, then cancel in the middle of extraction.
    TestEnv Probe(Work + "-probe");
    Probe.ServedFile = Image;
    InstallOptions Options;
    Options.Extras = false;
    CHECK(Installer(Probe, Spec, Options).Run().Ok);
    const int Total = Probe.Pumps;
    RemoveAll(Work + "-probe");
    CHECK(Total > 6);
    for (int At : {1, Total / 3, Total * 2 / 3, Total - 2}) {
      Clean();
      TestEnv Cut(Work);
      Cut.ServedFile = Image;
      Cut.CancelAfterPumps = At;
      const auto Result = Installer(Cut, Spec, Options).Run();
      CHECK(!Result.Ok && Result.Cancelled);
      CHECK(!Exists(Work + "/fex/config/rootfs.json"));
      CHECK(!Cut.RootfsPopulated());
      CHECK(!Exists(Work + "/fex/.download/Test.sqsh.part"));
      // Pump 1 is during the download; the last pumps are during extraction.
      if (At == 1) CHECK(!Exists(Work + "/fex/.download/Test.sqsh"));
      if (At == Total - 2) CHECK(Exists(Work + "/fex/.download/Test.sqsh"));
    }
    // A kept image is reused: the next install downloads nothing.
    {
      TestEnv Again(Work);
      Again.ServedFile = Image;
      CHECK(Installer(Again, Spec, Options).Run().Ok);
      CHECK(Again.Offsets.empty());
    }
    std::puts("ok install_cancel_cleanup");
  }
  // 7. Not enough space; existing RootFS without Replace; Replace wipes it.
  {
    Clean();
    TestEnv Env(Work);
    Fresh(Env);
    Env.Free = 1 << 20;
    auto Spec = SpecFor(Image);
    InstallOptions Options;
    Options.Extras = false;
    auto Result = Installer(Env, Spec, Options).Run();
    CHECK(!Result.Ok && Result.Error.find("Not enough free space") != std::string::npos && Env.Offsets.empty());
    Env.Free = 1ull << 40;
    {
      HostWriter Writer(Work + "/fex/rootfs", Env.Table);
      CHECK(Writer.WriteFile("/old", RootfsSetup::Bytes("x"), 0644, 0, 0, 0) == 0);
    }
    Result = Installer(Env, Spec, Options).Run();
    CHECK(!Result.Ok && Result.Error.find("not empty") != std::string::npos);
    CHECK(Exists(Work + "/fex/rootfs/old")); // an untouched RootFS is not wiped by a refused install
    Options.Replace = true;
    Result = Installer(Env, Spec, Options).Run();
    CHECK(Result.Ok && !Exists(Work + "/fex/rootfs/old") && Exists(Work + "/fex/rootfs/usr/bin/hello"));
    std::puts("ok install_space_and_replace");
  }
  // 8. Local image: no download; the index hash is checked when the card file has the index file name, a user image
  // with another name installs unverified with a warning, and a bad image never touches the installed RootFS.
  {
    Clean();
    TestEnv Env(Work);
    auto Spec = SpecFor(Image);
    MakeAll(Work + "/fex/images");
    CHECK(std::system(("cp '" + Image + "' '" + Work + "/fex/images/Test.sqsh'").c_str()) == 0);
    Spec.LocalPath = "/fex/images/Test.sqsh";
    InstallOptions Options;
    Options.Extras = false;
    auto Result = Installer(Env, Spec, Options).Run();
    CHECK(Result.Ok && Env.Offsets.empty() && Exists(Work + "/fex/images/Test.sqsh"));
    bool Matched = false;
    for (const auto& [Kind, Line] : Env.Logs) Matched |= Line.find("matches") != std::string::npos;
    CHECK(Matched);
    std::puts("ok install_local_image");

    // A user image (another name) installs over the RootFS, unverified.
    {
      HostWriter Writer(Work + "/fex/rootfs", Env.Table);
      CHECK(Writer.WriteFile("/old", RootfsSetup::Bytes("x"), 0644, 0, 0, 0) == 0);
    }
    CHECK(std::system(("cp '" + Image + "' '" + Work + "/fex/images/mine.sqsh'").c_str()) == 0);
    Spec.LocalPath = "/fex/images/mine.sqsh";
    Options.Replace = true;
    Env.Logs.clear();
    Result = Installer(Env, Spec, Options).Run();
    bool Unverified = false;
    for (const auto& [Kind, Line] : Env.Logs) Unverified |= Kind == LogKind::Warn && Line.find("not verified") != std::string::npos;
    CHECK(Result.Ok && Unverified && !Exists(Work + "/fex/rootfs/old"));

    // A broken image or a missing file fails before the installed RootFS is removed.
    {
      HostWriter Writer(Work + "/fex/rootfs", Env.Table);
      CHECK(Writer.WriteFile("/old", RootfsSetup::Bytes("x"), 0644, 0, 0, 0) == 0);
    }
    CHECK(std::system(("echo not-a-squashfs-image > '" + Work + "/fex/images/broken.sqsh'").c_str()) == 0);
    Spec.LocalPath = "/fex/images/broken.sqsh";
    Result = Installer(Env, Spec, Options).Run();
    CHECK(!Result.Ok && Exists(Work + "/fex/rootfs/old") && Exists(Work + "/fex/config/rootfs.json"));
    Spec.LocalPath = "/fex/images/missing.sqsh";
    Result = Installer(Env, Spec, Options).Run();
    CHECK(!Result.Ok && Exists(Work + "/fex/rootfs/old") && Exists(Work + "/fex/config/rootfs.json"));
    std::puts("ok install_local_image_keeps_rootfs");
  }
  // 8a. A reinstall whose download or hash fails keeps the installed RootFS; with too little space for the download next
  // to it, the old RootFS is removed first.
  {
    Clean();
    TestEnv Env(Work);
    Fresh(Env);
    auto Spec = SpecFor(Image);
    InstallOptions Options;
    Options.Extras = false;
    CHECK(Installer(Env, Spec, Options).Run().Ok);
    {
      HostWriter Writer(Work + "/fex/rootfs", Env.Table);
      CHECK(Writer.WriteFile("/old", RootfsSetup::Bytes("x"), 0644, 0, 0, 0) == 0);
    }
    Options.Replace = true;
    Env.ServedFile.clear(); // the server refuses
    auto Result = Installer(Env, Spec, Options).Run();
    CHECK(!Result.Ok && Exists(Work + "/fex/rootfs/old") && Exists(Work + "/fex/config/rootfs.json"));
    Env.ServedFile = Image;
    Spec.Hash = "0123456789abcdef"; // wrong hash
    Result = Installer(Env, Spec, Options).Run();
    CHECK(!Result.Ok && Result.Error.find("XXH3") != std::string::npos && Exists(Work + "/fex/rootfs/old"));
    Spec = SpecFor(Image);
    Spec.DownloadBytes = 1ull << 41; // a hostile index size
    Result = Installer(Env, Spec, Options).Run();
    CHECK(!Result.Ok && Result.Error.find("not valid") != std::string::npos && Exists(Work + "/fex/rootfs/old"));
    Spec = SpecFor(Image);
    Spec.DownloadBytes -= 100; // the server sends more than the index size: stop at the size
    Result = Installer(Env, Spec, Options).Run();
    CHECK(!Result.Ok && Result.Error.find("more data") != std::string::npos && Exists(Work + "/fex/rootfs/old"));
    Spec = SpecFor(Image);
    Env.Free = Spec.DownloadBytes / 2; // no room for the download next to the old RootFS
    Result = Installer(Env, Spec, Options).Run();
    CHECK(!Result.Ok && Result.Error.find("Not enough free space") != std::string::npos && !Exists(Work + "/fex/rootfs/old"));
    Env.Free = 1ull << 40;
    Result = Installer(Env, Spec, Options).Run();
    CHECK(Result.Ok && Exists(Work + "/fex/rootfs/usr/bin/hello"));
    std::puts("ok install_replace_keeps_rootfs_until_verified");
  }
  // 8b. A live index entry has no size: the installer learns it from the response head.
  {
    Clean();
    TestEnv Env(Work);
    Fresh(Env);
    auto Spec = SpecFor(Image);
    Spec.DownloadBytes = 0;
    Spec.InstalledBytes = 0;
    InstallOptions Options;
    Options.Extras = false;
    const auto Result = Installer(Env, Spec, Options).Run();
    CHECK(Result.Ok);
    CHECK(Env.Offsets.size() == 2 && Env.Offsets[0] == 0 && Env.Offsets[1] == 0); // the size probe, then the download
    std::puts("ok install_unknown_size");
  }
  // 9. Extra packages from a fake archive (InRelease, Packages.xz and .deb fixtures).
  {
    Clean();
    TestEnv Env(Work);
    Fresh(Env);
    ServeArchive(Env, Directory);
    auto Spec = SpecFor(Image);
    InstallOptions Options;
    Options.Extras = true;
    Installer Run(Env, Spec, Options);
    const auto Result = Run.Run();
    if (!Result.Ok) std::printf("extras failed: %s\n", Result.Error.c_str());
    CHECK(Result.Ok);
    CHECK(Env.ConfigureCalled && Run.ExtrasConfigured);
    CHECK(Run.ExtrasInstalled.size() == 12); // bubblewrap, netcat-openbsd, libbsd0 (its dependency), inotify-tools, libinotifytools0,
                                             // sudo, nano, wget, whiptail, bsdutils (script), util-linux (su), mount
    // Dependencies come before dependents; libc6 is in the image already.
    const auto Position = [&](std::string_view Name) {
      for (size_t Index = 0; Index < Run.ExtrasInstalled.size(); ++Index)
        if (Run.ExtrasInstalled[Index].starts_with(std::string(Name) + " ")) return static_cast<int>(Index);
      return -1;
    };
    CHECK(Position("libinotifytools0") >= 0 && Position("libinotifytools0") < Position("inotify-tools"));
    CHECK(Position("bubblewrap") >= 0 && Position("netcat-openbsd") >= 0);
    CHECK(Position("libbsd0") >= 0 && Position("libbsd0") < Position("netcat-openbsd"));
    CHECK(ReadText(Work + "/fex/rootfs/usr/bin/bwrap") == "bwrap\n" && ReadText(Work + "/fex/rootfs/usr/bin/inotifywait") == "inotifywait\n");
    CHECK(Exists(Work + "/fex/rootfs/usr/lib/x86_64-linux-gnu/libinotifytools.so.0"));
    const auto Status = ReadText(Work + "/fex/rootfs/var/lib/dpkg/status");
    CHECK(Status.find("Package: bubblewrap\nStatus: install ok unpacked\n") != std::string::npos);
    CHECK(Status.find("Package: libc6\nStatus: install ok installed\n") != std::string::npos); // the image's entries stay
    // The image's removed-but-remembered sudo entry is replaced, not duplicated (dpkg refuses two "sudo" instances).
    CHECK(Status.find("Package: sudo\nStatus: install ok unpacked\n") != std::string::npos);
    CHECK(Status.find("deinstall ok config-files") == std::string::npos);
    CHECK(Status.find("Package: sudo\n") == Status.rfind("Package: sudo\n"));
    // script, su and mount: bsdutils was not in the image; util-linux and mount were listed installed without their files.
    CHECK(ReadText(Work + "/fex/rootfs/usr/bin/script") == "bsdutils\n" && ReadText(Work + "/fex/rootfs/usr/bin/su") == "util-linux\n" &&
          ReadText(Work + "/fex/rootfs/usr/bin/mount") == "mount\n");
    CHECK(Status.find("Package: util-linux\nStatus: install ok unpacked\n") != std::string::npos && Status.find("Package: util-linux\n") == Status.rfind("Package: util-linux\n"));
    CHECK(Status.find("Package: mount\n") == Status.rfind("Package: mount\n"));
    CHECK(Exists(Work + "/fex/rootfs/var/lib/dpkg/info/bubblewrap.list") && Exists(Work + "/fex/rootfs/var/lib/dpkg/info/bubblewrap.md5sums"));
    CHECK(!Exists(Work + "/fex/.download/apt/noble_main.xz") && !Exists(Work + "/fex/.download/apt/noble_universe.xz"));
    CHECK(ReadText(Work + "/fex/config/rootfs.json").find("bubblewrap 0.9.0-1") != std::string::npos);
    std::puts("ok install_extras");
  }
  // 10. A package whose hash differs from the index is refused and the install fails cleanly.
  {
    Clean();
    TestEnv Env(Work);
    Fresh(Env);
    ServeArchive(Env, Directory);
    Env.Archive[std::string(ArchiveBase) + "/pool/main/b/bubblewrap/bubblewrap_0.9.0-1_amd64.deb"] = Directory + "/apt/tampered.deb";
    auto Spec = SpecFor(Image);
    InstallOptions Options;
    const auto Result = Installer(Env, Spec, Options).Run();
    CHECK(!Result.Ok && !Exists(Work + "/fex/config/rootfs.json") && !Env.RootfsPopulated());
    std::printf("  (%s)\n", Result.Error.c_str());
    std::puts("ok install_extras_bad_hash");
  }
  // 10b. An InRelease whose signed text was changed, one signed by another key, and one checked against the real Ubuntu
  // key are refused before any index or package is fetched.
  for (const char* Case : {"tampered", "otherkey", "ubuntu"}) {
    Clean();
    TestEnv Env(Work);
    Fresh(Env);
    ServeArchive(Env, Directory);
    const std::string Fixture = std::string(Case) == "ubuntu" ? "noble-updates" : Case;
    Env.Archive[std::string(ArchiveBase) + "/dists/noble-updates/InRelease"] = Directory + "/apt/" + Fixture + ".InRelease";
    if (std::string(Case) == "ubuntu") Env.ArchiveKey.clear();  // the fixture key is not the Ubuntu archive key
    auto Spec = SpecFor(Image);
    InstallOptions Options;
    const auto Result = Installer(Env, Spec, Options).Run();
    CHECK(!Result.Ok && Result.Error.find("not signed by the Ubuntu archive key") != std::string::npos);
    CHECK(!Exists(Work + "/fex/config/rootfs.json") && !Env.RootfsPopulated());
    std::printf("  %s: (%s)\n", Case, Result.Error.c_str());
  }
  Clean();
  return 0;
}

// A full install of a real image (and optionally the real Ubuntu archive extras).
static int InstallReal(const std::string& Image, const std::string& SdRoot, const std::string& Hash, bool Extras) {
  ApplyModes = true;
  RemoveAll(SdRoot);
  struct RealEnv final : TestEnv {
    using TestEnv::TestEnv;
    const char* HttpGet(const std::string& Url, uint64_t Offset, const Http::BodyFn& Body, Http::ResponseInfo& Info, const Http::HeadFn& OnHead) override {
      if (Url.ends_with(".sqsh")) return TestEnv::HttpGet(Url, Offset, Body, Info, OnHead);
      return Http::Get(Url, Offset, Body, Info, 30, OnHead);
    }
    void Log(LogKind Kind, const std::string& Line) override {
      std::printf("[%s] %s\n", Kind == LogKind::Ok ? "ok" : Kind == LogKind::Warn ? "warn" : Kind == LogKind::Error ? "error" : "info", Line.c_str());
      TestEnv::Log(Kind, Line);
    }
    void Progress(double Fraction, const std::string& Detail) override {
      if (Fraction - Printed > 0.05) {
        Printed = Fraction;
        std::printf("%5.1f%%  %s\n", Fraction * 100, Detail.c_str());
      }
      TestEnv::Progress(Fraction, Detail);
    }
    double Printed {};
  } Env(SdRoot);
  Env.ServedFile = Image;
  ImageSpec Spec;
  Spec.Name = "Ubuntu 24.04";
  Spec.FileName = "Ubuntu_24_04.sqsh";
  Spec.Url = "http://rootfs.fex-emu.gg/Ubuntu_24_04/2026-08-11/Ubuntu_24_04.sqsh";
  Spec.Hash = Hash;
  struct stat Info;
  CHECK(!stat(Image.c_str(), &Info));
  Spec.DownloadBytes = static_cast<uint64_t>(Info.st_size);
  Spec.InstalledBytes = 2200ull << 20;
  InstallOptions Options;
  Options.Extras = Extras;
  Installer Run(Env, Spec, Options);
  const auto Result = Run.Run();
  WriteMeta(SdRoot + "/fex/rootfs", Env.Table, SdRoot + "/rootfs.meta");
  std::printf("result: %s %s %s\n", Result.Ok ? "OK" : "FAILED", Result.Error.c_str(), Result.Warning.c_str());
  return Result.Ok ? 0 : 1;
}

// Host run of the download probe's cases 3, 5 and 6 against a real URL: guest-rootfs-install-test range-probe URL
static int RangeProbe(const std::string& Url) {
  Http::ResponseInfo Info;
  uint64_t Got = 0;
  auto Take = [&](uint64_t Offset, uint64_t Limit, uint64_t& Hash) {
    auto* State = XXH3_createState();
    XXH3_64bits_reset(State);
    Got = 0;
    const auto* Error = Http::Get(Url, Offset, [&](const uint8_t* Data, size_t Count) {
      const size_t Use = static_cast<size_t>(std::min<uint64_t>(Count, Limit - Got));
      XXH3_64bits_update(State, Data, Use);
      Got += Use;
      return Got < Limit;
    }, Info);
    Hash = XXH3_64bits_digest(State);
    XXH3_freeState(State);
    return Error;
  };
  uint64_t Prefix {}, Slice {};
  auto* Error = Take(0, 64ull << 20, Prefix);
  std::printf("prefix: %s status=%d got=%llu hash=%s\n", Error ? Error : "ok", Info.Status, (unsigned long long)Got, Hex64(Prefix).c_str());
  Error = Take(32ull << 20, 16ull << 20, Slice);
  std::printf("slice: %s status=%d range_start=%llu total=%llu got=%llu hash=%s\n", Error ? Error : "ok", Info.Status,
              (unsigned long long)Info.RangeStart.value_or(0), (unsigned long long)Info.TotalSize.value_or(0), (unsigned long long)Got, Hex64(Slice).c_str());
  return 0;
}

// The extraction cost model: an image whose entries pile up at its end (many small files last) costs far more time per byte there.
// Truth for the test: seconds = 2e-8 x bytes + 2e-4 x entries over 2 GB and 200k entries, entries done = total x x^3 at byte share x.
static void CostModelChecks() {
  constexpr double Bytes = 2e9, Entries = 2e5, A = 2e-8, B = 2e-4;
  const auto Cost = [&](double X) { return A * Bytes * X + B * Entries * X * X * X; };
  const double Total = Cost(1);
  ExtractCostModel Model;
  Model.SetTotals(Bytes, Entries);
  CHECK(Model.Fraction(0, 0) == 0.0 && Model.Fraction(Bytes / 2, 1) == 0.5);  // before any sample: by bytes
  double Time = 0;
  double X = 0;
  const auto Run = [&](double Until) {
    while (X < Until) {
      X += 0.002;
      // time of this point on the true curve, sampled when 0.25 s have passed since the last sample
      if (Cost(X) - Time >= 0.25) {
        Time = Cost(X);
        Model.Add(Time, Bytes * X, Entries * X * X * X);
      }
    }
  };
  Run(0.5);
  const auto Predicted = [&](double At) { return Model.Fraction(Bytes * At, Entries * At * At * At); };
  const auto Truth = [&](double At) { return Cost(At) / Total; };
  CHECK(std::fabs(Predicted(0.5) - Truth(0.5)) < 0.03);        // by bytes alone it would say 0.5 for a true 0.31
  CHECK(std::fabs(Predicted(0.8) - Truth(0.8)) < 0.06);        // the not yet seen tail is extrapolated from the fit
  CHECK(std::fabs(Model.SecondsPerByte() - A) < A * 0.1 && std::fabs(Model.SecondsPerEntry() - B) < B * 0.1);
  Run(0.9);
  CHECK(std::fabs(Predicted(0.9) - Truth(0.9)) < 0.01 && Predicted(1.0) > 0.999);
  CHECK(Predicted(0.8) < 0.8 - 0.1);  // the last fifth of the bytes is more than a fifth of the time
  // Bytes and entries growing in step (collinear): the fit stays by bytes only.
  ExtractCostModel Flat;
  Flat.SetTotals(Bytes, Entries);
  for (int I = 1; I <= 40; ++I) Flat.Add(I * 0.5, Bytes * I / 80.0, Entries * I / 80.0);
  CHECK(Flat.SecondsPerEntry() == 0.0 && std::fabs(Flat.Fraction(Bytes / 4, Entries / 4) - 0.25) < 1e-9);
}

int main(int argc, char** argv) {
  const std::string Mode = argc > 1 ? argv[1] : "";
  if (Mode == "range-probe" && argc == 3) return RangeProbe(argv[2]);
  if (Mode == "extract" && argc == 4) return Extract(argv[2], argv[3]);
  if (Mode == "scenarios" && argc == 3) {
    RejectChecks(argv[2]);
    std::puts("ok reject_images");
    return Scenarios(argv[2]);
  }
  if (Mode == "install" && argc >= 4) {
    bool Extras = false;
    std::string Hash;
    for (int Index = 4; Index < argc; ++Index) {
      if (std::string(argv[Index]) == "--extras") Extras = true;
      else Hash = argv[Index];
    }
    return InstallReal(argv[2], argv[3], Hash, Extras);
  }
  HashChecks();
  HttpChecks();
  DependsChecks();
  UnbreakChecks();
  AptMirrorChecks();
  SetupOnEmptyTree();
  CostModelChecks();
  std::puts("guest rootfs install: unit ok");
  return 0;
}
