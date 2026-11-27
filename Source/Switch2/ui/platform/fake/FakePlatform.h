// SPDX-License-Identifier: MIT
// Fake backends for host tests: no GPU, no libnx. A settable display size, an event script, a fake clock, and a renderer
// that records its calls.
#pragma once

#include <deque>
#include <string>
#include <vector>

#include "platform/Platform.h"

namespace ui {

class FakeDisplay final : public Display {
public:
  explicit FakeDisplay(Size S = Size {1280, 720}) : Current(S) {}
  Size GetSize() const override { return Current; }
  uint32_t SizeSerial() const override { return Serial; }
  bool BeginFrame() override {
    ++Frames;
    return true;
  }
  bool Present() override {
    ++Presents;
    return true;
  }
  bool SetVsync(bool On) override { return On; }
  bool Vsync() const override { return true; }
  // Test control: dock/undock.
  void SetSize(Size S) {
    if (S != Current) {
      Current = S;
      ++Serial;
    }
  }
  int Frames = 0, Presents = 0;

private:
  Size Current;
  uint32_t Serial = 0;
};

class FakeInput final : public Input {
public:
  explicit FakeInput(Display& D) : D(D), LastSerial(D.SizeSerial()) {}
  void Push(const Event& E) { Queue.push_back(E); }
  void Poll(std::vector<Event>& Out) override {
    if (D.SizeSerial() != LastSerial) {
      LastSerial = D.SizeSerial();
      Event E;
      E.Kind = Event::Type::SizeChanged;
      E.W = D.GetSize().W;
      E.H = D.GetSize().H;
      Out.push_back(E);
    }
    while (!Queue.empty()) {
      Out.push_back(Queue.front());
      Queue.pop_front();
    }
  }
  bool HasMouse() const override { return true; }
  bool HasWheel() const override { return true; }

private:
  Display& D;
  uint32_t LastSerial;
  std::deque<Event> Queue;
};

class FakeSystem final : public System {
public:
  uint64_t Now = 0;
  uint64_t MonotonicMs() override { return Now; }
  bool WallClock(WallTime& Out) override {
    Out = WallTime {2026, 1, 1, 12, 0, 0};
    return true;
  }
  bool FreeSpace(const char*, uint64_t& Free, uint64_t& Total) override {
    Free = 1;
    Total = 2;
    return true;
  }
  bool NetworkAddress(std::string& S) override {
    S = "10.0.0.2";
    return true;
  }
  bool Battery(int& P, bool& C) override {
    P = 50;
    C = true;
    return true;
  }
  bool ShowKeyboard(const char*, std::string&) override { return false; }
  bool Running = true;  // test control: false = the console asked the app to close
  bool KeepRunning() override { return Running; }
};

// Records every call as one line, for example "FillRect 10 20 30 40 rgba(1,2,3,255)".
class FakeRenderer final : public Renderer {
public:
  explicit FakeRenderer(Display& D) : D(D) {}
  std::vector<std::string> Calls;
  Size TargetSize() const override { return D.GetSize(); }
  bool BeginFrame(Color C) override {
    Calls.push_back("BeginFrame " + Str(C));
    return true;
  }
  bool EndFrame() override {
    Calls.push_back("EndFrame");
    return true;
  }
  void FillRect(Rect R, Color C) override { Calls.push_back("FillRect " + Str(R) + " " + Str(C) + (Opacity() < 1.0f ? " op=" + std::to_string(Opacity()) : std::string())); }
  void FillRoundRect(Rect R, float Radius, Color C) override { Calls.push_back("FillRoundRect " + Str(R) + " r=" + std::to_string(Radius) + " " + Str(C)); }
  void StrokeRoundRect(Rect R, float Radius, float T, Color C) override {
    Calls.push_back("StrokeRoundRect " + Str(R) + " r=" + std::to_string(Radius) + " t=" + std::to_string(T) + " " + Str(C));
  }
  bool Line(float X0, float Y0, float X1, float Y1, float T, Color C) override {
    Calls.push_back("Line " + std::to_string(X0) + "," + std::to_string(Y0) + "," + std::to_string(X1) + "," + std::to_string(Y1) + " t=" + std::to_string(T) + " " + Str(C));
    return X0 == X1 || Y0 == Y1;
  }
  void Image(TextureId T, Rect Dst, Rect Src, Color Tint) override { Calls.push_back("Image " + std::to_string(T) + " " + Str(Dst) + " " + Str(Src) + " " + Str(Tint)); }
  void PushClip(Rect R) override { Calls.push_back("PushClip " + Str(R)); }
  void PopClip() override { Calls.push_back("PopClip"); }
  TextureId CreateTexture(int W, int H, TextureFormat, TextureFilter) override {
    Calls.push_back("CreateTexture " + std::to_string(W) + "x" + std::to_string(H));
    return Next++;
  }
  bool UpdateTexture(TextureId T, int, int, int, int, const void*, size_t) override {
    Calls.push_back("UpdateTexture " + std::to_string(T));
    return true;
  }
  void DestroyTexture(TextureId T) override { Calls.push_back("DestroyTexture " + std::to_string(T)); }

private:
  static std::string Str(Color C) {
    return "rgba(" + std::to_string(C.R) + "," + std::to_string(C.G) + "," + std::to_string(C.B) + "," + std::to_string(C.A) + ")";
  }
  static std::string Str(Rect R) {
    return std::to_string(R.X) + " " + std::to_string(R.Y) + " " + std::to_string(R.W) + " " + std::to_string(R.H);
  }
  Display& D;
  TextureId Next = 0;
};

// Display, input, system and a recording renderer as one platform (the app shell tests).
class FakePlatform final : public Platform {
public:
  explicit FakePlatform(Size S = Size {1280, 720}) : Disp(S), In(Disp), Rend(Disp) {}
  Display& GetDisplay() override { return Disp; }
  Input& GetInput() override { return In; }
  System& GetSystem() override { return Sys; }
  Renderer& GetRenderer() override { return Rend; }
  FakeDisplay Disp;
  FakeInput In;
  FakeSystem Sys;
  FakeRenderer Rend;
};

} // namespace ui
