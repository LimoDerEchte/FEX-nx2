// SPDX-License-Identifier: MIT
// Filesystem compatibility suite for the Switch 2 guest runtime.
//
// Each test runs in a fresh scratch directory that is also the working
// directory. Results use TAP-style lines so a console log can be compared
// against the native Linux reference run. Expectations follow Linux; tests
// that depend on filesystem features or privilege skip with a reason when
// the feature is absent instead of passing silently.
//
// Usage: fs-suite [-l] [-d directory] [pattern...]
//   pattern   run tests whose name starts with the pattern ("io/", "path/rename")
//   !pattern  exclude tests whose name starts with the pattern
//   -l        list test names
//   -d dir    create the scratch tree in dir instead of the working directory
// Exit status: 42 when every selected test passes or skips, 1 otherwise.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <linux/falloc.h>
#include <linux/openat2.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#ifndef RWF_APPEND
#define RWF_APPEND 0x10
#endif
#ifndef RWF_NOAPPEND
#define RWF_NOAPPEND 0x20
#endif
#ifndef AT_HANDLE_FID
#define AT_HANDLE_FID 0x200
#endif
#ifndef SYS_fchmodat2
#define SYS_fchmodat2 452
#endif
#ifndef SYS_setxattrat
#define SYS_setxattrat 463
#define SYS_getxattrat 464
#endif

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

static int Root = -1;           // Suite scratch directory.
static int Dir = -1;            // Per-test scratch directory and working directory.
static unsigned CaseFailures;
static const char *SkipReason;
static int CanSwitchIDs;        // Root that can assume unrelated IDs in a child.

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
// System call must succeed (return >= 0).
#define OK(expr) do { errno = 0; long Result_ = (long)(expr); if (Result_ < 0) Fail(__LINE__, #expr, Result_, errno, 0); } while (0)
// System call must fail with the given errno.
#define FAILS(expr, error) do {                                                              \
    errno = 0; long Result_ = (long)(expr);                                                  \
    if (Result_ != -1 || errno != (error)) Fail(__LINE__, #expr, Result_, errno, error);      \
  } while (0)
#define SKIP(reason) do { SkipReason = (reason); return; } while (0)

static int Create(const char *Name, const char *Data) {
  const int FD = openat(Dir, Name, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (FD < 0) return -1;
  if (Data && write(FD, Data, strlen(Data)) != (ssize_t) strlen(Data)) {
    close(FD);
    return -1;
  }
  return FD;
}

static int MakeFile(const char *Name, const char *Data) {
  const int FD = Create(Name, Data);
  return FD < 0 ? -1 : close(FD);
}

static int Contents(const char *Name, const char *Expected) {
  char Buffer[256] = {0};
  const int FD = openat(Dir, Name, O_RDONLY | O_CLOEXEC);
  if (FD < 0) return 0;
  const ssize_t Read = read(FD, Buffer, sizeof(Buffer) - 1);
  close(FD);
  return Read == (ssize_t) strlen(Expected) && memcmp(Buffer, Expected, Read) == 0;
}

static int Exists(const char *Name) {
  struct stat Info;
  return fstatat(Dir, Name, &Info, AT_SYMLINK_NOFOLLOW) == 0;
}

static ino_t InodeOf(const char *Name, int Flags) {
  struct stat Info;
  return fstatat(Dir, Name, &Info, Flags) == 0 ? Info.st_ino : 0;
}

static int AllZero(const char *Buffer, size_t Size) {
  for (size_t Index = 0; Index < Size; ++Index) if (Buffer[Index]) return 0;
  return 1;
}

// Runs Function in a child with the given credentials and merges failures.
static void AsUser(uid_t User, gid_t Group, void (*Function)(void)) {
  fflush(stdout);
  const pid_t Child = fork();
  if (Child == 0) {
    CaseFailures = 0;
    if (setgroups(0, NULL) || setresgid(Group, Group, Group) || setresuid(User, User, User)) {
      printf("#   could not switch to uid %u gid %u: %s\n", User, Group, ErrnoName(errno));
      fflush(stdout);
      _exit(1);
    }
    Function();
    fflush(stdout);
    _exit(CaseFailures > 100 ? 100 : (int) CaseFailures);
  }
  int Status = 0;
  if (Child < 0 || waitpid(Child, &Status, 0) != Child || !WIFEXITED(Status)) {
    printf("#   credential child did not exit normally\n");
    ++CaseFailures;
    return;
  }
  CaseFailures += WEXITSTATUS(Status);
}

static void RemoveTree(int Parent, const char *Name) {
  if (unlinkat(Parent, Name, 0) == 0 || errno == ENOENT) return;
  const int FD = openat(Parent, Name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (FD < 0) return;
  fchmod(FD, 0700);
  const int Listing = dup(FD);
  DIR *Stream = Listing < 0 ? NULL : fdopendir(Listing);
  if (Stream) {
    for (;;) {
      const struct dirent *Entry = readdir(Stream);
      if (!Entry) break;
      if (strcmp(Entry->d_name, ".") && strcmp(Entry->d_name, "..")) RemoveTree(FD, Entry->d_name);
    }
    closedir(Stream);
  }
  close(FD);
  unlinkat(Parent, Name, AT_REMOVEDIR);
}

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

static int ProbeIDSwitch(void) {
  if (geteuid() != 0) return 0;
  fflush(stdout);
  const pid_t Child = fork();
  if (Child == 0) _exit(setresgid(4243, 4243, 4243) == 0 && setresuid(4242, 4242, 4242) == 0 ? 0 : 1);
  int Status = 0;
  return Child > 0 && waitpid(Child, &Status, 0) == Child && WIFEXITED(Status) && WEXITSTATUS(Status) == 0;
}

static int CompareTests(const void *Left, const void *Right) {
  return strcmp(((const struct Test *) Left)->Name, ((const struct Test *) Right)->Name);
}

int main(int argc, char **argv) {
  const char *Location = ".";
  int List = 0, Count = 0;
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
  char Scratch[PATH_MAX];
  snprintf(Scratch, sizeof(Scratch), "%s/fs-suite.XXXXXX", Location);
  if (!mkdtemp(Scratch)) {
    printf("Bail out! cannot create scratch directory in %s: %s\n", Location, ErrnoName(errno));
    return 1;
  }
  const int Origin = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  Root = open(Scratch, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (Root < 0) {
    printf("Bail out! cannot open %s: %s\n", Scratch, ErrnoName(errno));
    return 1;
  }
  CanSwitchIDs = ProbeIDSwitch();
  unsigned Selection = 0;
  for (unsigned Index = 0; Index < TestCount; ++Index) Selection += Selected(Tests[Index].Name, Count, Patterns);
  printf("TAP version 13\n1..%u\n# fs-suite scratch %s, uid %u, gid %u, id switching %s\n", Selection, Scratch,
         getuid(), getgid(), CanSwitchIDs ? "available" : "unavailable");
  unsigned Number = 0, Passed = 0, Failed = 0, Skipped = 0;
  const char* FailedNames[sizeof(Tests) / sizeof(Tests[0])];
  int Baseline = dup(0);
  if (Baseline >= 0) close(Baseline);
  for (unsigned Index = 0; Index < TestCount; ++Index) {
    if (!Selected(Tests[Index].Name, Count, Patterns)) continue;
    ++Number;
    char Name[32];
    snprintf(Name, sizeof(Name), "t%03u", Number);
    CaseFailures = 0;
    SkipReason = NULL;
    if (mkdirat(Root, Name, 0755) < 0 || (Dir = openat(Root, Name, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0 ||
        fchdir(Dir) < 0) {
      printf("not ok %u %s\n#   scratch setup failed: %s\n", Number, Tests[Index].Name, ErrnoName(errno));
      FailedNames[Failed++] = Tests[Index].Name;
      continue;
    }
    umask(022);
    Tests[Index].Function();
    // Leave no descriptors behind for the next case.
    for (int FD = Baseline > 0 ? Baseline : 3; FD < 1024; ++FD) {
      if (FD != Root && FD != Dir && FD != Origin) close(FD);
    }
    fchdir(Root);
    close(Dir);
    RemoveTree(Root, Name);
    if (CaseFailures) {
      printf("not ok %u %s\n", Number, Tests[Index].Name);
      FailedNames[Failed++] = Tests[Index].Name;
    } else if (SkipReason) {
      printf("ok %u %s # SKIP %s\n", Number, Tests[Index].Name, SkipReason);
      ++Skipped;
    } else {
      printf("ok %u %s\n", Number, Tests[Index].Name);
      ++Passed;
    }
  }
  close(Root);
  if (Origin >= 0) fchdir(Origin);
  if (rmdir(Scratch) < 0) printf("# scratch directory %s was not empty\n", Scratch);
  // Repeat the failures last so they survive a truncated console log.
  for (unsigned Index = 0; Index < Failed; ++Index) printf("# failed: %s\n", FailedNames[Index]);
  // Same result format in every suite: passed/failed/skipped, the failed names above, exit 42 when none failed.
  printf("# fs-suite summary: %u/%u/%u (passed/failed/skipped)\n", Passed, Failed, Skipped);
  // final-pass-suite collects the counts through this file.
  const char* ResultsPath = getenv("FEX_SUITE_RESULTS");
  // The runner creates the file; opening it without O_CREAT also works in a sticky /tmp (protected_regular).
  const int Results = ResultsPath ? open(ResultsPath, O_WRONLY | O_APPEND | O_CLOEXEC) : -1;
  if (Results >= 0) {
    dprintf(Results, "fs-suite %u %u %u\n", Passed, Failed, Skipped);
    close(Results);
  }
  return Failed ? 1 : 42;
}

// ---------------------------------------------------------------------------
// Descriptors and I/O

TEST(io, read_write_roundtrip) {
  int FD;
  char Buffer[16] = {0};
  OK(FD = Create("file", NULL));
  CHECK(write(FD, "hello", 5) == 5);
  CHECK(lseek(FD, 0, SEEK_CUR) == 5);
  CHECK(lseek(FD, 0, SEEK_SET) == 0);
  CHECK(read(FD, Buffer, sizeof(Buffer)) == 5 && !memcmp(Buffer, "hello", 5));
  CHECK(read(FD, Buffer, sizeof(Buffer)) == 0);
  CHECK(write(FD, "", 0) == 0);
}

TEST(io, shared_readonly_file_mapping) {
  // ldconfig maps libraries with PROT_READ, MAP_SHARED on a read-only
  // descriptor; the mapping follows later writes through other descriptors.
  int Writer, Reader;
  OK(Writer = Create("mapped", "abcdef"));
  OK(Reader = openat(Dir, "mapped", O_RDONLY | O_CLOEXEC));
  char *Map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, Reader, 0);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  CHECK(!memcmp(Map, "abcdef", 6) && Map[6] == 0);
  CHECK(pwrite(Writer, "XY", 2, 1) == 2);
  CHECK(!memcmp(Map, "aXYdef", 6));
  CHECK(close(Reader) == 0); // the mapping keeps the file
  CHECK(pwrite(Writer, "Z", 1, 5) == 1);
  CHECK(!memcmp(Map, "aXYdeZ", 6));
  FAILS(mprotect(Map, 4096, PROT_READ | PROT_WRITE), EACCES);
  CHECK(munmap(Map, 4096) == 0);
  OK(Reader = openat(Dir, "mapped", O_RDONLY | O_CLOEXEC));
  FAILS(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, Reader, 0) == MAP_FAILED ? -1 : 0, EACCES);
  close(Reader);
  close(Writer);
}

TEST(io, positional_io_keeps_offset) {
  int FD;
  char Buffer[4] = {0};
  OK(FD = Create("file", "abcdef"));
  CHECK(lseek(FD, 1, SEEK_SET) == 1);
  CHECK(pwrite(FD, "XY", 2, 3) == 2);
  CHECK(pread(FD, Buffer, 3, 2) == 3 && !memcmp(Buffer, "cXY", 3));
  CHECK(lseek(FD, 0, SEEK_CUR) == 1);
  FAILS(pread(FD, Buffer, 1, -1), EINVAL);
  FAILS(pwrite(FD, Buffer, 1, -1), EINVAL);
  CHECK(pread(FD, Buffer, 4, 100) == 0);
}

TEST(io, seek_whence) {
  int FD;
  OK(FD = Create("file", "0123456789"));
  CHECK(lseek(FD, -3, SEEK_END) == 7);
  CHECK(lseek(FD, 2, SEEK_CUR) == 9);
  CHECK(lseek(FD, 100, SEEK_SET) == 100);
  FAILS(lseek(FD, -1, SEEK_SET), EINVAL);
  FAILS(lseek(FD, -20, SEEK_END), EINVAL);
  FAILS(lseek(FD, 0, 42), EINVAL);
  int Pipe[2];
  OK(pipe(Pipe));
  FAILS(lseek(Pipe[0], 0, SEEK_CUR), ESPIPE);
}

TEST(io, write_past_eof_zero_fills) {
  int FD;
  char Buffer[8];
  OK(FD = Create("file", "ab"));
  CHECK(pwrite(FD, "z", 1, 6) == 1);
  struct stat Info;
  OK(fstat(FD, &Info));
  CHECK(Info.st_size == 7);
  CHECK(pread(FD, Buffer, 7, 0) == 7 && !memcmp(Buffer, "ab\0\0\0\0z", 7));
}

TEST(io, append_mode) {
  int FD;
  OK(FD = openat(Dir, "file", O_RDWR | O_CREAT | O_APPEND, 0644));
  CHECK(write(FD, "abc", 3) == 3);
  CHECK(lseek(FD, 0, SEEK_SET) == 0);
  CHECK(write(FD, "de", 2) == 2);
  CHECK(lseek(FD, 0, SEEK_CUR) == 5);
  // Linux appends pwrite on O_APPEND descriptors regardless of the offset.
  CHECK(pwrite(FD, "f", 1, 0) == 1);
  CHECK(Contents("file", "abcdef"));
}

TEST(io, vectored_io) {
  int FD;
  char A[3] = {0}, B[4] = {0};
  struct iovec Out[3] = {{"ab", 2}, {NULL, 0}, {"cdef", 4}};
  struct iovec In[2] = {{A, sizeof(A)}, {B, sizeof(B)}};
  OK(FD = Create("file", NULL));
  CHECK(writev(FD, Out, 3) == 6);
  CHECK(preadv(FD, In, 2, 0) == 6 && !memcmp(A, "abc", 3) && !memcmp(B, "def", 3));
  CHECK(writev(FD, Out, 0) == 0);
  static struct iovec Many[IOV_MAX + 1];
  FAILS(writev(FD, Many, IOV_MAX + 1), EINVAL);
  struct iovec Bad = {(void *) 1, 4};
  CHECK(lseek(FD, 0, SEEK_SET) == 0);
  FAILS(readv(FD, &Bad, 1), EFAULT);
  // At EOF nothing is copied, so the buffer is never touched.
  CHECK(lseek(FD, 0, SEEK_END) == 6 && readv(FD, &Bad, 1) == 0);
}

TEST(io, preadv2_pwritev2_flags) {
  int FD;
  char Buffer[8] = {0};
  struct iovec Vector = {"xy", 2};
  OK(FD = openat(Dir, "file", O_RDWR | O_CREAT | O_APPEND, 0644));
  CHECK(write(FD, "abcd", 4) == 4);
  // RWF_NOAPPEND writes at the offset despite O_APPEND.
  CHECK(pwritev2(FD, &Vector, 1, 1, RWF_NOAPPEND) == 2);
  CHECK(Contents("file", "axyd"));
  int Plain;
  OK(Plain = openat(Dir, "file", O_RDWR | O_CLOEXEC));
  CHECK(pwritev2(Plain, &Vector, 1, 0, RWF_APPEND) == 2);
  CHECK(Contents("file", "axydxy"));
  // Offset -1 uses and advances the file offset.
  CHECK(lseek(Plain, 2, SEEK_SET) == 2);
  struct iovec In = {Buffer, 2};
  CHECK(preadv2(Plain, &In, 1, -1, 0) == 2 && !memcmp(Buffer, "yd", 2));
  CHECK(lseek(Plain, 0, SEEK_CUR) == 4);
  FAILS(pwritev2(Plain, &Vector, 1, 0, RWF_APPEND | RWF_NOAPPEND), EINVAL);
  FAILS(preadv2(Plain, &In, 1, 0, 0x40000000), EOPNOTSUPP);
}

TEST(io, access_mode_enforced) {
  int ReadOnly, WriteOnly;
  char Byte;
  OK(MakeFile("file", "x"));
  OK(ReadOnly = openat(Dir, "file", O_RDONLY));
  OK(WriteOnly = openat(Dir, "file", O_WRONLY));
  FAILS(write(ReadOnly, "y", 1), EBADF);
  FAILS(read(WriteOnly, &Byte, 1), EBADF);
  CHECK((fcntl(ReadOnly, F_GETFL) & O_ACCMODE) == O_RDONLY);
  CHECK((fcntl(WriteOnly, F_GETFL) & O_ACCMODE) == O_WRONLY);
}

TEST(io, access_mode_three) {
  // Access mode 3 checks read and write permission but permits neither.
  int FD;
  char Byte;
  OK(MakeFile("file", "x"));
  OK(FD = openat(Dir, "file", 3));
  FAILS(read(FD, &Byte, 1), EBADF);
  FAILS(write(FD, "y", 1), EBADF);
  CHECK((fcntl(FD, F_GETFL) & O_ACCMODE) == 3);
  OK(fchmod(FD, 0444));
  if (geteuid() != 0) FAILS(openat(Dir, "file", 3), EACCES);
  FAILS(openat(Dir, ".", 3), EISDIR);
}

TEST(io, legacy_open_ignores_unknown_flags) {
  OK(MakeFile("file", "x"));
  OK(openat(Dir, "file", O_RDONLY | 0x40000000));
  OK(open("file", O_RDONLY | 0x40000000));
  struct open_how How = {.flags = O_RDONLY | 0x40000000};
  FAILS(syscall(SYS_openat2, Dir, "file", &How, sizeof(How)), EINVAL);
}

TEST(io, bad_descriptors) {
  char Byte;
  FAILS(read(-1, &Byte, 1), EBADF);
  FAILS(write(999, &Byte, 1), EBADF);
  FAILS(close(999), EBADF);
  int FD;
  OK(FD = Create("file", NULL));
  OK(close(FD));
  FAILS(close(FD), EBADF);
  FAILS(fstat(FD, &(struct stat) {0}), EBADF);
}

TEST(io, buffer_faults) {
  int FD;
  OK(FD = Create("file", "abc"));
  CHECK(lseek(FD, 0, SEEK_SET) == 0);
  FAILS(syscall(SYS_read, FD, (void *) 1, 3), EFAULT);
  FAILS(syscall(SYS_write, FD, (void *) 1, 3), EFAULT);
  CHECK(lseek(FD, 0, SEEK_CUR) == 0);
}

TEST(io, dup_shares_description) {
  int FD, Copy;
  OK(FD = Create("file", "abcdef"));
  OK(Copy = dup(FD));
  CHECK(lseek(FD, 2, SEEK_SET) == 2);
  CHECK(lseek(Copy, 0, SEEK_CUR) == 2);
  OK(fcntl(FD, F_SETFL, O_APPEND));
  CHECK(fcntl(Copy, F_GETFL) & O_APPEND);
  CHECK(!(fcntl(Copy, F_GETFD) & FD_CLOEXEC));
}

TEST(io, dup2_dup3_semantics) {
  int FD;
  OK(FD = Create("file", NULL));
  CHECK(dup2(FD, FD) == FD);
  FAILS(dup3(FD, FD, 0), EINVAL);
  FAILS(dup3(FD, 100, 0x4), EINVAL);
  CHECK(dup3(FD, 100, O_CLOEXEC) == 100);
  CHECK(fcntl(100, F_GETFD) == FD_CLOEXEC);
  FAILS(dup2(FD, -1), EBADF);
  FAILS(dup2(-1, 50), EBADF);
  CHECK(fcntl(FD, F_DUPFD, 200) >= 200);
  CHECK(fcntl(FD, F_DUPFD_CLOEXEC, 300) == 300 && fcntl(300, F_GETFD) == FD_CLOEXEC);
}

TEST(io, close_range) {
  int FD;
  OK(FD = Create("file", NULL));
  CHECK(dup2(FD, 400) == 400 && dup2(FD, 401) == 401 && dup2(FD, 403) == 403);
  OK(syscall(SYS_close_range, 401, 402, 0));
  CHECK(fcntl(400, F_GETFD) >= 0);
  FAILS(fcntl(401, F_GETFD), EBADF);
  CHECK(fcntl(403, F_GETFD) >= 0);
  OK(syscall(SYS_close_range, 400, 403, 4 /* CLOSE_RANGE_CLOEXEC */));
  CHECK(fcntl(400, F_GETFD) == FD_CLOEXEC);
  FAILS(syscall(SYS_close_range, 10, 5, 0), EINVAL);
  FAILS(syscall(SYS_close_range, 400, 403, 0x80), EINVAL);
}

TEST(io, status_flags) {
  int FD;
  OK(FD = openat(Dir, "file", O_RDWR | O_CREAT | O_NONBLOCK, 0644));
  CHECK(fcntl(FD, F_GETFL) & O_NONBLOCK);
  OK(fcntl(FD, F_SETFL, O_APPEND));
  CHECK((fcntl(FD, F_GETFL) & (O_APPEND | O_NONBLOCK | O_ACCMODE)) == (O_APPEND | O_RDWR));
  // F_SETFL cannot change the access mode.
  OK(fcntl(FD, F_SETFL, O_RDONLY));
  CHECK((fcntl(FD, F_GETFL) & O_ACCMODE) == O_RDWR);
}

TEST(io, poll_regular_file) {
  int FD;
  OK(MakeFile("file", "x"));
  OK(FD = openat(Dir, "file", O_RDONLY));
  struct pollfd Entry = {FD, POLLIN | POLLOUT, 0};
  CHECK(poll(&Entry, 1, 0) == 1 && (Entry.revents & (POLLIN | POLLOUT)) == (POLLIN | POLLOUT));
}

TEST(io, sync_calls) {
  int FD, ReadOnly;
  OK(FD = Create("file", "data"));
  OK(ReadOnly = openat(Dir, "file", O_RDONLY));
  OK(fsync(FD));
  OK(fdatasync(FD));
  OK(fsync(ReadOnly));
  OK(syncfs(FD));
  OK(sync_file_range(FD, 0, 0, SYNC_FILE_RANGE_WRITE));
  FAILS(sync_file_range(FD, 0, 0, 0x8), EINVAL);
  FAILS(sync_file_range(FD, -1, 0, 0), EINVAL);
  int Pipe[2];
  OK(pipe(Pipe));
  FAILS(fsync(Pipe[0]), EINVAL);
  sync();
}

TEST(io, directory_reads) {
  int FD;
  char Byte;
  OK(FD = openat(Dir, ".", O_RDONLY | O_DIRECTORY));
  FAILS(read(FD, &Byte, 1), EISDIR);
  FAILS(openat(Dir, ".", O_WRONLY), EISDIR);
  FAILS(openat(Dir, ".", O_RDWR), EISDIR);
}

TEST(io, getdents_lists_entries) {
  OK(MakeFile("one", NULL));
  OK(mkdirat(Dir, "two", 0755));
  OK(symlinkat("one", Dir, "three"));
  int FD;
  OK(FD = openat(Dir, ".", O_RDONLY | O_DIRECTORY));
  DIR *Stream = fdopendir(FD);
  CHECK(Stream != NULL);
  if (!Stream) return;
  unsigned Seen = 0, Total = 0;
  for (struct dirent *Entry; (Entry = readdir(Stream));) {
    ++Total;
    if (!strcmp(Entry->d_name, "one")) Seen |= Entry->d_type == DT_REG || Entry->d_type == DT_UNKNOWN ? 1 : 0;
    if (!strcmp(Entry->d_name, "two")) Seen |= Entry->d_type == DT_DIR || Entry->d_type == DT_UNKNOWN ? 2 : 0;
    if (!strcmp(Entry->d_name, "three")) Seen |= Entry->d_type == DT_LNK || Entry->d_type == DT_UNKNOWN ? 4 : 0;
    if (!strcmp(Entry->d_name, ".") || !strcmp(Entry->d_name, "..")) Seen |= 8;
  }
  CHECK(Seen == 15 && Total == 5);
  // Rewinding lists the same entries again.
  rewinddir(Stream);
  unsigned Again = 0;
  while (readdir(Stream)) ++Again;
  CHECK(Again == Total);
  closedir(Stream);
}

TEST(io, getdents_buffer_too_small) {
  int FD;
  char Buffer[8];
  OK(FD = openat(Dir, ".", O_RDONLY | O_DIRECTORY));
  FAILS(syscall(SYS_getdents64, FD, Buffer, sizeof(Buffer)), EINVAL);
  int File;
  OK(File = Create("file", NULL));
  char Large[512];
  FAILS(syscall(SYS_getdents64, File, Large, sizeof(Large)), ENOTDIR);
}

TEST(io, flock_conflicts) {
  int First, Second;
  OK(MakeFile("file", NULL));
  OK(First = openat(Dir, "file", O_RDONLY));
  OK(Second = openat(Dir, "file", O_RDONLY));
  OK(flock(First, LOCK_SH));
  OK(flock(Second, LOCK_SH | LOCK_NB));
  FAILS(flock(Second, LOCK_EX | LOCK_NB), EWOULDBLOCK);
  OK(flock(First, LOCK_UN));
  OK(flock(Second, LOCK_EX | LOCK_NB));
  FAILS(flock(First, LOCK_SH | LOCK_NB), EWOULDBLOCK);
  // Closing the last descriptor of a description releases its lock.
  OK(close(Second));
  OK(flock(First, LOCK_EX | LOCK_NB));
  FAILS(flock(First, 0), EINVAL);
}

TEST(io, record_locks_between_processes) {
  int FD;
  OK(FD = Create("file", "0123456789"));
  struct flock Lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 2, .l_len = 3};
  OK(fcntl(FD, F_SETLK, &Lock));
  fflush(stdout);
  const pid_t Child = fork();
  if (Child == 0) {
    struct flock Query = {.l_type = F_RDLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0};
    int Bad = fcntl(FD, F_GETLK, &Query) != 0 || Query.l_type != F_WRLCK || Query.l_start != 2 ||
              Query.l_len != 3 || Query.l_pid != getppid();
    struct flock Try = {.l_type = F_RDLCK, .l_whence = SEEK_SET, .l_start = 4, .l_len = 1};
    Bad |= fcntl(FD, F_SETLK, &Try) != -1 || (errno != EAGAIN && errno != EACCES);
    Try.l_start = 5;
    Bad |= fcntl(FD, F_SETLK, &Try) != 0;
    _exit(Bad);
  }
  int Status;
  CHECK(waitpid(Child, &Status, 0) == Child && WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  struct flock Invalid = {.l_type = 42, .l_whence = SEEK_SET};
  FAILS(fcntl(FD, F_SETLK, &Invalid), EINVAL);
  int ReadOnly;
  OK(ReadOnly = openat(Dir, "file", O_RDONLY));
  struct flock Write = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
  FAILS(fcntl(ReadOnly, F_SETLK, &Write), EBADF);
}

TEST(io, ofd_locks) {
  int First, Second;
  OK(MakeFile("file", "data"));
  OK(First = openat(Dir, "file", O_RDWR));
  OK(Second = openat(Dir, "file", O_RDWR));
  struct flock Lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1};
  OK(fcntl(First, F_OFD_SETLK, &Lock));
  // OFD locks conflict between descriptions in the same process.
  FAILS(fcntl(Second, F_OFD_SETLK, &Lock), EAGAIN);
  struct flock Query = Lock;
  OK(fcntl(Second, F_OFD_GETLK, &Query));
  CHECK(Query.l_type == F_WRLCK && Query.l_pid == -1);
  Lock.l_pid = 1;
  FAILS(fcntl(First, F_OFD_SETLK, &Lock), EINVAL);
}

// ---------------------------------------------------------------------------
// Paths and links

TEST(path, create_exclusive) {
  OK(MakeFile("file", NULL));
  FAILS(openat(Dir, "file", O_CREAT | O_EXCL | O_WRONLY, 0644), EEXIST);
  OK(symlinkat("missing", Dir, "dangling"));
  // O_EXCL never follows a final symlink, even a dangling one.
  FAILS(openat(Dir, "dangling", O_CREAT | O_EXCL | O_WRONLY, 0644), EEXIST);
  // Without O_EXCL the dangling link's target is created.
  OK(openat(Dir, "dangling", O_CREAT | O_WRONLY, 0644));
  CHECK(Exists("missing"));
}

TEST(path, open_type_errors) {
  OK(MakeFile("file", NULL));
  OK(symlinkat("file", Dir, "link"));
  FAILS(openat(Dir, "file", O_RDONLY | O_DIRECTORY), ENOTDIR);
  FAILS(openat(Dir, "file/", O_RDONLY), ENOTDIR);
  FAILS(openat(Dir, "file/x", O_RDONLY), ENOTDIR);
  FAILS(openat(Dir, "link", O_RDONLY | O_NOFOLLOW), ELOOP);
  FAILS(openat(Dir, "missing", O_RDONLY), ENOENT);
  FAILS(openat(Dir, "", O_RDONLY), ENOENT);
  FAILS(openat(Dir, "new/", O_CREAT | O_WRONLY, 0644), EISDIR);
  FAILS(openat(Dir, "missing/file", O_CREAT | O_WRONLY, 0644), ENOENT);
  FAILS(openat(Dir, ".", O_CREAT | O_RDONLY, 0644), EISDIR);
  FAILS(openat(Dir, "file", O_CREAT | O_DIRECTORY | O_RDONLY, 0644), EINVAL);
}

TEST(path, name_too_long) {
  char Name[300];
  memset(Name, 'n', sizeof(Name) - 1);
  Name[sizeof(Name) - 1] = 0;
  FAILS(openat(Dir, Name, O_CREAT | O_WRONLY, 0644), ENAMETOOLONG);
  FAILS(mkdirat(Dir, Name, 0755), ENAMETOOLONG);
  Name[255] = 0;
  if (MakeFile(Name, NULL) < 0) {
    // Report the longest name the backend accepts in this directory.
    const int Error = errno;
    size_t Low = 0, High = 255;
    while (Low + 1 < High) {
      const size_t Middle = (Low + High) / 2;
      char Probe[256];
      memset(Probe, 'p', Middle);
      Probe[Middle] = 0;
      if (MakeFile(Probe, NULL) >= 0) Low = Middle;
      else High = Middle;
    }
    char Where[PATH_MAX];
    printf("#   255-byte name failed with %s; longest created: %zu bytes in %s\n", ErrnoName(Error), Low,
           getcwd(Where, sizeof(Where)) ? Where : "?");
    Fail(__LINE__, "MakeFile(255-byte name)", -1, Error, 0);
  }
}

// Linux names may hold any byte but '/' and NUL; FAT and Horizon reject
// " * : < > ? \ | and control characters (dpkg writes "libc6:amd64.list").
static int HasEntry(const char *Name) {
  const int FD = openat(Dir, ".", O_RDONLY | O_DIRECTORY);
  DIR *Stream = FD < 0 ? NULL : fdopendir(FD);
  int Found = 0;
  for (struct dirent *Entry; Stream && (Entry = readdir(Stream));) Found |= !strcmp(Entry->d_name, Name);
  if (Stream) closedir(Stream);
  return Found;
}

TEST(path, sd_reserved_characters) {
  static const char *const Names[] = {"pkg:amd64.list", "q?mark", "star*", "quote\"", "lt<gt>", "pipe|", "back\\slash", "ctl\001x"};
  for (size_t Index = 0; Index < sizeof(Names) / sizeof(Names[0]); ++Index) {
    int FD;
    OK(FD = openat(Dir, Names[Index], O_RDWR | O_CREAT | O_EXCL, 0640));
    CHECK(write(FD, "named", 5) == 5);
    OK(close(FD));
    struct stat Info;
    OK(fstatat(Dir, Names[Index], &Info, 0));
    CHECK(S_ISREG(Info.st_mode) && Info.st_size == 5 && (Info.st_mode & 07777) == 0640);
    CHECK(HasEntry(Names[Index]));
  }
  // Missing reserved names are ENOENT, not a backend error.
  FAILS(openat(Dir, "missing:file", O_RDONLY), ENOENT);
  FAILS(unlinkat(Dir, "missing:file", 0), ENOENT);
  FAILS(mkdirat(Dir, "missing:dir/child", 0755), ENOENT);
  // Directories, their children, links and renames in both directions.
  OK(mkdirat(Dir, "dir:x", 0750));
  OK(MakeFile("dir:x/a:b", "inside"));
  OK(MakeFile("dir:x/plain", "plain"));
  OK(symlinkat("target:t", Dir, "link:l"));
  char Target[32] = {0};
  CHECK(readlinkat(Dir, "link:l", Target, sizeof(Target)) == 8 && !strcmp(Target, "target:t"));
  OK(linkat(Dir, "pkg:amd64.list", Dir, "hard:link", 0));
  struct stat Info;
  OK(fstatat(Dir, "hard:link", &Info, 0));
  CHECK(Info.st_nlink == 2);
  OK(renameat(Dir, "pkg:amd64.list", Dir, "native-name"));
  OK(renameat(Dir, "native-name", Dir, "back:again"));
  char Buffer[8] = {0};
  int FD;
  OK(FD = openat(Dir, "back:again", O_RDONLY));
  CHECK(read(FD, Buffer, 5) == 5 && !memcmp(Buffer, "named", 5));
  OK(close(FD));
  OK(renameat(Dir, "dir:x", Dir, "dir-plain"));
  OK(fstatat(Dir, "dir-plain/a:b", &Info, 0));
  OK(renameat(Dir, "dir-plain", Dir, "dir:y"));
  OK(fstatat(Dir, "dir:y/plain", &Info, 0));
  for (size_t Index = 1; Index < sizeof(Names) / sizeof(Names[0]); ++Index) OK(unlinkat(Dir, Names[Index], 0));
  OK(unlinkat(Dir, "back:again", 0));
  OK(unlinkat(Dir, "hard:link", 0));
  OK(unlinkat(Dir, "link:l", 0));
  OK(unlinkat(Dir, "dir:y/a:b", 0));
  OK(unlinkat(Dir, "dir:y/plain", 0));
  OK(unlinkat(Dir, "dir:y", AT_REMOVEDIR));
  CHECK(!HasEntry("dir:y"));
}

TEST(path, mkdir_rmdir) {
  OK(mkdirat(Dir, "dir", 0755));
  FAILS(mkdirat(Dir, "dir", 0755), EEXIST);
  OK(MakeFile("dir/file", NULL));
  FAILS(unlinkat(Dir, "dir", AT_REMOVEDIR), ENOTEMPTY);
  FAILS(unlinkat(Dir, "dir", 0), EISDIR);
  FAILS(unlinkat(Dir, "dir/file", AT_REMOVEDIR), ENOTDIR);
  FAILS(rmdir("."), EINVAL);
  FAILS(rmdir("dir/.."), ENOTEMPTY);
  OK(unlinkat(Dir, "dir/file", 0));
  OK(unlinkat(Dir, "dir", AT_REMOVEDIR));
  FAILS(unlinkat(Dir, "dir", AT_REMOVEDIR), ENOENT);
  FAILS(unlinkat(Dir, "x", 0x1), EINVAL);
}

TEST(path, unlink_open_file) {
  int FD;
  char Buffer[8] = {0};
  OK(FD = Create("file", "still"));
  OK(unlinkat(Dir, "file", 0));
  CHECK(!Exists("file"));
  CHECK(pread(FD, Buffer, 5, 0) == 5 && !memcmp(Buffer, "still", 5));
  CHECK(pwrite(FD, "!", 1, 5) == 1);
  struct stat Info;
  OK(fstat(FD, &Info));
  CHECK(Info.st_nlink == 0 && Info.st_size == 6);
  // A new file with the same name is a different inode.
  int Fresh;
  OK(Fresh = Create("file", "new"));
  struct stat Other;
  OK(fstat(Fresh, &Other));
  CHECK(Other.st_ino != Info.st_ino);
}

TEST(path, rename_files) {
  OK(MakeFile("a", "A"));
  OK(MakeFile("b", "B"));
  const ino_t Inode = InodeOf("a", 0);
  OK(renameat(Dir, "a", Dir, "b"));
  CHECK(!Exists("a") && Contents("b", "A") && InodeOf("b", 0) == Inode);
  OK(renameat(Dir, "b", Dir, "b"));
  CHECK(Contents("b", "A"));
  FAILS(renameat(Dir, "missing", Dir, "c"), ENOENT);
  OK(mkdirat(Dir, "dir", 0755));
  FAILS(renameat(Dir, "b", Dir, "dir"), EISDIR);
  FAILS(renameat(Dir, "dir", Dir, "b"), ENOTDIR);
  FAILS(renameat(Dir, "b", Dir, "missing/b"), ENOENT);
  OK(renameat(Dir, "b", Dir, "dir/b"));
  CHECK(Contents("dir/b", "A"));
}

TEST(path, rename_directories) {
  OK(mkdirat(Dir, "a", 0755));
  OK(mkdirat(Dir, "a/sub", 0755));
  OK(mkdirat(Dir, "empty", 0755));
  OK(mkdirat(Dir, "full", 0755));
  OK(MakeFile("full/file", NULL));
  FAILS(renameat(Dir, "a", Dir, "a/sub/inside"), EINVAL);
  FAILS(renameat(Dir, "a", Dir, "full"), ENOTEMPTY);
  OK(renameat(Dir, "a", Dir, "empty"));
  CHECK(!Exists("a") && Exists("empty/sub"));
  FAILS(renameat(Dir, ".", Dir, "x"), EBUSY);
  FAILS(renameat(Dir, "empty/sub/..", Dir, "x"), EBUSY);
  // A directory renamed while open keeps its handle.
  int FD;
  OK(FD = openat(Dir, "empty", O_RDONLY | O_DIRECTORY));
  OK(renameat(Dir, "empty", Dir, "moved"));
  OK(MakeFile("moved/through-handle", NULL));
  CHECK(faccessat(FD, "through-handle", F_OK, 0) == 0);
}

TEST(path, rename_flags) {
  OK(MakeFile("a", "A"));
  OK(MakeFile("b", "B"));
  FAILS(renameat2(Dir, "a", Dir, "b", RENAME_NOREPLACE), EEXIST);
  OK(renameat2(Dir, "a", Dir, "c", RENAME_NOREPLACE));
  OK(renameat2(Dir, "c", Dir, "b", RENAME_EXCHANGE));
  CHECK(Contents("b", "A") && Contents("c", "B"));
  FAILS(renameat2(Dir, "b", Dir, "missing", RENAME_EXCHANGE), ENOENT);
  FAILS(renameat2(Dir, "b", Dir, "c", RENAME_EXCHANGE | RENAME_NOREPLACE), EINVAL);
  FAILS(renameat2(Dir, "b", Dir, "c", 0x100), EINVAL);
  // Exchange works between a file and a directory.
  OK(mkdirat(Dir, "dir", 0755));
  OK(renameat2(Dir, "b", Dir, "dir", RENAME_EXCHANGE));
  struct stat Info;
  OK(fstatat(Dir, "b", &Info, 0));
  CHECK(S_ISDIR(Info.st_mode) && Contents("dir", "A"));
}

TEST(path, hard_links) {
  struct stat Info;
  OK(MakeFile("a", "data"));
  OK(linkat(Dir, "a", Dir, "b", 0));
  OK(fstatat(Dir, "a", &Info, 0));
  CHECK(Info.st_nlink == 2 && Info.st_ino == InodeOf("b", 0));
  FAILS(linkat(Dir, "a", Dir, "b", 0), EEXIST);
  OK(mkdirat(Dir, "dir", 0755));
  FAILS(linkat(Dir, "dir", Dir, "dir2", 0), EPERM);
  FAILS(linkat(Dir, "missing", Dir, "c", 0), ENOENT);
  FAILS(linkat(Dir, "a", Dir, "c", 0x1), EINVAL);
  OK(unlinkat(Dir, "a", 0));
  OK(fstatat(Dir, "b", &Info, 0));
  CHECK(Info.st_nlink == 1 && Contents("b", "data"));
  // linkat follows a symlink source only with AT_SYMLINK_FOLLOW.
  OK(symlinkat("b", Dir, "s"));
  OK(linkat(Dir, "s", Dir, "s-link", 0));
  OK(linkat(Dir, "s", Dir, "s-target", AT_SYMLINK_FOLLOW));
  CHECK(InodeOf("s-link", AT_SYMLINK_NOFOLLOW) == InodeOf("s", AT_SYMLINK_NOFOLLOW));
  CHECK(InodeOf("s-target", AT_SYMLINK_NOFOLLOW) == InodeOf("b", 0));
}

TEST(path, symlinks) {
  char Buffer[64];
  OK(mkdirat(Dir, "dir", 0755));
  OK(MakeFile("dir/target", "T"));
  OK(symlinkat("target", Dir, "dir/rel"));
  OK(symlinkat("dir/target", Dir, "top"));
  CHECK(Contents("dir/rel", "T") && Contents("top", "T"));
  CHECK(readlinkat(Dir, "top", Buffer, sizeof(Buffer)) == 10 && !memcmp(Buffer, "dir/target", 10));
  CHECK(readlinkat(Dir, "top", Buffer, 3) == 3 && !memcmp(Buffer, "dir", 3));
  FAILS(readlinkat(Dir, "dir/target", Buffer, sizeof(Buffer)), EINVAL);
  FAILS(readlinkat(Dir, "top", Buffer, 0), EINVAL);
  FAILS(symlinkat("x", Dir, "top"), EEXIST);
  FAILS(symlinkat("", Dir, "empty"), ENOENT);
  struct stat Info;
  OK(fstatat(Dir, "top", &Info, AT_SYMLINK_NOFOLLOW));
  CHECK(S_ISLNK(Info.st_mode) && Info.st_size == 10);
  OK(symlinkat("loop2", Dir, "loop1"));
  OK(symlinkat("loop1", Dir, "loop2"));
  FAILS(openat(Dir, "loop1", O_RDONLY), ELOOP);
  OK(symlinkat("dir", Dir, "dirlink"));
  CHECK(Contents("dirlink/target", "T"));
  // A trailing slash forces traversal of the final symlink.
  OK(fstatat(Dir, "dirlink/", &Info, AT_SYMLINK_NOFOLLOW));
  CHECK(S_ISDIR(Info.st_mode));
}

TEST(path, working_directory) {
  char Buffer[PATH_MAX], Original[PATH_MAX];
  CHECK(getcwd(Original, sizeof(Original)) != NULL);
  OK(mkdirat(Dir, "sub", 0755));
  OK(chdir("sub"));
  CHECK(getcwd(Buffer, sizeof(Buffer)) && strlen(Buffer) == strlen(Original) + 4 &&
        !strcmp(Buffer + strlen(Original), "/sub"));
  OK(MakeFile("file", NULL));
  FAILS(chdir("../file"), ENOTDIR);
  FAILS(chdir("missing"), ENOENT);
  OK(fchdir(Dir));
  CHECK(getcwd(Buffer, sizeof(Buffer)) && !strcmp(Buffer, Original));
  CHECK(getcwd(Buffer, 1) == NULL && errno == ERANGE);
  int File;
  OK(File = openat(Dir, "file", O_RDONLY));
  FAILS(fchdir(File), ENOTDIR);
}

TEST(path, removed_working_directory) {
  char Buffer[PATH_MAX];
  OK(mkdirat(Dir, "gone", 0755));
  OK(chdir("gone"));
  OK(rmdir("../gone"));
  FAILS(syscall(SYS_getcwd, Buffer, sizeof(Buffer)), ENOENT);
  FAILS(openat(AT_FDCWD, "new", O_CREAT | O_WRONLY, 0644), ENOENT);
  OK(fchdir(Dir));
}

TEST(path, special_nodes) {
  OK(mkfifoat(Dir, "fifo", 0644));
  struct stat Info;
  OK(fstatat(Dir, "fifo", &Info, 0));
  CHECK(S_ISFIFO(Info.st_mode));
  FAILS(openat(Dir, "fifo", O_WRONLY | O_NONBLOCK), ENXIO);
  int Reader, Writer;
  OK(Reader = openat(Dir, "fifo", O_RDONLY | O_NONBLOCK));
  OK(Writer = openat(Dir, "fifo", O_WRONLY | O_NONBLOCK));
  char Byte = 0;
  CHECK(write(Writer, "f", 1) == 1 && read(Reader, &Byte, 1) == 1 && Byte == 'f');
  FAILS(lseek(Reader, 0, SEEK_CUR), ESPIPE);
  FAILS(mknodat(Dir, "fifo", S_IFIFO | 0644, 0), EEXIST);
  FAILS(mknodat(Dir, "bad", 0170000 | 0644, 0), EINVAL);
  OK(mknodat(Dir, "regular", S_IFREG | 0600, 0));
  OK(fstatat(Dir, "regular", &Info, 0));
  CHECK(S_ISREG(Info.st_mode) && (Info.st_mode & 0777) == 0600);
  if (geteuid() != 0) FAILS(mknodat(Dir, "null", S_IFCHR | 0666, makedev(1, 3)), EPERM);
}

TEST(path, openat2_resolve) {
  struct open_how How = {.flags = O_RDONLY};
  OK(mkdirat(Dir, "jail", 0755));
  OK(mkdirat(Dir, "jail/sub", 0755));
  OK(MakeFile("outside", "O"));
  OK(MakeFile("jail/inside", "I"));
  OK(symlinkat("../outside", Dir, "jail/escape"));
  OK(symlinkat("/inside", Dir, "jail/absolute"));
  OK(symlinkat("inside", Dir, "jail/plain"));
  int Jail;
  OK(Jail = openat(Dir, "jail", O_RDONLY | O_DIRECTORY));
#define OPEN2(name) syscall(SYS_openat2, Jail, name, &How, sizeof(How))
  How.resolve = RESOLVE_BENEATH;
  OK(OPEN2("inside"));
  OK(OPEN2("sub/../inside"));
  FAILS(OPEN2("../outside"), EXDEV);
  FAILS(OPEN2("escape"), EXDEV);
  FAILS(OPEN2("/etc"), EXDEV);
  How.resolve = RESOLVE_IN_ROOT;
  int FD = (int) OPEN2("absolute");
  CHECK(FD >= 0);
  How.resolve = RESOLVE_NO_SYMLINKS;
  FAILS(OPEN2("plain"), ELOOP);
  OK(OPEN2("inside"));
  How.resolve = RESOLVE_BENEATH | RESOLVE_IN_ROOT;
  FAILS(OPEN2("inside"), EINVAL);
  How.resolve = 0x80;
  FAILS(OPEN2("inside"), EINVAL);
  How.resolve = 0;
  How.mode = 0644;
  FAILS(OPEN2("inside"), EINVAL);
  How.mode = 0;
  FAILS(syscall(SYS_openat2, Jail, "inside", &How, sizeof(How) - 1), EINVAL);
  struct { struct open_how How; uint64_t Extra; } Larger = {How, 1};
  FAILS(syscall(SYS_openat2, Jail, "inside", &Larger, sizeof(Larger)), E2BIG);
  Larger.Extra = 0;
  OK(syscall(SYS_openat2, Jail, "inside", &Larger, sizeof(Larger)));
#undef OPEN2
}

TEST(path, tmpfile) {
  int FD = openat(Dir, ".", O_TMPFILE | O_RDWR, 0600);
  if (FD < 0 && errno == EOPNOTSUPP) SKIP("O_TMPFILE unsupported by the scratch filesystem");
  CHECK(FD >= 0);
  if (FD < 0) return;
  struct stat Info;
  OK(fstat(FD, &Info));
  CHECK(Info.st_nlink == 0 && S_ISREG(Info.st_mode));
  CHECK(write(FD, "tmp", 3) == 3);
  FAILS(openat(Dir, ".", O_TMPFILE | O_RDONLY, 0600), EINVAL);
  FAILS(openat(Dir, "missing", O_TMPFILE | O_RDWR, 0600), ENOENT);
  OK(MakeFile("file", NULL));
  FAILS(openat(Dir, "file", O_TMPFILE | O_RDWR, 0600), ENOTDIR);
  int Exclusive;
  OK(Exclusive = openat(Dir, ".", O_TMPFILE | O_RDWR | O_EXCL, 0600));
  CHECK(fcntl(FD, F_GETFL) & O_RDWR);
  CHECK(Exclusive >= 0);
}

TEST(path, handles) {
  struct {
    unsigned Bytes;
    int Type;
    unsigned char Data[128];
  } Handle = {0};
  int Mount = 0;
  OK(MakeFile("file", "handle"));
  const int Result = (int) syscall(SYS_name_to_handle_at, Dir, "file", &Handle, &Mount, 0);
  const int Error = errno;
  if (Result < 0 && Error == EOPNOTSUPP) SKIP("filesystem does not export handles");
  CHECK(Result == -1 && Error == EOVERFLOW);
  CHECK(Handle.Bytes > 0 && Handle.Bytes <= 128);
  OK(syscall(SYS_name_to_handle_at, Dir, "file", &Handle, &Mount, 0));
  FAILS(syscall(SYS_name_to_handle_at, Dir, "file", &Handle, &Mount, 0x4), EINVAL);
  FAILS(syscall(SYS_name_to_handle_at, Dir, "missing", &Handle, &Mount, 0), ENOENT);
  Handle.Bytes = 129;
  FAILS(syscall(SYS_name_to_handle_at, Dir, "file", &Handle, &Mount, 0), EINVAL);
  Handle.Bytes = 128;
  OK(syscall(SYS_name_to_handle_at, Dir, "file", &Handle, &Mount, 0));
  if (geteuid() != 0) {
    FAILS(syscall(SYS_open_by_handle_at, Dir, &Handle, O_RDONLY), EPERM);
    return;
  }
  int FD = (int) syscall(SYS_open_by_handle_at, Dir, &Handle, O_RDONLY);
  if (FD < 0 && errno == EPERM) SKIP("CAP_DAC_READ_SEARCH unavailable");
  CHECK(FD >= 0);
  struct stat Info;
  CHECK(FD >= 0 && fstat(FD, &Info) == 0 && Info.st_ino == InodeOf("file", 0));
  OK(unlinkat(Dir, "file", 0));
  if (FD >= 0) close(FD);
  FAILS(syscall(SYS_open_by_handle_at, Dir, &Handle, O_RDONLY), ESTALE);
}

TEST(path, handle_fid_on_procfs) {
  struct {
    unsigned Bytes;
    int Type;
    unsigned char Data[128];
  } Handle = {.Bytes = 128};
  int Mount = 0;
  FAILS(syscall(SYS_name_to_handle_at, AT_FDCWD, "/proc/self", &Handle, &Mount, AT_SYMLINK_FOLLOW), EOPNOTSUPP);
  OK(syscall(SYS_name_to_handle_at, AT_FDCWD, "/proc/self", &Handle, &Mount, AT_SYMLINK_FOLLOW | AT_HANDLE_FID));
  CHECK(Handle.Bytes > 0);
}

// ---------------------------------------------------------------------------
// Metadata and permissions

TEST(meta, stat_basics) {
  struct stat File, Directory, Descriptor;
  int FD;
  OK(FD = Create("file", "12345"));
  OK(mkdirat(Dir, "dir", 0755));
  OK(fstatat(Dir, "file", &File, 0));
  OK(fstatat(Dir, "dir", &Directory, 0));
  OK(fstat(FD, &Descriptor));
  CHECK(S_ISREG(File.st_mode) && File.st_size == 5 && File.st_nlink == 1);
  CHECK(S_ISDIR(Directory.st_mode) && Directory.st_nlink >= 2);
  CHECK(File.st_ino == Descriptor.st_ino && File.st_dev == Descriptor.st_dev);
  CHECK(File.st_ino != Directory.st_ino);
  CHECK(File.st_uid == geteuid() && File.st_gid == getegid());
  CHECK(File.st_blksize > 0);
  FAILS(fstatat(Dir, "file", &File, 0x2), EINVAL);
  FAILS(fstatat(Dir, "missing", &File, 0), ENOENT);
  FAILS(fstatat(Dir, "", &File, 0), ENOENT);
  OK(fstatat(FD, "", &File, AT_EMPTY_PATH));
  CHECK(File.st_ino == Descriptor.st_ino);
}

TEST(meta, statx_fields) {
  struct statx Info;
  OK(MakeFile("file", "abc"));
  OK(statx(Dir, "file", 0, STATX_BASIC_STATS | STATX_BTIME, &Info));
  CHECK((Info.stx_mask & STATX_BASIC_STATS) == STATX_BASIC_STATS);
  CHECK(Info.stx_size == 3 && S_ISREG(Info.stx_mode) && Info.stx_nlink == 1);
  CHECK(Info.stx_ino == InodeOf("file", 0));
  FAILS(statx(Dir, "file", 0x80000000, STATX_BASIC_STATS, &Info), EINVAL);
  FAILS(statx(Dir, "file", 0, 0x80000000U, &Info), EINVAL);
  FAILS(statx(Dir, "file", AT_STATX_FORCE_SYNC | AT_STATX_DONT_SYNC, STATX_BASIC_STATS, &Info), EINVAL);
}

TEST(meta, chmod_and_umask) {
  struct stat Info;
  umask(027);
  OK(MakeFile("file", NULL));
  OK(mkdirat(Dir, "dir", 0777));
  OK(fstatat(Dir, "file", &Info, 0));
  CHECK((Info.st_mode & 07777) == 0640);
  OK(fstatat(Dir, "dir", &Info, 0));
  CHECK((Info.st_mode & 07777) == 0750);
  CHECK(umask(022) == 027);
  OK(fchmodat(Dir, "file", 0604, 0));
  OK(fstatat(Dir, "file", &Info, 0));
  CHECK((Info.st_mode & 07777) == 0604);
  int FD;
  OK(FD = openat(Dir, "file", O_RDONLY));
  OK(fchmod(FD, 0600));
  OK(fstat(FD, &Info));
  CHECK((Info.st_mode & 07777) == 0600);
  // The legacy fchmodat system call has no flags argument.
  OK(syscall(SYS_fchmodat, Dir, "file", 0600, AT_SYMLINK_NOFOLLOW));
  OK(symlinkat("file", Dir, "link"));
  // fchmodat2 with AT_SYMLINK_NOFOLLOW on a symlink is unsupported.
  errno = 0;
  long Result = syscall(SYS_fchmodat2, Dir, "link", 0600, AT_SYMLINK_NOFOLLOW);
  if (!(Result == -1 && errno == ENOSYS)) {
    FAILS(syscall(SYS_fchmodat2, Dir, "link", 0600, AT_SYMLINK_NOFOLLOW), EOPNOTSUPP);
    OK(syscall(SYS_fchmodat2, Dir, "file", 0640, AT_SYMLINK_NOFOLLOW));
    FAILS(syscall(SYS_fchmodat2, Dir, "file", 0640, 0x1), EINVAL);
  }
  int Path;
  OK(Path = openat(Dir, "file", O_PATH));
  FAILS(fchmod(Path, 0644), EBADF);
}

TEST(meta, ctime_and_mtime_updates) {
  struct stat Before, After;
  int FD;
  OK(FD = Create("file", "x"));
  OK(fstat(FD, &Before));
  struct timespec Pause = {0, 20 * 1000 * 1000};
  nanosleep(&Pause, NULL);
  CHECK(write(FD, "y", 1) == 1);
  OK(fstat(FD, &After));
  CHECK(After.st_mtim.tv_sec > Before.st_mtim.tv_sec ||
        (After.st_mtim.tv_sec == Before.st_mtim.tv_sec && After.st_mtim.tv_nsec > Before.st_mtim.tv_nsec));
  Before = After;
  nanosleep(&Pause, NULL);
  OK(fchmod(FD, 0600));
  OK(fstat(FD, &After));
  CHECK(After.st_mtim.tv_sec == Before.st_mtim.tv_sec && After.st_mtim.tv_nsec == Before.st_mtim.tv_nsec);
  CHECK(After.st_ctim.tv_sec > Before.st_ctim.tv_sec ||
        (After.st_ctim.tv_sec == Before.st_ctim.tv_sec && After.st_ctim.tv_nsec > Before.st_ctim.tv_nsec));
  // Creating an entry updates the parent directory's mtime.
  struct stat Parent, Updated;
  OK(fstat(Dir, &Parent));
  nanosleep(&Pause, NULL);
  OK(MakeFile("other", NULL));
  OK(fstat(Dir, &Updated));
  CHECK(Updated.st_mtim.tv_sec > Parent.st_mtim.tv_sec ||
        (Updated.st_mtim.tv_sec == Parent.st_mtim.tv_sec && Updated.st_mtim.tv_nsec > Parent.st_mtim.tv_nsec));
}

TEST(meta, utimensat) {
  struct stat Info;
  OK(MakeFile("file", NULL));
  struct timespec Times[2] = {{1000000000, 123}, {1100000000, 456}};
  OK(utimensat(Dir, "file", Times, 0));
  OK(fstatat(Dir, "file", &Info, 0));
  CHECK(Info.st_atim.tv_sec == 1000000000 && Info.st_mtim.tv_sec == 1100000000);
  struct timespec Omit[2] = {{0, UTIME_OMIT}, {1200000000, 0}};
  OK(utimensat(Dir, "file", Omit, 0));
  OK(fstatat(Dir, "file", &Info, 0));
  CHECK(Info.st_atim.tv_sec == 1000000000 && Info.st_mtim.tv_sec == 1200000000);
  struct timespec Now[2] = {{0, UTIME_NOW}, {0, UTIME_NOW}};
  OK(utimensat(Dir, "file", Now, 0));
  OK(fstatat(Dir, "file", &Info, 0));
  CHECK(Info.st_mtim.tv_sec > 1200000000);
  struct timespec Bad[2] = {{0, 1000000000}, {0, 0}};
  FAILS(utimensat(Dir, "file", Bad, 0), EINVAL);
  FAILS(utimensat(Dir, "file", Times, 0x1), EINVAL);
  OK(symlinkat("file", Dir, "link"));
  OK(utimensat(Dir, "link", Times, AT_SYMLINK_NOFOLLOW));
  OK(fstatat(Dir, "link", &Info, AT_SYMLINK_NOFOLLOW));
  CHECK(Info.st_mtim.tv_sec == 1100000000);
  int FD;
  OK(FD = openat(Dir, "file", O_RDONLY));
  OK(futimens(FD, Times));
  OK(fstat(FD, &Info));
  CHECK(Info.st_mtim.tv_sec == 1100000000 && Info.st_mtim.tv_nsec == 456);
}

TEST(meta, access_checks) {
  OK(MakeFile("file", NULL));
  OK(faccessat(Dir, "file", F_OK, 0));
  OK(faccessat(Dir, "file", R_OK | W_OK, 0));
  FAILS(faccessat(Dir, "file", X_OK, 0), EACCES);
  FAILS(faccessat(Dir, "file", 0x10, 0), EINVAL);
  FAILS(faccessat(Dir, "missing", F_OK, 0), ENOENT);
  OK(fchmodat(Dir, "file", 0, 0));
  if (geteuid() == 0) {
    OK(faccessat(Dir, "file", R_OK | W_OK, 0));
    FAILS(faccessat(Dir, "file", X_OK, 0), EACCES);
  } else {
    FAILS(faccessat(Dir, "file", R_OK, 0), EACCES);
  }
  OK(fchmodat(Dir, "file", 0100, 0));
  OK(faccessat(Dir, "file", X_OK, 0));
  long Result = syscall(SYS_faccessat2, Dir, "file", F_OK, AT_EACCESS | AT_SYMLINK_NOFOLLOW);
  if (!(Result == -1 && errno == ENOSYS)) CHECK(Result == 0);
  FAILS(syscall(SYS_faccessat2, Dir, "file", F_OK, 0x1), EINVAL);
}

static void LockedSearchChecks(void) {
  FAILS(open("locked/file", O_RDONLY), EACCES);
  FAILS(faccessat(AT_FDCWD, "locked/file", F_OK, 0), EACCES);
}

TEST(meta, search_permission) {
  if (geteuid() == 0 && !CanSwitchIDs) SKIP("root without ID switching bypasses search permission");
  OK(mkdirat(Dir, "locked", 0755));
  OK(MakeFile("locked/file", "x"));
  OK(fchmodat(Dir, "locked", 0600, 0));
  if (geteuid() == 0) {
    OK(fchmodat(Dir, ".", 0777, 0));
    AsUser(4242, 4242, LockedSearchChecks);
  } else {
    LockedSearchChecks();
  }
  OK(fchmodat(Dir, "locked", 0755, 0));
}

TEST(meta, ownership_changes) {
  OK(MakeFile("file", NULL));
  // Unchanged IDs are always permitted.
  OK(fchownat(Dir, "file", -1, -1, 0));
  OK(fchownat(Dir, "file", geteuid(), getegid(), 0));
  FAILS(fchownat(Dir, "file", -1, -1, 0x2), EINVAL);
  FAILS(fchownat(Dir, "missing", -1, -1, 0), ENOENT);
  if (geteuid() != 0) {
    FAILS(fchownat(Dir, "file", geteuid() + 1, -1, 0), EPERM);
    return;
  }
  OK(fchownat(Dir, "file", 4242, 4243, 0));
  struct stat Info;
  OK(fstatat(Dir, "file", &Info, 0));
  CHECK(Info.st_uid == 4242 && Info.st_gid == 4243);
  // chown clears set-user-ID and set-group-ID bits on executables.
  OK(fchmodat(Dir, "file", 06755, 0));
  OK(fchownat(Dir, "file", 4242, 4243, 0));
  OK(fstatat(Dir, "file", &Info, 0));
  CHECK((Info.st_mode & 07777) == 0755);
  OK(symlinkat("file", Dir, "link"));
  OK(fchownat(Dir, "link", 4244, -1, AT_SYMLINK_NOFOLLOW));
  OK(fstatat(Dir, "link", &Info, AT_SYMLINK_NOFOLLOW));
  CHECK(Info.st_uid == 4244);
  OK(fstatat(Dir, "file", &Info, 0));
  CHECK(Info.st_uid == 4242);
}

static void CrossUserChecks(void) {
  // Runs as uid 4242 with gid 4242 and no supplementary groups.
  FAILS(fchownat(AT_FDCWD, "theirs", 4242, -1, 0), EPERM);
  FAILS(fchmodat(AT_FDCWD, "theirs", 0777, 0), EPERM);
  FAILS(open("theirs", O_WRONLY), EACCES);
  OK(open("theirs", O_RDONLY));
  struct timespec Times[2] = {{1, 0}, {1, 0}};
  FAILS(utimensat(AT_FDCWD, "theirs", Times, 0), EPERM);
  FAILS(utimensat(AT_FDCWD, "theirs", NULL, 0), EACCES);
  OK(utimensat(AT_FDCWD, "shared", NULL, 0));
  // Sticky directories protect entries owned by others.
  FAILS(unlinkat(AT_FDCWD, "sticky/theirs", 0), EPERM);
  FAILS(renameat(AT_FDCWD, "sticky/theirs", AT_FDCWD, "sticky/mine"), EPERM);
  OK(MakeFile("sticky/own", NULL));
  OK(unlinkat(AT_FDCWD, "sticky/own", 0));
  // Group ownership may only change to a group the caller belongs to.
  OK(MakeFile("mine", NULL));
  FAILS(fchownat(AT_FDCWD, "mine", -1, 4243, 0), EPERM);
  OK(fchownat(AT_FDCWD, "mine", -1, 4242, 0));
}

TEST(meta, cross_user_permissions) {
  if (!CanSwitchIDs) SKIP("requires root with ID switching");
  OK(fchmodat(Dir, ".", 0777, 0));
  OK(MakeFile("theirs", "x"));
  OK(MakeFile("shared", NULL));
  OK(fchmodat(Dir, "shared", 0666, 0));
  OK(mkdirat(Dir, "sticky", 01777));
  OK(fchmodat(Dir, "sticky", 01777, 0));
  OK(MakeFile("sticky/theirs", NULL));
  AsUser(4242, 4242, CrossUserChecks);
}

static void SetgidChecks(void) {
  struct stat Info;
  OK(MakeFile("group/file", NULL));
  OK(mkdirat(AT_FDCWD, "group/sub", 0755));
  OK(fstatat(AT_FDCWD, "group/file", &Info, 0));
  CHECK(Info.st_gid == 4300);
  OK(fstatat(AT_FDCWD, "group/sub", &Info, 0));
  CHECK(Info.st_gid == 4300 && (Info.st_mode & S_ISGID));
  // Writing clears set-group-ID on a group-executable file.
  OK(fchmodat(AT_FDCWD, "mine", 02775, 0));
  int FD;
  OK(FD = open("mine", O_WRONLY));
  CHECK(write(FD, "x", 1) == 1);
  OK(fstat(FD, &Info));
  CHECK(!(Info.st_mode & S_ISGID));
}

TEST(meta, setgid_inheritance) {
  if (!CanSwitchIDs) SKIP("requires root with ID switching");
  OK(fchmodat(Dir, ".", 0777, 0));
  OK(mkdirat(Dir, "group", 0777));
  OK(fchownat(Dir, "group", 0, 4300, 0));
  OK(fchmodat(Dir, "group", 02777, 0));
  OK(MakeFile("mine", NULL));
  OK(fchownat(Dir, "mine", 4242, 4300, 0));
  AsUser(4242, 4242, SetgidChecks);
}

TEST(meta, statfs) {
  struct statfs Info, Descriptor;
  OK(statfs(".", &Info));
  CHECK(Info.f_bsize > 0 && Info.f_blocks >= Info.f_bfree && Info.f_bfree >= Info.f_bavail);
  CHECK(Info.f_namelen >= 255);
  OK(fstatfs(Dir, &Descriptor));
  CHECK(Descriptor.f_type == Info.f_type);
  FAILS(statfs("missing", &Info), ENOENT);
  OK(statfs("/proc", &Info));
  CHECK(Info.f_type == 0x9fa0);
}

TEST(meta, ustat) {
  struct stat Info;
  struct { int32_t FreeBlocks; unsigned long FreeInodes; char Name[6], Pack[6]; } Result;
  OK(fstat(Dir, &Info));
  long Status = syscall(SYS_ustat, (unsigned) Info.st_dev, &Result);
  if (Status == -1 && errno == ENOSYS) SKIP("ustat not provided by this kernel");
  CHECK(Status == 0);
  FAILS(syscall(SYS_ustat, makedev(4095, 1048575), &Result), EINVAL);
  FAILS(syscall(SYS_ustat, (unsigned) Info.st_dev, (void *) 1), EFAULT);
}

// ---------------------------------------------------------------------------
// Allocation and transfer

TEST(alloc, truncate) {
  int FD;
  char Buffer[8];
  struct stat Info;
  OK(FD = Create("file", "abcdef"));
  OK(ftruncate(FD, 3));
  OK(fstat(FD, &Info));
  CHECK(Info.st_size == 3);
  OK(ftruncate(FD, 6));
  CHECK(pread(FD, Buffer, 6, 0) == 6 && !memcmp(Buffer, "abc\0\0\0", 6));
  OK(truncate("file", 1));
  CHECK(Contents("file", "a"));
  FAILS(ftruncate(FD, -1), EINVAL);
  FAILS(truncate("missing", 0), ENOENT);
  FAILS(truncate(".", 0), EISDIR);
  int ReadOnly;
  OK(ReadOnly = openat(Dir, "file", O_RDONLY));
  FAILS(ftruncate(ReadOnly, 0), EINVAL);
  FAILS(ftruncate(Dir, 0), EINVAL);
  // O_TRUNC empties an existing file.
  OK(openat(Dir, "file", O_WRONLY | O_TRUNC));
  OK(fstat(FD, &Info));
  CHECK(Info.st_size == 0);
}

TEST(alloc, sparse_truncate_blocks) {
  int FD;
  struct stat Info;
  OK(FD = Create("file", NULL));
  OK(ftruncate(FD, 1 << 20));
  OK(fstat(FD, &Info));
  CHECK(Info.st_size == 1 << 20 && Info.st_blocks == 0);
  off_t Hole = lseek(FD, 0, SEEK_HOLE);
  CHECK(Hole == 0);
  FAILS(lseek(FD, 0, SEEK_DATA), ENXIO);
  FAILS(lseek(FD, 1 << 20, SEEK_HOLE), ENXIO);
}

TEST(alloc, fallocate_modes) {
  int FD;
  struct stat Info;
  OK(FD = Create("file", NULL));
  if (fallocate(FD, 0, 0, 8192) < 0 && errno == EOPNOTSUPP) SKIP("fallocate unsupported by the scratch filesystem");
  OK(fstat(FD, &Info));
  CHECK(Info.st_size == 8192 && Info.st_blocks >= 16);
  OK(fallocate(FD, FALLOC_FL_KEEP_SIZE, 8192, 8192));
  OK(fstat(FD, &Info));
  CHECK(Info.st_size == 8192 && Info.st_blocks >= 32);
  FAILS(fallocate(FD, FALLOC_FL_PUNCH_HOLE, 0, 4096), EOPNOTSUPP);
  FAILS(fallocate(FD, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_ZERO_RANGE, 0, 4096), EOPNOTSUPP);
  FAILS(fallocate(FD, 0x1000, 0, 4096), EOPNOTSUPP);
  FAILS(fallocate(FD, 0, 0, 0), EINVAL);
  FAILS(fallocate(FD, 0, -1, 4096), EINVAL);
  int ReadOnly;
  OK(ReadOnly = openat(Dir, "file", O_RDONLY));
  FAILS(fallocate(ReadOnly, 0, 0, 4096), EBADF);
  int Pipe[2];
  OK(pipe(Pipe));
  FAILS(fallocate(Pipe[1], 0, 0, 4096), ESPIPE);
  // The write-access check precedes the file type check.
  FAILS(fallocate(Dir, 0, 0, 4096), EBADF);
}

TEST(alloc, punch_hole) {
  int FD;
  char Buffer[12288];
  memset(Buffer, 'p', sizeof(Buffer));
  OK(FD = Create("file", NULL));
  CHECK(write(FD, Buffer, sizeof(Buffer)) == sizeof(Buffer));
  OK(fsync(FD));
  if (fallocate(FD, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 4096, 4096) < 0 && errno == EOPNOTSUPP) {
    SKIP("hole punching unsupported by the scratch filesystem");
  }
  struct stat Info;
  OK(fstat(FD, &Info));
  CHECK(Info.st_size == sizeof(Buffer) && Info.st_blocks <= 16 + 8);
  CHECK(pread(FD, Buffer, sizeof(Buffer), 0) == sizeof(Buffer));
  CHECK(Buffer[4095] == 'p' && AllZero(Buffer + 4096, 4096) && Buffer[8192] == 'p');
  CHECK(lseek(FD, 0, SEEK_HOLE) == 4096);
  CHECK(lseek(FD, 4096, SEEK_DATA) == 8192);
  // A partial-block punch zeroes bytes without changing the size.
  OK(fallocate(FD, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 10, 5));
  CHECK(pread(FD, Buffer, 16, 0) == 16 && Buffer[9] == 'p' && AllZero(Buffer + 10, 5) && Buffer[15] == 'p');
}

TEST(alloc, zero_range) {
  int FD;
  char Buffer[64];
  memset(Buffer, 'z', sizeof(Buffer));
  OK(FD = Create("file", NULL));
  CHECK(write(FD, Buffer, sizeof(Buffer)) == sizeof(Buffer));
  if (fallocate(FD, FALLOC_FL_ZERO_RANGE, 8, 8) < 0 && errno == EOPNOTSUPP) SKIP("zero range unsupported");
  CHECK(pread(FD, Buffer, sizeof(Buffer), 0) == sizeof(Buffer) && Buffer[7] == 'z' && AllZero(Buffer + 8, 8) &&
        Buffer[16] == 'z');
  OK(fallocate(FD, FALLOC_FL_ZERO_RANGE, 60, 100));
  struct stat Info;
  OK(fstat(FD, &Info));
  CHECK(Info.st_size == 160);
  OK(fallocate(FD, FALLOC_FL_ZERO_RANGE | FALLOC_FL_KEEP_SIZE, 150, 100));
  OK(fstat(FD, &Info));
  CHECK(Info.st_size == 160);
}

TEST(alloc, collapse_insert_range) {
  int FD;
  char Block[4096], Buffer[4096];
  OK(FD = Create("file", NULL));
  for (char Fill = 'a'; Fill <= 'd'; ++Fill) {
    memset(Block, Fill, sizeof(Block));
    CHECK(write(FD, Block, sizeof(Block)) == sizeof(Block));
  }
  if (fallocate(FD, FALLOC_FL_COLLAPSE_RANGE, 4096, 4096) < 0 && errno == EOPNOTSUPP) {
    SKIP("collapse range unsupported by the scratch filesystem");
  }
  struct stat Info;
  OK(fstat(FD, &Info));
  CHECK(Info.st_size == 3 * 4096);
  CHECK(pread(FD, Buffer, 1, 4096) == 1 && Buffer[0] == 'c');
  OK(fallocate(FD, FALLOC_FL_INSERT_RANGE, 0, 4096));
  OK(fstat(FD, &Info));
  CHECK(Info.st_size == 4 * 4096);
  CHECK(pread(FD, Buffer, 4096, 0) == 4096 && AllZero(Buffer, 4096));
  CHECK(pread(FD, Buffer, 1, 4096) == 1 && Buffer[0] == 'a');
  FAILS(fallocate(FD, FALLOC_FL_COLLAPSE_RANGE, 1, 4096), EINVAL);
  FAILS(fallocate(FD, FALLOC_FL_INSERT_RANGE, 4 * 4096, 4096), EINVAL);
  FAILS(fallocate(FD, FALLOC_FL_COLLAPSE_RANGE, 0, 4 * 4096), EINVAL);
}

TEST(alloc, seek_data_hole) {
  int FD;
  OK(FD = Create("file", NULL));
  CHECK(pwrite(FD, "d", 1, 1 << 20) == 1);
  OK(fsync(FD));
  // Filesystems without hole tracking may report the whole file as data.
  const off_t Data = lseek(FD, 0, SEEK_DATA);
  CHECK(Data >= 0 && Data <= 1 << 20);
  CHECK(lseek(FD, 0, SEEK_HOLE) == 0 || lseek(FD, 0, SEEK_HOLE) >= 0);
  CHECK(lseek(FD, (1 << 20), SEEK_HOLE) == (1 << 20) + 1);
  FAILS(lseek(FD, (1 << 20) + 1, SEEK_DATA), ENXIO);
  FAILS(lseek(FD, -1, SEEK_DATA), ENXIO);
}

TEST(alloc, memfd_basics) {
  int FD;
  OK(FD = memfd_create("suite", MFD_CLOEXEC | MFD_ALLOW_SEALING));
  CHECK(fcntl(FD, F_GETFD) == FD_CLOEXEC);
  CHECK(write(FD, "memory", 6) == 6);
  char Buffer[8] = {0};
  CHECK(pread(FD, Buffer, 6, 0) == 6 && !memcmp(Buffer, "memory", 6));
  struct stat Info;
  OK(fstat(FD, &Info));
  CHECK(Info.st_size == 6 && S_ISREG(Info.st_mode));
  FAILS(memfd_create("bad", 0x100), EINVAL);
  char Long[300];
  memset(Long, 'm', sizeof(Long) - 1);
  Long[sizeof(Long) - 1] = 0;
  FAILS(memfd_create(Long, 0), EINVAL);
  int Plain;
  OK(Plain = memfd_create("plain", 0));
  FAILS(fcntl(Plain, F_ADD_SEALS, F_SEAL_GROW), EPERM);
  CHECK(fcntl(Plain, F_GET_SEALS) == F_SEAL_SEAL);
}

#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x8U
#define MFD_EXEC 0x10U
#endif
#ifndef F_SEAL_EXEC
#define F_SEAL_EXEC 0x20
#endif

TEST(alloc, memfd_noexec_seal) {
  const int FD = memfd_create("noexec", MFD_NOEXEC_SEAL);
  if (FD < 0 && errno == EINVAL) SKIP("MFD_NOEXEC_SEAL needs Linux 6.3");
  CHECK(FD >= 0);
  struct stat Info;
  OK(fstat(FD, &Info));
  CHECK((Info.st_mode & 0777) == 0666);
  CHECK(fcntl(FD, F_GET_SEALS) == F_SEAL_EXEC);
  FAILS(fchmod(FD, 0777), EPERM);
  OK(fchmod(FD, 0644));
  // Sealing remains possible, so MFD_ALLOW_SEALING is implied.
  OK(fcntl(FD, F_ADD_SEALS, F_SEAL_GROW));
  FAILS(memfd_create("both", MFD_NOEXEC_SEAL | MFD_EXEC), EINVAL);
  int Sealable;
  OK(Sealable = memfd_create("exec-seal", MFD_ALLOW_SEALING));
  OK(fcntl(Sealable, F_ADD_SEALS, F_SEAL_EXEC));
  FAILS(fchmod(Sealable, 0666), EPERM);
}

TEST(alloc, memfd_seals) {
  int FD;
  OK(FD = memfd_create("seals", MFD_ALLOW_SEALING));
  CHECK(write(FD, "abcd", 4) == 4);
  OK(fcntl(FD, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW));
  // The grow seal blocks allocation past EOF even when the size is kept.
  OK(fallocate(FD, FALLOC_FL_KEEP_SIZE, 0, 4));
  FAILS(fallocate(FD, FALLOC_FL_KEEP_SIZE, 4, 1), EPERM);
  FAILS(ftruncate(FD, 2), EPERM);
  FAILS(ftruncate(FD, 8), EPERM);
  FAILS(pwrite(FD, "x", 1, 4), EPERM);
  CHECK(pwrite(FD, "x", 1, 0) == 1);
  OK(ftruncate(FD, 4));
  void *Map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, FD, 0);
  CHECK(Map != MAP_FAILED);
  FAILS(fcntl(FD, F_ADD_SEALS, F_SEAL_WRITE), EBUSY);
  if (Map != MAP_FAILED) munmap(Map, 4096);
  OK(fcntl(FD, F_ADD_SEALS, F_SEAL_WRITE));
  FAILS(write(FD, "y", 1), EPERM);
  FAILS(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, FD, 0) == MAP_FAILED ? -1 : 0, EPERM);
  OK(fcntl(FD, F_ADD_SEALS, F_SEAL_SEAL));
  FAILS(fcntl(FD, F_ADD_SEALS, F_SEAL_GROW), EPERM);
  CHECK(fcntl(FD, F_GET_SEALS) == (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE));
}

TEST(alloc, memfd_sparse_and_punch) {
  int FD;
  struct stat Info;
  char Page[4096];
  OK(FD = memfd_create("sparse", 0));
  OK(ftruncate(FD, 4 * 4096));
  OK(fstat(FD, &Info));
  CHECK(Info.st_blocks == 0);
  FAILS(lseek(FD, 0, SEEK_DATA), ENXIO);
  memset(Page, 'm', sizeof(Page));
  for (int Index = 0; Index < 4; ++Index) CHECK(pwrite(FD, Page, sizeof(Page), Index * 4096) == 4096);
  OK(fstat(FD, &Info));
  CHECK(Info.st_blocks == 32);
  OK(fallocate(FD, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 4096, 2 * 4096));
  OK(fstat(FD, &Info));
  CHECK(Info.st_size == 4 * 4096 && Info.st_blocks == 16);
  CHECK(pread(FD, Page, 4096, 4096) == 4096 && AllZero(Page, 4096));
  CHECK(lseek(FD, 0, SEEK_HOLE) == 4096 && lseek(FD, 4096, SEEK_DATA) == 3 * 4096);
  // A sub-page punch zeroes bytes but keeps the page.
  OK(fallocate(FD, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 1, 2));
  CHECK(pread(FD, Page, 4, 0) == 4 && Page[0] == 'm' && Page[1] == 0 && Page[2] == 0 && Page[3] == 'm');
  OK(fstat(FD, &Info));
  CHECK(Info.st_blocks == 16);
  FAILS(fallocate(FD, FALLOC_FL_ZERO_RANGE, 0, 4096), EOPNOTSUPP);
  FAILS(fallocate(FD, FALLOC_FL_COLLAPSE_RANGE, 0, 4096), EOPNOTSUPP);
  // KEEP_SIZE preallocation beyond EOF counts blocks; truncation frees it.
  OK(fallocate(FD, FALLOC_FL_KEEP_SIZE, 4 * 4096, 4096));
  OK(fstat(FD, &Info));
  CHECK(Info.st_size == 4 * 4096 && Info.st_blocks == 24);
  OK(ftruncate(FD, 4 * 4096));
  OK(fstat(FD, &Info));
  CHECK(Info.st_blocks == 16);
}

TEST(alloc, memfd_shared_mapping) {
  int FD;
  OK(FD = memfd_create("map", 0));
  OK(ftruncate(FD, 4096));
  char *Map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, FD, 0);
  CHECK(Map != MAP_FAILED);
  if (Map == MAP_FAILED) return;
  Map[10] = 'w';
  char Byte = 0;
  CHECK(pread(FD, &Byte, 1, 10) == 1 && Byte == 'w');
  CHECK(pwrite(FD, "r", 1, 11) == 1 && Map[11] == 'r');
  OK(fallocate(FD, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 4096));
  CHECK(Map[10] == 0 && Map[11] == 0);
  munmap(Map, 4096);
}

TEST(alloc, copy_file_range) {
  int In, Out;
  OK(In = Create("in", "0123456789"));
  OK(Out = Create("out", NULL));
  off_t InOffset = 2, OutOffset = 1;
  CHECK(copy_file_range(In, &InOffset, Out, &OutOffset, 4, 0) == 4);
  CHECK(InOffset == 6 && OutOffset == 5);
  CHECK(lseek(In, 0, SEEK_CUR) == 10 && lseek(Out, 0, SEEK_CUR) == 0);
  char Buffer[8] = {0};
  CHECK(pread(Out, Buffer, 5, 0) == 5 && !memcmp(Buffer, "\0" "2345", 5));
  CHECK(copy_file_range(In, &InOffset, Out, &OutOffset, 0, 0) == 0);
  InOffset = 100;
  CHECK(copy_file_range(In, &InOffset, Out, &OutOffset, 4, 0) == 0);
  FAILS(copy_file_range(In, NULL, Out, NULL, 4, 1), EINVAL);
  off_t Same = 0, Overlap = 2;
  FAILS(copy_file_range(In, &Same, In, &Overlap, 4, 0), EINVAL);
  int ReadOnly;
  OK(ReadOnly = openat(Dir, "out", O_RDONLY));
  FAILS(copy_file_range(In, NULL, ReadOnly, NULL, 4, 0), EBADF);
  FAILS(copy_file_range(Dir, NULL, Out, NULL, 4, 0), EISDIR);
}

TEST(alloc, sendfile) {
  int In, Out;
  OK(In = Create("in", "sendfile data"));
  OK(Out = Create("out", NULL));
  off_t Offset = 4;
  CHECK(sendfile(Out, In, &Offset, 5) == 5 && Offset == 9);
  CHECK(lseek(In, 0, SEEK_CUR) == 13);
  CHECK(lseek(In, 0, SEEK_SET) == 0);
  CHECK(sendfile(Out, In, NULL, 4) == 4 && lseek(In, 0, SEEK_CUR) == 4);
  CHECK(Contents("out", "file send"));
  int Pipe[2];
  OK(pipe(Pipe));
  CHECK(sendfile(Pipe[1], In, NULL, 100) == 9);
  char Buffer[16] = {0};
  CHECK(read(Pipe[0], Buffer, sizeof(Buffer)) == 9 && !memcmp(Buffer, "file data", 9));
  int WriteOnly;
  OK(WriteOnly = openat(Dir, "in", O_WRONLY));
  FAILS(sendfile(Out, WriteOnly, NULL, 1), EBADF);
}

TEST(alloc, splice_tee_vmsplice) {
  int File, First[2], Second[2];
  OK(File = Create("file", "splice-me"));
  OK(pipe(First));
  OK(pipe(Second));
  loff_t Offset = 0;
  CHECK(splice(File, &Offset, First[1], NULL, 6, 0) == 6 && Offset == 6);
  CHECK(lseek(File, 0, SEEK_CUR) == 9);
  CHECK(tee(First[0], Second[1], 6, 0) == 6);
  char A[8] = {0}, B[8] = {0};
  CHECK(read(Second[0], B, sizeof(B)) == 6 && !memcmp(B, "splice", 6));
  int Out;
  OK(Out = Create("out", NULL));
  CHECK(splice(First[0], NULL, Out, NULL, 6, 0) == 6);
  CHECK(Contents("out", "splice"));
  struct iovec Vector = {"vm", 2};
  CHECK(vmsplice(First[1], &Vector, 1, 0) == 2);
  CHECK(read(First[0], A, sizeof(A)) == 2 && !memcmp(A, "vm", 2));
  FAILS(splice(File, NULL, Out, NULL, 4, 0), EINVAL);
  FAILS(splice(First[0], &Offset, Out, NULL, 4, 0), ESPIPE);
  FAILS(tee(First[0], First[1], 1, 0), EINVAL);
  FAILS(splice(File, NULL, First[1], NULL, 4, 0x100), EINVAL);
}

TEST(alloc, advice) {
  int FD;
  OK(FD = Create("file", "advice"));
  CHECK(posix_fadvise(FD, 0, 0, POSIX_FADV_SEQUENTIAL) == 0);
  CHECK(posix_fadvise(FD, 0, 0, POSIX_FADV_DONTNEED) == 0);
  CHECK(posix_fadvise(FD, 0, 0, 42) == EINVAL);
  CHECK(posix_fadvise(FD, 0, -1, POSIX_FADV_NORMAL) == EINVAL);
  int Pipe[2];
  OK(pipe(Pipe));
  CHECK(posix_fadvise(Pipe[0], 0, 0, POSIX_FADV_NORMAL) == ESPIPE);
  OK(readahead(FD, 0, 4096));
  FAILS(readahead(Pipe[0], 0, 4096), EINVAL);
}

// ---------------------------------------------------------------------------
// Extended attributes

static int XattrsSupported(void) {
  OK(MakeFile("probe", NULL));
  const int Result = setxattr("probe", "user.probe", "1", 1, 0);
  unlink("probe");
  return !(Result < 0 && (errno == EOPNOTSUPP || errno == ENOTSUP));
}

TEST(xattr, roundtrip) {
  if (!XattrsSupported()) SKIP("user xattrs unsupported by the scratch filesystem");
  char Value[64] = {0};
  OK(MakeFile("file", NULL));
  OK(setxattr("file", "user.color", "blue", 4, 0));
  CHECK(getxattr("file", "user.color", Value, sizeof(Value)) == 4 && !memcmp(Value, "blue", 4));
  CHECK(getxattr("file", "user.color", NULL, 0) == 4);
  FAILS(getxattr("file", "user.color", Value, 2), ERANGE);
  OK(setxattr("file", "user.empty", "", 0, 0));
  CHECK(getxattr("file", "user.empty", Value, sizeof(Value)) == 0);
  const char Binary[4] = {0, 1, 0, (char) 0xff};
  OK(setxattr("file", "user.binary", Binary, 4, 0));
  CHECK(getxattr("file", "user.binary", Value, sizeof(Value)) == 4 && !memcmp(Value, Binary, 4));
  int FD;
  OK(FD = openat(Dir, "file", O_RDONLY));
  CHECK(fgetxattr(FD, "user.color", Value, sizeof(Value)) == 4);
  OK(fsetxattr(FD, "user.color", "green", 5, XATTR_REPLACE));
  CHECK(getxattr("file", "user.color", Value, sizeof(Value)) == 5 && !memcmp(Value, "green", 5));
}

TEST(xattr, create_replace_remove) {
  if (!XattrsSupported()) SKIP("user xattrs unsupported by the scratch filesystem");
  char Value[8];
  OK(MakeFile("file", NULL));
  FAILS(setxattr("file", "user.a", "1", 1, XATTR_REPLACE), ENODATA);
  OK(setxattr("file", "user.a", "1", 1, XATTR_CREATE));
  FAILS(setxattr("file", "user.a", "2", 1, XATTR_CREATE), EEXIST);
  FAILS(setxattr("file", "user.a", "2", 1, 0x4), EINVAL);
  OK(removexattr("file", "user.a"));
  FAILS(getxattr("file", "user.a", Value, sizeof(Value)), ENODATA);
  FAILS(removexattr("file", "user.a"), ENODATA);
}

TEST(xattr, listing) {
  if (!XattrsSupported()) SKIP("user xattrs unsupported by the scratch filesystem");
  char List[256];
  OK(MakeFile("file", NULL));
  CHECK(listxattr("file", List, sizeof(List)) >= 0);
  const ssize_t Empty = listxattr("file", NULL, 0);
  OK(setxattr("file", "user.one", "1", 1, 0));
  OK(setxattr("file", "user.two", "2", 1, 0));
  const ssize_t Size = listxattr("file", NULL, 0);
  CHECK(Size == Empty + (ssize_t) sizeof("user.one") + (ssize_t) sizeof("user.two"));
  CHECK(listxattr("file", List, sizeof(List)) == Size);
  int Found = 0;
  for (char *Name = List; Name < List + Size; Name += strlen(Name) + 1) {
    Found |= !strcmp(Name, "user.one") ? 1 : !strcmp(Name, "user.two") ? 2 : 0;
  }
  CHECK(Found == 3);
  FAILS(listxattr("file", List, 3), ERANGE);
}

TEST(xattr, name_and_value_limits) {
  if (!XattrsSupported()) SKIP("user xattrs unsupported by the scratch filesystem");
  OK(MakeFile("file", NULL));
  char Name[300] = "user.";
  memset(Name + 5, 'n', 260);
  FAILS(setxattr("file", Name, "1", 1, 0), ERANGE);
  FAILS(setxattr("file", "", "1", 1, 0), ERANGE);
  FAILS(setxattr("file", "user.", "1", 1, 0), EINVAL);
  FAILS(setxattr("file", "bogus.name", "1", 1, 0), EOPNOTSUPP);
  FAILS(setxattr("file", "noprefix", "1", 1, 0), EOPNOTSUPP);
  static char Large[65537];
  FAILS(setxattr("file", "user.big", Large, sizeof(Large), 0), E2BIG);
  FAILS(getxattr("missing", "user.x", NULL, 0), ENOENT);
}

TEST(xattr, follows_inode) {
  if (!XattrsSupported()) SKIP("user xattrs unsupported by the scratch filesystem");
  char Value[8];
  OK(MakeFile("file", NULL));
  OK(setxattr("file", "user.keep", "k", 1, 0));
  OK(linkat(Dir, "file", Dir, "hard", 0));
  OK(renameat(Dir, "file", Dir, "renamed"));
  CHECK(getxattr("hard", "user.keep", Value, sizeof(Value)) == 1);
  CHECK(getxattr("renamed", "user.keep", Value, sizeof(Value)) == 1);
  OK(symlinkat("renamed", Dir, "link"));
  CHECK(getxattr("link", "user.keep", Value, sizeof(Value)) == 1);
  FAILS(lgetxattr("link", "user.keep", Value, sizeof(Value)), ENODATA);
  // user.* attributes are not permitted on symlinks.
  FAILS(lsetxattr("link", "user.keep", "k", 1, 0), EPERM);
}

TEST(xattr, descriptor_rules) {
  if (!XattrsSupported()) SKIP("user xattrs unsupported by the scratch filesystem");
  OK(MakeFile("file", NULL));
  int Path, ReadOnly;
  OK(Path = openat(Dir, "file", O_PATH));
  OK(ReadOnly = openat(Dir, "file", O_RDONLY));
  char Value[8];
  FAILS(fgetxattr(Path, "user.x", Value, sizeof(Value)), EBADF);
  FAILS(fsetxattr(Path, "user.x", "1", 1, 0), EBADF);
  // A read-only descriptor still permits setting attributes as the owner.
  OK(fsetxattr(ReadOnly, "user.x", "1", 1, 0));
  int Pipe[2];
  OK(pipe(Pipe));
  FAILS(fsetxattr(Pipe[0], "user.x", "1", 1, 0), EPERM);
}

static void TrustedHiddenChecks(void) {
  char Names[256];
  const ssize_t Size = listxattr("file", Names, sizeof(Names));
  CHECK(Size >= 0);
  for (char *Name = Names; Size > 0 && Name < Names + Size; Name += strlen(Name) + 1) {
    CHECK(strncmp(Name, "trusted.", 8) != 0);
  }
  char Byte;
  FAILS(getxattr("file", "trusted.secret", &Byte, 1), ENODATA);
  FAILS(removexattr("file", "trusted.secret"), EPERM);
}

TEST(xattr, trusted_namespace) {
  if (!XattrsSupported()) SKIP("user xattrs unsupported by the scratch filesystem");
  OK(MakeFile("file", NULL));
  if (geteuid() != 0) {
    FAILS(setxattr("file", "trusted.secret", "1", 1, 0), EPERM);
    char List[256];
    const ssize_t Size = listxattr("file", List, sizeof(List));
    CHECK(Size >= 0);
    return;
  }
  const int Result = setxattr("file", "trusted.secret", "1", 1, 0);
  if (Result < 0 && errno == EPERM) SKIP("CAP_SYS_ADMIN unavailable in this namespace");
  CHECK(Result == 0);
  char Value[8];
  CHECK(getxattr("file", "trusted.secret", Value, sizeof(Value)) == 1);
  if (!CanSwitchIDs) return;
  OK(fchmodat(Dir, ".", 0777, 0));
  OK(fchmodat(Dir, "file", 0666, 0));
  AsUser(4242, 4242, TrustedHiddenChecks);
}

TEST(xattr, at_variants) {
  if (!XattrsSupported()) SKIP("user xattrs unsupported by the scratch filesystem");
  OK(MakeFile("file", NULL));
  struct { uint64_t Value; uint32_t Size, Flags; } Args = {(uintptr_t) "at", 2, 0};
  long Result = syscall(SYS_setxattrat, Dir, "file", 0, "user.at", &Args, sizeof(Args));
  if (Result == -1 && errno == ENOSYS) SKIP("setxattrat not provided by this kernel");
  CHECK(Result == 0);
  char Value[8] = {0};
  struct { uint64_t Value; uint32_t Size, Flags; } Get = {(uintptr_t) Value, sizeof(Value), 0};
  CHECK(syscall(SYS_getxattrat, Dir, "file", 0, "user.at", &Get, sizeof(Get)) == 2 && !memcmp(Value, "at", 2));
  FAILS(syscall(SYS_setxattrat, Dir, "file", 0x1, "user.at", &Args, sizeof(Args)), EINVAL);
  FAILS(syscall(SYS_setxattrat, Dir, "file", 0, "user.at", &Args, sizeof(Args) - 1), EINVAL);
}

// ---------------------------------------------------------------------------
// time: sleeping. Lock and pipe tests in the probes rely on these to order
// processes, so a missing or non-interruptible sleep hangs them.

static volatile sig_atomic_t SleepSignal;
static void CatchSleepSignal(int Number) { SleepSignal = Number; }

TEST(time, nanosleep_rules) {
  struct timespec Zero = {0, 0}, Short = {0, 1000000}, Bad = {0, 1000000000}, Negative = {-1, 0};
  OK(syscall(SYS_nanosleep, &Zero, NULL));
  OK(syscall(SYS_nanosleep, &Short, NULL));
  FAILS(syscall(SYS_nanosleep, &Bad, NULL), EINVAL);
  FAILS(syscall(SYS_nanosleep, &Negative, NULL), EINVAL);
  FAILS(syscall(SYS_nanosleep, (void*) 8, NULL), EFAULT);
  // clock_nanosleep validates the clock first and ignores unknown flag bits.
  OK(syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0x100, &Short, NULL));
  OK(syscall(SYS_clock_nanosleep, CLOCK_BOOTTIME, 0, &Short, NULL));
  FAILS(syscall(SYS_clock_nanosleep, CLOCK_THREAD_CPUTIME_ID, 0, &Short, NULL), EOPNOTSUPP);
  FAILS(syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC_RAW, 0, (void*) 8, NULL), EOPNOTSUPP);
  FAILS(syscall(SYS_clock_nanosleep, CLOCK_REALTIME_COARSE, 0, &Short, NULL), EOPNOTSUPP);
  FAILS(syscall(SYS_clock_nanosleep, 10, 0, &Short, NULL), EINVAL);
  FAILS(syscall(SYS_clock_nanosleep, 1234, 0, &Short, NULL), EINVAL);
  FAILS(syscall(SYS_clock_nanosleep, CLOCK_BOOTTIME_ALARM, 0, &Bad, NULL), EINVAL);
  FAILS(syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &Bad, NULL), EINVAL);
  // An absolute deadline in the past returns at once.
  OK(syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, TIMER_ABSTIME, &Short, NULL));
  OK(syscall(SYS_clock_nanosleep, CLOCK_REALTIME, TIMER_ABSTIME, &Short, NULL));
  struct timespec Before, After, Deadline;
  OK(clock_gettime(CLOCK_MONOTONIC, &Before));
  Deadline = Before;
  Deadline.tv_nsec += 20000000;
  if (Deadline.tv_nsec >= 1000000000) Deadline.tv_sec += 1, Deadline.tv_nsec -= 1000000000;
  OK(syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, TIMER_ABSTIME, &Deadline, NULL));
  OK(clock_gettime(CLOCK_MONOTONIC, &After));
  CHECK(After.tv_sec > Deadline.tv_sec || (After.tv_sec == Deadline.tv_sec && After.tv_nsec >= Deadline.tv_nsec));
}

TEST(time, sleep_interrupted_by_signal) {
  // A caught signal ends the sleep with EINTR even with SA_RESTART, and a
  // relative sleep reports the time left.
  struct sigaction Action = {.sa_handler = CatchSleepSignal, .sa_flags = SA_RESTART}, Previous;
  sigemptyset(&Action.sa_mask);
  OK(sigaction(SIGUSR1, &Action, &Previous));
  for (int Call = 0; Call < 2; ++Call) {
    SleepSignal = 0;
    const pid_t Parent = getpid();
    fflush(stdout);
    const pid_t Child = fork();
    if (Child == 0) {
      struct timespec Delay = {0, 50000000};
      nanosleep(&Delay, NULL);
      _exit(kill(Parent, SIGUSR1) != 0);
    }
    struct timespec Long = {30, 0}, Left = {-1, -1};
    long Result = Call ? syscall(SYS_clock_nanosleep, CLOCK_MONOTONIC, 0, &Long, &Left)
                       : syscall(SYS_nanosleep, &Long, &Left);
    int Error = errno;
    CHECK(Result == -1 && Error == EINTR);
    CHECK(SleepSignal == SIGUSR1);
    CHECK(Left.tv_sec >= 20 && Left.tv_sec < 30 && Left.tv_nsec >= 0 && Left.tv_nsec < 1000000000);
    int Status;
    CHECK(waitpid(Child, &Status, 0) == Child && WIFEXITED(Status) && WEXITSTATUS(Status) == 0);
  }
  OK(sigaction(SIGUSR1, &Previous, NULL));
}

// ---------------------------------------------------------------------------
// path: long paths. Horizon's SD card holds at most 257 bytes per path, so
// the runtime keeps deeper entries in its inode store. These cases cross
// that limit in every direction.

// Writes Count bytes of Fill followed by a NUL.
static char* Repeat(char* Out, char Fill, size_t Count) {
  memset(Out, Fill, Count);
  Out[Count] = 0;
  return Out;
}

TEST(path, long_paths) {
  char Path[4200], Name[256];
  // Five 200-byte levels put the file about 1 KiB deep.
  Path[0] = 0;
  for (int Level = 0; Level < 5; ++Level) {
    if (Level) strcat(Path, "/");
    strcat(Path, Repeat(Name, 'a' + Level, 200));
    OK(mkdirat(Dir, Path, 0750));
  }
  struct stat Info;
  OK(fstatat(Dir, Path, &Info, 0));
  CHECK(S_ISDIR(Info.st_mode) && (Info.st_mode & 07777) == 0750 && Info.st_nlink == 2);
  char File[4300];
  snprintf(File, sizeof(File), "%s/%s", Path, Repeat(Name, 'f', 255));
  int FD;
  OK(FD = openat(Dir, File, O_RDWR | O_CREAT | O_EXCL, 0640));
  CHECK(write(FD, "deep data", 9) == 9);
  CHECK(pwrite(FD, "!", 1, 4096) == 1);
  OK(fstat(FD, &Info));
  CHECK(S_ISREG(Info.st_mode) && Info.st_size == 4097 && (Info.st_mode & 07777) == 0640);
  OK(close(FD));
  FAILS(openat(Dir, File, O_RDWR | O_CREAT | O_EXCL, 0640), EEXIST);
  char Buffer[16] = {0};
  OK(FD = openat(Dir, File, O_RDONLY));
  CHECK(read(FD, Buffer, 9) == 9 && !memcmp(Buffer, "deep data", 9));
  OK(close(FD));
  // Missing entries below the limit report ENOENT, not a backend error.
  char Missing[4300];
  snprintf(Missing, sizeof(Missing), "%s/missing/child", Path);
  FAILS(openat(Dir, Missing, O_RDONLY), ENOENT);
  FAILS(mkdirat(Dir, Missing, 0755), ENOENT);
  // Listing shows the deep entries with their types.
  int Deep;
  OK(Deep = openat(Dir, Path, O_RDONLY | O_DIRECTORY));
  int Files = 0, Others = 0;
  DIR* Stream = fdopendir(dup(Deep));
  for (struct dirent* Entry; Stream && (Entry = readdir(Stream));) {
    if (Entry->d_type == DT_REG && strlen(Entry->d_name) == 255) ++Files;
    else if (strcmp(Entry->d_name, ".") && strcmp(Entry->d_name, "..")) ++Others;
  }
  if (Stream) closedir(Stream);
  CHECK(Files == 1 && Others == 0);
  // Relative operations from a deep descriptor and working directory.
  OK(mkdirat(Deep, "sub", 0755));
  OK(fstat(Deep, &Info));
  CHECK(Info.st_nlink == 3);
  OK(symlinkat("sub", Deep, "link"));
  char Target[8] = {0};
  CHECK(readlinkat(Deep, "link", Target, sizeof(Target)) == 3 && !strcmp(Target, "sub"));
  OK(linkat(Deep, Name, Deep, "hard", 0));
  OK(fstatat(Deep, "hard", &Info, 0));
  CHECK(Info.st_nlink == 2 && Info.st_size == 4097);
  OK(fchdir(Deep));
  char Cwd[4200];
  CHECK(getcwd(Cwd, sizeof(Cwd)) && strlen(Cwd) > 1000 && !strcmp(Cwd + strlen(Cwd) - 200, Repeat(Name, 'e', 200)));
  int Relative;
  OK(Relative = open("../relative", O_CREAT | O_EXCL | O_WRONLY, 0644));
  OK(close(Relative));
  OK(fchdir(Dir));
  OK(fstatat(Deep, "../relative", &Info, 0));
  OK(unlinkat(Deep, "../relative", 0));
  // Removal follows Linux rules.
  FAILS(unlinkat(Dir, Path, AT_REMOVEDIR), ENOTEMPTY);
  FAILS(unlinkat(Deep, "sub", 0), EISDIR);
  OK(unlinkat(Deep, "sub", AT_REMOVEDIR));
  OK(unlinkat(Deep, "link", 0));
  OK(unlinkat(Deep, "hard", 0));
  snprintf(File, sizeof(File), "%s/%s", Path, Repeat(Name, 'f', 255));
  OK(unlinkat(Dir, File, 0));
  OK(close(Deep));
  OK(unlinkat(Dir, Path, AT_REMOVEDIR));
  FAILS(fstatat(Dir, Path, &Info, 0), ENOENT);
  // PATH_MAX includes the terminating NUL.
  char Huge[4200];
  Repeat(Huge, 'x', 4096);
  for (int Index = 200; Index < 4096; Index += 201) Huge[Index] = '/';
  FAILS(openat(Dir, Huge, O_RDONLY), ENAMETOOLONG);
  // 4095 bytes plus the NUL fit; the missing first component is ENOENT.
  Huge[4095] = 0;
  FAILS(openat(Dir, Huge, O_RDONLY), ENOENT);
}

TEST(path, long_path_renames) {
  char Deep[1200], Name[256], Moved[1400], Shallow[64] = "shallow";
  Deep[0] = 0;
  for (int Level = 0; Level < 4; ++Level) {
    if (Level) strcat(Deep, "/");
    strcat(Deep, Repeat(Name, 'k' + Level, 150));
    OK(mkdirat(Dir, Deep, 0755));
  }
  // A small native tree that fits where it starts.
  OK(mkdirat(Dir, Shallow, 0755));
  OK(mkdirat(Dir, "shallow/inner", 0700));
  OK(MakeFile("shallow/inner/file", "payload"));
  OK(symlinkat("inner/file", Dir, "shallow/link"));
  // Moving it deep converts it; moving it back keeps the contents.
  snprintf(Moved, sizeof(Moved), "%s/tree", Deep);
  OK(renameat(Dir, Shallow, Dir, Moved));
  FAILS(fstatat(Dir, Shallow, &(struct stat) {0}, 0), ENOENT);
  char Inside[1500];
  snprintf(Inside, sizeof(Inside), "%s/inner/file", Moved);
  CHECK(Contents(Inside, "payload"));
  snprintf(Inside, sizeof(Inside), "%s/link", Moved);
  CHECK(Contents(Inside, "payload"));
  struct stat Info;
  snprintf(Inside, sizeof(Inside), "%s/inner", Moved);
  OK(fstatat(Dir, Inside, &Info, 0));
  CHECK(S_ISDIR(Info.st_mode) && (Info.st_mode & 07777) == 0700);
  OK(renameat(Dir, Moved, Dir, "back"));
  CHECK(Contents("back/inner/file", "payload") && Contents("back/link", "payload"));
  OK(MakeFile("back/inner/new", "added"));
  CHECK(Contents("back/inner/new", "added"));
  // A directory whose descendants only exceed the limit after the move.
  char Wide[256], Nested[600];
  OK(mkdirat(Dir, Repeat(Wide, 'w', 120), 0755));
  snprintf(Nested, sizeof(Nested), "%s/back", Wide);
  OK(renameat(Dir, "back", Dir, Nested));
  snprintf(Inside, sizeof(Inside), "%s/inner/file", Nested);
  CHECK(Contents(Inside, "payload"));
  OK(renameat(Dir, Nested, Dir, "back"));
  // Exchange between a deep and a shallow entry, and replacement rules.
  OK(MakeFile("plain", "shallow file"));
  snprintf(Moved, sizeof(Moved), "%s/deepfile", Deep);
  OK(MakeFile(Moved, "deep file"));
  OK(syscall(SYS_renameat2, Dir, "plain", Dir, Moved, RENAME_EXCHANGE));
  CHECK(Contents("plain", "deep file") && Contents(Moved, "shallow file"));
  FAILS(syscall(SYS_renameat2, Dir, "plain", Dir, Moved, RENAME_NOREPLACE), EEXIST);
  OK(renameat(Dir, "plain", Dir, Moved));
  CHECK(Contents(Moved, "deep file"));
  FAILS(renameat(Dir, "back", Dir, Moved), ENOTDIR);
  FAILS(renameat(Dir, Moved, Dir, "back"), EISDIR);
  FAILS(renameat(Dir, "back", Dir, Deep), ENOTEMPTY);
  // Ancestor checks come before type checks.
  FAILS(renameat(Dir, Moved, Dir, Deep), ENOTEMPTY);
  FAILS(syscall(SYS_renameat2, Dir, Moved, Dir, Deep, RENAME_EXCHANGE), EINVAL);
  snprintf(Moved, sizeof(Moved), "%s/inside", Deep);
  FAILS(renameat(Dir, Repeat(Name, 'k', 150), Dir, Moved), EINVAL);
  // An empty deep directory can be replaced by a native one.
  snprintf(Moved, sizeof(Moved), "%s/empty", Deep);
  OK(mkdirat(Dir, Moved, 0755));
  OK(mkdirat(Dir, "native", 0755));
  OK(renameat(Dir, "native", Dir, Moved));
  FAILS(fstatat(Dir, "native", &Info, 0), ENOENT);
  OK(fstatat(Dir, Moved, &Info, 0));
}
