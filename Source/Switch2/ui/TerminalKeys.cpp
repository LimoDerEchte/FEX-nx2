// SPDX-License-Identifier: MIT
#include "TerminalKeys.h"

namespace ui {

std::string EncodeTerminalKey(const TerminalGrid& Grid, Key K, uint8_t Mods, char32_t Text) {
  const int Modifier = 1 + (Mods & (Shift | Alt | Ctrl));
  const auto LetterKey = [&](char Final, bool SS3) {
    if (Modifier > 1)
      return "\x1b[1;" + std::to_string(Modifier) + Final;
    return std::string(SS3 ? "\x1bO" : "\x1b[") + Final;
  };
  const auto TildeKey = [&](int Number) {
    std::string Seq = "\x1b[" + std::to_string(Number);
    if (Modifier > 1)
      Seq += ";" + std::to_string(Modifier);
    return Seq + '~';
  };

  if (K >= Key::Up && K <= Key::End)
    return LetterKey("ABCDHF"[int(K) - int(Key::Up)], Grid.ApplicationCursor());
  if (K >= Key::F1 && K <= Key::F4)
    return LetterKey("PQRS"[int(K) - int(Key::F1)], true);
  if (K >= Key::F5 && K <= Key::F12) {
    constexpr int Numbers[] = {15, 17, 18, 19, 20, 21, 23, 24};
    return TildeKey(Numbers[int(K) - int(Key::F5)]);
  }
  if (K >= Key::Insert && K <= Key::PageDown) {
    constexpr int Numbers[] = {2, 3, 5, 6};  // Insert, Delete, PageUp, PageDown (4 is End)
    return TildeKey(Numbers[int(K) - int(Key::Insert)]);
  }
  if (K >= Key::Keypad0 && K <= Key::KeypadEquals) {
    const int N = int(K) - int(Key::Keypad0);
    if (Grid.ApplicationKeypad())
      return LetterKey("pqrstuvwxynojmkMX"[N], true);
    Text = "0123456789./*-+\r="[N];
    K = Key::Text;
  }
  if (K == Key::Tab && (Mods & Shift)) {
    if (Mods == Shift)
      return "\x1b[Z";
    return LetterKey('Z', false);
  }

  std::string Out;
  switch (K) {
  case Key::Enter: Text = '\r'; break;
  case Key::Backspace: Text = Mods & Ctrl ? '\b' : '\x7f'; break;
  case Key::Tab: Text = '\t'; break;
  case Key::Escape: Text = '\x1b'; break;
  case Key::Text:
    if (Text == 0 || Text > 0x10'FFFF || (Text >= 0xD800 && Text <= 0xDFFF))
      return {};
    if (Mods & Ctrl) {
      if (Text >= U'a' && Text <= U'z')
        Text -= U'a' - U'A';
      if (Text >= U'@' && Text <= U'_')
        Text &= 0x1F;
      else if (Text == U' ')
        Text = 0;
      else if (Text == U'?')
        Text = 0x7F;
    }
    break;
  default: return {};
  }
  if (Mods & Alt)
    Out += '\x1b';
  AppendTerminalUtf8(Out, Text);
  return Out;
}

std::string EncodeTerminalPaste(const TerminalGrid& Grid, std::string_view Text) {
  if (Text.empty() || !Grid.BracketedPaste())
    return std::string(Text);
  std::string Out = "\x1b[200~";
  Out.append(Text);
  Out += "\x1b[201~";
  return Out;
}

} // namespace ui
