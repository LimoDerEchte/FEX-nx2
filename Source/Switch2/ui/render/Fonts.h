// SPDX-License-Identifier: MIT
// Text: FreeType rasterizing and HarfBuzz shaping on the embedded fonts, one glyph atlas per (font, size x s) uploaded as
// Renderer textures (Alpha8 coverage, so atlases are color independent: the color is the draw tint), text measure and
// ellipsis. Uses only Renderer.h. Sizes are design units; pass the output scale s (Display::Scale()) to every call and all
// results are device pixels. Glyphs are rasterized at size x s, never scaled.
//
// Frame protocol: call Fonts::EndFrame() once per frame before Renderer::EndFrame() (it uploads the glyphs added this
// frame). The Renderer must outlive the Fonts object.
#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "assets/Assets.h"
#include "render/Renderer.h"

namespace ui {

// One entry of the spec's typography table. Size in du, Tracking in du added after every glyph, LineHeight a multiplier
// of the size (0 = the font's own line height).
struct TextStyle {
  const char* Name;
  FontId Font;
  float Size;
  float Tracking = 0;
  float LineHeight = 0;
  bool Upper = false;
  bool Italic = false;
};

namespace styles {
inline constexpr TextStyle Display {"display", FontId::NunitoExtraBold, 40, -0.5f};
inline constexpr TextStyle TitleInstall {"title_install", FontId::NunitoExtraBold, 36};
inline constexpr TextStyle TitleSettings {"title_settings", FontId::NunitoExtraBold, 28};
inline constexpr TextStyle CardLabel {"card_label", FontId::NunitoBold, 22};
inline constexpr TextStyle RowLabel {"row_label", FontId::NunitoBold, 18};
inline constexpr TextStyle Body {"body", FontId::NunitoSemiBold, 16};
inline constexpr TextStyle BodyRegular {"body_regular", FontId::NunitoRegular, 16};  // subtitles (400)
inline constexpr TextStyle Caption {"caption", FontId::NunitoRegular, 14};
inline constexpr TextStyle Section {"section", FontId::NunitoBold, 14, 0.6f, 0, true};
inline constexpr TextStyle Meta {"meta", FontId::NunitoSemiBold, 13};
inline constexpr TextStyle MonoUi {"mono_ui", FontId::MonoSemiBold, 15};
inline constexpr TextStyle MonoUiSmall {"mono_ui_small", FontId::MonoRegular, 13};  // path values
inline constexpr TextStyle MonoTerm {"mono_term", FontId::MonoRegular, 15, 0, 1.55f};
inline constexpr TextStyle MonoTermBold {"mono_term_bold", FontId::MonoBold, 15, 0, 1.55f};
inline constexpr TextStyle MonoLog {"mono_log", FontId::MonoRegular, 13, 0, 1.7f};
inline constexpr TextStyle KdeTab {"kde_tab", FontId::NotoRegular, 13};

inline constexpr TextStyle All[] = {Display, TitleInstall, TitleSettings, CardLabel, RowLabel, Body, BodyRegular, Caption,
                                    Section, Meta, MonoUi, MonoUiSmall, MonoTerm, MonoTermBold, MonoLog, KdeTab};
} // namespace styles

// Same font and size as Base with another size or tracking: for the spec's one-off sizes (18/700 values, 34/600 address).
constexpr TextStyle WithSize(TextStyle Base, float Size, float Tracking = 0) {
  Base.Size = Size;
  Base.Tracking = Tracking;
  return Base;
}

enum class Align : uint8_t { Left, Center, Right };
enum class Ellipsis : uint8_t { End, Middle };

struct LineMetrics {
  float Ascent = 0;   // above the baseline, device px
  float Descent = 0;  // below the baseline, positive
  float Gap = 0;      // extra leading of the font
  float Height() const { return Ascent + Descent + Gap; }
};

struct FontsStats {
  int Atlases = 0;     // (font, size) atlases alive
  int Pages = 0;       // texture pages alive
  int Glyphs = 0;      // glyphs rasterized and alive
  int Created = 0;     // atlases created since start (never decreases)
  int Evicted = 0;     // atlases dropped because they were unused
};

class Fonts {
public:
  explicit Fonts(Renderer& R);
  ~Fonts();
  Fonts(const Fonts&) = delete;
  Fonts& operator=(const Fonts&) = delete;

  bool Ok() const;  // FreeType started

  // Width of the text in device pixels (the pen advance incl. tracking between glyphs, not after the last one).
  float Measure(const TextStyle& St, std::string_view Utf8, float S);
  LineMetrics Metrics(const TextStyle& St, float S);
  // The spec's line height in whole device pixels: LineHeight x Size x S when the style has a multiplier, else the font's.
  float LineHeightPx(const TextStyle& St, float S);
  // Baseline (whole pixel) that centers the font's ascent+descent in a box starting at BoxTop with height BoxH.
  float BaselineInBox(const TextStyle& St, float S, float BoxTop, float BoxH);
  // Advance of one glyph of a monospaced style, for example the terminal cell width.
  float CellWidth(const TextStyle& St, float S) { return Measure(St, "M", S); }

  // Draws one line. X is the left, center or right anchor (device px), Baseline the baseline (rounded to a pixel).
  // Returns the width. Glyph quads are placed on whole pixels.
  float Draw(const TextStyle& St, std::string_view Utf8, float S, float X, float Baseline, Color C, Align A = Align::Left);

  // The text cut to fit MaxWidth device px, with "…" (U+2026) replacing the removed part; unchanged when it fits; empty
  // when not even the "…" fits. Cuts at code point boundaries.
  std::string Fit(const TextStyle& St, std::string_view Utf8, float S, float MaxWidth, Ellipsis Mode);

  // Uploads this frame's new glyphs and drops atlases unused for a long time. Call before Renderer::EndFrame().
  void EndFrame();
  FontsStats Stats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> P;
};

} // namespace ui
