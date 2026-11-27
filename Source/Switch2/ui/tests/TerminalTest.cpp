// SPDX-License-Identifier: MIT
#include "TerminalGrid.h"
#include "TerminalKeys.h"
#include "platform/HidKeyMap.h"

#include <cstdio>

using namespace ui;

namespace {

int Checks = 0, Failures = 0;
#define CHECK(...) do { \
  ++Checks; \
  if (!(__VA_ARGS__)) { \
    ++Failures; \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #__VA_ARGS__); \
  } \
} while (0)

void Controls() {
  TerminalGrid G(12, 5);
  G.Feed("abc\bZ\tT\nL\vV\fF\rR\a");
  CHECK(G.RowText(0) == "abZ     T");
  CHECK(G.At(9, 1).Ch == U'L' && G.At(10, 2).Ch == U'V' && G.At(11, 3).Ch == U'F');
  CHECK(G.At(0, 3).Ch == U'R' && G.TakeBell() && !G.TakeBell());
  G.Feed("\x1b[H\x1b[3g\x1b[4G\x1bH\r\t");
  CHECK(G.CursorCol() == 3);
  G.Feed("\x1b[g\r\t");
  CHECK(G.CursorCol() == 11);
  G.Feed("\x1b" "c\t");
  CHECK(G.CursorCol() == 8 && !G.AltScreen());

  TerminalGrid W(4, 3);
  W.Feed("abcd\x1b[1m");
  CHECK(W.CursorCol() == 3 && W.CursorRow() == 0);
  W.Feed("e\x1b[?7lfghij");
  CHECK(W.RowText(1) == "efgj" && W.CursorRow() == 1);
  W.Feed("\x1b[?7h\rABCD\rZ");
  CHECK(W.RowText(1) == "ZBCD" && W.CursorRow() == 1);
  W.Feed("\x1b[2;4H\x1b" "D");
  CHECK(W.CursorRow() == 2 && W.CursorCol() == 3);
  W.Feed("\x1b" "E");
  CHECK(W.CursorRow() == 2 && W.CursorCol() == 0 && W.ScrollbackLines() == 1);
}

void Utf8AndCharset() {
  TerminalGrid G(20, 4);
  G.Feed("a\xc3");
  G.Feed("\xa9\xe7\x95");
  G.Feed("\x8cx\xf0\x9f\x98\x80");
  CHECK(G.RowText(0) == "aé界x😀" && G.CursorCol() == 7);
  CHECK(G.At(2, 0).Width == 2 && G.At(3, 0).Width == 0 && G.At(4, 0).Ch == U'x');
  G.Feed("\r\n\xc0\xaf\xe2\x82!\xed\xa0\x80\xf4\x90\x80\x80");
  CHECK(G.RowText(1) == "���!�������");
  G.Feed("\r\n\x1b(");
  G.Feed("0lqkxmj\x1b" "7\x1b(Bq\x1b" "8q");
  CHECK(G.RowText(2) == "┌─┐│└┘─");
  G.Feed("\x1b(B\x1b[1;4H\x1b[X");
  CHECK(G.RowText(0) == "aé  x😀");
  G.Resize(6, 4);
  CHECK(G.RowText(0) == "aé  x" && G.At(5, 0).Width == 1);

  TerminalGrid W(4, 3);
  W.Feed("abc界!\r\n界ab\r\x1b[C\x1b[P");
  CHECK(W.RowText(0) == "abc" && W.RowText(1) == "界!");
  CHECK(W.RowText(2) == " ab" && W.At(0, 2).Width == 1);
  W.Feed("\x1b[2;2H\x1b[@");
  CHECK(W.RowText(1) == "   !");
  W.Feed("\x1b[2;1H界\x1b[2;1H\x1b[@");
  CHECK(W.RowText(1) == " 界" && W.At(2, 1).Width == 0);
}

void CursorEraseAndScroll() {
  TerminalGrid G(8, 5);
  G.Feed("\x1b[4;5H\x1b[2A\x1b[0B\x1b[2D\x1b[2C");
  CHECK(G.CursorCol() == 4 && G.CursorRow() == 2);
  G.Feed("\x1b[2E\x1b[F\x1b[3G");
  CHECK(G.CursorCol() == 2 && G.CursorRow() == 3);
  G.Feed("\x1b[1d\x1b[6`\x1b[0a\x1b[e");
  CHECK(G.CursorCol() == 6 && G.CursorRow() == 1);
  G.Feed("\x1b[3;4f!");
  CHECK(G.At(3, 2).Ch == U'!');

  G.Reset();
  G.Feed("123456\r\x1b[C\x1b[2X\x1b[2@");
  CHECK(G.RowText(0) == "1    456");
  G.Feed("\x1b[4P");
  CHECK(G.RowText(0) == "1456");
  G.Feed("\x1b[2Kabcdef\x1b[4G\x1b[1K");
  CHECK(G.RowText(0) == "    def");
  G.Feed("\x1b[K");
  CHECK(G.RowText(0).empty());
  G.Feed("\x1b[H123\r\n456\r\n789\x1b[2;2H\x1b[1J");
  CHECK(G.RowText(0).empty() && G.RowText(1) == "  6" && G.RowText(2) == "789");
  G.Feed("\x1b[2J\x1b[H1\r\n2\r\n3\r\n4\r\n5\x1b[2;4r\x1b[4;1H\n");
  CHECK(G.RowText(0) == "1" && G.RowText(1) == "3" && G.RowText(3).empty() && G.RowText(4) == "5");
  G.Feed("\x1b[2;1H\x1bM");
  CHECK(G.RowText(1).empty() && G.RowText(2) == "3" && G.RowText(3) == "4");
  G.Feed("\x1b[3;1H\x1b[L");
  CHECK(G.RowText(2).empty() && G.RowText(3) == "3");
  G.Feed("\x1b[M\x1b[S");
  CHECK(G.RowText(1) == "3" && G.RowText(2).empty());
  G.Feed("\x1b[T");
  CHECK(G.RowText(1).empty() && G.RowText(2) == "3" && G.ScrollbackLines() == 0);
  G.Feed("\x1b[r\x1b[S");
  CHECK(G.ScrollbackLines() == 1);
  G.ScrollView(1);
  CHECK(G.ViewAt(0, 0).Ch == U'1');
  G.Feed("\x1b[3J");
  CHECK(G.ScrollbackLines() == 0 && G.ScrollOffset() == 0 && G.RowText(2).empty() && G.RowText(3) == "5");
}

void Rendition() {
  TerminalGrid G;
  G.Feed("\x1b[1;2;3;4;5;7;8;9;36;45ma"
         "\x1b[22;23;24;25;27;28;29;93;104mb"
         "\x1b[39;49mc\x1b[38;5;8;48:5:232md"
         "\x1b[38;2;1;2;3;48:2::4:5:6me"
         "\x1b[38:2:7:8:9;48;2;10;11;12mf\x1b[mg");
  const auto& A = G.At(0, 0);
  CHECK(A.Bold && A.Dim && A.Italic && A.Underline && A.Blink && A.Reverse && A.Hidden && A.Strike);
  CHECK(A.Fg.Index == 6 && A.Bg.Index == 5);
  const auto& B = G.At(1, 0);
  CHECK(!B.Bold && !B.Dim && !B.Italic && !B.Underline && !B.Blink && !B.Reverse && !B.Hidden && !B.Strike);
  CHECK(B.Fg.Index == 11 && B.Bg.Index == 12);
  CHECK(G.At(2, 0).SameStyle(Cell()));
  CHECK(G.At(3, 0).Fg == CellColor{CellColor::Kind::Ansi, 8});
  CHECK(G.At(3, 0).Bg == CellColor{CellColor::Kind::Rgb, 0, 8, 8, 8});
  CHECK(G.At(4, 0).Fg == CellColor{CellColor::Kind::Rgb, 0, 1, 2, 3});
  CHECK(G.At(4, 0).Bg == CellColor{CellColor::Kind::Rgb, 0, 4, 5, 6});
  CHECK(G.At(5, 0).Fg == CellColor{CellColor::Kind::Rgb, 0, 7, 8, 9});
  CHECK(G.At(5, 0).Bg == CellColor{CellColor::Kind::Rgb, 0, 10, 11, 12});
  CHECK(G.At(6, 0).SameStyle(Cell()));
  G.Feed("\x1b[41;32;7m\x1b[X");
  CHECK(G.At(7, 0).Reverse && G.At(7, 0).Fg.Index == 2);
}

void ModesAndStrings() {
  TerminalGrid G(10, 4);
  G.Feed("main\x1b[3;5H\x1b[31m\x1b" "7\x1b[2;1H\x1b[32m\x1b" "8!");
  CHECK(G.At(4, 2).Ch == U'!' && G.At(4, 2).Fg.Index == 1);
  G.Feed("\x1b[?1048h\x1b[H\x1b[?1048l");
  CHECK(G.CursorCol() == 5 && G.CursorRow() == 2);
  G.Feed("\x1b[?47h\x1b[Halt\x1b[?47l");
  CHECK(!G.AltScreen() && G.RowText(0) == "main");
  G.Feed("\x1b[?1047h");
  CHECK(G.RowText(0) == "alt");
  G.Feed("\x1b[?1047l\x1b[?47h");
  CHECK(G.RowText(0).empty());
  G.Feed("\x1b[?47l\x1b[3;6H\x1b[?1049h\x1b[H\x1b[32m"
         "1\r\n2\r\n3\r\n4\r\n5");
  CHECK(G.AltScreen() && G.ScrollbackLines() == 0);
  G.Resize(7, 3);
  G.Feed("\x1b[?1049l?");
  CHECK(!G.AltScreen() && G.RowText(0) == "main" && G.At(5, 2).Ch == U'?' && G.At(5, 2).Fg.Index == 1);
  G.Feed("\x1b[?1;12;25;2004l\x1b=");
  CHECK(!G.ApplicationCursor() && !G.CursorVisible() && !G.CursorBlinks() && !G.BracketedPaste() && G.ApplicationKeypad());
  G.Feed("\x1b[?1;12;25;2004h\x1b>\x1b[5 q");
  CHECK(G.ApplicationCursor() && G.CursorVisible() && G.CursorBlinks() && G.BracketedPaste() && !G.ApplicationKeypad());
  G.Feed("\x1b[2;3H\x1b[5n\x1b[6n\x1b[c\x1b[>c");
  CHECK(G.TakeReplies() == "\x1b[0n\x1b[2;3R\x1b[?1;2c\x1b[>0;0;0c" && G.TakeReplies().empty());

  G.Feed("\x1b" "c\x1b]0;old\a\x1b]2;new\x1b");
  CHECK(G.Title() == "old");
  G.Feed("\\"
         "\x1b]8;;https://example.invalid\x1b\\"
         "\x1b]52;c;clipboard\a"
         "\x1b]99;garbage\x1b\\"
         "\x1bP$qhidden\x1b");
  G.Feed("\\"
         "\x1b[?9999h"
         "\x1b[1$y"
         "\x1b[99\x18"
         "x\x1b[2" "b");
  CHECK(G.Title() == "new" && G.TitleChanged() && !G.TitleChanged() && !G.TakeBell());
  CHECK(G.RowText(0) == "xxx");
  G.Feed("\x1b" "c");
  CHECK(G.Title().empty() && G.TitleChanged() && !G.ApplicationCursor() && !G.BracketedPaste());
}

void Keys() {
  TerminalGrid G;
  CHECK(EncodeTerminalKey(G, Key::Enter) == "\r" && EncodeTerminalKey(G, Key::Tab) == "\t");
  CHECK(EncodeTerminalKey(G, Key::Escape) == "\x1b" && EncodeTerminalKey(G, Key::Backspace) == "\x7f");
  CHECK(EncodeTerminalKey(G, Key::Text, 0, U'é') == "é" && EncodeTerminalKey(G, Key::Text, Alt, U'x') == "\x1bx");
  CHECK(EncodeTerminalKey(G, Key::Tab, Shift) == "\x1b[Z");
  CHECK(EncodeTerminalKey(G, Key::Keypad2) == "2");
  for (int I = 0; I < 26; ++I)
    CHECK(EncodeTerminalKey(G, Key::Text, Ctrl, U'a' + I) == std::string(1, char(I + 1)));
  for (int I = 0; I < 5; ++I)
    CHECK(EncodeTerminalKey(G, Key::Text, Ctrl, U'[' + I) == std::string(1, char(27 + I)));
  CHECK(EncodeTerminalKey(G, Key::Text, Ctrl | Alt, U'c') == "\x1b\003");
  CHECK(EncodeTerminalKey(G, Key::Text, Ctrl, U'@') == std::string(1, '\0'));
  for (int I = 0; I < 6; ++I) {
    const auto K = Key(int(Key::Up) + I);
    const char Final = "ABCDHF"[I];
    CHECK(EncodeTerminalKey(G, K) == std::string("\x1b[") + Final);
    G.Feed("\x1b[?1h");
    CHECK(EncodeTerminalKey(G, K) == std::string("\x1bO") + Final);
    for (int Mods = 1; Mods <= 7; ++Mods)
      CHECK(EncodeTerminalKey(G, K, Mods) == "\x1b[1;" + std::to_string(1 + Mods) + Final);
    G.Feed("\x1b[?1l");
  }
  CHECK(EncodeTerminalKey(G, Key::End) == "\x1b[F");  // End is not CSI 4 ~
  CHECK(EncodeTerminalKey(G, Key::Insert) == "\x1b[2~" && EncodeTerminalKey(G, Key::Delete) == "\x1b[3~");
  CHECK(EncodeTerminalKey(G, Key::PageUp) == "\x1b[5~" && EncodeTerminalKey(G, Key::PageDown) == "\x1b[6~");
  CHECK(EncodeTerminalKey(G, Key::PageUp, Ctrl) == "\x1b[5;5~");
  // The console key path up to the pty: HID usage (what the USB keyboard reports) -> event -> bytes for the session.
  {
    const auto Bytes = [&](int Usage, bool Shift_ = false, bool Ctrl_ = false) {
      Event E;
      if (!MapHidKeyboard(Usage, Shift_, false, Ctrl_, false, E) || E.Kind != Event::Type::KeyDown)
        return std::string("<unmapped>");
      return EncodeTerminalKey(G, E.KeyboardKey, E.Modifiers, E.Codepoint);
    };
    CHECK(Bytes(hid_usage::PageUp) == "\x1b[5~" && Bytes(hid_usage::PageDown) == "\x1b[6~");
    CHECK(Bytes(hid_usage::Home) == "\x1b[H" && Bytes(hid_usage::End) == "\x1b[F");
    CHECK(Bytes(hid_usage::Insert) == "\x1b[2~" && Bytes(hid_usage::Delete) == "\x1b[3~");
    CHECK(Bytes(hid_usage::Up) == "\x1b[A" && Bytes(hid_usage::Down) == "\x1b[B" && Bytes(hid_usage::Right) == "\x1b[C" && Bytes(hid_usage::Left) == "\x1b[D");
    CHECK(Bytes(hid_usage::PageUp, false, true) == "\x1b[5;5~" && Bytes(hid_usage::F12) == "\x1b[24~");
    CHECK(Bytes(hid_usage::A + 'c' - 'a', false, true) == "\x03" && Bytes(hid_usage::Tab) == "\t" && Bytes(hid_usage::Return) == "\r");
    CHECK(Bytes(hid_usage::A) == "a" && Bytes(hid_usage::A, true) == "A" && Bytes(hid_usage::D1, true) == "!");
  }
  for (int I = 0; I < 4; ++I) {
    CHECK(EncodeTerminalKey(G, Key(int(Key::F1) + I)) == std::string("\x1bO") + "PQRS"[I]);
  }
  constexpr int Numbers[] = {15, 17, 18, 19, 20, 21, 23, 24};
  for (int I = 0; I < 8; ++I) {
    CHECK(EncodeTerminalKey(G, Key(int(Key::F5) + I)) == "\x1b[" + std::to_string(Numbers[I]) + '~');
    CHECK(EncodeTerminalKey(G, Key(int(Key::F5) + I), Alt) == "\x1b[" + std::to_string(Numbers[I]) + ";3~");
  }
  CHECK(EncodeTerminalKey(G, Key::Delete, Ctrl) == "\x1b[3;5~" && EncodeTerminalKey(G, Key::F1, Shift) == "\x1b[1;2P");
  CHECK(EncodeTerminalPaste(G, "a\nb") == "a\nb");
  G.Feed("\x1b[?2004h\x1b=");
  CHECK(EncodeTerminalPaste(G, "a\nb") == "\x1b[200~a\nb\x1b[201~" && EncodeTerminalPaste(G, "").empty());
  CHECK(EncodeTerminalKey(G, Key::Keypad2) == "\x1bOr" && EncodeTerminalKey(G, Key::KeypadEnter) == "\x1bOM");
  G.Feed("\x1b[?2004l\x1b>");
  CHECK(EncodeTerminalPaste(G, "a\nb") == "a\nb" && EncodeTerminalKey(G, Key::KeypadEnter) == "\r");
}

void Replay() {
  constexpr std::string_view Top =
#include "data/TopReplay.inc"
    ;
  TerminalGrid T(80, 24);
  const auto TopExit = Top.find("\x1b[?1l");
  CHECK(TopExit != std::string_view::npos);
  for (char B : Top.substr(0, TopExit))
    T.Feed(std::string_view(&B, 1));
  CHECK(T.RowText(0).starts_with("top - ") && T.RowText(1).starts_with("Tasks:"));
  CHECK(T.RowText(6).find("COMMAND") != std::string::npos && T.RowText(10).find("top") != std::string::npos);
  CHECK(!T.CursorVisible() && T.ApplicationCursor() && T.ApplicationKeypad());
  T.Feed(Top.substr(TopExit));
  CHECK(T.CursorVisible() && !T.ApplicationCursor() && !T.ApplicationKeypad());
  for (int R = 0; R < T.Rows(); ++R)
    CHECK(T.RowText(R).find_first_of("\033\ufffd") == std::string::npos && T.RowText(R).find("[39;49m") == std::string::npos);

  constexpr std::string_view Less =
#include "data/LessReplay.inc"
    ;
  TerminalGrid L(40, 6);
  L.Feed("main prompt");
  const auto Quit = Less.find("\x1b[?1l");
  CHECK(Quit != std::string_view::npos);
  L.Feed(Less.substr(0, Quit));
  CHECK(L.AltScreen() && L.ApplicationCursor() && L.RowText(2) == "First line from less.");
  CHECK(L.RowText(3) == "Second line from less." && L.RowText(4) == "Third line from less.");
  L.Feed(Less.substr(Quit));
  CHECK(!L.AltScreen() && L.RowText(0) == "main prompt" && L.CursorCol() == 11 && L.ScrollbackLines() == 0);
}

} // namespace

int main() {
  Controls();
  Utf8AndCharset();
  CursorEraseAndScroll();
  Rendition();
  ModesAndStrings();
  Keys();
  Replay();
  std::printf("ui-terminal-test: %d checks, %d failures\n", Checks, Failures);
  return Failures != 0;
}
