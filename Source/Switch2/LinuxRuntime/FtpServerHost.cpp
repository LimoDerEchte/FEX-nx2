// SPDX-License-Identifier: MIT
// Serves a host directory through GuestFtp.h for protocol tests. Guest
// paths are joined below the root directory and run against the host
// kernel, which is the reference for the error codes the console reports.
//
// Usage: ftp-server-host <root> <password>
// Prints "port <n>" once listening, then serves until killed.
#include "../GuestFtp.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdlib>

using namespace Switch2Guest;

class HostFiles final : public FtpFileSystem {
public:
  explicit HostFiles(std::string Root) : Root {std::move(Root)} {}

  int64_t Stat(const std::string& Path, FtpEntry& Entry, bool NoFollow) override {
    struct stat Info {};
    if ((NoFollow ? lstat : stat)(Host(Path).c_str(), &Info)) return -errno;
    Fill(Entry, Info);
    return 0;
  }

  int64_t List(const std::string& Path, std::vector<FtpEntry>& Entries) override {
    const int FD = open(Host(Path).c_str(), O_RDONLY | O_DIRECTORY);
    if (FD < 0) return -errno;
    DIR* Stream = fdopendir(FD);
    if (!Stream) {
      close(FD);
      return -errno;
    }
    while (const auto* Item = readdir(Stream)) {
      if (!std::strcmp(Item->d_name, ".") || !std::strcmp(Item->d_name, "..")) continue;
      struct stat Info {};
      if (fstatat(FD, Item->d_name, &Info, AT_SYMLINK_NOFOLLOW)) continue;
      FtpEntry Entry {};
      Fill(Entry, Info);
      Entry.Name = Item->d_name;
      Entries.push_back(std::move(Entry));
    }
    closedir(Stream);
    return 0;
  }

  int64_t OpenRead(const std::string& Path, uint64_t Offset) override {
    const int FD = open(Host(Path).c_str(), O_RDONLY | O_NONBLOCK);
    if (FD < 0) return -errno;
    if (lseek(FD, Offset, SEEK_SET) < 0) {
      const int Error = -errno;
      close(FD);
      return Error;
    }
    return FD;
  }

  int64_t OpenWrite(const std::string& Path, uint64_t Offset, bool Append) override {
    const int Flags = O_WRONLY | O_CREAT | O_NONBLOCK | (Append ? O_APPEND : Offset ? 0 : O_TRUNC);
    const int FD = open(Host(Path).c_str(), Flags, 0644);
    if (FD < 0) return -errno;
    if (!Append && Offset && lseek(FD, Offset, SEEK_SET) < 0) {
      const int Error = -errno;
      close(FD);
      return Error;
    }
    return FD;
  }

  int64_t Read(int64_t Handle, void* Buffer, size_t Count) override {
    const auto Result = read(Handle, Buffer, Count);
    return Result < 0 ? -errno : Result;
  }

  int64_t Write(int64_t Handle, const void* Buffer, size_t Count) override {
    const auto Result = write(Handle, Buffer, Count);
    return Result < 0 ? -errno : Result;
  }

  int64_t Close(int64_t Handle) override { return close(Handle) ? -errno : 0; }

  int64_t EnterDirectory(const std::string& Path) override {
    struct stat Info {};
    if (stat(Host(Path).c_str(), &Info)) return -errno;
    if (!S_ISDIR(Info.st_mode)) return -ENOTDIR;
    return access(Host(Path).c_str(), X_OK) ? -errno : 0;
  }

  int64_t MakeDirectory(const std::string& Path) override { return mkdir(Host(Path).c_str(), 0755) ? -errno : 0; }
  int64_t RemoveDirectory(const std::string& Path) override { return rmdir(Host(Path).c_str()) ? -errno : 0; }
  int64_t Remove(const std::string& Path) override { return unlink(Host(Path).c_str()) ? -errno : 0; }
  int64_t Rename(const std::string& From, const std::string& To) override {
    return rename(Host(From).c_str(), Host(To).c_str()) ? -errno : 0;
  }

  int64_t SetModifyTime(const std::string& Path, int64_t Seconds) override {
    const timespec Times[2] {{0, UTIME_OMIT}, {Seconds, 0}};
    return utimensat(AT_FDCWD, Host(Path).c_str(), Times, 0) ? -errno : 0;
  }

private:
  std::string Root;

  // The guest view's ".." cannot leave the root; resolving lexically here
  // is enough for a test fixture without symlinks pointing outside.
  std::string Host(const std::string& Path) const { return Root + Path; }

  static void Fill(FtpEntry& Entry, const struct stat& Info) {
    Entry.Mode = Info.st_mode;
    Entry.Links = Info.st_nlink;
    Entry.User = Info.st_uid;
    Entry.Group = Info.st_gid;
    Entry.Size = Info.st_size;
    Entry.ModifySeconds = Info.st_mtim.tv_sec;
  }
};

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "Usage: %s root password\n", argv[0]);
    return 2;
  }
  HostFiles Files {argv[1]};
  FtpServer Server {Files, argv[2]};
  if (const int Error = Server.Start(0)) {
    std::fprintf(stderr, "listen failed: %s\n", std::strerror(-Error));
    return 1;
  }
  std::printf("port %u\n", Server.Port());
  std::fflush(stdout);
  for (;;) Server.Step(1000);
}
