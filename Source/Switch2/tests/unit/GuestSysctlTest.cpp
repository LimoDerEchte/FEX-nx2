// SPDX-License-Identifier: MIT
#include "GuestSysctl.h"

#include <cstdio>
#include <cstdlib>
#include <set>

#define CHECK(Cond) do { if (!(Cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #Cond); std::exit(1); } } while (0)

using namespace Switch2Guest::Sysctl;

int main() {
  // The fixed table: unique /proc/sys paths, a newline-terminated value, a Linux mode.
  std::set<std::string_view> Seen;
  for (const auto& Entry : FixedFiles) {
    CHECK(Entry.Path.starts_with("/proc/sys/"));
    CHECK(Seen.insert(Entry.Path).second);
    CHECK(!Entry.Text.empty() && Entry.Text.back() == '\n');
    CHECK(Entry.Mode == 0644 || Entry.Mode == 0444);
  }
  const auto Find = [&](std::string_view Path) -> const Fixed& {
    for (const auto& Entry : FixedFiles) {
      if (Entry.Path == Path) return Entry;
    }
    std::fprintf(stderr, "missing %s\n", std::string(Path).c_str());
    std::exit(1);
  };
  CHECK(Find("/proc/sys/kernel/pid_max").Text == "32768\n");
  CHECK(Find("/proc/sys/kernel/yama/ptrace_scope").Text == "0\n");
  CHECK(Find("/proc/sys/vm/max_map_count").Text == "65530\n");
  CHECK(Find("/proc/sys/vm/mmap_min_addr").Text == "65536\n");
  CHECK(Find("/proc/sys/fs/file-max").Text == "9223372036854775807\n");
  CHECK(Find("/proc/sys/net/core/somaxconn").Text == "4096\n");
  CHECK(Find("/proc/sys/kernel/random/poolsize").Mode == 0444);

  // Formatting.
  CHECK(FormatInt(-5) == "-5\n");
  CHECK(FormatUnsigned(18446744073692774399ULL) == "18446744073692774399\n");
  CHECK(FormatVector({32000, 1024000000, 500, 32000}) == "32000\t1024000000\t500\t32000\n");
  const uint8_t Random[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0xff, 0x77, 0xff, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  CHECK(FormatUuid(Random) == "00112233-4455-4f77-bf99-aabbccddeeff\n");

  // Integer writes: white space around, EINVAL for junk, empty and out of range values.
  CHECK(ParseInt("42\n", 0, 100) == 42);
  CHECK(ParseInt("  7  \n", 0, 100) == 7);
  CHECK(ParseInt("+9", 0, 100) == 9);
  CHECK(ParseInt("-3", -10, 10) == -3);
  CHECK(!ParseInt("", 0, 100));
  CHECK(!ParseInt("\n", 0, 100));
  CHECK(!ParseInt("12abc", 0, 100));
  CHECK(!ParseInt("1 2", 0, 100));
  CHECK(!ParseInt("101", 0, 100));
  CHECK(!ParseInt("-1", 0, 100));
  CHECK(!ParseInt("99999999999999999999999", 0, INT64_MAX));
  CHECK(ParseInt("-9223372036854775808", INT64_MIN, INT64_MAX) == INT64_MIN);
  CHECK(ParseInt("9223372036854775807", 0, INT64_MAX) == INT64_MAX);
  CHECK(!ParseInt("9223372036854775808", 0, INT64_MAX));

  // Vector writes (kernel.sem): exactly the count asked for.
  const auto Sem = ParseVector("250 32000\t32 128\n", 4, 0, 2147483647);
  CHECK(Sem && *Sem == (std::vector<int64_t> {250, 32000, 32, 128}));
  CHECK(!ParseVector("250 32000 32", 4, 0, 2147483647));
  CHECK(!ParseVector("250 32000 32 128 1", 4, 0, 2147483647));
  CHECK(!ParseVector("250 32000 32 -1", 4, 0, 2147483647));
  CHECK(!ParseVector("250 x 32 1", 4, 0, 2147483647));

  // String writes (hostname): up to a newline, at most Max bytes.
  CHECK(ParseString("box\n", 64) == "box");
  CHECK(ParseString("box", 64) == "box");
  CHECK(ParseString("a\nb", 64) == "a");
  CHECK(ParseString(std::string_view {"ab\0cd", 5}, 64) == "ab");
  CHECK(ParseString("abcdef", 3) == "abc");
  CHECK(ParseString("\n", 64).empty());

  std::puts("guest-sysctl-test: ok");
  return 0;
}
