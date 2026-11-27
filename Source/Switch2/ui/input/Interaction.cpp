// SPDX-License-Identifier: MIT
#include "input/Interaction.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ui {

// ---- Pointer sprite -----------------------------------------------------------------------------------------------------

// The 24 du arrow, rasterized with 4x4 supersampling into an RGBA texture at the current scale and theme (a tip at the
// hotspot, filled with hint_disc and outlined with hint_letter so it reads on any background).
struct Interaction::PointerSprite {
  Renderer* R = nullptr;
  TextureId Tex = NoTexture;
  int Size = 0, Pad = 0, Px = 0;
  Color Fill, Outline;
  float HotX = 0, HotY = 0;

  ~PointerSprite() {
    if (R && Tex != NoTexture)
      R->DestroyTexture(Tex);
  }

  static bool Inside(const float (*P)[2], int N, float X, float Y) {
    bool In = false;
    for (int I = 0, J = N - 1; I < N; J = I++)
      if ((P[I][1] > Y) != (P[J][1] > Y) && X < (P[J][0] - P[I][0]) * (Y - P[I][1]) / (P[J][1] - P[I][1]) + P[I][0])
        In = !In;
    return In;
  }
  static float DistSeg(float X, float Y, float Ax, float Ay, float Bx, float By) {
    const float Dx = Bx - Ax, Dy = By - Ay;
    float T = ((X - Ax) * Dx + (Y - Ay) * Dy) / (Dx * Dx + Dy * Dy);
    T = Clamp01(T);
    return std::hypot(X - (Ax + T * Dx), Y - (Ay + T * Dy));
  }

  bool Ensure(Renderer& Rd, int NewPx, Color F, Color O) {
    if (Tex != NoTexture && NewPx == Px && F.R == Fill.R && F.G == Fill.G && F.B == Fill.B && O.R == Outline.R && O.G == Outline.G && O.B == Outline.B)
      return true;
    if (Tex != NoTexture)
      Rd.DestroyTexture(Tex);
    R = &Rd;
    Px = NewPx;
    Fill = F;
    Outline = O;
    Pad = std::max(2, NewPx / 14);
    Size = NewPx + 2 * Pad;
    const float K = float(NewPx) / 24.0f;
    static const float V[7][2] = {{2, 1}, {2, 18}, {6.2f, 14.2f}, {9.3f, 21}, {12.2f, 19.7f}, {9.1f, 13.0f}, {14.5f, 13.0f}};
    float P[7][2];
    for (int I = 0; I < 7; ++I) {
      P[I][0] = V[I][0] * K + float(Pad);
      P[I][1] = V[I][1] * K + float(Pad);
    }
    HotX = P[0][0];
    HotY = P[0][1];
    const float Ow = std::max(1.0f, float(NewPx) / 16.0f);
    std::vector<uint8_t> Pix(size_t(Size) * size_t(Size) * 4, 0);
    for (int Y = 0; Y < Size; ++Y)
      for (int X = 0; X < Size; ++X) {
        int Fc = 0, Oc = 0;
        for (int Sy = 0; Sy < 4; ++Sy)
          for (int Sx = 0; Sx < 4; ++Sx) {
            const float Fx = float(X) + (float(Sx) + 0.5f) / 4, Fy = float(Y) + (float(Sy) + 0.5f) / 4;
            const bool In = Inside(P, 7, Fx, Fy);
            bool Out = In;
            for (int E = 0; E < 7 && !Out; ++E)
              Out = DistSeg(Fx, Fy, P[E][0], P[E][1], P[(E + 1) % 7][0], P[(E + 1) % 7][1]) <= Ow;
            Fc += In;
            Oc += Out;
          }
        if (!Oc)
          continue;
        const float Fa = float(Fc) / 16.0f, Oa = float(Oc) / 16.0f;
        const float W = Fa / Oa;
        uint8_t* Q = &Pix[(size_t(Y) * size_t(Size) + size_t(X)) * 4];
        Q[0] = uint8_t(std::lround(float(Outline.R) + (float(Fill.R) - float(Outline.R)) * W));
        Q[1] = uint8_t(std::lround(float(Outline.G) + (float(Fill.G) - float(Outline.G)) * W));
        Q[2] = uint8_t(std::lround(float(Outline.B) + (float(Fill.B) - float(Outline.B)) * W));
        Q[3] = uint8_t(std::lround(255.0f * Oa));
      }
    Tex = Rd.CreateTexture(Size, Size, TextureFormat::Rgba8, TextureFilter::Nearest);
    if (Tex == NoTexture)
      return false;
    Rd.UpdateTexture(Tex, 0, 0, Size, Size, Pix.data(), 0);
    return true;
  }
};

Interaction::Interaction() = default;
Interaction::~Interaction() = default;

// ---- Layers -------------------------------------------------------------------------------------------------------------

void Interaction::Flatten(const std::vector<Widget*>& In, std::vector<Widget*>& Out) {
  for (Widget* W : In) {
    if (!W)
      continue;
    if (W->Id == NoFocus)
      W->Id = NextId++;
    Out.push_back(W);
    std::vector<Widget*> Kids;
    W->Children(Kids);
    Flatten(Kids, Out);
  }
}

void Interaction::SetRoot(const std::vector<Widget*>& Widgets) {
  Layer& L = Layers[0];
  L.Roots = Widgets;
  L.Flat.clear();
  Flatten(Widgets, L.Flat);
}

void Interaction::Reflatten() {
  bool PressKept = PressedW == nullptr;
  for (Layer& L : Layers) {
    L.Flat.clear();
    Flatten(L.Roots, L.Flat);
    if (!PressKept && std::find(L.Flat.begin(), L.Flat.end(), PressedW) != L.Flat.end())
      PressKept = true;
  }
  if (!PressKept)
    PressedW = nullptr;
}

void Interaction::PushLayer(const std::vector<Widget*>& Widgets, Widget* InitialFocus, std::function<void()> Back) {
  ClearPress();
  Layer L;
  L.Modal = true;
  L.Back = std::move(Back);
  L.Roots = Widgets;
  Flatten(Widgets, L.Flat);
  Layers.push_back(std::move(L));
  Layer& N = Top();
  Rebuild(N, 0);
  if (InitialFocus && InitialFocus->Id != NoFocus)
    N.Tree.SetFocus(InitialFocus->Id);
  ApplyFocus(N);
}

void Interaction::PopLayer() {
  if (Layers.size() <= 1)
    return;
  ClearPress();
  Layers.pop_back();
}

Widget* Interaction::ById(const Layer& L, int Id) const {
  for (Widget* W : L.Flat)
    if (W->Id == Id)
      return W;
  return nullptr;
}

void Interaction::Rebuild(Layer& L, uint64_t Now) {
  const bool Base = &L == &Layers[0];
  const float Off = Base && Scroll ? Scroll->Target() : 0.0f;
  (void)Now;
  L.Tree.Begin();
  for (Widget* W : L.Flat) {
    if (!W->Focusable || !W->Visible)
      continue;
    FocusNode N;
    N.Id = W->Id;
    N.R = W->Fixed ? W->Bounds : Rect {W->Bounds.X, W->Bounds.Y - Off, W->Bounds.W, W->Bounds.H};
    N.Enabled = W->Enabled;
    N.Group = W->Group;
    for (int D = 0; D < 4; ++D)
      N.Over[D] = W->Over[D];
    N.Outset = W->RingOutset();
    N.Stroke = W->RingStroke();
    N.Radius = W->RingRadius();
    L.Tree.Add(N);
  }
  L.Tree.End();
}

void Interaction::ApplyFocus(Layer& L) {
  for (Widget* W : L.Flat)
    W->Focused = W->Focusable && W->Id == L.Tree.Focused();
}

int Interaction::FocusedId() const { return Top().Tree.Focused(); }
Widget* Interaction::FocusedWidget() const { return ById(Top(), Top().Tree.Focused()); }

void Interaction::FocusChanged(Widget* W, uint64_t Now) {
  if (W && Scroll && &Top() == &Layers[0] && !W->Fixed)
    Scroll->EnsureVisible(W->Bounds, Now);
}

bool Interaction::SetFocus(int Id, uint64_t Now) {
  Layer& L = Top();
  Rebuild(L, Now);
  const bool Changed = L.Tree.SetFocus(Id);
  ApplyFocus(L);
  if (Changed)
    FocusChanged(ById(L, Id), Now);
  return Changed;
}

void Interaction::ClearPress() {
  if (PressedW)
    PressedW->Pressed = false;
  PressedW = nullptr;
}

Widget* Interaction::HitWidget(float X, float Y, uint64_t Now) const {
  const Layer& L = Top();
  const bool Base = &L == &Layers[0];
  const float Off = Base && Scroll ? Scroll->Offset(Now) : 0.0f;
  for (size_t I = L.Flat.size(); I-- > 0;) {
    Widget* W = L.Flat[I];
    if (!W->Visible || !(W->Hittable || W->Focusable))
      continue;
    Rect B = W->Bounds;
    if (!W->Fixed) {
      B.Y -= Off;
      if (Base && Scroll && (Y < Scroll->ViewTop() || Y >= Scroll->ViewTop() + Scroll->ViewHeight()))
        continue;  // outside the scroll viewport
    }
    const Rect H = HitRect(B);
    if (X >= H.X && X < H.X + H.W && Y >= H.Y && Y < H.Y + H.H)
      return W;
  }
  return nullptr;
}

void Interaction::DoScroll(float Dy, uint64_t Now) {
  if (ScrollFilter && ScrollFilter(PtrX, PtrY, Dy))
    return;
  if (Scroll && Layers.size() == 1)
    Scroll->By(Dy, Now);
}

uint64_t Interaction::Count(const char* What) const {
  if (!std::strcmp(What, "nav"))
    return NavCount;
  if (!std::strcmp(What, "activate"))
    return ActivateCount;
  return BackCount;
}

// ---- Dispatch -----------------------------------------------------------------------------------------------------------

void Interaction::Dispatch(const UiEvent& E, uint64_t Now) {
  using T = UiEvent::Type;
  Layer& L = Top();
  switch (E.Kind) {
  case T::ModeChanged:
    if (E.NewMode != Mode::Touch)
      ClearPress();
    break;
  case T::Nav: {
    Rebuild(L, Now);
    Widget* F = ById(L, L.Tree.Focused());
    if (!F) {
      L.Tree.End();
      ApplyFocus(L);
      break;
    }
    if (F->Enabled && F->OnNav(E.D, Now)) {
      ++NavCount;
      break;
    }
    const int To = L.Tree.Navigate(E.D);
    ApplyFocus(L);
    if (To != NoFocus) {
      ++NavCount;
      FocusChanged(ById(L, To), Now);
    }
    break;
  }
  case T::Button: {
    Widget* F = FocusedWidget();
    if (E.Btn == Button::A) {
      if (F && F->Enabled) {
        ++ActivateCount;
        F->Activate(Now);
      }
    } else if (E.Btn == Button::B) {
      ++BackCount;
      if (L.Modal) {
        if (L.Back)
          L.Back();
      } else if (OnBack) {
        OnBack();
      }
    } else if (OnButton) {
      OnButton(E.Btn);
    }
    break;
  }
  case T::PointerMove: {
    PtrX = E.X;
    PtrY = E.Y;
    Rebuild(L, Now);
    Widget* H = HitWidget(E.X, E.Y, Now);
    if (E.Held && PressedW) {
      PressedW->Pressed = H == PressedW;
      if (PressedW->WantsDrag())
        PressedW->OnDragTo(E.X, E.Y, Now);
    } else if (H && H->Focusable && H->Enabled && L.Tree.SetFocus(H->Id)) {  // hover focuses
      ApplyFocus(L);
      FocusChanged(H, Now);
    }
    break;
  }
  case T::PointerDown: {
    PtrX = E.X;
    PtrY = E.Y;
    Rebuild(L, Now);
    ClearPress();
    Widget* H = HitWidget(E.X, E.Y, Now);
    if (H && H->Enabled) {
      PressedW = H;
      H->Pressed = true;
      if (H->Focusable && L.Tree.SetFocus(H->Id)) {
        ApplyFocus(L);
        FocusChanged(H, Now);
      }
      if (H->WantsDrag())
        H->OnDragTo(E.X, E.Y, Now);
    }
    break;
  }
  case T::Drag: {
    if (PressedW && PressedW->WantsDrag()) {
      PressedW->OnDragTo(E.X, E.Y, Now);
    } else {
      ClearPress();  // a drag is a scroll, not a tap
      PtrX = E.X;
      PtrY = E.Y;
      DoScroll(-E.DY, Now);
    }
    break;
  }
  case T::PointerUp: {
    Widget* P = PressedW;
    ClearPress();
    if (P && E.Tap && P->Enabled) {
      Rebuild(L, Now);
      if (HitWidget(E.X, E.Y, Now) == P && !P->WantsDrag()) {
        ++ActivateCount;
        P->Activate(Now);
      }
    }
    break;
  }
  case T::LongPress:
    if (OnLongPress)
      OnLongPress(E.X, E.Y);
    break;
  case T::Scroll:
    DoScroll(E.DY, Now);
    break;
  }
}

void Interaction::Update(uint64_t Now, float Scale, const std::vector<Event>& Raw) {
  if (Scale != LastScale) {
    LastScale = Scale;
    Modes.SetScale(Scale);
  }
  std::vector<UiEvent> Out;
  for (Event E : Raw) {
    E.TimeMs = Now;  // one clock for everything: the caller's (tests and the host use a fake one)
    Modes.Feed(E, Out);
  }
  Modes.Update(Now, Out);
  Reflatten();
  for (const UiEvent& U : Out)
    Dispatch(U, Now);
  Reflatten();  // a handler may have changed hints or children
  Layer& L = Top();
  Rebuild(L, Now);
  ApplyFocus(L);
  Ring.Update(L.Tree.Focused(), Modes.GetMode() != Mode::Touch, Now);
  for (Widget* W : L.Flat)
    W->Tick(Now);
  if (Layers.size() > 1)  // the base screen keeps animating under a modal
    for (Widget* W : Layers[0].Flat)
      W->Tick(Now);
}

// ---- Overlay ------------------------------------------------------------------------------------------------------------

void Interaction::DrawOverlay(const Frame& Base) {
  const Widget* W = FocusedWidget();
  if (W && W->Visible && W->ShowsRing() && Modes.GetMode() != Mode::Touch) {
    const Frame F = FrameFor(Base, *W);
    const bool Clip = W->RingClip.H > 0;
    if (Clip)
      Base.Dc.R.PushClip(Base.Dc.Du(W->RingClip.X, W->RingClip.Y, W->RingClip.W, W->RingClip.H));
    Ring.Draw(F, *W);
    if (Clip)
      Base.Dc.R.PopClip();
  }
  if (Modes.PointerVisible()) {
    const float S = Base.S();
    if (!Sprite)
      Sprite = std::make_unique<PointerSprite>();
    const int Px = int(std::lround(tuning::PointerSizeDu * S));
    if (Sprite->Ensure(Base.Dc.R, Px, Base.T().HintDisc, Base.T().HintLetter)) {
      const float X = std::round(Modes.PointerX() * S - Sprite->HotX), Y = std::round(Modes.PointerY() * S - Sprite->HotY);
      Base.Dc.R.Image(Sprite->Tex, Rect {X, Y, float(Sprite->Size), float(Sprite->Size)}, Rect {0, 0, float(Sprite->Size), float(Sprite->Size)}, Color::Rgb(255, 255, 255));
    }
  }
}

} // namespace ui
