/* SPDX-License-Identifier: MIT */
ENTRY(_start)
PHDRS {
    text PT_LOAD FILEHDR PHDRS FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS {
    . = SIZEOF_HEADERS;
    .text : { *(.text.start) *(.text .text.*) } :text
    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) } :rodata
    .rela.dyn : { *(.rela.dyn) } :rodata
    ASSERT(SIZEOF(.rela.dyn) == 0, "Guest ELF must be relocation-free")
    . = ALIGN(0x1000);
    .data : { *(.data .data.* .got .got.*) } :data
    .bss : { *(.bss .bss.* COMMON) } :data
    /DISCARD/ : { *(.dynamic) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) *(.eh_frame*) }
}
