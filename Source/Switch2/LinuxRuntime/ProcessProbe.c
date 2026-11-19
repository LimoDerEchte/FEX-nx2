// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "Process probe failed: %s (errno %d)\n", message, errno);
    exit(1);
  }
}

static void run_child(void) {
  volatile char private_value = 'p';
  char *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  check(shared != MAP_FAILED, "map shared page");
  int descriptors[2];
  check(pipe2(descriptors, O_CLOEXEC) == 0, "pipe2");
  const pid_t parent = getpid();
  const pid_t child = fork();
  check(child >= 0, "fork");
  if (child == 0) {
    if (getpid() == parent || getppid() != parent) _exit(91);
    private_value = 'c';
    *shared = 's';
    if (close(descriptors[0]) || write(descriptors[1], "x", 1) != 1) _exit(92);
    char *const argv[] = {"true", NULL};
    char *const envp[] = {"LANG=C", NULL};
    execve("/bin/true", argv, envp);
    _exit(93);
  }
  check(close(descriptors[1]) == 0, "parent closes writer");
  char byte;
  check(read(descriptors[0], &byte, 1) == 1 && byte == 'x', "read from inherited pipe");
  check(read(descriptors[0], &byte, 1) == 0, "exec closes child CLOEXEC writer");
  int status = 0;
  check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "wait for exec'd true");
  check(private_value == 'p' && *shared == 's', "private and shared fork mappings");
  check(waitpid(child, &status, 0) == -1 && errno == ECHILD, "child was reaped");
  check(close(descriptors[0]) == 0 && munmap(shared, 4096) == 0, "release resources");
}

int main(void) {
  run_child();
  run_child();
  puts("fork + pipe + execve + waitpid passed");
  return 42;
}
