// SPDX-License-Identifier: MIT
// final-pass-suite: runs every suite a milestone's final pass needs, one after another, and adds up the results.
//
// Usage: final-pass-suite <suite.elf>...
// Each argument names a suite program in this program's folder (sdmc:/fex/games, /mnt/sd/fex/games in the guest). The
// suite runs the way the launcher would run it: with the arguments of its .args file, as the uid and gid of its .user
// file (the default user 1000 1000 without one), in its own session with the terminal as its controlling terminal, in
// this folder, with the launcher's environment. This program therefore runs as root (its own .user file is "0 0").
// Sidecars the launcher applies to the whole runtime (.single-thread, .no-tso, .profile, .recovery-test) cannot apply
// to a child process: a suite that has one is reported as not run.
//
// Each suite prints "# <name> summary: P/F/S (passed/failed/skipped)" and writes the same counts to the file named by
// FEX_SUITE_RESULTS; it exits 42 when nothing failed. A suite is clean when it exits 42 with counts and 0 failures.
// This program prints the same summary over all suites, the names of the suites that were not clean, and exits 42 when
// every suite was clean.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { MaxSuites = 64, MaxArgs = 34, SuiteTimeoutSeconds = 45 * 60 };

static char Folder[PATH_MAX];

// Folder/Name+Suffix into Out; 0 when it does not fit.
static int SuitePath(char* Out, size_t Size, const char* Name, const char* Suffix) {
  const int Length = snprintf(Out, Size, "%s/%s%s", Folder, Name, Suffix);
  return Length > 0 && (size_t)Length < Size;
}

static int Exists(const char* Name, const char* Suffix) {
  char Path[PATH_MAX];
  return SuitePath(Path, sizeof(Path), Name, Suffix) && access(Path, F_OK) == 0;
}

// The .args file: one argument per line, as the launcher reads it. Returns the argument count (without argv[0]) or -1.
static int ReadArguments(const char* Name, char Storage[][PATH_MAX], char** Argv) {
  char Path[PATH_MAX];
  if (!SuitePath(Path, sizeof(Path), Name, ".args")) return -1;
  FILE* File = fopen(Path, "r");
  int Count = 0;
  if (File) {
    while (Count < MaxArgs - 2 && fgets(Storage[Count], PATH_MAX, File)) {
      Storage[Count][strcspn(Storage[Count], "\r\n")] = 0;
      Argv[Count + 1] = Storage[Count];
      ++Count;
    }
    fclose(File);
  } else if (errno != ENOENT) {
    return -1;
  }
  Argv[Count + 1] = NULL;
  return Count;
}

// Reads Count unsigned numbers separated by blanks after the first Skip words of Line. Parsed by hand: the host
// compiler's glibc headers redirect scanf and strtoul to C23 symbols the Debian libc does not have. Returns 1 when all were read.
static int ParseNumbers(const char* Line, int Skip, unsigned* Out, int Count) {
  const char* Cursor = Line;
  for (int Word = 0; Word < Skip; ++Word) {
    Cursor += strspn(Cursor, " \t");
    Cursor += strcspn(Cursor, " \t\n");
  }
  for (int Index = 0; Index < Count; ++Index) {
    Cursor += strspn(Cursor, " \t");
    unsigned long long Value = 0;
    const char* Digits = Cursor;
    while (*Cursor >= '0' && *Cursor <= '9' && Value <= 0xFFFFFFFFull) Value = Value * 10 + (unsigned)(*Cursor++ - '0');
    if (Cursor == Digits || Value > 0xFFFFFFFFull) return 0;
    Out[Index] = (unsigned)Value;
  }
  return 1;
}

static int ReadUser(const char* Name, unsigned* Uid, unsigned* Gid) {
  char Path[PATH_MAX];
  *Uid = *Gid = 1000;
  if (!SuitePath(Path, sizeof(Path), Name, ".user")) return -1;
  FILE* File = fopen(Path, "r");
  if (!File) return errno == ENOENT ? 0 : -1;
  char Line[128];
  unsigned Ids[2];
  const int Parsed = fgets(Line, sizeof(Line), File) && ParseNumbers(Line, 0, Ids, 2);
  fclose(File);
  if (!Parsed) return -1;
  *Uid = Ids[0];
  *Gid = Ids[1];
  return 0;
}

static double Seconds(void) {
  struct timespec Now;
  clock_gettime(CLOCK_MONOTONIC, &Now);
  return (double)Now.tv_sec + (double)Now.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: final-pass-suite <suite.elf>...\n");
    return 2;
  }
  if (argc - 1 > MaxSuites) {
    printf("# at most %d suites\n", MaxSuites);
    return 2;
  }
  ssize_t Length = readlink("/proc/self/exe", Folder, sizeof(Folder) - 1);
  if (Length <= 0) {
    printf("# cannot read /proc/self/exe: %s\n", strerror(errno));
    return 1;
  }
  Folder[Length] = 0;
  *strrchr(Folder, '/') = 0;
  if (geteuid() != 0) printf("# not running as root: suites with another user cannot run (add final-pass-suite.elf.user \"0 0\")\n");

  // The suites append their counts to this file. It sits in a folder of its own (a file of another user in the sticky
  // /tmp cannot be opened for writing under protected_regular) and is writable by the suites' users.
  char ResultsFolder[] = "/tmp/final-pass-XXXXXX";
  char ResultsPath[sizeof(ResultsFolder) + 16];
  int ResultsFD = -1;
  if (mkdtemp(ResultsFolder) && chmod(ResultsFolder, 0755) == 0) {
    snprintf(ResultsPath, sizeof(ResultsPath), "%s/results", ResultsFolder);
    ResultsFD = open(ResultsPath, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
  }
  if (ResultsFD < 0 || fchmod(ResultsFD, 0666) < 0) {
    printf("# cannot create the results file: %s\n", strerror(errno));
    return 1;
  }
  close(ResultsFD);

  sigset_t ChildSet, OldMask;
  sigemptyset(&ChildSet);
  sigaddset(&ChildSet, SIGCHLD);
  sigprocmask(SIG_BLOCK, &ChildSet, &OldMask);

  unsigned TotalPassed = 0, TotalFailed = 0, TotalSkipped = 0, Clean = 0;
  const char* NotClean[MaxSuites];
  char NotCleanWhy[MaxSuites][160];
  unsigned NotCleanCount = 0;
  const double Start = Seconds();

  for (int Index = 1; Index < argc; ++Index) {
    const char* Name = argv[Index];
    char Why[160] = "";
    unsigned Passed = 0, Failed = 0, Skipped = 0;
    int HaveCounts = 0;
    const double SuiteStart = Seconds();

    static char ArgStorage[MaxArgs][PATH_MAX];
    char* SuiteArgv[MaxArgs];
    char Program[PATH_MAX];
    if (!SuitePath(Program, sizeof(Program), Name, "")) Program[0] = 0;
    SuiteArgv[0] = Program;
    unsigned Uid, Gid;
    printf("\n### final-pass: %s\n", Name);
    fflush(stdout);

    if (!Program[0] || !Exists(Name, "")) {
      snprintf(Why, sizeof(Why), "missing");
    } else if (Exists(Name, ".single-thread") || Exists(Name, ".no-tso") || Exists(Name, ".profile") ||
               Exists(Name, ".recovery-test")) {
      snprintf(Why, sizeof(Why), "not run: has a sidecar that only applies to a whole launch");
    } else if (ReadArguments(Name, ArgStorage, SuiteArgv) < 0 || ReadUser(Name, &Uid, &Gid) < 0) {
      snprintf(Why, sizeof(Why), "unreadable .args or .user file");
    } else if (truncate(ResultsPath, 0) < 0) {
      snprintf(Why, sizeof(Why), "cannot reset the results file: %s", strerror(errno));
    } else {
      const pid_t Child = fork();
      if (Child == 0) {
        sigprocmask(SIG_SETMASK, &OldMask, NULL);
        // A session of its own with the terminal as its controlling terminal, as a launch has.
        setsid();
        if (isatty(STDIN_FILENO) && ioctl(STDIN_FILENO, TIOCSCTTY, 1) < 0)
          dprintf(STDOUT_FILENO, "# final-pass: TIOCSCTTY: %s (the suite runs without a controlling terminal)\n", strerror(errno));
        if (setgroups(0, NULL) < 0 || setresgid(Gid, Gid, Gid) < 0 || setresuid(Uid, Uid, Uid) < 0) {
          dprintf(STDOUT_FILENO, "# final-pass: cannot become uid %u gid %u: %s\n", Uid, Gid, strerror(errno));
          _exit(127);
        }
        if (chdir(Folder) < 0) {
          dprintf(STDOUT_FILENO, "# final-pass: chdir %s: %s\n", Folder, strerror(errno));
          _exit(127);
        }
        const int Root = Uid == 0;
        char Home[PATH_MAX], Results[PATH_MAX + 32];
        struct stat Info;
        snprintf(Home, sizeof(Home), "HOME=%s",
                 Root ? "/root" : stat("/home/fex", &Info) == 0 && S_ISDIR(Info.st_mode) ? "/home/fex" : "/");
        snprintf(Results, sizeof(Results), "FEX_SUITE_RESULTS=%s", ResultsPath);
        char* Environment[] = {
          Root ? "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" : "PATH=/usr/local/bin:/usr/bin:/bin",
          Home, "LANG=C", "TERM=dumb", Results, NULL};
        fflush(stdout);
        execve(Program, SuiteArgv, Environment);
        dprintf(STDOUT_FILENO, "# final-pass: exec %s: %s\n", Program, strerror(errno));
        _exit(127);
      }
      if (Child < 0) {
        snprintf(Why, sizeof(Why), "fork: %s", strerror(errno));
      } else {
        int Status = 0, Done = 0;
        while (!Done) {
          const pid_t Got = waitpid(Child, &Status, WNOHANG);
          if (Got == Child) {
            Done = 1;
            break;
          }
          if (Seconds() - SuiteStart > SuiteTimeoutSeconds) {
            kill(-Child, SIGKILL);
            kill(Child, SIGKILL);
            waitpid(Child, &Status, 0);
            snprintf(Why, sizeof(Why), "timed out after %d minutes", SuiteTimeoutSeconds / 60);
            Done = 1;
            break;
          }
          const struct timespec Wait = {1, 0};
          sigtimedwait(&ChildSet, NULL, &Wait);
        }
        FILE* Results = fopen(ResultsPath, "r");
        if (Results) {
          char Line[256];
          unsigned Counts[3];
          while (fgets(Line, sizeof(Line), Results))
            if (ParseNumbers(Line, 1, Counts, 3)) {
              Passed = Counts[0];
              Failed = Counts[1];
              Skipped = Counts[2];
              HaveCounts = 1;
            }
          fclose(Results);
        }
        if (!Why[0]) {
          if (WIFSIGNALED(Status)) snprintf(Why, sizeof(Why), "killed by signal %s", sigabbrev_np(WTERMSIG(Status)));
          else if (!HaveCounts) snprintf(Why, sizeof(Why), "no results (exit %d)", WEXITSTATUS(Status));
          else if (Failed) snprintf(Why, sizeof(Why), "%u failed", Failed);
          else if (WEXITSTATUS(Status) != 42) snprintf(Why, sizeof(Why), "exit %d", WEXITSTATUS(Status));
        }
      }
    }

    TotalPassed += Passed;
    TotalFailed += Failed;
    TotalSkipped += Skipped;
    if (Why[0]) {
      NotClean[NotCleanCount] = Name;
      snprintf(NotCleanWhy[NotCleanCount], sizeof(NotCleanWhy[0]), "%s", Why);
      ++NotCleanCount;
    } else {
      ++Clean;
    }
    if (HaveCounts)
      printf("### final-pass: %s: %u/%u/%u (passed/failed/skipped), %.0f s%s%s\n", Name, Passed, Failed, Skipped,
             Seconds() - SuiteStart, Why[0] ? ", " : "", Why);
    else
      printf("### final-pass: %s: %s\n", Name, Why);
    fflush(stdout);
  }
  unlink(ResultsPath);
  rmdir(ResultsFolder);

  printf("\n### final-pass results (%.0f s)\n", Seconds() - Start);
  // The same failure list and summary format as every suite.
  for (unsigned Index = 0; Index < NotCleanCount; ++Index) printf("# failed: %s (%s)\n", NotClean[Index], NotCleanWhy[Index]);
  printf("# final-pass-suite suites: %u/%u clean\n", Clean, (unsigned)(argc - 1));
  printf("# final-pass-suite summary: %u/%u/%u (passed/failed/skipped)\n", TotalPassed, TotalFailed, TotalSkipped);
  return NotCleanCount ? 1 : 42;
}
