// SPDX-License-Identifier: MIT
#pragma once

#include "GuestEvdev.h"

#include <array>
#include <bitset>
#include <deque>
#include <iterator>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>

namespace Switch2Guest {

inline constexpr size_t InputPadCount = 8;

// These bits match the low 16 bits of HidNpadButton.
enum PadButton : uint64_t {
  PadA = 1 << 0, PadB = 1 << 1, PadX = 1 << 2, PadY = 1 << 3,
  PadStickL = 1 << 4, PadStickR = 1 << 5, PadL = 1 << 6, PadR = 1 << 7,
  PadZL = 1 << 8, PadZR = 1 << 9, PadPlus = 1 << 10, PadMinus = 1 << 11,
  PadLeft = 1 << 12, PadUp = 1 << 13, PadRight = 1 << 14, PadDown = 1 << 15,
};

struct PadReport {
  uint64_t Buttons {};
  int32_t LX {}, LY {}, RX {}, RY {}; // Native stick coordinates, positive Y up.
  uint32_t Style {};
  bool Connected {};
  bool operator==(const PadReport&) const = default;
};

struct KeyboardReport {
  uint8_t Modifiers {}; // USB modifier bits, in usage order 0xe0 through 0xe7.
  std::bitset<256> Usages;
  bool operator==(const KeyboardReport&) const = default;
};

struct MouseReport {
  int32_t X {}, Y {}, Wheel {}, HWheel {};
  uint8_t Buttons {}; // Left, right and middle in bits 0 through 2.
};

struct PadConnection {
  uint64_t Generation {};
  Evdev::GamepadIdentity Identity {};
};

struct PadChange {
  uint32_t Index;
  PadReport Before, After;
  PadConnection Connection {}, PreviousConnection {};
};
struct KeyboardChange { KeyboardReport Before, After; };
struct MouseChange { uint8_t BeforeButtons; MouseReport After; };

struct InputChange {
  uint64_t TimeNs;
  std::variant<PadChange, KeyboardChange, MouseChange> Change;
};

struct InputSnapshot {
  std::array<PadReport, InputPadCount> Pads {};
  std::array<PadConnection, InputPadCount> Connections {};
  KeyboardReport Keyboard;
  uint8_t MouseButtons {};
  uint64_t TimeNs {};
};

struct InputBatch {
  std::vector<InputChange> Changes;
  // State immediately before the retained changes, supplied on the first drain and after loss.
  std::optional<InputSnapshot> Snapshot;
  bool Dropped {};
};

enum class InputClass { Pad, Keyboard, Mouse };

struct OwnershipHold {
  std::optional<uint64_t> SinceNs;
  bool Released {};
};

// One release per uninterrupted hold. Disconnects and releasing either button reset the hold.
inline OwnershipHold AdvanceOwnershipHold(OwnershipHold Hold, bool Held, bool Owned, uint64_t TimeNs) {
  if (!Held) return {};
  if (Hold.Released) return Hold;
  if (!Owned) return {};
  if (!Hold.SinceNs) Hold.SinceNs = TimeNs;
  Hold.Released = TimeNs - *Hold.SinceNs >= 1'000'000'000;
  return Hold;
}

inline std::vector<Evdev::InputEvent> ToEvdevPacket(
  const PadChange& Change, Evdev::GamepadIdentity Identity) {
  using namespace Evdev;
  const bool Xbox = Identity == GamepadIdentity::Xbox360;
  const PadReport Before = Change.Before.Connected ? Change.Before : PadReport {};
  const PadReport After = Change.After.Connected ? Change.After : PadReport {};
  std::vector<InputEvent> Events;
  const auto Key = [&](uint64_t Bit, uint16_t Code) {
    if ((Before.Buttons ^ After.Buttons) & Bit)
      Events.push_back({0, 0, EV_KEY, Code, bool(After.Buttons & Bit)});
  };
  const auto Axis = [&](uint16_t Code, int32_t Old, int32_t New) {
    if (Old != New)
      Events.push_back({0, 0, EV_ABS, Code, New});
  };
  constexpr uint16_t Keys[] {BTN_EAST, BTN_SOUTH, BTN_NORTH, BTN_WEST, BTN_THUMBL, BTN_THUMBR,
                             BTN_TL, BTN_TR, 0, 0, BTN_START, BTN_SELECT};
  for (size_t I = 0; I < std::size(Keys); ++I)
    if (Keys[I]) Key(1ull << I, Keys[I]);

  // xpad turns digital triggers into full-range axes. hid-nintendo reports them as buttons.
  if (Xbox) {
    Axis(ABS_Z, Before.Buttons & PadZL ? 255 : 0, After.Buttons & PadZL ? 255 : 0);
    Axis(ABS_RZ, Before.Buttons & PadZR ? 255 : 0, After.Buttons & PadZR ? 255 : 0);
  } else {
    Key(PadZL, BTN_TL2);
    Key(PadZR, BTN_TR2);
  }
  const auto Stick = [&](int32_t Value, bool Invert = false) {
    const int32_t Min = Xbox ? -32768 : -32767;
    const int32_t Clamped = std::clamp(Value, Invert ? -32767 : Min, Invert ? -Min : 32767);
    return Invert ? -Clamped : Clamped;
  };
  Axis(ABS_X, Stick(Before.LX), Stick(After.LX));
  Axis(ABS_Y, Stick(Before.LY, true), Stick(After.LY, true));
  Axis(ABS_RX, Stick(Before.RX), Stick(After.RX));
  Axis(ABS_RY, Stick(Before.RY, true), Stick(After.RY, true));
  const auto Hat = [](uint64_t Buttons, uint64_t Positive, uint64_t Negative) {
    return int(bool(Buttons & Positive)) - int(bool(Buttons & Negative));
  };
  Axis(ABS_HAT0X, Hat(Before.Buttons, PadRight, PadLeft), Hat(After.Buttons, PadRight, PadLeft));
  Axis(ABS_HAT0Y, Hat(Before.Buttons, PadDown, PadUp), Hat(After.Buttons, PadDown, PadUp));
  if (!Events.empty()) Events.push_back({0, 0, EV_SYN, SYN_REPORT, 0});
  return Events;
}

inline std::vector<Evdev::InputEvent> ToEvdevPacket(const KeyboardChange& Change) {
  using namespace Evdev;
  std::vector<InputEvent> Events;
  const auto Held = [](const KeyboardReport& Report, size_t Usage) {
    return Report.Usages[Usage] || (Usage >= 0xe0 && Usage <= 0xe7 && (Report.Modifiers & (1 << (Usage - 0xe0))));
  };
  for (size_t Usage = 0; Usage < Change.After.Usages.size(); ++Usage) {
    const auto Key = HidUsageToKey(static_cast<uint8_t>(Usage));
    const bool Down = Held(Change.After, Usage);
    if (Key && Down != Held(Change.Before, Usage)) {
      // hid-input emits the HID page and usage as MSC_SCAN before the key transition.
      Events.push_back({0, 0, EV_MSC, MSC_SCAN, 0x70000 + static_cast<int32_t>(Usage)});
      Events.push_back({0, 0, EV_KEY, Key, Down});
    }
  }
  if (!Events.empty()) Events.push_back({0, 0, EV_SYN, SYN_REPORT, 0});
  return Events;
}

inline std::vector<Evdev::InputEvent> ToEvdevPacket(const MouseChange& Change) {
  using namespace Evdev;
  std::vector<InputEvent> Events;
  constexpr uint16_t Keys[] {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE};
  for (size_t I = 0; I < std::size(Keys); ++I)
    if ((Change.BeforeButtons ^ Change.After.Buttons) & (1 << I))
      Events.push_back({0, 0, EV_KEY, Keys[I], bool(Change.After.Buttons & (1 << I))});
  const auto Relative = [&](uint16_t Code, int32_t Value) {
    if (Value) Events.push_back({0, 0, EV_REL, Code, Value});
  };
  Relative(REL_X, Change.After.X);
  Relative(REL_Y, Change.After.Y);
  Relative(REL_WHEEL, Change.After.Wheel);
  Relative(REL_HWHEEL, Change.After.HWheel);
  if (!Events.empty()) Events.push_back({0, 0, EV_SYN, SYN_REPORT, 0});
  return Events;
}

struct PadRumble { uint32_t Index; Evdev::RumbleRequest Request; };

class GuestInputFeed {
public:
  static constexpr size_t ChangeLimit = 256;

  uint64_t PublishPad(uint32_t Index, PadReport Report, uint64_t TimeNs) {
    if (Index >= State.Pads.size()) return 0;
    std::lock_guard Guard {Lock};
    if (!Report.Connected) Report = {};
    const auto Combo = PadPlus | PadMinus;
    auto& Hold = Holds[Index];
    const auto Next = AdvanceOwnershipHold(Hold, (Report.Buttons & Combo) == Combo,
                                           Owned[0] || Owned[1] || Owned[2], TimeNs);
    if (Next.Released && !Hold.Released) {
      Owned.fill(false);
      Rumble.clear();
      ++Generation;
    }
    Hold = Next;
    State.TimeNs = TimeNs;
    UpdatePad(Index, Report, TimeNs);
    return Report.Connected ? State.Connections[Index].Generation : 0;
  }

  void PublishKeyboard(const KeyboardReport& Report, uint64_t TimeNs) {
    std::lock_guard Guard {Lock};
    if (Collecting && State.Keyboard != Report) Queue({TimeNs, KeyboardChange {State.Keyboard, Report}});
    State.Keyboard = Report;
    State.TimeNs = TimeNs;
  }

  void PublishMouse(MouseReport Report, uint64_t TimeNs) {
    std::lock_guard Guard {Lock};
    Report.Buttons &= 7;
    if (Collecting && (State.MouseButtons != Report.Buttons || Report.X || Report.Y || Report.Wheel || Report.HWheel))
      Queue({TimeNs, MouseChange {State.MouseButtons, Report}});
    State.MouseButtons = Report.Buttons;
    State.TimeNs = TimeNs;
  }

  InputSnapshot Snapshot() const {
    std::lock_guard Guard {Lock};
    return State;
  }

  InputBatch Take() {
    std::lock_guard Guard {Lock};
    InputBatch Batch;
    if (!Changes.empty()) {
      Batch.Changes.assign(std::make_move_iterator(Changes.begin()), std::make_move_iterator(Changes.end()));
      Changes.clear();
    }
    if (!Collecting || Dropped) Batch.Snapshot = State;
    Batch.Dropped = std::exchange(Dropped, false);
    if (Batch.Dropped) {
      // Undo the retained tail to recover the pad topology and held state at the loss boundary.
      auto& Before = *Batch.Snapshot;
      Before.TimeNs = Batch.Changes.front().TimeNs;
      for (auto It = Batch.Changes.rbegin(); It != Batch.Changes.rend(); ++It) {
        if (const auto* Pad = std::get_if<PadChange>(&It->Change)) {
          Before.Pads[Pad->Index] = Pad->Before;
          Before.Connections[Pad->Index] = Pad->PreviousConnection;
        } else if (const auto* Keyboard = std::get_if<KeyboardChange>(&It->Change)) {
          Before.Keyboard = Keyboard->Before;
        } else {
          Before.MouseButtons = std::get<MouseChange>(It->Change).BeforeButtons;
        }
      }
    }
    Collecting = true;
    return Batch;
  }

  // Call when the last open description is released. Take starts again with the current state.
  void StopTaking() {
    std::lock_guard Guard {Lock};
    Changes.clear();
    Dropped = Collecting = false;
    Owned.fill(false);
  }

  void PushRumble(uint32_t Index, Evdev::RumbleRequest Request, uint64_t Connection) {
    std::lock_guard Guard {Lock};
    if (Index >= State.Pads.size() || !State.Pads[Index].Connected ||
        State.Connections[Index].Generation != Connection || Request.EffectId < 0 || Request.EffectId >= 16) return;
    // The UI applies pending requests together once per frame. The last command for an effect is the one it plays.
    const auto Pending = std::find_if(Rumble.begin(), Rumble.end(), [&](const PadRumble& Item) {
      return Item.Index == Index && Item.Request.EffectId == Request.EffectId;
    });
    if (Pending == Rumble.end()) Rumble.push_back({Index, Request});
    else Pending->Request = Request;
  }

  std::vector<PadRumble> TakeRumble() {
    std::lock_guard Guard {Lock};
    return std::exchange(Rumble, {});
  }

  // Use the generation recorded when the foreground guest opened the device.
  void SetOwned(InputClass Device, bool Value, uint64_t OpenGeneration) {
    std::lock_guard Guard {Lock};
    Owned[static_cast<size_t>(Device)] = Value && OpenGeneration == Generation;
  }

  bool IsOwned(InputClass Device) const {
    std::lock_guard Guard {Lock};
    return Owned[static_cast<size_t>(Device)];
  }

  uint64_t ReleasedGeneration() const {
    std::lock_guard Guard {Lock};
    return Generation;
  }

  void SetGamepadIdentity(Evdev::GamepadIdentity Value) {
    std::lock_guard Guard {Lock};
    if (Identity == Value) return;
    Identity = Value;
    for (uint32_t Index = 0; Index < State.Pads.size(); ++Index)
      if (State.Pads[Index].Connected) UpdatePad(Index, State.Pads[Index], State.TimeNs);
  }

  Evdev::GamepadIdentity GetGamepadIdentity() const {
    std::lock_guard Guard {Lock};
    return Identity;
  }

private:
  mutable std::mutex Lock;
  void UpdatePad(uint32_t Index, PadReport Report, uint64_t TimeNs) {
    auto& Before = State.Pads[Index];
    auto& Connection = State.Connections[Index];
    const auto PreviousConnection = Connection;
    const bool Reconnect = Report.Connected &&
      (!Before.Connected || Before.Style != Report.Style || Connection.Identity != Identity);
    if (Before.Connected && (!Report.Connected || Reconnect))
      std::erase_if(Rumble, [Index](const PadRumble& Item) { return Item.Index == Index; });
    if (Reconnect) Connection = {++LastConnection, Identity};
    if (Collecting && (Before != Report || Reconnect))
      Queue({TimeNs, PadChange {Index, Before, Report, Connection, PreviousConnection}});
    Before = Report;
  }

  void Queue(InputChange Change) {
    if (Changes.size() == ChangeLimit) {
      Changes.pop_front();
      Dropped = true;
    }
    Changes.push_back(std::move(Change));
  }

  InputSnapshot State;
  std::array<OwnershipHold, InputPadCount> Holds {};
  uint64_t LastConnection {};
  bool Collecting {}, Dropped {};
  std::deque<InputChange> Changes;
  std::vector<PadRumble> Rumble;
  std::array<bool, 3> Owned {};
  uint64_t Generation {};
  Evdev::GamepadIdentity Identity = Evdev::GamepadIdentity::Xbox360;
};

inline GuestInputFeed& InputFeed() {
  static GuestInputFeed Feed;
  return Feed;
}

} // namespace Switch2Guest
