// SPDX-License-Identifier: MIT
// Multithreading suite for the Switch 2 guest runtime (multithreading Task 1).
//
// Same harness as proc-suite: every test runs in its own forked child, the
// results are TAP lines, and the exit status is 42 when every selected test
// passes or skips. Expectations are Linux's; the suite passes natively
// before it runs on the console. Timing lines ("# ...") record what the
// console measured; only the scaling test checks a time ratio.
//
// Usage: thread-suite [-d dir] [pattern...] | thread-suite -l   (see proc-suite)
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/capability.h>
#include <linux/futex.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <string.h>
#include <dirent.h>
#include <sys/epoll.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/fsuid.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#ifndef TFD_IOC_SET_TICKS
#define TFD_IOC_SET_TICKS _IOW('T', 0, uint64_t)
#endif
#include <sys/timex.h>
#include <sys/times.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef P_PIDFD
#define P_PIDFD 3
#endif

#define SKIP_EXIT 77
#define TEST_TIMEOUT_SECONDS 10

// ---------------------------------------------------------------------------
// Harness

typedef void (*TestFunction)(void);
struct Test {
  const char *Name;
  TestFunction Function;
};
static struct Test Tests[512];
static unsigned TestCount;

static void Register(const char *Name, TestFunction Function) {
  if (TestCount < sizeof(Tests) / sizeof(Tests[0])) Tests[TestCount++] = (struct Test) {Name, Function};
}

#define TEST(group, name)                                                                    \
  static void group##_##name(void);                                                          \
  __attribute__((constructor)) static void Register_##group##_##name(void) {                 \
    Register(#group "/" #name, group##_##name);                                              \
  }                                                                                          \
  static void group##_##name(void)

// Shared with the per-test child so the skip reason reaches the TAP line.
struct Shared {
  char SkipReason[256];
};
static struct Shared *Shared;
static unsigned CaseFailures;
static int Skipped;

static const char *ErrnoName(int Error) {
  const char *Name = strerrorname_np(Error);
  return Name ? Name : "unknown";
}

static void Fail(int Line, const char *Text, long Result, int Error, int Expected) {
  printf("#   line %d: %s -> %ld", Line, Text, Result);
  if (Result == -1) printf(" (%s)", ErrnoName(Error));
  if (Expected) printf(", expected %s", ErrnoName(Expected));
  putchar('\n');
  ++CaseFailures;
}

// Expression must be true.
#define CHECK(expr) do { errno = 0; if (!(expr)) Fail(__LINE__, #expr, 0, errno, 0); } while (0)
// System call must fail with the given errno.
#define CHECK_ERRNO(call, error) do {                                                        \
    errno = 0; long Result_ = (long)(call);                                                  \
    if (Result_ != -1 || errno != (error)) Fail(__LINE__, #call, Result_, errno, error);      \
  } while (0)
#define SKIP(reason) do { SkipTest(reason); return; } while (0)

static __attribute__((unused)) void SkipTest(const char *Reason) {
  snprintf(Shared->SkipReason, sizeof(Shared->SkipReason), "%s", Reason);
  Skipped = 1;
}

// Forks a helper that runs Child and exits 0, or 1 if its CHECKs failed.
// Returns the pid in the parent, -1 on failure.
static __attribute__((unused)) pid_t Fork(void (*Child)(void)) {
  fflush(stdout);
  const pid_t Pid = fork();
  if (Pid != 0) return Pid;
  CaseFailures = 0;
  Skipped = 0;
  Child();
  fflush(stdout);
  _exit(CaseFailures ? 1 : Skipped ? SKIP_EXIT : 0);
}

// Waits for Pid and returns the raw wait status, or -1 on error.
static __attribute__((unused)) int WaitExit(pid_t Pid) {
  int Status = 0;
  pid_t Result;
  do Result = waitpid(Pid, &Status, 0); while (Result < 0 && errno == EINTR);
  return Result == Pid ? Status : -1;
}

// Returns an absolute CLOCK_MONOTONIC deadline Milliseconds from now.
static uint64_t NowMs(void) {
  struct timespec Now;
  clock_gettime(CLOCK_MONOTONIC, &Now);
  return (uint64_t) Now.tv_sec * 1000 + Now.tv_nsec / 1000000;
}
static __attribute__((unused)) uint64_t Deadline(unsigned Milliseconds) { return NowMs() + Milliseconds; }
static __attribute__((unused)) int DeadlineExpired(uint64_t Limit) { return NowMs() >= Limit; }

static int Selected(const char *Name, int Count, char **Patterns) {
  int Included = 1;
  for (int Index = 0; Index < Count; ++Index) {
    if (Patterns[Index][0] != '!') Included = 0;
  }
  for (int Index = 0; Index < Count; ++Index) {
    const char *Pattern = Patterns[Index];
    const int Exclude = Pattern[0] == '!';
    if (Exclude) ++Pattern;
    if (strncmp(Name, Pattern, strlen(Pattern)) == 0) {
      if (Exclude) return 0;
      Included = 1;
    }
  }
  return Included;
}

static int CompareTests(const void *Left, const void *Right) {
  return strcmp(((const struct Test *) Left)->Name, ((const struct Test *) Right)->Name);
}

static void ResetSignalState(void) {
  for (int Signal = 1; Signal <= 64; ++Signal) {
    if (Signal == SIGKILL || Signal == SIGSTOP) continue;
    struct sigaction Action = {.sa_handler = SIG_DFL};
    sigemptyset(&Action.sa_mask);
    if (sigaction(Signal, &Action, NULL) < 0 && errno != EINVAL) {
      printf("# cannot reset signal %d: %s\n", Signal, ErrnoName(errno));
    }
  }
  sigset_t Empty;
  sigemptyset(&Empty);
  sigprocmask(SIG_SETMASK, &Empty, NULL);
}

enum Outcome { Pass, FailOutcome, SkipOutcome, Timeout, Abnormal };

// Runs one test in an isolated child and waits up to the timeout.
static enum Outcome RunIsolated(const struct Test *Test, int *Status) {
  fflush(stdout);
  Shared->SkipReason[0] = 0;
  sigset_t ChildSet, OldMask;
  sigemptyset(&ChildSet);
  sigaddset(&ChildSet, SIGCHLD);
  sigprocmask(SIG_BLOCK, &ChildSet, &OldMask);
  const pid_t Child = fork();
  if (Child < 0) {
    sigprocmask(SIG_SETMASK, &OldMask, NULL);
    return Abnormal;
  }
  if (Child == 0) {
    sigprocmask(SIG_SETMASK, &OldMask, NULL);
    setpgid(0, 0);
    CaseFailures = 0;
    Skipped = 0;
    umask(022);
    ResetSignalState();
    Test->Function();
    fflush(stdout);
    _exit(CaseFailures ? 1 : Skipped ? SKIP_EXIT : 0);
  }
  setpgid(Child, Child);  // Also set from the parent to avoid a race with an early kill.
  const uint64_t Limit = Deadline(TEST_TIMEOUT_SECONDS * 1000);
  pid_t Result;
  int TimedOut = 0;
  // Sleeps until a child changes state (SIGCHLD) rather than polling: the runner shares its forked memory with the test,
  // and every wake of the runner would have to bring its own memory back in.
  for (;;) {
    Result = waitpid(Child, Status, WNOHANG);
    if (Result != 0 && !(Result < 0 && errno == EINTR)) break;
    if (DeadlineExpired(Limit)) {
      TimedOut = 1;
      break;
    }
    const struct timespec Slice = {.tv_sec = 1};
    sigtimedwait(&ChildSet, NULL, &Slice);
  }
  sigprocmask(SIG_SETMASK, &OldMask, NULL);
  if (TimedOut) {
    kill(-Child, SIGKILL);
    kill(Child, SIGKILL);
    waitpid(Child, Status, 0);
    return Timeout;
  }
  kill(-Child, SIGKILL);  // Reap nothing: just stop leftovers from this test.
  if (Result != Child || !WIFEXITED(*Status)) return Abnormal;
  switch (WEXITSTATUS(*Status)) {
  case 0: return Pass;
  case SKIP_EXIT: return SkipOutcome;
  default: return FailOutcome;
  }
}

// ---------------------------------------------------------------------------
// Helpers

static long Processors(void) {
  cpu_set_t Set;
  if (sched_getaffinity(0, sizeof(Set), &Set) == 0) return CPU_COUNT(&Set);
  return sysconf(_SC_NPROCESSORS_ONLN);
}

static uint64_t NowNs(void) {
  struct timespec Now;
  clock_gettime(CLOCK_MONOTONIC, &Now);
  return (uint64_t) Now.tv_sec * 1000000000ULL + Now.tv_nsec;
}

static long Futex(uint32_t *Word, int Op, uint32_t Value, const struct timespec *Timeout) {
  return syscall(SYS_futex, Word, Op, Value, Timeout, NULL, 0);
}

// A fixed amount of integer work the compiler cannot fold away.
static uint64_t Work(uint64_t Steps, uint64_t Seed) {
  uint64_t State = Seed;
  for (uint64_t Step = 0; Step < Steps; ++Step) {
    State = State * 6364136223846793005ULL + 1442695040888963407ULL;
    __asm__ volatile("" : "+r"(State));
  }
  return State;
}

struct WorkArgs {
  uint64_t Steps, Seed, Result;
};

static void *WorkThread(void *Raw) {
  struct WorkArgs *Args = Raw;
  Args->Result = Work(Args->Steps, Args->Seed);
  return NULL;
}

// Steps of Work that take about Milliseconds on this machine (at least 1).
static uint64_t CalibrateSteps(unsigned Milliseconds) {
  uint64_t Steps = 1 << 16;
  for (;;) {
    const uint64_t Start = NowNs();
    Work(Steps, 1);
    const uint64_t Elapsed = NowNs() - Start;
    if (Elapsed >= 20 * 1000000ULL || Steps >= (1ULL << 40)) return Steps * (Milliseconds * 1000000ULL) / (Elapsed ? Elapsed : 1) + 1;
    Steps *= 4;
  }
}

static volatile sig_atomic_t SignalSeen;
static void NoteSignal(int Signal) {
  (void) Signal;
  SignalSeen = 1;
}

// SIGUSR1 handler without SA_RESTART, so blocked calls end with EINTR.
static void CatchUsr1(void) {
  struct sigaction Action = {.sa_handler = NoteSignal};
  sigemptyset(&Action.sa_mask);
  sigaction(SIGUSR1, &Action, NULL);
  SignalSeen = 0;
}

// Sends SIGUSR1 to Thread until Done becomes true (the thread may not have blocked yet at the first one).
static int KickUntil(pthread_t Thread, volatile int *Done, unsigned Milliseconds) {
  const uint64_t Limit = Deadline(Milliseconds);
  while (!*Done && !DeadlineExpired(Limit)) {
    pthread_kill(Thread, SIGUSR1);
    nanosleep(&(struct timespec) {.tv_nsec = 20 * 1000000}, NULL);
  }
  return *Done;
}

// ---------------------------------------------------------------------------
// Parallel execution

TEST(thread, parallel_compute_scales) {
  const long Cpus = Processors();
  printf("# %ld processors\n", Cpus);
  if (Cpus < 2) SKIP("one processor");
  const int Count = Cpus >= 4 ? 4 : (int) Cpus;
  const uint64_t Steps = CalibrateSteps(300);
  uint64_t Start = NowNs();
  const uint64_t Expected = Work(Steps, 7);
  const uint64_t One = NowNs() - Start;
  pthread_t Threads[4];
  struct WorkArgs Args[4];
  Start = NowNs();
  for (int Index = 0; Index < Count; ++Index) {
    Args[Index] = (struct WorkArgs) {Steps, 7, 0};
    CHECK(pthread_create(&Threads[Index], NULL, WorkThread, &Args[Index]) == 0);
  }
  for (int Index = 0; Index < Count; ++Index) pthread_join(Threads[Index], NULL);
  const uint64_t All = NowNs() - Start;
  for (int Index = 0; Index < Count; ++Index) CHECK(Args[Index].Result == Expected);
  printf("# one thread %llu ms, %d threads %llu ms\n", (unsigned long long) (One / 1000000), Count,
         (unsigned long long) (All / 1000000));
  // Serial execution takes Count times as long; parallel about as long as one.
  CHECK(All * 10 < One * Count * 6);
}

// The same scaling check for three kinds of work: memory that misses the caches (a random walk through 64 MiB
// per thread), memory that stays in the cache (16 KiB per thread) and indirect calls (the same code in every thread).
enum Kernel { KernelChase, KernelCache, KernelCalls };
struct KernelArgs {
  enum Kernel Kind;
  uint64_t Steps, Result;
  uint32_t *Memory;
  uint64_t WallNs, CpuNs; // this thread's run: CPU time well below wall time means it waited for a core
};
#define CHASE_WORDS (16u << 20)
#define CACHE_WORDS 4096u

__attribute__((noinline)) static uint64_t CallAdd(uint64_t Value) { return Value + 0x9e3779b97f4a7c15ULL; }
__attribute__((noinline)) static uint64_t CallXor(uint64_t Value) { return Value ^ (Value >> 7); }
__attribute__((noinline)) static uint64_t CallMul(uint64_t Value) { return Value * 0xff51afd7ed558ccdULL; }
static uint64_t (*volatile CallTable[3])(uint64_t) = {CallAdd, CallXor, CallMul};

static uint64_t RunKernel(struct KernelArgs *Args) {
  uint64_t State = 1;
  switch (Args->Kind) {
  case KernelChase: {
    uint32_t Index = 0;
    for (uint64_t Step = 0; Step < Args->Steps; ++Step) Index = Args->Memory[Index];
    State = Index;
    break;
  }
  case KernelCache:
    for (uint64_t Step = 0; Step < Args->Steps; ++Step) {
      uint32_t *Word = &Args->Memory[(Step * 17) % CACHE_WORDS];
      *Word = *Word * 3 + (uint32_t) Step;
    }
    for (uint32_t Index = 0; Index < CACHE_WORDS; ++Index) State += Args->Memory[Index];
    break;
  case KernelCalls:
    for (uint64_t Step = 0; Step < Args->Steps; ++Step) State = CallTable[Step % 3](State);
    break;
  }
  return State;
}

// A buffer for one thread: a single random cycle through all words (Sattolo) for the walk, zeros for the cache loop.
// Later walk buffers copy the first.
static uint32_t *KernelMemory(enum Kernel Kind, const uint32_t *First) {
  const size_t Words = Kind == KernelChase ? CHASE_WORDS : CACHE_WORDS;
  uint32_t *Memory = malloc(Words * sizeof(uint32_t));
  if (!Memory) return NULL;
  if (First && Kind == KernelChase) return memcpy(Memory, First, Words * sizeof(uint32_t));
  for (uint32_t Index = 0; Index < Words; ++Index) Memory[Index] = Kind == KernelChase ? Index : 0;
  if (Kind == KernelChase) {
    uint64_t Random = 88172645463325252ULL;
    for (uint32_t Index = Words - 1; Index > 0; --Index) {
      Random ^= Random << 13, Random ^= Random >> 7, Random ^= Random << 17;
      const uint32_t Other = (uint32_t) (Random % Index);
      const uint32_t Swap = Memory[Index];
      Memory[Index] = Memory[Other];
      Memory[Other] = Swap;
    }
  }
  return Memory;
}

static uint64_t ThreadCpuNs(void) {
  struct timespec Time;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &Time);
  return (uint64_t) Time.tv_sec * 1000000000ULL + (uint64_t) Time.tv_nsec;
}

static void *KernelThread(void *Raw) {
  struct KernelArgs *Args = Raw;
  const uint64_t Wall = NowNs(), Cpu = ThreadCpuNs();
  Args->Result = RunKernel(Args);
  Args->WallNs = NowNs() - Wall;
  Args->CpuNs = ThreadCpuNs() - Cpu;
  return NULL;
}

static void KernelScales(enum Kernel Kind, const char *Name) {
  const long Cpus = Processors();
  if (Cpus < 2) SKIP("one processor");
  const int Count = Cpus >= 4 ? 4 : (int) Cpus;
  struct KernelArgs Args[4];
  pthread_t Threads[4];
  for (int Index = 0; Index < Count; ++Index) {
    Args[Index] = (struct KernelArgs) {Kind, 1 << 16, 0, KernelMemory(Kind, Index ? Args[0].Memory : NULL), 0, 0};
    CHECK(Args[Index].Memory != NULL);
  }
  // About 300 ms alone.
  for (;;) {
    const uint64_t Start = NowNs();
    RunKernel(&Args[0]);
    const uint64_t Elapsed = NowNs() - Start;
    if (Elapsed >= 20 * 1000000ULL) {
      Args[0].Steps = Args[0].Steps * 300000000ULL / Elapsed + 1;
      break;
    }
    Args[0].Steps *= 4;
  }
  if (Kind == KernelCache) memset(Args[0].Memory, 0, CACHE_WORDS * sizeof(uint32_t));
  uint64_t Start = NowNs();
  const uint64_t Expected = RunKernel(&Args[0]);
  const uint64_t One = NowNs() - Start;
  if (Kind == KernelCache) memset(Args[0].Memory, 0, CACHE_WORDS * sizeof(uint32_t));
  for (int Index = 1; Index < Count; ++Index) Args[Index].Steps = Args[0].Steps;
  Start = NowNs();
  for (int Index = 0; Index < Count; ++Index) CHECK(pthread_create(&Threads[Index], NULL, KernelThread, &Args[Index]) == 0);
  for (int Index = 0; Index < Count; ++Index) pthread_join(Threads[Index], NULL);
  const uint64_t All = NowNs() - Start;
  printf("# %s: one thread %llu ms, %d threads %llu ms, %llu steps (%llu ps each)\n", Name, (unsigned long long) (One / 1000000),
         Count, (unsigned long long) (All / 1000000), (unsigned long long) Args[0].Steps,
         (unsigned long long) (One * 1000 / Args[0].Steps));
  for (int Index = 0; Index < Count; ++Index) {
    printf("#   thread %d: wall %llu ms, cpu %llu ms, buffer %p\n", Index, (unsigned long long) (Args[Index].WallNs / 1000000),
           (unsigned long long) (Args[Index].CpuNs / 1000000), (void *) Args[Index].Memory);
  }
  for (int Index = 0; Index < Count; ++Index) {
    if (Kind != KernelChase) CHECK(Args[Index].Result == Expected);
    free(Args[Index].Memory);
  }
  // Each thread has its own data: Count threads take about as long as one, not Count times as long.
  CHECK(All * 10 < One * Count * 6);
}

TEST(thread, parallel_memory_walks_scale) { KernelScales(KernelChase, "random walk, 64 MiB each"); }
TEST(thread, parallel_cached_memory_scales) { KernelScales(KernelCache, "16 KiB each"); }
TEST(thread, parallel_indirect_calls_scale) { KernelScales(KernelCalls, "indirect calls"); }

static uint32_t PingPongWord;
static void *PingPongPartner(void *Raw) {
  const unsigned Rounds = *(unsigned *) Raw;
  for (unsigned Round = 0; Round < Rounds; ++Round) {
    while (__atomic_load_n(&PingPongWord, __ATOMIC_ACQUIRE) != 1) Futex(&PingPongWord, FUTEX_WAIT_PRIVATE, 0, NULL);
    __atomic_store_n(&PingPongWord, 0, __ATOMIC_RELEASE);
    Futex(&PingPongWord, FUTEX_WAKE_PRIVATE, 1, NULL);
  }
  return NULL;
}

TEST(thread, futex_ping_pong) {
  unsigned Rounds = 5000;
  pthread_t Partner;
  PingPongWord = 0;
  CHECK(pthread_create(&Partner, NULL, PingPongPartner, &Rounds) == 0);
  const uint64_t Start = NowNs();
  for (unsigned Round = 0; Round < Rounds; ++Round) {
    __atomic_store_n(&PingPongWord, 1, __ATOMIC_RELEASE);
    Futex(&PingPongWord, FUTEX_WAKE_PRIVATE, 1, NULL);
    while (__atomic_load_n(&PingPongWord, __ATOMIC_ACQUIRE) != 0) Futex(&PingPongWord, FUTEX_WAIT_PRIVATE, 1, NULL);
  }
  CHECK(pthread_join(Partner, NULL) == 0);
  printf("# %u round trips, %llu us each\n", Rounds, (unsigned long long) ((NowNs() - Start) / 1000 / Rounds));
}

static pthread_mutex_t CounterLock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t LockedCounter, AtomicCounter;
static void *CountThread(void *Raw) {
  const unsigned Times = *(unsigned *) Raw;
  for (unsigned Index = 0; Index < Times; ++Index) {
    pthread_mutex_lock(&CounterLock);
    ++LockedCounter;
    pthread_mutex_unlock(&CounterLock);
    __atomic_fetch_add(&AtomicCounter, 1, __ATOMIC_RELAXED);
  }
  return NULL;
}

TEST(thread, mutex_and_atomic_counters) {
  unsigned Times = 50000;
  pthread_t Threads[4];
  LockedCounter = AtomicCounter = 0;
  for (int Index = 0; Index < 4; ++Index) CHECK(pthread_create(&Threads[Index], NULL, CountThread, &Times) == 0);
  for (int Index = 0; Index < 4; ++Index) pthread_join(Threads[Index], NULL);
  CHECK(LockedCounter == 4ULL * Times);
  CHECK(AtomicCounter == 4ULL * Times);
}

// x86 keeps stores in order and loads in order (TSO): a reader that sees the flag sees the data written before it.
// On an ARM host this holds only with FEX's TSO emulation.
static struct {
  volatile uint64_t Data;
  char Pad[56];
  volatile uint64_t Flag;
} Message __attribute__((aligned(64)));

static void *MessageWriter(void *Raw) {
  const uint64_t Count = *(uint64_t *) Raw;
  for (uint64_t Value = 1; Value <= Count; ++Value) {
    Message.Data = Value;
    __asm__ volatile("" ::: "memory");
    Message.Flag = Value;
  }
  return NULL;
}

TEST(thread, message_passing_keeps_x86_order) {
  if (Processors() < 2) SKIP("one processor");
  uint64_t Count = 1000000, Reordered = 0, Flag = 0;
  Message.Data = Message.Flag = 0;
  pthread_t Writer;
  CHECK(pthread_create(&Writer, NULL, MessageWriter, &Count) == 0);
  const uint64_t Limit = Deadline(8000);
  while (Flag != Count && !DeadlineExpired(Limit)) {
    Flag = Message.Flag;
    __asm__ volatile("" ::: "memory");
    const uint64_t Data = Message.Data;
    if (Data < Flag) ++Reordered;
  }
  pthread_join(Writer, NULL);
  printf("# %llu reorderings\n", (unsigned long long) Reordered);
  CHECK(Flag == Count);
  CHECK(Reordered == 0);
}

static pthread_mutex_t QueueLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t QueueReady = PTHREAD_COND_INITIALIZER;
static int QueueItems, QueueDone, QueueTaken;
static void *QueueConsumer(void *Raw) {
  (void) Raw;
  pthread_mutex_lock(&QueueLock);
  for (;;) {
    while (!QueueItems && !QueueDone) pthread_cond_wait(&QueueReady, &QueueLock);
    if (!QueueItems && QueueDone) break;
    --QueueItems;
    ++QueueTaken;
  }
  pthread_mutex_unlock(&QueueLock);
  return NULL;
}

TEST(thread, condvar_producer_consumers) {
  pthread_t Consumers[8];
  QueueItems = QueueDone = QueueTaken = 0;
  for (int Index = 0; Index < 8; ++Index) CHECK(pthread_create(&Consumers[Index], NULL, QueueConsumer, NULL) == 0);
  for (int Item = 0; Item < 20000; ++Item) {
    pthread_mutex_lock(&QueueLock);
    ++QueueItems;
    pthread_cond_signal(&QueueReady);
    pthread_mutex_unlock(&QueueLock);
  }
  pthread_mutex_lock(&QueueLock);
  QueueDone = 1;
  pthread_cond_broadcast(&QueueReady);
  pthread_mutex_unlock(&QueueLock);
  for (int Index = 0; Index < 8; ++Index) pthread_join(Consumers[Index], NULL);
  CHECK(QueueTaken == 20000);
}

static void *Sleeper(void *Raw) {
  (void) Raw;
  nanosleep(&(struct timespec) {.tv_nsec = 300 * 1000000}, NULL);
  return NULL;
}

TEST(thread, sleeps_overlap) {
  pthread_t Threads[6];
  const uint64_t Start = NowNs();
  for (int Index = 0; Index < 6; ++Index) CHECK(pthread_create(&Threads[Index], NULL, Sleeper, NULL) == 0);
  for (int Index = 0; Index < 6; ++Index) pthread_join(Threads[Index], NULL);
  const uint64_t Elapsed = (NowNs() - Start) / 1000000;
  printf("# six 300 ms sleeps took %llu ms\n", (unsigned long long) Elapsed);
  CHECK(Elapsed >= 300 && Elapsed < 900);
}

TEST(thread, create_and_join_many) {
  // Batches of 16 live threads, 160 in all: slots and host threads are reused.
  for (int Batch = 0; Batch < 10; ++Batch) {
    pthread_t Threads[16];
    struct WorkArgs Args[16];
    for (int Index = 0; Index < 16; ++Index) {
      Args[Index] = (struct WorkArgs) {1000, (uint64_t) Index, 0};
      CHECK(pthread_create(&Threads[Index], NULL, WorkThread, &Args[Index]) == 0);
    }
    for (int Index = 0; Index < 16; ++Index) {
      CHECK(pthread_join(Threads[Index], NULL) == 0);
      CHECK(Args[Index].Result == Work(1000, (uint64_t) Index));
    }
  }
}

static volatile int YieldStop;
static void *Yielder(void *Raw) {
  uint64_t *Count = Raw;
  while (!YieldStop) {
    ++*Count;
    sched_yield();
  }
  return NULL;
}

TEST(thread, yielding_threads_all_progress) {
  uint64_t Counts[3] = {0, 0, 0};
  pthread_t Threads[3];
  YieldStop = 0;
  for (int Index = 0; Index < 3; ++Index) CHECK(pthread_create(&Threads[Index], NULL, Yielder, &Counts[Index]) == 0);
  nanosleep(&(struct timespec) {.tv_nsec = 300 * 1000000}, NULL);
  YieldStop = 1;
  for (int Index = 0; Index < 3; ++Index) pthread_join(Threads[Index], NULL);
  for (int Index = 0; Index < 3; ++Index) CHECK(Counts[Index] > 0);
}

struct PipeArgs {
  int Fd;
  unsigned Id;
};
static void *PipeLineWriter(void *Raw) {
  const struct PipeArgs *Args = Raw;
  char Line[16];
  for (unsigned Index = 0; Index < 2000; ++Index) {
    snprintf(Line, sizeof(Line), "%u:%07u\n", Args->Id, Index);
    if (write(Args->Fd, Line, 10) != 10) break;
  }
  return NULL;
}

TEST(thread, concurrent_pipe_writes_stay_whole) {
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  pthread_t Threads[4];
  struct PipeArgs Args[4];
  for (unsigned Index = 0; Index < 4; ++Index) {
    Args[Index] = (struct PipeArgs) {Pipe[1], Index};
    CHECK(pthread_create(&Threads[Index], NULL, PipeLineWriter, &Args[Index]) == 0);
  }
  unsigned Next[4] = {0, 0, 0, 0}, Lines = 0, Broken = 0;
  char Line[10];
  size_t Have = 0;
  while (Lines < 8000) {
    const ssize_t Count = read(Pipe[0], Line + Have, sizeof(Line) - Have);
    if (Count <= 0) break;
    Have += Count;
    if (Have < sizeof(Line)) continue;
    Have = 0;
    // "I:NNNNNNN\n" (parsed by hand: the Debian 12 glibc the guest runs lacks the host's C23 sscanf).
    const unsigned Id = (unsigned) (Line[0] - '0');
    unsigned Index = 0;
    for (int Digit = 2; Digit < 9; ++Digit) Index = Index * 10 + (unsigned) (Line[Digit] - '0');
    // PIPE_BUF writes are atomic: every line arrives whole and each writer's lines in order.
    if (Id > 3 || Line[1] != ':' || Line[9] != '\n' || Index != Next[Id]) ++Broken;
    else ++Next[Id];
    ++Lines;
  }
  for (int Index = 0; Index < 4; ++Index) pthread_join(Threads[Index], NULL);
  CHECK(Lines == 8000);
  CHECK(Broken == 0);
  close(Pipe[0]);
  close(Pipe[1]);
}

// ---------------------------------------------------------------------------
// Signals to threads

static volatile int BlockedDone, BlockedErrno;
static uint32_t BlockWord;
static int BlockPipe[2];

static void *BlockInFutex(void *Raw) {
  (void) Raw;
  const long Result = Futex(&BlockWord, FUTEX_WAIT_PRIVATE, 0, NULL);
  BlockedErrno = Result < 0 ? errno : 0;
  BlockedDone = 1;
  return NULL;
}
static void *BlockInRead(void *Raw) {
  (void) Raw;
  char Byte;
  const ssize_t Result = read(BlockPipe[0], &Byte, 1);
  BlockedErrno = Result < 0 ? errno : 0;
  BlockedDone = 1;
  return NULL;
}
static void *BlockInSleep(void *Raw) {
  (void) Raw;
  const int Result = nanosleep(&(struct timespec) {.tv_sec = 30}, NULL);
  BlockedErrno = Result < 0 ? errno : 0;
  BlockedDone = 1;
  return NULL;
}
static void *BlockInPoll(void *Raw) {
  (void) Raw;
  struct pollfd Entry = {.fd = BlockPipe[0], .events = POLLIN};
  const int Result = poll(&Entry, 1, 30000);
  BlockedErrno = Result < 0 ? errno : 0;
  BlockedDone = 1;
  return NULL;
}

static void InterruptBlocked(void *(*Body)(void *)) {
  CatchUsr1();
  BlockedDone = BlockedErrno = 0;
  BlockWord = 0;
  CHECK(pipe(BlockPipe) == 0);
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, Body, NULL) == 0);
  nanosleep(&(struct timespec) {.tv_nsec = 100 * 1000000}, NULL);
  CHECK(KickUntil(Thread, &BlockedDone, 3000));
  pthread_join(Thread, NULL);
  CHECK(BlockedErrno == EINTR);
  CHECK(SignalSeen);
}

TEST(thread, signal_interrupts_futex_wait) { InterruptBlocked(BlockInFutex); }
TEST(thread, signal_interrupts_read) { InterruptBlocked(BlockInRead); }
TEST(thread, signal_interrupts_nanosleep) { InterruptBlocked(BlockInSleep); }
TEST(thread, signal_interrupts_poll) { InterruptBlocked(BlockInPoll); }

static void *SpinUntilSignal(void *Raw) {
  (void) Raw;
  while (!SignalSeen) __asm__ volatile("" ::: "memory");
  BlockedDone = 1;
  return NULL;
}

TEST(thread, signal_reaches_running_thread) {
  // The thread never makes a syscall: the signal must stop it in guest code.
  CatchUsr1();
  BlockedDone = 0;
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, SpinUntilSignal, NULL) == 0);
  nanosleep(&(struct timespec) {.tv_nsec = 50 * 1000000}, NULL);
  CHECK(pthread_kill(Thread, SIGUSR1) == 0);
  const uint64_t Limit = Deadline(3000);
  while (!BlockedDone && !DeadlineExpired(Limit)) nanosleep(&(struct timespec) {.tv_nsec = 10 * 1000000}, NULL);
  CHECK(BlockedDone);
  if (BlockedDone) pthread_join(Thread, NULL);
}

static void *SpinForever(void *Raw) {
  (void) Raw;
  for (;;) __asm__ volatile("" ::: "memory");
  return NULL;
}
static void *WaitForever(void *Raw) {
  (void) Raw;
  uint32_t Word = 0;
  for (;;) Futex(&Word, FUTEX_WAIT_PRIVATE, 0, NULL);
  return NULL;
}

static void ExitWithThreads(void) {
  pthread_t Spinner, Waiter;
  if (pthread_create(&Spinner, NULL, SpinForever, NULL) || pthread_create(&Waiter, NULL, WaitForever, NULL)) _exit(2);
  nanosleep(&(struct timespec) {.tv_nsec = 50 * 1000000}, NULL);
  exit(7);
}

TEST(thread, exit_group_ends_running_and_blocked_threads) {
  const uint64_t Start = NowMs();
  const int Status = WaitExit(Fork(ExitWithThreads));
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 7);
  CHECK(NowMs() - Start < 3000);
}

static void *ExitFromThread(void *Raw) {
  (void) Raw;
  nanosleep(&(struct timespec) {.tv_nsec = 50 * 1000000}, NULL);
  syscall(SYS_exit_group, 5);
  return NULL;
}
static void ThreadExitsProcess(void) {
  pthread_t Spinner, Exiter;
  if (pthread_create(&Spinner, NULL, SpinForever, NULL) || pthread_create(&Exiter, NULL, ExitFromThread, NULL)) _exit(2);
  pause();
  _exit(3);
}

TEST(thread, exit_group_from_other_thread) {
  const int Status = WaitExit(Fork(ThreadExitsProcess));
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 5);
}

static void KilledWithThreads(void) {
  pthread_t Spinner, Waiter;
  if (pthread_create(&Spinner, NULL, SpinForever, NULL) || pthread_create(&Waiter, NULL, WaitForever, NULL)) _exit(2);
  pause();
  _exit(3);
}

TEST(thread, fatal_signal_ends_all_threads) {
  const pid_t Child = Fork(KilledWithThreads);
  nanosleep(&(struct timespec) {.tv_nsec = 100 * 1000000}, NULL);
  CHECK(kill(Child, SIGTERM) == 0);
  const int Status = WaitExit(Child);
  CHECK(WIFSIGNALED(Status) && WTERMSIG(Status) == SIGTERM);
}

// ---------------------------------------------------------------------------
// fork and exec from threaded processes

static void ForkWhileSiblingRuns(void) {
  pthread_t Spinner;
  if (pthread_create(&Spinner, NULL, SpinForever, NULL)) _exit(2);
  nanosleep(&(struct timespec) {.tv_nsec = 20 * 1000000}, NULL);
  const pid_t Child = fork();
  if (Child == 0) _exit(Work(100000, 3) == Work(100000, 3) ? 11 : 12);  // only the forking thread lives on
  const int Status = WaitExit(Child);
  _exit(WIFEXITED(Status) && WEXITSTATUS(Status) == 11 ? 0 : 1);
}

TEST(thread, fork_from_threaded_process) {
  const int Status = WaitExit(Fork(ForkWhileSiblingRuns));
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
}

static void ExecWhileSiblingRuns(void) {
  pthread_t Spinner, Waiter;
  if (pthread_create(&Spinner, NULL, SpinForever, NULL) || pthread_create(&Waiter, NULL, WaitForever, NULL)) _exit(2);
  nanosleep(&(struct timespec) {.tv_nsec = 20 * 1000000}, NULL);
  execl("/bin/sh", "sh", "-c", "exit 9", (char *) NULL);
  _exit(3);
}

TEST(thread, execve_from_threaded_process) {
  const int Status = WaitExit(Fork(ExecWhileSiblingRuns));
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 9);
}

static void PingPongAfterFork(void) {
  // Parent and child share their memory layout until exec: they alternate through a pipe pair 500 times.
  int Down[2], Up[2];
  if (pipe(Down) || pipe(Up)) _exit(2);
  uint64_t Local = 1;
  const pid_t Child = fork();
  if (Child == 0) {
    for (int Round = 0; Round < 500; ++Round) {
      uint64_t Value;
      if (read(Down[0], &Value, sizeof(Value)) != sizeof(Value)) _exit(3);
      Local += Value;  // each process keeps its own copy of Local
      if (write(Up[1], &Local, sizeof(Local)) != sizeof(Local)) _exit(4);
    }
    _exit(0);
  }
  uint64_t Sum = 0;
  for (int Round = 0; Round < 500; ++Round) {
    if (write(Down[1], &Local, sizeof(Local)) != sizeof(Local)) _exit(5);
    uint64_t Reply;
    if (read(Up[0], &Reply, sizeof(Reply)) != sizeof(Reply)) _exit(6);
    Sum = Reply;
  }
  const int Status = WaitExit(Child);
  // The child's Local grew by 1 per round; the parent's stayed 1.
  _exit(Local == 1 && Sum == 501 && WIFEXITED(Status) && WEXITSTATUS(Status) == 0 ? 0 : 1);
}

TEST(thread, forked_processes_alternate) {
  const int Status = WaitExit(Fork(PingPongAfterFork));
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
}

static void *SpinCpu(void *Raw) {
  struct WorkArgs *Args = Raw;
  Args->Result = Work(Args->Steps, 5);
  struct timespec Used;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &Used);
  Args->Seed = (uint64_t) Used.tv_sec * 1000 + Used.tv_nsec / 1000000;
  return NULL;
}

TEST(thread, cpu_clocks_count_each_thread) {
  const uint64_t Steps = CalibrateSteps(200);
  struct WorkArgs Args[2] = {{Steps, 0, 0}, {Steps, 0, 0}};
  pthread_t Threads[2];
  struct timespec Before, After;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &Before);
  for (int Index = 0; Index < 2; ++Index) CHECK(pthread_create(&Threads[Index], NULL, SpinCpu, &Args[Index]) == 0);
  for (int Index = 0; Index < 2; ++Index) pthread_join(Threads[Index], NULL);
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &After);
  const uint64_t Process = (After.tv_sec - Before.tv_sec) * 1000 + (After.tv_nsec - Before.tv_nsec) / 1000000;
  printf("# thread cpu %llu ms and %llu ms, process %llu ms\n", (unsigned long long) Args[0].Seed,
         (unsigned long long) Args[1].Seed, (unsigned long long) Process);
  CHECK(Args[0].Seed >= 100 && Args[1].Seed >= 100);
  CHECK(Process + 20 >= Args[0].Seed + Args[1].Seed);
}

int main(int argc, char **argv) {
  int List = 0, Count = 0;
  const char *Location = ".";  // -d dir: where the scratch folder is made
  char **Patterns = calloc(argc, sizeof(char *));
  for (int Index = 1; Index < argc; ++Index) {
    if (!strcmp(argv[Index], "-l")) List = 1;
    else if (!strcmp(argv[Index], "-d") && Index + 1 < argc) Location = argv[++Index];
    else Patterns[Count++] = argv[Index];
  }
  // Registration order is unspecified; sort so numbering is stable.
  qsort(Tests, TestCount, sizeof(Tests[0]), CompareTests);
  if (List) {
    for (unsigned Index = 0; Index < TestCount; ++Index) puts(Tests[Index].Name);
    return 0;
  }
  setvbuf(stdout, NULL, _IOLBF, 0);
  Shared = mmap(NULL, sizeof(*Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (Shared == MAP_FAILED) {
    printf("Bail out! cannot map shared memory: %s\n", ErrnoName(errno));
    return 1;
  }
  ResetSignalState();
  // Tests create files by fixed names; a private directory per run keeps a
  // root run and a user run in the same directory apart.
  char Scratch[PATH_MAX], Absolute[PATH_MAX];
  snprintf(Scratch, sizeof(Scratch), "%s/thread-suite.XXXXXX", Location);
  if (!mkdtemp(Scratch) || !realpath(Scratch, Absolute) || chdir(Absolute)) {
    printf("Bail out! cannot create a scratch directory: %s\n", ErrnoName(errno));
    return 1;
  }
  memcpy(Scratch, Absolute, sizeof(Scratch));

  unsigned Selection = 0;
  for (unsigned Index = 0; Index < TestCount; ++Index) Selection += Selected(Tests[Index].Name, Count, Patterns);
  if (Count && !Selection) {
    printf("Bail out! no tests match the given patterns\n");
    return 1;
  }
  printf("TAP version 13\n1..%u\n# thread-suite uid %u, gid %u\n", Selection, getuid(), getgid());
  unsigned Number = 0, Passed = 0, Failed = 0, SkippedCount = 0;
  const char *FailedNames[sizeof(Tests) / sizeof(Tests[0])];
  for (unsigned Index = 0; Index < TestCount; ++Index) {
    if (!Selected(Tests[Index].Name, Count, Patterns)) continue;
    ++Number;
    int Status = 0;
    switch (RunIsolated(&Tests[Index], &Status)) {
    case Pass:
      printf("ok %u %s\n", Number, Tests[Index].Name);
      ++Passed;
      break;
    case SkipOutcome:
      printf("ok %u %s # SKIP %s\n", Number, Tests[Index].Name, Shared->SkipReason);
      ++SkippedCount;
      break;
    case Timeout:
      printf("not ok %u %s # timeout\n", Number, Tests[Index].Name);
      FailedNames[Failed++] = Tests[Index].Name;
      break;
    case Abnormal:
      if (WIFSIGNALED(Status)) printf("not ok %u %s # signal %s\n", Number, Tests[Index].Name, sigabbrev_np(WTERMSIG(Status)));
      else printf("not ok %u %s # test process error\n", Number, Tests[Index].Name);
      FailedNames[Failed++] = Tests[Index].Name;
      break;
    default:
      if (WEXITSTATUS(Status) != 1) printf("not ok %u %s # exit %d\n", Number, Tests[Index].Name, WEXITSTATUS(Status));
      else printf("not ok %u %s\n", Number, Tests[Index].Name);
      FailedNames[Failed++] = Tests[Index].Name;
      break;
    }
  }
  // Repeat the failures last so they survive a truncated console log.
  for (unsigned Index = 0; Index < Failed; ++Index) printf("# failed: %s\n", FailedNames[Index]);
  // Same result format in every suite: passed/failed/skipped, the failed names above, exit 42 when none failed.
  printf("# thread-suite summary: %u/%u/%u (passed/failed/skipped)\n", Passed, Failed, SkippedCount);
  // final-pass-suite collects the counts through this file.
  const char* ResultsPath = getenv("FEX_SUITE_RESULTS");
  // The runner creates the file; opening it without O_CREAT also works in a sticky /tmp (protected_regular).
  const int Results = ResultsPath ? open(ResultsPath, O_WRONLY | O_APPEND | O_CLOEXEC) : -1;
  if (Results >= 0) {
    dprintf(Results, "thread-suite %u %u %u\n", Passed, Failed, SkippedCount);
    close(Results);
  }
  char Remove[PATH_MAX + 16];
  snprintf(Remove, sizeof(Remove), "rm -rf %s", Scratch);
  if (chdir("..") == 0 && system(Remove) != 0) printf("# could not remove %s\n", Scratch);
  return Failed ? 1 : 42;
}

