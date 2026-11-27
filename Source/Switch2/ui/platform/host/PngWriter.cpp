// SPDX-License-Identifier: MIT
#include "platform/host/PngWriter.h"

#include <zlib.h>

#include <cstdio>

namespace ui {
namespace {
void PutBe32(std::vector<uint8_t>& V, uint32_t X) {
  for (int I = 3; I >= 0; --I)
    V.push_back(uint8_t(X >> (I * 8)));
}

void Chunk(std::vector<uint8_t>& Out, const char* Type, const std::vector<uint8_t>& Data) {
  PutBe32(Out, uint32_t(Data.size()));
  std::vector<uint8_t> Body(Type, Type + 4);
  Body.insert(Body.end(), Data.begin(), Data.end());
  Out.insert(Out.end(), Body.begin(), Body.end());
  PutBe32(Out, uint32_t(crc32(0, Body.data(), uInt(Body.size()))));
}

} // namespace

bool WritePng(const char* Path, const std::vector<uint8_t>& Rgba, int W, int H) {
  std::vector<uint8_t> Raw;
  Raw.reserve(size_t(H) * (size_t(W) * 4 + 1));
  for (int Y = 0; Y < H; ++Y) {
    Raw.push_back(0);
    Raw.insert(Raw.end(), Rgba.begin() + long(size_t(Y) * size_t(W) * 4), Rgba.begin() + long(size_t(Y + 1) * size_t(W) * 4));
  }
  uLongf Bound = compressBound(uLong(Raw.size()));
  std::vector<uint8_t> Z(Bound);
  if (compress2(Z.data(), &Bound, Raw.data(), uLong(Raw.size()), 6) != Z_OK)
    return false;
  Z.resize(Bound);
  std::vector<uint8_t> Out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  std::vector<uint8_t> Ihdr;
  PutBe32(Ihdr, uint32_t(W));
  PutBe32(Ihdr, uint32_t(H));
  Ihdr.insert(Ihdr.end(), {8, 6, 0, 0, 0});
  Chunk(Out, "IHDR", Ihdr);
  Chunk(Out, "IDAT", Z);
  Chunk(Out, "IEND", {});
  FILE* F = std::fopen(Path, "wb");
  if (!F)
    return false;
  bool Ok = std::fwrite(Out.data(), 1, Out.size(), F) == Out.size();
  return std::fclose(F) == 0 && Ok;
}

} // namespace ui
