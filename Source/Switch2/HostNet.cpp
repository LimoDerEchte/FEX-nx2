// SPDX-License-Identifier: MIT
#include "HostNet.h"
#include "GuestInet.h"

#include <switch.h>
#include <switch/services/bsd.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>

namespace Switch2Guest::HostNet {
namespace {
int References {};
bool NifmUp {};

// g_bsdErrno is a libounce thread-local (compiled with -mtp=soft). This file is built with emulated
// TLS, which would look for a different symbol, so it reads the variable the way libounce does:
// the thread pointer from __aarch64_read_tp plus the variable's local-exec offset.
extern "C" void* __aarch64_read_tp(void);
int* HostErrnoAddress() {
  const auto Thread = reinterpret_cast<uintptr_t>(__aarch64_read_tp());
  uintptr_t Address;
  asm("add %0, %1, #:tprel_hi12:g_bsdErrno, lsl #12\n\t"
      "add %0, %0, #:tprel_lo12_nc:g_bsdErrno"
      : "=&r"(Address)
      : "r"(Thread));
  return reinterpret_cast<int*>(Address);
}

// Every host call starts with a clear g_bsdErrno: a wrapper that fails before
// it reaches the service (a full descriptor table, a bad descriptor) leaves
// it unchanged, and a stale value would be reported.
void ClearHostErrno() { *HostErrnoAddress() = 0; }

// The result of a failed host call: the Linux errno the service reported, or
// EIO when the call failed without reporting one.
int Failure() {
  const int Error = *HostErrnoAddress();
  return Error ? -Inet::LinuxErrno(Error) : -5;
}

sockaddr* Raw(Address& Value) { return reinterpret_cast<sockaddr*>(Value.data()); }
const sockaddr* Raw(const Address& Value) { return reinterpret_cast<const sockaddr*>(Value.data()); }
} // namespace

bool Acquire() {
  if (References == 0) {
    constexpr u32 SocketBuffer = 65536;
    // The FTP screen's configuration (ftpd), except for 4 BSD sessions.
    static const SocketInitConfig Config = {
      .tcp_tx_buf_size = 8 * SocketBuffer,
      .tcp_rx_buf_size = 8 * SocketBuffer,
      .tcp_tx_buf_max_size = 16 * SocketBuffer,
      .tcp_rx_buf_max_size = 16 * SocketBuffer,
      .udp_tx_buf_size = 0x2400,
      .udp_rx_buf_size = 0xA500,
      .sb_efficiency = 8,
      .num_bsd_sessions = 4,
      .bsd_service_type = BsdServiceType_User,
    };
    if (R_FAILED(socketInitialize(&Config))) return false;
  }
  ++References;
  return true;
}

void Release() {
  if (References == 0 || --References != 0) return;
  if (NifmUp) nifmExit();
  NifmUp = false;
  socketExit();
}

// nifm answers address and DNS questions; it starts on first use and stops with the socket service.
static bool EnsureNifm() {
  if (!NifmUp) NifmUp = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
  return NifmUp;
}

bool DnsServers(uint32_t& Primary, uint32_t& Secondary) {
  u32 Address {}, Mask {}, Gateway {}, First {}, Second {};
  if (!EnsureNifm() || R_FAILED(nifmGetCurrentIpConfigInfo(&Address, &Mask, &Gateway, &First, &Second)) || !Address) return false;
  Primary = First;
  Secondary = Second;
  return true;
}

int Socket(int BsdType, int Protocol) {
  ClearHostErrno();
  const int Fd = socket(AF_INET, BsdType, Protocol);
  if (Fd < 0) return Failure();
  // Host sockets stay nonblocking for good (the scheduler waits for them).
  ClearHostErrno();
  if (fcntl(Fd, F_SETFL, O_NONBLOCK) < 0) {
    const int Error = Failure();
    close(Fd);
    return Error;
  }
  return Fd;
}

int Close(int Fd) {
  ClearHostErrno();
  return close(Fd) < 0 ? Failure() : 0;
}
int Bind(int Fd, const Address& To) {
  ClearHostErrno();
  return bind(Fd, Raw(To), sizeof(sockaddr_in)) < 0 ? Failure() : 0;
}
int Listen(int Fd, int Backlog) {
  ClearHostErrno();
  return listen(Fd, Backlog) < 0 ? Failure() : 0;
}
int Connect(int Fd, const Address& To) {
  ClearHostErrno();
  return connect(Fd, Raw(To), sizeof(sockaddr_in)) < 0 ? Failure() : 0;
}

int Accept(int Fd, Address& Peer) {
  ClearHostErrno();
  socklen_t Length = sizeof(sockaddr_in);
  Peer = {};
  const int New = accept(Fd, Raw(Peer), &Length);
  if (New < 0) return Failure();
  ClearHostErrno();
  if (fcntl(New, F_SETFL, O_NONBLOCK) < 0) {
    const int Error = Failure();
    close(New);
    return Error;
  }
  return New;
}

int64_t Send(int Fd, const void* Data, size_t Size, int BsdFlags, const Address* To) {
  ClearHostErrno();
  const ssize_t Sent = To ? sendto(Fd, Data, Size, BsdFlags, Raw(*To), sizeof(sockaddr_in)) : send(Fd, Data, Size, BsdFlags);
  return Sent < 0 ? Failure() : Sent;
}

int64_t Receive(int Fd, void* Data, size_t Size, int BsdFlags, Address* From, bool& HasFrom) {
  ClearHostErrno();
  HasFrom = false;
  socklen_t Length = sizeof(sockaddr_in);
  if (From) *From = {};
  const ssize_t Got = recvfrom(Fd, Data, Size, BsdFlags, From ? Raw(*From) : nullptr, From ? &Length : nullptr);
  if (Got < 0) return Failure();
  HasFrom = From && Length >= 8 && (*From)[1] == Inet::BsdAfInet;
  return Got;
}

int GetName(int Fd, bool Peer, Address& Out) {
  ClearHostErrno();
  socklen_t Length = sizeof(sockaddr_in);
  Out = {};
  const int Result = Peer ? getpeername(Fd, Raw(Out), &Length) : getsockname(Fd, Raw(Out), &Length);
  return Result < 0 ? Failure() : 0;
}

int GetOption(int Fd, int Level, int Name, void* Out, size_t& Size) {
  ClearHostErrno();
  socklen_t Length = static_cast<socklen_t>(Size);
  if (getsockopt(Fd, Level, Name, Out, &Length) < 0) return Failure();
  Size = Length;
  return 0;
}

int SetOption(int Fd, int Level, int Name, const void* Value, size_t Size) {
  ClearHostErrno();
  return setsockopt(Fd, Level, Name, Value, static_cast<socklen_t>(Size)) < 0 ? Failure() : 0;
}

int Shutdown(int Fd, int How) {
  ClearHostErrno();
  return shutdown(Fd, How) < 0 ? Failure() : 0;
}

int BytesPending(int Fd) {
  ClearHostErrno();
  int Count {};
  if (ioctl(Fd, FIONREAD, &Count) < 0) return Failure();
  return Count;
}

int Poll(PollEntry* Entries, size_t Count, int TimeoutMs) {
  ClearHostErrno();
  static_assert(sizeof(PollEntry) == sizeof(pollfd));
  static_assert(offsetof(PollEntry, Fd) == offsetof(pollfd, fd) && offsetof(PollEntry, Events) == offsetof(pollfd, events) &&
                offsetof(PollEntry, Revents) == offsetof(pollfd, revents));
  const int Ready = poll(reinterpret_cast<pollfd*>(Entries), static_cast<nfds_t>(Count), TimeoutMs);
  return Ready < 0 ? Failure() : Ready;
}

} // namespace Switch2Guest::HostNet
