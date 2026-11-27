// SPDX-License-Identifier: MIT
// EXPERIMENT (multithreading Task 1, xz slowness): times small kernels built from the operations liblzma's encoder
// uses, so the console/native ratio of each shows which one FEX translates badly. Prints "# name: N ps per step".
// With an argument N, runs N copies of the whole list in parallel threads (contention check).
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <emmintrin.h>

#define BUF (1u << 20)
static uint8_t *Data;
static uint8_t CrcTable[256 * 4];
static volatile uint64_t Sink;

static uint64_t NowNs(void) {
  struct timespec T;
  clock_gettime(CLOCK_MONOTONIC, &T);
  return (uint64_t) T.tv_sec * 1000000000ull + (uint64_t) T.tv_nsec;
}

#define KERNEL(Name) static __attribute__((noinline)) uint64_t Name(uint64_t Steps)

KERNEL(aligned_load64) {
  uint64_t S = 0;
  for (uint64_t I = 0; I < Steps; ++I) S += *(const uint64_t *) (Data + ((I * 8) & (BUF - 8)));
  return S;
}
KERNEL(unaligned_load64) {
  uint64_t S = 0;
  for (uint64_t I = 0; I < Steps; ++I) {
    uint64_t V;
    memcpy(&V, Data + ((I * 8 + 3) & (BUF - 16)), 8);
    S += V;
  }
  return S;
}
KERNEL(unaligned_load32_hash) {
  uint32_t S = 0, P = 1;
  for (uint64_t I = 0; I < Steps; ++I) {
    uint32_t V;
    memcpy(&V, Data + (P & (BUF - 8)), 4);
    P = P * 2654435761u + V;
    S ^= P >> 12;
  }
  return S;
}
KERNEL(ctz_of_xor) {
  uint64_t S = 0;
  for (uint64_t I = 0; I < Steps; ++I) {
    uint64_t A, B;
    memcpy(&A, Data + ((I * 7) & (BUF - 16)), 8);
    memcpy(&B, Data + ((I * 13 + 1) & (BUF - 16)), 8);
    S += (uint64_t) __builtin_ctzll((A ^ B) | 1u << 31);
  }
  return S;
}
KERNEL(switch_table) {
  uint64_t S = 1;
  for (uint64_t I = 0; I < Steps; ++I) {
    switch (Data[I & (BUF - 1)] & 7) {
    case 0: S += 3; break;
    case 1: S ^= I; break;
    case 2: S *= 5; break;
    case 3: S -= I >> 1; break;
    case 4: S += S >> 3; break;
    case 5: S ^= 0x55; break;
    case 6: S += 11; break;
    default: S = S * 3 + 1; break;
    }
  }
  return S;
}
KERNEL(unpredictable_branch) {
  uint64_t S = 0;
  for (uint64_t I = 0; I < Steps; ++I) {
    if (Data[I & (BUF - 1)] & 1) S += I;
    else S ^= I;
  }
  return S;
}
KERNEL(cmov_min) {
  uint32_t S = 0xffffffff;
  for (uint64_t I = 0; I < Steps; ++I) {
    uint32_t V = Data[I & (BUF - 1)] * 977u + (uint32_t) I;
    S = V < S ? V : S;
    S += 1;
  }
  return S;
}
KERNEL(byte_copy) {
  uint8_t *Out = Data + BUF / 2;
  for (uint64_t I = 0; I < Steps; ++I) Out[I & (BUF / 2 - 1)] = Data[(I * 3) & (BUF / 2 - 1)];
  return Out[5];
}
KERNEL(table_crc) {
  uint32_t C = 0xffffffff;
  for (uint64_t I = 0; I < Steps; ++I) C = (C >> 8) ^ ((const uint32_t *) CrcTable)[(C ^ Data[I & (BUF - 1)]) & 0xff];
  return C;
}
KERNEL(sse_movemask) {
  uint64_t S = 0;
  for (uint64_t I = 0; I < Steps; ++I) {
    const __m128i A = _mm_loadu_si128((const __m128i *) (Data + ((I * 16 + 1) & (BUF - 32))));
    const __m128i B = _mm_loadu_si128((const __m128i *) (Data + ((I * 16 + 9) & (BUF - 32))));
    S += (uint32_t) _mm_movemask_epi8(_mm_cmpeq_epi8(A, B));
  }
  return S;
}
KERNEL(mul64_shift) {
  uint64_t S = 1;
  for (uint64_t I = 0; I < Steps; ++I) S = (S * 0x9E3779B97F4A7C15ull) >> 7 ^ I;
  return S;
}
KERNEL(carry_add128) {
  uint64_t Low = 0, High = 0;
  for (uint64_t I = 0; I < Steps; ++I) {
    const uint64_t Add = (uint64_t) Data[I & (BUF - 1)] << 56 | I;
    Low += Add;
    High += Low < Add;
  }
  return Low ^ High;
}
KERNEL(store_load_forward) {
  uint32_t *Slot = (uint32_t *) (Data + 64);
  uint64_t S = 0;
  for (uint64_t I = 0; I < Steps; ++I) {
    *(volatile uint32_t *) Slot = (uint32_t) I;
    S += *(volatile uint32_t *) Slot;
  }
  return S;
}
static __attribute__((noinline)) uint32_t Leaf(uint32_t X) { return X * 3 + 1; }
KERNEL(direct_call) {
  uint32_t S = 0;
  for (uint64_t I = 0; I < Steps; ++I) S = Leaf(S);
  return S;
}
KERNEL(stack_spill) {
  volatile uint64_t Local[8] = {0};
  for (uint64_t I = 0; I < Steps; ++I) Local[I & 7] += I;
  return Local[3];
}

struct Entry {
  const char *Name;
  uint64_t (*Run)(uint64_t);
};
static const struct Entry Kernels[] = {
  {"aligned_load64", aligned_load64},       {"unaligned_load64", unaligned_load64}, {"unaligned_load32_hash", unaligned_load32_hash},
  {"ctz_of_xor", ctz_of_xor},               {"switch_table", switch_table},         {"unpredictable_branch", unpredictable_branch},
  {"cmov_min", cmov_min},                   {"byte_copy", byte_copy},               {"table_crc", table_crc},
  {"sse_movemask", sse_movemask},           {"mul64_shift", mul64_shift},           {"carry_add128", carry_add128},
  {"store_load_forward", store_load_forward}, {"direct_call", direct_call},         {"stack_spill", stack_spill},
};

static void *RunAll(void *Raw) {
  const int Print = Raw != NULL;
  for (size_t K = 0; K < sizeof(Kernels) / sizeof(Kernels[0]); ++K) {
    uint64_t Steps = 1 << 16, Elapsed;
    for (;;) {
      const uint64_t Start = NowNs();
      Sink += Kernels[K].Run(Steps);
      Elapsed = NowNs() - Start;
      if (Elapsed >= 100000000ull || Steps >= (1ull << 34)) break;
      Steps *= Elapsed < 10000000ull ? 8 : 2;
    }
    if (Print) printf("# %s: %llu ps per step\n", Kernels[K].Name, (unsigned long long) (Elapsed * 1000 / Steps));
  }
  return NULL;
}

int main(int Argc, char **Argv) {
  Data = aligned_alloc(64, BUF);
  for (uint32_t I = 0; I < BUF; ++I) Data[I] = (uint8_t) (I * 2654435761u >> 13);
  for (uint32_t I = 0; I < sizeof(CrcTable); ++I) CrcTable[I] = (uint8_t) (I * 31);
  const int Threads = Argc > 1 ? atoi(Argv[1]) : 1;
  printf("# op-bench, %d thread(s)\n", Threads);
  if (Threads <= 1) {
    RunAll((void *) 1);
  } else {
    pthread_t Ids[16];
    for (int I = 1; I < Threads && I < 16; ++I) pthread_create(&Ids[I], NULL, RunAll, NULL);
    RunAll((void *) 1);
    for (int I = 1; I < Threads && I < 16; ++I) pthread_join(Ids[I], NULL);
  }
  return 0;
}
