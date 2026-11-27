// SPDX-License-Identifier: MIT
#include "TerminalGrid.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ui {

TerminalSize ComputeTerminalSize(float AreaW, float AreaH, float Advance, float Line) {
  TerminalSize S;
  if (Advance > 0 && Line > 0) {
    S.Cols = std::max(1, int(std::floor(AreaW / Advance + 1e-4f)));
    S.Rows = std::max(1, int(std::floor(AreaH / Line + 1e-4f)));
  }
  return S;
}

void Xterm256(int N, uint8_t& R, uint8_t& G, uint8_t& B) {
  if (N >= 232) {
    R = G = B = uint8_t(8 + (N - 232) * 10);
  } else if (N >= 16) {
    const int I = N - 16;
    auto L = [](int V) { return uint8_t(V == 0 ? 0 : 55 + V * 40); };
    R = L(I / 36);
    G = L((I / 6) % 6);
    B = L(I % 6);
  } else {
    R = G = B = 0;
  }
}

namespace {

constexpr struct { char32_t First, Last; } Wide[] = {
#include "TerminalWide.inc"
};

bool IsWide(char32_t C) {
  if (C < Wide[0].First)
    return false;
  const auto It = std::lower_bound(std::begin(Wide), std::end(Wide), C, [](const auto& Range, char32_t Value) {
    return Range.Last < Value;
  });
  return It != std::end(Wide) && C >= It->First;
}

} // namespace

Cell TerminalGrid::Blank() const {
  Cell C;
  // Erased cells take the current effective background.
  if (Pen.Reverse) {
    C.Fg = Pen.Fg;
    C.Reverse = true;
    C.Bold = Pen.Bold;
  } else {
    C.Bg = Pen.Bg;
  }
  return C;
}

void TerminalGrid::Reset() {
  const int Cols = W, Rows = H;
  const bool HadTitle = TitleDirty || !TitleText.empty();
  // Replies already generated in this read must still reach the guest.
  auto Pending = TakeReplies();
  *this = TerminalGrid(Cols, Rows);
  Replies = std::move(Pending);
  TitleDirty = HadTitle;
}

bool TerminalGrid::TitleChanged() { return std::exchange(TitleDirty, false); }
bool TerminalGrid::TakeBell() { return std::exchange(Bell, false); }
std::string TerminalGrid::TakeReplies() { return std::exchange(Replies, {}); }

void TerminalGrid::PrintStyled(std::string_view Text, CellColor Fg, bool Bold) {
  const Cell SavedPen = Pen;
  Pen.Fg = Fg;
  Pen.Bold = Bold;
  Feed(Text);
  Pen = SavedPen;
  Feed("\r\n");
}

void TerminalGrid::Resize(int Cols, int Rows) {
  Cols = std::max(1, Cols);
  Rows = std::max(1, Rows);
  if (Cols == W && Rows == H)
    return;
  const int Drop = std::max(0, Cy - Rows + 1);
  auto Fit = [&](std::vector<Cell>& Buffer, int Skip) {
    std::vector<Cell> New(size_t(Cols) * size_t(Rows));
    if (!Buffer.empty()) {
      for (int R = Skip; R < std::min(H, Skip + Rows); ++R) {
        auto Dst = New.begin() + (R - Skip) * Cols;
        std::copy_n(Buffer.begin() + R * W, std::min(W, Cols), Dst);
        if (Dst[Cols - 1].Width == 2)
          Dst[Cols - 1] = Cell();
      }
    }
    Buffer = std::move(New);
  };
  if (!Alt) {
    for (int R = 0; R < Drop; ++R) {
      Back.emplace_back(Screen.begin() + R * W, Screen.begin() + (R + 1) * W);
      if (Back.size() > MaxScrollback)
        Back.pop_front();
    }
  }
  Fit(Screen, Drop);
  if (!OtherScreen.empty())
    Fit(OtherScreen, 0);
  Saved[Alt].Y = std::max(0, Saved[Alt].Y - Drop);
  for (auto& Cursor : Saved) {
    Cursor.X = std::min(Cursor.X, Cols - 1);
    Cursor.Y = std::min(Cursor.Y, Rows - 1);
    Cursor.Pending = false;
  }
  const int OldCols = int(Tabs.size());
  if (Cols > OldCols) {
    Tabs.resize(Cols);
    for (int C = OldCols; C < Cols; ++C)
      Tabs[C] = C > 0 && C % 8 == 0;
  }
  W = Cols;
  H = Rows;
  Cx = std::min(Cx, W - 1);
  Cy -= Drop;
  Top = 0;
  Bottom = H - 1;
  WrapPending = false;
  View = 0;
}

void TerminalGrid::ScrollView(int Lines) {
  if (Alt)
    return;
  View = std::clamp(View + Lines, 0, int(Back.size()));
}

const Cell& TerminalGrid::ViewAt(int Col, int Row) const {
  if (View == 0)
    return At(Col, Row);
  const int Abs = int(Back.size()) - View + Row;  // index into Back ++ Screen
  if (Abs < 0)
    return At(Col, 0);
  if (Abs < int(Back.size())) {
    const std::vector<Cell>& L = Back[size_t(Abs)];
    static const Cell Empty;
    return Col < int(L.size()) && !(Col == W - 1 && L[size_t(Col)].Width == 2) ? L[size_t(Col)] : Empty;
  }
  return At(Col, Abs - int(Back.size()));
}

void AppendTerminalUtf8(std::string& Out, char32_t Ch) {
  if (Ch < 0x80) {
    Out += char(Ch);
  } else if (Ch < 0x800) {
    Out += char(0xC0 | (Ch >> 6));
    Out += char(0x80 | (Ch & 0x3F));
  } else if (Ch < 0x10000) {
    Out += char(0xE0 | (Ch >> 12));
    Out += char(0x80 | ((Ch >> 6) & 0x3F));
    Out += char(0x80 | (Ch & 0x3F));
  } else {
    Out += char(0xF0 | (Ch >> 18));
    Out += char(0x80 | ((Ch >> 12) & 0x3F));
    Out += char(0x80 | ((Ch >> 6) & 0x3F));
    Out += char(0x80 | (Ch & 0x3F));
  }
}

std::string TerminalGrid::RowText(int Row) const {
  std::string Out;
  int Last = W - 1;
  while (Last >= 0 && At(Last, Row).Ch == U' ')
    --Last;
  for (int C = 0; C <= Last; ++C)
    if (At(C, Row).Width)
      AppendTerminalUtf8(Out, At(C, Row).Ch);
  return Out;
}

void TerminalGrid::BreakWide(int Row, int Col) {
  // Break a double-width glyph that spans the boundary immediately before Col.
  if (Col > 0 && Col < W && Ref(Col, Row).Width == 0) {
    Ref(Col - 1, Row) = Blank();
    Ref(Col, Row) = Blank();
  }
}

void TerminalGrid::EraseCells(int Row, int From, int To) {
  if (From > To)
    return;
  To = std::min(W - 1, To);
  BreakWide(Row, From);
  BreakWide(Row, To + 1);
  std::fill_n(Screen.begin() + Row * W + From, To - From + 1, Blank());
}

void TerminalGrid::ScrollUp(int N, bool History) {
  N = std::min(N, Bottom - Top + 1);
  if (!Alt && History && Top == 0) {
    for (int R = 0; R < N; ++R) {
      Back.emplace_back(Screen.begin() + R * W, Screen.begin() + (R + 1) * W);
      if (Back.size() > MaxScrollback)
        Back.pop_front();
    }
    if (View > 0)
      View = std::min(View + N, int(Back.size()));  // the reader keeps their place
  }
  std::copy(Screen.begin() + (Top + N) * W, Screen.begin() + (Bottom + 1) * W, Screen.begin() + Top * W);
  std::fill_n(Screen.begin() + (Bottom - N + 1) * W, N * W, Blank());
}

void TerminalGrid::ScrollDown(int N) {
  N = std::min(N, Bottom - Top + 1);
  std::copy_backward(Screen.begin() + Top * W, Screen.begin() + (Bottom - N + 1) * W, Screen.begin() + (Bottom + 1) * W);
  std::fill_n(Screen.begin() + Top * W, N * W, Blank());
}

void TerminalGrid::LineFeed() {
  if (Cy == Bottom)
    ScrollUp(1);
  else if (Cy < H - 1)
    ++Cy;
  WrapPending = false;
}

void TerminalGrid::ReverseIndex() {
  if (Cy == Top)
    ScrollDown(1);
  else if (Cy > 0)
    --Cy;
  WrapPending = false;
}

void TerminalGrid::Put(char32_t C) {
  int Width = IsWide(C) ? 2 : 1;
  if (Width > W)
    return;
  if (AutoWrap && (WrapPending || Cx + Width > W)) {
    Cx = 0;
    LineFeed();
  }
  if (Cx + Width > W)
    return;
  BreakWide(Cy, Cx);
  BreakWide(Cy, Cx + Width);
  Cell& Dst = Ref(Cx, Cy);
  Dst = Pen;
  Dst.Ch = C;
  Dst.Width = Width;
  if (Width == 2) {
    Cell& Tail = Ref(Cx + 1, Cy);
    Tail = Pen;
    Tail.Width = 0;
  }
  Cx += Width;
  WrapPending = AutoWrap && Cx == W;
  Cx = std::min(Cx, W - 1);
  LastPrinted = C;
  if (View > 0)
    View = 0;  // output brings the reader back to the live screen
}

void TerminalGrid::Execute(unsigned char C) {
  switch (C) {
  case '\n': case '\v': case '\f':
    if (NewlineMode)
      Cx = 0;
    LineFeed();
    break;
  case '\r': Cx = 0; WrapPending = false; break;
  case '\b': if (Cx > 0) --Cx; WrapPending = false; break;
  case '\t':
    while (Cx < W - 1 && !Tabs[++Cx]) {}
    WrapPending = false;
    break;
  case '\a': Bell = true; break;
  default: break;
  }
}

void TerminalGrid::Sgr() {
  auto Ansi = [](int Index) { return CellColor {CellColor::Kind::Ansi, uint8_t(Index)}; };
  for (size_t I = 0; I < Params.size(); ++I) {
    const int P = Param(I, 0);
    size_t End = I;
    while (End < Separators.size() && Separators[End] == ':')
      ++End;
    const bool Colon = End > I;
    switch (P) {
    case 0: Pen = Cell(); break;
    case 1: Pen.Bold = true; break;
    case 2: Pen.Dim = true; break;
    case 3: Pen.Italic = true; break;
    case 4: Pen.Underline = !Colon || Param(I + 1, 1) != 0; break;
    case 5: case 6: Pen.Blink = true; break;
    case 7: Pen.Reverse = true; break;
    case 8: Pen.Hidden = true; break;
    case 9: Pen.Strike = true; break;
    case 22: Pen.Bold = Pen.Dim = false; break;
    case 23: Pen.Italic = false; break;
    case 24: Pen.Underline = false; break;
    case 25: Pen.Blink = false; break;
    case 27: Pen.Reverse = false; break;
    case 28: Pen.Hidden = false; break;
    case 29: Pen.Strike = false; break;
    case 39: Pen.Fg = CellColor(); break;
    case 49: Pen.Bg = CellColor(); break;
    case 38: case 48: {
      const int Mode = Param(I + 1, 0);
      if (!Colon) {
        if (Mode != 2 && Mode != 5)
          return;
        End = I + (Mode == 5 ? 2 : 4);
        if (End >= Params.size())
          return;
      }
      CellColor Color;
      if (Mode == 5 && End == I + 2 && Params[End] >= 0) {
        const int N = std::min(255, Params[End]);
        if (N < 16) {
          Color = Ansi(N);
        } else {
          Color.K = CellColor::Kind::Rgb;
          Xterm256(N, Color.R, Color.G, Color.B);
        }
      } else if (Mode == 2 && (End == I + 4 || (Colon && End == I + 5)) &&
                 Params[End - 2] >= 0 && Params[End - 1] >= 0 && Params[End] >= 0) {
        Color = {CellColor::Kind::Rgb, 0, uint8_t(std::min(255, Params[End - 2])),
                 uint8_t(std::min(255, Params[End - 1])), uint8_t(std::min(255, Params[End]))};
      }
      if (Color.K != CellColor::Kind::Default)
        (P == 38 ? Pen.Fg : Pen.Bg) = Color;
      break;
    }
    default:
      if (P >= 30 && P <= 37) Pen.Fg = Ansi(P - 30);
      if (P >= 40 && P <= 47) Pen.Bg = Ansi(P - 40);
      if (P >= 90 && P <= 97) Pen.Fg = Ansi(P - 90 + 8);
      if (P >= 100 && P <= 107) Pen.Bg = Ansi(P - 100 + 8);
      break;
    }
    I = End;
  }
}

void TerminalGrid::SaveCursor() {
  Saved[Alt] = {Cx, Cy, Pen, WrapPending, AutoWrap, LineDrawing};
}

void TerminalGrid::RestoreCursor() {
  const auto& S = Saved[Alt];
  Cx = S.X;
  Cy = S.Y;
  Pen = S.Pen;
  AutoWrap = S.Wrap;
  WrapPending = S.Pending;
  LineDrawing = S.Graphics;
}

void TerminalGrid::EnterAlt(bool On) {
  if (Alt == On)
    return;
  if (OtherScreen.empty())
    OtherScreen.resize(Screen.size());
  Screen.swap(OtherScreen);
  Alt = On;
  View = 0;
  WrapPending = false;
}

void TerminalGrid::SetMode(bool On) {
  for (int P : Params) {
    if (CsiPrefix.empty() && P == 20)
      NewlineMode = On;
    if (CsiPrefix != "?")
      continue;
    switch (P) {
    case 1: AppCursor = On; break;
    case 7: AutoWrap = On; WrapPending = false; break;
    case 12: BlinkCursor = On; break;
    case 25: ShowCursor = On; break;
    case 47: EnterAlt(On); break;
    case 66: AppKeypad = On; break;
    case 1047:
      if (!On && Alt)
        std::fill(Screen.begin(), Screen.end(), Cell());
      EnterAlt(On);
      break;
    case 1048: On ? SaveCursor() : RestoreCursor(); break;
    case 1049:
      if (On && !Alt) {
        SaveCursor();
        EnterAlt(true);
        std::fill(Screen.begin(), Screen.end(), Cell());
      } else if (!On && Alt) {
        EnterAlt(false);
        RestoreCursor();
      }
      break;
    case 2004: BracketPaste = On; break;
    default: break;
    }
  }
}

void TerminalGrid::CsiDispatch(unsigned char F) {
  if (F == 'h' || F == 'l') {
    SetMode(F == 'h');
    return;
  }
  if (F == 'c' && Param(0, 0) == 0) {
    if (CsiPrefix.empty())
      Replies += "\x1b[?1;2c";
    else if (CsiPrefix == ">")
      Replies += "\x1b[>0;0;0c";
    return;
  }
  if (CsiPrefix == " " && F == 'q')
    return;  // DECSCUSR accepted; the launcher uses a block cursor.
  if (!CsiPrefix.empty() || (F != 'm' && Separators.find(':') != std::string::npos))
    return;

  const int N = std::max(1, Param(0, 1));
  const int MinY = Cy >= Top ? Top : 0;
  const int MaxY = Cy <= Bottom ? Bottom : H - 1;
  switch (F) {
  case 'A': Cy = std::max(MinY, Cy - N); break;
  case 'B': case 'e': Cy = std::min(MaxY, Cy + N); break;
  case 'C': case 'a': Cx = std::min(W - 1, Cx + N); break;
  case 'D': Cx = std::max(0, Cx - N); break;
  case 'E': Cx = 0; Cy = std::min(MaxY, Cy + N); break;
  case 'F': Cx = 0; Cy = std::max(MinY, Cy - N); break;
  case 'G': case '`': Cx = std::min(W, N) - 1; break;
  case 'd': Cy = std::min(H, N) - 1; break;
  case 'H': case 'f':
    Cy = std::min(H, N) - 1;
    Cx = std::clamp(Param(1, 1), 1, W) - 1;
    break;
  case 'J':
    switch (Param(0, 0)) {
    case 0:
      EraseCells(Cy, Cx, W - 1);
      for (int R = Cy + 1; R < H; ++R)
        EraseCells(R, 0, W - 1);
      break;
    case 1:
      for (int R = 0; R < Cy; ++R)
        EraseCells(R, 0, W - 1);
      EraseCells(Cy, 0, Cx);
      break;
    case 2:
      std::fill(Screen.begin(), Screen.end(), Blank());
      break;
    case 3:
      Back.clear();
      View = 0;
      return;
    default: return;
    }
    break;
  case 'K':
    switch (Param(0, 0)) {
    case 0: EraseCells(Cy, Cx, W - 1); break;
    case 1: EraseCells(Cy, 0, Cx); break;
    case 2: EraseCells(Cy, 0, W - 1); break;
    default: return;
    }
    break;
  case 'L': case 'M':
    if (Cy >= Top && Cy <= Bottom) {
      const int SavedTop = Top;
      Top = Cy;
      if (F == 'L')
        ScrollDown(N);
      else
        ScrollUp(N, false);
      Top = SavedTop;
    }
    break;
  case '@': {
    const int Count = std::min(N, W - Cx);
    BreakWide(Cy, Cx);
    BreakWide(Cy, W - Count);
    auto Row = Screen.begin() + Cy * W;
    std::copy_backward(Row + Cx, Row + W - Count, Row + W);
    std::fill_n(Row + Cx, Count, Blank());
    break;
  }
  case 'P': {
    const int Count = std::min(N, W - Cx);
    BreakWide(Cy, Cx);
    BreakWide(Cy, Cx + Count);
    auto Row = Screen.begin() + Cy * W;
    std::copy(Row + Cx + Count, Row + W, Row + Cx);
    std::fill_n(Row + W - Count, Count, Blank());
    break;
  }
  case 'X': EraseCells(Cy, Cx, Cx + N - 1); break;
  case 'S': ScrollUp(N); return;
  case 'T': if (Params.size() == 1) ScrollDown(N); return;
  case 'm': Sgr(); return;
  case 'r': {
    const int T = N - 1, B = std::min(H, std::max(1, Param(1, H))) - 1;
    if (T >= B && H != 1)
      return;
    Top = H == 1 ? 0 : T;
    Bottom = B;
    Cx = Cy = 0;
    break;
  }
  case 'g':
    if (Param(0, 0) == 0)
      Tabs[Cx] = false;
    else if (Param(0, 0) == 3)
      std::fill(Tabs.begin(), Tabs.end(), false);
    return;
  case 's': SaveCursor(); return;
  case 'u': RestoreCursor(); return;
  case 'b':
    if (LastPrinted)
      for (int I = 0; I < N; ++I)
        Put(LastPrinted);
    return;
  case 'n':
    if (N == 5)
      Replies += "\x1b[0n";
    else if (N == 6)
      Replies += "\x1b[" + std::to_string(Cy + 1) + ";" + std::to_string(Cx + 1) + "R";
    return;
  default: return;
  }
  WrapPending = false;
}

void TerminalGrid::EscDispatch(unsigned char C) {
  switch (C) {
  case '7': SaveCursor(); break;
  case '8': RestoreCursor(); break;
  case 'M': ReverseIndex(); break;
  case 'D': LineFeed(); break;
  case 'E': Cx = 0; LineFeed(); break;
  case 'H': Tabs[Cx] = true; break;
  case 'c': Reset(); break;
  case '=': AppKeypad = true; break;
  case '>': AppKeypad = false; break;
  default: break;
  }
}

void TerminalGrid::OscDispatch() {
  const size_t Semi = OscText.find(';');
  if (Semi == std::string::npos)
    return;
  const std::string_view Code(OscText.data(), Semi);
  if (Code == "0" || Code == "2") {
    // Keep control bytes out of the title. The renderer handles printable UTF-8.
    std::string T;
    for (size_t I = Semi + 1; I < OscText.size() && T.size() < 200; ++I)
      if (auto C = static_cast<unsigned char>(OscText[I]); C >= 0x20 && C != 0x7F)
        T += char(C);
    if (T != TitleText) {
      TitleText = std::move(T);
      TitleDirty = true;
    }
  }
}

void TerminalGrid::Decode(unsigned char B) {
  if (Utf8Left) {
    if (B >= Utf8Low && B <= Utf8High) {
      Utf8Acc = (Utf8Acc << 6) | (B & 0x3F);
      Utf8Low = 0x80;
      Utf8High = 0xBF;
      if (--Utf8Left == 0)
        Put(Utf8Acc);
      return;
    }
    Utf8Left = 0;
    Put(U'\uFFFD');
  }
  if (B == 0x1B) {
    St = State::Esc;
  } else if (B < 0x20) {
    Execute(B);
  } else if (B < 0x7F) {
    constexpr std::u32string_view Graphics = U"◆▒␉␌␍␊°±␤␋┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│≤≥π≠£·";
    Put(LineDrawing && B >= '`' ? Graphics[B - '`'] : char32_t(B));
  } else if (B >= 0xC2 && B <= 0xF4) {
    Utf8Left = B < 0xE0 ? 1 : B < 0xF0 ? 2 : 3;
    Utf8Acc = B & (0x7F >> (Utf8Left + 1));
    Utf8Low = B == 0xE0 ? 0xA0 : B == 0xF0 ? 0x90 : 0x80;
    Utf8High = B == 0xED ? 0x9F : B == 0xF4 ? 0x8F : 0xBF;
  } else if (B != 0x7F) {
    Put(U'\uFFFD');
  }
}

void TerminalGrid::Feed(std::string_view Bytes) {
  for (unsigned char B : Bytes) {
    if (St == State::Ground) {
      Decode(B);
      continue;
    }
    if (B == 0x18 || B == 0x1A) {  // CAN, SUB
      St = State::Ground;
      continue;
    }
    if (B == 0x7F)
      continue;
    switch (St) {
    case State::Ground: break;
    case State::Osc: case State::String:
      if (B == 0x1B)
        St = St == State::Osc ? State::OscEsc : State::StringEsc;
      else if (St == State::Osc && B == '\a') {
        OscDispatch();
        St = State::Ground;
      } else if (St == State::Osc && OscText.size() < 1024) {
        OscText += char(B);
      }
      break;
    case State::OscEsc: case State::StringEsc:
      if (B == '\\') {
        if (St == State::OscEsc)
          OscDispatch();
        St = State::Ground;
        break;
      }
      St = State::Esc;
      [[fallthrough]];
    case State::Esc:
      if (B == '[') {
        Params.assign(1, -1);
        Separators.clear();
        CsiPrefix.clear();
        CsiInvalid = false;
        St = State::Csi;
      } else if (B == ']') {
        OscText.clear();
        St = State::Osc;
      } else if (B == 'P' || B == 'X' || B == '^' || B == '_') {
        St = State::String;
      } else if (B >= 0x20 && B <= 0x2F) {
        EscIntermediate = B;
        St = State::EscIntermediate;
      } else if (B != 0x1B && B >= 0x30) {
        St = State::Ground;
        EscDispatch(B);
      } else if (B < 0x20 && B != 0x1B) {
        Execute(B);
      }
      break;
    case State::EscIntermediate:
      if (B == 0x1B) {
        St = State::Esc;
      } else if (B < 0x20) {
        Execute(B);
      } else if (B <= 0x2F) {
        EscIntermediate = 0;
      } else {
        if (EscIntermediate == '(' && (B == '0' || B == 'B'))
          LineDrawing = B == '0';
        St = State::Ground;
      }
      break;
    case State::Csi:
      if (B == 0x1B) {
        St = State::Esc;
      } else if (B < 0x20) {
        Execute(B);
      } else if (B >= 0x40 && B <= 0x7E) {
        St = State::Ground;
        if (!CsiInvalid)
          CsiDispatch(B);
      } else if (CsiInvalid) {
        break;
      } else if ((B >= 0x20 && B <= 0x2F) || (B >= 0x3C && B <= 0x3F)) {
        if (CsiPrefix.size() < 4 && (B <= 0x2F || (Params[0] < 0 && Params.size() == 1 && CsiPrefix.empty())))
          CsiPrefix += char(B);
        else
          CsiInvalid = true;
      } else if (!CsiPrefix.empty() && CsiPrefix.back() <= 0x2F) {
        CsiInvalid = true;
      } else if (B >= '0' && B <= '9') {
        int& P = Params.back();
        P = std::min(100000, std::max(0, P) * 10 + (B - '0'));
      } else if ((B == ';' || B == ':') && Params.size() < 64) {
        Separators += char(B);
        Params.push_back(-1);
      } else {
        CsiInvalid = true;
      }
      break;
    }
  }
}

} // namespace ui
