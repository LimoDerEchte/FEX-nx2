// SPDX-License-Identifier: MIT
#include "GuestPty.h"

#include <cassert>
#include <cstring>

using namespace Switch2Guest;

namespace {
std::shared_ptr<GuestTty> NewPty() {
  return GuestPtys().Allocate(1000);
}

// Everything the slave side has put out for the master so far.
std::string Drain(GuestTty& Tty) {
  char Buffer[512];
  const auto Count = Tty.Queue->Read(Buffer, sizeof(Buffer));
  return std::string(Buffer, Count);
}

// The master types Data; returns the signals raised.
std::string Type(GuestTty& Tty, std::string_view Data, uint32_t* Signal = nullptr) {
  Tty.WriteMaster(Data.data(), Data.size(), [&](uint32_t Raised) {
    if (Signal) *Signal = Raised;
  });
  return Drain(Tty);
}

void SetLocal(GuestTty& Tty, uint32_t Clear, uint32_t Set) {
  auto Attributes = Tty.Input.Attributes;
  Attributes.Local = (Attributes.Local & ~Clear) | Set;
  Tty.SetAttributes(Attributes, false);
}
} // namespace

int main() {
  // The devpts index table hands out the lowest free index and frees it with the last reference.
  {
    auto First = NewPty(), Second = NewPty(), Third = NewPty();
    assert(First->Index + 1 == Second->Index && Second->Index + 1 == Third->Index);
    const auto Freed = Second->Index;
    Second.reset();
    assert(GuestPtys().Allocate(1)->Index == Freed);
    auto Held = std::weak_ptr<GuestTty>(First);
    First->MasterClosed = true;
    assert(!GuestPtys().Find(First->Index)); // the node is gone, the object stays while referenced
    assert(!Held.expired());
    uint32_t Index {};
    assert(PtyTable::ParseName("12", Index) && Index == 12);
    assert(!PtyTable::ParseName("012", Index) && !PtyTable::ParseName("a", Index) && !PtyTable::ParseName("", Index));
    assert(PtsRdev(3) == 0x8803 && PtsRdev(256) == 0x108800);
  }

  // The signal characters in the sequence of pty/isig_signals_to_foreground_group: every one raises its signal, ^C also with a
  // typed line pending (and flushes it), and a changed VINTR raises SIGINT on its new character.
  {
    auto Tty = NewPty();
    uint32_t Signal = 0;
    Type(*Tty, "\x03", &Signal);
    assert(Signal == 2);
    Signal = 0;
    Type(*Tty, "\x1c", &Signal);
    assert(Signal == 3);
    Signal = 0;
    Type(*Tty, "\x1a", &Signal);
    assert(Signal == 20);
    assert(Type(*Tty, "typed\n") == "typed\r\n");
    assert(Tty->Input.Pending() != 0);
    Signal = 0;
    assert(Type(*Tty, "\x03", &Signal) == "^C");
    assert(Signal == 2 && Tty->Input.Pending() == 0);  // flushed
    auto Attributes = Tty->Input.Attributes;
    Attributes.Characters[0] = 'x';
    Tty->SetAttributes(Attributes, false);
    Signal = 0;
    Type(*Tty, "x", &Signal);
    assert(Signal == 2);
    Signal = 0;
    Type(*Tty, "\x03", &Signal);  // the old character is data now
    assert(Signal == 0);
  }

  // Defaults of a new slave, and the baud rates of termios2.
  {
    auto Tty = NewPty();
    const auto& Attr = Tty->Input.Attributes;
    assert(Attr.Input == 0x500 && Attr.Output == 5 && Attr.Control == 0xbf && Attr.Local == 0x8a3b);
    assert(Tty->Locked && Tty->Gid == 5 && Tty->Mode == 0620 && Tty->Uid == 1000);
    auto Pair = Tty->Attributes2();
    assert(Pair.InputSpeed == 38400 && Pair.OutputSpeed == 38400);
    auto New = Attr;
    New.Control = (New.Control & ~0x100fu) | 0xd; // B9600
    Tty->SetAttributes(New, false);
    Pair = Tty->Attributes2();
    assert(Pair.InputSpeed == 9600 && Pair.OutputSpeed == 9600);
    Pair.Base.Control = (Pair.Base.Control & ~0x100fu) | BaudOther;
    Pair.InputSpeed = Pair.OutputSpeed = 12345;
    Tty->SetAttributes2(Pair, false);
    assert(Tty->Attributes2().OutputSpeed == 12345);
    assert(BaudRate(0x1002) == 115200 && BaudRate(0xf) == 38400);
    const std::array<uint16_t, 4> Size {24, 80, 0, 0};
    assert(Tty->Resize(Size) && !Tty->Resize(Size));
  }

  // Echo and editing, byte for byte as a Linux pty produces them (checked against a native run).
  {
    auto Tty = NewPty();
    assert(Type(*Tty, "hi\n") == "hi\r\n");
    assert(Type(*Tty, "\x01") == "^A");
    assert(Type(*Tty, "\x7f") == "\b \b\b \b");
    assert(Type(*Tty, "ab\x7f") == "ab\b \b");
    assert(Type(*Tty, "c\x15") == "c\b \b\b \b");
    SetLocal(*Tty, 0x800, 0); // no ECHOKE
    assert(Type(*Tty, "d\x15") == "d^U\r\n");
    SetLocal(*Tty, 0x10, 0); // no ECHOE
    assert(Type(*Tty, "ef\x7f\x15") == "ef^?^U\r\n");
    SetLocal(*Tty, 0, 0x10 | 0x800);
    assert(Type(*Tty, "ab cd\x17") == "ab cd\b \b\b \b");
    assert(Type(*Tty, "\x15") == "\b \b\b \b\b \b");
    assert(Type(*Tty, "ab\x12") == "ab^R\r\nab");
    assert(Type(*Tty, "\x15") == "\b \b\b \b");
    assert(Type(*Tty, "a\t\x7f") == "a\t\b\b\b\b\b\b\b");
    assert(Type(*Tty, "\x15").size() >= 3);
    assert(Type(*Tty, "\x04") == ""); // EOF is not echoed
    assert(Type(*Tty, "\x1b[A") == "^[[A");
    SetLocal(*Tty, 0x200, 0); // no ECHOCTL
    assert(Type(*Tty, "\x01") == "\x01");
    SetLocal(*Tty, 0, 0x200);
    // ^V quotes the next character.
    char Buffer[64];
    Tty->Input.Clear();
    assert(Type(*Tty, "\x16\x03x\n") == "^\b^Cx\r\n");
    assert(Tty->Input.Read(Buffer, sizeof(Buffer)) == 3 && !memcmp(Buffer, "\x03x\n", 3));
    // ECHO off with ECHONL: only the newline; raw ECHO echoes every byte, newline as ^J.
    SetLocal(*Tty, 8, 0x40);
    assert(Type(*Tty, "q\n") == "\r\n");
    Tty->Input.Clear();
    SetLocal(*Tty, 2 | 0x40, 8);
    assert(Type(*Tty, "k\n") == "k^J");
  }

  // Signals: echo, flush, the foreground group's signal number.
  {
    auto Tty = NewPty();
    uint32_t Signal {};
    assert(Type(*Tty, "typed\n").size() == 7);
    assert(Tty->Input.Pending() == 6);
    assert(Type(*Tty, "\x03", &Signal) == "^C" && Signal == 2 && Tty->Input.Pending() == 0);
    assert(Type(*Tty, "\x1c", &Signal) == "^\\" && Signal == 3);
    assert(Type(*Tty, "\x1a", &Signal) == "^Z" && Signal == 20);
    SetLocal(*Tty, 1, 0); // ISIG off: data
    Signal = 0;
    Type(*Tty, "\x03", &Signal);
    assert(!Signal);
  }

  // Output processing and room.
  {
    auto Tty = NewPty();
    assert(Tty->ProcessOutput("a\nb\r\n") == "a\r\nb\r\r\n");
    auto Attr = Tty->Input.Attributes;
    Attr.Output = 1 | 8; // OPOST | OCRNL
    Tty->SetAttributes(Attr, false);
    assert(Tty->ProcessOutput("x\ry\n") == "x\ny\n");
    Attr.Output = 0;
    Tty->SetAttributes(Attr, false);
    assert(Tty->ProcessOutput("p\nq") == "p\nq");
    Attr.Output = 5;
    Tty->SetAttributes(Attr, false);
    // The master's queue holds PtyQueueMaster::Capacity bytes; ONLCR counts both bytes of a newline.
    const std::string Block(PtyQueueMaster::Capacity - 1, 'x');
    assert(Tty->WriteSlave(Block.data(), Block.size()) == Block.size());
    assert(Tty->WriteSlave("\n", 1) == 0); // would need two bytes of one
    assert(Tty->WriteSlave("y", 1) == 1);
    assert(Tty->WriteSlave("z", 1) == 0);
    assert(Tty->Queue->Pending() == PtyQueueMaster::Capacity);
  }

  // Flow control, packet mode and flushes.
  {
    auto Tty = NewPty();
    Tty->Packet = true;
    assert(!Tty->OutputBlocked());
    assert(Type(*Tty, "\x13").empty() && Tty->OutputBlocked() && (Tty->PacketStatus & TiocpktStop));
    Type(*Tty, "\x11");
    assert(!Tty->OutputBlocked() && (Tty->PacketStatus & TiocpktStart) && !(Tty->PacketStatus & TiocpktStop));
    Tty->TcoStopped = true;
    Tty->SetStopped(true);
    Type(*Tty, "\x11"); // IXON cannot restart a TCOOFF
    assert(Tty->OutputBlocked());
    Tty->TcoStopped = false;
    Tty->SetStopped(false);
    Tty->PacketStatus = 0;
    Tty->Flush(0);
    assert(Tty->PacketStatus == TiocpktFlushRead);
    Tty->Flush(1);
    assert(Tty->PacketStatus == (TiocpktFlushRead | TiocpktFlushWrite));
    assert(Tty->MasterReadable());
    // Without IXON the bytes are data.
    auto Attr = Tty->Input.Attributes;
    Attr.Input = 0;
    Attr.Local = 0;
    Tty->SetAttributes(Attr, true);
    Type(*Tty, "\x13");
    assert(!Tty->OutputBlocked() && Tty->Input.Pending() == 1);
  }

  // Raw reads: the poll rule of n_tty with VMIN and VTIME; master write room.
  {
    auto Tty = NewPty();
    auto Attr = Tty->Input.Attributes;
    Attr.Local = 0;
    Attr.Input = 0;
    Attr.Characters[6] = 3;
    Attr.Characters[5] = 0;
    Tty->SetAttributes(Attr, true);
    Type(*Tty, "ab");
    assert(!Tty->SlaveReadable());
    Type(*Tty, "c");
    assert(Tty->SlaveReadable());
    Attr.Characters[5] = 1; // VTIME set: one byte is enough for poll
    Tty->SetAttributes(Attr, true);
    Type(*Tty, "a");
    assert(Tty->SlaveReadable());
    Tty->Input.Clear();
    const std::string Many(5000, 'm');
    assert(Tty->WriteMaster(Many.data(), Many.size(), [](uint32_t) {}) == 4096);
    assert(Tty->InputRoom() == 0);
  }

  // Host queues isolate input, output and resize state per tab.
  {
    HostPtyMaster First, Second;
    char Bytes[8] {};
    assert(First.Write("abc", 3) == 3 && !Second.HasInput());
    assert(First.TakeInput(Bytes, 2) == 2 && std::string_view(Bytes, 2) == "ab");
    assert(First.TakeInput(Bytes, sizeof(Bytes)) == 1 && Bytes[0] == 'c');
    const std::string Full(HostPtyMaster::OutputCapacity, 'x');
    First.Output(Full.data(), Full.size());
    const auto Before = First.Activity();
    assert(!First.Space() && !Second.PendingOutput());
    assert(First.Read(Bytes, sizeof(Bytes)) == sizeof(Bytes) && First.Space() == sizeof(Bytes));
    assert(First.Activity() != Before);
    First.SetSize(138, 42);
    const auto [Cols, Rows] = First.WantedSize();
    assert(Cols == 138 && Rows == 42 && First.WantedSize() != Second.WantedSize());
  }

  // The keys of a terminal tab reach a raw-mode reader (less, readline) unchanged: PageUp, PageDown, Home, End, Delete as typed by
  // the launcher, written through the host master, the way the runtime pumps them into the line discipline.
  {
    auto Tty = NewPty();
    auto Attributes = Tty->Input.Attributes;
    Attributes.Local &= ~uint32_t(2 | 8 | 1);  // raw: no ICANON, ECHO, ISIG (what less sets)
    Attributes.Input &= ~uint32_t(0x100 | 0x400);
    Tty->SetAttributes(Attributes, false);
    HostPtyMaster Master;
    for (const char* Keys : {"\x1b[5~", "\x1b[6~", "\x1b[H", "\x1b[F", "\x1b[3~", "\x1b[1;2A"})
      Master.Write(Keys, std::strlen(Keys));
    std::string Seen;
    char Typed[256];
    while (Tty->InputRoom() && Master.HasInput()) {
      const auto Count = Master.TakeInput(Typed, std::min<size_t>(sizeof(Typed), Tty->InputRoom()));
      Tty->WriteMaster(Typed, Count, [](uint32_t) {});
    }
    char Out[64];
    Seen.assign(Out, Tty->Input.Read(Out, sizeof(Out)));
    assert(Seen == "\x1b[5~\x1b[6~\x1b[H\x1b[F\x1b[3~\x1b[1;2A");
    assert(Drain(*Tty).empty());  // no echo
  }

  // Close bookkeeping a master read relies on.
  {
    auto Tty = NewPty();
    assert(!Tty->OtherClosed());
    Tty->SlaveOpened = true;
    Tty->SlaveFiles = 1;
    assert(!Tty->OtherClosed());
    Tty->SlaveFiles = 0;
    assert(Tty->OtherClosed());
  }
}
