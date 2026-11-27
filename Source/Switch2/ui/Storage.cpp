// SPDX-License-Identifier: MIT
#include "Storage.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "PathUtil.h"

namespace ui {

std::string PosixStorage::Real(const std::string& Path) const {
  const std::string N = NormalizePath(Path);
  if (IsSdPath(N))
    return Sd + "/" + N.substr(6);
  if (IsRootfsPath(N))
    return Rootfs + N;
  return {};
}

bool PosixStorage::ReadFile(const std::string& Path, std::string& Out) {
  const std::string R = Real(Path);
  if (R.empty())
    return false;
  FILE* F = std::fopen(R.c_str(), "rb");
  if (!F)
    return false;
  Out.clear();
  char Buf[4096];
  size_t N;
  while ((N = std::fread(Buf, 1, sizeof(Buf), F)) > 0)
    Out.append(Buf, N);
  const bool Ok = !std::ferror(F);
  std::fclose(F);
  return Ok;
}

bool PosixStorage::MakeDirs(const std::string& Path) {
  const std::string R = Real(Path);
  if (R.empty())
    return false;
  std::string Cur;
  size_t I = 0;
  // Keep a "sdmc:" style device prefix out of mkdir: start at the first slash after it.
  while (I < R.size()) {
    size_t J = R.find('/', I + 1);
    if (J == std::string::npos)
      J = R.size();
    Cur = R.substr(0, J);
    I = J;
    if (Cur.empty() || Cur.back() == ':')
      continue;
    struct stat St;
    if (stat(Cur.c_str(), &St) == 0) {
      if (!S_ISDIR(St.st_mode))
        return false;
      continue;
    }
    if (mkdir(Cur.c_str(), 0777) != 0 && errno != EEXIST)
      return false;
  }
  return true;
}

bool PosixStorage::Rename(const std::string& From, const std::string& To) {
  if (std::rename(From.c_str(), To.c_str()) == 0)
    return true;
  // FAT (the SD card) cannot rename over an existing file on some stacks: remove the target and retry once.
  if (std::remove(To.c_str()) == 0 || errno == ENOENT)
    return std::rename(From.c_str(), To.c_str()) == 0;
  return false;
}

bool PosixStorage::RemoveFile(const std::string& Real) { return std::remove(Real.c_str()) == 0; }

bool PosixStorage::WriteFileAtomic(const std::string& Path, const std::string& Data, std::string& Error) {
  const std::string R = Real(Path);
  if (R.empty()) {
    Error = "bad path " + Path;
    return false;
  }
  const size_t Slash = R.rfind('/');
  if (Slash != std::string::npos && !MakeDirs(ParentPath(Path))) {
    Error = "cannot create the folder of " + Path;
    return false;
  }
  const std::string Tmp = R + ".tmp";
  FILE* F = std::fopen(Tmp.c_str(), "wb");
  if (!F) {
    Error = std::string("cannot create ") + Tmp + ": " + std::strerror(errno);
    return false;
  }
  const bool Wrote = Data.empty() || std::fwrite(Data.data(), 1, Data.size(), F) == Data.size();
  const bool Flushed = std::fflush(F) == 0;
  if (Wrote && Flushed)
    fsync(fileno(F));
  const bool Closed = std::fclose(F) == 0;
  if (!Wrote || !Flushed || !Closed) {
    Error = "write failed for " + Path;
    RemoveFile(Tmp);
    return false;
  }
  if (!Rename(Tmp, R)) {
    Error = "rename failed for " + Path;
    RemoveFile(Tmp);
    return false;
  }
  return true;
}

bool PosixStorage::IsDir(const std::string& Path) {
  const std::string R = Real(Path);
  struct stat St;
  return !R.empty() && stat(R.c_str(), &St) == 0 && S_ISDIR(St.st_mode);
}

bool PosixStorage::Exists(const std::string& Path) {
  const std::string R = Real(Path);
  struct stat St;
  return !R.empty() && stat(R.c_str(), &St) == 0;
}

namespace {
class PosixDirReader final : public DirReader {
public:
  PosixDirReader(DIR* D, std::string Base) : Dir(D), BasePath(std::move(Base)) {}
  ~PosixDirReader() override {
    if (Dir)
      closedir(Dir);
  }
  DirStep Next(DirEntry& Out) override {
    while (struct dirent* E = readdir(Dir)) {
      if (!std::strcmp(E->d_name, ".") || !std::strcmp(E->d_name, ".."))
        continue;
      Out = DirEntry {};
      Out.Name = E->d_name;
      struct stat St;
      const std::string Full = BasePath + "/" + Out.Name;
      if (stat(Full.c_str(), &St) == 0) {
        Out.IsDir = S_ISDIR(St.st_mode);
        Out.Size = Out.IsDir ? 0 : uint64_t(St.st_size);
        Out.Mtime = int64_t(St.st_mtime);
      }
      return DirStep::Entry;
    }
    return DirStep::End;
  }

private:
  DIR* Dir;
  std::string BasePath;
};
} // namespace

std::unique_ptr<DirReader> PosixStorage::OpenDir(const std::string& Path) {
  const std::string R = Real(Path);
  if (R.empty())
    return nullptr;
  DIR* D = opendir(R.c_str());
  if (!D)
    return nullptr;
  return std::make_unique<PosixDirReader>(D, R);
}

bool PosixStorage::FreeSpace(const std::string& Path, uint64_t& FreeBytes, uint64_t& TotalBytes) {
  const std::string R = Real(Path);
  struct statvfs V;
  if (R.empty() || statvfs(R.c_str(), &V) != 0)
    return false;
  FreeBytes = uint64_t(V.f_bavail) * uint64_t(V.f_frsize);
  TotalBytes = uint64_t(V.f_blocks) * uint64_t(V.f_frsize);
  return true;
}

} // namespace ui
