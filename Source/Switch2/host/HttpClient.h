// SPDX-License-Identifier: MIT
// A small streaming HTTP/1.1 GET client over plain TCP (the console has no TLS), for the RootFS image download and the
// Ubuntu package indexes. It follows http:// redirects, supports a byte range for resuming, and hands the body to a
// callback in chunks so nothing is held in memory. Portable POSIX sockets: on the console the socket service must be
// running (HostNet::Acquire); on the host it is plain sockets. Over plain HTTP the hash checks of the callers protect
// integrity, not authenticity.
#pragma once
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "SocketTuning.h"

namespace Switch2Guest::Http {

struct Url {
  std::string Host, Path;
  uint16_t Port {80};
};

// "http://host[:port]/path". Rejects https and anything else.
inline std::optional<Url> ParseUrl(std::string_view Text) {
  constexpr std::string_view Scheme = "http://";
  if (!Text.starts_with(Scheme)) return std::nullopt;
  Text.remove_prefix(Scheme.size());
  const auto Slash = Text.find('/');
  const auto Authority = Text.substr(0, Slash);
  Url Result;
  Result.Path = Slash == Text.npos ? "/" : std::string(Text.substr(Slash));
  const auto Colon = Authority.find(':');
  Result.Host = std::string(Authority.substr(0, Colon));
  if (Colon != Authority.npos) {
    const auto Digits = Authority.substr(Colon + 1);
    if (Digits.empty() || Digits.size() > 5) return std::nullopt;
    unsigned Port = 0;
    for (char C : Digits) {
      if (C < '0' || C > '9') return std::nullopt;
      Port = Port * 10 + (C - '0');
    }
    if (!Port || Port > 65535) return std::nullopt;
    Result.Port = static_cast<uint16_t>(Port);
  }
  if (Result.Host.empty()) return std::nullopt;
  return Result;
}

struct ResponseInfo {
  int Status {};
  std::optional<uint64_t> ContentLength;  // of this response body
  std::optional<uint64_t> RangeStart;     // Content-Range start (206)
  std::optional<uint64_t> TotalSize;      // Content-Range total
  bool Chunked {};
  std::string Location;
};

inline std::string Lower(std::string_view Text) {
  std::string Result(Text);
  for (auto& C : Result)
    if (C >= 'A' && C <= 'Z') C = static_cast<char>(C + 32);
  return Result;
}

inline std::optional<uint64_t> ParseNumber(std::string_view Text) {
  while (Text.starts_with(' ')) Text.remove_prefix(1);
  while (Text.ends_with(' ') || Text.ends_with('\r')) Text.remove_suffix(1);
  if (Text.empty() || Text.size() > 19) return std::nullopt;
  uint64_t Value = 0;
  for (char C : Text) {
    if (C < '0' || C > '9') return std::nullopt;
    Value = Value * 10 + (C - '0');
  }
  return Value;
}

// Parses the status line and headers (Head excludes the blank line). Status 0 when malformed.
inline ResponseInfo ParseHead(std::string_view Head) {
  ResponseInfo Info;
  const auto StatusEnd = Head.find("\r\n");
  const auto StatusLine = Head.substr(0, StatusEnd);
  if (StatusLine.size() < 12 || !StatusLine.starts_with("HTTP/1.")) return Info;
  const auto Code = ParseNumber(StatusLine.substr(9, 3));
  if (!Code) return Info;
  Info.Status = static_cast<int>(*Code);
  for (size_t Start = StatusEnd == Head.npos ? Head.size() : StatusEnd + 2; Start < Head.size();) {
    auto End = Head.find("\r\n", Start);
    if (End == Head.npos) End = Head.size();
    const auto Line = Head.substr(Start, End - Start);
    Start = End + 2;
    const auto Colon = Line.find(':');
    if (Colon == Line.npos) continue;
    const auto Name = Lower(Line.substr(0, Colon));
    auto Value = Line.substr(Colon + 1);
    while (Value.starts_with(' ')) Value.remove_prefix(1);
    if (Name == "content-length") Info.ContentLength = ParseNumber(Value);
    else if (Name == "location") Info.Location = std::string(Value);
    else if (Name == "transfer-encoding") Info.Chunked = Lower(Value).find("chunked") != std::string::npos;
    else if (Name == "content-range") {
      // "bytes START-END/TOTAL"
      if (Value.starts_with("bytes ")) {
        Value.remove_prefix(6);
        const auto Dash = Value.find('-'), Slash = Value.find('/');
        if (Dash != Value.npos && Slash != Value.npos && Dash < Slash) {
          Info.RangeStart = ParseNumber(Value.substr(0, Dash));
          Info.TotalSize = ParseNumber(Value.substr(Slash + 1));
        }
      }
    }
  }
  return Info;
}

// Incremental removal of chunked framing.
class Dechunker {
public:
  // Feeds raw bytes; calls Out for payload bytes. Returns false when the framing is malformed.
  template <typename Fn> bool Feed(const uint8_t* Data, size_t Size, Fn&& Out) {
    while (Size) {
      switch (State) {
      case State::SizeLine: {
        const char C = static_cast<char>(*Data++);
        --Size;
        if (C == '\n') {
          if (!ParseSize()) return false;
          Line.clear();
          State = Remaining ? State::Payload : State::Trailer;
        } else {
          Line.push_back(C);
          if (Line.size() > 64) return false;
        }
        break;
      }
      case State::Payload: {
        const size_t Take = static_cast<size_t>(std::min<uint64_t>(Remaining, Size));
        if (!Out(Data, Take)) return false;
        Data += Take;
        Size -= Take;
        Remaining -= Take;
        if (!Remaining) State = State::PayloadEnd;
        break;
      }
      case State::PayloadEnd: {
        // CRLF after the chunk.
        const char C = static_cast<char>(*Data++);
        --Size;
        if (C == '\n') State = State::SizeLine;
        else if (C != '\r') return false;
        break;
      }
      case State::Trailer: {
        // Trailers and the final blank line; the connection closes afterwards.
        Data += Size;
        Size = 0;
        Done = true;
        break;
      }
      }
    }
    return true;
  }
  bool Finished() const { return Done || State == State::Trailer; }

private:
  enum class State { SizeLine, Payload, PayloadEnd, Trailer } State {State::SizeLine};
  bool ParseSize() {
    uint64_t Value = 0;
    bool Any = false;
    for (char C : Line) {
      int Digit;
      if (C >= '0' && C <= '9') Digit = C - '0';
      else if (C >= 'a' && C <= 'f') Digit = C - 'a' + 10;
      else if (C >= 'A' && C <= 'F') Digit = C - 'A' + 10;
      else break; // extensions after ';' or the CR
      Value = Value * 16 + Digit;
      if (Value > (uint64_t {1} << 40)) return false;
      Any = true;
    }
    Remaining = Value;
    return Any;
  }
  std::string Line;
  uint64_t Remaining {};
  bool Done {};
};

// Called with each body chunk; return false to cancel.
using BodyFn = std::function<bool(const uint8_t*, size_t)>;
// Called once with the final response head, before any body bytes; return false to cancel.
using HeadFn = std::function<bool(const ResponseInfo&)>;

// GET Location from byte Offset (0: no Range header). On success Info describes the final response. The body callback
// only sees bytes of a 200 or 206 response. A 200 answer to a ranged request means the server ignored the range: the body
// then starts at byte 0 and Info.RangeStart is empty. Returns an error text, or nullptr. "Cancelled" when the callback
// returned false.
inline const char* Get(const std::string& Location, uint64_t Offset, const BodyFn& Body, ResponseInfo& Info, int TimeoutSeconds = 30,
                       const HeadFn& OnHead = {}) {
  std::string Current = Location;
  for (int Redirects = 0; Redirects <= 5; ++Redirects) {
    const auto Parsed = ParseUrl(Current);
    if (!Parsed) return "Unsupported URL (only http:// works on the console)";
    addrinfo Hints {};
    Hints.ai_family = AF_INET;
    Hints.ai_socktype = SOCK_STREAM;
    addrinfo* Addresses {};
    const auto Port = std::to_string(Parsed->Port);
    if (getaddrinfo(Parsed->Host.c_str(), Port.c_str(), &Hints, &Addresses) || !Addresses) return "Could not resolve the server name";
    const int Socket = socket(AF_INET, SOCK_STREAM, 0);
    if (Socket < 0) {
      freeaddrinfo(Addresses);
      return "Could not open a socket";
    }
    timeval Timeout {TimeoutSeconds, 0};
    setsockopt(Socket, SOL_SOCKET, SO_RCVTIMEO, &Timeout, sizeof(Timeout));
    setsockopt(Socket, SOL_SOCKET, SO_SNDTIMEO, &Timeout, sizeof(Timeout));
    TuneTransferSocket(Socket); // Before connect: the receive window is announced in the handshake.
    const int Connected = connect(Socket, Addresses->ai_addr, Addresses->ai_addrlen);
    freeaddrinfo(Addresses);
    if (Connected) {
      close(Socket);
      return "Could not connect to the server";
    }
    std::string Request = "GET " + Parsed->Path + " HTTP/1.1\r\nHost: " + Parsed->Host + (Parsed->Port != 80 ? ":" + Port : "") +
                          "\r\nUser-Agent: FEX-Switch2-installer\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
    if (Offset) Request += "Range: bytes=" + std::to_string(Offset) + "-\r\n";
    Request += "\r\n";
    for (size_t Sent = 0; Sent < Request.size();) {
      const auto Count = send(Socket, Request.data() + Sent, Request.size() - Sent, 0);
      if (Count <= 0) {
        close(Socket);
        return "Could not send the request";
      }
      Sent += static_cast<size_t>(Count);
    }
    std::vector<uint8_t> Buffer(64 * 1024);
    std::string Head;
    bool HaveHead = false;
    Info = {};
    Dechunker Unchunk;
    uint64_t BodyBytes = 0;
    bool Cancelled = false, Failed = false;
    const auto Deliver = [&](const uint8_t* Data, size_t Size) {
      BodyBytes += Size;
      if (!Size) return true;
      if (!Body(Data, Size)) {
        Cancelled = true;
        return false;
      }
      return true;
    };
    for (;;) {
      const auto Received = recv(Socket, Buffer.data(), Buffer.size(), 0);
      if (Received < 0) {
        if (errno == EINTR) continue;
        close(Socket);
        return "The connection stalled or failed";
      }
      if (!Received) break;
      const uint8_t* Data = Buffer.data();
      size_t Size = static_cast<size_t>(Received);
      if (!HaveHead) {
        Head.append(reinterpret_cast<const char*>(Data), Size);
        const auto End = Head.find("\r\n\r\n");
        if (End == std::string::npos) {
          if (Head.size() > 16384) {
            close(Socket);
            return "Malformed HTTP response";
          }
          continue;
        }
        Info = ParseHead(std::string_view(Head).substr(0, End + 2));
        if (!Info.Status) {
          close(Socket);
          return "Malformed HTTP response";
        }
        HaveHead = true;
        const size_t BodyStart = End + 4;
        // Re-point Data at the leftover body bytes inside Head.
        std::vector<uint8_t> Leftover(Head.begin() + BodyStart, Head.end());
        if (Info.Status == 301 || Info.Status == 302 || Info.Status == 303 || Info.Status == 307 || Info.Status == 308) break;
        if (Info.Status != 200 && Info.Status != 206) break;
        if (Info.Status == 206 && (!Info.RangeStart || *Info.RangeStart != Offset)) {
          close(Socket);
          return "The server answered a different range";
        }
        if (OnHead && !OnHead(Info)) {
          close(Socket);
          return "Cancelled";
        }
        if (!Leftover.empty()) {
          if (Info.Chunked) Failed = !Unchunk.Feed(Leftover.data(), Leftover.size(), Deliver) && !Cancelled;
          else Deliver(Leftover.data(), Leftover.size());
        }
        if (Cancelled || Failed) break;
        continue;
      }
      if (Info.Chunked) {
        if (!Unchunk.Feed(Data, Size, Deliver)) {
          Failed = !Cancelled;
          break;
        }
      } else if (!Deliver(Data, Size)) {
        break;
      }
    }
    close(Socket);
    if (Cancelled) return "Cancelled";
    if (Failed) return "Malformed chunked response";
    if (!HaveHead) return "The server closed the connection without an answer";
    if (Info.Status == 301 || Info.Status == 302 || Info.Status == 303 || Info.Status == 307 || Info.Status == 308) {
      if (Info.Location.starts_with("/")) Current = "http://" + Parsed->Host + Info.Location;
      else Current = Info.Location;
      continue;
    }
    if (Info.Status != 200 && Info.Status != 206) return "The server refused the request";
    if (!Info.Chunked && Info.ContentLength && BodyBytes != *Info.ContentLength) return "The download was cut short";
    if (Info.Chunked && !Unchunk.Finished()) return "The download was cut short";
    return nullptr;
  }
  return "Too many redirects";
}

} // namespace Switch2Guest::Http
