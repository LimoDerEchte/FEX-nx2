// SPDX-License-Identifier: MIT
// Host tests for the UI platform and render layers (make check-ui). Fake backends test the interface helpers; the host
// headless Vulkan backend tests the real renderer by reading pixels back.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

#include "platform/Log.h"
#include "platform/fake/FakePlatform.h"
#include "platform/host/HostPlatform.h"

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

constexpr Color Bg = Color::Rgb(27, 27, 31);
constexpr Color Red = Color::Rgb(220, 30, 40);
constexpr Color Green = Color::Rgb(30, 200, 60);
constexpr Color Blue = Color::Rgb(40, 60, 230);

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
    Color A = At(X, Y);
    auto Near = [&](int V, int W) { return std::abs(V - W) <= Tol; };
    return Near(A.R, C.R) && Near(A.G, C.G) && Near(A.B, C.B);
  }
};

Shot Frame(Platform& P, const std::function<void(Renderer&)>& Draw) {
  Display& D = P.GetDisplay();
  Renderer& R = P.GetRenderer();
  CHECK(D.BeginFrame());
  CHECK(R.BeginFrame(Bg));
  Draw(R);
  CHECK(R.EndFrame());
  CHECK(D.Present());
  Shot S;
  CHECK(D.ReadPixels(S.Px, S.Sz));
  return S;
}

void TestInterfaceHelpers() {
  CHECK(HairlinePx(1.0f) == 1);
  CHECK(HairlinePx(1.5f) == 2);
  CHECK(HairlinePx(3.0f) == 3);
  CHECK(HairlinePx(0.4f) == 1);
  CHECK(ScaleForHeight(720) == 1.0f);
  CHECK(ScaleForHeight(1080) == 1.5f);
  CHECK(ScaleForHeight(2160) == 3.0f);
  Rect R = SnapRect(Rect {10.4f, 20.6f, 30.2f, 10.2f});
  CHECK(R.X == 10 && R.Y == 21 && R.W == 31 && R.H == 10);  // edges 10 and 41, 21 and 31
}

void TestFakeBackends() {
  FakeDisplay D(Size {1920, 1080});
  FakeInput In(D);
  FakeRenderer R(D);
  std::vector<Event> Ev;
  In.Poll(Ev);
  CHECK(Ev.empty());
  CHECK(D.Scale() == 1.5f);
  D.SetSize(Size {3840, 2160});  // docked
  In.Poll(Ev);
  CHECK(Ev.size() == 1 && Ev[0].Kind == Event::Type::SizeChanged && Ev[0].W == 3840 && Ev[0].H == 2160);
  CHECK(R.Scale() == 3.0f);
  D.SetSize(Size {3840, 2160});  // same size: no event
  Ev.clear();
  In.Poll(Ev);
  CHECK(Ev.empty());
  Event E;
  E.Kind = Event::Type::ButtonDown;
  E.Btn = Button::A;
  In.Push(E);
  In.Poll(Ev);
  CHECK(Ev.size() == 1 && Ev[0].Btn == Button::A);
  R.FillCircle(10, 20, 5, Bg);
  CHECK(R.Calls.size() == 1 && R.Calls[0].rfind("FillRoundRect 5.0", 0) == 0);
  FakeSystem Sys;
  Sys.Now = 1234;
  CHECK(Sys.MonotonicMs() == 1234);
}

void TestHostRenderer() {
  PlatformOptions O;
  O.Width = 640;
  O.Height = 360;
  auto P = CreateHostPlatform(O);
  CHECK(P != nullptr);
  if (!P)
    return;
  CHECK(P->GetDisplay().GetSize() == (Size {640, 360}));
  CHECK(P->GetRenderer().Scale() == 0.5f);

  TextureId Rgba = NoTexture, Alpha = NoTexture;
  Shot S = Frame(*P, [&](Renderer& R) {
    R.FillRect(Rect {10, 10, 20, 20}, Red);                        // exact edges
    R.FillRect(Rect {50.5f, 10, 10, 10}, Green);                   // half-pixel left edge: ~50% coverage
    R.FillRoundRect(Rect {100, 100, 40, 40}, 10, Blue);            // rounded corners
    R.Ring(220, 120, 20, 4, Red);                                  // ring
    R.FillCircle(300, 120, 10, Green);                             // circle
    R.Line(10, 200, 100, 200, 1, Red);                             // horizontal hairline
    R.Line(200, 180, 200, 260, 2, Green);                          // vertical 2 px
    CHECK(!R.Line(0, 0, 10, 10, 1, Red));                          // diagonal is not implemented
    R.FillRect(Rect {400, 10, 30, 30}, Color::Rgb(255, 255, 255, 128));  // 50% white over background
    R.PushClip(Rect {0, 300, 50, 50});
    R.FillRect(Rect {0, 280, 100, 100}, Blue);                     // clipped to (0,300)-(50,350)
    R.PopClip();
    R.FillRect(Rect {120, 300, 10, 10}, Green);                    // unclipped after pop
    const uint8_t Texels[4 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    Rgba = R.CreateTexture(2, 2, TextureFormat::Rgba8, TextureFilter::Nearest);
    CHECK(Rgba != NoTexture);
    CHECK(R.UpdateTexture(Rgba, 0, 0, 2, 2, Texels, 0));
    R.Image(Rgba, Rect {300, 300, 8, 8}, Rect {0, 0, 2, 2}, Color::Rgb(255, 255, 255));
    const uint8_t Cov[4] = {255, 0, 255, 0};
    Alpha = R.CreateTexture(2, 2, TextureFormat::Alpha8, TextureFilter::Nearest);
    CHECK(Alpha != NoTexture);
    CHECK(R.UpdateTexture(Alpha, 0, 0, 2, 2, Cov, 0));
    R.Image(Alpha, Rect {320, 300, 8, 8}, Rect {0, 0, 2, 2}, Color::Rgb(255, 128, 0));
  });
  CHECK(S.Sz == (Size {640, 360}));
  CHECK(S.Is(0, 0, Bg));
  CHECK(S.Is(639, 359, Bg));
  // Exact edges.
  CHECK(S.Is(9, 15, Bg) && S.Is(10, 15, Red) && S.Is(29, 15, Red) && S.Is(30, 15, Bg));
  CHECK(S.Is(15, 9, Bg) && S.Is(15, 10, Red) && S.Is(15, 29, Red) && S.Is(15, 30, Bg));
  // Half-pixel edge: roughly half way between the background and green.
  CHECK(S.Is(50, 15, Color::Rgb(28, 113, 45), 6));
  CHECK(S.Is(51, 15, Green));
  // Rounded rect: corner pixel is background, edge middle and centre are filled.
  CHECK(S.Is(100, 100, Bg) && S.Is(100, 120, Blue) && S.Is(120, 120, Blue) && S.Is(139, 139, Bg));
  // Ring: centre empty, band filled, outside empty.
  CHECK(S.Is(220, 120, Bg) && S.Is(220 + 18, 120, Red) && S.Is(220 + 22, 120, Bg) && S.Is(220 - 18, 120, Red));
  // Circle.
  CHECK(S.Is(300, 120, Green) && S.Is(300 + 12, 120, Bg) && S.Is(300, 120 - 9, Green));
  // Lines: 1 px and 2 px, snapped.
  CHECK(S.Is(50, 199, Bg) && S.Is(50, 200, Red) && S.Is(50, 201, Bg));
  CHECK(S.Is(199, 220, Green) && S.Is(200, 220, Green) && S.Is(198, 220, Bg) && S.Is(201, 220, Bg));
  CHECK(S.Is(9, 200, Bg) && S.Is(10, 200, Red) && S.Is(99, 200, Red) && S.Is(100, 200, Bg));
  // Alpha blend: 50% white over the background.
  CHECK(S.Is(410, 20, Color::Rgb(141, 141, 143), 3));
  // Clip.
  CHECK(S.Is(25, 320, Blue) && S.Is(49, 349, Blue) && S.Is(50, 320, Bg) && S.Is(25, 299, Bg) && S.Is(25, 285, Bg));
  CHECK(S.Is(125, 305, Green));
  // RGBA texture, nearest: 4 texels, each 4x4 pixels.
  CHECK(S.Is(301, 301, Color::Rgb(255, 0, 0)) && S.Is(306, 301, Color::Rgb(0, 255, 0)) && S.Is(301, 306, Color::Rgb(0, 0, 255)) && S.Is(306, 306, Color::Rgb(255, 255, 255)));
  // Alpha texture: covered texels take the tint, uncovered ones show the background.
  CHECK(S.Is(321, 301, Color::Rgb(255, 128, 0)) && S.Is(326, 301, Bg) && S.Is(321, 306, Color::Rgb(255, 128, 0)) && S.Is(326, 306, Bg));

  // Destroyed textures are ignored and recycled; the frame still renders.
  Shot S2 = Frame(*P, [&](Renderer& R) {
    R.DestroyTexture(Rgba);
    R.Image(Rgba, Rect {300, 300, 8, 8}, Rect {0, 0, 2, 2}, Color::Rgb(255, 255, 255));
    R.DestroyTexture(Alpha);
    TextureId Again = R.CreateTexture(2, 2, TextureFormat::Rgba8, TextureFilter::Nearest);
    CHECK(Again != NoTexture);
  });
  CHECK(S2.Is(301, 301, Bg));

  // Size change (dock): the next BeginFrame applies it, the display reports it, Input emits one SizeChanged.
  P->SetSize(1280, 720);
  Shot S3 = Frame(*P, [&](Renderer& R) { R.FillRect(Rect {100, 100, 50, 50}, Red); });
  CHECK(S3.Sz == (Size {1280, 720}));
  CHECK(P->GetRenderer().Scale() == 1.0f);
  std::vector<Event> Ev;
  P->GetInput().Poll(Ev);
  CHECK(Ev.size() == 1 && Ev[0].Kind == Event::Type::SizeChanged && Ev[0].W == 1280 && Ev[0].H == 720);
  CHECK(S3.Is(99, 120, Bg) && S3.Is(100, 120, Red) && S3.Is(149, 120, Red) && S3.Is(150, 120, Bg));
  Ev.clear();
  P->GetInput().Poll(Ev);
  CHECK(Ev.empty());

  // A burst of quads grows the vertex buffers (16384 quads at start).
  Shot S4 = Frame(*P, [&](Renderer& R) {
    for (int I = 0; I < 20000; ++I)
      R.FillRect(Rect {float(I % 100) * 10, float((I / 100) % 70) * 10, 5, 5}, Green);
  });
  CHECK(S4.Is(2, 2, Green) && S4.Is(7, 2, Bg));
}

} // namespace

int main() {
  TestInterfaceHelpers();
  TestFakeBackends();
  TestHostRenderer();
  std::printf("%d checks, %d failures\n", Checks, Failures);
  return Failures ? 1 : 0;
}
