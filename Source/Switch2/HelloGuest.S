/* SPDX-License-Identifier: MIT */
.global _start
.section .text
_start:
    mov $1, %eax
    mov $1, %edi
    lea Message(%rip), %rsi
    mov $MessageSize, %edx
    syscall

    cmp $MessageSize, %rax
    jne .Lwrite_failed
    mov $42, %edi
    jmp .Lexit
.Lwrite_failed:
    mov $1, %edi
.Lexit:
    mov $60, %eax
    syscall
    ud2

.section .rodata
Message:
    .ascii "Hello from an x86-64 ELF on Switch 2!\n"
.equ MessageSize, . - Message
