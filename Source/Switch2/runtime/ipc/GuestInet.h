// SPDX-License-Identifier: MIT
// Linux <-> Horizon BSD translation for AF_INET sockets. The host socket
// service is FreeBSD shaped (SOL_SOCKET 0xffff, sin_len, different SO_*,
// MSG_* and poll bits), so nothing a guest passes reaches it unchanged.
// Only fixed numeric constants are used here: no libnx or system socket
// headers, so the header builds and is tested on the host.
//
// BSD values were read from /opt/devkitpro/libounce/include/sys/socket.h,
// netinet/in.h, netinet/tcp.h and sys/poll.h (m3 WP5 step 1).
#pragma once

#include "GuestSocket.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Switch2Guest::Inet {

// ----- Linux values -----
constexpr int LinuxSolSocket = 1, LinuxAfInet = 2, LinuxIpprotoIp = 0, LinuxIpprotoIcmp = 1, LinuxIpprotoTcp = 6,
              LinuxIpprotoUdp = 17;

// ----- BSD values -----
constexpr int BsdSolSocket = 0xffff, BsdIpprotoIp = 0, BsdIpprotoTcp = 6, BsdAfInet = 2;
constexpr int BsdSoReuseAddr = 0x0004, BsdSoKeepAlive = 0x0008, BsdSoDontRoute = 0x0010, BsdSoBroadcast = 0x0020,
              BsdSoLinger = 0x0080, BsdSoOobInline = 0x0100, BsdSoReusePort = 0x0200, BsdSoSndBuf = 0x1001,
              BsdSoRcvBuf = 0x1002, BsdSoSndLowat = 0x1003, BsdSoRcvLowat = 0x1004, BsdSoSndTimeo = 0x1005,
              BsdSoRcvTimeo = 0x1006, BsdSoError = 0x1007, BsdSoType = 0x1008;
constexpr int BsdIpOptions = 1, BsdIpHdrincl = 2, BsdIpTos = 3, BsdIpTtl = 4, BsdIpMulticastIf = 9,
              BsdIpMulticastTtl = 10, BsdIpMulticastLoop = 11, BsdIpAddMembership = 12, BsdIpDropMembership = 13;
constexpr int BsdTcpNodelay = 1, BsdTcpMaxseg = 2, BsdTcpInfo = 32, BsdTcpCongestion = 64, BsdTcpKeepIdle = 256,
              BsdTcpKeepIntvl = 512, BsdTcpKeepCnt = 1024;
constexpr int BsdMsgOob = 0x1, BsdMsgPeek = 0x2, BsdMsgDontRoute = 0x4, BsdMsgEor = 0x8, BsdMsgTrunc = 0x10,
              BsdMsgCtrunc = 0x20, BsdMsgWaitAll = 0x40, BsdMsgDontWait = 0x80;
constexpr int16_t BsdPollIn = 1, BsdPollPri = 2, BsdPollOut = 4, BsdPollErr = 8, BsdPollHup = 0x10, BsdPollNval = 0x20;

// Linux MSG_*.
constexpr uint64_t MsgOob = 0x1, MsgPeek = 0x2, MsgDontRoute = 0x4, MsgCtrunc = 0x8, MsgTrunc = 0x20, MsgDontWait = 0x40,
                   MsgEor = 0x80, MsgWaitAll = 0x100, MsgConfirm = 0x800, MsgErrQueue = 0x2000, MsgNoSignal = 0x4000,
                   MsgMore = 0x8000, MsgWaitForOne = 0x10000, MsgCmsgCloexec = 0x40000000;
// Bits the runtime acts on itself and never hands to the host.
constexpr uint64_t SendRuntimeFlags = MsgNoSignal | MsgMore | MsgConfirm;
constexpr uint64_t ReceiveRuntimeFlags = MsgTrunc | MsgWaitAll | MsgErrQueue | MsgWaitForOne | MsgCmsgCloexec;

// Linux errno values used here.
constexpr int Eio = 5, Einval = 22, Enoent = 2, Enodev = 19, Enoprotoopt = 92, Eafnosupport = 97, Eperm = 1;

// ----- Options -----
enum class OptionKind : uint8_t { Int, Bool, Linger, Timeval, Ip4, Byte, Membership };
constexpr int NoHost = -1; // the host has no such option: the runtime stores it

struct OptionMap {
  int LinuxLevel, LinuxName, BsdLevel, BsdName;
  OptionKind Type;
};

// Options forwarded to the host socket. Everything else the runtime
// answers itself (see Emulated below and Switch2Guest::SocketOptions).
inline constexpr OptionMap OptionTable[] = {
  // SOL_SOCKET
  {1, SockOpt::ReuseAddr, BsdSolSocket, BsdSoReuseAddr, OptionKind::Bool},
  {1, SockOpt::DontRoute, BsdSolSocket, BsdSoDontRoute, OptionKind::Bool},
  {1, SockOpt::Broadcast, BsdSolSocket, BsdSoBroadcast, OptionKind::Bool},
  {1, SockOpt::SndBuf, BsdSolSocket, BsdSoSndBuf, OptionKind::Int},
  {1, SockOpt::RcvBuf, BsdSolSocket, BsdSoRcvBuf, OptionKind::Int},
  {1, SockOpt::KeepAlive, BsdSolSocket, BsdSoKeepAlive, OptionKind::Bool},
  {1, SockOpt::OobInline, BsdSolSocket, BsdSoOobInline, OptionKind::Bool},
  {1, SockOpt::Linger, BsdSolSocket, BsdSoLinger, OptionKind::Linger},
  {1, SockOpt::ReusePort, BsdSolSocket, BsdSoReusePort, OptionKind::Bool},
  {1, SockOpt::Error, BsdSolSocket, BsdSoError, OptionKind::Int},
  // IPPROTO_IP
  {0, 1, BsdIpprotoIp, BsdIpTos, OptionKind::Int},
  {0, 2, BsdIpprotoIp, BsdIpTtl, OptionKind::Int},
  {0, 3, BsdIpprotoIp, BsdIpHdrincl, OptionKind::Bool},
  {0, 32, BsdIpprotoIp, BsdIpMulticastIf, OptionKind::Ip4},
  {0, 33, BsdIpprotoIp, BsdIpMulticastTtl, OptionKind::Byte},
  {0, 34, BsdIpprotoIp, BsdIpMulticastLoop, OptionKind::Byte},
  {0, 35, BsdIpprotoIp, BsdIpAddMembership, OptionKind::Membership},
  {0, 36, BsdIpprotoIp, BsdIpDropMembership, OptionKind::Membership},
  // IPPROTO_TCP
  {6, 1, BsdIpprotoTcp, BsdTcpNodelay, OptionKind::Bool},
  {6, 2, BsdIpprotoTcp, BsdTcpMaxseg, OptionKind::Int},
  {6, 4, BsdIpprotoTcp, BsdTcpKeepIdle, OptionKind::Int},
  {6, 5, BsdIpprotoTcp, BsdTcpKeepIntvl, OptionKind::Int},
  {6, 6, BsdIpprotoTcp, BsdTcpKeepCnt, OptionKind::Int},
};

inline std::optional<OptionMap> MapOption(int LinuxLevel, int LinuxName) {
  for (const auto& Entry : OptionTable) {
    if (Entry.LinuxLevel == LinuxLevel && Entry.LinuxName == LinuxName) return Entry;
  }
  return std::nullopt;
}

inline std::optional<OptionMap> UnmapOption(int BsdLevel, int BsdName) {
  for (const auto& Entry : OptionTable) {
    if (Entry.BsdLevel == BsdLevel && Entry.BsdName == BsdName) return Entry;
  }
  return std::nullopt;
}

// Linux range checks the host would not make the same way (tcp_setsockopt).
// Returns 0 or -EINVAL.
inline int CheckOptionRange(int LinuxLevel, int LinuxName, int32_t Value) {
  if (LinuxLevel != LinuxIpprotoTcp) return 0;
  switch (LinuxName) {
  case 2: return Value < 8 || Value > 65535 ? -Einval : 0;             // TCP_MAXSEG
  case 4: case 5: return Value < 1 || Value > 32767 ? -Einval : 0;     // TCP_KEEPIDLE, TCP_KEEPINTVL
  case 6: return Value < 1 || Value > 127 ? -Einval : 0;               // TCP_KEEPCNT
  default: return 0;
  }
}

// Options kept by the runtime because the host has no equivalent. Linux
// stores them in the socket and so does this. Only values Linux accepts are
// accepted; none of them changes what goes on the wire.
struct Emulated {
  int32_t Cork {}, QuickAck {}, DeferAccept {}, UserTimeout {}, NotsentLowat {}, Timestamp {}, RecvErr {}, MtuDiscover {},
          IncomingCpu {}, BusyPoll {}, SendLowat {1}, FastOpen {}, Syncnt {}, Linger2 {}, WindowClamp {};
  std::string Congestion {"cubic"};
};

// Level/name pairs of Emulated. Result: 0 stored, a negative errno, or
// Unhandled when the pair is not one of ours.
constexpr int Unhandled = INT32_MIN;

inline int StoreEmulated(Emulated& Store, int Level, int Name, const uint8_t* Value, size_t Length) {
  int32_t Int {};
  if (Length >= sizeof(Int)) std::memcpy(&Int, Value, sizeof(Int));
  const bool HasInt = Length >= sizeof(Int);
  auto Put = [&](int32_t& Field, bool NonNegative = false) {
    if (!HasInt) return -Einval;
    if (NonNegative && Int < 0) return -Einval;
    Field = Int;
    return 0;
  };
  auto Flag = [&](int32_t& Field) {
    if (!HasInt) return -Einval;
    Field = Int != 0;
    return 0;
  };
  if (Level == LinuxSolSocket) {
    switch (Name) {
    case 25: // SO_BINDTODEVICE: only "no device" is possible without interfaces to name
      if (Length == 0 || Value[0] == 0) return 0;
      return -Enodev;
    case 29: return Flag(Store.Timestamp);
    case 19: return Put(Store.SendLowat); // SO_SNDLOWAT
    case 49: return Put(Store.IncomingCpu);
    case 46: return Put(Store.BusyPoll, true);
    default: return Unhandled;
    }
  }
  if (Level == LinuxIpprotoIp) {
    switch (Name) {
    case 11: return Flag(Store.RecvErr);                  // IP_RECVERR
    case 10:                                              // IP_MTU_DISCOVER: 0..3
      if (!HasInt || Int < 0 || Int > 3) return -Einval;
      Store.MtuDiscover = Int;
      return 0;
    default: return Unhandled;
    }
  }
  if (Level == LinuxIpprotoTcp) {
    switch (Name) {
    case 3: return Flag(Store.Cork);                      // TCP_CORK
    case 7: return Put(Store.Syncnt, true);               // TCP_SYNCNT
    case 8: return Put(Store.Linger2);                    // TCP_LINGER2
    case 9: return Put(Store.DeferAccept, true);          // TCP_DEFER_ACCEPT
    case 10: return Put(Store.WindowClamp, true);         // TCP_WINDOW_CLAMP
    case 12: return Flag(Store.QuickAck);                 // TCP_QUICKACK
    case 18: return Put(Store.UserTimeout, true);         // TCP_USER_TIMEOUT
    case 23: return Put(Store.FastOpen, true);            // TCP_FASTOPEN
    case 25: return Put(Store.NotsentLowat, true);        // TCP_NOTSENT_LOWAT
    case 13: {                                            // TCP_CONGESTION: an algorithm that is loaded
      if (Length < 1) return -Einval;
      // tcp_setsockopt copies at most TCP_CA_NAME_MAX - 1 bytes.
      std::string Algorithm(reinterpret_cast<const char*>(Value), std::min<size_t>(Length, 15));
      Algorithm.resize(strnlen(Algorithm.c_str(), Algorithm.size()));
      if (Algorithm != "cubic" && Algorithm != "reno") return -Enoent;
      Store.Congestion = Algorithm;
      return 0;
    }
    default: return Unhandled;
    }
  }
  return Unhandled;
}

// The getsockopt side. Out gets the natural-size value; false when the pair is not ours.
inline bool LoadEmulated(const Emulated& Store, int Level, int Name, std::vector<uint8_t>& Out) {
  auto Int = [&](int32_t Value) {
    Out.resize(sizeof(Value));
    std::memcpy(Out.data(), &Value, sizeof(Value));
    return true;
  };
  if (Level == LinuxSolSocket) {
    switch (Name) {
    case 25: Out.clear(); return true; // SO_BINDTODEVICE: an empty name
    case 29: return Int(Store.Timestamp);
    case 19: return Int(Store.SendLowat);
    case 49: return Int(Store.IncomingCpu);
    case 46: return Int(Store.BusyPoll);
    default: return false;
    }
  }
  if (Level == LinuxIpprotoIp) {
    switch (Name) {
    case 11: return Int(Store.RecvErr);
    case 10: return Int(Store.MtuDiscover);
    default: return false;
    }
  }
  if (Level == LinuxIpprotoTcp) {
    switch (Name) {
    case 3: return Int(Store.Cork);
    case 7: return Int(Store.Syncnt ? Store.Syncnt : 6);
    case 8: return Int(Store.Linger2 ? Store.Linger2 : 60);
    case 9: return Int(Store.DeferAccept);
    case 10: return Int(Store.WindowClamp);
    case 12: return Int(Store.QuickAck);
    case 18: return Int(Store.UserTimeout);
    case 23: return Int(Store.FastOpen);
    case 25: return Int(Store.NotsentLowat);
    case 13:
      Out.assign(Store.Congestion.begin(), Store.Congestion.end());
      Out.push_back(0);
      return true;
    default: return false;
    }
  }
  return false;
}

// ----- Message flags -----
// Linux MSG_* of a send to the host's. Bits the runtime handles itself
// (MSG_NOSIGNAL, MSG_MORE, MSG_CONFIRM) are removed. Unsupported collects
// every other bit with no host equivalent.
inline std::optional<int> MapSendFlags(uint64_t Linux, uint64_t& Unsupported) {
  int Bsd {};
  Unsupported = 0;
  if (Linux & MsgOob) Bsd |= BsdMsgOob;
  if (Linux & MsgDontRoute) Bsd |= BsdMsgDontRoute;
  if (Linux & MsgDontWait) Bsd |= BsdMsgDontWait;
  if (Linux & MsgEor) Bsd |= BsdMsgEor;
  Unsupported = Linux & ~(MsgOob | MsgDontRoute | MsgDontWait | MsgEor | SendRuntimeFlags);
  return Bsd;
}

// Receive: MSG_TRUNC, MSG_WAITALL, MSG_ERRQUEUE, MSG_WAITFORONE and
// MSG_CMSG_CLOEXEC are the runtime's.
inline std::optional<int> MapReceiveFlags(uint64_t Linux, uint64_t& Unsupported) {
  int Bsd {};
  Unsupported = 0;
  if (Linux & MsgPeek) Bsd |= BsdMsgPeek;
  if (Linux & MsgOob) Bsd |= BsdMsgOob;
  if (Linux & MsgDontWait) Bsd |= BsdMsgDontWait;
  Unsupported = Linux & ~(MsgPeek | MsgOob | MsgDontWait | ReceiveRuntimeFlags);
  return Bsd;
}

// msg_flags the host returned, in Linux bits.
inline uint64_t LinuxMessageFlags(int Bsd) {
  uint64_t Linux {};
  if (Bsd & BsdMsgOob) Linux |= MsgOob;
  if (Bsd & BsdMsgEor) Linux |= MsgEor;
  if (Bsd & BsdMsgTrunc) Linux |= MsgTrunc;
  if (Bsd & BsdMsgCtrunc) Linux |= MsgCtrunc;
  return Linux;
}

// ----- Poll bits -----
inline int16_t ToBsdPoll(int16_t Linux) {
  int16_t Bsd {};
  if (Linux & 1) Bsd |= BsdPollIn;
  if (Linux & 2) Bsd |= BsdPollPri;
  if (Linux & 4) Bsd |= BsdPollOut;
  return Bsd;
}

// Linux poll bits of a host revents. BSD has no POLLRDHUP: the runtime
// derives it (PeerClosedWrite: a readable TCP socket with FIONREAD 0).
inline int16_t FromBsdPoll(int16_t Bsd, bool PeerClosedWrite = false) {
  int16_t Linux {};
  if (Bsd & BsdPollIn) Linux |= 1;
  if (Bsd & BsdPollPri) Linux |= 2;
  if (Bsd & BsdPollOut) Linux |= 4;
  if (Bsd & BsdPollErr) Linux |= 8;
  if (Bsd & BsdPollHup) Linux |= 0x10;
  if (Bsd & BsdPollNval) Linux |= 0x20;
  if (PeerClosedWrite) Linux |= 0x2000;
  return Linux;
}

// ----- Addresses -----
// Linux sockaddr_in: u16 family; u16 port (network order); u32 address (network order); u8 zero[8].
// BSD: u8 len; u8 family; the rest as above. Both 16 bytes.
constexpr size_t AddressSize = 16;

inline int64_t ToBsdAddress(std::span<const uint8_t> Linux, std::array<uint8_t, AddressSize>& Bsd) {
  if (Linux.size() < AddressSize) return -Einval;
  uint16_t Family {};
  std::memcpy(&Family, Linux.data(), sizeof(Family));
  if (Family != LinuxAfInet) return -Eafnosupport;
  Bsd = {};
  Bsd[0] = AddressSize;
  Bsd[1] = BsdAfInet;
  std::memcpy(&Bsd[2], &Linux[2], 6); // port and address
  return 0;
}

// The Linux form of a BSD address; 0 when it is not AF_INET.
inline size_t ToLinuxAddress(std::span<const uint8_t> Bsd, std::array<uint8_t, AddressSize>& Linux) {
  if (Bsd.size() < AddressSize || Bsd[1] != BsdAfInet) return 0;
  Linux = {};
  Linux[0] = LinuxAfInet;
  std::memcpy(&Linux[2], &Bsd[2], 6);
  return AddressSize;
}

// ----- errno -----
// The service reports Linux numbers in g_bsdErrno (m3probe 2). A failed
// call that left none is EIO. Values outside 1..133 cannot be Linux errnos.
inline int LinuxErrno(int HostErrno) {
  if (HostErrno <= 0 || HostErrno > 133) return Eio;
  return HostErrno;
}

// ----- Address helpers (network byte order values) -----
constexpr uint32_t Ip4(uint8_t A, uint8_t B, uint8_t C, uint8_t D) {
  // Stored as the bytes A.B.C.D in memory, like sin_addr.
  return static_cast<uint32_t>(A) | static_cast<uint32_t>(B) << 8 | static_cast<uint32_t>(C) << 16 | static_cast<uint32_t>(D) << 24;
}
constexpr uint32_t LoopbackAddress = Ip4(127, 0, 0, 1);
constexpr bool IsLoopback(uint32_t NetworkOrder) { return (NetworkOrder & 0xff) == 127; }

// ----- Socket state -----
// What a guest AF_INET descriptor knows. The host descriptor lives with the
// runtime (it must be closed there); everything the guest can observe is
// here. Guest addresses are network-order address and host-order port.
struct State : SocketOptions {
  uint8_t Type {};        // 1 stream, 2 datagram, 3 raw
  int Protocol {};        // Linux IPPROTO_*
  bool Nonblocking {};
  bool Bound {};
  bool Listening {};
  bool Connecting {};     // a nonblocking host connect is in flight
  bool ConnectAck {};     // the connect finished; the next connect() reports it (Linux SS_CONNECTING)
  int32_t ConnectError {}; // the Linux errno of that outcome, 0 for success
  bool Connected {};
  bool ShutRead {}, ShutWrite {};
  int32_t PendingError {};  // a Linux errno the next SO_ERROR or receive reports once
  uint32_t GuestAddress {}; // what getsockname shows (0 for the wildcard)
  uint16_t GuestPort {};
  uint32_t PeerAddress {};
  uint16_t PeerPort {};
  Emulated Extra;
  uint64_t Generation {};   // bumped when the host reports different readiness
  int16_t LastEvents {};
  int16_t WaitEvents {};    // events a blocked call waits for (Linux poll bits); the scheduler's poll ends the wait on them
};

// struct tcp_info is 232 bytes on Linux 7.x; this runtime fills tcpi_state only.
constexpr size_t TcpInfoSize = 232;
inline std::vector<uint8_t> TcpInfo(const State& Socket) {
  std::vector<uint8_t> Info(TcpInfoSize);
  // TCP_ESTABLISHED 1, TCP_SYN_SENT 2, TCP_CLOSE 7, TCP_LISTEN 10.
  Info[0] = Socket.Listening ? 10 : Socket.Connected ? 1 : Socket.Connecting ? 2 : 7;
  return Info;
}

} // namespace Switch2Guest::Inet
