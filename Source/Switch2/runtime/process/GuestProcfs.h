// SPDX-License-Identifier: MIT
// The text of the process files procps-ng (ps, top, free, uptime, pgrep) reads: /proc/<pid>/stat, statm, cmdline, comm,
// environ, cgroup, oom_score*, /proc/stat, /proc/uptime, /proc/loadavg, /proc/meminfo, /proc/cpuinfo. The runtime fills
// the plain structs from its process table; the formats are those of the Linux 5.15 fs/proc code (do_task_stat,
// proc_pid_statm, show_stat, meminfo_proc_show, loadavg_proc_show, uptime_proc_show).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace Switch2Guest::Procfs {

constexpr uint64_t PageBytes = 4096;
constexpr uint64_t ClockTicks = 100;  // USER_HZ

// A process for /proc/<pid>/stat. Times are in clock ticks (USER_HZ), StartTime in ticks since boot.
struct Stat {
  uint32_t Pid {};
  std::string Comm;       // the 15 character task name
  char State {'S'};       // R S D T Z X
  uint32_t Ppid {}, Pgrp {}, Session {};
  int32_t TtyNr {};       // makedev(major, minor) of the controlling terminal, 0 for none
  int32_t Tpgid {-1};     // foreground group of that terminal, -1 for none
  uint32_t Flags {};      // PF_* (0x400000 PF_FORKNOEXEC is not tracked)
  uint64_t Utime {}, Stime {}, Cutime {}, Cstime {};
  int64_t Priority {20}, Nice {};
  uint32_t Threads {1};
  uint64_t StartTime {};
  uint64_t Vsize {}, RssPages {};
  uint64_t Rsslim {~uint64_t {0}};
  uint64_t StartCode {}, EndCode {}, StartStack {};
  uint64_t SigPending {}, SigBlocked {}, SigIgnored {}, SigCaught {};
  uint32_t ExitSignal {17};
  uint32_t Processor {};
  uint32_t RtPriority {}, Policy {};
  uint64_t StartData {}, EndData {}, StartBrk {}, ArgStart {}, ArgEnd {}, EnvStart {}, EnvEnd {};
  int32_t ExitCode {};
};

inline std::string FormatStat(const Stat& S) {
  char Head[96];
  std::snprintf(Head, sizeof(Head), "%u (", S.Pid);
  std::string Out = Head + S.Comm;
  char Tail[1024];
  std::snprintf(Tail, sizeof(Tail),
                ") %c %u %u %u %d %d %u 0 0 0 0 %llu %llu %llu %llu %lld %lld %u 0 %llu %llu %lld %llu %llu %llu %llu 0 0 %llu %llu %llu %llu 0 0 0 %u %u %u %u 0 0 0 %llu %llu %llu %llu %llu %llu %llu %d\n",
                S.State, S.Ppid, S.Pgrp, S.Session, S.TtyNr, S.Tpgid, S.Flags,
                static_cast<unsigned long long>(S.Utime), static_cast<unsigned long long>(S.Stime), static_cast<unsigned long long>(S.Cutime),
                static_cast<unsigned long long>(S.Cstime), static_cast<long long>(S.Priority), static_cast<long long>(S.Nice), S.Threads,
                static_cast<unsigned long long>(S.StartTime), static_cast<unsigned long long>(S.Vsize), static_cast<long long>(S.RssPages),
                static_cast<unsigned long long>(S.Rsslim), static_cast<unsigned long long>(S.StartCode), static_cast<unsigned long long>(S.EndCode),
                static_cast<unsigned long long>(S.StartStack), static_cast<unsigned long long>(S.SigPending),
                static_cast<unsigned long long>(S.SigBlocked), static_cast<unsigned long long>(S.SigIgnored),
                static_cast<unsigned long long>(S.SigCaught), S.ExitSignal, S.Processor, S.RtPriority, S.Policy,
                static_cast<unsigned long long>(S.StartData), static_cast<unsigned long long>(S.EndData), static_cast<unsigned long long>(S.StartBrk),
                static_cast<unsigned long long>(S.ArgStart), static_cast<unsigned long long>(S.ArgEnd), static_cast<unsigned long long>(S.EnvStart),
                static_cast<unsigned long long>(S.EnvEnd), S.ExitCode);
  return Out + Tail;
}

// /proc/<pid>/statm in pages: size resident shared text lib data dt.
inline std::string FormatStatm(uint64_t SizePages, uint64_t ResidentPages, uint64_t SharedPages, uint64_t TextPages, uint64_t DataPages) {
  char Line[160];
  std::snprintf(Line, sizeof(Line), "%llu %llu %llu %llu 0 %llu 0\n", static_cast<unsigned long long>(SizePages),
                static_cast<unsigned long long>(ResidentPages), static_cast<unsigned long long>(SharedPages),
                static_cast<unsigned long long>(TextPages), static_cast<unsigned long long>(DataPages));
  return Line;
}

// /proc/<pid>/cmdline and environ: the strings, each followed by NUL.
inline std::string FormatNulList(const std::vector<std::string>& Strings) {
  std::string Out;
  for (const auto& S : Strings) Out.append(S).push_back('\0');
  return Out;
}

// /proc/uptime: seconds since boot and the idle time summed over the CPUs, 2 decimals.
inline std::string FormatUptime(uint64_t UptimeNs, uint64_t IdleNs) {
  char Line[96];
  const auto Centis = [](uint64_t Ns) { return Ns / 10'000'000; };
  std::snprintf(Line, sizeof(Line), "%llu.%02llu %llu.%02llu\n", static_cast<unsigned long long>(Centis(UptimeNs) / 100),
                static_cast<unsigned long long>(Centis(UptimeNs) % 100), static_cast<unsigned long long>(Centis(IdleNs) / 100),
                static_cast<unsigned long long>(Centis(IdleNs) % 100));
  return Line;
}

// The load averages of kernel/sched/loadavg.c: fixed point (FSHIFT 11), exponentially damped every 5 s with the number of
// runnable tasks. The runtime has no tick that samples them; Update is called when /proc/loadavg is read and applies the
// elapsed 5 s intervals with the count of that moment (an explicit simplification of the kernel's per-tick sampling).
struct LoadAverage {
  static constexpr uint64_t FixedOne = 1 << 11, IntervalNs = 5'000'000'000;
  uint64_t Avg[3] {};
  uint64_t LastNs {};
  bool Started {};
  void Update(uint64_t NowNs, uint32_t Active) {
    constexpr uint64_t Exp[3] = {1884, 2014, 2037};  // 1/exp(5 s / 1, 5, 15 min)
    if (!Started) {
      Started = true;
      LastNs = NowNs;
      return;
    }
    for (; NowNs - LastNs >= IntervalNs; LastNs += IntervalNs)
      for (int I = 0; I < 3; ++I) Avg[I] = (Avg[I] * Exp[I] + uint64_t {Active} * FixedOne * (FixedOne - Exp[I]) + FixedOne / 2) / FixedOne;
  }
};

// /proc/loadavg: the three averages, runnable/total tasks, the last pid handed out.
inline std::string FormatLoadavg(const LoadAverage& L, uint32_t Running, uint32_t Total, uint32_t LastPid) {
  char Line[160];
  const auto Int = [](uint64_t X) { return static_cast<unsigned>(X >> 11); };
  const auto Frac = [](uint64_t X) { return static_cast<unsigned>(((X & 2047) * 100) >> 11); };
  std::snprintf(Line, sizeof(Line), "%u.%02u %u.%02u %u.%02u %u/%u %u\n", Int(L.Avg[0]), Frac(L.Avg[0]), Int(L.Avg[1]), Frac(L.Avg[1]),
                Int(L.Avg[2]), Frac(L.Avg[2]), Running, Total, LastPid);
  return Line;
}

// /proc/meminfo (kB). The lines procps reads (free, top) plus the usual ones.
struct MemInfo {
  uint64_t TotalKb {}, FreeKb {}, AvailableKb {}, BuffersKb {}, CachedKb {};
};

inline std::string FormatMeminfo(const MemInfo& M) {
  std::string Out;
  char Line[96];
  const auto Add = [&](const char* Name, uint64_t Kb) {
    std::snprintf(Line, sizeof(Line), "%-16s%8llu kB\n", Name, static_cast<unsigned long long>(Kb));
    Out += Line;
  };
  Add("MemTotal:", M.TotalKb);
  Add("MemFree:", M.FreeKb);
  Add("MemAvailable:", M.AvailableKb);
  Add("Buffers:", M.BuffersKb);
  Add("Cached:", M.CachedKb);
  Add("SwapCached:", 0);
  Add("Active:", 0);
  Add("Inactive:", 0);
  Add("SwapTotal:", 0);
  Add("SwapFree:", 0);
  Add("Dirty:", 0);
  Add("Writeback:", 0);
  Add("AnonPages:", M.TotalKb > M.FreeKb ? M.TotalKb - M.FreeKb : 0);
  Add("Mapped:", 0);
  Add("Shmem:", 0);
  Add("Slab:", 0);
  Add("SReclaimable:", 0);
  Add("SUnreclaim:", 0);
  Add("KernelStack:", 0);
  Add("PageTables:", 0);
  Add("CommitLimit:", M.TotalKb);
  Add("Committed_AS:", M.TotalKb > M.FreeKb ? M.TotalKb - M.FreeKb : 0);
  Add("VmallocTotal:", 0);
  Add("VmallocUsed:", 0);
  Add("VmallocChunk:", 0);
  return Out;
}

// /proc/stat. Totals are in clock ticks. The runtime does not split user and system time: busy time is all user time.
struct SystemStat {
  uint32_t Cpus {1};
  uint64_t BusyTicks {}, IdleTicks {};  // summed over the CPUs
  std::vector<uint64_t> CpuBusy, CpuIdle;  // per CPU when known (Cpus entries each), else an even share of the sums
  uint64_t Context {}, BootTime {}, Forks {};
  uint32_t Running {}, Blocked {};
};

inline std::string FormatSystemStat(const SystemStat& S) {
  std::string Out;
  char Line[256];
  const auto Cpu = [&](const char* Name, uint64_t Busy, uint64_t Idle) {
    std::snprintf(Line, sizeof(Line), "%s %llu 0 0 %llu 0 0 0 0 0 0\n", Name, static_cast<unsigned long long>(Busy),
                  static_cast<unsigned long long>(Idle));
    Out += Line;
  };
  Cpu("cpu ", S.BusyTicks, S.IdleTicks);
  const uint32_t Cpus = S.Cpus ? S.Cpus : 1;
  const bool PerCpu = S.CpuBusy.size() == Cpus && S.CpuIdle.size() == Cpus;
  for (uint32_t I = 0; I < Cpus; ++I) {
    const std::string Name = "cpu" + std::to_string(I);
    if (PerCpu) Cpu(Name.c_str(), S.CpuBusy[I], S.CpuIdle[I]);
    else Cpu(Name.c_str(), S.BusyTicks / Cpus + (I < S.BusyTicks % Cpus), S.IdleTicks / Cpus + (I < S.IdleTicks % Cpus));
  }
  std::snprintf(Line, sizeof(Line), "intr 0\nctxt %llu\nbtime %llu\nprocesses %llu\nprocs_running %u\nprocs_blocked %u\nsoftirq 0 0 0 0 0 0 0 0 0 0 0\n",
                static_cast<unsigned long long>(S.Context), static_cast<unsigned long long>(S.BootTime), static_cast<unsigned long long>(S.Forks),
                S.Running, S.Blocked);
  return Out + Line;
}

// /proc/cpuinfo (x86-64 style: the guest is an x86-64 program).
inline std::string FormatCpuinfo(uint32_t Cpus) {
  std::string Out;
  char Line[512];
  for (uint32_t I = 0; I < Cpus; ++I) {
    std::snprintf(Line, sizeof(Line),
                  "processor\t: %u\nvendor_id\t: FEXEmu\ncpu family\t: 6\nmodel\t\t: 1\nmodel name\t: FEX-Emu x86-64 on ARM64\ncpu MHz\t\t: 1000.000\n"
                  "cache size\t: 0 KB\nphysical id\t: 0\nsiblings\t: %u\ncore id\t\t: %u\ncpu cores\t: %u\nflags\t\t: fpu cx8 cmov mmx fxsr sse sse2\n\n",
                  I, Cpus, I, Cpus);
    Out += Line;
  }
  return Out;
}

}  // namespace Switch2Guest::Procfs
