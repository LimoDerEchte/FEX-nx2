// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <vector>

namespace Switch2Guest {
template<typename File>
class StreamSocket {
public:
  static constexpr size_t Capacity = 64 * 1024;

  std::weak_ptr<StreamSocket> Peer;
  std::deque<uint8_t> Input;
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
  };
  bool Pipe {};
  std::deque<PipeBuffer> PipeBuffers;
  size_t UsedCapacity() const { return Pipe ? PipeBuffers.size() * PipePage : Input.size(); }
  struct RightsMessage {
    size_t Offset;
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

  void Connect(const std::shared_ptr<StreamSocket>& Other) {
    Peer = Other;
    Connected = true;
  }

  void Consume(size_t Bytes) {
    Input.erase(Input.begin(), Input.begin() + Bytes);
    for (size_t Remaining = Bytes; Pipe && Remaining;) {
      auto& Front = PipeBuffers.front();
      const auto Step = std::min(Remaining, Front.End - Front.Start);
      Front.Start += Step;
      Remaining -= Step;
      if (Front.Start == Front.End) PipeBuffers.pop_front();
    }
    while (!Rights.empty() && Rights.front().Offset < Bytes) Rights.pop_front();
    for (auto& Message : Rights) Message.Offset -= Bytes;
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
  size_t Send(std::span<const uint8_t> Bytes, std::vector<std::shared_ptr<File>> Files = {}, bool Merge = true) {
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
        Buffers.push_back({0, Step, Merge});
        Remaining -= Step;
      }
    }
    if (!Files.empty()) Other->Rights.push_back({Other->Input.size(), std::move(Files)});
    Other->Input.insert(Other->Input.end(), Bytes.begin(), Bytes.begin() + Count);
    return Count;
  }

  size_t MessageBytes(size_t Limit) const {
    auto Count = std::min(Limit, Input.size());
    if (Rights.size() > 1) Count = std::min(Count, Rights[1].Offset);
    return Count;
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
    if (Listening) return Pending.empty() ? 0 : Events & 1;
    if (!Connected) return 0x10;
    if ((Events & 1) && (!Input.empty() || EndOfStream())) Ready |= 1;
    if ((Events & 4) && Other && !ShutWrite && !Other->ShutRead && Other->UsedCapacity() < Other->InputCapacity) Ready |= 4;
    if (!Other || ((ShutWrite || Other->ShutRead) && EndOfStream())) Ready |= 0x10;
    if ((Events & 0x2000) && EndOfStream()) Ready |= 0x2000;
    return Ready;
  }
};
} // namespace Switch2Guest
