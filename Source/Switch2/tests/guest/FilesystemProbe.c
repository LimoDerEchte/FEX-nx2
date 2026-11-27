// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <signal.h>
#include <linux/magic.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/sendfile.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/xattr.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned Failures;

#define CHECK(expression) do {   errno = 0;   if (!(expression)) {     fprintf(stderr, "%s: %s (errno %d)\n", #expression, strerror(errno), errno);     ++Failures;   } } while (0)

static void CheckFileIO(int file) {
  char first[2] = {0}, second[3] = {0};
  struct iovec vectors[] = {{first, sizeof(first)}, {second, sizeof(second)}};
  CHECK(pwrite(file, "hello", 5, 0) == 5);
  CHECK(lseek(file, 0, SEEK_CUR) == 0);
  CHECK(readv(file, vectors, 2) == 5);
  CHECK(memcmp(first, "he", 2) == 0 && memcmp(second, "llo", 3) == 0);
  CHECK(lseek(file, 1, SEEK_DATA) == 1);
  CHECK(lseek(file, 1, SEEK_HOLE) == 5);
  CHECK((lseek(file, 5, SEEK_DATA) == -1) && errno == ENXIO);
  CHECK((lseek(file, 5, SEEK_HOLE) == -1) && errno == ENXIO);
  CHECK((lseek(file, -1, SEEK_DATA) == -1) && errno == ENXIO);
  CHECK(lseek(file, 0, SEEK_CUR) == 5);
  CHECK(fallocate(file, 0, 4096, 4096) == 0);
  struct stat allocated = {0};
  CHECK(fstat(file, &allocated) == 0 && allocated.st_size == 8192);
  CHECK(sync_file_range(file, 4096, 4096, SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER) == 0);
  CHECK(ftruncate(file, 8192) == 0);
  char tail = 1;
  CHECK(pread(file, &tail, 1, 8191) == 1 && tail == 0);
  CHECK((pread(file, &tail, 0, -1) == -1) && errno == EINVAL);
  CHECK(fcntl(file, F_SETFL, O_APPEND) == 0);
  CHECK(pwrite(file, "!", 1, 0) == 1);
  CHECK(pread(file, &tail, 1, 8192) == 1 && tail == '!');
  CHECK(lseek(file, 0, SEEK_CUR) == 5);
  CHECK(fcntl(file, F_SETFL, 0) == 0);
  struct iovec output[] = {{"ab", 2}, {"cde", 3}};
  CHECK(pwritev(file, output, 2, 8) == 5);
  CHECK(preadv(file, vectors, 2, 8) == 5);
  CHECK(memcmp(first, "ab", 2) == 0 && memcmp(second, "cde", 3) == 0);
  CHECK(syscall(SYS_pwritev2, file, output, 2, 0UL, 0UL, RWF_APPEND) == 5);
  CHECK(syscall(SYS_preadv2, file, vectors, 2, 8193UL, 0UL, 0) == 5);
  CHECK(memcmp(first, "ab", 2) == 0 && memcmp(second, "cde", 3) == 0);
  CHECK(lseek(file, 0, SEEK_CUR) == 5);
  CHECK(syscall(SYS_preadv2, file, vectors, 2, -1L, 0UL, 0) == 5);
  CHECK(lseek(file, 0, SEEK_CUR) == 10);
  CHECK(fsync(file) == 0);
  CHECK(fcntl(file, F_GETFD) == FD_CLOEXEC);
  int duplicate = dup(file);
  CHECK(duplicate >= 0);
  if (duplicate >= 0) {
    CHECK(fcntl(duplicate, F_GETFD) == 0);
    CHECK(lseek(duplicate, 1, SEEK_SET) == 1);
    CHECK(lseek(file, 0, SEEK_CUR) == 1);
    CHECK(close(duplicate) == 0);
  }
}

static void CheckPartialIO(int file) {
  char *pages = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(pages != MAP_FAILED);
  if (pages == MAP_FAILED) return;
  int protected = mprotect(pages + 4096, 4096, PROT_NONE);
  CHECK(protected == 0);
  if (protected == 0) {
    char *boundary = pages + 4094;
    memcpy(boundary, "xy", 2);
    CHECK(lseek(file, 0, SEEK_SET) == 0);
    CHECK(write(file, boundary, 4) == 2);
    CHECK(lseek(file, 0, SEEK_CUR) == 2);
    CHECK(pwrite(file, boundary, 4, 2) == 2);
    CHECK(lseek(file, 0, SEEK_CUR) == 2);
    memset(boundary, 0, 2);
    CHECK(pread(file, boundary, 4, 0) == 2 && memcmp(boundary, "xy", 2) == 0);
    CHECK(lseek(file, 0, SEEK_CUR) == 2);
    memset(boundary, 0, 2);
    CHECK(read(file, boundary, 4) == 2 && memcmp(boundary, "xy", 2) == 0);
    CHECK(lseek(file, 0, SEEK_CUR) == 4);
  }
  CHECK(munmap(pages, 8192) == 0);
}

static void CheckVectorIO(int file) {
  const unsigned unknown = 0x80000000U;
  char byte = 0;
  struct iovec empty = {NULL, 0};
  struct iovec invalid = {&byte, (size_t)-1};
  struct iovec one = {&byte, 1};
  CHECK(syscall(SYS_preadv2, (uint64_t)file | (1ULL << 32), &one, (1ULL << 32) | 1, 0UL, 0UL, 0) == 1);
  CHECK(syscall(SYS_preadv2, file, &one, 1, 0UL, 0UL, RWF_HIPRI | RWF_DSYNC | RWF_SYNC | RWF_APPEND) == 1);
  CHECK(syscall(SYS_preadv2, file, &empty, 1, 0UL, 0UL, unknown) == 0);
  CHECK(syscall(SYS_pwritev2, file, NULL, 0, 0UL, 0UL, unknown) == 0);
  CHECK(syscall(SYS_preadv2, file, NULL, 1, 0UL, 0UL, unknown) == -1 && errno == EFAULT);
  CHECK(syscall(SYS_pwritev2, file, &invalid, 1, 0UL, 0UL, unknown) == -1 && errno == EINVAL);
  CHECK(syscall(SYS_preadv2, file, &one, 1, 0UL, 0UL, unknown) == -1 && errno == EOPNOTSUPP);
  struct iovec unmapped = {(void *)1, 1};
  struct iovec outside = {(void *)UINTPTR_MAX, 1};
  CHECK(syscall(SYS_pread64, file, unmapped.iov_base, 1UL, 65536UL) == 0);
  CHECK(syscall(SYS_pread64, file, unmapped.iov_base, 1UL, 0UL) == -1 && errno == EFAULT);
  CHECK(syscall(SYS_pread64, file, outside.iov_base, 0UL, 65536UL) == -1 && errno == EFAULT);
  CHECK(syscall(SYS_pread64, file, unmapped.iov_base, 1UL, INT64_MAX) == -1 && errno == EINVAL);
  CHECK(syscall(SYS_pwrite64, file, unmapped.iov_base, 1UL, INT64_MAX) == -1 && errno == EINVAL);
  CHECK(syscall(SYS_pwrite64, file, outside.iov_base, 1UL, INT64_MAX) == -1 && errno == EFAULT);
  CHECK(syscall(SYS_write, file, outside.iov_base, 0UL) == -1 && errno == EFAULT);
  off_t current = lseek(file, 0, SEEK_CUR);
  CHECK(current >= 0);
  CHECK(lseek(file, 65536, SEEK_SET) == 65536);
  CHECK(syscall(SYS_read, file, unmapped.iov_base, 1UL) == 0);
  CHECK(lseek(file, 0, SEEK_CUR) == 65536);
  CHECK(lseek(file, current, SEEK_SET) == current);
  CHECK(syscall(SYS_preadv2, file, &unmapped, 1, 65536UL, 0UL, 0) == 0);
  CHECK(syscall(SYS_preadv2, file, &unmapped, 1, INT64_MAX, 0UL, unknown) == -1 && errno == EINVAL);
  CHECK(syscall(SYS_preadv2, file, &unmapped, 1, 0UL, 0UL, 0) == -1 && errno == EFAULT);
  CHECK(syscall(SYS_preadv2, file, &outside, 1, 0UL, 0UL, unknown) == -1 && errno == EFAULT);
  int null = open("/dev/null", O_RDWR | O_CLOEXEC);
  CHECK(null >= 0);
  if (null >= 0) {
    CHECK(syscall(SYS_write, null, unmapped.iov_base, 0x80000000UL) == 0x7ffff000);
    CHECK(syscall(SYS_read, null, unmapped.iov_base, 1UL) == 0);
    CHECK(syscall(SYS_pwrite64, null, unmapped.iov_base, 1UL, 0UL) == 1);
    CHECK(syscall(SYS_pread64, null, unmapped.iov_base, 1UL, 0UL) == 0);
    CHECK(syscall(SYS_read, null, outside.iov_base, 0UL) == -1 && errno == EFAULT);
    CHECK(syscall(SYS_pwrite64, null, unmapped.iov_base, 1UL, INT64_MAX) == -1 && errno == EINVAL);
    CHECK(writev(null, &unmapped, 1) == 1);
    CHECK(readv(null, &unmapped, 1) == 0);
    CHECK(writev(null, &outside, 1) == -1 && errno == EFAULT);
    CHECK(syscall(SYS_pwritev2, null, &unmapped, 1, 0UL, 0UL, RWF_NOWAIT) == 1);
    CHECK(close(null) == 0);
  }

  int memory = memfd_create("vector-io", MFD_CLOEXEC);
  CHECK(memory >= 0);
  if (memory < 0) return;
  CHECK(syscall(SYS_preadv2, memory, &one, 1, 0UL, 0UL, RWF_NOWAIT) == -1 && errno == EOPNOTSUPP);
  char data[32 * 1024];
  memset(data, 'v', sizeof(data));
  struct iovec vectors[] = {{data, sizeof(data)}, {data, sizeof(data)}, {data, sizeof(data)}};
  CHECK(pwritev(memory, vectors, 3, 0) == 3 * sizeof(data));
  memset(data, 0, sizeof(data));
  CHECK(preadv(memory, vectors, 3, 0) == 3 * sizeof(data));
  CHECK(data[0] == 'v' && data[sizeof(data) - 1] == 'v');
  CHECK(lseek(memory, 0, SEEK_CUR) == 0);
  CHECK(close(memory) == 0);
}

static void CheckPartialFileIO(int directory) {
  int file = openat(directory, "partial-io", O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(file >= 0);
  if (file >= 0) {
    CheckPartialIO(file);
    CHECK(close(file) == 0);
    CHECK(unlinkat(directory, "partial-io", 0) == 0);
  }
  int memory = memfd_create("partial-io", MFD_CLOEXEC);
  CHECK(memory >= 0);
  if (memory >= 0) {
    CheckPartialIO(memory);
    CHECK(close(memory) == 0);
  }
}

static void CheckFilesystemCredentials(int directory) {
  uid_t real = 0, effective = 0, saved = 0;
  gid_t group = 0, effective_group = 0, saved_group = 0;
  CHECK(getresuid(&real, &effective, &saved) == 0 && real == getuid() && effective == geteuid());
  CHECK(getresgid(&group, &effective_group, &saved_group) == 0 &&
        group == getgid() && effective_group == getegid());
  CHECK(getgroups(0, NULL) >= 0);
  if (geteuid() != 0) {
    CHECK((setresuid(0, 0, 0) == -1) && errno == EPERM);
    CHECK(geteuid() == effective);
  }
  CHECK(mkdirat(directory, "group-parent", 0700) == 0);
  int parent = openat(directory, "group-parent", O_RDONLY | O_DIRECTORY);
  CHECK(parent >= 0);
  if (parent >= 0) {
    CHECK(fchmod(parent, 02700) == 0);
    CHECK(mkdirat(parent, "child", 0700) == 0);
    struct stat original = {0}, child = {0};
    CHECK(fstat(parent, &original) == 0);
    CHECK(fstatat(parent, "child", &child, 0) == 0);
    CHECK(child.st_uid == geteuid() && child.st_gid == original.st_gid && (child.st_mode & S_ISGID));
    CHECK(unlinkat(parent, "child", AT_REMOVEDIR) == 0);
    CHECK(close(parent) == 0);
  }
  CHECK(unlinkat(directory, "group-parent", AT_REMOVEDIR) == 0);

  pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    if (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0 ||
        prctl(PR_GET_NO_NEW_PRIVS, 0UL, 0UL, 0UL, 0UL) != 1 ||
        geteuid() != effective || getegid() != effective_group) _exit(1);
    _exit(0);
  }
  if (child > 0) {
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
}

static void CheckSetIdOnWrite(int directory) {
  const mode_t preserved = geteuid() == 0 ? S_ISUID | S_ISGID : 0;
  int fd = openat(directory, "set-id-test", O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(fd >= 0);
  if (fd < 0) return;
  struct stat info = {0};
  CHECK(fchmod(fd, 06710) == 0);
  CHECK(pwrite(fd, "x", 1, 0) == 1);
  CHECK(fstat(fd, &info) == 0 && (info.st_mode & 07777) == (0710 | preserved));
  CHECK(fchmod(fd, 06600) == 0);
  CHECK(ftruncate(fd, 1) == 0);
  CHECK(fstat(fd, &info) == 0 && (info.st_mode & 07777) == (02600 | preserved));
  CHECK(fchmod(fd, 06710) == 0);
  CHECK(fallocate(fd, 0, 0, 1) == 0);
  CHECK(fstat(fd, &info) == 0 && (info.st_mode & 07777) == (0710 | preserved));
  CHECK(fchmod(fd, 06710) == 0);
  int truncated = openat(directory, "set-id-test", O_WRONLY | O_TRUNC);
  CHECK(truncated >= 0);
  if (truncated >= 0) CHECK(close(truncated) == 0);
  CHECK(fstat(fd, &info) == 0 && (info.st_mode & 07777) == (0710 | preserved) && info.st_size == 0);
  CHECK(close(fd) == 0);
  CHECK(unlinkat(directory, "set-id-test", 0) == 0);
}

static void CheckZeroRange(int directory) {
  int file = openat(directory, "zero-range", O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(file >= 0);
  if (file < 0) return;
  CHECK(fallocate(file, FALLOC_FL_KEEP_SIZE, 4096, 4096) == 0);
  struct stat reserved = {0};
  CHECK(fstatat(directory, "zero-range", &reserved, 0) == 0 && reserved.st_size == 0 && reserved.st_blocks >= 8);
  struct stat opened = {0};
  CHECK(fstat(file, &opened) == 0 && opened.st_blocks == reserved.st_blocks);
  char tail = 1;
  CHECK(pread(file, &tail, 1, 4096) == 0);
  CHECK(write(file, "abcdef", 6) == 6);
  CHECK(fallocate(file, FALLOC_FL_ZERO_RANGE | FALLOC_FL_KEEP_SIZE, 1, 2) == 0);
  char data[9];
  CHECK(pread(file, data, sizeof(data), 0) == 6 && memcmp(data, "a\0\0def", 6) == 0);
  CHECK(fallocate(file, FALLOC_FL_ZERO_RANGE, 4, 5) == 0);
  CHECK(pread(file, data, sizeof(data), 0) == 9 && memcmp(data, "a\0\0d\0\0\0\0\0", 9) == 0);
  CHECK(lseek(file, 0, SEEK_CUR) == 6);
  CHECK(fallocate(file, FALLOC_FL_ZERO_RANGE | FALLOC_FL_KEEP_SIZE, 12288, 4096) == 0);
  CHECK(fstat(file, &opened) == 0 && opened.st_size == 9);
  CHECK(pread(file, data, sizeof(data), 12288) == 0);
  CHECK(fallocate(file, FALLOC_FL_ZERO_RANGE, 12288, 4096) == 0);
  CHECK(fstat(file, &opened) == 0 && opened.st_size == 16384);
  CHECK(pread(file, data, sizeof(data), 4096) == sizeof(data) && memcmp(data, (char[9]){0}, sizeof(data)) == 0);
  CHECK(lseek(file, 0, SEEK_CUR) == 6);
  CHECK(ftruncate(file, 16384) == 0);
  CHECK(pwrite(file, "z", 1, 16383) == 1);
  CHECK(pwrite(file, "ab", 2, 4096) == 2);
  CHECK(fallocate(file, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 4097, 1) == 0);
  CHECK(pread(file, data, 2, 4096) == 2 && memcmp(data, "a\0", 2) == 0);
  CHECK(fallocate(file, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 16383, 1) == 0);
  CHECK(lseek(file, 16383, SEEK_DATA) == 16383);
  CHECK(pread(file, &tail, 1, 16383) == 1 && tail == 0);
  CHECK(pwrite(file, "z", 1, 16383) == 1);
  CHECK(fallocate(file, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 8192, 4096) == 0);
  CHECK(lseek(file, 8192, SEEK_HOLE) == 8192);
  CHECK(lseek(file, 8192, SEEK_DATA) == 12288);
  CHECK(pread(file, &tail, 1, 8192) == 1 && tail == 0);
  CHECK(pread(file, &tail, 1, 16383) == 1 && tail == 'z');
  CHECK(pwrite(file, "q", 1, 8193) == 1);
  CHECK(pread(file, data, 3, 8192) == 3 && memcmp(data, "\0q\0", 3) == 0);
  CHECK(fallocate(file, FALLOC_FL_ZERO_RANGE | FALLOC_FL_KEEP_SIZE, 8193, 1) == 0);
  CHECK(pread(file, data, 3, 8192) == 3 && memcmp(data, (char[3]){0}, 3) == 0);
  CHECK(fallocate(file, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 8192, 8192) == 0);
  CHECK(fstat(file, &opened) == 0 && opened.st_size == 16384);
  tail = 1;
  CHECK(pread(file, &tail, 1, 16383) == 1 && tail == 0);
  CHECK(lseek(file, 12288, SEEK_HOLE) == 12288);
  CHECK((lseek(file, 12288, SEEK_DATA) == -1) && errno == ENXIO);
  CHECK(fallocate(file, FALLOC_FL_INSERT_RANGE, 4096, 4096) == 0);
  CHECK(fstat(file, &opened) == 0 && opened.st_size == 20480);
  CHECK(pread(file, &tail, 1, 4096) == 1 && tail == 0);
  CHECK(pread(file, &tail, 1, 8192) == 1 && tail == 'a');
  CHECK(lseek(file, 4096, SEEK_DATA) == 8192);
  CHECK(fallocate(file, FALLOC_FL_COLLAPSE_RANGE, 4096, 4096) == 0);
  CHECK(fstat(file, &opened) == 0 && opened.st_size == 16384);
  CHECK(pread(file, &tail, 1, 4096) == 1 && tail == 'a');
  CHECK(fallocate(file, FALLOC_FL_COLLAPSE_RANGE, 12288, 4096) == -1 && errno == EINVAL);
  CHECK(fallocate(file, FALLOC_FL_INSERT_RANGE, 1, 4096) == -1 && errno == EINVAL);
  CHECK(ftruncate(file, 0) == 0);
  CHECK(fstat(file, &opened) == 0 && opened.st_size == 0 && opened.st_blocks == 0);
  CHECK(close(file) == 0);
  CHECK(unlinkat(directory, "zero-range", 0) == 0);
}

static void CheckAllocationErrors(int directory, int file) {
  CHECK((ftruncate(-1, -1) == -1) && errno == EINVAL);
  CHECK((ftruncate(-1, 0) == -1) && errno == EBADF);
  CHECK((ftruncate(directory, 0) == -1) && errno == EINVAL);
  CHECK((fallocate(-1, 0, -1, 0) == -1) && errno == EBADF);
  CHECK((sync_file_range(-1, -1, 0, 8) == -1) && errno == EBADF);
  CHECK((fallocate(file, 0, 0, 0) == -1) && errno == EINVAL);
  CHECK((fallocate(file, FALLOC_FL_PUNCH_HOLE, 0, 1) == -1) && errno == EOPNOTSUPP);
  CHECK(fallocate(file, FALLOC_FL_INSERT_RANGE | FALLOC_FL_KEEP_SIZE, 0, 4096) == -1 && errno == EOPNOTSUPP);
  CHECK(fallocate(file, FALLOC_FL_COLLAPSE_RANGE | FALLOC_FL_ZERO_RANGE, 0, 4096) == -1 && errno == EOPNOTSUPP);
  CHECK(syscall(SYS_fallocate, (uint64_t)file | (1ULL << 32), (1ULL << 32) | FALLOC_FL_KEEP_SIZE, 0UL, 1UL) == 0);
  CHECK(fallocate(directory, FALLOC_FL_UNSHARE_RANGE, 0, 4096) == -1 && errno == EBADF);
  CHECK((sync_file_range(file, INT64_MAX, 1, SYNC_FILE_RANGE_WRITE) == -1) && errno == EINVAL);
  CHECK(syscall(SYS_sync_file_range, file, 0UL, 0UL, 1ULL << 32) == 0);
  CHECK((fallocate(directory, 0, 0, 1) == -1) && errno == EBADF);

  int endpoints[2];
  int result = pipe(endpoints);
  CHECK(result == 0);
  if (result == 0) {
    CHECK((fallocate(endpoints[1], 0, 0, 1) == -1) && errno == ESPIPE);
    CHECK((sync_file_range(endpoints[1], 0, 0, 0) == -1) && errno == ESPIPE);
    CHECK(close(endpoints[0]) == 0);
    CHECK(close(endpoints[1]) == 0);
  }
  int path = openat(directory, "file", O_PATH);
  CHECK(path >= 0);
  if (path >= 0) {
    CHECK((fallocate(path, 0, -1, 0) == -1) && errno == EBADF);
    CHECK((sync_file_range(path, 0, 0, 0) == -1) && errno == EBADF);
    CHECK(close(path) == 0);
  }
}

static void CheckSparseTruncate(int directory) {
  int file = openat(directory, "sparse-truncate", O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(file >= 0);
  if (file < 0) return;
  struct stat before = {0}, after = {0};
  CHECK(fstat(file, &before) == 0);
  CHECK(ftruncate(file, 16384) == 0);
  CHECK(fstat(file, &after) == 0 && after.st_size == 16384 && after.st_blocks == before.st_blocks);
  CHECK(lseek(file, 8192, SEEK_HOLE) == 8192);
  CHECK(lseek(file, 8192, SEEK_DATA) == -1 && errno == ENXIO);
  char byte = 1;
  CHECK(pread(file, &byte, 1, 16383) == 1 && byte == 0);
  CHECK(lseek(file, 0, SEEK_CUR) == 8192);
  CHECK(fstatat(directory, "sparse-truncate", &after, 0) == 0 && after.st_size == 16384);
  CHECK(pwrite(file, "z", 1, 12288) == 1);
  char boundary[3] = {1, 1, 1};
  CHECK(pread(file, boundary, sizeof(boundary), 12287) == sizeof(boundary) &&
        memcmp(boundary, "\0z\0", sizeof(boundary)) == 0);
  CHECK(fstat(file, &after) == 0 && after.st_size == 16384);
  CHECK(ftruncate(file, 0) == 0);
  CHECK(ftruncate(file, 16384) == 0);
  int previous = open(".", O_PATH | O_DIRECTORY);
  CHECK(previous >= 0);
  if (previous >= 0) {
    if (fchdir(directory) == 0) {
      CHECK(symlink("sparse-truncate", "sparse-alias") == 0);
      CHECK(truncate("sparse-alias", 32768) == 0);
      CHECK(fstat(file, &after) == 0 && after.st_size == 32768 && after.st_blocks == before.st_blocks);
      CHECK(unlink("sparse-alias") == 0);
      CHECK(fchdir(previous) == 0);
    } else {
      CHECK(0);
    }
    CHECK(close(previous) == 0);
  }
  CHECK(close(file) == 0);
  CHECK(unlinkat(directory, "sparse-truncate", 0) == 0);
}

static void CheckSparseExec(int directory) {
  int source = open("/bin/true", O_RDONLY);
  CHECK(source >= 0);
  if (source < 0) return;
  int target = openat(directory, "sparse-exec", O_CREAT | O_EXCL | O_RDWR, 0700);
  CHECK(target >= 0);
  if (target >= 0) {
    char buffer[4096];
    ssize_t count;
    while ((count = read(source, buffer, sizeof(buffer))) > 0) {
      ssize_t written = write(target, buffer, count);
      CHECK(written == count);
      if (written != count) break;
    }
    CHECK(count == 0);
    struct stat before = {0}, after = {0};
    CHECK(fstat(target, &before) == 0);
    CHECK(ftruncate(target, before.st_size + 16384) == 0);
    CHECK(fstat(target, &after) == 0 && after.st_blocks == before.st_blocks);
    CHECK(linkat(directory, "sparse-exec", directory, "sparse-exec-alias", 0) == 0);
    pid_t busy = fork();
    CHECK(busy >= 0);
    if (busy == 0) {
      if (fchdir(directory) != 0) _exit(1);
      execl("./sparse-exec-alias", "sparse-exec-alias", (char *)NULL);
      _exit(errno == ETXTBSY ? 42 : 1);
    }
    if (busy > 0) {
      int status = 0;
      CHECK(waitpid(busy, &status, 0) == busy && WIFEXITED(status) && WEXITSTATUS(status) == 42);
    }
    CHECK(close(target) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
      if (fchdir(directory) == 0) execl("./sparse-exec", "sparse-exec", (char *)NULL);
      _exit(1);
    }
    if (child > 0) {
      int status = 0;
      CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    CHECK(unlinkat(directory, "sparse-exec", 0) == 0);
    CHECK(unlinkat(directory, "sparse-exec-alias", 0) == 0);
  }
  CHECK(close(source) == 0);
}

static void CheckMemfdAllocation(void) {
  int file = memfd_create("fs-allocation", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  CHECK(file >= 0);
  if (file < 0) return;
  struct stat info = {0};
  CHECK(fallocate(file, FALLOC_FL_KEEP_SIZE, 4096, 4096) == 0);
  CHECK(fstat(file, &info) == 0 && info.st_size == 0);
  CHECK(pwrite(file, "x", 1, 8191) == 1);
  CHECK(fstat(file, &info) == 0 && info.st_size == 8192);
  char bytes[2] = {1, 1};
  CHECK(pread(file, bytes, sizeof(bytes), 8190) == 2 && bytes[0] == 0 && bytes[1] == 'x');
  char *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, file, 4096);
  CHECK(shared != MAP_FAILED);
  if (shared != MAP_FAILED) {
    shared[4094] = 'y';
    CHECK(fallocate(file, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 8191, 1) == 0);
    CHECK(shared[4094] == 'y' && shared[4095] == 0);
    CHECK(pread(file, bytes, sizeof(bytes), 8190) == 2 && bytes[0] == 'y' && bytes[1] == 0);
    CHECK(fstat(file, &info) == 0 && info.st_size == 8192);
    CHECK(munmap(shared, 4096) == 0);
  }
  CHECK(fallocate(file, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 8192, 4096) == 0);
  CHECK(ftruncate(file, 8191) == 0);
  shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, file, 4096);
  CHECK(shared != MAP_FAILED);
  if (shared != MAP_FAILED) {
    shared[4094] = 'x';
    shared[4095] = 'z';
    CHECK(fallocate(file, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 8190, 1) == 0);
    CHECK(shared[4094] == 0 && shared[4095] == 'z');
    CHECK(fallocate(file, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 8190, 2) == 0);
    CHECK(shared[4094] == 0 && shared[4095] == 0);
    CHECK(fstat(file, &info) == 0 && info.st_size == 8191);
    CHECK(munmap(shared, 4096) == 0);
  }
  CHECK(ftruncate(file, 8192) == 0);
  CHECK(fcntl(file, F_ADD_SEALS, F_SEAL_GROW) == 0);
  CHECK(fallocate(file, FALLOC_FL_KEEP_SIZE, 0, 4096) == 0);
  CHECK((fallocate(file, FALLOC_FL_KEEP_SIZE, 8192, 1) == -1) && errno == EPERM);
  CHECK((fallocate(file, 0, 8192, 1) == -1) && errno == EPERM);
  CHECK(pwrite(file, "ab", 2, 8191) == -1 && errno == EPERM);
  CHECK(pwrite(file, "ab", 2, 8192) == -1 && errno == EPERM);
  struct iovec vectors[] = {{"c", 1}, {"d", 1}};
  CHECK(pwritev(file, vectors, 2, 8191) == -1 && errno == EPERM);
  CHECK(lseek(file, 8191, SEEK_SET) == 8191);
  CHECK(writev(file, vectors, 2) == -1 && errno == EPERM);
  CHECK(lseek(file, 0, SEEK_CUR) == 8191);
  CHECK(pread(file, bytes, sizeof(bytes), 8191) == 1 && bytes[0] == 0);
  CHECK(pwritev(file, vectors, 2, 8190) == 2);
  CHECK(pwrite(file, "x", 1, 64 * 1024 * 1024) == -1 && errno == EPERM);
  CHECK(fallocate(file, FALLOC_FL_KEEP_SIZE, 64 * 1024 * 1024, 1) == -1 && errno == EPERM);
  CHECK(fstat(file, &info) == 0 && info.st_size == 8192);
  CHECK(close(file) == 0);
}

static void CheckMemfdAppend(void) {
  int file = memfd_create("fs-append", MFD_CLOEXEC);
  CHECK(file >= 0);
  if (file < 0) return;
  CHECK(write(file, "abc", 3) == 3);
  CHECK(lseek(file, 0, SEEK_SET) == 0);
  int duplicate = dup(file);
  CHECK(duplicate >= 0);
  if (duplicate >= 0) {
    CHECK(fcntl(duplicate, F_SETFL, O_APPEND) == 0);
    CHECK(fcntl(file, F_GETFL) & O_APPEND);
    CHECK(pwrite(file, "d", 1, 0) == 1);
    CHECK(lseek(duplicate, 0, SEEK_CUR) == 0);
    CHECK(write(duplicate, "e", 1) == 1);
    CHECK(lseek(file, 0, SEEK_CUR) == 5);
    CHECK(fcntl(file, F_SETFL, 0) == 0);
    CHECK(!(fcntl(duplicate, F_GETFL) & O_APPEND));
    CHECK(pwrite(duplicate, "z", 1, 0) == 1);
    char bytes[5];
    CHECK(pread(file, bytes, sizeof(bytes), 0) == sizeof(bytes) && memcmp(bytes, "zbcde", 5) == 0);
    CHECK(close(duplicate) == 0);
  }
  CHECK(close(file) == 0);
}

static void CheckMemfdWriteSeals(void) {
  int file = memfd_create("fs-seals", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  CHECK(file >= 0);
  if (file < 0) return;
  CHECK(ftruncate(file, 4096) == 0);
  char *mapping = mmap(NULL, 4096, PROT_READ, MAP_SHARED, file, 0);
  CHECK(mapping != MAP_FAILED);
  if (mapping != MAP_FAILED) {
    CHECK(fcntl(file, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EBUSY);
    CHECK(fcntl(file, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) == 0);
    CHECK(mprotect(mapping, 4096, PROT_READ | PROT_WRITE) == 0);
    mapping[0] = 's';
    CHECK(munmap(mapping, 4096) == 0);
  }
  char byte = 0;
  CHECK(pread(file, &byte, 1, 0) == 1 && byte == 's');
  CHECK(pwrite(file, "x", 1, 0) == -1 && errno == EPERM);
  CHECK(pwrite(file, "", 0, 0) == 0);
  CHECK(fallocate(file, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 4096) == -1 && errno == EPERM);
  CHECK(mmap(NULL, 4096, PROT_WRITE, MAP_SHARED, file, 0) == MAP_FAILED && errno == EPERM);
  mapping = mmap(NULL, 4096, PROT_READ, MAP_SHARED, file, 0);
  CHECK(mapping != MAP_FAILED);
  if (mapping != MAP_FAILED) {
    CHECK(fcntl(file, F_ADD_SEALS, F_SEAL_WRITE) == 0);
    CHECK(mprotect(mapping, 4096, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
    CHECK(mapping[0] == 's');
    CHECK(munmap(mapping, 4096) == 0);
  }
  CHECK(fcntl(file, F_GET_SEALS) == (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE));
  CHECK(close(file) == 0);
}

static void *UnshareDescriptor(void *opaque) {
  const int file = *(const int *)opaque;
  if (syscall(SYS_close_range, file, file, 2U | 4U) != 0) return opaque;
  if (fcntl(file, F_GETFD) != FD_CLOEXEC) return opaque;
  if (lseek(file, 2, SEEK_SET) != 2) return opaque;
  if (syscall(SYS_close_range, file, file, 0U) != 0) return opaque;
  errno = 0;
  if (fcntl(file, F_GETFD) != -1 || errno != EBADF) return opaque;
  return NULL;
}

static void CheckCloseRange(int source) {
  int child_fd = dup(source);
  CHECK(child_fd >= 0);
  if (child_fd < 0) return;
  pthread_t thread;
  int started = pthread_create(&thread, NULL, UnshareDescriptor, &child_fd);
  CHECK(started == 0);
  if (started == 0) {
    void *result = &child_fd;
    CHECK(pthread_join(thread, &result) == 0 && result == NULL);
    CHECK(fcntl(child_fd, F_GETFD) == 0);
    CHECK(lseek(source, 0, SEEK_CUR) == 2);
  }
  CHECK(close(child_fd) == 0);
}

static void CheckCopyRange(int directory, int source) {
  int output = openat(directory, "copy", O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(output >= 0);
  if (output < 0) return;
  off64_t input_at = 0, output_at = 2;
  CHECK(lseek(source, 1, SEEK_SET) == 1);
  CHECK(copy_file_range(source, &input_at, output, &output_at, 5, 0) == 5);
  CHECK(input_at == 5 && output_at == 7);
  CHECK(lseek(source, 0, SEEK_CUR) == 1 && lseek(output, 0, SEEK_CUR) == 0);
  char bytes[7];
  CHECK(pread(output, bytes, sizeof(bytes), 0) == sizeof(bytes) && memcmp(bytes, "\0\0hello", 7) == 0);
  input_at = 0;
  output_at = 1;
  CHECK((copy_file_range(source, &input_at, source, &output_at, 5, 0) == -1) && errno == EINVAL);
  CHECK(fcntl(output, F_SETFL, O_APPEND) == 0);
  CHECK((copy_file_range(source, NULL, output, NULL, 2, 0) == -1) && errno == EBADF);
  CHECK(fcntl(output, F_SETFL, 0) == 0);
  CHECK(copy_file_range(source, NULL, output, NULL, 2, 0) == 2);
  CHECK(lseek(source, 0, SEEK_CUR) == 3 && lseek(output, 0, SEEK_CUR) == 2);
  CHECK((copy_file_range(-1, NULL, output, NULL, 2, 1) == -1) && errno == EBADF);
  off64_t negative = -1;
  CHECK(syscall(SYS_copy_file_range, source, (void *)1, output, NULL, 0, 1) == -1 && errno == EFAULT);
  CHECK(syscall(SYS_copy_file_range, source, &negative, output, (void *)1, 0, 0) == -1 && errno == EFAULT);
  CHECK(copy_file_range(source, &negative, output, NULL, 0, 0) == -1 && errno == EINVAL);
  CHECK(copy_file_range(source, NULL, output, &negative, 1, 0) == -1 && errno == EOVERFLOW);
  CHECK(negative == -1);
  input_at = INT64_MAX;
  output_at = 0;
  CHECK(copy_file_range(source, &input_at, output, &output_at, 2, 0) == 0);
  CHECK(input_at == INT64_MAX && output_at == 0);
  off64_t *read_only = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(read_only != MAP_FAILED);
  if (read_only != MAP_FAILED) {
    CHECK(copy_file_range(source, read_only, output, &output_at, 0, 0) == 0);
    CHECK((copy_file_range(source, read_only, output, &output_at, 5, 0) == -1) && errno == EFAULT);
    CHECK(output_at == 5 && *read_only == 0);
    CHECK(lseek(source, 0, SEEK_CUR) == 3 && lseek(output, 0, SEEK_CUR) == 2);
    CHECK(pread(output, bytes, 5, 0) == 5 && memcmp(bytes, "hello", 5) == 0);
    CHECK(munmap(read_only, 4096) == 0);
  }
  CHECK(close(output) == 0);
  CHECK(unlinkat(directory, "copy", 0) == 0);
}

static void CheckLargeTransfers(int directory) {
  int source = openat(directory, "large-source", O_CREAT | O_EXCL | O_RDWR, 0600);
  int target = openat(directory, "large-target", O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(source >= 0 && target >= 0);
  const size_t total = 160 * 1024 + 13;
  unsigned char pattern[4096], actual[4096];
  if (source >= 0 && target >= 0) {
    unsigned char *large = malloc(total);
    CHECK(large != NULL);
    if (large) {
      for (size_t i = 0; i < total; ++i) large[i] = i % 251;
      CHECK(write(source, large, total) == (ssize_t)total);
      CHECK(lseek(source, 0, SEEK_CUR) == (off_t)total);
      CHECK(pwrite(source, large, total, 0) == (ssize_t)total);
      CHECK(lseek(source, 0, SEEK_CUR) == (off_t)total);
      CHECK(lseek(source, 0, SEEK_SET) == 0);
      memset(large, 0, total);
      CHECK(read(source, large, total) == (ssize_t)total);
      CHECK(lseek(source, 0, SEEK_CUR) == (off_t)total);
      for (size_t i = 0; i < total; ++i) {
        if (large[i] != i % 251) {
          CHECK(large[i] == i % 251);
          break;
        }
      }
      CHECK(pread(source, large, total, 0) == (ssize_t)total);
      CHECK(lseek(source, 0, SEEK_CUR) == (off_t)total);
      free(large);
    }
    CHECK(lseek(source, 0, SEEK_SET) == 0);
    off64_t output = 7;
    size_t copied = 0;
    while (copied < total) {
      ssize_t moved = copy_file_range(source, NULL, target, &output, total - copied, 0);
      CHECK(moved > 0 && moved <= (ssize_t)(total - copied));
      if (moved <= 0 || moved > (ssize_t)(total - copied)) break;
      copied += moved;
    }
    CHECK(copied == total && output == (off64_t)total + 7);
    CHECK(lseek(source, 0, SEEK_CUR) == (off_t)total);
    CHECK(lseek(target, 0, SEEK_CUR) == 0);
    CHECK(copy_file_range(source, NULL, target, &output, 1, 0) == 0);
    for (size_t offset = 0; offset < total;) {
      size_t count = total - offset < sizeof(actual) ? total - offset : sizeof(actual);
      ssize_t got = pread(target, actual, count, 7 + offset);
      CHECK(got == (ssize_t)count);
      if (got != (ssize_t)count) break;
      for (size_t i = 0; i < count; ++i) pattern[i] = (offset + i) % 251;
      CHECK(memcmp(actual, pattern, count) == 0);
      offset += count;
    }
    struct stat info = {0};
    CHECK(fstat(target, &info) == 0 && info.st_size == (off_t)total + 7);
  }
  if (source >= 0) {
    CHECK(close(source) == 0);
    CHECK(unlinkat(directory, "large-source", 0) == 0);
  }
  if (target >= 0) {
    CHECK(close(target) == 0);
    CHECK(unlinkat(directory, "large-target", 0) == 0);
  }
}

static void CheckSendFile(int source) {
  int pipefd[2];
  int made = pipe(pipefd);
  CHECK(made == 0);
  if (made < 0) return;
  off_t start = 0;
  off_t negative = -1, overflow = INT64_MAX;
  CHECK(sendfile(-1, source, &negative, 2) == -1 && errno == EINVAL && negative == -1);
  CHECK(sendfile(pipefd[1], -1, &negative, 2) == -1 && errno == EBADF && negative == -1);
  CHECK(sendfile(pipefd[1], source, &overflow, 2) == -1 && errno == EINVAL && overflow == INT64_MAX);
  off_t current = lseek(source, 0, SEEK_CUR);
  CHECK(current >= 0);
  ssize_t sent = sendfile(pipefd[1], source, &start, 5);
  CHECK(sent == 5 && start == 5);
  CHECK(lseek(source, 0, SEEK_CUR) == current);
  char bytes[5];
  CHECK(read(pipefd[0], bytes, sizeof(bytes)) == sizeof(bytes) && memcmp(bytes, "hello", 5) == 0);
  CHECK(fcntl(pipefd[1], F_SETPIPE_SZ, 4096) == 4096);
  CHECK(fcntl(pipefd[1], F_SETFL, O_NONBLOCK) == 0);
  start = 4095;
  CHECK(sendfile(pipefd[1], source, &start, 2) == 1 && start == 4096);
  CHECK((write(pipefd[1], "x", 1) == -1) && errno == EAGAIN);
  CHECK(close(pipefd[1]) == 0);
  CHECK(read(pipefd[0], bytes, sizeof(bytes)) == 1);
  CHECK(close(pipefd[0]) == 0);
}

static void CheckTransferWriteback(int source) {
  off64_t *read_only = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(read_only != MAP_FAILED);
  if (read_only == MAP_FAILED) return;
  int endpoints[2];
  int result = pipe2(endpoints, O_NONBLOCK);
  CHECK(result == 0);
  if (result == 0) {
    char data[2];
    CHECK((sendfile(endpoints[1], source, read_only, 0) == -1) && errno == EFAULT);
    CHECK((sendfile(endpoints[1], source, read_only, 2) == -1) && errno == EFAULT);
    CHECK(read(endpoints[0], data, sizeof(data)) == sizeof(data) && memcmp(data, "he", 2) == 0);
    CHECK(splice(-1, NULL, -1, NULL, 0, UINT32_MAX) == 0);
    CHECK((splice(source, read_only, endpoints[1], NULL, 2, 0) == -1) && errno == EFAULT);
    CHECK(read(endpoints[0], data, sizeof(data)) == sizeof(data) && memcmp(data, "he", 2) == 0);
    CHECK(close(endpoints[0]) == 0);
    CHECK(close(endpoints[1]) == 0);
  }
  CHECK(munmap(read_only, 4096) == 0);
}

static volatile sig_atomic_t PipeHandlerSignal;
static void HandlePipeSignal(int number) { PipeHandlerSignal = number; }

static void CheckLockInterruption(int directory, int file, int restart) {
  int ready[2];
  int result = pipe(ready);
  CHECK(result == 0);
  if (result != 0) return;
  CHECK(flock(file, LOCK_EX) == 0);
  pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    close(ready[0]);
    int separate = openat(directory, "file", O_RDWR);
    struct sigaction handler = {.sa_handler = HandlePipeSignal, .sa_flags = restart ? SA_RESTART : 0};
    sigemptyset(&handler.sa_mask);
    if (separate < 0 || sigaction(SIGUSR1, &handler, NULL) != 0 ||
        write(ready[1], "r", 1) != 1) _exit(2);
    int locked = flock(separate, LOCK_EX);
    _exit((restart ? locked == 0 : locked == -1 && errno == EINTR) &&
          PipeHandlerSignal == SIGUSR1 ? 0 : 3);
  }
  CHECK(close(ready[1]) == 0);
  if (child > 0) {
    char byte = 0;
    CHECK(read(ready[0], &byte, 1) == 1 && byte == 'r');
    usleep(100000);
    CHECK(kill(child, SIGUSR1) == 0);
    if (restart) {
      usleep(100000);
      CHECK(flock(file, LOCK_UN) == 0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  CHECK(close(ready[0]) == 0);
  CHECK(flock(file, LOCK_UN) == 0);
}

static void CheckBrokenPipeSignals(void) {
  for (int blocked = 0; blocked <= 1; ++blocked) {
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
      sigset_t set, pending;
      sigemptyset(&set);
      sigaddset(&set, SIGPIPE);
      if (signal(SIGPIPE, SIG_DFL) == SIG_ERR ||
          sigprocmask(blocked ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL) != 0) _exit(1);
      int pipefd[2];
      if (pipe(pipefd) != 0 || close(pipefd[0]) != 0) _exit(2);
      if (write(pipefd[1], "x", 1) != -1 || errno != EPIPE || !blocked) _exit(3);
      if (sigpending(&pending) != 0 || sigismember(&pending, SIGPIPE) != 1) _exit(4);
      sigprocmask(SIG_UNBLOCK, &set, NULL);
      _exit(5);
    }
    if (child > 0) {
      int status = 0;
      CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE);
    }
  }
  struct sigaction handler = {.sa_handler = HandlePipeSignal}, previous;
  sigemptyset(&handler.sa_mask);
  int installed = sigaction(SIGPIPE, &handler, &previous);
  CHECK(installed == 0);
  if (installed == 0) {
    int endpoints[2];
    int result = pipe(endpoints);
    CHECK(result == 0);
    if (result == 0) {
      CHECK(close(endpoints[0]) == 0);
      CHECK((write(endpoints[1], "x", 1) == -1) && errno == EPIPE);
      CHECK(PipeHandlerSignal == SIGPIPE);
      PipeHandlerSignal = 0;
      struct iovec vector = {"x", 1};
      CHECK(syscall(SYS_pwritev2, endpoints[1], &vector, 1, -1L, 0UL, 0x100) == -1 && errno == EPIPE);
      CHECK(PipeHandlerSignal == 0);
      CHECK(close(endpoints[1]) == 0);
    }
    CHECK(sigaction(SIGPIPE, &previous, NULL) == 0);
  }

}

static void CheckPipeControls(void) {
  int endpoints[2];
  int made = pipe(endpoints);
  CHECK(made == 0);
  if (made < 0) return;
  CHECK(fcntl(endpoints[0], F_SETPIPE_SZ, 1) == 4096);
  CHECK(fcntl(endpoints[1], F_GETPIPE_SZ) == 4096);
  char data[4096] = {0};
  struct iovec vector = {data, sizeof(data)};
  CHECK(syscall(SYS_preadv2, endpoints[0], &vector, 1, -1L, 0UL, RWF_NOWAIT) == -1 && errno == EAGAIN);
  CHECK(syscall(SYS_pwritev2, endpoints[1], &vector, 1, -1L, 0UL, RWF_NOWAIT) == sizeof(data));
  CHECK(syscall(SYS_pwritev2, endpoints[1], &vector, 1, -1L, 0UL, RWF_NOWAIT) == -1 && errno == EAGAIN);
  CHECK(syscall(SYS_preadv2, endpoints[0], &vector, 1, -1L, 0UL, RWF_NOWAIT) == sizeof(data));
  int enabled = 1;
  CHECK(ioctl(endpoints[1], FIONBIO, &enabled) == 0);
  CHECK(fcntl(endpoints[1], F_GETFL) & O_NONBLOCK);
  CHECK(ioctl(endpoints[0], FIOCLEX) == 0);
  CHECK(fcntl(endpoints[0], F_GETFD) == FD_CLOEXEC);
  CHECK(ioctl(endpoints[0], FIONCLEX) == 0);
  CHECK(fcntl(endpoints[0], F_GETFD) == 0);
  CHECK(write(endpoints[1], data, sizeof(data)) == sizeof(data));
  int pending = 0;
  CHECK(ioctl(endpoints[0], FIONREAD, &pending) == 0 && pending == sizeof(data));
  CHECK((write(endpoints[1], data, 1) == -1) && errno == EAGAIN);
  struct pollfd watcher = {endpoints[1], POLLOUT, 0};
  CHECK(poll(&watcher, 1, 0) == 0);
  CHECK(read(endpoints[0], data, 1) == 1);
  CHECK((write(endpoints[1], data, 1) == -1) && errno == EAGAIN);
  CHECK(read(endpoints[0], data, sizeof(data)) == sizeof(data) - 1);
  CHECK(poll(&watcher, 1, 0) == 1 && (watcher.revents & POLLOUT));
  CHECK(write(endpoints[1], data, sizeof(data) - 1) == sizeof(data) - 1);
  CHECK((write(endpoints[1], data, 2) == -1) && errno == EAGAIN);
  CHECK(write(endpoints[1], data, 1) == 1);
  char large[4097] = {0};
  CHECK((write(endpoints[1], large, sizeof(large)) == -1) && errno == EAGAIN);
  CHECK(read(endpoints[0], data, sizeof(data)) == sizeof(data));
  CHECK(write(endpoints[1], large, sizeof(large)) == 4096);
  CHECK(read(endpoints[0], data, sizeof(data)) == sizeof(data));
  const int expanded = fcntl(endpoints[0], F_SETPIPE_SZ, 128 * 1024);
  if (expanded >= 128 * 1024) {
    char *block = malloc(128 * 1024);
    CHECK(block != NULL);
    if (block) {
      memset(block, 'v', 128 * 1024);
      struct iovec chunks[] = {{block, 65536}, {block + 65536, 65536}};
      CHECK(writev(endpoints[1], chunks, 2) == 128 * 1024);
      memset(block, 0, 128 * 1024);
      CHECK(readv(endpoints[0], chunks, 2) == 128 * 1024);
      CHECK(block[0] == 'v' && block[128 * 1024 - 1] == 'v');
      free(block);
    }
  } else {
    CHECK(expanded == -1 && errno == EPERM);
  }
  CHECK(close(endpoints[1]) == 0);
  CHECK(close(endpoints[0]) == 0);
}

static void CheckPrivatePipePageAcrossFork(void) {
  char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(page != MAP_FAILED);
  if (page == MAP_FAILED) return;
  memcpy(page, "ab", 2);
  int endpoints[2];
  int result = pipe2(endpoints, O_NONBLOCK);
  CHECK(result == 0);
  if (result == 0) {
    struct iovec part = {page, 2};
    ssize_t queued = vmsplice(endpoints[1], &part, 1, SPLICE_F_NONBLOCK);
    CHECK(queued == 2);
    if (queued == 2) {
      pid_t child = fork();
      CHECK(child >= 0);
      if (child == 0) {
        *(volatile char *)page = 'x';
        char byte = 0;
        _exit(read(endpoints[0], &byte, 1) == 1 && byte == 'a' ? 0 : 1);
      }
      if (child > 0) {
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        char byte = 0;
        // After fork a native write may COW away from the pinned page.
        CHECK(read(endpoints[0], &byte, 1) == 1 && byte == 'b');
      }
    }
    CHECK(close(endpoints[0]) == 0);
    CHECK(close(endpoints[1]) == 0);
  }
  CHECK(munmap(page, 4096) == 0);
}

static void CheckSplice(int source) {
  int incoming[2], outgoing[2];
  int made = pipe(incoming);
  CHECK(made == 0);
  if (made < 0) return;
  made = pipe(outgoing);
  CHECK(made == 0);
  if (made < 0) {
    close(incoming[0]);
    close(incoming[1]);
    return;
  }
  CHECK((tee(incoming[0], outgoing[1], 1, SPLICE_F_NONBLOCK) == -1) && errno == EAGAIN);
  off64_t position = 0;
  CHECK(splice(source, &position, incoming[1], NULL, 5, 0) == 5 && position == 5);
  CHECK(tee(incoming[0], outgoing[1], 3, SPLICE_F_NONBLOCK) == 3);
  CHECK(splice(incoming[0], NULL, outgoing[1], NULL, 5, 0) == 5);
  char data[8] = {0};
  CHECK(read(outgoing[0], data, sizeof(data)) == sizeof(data));
  CHECK(memcmp(data, "helhello", sizeof(data)) == 0);

  CHECK(fcntl(outgoing[1], F_SETPIPE_SZ, 4096) == 4096);
  position = 0;
  CHECK(splice(source, &position, incoming[1], NULL, 1, 0) == 1);
  CHECK(splice(source, &position, incoming[1], NULL, 1, 0) == 1);
  CHECK(tee(incoming[0], outgoing[1], 2, SPLICE_F_NONBLOCK) == 1);
  CHECK(read(outgoing[0], data, 1) == 1 && data[0] == 'h');
  CHECK(read(incoming[0], data, 2) == 2 && memcmp(data, "he", 2) == 0);

  char first[] = "xy", second[] = "z";
  struct iovec parts[] = {{first, 2}, {second, 1}};
  CHECK(vmsplice(incoming[1], parts, 2, 0) == 3);
  CHECK(read(incoming[0], data, 3) == 3 && memcmp(data, "xyz", 3) == 0);
  CHECK(vmsplice(outgoing[1], parts, 2, SPLICE_F_NONBLOCK) == 2);
  CHECK(read(outgoing[0], data, sizeof(data)) == 2 && memcmp(data, "xy", 2) == 0);
  _Alignas(4096) char stack_page[4096] = {'s'};
  struct iovec stack_part = {stack_page, 1};
  CHECK(vmsplice(incoming[1], &stack_part, 1, SPLICE_F_NONBLOCK) == 1);
  stack_page[0] = 't';
  CHECK(read(incoming[0], data, 1) == 1 && data[0] == 't');
  for (int mode = MAP_SHARED; mode <= MAP_PRIVATE; ++mode) {
    char *mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, mode | MAP_ANONYMOUS, -1, 0);
    CHECK(mapping != MAP_FAILED);
    if (mapping != MAP_FAILED) {
      *mapping = 'a';
      struct iovec page = {mapping, 1};
      CHECK(vmsplice(incoming[1], &page, 1, SPLICE_F_NONBLOCK) == 1);
      CHECK(tee(incoming[0], outgoing[1], 1, SPLICE_F_NONBLOCK) == 1);
      *mapping = 'b';
      CHECK(munmap(mapping, 4096) == 0);
      CHECK(read(incoming[0], data, 1) == 1 && data[0] == 'b');
      CHECK(read(outgoing[0], data, 1) == 1 && data[0] == 'b');
    }
  }
  CHECK(close(incoming[0]) == 0);
  CHECK(close(incoming[1]) == 0);
  CHECK(close(outgoing[0]) == 0);
  CHECK(close(outgoing[1]) == 0);
}

static void CheckFilesystem(const char *path, int directory) {
  struct statfs named = {0}, opened = {0};
  CHECK(statfs(path, &named) == 0);
  CHECK(fstatfs(directory, &opened) == 0);
  CHECK(named.f_bsize > 0 && named.f_frsize > 0 && named.f_namelen > 0);
  CHECK(named.f_bavail <= named.f_bfree && named.f_bfree <= named.f_blocks);
  CHECK(named.f_type == opened.f_type && named.f_blocks == opened.f_blocks);
  int dirty = openat(directory, "sync-dirty", O_CREAT | O_EXCL | O_WRONLY, 0600);
  CHECK(dirty >= 0);
  if (dirty >= 0) {
    CHECK(write(dirty, "persist", 7) == 7);
    CHECK(close(dirty) == 0);
    CHECK(renameat(directory, "sync-dirty", directory, "sync-renamed") == 0);
    int reopened = openat(directory, "sync-renamed", O_RDONLY);
    CHECK(reopened >= 0);
    if (reopened >= 0) {
      CHECK(sync_file_range(reopened, 0, 7, SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER) == 0);
      char content[7];
      CHECK(read(reopened, content, sizeof(content)) == sizeof(content) && memcmp(content, "persist", 7) == 0);
      CHECK(fsync(reopened) == 0);
      CHECK(close(reopened) == 0);
    }
    CHECK(unlinkat(directory, "sync-renamed", 0) == 0);
  }
  CHECK(syncfs(directory) == 0);
  CHECK((fstatfs(-1, &opened) == -1) && errno == EBADF);
}

static void CheckAnonymousFilesystem(int fd, long type) {
  struct statfs info = {0};
  CHECK(fd >= 0);
  if (fd < 0) return;
  CHECK(fstatfs(fd, &info) == 0 && info.f_type == type);
  CHECK(info.f_bsize > 0 && info.f_frsize > 0 && info.f_namelen > 0);
  CHECK(syncfs(fd) == 0);
  CHECK(close(fd) == 0);
}

static void CheckVirtualFilesystems(void) {
  CHECK(truncate("/proc/../dev/null", 0) == -1 && errno == EINVAL);
  CHECK(truncate("/proc/../dev/null/", 0) == -1 && errno == ENOTDIR);
  CHECK(truncate("/proc/self", 0) == -1 && errno == EISDIR);
  CHECK(truncate("/dev/../proc/version", 0) == -1 && errno == EACCES);
  struct statfs named = {0}, alias = {0};
  CHECK(statfs("/proc/self/status", &named) == 0 && named.f_type == PROC_SUPER_MAGIC);
  CHECK(statfs("/dev/../proc/self/status", &alias) == 0 && alias.f_type == named.f_type);
  CheckAnonymousFilesystem(open("/proc/self/status", O_RDONLY), PROC_SUPER_MAGIC);
  char target[32], pid[32];
  const int pid_bytes = snprintf(pid, sizeof(pid), "%ld", (long)getpid());
  CHECK(readlink("/proc/self", target, sizeof(target)) == pid_bytes && memcmp(target, pid, pid_bytes) == 0);
  CHECK(readlink("/proc/mounts", target, sizeof(target)) == 11 && memcmp(target, "self/mounts", 11) == 0);
  struct stat link = {0}, followed = {0};
  CHECK(lstat("/proc/self", &link) == 0 && S_ISLNK(link.st_mode));
  CHECK(stat("/proc/self", &followed) == 0 && S_ISDIR(followed.st_mode) && followed.st_ino != link.st_ino);
  CHECK(open("/proc/self", O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP);
  int reference = open("/proc/self", O_PATH | O_NOFOLLOW);
  CHECK(reference >= 0);
  if (reference >= 0) {
    struct stat retained = {0};
    CHECK(fstat(reference, &retained) == 0 && S_ISLNK(retained.st_mode) && retained.st_ino == link.st_ino);
    CHECK(readlinkat(reference, "", target, sizeof(target)) == pid_bytes && memcmp(target, pid, pid_bytes) == 0);
    CHECK(close(reference) == 0);
  }
  DIR *proc = opendir("/proc");
  CHECK(proc != NULL);
  if (proc) {
    int directory = dirfd(proc);
    struct stat root = {0}, opened = {0};
    CHECK(stat("/proc", &root) == 0);
    CHECK(fstat(directory, &opened) == 0 && S_ISDIR(opened.st_mode) &&
          opened.st_dev == root.st_dev && opened.st_ino == root.st_ino);
    CHECK(fstatfs(directory, &alias) == 0 && alias.f_type == PROC_SUPER_MAGIC);
    CHECK(sync_file_range(directory, 0, 0, SYNC_FILE_RANGE_WRITE) == 0);
    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(proc)) != NULL) {
      if (strcmp(entry->d_name, "version") == 0) found = 1;
    }
    CHECK(found);
    int status = openat(directory, "self/status", O_RDONLY);
    CHECK(status >= 0);
    if (status >= 0) {
      CHECK(sync_file_range(status, 0, 0, SYNC_FILE_RANGE_WRITE) == 0);
      CHECK(sync_file_range(status, 0, 0, 8) == -1 && errno == EINVAL);
      CheckAnonymousFilesystem(status, PROC_SUPER_MAGIC);
    }
    uint64_t how[] = {O_RDONLY, 0, 1};
    CheckAnonymousFilesystem(syscall(SYS_openat2, directory, "version", how, sizeof(how)), PROC_SUPER_MAGIC);
    CHECK(syscall(SYS_openat2, directory, "../dev/null", how, sizeof(how)) == -1 && errno == EXDEV);
    CHECK(closedir(proc) == 0);
  }
  CHECK(statfs("/dev/null", &named) == 0);
  CHECK(statfs("/proc/../dev/null", &alias) == 0 && alias.f_type == named.f_type);
  int path = open("/dev/null", O_PATH);
  CHECK(path >= 0);
  if (path >= 0) {
    struct stat by_path = {0}, by_fd = {0}, root = {0};
    CHECK(stat("/proc/../dev/null", &by_path) == 0);
    CHECK(fstat(path, &by_fd) == 0 && by_fd.st_ino == by_path.st_ino && by_fd.st_dev == by_path.st_dev);
    CHECK(stat("/", &root) == 0 && root.st_dev != by_path.st_dev);
    struct statfs opened = {0};
    CHECK(fstatfs(path, &opened) == 0 && opened.f_type == named.f_type);
    CHECK((syncfs(path) == -1) && errno == EBADF);
    CHECK(close(path) == 0);
  }
  CheckAnonymousFilesystem(memfd_create("fs-probe", MFD_CLOEXEC), TMPFS_MAGIC);
  int pair[2];
  int result = pipe(pair);
  CHECK(result == 0);
  if (result == 0) {
    CheckAnonymousFilesystem(pair[0], PIPEFS_MAGIC);
    CheckAnonymousFilesystem(pair[1], PIPEFS_MAGIC);
  }
  result = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
  CHECK(result == 0);
  if (result == 0) {
    CheckAnonymousFilesystem(pair[0], SOCKFS_MAGIC);
    CheckAnonymousFilesystem(pair[1], SOCKFS_MAGIC);
  }
}

static void CheckOpenStatusFlags(int directory) {
  struct stat proc = {0};
  CHECK(stat("/proc/version", &proc) == 0);
  int version = open("/proc/version", O_RDONLY);
  CHECK(version >= 0);
  if (version >= 0) {
    if (geteuid() && proc.st_uid != geteuid()) {
      CHECK(open("/proc/version", O_RDONLY | O_NOATIME) == -1 && errno == EPERM);
      CHECK(fcntl(version, F_SETFL, O_NOATIME | O_NONBLOCK) == -1 && errno == EPERM);
      CHECK((fcntl(version, F_GETFL) & (O_NOATIME | O_NONBLOCK)) == 0);
    } else {
      CHECK(fcntl(version, F_SETFL, O_NOATIME) == 0);
      CHECK(fcntl(version, F_GETFL) & O_NOATIME);
    }
    CHECK(close(version) == 0);
  }
  int file = openat(directory, "sync-flags", O_CREAT | O_EXCL | O_RDWR | O_SYNC | O_APPEND | O_NOATIME, 0600);
  CHECK(file >= 0);
  if (file < 0) return;
  CHECK((fcntl(file, F_GETFL) & (O_SYNC | O_APPEND | O_NOATIME)) == (O_SYNC | O_APPEND | O_NOATIME));
  CHECK(write(file, "abc", 3) == 3);
  struct iovec vec = {"z", 1};
  CHECK(syscall(SYS_pwritev2, file, &vec, 1, 0UL, 0UL, 0x20) == 1); // RWF_NOAPPEND
  char data[3];
  CHECK(pread(file, data, sizeof(data), 0) == sizeof(data) && memcmp(data, "zbc", 3) == 0);
  const struct timespec times[] = {{111, 22}, {333, 44}};
  CHECK(futimens(file, times) == 0);
  CHECK(pread(file, data, sizeof(data), 0) == sizeof(data));
  struct stat info = {0};
  CHECK(fstat(file, &info) == 0 && info.st_atim.tv_sec == 111 && info.st_atim.tv_nsec == 22);
  CHECK(close(file) == 0);
  CHECK(unlinkat(directory, "sync-flags", 0) == 0);
}

static void CheckTemporary(int directory) {
  struct stat unnamed = {0}, named = {0};
  CHECK(openat(directory, ".", O_TMPFILE | O_RDONLY, 0600) == -1 && errno == EINVAL);
  CHECK(syscall(SYS_openat, directory, NULL, O_TMPFILE | O_RDONLY, 0600) == -1 && errno == EINVAL);
  CHECK(syscall(SYS_openat, directory, NULL, O_TMPFILE | O_RDWR, 0600) == -1 && errno == EFAULT);
  CHECK(openat(directory, "", O_TMPFILE | O_RDONLY, 0600) == -1 && errno == EINVAL);
  CHECK(openat(directory, ".", (O_TMPFILE & ~O_DIRECTORY) | O_RDWR, 0600) == -1 && errno == EINVAL);
  CHECK(openat(directory, ".", O_TMPFILE | O_RDWR | O_CREAT, 0600) == -1 && errno == EINVAL);
  CHECK(open("/proc/version", O_TMPFILE | O_RDWR, 0600) == -1 && errno == ENOTDIR);
  CHECK(open("/proc/self", O_TMPFILE | O_RDWR | O_NOFOLLOW, 0600) == -1 && errno == ENOTDIR);
  CHECK(open("/dev/null", O_TMPFILE | O_RDWR, 0600) == -1 && errno == ENOTDIR);
  CHECK(symlinkat(".", directory, "tmp-directory-link") == 0);
  CHECK(openat(directory, "tmp-directory-link", O_TMPFILE | O_RDWR | O_NOFOLLOW, 0600) == -1 && errno == ENOTDIR);
  int followed = openat(directory, "tmp-directory-link/", O_TMPFILE | O_RDWR | O_NOFOLLOW, 0600);
  CHECK(followed >= 0);
  if (followed >= 0) CHECK(close(followed) == 0);
  CHECK(unlinkat(directory, "tmp-directory-link", 0) == 0);
  int temporary = syscall(SYS_openat, (uint64_t)directory | (1ULL << 32), ".",
                          (1ULL << 32) | O_TMPFILE | O_RDWR | O_CLOEXEC | O_TRUNC | O_NOFOLLOW, 0600);
  CHECK(temporary >= 0);
  if (temporary < 0) return;
  CHECK(fstat(temporary, &unnamed) == 0 && unnamed.st_nlink == 0);
  CHECK(fcntl(temporary, F_GETFD) == FD_CLOEXEC);
  CHECK((fcntl(temporary, F_GETFL) & (O_TMPFILE | O_NOFOLLOW | O_TRUNC | O_ACCMODE)) ==
        (O_TMPFILE | O_NOFOLLOW | O_RDWR));
  CHECK(write(temporary, "tmp", 3) == 3);
  CHECK(fallocate(temporary, FALLOC_FL_KEEP_SIZE, 4096, 4096) == 0);
  CHECK(fsync(directory) == 0);
  CHECK(fsync(directory) == 0);
  char preserved[3];
  CHECK(pread(temporary, preserved, sizeof(preserved), 0) == sizeof(preserved) && memcmp(preserved, "tmp", 3) == 0);
  CHECK(fstat(temporary, &unnamed) == 0 && unnamed.st_size == 3 && unnamed.st_blocks >= 8);
  CHECK(linkat(temporary, "", directory, "published", AT_EMPTY_PATH) == 0);
  CHECK(fstatat(directory, "published", &named, 0) == 0 && named.st_ino == unnamed.st_ino &&
        named.st_nlink == 1 && named.st_size == 3 && named.st_blocks == unnamed.st_blocks);
  CHECK(linkat(temporary, "", directory, "second-publication", AT_EMPTY_PATH) == 0);
  CHECK(fstatat(directory, "second-publication", &named, 0) == 0 &&
        named.st_ino == unnamed.st_ino && named.st_nlink == 2);
  CHECK(unlinkat(directory, "published", 0) == 0);
  CHECK(unlinkat(directory, "second-publication", 0) == 0);
  CHECK(linkat(temporary, "", directory, "republication", AT_EMPTY_PATH) == -1 && errno == ENOENT);
  CHECK(close(temporary) == 0);

  int exclusive = openat(directory, ".", O_TMPFILE | O_WRONLY | O_EXCL, 0600);
  CHECK(exclusive >= 0);
  if (exclusive >= 0) {
    CHECK((linkat(exclusive, "", directory, "exclusive", AT_EMPTY_PATH) == -1) && errno == ENOENT);
    CHECK(close(exclusive) == 0);
  }
}

static void CheckNodes(int directory) {
  struct stat info = {0};
  CHECK(mknodat(directory, "node", S_IFREG | 0600, 0) == 0);
  CHECK(fstatat(directory, "node", &info, 0) == 0 && S_ISREG(info.st_mode));
  CHECK(unlinkat(directory, "node", 0) == 0);
  CHECK(mkfifoat(directory, "fifo", 0600) == 0);
  struct stat trailing = {0};
  CHECK((fstatat(directory, "fifo/", &trailing, 0) == -1) && errno == ENOTDIR);
  CHECK((linkat(directory, "fifo/", directory, "invalid-fifo-link", 0) == -1) && errno == ENOTDIR);
  CHECK((openat(directory, "fifo/", O_CREAT | O_RDONLY, 0600) == -1) && errno == EISDIR);
  CHECK((openat(directory, "missing/", O_CREAT | O_EXCL | O_RDONLY, 0600) == -1) && errno == EISDIR);

  CHECK(fstatat(directory, "fifo", &info, 0) == 0 && S_ISFIFO(info.st_mode));
  int cwd = open(".", O_RDONLY | O_DIRECTORY);
  CHECK(cwd >= 0);
  if (cwd >= 0) {
    int changed = fchdir(directory);
    CHECK(changed == 0);
    if (changed == 0) {
      CHECK((truncate("fifo", 0) == -1) && errno == EINVAL);
      CHECK((truncate("fifo/", 0) == -1) && errno == ENOTDIR);
      CHECK(fchdir(cwd) == 0);
    }
    CHECK(close(cwd) == 0);
  }
  const struct timespec stamps[] = {{100, 1}, {200, 2}};
  CHECK((syscall(SYS_utime, NULL, NULL) == -1) && errno == EFAULT);
  CHECK((syscall(SYS_utimes, NULL, NULL) == -1) && errno == EFAULT);
  CHECK(utimensat(directory, "fifo", stamps, 0) == 0);
  int path = openat(directory, "fifo", O_PATH);
  CHECK(path >= 0);
  if (path >= 0) {
    CHECK(fstat(path, &info) == 0 && info.st_mtim.tv_sec == 200 && info.st_mtim.tv_nsec == 2);
    CHECK((syscall(SYS_utimensat, path, NULL, stamps, 0) == -1) && errno == EBADF);
    CHECK(utimensat(path, "", stamps, AT_EMPTY_PATH) == 0);
    CHECK(close(path) == 0);
  }
  CHECK((openat(directory, "fifo", O_WRONLY | O_NONBLOCK) == -1) && errno == ENXIO);
  int reader = openat(directory, "fifo", O_RDONLY | O_NONBLOCK);
  CHECK(reader >= 0);
  if (reader >= 0) {
    struct pollfd watched = {reader, POLLIN, 0};
    CHECK(poll(&watched, 1, 0) == 0);
    int writer = openat(directory, "fifo", O_WRONLY | O_NONBLOCK);
    CHECK(writer >= 0);
    if (writer >= 0) {
      char byte = 0;
      CHECK((read(reader, &byte, 1) == -1) && errno == EAGAIN);
      CHECK(write(writer, "r", 1) == 1);
      CHECK(close(reader) == 0);
      reader = openat(directory, "fifo", O_RDONLY | O_NONBLOCK);
      CHECK(reader >= 0);
      if (reader >= 0) CHECK(read(reader, &byte, 1) == 1 && byte == 'r');
      CHECK(unlinkat(directory, "fifo", 0) == 0);
      CHECK(write(writer, "x", 1) == 1);
      CHECK(read(reader, &byte, 1) == 1 && byte == 'x');
      CHECK(close(writer) == 0);
      CHECK(read(reader, &byte, 1) == 0);
      watched.fd = reader;
      CHECK(poll(&watched, 1, 0) == 1 && (watched.revents & POLLHUP));
    }
    if (reader >= 0) CHECK(close(reader) == 0);
  }
}

static void CheckReadAdvice(int directory, int file) {
  int pair[2];
  int opened = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
  CHECK(opened == 0);
  if (opened == 0) {
    CHECK(posix_fadvise(pair[0], 0, 1, POSIX_FADV_NORMAL) == 0);
    CHECK(posix_fadvise(pair[0], 0, 1, 99) == EINVAL);
    CHECK(close(pair[0]) == 0);
    CHECK(close(pair[1]) == 0);
  }
  opened = pipe(pair);
  CHECK(opened == 0);
  if (opened == 0) {
    CHECK(posix_fadvise(pair[0], -1, 1, 99) == ESPIPE);
    CHECK(readahead(pair[0], 0, 1) == -1 && errno == EINVAL);
    CHECK(close(pair[0]) == 0);
    CHECK(close(pair[1]) == 0);
  }
  int proc = open("/proc/version", O_RDONLY);
  CHECK(proc >= 0);
  if (proc >= 0) {
    CHECK(readahead(proc, 0, 32) == 0);
    CHECK(readahead(proc, -1, 32) == -1 && errno == EINVAL);
    CHECK(lseek(proc, 0, SEEK_CUR) == 0);
    CHECK(close(proc) == 0);
  }
  int alias = openat(directory, "file", O_RDONLY);
  CHECK(alias >= 0);
  if (alias < 0) return;
  CHECK(posix_fadvise(alias, 0, 32, POSIX_FADV_SEQUENTIAL) == 0);
  CHECK(posix_fadvise(alias, 0, 32, POSIX_FADV_WILLNEED) == 0);
  CHECK(readahead(alias, 0, 32) == 0);
  CHECK(lseek(alias, 0, SEEK_CUR) == 0);
  char byte;
  CHECK(pread(alias, &byte, 1, 0) == 1 && byte == 'h');
  CHECK(pwrite(file, "H", 1, 0) == 1);
  CHECK(pread(alias, &byte, 1, 0) == 1 && byte == 'H');
  CHECK(pwrite(file, "h", 1, 0) == 1);
  CHECK(posix_fadvise(alias, 0, 32, POSIX_FADV_DONTNEED) == 0);
  CHECK(posix_fadvise(alias, 0, 32, 99) == EINVAL);
  CHECK(close(alias) == 0);
}

static void CheckDirents(int directory, int file, int wide) {
  unsigned char bytes[512];
  struct stat info = {0};
  CHECK(fstat(file, &info) == 0);
  CHECK(lseek(directory, 0, SEEK_SET) == 0);
  long length = syscall(wide ? SYS_getdents64 : SYS_getdents, directory, bytes, sizeof(bytes));
  CHECK(length > 0);
  int found = 0;
  for (long offset = 0; offset + 20 <= length;) {
    uint16_t record;
    memcpy(&record, bytes + offset + 16, sizeof(record));
    CHECK(record >= 20 && record <= length - offset);
    if (record < 20 || record > length - offset) break;
    const size_t name_offset = wide ? 19 : 18;
    const size_t name_capacity = record - 19;
    const char *name = (const char *)bytes + offset + name_offset;
    if (memchr(name, 0, name_capacity) && strcmp(name, "file") == 0) {
      uint64_t inode;
      memcpy(&inode, bytes + offset, sizeof(inode));
      CHECK(inode == info.st_ino);
      ++found;
    }
    offset += record;
  }
  CHECK(found == 1);
  if (length < 20) return;
  const long number = wide ? SYS_getdents64 : SYS_getdents;
  uint16_t first_size;
  int64_t first_cookie;
  memcpy(&first_size, bytes + 16, sizeof(first_size));
  memcpy(&first_cookie, bytes + 8, sizeof(first_cookie));
  if (first_size < 20 || first_size >= length) return;
  unsigned char partial[512] = {0};
  CHECK(lseek(directory, 0, SEEK_SET) == 0);
  CHECK((syscall(number, directory, partial, (1ULL << 32) | 1) == -1) && errno == EINVAL);
  CHECK(lseek(directory, 0, SEEK_SET) == 0);
  CHECK(syscall(number, directory, partial, (unsigned long)first_size) == first_size);
  int64_t cookie;
  memcpy(&cookie, partial + 8, sizeof(cookie));
  CHECK(cookie == first_cookie && lseek(directory, 0, SEEK_CUR) == cookie);
  CHECK(lseek(directory, cookie, SEEK_SET) == cookie);
  CHECK(syscall(number, directory, partial, sizeof(partial)) == length - first_size);
  CHECK(syscall(number, directory, partial, sizeof(partial)) == 0);

}

static void CheckLocks(int directory, int file) {
  int separate = openat(directory, "file", O_RDONLY);
  int duplicate = dup(file);
  CHECK(separate >= 0 && duplicate >= 0);
  if (separate >= 0 && duplicate >= 0) {
    CHECK(flock(file, LOCK_SH) == 0);
    CHECK(flock(separate, LOCK_SH | LOCK_NB) == 0);
    CHECK((flock(duplicate, LOCK_EX | LOCK_NB) == -1) && errno == EWOULDBLOCK);
    CHECK(flock(separate, LOCK_UN) == 0);
    CHECK(flock(duplicate, LOCK_EX | LOCK_NB) == 0);
    CHECK(close(duplicate) == 0);
    duplicate = -1;
    CHECK((flock(separate, LOCK_SH | LOCK_NB) == -1) && errno == EWOULDBLOCK);
    CHECK(flock(file, LOCK_UN) == 0);
    CHECK(flock(separate, LOCK_EX | LOCK_NB) == 0);
  }
  if (duplicate >= 0) CHECK(close(duplicate) == 0);
  if (separate >= 0) CHECK(close(separate) == 0);
}

static void CheckRecordLocks(int directory, int file) {
  int separate = openat(directory, "file", O_RDWR);
  CHECK(separate >= 0);
  if (separate < 0) return;
  struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 10, .l_len = 10};
  CHECK(fcntl(file, F_OFD_SETLK, &lock) == 0);
  struct flock query = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 11, .l_len = 1};
  CHECK(fcntl(separate, F_OFD_GETLK, &query) == 0);
  CHECK(query.l_type == F_WRLCK && query.l_start == 10 && query.l_len == 10 && query.l_pid == -1);
  struct flock unlock = {.l_type = F_UNLCK, .l_whence = SEEK_SET, .l_start = 13, .l_len = 2};
  CHECK(fcntl(file, F_OFD_SETLK, &unlock) == 0);
  query = (struct flock){.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 13, .l_len = 2};
  CHECK(fcntl(separate, F_OFD_GETLK, &query) == 0 && query.l_type == F_UNLCK);
  unlock = (struct flock){.l_type = F_UNLCK, .l_whence = SEEK_SET};
  CHECK(fcntl(file, F_OFD_SETLK, &unlock) == 0);

  CHECK(fcntl(file, F_SETLK, &lock) == 0);
  query = (struct flock){.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 11, .l_len = 1};
  CHECK(fcntl(separate, F_OFD_GETLK, &query) == 0 && query.l_pid == getpid() && query.l_type == F_WRLCK);
  int duplicate = dup(file);
  CHECK(duplicate >= 0);
  if (duplicate >= 0) CHECK(close(duplicate) == 0);
  query = (struct flock){.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 11, .l_len = 1};
  CHECK(fcntl(separate, F_OFD_GETLK, &query) == 0 && query.l_type == F_UNLCK);
  CHECK(close(separate) == 0);

  int endpoints[2];
  int made = pipe(endpoints);
  CHECK(made == 0);
  if (made == 0) {
    lock = (struct flock){.l_type = F_RDLCK, .l_whence = SEEK_SET, .l_len = 1};
    CHECK(fcntl(endpoints[0], F_SETLK, &lock) == 0);
    query = (struct flock){.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_len = 1};
    CHECK(fcntl(endpoints[1], F_OFD_GETLK, &query) == 0 && query.l_type == F_RDLCK && query.l_pid == getpid());
    CHECK(close(endpoints[0]) == 0);
    query = (struct flock){.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_len = 1};
    CHECK(fcntl(endpoints[1], F_OFD_GETLK, &query) == 0 && query.l_type == F_UNLCK);
    CHECK(close(endpoints[1]) == 0);
  }
}

static void CheckRecordLockDeadlock(int file) {
  struct flock first = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_len = 1};
  struct flock second = first;
  second.l_start = 1;
  int ready[2];
  int result = pipe(ready);
  CHECK(result == 0);
  if (result != 0) return;
  result = fcntl(file, F_SETLK, &first);
  CHECK(result == 0);
  if (result == 0) {
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
      close(ready[0]);
      if (fcntl(file, F_SETLK, &second) != 0 || write(ready[1], "r", 1) != 1) _exit(2);
      close(ready[1]);
      // Let the parent reach its blocking request first. Either ordering
      // must produce one EDEADLK and one successful acquisition.
      usleep(100000);
      if (fcntl(file, F_SETLKW, &first) == 0) _exit(0);
      _exit(errno == EDEADLK ? 1 : 2);
    }
    close(ready[1]);
    if (child > 0) {
      char byte;
      result = read(ready[0], &byte, 1);
      CHECK(result == 1 && byte == 'r');
      int deadlocked = -1;
      if (result == 1) {
        result = fcntl(file, F_SETLKW, &second);
        deadlocked = result == 0 ? 0 : errno == EDEADLK ? 1 : -1;
      }
      first.l_type = F_UNLCK;
      first.l_len = 0;
      CHECK(fcntl(file, F_SETLK, &first) == 0);
      int status = 0;
      CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status));
      CHECK(deadlocked >= 0 && WEXITSTATUS(status) == 1 - deadlocked);
    } else {
      first.l_type = F_UNLCK;
      CHECK(fcntl(file, F_SETLK, &first) == 0);
    }
  } else {
    close(ready[1]);
  }
  close(ready[0]);
}

static void CheckAcl(int directory) {
  static const char access[] = "system.posix_acl_access";
  static const char defaults[] = "system.posix_acl_default";
  struct Entry { uint16_t tag, permissions; uint32_t id; };
  struct Acl { uint32_t version; struct Entry entries[5]; } acl = {
    2, {{1, 7, UINT32_MAX}, {2, 4, 0}, {4, 5, UINT32_MAX},
        {16, 5, UINT32_MAX}, {32, 1, UINT32_MAX}}
  };
  acl.entries[1].id = geteuid();
  CHECK(fsetxattr(directory, defaults, &acl, sizeof(acl), 0) == 0);
  CHECK(fsetxattr(directory, defaults, &acl, sizeof(acl), XATTR_CREATE) == 0);
  const mode_t mask = umask(0077);
  int file = openat(directory, "acl-child", O_CREAT | O_EXCL | O_RDWR, 0666);
  umask(mask);
  CHECK(file >= 0);
  CHECK(fremovexattr(directory, defaults) == 0);
  if (file >= 0) {
    struct stat info = {0};
    struct Acl inherited = {0};
    CHECK(fstat(file, &info) == 0 && (info.st_mode & 0777) == 0640);
    CHECK(fgetxattr(file, access, &inherited, sizeof(inherited)) == sizeof(inherited));
    CHECK(inherited.entries[1].permissions == 4 && inherited.entries[3].permissions == 4);
    CHECK(fchmod(file, 0600) == 0);
    CHECK(fgetxattr(file, access, &inherited, sizeof(inherited)) == sizeof(inherited));
    CHECK(inherited.entries[1].permissions == 4 && inherited.entries[3].permissions == 0);
    inherited.entries[3].permissions = 8;
    CHECK((fsetxattr(file, access, &inherited, sizeof(inherited), 0) == -1) && errno == EINVAL);
    CHECK((fsetxattr(file, defaults, &acl, sizeof(acl), 0) == -1) && errno == EACCES);
    CHECK(fsetxattr(file, defaults, NULL, 0, 0) == 0);
    CHECK(fsetxattr(file, access, NULL, 0, XATTR_REPLACE) == 0);
    CHECK((fgetxattr(file, access, NULL, 0) == -1) && errno == ENODATA);
    CHECK(fstat(file, &info) == 0 && (info.st_mode & 0777) == 0600);
    CHECK(fremovexattr(file, access) == 0);
    CHECK(fsetxattr(file, access, &acl, sizeof(acl), XATTR_REPLACE) == 0);
    CHECK(fsetxattr(file, access, &acl.version, sizeof(acl.version), 0) == 0);
    CHECK((fgetxattr(file, access, NULL, 0) == -1) && errno == ENODATA);
    uint32_t unsupported = 3;
    CHECK((fsetxattr(file, access, &unsupported, sizeof(unsupported), 0) == -1) && errno == EOPNOTSUPP);
    int path = openat(directory, "acl-child", O_PATH | O_CLOEXEC);
    CHECK(path >= 0);
    if (path >= 0) {
      CHECK((fgetxattr(path, access, NULL, 0) == -1) && errno == EBADF);
      CHECK((flistxattr(path, NULL, 0) == -1) && errno == EBADF);
      CHECK(close(path) == 0);
    }
    CHECK(close(file) == 0);
    CHECK(unlinkat(directory, "acl-child", 0) == 0);
  }
}

static void CheckMetadata(int directory, int file) {
  struct stat info = {0};
  struct timespec times[] = {{123456789, 111}, {123456790, 222}};
  CHECK(fchmod(file, 0640) == 0);
  CHECK(fchown(file, geteuid(), getegid()) == 0);
  CHECK(fchownat(directory, "file", (uid_t)-1, (gid_t)-1, 0) == 0);
  if (geteuid() != 0) CHECK((fchown(file, 0, (gid_t)-1) == -1) && errno == EPERM);
  CHECK(futimens(file, times) == 0);
  CHECK(fstat(file, &info) == 0);
  CHECK((info.st_mode & 0777) == 0640);
  CHECK(fchmodat(directory, "file", 0400, 0) == 0);
  CHECK(faccessat(directory, "file", R_OK, 0) == 0);
  if (geteuid() != 0) {
    CHECK((faccessat(directory, "file", W_OK, 0) == -1) && errno == EACCES);
    CHECK((openat(directory, "file", O_WRONLY) == -1) && errno == EACCES);
  }
  CHECK(utimensat(directory, "file", NULL, 0) == 0);
  CHECK(utimensat(directory, "missing-timestamp",
                  (struct timespec[]){{0, UTIME_OMIT}, {0, UTIME_OMIT}}, 0) == 0);
  CHECK(syscall(SYS_utimensat, -1, NULL,
                (struct timespec[]){{0, UTIME_OMIT}, {0, UTIME_OMIT}}, 8U) == 0);
  CHECK(syscall(SYS_utimensat, directory, "file", (void *)1, 8U) == -1 && errno == EFAULT);
  CHECK(syscall(SYS_utimensat, directory, "file", times, 1ULL << 32) == 0);
  struct stat foreign = {0};
  CHECK(stat("/proc/version", &foreign) == 0);
  if (geteuid() && foreign.st_uid != geteuid()) {
    CHECK(utimensat(AT_FDCWD, "/proc/version", NULL, 0) == -1 && errno == EACCES);
    CHECK(utimensat(AT_FDCWD, "/proc/version", times, 0) == -1 && errno == EPERM);
  }
  CHECK(fchmod(file, 0640) == 0);
  CHECK(futimens(file, times) == 0);
  CHECK(info.st_mtim.tv_sec == times[1].tv_sec && info.st_mtim.tv_nsec == times[1].tv_nsec);
  CHECK(utimensat(directory, "file", (struct timespec[]){{1, UTIME_OMIT}, {987654321, 456}}, 0) == 0);
  struct statx details = {0};
  CHECK(statx(directory, "file", 0, STATX_BASIC_STATS, &details) == 0);
  CHECK((details.stx_mask & (STATX_ATIME | STATX_MTIME)) == (STATX_ATIME | STATX_MTIME));
  CHECK(details.stx_atime.tv_sec == times[0].tv_sec && details.stx_atime.tv_nsec == times[0].tv_nsec);
  CHECK(details.stx_mtime.tv_sec == 987654321 && details.stx_mtime.tv_nsec == 456);
  struct stat null_path = {0};
  CHECK(syscall(SYS_newfstatat, file, NULL, &null_path, AT_EMPTY_PATH) == 0 && null_path.st_ino == details.stx_ino);
  CHECK(syscall(SYS_statx, file, NULL, AT_EMPTY_PATH, STATX_BASIC_STATS, &details) == 0);
  CHECK(details.stx_ino == info.st_ino);
  CHECK((syscall(SYS_statx, directory, "missing", 0x6000, STATX_BASIC_STATS, &details) == -1) && errno == EINVAL);
  CHECK((syscall(SYS_statx, directory, "missing", 0, STATX_BASIC_STATS, NULL) == -1) && errno == ENOENT);
  int null_device = open("/dev/null", O_PATH);
  CHECK(null_device >= 0);
  if (null_device >= 0) {
    CHECK(fstat(null_device, &null_path) == 0 && S_ISCHR(null_path.st_mode));
    CHECK(major(null_path.st_rdev) == 1 && minor(null_path.st_rdev) == 3);
    CHECK(statx(null_device, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &details) == 0);
    CHECK(details.stx_rdev_major == 1 && details.stx_rdev_minor == 3);
    CHECK(close(null_device) == 0);
  }
  CHECK((utimensat(directory, "file", (struct timespec[]){{0, 1000000000}, {0, 0}}, 0) == -1) && errno == EINVAL);
  if (geteuid() != 0) {
    CHECK((fsetxattr(file, "trusted.fex-probe", "x", 1, 0) == -1) && errno == EPERM);
    CHECK((fgetxattr(file, "trusted.fex-probe", NULL, 0) == -1) && errno == ENODATA);
    CHECK(fremovexattr(file, "trusted.fex-probe") == -1 && errno == EPERM);
  }
  CHECK(getxattr("/proc/version", "user.fex-probe", NULL, 0) == -1 && errno == EOPNOTSUPP);
  CHECK(getxattr("/proc/version", "system.posix_acl_access", NULL, 0) == -1 && errno == EOPNOTSUPP);
  if (geteuid() != 0) {
    CHECK(getxattr("/proc/version", "trusted.fex-probe", NULL, 0) == -1 && errno == ENODATA);
    CHECK(removexattr("/proc/version", "user.fex-probe") == -1 && errno == EACCES);
  }
  CHECK((fgetxattr(file, "system.fex-unknown", NULL, 0) == -1) && errno == EOPNOTSUPP);
  CHECK((fgetxattr(file, "user.", NULL, 0) == -1) && errno == EINVAL);
  CHECK(fgetxattr(file, "security.", NULL, 0) == -1 && errno == EINVAL);
  CHECK(fgetxattr(-1, "", NULL, 0) == -1 && errno == ERANGE);
  CHECK(fsetxattr(file, "user.fex-probe", "abc", 3, XATTR_CREATE) == 0);
  CHECK((fsetxattr(file, "user.fex-probe", "abc", 3, XATTR_CREATE) == -1) && errno == EEXIST);
  CHECK(fsetxattr(file, "user.fex-probe", "abc", 3, XATTR_CREATE | XATTR_REPLACE) == -1 && errno == EEXIST);
  CHECK(fsetxattr(file, "user.fex-absent", "abc", 3, XATTR_CREATE | XATTR_REPLACE) == -1 && errno == ENODATA);
  CHECK(syscall(SYS_fsetxattr, file, "user.fex-probe", 1UL, 3UL, XATTR_CREATE) == -1 && errno == EFAULT);
  char value[16] = {0};
  CHECK(fgetxattr(file, "user.fex-probe", value, sizeof(value)) == 3 && memcmp(value, "abc", 3) == 0);
  const char attr[] = "user.fex-probe";
  CHECK(flistxattr(file, value, sizeof(value)) == (ssize_t)sizeof(attr) && memcmp(value, attr, sizeof(attr)) == 0);
  CHECK(getxattr("/dev/null", "user.fex-probe", NULL, 0) == -1);
  CHECK((fgetxattr(file, "user.fex-probe", value, 2) == -1) && errno == ERANGE);
  CHECK(fgetxattr(file, "user.fex-probe", NULL, 0) == 3);

  // Numbered directly to keep building with Debian 12's headers and libc.
  enum { SetXattrAt = 463, GetXattrAt, ListXattrAt, RemoveXattrAt };
  struct XattrArgs { uint64_t value; uint32_t size, flags; };
  struct XattrArgs args = {(uintptr_t)"xy", 2, XATTR_REPLACE};
  CHECK(syscall(SetXattrAt, directory, "file", 0, attr, &args, sizeof(args)) == 0);
  CHECK(fgetxattr(file, attr, value, sizeof(value)) == 2 && memcmp(value, "xy", 2) == 0);
  args = (struct XattrArgs){(uintptr_t)value, sizeof(value), 0};
  CHECK(syscall(GetXattrAt, file, "", AT_EMPTY_PATH, attr, &args, sizeof(args)) == 2);
  int path_fd = openat(directory, "file", O_PATH);
  CHECK(path_fd >= 0);
  if (path_fd >= 0) {
    CHECK(syscall(GetXattrAt, path_fd, "", AT_EMPTY_PATH, attr, &args, sizeof(args)) == -1 && errno == EBADF);
    CHECK(close(path_fd) == 0);
  }
  CHECK(syscall(ListXattrAt, directory, "file", 0, value, sizeof(value)) == sizeof(attr));
  CHECK(memcmp(value, attr, sizeof(attr)) == 0);
  CHECK(syscall(RemoveXattrAt, directory, "file", 0, attr) == 0);
  CHECK(fsetxattr(file, attr, NULL, 0, XATTR_CREATE) == 0);
  CHECK(fgetxattr(file, attr, NULL, 0) == 0);
  CHECK(fremovexattr(file, "user.fex-probe") == 0);
  CHECK((fgetxattr(file, "user.fex-probe", value, sizeof(value)) == -1) && errno == ENODATA);
}

static void CheckRenameReplacement(int directory) {
  int left = openat(directory, "left", O_CREAT | O_EXCL | O_RDWR, 0600);
  int right = openat(directory, "right", O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(left >= 0 && right >= 0);
  if (left >= 0 && right >= 0) {
    struct stat source = {0}, overwritten = {0}, named = {0};
    CHECK(write(left, "L", 1) == 1 && write(right, "R", 1) == 1);
    CHECK(fstat(left, &source) == 0 && fstat(right, &overwritten) == 0);
    CHECK(syscall(SYS_renameat2, directory, "left", directory, "right", 2) == 0);
    CHECK(fstatat(directory, "left", &named, 0) == 0 && named.st_ino == overwritten.st_ino);
    CHECK(fstatat(directory, "right", &named, 0) == 0 && named.st_ino == source.st_ino);
    CHECK(renameat(directory, "right", directory, "left") == 0);
    CHECK(fstatat(directory, "left", &named, 0) == 0 && named.st_ino == source.st_ino);
    CHECK(fstat(right, &named) == 0 && named.st_nlink == 0);
    char byte = 0;
    CHECK(pread(right, &byte, 1, 0) == 1 && byte == 'R');
    CHECK(pread(left, &byte, 1, 0) == 1 && byte == 'L');
  }
  if (left >= 0) CHECK(close(left) == 0);
  if (right >= 0) CHECK(close(right) == 0);
  CHECK(unlinkat(directory, "left", 0) == 0);
}

static void CheckDirectoryMetadata(int directory) {
  const struct timespec previous[] = {{111, 22}, {333, 44}};
  struct stat before = {0}, grown = {0}, shrunk = {0};
  CHECK(futimens(directory, previous) == 0);
  CHECK(fstat(directory, &before) == 0);
  CHECK(mkdirat(directory, "subdirectory", 0700) == 0);
  CHECK(fstat(directory, &grown) == 0);
  CHECK(grown.st_nlink == before.st_nlink + 1);
  CHECK(grown.st_mtim.tv_sec != before.st_mtim.tv_sec || grown.st_mtim.tv_nsec != before.st_mtim.tv_nsec);
  CHECK(unlinkat(directory, "subdirectory", AT_REMOVEDIR) == 0);
  CHECK(fstat(directory, &shrunk) == 0 && shrunk.st_nlink == before.st_nlink);
}

static void CheckDirectoryExchange(int directory) {
  CHECK(mkdirat(directory, "left-dir", 0700) == 0);
  CHECK(mkdirat(directory, "right-dir", 0700) == 0);
  int left = openat(directory, "left-dir", O_RDONLY | O_DIRECTORY);
  int right = openat(directory, "right-dir", O_RDONLY | O_DIRECTORY);
  CHECK(left >= 0 && right >= 0);
  if (left >= 0 && right >= 0) {
    struct stat original = {0}, exchanged = {0};
    CHECK(mkdirat(left, "nested", 0700) == 0);
    CHECK(symlinkat("missing", right, "soft") == 0);
    CHECK(fstat(left, &original) == 0);
    CHECK(syscall(SYS_renameat2, directory, "left-dir", directory, "right-dir", 2) == 0);
    CHECK(fstatat(directory, "right-dir", &exchanged, 0) == 0 && exchanged.st_ino == original.st_ino);
    CHECK(fstatat(directory, "left-dir/soft", &exchanged, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(exchanged.st_mode));
    CHECK(fstatat(directory, "right-dir/nested", &exchanged, 0) == 0 && S_ISDIR(exchanged.st_mode));
    CHECK(unlinkat(right, "soft", 0) == 0);
    CHECK(unlinkat(left, "nested", AT_REMOVEDIR) == 0);
  }
  if (left >= 0) CHECK(close(left) == 0);
  if (right >= 0) CHECK(close(right) == 0);
  CHECK(unlinkat(directory, "left-dir", AT_REMOVEDIR) == 0);
  CHECK(unlinkat(directory, "right-dir", AT_REMOVEDIR) == 0);
}

static void CheckMixedExchange(int directory) {
  CHECK(mkdirat(directory, "mixed-dir", 0700) == 0);
  CHECK(symlinkat("file", directory, "mixed-link") == 0);
  int open_directory = openat(directory, "mixed-dir", O_RDONLY | O_DIRECTORY);
  CHECK(open_directory >= 0);
  if (open_directory >= 0) {
    struct stat details = {0};
    CHECK(syscall(SYS_renameat2, directory, "mixed-dir", directory, "mixed-link", 2) == 0);
    CHECK(fstatat(directory, "mixed-dir", &details, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(details.st_mode));
    CHECK(fstatat(directory, "mixed-link", &details, 0) == 0 && S_ISDIR(details.st_mode));
    CHECK(mkdirat(open_directory, "retained", 0700) == 0);
    CHECK(unlinkat(open_directory, "retained", AT_REMOVEDIR) == 0);
    CHECK(syscall(SYS_renameat2, directory, "mixed-dir", directory, "mixed-link", 2) == 0);
    CHECK(close(open_directory) == 0);
  }
  CHECK(unlinkat(directory, "mixed-link", 0) == 0);
  CHECK(unlinkat(directory, "mixed-dir", AT_REMOVEDIR) == 0);
}

static void CheckDirectoryReplacement(int directory) {
  CHECK(mkdirat(directory, "from-dir", 0700) == 0);
  CHECK(mkdirat(directory, "to-dir", 0700) == 0);
  int source = openat(directory, "from-dir", O_RDONLY | O_DIRECTORY);
  int displaced = openat(directory, "to-dir", O_RDONLY | O_DIRECTORY);
  CHECK(source >= 0 && displaced >= 0);
  if (source >= 0 && displaced >= 0) {
    CHECK(symlinkat("missing", displaced, "child") == 0);
    CHECK((renameat(directory, "from-dir", directory, "to-dir") == -1) && errno == ENOTEMPTY);
    CHECK(unlinkat(displaced, "child", 0) == 0);
    struct stat original = {0}, moved = {0};
    CHECK(fstat(source, &original) == 0);
    CHECK(renameat(directory, "from-dir", directory, "to-dir") == 0);
    CHECK(fstatat(directory, "to-dir", &moved, 0) == 0 && moved.st_ino == original.st_ino);
    CHECK(fstat(displaced, &moved) == 0 && moved.st_nlink == 0);
    CHECK((mkdirat(displaced, "detached", 0700) == -1) && errno == ENOENT);
    CHECK(mkdirat(source, "child", 0700) == 0);
    CHECK(unlinkat(source, "child", AT_REMOVEDIR) == 0);
  }
  if (source >= 0) CHECK(close(source) == 0);
  if (displaced >= 0) CHECK(close(displaced) == 0);
  CHECK(unlinkat(directory, "to-dir", AT_REMOVEDIR) == 0);
}

static void CheckUnlink(int directory) {
  int old = openat(directory, "unlink-me", O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(old >= 0);
  if (old < 0) return;
  CHECK(write(old, "old", 3) == 3);
  CHECK(unlinkat(directory, "unlink-me", 0) == 0);
  struct stat original = {0}, replacement = {0};
  CHECK(fstat(old, &original) == 0 && original.st_nlink == 0);
  CHECK((fstatat(directory, "unlink-me", &replacement, 0) == -1) && errno == ENOENT);
  int fresh = openat(directory, "unlink-me", O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(fresh >= 0);
  if (fresh >= 0) {
    CHECK(fstat(fresh, &replacement) == 0 && replacement.st_ino != original.st_ino);
    CHECK(write(fresh, "new", 3) == 3);
    CHECK(close(fresh) == 0);
  }
  char text[3];
  CHECK(pread(old, text, sizeof(text), 0) == sizeof(text) && memcmp(text, "old", 3) == 0);
  CHECK(pwrite(old, "OLD", 3, 0) == 3);
  CHECK(close(old) == 0);
  CHECK(unlinkat(directory, "unlink-me", 0) == 0);
}

static void CheckOpenAt2(int directory) {
  struct OpenHow { uint64_t flags, mode, resolve, padding; } how = {O_RDONLY, 0, 8, 0};
  struct OpenHow cached = {O_RDONLY, 0, 32, 0};
  CHECK(syscall(SYS_openat2, directory, NULL, &cached, 24) == -1 && errno == EFAULT);
  CHECK(syscall(SYS_openat2, directory, "", &cached, 24) == -1 && errno == ENOENT);
  CHECK(syscall(SYS_openat2, -1, "file", &cached, 24) == -1 && errno == EBADF);
  cached.flags = UINT64_C(1) << 63;
  CHECK(syscall(SYS_openat2, directory, "file", &cached, 24) == -1 && errno == EINVAL);
  cached.flags = O_CREAT | O_RDWR;
  CHECK(syscall(SYS_openat2, directory, "", &cached, 24) == -1 && errno == EAGAIN);
  int file = syscall(SYS_openat2, directory, "file", &how, sizeof(how));
  CHECK(file >= 0);
  if (file >= 0) CHECK(close(file) == 0);
  CHECK((syscall(SYS_openat2, directory, "../file", &how, 24) == -1) && errno == EXDEV);
  CHECK((syscall(SYS_openat2, directory, "/file", &how, 24) == -1) && errno == EXDEV);
  how.resolve = 16;
  file = syscall(SYS_openat2, directory, "/../../file", &how, 24);
  CHECK(file >= 0);
  if (file >= 0) CHECK(close(file) == 0);
  how.padding = 1;
  CHECK((syscall(SYS_openat2, directory, "file", &how, sizeof(how)) == -1) && errno == E2BIG);
  CHECK(symlinkat("/file", directory, "scoped-link") == 0);
  file = syscall(SYS_openat2, directory, "scoped-link", &how, 24);
  CHECK(file >= 0);
  if (file >= 0) CHECK(close(file) == 0);
  how.resolve = 4;
  CHECK((syscall(SYS_openat2, directory, "scoped-link", &how, 24) == -1) && errno == ELOOP);
  how.resolve = 8;
  CHECK((syscall(SYS_openat2, directory, "scoped-link", &how, 24) == -1) && errno == EXDEV);
  CHECK(unlinkat(directory, "scoped-link", 0) == 0);
  CHECK(symlinkat("/proc/version", directory, "proc-link") == 0);
  how.resolve = 1;
  CHECK(syscall(SYS_openat2, directory, "proc-link", &how, 24) == -1 && errno == EXDEV);
  CHECK(syscall(SYS_openat2, AT_FDCWD, "/dev/../proc/version", &how, 24) == -1 && errno == EXDEV);
  how.resolve = 4;
  CHECK(syscall(SYS_openat2, AT_FDCWD, "/proc/self/status", &how, 24) == -1 && errno == ELOOP);
  CHECK(syscall(SYS_openat2, AT_FDCWD, "/proc/mounts", &how, 24) == -1 && errno == ELOOP);
  how.resolve = 0;
  CHECK(syscall(SYS_openat2, AT_FDCWD, "/proc/version/../version", &how, 24) == -1 && errno == ENOTDIR);
  file = syscall(SYS_openat2, directory, "proc-link", &how, 24);
  CHECK(file >= 0);
  if (file >= 0) {
    struct stat direct = {0}, alias = {0};
    CHECK(stat("/proc/version", &direct) == 0);
    CHECK(fstatat(directory, "proc-link", &alias, 0) == 0 && alias.st_ino == direct.st_ino);
    CHECK(close(file) == 0);
  }
  CHECK(unlinkat(directory, "proc-link", 0) == 0);
}

static void CheckSymlinks(int directory) {
  struct stat info = {0};
  CHECK(symlinkat("file", directory, "symbolic") == 0);
  CHECK(fstatat(directory, "symbolic", &info, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(info.st_mode));
  CHECK(fstatat(directory, "symbolic", &info, 0) == 0 && S_ISREG(info.st_mode));
  CHECK((openat(directory, "symbolic", O_RDONLY | O_NOFOLLOW) == -1) && errno == ELOOP);
  int reference = openat(directory, "symbolic", O_PATH | O_NOFOLLOW | O_CLOEXEC | O_TRUNC);
  CHECK(reference >= 0);
  if (reference >= 0) {
    CHECK(fstat(reference, &info) == 0 && S_ISLNK(info.st_mode));
    CHECK(fcntl(reference, F_GETFL) == (O_PATH | O_NOFOLLOW));
    CHECK(fcntl(reference, F_GETFD) == FD_CLOEXEC);
    CHECK((read(reference, &info, 1) == -1) && errno == EBADF);
  }
  CHECK(renameat(directory, "symbolic", directory, "renamed-link") == 0);
  char target[3] = {0, 0, '!'};
  CHECK(readlinkat(directory, "renamed-link", target, 2) == 2 && memcmp(target, "fi!", 3) == 0);
  CHECK(symlinkat("missing", directory, "other-link") == 0);
  CHECK((syscall(SYS_renameat2, directory, "renamed-link", directory, "other-link", 1) == -1) && errno == EEXIST);
  CHECK(syscall(SYS_renameat2, directory, "renamed-link", directory, "other-link", 2) == 0);
  CHECK(readlinkat(directory, "renamed-link", target, 2) == 2 && memcmp(target, "mi!", 3) == 0);
  CHECK(readlinkat(directory, "other-link", target, 2) == 2 && memcmp(target, "fi!", 3) == 0);
  CHECK(renameat(directory, "other-link", directory, "renamed-link") == 0);
  CHECK(readlinkat(directory, "renamed-link", target, 2) == 2 && memcmp(target, "fi!", 3) == 0);
  CHECK((readlinkat(directory, "other-link", target, sizeof(target)) == -1) && errno == ENOENT);
  CHECK(unlinkat(directory, "renamed-link", 0) == 0);
  if (reference >= 0) {
    CHECK(readlinkat(reference, "", target, 2) == 2 && memcmp(target, "fi!", 3) == 0);
    CHECK(fstat(reference, &info) == 0 && info.st_nlink == 0);
    CHECK(close(reference) == 0);
  }
  CHECK((readlinkat(directory, "", target, sizeof(target)) == -1) && errno == ENOENT);
  CHECK((syscall(SYS_readlinkat, directory, "", target, 1ULL << 31) == -1) && errno == EINVAL);
  CHECK(symlinkat("file/", directory, "trailing-link") == 0);
  CHECK((fstatat(directory, "trailing-link", &info, 0) == -1) && errno == ENOTDIR);
  CHECK((openat(directory, "trailing-link", O_RDONLY) == -1) && errno == ENOTDIR);
  CHECK(fstatat(directory, "trailing-link", &info, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(info.st_mode));
  CHECK(unlinkat(directory, "trailing-link", 0) == 0);
  CHECK(symlinkat("./", directory, "trailing-dir") == 0);
  CHECK(fstatat(directory, "trailing-dir", &info, 0) == 0 && S_ISDIR(info.st_mode));
  CHECK(unlinkat(directory, "trailing-dir", 0) == 0);
  CHECK(symlinkat("loop", directory, "loop") == 0);
  CHECK((fstatat(directory, "loop", &info, 0) == -1) && errno == ELOOP);
  CHECK(unlinkat(directory, "loop", 0) == 0);
}

static void CheckNames(int root, int directory, int file) {
  struct stat original = {0}, linked = {0};
  CHECK(fstat(file, &original) == 0);
  CHECK((syscall(SYS_renameat2, root, "original", root, "original", 1) == -1) && errno == EEXIST);
  CHECK(renameat(root, "original", root, "renamed") == 0);
  CHECK((fstatat(root, "original", &linked, 0) == -1) && errno == ENOENT);
  CHECK(fstatat(directory, "file", &linked, 0) == 0 && linked.st_ino == original.st_ino);
  CHECK(linkat(directory, "file", directory, "hard", 0) == 0);
  int alias = openat(directory, "hard", O_RDWR);
  CHECK(alias >= 0);
  if (alias >= 0) {
    CHECK(pwrite(alias, "!", 1, 8) == 1);
    char byte;
    CHECK(pread(file, &byte, 1, 8) == 1 && byte == '!');
    CHECK(close(alias) == 0);
  }
  CHECK(symlinkat("hard", directory, "soft") == 0);
  char target[4];
  CHECK(readlinkat(directory, "soft", target, sizeof(target)) == sizeof(target) && memcmp(target, "hard", 4) == 0);
  CHECK((fstatat(directory, "soft", &linked, AT_SYMLINK_NOFOLLOW) == 0) && S_ISLNK(linked.st_mode));
  CHECK(fstatat(directory, "soft", &linked, 0) == 0);
  CHECK(linked.st_ino == original.st_ino && linked.st_nlink == 2);
  CHECK(unlinkat(directory, "file", 0) == 0);
  CHECK(unlinkat(directory, "hard", 0) == 0);
  CHECK((fstatat(directory, "soft", &linked, 0) == -1) && errno == ENOENT);
  CHECK(fstat(file, &linked) == 0 && linked.st_nlink == 0 && linked.st_ino == original.st_ino);
  char text[5];
  CHECK(pread(file, text, sizeof(text), 0) == sizeof(text) && memcmp(text, "hello", 5) == 0);
  CHECK(unlinkat(directory, "soft", 0) == 0);
}

// "long/" followed by two 200-byte directories, and a 255-byte file name.
static void LongPersistencePath(char *deep, char *name) {
  strcpy(deep, "long/");
  memset(deep + 5, 'p', 200);
  deep[205] = '/';
  memset(deep + 206, 'q', 200);
  deep[406] = 0;
  memset(name, 'n', 255);
  name[255] = 0;
}

static int CheckPersistence(const char *path, int writing) {
  const char attribute[] = "user.fex.persist";
  const unsigned char payload[] = {0x00, 0xa5, 0x00, 0xff, 0x41};
  if (writing && mkdir(path, 0700) < 0) {
    perror("persistence directory");
    return 1;
  }
  int directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  CHECK(directory >= 0);
  if (directory < 0) return 1;
  if (writing) {
    int file = openat(directory, "source", O_CREAT | O_EXCL | O_RDWR, 0640);
    CHECK(file >= 0);
    if (file >= 0) {
      CHECK(write(file, "persistent", 10) == 10);
      CHECK(fallocate(file, FALLOC_FL_KEEP_SIZE, 8192, 4096) == 0);
      CHECK(fchmod(file, 0640) == 0);
      CHECK(futimens(file, (struct timespec[]){{123, 45}, {678, 901}}) == 0);
      CHECK(fsetxattr(file, attribute, payload, sizeof(payload), XATTR_CREATE) == 0);
      CHECK(fdatasync(file) == 0);
      CHECK(close(file) == 0);
      CHECK(linkat(directory, "source", directory, "hard", 0) == 0);
      CHECK(symlinkat("hard", directory, "staging-soft") == 0);
      CHECK(renameat(directory, "staging-soft", directory, "soft") == 0);
      CHECK(mkfifoat(directory, "persistent-fifo", 0600) == 0);
      int sparse = openat(directory, "sparse-tail", O_CREAT | O_EXCL | O_RDWR, 0600);
      CHECK(sparse >= 0);
      if (sparse >= 0) {
        CHECK(pwrite(sparse, "a", 1, 0) == 1);
        CHECK(pwrite(sparse, "z", 1, 16383) == 1);
        CHECK(fallocate(sparse, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 8192, 8192) == 0);
        CHECK(fsync(sparse) == 0);
        CHECK(close(sparse) == 0);
      }
      int grown = openat(directory, "grown", O_CREAT | O_EXCL | O_RDWR, 0600);
      CHECK(grown >= 0);
      if (grown >= 0) {
        CHECK(ftruncate(grown, 32768) == 0);
        CHECK(linkat(directory, "grown", directory, "grown-link", 0) == 0);
        CHECK(renameat(directory, "grown", directory, "grown-renamed") == 0);
        CHECK(fsync(grown) == 0);
        CHECK(close(grown) == 0);
      }
      int interior = openat(directory, "interior", O_CREAT | O_EXCL | O_RDWR, 0600);
      CHECK(interior >= 0);
      if (interior >= 0) {
        CHECK(fallocate(interior, 0, 0, 12288) == 0);
        CHECK(pwrite(interior, "a", 1, 0) == 1);
        CHECK(pwrite(interior, "z", 1, 12287) == 1);
        CHECK(linkat(directory, "interior", directory, "interior-link", 0) == 0);
        CHECK(fallocate(interior, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 4096, 4096) == 0);
        CHECK(pwrite(interior, "b", 1, 0) == 1);
        struct iovec synchronous = {(void *)"c", 1};
        CHECK(syscall(SYS_pwritev2, interior, &synchronous, 1, 1UL, 0UL, RWF_DSYNC) == 1);
        CHECK(fsync(interior) == 0);
        CHECK(close(interior) == 0);
      }
      // Paths beyond the SD card's 257-byte limit live in the inode store:
      // a directory chain, a 255-byte name and a native tree moved below it.
      char deep[512], name[256], entry[800];
      LongPersistencePath(deep, name);
      CHECK(mkdirat(directory, "long", 0750) == 0);
      deep[205] = 0;
      CHECK(mkdirat(directory, deep, 0750) == 0);
      deep[205] = '/';
      CHECK(mkdirat(directory, deep, 0750) == 0);
      snprintf(entry, sizeof(entry), "%s/%s", deep, name);
      int overflow = openat(directory, entry, O_CREAT | O_EXCL | O_WRONLY, 0604);
      CHECK(overflow >= 0);
      if (overflow >= 0) {
        CHECK(write(overflow, "overflow", 8) == 8);
        CHECK(fsync(overflow) == 0);
        CHECK(close(overflow) == 0);
      }
      CHECK(mkdirat(directory, "tree", 0700) == 0 && mkdirat(directory, "tree/inner", 0710) == 0);
      int native = openat(directory, "tree/inner/data", O_CREAT | O_EXCL | O_WRONLY, 0600);
      CHECK(native >= 0);
      if (native >= 0) {
        CHECK(write(native, "native", 6) == 6);
        CHECK(close(native) == 0);
      }
      snprintf(entry, sizeof(entry), "%s/tree", deep);
      CHECK(renameat(directory, "tree", directory, entry) == 0);
      CHECK(fsync(directory) == 0);
    }
  } else {
    char deep[512], name[256], entry[800], data[16] = {0};
    struct stat info = {0};
    LongPersistencePath(deep, name);
    CHECK(fstatat(directory, deep, &info, 0) == 0 && S_ISDIR(info.st_mode) && (info.st_mode & 07777) == 0750 &&
          info.st_nlink == 3);
    snprintf(entry, sizeof(entry), "%s/%s", deep, name);
    int overflow = openat(directory, entry, O_RDONLY);
    CHECK(overflow >= 0);
    if (overflow >= 0) {
      CHECK(fstat(overflow, &info) == 0 && S_ISREG(info.st_mode) && (info.st_mode & 07777) == 0604 && info.st_size == 8);
      CHECK(read(overflow, data, sizeof(data)) == 8 && memcmp(data, "overflow", 8) == 0);
      CHECK(close(overflow) == 0);
    }
    snprintf(entry, sizeof(entry), "%s/tree/inner", deep);
    CHECK(fstatat(directory, entry, &info, 0) == 0 && S_ISDIR(info.st_mode) && (info.st_mode & 07777) == 0710);
    snprintf(entry, sizeof(entry), "%s/tree/inner/data", deep);
    int native = openat(directory, entry, O_RDONLY);
    CHECK(native >= 0);
    if (native >= 0) {
      memset(data, 0, sizeof(data));
      CHECK(read(native, data, sizeof(data)) == 6 && memcmp(data, "native", 6) == 0);
      CHECK(close(native) == 0);
    }
    CHECK(fstatat(directory, "tree", &info, 0) == -1 && errno == ENOENT);
    int listing = openat(directory, deep, O_RDONLY | O_DIRECTORY);
    CHECK(listing >= 0);
    DIR *stream = listing >= 0 ? fdopendir(listing) : NULL;
    int names = 0;
    for (struct dirent *item; stream && (item = readdir(stream));) {
      if (!strcmp(item->d_name, "tree")) names += item->d_type == DT_DIR ? 1 : 100;
      else if (!strcmp(item->d_name, name)) names += item->d_type == DT_REG ? 10 : 100;
    }
    CHECK(names == 11);
    if (stream) closedir(stream);
    struct stat source = {0}, link = {0}, fifo = {0};
    CHECK(fstatat(directory, "persistent-fifo", &fifo, 0) == 0 && S_ISFIFO(fifo.st_mode));
    CHECK((fifo.st_mode & 0777) == 0600);
    CHECK((fstatat(directory, "staging-soft", &link, AT_SYMLINK_NOFOLLOW) == -1) && errno == ENOENT);
    CHECK(fstatat(directory, "source", &source, 0) == 0);
    CHECK(fstatat(directory, "soft", &link, 0) == 0);
    CHECK(source.st_ino == link.st_ino && source.st_nlink == 2);
    CHECK((source.st_mode & 0777) == 0640);
    CHECK(source.st_size == 10 && source.st_blocks >= 16);
    CHECK(source.st_mtim.tv_sec == 678 && source.st_mtim.tv_nsec == 901);
    int sparse = openat(directory, "sparse-tail", O_RDONLY);
    CHECK(sparse >= 0);
    if (sparse >= 0) {
      struct stat info = {0};
      char byte = 1;
      CHECK(fstat(sparse, &info) == 0 && info.st_size == 16384);
      CHECK(pread(sparse, &byte, 1, 0) == 1 && byte == 'a');
      CHECK(pread(sparse, &byte, 1, 16383) == 1 && byte == 0);
      CHECK(pread(sparse, &byte, 1, 16384) == 0);
      CHECK(lseek(sparse, 8192, SEEK_HOLE) == 8192);
      CHECK((lseek(sparse, 8192, SEEK_DATA) == -1) && errno == ENXIO);
      CHECK(close(sparse) == 0);
    }
    int file = openat(directory, "soft", O_RDONLY);
    CHECK(file >= 0);
    if (file >= 0) {
      char value[10];
      CHECK(fgetxattr(file, attribute, value, sizeof(value)) == sizeof(payload) &&
            memcmp(value, payload, sizeof(payload)) == 0);
      CHECK(read(file, value, sizeof(value)) == sizeof(value) && memcmp(value, "persistent", sizeof(value)) == 0);
      CHECK(read(file, value, sizeof(value)) == 0);
      CHECK(close(file) == 0);
    }
    int interior = openat(directory, "interior-link", O_RDONLY);
    CHECK(interior >= 0);
    if (interior >= 0) {
      struct stat info = {0}, alias = {0};
      char byte = 1;
      CHECK(fstat(interior, &info) == 0 && info.st_size == 12288 && info.st_nlink == 2 && info.st_blocks == 16);
      CHECK(fstatat(directory, "interior", &alias, 0) == 0 &&
            alias.st_ino == info.st_ino && alias.st_blocks == info.st_blocks);
      CHECK(lseek(interior, 0, SEEK_HOLE) == 4096);
      CHECK(lseek(interior, 4096, SEEK_DATA) == 8192);
      CHECK(pread(interior, &byte, 1, 0) == 1 && byte == 'b');
      CHECK(pread(interior, &byte, 1, 1) == 1 && byte == 'c');
      CHECK(pread(interior, &byte, 1, 4096) == 1 && byte == 0);
      CHECK(pread(interior, &byte, 1, 12287) == 1 && byte == 'z');
      CHECK(close(interior) == 0);
    }
    struct stat grown = {0}, grown_link = {0};
    CHECK(fstatat(directory, "grown-renamed", &grown, 0) == 0 && grown.st_size == 32768 && grown.st_blocks == 0);
    CHECK(fstatat(directory, "grown-link", &grown_link, 0) == 0 &&
          grown_link.st_ino == grown.st_ino && grown_link.st_nlink == 2 && grown_link.st_blocks == 0);
    int reopened = openat(directory, "grown-link", O_RDONLY);
    CHECK(reopened >= 0);
    if (reopened >= 0) {
      char byte = 1;
      CHECK(lseek(reopened, 0, SEEK_HOLE) == 0);
      CHECK(lseek(reopened, 0, SEEK_DATA) == -1 && errno == ENXIO);
      CHECK(pread(reopened, &byte, 1, 32767) == 1 && byte == 0);
      CHECK(pread(reopened, &byte, 1, 32768) == 0);
      CHECK(close(reopened) == 0);
    }
  }
  CHECK(close(directory) == 0);
  return Failures ? 1 : 42;
}

static int CheckInterruptedRename(const char *phase, int writing) {
  const char directory[] = "/.fex-recovery-test";
  const int committed = strcmp(phase, "after-checkpoint") == 0;
  if (!committed && strcmp(phase, "before-checkpoint") != 0) return 2;
  if (writing && mkdir(directory, 0700) != 0) {
    perror(directory);
    return 1;
  }
  int dir = open(directory, O_RDONLY | O_DIRECTORY);
  CHECK(dir >= 0);
  if (dir < 0) return 1;
  if (writing) {
    int file = openat(dir, "source", O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(file >= 0);
    if (file >= 0) {
      CHECK(write(file, "recovery", 8) == 8);
      CHECK(fsync(file) == 0);
      CHECK(close(file) == 0);
    }
    if (!Failures) CHECK((renameat(dir, "source", dir, phase) == -1) && errno == EINTR);
  } else {
    const char *expected = committed ? phase : "source";
    const char *absent = committed ? "source" : phase;
    struct stat info = {0};
    CHECK((fstatat(dir, absent, &info, AT_SYMLINK_NOFOLLOW) == -1) && errno == ENOENT);
    int file = openat(dir, expected, O_RDONLY);
    CHECK(file >= 0);
    if (file >= 0) {
      char bytes[8];
      CHECK(read(file, bytes, sizeof(bytes)) == sizeof(bytes) && memcmp(bytes, "recovery", 8) == 0);
      CHECK(close(file) == 0);
    }
    if (!Failures) CHECK(unlinkat(dir, expected, 0) == 0);
  }
  CHECK(close(dir) == 0);
  if (!writing && !Failures) CHECK(rmdir(directory) == 0);
  return Failures ? 1 : 42;
}

int main(int argc, char **argv) {
  if (argc == 3 && (strcmp(argv[1], "--recover-write") == 0 || strcmp(argv[1], "--recover-read") == 0)) {
    return CheckInterruptedRename(argv[2], strcmp(argv[1], "--recover-write") == 0);
  }
  if (argc == 3 && (strcmp(argv[1], "--persist-write") == 0 || strcmp(argv[1], "--persist-read") == 0)) {
    return CheckPersistence(argv[2], strcmp(argv[1], "--persist-write") == 0);
  }
  const char *location = ".";
  if (argc == 3 && strcmp(argv[1], "-d") == 0) {
    location = argv[2];
  } else if (argc != 1) {
    fprintf(stderr, "usage: %s [-d scratch-parent | --persist-write|--persist-read directory]\n", argv[0]);
    return 2;
  }
  char scratch[PATH_MAX];
  snprintf(scratch, sizeof(scratch), "%s/fex-fs-probe.XXXXXX", location);
  if (!mkdtemp(scratch)) {
    perror("mkdtemp in probe working directory");
    return 1;
  }
  int root = open(scratch, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  CHECK(root >= 0);
  if (root >= 0) {
    CheckFilesystem(scratch, root);
    CheckVirtualFilesystems();
    CheckMemfdAppend();
    CheckMemfdAllocation();
    CheckMemfdWriteSeals();
    CHECK(mkdirat(root, "original", 0700) == 0);
    int directory = openat(root, "original", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(directory >= 0);
    if (directory >= 0) {
      char byte;
      CHECK((read(directory, &byte, 0) == -1) && errno == EISDIR);
      CHECK((openat(directory, "missing/../wrong", O_CREAT | O_EXCL | O_WRONLY, 0600) == -1) && errno == ENOENT);
      CHECK((pread(directory, &byte, 0, 0) == -1) && errno == EISDIR);
      int file = openat(directory, "file", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
      CHECK(file >= 0);
      if (file >= 0) {
        CHECK((openat(directory, "file", O_CREAT | O_EXCL | O_RDWR, 0600) == -1) && errno == EEXIST);
        puts("Checking descriptors and file I/O...");
        CheckFileIO(file);
        CheckVectorIO(file);
        CheckAllocationErrors(directory, file);
        CheckSparseTruncate(directory);
        CheckSparseExec(directory);
        CheckZeroRange(directory);
        CheckSetIdOnWrite(directory);
        CheckFilesystemCredentials(directory);
        CheckPartialFileIO(directory);
        CheckDirents(directory, file, 0);
        CheckDirents(directory, file, 1);
        CheckLocks(directory, file);
        CheckRecordLocks(directory, file);
        CheckRecordLockDeadlock(file);
        CheckLockInterruption(directory, file, 0);
        CheckLockInterruption(directory, file, 1);
        CheckOpenStatusFlags(directory);
        CheckTemporary(directory);
        CheckNodes(directory);
        CheckReadAdvice(directory, file);
        CheckCloseRange(file);
        puts("Checking file and pipe transfers...");
        CheckCopyRange(directory, file);
        CheckLargeTransfers(directory);
        CheckSendFile(file);
        CheckSplice(file);
        CheckPrivatePipePageAcrossFork();
        CheckTransferWriteback(file);
        CheckPipeControls();
        CheckBrokenPipeSignals();
        puts("Checking metadata, permissions and extended attributes...");
        CheckMetadata(directory, file);
        CheckAcl(directory);
        puts("Checking paths, links and namespace mutations...");
        CheckOpenAt2(directory);
        CheckSymlinks(directory);
        CheckRenameReplacement(directory);
        CheckDirectoryMetadata(directory);
        CheckDirectoryExchange(directory);
        CheckMixedExchange(directory);
        CheckDirectoryReplacement(directory);
        CheckUnlink(directory);
        CheckNames(root, directory, file);
        CHECK(close(file) == 0);
      }
      CHECK(close(directory) == 0);
    }
    // A failed rename leaves the original name in use.
    if (unlinkat(root, "renamed", AT_REMOVEDIR) < 0) {
      CHECK(unlinkat(root, "original", AT_REMOVEDIR) == 0);
    }
    const mode_t previous_mask = umask(0077);
    CHECK(mkdirat(root, "recreated", 0777) == 0);
    CHECK(umask(previous_mask) == 0077);
    struct stat created = {0};
    CHECK(fstatat(root, "recreated", &created, 0) == 0 && (created.st_mode & 0777) == 0700);
    int removed = openat(root, "recreated", O_RDONLY | O_DIRECTORY);
    CHECK(removed >= 0);
    if (removed >= 0) {
      struct stat old = {0}, fresh = {0};
      CHECK(unlinkat(root, "recreated", AT_REMOVEDIR) == 0);
      CHECK(mkdirat(root, "recreated", 0700) == 0);
      CHECK(fstat(removed, &old) == 0 && old.st_nlink == 0);
      CHECK(fstatat(root, "recreated", &fresh, 0) == 0 && fresh.st_ino != old.st_ino);
      CHECK((mkdirat(removed, "child", 0700) == -1) && errno == ENOENT);
      CHECK(close(removed) == 0);
    }
    CHECK(unlinkat(root, "recreated", AT_REMOVEDIR) == 0);
    CHECK(close(root) == 0);
  }
  CHECK(rmdir(scratch) == 0);
  if (Failures) {
    fprintf(stderr, "Filesystem probe failed %u checks.\n", Failures);
    return 1;
  }
  puts("Filesystem probe passed.");
  return 42;
}
