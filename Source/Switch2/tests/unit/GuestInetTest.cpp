// SPDX-License-Identifier: MIT
// Linux <-> Horizon BSD translation for AF_INET. Linux values come from
// native Linux 7.3 headers; BSD values from libounce's headers.
#include "GuestInet.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(x)                                                     \
  do {                                                               \
    if (!(x)) {                                                      \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x);        \
      std::exit(1);                                                  \
    }                                                                \
  } while (0)

using namespace Switch2Guest;
using namespace Switch2Guest::Inet;

static std::array<uint8_t, 16> LinuxAddress(uint16_t PortHost, uint32_t Address) {
  std::array<uint8_t, 16> Bytes {};
  Bytes[0] = 2;
  Bytes[2] = PortHost >> 8;
  Bytes[3] = PortHost & 0xff;
  std::memcpy(&Bytes[4], &Address, 4);
  return Bytes;
}

static void Addresses() {
  const auto Linux = LinuxAddress(8080, Ip4(10, 1, 2, 3));
  std::array<uint8_t, 16> Bsd {};
  CHECK(ToBsdAddress(Linux, Bsd) == 0);
  CHECK(Bsd[0] == 16 && Bsd[1] == 2);
  CHECK(Bsd[2] == 0x1f && Bsd[3] == 0x90);
  CHECK(Bsd[4] == 10 && Bsd[5] == 1 && Bsd[6] == 2 && Bsd[7] == 3);
  std::array<uint8_t, 16> Back {};
  CHECK(ToLinuxAddress(Bsd, Back) == 16);
  CHECK(Back == Linux);
  // Too short: EINVAL (inet_bind and inet_stream_connect check the length first).
  CHECK(ToBsdAddress(std::span<const uint8_t>(Linux.data(), 15), Bsd) == -22);
  CHECK(ToBsdAddress(std::span<const uint8_t>(Linux.data(), 1), Bsd) == -22);
  // AF_INET6 (10) with a full buffer: EAFNOSUPPORT.
  auto V6 = Linux;
  V6[0] = 10;
  CHECK(ToBsdAddress(V6, Bsd) == -97);
  // A longer buffer is fine, only 16 bytes are used.
  std::array<uint8_t, 28> Long {};
  std::memcpy(Long.data(), Linux.data(), 16);
  CHECK(ToBsdAddress(Long, Bsd) == 0);
  // A BSD address of another family does not convert.
  Bsd[1] = 28;
  CHECK(ToLinuxAddress(Bsd, Back) == 0);
}

static void Options() {
  // Every mapped option maps both ways.
  for (const auto& Entry : OptionTable) {
    const auto Forward = MapOption(Entry.LinuxLevel, Entry.LinuxName);
    CHECK(Forward && Forward->BsdName == Entry.BsdName && Forward->BsdLevel == Entry.BsdLevel);
    const auto Reverse = UnmapOption(Entry.BsdLevel, Entry.BsdName);
    CHECK(Reverse && Reverse->LinuxLevel == Entry.LinuxLevel && Reverse->LinuxName == Entry.LinuxName);
  }
  // Spot values: SOL_SOCKET moves to 0xffff, SO_REUSEADDR 2 -> 4, TCP_KEEPIDLE 4 -> 256.
  CHECK(MapOption(1, 2)->BsdLevel == 0xffff && MapOption(1, 2)->BsdName == 4);
  CHECK(MapOption(1, 6)->BsdName == 0x20);   // SO_BROADCAST
  CHECK(MapOption(1, 15)->BsdName == 0x200); // SO_REUSEPORT
  CHECK(MapOption(1, 13)->Type == OptionKind::Linger);
  CHECK(MapOption(0, 1)->BsdName == 3 && MapOption(0, 2)->BsdName == 4); // IP_TOS, IP_TTL
  CHECK(MapOption(0, 33)->Type == OptionKind::Byte && MapOption(0, 35)->Type == OptionKind::Membership);
  CHECK(MapOption(6, 4)->BsdName == 256 && MapOption(6, 5)->BsdName == 512 && MapOption(6, 6)->BsdName == 1024);
  CHECK(MapOption(6, 1)->BsdName == 1);
  // Unknown options: nullopt (ENOPROTOOPT).
  CHECK(!MapOption(1, 9999));
  CHECK(!MapOption(41, 1));
  CHECK(!MapOption(6, 3)); // TCP_CORK is emulated, never forwarded
  // Range checks Linux makes itself.
  CHECK(CheckOptionRange(6, 4, 0) == -22 && CheckOptionRange(6, 4, 1) == 0 && CheckOptionRange(6, 4, 32768) == -22);
  CHECK(CheckOptionRange(6, 6, 128) == -22 && CheckOptionRange(6, 6, 127) == 0);
  CHECK(CheckOptionRange(6, 2, 7) == -22 && CheckOptionRange(6, 2, 1460) == 0);
  CHECK(CheckOptionRange(0, 1, -5) == 0);
}

static void Emulation() {
  Emulated Store;
  std::vector<uint8_t> Out;
  const int32_t One = 1, Minus = -1;
  uint8_t Raw[4];
  std::memcpy(Raw, &One, 4);
  CHECK(StoreEmulated(Store, 6, 3, Raw, 4) == 0); // TCP_CORK
  CHECK(LoadEmulated(Store, 6, 3, Out) && Out.size() == 4 && Out[0] == 1);
  CHECK(StoreEmulated(Store, 6, 3, Raw, 2) == -22);
  std::memcpy(Raw, &Minus, 4);
  CHECK(StoreEmulated(Store, 6, 18, Raw, 4) == -22); // TCP_USER_TIMEOUT must not be negative
  CHECK(StoreEmulated(Store, 6, 12, Raw, 4) == 0 && Store.QuickAck == 1); // any nonzero is true
  // TCP_CONGESTION
  CHECK(LoadEmulated(Store, 6, 13, Out) && std::string(Out.begin(), Out.end() - 1) == "cubic" && Out.back() == 0);
  CHECK(StoreEmulated(Store, 6, 13, reinterpret_cast<const uint8_t*>("reno"), 4) == 0 && Store.Congestion == "reno");
  CHECK(StoreEmulated(Store, 6, 13, reinterpret_cast<const uint8_t*>("bbr"), 3) == -2);
  CHECK(StoreEmulated(Store, 6, 13, reinterpret_cast<const uint8_t*>("cubic\0"), 6) == 0 && Store.Congestion == "cubic");
  CHECK(StoreEmulated(Store, 6, 13, reinterpret_cast<const uint8_t*>(""), 0) == -22);
  // SO_BINDTODEVICE: empty only.
  CHECK(StoreEmulated(Store, 1, 25, reinterpret_cast<const uint8_t*>(""), 1) == 0);
  CHECK(StoreEmulated(Store, 1, 25, reinterpret_cast<const uint8_t*>(""), 0) == 0);
  CHECK(StoreEmulated(Store, 1, 25, reinterpret_cast<const uint8_t*>("eth0"), 4) == -19);
  CHECK(LoadEmulated(Store, 1, 25, Out) && Out.empty());
  // IP_MTU_DISCOVER range.
  const int32_t Four = 4;
  std::memcpy(Raw, &Four, 4);
  CHECK(StoreEmulated(Store, 0, 10, Raw, 4) == -22);
  // Not ours.
  CHECK(StoreEmulated(Store, 6, 1, Raw, 4) == Unhandled);
  CHECK(!LoadEmulated(Store, 6, 1, Out));
  CHECK(StoreEmulated(Store, 1, 2, Raw, 4) == Unhandled);
  // Defaults Linux reports.
  CHECK(LoadEmulated(Store, 6, 7, Out) && Out[0] == 6);
  CHECK(LoadEmulated(Store, 1, 19, Out) && Out[0] == 1); // SO_SNDLOWAT
}

static void Flags() {
  uint64_t Unsupported = 123;
  // MSG_DONTWAIT 0x40 -> BSD 0x80.
  CHECK(*MapSendFlags(MsgDontWait, Unsupported) == BsdMsgDontWait && Unsupported == 0);
  CHECK(*MapReceiveFlags(MsgDontWait, Unsupported) == BsdMsgDontWait && Unsupported == 0);
  // MSG_NOSIGNAL (0x4000) is the runtime's: never passed to the host.
  CHECK(*MapSendFlags(MsgNoSignal, Unsupported) == 0 && Unsupported == 0);
  CHECK(*MapSendFlags(MsgNoSignal | MsgDontWait | MsgOob, Unsupported) == (BsdMsgDontWait | BsdMsgOob) && Unsupported == 0);
  CHECK(*MapSendFlags(MsgEor | MsgDontRoute, Unsupported) == (BsdMsgEor | BsdMsgDontRoute));
  // Unsupported bits are reported (MSG_SYN 0x400, MSG_FASTOPEN 0x20000000).
  CHECK(*MapSendFlags(0x400 | MsgDontWait, Unsupported) == BsdMsgDontWait && Unsupported == 0x400);
  CHECK(MapSendFlags(0x20000000, Unsupported) && Unsupported == 0x20000000);
  CHECK(*MapReceiveFlags(MsgPeek | MsgTrunc | MsgWaitAll, Unsupported) == BsdMsgPeek && Unsupported == 0);
  CHECK(MapReceiveFlags(MsgEor, Unsupported) && Unsupported == MsgEor);
  CHECK(*MapReceiveFlags(MsgOob, Unsupported) == BsdMsgOob);
  // Message flags back.
  CHECK(LinuxMessageFlags(BsdMsgTrunc) == MsgTrunc);
  CHECK(LinuxMessageFlags(BsdMsgCtrunc | BsdMsgEor) == (MsgCtrunc | MsgEor));
  CHECK(LinuxMessageFlags(0) == 0);
}

static void Poll() {
  CHECK(ToBsdPoll(1 | 4) == (BsdPollIn | BsdPollOut));
  CHECK(FromBsdPoll(BsdPollIn | BsdPollHup) == (1 | 0x10));
  CHECK(FromBsdPoll(BsdPollErr | BsdPollNval) == (8 | 0x20));
  CHECK(FromBsdPoll(BsdPollIn, true) == (1 | 0x2000));
  CHECK(FromBsdPoll(0) == 0);
}

static void Errors() {
  CHECK(LinuxErrno(111) == 111);
  CHECK(LinuxErrno(11) == 11);
  CHECK(LinuxErrno(0) == 5);
  CHECK(LinuxErrno(-4) == 5);
  CHECK(LinuxErrno(5000) == 5);
}

static void Info() {
  State Socket;
  CHECK(TcpInfo(Socket).size() == 232 && TcpInfo(Socket)[0] == 7);
  Socket.Connected = true;
  CHECK(TcpInfo(Socket)[0] == 1);
  Socket.Connected = false;
  Socket.Listening = true;
  CHECK(TcpInfo(Socket)[0] == 10);
  Socket.Listening = false;
  Socket.Connecting = true;
  CHECK(TcpInfo(Socket)[0] == 2);
}

int main() {
  Addresses();
  Options();
  Emulation();
  Flags();
  Poll();
  Errors();
  Info();
  std::puts("guest inet tests passed");
  return 0;
}
