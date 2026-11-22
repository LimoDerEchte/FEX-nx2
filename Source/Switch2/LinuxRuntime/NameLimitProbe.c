// SPDX-License-Identifier: MIT
// Measures the longest file and directory name the backing filesystem
// accepts at several directory depths. A per-name limit keeps the longest
// name constant across depths; a whole-path limit keeps the longest name
// plus its directory path constant. Multibyte names show whether the limit
// counts UTF-8 bytes or characters. Exits 42 when the measurement completes.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum Kind { File, Directory, Wide };

// Builds a name of Count units: 'f' or 'd' bytes, or two-byte "é".
static void BuildName(char* Name, enum Kind Type, size_t Count) {
  if (Type == Wide) {
    for (size_t Index = 0; Index < Count; ++Index) memcpy(Name + Index * 2, "\xc3\xa9", 2);
    Name[Count * 2] = 0;
  } else {
    memset(Name, Type == File ? 'f' : 'd', Count);
    Name[Count] = 0;
  }
}

static int TryCreate(const char* Parent, enum Kind Type, size_t Count, int* Error) {
  char Name[2 * NAME_MAX + 2], Path[PATH_MAX];
  BuildName(Name, Type, Count);
  if (snprintf(Path, sizeof(Path), "%s/%s", Parent, Name) >= (int) sizeof(Path)) {
    *Error = ENAMETOOLONG;
    return 0;
  }
  int Created;
  if (Type == Directory) {
    Created = mkdir(Path, 0755) == 0;
    *Error = errno;
    if (Created) rmdir(Path);
  } else {
    const int FD = open(Path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    *Error = errno;
    Created = FD >= 0;
    if (Created) {
      close(FD);
      unlink(Path);
    }
  }
  return Created;
}

// Longest accepted name in units (bytes, or characters for Wide), and the
// error returned one unit beyond it.
static size_t Longest(const char* Parent, enum Kind Type, int* FirstError) {
  const size_t Maximum = Type == Wide ? NAME_MAX / 2 : NAME_MAX;
  int Error = 0;
  *FirstError = 0;
  if (TryCreate(Parent, Type, Maximum, &Error)) return Maximum;
  *FirstError = Error;
  size_t Low = 0, High = Maximum;
  while (Low + 1 < High) {
    const size_t Middle = (Low + High) / 2;
    if (TryCreate(Parent, Type, Middle, &Error)) Low = Middle;
    else High = Middle;
  }
  TryCreate(Parent, Type, High, FirstError);
  return Low;
}

int main(void) {
  // Work in the current directory, which the launcher sets to "/".
  char Base[PATH_MAX];
  if (!getcwd(Base, sizeof(Base) - 16)) return 1;
  strcat(Base, strcmp(Base, "/") ? "/nl.XXXXXX" : "nl.XXXXXX");
  if (!mkdtemp(Base)) {
    printf("Could not create %s: %s\n", Base, strerror(errno));
    return 1;
  }
  char Parent[PATH_MAX];
  snprintf(Parent, sizeof(Parent), "%s", Base);
  const size_t Root = strlen("/fex/rootfs"); // SD prefix of every guest path
  printf("Name limit probe. SD path = /fex/rootfs + guest path.\n");
  printf("depth | dir path bytes (SD) | file max | dir max | 2-byte chars max | file + dir path (SD)\n");
  int Status = 42;
  for (int Depth = 0; Depth <= 5; ++Depth) {
    int FileError, DirError, WideError;
    const size_t Path = strlen(Parent) + 1; // including the separating slash
    const size_t Files = Longest(Parent, File, &FileError);
    const size_t Dirs = Longest(Parent, Directory, &DirError);
    const size_t Wides = Longest(Parent, Wide, &WideError);
    printf("%5d | %19zu | %8zu | %7zu | %5zu (%3zu bytes) | %zu\n", Depth, Root + Path, Files, Dirs, Wides,
           Wides * 2, Root + Path + Files);
    printf("      errors beyond the maximum: file %s, dir %s, 2-byte %s\n", FileError ? strerror(FileError) : "none",
           DirError ? strerror(DirError) : "none", WideError ? strerror(WideError) : "none");
    // Descend through a 40-byte directory name for the next depth.
    char Next[PATH_MAX];
    snprintf(Next, sizeof(Next), "%s/dddddddddddddddddddddddddddddddddddddddd", Parent);
    if (mkdir(Next, 0755) != 0) {
      printf("Stopped at depth %d: mkdir of the next level failed with %s\n", Depth + 1, strerror(errno));
      break;
    }
    snprintf(Parent, sizeof(Parent), "%s", Next);
  }
  // Remove the directory chain from the deepest level up.
  while (strlen(Parent) > strlen(Base)) {
    if (rmdir(Parent) != 0) Status = 1;
    *strrchr(Parent, '/') = 0;
  }
  if (rmdir(Base) != 0) Status = 1;
  printf("Name limit probe %s.\n", Status == 42 ? "finished" : "could not clean up");
  return Status;
}
