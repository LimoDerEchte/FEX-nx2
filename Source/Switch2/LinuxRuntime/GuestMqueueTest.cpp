// SPDX-License-Identifier: MIT
#include "../GuestMqueue.h"
#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(x)                                                     \
  do {                                                               \
    if (!(x)) {                                                      \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x);        \
      std::exit(1);                                                  \
    }                                                                \
  } while (0)

using namespace Switch2Guest;

static std::span<const uint8_t> Text(const char* S) {
  return {reinterpret_cast<const uint8_t*>(S), std::char_traits<char>::length(S)};
}

int main() {
  // Priority order and FIFO within a priority (native: 32767 first, then
  // the two 5s in send order, then 1).
  {
    MessageQueue Queue {10, 8192};
    uint8_t Buffer[8192];
    uint32_t Priority {};
    CHECK(Queue.Receive(Buffer, Priority) == -11);
    CHECK(Queue.Send(Text("a5"), 5) == 0);
    CHECK(Queue.Send(Text("lo"), 1) == 0);
    CHECK(Queue.Send(Text("b5"), 5) == 0);
    CHECK(Queue.Send(Text("top"), 32767) == 0);
    CHECK(Queue.Pending() == 4 && Queue.QueuedBytes() == 9);
    CHECK(Queue.FrontLength() == 3);
    CHECK(Queue.Receive(Buffer, Priority) == 3 && Priority == 32767 && Buffer[0] == 't');
    CHECK(Queue.Receive(Buffer, Priority) == 2 && Priority == 5 && Buffer[0] == 'a');
    CHECK(Queue.Receive(Buffer, Priority) == 2 && Priority == 5 && Buffer[0] == 'b');
    CHECK(Queue.Receive(Buffer, Priority) == 2 && Priority == 1 && Buffer[0] == 'l');
    CHECK(Queue.Empty() && Queue.QueuedBytes() == 0);
  }

  // Priority limit, EMSGSIZE both ways, full and empty.
  {
    MessageQueue Queue {2, 16};
    uint8_t Big[17] {};
    uint8_t Small[15];
    uint8_t Exact[16];
    uint32_t Priority {};
    CHECK(Queue.Send(Text("x"), 32768) == -22);
    CHECK(Queue.Send(Big, 0) == -90);              // longer than mq_msgsize
    CHECK(Queue.Receive(Small, Priority) == -90);  // buffer shorter than mq_msgsize, even when empty
    CHECK(Queue.Receive(Exact, Priority) == -11);
    CHECK(Queue.Send(Text("a"), 0) == 0);
    CHECK(Queue.Send(Text("b"), 0) == 0);
    CHECK(Queue.Full());
    CHECK(Queue.Send(Text("c"), 0) == -11);
    CHECK(Queue.Receive(Small, Priority) == -90);  // the message stays queued
    CHECK(Queue.Pending() == 2);
    CHECK(Queue.Receive(Exact, Priority) == 1);
    CHECK(!Queue.Full());
    const auto Attr = Queue.Attr(04000);
    CHECK(Attr.Flags == 04000 && Attr.MaxMsg == 2 && Attr.MsgSize == 16 && Attr.CurMsgs == 1);
    // A zero-length message is legal.
    CHECK(Queue.Send({}, 3) == 0);
    CHECK(Queue.Receive(Exact, Priority) == 0 && Priority == 3);
  }

  // Attribute checks.
  CHECK(CheckMqAttr({0, 10, 8192, 0}, false) == 0);
  CHECK(CheckMqAttr({0, 11, 8192, 0}, false) == -22);
  CHECK(CheckMqAttr({0, 10, 8193, 0}, false) == -22);
  CHECK(CheckMqAttr({0, 0, 10, 0}, true) == -22);
  CHECK(CheckMqAttr({0, 10, 0, 0}, true) == -22);
  CHECK(CheckMqAttr({0, 65536, 16 * 1024 * 1024, 0}, true) == 0);
  CHECK(CheckMqAttr({0, 65537, 8, 0}, true) == -22);
  // The default queue is charged just under a ninth of RLIMIT_MSGQUEUE
  // (native: nine default queues fit in 819200 bytes, a tenth does not).
  CHECK(MqChargedBytes(10, 8192) * 9 <= 819200 && MqChargedBytes(10, 8192) * 10 > 819200);
  std::puts("GuestMqueueTest ok");
  return 0;
}
