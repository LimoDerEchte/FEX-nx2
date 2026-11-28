// SPDX-License-Identifier: MIT
// The RootFS image installer: free-space check, resumable HTTP download, XXH3-64 verification, squashfs extraction into
// the guest root filesystem, the "unbreak" step of FEX's RootFS project, the everyday setup, the extra Ubuntu packages
// our acceptance tools need, and the marker file that makes the RootFS valid (written last). Portable and driven through
// InstallEnv, so the host tests run the same code with a host-directory writer and a fake HTTP server.
//
// Documented differences from FEX's unbreak_chroot.sh: docs/ui/rootfs-image-format.md.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include "xxhash.h"

#include "GuestRootfsSetup.h"
#include "OpenPgp.h"
#include "UbuntuArchiveKey.h"
#include "GuestSquashfs.h"
#include "HttpClient.h"

namespace Switch2Guest::RootfsInstall {

using RootfsSetup::RootfsWriter;

inline double MonotonicSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// SHA-256 (the Ubuntu archive's hashes) and XXH3-64 (the RootFS project's image hash).

class Sha256 {
public:
  Sha256() { Reset(); }
  void Reset() {
    static constexpr uint32_t Initial[8] {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(State, Initial, sizeof(State));
    Length = 0;
    Used = 0;
  }
  void Update(const uint8_t* Data, size_t Size) {
    Length += Size;
    while (Size) {
      const size_t Take = std::min<size_t>(64 - Used, Size);
      std::memcpy(Block + Used, Data, Take);
      Used += Take;
      Data += Take;
      Size -= Take;
      if (Used == 64) {
        Compress();
        Used = 0;
      }
    }
  }
  std::string Hex() {
    const uint64_t Bits = Length * 8;
    const uint8_t One = 0x80;
    Update(&One, 1);
    const uint8_t Zero = 0;
    while (Used != 56) Update(&Zero, 1);
    uint8_t Tail[8];
    for (int Index = 0; Index < 8; ++Index) Tail[Index] = static_cast<uint8_t>(Bits >> (56 - 8 * Index));
    Update(Tail, 8);
    std::string Text;
    char Byte[9];
    for (uint32_t Word : State) {
      std::snprintf(Byte, sizeof(Byte), "%08x", Word);
      Text += Byte;
    }
    return Text;
  }

private:
  static uint32_t Rotate(uint32_t Value, int Count) { return (Value >> Count) | (Value << (32 - Count)); }
  void Compress() {
    static constexpr uint32_t K[64] {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t W[64];
    for (int Index = 0; Index < 16; ++Index)
      W[Index] = uint32_t {Block[Index * 4]} << 24 | uint32_t {Block[Index * 4 + 1]} << 16 | uint32_t {Block[Index * 4 + 2]} << 8 | Block[Index * 4 + 3];
    for (int Index = 16; Index < 64; ++Index) {
      const uint32_t S0 = Rotate(W[Index - 15], 7) ^ Rotate(W[Index - 15], 18) ^ (W[Index - 15] >> 3);
      const uint32_t S1 = Rotate(W[Index - 2], 17) ^ Rotate(W[Index - 2], 19) ^ (W[Index - 2] >> 10);
      W[Index] = W[Index - 16] + S0 + W[Index - 7] + S1;
    }
    uint32_t A = State[0], B = State[1], C = State[2], D = State[3], E = State[4], F = State[5], G = State[6], H = State[7];
    for (int Index = 0; Index < 64; ++Index) {
      const uint32_t T1 = H + (Rotate(E, 6) ^ Rotate(E, 11) ^ Rotate(E, 25)) + ((E & F) ^ (~E & G)) + K[Index] + W[Index];
      const uint32_t T2 = (Rotate(A, 2) ^ Rotate(A, 13) ^ Rotate(A, 22)) + ((A & B) ^ (A & C) ^ (B & C));
      H = G;
      G = F;
      F = E;
      E = D + T1;
      D = C;
      C = B;
      B = A;
      A = T1 + T2;
    }
    State[0] += A;
    State[1] += B;
    State[2] += C;
    State[3] += D;
    State[4] += E;
    State[5] += F;
    State[6] += G;
    State[7] += H;
  }
  uint32_t State[8];
  uint8_t Block[64];
  uint64_t Length {};
  size_t Used {};
};

inline std::string Sha256Of(std::span<const uint8_t> Data) {
  Sha256 Hash;
  Hash.Update(Data.data(), Data.size());
  return Hash.Hex();
}

// Parses a hex hash of 1 to 16 digits (the RootFS index drops leading zeros).
inline std::optional<uint64_t> ParseHash64(std::string_view Text) {
  if (Text.empty() || Text.size() > 16) return std::nullopt;
  uint64_t Value = 0;
  for (char C : Text) {
    int Digit;
    if (C >= '0' && C <= '9') Digit = C - '0';
    else if (C >= 'a' && C <= 'f') Digit = C - 'a' + 10;
    else if (C >= 'A' && C <= 'F') Digit = C - 'A' + 10;
    else return std::nullopt;
    Value = Value << 4 | static_cast<uint64_t>(Digit);
  }
  return Value;
}

inline std::string Hex64(uint64_t Value) {
  char Text[17];
  std::snprintf(Text, sizeof(Text), "%016llx", static_cast<unsigned long long>(Value));
  return Text;
}

// ---------------------------------------------------------------------------
// The environment the installer runs in.

enum class LogKind { Info, Ok, Warn, Error };

struct ImageSpec {
  std::string Name;      // "Ubuntu 24.04"
  std::string FileName;  // "Ubuntu_24_04.sqsh"
  std::string Url;       // http://rootfs.fex-emu.gg/...
  std::string Hash;      // hex XXH3-64 of the whole file; empty = not verified
  uint64_t DownloadBytes {}, InstalledBytes {};
  std::string LocalPath; // set: install this image from the SD card instead of downloading
};

inline constexpr const char* DownloadFolderPath = "/fex/.download";
inline constexpr const char* MarkerPath = "/fex/config/rootfs.json";

class InstallEnv {
public:
  virtual ~InstallEnv() = default;
  virtual bool Cancelled() = 0;
  // Called often (per received chunk, per extracted entry); the UI draws a frame here.
  virtual void Pump() {}
  virtual void Log(LogKind Kind, const std::string& Line) = 0;
  virtual void Step(int Index, const std::string& Title) { (void)Index, (void)Title; }
  // Overall fraction 0..1 (weighted over the steps) and a short detail line.
  virtual void Progress(double Fraction, const std::string& Detail) = 0;

  virtual uint64_t FreeBytes() = 0;
  // Maps a logical SD path ("/fex/.download/x") to a path fopen can open.
  virtual std::string Native(const std::string& Logical) = 0;
  virtual std::unique_ptr<RootfsWriter> OpenWriter() = 0;
  // True when /fex/rootfs holds anything.
  virtual bool RootfsPopulated() = 0;
  // Deletes the RootFS tree and its inode metadata, leaving an empty rootfs folder. Tick gets (entries removed, total entries);
  // total is 0 while the entries are still being counted (then the first value is the count so far).
  using WipeTick = std::function<void(uint64_t Done, uint64_t Total)>;
  virtual bool WipeRootfs(const WipeTick& Tick, std::string& Error) = 0;
  // GET with an optional byte offset. Tests replace it.
  virtual const char* HttpGet(const std::string& Url, uint64_t Offset, const Http::BodyFn& Body, Http::ResponseInfo& Info,
                              const Http::HeadFn& OnHead = {}) {
    return Http::Get(Url, Offset, Body, Info, 30, OnHead);
  }
  virtual std::vector<uint32_t> DnsServers() { return {}; } // network order
  // RSA moduli (e = 65537) trusted to sign the archive's InRelease files. Tests replace it with their own key.
  virtual std::vector<std::span<const uint8_t>> ArchiveKeys() { return {std::span<const uint8_t>(UbuntuArchiveKey2018)}; }
  virtual void RandomBytes(uint8_t* Data, size_t Size) {
    uint64_t State = static_cast<uint64_t>(std::time(nullptr)) * 0x9e3779b97f4a7c15ull ^ reinterpret_cast<uintptr_t>(this);
    for (size_t Index = 0; Index < Size; ++Index) {
      State ^= State << 13;
      State ^= State >> 7;
      State ^= State << 17;
      Data[Index] = static_cast<uint8_t>(State >> 24);
    }
  }
  // Runs "dpkg --configure -a" in the guest for the unpacked extra packages. Returns false with Detail on failure.
  // The default does nothing (host tests).
  virtual bool ConfigureExtras(std::string& Detail) { Detail = "not run in this environment"; return false; }
  virtual int64_t Now() { return static_cast<int64_t>(std::time(nullptr)); }
};

struct InstallOptions {
  bool Replace {};        // wipe an existing RootFS first (reinstall)
  bool Extras {true};     // download and unpack the extra Ubuntu packages
  bool KeepDownload {};   // keep the verified image (tests)
};

struct InstallResult {
  bool Ok {};
  bool Cancelled {};
  std::string Error;
  std::string Warning;    // non-fatal problems (extras not configured, devices skipped)
};

// ---------------------------------------------------------------------------
// Files on the SD card (stdio).

inline bool FileSize(const std::string& Native, uint64_t& Size) {
  struct stat Info {};
  if (stat(Native.c_str(), &Info) || !S_ISREG(Info.st_mode)) return false;
  Size = static_cast<uint64_t>(Info.st_size);
  return true;
}

inline void MakeFolders(InstallEnv& Env, const std::string& Logical) {
  for (size_t Slash = 1; Slash != std::string::npos; Slash = Logical.find('/', Slash + 1)) {
    if (Slash > 1 || Logical.size() > 1) mkdir(Env.Native(Logical.substr(0, Slash)).c_str(), 0777);
  }
  mkdir(Env.Native(Logical).c_str(), 0777);
}

inline bool WriteTextFile(InstallEnv& Env, const std::string& Logical, const std::string& Text) {
  const auto Native = Env.Native(Logical);
  const auto Temporary = Native + ".tmp";
  auto* File = std::fopen(Temporary.c_str(), "wb");
  if (!File) return false;
  const bool Written = Text.empty() || std::fwrite(Text.data(), 1, Text.size(), File) == Text.size();
  if (std::fclose(File) || !Written) {
    std::remove(Temporary.c_str());
    return false;
  }
  std::remove(Native.c_str());
  return std::rename(Temporary.c_str(), Native.c_str()) == 0;
}

// Streams a file through Fn(chunk); false on a read error or when Fn returns false.
inline bool ReadChunks(const std::string& Native, const std::function<bool(const uint8_t*, size_t)>& Fn) {
  auto* File = std::fopen(Native.c_str(), "rb");
  if (!File) return false;
  std::vector<uint8_t> Buffer(256 * 1024);
  bool Good = true;
  for (;;) {
    const auto Count = std::fread(Buffer.data(), 1, Buffer.size(), File);
    if (Count && !Fn(Buffer.data(), Count)) {
      Good = false;
      break;
    }
    if (Count < Buffer.size()) {
      Good = !std::ferror(File);
      break;
    }
  }
  std::fclose(File);
  return Good;
}

// XXH3-64 of a whole file, in bounded chunks. Returns false on a read error or cancel.
inline bool HashFileXxh3(const std::string& Native, uint64_t& Hash, const std::function<bool(uint64_t)>& Progress) {
  auto* State = XXH3_createState();
  if (!State) return false;
  XXH3_64bits_reset(State);
  uint64_t Done = 0;
  const bool Good = ReadChunks(Native, [&](const uint8_t* Data, size_t Size) {
    XXH3_64bits_update(State, Data, Size);
    Done += Size;
    return Progress(Done);
  });
  Hash = XXH3_64bits_digest(State);
  XXH3_freeState(State);
  return Good;
}

// ---------------------------------------------------------------------------
// Squashfs -> guest filesystem.

struct ExtractStats {
  uint64_t Directories {}, Files {}, HardLinks {}, Symlinks {}, Fifos {}, Bytes {};
  uint64_t SkippedDevices {}, SkippedSockets {};
};

struct ImageSize {
  uint64_t Files {}, Bytes {}, Entries {};
};

// Walks the image's metadata once for entry and byte counts.
inline const char* MeasureImage(Squashfs::Reader& Reader, ImageSize& Size, const std::function<bool()>& Cancel) {
  Size = {};
  return Reader.Walk([&](const Squashfs::Entry& Entry) -> const char* {
    if (Cancel && Cancel()) return "Cancelled";
    ++Size.Entries;
    if (Entry.Kind == Squashfs::Type::File && Entry.HardLinkTo.empty()) {
      ++Size.Files;
      Size.Bytes += Entry.Size;
    }
    return nullptr;
  });
}

// What extracting costs, learned while it runs: seconds ~ A x bytes + B x entries (every entry has a fixed cost: it creates an
// inode, journals it and sets its owner and mode, so the many small files near the end of an image take far longer per byte than the
// big ones). A and B come from a least-squares fit over every sample since the start (the whole elapsed time, not a window); with too
// little data or a degenerate fit (A or B negative, bytes and entries growing in step) it is bytes only. Fraction() is the share of the
// predicted total time done so far, so the bar and the ETA the UI derives from it follow the real cost of the tail.
class ExtractCostModel {
public:
  void SetTotals(double Bytes, double Entries) {
    TotalBytes = Bytes;
    TotalEntries = Entries;
  }
  // Cumulative counts after `Seconds` of extraction.
  void Add(double Seconds, double Bytes, double Entries) {
    ++Samples;
    Sbb += Bytes * Bytes;
    See += Entries * Entries;
    Sbe += Bytes * Entries;
    Sbt += Bytes * Seconds;
    Set += Entries * Seconds;
    Fit();
  }
  double Fraction(double Bytes, double Entries) const {
    const double Total = A * TotalBytes + B * TotalEntries;
    const double Part = A * Bytes + B * Entries;
    if (Total <= 0 || Part < 0) return TotalBytes > 0 ? std::clamp(Bytes / TotalBytes, 0.0, 1.0) : 0.0;
    return std::clamp(Part / Total, 0.0, 1.0);
  }
  double SecondsPerByte() const { return A; }
  double SecondsPerEntry() const { return B; }

private:
  void Fit() {
    A = Sbb > 0 ? Sbt / Sbb : 0;
    B = 0;
    if (Samples < 4 || Sbb <= 0 || See <= 0) return;
    const double Det = Sbb * See - Sbe * Sbe;
    if (Det <= 1e-6 * Sbb * See) return;
    const double Pa = (Sbt * See - Set * Sbe) / Det, Pb = (Set * Sbb - Sbt * Sbe) / Det;
    if (Pa > 0 && Pb >= 0) {
      A = Pa;
      B = Pb;
    } else if (Pa <= 0 && See > 0 && Set > 0) {
      A = 0;
      B = Set / See;
    }
  }

  double TotalBytes {}, TotalEntries {}, Sbb {}, See {}, Sbe {}, Sbt {}, Set {};
  double A {}, B {};
  unsigned Samples {};
};

// The id that owns every entry of a FEX RootFS image: the user who built it (docs/ui/rootfs-image-format.md).
inline constexpr uint32_t ImageBuilderId = 1000;

struct ExtractHooks {
  // The FEX images are built without root, so the system tree is owned by ImageBuilderId. A Linux system tree is root's (sudo
  // refuses a /etc/sudoers.d it does not own), so the installer sets this: ImageBuilderId as uid or gid becomes 0.
  bool BuilderToRoot {};
  std::function<bool()> Cancelled;
  std::function<void(const ExtractStats&, const std::string&)> Entry; // after each entry
  std::function<void(LogKind, const std::string&)> Log;
};

// Extracts every entry through the writer. Owners (re-owned per Hooks.BuilderToRoot), modes, symlinks and hard links are applied as
// the image records them.
// Device nodes cannot be created by the runtime (it has no device nodes): they are counted and skipped with a warning.
inline const char* ExtractSquashfs(Squashfs::Reader& Reader, RootfsWriter& Writer, const ExtractHooks& Hooks, ExtractStats& Stats, std::string& Detail) {
  using Squashfs::Type;
  Stats = {};
  const auto Own = [&](uint32_t Id) { return Hooks.BuilderToRoot && Id == ImageBuilderId ? 0u : Id; };
  return Reader.Walk([&](const Squashfs::Entry& Entry) -> const char* {
    if (Hooks.Cancelled && Hooks.Cancelled()) return "Cancelled";
    int64_t Result = 0;
    const uint32_t Uid = Own(Entry.Uid), Gid = Own(Entry.Gid);
    switch (Entry.Kind) {
    case Type::Directory:
      if (Entry.Path != "/") {
        Result = Writer.MakeDirectory(Entry.Path, Entry.Mode, Uid, Gid);
      } else {
        // The root folder exists already: it takes the image root's owner and mode like any other folder (chown first: it
        // clears set-id bits). -38: a writer without these operations.
        Result = Writer.SetOwner("/", Uid, Gid);
        if (Result >= 0) Result = Writer.SetMode("/", Entry.Mode);
        if (Result == -38) Result = 0;
      }
      ++Stats.Directories;
      break;
    case Type::File:
      if (!Entry.HardLinkTo.empty()) {
        Result = Writer.HardLink(Entry.HardLinkTo, Entry.Path);
        ++Stats.HardLinks;
        break;
      }
      Result = Writer.BeginFile(Entry.Path, Entry.Mode, Uid, Gid, Entry.ModifyTime);
      if (Result >= 0) {
        const char* Failure = Reader.ReadFile(Entry, [&](const uint8_t* Data, size_t Size) -> const char* {
          if (Hooks.Cancelled && Hooks.Cancelled()) return "Cancelled";
          const auto Written = Writer.AppendFile({Data, Size});
          if (Written < 0) {
            Result = Written;
            return "Could not write a file";
          }
          Stats.Bytes += Size;
          if (Hooks.Entry) Hooks.Entry(Stats, Entry.Path);
          return nullptr;
        });
        const auto Ended = Writer.EndFile();
        if (Failure) {
          if (Result >= 0 && std::strcmp(Failure, "Cancelled")) Detail = Entry.Path;
          return Failure;
        }
        if (Ended < 0) Result = Ended;
      }
      ++Stats.Files;
      break;
    case Type::Symlink:
      Result = Writer.Symlink(Entry.LinkTarget, Entry.Path, Uid, Gid);
      ++Stats.Symlinks;
      break;
    case Type::Fifo:
      Result = Writer.MakeNode(Entry.Path, 0010000 | Entry.Mode, 0, Uid, Gid);
      ++Stats.Fifos;
      break;
    case Type::CharDevice:
    case Type::BlockDevice:
      // The runtime provides its own /dev; devices in an image cannot be created.
      ++Stats.SkippedDevices;
      if (Hooks.Log) Hooks.Log(LogKind::Warn, "Skipped device node " + Entry.Path);
      break;
    case Type::Socket:
      ++Stats.SkippedSockets;
      if (Hooks.Log) Hooks.Log(LogKind::Warn, "Skipped socket " + Entry.Path);
      break;
    }
    if (Result < 0) {
      Detail = Entry.Path + " (error " + std::to_string(-Result) + ")";
      return "Could not write to the RootFS";
    }
    if (Hooks.Entry) Hooks.Entry(Stats, Entry.Path);
    return nullptr;
  });
}

// ---------------------------------------------------------------------------
// The unbreak step (FEX RootFS Scripts/chroot.py "unbreak" and Scripts/Ubuntu/unbreak_chroot.sh).

struct UnbreakStats {
  uint64_t Moved {}, Merged {}, Kept {}, Removed {};
  std::vector<std::string> Missing; // expected /chroot entries the image did not have
};

inline std::string JoinPath(std::string_view Base, std::string_view Name) {
  std::string Path(Base);
  if (!Path.ends_with('/')) Path += '/';
  Path += Name;
  return Path;
}

inline bool IsDirectoryMode(uint32_t Mode) { return (Mode & 0170000) == 0040000; }

inline int64_t RemoveTree(RootfsWriter& Writer, const std::string& Path, uint64_t& Count) {
  RootfsWriter::TreeEntry Entry;
  if (const auto Error = Writer.TreeStat(Path, Entry)) return Error == -2 ? 0 : Error;
  if (IsDirectoryMode(Entry.Mode)) {
    std::vector<std::string> Names;
    if (const auto Error = Writer.TreeList(Path, Names)) return Error;
    for (const auto& Name : Names)
      if (const auto Error = RemoveTree(Writer, JoinPath(Path, Name), Count)) return Error;
  }
  if (const auto Error = Writer.TreeRemove(Path)) return Error;
  ++Count;
  return 0;
}

// Moves From to To like mv, but merges directories: a directory that exists on both sides is merged entry by entry. A file
// or symlink that exists on both sides keeps the destination and drops the source (the script's mv would overwrite).
inline int64_t MergeMove(RootfsWriter& Writer, const std::string& From, const std::string& To, UnbreakStats& Stats) {
  RootfsWriter::TreeEntry Source, Destination;
  if (const auto Error = Writer.TreeStat(From, Source)) return Error == -2 ? 0 : Error;
  const auto Present = Writer.TreeStat(To, Destination);
  if (Present && Present != -2) return Present;
  if (Present == -2) {
    if (const auto Error = Writer.TreeRename(From, To)) return Error;
    ++Stats.Moved;
    return 0;
  }
  if (IsDirectoryMode(Source.Mode) && IsDirectoryMode(Destination.Mode)) {
    std::vector<std::string> Names;
    if (const auto Error = Writer.TreeList(From, Names)) return Error;
    for (const auto& Name : Names)
      if (const auto Error = MergeMove(Writer, JoinPath(From, Name), JoinPath(To, Name), Stats)) return Error;
    if (const auto Error = Writer.TreeRemove(From)) return Error;
    ++Stats.Merged;
    return 0;
  }
  uint64_t Removed = 0;
  if (const auto Error = RemoveTree(Writer, From, Removed)) return Error;
  ++Stats.Kept;
  return 0;
}

// The paths FEX's break step moves into /chroot (chroot.py BackupPathsClass). They are reported when the image lacks them;
// the move itself sweeps every remaining /chroot entry back to its place.
inline constexpr const char* BrokenChrootPaths[] = {
    "boot", "home", "media", "mnt", "root", "srv", "tmp", "run", "var/tmp", "var/run", "var/lock", "etc/hosts", "etc/resolv.conf",
    "etc/timezone", "etc/localtime", "etc/passwd", "etc/passwd-", "etc/group", "etc/group-", "etc/shadow", "etc/shadow-", "etc/gshadow",
    "etc/gshadow-", "etc/fstab", "etc/hostname", "etc/mtab", "etc/subuid", "etc/subgid", "etc/machine-id", "var/lib/dbus/machine-id"};

// Moves every /chroot entry back to its place and removes /chroot. Returns an error text, or nullptr.
inline const char* Unbreak(RootfsWriter& Writer, UnbreakStats& Stats, std::string& Detail) {
  Stats = {};
  RootfsWriter::TreeEntry Entry;
  if (Writer.TreeStat("/chroot", Entry) != 0 || !IsDirectoryMode(Entry.Mode)) return nullptr; // not a broken chroot
  for (const char* Path : BrokenChrootPaths)
    if (Writer.TreeStat(std::string("/chroot/") + Path, Entry) != 0) Stats.Missing.emplace_back(Path);
  std::vector<std::string> Names;
  if (const auto Error = Writer.TreeList("/chroot", Names)) {
    Detail = "/chroot (error " + std::to_string(-Error) + ")";
    return "Could not read /chroot";
  }
  // Folders first, as chroot.py does, then etc: the order only matters for readable logs.
  std::sort(Names.begin(), Names.end());
  for (const auto& Name : Names) {
    if (const auto Error = MergeMove(Writer, "/chroot/" + Name, "/" + Name, Stats)) {
      Detail = "/chroot/" + Name + " (error " + std::to_string(-Error) + ")";
      return "Could not move a /chroot entry back";
    }
  }
  if (const auto Error = RemoveTree(Writer, "/chroot", Stats.Removed)) {
    Detail = "/chroot (error " + std::to_string(-Error) + ")";
    return "Could not remove /chroot";
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Everyday setup.

inline constexpr char ResolvMarker[] = "# Generated by FEX Switch 2 launcher\n";
inline constexpr char DefaultUser[] = "fex";

inline std::string DnsText(uint32_t Address) {
  const auto* Part = reinterpret_cast<const uint8_t*>(&Address);
  return std::to_string(Part[0]) + "." + std::to_string(Part[1]) + "." + std::to_string(Part[2]) + "." + std::to_string(Part[3]);
}

struct SetupStats {
  std::vector<std::string> Notes; // one line per change, for the install log
};

inline constexpr char AptSourcesPath[] = "/etc/apt/sources.list.d/ubuntu.sources";
inline constexpr char AptArchiveUri[] = "http://archive.ubuntu.com/ubuntu/";
inline constexpr char SudoersPath[] = "/etc/sudoers.d/fex";

// The deb822 sources as Ubuntu 24.04 ships them on a normal install: binary indexes only (the image's deb-src stanzas make
// apt fetch a Sources index per suite and component), and the main archive on its plain URI (an earlier setup switched it to
// apt's mirror method; the single archive URI needs one request per index). Suites, components, comments and other
// stanzas stay as they are.
inline std::string NormalizeAptSources(std::string_view Text) {
  constexpr std::string_view MirrorList = "mirror+http://mirrors.ubuntu.com/mirrors.txt";
  std::string Out;
  for (size_t Start = 0; Start < Text.size();) {
    const auto End = std::min(Text.find('\n', Start), Text.size());
    std::string Line(Text.substr(Start, End - Start));
    if (Line.starts_with("Types:")) {
      Line = "Types: deb";
    } else if (Line.starts_with("URIs:")) {
      if (const auto At = Line.find(MirrorList); At != Line.npos) Line.replace(At, MirrorList.size(), AptArchiveUri);
      else if (const auto Bare = Line.find("http://archive.ubuntu.com/ubuntu"); Bare != Line.npos && !Line.ends_with("/"))
        Line += "/";
    }
    Out += Line;
    if (End < Text.size()) Out += '\n';
    Start = End + 1;
  }
  return Out;
}

inline bool ContainsLine(std::string_view Text, std::string_view Prefix) {
  for (size_t Start = 0; Start < Text.size();) {
    const auto End = std::min(Text.find('\n', Start), Text.size());
    if (Text.substr(Start, End - Start).starts_with(Prefix)) return true;
    Start = End + 1;
  }
  return false;
}

// The host name the runtime reports (uname, runtime/GuestRuntime.cpp).
inline constexpr char GuestHostName[] = "fex-switch2";

// Whether a hosts file maps Name: a line (comments dropped) whose names after the address include it.
inline bool HostsNames(std::string_view Text, std::string_view Name) {
  for (size_t Start = 0; Start < Text.size();) {
    const auto End = std::min(Text.find('\n', Start), Text.size());
    auto Line = Text.substr(Start, End - Start);
    Line = Line.substr(0, std::min(Line.find('#'), Line.size()));
    Start = End + 1;
    bool First = true;
    for (size_t At = 0; At < Line.size();) {
      const auto From = Line.find_first_not_of(" \t", At);
      if (From == Line.npos) break;
      const auto To = std::min(Line.find_first_of(" \t", From), Line.size());
      if (!First && Line.substr(From, To - From) == Name) return true;
      First = false;
      At = To;
    }
  }
  return false;
}

// Mount points, /tmp mode, the account database, and the network files. Returns an error text, or nullptr.
inline const char* EverydaySetup(RootfsWriter& Writer, const std::vector<uint32_t>& Dns, InstallEnv& Env, SetupStats& Stats, std::string& Detail) {
  const auto Fail = [&](const std::string& What, int64_t Error) {
    Detail = What + " (error " + std::to_string(-Error) + ")";
    return "Could not set up the RootFS";
  };
  const auto Time = Env.Now();
  for (const char* Path : {"/proc", "/sys", "/dev", "/dev/pts"})
    if (const auto Error = Writer.MakeDirectory(Path, 0755, 0, 0); Error < 0) { Fail(Path, Error); return "Could not create a mount point"; }
  Stats.Notes.push_back("mount points /proc /sys /dev /dev/pts");
  if (const auto Error = Writer.SetMode("/tmp", 01777)) {
    if (Error == -2) {
      if (const auto Made = Writer.MakeDirectory("/tmp", 01777, 0, 0); Made < 0) { Fail("/tmp", Made); return "Could not create /tmp"; }
      Writer.SetMode("/tmp", 01777);
    } else {
      Fail("/tmp", Error);
      return "Could not set the mode of /tmp";
    }
  }
  Stats.Notes.push_back("/tmp mode 1777");
  for (const char* Path : {"/opt", "/home", "/root"}) {
    RootfsWriter::TreeEntry Entry;
    if (Writer.TreeStat(Path, Entry) == -2) Writer.MakeDirectory(Path, std::string_view(Path) == "/root" ? 0700 : 0755, 0, 0);
  }

  // Account database. Files that the image carries are kept; the default user is added when uid 1000 has no entry.
  const auto ReadText = [&](const char* Path, std::string& Text, bool& Exists) {
    std::vector<uint8_t> Data;
    const auto Error = Writer.ReadWholeFile(Path, Data);
    Exists = Error == 0;
    Text.assign(Data.begin(), Data.end());
    return Error == 0 || Error == -2;
  };
  const auto Account = [&](const char* Path, uint32_t Mode, uint32_t Group, const std::string& Initial, const std::string& Line,
                           std::string_view IdMarker) -> const char* {
    std::string Text;
    bool Exists = false;
    if (!ReadText(Path, Text, Exists)) return "Could not read an account file";
    if (!Exists) Text = Initial;
    else if (ContainsLine(Text, IdMarker)) return nullptr;
    else {
      if (!Text.empty() && !Text.ends_with('\n')) Text += '\n';
      Text += Line;
    }
    if (Writer.WriteFile(Path, RootfsSetup::Bytes(Text), Mode, 0, Group, Time) < 0) return "Could not write an account file";
    Stats.Notes.push_back(std::string(Exists ? "added " : "created ") + Path);
    return nullptr;
  };
  const std::string User = DefaultUser;
  // passwd has no easy "uid 1000" prefix test: look for ":1000:" in field 3.
  {
    std::string Text;
    bool Exists = false;
    if (!ReadText("/etc/passwd", Text, Exists)) return "Could not read /etc/passwd";
    bool Has1000 = false;
    for (size_t Start = 0; Start < Text.size();) {
      const auto End = std::min(Text.find('\n', Start), Text.size());
      const auto Line = std::string_view(Text).substr(Start, End - Start);
      size_t Colons[3];
      size_t Found = 0;
      for (size_t Index = 0; Index < Line.size() && Found < 3; ++Index)
        if (Line[Index] == ':') Colons[Found++] = Index;
      if (Found == 3 && Line.substr(Colons[1] + 1, Colons[2] - Colons[1] - 1) == "1000") Has1000 = true;
      Start = End + 1;
    }
    const std::string Root = "root:x:0:0:root:/root:/bin/bash\n";
    const std::string Line = User + ":x:1000:1000:" + User + ":/home/" + User + ":/bin/bash\n";
    if (!Exists || !Has1000) {
      std::string Result = Exists ? Text : Root;
      if (!Result.empty() && !Result.ends_with('\n')) Result += '\n';
      Result += Line;
      if (Writer.WriteFile("/etc/passwd", RootfsSetup::Bytes(Result), 0644, 0, 0, Time) < 0) return "Could not write /etc/passwd";
      Stats.Notes.push_back(Exists ? "added user " + User + " (1000) to /etc/passwd" : "created /etc/passwd");
    }
  }
  if (const auto* Error = Account("/etc/group", 0644, 0, "root:x:0:\n" + User + ":x:1000:\n", User + ":x:1000:\n", User + ":")) return Error;
  {
    // group needs the 1000 group even when the file exists but has no gid 1000.
    std::string Text;
    bool Exists = false;
    ReadText("/etc/group", Text, Exists);
    if (Exists && Text.find(":1000:") == std::string::npos) {
      Text += User + ":x:1000:\n";
      if (Writer.WriteFile("/etc/group", RootfsSetup::Bytes(Text), 0644, 0, 0, Time) < 0) return "Could not write /etc/group";
      Stats.Notes.push_back("added group " + User + " (1000)");
    }
  }
  if (const auto Gid = RootfsSetup::EnsureInputGroup(Writer, Time); Gid < 0)
    return Fail("/etc/group (input)", Gid);
  if (const auto* Error = Account("/etc/shadow", 0640, 0, "root:*:19000:0:99999:7:::\n" + User + ":*:19000:0:99999:7:::\n",
                                  User + ":*:19000:0:99999:7:::\n", User + ":"))
    return Error;
  if (const auto* Error = Account("/etc/gshadow", 0640, 0, "root:*::\n" + User + ":!::\n", User + ":!::\n", User + ":")) return Error;
  {
    RootfsWriter::TreeEntry Entry;
    if (Writer.TreeStat("/home/" + User, Entry) == -2) {
      if (const auto Error = Writer.MakeDirectory("/home/" + User, 0755, 1000, 1000); Error < 0) { Fail("/home/" + User, Error); return "Could not create the home folder"; }
      Stats.Notes.push_back("home folder /home/" + User);
    }
  }

  // Network files: the resolver is always ours (the image's is a link into a systemd runtime folder that does not exist).
  std::string Resolv = ResolvMarker;
  size_t Servers = 0;
  for (const auto Address : Dns) {
    if (!Address) continue;
    Resolv += "nameserver " + DnsText(Address) + "\n";
    ++Servers;
  }
  if (!Servers) Resolv += "nameserver 1.1.1.1\nnameserver 8.8.8.8\n";
  Resolv += "options timeout:2 attempts:2\n";
  if (Writer.WriteFile("/etc/resolv.conf", RootfsSetup::Bytes(Resolv), 0644, 0, 0, Time) < 0) return "Could not write /etc/resolv.conf";
  Stats.Notes.push_back(Servers ? "/etc/resolv.conf from the console's DNS" : "/etc/resolv.conf with public resolvers (the console reported no DNS)");
  {
    std::string Text;
    bool Exists = false;
    ReadText("/etc/hosts", Text, Exists);
    bool Changed = false;
    if (!Exists || !ContainsLine(Text, "127.0.0.1")) {
      Text = "127.0.0.1\tlocalhost\n::1\t\tlocalhost ip6-localhost ip6-loopback\n" + (Exists ? Text : std::string());
      Stats.Notes.push_back("/etc/hosts localhost entries");
      Changed = true;
    }
    // The runtime's host name (uname) must resolve: sudo looks it up. Debian's convention is 127.0.1.1.
    if (!HostsNames(Text, GuestHostName)) {
      if (!Text.empty() && !Text.ends_with('\n')) Text += '\n';
      Text += std::string("127.0.1.1\t") + GuestHostName + "\n";
      Stats.Notes.push_back(std::string("/etc/hosts ") + GuestHostName);
      Changed = true;
    }
    if (Changed && Writer.WriteFile("/etc/hosts", RootfsSetup::Bytes(Text), 0644, 0, 0, Time) < 0) return "Could not write /etc/hosts";
  }
  {
    std::string Text;
    bool Exists = false;
    ReadText("/etc/hostname", Text, Exists);
    if (!Exists || Text.empty()) {
      if (Writer.WriteFile("/etc/hostname", RootfsSetup::Bytes(std::string(GuestHostName) + "\n"), 0644, 0, 0, Time) < 0) return "Could not write /etc/hostname";
      Stats.Notes.push_back(std::string("/etc/hostname ") + GuestHostName);
    }
  }
  {
    std::string Text;
    bool Exists = false;
    ReadText("/etc/machine-id", Text, Exists);
    if (!Exists || Text.size() < 32) {
      uint8_t Random[16];
      Env.RandomBytes(Random, sizeof(Random));
      std::string Id;
      char Byte[3];
      for (auto Value : Random) {
        std::snprintf(Byte, sizeof(Byte), "%02x", Value);
        Id += Byte;
      }
      Id += '\n';
      if (Writer.WriteFile("/etc/machine-id", RootfsSetup::Bytes(Id), 0444, 0, 0, Time) < 0) return "Could not write /etc/machine-id";
      Stats.Notes.push_back("/etc/machine-id generated");
    }
  }
  // apt sources as a normal Ubuntu 24.04 install has them: deb only, plain archive and security URIs (NormalizeAptSources).
  {
    std::string Text;
    bool Exists = false;
    ReadText(AptSourcesPath, Text, Exists);
    if (Exists) {
      const std::string Updated = NormalizeAptSources(Text);
      if (Updated != Text) {
        if (Writer.WriteFile(AptSourcesPath, RootfsSetup::Bytes(Updated), 0644, 0, 0, Time) < 0) return "Could not write the apt sources";
        Stats.Notes.push_back(std::string(AptSourcesPath) + ": deb only, plain archive URIs");
      }
    }
  }
  // sudo for the default user: it has no password (shadow "*"), so the rule needs no password.
  {
    RootfsWriter::TreeEntry Entry;
    if (Writer.TreeStat("/etc/sudoers.d", Entry) == -2) Writer.MakeDirectory("/etc/sudoers.d", 0750, 0, 0);
    const std::string Rule = std::string(ResolvMarker) + DefaultUser + " ALL=(ALL:ALL) NOPASSWD: ALL\n";
    if (Writer.WriteFile(SudoersPath, RootfsSetup::Bytes(Rule), 0440, 0, 0, Time) < 0) return "Could not write the sudoers rule";
    Stats.Notes.push_back(std::string(SudoersPath) + ": " + DefaultUser + " may use sudo without a password");
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Extra Ubuntu packages (what our tests need plus everyday tools), from archive.ubuntu.com over HTTP.

struct ExtraPackage {
  const char* Name;
  const char* Reason;
  // A file the package must provide. The FEX image's dpkg database lists util-linux and mount as installed, but the image
  // lacks their files (su, mount, umount), so a package whose File is missing is unpacked even when dpkg says installed.
  const char* File {nullptr};
};

// One table: the package the acceptance programs and tool scripts need but the FEX image lacks (checked against the
// image's dpkg status on 2026-11-25), and why. Dependencies the image lacks are resolved from the index.
inline constexpr ExtraPackage ExtraPackages[] = {
    {"bubblewrap", "tests/guest/ContainerTools.sh: bwrap (user, mount, PID, UTS, IPC namespaces, seccomp fd)"},
    {"netcat-openbsd", "tests/guest/CommTools.sh: nc over Unix, UDP and TCP sockets (nc -N, -U, -u)"},
    {"inotify-tools", "tests/guest/ProcessTools.sh: inotifywait (create, delete, timeout, monitor)"},
    // Everyday tools the image lacks (git is in the image; sudo is only left as config files).
    {"sudo", "administration from the default user's shell (/etc/sudoers.d/fex)"},
    {"nano", "a text editor"},
    {"wget", "downloads from the shell"},
    {"whiptail", "debconf's Dialog frontend (without it debconf falls back to Readline)"},
    // The image's dpkg database lists these as installed (bsdutils is not listed at all) but the image lacks their commands.
    {"bsdutils", "script, scriptreplay, scriptlive, logger, renice, wall", "/usr/bin/script"},
    {"util-linux", "su and runuser (sudo su; /etc/pam.d/su is already in the image)", "/usr/bin/su"},
    {"mount", "mount, umount, losetup, swapon", "/usr/bin/mount"},
};

inline constexpr const char* ArchiveBase = "http://archive.ubuntu.com/ubuntu";
inline constexpr const char* ArchiveSuites[] = {"noble", "noble-updates", "noble-security"};
inline constexpr const char* ArchiveComponents[] = {"main", "universe"};

struct PackageRecord {
  std::string Name, Version, Filename, Sha256, Depends, Provides;
  uint64_t Size {};
};

// "a (>= 1), b | c:any" -> groups of alternatives with versions and architecture qualifiers removed.
inline std::vector<std::vector<std::string>> ParseDepends(std::string_view Text) {
  std::vector<std::vector<std::string>> Groups;
  std::vector<std::string> Current;
  std::string Name;
  bool InVersion = false, InQualifier = false;
  const auto EndName = [&]() {
    if (!Name.empty()) Current.push_back(Name);
    Name.clear();
    InQualifier = false;
  };
  for (char C : Text) {
    if (InVersion) {
      if (C == ')') InVersion = false;
      continue;
    }
    if (C == '(') InVersion = true;
    else if (C == ',') {
      EndName();
      if (!Current.empty()) Groups.push_back(std::move(Current));
      Current.clear();
    } else if (C == '|') EndName();
    else if (C == ':') InQualifier = true;
    else if (C == ' ' || C == '\n' || C == '\t') continue;
    else if (!InQualifier) Name += C;
  }
  EndName();
  if (!Current.empty()) Groups.push_back(std::move(Current));
  return Groups;
}

// A line-wise stanza splitter for Packages files; calls Visit with each paragraph.
class StanzaScanner {
public:
  explicit StanzaScanner(std::function<void(const std::string&)> Visit) : Visit {std::move(Visit)} {}
  void Feed(const uint8_t* Data, size_t Size) {
    for (size_t Index = 0; Index < Size; ++Index) {
      const char C = static_cast<char>(Data[Index]);
      Line.push_back(C);
      if (C != '\n') continue;
      if (Line == "\n") Flush();
      else Paragraph += Line;
      Line.clear();
    }
  }
  void Finish() {
    if (!Line.empty()) Paragraph += Line;
    Flush();
  }

private:
  void Flush() {
    if (!Paragraph.empty()) Visit(Paragraph);
    Paragraph.clear();
  }
  std::function<void(const std::string&)> Visit;
  std::string Line, Paragraph;
};

// Decodes an .xz file in chunks and feeds the decoded bytes to Sink. Returns false on corrupt data or a read error.
inline bool XzStreamFile(const std::string& Native, const std::function<void(const uint8_t*, size_t)>& Sink, const std::function<bool()>& Cancel) {
  xz_crc32_init();
#ifdef XZ_USE_CRC64
  xz_crc64_init();
#endif
  auto* Decoder = xz_dec_init(XZ_DYNALLOC, 1u << 27);
  if (!Decoder) return false;
  auto* File = std::fopen(Native.c_str(), "rb");
  if (!File) {
    xz_dec_end(Decoder);
    return false;
  }
  std::vector<uint8_t> In(64 * 1024), Out(256 * 1024);
  xz_buf Buffer {In.data(), 0, 0, Out.data(), 0, Out.size()};
  xz_ret Result = XZ_OK;
  bool EndOfFile = false;
  while (Result == XZ_OK) {
    if (Cancel && Cancel()) {
      Result = XZ_DATA_ERROR;
      break;
    }
    if (Buffer.in_pos == Buffer.in_size && !EndOfFile) {
      Buffer.in_size = std::fread(In.data(), 1, In.size(), File);
      Buffer.in_pos = 0;
      if (Buffer.in_size < In.size()) EndOfFile = true;
    }
    Result = xz_dec_run(Decoder, &Buffer);
    const bool Produced = Buffer.out_pos != 0;
    if (Produced) {
      Sink(Out.data(), Buffer.out_pos);
      Buffer.out_pos = 0;
    }
    if (Result == XZ_OK && EndOfFile && Buffer.in_pos == Buffer.in_size && !Produced) {
      // No more input and no progress: truncated.
      Result = XZ_DATA_ERROR;
    }
  }
  std::fclose(File);
  xz_dec_end(Decoder);
  return Result == XZ_STREAM_END;
}

// From an InRelease or Release text: the SHA256 entries "path -> (hash, size)".
inline std::map<std::string, std::pair<std::string, uint64_t>> ParseReleaseHashes(std::string_view Text) {
  std::map<std::string, std::pair<std::string, uint64_t>> Result;
  bool InSection = false;
  for (size_t Start = 0; Start < Text.size();) {
    const auto End = std::min(Text.find('\n', Start), Text.size());
    auto Line = Text.substr(Start, End - Start);
    Start = End + 1;
    if (Line.ends_with('\r')) Line.remove_suffix(1);
    if (Line == "SHA256:") {
      InSection = true;
      continue;
    }
    if (!Line.starts_with(' ')) {
      InSection = false;
      continue;
    }
    if (!InSection) continue;
    const auto Fields = RootfsSetup::SplitFields(Line);
    if (Fields.size() != 3) continue;
    const auto Size = Http::ParseNumber(Fields[1]);
    if (!Size || Fields[0].size() != 64) continue;
    Result[std::string(Fields[2])] = {std::string(Fields[0]), *Size};
  }
  return Result;
}

inline std::string StatusField(std::string_view Paragraph, std::string_view Field) { return RootfsSetup::ControlField(Paragraph, Field); }

// Names the image's dpkg status marks installed, plus what they provide.
inline std::set<std::string> InstalledNames(std::string_view Status) {
  std::set<std::string> Names;
  size_t Start = 0;
  while (Start < Status.size()) {
    auto End = Status.find("\n\n", Start);
    if (End == Status.npos) End = Status.size();
    const auto Paragraph = Status.substr(Start, End - Start);
    Start = End + 2;
    if (StatusField(Paragraph, "Status") != "install ok installed") continue;
    Names.insert(StatusField(Paragraph, "Package"));
    for (const auto& Group : ParseDepends(StatusField(Paragraph, "Provides")))
      for (const auto& Name : Group) Names.insert(Name);
  }
  return Names;
}

// The status text without the entries a newly unpacked package replaces. dpkg keeps one instance of a package that
// is not "Multi-Arch: same" (of any architecture), and one per architecture of one that is; a second instance makes
// it refuse the whole database ("mixed non-coinstallable and coinstallable package instances present"). The replaced
// entries are ones the image remembers without the package installed, e.g. "deinstall ok config-files".
inline std::string WithoutReplacedEntries(std::string_view Status, const std::vector<RootfsSetup::InstalledPackage>& Packages) {
  std::string Out;
  size_t Start = 0;
  while (Start < Status.size()) {
    auto End = Status.find("\n\n", Start);
    if (End == Status.npos) End = Status.size();
    const auto Paragraph = Status.substr(Start, End - Start);
    Start = End + 2;
    if (Paragraph.find_first_not_of('\n') == Paragraph.npos) continue;
    const auto Name = StatusField(Paragraph, "Package");
    const bool Replaced = std::any_of(Packages.begin(), Packages.end(), [&](const RootfsSetup::InstalledPackage& Package) {
      if (StatusField(Package.Control, "Package") != Name) return false;
      const bool Same = StatusField(Package.Control, "Multi-Arch") == "same" && StatusField(Paragraph, "Multi-Arch") == "same";
      return !Same || StatusField(Paragraph, "Architecture") == StatusField(Package.Control, "Architecture");
    });
    if (Replaced) continue;
    Out.append(Paragraph).append("\n\n");
  }
  return Out;
}

// Hex of a regular file's SHA-256, streamed.
inline bool Sha256File(const std::string& Native, std::string& Hex, const std::function<bool(uint64_t)>& Progress) {
  Sha256 Hash;
  uint64_t Done = 0;
  const bool Good = ReadChunks(Native, [&](const uint8_t* Data, size_t Size) {
    Hash.Update(Data, Size);
    Done += Size;
    return !Progress || Progress(Done);
  });
  Hex = Hash.Hex();
  return Good;
}

// ---------------------------------------------------------------------------
// The pipeline.

struct StepWeights {
  static constexpr double Space = 1, Download = 30, Verify = 6, Extract = 40, Setup = 2, Extras = 20, Finish = 1;
  static constexpr double Total = Space + Download + Verify + Extract + Setup + Extras + Finish;
};

class Installer {
public:
  Installer(InstallEnv& Env, ImageSpec Spec, InstallOptions Options) : Env {Env}, Spec {std::move(Spec)}, Options {Options} {}

  InstallResult Run() {
    InstallResult Result;
    Created = false;
    Verified = false;
    const char* Failure = RunSteps(Result);
    if (!Failure) {
      Result.Ok = true;
      return Result;
    }
    Result.Cancelled = std::strcmp(Failure, "Cancelled") == 0 || Env.Cancelled();
    Result.Error = Result.Cancelled ? "Cancelled" : Failure;
    if (!Detail.empty() && !Result.Cancelled) Result.Error += ": " + Detail;
    Cleanup(Result.Cancelled);
    return Result;
  }

  ExtractStats Extracted;
  UnbreakStats Unbroken;
  SetupStats Setup;
  std::vector<std::string> ExtrasInstalled;
  bool ExtrasConfigured {};

private:
  void Say(LogKind Kind, const std::string& Line) { Env.Log(Kind, Line); }

  void Report(double Base, double Weight, double Fraction, const std::string& Text) {
    Env.Progress((Base + Weight * std::clamp(Fraction, 0.0, 1.0)) / StepWeights::Total, Text);
  }

  std::string DownloadPath() const { return std::string(DownloadFolderPath) + "/" + Spec.FileName; }

  const char* RunSteps(InstallResult& Result) {
    double Base = 0;
    // 1. Space
    Env.Step(0, "Check free space");
    const bool Local = !Spec.LocalPath.empty();
    if (!Local && !Spec.DownloadBytes) {
      // A live index entry has no size: ask the server (the response head carries it), then stop the transfer.
      Http::ResponseInfo Probe;
      uint64_t Length = 0;
      const char* Error = Env.HttpGet(
          Spec.Url, 0, [](const uint8_t*, size_t) { return false; }, Probe,
          [&](const Http::ResponseInfo& Head) {
            if (Head.ContentLength) Length = *Head.ContentLength;
            return false;
          });
      if (!Length) {
        Detail = Error ? Error : "the server sent no size";
        return "Could not learn the size of the image";
      }
      Spec.DownloadBytes = Length;
      Say(LogKind::Info, "The image is " + Size(Length));
    }
    // Index numbers are untrusted: a huge size would wrap the space math below.
    constexpr uint64_t MaxImageBytes = 1ull << 40;
    if (Spec.DownloadBytes > MaxImageBytes || Spec.InstalledBytes > MaxImageBytes) return "The image size in the index is not valid";
    if (!Spec.InstalledBytes) Spec.InstalledBytes = Spec.DownloadBytes * 4;
    if (Env.RootfsPopulated() && !Options.Replace) return "The RootFS folder is not empty";
    // A reinstall keeps the old RootFS until the new image is downloaded, verified and opens, so a failed download,
    // a bad hash or a broken image leaves the user with the RootFS they had. The old RootFS is removed first only when
    // the card cannot hold the download next to it.
    const uint64_t NeedRootfs = Spec.InstalledBytes + Spec.InstalledBytes / 20 + (Options.Extras ? (64ull << 20) : 0);
    const uint64_t NeedDownload = Local ? 0 : Spec.DownloadBytes;
    bool Old = Env.RootfsPopulated();
    uint64_t Free = Env.FreeBytes();
    Say(LogKind::Info, "Free space " + Size(Free) + ", needed about " + Size(NeedRootfs + NeedDownload));
    if (Old && !Local && Free < NeedDownload + (64ull << 20)) {
      Say(LogKind::Warn, "Not enough free space to keep the old RootFS during the download: removing it first");
      if (const auto* Error = RemoveOld(Base, StepWeights::Space)) return Error;
      Old = false;
      Free = Env.FreeBytes();
    }
    if (!Old && Free < NeedRootfs + NeedDownload) {
      Detail = "needs " + Size(NeedRootfs + NeedDownload) + ", " + Size(Free) + " free";
      return "Not enough free space on the SD card";
    }
    Report(Base, StepWeights::Space, 1, "Space ok");
    Base += StepWeights::Space;
    if (Env.Cancelled()) return "Cancelled";

    // 2. Download
    std::string ImagePath;
    if (Local) {
      ImagePath = Env.Native(Spec.LocalPath);
      Say(LogKind::Info, "Using the image " + Spec.LocalPath);
    } else {
      Env.Step(1, "Download " + Spec.FileName);
      if (const auto* Error = Download(Base)) return Error;
      ImagePath = Env.Native(DownloadPath());
    }
    if (!Local) Base += StepWeights::Download;

    // 3. Verify
    Env.Step(2, "Verify");
    if (const auto* Error = Verify(ImagePath, Base)) return Error;
    Verified = true;
    {
      Squashfs::StdioImageSource Source(ImagePath);
      if (!Source.Opened()) return "Could not open the image file";
      Squashfs::Reader Reader(Source);
      if (const auto* Error = Reader.Open()) {
        Detail = Reader.Detail;
        return Error;
      }
    }
    Base += StepWeights::Verify;
    if (Env.Cancelled()) return "Cancelled";

    // The new image is good: now the old RootFS goes.
    Env.Step(3, "Extract the RootFS");
    if (Old) {
      if (const auto* Error = RemoveOld(Base, 0)) return Error;
      const uint64_t FreeNow = Env.FreeBytes();
      if (FreeNow < NeedRootfs) {
        Detail = "needs " + Size(NeedRootfs) + ", " + Size(FreeNow) + " free";
        return "Not enough free space on the SD card";
      }
    }
    std::remove(Env.Native(MarkerPath).c_str());
    Created = true;
    if (Env.Cancelled()) return "Cancelled";

    // 4. Extract
    {
      auto Writer = Env.OpenWriter();
      if (!Writer) return "Could not reserve memory for the guest filesystem";
      if (const auto* Error = Extract(ImagePath, *Writer, Base)) return Error;
      Base += StepWeights::Extract;

      // 5. Setup
      Env.Step(4, "Set up the RootFS");
      Say(LogKind::Info, "Moving the /chroot folders back");
      if (const auto* Error = Unbreak(*Writer, Unbroken, Detail)) return Error;
      Say(LogKind::Ok, "Unbreak: " + std::to_string(Unbroken.Moved) + " moved, " + std::to_string(Unbroken.Merged) + " merged, " +
                           std::to_string(Unbroken.Kept) + " kept, " + std::to_string(Unbroken.Removed) + " removed");
      for (const auto& Path : Unbroken.Missing) Say(LogKind::Info, "The image has no /chroot/" + Path);
      if (Env.Cancelled()) return "Cancelled";
      if (const auto* Error = EverydaySetup(*Writer, Env.DnsServers(), Env, Setup, Detail)) return Error;
      for (const auto& Note : Setup.Notes) Say(LogKind::Info, "Setup: " + Note);
      Report(Base, StepWeights::Setup, 1, "Set up");
      Base += StepWeights::Setup;
      if (Env.Cancelled()) return "Cancelled";

      // 6. Extras
      Env.Step(5, "Extra packages");
      if (Options.Extras) {
        if (const auto* Error = InstallExtras(*Writer, Base, Result)) return Error;
      }
      Base += StepWeights::Extras;
    }
    if (Options.Extras && !ExtrasInstalled.empty()) {
      std::string ConfigureDetail;
      Say(LogKind::Info, "Configuring the extra packages in the guest (dpkg --configure -a)");
      ExtrasConfigured = Env.ConfigureExtras(ConfigureDetail);
      if (!ExtrasConfigured) {
        Say(LogKind::Warn, "The extra packages are unpacked but not configured (" + ConfigureDetail + "). Run 'dpkg --configure -a' as root.");
        Result.Warning += "Extra packages not configured. ";
      }
    }

    // 7. Finish: marker last.
    Env.Step(6, "Finish");
    if (!Local && !Options.KeepDownload) std::remove(Env.Native(DownloadPath()).c_str());
    if (!WriteMarker()) return "Could not write the RootFS marker file";
    Report(Base, StepWeights::Finish, 1, "Done");
    Say(LogKind::Ok, "RootFS installed");
    return nullptr;
  }

  static std::string Size(uint64_t Bytes) {
    char Text[40];
    if (Bytes >= (1ull << 30)) std::snprintf(Text, sizeof(Text), "%.2f GiB", Bytes / double(1ull << 30));
    else std::snprintf(Text, sizeof(Text), "%.1f MiB", Bytes / 1048576.0);
    return Text;
  }

  // Resumable download into /fex/.download/<name>.part, renamed when complete.
  // Removes the installed RootFS (marker first, so a half-removed RootFS is never valid). Weight 0 keeps the bar where it is.
  const char* RemoveOld(double Base, double Weight) {
    std::remove(Env.Native(MarkerPath).c_str());
    Created = true;
    Say(LogKind::Info, "Removing the existing RootFS");
    Report(Base, Weight, 0.5, "Removing the existing RootFS");
    std::string Error;
    const auto Tick = [&](uint64_t Done, uint64_t Total) {
      if (!Total)
        Report(Base, Weight, 0.05, "Counting files: " + std::to_string(Done));
      else
        Report(Base, Weight, 0.05 + 0.9 * double(Done) / double(Total), "Removed " + std::to_string(Done) + " of " + std::to_string(Total));
    };
    if (!Env.WipeRootfs(Tick, Error)) {
      Detail = Error;
      return "Could not remove the existing RootFS";
    }
    return nullptr;
  }

  const char* Download(double Base) {
    MakeFolders(Env, DownloadFolderPath);
    const auto Final = Env.Native(DownloadPath());
    const auto Part = Final + ".part";
    uint64_t Existing = 0;
    if (FileSize(Final, Existing) && Existing == Spec.DownloadBytes) {
      Say(LogKind::Info, "The image is already downloaded");
      Report(Base, StepWeights::Download, 1, "Downloaded");
      return nullptr;
    }
    std::remove(Final.c_str());
    uint64_t Have = 0;
    if (FileSize(Part, Have)) {
      if (Have > Spec.DownloadBytes) {
        std::remove(Part.c_str());
        Have = 0;
      } else if (Have) {
        Say(LogKind::Info, "Resuming the download at " + Size(Have));
      }
    }
    const char* Error = nullptr;
    for (int Attempt = 0; Attempt < 4; ++Attempt) {
      if (Env.Cancelled()) return "Cancelled";
      if (!FileSize(Part, Have)) Have = 0;
      if (Have == Spec.DownloadBytes) break;
      auto* File = std::fopen(Part.c_str(), Have ? "ab" : "wb");
      if (!File) return "Could not write the download file";
      std::setvbuf(File, nullptr, _IOFBF, 256 * 1024);
      Http::ResponseInfo Info;
      uint64_t Received = 0;
      bool WriteFailed = false, TooLong = false;
      const auto Started = MonotonicSeconds();
      uint64_t LastReport = 0;
      Error = Env.HttpGet(
          Spec.Url, Have,
          [&](const uint8_t* Data, size_t Count) {
            // Never write more than the image size: a bad server could otherwise fill the card.
            if (Have + Received + Count > Spec.DownloadBytes) {
              TooLong = true;
              return false;
            }
            if (std::fwrite(Data, 1, Count, File) != Count) {
              WriteFailed = true;
              return false;
            }
            Received += Count;
            if (Received - LastReport >= 512 * 1024) {
              LastReport = Received;
              const double Elapsed = std::max(0.001, MonotonicSeconds() - Started);
              const uint64_t Total = Have + Received;
              Report(Base, StepWeights::Download, double(Total) / double(std::max<uint64_t>(1, Spec.DownloadBytes)),
                     Size(Total) + " / " + Size(Spec.DownloadBytes) + "  " + Size(static_cast<uint64_t>(Received / Elapsed)) + "/s");
              Env.Pump();
            }
            return !Env.Cancelled();
          },
          Info,
          [&](const Http::ResponseInfo& Head) {
            if (Head.Status == 200 && Have) {
              // The server ignored the range and sends the whole file: start the file over.
              Say(LogKind::Warn, "The server does not support resuming; starting over");
              std::fclose(File);
              File = std::fopen(Part.c_str(), "wb");
              Have = 0;
              if (!File) {
                WriteFailed = true;
                return false;
              }
            }
            return true;
          });
      if (File) std::fclose(File);
      if (WriteFailed) return "Could not write the download file (SD card full or read-only)";
      if (TooLong) {
        std::remove(Part.c_str());
        Detail = "more than " + Size(Spec.DownloadBytes);
        return "The server sent more data than the image size";
      }
      if (Error && std::strcmp(Error, "Cancelled") == 0) return "Cancelled";
      if (!Error) {
        if (!FileSize(Part, Have)) Have = 0;
        break;
      }
      Say(LogKind::Warn, std::string("Download interrupted: ") + Error + " (HTTP " + std::to_string(Info.Status) + "), retrying");
    }
    if (!FileSize(Part, Have) || Have != Spec.DownloadBytes) {
      Detail = Error ? Error : "the file has the wrong size";
      return "The download did not complete";
    }
    std::remove(Final.c_str());
    if (std::rename(Part.c_str(), Final.c_str())) return "Could not finish the download file";
    Report(Base, StepWeights::Download, 1, "Downloaded");
    return nullptr;
  }

  const char* Verify(const std::string& ImagePath, double Base) {
    const auto Expected = ParseHash64(Spec.Hash);
    // The index hash belongs to the index file. A card image with another name is the user's own image: no hash to check.
    if (!Spec.LocalPath.empty() && Spec.LocalPath.substr(Spec.LocalPath.find_last_of('/') + 1) != Spec.FileName) {
      Say(LogKind::Warn, "The image is not from the index: it is not verified");
      Report(Base, StepWeights::Verify, 1, "Not verified");
      return nullptr;
    }
    if (Spec.Hash.empty()) {
      Say(LogKind::Warn, "No hash to check: the image is not verified");
      Report(Base, StepWeights::Verify, 1, "Not verified");
      return nullptr;
    }
    if (!Expected) return "The image hash in the index is not valid";
    uint64_t SizeNow = 0;
    if (!FileSize(ImagePath, SizeNow)) return "Could not read the image file";
    uint64_t Actual = 0;
    bool Cancelled = false;
    const bool Read = HashFileXxh3(ImagePath, Actual, [&](uint64_t Done) {
      Report(Base, StepWeights::Verify, double(Done) / double(std::max<uint64_t>(1, SizeNow)), "Checking " + Size(Done) + " / " + Size(SizeNow));
      Env.Pump();
      Cancelled = Env.Cancelled();
      return !Cancelled;
    });
    if (Cancelled) return "Cancelled";
    if (!Read) return "Could not read the image file";
    if (Actual != *Expected) {
      Detail = "expected " + Hex64(*Expected) + ", got " + Hex64(Actual);
      if (Spec.LocalPath.empty()) std::remove(ImagePath.c_str());
      return "The image does not match its XXH3 hash";
    }
    Say(LogKind::Ok, "XXH3-64 " + Hex64(Actual) + " matches");
    return nullptr;
  }

  const char* Extract(const std::string& ImagePath, RootfsWriter& Writer, double Base) {
    Squashfs::StdioImageSource Source(ImagePath);
    if (!Source.Opened()) return "Could not open the image file";
    Squashfs::Reader Reader(Source);
    if (const auto* Error = Reader.Open()) {
      Detail = Reader.Detail;
      return Error;
    }
    ImageSize Measured;
    Say(LogKind::Info, "Reading the image's file list");
    if (const auto* Error = MeasureImage(Reader, Measured, [&] {
          Env.Pump();
          return Env.Cancelled();
        }))
      return Error;
    Say(LogKind::Info, std::to_string(Measured.Entries) + " entries, " + Size(Measured.Bytes) + " of file data");
    uint64_t LastReport = 0;
    ExtractHooks Hooks;
    Hooks.BuilderToRoot = true;
    Hooks.Cancelled = [&] { return Env.Cancelled(); };
    Hooks.Log = [&](LogKind Kind, const std::string& Line) { Say(Kind, Line); };
    uint64_t Calls = 0, LastEntries = 0;
    ExtractCostModel Cost;
    Cost.SetTotals(double(Measured.Bytes), double(Measured.Entries));
    const auto Started = MonotonicSeconds();
    double LastSample = 0;
    Hooks.Entry = [&](const ExtractStats& Stats, const std::string& Path) {
      if (++Calls % 64 == 0) Env.Pump();  // runs of small entries and links carry no bytes: still let the UI draw
      const uint64_t Entries = Stats.Directories + Stats.Files + Stats.Symlinks + Stats.HardLinks + Stats.Fifos + Stats.SkippedDevices + Stats.SkippedSockets;
      if (Stats.Bytes - LastReport >= 256 * 1024 || Entries - LastEntries >= 256 || Entries <= 1) {
        LastReport = Stats.Bytes;
        LastEntries = Entries;
        const double Elapsed = MonotonicSeconds() - Started;
        if (Elapsed - LastSample >= 0.25) {
          LastSample = Elapsed;
          Cost.Add(Elapsed, double(Stats.Bytes), double(Entries));
        }
        // The fraction is the share of the predicted time (bytes and entries), not of the bytes alone.
        Report(Base, StepWeights::Extract, Cost.Fraction(double(Stats.Bytes), double(Entries)),
               Size(Stats.Bytes) + " / " + Size(Measured.Bytes) + "  " + Path);
        Env.Pump();
      }
    };
    if (const auto* Error = ExtractSquashfs(Reader, Writer, Hooks, Extracted, Detail)) return Error;
    Say(LogKind::Ok, "Extracted " + std::to_string(Extracted.Files) + " files, " + std::to_string(Extracted.Directories) + " folders, " +
                         std::to_string(Extracted.Symlinks) + " links, " + std::to_string(Extracted.HardLinks) + " hard links");
    if (Extracted.SkippedDevices || Extracted.SkippedSockets)
      Say(LogKind::Warn, std::to_string(Extracted.SkippedDevices) + " device nodes and " + std::to_string(Extracted.SkippedSockets) + " sockets skipped");
    return nullptr;
  }

  // Fetches a small text file fully into memory.
  const char* FetchText(const std::string& Url, std::string& Text) {
    Text.clear();
    Http::ResponseInfo Info;
    const char* Error = nullptr;
    for (int Attempt = 0; Attempt < 3; ++Attempt) {
      Text.clear();
      Error = Env.HttpGet(Url, 0, [&](const uint8_t* Data, size_t Count) {
        Text.append(reinterpret_cast<const char*>(Data), Count);
        Env.Pump();
        return Text.size() < (8u << 20) && !Env.Cancelled();
      }, Info);
      if (!Error) return nullptr;
      if (std::strcmp(Error, "Cancelled") == 0) return Error;
    }
    Detail = Url + ": " + Error;
    return "Could not download from the Ubuntu archive";
  }

  const char* FetchToFile(const std::string& Url, const std::string& Native, uint64_t Expected) {
    auto* File = std::fopen(Native.c_str(), "wb");
    if (!File) return "Could not write a download file";
    Http::ResponseInfo Info;
    bool WriteFailed = false, TooLong = false;
    const char* Error = nullptr;
    // The index gives the size; files without one (InRelease) are small.
    const uint64_t Limit = Expected ? Expected : (64ull << 20);
    for (int Attempt = 0; Attempt < 3; ++Attempt) {
      std::fclose(File);
      File = std::fopen(Native.c_str(), "wb");
      if (!File) return "Could not write a download file";
      uint64_t Got = 0;
      Error = Env.HttpGet(Url, 0, [&](const uint8_t* Data, size_t Count) {
        if (Got + Count > Limit) {
          TooLong = true;
          return false;
        }
        Got += Count;
        if (std::fwrite(Data, 1, Count, File) != Count) {
          WriteFailed = true;
          return false;
        }
        Env.Pump();
        return !Env.Cancelled();
      }, Info);
      if (!Error || WriteFailed || TooLong || std::strcmp(Error, "Cancelled") == 0) break;
    }
    std::fclose(File);
    if (WriteFailed) return "Could not write a download file (SD card full or read-only)";
    if (TooLong) {
      Detail = Url + ": more than " + std::to_string(Limit) + " bytes";
      return "A download from the Ubuntu archive has the wrong size";
    }
    if (Error) {
      if (std::strcmp(Error, "Cancelled") == 0) return Error;
      Detail = Url + ": " + Error;
      return "Could not download from the Ubuntu archive";
    }
    uint64_t Size = 0;
    if (!FileSize(Native, Size) || (Expected && Size != Expected)) {
      Detail = Url + ": wrong size";
      return "A download from the Ubuntu archive has the wrong size";
    }
    return nullptr;
  }

  struct Candidate {
    PackageRecord Record;
    std::string Suite;
  };

  const char* InstallExtras(RootfsWriter& Writer, double Base, InstallResult& Result) {
    const auto Weight = StepWeights::Extras;
    std::vector<uint8_t> StatusBytes;
    if (Writer.ReadWholeFile("/var/lib/dpkg/status", StatusBytes)) {
      Say(LogKind::Warn, "The image has no dpkg status file: extra packages skipped");
      Result.Warning += "Extra packages skipped (no dpkg database). ";
      return nullptr;
    }
    std::string Status(StatusBytes.begin(), StatusBytes.end());
    auto Installed = InstalledNames(Status);
    std::set<std::string> Want;
    for (const auto& Extra : ExtraPackages) {
      RootfsWriter::TreeEntry Present;
      const bool FileMissing = Extra.File && Writer.TreeStat(Extra.File, Present) == -2;
      if (Installed.count(Extra.Name) && !FileMissing) {
        Say(LogKind::Info, std::string(Extra.Name) + " is already in the image");
        continue;
      }
      if (FileMissing && Installed.count(Extra.Name))
        Say(LogKind::Info, std::string(Extra.Name) + " is in the dpkg database but " + Extra.File + " is missing: unpacking it");
      Want.insert(Extra.Name);
    }
    if (Want.empty()) {
      Report(Base, Weight, 1, "No extra packages needed");
      return nullptr;
    }
    const std::string AptFolder = std::string(DownloadFolderPath) + "/apt";
    MakeFolders(Env, AptFolder);

    // Index files per suite and component: InRelease gives the hashes, the Packages.xz are scanned in a stream.
    std::map<std::string, std::map<std::string, std::pair<std::string, uint64_t>>> Releases;
    std::vector<std::pair<std::string, std::string>> Fetched; // suite, component
    std::vector<std::string> Components {"main"};
    std::map<std::string, Candidate> Chosen;
    std::vector<std::string> Order;
    std::set<std::string> Scanned = Want;
    for (int Pass = 0; Pass < 6; ++Pass) {
      if (Env.Cancelled()) return "Cancelled";
      // Make sure every wanted index is on disk.
      for (const auto& Component : Components) {
        for (const auto* Suite : ArchiveSuites) {
          if (std::find(Fetched.begin(), Fetched.end(), std::make_pair(std::string(Suite), Component)) != Fetched.end()) continue;
          if (!Releases.count(Suite)) {
            Report(Base, Weight, 0.05, std::string("Reading the ") + Suite + " release");
            std::string Text;
            if (const auto* Error = FetchText(std::string(ArchiveBase) + "/dists/" + Suite + "/InRelease", Text)) return Error;
            // The index arrives over plain HTTP: only the text the archive key signed is used (its hashes pin the
            // Packages files, which pin each .deb).
            std::string Signed;
            const auto Keys = Env.ArchiveKeys();
            if (const auto* Problem = OpenPgp::VerifyClearsigned(Text, Keys, Signed)) {
              Detail = std::string(Suite) + "/InRelease: " + Problem;
              return "The Ubuntu archive index is not signed by the Ubuntu archive key";
            }
            Releases[Suite] = ParseReleaseHashes(Signed);
          }
          const auto Path = Component + "/binary-amd64/Packages.xz";
          const auto Found = Releases[Suite].find(Path);
          if (Found == Releases[Suite].end()) {
            Detail = std::string(Suite) + " has no " + Path;
            return "The Ubuntu archive index is missing";
          }
          const auto File = AptFolder + "/" + Suite + "_" + Component + ".xz";
          Report(Base, Weight, 0.1, std::string("Downloading the ") + Suite + " " + Component + " index");
          if (const auto* Error = FetchToFile(std::string(ArchiveBase) + "/dists/" + Suite + "/" + Path, Env.Native(File), Found->second.second))
            return Error;
          std::string Actual;
          if (!Sha256File(Env.Native(File), Actual, nullptr) || Actual != Found->second.first) {
            Detail = std::string(Suite) + "/" + Path;
            return "An Ubuntu archive index does not match its release hash";
          }
          Fetched.emplace_back(Suite, Component);
        }
      }
      // Scan every index for the wanted names; later suites override earlier ones.
      Chosen.clear();
      for (const auto& [Suite, Component] : Fetched) {
        Report(Base, Weight, 0.3, "Reading the " + Suite + " " + Component + " index");
        StanzaScanner Scanner([&](const std::string& Paragraph) {
          const auto Name = StatusField(Paragraph, "Package");
          if (!Scanned.count(Name)) return;
          const auto Architecture = StatusField(Paragraph, "Architecture");
          if (Architecture != "amd64" && Architecture != "all") return;
          Candidate Item;
          Item.Suite = Suite;
          Item.Record.Name = Name;
          Item.Record.Version = StatusField(Paragraph, "Version");
          Item.Record.Filename = StatusField(Paragraph, "Filename");
          Item.Record.Sha256 = StatusField(Paragraph, "SHA256");
          Item.Record.Size = Http::ParseNumber(StatusField(Paragraph, "Size")).value_or(0);
          Item.Record.Depends = StatusField(Paragraph, "Pre-Depends");
          if (const auto Depends = StatusField(Paragraph, "Depends"); !Depends.empty())
            Item.Record.Depends += (Item.Record.Depends.empty() ? "" : ", ") + Depends;
          Item.Record.Provides = StatusField(Paragraph, "Provides");
          Chosen[Name] = std::move(Item);
        });
        const auto File = Env.Native(AptFolder + "/" + Suite + "_" + Component + ".xz");
        if (!XzStreamFile(File, [&](const uint8_t* Data, size_t Count) {
              Scanner.Feed(Data, Count);
              Env.Pump();
            }, [&] { return Env.Cancelled(); })) {
          if (Env.Cancelled()) return "Cancelled";
          Detail = Suite + " " + Component;
          return "An Ubuntu archive index is corrupt";
        }
        Scanner.Finish();
      }
      // Resolve: a chosen package's unmet dependencies join the wanted set.
      std::set<std::string> Next = Scanned;
      Order.clear();
      std::set<std::string> Visiting, Done;
      std::function<void(const std::string&)> Visit = [&](const std::string& Name) {
        if (Done.count(Name) || Visiting.count(Name)) return;
        Visiting.insert(Name);
        const auto Item = Chosen.find(Name);
        if (Item != Chosen.end()) {
          for (const auto& Group : ParseDepends(Item->second.Record.Depends)) {
            bool Satisfied = false;
            for (const auto& Alternative : Group)
              if (Installed.count(Alternative)) Satisfied = true;
            if (Satisfied) continue;
            // The first alternative that is available, else the first one.
            const std::string* Pick = &Group.front();
            for (const auto& Alternative : Group) {
              if (Chosen.count(Alternative)) {
                Pick = &Alternative;
                break;
              }
            }
            if (!Scanned.count(*Pick)) Next.insert(*Pick);
            if (Chosen.count(*Pick)) Visit(*Pick);
          }
        }
        Visiting.erase(Name);
        Done.insert(Name);
        if (Item != Chosen.end()) Order.push_back(Name);
      };
      for (const auto& Extra : Want) Visit(Extra);
      if (Next != Scanned) {
        Scanned = Next; // new dependency names: scan again
        continue;
      }
      std::string Missing;
      for (const auto& Name : Scanned)
        if (!Chosen.count(Name) && Missing.empty()) Missing = Name;
      if (Missing.empty()) break;
      if (Components.size() < 2) {
        Components.push_back("universe"); // not in main: add the universe index
        continue;
      }
      Detail = Missing;
      return "A package is missing from the Ubuntu archive";
    }
    if (Order.empty()) return "No package could be selected from the Ubuntu archive";

    // Download, verify and unpack in dependency order. The image's dpkg database stays as it is; only the info folder is made sure of.
    if (Writer.MakeDirectory(RootfsSetup::DpkgInfo, 0755, 0, 0) < 0) return "Could not create the dpkg database";
    std::vector<RootfsSetup::InstalledPackage> Records;
    uint64_t TotalBytes = 0, DoneBytes = 0;
    for (const auto& Name : Order) TotalBytes += Chosen[Name].Record.Size;
    for (const auto& Name : Order) {
      if (Env.Cancelled()) return "Cancelled";
      const auto& Record = Chosen[Name].Record;
      const auto File = AptFolder + "/" + Name + ".deb";
      Say(LogKind::Info, "Package " + Name + " " + Record.Version);
      if (const auto* Error = FetchToFile(std::string(ArchiveBase) + "/" + Record.Filename, Env.Native(File), Record.Size)) return Error;
      std::string Actual;
      if (!Sha256File(Env.Native(File), Actual, nullptr) || Actual != Record.Sha256) {
        Detail = Name;
        return "A package does not match the hash in the Ubuntu archive index";
      }
      std::vector<uint8_t> Deb;
      if (!ReadChunks(Env.Native(File), [&](const uint8_t* Data, size_t Count) {
            Deb.insert(Deb.end(), Data, Data + Count);
            return true;
          })) {
        Detail = Name;
        return "Could not read a downloaded package";
      }
      RootfsSetup::InstalledPackage Package;
      std::string UnpackDetail;
      if (const auto* Error = RootfsSetup::UnpackDeb(Deb, Writer, Package, UnpackDetail)) {
        Detail = Name + (UnpackDetail.empty() ? "" : " " + UnpackDetail);
        return Error;
      }
      Records.push_back(std::move(Package));
      ExtrasInstalled.push_back(Name + " " + Record.Version);
      std::remove(Env.Native(File).c_str());
      DoneBytes += Record.Size;
      Report(Base, Weight, 0.4 + 0.6 * double(DoneBytes) / double(std::max<uint64_t>(1, TotalBytes)), "Unpacked " + Name);
      Say(LogKind::Ok, "Unpacked " + Name + " " + Record.Version);
    }
    // dpkg database: the unpacked packages replace any entry the image keeps for them, then join the status file.
    std::string NewStatus = WithoutReplacedEntries(Status, Records);
    NewStatus += RootfsSetup::StatusFile(Records);
    if (Writer.WriteFile("/var/lib/dpkg/status", RootfsSetup::Bytes(NewStatus), 0644, 0, 0, Env.Now()) < 0) return "Could not write /var/lib/dpkg/status";
    // Remove the apt downloads.
    for (const auto& [Suite, Component] : Fetched) std::remove(Env.Native(AptFolder + "/" + Suite + "_" + Component + ".xz").c_str());
    return nullptr;
  }

  static std::string JsonEscape(std::string_view Text) {
    std::string Out;
    for (char C : Text) {
      if (C == '"' || C == '\\') {
        Out += '\\';
        Out += C;
      } else if (static_cast<unsigned char>(C) < 0x20) {
        char Buffer[8];
        std::snprintf(Buffer, sizeof(Buffer), "\\u%04x", C);
        Out += Buffer;
      } else {
        Out += C;
      }
    }
    return Out;
  }

  bool WriteMarker() {
    MakeFolders(Env, "/fex/config");
    std::string Json = "{\n  \"installed\": true,\n  \"name\": \"" + JsonEscape(Spec.Name) + "\",\n  \"image\": \"" + JsonEscape(Spec.FileName) +
                       "\",\n  \"url\": \"" + JsonEscape(Spec.Url) + "\",\n  \"hash\": \"" + JsonEscape(Spec.Hash) +
                       "\",\n  \"hash_kind\": \"xxh3-64\",\n  \"verified\": " + (Spec.Hash.empty() ? "false" : "true") + ",\n  \"installed_at\": " +
                       std::to_string(Env.Now()) + ",\n  \"extras\": [";
    for (size_t Index = 0; Index < ExtrasInstalled.size(); ++Index) Json += (Index ? ", \"" : "\"") + JsonEscape(ExtrasInstalled[Index]) + "\"";
    Json += std::string("],\n  \"extras_configured\": ") + (ExtrasConfigured ? "true" : "false") + "\n}\n";
    return WriteTextFile(Env, MarkerPath, Json);
  }

  void Cleanup(bool Cancelled) {
    if (Created) Say(LogKind::Info, Cancelled ? "Cancelled: removing everything the install created" : "Failed: removing the incomplete RootFS");
    else Say(LogKind::Info, Cancelled ? "Cancelled: the installed RootFS was not changed" : "Failed: the installed RootFS was not changed");
    if (Created) {
      std::remove(Env.Native(MarkerPath).c_str());
      std::string Error;
      if (!Env.WipeRootfs({}, Error)) Say(LogKind::Error, "Could not remove the incomplete RootFS: " + Error);
    }
    // A downloaded image that passed its hash check stays for the next try
    // (Download reuses a complete file).
    if (Cancelled && Spec.LocalPath.empty() && !Verified) {
      std::remove(Env.Native(DownloadPath()).c_str());
      std::remove((Env.Native(DownloadPath()) + ".part").c_str());
    }
    // The apt scratch files never stay.
    for (const auto* Suite : ArchiveSuites)
      for (const auto* Component : ArchiveComponents)
        std::remove(Env.Native(std::string(DownloadFolderPath) + "/apt/" + Suite + "_" + Component + ".xz").c_str());
    for (const auto& Extra : ExtraPackages) std::remove(Env.Native(std::string(DownloadFolderPath) + "/apt/" + Extra.Name + ".deb").c_str());
  }

  InstallEnv& Env;
  ImageSpec Spec;
  InstallOptions Options;
  std::string Detail;
  bool Created {};
  bool Verified {};
};

} // namespace Switch2Guest::RootfsInstall
