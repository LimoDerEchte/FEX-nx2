// SPDX-License-Identifier: MIT
// Packet-mode pipes (pipe2 O_DIRECT) over StreamSocket, checked against the
// native Linux reads recorded in the WP3 notes.
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

struct Pipe {
  std::shared_ptr<Socket> Reader {std::make_shared<Socket>()}, Writer {std::make_shared<Socket>()};
  Pipe() {
    Reader->Pipe = Writer->Pipe = true;
    Reader->Connect(Writer);
    Writer->Connect(Reader);
  }
  size_t Write(size_t Count, bool Packet) {
    std::vector<uint8_t> Data(Count, 'a');
    return Writer->Send(Data, {}, true, Packet);
  }
  // What a read of Count bytes returns (a ReadSocket without blocking).
  size_t Read(size_t Count) {
    const auto Bytes = Reader->ReadLimit(Count);
    Reader->ConsumeRead(Bytes);
    return Bytes;
  }
};

int main() {
  // A packet read gives one packet and discards the rest (native: write 3,
  // write 4, read 2 -> 2, read 100 -> 4).
  {
    Pipe P;
    CHECK(P.Write(3, true) == 3 && P.Write(4, true) == 4);
    CHECK(P.Read(2) == 2);
    CHECK(P.Read(100) == 4);
    CHECK(P.Reader->Input.empty() && P.Reader->PipeBuffers.empty());
  }
  // Big writes split on page boundaries: 9000 -> 4096, 4096, 808.
  {
    Pipe P;
    CHECK(P.Write(9000, true) == 9000);
    CHECK(P.Read(10000) == 4096 && P.Read(10000) == 4096 && P.Read(10000) == 808);
  }
  // Packets never merge: 4096, 4097 and 1 read back as 4096, 4096, 1, 1.
  {
    Pipe P;
    CHECK(P.Write(4096, true) == 4096 && P.Write(4097, true) == 4097 && P.Write(1, true) == 1);
    CHECK(P.Read(5000) == 4096 && P.Read(5000) == 4096 && P.Read(5000) == 1 && P.Read(5000) == 1);
  }
  // Without packet mode writes merge and a read crosses write boundaries.
  {
    Pipe P;
    CHECK(P.Write(3, false) == 3 && P.Write(4, false) == 4);
    CHECK(P.Read(100) == 7);
  }
  // The mode follows the buffer, not the reader: a read runs over stream
  // buffers and stops inside the first packet it touches.
  {
    Pipe P;
    CHECK(P.Write(3, false) == 3 && P.Write(2, true) == 2 && P.Write(5, false) == 5);
    CHECK(P.Read(4) == 4);    // 3 stream bytes and 1 of the packet; the packet's other byte is dropped
    CHECK(P.Read(100) == 5);  // the later stream write
    CHECK(P.Reader->Input.empty());
    CHECK(P.Write(3, false) == 3 && P.Write(2, true) == 2 && P.Write(5, false) == 5);
    CHECK(P.Read(100) == 5);  // 3 + the whole packet, then stop
    CHECK(P.Read(100) == 5);
  }
  std::puts("GuestPipePacketTest ok");
  return 0;
}
