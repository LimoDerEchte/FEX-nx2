// SPDX-License-Identifier: MIT
#include "render/Icons.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#define NANOSVG_IMPLEMENTATION
#include "ThirdParty/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "ThirdParty/nanosvg/nanosvgrast.h"
#pragma GCC diagnostic pop

#include "platform/Log.h"
#include "render/Atlas.h"

namespace ui {
namespace {

struct Slot {
  int Page = -1;
  int X = 0, Y = 0, Size = 0;
};

void Replace(std::string& S, const std::string& From, const std::string& To) {
  const size_t Pos = S.find(From);
  if (Pos != std::string::npos)
    S.replace(Pos, From.size(), To);
}

} // namespace

struct Icons::Impl {
  explicit Impl(Renderer& R) : R(R) { Raster = nsvgCreateRasterizer(); }
  ~Impl() {
    Pages.clear();
    if (Raster)
      nsvgDeleteRasterizer(Raster);
  }

  bool Rasterize(IconId Id, int Px, float Stroke, std::vector<uint8_t>& Out) {
    const Blob B = GetIcon(Id);
    if (!B.Data || !Raster)
      return false;
    std::string Svg(reinterpret_cast<const char*>(B.Data), B.Size);
    Replace(Svg, "currentColor", "white");
    char Width[48];
    std::snprintf(Width, sizeof(Width), "stroke-width=\"%.3f\"", double(Stroke));
    Replace(Svg, "stroke-width=\"2\"", Width);
    NSVGimage* Image = nsvgParse(Svg.data(), "px", 96.0f);
    if (!Image) {
      Log("FAIL icons: nanosvg cannot parse %s", IconName(Id));
      return false;
    }
    Out.assign(size_t(Px) * size_t(Px) * 4, 0);
    nsvgRasterize(Raster, Image, 0, 0, float(Px) / (Image->width > 0 ? Image->width : 24.0f), Out.data(), Px, Px, Px * 4);
    nsvgDelete(Image);
    return true;
  }

  Renderer& R;
  NSVGrasterizer* Raster = nullptr;
  std::vector<std::unique_ptr<AtlasPage>> Pages;
  std::unordered_map<uint64_t, Slot> Cache;
  int Rasterized = 0;
};

Icons::Icons(Renderer& R) : P(std::make_unique<Impl>(R)) {}
Icons::~Icons() = default;

bool Icons::Draw(IconId Id, Rect Dst, Color Tint, float StrokeWidth) {
  const int Px = std::max(1, int(std::lround(std::min(Dst.W, Dst.H))));
  const uint64_t Key = (uint64_t(Id) << 48) | (uint64_t(uint32_t(std::lround(double(StrokeWidth) * 100.0))) << 24) | uint64_t(Px);
  auto It = P->Cache.find(Key);
  if (It == P->Cache.end()) {
    Slot S;
    S.Size = Px;
    std::vector<uint8_t> Pixels;
    if (Px + 2 <= 512 && P->Rasterize(Id, Px, StrokeWidth, Pixels)) {
      int X = 0, Y = 0;
      if (!P->Pages.empty() && P->Pages.back()->Allocate(Px, Px, X, Y)) {
        S.Page = int(P->Pages.size()) - 1;
      } else {
        auto NewPage = std::make_unique<AtlasPage>(P->R, 512, 512, TextureFormat::Rgba8, TextureFilter::Nearest);
        if (NewPage->Valid() && NewPage->Allocate(Px, Px, X, Y)) {
          P->Pages.push_back(std::move(NewPage));
          S.Page = int(P->Pages.size()) - 1;
        }
      }
      if (S.Page >= 0) {
        P->Pages[size_t(S.Page)]->Write(X, Y, Px, Px, Pixels.data(), Px * 4);
        S.X = X;
        S.Y = Y;
        ++P->Rasterized;
      }
    }
    It = P->Cache.emplace(Key, S).first;
  }
  const Slot& S = It->second;
  if (S.Page < 0)
    return false;
  const float Cx = Dst.X + Dst.W * 0.5f, Cy = Dst.Y + Dst.H * 0.5f;
  P->R.Image(P->Pages[size_t(S.Page)]->Texture(), Rect {std::round(Cx - float(S.Size) * 0.5f), std::round(Cy - float(S.Size) * 0.5f), float(S.Size), float(S.Size)},
             Rect {float(S.X), float(S.Y), float(S.Size), float(S.Size)}, Tint);
  return true;
}

void Icons::EndFrame() {
  for (auto& Page : P->Pages)
    Page->Flush();
}

IconsStats Icons::Stats() const {
  IconsStats S;
  S.Pages = int(P->Pages.size());
  S.Rasterized = P->Rasterized;
  for (const auto& E : P->Cache)
    S.Cached += E.second.Page >= 0 ? 1 : 0;
  return S;
}

} // namespace ui
