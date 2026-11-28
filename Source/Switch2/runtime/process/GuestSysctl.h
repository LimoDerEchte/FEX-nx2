// SPDX-License-Identifier: MIT
// The pure part of /proc/sys: the fixed values the runtime serves, the text of integer and vector files, and the
// parsing of writes (proc_dointvec_minmax, proc_doulongvec_minmax, proc_dostring). The runtime fills the nodes of its
// kernel tree from these; values that come from other runtime headers (System V IPC, inotify, mqueue, pipes) are
// formatted by the runtime with FormatInt and FormatVector.
#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Switch2Guest::Sysctl {

constexpr int64_t Einval = -22;

// A file whose value never changes while the runtime runs. Mode is what Linux shows (0644 for a file root may write);
// the runtime gives these files no writer, so a write fails at open (EACCES) because the runtime has no behavior to
// change.
struct Fixed {
  std::string_view Path;
  uint32_t Mode;
  std::string_view Text;
};

inline constexpr Fixed FixedFiles[] = {
    {"/proc/sys/kernel/pid_max", 0644, "32768\n"},  // the default of a machine with up to 32 CPUs
    {"/proc/sys/kernel/random/entropy_avail", 0444, "256\n"},
    {"/proc/sys/kernel/random/poolsize", 0444, "256\n"},
    {"/proc/sys/kernel/yama/ptrace_scope", 0644, "0\n"},  // classic ptrace rules: the runtime has no Yama restriction
    {"/proc/sys/kernel/dmesg_restrict", 0644, "1\n"},
    {"/proc/sys/kernel/kptr_restrict", 0644, "1\n"},
    {"/proc/sys/kernel/perf_event_paranoid", 0644, "2\n"},
    {"/proc/sys/kernel/unprivileged_userns_clone", 0644, "1\n"},
    {"/proc/sys/kernel/ngroups_max", 0444, "65536\n"},
    {"/proc/sys/vm/overcommit_memory", 0644, "0\n"},
    {"/proc/sys/vm/max_map_count", 0644, "65530\n"},
    {"/proc/sys/vm/mmap_min_addr", 0644, "65536\n"},
    {"/proc/sys/vm/swappiness", 0644, "60\n"},
    {"/proc/sys/fs/file-max", 0644, "9223372036854775807\n"},
    {"/proc/sys/fs/nr_open", 0644, "1048576\n"},
    {"/proc/sys/net/core/somaxconn", 0644, "4096\n"},
};

inline std::string FormatInt(int64_t Value) { return std::to_string(Value) + "\n"; }
inline std::string FormatUnsigned(uint64_t Value) { return std::to_string(Value) + "\n"; }

// proc_dointvec with several values (kernel.sem): tab separated, one line.
inline std::string FormatVector(const std::vector<int64_t>& Values) {
  std::string Out;
  for (size_t I = 0; I < Values.size(); ++I) Out += (I ? "\t" : "") + std::to_string(Values[I]);
  return Out + "\n";
}

// A version 4 UUID in the 8-4-4-4-12 form of kernel/random/uuid and boot_id (generate_random_uuid).
inline std::string FormatUuid(const uint8_t (&Random)[16]) {
  uint8_t B[16];
  for (int I = 0; I < 16; ++I) B[I] = Random[I];
  B[6] = static_cast<uint8_t>((B[6] & 0x0f) | 0x40);
  B[8] = static_cast<uint8_t>((B[8] & 0x3f) | 0x80);
  char Text[40];
  std::snprintf(Text, sizeof(Text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", B[0], B[1], B[2], B[3],
                B[4], B[5], B[6], B[7], B[8], B[9], B[10], B[11], B[12], B[13], B[14], B[15]);
  return Text;
}

// One number of proc_dointvec_minmax: leading white space, an optional sign, digits, then only white space. Nothing
// else, no value, or a value outside [Min, Max] is EINVAL.
inline std::optional<int64_t> ParseInt(std::string_view Text, int64_t Min, int64_t Max) {
  const auto Space = [](char C) { return C == ' ' || C == '\t' || C == '\n' || C == '\r'; };
  size_t I = 0;
  while (I < Text.size() && Space(Text[I])) ++I;
  bool Negative = false;
  if (I < Text.size() && (Text[I] == '-' || Text[I] == '+')) Negative = Text[I++] == '-';
  if (I >= Text.size() || Text[I] < '0' || Text[I] > '9') return std::nullopt;
  uint64_t Value = 0;
  for (; I < Text.size() && Text[I] >= '0' && Text[I] <= '9'; ++I) {
    if (Value > (UINT64_MAX - 9) / 10) return std::nullopt;
    Value = Value * 10 + static_cast<uint64_t>(Text[I] - '0');
  }
  for (; I < Text.size(); ++I) {
    if (!Space(Text[I])) return std::nullopt;
  }
  if (Negative) {
    if (Value > uint64_t {1} << 63) return std::nullopt;
    const int64_t Result = Value == uint64_t {1} << 63 ? INT64_MIN : -static_cast<int64_t>(Value);
    if (Result < Min) return std::nullopt;
    return Result;
  }
  if (Value > static_cast<uint64_t>(INT64_MAX)) return std::nullopt;
  const auto Result = static_cast<int64_t>(Value);
  if (Result < Min || Result > Max) return std::nullopt;
  return Result;
}

// Exactly Count white-space separated numbers (kernel.sem takes four), each in [Min, Max].
inline std::optional<std::vector<int64_t>> ParseVector(std::string_view Text, size_t Count, int64_t Min, int64_t Max) {
  const auto Space = [](char C) { return C == ' ' || C == '\t' || C == '\n' || C == '\r'; };
  std::vector<int64_t> Out;
  size_t I = 0;
  while (true) {
    while (I < Text.size() && Space(Text[I])) ++I;
    if (I >= Text.size()) break;
    const size_t Start = I;
    while (I < Text.size() && !Space(Text[I])) ++I;
    const auto Value = ParseInt(Text.substr(Start, I - Start), Min, Max);
    if (!Value) return std::nullopt;
    Out.push_back(*Value);
  }
  if (Out.size() != Count) return std::nullopt;
  return Out;
}

// proc_dostring on a write: the text up to the first newline or NUL, cut to Max bytes (the rest is dropped, as
// Linux does for a string file).
inline std::string ParseString(std::string_view Text, size_t Max) {
  const auto End = Text.find_first_of(std::string_view {"\n\0", 2});
  if (End != std::string_view::npos) Text = Text.substr(0, End);
  return std::string {Text.substr(0, Max)};
}

}  // namespace Switch2Guest::Sysctl
