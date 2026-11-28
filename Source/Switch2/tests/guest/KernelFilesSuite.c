// SPDX-License-Identifier: MIT
// Kernel file compatibility suite. See docs/KernelFilesInventory.md.
// Test isolation, filters and result reporting follow ProcSuite.c.
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/capability.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

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

static void SkipTest(const char *Reason) {
  snprintf(Shared->SkipReason, sizeof(Shared->SkipReason), "%s", Reason);
  Skipped = 1;
}

// Forks a helper that runs Child and exits 0, or 1 if its CHECKs failed.
// Returns the pid in the parent, -1 on failure.
static pid_t Fork(void (*Child)(void)) {
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
static int WaitExit(pid_t Pid) {
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
static uint64_t Deadline(unsigned Milliseconds) { return NowMs() + Milliseconds; }
static int DeadlineExpired(uint64_t Limit) { return NowMs() >= Limit; }

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
  struct utsname Name;
  GuestRuntime = uname(&Name) == 0 && strstr(Name.version, "FEX Switch 2") != NULL;
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
  snprintf(Scratch, sizeof(Scratch), "%s/kernel-files-suite.XXXXXX", Location);
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
  printf("TAP version 13\n1..%u\n# kernel-files-suite uid %u, gid %u\n", Selection, getuid(), getgid());
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
  printf("# kernel-files-suite summary: %u/%u/%u (passed/failed/skipped)\n", Passed, Failed, SkippedCount);
  // final-pass-suite collects the counts through this file.
  const char* ResultsPath = getenv("FEX_SUITE_RESULTS");
  // The runner creates the file; opening it without O_CREAT also works in a sticky /tmp (protected_regular).
  const int Results = ResultsPath ? open(ResultsPath, O_WRONLY | O_APPEND | O_CLOEXEC) : -1;
  if (Results >= 0) {
    dprintf(Results, "kernel-files-suite %u %u %u\n", Passed, Failed, SkippedCount);
    close(Results);
  }
  if (chdir("..") == 0 && rmdir(Scratch) != 0) printf("# could not remove %s\n", Scratch);
  return Failed ? 1 : 42;
}

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

static ssize_t ReadText(const char *Path, char *Text, size_t Size) {
  Text[0] = 0;
  const int Fd = open(Path, O_RDONLY | O_CLOEXEC);
  if (Fd < 0) return -1;
  size_t Used = 0;
  ssize_t Length;
  while ((Length = read(Fd, Text + Used, Size - Used - 1)) > 0) {
    Used += Length;
    if (Used == Size - 1) {
      char Extra;
      Length = read(Fd, &Extra, 1);
      if (Length > 0) {
        errno = EOVERFLOW;
        Length = -1;
      }
      break;
    }
  }
  const int Error = errno;
  close(Fd);
  Text[Used] = 0;
  errno = Error;
  return Length < 0 ? -1 : (ssize_t)Used;
}

static int WriteText(const char *Path, const char *Text) {
  const int Fd = open(Path, O_WRONLY | O_CLOEXEC);
  if (Fd < 0) return -1;
  const ssize_t Length = write(Fd, Text, strlen(Text));
  const int Error = Length < 0 ? errno : EIO;
  close(Fd);
  if (Length == (ssize_t)strlen(Text)) return 0;
  errno = Error;
  return -1;
}

static const char *Field(const char *Text, const char *Name) {
  const size_t Length = strlen(Name);
  for (const char *Line = Text; Line; ) {
    if (!strncmp(Line, Name, Length) && Line[Length] == ':') return Line + Length + 1;
    Line = strchr(Line, '\n');
    if (Line) ++Line;
  }
  return "";
}

static long long NumericField(const char *Text, const char *Name, const char *Unit) {
  long long Value;
  int End = 0;
  const char *FieldText = Field(Text, Name);
  if (sscanf(FieldText, "%lld%n", &Value, &End) != 1 || Value < 0) return -1;
  while (FieldText[End] == ' ' || FieldText[End] == '\t') ++End;
  const size_t UnitLength = strlen(Unit);
  return !strncmp(FieldText + End, Unit, UnitLength) && FieldText[End + UnitLength] == '\n' ? Value : -1;
}

struct Map {
  unsigned long Start, End, Offset, Inode;
  unsigned Major, Minor;
  char Perms[5];
};

static const char *ParseMap(const char *Line, struct Map *Map) {
  int Pos = 0;
  if (sscanf(Line, "%lx-%lx %4s %lx %x:%x %lu%n", &Map->Start, &Map->End, Map->Perms,
             &Map->Offset, &Map->Major, &Map->Minor, &Map->Inode, &Pos) != 7) return NULL;
  while (Line[Pos] == ' ' || Line[Pos] == '\t') ++Pos;
  return Line + Pos;
}

static int CheckMap(const char *Text, unsigned long Start, size_t Size,
                    const char *Perms, unsigned long Offset, const char *Name) {
  const size_t NameLength = Name ? strlen(Name) : 0;
  for (const char *Line = Text; *Line; ) {
    struct Map Map;
    const char *Actual = ParseMap(Line, &Map);
    if (!Actual) return 0;
    if (Map.Start <= Start && Start < Map.End) {
      return Size && Map.Start == Start && Map.End == Start + Size &&
             !strcmp(Map.Perms, Perms) && Map.Offset == Offset &&
             !strncmp(Actual, Name, NameLength) && Actual[NameLength] == '\n';
    }
    Line = strchr(Line, '\n');
    if (!Line) return 0;
    ++Line;
  }
  return !Size;
}

TEST(maps, ranges_and_offsets) {
  char Text[32768], FilePath[PATH_MAX];
  CHECK(getcwd(FilePath, sizeof(FilePath) - sizeof("/mapping")) != NULL);
  if (CaseFailures) return;
  strcat(FilePath, "/mapping");
  const size_t Page = sysconf(_SC_PAGESIZE);
  const int File = open("mapping", O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(File >= 0);
  if (File < 0) return;
  CHECK(ftruncate(File, 3 * Page) == 0);

  // Guard pages keep the VMAs from merging with unrelated mappings.
  char *Base = mmap(NULL, 10 * Page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(Base != MAP_FAILED);
  if (Base != MAP_FAILED) {
    char *Private = Base + Page, *MappedFile = Base + 5 * Page, *Shared = Base + 8 * Page;
    CHECK(mmap(Private, 3 * Page, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == Private);
    CHECK(mmap(MappedFile, 2 * Page, PROT_READ, MAP_FIXED | MAP_PRIVATE, File, Page) == MappedFile);
    CHECK(mmap(Shared, Page, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED | MAP_ANONYMOUS, -1, 0) == Shared);
    CHECK(ReadText("/proc/self/maps", Text, sizeof(Text)) > 0);
    CHECK(CheckMap(Text, (uintptr_t)Private, 3 * Page, "rw-p", 0, ""));
    CHECK(CheckMap(Text, (uintptr_t)MappedFile, 2 * Page, "r--p", Page, FilePath));
    CHECK(CheckMap(Text, (uintptr_t)Shared, Page, "rw-s", 0, "/dev/zero (deleted)"));

    CHECK(mprotect(Private + Page, Page, PROT_READ) == 0);
    CHECK(ReadText("/proc/self/maps", Text, sizeof(Text)) > 0);
    CHECK(CheckMap(Text, (uintptr_t)Private, Page, "rw-p", 0, ""));
    CHECK(CheckMap(Text, (uintptr_t)Private + Page, Page, "r--p", 0, ""));
    CHECK(CheckMap(Text, (uintptr_t)Private + 2 * Page, Page, "rw-p", 0, ""));
    CHECK(munmap(Private + Page, Page) == 0);
    CHECK(ReadText("/proc/self/maps", Text, sizeof(Text)) > 0);
    CHECK(CheckMap(Text, (uintptr_t)Private + Page, 0, NULL, 0, NULL));
    CHECK(CheckMap(Text, (uintptr_t)Private, Page, "rw-p", 0, ""));
    CHECK(CheckMap(Text, (uintptr_t)Private + 2 * Page, Page, "rw-p", 0, ""));
    CHECK(munmap(Base, 10 * Page) == 0);
  }
  close(File);
  CHECK(unlink("mapping") == 0);
}

static unsigned long NamedMapEnd(char *Text, const char *Name) {
  unsigned long End = 0;
  for (char *Line = strtok(Text, "\n"); Line; Line = strtok(NULL, "\n")) {
    struct Map Map;
    const char *Actual = ParseMap(Line, &Map);
    if (Actual && !strcmp(Actual, Name) && Map.End > End) End = Map.End;
  }
  return End;
}

TEST(maps, heap_growth_and_stack) {
  char Text[32768];
  const long Page = sysconf(_SC_PAGESIZE);
  void *Before = sbrk(0);
  CHECK(Before != (void *)-1 && Page > 0);
  if (CaseFailures) return;
  // Establish a heap even with a libc that did not allocate one at startup.
  void *Small = (char *)Before + Page;
  void *Large = (char *)Small + 4 * Page;
  CHECK(brk(Small) == 0);
  CHECK(ReadText("/proc/self/maps", Text, sizeof(Text)) > 0 && strstr(Text, "[stack]\n"));
  const unsigned long SmallEnd = NamedMapEnd(Text, "[heap]");
  CHECK(brk(Large) == 0);
  CHECK(ReadText("/proc/self/maps", Text, sizeof(Text)) > 0);
  const unsigned long LargeEnd = NamedMapEnd(Text, "[heap]");
  CHECK(brk(Before) == 0);
  if (SmallEnd != ((uintptr_t)Small + Page - 1) / Page * Page || LargeEnd != SmallEnd + 4 * Page) {
    printf("#   brk %p -> %p -> %p, maps heap end %lx -> %lx\n", Before, Small, Large, SmallEnd, LargeEnd);
  }
  CHECK(SmallEnd == ((uintptr_t)Small + Page - 1) / Page * Page);
  CHECK(LargeEnd == SmallEnd + 4 * Page);
}

static int DropIds(uid_t Uid, gid_t Gid) {
  return setgroups(0, NULL) || setgid(Gid) || setuid(Uid);
}

static void CrossUidMapsReader(void) {
  char Path[64];
  snprintf(Path, sizeof(Path), "/proc/%d/maps", getppid());
  CHECK(DropIds(65534, 65534) == 0 && geteuid() == 65534);
  if (!CaseFailures) CHECK_ERRNO(open(Path, O_RDONLY), EACCES);
}

TEST(maps, cross_uid_denied) {
  if (geteuid() == 0) {
    if (!GuestRuntime) {
      char Map[256], Extra;
      unsigned Start, Outside, Count;
      CHECK(ReadText("/proc/self/uid_map", Map, sizeof(Map)) > 0);
      if (sscanf(Map, "%u %u %u %c", &Start, &Outside, &Count, &Extra) == 3 && Start == 0 && Count == 1)
        SKIP("only one UID mapped in the native user namespace");
    }
    const pid_t Child = Fork(CrossUidMapsReader);
    CHECK(Child > 0);
    if (Child > 0) CHECK(WaitExit(Child) == 0);
  } else {
    char Path[64];
    // The final-pass launcher is a live root ancestor. A standalone run may
    // only see same-UID processes, so look for a different visible owner.
    DIR *Proc = opendir("/proc");
    CHECK(Proc != NULL);
    if (!Proc) return;
    int Found = 0;
    for (struct dirent *Entry; (Entry = readdir(Proc));) {
      char *End;
      const long Pid = strtol(Entry->d_name, &End, 10);
      if (*End || Pid <= 0 || Pid > INT_MAX) continue;
      char Status[4096];
      snprintf(Path, sizeof(Path), "/proc/%ld/status", Pid);
      unsigned Uid;
      if (ReadText(Path, Status, sizeof(Status)) <= 0 ||
          sscanf(Field(Status, "Uid"), "%u", &Uid) != 1 || Uid == geteuid()) continue;
      // A kernel thread has no userspace mappings to protect.
      if (NumericField(Status, "VmSize", "kB") <= 0) continue;
      snprintf(Path, sizeof(Path), "/proc/%ld/maps", Pid);
      Found = 1;
      break;
    }
    closedir(Proc);
    if (!Found) SKIP("no visible userspace process with a different UID");
    CHECK_ERRNO(open(Path, O_RDONLY), EACCES);
  }
}

static int ReadSizeFields(FILE *File, char **Line, size_t *Capacity,
                          const char *const *Fields, size_t Count, long long *Rss) {
  for (size_t Index = 0; Index < Count; ++Index) {
    if (getline(Line, Capacity, File) < 0) return 0;
    // Newer Linux versions can insert fields into the 5.15 layout.
    if (!GuestRuntime && (!strncmp(*Line, "Pss_Dirty:", 10) || !strncmp(*Line, "KSM:", 4))) {
      --Index;
      continue;
    }
    const long long Value = NumericField(*Line, Fields[Index], "kB");
    if (Value < 0) {
      printf("#   expected %s, got %s", Fields[Index], *Line);
      return 0;
    }
    if (!strcmp(Fields[Index], "Rss")) *Rss = Value;
  }
  return 1;
}

static const char *const SmapsTotals[] = {
  "Shared_Clean", "Shared_Dirty", "Private_Clean", "Private_Dirty", "Referenced",
  "Anonymous", "LazyFree", "AnonHugePages", "ShmemPmdMapped", "FilePmdMapped",
  "Shared_Hugetlb", "Private_Hugetlb", "Swap", "SwapPss", "Locked"
};

TEST(smaps, private_range_and_rollup) {
  const size_t Page = sysconf(_SC_PAGESIZE);
  char *Base = mmap(NULL, 6 * Page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(Base != MAP_FAILED);
  if (Base == MAP_FAILED) return;
  volatile char *Touched = Base + Page;
  const size_t Size = 4 * Page;
  CHECK(mmap(Base + Page, Size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == Base + Page);
  if (CaseFailures) return;
  for (size_t Offset = 0; Offset < Size; Offset += Page) Touched[Offset] = 1;

  FILE *File = fopen("/proc/self/smaps", "r");
  CHECK(File != NULL);
  if (!File) return;
  char *Line = NULL;
  size_t Capacity = 0;
  struct Map Map;
  int Found = 0;
  while (getline(&Line, &Capacity, File) >= 0) {
    const char *Name = ParseMap(Line, &Map);
    if (!Name || Map.Start != (uintptr_t)Touched) continue;
    Found = 1;
    CHECK(Map.End == Map.Start + Size && !strcmp(Map.Perms, "rw-p") && Map.Offset == 0 && !strcmp(Name, "\n"));
    CHECK(getline(&Line, &Capacity, File) > 0 && NumericField(Line, "Size", "kB") == (long long)(Size / 1024));
    long long Rss = -1;
    const char *const Fields[] = {"KernelPageSize", "MMUPageSize", "Rss", "Pss"};
    CHECK(ReadSizeFields(File, &Line, &Capacity, Fields, COUNT(Fields), &Rss));
    CHECK(Rss >= (long long)(Size / 1024));
    CHECK(ReadSizeFields(File, &Line, &Capacity, SmapsTotals, COUNT(SmapsTotals), &Rss));
    CHECK(getline(&Line, &Capacity, File) > 0 && NumericField(Line, "THPeligible", "") >= 0);
    if (getline(&Line, &Capacity, File) > 0 && !GuestRuntime && !strncmp(Line, "ProtectionKey:", 14))
      CHECK(getline(&Line, &Capacity, File) > 0);
    CHECK(!strncmp(Line, "VmFlags:", 8));
    const char *const Flags[] = {"rd", "wr", "mr", "mw", "me", "ac"};
    unsigned Seen = 0;
    if (!strncmp(Line, "VmFlags:", 8)) {
      for (char *Flag = strtok(Line + 8, " \n\t"); Flag; Flag = strtok(NULL, " \n\t"))
        for (size_t Index = 0; Index < COUNT(Flags); ++Index)
          if (!strcmp(Flag, Flags[Index])) Seen |= 1u << Index;
    }
    CHECK(Seen == (1u << COUNT(Flags)) - 1);
    break;
  }
  CHECK(Found);
  fclose(File);

  File = fopen("/proc/self/smaps_rollup", "r");
  CHECK(File != NULL);
  if (File) {
    const char *Name = getline(&Line, &Capacity, File) > 0 ? ParseMap(Line, &Map) : NULL;
    CHECK(Name && !strcmp(Name, "[rollup]\n") && !strcmp(Map.Perms, "---p") &&
          Map.Start <= (uintptr_t)Touched && Map.End >= (uintptr_t)Touched + Size);
    long long Rss = -1;
    const char *const Fields[] = {"Rss", "Pss", "Pss_Anon", "Pss_File", "Pss_Shmem"};
    CHECK(ReadSizeFields(File, &Line, &Capacity, Fields, COUNT(Fields), &Rss));
    CHECK(Rss >= (long long)(Size / 1024));
    CHECK(ReadSizeFields(File, &Line, &Capacity, SmapsTotals, COUNT(SmapsTotals), &Rss));
    CHECK(getline(&Line, &Capacity, File) == -1 && feof(File));
    fclose(File);
  }
  free(Line);
  CHECK(munmap(Base, 6 * Page) == 0);
}

TEST(smaps, statm_matches_status) {
  char Status[4096], Statm[256], Extra;
  // Fault in the read buffers before comparing the independently sampled RSS.
  CHECK(ReadText("/proc/self/status", Status, sizeof(Status)) > 0);
  CHECK(ReadText("/proc/self/statm", Statm, sizeof(Statm)) > 0);
  CHECK(ReadText("/proc/self/status", Status, sizeof(Status)) > 0);
  long long Size, Rss, Shared, Text, Lib, Data, Dirty;
  const int Fields = sscanf(Statm, "%lld %lld %lld %lld %lld %lld %lld %c",
                            &Size, &Rss, &Shared, &Text, &Lib, &Data, &Dirty, &Extra);
  CHECK(Fields == 7);
  if (Fields != 7) return;
  const long PageKb = sysconf(_SC_PAGESIZE) / 1024;
  CHECK(Size > 0 && Rss >= 0 && Rss <= Size && Shared >= 0 && Shared <= Rss);
  CHECK(Text > 0 && Data > 0 && Lib == 0 && Dirty == 0);
  CHECK(Size * PageKb == NumericField(Status, "VmSize", "kB"));
  CHECK(Rss * PageKb == NumericField(Status, "VmRSS", "kB"));
}

TEST(mem, self_io_and_unmapped_address) {
  const size_t Page = sysconf(_SC_PAGESIZE);
  char *Base = mmap(NULL, 3 * Page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(Base != MAP_FAILED);
  if (Base == MAP_FAILED) return;
  volatile char *Byte = Base;
  *Byte = 'a';
  const int Fd = open("/proc/self/mem", O_RDWR);
  CHECK(Fd >= 0);
  if (Fd >= 0) {
    char Data = 0;
    CHECK(pread(Fd, &Data, 1, (uintptr_t)Byte) == 1 && Data == 'a');
    CHECK(pwrite(Fd, "b", 1, (uintptr_t)Byte) == 1 && *Byte == 'b');
    CHECK(munmap(Base + Page, Page) == 0);
    CHECK_ERRNO(pread(Fd, &Data, 1, (uintptr_t)Base + Page), EIO);
    CHECK_ERRNO(pwrite(Fd, "c", 1, (uintptr_t)Base + Page), EIO);
    close(Fd);
  }
  CHECK(munmap(Base, 3 * Page) == 0);
}

static void DeniedProcReader(void) {
  struct __user_cap_header_struct Header = {_LINUX_CAPABILITY_VERSION_3, 0};
  struct __user_cap_data_struct Data[2] = {{0}};
  CHECK(syscall(SYS_capget, &Header, Data) == 0);
  Data[CAP_SYS_PTRACE / 32].effective &= ~(1u << (CAP_SYS_PTRACE % 32));
  CHECK(syscall(SYS_capset, &Header, Data) == 0);

  char Path[64];
  snprintf(Path, sizeof(Path), "/proc/%d/mem", getppid());
  CHECK_ERRNO(open(Path, O_RDONLY), EACCES);
  snprintf(Path, sizeof(Path), "/proc/%d/maps", getppid());
  CHECK_ERRNO(open(Path, O_RDONLY), EACCES);
}

TEST(mem, nondumpable_process_denied) {
  CHECK(prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == 0);
  const pid_t Child = Fork(DeniedProcReader);
  CHECK(Child > 0);
  if (Child > 0) CHECK(WaitExit(Child) == 0);
}

TEST(process, status_fields) {
  char Text[4096], State = 0;
  CHECK(ReadText("/proc/self/status", Text, sizeof(Text)) > 0);
  CHECK(*Field(Text, "Name") == '\t' && Field(Text, "Name")[1] != '\n');
  CHECK(sscanf(Field(Text, "State"), " %c", &State) == 1 && State == 'R');
  CHECK(NumericField(Text, "Pid", "") == getpid());
  CHECK(NumericField(Text, "Tgid", "") == getpid());
  CHECK(NumericField(Text, "PPid", "") == getppid());
  CHECK(NumericField(Text, "Threads", "") == 1);
  CHECK(NumericField(Text, "FDSize", "") > 0);
  const char *const Memory[] = {"VmPeak", "VmSize", "VmLck", "VmPin", "VmHWM", "VmRSS",
                                "RssAnon", "RssFile", "RssShmem", "VmData", "VmStk", "VmExe",
                                "VmLib", "VmPTE", "VmSwap"};
  for (size_t Index = 0; Index < COUNT(Memory); ++Index)
    CHECK(NumericField(Text, Memory[Index], "kB") >= 0);
  CHECK(NumericField(Text, "VmPeak", "kB") >= NumericField(Text, "VmSize", "kB"));
  const char *const Counters[] = {"TracerPid", "NoNewPrivs", "voluntary_ctxt_switches", "nonvoluntary_ctxt_switches"};
  for (size_t Index = 0; Index < COUNT(Counters); ++Index)
    CHECK(NumericField(Text, Counters[Index], "") >= 0);
  const char *const Masks[] = {"SigPnd", "ShdPnd", "SigBlk", "SigIgn", "SigCgt", "CapInh", "CapPrm", "CapEff", "CapBnd", "CapAmb"};
  for (size_t Index = 0; Index < COUNT(Masks); ++Index) {
    const char *Mask = Field(Text, Masks[Index]);
    while (*Mask == '\t' || *Mask == ' ') ++Mask;
    CHECK(strspn(Mask, "0123456789abcdef") == 16 && Mask[16] == '\n');
  }
  uid_t Real, Effective, Saved, Uids[4];
  CHECK(getresuid(&Real, &Effective, &Saved) == 0);
  CHECK(sscanf(Field(Text, "Uid"), "%u %u %u %u", &Uids[0], &Uids[1], &Uids[2], &Uids[3]) == 4 &&
        Uids[0] == Real && Uids[1] == Effective && Uids[2] == Saved && Uids[3] == Effective);
  gid_t RealGroup, EffectiveGroup, SavedGroup, Gids[4];
  CHECK(getresgid(&RealGroup, &EffectiveGroup, &SavedGroup) == 0);
  CHECK(sscanf(Field(Text, "Gid"), "%u %u %u %u", &Gids[0], &Gids[1], &Gids[2], &Gids[3]) == 4 &&
        Gids[0] == RealGroup && Gids[1] == EffectiveGroup && Gids[2] == SavedGroup && Gids[3] == EffectiveGroup);
}

TEST(process, initial_environment_and_auxv) {
  FILE *File = fopen("/proc/self/environ", "r");
  CHECK(File != NULL);
  if (File) {
    if (environ[0] && environ[0][0]) {
      const char Original = environ[0][0];
      char Read = 0;
      environ[0][0] = Original == 'A' ? 'B' : 'A';
      CHECK(pread(fileno(File), &Read, 1, 0) == 1 && Read == environ[0][0]);
      environ[0][0] = Original;
    }
    char *Entry = NULL;
    size_t Capacity = 0, Index = 0;
    ssize_t Length;
    while ((Length = getdelim(&Entry, &Capacity, '\0', File)) > 0) {
      CHECK(Entry[Length - 1] == '\0' && environ[Index] && !strcmp(Entry, environ[Index]));
      if (!environ[Index]) break;
      ++Index;
    }
    CHECK(feof(File) && !environ[Index]);
    free(Entry);
    fclose(File);
  }

  struct { unsigned long Type, Value; } Aux[128];
  const ssize_t Length = ReadText("/proc/self/auxv", (char *)Aux, sizeof(Aux));
  CHECK(Length > 0 && Length % sizeof(*Aux) == 0);
  if (Length <= 0 || Length % sizeof(*Aux)) return;
  const size_t Count = Length / sizeof(*Aux);
  unsigned long Page = 0;
  for (size_t Index = 0; Index + 1 < Count; ++Index) {
    CHECK(Aux[Index].Type != AT_NULL);
    if (Aux[Index].Type == AT_PAGESZ) Page = Aux[Index].Value;
  }
  CHECK(Aux[Count - 1].Type == AT_NULL && Aux[Count - 1].Value == 0);
  CHECK(Page > 0 && Page == getauxval(AT_PAGESZ));
}

static int LimitEquals(const char *Text, rlim_t Expected) {
  if (Expected == RLIM_INFINITY) return !strcmp(Text, "unlimited");
  char *End;
  errno = 0;
  const unsigned long long Value = strtoull(Text, &End, 10);
  return !errno && End != Text && !*End && Value == Expected;
}

TEST(process, limits_match_getrlimit) {
  const struct { int Resource; const char *Name; } Limits[] = {
    {RLIMIT_CPU, "Max cpu time"}, {RLIMIT_FSIZE, "Max file size"},
    {RLIMIT_DATA, "Max data size"}, {RLIMIT_STACK, "Max stack size"},
    {RLIMIT_CORE, "Max core file size"}, {RLIMIT_RSS, "Max resident set"},
    {RLIMIT_NPROC, "Max processes"}, {RLIMIT_NOFILE, "Max open files"},
    {RLIMIT_MEMLOCK, "Max locked memory"}, {RLIMIT_AS, "Max address space"},
    {RLIMIT_LOCKS, "Max file locks"}, {RLIMIT_SIGPENDING, "Max pending signals"},
    {RLIMIT_MSGQUEUE, "Max msgqueue size"}, {RLIMIT_NICE, "Max nice priority"},
    {RLIMIT_RTPRIO, "Max realtime priority"}, {RLIMIT_RTTIME, "Max realtime timeout"},
  };
  FILE *File = fopen("/proc/self/limits", "r");
  CHECK(File != NULL);
  if (!File) return;
  char Line[256];
  CHECK(fgets(Line, sizeof(Line), File) && !strncmp(Line, "Limit ", 6) &&
        strstr(Line, "Soft Limit") && strstr(Line, "Hard Limit") && strstr(Line, "Units"));
  for (size_t Index = 0; Index < COUNT(Limits); ++Index) {
    struct rlimit Limit;
    const size_t Prefix = strlen(Limits[Index].Name);
    char Soft[32], Hard[32];
    CHECK(fgets(Line, sizeof(Line), File) && !strncmp(Line, Limits[Index].Name, Prefix) &&
          sscanf(Line + Prefix, "%31s%31s", Soft, Hard) == 2 &&
          getrlimit(Limits[Index].Resource, &Limit) == 0 &&
          LimitEquals(Soft, Limit.rlim_cur) && LimitEquals(Hard, Limit.rlim_max));
    if (CaseFailures) printf("#   limit row %s\n", Limits[Index].Name);
  }
  CHECK(fgetc(File) == EOF && feof(File));
  fclose(File);
}

TEST(process, fdinfo_position_and_flags) {
  const int Fd = open("fdinfo-probe", O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(Fd >= 0);
  if (Fd < 0) return;
  CHECK(lseek(Fd, 37, SEEK_SET) == 37);
  CHECK(fcntl(Fd, F_SETFL, O_APPEND | O_NONBLOCK) == 0);
  CHECK(fcntl(Fd, F_SETFD, FD_CLOEXEC) == 0);
  const int Status = fcntl(Fd, F_GETFL);
  CHECK(Status >= 0 && (Status & (O_APPEND | O_NONBLOCK)) == (O_APPEND | O_NONBLOCK));
  char Path[64], Text[512];
  snprintf(Path, sizeof(Path), "/proc/self/fdinfo/%d", Fd);
  CHECK(ReadText(Path, Text, sizeof(Text)) > 0);
  CHECK(NumericField(Text, "pos", "") == 37);
  unsigned Flags;
  int End = 0;
  const char *FlagText = Field(Text, "flags");
  // fdinfo also reports the forced x86-64 kernel O_LARGEFILE bit.
  const unsigned LargeFile = 0100000;
  CHECK(sscanf(FlagText, "%o%n", &Flags, &End) == 1 && FlagText[End] == '\n' &&
        Flags == ((unsigned)Status | O_CLOEXEC | LargeFile));
  close(Fd);
  CHECK(unlink("fdinfo-probe") == 0);
}

TEST(process, io_counters) {
  char Before[512], After[512], Data[32];
  const int In = open("/dev/zero", O_RDONLY), Out = open("/dev/null", O_WRONLY);
  CHECK(In >= 0 && Out >= 0);
  if (In < 0 || Out < 0) return;
  CHECK(ReadText("/proc/self/io", Before, sizeof(Before)) > 0);
  CHECK(read(In, Data, sizeof(Data)) == sizeof(Data));
  CHECK(write(Out, Data, sizeof(Data)) == sizeof(Data));
  CHECK(ReadText("/proc/self/io", After, sizeof(After)) > 0);
  const char *const Fields[] = {"rchar", "wchar", "syscr", "syscw", "read_bytes", "write_bytes"};
  const unsigned Delta[] = {sizeof(Data), sizeof(Data), 1, 1, 0, 0};
  for (size_t Index = 0; Index < COUNT(Fields); ++Index) {
    const long long First = NumericField(Before, Fields[Index], "");
    CHECK(First >= 0 && NumericField(After, Fields[Index], "") >= First + Delta[Index]);
  }
  // Linux can report a negative cancellation total.
  long long Cancelled;
  CHECK(sscanf(Field(After, "cancelled_write_bytes"), "%lld", &Cancelled) == 1);
  close(In);
  close(Out);
}

TEST(process, comm_round_trip) {
  char Text[64];
  CHECK(WriteText("/proc/self/comm", "m5-kernel-files") == 0);
  CHECK(ReadText("/proc/self/comm", Text, sizeof(Text)) > 0 && !strcmp(Text, "m5-kernel-files\n"));
}

TEST(process, oom_score_adj_round_trip) {
  const char *Path = "/proc/self/oom_score_adj";
  char Before[32], Text[32], Extra;
  int Score;
  CHECK(ReadText(Path, Before, sizeof(Before)) > 0 && sscanf(Before, "%d %c", &Score, &Extra) == 1 &&
        Score >= -1000 && Score <= 1000);
  if (CaseFailures) return;
  // Raising the score needs no privilege. Keep the change in the isolated child.
  if (Score == 1000) SKIP("inherited oom_score_adj is already at the unprivileged maximum");
  snprintf(Text, sizeof(Text), "%d\n", Score + 1);
  CHECK(WriteText(Path, Text) == 0);
  char After[32];
  CHECK(ReadText(Path, After, sizeof(After)) > 0 && !strcmp(After, Text));
}

struct ThreadPipes {
  int Ready;
  int Done;
};

static void *HoldTask(void *Argument) {
  const struct ThreadPipes *Pipes = Argument;
  const pid_t Tid = syscall(SYS_gettid);
  if (write(Pipes->Ready, &Tid, sizeof(Tid)) != sizeof(Tid)) return (void *)1;
  char Done;
  return read(Pipes->Done, &Done, 1) == 1 ? NULL : (void *)1;
}

TEST(process, second_thread_task_directory) {
  int Ready[2], Done[2];
  CHECK(pipe(Ready) == 0 && pipe(Done) == 0);
  if (CaseFailures) return;
  const struct ThreadPipes Pipes = {Ready[1], Done[0]};
  pthread_t Thread;
  CHECK(pthread_create(&Thread, NULL, HoldTask, (void *)&Pipes) == 0);
  if (CaseFailures) return;
  pid_t Tid = 0;
  CHECK(read(Ready[0], &Tid, sizeof(Tid)) == sizeof(Tid) && Tid > 0 && Tid != getpid());
  char Path[64], Text[4096], Id[32];
  snprintf(Path, sizeof(Path), "/proc/self/task/%d/status", Tid);
  CHECK(ReadText(Path, Text, sizeof(Text)) > 0);
  CHECK(NumericField(Text, "Pid", "") == Tid);
  CHECK(NumericField(Text, "Tgid", "") == getpid());
  CHECK(NumericField(Text, "Threads", "") == 2);
  snprintf(Id, sizeof(Id), "%d", Tid);
  DIR *Tasks = opendir("/proc/self/task");
  CHECK(Tasks != NULL);
  if (Tasks) {
    int Found = 0;
    for (struct dirent *Entry; (Entry = readdir(Tasks));) Found |= !strcmp(Entry->d_name, Id);
    CHECK(Found);
    closedir(Tasks);
  }
  CHECK(write(Done[1], "x", 1) == 1);
  void *Result;
  CHECK(pthread_join(Thread, &Result) == 0 && Result == NULL);
  close(Ready[0]);
  close(Ready[1]);
  close(Done[0]);
  close(Done[1]);
}

TEST(system, cpuinfo_and_stat_counts) {
  const long Online = sysconf(_SC_NPROCESSORS_ONLN);
  CHECK(Online > 0);
  FILE *CpuInfo = fopen("/proc/cpuinfo", "r");
  CHECK(CpuInfo != NULL);
  if (!CpuInfo) return;
  long Processors = 0;
  char *Line = NULL;
  size_t Capacity = 0;
  while (getline(&Line, &Capacity, CpuInfo) > 0) {
    unsigned Cpu;
    if (sscanf(Line, "processor : %u", &Cpu) == 1) {
      if (GuestRuntime) CHECK(Cpu == (unsigned long)Processors);
      ++Processors;
    }
  }
  CHECK(feof(CpuInfo) && Processors == Online);
  fclose(CpuInfo);

  FILE *Stat = fopen("/proc/stat", "r");
  CHECK(Stat != NULL);
  if (Stat) {
    CHECK(getline(&Line, &Capacity, Stat) > 0 && !strncmp(Line, "cpu ", 4));
    long Cpus = 0;
    while (getline(&Line, &Capacity, Stat) > 0) {
      unsigned Cpu;
      unsigned long long User, Nice, System, Idle;
      if (sscanf(Line, "cpu%u %llu %llu %llu %llu", &Cpu, &User, &Nice, &System, &Idle) == 5) {
        if (GuestRuntime) CHECK(Cpu == (unsigned long)Cpus);
        ++Cpus;
      }
    }
    CHECK(feof(Stat) && Cpus == Online);
    fclose(Stat);
  }
  free(Line);
}

TEST(system, meminfo_version_uptime_loadavg) {
  char Text[8192], Extra;
  CHECK(ReadText("/proc/meminfo", Text, sizeof(Text)) > 0);
  const long long Total = NumericField(Text, "MemTotal", "kB");
  const long long Available = NumericField(Text, "MemAvailable", "kB");
  CHECK(Total > 0 && Available >= 0 && Available <= Total);
  CHECK(ReadText("/proc/version", Text, sizeof(Text)) > 0 && !strncmp(Text, "Linux version ", 14));
  double Up, Idle, One, Five, Fifteen;
  unsigned Running, Tasks, Pid;
  CHECK(ReadText("/proc/uptime", Text, sizeof(Text)) > 0 &&
        sscanf(Text, "%lf %lf %c", &Up, &Idle, &Extra) == 2 && Up > 0 && Idle >= 0);
  CHECK(ReadText("/proc/loadavg", Text, sizeof(Text)) > 0 &&
        sscanf(Text, "%lf %lf %lf %u/%u %u %c", &One, &Five, &Fifteen, &Running, &Tasks, &Pid, &Extra) == 6 &&
        One >= 0 && Five >= 0 && Fifteen >= 0 && Tasks >= Running && Tasks > 0 && Pid > 0);
}

TEST(sysctl, pid_max_and_ostype) {
  char Text[128], Extra;
  unsigned PidMax;
  CHECK(ReadText("/proc/sys/kernel/pid_max", Text, sizeof(Text)) > 0 &&
        sscanf(Text, "%u %c", &PidMax, &Extra) == 1 && PidMax >= 32768);
  if (GuestRuntime && !CaseFailures) CHECK(PidMax == 32768);
  CHECK(ReadText("/proc/sys/kernel/ostype", Text, sizeof(Text)) > 0 && !strcmp(Text, "Linux\n"));
}

TEST(sysctl, readonly_write_denied) {
  if (GuestRuntime && geteuid() == 0) {
    CHECK(DropIds(1000, 1000) == 0);
    if (CaseFailures) return;
  }
  CHECK_ERRNO(open("/proc/sys/kernel/ostype", O_WRONLY), EACCES);
}

TEST(sysctl, hostname_in_private_uts_namespace) {
  const uid_t Uid = geteuid();
  const gid_t Gid = getegid();
  const char *Path = "/proc/sys/kernel/hostname";
  const char *Hostname = "m5-kernel-files";
  char Text[128];
  if (unshare(CLONE_NEWUTS) != 0) {
    const int Result = unshare(CLONE_NEWUSER | CLONE_NEWUTS);
    if (!GuestRuntime && Result != 0 && (errno == EPERM || errno == EINVAL || errno == ENOSPC))
      SKIP("native UTS namespace creation is unavailable");
    CHECK(Result == 0);
    if (Result != 0) return;
    CHECK(WriteText("/proc/self/setgroups", "deny") == 0);
    snprintf(Text, sizeof(Text), "0 %u 1", Uid);
    CHECK(WriteText("/proc/self/uid_map", Text) == 0);
    snprintf(Text, sizeof(Text), "0 %u 1", Gid);
    CHECK(WriteText("/proc/self/gid_map", Text) == 0);
    if (CaseFailures) return;
  }

  const int Result = WriteText(Path, Hostname);
  if (!GuestRuntime && Result != 0 && (errno == EACCES || errno == EPERM || errno == EROFS))
    SKIP("native procfs denies the hostname sysctl in this namespace");
  CHECK(Result == 0);
  struct utsname Name;
  CHECK(uname(&Name) == 0 && !strcmp(Name.nodename, Hostname));
  snprintf(Text, sizeof(Text), "%s\n", Hostname);
  char Actual[128];
  CHECK(ReadText(Path, Actual, sizeof(Actual)) > 0 && !strcmp(Actual, Text));
}

#define CPU_DIR "/sys/devices/system/cpu"
#define NODE_ZERO "/sys/devices/system/node/node0"
#define CGROUP_DIR "/sys/fs/cgroup"

static long ParseCpuList(const char *Text, unsigned char *Bits, size_t Size) {
  const char *Cursor = Text;
  long Count = 0;
  unsigned long Previous = 0;
  if (*Cursor == '\n' && !Cursor[1]) return 0;
  for (;;) {
    char *End;
    if (!isdigit((unsigned char)*Cursor)) return -1;
    const unsigned long First = strtoul(Cursor, &End, 10);
    unsigned long Last = First;
    if (*End == '-') {
      if (!isdigit((unsigned char)End[1])) return -1;
      Last = strtoul(End + 1, &End, 10);
    }
    if (Last < First || Last >= Size || (Count && First <= Previous)) return -1;
    for (unsigned long Cpu = First; Cpu <= Last; ++Cpu) Bits[Cpu] = 1;
    Count += Last - First + 1;
    Previous = Last;
    if (*End == '\n' && !End[1]) return Count;
    if (*End != ',') return -1;
    Cursor = End + 1;
  }
}

TEST(sysfs, online_cpus_and_topology) {
  char Text[8192];
  unsigned char Cpus[32768] = {0};
  const long Online = sysconf(_SC_NPROCESSORS_ONLN);
  CHECK(Online > 0 && ReadText(CPU_DIR "/online", Text, sizeof(Text)) > 0);
  if (CaseFailures) return;
  CHECK(ParseCpuList(Text, Cpus, COUNT(Cpus)) == Online);
  if (GuestRuntime) {
    char Expected[64];
    if (Online == 1) strcpy(Expected, "0\n");
    else snprintf(Expected, sizeof(Expected), "0-%ld\n", Online - 1);
    CHECK(!strcmp(Text, Expected));
  }
  for (size_t Cpu = 0; Cpu < COUNT(Cpus); ++Cpu) {
    if (!Cpus[Cpu]) continue;
    const char *const Fields[] = {"core_id", "physical_package_id", "thread_siblings", "thread_siblings_list",
                                  "core_siblings", "core_siblings_list", "die_id", "core_cpus", "core_cpus_list",
                                  "package_cpus", "package_cpus_list"};
    const size_t Count = GuestRuntime ? COUNT(Fields) : 6;
    for (size_t Index = 0; Index < Count; ++Index) {
      char Path[128];
      snprintf(Path, sizeof(Path), CPU_DIR "/cpu%zu/topology/%s", Cpu, Fields[Index]);
      const ssize_t Length = ReadText(Path, Text, sizeof(Text));
      CHECK(Length > 0);
      if (Length <= 0) {
        printf("#   missing or empty %s\n", Path);
        continue;
      }
      if (!GuestRuntime) continue;
      char Expected[64];
      if (Index <= 1 || Index == 6) {
        snprintf(Expected, sizeof(Expected), "%zu\n", Index == 0 ? Cpu : 0);
      } else if (strstr(Fields[Index], "_list")) {
        if (Index == 3 || Index == 8) snprintf(Expected, sizeof(Expected), "%zu\n", Cpu);
        else if (Online == 1) strcpy(Expected, "0\n");
        else snprintf(Expected, sizeof(Expected), "0-%ld\n", Online - 1);
      } else {
        // Switch 2 has one package and six CPUs with one thread each.
        CHECK(Online <= 32 && Cpu < 32);
        if (Online > 32 || Cpu >= 32) return;
        const unsigned Mask = Index == 2 || Index == 7 ? 1u << Cpu : (unsigned)((1ull << Online) - 1);
        snprintf(Expected, sizeof(Expected), "%0*x\n", (int)(Online + 3) / 4, Mask);
      }
      if (strcmp(Text, Expected)) printf("#   %s expected %s", Path, Expected);
      CHECK(!strcmp(Text, Expected));
    }
  }
}

TEST(sysfs, node0_cpu_mask) {
  char List[8192], Mask[16384];
  unsigned char Cpus[32768] = {0};
  CHECK(ReadText(NODE_ZERO "/cpulist", List, sizeof(List)) > 0);
  const long Count = ParseCpuList(List, Cpus, COUNT(Cpus));
  CHECK(Count > 0);
  const ssize_t Length = ReadText(NODE_ZERO "/cpumap", Mask, sizeof(Mask));
  CHECK(Length > 1 && Mask[Length - 1] == '\n');
  if (CaseFailures) return;
  if (GuestRuntime) {
    char Online[64];
    CHECK(ReadText(CPU_DIR "/online", Online, sizeof(Online)) > 0 && !strcmp(List, Online));
  }
  size_t Bit = 0;
  long Set = 0;
  for (ssize_t Index = Length - 2; Index >= 0; --Index) {
    if (Mask[Index] == ',') {
      CHECK(Bit > 0 && Bit % 32 == 0);
      continue;
    }
    const int C = tolower((unsigned char)Mask[Index]);
    const unsigned Nibble = C >= '0' && C <= '9' ? (unsigned)(C - '0') : (unsigned)(C - 'a' + 10);
    CHECK(isxdigit(C) && Nibble < 16 && Bit + 4 <= COUNT(Cpus));
    if (CaseFailures) return;
    for (unsigned Shift = 0; Shift < 4; ++Shift, ++Bit) {
      const int Present = (Nibble >> Shift) & 1;
      CHECK(Present == Cpus[Bit]);
      Set += Present;
    }
    if (CaseFailures) return;
  }
  CHECK(Set == Count);
}

static int CgroupProcsPath(char *Path, size_t Size) {
  if (GuestRuntime) return snprintf(Path, Size, CGROUP_DIR "/cgroup.procs") < (int)Size;
  // On Linux the suite normally belongs to a systemd scope below the root.
  char Text[PATH_MAX];
  if (ReadText("/proc/self/cgroup", Text, sizeof(Text)) < 0) return 0;
  for (char *Line = strtok(Text, "\n"); Line; Line = strtok(NULL, "\n")) {
    if (strncmp(Line, "0::/", 4)) continue;
    const char *Group = Line + 3;
    const int Length = snprintf(Path, Size, CGROUP_DIR "%s/cgroup.procs", !strcmp(Group, "/") ? "" : Group);
    return Length >= 0 && Length < (int)Size;
  }
  return 0;
}

TEST(sysfs, cgroup_contains_self) {
  char Path[PATH_MAX];
  const int HavePath = CgroupProcsPath(Path, sizeof(Path));
  if (!GuestRuntime && !HavePath) SKIP("native cgroup v2 path unavailable");
  CHECK(HavePath);
  if (!HavePath) return;
  FILE *File = fopen(Path, "r");
  if (!GuestRuntime && !File && (errno == ENOENT || errno == EOPNOTSUPP))
    SKIP("native cgroup v2 membership file unavailable");
  CHECK(File != NULL);
  if (!File) return;
  int Found = 0;
  unsigned Pid;
  while (fscanf(File, "%u", &Pid) == 1) Found |= Pid == (unsigned)getpid();
  CHECK(Found && feof(File));
  fclose(File);
}

TEST(sysfs, cgroup_write_denied) {
  char Path[PATH_MAX], Pid[32];
  const int HavePath = CgroupProcsPath(Path, sizeof(Path));
  if (!GuestRuntime && !HavePath) SKIP("native cgroup v2 path unavailable");
  CHECK(HavePath);
  if (!HavePath) return;
  snprintf(Pid, sizeof(Pid), "%d\n", getpid());
  if (GuestRuntime) {
    CHECK_ERRNO(WriteText(Path, Pid), EROFS);
    return;
  }
  // Rejoining the current native cgroup can succeed on delegated writable cgroups.
  const int Result = WriteText(Path, Pid);
  if (Result == 0) SKIP("native cgroup permits writing the caller's PID");
  if (errno != EACCES && errno != EBUSY) {
    char Reason[96];
    snprintf(Reason, sizeof(Reason), "native cgroup write returned %s", ErrnoName(errno));
    SKIP(Reason);
  }
  CHECK(Result == -1);
}

TEST(sysfs, power_supply_type_and_capacity) {
  const char *Directory = "/sys/class/power_supply";
  DIR *Supplies = opendir(Directory);
  if (!Supplies && !GuestRuntime && errno == ENOENT) SKIP("native power_supply class unavailable");
  CHECK(Supplies != NULL);
  if (!Supplies) return;
  int Count = 0;
  for (struct dirent *Entry; (Entry = readdir(Supplies));) {
    if (Entry->d_name[0] == '.') continue;
    ++Count;
    char Path[PATH_MAX], Type[64], Text[64], Extra;
    snprintf(Path, sizeof(Path), "%s/%s/type", Directory, Entry->d_name);
    CHECK(ReadText(Path, Type, sizeof(Type)) > 1 && strchr(Type, '\n'));
    if (GuestRuntime) {
      CHECK((!strcmp(Entry->d_name, "BAT0") && !strcmp(Type, "Battery\n")) ||
            (!strcmp(Entry->d_name, "ADP1") && !strcmp(Type, "Mains\n")));
    }
    snprintf(Path, sizeof(Path), "%s/%s/capacity", Directory, Entry->d_name);
    const ssize_t Length = ReadText(Path, Text, sizeof(Text));
    if (Length < 0 && errno == ENOENT && (!GuestRuntime || strcmp(Entry->d_name, "BAT0"))) continue;
    int Capacity;
    CHECK(Length > 0 && sscanf(Text, "%d %c", &Capacity, &Extra) == 1 && Capacity >= 0 && Capacity <= 100);
  }
  closedir(Supplies);
  if (CaseFailures) return;
  if (!Count) SKIP("no power supplies exposed");
  if (GuestRuntime) CHECK(Count == 2);
}

static unsigned CheckSysfsFiles(const char *Directory) {
  DIR *Dir = opendir(Directory);
  CHECK(Dir != NULL);
  if (!Dir) return 0;
  unsigned Files = 0;
  for (;;) {
    errno = 0;
    struct dirent *Entry = readdir(Dir);
    if (!Entry) {
      CHECK(errno == 0);
      break;
    }
    if (!strcmp(Entry->d_name, ".") || !strcmp(Entry->d_name, "..")) continue;
    char Path[PATH_MAX];
    const int Length = snprintf(Path, sizeof(Path), "%s/%s", Directory, Entry->d_name);
    CHECK(Length >= 0 && Length < (int)sizeof(Path));
    if (Length < 0 || Length >= (int)sizeof(Path)) continue;
    struct stat Stat;
    const int Result = lstat(Path, &Stat);
    CHECK(Result == 0);
    if (Result < 0) continue;
    if (S_ISDIR(Stat.st_mode)) Files += CheckSysfsFiles(Path);
    if (!S_ISREG(Stat.st_mode)) continue;
    ++Files;
    const unsigned Failures = CaseFailures;
    CHECK(Stat.st_size == 0);
    if (!(Stat.st_mode & 044) && geteuid() != Stat.st_uid && geteuid() != 0) {
      CHECK_ERRNO(open(Path, O_RDONLY), EACCES);
    } else {
      const int Fd = open(Path, O_RDONLY);
      CHECK(Fd >= 0);
      if (Fd >= 0) {
        CHECK_ERRNO(lseek(Fd, 0, SEEK_END), EINVAL);
        close(Fd);
      }
    }
    if (CaseFailures != Failures) printf("#   path %s\n", Path);
  }
  closedir(Dir);
  return Files;
}

TEST(sysfs, generated_file_size_and_seek_end) {
  if (!GuestRuntime) SKIP("Switch 2 generated file metadata");
  CHECK(CheckSysfsFiles("/sys") > 0);
}

TEST(dev, null_zero_full) {
  const char *const Paths[] = {"/dev/null", "/dev/zero", "/dev/full"};
  const unsigned Minors[] = {3, 5, 7};
  const char Zero[32] = {0};
  for (size_t Index = 0; Index < COUNT(Paths); ++Index) {
    struct stat Stat;
    CHECK(stat(Paths[Index], &Stat) == 0 && S_ISCHR(Stat.st_mode) &&
          major(Stat.st_rdev) == 1 && minor(Stat.st_rdev) == Minors[Index]);
    const int Fd = open(Paths[Index], O_RDWR);
    CHECK(Fd >= 0);
    if (Fd < 0) continue;
    char Data[sizeof(Zero)];
    memset(Data, 1, sizeof(Data));
    if (Index == 0) CHECK(read(Fd, Data, sizeof(Data)) == 0 && Data[0] == 1);
    else CHECK(read(Fd, Data, sizeof(Data)) == sizeof(Data) && !memcmp(Data, Zero, sizeof(Data)));
    if (Index == 2) CHECK_ERRNO(write(Fd, "x", 1), ENOSPC);
    else CHECK(write(Fd, "x", 1) == 1);
    close(Fd);
  }
}

TEST(dev, random_and_urandom) {
  const char *const Paths[] = {"/dev/random", "/dev/urandom"};
  for (size_t Index = 0; Index < COUNT(Paths); ++Index) {
    struct stat Stat;
    CHECK(stat(Paths[Index], &Stat) == 0 && S_ISCHR(Stat.st_mode) &&
          major(Stat.st_rdev) == 1 && minor(Stat.st_rdev) == 8 + Index);
    const int Fd = open(Paths[Index], O_RDONLY | O_NONBLOCK);
    CHECK(Fd >= 0);
    if (Fd < 0) continue;
    unsigned char First[32], Second[32];
    CHECK(read(Fd, First, sizeof(First)) == sizeof(First) &&
          read(Fd, Second, sizeof(Second)) == sizeof(Second) &&
          memcmp(First, Second, sizeof(First)) != 0);
    close(Fd);
  }
}

static void CheckTty(void) {
  CHECK(setsid() == getpid());
  if (CaseFailures) return;
  CHECK_ERRNO(open("/dev/tty", O_RDWR | O_NONBLOCK), ENXIO);
  const int Master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
  CHECK(Master >= 0);
  if (Master < 0) return;
  char Path[64];
  CHECK(grantpt(Master) == 0 && unlockpt(Master) == 0 && ptsname_r(Master, Path, sizeof(Path)) == 0);
  if (CaseFailures) return;
  const int Slave = open(Path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  CHECK(Slave >= 0);
  if (Slave < 0) return;
  CHECK(ioctl(Slave, TIOCSCTTY, 0) == 0);
  const int Fd = open("/dev/tty", O_RDWR | O_NONBLOCK);
  CHECK(Fd >= 0);
  if (Fd >= 0) {
    struct termios Term;
    CHECK(tcgetattr(Fd, &Term) == 0);
    CHECK(tcgetsid(Fd) == getsid(0));
    close(Fd);
  }
  // Closing the master hangs up this session and signals its foreground group.
  CHECK(signal(SIGHUP, SIG_IGN) != SIG_ERR);
  close(Slave);
  close(Master);
}

TEST(dev, controlling_tty) {
  struct stat Stat;
  CHECK(stat("/dev/tty", &Stat) == 0 && S_ISCHR(Stat.st_mode) &&
        major(Stat.st_rdev) == 5 && minor(Stat.st_rdev) == 0);
  const pid_t Child = Fork(CheckTty);
  CHECK(Child > 0);
  if (Child > 0) CHECK(WaitExit(Child) == 0);
}

TEST(dev, fd_and_stdin_links) {
  const char *const Paths[] = {"/dev/fd", "/dev/stdin"};
  const char *const Targets[] = {"/proc/self/fd", "/proc/self/fd/0"};
  for (size_t Index = 0; Index < COUNT(Paths); ++Index) {
    char Text[64];
    const size_t Length = strlen(Targets[Index]);
    CHECK(readlink(Paths[Index], Text, sizeof(Text)) == (ssize_t)Length &&
          !memcmp(Text, Targets[Index], Length));
  }
}

TEST(dev, pipe_and_non_tty_ioctls) {
  int Pipe[2], Count = -1, Mode = 1;
  struct termios Term;
  CHECK(pipe(Pipe) == 0);
  if (CaseFailures) return;
  CHECK(write(Pipe[1], "abc", 3) == 3);
  CHECK(ioctl(Pipe[0], FIONREAD, &Count) == 0 && Count == 3);
  CHECK(ioctl(Pipe[0], FIONBIO, &Mode) == 0);
  int Flags = fcntl(Pipe[0], F_GETFL);
  CHECK(Flags >= 0 && (Flags & O_NONBLOCK));
  Mode = 0;
  CHECK(ioctl(Pipe[0], FIONBIO, &Mode) == 0);
  Flags = fcntl(Pipe[0], F_GETFL);
  CHECK(Flags >= 0 && !(Flags & O_NONBLOCK));
  CHECK(ioctl(Pipe[0], FIOCLEX) == 0 && fcntl(Pipe[0], F_GETFD) == FD_CLOEXEC);
  CHECK_ERRNO(ioctl(Pipe[0], TCGETS, &Term), ENOTTY);
  close(Pipe[0]);
  close(Pipe[1]);
}

// Check stat and discovery metadata for each event node, then select the
// requested device class from udev so a permission bug cannot hide the pad.
static int FindEventNode(const char *Class, char *Path, size_t Size) {
  DIR *Input = opendir("/dev/input");
  CHECK(Input != NULL);
  if (!Input) return 0;
  int Found = 0;
  for (struct dirent *Entry; (Entry = readdir(Input));) {
    unsigned Number;
    int End = 0;
    if (sscanf(Entry->d_name, "event%u%n", &Number, &End) != 1 || Entry->d_name[End] || Number > 191) continue;
    char Device[64], File[128], Text[4096], Expected[32];
    snprintf(Device, sizeof(Device), "/dev/input/event%u", Number);
    const unsigned Minor = 64 + Number;
    struct stat Stat;
    CHECK(stat(Device, &Stat) == 0 && S_ISCHR(Stat.st_mode) &&
          major(Stat.st_rdev) == 13 && minor(Stat.st_rdev) == Minor &&
          Stat.st_uid == 0 && (Stat.st_mode & 0777) == 0660 && Stat.st_ctim.tv_sec > 0);

    snprintf(File, sizeof(File), "/sys/class/input/event%u/dev", Number);
    snprintf(Expected, sizeof(Expected), "13:%u\n", Minor);
    CHECK(ReadText(File, Text, sizeof(Text)) > 0 && !strcmp(Text, Expected));
    snprintf(File, sizeof(File), "/sys/class/input/event%u", Number);
    CHECK(lstat(File, &Stat) == 0 && S_ISLNK(Stat.st_mode) && (Stat.st_mode & 0777) == 0777 &&
          Stat.st_uid == 0 && Stat.st_gid == 0 && Stat.st_ctim.tv_sec > 0);

    snprintf(File, sizeof(File), "/run/udev/data/c13:%u", Minor);
    CHECK(stat(File, &Stat) == 0 && Stat.st_uid == 0 && Stat.st_gid == 0 && Stat.st_ctim.tv_sec > 0);
    CHECK(ReadText(File, Text, sizeof(Text)) > 0 && strstr(Text, "\nE:ID_INPUT=1\n"));
    if (!Found && strstr(Text, Class)) {
      if (strlen(Device) >= Size) break;
      strcpy(Path, Device);
      Found = 1;
    }
  }
  closedir(Input);
  return Found;
}

TEST(input, pad_identity_capabilities_and_idle) {
  if (!GuestRuntime) SKIP("Switch 2 evdev and udev data");
  char Path[64];
  const int Found = FindEventNode("\nE:ID_INPUT_JOYSTICK=1\n", Path, sizeof(Path));
  if (CaseFailures) return;
  if (!Found) SKIP("no connected Switch 2 gamepad");
  if (access(Path, R_OK) < 0) {
    const int AccessError = errno;
    CHECK(AccessError == EACCES);
    CHECK_ERRNO(open(Path, O_RDONLY | O_NONBLOCK), EACCES);
    if (CaseFailures) return;
    SKIP("the foreground login owns the pad ACL");
  }
  const int Fd = open(Path, O_RDONLY | O_NONBLOCK);
  CHECK(Fd >= 0);
  if (Fd < 0) return;
  int Version = 0;
  struct input_id Id = {0};
  char Name[128] = {0};
  unsigned char Bits[(EV_MAX + 8) / 8] = {0};
  CHECK(ioctl(Fd, EVIOCGVERSION, &Version) == 0 && Version == 0x010001);
  CHECK(ioctl(Fd, EVIOCGID, &Id) == 0 && Id.bustype != 0 && Id.vendor != 0 && Id.product != 0);
  const int Length = ioctl(Fd, EVIOCGNAME(sizeof(Name)), Name);
  CHECK(Length > 1 && Length <= (int)sizeof(Name) && Name[0] && Name[Length - 1] == 0);
  CHECK(ioctl(Fd, EVIOCGBIT(0, sizeof(Bits)), Bits) > 0 &&
        (Bits[EV_SYN / 8] & (1u << (EV_SYN % 8))) && (Bits[EV_KEY / 8] & (1u << (EV_KEY % 8))));
  struct input_event Event;
  CHECK_ERRNO(read(Fd, &Event, sizeof(Event)), EAGAIN);
  struct pollfd Poll = {.fd = Fd, .events = POLLIN};
  CHECK(poll(&Poll, 1, 0) == 0 && Poll.revents == 0);
  close(Fd);
}

TEST(input, inotify_watch) {
  if (!GuestRuntime) SKIP("Switch 2 input hot-plug directory");
  const int Fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  CHECK(Fd >= 0);
  if (Fd < 0) return;
  const int Watch = inotify_add_watch(Fd, "/dev/input", IN_CREATE | IN_DELETE | IN_ATTRIB | IN_ONLYDIR);
  CHECK(Watch >= 0);
  if (Watch >= 0) CHECK(inotify_rm_watch(Fd, Watch) == 0);
  close(Fd);
}

TEST(input, keyboard_denied_to_seat_user) {
  if (!GuestRuntime) SKIP("Switch 2 keyboard and seat ACL");
  if (geteuid() != 1000) SKIP("requires the suite's UID 1000 .user identity");
  char Path[64];
  const int Found = FindEventNode("\nE:ID_INPUT_KEYBOARD=1\n", Path, sizeof(Path));
  CHECK(Found);
  if (Found) CHECK_ERRNO(open(Path, O_RDONLY | O_NONBLOCK), EACCES);
}
