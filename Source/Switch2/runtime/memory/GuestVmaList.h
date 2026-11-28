// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Switch2Guest {

struct Vma {
  enum : uint8_t { None, Heap, Stack, Vvar, Vdso };
  enum BackingKind : uint8_t { Anonymous, File, Shmem, Sysv };

  uint64_t Start {}, End {};
  uint8_t Prot {};
  bool Shared {};
  uint64_t Offset {};
  uint32_t DevMajor {}, DevMinor {};
  uint64_t Inode {};
  std::string Name {};
  uint8_t Special {};
  BackingKind Backing {};
  uint64_t Object {}; // Shared anonymous allocation or System V backing address.
  bool MainText {};
  bool Deleted {};
  bool GrowsDown {};
  bool MayWrite {true};
  bool Accounted {};
  bool MayExecute {true};
  std::string BackingPath {};

  bool HasOffsets() const { return Inode || Backing != Anonymous || Shared; }

  bool SameSharedObject(const Vma& Other) const {
    if (!Shared) return false;
    // The mappings share a page only when their ranges of the object overlap.
    if (Offset >= Other.Offset + (Other.End - Other.Start) || Other.Offset >= Offset + (End - Start)) return false;
    if (Object || Other.Object) return Backing == Other.Backing && Object == Other.Object;
    return Inode && Inode == Other.Inode && DevMajor == Other.DevMajor && DevMinor == Other.DevMinor;
  }
};

class VmaList {
public:
  const std::vector<Vma>& Ranges() const { return Entries; }
  uint64_t PeakSize() const { return Peak; }
  void ResetPeak() { Peak = Size; }

  const Vma* Find(uint64_t Address) const {
    auto It = std::upper_bound(Entries.begin(), Entries.end(), Address,
                               [](uint64_t Start, const Vma& Range) { return Start < Range.Start; });
    if (It == Entries.begin() || (--It)->End <= Address) return nullptr;
    return &*It;
  }

  void Map(Vma Range) {
    if (Range.Start >= Range.End) return;
    Replace(Range.Start, Range.End, {std::move(Range)});
  }

  void Unmap(uint64_t Start, uint64_t Length) {
    if (!Length || Length > UINT64_MAX - Start) return;
    Replace(Start, Start + Length, {});
  }

  void Protect(uint64_t Start, uint64_t Length, uint8_t Prot) {
    if (!Length || Length > UINT64_MAX - Start) return;
    auto Parts = Slice(Start, Start + Length);
    for (auto& Range : Parts) {
      // mprotect_fixup: a private mapping that gains write access is charged and gets VM_ACCOUNT.
      if ((Prot & 2) && !(Range.Prot & 2) && !Range.Shared) Range.Accounted = true;
      Range.Prot = Prot;
    }
    Replace(Start, Start + Length, std::move(Parts));
  }

  void Remap(uint64_t OldStart, uint64_t OldLength, uint64_t NewStart, uint64_t NewLength, bool KeepOld = false) {
    if (!OldLength || !NewLength || OldLength > UINT64_MAX - OldStart || NewLength > UINT64_MAX - NewStart) return;
    auto Parts = Slice(OldStart, OldStart + std::min(OldLength, NewLength));
    const bool GrowTail = !Parts.empty() && NewLength > OldLength && Parts.back().End == OldStart + OldLength;
    for (auto& Range : Parts) {
      Range.Start = NewStart + (Range.Start - OldStart);
      Range.End = NewStart + (Range.End - OldStart);
    }
    if (GrowTail) Parts.back().End = NewStart + NewLength;
    if (!KeepOld) Unmap(OldStart, OldLength);
    Replace(NewStart, NewStart + NewLength, std::move(Parts));
  }

  template<typename Update>
  void UpdateNames(Update UpdateName) {
    for (auto& Range : Entries) UpdateName(Range);
  }

private:
  static Vma Trim(Vma Range, uint64_t Start, uint64_t End) {
    if (Range.HasOffsets()) Range.Offset += Start - Range.Start;
    Range.Start = Start;
    Range.End = End;
    return Range;
  }

  std::vector<Vma> Slice(uint64_t Start, uint64_t End) const {
    std::vector<Vma> Parts;
    for (const auto& Range : Entries) {
      if (Range.Start >= End) break;
      if (Range.End > Start) Parts.push_back(Trim(Range, std::max(Start, Range.Start), std::min(End, Range.End)));
    }
    return Parts;
  }

  static bool CanMerge(const Vma& Left, const Vma& Right) {
    if (Left.End != Right.Start || Left.Prot != Right.Prot || Left.Shared != Right.Shared ||
        Left.Backing != Right.Backing || Left.Special != Right.Special || Left.Name != Right.Name ||
        Left.DevMajor != Right.DevMajor || Left.DevMinor != Right.DevMinor || Left.Inode != Right.Inode ||
        Left.Object != Right.Object || Left.MainText != Right.MainText || Left.Deleted != Right.Deleted ||
        Left.GrowsDown != Right.GrowsDown || Left.MayWrite != Right.MayWrite || Left.Accounted != Right.Accounted ||
        Left.MayExecute != Right.MayExecute || Left.BackingPath != Right.BackingPath) return false;
    const uint64_t Size = Left.End - Left.Start;
    return !Left.HasOffsets() || (Size <= UINT64_MAX - Left.Offset && Left.Offset + Size == Right.Offset);
  }

  void Replace(uint64_t Start, uint64_t End, std::vector<Vma> Parts) {
    std::vector<Vma> Result;
    Result.reserve(Entries.size() + Parts.size() + 1);
    Size = 0;
    const auto Add = [&](Vma Range) {
      Size += Range.End - Range.Start;
      if (!Result.empty() && CanMerge(Result.back(), Range)) Result.back().End = Range.End;
      else Result.push_back(std::move(Range));
    };
    size_t Index = 0;
    while (Index < Entries.size() && Entries[Index].End <= Start) Add(std::move(Entries[Index++]));
    if (Index < Entries.size() && Entries[Index].Start < Start) {
      Add(Trim(Entries[Index], Entries[Index].Start, Start));
    }
    for (auto& Range : Parts) Add(std::move(Range));
    while (Index < Entries.size() && Entries[Index].End <= End) ++Index;
    if (Index < Entries.size() && Entries[Index].Start < End) {
      Add(Trim(Entries[Index], End, Entries[Index].End));
      ++Index;
    }
    while (Index < Entries.size()) Add(std::move(Entries[Index++]));
    Entries = std::move(Result);
    Peak = std::max(Peak, Size);
  }

  std::vector<Vma> Entries;
  uint64_t Size {}, Peak {};
};

constexpr uint64_t VmaPageBytes = 4096;
constexpr unsigned VmaPssShift = 12;

inline void AppendMapLine(std::string& Out, const Vma& Range) {
  using ULL = unsigned long long;
  char Prefix[100];
  const int Width = std::snprintf(Prefix, sizeof(Prefix), "%08llx-%08llx %c%c%c%c %08llx %02x:%02x %llu ",
                                  static_cast<ULL>(Range.Start), static_cast<ULL>(Range.End),
                                  Range.Prot & 1 ? 'r' : '-', Range.Prot & 2 ? 'w' : '-', Range.Prot & 4 ? 'x' : '-',
                                  Range.Shared ? 's' : 'p', static_cast<ULL>(Range.Inode ? Range.Offset : 0),
                                  Range.DevMajor, Range.DevMinor, static_cast<ULL>(Range.Inode));
  Out += Prefix;
  constexpr std::array<std::string_view, 5> Specials {"", "[heap]", "[stack]", "[vvar]", "[vdso]"};
  const auto Name = Range.Special ? Specials[Range.Special] : std::string_view(Range.Name);
  if (!Name.empty()) {
    // seq_pad: the name starts at column 73. The prefix is at most 67 characters, so the padding is never negative.
    Out.append(73 - Width, ' ');
    for (const char C : Name) {
      if (C == '\n') Out += "\\012";
      else Out += C;
    }
    if (Range.Deleted) Out += " (deleted)";
  }
  Out += '\n';
}

inline std::string FormatMaps(const VmaList& List) {
  std::string Out;
  for (const auto& Range : List.Ranges()) AppendMapLine(Out, Range);
  return Out;
}

struct VmaTotals {
  uint64_t Size {}, Rss {}, RssAnon {}, RssFile {}, RssShmem {};
  uint64_t Text {}, Data {}, Stack {}, Executable {}, Library {};
};

inline Vma::BackingKind ResidentKind(const Vma& Range) {
  if (!Range.Shared && (Range.Prot & 2)) return Vma::Anonymous;
  return Range.Backing;
}

template<typename Committed>
VmaTotals CountVmas(const VmaList& List, Committed ResidentPages) {
  VmaTotals Total;
  uint64_t TextStart = UINT64_MAX, TextEnd = 0;
  for (const auto& Range : List.Ranges()) {
    const auto Size = Range.End - Range.Start;
    const auto Rss = ResidentPages(Range) * VmaPageBytes;
    Total.Size += Size;
    Total.Rss += Rss;
    switch (ResidentKind(Range)) {
    case Vma::Anonymous: Total.RssAnon += Rss; break;
    case Vma::File: Total.RssFile += Rss; break;
    case Vma::Shmem: case Vma::Sysv: Total.RssShmem += Rss; break;
    }
    if (Range.MainText) {
      TextStart = std::min(TextStart, Range.Start);
      TextEnd = std::max(TextEnd, Range.End);
    }
    if (Range.GrowsDown) Total.Stack += Size;
    else if (!Range.Shared && (Range.Prot & 2)) Total.Data += Size;
    if ((Range.Prot & 4) && !(Range.Prot & 2) && !Range.Shared) {
      if (Range.MainText) Total.Executable += Size;
      else Total.Library += Size;
    }
  }
  if (TextEnd) Total.Text = TextEnd - TextStart;
  return Total;
}

struct VmaSmapsTotals {
  uint64_t Rss {}, PssAnon {}, PssFile {}, PssShmem {};
  uint64_t SharedClean {}, SharedDirty {}, PrivateClean {}, PrivateDirty {}, Anonymous {};

  void Add(const Vma& Range, uint64_t Pages, uint64_t Processes) {
    const uint64_t Bytes = Pages * VmaPageBytes;
    const uint64_t Pss = Pages * ((VmaPageBytes << VmaPssShift) / Processes);
    Rss += Bytes;
    switch (ResidentKind(Range)) {
    case Vma::Anonymous: PssAnon += Pss; break;
    case Vma::File: PssFile += Pss; break;
    case Vma::Shmem: case Vma::Sysv: PssShmem += Pss; break;
    }
    if (Processes > 1) {
      if (Range.Prot & 2) SharedDirty += Bytes;
      else SharedClean += Bytes;
    } else if (Range.Prot & 2) {
      PrivateDirty += Bytes;
    } else {
      PrivateClean += Bytes;
    }
    if (!Range.Shared && (Range.Prot & 2)) Anonymous += Bytes;
  }
};

inline void AppendSmapsField(std::string& Out, std::string_view Name, uint64_t Bytes) {
  Out += Name;
  Out += ':';
  Out.append(15 - Name.size(), ' ');
  const auto Value = std::to_string(Bytes >> 10);
  if (Value.size() < 8) Out.append(8 - Value.size(), ' ');
  Out += Value;
  Out += " kB\n";
}

inline void AppendSmapsTotals(std::string& Out, const VmaSmapsTotals& Total, bool Rollup) {
  const auto Add = [&](std::string_view Name, uint64_t Bytes) { AppendSmapsField(Out, Name, Bytes); };
  Add("Rss", Total.Rss);
  Add("Pss", (Total.PssAnon + Total.PssFile + Total.PssShmem) >> VmaPssShift);
  if (Rollup) {
    Add("Pss_Anon", Total.PssAnon >> VmaPssShift);
    Add("Pss_File", Total.PssFile >> VmaPssShift);
    Add("Pss_Shmem", Total.PssShmem >> VmaPssShift);
  }
  Add("Shared_Clean", Total.SharedClean);
  Add("Shared_Dirty", Total.SharedDirty);
  Add("Private_Clean", Total.PrivateClean);
  Add("Private_Dirty", Total.PrivateDirty);
  Add("Referenced", Total.Rss);
  Add("Anonymous", Total.Anonymous);
  for (const auto* Name : {"LazyFree", "AnonHugePages", "ShmemPmdMapped", "FilePmdMapped", "Shared_Hugetlb",
                           "Private_Hugetlb", "Swap", "SwapPss", "Locked"}) Add(Name, 0);
}

template<typename Committed, typename Sharing>
std::string FormatSmaps(const VmaList& List, Committed ResidentPages, Sharing ProcessCount) {
  std::string Out;
  for (const auto& Range : List.Ranges()) {
    AppendMapLine(Out, Range);
    AppendSmapsField(Out, "Size", Range.End - Range.Start);
    AppendSmapsField(Out, "KernelPageSize", VmaPageBytes);
    AppendSmapsField(Out, "MMUPageSize", VmaPageBytes);
    VmaSmapsTotals Total;
    Total.Add(Range, ResidentPages(Range), Range.Shared ? ProcessCount(Range) : 1);
    AppendSmapsTotals(Out, Total, false);
    Out += "THPeligible:    0\nVmFlags: ";
    if (Range.Prot & 1) Out += "rd ";
    if (Range.Prot & 2) Out += "wr ";
    if (Range.Prot & 4) Out += "ex ";
    if (Range.Shared) Out += "sh ";
    Out += "mr ";
    if (Range.MayWrite) Out += "mw ";
    if (Range.MayExecute) Out += "me ";
    if (Range.Shared) Out += "ms ";
    if (Range.GrowsDown) Out += "gd ";
    if (Range.Accounted) Out += "ac ";
    Out += '\n';
  }
  return Out;
}

template<typename Committed, typename Sharing>
std::string FormatSmapsRollup(const VmaList& List, Committed ResidentPages, Sharing ProcessCount) {
  const auto& Ranges = List.Ranges();
  if (Ranges.empty()) return {};
  std::string Out;
  AppendMapLine(Out, {.Start = Ranges.front().Start, .End = Ranges.back().End, .Name = "[rollup]"});
  VmaSmapsTotals Total;
  for (const auto& Range : Ranges) Total.Add(Range, ResidentPages(Range), Range.Shared ? ProcessCount(Range) : 1);
  AppendSmapsTotals(Out, Total, true);
  return Out;
}

} // namespace Switch2Guest
