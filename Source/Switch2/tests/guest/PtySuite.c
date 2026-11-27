// SPDX-License-Identifier: MIT
// Pseudo-terminal compatibility suite for the Switch 2 guest runtime.
//
// Same harness as proc-suite: every test runs in its own forked child, the
// results are TAP lines, and the exit status is 42 when every selected test
// passes or skips. Expectations are Linux's; the suite passes natively
// before it runs on the console. Session leaders that a test needs (a
// controlling terminal is per session) are forked helpers that alarm out.
//
// Usage: pty-suite [pattern...] | pty-suite -l   (see proc-suite)
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

#ifndef TIOCGPTPEER
#define TIOCGPTPEER 0x5441
#endif
#ifndef TIOCGPTLCK
#define TIOCGPTLCK _IOR('T', 0x39, int)
#endif
#ifndef TIOCPKT_FLUSHREAD
#define TIOCPKT_FLUSHREAD 1
#define TIOCPKT_FLUSHWRITE 2
#define TIOCPKT_STOP 4
#define TIOCPKT_START 8
#endif

// struct termios2 of the x86-64 kernel (<asm/termbits.h> clashes with <termios.h>).
struct KernelTermios2 {
  unsigned c_iflag, c_oflag, c_cflag, c_lflag;
  unsigned char c_line, c_cc[19];
  unsigned c_ispeed, c_ospeed;
};
#define KTCGETS2 _IOR('T', 0x2A, struct KernelTermios2)
#define KTCSETS2 _IOW('T', 0x2B, struct KernelTermios2)
#define KTCSETSW2 _IOW('T', 0x2C, struct KernelTermios2)
#define KTCSETSF2 _IOW('T', 0x2D, struct KernelTermios2)
#define KBOTHER 0x1000

// ---------------------------------------------------------------------------
// Helpers

static int OpenMaster(int Flags) { return open("/dev/ptmx", O_RDWR | O_NOCTTY | Flags); }

static int SlaveName(int Master, char *Name, size_t Size) {
  int Index = -1;
  if (ioctl(Master, TIOCGPTN, &Index) < 0) return -1;
  snprintf(Name, Size, "/dev/pts/%d", Index);
  return Index;
}

// Opens a master and an unlocked slave, neither a controlling terminal.
static int OpenPair(int *Master, int *Slave, int SlaveFlags) {
  char Name[64];
  *Master = OpenMaster(0);
  if (*Master < 0) return -1;
  int Unlock = 0;
  if (ioctl(*Master, TIOCSPTLCK, &Unlock) < 0) return -1;
  if (SlaveName(*Master, Name, sizeof(Name)) < 0) return -1;
  *Slave = open(Name, O_RDWR | O_NOCTTY | SlaveFlags);
  return *Slave < 0 ? -1 : 0;
}

static void SetNonblocking(int Fd) { fcntl(Fd, F_SETFL, fcntl(Fd, F_GETFL) | O_NONBLOCK); }

// poll with one event set; returns revents or -1.
static int PollEvents(int Fd, short Events, int Milliseconds) {
  struct pollfd Entry = {.fd = Fd, .events = Events};
  const int Count = poll(&Entry, 1, Milliseconds);
  return Count < 0 ? -1 : Count == 0 ? 0 : Entry.revents;
}

// Reads exactly Size bytes or stops at the timeout; returns the bytes read.
static int ReadExact(int Fd, void *Buffer, size_t Size, int Milliseconds) {
  size_t Total = 0;
  const uint64_t Limit = Deadline(Milliseconds);
  while (Total < Size && !DeadlineExpired(Limit)) {
    if (!PollEvents(Fd, POLLIN, 20)) continue;
    const ssize_t Count = read(Fd, (char *)Buffer + Total, Size - Total);
    if (Count > 0) Total += Count;
    else if (Count == 0 || (errno != EAGAIN && errno != EINTR)) break;
  }
  return (int)Total;
}

// The master or slave reads exactly Text (accumulating over short reads).
static int ReadsText(int Fd, const char *Text) {
  char Buffer[512] = {0};
  const size_t Size = strlen(Text);
  const int Got = ReadExact(Fd, Buffer, Size, 2000);
  if (Got != (int)Size || memcmp(Buffer, Text, Size)) {
    printf("#   expected \"%s\", got %d bytes \"%.*s\"\n", Text, Got, Got > 0 ? Got : 0, Buffer);
    return 0;
  }
  return 1;
}

static int WritesText(int Fd, const char *Text) { return write(Fd, Text, strlen(Text)) == (ssize_t)strlen(Text); }

static int NoDataWithin(int Fd, int Milliseconds) { return !(PollEvents(Fd, POLLIN, Milliseconds) & POLLIN); }

static void MakeRaw(int Fd) {
  struct termios Attr;
  tcgetattr(Fd, &Attr);
  cfmakeraw(&Attr);
  Attr.c_cc[VMIN] = 1;
  Attr.c_cc[VTIME] = 0;
  tcsetattr(Fd, TCSANOW, &Attr);
}

static void SetLocal(int Fd, tcflag_t Clear, tcflag_t Set) {
  struct termios Attr;
  tcgetattr(Fd, &Attr);
  Attr.c_lflag = (Attr.c_lflag & ~Clear) | Set;
  tcsetattr(Fd, TCSANOW, &Attr);
}

// Parses digits of Base at *Text and advances it; the debian glibc of the guest has no C23 strtol/scanf entry points.
static unsigned long long ParseNumber(const char **Text, int Base) {
  unsigned long long Value = 0;
  while (**Text >= '0' && **Text < '0' + Base) Value = Value * Base + (unsigned long long)(*(*Text)++ - '0');
  return Value;
}

static const char *SkipSpaces(const char *Text) {
  while (*Text == ' ' || *Text == '\t') ++Text;
  return Text;
}

// The mode and group the devpts mount gives new nodes: /proc/mounts options, else the kernel defaults.
static void ExpectedPtsOwnership(unsigned *Mode, int *Group) {
  *Mode = 0620;
  *Group = 5;
  FILE *Mounts = fopen("/proc/mounts", "r");
  if (!Mounts) return;
  char Line[512];
  while (fgets(Line, sizeof(Line), Mounts)) {
    // source target type options dump pass
    char *Save = NULL;
    strtok_r(Line, " ", &Save);
    const char *Target = strtok_r(NULL, " ", &Save);
    const char *Type = strtok_r(NULL, " ", &Save);
    const char *Options = strtok_r(NULL, " ", &Save);
    if (!Target || !Type || !Options || strcmp(Target, "/dev/pts") || strcmp(Type, "devpts")) continue;
    *Mode = 0600;
    *Group = (int)getgid();
    const char *Item = strstr(Options, "mode=");
    if (Item) {
      Item += 5;
      *Mode = (unsigned)ParseNumber(&Item, 8);
    }
    Item = strstr(Options, "gid=");
    if (Item) {
      Item += 4;
      *Group = (int)ParseNumber(&Item, 10);
    }
  }
  fclose(Mounts);
}

// The id a process sees for kernel group Gid: its gid_map translation, else the overflow id.
static int VisibleGid(int Gid) {
  FILE *Map = fopen("/proc/self/gid_map", "r");
  if (!Map) return Gid;
  char Line[128];
  int Result = 65534;
  while (fgets(Line, sizeof(Line), Map)) {
    const char *Text = SkipSpaces(Line);
    const unsigned long long Inside = ParseNumber(&Text, 10);
    Text = SkipSpaces(Text);
    const unsigned long long Outside = ParseNumber(&Text, 10);
    Text = SkipSpaces(Text);
    const unsigned long long Count = ParseNumber(&Text, 10);
    if ((unsigned long long)Gid >= Outside && (unsigned long long)Gid - Outside < Count) Result = (int)(Inside + ((unsigned long long)Gid - Outside));
  }
  fclose(Map);
  return Result;
}

// Root of the initial user namespace (the rule for stealing a terminal is capable(CAP_SYS_ADMIN)).
static int IsInitialRoot(void) {
  if (geteuid() != 0) return 0;
  FILE *Map = fopen("/proc/self/uid_map", "r");
  if (!Map) return 1;
  char Line[128] = {0};
  const char *Read = fgets(Line, sizeof(Line), Map);
  fclose(Map);
  if (!Read) return 0;
  const char *Text = SkipSpaces(Line);
  const unsigned long long Inside = ParseNumber(&Text, 10);
  Text = SkipSpaces(Text);
  const unsigned long long Outside = ParseNumber(&Text, 10);
  Text = SkipSpaces(Text);
  const unsigned long long Count = ParseNumber(&Text, 10);
  return Inside == 0 && Outside == 0 && Count >= 4294967295ULL;
}

// Per-test session children. Up carries bytes from the child, Down to it.
static int Up[2], Down[2], Peer[2];
static volatile sig_atomic_t GotHup, GotInt, GotQuit, GotWinch, GotTstp, GotCont, GotTerm, GotUsr1;

static void Count(int Signal) {
  switch (Signal) {
  case SIGHUP: ++GotHup; break;
  case SIGINT: ++GotInt; break;
  case SIGQUIT: ++GotQuit; break;
  case SIGWINCH: ++GotWinch; break;
  case SIGTSTP: ++GotTstp; break;
  case SIGCONT: ++GotCont; break;
  case SIGTERM: ++GotTerm; break;
  case SIGUSR1: ++GotUsr1; break;
  }
}

static void Handle(int Signal) {
  struct sigaction Action = {.sa_handler = Count};
  sigemptyset(&Action.sa_mask);
  sigaction(Signal, &Action, NULL);
}

static void Ignore(int Signal) {
  struct sigaction Action = {.sa_handler = SIG_IGN};
  sigemptyset(&Action.sa_mask);
  sigaction(Signal, &Action, NULL);
}

static void Tell(char Byte) { (void)!write(Up[1], &Byte, 1); }
static void TellDown(char Byte) { (void)!write(Down[1], &Byte, 1); }
// Waits for a byte from the other side; returns it or -1 on timeout.
static int Hear(int Fd, int Milliseconds) {
  if (!(PollEvents(Fd, POLLIN, Milliseconds) & POLLIN)) return -1;
  char Byte;
  return read(Fd, &Byte, 1) == 1 ? Byte : -1;
}
// Waits until Counter reaches Target (signal delivery interrupts the poll).
static int WaitCount(volatile sig_atomic_t *Counter, int Target, int Milliseconds) {
  const uint64_t Limit = Deadline(Milliseconds);
  while (*Counter < Target && !DeadlineExpired(Limit)) poll(NULL, 0, 10);
  return *Counter >= Target;
}

typedef void (*SessionBody)(int Slave);
static int SessionSlave;
static int SessionMaster;
static SessionBody SessionBodyFunction;

static void SessionChildMain(void) {
  alarm(8);
  setsid();
  if (SessionMaster >= 0) close(SessionMaster);
  SessionBodyFunction(SessionSlave);
}

// Forks a child that leads a new session, closes Master (when >= 0), and runs Body with Slave. The child
// opens nothing: the body decides how the slave becomes its controlling terminal. Pipes Up and Down are set.
static pid_t StartSession(int Master, int Slave, SessionBody Body) {
  CHECK(pipe(Up) == 0 && pipe(Down) == 0 && pipe(Peer) == 0);
  SessionMaster = Master;
  SessionSlave = Slave;
  SessionBodyFunction = Body;
  return Fork(SessionChildMain);
}

static int ExitCode(pid_t Pid) {
  const int Status = WaitExit(Pid);
  return WIFEXITED(Status) ? WEXITSTATUS(Status) : 100 + (WIFSIGNALED(Status) ? WTERMSIG(Status) : 99);
}

// ---------------------------------------------------------------------------
// Opening, allocation and identity

TEST(pty, posix_openpt_grantpt_unlockpt_ptsname) {
  const int Master = posix_openpt(O_RDWR | O_NOCTTY);
  CHECK(Master >= 0);
  CHECK(grantpt(Master) == 0);
  CHECK(unlockpt(Master) == 0);
  char Name[64], Small[6];
  CHECK(ptsname_r(Master, Name, sizeof(Name)) == 0);
  CHECK(strncmp(Name, "/dev/pts/", 9) == 0);
  CHECK(ptsname_r(Master, Small, sizeof(Small)) == ERANGE);
  CHECK(strcmp(ptsname(Master), Name) == 0);
  const int Slave = open(Name, O_RDWR | O_NOCTTY);
  CHECK(Slave >= 0);
  CHECK(isatty(Slave) && isatty(Master));
  CHECK_ERRNO(lseek(Slave, 0, SEEK_SET), ESPIPE);
  char Byte;
  CHECK_ERRNO(pread(Slave, &Byte, 1, 0), ESPIPE);
  CHECK_ERRNO(pwrite(Master, "x", 1, 0), ESPIPE);
}

TEST(pty, ptmx_open_flags) {
  const int Master = OpenMaster(O_NONBLOCK | O_CLOEXEC);
  CHECK(Master >= 0);
  CHECK(fcntl(Master, F_GETFD) & FD_CLOEXEC);
  CHECK(fcntl(Master, F_GETFL) & O_NONBLOCK);
  char Byte;
  CHECK_ERRNO(read(Master, &Byte, 1), EAGAIN);
  const int ReadOnly = open("/dev/ptmx", O_RDONLY | O_NOCTTY);
  CHECK(ReadOnly >= 0);
  struct stat Info;
  CHECK(fstat(Master, &Info) == 0 && S_ISCHR(Info.st_mode));
  CHECK(major(Info.st_rdev) == 5 && minor(Info.st_rdev) == 2);
  // Each open allocates a new pair.
  int First = -1, Second = -1;
  CHECK(ioctl(Master, TIOCGPTN, &First) == 0 && ioctl(ReadOnly, TIOCGPTN, &Second) == 0);
  CHECK(First != Second);
}

TEST(pty, index_allocation_and_reuse) {
  const int A = OpenMaster(0), B = OpenMaster(0);
  CHECK(A >= 0 && B >= 0);
  int IndexA = -1, IndexB = -1;
  CHECK(ioctl(A, TIOCGPTN, &IndexA) == 0 && ioctl(B, TIOCGPTN, &IndexB) == 0);
  CHECK(IndexA != IndexB);
  int Unlock = 0;
  ioctl(A, TIOCSPTLCK, &Unlock);
  char Name[64];
  snprintf(Name, sizeof(Name), "/dev/pts/%d", IndexA);
  const int Slave = open(Name, O_RDWR | O_NOCTTY);
  CHECK(Slave >= 0);
  close(A);
  // The slave still holds the pair: the index is not free yet.
  const int C = OpenMaster(0);
  int IndexC = -1;
  CHECK(ioctl(C, TIOCGPTN, &IndexC) == 0 && IndexC != IndexA && IndexC != IndexB);
  close(Slave);
  close(C);
  // Everything of the first pair is closed: the lowest free index is at most its old one.
  const int D = OpenMaster(0);
  int IndexD = -1;
  CHECK(ioctl(D, TIOCGPTN, &IndexD) == 0 && IndexD <= IndexA && IndexD != IndexB);
}

TEST(pty, slave_locked_until_unlocked) {
  const int Master = OpenMaster(0);
  CHECK(Master >= 0);
  char Name[64];
  CHECK(SlaveName(Master, Name, sizeof(Name)) >= 0);
  int Lock = -1;
  CHECK(ioctl(Master, TIOCGPTLCK, &Lock) == 0 && Lock == 1);
  CHECK_ERRNO(open(Name, O_RDWR | O_NOCTTY), EIO);
  // TIOCGPTPEER honours the lock like any other open.
  CHECK_ERRNO(ioctl(Master, TIOCGPTPEER, O_RDWR | O_NOCTTY), EIO);
  Lock = 0;
  CHECK(ioctl(Master, TIOCSPTLCK, &Lock) == 0);
  CHECK(ioctl(Master, TIOCGPTLCK, &Lock) == 0 && Lock == 0);
  const int Slave = open(Name, O_RDWR | O_NOCTTY);
  CHECK(Slave >= 0);
  Lock = 1;
  CHECK(ioctl(Master, TIOCSPTLCK, &Lock) == 0);
  CHECK_ERRNO(open(Name, O_RDWR | O_NOCTTY), EIO);
  CHECK(unlockpt(Master) == 0);
  CHECK(open(Name, O_RDWR | O_NOCTTY) >= 0);
}

TEST(pty, ioctls_belong_to_one_side) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  int Index = -1;
  CHECK(ioctl(Master, TIOCGPTN, &Index) == 0);
  char Name[64];
  snprintf(Name, sizeof(Name), "/dev/pts/%d", Index);
  CHECK(strcmp(ptsname(Master), Name) == 0);
  CHECK_ERRNO(ioctl(Slave, TIOCGPTN, &Index), ENOTTY);
  int Lock = 0;
  CHECK_ERRNO(ioctl(Slave, TIOCSPTLCK, &Lock), ENOTTY);
  CHECK_ERRNO(ioctl(Slave, TIOCGPTPEER, O_RDWR), EIO);
  int Pipe[2];
  CHECK(pipe(Pipe) == 0);
  CHECK_ERRNO(ioctl(Pipe[0], TIOCGPTN, &Index), ENOTTY);
  CHECK_ERRNO(ioctl(Pipe[0], TCGETS, &(struct termios) {0}), ENOTTY);
  CHECK(!isatty(Pipe[0]));
}

TEST(pty, tiocgptpeer) {
  const int Master = OpenMaster(0);
  CHECK(Master >= 0);
  CHECK(unlockpt(Master) == 0);
  const int Peer = ioctl(Master, TIOCGPTPEER, O_RDWR | O_NOCTTY | O_CLOEXEC);
  CHECK(Peer >= 0);
  CHECK(fcntl(Peer, F_GETFD) & FD_CLOEXEC);
  int Index = -1;
  ioctl(Master, TIOCGPTN, &Index);
  struct stat Info;
  CHECK(fstat(Peer, &Info) == 0 && S_ISCHR(Info.st_mode));
  CHECK(major(Info.st_rdev) == 136 && (int)minor(Info.st_rdev) == Index);
  MakeRaw(Peer);
  CHECK(WritesText(Master, "ping"));
  CHECK(ReadsText(Peer, "ping"));
  CHECK(WritesText(Peer, "pong"));
  CHECK(ReadsText(Master, "pong"));
  char Link[64], Expected[64];
  snprintf(Expected, sizeof(Expected), "/dev/pts/%d", Index);
  snprintf(Link, sizeof(Link), "/proc/self/fd/%d", Peer);
  char Target[64] = {0};
  CHECK(readlink(Link, Target, sizeof(Target) - 1) > 0 && !strcmp(Target, Expected));
  // The flags are checked: an invalid flag fails, a read-only open works.
  CHECK(ioctl(Master, TIOCGPTPEER, O_RDONLY | O_NOCTTY) >= 0);
}

TEST(pty, slave_node_stat_and_directory) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  char Name[64];
  const int Index = SlaveName(Master, Name, sizeof(Name));
  unsigned Mode;
  int Group;
  ExpectedPtsOwnership(&Mode, &Group);
  struct stat ByFd, ByName;
  CHECK(fstat(Slave, &ByFd) == 0 && stat(Name, &ByName) == 0);
  CHECK(S_ISCHR(ByFd.st_mode));
  CHECK(major(ByFd.st_rdev) == 136 && (int)minor(ByFd.st_rdev) == Index);
  CHECK((ByFd.st_mode & 07777) == Mode);
  CHECK(ByFd.st_uid == geteuid());
  CHECK((int)ByFd.st_gid == VisibleGid(Group));
  CHECK(ByFd.st_dev == ByName.st_dev && ByFd.st_ino == ByName.st_ino);
  CHECK(ByFd.st_rdev == ByName.st_rdev && ByFd.st_mode == ByName.st_mode);
  CHECK(ByFd.st_uid == ByName.st_uid && ByFd.st_gid == ByName.st_gid);
  CHECK(lstat(Name, &ByName) == 0 && S_ISCHR(ByName.st_mode));
  CHECK(access(Name, R_OK | W_OK) == 0);
  // The directory lists the slave and ptmx while the master is open.
  int Found = 0, FoundPtmx = 0;
  DIR *Directory = opendir("/dev/pts");
  CHECK(Directory != NULL);
  const char *Base = strrchr(Name, '/') + 1;
  for (struct dirent *Entry; Directory && (Entry = readdir(Directory));) {
    if (!strcmp(Entry->d_name, Base)) {
      Found = 1;
      CHECK(Entry->d_type == DT_CHR);
    }
    if (!strcmp(Entry->d_name, "ptmx")) FoundPtmx = 1;
  }
  if (Directory) closedir(Directory);
  CHECK(Found);
  CHECK(FoundPtmx);
  // The master's close removes the node, though the slave descriptor lives on.
  close(Master);
  CHECK_ERRNO(stat(Name, &ByName), ENOENT);
  CHECK_ERRNO(open(Name, O_RDWR | O_NOCTTY), ENOENT);
  Directory = opendir("/dev/pts");
  Found = 0;
  for (struct dirent *Entry; Directory && (Entry = readdir(Directory));) Found |= !strcmp(Entry->d_name, Base);
  if (Directory) closedir(Directory);
  CHECK(!Found);
  CHECK(fstat(Slave, &ByFd) == 0 && S_ISCHR(ByFd.st_mode));
}

TEST(pty, slave_node_chmod_chown) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  char Name[64];
  SlaveName(Master, Name, sizeof(Name));
  struct stat Info;
  // The owner changes the mode through the descriptor or the path; both views follow.
  CHECK(fchmod(Slave, 0600) == 0);
  CHECK(fstat(Slave, &Info) == 0 && (Info.st_mode & 07777) == 0600);
  CHECK(stat(Name, &Info) == 0 && (Info.st_mode & 07777) == 0600);
  CHECK(chmod(Name, 0660) == 0);
  CHECK(fstat(Slave, &Info) == 0 && (Info.st_mode & 07777) == 0660 && S_ISCHR(Info.st_mode));
  // chown to the caller's own group; another owner needs privilege.
  CHECK(fchown(Slave, (uid_t)-1, getegid()) == 0);
  CHECK(fstat(Slave, &Info) == 0 && Info.st_gid == getegid() && Info.st_uid == geteuid());
  CHECK(chown(Name, geteuid(), getegid()) == 0);
  if (geteuid() != 0) CHECK_ERRNO(fchown(Slave, geteuid() + 1, (gid_t)-1), EPERM);
  // The mode governs a later open: owner access removed.
  CHECK(fchmod(Slave, 0000) == 0);
  if (geteuid() != 0) CHECK_ERRNO(open(Name, O_RDWR | O_NOCTTY), EACCES);
  CHECK(fchmod(Slave, 0620) == 0);
  CHECK(open(Name, O_RDWR | O_NOCTTY) >= 0);
}

TEST(pty, ptmx_node) {
  struct stat Info;
  CHECK(stat("/dev/ptmx", &Info) == 0 && S_ISCHR(Info.st_mode));
  CHECK(major(Info.st_rdev) == 5 && minor(Info.st_rdev) == 2);
  CHECK((Info.st_mode & 0666) == 0666);
  int Found = 0;
  DIR *Directory = opendir("/dev");
  for (struct dirent *Entry; Directory && (Entry = readdir(Directory));) Found |= !strcmp(Entry->d_name, "ptmx");
  if (Directory) closedir(Directory);
  CHECK(Found);
  CHECK(stat("/dev/pts/ptmx", &Info) == 0 && S_ISCHR(Info.st_mode));
  CHECK(major(Info.st_rdev) == 5 && minor(Info.st_rdev) == 2);
  if (access("/dev/pts/ptmx", R_OK | W_OK) == 0) {
    const int Master = open("/dev/pts/ptmx", O_RDWR | O_NOCTTY);
    CHECK(Master >= 0);
    int Index = -1;
    CHECK(ioctl(Master, TIOCGPTN, &Index) == 0);
  }
}

TEST(pty, ttyname_and_proc_fd_link) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  char Name[64];
  SlaveName(Master, Name, sizeof(Name));
  const char *Found = ttyname(Slave);
  CHECK(Found && !strcmp(Found, Name));
  char Buffer[64];
  CHECK(ttyname_r(Slave, Buffer, sizeof(Buffer)) == 0 && !strcmp(Buffer, Name));
  CHECK(ttyname_r(Slave, Buffer, 3) == ERANGE);
  char Link[64], Target[64] = {0};
  snprintf(Link, sizeof(Link), "/proc/self/fd/%d", Slave);
  CHECK(readlink(Link, Target, sizeof(Target) - 1) > 0 && !strcmp(Target, Name));
  snprintf(Link, sizeof(Link), "/proc/self/fd/%d", Master);
  memset(Target, 0, sizeof(Target));
  CHECK(readlink(Link, Target, sizeof(Target) - 1) > 0 && (!strcmp(Target, "/dev/ptmx") || !strcmp(Target, "/dev/pts/ptmx")));
  CHECK(!isatty(open("/dev/null", O_RDWR)));
}

// ---------------------------------------------------------------------------
// termios and window size

// The speed a cfget*speed value stands for, in bits per second. The Bxxx macros are the kernel's codes (B9600 = 0015)
// before glibc 2.42 and the rates themselves (B9600 = 9600) from 2.42 on, and the guest programs are compiled with the
// host's headers but run against Debian 12's glibc 2.36, whose cfgetospeed returns the code. Both conventions mean the
// same line speed, so the test compares speeds.
static unsigned SpeedBps(speed_t Value) {
  static const unsigned Basic[] = {0, 50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400, 4800, 9600, 19200, 38400};
  static const unsigned Extended[] = {0, 57600, 115200, 230400, 460800, 500000, 576000, 921600,
                                      1000000, 1152000, 1500000, 2000000, 2500000, 3000000, 3500000, 4000000};
  if (Value <= 15) return Basic[Value];
  if (Value >= 010001 && Value <= 010017) return Extended[Value - 010000];
  return (unsigned) Value;
}

TEST(pty, termios_defaults_and_round_trip) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  struct termios Attr, FromMaster;
  CHECK(tcgetattr(Slave, &Attr) == 0);
  CHECK((Attr.c_iflag & (ICRNL | IXON)) == (ICRNL | IXON));
  CHECK((Attr.c_oflag & (OPOST | ONLCR)) == (OPOST | ONLCR));
  CHECK((Attr.c_cflag & CSIZE) == CS8 && (Attr.c_cflag & CREAD));
  CHECK((Attr.c_lflag & (ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | IEXTEN)) ==
        (ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | IEXTEN));
  CHECK(Attr.c_cc[VINTR] == 3 && Attr.c_cc[VQUIT] == 28 && Attr.c_cc[VERASE] == 127 && Attr.c_cc[VKILL] == 21);
  CHECK(Attr.c_cc[VEOF] == 4 && Attr.c_cc[VMIN] == 1 && Attr.c_cc[VTIME] == 0 && Attr.c_cc[VSTART] == 17);
  CHECK(Attr.c_cc[VSTOP] == 19 && Attr.c_cc[VSUSP] == 26 && Attr.c_cc[VEOL] == 0);
  CHECK(SpeedBps(cfgetospeed(&Attr)) == 38400 && SpeedBps(cfgetispeed(&Attr)) == 38400);
  // The master sees the slave's settings, and sets them.
  CHECK(tcgetattr(Master, &FromMaster) == 0);
  CHECK(!memcmp(&Attr.c_iflag, &FromMaster.c_iflag, 4) && !memcmp(&Attr.c_lflag, &FromMaster.c_lflag, 4));
  Attr.c_lflag &= ~(ECHO | ICANON);
  Attr.c_cc[VMIN] = 4;
  Attr.c_cc[VTIME] = 7;
  Attr.c_cc[VINTR] = 'x';
  CHECK(tcsetattr(Slave, TCSANOW, &Attr) == 0);
  struct termios Check;
  CHECK(tcgetattr(Slave, &Check) == 0 && !(Check.c_lflag & (ECHO | ICANON)));
  CHECK(Check.c_cc[VMIN] == 4 && Check.c_cc[VTIME] == 7 && Check.c_cc[VINTR] == 'x');
  CHECK(tcgetattr(Master, &Check) == 0 && Check.c_cc[VMIN] == 4);
  Attr.c_cc[VMIN] = 2;
  CHECK(tcsetattr(Master, TCSADRAIN, &Attr) == 0);
  CHECK(tcgetattr(Slave, &Check) == 0 && Check.c_cc[VMIN] == 2);
  CHECK(tcsetattr(Slave, TCSAFLUSH, &Attr) == 0);
  CHECK(cfsetspeed(&Attr, B9600) == 0);
  CHECK(tcsetattr(Slave, TCSANOW, &Attr) == 0);
  CHECK(tcgetattr(Slave, &Check) == 0 && SpeedBps(cfgetospeed(&Check)) == 9600 && SpeedBps(cfgetispeed(&Check)) == 9600);
}

TEST(pty, termios2_speeds) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  struct KernelTermios2 Attr;
  CHECK(ioctl(Slave, KTCGETS2, &Attr) == 0);
  CHECK(Attr.c_ispeed == 38400 && Attr.c_ospeed == 38400);
  CHECK((Attr.c_lflag & ICANON) && Attr.c_cc[VMIN] == 1);
  Attr.c_cflag = (Attr.c_cflag & ~CBAUD) | KBOTHER;
  Attr.c_ispeed = Attr.c_ospeed = 12345;
  CHECK(ioctl(Slave, KTCSETS2, &Attr) == 0);
  struct KernelTermios2 Read;
  CHECK(ioctl(Master, KTCGETS2, &Read) == 0);
  CHECK((Read.c_cflag & CBAUD) == KBOTHER && Read.c_ispeed == 12345 && Read.c_ospeed == 12345);
  Attr.c_lflag &= ~ECHO;
  CHECK(ioctl(Slave, KTCSETSW2, &Attr) == 0 && ioctl(Slave, KTCSETSF2, &Attr) == 0);
  struct termios Old;
  CHECK(tcgetattr(Slave, &Old) == 0 && !(Old.c_lflag & ECHO));
}

TEST(pty, window_size_both_sides) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  struct winsize Size = {0xffff, 0xffff, 0xffff, 0xffff};
  CHECK(ioctl(Slave, TIOCGWINSZ, &Size) == 0);
  CHECK(Size.ws_row == 0 && Size.ws_col == 0 && Size.ws_xpixel == 0 && Size.ws_ypixel == 0);
  struct winsize Set = {24, 80, 640, 480};
  CHECK(ioctl(Master, TIOCSWINSZ, &Set) == 0);
  CHECK(ioctl(Slave, TIOCGWINSZ, &Size) == 0 && !memcmp(&Size, &Set, sizeof(Size)));
  CHECK(ioctl(Master, TIOCGWINSZ, &Size) == 0 && !memcmp(&Size, &Set, sizeof(Size)));
  Set = (struct winsize) {50, 132, 0, 0};
  CHECK(ioctl(Slave, TIOCSWINSZ, &Set) == 0);
  CHECK(ioctl(Master, TIOCGWINSZ, &Size) == 0 && Size.ws_row == 50 && Size.ws_col == 132);
  CHECK_ERRNO(ioctl(Slave, TIOCGWINSZ, (void *)8), EFAULT);
}

// ---------------------------------------------------------------------------
// Line discipline: canonical, raw, echo, input and output flags

TEST(pty, canonical_reads_one_line) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, O_NONBLOCK) == 0);
  CHECK(WritesText(Master, "ab"));
  char Buffer[64];
  poll(NULL, 0, 50);
  CHECK_ERRNO(read(Slave, Buffer, sizeof(Buffer)), EAGAIN);
  CHECK(!(PollEvents(Slave, POLLIN, 0) & POLLIN));
  CHECK(WritesText(Master, "c\nsecond\nthi"));
  CHECK(ReadExact(Slave, Buffer, 4, 2000) == 4 && !memcmp(Buffer, "abc\n", 4));
  // The next line is a separate read, even with room for more.
  CHECK(ReadExact(Slave, Buffer, 7, 2000) == 7 && !memcmp(Buffer, "second\n", 7));
  CHECK_ERRNO(read(Slave, Buffer, sizeof(Buffer)), EAGAIN);
  // A short buffer takes part of a line; the rest stays for the next read.
  CHECK(WritesText(Master, "d\n"));
  CHECK(ReadExact(Slave, Buffer, 1, 2000) == 1 && Buffer[0] == 't');
  CHECK(ReadExact(Slave, Buffer, 4, 2000) == 4 && !memcmp(Buffer, "hid\n", 4));
}

TEST(pty, canonical_erase_kill_eof) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, O_NONBLOCK) == 0);
  char Buffer[64];
  CHECK(WritesText(Master, "abc\x7f\n"));
  CHECK(ReadExact(Slave, Buffer, 3, 2000) == 3 && !memcmp(Buffer, "ab\n", 3));
  CHECK(WritesText(Master, "abc\x15xy\n"));
  CHECK(ReadExact(Slave, Buffer, 3, 2000) == 3 && !memcmp(Buffer, "xy\n", 3));
  // ^D after text ends the line without a newline; ^D on an empty line is end of file.
  CHECK(WritesText(Master, "ab\x04"));
  CHECK(ReadExact(Slave, Buffer, 2, 2000) == 2 && !memcmp(Buffer, "ab", 2));
  CHECK_ERRNO(read(Slave, Buffer, sizeof(Buffer)), EAGAIN);
  CHECK(WritesText(Master, "\x04"));
  poll(NULL, 0, 50);
  CHECK(read(Slave, Buffer, sizeof(Buffer)) == 0);
  CHECK_ERRNO(read(Slave, Buffer, sizeof(Buffer)), EAGAIN);
  // Erase on an empty line does nothing; the erased text is gone.
  CHECK(WritesText(Master, "\x7f\x7f\nz\n"));
  CHECK(ReadExact(Slave, Buffer, 3, 2000) == 3 && !memcmp(Buffer, "\nz\n", 3));
}

TEST(pty, canonical_lnext_werase_reprint) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, O_NONBLOCK) == 0);
  char Buffer[64];
  // ^V quotes the next byte: it is data, even a signal character.
  CHECK(WritesText(Master, "\x16\x03x\n"));
  CHECK(ReadsText(Master, "^\b^Cx\r\n"));
  CHECK(ReadExact(Slave, Buffer, 3, 2000) == 3 && !memcmp(Buffer, "\x03x\n", 3));
  // ^W erases the last word, ^R reprints the line.
  CHECK(WritesText(Master, "one two\x17"));
  CHECK(ReadsText(Master, "one two\b \b\b \b\b \b"));
  CHECK(WritesText(Master, "\x12"));
  CHECK(ReadsText(Master, "^R\r\none "));
  CHECK(WritesText(Master, "z\n"));
  CHECK(ReadsText(Master, "z\r\n"));
  CHECK(ReadExact(Slave, Buffer, 6, 2000) == 6 && !memcmp(Buffer, "one z\n", 6));
  // A tab is erased by as many backspaces as columns it covers.
  CHECK(WritesText(Master, "a\t\x7f"));
  CHECK(ReadsText(Master, "a\t\b\b\b\b\b\b\b"));
  CHECK(tcflush(Slave, TCIFLUSH) == 0);
}

TEST(pty, raw_read_vmin_vtime) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  MakeRaw(Slave);
  struct termios Attr;
  tcgetattr(Slave, &Attr);
  char Buffer[64];
  // VMIN 3: readable only with three bytes.
  Attr.c_cc[VMIN] = 3;
  Attr.c_cc[VTIME] = 0;
  tcsetattr(Slave, TCSANOW, &Attr);
  CHECK(WritesText(Master, "ab"));
  CHECK(!(PollEvents(Slave, POLLIN, 100) & POLLIN));
  CHECK(WritesText(Master, "cde"));
  CHECK(PollEvents(Slave, POLLIN, 1000) & POLLIN);
  CHECK(read(Slave, Buffer, sizeof(Buffer)) == 5);
  // VMIN 0, VTIME 0: an immediate poll.
  Attr.c_cc[VMIN] = 0;
  CHECK(tcsetattr(Slave, TCSANOW, &Attr) == 0);
  CHECK(read(Slave, Buffer, sizeof(Buffer)) == 0);
  // VMIN 0, VTIME 3: wait up to 0.3 s for the first byte.
  Attr.c_cc[VTIME] = 3;
  CHECK(tcsetattr(Slave, TCSANOW, &Attr) == 0);
  uint64_t Start = NowMs();
  CHECK(read(Slave, Buffer, sizeof(Buffer)) == 0);
  uint64_t Elapsed = NowMs() - Start;
  CHECK(Elapsed >= 250 && Elapsed < 1500);
  CHECK(WritesText(Master, "q"));
  CHECK(read(Slave, Buffer, sizeof(Buffer)) == 1);
  // VMIN 2, VTIME 3: the inter-byte timer returns what arrived once it expires.
  Attr.c_cc[VMIN] = 2;
  CHECK(tcsetattr(Slave, TCSANOW, &Attr) == 0);
  CHECK(WritesText(Master, "z"));
  Start = NowMs();
  CHECK(read(Slave, Buffer, sizeof(Buffer)) == 1);
  Elapsed = NowMs() - Start;
  CHECK(Elapsed >= 250 && Elapsed < 1500);
}

TEST(pty, raw_read_blocks_until_data) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  MakeRaw(Slave);
  const pid_t Writer = fork();
  if (Writer == 0) {
    poll(NULL, 0, 150);
    (void)!write(Master, "wake", 4);
    _exit(0);
  }
  char Buffer[16];
  const uint64_t Start = NowMs();
  const ssize_t Count = read(Slave, Buffer, sizeof(Buffer));
  CHECK(Count == 4 && !memcmp(Buffer, "wake", 4));
  CHECK(NowMs() - Start >= 100);
  WaitExit(Writer);
  // And the other way: a blocked master read wakes when the slave writes.
  const pid_t Other = fork();
  if (Other == 0) {
    poll(NULL, 0, 150);
    (void)!write(Slave, "back", 4);
    _exit(0);
  }
  CHECK(read(Master, Buffer, sizeof(Buffer)) == 4 && !memcmp(Buffer, "back", 4));
  WaitExit(Other);
}

TEST(pty, echo_rules) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  // Canonical with ECHO: typed text and the newline come back; ECHOCTL shows control characters.
  CHECK(WritesText(Master, "hi\n"));
  CHECK(ReadsText(Master, "hi\r\n"));
  CHECK(WritesText(Master, "\x01"));
  CHECK(ReadsText(Master, "^A"));
  // ECHOE shows an erase as backspace, space, backspace; a control character takes two.
  CHECK(WritesText(Master, "\x7f"));
  CHECK(ReadsText(Master, "\b \b\b \b"));
  CHECK(WritesText(Master, "ab\x7f"));
  CHECK(ReadsText(Master, "ab\b \b"));
  // ECHOKE erases the whole line on the screen.
  CHECK(WritesText(Master, "c\x15"));
  CHECK(ReadsText(Master, "c\b \b\b \b"));
  // Without ECHOKE the kill character shows as ^U and ECHOK adds a newline.
  SetLocal(Slave, ECHOKE, 0);
  CHECK(WritesText(Master, "d\x15"));
  CHECK(ReadsText(Master, "d^U\r\n"));
  // Without ECHOE the erase character is echoed.
  SetLocal(Slave, ECHOE, 0);
  CHECK(WritesText(Master, "ef\x7f\x15"));
  CHECK(ReadsText(Master, "ef^?^U\r\n"));
  // VWERASE erases a word.
  SetLocal(Slave, 0, ECHOE | ECHOKE);
  CHECK(WritesText(Master, "ab cd\x17"));
  CHECK(ReadsText(Master, "ab cd\b \b\b \b"));
  CHECK(WritesText(Master, "\x15"));
  CHECK(ReadsText(Master, "\b \b\b \b\b \b"));
  char Buffer[64];
  CHECK(tcflush(Slave, TCIFLUSH) == 0);
  // ECHO off: nothing comes back, in canonical or raw mode.
  SetLocal(Slave, ECHO, 0);
  CHECK(WritesText(Master, "quiet\n"));
  CHECK(NoDataWithin(Master, 100));
  CHECK(ReadExact(Slave, Buffer, 6, 2000) == 6);
  // ECHO off but ECHONL: only the newline.
  SetLocal(Slave, 0, ECHONL);
  CHECK(WritesText(Master, "q\n"));
  CHECK(ReadsText(Master, "\r\n"));
  CHECK(ReadExact(Slave, Buffer, 2, 2000) == 2);
  // Raw with ECHO echoes each byte at once.
  SetLocal(Slave, ICANON | ECHONL, ECHO);
  CHECK(WritesText(Master, "k"));
  CHECK(ReadsText(Master, "k"));
  CHECK(read(Slave, Buffer, 8) == 1);
}

TEST(pty, output_post_processing) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  CHECK(WritesText(Slave, "a\nb\r\n"));
  CHECK(ReadsText(Master, "a\r\nb\r\r\n"));
  struct termios Attr;
  tcgetattr(Slave, &Attr);
  Attr.c_oflag &= ~ONLCR;
  tcsetattr(Slave, TCSANOW, &Attr);
  CHECK(WritesText(Slave, "a\nb"));
  CHECK(ReadsText(Master, "a\nb"));
  Attr.c_oflag |= OCRNL;
  tcsetattr(Slave, TCSANOW, &Attr);
  CHECK(WritesText(Slave, "x\ry"));
  CHECK(ReadsText(Master, "x\ny"));
  Attr.c_oflag = ONLCR;
  tcsetattr(Slave, TCSANOW, &Attr);
  CHECK(WritesText(Slave, "p\nq"));
  CHECK(ReadsText(Master, "p\nq"));
}

TEST(pty, input_flags) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  MakeRaw(Slave);
  struct termios Attr;
  tcgetattr(Slave, &Attr);
  char Buffer[16];
  // ICRNL maps CR to NL.
  Attr.c_iflag |= ICRNL;
  tcsetattr(Slave, TCSANOW, &Attr);
  CHECK(WritesText(Master, "a\r"));
  CHECK(ReadExact(Slave, Buffer, 2, 2000) == 2 && !memcmp(Buffer, "a\n", 2));
  // INLCR maps NL to CR.
  Attr.c_iflag = INLCR;
  tcsetattr(Slave, TCSANOW, &Attr);
  CHECK(WritesText(Master, "b\n"));
  CHECK(ReadExact(Slave, Buffer, 2, 2000) == 2 && !memcmp(Buffer, "b\r", 2));
  // IGNCR drops CR.
  Attr.c_iflag = IGNCR;
  tcsetattr(Slave, TCSANOW, &Attr);
  CHECK(WritesText(Master, "c\rd"));
  CHECK(ReadExact(Slave, Buffer, 2, 2000) == 2 && !memcmp(Buffer, "cd", 2));
  // None: bytes pass unchanged.
  Attr.c_iflag = 0;
  tcsetattr(Slave, TCSANOW, &Attr);
  CHECK(WritesText(Master, "e\r\n"));
  CHECK(ReadExact(Slave, Buffer, 3, 2000) == 3 && !memcmp(Buffer, "e\r\n", 3));
}

TEST(pty, fionread_tcflush_outq) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, O_NONBLOCK) == 0);
  SetNonblocking(Master);
  int Count = -1;
  CHECK(ioctl(Slave, FIONREAD, &Count) == 0 && Count == 0);
  CHECK(WritesText(Master, "ab"));
  poll(NULL, 0, 50);
  CHECK(ioctl(Slave, FIONREAD, &Count) == 0 && Count == 0); // an unfinished line is not readable
  CHECK(WritesText(Master, "\n"));
  poll(NULL, 0, 50);
  CHECK(ioctl(Slave, FIONREAD, &Count) == 0 && Count == 3);
  // TCIFLUSH drops unread input.
  CHECK(tcflush(Slave, TCIFLUSH) == 0);
  CHECK(ioctl(Slave, FIONREAD, &Count) == 0 && Count == 0);
  char Buffer[16];
  CHECK_ERRNO(read(Slave, Buffer, sizeof(Buffer)), EAGAIN);
  // The master counts what the slave wrote.
  while (read(Master, Buffer, sizeof(Buffer)) > 0) {}
  CHECK(WritesText(Slave, "xyz"));
  poll(NULL, 0, 50);
  CHECK(ioctl(Master, FIONREAD, &Count) == 0 && Count == 3);
  CHECK(ioctl(Slave, TIOCOUTQ, &Count) == 0 && Count >= 0);
  CHECK_ERRNO(tcflush(Slave, 77), EINVAL);
  CHECK(tcflush(Slave, TCIOFLUSH) == 0);
}

TEST(pty, flow_control_tcxonc) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, O_NONBLOCK) == 0);
  CHECK(tcflow(Slave, TCOOFF) == 0);
  CHECK_ERRNO(write(Slave, "zz", 2), EAGAIN);
  CHECK(!(PollEvents(Slave, POLLOUT, 50) & POLLOUT));
  CHECK(tcflow(Slave, TCOON) == 0);
  CHECK(WritesText(Slave, "zz"));
  CHECK(ReadsText(Master, "zz"));
  // TCIOFF and TCION put the STOP and START characters into the master's output.
  CHECK(tcflow(Slave, TCIOFF) == 0);
  CHECK(ReadsText(Master, "\x13"));
  CHECK(tcflow(Slave, TCION) == 0);
  CHECK(ReadsText(Master, "\x11"));
  CHECK_ERRNO(ioctl(Slave, TCXONC, 9), EINVAL);
  // IXON: a STOP byte typed on the master stops the slave's output; START resumes.
  CHECK(WritesText(Master, "\x13"));
  poll(NULL, 0, 50);
  CHECK_ERRNO(write(Slave, "q", 1), EAGAIN);
  CHECK(WritesText(Master, "\x11"));
  poll(NULL, 0, 50);
  CHECK(WritesText(Slave, "q"));
  CHECK(ReadsText(Master, "q"));
  // The flow-control bytes are not input data.
  CHECK(NoDataWithin(Slave, 50));
  CHECK(tcdrain(Slave) == 0);
  CHECK(tcsendbreak(Slave, 0) == 0);
}

TEST(pty, packet_mode) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  MakeRaw(Slave);
  int On = 1;
  CHECK(ioctl(Master, TIOCPKT, &On) == 0);
  CHECK(WritesText(Slave, "ab"));
  unsigned char Buffer[16];
  CHECK(ReadExact(Master, Buffer, 3, 2000) == 3 && Buffer[0] == 0 && Buffer[1] == 'a' && Buffer[2] == 'b');
  CHECK(tcflush(Slave, TCIFLUSH) == 0);
  CHECK(PollEvents(Master, POLLIN, 1000) & POLLIN);
  CHECK(read(Master, Buffer, sizeof(Buffer)) == 1 && Buffer[0] == TIOCPKT_FLUSHREAD);
  CHECK(tcflush(Slave, TCOFLUSH) == 0);
  CHECK(read(Master, Buffer, sizeof(Buffer)) == 1 && Buffer[0] == TIOCPKT_FLUSHWRITE);
  CHECK(tcflow(Slave, TCOOFF) == 0);
  CHECK(read(Master, Buffer, sizeof(Buffer)) == 1 && Buffer[0] == TIOCPKT_STOP);
  CHECK(tcflow(Slave, TCOON) == 0);
  CHECK(read(Master, Buffer, sizeof(Buffer)) == 1 && Buffer[0] == TIOCPKT_START);
  On = 0;
  CHECK(ioctl(Master, TIOCPKT, &On) == 0);
  CHECK(WritesText(Slave, "c"));
  CHECK(ReadExact(Master, Buffer, 1, 2000) == 1 && Buffer[0] == 'c');
}

// ---------------------------------------------------------------------------
// Master and slave lifetime, readiness

TEST(pty, master_without_slave) {
  const int Master = OpenMaster(O_NONBLOCK);
  CHECK(Master >= 0);
  char Buffer[16];
  CHECK_ERRNO(read(Master, Buffer, sizeof(Buffer)), EAGAIN);
  const int Events = PollEvents(Master, POLLIN | POLLOUT, 0);
  CHECK((Events & POLLOUT) && !(Events & (POLLIN | POLLHUP)));
  // Writes are accepted before any slave exists and wait in the slave's input queue.
  CHECK(WritesText(Master, "early\n"));
  CHECK(ReadsText(Master, "early\r\n"));
  char Name[64];
  SlaveName(Master, Name, sizeof(Name));
  unlockpt(Master);
  const int Slave = open(Name, O_RDWR | O_NOCTTY);
  CHECK(Slave >= 0);
  CHECK(ReadExact(Slave, Buffer, 6, 2000) == 6 && !memcmp(Buffer, "early\n", 6));
}

TEST(pty, master_read_eio_after_slave_closes) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  const int Duplicate = dup(Slave);
  CHECK(Duplicate >= 0);
  MakeRaw(Slave);
  CHECK(WritesText(Slave, "last"));
  close(Slave);
  // A duplicate keeps the slave open: no EOF yet.
  CHECK(PollEvents(Master, POLLIN, 1000) & POLLIN);
  char Buffer[16];
  SetNonblocking(Master);
  CHECK(ReadExact(Master, Buffer, 4, 2000) == 4 && !memcmp(Buffer, "last", 4));
  CHECK_ERRNO(read(Master, Buffer, sizeof(Buffer)), EAGAIN);
  CHECK(!(PollEvents(Master, POLLIN, 0) & POLLHUP));
  WritesText(Duplicate, "tail");
  close(Duplicate);
  // Pending data is still read; then every read fails with EIO and poll reports a hangup.
  CHECK(PollEvents(Master, POLLIN, 1000) & POLLIN);
  CHECK(ReadExact(Master, Buffer, 4, 2000) == 4 && !memcmp(Buffer, "tail", 4));
  CHECK_ERRNO(read(Master, Buffer, sizeof(Buffer)), EIO);
  CHECK(PollEvents(Master, POLLIN, 0) & POLLHUP);
  fcntl(Master, F_SETFL, fcntl(Master, F_GETFL) & ~O_NONBLOCK);
  CHECK_ERRNO(read(Master, Buffer, sizeof(Buffer)), EIO);
  // Writes to the master still succeed. A new slave open makes the pair live again.
  CHECK(WritesText(Master, "z"));
  char Name[64];
  SlaveName(Master, Name, sizeof(Name));
  const int Again = open(Name, O_RDWR | O_NOCTTY | O_NONBLOCK);
  CHECK(Again >= 0);
  SetNonblocking(Master);
  CHECK_ERRNO(read(Master, Buffer, 1), EAGAIN);
}

TEST(pty, hangup_without_session) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  MakeRaw(Slave);
  CHECK(WritesText(Master, "pending"));
  CHECK(PollEvents(Slave, POLLIN, 1000) & POLLIN);
  close(Master);
  char Buffer[16];
  // After the master's close the slave is hung up: reads return 0, writes EIO.
  CHECK(read(Slave, Buffer, sizeof(Buffer)) == 0);
  CHECK_ERRNO(write(Slave, "x", 1), EIO);
  const int Events = PollEvents(Slave, POLLIN | POLLOUT, 0);
  CHECK((Events & POLLHUP) && (Events & POLLIN));
  CHECK_ERRNO(tcgetattr(Slave, &(struct termios) {0}), EIO);
  CHECK_ERRNO(ioctl(Slave, TIOCGWINSZ, &(struct winsize) {0}), EIO);
  CHECK_ERRNO(ioctl(Slave, TIOCSPGRP, &(int) {1}), ENOTTY);
}

TEST(pty, poll_select_epoll_both_ends) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  MakeRaw(Slave);
  CHECK(PollEvents(Slave, POLLIN | POLLOUT, 0) == POLLOUT);
  CHECK(PollEvents(Master, POLLIN | POLLOUT, 0) == POLLOUT);
  CHECK(WritesText(Master, "m"));
  CHECK(PollEvents(Slave, POLLIN | POLLOUT, 1000) == (POLLIN | POLLOUT));
  CHECK(WritesText(Slave, "s"));
  CHECK(PollEvents(Master, POLLIN | POLLOUT, 1000) == (POLLIN | POLLOUT));
  // select: both readable, both writable.
  fd_set Read, Write;
  FD_ZERO(&Read);
  FD_ZERO(&Write);
  FD_SET(Master, &Read);
  FD_SET(Slave, &Read);
  FD_SET(Master, &Write);
  struct timeval Zero = {0, 0};
  const int Max = (Master > Slave ? Master : Slave) + 1;
  CHECK(select(Max, &Read, &Write, NULL, &Zero) == 3);
  CHECK(FD_ISSET(Master, &Read) && FD_ISSET(Slave, &Read) && FD_ISSET(Master, &Write));
  char Buffer[8];
  CHECK(read(Master, Buffer, 1) == 1 && read(Slave, Buffer, 1) == 1);
  // epoll: edge and level on both ends.
  const int Epoll = epoll_create1(0);
  struct epoll_event Event = {.events = EPOLLIN, .data.fd = Slave};
  CHECK(epoll_ctl(Epoll, EPOLL_CTL_ADD, Slave, &Event) == 0);
  Event.data.fd = Master;
  CHECK(epoll_ctl(Epoll, EPOLL_CTL_ADD, Master, &Event) == 0);
  struct epoll_event Ready[4];
  CHECK(epoll_wait(Epoll, Ready, 4, 50) == 0);
  const pid_t Writer = fork();
  if (Writer == 0) {
    poll(NULL, 0, 100);
    (void)!write(Master, "1", 1);
    _exit(0);
  }
  CHECK(epoll_wait(Epoll, Ready, 4, 2000) == 1 && Ready[0].data.fd == Slave);
  WaitExit(Writer);
  CHECK(read(Slave, Buffer, 1) == 1);
  const pid_t Other = fork();
  if (Other == 0) {
    poll(NULL, 0, 100);
    (void)!write(Slave, "2", 1);
    _exit(0);
  }
  CHECK(epoll_wait(Epoll, Ready, 4, 2000) == 1 && Ready[0].data.fd == Master);
  WaitExit(Other);
  CHECK(read(Master, Buffer, 1) == 1);
  // A blocked poll wakes for the master's hang-up when the slave closes.
  close(Slave);
  CHECK(PollEvents(Master, POLLIN, 1000) & POLLHUP);
  close(Epoll);
}

TEST(pty, large_transfers_in_both_directions) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  MakeRaw(Slave);
  struct termios Attr;
  tcgetattr(Slave, &Attr);
  Attr.c_oflag &= ~OPOST;
  tcsetattr(Slave, TCSANOW, &Attr);
  enum { Total = 200000 };
  const pid_t Writer = fork();
  if (Writer == 0) {
    char Chunk[1000];
    for (int Offset = 0; Offset < Total; Offset += (int)sizeof(Chunk)) {
      for (size_t Index = 0; Index < sizeof(Chunk); ++Index) Chunk[Index] = (char)('a' + (Offset + (int)Index) % 26);
      for (size_t Done = 0; Done < sizeof(Chunk);) {
        const ssize_t Count = write(Slave, Chunk + Done, sizeof(Chunk) - Done);
        if (Count <= 0) _exit(1);
        Done += (size_t)Count;
      }
    }
    _exit(0);
  }
  char Buffer[4096];
  int Received = 0, Bad = 0;
  while (Received < Total) {
    const ssize_t Count = read(Master, Buffer, sizeof(Buffer));
    if (Count <= 0) break;
    for (ssize_t Index = 0; Index < Count; ++Index) Bad += Buffer[Index] != (char)('a' + (Received + Index) % 26);
    Received += (int)Count;
  }
  CHECK(Received == Total && !Bad);
  CHECK(WaitExit(Writer) == 0);
  // The other direction: raw input larger than the line buffer, read by a concurrent reader.
  const pid_t Reader = fork();
  if (Reader == 0) {
    int Got = 0;
    while (Got < 20000) {
      const ssize_t Count = read(Slave, Buffer, sizeof(Buffer));
      if (Count <= 0) _exit(1);
      Got += (int)Count;
    }
    _exit(0);
  }
  tcgetattr(Master, &Attr);
  Attr.c_lflag &= ~ECHO;
  tcsetattr(Master, TCSANOW, &Attr);
  char Chunk[2000];
  memset(Chunk, 'x', sizeof(Chunk));
  for (int Sent = 0; Sent < 20000;) {
    const ssize_t Count = write(Master, Chunk, sizeof(Chunk));
    CHECK(Count > 0);
    if (Count <= 0) break;
    Sent += (int)Count;
  }
  CHECK(WaitExit(Reader) == 0);
}

// ---------------------------------------------------------------------------
// Controlling terminals and job control

static void ChildOpensSlave(int Slave) {
  (void)Slave;
  // Without a controlling terminal /dev/tty does not open.
  CHECK_ERRNO(open("/dev/tty", O_RDWR), ENXIO);
  int Group = 0;
  CHECK_ERRNO(ioctl(SessionSlave, TIOCGPGRP, &Group), ENOTTY);
  CHECK_ERRNO(ioctl(SessionSlave, TIOCGSID, &Group), ENOTTY);
  CHECK(ioctl(SessionSlave, TIOCSCTTY, 0) == 0);
  CHECK(ioctl(SessionSlave, TIOCGPGRP, &Group) == 0 && Group == getpgrp());
  CHECK(ioctl(SessionSlave, TIOCGSID, &Group) == 0 && Group == getsid(0));
  CHECK(ioctl(SessionSlave, TIOCSCTTY, 0) == 0); // already ours
  const int Tty = open("/dev/tty", O_RDWR);
  CHECK(Tty >= 0);
  struct stat Info;
  CHECK(fstat(Tty, &Info) == 0 && S_ISCHR(Info.st_mode) && major(Info.st_rdev) == 5 && minor(Info.st_rdev) == 0);
  CHECK(write(Tty, "viatty", 6) == 6);
  Tell('w');
  _exit(CaseFailures ? 1 : 0);
}

TEST(pty, controlling_terminal_tiocsctty_and_dev_tty) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  MakeRaw(Slave);
  const pid_t Child = StartSession(Master, Slave, ChildOpensSlave);
  CHECK(Child > 0);
  CHECK(Hear(Up[0], 3000) == 'w');
  CHECK(ReadsText(Master, "viatty"));
  CHECK(ExitCode(Child) == 0);
}

static int OpenRulesMaster;

static void ChildNoctty(int Slave) {
  (void)Slave;
  char Name[64];
  SlaveName(OpenRulesMaster, Name, sizeof(Name));
  // O_NOCTTY: no controlling terminal. A write-only open does not acquire one either.
  int Fd = open(Name, O_RDWR | O_NOCTTY);
  CHECK(Fd >= 0);
  CHECK_ERRNO(open("/dev/tty", O_RDWR), ENXIO);
  close(Fd);
  Fd = open(Name, O_WRONLY);
  CHECK(Fd >= 0);
  CHECK_ERRNO(open("/dev/tty", O_RDWR), ENXIO);
  close(Fd);
  // Opening the master does not acquire a terminal.
  Fd = open("/dev/ptmx", O_RDWR);
  CHECK(Fd >= 0);
  CHECK_ERRNO(open("/dev/tty", O_RDWR), ENXIO);
  close(Fd);
  // Without O_NOCTTY the first read-open of the slave acquires it.
  Fd = open(Name, O_RDWR);
  CHECK(Fd >= 0);
  const int Tty = open("/dev/tty", O_RDWR);
  CHECK(Tty >= 0);
  int Group = 0;
  CHECK(ioctl(Fd, TIOCGPGRP, &Group) == 0 && Group == getpgrp());
  CHECK(ioctl(Fd, TIOCGSID, &Group) == 0 && Group == getsid(0));
  // A second open of the same terminal changes nothing. TIOCNOTTY by the leader hangs up its group.
  Tell('a');
  _exit(CaseFailures ? 1 : 0);
}

TEST(pty, controlling_terminal_open_rules) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  OpenRulesMaster = Master;
  const pid_t Child = StartSession(-1, Slave, ChildNoctty);
  CHECK(Child > 0);
  CHECK(Hear(Up[0], 3000) == 'a');
  CHECK(ExitCode(Child) == 0);
}

static void ChildFirstSession(int Slave) {
  CHECK(ioctl(Slave, TIOCSCTTY, 0) == 0);
  Tell('1');
  Hear(Down[0], 5000);
  _exit(CaseFailures ? 1 : 0);
}

static int SecondSlave;
static void SecondSessionMain(void) {
  setsid();
  // The terminal belongs to another session: EPERM, even when stealing is asked for without privilege.
  CHECK_ERRNO(ioctl(SecondSlave, TIOCSCTTY, 0), EPERM);
  if (!IsInitialRoot()) {
    CHECK_ERRNO(ioctl(SecondSlave, TIOCSCTTY, 1), EPERM);
  } else {
    CHECK(ioctl(SecondSlave, TIOCSCTTY, 1) == 0);
    int Group = 0;
    CHECK(ioctl(SecondSlave, TIOCGPGRP, &Group) == 0 && Group == getpgrp());
  }
}

TEST(pty, controlling_terminal_second_session_eperm) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  const pid_t First = StartSession(Master, Slave, ChildFirstSession);
  CHECK(First > 0);
  CHECK(Hear(Up[0], 3000) == '1');
  SecondSlave = Slave;
  const pid_t Second = Fork(SecondSessionMain);
  CHECK(ExitCode(Second) == 0);
  TellDown('x');
  CHECK(ExitCode(First) == 0);
}

static void NonLeaderMain(void) {
  CHECK_ERRNO(ioctl(SecondSlave, TIOCSCTTY, 0), EPERM);
  int Group;
  CHECK_ERRNO(ioctl(SecondSlave, TIOCGPGRP, &Group), ENOTTY);
}

TEST(pty, controlling_terminal_non_leader) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  SecondSlave = Slave;
  const pid_t Child = Fork(NonLeaderMain);
  CHECK(ExitCode(Child) == 0);
}

static void ChildNoctty2(int Slave) {
  CHECK(ioctl(Slave, TIOCSCTTY, 0) == 0);
  int Group;
  CHECK(ioctl(Slave, TIOCGPGRP, &Group) == 0);
  // The session leader's TIOCNOTTY hangs up its own group (SIGHUP, then SIGCONT).
  Handle(SIGHUP);
  Handle(SIGCONT);
  CHECK(ioctl(Slave, TIOCNOTTY, 0) == 0);
  CHECK(WaitCount(&GotHup, 1, 2000));
  CHECK(GotCont >= 1);
  CHECK_ERRNO(open("/dev/tty", O_RDWR), ENXIO);
  CHECK_ERRNO(ioctl(Slave, TIOCGPGRP, &Group), ENOTTY);
  // And it can acquire a terminal again.
  CHECK(ioctl(Slave, TIOCSCTTY, 0) == 0);
  CHECK(ioctl(Slave, TIOCGPGRP, &Group) == 0 && Group == getpgrp());
  _exit(CaseFailures ? 1 : 0);
}

TEST(pty, tiocnotty_leader) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  const pid_t Child = StartSession(Master, Slave, ChildNoctty2);
  CHECK(ExitCode(Child) == 0);
}

// The session child: signal counters are checked against what the master typed.
static void ChildSignals(int Slave) {
  CHECK(ioctl(Slave, TIOCSCTTY, 0) == 0);
  Handle(SIGINT);
  Handle(SIGQUIT);
  Handle(SIGTSTP);
  Tell('r');
  // Each signal is acknowledged before the master types the next: standard signals do not queue, so two ^C pending
  // at once are one SIGINT (Linux too, when the child has not run in between, e.g. on one CPU).
  CHECK(Hear(Down[0], 5000) == 'i');
  CHECK(WaitCount(&GotInt, 1, 3000));
  Tell('i');
  CHECK(Hear(Down[0], 5000) == 'q');
  CHECK(WaitCount(&GotQuit, 1, 3000));
  Tell('q');
  CHECK(Hear(Down[0], 5000) == 't');
  CHECK(WaitCount(&GotTstp, 1, 3000));
  Tell('t');
  // The signal characters flush pending input, unless NOFLSH is set.
  CHECK(Hear(Down[0], 5000) == 'f');
  CHECK(WaitCount(&GotInt, 2, 3000));
  char Buffer[16];
  SetNonblocking(Slave);
  CHECK_ERRNO(read(Slave, Buffer, sizeof(Buffer)), EAGAIN);
  Tell('f');
  // A changed VINTR, then ISIG off: the byte is data.
  CHECK(Hear(Down[0], 5000) == 'v');
  CHECK(WaitCount(&GotInt, 3, 3000));
  CHECK(Hear(Down[0], 5000) == 'n');
  poll(NULL, 0, 100);
  CHECK(GotInt == 3);
  CHECK(read(Slave, Buffer, sizeof(Buffer)) == 1 && Buffer[0] == 3);
  _exit(CaseFailures ? 1 : 0);
}

TEST(pty, isig_signals_to_foreground_group) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  const pid_t Child = StartSession(Master, Slave, ChildSignals);
  CHECK(Child > 0);
  CHECK(Hear(Up[0], 3000) == 'r');
  CHECK(WritesText(Master, "\x03"));
  CHECK(ReadsText(Master, "^C"));
  TellDown('i');
  CHECK(Hear(Up[0], 5000) == 'i');
  CHECK(WritesText(Master, "\x1c"));
  CHECK(ReadsText(Master, "^\\"));
  TellDown('q');
  CHECK(Hear(Up[0], 5000) == 'q');
  CHECK(WritesText(Master, "\x1a"));
  CHECK(ReadsText(Master, "^Z"));
  TellDown('t');
  CHECK(Hear(Up[0], 5000) == 't');
  // Typed text, then ^C: the unread input is flushed.
  CHECK(WritesText(Master, "typed\n"));
  CHECK(ReadsText(Master, "typed\r\n"));
  CHECK(WritesText(Master, "\x03"));
  CHECK(ReadsText(Master, "^C"));
  TellDown('f');
  CHECK(Hear(Up[0], 3000) == 'f');
  // VINTR becomes 'x'; the default ^C is then plain data.
  struct termios Attr;
  CHECK(tcgetattr(Master, &Attr) == 0);
  Attr.c_cc[VINTR] = 'x';
  CHECK(tcsetattr(Master, TCSANOW, &Attr) == 0);
  CHECK(WritesText(Master, "x"));
  poll(NULL, 0, 50);
  TellDown('v');
  poll(NULL, 0, 150);
  CHECK(ioctl(Master, TCGETS, &Attr) == 0);
  Attr.c_cc[VINTR] = 3;
  Attr.c_lflag &= ~(ISIG | ICANON | ECHO);
  CHECK(tcsetattr(Master, TCSANOW, &Attr) == 0);
  CHECK(WritesText(Master, "\x03"));
  TellDown('n');
  CHECK(ExitCode(Child) == 0);
}

// The session leader makes a second group; the second group is a background group.
static void GroupMemberMain(void) {
  setpgid(0, 0);
  alarm(8);
  char Buffer[8];
  const int Tty = open("/dev/tty", O_RDWR);
  CHECK(Tty >= 0);
  (void)!write(Peer[1], "m", 1);
  // A background read stops with SIGTTIN; after SIGCONT and tcsetpgrp the read goes on.
  const ssize_t Count = read(Tty, Buffer, sizeof(Buffer));
  CHECK(Count == 3 && !memcmp(Buffer, "go\n", 3));
  Tell('d');
  // A background write with TOSTOP: SIGTTOU. Here the group is in the foreground, so it works.
  CHECK(write(Tty, "ok", 2) == 2);
  _exit(CaseFailures ? 1 : 0);
}

static void ChildJobControl(int Slave) {
  CHECK(ioctl(Slave, TIOCSCTTY, 0) == 0);
  const pid_t Member = fork();
  if (Member == 0) {
    close(Up[0]);
    GroupMemberMain();
  }
  setpgid(Member, Member);
  CHECK(Hear(Peer[0], 3000) == 'm');
  int Status = 0;
  CHECK(waitpid(Member, &Status, WUNTRACED) == Member);
  CHECK(WIFSTOPPED(Status) && WSTOPSIG(Status) == SIGTTIN);
  // The leader's group is orphaned (its parent is outside the session) and in the foreground: TIOCSPGRP works.
  CHECK(tcsetpgrp(Slave, Member) == 0);
  int Group = 0;
  CHECK(ioctl(Slave, TIOCGPGRP, &Group) == 0 && Group == Member);
  CHECK(kill(Member, SIGCONT) == 0);
  Tell('s');
  CHECK(waitpid(Member, &Status, 0) == Member);
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  _exit(CaseFailures ? 1 : 0);
}

TEST(pty, background_read_sigttin_and_tiocspgrp) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  const pid_t Child = StartSession(Master, Slave, ChildJobControl);
  CHECK(Child > 0);
  CHECK(Hear(Up[0], 5000) == 's');
  CHECK(WritesText(Master, "go\n"));
  CHECK(ReadsText(Master, "go\r\n"));
  CHECK(Hear(Up[0], 3000) == 'd');
  CHECK(ReadsText(Master, "ok"));
  CHECK(ExitCode(Child) == 0);
}

static void TtouMemberMain(void) {
  setpgid(0, 0);
  alarm(8);
  const int Tty = open("/dev/tty", O_RDWR);
  CHECK(Tty >= 0);
  (void)!write(Peer[1], "m", 1);
  // TOSTOP is set: a background write stops with SIGTTOU and, once the group is in the foreground, completes.
  CHECK(write(Tty, "late", 4) == 4);
  (void)!write(Peer[1], "w", 1);
  // The leader took the terminal back. With SIGTTOU ignored a background tcsetattr succeeds; with SIGTTIN
  // ignored a background read fails with EIO.
  CHECK(Hear(Down[0], 4000) == 'b');
  Ignore(SIGTTOU);
  Ignore(SIGTTIN);
  struct termios Attr;
  CHECK(tcgetattr(Tty, &Attr) == 0);
  CHECK(tcsetattr(Tty, TCSANOW, &Attr) == 0);
  CHECK(write(Tty, "bg", 2) == 2);
  char Buffer[4];
  CHECK_ERRNO(read(Tty, Buffer, sizeof(Buffer)), EIO);
  _exit(CaseFailures ? 1 : 0);
}

static void ChildTtou(int Slave) {
  CHECK(ioctl(Slave, TIOCSCTTY, 0) == 0);
  struct termios Attr;
  tcgetattr(Slave, &Attr);
  Attr.c_lflag |= TOSTOP;
  tcsetattr(Slave, TCSANOW, &Attr);
  const pid_t Member = fork();
  if (Member == 0) TtouMemberMain();
  setpgid(Member, Member);
  CHECK(Hear(Peer[0], 3000) == 'm');
  int Status = 0;
  CHECK(waitpid(Member, &Status, WUNTRACED) == Member);
  CHECK(WIFSTOPPED(Status) && WSTOPSIG(Status) == SIGTTOU);
  CHECK(tcsetpgrp(Slave, Member) == 0);
  CHECK(kill(Member, SIGCONT) == 0);
  CHECK(Hear(Peer[0], 3000) == 'w');
  Ignore(SIGTTOU);
  CHECK(tcsetpgrp(Slave, getpgrp()) == 0); // from a background group: allowed with SIGTTOU ignored
  TellDown('b');
  CHECK(waitpid(Member, &Status, 0) == Member);
  CHECK(WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  CHECK(write(Slave, "lead", 4) == 4);
  _exit(CaseFailures ? 1 : 0);
}

TEST(pty, background_write_tostop_and_ignored_signals) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  const pid_t Child = StartSession(Master, Slave, ChildTtou);
  CHECK(Child > 0);
  CHECK(ExitCode(Child) == 0);
  CHECK(ReadsText(Master, "latebglead"));
}

static void ChildOrphan(int Slave) {
  CHECK(ioctl(Slave, TIOCSCTTY, 0) == 0);
  const pid_t Member = fork();
  if (Member == 0) {
    setpgid(0, 0);
    alarm(8);
    pause();
    _exit(0);
  }
  setpgid(Member, Member);
  CHECK(tcsetpgrp(Slave, Member) == 0);
  // The leader is now in a background group, and its group is orphaned: reading fails with EIO, no signal.
  char Buffer[4];
  CHECK_ERRNO(read(Slave, Buffer, sizeof(Buffer)), EIO);
  kill(Member, SIGKILL);
  waitpid(Member, NULL, 0);
  _exit(CaseFailures ? 1 : 0);
}

TEST(pty, background_read_in_orphaned_group_eio) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  const pid_t Child = StartSession(Master, Slave, ChildOrphan);
  CHECK(ExitCode(Child) == 0);
}

static void ChildTiocspgrpErrors(int Slave) {
  CHECK(ioctl(Slave, TIOCSCTTY, 0) == 0);
  int Group = getpgrp();
  CHECK(ioctl(Slave, TIOCSPGRP, &Group) == 0);
  Group = 999999;
  CHECK_ERRNO(ioctl(Slave, TIOCSPGRP, &Group), ESRCH);
  Group = -5;
  CHECK_ERRNO(ioctl(Slave, TIOCSPGRP, &Group), EINVAL);
  Group = 1; // exists but is in another session
  CHECK_ERRNO(ioctl(Slave, TIOCSPGRP, &Group), EPERM);
  CHECK_ERRNO(ioctl(Slave, TIOCSPGRP, (void *)8), EFAULT);
  _exit(CaseFailures ? 1 : 0);
}

TEST(pty, tiocspgrp_errors) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  const pid_t Child = StartSession(Master, Slave, ChildTiocspgrpErrors);
  CHECK(ExitCode(Child) == 0);
}

static void ChildSuspend(int Slave) {
  CHECK(ioctl(Slave, TIOCSCTTY, 0) == 0);
  const pid_t Member = fork();
  if (Member == 0) {
    setpgid(0, 0);
    alarm(8);
    for (;;) pause();
  }
  setpgid(Member, Member);
  CHECK(tcsetpgrp(Slave, Member) == 0);
  Tell('g');
  int Status = 0;
  // ^Z from the master stops the foreground group.
  CHECK(waitpid(Member, &Status, WUNTRACED) == Member);
  CHECK(WIFSTOPPED(Status) && WSTOPSIG(Status) == SIGTSTP);
  CHECK(kill(Member, SIGKILL) == 0);
  CHECK(waitpid(Member, &Status, 0) == Member && WIFSIGNALED(Status) && WTERMSIG(Status) == SIGKILL);
  _exit(CaseFailures ? 1 : 0);
}

TEST(pty, vsusp_stops_foreground_group) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  const pid_t Child = StartSession(Master, Slave, ChildSuspend);
  CHECK(Hear(Up[0], 3000) == 'g');
  poll(NULL, 0, 100);
  CHECK(WritesText(Master, "\x1a"));
  CHECK(ReadsText(Master, "^Z"));
  CHECK(ExitCode(Child) == 0);
}

static void ChildWinch(int Slave) {
  CHECK(ioctl(Slave, TIOCSCTTY, 0) == 0);
  Handle(SIGWINCH);
  Tell('r');
  CHECK(WaitCount(&GotWinch, 1, 3000));
  Tell('1');
  // Setting the same size again sends nothing; a changed size from the slave side signals too.
  CHECK(Hear(Down[0], 3000) == 'a');
  poll(NULL, 0, 100);
  CHECK(GotWinch == 1);
  struct winsize Size = {30, 100, 0, 0};
  CHECK(ioctl(Slave, TIOCSWINSZ, &Size) == 0);
  CHECK(WaitCount(&GotWinch, 2, 3000));
  _exit(CaseFailures ? 1 : 0);
}

TEST(pty, sigwinch_on_size_change) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  const pid_t Child = StartSession(Master, Slave, ChildWinch);
  CHECK(Hear(Up[0], 3000) == 'r');
  struct winsize Size = {24, 80, 0, 0};
  CHECK(ioctl(Master, TIOCSWINSZ, &Size) == 0);
  CHECK(Hear(Up[0], 3000) == '1');
  CHECK(ioctl(Master, TIOCSWINSZ, &Size) == 0);
  TellDown('a');
  CHECK(ExitCode(Child) == 0);
}

static void ChildHangup(int Slave) {
  CHECK(ioctl(Slave, TIOCSCTTY, 0) == 0);
  Handle(SIGHUP);
  Handle(SIGCONT);
  // Only the session leader is signalled by the hang-up; a member of the foreground group is not
  // (the leader's own exit or TIOCNOTTY signals the group).
  const pid_t Member = fork();
  if (Member == 0) {
    Handle(SIGHUP);
    (void)!write(Peer[1], "m", 1);
    poll(NULL, 0, 600);
    CHECK(GotHup == 0);
    _exit(CaseFailures ? 1 : 0);
  }
  CHECK(Hear(Peer[0], 3000) == 'm');
  Tell('r');
  CHECK(WaitCount(&GotHup, 1, 4000));
  CHECK(GotCont >= 1);
  char Buffer[8];
  CHECK(read(Slave, Buffer, sizeof(Buffer)) == 0);
  CHECK_ERRNO(write(Slave, "x", 1), EIO);
  CHECK(PollEvents(Slave, POLLIN, 0) & POLLHUP);
  // The session lost its terminal.
  CHECK_ERRNO(open("/dev/tty", O_RDWR), ENXIO);
  int Group = 0;
  CHECK_ERRNO(ioctl(Slave, TIOCGPGRP, &Group), EIO);
  int Status = 0;
  CHECK(waitpid(Member, &Status, 0) == Member && WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  _exit(CaseFailures ? 1 : 0);
}

TEST(pty, master_close_hangs_up_session) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  const pid_t Child = StartSession(Master, Slave, ChildHangup);
  CHECK(Child > 0);
  CHECK(Hear(Up[0], 3000) == 'r');
  close(Slave);
  close(Master);
  CHECK(ExitCode(Child) == 0);
}

static void ChildLeaderExit(int Slave) {
  CHECK(ioctl(Slave, TIOCSCTTY, 0) == 0);
  const pid_t Member = fork();
  if (Member == 0) {
    setpgid(0, 0);
    Handle(SIGHUP);
    (void)!write(Peer[1], "m", 1);
    CHECK(WaitCount(&GotHup, 1, 4000));
    _exit(CaseFailures ? 1 : 0);
  }
  setpgid(Member, Member);
  CHECK(tcsetpgrp(Slave, Member) == 0);
  CHECK(Hear(Peer[0], 3000) == 'm');
  Tell('x');
  int Status = 0;
  // The leader leaves: the foreground group gets SIGHUP.
  if (fork() == 0) {
    waitpid(Member, &Status, 0);
    _exit(WIFEXITED(Status) && WEXITSTATUS(Status) == 0 ? 0 : 1);
  }
  _exit(0);
}

TEST(pty, session_leader_exit_hangs_up_foreground_group) {
  int Master, Slave;
  CHECK(OpenPair(&Master, &Slave, 0) == 0);
  const pid_t Child = StartSession(Master, Slave, ChildLeaderExit);
  CHECK(Hear(Up[0], 3000) == 'x');
  CHECK(ExitCode(Child) == 0);
  // The terminal is free again for another session.
  CHECK(PollEvents(Master, POLLIN | POLLOUT, 0) & POLLOUT);
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
  snprintf(Scratch, sizeof(Scratch), "%s/pty-suite.XXXXXX", Location);
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
  printf("TAP version 13\n1..%u\n# pty-suite uid %u, gid %u\n", Selection, getuid(), getgid());
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
  printf("# pty-suite summary: %u/%u/%u (passed/failed/skipped)\n", Passed, Failed, SkippedCount);
  // final-pass-suite collects the counts through this file.
  const char* ResultsPath = getenv("FEX_SUITE_RESULTS");
  // The runner creates the file; opening it without O_CREAT also works in a sticky /tmp (protected_regular).
  const int Results = ResultsPath ? open(ResultsPath, O_WRONLY | O_APPEND | O_CLOEXEC) : -1;
  if (Results >= 0) {
    dprintf(Results, "pty-suite %u %u %u\n", Passed, Failed, SkippedCount);
    close(Results);
  }
  char Remove[PATH_MAX + 16];
  snprintf(Remove, sizeof(Remove), "rm -rf %s", Scratch);
  if (chdir("..") == 0 && system(Remove) != 0) printf("# could not remove %s\n", Scratch);
  return Failed ? 1 : 42;
}

