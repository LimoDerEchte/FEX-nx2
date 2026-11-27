// SPDX-License-Identifier: MIT
// Per-client counters of GuestFtp.h (launcher FTP screen): peer address, user, activity, byte counts, totals, required user name,
// Idle callback during a transfer, and Disconnect. Runs a real client over loopback against HostFiles from FtpServerHost.cpp.
#define main FtpServerHostMain
#include "FtpServerHost.cpp"
#undef main

#include <sys/stat.h>

#include <atomic>
#include <thread>

namespace {
int Failures = 0, Checks = 0;
#define CHECK(Cond)                                                  \
  do {                                                               \
    ++Checks;                                                        \
    if (!(Cond)) {                                                   \
      ++Failures;                                                    \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond);    \
    }                                                                \
  } while (0)

struct Client {
  int FD = -1;
  std::string Buffer;
  bool Connect(uint16_t Port) {
    FD = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in A {};
    A.sin_family = AF_INET;
    A.sin_port = htons(Port);
    A.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return connect(FD, reinterpret_cast<sockaddr*>(&A), sizeof(A)) == 0;
  }
  // One reply: reads lines until "NNN text" (not "NNN-").
  int Reply(std::string* Text = nullptr) {
    for (;;) {
      size_t End;
      while ((End = Buffer.find("\r\n")) != std::string::npos) {
        std::string Line = Buffer.substr(0, End);
        Buffer.erase(0, End + 2);
        if (Line.size() >= 4 && Line[3] == ' ') {
          if (Text) *Text = Line.substr(4);
          return std::atoi(Line.c_str());
        }
      }
      char C[512];
      const auto N = recv(FD, C, sizeof(C), 0);
      if (N <= 0) return -1;
      Buffer.append(C, N);
    }
  }
  int Cmd(const std::string& Line, std::string* Text = nullptr) {
    const std::string L = Line + "\r\n";
    send(FD, L.data(), L.size(), 0);
    return Reply(Text);
  }
  int Data(std::string& Reply227) {
    if (Cmd("EPSV", &Reply227) != 229) return -1;
    const auto P = Reply227.find("|||");
    const int Port = std::atoi(Reply227.c_str() + P + 3);
    int D = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in A {};
    A.sin_family = AF_INET;
    A.sin_port = htons(Port);
    A.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return connect(D, reinterpret_cast<sockaddr*>(&A), sizeof(A)) == 0 ? D : -1;
  }
};
} // namespace

int main() {
  char Tpl[] = "/tmp/fex-ftp-stats-XXXXXX";
  const std::string Root = mkdtemp(Tpl);
  HostFiles Files {Root};
  FtpServer Server {Files, "pw1234"};
  Server.SetUser("switch");
  CHECK(Server.Start(0, htonl(INADDR_LOOPBACK)) == 0);
  const uint16_t Port = Server.Port();
  CHECK(Port != 0);
  int IdleCalls = 0, SawReceiving = 0, SawSending = 0;
  uint64_t MaxDone = 0;
  Server.Idle = [&]() {
    ++IdleCalls;
    for (const FtpClientInfo& C : Server.Clients()) {
      if (C.State == FtpClientInfo::Activity::Receiving && C.File == "/big.bin") ++SawReceiving;
      if (C.State == FtpClientInfo::Activity::Sending && C.File == "/big.bin" && C.Total == 8u << 20) ++SawSending;
      MaxDone = std::max(MaxDone, C.Done);
    }
  };

  std::atomic<int> Stage {0};
  std::atomic<bool> BadUserRejected {false}, GoodOk {false};
  std::thread T([&]() {
    Client Bad;
    Bad.Connect(Port);
    Bad.Reply();
    Bad.Cmd("USER other");
    BadUserRejected = Bad.Cmd("PASS pw1234") == 530;
    Bad.Cmd("QUIT");
    close(Bad.FD);

    Client C;
    C.Connect(Port);
    C.Reply();
    C.Cmd("USER switch");
    GoodOk = C.Cmd("PASS pw1234") == 230;
    std::string T2;
    int D = C.Data(T2);
    send(C.FD, "STOR big.bin\r\n", 14, 0);
    C.Reply();  // 150? the server replies after accept
    std::string Block(1 << 20, 'x');
    for (int I = 0; I < 8; ++I) {
      send(D, Block.data(), Block.size(), 0);
      usleep(40000);
    }
    close(D);
    C.Reply();
    D = C.Data(T2);
    send(C.FD, "RETR big.bin\r\n", 14, 0);
    char Buf[65536];
    uint64_t Got = 0;
    for (ssize_t N; (N = recv(D, Buf, sizeof(Buf), 0)) > 0;) {
      Got += uint64_t(N);
      usleep(2000);
    }
    close(D);
    C.Reply();
    (void)Got;
    Stage = 1;
    while (Stage != 2) usleep(2000);  // the main thread checks the idle client
    C.Cmd("QUIT");
    close(C.FD);
    Stage = 3;
  });

  uint64_t Id = 0;
  for (int Loops = 0; Loops < 4000 && Stage != 1; ++Loops) Server.Step(10);
  const auto Cl = Server.Clients();
  CHECK(Cl.size() == 1);
  if (Cl.size() == 1) {
    Id = Cl[0].Id;
    CHECK(Cl[0].Address == "127.0.0.1");
    CHECK(Cl[0].User == "switch" && Cl[0].LoggedIn);
    CHECK(Cl[0].State == FtpClientInfo::Activity::Idle && Cl[0].File.empty());
    CHECK(Cl[0].BytesIn == (8u << 20) && Cl[0].BytesOut == (8u << 20));
  }
  CHECK(BadUserRejected && GoodOk);
  CHECK(Server.TotalIn() == (8u << 20) && Server.TotalOut() == (8u << 20));
  CHECK(IdleCalls > 0 && SawReceiving > 0 && SawSending > 0 && MaxDone > 0);
  CHECK(Server.Disconnect(Id) && !Server.Disconnect(Id + 99));
  Server.Step(10);
  CHECK(Server.Clients().empty());
  Stage = 2;
  T.join();
  Server.Stop();
  std::printf("ftp-stats-test: %d checks, %d failures\n", Checks, Failures);
  return Failures ? 1 : 0;
}
