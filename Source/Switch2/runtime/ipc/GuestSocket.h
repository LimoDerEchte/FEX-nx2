// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Switch2Guest {
enum class SocketKind : uint8_t { Stream, Datagram, SeqPacket };

// struct ucred. The defaults are what SO_PEERCRED reports on a socket that
// has no peer yet (pid 0, uid and gid -1).
struct SocketCred {
  int32_t Pid {};
  uint32_t Uid {UINT32_MAX}, Gid {UINT32_MAX};
  bool operator==(const SocketCred&) const = default;
};

// Linux SOL_SOCKET (1) option numbers.
namespace SockOpt {
constexpr int Debug = 1, ReuseAddr = 2, Type = 3, Error = 4, DontRoute = 5, Broadcast = 6, SndBuf = 7, RcvBuf = 8,
              KeepAlive = 9, OobInline = 10, NoCheck = 11, Priority = 12, Linger = 13, ReusePort = 15, PassCred = 16,
              PeerCred = 17, RcvLowat = 18, SndLowat = 19, RcvTimeoOld = 20, SndTimeoOld = 21, AcceptConn = 30,
              SndBufForce = 32, RcvBufForce = 33, PassSec = 34, Protocol = 38, Domain = 39, PeekOff = 42,
              PeerGroups = 59, RcvTimeoNew = 66, SndTimeoNew = 67, PeerPidfd = 77;
} // namespace SockOpt

// State of the SOL_SOCKET options every socket family shares. Buffer sizes
// are stored as Linux reports them (already doubled).
struct SocketOptions {
  static constexpr size_t DefaultBuffer = 212992; // net.core.wmem_default and rmem_default
  static constexpr int64_t MaxBuffer = 212992;    // net.core.wmem_max and rmem_max
  size_t SendBuffer {DefaultBuffer}, ReceiveBuffer {DefaultBuffer};
  bool PassCred {}, PassSec {};
  int32_t Linger[2] {};
  int32_t RcvLowat {1};
  int32_t PeekOff {-1};
  int32_t Priority {};
  uint32_t Flags {}; // bit per stored boolean option, see FlagBit
  uint64_t RcvTimeoutNs {}, SndTimeoutNs {};

  static int FlagBit(int Name) {
    switch (Name) {
    case SockOpt::Debug: return 0;
    case SockOpt::ReuseAddr: return 1;
    case SockOpt::DontRoute: return 2;
    case SockOpt::Broadcast: return 3;
    case SockOpt::KeepAlive: return 4;
    case SockOpt::OobInline: return 5;
    case SockOpt::NoCheck: return 6;
    case SockOpt::ReusePort: return 7;
    default: return -1;
    }
  }
  bool Flag(int Name) const { return (Flags >> FlagBit(Name)) & 1; }
};

constexpr int OptionUnhandled = INT_MIN;

struct OptionValue {
  std::array<uint8_t, 16> Data {};
  size_t Size {};
};

// Bytes the option needs from the caller (Linux checks optlen before it
// reads the value, so a short length is EINVAL even for a bad pointer).
inline size_t RequiredSetSize(int Name) {
  switch (Name) {
  case SockOpt::Linger: return 8;
  case SockOpt::RcvTimeoOld: case SockOpt::SndTimeoOld: case SockOpt::RcvTimeoNew: case SockOpt::SndTimeoNew: return 16;
  default: return 4;
  }
}

// sock_setsockopt for the options that need no socket. Returns 0 or a
// negative errno. Options the family has to answer itself (SO_TYPE,
// SO_ERROR, SO_DOMAIN, SO_PROTOCOL, SO_ACCEPTCONN, SO_PEER*) are read-only
// everywhere: ENOPROTOOPT.
inline int SetSocketOption(SocketOptions& Options, int Name, const uint8_t* Value, size_t Length, bool CanNetAdmin) {
  if (Length < RequiredSetSize(Name)) return -22;
  int32_t Int {};
  std::memcpy(&Int, Value, sizeof(Int));
  auto Buffer = [&](size_t& Out, int64_t Minimum, bool Force) {
    int64_t Capped = Force ? int64_t {Int} : std::min<int64_t>(static_cast<uint32_t>(Int), SocketOptions::MaxBuffer);
    // INT_MAX / 2 keeps the doubling inside an int, as Linux does.
    Capped = std::min<int64_t>(Capped, INT_MAX / 2);
    Out = static_cast<size_t>(std::max<int64_t>(Capped * 2, Minimum));
  };
  switch (Name) {
  case SockOpt::SndBuf: Buffer(Options.SendBuffer, 4608, false); return 0;
  case SockOpt::RcvBuf: Buffer(Options.ReceiveBuffer, 2304, false); return 0;
  case SockOpt::SndBufForce:
    if (!CanNetAdmin) return -1;
    Buffer(Options.SendBuffer, 4608, true);
    return 0;
  case SockOpt::RcvBufForce:
    if (!CanNetAdmin) return -1;
    Buffer(Options.ReceiveBuffer, 2304, true);
    return 0;
  case SockOpt::PassCred: Options.PassCred = Int != 0; return 0;
  case SockOpt::PassSec: Options.PassSec = Int != 0; return 0;
  case SockOpt::RcvLowat:
    Options.RcvLowat = Int < 0 ? INT_MAX : Int ? Int : 1;
    return 0;
  case SockOpt::PeekOff: Options.PeekOff = Int; return 0;
  case SockOpt::Priority:
    // Above 6 needs CAP_NET_RAW or CAP_NET_ADMIN.
    if ((Int < 0 || Int > 6) && !CanNetAdmin) return -1;
    Options.Priority = Int;
    return 0;
  case SockOpt::Linger: {
    // l_onoff 0 clears SOCK_LINGER and keeps the old linger time.
    int32_t Linger[2];
    std::memcpy(Linger, Value, sizeof(Linger));
    Options.Linger[0] = Linger[0] != 0;
    if (Linger[0]) Options.Linger[1] = Linger[1];
    return 0;
  }
  case SockOpt::RcvTimeoOld: case SockOpt::SndTimeoOld: case SockOpt::RcvTimeoNew: case SockOpt::SndTimeoNew: {
    int64_t Parts[2];
    std::memcpy(Parts, Value, sizeof(Parts));
    if (Parts[1] < 0 || Parts[1] >= 1'000'000) return -33; // EDOM
    uint64_t Nanoseconds {};
    if (Parts[0] >= 0) {
      // Linux rounds up to a jiffy (HZ 1000) and treats anything that does not fit as no timeout.
      const auto Milliseconds = (static_cast<uint64_t>(Parts[1]) + 999) / 1000;
      constexpr int64_t Limit = int64_t {1} << 40;
      Nanoseconds = Parts[0] >= Limit ? 0 : (static_cast<uint64_t>(Parts[0]) * 1000 + Milliseconds) * 1'000'000;
    }
    (Name == SockOpt::RcvTimeoOld || Name == SockOpt::RcvTimeoNew ? Options.RcvTimeoutNs : Options.SndTimeoutNs) = Nanoseconds;
    return 0;
  }
  default:
    if (SocketOptions::FlagBit(Name) >= 0) {
      Options.Flags = (Options.Flags & ~(1u << SocketOptions::FlagBit(Name))) | ((Int != 0) << SocketOptions::FlagBit(Name));
      return 0;
    }
    return -92;
  }
}

// sock_getsockopt for the same options. Value gets the natural-size bytes.
inline int GetSocketOption(const SocketOptions& Options, int Name, OptionValue& Value) {
  auto Put = [&](int32_t Int) {
    std::memcpy(Value.Data.data(), &Int, sizeof(Int));
    Value.Size = sizeof(Int);
    return 0;
  };
  switch (Name) {
  case SockOpt::SndBuf: return Put(static_cast<int32_t>(std::min<size_t>(Options.SendBuffer, INT_MAX)));
  case SockOpt::RcvBuf: return Put(static_cast<int32_t>(std::min<size_t>(Options.ReceiveBuffer, INT_MAX)));
  case SockOpt::PassCred: return Put(Options.PassCred);
  case SockOpt::PassSec: return Put(Options.PassSec);
  case SockOpt::RcvLowat: return Put(Options.RcvLowat);
  case SockOpt::PeekOff: return Put(Options.PeekOff);
  case SockOpt::Priority: return Put(Options.Priority);
  case SockOpt::Linger:
    std::memcpy(Value.Data.data(), Options.Linger, sizeof(Options.Linger));
    Value.Size = sizeof(Options.Linger);
    return 0;
  case SockOpt::RcvTimeoOld: case SockOpt::SndTimeoOld: case SockOpt::RcvTimeoNew: case SockOpt::SndTimeoNew: {
    const auto Nanoseconds = Name == SockOpt::RcvTimeoOld || Name == SockOpt::RcvTimeoNew ? Options.RcvTimeoutNs : Options.SndTimeoutNs;
    const int64_t Parts[2] {static_cast<int64_t>(Nanoseconds / 1'000'000'000), static_cast<int64_t>(Nanoseconds % 1'000'000'000 / 1000)};
    std::memcpy(Value.Data.data(), Parts, sizeof(Parts));
    Value.Size = sizeof(Parts);
    return 0;
  }
  default:
    if (SocketOptions::FlagBit(Name) >= 0) return Put(Options.Flag(Name));
    return -92;
  }
}

// unix_autobind: a NUL and five lowercase hex digits of the sequence number.
inline std::string AutobindName(uint32_t Sequence) {
  static constexpr char Digits[] = "0123456789abcdef";
  std::string Name(6, '\0');
  for (int Index = 5; Index >= 1; --Index) {
    Name[Index] = Digits[Sequence & 15];
    Sequence >>= 4;
  }
  return Name;
}

template<typename File>
class StreamSocket : public SocketOptions, public std::enable_shared_from_this<StreamSocket<File>> {
public:
  static constexpr size_t Capacity = 64 * 1024;
  // net.core.max_dgram_qlen: a receiver refuses new datagrams once its queue holds more.
  static constexpr size_t MaxDatagramQueue = 512;

  SocketKind Kind {SocketKind::Stream};
  // One entry per queued datagram or seqpacket record. The bytes live in Input.
  struct Record {
    size_t Bytes {};
    std::string Source;     // sender's bound name (empty: unnamed)
    SocketCred Cred;
    bool Eor {};
    size_t Truesize {};
    std::weak_ptr<StreamSocket> Sender; // whose Wmem pays for it
    std::vector<std::shared_ptr<File>> Files;
  };
  std::deque<Record> Records;
  std::string Name;          // bound address bytes after sun_family; abstract names start with '\0'
  std::optional<SocketCred> PeerCred;   // SO_PEERCRED, set at connect(), listen() and socketpair()
  std::vector<uint32_t> PeerGroups;     // SO_PEERGROUPS
  bool ResetPending {};      // the peer closed with unread data: ECONNRESET once
  int32_t PendingError {};   // sk_err: a Linux errno the next receive reports once (recvmmsg keeps one here)
  std::string PeerName;      // the peer's bound name when this socket connected (getpeername after the peer closed)
  bool PeerNamed {};
  bool Embryo {};            // queued by connect() and not yet accepted
  size_t Wmem {};            // truesize of this sender's records still queued at receivers
  struct CredMark {
    size_t Offset;
    SocketCred Cred;
  };
  std::deque<CredMark> CredMarks; // stream: credentials of the sender, per run of bytes

  StreamSocket() = default;
  StreamSocket(const StreamSocket&) = delete;
  StreamSocket& operator=(const StreamSocket&) = delete;

  ~StreamSocket() {
    for (auto& Queued : Records) Release(Queued);
    if (Pipe || Kind == SocketKind::Datagram) return;
    // unix_release_sock: a stream or seqpacket peer is reset when this socket
    // dies with unread data, or while still queued by a connect().
    if (const auto Other = Peer.lock(); Other && (Embryo || !Input.empty() || !Records.empty())) {
      Other->ResetPending = true;
      ++Other->Changes;
    }
  }

  std::weak_ptr<StreamSocket> Peer;
  std::deque<uint8_t> Input;
  // Counts changes to Input, for edge-triggered epoll.
  uint64_t Changes {};
  size_t InputCapacity {Capacity};

  static constexpr size_t PipePage = 4096;
  struct PinnedPage {
    const uint8_t* Live;
    std::shared_ptr<const uint8_t> Owner;
    size_t Size;
    bool Suspended {};
    std::unique_ptr<uint8_t[]> Saved;
    const uint8_t* Data() const { return Suspended ? Saved.get() : Live; }
    void Suspend() {
      if (Suspended) return;
      std::copy_n(Live, Size, Saved.get());
      Suspended = true;
    }
  };
  struct PipeBuffer {
    size_t Start, End;
    bool Mergeable;
    std::shared_ptr<PinnedPage> Pin {};
    // Written through an O_DIRECT descriptor: a read takes at most this
    // buffer and discards what it leaves (Linux PIPE_BUF_FLAG_PACKET).
    bool Packet {};
  };
  bool Pipe {};
  std::deque<PipeBuffer> PipeBuffers;
  size_t UsedCapacity() const { return Pipe ? PipeBuffers.size() * PipePage : Input.size(); }
  struct RightsMessage {
    size_t Offset;
    size_t End; // where the bytes of the sending call end (its skb)
    std::vector<std::shared_ptr<File>> Files;
  };
  std::deque<RightsMessage> Rights;
  bool Nonblocking {};
  bool ShutRead {};
  bool ShutWrite {};
  bool Connected {};
  bool Bound {};
  bool Listening {};
  size_t Backlog {};
  std::deque<std::shared_ptr<StreamSocket>> Pending;

  // Gives a queued record's memory back to the socket that sent it.
  static void Release(Record& Queued) {
    if (const auto Owner = Queued.Sender.lock()) Owner->Wmem -= std::min(Owner->Wmem, Queued.Truesize);
    Queued.Truesize = 0;
  }

  void Connect(const std::shared_ptr<StreamSocket>& Other) {
    Peer = Other;
    Connected = true;
  }

  void Consume(size_t Bytes) {
    if (Bytes) ++Changes;
    Input.erase(Input.begin(), Input.begin() + Bytes);
    for (size_t Remaining = Bytes; Pipe && Remaining;) {
      auto& Front = PipeBuffers.front();
      const auto Step = std::min(Remaining, Front.End - Front.Start);
      Front.Start += Step;
      Remaining -= Step;
      if (Front.Start == Front.End) PipeBuffers.pop_front();
    }
    while (!Rights.empty() && Rights.front().Offset < Bytes) Rights.pop_front();
    for (auto& Message : Rights) {
      Message.Offset -= Bytes;
      Message.End -= Bytes;
    }
    while (CredMarks.size() > 1 && CredMarks[1].Offset <= Bytes) CredMarks.pop_front();
    for (auto& Mark : CredMarks) Mark.Offset = Mark.Offset > Bytes ? Mark.Offset - Bytes : 0;
    if (Input.empty()) CredMarks.clear();
  }

  void CopyInput(char* Output, size_t Count) const {
    std::copy_n(Input.begin(), Count, Output);
    size_t Offset {};
    for (const auto& Buffer : PipeBuffers) {
      if (Offset == Count) break;
      const auto Bytes = std::min(Count - Offset, Buffer.End - Buffer.Start);
      if (Buffer.Pin) std::copy_n(Buffer.Pin->Data() + Buffer.Start, Bytes, Output + Offset);
      Offset += Bytes;
    }
  }

  size_t SendPinned(std::shared_ptr<PinnedPage> Pin, size_t Count, size_t Start = 0) {
    const auto Other = Peer.lock();
    if (!Other || !Other->Pipe || Count > PipePage || !WriteSpace(false)) return 0;
    Other->PipeBuffers.push_back({Start, Start + Count, false, std::move(Pin)});
    Other->Input.insert(Other->Input.end(), Count, 0);
    ++Other->Changes;
    return Count;
  }

  size_t MergeableBytes(size_t Count) const {
    const auto Tail = Count % PipePage;
    return Pipe && !PipeBuffers.empty() && PipeBuffers.back().Mergeable &&
      Tail <= PipePage - PipeBuffers.back().End ? Tail : 0;
  }

  size_t WriteSpace(bool Merge = true, size_t Count = 0) const {
    const auto Other = Peer.lock();
    if (!Other || ShutWrite || Other->ShutRead) return 0;
    return Other->InputCapacity - Other->UsedCapacity() + (Merge ? Other->MergeableBytes(Count) : 0);
  }

  // Control messages travel at a byte boundary. A caller that sends with
  // vectored I/O attaches rights to its first successfully queued bytes.
  size_t Send(std::span<const uint8_t> Bytes, std::vector<std::shared_ptr<File>> Files = {}, bool Merge = true,
              bool Packet = false, std::optional<SocketCred> Cred = std::nullopt) {
    if (Packet) Merge = false;
    const auto Other = Peer.lock();
    if (!Other || ShutWrite || Other->ShutRead) return 0;
    const auto Count = std::min(Bytes.size(), WriteSpace(Merge, Bytes.size()));
    if (!Count) return 0;
    if (Other->Pipe) {
      size_t Remaining = Count;
      auto& Buffers = Other->PipeBuffers;
      if (const auto Step = Merge ? Other->MergeableBytes(Bytes.size()) : 0; Step) {
        Buffers.back().End += Step;
        Remaining -= Step;
      }
      while (Remaining) {
        const auto Step = std::min(Remaining, PipePage);
        Buffers.push_back({0, Step, Merge, nullptr, Packet});
        Remaining -= Step;
      }
    }
    if (!Files.empty()) Other->Rights.push_back({Other->Input.size(), Other->Input.size() + Count, std::move(Files)});
    if (Cred && (Other->CredMarks.empty() || !(Other->CredMarks.back().Cred == *Cred))) {
      Other->CredMarks.push_back({Other->Input.size(), *Cred});
    }
    Other->Input.insert(Other->Input.end(), Bytes.begin(), Bytes.begin() + Count);
    ++Other->Changes;
    return Count;
  }

  // Linux pipe_read: a read runs over stream buffers and stops after it
  // takes from a packet buffer.
  size_t ReadLimit(size_t Count) const {
    if (!Pipe) return std::min(Count, Input.size());
    size_t Total {};
    for (const auto& Buffer : PipeBuffers) {
      if (Total == Count) break;
      Total += std::min(Count - Total, Buffer.End - Buffer.Start);
      if (Buffer.Packet) break;
    }
    return Total;
  }

  // Consumes a read of Bytes and, when it ended inside a packet, throws away
  // the rest of that packet.
  void ConsumeRead(size_t Bytes) {
    size_t Discard {}, Left = Bytes;
    for (const auto& Buffer : PipeBuffers) {
      if (!Pipe || !Left) break;
      const auto Size = Buffer.End - Buffer.Start;
      if (Left < Size) {
        if (Buffer.Packet) Discard = Size - Left;
        break;
      }
      Left -= Size;
    }
    Consume(Bytes + Discard);
  }

  // unix_stream_read_generic: a read may take plain bytes before a message
  // with rights, and ends with the bytes of that message (the next read
  // starts after it).
  size_t MessageBytes(size_t Limit) const {
    auto Count = std::min(Limit, Input.size());
    if (!Rights.empty() && Rights.front().Offset < Count) Count = std::min(Count, Rights.front().End);
    // A receive never mixes bytes sent with different credentials.
    if (CredMarks.size() > 1) Count = std::min(Count, CredMarks[1].Offset);
    return Count;
  }

  SocketCred FrontCred() const { return CredMarks.empty() ? SocketCred {} : CredMarks.front().Cred; }

  // ---- Datagram and seqpacket records ----

  // skb->truesize of a unix datagram: kmalloc bucket of the data plus
  // skb_shared_info, plus the sk_buff itself. Measured on Linux 7.3: one
  // sender queues 278 one-byte and 93 thousand-byte datagrams.
  static size_t Truesize(size_t Bytes) {
    static constexpr size_t Buckets[] {8, 16, 32, 64, 96, 128, 192, 256, 512, 1024, 2048, 4096, 8192};
    const auto Need = Bytes + 320;
    size_t Size = (Need + 4095) & ~size_t {4095};
    for (const auto Bucket : Buckets) {
      if (Need <= Bucket) {
        Size = Bucket;
        break;
      }
    }
    return Size + 256;
  }

  // unix_dgram_sendmsg: a datagram above sk_sndbuf - 32 is EMSGSIZE.
  size_t MaxRecord() const { return SendBuffer > 32 ? SendBuffer - 32 : 0; }

  // Sender side of unix_dgram_sendmsg: memory left for one more record.
  bool SenderHasRoom() const { return Wmem < SendBuffer; }
  // unix_writable for poll: (sk_wmem_alloc << 2) <= sk_sndbuf, where
  // sk_wmem_alloc starts at 1. POLLOUT goes long before sends block.
  bool PollWritable() const { return (Wmem + 1) * 4 <= SendBuffer; }

  enum class SendStatus { Ok, Refused, Shutdown, Full };

  // Queues one record at Target, or at the connected peer. Refused: nobody
  // is there (ECONNREFUSED). Shutdown: EPIPE. Full: the caller blocks or
  // gets EAGAIN. Linux checks sender memory and then the receiver queue.
  SendStatus TrySend(std::span<const uint8_t> Bytes, std::string Source, SocketCred Cred,
                     std::vector<std::shared_ptr<File>> Files, bool Eor, std::shared_ptr<StreamSocket> Target = {}) {
    const auto Other = Target ? Target : Peer.lock();
    if (!Other) return SendStatus::Refused;
    if (ShutWrite || Other->ShutRead) return SendStatus::Shutdown;
    if (!SenderHasRoom() || Other->Records.size() > MaxDatagramQueue) return SendStatus::Full;
    Record Queued;
    Queued.Bytes = Bytes.size();
    Queued.Source = std::move(Source);
    Queued.Cred = Cred;
    Queued.Eor = Eor;
    Queued.Truesize = Truesize(Bytes.size());
    Queued.Sender = this->weak_from_this();
    Queued.Files = std::move(Files);
    Wmem += Queued.Truesize;
    Other->Input.insert(Other->Input.end(), Bytes.begin(), Bytes.end());
    Other->Records.push_back(std::move(Queued));
    ++Other->Changes;
    return SendStatus::Ok;
  }

  size_t SendRecord(std::span<const uint8_t> Bytes, std::string Source, SocketCred Cred,
                    std::vector<std::shared_ptr<File>> Files, bool Eor, std::shared_ptr<StreamSocket> Target = {}) {
    return TrySend(Bytes, std::move(Source), Cred, std::move(Files), Eor, std::move(Target)) == SendStatus::Ok ? Bytes.size() : 0;
  }

  // Takes (or with Peek leaves) the first record: its bytes up to Out.size(),
  // the rest discarded. FullLength is what the record held after Skip bytes
  // (SO_PEEK_OFF), for MSG_TRUNC. Files are moved out, or copied on a peek.
  // The queue must not be empty.
  size_t ReceiveRecord(std::span<uint8_t> Out, bool Peek, size_t& FullLength, std::string* Source = nullptr,
                       SocketCred* Cred = nullptr, std::vector<std::shared_ptr<File>>* Files = nullptr, size_t Skip = 0) {
    size_t Index {}, Start {};
    if (Peek) {
      // Skip whole records, then bytes inside the record that is left.
      while (Index + 1 < Records.size() && Records[Index].Bytes && Skip >= Records[Index].Bytes) {
        Skip -= Records[Index].Bytes;
        Start += Records[Index].Bytes;
        ++Index;
      }
      Skip = std::min(Skip, Records[Index].Bytes);
    } else {
      Skip = 0;
    }
    auto& Front = Records[Index];
    FullLength = Front.Bytes - Skip;
    const auto Count = std::min(Out.size(), FullLength);
    std::copy_n(Input.begin() + (Start + Skip), Count, Out.begin());
    if (Source) *Source = Front.Source;
    if (Cred) *Cred = Front.Cred;
    if (Files) {
      if (Peek) Files->insert(Files->end(), Front.Files.begin(), Front.Files.end());
      else *Files = std::move(Front.Files);
    }
    if (!Peek) {
      Input.erase(Input.begin(), Input.begin() + Front.Bytes);
      Release(Front);
      Records.pop_front();
      ++Changes;
    }
    return Count;
  }

  // The peer queue that a connected sender writes to is full.
  bool PeerQueueFull() const {
    const auto Other = Peer.lock();
    return Other && Other->Records.size() > MaxDatagramQueue;
  }

  const RightsMessage* MessageRights(size_t Count) const {
    return !Rights.empty() && Rights.front().Offset < Count ? &Rights.front() : nullptr;
  }

  bool EndOfStream() const {
    const auto Other = Peer.lock();
    return ShutRead || !Other || Other->ShutWrite;
  }

  int16_t Poll(int16_t Events) const {
    const auto Other = Peer.lock();
    int16_t Ready {};
    if (Kind == SocketKind::Datagram) {
      // unix_dgram_poll: a socket without a peer is still writable.
      if (ResetPending) Ready |= 8;
      if (ShutRead && ShutWrite) Ready |= 0x10;
      if ((Events & 1) && (!Records.empty() || ShutRead)) Ready |= 1;
      if ((Events & 0x2000) && ShutRead) Ready |= 0x2000;
      if ((Events & 4) && !ShutWrite && PollWritable() && !(Connected && PeerQueueFull())) Ready |= 4;
      return Ready;
    }
    if (Listening) return Pending.empty() ? 0 : Events & 1;
    if (!Connected) {
      // unix_poll on a closed stream or seqpacket socket: POLLHUP, and
      // POLLOUT from unix_writable (verified natively: 0x14).
      Ready = 0x10 | (Events & 4);
      if (ShutRead) Ready |= Events & (1 | 0x2000);
      return Ready;
    }
    if (ResetPending) Ready |= 8;
    if (Kind == SocketKind::SeqPacket) {
      if ((Events & 1) && (!Records.empty() || EndOfStream())) Ready |= 1;
      if ((Events & 4) && Other && !ShutWrite && !Other->ShutRead && PollWritable() && !PeerQueueFull()) Ready |= 4;
      if (!Other || ((ShutWrite || Other->ShutRead) && EndOfStream())) Ready |= 0x10;
      if ((Events & 0x2000) && EndOfStream()) Ready |= 0x2000;
      return Ready;
    }
    if ((Events & 1) && (!Input.empty() || EndOfStream())) Ready |= 1;
    if ((Events & 4) && Other && !ShutWrite && !Other->ShutRead && Other->UsedCapacity() < Other->InputCapacity) Ready |= 4;
    if (!Other || ((ShutWrite || Other->ShutRead) && EndOfStream())) Ready |= 0x10;
    if ((Events & 0x2000) && EndOfStream()) Ready |= 0x2000;
    return Ready;
  }
};
} // namespace Switch2Guest
