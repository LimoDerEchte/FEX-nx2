// SPDX-License-Identifier: MIT
// One texture page with a CPU copy and shelf packing, shared by the glyph atlases (Alpha8) and the icon cache (Rgba8).
// Writes go to the CPU copy and mark a dirty rectangle; Flush uploads that rectangle with one UpdateTexture call. Only
// Renderer.h is used.
#pragma once

#include <cstdint>
#include <vector>

#include "render/Renderer.h"

namespace ui {

class AtlasPage {
public:
  AtlasPage(Renderer& R, int W, int H, TextureFormat Format, TextureFilter Filter);
  ~AtlasPage();
  AtlasPage(const AtlasPage&) = delete;
  AtlasPage& operator=(const AtlasPage&) = delete;

  bool Valid() const { return Tex != NoTexture; }
  TextureId Texture() const { return Tex; }
  int Width() const { return W; }
  int Height() const { return H; }

  // Reserves a W x H block (1 texel of padding on every side, so Nearest and Linear sampling never bleed).
  bool Allocate(int BlockW, int BlockH, int& X, int& Y);
  // Copies rows into the CPU copy; Src is tightly packed with SrcStride bytes per row.
  void Write(int X, int Y, int BlockW, int BlockH, const uint8_t* Src, int SrcStride);
  // Uploads the dirty rectangle (if any). Returns false when the renderer refused it (the rectangle stays dirty).
  bool Flush();

private:
  Renderer& R;
  TextureId Tex = NoTexture;
  int W, H, Bpp;
  std::vector<uint8_t> Pixels;
  int ShelfX = 1, ShelfY = 1, ShelfH = 0;
  int DirtyX0 = 0, DirtyY0 = 0, DirtyX1 = 0, DirtyY1 = 0;  // empty when X1 <= X0
};

} // namespace ui
