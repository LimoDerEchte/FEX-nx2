// SPDX-License-Identifier: MIT
#pragma once

// POSIX message queue storage. Pure bookkeeping: the runtime owns names,
// descriptors, waiting and signals.

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <span>
#include <vector>

namespace Switch2Guest {
// Linux 7.3 defaults and hard limits (fs.mqueue.*, mq_attr_ok).
constexpr int64_t MqDefaultMaxMsg = 10, MqDefaultMsgSize = 8192;
constexpr int64_t MqHardMaxMsg = 65536, MqHardMsgSize = 16 * 1024 * 1024;
constexpr uint32_t MqPrioMax = 32768;
constexpr size_t MqQueuesMax = 256;
constexpr uint64_t MqNameMax = 255;
constexpr uint64_t MqMessageOverhead = 48; // sizeof(struct msg_msg), and a tree node per priority

struct MqAttr {
  int64_t Flags, MaxMsg, MsgSize, CurMsgs;
};
static_assert(sizeof(MqAttr) == 32); // the struct mq_attr tail is reserved: the guest copy is 64 bytes

// mq_attr_ok: positive sizes; without CAP_SYS_RESOURCE they must not exceed
// the sysctl defaults. Returns 0 or -EINVAL.
inline int64_t CheckMqAttr(const MqAttr& Attr, bool CapSysResource) {
  if (Attr.MaxMsg <= 0 || Attr.MsgSize <= 0) return -22;
  if (CapSysResource) {
    if (Attr.MaxMsg > MqHardMaxMsg || Attr.MsgSize > MqHardMsgSize) return -22;
  } else if (Attr.MaxMsg > MqDefaultMaxMsg || Attr.MsgSize > MqDefaultMsgSize) {
    return -22;
  }
  return 0;
}

// Bytes a queue is charged against RLIMIT_MSGQUEUE (mqueue_get_inode).
inline uint64_t MqChargedBytes(int64_t MaxMsg, int64_t MsgSize) {
  const auto Nodes = static_cast<uint64_t>(std::min<int64_t>(MaxMsg, MqPrioMax));
  return static_cast<uint64_t>(MaxMsg) * (MqMessageOverhead + static_cast<uint64_t>(MsgSize)) + Nodes * MqMessageOverhead;
}

class MessageQueue {
public:
  MessageQueue(int64_t MaxMsg, int64_t MsgSize) : MaxMsg {MaxMsg}, MsgSize {MsgSize} {}

  // 0, -EINVAL (priority out of range), -EMSGSIZE or -EAGAIN when full.
  int64_t Send(std::span<const uint8_t> Text, uint32_t Priority) {
    if (Priority >= MqPrioMax) return -22;
    if (static_cast<int64_t>(Text.size()) > MsgSize) return -90;
    if (Full()) return -11;
    Messages[Priority].emplace_back(Text.begin(), Text.end());
    ++Count;
    Bytes += Text.size();
    return 0;
  }

  // The length received, -EMSGSIZE when the buffer is smaller than mq_msgsize
  // (checked before emptiness), or -EAGAIN when empty. Highest priority
  // first, FIFO within a priority.
  int64_t Receive(std::span<uint8_t> Buffer, uint32_t& Priority) {
    if (static_cast<int64_t>(Buffer.size()) < MsgSize) return -90;
    if (!Count) return -11;
    auto Top = std::prev(Messages.end());
    auto& Front = Top->second.front();
    std::copy(Front.begin(), Front.end(), Buffer.begin());
    const auto Length = static_cast<int64_t>(Front.size());
    Priority = Top->first;
    Bytes -= Front.size();
    Top->second.pop_front();
    if (Top->second.empty()) Messages.erase(Top);
    --Count;
    return Length;
  }

  // The length of the message Receive would return next (0 when empty).
  size_t FrontLength() const { return Count ? std::prev(Messages.end())->second.front().size() : 0; }
  bool Empty() const { return !Count; }
  bool Full() const { return static_cast<int64_t>(Count) >= MaxMsg; }
  int64_t Maximum() const { return MaxMsg; }
  int64_t MessageSize() const { return MsgSize; }
  size_t Pending() const { return Count; }
  size_t QueuedBytes() const { return Bytes; } // the QSIZE of the status line
  MqAttr Attr(int64_t Flags) const { return {Flags, MaxMsg, MsgSize, static_cast<int64_t>(Count)}; }

private:
  int64_t MaxMsg, MsgSize;
  size_t Count {}, Bytes {};
  std::map<uint32_t, std::deque<std::vector<uint8_t>>> Messages;
};
} // namespace Switch2Guest
