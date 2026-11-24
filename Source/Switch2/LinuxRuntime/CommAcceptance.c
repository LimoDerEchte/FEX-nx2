// SPDX-License-Identifier: MIT
// Milestone 3 acceptance: runs the communication suite, then the Debian tool
// script, and prints "Communication acceptance results: suite N, tools N".
// Exits 0 when both report 42.
#include <stdio.h>
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
  char *const Suite[] = {"/bin/comm-suite.elf", NULL};
  char *const Tools[] = {"/bin/bash", "--noprofile", "--norc", "/bin/comm-tools.sh", "./comm-tools.scratch", NULL};
  const int SuiteStatus = Run(Suite);
  const int ToolsStatus = Run(Tools);
  printf("Communication acceptance results: suite %d, tools %d\n", SuiteStatus, ToolsStatus);
  return SuiteStatus == 42 && ToolsStatus == 42 ? 0 : 1;
}
