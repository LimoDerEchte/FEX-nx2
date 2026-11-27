// SPDX-License-Identifier: MIT
// ProgressBar (12 high, fill eases over 200 ms, never backwards) and LogView (2,000-line ring buffer, bottom-anchored,
// block cursor blinking at 530 ms).
#pragma once

#include <deque>

#include "components/Widget.h"

namespace ui {

class ProgressBar final : public Widget {
public:
  ProgressBar() { Focusable = false; Hittable = false; Bounds.H = 12; }
  // Fraction 0..1. A value lower than the current target is ignored: the bar never moves backwards (Reset starts a new job).
  void SetValue(float V, uint64_t Now);
  void Reset(uint64_t Now);
  float Target() const { return Fill.To; }
  float Displayed(uint64_t Now) const { return Fill.Value(Now); }
  void Draw(const Frame& F) override;

private:
  Tween Fill;
};

class LogView final : public Widget {
public:
  enum class Kind : uint8_t { Normal, Dim, Ok, Warn, Error };
  static constexpr size_t MaxLines = 2000;

  LogView() { Focusable = false; Hittable = false; }
  void Append(std::string Line, Kind K = Kind::Normal);
  // A line of colored parts (the installation log: dim stamp, "ok"/"warn:"/"error:" prefix, text).
  struct Span {
    Kind K = Kind::Normal;
    std::string Text;
  };
  void AppendSpans(std::vector<Span> Parts);
  void Clear() {
    Lines.clear();
    DroppedLines = 0;
    FromBottom = 0;
  }
  size_t LineCount() const { return Lines.size(); }
  const std::string& Line(size_t I) const { return Lines[I].Text; }
  uint64_t Dropped() const { return DroppedLines; }
  // Scroll by du (positive = towards the newest line). Reaching the bottom resumes following the newest line.
  void ScrollBy(float Dy);
  bool Following() const { return FromBottom <= 0; }
  void Draw(const Frame& F) override;

private:
  struct Entry {
    std::string Text;
    Kind K;
    std::vector<Span> Parts;  // when set, Text is the concatenation and the parts are drawn in their own colors
  };
  std::deque<Entry> Lines;
  uint64_t DroppedLines = 0;
  float FromBottom = 0;  // du above the bottom-anchored position; 0 = following
  float LineHeightDu = 22;
};

} // namespace ui
