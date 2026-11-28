// SPDX-License-Identifier: MIT
#include "GuestProcfs.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(Cond) do { if (!(Cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #Cond); std::exit(1); } } while (0)

using namespace Switch2Guest::Procfs;

static int Fields(const std::string& Text) {  // the fields after "(comm)" plus pid and comm, as procps counts them
  const auto Close = Text.rfind(')');
  int Count = 2;
  size_t At = Close + 1;
  while (At < Text.size() && Text[At] != '\n') {
    while (Text[At] == ' ') ++At;
    if (Text[At] == '\n') break;
    ++Count;
    while (Text[At] != ' ' && Text[At] != '\n') ++At;
  }
  return Count;
}

int main() {
  Stat S;
  S.Pid = 112;
  S.Comm = "bash (x)";  // a ')' in the name: the parser cuts at the last one
  S.State = 'S';
  S.Ppid = 1;
  S.Pgrp = 112;
  S.Session = 112;
  S.TtyNr = (136 << 8) | 3;
  S.Tpgid = 112;
  S.Utime = 7;
  S.Stime = 2;
  S.Cutime = 3;
  S.Priority = 20;
  S.Nice = 0;
  S.Threads = 4;
  S.StartTime = 1234;
  S.Vsize = 8192000;
  S.RssPages = 321;
  S.ExitCode = 0;
  const auto Line = FormatStat(S);
  CHECK(Line.rfind("112 (bash (x)) S 1 112 112 34819 112 0 ", 0) == 0);
  CHECK(Line.back() == '\n' && Fields(Line) == 52);
  // field 14 is utime, 15 stime, 16 cutime, 20 num_threads, 22 starttime, 23 vsize, 24 rss
  const char* Cursor = std::strrchr(Line.c_str(), ')') + 1;
  long Values[53] {};
  for (int N = 3; N <= 52; ++N) {
    while (*Cursor == ' ') ++Cursor;
    if (N == 3) {
      CHECK(*Cursor == 'S');
      while (*Cursor != ' ') ++Cursor;
      continue;
    }
    char* End;
    Values[N] = std::strtol(Cursor, &End, 10);
    Cursor = End;
  }
  CHECK(Values[14] == 7 && Values[15] == 2 && Values[16] == 3 && Values[18] == 20 && Values[19] == 0 && Values[20] == 4);
  CHECK(Values[22] == 1234 && Values[23] == 8192000 && Values[24] == 321 && Values[38] == 17 && Values[52] == 0);

  CHECK(FormatStatm(2000, 321, 0, 0, 5) == "2000 321 0 0 0 5 0\n");
  CHECK(FormatNulList({"ls", "-l"}) == std::string("ls\0-l\0", 6) && FormatNulList({}).empty());
  CHECK(FormatUptime(12'345'678'901ull, 3'000'000'000ull) == "12.34 3.00\n");

  LoadAverage L;
  L.Update(0, 3);                     // starts the clock
  CHECK(FormatLoadavg(L, 1, 5, 200) == "0.00 0.00 0.00 1/5 200\n");
  L.Update(60'000'000'000ull, 3);     // 12 intervals with three runnable tasks
  const auto Load = FormatLoadavg(L, 1, 5, 200);
  CHECK(Load.rfind("1.", 0) == 0 || Load.rfind("2.", 0) == 0);  // the 1 minute average moved most of the way to 3
  CHECK(L.Avg[0] > L.Avg[1] && L.Avg[1] > L.Avg[2] && L.Avg[2] > 0);

  MemInfo M;
  M.TotalKb = 4096;
  M.FreeKb = 1000;
  M.AvailableKb = 1000;
  const auto Mem = FormatMeminfo(M);
  CHECK(Mem.rfind("MemTotal:           4096 kB\nMemFree:            1000 kB\nMemAvailable:       1000 kB\n", 0) == 0);
  CHECK(Mem.find("SwapTotal:") != std::string::npos);

  SystemStat Sys;
  Sys.Cpus = 3;
  Sys.BusyTicks = 10;
  Sys.IdleTicks = 100;
  Sys.Context = 55;
  Sys.BootTime = 1700000000;
  Sys.Forks = 9;
  Sys.Running = 2;
  Sys.Blocked = 1;
  const auto Stat_ = FormatSystemStat(Sys);
  CHECK(Stat_.rfind("cpu  10 0 0 100 0 0 0 0 0 0\ncpu0 4 0 0 34 0 0 0 0 0 0\ncpu1 3 0 0 33 0 0 0 0 0 0\ncpu2 3 0 0 33", 0) == 0);
  CHECK(Stat_.find("\nctxt 55\nbtime 1700000000\nprocesses 9\nprocs_running 2\nprocs_blocked 1\n") != std::string::npos);
  Sys.CpuBusy = {7, 0, 3};
  Sys.CpuIdle = {26, 33, 41};
  CHECK(FormatSystemStat(Sys).rfind("cpu  10 0 0 100 0 0 0 0 0 0\ncpu0 7 0 0 26 0 0 0 0 0 0\ncpu1 0 0 0 33 0 0 0 0 0 0\ncpu2 3 0 0 41", 0) == 0);
  // meminfo: all Linux 5.15 fields in order.
  M.AnonPagesKb = 77;
  M.ShmemKb = 5;
  M.CommitLimitKb = 4096;
  M.CommittedKb = 123;
  const auto Full = FormatMeminfo(M);
  const char* Order[] = {"MemTotal:", "MemFree:", "MemAvailable:", "Buffers:", "Cached:", "SwapCached:", "Active:", "Inactive:",
                         "Active(anon):", "Inactive(anon):", "Active(file):", "Inactive(file):", "Unevictable:", "Mlocked:",
                         "SwapTotal:", "SwapFree:", "Dirty:", "Writeback:", "AnonPages:", "Mapped:", "Shmem:", "KReclaimable:",
                         "Slab:", "SReclaimable:", "SUnreclaim:", "KernelStack:", "PageTables:", "NFS_Unstable:", "Bounce:",
                         "WritebackTmp:", "CommitLimit:", "Committed_AS:", "VmallocTotal:", "VmallocUsed:", "VmallocChunk:",
                         "Percpu:", "HardwareCorrupted:", "AnonHugePages:", "ShmemHugePages:", "ShmemPmdMapped:", "FileHugePages:",
                         "FilePmdMapped:", "HugePages_Total:", "HugePages_Free:", "HugePages_Rsvd:", "HugePages_Surp:",
                         "Hugepagesize:", "Hugetlb:", "DirectMap4k:", "DirectMap2M:", "DirectMap1G:"};
  size_t At = 0;
  for (const auto* Name : Order) {
    const auto Found = Full.find(std::string(Name), At);
    CHECK(Found != std::string::npos && (Found == 0 || Full[Found - 1] == '\n'));
    At = Found;
  }
  CHECK(Full.find("AnonPages:            77 kB\n") != std::string::npos);
  CHECK(Full.find("Shmem:                 5 kB\n") != std::string::npos);
  CHECK(Full.find("CommitLimit:        4096 kB\nCommitted_AS:        123 kB\n") != std::string::npos);
  CHECK(Full.find("\nHugePages_Total:       0\nHugePages_Free:        0\nHugePages_Rsvd:        0\nHugePages_Surp:        0\n"
                  "Hugepagesize:       2048 kB\nHugetlb:               0 kB\n") != std::string::npos);

  // cpuinfo from a fake CPUID: a Zen-like part with a few flags.
  CpuidInfo Cpu;
  Cpu.Leaf0 = {0xd, 0x68747541, 0x444d4163, 0x69746e65};  // "AuthenticAMD" (EBX, EDX, ECX = Auth, enti, cAMD)
  Cpu.Leaf1 = {0x00a20f10, 0x00100800, 0x00000201 | (1u << 28), (1u << 0) | (1u << 25) | (1u << 26)};  // family 0x19, model 0x21
  Cpu.Leaf7.Ebx = (1u << 5) | (1u << 9);                                                        // avx2 erms
  Cpu.Ext1.Edx = (1u << 11) | (1u << 29);                                                       // syscall lm
  Cpu.LeafD1.Eax = 1;                                                                           // xsaveopt
  Cpu.Ext8.Ebx = 1u << 9;                                                                       // wbnoinvd
  const char Brand[] = "  FEX Test CPU @ 2.0GHz";
  uint32_t Words[12] {};
  std::memcpy(Words, Brand, sizeof(Brand) - 1);
  for (int I = 0; I < 3; ++I) Cpu.Brand[I] = {Words[4 * I], Words[4 * I + 1], Words[4 * I + 2], Words[4 * I + 3]};
  CHECK(CpuVendor(Cpu) == "AuthenticAMD");
  CHECK(CpuBrand(Cpu) == "FEX Test CPU @ 2.0GHz");
  CHECK(CpuFlags(Cpu) == " fpu sse sse2 syscall lm pni ssse3 avx avx2 erms xsaveopt wbnoinvd");
  const auto Info = FormatCpuinfo(Cpu, 2);
  CHECK(Info.rfind("processor\t: 0\nvendor_id\t: AuthenticAMD\ncpu family\t: 25\nmodel\t\t: 33\nmodel name\t: FEX Test CPU @ 2.0GHz\n"
                   "stepping\t: 0\nmicrocode\t: 0x0\ncpu MHz\t\t: 1000.000\ncache size\t: 512 KB\nphysical id\t: 0\nsiblings\t: 2\n"
                   "core id\t\t: 0\ncpu cores\t: 2\napicid\t\t: 0\ninitial apicid\t: 0\nfpu\t\t: yes\nfpu_exception\t: yes\n"
                   "cpuid level\t: 13\nwp\t\t: yes\nflags\t\t: fpu sse sse2 syscall lm pni ssse3 avx avx2 erms xsaveopt wbnoinvd\n",
                   0) == 0);
  CHECK(Info.find(" wbnoinvd\nbugs\t\t:\nbogomips\t: 2000.00\nclflush size\t: 64\ncache_alignment\t: 64\n"
                  "address sizes\t: 48 bits physical, 48 bits virtual\npower management:\n\nprocessor\t: 1\n") != std::string::npos);
  CHECK(Info.find("core id\t\t: 1\n") != std::string::npos && Info.size() > 2 * 400);
  CHECK(Info.substr(Info.size() - 19) == "power management:\n\n");

  // bit masks as %*pb and %*pbl print them
  CHECK(FormatBitmap(0x3f, 6) == "3f");
  CHECK(FormatBitmap(0xf, 6) == "0f");
  CHECK(FormatBitmap(0x1, 1) == "1");
  CHECK(FormatBitmap(0x3, 2) == "3");
  CHECK(FormatBitmap(0xff, 8) == "ff");
  CHECK(FormatBitmap(0x1ffffffffULL, 33) == "1,ffffffff");
  CHECK(FormatBitmap(0xffffffffffULL, 40) == "ff,ffffffff");
  CHECK(FormatBitList(0x3f, 6) == "0-5");
  CHECK(FormatBitList(0x2d, 6) == "0,2-3,5");
  CHECK(FormatBitList(0, 6).empty());

  // status: every Linux 5.15 line in order
  Status St;
  St.Name = "bash";
  St.Umask = 022;
  St.State = 'S';
  St.Tgid = 112;
  St.Pid = 113;
  St.PPid = 1;
  St.Uid[0] = St.Uid[1] = St.Uid[2] = St.Uid[3] = 1000;
  St.Gid[0] = St.Gid[1] = St.Gid[2] = St.Gid[3] = 100;
  St.FdSize = 64;
  St.Groups = {100, 27};
  St.NStgid = "112\t1";
  St.NSpid = "113\t2";
  St.NSpgid = "112\t1";
  St.NSsid = "112\t1";
  St.VmPeak = 2000;
  St.VmSize = 1500;
  St.VmHwm = 700;
  St.VmRss = 600;
  St.RssAnon = 400;
  St.RssFile = 150;
  St.RssShmem = 50;
  St.VmData = 300;
  St.VmStk = 132;
  St.VmExe = 8;
  St.VmLib = 90;
  St.Threads = 3;
  St.SigQueued = 1;
  St.SigLimit = 4096;
  St.SigBlk = 0x10000;
  St.SigIgn = 0x1000;
  St.SigCgt = 0x4a00000000ULL;
  St.CapEff = 0x1ffffffffffULL;
  St.NoNewPrivs = 1;
  St.Seccomp = 2;
  St.SeccompFilters = 3;
  St.CpusAllowed = 0x3f;
  St.Cpus = 6;
  St.VoluntarySwitches = 9;
  St.InvoluntarySwitches = 2;
  CHECK(FormatStatus(St) ==
        "Name:\tbash\nUmask:\t0022\nState:\tS (sleeping)\nTgid:\t112\nNgid:\t0\nPid:\t113\nPPid:\t1\nTracerPid:\t0\n"
        "Uid:\t1000\t1000\t1000\t1000\nGid:\t100\t100\t100\t100\nFDSize:\t64\nGroups:\t100 27 \n"
        "NStgid:\t112\t1\nNSpid:\t113\t2\nNSpgid:\t112\t1\nNSsid:\t112\t1\n"
        "VmPeak:\t    2000 kB\nVmSize:\t    1500 kB\nVmLck:\t       0 kB\nVmPin:\t       0 kB\nVmHWM:\t     700 kB\nVmRSS:\t     600 kB\n"
        "RssAnon:\t     400 kB\nRssFile:\t     150 kB\nRssShmem:\t      50 kB\nVmData:\t     300 kB\nVmStk:\t     132 kB\n"
        "VmExe:\t       8 kB\nVmLib:\t      90 kB\nVmPTE:\t       0 kB\nVmSwap:\t       0 kB\nHugetlbPages:\t       0 kB\n"
        "CoreDumping:\t0\nTHP_enabled:\t0\nThreads:\t3\nSigQ:\t1/4096\n"
        "SigPnd:\t0000000000000000\nShdPnd:\t0000000000000000\nSigBlk:\t0000000000010000\nSigIgn:\t0000000000001000\n"
        "SigCgt:\t0000004a00000000\n"
        "CapInh:\t0000000000000000\nCapPrm:\t0000000000000000\nCapEff:\t000001ffffffffff\nCapBnd:\t0000000000000000\n"
        "CapAmb:\t0000000000000000\nNoNewPrivs:\t1\nSeccomp:\t2\nSeccomp_filters:\t3\nSpeculation_Store_Bypass:\tthread vulnerable\n"
        "Cpus_allowed:\t3f\nCpus_allowed_list:\t0-5\nMems_allowed:\t1\nMems_allowed_list:\t0\n"
        "voluntary_ctxt_switches:\t9\nnonvoluntary_ctxt_switches:\t2\n");
  St.State = 'Z';
  St.Groups.clear();
  const auto Zombie = FormatStatus(St);
  CHECK(Zombie.find("State:\tZ (zombie)\n") != std::string::npos && Zombie.find("Groups:\t\nNStgid") != std::string::npos);

  // limits: the header and 16 rows, padded like Linux
  uint64_t Limits[16][2];
  for (auto& Row : Limits) Row[0] = Row[1] = ~uint64_t {0};
  Limits[3][0] = 8388608;
  Limits[4][0] = 0;
  Limits[7][0] = Limits[7][1] = 1024;
  const auto Lim = FormatLimits(Limits);
  CHECK(Lim.rfind("Limit                     Soft Limit           Hard Limit           Units     \n"
                  "Max cpu time              unlimited            unlimited            seconds   \n"
                  "Max file size             unlimited            unlimited            bytes     \n"
                  "Max data size             unlimited            unlimited            bytes     \n"
                  "Max stack size            8388608              unlimited            bytes     \n"
                  "Max core file size        0                    unlimited            bytes     \n",
                  0) == 0);
  CHECK(Lim.find("Max open files            1024                 1024                 files     \n") != std::string::npos);
  CHECK(Lim.find("Max nice priority         unlimited            unlimited            \n") != std::string::npos);
  CHECK(Lim.find("Max realtime timeout      unlimited            unlimited            us        \n") != std::string::npos);
  CHECK(std::count(Lim.begin(), Lim.end(), '\n') == 17);

  // io
  IoCounters Io;
  Io.Rchar = 100;
  Io.Wchar = 20;
  Io.Syscr = 5;
  Io.Syscw = 3;
  CHECK(FormatIo(Io) == "rchar: 100\nwchar: 20\nsyscr: 5\nsyscw: 3\nread_bytes: 0\nwrite_bytes: 0\ncancelled_write_bytes: 0\n");

  // sched
  Sched Sc;
  Sc.Comm = "bash";
  Sc.Pid = 112;
  Sc.Threads = 2;
  Sc.ExecStartNs = 5'123'456'789;
  Sc.SumExecRuntimeNs = 42'000'001;
  Sc.Switches = 11;
  Sc.VoluntarySwitches = 9;
  Sc.InvoluntarySwitches = 2;
  Sc.Weight = NiceWeight(0);
  Sc.Policy = 0;
  Sc.Prio = 120;
  Sc.ClockDeltaNs = 0;
  CHECK(FormatSched(Sc) ==
        "bash (112, #threads: 2)\n---------------------------------------------------------\n"
        "se.exec_start                                :          5123.456789\n"
        "se.sum_exec_runtime                          :            42.000001\n"
        "nr_switches                                  :                   11\n"
        "nr_voluntary_switches                        :                    9\n"
        "nr_involuntary_switches                      :                    2\n"
        "se.load.weight                               :                 1024\n"
        "policy                                       :                    0\n"
        "prio                                         :                  120\n"
        "clock-delta                                  :                    0\n");
  CHECK(NiceWeight(-20) == 88761 && NiceWeight(19) == 15 && NiceWeight(5) == 335 && NiceWeight(100) == 15);

  // oom_adj scaling (oom_adjust_write and oom_adj_read)
  CHECK(OomAdjOf(1000) == 15 && OomAdjOf(-1000) == -17 && OomAdjOf(0) == 0 && OomAdjOf(500) == 8);
  CHECK(OomScoreOf(15) == 1000 && OomScoreOf(-17) == -1000 && OomScoreOf(0) == 0 && OomScoreOf(8) == 470);

  // fdinfo
  CHECK(FormatFdinfo(5, 02100002, 26, 77) == "pos:\t5\nflags:\t02100002\nmnt_id:\t26\nino:\t77\n");
  CHECK(FormatEventfdInfo(3) == "eventfd-count:                3\n");
  CHECK(FormatEpollInfo(7, 0x19, 0xdead, 0, 0x4d, 0xe) == "tfd:        7 events:       19 data:             dead  pos:0 ino:4d sdev:e\n");
  CHECK(FormatInotifyInfo(2, 0x4d, 0xe, 0xfff) == "inotify wd:2 ino:4d sdev:e mask:fff ignored_mask:0\n");
  CHECK(FormatSignalfdInfo(0x20000) == "sigmask:\t0000000000020000\n");
  CHECK(FormatTimerfdInfo(1, 4, 01, 2, 500, 1, 0) ==
        "clockid: 1\nticks: 4\nsettime flags: 01\nit_value: (2, 500)\nit_interval: (1, 0)\n");

  // small files
  CHECK(FormatPersonality(0) == "00000000\n" && FormatPersonality(0x0800000) == "00800000\n");
  const uint64_t Args[6] = {1, 2, 3, 0x10, 0x20, 0x30};
  CHECK(FormatSyscall(true, 0, Args, 0, 0) == "running\n");
  CHECK(FormatSyscall(false, 202, Args, 0x7ffc0, 0x401000) == "202 0x1 0x2 0x3 0x10 0x20 0x30 0x7ffc0 0x401000\n");
  CHECK(FormatSyscall(false, -1, Args, 0x7ffc0, 0x401000) == "-1 0x7ffc0 0x401000\n");
  CHECK(FormatMountstat("/dev/mmcblk0p1", "/mnt/sd", "vfat") == "device /dev/mmcblk0p1 mounted on /mnt/sd with fstype vfat\n");
  CHECK(FormatMountstat("", "/proc", "proc") == "device none mounted on /proc with fstype proc\n");
  CHECK(FormatChildren({4, 9}) == "4 9 " && FormatChildren({}).empty());

  // system files
  CHECK(FormatVersion("5.15.0", "#1 SMP PREEMPT FEX Switch 2") ==
        "Linux version 5.15.0 (fex@switch2) (gcc version 12.2.0) #1 SMP PREEMPT FEX Switch 2\n");
  CHECK(FormatFilesystems({{"sysfs", true}, {"tmpfs", true}, {"vfat", false}}) == "nodev\tsysfs\nnodev\ttmpfs\n\tvfat\n");
  CHECK(FormatSwaps() == "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");
  CHECK(FormatPartitions() == "major minor  #blocks  name\n\n");
  CHECK(FormatDevices({{1, "mem"}, {4, "tty"}, {136, "pts"}}) == "Character devices:\n  1 mem\n  4 tty\n136 pts\n\nBlock devices:\n");
  std::puts("guest-procfs-test: ok");
  return 0;
}
