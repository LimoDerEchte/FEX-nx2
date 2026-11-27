// SPDX-License-Identifier: MIT
// Host tests for Task 2 (make check-ui): theme tokens, text (measure, ellipsis, atlases, uploads), icons and pixel snapping.
// Fake backends test logic without a GPU; the host headless Vulkan backend tests pixels. `--dump-theme` prints every theme
// value for tests/CheckTheme.py, which compares them with the theming spec.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "DrawContext.h"
#include "platform/Log.h"
#include "platform/fake/FakePlatform.h"
#include "platform/host/HostPlatform.h"
#include "render/Fonts.h"
#include "render/Icons.h"
#include "theme.h"

namespace {
int Failures = 0, Checks = 0;

#define CHECK(Cond)                                                          \
  do {                                                                       \
    ++Checks;                                                                \
    if (!(Cond)) {                                                           \
      ++Failures;                                                            \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond);            \
    }                                                                        \
  } while (0)

using namespace ui;

const float Scales[] = {1.0f, 1.5f, 2.0f, 3.0f};

int CountCalls(const FakeRenderer& R, const char* Prefix) {
  int N = 0;
  for (const std::string& C : R.Calls)
    if (C.rfind(Prefix, 0) == 0)
      ++N;
  return N;
}

// ---- Theme -----------------------------------------------------------------------------------------------------------

void DumpTheme() {
  for (ThemeMode M : {ThemeMode::Dark, ThemeMode::Light}) {
    const char* Mode = M == ThemeMode::Dark ? "dark" : "light";
    const Theme& T = GetTheme(M);
    auto Line = [&](const char* Name, Color C) { std::printf("%s %s %02X%02X%02X %d\n", Mode, Name, C.R, C.G, C.B, C.A); };
#define UI_DUMP(Field, Name) Line(#Name, T.Field);
    UI_THEME_COLORS(UI_DUMP)
#undef UI_DUMP
    for (size_t P = 0; P < size_t(AccentPreset::Count); ++P) {
      std::string Name = std::string("preset_") + AccentName(AccentPreset(P));
      Line(Name.c_str(), T.AccentPresets[P]);
    }
  }
}

void TestThemeCompleteness() {
  for (ThemeMode M : {ThemeMode::Dark, ThemeMode::Light})
    for (size_t P = 0; P < size_t(AccentPreset::Count); ++P) {
      const Theme& T = GetTheme(M, AccentPreset(P));
      CHECK(T.Mode == M && T.Preset == AccentPreset(P));
#define UI_SET(Field, Name) CHECK(!(T.Field.R == UnsetColor.R && T.Field.G == UnsetColor.G && T.Field.B == UnsetColor.B && T.Field.A == UnsetColor.A));
      UI_THEME_COLORS(UI_SET)
#undef UI_SET
      for (const Color& C : T.AccentPresets)
        CHECK(!(C.R == UnsetColor.R && C.G == UnsetColor.G && C.B == UnsetColor.B && C.A == UnsetColor.A));
      // The accent token is the preset's value; the other tokens do not change with the accent.
      CHECK(T.Accent.R == T.AccentPresets[P].R && T.Accent.G == T.AccentPresets[P].G && T.Accent.B == T.AccentPresets[P].B);
      CHECK(T.Bg.R == GetTheme(M).Bg.R && T.Surface.R == GetTheme(M).Surface.R && T.Ok.G == GetTheme(M).Ok.G);
    }
  // Presets are four distinct colors per theme, and light differs from dark.
  for (ThemeMode M : {ThemeMode::Dark, ThemeMode::Light}) {
    const Theme& T = GetTheme(M);
    for (size_t A = 0; A < 4; ++A)
      for (size_t B = A + 1; B < 4; ++B)
        CHECK(T.AccentPresets[A].R != T.AccentPresets[B].R || T.AccentPresets[A].G != T.AccentPresets[B].G || T.AccentPresets[A].B != T.AccentPresets[B].B);
  }
  CHECK(GetTheme(ThemeMode::Dark).Bg.R != GetTheme(ThemeMode::Light).Bg.R);
  CHECK(GetTheme(ThemeMode::Dark).Scrim.A == 158 && GetTheme(ThemeMode::Light).Scrim.A == 89);  // 62%, 35%
  // Names round trip; the stored form is the preset name, not a hex.
  for (size_t P = 0; P < size_t(AccentPreset::Count); ++P) {
    AccentPreset Out = AccentPreset::Count;
    CHECK(ParseAccent(AccentName(AccentPreset(P)), Out) && Out == AccentPreset(P));
  }
  AccentPreset Dummy;
  CHECK(!ParseAccent("00C3E3", Dummy) && !ParseAccent(nullptr, Dummy));
  // CurrentTheme follows SetTheme and defaults to dark.
  g_theme = nullptr;
  CHECK(CurrentTheme().Mode == ThemeMode::Dark);
  SetTheme(ThemeMode::Light, AccentPreset::Violet);
  CHECK(&CurrentTheme() == &GetTheme(ThemeMode::Light, AccentPreset::Violet) && g_theme == &CurrentTheme());
  SetTheme(ThemeMode::Dark, AccentPreset::Cyan);
}

void TestContrast() {
  struct Pair {
    const char* Name;
    Color Theme::* Fg;
    Color Theme::* Bg;
    double Min;
  };
  // The contrast floor of the theming spec: text 4.5:1 on the surface it sits on, focus ring and toggle tracks 3:1 on bg.
  const Pair Pairs[] = {
      {"text/bg", &Theme::Text, &Theme::Bg, 4.5},
      {"text/surface", &Theme::Text, &Theme::Surface, 4.5},
      {"text_muted/bg", &Theme::TextMuted, &Theme::Bg, 4.5},
      {"text_muted/surface", &Theme::TextMuted, &Theme::Surface, 4.5},
      {"text_field/field", &Theme::TextField, &Theme::Field, 4.5},
      {"text_dim/log_bg", &Theme::TextDim, &Theme::LogBg, 4.5},
      {"accent/bg", &Theme::Accent, &Theme::Bg, 4.5},
      {"accent/surface", &Theme::Accent, &Theme::Surface, 4.5},
      {"on_accent/accent", &Theme::OnAccent, &Theme::Accent, 4.5},
      {"hint_letter/hint_disc", &Theme::HintLetter, &Theme::HintDisc, 4.5},
      {"ok/log_bg", &Theme::Ok, &Theme::LogBg, 4.5},
      {"warn/log_bg", &Theme::Warn, &Theme::LogBg, 4.5},
      {"error/log_bg", &Theme::Error, &Theme::LogBg, 4.5},
      {"kde_text/kde_view", &Theme::KdeText, &Theme::KdeView, 4.5},
      {"kde_text_inactive/kde_header", &Theme::KdeTextInactive, &Theme::KdeHeader, 4.5},
      {"accent ring/bg (3:1)", &Theme::Accent, &Theme::Bg, 3.0},
  };
  for (ThemeMode M : {ThemeMode::Dark, ThemeMode::Light}) {
    const Theme& T = GetTheme(M);
    for (const Pair& P : Pairs) {
      const double R = ContrastRatio(T.*(P.Fg), T.*(P.Bg));
      if (R < P.Min)
        std::printf("  contrast %s %s: %.2f < %.1f\n", M == ThemeMode::Dark ? "dark" : "light", P.Name, R, P.Min);
      CHECK(R >= P.Min);
    }
  }
  // Light mode: toggle track and radio ring are 3:1 non-text on bg/surface (the spec's table gives these ratios for light).
  const Theme& L = GetTheme(ThemeMode::Light);
  CHECK(ContrastRatio(L.ToggleOffTrack, L.Bg) >= 3.0);
  CHECK(ContrastRatio(L.RadioOff, L.Surface) >= 3.0);
  // Known values from the spec's contrast column (tolerance 0.1).
  CHECK(std::fabs(ContrastRatio(L.Text, L.Bg) - 14.4) < 0.1);
  CHECK(std::fabs(ContrastRatio(L.TextMuted, L.Bg) - 5.4) < 0.1);
  CHECK(std::fabs(ContrastRatio(L.Accent, L.Bg) - 5.2) < 0.1);
  CHECK(std::fabs(ContrastRatio(L.Error, L.LogBg) - 6.1) < 0.1);
  // Blend: 62% black on bg.
  const Theme& D = GetTheme(ThemeMode::Dark);
  const Color B = Blend(D.Scrim, D.Bg);
  CHECK(B.R == std::lround(27 * (1 - 158 / 255.0)));
}

// ---- Fonts (fake backends) ---------------------------------------------------------------------------------------------

bool ValidUtf8(const std::string& S) {
  for (size_t I = 0; I < S.size();) {
    const unsigned char C = static_cast<unsigned char>(S[I]);
    size_t N = C < 0x80 ? 1 : (C >> 5) == 6 ? 2 : (C >> 4) == 14 ? 3 : (C >> 3) == 30 ? 4 : 0;
    if (!N || I + N > S.size())
      return false;
    for (size_t K = 1; K < N; ++K)
      if ((static_cast<unsigned char>(S[I + K]) & 0xC0) != 0x80)
        return false;
    I += N;
  }
  return true;
}

void TestMeasure() {
  FakeDisplay D(Size {1280, 720});
  FakeRenderer R(D);
  Fonts F(R);
  CHECK(F.Ok());
  const char* Samples[] = {"FEX Switch 2 · Launch", "Appearance settings", "AV To Ty fi fl ffi 0123", "ftp://192.168.1.50:5000", "Wipe RootFS?", "äöüß ÄÖÜ"};
  for (const TextStyle& St : styles::All)
    for (const char* Text : Samples) {
      const float W1 = F.Measure(St, Text, 1.0f);
      CHECK(W1 > 0);
      for (float S : Scales) {
        const float WS = F.Measure(St, Text, S);
        if (std::fabs(WS - W1 * S) > 1.0f)
          std::printf("  measure %s \"%s\" s=%.1f: %.2f vs %.2f\n", St.Name, Text, double(S), double(WS), double(W1 * S));
        CHECK(std::fabs(WS - W1 * S) <= 1.0f);  // width x s within 1 px
      }
    }
  CHECK(F.Measure(styles::Body, "", 1.0f) == 0.0f);
  CHECK(F.Measure(styles::Body, "WW", 1.0f) > F.Measure(styles::Body, "W", 1.0f));
  // Kerning: "AV" is narrower than A + V measured apart.
  CHECK(F.Measure(styles::Body, "AV", 2.0f) < F.Measure(styles::Body, "A", 2.0f) + F.Measure(styles::Body, "V", 2.0f) - 0.5f);
  // Tracking: display is -0.5 du per glyph gap, section +0.6.
  {
    const TextStyle Plain = WithSize(styles::Display, 40, 0);
    const float Diff = F.Measure(Plain, "Launch", 2.0f) - F.Measure(styles::Display, "Launch", 2.0f);
    CHECK(std::fabs(Diff - 5 * 0.5f * 2.0f) < 0.2f);  // 5 gaps x 0.5 du x s
  }
  // Monospace: terminal cell is 0.6 em = 9 du at 15 du; every glyph has the same advance.
  for (float S : Scales) {
    CHECK(std::fabs(F.CellWidth(styles::MonoTerm, S) - 9.0f * S) < 0.1f);
    CHECK(std::fabs(F.Measure(styles::MonoTerm, "iiiiiiii", S) - F.Measure(styles::MonoTerm, "MMMMMMMM", S)) < 0.1f);
  }
  // Line heights of the spec: terminal 15 x 1.55, log 13 x 1.7 (whole pixels).
  CHECK(F.LineHeightPx(styles::MonoTerm, 1.0f) == 23.0f);
  CHECK(F.LineHeightPx(styles::MonoLog, 1.0f) == 22.0f);
  CHECK(F.LineHeightPx(styles::MonoTerm, 2.0f) == std::round(2 * 23.25f));
  CHECK(F.LineHeightPx(styles::Body, 1.0f) >= 16.0f);
  // Metrics and baseline centering.
  for (float S : Scales) {
    const LineMetrics M = F.Metrics(styles::RowLabel, S);
    CHECK(M.Ascent > 0 && M.Descent > 0 && M.Height() >= (M.Ascent + M.Descent));
    const float Base = F.BaselineInBox(styles::RowLabel, S, 100.0f * S, 56.0f * S);
    CHECK(Base == std::round(Base) && Base > 100.0f * S && Base < 156.0f * S);
  }
  // Uppercase (section): same widths as typing the capitals.
  {
    TextStyle NoUpper = styles::Section;
    NoUpper.Upper = false;
    CHECK(F.Measure(styles::Section, "settings äö", 1.5f) == F.Measure(NoUpper, "SETTINGS ÄÖ", 1.5f));
  }
}

void TestEllipsis() {
  FakeDisplay D(Size {1280, 720});
  FakeRenderer R(D);
  Fonts F(R);
  const TextStyle St = styles::MonoUiSmall;
  const std::string Path = "/switch/fex/rootfs/Ubuntu_24.04_amd64_very_long_image_name.sqsh";
  for (float S : Scales) {
    const float Full = F.Measure(St, Path, S);
    CHECK(F.Fit(St, Path, S, Full + 1.0f, Ellipsis::End) == Path);
    CHECK(F.Fit(St, Path, S, Full + 1.0f, Ellipsis::Middle) == Path);
    const float Max = 200.0f * S;
    const std::string E = F.Fit(St, Path, S, Max, Ellipsis::End);
    CHECK(E.size() > 3 && E.compare(E.size() - 3, 3, "\xE2\x80\xA6") == 0 && E[0] == '/');
    CHECK(F.Measure(St, E, S) <= Max && F.Measure(St, E, S) > Max - 2.0f * 13.0f * 0.6f * S);  // tight: within one more glyph
    const std::string M = F.Fit(St, Path, S, Max, Ellipsis::Middle);
    const size_t At = M.find("\xE2\x80\xA6");
    CHECK(At != std::string::npos && At > 0 && At + 3 < M.size());
    CHECK(M[0] == '/' && M.back() == 'h');  // head and tail survive: ".../...sqsh"
    CHECK(F.Measure(St, M, S) <= Max && F.Measure(St, M, S) > Max - 2.0f * 13.0f * 0.6f * S);
    CHECK(ValidUtf8(E) && ValidUtf8(M));
  }
  // Multi-byte text is never cut inside a code point.
  const std::string Uml = "ääääääääääääöööööööööööö€€€€€€€€€€€€€€€";
  for (float W = 10; W < 300; W += 7) {
    CHECK(ValidUtf8(F.Fit(styles::Body, Uml, 1.0f, W, Ellipsis::End)));
    CHECK(ValidUtf8(F.Fit(styles::Body, Uml, 1.0f, W, Ellipsis::Middle)));
  }
  // Not even the ellipsis fits.
  CHECK(F.Fit(styles::Body, "long text here", 1.0f, 1.0f, Ellipsis::End).empty());
  CHECK(F.Fit(styles::Body, "", 1.0f, 1.0f, Ellipsis::Middle).empty());
}

void TestAtlasesAndUploads() {
  FakeDisplay D(Size {1920, 1080});
  FakeRenderer R(D);
  Fonts F(R);
  const Color White = Color::Rgb(255, 255, 255);
  F.Draw(styles::Body, "Hello", 1.5f, 100, 100, White);
  CHECK(CountCalls(R, "CreateTexture") == 1);
  // "Hello": 5 glyph quads, all on whole pixels, sized by the atlas bitmap.
  int Quads = 0;
  bool Whole = true;
  for (const std::string& C : R.Calls)
    if (C.rfind("Image", 0) == 0) {
      ++Quads;
      float T, X, Y, W, H;
      std::sscanf(C.c_str(), "Image %f %f %f %f %f", &T, &X, &Y, &W, &H);
      Whole = Whole && X == std::floor(X) && Y == std::floor(Y) && W == std::floor(W) && H == std::floor(H);
    }
  CHECK(Quads == 5 && Whole);
  // Same (font, size x s): reuse the atlas, no new texture and no new glyph rasterization.
  F.Draw(styles::Body, "Hello", 1.5f, 100, 150, White);
  CHECK(CountCalls(R, "CreateTexture") == 1 && F.Stats().Atlases == 1);
  // Another scale or font is another atlas (never a scaled bitmap).
  F.Draw(styles::Body, "Hello", 2.0f, 100, 150, White);
  F.Draw(styles::Caption, "Hello", 1.5f, 100, 150, White);
  CHECK(F.Stats().Atlases == 3 && CountCalls(R, "CreateTexture") == 3);
  // One upload per page per frame, not per glyph; nothing new, nothing uploaded.
  R.Calls.clear();
  F.EndFrame();
  CHECK(CountCalls(R, "UpdateTexture") == 3);
  R.Calls.clear();
  F.EndFrame();
  CHECK(CountCalls(R, "UpdateTexture") == 0);
  // A page that fills up is followed by another one.
  FakeRenderer R2(D);
  Fonts F2(R2);
  std::string Many;
  for (uint32_t Cp = 0xC0; Cp < 0x180; ++Cp) {
    if (Cp < 0x80) {
      Many.push_back(char(Cp));
    } else {
      Many.push_back(char(0xC0 | (Cp >> 6)));
      Many.push_back(char(0x80 | (Cp & 0x3F)));
    }
  }
  F2.Draw(WithSize(styles::Display, 60), Many, 3.0f, 0, 200, White);  // 180 px glyphs, a few dozen per page
  CHECK(F2.Stats().Pages >= 2 && CountCalls(R2, "CreateTexture") == F2.Stats().Pages);
  R2.Calls.clear();
  F2.EndFrame();
  CHECK(CountCalls(R2, "UpdateTexture") == F2.Stats().Pages);
  // Unused atlases are dropped after a long time (the old scale after a dock/undock).
  FakeRenderer R3(D);
  Fonts F3(R3);
  F3.Draw(styles::Body, "Hello", 1.0f, 0, 50, White);
  for (int I = 0; I < 1000; ++I) {
    F3.Draw(styles::Caption, "kept", 1.5f, 0, 50, White);
    F3.EndFrame();
  }
  CHECK(F3.Stats().Evicted == 1 && F3.Stats().Atlases == 1 && CountCalls(R3, "DestroyTexture") == 1);
}

void TestIconsFake() {
  FakeDisplay D(Size {1920, 1080});
  FakeRenderer R(D);
  Icons I(R);
  const Color C = Color::Rgb(200, 200, 200);
  for (int Id = 0; Id < int(IconId::Count); ++Id)
    CHECK(I.Draw(IconId(Id), Rect {10, 10, 36, 36}, C));
  CHECK(I.Stats().Cached == int(IconId::Count) && I.Stats().Rasterized == int(IconId::Count));
  CHECK(CountCalls(R, "CreateTexture") == 1);  // all on one page
  // Same icon, same size and stroke: cached. Another size or stroke: rasterized again. Another color: no re-raster.
  I.Draw(IconId::House, Rect {100, 100, 36, 36}, Color::Rgb(1, 2, 3));
  CHECK(I.Stats().Rasterized == int(IconId::Count));
  I.Draw(IconId::House, Rect {100, 100, 48, 48}, C);
  I.Draw(IconId::House, Rect {100, 100, 36, 36}, C, stroke::Close);
  CHECK(I.Stats().Rasterized == int(IconId::Count) + 2);
  // Icons are centered in a larger box and placed on whole pixels.
  R.Calls.clear();
  I.Draw(IconId::Gamepad, Rect {0, 0, 57, 57}, C);
  float T, X, Y, W, H;
  CHECK(!R.Calls.empty() && std::sscanf(R.Calls.back().c_str(), "Image %f %f %f %f %f", &T, &X, &Y, &W, &H) == 5);
  CHECK(W == 57 && H == 57 && X == 0 && Y == 0);
  R.Calls.clear();
  I.EndFrame();
  CHECK(CountCalls(R, "UpdateTexture") == 1);
}

// ---- Pixels (host headless Vulkan) ---------------------------------------------------------------------------------------

struct Shot {
  std::vector<uint8_t> Px;
  Size Sz;
  Color At(int X, int Y) const {
    if (X < 0 || Y < 0 || X >= Sz.W || Y >= Sz.H)
      return Color::Rgb(1, 2, 3, 4);
    const uint8_t* P = &Px[(size_t(Y) * size_t(Sz.W) + size_t(X)) * 4];
    return Color {P[0], P[1], P[2], P[3]};
  }
  bool Is(int X, int Y, Color C, int Tol = 0) const {
    const Color A = At(X, Y);
    return std::abs(A.R - C.R) <= Tol && std::abs(A.G - C.G) <= Tol && std::abs(A.B - C.B) <= Tol;
  }
  // Bounding box of pixels that differ from Bg by more than Tol in any channel; false when there is none.
  bool Ink(Color Bg, int& X0, int& Y0, int& X1, int& Y1, int Tol = 0) const {
    X0 = Sz.W;
    Y0 = Sz.H;
    X1 = Y1 = -1;
    for (int Y = 0; Y < Sz.H; ++Y)
      for (int X = 0; X < Sz.W; ++X)
        if (!Is(X, Y, Bg, Tol)) {
          X0 = std::min(X0, X);
          Y0 = std::min(Y0, Y);
          X1 = std::max(X1, X);
          Y1 = std::max(Y1, Y);
        }
    return X1 >= 0;
  }
};

std::unique_ptr<HostPlatform> MakeHost(float S) {
  PlatformOptions O;
  O.Width = int(std::lround(1280 * S));
  O.Height = int(std::lround(720 * S));
  return CreateHostPlatform(O);
}

Shot Render(Platform& P, Fonts& F, Icons& I, const Theme& T, const std::function<void(const DrawContext&)>& Draw) {
  Display& D = P.GetDisplay();
  Renderer& R = P.GetRenderer();
  CHECK(D.BeginFrame());
  CHECK(R.BeginFrame(T.Bg));
  DrawContext C {R, F, I, T, R.Scale()};
  Draw(C);
  F.EndFrame();
  I.EndFrame();
  CHECK(R.EndFrame());
  CHECK(D.Present());
  Shot S;
  CHECK(D.ReadPixels(S.Px, S.Sz));
  return S;
}

void TestSnapping() {
  for (float S : Scales) {
    auto P = MakeHost(S);
    CHECK(P != nullptr);
    if (!P)
      continue;
    Fonts F(P->GetRenderer());
    Icons I(P->GetRenderer());
    const Theme& T = GetTheme(ThemeMode::Dark);
    const int Hair = HairlinePx(S);
    CHECK(Hair == (S == 1.0f ? 1 : S == 3.0f ? 3 : 2));
    Shot Sh = Render(*P, F, I, T, [&](const DrawContext& C) {
      // A 1 du divider at y = 100 du, and a card rect at (33, 200) 101 x 60 du, edges snapped to whole device pixels.
      C.R.Line(0, std::round(100 * C.S), 1280 * C.S, std::round(100 * C.S), float(HairlinePx(C.S)), T.Divider);
      C.R.FillRect(C.Du(33, 200, 101, 60), T.Surface);
      C.R.FillRoundRect(C.Du(300, 300, 280, 300), 20 * C.S, T.Surface);
    });
    const int Y0 = int(std::round(100 * S - Hair * 0.5f));  // Line is centered on y
    CHECK(Sh.Is(500, Y0 - 1, T.Bg) && Sh.Is(500, Y0, T.Divider) && Sh.Is(500, Y0 + Hair - 1, T.Divider) && Sh.Is(500, Y0 + Hair, T.Bg));
    const Rect R = SnapRect(Rect {33 * S, 200 * S, 101 * S, 60 * S});
    const int Xa = int(R.X), Xb = int(R.X + R.W), Ya = int(R.Y), Yb = int(R.Y + R.H), Xm = (Xa + Xb) / 2, Ym = (Ya + Yb) / 2;
    CHECK(Sh.Is(Xa - 1, Ym, T.Bg) && Sh.Is(Xa, Ym, T.Surface) && Sh.Is(Xb - 1, Ym, T.Surface) && Sh.Is(Xb, Ym, T.Bg));
    CHECK(Sh.Is(Xm, Ya - 1, T.Bg) && Sh.Is(Xm, Ya, T.Surface) && Sh.Is(Xm, Yb - 1, T.Surface) && Sh.Is(Xm, Yb, T.Bg));
    // Card: straight edges are exact between the corners.
    const Rect Cd = SnapRect(Rect {300 * S, 300 * S, 280 * S, 300 * S});
    const int Cy = int(Cd.Y + Cd.H / 2);
    CHECK(Sh.Is(int(Cd.X) - 1, Cy, T.Bg) && Sh.Is(int(Cd.X), Cy, T.Surface) && Sh.Is(int(Cd.X + Cd.W) - 1, Cy, T.Surface) && Sh.Is(int(Cd.X + Cd.W), Cy, T.Bg));
    // Corner pixel of the 20 du radius is background, the pixel one radius in is filled.
    CHECK(Sh.Is(int(Cd.X), int(Cd.Y), T.Bg) && Sh.Is(int(Cd.X + 20 * S), int(Cd.Y + 20 * S), T.Surface));
  }
}

void TestTextPixels() {
  for (float S : Scales) {
    auto P = MakeHost(S);
    CHECK(P != nullptr);
    if (!P)
      continue;
    Fonts F(P->GetRenderer());
    Icons I(P->GetRenderer());
    const Theme& T = GetTheme(ThemeMode::Dark);
    const char* Text = "Hello World";
    Shot Sh = Render(*P, F, I, T, [&](const DrawContext& C) { C.F.Draw(styles::Display, Text, C.S, std::round(40 * C.S), std::round(200 * C.S), T.Text); });
    int X0, Y0, X1, Y1;
    CHECK(Sh.Ink(T.Bg, X0, Y0, X1, Y1));
    const float W = F.Measure(styles::Display, Text, S);
    const LineMetrics M = F.Metrics(styles::Display, S);
    const int Left = int(std::round(40 * S)), Base = int(std::round(200 * S));
    // The ink lies inside the measured advance (a pixel or two of bearing and AA) and between the font's ascent and descent.
    CHECK(X0 >= Left - 2 && X1 <= Left + int(std::ceil(W)) + 2);
    CHECK(double(X1 - X0) > double(W) * 0.85);
    CHECK(Y0 >= Base - int(std::ceil(M.Ascent)) - 1 && Y1 <= Base + int(std::ceil(M.Descent)) + 1);
    // The H is a tall stem: full-coverage pixels equal the tint exactly (coverage-only atlas times the draw color).
    bool Full = false;
    for (int Y = Y0; Y <= Y1 && !Full; ++Y)
      for (int X = X0; X <= X1 && !Full; ++X)
        Full = Sh.Is(X, Y, T.Text, 2);
    CHECK(Full);
    // Same string on the light theme: ink with the dark text color on the light background.
    const Theme& L = GetTheme(ThemeMode::Light);
    Shot Sl = Render(*P, F, I, L, [&](const DrawContext& C) { C.F.Draw(styles::Display, Text, C.S, std::round(40 * C.S), std::round(200 * C.S), L.Text); });
    int A0, B0, A1, B1;
    CHECK(Sl.Ink(L.Bg, A0, B0, A1, B1) && A0 == X0 && A1 == X1 && B0 == Y0 && B1 == Y1);  // identical placement: atlases are color independent
    // Alignment: right-aligned text ends at the anchor, centered text is centered.
    Shot Sr = Render(*P, F, I, T, [&](const DrawContext& C) {
      C.F.Draw(styles::Body, "Right", C.S, std::round(600 * C.S), std::round(100 * C.S), T.Text, Align::Right);
      C.F.Draw(styles::Body, "Center", C.S, std::round(900 * C.S), std::round(200 * C.S), T.Text, Align::Center);
    });
    CHECK(Sr.Ink(T.Bg, A0, B0, A1, B1));
    int R0, R1, C0, C1, Dummy0, Dummy1;
    Shot OnlyRight = Render(*P, F, I, T, [&](const DrawContext& C) { C.F.Draw(styles::Body, "Right", C.S, std::round(600 * C.S), std::round(100 * C.S), T.Text, Align::Right); });
    CHECK(OnlyRight.Ink(T.Bg, R0, Dummy0, R1, Dummy1) && R1 <= int(std::round(600 * S)) + 1 && R1 >= int(std::round(600 * S)) - 3);
    Shot OnlyCenter = Render(*P, F, I, T, [&](const DrawContext& C) { C.F.Draw(styles::Body, "Center", C.S, std::round(900 * C.S), std::round(200 * C.S), T.Text, Align::Center); });
    CHECK(OnlyCenter.Ink(T.Bg, C0, Dummy0, C1, Dummy1) && std::abs((C0 + C1) / 2 - int(std::round(900 * S))) <= 3);
  }
}

void TestIconPixels() {
  for (float S : Scales) {
    auto P = MakeHost(S);
    CHECK(P != nullptr);
    if (!P)
      continue;
    Fonts F(P->GetRenderer());
    Icons I(P->GetRenderer());
    const Theme& T = GetTheme(ThemeMode::Dark);
    for (int Id = 0; Id < int(IconId::Count); ++Id) {
      const Rect Box = SnapRect(Rect {100 * S, 100 * S, 24 * S, 24 * S});
      Shot Sh = Render(*P, F, I, T, [&](const DrawContext& C) { C.I.Draw(IconId(Id), Box, T.Accent); });
      int X0, Y0, X1, Y1;
      const bool Ink = Sh.Ink(T.Bg, X0, Y0, X1, Y1);
      CHECK(Ink);
      if (!Ink)
        continue;
      // Rasterized at the target size: ink inside the dst box, and a stroke of full coverage has the accent color.
      CHECK(X0 >= int(Box.X) && Y0 >= int(Box.Y) && X1 < int(Box.X + Box.W) && Y1 < int(Box.Y + Box.H));
      CHECK(double(X1 - X0 + 1) >= double(Box.W) * 0.2 && double(Y1 - Y0 + 1) >= double(Box.H) * 0.2);
      bool Full = false;
      for (int Y = Y0; Y <= Y1 && !Full; ++Y)
        for (int X = X0; X <= X1 && !Full; ++X)
          Full = Sh.Is(X, Y, T.Accent, 6);
      if (S >= 1.5f)
        CHECK(Full);
    }
  }
}

} // namespace

int main(int Argc, char** Argv) {
  if (Argc > 1 && std::strcmp(Argv[1], "--dump-theme") == 0) {
    DumpTheme();
    return 0;
  }
  TestThemeCompleteness();
  TestContrast();
  TestMeasure();
  TestEllipsis();
  TestAtlasesAndUploads();
  TestIconsFake();
  TestSnapping();
  TestTextPixels();
  TestIconPixels();
  std::printf("ui-text-test: %d checks, %d failures\n", Checks, Failures);
  return Failures ? 1 : 0;
}
