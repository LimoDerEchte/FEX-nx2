// SPDX-License-Identifier: MIT
#include "../SDL2/GuestSyscall.h"

#include <elf.h>
#include <stddef.h>
#include <stdint.h>

#ifndef DEMO_LIBRARY_DIR
#define DEMO_LIBRARY_DIR "/lib/"
#endif

enum { Limit = 64 * 1024 };
typedef struct {
  const char* strings;
  const Elf64_Sym* symbols;
  const uint32_t* hash;
  const Elf64_Rela* plt;
  size_t plt_size;
  size_t rela_size;
  size_t needed_offset;
  unsigned needed;
  int plt_type;
} Dynamic;

static int equal(const char* a, const char* b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

static Dynamic parse_dynamic(uintptr_t base, const Elf64_Phdr* ph, size_t count) {
  Dynamic out = {0};
  for (size_t i = 0; i < count; ++i) {
    if (ph[i].p_type != PT_DYNAMIC) continue;
    const Elf64_Dyn* tags = (const Elf64_Dyn*)(base + ph[i].p_vaddr);
    for (size_t j = 0; j < ph[i].p_memsz / sizeof(*tags) && tags[j].d_tag != DT_NULL; ++j) {
      const uint64_t value = tags[j].d_un.d_val;
      switch (tags[j].d_tag) {
      case DT_STRTAB: out.strings = (const char*)(base + value); break;
      case DT_SYMTAB: out.symbols = (const Elf64_Sym*)(base + value); break;
      case DT_HASH: out.hash = (const uint32_t*)(base + value); break;
      case DT_JMPREL: out.plt = (const Elf64_Rela*)(base + value); break;
      case DT_PLTRELSZ: out.plt_size = value; break;
      case DT_PLTREL: out.plt_type = value; break;
      case DT_RELASZ: out.rela_size = value; break;
      case DT_NEEDED: ++out.needed; out.needed_offset = value; break;
      }
    }
  }
  return out;
}

static void* allocate(void) {
  const long address = guest_syscall(9, 0, Limit, 3, 0x22, -1, 0);
  return address < 0 ? NULL : (void*)address;
}

static uintptr_t load_library(const char* path, Dynamic* metadata) {
  unsigned char* file = allocate();
  if (!file) return 0;
  const long fd = guest_call(257, -100, (long)path, 0);
  size_t size = 0;
  if (fd >= 0) {
    while (size < Limit) {
      const long received = guest_call(0, fd, (long)(file + size), Limit - size);
      if (received <= 0) {
        if (received < 0) size = 0;
        break;
      }
      size += received;
    }
    guest_call(3, fd, 0, 0);
  }
  uintptr_t result = 0;
  if (size >= sizeof(Elf64_Ehdr) && size < Limit) {
    const Elf64_Ehdr* header = (const Elf64_Ehdr*)file;
    if (header->e_ident[0] == ELFMAG0 && header->e_ident[1] == ELFMAG1 &&
        header->e_ident[2] == ELFMAG2 && header->e_ident[3] == ELFMAG3 &&
        header->e_ident[EI_CLASS] == ELFCLASS64 && header->e_ident[EI_DATA] == ELFDATA2LSB &&
        header->e_type == ET_DYN && header->e_machine == EM_X86_64 &&
        header->e_phentsize == sizeof(Elf64_Phdr) && header->e_phoff <= size &&
        header->e_phnum <= (size - header->e_phoff) / sizeof(Elf64_Phdr)) {
      const Elf64_Phdr* ph = (const Elf64_Phdr*)(file + header->e_phoff);
      unsigned char* image = allocate();
      if (image) {
        int valid = 1;
        for (size_t i = 0; i < header->e_phnum; ++i) {
          if (ph[i].p_type != PT_LOAD && ph[i].p_type != PT_DYNAMIC) continue;
          if (ph[i].p_vaddr > Limit || ph[i].p_memsz > Limit - ph[i].p_vaddr ||
              ph[i].p_filesz > ph[i].p_memsz || ph[i].p_offset > size || ph[i].p_filesz > size - ph[i].p_offset) {
            valid = 0;
            break;
          }
          if (ph[i].p_type == PT_LOAD) {
            for (size_t j = 0; j < ph[i].p_filesz; ++j) image[ph[i].p_vaddr + j] = file[ph[i].p_offset + j];
          }
        }
        if (valid) {
          *metadata = parse_dynamic((uintptr_t)image, ph, header->e_phnum);
          valid = metadata->symbols && metadata->strings && metadata->hash && !metadata->needed &&
            !metadata->rela_size && !metadata->plt_size;
        }
        if (valid && guest_call(10, (long)image, Limit, 5) == 0) {
          result = (uintptr_t)image;
        } else {
          guest_call(11, (long)image, Limit, 0);
        }
      }
    }
  }
  guest_call(11, (long)file, Limit, 0);
  return result;
}

uintptr_t load_program(const uintptr_t* stack) {
  const uintptr_t* aux = stack + stack[0] + 2;
  while (*aux++) {} // Skip envp, including the terminator.
  uintptr_t entry = 0;
  const Elf64_Phdr* ph = NULL;
  size_t count = 0;
  for (; aux[0]; aux += 2) {
    if (aux[0] == AT_ENTRY) entry = aux[1];
    if (aux[0] == AT_PHDR) ph = (const Elf64_Phdr*)aux[1];
    if (aux[0] == AT_PHNUM) count = aux[1];
  }
  if (!entry || !ph || !count) goto fail;
  uintptr_t base = 0;
  for (size_t i = 0; i < count; ++i) {
    if (ph[i].p_type == PT_PHDR) base = (uintptr_t)ph - ph[i].p_vaddr;
  }
  if (!base) goto fail;
  const Dynamic program = parse_dynamic(base, ph, count);
  if (program.needed != 1 || !program.strings || !program.symbols || !program.plt ||
      program.plt_type != DT_RELA || program.plt_size % sizeof(Elf64_Rela) || program.rela_size ||
      !equal(program.strings + program.needed_offset, "libfex-demo.so")) goto fail;
  Dynamic library = {0};
  const uintptr_t library_base = load_library(DEMO_LIBRARY_DIR "libfex-demo.so", &library);
  if (!library_base) goto fail;
  for (size_t i = 0; i < program.plt_size / sizeof(Elf64_Rela); ++i) {
    const Elf64_Rela* relocation = &program.plt[i];
    if (ELF64_R_TYPE(relocation->r_info) != R_X86_64_JUMP_SLOT) goto fail;
    const char* name = program.strings + program.symbols[ELF64_R_SYM(relocation->r_info)].st_name;
    uintptr_t address = 0;
    for (uint32_t j = 1; j < library.hash[1]; ++j) {
      const Elf64_Sym* symbol = &library.symbols[j];
      if (symbol->st_shndx != SHN_UNDEF && equal(name, library.strings + symbol->st_name)) {
        address = library_base + symbol->st_value;
        break;
      }
    }
    if (!address) goto fail;
    *(uintptr_t*)(base + relocation->r_offset) = address;
  }
  return entry;

fail:;
  static const char error[] = "Could not link the test ELF and its rootfs library.\n";
  guest_call(1, 2, (long)error, sizeof(error) - 1);
  return 0;
}
