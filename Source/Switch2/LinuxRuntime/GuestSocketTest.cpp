// SPDX-License-Identifier: MIT
// AF_UNIX datagram, seqpacket and stream socket logic. Expected values come
// from native Linux 7.3 runs (see CommunicationInventory.md, Sockets).
#include "../GuestSocket.h"
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
using Socket = StreamSocket<int>;
using Ptr = std::shared_ptr<Socket>;

static Ptr Make(SocketKind Kind) {
  auto S = std::make_shared<Socket>();
  S->Kind = Kind;
  return S;
}

static std::pair<Ptr, Ptr> Pair(SocketKind Kind) {
  auto A = Make(Kind), B = Make(Kind);
  A->Connect(B);
  B->Connect(A);
  return {A, B};
}

static std::vector<uint8_t> Bytes(size_t Count, uint8_t Fill = 'a') { return std::vector<uint8_t>(Count, Fill); }

static bool Send(Socket& S, size_t Count, bool Eor = false) {
  const auto Data = Bytes(Count);
  return S.TrySend(Data, {}, {}, {}, Eor) == Socket::SendStatus::Ok;
}

int main() {
  // 1. Datagram boundaries are kept.
  {
    auto [A, B] = Pair(SocketKind::Datagram);
    CHECK(Send(*A, 3));
    CHECK(Send(*A, 5));
    CHECK(Send(*A, 0));
    CHECK(B->Records.size() == 3);
    uint8_t Out[16];
    size_t Full {};
    CHECK(B->ReceiveRecord(Out, false, Full) == 3 && Full == 3);
    CHECK(B->ReceiveRecord(Out, false, Full) == 5 && Full == 5);
    CHECK(B->ReceiveRecord(Out, false, Full) == 0 && Full == 0);
    CHECK(B->Records.empty() && B->Input.empty());
    CHECK(A->Wmem == 0);
  }
  // 2. A short buffer truncates, reports the full length and drops the rest.
  {
    auto [A, B] = Pair(SocketKind::Datagram);
    const std::vector<uint8_t> Text {'a', 'b', 'c', 'd', 'e', 'f'};
    CHECK(A->TrySend(Text, {}, {}, {}, false) == Socket::SendStatus::Ok);
    CHECK(Send(*A, 2));
    uint8_t Out[4] {};
    size_t Full {};
    CHECK(B->ReceiveRecord({Out, 2}, true, Full) == 2 && Full == 6 && Out[0] == 'a' && B->Records.size() == 2);
    CHECK(B->ReceiveRecord({Out, 2}, false, Full) == 2 && Full == 6 && Out[1] == 'b');
    CHECK(B->Records.size() == 1 && B->Input.size() == 2);
    CHECK(B->ReceiveRecord({Out, 0}, false, Full) == 0 && Full == 2 && B->Records.empty() && B->Input.empty());
  }
  // 3. SEQPACKET keeps boundaries and EOR.
  {
    auto [A, B] = Pair(SocketKind::SeqPacket);
    CHECK(Send(*A, 3, true));
    CHECK(Send(*A, 2, false));
    CHECK(B->Records[0].Eor && !B->Records[1].Eor);
    uint8_t Out[16];
    size_t Full {};
    CHECK(B->ReceiveRecord(Out, false, Full) == 3);
    CHECK(B->ReceiveRecord(Out, false, Full) == 2);
  }
  // 4. Rights belong to their record, with the sender's name and credentials.
  {
    auto [A, B] = Pair(SocketKind::Datagram);
    auto File = std::make_shared<int>(7);
    const auto Data = Bytes(1);
    CHECK(A->TrySend(Data, "/tmp/x", SocketCred {42, 1000, 1001}, {File}, false) == Socket::SendStatus::Ok);
    CHECK(Send(*A, 1));
    uint8_t Out[4];
    size_t Full {};
    std::string Source;
    SocketCred Cred;
    std::vector<std::shared_ptr<int>> Files;
    CHECK(B->ReceiveRecord(Out, true, Full, &Source, &Cred, &Files) == 1);
    CHECK(Files.size() == 1 && B->Records.front().Files.size() == 1);
    Files.clear();
    CHECK(B->ReceiveRecord(Out, false, Full, &Source, &Cred, &Files) == 1);
    CHECK(Source == "/tmp/x" && Cred.Pid == 42 && Cred.Uid == 1000 && Cred.Gid == 1001);
    CHECK(Files.size() == 1 && *Files[0] == 7);
    Files.clear();
    CHECK(B->ReceiveRecord(Out, false, Full, &Source, &Cred, &Files) == 1);
    CHECK(Files.empty() && Source.empty());
  }
  // 5. Queue limits. Native 7.3: one sender manages 278 one-byte and 93
  // thousand-byte datagrams (sender memory), many senders fill 513 records.
  {
    auto [A, B] = Pair(SocketKind::Datagram);
    int Count {};
    while (Send(*A, 1)) ++Count;
    CHECK(Count == 278);
    CHECK(A->TrySend(Bytes(1), {}, {}, {}, false) == Socket::SendStatus::Full);
    uint8_t Out[4];
    size_t Full {};
    CHECK(B->ReceiveRecord(Out, false, Full) == 1);
    CHECK(A->Wmem + Socket::Truesize(1) == 278 * Socket::Truesize(1));
    CHECK(Send(*A, 1));
  }
  {
    auto [A, B] = Pair(SocketKind::Datagram);
    int Count {};
    while (Send(*A, 1000)) ++Count;
    CHECK(Count == 93);
    auto [C, D] = Pair(SocketKind::SeqPacket);
    Count = 0;
    while (Send(*C, 1)) ++Count;
    CHECK(Count == 278);
  }
  {
    auto Receiver = Make(SocketKind::Datagram);
    std::vector<Ptr> Senders;
    int Count {};
    for (;;) {
      auto Sender = Make(SocketKind::Datagram);
      Sender->Connect(Receiver);
      if (!Send(*Sender, 1)) break;
      Senders.push_back(Sender);
      ++Count;
    }
    CHECK(Count == 513);
    // A datagram larger than the send buffer minus 32 never fits.
    CHECK(Receiver->MaxRecord() == 212992 - 32);
    Senders.clear();
    CHECK(Receiver->Records.size() == 513);
  }
  // Dropping the receiver gives back what its queue charged to the sender.
  {
    auto Sender = Make(SocketKind::Datagram);
    {
      auto Receiver = Make(SocketKind::Datagram);
      Sender->Connect(Receiver);
      CHECK(Send(*Sender, 10) && Send(*Sender, 10));
      CHECK(Sender->Wmem == 2 * Socket::Truesize(10));
    }
    CHECK(Sender->Wmem == 0);
    CHECK(Send(*Sender, 1) == false); // peer is gone
  }
  // 6. A peer that closes with unread input resets the other end once.
  {
    auto [A, B] = Pair(SocketKind::Stream);
    CHECK(A->Send(Bytes(2)) == 2);
    CHECK(B->Send(Bytes(3)) == 3);
    B.reset(); // B has A's two bytes unread
    CHECK(A->ResetPending);
    CHECK(A->Input.size() == 3); // data first, then the error
    auto [C, D] = Pair(SocketKind::Stream);
    D.reset(); // nothing unread: a plain EOF
    CHECK(!C->ResetPending && C->EndOfStream());
    auto [E, F] = Pair(SocketKind::SeqPacket);
    CHECK(F->TrySend(Bytes(1), {}, {}, {}, false) == Socket::SendStatus::Ok);
    E.reset();
    CHECK(F->ResetPending);
    auto [G, H] = Pair(SocketKind::Datagram);
    CHECK(Send(*H, 1));
    G.reset();
    CHECK(!H->ResetPending);
    // Queued connections that never got accepted reset their connector.
    auto Listener = Make(SocketKind::Stream);
    auto Client = Make(SocketKind::Stream);
    auto Embryo = Make(SocketKind::Stream);
    Embryo->Embryo = true;
    Embryo->Connect(Client);
    Client->Connect(Embryo);
    Listener->Pending.push_back(Embryo);
    Embryo.reset();
    Listener.reset();
    CHECK(Client->ResetPending);
  }
  // 7. Poll of datagram sockets.
  {
    auto [A, B] = Pair(SocketKind::Datagram);
    CHECK((A->Poll(4) & 4));
    CHECK(!(B->Poll(1) & 1));
    CHECK(Send(*A, 4));
    CHECK((B->Poll(1) & 1));
    CHECK((A->Poll(4) & 4));
    while (Send(*A, 1)) {}
    CHECK(!(A->Poll(4) & 4)); // sender memory exhausted
    auto Loose = Make(SocketKind::Datagram);
    CHECK((Loose->Poll(5) & 4) && !(Loose->Poll(5) & 0x10)); // unconnected, still writable
    // Connected to a peer whose queue is full: not writable.
    auto Receiver = Make(SocketKind::Datagram);
    std::vector<Ptr> Senders;
    for (int I = 0; I < 513; ++I) {
      auto S = Make(SocketKind::Datagram);
      S->Connect(Receiver);
      CHECK(Send(*S, 1));
      Senders.push_back(S);
    }
    auto Extra = Make(SocketKind::Datagram);
    Extra->Connect(Receiver);
    CHECK(!(Extra->Poll(4) & 4));
    uint8_t Out[4];
    size_t Full {};
    CHECK(Receiver->ReceiveRecord(Out, false, Full) == 1);
    CHECK((Extra->Poll(4) & 4));
    // Shutdown of the receive side: readable (EOF) and RDHUP.
    Loose->ShutRead = true;
    CHECK((Loose->Poll(1 | 0x2000) & 1) && (Loose->Poll(0x2000) & 0x2000));
  }
  // Seqpacket readiness follows its records.
  {
    auto [A, B] = Pair(SocketKind::SeqPacket);
    CHECK(!(B->Poll(1) & 1) && (A->Poll(4) & 4));
    CHECK(Send(*A, 0));
    CHECK((B->Poll(1) & 1));
    A->ShutWrite = true;
    CHECK(B->EndOfStream());
  }
  // A stream read may take plain bytes before a rights message and ends with
  // that message (Linux 7.3: 3 plain, 5 with SCM_RIGHTS, 7 plain reads 8, 7).
  {
    auto [A, B] = Pair(SocketKind::Stream);
    const auto Three = Bytes(3), Five = Bytes(5), Seven = Bytes(7);
    CHECK(A->Send(Three) == 3);
    CHECK(A->Send(Five, {std::make_shared<int>(1)}) == 5);
    CHECK(A->Send(Seven) == 7);
    CHECK(B->MessageBytes(100) == 8 && B->MessageRights(8));
    CHECK(B->MessageBytes(2) == 2 && !B->MessageRights(2));
    B->Consume(8);
    CHECK(B->MessageBytes(100) == 7 && !B->MessageRights(7));
    B->Consume(7);
    // Two rights messages in a row are two reads.
    CHECK(A->Send(Five, {std::make_shared<int>(2)}) == 5 && A->Send(Five, {std::make_shared<int>(3)}) == 5);
    CHECK(B->MessageBytes(100) == 5);
    B->Consume(5);
    CHECK(B->MessageBytes(100) == 5 && B->MessageRights(5));
  }
  // Unconnected stream and seqpacket sockets poll POLLOUT|POLLHUP (0x14).
  {
    CHECK(Make(SocketKind::Stream)->Poll(5) == 0x14);
    CHECK(Make(SocketKind::SeqPacket)->Poll(5) == 0x14);
    CHECK(Make(SocketKind::Stream)->Poll(1) == 0x10);
  }
  // Datagram POLLOUT follows unix_writable ((wmem_alloc << 2) <= sndbuf):
  // Linux 7.3 drops it after 70 one-byte sends, long before EAGAIN at 278.
  {
    auto [A, B] = Pair(SocketKind::Datagram);
    int Sent {};
    while (A->Poll(4) & 4) {
      CHECK(Send(*A, 1));
      ++Sent;
    }
    CHECK(Sent == 70);
    while (Send(*A, 1)) ++Sent;
    CHECK(Sent == 278);
  }
  // Stream credentials travel in runs; one receive never mixes runs.
  {
    auto [A, B] = Pair(SocketKind::Stream);
    const auto One = Bytes(2), Two = Bytes(3);
    CHECK(A->Send(One, {}, true, false, SocketCred {1, 2, 3}) == 2);
    CHECK(A->Send(One, {}, true, false, SocketCred {1, 2, 3}) == 2);
    CHECK(A->Send(Two, {}, true, false, SocketCred {9, 2, 3}) == 3);
    CHECK(B->CredMarks.size() == 2);
    CHECK(B->MessageBytes(100) == 4 && B->FrontCred().Pid == 1);
    B->Consume(4);
    CHECK(B->MessageBytes(100) == 3 && B->FrontCred().Pid == 9);
    B->Consume(3);
    CHECK(B->CredMarks.empty());
  }
  // Socket options (net.core defaults on Debian 12 and Linux 7.3).
  {
    SocketOptions O;
    CHECK(O.SendBuffer == 212992 && O.ReceiveBuffer == 212992 && O.RcvLowat == 1 && O.PeekOff == -1);
    OptionValue V;
    auto SetInt = [&](int Name, int32_t Value, bool Capable = false, size_t Length = 4) {
      uint8_t Raw[16] {};
      std::memcpy(Raw, &Value, 4);
      return SetSocketOption(O, Name, Raw, Length, Capable);
    };
    auto GetInt = [&](int Name) {
      CHECK(GetSocketOption(O, Name, V) == 0 && V.Size == 4);
      int32_t Value;
      std::memcpy(&Value, V.Data.data(), 4);
      return Value;
    };
    CHECK(SetInt(SockOpt::SndBuf, 1) == 0 && GetInt(SockOpt::SndBuf) == 4608);
    CHECK(SetInt(SockOpt::RcvBuf, 1) == 0 && GetInt(SockOpt::RcvBuf) == 2304);
    CHECK(SetInt(SockOpt::RcvBuf, 100000) == 0 && GetInt(SockOpt::RcvBuf) == 200000);
    CHECK(SetInt(SockOpt::RcvBuf, 1000000) == 0 && GetInt(SockOpt::RcvBuf) == 425984);
    CHECK(SetInt(SockOpt::SndBufForce, 5) == -1);
    CHECK(SetInt(SockOpt::SndBufForce, 5, true) == 0 && GetInt(SockOpt::SndBuf) == 4608);
    CHECK(SetInt(SockOpt::RcvBufForce, 1000000, true) == 0 && GetInt(SockOpt::RcvBuf) == 2000000);
    CHECK(SetInt(SockOpt::SndBuf, 5, false, 2) == -22);
    CHECK(SetInt(SockOpt::PassCred, 7) == 0 && GetInt(SockOpt::PassCred) == 1 && O.PassCred);
    CHECK(SetInt(SockOpt::KeepAlive, 7) == 0 && GetInt(SockOpt::KeepAlive) == 1);
    CHECK(SetInt(SockOpt::ReuseAddr, 7) == 0 && GetInt(SockOpt::ReuseAddr) == 1);
    CHECK(SetInt(SockOpt::PassSec, 1) == 0 && GetInt(SockOpt::PassSec) == 1);
    CHECK(SetInt(SockOpt::RcvLowat, 0) == 0 && GetInt(SockOpt::RcvLowat) == 1);
    CHECK(SetInt(SockOpt::RcvLowat, -5) == 0 && GetInt(SockOpt::RcvLowat) == 2147483647);
    CHECK(SetInt(SockOpt::PeekOff, -5) == 0 && GetInt(SockOpt::PeekOff) == -5);
    CHECK(SetInt(SockOpt::Type, 1) == -92 && SetInt(SockOpt::Error, 1) == -92 && SetInt(SockOpt::PeerCred, 1) == -92);
    CHECK(SetInt(9999, 1) == -92 && GetSocketOption(O, 9999, V) == -92);
    CHECK(SetInt(SockOpt::Type, 1, false, 0) == -22); // length is checked first
    // SO_LINGER
    uint8_t Linger[16] {};
    const int32_t Pair[2] {1, 6};
    std::memcpy(Linger, Pair, 8);
    CHECK(SetSocketOption(O, SockOpt::Linger, Linger, 7, false) == -22);
    CHECK(SetSocketOption(O, SockOpt::Linger, Linger, 8, false) == 0);
    CHECK(GetSocketOption(O, SockOpt::Linger, V) == 0 && V.Size == 8 && V.Data[0] == 1 && V.Data[4] == 6);
    // l_onoff 0 keeps the old linger time (Linux 7.3).
    const int32_t Off[2] {0, 7};
    std::memcpy(Linger, Off, 8);
    CHECK(SetSocketOption(O, SockOpt::Linger, Linger, 8, false) == 0);
    CHECK(GetSocketOption(O, SockOpt::Linger, V) == 0 && V.Data[0] == 0 && V.Data[4] == 6);
    // Timeouts: struct timeval, microseconds rounded up to a millisecond.
    uint8_t Time[16] {};
    int64_t Parts[2] {1, 500000};
    std::memcpy(Time, Parts, 16);
    CHECK(SetSocketOption(O, SockOpt::RcvTimeoOld, Time, 15, false) == -22);
    CHECK(SetSocketOption(O, SockOpt::RcvTimeoOld, Time, 16, false) == 0 && O.RcvTimeoutNs == 1'500'000'000);
    CHECK(GetSocketOption(O, SockOpt::RcvTimeoOld, V) == 0 && V.Size == 16);
    std::memcpy(Parts, V.Data.data(), 16);
    CHECK(Parts[0] == 1 && Parts[1] == 500000);
    Parts[0] = 0;
    Parts[1] = 1;
    std::memcpy(Time, Parts, 16);
    CHECK(SetSocketOption(O, SockOpt::SndTimeoOld, Time, 16, false) == 0 && O.SndTimeoutNs == 1'000'000);
    Parts[1] = 2'000'000;
    std::memcpy(Time, Parts, 16);
    CHECK(SetSocketOption(O, SockOpt::RcvTimeoOld, Time, 16, false) == -33);
    Parts[0] = -1;
    Parts[1] = 0;
    std::memcpy(Time, Parts, 16);
    CHECK(SetSocketOption(O, SockOpt::RcvTimeoOld, Time, 16, false) == 0 && O.RcvTimeoutNs == 0);
    CHECK(RequiredSetSize(SockOpt::Linger) == 8 && RequiredSetSize(SockOpt::RcvTimeoOld) == 16 && RequiredSetSize(SockOpt::SndBuf) == 4);
  }
  // Autobind names: '\0' and five lowercase hex digits.
  {
    const auto Name = AutobindName(0xabc);
    CHECK(Name.size() == 6 && Name[0] == '\0' && Name.substr(1) == "00abc");
    CHECK(AutobindName(0xfffff).substr(1) == "fffff");
  }
  std::puts("guest-socket ok");
  return 0;
}
