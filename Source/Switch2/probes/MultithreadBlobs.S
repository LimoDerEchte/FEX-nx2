# SPDX-License-Identifier: MIT
# EXPERIMENT (multithreading Task 0): x86-64 test code the multithreading probe runs through FEXCore. Assembled on the
# build machine (host `as`, x86-64) into a flat blob; probes/MakeMultithreadBlobs.py writes the bytes and symbol offsets.
# Calling convention: the probe sets rdi/rsi/rdx and rip; every routine ends with exit(rdi) (`syscall`, rax 60), which the
# probe's syscall handler turns into a return to the host.
.intel_syntax noprefix
.text

.macro EXIT_WITH reg
  mov rdi, \reg
  mov eax, 60
  syscall
.endm

# spin(rdi iterations, rsi seed) -> LCG state after rdi steps. Pure compute, no memory.
.globl spin
spin:
  mov rax, rsi
  mov rcx, 6364136223846793005
  mov rdx, 1442695040888963407
1:
  imul rax, rcx
  add rax, rdx
  dec rdi
  jnz 1b
  EXIT_WITH rax

# forever(): a backward branch that never ends. Only a poll-word stop returns from it.
.globl forever
forever:
  xor eax, eax
1:
  inc rax
  jmp 1b

# smc(rdi wanted value, rsi progress counter): calls smc_target until it returns rdi; returns the call count.
.globl smc
smc:
1:
  call .Lsmc_target
  inc qword ptr [rsi]
  cmp eax, edi
  jne 1b
  mov rax, [rsi]
  EXIT_WITH rax
.globl smc_target
smc_target:
.Lsmc_target:
  mov eax, 0x11111111
  ret

# mp_writer(rdi count, rsi base): data at [rsi], flag at [rsi+64]; stores data then flag, 1..count.
.globl mp_writer
mp_writer:
  xor ecx, ecx
1:
  inc rcx
  mov [rsi], rcx
  mov [rsi+64], rcx
  cmp rcx, rdi
  jne 1b
  EXIT_WITH 0

# mp_reader(rdi count, rsi base) -> number of times the flag was newer than the data (impossible under x86 TSO).
.globl mp_reader
mp_reader:
  xor edx, edx
1:
  mov rax, [rsi+64]
  mov rcx, [rsi]
  cmp rcx, rax
  jae 2f
  inc rdx
2:
  cmp rax, rdi
  jne 1b
  EXIT_WITH rdx

# mem_rmw(rdi passes, rsi buffer, rdx qword count): add 1 to every qword, rdi times. Loads and stores.
.globl mem_rmw
mem_rmw:
  mov r8, rdi
1:
  xor ecx, ecx
2:
  add qword ptr [rsi+rcx*8], 1
  inc rcx
  cmp rcx, rdx
  jne 2b
  dec r8
  jnz 1b
  EXIT_WITH 0

# mem_vec(rdi passes, rsi buffer, rdx qword count, even): copy rdx qwords at rsi to rsi+rdx*8 with movups.
.globl mem_vec
mem_vec:
  mov r8, rdi
  mov r9, rdx
  shl r9, 3
  lea r10, [rsi+r9]
1:
  xor ecx, ecx
2:
  movups xmm0, [rsi+rcx]
  movups [r10+rcx], xmm0
  add rcx, 16
  cmp rcx, r9
  jne 2b
  dec r8
  jnz 1b
  EXIT_WITH 0

# mem_copy(rdi passes, rsi buffer, rdx byte count): rep movsb the first half onto the second half.
.globl mem_copy
mem_copy:
  mov r8, rdi
  mov r11, rsi
  mov r9, rdx
1:
  mov rsi, r11
  lea rdi, [r11+r9]
  mov rcx, r9
  rep movsb
  dec r8
  jnz 1b
  EXIT_WITH 0

# chase(rdi steps, rsi start): follows rsi = [rsi]; returns the last pointer.
.globl chase
chase:
1:
  mov rsi, [rsi]
  dec rdi
  jnz 1b
  EXIT_WITH rsi
