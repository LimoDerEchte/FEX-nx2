// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
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

static void check_shared_descriptor(void) {
  int channel[2];
  check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, channel) == 0, "create descriptor channel");
  const pid_t child = fork();
  check(child >= 0, "fork descriptor receiver");

  char byte = 'm';
  struct iovec vector = {&byte, 1};
  union {
    struct cmsghdr alignment;
    char bytes[CMSG_SPACE(sizeof(int))];
  } control = {0};
  struct msghdr message = {
    .msg_iov = &vector, .msg_iovlen = 1,
    .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes),
  };
  if (child == 0) {
    check(close(channel[0]) == 0, "close sender endpoint in child");
    struct pollfd event = {channel[1], POLLIN, 0};
    const struct timespec timeout = {1, 0};
    check(ppoll(&event, 1, &timeout, NULL) == 1 && (event.revents & POLLIN), "wait for descriptor");
    check(recvmsg(channel[1], &message, MSG_CMSG_CLOEXEC) == 1 && byte == 'm', "receive descriptor");
    const struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    check(header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS &&
          header->cmsg_len == CMSG_LEN(sizeof(int)) && !(message.msg_flags & MSG_CTRUNC), "decode descriptor");
    int fd;
    memcpy(&fd, CMSG_DATA(header), sizeof(fd));
    check(fcntl(fd, F_GETFD) == FD_CLOEXEC, "received descriptor is close-on-exec");
    char *pixels = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 4096);
    check(pixels != MAP_FAILED && close(fd) == 0 && pixels[0] == 'p', "map received file after sender close");
    pixels[0] = 'c';
    check(write(channel[1], "r", 1) == 1, "acknowledge shared write");
    check(munmap(pixels, 4096) == 0 && close(channel[1]) == 0, "release child mapping");
    _exit(0);
  }

  check(close(channel[1]) == 0, "close receiver endpoint in parent");
  const int fd = memfd_create("fex-shared-buffer", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  check(fd >= 0 && ftruncate(fd, 8192) == 0, "create memory file");
  check(fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) == 0, "seal memory file against shrink");
  check(ftruncate(fd, 4096) == -1 && errno == EPERM, "respect shrink seal");
  char *pixels = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 4096);
  check(pixels != MAP_FAILED && pixels[0] == 0, "initial shared file contents");
  pixels[0] = 'p';
  check(ftruncate(fd, 12288) == 0 && pixels[0] == 'p', "grow a mapped file");
  struct cmsghdr *header = CMSG_FIRSTHDR(&message);
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  header->cmsg_len = CMSG_LEN(sizeof(fd));
  memcpy(CMSG_DATA(header), &fd, sizeof(fd));
  check(sendmsg(channel[0], &message, MSG_NOSIGNAL) == 1 && close(fd) == 0, "send descriptor and close original");
  check(read(channel[0], &byte, 1) == 1 && byte == 'r' && pixels[0] == 'c', "retain and observe shared writes");
  int status;
  check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "reap descriptor receiver");
  check(munmap(pixels, 4096) == 0 && close(channel[0]) == 0, "release parent mapping");
}

int main(void) {
  run_child();
  run_child();
  puts("fork + pipe + execve + waitpid passed");
  check_shared_descriptor();
  puts("SCM_RIGHTS + shared memfd passed");
  return 42;
}
