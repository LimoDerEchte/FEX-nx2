// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
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

  // n_tty's input editing and echo rules (pseudo-terminals): ISTRIP, VWERASE, VREPRINT, VLNEXT, ECHOKE and
  // ECHOCTL erase and kill echo. The console keeps the older, simpler Feed below.
  bool Linux {};

  std::string Feed(char Byte) {
    if (Linux) return FeedLinux(Byte);
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
  size_t Buffered() const { return Ready.size() + Line.size(); }
  bool Quoting() const { return LiteralNext; } // the next byte is literal (VLNEXT)
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

  // n_tty echo_char: a control character other than TAB shows as ^X with ECHOCTL.
  std::string EchoChar(char Byte) const {
    const auto Value = static_cast<uint8_t>(Byte);
    if ((Attributes.Local & 0x200) && (Value < 0x20 || Value == 0x7f) && Byte != '\t') {
      return {'^', static_cast<char>(Value ^ 0x40)};
    }
    return std::string(1, Byte);
  }

private:
  // Display width of a character already in the line: TAB goes to the next stop of 8.
  size_t Width(char Byte, size_t Column) const {
    const auto Value = static_cast<uint8_t>(Byte);
    if (Byte == '\t') return 8 - Column % 8;
    return (Value < 0x20 || Value == 0x7f) && (Attributes.Local & 0x200) ? 2 : 1;
  }

  // Removes the last character of the line; the echo (n_tty echo_erase) erases it from the screen
  // when Visual is set. The column of a TAB assumes the line started at column 0.
  std::string DropLast(bool Visual) {
    size_t Column {};
    for (size_t Index = 0; Index + 1 < Line.size(); ++Index) Column += Width(Line[Index], Column);
    const auto Count = Width(Line.back(), Column);
    const bool Tab = Line.back() == '\t';
    Line.pop_back();
    if (!Visual) return {};
    if (Tab) return std::string(Count, '\b');
    std::string Text;
    for (size_t Index = 0; Index < Count; ++Index) Text += "\b \b";
    return Text;
  }

  // ERASE removes one character; WERASE removes a word (BSD ALTWERASE: alphanumerics and _ form words).
  std::string EraseText(bool Word, bool Visual) {
    std::string Text;
    size_t Words {};
    while (!Line.empty()) {
      const auto Last = static_cast<unsigned char>(Line.back());
      if (Word) {
        const bool WordChar = std::isalnum(Last) || Last == '_';
        if (WordChar) ++Words;
        else if (Words) break;
      }
      Text += DropLast(Visual);
      if (!Word) break;
    }
    return Text;
  }

  std::string FeedLinux(char Byte) {
    constexpr uint32_t Canonical = 2, Echo = 8, EchoErase = 0x10, EchoKill = 0x20, EchoNewline = 0x40, Extended = 0x8000,
                       EchoKillErase = 0x800, StripEighth = 0x20;
    constexpr size_t Verase = 2, Vkill = 3, Veof = 4, Veol = 11, Vreprint = 12, Vwerase = 14, Vlnext = 15, Veol2 = 16;
    const auto& Local = Attributes.Local;
    std::string Text;
    const auto Special = [&](size_t Index) {
      return Attributes.Characters[Index] && static_cast<uint8_t>(Byte) == Attributes.Characters[Index];
    };
    const bool Canon = Local & Canonical;
    if (LiteralNext) {
      LiteralNext = false;
      if (Local & Echo) Text += EchoChar(Byte);
      if (Canon) {
        if (Line.size() < 4095) Line.push_back(Byte);
      } else if (Ready.size() < 4096) {
        Ready.push_back(Byte);
      }
      return Text;
    }
    if (Attributes.Input & StripEighth) Byte &= 0x7f;
    if (Byte == '\r') {
      if (Attributes.Input & 0x80) return {};
      if (Attributes.Input & 0x100) Byte = '\n';
    } else if (Byte == '\n' && (Attributes.Input & 0x40)) {
      Byte = '\r';
    }
    if (Ready.size() + Line.size() >= 4096) return {};
    if (!Canon) {
      Ready.push_back(Byte);
      return Local & Echo ? EchoChar(Byte) : std::string {};
    }
    if ((Local & Extended) && Special(Vlnext)) {
      LiteralNext = true;
      return (Local & Echo) && (Local & 0x200) ? "^\b" : "";
    }
    if (Special(Verase) || Special(Vkill) || ((Local & Extended) && Special(Vwerase))) {
      if (Line.empty()) return {};
      const bool Kill = !Special(Verase) && Special(Vkill);
      const bool Word = !Special(Verase) && !Kill;
      if (Kill) {
        if (!(Local & Echo)) {
          Line.clear();
          return {};
        }
        // Only ECHOK, ECHOKE and ECHOE together erase the line on the screen.
        if (!(Local & EchoKill) || !(Local & EchoKillErase) || !(Local & EchoErase)) {
          Line.clear();
          Text = EchoChar(Byte);
          if (Local & EchoKill) Text += '\n';
          return Text;
        }
        while (!Line.empty()) Text += DropLast(true);
        return Text;
      }
      if (!(Local & Echo)) return EraseText(Word, false);
      if (!(Local & EchoErase)) {
        EraseText(Word, false);
        return EchoChar(Byte);
      }
      return EraseText(Word, true);
    }
    if ((Local & Extended) && (Local & Echo) && Special(Vreprint)) {
      Text = EchoChar(Byte) + '\n';
      for (const auto Item : Line) Text += EchoChar(Item);
      return Text;
    }
    if (Byte == '\n') {
      Line.push_back(Byte);
      CommitLine();
      return (Local & (Echo | EchoNewline)) ? "\n" : "";
    }
    if (Special(Veof)) {
      if (Line.empty()) EndOfFile = true;
      else CommitLine();
      return {};
    }
    if (Special(Veol) || ((Local & Extended) && Special(Veol2))) {
      Line.push_back(Byte);
      CommitLine();
      return Local & Echo ? EchoChar(Byte) : std::string {};
    }
    if (Line.size() == 4095) return {};
    Line.push_back(Byte);
    return Local & Echo ? EchoChar(Byte) : std::string {};
  }

  void CommitLine() {
    Ready.insert(Ready.end(), Line.begin(), Line.end());
    Line.clear();
  }

  std::deque<char> Ready;
  std::string Line;
  bool EndOfFile {};
  bool LiteralNext {};
};
} // namespace Switch2Guest
