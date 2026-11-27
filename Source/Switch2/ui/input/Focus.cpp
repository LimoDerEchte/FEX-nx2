// SPDX-License-Identifier: MIT
#include "input/Focus.h"

#include <algorithm>
#include <cmath>

#include "input/InputModes.h"

namespace ui {

Rect HitRect(Rect R) {
  const float Min = tuning::MinHitDu;
  if (R.W < Min) {
    R.X -= (Min - R.W) / 2;
    R.W = Min;
  }
  if (R.H < Min) {
    R.Y -= (Min - R.H) / 2;
    R.H = Min;
  }
  return R;
}

const FocusNode* FocusTree::Find(int Id) const {
  for (const FocusNode& N : Nodes)
    if (N.Id == Id)
      return &N;
  return nullptr;
}

void FocusTree::End() {
  const FocusNode* F = Find(Cur);
  if (F && F->Enabled)
    return;
  Cur = NoFocus;
  for (const FocusNode& N : Nodes)
    if (N.Enabled) {
      Cur = N.Id;
      if (N.Group != NoFocus)
        LastInGroup[N.Group] = N.Id;
      return;
    }
}

bool FocusTree::SetFocus(int Id) {
  const FocusNode* N = Find(Id);
  if (!N || !N->Enabled || Id == Cur)
    return false;
  Cur = Id;
  if (N->Group != NoFocus)
    LastInGroup[N->Group] = Id;
  return true;
}

// Direction vector components: Along is distance in the pressed direction, Side the perpendicular offset.
static void Components(Dir D, float Dx, float Dy, float& Along, float& Side) {
  Along = Side = 0;
  switch (D) {
  case Dir::Up: Along = -Dy; Side = Dx; break;
  case Dir::Down: Along = Dy; Side = Dx; break;
  case Dir::Left: Along = -Dx; Side = Dy; break;
  case Dir::Right: Along = Dx; Side = Dy; break;
  }
}

int FocusTree::Best(const FocusNode& From, Dir D) const {
  const float Cx = From.R.X + From.R.W / 2, Cy = From.R.Y + From.R.H / 2;
  int BestId = NoFocus;
  float BestDist = 0, BestSide = 0;
  for (const FocusNode& N : Nodes) {
    if (N.Id == From.Id || !N.Enabled)
      continue;
    float Along, Side;
    Components(D, N.R.X + N.R.W / 2 - Cx, N.R.Y + N.R.H / 2 - Cy, Along, Side);
    // +-45 degree cone: strictly ahead and not more sideways than ahead.
    if (Along <= 0 || std::fabs(Side) > Along)
      continue;
    const float Dist = std::hypot(Along, Side);
    const float AbsSide = std::fabs(Side);
    const bool Better = BestId == NoFocus || Dist < BestDist - 0.5f || (std::fabs(Dist - BestDist) <= 0.5f && (AbsSide < BestSide || (AbsSide == BestSide && N.Id < BestId)));
    if (Better) {
      BestId = N.Id;
      BestDist = Dist;
      BestSide = AbsSide;
    }
  }
  return BestId;
}

int FocusTree::Navigate(Dir D) {
  const FocusNode* From = Find(Cur);
  if (!From)
    return NoFocus;
  int Target = NoFocus;
  const Override& O = From->Over[int(D)];
  if (O.Id != NoFocus) {
    const FocusNode* T = Find(O.Id);
    if (T && T->Enabled)
      Target = O.Id;
  } else if (O.Group != NoFocus) {
    auto It = LastInGroup.find(O.Group);
    const FocusNode* T = It != LastInGroup.end() ? Find(It->second) : nullptr;
    if (T && T->Enabled)
      Target = T->Id;
    else
      for (const FocusNode& N : Nodes)  // never focused: the first enabled node of the group
        if (N.Group == O.Group && N.Enabled) {
          Target = N.Id;
          break;
        }
  }
  if (Target == NoFocus)
    Target = Best(*From, D);
  if (Target == NoFocus || Target == Cur)
    return NoFocus;
  SetFocus(Target);
  return Target;
}

int FocusTree::HitTest(float X, float Y) const {
  for (size_t I = Nodes.size(); I-- > 0;) {
    const FocusNode& N = Nodes[I];
    if (!N.Enabled)
      continue;
    const Rect H = HitRect(N.R);
    if (X >= H.X && X < H.X + H.W && Y >= H.Y && Y < H.Y + H.H)
      return N.Id;
  }
  return NoFocus;
}

// ---- ScrollView -------------------------------------------------------------------------------------------------------

void ScrollView::SetViewport(float T, float H, float C) {
  Top = T;
  Height = H;
  Content = C;
  const float Max = MaxOffset();
  if (Pos.To > Max)
    Pos.Set(Max);
}

float ScrollView::Offset(uint64_t Now) const { return std::clamp(Pos.Value(Now), 0.0f, MaxOffset()); }

void ScrollView::EnsureVisible(Rect R, uint64_t Now) {
  constexpr float Margin = 24;
  // R is in the coordinates of the unscrolled layout; the viewport shows [Top + Target, Top + Target + Height).
  float Target = Pos.To;
  if (R.Y - Margin < Top + Target)
    Target = R.Y - Margin - Top;
  else if (R.Y + R.H + Margin > Top + Target + Height)
    Target = R.Y + R.H + Margin - Top - Height;
  Target = std::clamp(Target, 0.0f, MaxOffset());
  if (Target != Pos.To)
    Pos.Go(Target, Now, 150);
}

void ScrollView::By(float Dy, uint64_t Now) {
  const float Cur = Offset(Now);
  Pos.Set(std::clamp(Cur + Dy, 0.0f, MaxOffset()));
}

void ScrollView::To(float Offset, uint64_t Now, uint32_t DurMs) { Pos.Go(std::clamp(Offset, 0.0f, MaxOffset()), Now, DurMs); }

} // namespace ui
