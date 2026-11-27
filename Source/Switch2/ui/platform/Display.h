// SPDX-License-Identifier: MIT
// Output surface: size, scale, frame begin and present, vsync. Console backend: default NWindow and a FIFO swapchain.
// Host backend: headless offscreen image. No Vulkan or libnx types here.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ui {

struct Size {
  int W = 0;
  int H = 0;
  bool operator==(const Size& O) const { return W == O.W && H == O.H; }
  bool operator!=(const Size& O) const { return !(*this == O); }
};

// Design units (du) are laid out on 1280x720; s = output_height / 720.
constexpr float DesignHeight = 720.0f;
inline float ScaleForHeight(int Height) { return float(Height) / DesignHeight; }
// 1 du hairline in device pixels: max(1, round(s)).
inline int HairlinePx(float Scale) { return std::max(1, int(std::lround(Scale))); }

class Display {
public:
  virtual ~Display() = default;

  // Current output size in device pixels (1920x1080 handheld, up to 3840x2160 docked on the console).
  virtual Size GetSize() const = 0;
  // s = GetSize().H / 720.
  float Scale() const { return ScaleForHeight(GetSize().H); }
  // Increments every time the output size changed (dock, undock, host resize). Input backends turn it into SizeChanged events.
  virtual uint32_t SizeSerial() const = 0;

  // Frame protocol: Display::BeginFrame, Renderer::BeginFrame, draw, Renderer::EndFrame, Display::Present.
  // BeginFrame waits for the previous frame and applies a pending size change. false = the frame cannot be drawn.
  virtual bool BeginFrame() = 0;
  // Presents the frame the renderer submitted in EndFrame. false = it could not be presented.
  virtual bool Present() = 0;

  // vsync 60 fps. The console swapchain is FIFO only: SetVsync(false) returns false and vsync stays on.
  virtual bool SetVsync(bool On) = 0;
  virtual bool Vsync() const = 0;

  // Reads the last presented frame back as tightly packed RGBA8 (host headless backend only; the console returns false).
  virtual bool ReadPixels(std::vector<uint8_t>& Rgba, Size& OutSize) {
    (void)Rgba;
    (void)OutSize;
    return false;
  }
};

} // namespace ui
