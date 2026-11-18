/* SPDX-License-Identifier: MIT */
ENTRY(_start)
PHDRS {
    text PT_LOAD FILEHDR PHDRS FLAGS(5);
    rodata PT_LOAD FLAGS(4);
}

SECTIONS {
    . = SIZEOF_HEADERS;
    .text : { *(.text) } :text
    . = ALIGN(0x1000);
    .rodata : { *(.rodata) } :rodata
    /DISCARD/ : { *(.dynamic) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) }
}
