/* SPDX-License-Identifier: MIT */
.global _start
.section .text.start
_start:
    mov %rsp, %r12
    mov %rsp, %rdi
    call load_program
    test %rax, %rax
    jz .Lfailed
    mov %r12, %rsp
    xor %edx, %edx
    jmp *%rax
.Lfailed:
    mov $127, %edi
    mov $60, %eax
    syscall
    ud2
