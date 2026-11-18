// SPDX-License-Identifier: MIT
#include "GuestElf.h"

#include <algorithm>
#include <cstring>
#include <elf.h>
#include <optional>

namespace Switch2Guest {
const char* LoadElf(std::span<const uint8_t> File, ElfImage& Image) {
  Elf64_Ehdr Header {};
  if (File.size() < sizeof(Header) || File.size() > MaxElfSize) {
    return "Invalid ELF file size.";
  }
  std::memcpy(&Header, File.data(), sizeof(Header));
  if (std::memcmp(Header.e_ident, ELFMAG, SELFMAG) != 0 || Header.e_ident[EI_CLASS] != ELFCLASS64 ||
      Header.e_ident[EI_DATA] != ELFDATA2LSB || Header.e_ident[EI_VERSION] != EV_CURRENT ||
      Header.e_version != EV_CURRENT || Header.e_machine != EM_X86_64 || Header.e_type != ET_DYN ||
      Header.e_ehsize != sizeof(Header)) {
    return "Expected a little-endian x86-64 position-independent ELF.";
  }

  if (Header.e_phentsize != sizeof(Elf64_Phdr) || Header.e_phnum == 0 || Header.e_phoff > File.size() ||
      Header.e_phnum > (File.size() - Header.e_phoff) / sizeof(Elf64_Phdr)) {
    return "Invalid ELF program headers.";
  }

  ElfImage Loaded;
  std::array<Elf64_Phdr, 16> Loads {};
  uint64_t ImageEnd {};
  bool EntryIsExecutable {};
  std::optional<uint64_t> ProgramHeaders;
  bool HasDynamicSegment {};
  for (size_t Index = 0; Index < Header.e_phnum; ++Index) {
    Elf64_Phdr SegmentHeader {};
    std::memcpy(&SegmentHeader, File.data() + Header.e_phoff + Index * sizeof(SegmentHeader), sizeof(SegmentHeader));
    if (SegmentHeader.p_type == PT_INTERP) {
      if (!Loaded.Interpreter.empty() || SegmentHeader.p_filesz < 2 || SegmentHeader.p_filesz > 256 ||
          SegmentHeader.p_offset > File.size() || SegmentHeader.p_filesz > File.size() - SegmentHeader.p_offset) {
        return "Invalid ELF interpreter entry.";
      }
      const char* Path = reinterpret_cast<const char*>(File.data() + SegmentHeader.p_offset);
      if (Path[0] != '/' || std::memchr(Path, 0, SegmentHeader.p_filesz) != Path + SegmentHeader.p_filesz - 1) {
        return "Invalid ELF interpreter path.";
      }
      Loaded.Interpreter.assign(Path, SegmentHeader.p_filesz - 1);
    }
    HasDynamicSegment |= SegmentHeader.p_type == PT_DYNAMIC;
    if (SegmentHeader.p_type != PT_LOAD || (SegmentHeader.p_filesz == 0 && SegmentHeader.p_memsz == 0)) {
      continue;
    }
    if (Loaded.SegmentCount == Loads.size() || SegmentHeader.p_filesz > SegmentHeader.p_memsz ||
        SegmentHeader.p_offset > File.size() || SegmentHeader.p_filesz > File.size() - SegmentHeader.p_offset ||
        SegmentHeader.p_vaddr >= MaxElfSize || SegmentHeader.p_memsz > MaxElfSize - SegmentHeader.p_vaddr ||
        SegmentHeader.p_align > PageSize ||
        (SegmentHeader.p_align > 1 && ((SegmentHeader.p_align & (SegmentHeader.p_align - 1)) ||
         (SegmentHeader.p_vaddr % SegmentHeader.p_align != SegmentHeader.p_offset % SegmentHeader.p_align)))) {
      return "Invalid or unsupported ELF load segment.";
    }
    const uint64_t End = SegmentHeader.p_vaddr + SegmentHeader.p_memsz;
    for (size_t Previous = 0; Previous < Loaded.SegmentCount; ++Previous) {
      const auto& Other = Loads[Previous];
      if (SegmentHeader.p_vaddr < Other.p_vaddr + Other.p_memsz && Other.p_vaddr < End) {
        return "Overlapping ELF load segments.";
      }
    }
    if ((SegmentHeader.p_flags & PF_X) && Header.e_entry >= SegmentHeader.p_vaddr &&
        Header.e_entry - SegmentHeader.p_vaddr < SegmentHeader.p_filesz) {
      EntryIsExecutable = true;
    }
    const auto TableBytes = size_t {Header.e_phnum} * sizeof(Elf64_Phdr);
    if (Header.e_phoff >= SegmentHeader.p_offset && Header.e_phoff - SegmentHeader.p_offset <= SegmentHeader.p_filesz &&
        TableBytes <= SegmentHeader.p_filesz - (Header.e_phoff - SegmentHeader.p_offset)) {
      ProgramHeaders = SegmentHeader.p_vaddr + Header.e_phoff - SegmentHeader.p_offset;
    }
    ImageEnd = std::max(ImageEnd, End);
    Loads[Loaded.SegmentCount++] = SegmentHeader;
  }

  if (!EntryIsExecutable) {
    return "ELF entry point is outside executable file data.";
  }

  if (!Loaded.Interpreter.empty() && (!ProgramHeaders || !HasDynamicSegment)) {
    return "Dynamic ELF must map its program headers and dynamic table.";
  }

  // Reserve a zeroed page beyond the image for the decoder's lookahead.
  const size_t AllocationSize = ((ImageEnd + PageSize - 1) / PageSize + 1) * PageSize;
  Loaded.Storage.reset(static_cast<uint8_t*>(std::aligned_alloc(PageSize, AllocationSize)));
  if (!Loaded.Storage) {
    return "Could not allocate guest memory.";
  }
  std::memset(Loaded.Storage.get(), 0, AllocationSize);
  const auto Base = reinterpret_cast<uint64_t>(Loaded.Storage.get());
  for (size_t Index = 0; Index < Loaded.SegmentCount; ++Index) {
    const auto& Load = Loads[Index];
    std::memcpy(Loaded.Storage.get() + Load.p_vaddr, File.data() + Load.p_offset, Load.p_filesz);
    Loaded.Segments[Index] = {Base + Load.p_vaddr, Load.p_memsz, Load.p_flags};
  }
  Loaded.Entry = Base + Header.e_entry;
  Loaded.Base = Base;
  Loaded.ProgramHeaders = ProgramHeaders ? Base + *ProgramHeaders : 0;
  Loaded.ProgramHeaderCount = Header.e_phnum;
  Image = std::move(Loaded);
  return nullptr;
}
} // namespace Switch2Guest
