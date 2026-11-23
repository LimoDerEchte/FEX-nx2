// SPDX-License-Identifier: MIT
// Debian rootfs setup for the Switch 2 launcher: the package manifest, .deb
// (ar) members, xz decompression, tar extraction and the dpkg database. This
// is the first stage of a debootstrap: packages are unpacked and recorded as
// "install ok unpacked"; their maintainer scripts run later from the guest
// with `dpkg --configure -a`. Portable, so it is tested natively.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include "ThirdParty/xz-embedded/xz.h"
}

namespace Switch2Guest::RootfsSetup {

// ---------------------------------------------------------------------------
// Manifest (LinuxRuntime/RootfsManifest.py).

struct ManifestPackage {
  std::string Archive, Path, Sha256, Name, Version;
  uint64_t Size {};
};

struct Manifest {
  std::string Suite, Snapshot;
  std::map<std::string, std::string> Archives; // id -> base URL
  std::vector<ManifestPackage> Packages;
  uint64_t TotalSize() const {
    uint64_t Total {};
    for (const auto& Package : Packages) Total += Package.Size;
    return Total;
  }
};

inline std::vector<std::string_view> SplitFields(std::string_view Line) {
  std::vector<std::string_view> Fields;
  while (!Line.empty()) {
    const auto Space = Line.find(' ');
    if (Space) Fields.push_back(Line.substr(0, Space));
    if (Space == Line.npos) break;
    Line.remove_prefix(Space + 1);
  }
  return Fields;
}

inline bool IsHex(std::string_view Text) {
  return std::all_of(Text.begin(), Text.end(), [](char C) { return (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f'); });
}

// Returns an error text, or nullptr.
inline const char* ParseManifest(std::string_view Text, Manifest& Out) {
  Out = {};
  while (!Text.empty()) {
    const auto End = Text.find('\n');
    auto Line = Text.substr(0, End);
    Text.remove_prefix(End == Text.npos ? Text.size() : End + 1);
    if (!Line.empty() && Line.back() == '\r') Line.remove_suffix(1);
    if (Line.empty() || Line.front() == '#') continue;
    const auto Fields = SplitFields(Line);
    if (Fields[0] == "suite" && Fields.size() == 2) {
      Out.Suite = Fields[1];
    } else if (Fields[0] == "snapshot" && Fields.size() == 2) {
      Out.Snapshot = Fields[1];
    } else if (Fields[0] == "archive" && Fields.size() == 3) {
      Out.Archives[std::string {Fields[1]}] = Fields[2];
    } else if (Fields[0] == "package" && Fields.size() == 7) {
      ManifestPackage Package {std::string {Fields[1]}, std::string {Fields[2]}, std::string {Fields[4]},
                               std::string {Fields[5]}, std::string {Fields[6]}};
      const auto SizeText = Fields[3];
      if (SizeText.empty() || SizeText.size() > 12 || !std::all_of(SizeText.begin(), SizeText.end(), [](char C) { return C >= '0' && C <= '9'; }))
        return "Bad package size in manifest";
      for (char C : SizeText) Package.Size = Package.Size * 10 + (C - '0');
      if (Package.Sha256.size() != 64 || !IsHex(Package.Sha256)) return "Bad SHA-256 in manifest";
      // Pool paths only: no absolute paths, parent references or URL syntax.
      if (!Package.Path.starts_with("pool/") || Package.Path.find("..") != std::string::npos ||
          Package.Path.find_first_of("?#%:") != std::string::npos)
        return "Bad package path in manifest";
      if (!Out.Archives.contains(Package.Archive)) return "Unknown archive in manifest";
      Out.Packages.push_back(std::move(Package));
    } else {
      return "Unknown manifest line";
    }
  }
  if (Out.Packages.empty()) return "Manifest lists no packages";
  return nullptr;
}

// ---------------------------------------------------------------------------
// ar archive (.deb container).

struct ArMember {
  std::string Name;
  std::span<const uint8_t> Data;
};

inline std::optional<std::vector<ArMember>> ReadAr(std::span<const uint8_t> Archive) {
  if (Archive.size() < 8 || std::memcmp(Archive.data(), "!<arch>\n", 8)) return std::nullopt;
  std::vector<ArMember> Members;
  size_t Offset = 8;
  while (Offset + 60 <= Archive.size()) {
    const auto* Header = reinterpret_cast<const char*>(Archive.data() + Offset);
    if (Header[58] != '`' || Header[59] != '\n') return std::nullopt;
    std::string Name(Header, 16);
    Name.erase(Name.find_last_not_of(' ') + 1);
    if (Name.ends_with('/')) Name.pop_back(); // GNU ar terminator
    uint64_t Size {};
    for (int Index = 48; Index < 58 && Header[Index] != ' '; ++Index) {
      if (Header[Index] < '0' || Header[Index] > '9') return std::nullopt;
      Size = Size * 10 + (Header[Index] - '0');
    }
    Offset += 60;
    if (Size > Archive.size() - Offset) return std::nullopt;
    Members.push_back({std::move(Name), Archive.subspan(Offset, Size)});
    Offset += Size + (Size & 1);
  }
  return Members;
}

// ---------------------------------------------------------------------------
// xz.

inline bool XzDecode(std::span<const uint8_t> Input, std::vector<uint8_t>& Output) {
  xz_crc32_init();
#ifdef XZ_USE_CRC64
  xz_crc64_init();
#endif
  auto* Decoder = xz_dec_init(XZ_DYNALLOC, 1u << 27);
  if (!Decoder) return false;
  Output.assign(std::max<size_t>(Input.size() * 4, 1 << 16), 0);
  xz_buf Buffer {Input.data(), 0, Input.size(), Output.data(), 0, Output.size()};
  xz_ret Result;
  for (;;) {
    Result = xz_dec_run(Decoder, &Buffer);
    if (Result != XZ_OK) break;
    if (Buffer.out_pos == Buffer.out_size) {
      Output.resize(Output.size() * 2);
      Buffer.out = Output.data();
      Buffer.out_size = Output.size();
    } else if (Buffer.in_pos == Buffer.in_size) {
      Result = XZ_DATA_ERROR; // truncated
      break;
    }
  }
  xz_dec_end(Decoder);
  Output.resize(Buffer.out_pos);
  return Result == XZ_STREAM_END;
}

// Decompresses a data.tar.* or control.tar.* member by its suffix.
inline const char* DecodeMember(const ArMember& Member, std::vector<uint8_t>& Output) {
  if (Member.Name.ends_with(".tar")) {
    Output.assign(Member.Data.begin(), Member.Data.end());
    return nullptr;
  }
  if (Member.Name.ends_with(".tar.xz")) return XzDecode(Member.Data, Output) ? nullptr : "Corrupt xz data in package";
  return "Unsupported package compression (only xz)";
}

// ---------------------------------------------------------------------------
// tar.

enum class EntryType { File, HardLink, Symlink, Directory, CharDevice, BlockDevice, Fifo };

struct TarEntry {
  std::string Path;     // absolute, without a trailing slash; "/" for the root
  std::string LinkName; // symlink target, or the absolute path of a hard link's source
  EntryType Type {};
  uint32_t Mode {}, Uid {}, Gid {}, Major {}, Minor {};
  int64_t ModifyTime {};
  std::span<const uint8_t> Data;
};

inline std::optional<uint64_t> TarNumber(const char* Field, size_t Width) {
  if (static_cast<uint8_t>(Field[0]) & 0x80) {
    // GNU base-256 for large values.
    uint64_t Value = static_cast<uint8_t>(Field[0]) & 0x7f;
    for (size_t Index = 1; Index < Width; ++Index) Value = (Value << 8) | static_cast<uint8_t>(Field[Index]);
    return Value;
  }
  uint64_t Value {};
  size_t Index {};
  while (Index < Width && Field[Index] == ' ') ++Index;
  for (; Index < Width && Field[Index] && Field[Index] != ' '; ++Index) {
    if (Field[Index] < '0' || Field[Index] > '7') return std::nullopt;
    Value = Value * 8 + (Field[Index] - '0');
  }
  return Value;
}

// "./usr/bin/" -> "/usr/bin". Rejects parent references.
inline std::optional<std::string> NormalizeMemberPath(std::string_view Name) {
  while (Name.starts_with("./")) Name.remove_prefix(2);
  while (Name.starts_with('/')) Name.remove_prefix(1);
  while (Name.ends_with('/')) Name.remove_suffix(1);
  if (Name == ".") Name = {};
  std::string Path = "/";
  for (size_t Start = 0; Start < Name.size();) {
    const auto End = std::min(Name.find('/', Start), Name.size());
    const auto Part = Name.substr(Start, End - Start);
    if (Part == "..") return std::nullopt;
    if (!Part.empty() && Part != ".") {
      if (Path.size() > 1) Path += '/';
      Path += Part;
    }
    Start = End + 1;
  }
  return Path;
}

inline std::string FieldText(const char* Field, size_t Width) {
  return std::string(Field, strnlen(Field, Width));
}

// Calls Visit(const TarEntry&) for each entry; Visit returns an error text or
// nullptr. Handles ustar, GNU long names (L/K) and pax path/linkpath/size.
template<typename F>
const char* ReadTar(std::span<const uint8_t> Tar, F&& Visit) {
  size_t Offset {};
  std::optional<std::string> LongName, LongLink;
  std::optional<uint64_t> PaxSize;
  while (Offset + 512 <= Tar.size()) {
    const auto* Header = reinterpret_cast<const char*>(Tar.data() + Offset);
    if (std::all_of(Header, Header + 512, [](char C) { return C == 0; })) return nullptr;
    uint32_t Sum {};
    for (int Index = 0; Index < 512; ++Index) Sum += Index >= 148 && Index < 156 ? ' ' : static_cast<uint8_t>(Header[Index]);
    if (TarNumber(Header + 148, 8) != Sum) return "Bad tar checksum";
    auto Size = TarNumber(Header + 124, 12);
    if (!Size) return "Bad tar size";
    if (PaxSize) Size = PaxSize;
    Offset += 512;
    if (*Size > Tar.size() - Offset) return "Truncated tar";
    const auto Data = Tar.subspan(Offset, *Size);
    Offset += (*Size + 511) & ~uint64_t {511};
    const char Type = Header[156];
    if (Type == 'L' || Type == 'K') {
      (Type == 'L' ? LongName : LongLink) = std::string(reinterpret_cast<const char*>(Data.data()), strnlen(reinterpret_cast<const char*>(Data.data()), Data.size()));
      continue;
    }
    if (Type == 'x' || Type == 'g') {
      if (Type == 'g') continue;
      // "<len> key=value\n" records.
      std::string_view Records(reinterpret_cast<const char*>(Data.data()), Data.size());
      while (!Records.empty()) {
        const auto Space = Records.find(' ');
        if (Space == Records.npos) return "Bad pax header";
        size_t Length {};
        for (char C : Records.substr(0, Space)) Length = Length * 10 + (C - '0');
        if (Length <= Space + 1 || Length > Records.size()) return "Bad pax header";
        auto Record = Records.substr(Space + 1, Length - Space - 2);
        Records.remove_prefix(Length);
        const auto Equals = Record.find('=');
        if (Equals == Record.npos) continue;
        const auto Key = Record.substr(0, Equals), Value = Record.substr(Equals + 1);
        if (Key == "path") LongName = std::string {Value};
        else if (Key == "linkpath") LongLink = std::string {Value};
        else if (Key == "size") {
          uint64_t Number {};
          for (char C : Value) Number = Number * 10 + (C - '0');
          PaxSize = Number;
        }
      }
      continue;
    }
    TarEntry Entry;
    std::string Name = LongName ? *LongName : FieldText(Header, 100);
    if (!LongName && std::memcmp(Header + 257, "ustar", 5) == 0 && Header[345]) Name = FieldText(Header + 345, 155) + "/" + Name;
    const auto Path = NormalizeMemberPath(Name);
    if (!Path) return "Unsafe path in package";
    Entry.Path = *Path;
    Entry.LinkName = LongLink ? *LongLink : FieldText(Header + 157, 100);
    LongName.reset();
    LongLink.reset();
    PaxSize.reset();
    Entry.Mode = TarNumber(Header + 100, 8).value_or(0) & 07777;
    Entry.Uid = TarNumber(Header + 108, 8).value_or(0);
    Entry.Gid = TarNumber(Header + 116, 8).value_or(0);
    Entry.ModifyTime = TarNumber(Header + 136, 12).value_or(0);
    Entry.Major = TarNumber(Header + 329, 8).value_or(0);
    Entry.Minor = TarNumber(Header + 337, 8).value_or(0);
    switch (Type) {
    case '0': case '\0': case '7': Entry.Type = EntryType::File; Entry.Data = Data; break;
    case '1': {
      Entry.Type = EntryType::HardLink;
      const auto Source = NormalizeMemberPath(Entry.LinkName);
      if (!Source) return "Unsafe link in package";
      Entry.LinkName = *Source;
      break;
    }
    case '2': Entry.Type = EntryType::Symlink; break;
    case '3': Entry.Type = EntryType::CharDevice; break;
    case '4': Entry.Type = EntryType::BlockDevice; break;
    case '5': Entry.Type = EntryType::Directory; break;
    case '6': Entry.Type = EntryType::Fifo; break;
    default: return "Unsupported tar entry type";
    }
    if (const auto* Error = Visit(Entry)) return Error;
  }
  return "Truncated tar";
}

// ---------------------------------------------------------------------------
// Writing into the guest rootfs. Errors are negative Linux errno values.

class RootfsWriter {
public:
  virtual ~RootfsWriter() = default;
  // Creates the directory with Mode and owner. Like dpkg, an existing
  // directory, or a symlink to one, is left unchanged.
  virtual int64_t MakeDirectory(const std::string& Path, uint32_t Mode, uint32_t Uid, uint32_t Gid) = 0;
  // Writes Path atomically (via Path.dpkg-new and rename, as dpkg does).
  virtual int64_t WriteFile(const std::string& Path, std::span<const uint8_t> Data, uint32_t Mode, uint32_t Uid,
                            uint32_t Gid, int64_t ModifyTime) = 0;
  virtual int64_t Symlink(const std::string& Target, const std::string& Path, uint32_t Uid, uint32_t Gid) = 0;
  virtual int64_t HardLink(const std::string& Existing, const std::string& Path) = 0;
  // Character and block devices and FIFOs; Mode includes the file type bits.
  virtual int64_t MakeNode(const std::string& Path, uint32_t Mode, uint64_t Device, uint32_t Uid, uint32_t Gid) = 0;
};

inline std::span<const uint8_t> Bytes(std::string_view Text) {
  return {reinterpret_cast<const uint8_t*>(Text.data()), Text.size()};
}

// ---------------------------------------------------------------------------
// Control files and the dpkg database.

// Returns the value of Field in a control paragraph, or empty.
inline std::string ControlField(std::string_view Control, std::string_view Field) {
  size_t Start {};
  while (Start < Control.size()) {
    const auto End = std::min(Control.find('\n', Start), Control.size());
    const auto Line = Control.substr(Start, End - Start);
    if (Line.size() > Field.size() && Line.starts_with(Field) && Line[Field.size()] == ':') {
      auto Value = Line.substr(Field.size() + 1);
      while (Value.starts_with(' ')) Value.remove_prefix(1);
      return std::string {Value};
    }
    Start = End + 1;
  }
  return {};
}

struct InstalledPackage {
  std::string Name;    // dpkg database name: "libc6:amd64" for Multi-Arch: same
  std::string Control; // the control paragraph, without a trailing blank line
  std::vector<std::string> Conffiles;
};

// The status paragraph dpkg writes for an unpacked, unconfigured package.
inline std::string StatusParagraph(const InstalledPackage& Package) {
  std::string Paragraph;
  std::string_view Control = Package.Control;
  bool StatusWritten {};
  while (!Control.empty()) {
    const auto End = Control.find('\n');
    const auto Line = Control.substr(0, End);
    Control.remove_prefix(End == Control.npos ? Control.size() : End + 1);
    if (Line.empty()) continue;
    Paragraph.append(Line).push_back('\n');
    if (!StatusWritten && Line.starts_with("Package:")) {
      Paragraph += "Status: install ok unpacked\n";
      StatusWritten = true;
    }
  }
  if (!Package.Conffiles.empty()) {
    Paragraph += "Conffiles:\n";
    for (const auto& File : Package.Conffiles) Paragraph += " " + File + " newconffile\n";
  }
  return Paragraph;
}

inline std::string StatusFile(const std::vector<InstalledPackage>& Packages) {
  std::string Text;
  for (const auto& Package : Packages) Text += StatusParagraph(Package) + "\n";
  return Text;
}

inline constexpr const char* DpkgDirectory = "/var/lib/dpkg";
inline constexpr const char* DpkgInfo = "/var/lib/dpkg/info";

// Unpacks one .deb into the writer and its control files into the dpkg info
// directory. Returns an error text, or nullptr.
inline const char* UnpackDeb(std::span<const uint8_t> Deb, RootfsWriter& Writer, InstalledPackage& Out, std::string& Detail) {
  const auto Members = ReadAr(Deb);
  if (!Members || Members->size() < 3 || (*Members)[0].Name != "debian-binary") return "Not a Debian package";
  const ArMember* ControlMember {};
  const ArMember* DataMember {};
  for (const auto& Member : *Members) {
    if (Member.Name.starts_with("control.tar")) ControlMember = &Member;
    if (Member.Name.starts_with("data.tar")) DataMember = &Member;
  }
  if (!ControlMember || !DataMember) return "Package lacks control or data";
  std::vector<uint8_t> ControlTar, DataTar;
  if (const auto* Error = DecodeMember(*ControlMember, ControlTar)) return Error;

  std::map<std::string, TarEntry> ControlFiles;
  if (const auto* Error = ReadTar(ControlTar, [&](const TarEntry& Entry) -> const char* {
        if (Entry.Type == EntryType::File && Entry.Path.rfind('/') == 0) ControlFiles[Entry.Path.substr(1)] = Entry;
        return nullptr;
      }))
    return Error;
  const auto Control = ControlFiles.find("control");
  if (Control == ControlFiles.end()) return "Package has no control file";
  std::string ControlText(reinterpret_cast<const char*>(Control->second.Data.data()), Control->second.Data.size());
  while (ControlText.ends_with('\n')) ControlText.pop_back();
  Out = {};
  Out.Control = ControlText;
  Out.Name = ControlField(ControlText, "Package");
  if (Out.Name.empty()) return "Package has no name";
  if (ControlField(ControlText, "Multi-Arch") == "same") Out.Name += ":" + ControlField(ControlText, "Architecture");
  if (const auto Conffiles = ControlFiles.find("conffiles"); Conffiles != ControlFiles.end()) {
    std::string_view Text(reinterpret_cast<const char*>(Conffiles->second.Data.data()), Conffiles->second.Data.size());
    while (!Text.empty()) {
      const auto End = Text.find('\n');
      auto Line = Text.substr(0, End);
      Text.remove_prefix(End == Text.npos ? Text.size() : End + 1);
      // "remove-on-upgrade /etc/x" lines name files that are not shipped.
      if (Line.starts_with('/')) Out.Conffiles.emplace_back(Line);
    }
  }

  if (const auto* Error = DecodeMember(*DataMember, DataTar)) return Error;
  std::string List = "/.\n";
  const char* Failure = ReadTar(DataTar, [&](const TarEntry& Entry) -> const char* {
    if (Entry.Path == "/") return nullptr;
    int64_t Result {};
    switch (Entry.Type) {
    case EntryType::Directory: Result = Writer.MakeDirectory(Entry.Path, Entry.Mode, Entry.Uid, Entry.Gid); break;
    case EntryType::File:
      Result = Writer.WriteFile(Entry.Path, Entry.Data, Entry.Mode, Entry.Uid, Entry.Gid, Entry.ModifyTime);
      break;
    case EntryType::Symlink: Result = Writer.Symlink(Entry.LinkName, Entry.Path, Entry.Uid, Entry.Gid); break;
    case EntryType::HardLink: Result = Writer.HardLink(Entry.LinkName, Entry.Path); break;
    case EntryType::CharDevice:
    case EntryType::BlockDevice:
    case EntryType::Fifo: {
      const uint32_t Format = Entry.Type == EntryType::CharDevice ? 0020000 : Entry.Type == EntryType::BlockDevice ? 0060000 : 0010000;
      // Linux dev_t encoding (makedev).
      const uint64_t Device = (uint64_t {Entry.Major & 0xfff} << 8) | (Entry.Minor & 0xff) | (uint64_t {Entry.Minor & ~0xffu} << 12);
      Result = Writer.MakeNode(Entry.Path, Format | Entry.Mode, Device, Entry.Uid, Entry.Gid);
      break;
    }
    }
    if (Result < 0) {
      Detail = Entry.Path + " (error " + std::to_string(-Result) + ")";
      return "Could not write a package file";
    }
    List += Entry.Path + "\n";
    return nullptr;
  });
  if (Failure) return Failure;

  const auto Info = std::string {DpkgInfo} + "/" + Out.Name + ".";
  if (Writer.WriteFile(Info + "list", Bytes(List), 0644, 0, 0, 0) < 0) {
    Detail = Info + "list";
    return "Could not write the dpkg database";
  }
  for (const auto& [Name, Entry] : ControlFiles) {
    if (Name == "control") continue;
    if (Writer.WriteFile(Info + Name, Entry.Data, Entry.Mode, 0, 0, Entry.ModifyTime) < 0) {
      Detail = Info + Name;
      return "Could not write the dpkg database";
    }
  }
  return nullptr;
}

// Directories and files of an empty dpkg database (as debootstrap creates).
inline const char* PrepareDpkgDatabase(RootfsWriter& Writer) {
  for (const char* Directory : {"/var", "/var/lib", "/var/lib/dpkg", "/var/lib/dpkg/info", "/var/lib/dpkg/updates",
                                "/var/lib/dpkg/triggers", "/var/lib/dpkg/alternatives", "/var/lib/dpkg/parts", "/etc",
                                "/etc/apt", "/etc/apt/sources.list.d"}) {
    if (Writer.MakeDirectory(Directory, 0755, 0, 0) < 0) return "Could not create the dpkg database";
  }
  for (const char* File : {"/var/lib/dpkg/available", "/var/lib/dpkg/diversions", "/var/lib/dpkg/statoverride"}) {
    if (Writer.WriteFile(File, {}, 0644, 0, 0, 0) < 0) return "Could not create the dpkg database";
  }
  return nullptr;
}

// apt sources for the installed suite (apt verifies the archive signatures).
inline std::string SourcesList(std::string_view Suite) {
  std::string Text;
  Text += "deb http://deb.debian.org/debian " + std::string {Suite} + " main\n";
  Text += "deb http://deb.debian.org/debian " + std::string {Suite} + "-updates main\n";
  Text += "deb http://security.debian.org/debian-security " + std::string {Suite} + "-security main\n";
  return Text;
}

// ---------------------------------------------------------------------------
// URLs.

inline std::string PackageUrl(const Manifest& List, const ManifestPackage& Package) {
  return List.Archives.at(Package.Archive) + "/" + Package.Path;
}

// snapshot.debian.org keeps every published file, so it still serves a
// package after the mirrors drop that version.
inline std::string SnapshotUrl(const Manifest& List, const ManifestPackage& Package) {
  const auto Archive = Package.Archive == "security" ? "debian-security" : "debian";
  return "http://snapshot.debian.org/archive/" + std::string {Archive} + "/" + List.Snapshot + "/" + Package.Path;
}

struct Url {
  std::string Host, Path;
  uint16_t Port {80};
};

inline std::optional<Url> ParseHttpUrl(std::string_view Text) {
  if (!Text.starts_with("http://")) return std::nullopt;
  Text.remove_prefix(7);
  const auto Slash = Text.find('/');
  Url Result;
  auto Authority = Text.substr(0, Slash);
  Result.Path = Slash == Text.npos ? "/" : std::string {Text.substr(Slash)};
  if (const auto Colon = Authority.find(':'); Colon != Authority.npos) {
    uint32_t Port {};
    for (char C : Authority.substr(Colon + 1)) {
      if (C < '0' || C > '9') return std::nullopt;
      Port = Port * 10 + (C - '0');
    }
    if (!Port || Port > 65535) return std::nullopt;
    Result.Port = Port;
    Authority = Authority.substr(0, Colon);
  }
  if (Authority.empty()) return std::nullopt;
  Result.Host = Authority;
  return Result;
}

// Parses an HTTP/1.1 response head. Returns the status, or 0 if malformed.
struct ResponseHead {
  int Status {};
  std::optional<uint64_t> ContentLength;
  bool Chunked {};
  std::string Location;
};

inline ResponseHead ParseResponseHead(std::string_view Head) {
  ResponseHead Result;
  const auto LineEnd = Head.find("\r\n");
  const auto StatusLine = Head.substr(0, LineEnd);
  if (!StatusLine.starts_with("HTTP/1.") || StatusLine.size() < 12) return {};
  for (char C : StatusLine.substr(9, 3)) {
    if (C < '0' || C > '9') return {};
    Result.Status = Result.Status * 10 + (C - '0');
  }
  for (size_t Start = LineEnd == Head.npos ? Head.size() : LineEnd + 2; Start < Head.size();) {
    const auto End = std::min(Head.find("\r\n", Start), Head.size());
    const auto Line = Head.substr(Start, End - Start);
    Start = End + 2;
    const auto Colon = Line.find(':');
    if (Colon == Line.npos) continue;
    std::string Name {Line.substr(0, Colon)};
    std::transform(Name.begin(), Name.end(), Name.begin(), [](char C) { return C >= 'A' && C <= 'Z' ? C + 32 : C; });
    auto Value = Line.substr(Colon + 1);
    while (Value.starts_with(' ')) Value.remove_prefix(1);
    if (Name == "content-length") {
      uint64_t Length {};
      for (char C : Value) {
        if (C < '0' || C > '9') return {};
        Length = Length * 10 + (C - '0');
      }
      Result.ContentLength = Length;
    } else if (Name == "transfer-encoding") {
      std::string Lower {Value};
      std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](char C) { return C >= 'A' && C <= 'Z' ? C + 32 : C; });
      Result.Chunked = Lower.find("chunked") != std::string::npos;
    } else if (Name == "location") {
      Result.Location = Value;
    }
  }
  return Result;
}

// Removes chunked transfer framing. Returns false if malformed.
inline bool Dechunk(std::vector<uint8_t>& Body) {
  std::vector<uint8_t> Out;
  size_t Offset {};
  for (;;) {
    uint64_t Size {};
    size_t Digits {};
    while (Offset < Body.size() && Body[Offset] != '\r' && Body[Offset] != ';') {
      const char C = Body[Offset++];
      const int Value = C >= '0' && C <= '9' ? C - '0' : C >= 'a' && C <= 'f' ? C - 'a' + 10 : C >= 'A' && C <= 'F' ? C - 'A' + 10 : -1;
      if (Value < 0 || ++Digits > 15) return false;
      Size = Size * 16 + Value;
    }
    while (Offset + 1 < Body.size() && !(Body[Offset] == '\r' && Body[Offset + 1] == '\n')) ++Offset;
    Offset += 2;
    if (!Digits || Offset > Body.size()) return false;
    if (!Size) break;
    if (Size > Body.size() - Offset) return false;
    Out.insert(Out.end(), Body.begin() + Offset, Body.begin() + Offset + Size);
    Offset += Size + 2;
  }
  Body = std::move(Out);
  return true;
}

} // namespace Switch2Guest::RootfsSetup
