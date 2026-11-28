// SPDX-License-Identifier: ISC
// Copyright 2026 nx-hbmenu Switch 2 port contributors (swapchain and surface setup adapted from ounce-hbmenu
// switch2/graphics_vulkan.c; touch handling from nx_main/nx_touch.c).
//
// Console backend: Display on the default NWindow with a FIFO swapchain (recreated on a size change), Input over libnx hid,
// System (clock only in Task 1) and the shared Vulkan renderer.
#define VK_USE_PLATFORM_VI_NN 1
#include <vulkan/vulkan.h>

#include <switch.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "GuestInputFeed.h"
#include "platform/HidKeyMap.h"
#include "platform/Log.h"
#include "platform/Platform.h"
#include "platform/switch2/Switch2Platform.h"
#include "render/vulkan/Target.h"
#include "render/vulkan/VulkanRenderer.h"

namespace ui {
namespace {

using namespace Switch2Guest;

uint64_t NowMs() {
  using namespace std::chrono;
  return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// ---- Display --------------------------------------------------------------

class Switch2Target final : public vk::Target {
public:
  ~Switch2Target() override { Close(); }
  VkSurfaceKHR Surface = VK_NULL_HANDLE;
  VkSwapchainKHR Swapchain = VK_NULL_HANDLE;

protected:
  const char* AppName() const override { return "fex-switch2-ui"; }
  std::vector<const char*> InstanceExtensions() const override { return {VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME}; }
  std::vector<const char*> DeviceExtensions() const override { return {VK_KHR_SWAPCHAIN_EXTENSION_NAME}; }
  int ScoreDevice(const VkPhysicalDeviceProperties&, uint32_t Index, uint32_t) const override { return 100 - int(Index); }
  bool UsesSemaphores() const override { return true; }
  VkImageLayout FinalLayout() const override { return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; }

  bool CreateImages() override {
    NWindow* Window = nwindowGetDefault();
    if (!Surface) {
      u32 Wd = 0, Ht = 0;
      Result Rc = nwindowGetDimensions(Window, &Wd, &Ht);
      Log("step nwindowGetDimensions: rc=0x%x %ux%u (userAppInit ran: %s)", unsigned(Rc), Wd, Ht, WindowPrepared ? "yes" : "NO");
      VkViSurfaceCreateInfoNN SurfaceInfo {};
      SurfaceInfo.sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN;
      SurfaceInfo.window = Window;
      if (!vk::Step("vkCreateViSurfaceNN", vkCreateViSurfaceNN(Instance, &SurfaceInfo, nullptr, &Surface)) || !Surface)
        return false;
      VkSurfaceFormatKHR Formats[16];
      uint32_t Count = 16;
      VkResult R = vkGetPhysicalDeviceSurfaceFormatsKHR(Physical, Surface, &Count, Formats);
      if (R != VK_SUCCESS && R != VK_INCOMPLETE) {
        vk::Step("vkGetPhysicalDeviceSurfaceFormatsKHR", R);
        return false;
      }
      Format = VK_FORMAT_UNDEFINED;
      for (uint32_t I = 0; I < Count; ++I) {
        Log("  surface format %u: format=%d colorspace=%d", I, int(Formats[I].format), int(Formats[I].colorSpace));
        // Draw in stored byte values: no sRGB conversion (same as ounce-hbmenu).
        if (Formats[I].format == VK_FORMAT_R8G8B8A8_UNORM && Formats[I].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
          Format = Formats[I].format;
      }
      if (Format == VK_FORMAT_UNDEFINED) {
        Log("FAIL surface format: R8G8B8A8_UNORM / SRGB_NONLINEAR not offered (%u formats)", Count);
        return false;
      }
      Log("step surface format: ok (R8G8B8A8_UNORM)");
    }
    VkSurfaceCapabilitiesKHR Caps {};
    VkResult R = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(Physical, Surface, &Caps);
    if (!vk::Step("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", R))
      return false;
    Log("  caps: minImages=%u maxImages=%u current=%ux%u usage=0x%x", Caps.minImageCount, Caps.maxImageCount, Caps.currentExtent.width,
        Caps.currentExtent.height, unsigned(Caps.supportedUsageFlags));
    if (Caps.minImageCount == 0 || Caps.minImageCount > 16 || !Caps.currentExtent.width || Caps.currentExtent.width == UINT32_MAX ||
        !(Caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
      Log("FAIL surface capabilities: unusable");
      return false;
    }
    VkSwapchainCreateInfoKHR Info {};
    Info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    Info.surface = Surface;
    Info.minImageCount = Caps.minImageCount;
    Info.imageFormat = Format;
    Info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    Info.imageExtent = Caps.currentExtent;
    Info.imageArrayLayers = 1;
    Info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    Info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    Info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    Info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    Info.clipped = VK_TRUE;
    Info.oldSwapchain = Swapchain;
    VkSwapchainKHR New = VK_NULL_HANDLE;
    R = vkCreateSwapchainKHR(Device, &Info, nullptr, &New);
    if (Swapchain)
      vkDestroySwapchainKHR(Device, Swapchain, nullptr);
    Swapchain = New;
    if (!vk::Step("vkCreateSwapchainKHR(FIFO)", R))
      return false;
    uint32_t Count = 16;
    Images.assign(16, VK_NULL_HANDLE);
    R = vkGetSwapchainImagesKHR(Device, Swapchain, &Count, Images.data());
    if (!vk::Step("vkGetSwapchainImagesKHR", R) || Count < Caps.minImageCount)
      return false;
    Images.resize(Count);
    Extent = Caps.currentExtent;
    Log("  swapchain: %u images at %ux%u", Count, Extent.width, Extent.height);
    return true;
  }

  void DestroyImages() override {
    if (Swapchain)
      vkDestroySwapchainKHR(Device, Swapchain, nullptr);
    if (Surface)
      vkDestroySurfaceKHR(Instance, Surface, nullptr);
    Swapchain = VK_NULL_HANDLE;
    Surface = VK_NULL_HANDLE;
    Images.clear();
  }

  VkResult DoAcquire(uint32_t& Index) override { return vkAcquireNextImageKHR(Device, Swapchain, UINT64_MAX, Acquired, VK_NULL_HANDLE, &Index); }

  VkResult DoPresent(uint32_t Index) override {
    VkPresentInfoKHR Info {};
    Info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    Info.waitSemaphoreCount = 1;
    Info.pWaitSemaphores = &Ready[Index];
    Info.swapchainCount = 1;
    Info.pSwapchains = &Swapchain;
    Info.pImageIndices = &Index;
    return vkQueuePresentKHR(Queue, &Info);
  }
};

class Switch2Display final : public Display {
public:
  bool Open() { return Tgt.Open(); }
  Size GetSize() const override { return Size {int(Tgt.Extent.width), int(Tgt.Extent.height)}; }
  uint32_t SizeSerial() const override { return Tgt.SizeSerial; }
  bool BeginFrame() override { return Tgt.BeginFrame(); }
  bool Present() override { return Tgt.PresentImage(); }
  bool SetVsync(bool On) override { return On; }  // FIFO only
  bool Vsync() const override { return true; }
  Switch2Target Tgt;
};

// ---- Input ----------------------------------------------------------------

// Joy-Con 2 mouse and USB mouse through the Switch 2 Nmouse service (libounce switch2/services/nmouse.h). Written from the
// SDK header and from the behaviour described for Joy-Con "stylus" use (one Joy-Con is followed until it has been still for
// 3 s; its deltas are summed here; samples the sensor marks as not tracking are skipped; the click is the Joy-Con's own
// ZL/ZR): no code is copied from other projects. The SDK's PadPointer helper only supplies the list of Joy-Con handles.
class Switch2Input final : public Input {
public:
  explicit Switch2Input(Display& D) : D(D), LastSerial(D.SizeSerial()) {
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    for (int I = 0; I < 9; ++I) {
      u64 Id = I < 8 ? u64(HidNpadIdType_No1) + u64(I) : u64(HidNpadIdType_Handheld);
      padInitializeWithMask(&Pads[I], BITL(Id));
    }
    // Handheld controls stay active while a pair of Joy-Con is docked (same as hbmenu).
    hidSetNpadHandheldActivationMode(HidNpadHandheldActivationMode_Single);
    hidInitializeTouchScreen();
    hidInitializeKeyboard();
    // Nmouse first: padPointerInitialize starts it (hidInitializeNmouse) with the pointer area given here. The deltas are
    // summed by this class, so the area only sets the unit of a count: 1280 x 720.
    NmouseRc = padPointerInitialize(&Pointers[0], AreaW, AreaH);
    NmouseOk = R_SUCCEEDED(NmouseRc);
    for (int I = 1; I < 9 && NmouseOk; ++I)
      padPointerInitialize(&Pointers[I], AreaW, AreaH);
    Log("step nmouse: padPointerInitialize(%dx%d) -> %s (rc=0x%x)", AreaW, AreaH, NmouseOk ? "ok" : "FAIL", unsigned(NmouseRc));
    if (!NmouseOk) {
      // Fallback: the ordinary libnx mouse (USB mouse on firmware that still reports it).
      hidInitializeMouse();
      Log("step nmouse fallback: hidInitializeMouse called (Joy-Con mouse unavailable)");
    }
    LoadGainFile();
    PX = 640;
    PY = 360;
    Log("step input init: ok (9 pads, touch, %s, pointer gain %.2f)", NmouseOk ? "nmouse" : "legacy mouse", double(Gain));
  }
  ~Switch2Input() override {
    for (auto& Pad : Vibration)
      SendVibration(Pad, 0, 0);
    if (NmouseOk)
      for (int I = 8; I >= 0; --I)
        padPointerExit(&Pointers[I]);
  }

  bool HasMouse() const override { return MouseSeen; }
  bool HasWheel() const override { return MouseSeen; }
  float PointerGain() const override { return Gain; }
  void SetPointerGain(float G) override { Gain = G < 0.05f ? 0.05f : G > 20.0f ? 20.0f : G; }

  std::string MouseStatus() const override {
    char Buf[256];
    std::snprintf(Buf, sizeof(Buf), "nmouse %s (rc 0x%x) devs %d active %s  raw dx %lld dy %lld  wheel %lld  samples %llu skipped %llu  gain %.2f",
                  NmouseOk ? "ok" : "FAIL", unsigned(NmouseRc), SeenDevices, ActiveName(), (long long)RawDx, (long long)RawDy, (long long)RawWheel,
                  (unsigned long long)Samples, (unsigned long long)Skipped, double(Gain));
    return Buf;
  }

  void Poll(std::vector<Event>& Out) override {
    const uint64_t Now = NowMs();
    FrameNs = armTicksToNs(armGetSystemTick());
    if (D.SizeSerial() != LastSerial) {
      LastSerial = D.SizeSerial();
      Event E;
      E.Kind = Event::Type::SizeChanged;
      E.TimeMs = Now;
      E.W = D.GetSize().W;
      E.H = D.GetSize().H;
      Out.push_back(E);
    }
    for (int I = 0; I < 9; ++I)
      padUpdate(&Pads[I]);
    PublishPads();
    PollRumble();
    const bool OwnsPads = InputFeed().IsOwned(InputClass::Pad);
    if (OwnsPads && !PadsOwned)
      Out.push_back({.Kind = Event::Type::PadCancel, .TimeMs = Now});
    PadsOwned = OwnsPads;
    MouseOwned = InputFeed().IsOwned(InputClass::Mouse);
    if (MouseOwned)
      for (size_t I = 0; I < MouseOut.size(); ++I)
        EmitButton(Out, Now, static_cast<MouseButton>(I), false, true);
    std::fill(std::begin(SuppressMask), std::end(SuppressMask), u64(0));
    if (NmouseOk)
      PollNmouse(Out, Now);
    else
      PollLegacyMouse(Out, Now);
    PollPads(Out, Now);
    PollTouch(Out, Now);
    PollKeyboard(Out, Now);
  }

private:
  struct PlayingRumble {
    Evdev::RumbleRequest Request;
    uint64_t StartNs;
  };

  struct PadVibration {
    std::array<std::optional<PlayingRumble>, 16> Effects;
    HidVibrationDeviceHandle Handles[2] {};
    int Count {};
    uint32_t Strong {}, Weak {};
  };

  uint64_t FrameNs {};
  std::array<int, InputPadCount> FeedPads {};
  std::array<uint32_t, InputPadCount> FeedStyles {};
  std::array<uint64_t, InputPadCount> FeedConnections {};
  std::array<PadVibration, InputPadCount> Vibration {};
  uint64_t ReleasedGeneration {};

  static void SendVibration(PadVibration& Pad, uint32_t Strong, uint32_t Weak) {
    if (!Pad.Count || (Pad.Strong == Strong && Pad.Weak == Weak))
      return;
    const HidVibrationValue Value {float(Strong) / 65535.0f, 160.0f, float(Weak) / 65535.0f, 320.0f};
    const HidVibrationValue Values[] {Value, Value};
    if (R_SUCCEEDED(hidSendVibrationValues(Pad.Handles, Values, Pad.Count))) {
      Pad.Strong = Strong;
      Pad.Weak = Weak;
    }
  }

  void PublishPads() {
    int HandheldSlot = -1;
    if (padIsConnected(&Pads[8])) {
      for (size_t I = 0; I < InputPadCount; ++I) {
        if (!padIsConnected(&Pads[I]) && (HandheldSlot < 0 || FeedPads[I] == 8))
          HandheldSlot = int(I);
      }
    }
    for (size_t I = 0; I < InputPadCount; ++I) {
      const int Physical = int(I) == HandheldSlot ? 8 : int(I);
      const auto& Pad = Pads[Physical];
      const bool Connected = padIsConnected(&Pad);
      const uint32_t Style = Connected ? padGetStyleSet(&Pad) : 0;
      if (FeedStyles[I] && (FeedPads[I] != Physical || FeedStyles[I] != Style))
        InputFeed().PublishPad(I, {}, FrameNs);
      FeedPads[I] = Physical;
      FeedStyles[I] = Style;
      const auto Left = padGetStickPos(&Pad, 0), Right = padGetStickPos(&Pad, 1);
      const auto Connection = InputFeed().PublishPad(
        I, {padGetButtons(&Pad), Left.x, Left.y, Right.x, Right.y, Style, Connected}, FrameNs);
      if (FeedConnections[I] != Connection) {
        SendVibration(Vibration[I], 0, 0);
        Vibration[I] = {};
        FeedConnections[I] = Connection;
      }
    }
  }

  void PollRumble() {
    const uint64_t Generation = InputFeed().ReleasedGeneration();
    const auto Requests = InputFeed().TakeRumble();
    if (Generation != ReleasedGeneration) {
      for (auto& Pad : Vibration)
        Pad.Effects = {};
      ReleasedGeneration = Generation;
    }
    for (const auto& [Index, Request] : Requests) {
      if (!FeedStyles[Index] || Request.EffectId < 0 || Request.EffectId >= int(Vibration[Index].Effects.size()))
        continue;
      auto& Pad = Vibration[Index];
      auto& Effect = Pad.Effects[Request.EffectId];
      if (Request.Stop) {
        Effect.reset();
        continue;
      }
      if (!Pad.Count) {
        const int Physical = FeedPads[Index];
        const auto Id = Physical == 8 ? HidNpadIdType_Handheld : static_cast<HidNpadIdType>(Physical);
        const auto Style = static_cast<HidNpadStyleTag>(FeedStyles[Index]);
        const int Count = Style == HidNpadStyleTag_NpadJoyLeft || Style == HidNpadStyleTag_NpadJoyRight ? 1 : 2;
        if (R_FAILED(hidInitializeVibrationDevices(Pad.Handles, Count, Id, Style)))
          continue;
        Pad.Count = Count;
      }
      Effect = PlayingRumble {Request, FrameNs};
    }
    for (auto& Pad : Vibration) {
      uint32_t Strong = 0, Weak = 0;
      for (auto& Effect : Pad.Effects) {
        if (!Effect)
          continue;
        const auto& Request = Effect->Request;
        const uint64_t ElapsedMs = (FrameNs - Effect->StartNs) / 1'000'000;
        const uint32_t ReplayMs = uint32_t {Request.DelayMs} + Request.DurationMs;
        if (Request.DurationMs && ElapsedMs / ReplayMs >= Request.PlayCount) {
          Effect.reset();
          continue;
        }
        const auto [StrongSample, WeakSample] = Evdev::RumbleAt(Request, Request.DurationMs ? ElapsedMs % ReplayMs : ElapsedMs);
        Strong += StrongSample;
        Weak += WeakSample;
      }
      SendVibration(Pad, std::min(Strong, 65535u), std::min(Weak, 65535u));
    }
  }

  static bool KeyboardEvent(const HidKeyboardState& State, int Usage, uint64_t Now, Event& E) {
    const auto Mods = State.modifiers;
    // The mapping of USB HID usages to events is host code (platform/HidKeyMap.h) so that the unit tests cover it.
    const bool Mapped = MapHidKeyboard(Usage, Mods & HidKeyboardModifier_Shift, Mods & (HidKeyboardModifier_LeftAlt | HidKeyboardModifier_RightAlt),
                                       Mods & HidKeyboardModifier_Control, Mods & HidKeyboardModifier_CapsLock, E);
    E.TimeMs = Now;
    // Diagnostic (kept: rare keys only): which usage became which key, so a console log shows what the keyboard really sent.
    if (Mapped && E.KeyboardKey != Key::Text && E.KeyboardKey != Key::Enter && E.KeyboardKey != Key::Backspace && E.KeyboardKey != Key::Tab &&
        E.KeyboardKey != Key::Escape && !(E.KeyboardKey >= Key::Up && E.KeyboardKey <= Key::Left))
      Log("input: keyboard usage %d -> key %d mods 0x%x", Usage, int(E.KeyboardKey), unsigned(E.Modifiers));
    return Mapped;
  }

  void PollKeyboard(std::vector<Event>& Out, uint64_t Now) {
    HidKeyboardState States[16];
    const size_t Count = hidGetKeyboardStates(States, std::size(States));
    const bool Owned = InputFeed().IsOwned(InputClass::Keyboard);
    if (Owned)
      RepeatUsage = 0;
    if (!Count) {
      InputFeed().PublishKeyboard({}, FrameNs);
      Keyboard = {};
      RepeatUsage = 0;
      return;
    }
    // The SDK returns newest first. Process every new state so a short press survives between frames.
    for (size_t I = Count; I-- > 0;) {
      const auto& Current = States[I];
      if (Current.sampling_number <= Keyboard.sampling_number)
        continue;
      auto Held = [&](int Usage) { return hidKeyboardStateGetKey(&Current, HidKeyboardKey(Usage)); };
      KeyboardReport Report {};
      for (size_t Usage = 0; Usage < Report.Usages.size(); ++Usage)
        Report.Usages[Usage] = Held(int(Usage));
      for (size_t I = 0; I < 8; ++I) {
        if (Report.Usages[0xe0 + I])
          Report.Modifiers |= 1 << I;
        Report.Usages.reset(0xe0 + I);
      }
      InputFeed().PublishKeyboard(Report, FrameNs);
      if (Owned) {
        Keyboard = Current;
        continue;
      }
      if (RepeatUsage && !Held(RepeatUsage))
        RepeatUsage = 0;
      for (int Usage = HidKeyboardKey_A; Usage <= HidKeyboardKey_NumPadEquals; ++Usage) {
        if (!Held(Usage) || hidKeyboardStateGetKey(&Keyboard, HidKeyboardKey(Usage)))
          continue;
        Event E;
        if (KeyboardEvent(Current, Usage, Now, E)) {
          Out.push_back(E);
          RepeatUsage = Usage;
          RepeatAt = Now + 450;
        }
      }
      Keyboard = Current;
    }
    if (RepeatUsage && Now >= RepeatAt) {
      Event E;
      if (KeyboardEvent(Keyboard, RepeatUsage, Now, E))
        Out.push_back(E);
      RepeatAt = Now + 40;
    }
  }

  HidKeyboardState Keyboard {};
  int RepeatUsage = 0;
  uint64_t RepeatAt = 0;

  static constexpr int AreaW = 1280, AreaH = 720;
  static constexpr uint64_t StillNs = 3000ull * 1000000ull;

  Display& D;
  uint32_t LastSerial;
  PadState Pads[9];
  PadPointer Pointers[9];
  Result NmouseRc = 0;
  bool NmouseOk = false;
  float StickX[9][2] {}, StickY[9][2] {};
  u64 HeldOut[9] {};   // buttons already reported per pad
  bool PadsOwned = false;
  u64 GuestHeld[9] {};
  u64 SuppressMask[9] {};  // bits that the Joy-Con pointer owns this poll (ZL/ZR as click)
  bool MouseSeen = false;
  bool MouseOwned = false;
  std::array<bool, static_cast<size_t>(MouseButton::Count)> MouseOut {};
  float Gain = 1.0f;
  float PX = 0, PY = 0;  // pointer, output pixels

  // Nmouse state.
  struct Dev {
    u64 Handle = 0;
    u64 Seen = 0;
    bool Primed = false;
    bool Alive = false;
  };
  std::vector<Dev> Devs;
  u64 ActiveHandle = 0;
  bool HasActive = false;
  int ActivePad = 0;
  bool ActiveLeft = false;
  uint64_t ActiveSinceStill = 0;  // ns of the last movement or click
  HidNmouseHandle StdHandle {};
  bool HasStd = false;
  int StdTry = 0;
  u64 StdSeen = 0;
  bool StdPrimed = false;
  u32 StdButtons = 0;
  bool LeftDown = false;  // Physical trigger state of the active Joy-Con pointer.
  bool JoyLeftOut = false;
  int SeenDevices = 0;
  long long RawDx = 0, RawDy = 0, RawWheel = 0;
  uint64_t Samples = 0, Skipped = 0;

  // Legacy mouse (fallback only).
  u64 LastMouseSample = 0;
  bool MousePrimed = false;
  u32 LastMouseButtons = 0;

  struct Finger {
    u32 Id;
    float X, Y;
  };
  std::vector<Finger> Fingers;

  const char* ActiveName() const { return HasActive ? (ActiveLeft ? "JoyLeft" : "JoyRight") : "none"; }

  void PublishMouse(int32_t X = 0, int32_t Y = 0, int32_t Wheel = 0, int32_t HWheel = 0) {
    const uint8_t Buttons = uint8_t((NmouseOk ? StdButtons : LastMouseButtons) | (LeftDown ? HidMouseButton_Left : 0));
    InputFeed().PublishMouse({X, Y, Wheel, HWheel, Buttons}, FrameNs);
  }

  void LoadGainFile() {
    // Optional sidecar sdmc:/fex/ui-pointer-gain.txt with one number (a test aid, never required).
    if (FILE* F = std::fopen("sdmc:/fex/ui-pointer-gain.txt", "r")) {
      float G = 0;
      if (std::fscanf(F, "%f", &G) == 1 && G > 0)
        SetPointerGain(G);
      std::fclose(F);
      Log("input: pointer gain %.2f from sdmc:/fex/ui-pointer-gain.txt", double(Gain));
    }
  }

  static Button MapBit(int Bit) {
    static const Button Map[16] = {Button::A, Button::B, Button::X, Button::Y, Button::StickL, Button::StickR, Button::L, Button::R,
                                   Button::ZL, Button::ZR, Button::Plus, Button::Minus, Button::Left, Button::Up, Button::Right, Button::Down};
    return Map[Bit];
  }

  void EmitMove(std::vector<Event>& Out, uint64_t Now, float DxCounts, float DyCounts) {
    if (MouseOwned)
      return;
    const Size Sz = D.GetSize();
    const float K = Gain * float(Sz.W) / float(AreaW);
    const float Ox = PX, Oy = PY;
    PX = std::clamp(PX + DxCounts * K, 0.0f, float(Sz.W - 1));
    PY = std::clamp(PY + DyCounts * K, 0.0f, float(Sz.H - 1));
    if (PX != Ox || PY != Oy) {
      Event E;
      E.Kind = Event::Type::MouseMove;
      E.TimeMs = Now;
      E.X = PX;
      E.Y = PY;
      E.DX = PX - Ox;
      E.DY = PY - Oy;
      Out.push_back(E);
    }
  }
  void EmitButton(std::vector<Event>& Out, uint64_t Now, MouseButton B, bool Pressed, bool Cancel = false) {
    auto& WasPressed = MouseOut[static_cast<size_t>(B)];
    if ((MouseOwned && Pressed) || WasPressed == Pressed)
      return;
    WasPressed = Pressed;
    Event E;
    E.Kind = Cancel ? Event::Type::MouseCancel : Event::Type::MouseButton;
    E.TimeMs = Now;
    E.Mouse = B;
    E.Pressed = Pressed;
    Out.push_back(E);
  }
  void EmitWheel(std::vector<Event>& Out, uint64_t Now, float Dy) {
    if (MouseOwned)
      return;
    Event E;
    E.Kind = Event::Type::MouseWheel;
    E.TimeMs = Now;
    E.DY = Dy;
    Out.push_back(E);
  }

  Dev& DevFor(u64 Handle, bool& IsNew) {
    for (Dev& V : Devs)
      if (V.Handle == Handle) {
        IsNew = false;
        return V;
      }
    Devs.push_back(Dev {Handle, 0, false, false});
    IsNew = true;
    return Devs.back();
  }

  void PollNmouse(std::vector<Event>& Out, uint64_t Now) {
    const uint64_t NowNs = FrameNs;
    const bool JoyLeftWasDown = LeftDown;
    const bool PreviousJoyLeftOut = JoyLeftOut;
    for (Dev& V : Devs)
      V.Alive = false;
    int Count = 0;
    int32_t JoyX = 0, JoyY = 0, JoyWheel = 0;
    bool ActiveSeen = false;
    for (int Pad = 0; Pad < 9; ++Pad) {
      padPointerUpdate(&Pointers[Pad], &Pads[Pad]);
      const u32 N = std::min<u32>(Pointers[Pad].device_count, PAD_POINTER_MAX_DEVICES);
      for (u32 K = 0; K < N; ++K) {
        const HidNmouseHandle H = Pointers[Pad].devices[K].handle;
        ++Count;
        bool IsNew = false;
        Dev& V = DevFor(H.handle, IsNew);
        V.Alive = true;
        HidNmouseStateWithDelta St[16];
        size_t Total = 0;
        if (R_FAILED(hidGetNmouseStatesWithDelta(H, St, 16, &Total)) || Total == 0)
          continue;
        if (!V.Primed) {  // start from the newest sample: no replay of old history
          V.Primed = true;
          V.Seen = St[0].sampling_number;
          Log("input: nmouse device handle=%llx on pad %d (new)", (unsigned long long)H.handle, Pad);
          u32 Types = 0;
          hidGetNmouseTypeSet(H, &Types);
          Log("input: nmouse type set 0x%x (1 merged, 2 JoyLeft, 4 JoyRight, 8 StandardMouse)", Types);
          continue;
        }
        int32_t Dx = 0, Dy = 0, Wheel = 0;
        for (size_t I = 0; I < Total; ++I) {
          if (St[I].sampling_number <= V.Seen)
            break;  // newest first
          ++Samples;
          Wheel += St[I].wheel_delta_y;
          if (St[I].flags & 2) {  // bit 1 clear: the sensor did not track (lifted off the surface)
            Dx += St[I].delta_x;
            Dy += St[I].delta_y;
          } else {
            ++Skipped;
          }
        }
        V.Seen = St[0].sampling_number;
        u32 Types = 0;
        hidGetNmouseTypeSet(H, &Types);
        const bool IsLeft = (Types & HidNmouseType_JoyLeft) != 0, IsRight = (Types & HidNmouseType_JoyRight) != 0;
        if (!IsLeft && !IsRight)
          continue;  // a standard mouse is read through its own handle below
        if (!HasActive && (Dx != 0 || Dy != 0)) {
          HasActive = true;
          ActiveHandle = H.handle;
          ActivePad = Pad;
          ActiveLeft = IsLeft;
          ActiveSinceStill = NowNs;
          LeftDown = false;
          Log("input: joy-con pointer in use: %s on pad %d", IsLeft ? "left" : "right", Pad);
        }
        if (HasActive && ActiveHandle == H.handle) {
          JoyX = Dx;
          JoyY = Dy;
          JoyWheel = Wheel;
          ActiveSeen = true;
          MouseSeen = true;
          RawDx += (long long)Dx;
          RawDy += (long long)Dy;
          RawWheel += (long long)Wheel;
          if (!PadsOwned && (Dx != 0 || Dy != 0))
            EmitMove(Out, Now, Dx, Dy);
          if (!PadsOwned && Wheel != 0)
            EmitWheel(Out, Now, Wheel);
          // The click is the Joy-Con's own ZL (left) or ZR (right), so that trigger is not also a button meanwhile.
          const u64 Bit = ActiveLeft ? HidNpadButton_ZL : HidNpadButton_ZR;
          const bool Down = (padGetButtons(&Pads[Pad]) & Bit) != 0;
          SuppressMask[Pad] |= Bit;
          LeftDown = Down;
          if (Dx != 0 || Dy != 0 || Down)
            ActiveSinceStill = NowNs;
        }
      }
    }
    SeenDevices = Count;
    // Release the Joy-Con when it left the list or has been still for 3 s (the trigger is a button again).
    if (HasActive && (!ActiveSeen || NowNs - ActiveSinceStill >= StillNs)) {
      LeftDown = false;
      Log("input: joy-con pointer released (%s)", ActiveSeen ? "still for 3 s" : "device gone");
      HasActive = false;
    }
    if (!LeftDown || PadsOwned || MouseOwned)
      JoyLeftOut = false;
    else if (!JoyLeftWasDown)
      JoyLeftOut = true;
    if (JoyLeftOut != PreviousJoyLeftOut)
      EmitButton(Out, Now, MouseButton::Left, JoyLeftOut || (StdButtons & HidMouseButton_Left),
                 PadsOwned || !ActiveSeen);
    PublishMouse(JoyX, JoyY, JoyWheel);
    PollStandardMouse(Out, Now);
  }

  void PollStandardMouse(std::vector<Event>& Out, uint64_t Now) {
    if (!HasStd) {
      if (++StdTry >= 60) {  // look again about once a second
        StdTry = 0;
        HidNmouseHandle H {};
        if (R_SUCCEEDED(hidGetStandardMouseNmouseHandle(&H))) {
          StdHandle = H;
          HasStd = true;
          StdPrimed = false;
          Log("input: nmouse standard mouse found (handle=%llx)", (unsigned long long)H.handle);
        }
      }
      return;
    }
    HidNmouseStateWithDelta St[16];
    size_t Total = 0;
    if (R_FAILED(hidGetNmouseStatesWithDelta(StdHandle, St, 16, &Total))) {
      HasStd = false;
      StdButtons = 0;
      PublishMouse();
      for (size_t I = 0; I < MouseOut.size(); ++I) {
        if (I != 0 || !JoyLeftOut)
          EmitButton(Out, Now, static_cast<MouseButton>(I), false, true);
      }
      Log("input: nmouse standard mouse gone");
      return;
    }
    if (Total == 0)
      return;
    if (!StdPrimed) {
      StdPrimed = true;
      StdSeen = St[0].sampling_number;
      StdButtons = St[0].buttons;
      PublishMouse();
      return;
    }
    float Dx = 0, Dy = 0, Wheel = 0;
    const u32 PreviousButtons = StdButtons;
    for (size_t I = Total; I-- > 0;) {
      const auto& Sample = St[I];
      if (Sample.sampling_number <= StdSeen)
        continue;
      StdButtons = Sample.buttons;
      PublishMouse(Sample.delta_x, Sample.delta_y, Sample.wheel_delta_y);
      Dx += float(Sample.delta_x);
      Dy += float(Sample.delta_y);
      Wheel += float(Sample.wheel_delta_y);
    }
    StdSeen = St[0].sampling_number;
    if (Dx != 0 || Dy != 0 || Wheel != 0 || StdButtons != PreviousButtons)
      MouseSeen = true;
    RawDx += (long long)Dx;
    RawDy += (long long)Dy;
    RawWheel += (long long)Wheel;
    if (Dx != 0 || Dy != 0)
      EmitMove(Out, Now, Dx, Dy);
    if (Wheel != 0)
      EmitWheel(Out, Now, Wheel);
    const u32 Changed = StdButtons ^ PreviousButtons;
    static const u32 Bits[5] = {HidMouseButton_Left, HidMouseButton_Right, HidMouseButton_Middle, HidMouseButton_Back, HidMouseButton_Forward};
    static const MouseButton Map[5] = {MouseButton::Left, MouseButton::Right, MouseButton::Middle, MouseButton::Back, MouseButton::Forward};
    for (int B = 0; B < 5; ++B)
      if (Changed & Bits[B])
        EmitButton(Out, Now, Map[B], (StdButtons & Bits[B]) != 0 || (B == 0 && JoyLeftOut));
  }

  void PollPads(std::vector<Event>& Out, uint64_t Now) {
    for (int I = 0; I < 9; ++I) {
      // Report the difference between what the pad holds (minus what the pointer owns) and what was reported before.
      const u64 Buttons = padGetButtons(&Pads[I]);
      if (PadsOwned) {
        GuestHeld[I] = Buttons;
        HeldOut[I] = 0;
        for (unsigned S = 0; S < 2; ++S)
          StickX[I][S] = StickY[I][S] = 0;
        continue;
      }
      GuestHeld[I] &= Buttons;
      const u64 Held = Buttons & ~(SuppressMask[I] | GuestHeld[I]);
      const u64 Diff = Held ^ HeldOut[I];
      for (int Bit = 0; Bit < 16; ++Bit) {
        if (Diff & (1ull << Bit)) {
          Event E;
          E.Kind = (Held & (1ull << Bit)) ? Event::Type::ButtonDown : Event::Type::ButtonUp;
          E.TimeMs = Now;
          E.Pad = uint32_t(I);
          E.Btn = MapBit(Bit);
          Out.push_back(E);
        }
      }
      HeldOut[I] = Held;
      for (unsigned S = 0; S < 2; ++S) {
        const auto St = padGetStickPos(&Pads[I], S);
        float X = float(St.x) / 32767.0f, Y = float(St.y) / 32767.0f;
        if (X != StickX[I][S] || Y != StickY[I][S]) {
          StickX[I][S] = X;
          StickY[I][S] = Y;
          Event E;
          E.Kind = Event::Type::Stick;
          E.TimeMs = Now;
          E.Pad = uint32_t(I);
          E.StickIndex = uint8_t(S);
          E.X = X;
          E.Y = Y;
          Out.push_back(E);
        }
      }
    }
  }

  void PollTouch(std::vector<Event>& Out, uint64_t Now) {
    HidTouchScreenState State {};
    hidGetTouchScreenStates(&State, 1);
    // libnx touch coordinates are on the 1280x720 handheld screen; scale to the output.
    const Size Sz = D.GetSize();
    const float Sx = float(Sz.W) / 1280.0f, Sy = float(Sz.H) / 720.0f;
    std::vector<Finger> Now2;
    for (s32 I = 0; I < State.count && I < 16; ++I)
      Now2.push_back(Finger {State.touches[I].finger_id, float(State.touches[I].x) * Sx, float(State.touches[I].y) * Sy});
    auto Emit = [&](const Finger& F, TouchPhase Phase) {
      Event E;
      E.Kind = Event::Type::Touch;
      E.TimeMs = Now;
      E.TouchId = F.Id;
      E.Phase = Phase;
      E.X = F.X;
      E.Y = F.Y;
      Out.push_back(E);
    };
    for (const Finger& Old : Fingers) {
      bool Found = false;
      for (const Finger& Cur : Now2)
        Found |= Cur.Id == Old.Id;
      if (!Found)
        Emit(Old, TouchPhase::Up);
    }
    for (const Finger& Cur : Now2) {
      const Finger* Prev = nullptr;
      for (const Finger& Old : Fingers)
        if (Old.Id == Cur.Id)
          Prev = &Old;
      if (!Prev)
        Emit(Cur, TouchPhase::Down);
      else if (Prev->X != Cur.X || Prev->Y != Cur.Y)
        Emit(Cur, TouchPhase::Move);
    }
    Fingers = Now2;
  }

  // Fallback when Nmouse could not start: the ordinary libnx mouse, deltas summed into the same pointer.
  void PollLegacyMouse(std::vector<Event>& Out, uint64_t Now) {
    HidMouseState States[8] {};
    size_t Count = hidGetMouseStates(States, 8);
    if (!Count || !(States[0].attributes & HidMouseAttribute_IsConnected)) {
      MousePrimed = false;
      LastMouseButtons = 0;
      PublishMouse();
      for (size_t I = 0; I < MouseOut.size(); ++I)
        EmitButton(Out, Now, static_cast<MouseButton>(I), false, true);
      return;
    }
    if (!MousePrimed) {
      MousePrimed = true;
      LastMouseSample = States[0].sampling_number;
      LastMouseButtons = States[0].buttons;
      PublishMouse();
      return;
    }
    for (size_t I = Count; I-- > 0;) {
      const HidMouseState& M = States[I];
      if (M.sampling_number <= LastMouseSample)
        continue;
      LastMouseSample = M.sampling_number;
      MouseSeen = true;
      if (M.delta_x || M.delta_y)
        EmitMove(Out, Now, float(M.delta_x), float(M.delta_y));
      const u32 Changed = M.buttons ^ LastMouseButtons;
      static const u32 Bits[5] = {HidMouseButton_Left, HidMouseButton_Right, HidMouseButton_Middle, HidMouseButton_Back, HidMouseButton_Forward};
      static const MouseButton Map[5] = {MouseButton::Left, MouseButton::Right, MouseButton::Middle, MouseButton::Back, MouseButton::Forward};
      for (int B = 0; B < 5; ++B)
        if (Changed & Bits[B])
          EmitButton(Out, Now, Map[B], (M.buttons & Bits[B]) != 0);
      LastMouseButtons = M.buttons;
      PublishMouse(M.delta_x, M.delta_y, M.wheel_delta_y, M.wheel_delta_x);
      if (M.wheel_delta_y)
        EmitWheel(Out, Now, float(M.wheel_delta_y));
    }
  }
};

// ---- System ---------------------------------------------------------------

class Switch2System final : public System {
public:
  Switch2System() {
    Result Rc = timeInitialize();
    TimeOk = R_SUCCEEDED(Rc);
    Log("step timeInitialize: %s (rc=0x%x)", TimeOk ? "ok" : "FAIL", unsigned(Rc));
  }
  ~Switch2System() override {
    if (TimeOk)
      timeExit();
  }
  uint64_t MonotonicMs() override { return NowMs(); }
  bool WallClock(WallTime& Out) override {
    if (!TimeOk)
      return false;
    u64 Stamp = 0;
    TimeCalendarTime Cal {};
    TimeCalendarAdditionalInfo Info {};
    if (R_FAILED(timeGetCurrentTime(TimeType_LocalSystemClock, &Stamp)) || R_FAILED(timeToCalendarTimeWithMyRule(Stamp, &Cal, &Info)))
      return false;
    Out = WallTime {int(Cal.year), int(Cal.month), int(Cal.day), int(Cal.hour), int(Cal.minute), int(Cal.second)};
    return true;
  }
  // Not implemented in Task 1: the tasks that need them add and test them on the console.
  bool FreeSpace(const char*, uint64_t&, uint64_t&) override { return false; }
  bool NetworkAddress(std::string&) override { return false; }
  bool Battery(int&, bool&) override { return false; }
  bool ShowKeyboard(const char*, std::string&) override { return false; }
  // The applet main loop also handles the console's HOME and sleep messages.
  bool KeepRunning() override { return appletMainLoop(); }

private:
  bool TimeOk = false;
};

class Switch2Platform final : public Platform {
public:
  Switch2Platform() : In(Disp) {}
  ~Switch2Platform() override { Rend.reset(); }
  bool Open() {
    if (!Disp.Open())
      return false;
    Rend = CreateVulkanRenderer(Disp.Tgt);
    return Rend != nullptr;
  }
  Display& GetDisplay() override { return Disp; }
  Input& GetInput() override { return In; }
  System& GetSystem() override { return Sys; }
  Renderer& GetRenderer() override { return *Rend; }

private:
  Switch2Display Disp;
  Switch2Input In;
  Switch2System Sys;
  std::unique_ptr<Renderer> Rend;
};

} // namespace

std::unique_ptr<Platform> CreatePlatform(const PlatformOptions&) {
  Log("platform: Switch 2 (Vulkan on OunceVK, VI NWindow, libnx hid); userAppInit ran: %s", WindowPrepared ? "yes" : "NO");
  auto P = std::make_unique<Switch2Platform>();
  if (!P->Open())
    return nullptr;
  return P;
}

} // namespace ui
