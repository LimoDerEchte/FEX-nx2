// SPDX-License-Identifier: MIT
// EXPERIMENT console probe (not part of the launcher): does Switch 2 Horizon really forbid bind(0.0.0.0)? Tests wildcard, loopback and
// console-IP binds (TCP and UDP), listen, self-connect, UDP loopback, with the FTP server's socket
// config and the libnx default. One result line per case on screen and in the SD log.
#include <switch.h>
#include <switch/services/bsd.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr const char* Version = "bind-probe 1";
constexpr const char* LogPath = "sdmc:/switch/fex-bindprobe.log";
constexpr uint16_t HighPort = 40001, ListenPort = 40002;

FILE* Log;
int Fails;
int TcpAny = -2, UdpAny = -2;   // -2 untested, 0 allowed, else the errno of the failed bind
bool Quit;
PadState Pad;
u32 ConsoleIp; // network byte order

void Out(const char* Format, ...) {
  char Line[256];
  va_list Args;
  va_start(Args, Format);
  std::vsnprintf(Line, sizeof(Line), Format, Args);
  va_end(Args);
  std::printf("%s\n", Line);
  consoleUpdate(nullptr);
  if (Log) {
    std::fprintf(Log, "%s\n", Line);
    std::fflush(Log);
  }
}

void PollQuit() {
  padUpdate(&Pad);
  if (padGetButtonsDown(&Pad) & HidNpadButton_Plus) Quit = true;
}

void Clear() { g_bsdErrno = 0; errno = 0; }

sockaddr_in Addr(u32 Ip, uint16_t Port) {
  sockaddr_in A {};
  A.sin_family = AF_INET;
  A.sin_addr.s_addr = Ip;
  A.sin_port = htons(Port);
  return A;
}

const char* Ntoa(u32 Ip) {
  static char Text[4][INET_ADDRSTRLEN];
  static int Slot;
  char* Buf = Text[Slot++ & 3];
  in_addr In {Ip};
  inet_ntop(AF_INET, &In, Buf, INET_ADDRSTRLEN);
  return Buf;
}

int Open(int Type) {
  Clear();
  const int Fd = socket(AF_INET, Type, 0);
  if (Fd >= 0) fcntl(Fd, F_SETFL, O_NONBLOCK);
  return Fd;
}

// Prints one case line. Pass = expected-to-be-informative success flag.
void Report(const char* Tag, const char* Case, bool Pass, int Ret) {
  Out("%s %s: %s ret=%d errno=%d bsd_errno=%d", Tag, Case, Pass ? "PASS" : "FAIL", Ret, errno, g_bsdErrno);
  if (!Pass) ++Fails;
}

uint16_t BoundPort(int Fd, u32* Ip = nullptr) {
  sockaddr_in A {};
  socklen_t Len = sizeof(A);
  Clear();
  if (getsockname(Fd, reinterpret_cast<sockaddr*>(&A), &Len) < 0) return 0xFFFF;
  if (Ip) *Ip = A.sin_addr.s_addr;
  return ntohs(A.sin_port);
}

int ErrnoOf() { return g_bsdErrno ? g_bsdErrno : errno; }

// Self-connect tests against a listener at Ip:Port (TCP, listener already listening).
void SelfConnect(const char* Tag, int Listener, uint16_t Port, const char* What, u32 Target) {
  const int C = Open(SOCK_STREAM);
  if (C < 0) { Report(Tag, What, false, C); return; }
  const sockaddr_in To = Addr(Target, Port);
  Clear();
  int R = connect(C, reinterpret_cast<const sockaddr*>(&To), sizeof(To));
  const int ConnErr = ErrnoOf();
  bool Connected = R == 0;
  if (R < 0 && (errno == EINPROGRESS || g_bsdErrno == 115 || g_bsdErrno == 36 || errno == EWOULDBLOCK)) {
    pollfd P {C, POLLOUT, 0};
    Clear();
    if (poll(&P, 1, 2000) > 0 && (P.revents & POLLOUT) && !(P.revents & (POLLERR | POLLHUP))) {
      int So = 0;
      socklen_t L = sizeof(So);
      getsockopt(C, SOL_SOCKET, SO_ERROR, &So, &L);
      Connected = So == 0;
      if (!Connected) g_bsdErrno = So;
    }
  }
  int Accepted = -1;
  if (Connected) {
    for (int I = 0; I < 20 && Accepted < 0; ++I) {
      pollfd P {Listener, POLLIN, 0};
      poll(&P, 1, 100);
      Clear();
      sockaddr_in Peer {};
      socklen_t L = sizeof(Peer);
      Accepted = accept(Listener, reinterpret_cast<sockaddr*>(&Peer), &L);
      if (Accepted >= 0) Out("      accepted peer %s:%u", Ntoa(Peer.sin_addr.s_addr), ntohs(Peer.sin_port));
    }
  }
  char Name[96];
  std::snprintf(Name, sizeof(Name), "%s connect+accept (initial connect errno %d)", What, ConnErr);
  if (!Connected || Accepted < 0) { /* keep the errno of the failing step */ }
  Report(Tag, Name, Connected && Accepted >= 0, Connected ? Accepted : -1);
  if (Accepted >= 0) close(Accepted);
  close(C);
}

// One TCP bind case. Returns true when the bind succeeded.
bool TcpCase(const char* Tag, const char* Label, u32 Ip, uint16_t Port, bool Reuse, bool Full, int* ErrOut = nullptr) {
  char Name[96];
  std::snprintf(Name, sizeof(Name), "TCP bind %s:%u%s", Label, Port, Reuse ? " +REUSEADDR" : "");
  const int Fd = Open(SOCK_STREAM);
  if (Fd < 0) { Report(Tag, Name, false, Fd); return false; }
  if (Reuse) {
    int One = 1;
    Clear();
    const int R = setsockopt(Fd, SOL_SOCKET, SO_REUSEADDR, &One, sizeof(One));
    if (R < 0) Out("%s   setsockopt SO_REUSEADDR failed ret=%d errno=%d bsd_errno=%d", Tag, R, errno, g_bsdErrno);
  }
  const sockaddr_in A = Addr(Ip, Port);
  Clear();
  const int R = bind(Fd, reinterpret_cast<const sockaddr*>(&A), sizeof(A));
  const int Err = R < 0 ? ErrnoOf() : 0;
  Report(Tag, Name, R == 0, R);
  if (ErrOut) *ErrOut = Err;
  if (R == 0 && Full) {
    u32 Got = 0;
    const uint16_t P = BoundPort(Fd, &Got);
    Out("%s   getsockname -> %s:%u", Tag, Ntoa(Got), P);
    Clear();
    const int L = listen(Fd, 4);
    Report(Tag, "  listen", L == 0, L);
    if (L == 0 && Ip == INADDR_ANY && P != 0 && P != 0xFFFF) {
      SelfConnect(Tag, Fd, P, "  127.0.0.1", htonl(INADDR_LOOPBACK));
      if (ConsoleIp) SelfConnect(Tag, Fd, P, "  console-ip", ConsoleIp);
    }
  }
  close(Fd);
  return R == 0;
}

bool UdpCase(const char* Tag, const char* Label, u32 Ip, uint16_t Port, bool Full, int* ErrOut = nullptr) {
  char Name[96];
  std::snprintf(Name, sizeof(Name), "UDP bind %s:%u", Label, Port);
  const int Fd = Open(SOCK_DGRAM);
  if (Fd < 0) { Report(Tag, Name, false, Fd); return false; }
  const sockaddr_in A = Addr(Ip, Port);
  Clear();
  const int R = bind(Fd, reinterpret_cast<const sockaddr*>(&A), sizeof(A));
  const int Err = R < 0 ? ErrnoOf() : 0;
  Report(Tag, Name, R == 0, R);
  if (ErrOut) *ErrOut = Err;
  if (R == 0 && Full) {
    u32 Got = 0;
    const uint16_t P = BoundPort(Fd, &Got);
    Out("%s   getsockname -> %s:%u", Tag, Ntoa(Got), P);
    if (Ip == INADDR_ANY && P != 0 && P != 0xFFFF) {
      for (const u32 Target : {htonl(INADDR_LOOPBACK), ConsoleIp}) {
        if (!Target) continue;
        const sockaddr_in To = Addr(Target, P);
        const char Msg[] = "bindprobe";
        Clear();
        const ssize_t S = sendto(Fd, Msg, sizeof(Msg), 0, reinterpret_cast<const sockaddr*>(&To), sizeof(To));
        int SendErr = ErrnoOf();
        ssize_t Got2 = -1;
        char Buf[32];
        sockaddr_in From {};
        for (int I = 0; I < 20 && S > 0; ++I) {
          pollfd Pf {Fd, POLLIN, 0};
          poll(&Pf, 1, 100);
          socklen_t L = sizeof(From);
          Clear();
          Got2 = recvfrom(Fd, Buf, sizeof(Buf), 0, reinterpret_cast<sockaddr*>(&From), &L);
          if (Got2 >= 0) break;
        }
        char Name2[96];
        std::snprintf(Name2, sizeof(Name2), "  sendto %s + recvfrom (send errno %d, from %s)", Ntoa(Target), SendErr,
                      Got2 >= 0 ? Ntoa(From.sin_addr.s_addr) : "-");
        Report(Tag, Name2, S > 0 && Got2 > 0, static_cast<int>(Got2));
      }
    }
  }
  close(Fd);
  return R == 0;
}

bool Init(const char* Which, bool Ftp) {
  constexpr u32 Buf = 65536;
  SocketInitConfig Config = *socketGetDefaultInitConfig();
  if (Ftp) {
    // Same as HostNet.cpp / the FTP screen (ftpd).
    Config = {
      .tcp_tx_buf_size = 8 * Buf,
      .tcp_rx_buf_size = 8 * Buf,
      .tcp_tx_buf_max_size = 16 * Buf,
      .tcp_rx_buf_max_size = 16 * Buf,
      .udp_tx_buf_size = 0x2400,
      .udp_rx_buf_size = 0xA500,
      .sb_efficiency = 8,
      .num_bsd_sessions = 4,
      .bsd_service_type = BsdServiceType_User,
    };
  }
  const Result Rc = socketInitialize(&Config);
  Out("== socket init %s rc=%#x", Which, Rc);
  return R_SUCCEEDED(Rc);
}

void KeyCases(const char* Tag) {
  TcpCase(Tag, "0.0.0.0", INADDR_ANY, 0, false, true, &TcpAny);
  TcpCase(Tag, "0.0.0.0", INADDR_ANY, HighPort, false, true);
  TcpCase(Tag, "0.0.0.0", INADDR_ANY, HighPort, true, true);
  UdpCase(Tag, "0.0.0.0", INADDR_ANY, 0, true, &UdpAny);
  UdpCase(Tag, "0.0.0.0", INADDR_ANY, HighPort, true);
}

void Matrix(const char* Tag) {
  TcpCase(Tag, "0.0.0.0", INADDR_ANY, 0, false, true, &TcpAny);
  TcpCase(Tag, "0.0.0.0", INADDR_ANY, 0, true, true);
  TcpCase(Tag, "0.0.0.0", INADDR_ANY, HighPort, false, true);
  TcpCase(Tag, "0.0.0.0", INADDR_ANY, HighPort, true, true);
  TcpCase(Tag, "0.0.0.0", INADDR_ANY, 1337, false, true);
  TcpCase(Tag, "0.0.0.0", INADDR_ANY, 1337, true, true);
  UdpCase(Tag, "0.0.0.0", INADDR_ANY, 0, true, &UdpAny);
  UdpCase(Tag, "0.0.0.0", INADDR_ANY, HighPort, true);
  const u32 Lo = htonl(INADDR_LOOPBACK);
  TcpCase(Tag, "127.0.0.1", Lo, 0, false, true);
  TcpCase(Tag, "127.0.0.1", Lo, HighPort, true, true);
  UdpCase(Tag, "127.0.0.1", Lo, 0, true);
  UdpCase(Tag, "127.0.0.1", Lo, HighPort, true);
  if (ConsoleIp) {
    TcpCase(Tag, "console-ip", ConsoleIp, 0, false, true);
    TcpCase(Tag, "console-ip", ConsoleIp, HighPort, true, true);
    TcpCase(Tag, "console-ip", ConsoleIp, 1337, true, true);
    UdpCase(Tag, "console-ip", ConsoleIp, 0, true);
    UdpCase(Tag, "console-ip", ConsoleIp, HighPort, true);
  } else {
    Out("%s console-ip cases SKIPPED (no address)", Tag);
  }
}

void Listener() {
  if (TcpAny != 0) { Out("Listener stage skipped: 0.0.0.0 TCP bind failed."); return; }
  const int Fd = Open(SOCK_STREAM);
  int One = 1;
  setsockopt(Fd, SOL_SOCKET, SO_REUSEADDR, &One, sizeof(One));
  const sockaddr_in A = Addr(INADDR_ANY, ListenPort);
  Clear();
  if (Fd < 0 || bind(Fd, reinterpret_cast<const sockaddr*>(&A), sizeof(A)) < 0 || listen(Fd, 4) < 0) {
    Out("Listener 0.0.0.0:%u failed errno=%d bsd_errno=%d", ListenPort, errno, g_bsdErrno);
    if (Fd >= 0) close(Fd);
    return;
  }
  Out("Listening 0.0.0.0:%u for 60 s. From PC: nc %s %u   (+ to exit)", ListenPort, ConsoleIp ? Ntoa(ConsoleIp) : "<console-ip>",
      ListenPort);
  int Seen = 0;
  for (int Tick = 0; Tick < 600 && !Quit && appletMainLoop(); ++Tick) {
    PollQuit();
    pollfd P {Fd, POLLIN, 0};
    if (poll(&P, 1, 100) > 0 && (P.revents & POLLIN)) {
      sockaddr_in Peer {};
      socklen_t L = sizeof(Peer);
      Clear();
      const int C = accept(Fd, reinterpret_cast<sockaddr*>(&Peer), &L);
      if (C >= 0) {
        ++Seen;
        Out("LISTENER accepted peer %s:%u", Ntoa(Peer.sin_addr.s_addr), ntohs(Peer.sin_port));
        close(C);
      }
    }
  }
  Out("Listener done: %d peers accepted.", Seen);
  close(Fd);
}

} // namespace

int main() {
  consoleInit(nullptr);
  fsdevMountSdmc();
  // Append so repeated runs are all kept; fall back to the SD root if /switch is missing.
  const char* Opened = LogPath;
  Log = std::fopen(LogPath, "a");
  if (!Log) {
    Opened = "sdmc:/fex-bindprobe.log";
    Log = std::fopen(Opened, "a");
  }
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&Pad);
  if (Log) std::fprintf(Log, "\n===== new run =====\n");
  Out("%s. Log: %s", Version, Log ? Opened : "NOT OPEN (SD write failed), screen only");
  const bool Nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
  Result Rc = Nifm ? nifmGetCurrentIpAddress(&ConsoleIp) : 1;
  Out("nifm=%d ip_rc=%#x ip=%s", Nifm, Rc, ConsoleIp ? Ntoa(ConsoleIp) : "none");
  if (R_FAILED(Rc)) ConsoleIp = 0;

  // Default libnx config first (key cases), then the FTP server's config (full matrix).
  int DefTcp = -2, DefUdp = -2;
  if (Init("default", false)) {
    KeyCases("[default]");
    DefTcp = TcpAny;
    DefUdp = UdpAny;
    socketExit();
  }
  TcpAny = UdpAny = -2;
  if (Init("ftpd(4 sessions)", true)) {
    Matrix("[ftpd]");
    if (!Quit) Listener();
    socketExit();
  }
  Out("Config default: TCP %s, UDP %s", DefTcp == 0 ? "allowed" : DefTcp == -2 ? "untested" : "forbidden",
      DefUdp == 0 ? "allowed" : DefUdp == -2 ? "untested" : "forbidden");
  Out("FAIL count: %d", Fails);
  char T[64], U[64];
  std::snprintf(T, sizeof(T), "TCP %s", TcpAny == 0 ? "allowed" : TcpAny == -2 ? "untested" : "forbidden");
  std::snprintf(U, sizeof(U), "UDP %s", UdpAny == 0 ? "allowed" : UdpAny == -2 ? "untested" : "forbidden");
  if (TcpAny > 0) std::snprintf(T, sizeof(T), "TCP forbidden errno %d", TcpAny);
  if (UdpAny > 0) std::snprintf(U, sizeof(U), "UDP forbidden errno %d", UdpAny);
  Out("Wildcard bind: %s, %s", T, U);
  Out("DONE. Press + to exit.");
  while (appletMainLoop() && !Quit) {
    PollQuit();
    svcSleepThread(50'000'000);
  }
  if (Log) std::fclose(Log);
  consoleExit(nullptr);
  return 0;
}
