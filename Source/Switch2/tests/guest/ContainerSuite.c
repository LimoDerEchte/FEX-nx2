// SPDX-License-Identifier: MIT
// Container and scheduling compatibility suite for the Switch 2 guest
// runtime (milestone 4): scheduling calls, ioprio, kcmp, the Linux errors of
// machine-wide calls, user/UTS/IPC/PID/mount namespaces, mount, pivot_root,
// chroot and seccomp.
//
// The harness is the one of ProcSuite.c and CommSuite.c: every test runs in
// its own forked child (exit 0 pass, 1 fail, 77 skip, 10 s timeout) and the
// results are TAP lines. Expectations follow Linux 7.3. Namespace cases create
// their own user namespace first, so they run unprivileged as uid 1000 and as
// root alike. Deliberate differences of the Switch 2 runtime
// (ContainerInventory.md) are checked only in the guest ("#1 FEX Switch 2"
// uname version) and skip natively.
//
// Usage: container-suite [pattern...] | container-suite -l
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
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
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
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/nsfs.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <dirent.h>
#include <sys/fsuid.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/swap.h>

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
    // Cases create files by fixed names: each gets its own directory.
    char CaseDir[] = "./case.XXXXXX";
    // World writable so cases that drop to uid 1000 can create files.
    if (!mkdtemp(CaseDir) || chmod(CaseDir, 0777) || chdir(CaseDir)) _exit(1);
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
  // Exec probe of seccomp/filter_kept_across_exec: exits 0 when getppid fails
  // with the errno the inherited filter returns.
  if (argc == 2 && !strcmp(argv[1], "--getppid-probe")) {
    errno = 0;
    return syscall(SYS_getppid) == -1 && errno == ENOMSG ? 0 : 1;
  }
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
  snprintf(Scratch, sizeof(Scratch), "%s/container-suite.XXXXXX", Location);
  if (!mkdtemp(Scratch) || !realpath(Scratch, Absolute) || chdir(Absolute)) {
    const int Error = errno;
    char Here[PATH_MAX] = "?";
    struct stat Info = {0};
    const int Found = getcwd(Here, sizeof(Here)) && stat(".", &Info) == 0;
    printf("Bail out! cannot create a scratch directory in %s (uid %u, folder %s owner %u:%u mode %04o): %s\n", Here,
           (unsigned) geteuid(), Found ? "found" : "unreadable", (unsigned) Info.st_uid, (unsigned) Info.st_gid,
           (unsigned) (Info.st_mode & 07777), ErrnoName(Error));
    return 1;
  }
  memcpy(Scratch, Absolute, sizeof(Scratch));

  unsigned Selection = 0;
  for (unsigned Index = 0; Index < TestCount; ++Index) Selection += Selected(Tests[Index].Name, Count, Patterns);
  if (Count && !Selection) {
    printf("Bail out! no tests match the given patterns\n");
    return 1;
  }
  printf("TAP version 13\n1..%u\n# container-suite uid %u, gid %u, %s\n", Selection, getuid(), getgid(),
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
  printf("# container-suite summary: %u passed, %u failed, %u skipped\n", Passed, Failed, SkippedCount);
  char Remove[PATH_MAX + 16];
  snprintf(Remove, sizeof(Remove), "rm -rf %s", Scratch);
  if (chdir("..") == 0 && system(Remove) != 0) printf("# could not remove %s\n", Scratch);
  return Failed ? 1 : 42;
}

// ---------------------------------------------------------------------------
// Shared helpers

// Cases that check a deliberate difference of the Switch 2 runtime.
#define GUEST_ONLY() do { if (!GuestRuntime) SKIP("guest-only expectation (ContainerInventory.md)"); } while (0)

// Runs Body as uid/gid 1000: directly when not root, otherwise in a child
// that drops every identity.
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

static int WriteText(const char *Path, const char *Text) {
  const int Fd = open(Path, O_WRONLY | O_CLOEXEC);
  if (Fd < 0) return -1;
  const ssize_t Length = (ssize_t) strlen(Text);
  const ssize_t Written = write(Fd, Text, Length);
  const int Error = errno;
  close(Fd);
  errno = Error;
  return Written == Length ? 0 : -1;
}

// Reads a whole small file into Buffer (NUL-terminated); returns the length or -1.
static ssize_t ReadText(const char *Path, char *Buffer, size_t Size) {
  const int Fd = open(Path, O_RDONLY | O_CLOEXEC);
  if (Fd < 0) return -1;
  size_t Total = 0;
  for (;;) {
    const ssize_t Got = read(Fd, Buffer + Total, Size - 1 - Total);
    if (Got <= 0) break;
    Total += (size_t) Got;
    if (Total == Size - 1) break;
  }
  close(Fd);
  Buffer[Total] = 0;
  return (ssize_t) Total;
}

// Enters a new user namespace (plus Flags) and maps the caller's ids to root.
// Returns 0, or -1 with errno from the failing step.
static int EnterUserNs(int Flags) {
  const uid_t Uid = geteuid();
  const gid_t Gid = getegid();
  if (unshare(CLONE_NEWUSER | Flags) < 0) return -1;
  char Map[64];
  if (WriteText("/proc/self/setgroups", "deny") < 0) return -1;
  snprintf(Map, sizeof(Map), "0 %u 1", Uid);
  if (WriteText("/proc/self/uid_map", Map) < 0) return -1;
  snprintf(Map, sizeof(Map), "0 %u 1", Gid);
  if (WriteText("/proc/self/gid_map", Map) < 0) return -1;
  return 0;
}
#define ENTER_USERNS(flags) do {                                                             \
    if (EnterUserNs(flags) < 0) {                                                            \
      if (!GuestRuntime && (errno == EPERM || errno == ENOSPC)) SKIP("user namespaces unavailable"); \
      Fail(__LINE__, "EnterUserNs(" #flags ")", -1, errno, 0);                               \
      return;                                                                                \
    }                                                                                        \
  } while (0)

// Mounts in a new mount namespace must not propagate back (Linux: after
// CLONE_NEWNS from a user namespace the copies are already locked/slave).
static void MakeRootPrivate(void) { CHECK_OK(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL)); }

static unsigned ParseUnsigned(const char **Cursor) {
  while (**Cursor == ' ' || **Cursor == '\t') ++*Cursor;
  unsigned Value = 0;
  while (**Cursor >= '0' && **Cursor <= '9') Value = Value * 10 + (unsigned) (*(*Cursor)++ - '0');
  return Value;
}

static int Exists(const char *Path) { struct stat Info; return lstat(Path, &Info) == 0; }

static char *Absolute(const char *Name, char *Buffer, size_t Size) {
  char Here[PATH_MAX];
  if (!getcwd(Here, sizeof(Here))) return NULL;
  const size_t Used = strlen(Here), Extra = strlen(Name);
  if (Used + 1 + Extra + 1 > Size) return NULL;
  memcpy(Buffer, Here, Used);
  Buffer[Used] = '/';
  memcpy(Buffer + Used + 1, Name, Extra + 1);
  return Buffer;
}

// ---------------------------------------------------------------------------
// sched/: scheduling policy, priority and CPU affinity

TEST(sched, getscheduler_default) {
  CHECK(sched_getscheduler(0) == SCHED_OTHER);
  struct sched_param Param = {.sched_priority = 7};
  CHECK_OK(sched_getparam(0, &Param));
  CHECK(Param.sched_priority == 0);
}

TEST(sched, priority_range) {
  CHECK(sched_get_priority_max(SCHED_FIFO) == 99);
  CHECK(sched_get_priority_min(SCHED_FIFO) == 1);
  CHECK(sched_get_priority_max(SCHED_RR) == 99);
  CHECK(sched_get_priority_max(SCHED_OTHER) == 0);
  CHECK(sched_get_priority_min(SCHED_BATCH) == 0);
  CHECK_ERRNO(sched_get_priority_max(42), EINVAL);
}

static void RtDeniedBody(void) {
  const struct rlimit None = {0, 0};
  CHECK_OK(setrlimit(RLIMIT_RTPRIO, &None));
  const struct sched_param Param = {.sched_priority = 1};
  CHECK_ERRNO(sched_setscheduler(0, SCHED_FIFO, &Param), EPERM);
  CHECK(sched_getscheduler(0) == SCHED_OTHER);
}
TEST(sched, setscheduler_rt_eperm_user) { AsUser(RtDeniedBody); }

TEST(sched, setscheduler_batch_roundtrip) {
  const struct sched_param Param = {.sched_priority = 0};
  CHECK_OK(sched_setscheduler(0, SCHED_BATCH, &Param));
  CHECK(sched_getscheduler(0) == SCHED_BATCH);
  CHECK_OK(sched_setscheduler(0, SCHED_IDLE, &Param));
  CHECK(sched_getscheduler(0) == SCHED_IDLE);
  const struct sched_param Bad = {.sched_priority = 5};
  CHECK_ERRNO(sched_setscheduler(0, SCHED_BATCH, &Bad), EINVAL);
}

TEST(sched, getparam_errors) {
  struct sched_param Param;
  CHECK_ERRNO(sched_getparam(-1, &Param), EINVAL);
  CHECK_ERRNO(sched_getparam(0x3ffffff0, &Param), ESRCH);
  CHECK_ERRNO(sched_getscheduler(-1), EINVAL);
}

TEST(sched, rr_interval) {
  struct timespec Interval = {-1, -1};
  CHECK_OK(sched_rr_get_interval(0, &Interval));
  CHECK(Interval.tv_sec >= 0 && Interval.tv_nsec >= 0);
}

TEST(sched, reset_on_fork) {
  const struct sched_param Param = {.sched_priority = 0};
  CHECK_OK(sched_setscheduler(0, SCHED_BATCH | SCHED_RESET_ON_FORK, &Param));
  CHECK(sched_getscheduler(0) == (SCHED_BATCH | SCHED_RESET_ON_FORK));
  int Pipe[2];
  CHECK_OK(pipe(Pipe));
  const pid_t Child = fork();
  if (Child == 0) {
    const int Policy = sched_getscheduler(0);
    _exit(write(Pipe[1], &Policy, sizeof(Policy)) == sizeof(Policy) ? 0 : 1);
  }
  int Policy = -1;
  CHECK(read(Pipe[0], &Policy, sizeof(Policy)) == sizeof(Policy));
  // Reset-on-fork only turns real-time and deadline policies (and a negative
  // nice) into SCHED_OTHER; BATCH is kept, and the child's bit is cleared.
  CHECK(Policy == SCHED_BATCH);
  CHECK(WaitExit(Child) == 0);
}

TEST(sched, affinity_roundtrip) {
  cpu_set_t Set;
  CPU_ZERO(&Set);
  CHECK_OK(sched_getaffinity(0, sizeof(Set), &Set));
  CHECK(CPU_COUNT(&Set) >= 1 && CPU_ISSET(0, &Set));
  cpu_set_t One;
  CPU_ZERO(&One);
  CPU_SET(0, &One);
  CHECK_OK(sched_setaffinity(0, sizeof(One), &One));
  cpu_set_t Now;
  CPU_ZERO(&Now);
  CHECK_OK(sched_getaffinity(0, sizeof(Now), &Now));
  CHECK(CPU_COUNT(&Now) == 1 && CPU_ISSET(0, &Now));
  // Inherited by fork.
  int Pipe[2];
  CHECK_OK(pipe(Pipe));
  const pid_t Child = fork();
  if (Child == 0) {
    cpu_set_t Seen;
    CPU_ZERO(&Seen);
    const int Count = sched_getaffinity(0, sizeof(Seen), &Seen) == 0 ? CPU_COUNT(&Seen) : -1;
    _exit(write(Pipe[1], &Count, sizeof(Count)) == sizeof(Count) ? 0 : 1);
  }
  int Count = -1;
  CHECK(read(Pipe[0], &Count, sizeof(Count)) == sizeof(Count));
  CHECK(Count == 1);
  CHECK(WaitExit(Child) == 0);
}

TEST(sched, affinity_errors) {
  cpu_set_t Empty;
  CPU_ZERO(&Empty);
  CHECK_ERRNO(sched_setaffinity(0, sizeof(Empty), &Empty), EINVAL);
  cpu_set_t Set;
  CHECK_ERRNO(syscall(SYS_sched_getaffinity, 0, 1, &Set), EINVAL);
  CHECK_ERRNO(sched_getaffinity(0x3ffffff0, sizeof(Set), &Set), ESRCH);
}

// struct sched_attr is not in Debian 12's headers.
struct SchedAttr {
  uint32_t Size, Policy;
  uint64_t Flags;
  int32_t Nice;
  uint32_t Priority;
  uint64_t Runtime, Deadline, Period;
  uint32_t UtilMin, UtilMax;
};

TEST(sched, setattr_nice_and_getattr) {
  struct SchedAttr Attr = {.Size = 48, .Policy = SCHED_OTHER, .Nice = 5};
  CHECK_OK(syscall(SYS_sched_setattr, 0, &Attr, 0));
  CHECK(getpriority(PRIO_PROCESS, 0) == 5);
  struct SchedAttr Out;
  memset(&Out, 0xff, sizeof(Out));
  CHECK_OK(syscall(SYS_sched_getattr, 0, &Out, sizeof(Out), 0));
  CHECK(Out.Policy == SCHED_OTHER && Out.Nice == 5 && Out.Priority == 0);
  CHECK(Out.Size == sizeof(Out));
  CHECK_ERRNO(syscall(SYS_sched_getattr, 0, &Out, 8, 0), EINVAL);
  CHECK_ERRNO(syscall(SYS_sched_getattr, 0, &Out, sizeof(Out), 1), EINVAL);
}

// ---------------------------------------------------------------------------
// ioprio/ and kcmp/

#define IOPRIO_WHO_PROCESS 1
#define IOPRIO_CLASS_RT 1
#define IOPRIO_CLASS_BE 2
#define IOPRIO_VALUE(Class, Level) (((Class) << 13) | (Level))

TEST(ioprio, set_get_roundtrip) {
  CHECK_OK(syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_VALUE(IOPRIO_CLASS_BE, 7)));
  CHECK(syscall(SYS_ioprio_get, IOPRIO_WHO_PROCESS, 0) == IOPRIO_VALUE(IOPRIO_CLASS_BE, 7));
  CHECK_ERRNO(syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_VALUE(5, 0)), EINVAL);
  CHECK_ERRNO(syscall(SYS_ioprio_get, 42, 0), EINVAL);
}

static void IoprioRtBody(void) {
  CHECK_ERRNO(syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_VALUE(IOPRIO_CLASS_RT, 0)), EPERM);
}
TEST(ioprio, rt_eperm_user) { AsUser(IoprioRtBody); }

#define KCMP_FILE 0
#define KCMP_VM 1
#define KCMP_FILES 2

TEST(kcmp, file_and_vm) {
  const pid_t Self = getpid();
  if (syscall(SYS_kcmp, Self, Self, KCMP_FILE, 0, 0) < 0 && errno == ENOSYS && !GuestRuntime) SKIP("kernel without kcmp");
  const int Fd = open(".", O_RDONLY | O_DIRECTORY);
  const int Copy = dup(Fd);
  const int Other = open(".", O_RDONLY | O_DIRECTORY);
  CHECK(syscall(SYS_kcmp, Self, Self, KCMP_FILE, Fd, Copy) == 0);
  const long Different = syscall(SYS_kcmp, Self, Self, KCMP_FILE, Fd, Other);
  CHECK(Different == 1 || Different == 2);
  CHECK_ERRNO(syscall(SYS_kcmp, Self, Self, KCMP_FILE, Fd, 999), EBADF);
  int Pipe[2];
  CHECK_OK(pipe(Pipe));
  const pid_t Child = fork();
  if (Child == 0) {
    char Byte;
    _exit(read(Pipe[0], &Byte, 1) == 1 ? 0 : 1);
  }
  const long Vm = syscall(SYS_kcmp, Self, Child, KCMP_VM, 0, 0);
  CHECK(Vm == 1 || Vm == 2);
  CHECK(syscall(SYS_kcmp, Self, Self, KCMP_FILES, 0, 0) == 0);
  CHECK(write(Pipe[1], "x", 1) == 1);
  CHECK(WaitExit(Child) == 0);
}

// ---------------------------------------------------------------------------
// deny/: machine-wide calls fail EPERM for everybody in a container

TEST(deny, reboot) {
  errno = 0;
  const long Result = syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_CAD_OFF, NULL);
  const int Error = errno;
  // The guest always answers EPERM. Natively, a root that owns a nested PID
  // namespace (RunPackagedNative.sh --root) holds CAP_SYS_BOOT there and
  // reaches the "not the initial PID namespace" EINVAL instead.
  if (Result != -1 || (Error != EPERM && !(Error == EINVAL && !GuestRuntime))) Fail(__LINE__, "reboot(CAD_OFF)", Result, Error, EPERM);
}
TEST(deny, swapon_swapoff) {
  CHECK_ERRNO(swapon("/nonexistent-swap", 0), EPERM);
  CHECK_ERRNO(swapoff("/nonexistent-swap"), EPERM);
}
TEST(deny, acct) { CHECK_ERRNO(acct(NULL), EPERM); }
TEST(deny, modules) {
  CHECK_ERRNO(syscall(SYS_init_module, NULL, 0, ""), EPERM);
  CHECK_ERRNO(syscall(SYS_finit_module, -1, "", 0), EPERM);
  CHECK_ERRNO(syscall(SYS_delete_module, "nonexistent", 0), EPERM);
}
TEST(deny, kexec) { CHECK_ERRNO(syscall(SYS_kexec_load, 0, 0, NULL, 0), EPERM); }
TEST(deny, iopl_ioperm) {
  CHECK_ERRNO(syscall(SYS_iopl, 3), EPERM);
  CHECK_ERRNO(syscall(SYS_ioperm, 0x80, 1, 1), EPERM);
}
TEST(deny, vhangup) { CHECK_ERRNO(vhangup(), EPERM); }
TEST(deny, syslog_read) {
  char Value[8];
  if (!GuestRuntime && (ReadText("/proc/sys/kernel/dmesg_restrict", Value, sizeof(Value)) < 0 || Value[0] != '1'))
    SKIP("dmesg_restrict is not 1 on this host");
  if (geteuid() == 0 && GuestRuntime) {
    // Guest root lacks CAP_SYSLOG? No: Linux root keeps CAP_SYSLOG; the
    // runtime keeps the kernel log private for everyone (inventory).
  }
  char Buffer[64];
  CHECK_ERRNO(syscall(SYS_syslog, 3, Buffer, sizeof(Buffer)), EPERM);
}
TEST(deny, ptrace_traceme) {
  GUEST_ONLY();
  CHECK_ERRNO(syscall(SYS_ptrace, 0 /* PTRACE_TRACEME */, 0, 0, 0), EPERM);
}
TEST(deny, enosys_features) {
  GUEST_ONLY();
  CHECK_ERRNO(syscall(SYS_io_setup, 1, NULL), ENOSYS);
  CHECK_ERRNO(syscall(SYS_io_uring_setup, 1, NULL), ENOSYS);
  CHECK_ERRNO(syscall(SYS_add_key, "user", "k", "v", 1, 0), ENOSYS);
  CHECK_ERRNO(syscall(SYS_keyctl, 0, 0), ENOSYS);
  CHECK_ERRNO(syscall(SYS_rseq, NULL, 0, 0, 0), ENOSYS);
  CHECK_ERRNO(syscall(SYS_fsopen, "tmpfs", 0), ENOSYS);
  CHECK_ERRNO(syscall(SYS_open_tree, AT_FDCWD, ".", 0), ENOSYS);
  CHECK_ERRNO(syscall(SYS_landlock_create_ruleset, NULL, 0, 1), ENOSYS);
}
TEST(deny, capability_bounding_set) {
  GUEST_ONLY();
  // CAP_SYS_MODULE 16, CAP_SYS_RAWIO 17, CAP_SYS_BOOT 22 are never available.
  CHECK(prctl(PR_CAPBSET_READ, 16) == 0);
  CHECK(prctl(PR_CAPBSET_READ, 17) == 0);
  CHECK(prctl(PR_CAPBSET_READ, 22) == 0);
  CHECK(prctl(PR_CAPBSET_READ, 21) == 1);
}

// ---------------------------------------------------------------------------
// ns/: user, UTS, IPC and PID namespaces

TEST(ns, user_overflow_before_map) {
  char Text[32];
  CHECK(ReadText("/proc/sys/kernel/overflowuid", Text, sizeof(Text)) > 0 && !strcmp(Text, "65534\n"));
  CHECK(ReadText("/proc/sys/kernel/overflowgid", Text, sizeof(Text)) > 0 && !strcmp(Text, "65534\n"));
  if (unshare(CLONE_NEWUSER) < 0) {
    if (!GuestRuntime) SKIP("user namespaces unavailable");
    Fail(__LINE__, "unshare(CLONE_NEWUSER)", -1, errno, 0);
    return;
  }
  CHECK(getuid() == 65534 && getgid() == 65534);
}

TEST(ns, user_map_root) {
  const uid_t Outer = getuid();
  int Pipe[2];
  CHECK_OK(pipe(Pipe));
  ENTER_USERNS(0);
  CHECK(getuid() == 0 && geteuid() == 0 && getgid() == 0);
  char Status[4096];
  CHECK(ReadText("/proc/self/status", Status, sizeof(Status)) > 0);
  CHECK(strstr(Status, "CapEff:\t0000000000000000") == NULL);
  char Map[64];
  CHECK(ReadText("/proc/self/uid_map", Map, sizeof(Map)) > 0);
  unsigned Inside = 9, OutsideId = 9, Length = 0;
  // Hand parsed: the host headers map sscanf and strtoul to C23 symbols that
  // the Debian 12 libc lacks.
  const char *Cursor = Map;
  Inside = ParseUnsigned(&Cursor);
  OutsideId = ParseUnsigned(&Cursor);
  Length = ParseUnsigned(&Cursor);
  CHECK(Inside == 0 && OutsideId == Outer && Length == 1);
  // A file of the outer uid now belongs to root; one of an unmapped id to 65534.
  CHECK_OK(close(open("mine", O_CREAT | O_WRONLY, 0600)));
  struct stat Info;
  CHECK_OK(stat("mine", &Info));
  CHECK(Info.st_uid == 0);
  // The guest's "/" has no owner record and reports the default user (ContainerInventory.md), so this holds
  // on Linux only.
  if (Outer != 0 && !GuestRuntime) {
    CHECK_OK(stat("/", &Info));
    CHECK(Info.st_uid == 65534);
  }
}

TEST(ns, user_map_rules) {
  if (unshare(CLONE_NEWUSER) < 0) {
    if (!GuestRuntime) SKIP("user namespaces unavailable");
    Fail(__LINE__, "unshare(CLONE_NEWUSER)", -1, errno, 0);
    return;
  }
  char Map[64];
  // Unprivileged writers must deny setgroups before writing gid_map, and may
  // map only their own id (as root of the parent namespace both are allowed).
  snprintf(Map, sizeof(Map), "0 %u 1", (unsigned) getauxval(AT_EGID));
  const int GidFirst = WriteText("/proc/self/gid_map", Map);
  const int GidFirstError = errno;
  if (getauxval(AT_EUID) != 0) CHECK(GidFirst < 0 && GidFirstError == EPERM);
  CHECK_OK(WriteText("/proc/self/setgroups", "deny") == 0 ? 0 : -1);
  snprintf(Map, sizeof(Map), "0 %u 1", (unsigned) getauxval(AT_EUID));
  CHECK_OK(WriteText("/proc/self/uid_map", Map));
  CHECK_ERRNO(WriteText("/proc/self/uid_map", Map), EPERM);
  CHECK_ERRNO(WriteText("/proc/self/setgroups", "allow"), EPERM);
}

TEST(ns, uts_hostname) {
  char Before[65];
  CHECK_OK(gethostname(Before, sizeof(Before)));
  int Pipe[2];
  CHECK_OK(pipe(Pipe));
  const pid_t Child = fork();
  if (Child == 0) {
    if (EnterUserNs(CLONE_NEWUTS) < 0) _exit(errno == EPERM && !GuestRuntime ? 77 : 2);
    if (sethostname("box", 3) < 0) _exit(3);
    struct utsname Name;
    char Text[64];
    if (uname(&Name) < 0 || strcmp(Name.nodename, "box")) _exit(4);
    if (ReadText("/proc/sys/kernel/hostname", Text, sizeof(Text)) < 0 || strcmp(Text, "box\n")) _exit(5);
    _exit(0);
  }
  const int Status = WaitExit(Child);
  if (WIFEXITED(Status) && WEXITSTATUS(Status) == 77) SKIP("user namespaces unavailable");
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  char After[65];
  CHECK_OK(gethostname(After, sizeof(After)));
  CHECK(!strcmp(Before, After));
}

static void HostnameDeniedBody(void) { CHECK_ERRNO(sethostname("nope", 4), EPERM); }
TEST(ns, sethostname_eperm_initial) { AsUser(HostnameDeniedBody); }

TEST(ns, ipc_isolated) {
  const key_t Key = 0x4d340000 + (getpid() & 0xffff);
  const int Queue = msgget(Key, IPC_CREAT | 0600);
  CHECK(Queue >= 0);
  const pid_t Child = fork();
  if (Child == 0) {
    if (EnterUserNs(CLONE_NEWIPC) < 0) _exit(77);
    _exit(msgget(Key, 0) == -1 && errno == ENOENT ? 0 : 1);
  }
  const int Status = WaitExit(Child);
  msgctl(Queue, IPC_RMID, NULL);
  if (WIFEXITED(Status) && WEXITSTATUS(Status) == 77 && !GuestRuntime) SKIP("user namespaces unavailable");
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
}

TEST(ns, pid_init_numbers) {
  ENTER_USERNS(CLONE_NEWPID);
  int Pipe[2];
  CHECK_OK(pipe(Pipe));
  const pid_t Child = fork();
  if (Child == 0) {
    char Status[8192] = {0};
    const pid_t Pid = getpid(), Parent = getppid();
    int Ok = Pid == 1 && Parent == 0;
    // NSpid lists the outer pid first and 1 last.
    const ssize_t Length = ReadText("/proc/self/status", Status, sizeof(Status));
    const int ReadError = errno;
    if (Length < 0) Ok = 0;
    const char *Line = strstr(Status, "NSpid:");
    const char *End = Line ? strchr(Line, '\n') : NULL;
    if (!Line || !End || End - Line < 3 || strncmp(End - 2, "\t1", 2)) Ok = 0;
    if (!Ok) {
      printf("#   child: getpid %d getppid %d status read %zd (%s) NSpid line: %.*s\n", (int) Pid, (int) Parent, Length,
             Length < 0 ? ErrnoName(ReadError) : "ok", Line && End ? (int) (End - Line) : 6, Line ? Line : "absent");
      fflush(stdout);
    }
    _exit(Ok ? 0 : 1);
  }
  CHECK(Child > 1);
  CHECK(WaitExit(Child) == 0);
  // The namespace's init is gone: another fork fails with ENOMEM.
  const pid_t Again = fork();
  const int AgainError = errno;
  if (Again == 0) _exit(0);
  CHECK(Again == -1 && AgainError == ENOMEM);
}

TEST(ns, pid_orphan_reparents_to_init) {
  ENTER_USERNS(CLONE_NEWPID);
  const pid_t Init = fork();
  if (Init == 0) {
    int Pipe[2];
    if (pipe(Pipe) < 0) _exit(2);
    const pid_t Middle = fork();
    if (Middle == 0) {
      const pid_t Grand = fork();
      if (Grand == 0) {
        // Wait until the middle process is gone, then report the parent.
        for (int Try = 0; Try < 300 && getppid() != 1; ++Try) usleep(10000);
        const pid_t Parent = getppid();
        _exit(write(Pipe[1], &Parent, sizeof(Parent)) == sizeof(Parent) ? 0 : 1);
      }
      _exit(0);
    }
    waitpid(Middle, NULL, 0);
    pid_t Parent = 0;
    if (read(Pipe[0], &Parent, sizeof(Parent)) != sizeof(Parent)) _exit(3);
    while (wait(NULL) > 0) {}
    _exit(Parent == 1 ? 0 : 4);
  }
  CHECK(WaitExit(Init) == 0);
}

TEST(ns, pid_init_exit_kills_members) {
  ENTER_USERNS(CLONE_NEWPID);
  int Pipe[2];
  CHECK_OK(pipe(Pipe));
  const pid_t Init = fork();
  if (Init == 0) {
    const pid_t Member = fork();
    if (Member == 0) {
      close(Pipe[0]);
      for (;;) pause();
    }
    _exit(0);
  }
  close(Pipe[1]);
  CHECK(WaitExit(Init) == 0);
  // The member held the write end; it is killed, so the read sees EOF.
  struct pollfd Poll = {Pipe[0], POLLIN, 0};
  CHECK(poll(&Poll, 1, 3000) == 1);
  char Byte;
  CHECK(read(Pipe[0], &Byte, 1) == 0);
}

TEST(ns, setns_into_uts) {
  int Ready[2], Hold[2];
  CHECK_OK(pipe(Ready));
  CHECK_OK(pipe(Hold));
  const pid_t Owner = fork();
  if (Owner == 0) {
    close(Hold[1]);
    char Byte = EnterUserNs(CLONE_NEWUTS) == 0 && sethostname("nsA", 3) == 0 ? 'y' : 'n';
    if (write(Ready[1], &Byte, 1) != 1) _exit(1);
    read(Hold[0], &Byte, 1);
    _exit(0);
  }
  char Byte = 0;
  CHECK(read(Ready[0], &Byte, 1) == 1);
  if (Byte != 'y') {
    close(Hold[1]);
    WaitExit(Owner);
    if (!GuestRuntime) SKIP("user namespaces unavailable");
    Fail(__LINE__, "owner namespace setup", -1, 0, 0);
    return;
  }
  char Path[64];
  snprintf(Path, sizeof(Path), "/proc/%d/ns/user", Owner);
  const int UserFd = open(Path, O_RDONLY | O_CLOEXEC);
  snprintf(Path, sizeof(Path), "/proc/%d/ns/uts", Owner);
  const int UtsFd = open(Path, O_RDONLY | O_CLOEXEC);
  CHECK(UserFd >= 0 && UtsFd >= 0);
  CHECK(ioctl(UtsFd, NS_GET_NSTYPE) == CLONE_NEWUTS);
  const int Parent = ioctl(UtsFd, NS_GET_USERNS);
  CHECK(Parent >= 0);
  struct stat A, B;
  CHECK(fstat(Parent, &A) == 0 && fstat(UserFd, &B) == 0 && A.st_ino == B.st_ino);
  char Link[64];
  const ssize_t Length = readlink(Path, Link, sizeof(Link) - 1);
  CHECK(Length > 6 && !strncmp(Link, "uts:[", 5));
  CHECK_OK(setns(UserFd, CLONE_NEWUSER));
  CHECK_ERRNO(setns(UserFd, CLONE_NEWUTS), EINVAL);
  CHECK_OK(setns(UtsFd, CLONE_NEWUTS));
  struct utsname Name;
  CHECK(uname(&Name) == 0 && !strcmp(Name.nodename, "nsA"));
  close(Hold[1]);
  CHECK(WaitExit(Owner) == 0);
}

static void *Sleeper(void *Argument) {
  (void) Argument;
  pause();
  return NULL;
}
TEST(ns, unshare_user_multithreaded_einval) {
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, Sleeper, NULL) == 0);
  CHECK_ERRNO(unshare(CLONE_NEWUSER), EINVAL);
}

TEST(ns, newnet_eperm) {
  GUEST_ONLY();
  ENTER_USERNS(0);
  CHECK_ERRNO(unshare(CLONE_NEWNET), EPERM);
}

// ---------------------------------------------------------------------------
// mnt/: mount namespaces, mount, umount2, pivot_root, chroot

TEST(mnt, tmpfs_mount_and_umount) {
  ENTER_USERNS(CLONE_NEWNS);
  MakeRootPrivate();
  CHECK_OK(mkdir("t", 0755));
  CHECK_OK(mount("none", "t", "tmpfs", 0, "size=1m,mode=0700"));
  struct stat Info;
  CHECK(stat("t", &Info) == 0 && (Info.st_mode & 07777) == 0700);
  CHECK_OK(close(open("t/inside", O_CREAT | O_WRONLY, 0600)));
  char Text[16384];
  CHECK(ReadText("/proc/self/mountinfo", Text, sizeof(Text)) > 0);
  CHECK(strstr(Text, " - tmpfs ") != NULL);
  struct statfs Fs;
  CHECK(statfs("t", &Fs) == 0 && Fs.f_type == 0x01021994);
  CHECK_OK(umount2("t", 0));
  CHECK(!Exists("t/inside"));
}

TEST(mnt, bind_dir_and_file) {
  ENTER_USERNS(CLONE_NEWNS);
  MakeRootPrivate();
  CHECK_OK(mkdir("a", 0755));
  CHECK_OK(mkdir("b", 0755));
  CHECK_OK(WriteText("a/f", "x") == 0 || (close(open("a/f", O_CREAT | O_WRONLY, 0644)) == 0 && WriteText("a/f", "bound") == 0) ? 0 : -1);
  CHECK_OK(mount("a", "b", NULL, MS_BIND, NULL));
  char Text[16];
  CHECK(ReadText("b/f", Text, sizeof(Text)) == 5 && !strcmp(Text, "bound"));
  CHECK_OK(close(open("target", O_CREAT | O_WRONLY, 0644)));
  CHECK_OK(mount("a/f", "target", NULL, MS_BIND, NULL));
  CHECK(ReadText("target", Text, sizeof(Text)) == 5 && !strcmp(Text, "bound"));
  // /dev/null bound onto a file, as bubblewrap's --dev does.
  CHECK_OK(close(open("null", O_CREAT | O_WRONLY, 0644)));
  CHECK_OK(mount("/dev/null", "null", NULL, MS_BIND, NULL));
  struct stat Info;
  CHECK(stat("null", &Info) == 0 && S_ISCHR(Info.st_mode) && Info.st_rdev == makedev(1, 3));
  CHECK_OK(umount2("b", 0));
  CHECK(!Exists("b/f"));
}

TEST(mnt, remount_readonly) {
  ENTER_USERNS(CLONE_NEWNS);
  MakeRootPrivate();
  CHECK_OK(mkdir("a", 0755));
  CHECK_OK(mkdir("b", 0755));
  CHECK_OK(close(open("a/f", O_CREAT | O_WRONLY, 0644)));
  CHECK_OK(mount("a", "b", NULL, MS_BIND, NULL));
  CHECK_OK(mount(NULL, "b", NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL));
  CHECK_ERRNO(open("b/f", O_WRONLY), EROFS);
  CHECK_ERRNO(mkdir("b/d", 0755), EROFS);
  const int Fd = open("a/f", O_WRONLY);
  CHECK(Fd >= 0);
  close(Fd);
  struct statvfs Vfs;
  CHECK(statvfs("b", &Vfs) == 0 && (Vfs.f_flag & ST_RDONLY));
}

static void MountDeniedBody(void) {
  CHECK_OK(mkdir("denied", 0755));
  CHECK_ERRNO(mount("none", "denied", "tmpfs", 0, NULL), EPERM);
  CHECK_ERRNO(umount2("/", 0), EPERM);
  CHECK_ERRNO(chroot("."), EPERM);
}
TEST(mnt, eperm_without_namespace) { AsUser(MountDeniedBody); }

TEST(mnt, umount_busy_and_detach) {
  ENTER_USERNS(CLONE_NEWNS);
  MakeRootPrivate();
  CHECK_OK(mkdir("t", 0755));
  CHECK_OK(mount("none", "t", "tmpfs", 0, NULL));
  CHECK_OK(chdir("t"));
  CHECK_ERRNO(umount2("../t", 0), EBUSY);
  CHECK_OK(chdir(".."));
  const int Fd = open("t/kept", O_CREAT | O_RDWR, 0600);
  CHECK(Fd >= 0 && write(Fd, "abc", 3) == 3);
  CHECK_OK(umount2("t", MNT_DETACH));
  CHECK(!Exists("t/kept"));
  char Text[4] = {0};
  CHECK(pread(Fd, Text, 3, 0) == 3 && !strcmp(Text, "abc"));
  CHECK_ERRNO(umount2("t", 0), EINVAL);
  CHECK_ERRNO(mount("none", "missing-dir", "tmpfs", 0, NULL), ENOENT);
  CHECK_ERRNO(mount("none", "t", "no-such-fs", 0, NULL), ENODEV);
}

TEST(mnt, namespace_isolation) {
  CHECK_OK(mkdir("t", 0755));
  const pid_t Child = fork();
  if (Child == 0) {
    if (EnterUserNs(CLONE_NEWNS) < 0) _exit(77);
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) _exit(2);
    if (mount("none", "t", "tmpfs", 0, NULL) < 0) _exit(3);
    _exit(close(open("t/only-in-child", O_CREAT | O_WRONLY, 0600)) == 0 ? 0 : 4);
  }
  const int Status = WaitExit(Child);
  if (WIFEXITED(Status) && WEXITSTATUS(Status) == 77 && !GuestRuntime) SKIP("user namespaces unavailable");
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  CHECK(!Exists("t/only-in-child"));
}

TEST(mnt, pivot_root) {
  ENTER_USERNS(CLONE_NEWNS);
  MakeRootPrivate();
  char New[PATH_MAX], Old[PATH_MAX];
  CHECK_OK(mkdir("newroot", 0755));
  CHECK_ERRNO(syscall(SYS_pivot_root, Absolute("newroot", New, sizeof(New)), Absolute("newroot", Old, sizeof(Old))), EINVAL);
  CHECK_OK(mount("none", "newroot", "tmpfs", 0, NULL));
  CHECK_OK(mkdir("newroot/old", 0755));
  CHECK_OK(close(open("newroot/marker", O_CREAT | O_WRONLY, 0644)));
  CHECK_OK(syscall(SYS_pivot_root, Absolute("newroot", New, sizeof(New)), Absolute("newroot/old", Old, sizeof(Old))));
  CHECK_OK(chdir("/"));
  CHECK(Exists("/marker"));
  CHECK(Exists("/old/proc"));
  CHECK_OK(umount2("/old", MNT_DETACH));
  CHECK(!Exists("/old/proc"));
  CHECK(Exists("/old"));
  // ".." of the new root stays at the root.
  struct stat Root, Up;
  CHECK(stat("/", &Root) == 0 && stat("/..", &Up) == 0 && Root.st_ino == Up.st_ino);
}

TEST(mnt, chroot_in_user_namespace) {
  ENTER_USERNS(0);
  CHECK_OK(mkdir("jail", 0755));
  CHECK_OK(mkdir("jail/sub", 0755));
  CHECK_OK(close(open("jail/inside", O_CREAT | O_WRONLY, 0644)));
  CHECK_OK(chroot("jail"));
  CHECK_OK(chdir("/"));
  CHECK(Exists("/inside"));
  CHECK(Exists("/../inside"));
  CHECK(Exists("/sub/../../inside"));
  CHECK(!Exists("/proc/self"));
}

TEST(mnt, proc_mount_follows_pid_namespace) {
  ENTER_USERNS(CLONE_NEWNS | CLONE_NEWPID);
  MakeRootPrivate();
  CHECK_OK(mkdir("p", 0755));
  const pid_t Init = fork();
  if (Init == 0) {
    if (mount("proc", "p", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0) _exit(2);
    DIR *Dir = opendir("p");
    if (!Dir) _exit(3);
    int Pids = 0, One = 0;
    for (struct dirent *Entry; (Entry = readdir(Dir));) {
      if (Entry->d_name[0] < '0' || Entry->d_name[0] > '9') continue;
      ++Pids;
      if (!strcmp(Entry->d_name, "1")) One = 1;
    }
    closedir(Dir);
    char Link[32] = {0};
    if (readlink("p/self", Link, sizeof(Link) - 1) < 0 || strcmp(Link, "1")) _exit(4);
    _exit(Pids == 1 && One ? 0 : 5);
  }
  CHECK(WaitExit(Init) == 0);
}

// ---------------------------------------------------------------------------
// seccomp/: strict and filter mode

#define ALLOW_REST BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
#define LOAD_ARCH BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch))
#define LOAD_NR BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr))

// Filter: on x86-64, Action for getppid, allow everything else.
static int InstallGetppidFilter(uint32_t Action, unsigned Flags) {
  struct sock_filter Code[] = {
    LOAD_ARCH,
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
    LOAD_NR,
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, Action),
    ALLOW_REST,
  };
  struct sock_fprog Program = {sizeof(Code) / sizeof(Code[0]), Code};
  return (int) syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, Flags, &Program);
}

TEST(seccomp, strict_mode) {
  const pid_t Child = fork();
  if (Child == 0) {
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT) < 0) syscall(SYS_exit, 2);
    syscall(SYS_write, 1, "", 0);
    syscall(SYS_getppid);
    syscall(SYS_exit, 3);
  }
  const int Status = WaitExit(Child);
  CHECK(WIFSIGNALED(Status) && WTERMSIG(Status) == SIGKILL);
}

static void NeedsNoNewPrivsBody(void) {
  CHECK(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 0);
  CHECK_ERRNO(InstallGetppidFilter(SECCOMP_RET_ERRNO | ENOMSG, 0), EACCES);
}
TEST(seccomp, filter_needs_no_new_privs) { AsUser(NeedsNoNewPrivsBody); }

TEST(seccomp, filter_errno_and_status) {
  CHECK_OK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
  CHECK_OK(InstallGetppidFilter(SECCOMP_RET_ERRNO | ENOMSG, 0));
  CHECK_ERRNO(syscall(SYS_getppid), ENOMSG);
  CHECK(syscall(SYS_getpid) == getpid());
  CHECK(prctl(PR_GET_SECCOMP) == 2);
  char Status[4096];
  CHECK(ReadText("/proc/self/status", Status, sizeof(Status)) > 0);
  CHECK(strstr(Status, "NoNewPrivs:\t1\n") && strstr(Status, "Seccomp:\t2\n") && strstr(Status, "Seccomp_filters:\t1\n"));
  CHECK_ERRNO(prctl(PR_SET_NO_NEW_PRIVS, 0, 0, 0, 0), EINVAL);
}

static volatile sig_atomic_t TrapCode, TrapSyscall, TrapErrno;
static volatile unsigned TrapArch;
static void OnSigsys(int Signal, siginfo_t *Info, void *Context) {
  (void) Signal;
  (void) Context;
  TrapCode = Info->si_code;
  TrapSyscall = Info->si_syscall;
  TrapArch = Info->si_arch;
  TrapErrno = Info->si_errno;
}
TEST(seccomp, filter_trap_siginfo) {
  struct sigaction Action = {.sa_sigaction = OnSigsys, .sa_flags = SA_SIGINFO};
  sigemptyset(&Action.sa_mask);
  CHECK_OK(sigaction(SIGSYS, &Action, NULL));
  CHECK_OK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
  CHECK_OK(InstallGetppidFilter(SECCOMP_RET_TRAP | 7, 0));
  syscall(SYS_getppid);
  CHECK(TrapCode == 1 /* SYS_SECCOMP */);
  CHECK(TrapSyscall == SYS_getppid);
  CHECK(TrapArch == AUDIT_ARCH_X86_64);
  CHECK(TrapErrno == 7);
}

TEST(seccomp, filter_kill_process) {
  const pid_t Child = fork();
  if (Child == 0) {
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || InstallGetppidFilter(SECCOMP_RET_KILL_PROCESS, 0)) _exit(2);
    syscall(SYS_getppid);
    _exit(3);
  }
  const int Status = WaitExit(Child);
  CHECK(WIFSIGNALED(Status) && WTERMSIG(Status) == SIGSYS);
}

TEST(seccomp, filter_inherited_and_kept_across_exec) {
  CHECK_OK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
  CHECK_OK(InstallGetppidFilter(SECCOMP_RET_ERRNO | ENOMSG, 0));
  const pid_t Child = fork();
  if (Child == 0) _exit(syscall(SYS_getppid) == -1 && errno == ENOMSG ? 0 : 1);
  CHECK(WaitExit(Child) == 0);
  char Self[PATH_MAX];
  const ssize_t Length = readlink("/proc/self/exe", Self, sizeof(Self) - 1);
  CHECK(Length > 0);
  Self[Length > 0 ? Length : 0] = 0;
  const pid_t Exec = fork();
  if (Exec == 0) {
    execl(Self, Self, "--getppid-probe", (char *) NULL);
    _exit(127);
  }
  CHECK(WaitExit(Exec) == 0);
}

TEST(seccomp, action_avail_and_bad_filters) {
  uint32_t Action = SECCOMP_RET_KILL_PROCESS;
  CHECK_OK(syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &Action));
  Action = SECCOMP_RET_ERRNO;
  CHECK_OK(syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &Action));
  Action = SECCOMP_RET_TRAP;
  CHECK_OK(syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &Action));
  Action = SECCOMP_RET_LOG;
  CHECK_OK(syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &Action));
  Action = 0x12340000;
  CHECK_ERRNO(syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &Action), EOPNOTSUPP);
  CHECK_OK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
  struct sock_filter NoReturn[] = {LOAD_NR};
  struct sock_fprog Program = {1, NoReturn};
  CHECK_ERRNO(syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &Program), EINVAL);
  Program.len = 0;
  CHECK_ERRNO(syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &Program), EINVAL);
  struct sock_filter OutOfRange[] = {BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 4096), ALLOW_REST};
  Program = (struct sock_fprog) {2, OutOfRange};
  CHECK_ERRNO(syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &Program), EINVAL);
  CHECK_ERRNO(syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0x80000000u, &Program), EINVAL);
  CHECK(prctl(PR_GET_SECCOMP) == 0);
}

TEST(seccomp, precedence_most_recent_errno) {
  CHECK_OK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
  CHECK_OK(InstallGetppidFilter(SECCOMP_RET_ERRNO | 5, 0));
  CHECK_OK(InstallGetppidFilter(SECCOMP_RET_ERRNO | 6, 0));
  CHECK_ERRNO(syscall(SYS_getppid), 6);
  CHECK_OK(InstallGetppidFilter(SECCOMP_RET_TRAP, 0));
  struct sigaction Action = {.sa_sigaction = OnSigsys, .sa_flags = SA_SIGINFO};
  sigemptyset(&Action.sa_mask);
  CHECK_OK(sigaction(SIGSYS, &Action, NULL));
  TrapCode = 0;
  syscall(SYS_getppid);
  CHECK(TrapCode == 1);
}

static volatile int ThreadSaw = -1;
static void *CallGetppid(void *Argument) {
  int *Go = Argument;
  while (!__atomic_load_n(Go, __ATOMIC_SEQ_CST)) usleep(1000);
  errno = 0;
  ThreadSaw = syscall(SYS_getppid) == -1 ? errno : 0;
  return NULL;
}
TEST(seccomp, tsync_applies_to_threads) {
  int Go = 0;
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, CallGetppid, &Go) == 0);
  CHECK_OK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
  CHECK(InstallGetppidFilter(SECCOMP_RET_ERRNO | ENOMSG, SECCOMP_FILTER_FLAG_TSYNC) == 0);
  __atomic_store_n(&Go, 1, __ATOMIC_SEQ_CST);
  CHECK(pthread_join(Thread, NULL) == 0);
  CHECK(ThreadSaw == ENOMSG);
}
