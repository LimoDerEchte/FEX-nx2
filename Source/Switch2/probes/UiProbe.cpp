// SPDX-License-Identifier: MIT
// EXPERIMENT console probe (not part of the launcher). Replaces the failed SDL2 probe: Vulkan through OunceVK behind the
// platform abstraction. Draws: clear (27,27,31), a sharp rect, a rounded rect, a ring, a filled circle, a 1 du hairline,
// text through the real render/Fonts module (Nunito Sans / JetBrains Mono, HarfBuzz shaping, atlas per size x s), Lucide
// icons through render/Icons, and a readout of output size, s, last button, sticks, touch points, mouse position, buttons and wheel, size changes
// and frame time. Task 3: Minus switches to the component gallery page (screens/gallery.cpp: focus, touch, Joy-Con mouse and
// mouse pointer on the real components); L/R slow down or speed up the Joy-Con pointer. + exits. The main logs every step to the SD log.
#include "UiProbe.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "platform/Log.h"
#include "render/Fonts.h"
#include "render/Icons.h"
#include "screens/gallery.h"
#include "theme.h"

namespace probe {
namespace {

using ui::Color;
using ui::Rect;
using ui::Renderer;
using ui::TextureId;

constexpr Color Background = Color::Rgb(27, 27, 31);
constexpr Color Panel = Color::Rgb(60, 64, 80);
constexpr Color TextColor = Color::Rgb(230, 225, 229);
constexpr Color Accent = Color::Rgb(142, 184, 255);
constexpr Color Warm = Color::Rgb(255, 179, 71);
constexpr Color Divider = Color::Rgb(110, 110, 124);

// ---- Readout state -------------------------------------------------------------------------------------------------

struct Touch {
  uint32_t Id;
  float X, Y;
};

struct State {
  std::string LastButton = "none";
  float Stick[2][2] {};
  std::vector<Touch> Touches;
  float MouseX = 0, MouseY = 0;
  bool MouseSeen = false;
  unsigned MouseButtons = 0;
  float WheelX = 0, WheelY = 0, WheelTotalY = 0;
  int SizeChanges = 0;
  ui::Size LastSize;
  bool Quit = false;
  int Page = 0;  // 0 input readout, 1 component gallery
  int GainSteps = 0;
  bool LoggedStick = false, LoggedMouseMove = false, LoggedTouchMove = false;
};

const char* PhaseName(ui::TouchPhase P) { return P == ui::TouchPhase::Down ? "down" : P == ui::TouchPhase::Up ? "up" : "move"; }

void Handle(const ui::Event& E, State& S) {
  using T = ui::Event::Type;
  switch (E.Kind) {
  case T::ButtonDown:
  case T::ButtonUp: {
    const bool Down = E.Kind == T::ButtonDown;
    S.LastButton = std::string(ui::ButtonName(E.Btn)) + (Down ? " down" : " up") + " (pad " + std::to_string(E.Pad) + ")";
    ui::Log("input: button %s %s pad=%u", ui::ButtonName(E.Btn), Down ? "down" : "up", E.Pad);
    if (Down && E.Btn == ui::Button::Plus)
      S.Quit = true;
    if (Down && E.Btn == ui::Button::Minus) {
      S.Page ^= 1;
      ui::Log("page: %s", S.Page ? "gallery" : "input readout");
    }
    break;
  }
  case T::Stick:
    S.Stick[E.StickIndex & 1][0] = E.X;
    S.Stick[E.StickIndex & 1][1] = E.Y;
    if (!S.LoggedStick && (E.X != 0 || E.Y != 0)) {
      S.LoggedStick = true;
      ui::Log("input: first stick event pad=%u stick=%u x=%.2f y=%.2f", E.Pad, E.StickIndex, E.X, E.Y);
    }
    break;
  case T::PadCancel:
    for (auto& Axes : S.Stick)
      Axes[0] = Axes[1] = 0;
    break;
  case T::Touch: {
    auto It = std::find_if(S.Touches.begin(), S.Touches.end(), [&](const Touch& T2) { return T2.Id == E.TouchId; });
    if (E.Phase == ui::TouchPhase::Up) {
      if (It != S.Touches.end())
        S.Touches.erase(It);
    } else if (It != S.Touches.end()) {
      It->X = E.X;
      It->Y = E.Y;
    } else {
      S.Touches.push_back(Touch {E.TouchId, E.X, E.Y});
    }
    if (E.Phase != ui::TouchPhase::Move)
      ui::Log("input: touch %s id=%u x=%.0f y=%.0f", PhaseName(E.Phase), E.TouchId, E.X, E.Y);
    else if (!S.LoggedTouchMove) {
      S.LoggedTouchMove = true;
      ui::Log("input: first touch move id=%u x=%.0f y=%.0f", E.TouchId, E.X, E.Y);
    }
    break;
  }
  case T::MouseMove:
    S.MouseX = E.X;
    S.MouseY = E.Y;
    S.MouseSeen = true;
    if (!S.LoggedMouseMove) {
      S.LoggedMouseMove = true;
      ui::Log("input: first mouse move x=%.0f y=%.0f dx=%.1f dy=%.1f (output px)", E.X, E.Y, E.DX, E.DY);
    }
    break;
  case T::MouseCancel:
  case T::MouseButton:
    S.MouseSeen = true;
    if (E.Pressed)
      S.MouseButtons |= 1u << unsigned(E.Mouse);
    else
      S.MouseButtons &= ~(1u << unsigned(E.Mouse));
    ui::Log("input: mouse button %s %s", ui::MouseButtonName(E.Mouse), E.Pressed ? "down" : "up");
    break;
  case T::MouseWheel:
    S.MouseSeen = true;
    S.WheelX = E.DX;
    S.WheelY = E.DY;
    S.WheelTotalY += E.DY;
    ui::Log("input: mouse wheel dx=%.0f dy=%.0f (total y %.0f)", E.DX, E.DY, S.WheelTotalY);
    break;
  case T::KeyDown: break;
  case T::SizeChanged:
    ++S.SizeChanges;
    S.LastSize = ui::Size {E.W, E.H};
    ui::Log("input: size changed to %dx%d (s=%.3f)", E.W, E.H, ui::ScaleForHeight(E.H));
    break;
  }
}

std::string Fmt(const char* Format, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* Format, ...) {
  char Buf[256];
  va_list Args;
  va_start(Args, Format);
  std::vsnprintf(Buf, sizeof(Buf), Format, Args);
  va_end(Args);
  return Buf;
}

Rect Du(float S, float X, float Y, float W, float H) { return ui::SnapRect(Rect {X * S, Y * S, W * S, H * S}); }

} // namespace

int Run(ui::Platform& P, const Options& O) {
  ui::Display& D = P.GetDisplay();
  ui::Input& In = P.GetInput();
  ui::System& Sys = P.GetSystem();
  Renderer& R = P.GetRenderer();

  ui::WallTime Wall;
  if (Sys.WallClock(Wall))
    ui::Log("step clock: ok (wall %04d-%02d-%02d %02d:%02d:%02d)", Wall.Year, Wall.Month, Wall.Day, Wall.Hour, Wall.Minute, Wall.Second);
  else
    ui::Log("step clock: wall clock unavailable (monotonic ok: %llu ms)", (unsigned long long)Sys.MonotonicMs());
  ui::Log("step display: %dx%d s=%.3f vsync=%s", D.GetSize().W, D.GetSize().H, D.Scale(), D.Vsync() ? "on" : "off");
  ui::Log("input: mouse %s, wheel %s (as reported before the first sample)", In.HasMouse() ? "connected" : "not seen yet", In.HasWheel() ? "available" : "not seen yet");

  ui::Fonts Fonts(R);
  if (!Fonts.Ok()) {
    ui::Log("FAIL probe init: freetype");
    return 1;
  }
  ui::Icons Icons(R);
  ui::SetTheme(ui::ThemeMode::Dark, ui::AccentPreset::Cyan);
  {
    // HarfBuzz proof (Task 2 console risk): shaped "AV" and "To" are narrower than the sum of their single glyphs (kerning).
    namespace st = ui::styles;
    const float Sc = D.Scale();
    const float Av = Fonts.Measure(st::Body, "AV", Sc), AvSum = Fonts.Measure(st::Body, "A", Sc) + Fonts.Measure(st::Body, "V", Sc);
    const float To = Fonts.Measure(st::Body, "To", Sc), ToSum = Fonts.Measure(st::Body, "T", Sc) + Fonts.Measure(st::Body, "o", Sc);
    ui::Log("step shaping: HarfBuzz kerning %s (\"AV\" %.2f px vs %.2f px summed, \"To\" %.2f vs %.2f)", Av < AvSum - 0.01f && To < ToSum - 0.01f ? "applied: yes" : "NOT applied: NO", double(Av), double(AvSum), double(To), double(ToSum));
  }
  ui::Gallery Gal;
  ui::Log("step gallery: built (Minus toggles the gallery page)");
  ui::Log("step fonts: ok (Nunito Sans 400/600/700/800, JetBrains Mono 400/600/700, Noto Sans 400/500 embedded; FreeType + HarfBuzz)");
  int ScaleHeight = D.GetSize().H;

  State S;
  S.LastSize = D.GetSize();
  uint64_t Start = Sys.MonotonicMs();
  uint64_t LastFrame = Start;
  double AvgMs = 0;
  int Failures = 0, Frame = 0, Presented = 0;
  std::vector<ui::Event> Events;
  ui::Log("step main loop: start");

  while (!S.Quit && (O.MaxFrames == 0 || Frame < O.MaxFrames)) {
    if (O.OnFrame)
      O.OnFrame(Frame, P);
    Events.clear();
    In.Poll(Events);
    for (const ui::Event& E : Events) {
      Handle(E, S);
      if (E.Kind == ui::Event::Type::ButtonDown && (E.Btn == ui::Button::L || E.Btn == ui::Button::R)) {
        In.SetPointerGain(In.PointerGain() * (E.Btn == ui::Button::R ? 1.25f : 0.8f));
        ui::Log("input: pointer gain now %.2f", double(In.PointerGain()));
      }
    }
    ++Frame;
    const uint64_t GNow = O.FixedFrameMs ? 1000 + uint64_t(Frame) * O.FixedFrameMs : Sys.MonotonicMs();
    if (S.Page == 1) {
      Gal.ExtraStatus = "Minus: input page";
      Gal.Update(GNow, D.Scale(), Events);
    }
    if (Frame % 300 == 0 && In.HasMouse())
      ui::Log("input: %s", In.MouseStatus().c_str());

    if (!D.BeginFrame() || !R.BeginFrame(Background)) {
      if (++Failures >= 120) {
        ui::Log("FAIL frame %d: 120 consecutive frames could not begin", Frame);
        return 1;
      }
      continue;
    }
    const ui::Size Sz = R.TargetSize();
    const float s = ui::ScaleForHeight(Sz.H);
    const int Hair = ui::HairlinePx(s);
    if (Sz.H != ScaleHeight) {
      ScaleHeight = Sz.H;
      const ui::FontsStats FS = Fonts.Stats();
      ui::Log("rescale: %dx%d s=%.3f (atlases and icons rebuilt on demand; %d font atlases so far)", Sz.W, Sz.H, s, FS.Atlases);
    }

    if (S.Page == 1) {
      const ui::DrawContext C {R, Fonts, Icons, ui::CurrentTheme(), s};
      R.FillRect(Rect {0, 0, float(Sz.W), float(Sz.H)}, C.T.Bg);
      Gal.Draw(C, GNow);
    } else {
    // Shapes.
    R.FillRect(Du(s, 40, 90, 160, 60), Accent);
    R.FillRoundRect(Du(s, 220, 90, 160, 60), 16 * s, Panel);
    R.Ring(std::round(460 * s), std::round(120 * s), std::round(30 * s), std::round(4 * s), Accent);
    R.FillCircle(std::round(560 * s), std::round(120 * s), std::round(24 * s), Warm);
    R.Line(40 * s, 170 * s, 1240 * s, 170 * s, float(Hair), Divider);
    R.StrokeRoundRect(Du(s, 620, 90, 160, 60), 16 * s, float(Hair * 2), Accent);
    if (!Icons.Draw(ui::IconId::House, Du(s, 40, 190, 48, 48), Accent))
      ++Failures;
    {
      // More Lucide icons at the spec stroke widths, down the left edge.
      const ui::IconId Set[] = {ui::IconId::Settings, ui::IconId::Gamepad, ui::IconId::Monitor, ui::IconId::TerminalSquare,
                                ui::IconId::ChevronRight, ui::IconId::X, ui::IconId::Folder, ui::IconId::Transfer};
      const float Weights[] = {ui::stroke::Gear, ui::stroke::Card, ui::stroke::Card, ui::stroke::Card, ui::stroke::Chevron, ui::stroke::Close, 2.0f, 2.0f};
      for (size_t I = 0; I < sizeof(Set) / sizeof(Set[0]); ++I)
        Icons.Draw(Set[I], Du(s, 46, 252 + 44 * float(I), 36, 36), TextColor, Weights[I]);
    }

    // Text.
    namespace st = ui::styles;
    Fonts.Draw(st::TitleSettings, std::string("FEX Switch 2 - ") + Version, s, std::round(40 * s), std::round(64 * s), TextColor);
    int Line = 0;
    auto Row = [&](const std::string& Str) {
      Fonts.Draw(st::Body, Str, s, std::round(120 * s), std::round((214 + 24 * Line) * s), TextColor);
      ++Line;
    };
    Row(Fmt("Output %dx%d  s=%.3f  hairline=%dpx  vsync=%s", Sz.W, Sz.H, s, Hair, D.Vsync() ? "on" : "off"));
    Row("Last button: " + S.LastButton);
    Row(Fmt("Sticks  L(%.2f, %.2f)  R(%.2f, %.2f)", S.Stick[0][0], S.Stick[0][1], S.Stick[1][0], S.Stick[1][1]));
    std::string TouchLine = Fmt("Touch: %zu", S.Touches.size());
    for (const Touch& T : S.Touches)
      TouchLine += Fmt("  #%u(%.0f,%.0f)", T.Id, T.X, T.Y);
    Row(TouchLine);
    Row(S.MouseSeen || In.HasMouse() ? Fmt("Mouse (%.0f, %.0f)  buttons 0x%02x  wheel last (%.0f,%.0f) total y %.0f  [%s]", S.MouseX, S.MouseY, S.MouseButtons,
                                           S.WheelX, S.WheelY, S.WheelTotalY, In.HasMouse() ? "connected" : "no mouse")
                                     : std::string("Mouse: none seen"));
    {
      const std::string Ms = In.MouseStatus();
      if (!Ms.empty())
        Fonts.Draw(st::Caption, Fonts.Fit(st::Caption, Ms, s, 1100 * s, ui::Ellipsis::End), s, std::round(120 * s), std::round((214 + 24 * Line) * s), Warm);
      ++Line;
    }
    Row(Fmt("Size changes: %d  last %dx%d", S.SizeChanges, S.LastSize.W, S.LastSize.H));
    Row(Fmt("Frame %.1f ms avg  (%d frames)", AvgMs, Frame));
    Row("Press + to exit, - for the component gallery, L/R pointer speed");

    // Text rendering samples (Task 2): weights, kerning and ligatures, mono, tracking, ellipsis, scale-stable widths.
    {
      const float X = std::round(120 * s);
      float Y = 440;
      auto Next = [&]() { return std::round((Y += 26) * s); };
      Fonts.Draw(st::RowLabel, "Nunito Sans 700 row label 18", s, X, Next(), TextColor);
      Fonts.Draw(st::CardLabel, "Card label 22/700", s, X, Next(), TextColor);
      Fonts.Draw(st::Caption, "AV To Ty fi fl ffi - kerning and ligatures, caption 14/400", s, X, Next(), Accent);
      Fonts.Draw(st::Section, "Section header tracking", s, X, Next(), Warm);
      Fonts.Draw(st::MonoUi, "JetBrains Mono ftp://192.168.1.50:5000 {}[]->", s, X, Next(), TextColor);
      Fonts.Draw(st::MonoLog, "12:00:01  ok  mono_log 13/400  line height 1.7", s, X, Next(), Accent);
      Fonts.Draw(st::KdeTab, "Noto Sans 400 shell tab: user@host: ~", s, X, Next(), TextColor);
      const std::string Long = "/switch/fex/rootfs/Ubuntu_24.04_amd64_very_long_image_name.sqsh";
      const float Box = 300 * s;
      const float Base = Next();
      R.StrokeRoundRect(ui::SnapRect(Rect {X - 4 * s, Base - 18 * s, Box + 8 * s, 24 * s}), 4 * s, float(Hair), Divider);
      Fonts.Draw(st::MonoUiSmall, Fonts.Fit(st::MonoUiSmall, Long, s, Box, ui::Ellipsis::Middle), s, X, Base, TextColor);
      const float Base2 = Next();
      R.StrokeRoundRect(ui::SnapRect(Rect {X - 4 * s, Base2 - 18 * s, Box + 8 * s, 24 * s}), 4 * s, float(Hair), Divider);
      Fonts.Draw(st::Body, Fonts.Fit(st::Body, "A very long title that is cut at the end with an ellipsis", s, Box, ui::Ellipsis::End), s, X, Base2, TextColor);
    }

    // Mouse pointer (raw readout page): a cross at the reported position.
    if (S.MouseSeen) {
      R.FillRect(Rect {S.MouseX - 12 * s, S.MouseY - std::round(s), 24 * s, std::max(1.0f, std::round(2 * s))}, Warm);
      R.FillRect(Rect {S.MouseX - std::round(s), S.MouseY - 12 * s, std::max(1.0f, std::round(2 * s)), 24 * s}, Warm);
    }
    // Touch points.
    for (const Touch& T : S.Touches) {
      R.Ring(T.X, T.Y, std::round(28 * s), std::round(3 * s), Warm);
      R.FillCircle(T.X, T.Y, std::round(5 * s), Warm);
    }

    // Time-based animation: a square sliding along the bottom (host screenshots fix the time).
    const uint64_t T = O.FixedFrameMs ? uint64_t(Frame) * O.FixedFrameMs : Sys.MonotonicMs() - Start;
    const float Travel = float(Sz.W) - std::round(80 * s);
    const float Pos = Travel * float(T % 4000) / 4000.0f;
    R.FillRoundRect(ui::SnapRect(Rect {std::round(40 * s) + Pos, std::round(660 * s), std::round(40 * s), std::round(40 * s)}), 8 * s, Accent);

    }  // page 0
    Fonts.EndFrame();
    Icons.EndFrame();
    const bool Ended = R.EndFrame();
    const bool Shown = Ended && D.Present();
    if (!Shown) {
      if (++Failures >= 120) {
        ui::Log("FAIL frame %d: 120 consecutive frames failed (EndFrame=%d)", Frame, int(Ended));
        return 1;
      }
    } else {
      Failures = 0;
      ++Presented;
      if (Presented == 1 || Presented == 2 || Presented == 10 || Presented == 60 || Presented % 600 == 0)
        ui::Log("frame %d presented ok (%.1f ms avg)", Frame, AvgMs);
    }
    const uint64_t Now = Sys.MonotonicMs();
    AvgMs = AvgMs == 0 ? double(Now - LastFrame) : AvgMs * 0.95 + double(Now - LastFrame) * 0.05;
    LastFrame = Now;
  }
  if (S.Quit)
    ui::Log("exit requested (Plus) after %d frames", Frame);
  ui::Log("step main loop: done (%d frames, %d presented, avg %.1f ms)", Frame, Presented, AvgMs);
  return 0;
}

} // namespace probe
