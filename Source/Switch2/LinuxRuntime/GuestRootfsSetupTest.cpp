// SPDX-License-Identifier: MIT
// Unpacks .deb files with GuestRootfsSetup.h into a host directory, for
// CheckRootfsSetup.py to compare against Python's ar/tar readers.
// Usage: guest-rootfs-setup-test OUTPUT_DIR MANIFEST DEB...
#include "../GuestRootfsSetup.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <unistd.h>

using namespace Switch2Guest::RootfsSetup;

#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      std::exit(1);                                           \
    }                                                         \
  } while (0)

// Writes below Base. Ownership is recorded in a side file instead of chown
// so the test runs unprivileged.
class HostWriter final : public RootfsWriter {
public:
  explicit HostWriter(std::string Base) : Base {std::move(Base)} {}
  std::string Owners;

  int64_t MakeDirectory(const std::string& Path, uint32_t Mode, uint32_t Uid, uint32_t Gid) override {
    struct stat Info;
    if (!lstat(Full(Path).c_str(), &Info)) {
      if (S_ISLNK(Info.st_mode) && !stat(Full(Path).c_str(), &Info) && S_ISDIR(Info.st_mode)) return 0;
      return S_ISDIR(Info.st_mode) ? 0 : -EEXIST;
    }
    if (mkdir(Full(Path).c_str(), 0700)) return -errno;
    Own(Path, Uid, Gid);
    return chmod(Full(Path).c_str(), Mode) ? -errno : 0;
  }

  int64_t WriteFile(const std::string& Path, std::span<const uint8_t> Data, uint32_t Mode, uint32_t Uid, uint32_t Gid,
                    int64_t ModifyTime) override {
    const auto Temporary = Full(Path) + ".dpkg-new";
    const int FD = open(Temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (FD < 0) return -errno;
    if (!Data.empty() && write(FD, Data.data(), Data.size()) != static_cast<ssize_t>(Data.size())) {
      close(FD);
      return -EIO;
    }
    fchmod(FD, Mode);
    close(FD);
    const struct timeval Times[2] {{ModifyTime, 0}, {ModifyTime, 0}};
    utimes(Temporary.c_str(), Times);
    Own(Path, Uid, Gid);
    return rename(Temporary.c_str(), Full(Path).c_str()) ? -errno : 0;
  }

  int64_t Symlink(const std::string& Target, const std::string& Path, uint32_t Uid, uint32_t Gid) override {
    unlink(Full(Path).c_str());
    Own(Path, Uid, Gid);
    return symlink(Target.c_str(), Full(Path).c_str()) ? -errno : 0;
  }

  int64_t HardLink(const std::string& Existing, const std::string& Path) override {
    unlink(Full(Path).c_str());
    return link(Full(Existing).c_str(), Full(Path).c_str()) ? -errno : 0;
  }

  int64_t MakeNode(const std::string& Path, uint32_t Mode, uint64_t, uint32_t Uid, uint32_t Gid) override {
    // Devices need privilege; record them as empty files with the type in Owners.
    Own(Path, Uid, Gid);
    Owners += "node " + Path + " " + std::to_string(Mode) + "\n";
    return WriteFile(Path, {}, Mode & 07777, Uid, Gid, 0);
  }

private:
  std::string Base;
  std::string Full(const std::string& Path) const { return Base + Path; }
  void Own(const std::string& Path, uint32_t Uid, uint32_t Gid) {
    Owners += "owner " + Path + " " + std::to_string(Uid) + " " + std::to_string(Gid) + "\n";
  }
};

static std::vector<uint8_t> ReadAll(const char* Path) {
  std::ifstream File(Path, std::ios::binary);
  return {std::istreambuf_iterator<char>(File), {}};
}

static void UnitChecks() {
  CHECK(NormalizeMemberPath("./usr/bin/") == "/usr/bin");
  CHECK(NormalizeMemberPath("./") == "/");
  CHECK(!NormalizeMemberPath("./../etc/passwd"));
  CHECK(!NormalizeMemberPath("usr/../../x"));
  const auto Head = ParseResponseHead("HTTP/1.1 200 OK\r\nContent-Length: 12\r\nTransfer-Encoding: Chunked\r\n");
  CHECK(Head.Status == 200 && Head.ContentLength == 12u && Head.Chunked);
  CHECK(ParseResponseHead("HTTP/1.1 302 Found\r\nLocation: http://x/y\r\n").Location == "http://x/y");
  std::vector<uint8_t> Body;
  for (char C : std::string_view("4\r\nWiki\r\n5;x=1\r\npedia\r\n0\r\n\r\n")) Body.push_back(C);
  CHECK(Dechunk(Body) && std::string(Body.begin(), Body.end()) == "Wikipedia");
  const auto Parsed = ParseHttpUrl("http://deb.debian.org:8080/debian/pool/a.deb");
  CHECK(Parsed && Parsed->Host == "deb.debian.org" && Parsed->Port == 8080 && Parsed->Path == "/debian/pool/a.deb");
  CHECK(!ParseHttpUrl("https://deb.debian.org/"));
  Manifest List;
  const std::string Sha(64, 'a');
  CHECK(!ParseManifest("suite bookworm\nsnapshot 20261122T000000Z\narchive debian http://deb.debian.org/debian\n"
                       "package debian pool/main/a/a.deb 10 " + Sha + " a 1.0\n",
                       List));
  CHECK(List.Packages.size() == 1 && List.TotalSize() == 10);
  CHECK(SnapshotUrl(List, List.Packages[0]) == "http://snapshot.debian.org/archive/debian/20261122T000000Z/pool/main/a/a.deb");
  CHECK(ParseManifest("archive debian http://d\npackage debian ../x 1 " + Sha + " a 1\n", List));
  CHECK(ParseManifest("archive debian http://d\npackage other pool/x 1 " + Sha + " a 1\n", List));
  InstalledPackage Package {"libc6:amd64", "Package: libc6\nVersion: 2.36\nMulti-Arch: same", {"/etc/ld.so.conf"}};
  CHECK(StatusParagraph(Package) ==
        "Package: libc6\nStatus: install ok unpacked\nVersion: 2.36\nMulti-Arch: same\nConffiles:\n /etc/ld.so.conf newconffile\n");
}

int main(int argc, char** argv) {
  UnitChecks();
  if (argc < 4) {
    std::puts("guest rootfs setup: unit ok");
    return 0;
  }
  const std::string Output = argv[1];
  HostWriter Writer(Output);
  CHECK(!PrepareDpkgDatabase(Writer));
  std::vector<InstalledPackage> Installed;
  for (int Index = 3; Index < argc; ++Index) {
    const auto Deb = ReadAll(argv[Index]);
    InstalledPackage Package;
    std::string Detail;
    if (const auto* Error = UnpackDeb(Deb, Writer, Package, Detail)) {
      std::printf("FAIL %s: %s %s\n", argv[Index], Error, Detail.c_str());
      return 1;
    }
    Installed.push_back(std::move(Package));
  }
  CHECK(Writer.WriteFile("/var/lib/dpkg/status", Bytes(StatusFile(Installed)), 0644, 0, 0, 0) == 0);
  std::ofstream(Output + ".owners") << Writer.Owners;
  std::printf("guest rootfs setup: unpacked %d packages\n", argc - 3);
}
