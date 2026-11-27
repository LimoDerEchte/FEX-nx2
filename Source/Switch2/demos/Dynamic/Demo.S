/* SPDX-License-Identifier: MIT */
.global _start
.section .text
_start:
    call fex_demo_message@PLT
    mov %eax, %edi
    mov $60, %eax
    syscall
    ud2
