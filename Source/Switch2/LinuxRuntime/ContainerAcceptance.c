// SPDX-License-Identifier: MIT
// Milestone 4 acceptance: runs the container suite, then the Debian tool
// script, and prints "Container acceptance results: suite N, tools N".
// Exits 0 when both report 42.
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int Run(char *const Arguments[]) {
  fflush(stdout);
  const pid_t Child = fork();
  if (Child < 0) return -1;
  if (!Child) {
    execv(Arguments[0], Arguments);
    _exit(127);
  }
  int Status;
  if (waitpid(Child, &Status, 0) != Child) return -1;
  return WIFEXITED(Status) ? WEXITSTATUS(Status) : 128 + WTERMSIG(Status);
}

int main(void) {
  char *const Suite[] = {"/bin/container-suite.elf", NULL};
  char *const Tools[] = {"/bin/bash", "--noprofile", "--norc", "/bin/container-tools.sh", "./container-tools.scratch", NULL};
  const int SuiteStatus = Run(Suite);
  const int ToolsStatus = Run(Tools);
  printf("Container acceptance results: suite %d, tools %d\n", SuiteStatus, ToolsStatus);
  return SuiteStatus == 42 && ToolsStatus == 42 ? 0 : 1;
}
