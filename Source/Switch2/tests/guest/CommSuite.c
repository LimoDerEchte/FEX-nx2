// SPDX-License-Identifier: MIT
// Memory and communication compatibility suite for the Switch 2 guest
// runtime (milestone 3): virtual memory, futexes, threads, pipes, System V
// and POSIX IPC, Unix sockets and loopback TCP/UDP.
//
// The harness is the one of ProcSuite.c. Results use TAP-style lines so a
// console log can be compared against the native Linux reference run.
// Expectations follow Linux; tests that depend on privilege or features
// skip with a reason instead of passing silently.
//
// Every test runs in its own forked child of the harness. The child calls
// setpgid(0, 0), runs the test and exits with
//   0   pass
//   1   one or more CHECKs failed
//   77  skipped (the reason travels to the parent in shared memory)
// The parent waits at most 10 s. On expiry it sends SIGKILL to the child's
// process group and reports "not ok <name> # timeout". After a normal exit
// it also kills the group so stray helper processes do not leak.
//
// Guest-only expectations: the deliberate differences listed in
// CommunicationInventory.md apply only under the Switch 2 runtime, which
// reports "#1 FEX Switch 2" as its uname version. Those cases skip natively
// (or check the Linux behavior natively and the documented one in the
// guest), so the native reference run passes every case.
//
// Network cases use 127.0.0.1 only, so the suite needs no internet.
//
// Usage: comm-suite [pattern...] | comm-suite -l
//   pattern   run tests whose name starts with the pattern ("mem/", "inet/tcp")
//   !pattern  exclude tests whose name starts with the pattern
//   -l        list test names
// Exit status: 42 when every selected test passes or skips, 1 otherwise.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/futex.h>
#include <mqueue.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SKIP_EXIT 77
#define TEST_TIMEOUT_SECONDS 10
#define PAGE 4096UL

// ---------------------------------------------------------------------------
// Harness (copied from ProcSuite.c)

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
// System call must succeed (not return -1).
#define CHECK_OK(call) do {                                                                  \
    errno = 0; long Result_ = (long)(call);                                                  \
    if (Result_ == -1) Fail(__LINE__, #call, Result_, errno, 0);                             \
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

// Waits for a helper made by Fork and folds its result into this test.
static __attribute__((unused)) void Join(pid_t Pid, int Line) {
  const int Status = WaitExit(Pid);
  if (Status == -1 || !WIFEXITED(Status)) {
    printf("#   line %d: helper %d ended abnormally (status %#x)\n", Line, Pid, Status);
    ++CaseFailures;
  } else if (WEXITSTATUS(Status) == SKIP_EXIT) {
    Skipped = 1;
  } else if (WEXITSTATUS(Status) != 0) {
    printf("#   line %d: helper %d failed\n", Line, Pid);
    ++CaseFailures;
  }
}
#define JOIN(pid) Join(pid, __LINE__)

static uint64_t NowMs(void) {
  struct timespec Now;
  clock_gettime(CLOCK_MONOTONIC, &Now);
  return (uint64_t) Now.tv_sec * 1000 + Now.tv_nsec / 1000000;
}
static __attribute__((unused)) uint64_t Deadline(unsigned Milliseconds) { return NowMs() + Milliseconds; }
static __attribute__((unused)) int DeadlineExpired(uint64_t Limit) { return NowMs() >= Limit; }
static __attribute__((unused)) void SleepMs(unsigned Milliseconds) {
  struct timespec Time = {Milliseconds / 1000, (long)(Milliseconds % 1000) * 1000000};
  while (nanosleep(&Time, &Time) < 0 && errno == EINTR) {}
}
// Polls Condition every millisecond for up to three seconds.
#define WAIT_UNTIL(Condition) do {                                                           \
    const uint64_t Limit_ = Deadline(3000);                                                  \
    while (!(Condition) && !DeadlineExpired(Limit_)) SleepMs(1);                             \
  } while (0)

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
  const pid_t Child = fork();
  if (Child < 0) return Abnormal;
  if (Child == 0) {
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
  for (;;) {
    Result = waitpid(Child, Status, WNOHANG);
    if (Result != 0 && !(Result < 0 && errno == EINTR)) break;
    if (DeadlineExpired(Limit)) {
      TimedOut = 1;
      break;
    }
    nanosleep(&(struct timespec) {.tv_nsec = 10 * 1000000}, NULL);
  }
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

static int GuestRuntime;

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
  // The Switch 2 runtime's uname version (GuestRuntime.cpp UnameGuest).
  struct utsname Name;
  GuestRuntime = uname(&Name) == 0 && strstr(Name.version, "FEX Switch 2") != NULL;
  Shared = mmap(NULL, sizeof(*Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (Shared == MAP_FAILED) {
    printf("Bail out! cannot map shared memory: %s\n", ErrnoName(errno));
    return 1;
  }
  ResetSignalState();
  // Tests create files by fixed names; a private directory per run keeps a
  // root run and a user run in the same directory apart.
  char Scratch[PATH_MAX], Absolute[PATH_MAX];
  snprintf(Scratch, sizeof(Scratch), "%s/comm-suite.XXXXXX", Location);
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
  printf("TAP version 13\n1..%u\n# comm-suite uid %u, gid %u, %s\n", Selection, getuid(), getgid(),
         GuestRuntime ? "Switch 2 guest" : "native Linux");
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
  printf("# comm-suite summary: %u/%u/%u (passed/failed/skipped)\n", Passed, Failed, SkippedCount);
  // final-pass-suite collects the counts through this file.
  const char* ResultsPath = getenv("FEX_SUITE_RESULTS");
  // The runner creates the file; opening it without O_CREAT also works in a sticky /tmp (protected_regular).
  const int Results = ResultsPath ? open(ResultsPath, O_WRONLY | O_APPEND | O_CLOEXEC) : -1;
  if (Results >= 0) {
    dprintf(Results, "comm-suite %u %u %u\n", Passed, Failed, SkippedCount);
    close(Results);
  }
  char Remove[PATH_MAX + 16];
  snprintf(Remove, sizeof(Remove), "rm -rf %s", Scratch);
  if (chdir("..") == 0 && system(Remove) != 0) printf("# could not remove %s\n", Scratch);
  return Failed ? 1 : 42;
}

// ---------------------------------------------------------------------------
// Shared helpers

// Cases that check a deliberate difference of the Switch 2 runtime.
#define GUEST_ONLY() do { if (!GuestRuntime) SKIP("guest-only expectation (CommunicationInventory.md)"); } while (0)

// Runs Body as uid/gid 1000: directly when not root, otherwise in a child
// that drops every identity (root has capabilities that bypass limits).
static void AsUser(void (*Body)(void)) {
  if (geteuid() != 0) {
    Body();
    return;
  }
  fflush(stdout);
  const pid_t Pid = fork();
  if (Pid == 0) {
    CaseFailures = 0;
    Skipped = 0;
    if (setgroups(0, NULL) || setresgid(1000, 1000, 1000) || setresuid(1000, 1000, 1000)) {
      printf("#   cannot switch to uid 1000: %s\n", ErrnoName(errno));
      _exit(1);
    }
    Body();
    fflush(stdout);
    _exit(CaseFailures ? 1 : Skipped ? SKIP_EXIT : 0);
  }
  JOIN(Pid);
}

static __attribute__((unused)) int Mapped(void *Address, size_t Size) {
  // msync on an unmapped range is ENOMEM and never touches the pages.
  return msync(Address, Size, MS_ASYNC) == 0;
}

// glibc passes a stale fifth argument without MREMAP_FIXED, which Linux 7.3
// rejects for MREMAP_DONTUNMAP; call the system call with an explicit zero.
// fscanf without the C23 redirect that Debian 12's glibc lacks.
static int ReadLong(FILE *File, long *Value) {
  char Text[64];
  if (!fgets(Text, sizeof(Text), File) || (Text[0] < '0' || Text[0] > '9')) return 0;
  long Result = 0;
  for (const char *Digit = Text; *Digit >= '0' && *Digit <= '9'; ++Digit) Result = Result * 10 + (*Digit - '0');
  *Value = Result;
  return 1;
}

static void *RawMremap(void *Old, size_t OldSize, size_t NewSize, int Flags, void *New) {
  return (void *)syscall(SYS_mremap, Old, OldSize, NewSize, Flags, New);
}

static void *MapAnonymous(size_t Size, int Protection) {
  return mmap(NULL, Size, Protection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

static long Futex(uint32_t *Word, int Op, uint32_t Value, const struct timespec *Timeout, uint32_t *Word2, uint32_t Value3) {
  return syscall(SYS_futex, Word, Op, Value, Timeout, Word2, Value3);
}
// FUTEX_REQUEUE and friends pass the requeue count in the timeout slot.
static long FutexRequeue(uint32_t *Word, int Op, uint32_t Wake, uint32_t Requeue, uint32_t *Word2, uint32_t Value3) {
  return syscall(SYS_futex, Word, Op, Wake, (uintptr_t)Requeue, Word2, Value3);
}
static pid_t Tid(void) { return (pid_t)syscall(SYS_gettid); }

static int Bump(int *Counter) { return __atomic_add_fetch(Counter, 1, __ATOMIC_SEQ_CST); }
static int Load(const int *Counter) { return __atomic_load_n(Counter, __ATOMIC_SEQ_CST); }

// ---------------------------------------------------------------------------
// mem/: virtual memory

TEST(mem, mmap_anon_zero) {
  const size_t Size = 1 << 20;
  unsigned char *Map = MapAnonymous(Size, PROT_READ | PROT_WRITE);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  CHECK((uintptr_t)Map % PAGE == 0);
  size_t NonZero = 0;
  for (size_t Index = 0; Index < Size; Index += 512) NonZero += Map[Index] != 0;
  CHECK(NonZero == 0);
  memset(Map, 0x5a, Size);
  CHECK(Map[Size - 1] == 0x5a);
  CHECK(munmap(Map, Size) == 0);
  CHECK_ERRNO(mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), EINVAL);
  CHECK_ERRNO(mmap(NULL, PAGE, PROT_READ, MAP_ANONYMOUS, -1, 0), EINVAL);
}

TEST(mem, mmap_prot_none_reserve_then_commit) {
  const size_t Reserve = 1UL << 30, Commit = 1 << 20;
  unsigned char *Map = mmap(NULL, Reserve, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  CHECK(mprotect(Map, Commit, PROT_READ | PROT_WRITE) == 0);
  for (size_t Index = 0; Index < Commit; Index += PAGE) Map[Index] = (unsigned char)(Index / PAGE);
  size_t Bad = 0;
  for (size_t Index = 0; Index < Commit; Index += PAGE) Bad += Map[Index] != (unsigned char)(Index / PAGE);
  CHECK(Bad == 0);
  // The rest stays reserved: a fixed no-replace map inside it collides.
  CHECK_ERRNO(mmap(Map + Commit, PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0), EEXIST);
  CHECK(munmap(Map, Reserve) == 0);
}

TEST(mem, mmap_fixed_noreplace_eexist) {
  unsigned char *Map = MapAnonymous(2 * PAGE, PROT_READ | PROT_WRITE);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  Map[0] = 1;
  CHECK_ERRNO(mmap(Map + PAGE, PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0), EEXIST);
  CHECK(Map[0] == 1);
  CHECK(munmap(Map + PAGE, PAGE) == 0);
  void *Again = mmap(Map + PAGE, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  CHECK(Again == Map + PAGE);
  CHECK_ERRNO(mmap(Map + 1, PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0), EINVAL);
}

TEST(mem, mmap_fixed_over_existing) {
  unsigned char *Map = MapAnonymous(4 * PAGE, PROT_READ | PROT_WRITE);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  memset(Map, 7, 4 * PAGE);
  void *Over = mmap(Map + PAGE, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  CHECK(Over == Map + PAGE);
  CHECK(Map[PAGE] == 0 && Map[2 * PAGE - 1] == 0);
  CHECK(Map[0] == 7 && Map[PAGE - 1] == 7 && Map[2 * PAGE] == 7 && Map[4 * PAGE - 1] == 7);
  Map[PAGE] = 9;
  CHECK(Map[PAGE] == 9);
  CHECK(munmap(Map, 4 * PAGE) == 0);
}

TEST(mem, mmap_hugetlb_enomem) {
  long Pages = 0;
  FILE *File = fopen("/proc/sys/vm/nr_hugepages", "r");
  if (File) {
    if (!ReadLong(File, &Pages)) Pages = 0;
    fclose(File);
  }
  if (Pages > 0) SKIP("this system has a huge page pool");
  CHECK_ERRNO(mmap(NULL, 2 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0), ENOMEM);
  CHECK_ERRNO(mmap(NULL, 2 << 20, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0), ENOMEM);
}

TEST(mem, mmap_shared_validate_unknown) {
  const int Fd = open("validate.dat", O_RDWR | O_CREAT | O_TRUNC, 0600);
  CHECK(Fd >= 0);
  CHECK(ftruncate(Fd, PAGE) == 0);
  const int Unknown = 0x200000;  // no MAP_* flag uses this bit on x86-64
  CHECK_ERRNO(mmap(NULL, PAGE, PROT_READ, MAP_SHARED_VALIDATE | Unknown, Fd, 0), EOPNOTSUPP);
  // MAP_SYNC needs a DAX file.
  CHECK_ERRNO(mmap(NULL, PAGE, PROT_READ, MAP_SHARED_VALIDATE | MAP_SYNC, Fd, 0), EOPNOTSUPP);
  // Plain MAP_SHARED ignores unknown flags.
  void *Map = mmap(NULL, PAGE, PROT_READ, MAP_SHARED | Unknown, Fd, 0);
  CHECK(Map != MAP_FAILED);
  if (Map != MAP_FAILED) CHECK(munmap(Map, PAGE) == 0);
  Map = mmap(NULL, PAGE, PROT_READ, MAP_SHARED_VALIDATE, Fd, 0);
  CHECK(Map != MAP_FAILED);
  if (Map != MAP_FAILED) CHECK(munmap(Map, PAGE) == 0);
  CHECK_ERRNO(mmap(NULL, PAGE, PROT_READ, MAP_SHARED_VALIDATE | MAP_ANONYMOUS, -1, 0), EINVAL);
  close(Fd);
}

TEST(mem, mprotect_unmapped_enomem) {
  unsigned char *Map = MapAnonymous(3 * PAGE, PROT_READ | PROT_WRITE);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  CHECK(munmap(Map + PAGE, PAGE) == 0);
  CHECK_ERRNO(mprotect(Map, 3 * PAGE, PROT_READ), ENOMEM);
  CHECK_ERRNO(mprotect(Map + PAGE, PAGE, PROT_READ), ENOMEM);
  CHECK_ERRNO(mprotect(Map + 1, PAGE, PROT_READ), EINVAL);
  CHECK_ERRNO(mprotect(Map, PAGE, 0x100), EINVAL);
  CHECK(mprotect(Map, PAGE, PROT_READ) == 0);
  CHECK(mprotect(Map + 2 * PAGE, PAGE, PROT_NONE) == 0);
}

TEST(mem, syscall_into_prot_none_efault) {
  unsigned char *Map = MapAnonymous(PAGE, PROT_NONE);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK(write(Pipe[1], "data", 4) == 4);
  CHECK_ERRNO(read(Pipe[0], Map, 4), EFAULT);
  CHECK_ERRNO(write(Pipe[1], Map, 4), EFAULT);
  struct stat Status;
  CHECK_ERRNO(fstat(Pipe[0], (struct stat *)Map), EFAULT);
  CHECK(fstat(Pipe[0], &Status) == 0);
  CHECK(mprotect(Map, PAGE, PROT_READ | PROT_WRITE) == 0);
  CHECK(read(Pipe[0], Map, 4) >= 0);
}

TEST(mem, munmap_partial_split) {
  unsigned char *Map = MapAnonymous(3 * PAGE, PROT_READ | PROT_WRITE);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  Map[0] = 1;
  Map[2 * PAGE] = 3;
  CHECK(munmap(Map + PAGE, PAGE) == 0);
  CHECK(Mapped(Map, PAGE));
  CHECK(!Mapped(Map + PAGE, PAGE));
  CHECK(Mapped(Map + 2 * PAGE, PAGE));
  CHECK(Map[0] == 1 && Map[2 * PAGE] == 3);
  // Unmapping a hole again is not an error; an unaligned start is.
  CHECK(munmap(Map + PAGE, PAGE) == 0);
  CHECK_ERRNO(munmap(Map + 1, PAGE), EINVAL);
  CHECK_ERRNO(munmap(Map, 0), EINVAL);
  CHECK(munmap(Map, 3 * PAGE) == 0);
  CHECK(!Mapped(Map, PAGE));
}

TEST(mem, mremap_shrink) {
  unsigned char *Map = MapAnonymous(4 * PAGE, PROT_READ | PROT_WRITE);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  Map[0] = 0x11;
  CHECK(mremap(Map, 4 * PAGE, PAGE, 0) == Map);
  CHECK(Map[0] == 0x11);
  CHECK(!Mapped(Map + PAGE, 3 * PAGE));
}

TEST(mem, mremap_grow_inplace) {
  unsigned char *Map = MapAnonymous(8 * PAGE, PROT_READ | PROT_WRITE);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  CHECK(munmap(Map + 4 * PAGE, 4 * PAGE) == 0);
  Map[0] = 0x22;
  CHECK(mremap(Map, 4 * PAGE, 8 * PAGE, 0) == Map);
  CHECK(Map[0] == 0x22);
  CHECK(Map[8 * PAGE - 1] == 0);
  Map[8 * PAGE - 1] = 1;
  CHECK(Mapped(Map, 8 * PAGE));
}

// Two pages with different protections: the first cannot grow in place.
static unsigned char *BlockedPage(void) {
  unsigned char *Map = MapAnonymous(2 * PAGE, PROT_READ | PROT_WRITE);
  if (Map == MAP_FAILED) return NULL;
  if (mprotect(Map + PAGE, PAGE, PROT_READ)) return NULL;
  return Map;
}

TEST(mem, mremap_maymove_copies) {
  unsigned char *Map = BlockedPage();
  CHECK(Map != NULL);
  if (!Map) return;
  memset(Map, 0x33, PAGE);
  unsigned char *Moved = mremap(Map, PAGE, 4 * PAGE, MREMAP_MAYMOVE);
  CHECK(Moved != MAP_FAILED && Moved != Map);
  if (Moved == MAP_FAILED) return;
  CHECK(Moved[0] == 0x33 && Moved[PAGE - 1] == 0x33);
  CHECK(Moved[PAGE] == 0 && Moved[4 * PAGE - 1] == 0);
  Moved[4 * PAGE - 1] = 1;
  CHECK(!Mapped(Map, PAGE));
  CHECK(Mapped(Map + PAGE, PAGE));
}

TEST(mem, mremap_fixed) {
  unsigned char *Source = MapAnonymous(PAGE, PROT_READ | PROT_WRITE);
  unsigned char *Target = MapAnonymous(4 * PAGE, PROT_READ | PROT_WRITE);
  CHECK(Source != MAP_FAILED && Target != MAP_FAILED);
  if (Source == MAP_FAILED || Target == MAP_FAILED) return;
  memset(Source, 0x44, PAGE);
  memset(Target, 0x55, 4 * PAGE);
  CHECK(mremap(Source, PAGE, PAGE, MREMAP_MAYMOVE | MREMAP_FIXED, Target) == Target);
  CHECK(Target[0] == 0x44 && Target[PAGE - 1] == 0x44);
  CHECK(Target[PAGE] == 0x55 && Target[4 * PAGE - 1] == 0x55);
  CHECK(!Mapped(Source, PAGE));
  // Overlapping source and target is EINVAL.
  CHECK_ERRNO(mremap(Target, 2 * PAGE, 2 * PAGE, MREMAP_MAYMOVE | MREMAP_FIXED, Target + PAGE), EINVAL);
}

TEST(mem, mremap_dontunmap) {
  unsigned char *Map = MapAnonymous(2 * PAGE, PROT_READ | PROT_WRITE);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  memset(Map, 0x66, 2 * PAGE);
  unsigned char *Moved = RawMremap(Map, 2 * PAGE, 2 * PAGE, MREMAP_MAYMOVE | MREMAP_DONTUNMAP, NULL);
  CHECK(Moved != MAP_FAILED && Moved != Map);
  if (Moved == MAP_FAILED) return;
  CHECK(Moved[0] == 0x66 && Moved[2 * PAGE - 1] == 0x66);
  // The old range stays mapped and reads as fresh anonymous memory.
  CHECK(Mapped(Map, 2 * PAGE));
  CHECK(Map[0] == 0 && Map[2 * PAGE - 1] == 0);
  CHECK_ERRNO(RawMremap(Moved, 2 * PAGE, 4 * PAGE, MREMAP_MAYMOVE | MREMAP_DONTUNMAP, NULL), EINVAL);
  CHECK_ERRNO(RawMremap(Moved, 2 * PAGE, 2 * PAGE, MREMAP_DONTUNMAP, NULL), EINVAL);
  unsigned char *Shared = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  CHECK(Shared != MAP_FAILED);
  if (GuestRuntime) {
    // Deliberate difference: the runtime cannot map one shared object twice.
    CHECK_ERRNO(RawMremap(Shared, PAGE, PAGE, MREMAP_MAYMOVE | MREMAP_DONTUNMAP, NULL), EINVAL);
  } else {
    void *Copy = RawMremap(Shared, PAGE, PAGE, MREMAP_MAYMOVE | MREMAP_DONTUNMAP, NULL);
    CHECK(Copy != MAP_FAILED && Copy != Shared);
  }
}

TEST(mem, mremap_errors) {
  unsigned char *Map = BlockedPage();
  CHECK(Map != NULL);
  if (!Map) return;
  CHECK_ERRNO(mremap(Map + 1, PAGE, 2 * PAGE, MREMAP_MAYMOVE), EINVAL);
  CHECK_ERRNO(mremap(Map, PAGE, 2 * PAGE, 8), EINVAL);
  CHECK_ERRNO(mremap(Map, PAGE, 0, MREMAP_MAYMOVE), EINVAL);
  CHECK_ERRNO(mremap(Map, PAGE, PAGE, MREMAP_FIXED, Map + 8 * PAGE), EINVAL);
  CHECK_ERRNO(mremap(Map, PAGE, 2 * PAGE, 0), ENOMEM);
  unsigned char *Hole = MapAnonymous(PAGE, PROT_READ);
  CHECK(Hole != MAP_FAILED);
  CHECK(munmap(Hole, PAGE) == 0);
  CHECK_ERRNO(mremap(Hole, PAGE, 2 * PAGE, MREMAP_MAYMOVE), EFAULT);
  CHECK(Mapped(Map, 2 * PAGE));
}

TEST(mem, mremap_huge_size) {
  unsigned char *Map = MapAnonymous(PAGE, PROT_READ | PROT_WRITE);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  Map[0] = 0x77;
  CHECK_ERRNO(mremap(Map, PAGE, 0xFFFFFFFFFFFFE000UL, MREMAP_MAYMOVE), EINVAL);
  CHECK_ERRNO(mremap(Map, PAGE, 0xFFFFFFFFFFFFE000UL, 0), EINVAL);
  // A size that fits the address space but not memory is ENOMEM.
  CHECK_ERRNO(mremap(Map, PAGE, 0x7FFFFFFFF000UL, MREMAP_MAYMOVE), ENOMEM);
  CHECK(Mapped(Map, PAGE));
  CHECK(Map[0] == 0x77);
}

TEST(mem, msync_flags) {
  unsigned char *Map = MapAnonymous(2 * PAGE, PROT_READ | PROT_WRITE);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  CHECK(msync(Map, 2 * PAGE, MS_SYNC) == 0);
  CHECK(msync(Map, 2 * PAGE, MS_ASYNC | MS_INVALIDATE) == 0);
  CHECK(msync(Map, 0, MS_SYNC) == 0);
  CHECK_ERRNO(msync(Map, PAGE, MS_SYNC | MS_ASYNC), EINVAL);
  CHECK_ERRNO(msync(Map, PAGE, 8), EINVAL);
  CHECK_ERRNO(msync(Map + 1, PAGE, MS_SYNC), EINVAL);
  CHECK(munmap(Map + PAGE, PAGE) == 0);
  CHECK_ERRNO(msync(Map, 2 * PAGE, MS_SYNC), ENOMEM);
}

// A scratch file of Size bytes holding 'a' + page index.
static int PatternFile(const char *Name, size_t Size) {
  const int Fd = open(Name, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (Fd < 0) return -1;
  char Page[PAGE];
  for (size_t Offset = 0; Offset < Size; Offset += PAGE) {
    memset(Page, 'a' + (int)(Offset / PAGE), PAGE);
    if (pwrite(Fd, Page, PAGE, Offset) != (ssize_t)PAGE) return -1;
  }
  return Fd;
}

TEST(mem, msync_writeback_file) {
  const int Fd = PatternFile("msync.dat", 2 * PAGE);
  CHECK(Fd >= 0);
  if (Fd < 0) return;
  char *Map = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, Fd, 0);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  CHECK(Map[0] == 'a' && Map[PAGE] == 'b');
  memcpy(Map + PAGE + 10, "stored", 6);
  CHECK(msync(Map, 2 * PAGE, MS_SYNC) == 0);
  char Buffer[6] = {0};
  CHECK(pread(Fd, Buffer, 6, PAGE + 10) == 6);
  CHECK(memcmp(Buffer, "stored", 6) == 0);
  // A second descriptor sees it too.
  const int Other = open("msync.dat", O_RDONLY);
  CHECK(Other >= 0 && pread(Other, Buffer, 6, PAGE + 10) == 6 && memcmp(Buffer, "stored", 6) == 0);
  close(Other);
  CHECK(munmap(Map, 2 * PAGE) == 0);
  close(Fd);
}

TEST(mem, munmap_writeback_file) {
  const int Fd = PatternFile("munmap.dat", PAGE);
  CHECK(Fd >= 0);
  if (Fd < 0) return;
  char *Map = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, Fd, 0);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  memcpy(Map + 100, "unmapped", 8);
  CHECK(munmap(Map, PAGE) == 0);
  close(Fd);
  char Buffer[8] = {0};
  const int Again = open("munmap.dat", O_RDONLY);
  CHECK(Again >= 0 && pread(Again, Buffer, 8, 100) == 8);
  CHECK(memcmp(Buffer, "unmapped", 8) == 0);
  close(Again);
}

TEST(mem, write_then_mapping_sees_it) {
  const int Fd = PatternFile("coherent.dat", PAGE);
  CHECK(Fd >= 0);
  if (Fd < 0) return;
  volatile char *Map = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, Fd, 0);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  CHECK(Map[5] == 'a');
  CHECK(pwrite(Fd, "WRITTEN", 7, 5) == 7);
  CHECK(Map[5] == 'W' && Map[11] == 'N');
  // read() after a store through the mapping sees the store.
  Map[20] = 'Z';
  char Byte = 0;
  CHECK(pread(Fd, &Byte, 1, 20) == 1 && Byte == 'Z');
  close(Fd);
}

TEST(mem, mincore_basic) {
  unsigned char *Map = MapAnonymous(2 * PAGE, PROT_READ | PROT_WRITE);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  Map[0] = 1;
  unsigned char Vector[2] = {0xfe, 0xfe};
  CHECK(mincore(Map, 2 * PAGE, Vector) == 0);
  CHECK((Vector[0] & 1) == 1);
  CHECK((Vector[0] & 0xfe) == 0 && (Vector[1] & 0xfe) == 0);
  CHECK_ERRNO(mincore(Map + 1, PAGE, Vector), EINVAL);
  unsigned char *NoAccess = MapAnonymous(PAGE, PROT_NONE);
  CHECK_ERRNO(mincore(Map, PAGE, NoAccess), EFAULT);
  CHECK(munmap(Map + PAGE, PAGE) == 0);
  CHECK_ERRNO(mincore(Map, 2 * PAGE, Vector), ENOMEM);
}

TEST(mem, madvise_dontneed_zeroes_private) {
  unsigned char *Map = MapAnonymous(2 * PAGE, PROT_READ | PROT_WRITE);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  memset(Map, 0x12, 2 * PAGE);
  CHECK(madvise(Map, PAGE, MADV_DONTNEED) == 0);
  CHECK(Map[0] == 0 && Map[PAGE - 1] == 0);
  CHECK(Map[PAGE] == 0x12);
}

TEST(mem, madvise_dontneed_keeps_shared) {
  unsigned char *Map = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  memset(Map, 0x34, PAGE);
  CHECK(madvise(Map, PAGE, MADV_DONTNEED) == 0);
  CHECK(Map[0] == 0x34 && Map[PAGE - 1] == 0x34);
}

static unsigned char *ForkRegion;
static void ChildSeesNoRegion(void) {
  CHECK(!Mapped(ForkRegion, PAGE));
  // The rest of the address space came along.
  CHECK(Mapped(ForkRegion + PAGE, PAGE) && ForkRegion[PAGE] == 0x56);
}

TEST(mem, madvise_dontfork) {
  ForkRegion = MapAnonymous(2 * PAGE, PROT_READ | PROT_WRITE);
  CHECK(ForkRegion != MAP_FAILED);
  if (ForkRegion == MAP_FAILED) return;
  memset(ForkRegion, 0x56, 2 * PAGE);
  CHECK(madvise(ForkRegion, PAGE, MADV_DONTFORK) == 0);
  JOIN(Fork(ChildSeesNoRegion));
  CHECK(ForkRegion[0] == 0x56);
  CHECK(madvise(ForkRegion, PAGE, MADV_DOFORK) == 0);
}

static void ChildSeesWiped(void) {
  CHECK(Mapped(ForkRegion, 2 * PAGE));
  CHECK(ForkRegion[0] == 0 && ForkRegion[PAGE - 1] == 0);
  CHECK(ForkRegion[PAGE] == 0x78);
}

TEST(mem, madvise_wipeonfork) {
  ForkRegion = MapAnonymous(2 * PAGE, PROT_READ | PROT_WRITE);
  CHECK(ForkRegion != MAP_FAILED);
  if (ForkRegion == MAP_FAILED) return;
  memset(ForkRegion, 0x78, 2 * PAGE);
  CHECK(madvise(ForkRegion, PAGE, MADV_WIPEONFORK) == 0);
  JOIN(Fork(ChildSeesWiped));
  CHECK(ForkRegion[0] == 0x78);
  unsigned char *Shared = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  CHECK(Shared != MAP_FAILED);
  CHECK_ERRNO(madvise(Shared, PAGE, MADV_WIPEONFORK), EINVAL);
}

TEST(mem, madvise_invalid) {
  unsigned char *Map = MapAnonymous(2 * PAGE, PROT_READ | PROT_WRITE);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  CHECK_ERRNO(madvise(Map, PAGE, 1000), EINVAL);
  CHECK_ERRNO(madvise(Map + 1, PAGE, MADV_NORMAL), EINVAL);
  CHECK(madvise(Map, 2 * PAGE, MADV_SEQUENTIAL) == 0);
  CHECK(madvise(Map, 2 * PAGE, MADV_WILLNEED) == 0);
  CHECK(munmap(Map + PAGE, PAGE) == 0);
  CHECK_ERRNO(madvise(Map, 2 * PAGE, MADV_NORMAL), ENOMEM);
}

static void MlockLimits(void) {
  unsigned char *Map = MapAnonymous(2 * PAGE, PROT_READ | PROT_WRITE);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  struct rlimit Limit = {PAGE, PAGE};
  CHECK(setrlimit(RLIMIT_MEMLOCK, &Limit) == 0);
  CHECK_ERRNO(mlock(Map, 2 * PAGE), ENOMEM);
  CHECK(mlock(Map, PAGE) == 0);
  CHECK(munlock(Map, PAGE) == 0);
  CHECK_ERRNO(mlock2(Map, PAGE, 2), EINVAL);
  Limit = (struct rlimit) {0, 0};
  CHECK(setrlimit(RLIMIT_MEMLOCK, &Limit) == 0);
  CHECK_ERRNO(mlock(Map, PAGE), EPERM);
  CHECK_ERRNO(mlockall(MCL_CURRENT), EPERM);
  CHECK(munlock(Map, PAGE) == 0);
}

TEST(mem, mlock_rlimit) { AsUser(MlockLimits); }

TEST(mem, mlockall_flags) {
  CHECK_ERRNO(mlockall(0), EINVAL);
  CHECK_ERRNO(mlockall(MCL_ONFAULT), EINVAL);
  CHECK_ERRNO(mlockall(0x100), EINVAL);
  CHECK(munlockall() == 0);
}

TEST(mem, brk_grow_shrink_64mib) {
  const uintptr_t Start = (uintptr_t)syscall(SYS_brk, 0);
  CHECK(Start != 0);
  const uintptr_t Grown = Start + (64UL << 20);
  CHECK((uintptr_t)syscall(SYS_brk, Grown) == Grown);
  volatile unsigned char *Heap = (unsigned char *)Start;
  Heap[0] = 1;
  Heap[Grown - Start - 1] = 2;
  CHECK(Heap[0] == 1 && Heap[Grown - Start - 1] == 2);
  CHECK((uintptr_t)syscall(SYS_brk, Start) == Start);
  const uintptr_t Above = (Start + PAGE - 1) & ~(PAGE - 1);
  CHECK(!Mapped((void *)Above, PAGE));
  // Growing again gives zeroed pages.
  CHECK((uintptr_t)syscall(SYS_brk, Grown) == Grown);
  CHECK(Heap[Grown - Start - 1] == 0);
  CHECK((uintptr_t)syscall(SYS_brk, Start) == Start);
}

TEST(mem, memfd_large_sparse) {
  const off_t Size = 512L << 20;
  const int Fd = memfd_create("comm-suite-large", 0);
  CHECK(Fd >= 0);
  if (Fd < 0) return;
  CHECK(ftruncate(Fd, Size) == 0);
  struct stat Status;
  CHECK(fstat(Fd, &Status) == 0 && Status.st_size == Size);
  CHECK(pwrite(Fd, "first", 5, 0) == 5);
  CHECK(pwrite(Fd, "last", 4, Size - PAGE) == 4);
  char *Low = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, Fd, 0);
  char *High = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, Fd, Size - PAGE);
  CHECK(Low != MAP_FAILED && High != MAP_FAILED);
  if (Low == MAP_FAILED || High == MAP_FAILED) return;
  CHECK(memcmp(Low, "first", 5) == 0 && memcmp(High, "last", 4) == 0);
  High[10] = 'x';
  char Byte = 0;
  CHECK(pread(Fd, &Byte, 1, Size - PAGE + 10) == 1 && Byte == 'x');
  CHECK(pread(Fd, &Byte, 1, Size / 2) == 1 && Byte == 0);
  close(Fd);
}

static char VmBuffer[64];
static int VmPipe[2], VmBack[2];
static void VmTarget(void) {
  strcpy(VmBuffer, "child data");
  if (write(VmPipe[1], "r", 1) != 1) _exit(1);
  char Byte;
  if (read(VmBack[0], &Byte, 1) != 1) _exit(1);
  CHECK(strcmp(VmBuffer, "parent wrote") == 0 || Byte == 'r');
}

TEST(mem, process_vm_readv_child) {
  strcpy(VmBuffer, "parent data");
  CHECK(pipe(VmPipe) == 0 && pipe(VmBack) == 0);
  const pid_t Child = Fork(VmTarget);
  char Byte;
  CHECK(read(VmPipe[0], &Byte, 1) == 1);
  char Copy[64] = {0};
  struct iovec Local = {Copy, 11}, Remote = {VmBuffer, 11};
  CHECK(process_vm_readv(Child, &Local, 1, &Remote, 1, 0) == 11);
  CHECK(memcmp(Copy, "child data", 11) == 0);
  CHECK(strcmp(VmBuffer, "parent data") == 0);
  // Split iovecs on both sides.
  char A[4], B[7];
  struct iovec Locals[2] = {{A, 4}, {B, 7}}, Remotes[2] = {{VmBuffer, 6}, {VmBuffer + 6, 5}};
  CHECK(process_vm_readv(Child, Locals, 2, Remotes, 2, 0) == 11);
  CHECK(memcmp(A, "chil", 4) == 0 && memcmp(B, "d data", 7) == 0);
  CHECK_ERRNO(process_vm_readv(Child, &Local, 1, &Remote, 1, 1), EINVAL);
  struct iovec Bad = {(void *)PAGE, 4};
  CHECK_ERRNO(process_vm_readv(Child, &Local, 1, &Bad, 1, 0), EFAULT);
  CHECK(write(VmBack[1], "r", 1) == 1);
  JOIN(Child);
  CHECK_ERRNO(process_vm_readv(Child, &Local, 1, &Remote, 1, 0), ESRCH);
}

static void VmWriteTarget(void) {
  strcpy(VmBuffer, "child data");
  if (write(VmPipe[1], "r", 1) != 1) _exit(1);
  char Byte;
  if (read(VmBack[0], &Byte, 1) != 1) _exit(1);
  CHECK(strcmp(VmBuffer, "parent wrote") == 0);
}

TEST(mem, process_vm_writev_child) {
  CHECK(pipe(VmPipe) == 0 && pipe(VmBack) == 0);
  const pid_t Child = Fork(VmWriteTarget);
  char Byte;
  CHECK(read(VmPipe[0], &Byte, 1) == 1);
  char Text[] = "parent wrote";
  struct iovec Local = {Text, sizeof(Text)}, Remote = {VmBuffer, sizeof(Text)};
  CHECK(process_vm_writev(Child, &Local, 1, &Remote, 1, 0) == (ssize_t)sizeof(Text));
  CHECK(write(VmBack[1], "w", 1) == 1);
  JOIN(Child);
}

static pid_t VmRootTarget;
static void VmUnprivilegedReader(void) {
  if (setgroups(0, NULL) || setresgid(1000, 1000, 1000) || setresuid(1000, 1000, 1000)) {
    printf("#   cannot switch to uid 1000: %s\n", ErrnoName(errno));
    ++CaseFailures;
    return;
  }
  char Copy[8];
  struct iovec Local = {Copy, 8}, Remote = {VmBuffer, 8};
  CHECK_ERRNO(process_vm_readv(VmRootTarget, &Local, 1, &Remote, 1, 0), EPERM);
  CHECK_ERRNO(process_vm_writev(VmRootTarget, &Local, 1, &Remote, 1, 0), EPERM);
}

TEST(mem, process_vm_eperm) {
  if (geteuid() != 0) SKIP("needs root to own the target");
  CHECK(pipe(VmPipe) == 0 && pipe(VmBack) == 0);
  VmRootTarget = Fork(VmTarget);
  char Byte;
  CHECK(read(VmPipe[0], &Byte, 1) == 1);
  JOIN(Fork(VmUnprivilegedReader));
  CHECK(write(VmBack[1], "r", 1) == 1);
  JOIN(VmRootTarget);
}

enum {
  MembarrierQuery = 0, MembarrierPrivateExpedited = 1 << 3, MembarrierRegisterPrivateExpedited = 1 << 4,
  MembarrierGetRegistrations = 1 << 9,
};

TEST(mem, membarrier_query_register) {
  const long Mask = syscall(SYS_membarrier, MembarrierQuery, 0, 0);
  CHECK(Mask > 0);
  CHECK(Mask & MembarrierPrivateExpedited);
  CHECK(Mask & MembarrierRegisterPrivateExpedited);
  CHECK_ERRNO(syscall(SYS_membarrier, MembarrierPrivateExpedited, 0, 0), EPERM);
  CHECK(syscall(SYS_membarrier, MembarrierRegisterPrivateExpedited, 0, 0) == 0);
  CHECK(syscall(SYS_membarrier, MembarrierPrivateExpedited, 0, 0) == 0);
  CHECK(syscall(SYS_membarrier, MembarrierGetRegistrations, 0, 0) & MembarrierRegisterPrivateExpedited);
  CHECK_ERRNO(syscall(SYS_membarrier, 1 << 30, 0, 0), EINVAL);
  CHECK_ERRNO(syscall(SYS_membarrier, MembarrierQuery, 4, 0), EINVAL);
}

TEST(mem, pkey_alloc_enospc) {
  int Keys[32], Count = 0;
  long Key = -1;
  while (Count < 32 && (Key = syscall(SYS_pkey_alloc, 0, 0)) >= 0) Keys[Count++] = (int)Key;
  const int KeyErrno = errno;
  CHECK(Count < 32);
  CHECK(Key == -1 && KeyErrno == ENOSPC);
  CHECK_ERRNO(syscall(SYS_pkey_alloc, 1, 0), EINVAL);
  CHECK_ERRNO(syscall(SYS_pkey_alloc, 0, 4), EINVAL);
  for (int Index = 0; Index < Count; ++Index) CHECK(syscall(SYS_pkey_free, Keys[Index]) == 0);
  CHECK_ERRNO(syscall(SYS_pkey_free, 15), EINVAL);
  // pkey -1 is plain mprotect.
  unsigned char *Map = MapAnonymous(PAGE, PROT_READ);
  CHECK(syscall(SYS_pkey_mprotect, Map, PAGE, PROT_READ | PROT_WRITE, -1) == 0);
  Map[0] = 1;
}

TEST(mem, mseal_blocks_munmap) {
  unsigned char *Map = MapAnonymous(2 * PAGE, PROT_READ | PROT_WRITE);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  CHECK_ERRNO(syscall(462, Map, PAGE, 1), EINVAL);
  CHECK(syscall(462, Map, PAGE, 0) == 0);
  CHECK_ERRNO(munmap(Map, PAGE), EPERM);
  CHECK_ERRNO(mprotect(Map, PAGE, PROT_READ), EPERM);
  CHECK_ERRNO(mremap(Map, PAGE, 2 * PAGE, MREMAP_MAYMOVE), EPERM);
  CHECK_ERRNO(mmap(Map, PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0), EPERM);
  // An unsealed neighbour is still free to change; a range that touches the seal is not.
  CHECK(mprotect(Map + PAGE, PAGE, PROT_READ) == 0);
  CHECK_ERRNO(munmap(Map, 2 * PAGE), EPERM);
  CHECK(munmap(Map + PAGE, PAGE) == 0);
  Map[0] = 2;
  CHECK(Map[0] == 2);
}

TEST(mem, numa_single_node) {
  if (access("/sys/devices/system/node/node1", F_OK) == 0) SKIP("this system has more than one NUMA node");
  int Mode = -1;
  unsigned long Mask = 0;
  CHECK(syscall(SYS_get_mempolicy, &Mode, NULL, 0, NULL, 0) == 0);
  CHECK(Mode == 0);  // MPOL_DEFAULT
  Mask = 1;
  CHECK(syscall(SYS_set_mempolicy, 2 /* MPOL_BIND */, &Mask, 64) == 0);
  CHECK(syscall(SYS_get_mempolicy, &Mode, &Mask, 64, NULL, 0) == 0);
  CHECK(Mode == 2 && Mask == 1);
  Mask = 2;
  CHECK_ERRNO(syscall(SYS_set_mempolicy, 2, &Mask, 64), EINVAL);
  CHECK(syscall(SYS_set_mempolicy, 0, NULL, 0) == 0);
  CHECK_ERRNO(syscall(SYS_set_mempolicy, 99, NULL, 0), EINVAL);
  unsigned Cpu = 99, Node = 99;
  CHECK(syscall(SYS_getcpu, &Cpu, &Node, NULL) == 0);
  CHECK(Node == 0);
}

TEST(mem, userfaultfd_enosys) {
  GUEST_ONLY();
  CHECK_ERRNO(syscall(SYS_userfaultfd, O_CLOEXEC), ENOSYS);
}

// ---------------------------------------------------------------------------
// futex/

static uint32_t Word, Word2;
static int Started, Finished;
static long ThreadResult;
static int ThreadErrno;

static void *WaitOnWord(void *Argument) {
  (void)Argument;
  Bump(&Started);
  ThreadResult = Futex(&Word, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
  ThreadErrno = errno;
  Bump(&Finished);
  return NULL;
}

TEST(futex, wait_wake) {
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, WaitOnWord, NULL) == 0);
  long Woken = 0;
  const uint64_t Limit = Deadline(3000);
  while (!Woken && !DeadlineExpired(Limit)) {
    SleepMs(5);
    Woken = Futex(&Word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
  }
  CHECK(Woken == 1);
  CHECK(pthread_join(Thread, NULL) == 0);
  CHECK(ThreadResult == 0);
  CHECK(Futex(&Word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0) == 0);
  // A value mismatch returns at once.
  Word = 1;
  CHECK_ERRNO(Futex(&Word, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0), EAGAIN);
  CHECK_ERRNO(Futex((uint32_t *)((char *)&Word + 1), FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0), EINVAL);
  CHECK_ERRNO(Futex(NULL, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0), EFAULT);
  CHECK_ERRNO(Futex(&Word, 99, 0, NULL, NULL, 0), ENOSYS);
}

TEST(futex, wait_timeout) {
  const struct timespec Timeout = {0, 50 * 1000000};
  const uint64_t Start = NowMs();
  CHECK_ERRNO(Futex(&Word, FUTEX_WAIT_PRIVATE, 0, &Timeout, NULL, 0), ETIMEDOUT);
  CHECK(NowMs() - Start >= 49);
  const struct timespec Bad = {0, 1000000000};
  CHECK_ERRNO(Futex(&Word, FUTEX_WAIT_PRIVATE, 0, &Bad, NULL, 0), EINVAL);
  const struct timespec Zero = {0, 0};
  CHECK_ERRNO(Futex(&Word, FUTEX_WAIT_PRIVATE, 0, &Zero, NULL, 0), ETIMEDOUT);
}

TEST(futex, wait_bitset_realtime_abs) {
  struct timespec Now;
  clock_gettime(CLOCK_REALTIME, &Now);
  struct timespec Until = Now;
  Until.tv_nsec += 50 * 1000000;
  if (Until.tv_nsec >= 1000000000) {
    Until.tv_nsec -= 1000000000;
    ++Until.tv_sec;
  }
  const uint64_t Start = NowMs();
  CHECK_ERRNO(Futex(&Word, FUTEX_WAIT_BITSET_PRIVATE | FUTEX_CLOCK_REALTIME, 0, &Until, NULL, FUTEX_BITSET_MATCH_ANY), ETIMEDOUT);
  CHECK(NowMs() - Start >= 40);
  // A deadline in the past times out at once.
  CHECK_ERRNO(Futex(&Word, FUTEX_WAIT_BITSET_PRIVATE | FUTEX_CLOCK_REALTIME, 0, &Now, NULL, FUTEX_BITSET_MATCH_ANY), ETIMEDOUT);
  CHECK_ERRNO(Futex(&Word, FUTEX_WAIT_BITSET_PRIVATE, 0, &Now, NULL, 0), EINVAL);
  CHECK_ERRNO(Futex(&Word, FUTEX_WAKE_BITSET_PRIVATE, 1, NULL, NULL, 0), EINVAL);
  // FUTEX_CLOCK_REALTIME is only for the wait operations.
  CHECK_ERRNO(Futex(&Word, FUTEX_WAKE_PRIVATE | FUTEX_CLOCK_REALTIME, 1, NULL, NULL, 0), ENOSYS);
}

TEST(futex, requeue) {
  pthread_t Threads[3];
  for (int Index = 0; Index < 3; ++Index) CHECK(pthread_create(&Threads[Index], NULL, WaitOnWord, NULL) == 0);
  WAIT_UNTIL(Load(&Started) == 3);
  // Move every waiter to Word2 as they arrive (CMP_REQUEUE checks Word is still 0).
  long Moved = 0;
  const uint64_t Limit = Deadline(3000);
  while (Moved < 3 && !DeadlineExpired(Limit)) {
    SleepMs(5);
    const long Result = FutexRequeue(&Word, FUTEX_CMP_REQUEUE_PRIVATE, 0, INT_MAX, &Word2, 0);
    if (Result > 0) Moved += Result;
  }
  CHECK(Moved == 3);
  // FUTEX_REQUEUE wakes one and requeues one back; the count includes both.
  CHECK(FutexRequeue(&Word2, FUTEX_REQUEUE_PRIVATE, 1, 1, &Word, 0) == 2);
  WAIT_UNTIL(Load(&Finished) == 1);
  CHECK(Load(&Finished) == 1);
  CHECK(Futex(&Word, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0) == 1);
  CHECK(Futex(&Word2, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0) == 1);
  for (int Index = 0; Index < 3; ++Index) CHECK(pthread_join(Threads[Index], NULL) == 0);
  CHECK(ThreadResult == 0);
}

TEST(futex, cmp_requeue_eagain) {
  Word = 5;
  CHECK_ERRNO(FutexRequeue(&Word, FUTEX_CMP_REQUEUE_PRIVATE, 1, 1, &Word2, 4), EAGAIN);
  CHECK(FutexRequeue(&Word, FUTEX_CMP_REQUEUE_PRIVATE, 1, 1, &Word2, 5) == 0);
  CHECK_ERRNO(FutexRequeue(&Word, FUTEX_CMP_REQUEUE_PRIVATE, (uint32_t)-1, 1, &Word2, 5), EINVAL);
  CHECK_ERRNO(FutexRequeue(&Word, FUTEX_CMP_REQUEUE_PRIVATE, 1, (uint32_t)-1, &Word2, 5), EINVAL);
}

static void *WaitOnWord2(void *Argument) {
  (void)Argument;
  Bump(&Started);
  ThreadResult = Futex(&Word2, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
  ThreadErrno = errno;
  Bump(&Finished);
  return NULL;
}

TEST(futex, wake_op_all_ops) {
  // Arithmetic on Word2; no waiters, so every call wakes nobody.
  static const struct { int Op, Arg; uint32_t Before, After; } Ops[] = {
    {FUTEX_OP_SET, 5, 1, 5}, {FUTEX_OP_ADD, 3, 5, 8}, {FUTEX_OP_OR, 0x10, 8, 0x18},
    {FUTEX_OP_ANDN, 0x8, 0x18, 0x10}, {FUTEX_OP_XOR, 0x11, 0x10, 0x01},
    {FUTEX_OP_SET | FUTEX_OP_OPARG_SHIFT, 4, 0, 16}, {FUTEX_OP_ADD | FUTEX_OP_OPARG_SHIFT, 1, 16, 18},
  };
  for (unsigned Index = 0; Index < sizeof(Ops) / sizeof(Ops[0]); ++Index) {
    Word2 = Ops[Index].Before;
    CHECK(Futex(&Word, FUTEX_WAKE_OP_PRIVATE, 1, (void *)1, &Word2, FUTEX_OP(Ops[Index].Op, Ops[Index].Arg, FUTEX_OP_CMP_EQ, 0)) == 0);
    if (Word2 != Ops[Index].After) printf("#   op %u: %#x, expected %#x\n", Index, Word2, Ops[Index].After);
    CHECK(Word2 == Ops[Index].After);
  }
  CHECK_ERRNO(Futex(&Word, FUTEX_WAKE_OP_PRIVATE, 1, (void *)1, &Word2, FUTEX_OP(7, 0, FUTEX_OP_CMP_EQ, 0)), ENOSYS);
  // Comparisons on the old value of Word2 decide whether its waiter wakes.
  Word2 = 0;
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, WaitOnWord2, NULL) == 0);
  WAIT_UNTIL(Load(&Started) == 1);
  SleepMs(100);
  // Each call sets Word2 to 3 and compares the old value 3 (0 the first time).
  static const struct { int Cmp, Arg; } False[] = {
    {FUTEX_OP_CMP_EQ, 1}, {FUTEX_OP_CMP_NE, 3}, {FUTEX_OP_CMP_LT, 3}, {FUTEX_OP_CMP_LE, 2},
    {FUTEX_OP_CMP_GT, 3}, {FUTEX_OP_CMP_GE, 4},
  };
  CHECK(Futex(&Word, FUTEX_WAKE_OP_PRIVATE, 1, (void *)1, &Word2, FUTEX_OP(FUTEX_OP_SET, 3, FUTEX_OP_CMP_GT, 0)) == 0);
  for (unsigned Index = 0; Index < sizeof(False) / sizeof(False[0]); ++Index) {
    const long Result = Futex(&Word, FUTEX_WAKE_OP_PRIVATE, 1, (void *)1, &Word2, FUTEX_OP(FUTEX_OP_SET, 3, False[Index].Cmp, False[Index].Arg));
    if (Result != 0) printf("#   cmp %u woke %ld\n", Index, Result);
    CHECK(Result == 0);
  }
  CHECK(Load(&Finished) == 0);
  CHECK(Futex(&Word, FUTEX_WAKE_OP_PRIVATE, 1, (void *)1, &Word2, FUTEX_OP(FUTEX_OP_SET, 3, FUTEX_OP_CMP_GE, 3)) == 1);
  CHECK(pthread_join(Thread, NULL) == 0);
  CHECK(ThreadResult == 0);
}

static uint32_t *SharedWord;
static void SharedWaiter(void) {
  CHECK(Futex(SharedWord, FUTEX_WAIT, 0, NULL, NULL, 0) == 0);
}

TEST(futex, shared_across_fork) {
  SharedWord = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  CHECK(SharedWord != MAP_FAILED);
  if (SharedWord == MAP_FAILED) return;
  const pid_t Child = Fork(SharedWaiter);
  long Woken = 0;
  const uint64_t Limit = Deadline(3000);
  while (!Woken && !DeadlineExpired(Limit)) {
    SleepMs(5);
    Woken = Futex(SharedWord, FUTEX_WAKE, 1, NULL, NULL, 0);
  }
  CHECK(Woken == 1);
  JOIN(Child);
  // A private wake does not reach a shared waiter in another process.
  const pid_t Second = Fork(SharedWaiter);
  SleepMs(100);
  CHECK(Futex(SharedWord, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0) == 0);
  WAIT_UNTIL(Futex(SharedWord, FUTEX_WAKE, 1, NULL, NULL, 0) == 1);
  JOIN(Second);
}

static uint32_t PiWord;
static pid_t WorkerTid;

static void *LockPi(void *Argument) {
  (void)Argument;
  WorkerTid = Tid();
  Bump(&Started);
  ThreadResult = Futex(&PiWord, FUTEX_LOCK_PI_PRIVATE, 0, NULL, NULL, 0);
  ThreadErrno = errno;
  // The handoff made this thread the owner.
  if (ThreadResult == 0 && (PiWord & FUTEX_TID_MASK) != (uint32_t)Tid()) ThreadResult = -2;
  Bump(&Finished);
  if (ThreadResult == 0 && Futex(&PiWord, FUTEX_UNLOCK_PI_PRIVATE, 0, NULL, NULL, 0) != 0) ThreadResult = -3;
  return NULL;
}

TEST(futex, lock_pi_unlock_handoff) {
  CHECK(Futex(&PiWord, FUTEX_LOCK_PI_PRIVATE, 0, NULL, NULL, 0) == 0);
  CHECK(PiWord == (uint32_t)Tid());
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, LockPi, NULL) == 0);
  WAIT_UNTIL(PiWord & FUTEX_WAITERS);
  CHECK(PiWord == ((uint32_t)Tid() | FUTEX_WAITERS));
  CHECK(Load(&Finished) == 0);
  CHECK(Futex(&PiWord, FUTEX_UNLOCK_PI_PRIVATE, 0, NULL, NULL, 0) == 0);
  CHECK(pthread_join(Thread, NULL) == 0);
  if (ThreadResult) printf("#   worker result %ld (%s)\n", ThreadResult, ErrnoName(ThreadErrno));
  CHECK(ThreadResult == 0);
  CHECK(PiWord == 0);
}

static void *TryLockPi(void *Argument) {
  (void)Argument;
  ThreadResult = Futex(&PiWord, FUTEX_TRYLOCK_PI_PRIVATE, 0, NULL, NULL, 0);
  ThreadErrno = errno;
  return NULL;
}

TEST(futex, trylock_pi_eagain) {
  CHECK(Futex(&PiWord, FUTEX_TRYLOCK_PI_PRIVATE, 0, NULL, NULL, 0) == 0);
  CHECK(PiWord == (uint32_t)Tid());
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, TryLockPi, NULL) == 0);
  CHECK(pthread_join(Thread, NULL) == 0);
  CHECK(ThreadResult == -1 && ThreadErrno == EAGAIN);
  CHECK((PiWord & FUTEX_TID_MASK) == (uint32_t)Tid());
  CHECK(Futex(&PiWord, FUTEX_UNLOCK_PI_PRIVATE, 0, NULL, NULL, 0) == 0);
  CHECK(PiWord == 0);
}

TEST(futex, lock_pi_deadlock) {
  CHECK(Futex(&PiWord, FUTEX_LOCK_PI_PRIVATE, 0, NULL, NULL, 0) == 0);
  CHECK_ERRNO(Futex(&PiWord, FUTEX_LOCK_PI_PRIVATE, 0, NULL, NULL, 0), EDEADLK);
  CHECK_ERRNO(Futex(&PiWord, FUTEX_TRYLOCK_PI_PRIVATE, 0, NULL, NULL, 0), EDEADLK);
  CHECK(Futex(&PiWord, FUTEX_UNLOCK_PI_PRIVATE, 0, NULL, NULL, 0) == 0);
}

TEST(futex, unlock_pi_eperm) {
  PiWord = (uint32_t)getppid();
  CHECK_ERRNO(Futex(&PiWord, FUTEX_UNLOCK_PI_PRIVATE, 0, NULL, NULL, 0), EPERM);
  PiWord = 0;
  CHECK_ERRNO(Futex(&PiWord, FUTEX_UNLOCK_PI_PRIVATE, 0, NULL, NULL, 0), EPERM);
}

static pthread_mutex_t RobustMutex;
static void *LockAndExit(void *Argument) {
  (void)Argument;
  ThreadResult = pthread_mutex_lock(&RobustMutex);
  return NULL;
}

TEST(futex, robust_owner_died) {
  pthread_mutexattr_t Attributes;
  pthread_mutexattr_init(&Attributes);
  CHECK(pthread_mutexattr_setrobust(&Attributes, PTHREAD_MUTEX_ROBUST) == 0);
  CHECK(pthread_mutex_init(&RobustMutex, &Attributes) == 0);
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, LockAndExit, NULL) == 0);
  CHECK(pthread_join(Thread, NULL) == 0);
  CHECK(ThreadResult == 0);
  CHECK(pthread_mutex_lock(&RobustMutex) == EOWNERDEAD);
  CHECK(pthread_mutex_consistent(&RobustMutex) == 0);
  CHECK(pthread_mutex_unlock(&RobustMutex) == 0);
  CHECK(pthread_mutex_lock(&RobustMutex) == 0);
  CHECK(pthread_mutex_unlock(&RobustMutex) == 0);
}

struct RobustHead {
  void *Next;
  long Offset;
  void *Pending;
};

TEST(futex, robust_list_len_einval) {
  struct RobustHead Head = {&Head, 0, NULL};
  CHECK_ERRNO(syscall(SYS_set_robust_list, &Head, sizeof(Head) - 1), EINVAL);
  CHECK_ERRNO(syscall(SYS_set_robust_list, &Head, sizeof(Head) + 1), EINVAL);
}

TEST(futex, get_robust_list_self) {
  void *Head = NULL;
  size_t Length = 0;
  CHECK(syscall(SYS_get_robust_list, 0, &Head, &Length) == 0);
  CHECK(Length == sizeof(struct RobustHead));
  CHECK(Head != NULL);  // glibc registers one per thread
  void *Same = NULL;
  CHECK(syscall(SYS_get_robust_list, Tid(), &Same, &Length) == 0 && Same == Head);
  CHECK_ERRNO(syscall(SYS_get_robust_list, INT_MAX, &Head, &Length), ESRCH);
}

struct WaitV {
  uint64_t Value;
  uint64_t Address;
  uint32_t Flags;
  uint32_t Reserved;
};
enum { Futex2SizeU32 = 2, Futex2Private = 128 };
static uint32_t Words[2];

static void *WakeSecond(void *Argument) {
  (void)Argument;
  long Woken = 0;
  const uint64_t Limit = Deadline(3000);
  while (!Woken && !DeadlineExpired(Limit)) {
    SleepMs(5);
    Woken = Futex(&Words[1], FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
  }
  return NULL;
}

TEST(futex, waitv_index) {
  struct WaitV Waiters[2] = {
    {0, (uintptr_t)&Words[0], Futex2SizeU32 | Futex2Private, 0},
    {0, (uintptr_t)&Words[1], Futex2SizeU32 | Futex2Private, 0},
  };
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, WakeSecond, NULL) == 0);
  CHECK(syscall(SYS_futex_waitv, Waiters, 2, 0, NULL, CLOCK_MONOTONIC) == 1);
  CHECK(pthread_join(Thread, NULL) == 0);
  // An absolute timeout on the given clock.
  struct timespec Until;
  clock_gettime(CLOCK_MONOTONIC, &Until);
  Until.tv_nsec += 20 * 1000000;
  if (Until.tv_nsec >= 1000000000) {
    Until.tv_nsec -= 1000000000;
    ++Until.tv_sec;
  }
  CHECK_ERRNO(syscall(SYS_futex_waitv, Waiters, 2, 0, &Until, CLOCK_MONOTONIC), ETIMEDOUT);
}

TEST(futex, waitv_eagain) {
  Words[1] = 3;
  struct WaitV Waiters[2] = {
    {0, (uintptr_t)&Words[0], Futex2SizeU32 | Futex2Private, 0},
    {0, (uintptr_t)&Words[1], Futex2SizeU32 | Futex2Private, 0},
  };
  CHECK_ERRNO(syscall(SYS_futex_waitv, Waiters, 2, 0, NULL, CLOCK_MONOTONIC), EAGAIN);
  CHECK_ERRNO(syscall(SYS_futex_waitv, Waiters, 0, 0, NULL, CLOCK_MONOTONIC), EINVAL);
  CHECK_ERRNO(syscall(SYS_futex_waitv, Waiters, 2, 1, NULL, CLOCK_MONOTONIC), EINVAL);
  Waiters[0].Flags = 3;  // FUTEX2_SIZE_U64 is not supported
  CHECK_ERRNO(syscall(SYS_futex_waitv, Waiters, 2, 0, NULL, CLOCK_MONOTONIC), EINVAL);
  Waiters[0].Flags = Futex2SizeU32;
  Waiters[0].Reserved = 1;
  CHECK_ERRNO(syscall(SYS_futex_waitv, Waiters, 2, 0, NULL, CLOCK_MONOTONIC), EINVAL);
}

static void *Futex2Waiter(void *Argument) {
  (void)Argument;
  Bump(&Started);
  ThreadResult = syscall(455, &Word, 0, 0xffffffffUL, Futex2SizeU32 | Futex2Private, NULL, CLOCK_MONOTONIC);
  ThreadErrno = errno;
  return NULL;
}

TEST(futex, futex2_wait_wake) {
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, Futex2Waiter, NULL) == 0);
  long Woken = 0;
  const uint64_t Limit = Deadline(3000);
  while (!Woken && !DeadlineExpired(Limit)) {
    SleepMs(5);
    Woken = syscall(454, &Word, 0xffffffffUL, 1, Futex2SizeU32 | Futex2Private);
  }
  CHECK(Woken == 1);
  CHECK(pthread_join(Thread, NULL) == 0);
  CHECK(ThreadResult == 0);
  Word = 1;
  CHECK_ERRNO(syscall(455, &Word, 0, 0xffffffffUL, Futex2SizeU32 | Futex2Private, NULL, CLOCK_MONOTONIC), EAGAIN);
  Word = 0;
  struct timespec Until;
  clock_gettime(CLOCK_MONOTONIC, &Until);
  CHECK_ERRNO(syscall(455, &Word, 0, 0xffffffffUL, Futex2SizeU32 | Futex2Private, &Until, CLOCK_MONOTONIC), ETIMEDOUT);
  CHECK_ERRNO(syscall(455, &Word, 0, 0UL, Futex2SizeU32 | Futex2Private, NULL, CLOCK_MONOTONIC), EINVAL);
  CHECK_ERRNO(syscall(454, &Word, 0xffffffffUL, 1, 0x1000 | Futex2SizeU32), EINVAL);
}

// FUTEX_WAIT_REQUEUE_PI and a signal. Before the requeue the wait restarts
// (-ERESTARTNOINTR, even without SA_RESTART); after it, the waiter is blocked
// on the PI futex and the signal ends the wait with EAGAIN (futex_wait_requeue_pi).
static uint32_t Condition;
static volatile sig_atomic_t RequeueSignals;
static void CountRequeueSignal(int Signal) {
  (void)Signal;
  ++RequeueSignals;
}

static void *WaitRequeuePi(void *Argument) {
  (void)Argument;
  WorkerTid = Tid();
  Bump(&Started);
  ThreadResult = Futex(&Condition, FUTEX_WAIT_REQUEUE_PI_PRIVATE, 0, NULL, &PiWord, 0);
  ThreadErrno = errno;
  const int Owner = ThreadResult == 0 && (PiWord & FUTEX_TID_MASK) == (uint32_t)Tid();
  Bump(&Finished);
  if (Owner && Futex(&PiWord, FUTEX_UNLOCK_PI_PRIVATE, 0, NULL, NULL, 0) != 0) ThreadResult = -3;
  if (ThreadResult == 0 && !Owner) ThreadResult = -2;
  return NULL;
}

static void InstallRequeueHandler(void) {
  struct sigaction Action = {.sa_handler = CountRequeueSignal};
  sigemptyset(&Action.sa_mask);
  sigaction(SIGUSR1, &Action, NULL);  // no SA_RESTART
}

TEST(futex, wait_requeue_pi_signal_before) {
  InstallRequeueHandler();
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, WaitRequeuePi, NULL) == 0);
  WAIT_UNTIL(Load(&Started) == 1);
  SleepMs(100);
  CHECK(syscall(SYS_tgkill, getpid(), WorkerTid, SIGUSR1) == 0);
  WAIT_UNTIL(RequeueSignals == 1);
  SleepMs(50);
  // The handler ran and the wait restarted.
  CHECK(RequeueSignals == 1);
  CHECK(Load(&Finished) == 0);
  // CMP_REQUEUE_PI with a free PI futex hands the lock to the waiter.
  long Result = 0;
  const uint64_t Limit = Deadline(3000);
  while (Result == 0 && !DeadlineExpired(Limit)) {
    Result = FutexRequeue(&Condition, FUTEX_CMP_REQUEUE_PI_PRIVATE, 1, 0, &PiWord, 0);
    if (Result == 0) SleepMs(5);
  }
  CHECK(Result == 1);
  CHECK(pthread_join(Thread, NULL) == 0);
  if (ThreadResult) printf("#   waiter result %ld (%s)\n", ThreadResult, ErrnoName(ThreadErrno));
  CHECK(ThreadResult == 0);
  CHECK(PiWord == 0);
}

TEST(futex, wait_requeue_pi_signal_after) {
  InstallRequeueHandler();
  CHECK(Futex(&PiWord, FUTEX_LOCK_PI_PRIVATE, 0, NULL, NULL, 0) == 0);
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, WaitRequeuePi, NULL) == 0);
  WAIT_UNTIL(Load(&Started) == 1);
  long Result = 0;
  const uint64_t Limit = Deadline(3000);
  while (Result == 0 && !DeadlineExpired(Limit)) {
    SleepMs(5);
    Result = FutexRequeue(&Condition, FUTEX_CMP_REQUEUE_PI_PRIVATE, 1, 1, &PiWord, 0);
  }
  // The lock is held, so the waiter was requeued onto the PI futex.
  CHECK(Result == 1);
  WAIT_UNTIL(PiWord & FUTEX_WAITERS);
  CHECK(PiWord == ((uint32_t)Tid() | FUTEX_WAITERS));
  CHECK(Load(&Finished) == 0);
  CHECK(syscall(SYS_tgkill, getpid(), WorkerTid, SIGUSR1) == 0);
  WAIT_UNTIL(Load(&Finished) == 1);
  CHECK(RequeueSignals == 1);
  CHECK(pthread_join(Thread, NULL) == 0);
  // EAGAIN, or 0 when the lock was acquired while leaving the wait.
  if (ThreadResult) CHECK(ThreadResult == -1 && ThreadErrno == EAGAIN);
  if (!ThreadResult) printf("#   waiter acquired the lock\n");
  CHECK((PiWord & FUTEX_TID_MASK) == (uint32_t)Tid() || ThreadResult == 0);
  if ((PiWord & FUTEX_TID_MASK) == (uint32_t)Tid()) CHECK(Futex(&PiWord, FUTEX_UNLOCK_PI_PRIVATE, 0, NULL, NULL, 0) == 0);
  CHECK(PiWord == 0);
}

// ---------------------------------------------------------------------------
// thread/: glibc synchronization built on futexes, and /dev/shm

static pthread_mutex_t Mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t Go = PTHREAD_COND_INITIALIZER, Done = PTHREAD_COND_INITIALIZER;
static int Released, Counter, Exited;

static void *CountUnderMutex(void *Argument) {
  (void)Argument;
  pthread_mutex_lock(&Mutex);
  ++Started;
  pthread_cond_signal(&Done);
  while (!Released) pthread_cond_wait(&Go, &Mutex);
  pthread_mutex_unlock(&Mutex);
  for (int Index = 0; Index < 2000; ++Index) {
    pthread_mutex_lock(&Mutex);
    ++Counter;
    if (Index % 256 == 0) sched_yield();
    pthread_mutex_unlock(&Mutex);
  }
  pthread_mutex_lock(&Mutex);
  ++Exited;
  pthread_cond_signal(&Done);
  pthread_mutex_unlock(&Mutex);
  return NULL;
}

TEST(thread, pthread_mutex_condvar_8_threads) {
  pthread_t Threads[8];
  for (int Index = 0; Index < 8; ++Index) CHECK(pthread_create(&Threads[Index], NULL, CountUnderMutex, NULL) == 0);
  pthread_mutex_lock(&Mutex);
  while (Started < 8) pthread_cond_wait(&Done, &Mutex);
  Released = 1;
  pthread_cond_broadcast(&Go);
  while (Exited < 8) pthread_cond_wait(&Done, &Mutex);
  pthread_mutex_unlock(&Mutex);
  for (int Index = 0; Index < 8; ++Index) CHECK(pthread_join(Threads[Index], NULL) == 0);
  CHECK(Counter == 16000);
  // A timed wait on a condition nobody signals times out.
  struct timespec Until;
  clock_gettime(CLOCK_REALTIME, &Until);
  Until.tv_nsec += 20 * 1000000;
  if (Until.tv_nsec >= 1000000000) {
    Until.tv_nsec -= 1000000000;
    ++Until.tv_sec;
  }
  pthread_mutex_lock(&Mutex);
  CHECK(pthread_cond_timedwait(&Go, &Mutex, &Until) == ETIMEDOUT);
  pthread_mutex_unlock(&Mutex);
}

static pthread_rwlock_t RwLock = PTHREAD_RWLOCK_INITIALIZER;
static void *ReadAndWrite(void *Argument) {
  (void)Argument;
  ThreadResult = pthread_rwlock_tryrdlock(&RwLock);
  if (!ThreadResult) pthread_rwlock_unlock(&RwLock);
  if (!ThreadResult) ThreadResult = pthread_rwlock_trywrlock(&RwLock) == EBUSY ? 0 : -1;
  Bump(&Started);
  if (!ThreadResult) ThreadResult = pthread_rwlock_wrlock(&RwLock);
  Bump(&Finished);
  if (!ThreadResult) pthread_rwlock_unlock(&RwLock);
  return NULL;
}

TEST(thread, pthread_rwlock) {
  CHECK(pthread_rwlock_rdlock(&RwLock) == 0);
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, ReadAndWrite, NULL) == 0);
  WAIT_UNTIL(Load(&Started) == 1);
  SleepMs(50);
  CHECK(Load(&Finished) == 0);  // the writer waits for the reader
  CHECK(pthread_rwlock_unlock(&RwLock) == 0);
  CHECK(pthread_join(Thread, NULL) == 0);
  CHECK(ThreadResult == 0);
  CHECK(Load(&Finished) == 1);
  CHECK(pthread_rwlock_wrlock(&RwLock) == 0);
  CHECK(pthread_rwlock_tryrdlock(&RwLock) == EDEADLK || pthread_rwlock_tryrdlock(&RwLock) == EBUSY);
  CHECK(pthread_rwlock_unlock(&RwLock) == 0);
}

static pthread_barrier_t Barrier;
static int SerialCount;
static void *BarrierRounds(void *Argument) {
  (void)Argument;
  for (int Round = 0; Round < 3; ++Round) {
    const int Result = pthread_barrier_wait(&Barrier);
    if (Result == PTHREAD_BARRIER_SERIAL_THREAD) Bump(&SerialCount);
    else if (Result != 0) Bump(&Exited);
  }
  return NULL;
}

TEST(thread, pthread_barrier) {
  CHECK(pthread_barrier_init(&Barrier, NULL, 4) == 0);
  pthread_t Threads[4];
  for (int Index = 0; Index < 4; ++Index) CHECK(pthread_create(&Threads[Index], NULL, BarrierRounds, NULL) == 0);
  for (int Index = 0; Index < 4; ++Index) CHECK(pthread_join(Threads[Index], NULL) == 0);
  CHECK(SerialCount == 3);
  CHECK(Exited == 0);
  CHECK(pthread_barrier_destroy(&Barrier) == 0);
  CHECK(pthread_barrier_init(&Barrier, NULL, 0) == EINVAL);
}

static pthread_spinlock_t Spin;
static void *SpinCount(void *Argument) {
  (void)Argument;
  for (int Index = 0; Index < 5000; ++Index) {
    pthread_spin_lock(&Spin);
    ++Counter;
    pthread_spin_unlock(&Spin);
  }
  return NULL;
}

TEST(thread, pthread_spin) {
  CHECK(pthread_spin_init(&Spin, PTHREAD_PROCESS_PRIVATE) == 0);
  pthread_t Threads[4];
  for (int Index = 0; Index < 4; ++Index) CHECK(pthread_create(&Threads[Index], NULL, SpinCount, NULL) == 0);
  for (int Index = 0; Index < 4; ++Index) CHECK(pthread_join(Threads[Index], NULL) == 0);
  CHECK(Counter == 20000);
  CHECK(pthread_spin_lock(&Spin) == 0);
  CHECK(pthread_spin_trylock(&Spin) == EBUSY);
  CHECK(pthread_spin_unlock(&Spin) == 0);
  CHECK(pthread_spin_destroy(&Spin) == 0);
}

static char ObjectName[64];
static void NameObject(const char *Kind) { snprintf(ObjectName, sizeof(ObjectName), "/comm-suite.%s.%d", Kind, getpid()); }

static void PostNamed(void) {
  sem_t *Semaphore = sem_open(ObjectName, 0);
  CHECK(Semaphore != SEM_FAILED);
  if (Semaphore != SEM_FAILED) CHECK(sem_post(Semaphore) == 0);
}

static int TimedSemWait(sem_t *Semaphore, unsigned Milliseconds) {
  struct timespec Until;
  clock_gettime(CLOCK_REALTIME, &Until);
  Until.tv_sec += Milliseconds / 1000;
  Until.tv_nsec += (long)(Milliseconds % 1000) * 1000000;
  if (Until.tv_nsec >= 1000000000) {
    Until.tv_nsec -= 1000000000;
    ++Until.tv_sec;
  }
  int Result;
  while ((Result = sem_timedwait(Semaphore, &Until)) < 0 && errno == EINTR) {}
  return Result;
}

TEST(thread, sem_open_named) {
  NameObject("sem");
  sem_t *Semaphore = sem_open(ObjectName, O_CREAT | O_EXCL, 0600, 0);
  CHECK(Semaphore != SEM_FAILED);
  if (Semaphore == SEM_FAILED) return;
  CHECK(sem_open(ObjectName, O_CREAT | O_EXCL, 0600, 0) == SEM_FAILED && errno == EEXIST);
  const pid_t Child = Fork(PostNamed);
  CHECK(TimedSemWait(Semaphore, 3000) == 0);
  JOIN(Child);
  CHECK_ERRNO(sem_trywait(Semaphore), EAGAIN);
  CHECK(TimedSemWait(Semaphore, 20) == -1 && errno == ETIMEDOUT);
  char Path[96];
  snprintf(Path, sizeof(Path), "/dev/shm/sem.%s", ObjectName + 1);
  CHECK(access(Path, F_OK) == 0);
  CHECK(sem_unlink(ObjectName) == 0);
  CHECK(access(Path, F_OK) == -1);
  CHECK(sem_unlink(ObjectName) == -1 && errno == ENOENT);
  CHECK(sem_post(Semaphore) == 0);  // still usable after unlink
  CHECK(sem_close(Semaphore) == 0);
}

static sem_t *SharedSemaphore;
static void PostShared(void) { CHECK(sem_post(SharedSemaphore) == 0); }

TEST(thread, sem_init_pshared_fork) {
  SharedSemaphore = mmap(NULL, sizeof(sem_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  CHECK(SharedSemaphore != MAP_FAILED);
  if (SharedSemaphore == MAP_FAILED) return;
  CHECK(sem_init(SharedSemaphore, 1, 0) == 0);
  const pid_t Child = Fork(PostShared);
  CHECK(TimedSemWait(SharedSemaphore, 3000) == 0);
  JOIN(Child);
  int Value = -1;
  CHECK(sem_getvalue(SharedSemaphore, &Value) == 0 && Value == 0);
  CHECK(sem_destroy(SharedSemaphore) == 0);
}

static char *ShmView;
static void WriteShm(void) {
  strcpy(ShmView, "from child");
  // A fresh mapping in the child shares the object too.
  const int Fd = shm_open(ObjectName, O_RDWR, 0);
  CHECK(Fd >= 0);
  CHECK(pwrite(Fd, "Z", 1, 100) == 1);
  close(Fd);
}

TEST(thread, shm_open_mmap_fork) {
  NameObject("shm");
  const int Fd = shm_open(ObjectName, O_RDWR | O_CREAT | O_EXCL, 0600);
  CHECK(Fd >= 0);
  if (Fd < 0) return;
  CHECK(ftruncate(Fd, 2 * PAGE) == 0);
  ShmView = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, Fd, 0);
  CHECK(ShmView != MAP_FAILED);
  if (ShmView == MAP_FAILED) return;
  JOIN(Fork(WriteShm));
  CHECK(strcmp(ShmView, "from child") == 0);
  CHECK(ShmView[100] == 'Z');
  struct stat Status;
  CHECK(fstat(Fd, &Status) == 0 && Status.st_size == (off_t)(2 * PAGE) && (Status.st_mode & 07777) == 0600);
  CHECK(shm_unlink(ObjectName) == 0);
  CHECK(shm_open(ObjectName, O_RDWR, 0) == -1 && errno == ENOENT);
  CHECK(strcmp(ShmView, "from child") == 0);
  close(Fd);
}

static void UnlinkOthersFile(void) {
  if (setgroups(0, NULL) || setresgid(1000, 1000, 1000) || setresuid(1000, 1000, 1000)) {
    printf("#   cannot switch to uid 1000: %s\n", ErrnoName(errno));
    ++CaseFailures;
    return;
  }
  // glibc turns the sticky-directory EPERM into EACCES.
  CHECK_ERRNO(shm_unlink(ObjectName), EACCES);
  // A file of one's own in the sticky directory can be removed.
  char Own[80];
  snprintf(Own, sizeof(Own), "%s.own", ObjectName);
  const int Fd = shm_open(Own, O_RDWR | O_CREAT | O_EXCL, 0600);
  CHECK(Fd >= 0);
  close(Fd);
  CHECK(shm_unlink(Own) == 0);
}

TEST(thread, dev_shm_sticky) {
  struct stat Status;
  CHECK(stat("/dev/shm", &Status) == 0);
  CHECK(S_ISDIR(Status.st_mode));
  CHECK((Status.st_mode & 07777) == 01777);
  if (geteuid() != 0) return;
  NameObject("sticky");
  const int Fd = shm_open(ObjectName, O_RDWR | O_CREAT | O_EXCL, 0666);
  CHECK(Fd >= 0);
  close(Fd);
  JOIN(Fork(UnlinkOthersFile));
  CHECK(shm_unlink(ObjectName) == 0);
}

// ---------------------------------------------------------------------------
// ipc/: System V IPC, POSIX message queues, pipes

union SemArgument {
  int Value;
  struct semid_ds *Status;
  unsigned short *Array;
  struct seminfo *Info;
};

static int ShmId = -1;
static char *ShmAddress;

static void WriteSegment(void) {
  // The attachment is inherited by fork.
  strcpy(ShmAddress, "child");
  struct shmid_ds Status;
  CHECK(shmctl(ShmId, IPC_STAT, &Status) == 0 && Status.shm_nattch == 2);
}

TEST(ipc, shm_create_attach_fork) {
  ShmId = shmget(IPC_PRIVATE, 64 * 1024, IPC_CREAT | 0600);
  CHECK(ShmId >= 0);
  if (ShmId < 0) return;
  ShmAddress = shmat(ShmId, NULL, 0);
  CHECK(ShmAddress != (void *)-1);
  if (ShmAddress == (void *)-1) {
    shmctl(ShmId, IPC_RMID, NULL);
    return;
  }
  CHECK(ShmAddress[0] == 0 && ShmAddress[64 * 1024 - 1] == 0);
  JOIN(Fork(WriteSegment));
  CHECK(strcmp(ShmAddress, "child") == 0);
  struct shmid_ds Status;
  CHECK(shmctl(ShmId, IPC_STAT, &Status) == 0 && Status.shm_nattch == 1);
  CHECK(shmdt(ShmAddress) == 0);
  CHECK_ERRNO(shmdt(ShmAddress), EINVAL);
  // Read-only attach.
  const char *ReadOnly = shmat(ShmId, NULL, SHM_RDONLY);
  CHECK(ReadOnly != (void *)-1 && strcmp(ReadOnly, "child") == 0);
  CHECK(shmdt(ReadOnly) == 0);
  CHECK(shmctl(ShmId, IPC_RMID, NULL) == 0);
  CHECK_ERRNO(shmget(IPC_PRIVATE, 0, IPC_CREAT | 0600), EINVAL);
}

TEST(ipc, shm_rmid_lifetime) {
  ShmId = shmget(IPC_PRIVATE, PAGE, IPC_CREAT | 0600);
  CHECK(ShmId >= 0);
  if (ShmId < 0) return;
  char *Address = shmat(ShmId, NULL, 0);
  CHECK(Address != (void *)-1);
  strcpy(Address, "alive");
  CHECK(shmctl(ShmId, IPC_RMID, NULL) == 0);
  // Marked for destruction: still attached and readable.
  struct shmid_ds Status;
  CHECK(shmctl(ShmId, IPC_STAT, &Status) == 0);
  CHECK(Status.shm_perm.mode & SHM_DEST);
  CHECK(Status.shm_perm.__key == IPC_PRIVATE);
  CHECK(strcmp(Address, "alive") == 0);
  CHECK(shmdt(Address) == 0);
  CHECK_ERRNO(shmctl(ShmId, IPC_STAT, &Status), EINVAL);
  CHECK_ERRNO(shmat(ShmId, NULL, 0), EINVAL);
}

TEST(ipc, shm_double_attach_enomem) {
  ShmId = shmget(IPC_PRIVATE, PAGE, IPC_CREAT | 0600);
  CHECK(ShmId >= 0);
  if (ShmId < 0) return;
  char *First = shmat(ShmId, NULL, 0);
  CHECK(First != (void *)-1);
  if (GuestRuntime) {
    // Deliberate difference: one address per segment (no double mapping).
    CHECK_ERRNO(shmat(ShmId, NULL, 0), ENOMEM);
  } else {
    char *Second = shmat(ShmId, NULL, 0);
    CHECK(Second != (void *)-1 && Second != First);
    if (Second != (void *)-1) {
      First[0] = 'x';
      CHECK(Second[0] == 'x');
      CHECK(shmdt(Second) == 0);
    }
  }
  CHECK(shmdt(First) == 0);
  CHECK(shmctl(ShmId, IPC_RMID, NULL) == 0);
}

TEST(ipc, shm_stat_info) {
  ShmId = shmget(IPC_PRIVATE, 3 * PAGE + 1, IPC_CREAT | 0640);
  CHECK(ShmId >= 0);
  if (ShmId < 0) return;
  struct shmid_ds Status;
  CHECK(shmctl(ShmId, IPC_STAT, &Status) == 0);
  CHECK(Status.shm_segsz == 3 * PAGE + 1);
  CHECK(Status.shm_nattch == 0);
  CHECK(Status.shm_cpid == getpid());
  CHECK((Status.shm_perm.mode & 0777) == 0640);
  CHECK(Status.shm_perm.uid == geteuid() && Status.shm_perm.cuid == geteuid());
  struct shminfo Info;
  CHECK(shmctl(0, IPC_INFO, (struct shmid_ds *)&Info) >= 0);
  CHECK(Info.shmmax > 0 && Info.shmmni > 0 && Info.shmmin == 1);
  struct shm_info Usage;
  CHECK(shmctl(0, SHM_INFO, (struct shmid_ds *)&Usage) >= 0);
  CHECK(Usage.used_ids >= 1);
  Status.shm_perm.mode = 0600;
  CHECK(shmctl(ShmId, IPC_SET, &Status) == 0);
  CHECK(shmctl(ShmId, IPC_STAT, &Status) == 0 && (Status.shm_perm.mode & 0777) == 0600);
  CHECK(shmctl(ShmId, IPC_RMID, NULL) == 0);
  CHECK_ERRNO(shmctl(ShmId, 99, &Status), EINVAL);
}

static int SemId = -1;

TEST(ipc, sem_basic) {
  SemId = semget(IPC_PRIVATE, 2, IPC_CREAT | 0600);
  CHECK(SemId >= 0);
  if (SemId < 0) return;
  CHECK(semctl(SemId, 0, SETVAL, (union SemArgument) {.Value = 2}) == 0);
  CHECK(semctl(SemId, 0, GETVAL) == 2);
  struct sembuf Ops[2] = {{0, -1, 0}, {1, 3, 0}};
  CHECK(semop(SemId, Ops, 2) == 0);
  CHECK(semctl(SemId, 0, GETVAL) == 1 && semctl(SemId, 1, GETVAL) == 3);
  CHECK(semctl(SemId, 0, GETPID) == getpid());
  // All-or-nothing: the second operation would block, so nothing changes.
  struct sembuf Atomic[2] = {{1, -1, 0}, {0, -2, IPC_NOWAIT}};
  CHECK_ERRNO(semop(SemId, Atomic, 2), EAGAIN);
  CHECK(semctl(SemId, 1, GETVAL) == 3);
  // Wait-for-zero.
  struct sembuf Zero = {0, 0, IPC_NOWAIT};
  CHECK_ERRNO(semop(SemId, &Zero, 1), EAGAIN);
  unsigned short Values[2] = {0, 7};
  CHECK(semctl(SemId, 0, SETALL, (union SemArgument) {.Array = Values}) == 0);
  CHECK(semop(SemId, &Zero, 1) == 0);
  unsigned short Read[2] = {9, 9};
  CHECK(semctl(SemId, 0, GETALL, (union SemArgument) {.Array = Read}) == 0 && Read[0] == 0 && Read[1] == 7);
  struct sembuf OutOfRange = {2, 1, 0};
  CHECK_ERRNO(semop(SemId, &OutOfRange, 1), EFBIG);
  struct semid_ds Status;
  CHECK(semctl(SemId, 0, IPC_STAT, (union SemArgument) {.Status = &Status}) == 0 && Status.sem_nsems == 2);
  CHECK(semctl(SemId, 0, IPC_RMID) == 0);
}

static void UndoIncrement(void) {
  struct sembuf Op = {0, 5, SEM_UNDO};
  CHECK(semop(SemId, &Op, 1) == 0);
  CHECK(semctl(SemId, 0, GETVAL) == 5);
}

TEST(ipc, sem_undo_on_exit) {
  SemId = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
  CHECK(SemId >= 0);
  if (SemId < 0) return;
  JOIN(Fork(UndoIncrement));
  CHECK(semctl(SemId, 0, GETVAL) == 0);
  // SETVAL clears pending adjustments.
  struct sembuf Op = {0, 2, SEM_UNDO};
  CHECK(semop(SemId, &Op, 1) == 0);
  CHECK(semctl(SemId, 0, SETVAL, (union SemArgument) {.Value = 4}) == 0);
  CHECK(semctl(SemId, 0, IPC_RMID) == 0);
}

TEST(ipc, semtimedop_eagain) {
  SemId = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
  CHECK(SemId >= 0);
  if (SemId < 0) return;
  struct sembuf Op = {0, -1, 0};
  const struct timespec Timeout = {0, 50 * 1000000};
  const uint64_t Start = NowMs();
  CHECK_ERRNO(semtimedop(SemId, &Op, 1, &Timeout), EAGAIN);
  CHECK(NowMs() - Start >= 49);
  const struct timespec Bad = {0, -1};
  CHECK_ERRNO(semtimedop(SemId, &Op, 1, &Bad), EINVAL);
  CHECK(semctl(SemId, 0, IPC_RMID) == 0);
}

static void BlockOnSemaphore(void) {
  struct sembuf Op = {0, -1, 0};
  CHECK_ERRNO(semop(SemId, &Op, 1), EIDRM);
}

TEST(ipc, sem_rmid_eidrm) {
  SemId = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
  CHECK(SemId >= 0);
  if (SemId < 0) return;
  const pid_t Child = Fork(BlockOnSemaphore);
  WAIT_UNTIL(semctl(SemId, 0, GETNCNT) == 1);
  CHECK(semctl(SemId, 0, GETNCNT) == 1);
  CHECK(semctl(SemId, 0, IPC_RMID) == 0);
  JOIN(Child);
  CHECK_ERRNO(semctl(SemId, 0, GETVAL), EINVAL);
}

TEST(ipc, sem_setall_erange) {
  SemId = semget(IPC_PRIVATE, 2, IPC_CREAT | 0600);
  CHECK(SemId >= 0);
  if (SemId < 0) return;
  CHECK_ERRNO(semctl(SemId, 0, SETVAL, (union SemArgument) {.Value = 32768}), ERANGE);
  CHECK_ERRNO(semctl(SemId, 0, SETVAL, (union SemArgument) {.Value = -1}), ERANGE);
  CHECK(semctl(SemId, 0, SETVAL, (union SemArgument) {.Value = 32767}) == 0);
  unsigned short Values[2] = {1, 32768};
  CHECK_ERRNO(semctl(SemId, 0, SETALL, (union SemArgument) {.Array = Values}), ERANGE);
  CHECK(semctl(SemId, 0, GETVAL) == 32767);
  struct sembuf Op = {0, 1, IPC_NOWAIT};
  CHECK_ERRNO(semop(SemId, &Op, 1), ERANGE);
  CHECK_ERRNO(semget(IPC_PRIVATE, 0, IPC_CREAT | 0600), EINVAL);
  CHECK(semctl(SemId, 0, IPC_RMID) == 0);
}

struct Message {
  long Type;
  char Text[64];
};
static int MsgId = -1;

static int SendMessage(long Type, const char *Text, int Flags) {
  struct Message Message = {Type, {0}};
  snprintf(Message.Text, sizeof(Message.Text), "%s", Text);
  return msgsnd(MsgId, &Message, strlen(Text) + 1, Flags);
}

TEST(ipc, msg_types) {
  MsgId = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
  CHECK(MsgId >= 0);
  if (MsgId < 0) return;
  CHECK(SendMessage(3, "three", 0) == 0);
  CHECK(SendMessage(1, "one", 0) == 0);
  CHECK(SendMessage(2, "two", 0) == 0);
  CHECK(SendMessage(1, "one again", 0) == 0);
  struct Message Out;
  CHECK(msgrcv(MsgId, &Out, sizeof(Out.Text), 2, 0) == 4 && Out.Type == 2 && strcmp(Out.Text, "two") == 0);
  // Negative: the lowest type at most |msgtyp|.
  CHECK(msgrcv(MsgId, &Out, sizeof(Out.Text), -3, 0) == 4 && Out.Type == 1 && strcmp(Out.Text, "one") == 0);
  CHECK(msgrcv(MsgId, &Out, sizeof(Out.Text), 1, MSG_EXCEPT) == 6 && Out.Type == 3);
  CHECK(msgrcv(MsgId, &Out, sizeof(Out.Text), 0, 0) == 10 && strcmp(Out.Text, "one again") == 0);
  CHECK_ERRNO(msgrcv(MsgId, &Out, sizeof(Out.Text), 0, IPC_NOWAIT), ENOMSG);
  CHECK_ERRNO(SendMessage(0, "zero", 0), EINVAL);
  struct msqid_ds Status;
  CHECK(msgctl(MsgId, IPC_STAT, &Status) == 0 && Status.msg_qnum == 0 && Status.msg_lrpid == getpid());
  CHECK(msgctl(MsgId, IPC_RMID, NULL) == 0);
  CHECK_ERRNO(msgrcv(MsgId, &Out, sizeof(Out.Text), 0, IPC_NOWAIT), EINVAL);
}

TEST(ipc, msg_noerror_e2big) {
  MsgId = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
  CHECK(MsgId >= 0);
  if (MsgId < 0) return;
  CHECK(SendMessage(1, "a long message", 0) == 0);
  struct Message Out;
  CHECK_ERRNO(msgrcv(MsgId, &Out, 4, 0, IPC_NOWAIT), E2BIG);
  memset(Out.Text, 0, sizeof(Out.Text));
  CHECK(msgrcv(MsgId, &Out, 4, 0, IPC_NOWAIT | MSG_NOERROR) == 4);
  CHECK(memcmp(Out.Text, "a lo", 4) == 0 && Out.Text[4] == 0);
  CHECK_ERRNO(msgrcv(MsgId, &Out, 4, 0, IPC_NOWAIT), ENOMSG);
  CHECK_ERRNO(msgrcv(MsgId, &Out, -1, 0, IPC_NOWAIT), EINVAL);
  CHECK(msgctl(MsgId, IPC_RMID, NULL) == 0);
}

TEST(ipc, msg_copy) {
  MsgId = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
  CHECK(MsgId >= 0);
  if (MsgId < 0) return;
  CHECK(SendMessage(5, "first", 0) == 0);
  CHECK(SendMessage(6, "second", 0) == 0);
  struct Message Out;
  CHECK(msgrcv(MsgId, &Out, sizeof(Out.Text), 1, MSG_COPY | IPC_NOWAIT) == 7 && Out.Type == 6);
  CHECK(msgrcv(MsgId, &Out, sizeof(Out.Text), 0, MSG_COPY | IPC_NOWAIT) == 6 && Out.Type == 5);
  CHECK_ERRNO(msgrcv(MsgId, &Out, sizeof(Out.Text), 2, MSG_COPY | IPC_NOWAIT), ENOMSG);
  CHECK_ERRNO(msgrcv(MsgId, &Out, sizeof(Out.Text), 0, MSG_COPY), EINVAL);
  CHECK_ERRNO(msgrcv(MsgId, &Out, sizeof(Out.Text), 0, MSG_COPY | MSG_EXCEPT | IPC_NOWAIT), EINVAL);
  struct msqid_ds Status;
  CHECK(msgctl(MsgId, IPC_STAT, &Status) == 0 && Status.msg_qnum == 2);
  CHECK(msgctl(MsgId, IPC_RMID, NULL) == 0);
}

static void BlockedSend(void) {
  char Text[61];
  memset(Text, 'b', 60);
  Text[60] = 0;
  CHECK(SendMessage(2, Text, 0) == 0);
}

TEST(ipc, msg_qbytes_block) {
  MsgId = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
  CHECK(MsgId >= 0);
  if (MsgId < 0) return;
  struct msqid_ds Status;
  CHECK(msgctl(MsgId, IPC_STAT, &Status) == 0);
  Status.msg_qbytes = 100;
  CHECK(msgctl(MsgId, IPC_SET, &Status) == 0);
  char Text[61];
  memset(Text, 'a', 60);
  Text[60] = 0;
  CHECK(SendMessage(1, Text, 0) == 0);  // 61 bytes queued
  CHECK_ERRNO(SendMessage(1, Text, IPC_NOWAIT), EAGAIN);
  const pid_t Child = Fork(BlockedSend);
  SleepMs(100);
  int Status_ = 0;
  CHECK(waitpid(Child, &Status_, WNOHANG) == 0);  // still blocked
  struct Message Out;
  CHECK(msgrcv(MsgId, &Out, sizeof(Out.Text), 1, 0) == 61);
  JOIN(Child);
  CHECK(msgrcv(MsgId, &Out, sizeof(Out.Text), 2, IPC_NOWAIT) == 61 && Out.Text[0] == 'b');
  CHECK(msgctl(MsgId, IPC_RMID, NULL) == 0);
}

static mqd_t OpenQueue(int Flags, long MaxMessages, long MessageSize) {
  struct mq_attr Attributes = {.mq_maxmsg = MaxMessages, .mq_msgsize = MessageSize};
  return mq_open(ObjectName, Flags, 0600, MaxMessages ? &Attributes : NULL);
}

TEST(ipc, mq_send_receive_priority) {
  NameObject("mq");
  const mqd_t Queue = OpenQueue(O_RDWR | O_CREAT | O_EXCL, 8, 64);
  CHECK(Queue != (mqd_t)-1);
  if (Queue == (mqd_t)-1) return;
  CHECK(mq_send(Queue, "low", 4, 1) == 0);
  CHECK(mq_send(Queue, "high", 5, 5) == 0);
  CHECK(mq_send(Queue, "mid", 4, 3) == 0);
  CHECK(mq_send(Queue, "mid2", 5, 3) == 0);
  char Buffer[64];
  unsigned Priority = 0;
  CHECK(mq_receive(Queue, Buffer, sizeof(Buffer), &Priority) == 5 && Priority == 5 && strcmp(Buffer, "high") == 0);
  CHECK(mq_receive(Queue, Buffer, sizeof(Buffer), &Priority) == 4 && Priority == 3 && strcmp(Buffer, "mid") == 0);
  CHECK(mq_receive(Queue, Buffer, sizeof(Buffer), &Priority) == 5 && Priority == 3 && strcmp(Buffer, "mid2") == 0);
  CHECK(mq_receive(Queue, Buffer, sizeof(Buffer), &Priority) == 4 && Priority == 1);
  CHECK_ERRNO(mq_send(Queue, Buffer, 65, 0), EMSGSIZE);
  CHECK_ERRNO(mq_receive(Queue, Buffer, 63, NULL), EMSGSIZE);
  CHECK_ERRNO(mq_send(Queue, "x", 1, 32768), EINVAL);
  CHECK(mq_close(Queue) == 0);
  CHECK(mq_unlink(ObjectName) == 0);
}

TEST(ipc, mq_timedreceive_timeout) {
  NameObject("mqt");
  const mqd_t Queue = OpenQueue(O_RDWR | O_CREAT | O_EXCL, 2, 16);
  CHECK(Queue != (mqd_t)-1);
  if (Queue == (mqd_t)-1) return;
  struct timespec Until;
  clock_gettime(CLOCK_REALTIME, &Until);
  Until.tv_nsec += 50 * 1000000;
  if (Until.tv_nsec >= 1000000000) {
    Until.tv_nsec -= 1000000000;
    ++Until.tv_sec;
  }
  char Buffer[16];
  const uint64_t Start = NowMs();
  CHECK_ERRNO(mq_timedreceive(Queue, Buffer, sizeof(Buffer), NULL, &Until), ETIMEDOUT);
  CHECK(NowMs() - Start >= 40);
  // A full queue times out a timed send the same way.
  CHECK(mq_send(Queue, "a", 1, 0) == 0 && mq_send(Queue, "b", 1, 0) == 0);
  clock_gettime(CLOCK_REALTIME, &Until);
  CHECK_ERRNO(mq_timedsend(Queue, "c", 1, 0, &Until), ETIMEDOUT);
  Until.tv_nsec = -1;
  CHECK_ERRNO(mq_timedsend(Queue, "c", 1, 0, &Until), EINVAL);
  CHECK(mq_close(Queue) == 0);
  CHECK(mq_unlink(ObjectName) == 0);
}

static mqd_t NotifyQueue;
static void NotifyBusy(void) {
  const struct sigevent Event = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGUSR2};
  CHECK_ERRNO(mq_notify(NotifyQueue, &Event), EBUSY);
}

TEST(ipc, mq_notify_signal) {
  NameObject("mqn");
  NotifyQueue = OpenQueue(O_RDWR | O_CREAT | O_EXCL, 4, 16);
  CHECK(NotifyQueue != (mqd_t)-1);
  if (NotifyQueue == (mqd_t)-1) return;
  sigset_t Set;
  sigemptyset(&Set);
  sigaddset(&Set, SIGUSR1);
  sigprocmask(SIG_BLOCK, &Set, NULL);
  struct sigevent Event = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGUSR1};
  Event.sigev_value.sival_int = 42;
  CHECK(mq_notify(NotifyQueue, &Event) == 0);
  CHECK_ERRNO(mq_notify(NotifyQueue, &Event), EBUSY);
  JOIN(Fork(NotifyBusy));
  CHECK(mq_send(NotifyQueue, "ping", 5, 0) == 0);
  siginfo_t Info;
  const struct timespec Timeout = {3, 0};
  CHECK(sigtimedwait(&Set, &Info, &Timeout) == SIGUSR1);
  CHECK(Info.si_code == SI_MESGQ);
  CHECK(Info.si_value.sival_int == 42);
  CHECK(Info.si_pid == getpid());
  // The registration is gone after one notification.
  CHECK(mq_notify(NotifyQueue, &Event) == 0);
  CHECK(mq_notify(NotifyQueue, NULL) == 0);
  CHECK(mq_send(NotifyQueue, "ping", 5, 0) == 0);
  const struct timespec Short = {0, 20 * 1000000};
  CHECK(sigtimedwait(&Set, &Info, &Short) == -1 && errno == EAGAIN);
  Event.sigev_notify = SIGEV_SIGNAL;
  Event.sigev_signo = 0;
  CHECK_ERRNO(mq_notify(NotifyQueue, &Event), EINVAL);
  CHECK(mq_close(NotifyQueue) == 0);
  CHECK(mq_unlink(ObjectName) == 0);
}

TEST(ipc, mq_attr_limits) {
  NameObject("mqa");
  CHECK_ERRNO(mq_open(ObjectName, O_RDWR | O_CREAT, 0600, &(struct mq_attr) {.mq_maxmsg = 0, .mq_msgsize = 16}), EINVAL);
  CHECK_ERRNO(mq_open(ObjectName, O_RDWR | O_CREAT, 0600, &(struct mq_attr) {.mq_maxmsg = 4, .mq_msgsize = 0}), EINVAL);
  CHECK_ERRNO(mq_open(ObjectName, O_RDWR | O_CREAT, 0600, &(struct mq_attr) {.mq_maxmsg = 100000, .mq_msgsize = 16}), EINVAL);
  CHECK_ERRNO(mq_open("/", O_RDWR | O_CREAT, 0600, NULL), ENOENT);
  CHECK_ERRNO(mq_open(ObjectName, O_RDWR, 0600, NULL), ENOENT);
  const mqd_t Queue = mq_open(ObjectName, O_RDWR | O_CREAT | O_EXCL, 0600, NULL);
  CHECK(Queue != (mqd_t)-1);
  if (Queue == (mqd_t)-1) return;
  struct mq_attr Attributes;
  CHECK(mq_getattr(Queue, &Attributes) == 0);
  CHECK(Attributes.mq_maxmsg == 10 && Attributes.mq_msgsize == 8192 && Attributes.mq_curmsgs == 0 && Attributes.mq_flags == 0);
  struct mq_attr NonBlocking = {.mq_flags = O_NONBLOCK}, Old;
  CHECK(mq_setattr(Queue, &NonBlocking, &Old) == 0 && Old.mq_flags == 0);
  char Buffer[8192];
  CHECK_ERRNO(mq_receive(Queue, Buffer, sizeof(Buffer), NULL), EAGAIN);
  CHECK(mq_getattr(Queue, &Attributes) == 0 && Attributes.mq_flags == O_NONBLOCK);
  CHECK_ERRNO(mq_open(ObjectName, O_RDWR | O_CREAT | O_EXCL, 0600, NULL), EEXIST);
  CHECK(mq_close(Queue) == 0);
  CHECK(mq_unlink(ObjectName) == 0);
}

TEST(ipc, mq_unlink_lifetime) {
  NameObject("mqu");
  const mqd_t Queue = OpenQueue(O_RDWR | O_CREAT | O_EXCL, 2, 16);
  CHECK(Queue != (mqd_t)-1);
  if (Queue == (mqd_t)-1) return;
  CHECK(mq_unlink(ObjectName) == 0);
  CHECK_ERRNO(mq_unlink(ObjectName), ENOENT);
  CHECK_ERRNO(mq_open(ObjectName, O_RDWR), ENOENT);
  // The open descriptor keeps the queue.
  char Buffer[16];
  CHECK(mq_send(Queue, "kept", 5, 0) == 0);
  CHECK(mq_receive(Queue, Buffer, sizeof(Buffer), NULL) == 5 && strcmp(Buffer, "kept") == 0);
  // A new queue by the same name is a different queue.
  const mqd_t Fresh = OpenQueue(O_RDWR | O_CREAT | O_EXCL, 2, 16);
  CHECK(Fresh != (mqd_t)-1);
  CHECK(mq_send(Queue, "old", 4, 0) == 0);
  struct mq_attr Attributes;
  CHECK(mq_getattr(Fresh, &Attributes) == 0 && Attributes.mq_curmsgs == 0);
  CHECK(mq_close(Queue) == 0);
  CHECK_ERRNO(mq_close(Queue), EBADF);
  CHECK(mq_close(Fresh) == 0);
  CHECK(mq_unlink(ObjectName) == 0);
}

TEST(ipc, pipe_direct_packets) {
  int Pipe[2];
  CHECK(pipe2(Pipe, O_DIRECT | O_NONBLOCK) == 0);
  CHECK(fcntl(Pipe[1], F_GETFL) & O_DIRECT);
  CHECK(write(Pipe[1], "abc", 3) == 3);
  CHECK(write(Pipe[1], "defgh", 5) == 5);
  CHECK(write(Pipe[1], "ij", 2) == 2);
  char Buffer[100];
  CHECK(read(Pipe[0], Buffer, sizeof(Buffer)) == 3 && memcmp(Buffer, "abc", 3) == 0);
  // A short read takes the start of a packet and discards the rest.
  CHECK(read(Pipe[0], Buffer, 2) == 2 && memcmp(Buffer, "de", 2) == 0);
  CHECK(read(Pipe[0], Buffer, sizeof(Buffer)) == 2 && memcmp(Buffer, "ij", 2) == 0);
  CHECK_ERRNO(read(Pipe[0], Buffer, sizeof(Buffer)), EAGAIN);
  // Packet mode can be switched off with fcntl.
  CHECK(fcntl(Pipe[1], F_SETFL, O_NONBLOCK) == 0);
  CHECK(write(Pipe[1], "ab", 2) == 2 && write(Pipe[1], "cd", 2) == 2);
  CHECK(read(Pipe[0], Buffer, sizeof(Buffer)) == 4);
}

static void PipeSizeLimits(void) {
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK(fcntl(Pipe[0], F_GETPIPE_SZ) == 65536);
  CHECK(fcntl(Pipe[0], F_SETPIPE_SZ, 4096) == 4096);
  CHECK(fcntl(Pipe[0], F_SETPIPE_SZ, 5000) == 8192);
  long Maximum = 1048576;
  FILE *File = fopen("/proc/sys/fs/pipe-max-size", "r");
  if (File) {
    if (!ReadLong(File, &Maximum)) Maximum = 1048576;
    fclose(File);
  }
  CHECK(fcntl(Pipe[0], F_SETPIPE_SZ, (int)Maximum) == Maximum);
  CHECK_ERRNO(fcntl(Pipe[0], F_SETPIPE_SZ, (int)Maximum * 2), EPERM);
  CHECK_ERRNO(fcntl(Pipe[0], F_SETPIPE_SZ, -1), EINVAL);
}

TEST(ipc, pipe_setsz_eperm) { AsUser(PipeSizeLimits); }

// ---------------------------------------------------------------------------
// unix/: AF_UNIX sockets

static socklen_t UnixAddress(struct sockaddr_un *Address, const char *Path) {
  memset(Address, 0, sizeof(*Address));
  Address->sun_family = AF_UNIX;
  if (Path[0] == '@') {
    // Abstract name: a leading NUL, not terminated.
    memcpy(Address->sun_path + 1, Path + 1, strlen(Path + 1));
    return offsetof(struct sockaddr_un, sun_path) + strlen(Path);
  }
  memcpy(Address->sun_path, Path, strlen(Path));
  return offsetof(struct sockaddr_un, sun_path) + strlen(Path) + 1;
}

static int UnixSocket(int Type, const char *Path) {
  const int Fd = socket(AF_UNIX, Type, 0);
  if (Fd < 0 || !Path) return Fd;
  struct sockaddr_un Address;
  const socklen_t Length = UnixAddress(&Address, Path);
  if (bind(Fd, (struct sockaddr *)&Address, Length)) {
    close(Fd);
    return -1;
  }
  return Fd;
}

static int UnixConnect(int Fd, const char *Path) {
  struct sockaddr_un Address;
  const socklen_t Length = UnixAddress(&Address, Path);
  return connect(Fd, (struct sockaddr *)&Address, Length);
}

TEST(unix, stream_path_bind_connect) {
  const int Server = UnixSocket(SOCK_STREAM, "stream.sock");
  CHECK(Server >= 0);
  CHECK(listen(Server, 4) == 0);
  struct stat Status;
  CHECK(stat("stream.sock", &Status) == 0 && S_ISSOCK(Status.st_mode));
  const int Client = socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(UnixConnect(Client, "stream.sock") == 0);
  const int Accepted = accept(Server, NULL, NULL);
  CHECK(Accepted >= 0);
  CHECK(write(Client, "hello", 5) == 5);
  char Buffer[8] = {0};
  CHECK(read(Accepted, Buffer, sizeof(Buffer)) == 5 && strcmp(Buffer, "hello") == 0);
  CHECK(write(Accepted, "back", 4) == 4);
  CHECK(read(Client, Buffer, sizeof(Buffer)) == 4 && memcmp(Buffer, "back", 4) == 0);
  CHECK_ERRNO(UnixConnect(Client, "stream.sock"), EISCONN);
  CHECK_ERRNO(UnixConnect(socket(AF_UNIX, SOCK_STREAM, 0), "missing.sock"), ENOENT);
  CHECK_ERRNO(UnixConnect(socket(AF_UNIX, SOCK_DGRAM, 0), "stream.sock"), EPROTOTYPE);
  // A listening socket is not connected.
  CHECK_ERRNO(write(Server, "x", 1), ENOTCONN);
  CHECK(unlink("stream.sock") == 0);
}

TEST(unix, bind_eaddrinuse) {
  const int First = UnixSocket(SOCK_STREAM, "inuse.sock");
  CHECK(First >= 0);
  const int Second = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un Address;
  socklen_t Length = UnixAddress(&Address, "inuse.sock");
  CHECK_ERRNO(bind(Second, (struct sockaddr *)&Address, Length), EADDRINUSE);
  // A bound socket cannot be bound again.
  Length = UnixAddress(&Address, "other.sock");
  CHECK_ERRNO(bind(First, (struct sockaddr *)&Address, Length), EINVAL);
  Length = UnixAddress(&Address, "@comm-suite-inuse");
  CHECK(bind(Second, (struct sockaddr *)&Address, Length) == 0);
  // Abstract names are per socket type: a datagram socket may reuse it.
  const int Other = socket(AF_UNIX, SOCK_DGRAM, 0);
  CHECK(bind(Other, (struct sockaddr *)&Address, Length) == 0);
  const int Third = socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK_ERRNO(bind(Third, (struct sockaddr *)&Address, Length), EADDRINUSE);
  CHECK_ERRNO(bind(Third, (struct sockaddr *)&Address, 1), EINVAL);
}

TEST(unix, connect_stale_econnrefused) {
  int Fd = UnixSocket(SOCK_STREAM, "stale.sock");
  CHECK(Fd >= 0);
  close(Fd);
  CHECK(access("stale.sock", F_OK) == 0);
  CHECK_ERRNO(UnixConnect(socket(AF_UNIX, SOCK_STREAM, 0), "stale.sock"), ECONNREFUSED);
  // Bound but not listening is refused too.
  Fd = UnixSocket(SOCK_STREAM, "quiet.sock");
  CHECK(Fd >= 0);
  CHECK_ERRNO(UnixConnect(socket(AF_UNIX, SOCK_STREAM, 0), "quiet.sock"), ECONNREFUSED);
  // A regular file at the path.
  CHECK(close(open("plain.sock", O_CREAT | O_WRONLY, 0600)) == 0);
  CHECK_ERRNO(UnixConnect(socket(AF_UNIX, SOCK_STREAM, 0), "plain.sock"), ECONNREFUSED);
}

TEST(unix, abstract_names) {
  char Name[64];
  snprintf(Name, sizeof(Name), "@comm-suite-abstract-%d", getpid());
  const int Server = UnixSocket(SOCK_STREAM, Name);
  CHECK(Server >= 0);
  CHECK(listen(Server, 1) == 0);
  struct sockaddr_un Address;
  socklen_t Length = sizeof(Address);
  CHECK(getsockname(Server, (struct sockaddr *)&Address, &Length) == 0);
  CHECK(Length == offsetof(struct sockaddr_un, sun_path) + strlen(Name));
  CHECK(Address.sun_path[0] == 0 && memcmp(Address.sun_path + 1, Name + 1, strlen(Name) - 1) == 0);
  const int Client = socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(UnixConnect(Client, Name) == 0);
  // Abstract names are not in the filesystem.
  CHECK(access(Name + 1, F_OK) == -1);
  close(Server);
  CHECK_ERRNO(UnixConnect(socket(AF_UNIX, SOCK_STREAM, 0), Name), ECONNREFUSED);
  // Closing the last socket frees the name.
  CHECK(UnixSocket(SOCK_STREAM, Name) >= 0);
}

TEST(unix, autobind) {
  const int Fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  struct sockaddr_un Address = {.sun_family = AF_UNIX};
  CHECK(bind(Fd, (struct sockaddr *)&Address, sizeof(sa_family_t)) == 0);
  socklen_t Length = sizeof(Address);
  CHECK(getsockname(Fd, (struct sockaddr *)&Address, &Length) == 0);
  CHECK(Length == offsetof(struct sockaddr_un, sun_path) + 6);
  CHECK(Address.sun_path[0] == 0);
  int Hex = 1;
  for (int Index = 1; Index <= 5; ++Index) Hex = Hex && strchr("0123456789abcdef", Address.sun_path[Index]);
  CHECK(Hex);
  // An unbound datagram socket with SO_PASSCRED autobinds when it sends.
  const int Sender = socket(AF_UNIX, SOCK_DGRAM, 0);
  const int On = 1;
  CHECK(setsockopt(Sender, SOL_SOCKET, SO_PASSCRED, &On, sizeof(On)) == 0);
  CHECK(sendto(Sender, "x", 1, 0, (struct sockaddr *)&Address, Length) == 1);
  struct sockaddr_un Source;
  socklen_t SourceLength = sizeof(Source);
  char Byte;
  CHECK(recvfrom(Fd, &Byte, 1, 0, (struct sockaddr *)&Source, &SourceLength) == 1);
  CHECK(SourceLength == offsetof(struct sockaddr_un, sun_path) + 6 && Source.sun_path[0] == 0);
}

TEST(unix, dgram_boundaries) {
  int Pair[2];
  CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, Pair) == 0);
  CHECK(send(Pair[0], "one", 3, 0) == 3);
  CHECK(send(Pair[0], "", 0, 0) == 0);
  CHECK(send(Pair[0], "three", 5, 0) == 5);
  char Buffer[16];
  CHECK(recv(Pair[1], Buffer, sizeof(Buffer), 0) == 3);
  CHECK(recv(Pair[1], Buffer, sizeof(Buffer), 0) == 0);
  CHECK(recv(Pair[1], Buffer, sizeof(Buffer), 0) == 5 && memcmp(Buffer, "three", 5) == 0);
  CHECK_ERRNO(recv(Pair[1], Buffer, sizeof(Buffer), MSG_DONTWAIT), EAGAIN);
  int Pending = -1;
  CHECK(send(Pair[0], "four", 4, 0) == 4 && send(Pair[0], "fivefive", 8, 0) == 8);
  CHECK(ioctl(Pair[1], FIONREAD, &Pending) == 0 && Pending == 4);
}

TEST(unix, dgram_trunc) {
  int Pair[2];
  CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, Pair) == 0);
  CHECK(send(Pair[0], "truncated", 9, 0) == 9);
  char Buffer[4];
  struct iovec Vector = {Buffer, sizeof(Buffer)};
  struct msghdr Header = {.msg_iov = &Vector, .msg_iovlen = 1};
  CHECK(recvmsg(Pair[1], &Header, 0) == 4);
  CHECK(Header.msg_flags & MSG_TRUNC);
  CHECK(memcmp(Buffer, "trun", 4) == 0);
  // MSG_TRUNC returns the real length; the rest of the record is gone.
  CHECK(send(Pair[0], "abcdefgh", 8, 0) == 8);
  CHECK(recv(Pair[1], Buffer, sizeof(Buffer), MSG_TRUNC) == 8);
  CHECK_ERRNO(recv(Pair[1], Buffer, sizeof(Buffer), MSG_DONTWAIT), EAGAIN);
  // MSG_PEEK keeps the record.
  CHECK(send(Pair[0], "peeked", 6, 0) == 6);
  CHECK(recv(Pair[1], Buffer, sizeof(Buffer), MSG_PEEK | MSG_TRUNC) == 6);
  CHECK(recv(Pair[1], Buffer, sizeof(Buffer), 0) == 4);
}

TEST(unix, dgram_connect_default_peer) {
  const int A = UnixSocket(SOCK_DGRAM, "a.sock"), B = UnixSocket(SOCK_DGRAM, "b.sock"), C = UnixSocket(SOCK_DGRAM, "c.sock");
  CHECK(A >= 0 && B >= 0 && C >= 0);
  CHECK_ERRNO(send(A, "x", 1, 0), ENOTCONN);
  CHECK(UnixConnect(A, "b.sock") == 0);
  CHECK(send(A, "to b", 4, 0) == 4);
  char Buffer[8];
  struct sockaddr_un Source;
  socklen_t Length = sizeof(Source);
  CHECK(recvfrom(B, Buffer, sizeof(Buffer), 0, (struct sockaddr *)&Source, &Length) == 4);
  CHECK(strcmp(Source.sun_path, "a.sock") == 0);
  // A connected datagram socket takes data only from its peer.
  struct sockaddr_un Target;
  const socklen_t TargetLength = UnixAddress(&Target, "a.sock");
  CHECK_ERRNO(sendto(C, "x", 1, 0, (struct sockaddr *)&Target, TargetLength), EPERM);
  CHECK(sendto(B, "ok", 2, 0, (struct sockaddr *)&Target, TargetLength) == 2);
  CHECK(recv(A, Buffer, sizeof(Buffer), 0) == 2);
  // AF_UNSPEC dissolves the association.
  struct sockaddr Unspecified = {.sa_family = AF_UNSPEC};
  CHECK(connect(A, &Unspecified, sizeof(Unspecified)) == 0);
  CHECK(sendto(C, "x", 1, 0, (struct sockaddr *)&Target, TargetLength) == 1);
  CHECK_ERRNO(send(A, "x", 1, 0), ENOTCONN);
}

TEST(unix, seqpacket_eor) {
  int Pair[2];
  CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, Pair) == 0);
  CHECK(send(Pair[0], "one", 3, MSG_EOR) == 3);
  CHECK(send(Pair[0], "twotwo", 6, 0) == 6);
  CHECK(send(Pair[0], "three", 5, MSG_EOR) == 5);
  char Buffer[16];
  CHECK(recv(Pair[1], Buffer, sizeof(Buffer), 0) == 3);
  struct iovec Vector = {Buffer, 2};
  struct msghdr Header = {.msg_iov = &Vector, .msg_iovlen = 1};
  CHECK(recvmsg(Pair[1], &Header, 0) == 2);
  CHECK(Header.msg_flags & MSG_TRUNC);
  CHECK(recv(Pair[1], Buffer, sizeof(Buffer), 0) == 5 && memcmp(Buffer, "three", 5) == 0);
  // Connection semantics: listen, accept, and EOF at close.
  const int Server = UnixSocket(SOCK_SEQPACKET, "seq.sock");
  CHECK(Server >= 0 && listen(Server, 1) == 0);
  const int Client = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  CHECK(UnixConnect(Client, "seq.sock") == 0);
  const int Accepted = accept(Server, NULL, NULL);
  CHECK(Accepted >= 0);
  CHECK(send(Client, "rec", 3, 0) == 3);
  CHECK(recv(Accepted, Buffer, sizeof(Buffer), 0) == 3);
  close(Client);
  CHECK(recv(Accepted, Buffer, sizeof(Buffer), 0) == 0);
}

TEST(unix, getsockname_getpeername) {
  const int Server = UnixSocket(SOCK_STREAM, "names.sock");
  CHECK(Server >= 0 && listen(Server, 1) == 0);
  const int Client = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un Address;
  socklen_t Length = sizeof(Address);
  CHECK_ERRNO(getpeername(Client, (struct sockaddr *)&Address, &Length), ENOTCONN);
  CHECK(getsockname(Client, (struct sockaddr *)&Address, &Length) == 0 && Length == sizeof(sa_family_t));
  CHECK(UnixConnect(Client, "names.sock") == 0);
  Length = sizeof(Address);
  CHECK(getpeername(Client, (struct sockaddr *)&Address, &Length) == 0);
  CHECK(Length == offsetof(struct sockaddr_un, sun_path) + sizeof("names.sock"));
  CHECK(strcmp(Address.sun_path, "names.sock") == 0);
  Length = sizeof(Address);
  const int Accepted = accept(Server, (struct sockaddr *)&Address, &Length);
  CHECK(Accepted >= 0 && Length == sizeof(sa_family_t));
  Length = sizeof(Address);
  CHECK(getsockname(Accepted, (struct sockaddr *)&Address, &Length) == 0 && strcmp(Address.sun_path, "names.sock") == 0);
  Length = sizeof(Address);
  CHECK(getpeername(Accepted, (struct sockaddr *)&Address, &Length) == 0 && Length == sizeof(sa_family_t));
  // A short buffer gets a truncated address and the full length.
  Length = 4;
  CHECK(getsockname(Server, (struct sockaddr *)&Address, &Length) == 0 && Length == offsetof(struct sockaddr_un, sun_path) + sizeof("names.sock"));
  CHECK_ERRNO(getsockname(0 + open("/dev/null", O_RDONLY), (struct sockaddr *)&Address, &Length), ENOTSOCK);
}

static int GetInt(int Fd, int Level, int Name) {
  int Value = -12345;
  socklen_t Length = sizeof(Value);
  if (getsockopt(Fd, Level, Name, &Value, &Length) || Length != sizeof(Value)) return -12345;
  return Value;
}

TEST(unix, sockopt_basic) {
  const int Server = UnixSocket(SOCK_STREAM, "opts.sock");
  CHECK(Server >= 0 && listen(Server, 1) == 0);
  CHECK(GetInt(Server, SOL_SOCKET, SO_TYPE) == SOCK_STREAM);
  CHECK(GetInt(Server, SOL_SOCKET, SO_DOMAIN) == AF_UNIX);
  CHECK(GetInt(Server, SOL_SOCKET, SO_PROTOCOL) == 0);
  CHECK(GetInt(Server, SOL_SOCKET, SO_ACCEPTCONN) == 1);
  CHECK(GetInt(Server, SOL_SOCKET, SO_ERROR) == 0);
  const int Size = 10000;
  CHECK(setsockopt(Server, SOL_SOCKET, SO_SNDBUF, &Size, sizeof(Size)) == 0);
  CHECK(GetInt(Server, SOL_SOCKET, SO_SNDBUF) == 2 * Size);
  const int Dgram = socket(AF_UNIX, SOCK_DGRAM, 0);
  CHECK(GetInt(Dgram, SOL_SOCKET, SO_TYPE) == SOCK_DGRAM && GetInt(Dgram, SOL_SOCKET, SO_ACCEPTCONN) == 0);
  int Value = 0;
  socklen_t Length = sizeof(Value);
  CHECK_ERRNO(getsockopt(Server, SOL_SOCKET, 9999, &Value, &Length), ENOPROTOOPT);
  CHECK_ERRNO(setsockopt(Server, SOL_SOCKET, SO_SNDBUF, &Size, 2), EINVAL);
  CHECK_ERRNO(getsockopt(Server, IPPROTO_TCP, TCP_NODELAY, &Value, &Length), EOPNOTSUPP);
  const struct timeval Timeout = {0, 20000};
  CHECK(setsockopt(Dgram, SOL_SOCKET, SO_RCVTIMEO, &Timeout, sizeof(Timeout)) == 0);
  const int Bound = UnixSocket(SOCK_DGRAM, "timeo.sock");
  CHECK(setsockopt(Bound, SOL_SOCKET, SO_RCVTIMEO, &Timeout, sizeof(Timeout)) == 0);
  char Byte;
  const uint64_t Start = NowMs();
  CHECK_ERRNO(recv(Bound, &Byte, 1, 0), EAGAIN);
  CHECK(NowMs() - Start >= 15);
}

static int PeerSocket = -1;
static void ConnectFromChild(void) {
  const int Fd = socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(UnixConnect(Fd, "cred.sock") == 0);
  char Byte;
  CHECK(read(Fd, &Byte, 1) == 1);
}

TEST(unix, so_peercred) {
  int Pair[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, Pair) == 0);
  struct ucred Credentials;
  socklen_t Length = sizeof(Credentials);
  CHECK(getsockopt(Pair[0], SOL_SOCKET, SO_PEERCRED, &Credentials, &Length) == 0);
  CHECK(Length == sizeof(Credentials));
  CHECK(Credentials.pid == getpid() && Credentials.uid == geteuid() && Credentials.gid == getegid());
  PeerSocket = UnixSocket(SOCK_STREAM, "cred.sock");
  CHECK(PeerSocket >= 0 && listen(PeerSocket, 1) == 0);
  const pid_t Child = Fork(ConnectFromChild);
  const int Accepted = accept(PeerSocket, NULL, NULL);
  CHECK(Accepted >= 0);
  Length = sizeof(Credentials);
  CHECK(getsockopt(Accepted, SOL_SOCKET, SO_PEERCRED, &Credentials, &Length) == 0);
  CHECK(Credentials.pid == Child);
  // A listening socket reports its own credentials.
  Length = sizeof(Credentials);
  CHECK(getsockopt(PeerSocket, SOL_SOCKET, SO_PEERCRED, &Credentials, &Length) == 0 && Credentials.pid == getpid());
  CHECK(write(Accepted, "x", 1) == 1);
  JOIN(Child);
  const int Unconnected = socket(AF_UNIX, SOCK_STREAM, 0);
  Length = sizeof(Credentials);
  CHECK(getsockopt(Unconnected, SOL_SOCKET, SO_PEERCRED, &Credentials, &Length) == 0);
  CHECK(Credentials.pid == 0 && Credentials.uid == (uid_t)-1 && Credentials.gid == (gid_t)-1);
}

// Receives one byte and returns the SCM_CREDENTIALS it carried.
static int ReceiveCredentials(int Fd, struct ucred *Credentials) {
  char Byte;
  union {
    struct cmsghdr Header;
    char Space[CMSG_SPACE(sizeof(struct ucred))];
  } Control;
  struct iovec Vector = {&Byte, 1};
  struct msghdr Header = {.msg_iov = &Vector, .msg_iovlen = 1, .msg_control = &Control, .msg_controllen = sizeof(Control)};
  if (recvmsg(Fd, &Header, 0) != 1) return -1;
  struct cmsghdr *Message = CMSG_FIRSTHDR(&Header);
  if (!Message || Message->cmsg_level != SOL_SOCKET || Message->cmsg_type != SCM_CREDENTIALS) return -1;
  memcpy(Credentials, CMSG_DATA(Message), sizeof(*Credentials));
  return 0;
}

static ssize_t SendCredentials(int Fd, const struct ucred *Credentials) {
  union {
    struct cmsghdr Header;
    char Space[CMSG_SPACE(sizeof(struct ucred))];
  } Control;
  memset(&Control, 0, sizeof(Control));
  struct iovec Vector = {"c", 1};
  struct msghdr Header = {.msg_iov = &Vector, .msg_iovlen = 1, .msg_control = &Control, .msg_controllen = sizeof(Control)};
  struct cmsghdr *Message = CMSG_FIRSTHDR(&Header);
  Message->cmsg_level = SOL_SOCKET;
  Message->cmsg_type = SCM_CREDENTIALS;
  Message->cmsg_len = CMSG_LEN(sizeof(*Credentials));
  memcpy(CMSG_DATA(Message), Credentials, sizeof(*Credentials));
  return sendmsg(Fd, &Header, 0);
}

static void ForeignCredentials(void) {
  int Pair[2];
  CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, Pair) == 0);
  const struct ucred Foreign = {getpid(), 0, 0};
  CHECK_ERRNO(SendCredentials(Pair[0], &Foreign), EPERM);
  const struct ucred OtherPid = {1, geteuid(), getegid()};
  CHECK_ERRNO(SendCredentials(Pair[0], &OtherPid), EPERM);
}

TEST(unix, scm_credentials) {
  int Pair[2];
  CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, Pair) == 0);
  const int On = 1;
  CHECK(setsockopt(Pair[1], SOL_SOCKET, SO_PASSCRED, &On, sizeof(On)) == 0);
  CHECK(GetInt(Pair[1], SOL_SOCKET, SO_PASSCRED) == 1);
  // Without explicit credentials the kernel attaches the sender's.
  CHECK(send(Pair[0], "p", 1, 0) == 1);
  struct ucred Credentials = {0, 0, 0};
  CHECK(ReceiveCredentials(Pair[1], &Credentials) == 0);
  CHECK(Credentials.pid == getpid() && Credentials.uid == geteuid() && Credentials.gid == getegid());
  const struct ucred Own = {getpid(), getuid(), getgid()};
  CHECK(SendCredentials(Pair[0], &Own) == 1);
  CHECK(ReceiveCredentials(Pair[1], &Credentials) == 0 && Credentials.pid == getpid());
  AsUser(ForeignCredentials);
}

static int ReceiveDescriptor(int Fd) {
  char Byte;
  union {
    struct cmsghdr Header;
    char Space[CMSG_SPACE(sizeof(int))];
  } Control;
  struct iovec Vector = {&Byte, 1};
  struct msghdr Header = {.msg_iov = &Vector, .msg_iovlen = 1, .msg_control = &Control, .msg_controllen = sizeof(Control)};
  if (recvmsg(Fd, &Header, MSG_CMSG_CLOEXEC) != 1) return -1;
  struct cmsghdr *Message = CMSG_FIRSTHDR(&Header);
  if (!Message || Message->cmsg_type != SCM_RIGHTS) return -1;
  int Received;
  memcpy(&Received, CMSG_DATA(Message), sizeof(Received));
  return Received;
}

static ssize_t SendDescriptor(int Fd, int Passed) {
  union {
    struct cmsghdr Header;
    char Space[CMSG_SPACE(sizeof(int))];
  } Control;
  memset(&Control, 0, sizeof(Control));
  struct iovec Vector = {"r", 1};
  struct msghdr Header = {.msg_iov = &Vector, .msg_iovlen = 1, .msg_control = &Control, .msg_controllen = sizeof(Control)};
  struct cmsghdr *Message = CMSG_FIRSTHDR(&Header);
  Message->cmsg_level = SOL_SOCKET;
  Message->cmsg_type = SCM_RIGHTS;
  Message->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(Message), &Passed, sizeof(Passed));
  return sendmsg(Fd, &Header, 0);
}

TEST(unix, scm_rights_dgram) {
  int Pair[2], Pipe[2];
  CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, Pair) == 0);
  CHECK(pipe(Pipe) == 0);
  CHECK(SendDescriptor(Pair[0], Pipe[0]) == 1);
  close(Pipe[0]);
  const int Received = ReceiveDescriptor(Pair[1]);
  CHECK(Received >= 0);
  CHECK(fcntl(Received, F_GETFD) & FD_CLOEXEC);
  CHECK(write(Pipe[1], "through", 7) == 7);
  char Buffer[8] = {0};
  CHECK(read(Received, Buffer, sizeof(Buffer)) == 7 && strcmp(Buffer, "through") == 0);
  CHECK_ERRNO(SendDescriptor(Pair[0], 999), EBADF);
}

TEST(unix, sendmmsg_recvmmsg) {
  int Pair[2];
  CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, Pair) == 0);
  struct iovec Out[3] = {{"a", 1}, {"bb", 2}, {"ccc", 3}};
  struct mmsghdr Send[3];
  memset(Send, 0, sizeof(Send));
  for (int Index = 0; Index < 3; ++Index) {
    Send[Index].msg_hdr.msg_iov = &Out[Index];
    Send[Index].msg_hdr.msg_iovlen = 1;
  }
  CHECK(sendmmsg(Pair[0], Send, 3, 0) == 3);
  CHECK(Send[0].msg_len == 1 && Send[1].msg_len == 2 && Send[2].msg_len == 3);
  char Buffers[4][8];
  struct iovec In[4];
  struct mmsghdr Receive[4];
  memset(Receive, 0, sizeof(Receive));
  for (int Index = 0; Index < 4; ++Index) {
    In[Index] = (struct iovec) {Buffers[Index], 8};
    Receive[Index].msg_hdr.msg_iov = &In[Index];
    Receive[Index].msg_hdr.msg_iovlen = 1;
  }
  CHECK(recvmmsg(Pair[1], Receive, 4, MSG_DONTWAIT, NULL) == 3);
  CHECK(Receive[0].msg_len == 1 && Receive[1].msg_len == 2 && Receive[2].msg_len == 3);
  CHECK(memcmp(Buffers[2], "ccc", 3) == 0);
  CHECK_ERRNO(recvmmsg(Pair[1], Receive, 4, MSG_DONTWAIT, NULL), EAGAIN);
  // MSG_WAITFORONE: blocks for the first only.
  CHECK(send(Pair[0], "d", 1, 0) == 1);
  CHECK(recvmmsg(Pair[1], Receive, 4, MSG_WAITFORONE, NULL) == 1);
}

static int StreamPair[2];
static void *SendInPieces(void *Argument) {
  (void)Argument;
  char Block[25];
  for (int Index = 0; Index < 4; ++Index) {
    SleepMs(10);
    memset(Block, 'a' + Index, sizeof(Block));
    if (write(StreamPair[0], Block, sizeof(Block)) != sizeof(Block)) Bump(&Exited);
  }
  return NULL;
}

TEST(unix, msg_waitall) {
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, StreamPair) == 0);
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, SendInPieces, NULL) == 0);
  char Buffer[100];
  CHECK(recv(StreamPair[1], Buffer, sizeof(Buffer), MSG_WAITALL) == 100);
  CHECK(Buffer[0] == 'a' && Buffer[99] == 'd');
  CHECK(pthread_join(Thread, NULL) == 0);
  CHECK(Exited == 0);
  // EOF ends MSG_WAITALL early with what arrived.
  CHECK(write(StreamPair[0], "end", 3) == 3);
  shutdown(StreamPair[0], SHUT_WR);
  CHECK(recv(StreamPair[1], Buffer, sizeof(Buffer), MSG_WAITALL) == 3);
}

TEST(unix, peer_close_econnreset) {
  int Pair[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, Pair) == 0);
  CHECK(write(Pair[0], "unread", 6) == 6);
  close(Pair[1]);  // closes with data still queued for it
  char Buffer[8];
  CHECK_ERRNO(read(Pair[0], Buffer, sizeof(Buffer)), ECONNRESET);
  CHECK(read(Pair[0], Buffer, sizeof(Buffer)) == 0);
  // Without unread data, the peer just sees EOF.
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, Pair) == 0);
  close(Pair[1]);
  CHECK(read(Pair[0], Buffer, sizeof(Buffer)) == 0);
  signal(SIGPIPE, SIG_IGN);
  CHECK_ERRNO(write(Pair[0], "x", 1), EPIPE);
}

TEST(unix, rdhup_poll) {
  int Pair[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, Pair) == 0);
  struct pollfd Poll = {Pair[0], POLLIN | POLLRDHUP, 0};
  CHECK(poll(&Poll, 1, 0) == 0);
  CHECK(shutdown(Pair[1], SHUT_WR) == 0);
  CHECK(poll(&Poll, 1, 1000) == 1);
  CHECK((Poll.revents & (POLLIN | POLLRDHUP)) == (POLLIN | POLLRDHUP));
  CHECK(!(Poll.revents & POLLHUP));
  // Our side can still write.
  CHECK(write(Pair[0], "x", 1) == 1);
  close(Pair[1]);
  CHECK(poll(&Poll, 1, 1000) == 1 && (Poll.revents & POLLHUP));
  const int Epoll = epoll_create1(0);
  struct epoll_event Event = {.events = EPOLLIN | EPOLLRDHUP};
  CHECK(epoll_ctl(Epoll, EPOLL_CTL_ADD, Pair[0], &Event) == 0);
  CHECK(epoll_wait(Epoll, &Event, 1, 1000) == 1 && (Event.events & EPOLLRDHUP));
}

TEST(unix, epoll_et_socket) {
  int Pair[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, Pair) == 0);
  const int Epoll = epoll_create1(0);
  struct epoll_event Event = {.events = EPOLLIN | EPOLLET, .data.u32 = 7};
  CHECK(epoll_ctl(Epoll, EPOLL_CTL_ADD, Pair[0], &Event) == 0);
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 0);
  CHECK(write(Pair[1], "one", 3) == 3);
  CHECK(epoll_wait(Epoll, &Event, 1, 1000) == 1 && Event.data.u32 == 7);
  // No new edge while the data sits unread.
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 0);
  char Buffer[8];
  CHECK(read(Pair[0], Buffer, sizeof(Buffer)) == 3);
  CHECK_ERRNO(read(Pair[0], Buffer, sizeof(Buffer)), EAGAIN);
  CHECK(write(Pair[1], "two", 3) == 3);
  CHECK(epoll_wait(Epoll, &Event, 1, 1000) == 1);
  // EPOLLONESHOT disarms after one event until rearmed.
  Event = (struct epoll_event) {.events = EPOLLIN | EPOLLONESHOT, .data.u32 = 8};
  CHECK(epoll_ctl(Epoll, EPOLL_CTL_MOD, Pair[0], &Event) == 0);
  CHECK(epoll_wait(Epoll, &Event, 1, 1000) == 1);
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 0);
  Event = (struct epoll_event) {.events = EPOLLIN, .data.u32 = 9};
  CHECK(epoll_ctl(Epoll, EPOLL_CTL_MOD, Pair[0], &Event) == 0);
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 1 && Event.data.u32 == 9);
}

TEST(unix, recvmsg_name_108_byte_path) {
  char Path[109];
  memset(Path, 'p', 108);
  Path[108] = 0;
  const int Receiver = UnixSocket(SOCK_DGRAM, "recv.sock");
  CHECK(Receiver >= 0);
  const int Sender = socket(AF_UNIX, SOCK_DGRAM, 0);
  struct sockaddr_un Address = {.sun_family = AF_UNIX};
  memcpy(Address.sun_path, Path, 108);  // no terminating NUL fits
  if (bind(Sender, (struct sockaddr *)&Address, sizeof(Address))) {
    printf("#   bind of a 108-byte path: %s\n", ErrnoName(errno));
    ++CaseFailures;
    return;
  }
  CHECK(access(Path, F_OK) == 0);
  struct sockaddr_un Target;
  const socklen_t TargetLength = UnixAddress(&Target, "recv.sock");
  CHECK(sendto(Sender, "n", 1, 0, (struct sockaddr *)&Target, TargetLength) == 1);
  struct {
    struct sockaddr_un Address;
    char Extra[2];
  } Name;
  memset(&Name, 0x7f, sizeof(Name));
  char Byte;
  struct iovec Vector = {&Byte, 1};
  struct msghdr Header = {.msg_name = &Name, .msg_namelen = 112, .msg_iov = &Vector, .msg_iovlen = 1};
  CHECK(recvmsg(Receiver, &Header, 0) == 1);
  CHECK(Header.msg_namelen == 111);
  CHECK(memcmp(Name.Address.sun_path, Path, 108) == 0);
  CHECK(Name.Extra[0] == 0);  // the kernel's terminating NUL after the path
  // getsockname reports the same length.
  socklen_t Length = sizeof(Name);
  CHECK(getsockname(Sender, (struct sockaddr *)&Name, &Length) == 0 && Length == 111);
  unlink(Path);
}

// ---------------------------------------------------------------------------
// inet/: TCP and UDP on 127.0.0.1

static struct sockaddr_in Loopback(uint16_t Port) {
  return (struct sockaddr_in) {.sin_family = AF_INET, .sin_port = htons(Port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
}

static uint16_t LocalPort(int Fd) {
  struct sockaddr_in Address;
  socklen_t Length = sizeof(Address);
  if (getsockname(Fd, (struct sockaddr *)&Address, &Length)) return 0;
  return ntohs(Address.sin_port);
}

// A socket of Type bound to 127.0.0.1 on a free port (listening for TCP).
static int BoundLoopback(int Type, uint16_t *Port) {
  const int Fd = socket(AF_INET, Type, 0);
  if (Fd < 0) return -1;
  struct sockaddr_in Address = Loopback(0);
  if (bind(Fd, (struct sockaddr *)&Address, sizeof(Address))) return -1;
  if ((Type & 0xf) == SOCK_STREAM && listen(Fd, 16)) return -1;
  *Port = LocalPort(Fd);
  return Fd;
}

static int ConnectLoopback(uint16_t Port) {
  const int Fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in Address = Loopback(Port);
  if (Fd < 0 || connect(Fd, (struct sockaddr *)&Address, sizeof(Address))) return -1;
  return Fd;
}

// A connected TCP pair: Ends[0] the client, Ends[1] the accepted socket.
static int TcpPair(int Ends[2]) {
  uint16_t Port;
  const int Listener = BoundLoopback(SOCK_STREAM, &Port);
  if (Listener < 0) return -1;
  Ends[0] = ConnectLoopback(Port);
  Ends[1] = accept(Listener, NULL, NULL);
  close(Listener);
  return Ends[0] >= 0 && Ends[1] >= 0 ? 0 : -1;
}

// A port that nothing listens on (bound briefly, then released).
static uint16_t ClosedPort(int Type) {
  // Below the ephemeral range, so a connect from an ephemeral client port
  // cannot self-connect to the same number.
  for (uint16_t Port = 20000 + (uint16_t)(getpid() % 5000); Port < 30000; ++Port) {
    const int Fd = socket(AF_INET, Type, 0);
    struct sockaddr_in Address = Loopback(Port);
    const int Bound = Fd >= 0 && bind(Fd, (struct sockaddr *)&Address, sizeof(Address)) == 0;
    if (Fd >= 0) close(Fd);
    if (Bound) return Port;
  }
  return 0;
}

TEST(inet, tcp_connect_accept) {
  uint16_t Port;
  const int Listener = BoundLoopback(SOCK_STREAM, &Port);
  CHECK(Listener >= 0 && Port != 0);
  const int Client = ConnectLoopback(Port);
  CHECK(Client >= 0);
  struct sockaddr_in Peer;
  socklen_t Length = sizeof(Peer);
  const int Accepted = accept4(Listener, (struct sockaddr *)&Peer, &Length, SOCK_CLOEXEC);
  CHECK(Accepted >= 0);
  CHECK(Length == sizeof(Peer) && Peer.sin_family == AF_INET);
  CHECK(Peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && ntohs(Peer.sin_port) == LocalPort(Client));
  CHECK(fcntl(Accepted, F_GETFD) & FD_CLOEXEC);
  CHECK(LocalPort(Accepted) == Port);
  struct sockaddr_in Remote;
  Length = sizeof(Remote);
  CHECK(getpeername(Client, (struct sockaddr *)&Remote, &Length) == 0 && ntohs(Remote.sin_port) == Port);
  CHECK(write(Client, "ping", 4) == 4);
  char Buffer[8] = {0};
  CHECK(read(Accepted, Buffer, sizeof(Buffer)) == 4 && strcmp(Buffer, "ping") == 0);
  CHECK(send(Accepted, "pong", 4, 0) == 4);
  CHECK(recv(Client, Buffer, sizeof(Buffer), 0) == 4 && memcmp(Buffer, "pong", 4) == 0);
  CHECK(GetInt(Accepted, SOL_SOCKET, SO_TYPE) == SOCK_STREAM && GetInt(Accepted, SOL_SOCKET, SO_PROTOCOL) == IPPROTO_TCP);
  CHECK(GetInt(Listener, SOL_SOCKET, SO_ACCEPTCONN) == 1 && GetInt(Client, SOL_SOCKET, SO_ACCEPTCONN) == 0);
  CHECK_ERRNO(accept(Client, NULL, NULL), EINVAL);
  const int Fresh = socket(AF_INET, SOCK_STREAM, 0);
  Length = sizeof(Remote);
  CHECK_ERRNO(getpeername(Fresh, (struct sockaddr *)&Remote, &Length), ENOTCONN);
  CHECK_ERRNO(recv(Fresh, Buffer, 1, 0), ENOTCONN);
}

TEST(inet, tcp_nonblocking_connect_einprogress) {
  uint16_t Port;
  const int Listener = BoundLoopback(SOCK_STREAM, &Port);
  CHECK(Listener >= 0);
  const int Client = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  struct sockaddr_in Address = Loopback(Port);
  CHECK_ERRNO(connect(Client, (struct sockaddr *)&Address, sizeof(Address)), EINPROGRESS);
  struct pollfd Poll = {Client, POLLOUT, 0};
  CHECK(poll(&Poll, 1, 3000) == 1 && (Poll.revents & POLLOUT));
  CHECK(GetInt(Client, SOL_SOCKET, SO_ERROR) == 0);
  // Linux completes a SS_CONNECTING socket with one more connect() returning
  // 0; after that it is EISCONN.
  errno = 0;
  const int Again = connect(Client, (struct sockaddr *)&Address, sizeof(Address));
  const int AgainErrno = errno;  // CHECK clears errno before it evaluates its expression
  CHECK(Again == 0 || (Again == -1 && AgainErrno == EISCONN));
  CHECK_ERRNO(connect(Client, (struct sockaddr *)&Address, sizeof(Address)), EISCONN);
  const int Accepted = accept(Listener, NULL, NULL);
  CHECK(Accepted >= 0);
  // A nonblocking accept on an empty queue.
  CHECK(fcntl(Listener, F_SETFL, O_NONBLOCK) == 0);
  CHECK_ERRNO(accept(Listener, NULL, NULL), EAGAIN);
  char Byte;
  CHECK_ERRNO(recv(Client, &Byte, 1, 0), EAGAIN);
}

TEST(inet, tcp_so_error_refused) {
  const uint16_t Port = ClosedPort(SOCK_STREAM);
  const int Client = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  struct sockaddr_in Address = Loopback(Port);
  const int Result = connect(Client, (struct sockaddr *)&Address, sizeof(Address));
  const int ConnectErrno = errno;
  CHECK(Result == -1 && (ConnectErrno == EINPROGRESS || ConnectErrno == ECONNREFUSED));
  if (Result == -1 && ConnectErrno == EINPROGRESS) {
    struct pollfd Poll = {Client, POLLOUT, 0};
    CHECK(poll(&Poll, 1, 3000) == 1 && (Poll.revents & (POLLOUT | POLLERR)));
    // Linux numbering, translated from the host's errno.
    const int Error = GetInt(Client, SOL_SOCKET, SO_ERROR);
    if (Error != 111) printf("#   SO_ERROR %d\n", Error);
    CHECK(Error == 111);
    CHECK(GetInt(Client, SOL_SOCKET, SO_ERROR) == 0);  // reading clears it
  }
  const int Blocking = socket(AF_INET, SOCK_STREAM, 0);
  CHECK_ERRNO(connect(Blocking, (struct sockaddr *)&Address, sizeof(Address)), ECONNREFUSED);
}

TEST(inet, tcp_eaddrinuse) {
  uint16_t Port;
  const int Listener = BoundLoopback(SOCK_STREAM, &Port);
  CHECK(Listener >= 0);
  const int Second = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in Address = Loopback(Port);
  CHECK_ERRNO(bind(Second, (struct sockaddr *)&Address, sizeof(Address)), EADDRINUSE);
  // SO_REUSEADDR does not allow a second listener on the same port.
  const int On = 1;
  CHECK(setsockopt(Second, SOL_SOCKET, SO_REUSEADDR, &On, sizeof(On)) == 0);
  CHECK_ERRNO(bind(Second, (struct sockaddr *)&Address, sizeof(Address)), EADDRINUSE);
  // A bound socket cannot be bound again.
  Address.sin_port = 0;
  CHECK_ERRNO(bind(Listener, (struct sockaddr *)&Address, sizeof(Address)), EINVAL);
  CHECK_ERRNO(bind(Second, (struct sockaddr *)&Address, sizeof(Address) - 1), EINVAL);
  // An address that no interface owns.
  Address.sin_addr.s_addr = inet_addr("192.0.2.1");
  CHECK_ERRNO(bind(Second, (struct sockaddr *)&Address, sizeof(Address)), EADDRNOTAVAIL);
}

TEST(inet, tcp_bind_privileged_port_eacces) {
  if (geteuid() == 0) SKIP("root may bind privileged ports");
  long Start = 1024;
  FILE *File = fopen("/proc/sys/net/ipv4/ip_unprivileged_port_start", "r");
  if (File) {
    if (!ReadLong(File, &Start)) Start = 1024;
    fclose(File);
  }
  if (Start <= 80) SKIP("unprivileged ports start at or below 80");
  const int Fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in Address = Loopback(80);
  CHECK_ERRNO(bind(Fd, (struct sockaddr *)&Address, sizeof(Address)), EACCES);
}

TEST(inet, tcp_shutdown_eof) {
  int Ends[2];
  CHECK(TcpPair(Ends) == 0);
  CHECK(write(Ends[0], "last", 4) == 4);
  CHECK(shutdown(Ends[0], SHUT_WR) == 0);
  char Buffer[8];
  CHECK(read(Ends[1], Buffer, sizeof(Buffer)) == 4);
  CHECK(read(Ends[1], Buffer, sizeof(Buffer)) == 0);
  // The other direction still works.
  CHECK(write(Ends[1], "reply", 5) == 5);
  CHECK(recv(Ends[0], Buffer, 5, MSG_WAITALL) == 5);
  signal(SIGPIPE, SIG_IGN);
  CHECK_ERRNO(write(Ends[0], "x", 1), EPIPE);
  CHECK_ERRNO(shutdown(Ends[0], 7), EINVAL);
  CHECK(shutdown(Ends[1], SHUT_RDWR) == 0);
  CHECK(read(Ends[0], Buffer, sizeof(Buffer)) == 0);
}

static volatile sig_atomic_t PipeSignals;
static void CountPipe(int Signal) {
  (void)Signal;
  ++PipeSignals;
}

// Writes to a connection the peer closed until the write fails.
static int WriteUntilError(int Fd, int Flags) {
  const uint64_t Limit = Deadline(3000);
  while (!DeadlineExpired(Limit)) {
    if (send(Fd, "data", 4, Flags) < 0) return errno;
    SleepMs(10);
  }
  return 0;
}

TEST(inet, tcp_epipe_sigpipe) {
  int Ends[2];
  CHECK(TcpPair(Ends) == 0);
  struct sigaction Action = {.sa_handler = CountPipe};
  sigemptyset(&Action.sa_mask);
  sigaction(SIGPIPE, &Action, NULL);
  close(Ends[1]);
  CHECK(WriteUntilError(Ends[0], 0) == EPIPE);
  CHECK(PipeSignals == 1);
  CHECK_ERRNO(write(Ends[0], "x", 1), EPIPE);
  CHECK(PipeSignals == 2);
}

TEST(inet, tcp_msg_nosignal) {
  int Ends[2];
  CHECK(TcpPair(Ends) == 0);
  // Default SIGPIPE disposition: a signal would end the test.
  close(Ends[1]);
  CHECK(WriteUntilError(Ends[0], MSG_NOSIGNAL) == EPIPE);
  struct iovec Vector = {"x", 1};
  struct msghdr Header = {.msg_iov = &Vector, .msg_iovlen = 1};
  CHECK_ERRNO(sendmsg(Ends[0], &Header, MSG_NOSIGNAL), EPIPE);
  struct sockaddr_in Ignored = Loopback(1);
  CHECK_ERRNO(sendto(Ends[0], "x", 1, MSG_NOSIGNAL, (struct sockaddr *)&Ignored, sizeof(Ignored)), EPIPE);
}

TEST(inet, tcp_peek_fionread) {
  int Ends[2];
  CHECK(TcpPair(Ends) == 0);
  CHECK(write(Ends[0], "0123456789", 10) == 10);
  int Pending = 0;
  WAIT_UNTIL(ioctl(Ends[1], FIONREAD, &Pending) == 0 && Pending == 10);
  CHECK(Pending == 10);
  char Buffer[16] = {0};
  CHECK(recv(Ends[1], Buffer, sizeof(Buffer), MSG_PEEK) == 10 && memcmp(Buffer, "0123456789", 10) == 0);
  CHECK(ioctl(Ends[1], FIONREAD, &Pending) == 0 && Pending == 10);
  CHECK(recv(Ends[1], Buffer, 4, 0) == 4 && memcmp(Buffer, "0123", 4) == 0);
  CHECK(ioctl(Ends[1], FIONREAD, &Pending) == 0 && Pending == 6);
  CHECK(recv(Ends[1], Buffer, sizeof(Buffer), MSG_DONTWAIT) == 6);
  CHECK_ERRNO(recv(Ends[1], Buffer, sizeof(Buffer), MSG_DONTWAIT), EAGAIN);
}

TEST(inet, tcp_poll_epoll_select) {
  int Ends[2];
  CHECK(TcpPair(Ends) == 0);
  struct pollfd Poll = {Ends[1], POLLIN | POLLOUT, 0};
  CHECK(poll(&Poll, 1, 0) == 1 && Poll.revents == POLLOUT);
  const int Epoll = epoll_create1(0);
  struct epoll_event Event = {.events = EPOLLIN, .data.u32 = 3};
  CHECK(epoll_ctl(Epoll, EPOLL_CTL_ADD, Ends[1], &Event) == 0);
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 0);
  CHECK(write(Ends[0], "x", 1) == 1);
  Poll.events = POLLIN;
  CHECK(poll(&Poll, 1, 3000) == 1 && (Poll.revents & POLLIN));
  CHECK(epoll_wait(Epoll, &Event, 1, 3000) == 1 && Event.data.u32 == 3 && (Event.events & EPOLLIN));
  fd_set Read;
  FD_ZERO(&Read);
  FD_SET(Ends[1], &Read);
  struct timeval Timeout = {3, 0};
  CHECK(select(Ends[1] + 1, &Read, NULL, NULL, &Timeout) == 1 && FD_ISSET(Ends[1], &Read));
  char Byte;
  CHECK(read(Ends[1], &Byte, 1) == 1);
  FD_ZERO(&Read);
  FD_SET(Ends[1], &Read);
  Timeout = (struct timeval) {0, 20000};
  CHECK(select(Ends[1] + 1, &Read, NULL, NULL, &Timeout) == 0);
  // A listening socket is readable when a connection waits.
  uint16_t Port;
  const int Listener = BoundLoopback(SOCK_STREAM, &Port);
  Poll = (struct pollfd) {Listener, POLLIN, 0};
  CHECK(poll(&Poll, 1, 0) == 0);
  const int Client = ConnectLoopback(Port);
  CHECK(Client >= 0);
  CHECK(poll(&Poll, 1, 3000) == 1 && (Poll.revents & POLLIN));
  // A closed peer reads as POLLIN with EOF.
  close(Ends[0]);
  Poll = (struct pollfd) {Ends[1], POLLIN | POLLRDHUP, 0};
  CHECK(poll(&Poll, 1, 3000) == 1 && (Poll.revents & POLLIN) && (Poll.revents & POLLRDHUP));
}

TEST(inet, tcp_epoll_et) {
  int Ends[2];
  CHECK(TcpPair(Ends) == 0);
  CHECK(fcntl(Ends[1], F_SETFL, O_NONBLOCK) == 0);
  const int Epoll = epoll_create1(0);
  struct epoll_event Event = {.events = EPOLLIN | EPOLLET, .data.u32 = 4};
  CHECK(epoll_ctl(Epoll, EPOLL_CTL_ADD, Ends[1], &Event) == 0);
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 0);
  CHECK(write(Ends[0], "first", 5) == 5);
  CHECK(epoll_wait(Epoll, &Event, 1, 3000) == 1 && Event.data.u32 == 4);
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 0);
  // Drain to EAGAIN, as edge-triggered readers do; new data is a new edge.
  char Buffer[16];
  CHECK(read(Ends[1], Buffer, sizeof(Buffer)) == 5);
  CHECK_ERRNO(read(Ends[1], Buffer, sizeof(Buffer)), EAGAIN);
  CHECK(write(Ends[0], "second", 6) == 6);
  CHECK(epoll_wait(Epoll, &Event, 1, 3000) == 1);
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 0);
}

static int BulkFd;
static void *SendBulk(void *Argument) {
  (void)Argument;
  static unsigned char Block[65536];
  size_t Sent = 0;
  const size_t Total = 16 << 20;
  while (Sent < Total) {
    for (size_t Index = 0; Index < sizeof(Block); ++Index) Block[Index] = (unsigned char)((Sent + Index) * 7 + ((Sent + Index) >> 16));
    size_t Offset = 0;
    while (Offset < sizeof(Block)) {
      const ssize_t Result = write(BulkFd, Block + Offset, sizeof(Block) - Offset);
      if (Result <= 0) {
        Bump(&Exited);
        return NULL;
      }
      Offset += Result;
    }
    Sent += sizeof(Block);
  }
  shutdown(BulkFd, SHUT_WR);
  return NULL;
}

TEST(inet, tcp_large_transfer_16mib) {
  int Ends[2];
  CHECK(TcpPair(Ends) == 0);
  BulkFd = Ends[0];
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, SendBulk, NULL) == 0);
  static unsigned char Buffer[100000];
  size_t Received = 0, Bad = 0;
  for (;;) {
    const ssize_t Result = read(Ends[1], Buffer, sizeof(Buffer));
    if (Result <= 0) {
      CHECK(Result == 0);
      break;
    }
    for (ssize_t Index = 0; Index < Result; ++Index) {
      const size_t Position = Received + Index;
      Bad += Buffer[Index] != (unsigned char)(Position * 7 + (Position >> 16));
    }
    Received += Result;
  }
  CHECK(pthread_join(Thread, NULL) == 0);
  CHECK(Exited == 0);
  CHECK(Received == 16 << 20);
  CHECK(Bad == 0);
}

TEST(inet, udp_boundaries) {
  uint16_t Port;
  const int Receiver = BoundLoopback(SOCK_DGRAM, &Port);
  CHECK(Receiver >= 0);
  const int Sender = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in Target = Loopback(Port);
  CHECK(sendto(Sender, "one", 3, 0, (struct sockaddr *)&Target, sizeof(Target)) == 3);
  CHECK(sendto(Sender, "", 0, 0, (struct sockaddr *)&Target, sizeof(Target)) == 0);
  CHECK(sendto(Sender, "three", 5, 0, (struct sockaddr *)&Target, sizeof(Target)) == 5);
  char Buffer[16];
  struct sockaddr_in Source;
  socklen_t Length = sizeof(Source);
  CHECK(recvfrom(Receiver, Buffer, sizeof(Buffer), 0, (struct sockaddr *)&Source, &Length) == 3);
  CHECK(Length == sizeof(Source) && Source.sin_family == AF_INET && Source.sin_port == htons(LocalPort(Sender)));
  CHECK(recv(Receiver, Buffer, sizeof(Buffer), 0) == 0);
  CHECK(recv(Receiver, Buffer, sizeof(Buffer), 0) == 5 && memcmp(Buffer, "three", 5) == 0);
  CHECK_ERRNO(recv(Receiver, Buffer, sizeof(Buffer), MSG_DONTWAIT), EAGAIN);
  CHECK_ERRNO(send(Sender, "x", 1, 0), EDESTADDRREQ);
  static char Large[65508];
  CHECK_ERRNO(sendto(Sender, Large, sizeof(Large), 0, (struct sockaddr *)&Target, sizeof(Target)), EMSGSIZE);
  CHECK(GetInt(Receiver, SOL_SOCKET, SO_TYPE) == SOCK_DGRAM && GetInt(Receiver, SOL_SOCKET, SO_PROTOCOL) == IPPROTO_UDP);
}

TEST(inet, udp_connected_refused) {
  const uint16_t Port = ClosedPort(SOCK_DGRAM);
  const int Fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in Target = Loopback(Port);
  CHECK(connect(Fd, (struct sockaddr *)&Target, sizeof(Target)) == 0);
  CHECK(send(Fd, "x", 1, 0) == 1);
  // The ICMP port unreachable comes back as a pending error.
  struct pollfd Poll = {Fd, POLLIN, 0};
  CHECK(poll(&Poll, 1, 3000) == 1 && (Poll.revents & POLLERR));
  char Byte;
  CHECK_ERRNO(recv(Fd, &Byte, 1, 0), ECONNREFUSED);
  CHECK(send(Fd, "y", 1, 0) == 1);
  WAIT_UNTIL(poll(&Poll, 1, 0) == 1);
  CHECK(GetInt(Fd, SOL_SOCKET, SO_ERROR) == ECONNREFUSED);
  CHECK(GetInt(Fd, SOL_SOCKET, SO_ERROR) == 0);
}

TEST(inet, udp_msg_trunc) {
  uint16_t Port;
  const int Receiver = BoundLoopback(SOCK_DGRAM, &Port);
  const int Sender = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in Target = Loopback(Port);
  CHECK(sendto(Sender, "truncated", 9, 0, (struct sockaddr *)&Target, sizeof(Target)) == 9);
  char Buffer[4];
  struct iovec Vector = {Buffer, sizeof(Buffer)};
  struct msghdr Header = {.msg_iov = &Vector, .msg_iovlen = 1};
  CHECK(recvmsg(Receiver, &Header, 0) == 4);
  CHECK(Header.msg_flags & MSG_TRUNC);
  CHECK(sendto(Sender, "abcdefgh", 8, 0, (struct sockaddr *)&Target, sizeof(Target)) == 8);
  CHECK(recv(Receiver, Buffer, sizeof(Buffer), MSG_TRUNC) == 8);
  CHECK(sendto(Sender, "peek", 4, 0, (struct sockaddr *)&Target, sizeof(Target)) == 4);
  int Pending = 0;
  WAIT_UNTIL(ioctl(Receiver, FIONREAD, &Pending) == 0 && Pending == 4);
  CHECK(Pending == 4);
  CHECK(recv(Receiver, Buffer, 2, MSG_PEEK | MSG_TRUNC) == 4);
  CHECK(recv(Receiver, Buffer, sizeof(Buffer), 0) == 4);
}

TEST(inet, sockopt_map) {
  const int Fd = socket(AF_INET, SOCK_STREAM, 0);
  const int On = 1, Off = 0;
  CHECK(GetInt(Fd, SOL_SOCKET, SO_REUSEADDR) == 0);
  CHECK(setsockopt(Fd, SOL_SOCKET, SO_REUSEADDR, &On, sizeof(On)) == 0);
  CHECK(GetInt(Fd, SOL_SOCKET, SO_REUSEADDR) == 1);
  CHECK(setsockopt(Fd, SOL_SOCKET, SO_KEEPALIVE, &On, sizeof(On)) == 0);
  CHECK(GetInt(Fd, SOL_SOCKET, SO_KEEPALIVE) == 1);
  CHECK(setsockopt(Fd, SOL_SOCKET, SO_KEEPALIVE, &Off, sizeof(Off)) == 0);
  CHECK(GetInt(Fd, SOL_SOCKET, SO_KEEPALIVE) == 0);
  CHECK(GetInt(Fd, IPPROTO_TCP, TCP_NODELAY) == 0);
  CHECK(setsockopt(Fd, IPPROTO_TCP, TCP_NODELAY, &On, sizeof(On)) == 0);
  CHECK(GetInt(Fd, IPPROTO_TCP, TCP_NODELAY) == 1);
  const int Size = 65536;
  CHECK(setsockopt(Fd, SOL_SOCKET, SO_RCVBUF, &Size, sizeof(Size)) == 0);
  const int Read = GetInt(Fd, SOL_SOCKET, SO_RCVBUF);
  if (Read != 2 * Size) printf("#   SO_RCVBUF %d\n", Read);
  CHECK(Read == 2 * Size);
  // Values the kernel does not have.
  int Value;
  socklen_t Length = sizeof(Value);
  CHECK_ERRNO(getsockopt(Fd, SOL_SOCKET, 9999, &Value, &Length), ENOPROTOOPT);
  CHECK_ERRNO(getsockopt(Fd, IPPROTO_TCP, 9999, &Value, &Length), ENOPROTOOPT);
  CHECK_ERRNO(setsockopt(Fd, IPPROTO_TCP, TCP_NODELAY, &On, 2), EINVAL);
  const struct linger Linger = {1, 5};
  CHECK(setsockopt(Fd, SOL_SOCKET, SO_LINGER, &Linger, sizeof(Linger)) == 0);
  CHECK(GetInt(Fd, IPPROTO_IP, IP_TTL) == 64);
  const int Udp = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(setsockopt(Udp, SOL_SOCKET, SO_BROADCAST, &On, sizeof(On)) == 0);
  CHECK(GetInt(Udp, SOL_SOCKET, SO_BROADCAST) == 1);
  CHECK_ERRNO(getsockopt(Udp, IPPROTO_TCP, TCP_NODELAY, &Value, &Length), EOPNOTSUPP);
}

TEST(inet, inet6_eafnosupport) {
  GUEST_ONLY();
  CHECK_ERRNO(socket(AF_INET6, SOCK_STREAM, 0), EAFNOSUPPORT);
  CHECK_ERRNO(socket(AF_INET6, SOCK_DGRAM, 0), EAFNOSUPPORT);
}

static int AcceptListener;
static int AcceptedFd = -2;
static void *BlockingAccept(void *Argument) {
  (void)Argument;
  Bump(&Started);
  AcceptedFd = accept(AcceptListener, NULL, NULL);
  Bump(&Finished);
  return NULL;
}

TEST(inet, blocking_accept_does_not_stall_other_threads) {
  uint16_t Port;
  AcceptListener = BoundLoopback(SOCK_STREAM, &Port);
  CHECK(AcceptListener >= 0);
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, BlockingAccept, NULL) == 0);
  WAIT_UNTIL(Load(&Started) == 1);
  // The main thread keeps running while the other blocks in accept.
  volatile uint64_t Sum = 0; (void)Sum;
  const uint64_t Until = NowMs() + 100;
  uint64_t Rounds = 0;
  while (NowMs() < Until) {
    for (int Index = 0; Index < 1000; ++Index) Sum += Index;
    ++Rounds;
  }
  CHECK(Rounds > 10);
  CHECK(Load(&Finished) == 0);
  const int Client = ConnectLoopback(Port);
  CHECK(Client >= 0);
  CHECK(pthread_join(Thread, NULL) == 0);
  CHECK(AcceptedFd >= 0);
  CHECK(write(Client, "z", 1) == 1);
  char Byte;
  CHECK(read(AcceptedFd, &Byte, 1) == 1 && Byte == 'z');
}

TEST(inet, wildcard_bind_loopback) {
  // TCP: a listener bound to 0.0.0.0 is reachable through 127.0.0.1.
  const int Listener = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in Any = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)};
  CHECK(bind(Listener, (struct sockaddr *)&Any, sizeof(Any)) == 0);
  CHECK(listen(Listener, 4) == 0);
  struct sockaddr_in Name;
  socklen_t Length = sizeof(Name);
  CHECK(getsockname(Listener, (struct sockaddr *)&Name, &Length) == 0);
  CHECK(Name.sin_addr.s_addr == htonl(INADDR_ANY) && Name.sin_port != 0);
  const uint16_t Port = ntohs(Name.sin_port);
  const int Client = ConnectLoopback(Port);
  CHECK(Client >= 0);
  const int Accepted = accept(Listener, NULL, NULL);
  CHECK(Accepted >= 0);
  char Buffer[8];
  CHECK(write(Client, "wild", 4) == 4);
  CHECK(recv(Accepted, Buffer, 4, MSG_WAITALL) == 4 && memcmp(Buffer, "wild", 4) == 0);
  // UDP: the same, and it stays reachable after the receiver connects.
  const int Receiver = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(bind(Receiver, (struct sockaddr *)&Any, sizeof(Any)) == 0);
  Length = sizeof(Name);
  CHECK(getsockname(Receiver, (struct sockaddr *)&Name, &Length) == 0);
  CHECK(Name.sin_addr.s_addr == htonl(INADDR_ANY) && Name.sin_port != 0);
  struct sockaddr_in Target = Loopback(ntohs(Name.sin_port));
  const int Sender = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK(sendto(Sender, "u1", 2, 0, (struct sockaddr *)&Target, sizeof(Target)) == 2);
  struct sockaddr_in Source;
  Length = sizeof(Source);
  CHECK(recvfrom(Receiver, Buffer, sizeof(Buffer), 0, (struct sockaddr *)&Source, &Length) == 2);
  CHECK(Source.sin_port == htons(LocalPort(Sender)));
  CHECK(connect(Receiver, (struct sockaddr *)&Source, sizeof(Source)) == 0);
  CHECK(sendto(Sender, "u2", 2, 0, (struct sockaddr *)&Target, sizeof(Target)) == 2);
  struct pollfd Poll = {Receiver, POLLIN, 0};
  CHECK(poll(&Poll, 1, 3000) == 1);
  CHECK(recv(Receiver, Buffer, sizeof(Buffer), MSG_DONTWAIT) == 2 && memcmp(Buffer, "u2", 2) == 0);
  CHECK(send(Receiver, "r", 1, 0) == 1);
  CHECK(poll(&(struct pollfd) {Sender, POLLIN, 0}, 1, 3000) == 1);
  CHECK(recv(Sender, Buffer, sizeof(Buffer), MSG_DONTWAIT) == 1 && Buffer[0] == 'r');
}

TEST(inet, connect_unbound_getsockname) {
  uint16_t Port;
  const int Listener = BoundLoopback(SOCK_STREAM, &Port);
  CHECK(Listener >= 0);
  const int Client = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in Name;
  socklen_t Length = sizeof(Name);
  CHECK(getsockname(Client, (struct sockaddr *)&Name, &Length) == 0);
  CHECK(Name.sin_addr.s_addr == 0 && Name.sin_port == 0);
  struct sockaddr_in Target = Loopback(Port);
  CHECK(connect(Client, (struct sockaddr *)&Target, sizeof(Target)) == 0);
  Length = sizeof(Name);
  CHECK(getsockname(Client, (struct sockaddr *)&Name, &Length) == 0);
  CHECK(Length == sizeof(Name) && Name.sin_family == AF_INET);
  CHECK(Name.sin_addr.s_addr != 0 && Name.sin_port != 0);
  const int Accepted = accept(Listener, NULL, NULL);
  CHECK(Accepted >= 0);
  struct sockaddr_in Peer;
  Length = sizeof(Peer);
  CHECK(getpeername(Accepted, (struct sockaddr *)&Peer, &Length) == 0);
  CHECK(Peer.sin_port == Name.sin_port && Peer.sin_addr.s_addr == Name.sin_addr.s_addr);
}
