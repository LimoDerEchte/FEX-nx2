// SPDX-License-Identifier: MIT
// Process and event compatibility suite for the Switch 2 guest runtime.
//
// Results use TAP-style lines so a console log can be compared against the
// native Linux reference run. Expectations follow Linux; tests that depend on
// privilege or features skip with a reason instead of passing silently.
//
// Every test runs in its own forked child of the harness so a test that
// kills, stops or changes credentials cannot affect the next one. The child
// calls setpgid(0, 0), runs the test and exits with
//   0   pass
//   1   one or more CHECKs failed
//   77  skipped (the reason travels to the parent in shared memory)
// The parent waits at most 10 s. On expiry it sends SIGKILL to the child's
// process group and reports "not ok <name> # timeout". After a normal exit
// it also kills the group so stray helper processes do not leak.
//
// Helper children made with Fork() exit 0/1 the same way, and exit 77 when
// they call SKIP, so WaitExit callers can tell a skip from a pass.
// Limit: helpers that leave the test's process group (setsid, setpgid
// elsewhere) must be cleaned up by the test itself; the harness is not a
// subreaper. Every test starts with default signal dispositions and an
// empty signal mask.
//
// Usage: proc-suite [pattern...] | proc-suite -l
//   pattern   run tests whose name starts with the pattern ("proc/", "signal/kill")
//   !pattern  exclude tests whose name starts with the pattern
//   -l        list test names
// Exit status: 42 when every selected test passes or skips, 1 otherwise.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/capability.h>
#include <linux/futex.h>
#include <poll.h>
#include <pthread.h>
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

static char ExePath[PATH_MAX];
static int CredentialExecCheck(const char *Mode);

int main(int argc, char **argv) {
  // cred/exec_* re-execute this binary to inspect the capabilities exec gave it.
  if (argc == 3 && !strcmp(argv[1], "--cred-exec-check")) return CredentialExecCheck(argv[2]);
  // timer/alarm_survives_exec: the alarm left after exec is the exit status.
  if (argc == 2 && !strcmp(argv[1], "--alarm-left")) return (int)alarm(0);
  // proc/rlimit_inherited_by_fork_and_exec: the RLIMIT_NOFILE soft limit.
  if (argc == 2 && !strcmp(argv[1], "--nofile-soft")) {
    struct rlimit Limit;
    return getrlimit(RLIMIT_NOFILE, &Limit) ? 255 : (int)Limit.rlim_cur;
  }
  {
    const ssize_t Length = readlink("/proc/self/exe", ExePath, sizeof(ExePath) - 1);
    if (Length > 0) ExePath[Length] = 0;
    else snprintf(ExePath, sizeof(ExePath), "%s", argv[0]);
  }
  int List = 0, Count = 0;
  char **Patterns = calloc(argc, sizeof(char *));
  for (int Index = 1; Index < argc; ++Index) {
    if (!strcmp(argv[Index], "-l")) List = 1;
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
  char Scratch[] = "./proc-suite.XXXXXX";
  if (!mkdtemp(Scratch) || chdir(Scratch)) {
    printf("Bail out! cannot create a scratch directory: %s\n", ErrnoName(errno));
    return 1;
  }

  unsigned Selection = 0;
  for (unsigned Index = 0; Index < TestCount; ++Index) Selection += Selected(Tests[Index].Name, Count, Patterns);
  if (Count && !Selection) {
    printf("Bail out! no tests match the given patterns\n");
    return 1;
  }
  printf("TAP version 13\n1..%u\n# proc-suite uid %u, gid %u\n", Selection, getuid(), getgid());
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
  printf("# proc-suite summary: %u passed, %u failed, %u skipped\n", Passed, Failed, SkippedCount);
  char Remove[64];
  snprintf(Remove, sizeof(Remove), "rm -rf %s", Scratch);
  if (chdir("..") == 0 && system(Remove) != 0) printf("# could not remove %s\n", Scratch);
  return Failed ? 1 : 42;
}

// ---------------------------------------------------------------------------
// Process identity

TEST(proc, getpid_matches_gettid_in_main_thread) {
  CHECK(getpid() == syscall(SYS_gettid));
  CHECK(getppid() > 0);
}

// ---------------------------------------------------------------------------
// Signals: generation, queueing, delivery and default actions.

static volatile sig_atomic_t HandlerCalls;
static volatile int LastSignal;
static siginfo_t LastInfo;
static volatile pid_t HandlerTid;
static volatile int SeenSignals[8];
static volatile long SeenValues[8];
static volatile int SeenCount;

static void CountSignal(int Signal) {
  ++HandlerCalls;
  LastSignal = Signal;
  HandlerTid = gettid();
}

static void RecordInfo(int Signal, siginfo_t *Info, void *Context) {
  (void)Context;
  ++HandlerCalls;
  LastSignal = Signal;
  LastInfo = *Info;
  if (SeenCount < 8) {
    SeenSignals[SeenCount] = Signal;
    SeenValues[SeenCount] = (long)Info->si_value.sival_ptr;
    ++SeenCount;
  }
}

static void ResetRecorder(void) {
  HandlerCalls = 0;
  LastSignal = 0;
  HandlerTid = 0;
  SeenCount = 0;
  memset(&LastInfo, 0, sizeof(LastInfo));
}

static int Catch(int Signal, int Flags) {
  struct sigaction Action;
  memset(&Action, 0, sizeof(Action));
  if (Flags & SA_SIGINFO) Action.sa_sigaction = RecordInfo;
  else Action.sa_handler = CountSignal;
  Action.sa_flags = Flags;
  return sigaction(Signal, &Action, NULL);
}

static int Dispose(int Signal, void (*Handler)(int)) {
  struct sigaction Action;
  memset(&Action, 0, sizeof(Action));
  Action.sa_handler = Handler;
  return sigaction(Signal, &Action, NULL);
}

static int Block(int Signal) {
  sigset_t Set;
  sigemptyset(&Set);
  sigaddset(&Set, Signal);
  return sigprocmask(SIG_BLOCK, &Set, NULL);
}

static int Unblock(int Signal) {
  sigset_t Set;
  sigemptyset(&Set);
  sigaddset(&Set, Signal);
  return sigprocmask(SIG_UNBLOCK, &Set, NULL);
}

static void SleepMs(unsigned Milliseconds) {
  struct timespec Time = {Milliseconds / 1000, (long)(Milliseconds % 1000) * 1000000L};
  while (nanosleep(&Time, &Time) < 0 && errno == EINTR) {}
}

// Waits in sigsuspend with an empty mask until a handler has run.
static void SuspendUntilHandled(void) {
  sigset_t Empty;
  sigemptyset(&Empty);
  while (!HandlerCalls) sigsuspend(&Empty);
}

static void RaiseTerm(void) { raise(SIGTERM); }
static void RaiseQuit(void) { raise(SIGQUIT); }

TEST(signal, default_term_status) {
  const int Status = WaitExit(Fork(RaiseTerm));
  CHECK(WIFSIGNALED(Status) && WTERMSIG(Status) == SIGTERM);
}

TEST(signal, default_core_status) {
  // WCOREDUMP depends on whether a dump was written; only the signal is portable.
  const int Status = WaitExit(Fork(RaiseQuit));
  CHECK(WIFSIGNALED(Status) && WTERMSIG(Status) == SIGQUIT);
}

TEST(signal, default_ignore_chld_winch_urg) {
  CHECK(raise(SIGCHLD) == 0);
  CHECK(raise(SIGWINCH) == 0);
  CHECK(raise(SIGURG) == 0);
  CHECK(raise(SIGCONT) == 0);
}

static int ReadyPipe[2];

static void SignalReady(void) {
  const char Byte = 1;
  CHECK(write(ReadyPipe[1], &Byte, 1) == 1);
}

static void WaitReady(int Count) {
  char Byte;
  for (int I = 0; I < Count; ++I) CHECK(read(ReadyPipe[0], &Byte, 1) == 1);
}

// Helper: blocks SIGUSR1, reports ready, then waits for it with a handler.
static void AwaitUsr1(void) {
  ResetRecorder();
  CHECK(Catch(SIGUSR1, 0) == 0);
  CHECK(Block(SIGUSR1) == 0);
  SignalReady();
  SuspendUntilHandled();
  CHECK(LastSignal == SIGUSR1);
}

static void AwaitUsr1Forever(void) {
  for (;;) pause();
}

static pid_t GroupLeader;

static void AwaitUsr1InGroup(void) {
  CHECK(setpgid(0, GroupLeader ? GroupLeader : 0) == 0);
  AwaitUsr1();
}

TEST(signal, kill_errors) {
  CHECK_ERRNO(kill(getpid(), 65), EINVAL);
  CHECK_ERRNO(kill(INT_MAX, 0), ESRCH);
  CHECK_ERRNO(kill(INT_MIN, 0), ESRCH);
  CHECK_ERRNO(kill(INT_MAX, 65), ESRCH);
  if (geteuid() != 0) return;
  // An unprivileged sender may not signal a process of another user.
  const pid_t Target = Fork(AwaitUsr1Forever);
  CHECK(Target > 0);
  const pid_t Sender = fork();
  if (Sender == 0) {
    CaseFailures = 0;
    if (setresuid(1000, 1000, 1000) != 0) _exit(1);
    CHECK_ERRNO(kill(Target, 0), EPERM);
    CHECK_ERRNO(kill(Target, SIGTERM), EPERM);
    _exit(CaseFailures ? 1 : 0);
  }
  const int Status = WaitExit(Sender);
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  CHECK(kill(Target, SIGKILL) == 0);
  WaitExit(Target);
}

static void ExitAtOnce(void) {}

TEST(signal, kill_zombie_succeeds) {
  const pid_t Child = Fork(ExitAtOnce);
  CHECK(Child > 0);
  siginfo_t Info;
  CHECK(waitid(P_PID, Child, &Info, WEXITED | WNOWAIT) == 0);
  CHECK(kill(Child, 0) == 0);
  CHECK(kill(Child, SIGTERM) == 0);
  const int Status = WaitExit(Child);
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  CHECK_ERRNO(kill(Child, 0), ESRCH);
}

TEST(signal, kill_process_group) {
  CHECK(pipe(ReadyPipe) == 0);
  GroupLeader = 0;
  const pid_t First = Fork(AwaitUsr1InGroup);
  CHECK(First > 0);
  setpgid(First, First);
  GroupLeader = First;
  const pid_t Second = Fork(AwaitUsr1InGroup);
  CHECK(Second > 0);
  setpgid(Second, First);
  WaitReady(2);
  CHECK(kill(-First, SIGUSR1) == 0);
  int Status = WaitExit(First);
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  Status = WaitExit(Second);
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  CHECK_ERRNO(kill(-First, 0), ESRCH);
}

TEST(signal, kill_zero_targets_group) {
  CHECK(pipe(ReadyPipe) == 0);
  const pid_t Child = Fork(AwaitUsr1);
  CHECK(Child > 0);
  WaitReady(1);
  CHECK(Dispose(SIGUSR1, SIG_IGN) == 0);
  CHECK(kill(0, SIGUSR1) == 0);
  const int Status = WaitExit(Child);
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
}

TEST(signal, kill_minus_one_signal_zero) {
  // Signal 0 only checks; any real signal would reach every process of the user.
  CHECK(kill(-1, 0) == 0);
}

static void KillParentUsr1(void) {
  CHECK(kill(getppid(), SIGUSR1) == 0);
}

TEST(signal, siginfo_from_kill) {
  ResetRecorder();
  CHECK(Catch(SIGUSR1, SA_SIGINFO) == 0);
  CHECK(Block(SIGUSR1) == 0);
  const pid_t Child = Fork(KillParentUsr1);
  CHECK(Child > 0);
  const int Status = WaitExit(Child);
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  SuspendUntilHandled();
  CHECK(LastInfo.si_signo == SIGUSR1);
  CHECK(LastInfo.si_code == SI_USER);
  CHECK(LastInfo.si_pid == Child);
  CHECK(LastInfo.si_uid == getuid());
}

TEST(signal, sigqueue_value_and_rt_order) {
  ResetRecorder();
  CHECK(Catch(SIGRTMIN, SA_SIGINFO) == 0);
  CHECK(Catch(SIGRTMIN + 1, SA_SIGINFO) == 0);
  sigset_t Set, Old;
  sigemptyset(&Set);
  sigaddset(&Set, SIGRTMIN);
  sigaddset(&Set, SIGRTMIN + 1);
  CHECK(sigprocmask(SIG_BLOCK, &Set, &Old) == 0);
  CHECK(sigqueue(getpid(), SIGRTMIN + 1, (union sigval) {.sival_ptr = (void *)7}) == 0);
  CHECK(sigqueue(getpid(), SIGRTMIN, (union sigval) {.sival_ptr = (void *)5}) == 0);
  CHECK(sigqueue(getpid(), SIGRTMIN + 1, (union sigval) {.sival_ptr = (void *)8}) == 0);
  CHECK(sigprocmask(SIG_SETMASK, &Old, NULL) == 0);
  CHECK(SeenCount == 3);
  // Each handler blocks only its own signal. Linux sets up the SIGRTMIN frame
  // first and stacks the first SIGRTMIN+1 frame on top, which runs first;
  // its sigreturn unblocks SIGRTMIN+1 for the second entry. SIGRTMIN's
  // handler runs last.
  CHECK(SeenSignals[0] == SIGRTMIN + 1 && SeenValues[0] == 7);
  CHECK(SeenSignals[1] == SIGRTMIN + 1 && SeenValues[1] == 8);
  CHECK(SeenSignals[2] == SIGRTMIN && SeenValues[2] == 5);
  CHECK(LastInfo.si_code == SI_QUEUE && LastInfo.si_pid == getpid());
}

TEST(signal, rt_order_with_blocking_handlers) {
  ResetRecorder();
  sigset_t Set, Old;
  sigemptyset(&Set);
  sigaddset(&Set, SIGRTMIN);
  sigaddset(&Set, SIGRTMIN + 1);
  struct sigaction Action;
  memset(&Action, 0, sizeof(Action));
  Action.sa_sigaction = RecordInfo;
  Action.sa_flags = SA_SIGINFO;
  Action.sa_mask = Set;
  CHECK(sigaction(SIGRTMIN, &Action, NULL) == 0);
  CHECK(sigaction(SIGRTMIN + 1, &Action, NULL) == 0);
  CHECK(sigprocmask(SIG_BLOCK, &Set, &Old) == 0);
  CHECK(sigqueue(getpid(), SIGRTMIN + 1, (union sigval) {.sival_ptr = (void *)7}) == 0);
  CHECK(sigqueue(getpid(), SIGRTMIN, (union sigval) {.sival_ptr = (void *)5}) == 0);
  CHECK(sigqueue(getpid(), SIGRTMIN + 1, (union sigval) {.sival_ptr = (void *)8}) == 0);
  CHECK(sigprocmask(SIG_SETMASK, &Old, NULL) == 0);
  CHECK(SeenCount == 3);
  CHECK(SeenSignals[0] == SIGRTMIN && SeenValues[0] == 5);
  CHECK(SeenSignals[1] == SIGRTMIN + 1 && SeenValues[1] == 7);
  CHECK(SeenSignals[2] == SIGRTMIN + 1 && SeenValues[2] == 8);
}

TEST(signal, standard_signal_collapses) {
  ResetRecorder();
  CHECK(Catch(SIGUSR1, 0) == 0);
  CHECK(Block(SIGUSR1) == 0);
  // Process-directed twice: one shared-queue entry.
  CHECK(kill(getpid(), SIGUSR1) == 0);
  CHECK(kill(getpid(), SIGUSR1) == 0);
  CHECK(Unblock(SIGUSR1) == 0);
  CHECK(HandlerCalls == 1);
}

TEST(signal, sigqueueinfo_eperm_positive_code) {
  siginfo_t Info;
  memset(&Info, 0, sizeof(Info));
  Info.si_signo = SIGUSR1;
  Info.si_code = SI_USER;
  CHECK_ERRNO(syscall(SYS_rt_sigqueueinfo, getppid(), SIGUSR1, &Info), EPERM);
  Info.si_code = SI_TKILL;
  CHECK_ERRNO(syscall(SYS_rt_sigqueueinfo, getppid(), SIGUSR1, &Info), EPERM);
  // A process may claim any origin when it signals itself.
  ResetRecorder();
  CHECK(Catch(SIGUSR1, SA_SIGINFO) == 0);
  CHECK(Block(SIGUSR1) == 0);
  Info.si_code = SI_USER;
  Info.si_pid = 4242;
  CHECK(syscall(SYS_rt_sigqueueinfo, getpid(), SIGUSR1, &Info) == 0);
  CHECK(Unblock(SIGUSR1) == 0);
  CHECK(HandlerCalls == 1);
  CHECK(LastInfo.si_signo == SIGUSR1 && LastInfo.si_code == SI_USER && LastInfo.si_pid == 4242);
}

static volatile pid_t ReceiverTid;
static volatile int ReceiverStop;

static void *ReceiveUsr1(void *Argument) {
  (void)Argument;
  CHECK(Unblock(SIGUSR1) == 0);
  ReceiverTid = gettid();
  while (!HandlerCalls && !ReceiverStop) SleepMs(1);
  return NULL;
}

TEST(signal, tgkill_targets_thread) {
  ResetRecorder();
  ReceiverTid = 0;
  ReceiverStop = 0;
  CHECK(Catch(SIGUSR1, 0) == 0);
  CHECK(Block(SIGUSR1) == 0);
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, ReceiveUsr1, NULL) == 0);
  uint64_t Limit = Deadline(2000);
  while (!ReceiverTid && !DeadlineExpired(Limit)) SleepMs(1);
  CHECK(ReceiverTid != 0);
  CHECK(syscall(SYS_tgkill, getpid(), ReceiverTid, SIGUSR1) == 0);
  Limit = Deadline(2000);
  while (!HandlerCalls && !DeadlineExpired(Limit)) SleepMs(1);
  ReceiverStop = 1;
  CHECK(pthread_join(Thread, NULL) == 0);
  CHECK(HandlerCalls == 1);
  CHECK(HandlerTid == ReceiverTid);
  sigset_t Pending;
  CHECK(sigpending(&Pending) == 0);
  CHECK(!sigismember(&Pending, SIGUSR1));
}

TEST(signal, process_signal_reaches_unblocked_thread) {
  ResetRecorder();
  ReceiverTid = 0;
  ReceiverStop = 0;
  CHECK(Catch(SIGUSR1, 0) == 0);
  CHECK(Block(SIGUSR1) == 0);
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, ReceiveUsr1, NULL) == 0);
  uint64_t Limit = Deadline(2000);
  while (!ReceiverTid && !DeadlineExpired(Limit)) SleepMs(1);
  CHECK(kill(getpid(), SIGUSR1) == 0);
  Limit = Deadline(2000);
  while (!HandlerCalls && !DeadlineExpired(Limit)) SleepMs(1);
  ReceiverStop = 1;
  CHECK(pthread_join(Thread, NULL) == 0);
  CHECK(HandlerCalls == 1);
  CHECK(HandlerTid == ReceiverTid);
}

TEST(signal, tgkill_errors) {
  CHECK_ERRNO(syscall(SYS_tgkill, 0, 1, 0), EINVAL);
  CHECK_ERRNO(syscall(SYS_tgkill, getpid(), 0, 0), EINVAL);
  CHECK_ERRNO(syscall(SYS_tgkill, getpid(), 999999, 0), ESRCH);
  CHECK_ERRNO(syscall(SYS_tgkill, getpid(), getpid(), 65), EINVAL);
  CHECK_ERRNO(syscall(SYS_tkill, 0, 0), EINVAL);
  CHECK(syscall(SYS_tkill, gettid(), 0) == 0);
}

TEST(signal, sigaction_kill_stop_rules) {
  struct sigaction Action;
  memset(&Action, 0, sizeof(Action));
  Action.sa_handler = SIG_DFL;
  CHECK_ERRNO(sigaction(SIGKILL, &Action, NULL), EINVAL);
  CHECK_ERRNO(sigaction(SIGSTOP, &Action, NULL), EINVAL);
  Action.sa_handler = CountSignal;
  CHECK_ERRNO(sigaction(SIGKILL, &Action, NULL), EINVAL);
  CHECK(sigaction(SIGKILL, NULL, &Action) == 0);
  CHECK(Action.sa_handler == SIG_DFL);
  CHECK_ERRNO(sigaction(0, NULL, &Action), EINVAL);
  CHECK_ERRNO(sigaction(65, NULL, &Action), EINVAL);
  struct { void *Handler; unsigned long Flags; void *Restorer; uint64_t Mask; } Raw = {0};
  CHECK_ERRNO(syscall(SYS_rt_sigaction, SIGUSR1, NULL, &Raw, 4), EINVAL);
}

TEST(signal, ignore_discards_pending) {
  ResetRecorder();
  CHECK(Block(SIGUSR1) == 0);
  CHECK(raise(SIGUSR1) == 0);
  sigset_t Pending;
  CHECK(sigpending(&Pending) == 0 && sigismember(&Pending, SIGUSR1));
  CHECK(Dispose(SIGUSR1, SIG_IGN) == 0);
  CHECK(sigpending(&Pending) == 0 && !sigismember(&Pending, SIGUSR1));
  CHECK(Catch(SIGUSR1, 0) == 0);
  CHECK(Unblock(SIGUSR1) == 0);
  CHECK(HandlerCalls == 0);
}

TEST(signal, blocked_ignored_signal_stays_pending) {
  // An ignored signal that is blocked is still queued (Linux sig_ignored).
  ResetRecorder();
  CHECK(Dispose(SIGUSR1, SIG_IGN) == 0);
  CHECK(Block(SIGUSR1) == 0);
  CHECK(raise(SIGUSR1) == 0);
  CHECK(Catch(SIGUSR1, 0) == 0);
  CHECK(Unblock(SIGUSR1) == 0);
  CHECK(HandlerCalls == 1);
}

TEST(signal, sigpending_reports_blocked) {
  CHECK(Catch(SIGUSR2, 0) == 0);
  CHECK(Block(SIGUSR2) == 0);
  sigset_t Pending;
  CHECK(sigpending(&Pending) == 0 && !sigismember(&Pending, SIGUSR2));
  CHECK(raise(SIGUSR2) == 0);
  CHECK(sigpending(&Pending) == 0 && sigismember(&Pending, SIGUSR2));
  uint64_t Raw = 0;
  CHECK(syscall(SYS_rt_sigpending, &Raw, 4) == 0);
  CHECK(Raw == (1u << (SIGUSR2 - 1)));
  CHECK_ERRNO(syscall(SYS_rt_sigpending, &Raw, 16), EINVAL);
}

static volatile int Usr2BlockedInHandler;

static void CheckMaskInHandler(int Signal) {
  CountSignal(Signal);
  sigset_t Now;
  sigprocmask(SIG_BLOCK, NULL, &Now);
  Usr2BlockedInHandler = sigismember(&Now, SIGUSR2) && sigismember(&Now, SIGUSR1);
}

TEST(signal, sigsuspend_restores_mask) {
  ResetRecorder();
  Usr2BlockedInHandler = 0;
  CHECK(Dispose(SIGUSR1, CheckMaskInHandler) == 0);
  sigset_t Blocked, Suspend, After;
  sigemptyset(&Blocked);
  sigaddset(&Blocked, SIGUSR1);
  sigaddset(&Blocked, SIGINT);
  CHECK(sigprocmask(SIG_SETMASK, &Blocked, NULL) == 0);
  CHECK(raise(SIGUSR1) == 0);
  sigemptyset(&Suspend);
  sigaddset(&Suspend, SIGUSR2);
  CHECK_ERRNO(sigsuspend(&Suspend), EINTR);
  CHECK(HandlerCalls == 1);
  CHECK(Usr2BlockedInHandler);
  CHECK(sigprocmask(SIG_BLOCK, NULL, &After) == 0);
  CHECK(sigismember(&After, SIGUSR1) && sigismember(&After, SIGINT) && !sigismember(&After, SIGUSR2));
}

// Set by the main thread right before it blocks. Without /proc task state
// the helper cannot see the block itself; waiting for the flag and then
// 50 ms narrows the race to the instructions before the syscall.
static volatile int AboutToBlock;

static void WaitUntilAboutToBlock(void) {
  const uint64_t Limit = Deadline(5000);
  while (!AboutToBlock && !DeadlineExpired(Limit)) SleepMs(1);
  SleepMs(50);
}

static void *SignalProcessLater(void *Argument) {
  WaitUntilAboutToBlock();
  kill(getpid(), (int)(intptr_t)Argument);
  return NULL;
}

TEST(signal, pause_returns_eintr) {
  ResetRecorder();
  CHECK(Catch(SIGUSR1, SA_RESTART) == 0);
  pthread_t Thread;
  sigset_t Set;
  sigemptyset(&Set);
  sigaddset(&Set, SIGUSR1);
  // The helper thread blocks SIGUSR1 so the main thread takes it.
  CHECK(pthread_sigmask(SIG_BLOCK, &Set, NULL) == 0);
  CHECK(pthread_create(&Thread, NULL, SignalProcessLater, (void *)(intptr_t)SIGUSR1) == 0);
  AboutToBlock = 0;
  CHECK(pthread_sigmask(SIG_UNBLOCK, &Set, NULL) == 0);
  AboutToBlock = 1;
  CHECK_ERRNO(pause(), EINTR);
  CHECK(HandlerCalls == 1);
  CHECK(pthread_join(Thread, NULL) == 0);
}

TEST(signal, sigtimedwait_dequeues) {
  CHECK(Block(SIGUSR1) == 0);
  CHECK(raise(SIGUSR1) == 0);
  sigset_t Set;
  sigemptyset(&Set);
  sigaddset(&Set, SIGUSR1);
  siginfo_t Info;
  memset(&Info, 0, sizeof(Info));
  const struct timespec Timeout = {1, 0};
  CHECK(sigtimedwait(&Set, &Info, &Timeout) == SIGUSR1);
  // raise()'s si_code depends on the libc; the sender is portable.
  CHECK(Info.si_signo == SIGUSR1 && Info.si_pid == getpid());
  sigset_t Pending;
  CHECK(sigpending(&Pending) == 0 && !sigismember(&Pending, SIGUSR1));
}

TEST(signal, sigtimedwait_takes_handled_signal) {
  // A signal in the set is returned, not delivered to its handler.
  ResetRecorder();
  CHECK(Catch(SIGUSR1, 0) == 0);
  CHECK(Block(SIGUSR1) == 0);
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, SignalProcessLater, (void *)(intptr_t)SIGUSR1) == 0);
  sigset_t Set;
  sigemptyset(&Set);
  sigaddset(&Set, SIGUSR1);
  const struct timespec Timeout = {5, 0};
  AboutToBlock = 1;
  CHECK(sigtimedwait(&Set, NULL, &Timeout) == SIGUSR1);
  CHECK(HandlerCalls == 0);
  CHECK(pthread_join(Thread, NULL) == 0);
}

TEST(signal, sigtimedwait_timeout_eagain) {
  sigset_t Set;
  sigemptyset(&Set);
  sigaddset(&Set, SIGUSR1);
  CHECK(Block(SIGUSR1) == 0);
  const struct timespec Timeout = {0, 20 * 1000 * 1000};
  CHECK_ERRNO(sigtimedwait(&Set, NULL, &Timeout), EAGAIN);
  const struct timespec Zero = {0, 0};
  CHECK_ERRNO(sigtimedwait(&Set, NULL, &Zero), EAGAIN);
}

TEST(signal, sigtimedwait_bad_timespec_einval) {
  sigset_t Set;
  sigemptyset(&Set);
  sigaddset(&Set, SIGUSR1);
  const struct timespec Bad = {0, 1000000000L};
  CHECK_ERRNO(sigtimedwait(&Set, NULL, &Bad), EINVAL);
  const struct timespec Negative = {-1, 0};
  CHECK_ERRNO(sigtimedwait(&Set, NULL, &Negative), EINVAL);
}

static int RestartPipe[2];
static pthread_t MainThread;

static void *InterruptThenWrite(void *Argument) {
  (void)Argument;
  WaitUntilAboutToBlock();
  pthread_kill(MainThread, SIGUSR1);
  SleepMs(50);
  const char Byte = 'x';
  if (write(RestartPipe[1], &Byte, 1) != 1) return (void *)1;
  return NULL;
}

static void ReadAcrossSignal(int Flags, long Expected, int ExpectedErrno) {
  ResetRecorder();
  CHECK(pipe(RestartPipe) == 0);
  CHECK(Catch(SIGUSR1, Flags) == 0);
  AboutToBlock = 0;
  MainThread = pthread_self();
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, InterruptThenWrite, NULL) == 0);
  char Byte = 0;
  errno = 0;
  AboutToBlock = 1;
  const long Result = read(RestartPipe[0], &Byte, 1);
  const int Error = errno;
  CHECK(pthread_join(Thread, NULL) == 0);
  CHECK(HandlerCalls == 1);
  CHECK(Result == Expected);
  if (Expected < 0) CHECK(Error == ExpectedErrno);
  else CHECK(Byte == 'x');
}

TEST(signal, restart_read_with_sa_restart) {
  ReadAcrossSignal(SA_RESTART, 1, 0);
}

TEST(signal, no_restart_without_sa_restart) {
  ReadAcrossSignal(0, -1, EINTR);
}

static volatile uintptr_t HandlerStackAddress;

static void RecordStack(int Signal) {
  volatile char Local = 0;
  (void)Local;
  CountSignal(Signal);
  HandlerStackAddress = (uintptr_t)&Local;
}

TEST(signal, sigaltstack_handler_runs_on_alt_stack) {
  ResetRecorder();
  static char AltStack[64 * 1024];
  stack_t Stack = {.ss_sp = AltStack, .ss_size = sizeof(AltStack), .ss_flags = 0};
  CHECK(sigaltstack(&Stack, NULL) == 0);
  struct sigaction Action;
  memset(&Action, 0, sizeof(Action));
  Action.sa_handler = RecordStack;
  Action.sa_flags = SA_ONSTACK;
  CHECK(sigaction(SIGUSR1, &Action, NULL) == 0);
  CHECK(raise(SIGUSR1) == 0);
  CHECK(HandlerCalls == 1);
  CHECK(HandlerStackAddress >= (uintptr_t)AltStack && HandlerStackAddress < (uintptr_t)AltStack + sizeof(AltStack));
}

static void WriteToClosedPipe(void) {
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  close(Pipe[0]);
  const char Byte = 0;
  (void)!write(Pipe[1], &Byte, 1);
}

TEST(signal, sigpipe_default_kills_writer) {
  const int Status = WaitExit(Fork(WriteToClosedPipe));
  CHECK(WIFSIGNALED(Status) && WTERMSIG(Status) == SIGPIPE);
}

TEST(signal, sigpipe_handler_siginfo) {
  ResetRecorder();
  CHECK(Catch(SIGPIPE, SA_SIGINFO) == 0);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  close(Pipe[0]);
  const char Byte = 0;
  CHECK_ERRNO(write(Pipe[1], &Byte, 1), EPIPE);
  CHECK(HandlerCalls == 1);
  CHECK(LastInfo.si_code == SI_USER && LastInfo.si_pid == getpid() && LastInfo.si_uid == getuid());
}

TEST(signal, sigpipe_ignored_epipe) {
  CHECK(Dispose(SIGPIPE, SIG_IGN) == 0);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  close(Pipe[0]);
  const char Byte = 0;
  CHECK_ERRNO(write(Pipe[1], &Byte, 1), EPIPE);
}

static void RaiseTermBlocked(void) {
  CHECK(Block(SIGTERM) == 0);
  CHECK(raise(SIGTERM) == 0);
  // A blocked fatal signal stays pending; unblocking kills the process.
  CHECK(Unblock(SIGTERM) == 0);
  _exit(3);
}

TEST(signal, blocked_fatal_signal_kills_on_unblock) {
  const int Status = WaitExit(Fork(RaiseTermBlocked));
  CHECK(WIFSIGNALED(Status) && WTERMSIG(Status) == SIGTERM);
}

static void *SpinInThread(void *Argument) {
  volatile unsigned long *Counter = Argument;
  for (;;) (*Counter)++;
  return NULL;
}

static void KillSelfWithThreads(void) {
  static volatile unsigned long Counter;
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, SpinInThread, (void *)&Counter) == 0);
  SleepMs(20);
  kill(getpid(), SIGTERM);
  for (;;) pause();
}

TEST(signal, fatal_signal_ends_all_threads) {
  const int Status = WaitExit(Fork(KillSelfWithThreads));
  CHECK(WIFSIGNALED(Status) && WTERMSIG(Status) == SIGTERM);
}

// ---------------------------------------------------------------------------
// Preemption: CPU-bound guest code must not starve other threads or processes.

static volatile unsigned long Spins;

static void SpinForever(void) {
  for (;;) Spins++;
}

TEST(proc, busy_child_is_killable) {
  pid_t Child = Fork(SpinForever);
  CHECK(Child > 0);
  struct timespec Pause = {0, 50 * 1000 * 1000};
  nanosleep(&Pause, NULL);
  CHECK(kill(Child, SIGKILL) == 0);
  int Status = WaitExit(Child);
  CHECK(WIFSIGNALED(Status) && WTERMSIG(Status) == SIGKILL);
}

static volatile int StopSpinning;
static volatile unsigned long ThreadSpins[2];

static void *SpinThread(void *Argument) {
  volatile unsigned long *Counter = Argument;
  while (!StopSpinning) (*Counter)++;
  return NULL;
}

TEST(proc, busy_threads_share_cpu) {
  pthread_t Threads[2];
  StopSpinning = 0;
  ThreadSpins[0] = ThreadSpins[1] = 0;
  for (int I = 0; I < 2; ++I) CHECK(pthread_create(&Threads[I], NULL, SpinThread, (void *)&ThreadSpins[I]) == 0);
  // The main thread also computes instead of sleeping, so without
  // preemption neither spinning thread would ever run.
  uint64_t Limit = Deadline(200);
  while (!DeadlineExpired(Limit)) Spins++;
  StopSpinning = 1;
  for (int I = 0; I < 2; ++I) CHECK(pthread_join(Threads[I], NULL) == 0);
  CHECK(ThreadSpins[0] > 0);
  CHECK(ThreadSpins[1] > 0);
}

// Preempted code must resume at the exact guest instruction. These loops
// keep their state in registers only, so a resume at the wrong RIP changes
// the result. A second busy thread forces preemptions while they run.
static void *SpinUntilStopped(void *Argument) {
  (void)Argument;
  while (!StopSpinning) Spins++;
  return NULL;
}

static unsigned long RegisterCountdown(unsigned long Count) {
  unsigned long Sum = 0;
  __asm__ volatile("1:\n\t"
                   "add $3, %0\n\t"
                   "dec %1\n\t"
                   "jnz 1b"
                   : "+r"(Sum), "+r"(Count));
  return Sum;
}

__attribute__((noinline)) static unsigned long AddOne(unsigned long Value) {
  __asm__ volatile("" : "+r"(Value));
  return Value + 1;
}

static unsigned long CallLoop(unsigned long Count) {
  unsigned long Sum = 0;
  for (unsigned long I = 0; I < Count; ++I) Sum = AddOne(Sum);
  return Sum;
}

TEST(proc, preempted_register_loop_is_exact) {
  pthread_t Thread;
  StopSpinning = 0;
  CHECK(pthread_create(&Thread, NULL, SpinUntilStopped, NULL) == 0);
  const unsigned long Count = 100UL * 1000 * 1000;
  const unsigned long Sum = RegisterCountdown(Count);
  StopSpinning = 1;
  CHECK(pthread_join(Thread, NULL) == 0);
  CHECK(Sum == 3 * Count);
}

TEST(proc, preempted_call_loop_is_exact) {
  pthread_t Thread;
  StopSpinning = 0;
  CHECK(pthread_create(&Thread, NULL, SpinUntilStopped, NULL) == 0);
  const unsigned long Count = 3UL * 1000 * 1000;
  const unsigned long Sum = CallLoop(Count);
  StopSpinning = 1;
  CHECK(pthread_join(Thread, NULL) == 0);
  CHECK(Sum == Count);
}

static volatile sig_atomic_t Alarmed;

static void OnAlarm(int Signal) {
  (void)Signal;
  Alarmed = 1;
}

TEST(signal, alarm_interrupts_busy_loop) {
  Alarmed = 0;
  CHECK(signal(SIGALRM, OnAlarm) != SIG_ERR);
  alarm(1);
  while (!Alarmed) Spins++;
  CHECK(Alarmed);
}

// ---------------------------------------------------------------------------
// Process lifecycle and wait.

static void ExitSeven(void) { _exit(7); }
static void SpinBriefly(void) {
  const uint64_t Limit = Deadline(100);
  while (!DeadlineExpired(Limit)) Spins++;
}

TEST(proc, fork_wait_exit_code) {
  const int Status = WaitExit(Fork(ExitSeven));
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 7);
}

TEST(proc, wait_echild_without_children) {
  int Status;
  CHECK_ERRNO(waitpid(-1, &Status, 0), ECHILD);
  CHECK_ERRNO(waitpid(-1, &Status, WNOHANG), ECHILD);
  CHECK_ERRNO(waitpid(getpid(), &Status, 0), ECHILD);
  CHECK_ERRNO(waitpid(-1, &Status, 0x10), EINVAL);
}

TEST(proc, waitpid_specific_and_any) {
  const pid_t First = Fork(ExitSeven), Second = Fork(ExitAtOnce);
  int Status;
  CHECK(waitpid(Second, &Status, 0) == Second && WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  CHECK(waitpid(-1, &Status, 0) == First && WEXITSTATUS(Status) == 7);
  CHECK_ERRNO(waitpid(-1, &Status, 0), ECHILD);
}

static void WaitForPipeEof(void) {
  char Byte;
  close(ReadyPipe[1]);
  while (read(ReadyPipe[0], &Byte, 1) > 0) {}
}

TEST(proc, wait_pgid_selector) {
  CHECK(pipe(ReadyPipe) == 0);
  const pid_t Other = Fork(WaitForPipeEof);
  CHECK(setpgid(Other, Other) == 0);
  const pid_t Same = Fork(ExitSeven);
  int Status;
  CHECK(waitpid(0, &Status, 0) == Same);
  CHECK(waitpid(-Other, &Status, WNOHANG) == 0);
  close(ReadyPipe[1]);
  CHECK(waitpid(-Other, &Status, 0) == Other && WIFEXITED(Status));
}

TEST(proc, waitid_wexited_siginfo) {
  const pid_t Child = Fork(ExitSeven);
  siginfo_t Info;
  memset(&Info, 0xff, sizeof(Info));
  CHECK(waitid(P_PID, Child, &Info, WEXITED) == 0);
  CHECK(Info.si_signo == SIGCHLD && Info.si_code == CLD_EXITED && Info.si_pid == Child);
  CHECK(Info.si_status == 7 && Info.si_uid == getuid());
  const pid_t Killed = Fork(RaiseTerm);
  CHECK(waitid(P_ALL, 0, &Info, WEXITED) == 0);
  CHECK(Info.si_pid == Killed && Info.si_code == CLD_KILLED && Info.si_status == SIGTERM);
}

TEST(proc, waitid_wnowait_keeps_zombie) {
  const pid_t Child = Fork(ExitSeven);
  siginfo_t Info;
  CHECK(waitid(P_PID, Child, &Info, WEXITED | WNOWAIT) == 0 && Info.si_pid == Child);
  CHECK(waitid(P_PID, Child, &Info, WEXITED | WNOWAIT) == 0 && Info.si_pid == Child);
  int Status;
  CHECK(waitpid(Child, &Status, 0) == Child && WEXITSTATUS(Status) == 7);
}

TEST(proc, waitid_wnohang_zeroes_info) {
  CHECK(pipe(ReadyPipe) == 0);
  const pid_t Child = Fork(WaitForPipeEof);
  siginfo_t Info;
  memset(&Info, 0xff, sizeof(Info));
  CHECK(waitid(P_PID, Child, &Info, WEXITED | WNOHANG) == 0);
  CHECK(Info.si_pid == 0 && Info.si_signo == 0);
  close(ReadyPipe[1]);
  CHECK(waitid(P_PID, Child, &Info, WEXITED) == 0 && Info.si_pid == Child);
}

TEST(proc, waitid_requires_state_flag_einval) {
  siginfo_t Info;
  CHECK_ERRNO(waitid(P_ALL, 0, &Info, WNOHANG), EINVAL);
  CHECK_ERRNO(waitid(7, 0, &Info, WEXITED), EINVAL);
  CHECK_ERRNO(waitid(P_ALL, 0, &Info, WEXITED), ECHILD);
}

TEST(proc, wait4_rusage_utime_advances) {
  const pid_t Child = Fork(SpinBriefly);
  struct rusage Usage;
  memset(&Usage, 0, sizeof(Usage));
  int Status;
  CHECK(wait4(Child, &Status, 0, &Usage) == Child);
  struct rusage Children;
  CHECK(getrusage(RUSAGE_CHILDREN, &Children) == 0);
  CHECK(Children.ru_utime.tv_sec * 1000000 + Children.ru_utime.tv_usec + Children.ru_stime.tv_sec * 1000000 +
        Children.ru_stime.tv_usec >= 50000);
  CHECK_ERRNO(getrusage(5, &Children), EINVAL);
}

TEST(proc, sigchld_siginfo_on_exit) {
  ResetRecorder();
  CHECK(Catch(SIGCHLD, SA_SIGINFO) == 0);
  CHECK(Block(SIGCHLD) == 0);
  const pid_t Child = Fork(ExitSeven);
  int Status;
  CHECK(waitpid(Child, &Status, 0) == Child);
  SuspendUntilHandled();
  CHECK(LastInfo.si_signo == SIGCHLD && LastInfo.si_code == CLD_EXITED);
  CHECK(LastInfo.si_pid == Child && LastInfo.si_status == 7);
}

TEST(proc, sigchld_ignored_autoreaps) {
  CHECK(Dispose(SIGCHLD, SIG_IGN) == 0);
  const pid_t Child = Fork(ExitAtOnce);
  int Status;
  // The child is reaped by the kernel; wait blocks until it is gone.
  CHECK_ERRNO(waitpid(Child, &Status, 0), ECHILD);
}

TEST(proc, sa_nocldwait_autoreaps) {
  ResetRecorder();
  CHECK(Catch(SIGCHLD, SA_NOCLDWAIT) == 0);
  CHECK(Block(SIGCHLD) == 0);
  const pid_t Child = Fork(ExitAtOnce);
  int Status;
  CHECK_ERRNO(waitpid(Child, &Status, 0), ECHILD);
  // SA_NOCLDWAIT still sends SIGCHLD.
  SuspendUntilHandled();
  CHECK(LastSignal == SIGCHLD);
}

static void ReportParentAfterOrphan(void) {
  const pid_t Grandchild = fork();
  if (Grandchild == 0) {
    const pid_t Original = getppid();
    const uint64_t Limit = Deadline(5000);
    while (getppid() == Original && !DeadlineExpired(Limit)) SleepMs(1);
    const pid_t Now = getppid();
    _exit(write(ReadyPipe[1], &Now, sizeof(Now)) == sizeof(Now) ? 0 : 1);
  }
  _exit(Grandchild > 0 ? 0 : 1);
}

TEST(proc, orphan_reparented_ppid_1) {
  CHECK(pipe(ReadyPipe) == 0);
  const int Status = WaitExit(Fork(ReportParentAfterOrphan));
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  pid_t Reported = 0;
  CHECK(read(ReadyPipe[0], &Reported, sizeof(Reported)) == sizeof(Reported));
  // Without a subreaper the orphan's parent is init (or a session subreaper
  // such as systemd --user natively).
  CHECK(Reported != getpid());
}

TEST(proc, orphan_reparented_to_subreaper) {
  CHECK(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
  int Value = 0;
  CHECK(prctl(PR_GET_CHILD_SUBREAPER, &Value) == 0 && Value == 1);
  CHECK(pipe(ReadyPipe) == 0);
  const pid_t Child = Fork(ReportParentAfterOrphan);
  int Status;
  CHECK(waitpid(Child, &Status, 0) == Child && WEXITSTATUS(Status) == 0);
  pid_t Reported = 0;
  CHECK(read(ReadyPipe[0], &Reported, sizeof(Reported)) == sizeof(Reported));
  CHECK(Reported == getpid());
  // The grandchild is now our child.
  CHECK(waitpid(-1, &Status, 0) > 0 && WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
}

static void *ExitGroupFromThread(void *Argument) {
  (void)Argument;
  SleepMs(20);
  syscall(SYS_exit_group, 5);
  return NULL;
}

static void ThreadCallsExitGroup(void) {
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, ExitGroupFromThread, NULL) == 0);
  for (;;) pause();
}

TEST(proc, exit_group_kills_threads) {
  const int Status = WaitExit(Fork(ThreadCallsExitGroup));
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 5);
}

static void *LastThreadExits(void *Argument) {
  (void)Argument;
  SleepMs(20);
  syscall(SYS_exit, 9);
  return NULL;
}

static void MainThreadExitsFirst(void) {
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, LastThreadExits, NULL) == 0);
  syscall(SYS_exit, 3);
}

TEST(proc, last_thread_exit_status) {
  // The process ends with the last thread; its exit code is the status.
  const int Status = WaitExit(Fork(MainThreadExitsFirst));
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 9);
}

static volatile int VforkShared;

TEST(proc, vfork_parent_waits_for_exec) {
  const uint64_t Start = NowMs();
  const pid_t Child = vfork();
  if (Child == 0) {
    SleepMs(100);
    execl("/bin/true", "true", (char *)NULL);
    _exit(127);
  }
  CHECK(Child > 0);
  CHECK(NowMs() - Start >= 90);
  int Status;
  CHECK(waitpid(Child, &Status, 0) == Child && WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
}

TEST(proc, vfork_parent_waits_for_exit) {
  const uint64_t Start = NowMs();
  const pid_t Child = vfork();
  if (Child == 0) {
    SleepMs(100);
    _exit(4);
  }
  CHECK(NowMs() - Start >= 90);
  int Status;
  CHECK(waitpid(Child, &Status, 0) == Child && WEXITSTATUS(Status) == 4);
}

struct CloneArgs {
  uint64_t Flags, Pidfd, ChildTid, ParentTid, ExitSignal, Stack, StackSize, Tls, SetTid, SetTidSize, Cgroup;
};

static long Clone3(struct CloneArgs *Args, size_t Size) { return syscall(SYS_clone3, Args, Size); }

TEST(proc, clone3_basic_and_size_errors) {
  struct CloneArgs Args;
  memset(&Args, 0, sizeof(Args));
  Args.ExitSignal = SIGCHLD;
  CHECK_ERRNO(Clone3(&Args, 32), EINVAL);
  char Big[128];
  memset(Big, 0, sizeof(Big));
  memcpy(Big, &Args, sizeof(Args));
  Big[100] = 1;
  CHECK_ERRNO(Clone3((struct CloneArgs *)Big, sizeof(Big)), E2BIG);
  Args.ExitSignal = 65;
  CHECK_ERRNO(Clone3(&Args, sizeof(Args)), EINVAL);
  Args.ExitSignal = SIGCHLD;
  Args.Flags = SIGCHLD;
  CHECK_ERRNO(Clone3(&Args, sizeof(Args)), EINVAL);
  Args.Flags = 0;
  const long Child = Clone3(&Args, 64);
  if (Child == 0) _exit(6);
  CHECK(Child > 0);
  int Status;
  CHECK(waitpid(Child, &Status, 0) == Child && WEXITSTATUS(Status) == 6);
}

TEST(proc, clone_pidfd_poll_exit) {
  struct CloneArgs Args;
  memset(&Args, 0, sizeof(Args));
  int Pidfd = -1;
  Args.Flags = CLONE_PIDFD;
  Args.Pidfd = (uint64_t)(uintptr_t)&Pidfd;
  Args.ExitSignal = SIGCHLD;
  const long Child = Clone3(&Args, sizeof(Args));
  if (Child == 0) _exit(2);
  CHECK(Child > 0 && Pidfd >= 0);
  CHECK(fcntl(Pidfd, F_GETFD) == FD_CLOEXEC);
  // Readable once the child exited (asyncio's PidfdChildWatcher relies on it).
  struct pollfd Entry = {.fd = Pidfd, .events = POLLIN};
  CHECK(poll(&Entry, 1, 5000) == 1 && (Entry.revents & POLLIN));
  siginfo_t Info;
  CHECK(waitid(P_PIDFD, Pidfd, &Info, WEXITED) == 0);
  CHECK(Info.si_pid == Child && Info.si_status == 2);
  close(Pidfd);
}

TEST(proc, pidfd_open_errors) {
  CHECK_ERRNO(syscall(SYS_pidfd_open, getpid(), 1), EINVAL);
  CHECK_ERRNO(syscall(SYS_pidfd_open, 0, 0), EINVAL);
  CHECK_ERRNO(syscall(SYS_pidfd_open, INT_MAX, 0), ESRCH);
  const int Fd = syscall(SYS_pidfd_open, getpid(), 0);
  CHECK(Fd >= 0);
  CHECK(fcntl(Fd, F_GETFD) == FD_CLOEXEC);
  close(Fd);
}

TEST(proc, pidfd_send_signal_kills) {
  const pid_t Child = Fork(AwaitUsr1Forever);
  const int Fd = syscall(SYS_pidfd_open, Child, 0);
  CHECK(Fd >= 0);
  CHECK_ERRNO(syscall(SYS_pidfd_send_signal, Fd, SIGKILL, NULL, 8), EINVAL);
  CHECK_ERRNO(syscall(SYS_pidfd_send_signal, Fd, SIGKILL, NULL, 3), EINVAL);
  CHECK(syscall(SYS_pidfd_send_signal, Fd, SIGKILL, NULL, 0) == 0);
  const int Status = WaitExit(Child);
  CHECK(WIFSIGNALED(Status) && WTERMSIG(Status) == SIGKILL);
  CHECK_ERRNO(syscall(SYS_pidfd_send_signal, Fd, 0, NULL, 0), ESRCH);
  close(Fd);
}

TEST(proc, pidfd_not_readable_while_running) {
  CHECK(pipe(ReadyPipe) == 0);
  const pid_t Child = Fork(WaitForPipeEof);
  const int Fd = syscall(SYS_pidfd_open, Child, 0);
  CHECK(Fd >= 0);
  struct pollfd Entry = {.fd = Fd, .events = POLLIN};
  CHECK(poll(&Entry, 1, 0) == 0);
  close(ReadyPipe[1]);
  CHECK(poll(&Entry, 1, 5000) == 1 && (Entry.revents & POLLIN));
  WaitExit(Child);
  close(Fd);
}

TEST(proc, wait4_rusage_reports_child_time) {
  const pid_t Child = Fork(SpinBriefly);
  struct rusage Usage;
  memset(&Usage, 0, sizeof(Usage));
  int Status;
  CHECK(wait4(Child, &Status, 0, &Usage) == Child);
  CHECK(Usage.ru_utime.tv_sec * 1000000 + Usage.ru_utime.tv_usec + Usage.ru_stime.tv_sec * 1000000 +
        Usage.ru_stime.tv_usec >= 50000);
}

static void HoldPipeOpen(void) {
  char Byte;
  close(ReadyPipe[1]);
  while (read(ReadyPipe[0], &Byte, 1) > 0) {}
}

TEST(proc, pidfd_getfd_dup) {
  CHECK(pipe(ReadyPipe) == 0);
  const pid_t Child = Fork(HoldPipeOpen);
  const int Fd = syscall(SYS_pidfd_open, Child, 0);
  CHECK(Fd >= 0);
  const int Copy = syscall(SYS_pidfd_getfd, Fd, ReadyPipe[0], 0);
  CHECK(Copy >= 0);
  CHECK(fcntl(Copy, F_GETFD) == FD_CLOEXEC);
  CHECK_ERRNO(syscall(SYS_pidfd_getfd, Fd, 900, 0), EBADF);
  CHECK_ERRNO(syscall(SYS_pidfd_getfd, Fd, ReadyPipe[0], 1), EINVAL);
  close(Copy);
  close(ReadyPipe[1]);
  WaitExit(Child);
  close(Fd);
}

TEST(proc, execveat_empty_path) {
  const pid_t Child = fork();
  if (Child == 0) {
    const int Fd = open("/bin/true", O_RDONLY);
    char *Argv[] = {"true", NULL}, *Envp[] = {NULL};
    syscall(SYS_execveat, Fd, "", Argv, Envp, AT_EMPTY_PATH);
    _exit(127);
  }
  int Status;
  CHECK(waitpid(Child, &Status, 0) == Child && WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  char *Argv[] = {"true", NULL}, *Envp[] = {NULL};
  CHECK_ERRNO(syscall(SYS_execveat, AT_FDCWD, "/bin/true", Argv, Envp, 0x10), EINVAL);
}

static int WriteScript(const char *Path, const char *Text) {
  const int Fd = open(Path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
  if (Fd < 0) return -1;
  const ssize_t Size = (ssize_t)strlen(Text);
  const int Ok = write(Fd, Text, Size) == Size;
  return close(Fd) == 0 && Ok ? 0 : -1;
}

// Runs Path with Argv in a child; returns its exit status and up to Size-1
// bytes of its standard output in Out, or the child's exec errno as
// 1000 + errno.
static int RunScript(const char *Path, char *const Argv[], char *Out, size_t Size) {
  int Pipe[2];
  if (pipe(Pipe) < 0) return -1;
  fflush(stdout);
  const pid_t Child = fork();
  if (Child == 0) {
    dup2(Pipe[1], 1);
    char *Envp[] = {"PATH=/usr/bin:/bin", NULL};
    execve(Path, Argv, Envp);
    _exit(errno < 200 ? 50 + errno : 49);
  }
  close(Pipe[1]);
  size_t Used = 0;
  ssize_t Count;
  while (Used + 1 < Size && (Count = read(Pipe[0], Out + Used, Size - 1 - Used)) > 0) Used += Count;
  Out[Used] = 0;
  close(Pipe[0]);
  const int Status = WaitExit(Child);
  if (!WIFEXITED(Status)) return -1;
  return WEXITSTATUS(Status) >= 50 ? 1000 + WEXITSTATUS(Status) - 50 : WEXITSTATUS(Status);
}

TEST(proc, exec_interpreter_script) {
  char Dir[] = "./proc-exec.XXXXXX";
  if (!mkdtemp(Dir)) SKIP("no writable scratch directory");
  char Outer[64], Inner[64], Bad[64], Loop[64], Command[200], Out[256];
  snprintf(Outer, sizeof(Outer), "%s/outer", Dir);
  snprintf(Inner, sizeof(Inner), "%s/inner", Dir);
  snprintf(Bad, sizeof(Bad), "%s/bad", Dir);
  snprintf(Loop, sizeof(Loop), "%s/loop", Dir);
  // outer runs inner with one argument "-x y" (inner blanks kept, trailing
  // blanks dropped); inner is itself a script run by /bin/sh.
  snprintf(Command, sizeof(Command), "#! \t%s  -x y \t\nexit 9\n", Inner);
  CHECK(WriteScript(Outer, Command) == 0);
  CHECK(WriteScript(Inner, "#!/bin/sh\nprintf '%s|' \"$@\"\nexit 7\n") == 0);
  char *Argv[] = {"name", "p", "q", NULL};
  CHECK(RunScript(Outer, Argv, Out, sizeof(Out)) == 7);
  snprintf(Command, sizeof(Command), "-x y|%s|p|q|", Outer);
  CHECK(strcmp(Out, Command) == 0);
  if (strcmp(Out, Command)) printf("# got '%s', want '%s'\n", Out, Command);

  CHECK(WriteScript(Bad, "#!   \nexit 0\n") == 0);
  CHECK(RunScript(Bad, Argv, Out, sizeof(Out)) == 1000 + ENOEXEC);
  // Each rewrite names the script again; Linux stops after four.
  snprintf(Command, sizeof(Command), "#!%s\n", Loop);
  CHECK(WriteScript(Loop, Command) == 0);
  CHECK(RunScript(Loop, Argv, Out, sizeof(Out)) == 1000 + ELOOP);
  // A script reached through a close-on-exec descriptor has no name the
  // interpreter could open.
  const int Fd = open(Inner, O_RDONLY | O_CLOEXEC);
  char *Envp[] = {NULL};
  CHECK_ERRNO(syscall(SYS_execveat, Fd, "", Argv, Envp, AT_EMPTY_PATH), ENOENT);
  close(Fd);
  unlink(Outer); unlink(Inner); unlink(Bad); unlink(Loop);
  CHECK(rmdir(Dir) == 0);
}

static void ExecCheckHandlers(void) {
  execl("/bin/sh", "sh", "-c", "kill -USR1 $$; kill -USR2 $$; exit 3", (char *)NULL);
  _exit(127);
}

TEST(proc, exec_resets_handlers_keeps_ignored) {
  CHECK(Catch(SIGUSR1, 0) == 0);
  CHECK(Dispose(SIGUSR2, SIG_IGN) == 0);
  // After exec SIGUSR1 is default (kills sh), SIGUSR2 stays ignored.
  const int Status = WaitExit(Fork(ExecCheckHandlers));
  CHECK(WIFSIGNALED(Status) && WTERMSIG(Status) == SIGUSR1);
}

static void ExecWithPendingSignal(void) {
  CHECK(Block(SIGUSR1) == 0);
  CHECK(raise(SIGUSR1) == 0);
  // sh unblocks nothing; grep SigBlk/SigPnd needs /proc, so test through
  // the pending signal killing sh when it unblocks via its own trap handling.
  execl("/bin/sh", "sh", "-c", "exit 0", (char *)NULL);
  _exit(127);
}

TEST(proc, exec_keeps_pending_and_mask) {
  // The pending blocked SIGUSR1 survives exec and the mask stays blocked,
  // so sh exits normally.
  const int Status = WaitExit(Fork(ExecWithPendingSignal));
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
}

static void ParentDiesWithChild(void) {
  const pid_t Grandchild = fork();
  if (Grandchild == 0) {
    ResetRecorder();
    CHECK(Catch(SIGUSR2, 0) == 0);
    CHECK(prctl(PR_SET_PDEATHSIG, SIGUSR2) == 0);
    int Value = 0;
    CHECK(prctl(PR_GET_PDEATHSIG, &Value) == 0 && Value == SIGUSR2);
    SignalReady();
    const uint64_t Limit = Deadline(5000);
    while (!HandlerCalls && !DeadlineExpired(Limit)) SleepMs(1);
    const char Byte = HandlerCalls ? 'y' : 'n';
    _exit(write(RestartPipe[1], &Byte, 1) == 1 ? 0 : 1);
  }
  WaitReady(1);
  _exit(0);
}

TEST(proc, pdeathsig_on_parent_exit) {
  CHECK(pipe(ReadyPipe) == 0);
  CHECK(pipe(RestartPipe) == 0);
  WaitExit(Fork(ParentDiesWithChild));
  char Byte = 0;
  CHECK(read(RestartPipe[0], &Byte, 1) == 1);
  CHECK(Byte == 'y');
}

TEST(proc, prctl_name_roundtrip) {
  char Name[16] = {0};
  CHECK(prctl(PR_SET_NAME, "a-very-long-thread-name") == 0);
  CHECK(prctl(PR_GET_NAME, Name) == 0);
  CHECK(strcmp(Name, "a-very-long-thr") == 0);
  CHECK_ERRNO(prctl(9999, 0, 0, 0, 0), EINVAL);
  CHECK(prctl(PR_SET_DUMPABLE, 0) == 0 && prctl(PR_GET_DUMPABLE) == 0);
  CHECK_ERRNO(prctl(PR_SET_DUMPABLE, 5), EINVAL);
}

TEST(proc, priority_rules) {
  errno = 0;
  const int Start = getpriority(PRIO_PROCESS, 0);
  CHECK(errno == 0);
  CHECK(setpriority(PRIO_PROCESS, 0, Start + 1) == 0);
  CHECK(getpriority(PRIO_PROCESS, 0) == Start + 1);
  // Lowering nice needs CAP_SYS_NICE or RLIMIT_NICE (20 - nice limit).
  struct rlimit Nice;
  CHECK(getrlimit(RLIMIT_NICE, &Nice) == 0);
  const int Floor = Nice.rlim_cur == RLIM_INFINITY ? -20 : 20 - (int)Nice.rlim_cur;
  if (geteuid() != 0 && Start < Floor) CHECK_ERRNO(setpriority(PRIO_PROCESS, 0, Start), EACCES);
  else CHECK(setpriority(PRIO_PROCESS, 0, Start) == 0);
  CHECK_ERRNO(setpriority(PRIO_PROCESS, INT_MAX, 0), ESRCH);
  CHECK_ERRNO(getpriority(7, 0), EINVAL);
  CHECK(syscall(SYS_getpriority, PRIO_PGRP, 0) >= 1);
}

TEST(proc, times_ticks_advance) {
  struct tms Before, After;
  const clock_t First = times(&Before);
  SpinBriefly();
  const clock_t Second = times(&After);
  CHECK(Second > First);
  CHECK(After.tms_utime + After.tms_stime >= Before.tms_utime + Before.tms_stime + 5);
  CHECK(times(NULL) != (clock_t)-1);
}

TEST(proc, getcpu_zero) {
  unsigned Cpu = 99, Node = 99;
  CHECK(syscall(SYS_getcpu, &Cpu, &Node, NULL) == 0);
  CHECK(Cpu < 4096 && Node < 64);
  CHECK(syscall(SYS_getcpu, NULL, NULL, NULL) == 0);
}

TEST(proc, sysinfo_counts_processes) {
  struct sysinfo Info;
  CHECK(sysinfo(&Info) == 0);
  CHECK(Info.procs >= 2);
}

TEST(proc, personality_query_and_set) {
  const int Old = personality(0xffffffff);
  CHECK(Old >= 0);
  CHECK(personality(Old | ADDR_NO_RANDOMIZE) == Old);
  CHECK(personality(0xffffffff) == (Old | ADDR_NO_RANDOMIZE));
}

// ---------------------------------------------------------------------------
// Sessions and job control.

TEST(session, setsid_new_session) {
  const pid_t Child = fork();
  if (Child == 0) {
    CaseFailures = 0;
    // The test child leads its own group, so a grandchild is needed.
    const pid_t Grandchild = fork();
    if (Grandchild == 0) {
      CHECK(setsid() == getpid());
      CHECK(getsid(0) == getpid() && getpgrp() == getpid());
      CHECK(open("/dev/tty", O_RDWR) < 0);
      _exit(CaseFailures ? 1 : 0);
    }
    const int Status = WaitExit(Grandchild);
    _exit(WIFEXITED(Status) ? WEXITSTATUS(Status) : 1);
  }
  const int Status = WaitExit(Child);
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
}

TEST(session, setsid_eperm_group_leader) {
  CHECK(getpgrp() == getpid());
  CHECK_ERRNO(setsid(), EPERM);
}

static void WaitForever(void) {
  for (;;) pause();
}

TEST(session, setpgid_rules) {
  CHECK_ERRNO(setpgid(0, -1), EINVAL);
  CHECK_ERRNO(setpgid(getppid(), 0), ESRCH);
  CHECK_ERRNO(setpgid(INT_MAX, 0), ESRCH);
  const pid_t Child = Fork(WaitForever);
  CHECK(setpgid(Child, Child) == 0 && getpgid(Child) == Child);
  CHECK(setpgid(Child, getpgrp()) == 0 && getpgid(Child) == getpgrp());
  CHECK_ERRNO(setpgid(Child, 999999), EPERM);
  CHECK(kill(Child, SIGKILL) == 0);
  WaitExit(Child);
}

TEST(session, setpgid_after_exec_eacces) {
  const pid_t Child = fork();
  if (Child == 0) {
    execl("/bin/sleep", "sleep", "5", (char *)NULL);
    _exit(127);
  }
  SleepMs(300);
  CHECK_ERRNO(setpgid(Child, Child), EACCES);
  kill(Child, SIGKILL);
  WaitExit(Child);
}

TEST(session, getsid_esrch) {
  CHECK(getsid(0) == getsid(getpid()));
  CHECK_ERRNO(getsid(INT_MAX), ESRCH);
  CHECK_ERRNO(getpgid(INT_MAX), ESRCH);
}

static void StopSelf(void) {
  raise(SIGSTOP);
  _exit(8);
}

TEST(session, stop_and_continue_reported) {
  const pid_t Child = Fork(StopSelf);
  int Status;
  CHECK(waitpid(Child, &Status, WUNTRACED) == Child);
  CHECK(WIFSTOPPED(Status) && WSTOPSIG(Status) == SIGSTOP);
  CHECK(waitpid(Child, &Status, WUNTRACED | WNOHANG) == 0);
  siginfo_t Info;
  CHECK(waitid(P_PID, Child, &Info, WSTOPPED | WNOHANG) == 0 && Info.si_pid == 0);
  CHECK(kill(Child, SIGCONT) == 0);
  CHECK(waitpid(Child, &Status, WCONTINUED) == Child);
  CHECK(WIFCONTINUED(Status));
  CHECK(waitpid(Child, &Status, 0) == Child && WIFEXITED(Status) && WEXITSTATUS(Status) == 8);
}

TEST(session, stopped_child_killable) {
  const pid_t Child = Fork(StopSelf);
  int Status;
  CHECK(waitpid(Child, &Status, WUNTRACED) == Child && WIFSTOPPED(Status));
  CHECK(kill(Child, SIGTERM) == 0);
  CHECK(waitpid(Child, &Status, WNOHANG) == 0);
  CHECK(kill(Child, SIGKILL) == 0);
  CHECK(waitpid(Child, &Status, 0) == Child && WIFSIGNALED(Status) && WTERMSIG(Status) == SIGKILL);
}

static void StopAndReportUsr1(void) {
  ResetRecorder();
  CHECK(Catch(SIGCONT, 0) == 0);
  raise(SIGSTOP);
  _exit(HandlerCalls == 1 && LastSignal == SIGCONT ? 0 : 1);
}

TEST(session, sigcont_handler_runs_after_resume) {
  const pid_t Child = Fork(StopAndReportUsr1);
  int Status;
  CHECK(waitpid(Child, &Status, WUNTRACED) == Child && WIFSTOPPED(Status));
  CHECK(kill(Child, SIGCONT) == 0);
  CHECK(waitpid(Child, &Status, 0) == Child && WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
}

static void BlockStopThenCont(void) {
  CHECK(Block(SIGTSTP) == 0);
  CHECK(raise(SIGTSTP) == 0);
  CHECK(kill(getpid(), SIGCONT) == 0);
  sigset_t Pending;
  CHECK(sigpending(&Pending) == 0);
  _exit(sigismember(&Pending, SIGTSTP) ? 1 : 0);
}

TEST(session, sigcont_discards_pending_stop) {
  const int Status = WaitExit(Fork(BlockStopThenCont));
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
}

TEST(session, sa_nocldstop_suppresses) {
  ResetRecorder();
  CHECK(Catch(SIGCHLD, SA_NOCLDSTOP) == 0);
  const pid_t Child = Fork(StopSelf);
  int Status;
  CHECK(waitpid(Child, &Status, WUNTRACED) == Child && WIFSTOPPED(Status));
  CHECK(HandlerCalls == 0);
  CHECK(kill(Child, SIGCONT) == 0);
  CHECK(waitpid(Child, &Status, 0) == Child);
  SuspendUntilHandled();
  CHECK(HandlerCalls == 1);
}

static volatile int HupThenCont;

// Both handlers run; SIGCONT's frame is stacked last, so it runs first.
static void RecordHupCont(int Signal) { HupThenCont |= Signal == SIGHUP ? 1 : Signal == SIGCONT ? 2 : 4; }

static void OrphanStoppedGrandchild(void) {
  const pid_t Grandchild = fork();
  if (Grandchild == 0) {
    CHECK(setpgid(0, 0) == 0);
    HupThenCont = 0;
    CHECK(Dispose(SIGHUP, RecordHupCont) == 0);
    CHECK(Dispose(SIGCONT, RecordHupCont) == 0);
    SignalReady();
    raise(SIGSTOP);
    const char Byte = HupThenCont == 3 ? 'y' : 'n';
    _exit(write(RestartPipe[1], &Byte, 1) == 1 ? 0 : 1);
  }
  setpgid(Grandchild, Grandchild);
  WaitReady(1);
  int Status;
  // Exit once the grandchild is stopped; its group becomes orphaned.
  waitpid(Grandchild, &Status, WUNTRACED);
  _exit(0);
}

TEST(session, orphaned_stopped_group_gets_hup) {
  CHECK(pipe(ReadyPipe) == 0);
  CHECK(pipe(RestartPipe) == 0);
  WaitExit(Fork(OrphanStoppedGrandchild));
  char Byte = 0;
  CHECK(read(RestartPipe[0], &Byte, 1) == 1);
  CHECK(Byte == 'y');
}

static void TstpInOrphanedGroup(void) {
  const pid_t Grandchild = fork();
  if (Grandchild == 0) {
    CHECK(setpgid(0, 0) == 0);
    WaitReady(1);
    // The group is orphaned now: SIGTSTP with the default action is discarded.
    raise(SIGTSTP);
    const char Byte = 'y';
    _exit(write(RestartPipe[1], &Byte, 1) == 1 ? 0 : 1);
  }
  _exit(0);
}

TEST(session, tstp_to_orphan_discarded) {
  CHECK(pipe(ReadyPipe) == 0);
  CHECK(pipe(RestartPipe) == 0);
  WaitExit(Fork(TstpInOrphanedGroup));
  const char Go = 1;
  CHECK(write(ReadyPipe[1], &Go, 1) == 1);
  char Byte = 0;
  CHECK(read(RestartPipe[0], &Byte, 1) == 1 && Byte == 'y');
}

TEST(session, terminal_job_control_ioctls) {
  if (!isatty(0)) SKIP("stdin is not a terminal");
  // The test runs in a background group; blocked SIGTTOU permits tcsetpgrp.
  CHECK(Block(SIGTTOU) == 0);
  CHECK(tcgetpgrp(0) > 0);
  CHECK(tcgetsid(0) > 0);
  CHECK_ERRNO(tcsetpgrp(0, 999999), ESRCH);
}

// ---------------------------------------------------------------------------
// Task 4 review additions.

TEST(signal, unblocked_ignored_signal_dropped) {
  ResetRecorder();
  CHECK(Block(SIGWINCH) == 0);
  CHECK(raise(SIGWINCH) == 0);
  CHECK(Unblock(SIGWINCH) == 0); // default action ignore: discarded on delivery
  CHECK(Block(SIGWINCH) == 0);
  sigset_t Pending;
  CHECK(sigpending(&Pending) == 0);
  CHECK(!sigismember(&Pending, SIGWINCH));
  CHECK(Catch(SIGWINCH, 0) == 0);
  CHECK(Unblock(SIGWINCH) == 0);
  CHECK(HandlerCalls == 0);
}

static volatile int FutexWord;

static void *WakeFutexLater(void *Argument) {
  (void)Argument;
  WaitUntilAboutToBlock();
  pthread_kill(MainThread, SIGUSR1);
  SleepMs(50);
  FutexWord = 1;
  syscall(SYS_futex, &FutexWord, FUTEX_WAKE, 1, NULL, NULL, 0);
  return NULL;
}

static long FutexAcrossSignal(int Flags, const struct timespec *Timeout) {
  ResetRecorder();
  FutexWord = 0;
  AboutToBlock = 0;
  CHECK(Catch(SIGUSR1, Flags) == 0);
  MainThread = pthread_self();
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, WakeFutexLater, NULL) == 0);
  AboutToBlock = 1;
  errno = 0;
  long Result = syscall(SYS_futex, &FutexWord, FUTEX_WAIT, 0, Timeout, NULL, 0);
  if (Result < 0) Result = -errno;
  CHECK(pthread_join(Thread, NULL) == 0);
  CHECK(HandlerCalls == 1);
  return Result;
}

TEST(signal, futex_wait_restarts_with_sa_restart) {
  CHECK(FutexAcrossSignal(SA_RESTART, NULL) == 0);
}

TEST(signal, futex_wait_eintr_without_sa_restart) {
  CHECK(FutexAcrossSignal(0, NULL) == -EINTR);
}

TEST(signal, timed_futex_wait_eintr_despite_sa_restart) {
  const struct timespec Timeout = {5, 0};
  CHECK(FutexAcrossSignal(SA_RESTART, &Timeout) == -EINTR);
}

// Interruption and restart table (ProcessInventory.md). Each case blocks
// the main thread in one syscall; a helper signals it once blocked, waits
// 50 ms and then releases the block. With SA_RESTART, -ERESTARTSYS calls
// complete after the release; sleeps, polls and signal waits return EINTR.
struct RestartCase {
  void (*Setup)(void);
  long (*Block)(void);    // the syscall; returns the result or -errno
  void (*Release)(void);
  void (*Cleanup)(void);
};
static const struct RestartCase *ActiveRestart;
static int RestartFds[4];
static pid_t RestartChild;

static void *InterruptThenRelease(void *Argument) {
  (void)Argument;
  WaitUntilAboutToBlock();
  pthread_kill(MainThread, SIGUSR1);
  SleepMs(50);
  if (ActiveRestart->Release) ActiveRestart->Release();
  return NULL;
}

static long RestartAcrossSignal(const struct RestartCase *Case, int Flags) {
  ResetRecorder();
  ActiveRestart = Case;
  for (int Index = 0; Index < 4; ++Index) RestartFds[Index] = -1;
  RestartChild = 0;
  if (Case->Setup) Case->Setup();
  CHECK(Catch(SIGUSR1, Flags) == 0);
  AboutToBlock = 0;
  MainThread = pthread_self();
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, InterruptThenRelease, NULL) == 0);
  AboutToBlock = 1;
  const long Result = Case->Block();
  CHECK(pthread_join(Thread, NULL) == 0);
  CHECK(HandlerCalls == 1);
  if (Case->Cleanup) Case->Cleanup();
  for (int Index = 0; Index < 4; ++Index) {
    if (RestartFds[Index] >= 0) close(RestartFds[Index]);
  }
  return Result;
}

static long Errno(long Result) { return Result < 0 ? -errno : Result; }

// write: a full pipe; the release drains one page.
static void FullPipeSetup(void) {
  CHECK(pipe(RestartFds) == 0);
  CHECK(fcntl(RestartFds[1], F_SETFL, O_NONBLOCK) == 0);
  char Chunk[4096] = {0};
  while (write(RestartFds[1], Chunk, sizeof(Chunk)) > 0) {}
  CHECK(fcntl(RestartFds[1], F_SETFL, 0) == 0);
}
static long FullPipeWrite(void) { const char Byte = 'w'; return Errno(write(RestartFds[1], &Byte, 1)); }
static void FullPipeDrain(void) { char Chunk[4096]; if (read(RestartFds[0], Chunk, sizeof(Chunk)) <= 0) {} }

// wait4: the child exits when the release writes to its pipe.
static void ChildWaitsSetup(void) {
  CHECK(pipe(RestartFds) == 0);
  fflush(stdout);
  RestartChild = fork();
  if (RestartChild == 0) {
    char Byte;
    _exit(read(RestartFds[0], &Byte, 1) == 1 ? 5 : 6);
  }
}
static long WaitChild(void) {
  int Status = 0;
  const long Result = Errno(wait4(RestartChild, &Status, 0, NULL));
  return Result == RestartChild ? (WIFEXITED(Status) ? WEXITSTATUS(Status) : -1000) : Result;
}
static void ReleaseChild(void) { if (write(RestartFds[1], "x", 1) != 1) {} }
static void ReapChild(void) { if (RestartChild > 0) waitpid(RestartChild, NULL, 0); }

// flock: another open file description holds the lock.
static char LockPath[64];
static void FlockSetup(void) {
  snprintf(LockPath, sizeof(LockPath), "./proc-lock.%d", (int)getpid());
  CHECK((RestartFds[0] = open(LockPath, O_RDWR | O_CREAT | O_CLOEXEC, 0600)) >= 0);
  CHECK((RestartFds[1] = open(LockPath, O_RDWR | O_CLOEXEC)) >= 0);
  CHECK(flock(RestartFds[0], LOCK_EX) == 0);
}
static long FlockBlock(void) { return Errno(flock(RestartFds[1], LOCK_EX)); }
static void FlockRelease(void) { flock(RestartFds[0], LOCK_UN); }
static void LockCleanup(void) { unlink(LockPath); ReapChild(); }

// F_SETLKW: record locks conflict only between processes, so a child holds
// the lock until the release tells it to exit.
static void RecordLockSetup(void) {
  snprintf(LockPath, sizeof(LockPath), "./proc-lock.%d", (int)getpid());
  CHECK((RestartFds[0] = open(LockPath, O_RDWR | O_CREAT | O_CLOEXEC, 0600)) >= 0);
  CHECK(pipe(RestartFds + 1) == 0);
  int Ready[2];
  CHECK(pipe(Ready) == 0);
  fflush(stdout);
  RestartChild = fork();
  if (RestartChild == 0) {
    struct flock Lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    char Byte;
    if (fcntl(RestartFds[0], F_SETLK, &Lock) != 0 || write(Ready[1], "r", 1) != 1) _exit(1);
    _exit(read(RestartFds[1], &Byte, 1) == 1 ? 0 : 1);
  }
  char Byte;
  CHECK(read(Ready[0], &Byte, 1) == 1);
  close(Ready[0]);
  close(Ready[1]);
}
static long RecordLockBlock(void) {
  struct flock Lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
  return Errno(fcntl(RestartFds[0], F_SETLKW, &Lock));
}
static void RecordLockRelease(void) { if (write(RestartFds[2], "x", 1) != 1) {} }

// accept and connect on an abstract Unix stream socket.
static struct sockaddr_un RestartAddress;
static socklen_t RestartAddressLength;
static void ListenerSetup(void) {
  memset(&RestartAddress, 0, sizeof(RestartAddress));
  RestartAddress.sun_family = AF_UNIX;
  const int Length = snprintf(RestartAddress.sun_path + 1, sizeof(RestartAddress.sun_path) - 1, "proc-restart-%d", (int)getpid());
  RestartAddressLength = offsetof(struct sockaddr_un, sun_path) + 1 + Length;
  CHECK((RestartFds[0] = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) >= 0);
  CHECK(bind(RestartFds[0], (struct sockaddr *)&RestartAddress, RestartAddressLength) == 0);
  CHECK(listen(RestartFds[0], 0) == 0);
}
static long AcceptBlock(void) {
  const long Result = Errno(accept4(RestartFds[0], NULL, NULL, SOCK_CLOEXEC));
  if (Result >= 0) close((int)Result);
  return Result >= 0 ? 0 : Result;
}
static void ConnectOnce(void) {
  RestartFds[1] = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (connect(RestartFds[1], (struct sockaddr *)&RestartAddress, RestartAddressLength) != 0) {}
}
// With backlog 0 Linux queues one connection; the next connect blocks.
static void FullBacklogSetup(void) {
  ListenerSetup();
  CHECK((RestartFds[1] = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) >= 0);
  CHECK(connect(RestartFds[1], (struct sockaddr *)&RestartAddress, RestartAddressLength) == 0);
  CHECK((RestartFds[2] = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) >= 0);
}
static long ConnectBlock(void) {
  return Errno(connect(RestartFds[2], (struct sockaddr *)&RestartAddress, RestartAddressLength));
}
static void AcceptQueued(void) {
  const int Connection = accept4(RestartFds[0], NULL, NULL, SOCK_CLOEXEC);
  if (Connection >= 0) RestartFds[3] = Connection;
}

// Calls that never restart; their release writes to a pipe that is watched.
static void PipeSetup(void) { CHECK(pipe(RestartFds) == 0); }
static void PipeRelease(void) { if (write(RestartFds[1], "x", 1) != 1) {} }
static long ReadBlock(void) { char Byte; return Errno(read(RestartFds[0], &Byte, 1)); }
static long EpollBlock(void) {
  const int Epoll = epoll_create1(EPOLL_CLOEXEC);
  struct epoll_event Event = {.events = EPOLLIN};
  if (Epoll < 0 || epoll_ctl(Epoll, EPOLL_CTL_ADD, RestartFds[0], &Event) != 0) return -1000;
  RestartFds[2] = Epoll;
  return Errno(epoll_wait(Epoll, &Event, 1, 5000));
}
static long PollBlock(void) {
  struct pollfd Entry = {.fd = RestartFds[0], .events = POLLIN};
  return Errno(poll(&Entry, 1, 5000));
}
static long SelectBlock(void) {
  fd_set Read;
  FD_ZERO(&Read);
  FD_SET(RestartFds[0], &Read);
  struct timeval Timeout = {5, 0};
  return Errno(select(RestartFds[0] + 1, &Read, NULL, NULL, &Timeout));
}
static long SleepBlock(void) {
  const struct timespec Duration = {5, 0};
  return Errno(nanosleep(&Duration, NULL));
}
static long SuspendBlock(void) {
  sigset_t Mask;
  sigemptyset(&Mask);
  return Errno(sigsuspend(&Mask));
}

// A blocking pipe write queues every byte before it returns; a signal
// after some bytes ends it with their count, even with SA_RESTART.
static char LargeWrite[256 * 1024];
static long LargePipeWrite(void) { return Errno(write(RestartFds[1], LargeWrite, sizeof(LargeWrite))); }

static void *DrainPipe(void *Argument) {
  (void)Argument;
  static char Chunk[8192];
  long Total = 0, Count;
  while ((Count = read(RestartFds[0], Chunk, sizeof(Chunk))) > 0) Total += Count;
  return (void *)Total;
}

TEST(proc, blocking_pipe_write_is_complete) {
  CHECK(pipe(RestartFds) == 0);
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, DrainPipe, NULL) == 0);
  CHECK(write(RestartFds[1], LargeWrite, sizeof(LargeWrite)) == (ssize_t)sizeof(LargeWrite));
  close(RestartFds[1]);
  void *Total;
  CHECK(pthread_join(Thread, &Total) == 0);
  CHECK((long)Total == (long)sizeof(LargeWrite));
  close(RestartFds[0]);
}

TEST(signal, interrupted_pipe_write_returns_count) {
  static const struct RestartCase Case = {PipeSetup, LargePipeWrite, NULL, NULL};
  const long Capacity = 65536; // F_GETPIPE_SZ default
  CHECK(RestartAcrossSignal(&Case, SA_RESTART) == Capacity);
  CHECK(RestartAcrossSignal(&Case, 0) == Capacity);
}

// unix_recvq_full: a listener queues backlog + 1 connections.
TEST(proc, unix_listen_queues_backlog_plus_one) {
  ListenerSetup();
  CHECK(listen(RestartFds[0], 2) == 0);
  int Clients[4];
  for (int Index = 0; Index < 4; ++Index) {
    Clients[Index] = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    const int Result = connect(Clients[Index], (struct sockaddr *)&RestartAddress, RestartAddressLength);
    const int Error = errno;
    if (Index < 3) CHECK(Result == 0);
    else CHECK(Result == -1 && Error == EAGAIN);
  }
  for (int Index = 0; Index < 4; ++Index) close(Clients[Index]);
  close(RestartFds[0]);
}

#define RESTART_CASE(Name, Setup, Block, Release, Cleanup, Restarted, Interrupted) \
  TEST(signal, restart_table_##Name) {                                            \
    static const struct RestartCase Case = {Setup, Block, Release, Cleanup};      \
    CHECK(RestartAcrossSignal(&Case, SA_RESTART) == (Restarted));                 \
    CHECK(RestartAcrossSignal(&Case, 0) == (Interrupted));                        \
  }

RESTART_CASE(read_pipe, PipeSetup, ReadBlock, PipeRelease, NULL, 1, -EINTR)
RESTART_CASE(write_full_pipe, FullPipeSetup, FullPipeWrite, FullPipeDrain, NULL, 1, -EINTR)
RESTART_CASE(wait4, ChildWaitsSetup, WaitChild, ReleaseChild, ReapChild, 5, -EINTR)
RESTART_CASE(flock, FlockSetup, FlockBlock, FlockRelease, LockCleanup, 0, -EINTR)
RESTART_CASE(fcntl_setlkw, RecordLockSetup, RecordLockBlock, RecordLockRelease, LockCleanup, 0, -EINTR)
RESTART_CASE(accept, ListenerSetup, AcceptBlock, ConnectOnce, NULL, 0, -EINTR)
RESTART_CASE(connect_unix, FullBacklogSetup, ConnectBlock, AcceptQueued, NULL, 0, -EINTR)
TEST(signal, restart_table_epoll_wait) {
  const int Probe = epoll_create1(EPOLL_CLOEXEC);
  if (Probe < 0 && errno == ENOSYS) SKIP("epoll is not routed yet (Milestone 2 Task 13)");
  if (Probe >= 0) close(Probe);
  static const struct RestartCase Case = {PipeSetup, EpollBlock, PipeRelease, NULL};
  CHECK(RestartAcrossSignal(&Case, SA_RESTART) == -EINTR);
  CHECK(RestartAcrossSignal(&Case, 0) == -EINTR);
}
RESTART_CASE(poll, PipeSetup, PollBlock, PipeRelease, NULL, -EINTR, -EINTR)
RESTART_CASE(select, PipeSetup, SelectBlock, PipeRelease, NULL, -EINTR, -EINTR)
RESTART_CASE(nanosleep, NULL, SleepBlock, NULL, NULL, -EINTR, -EINTR)
RESTART_CASE(sigsuspend, NULL, SuspendBlock, NULL, NULL, -EINTR, -EINTR)

TEST(signal, sa_resethand_keeps_flags) {
  ResetRecorder();
  CHECK(Catch(SIGUSR1, SA_RESETHAND | SA_RESTART) == 0);
  CHECK(raise(SIGUSR1) == 0);
  CHECK(HandlerCalls == 1);
  struct sigaction Old;
  CHECK(sigaction(SIGUSR1, NULL, &Old) == 0);
  CHECK(Old.sa_handler == SIG_DFL);
}

static volatile int NestedDepth, MaxNestedDepth, NestCalls;

static void NestOnce(int Signal) {
  (void)Signal;
  if (++NestedDepth > MaxNestedDepth) MaxNestedDepth = NestedDepth;
  if (++NestCalls == 1) raise(SIGUSR1);
  --NestedDepth;
}

TEST(signal, sa_nodefer_allows_nesting) {
  struct sigaction Action;
  memset(&Action, 0, sizeof(Action));
  Action.sa_handler = NestOnce;
  Action.sa_flags = SA_NODEFER;
  CHECK(sigaction(SIGUSR1, &Action, NULL) == 0);
  MaxNestedDepth = 0;
  NestCalls = 0;
  CHECK(raise(SIGUSR1) == 0);
  CHECK(MaxNestedDepth == 2);
  Action.sa_flags = 0;
  CHECK(sigaction(SIGUSR1, &Action, NULL) == 0);
  MaxNestedDepth = 0;
  NestCalls = 0;
  CHECK(raise(SIGUSR1) == 0);
  CHECK(MaxNestedDepth == 1);
}

static volatile pid_t HelperTid;

static void *PublishTidThenWait(void *Argument) {
  (void)Argument;
  sigset_t Set;
  sigemptyset(&Set);
  sigaddset(&Set, SIGUSR1);
  pthread_sigmask(SIG_BLOCK, &Set, NULL);
  HelperTid = (pid_t)syscall(SYS_gettid);
  WaitReady(1);
  return NULL;
}

TEST(signal, kill_by_thread_id) {
  // kill() with a non-leader thread ID signals the whole process.
  ResetRecorder();
  CHECK(pipe(ReadyPipe) == 0);
  CHECK(Catch(SIGUSR1, 0) == 0);
  CHECK(Block(SIGUSR1) == 0);
  HelperTid = 0;
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, PublishTidThenWait, NULL) == 0);
  const uint64_t Limit = Deadline(5000);
  while (!HelperTid && !DeadlineExpired(Limit)) SleepMs(1);
  CHECK(HelperTid != getpid());
  CHECK(kill(HelperTid, SIGUSR1) == 0);
  CHECK(Unblock(SIGUSR1) == 0);
  CHECK(HandlerCalls == 1);
  SignalReady();
  CHECK(pthread_join(Thread, NULL) == 0);
}

// ---------------------------------------------------------------------------
// cred/: credentials and capabilities.
//
// Root-only cases SKIP as an ordinary user and run in the root suite. Cases
// that need an unprivileged identity drop to one with setresuid, which also
// clears capabilities exactly as Linux does.

static int GetCaps(uint64_t *Effective, uint64_t *Permitted, uint64_t *Inheritable) {
  struct __user_cap_header_struct Header = {_LINUX_CAPABILITY_VERSION_3, 0};
  struct __user_cap_data_struct Data[2] = {{0}};
  if (syscall(SYS_capget, &Header, Data) != 0) return -1;
  if (Effective) *Effective = (uint64_t) Data[1].effective << 32 | Data[0].effective;
  if (Permitted) *Permitted = (uint64_t) Data[1].permitted << 32 | Data[0].permitted;
  if (Inheritable) *Inheritable = (uint64_t) Data[1].inheritable << 32 | Data[0].inheritable;
  return 0;
}

static long SetCaps(uint64_t Effective, uint64_t Permitted, uint64_t Inheritable) {
  struct __user_cap_header_struct Header = {_LINUX_CAPABILITY_VERSION_3, 0};
  struct __user_cap_data_struct Data[2] = {
    {(uint32_t) Effective, (uint32_t) Permitted, (uint32_t) Inheritable},
    {(uint32_t) (Effective >> 32), (uint32_t) (Permitted >> 32), (uint32_t) (Inheritable >> 32)},
  };
  return syscall(SYS_capset, &Header, Data);
}

static int Ids(uid_t *Real, uid_t *Effective, uid_t *Saved) { return getresuid(Real, Effective, Saved); }

static int IsRoot(void) { return geteuid() == 0; }

#define BIT(cap) ((uint64_t) 1 << (cap))

// Root becomes uid 1000 with saved uid 2000 (no capabilities remain).
static int BecomeUnprivileged(void) {
  if (!IsRoot()) return 0;
  return setresuid(1000, 1000, 2000);
}

TEST(cred, setuid_unprivileged_rules) {
  if (BecomeUnprivileged() != 0) SKIP("cannot drop privileges");
  const uid_t Me = getuid();
  CHECK_ERRNO(setuid(0), EPERM);
  CHECK_ERRNO(setuid(Me + 12345), EPERM);
  CHECK_ERRNO(setuid((uid_t) -1), EINVAL);
  CHECK(setuid(Me) == 0);
  {
    uid_t R, E, S;
    CHECK(Ids(&R, &E, &S) == 0);
    if (S == 2000) {
      // Saved uid 2000: setuid may switch the effective id only.
      CHECK(setuid(2000) == 0);
      CHECK(Ids(&R, &E, &S) == 0 && R == 1000 && E == 2000 && S == 2000);
      CHECK(setuid(1000) == 0);
      CHECK(Ids(&R, &E, &S) == 0 && R == 1000 && E == 1000 && S == 2000);
    }
  }
}

TEST(cred, setreuid_saved_update) {
  CHECK(BecomeUnprivileged() == 0);
  CHECK_ERRNO(setreuid(0, (uid_t) -1), EPERM);
  if (!IsRoot()) {
    CHECK(setreuid(getuid(), getuid()) == 0);
    return;
  }
  uid_t R, E, S;
  CHECK(setreuid((uid_t) -1, 2000) == 0); // effective != old real: saved follows
  CHECK(Ids(&R, &E, &S) == 0 && R == 1000 && E == 2000 && S == 2000);
  CHECK(setreuid((uid_t) -1, 1000) == 0); // effective == old real: saved stays
  CHECK(Ids(&R, &E, &S) == 0 && R == 1000 && E == 1000 && S == 2000);
  CHECK_ERRNO(setreuid(2000, (uid_t) -1), EPERM); // 2000 is only the saved id
  CHECK(setreuid(1000, (uid_t) -1) == 0);         // a real-id request resets saved
  CHECK(Ids(&R, &E, &S) == 0 && R == 1000 && E == 1000 && S == 1000);
}

TEST(cred, setresuid_unprivileged) {
  CHECK(BecomeUnprivileged() == 0);
  CHECK_ERRNO(setresuid(0, (uid_t) -1, (uid_t) -1), EPERM);
  CHECK_ERRNO(setresuid((uid_t) -1, 0, (uid_t) -1), EPERM);
  CHECK_ERRNO(setresuid((uid_t) -1, (uid_t) -1, 0), EPERM);
  if (!IsRoot()) {
    CHECK(setresuid((uid_t) -1, (uid_t) -1, (uid_t) -1) == 0);
    CHECK(setresuid(getuid(), getuid(), getuid()) == 0);
    return;
  }
  uid_t R, E, S;
  CHECK_ERRNO(setresuid(3000, (uid_t) -1, (uid_t) -1), EPERM);
  CHECK(setresuid((uid_t) -1, 2000, (uid_t) -1) == 0);
  CHECK(Ids(&R, &E, &S) == 0 && R == 1000 && E == 2000 && S == 2000);
  CHECK(setresuid((uid_t) -1, (uid_t) -1, 1000) == 0);
  CHECK(Ids(&R, &E, &S) == 0 && R == 1000 && E == 2000 && S == 1000);
  CHECK(setfsuid((uid_t) -1) == 2000); // the filesystem id followed the effective id
}

TEST(cred, setfsuid_returns_old) {
  const uid_t Me = geteuid();
  CHECK((uid_t) setfsuid((uid_t) -1) == Me);
  CHECK((uid_t) setfsuid(Me + 4242) == Me); // refused silently when unprivileged
  if (!IsRoot()) {
    CHECK((uid_t) setfsuid((uid_t) -1) == Me);
    return;
  }
  CHECK(BecomeUnprivileged() == 0);
  CHECK((uid_t) setfsuid((uid_t) -1) == 1000);
  CHECK((uid_t) setfsuid(2000) == 1000); // saved id is allowed
  CHECK((uid_t) setfsuid((uid_t) -1) == 2000);
  CHECK((uid_t) setfsuid(3000) == 2000); // not one of ours: ignored
  CHECK((uid_t) setfsuid((uid_t) -1) == 2000);
}

TEST(cred, setgid_family_mirrors_uid) {
  if (IsRoot()) {
    CHECK(setresgid(1000, 1000, 2000) == 0);
    CHECK(setresuid(1000, 1000, 1000) == 0);
  }
  const gid_t Me = getgid();
  CHECK_ERRNO(setgid(0), EPERM);
  CHECK_ERRNO(setgid((gid_t) -1), EINVAL);
  CHECK_ERRNO(setregid(0, (gid_t) -1), EPERM);
  CHECK_ERRNO(setresgid(0, (gid_t) -1, (gid_t) -1), EPERM);
  CHECK((gid_t) setfsgid(Me + 4242) == Me);
  CHECK((gid_t) setfsgid((gid_t) -1) == Me);
  CHECK(setgid(Me) == 0);
  if (!IsRoot() && getuid() != 1000) return;
  if (Me != 1000) return;
  gid_t R, E, S;
  if (getresgid(&R, &E, &S) == 0 && S == 2000) {
    CHECK(setgid(2000) == 0);
    CHECK(getresgid(&R, &E, &S) == 0 && R == 1000 && E == 2000 && S == 2000);
    CHECK((gid_t) setfsgid((gid_t) -1) == 2000);
    CHECK_ERRNO(setregid(3000, (gid_t) -1), EPERM);
  }
}

TEST(cred, setgroups_requires_cap) {
  if (IsRoot()) {
    const gid_t List[] = {3, 1, 2};
    CHECK(setgroups(3, List) == 0);
    gid_t Back[8];
    CHECK(getgroups(0, NULL) == 3);
    CHECK(getgroups(8, Back) == 3 && Back[0] == 1 && Back[1] == 2 && Back[2] == 3);
    CHECK(BecomeUnprivileged() == 0);
  }
  CHECK_ERRNO(setgroups(0, NULL), EPERM);
}

TEST(cred, getgroups_size_einval) {
  if (IsRoot()) {
    const gid_t List[] = {10, 20, 30};
    CHECK(setgroups(3, List) == 0);
  }
  gid_t Buffer[256];
  const int Count = getgroups(0, NULL);
  CHECK(Count >= 0 && Count <= 256);
  if (Count > 1) CHECK_ERRNO(getgroups(Count - 1, Buffer), EINVAL); // a size of 0 only counts
  if (Count > 0) CHECK(getgroups(Count, Buffer) == Count);
}

TEST(cred, root_drop_clears_caps) {
  if (!IsRoot()) SKIP("needs root");
  uint64_t Effective = 0, Permitted = 0;
  CHECK(GetCaps(&Effective, &Permitted, NULL) == 0);
  CHECK(Effective != 0 && Effective == Permitted);
  CHECK(Effective & BIT(CAP_SETUID));
  // Effective uid leaves root while real and saved stay: only effective drops.
  CHECK(setresuid((uid_t) -1, 1000, (uid_t) -1) == 0);
  uint64_t Reduced = 0, Kept = 0;
  CHECK(GetCaps(&Reduced, &Kept, NULL) == 0 && Reduced == 0 && Kept == Permitted);
  CHECK(setresuid((uid_t) -1, 0, (uid_t) -1) == 0);
  CHECK(GetCaps(&Reduced, &Kept, NULL) == 0 && Reduced == Permitted);
  // Every id nonzero: permitted and effective are both cleared.
  CHECK(setresuid(1000, 1000, 1000) == 0);
  CHECK(GetCaps(&Reduced, &Kept, NULL) == 0 && Reduced == 0 && Kept == 0);
}

TEST(cred, keepcaps_preserves_permitted) {
  if (!IsRoot()) SKIP("needs root");
  uint64_t Permitted = 0;
  CHECK(GetCaps(NULL, &Permitted, NULL) == 0);
  CHECK(prctl(PR_GET_KEEPCAPS) == 0);
  CHECK_ERRNO(prctl(PR_SET_KEEPCAPS, 2), EINVAL);
  CHECK(prctl(PR_SET_KEEPCAPS, 1) == 0);
  CHECK(prctl(PR_GET_KEEPCAPS) == 1);
  CHECK(setresuid(1000, 1000, 1000) == 0);
  uint64_t Effective = 0, Kept = 0;
  CHECK(GetCaps(&Effective, &Kept, NULL) == 0 && Effective == 0 && Kept == Permitted);
  // The effective set can be rebuilt from the retained permitted set.
  CHECK(SetCaps(BIT(CAP_KILL), Kept, 0) == 0);
  CHECK(GetCaps(&Effective, NULL, NULL) == 0 && Effective == BIT(CAP_KILL));
}

TEST(cred, capget_versions_and_einval) {
  struct __user_cap_data_struct Data[2];
  struct __user_cap_header_struct Header = {_LINUX_CAPABILITY_VERSION_3, 0};
  CHECK(syscall(SYS_capget, &Header, Data) == 0);
  Header = (struct __user_cap_header_struct) {_LINUX_CAPABILITY_VERSION_2, 0};
  CHECK(syscall(SYS_capget, &Header, Data) == 0 && Header.version == _LINUX_CAPABILITY_VERSION_2);
  Header = (struct __user_cap_header_struct) {_LINUX_CAPABILITY_VERSION_1, getpid()};
  CHECK(syscall(SYS_capget, &Header, Data) == 0);
  Header = (struct __user_cap_header_struct) {0x12345678, 0};
  CHECK_ERRNO(syscall(SYS_capget, &Header, Data), EINVAL);
  CHECK(Header.version == _LINUX_CAPABILITY_VERSION_3);
  Header = (struct __user_cap_header_struct) {0x12345678, 0};
  CHECK(syscall(SYS_capget, &Header, NULL) == 0); // a version probe
  CHECK(Header.version == _LINUX_CAPABILITY_VERSION_3);
  Header = (struct __user_cap_header_struct) {_LINUX_CAPABILITY_VERSION_3, -1};
  CHECK_ERRNO(syscall(SYS_capget, &Header, Data), EINVAL);
  Header = (struct __user_cap_header_struct) {_LINUX_CAPABILITY_VERSION_3, INT_MAX};
  CHECK_ERRNO(syscall(SYS_capget, &Header, Data), ESRCH);
  Header = (struct __user_cap_header_struct) {_LINUX_CAPABILITY_VERSION_3, 0};
  CHECK_ERRNO(syscall(SYS_capget, &Header, (void *) 8), EFAULT);
  if (!IsRoot()) {
    uint64_t Effective = 0, Permitted = 0;
    CHECK(GetCaps(&Effective, &Permitted, NULL) == 0 && Effective == 0 && Permitted == 0);
  }
  // Another process is readable: the parent's sets match its own view.
  Header = (struct __user_cap_header_struct) {_LINUX_CAPABILITY_VERSION_3, getppid()};
  CHECK(syscall(SYS_capget, &Header, Data) == 0);
}

TEST(cred, capset_cannot_raise) {
  uint64_t Effective = 0, Permitted = 0, Inheritable = 0;
  CHECK(GetCaps(&Effective, &Permitted, &Inheritable) == 0);
  struct __user_cap_header_struct Header = {_LINUX_CAPABILITY_VERSION_3, getpid() + 1};
  struct __user_cap_data_struct Data[2] = {{0}};
  CHECK_ERRNO(syscall(SYS_capset, &Header, Data), EPERM); // only the caller itself
  Header.pid = 0;
  Header.version = 0x12345678;
  CHECK_ERRNO(syscall(SYS_capset, &Header, Data), EINVAL);
  if (!IsRoot()) {
    CHECK_ERRNO(SetCaps(0, BIT(CAP_SYS_TIME), 0), EPERM);
    CHECK_ERRNO(SetCaps(BIT(CAP_CHOWN), 0, 0), EPERM);
    CHECK(SetCaps(0, 0, 0) == 0);
    return;
  }
  CHECK(Permitted & BIT(CAP_SYS_TIME));
  CHECK(SetCaps(Effective & ~BIT(CAP_SYS_TIME), Permitted & ~BIT(CAP_SYS_TIME), Inheritable) == 0);
  CHECK_ERRNO(SetCaps(Effective, Permitted, Inheritable), EPERM); // cannot take it back
  CHECK_ERRNO(SetCaps(BIT(CAP_CHOWN), 0, 0), EPERM);              // effective outside permitted
  CHECK(GetCaps(&Effective, &Permitted, NULL) == 0 && !(Permitted & BIT(CAP_SYS_TIME)));
}

TEST(cred, capbset_read) {
  CHECK(prctl(PR_CAPBSET_READ, CAP_CHOWN) == 1);
  CHECK_ERRNO(prctl(PR_CAPBSET_READ, 63), EINVAL);
  if (!IsRoot()) {
    CHECK_ERRNO(prctl(PR_CAPBSET_DROP, CAP_SYS_TIME), EPERM);
    CHECK(prctl(PR_CAPBSET_READ, CAP_SYS_TIME) == 1);
    return;
  }
  CHECK_ERRNO(prctl(PR_CAPBSET_DROP, 63), EINVAL);
  CHECK(prctl(PR_CAPBSET_DROP, CAP_SYS_TIME) == 0);
  CHECK(prctl(PR_CAPBSET_READ, CAP_SYS_TIME) == 0);
  CHECK(prctl(PR_CAPBSET_READ, CAP_KILL) == 1);
  CHECK(setresuid(1000, 1000, 1000) == 0);
  CHECK_ERRNO(prctl(PR_CAPBSET_DROP, CAP_KILL), EPERM); // CAP_SETPCAP is gone
}

TEST(cred, ambient_raise_requires_permitted_inheritable) {
  CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, CAP_NET_RAW, 0, 0) == 0);
  CHECK_ERRNO(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, 63, 0, 0), EINVAL);
  CHECK_ERRNO(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 1, 0, 0), EINVAL);
  CHECK_ERRNO(prctl(PR_CAP_AMBIENT, 99, CAP_NET_RAW, 0, 0), EINVAL);
  CHECK_ERRNO(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_NET_RAW, 0, 0), EPERM);
  if (!IsRoot()) return;
  uint64_t Effective = 0, Permitted = 0, Inheritable = 0;
  CHECK(GetCaps(&Effective, &Permitted, &Inheritable) == 0);
  CHECK_ERRNO(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_NET_RAW, 0, 0), EPERM); // not inheritable
  CHECK(SetCaps(Effective, Permitted, Inheritable | BIT(CAP_NET_RAW)) == 0);
  CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_NET_RAW, 0, 0) == 0);
  CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, CAP_NET_RAW, 0, 0) == 1);
  CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_LOWER, CAP_NET_RAW, 0, 0) == 0);
  CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, CAP_NET_RAW, 0, 0) == 0);
  CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_NET_RAW, 0, 0) == 0);
  CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) == 0);
  CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, CAP_NET_RAW, 0, 0) == 0);
  // Leaving root for good clears the ambient set.
  CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_NET_RAW, 0, 0) == 0);
  CHECK(setresuid(1000, 1000, 1000) == 0);
  CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, CAP_NET_RAW, 0, 0) == 0);
}

// Runs in the re-executed binary: Mode "full" expects root's exec capabilities,
// "none" expects an unprivileged process to have none.
static int CredentialExecCheck(const char *Mode) {
  uint64_t Effective = 0, Permitted = 0;
  if (GetCaps(&Effective, &Permitted, NULL) != 0) return 2;
  if (!strcmp(Mode, "full")) {
    return Effective != 0 && Effective == Permitted && (Effective & BIT(CAP_SYS_TIME)) && (Effective & BIT(CAP_SETUID)) ? 0 : 1;
  }
  return Effective == 0 && Permitted == 0 ? 0 : 1;
}

static void ExecCheckFull(void) {
  // Shrink to one capability first: exec must still hand root the full set.
  if (SetCaps(BIT(CAP_CHOWN), BIT(CAP_CHOWN), 0) != 0) _exit(3);
  execl(ExePath, ExePath, "--cred-exec-check", "full", (char *) NULL);
  _exit(4);
}

static void ExecCheckNone(void) {
  if (setresuid(1000, 1000, 1000) != 0) _exit(3);
  execl(ExePath, ExePath, "--cred-exec-check", "none", (char *) NULL);
  // The unprivileged identity may be unable to reach the binary (a user
  // namespace maps uid 1000 to an unrelated host id): not a credential bug.
  _exit(errno == EACCES ? SKIP_EXIT : 4);
}

static void ExecCheckKeepCapsNone(void) {
  // KEEPCAPS keeps permitted across setuid but not across exec.
  if (prctl(PR_SET_KEEPCAPS, 1) != 0 || setresuid(1000, 1000, 1000) != 0) _exit(3);
  execl(ExePath, ExePath, "--cred-exec-check", "none", (char *) NULL);
  // The unprivileged identity may be unable to reach the binary (a user
  // namespace maps uid 1000 to an unrelated host id): not a credential bug.
  _exit(errno == EACCES ? SKIP_EXIT : 4);
}

TEST(cred, exec_as_root_gets_full_caps) {
  if (!IsRoot()) SKIP("needs root");
  static void (*const Cases[])(void) = {ExecCheckFull, ExecCheckNone, ExecCheckKeepCapsNone};
  for (unsigned Index = 0; Index < sizeof(Cases) / sizeof(Cases[0]); ++Index) {
    const pid_t Child = Fork(Cases[Index]);
    CHECK(Child > 0);
    const int Status = WaitExit(Child);
    CHECK(WIFEXITED(Status) && (WEXITSTATUS(Status) == 0 || (Index > 0 && WEXITSTATUS(Status) == SKIP_EXIT)));
    if (WIFEXITED(Status) && WEXITSTATUS(Status) == SKIP_EXIT) printf("# exec case %u: uid 1000 cannot reach %s\n", Index, ExePath);
  }
}

static int KillReady[2];

static void AwaitAsUid3000(void) {
  if (setresuid(3000, 3000, 3000) != 0) _exit(1);
  if (write(KillReady[1], "x", 1) != 1) _exit(1);
  AwaitUsr1Forever();
}

static void KillWithoutCap(void) {
  if (setresuid(2000, 2000, 2000) != 0) _exit(1);
  pid_t Target;
  if (read(KillReady[0], &Target, sizeof(Target)) != sizeof(Target)) _exit(1);
  CHECK_ERRNO(kill(Target, 0), EPERM);
  CHECK_ERRNO(kill(Target, SIGTERM), EPERM);
  CHECK(kill(getpid(), 0) == 0);
}

static void KillWithCapKill(void) {
  // Keep CAP_KILL alone while the uid changes: signals reach any process.
  if (prctl(PR_SET_KEEPCAPS, 1) != 0 || setresuid(2000, 2000, 2000) != 0) _exit(1);
  pid_t Target;
  if (read(KillReady[0], &Target, sizeof(Target)) != sizeof(Target)) _exit(1);
  uint64_t Permitted = 0;
  CHECK(GetCaps(NULL, &Permitted, NULL) == 0 && (Permitted & BIT(CAP_KILL)));
  CHECK_ERRNO(kill(Target, 0), EPERM); // effective set is empty after the uid change
  CHECK(SetCaps(BIT(CAP_KILL), Permitted, 0) == 0);
  CHECK(kill(Target, 0) == 0);
  CHECK(kill(Target, SIGKILL) == 0);
}

TEST(cred, kill_needs_matching_uid) {
  if (!IsRoot()) SKIP("needs root");
  CHECK(pipe(KillReady) == 0);
  const pid_t Target = Fork(AwaitAsUid3000);
  CHECK(Target > 0);
  char Byte;
  CHECK(read(KillReady[0], &Byte, 1) == 1); // target now runs as 3000
  for (void (*Case)(void) = KillWithoutCap; ; Case = KillWithCapKill) {
    const pid_t Sender = Fork(Case);
    CHECK(Sender > 0);
    CHECK(write(KillReady[1], &Target, sizeof(Target)) == sizeof(Target));
    const int Status = WaitExit(Sender);
    CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
    if (Case == KillWithCapKill) break;
  }
  WaitExit(Target);
}

// ---------------------------------------------------------------------------
// clock/: clocks, resolution, CPU-time clocks, setting the clock and adjtimex.
//
// Expected values come from the native Linux kernel. The guest differs only
// where ProcessInventory.md says so: PROF/VIRT and coarse clocks tick at 4 ms
// (CONFIG_HZ=250) where a native kernel may use 1 ms, so resolutions are
// asserted against a 10 ms bound instead of one value.

// CLOCK ids as the kernel encodes them (include/linux/posix-timers.h).
#define CPU_CLOCK(pid, type) ((clockid_t) ((~(unsigned) (pid) << 3) | (type)))
#define CPUCLOCK_PERTHREAD_BIT 4
#ifndef ADJ_ADJTIME
#define ADJ_ADJTIME 0x8000
#endif
#define CLK_PROF 0
#define CLK_VIRT 1
#define CLK_SCHED 2

static int64_t ToNs(struct timespec T) { return (int64_t) T.tv_sec * 1000000000 + T.tv_nsec; }

static int64_t ClockNs(clockid_t Id) {
  struct timespec T;
  if (syscall(SYS_clock_gettime, Id, &T) != 0) return -1;
  return ToNs(T);
}

// Spins until clock Id has advanced by Nanoseconds; returns the time used.
static int64_t SpinOn(clockid_t Id, int64_t Nanoseconds) {
  const int64_t Start = ClockNs(Id);
  volatile unsigned Sink = 0;
  while (ClockNs(Id) - Start < Nanoseconds) Sink = Sink + 1;
  (void) Sink;
  return ClockNs(Id) - Start;
}

TEST(clock, all_ids_gettime) {
  struct timespec T;
  static const clockid_t Plain[] = {CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID,
                                    CLOCK_MONOTONIC_RAW, CLOCK_REALTIME_COARSE, CLOCK_MONOTONIC_COARSE, CLOCK_BOOTTIME,
                                    CLOCK_TAI};
  for (unsigned Index = 0; Index < sizeof(Plain) / sizeof(*Plain); ++Index) {
    CHECK(syscall(SYS_clock_gettime, Plain[Index], &T) == 0);
    CHECK(T.tv_sec >= 0 && T.tv_nsec >= 0 && T.tv_nsec < 1000000000);
  }
  // The alarm clocks read when an RTC can wake the system. The guest has none
  // and answers like the plain clocks (ProcessInventory.md); a host without an
  // RTC answers EINVAL.
  for (clockid_t Alarm = CLOCK_REALTIME_ALARM; Alarm <= CLOCK_BOOTTIME_ALARM; ++Alarm) {
    errno = 0;
    CHECK(syscall(SYS_clock_gettime, Alarm, &T) == 0 || errno == EINVAL);
  }
  CHECK(syscall(SYS_clock_gettime, CPU_CLOCK(0, CLK_SCHED), &T) == 0);
  CHECK(syscall(SYS_clock_gettime, CPU_CLOCK(getpid(), CLK_SCHED), &T) == 0);
  CHECK(syscall(SYS_clock_gettime, CPU_CLOCK(getpid(), CLK_PROF), &T) == 0);
  CHECK(syscall(SYS_clock_gettime, CPU_CLOCK(getpid(), CLK_VIRT), &T) == 0);
  CHECK(syscall(SYS_clock_gettime, CPU_CLOCK(0, CLK_SCHED) | CPUCLOCK_PERTHREAD_BIT, &T) == 0);
  CHECK(syscall(SYS_clock_gettime, CPU_CLOCK(syscall(SYS_gettid), CLK_SCHED) | CPUCLOCK_PERTHREAD_BIT, &T) == 0);
  // Any process may be read: the parent is the suite runner.
  CHECK(syscall(SYS_clock_gettime, CPU_CLOCK(getppid(), CLK_SCHED), &T) == 0);
  CHECK(ClockNs(CLOCK_REALTIME) > 1000000000LL * 1000000000);          // after 2001, from a real RTC
}

TEST(clock, invalid_ids_einval) {
  struct timespec T;
  const clockid_t Bad[] = {10, 12, 99, 1000, CPU_CLOCK(0, 3), CPU_CLOCK(getpid(), 3), CPU_CLOCK(999999, CLK_SCHED),
                                  CPU_CLOCK(999999, CLK_SCHED) | CPUCLOCK_PERTHREAD_BIT,
                                  CPU_CLOCK(1, CLK_SCHED) | CPUCLOCK_PERTHREAD_BIT}; // another process's thread
  for (unsigned Index = 0; Index < sizeof(Bad) / sizeof(*Bad); ++Index) {
    CHECK_ERRNO(syscall(SYS_clock_gettime, Bad[Index], &T), EINVAL);
    CHECK_ERRNO(syscall(SYS_clock_getres, Bad[Index], &T), EINVAL);
  }
  // An invalid id fails before the output pointer is used; a valid id then faults.
  CHECK_ERRNO(syscall(SYS_clock_gettime, 99, (void *) 8), EINVAL);
  CHECK_ERRNO(syscall(SYS_clock_gettime, CLOCK_REALTIME, (void *) 8), EFAULT);
  CHECK_ERRNO(syscall(SYS_clock_gettime, CLOCK_MONOTONIC, NULL), EFAULT);
  CHECK_ERRNO(syscall(SYS_clock_getres, CLOCK_REALTIME, (void *) 8), EFAULT);
  CHECK_ERRNO(syscall(SYS_clock_getres, 99, NULL), EINVAL);
}

TEST(clock, getres_values) {
  struct timespec T;
  static const clockid_t Fine[] = {CLOCK_REALTIME, CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW, CLOCK_BOOTTIME, CLOCK_TAI,
                                   CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID, CPU_CLOCK(0, CLK_SCHED)};
  for (unsigned Index = 0; Index < sizeof(Fine) / sizeof(*Fine); ++Index) {
    CHECK(syscall(SYS_clock_getres, Fine[Index], &T) == 0);
    CHECK(T.tv_sec == 0 && T.tv_nsec == 1);
  }
  // Coarse and PROF/VIRT clocks tick once per jiffy: 1 ms to 4 ms by kernel config.
  const clockid_t Jiffy[] = {CLOCK_REALTIME_COARSE, CLOCK_MONOTONIC_COARSE, CPU_CLOCK(0, CLK_PROF),
                                    CPU_CLOCK(getpid(), CLK_VIRT)};
  for (unsigned Index = 0; Index < sizeof(Jiffy) / sizeof(*Jiffy); ++Index) {
    CHECK(syscall(SYS_clock_getres, Jiffy[Index], &T) == 0);
    CHECK(T.tv_sec == 0 && T.tv_nsec >= 1000000 && T.tv_nsec <= 10000000);
  }
  CHECK(syscall(SYS_clock_getres, CLOCK_REALTIME, NULL) == 0); // a null result is allowed
  CHECK(syscall(SYS_clock_getres, CLOCK_REALTIME_COARSE, NULL) == 0);
}

TEST(clock, realtime_subsecond_advances) {
  const int64_t First = ClockNs(CLOCK_REALTIME);
  SleepMs(20);
  const int64_t Second = ClockNs(CLOCK_REALTIME);
  const int64_t Elapsed = Second - First;
  CHECK(Elapsed >= 15000000 && Elapsed <= 100000000);
  // Sub-second digits are real, not a whole-second tick.
  int Fractional = 0;
  for (int Index = 0; Index < 5; ++Index) {
    if (ClockNs(CLOCK_REALTIME) % 1000000000 != 0) ++Fractional;
    SleepMs(3);
  }
  CHECK(Fractional > 0);
}

TEST(clock, realtime_matches_time) {
  struct timespec T;
  CHECK(syscall(SYS_clock_gettime, CLOCK_REALTIME, &T) == 0);
  const time_t Seconds = time(NULL);
  CHECK(llabs((long long) Seconds - (long long) T.tv_sec) <= 1);
  struct timeval V;
  CHECK(syscall(SYS_gettimeofday, &V, NULL) == 0);
  CHECK(V.tv_usec >= 0 && V.tv_usec < 1000000);
  CHECK(llabs((long long) V.tv_sec - (long long) T.tv_sec) <= 1);
  CHECK(llabs((long long) syscall(SYS_time, NULL) - (long long) T.tv_sec) <= 1);
  time_t Stored = 0;
  const long Result = syscall(SYS_time, &Stored);
  CHECK(Result == (long) Stored && Result > 0);
  struct timespec Coarse;
  CHECK(syscall(SYS_clock_gettime, CLOCK_REALTIME_COARSE, &Coarse) == 0);
  CHECK(llabs((long long) Coarse.tv_sec - (long long) T.tv_sec) <= 1);
  // The adjtimex clock and the TAI clock follow the realtime clock.
  struct timespec Tai;
  CHECK(syscall(SYS_clock_gettime, CLOCK_TAI, &Tai) == 0);
  CHECK(llabs((long long) Tai.tv_sec - (long long) T.tv_sec) <= 100); // TAI - UTC offset is 0 or 37 s
}

TEST(clock, file_times_match_realtime) {
  char Name[] = "./clock-file.XXXXXX";
  const int FD = mkstemp(Name);
  if (FD < 0) SKIP("no writable scratch directory");
  CHECK(write(FD, "x", 1) == 1);
  struct stat Info;
  CHECK(fstat(FD, &Info) == 0);
  struct timespec Now;
  CHECK(syscall(SYS_clock_gettime, CLOCK_REALTIME, &Now) == 0);
  CHECK(llabs((long long) Info.st_mtim.tv_sec - (long long) Now.tv_sec) <= 2);
  CHECK(Info.st_mtim.tv_sec <= Now.tv_sec + 1);
  close(FD);
  unlink(Name);
}

TEST(clock, monotonic_never_decreases) {
  static const clockid_t Clocks[] = {CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW, CLOCK_BOOTTIME, CLOCK_MONOTONIC_COARSE};
  for (unsigned Index = 0; Index < sizeof(Clocks) / sizeof(*Clocks); ++Index) {
    int64_t Last = ClockNs(Clocks[Index]);
    int Backwards = 0;
    for (int Read = 0; Read < 2000; ++Read) {
      const int64_t Now = ClockNs(Clocks[Index]);
      if (Now < Last) ++Backwards;
      Last = Now;
    }
    CHECK(Backwards == 0);
  }
  // The coarse clock trails the precise one by less than a few jiffies and never leads it.
  const int64_t CoarseNs = ClockNs(CLOCK_MONOTONIC_COARSE);
  const int64_t PreciseNs = ClockNs(CLOCK_MONOTONIC);
  CHECK(CoarseNs <= PreciseNs && PreciseNs - CoarseNs < 20000000);
  // MONOTONIC and BOOTTIME agree (no suspend happens), within a scheduling gap.
  CHECK(llabs(ClockNs(CLOCK_MONOTONIC) - ClockNs(CLOCK_BOOTTIME)) < 100000000);
}

TEST(clock, process_cpu_advances_with_work) {
  const int64_t Before = ClockNs(CLOCK_PROCESS_CPUTIME_ID);
  const int64_t Used = SpinOn(CLOCK_PROCESS_CPUTIME_ID, 20000000);
  const int64_t After = ClockNs(CLOCK_PROCESS_CPUTIME_ID);
  CHECK(Used >= 20000000);
  CHECK(After - Before >= 20000000);
  const int64_t Thread = ClockNs(CLOCK_THREAD_CPUTIME_ID); // the only thread
  const int64_t Process = ClockNs(CLOCK_PROCESS_CPUTIME_ID);
  CHECK(Thread <= Process && Thread >= Used);
  CHECK(Process >= After);
  CHECK(ClockNs(CPU_CLOCK(0, CLK_SCHED)) >= Process);
  // Sleeping does not use CPU time.
  const int64_t Idle = ClockNs(CLOCK_PROCESS_CPUTIME_ID);
  SleepMs(50);
  CHECK(ClockNs(CLOCK_PROCESS_CPUTIME_ID) - Idle < 30000000);
}

static volatile int WorkerState; // 1 spinning, 2 done spinning, 3 told to exit
static volatile int64_t WorkerSpun;

static void *SpinThenWait(void *Argument) {
  (void) Argument;
  WorkerSpun = SpinOn(CLOCK_THREAD_CPUTIME_ID, 30000000);
  WorkerState = 2;
  while (WorkerState != 3) SleepMs(2);
  return NULL;
}

TEST(clock, thread_cpu_per_thread) {
  WorkerState = 1;
  const int64_t MainBefore = ClockNs(CLOCK_THREAD_CPUTIME_ID);
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, SpinThenWait, NULL) == 0);
  const uint64_t Limit = Deadline(8000);
  while (WorkerState != 2 && !DeadlineExpired(Limit)) SleepMs(2);
  CHECK(WorkerState == 2);
  clockid_t WorkerClock;
  CHECK(pthread_getcpuclockid(Thread, &WorkerClock) == 0);
  const int64_t Worker = ClockNs(WorkerClock);
  const int64_t MainUsed = ClockNs(CLOCK_THREAD_CPUTIME_ID) - MainBefore;
  CHECK(Worker >= 30000000);
  CHECK(MainUsed < Worker); // waiting on the thread costs the waiter far less than the work
  CHECK(ClockNs(CLOCK_PROCESS_CPUTIME_ID) >= Worker);
  struct timespec T;
  CHECK(syscall(SYS_clock_getres, WorkerClock, &T) == 0);
  WorkerState = 3;
  CHECK(pthread_join(Thread, NULL) == 0);
}

static int ChildReady[2], ChildHold[2];

static void SpinThenBlock(void) {
  SpinOn(CLOCK_PROCESS_CPUTIME_ID, 30000000);
  CHECK(write(ChildReady[1], "x", 1) == 1);
  char Byte;
  (void) !read(ChildHold[0], &Byte, 1);
}

TEST(clock, pid_cpu_clock_of_child) {
  CHECK(pipe(ChildReady) == 0);
  CHECK(pipe(ChildHold) == 0);
  const pid_t Child = Fork(SpinThenBlock);
  CHECK(Child > 0);
  char Byte;
  CHECK(read(ChildReady[0], &Byte, 1) == 1);
  const clockid_t ChildClock = CPU_CLOCK(Child, CLK_SCHED);
  const int64_t Used = ClockNs(ChildClock);
  CHECK(Used >= 30000000);
  CHECK(ClockNs(CPU_CLOCK(Child, CLK_PROF)) >= 20000000); // jiffy-sampled natively: may lag by a few ms
  CHECK(ClockNs(CLOCK_PROCESS_CPUTIME_ID) < Used + 25000000); // the parent's own time is separate
  CHECK(ClockNs(ChildClock) >= Used);
  // A thread clock of another process is not readable.
  struct timespec T;
  CHECK_ERRNO(syscall(SYS_clock_gettime, ChildClock | CPUCLOCK_PERTHREAD_BIT, &T), EINVAL);
  CHECK(kill(Child, SIGKILL) == 0);
  const int Status = WaitExit(Child);
  CHECK(WIFSIGNALED(Status));
  CHECK_ERRNO(syscall(SYS_clock_gettime, ChildClock, &T), EINVAL); // reaped: no such process
}

TEST(clock, settime_eperm_unprivileged) {
  CHECK(BecomeUnprivileged() == 0);
  struct timespec Now;
  CHECK(syscall(SYS_clock_gettime, CLOCK_REALTIME, &Now) == 0);
  CHECK_ERRNO(syscall(SYS_clock_settime, CLOCK_REALTIME, &Now), EPERM);
  struct timeval V = {Now.tv_sec, 0};
  CHECK_ERRNO(syscall(SYS_settimeofday, &V, NULL), EPERM);
  CHECK_ERRNO(syscall(SYS_settimeofday, NULL, NULL), EPERM);
  struct timezone Zone = {0, 0};
  CHECK_ERRNO(syscall(SYS_settimeofday, NULL, &Zone), EPERM);
  // Dynamic CPU clocks can never be set, whoever asks.
  CHECK_ERRNO(syscall(SYS_clock_settime, CPU_CLOCK(getpid(), CLK_SCHED), &Now), EPERM);
  // The clock did not move backwards or change.
  CHECK(llabs(ClockNs(CLOCK_REALTIME) - ToNs(Now)) < 1000000000LL);
}

TEST(clock, settime_monotonic_einval) {
  struct timespec Zero = {1000, 0};
  static const clockid_t NotSettable[] = {CLOCK_MONOTONIC, CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID,
                                         CLOCK_MONOTONIC_RAW, CLOCK_REALTIME_COARSE, CLOCK_MONOTONIC_COARSE,
                                         CLOCK_BOOTTIME, CLOCK_REALTIME_ALARM, CLOCK_BOOTTIME_ALARM, CLOCK_TAI, 10, 12};
  for (unsigned Index = 0; Index < sizeof(NotSettable) / sizeof(*NotSettable); ++Index) {
    CHECK_ERRNO(syscall(SYS_clock_settime, NotSettable[Index], &Zero), EINVAL);
  }
  // Request errors come before the capability check.
  struct timespec Bad = {1000, 1000000000};
  CHECK_ERRNO(syscall(SYS_clock_settime, CLOCK_REALTIME, &Bad), EINVAL);
  Bad = (struct timespec) {1000, -1};
  CHECK_ERRNO(syscall(SYS_clock_settime, CLOCK_REALTIME, &Bad), EINVAL);
  Bad = (struct timespec) {-1, 0};
  CHECK_ERRNO(syscall(SYS_clock_settime, CLOCK_REALTIME, &Bad), EINVAL);
  CHECK_ERRNO(syscall(SYS_clock_settime, CLOCK_REALTIME, NULL), EFAULT);
  CHECK_ERRNO(syscall(SYS_clock_settime, CLOCK_MONOTONIC, NULL), EINVAL);
  // Dynamic CPU clocks: a missing process is EINVAL, an existing one EPERM.
  CHECK_ERRNO(syscall(SYS_clock_settime, CPU_CLOCK(999999, CLK_SCHED), &Zero), EINVAL);
  CHECK_ERRNO(syscall(SYS_clock_settime, CPU_CLOCK(getpid(), CLK_SCHED), &Zero), EPERM);
}

TEST(clock, settimeofday_bad_usec_einval) {
  struct timeval V = {1000, 1000000};
  CHECK_ERRNO(syscall(SYS_settimeofday, &V, NULL), EINVAL);
  V = (struct timeval) {1000, -1};
  CHECK_ERRNO(syscall(SYS_settimeofday, &V, NULL), EINVAL);
  V = (struct timeval) {-1, 0};
  CHECK_ERRNO(syscall(SYS_settimeofday, &V, NULL), EINVAL);
  CHECK_ERRNO(syscall(SYS_settimeofday, (void *) 8, NULL), EFAULT);
  CHECK_ERRNO(syscall(SYS_settimeofday, NULL, (void *) 8), EFAULT);
}

TEST(clock, adjtimex_readonly) {
  struct timex T;
  memset(&T, 0, sizeof(T));
  // TIME_OK when an NTP daemon synchronised the clock, TIME_ERROR while unsynchronised.
  long Result = adjtimex(&T);
  CHECK(Result == TIME_OK || Result == TIME_ERROR);
  CHECK(T.tick == 10000); // USER_HZ 100
  CHECK(T.precision == 1);
  CHECK(T.tolerance == 32768000);
  CHECK(T.modes == 0);
  struct timespec Now;
  CHECK(syscall(SYS_clock_gettime, CLOCK_REALTIME, &Now) == 0);
  CHECK(llabs((long long) T.time.tv_sec - (long long) Now.tv_sec) <= 1);
  CHECK(T.time.tv_usec >= 0 && T.time.tv_usec < ((T.status & STA_NANO) ? 1000000000 : 1000000));
  if (Result == TIME_ERROR) CHECK(T.status & STA_UNSYNC);
  CHECK(T.tai >= 0);
  // clock_adjtime reads the realtime clock only.
  memset(&T, 0, sizeof(T));
  Result = syscall(SYS_clock_adjtime, CLOCK_REALTIME, &T);
  CHECK(Result == TIME_OK || Result == TIME_ERROR);
  memset(&T, 0, sizeof(T));
  CHECK_ERRNO(syscall(SYS_clock_adjtime, CLOCK_MONOTONIC, &T), EOPNOTSUPP);
  CHECK_ERRNO(syscall(SYS_clock_adjtime, CLOCK_TAI, &T), EOPNOTSUPP);
  CHECK_ERRNO(syscall(SYS_clock_adjtime, CPU_CLOCK(getpid(), CLK_SCHED), &T), EOPNOTSUPP);
  CHECK_ERRNO(syscall(SYS_clock_adjtime, 99, &T), EINVAL);
  CHECK_ERRNO(syscall(SYS_adjtimex, NULL), EFAULT);
  // adjtime(3)'s read-only form works for anyone; the other adjtime forms need a bare single-shot bit.
  memset(&T, 0, sizeof(T));
  T.modes = ADJ_OFFSET_SS_READ;
  CHECK(syscall(SYS_adjtimex, &T) >= 0);
  memset(&T, 0, sizeof(T));
  T.modes = ADJ_ADJTIME;
  CHECK_ERRNO(syscall(SYS_adjtimex, &T), EINVAL);
}

TEST(clock, adjtimex_eperm_unprivileged) {
  CHECK(BecomeUnprivileged() == 0);
  struct timex T;
  static const unsigned Modes[] = {ADJ_OFFSET, ADJ_FREQUENCY, ADJ_MAXERROR, ADJ_STATUS, ADJ_TIMECONST, ADJ_TAI,
                                   ADJ_SETOFFSET, ADJ_TICK, ADJ_OFFSET_SINGLESHOT};
  for (unsigned Index = 0; Index < sizeof(Modes) / sizeof(*Modes); ++Index) {
    memset(&T, 0, sizeof(T));
    T.modes = Modes[Index];
    CHECK_ERRNO(syscall(SYS_adjtimex, &T), EPERM);
    CHECK_ERRNO(syscall(SYS_clock_adjtime, CLOCK_REALTIME, &T), EPERM);
  }
  memset(&T, 0, sizeof(T));
  T.modes = ADJ_TICK;
  T.tick = 1; // out of range, but the capability check comes first
  CHECK_ERRNO(syscall(SYS_adjtimex, &T), EPERM);
}

// Privileged cases only act when the caller really holds CAP_SYS_TIME; each
// leaves the clock where it found it (set to the value just read).
TEST(clock, settime_privileged_roundtrip) {
  struct timespec Now;
  CHECK(syscall(SYS_clock_gettime, CLOCK_REALTIME, &Now) == 0);
  errno = 0;
  if (syscall(SYS_clock_settime, CLOCK_REALTIME, &Now) != 0) {
    if (errno == EPERM) SKIP("needs CAP_SYS_TIME");
    CHECK(0);
    return;
  }
  const int64_t Set = ClockNs(CLOCK_REALTIME);
  CHECK(llabs(Set - ToNs(Now)) < 1000000000LL);
  struct timeval V = {Now.tv_sec, Now.tv_nsec / 1000};
  CHECK(syscall(SYS_settimeofday, &V, NULL) == 0);
  struct timex T;
  memset(&T, 0, sizeof(T));
  T.modes = ADJ_SETOFFSET | ADJ_NANO; // zero offset: the clock stays put
  CHECK(syscall(SYS_adjtimex, &T) >= 0);
  CHECK(llabs(ClockNs(CLOCK_REALTIME) - ToNs(Now)) < 1000000000LL);
  struct timespec Over = {Now.tv_sec, 1000000000};
  CHECK_ERRNO(syscall(SYS_clock_settime, CLOCK_REALTIME, &Over), EINVAL);
  memset(&T, 0, sizeof(T));
  T.modes = ADJ_SETOFFSET | ADJ_NANO;
  T.time.tv_usec = 1000000000; // nanoseconds out of range
  CHECK_ERRNO(syscall(SYS_adjtimex, &T), EINVAL);
}

static void *SpinUntilWorkerDone(void *Argument) {
  (void) Argument;
  volatile unsigned Sink = 0;
  while (WorkerState != 3) Sink = Sink + 1;
  return NULL;
}

TEST(clock, cpu_nanosleep_process_clock) {
  struct timespec Request = {0, 30000000}, Left = {0, 0};
  WorkerState = 1;
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, SpinUntilWorkerDone, NULL) == 0);
  // The sleeper burns no CPU itself: the other thread's work ends the sleep.
  const int64_t CpuBefore = ClockNs(CLOCK_PROCESS_CPUTIME_ID);
  const uint64_t Start = NowMs();
  CHECK(syscall(SYS_clock_nanosleep, CLOCK_PROCESS_CPUTIME_ID, 0, &Request, &Left) == 0);
  CHECK(ClockNs(CLOCK_PROCESS_CPUTIME_ID) - CpuBefore >= 30000000);
  CHECK(NowMs() - Start < 5000);
  // Absolute: until the process has used 20 ms more CPU time.
  const int64_t Target = ClockNs(CLOCK_PROCESS_CPUTIME_ID) + 20000000;
  struct timespec Absolute = {Target / 1000000000, Target % 1000000000};
  CHECK(syscall(SYS_clock_nanosleep, CLOCK_PROCESS_CPUTIME_ID, TIMER_ABSTIME, &Absolute, NULL) == 0);
  CHECK(ClockNs(CLOCK_PROCESS_CPUTIME_ID) >= Target);
  // A target already reached returns at once; so does a zero duration.
  CHECK(syscall(SYS_clock_nanosleep, CLOCK_PROCESS_CPUTIME_ID, TIMER_ABSTIME, &Absolute, NULL) == 0);
  Request = (struct timespec) {0, 0};
  CHECK(syscall(SYS_clock_nanosleep, CLOCK_PROCESS_CPUTIME_ID, 0, &Request, NULL) == 0);
  // The dynamic form of the caller's own process clock sleeps too.
  Request = (struct timespec) {0, 10000000};
  CHECK(syscall(SYS_clock_nanosleep, CPU_CLOCK(0, CLK_SCHED), 0, &Request, NULL) == 0);
  WorkerState = 3;
  CHECK(pthread_join(Thread, NULL) == 0);
  // Clocks without a sleep operation, and unusable CPU clocks.
  Request = (struct timespec) {0, 1000000};
  CHECK_ERRNO(syscall(SYS_clock_nanosleep, CLOCK_THREAD_CPUTIME_ID, 0, &Request, NULL), EOPNOTSUPP);
  CHECK_ERRNO(syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC_RAW, 0, &Request, NULL), EOPNOTSUPP);
  CHECK_ERRNO(syscall(SYS_clock_nanosleep, CLOCK_REALTIME_COARSE, 0, &Request, NULL), EOPNOTSUPP);
  CHECK_ERRNO(syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC_COARSE, 0, &Request, NULL), EOPNOTSUPP);
  CHECK_ERRNO(syscall(SYS_clock_nanosleep, CPU_CLOCK(0, CLK_SCHED) | CPUCLOCK_PERTHREAD_BIT, 0, &Request, NULL), EINVAL);
  CHECK_ERRNO(syscall(SYS_clock_nanosleep, CPU_CLOCK(999999, CLK_SCHED), 0, &Request, NULL), EINVAL);
  CHECK_ERRNO(syscall(SYS_clock_nanosleep, 10, 0, &Request, NULL), EINVAL);
  CHECK_ERRNO(syscall(SYS_clock_nanosleep, CLOCK_PROCESS_CPUTIME_ID, 0, (void *) 8, NULL), EFAULT);
  Request = (struct timespec) {0, -1};
  CHECK_ERRNO(syscall(SYS_clock_nanosleep, CLOCK_PROCESS_CPUTIME_ID, 0, &Request, NULL), EINVAL);
}

TEST(clock, alarm_sleep_needs_capability) {
  CHECK(BecomeUnprivileged() == 0);
  struct timespec Request = {0, 1000000};
  CHECK_ERRNO(syscall(SYS_clock_nanosleep, CLOCK_REALTIME_ALARM, 0, &Request, NULL), EPERM);
  CHECK_ERRNO(syscall(SYS_clock_nanosleep, CLOCK_BOOTTIME_ALARM, 0, &Request, NULL), EPERM);
}

TEST(clock, absolute_realtime_sleep) {
  static const clockid_t Clocks[] = {CLOCK_REALTIME, CLOCK_TAI, CLOCK_MONOTONIC, CLOCK_BOOTTIME};
  for (unsigned Index = 0; Index < sizeof(Clocks) / sizeof(*Clocks); ++Index) {
    const int64_t Target = ClockNs(Clocks[Index]) + 40000000;
    struct timespec Absolute = {Target / 1000000000, Target % 1000000000};
    const int64_t Start = ClockNs(CLOCK_MONOTONIC);
    CHECK(syscall(SYS_clock_nanosleep, Clocks[Index], TIMER_ABSTIME, &Absolute, NULL) == 0);
    const int64_t Elapsed = ClockNs(CLOCK_MONOTONIC) - Start;
    CHECK(Elapsed >= 30000000 && Elapsed < 1000000000);
    CHECK(ClockNs(Clocks[Index]) >= Target);
  }
}

TEST(clock, gettimeofday_null_tv_ok) {
  CHECK(syscall(SYS_gettimeofday, NULL, NULL) == 0);
  int Zone[2] = {-1, -1};
  CHECK(syscall(SYS_gettimeofday, NULL, Zone) == 0);
  CHECK(Zone[0] >= -15 * 60 && Zone[0] <= 15 * 60); // minutes west of UTC; the kernel zone is rarely set
  struct timeval V = {0, 0};
  CHECK(syscall(SYS_gettimeofday, &V, NULL) == 0 && V.tv_sec > 0);
  CHECK_ERRNO(syscall(SYS_gettimeofday, (void *) 8, NULL), EFAULT);
  V.tv_sec = 0;
  CHECK_ERRNO(syscall(SYS_gettimeofday, &V, (void *) 8), EFAULT);
  CHECK(V.tv_sec > 0); // the time was written before the zone faulted
  CHECK_ERRNO(syscall(SYS_time, (void *) 8), EFAULT);
}

// ---------------------------------------------------------------------------
// Timers: alarm, itimers, POSIX timers and timerfd.

// Blocks Signal and waits for it; returns the signal or -1.
static int WaitBlocked(int Signal, siginfo_t *Info) {
  sigset_t Set;
  sigemptyset(&Set);
  sigaddset(&Set, Signal);
  int Result;
  do Result = sigwaitinfo(&Set, Info); while (Result < 0 && errno == EINTR);
  return Result;
}

static int CreateTimer(clockid_t Clock, int Notify, int Signal, long Value, pid_t Tid, int *Id) {
  struct sigevent Event;
  memset(&Event, 0, sizeof(Event));
  Event.sigev_notify = Notify;
  Event.sigev_signo = Signal;
  Event.sigev_value.sival_ptr = (void *)Value;
  Event._sigev_un._tid = Tid;
  return (int)syscall(SYS_timer_create, Clock, &Event, Id);
}

static int ArmTimer(int Id, int Flags, long ValueNs, long IntervalNs) {
  struct itimerspec Spec = {{IntervalNs / 1000000000, IntervalNs % 1000000000}, {ValueNs / 1000000000, ValueNs % 1000000000}};
  return (int)syscall(SYS_timer_settime, Id, Flags, &Spec, NULL);
}

TEST(timer, alarm_returns_remaining) {
  CHECK(alarm(5) == 0);
  CHECK(alarm(3) == 5); // rounded to the nearest second
  CHECK(alarm(0) == 3);
  CHECK(alarm(0) == 0);
}

TEST(timer, alarm_zero_cancels) {
  ResetRecorder();
  CHECK(Catch(SIGALRM, 0) == 0);
  alarm(1);
  CHECK(alarm(0) == 1);
  SleepMs(1300);
  CHECK(HandlerCalls == 0);
}

TEST(timer, alarm_delivers_sigalrm) {
  ResetRecorder();
  CHECK(Catch(SIGALRM, SA_SIGINFO) == 0);
  const uint64_t Start = NowMs();
  alarm(1);
  SuspendUntilHandled();
  const uint64_t Elapsed = NowMs() - Start;
  CHECK(HandlerCalls == 1 && LastSignal == SIGALRM);
  CHECK(LastInfo.si_code == SI_KERNEL);
  CHECK(Elapsed >= 900 && Elapsed < 3000);
}

TEST(timer, setitimer_real_periodic) {
  ResetRecorder();
  CHECK(Catch(SIGALRM, 0) == 0);
  struct itimerval Value = {{0, 10000}, {0, 10000}}, Old;
  const uint64_t Start = NowMs();
  CHECK(setitimer(ITIMER_REAL, &Value, NULL) == 0);
  sigset_t Empty;
  sigemptyset(&Empty);
  while (HandlerCalls < 3 && NowMs() - Start < 3000) sigsuspend(&Empty);
  const uint64_t Elapsed = NowMs() - Start;
  memset(&Value, 0, sizeof(Value));
  CHECK(setitimer(ITIMER_REAL, &Value, &Old) == 0);
  CHECK(HandlerCalls >= 3);
  CHECK(Elapsed >= 25 && Elapsed < 1000);
  CHECK(Old.it_interval.tv_sec == 0 && Old.it_interval.tv_usec == 10000);
  CHECK(Old.it_value.tv_sec == 0 && Old.it_value.tv_usec > 0 && Old.it_value.tv_usec <= 10000);
  CHECK(getitimer(ITIMER_REAL, &Old) == 0 && !Old.it_value.tv_sec && !Old.it_value.tv_usec);
}

TEST(timer, setitimer_bad_which_and_usec) {
  struct itimerval Value = {{0, 0}, {0, 1000000}};
  CHECK_ERRNO(syscall(SYS_setitimer, 3, &Value, NULL), EINVAL);
  CHECK_ERRNO(syscall(SYS_setitimer, ITIMER_REAL, &Value, NULL), EINVAL);
  Value.it_value.tv_usec = -1;
  CHECK_ERRNO(syscall(SYS_setitimer, ITIMER_REAL, &Value, NULL), EINVAL);
  Value.it_value.tv_usec = 0;
  Value.it_interval.tv_usec = 1000000;
  CHECK_ERRNO(syscall(SYS_setitimer, ITIMER_VIRTUAL, &Value, NULL), EINVAL);
  CHECK_ERRNO(syscall(SYS_getitimer, 3, &Value), EINVAL);
  CHECK_ERRNO(syscall(SYS_getitimer, ITIMER_REAL, (void *)8), EFAULT);
  CHECK_ERRNO(syscall(SYS_setitimer, ITIMER_REAL, (void *)8, NULL), EFAULT);
  // A null new value disarms the timer.
  alarm(5);
  CHECK(syscall(SYS_setitimer, ITIMER_REAL, NULL, NULL) == 0);
  CHECK(alarm(0) == 0);
}

TEST(timer, getitimer_reports_remaining) {
  struct itimerval Value = {{0, 0}, {2, 0}};
  CHECK(setitimer(ITIMER_REAL, &Value, NULL) == 0);
  SleepMs(100);
  CHECK(getitimer(ITIMER_REAL, &Value) == 0);
  const long Left = Value.it_value.tv_sec * 1000000L + Value.it_value.tv_usec;
  CHECK(Left > 1500000 && Left <= 1910000);
  CHECK(!Value.it_interval.tv_sec && !Value.it_interval.tv_usec);
  memset(&Value, 0, sizeof(Value));
  CHECK(setitimer(ITIMER_REAL, &Value, NULL) == 0);
}

static void CpuItimer(int Which, int Signal) {
  ResetRecorder();
  CHECK(Catch(Signal, 0) == 0);
  struct itimerval Value = {{0, 0}, {0, 50000}};
  const uint64_t Start = NowMs();
  CHECK(setitimer(Which, &Value, NULL) == 0);
  while (!HandlerCalls && NowMs() - Start < 3000) Spins++;
  CHECK(HandlerCalls == 1 && LastSignal == Signal);
  CHECK(NowMs() - Start >= 40);
}

TEST(timer, itimer_virtual_counts_cpu) { CpuItimer(ITIMER_VIRTUAL, SIGVTALRM); }
TEST(timer, itimer_prof_counts_cpu) { CpuItimer(ITIMER_PROF, SIGPROF); }

static void ChildHasNoItimer(void) {
  struct itimerval Value;
  CHECK(getitimer(ITIMER_REAL, &Value) == 0);
  CHECK(!Value.it_value.tv_sec && !Value.it_value.tv_usec);
  CHECK(alarm(0) == 0);
}

TEST(timer, itimer_not_inherited_by_fork) {
  alarm(5);
  const int Status = WaitExit(Fork(ChildHasNoItimer));
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  CHECK(alarm(0) == 5);
}

static void ExecAlarmLeft(void) {
  alarm(7);
  execl(ExePath, ExePath, "--alarm-left", (char *)NULL);
  _exit(99);
}

TEST(timer, alarm_survives_exec) {
  const int Status = WaitExit(Fork(ExecAlarmLeft));
  CHECK(WIFEXITED(Status) && (WEXITSTATUS(Status) == 7 || WEXITSTATUS(Status) == 6));
}

TEST(timer, posix_timer_default_sigevent) {
  CHECK(Block(SIGALRM) == 0);
  int Id = -1;
  CHECK(syscall(SYS_timer_create, CLOCK_MONOTONIC, NULL, &Id) == 0);
  CHECK(ArmTimer(Id, 0, 1000000, 0) == 0);
  siginfo_t Info;
  CHECK(WaitBlocked(SIGALRM, &Info) == SIGALRM);
  CHECK(Info.si_code == SI_TIMER);
  CHECK(Info.si_timerid == Id);
  CHECK(Info.si_value.sival_int == Id);
  CHECK(Info.si_overrun == 0);
  CHECK(syscall(SYS_timer_delete, Id) == 0);
}

TEST(timer, posix_timer_thread_id) {
  CHECK(Block(SIGUSR1) == 0);
  int Id = -1;
  CHECK_ERRNO(CreateTimer(CLOCK_MONOTONIC, SIGEV_THREAD_ID, SIGUSR1, 0, getppid(), &Id), EINVAL);
  CHECK(CreateTimer(CLOCK_MONOTONIC, SIGEV_THREAD_ID, SIGUSR1, 1234, gettid(), &Id) == 0);
  CHECK(ArmTimer(Id, 0, 1000000, 0) == 0);
  SleepMs(20);
  sigset_t Pending;
  CHECK(sigpending(&Pending) == 0 && sigismember(&Pending, SIGUSR1));
  siginfo_t Info;
  CHECK(WaitBlocked(SIGUSR1, &Info) == SIGUSR1);
  CHECK(Info.si_code == SI_TIMER && Info.si_value.sival_ptr == (void *)1234 && Info.si_timerid == Id);
}

TEST(timer, posix_timer_overrun) {
  CHECK(Block(SIGALRM) == 0);
  int Id = -1;
  CHECK(CreateTimer(CLOCK_MONOTONIC, SIGEV_SIGNAL, SIGALRM, 0, 0, &Id) == 0);
  CHECK(ArmTimer(Id, 0, 10000000, 10000000) == 0);
  SleepMs(105);
  siginfo_t Info;
  CHECK(WaitBlocked(SIGALRM, &Info) == SIGALRM);
  CHECK(Info.si_overrun >= 5);
  CHECK(syscall(SYS_timer_getoverrun, Id) == Info.si_overrun);
  struct itimerspec Value;
  CHECK(syscall(SYS_timer_gettime, Id, &Value) == 0);
  CHECK(Value.it_interval.tv_nsec == 10000000 && Value.it_value.tv_nsec > 0 && Value.it_value.tv_nsec <= 10000000);
  CHECK(syscall(SYS_timer_delete, Id) == 0);
}

TEST(timer, posix_timer_errors) {
  int Id = -1, Second = -1;
  CHECK_ERRNO(syscall(SYS_timer_create, 10, NULL, &Id), EINVAL);
  CHECK_ERRNO(syscall(SYS_timer_create, CLOCK_MONOTONIC_RAW, NULL, &Id), EOPNOTSUPP);
  CHECK_ERRNO(syscall(SYS_timer_create, CLOCK_REALTIME_COARSE, NULL, &Id), EOPNOTSUPP);
  CHECK_ERRNO(CreateTimer(CLOCK_MONOTONIC, 3, SIGUSR1, 0, 0, &Id), EINVAL);
  CHECK_ERRNO(CreateTimer(CLOCK_MONOTONIC, SIGEV_SIGNAL, 65, 0, 0, &Id), EINVAL);
  CHECK_ERRNO(CreateTimer(CLOCK_MONOTONIC, SIGEV_SIGNAL, 0, 0, 0, &Id), EINVAL);
  CHECK_ERRNO(syscall(SYS_timer_create, CLOCK_MONOTONIC, NULL, (void *)8), EFAULT);
  CHECK_ERRNO(syscall(SYS_timer_create, CLOCK_MONOTONIC, (void *)8, &Id), EFAULT);
  if (getuid() != 0) CHECK_ERRNO(syscall(SYS_timer_create, CLOCK_REALTIME_ALARM, NULL, &Id), EPERM);
  CHECK(CreateTimer(CLOCK_MONOTONIC, SIGEV_NONE, 65, 0, 0, &Id) == 0);
  CHECK(syscall(SYS_timer_create, CLOCK_MONOTONIC, NULL, &Second) == 0);
  CHECK(Second > Id);
  struct itimerspec Spec = {{0, 0}, {0, 1000000000}};
  CHECK_ERRNO(syscall(SYS_timer_settime, Id, 0, &Spec, NULL), EINVAL);
  CHECK_ERRNO(syscall(SYS_timer_settime, Id, 0, NULL, NULL), EINVAL);
  CHECK_ERRNO(syscall(SYS_timer_settime, Id, 0, (void *)8, NULL), EFAULT);
  Spec.it_value.tv_nsec = 0;
  CHECK_ERRNO(syscall(SYS_timer_settime, 4242, 0, &Spec, NULL), EINVAL);
  CHECK_ERRNO(syscall(SYS_timer_gettime, 4242, &Spec), EINVAL);
  CHECK_ERRNO(syscall(SYS_timer_getoverrun, 4242), EINVAL);
  CHECK_ERRNO(syscall(SYS_timer_delete, 4242), EINVAL);
  CHECK_ERRNO(syscall(SYS_timer_gettime, Id, (void *)8), EFAULT);
  // SIGEV_NONE: the timer runs without a signal.
  CHECK(ArmTimer(Id, 0, 20000000, 0) == 0);
  CHECK(syscall(SYS_timer_gettime, Id, &Spec) == 0 && Spec.it_value.tv_nsec > 0);
  SleepMs(40);
  CHECK(syscall(SYS_timer_gettime, Id, &Spec) == 0 && !Spec.it_value.tv_sec && !Spec.it_value.tv_nsec);
  CHECK(syscall(SYS_timer_delete, Id) == 0);
  CHECK_ERRNO(syscall(SYS_timer_delete, Id), EINVAL);
}

TEST(timer, posix_timer_cpu_clock) {
  ResetRecorder();
  CHECK(Catch(SIGUSR1, SA_SIGINFO) == 0);
  int Id = -1;
  CHECK(CreateTimer(CLOCK_PROCESS_CPUTIME_ID, SIGEV_SIGNAL, SIGUSR1, 77, 0, &Id) == 0);
  const uint64_t Start = NowMs();
  CHECK(ArmTimer(Id, 0, 50000000, 0) == 0);
  while (!HandlerCalls && NowMs() - Start < 3000) Spins++;
  CHECK(HandlerCalls == 1 && LastInfo.si_code == SI_TIMER && LastInfo.si_value.sival_int == 77);
  CHECK(NowMs() - Start >= 40);
}

TEST(timer, stale_timer_signal_dropped) {
  CHECK(Block(SIGUSR2) == 0);
  int Id = -1;
  CHECK(CreateTimer(CLOCK_MONOTONIC, SIGEV_SIGNAL, SIGUSR2, 0, 0, &Id) == 0);
  CHECK(ArmTimer(Id, 0, 1000000, 0) == 0);
  SleepMs(20);
  sigset_t Pending;
  CHECK(sigpending(&Pending) == 0 && sigismember(&Pending, SIGUSR2));
  CHECK(syscall(SYS_timer_delete, Id) == 0);
  // The signal stays pending but is dropped when dequeued (Linux 7.3).
  CHECK(sigpending(&Pending) == 0 && sigismember(&Pending, SIGUSR2));
  const struct timespec Zero = {0, 0};
  sigset_t Set;
  sigemptyset(&Set);
  sigaddset(&Set, SIGUSR2);
  CHECK_ERRNO(sigtimedwait(&Set, NULL, &Zero), EAGAIN);
  // Re-arming makes a queued signal stale the same way.
  CHECK(CreateTimer(CLOCK_MONOTONIC, SIGEV_SIGNAL, SIGUSR2, 0, 0, &Id) == 0);
  CHECK(ArmTimer(Id, 0, 1000000, 0) == 0);
  SleepMs(20);
  CHECK(ArmTimer(Id, 0, 0, 0) == 0);
  CHECK(sigpending(&Pending) == 0 && sigismember(&Pending, SIGUSR2));
  CHECK_ERRNO(sigtimedwait(&Set, NULL, &Zero), EAGAIN);
  // A live one is delivered.
  CHECK(ArmTimer(Id, 0, 1000000, 0) == 0);
  SleepMs(20);
  siginfo_t Info;
  CHECK(sigtimedwait(&Set, &Info, &Zero) == SIGUSR2 && Info.si_code == SI_TIMER);
}

static int TimerFd(int Flags, long ValueNs, long IntervalNs) {
  const int Fd = timerfd_create(CLOCK_MONOTONIC, Flags);
  if (Fd < 0) return Fd;
  struct itimerspec Spec = {{IntervalNs / 1000000000, IntervalNs % 1000000000}, {ValueNs / 1000000000, ValueNs % 1000000000}};
  if (ValueNs && timerfd_settime(Fd, 0, &Spec, NULL) < 0) return -1;
  return Fd;
}

TEST(timer, timerfd_read_ticks) {
  const int Fd = TimerFd(0, 10000000, 10000000);
  CHECK(Fd >= 0);
  SleepMs(55);
  uint64_t Ticks = 0;
  CHECK(read(Fd, &Ticks, sizeof(Ticks)) == 8);
  CHECK(Ticks >= 4 && Ticks <= 8);
  const uint64_t Start = NowMs();
  CHECK(read(Fd, &Ticks, sizeof(Ticks)) == 8); // blocks until the next tick
  CHECK(Ticks >= 1 && NowMs() - Start < 1000);
  struct itimerspec Spec;
  CHECK(timerfd_gettime(Fd, &Spec) == 0);
  CHECK(Spec.it_interval.tv_nsec == 10000000 && Spec.it_value.tv_nsec > 0 && Spec.it_value.tv_nsec <= 10000000);
  close(Fd);
}

TEST(timer, timerfd_nonblock_eagain) {
  const int Fd = TimerFd(TFD_NONBLOCK, 0, 0);
  CHECK(Fd >= 0);
  CHECK((fcntl(Fd, F_GETFL) & (O_NONBLOCK | O_ACCMODE)) == (O_NONBLOCK | O_RDWR));
  uint64_t Ticks;
  CHECK_ERRNO(read(Fd, &Ticks, sizeof(Ticks)), EAGAIN);
  struct itimerspec Spec = {{0, 0}, {0, 1000000}};
  CHECK(timerfd_settime(Fd, 0, &Spec, NULL) == 0);
  SleepMs(20);
  CHECK(read(Fd, &Ticks, sizeof(Ticks)) == 8 && Ticks == 1);
  CHECK_ERRNO(read(Fd, &Ticks, sizeof(Ticks)), EAGAIN);
  close(Fd);
}

TEST(timer, timerfd_short_read_einval) {
  const int Fd = TimerFd(TFD_NONBLOCK, 1000000, 0);
  CHECK(Fd >= 0);
  SleepMs(10);
  uint64_t Ticks;
  CHECK_ERRNO(read(Fd, &Ticks, 4), EINVAL);
  CHECK_ERRNO(read(Fd, &Ticks, 0), EINVAL);
  uint64_t Wide[2] = {0, 0};
  CHECK(read(Fd, Wide, sizeof(Wide)) == 8 && Wide[0] == 1);
  close(Fd);
}

TEST(timer, timerfd_abstime) {
  const int Fd = timerfd_create(CLOCK_MONOTONIC, 0);
  CHECK(Fd >= 0);
  struct timespec Now;
  clock_gettime(CLOCK_MONOTONIC, &Now);
  const int64_t Target = Now.tv_sec * 1000000000LL + Now.tv_nsec + 30000000;
  struct itimerspec Spec = {{0, 0}, {Target / 1000000000, Target % 1000000000}};
  const uint64_t Start = NowMs();
  CHECK(timerfd_settime(Fd, TFD_TIMER_ABSTIME, &Spec, NULL) == 0);
  uint64_t Ticks = 0;
  CHECK(read(Fd, &Ticks, sizeof(Ticks)) == 8 && Ticks == 1);
  CHECK(NowMs() - Start >= 25);
  // A time in the past expires at once.
  Spec.it_value.tv_sec = 1;
  Spec.it_value.tv_nsec = 0;
  CHECK(timerfd_settime(Fd, TFD_TIMER_ABSTIME, &Spec, NULL) == 0);
  CHECK(read(Fd, &Ticks, sizeof(Ticks)) == 8 && Ticks == 1);
  close(Fd);
}

TEST(timer, timerfd_poll_readable) {
  const int Fd = TimerFd(0, 0, 0);
  CHECK(Fd >= 0);
  struct pollfd Entry = {Fd, POLLIN | POLLOUT, 0};
  CHECK(poll(&Entry, 1, 0) == 0);
  struct itimerspec Spec = {{0, 0}, {0, 20000000}};
  CHECK(timerfd_settime(Fd, 0, &Spec, NULL) == 0);
  const uint64_t Start = NowMs();
  CHECK(poll(&Entry, 1, 2000) == 1 && Entry.revents == POLLIN);
  CHECK(NowMs() - Start >= 15);
  close(Fd);
}

TEST(timer, timerfd_set_ticks_ioctl) {
  const int Fd = TimerFd(TFD_NONBLOCK, 0, 0);
  CHECK(Fd >= 0);
  uint64_t Ticks = 5;
  if (ioctl(Fd, TFD_IOC_SET_TICKS, &Ticks) < 0 && errno == ENOTTY) SKIP("kernel without CONFIG_CHECKPOINT_RESTORE");
  Ticks = 0;
  CHECK(read(Fd, &Ticks, sizeof(Ticks)) == 8 && Ticks == 5);
  Ticks = 0;
  CHECK_ERRNO(ioctl(Fd, TFD_IOC_SET_TICKS, &Ticks), EINVAL);
  CHECK_ERRNO(ioctl(Fd, TFD_IOC_SET_TICKS, (void *)8), EFAULT);
  close(Fd);
}

TEST(timer, timerfd_cancel_on_set) {
  const int Fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
  CHECK(Fd >= 0);
  struct timespec Now;
  clock_gettime(CLOCK_REALTIME, &Now);
  struct itimerspec Spec = {{0, 0}, {Now.tv_sec + 100, 0}};
  CHECK(timerfd_settime(Fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &Spec, NULL) == 0);
  clock_gettime(CLOCK_REALTIME, &Now);
  if (clock_settime(CLOCK_REALTIME, &Now) < 0) {
    const int Error = errno;
    CHECK(Error == EPERM);
    close(Fd);
    SKIP("clock_settime needs CAP_SYS_TIME");
  }
  uint64_t Ticks;
  struct pollfd Entry = {Fd, POLLIN, 0};
  CHECK(poll(&Entry, 1, 0) == 1);
  CHECK_ERRNO(read(Fd, &Ticks, sizeof(Ticks)), ECANCELED);
  CHECK_ERRNO(read(Fd, &Ticks, sizeof(Ticks)), EAGAIN);
  CHECK(timerfd_gettime(Fd, &Spec) == 0 && Spec.it_value.tv_sec > 90);
  close(Fd);
}

TEST(timer, timerfd_errors) {
  CHECK_ERRNO(timerfd_create(CLOCK_MONOTONIC_RAW, 0), EINVAL);
  CHECK_ERRNO(timerfd_create(CLOCK_TAI, 0), EINVAL);
  CHECK_ERRNO(timerfd_create(CLOCK_PROCESS_CPUTIME_ID, 0), EINVAL);
  CHECK_ERRNO(timerfd_create(CLOCK_MONOTONIC, 0x80), EINVAL);
  if (getuid() != 0) CHECK_ERRNO(timerfd_create(CLOCK_REALTIME_ALARM, 0), EPERM);
  const int Fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
  CHECK(Fd >= 0);
  CHECK(fcntl(Fd, F_GETFD) == FD_CLOEXEC);
  struct itimerspec Spec = {{0, 0}, {1, 0}};
  CHECK_ERRNO(timerfd_settime(Fd, 4, &Spec, NULL), EINVAL);
  CHECK(timerfd_settime(Fd, TFD_TIMER_CANCEL_ON_SET, &Spec, NULL) == 0); // ignored without ABSTIME on REALTIME
  CHECK_ERRNO(timerfd_settime(Fd, 0, NULL, NULL), EFAULT);
  Spec.it_value.tv_nsec = 1000000000;
  CHECK_ERRNO(timerfd_settime(Fd, 0, &Spec, NULL), EINVAL);
  Spec.it_value.tv_nsec = 0;
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK_ERRNO(timerfd_settime(Pipe[0], 0, &Spec, NULL), EINVAL);
  CHECK_ERRNO(timerfd_gettime(Pipe[0], &Spec), EINVAL);
  CHECK_ERRNO(timerfd_settime(999, 0, &Spec, NULL), EBADF);
  uint64_t Ticks = 1;
  CHECK_ERRNO(write(Fd, &Ticks, sizeof(Ticks)), EINVAL);
  CHECK_ERRNO(pread(Fd, &Ticks, sizeof(Ticks), 0), ESPIPE);
  CHECK(lseek(Fd, 5, SEEK_SET) == 0);
  int Count;
  CHECK_ERRNO(ioctl(Fd, FIONREAD, &Count), ENOTTY);
  struct stat Info;
  CHECK(fstat(Fd, &Info) == 0);
  CHECK(Info.st_mode == 0600 && Info.st_nlink == 1 && Info.st_size == 0);
  CHECK(Info.st_uid == 0 || Info.st_uid == 65534); // root, or the overflow uid in a user namespace
  close(Fd);
}

TEST(timer, timerfd_fork_shares_ticks) {
  const int Fd = TimerFd(TFD_NONBLOCK, 1000000, 0);
  CHECK(Fd >= 0);
  SleepMs(10);
  const pid_t Child = fork();
  if (!Child) {
    uint64_t Ticks = 0;
    _exit(read(Fd, &Ticks, sizeof(Ticks)) == 8 && Ticks == 1 ? 0 : 1);
  }
  const int Status = WaitExit(Child);
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  uint64_t Ticks;
  CHECK_ERRNO(read(Fd, &Ticks, sizeof(Ticks)), EAGAIN);
  close(Fd);
}

// ---------------------------------------------------------------------------
// Event descriptors: eventfd and signalfd.

TEST(event, eventfd_counter) {
  const int Fd = eventfd(3, 0);
  CHECK(Fd >= 0);
  uint64_t Value = 4;
  CHECK(write(Fd, &Value, sizeof(Value)) == 8);
  Value = 0;
  CHECK(read(Fd, &Value, sizeof(Value)) == 8 && Value == 7);
  Value = 0;
  CHECK(write(Fd, &Value, sizeof(Value)) == 8); // adding 0 succeeds
  CHECK(fcntl(Fd, F_SETFL, O_NONBLOCK) == 0);
  CHECK_ERRNO(read(Fd, &Value, sizeof(Value)), EAGAIN);
  close(Fd);
}

TEST(event, eventfd_semaphore) {
  const int Fd = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
  CHECK(Fd >= 0);
  uint64_t Value = 0;
  CHECK(read(Fd, &Value, sizeof(Value)) == 8 && Value == 1);
  CHECK(read(Fd, &Value, sizeof(Value)) == 8 && Value == 1);
  CHECK_ERRNO(read(Fd, &Value, sizeof(Value)), EAGAIN);
  close(Fd);
}

TEST(event, eventfd_nonblock_eagain) {
  const int Fd = eventfd(0, EFD_NONBLOCK);
  CHECK(Fd >= 0);
  uint64_t Value;
  CHECK_ERRNO(read(Fd, &Value, sizeof(Value)), EAGAIN);
  Value = UINT64_MAX - 1;
  CHECK(write(Fd, &Value, sizeof(Value)) == 8);
  Value = 1;
  CHECK_ERRNO(write(Fd, &Value, sizeof(Value)), EAGAIN); // would exceed the maximum
  Value = UINT64_MAX;
  CHECK_ERRNO(write(Fd, &Value, sizeof(Value)), EINVAL);
  close(Fd);
}

TEST(event, eventfd_short_buffer_einval) {
  const int Fd = eventfd(1, 0);
  CHECK(Fd >= 0);
  uint64_t Value = 1;
  CHECK_ERRNO(read(Fd, &Value, 4), EINVAL);
  CHECK_ERRNO(read(Fd, &Value, 0), EINVAL);
  CHECK_ERRNO(write(Fd, &Value, 7), EINVAL);
  CHECK_ERRNO(pread(Fd, &Value, 8, 0), ESPIPE);
  uint64_t Wide[2] = {0, 0};
  CHECK(read(Fd, Wide, sizeof(Wide)) == 8 && Wide[0] == 1);
  close(Fd);
}

TEST(event, eventfd_bad_flags_einval) {
  CHECK_ERRNO(eventfd(0, 8), EINVAL);
  CHECK_ERRNO(eventfd(0, O_RDWR), EINVAL);
  const int Fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  CHECK(Fd >= 0);
  CHECK(fcntl(Fd, F_GETFD) == FD_CLOEXEC);
  CHECK(fcntl(Fd, F_GETFL) == (O_RDWR | O_NONBLOCK));
  CHECK(syscall(SYS_eventfd, 5) >= 0); // the legacy call takes no flags
  close(Fd);
}

static int SharedEventFd;
static void *WriteEventLater(void *Unused) {
  (void)Unused;
  SleepMs(50);
  uint64_t Value = 9;
  if (write(SharedEventFd, &Value, sizeof(Value)) != 8) return (void *)1;
  return NULL;
}

TEST(event, eventfd_blocking_read_woken_by_thread) {
  SharedEventFd = eventfd(0, 0);
  CHECK(SharedEventFd >= 0);
  pthread_t Writer;
  CHECK(pthread_create(&Writer, NULL, WriteEventLater, NULL) == 0);
  uint64_t Value = 0;
  const uint64_t Start = NowMs();
  CHECK(read(SharedEventFd, &Value, sizeof(Value)) == 8 && Value == 9);
  CHECK(NowMs() - Start >= 40);
  void *Result;
  CHECK(pthread_join(Writer, &Result) == 0 && !Result);
  close(SharedEventFd);
}

TEST(event, eventfd_blocking_write_waits_for_read) {
  SharedEventFd = eventfd(0, 0);
  CHECK(SharedEventFd >= 0);
  uint64_t Value = UINT64_MAX - 1;
  CHECK(write(SharedEventFd, &Value, sizeof(Value)) == 8);
  const pid_t Child = fork();
  if (!Child) {
    SleepMs(50);
    uint64_t Read = 0;
    _exit(read(SharedEventFd, &Read, sizeof(Read)) == 8 && Read == UINT64_MAX - 1 ? 0 : 1);
  }
  Value = 2;
  const uint64_t Start = NowMs();
  CHECK(write(SharedEventFd, &Value, sizeof(Value)) == 8); // blocks until the child reads
  CHECK(NowMs() - Start >= 40);
  const int Status = WaitExit(Child);
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  close(SharedEventFd);
}

TEST(event, eventfd_poll_states) {
  const int Fd = eventfd(0, EFD_NONBLOCK);
  CHECK(Fd >= 0);
  struct pollfd Entry = {Fd, POLLIN | POLLOUT, 0};
  CHECK(poll(&Entry, 1, 0) == 1 && Entry.revents == POLLOUT);
  uint64_t Value = 1;
  CHECK(write(Fd, &Value, sizeof(Value)) == 8);
  CHECK(poll(&Entry, 1, 0) == 1 && Entry.revents == (POLLIN | POLLOUT));
  Value = UINT64_MAX - 2;
  CHECK(write(Fd, &Value, sizeof(Value)) == 8);
  CHECK(poll(&Entry, 1, 0) == 1 && Entry.revents == POLLIN);
  fd_set Read, Write;
  FD_ZERO(&Read);
  FD_ZERO(&Write);
  FD_SET(Fd, &Read);
  FD_SET(Fd, &Write);
  struct timeval Zero = {0, 0};
  CHECK(select(Fd + 1, &Read, &Write, NULL, &Zero) == 1 && FD_ISSET(Fd, &Read) && !FD_ISSET(Fd, &Write));
  close(Fd);
}

TEST(event, eventfd_fork_shares_counter) {
  const int Fd = eventfd(0, 0);
  CHECK(Fd >= 0);
  const pid_t Child = fork();
  if (!Child) {
    uint64_t Value = 5;
    _exit(write(Fd, &Value, sizeof(Value)) == 8 ? 0 : 1);
  }
  uint64_t Value = 0;
  CHECK(read(Fd, &Value, sizeof(Value)) == 8 && Value == 5);
  const int Status = WaitExit(Child);
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  close(Fd);
}

TEST(event, eventfd_stat_shape) {
  const int Fd = eventfd(0, 0);
  CHECK(Fd >= 0);
  struct stat Info;
  CHECK(fstat(Fd, &Info) == 0);
  CHECK(Info.st_mode == 0600 && Info.st_nlink == 1 && Info.st_size == 0 && Info.st_blksize == 4096);
  CHECK(Info.st_uid == 0 || Info.st_uid == 65534);
  CHECK(lseek(Fd, 3, SEEK_SET) == 0);
  int Count;
  CHECK_ERRNO(ioctl(Fd, FIONREAD, &Count), ENOTTY);
  close(Fd);
}

static int SignalFdFor(int Signal, int Flags) {
  sigset_t Set;
  sigemptyset(&Set);
  sigaddset(&Set, Signal);
  return signalfd(-1, &Set, Flags);
}

TEST(event, signalfd_reads_blocked_signal) {
  CHECK(Block(SIGUSR1) == 0);
  const int Fd = SignalFdFor(SIGUSR1, 0);
  CHECK(Fd >= 0);
  CHECK(kill(getpid(), SIGUSR1) == 0);
  struct signalfd_siginfo Info;
  CHECK(read(Fd, &Info, sizeof(Info)) == sizeof(Info));
  CHECK(Info.ssi_signo == SIGUSR1 && Info.ssi_code == SI_USER);
  CHECK(Info.ssi_pid == (uint32_t)getpid() && Info.ssi_uid == getuid());
  sigset_t Pending;
  CHECK(sigpending(&Pending) == 0 && !sigismember(&Pending, SIGUSR1));
  close(Fd);
}

TEST(event, signalfd_multiple_records) {
  CHECK(Block(SIGRTMIN) == 0);
  CHECK(Block(SIGUSR2) == 0);
  sigset_t Set;
  sigemptyset(&Set);
  sigaddset(&Set, SIGRTMIN);
  sigaddset(&Set, SIGUSR2);
  const int Fd = signalfd(-1, &Set, SFD_NONBLOCK);
  CHECK(Fd >= 0);
  CHECK(sigqueue(getpid(), SIGRTMIN, (union sigval) {.sival_int = 11}) == 0);
  CHECK(sigqueue(getpid(), SIGRTMIN, (union sigval) {.sival_int = 12}) == 0);
  CHECK(kill(getpid(), SIGUSR2) == 0);
  struct signalfd_siginfo Info[4];
  CHECK(read(Fd, Info, sizeof(Info)) == 3 * (long)sizeof(Info[0]));
  CHECK(Info[0].ssi_signo == SIGUSR2);
  CHECK(Info[1].ssi_signo == (uint32_t)SIGRTMIN && Info[1].ssi_code == SI_QUEUE && Info[1].ssi_int == 11);
  CHECK(Info[2].ssi_signo == (uint32_t)SIGRTMIN && Info[2].ssi_int == 12 && Info[2].ssi_ptr == 12);
  CHECK_ERRNO(read(Fd, Info, sizeof(Info)), EAGAIN);
  close(Fd);
}

TEST(event, signalfd_short_buffer_einval) {
  const int Fd = SignalFdFor(SIGUSR1, SFD_NONBLOCK);
  CHECK(Fd >= 0);
  struct signalfd_siginfo Info;
  CHECK_ERRNO(read(Fd, &Info, sizeof(Info) - 1), EINVAL);
  CHECK_ERRNO(read(Fd, &Info, 0), EINVAL);
  sigset_t Set;
  sigemptyset(&Set);
  CHECK_ERRNO(syscall(SYS_signalfd4, -1, &Set, 4, 0), EINVAL);
  CHECK_ERRNO(syscall(SYS_signalfd4, -1, (void *)8, 8, 0), EFAULT);
  CHECK_ERRNO(syscall(SYS_signalfd4, -1, &Set, 8, 1), EINVAL);
  CHECK_ERRNO(syscall(SYS_signalfd4, 999, &Set, 8, 0), EBADF);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK_ERRNO(syscall(SYS_signalfd4, Pipe[0], &Set, 8, 0), EINVAL);
  CHECK(syscall(SYS_signalfd, -1, &Set, 8) >= 0);
  close(Fd);
}

TEST(event, signalfd_update_mask) {
  CHECK(Block(SIGUSR1) == 0);
  CHECK(Block(SIGUSR2) == 0);
  const int Fd = SignalFdFor(SIGUSR1, SFD_NONBLOCK);
  CHECK(Fd >= 0);
  CHECK(kill(getpid(), SIGUSR2) == 0);
  struct signalfd_siginfo Info;
  CHECK_ERRNO(read(Fd, &Info, sizeof(Info)), EAGAIN);
  sigset_t Set;
  sigemptyset(&Set);
  sigaddset(&Set, SIGUSR2);
  CHECK(signalfd(Fd, &Set, 0) == Fd);
  CHECK(read(Fd, &Info, sizeof(Info)) == sizeof(Info) && Info.ssi_signo == SIGUSR2);
  close(Fd);
}

TEST(event, signalfd_nonblock_eagain) {
  const int Fd = SignalFdFor(SIGUSR1, SFD_NONBLOCK | SFD_CLOEXEC);
  CHECK(Fd >= 0);
  CHECK(fcntl(Fd, F_GETFD) == FD_CLOEXEC);
  CHECK(fcntl(Fd, F_GETFL) == (O_RDWR | O_NONBLOCK));
  struct signalfd_siginfo Info;
  CHECK_ERRNO(read(Fd, &Info, sizeof(Info)), EAGAIN);
  close(Fd);
}

TEST(event, signalfd_poll_readable) {
  CHECK(Block(SIGUSR1) == 0);
  const int Fd = SignalFdFor(SIGUSR1, 0);
  CHECK(Fd >= 0);
  struct pollfd Entry = {Fd, POLLIN, 0};
  CHECK(poll(&Entry, 1, 0) == 0);
  const pid_t Parent = getpid();
  const pid_t Child = fork();
  if (!Child) {
    SleepMs(50);
    _exit(kill(Parent, SIGUSR1) == 0 ? 0 : 1);
  }
  CHECK(poll(&Entry, 1, 3000) == 1 && Entry.revents == POLLIN);
  struct signalfd_siginfo Info;
  CHECK(read(Fd, &Info, sizeof(Info)) == sizeof(Info) && Info.ssi_pid == (uint32_t)Child);
  CHECK(poll(&Entry, 1, 0) == 0);
  WaitExit(Child);
  close(Fd);
}

TEST(event, signalfd_ignores_kill_stop_in_mask) {
  sigset_t Set;
  sigfillset(&Set);
  const int Fd = signalfd(-1, &Set, SFD_NONBLOCK);
  CHECK(Fd >= 0);
  // The mask is reported through fdinfo on Linux; here SIGSTOP must still stop.
  const pid_t Child = fork();
  if (!Child) {
    sigset_t All;
    sigfillset(&All);
    sigprocmask(SIG_BLOCK, &All, NULL);
    const int Own = signalfd(-1, &All, 0);
    struct signalfd_siginfo Info;
    for (;;) if (read(Own, &Info, sizeof(Info)) < 0) _exit(2);
  }
  SleepMs(30);
  CHECK(kill(Child, SIGSTOP) == 0);
  int Status = 0;
  CHECK(waitpid(Child, &Status, WUNTRACED) == Child && WIFSTOPPED(Status) && WSTOPSIG(Status) == SIGSTOP);
  CHECK(kill(Child, SIGKILL) == 0);
  CHECK(waitpid(Child, &Status, 0) == Child && WIFSIGNALED(Status) && WTERMSIG(Status) == SIGKILL);
  close(Fd);
}

TEST(event, signalfd_sigchld_from_child) {
  CHECK(Block(SIGCHLD) == 0);
  const int Fd = SignalFdFor(SIGCHLD, 0);
  CHECK(Fd >= 0);
  const pid_t Child = fork();
  if (!Child) _exit(7);
  struct signalfd_siginfo Info;
  CHECK(read(Fd, &Info, sizeof(Info)) == sizeof(Info));
  CHECK(Info.ssi_signo == SIGCHLD && Info.ssi_code == CLD_EXITED);
  CHECK(Info.ssi_pid == (uint32_t)Child && Info.ssi_status == 7);
  WaitExit(Child);
  close(Fd);
}

TEST(event, signalfd_timer_signal) {
  CHECK(Block(SIGALRM) == 0);
  const int Fd = SignalFdFor(SIGALRM, 0);
  CHECK(Fd >= 0);
  int Id = -1;
  CHECK(CreateTimer(CLOCK_MONOTONIC, SIGEV_SIGNAL, SIGALRM, 99, 0, &Id) == 0);
  CHECK(ArmTimer(Id, 0, 10000000, 10000000) == 0);
  SleepMs(55);
  struct signalfd_siginfo Info;
  CHECK(read(Fd, &Info, sizeof(Info)) == sizeof(Info));
  CHECK(Info.ssi_code == SI_TIMER && Info.ssi_tid == (uint32_t)Id && Info.ssi_int == 99 && Info.ssi_overrun >= 2);
  close(Fd);
}

// ---------------------------------------------------------------------------
// Readiness: poll, select and epoll.

TEST(poll, pipe_states) {
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  struct pollfd Entries[2] = {{Pipe[0], POLLIN | POLLOUT | POLLRDNORM, 0}, {Pipe[1], POLLIN | POLLOUT | POLLWRNORM, 0}};
  CHECK(poll(Entries, 2, 0) == 1);
  CHECK(Entries[0].revents == 0 && Entries[1].revents == (POLLOUT | POLLWRNORM));
  CHECK(write(Pipe[1], "x", 1) == 1);
  CHECK(poll(Entries, 2, 0) == 2);
  CHECK(Entries[0].revents == (POLLIN | POLLRDNORM));
  close(Pipe[0]);
  CHECK(poll(&Entries[1], 1, 0) == 1 && Entries[1].revents == (POLLOUT | POLLWRNORM | POLLERR));
  close(Pipe[1]);
}

TEST(poll, negative_fd_ignored) {
  struct pollfd Entry = {-1, POLLIN, 0x7f};
  CHECK(poll(&Entry, 1, 0) == 0 && Entry.revents == 0);
}

TEST(poll, closed_fd_pollnval) {
  struct pollfd Entry = {999, POLLIN, 0};
  CHECK(poll(&Entry, 1, 0) == 1 && Entry.revents == POLLNVAL);
  struct rlimit Limit;
  CHECK(getrlimit(RLIMIT_NOFILE, &Limit) == 0);
  CHECK_ERRNO(syscall(SYS_poll, NULL, Limit.rlim_cur + 1, 0), EINVAL); // above RLIMIT_NOFILE
  CHECK_ERRNO(syscall(SYS_poll, (void *)8, 1, 0), EFAULT);
}

TEST(poll, hup_after_writer_close) {
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK(write(Pipe[1], "x", 1) == 1);
  close(Pipe[1]);
  struct pollfd Entry = {Pipe[0], 0, 0}; // POLLHUP is reported even when not requested
  CHECK(poll(&Entry, 1, 0) == 1 && Entry.revents == POLLHUP);
  Entry.events = POLLIN;
  CHECK(poll(&Entry, 1, 0) == 1 && Entry.revents == (POLLIN | POLLHUP));
  close(Pipe[0]);
}

TEST(poll, poll_timeout_and_wakeup) {
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  struct pollfd Entry = {Pipe[0], POLLIN, 0};
  uint64_t Start = NowMs();
  CHECK(poll(&Entry, 1, 50) == 0);
  CHECK(NowMs() - Start >= 45);
  const pid_t Child = fork();
  if (!Child) {
    SleepMs(50);
    _exit(write(Pipe[1], "x", 1) == 1 ? 0 : 1);
  }
  Start = NowMs();
  CHECK(poll(&Entry, 1, 5000) == 1 && Entry.revents == POLLIN);
  CHECK(NowMs() - Start >= 40);
  WaitExit(Child);
}

TEST(poll, ppoll_sigmask_atomic) {
  ResetRecorder();
  CHECK(Catch(SIGUSR1, 0) == 0);
  CHECK(Block(SIGUSR1) == 0);
  CHECK(raise(SIGUSR1) == 0);
  CHECK(HandlerCalls == 0);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  struct pollfd Entry = {Pipe[0], POLLIN, 0};
  sigset_t Empty;
  sigemptyset(&Empty);
  CHECK_ERRNO(ppoll(&Entry, 1, NULL, &Empty), EINTR);
  CHECK(HandlerCalls == 1);
  sigset_t Now;
  CHECK(sigprocmask(SIG_SETMASK, NULL, &Now) == 0 && sigismember(&Now, SIGUSR1)); // restored
  CHECK_ERRNO(syscall(SYS_ppoll, &Entry, 1, NULL, &Empty, 4), EINVAL);
  CHECK_ERRNO(syscall(SYS_ppoll, &Entry, 1, NULL, (void *)8, 8), EFAULT);
}

TEST(poll, ppoll_mask_restored_when_ready) {
  ResetRecorder();
  CHECK(Catch(SIGUSR1, 0) == 0);
  CHECK(Block(SIGUSR1) == 0);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK(write(Pipe[1], "x", 1) == 1);
  sigset_t Full;
  sigfillset(&Full);
  struct pollfd Entry = {Pipe[0], POLLIN, 0};
  CHECK(ppoll(&Entry, 1, NULL, &Full) == 1);
  sigset_t Now;
  CHECK(sigprocmask(SIG_SETMASK, NULL, &Now) == 0 && sigismember(&Now, SIGUSR1) && !sigismember(&Now, SIGUSR2));
  CHECK(HandlerCalls == 0);
}

TEST(poll, ppoll_writes_remaining) {
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  struct pollfd Entry = {Pipe[0], POLLIN, 0};
  struct timespec Timeout = {0, 50000000};
  CHECK(syscall(SYS_ppoll, &Entry, 1, &Timeout, NULL, 8) == 0);
  CHECK(Timeout.tv_sec == 0 && Timeout.tv_nsec == 0);
  CHECK(write(Pipe[1], "x", 1) == 1);
  Timeout.tv_sec = 5;
  CHECK(syscall(SYS_ppoll, &Entry, 1, &Timeout, NULL, 8) == 1);
  CHECK(Timeout.tv_sec >= 4 && Timeout.tv_sec <= 5);
  Timeout.tv_nsec = 1000000000;
  CHECK_ERRNO(syscall(SYS_ppoll, &Entry, 1, &Timeout, NULL, 8), EINVAL);
}

TEST(poll, select_ebadf) {
  fd_set Read;
  FD_ZERO(&Read);
  close(40);
  FD_SET(40, &Read); // within Linux's smallest descriptor table
  struct timeval Zero = {0, 0};
  CHECK_ERRNO(select(41, &Read, NULL, NULL, &Zero), EBADF);
}

TEST(poll, select_nfds_einval) {
  struct timeval Zero = {0, 0};
  CHECK_ERRNO(syscall(SYS_select, -1, NULL, NULL, NULL, &Zero), EINVAL);
  CHECK(syscall(SYS_select, 5000, NULL, NULL, NULL, &Zero) == 0); // clamped to the table size
  Zero.tv_usec = -1;
  CHECK_ERRNO(syscall(SYS_select, 0, NULL, NULL, NULL, &Zero), EINVAL);
}

TEST(poll, select_writes_remaining) {
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  fd_set Read;
  FD_ZERO(&Read);
  FD_SET(Pipe[0], &Read);
  struct timeval Timeout = {0, 30000};
  const uint64_t Start = NowMs();
  CHECK(select(Pipe[0] + 1, &Read, NULL, NULL, &Timeout) == 0);
  CHECK(NowMs() - Start >= 25);
  CHECK(Timeout.tv_sec == 0 && Timeout.tv_usec == 0 && !FD_ISSET(Pipe[0], &Read));
  CHECK(write(Pipe[1], "x", 1) == 1);
  FD_SET(Pipe[0], &Read);
  Timeout.tv_sec = 3;
  CHECK(select(Pipe[0] + 1, &Read, NULL, NULL, &Timeout) == 1 && FD_ISSET(Pipe[0], &Read));
  CHECK(Timeout.tv_sec >= 2);
}

TEST(poll, pselect6_sigmask) {
  ResetRecorder();
  CHECK(Catch(SIGUSR2, 0) == 0);
  CHECK(Block(SIGUSR2) == 0);
  CHECK(raise(SIGUSR2) == 0);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  fd_set Read;
  FD_ZERO(&Read);
  FD_SET(Pipe[0], &Read);
  sigset_t Empty;
  sigemptyset(&Empty);
  CHECK_ERRNO(pselect(Pipe[0] + 1, &Read, NULL, NULL, NULL, &Empty), EINTR);
  CHECK(HandlerCalls == 1);
  sigset_t Now;
  CHECK(sigprocmask(SIG_SETMASK, NULL, &Now) == 0 && sigismember(&Now, SIGUSR2));
  struct { const sigset_t *Mask; size_t Size; } Data = {&Empty, 4};
  struct timespec Zero = {0, 0};
  CHECK_ERRNO(syscall(SYS_pselect6, Pipe[0] + 1, &Read, NULL, NULL, &Zero, &Data), EINVAL);
}

TEST(poll, regular_file_always_ready) {
  const int Fd = open("poll-regular", O_RDWR | O_CREAT | O_TRUNC, 0600);
  CHECK(Fd >= 0);
  struct pollfd Entry = {Fd, POLLIN | POLLOUT | POLLPRI, 0};
  CHECK(poll(&Entry, 1, -1) == 1 && Entry.revents == (POLLIN | POLLOUT));
  fd_set Read, Write;
  FD_ZERO(&Read);
  FD_ZERO(&Write);
  FD_SET(Fd, &Read);
  FD_SET(Fd, &Write);
  CHECK(select(Fd + 1, &Read, &Write, NULL, NULL) == 2);
  close(Fd);
  unlink("poll-regular");
}

static int EpollAdd(int Epoll, int Fd, uint32_t Events, uint64_t Data) {
  struct epoll_event Event = {.events = Events, .data.u64 = Data};
  return epoll_ctl(Epoll, EPOLL_CTL_ADD, Fd, &Event);
}

TEST(epoll, create_errors) {
  CHECK_ERRNO(epoll_create(0), EINVAL);
  CHECK_ERRNO(epoll_create(-1), EINVAL);
  CHECK_ERRNO(epoll_create1(1), EINVAL);
  const int Epoll = epoll_create1(EPOLL_CLOEXEC);
  CHECK(Epoll >= 0);
  CHECK(fcntl(Epoll, F_GETFD) == FD_CLOEXEC && fcntl(Epoll, F_GETFL) == O_RDWR);
  CHECK(epoll_create(1) >= 0);
  char Buffer[8];
  CHECK_ERRNO(read(Epoll, Buffer, sizeof(Buffer)), EINVAL);
  CHECK_ERRNO(write(Epoll, Buffer, sizeof(Buffer)), EINVAL);
  int Count;
  CHECK_ERRNO(ioctl(Epoll, FIONREAD, &Count), EINVAL);
}

TEST(epoll, ctl_errors) {
  const int Epoll = epoll_create1(0), Other = epoll_create1(0);
  int Pipe[2];
  CHECK(Epoll >= 0 && Other >= 0 && pipe(Pipe) == 0);
  CHECK(EpollAdd(Epoll, Pipe[0], EPOLLIN, 1) == 0);
  CHECK_ERRNO(EpollAdd(Epoll, Pipe[0], EPOLLIN, 1), EEXIST);
  struct epoll_event Event = {.events = EPOLLIN};
  CHECK_ERRNO(epoll_ctl(Epoll, EPOLL_CTL_MOD, Pipe[1], &Event), ENOENT);
  CHECK_ERRNO(epoll_ctl(Epoll, EPOLL_CTL_DEL, Pipe[1], NULL), ENOENT);
  CHECK_ERRNO(epoll_ctl(Epoll, EPOLL_CTL_ADD, Epoll, &Event), EINVAL);
  CHECK_ERRNO(epoll_ctl(Pipe[0], EPOLL_CTL_ADD, Pipe[1], &Event), EINVAL);
  CHECK_ERRNO(epoll_ctl(999, EPOLL_CTL_ADD, Pipe[1], &Event), EBADF);
  CHECK_ERRNO(epoll_ctl(Epoll, EPOLL_CTL_ADD, 999, &Event), EBADF);
  CHECK_ERRNO(epoll_ctl(Epoll, EPOLL_CTL_ADD, Pipe[1], (void *)8), EFAULT);
  CHECK_ERRNO(epoll_ctl(Epoll, 9, Pipe[1], &Event), EINVAL);
  const int File = open("epoll-regular", O_RDWR | O_CREAT | O_TRUNC, 0600);
  CHECK(File >= 0);
  CHECK_ERRNO(epoll_ctl(Epoll, EPOLL_CTL_ADD, File, &Event), EPERM);
  unlink("epoll-regular");
  // EPOLLEXCLUSIVE: not with MOD, not with EPOLLONESHOT, not on an epoll target.
  Event.events = EPOLLIN | EPOLLEXCLUSIVE | EPOLLONESHOT;
  CHECK_ERRNO(epoll_ctl(Epoll, EPOLL_CTL_ADD, Pipe[1], &Event), EINVAL);
  Event.events = EPOLLIN | EPOLLEXCLUSIVE;
  CHECK_ERRNO(epoll_ctl(Epoll, EPOLL_CTL_ADD, Other, &Event), EINVAL);
  CHECK(epoll_ctl(Epoll, EPOLL_CTL_ADD, Pipe[1], &Event) == 0);
  CHECK_ERRNO(epoll_ctl(Epoll, EPOLL_CTL_MOD, Pipe[1], &Event), EINVAL);
  Event.events = EPOLLIN;
  CHECK_ERRNO(epoll_ctl(Epoll, EPOLL_CTL_MOD, Pipe[1], &Event), EINVAL);
  // Nesting: a cycle is ELOOP.
  CHECK(EpollAdd(Other, Epoll, EPOLLIN, 0) == 0);
  CHECK_ERRNO(EpollAdd(Epoll, Other, EPOLLIN, 0), ELOOP);
  CHECK(epoll_ctl(Epoll, EPOLL_CTL_DEL, Pipe[0], NULL) == 0);
}

TEST(epoll, nesting_depth_limit) {
  int Levels[7];
  for (int Index = 0; Index < 7; ++Index) Levels[Index] = epoll_create1(0);
  int Result = 0, Depth = 0;
  for (; Depth < 6; ++Depth) {
    Result = EpollAdd(Levels[Depth + 1], Levels[Depth], EPOLLIN, 0);
    if (Result < 0) break;
  }
  const int Error = errno;
  CHECK(Result < 0 && Error == ELOOP);
  CHECK(Depth == 4); // EP_MAX_NESTS
}

TEST(epoll, level_triggered_repeats) {
  const int Epoll = epoll_create1(0);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK(EpollAdd(Epoll, Pipe[0], EPOLLIN, 7) == 0);
  struct epoll_event Event;
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 0);
  CHECK(write(Pipe[1], "x", 1) == 1);
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 1 && Event.events == EPOLLIN && Event.data.u64 == 7);
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 1);
  char Byte;
  CHECK(read(Pipe[0], &Byte, 1) == 1);
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 0);
}

TEST(epoll, edge_triggered_once) {
  const int Epoll = epoll_create1(0);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK(EpollAdd(Epoll, Pipe[0], EPOLLIN | EPOLLET, 1) == 0);
  CHECK(write(Pipe[1], "x", 1) == 1);
  struct epoll_event Event;
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 1);
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 0);
  CHECK(write(Pipe[1], "y", 1) == 1); // new data is a new edge
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 1);
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 0);
}

TEST(epoll, oneshot_rearm) {
  const int Epoll = epoll_create1(0);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK(write(Pipe[1], "x", 1) == 1);
  CHECK(EpollAdd(Epoll, Pipe[0], EPOLLIN | EPOLLONESHOT, 2) == 0);
  struct epoll_event Event;
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 1 && Event.data.u64 == 2);
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 0);
  CHECK(write(Pipe[1], "y", 1) == 1);
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 0); // disabled until modified
  Event.events = EPOLLIN | EPOLLONESHOT;
  Event.data.u64 = 3;
  CHECK(epoll_ctl(Epoll, EPOLL_CTL_MOD, Pipe[0], &Event) == 0);
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 1 && Event.data.u64 == 3);
}

TEST(epoll, data_roundtrip) {
  const int Epoll = epoll_create1(0);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK(EpollAdd(Epoll, Pipe[1], EPOLLOUT, 0xfedcba9876543210ULL) == 0);
  struct epoll_event Event;
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 1);
  CHECK(Event.events == EPOLLOUT && Event.data.u64 == 0xfedcba9876543210ULL);
}

TEST(epoll, close_dup_keeps_interest) {
  const int Epoll = epoll_create1(0);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK(EpollAdd(Epoll, Pipe[0], EPOLLIN, 5) == 0);
  const int Copy = dup(Pipe[0]);
  CHECK(Copy >= 0);
  close(Pipe[0]);
  CHECK(write(Pipe[1], "x", 1) == 1);
  struct epoll_event Event;
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 1 && Event.data.u64 == 5);
  // The item is keyed on the closed number: DEL through the copy fails.
  CHECK_ERRNO(epoll_ctl(Epoll, EPOLL_CTL_DEL, Copy, NULL), ENOENT);
}

TEST(epoll, close_last_removes) {
  const int Epoll = epoll_create1(0);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK(write(Pipe[1], "x", 1) == 1);
  CHECK(EpollAdd(Epoll, Pipe[0], EPOLLIN, 5) == 0);
  close(Pipe[0]);
  struct epoll_event Event;
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 0);
  int Again[2];
  CHECK(pipe(Again) == 0);
  CHECK(EpollAdd(Epoll, Again[0], EPOLLIN, 6) == 0); // the reused number is free
}

TEST(epoll, nested_epoll_readable) {
  const int Inner = epoll_create1(0), Outer = epoll_create1(0);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK(EpollAdd(Inner, Pipe[0], EPOLLIN, 1) == 0);
  CHECK(EpollAdd(Outer, Inner, EPOLLIN, 2) == 0);
  struct epoll_event Event;
  CHECK(epoll_wait(Outer, &Event, 1, 0) == 0);
  struct pollfd Entry = {Inner, POLLIN, 0};
  CHECK(poll(&Entry, 1, 0) == 0);
  CHECK(write(Pipe[1], "x", 1) == 1);
  CHECK(epoll_wait(Outer, &Event, 1, 0) == 1 && Event.data.u64 == 2 && Event.events == EPOLLIN);
  CHECK(poll(&Entry, 1, 0) == 1 && Entry.revents == POLLIN);
}

TEST(epoll, pwait_sigmask) {
  ResetRecorder();
  CHECK(Catch(SIGUSR1, 0) == 0);
  CHECK(Block(SIGUSR1) == 0);
  CHECK(raise(SIGUSR1) == 0);
  const int Epoll = epoll_create1(0);
  struct epoll_event Event;
  sigset_t Empty;
  sigemptyset(&Empty);
  CHECK_ERRNO(epoll_pwait(Epoll, &Event, 1, -1, &Empty), EINTR);
  CHECK(HandlerCalls == 1);
  sigset_t Now;
  CHECK(sigprocmask(SIG_SETMASK, NULL, &Now) == 0 && sigismember(&Now, SIGUSR1));
  CHECK_ERRNO(syscall(SYS_epoll_pwait, Epoll, &Event, 1, 0, &Empty, 4), EINVAL);
}

TEST(epoll, pwait2_timespec) {
  const int Epoll = epoll_create1(0);
  struct epoll_event Event;
  struct timespec Timeout = {0, 30000000};
  const uint64_t Start = NowMs();
  CHECK(syscall(SYS_epoll_pwait2, Epoll, &Event, 1, &Timeout, NULL, 8) == 0);
  CHECK(NowMs() - Start >= 25);
  Timeout.tv_nsec = -1;
  CHECK_ERRNO(syscall(SYS_epoll_pwait2, Epoll, &Event, 1, &Timeout, NULL, 8), EINVAL);
  CHECK_ERRNO(syscall(SYS_epoll_pwait2, Epoll, &Event, 1, (void *)8, NULL, 8), EFAULT);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK(write(Pipe[1], "x", 1) == 1);
  CHECK(EpollAdd(Epoll, Pipe[0], EPOLLIN, 1) == 0);
  CHECK(syscall(SYS_epoll_pwait2, Epoll, &Event, 1, NULL, NULL, 8) == 1); // NULL waits forever
}

TEST(epoll, wait_timeout_zero) {
  const int Epoll = epoll_create1(0);
  struct epoll_event Event;
  const uint64_t Start = NowMs();
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 0);
  CHECK(epoll_wait(Epoll, &Event, 1, 40) == 0);
  CHECK(NowMs() - Start >= 35);
}

TEST(epoll, hup_err_always) {
  const int Epoll = epoll_create1(0);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK(EpollAdd(Epoll, Pipe[0], 0, 1) == 0);
  CHECK(EpollAdd(Epoll, Pipe[1], 0, 2) == 0);
  close(Pipe[1]);
  struct epoll_event Event[2];
  CHECK(epoll_wait(Epoll, Event, 2, 0) == 1 && Event[0].data.u64 == 1 && Event[0].events == EPOLLHUP);
  int Second[2];
  CHECK(pipe(Second) == 0);
  CHECK(EpollAdd(Epoll, Second[1], 0, 3) == 0);
  close(Second[0]);
  CHECK(epoll_wait(Epoll, Event, 2, 0) == 2);
  CHECK((Event[0].data.u64 == 3 && Event[0].events == EPOLLERR) || (Event[1].data.u64 == 3 && Event[1].events == EPOLLERR));
}

TEST(epoll, eventfd_timerfd_signalfd_sources) {
  CHECK(Block(SIGUSR1) == 0);
  const int Epoll = epoll_create1(0);
  const int Event = eventfd(0, EFD_NONBLOCK), Timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  const int Signals = SignalFdFor(SIGUSR1, SFD_NONBLOCK);
  CHECK(Event >= 0 && Timer >= 0 && Signals >= 0);
  CHECK(EpollAdd(Epoll, Event, EPOLLIN, 1) == 0);
  CHECK(EpollAdd(Epoll, Timer, EPOLLIN, 2) == 0);
  CHECK(EpollAdd(Epoll, Signals, EPOLLIN, 3) == 0);
  struct epoll_event Ready[3];
  CHECK(epoll_wait(Epoll, Ready, 3, 0) == 0);
  struct itimerspec Spec = {{0, 0}, {0, 30000000}};
  CHECK(timerfd_settime(Timer, 0, &Spec, NULL) == 0);
  CHECK(epoll_wait(Epoll, Ready, 3, 2000) == 1 && Ready[0].data.u64 == 2);
  uint64_t Value = 1;
  CHECK(write(Event, &Value, sizeof(Value)) == 8);
  CHECK(kill(getpid(), SIGUSR1) == 0);
  CHECK(epoll_wait(Epoll, Ready, 3, 0) == 3);
  unsigned Seen = 0;
  for (int Index = 0; Index < 3; ++Index) Seen |= 1u << Ready[Index].data.u64;
  CHECK(Seen == 0xe);
}

TEST(epoll, wakes_on_pipe_write_from_child) {
  const int Epoll = epoll_create1(0);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK(EpollAdd(Epoll, Pipe[0], EPOLLIN, 9) == 0);
  const pid_t Child = fork();
  if (!Child) {
    SleepMs(50);
    _exit(write(Pipe[1], "x", 1) == 1 ? 0 : 1);
  }
  struct epoll_event Event;
  const uint64_t Start = NowMs();
  CHECK(epoll_wait(Epoll, &Event, 1, 5000) == 1 && Event.data.u64 == 9);
  CHECK(NowMs() - Start >= 40);
  WaitExit(Child);
}

TEST(epoll, fork_shares_instance) {
  const int Epoll = epoll_create1(0);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  const pid_t Child = fork();
  if (!Child) _exit(EpollAdd(Epoll, Pipe[0], EPOLLIN, 4) == 0 ? 0 : 1);
  const int Status = WaitExit(Child);
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  CHECK(write(Pipe[1], "x", 1) == 1);
  struct epoll_event Event;
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 1 && Event.data.u64 == 4);
}

TEST(epoll, maxevents_einval) {
  const int Epoll = epoll_create1(0);
  struct epoll_event Event;
  CHECK_ERRNO(epoll_wait(Epoll, &Event, 0, 0), EINVAL);
  CHECK_ERRNO(syscall(SYS_epoll_wait, Epoll, &Event, -1, 0), EINVAL);
  CHECK_ERRNO(syscall(SYS_epoll_wait, Epoll, &Event, INT_MAX / 12 + 1, 0), EINVAL);
  CHECK_ERRNO(epoll_wait(999, &Event, 1, 0), EBADF);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK_ERRNO(epoll_wait(Pipe[0], &Event, 1, 0), EINVAL);
}

TEST(epoll, round_robin_with_small_max) {
  const int Epoll = epoll_create1(0);
  int First[2], Second[2];
  CHECK(pipe(First) == 0 && pipe(Second) == 0);
  CHECK(write(First[1], "x", 1) == 1 && write(Second[1], "y", 1) == 1);
  CHECK(EpollAdd(Epoll, First[0], EPOLLIN, 1) == 0);
  CHECK(EpollAdd(Epoll, Second[0], EPOLLIN, 2) == 0);
  struct epoll_event A, B;
  CHECK(epoll_wait(Epoll, &A, 1, 0) == 1 && epoll_wait(Epoll, &B, 1, 0) == 1);
  CHECK(A.data.u64 != B.data.u64);
}

// ---------------------------------------------------------------------------
// inotify.

struct Notified {
  int Wd;
  uint32_t Mask, Cookie;
  char Name[64];
};

// Reads every queued event without blocking.
static int Drain(int Fd, struct Notified *Out, int Max) {
  char Buffer[4096] __attribute__((aligned(8)));
  int Count = 0;
  for (;;) {
    struct pollfd Entry = {Fd, POLLIN, 0};
    if (poll(&Entry, 1, 0) != 1) break;
    const ssize_t Length = read(Fd, Buffer, sizeof(Buffer));
    if (Length <= 0) break;
    for (ssize_t Offset = 0; Offset < Length;) {
      const struct inotify_event *Event = (const struct inotify_event *)(Buffer + Offset);
      if (Count < Max) {
        Out[Count].Wd = Event->wd;
        Out[Count].Mask = Event->mask;
        Out[Count].Cookie = Event->cookie;
        snprintf(Out[Count].Name, sizeof(Out[Count].Name), "%s", Event->len ? Event->name : "");
      }
      ++Count;
      Offset += sizeof(*Event) + Event->len;
    }
  }
  return Count;
}

static int Same(const struct Notified *Event, int Wd, uint32_t Mask, const char *Name) {
  return Event->Wd == Wd && Event->Mask == Mask && !strcmp(Event->Name, Name);
}

static void FreshDirectory(const char *Path) {
  char Command[256];
  snprintf(Command, sizeof(Command), "rm -rf %s", Path);
  if (system(Command)) {}
  mkdir(Path, 0700);
}

TEST(inotify, init_flags_and_shape) {
  CHECK_ERRNO(inotify_init1(1), EINVAL);
  const int Fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  CHECK(Fd >= 0);
  CHECK(fcntl(Fd, F_GETFD) == FD_CLOEXEC && fcntl(Fd, F_GETFL) == (O_RDONLY | O_NONBLOCK));
  char Buffer[64];
  CHECK_ERRNO(read(Fd, Buffer, sizeof(Buffer)), EAGAIN);
  CHECK_ERRNO(write(Fd, Buffer, 1), EBADF);
  int Pending = -1;
  CHECK(ioctl(Fd, FIONREAD, &Pending) == 0 && Pending == 0);
  CHECK(inotify_init() >= 0);
}

TEST(inotify, add_watch_errors) {
  FreshDirectory("ino-errors");
  const int Fd = inotify_init1(IN_NONBLOCK);
  CHECK_ERRNO(inotify_add_watch(Fd, "ino-errors", 0), EINVAL);
  CHECK_ERRNO(inotify_add_watch(Fd, "ino-errors", IN_CREATE | IN_MASK_ADD | IN_MASK_CREATE), EINVAL);
  CHECK_ERRNO(inotify_add_watch(Fd, "ino-errors/missing", IN_CREATE), ENOENT);
  CHECK_ERRNO(inotify_add_watch(999, "ino-errors", IN_CREATE), EBADF);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK_ERRNO(inotify_add_watch(Pipe[0], "ino-errors", IN_CREATE), EINVAL);
  const int File = open("ino-errors/file", O_CREAT | O_WRONLY, 0600);
  CHECK(File >= 0);
  CHECK_ERRNO(inotify_add_watch(Fd, "ino-errors/file", IN_ONLYDIR | IN_MODIFY), ENOTDIR);
  CHECK_ERRNO(inotify_rm_watch(Fd, 12345), EINVAL);
  CHECK_ERRNO(inotify_rm_watch(Pipe[0], 1), EINVAL);
}

TEST(inotify, same_inode_same_wd) {
  FreshDirectory("ino-wd");
  const int Fd = inotify_init1(IN_NONBLOCK);
  const int First = inotify_add_watch(Fd, "ino-wd", IN_CREATE);
  CHECK(First >= 1);
  CHECK(inotify_add_watch(Fd, "./ino-wd/", IN_DELETE) == First);
  CHECK_ERRNO(inotify_add_watch(Fd, "ino-wd", IN_CREATE | IN_MASK_CREATE), EEXIST);
  // The mask was replaced by IN_DELETE: a create is not reported.
  close(open("ino-wd/a", O_CREAT | O_WRONLY, 0600));
  struct Notified Events[8];
  CHECK(Drain(Fd, Events, 8) == 0);
  CHECK(inotify_add_watch(Fd, "ino-wd", IN_CREATE | IN_MASK_ADD) == First);
  close(open("ino-wd/b", O_CREAT | O_WRONLY, 0600));
  CHECK(Drain(Fd, Events, 8) == 1 && Same(&Events[0], First, IN_CREATE, "b"));
  const int Second = inotify_add_watch(Fd, "ino-wd/a", IN_MODIFY);
  CHECK(Second == First + 1);
}

TEST(inotify, create_write_close_sequence) {
  FreshDirectory("ino-seq");
  const int Fd = inotify_init1(IN_NONBLOCK);
  const int Wd = inotify_add_watch(Fd, "ino-seq", IN_ALL_EVENTS);
  CHECK(Wd >= 0);
  const int File = open("ino-seq/f", O_CREAT | O_WRONLY, 0600);
  CHECK(File >= 0 && write(File, "abc", 3) == 3);
  close(File);
  struct Notified Events[8];
  CHECK(Drain(Fd, Events, 8) == 4);
  CHECK(Same(&Events[0], Wd, IN_CREATE, "f"));
  CHECK(Same(&Events[1], Wd, IN_OPEN, "f"));
  CHECK(Same(&Events[2], Wd, IN_MODIFY, "f"));
  CHECK(Same(&Events[3], Wd, IN_CLOSE_WRITE, "f"));
}

TEST(inotify, read_access_close_nowrite) {
  FreshDirectory("ino-read");
  close(open("ino-read/f", O_CREAT | O_WRONLY, 0600));
  int File = open("ino-read/f", O_WRONLY);
  CHECK(write(File, "x", 1) == 1);
  close(File);
  const int Fd = inotify_init1(IN_NONBLOCK);
  const int Wd = inotify_add_watch(Fd, "ino-read/f", IN_ALL_EVENTS);
  File = open("ino-read/f", O_RDONLY);
  char Byte;
  CHECK(read(File, &Byte, 1) == 1);
  CHECK(read(File, &Byte, 1) == 0); // EOF is not an access
  close(File);
  struct Notified Events[8];
  CHECK(Drain(Fd, Events, 8) == 3);
  CHECK(Same(&Events[0], Wd, IN_OPEN, ""));
  CHECK(Same(&Events[1], Wd, IN_ACCESS, ""));
  CHECK(Same(&Events[2], Wd, IN_CLOSE_NOWRITE, ""));
}

TEST(inotify, directory_events_isdir) {
  FreshDirectory("ino-dir");
  const int Fd = inotify_init1(IN_NONBLOCK);
  const int Wd = inotify_add_watch(Fd, "ino-dir", IN_CREATE | IN_DELETE | IN_OPEN | IN_CLOSE_NOWRITE);
  CHECK(mkdir("ino-dir/sub", 0700) == 0);
  CHECK(rmdir("ino-dir/sub") == 0);
  const int Dir = open("ino-dir", O_RDONLY | O_DIRECTORY);
  close(Dir);
  struct Notified Events[8];
  CHECK(Drain(Fd, Events, 8) == 4);
  CHECK(Same(&Events[0], Wd, IN_CREATE | IN_ISDIR, "sub"));
  CHECK(Same(&Events[1], Wd, IN_DELETE | IN_ISDIR, "sub"));
  CHECK(Same(&Events[2], Wd, IN_OPEN | IN_ISDIR, ""));
  CHECK(Same(&Events[3], Wd, IN_CLOSE_NOWRITE | IN_ISDIR, ""));
}

TEST(inotify, attrib_on_chmod_and_utimes) {
  FreshDirectory("ino-attr");
  close(open("ino-attr/f", O_CREAT | O_WRONLY, 0600));
  const int Fd = inotify_init1(IN_NONBLOCK);
  const int Dir = inotify_add_watch(Fd, "ino-attr", IN_ATTRIB);
  const int File = inotify_add_watch(Fd, "ino-attr/f", IN_ATTRIB);
  CHECK(chmod("ino-attr/f", 0640) == 0);
  CHECK(utimensat(AT_FDCWD, "ino-attr/f", NULL, 0) == 0);
  struct Notified Events[8];
  CHECK(Drain(Fd, Events, 8) == 4);
  // The directory's watch comes first.
  CHECK(Same(&Events[0], Dir, IN_ATTRIB, "f") && Same(&Events[1], File, IN_ATTRIB, ""));
  CHECK(Same(&Events[2], Dir, IN_ATTRIB, "f") && Same(&Events[3], File, IN_ATTRIB, ""));
}

TEST(inotify, rename_cookie_pairs) {
  FreshDirectory("ino-mv");
  mkdir("ino-mv/a", 0700);
  mkdir("ino-mv/b", 0700);
  close(open("ino-mv/a/f", O_CREAT | O_WRONLY, 0600));
  const int Fd = inotify_init1(IN_NONBLOCK);
  const int From = inotify_add_watch(Fd, "ino-mv/a", IN_MOVE);
  const int To = inotify_add_watch(Fd, "ino-mv/b", IN_MOVE);
  const int Self = inotify_add_watch(Fd, "ino-mv/a/f", IN_MOVE_SELF);
  CHECK(rename("ino-mv/a/f", "ino-mv/b/g") == 0);
  struct Notified Events[8];
  CHECK(Drain(Fd, Events, 8) == 3);
  CHECK(Same(&Events[0], From, IN_MOVED_FROM, "f"));
  CHECK(Same(&Events[1], To, IN_MOVED_TO, "g"));
  CHECK(Events[0].Cookie && Events[0].Cookie == Events[1].Cookie);
  CHECK(Same(&Events[2], Self, IN_MOVE_SELF, ""));
  CHECK(rename("ino-mv/b/g", "ino-mv/b/h") == 0);
  CHECK(Drain(Fd, Events, 8) == 3);
  CHECK(Events[0].Cookie == Events[1].Cookie && Events[0].Cookie != 0);
}

TEST(inotify, delete_self_then_ignored) {
  FreshDirectory("ino-del");
  close(open("ino-del/f", O_CREAT | O_WRONLY, 0600));
  const int Fd = inotify_init1(IN_NONBLOCK);
  const int Dir = inotify_add_watch(Fd, "ino-del", IN_DELETE);
  const int File = inotify_add_watch(Fd, "ino-del/f", IN_DELETE_SELF | IN_ATTRIB);
  CHECK(unlink("ino-del/f") == 0);
  struct Notified Events[8];
  CHECK(Drain(Fd, Events, 8) == 4);
  CHECK(Same(&Events[0], File, IN_ATTRIB, ""));
  CHECK(Same(&Events[1], File, IN_DELETE_SELF, ""));
  CHECK(Same(&Events[2], File, IN_IGNORED, ""));
  CHECK(Same(&Events[3], Dir, IN_DELETE, "f"));
  CHECK_ERRNO(inotify_rm_watch(Fd, File), EINVAL);
}

TEST(inotify, unlinked_open_file_deleted_at_close) {
  FreshDirectory("ino-open");
  const int File = open("ino-open/f", O_CREAT | O_RDWR, 0600);
  const int Fd = inotify_init1(IN_NONBLOCK);
  const int Wd = inotify_add_watch(Fd, "ino-open/f", IN_DELETE_SELF | IN_CLOSE_WRITE);
  CHECK(unlink("ino-open/f") == 0);
  struct Notified Events[8];
  CHECK(Drain(Fd, Events, 8) == 0);
  close(File);
  CHECK(Drain(Fd, Events, 8) == 3);
  CHECK(Same(&Events[0], Wd, IN_CLOSE_WRITE, ""));
  CHECK(Same(&Events[1], Wd, IN_DELETE_SELF, ""));
  CHECK(Same(&Events[2], Wd, IN_IGNORED, ""));
}

TEST(inotify, rm_watch_ignored) {
  FreshDirectory("ino-rm");
  const int Fd = inotify_init1(IN_NONBLOCK);
  const int Wd = inotify_add_watch(Fd, "ino-rm", IN_CREATE);
  CHECK(inotify_rm_watch(Fd, Wd) == 0);
  close(open("ino-rm/f", O_CREAT | O_WRONLY, 0600));
  struct Notified Events[8];
  CHECK(Drain(Fd, Events, 8) == 1 && Same(&Events[0], Wd, IN_IGNORED, ""));
}

TEST(inotify, oneshot) {
  FreshDirectory("ino-once");
  const int Fd = inotify_init1(IN_NONBLOCK);
  const int Wd = inotify_add_watch(Fd, "ino-once", IN_CREATE | IN_ONESHOT);
  close(open("ino-once/a", O_CREAT | O_WRONLY, 0600));
  close(open("ino-once/b", O_CREAT | O_WRONLY, 0600));
  struct Notified Events[8];
  CHECK(Drain(Fd, Events, 8) == 2);
  CHECK(Same(&Events[0], Wd, IN_CREATE, "a") && Same(&Events[1], Wd, IN_IGNORED, ""));
}

TEST(inotify, identical_events_merge) {
  FreshDirectory("ino-merge");
  const int File = open("ino-merge/f", O_CREAT | O_WRONLY, 0600);
  const int Fd = inotify_init1(IN_NONBLOCK);
  const int Wd = inotify_add_watch(Fd, "ino-merge/f", IN_MODIFY);
  for (int Index = 0; Index < 5; ++Index) CHECK(write(File, "x", 1) == 1);
  struct Notified Events[8];
  CHECK(Drain(Fd, Events, 8) == 1 && Same(&Events[0], Wd, IN_MODIFY, ""));
  close(File);
}

TEST(inotify, read_buffer_rules) {
  FreshDirectory("ino-buf");
  const int Fd = inotify_init1(IN_NONBLOCK);
  inotify_add_watch(Fd, "ino-buf", IN_CREATE);
  close(open("ino-buf/name", O_CREAT | O_WRONLY, 0600));
  close(open("ino-buf/other", O_CREAT | O_WRONLY, 0600));
  int Pending = 0;
  CHECK(ioctl(Fd, FIONREAD, &Pending) == 0 && Pending == 64); // two 16+16 byte records
  char Buffer[64] __attribute__((aligned(8)));
  CHECK_ERRNO(read(Fd, Buffer, 16), EINVAL);
  CHECK(read(Fd, Buffer, 40) == 32); // whole events only
  const struct inotify_event *Event = (const void *)Buffer;
  CHECK(Event->len == 16 && !strcmp(Event->name, "name"));
  CHECK(read(Fd, Buffer, sizeof(Buffer)) == 32);
}

TEST(inotify, blocking_read_wakes) {
  FreshDirectory("ino-block");
  const int Fd = inotify_init();
  const int Wd = inotify_add_watch(Fd, "ino-block", IN_CREATE);
  const pid_t Child = fork();
  if (!Child) {
    SleepMs(50);
    close(open("ino-block/late", O_CREAT | O_WRONLY, 0600));
    _exit(0);
  }
  char Buffer[64] __attribute__((aligned(8)));
  CHECK(read(Fd, Buffer, sizeof(Buffer)) == 32);
  const struct inotify_event *Event = (const void *)Buffer;
  CHECK(Event->wd == Wd && Event->mask == IN_CREATE && !strcmp(Event->name, "late"));
  WaitExit(Child);
}

TEST(inotify, poll_and_epoll_readable) {
  FreshDirectory("ino-poll");
  const int Fd = inotify_init1(IN_NONBLOCK);
  inotify_add_watch(Fd, "ino-poll", IN_CREATE);
  const int Epoll = epoll_create1(0);
  struct epoll_event Event = {.events = EPOLLIN | EPOLLET, .data.u64 = 1};
  CHECK(epoll_ctl(Epoll, EPOLL_CTL_ADD, Fd, &Event) == 0);
  struct pollfd Entry = {Fd, POLLIN, 0};
  CHECK(poll(&Entry, 1, 0) == 0 && epoll_wait(Epoll, &Event, 1, 0) == 0);
  close(open("ino-poll/f", O_CREAT | O_WRONLY, 0600));
  CHECK(poll(&Entry, 1, 0) == 1 && Entry.revents == POLLIN);
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 1);
  CHECK(epoll_wait(Epoll, &Event, 1, 0) == 0);
}

TEST(inotify, link_and_symlink) {
  FreshDirectory("ino-link");
  close(open("ino-link/f", O_CREAT | O_WRONLY, 0600));
  const int Fd = inotify_init1(IN_NONBLOCK);
  const int Dir = inotify_add_watch(Fd, "ino-link", IN_CREATE);
  const int File = inotify_add_watch(Fd, "ino-link/f", IN_ATTRIB);
  CHECK(link("ino-link/f", "ino-link/g") == 0);
  CHECK(symlink("f", "ino-link/s") == 0);
  struct Notified Events[8];
  CHECK(Drain(Fd, Events, 8) == 3);
  CHECK(Same(&Events[0], File, IN_ATTRIB, ""));
  CHECK(Same(&Events[1], Dir, IN_CREATE, "g"));
  CHECK(Same(&Events[2], Dir, IN_CREATE, "s"));
  // IN_DONT_FOLLOW watches the link itself.
  const int Link = inotify_add_watch(Fd, "ino-link/s", IN_ATTRIB | IN_DONT_FOLLOW);
  CHECK(Link != File && Link > 0);
  CHECK(inotify_add_watch(Fd, "ino-link/s", IN_ATTRIB) == File);
}

TEST(inotify, fanotify_unavailable) {
  const long Result = syscall(SYS_fanotify_init, 0, O_RDONLY);
  const int Error = errno;
  CHECK(Result < 0 && (Error == EPERM || Error == ENOSYS));
}

// ---------------------------------------------------------------------------
// Resource limits.

TEST(proc, rlimit_roundtrip) {
  struct rlimit Limit, Again;
  CHECK(getrlimit(RLIMIT_NOFILE, &Limit) == 0 && Limit.rlim_cur <= Limit.rlim_max);
  struct rlimit Lower = {64, Limit.rlim_max};
  CHECK(setrlimit(RLIMIT_NOFILE, &Lower) == 0);
  CHECK(getrlimit(RLIMIT_NOFILE, &Again) == 0 && Again.rlim_cur == 64 && Again.rlim_max == Limit.rlim_max);
  CHECK(setrlimit(RLIMIT_NOFILE, &Limit) == 0); // the soft limit may rise to the hard one
  struct rlimit Old;
  CHECK(prlimit(0, RLIMIT_NOFILE, &Lower, &Old) == 0 && Old.rlim_cur == Limit.rlim_cur);
  CHECK(syscall(SYS_prlimit64, getpid(), RLIMIT_NOFILE, NULL, &Old) == 0 && Old.rlim_cur == 64);
  CHECK(getrlimit(RLIMIT_CORE, &Again) == 0 && getrlimit(RLIMIT_STACK, &Again) == 0 && Again.rlim_cur > 0);
  CHECK(getrlimit(RLIMIT_SIGPENDING, &Again) == 0 && Again.rlim_cur > 0);
  CHECK(getrlimit(RLIMIT_NPROC, &Again) == 0 && Again.rlim_cur > 0);
}

TEST(proc, rlimit_soft_above_hard_einval) {
  struct rlimit Limit = {100, 50};
  CHECK_ERRNO(setrlimit(RLIMIT_NOFILE, &Limit), EINVAL);
  CHECK_ERRNO(syscall(SYS_getrlimit, 16, &Limit), EINVAL);
  CHECK_ERRNO(syscall(SYS_setrlimit, 16, &Limit), EINVAL);
  CHECK_ERRNO(syscall(SYS_prlimit64, 0, 16, NULL, &Limit), EINVAL);
  CHECK_ERRNO(syscall(SYS_setrlimit, RLIMIT_NOFILE, (void *)8), EFAULT);
  CHECK_ERRNO(syscall(SYS_getrlimit, RLIMIT_NOFILE, (void *)8), EFAULT);
}

static void RaiseHardChild(void) {
  struct rlimit Limit;
  if (getrlimit(RLIMIT_CORE, &Limit)) _exit(1);
  struct rlimit Lower = {0, 4096};
  if (setrlimit(RLIMIT_CORE, &Lower)) _exit(2);
  Lower.rlim_max = 8192;
  const int Result = setrlimit(RLIMIT_CORE, &Lower);
  if (Result == 0) _exit(geteuid() == 0 ? 0 : 3); // only CAP_SYS_RESOURCE raises the hard limit
  _exit(errno == EPERM ? 0 : 4);
}

TEST(proc, rlimit_raise_hard_eperm) {
  const int Status = WaitExit(Fork(RaiseHardChild));
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
}

static void NofileChild(void) {
  struct rlimit Limit;
  if (getrlimit(RLIMIT_NOFILE, &Limit)) _exit(1);
  Limit.rlim_cur = 16;
  if (setrlimit(RLIMIT_NOFILE, &Limit)) _exit(2);
  int Last = -1;
  for (;;) {
    const int Fd = open("/dev/null", O_RDONLY);
    if (Fd < 0) {
      if (errno != EMFILE) _exit(3);
      break;
    }
    Last = Fd;
  }
  if (Last != 15) _exit(4);
  if (dup(0) != -1 || errno != EMFILE) _exit(5);
  if (dup2(0, 16) != -1 || errno != EBADF) _exit(6);
  if (fcntl(0, F_DUPFD, 16) != -1 || errno != EINVAL) _exit(7);
  int Pipe[2];
  if (pipe(Pipe) != -1 || errno != EMFILE) _exit(8);
  if (socket(AF_UNIX, SOCK_STREAM, 0) != -1 || errno != EMFILE) _exit(9);
  struct pollfd Entries[17] = {{0}};
  for (int Index = 0; Index < 17; ++Index) Entries[Index].fd = -1;
  if (syscall(SYS_poll, Entries, 17, 0) != -1 || errno != EINVAL) _exit(10);
  if (syscall(SYS_poll, Entries, 16, 0) != 0) _exit(11);
  close(10);
  if (open("/dev/null", O_RDONLY) != 10) _exit(12);
  _exit(0);
}

TEST(proc, rlimit_nofile_emfile) {
  const int Status = WaitExit(Fork(NofileChild));
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
}

static void InheritChild(void) {
  struct rlimit Limit;
  if (getrlimit(RLIMIT_NOFILE, &Limit)) _exit(1);
  Limit.rlim_cur = 77;
  if (setrlimit(RLIMIT_NOFILE, &Limit)) _exit(2);
  const pid_t Grandchild = fork();
  if (!Grandchild) {
    struct rlimit Seen;
    _exit(getrlimit(RLIMIT_NOFILE, &Seen) == 0 && Seen.rlim_cur == 77 ? 0 : 1);
  }
  int Status;
  if (waitpid(Grandchild, &Status, 0) != Grandchild || !WIFEXITED(Status) || WEXITSTATUS(Status)) _exit(3);
  execl(ExePath, ExePath, "--nofile-soft", (char *)NULL);
  _exit(4);
}

TEST(proc, rlimit_inherited_by_fork_and_exec) {
  const int Status = WaitExit(Fork(InheritChild));
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 77);
}

TEST(proc, prlimit_other_pid) {
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  const pid_t Child = fork();
  if (!Child) {
    char Byte;
    close(Pipe[1]);
    if (read(Pipe[0], &Byte, 1) != 1) _exit(1);
    struct rlimit Seen;
    _exit(getrlimit(RLIMIT_NOFILE, &Seen) == 0 && Seen.rlim_cur == 33 ? 0 : 2);
  }
  close(Pipe[0]);
  struct rlimit Old, New;
  CHECK(prlimit(Child, RLIMIT_NOFILE, NULL, &Old) == 0);
  New.rlim_cur = 33;
  New.rlim_max = Old.rlim_max;
  CHECK(prlimit(Child, RLIMIT_NOFILE, &New, &Old) == 0);
  CHECK(write(Pipe[1], "x", 1) == 1);
  const int Status = WaitExit(Child);
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  CHECK_ERRNO(prlimit(Child, RLIMIT_NOFILE, NULL, &Old), ESRCH); // reaped
  CHECK_ERRNO(prlimit(Child, RLIMIT_NOFILE, (void *)8, &Old), EFAULT); // the new value is read first
}

static volatile sig_atomic_t CpuSignals;
static void CountCpu(int Signal) {
  (void)Signal;
  ++CpuSignals;
}

static void CpuChild(int Report) {
  signal(SIGXCPU, CountCpu);
  struct rlimit Limit = {1, 2};
  if (setrlimit(RLIMIT_CPU, &Limit)) _exit(1);
  int Reported = 0;
  for (;;) {
    if (CpuSignals && !Reported) {
      struct rlimit Now;
      // The soft limit moved up a second when SIGXCPU was sent.
      const char Byte = getrlimit(RLIMIT_CPU, &Now) == 0 && Now.rlim_cur == 2 ? 'y' : 'n';
      if (write(Report, &Byte, 1) != 1) _exit(2);
      Reported = 1;
    }
  }
}

TEST(proc, rlimit_cpu_sigxcpu) {
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  const pid_t Child = fork();
  if (!Child) {
    close(Pipe[0]);
    CpuChild(Pipe[1]);
  }
  close(Pipe[1]);
  char Byte = 0;
  CHECK(read(Pipe[0], &Byte, 1) == 1 && Byte == 'y');
  const int Status = WaitExit(Child);
  CHECK(WIFSIGNALED(Status) && WTERMSIG(Status) == SIGKILL); // the hard limit
}

// The anonymous inode names of /proc/<pid>/fd links. The guest has no
// /proc/<pid>/fd until the procfs milestone; the case skips there.
TEST(event, anon_inode_names) {
  struct stat Directory;
  if (stat("/proc/self/fd", &Directory) != 0 || !S_ISDIR(Directory.st_mode)) SKIP("needs /proc/<pid>/fd (procfs milestone)");
  sigset_t Mask;
  sigemptyset(&Mask);
  sigaddset(&Mask, SIGUSR1);
  const struct {
    int Fd;
    const char *Name;
  } Cases[] = {
    {timerfd_create(CLOCK_MONOTONIC, 0), "anon_inode:[timerfd]"},
    {eventfd(0, 0), "anon_inode:[eventfd]"},
    {signalfd(-1, &Mask, 0), "anon_inode:[signalfd]"},
    {epoll_create1(0), "anon_inode:[eventpoll]"},
    {inotify_init(), "anon_inode:inotify"},
  };
  for (size_t Index = 0; Index < sizeof(Cases) / sizeof(Cases[0]); ++Index) {
    char Link[64], Target[64] = {0};
    CHECK(Cases[Index].Fd >= 0);
    snprintf(Link, sizeof(Link), "/proc/self/fd/%d", Cases[Index].Fd);
    CHECK(readlink(Link, Target, sizeof(Target) - 1) > 0 && !strcmp(Target, Cases[Index].Name));
  }
}

// The character devices of /dev and the /proc/sys files a container tool reads at start (bwrap reads overflowuid).
TEST(dev, char_devices) {
  const struct {
    const char *Path;
    unsigned Major, Minor;
  } Devices[] = {{"/dev/null", 1, 3}, {"/dev/zero", 1, 5}, {"/dev/full", 1, 7}, {"/dev/random", 1, 8}, {"/dev/urandom", 1, 9}, {"/dev/tty", 5, 0}};
  for (size_t Index = 0; Index < sizeof(Devices) / sizeof(Devices[0]); ++Index) {
    struct stat Info;
    CHECK(stat(Devices[Index].Path, &Info) == 0 && S_ISCHR(Info.st_mode) && (Info.st_mode & 07777) == 0666);
    CHECK(major(Info.st_rdev) == Devices[Index].Major && minor(Info.st_rdev) == Devices[Index].Minor);
  }
  char Buffer[64];
  const int Zero = open("/dev/zero", O_RDWR);
  CHECK(Zero >= 0);
  memset(Buffer, 0xaa, sizeof(Buffer));
  CHECK(read(Zero, Buffer, sizeof(Buffer)) == (ssize_t)sizeof(Buffer) && Buffer[0] == 0 && Buffer[63] == 0);
  CHECK(write(Zero, "abc", 3) == 3 && lseek(Zero, 100, SEEK_SET) == 0);
  close(Zero);
  const int Full = open("/dev/full", O_RDWR);
  CHECK(Full >= 0);
  CHECK(write(Full, "x", 1) == -1 && errno == ENOSPC);
  CHECK(write(Full, "", 0) == -1 && errno == ENOSPC);
  CHECK(read(Full, Buffer, 4) == 4 && Buffer[0] == 0);
  close(Full);
  const char *const RandomPaths[] = {"/dev/urandom", "/dev/random"};
  for (size_t Which = 0; Which < 2; ++Which) {
    const char *Path = RandomPaths[Which];
    char First[32] = {0}, Second[32] = {0};
    const int Random = open(Path, O_RDONLY);
    CHECK(Random >= 0);
    CHECK(read(Random, First, sizeof(First)) == (ssize_t)sizeof(First) && read(Random, Second, sizeof(Second)) == (ssize_t)sizeof(Second));
    CHECK(memcmp(First, Second, sizeof(First)) != 0);
    struct pollfd Poll = {Random, POLLIN, 0};
    CHECK(poll(&Poll, 1, 0) == 1 && (Poll.revents & POLLIN));
    close(Random);
  }
  // Without a controlling terminal /dev/tty exists but cannot be opened (the console run has none).
  const int Terminal = open("/dev/tty", O_RDWR);
  const int TerminalError = errno;
  if (Terminal >= 0) close(Terminal);
  else CHECK(TerminalError == ENXIO);
}

TEST(proc, sys_files) {
  const struct {
    const char *Path;
    const char *Value;
  } Files[] = {{"/proc/sys/kernel/overflowuid", "65534\n"}, {"/proc/sys/kernel/overflowgid", "65534\n"}, {"/proc/sys/kernel/ostype", "Linux\n"}};
  for (size_t Index = 0; Index < sizeof(Files) / sizeof(Files[0]); ++Index) {
    char Buffer[64] = {0};
    const int Fd = open(Files[Index].Path, O_RDONLY);
    CHECK(Fd >= 0 && read(Fd, Buffer, sizeof(Buffer) - 1) == (ssize_t)strlen(Files[Index].Value) && !strcmp(Buffer, Files[Index].Value));
    if (Fd >= 0) close(Fd);
  }
  struct utsname Name;
  CHECK(uname(&Name) == 0);
  char Text[128] = {0};
  int Fd = open("/proc/sys/kernel/hostname", O_RDONLY);
  CHECK(Fd >= 0 && read(Fd, Text, sizeof(Text) - 1) > 0);
  Text[strcspn(Text, "\n")] = 0;
  CHECK(!strcmp(Text, Name.nodename));
  if (Fd >= 0) close(Fd);
  memset(Text, 0, sizeof(Text));
  Fd = open("/proc/sys/kernel/osrelease", O_RDONLY);
  CHECK(Fd >= 0 && read(Fd, Text, sizeof(Text) - 1) > 0);
  Text[strcspn(Text, "\n")] = 0;
  CHECK(!strcmp(Text, Name.release));
  if (Fd >= 0) close(Fd);
  memset(Text, 0, sizeof(Text));
  Fd = open("/proc/sys/user/max_user_namespaces", O_RDONLY);
  CHECK(Fd >= 0 && read(Fd, Text, sizeof(Text) - 1) > 0 && Text[0] >= '1' && Text[0] <= '9');
  if (Fd >= 0) close(Fd);
  // the directories list and stat as directories
  struct stat Info;
  CHECK(stat("/proc/sys/kernel", &Info) == 0 && S_ISDIR(Info.st_mode));
  DIR *Directory = opendir("/proc/sys/kernel");
  CHECK(Directory != NULL);
  int SawUid = 0;
  for (struct dirent *Entry; Directory && (Entry = readdir(Directory));) SawUid |= !strcmp(Entry->d_name, "overflowuid");
  CHECK(SawUid);
  if (Directory) closedir(Directory);
}
