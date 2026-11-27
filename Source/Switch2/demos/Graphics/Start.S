/* SPDX-License-Identifier: MIT */
.global _start
.section .text.start
_start:
    mov (%rsp), %rdi
    lea 8(%rsp), %rsi
    call graphics_main
    mov %eax, %edi
    mov $60, %eax
    syscall
    ud2
