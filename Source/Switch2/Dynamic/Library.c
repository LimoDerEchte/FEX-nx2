// SPDX-License-Identifier: MIT
#include "../SDL2/GuestSyscall.h"

int fex_demo_message(void) {
  static const char message[] = "Hello from a dynamically linked x86-64 library!\n";
  static const char proc[] = "/proc/version";
  char version[128];
  const long fd = guest_call(257, -100, (long)proc, 0);
  if (fd < 0) return 1;
  const long read = guest_call(0, fd, (long)version, sizeof(version));
  guest_call(3, fd, 0, 0);
  if (read <= 0) return 1;
  guest_call(1, 1, (long)version, read);
  return guest_call(1, 1, (long)message, sizeof(message) - 1) == sizeof(message) - 1 ? 42 : 1;
}
