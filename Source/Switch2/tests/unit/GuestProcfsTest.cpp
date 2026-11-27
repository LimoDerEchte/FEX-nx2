// SPDX-License-Identifier: MIT
#include "GuestProcfs.h"

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
  CHECK(FormatCpuinfo(2).find("processor\t: 1\n") != std::string::npos);
  std::puts("guest-procfs-test: ok");
  return 0;
}
