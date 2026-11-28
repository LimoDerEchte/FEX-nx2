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

// /proc/meminfo (kB), the lines of Linux 5.15 meminfo_proc_show in its order. Fields the runtime does not track are 0.
struct MemInfo {
  uint64_t TotalKb {}, FreeKb {}, AvailableKb {}, BuffersKb {}, CachedKb {};
  uint64_t AnonPagesKb {};  // the anonymous pages of every process (RssAnon summed)
  uint64_t ShmemKb {};      // memory-backed tmpfs files (/dev/shm)
  uint64_t CommitLimitKb {}, CommittedKb {};
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
  Add("Active(anon):", 0);
  Add("Inactive(anon):", 0);
  Add("Active(file):", 0);
  Add("Inactive(file):", 0);
  Add("Unevictable:", 0);
  Add("Mlocked:", 0);
  Add("SwapTotal:", 0);
  Add("SwapFree:", 0);
  Add("Dirty:", 0);
  Add("Writeback:", 0);
  Add("AnonPages:", M.AnonPagesKb);
  Add("Mapped:", 0);
  Add("Shmem:", M.ShmemKb);
  Add("KReclaimable:", 0);
  Add("Slab:", 0);
  Add("SReclaimable:", 0);
  Add("SUnreclaim:", 0);
  Add("KernelStack:", 0);
  Add("PageTables:", 0);
  Add("NFS_Unstable:", 0);
  Add("Bounce:", 0);
  Add("WritebackTmp:", 0);
  Add("CommitLimit:", M.CommitLimitKb);
  Add("Committed_AS:", M.CommittedKb);
  Add("VmallocTotal:", 0);
  Add("VmallocUsed:", 0);
  Add("VmallocChunk:", 0);
  Add("Percpu:", 0);
  Add("HardwareCorrupted:", 0);
  Add("AnonHugePages:", 0);
  Add("ShmemHugePages:", 0);
  Add("ShmemPmdMapped:", 0);
  Add("FileHugePages:", 0);
  Add("FilePmdMapped:", 0);
  Out += "HugePages_Total:       0\nHugePages_Free:        0\nHugePages_Rsvd:        0\nHugePages_Surp:        0\n";
  Add("Hugepagesize:", 2048);
  Add("Hugetlb:", 0);
  Add("DirectMap4k:", 0);
  Add("DirectMap2M:", 0);
  Add("DirectMap1G:", 0);
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

// ---- /proc/cpuinfo (arch/x86/kernel/cpu/proc.c) ----

// The CPUID leaves the runtime reads from FEX for the guest: leaf 0, 1, 7 (sub 0), 0xd (sub 1), 0x80000001, 0x80000008
// and the brand string (0x80000002..4). Regs are EAX, EBX, ECX, EDX.
struct CpuidRegs {
  uint32_t Eax {}, Ebx {}, Ecx {}, Edx {};
};
struct CpuidInfo {
  CpuidRegs Leaf0, Leaf1, Leaf7, LeafD1, Ext1, Ext8, Brand[3];
};

// A flag name per bit of one CPUID register, 32 entries, null for a bit Linux does not name (cpufeatures.h).
// The words of x86_capability that come from the leaves above, in the order /proc/cpuinfo prints them (word 0 leaf 1 EDX,
// 1 0x80000001 EDX, 4 leaf 1 ECX, 6 0x80000001 ECX, 9 leaf 7 EBX, 10 0xd.1 EAX, 13 0x80000008 EBX, 16 leaf 7 ECX, 18 leaf 7
// EDX). The synthetic flags Linux adds itself (constant_tsc, rep_good, nopl, cpuid, ...) are not printed.
inline constexpr const char* FlagsLeaf1Edx[32] = {
    "fpu", "vme", "de", "pse", "tsc", "msr", "pae", "mce", "cx8", "apic", nullptr, "sep", "mtrr", "pge", "mca", "cmov",
    "pat", "pse36", "pn", "clflush", nullptr, "dts", "acpi", "mmx", "fxsr", "sse", "sse2", "ss", "ht", "tm", "ia64", "pbe"};
inline constexpr const char* FlagsExt1Edx[32] = {
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "syscall", nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, nullptr, "mp", "nx", nullptr, "mmxext", nullptr, nullptr, "fxsr_opt", "pdpe1gb", "rdtscp",
    nullptr, "lm", "3dnowext", "3dnow"};
inline constexpr const char* FlagsLeaf1Ecx[32] = {
    "pni", "pclmulqdq", "dtes64", "monitor", "ds_cpl", "vmx", "smx", "est", "tm2", "ssse3", "cid", "sdbg", "fma", "cx16", "xtpr",
    "pdcm", nullptr, "pcid", "dca", "sse4_1", "sse4_2", "x2apic", "movbe", "popcnt", "tsc_deadline_timer", "aes", "xsave", nullptr,
    "avx", "f16c", "rdrand", "hypervisor"};
inline constexpr const char* FlagsExt1Ecx[32] = {
    "lahf_lm", "cmp_legacy", "svm", "extapic", "cr8_legacy", "abm", "sse4a", "misalignsse", "3dnowprefetch", "osvw", "ibs", "xop",
    "skinit", "wdt", nullptr, "lwp", "fma4", "tce", nullptr, "nodeid_msr", nullptr, "tbm", "topoext", "perfctr_core", "perfctr_nb",
    nullptr, "bpext", "ptsc", "perfctr_llc", "mwaitx", nullptr, nullptr};
inline constexpr const char* FlagsLeaf7Ebx[32] = {
    "fsgsbase", "tsc_adjust", "sgx", "bmi1", "hle", "avx2", nullptr, "smep", "bmi2", "erms", "invpcid", "rtm", "cqm", nullptr, "mpx",
    "rdt_a", "avx512f", "avx512dq", "rdseed", "adx", "smap", "avx512ifma", nullptr, "clflushopt", "clwb", "intel_pt", "avx512pf",
    "avx512er", "avx512cd", "sha_ni", "avx512bw", "avx512vl"};
inline constexpr const char* FlagsLeafD1Eax[32] = {"xsaveopt", "xsavec", "xgetbv1", "xsaves"};
inline constexpr const char* FlagsExt8Ebx[32] = {
    "clzero", "irperf", "xsaveerptr", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "wbnoinvd", nullptr, nullptr, "ibpb",
    nullptr, "ibrs", "stibp"};
inline constexpr const char* FlagsLeaf7Ecx[32] = {
    "prefetchwt1", "avx512vbmi", "umip", "pku", "ospke", "waitpkg", "avx512_vbmi2", nullptr, "gfni", "vaes", "vpclmulqdq",
    "avx512_vnni", "avx512_bitalg", "tme", "avx512_vpopcntdq", nullptr, "la57", nullptr, nullptr, nullptr, nullptr, nullptr, "rdpid",
    nullptr, nullptr, "cldemote", nullptr, "movdiri", "movdir64b", "enqcmd", "sgx_lc"};
inline constexpr const char* FlagsLeaf7Edx[32] = {
    nullptr, nullptr, "avx512_4vnniw", "avx512_4fmaps", "fsrm", nullptr, nullptr, nullptr, "avx512_vp2intersect", nullptr,
    "md_clear", nullptr, nullptr, nullptr, "serialize", nullptr, "tsxldtrk", nullptr, "pconfig", "arch_lbr", nullptr, nullptr,
    "amx_bf16", "avx512_fp16", "amx_tile", "amx_int8", nullptr, nullptr, "flush_l1d", "arch_capabilities", nullptr, nullptr};

// The flags line: " name" for every set bit that Linux names, in word order then bit order.
inline std::string CpuFlags(const CpuidInfo& C) {
  struct Word {
    uint32_t Value;
    const char* const* Names;
  };
  const Word Words[] = {{C.Leaf1.Edx, FlagsLeaf1Edx}, {C.Ext1.Edx, FlagsExt1Edx},   {C.Leaf1.Ecx, FlagsLeaf1Ecx},
                        {C.Ext1.Ecx, FlagsExt1Ecx},   {C.Leaf7.Ebx, FlagsLeaf7Ebx}, {C.LeafD1.Eax, FlagsLeafD1Eax},
                        {C.Ext8.Ebx, FlagsExt8Ebx},   {C.Leaf7.Ecx, FlagsLeaf7Ecx}, {C.Leaf7.Edx, FlagsLeaf7Edx}};
  std::string Out;
  for (const auto& W : Words) {
    for (int Bit = 0; Bit < 32; ++Bit) {
      if ((W.Value >> Bit & 1) && W.Names[Bit]) Out += std::string(" ") + W.Names[Bit];
    }
  }
  return Out;
}

// The 12 bytes of the vendor string: EBX, EDX, ECX of leaf 0.
inline std::string CpuVendor(const CpuidInfo& C) {
  std::string Out;
  for (const uint32_t Reg : {C.Leaf0.Ebx, C.Leaf0.Edx, C.Leaf0.Ecx}) {
    for (int I = 0; I < 4; ++I) Out.push_back(static_cast<char>(Reg >> (8 * I) & 0xff));
  }
  return Out;
}

// The 48 bytes of the brand string without leading spaces and the padding NULs.
inline std::string CpuBrand(const CpuidInfo& C) {
  std::string Out;
  for (const auto& R : C.Brand) {
    for (const uint32_t Reg : {R.Eax, R.Ebx, R.Ecx, R.Edx}) {
      for (int I = 0; I < 4; ++I) Out.push_back(static_cast<char>(Reg >> (8 * I) & 0xff));
    }
  }
  while (!Out.empty() && Out.back() == '\0') Out.pop_back();
  const auto First = Out.find_first_not_of(' ');
  return First == std::string::npos ? std::string {} : Out.substr(First);
}

// /proc/cpuinfo, one block per CPU (show_cpuinfo). Family, model and stepping decode leaf 1 as Linux does. The values the
// console cannot know are fixed: microcode 0x0, cpu MHz 1000.000, bogomips 2000.00, cache size 512 KB, address sizes
// 48/48, one package whose cores are the CPUs, and no bugs.
inline std::string FormatCpuinfo(const CpuidInfo& C, uint32_t Cpus) {
  const uint32_t Eax = C.Leaf1.Eax;
  uint32_t Family = Eax >> 8 & 0xf, Model = Eax >> 4 & 0xf;
  if (Family == 0xf) Family += Eax >> 20 & 0xff;
  if (Family >= 6) Model += (Eax >> 16 & 0xf) << 4;
  const unsigned Stepping = Eax & 0xf;
  const unsigned Clflush = (C.Leaf1.Ebx >> 8 & 0xff) * 8;
  const std::string Vendor = CpuVendor(C), Brand = CpuBrand(C), Flags = CpuFlags(C);
  std::string Out;
  char Line[512];
  for (uint32_t I = 0; I < Cpus; ++I) {
    std::snprintf(Line, sizeof(Line), "processor\t: %u\nvendor_id\t: ", I);
    Out += Line + Vendor;
    std::snprintf(Line, sizeof(Line), "\ncpu family\t: %u\nmodel\t\t: %u\nmodel name\t: ", Family, Model);
    Out += Line + Brand;
    std::snprintf(Line, sizeof(Line),
                  "\nstepping\t: %u\nmicrocode\t: 0x0\ncpu MHz\t\t: 1000.000\ncache size\t: 512 KB\nphysical id\t: 0\n"
                  "siblings\t: %u\ncore id\t\t: %u\ncpu cores\t: %u\napicid\t\t: %u\ninitial apicid\t: %u\nfpu\t\t: yes\n"
                  "fpu_exception\t: yes\ncpuid level\t: %u\nwp\t\t: yes\nflags\t\t:",
                  Stepping, Cpus, I, Cpus, I, I, C.Leaf0.Eax);
    Out += Line + Flags;
    std::snprintf(Line, sizeof(Line),
                  "\nbugs\t\t:\nbogomips\t: 2000.00\nclflush size\t: %u\ncache_alignment\t: %u\n"
                  "address sizes\t: 48 bits physical, 48 bits virtual\npower management:\n\n",
                  Clflush, Clflush);
    Out += Line;
  }
  return Out;
}

// ---- /proc/<pid>/status (proc_pid_status) ----

// "R (running)" and the other task_state_array names.
inline const char* StateName(char State) {
  switch (State) {
  case 'R': return "R (running)";
  case 'S': return "S (sleeping)";
  case 'D': return "D (disk sleep)";
  case 'T': return "T (stopped)";
  case 't': return "t (tracing stop)";
  case 'Z': return "Z (zombie)";
  case 'X': return "X (dead)";
  default: return "R (running)";
  }
}

// A bit mask as %*pb prints it for Bits bits: 32-bit hex chunks, the first as wide as its bits need, joined with commas.
inline std::string FormatBitmap(uint64_t Mask, uint32_t Bits) {
  if (Bits == 0) return "0";
  if (Bits < 64) Mask &= (uint64_t {1} << Bits) - 1;
  const uint32_t Chunks = (Bits + 31) / 32;
  std::string Out;
  char Text[16];
  for (uint32_t Index = Chunks; Index-- > 0;) {
    const uint32_t Value = static_cast<uint32_t>(Mask >> (32 * Index));
    uint32_t Width = 8;
    if (Index == Chunks - 1) Width = ((Bits - 32 * Index) + 3) / 4;
    std::snprintf(Text, sizeof(Text), "%0*x", static_cast<int>(Width), Value);
    Out += (Out.empty() ? "" : ",") + std::string(Text);
  }
  return Out;
}

// A bit mask as %*pbl prints it: ranges "0-3,5". Empty mask gives an empty string.
inline std::string FormatBitList(uint64_t Mask, uint32_t Bits) {
  std::string Out;
  for (uint32_t Bit = 0; Bit < Bits && Bit < 64;) {
    if (!(Mask >> Bit & 1)) {
      ++Bit;
      continue;
    }
    uint32_t End = Bit;
    while (End + 1 < Bits && End + 1 < 64 && (Mask >> (End + 1) & 1)) ++End;
    if (!Out.empty()) Out += ",";
    Out += std::to_string(Bit);
    if (End > Bit) Out += "-" + std::to_string(End);
    Bit = End + 1;
  }
  return Out;
}

struct Status {
  std::string Name;
  uint32_t Umask {};
  char State {'S'};
  uint32_t Tgid {}, Pid {}, PPid {}, TracerPid {};
  uint32_t Uid[4] {}, Gid[4] {};
  uint32_t FdSize {};
  std::vector<uint32_t> Groups;
  std::string NStgid, NSpid, NSpgid, NSsid;  // tab-separated numbers per namespace level
  // Kilobytes. VmPeak and VmHWM are the highest values seen since exec.
  uint64_t VmPeak {}, VmSize {}, VmLck {}, VmPin {}, VmHwm {}, VmRss {}, RssAnon {}, RssFile {}, RssShmem {}, VmData {}, VmStk {},
      VmExe {}, VmLib {}, VmPte {}, VmSwap {};
  uint32_t Threads {1};
  uint64_t SigQueued {}, SigLimit {};
  uint64_t SigPnd {}, ShdPnd {}, SigBlk {}, SigIgn {}, SigCgt {};
  uint64_t CapInh {}, CapPrm {}, CapEff {}, CapBnd {}, CapAmb {};
  uint32_t NoNewPrivs {}, Seccomp {}, SeccompFilters {};
  uint64_t CpusAllowed {};
  uint32_t Cpus {1};  // the CPUs the guest sees
  uint64_t VoluntarySwitches {}, InvoluntarySwitches {};
};

// The lines of Linux 5.15 proc_pid_status in order. Mems_allowed is node 0 only, Speculation_Store_Bypass is the
// "thread vulnerable" of a task with no prctl control, CoreDumping and THP_enabled are 0.
inline std::string FormatStatus(const Status& S) {
  std::string Out;
  char Line[256];
  const auto Add = [&](const char* Format, auto... Values) {
    std::snprintf(Line, sizeof(Line), Format, Values...);
    Out += Line;
  };
  const auto Hex = [](uint64_t V) { return static_cast<unsigned long long>(V); };
  const auto Kb = [](uint64_t V) { return static_cast<unsigned long long>(V); };
  Out += "Name:\t" + S.Name + "\n";
  Add("Umask:\t%#04o\nState:\t%s\n", S.Umask, StateName(S.State));
  Add("Tgid:\t%u\nNgid:\t0\nPid:\t%u\nPPid:\t%u\nTracerPid:\t%u\n", S.Tgid, S.Pid, S.PPid, S.TracerPid);
  Add("Uid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nFDSize:\t%u\n", S.Uid[0], S.Uid[1], S.Uid[2], S.Uid[3], S.Gid[0], S.Gid[1], S.Gid[2],
      S.Gid[3], S.FdSize);
  Out += "Groups:\t";
  for (const auto Group : S.Groups) Out += std::to_string(Group) + " ";
  Out += "\nNStgid:\t" + S.NStgid + "\nNSpid:\t" + S.NSpid + "\nNSpgid:\t" + S.NSpgid + "\nNSsid:\t" + S.NSsid + "\n";
  Add("VmPeak:\t%8llu kB\nVmSize:\t%8llu kB\nVmLck:\t%8llu kB\nVmPin:\t%8llu kB\nVmHWM:\t%8llu kB\nVmRSS:\t%8llu kB\n", Kb(S.VmPeak),
      Kb(S.VmSize), Kb(S.VmLck), Kb(S.VmPin), Kb(S.VmHwm), Kb(S.VmRss));
  Add("RssAnon:\t%8llu kB\nRssFile:\t%8llu kB\nRssShmem:\t%8llu kB\nVmData:\t%8llu kB\nVmStk:\t%8llu kB\nVmExe:\t%8llu kB\n", Kb(S.RssAnon),
      Kb(S.RssFile), Kb(S.RssShmem), Kb(S.VmData), Kb(S.VmStk), Kb(S.VmExe));
  Add("VmLib:\t%8llu kB\nVmPTE:\t%8llu kB\nVmSwap:\t%8llu kB\nHugetlbPages:\t%8llu kB\nCoreDumping:\t0\nTHP_enabled:\t0\n", Kb(S.VmLib),
      Kb(S.VmPte), Kb(S.VmSwap), Kb(0));
  Add("Threads:\t%u\nSigQ:\t%llu/%llu\n", S.Threads, Hex(S.SigQueued), Hex(S.SigLimit));
  Add("SigPnd:\t%016llx\nShdPnd:\t%016llx\nSigBlk:\t%016llx\nSigIgn:\t%016llx\nSigCgt:\t%016llx\n", Hex(S.SigPnd), Hex(S.ShdPnd), Hex(S.SigBlk),
      Hex(S.SigIgn), Hex(S.SigCgt));
  Add("CapInh:\t%016llx\nCapPrm:\t%016llx\nCapEff:\t%016llx\nCapBnd:\t%016llx\nCapAmb:\t%016llx\n", Hex(S.CapInh), Hex(S.CapPrm), Hex(S.CapEff),
      Hex(S.CapBnd), Hex(S.CapAmb));
  Add("NoNewPrivs:\t%u\nSeccomp:\t%u\nSeccomp_filters:\t%u\nSpeculation_Store_Bypass:\tthread vulnerable\n", S.NoNewPrivs, S.Seccomp,
      S.SeccompFilters);
  Out += "Cpus_allowed:\t" + FormatBitmap(S.CpusAllowed, S.Cpus) + "\nCpus_allowed_list:\t" + FormatBitList(S.CpusAllowed, S.Cpus) + "\n";
  Out += "Mems_allowed:\t1\nMems_allowed_list:\t0\n";
  Add("voluntary_ctxt_switches:\t%llu\nnonvoluntary_ctxt_switches:\t%llu\n", Hex(S.VoluntarySwitches), Hex(S.InvoluntarySwitches));
  return Out;
}

// ---- /proc/<pid>/limits (proc_pid_limits) ----

// The sixteen RLIMIT_* rows in resource order. A value of ~0 is "unlimited".
inline std::string FormatLimits(const uint64_t (&Limits)[16][2]) {
  static constexpr const char* Names[16] = {"Max cpu time",        "Max file size",       "Max data size",        "Max stack size",
                                            "Max core file size",  "Max resident set",    "Max processes",        "Max open files",
                                            "Max locked memory",   "Max address space",   "Max file locks",       "Max pending signals",
                                            "Max msgqueue size",   "Max nice priority",   "Max realtime priority", "Max realtime timeout"};
  static constexpr const char* Units[16] = {"seconds", "bytes", "bytes", "bytes", "bytes", "bytes", "processes", "files",
                                            "bytes",   "bytes", "locks", "signals", "bytes", nullptr, nullptr, "us"};
  std::string Out;
  char Line[160];
  std::snprintf(Line, sizeof(Line), "%-25s %-20s %-20s %-10s\n", "Limit", "Soft Limit", "Hard Limit", "Units");
  Out += Line;
  for (int I = 0; I < 16; ++I) {
    char Soft[24], Hard[24];
    const auto Number = [](char* To, uint64_t V) {
      if (V == ~uint64_t {0}) std::snprintf(To, 24, "unlimited");
      else std::snprintf(To, 24, "%llu", static_cast<unsigned long long>(V));
    };
    Number(Soft, Limits[I][0]);
    Number(Hard, Limits[I][1]);
    std::snprintf(Line, sizeof(Line), "%-25s %-20s %-20s ", Names[I], Soft, Hard);
    Out += Line;
    if (Units[I]) {
      std::snprintf(Line, sizeof(Line), "%-10s\n", Units[I]);
      Out += Line;
    } else {
      Out += "\n";
    }
  }
  return Out;
}

// ---- /proc/<pid>/io (do_io_accounting) ----

struct IoCounters {
  uint64_t Rchar {}, Wchar {}, Syscr {}, Syscw {};  // read_bytes, write_bytes, cancelled_write_bytes are always 0
};

inline std::string FormatIo(const IoCounters& C) {
  char Text[256];
  std::snprintf(Text, sizeof(Text),
                "rchar: %llu\nwchar: %llu\nsyscr: %llu\nsyscw: %llu\nread_bytes: 0\nwrite_bytes: 0\ncancelled_write_bytes: 0\n",
                static_cast<unsigned long long>(C.Rchar), static_cast<unsigned long long>(C.Wchar),
                static_cast<unsigned long long>(C.Syscr), static_cast<unsigned long long>(C.Syscw));
  return Text;
}

// ---- /proc/<pid>/sched (proc_sched_show_task) ----

// Linux prio_to_weight: the load weight of a nice value (-20..19).
inline uint32_t NiceWeight(int32_t Nice) {
  static constexpr uint32_t Weights[40] = {88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
                                           9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
                                           1024,  820,   655,   526,   423,   335,   272,   215,   172,   137,
                                           110,   87,    70,    56,    45,    36,    29,    23,    18,    15};
  if (Nice < -20) Nice = -20;
  if (Nice > 19) Nice = 19;
  return Weights[Nice + 20];
}

struct Sched {
  std::string Comm;
  uint32_t Pid {}, Threads {1};
  uint64_t ExecStartNs {}, SumExecRuntimeNs {};
  uint64_t Switches {}, VoluntarySwitches {}, InvoluntarySwitches {};
  uint32_t Weight {1024};
  int32_t Policy {}, Prio {120};
  int64_t ClockDeltaNs {};
};

// The header line, a 57 dash line, then P() lines "%-45s:%21Ld" and PN() lines "%-45s:%14Ld.%06ld" (ns split in ms and ns).
inline std::string FormatSched(const Sched& S) {
  std::string Out;
  char Line[160];
  std::snprintf(Line, sizeof(Line), "%s (%u, #threads: %u)\n", S.Comm.c_str(), S.Pid, S.Threads);
  Out += Line;
  Out += std::string(57, '-') + "\n";
  const auto Number = [&](const char* Name, long long V) {
    std::snprintf(Line, sizeof(Line), "%-45s:%21lld\n", Name, V);
    Out += Line;
  };
  const auto Nanos = [&](const char* Name, uint64_t Ns) {
    std::snprintf(Line, sizeof(Line), "%-45s:%14lld.%06ld\n", Name, static_cast<long long>(Ns / 1'000'000), static_cast<long>(Ns % 1'000'000));
    Out += Line;
  };
  Nanos("se.exec_start", S.ExecStartNs);
  Nanos("se.sum_exec_runtime", S.SumExecRuntimeNs);
  Number("nr_switches", static_cast<long long>(S.Switches));
  Number("nr_voluntary_switches", static_cast<long long>(S.VoluntarySwitches));
  Number("nr_involuntary_switches", static_cast<long long>(S.InvoluntarySwitches));
  Number("se.load.weight", S.Weight);
  Number("policy", S.Policy);
  Number("prio", S.Prio);
  Number("clock-delta", S.ClockDeltaNs);
  return Out;
}

// ---- /proc/<pid>/oom_score_adj and oom_adj (oom_adjust_write) ----

constexpr int32_t OomScoreAdjMin = -1000, OomScoreAdjMax = 1000;
constexpr int32_t OomAdjustMax = 15, OomDisable = -17;

// oom_adj as read: the score scaled to the legacy range.
inline int32_t OomAdjOf(int32_t Score) {
  if (Score == OomScoreAdjMax) return OomAdjustMax;
  return Score * -OomDisable / OomScoreAdjMax;
}
// oom_adj as written (already range checked to -17..15): the score it means.
inline int32_t OomScoreOf(int32_t Adj) {
  if (Adj == OomAdjustMax) return OomScoreAdjMax;
  return Adj * OomScoreAdjMax / -OomDisable;
}

// ---- /proc/<pid>/fdinfo/<fd> (seq_show and the *_show_fdinfo of each file kind) ----

inline std::string FormatFdinfo(int64_t Pos, uint32_t Flags, int Mount, uint64_t Inode) {
  char Text[160];
  std::snprintf(Text, sizeof(Text), "pos:\t%lld\nflags:\t0%o\nmnt_id:\t%d\nino:\t%llu\n", static_cast<long long>(Pos), Flags, Mount,
                static_cast<unsigned long long>(Inode));
  return Text;
}
inline std::string FormatEventfdInfo(uint64_t Count) {
  char Text[64];
  std::snprintf(Text, sizeof(Text), "eventfd-count: %16llx\n", static_cast<unsigned long long>(Count));
  return Text;
}
inline std::string FormatEpollInfo(int32_t Fd, uint32_t Events, uint64_t Data, int64_t Pos, uint64_t Inode, uint32_t Sdev) {
  char Text[160];
  std::snprintf(Text, sizeof(Text), "tfd: %8d events: %8x data: %16llx  pos:%lld ino:%llx sdev:%x\n", Fd, Events,
                static_cast<unsigned long long>(Data), static_cast<long long>(Pos), static_cast<unsigned long long>(Inode), Sdev);
  return Text;
}
// The fhandle part Linux appends to an inotify line is not printed: the runtime has no file handles.
inline std::string FormatInotifyInfo(int32_t Wd, uint64_t Inode, uint32_t Sdev, uint32_t Mask) {
  char Text[128];
  std::snprintf(Text, sizeof(Text), "inotify wd:%x ino:%llx sdev:%x mask:%x ignored_mask:0\n", static_cast<unsigned>(Wd),
                static_cast<unsigned long long>(Inode), Sdev, Mask);
  return Text;
}
inline std::string FormatSignalfdInfo(uint64_t Mask) {
  char Text[64];
  std::snprintf(Text, sizeof(Text), "sigmask:\t%016llx\n", static_cast<unsigned long long>(Mask));
  return Text;
}
inline std::string FormatTimerfdInfo(int32_t ClockId, uint64_t Ticks, uint32_t SetFlags, uint64_t ValueSec, uint64_t ValueNsec,
                                     uint64_t IntervalSec, uint64_t IntervalNsec) {
  char Text[256];
  std::snprintf(Text, sizeof(Text),
                "clockid: %d\nticks: %llu\nsettime flags: 0%o\nit_value: (%llu, %llu)\nit_interval: (%llu, %llu)\n", ClockId,
                static_cast<unsigned long long>(Ticks), SetFlags, static_cast<unsigned long long>(ValueSec),
                static_cast<unsigned long long>(ValueNsec), static_cast<unsigned long long>(IntervalSec),
                static_cast<unsigned long long>(IntervalNsec));
  return Text;
}

// ---- Small files ----

// /proc/<pid>/personality: the stored value as %08x.
inline std::string FormatPersonality(uint32_t Value) {
  char Text[24];
  std::snprintf(Text, sizeof(Text), "%08x\n", Value);
  return Text;
}

// /proc/<pid>/syscall (proc_pid_syscall): "running" while it executes, else the number, six arguments, sp and pc in hex;
// "-1 sp pc" for a task that is blocked outside a system call.
inline std::string FormatSyscall(bool Running, int64_t Number, const uint64_t (&Args)[6], uint64_t Sp, uint64_t Pc) {
  if (Running) return "running\n";
  char Text[256];
  if (Number < 0) {
    std::snprintf(Text, sizeof(Text), "-1 0x%llx 0x%llx\n", static_cast<unsigned long long>(Sp), static_cast<unsigned long long>(Pc));
    return Text;
  }
  std::snprintf(Text, sizeof(Text), "%lld 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx\n", static_cast<long long>(Number),
                static_cast<unsigned long long>(Args[0]), static_cast<unsigned long long>(Args[1]), static_cast<unsigned long long>(Args[2]),
                static_cast<unsigned long long>(Args[3]), static_cast<unsigned long long>(Args[4]), static_cast<unsigned long long>(Args[5]),
                static_cast<unsigned long long>(Sp), static_cast<unsigned long long>(Pc));
  return Text;
}

// /proc/<pid>/mountstats (show_vfsstat): one line per mount, "device <src> mounted on <path> with fstype <type>".
inline std::string FormatMountstat(const std::string& Source, const std::string& Path, const std::string& Type) {
  return "device " + (Source.empty() ? std::string {"none"} : Source) + " mounted on " + Path + " with fstype " + Type + "\n";
}

// /proc/<pid>/task/<tid>/children: the child pids, space separated, each followed by a space.
inline std::string FormatChildren(const std::vector<uint32_t>& Pids) {
  std::string Out;
  for (const auto Pid : Pids) Out += std::to_string(Pid) + " ";
  return Out;
}

// ---- System files ----

// /proc/version: the same release and version strings uname reports.
inline std::string FormatVersion(std::string_view Release, std::string_view Version) {
  return "Linux version " + std::string(Release) + " (fex@switch2) (gcc version 12.2.0) " + std::string(Version) + "\n";
}

// /proc/filesystems: "nodev" types have no block device. Types are listed in the order given.
struct FilesystemType {
  std::string_view Name;
  bool Nodev;
};
inline std::string FormatFilesystems(const std::vector<FilesystemType>& Types) {
  std::string Out;
  for (const auto& Type : Types) Out += std::string(Type.Nodev ? "nodev" : "") + "\t" + std::string(Type.Name) + "\n";
  return Out;
}

// /proc/swaps: no swap, header only. /proc/partitions: the header and its blank line.
inline std::string FormatSwaps() { return "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n"; }
inline std::string FormatPartitions() { return "major minor  #blocks  name\n\n"; }

// /proc/devices: the character majors the runtime serves and an empty block section.
struct CharMajor {
  uint32_t Major;
  std::string_view Name;
};
inline std::string FormatDevices(const std::vector<CharMajor>& Majors) {
  std::string Out = "Character devices:\n";
  char Line[96];
  for (const auto& Major : Majors) {
    std::snprintf(Line, sizeof(Line), "%3u %s\n", Major.Major, std::string(Major.Name).c_str());
    Out += Line;
  }
  return Out + "\nBlock devices:\n";
}

}  // namespace Switch2Guest::Procfs
