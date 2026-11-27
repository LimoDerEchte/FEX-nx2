// SPDX-License-Identifier: MIT
// Terminal devices of the Switch 2 guest runtime: one tty object per terminal
// (the launcher's console and every pseudo-terminal) and the devpts index
// table. Linux drivers/tty/n_tty.c, tty_io.c, tty_jobctrl.c and pty.c are the
// reference. The header holds the state and the byte-level rules only; the
// runtime wires them to descriptors, signals and the scheduler.
#pragma once

#include "GuestJobControl.h"
#include "GuestTerminal.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <cstring>
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Switch2Guest {

// The master side of a terminal. The slave side (the line discipline and the
// descriptors of the guest) delivers its output here; the master side
// supplies input through TakeInput. The guest's /dev/ptmx descriptors use
// PtyQueueMaster. A later host-side master (the launcher's terminal tabs)
// implements this interface with locked queues, so the runtime thread and
// the UI thread never share other state.
class TtyMaster {
public:
  virtual ~TtyMaster() = default;
  // Free room for output from the slave side, in bytes. A writer waits when it is 0.
  virtual size_t Space() const = 0;
  // Slave output, already processed by the output flags. At most Space() bytes.
  virtual void Output(const char* Data, size_t Size) = 0;
  // Bytes typed on the master side that the line discipline has not received yet.
  virtual size_t TakeInput(char* Data, size_t Size) = 0;
};

// A master that guest processes read and write through a /dev/ptmx descriptor.
class PtyQueueMaster final : public TtyMaster {
public:
  static constexpr size_t Capacity = 65536;
  size_t Space() const override { return Capacity - Data.size(); }
  void Output(const char* Bytes, size_t Size) override { Data.insert(Data.end(), Bytes, Bytes + Size); }
  size_t TakeInput(char*, size_t) override { return 0; }
  size_t Pending() const { return Data.size(); }
  size_t Read(char* Output, size_t Count) {
    const auto Bytes = std::min(Count, Data.size());
    for (size_t Index = 0; Index < Bytes; ++Index) {
      Output[Index] = Data.front();
      Data.pop_front();
    }
    return Bytes;
  }

private:
  std::deque<char> Data;
};

// The master side of a launcher terminal tab: two locked byte queues, so the runtime thread (Space, Output, TakeInput) and the UI
// thread (Read, Write, Resize) share nothing else. Output waits in the queue until the UI reads it; a full queue makes the slave's
// writers wait (Space 0). Activity() changes whenever the UI read output (room appeared) or wrote input, so the runtime thread
// knows to wake waiters and to pump the typed bytes.
class HostPtyMaster final : public TtyMaster {
public:
  static constexpr size_t OutputCapacity = 128 * 1024, InputCapacity = 1024 * 1024;

  // runtime thread
  size_t Space() const override {
    std::lock_guard<std::mutex> Lock(Mutex);
    return OutputCapacity - Out.size();
  }
  void Output(const char* Data, size_t Size) override {
    std::lock_guard<std::mutex> Lock(Mutex);
    Out.append(Data, Size);
  }
  size_t TakeInput(char* Data, size_t Size) override {
    std::lock_guard<std::mutex> Lock(Mutex);
    const size_t Count = std::min(Size, In.size());
    std::memcpy(Data, In.data(), Count);
    In.erase(0, Count);
    return Count;
  }
  bool HasInput() const {
    std::lock_guard<std::mutex> Lock(Mutex);
    return !In.empty();
  }
  // UI thread
  size_t Read(char* Data, size_t Size) {
    std::lock_guard<std::mutex> Lock(Mutex);
    const size_t Count = std::min(Size, Out.size());
    std::memcpy(Data, Out.data(), Count);
    Out.erase(0, Count);
    if (Count) Touched.fetch_add(1);
    return Count;
  }
  // Input beyond InputCapacity is dropped (a stuck program that never reads); returns the bytes kept.
  size_t Write(const char* Data, size_t Size) {
    std::lock_guard<std::mutex> Lock(Mutex);
    const size_t Count = std::min(Size, InputCapacity - std::min(InputCapacity, In.size()));
    In.append(Data, Count);
    if (Count) Touched.fetch_add(1);
    return Count;
  }
  size_t PendingOutput() const {
    std::lock_guard<std::mutex> Lock(Mutex);
    return Out.size();
  }
  void SetSize(int NewCols, int NewRows) {
    const uint32_t Cols = std::clamp(NewCols, 0, 65535);
    const uint32_t Rows = std::clamp(NewRows, 0, 65535);
    Size.store((Rows << 16) | Cols);
    Touched.fetch_add(1);
  }
  std::pair<uint16_t, uint16_t> WantedSize() const {
    const auto Value = Size.load();
    return {static_cast<uint16_t>(Value), static_cast<uint16_t>(Value >> 16)};
  }
  uint64_t Activity() const { return Touched.load(); }

private:
  mutable std::mutex Mutex;
  std::string Out, In;
  std::atomic<uint64_t> Touched {0};
  std::atomic<uint32_t> Size {(25 << 16) | 80};
};

// c_cflag baud rates (CBAUD and CBAUDEX) in bits per second, indexed by the
// low four bits; the extended table is selected by CBAUDEX (0x1000).
constexpr uint32_t BaudRate(uint32_t ControlFlags) {
  constexpr uint32_t Basic[] {0, 50, 75, 110, 134, 150, 200, 300, 600, 1200, 1800, 2400, 4800, 9600, 19200, 38400};
  constexpr uint32_t Extended[] {0, 57600, 115200, 230400, 460800, 500000, 576000, 921600,
                                 1000000, 1152000, 1500000, 2000000, 2500000, 3000000, 3500000, 4000000};
  return ControlFlags & 0x1000 ? Extended[ControlFlags & 15] : Basic[ControlFlags & 15];
}
constexpr uint32_t BaudOther = 0x1000; // BOTHER: the rate is the explicit speed field

// struct termios2: the kernel termios plus the two explicit speeds.
struct TerminalAttributes2 {
  TerminalAttributes Base;
  uint32_t InputSpeed, OutputSpeed;
};
static_assert(sizeof(TerminalAttributes2) == 44);

constexpr uint32_t TiocpktFlushRead = 1, TiocpktFlushWrite = 2, TiocpktStop = 4, TiocpktStart = 8;

struct TtyReceive {
  uint32_t Signal {}; // the signal for the foreground group, or 0
  std::string Echo;   // bytes the line discipline echoes to the output side
};

class GuestTty {
public:
  GuestTerminalInput Input;
  std::array<uint16_t, 4> Winsize {};
  uint32_t Session {}, Foreground {};
  bool HungUp {};
  // The pty rules of n_tty: output post-processing, ECHOCTL carets and XON/XOFF.
  // The console keeps its older behavior (its output is not post-processed).
  bool FullDiscipline {};
  bool Stopped {};    // stopped by a received STOP character (IXON)
  bool TcoStopped {}; // stopped by TCOOFF
  uint32_t InputSpeed {38400}, OutputSpeed {38400};
  std::shared_ptr<TtyMaster> Master;
  std::shared_ptr<PtyQueueMaster> Queue; // Master when a guest descriptor holds it
  // Identity of a pseudo-terminal slave (devpts).
  bool Pty {};
  uint32_t Index {};
  bool Locked {true};
  bool MasterClosed {};
  bool SlaveOpened {}; // a slave descriptor has existed
  unsigned MasterFiles {}, SlaveFiles {}; // open file descriptions
  uint32_t Uid {}, Gid {5};
  uint32_t Mode {0620};
  // TIOCPKT.
  bool Packet {};
  uint8_t PacketStatus {};

  // tty->link->count == 0 after the slave was open: the master reads EIO.
  bool OtherClosed() const { return SlaveOpened && !SlaveFiles; }

  // The master side typed or the slave side echoed one byte. Returns the
  // signal and echo; the caller delivers both.
  TtyReceive Receive(char Byte) {
    TtyReceive Result;
    auto& Attr = Input.Attributes;
    constexpr uint32_t IxOn = 0x400, IxAny = 0x800;
    if (Input.Quoting()) {
      // The byte after VLNEXT is data, whatever it is.
      Result.Echo = Input.Feed(Byte);
      return Result;
    }
    if (FullDiscipline) {
      const auto Start = Attr.Characters[8], Stop = Attr.Characters[9];
      if (Attr.Input & IxOn) {
        if (Start && static_cast<uint8_t>(Byte) == Start) {
          if (!TcoStopped) SetStopped(false);
          return Result;
        }
        if (Stop && static_cast<uint8_t>(Byte) == Stop) {
          SetStopped(true);
          return Result;
        }
      }
      if (Stopped && !TcoStopped && (Attr.Input & IxAny)) SetStopped(false);
    }
    if (const auto Signal = TerminalSignalFor(static_cast<uint8_t>(Byte), Attr.Characters.data(), Attr.Local)) {
      // n_tty_receive_signal_char: flush unless NOFLSH, echo ^X with ECHO, signal.
      constexpr uint32_t NoFlush = 0x80, Echo = 8, EchoControl = 0x200;
      if (!(Attr.Local & NoFlush)) Input.Clear();
      if (Attr.Local & Echo) {
        if (!FullDiscipline) {
          if (Attr.Local & EchoControl) Result.Echo = {'^', static_cast<char>(Byte ^ 0x40)};
        } else {
          Result.Echo = Input.EchoChar(Byte);
        }
      }
      Result.Signal = Signal;
      return Result;
    }
    Result.Echo = Input.Feed(Byte);
    return Result;
  }

  // Switches the tty to the pty rules of n_tty (see FullDiscipline).
  void UseFullDiscipline() {
    FullDiscipline = true;
    Input.Linux = true;
  }

  // start_tty and stop_tty with the packet-mode status bits of pty_start and pty_stop.
  void SetStopped(bool Value) {
    if (Stopped == Value) return;
    Stopped = Value;
    if (Packet) {
      PacketStatus &= ~(Value ? TiocpktStart : TiocpktStop);
      PacketStatus |= Value ? TiocpktStop : TiocpktStart;
    }
  }

  // do_output_char for the flags the runtime supports: OPOST with ONLCR and OCRNL.
  // The other output flags (OLCUC, ONOCR, ONLRET, delays, tab expansion) are
  // not implemented and pass bytes through.
  std::string ProcessOutput(std::string_view Data) const {
    const auto Flags = Input.Attributes.Output;
    constexpr uint32_t Post = 1, NlToCrNl = 4, CrToNl = 8;
    if (!FullDiscipline || !(Flags & Post)) return std::string(Data);
    std::string Out;
    Out.reserve(Data.size());
    for (const auto Byte : Data) {
      if (Byte == '\n' && (Flags & NlToCrNl)) Out += '\r';
      if (Byte == '\r' && (Flags & CrToNl)) Out += '\n';
      else Out += Byte;
    }
    return Out;
  }

  bool OutputBlocked() const { return Stopped || TcoStopped; }

  // Output of the slave side: process and hand to the master, as much as fits.
  // Returns the input bytes consumed.
  size_t WriteSlave(const char* Data, size_t Count) {
    if (!Master) return Count;
    size_t Used {};
    size_t Room = Master->Space();
    std::string Chunk;
    while (Used < Count) {
      Chunk = ProcessOutput(std::string_view {Data + Used, 1});
      if (Chunk.size() > Room) break;
      Master->Output(Chunk.data(), Chunk.size());
      Room -= Chunk.size();
      ++Used;
    }
    return Used;
  }

  // Delivers echo bytes to the master (best effort: a full queue drops them).
  void EchoToMaster(const std::string& Echo) {
    if (Echo.empty() || !Master) return;
    const auto Text = ProcessOutput(Echo);
    if (Text.size() <= Master->Space()) Master->Output(Text.data(), Text.size());
  }

  size_t InputRoom() const {
    constexpr size_t Limit = 4096;
    return Limit - std::min(Limit, Input.Buffered());
  }

  // Master write: the bytes go through the line discipline. Signal is called
  // with each signal the discipline raises. Returns the bytes consumed.
  template<typename F>
  size_t WriteMaster(const char* Data, size_t Count, F&& Signal) {
    const auto Limit = FullDiscipline ? InputRoom() : Count;
    const auto Take = std::min(Count, Limit);
    for (size_t Index = 0; Index < Take; ++Index) {
      const auto Received = Receive(Data[Index]);
      if (Received.Signal) Signal(Received.Signal);
      EchoToMaster(Received.Echo);
    }
    return Take;
  }

  // A raw read of the slave is ready under n_tty's poll rule (input_available_p).
  bool SlaveReadable() const {
    const auto& Attr = Input.Attributes;
    if (Attr.Local & 2) return Input.Readable();
    const size_t Minimum = !Attr.Characters[5] && Attr.Characters[6] ? Attr.Characters[6] : 1;
    return Input.Pending() >= Minimum;
  }

  // TCFLSH: arg 0 flushes the unread input (TCIFLUSH), 1 only the output queue (TCOFLUSH), 2 both.
  void Flush(uint32_t Queue) {
    if (Queue != 1) {
      Input.Clear();
      if (Packet) PacketStatus |= TiocpktFlushRead;
    }
    // Data the master has not read is already in its queue; Linux flushes only
    // what is still in the driver's flip buffers, which is nothing here.
    if (Queue != 0 && Packet) PacketStatus |= TiocpktFlushWrite;
  }

  // Replaces the termios of the line (TCSETS, TCSETSW, TCSETSF).
  void SetAttributes(const TerminalAttributes& New, bool FlushInput) {
    Input.SetAttributes(New, FlushInput);
    if (FlushInput && Packet) PacketStatus |= TiocpktFlushRead;
  }

  // The rates set_termios derives: explicit speeds only count with BOTHER in the matching field.
  TerminalAttributes2 Attributes2() const {
    const auto& Base = Input.Attributes;
    const auto OutputBits = Base.Control & 0x100f;
    const auto InputBits = (Base.Control >> 16) & 0x100f;
    const auto Output = OutputBits == BaudOther ? OutputSpeed : BaudRate(OutputBits);
    const auto In = !InputBits ? Output : InputBits == BaudOther ? InputSpeed : BaudRate(InputBits);
    return {Base, In, Output};
  }

  void SetAttributes2(const TerminalAttributes2& New, bool FlushInput) {
    Input.SetAttributes(New.Base, FlushInput);
    if (FlushInput && Packet) PacketStatus |= TiocpktFlushRead;
    InputSpeed = New.InputSpeed;
    OutputSpeed = New.OutputSpeed;
  }

  // TIOCSWINSZ on a new size; returns true when the size changed (SIGWINCH is due).
  bool Resize(const std::array<uint16_t, 4>& Size) {
    if (Size == Winsize) return false;
    Winsize = Size;
    return true;
  }

  // Master status and data for a read with Packet mode off.
  size_t MasterPending() const { return Queue ? Queue->Pending() : 0; }
  bool MasterReadable() const { return MasterPending() || (Packet && PacketStatus); }
};

// The devpts index table: the lowest free index, as Linux's ida allocates it.
// A slot is free once its tty object is gone, which happens when the master,
// every slave description and every controlling-terminal reference is gone.
class PtyTable {
public:
  static constexpr uint32_t MaxPtys = 4096;

  std::shared_ptr<GuestTty> Allocate(uint32_t Uid) {
    for (uint32_t Index = 0; Index < MaxPtys; ++Index) {
      const auto Found = Slots.find(Index);
      if (Found != Slots.end() && !Found->second.expired()) continue;
      auto Tty = std::make_shared<GuestTty>();
      Tty->Pty = true;
      Tty->UseFullDiscipline();
      Tty->Index = Index;
      Tty->Uid = Uid;
      Tty->Queue = std::make_shared<PtyQueueMaster>();
      Tty->Master = Tty->Queue;
      Slots[Index] = Tty;
      return Tty;
    }
    return nullptr;
  }

  // The slave /dev/pts/<Index> as a path walk sees it: it exists while the master is open.
  std::shared_ptr<GuestTty> Find(uint32_t Index) const {
    const auto Found = Slots.find(Index);
    if (Found == Slots.end()) return nullptr;
    auto Tty = Found->second.lock();
    return Tty && !Tty->MasterClosed ? Tty : nullptr;
  }

  std::vector<uint32_t> Visible() const {
    std::vector<uint32_t> Result;
    for (const auto& [Index, Weak] : Slots) {
      if (const auto Tty = Weak.lock(); Tty && !Tty->MasterClosed) Result.push_back(Index);
    }
    return Result;
  }

  // "/dev/pts/<n>" to n; false for any other name. Linux names are plain decimal.
  static bool ParseName(std::string_view Name, uint32_t& Index) {
    if (Name.empty() || Name.size() > 5 || (Name.size() > 1 && Name[0] == '0')) return false;
    uint32_t Value {};
    for (const auto Char : Name) {
      if (Char < '0' || Char > '9') return false;
      Value = Value * 10 + (Char - '0');
    }
    Index = Value;
    return true;
  }

private:
  std::map<uint32_t, std::weak_ptr<GuestTty>> Slots;
};

inline PtyTable& GuestPtys() {
  static PtyTable Table;
  return Table;
}

// st_rdev of /dev/pts/<n>: major 136 (new_encode_dev).
constexpr uint64_t PtsRdev(uint32_t Index) {
  return (136ULL << 8) | (Index & 0xff) | (static_cast<uint64_t>(Index & ~0xffu) << 12);
}
constexpr uint64_t PtmxRdev = 0x502; // 5:2
} // namespace Switch2Guest
