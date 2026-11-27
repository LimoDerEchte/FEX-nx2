// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct Shared {
  int listener;
  int request;
  int result;
};

static void check(int condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "Thread/IPC probe failed: %s (errno %d)\n", message, errno);
    exit(1);
  }
}

static void *worker(void *argument) {
  struct Shared *shared = argument;
  const int server = accept4(shared->listener, NULL, NULL, SOCK_CLOEXEC);
  check(server >= 0, "accept4");
  char byte;
  check(recv(server, &byte, 1, 0) == 1 && byte == 'x', "receive request");
  check(recv(server, &byte, 1, 0) == 0, "observe peer half-close");
  shared->result = shared->request * 2;
  check(send(server, "ok", 2, MSG_NOSIGNAL) == 2, "send reply");
  check(close(server) == 0, "close accepted socket");
  return shared;
}

int main(void) {
  int channel[2];
  char pipe_byte;
  check(pipe2(channel, O_NONBLOCK | O_CLOEXEC) == 0, "pipe2");
  check((fcntl(channel[0], F_GETFD) & FD_CLOEXEC) != 0, "pipe close-on-exec");
  check(read(channel[0], &pipe_byte, 1) == -1 && errno == EAGAIN, "empty nonblocking pipe");
  check(write(channel[0], "p", 1) == -1 && errno == EBADF, "read-only pipe end");
  check(read(channel[1], &pipe_byte, 1) == -1 && errno == EBADF, "write-only pipe end");
  const int writer = dup(channel[1]);
  check(writer >= 0 && close(channel[1]) == 0, "duplicate pipe writer");
  check(write(writer, "p", 1) == 1 && close(writer) == 0, "write and close pipe");
  struct pollfd pipe_event = {channel[0], POLLIN, 0};
  check(poll(&pipe_event, 1, 0) == 1 && (pipe_event.revents & (POLLIN | POLLHUP)) == (POLLIN | POLLHUP),
        "pipe pending data and hangup");
  check(read(channel[0], &pipe_byte, 1) == 1 && pipe_byte == 'p', "pipe data");
  check(read(channel[0], &pipe_byte, 1) == 0 && close(channel[0]) == 0, "pipe EOF");

  int pair[2];
  check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0, "socketpair");
  char reply[2];
  check(read(pair[1], reply, 1) == -1 && errno == EAGAIN, "nonblocking read");
  struct pollfd event = {pair[1], POLLIN, 0};
  check(poll(&event, 1, 5) == 0, "poll timeout");
  check(write(pair[0], "xy", 2) == 2 && close(pair[0]) == 0, "write and close");
  check(poll(&event, 1, 0) == 1 && (event.revents & POLLIN), "poll queued input");
  check(read(pair[1], reply, 1) == 1 && reply[0] == 'x', "partial stream read");
  check(read(pair[1], reply, sizeof(reply)) == 1 && reply[0] == 'y', "drain stream");
  check(read(pair[1], reply, 1) == 0 && close(pair[1]) == 0, "socket EOF");

  struct Shared *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  check(shared != MAP_FAILED, "shared mmap");
  shared->request = 21;
  shared->listener = socket(AF_UNIX, SOCK_STREAM, 0);
  check(shared->listener >= 0, "listener socket");
  const struct sockaddr_un address = {.sun_family = AF_UNIX, .sun_path = "\0fex-switch2-ipc-probe"};
  check(bind(shared->listener, (const struct sockaddr *)&address, sizeof(address)) == 0, "bind");
  check(listen(shared->listener, 4) == 0, "listen");

  pthread_t thread;
  check(pthread_create(&thread, NULL, worker, shared) == 0, "pthread_create");
  const int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  check(client >= 0, "client socket");
  check(connect(client, (const struct sockaddr *)&address, sizeof(address)) == 0, "connect");
  check(send(client, "x", 1, MSG_NOSIGNAL) == 1, "send request");
  check(shutdown(client, SHUT_WR) == 0, "half-close client");

  void *returned = NULL;
  check(pthread_join(thread, &returned) == 0 && returned == shared, "pthread_join");
  check(shared->result == 42, "shared mapping result");
  check(read(client, reply, sizeof(reply)) == 2 && memcmp(reply, "ok", 2) == 0, "receive reply");
  check(read(client, reply, 1) == 0, "observe server EOF");
  check(close(client) == 0 && close(shared->listener) == 0 && munmap(shared, 4096) == 0, "release resources");
  puts("pthread + Unix sockets + shared mmap = 42");
  return 42;
}
