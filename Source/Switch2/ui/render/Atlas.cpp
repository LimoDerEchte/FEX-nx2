// SPDX-License-Identifier: MIT
#include "render/Atlas.h"

#include <algorithm>
#include <cstring>

namespace ui {

AtlasPage::AtlasPage(Renderer& R, int W, int H, TextureFormat Format, TextureFilter Filter)
    : R(R), W(W), H(H), Bpp(Format == TextureFormat::Alpha8 ? 1 : 4) {
  Tex = R.CreateTexture(W, H, Format, Filter);
  if (Tex != NoTexture)
    Pixels.assign(size_t(W) * size_t(H) * size_t(Bpp), 0);
}

AtlasPage::~AtlasPage() {
  if (Tex != NoTexture)
    R.DestroyTexture(Tex);
}

bool AtlasPage::Allocate(int BlockW, int BlockH, int& X, int& Y) {
  if (!Valid() || BlockW <= 0 || BlockH <= 0 || BlockW + 2 > W || BlockH + 2 > H)
    return false;
  if (ShelfX + BlockW + 1 > W) {  // next shelf
    ShelfY += ShelfH + 1;
    ShelfX = 1;
    ShelfH = 0;
  }
  if (ShelfY + BlockH + 1 > H)
    return false;
  X = ShelfX;
  Y = ShelfY;
  ShelfX += BlockW + 1;
  ShelfH = std::max(ShelfH, BlockH);
  return true;
}

void AtlasPage::Write(int X, int Y, int BlockW, int BlockH, const uint8_t* Src, int SrcStride) {
  if (!Valid() || X < 0 || Y < 0 || X + BlockW > W || Y + BlockH > H)
    return;
  for (int Row = 0; Row < BlockH; ++Row)
    std::memcpy(&Pixels[(size_t(Y + Row) * size_t(W) + size_t(X)) * size_t(Bpp)], Src + size_t(Row) * size_t(SrcStride), size_t(BlockW) * size_t(Bpp));
  if (DirtyX1 <= DirtyX0) {
    DirtyX0 = X;
    DirtyY0 = Y;
    DirtyX1 = X + BlockW;
    DirtyY1 = Y + BlockH;
  } else {
    DirtyX0 = std::min(DirtyX0, X);
    DirtyY0 = std::min(DirtyY0, Y);
    DirtyX1 = std::max(DirtyX1, X + BlockW);
    DirtyY1 = std::max(DirtyY1, Y + BlockH);
  }
}

bool AtlasPage::Flush() {
  if (!Valid() || DirtyX1 <= DirtyX0)
    return true;
  const size_t Stride = size_t(W) * size_t(Bpp);
  if (!R.UpdateTexture(Tex, DirtyX0, DirtyY0, DirtyX1 - DirtyX0, DirtyY1 - DirtyY0, &Pixels[size_t(DirtyY0) * Stride + size_t(DirtyX0) * size_t(Bpp)], Stride))
    return false;
  DirtyX0 = DirtyY0 = DirtyX1 = DirtyY1 = 0;
  return true;
}

} // namespace ui
