// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>

namespace Switch2Guest {
class StreamSocket {
public:
  static constexpr size_t Capacity = 64 * 1024;

  std::weak_ptr<StreamSocket> Peer;
  std::deque<uint8_t> Input;
  bool Nonblocking {};
  bool ShutRead {};
  bool ShutWrite {};
  bool Connected {};
  bool Bound {};
  bool Listening {};
  size_t Backlog {};
  std::deque<std::shared_ptr<StreamSocket>> Pending;

  void Connect(const std::shared_ptr<StreamSocket>& Other) {
    Peer = Other;
    Connected = true;
  }

  bool EndOfStream() const {
    const auto Other = Peer.lock();
    return ShutRead || !Other || Other->ShutWrite;
  }

  int16_t Poll(int16_t Events) const {
    const auto Other = Peer.lock();
    int16_t Ready {};
    if (Listening) return Pending.empty() ? 0 : Events & 1;
    if (!Connected) return 0x10;
    if ((Events & 1) && (!Input.empty() || EndOfStream())) Ready |= 1;
    if ((Events & 4) && Other && !ShutWrite && !Other->ShutRead && Other->Input.size() < Capacity) Ready |= 4;
    if (!Other || ((ShutWrite || Other->ShutRead) && EndOfStream())) Ready |= 0x10;
    if ((Events & 0x2000) && EndOfStream()) Ready |= 0x2000;
    return Ready;
  }
};
} // namespace Switch2Guest
