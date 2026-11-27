// SPDX-License-Identifier: MIT
// Time-based animation helpers (ms). Animations never count frames.
#pragma once

#include <algorithm>
#include <cstdint>

namespace ui {

inline float Clamp01(float V) { return V < 0 ? 0 : V > 1 ? 1 : V; }
// Cubic ease-out: fast start, slow end.
inline float EaseOut(float T) {
  T = Clamp01(T);
  const float U = 1.0f - T;
  return 1.0f - U * U * U;
}

// A value moving from From to To over Dur ms starting at Start (ease-out). Dur 0 = immediate.
struct Tween {
  float From = 0, To = 0;
  uint64_t Start = 0;
  uint32_t Dur = 0;

  void Set(float V) {
    From = To = V;
    Dur = 0;
  }
  float Value(uint64_t Now) const {
    if (Dur == 0 || Now >= Start + Dur)
      return To;
    if (Now <= Start)
      return From;
    return From + (To - From) * EaseOut(float(Now - Start) / float(Dur));
  }
  // Starts a move from the current value to V. Does nothing when already heading to V.
  void Go(float V, uint64_t Now, uint32_t DurMs) {
    if (V == To)
      return;
    From = Value(Now);
    To = V;
    Start = Now;
    Dur = DurMs;
  }
  bool Active(uint64_t Now) const { return Dur != 0 && Now < Start + Dur; }
};

} // namespace ui
