// SPDX-License-Identifier: MIT
#pragma once

static inline long guest_syscall(long number, long first, long second, long third, long fourth, long fifth, long sixth) {
  register long r10 __asm__("r10") = fourth;
  register long r8 __asm__("r8") = fifth;
  register long r9 __asm__("r9") = sixth;
  __asm__ volatile("syscall" : "+a"(number) : "D"(first), "S"(second), "d"(third), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
  return number;
}

static inline long guest_call(long number, long first, long second, long third) {
  return guest_syscall(number, first, second, third, 0, 0, 0);
}
