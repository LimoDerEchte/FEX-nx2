/* SPDX-License-Identifier: MIT */
.global _start
.section .text.start
_start:
    mov (%rsp), %edi
    lea 8(%rsp), %rsi
    and $-16, %rsp
    call main
    mov %eax, %edi
    mov $60, %eax
    syscall
    ud2
