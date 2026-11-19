// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace Switch2Guest {
struct TerminalKey {
  std::array<char, 5> Bytes {};
  size_t Length {};
};

inline TerminalKey EncodeTerminalKey(unsigned Key, bool Shift, bool Control, bool Alt, bool CapsLock) {
  char Character {};
  std::string_view Escape;
  if (Key >= 4 && Key <= 29) {
    Character = Control ? Key - 3 : (Shift != CapsLock ? 'A' : 'a') + Key - 4;
  } else if (Key >= 30 && Key <= 39) {
    Character = (Shift ? "!@#$%^&*()" : "1234567890")[Key - 30];
  } else if (Key >= 45 && Key <= 56) {
    Character = (Shift ? "_+{}|~:\"~<>?" : "-=[]\\#;'`,./")[Key - 45];
  } else if (Key >= 89 && Key <= 98) {
    Character = "1234567890"[Key - 89];
  } else {
    switch (Key) {
    case 40: case 88: Character = '\r'; break;
    case 41: Character = '\x1b'; break;
    case 42: Character = '\x7f'; break;
    case 43: Character = '\t'; break;
    case 44: Character = ' '; break;
    case 74: Escape = "\x1b[H"; break;
    case 76: Escape = "\x1b[3~"; break;
    case 77: Escape = "\x1b[F"; break;
    case 79: Escape = "\x1b[C"; break;
    case 80: Escape = "\x1b[D"; break;
    case 81: Escape = "\x1b[B"; break;
    case 82: Escape = "\x1b[A"; break;
    case 84: Character = '/'; break;
    case 85: Character = '*'; break;
    case 86: Character = '-'; break;
    case 87: Character = '+'; break;
    case 99: Character = '.'; break;
    case 100: Character = Shift ? '|' : '\\'; break;
    default: return {};
    }
  }
  TerminalKey Result;
  if (Alt) Result.Bytes[Result.Length++] = '\x1b';
  if (!Escape.empty()) {
    std::copy(Escape.begin(), Escape.end(), Result.Bytes.begin() + Result.Length);
    Result.Length += Escape.size();
  } else {
    if (Control && Character >= '@' && Character <= '_') Character &= 31;
    if (Control && Character == ' ') Character = 0;
    Result.Bytes[Result.Length++] = Character;
  }
  return Result;
}

// The native renderer supports ANSI cursor and color sequences. Consume
// DEC private mode controls here, since it doesn't implement those modes.
class GuestConsoleOutput {
public:
  std::string Filter(std::string_view Input) {
    std::string Output;
    Output.reserve(Input.size());
    for (const unsigned char Byte : Input) {
      switch (Pending) {
      case State::Text:
        if (Byte == 27) Pending = State::Escape;
        else Output += static_cast<char>(Byte);
        break;
      case State::Escape:
        if (Byte == '[') {
          Pending = State::CSI;
        } else {
          Output += '\x1b';
          if (Byte != 27) {
            Output += static_cast<char>(Byte);
            Pending = State::Text;
          }
        }
        break;
      case State::CSI:
        if (Byte == '?') {
          Pending = State::PrivateCSI;
        } else {
          Output += "\x1b[";
          if (Byte == 27) {
            Pending = State::Escape;
          } else {
            Output += static_cast<char>(Byte);
            Pending = State::Text;
          }
        }
        break;
      case State::PrivateCSI:
        if (Byte == 27) Pending = State::Escape;
        else if (Byte < 0x20 || Byte >= 0x40) Pending = State::Text;
        break;
      }
    }
    return Output;
  }

private:
  enum class State { Text, Escape, CSI, PrivateCSI };
  State Pending {};
};

// Linux x86-64 kernel termios. glibc translates its larger user-space termios
// structure through TCGETS and TCSETS.
struct TerminalAttributes {
  uint32_t Input {0x500}; // ICRNL | IXON
  uint32_t Output {5};    // OPOST | ONLCR
  uint32_t Control {0xbf};
  uint32_t Local {0x8a3b};
  uint8_t Line {};
  std::array<uint8_t, 19> Characters {3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22};
};
static_assert(sizeof(TerminalAttributes) == 36);

class GuestTerminalInput {
public:
  TerminalAttributes Attributes {};

  std::string Feed(char Byte) {
    constexpr uint32_t Canonical = 2, Echo = 8, EchoErase = 0x10, EchoKill = 0x20, EchoNewline = 0x40;
    if (Byte == '\r') {
      if (Attributes.Input & 0x80) return {};
      if (Attributes.Input & 0x100) Byte = '\n';
    } else if (Byte == '\n' && (Attributes.Input & 0x40)) {
      Byte = '\r';
    }
    if (Ready.size() + Line.size() >= 4096) return {};
    const auto Matches = [&](size_t Index) {
      return Attributes.Characters[Index] && static_cast<uint8_t>(Byte) == Attributes.Characters[Index];
    };
    if (Attributes.Local & Canonical) {
      if (Matches(2)) {
        if (Line.empty()) return {};
        Line.pop_back();
        return (Attributes.Local & (Echo | EchoErase)) == (Echo | EchoErase) ? "\b \b" : "";
      }
      if (Matches(3)) {
        Line.clear();
        return (Attributes.Local & (Echo | EchoKill)) == (Echo | EchoKill) ? "\n" : "";
      }
      if (Matches(4)) {
        if (Line.empty()) EndOfFile = true;
        else CommitLine();
        return {};
      }
      if (Byte != '\n' && !Matches(11) && Line.size() == 4095) return {};
      Line.push_back(Byte);
      if (Byte == '\n' || Matches(11)) CommitLine();
    } else {
      Ready.push_back(Byte);
    }
    if ((Attributes.Local & Echo) || (Byte == '\n' && (Attributes.Local & EchoNewline))) return std::string(1, Byte);
    return {};
  }

  void SetAttributes(const TerminalAttributes& New, bool Flush) {
    if (Flush) Clear();
    if ((Attributes.Local & 2) && !(New.Local & 2)) CommitLine();
    Attributes = New;
  }

  void Clear() {
    Ready.clear();
    Line.clear();
    EndOfFile = false;
  }

  size_t Pending() const { return Ready.size(); }
  bool Readable() const { return !Ready.empty() || EndOfFile; }
  bool TakeEof() {
    if (!Ready.empty() || !EndOfFile) return false;
    EndOfFile = false;
    return true;
  }

  size_t Read(char* Output, size_t Count) {
    const auto Bytes = std::min(Count, Ready.size());
    for (size_t Index = 0; Index < Bytes; ++Index) {
      Output[Index] = Ready.front();
      Ready.pop_front();
    }
    return Bytes;
  }

private:
  void CommitLine() {
    Ready.insert(Ready.end(), Line.begin(), Line.end());
    Line.clear();
  }

  std::deque<char> Ready;
  std::string Line;
  bool EndOfFile {};
};
} // namespace Switch2Guest
