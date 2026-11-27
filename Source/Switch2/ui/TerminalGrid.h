// SPDX-License-Identifier: MIT
// The Shell screen's terminal: a cell grid fed with the guest's output bytes. GuestTerminal.h has no cell grid (it only filters
// DEC private modes for the libnx console and runs the line discipline), so this is the grid. No Vulkan or libnx types here.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace ui {

// A cell color: default, one of the 16 ANSI colors, or 24-bit RGB (the 256-color cube is resolved to RGB).
struct CellColor {
  enum class Kind : uint8_t { Default, Ansi, Rgb, Focus, Inactive } K = Kind::Default;  // Focus/Inactive: theme tokens (banner)
  uint8_t Index = 0;     // Ansi: 0..15
  uint8_t R = 0, G = 0, B = 0;
  bool operator==(const CellColor&) const = default;
};

struct Cell {
  char32_t Ch = U' ';
  CellColor Fg, Bg;
  bool Bold = false, Reverse = false, Underline = false;
  bool Dim = false, Italic = false, Blink = false, Hidden = false, Strike = false;
  uint8_t Width = 1;  // 2 for a wide glyph, 0 for its trailing cell
  bool SameStyle(const Cell& O) const {
    return Fg == O.Fg && Bg == O.Bg && Bold == O.Bold && Reverse == O.Reverse && Underline == O.Underline &&
           Dim == O.Dim && Italic == O.Italic && Blink == O.Blink && Hidden == O.Hidden && Strike == O.Strike;
  }
};

void AppendTerminalUtf8(std::string& Out, char32_t Ch);

class TerminalGrid {
public:
  static constexpr size_t MaxScrollback = 2000;

  TerminalGrid(int Cols = 80, int Rows = 24) { Resize(Cols, Rows); }
  void Resize(int Cols, int Rows);
  void Feed(std::string_view Bytes);
  void Reset();
  // Writes a line with the given foreground (and bold) and starts the next one: the session banner.
  void PrintStyled(std::string_view Text, CellColor Fg, bool Bold);

  int Cols() const { return W; }
  int Rows() const { return H; }
  const Cell& At(int Col, int Row) const { return Screen[size_t(Row) * size_t(W) + size_t(Col)]; }
  int CursorCol() const { return Cx; }
  int CursorRow() const { return Cy; }
  bool CursorVisible() const { return ShowCursor; }
  bool CursorBlinks() const { return BlinkCursor; }
  bool ApplicationCursor() const { return AppCursor; }
  bool ApplicationKeypad() const { return AppKeypad; }
  bool BracketedPaste() const { return BracketPaste; }
  const std::string& Title() const { return TitleText; }
  bool TitleChanged();
  bool TakeBell();
  std::string TakeReplies();
  bool AltScreen() const { return Alt; }

  // Scrollback (the primary screen only). Offset 0 = live; Offset N shows N lines above.
  size_t ScrollbackLines() const { return Back.size(); }
  int ScrollOffset() const { return View; }
  void ScrollView(int Lines);          // positive = further back
  void ScrollToLive() { View = 0; }
  // The cell shown at (Col, Row) of the view (scrollback lines above the live screen when Offset > 0).
  const Cell& ViewAt(int Col, int Row) const;
  // The text of a row (live screen), trailing blanks removed; for tests.
  std::string RowText(int Row) const;

private:
  enum class State : uint8_t { Ground, Esc, EscIntermediate, Csi, Osc, String, OscEsc, StringEsc };
  struct SavedCursor {
    int X = 0, Y = 0;
    Cell Pen;
    bool Pending = false, Wrap = true, Graphics = false;
  };
  void Put(char32_t C);
  void Decode(unsigned char C);
  void Execute(unsigned char C);
  void CsiDispatch(unsigned char Final);
  void EscDispatch(unsigned char C);
  void OscDispatch();
  void Sgr();
  void LineFeed();
  void ReverseIndex();
  void ScrollUp(int N, bool History = true);
  void ScrollDown(int N);
  void BreakWide(int Row, int Col);
  void EraseCells(int Row, int From, int To);
  Cell Blank() const;
  Cell& Ref(int Col, int Row) { return Screen[size_t(Row) * size_t(W) + size_t(Col)]; }
  int Param(size_t I, int Default) const { return I < Params.size() && Params[I] >= 0 ? Params[I] : Default; }
  void SetMode(bool On);
  void EnterAlt(bool On);
  void SaveCursor();
  void RestoreCursor();

  int W = 0, H = 0, Cx = 0, Cy = 0;
  bool WrapPending = false, ShowCursor = true, BlinkCursor = true, Alt = false;
  bool AutoWrap = true, AppCursor = false, AppKeypad = false, BracketPaste = false;
  bool LineDrawing = false, NewlineMode = false;
  int Top = 0, Bottom = 0;  // scroll region rows
  SavedCursor Saved[2];
  std::vector<Cell> Screen, OtherScreen;
  std::deque<std::vector<Cell>> Back;
  std::vector<bool> Tabs;
  int View = 0;
  Cell Pen;  // the current attributes (Ch unused)
  char32_t LastPrinted = 0;
  State St = State::Ground;
  unsigned char EscIntermediate = 0;
  std::vector<int> Params;
  std::string Separators, CsiPrefix, OscText;
  bool CsiInvalid = false;
  int Utf8Left = 0;
  unsigned char Utf8Low = 0x80, Utf8High = 0xBF;
  char32_t Utf8Acc = 0;
  std::string TitleText, Replies;
  bool TitleDirty = false, Bell = false;
};

// The grid for an area: floor(width / advance) x floor(height / line), all in device pixels. At least 1x1.
struct TerminalSize {
  int Cols = 1, Rows = 1;
};
TerminalSize ComputeTerminalSize(float AreaW, float AreaH, float AdvancePx, float LinePx);

// The ANSI color index to RGB (Breeze-like terminal palette lives in the theme; this is the xterm 256 cube and grays for 16..255).
void Xterm256(int N, uint8_t& R, uint8_t& G, uint8_t& B);

} // namespace ui
