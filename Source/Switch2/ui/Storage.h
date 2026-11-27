// SPDX-License-Identifier: MIT
// File access the UI needs (settings, license files, the file picker). Paths use the two forms of the spec: `sdmc:/...` for the
// microSD card and `/...` for the inside of the RootFS. No libnx types here; the console backend maps both forms.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace ui {

struct DirEntry {
  std::string Name;
  bool IsDir = false;
  uint64_t Size = 0;
  int64_t Mtime = 0;  // unix seconds, 0 = unknown
};

enum class DirStep : uint8_t {
  Entry,    // Out is filled
  End,      // no more entries (also after a read error, see Failed())
  Pending,  // nothing available yet: ask again next frame (a backend that reads on another thread, or a slow card)
};

// A directory read in small steps so the main thread never blocks on a folder with thousands of entries.
class DirReader {
public:
  virtual ~DirReader() = default;
  virtual DirStep Next(DirEntry& Out) = 0;
  virtual bool Failed() const { return false; }
};

class Storage {
public:
  virtual ~Storage() = default;
  virtual bool ReadFile(const std::string& Path, std::string& Out) = 0;  // false: missing or unreadable
  // temp file + rename: a reader sees the old file or the complete new one, never a partial file. Creates missing folders.
  virtual bool WriteFileAtomic(const std::string& Path, const std::string& Data, std::string& Error) = 0;
  virtual bool MakeDirs(const std::string& Path) = 0;
  virtual bool IsDir(const std::string& Path) = 0;
  virtual bool Exists(const std::string& Path) = 0;
  virtual std::unique_ptr<DirReader> OpenDir(const std::string& Path) = 0;  // null: cannot be opened
  virtual bool FreeSpace(const std::string& Path, uint64_t& FreeBytes, uint64_t& TotalBytes) = 0;
};

// Plain POSIX files. `sdmc:/x` maps to SdRoot + "/x" and `/x` to RootfsRoot + "/x". On the console SdRoot is "sdmc:" (devoptab)
// and RootfsRoot the mounted RootFS folder; on the host tests use a temp folder for both.
class PosixStorage : public Storage {
public:
  PosixStorage(std::string SdRoot, std::string RootfsRoot) : Sd(std::move(SdRoot)), Rootfs(std::move(RootfsRoot)) {}
  bool ReadFile(const std::string& Path, std::string& Out) override;
  bool WriteFileAtomic(const std::string& Path, const std::string& Data, std::string& Error) override;
  bool MakeDirs(const std::string& Path) override;
  bool IsDir(const std::string& Path) override;
  bool Exists(const std::string& Path) override;
  std::unique_ptr<DirReader> OpenDir(const std::string& Path) override;
  bool FreeSpace(const std::string& Path, uint64_t& FreeBytes, uint64_t& TotalBytes) override;
  std::string Real(const std::string& Path) const;  // the host path of a UI path; empty when the form is not recognised

protected:
  virtual bool Rename(const std::string& From, const std::string& To);  // overridden by tests to fail the last step
  virtual bool RemoveFile(const std::string& Real);

private:
  std::string Sd, Rootfs;
};

} // namespace ui
