// SPDX-License-Identifier: MIT
// The pure evdev model (Linux drivers/input/evdev.c, input.c, ff-core.c and include/uapi/linux/input.h): the layouts
// of input_event, input_id, input_absinfo and ff_effect, the devices the runtime offers (gamepad, keyboard, mouse),
// the per-open-file client with its event ring, the evdev ioctls on plain byte buffers, and the USB HID usage table of
// drivers/hid/hid-input.c. No Horizon calls: the runtime copies guest memory in and out and feeds the packets.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Switch2Guest::Evdev {

// ---- Constants (include/uapi/linux/input-event-codes.h, input.h) ----------------------------------------------------

inline constexpr uint16_t EV_SYN = 0x00, EV_KEY = 0x01, EV_REL = 0x02, EV_ABS = 0x03, EV_MSC = 0x04, EV_SW = 0x05, EV_LED = 0x11,
                          EV_SND = 0x12, EV_REP = 0x14, EV_FF = 0x15, EV_MAX = 0x1f;
inline constexpr uint16_t SYN_REPORT = 0, SYN_DROPPED = 3;
inline constexpr uint16_t KEY_MAX = 0x2ff, REL_MAX = 0x0f, ABS_MAX = 0x3f, MSC_MAX = 0x07, SW_MAX = 0x10, LED_MAX = 0x0f, SND_MAX = 0x07,
                          FF_MAX = 0x7f, INPUT_PROP_MAX = 0x1f;
inline constexpr uint16_t REL_X = 0, REL_Y = 1, REL_HWHEEL = 6, REL_WHEEL = 8;
inline constexpr uint16_t ABS_X = 0, ABS_Y = 1, ABS_Z = 2, ABS_RX = 3, ABS_RY = 4, ABS_RZ = 5, ABS_HAT0X = 0x10, ABS_HAT0Y = 0x11;
inline constexpr uint16_t MSC_SCAN = 4;
inline constexpr uint16_t LED_NUML = 0, LED_CAPSL = 1, LED_SCROLLL = 2;
inline constexpr uint16_t REP_DELAY = 0, REP_PERIOD = 1;
inline constexpr uint16_t BTN_LEFT = 0x110, BTN_RIGHT = 0x111, BTN_MIDDLE = 0x112;
inline constexpr uint16_t BTN_SOUTH = 0x130, BTN_EAST = 0x131, BTN_NORTH = 0x133, BTN_WEST = 0x134, BTN_Z = 0x135, BTN_TL = 0x136,
                          BTN_TR = 0x137, BTN_TL2 = 0x138, BTN_TR2 = 0x139, BTN_SELECT = 0x13a, BTN_START = 0x13b, BTN_MODE = 0x13c,
                          BTN_THUMBL = 0x13d, BTN_THUMBR = 0x13e;
inline constexpr uint16_t FF_RUMBLE = 0x50, FF_PERIODIC = 0x51, FF_SQUARE = 0x58, FF_TRIANGLE = 0x59,
                          FF_SINE = 0x5a, FF_GAIN = 0x60;
inline constexpr uint32_t EvVersion = 0x010001; // EV_VERSION
inline constexpr uint16_t BUS_USB = 0x03;
inline constexpr size_t InputEventSize = 24;

// Errors.
inline constexpr int64_t Enoent = -2, Eio = -5, Enodev = -19, Einval = -22, Enotty = -25, Enospc = -28, Enosys = -38, Eagain = -11,
                         Ebusy = -16, Eacces = -13;
// Read result: the queue is empty and the file is blocking, so the caller waits for data.
inline constexpr int64_t ReadWait = INT64_MIN;

// ---- Layouts (x86-64) -----------------------------------------------------------------------------------------------

struct InputEvent {
  int64_t Sec;
  int64_t Usec;
  uint16_t Type;
  uint16_t Code;
  int32_t Value;
};
static_assert(sizeof(InputEvent) == InputEventSize);

struct InputId {
  uint16_t Bustype, Vendor, Product, Version;
};
static_assert(sizeof(InputId) == 8);

struct AbsInfo {
  int32_t Value, Minimum, Maximum, Fuzz, Flat, Resolution;
};
static_assert(sizeof(AbsInfo) == 24);

// ff_effect: 48 bytes. The union starts at 16 (the periodic effect holds a pointer) and is 32 bytes.
struct FfEffect {
  uint16_t Type;
  int16_t Id;
  uint16_t Direction;
  uint16_t TriggerButton, TriggerInterval;
  uint16_t ReplayLength, ReplayDelay;
  uint16_t Pad;
  uint8_t Union[32];

  // ff_rumble_effect {strong_magnitude, weak_magnitude}
  uint16_t Strong() const { return Field(0); }
  uint16_t Weak() const { return Field(2); }

  template<typename T = uint16_t>
  T Field(size_t Offset) const {
    T Value;
    std::memcpy(&Value, Union + Offset, sizeof(Value));
    return Value;
  }
};
static_assert(sizeof(FfEffect) == 48);

// input_mask {type, codes_size, codes_ptr}
struct InputMask {
  uint32_t Type;
  uint32_t CodesSize;
  uint64_t CodesPtr;
};
static_assert(sizeof(InputMask) == 16);

// ---- Bitmaps --------------------------------------------------------------------------------------------------------

using Bits = std::vector<uint64_t>;
inline Bits MakeBits(uint32_t MaxBit) { return Bits((MaxBit + 64) / 64, 0); } // room for bits 0..MaxBit
inline void SetBit(Bits& B, uint32_t N) { if (N / 64 < B.size()) B[N / 64] |= 1ull << (N % 64); }
inline void ClearBit(Bits& B, uint32_t N) { if (N / 64 < B.size()) B[N / 64] &= ~(1ull << (N % 64)); }
inline bool TestBit(const Bits& B, uint32_t N) { return N / 64 < B.size() && (B[N / 64] >> (N % 64) & 1); }
inline uint32_t BitCount(const Bits& B) {
  uint32_t Count {};
  for (const auto Word : B) Count += std::popcount(Word);
  return Count;
}

// ---- The USB HID usage table (hid_keyboard[] of drivers/hid/hid-input.c) ---------------------------------------------

inline const std::array<uint16_t, 256>& HidKeyTable() {
  static const std::array<uint16_t, 256> Table = [] {
    std::array<uint16_t, 256> T {};
    const uint16_t Letters[26] = {30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
    for (int I = 0; I < 26; ++I) T[4 + I] = Letters[I];
    for (int I = 0; I < 10; ++I) T[30 + I] = static_cast<uint16_t>(2 + I); // 1..9, 0
    const uint16_t Row2[] = {28, 1, 14, 15, 57, 12, 13, 26, 27, 43, 43, 39, 40, 41, 51, 52, 53, 58}; // 0x28..0x39
    for (size_t I = 0; I < std::size(Row2); ++I) T[40 + I] = Row2[I];
    const uint16_t Fn[] = {59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88}; // F1..F12
    for (size_t I = 0; I < std::size(Fn); ++I) T[58 + I] = Fn[I];
    const uint16_t Nav[] = {99, 70, 119, 110, 102, 104, 111, 107, 109, 106, 105, 108, 103}; // 0x46..0x52
    for (size_t I = 0; I < std::size(Nav); ++I) T[70 + I] = Nav[I];
    const uint16_t Pad[] = {69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71, 72, 73, 82, 83, 86, 127, 116, 117}; // 0x53..0x67
    for (size_t I = 0; I < std::size(Pad); ++I) T[83 + I] = Pad[I];
    for (int I = 0; I < 12; ++I) T[104 + I] = static_cast<uint16_t>(183 + I); // F13..F24
    const uint16_t Misc[] = {134, 138, 130, 132, 128, 129, 131, 137, 133, 135, 136, 113, 115, 114}; // 0x74..0x81
    for (size_t I = 0; I < std::size(Misc); ++I) T[116 + I] = Misc[I];
    T[133] = 121; T[135] = 89; T[136] = 93; T[137] = 124; T[138] = 92; T[139] = 94; T[140] = 95;
    T[144] = 122; T[145] = 123; T[146] = 90; T[147] = 91; T[148] = 85;
    const uint16_t Mods[] = {29, 42, 56, 125, 97, 54, 100, 126}; // 0xe0..0xe7
    for (size_t I = 0; I < std::size(Mods); ++I) T[224 + I] = Mods[I];
    const uint16_t Media[] = {164, 166, 165, 163, 161, 115, 114, 113, 150, 158, 159, 128, 136, 177, 178, 176, 142, 152, 173, 140}; // 0xe8..0xfb
    for (size_t I = 0; I < std::size(Media); ++I) T[232 + I] = Media[I];
    return T;
  }();
  return Table;
}
inline uint16_t HidUsageToKey(uint8_t Usage) { return HidKeyTable()[Usage]; }

// ---- The device -----------------------------------------------------------------------------------------------------

class EvdevClient;

struct StoredEffect {
  FfEffect Effect {};
  const EvdevClient* Owner {};
};

struct EvdevDevice {
  std::string Name, Phys, Uniq; // an empty Uniq (or Phys) reads as ENOENT, as a NULL string does
  InputId Id {};
  Bits Prop = MakeBits(INPUT_PROP_MAX);
  Bits Ev = MakeBits(EV_MAX), Key = MakeBits(KEY_MAX), Rel = MakeBits(REL_MAX), Abs = MakeBits(ABS_MAX), Msc = MakeBits(MSC_MAX),
       Sw = MakeBits(SW_MAX), Led = MakeBits(LED_MAX), Snd = MakeBits(SND_MAX), Ff = MakeBits(FF_MAX);
  // The current state (the bitmaps of EVIOCGKEY/LED/SND/SW and the absinfo values).
  Bits KeyState = MakeBits(KEY_MAX), LedState = MakeBits(LED_MAX), SndState = MakeBits(SND_MAX), SwState = MakeBits(SW_MAX);
  std::array<AbsInfo, ABS_MAX + 1> AbsInfos {};
  std::array<int32_t, 2> Rep {}; // REP_DELAY, REP_PERIOD
  uint32_t MaxEffects {};        // 16 for gamepads, 0 otherwise
  std::vector<std::optional<StoredEffect>> Effects;
  uint32_t Gain = 0xffff;
  EvdevClient* Grab {};

  // input_estimate_events_per_packet, approximated: one SYN_REPORT, one event per abs and rel axis, and one for each of
  // the other event types the device has (a key packet holds the changed keys, which the estimate counts once).
  uint32_t EventsPerPacket() const {
    uint32_t Events = 1 + BitCount(Abs) + BitCount(Rel);
    if (BitCount(Key)) ++Events;
    if (BitCount(Msc)) ++Events;
    if (BitCount(Led)) ++Events;
    if (BitCount(Snd)) ++Events;
    if (BitCount(Sw)) ++Events;
    return Events;
  }

  void AddType(uint16_t Type) { SetBit(Ev, Type); }
  void AddKey(uint16_t Code) { AddType(EV_KEY); SetBit(Key, Code); }
  void AddAxis(uint16_t Axis, int32_t Min, int32_t Max, int32_t Fuzz, int32_t Flat) {
    AddType(EV_ABS);
    SetBit(Abs, Axis);
    AbsInfos[Axis] = AbsInfo {0, Min, Max, Fuzz, Flat, 0};
  }
  void AddFf(std::initializer_list<uint16_t> Effects16, uint32_t Max) {
    AddType(EV_FF);
    for (const auto Code : Effects16) SetBit(Ff, Code);
    MaxEffects = Max;
    Effects.assign(Max, std::nullopt);
  }

  // input_handle_event for one event of a packet: returns whether it reaches the clients. Events of a type or code the
  // device does not have, and key or axis events that change nothing, are dropped (input_get_disposition). The state
  // bitmaps and axis values follow. Deliberate difference: no fuzz filtering of axis values.
  bool Apply(const InputEvent& Event) {
    if (Event.Type == EV_SYN) return Event.Code == SYN_REPORT;
    if (Event.Type > EV_MAX || !TestBit(Ev, Event.Type)) return false;
    switch (Event.Type) {
    case EV_KEY: {
      if (!TestBit(Key, Event.Code)) return false;
      const bool Down = TestBit(KeyState, Event.Code);
      if (Event.Value == 2) return Down; // a repeat of a pressed key
      if (Down == (Event.Value != 0)) return false;
      if (Event.Value) SetBit(KeyState, Event.Code); else ClearBit(KeyState, Event.Code);
      return true;
    }
    case EV_ABS: {
      if (Event.Code > ABS_MAX || !TestBit(Abs, Event.Code) || AbsInfos[Event.Code].Value == Event.Value) return false;
      AbsInfos[Event.Code].Value = Event.Value;
      return true;
    }
    case EV_REL: return Event.Code <= REL_MAX && TestBit(Rel, Event.Code);
    case EV_MSC: return Event.Code <= MSC_MAX && TestBit(Msc, Event.Code);
    case EV_LED: {
      if (Event.Code > LED_MAX || !TestBit(Led, Event.Code)) return false;
      if (TestBit(LedState, Event.Code) == (Event.Value != 0)) return false;
      if (Event.Value) SetBit(LedState, Event.Code); else ClearBit(LedState, Event.Code);
      return true;
    }
    case EV_SW: {
      if (Event.Code > SW_MAX || !TestBit(Sw, Event.Code)) return false;
      if (TestBit(SwState, Event.Code) == (Event.Value != 0)) return false;
      if (Event.Value) SetBit(SwState, Event.Code); else ClearBit(SwState, Event.Code);
      return true;
    }
    default: return false;
    }
  }
};

// ---- Factories ------------------------------------------------------------------------------------------------------

enum class GamepadIdentity { Xbox360, SwitchPro };

// The gamepad of npad Index. Buttons follow their position, not their label (user decision): the right face button is
// BTN_EAST, the bottom one BTN_SOUTH, the top one BTN_NORTH, the left one BTN_WEST, for both identities.
inline EvdevDevice MakeGamepad(uint32_t Index, GamepadIdentity Identity = GamepadIdentity::Xbox360) {
  EvdevDevice D;
  D.Phys = "switch2/npad" + std::to_string(Index);
  D.AddType(EV_SYN);
  for (const auto Code : {BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR}) {
    D.AddKey(Code);
  }
  if (Identity == GamepadIdentity::Xbox360) {
    // xpad: Microsoft X-Box 360 pad
    D.Name = "Microsoft X-Box 360 pad";
    D.Id = {BUS_USB, 0x045e, 0x028e, 0x0114};
    for (const auto Axis : {ABS_X, ABS_Y, ABS_RX, ABS_RY}) D.AddAxis(Axis, -32768, 32767, 16, 128);
    for (const auto Axis : {ABS_Z, ABS_RZ}) D.AddAxis(Axis, 0, 255, 0, 0);
  } else {
    // hid-nintendo: the Pro Controller. ZL and ZR are buttons, the capture button is BTN_Z, no analog triggers.
    D.Name = "Nintendo Switch Pro Controller";
    D.Id = {BUS_USB, 0x057e, 0x2009, 0x8001};
    for (const auto Code : {BTN_TL2, BTN_TR2, BTN_Z}) D.AddKey(Code);
    for (const auto Axis : {ABS_X, ABS_Y, ABS_RX, ABS_RY}) D.AddAxis(Axis, -32767, 32767, 250, 500);
  }
  for (const auto Axis : {ABS_HAT0X, ABS_HAT0Y}) D.AddAxis(Axis, -1, 1, 0, 0);
  D.AddFf({FF_RUMBLE, FF_PERIODIC, FF_SQUARE, FF_TRIANGLE, FF_SINE, FF_GAIN}, 16);
  return D;
}

// A USB keyboard: every key of the HID table, the three lock LEDs, key repeat and the scan code.
inline EvdevDevice MakeKeyboard() {
  EvdevDevice D;
  D.Name = "Switch 2 USB Keyboard";
  D.Phys = "switch2/keyboard0";
  D.Id = {BUS_USB, 0x057e, 0x3001, 0x0111}; // fixed, invented (the console does not report the keyboard's ids)
  D.AddType(EV_SYN);
  for (const auto Code : HidKeyTable()) {
    if (Code) D.AddKey(Code);
  }
  D.AddType(EV_MSC);
  SetBit(D.Msc, MSC_SCAN);
  D.AddType(EV_LED);
  for (const auto Code : {LED_NUML, LED_CAPSL, LED_SCROLLL}) SetBit(D.Led, Code);
  D.AddType(EV_REP);
  D.Rep = {250, 33}; // input_enable_softrepeat defaults
  return D;
}

// A USB mouse with a wheel in both directions and three buttons.
inline EvdevDevice MakeMouse() {
  EvdevDevice D;
  D.Name = "Switch 2 USB Mouse";
  D.Phys = "switch2/mouse0";
  D.Id = {BUS_USB, 0x057e, 0x3002, 0x0110};
  D.AddType(EV_SYN);
  for (const auto Code : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}) D.AddKey(Code);
  D.AddType(EV_REL);
  for (const auto Code : {REL_X, REL_Y, REL_HWHEEL, REL_WHEEL}) SetBit(D.Rel, Code);
  D.AddType(EV_MSC);
  SetBit(D.Msc, MSC_SCAN);
  return D;
}

// ---- The client (evdev_client: one per open file) -------------------------------------------------------------------

// The time of a packet on the three clocks a client may ask for, in nanoseconds.
struct ClockTimes {
  int64_t RealtimeNs {}, MonotonicNs {}, BoottimeNs {};
};

enum class ClientClock : int { Realtime = 0, Monotonic = 1, Boottime = 7 };

// A request the runtime passes to the host's vibration: Strong and Weak are 0..65535 (already scaled by the gain).
struct RumbleRequest {
  uint16_t Strong {}, Weak {};
  uint32_t DurationMs {}; // ff_replay.length; 0 plays until stopped
  uint32_t PlayCount {};  // the value of the write: how often the effect plays
  int EffectId {};
  bool Stop {};
  uint16_t DelayMs {};
  struct PeriodicEffect {
    int Magnitude;
    uint32_t Gain;
    uint16_t AttackMs, AttackLevel, FadeMs, FadeLevel;
  };
  std::optional<PeriodicEffect> Periodic {};
};

inline std::pair<uint16_t, uint16_t> RumbleAt(const RumbleRequest& Request, uint64_t ReplayMs) {
  if (ReplayMs < Request.DelayMs) return {};
  if (!Request.Periodic) return {Request.Strong, Request.Weak};
  const auto& Effect = *Request.Periodic;
  const auto Elapsed = ReplayMs - Request.DelayMs;
  const auto Interpolate = [&](uint16_t Level, uint16_t Duration, uint64_t Position) {
    const int End = std::min<int>(Level, 0x7fff);
    return End + (Effect.Magnitude - End) * static_cast<int>(Position) / Duration;
  };
  int Force = Effect.Magnitude;
  // ff-memless applies the magnitude envelope and drives both rumble motors at the same strength.
  if (Elapsed < Effect.AttackMs) {
    Force = Interpolate(Effect.AttackLevel, Effect.AttackMs, Elapsed);
  } else if (Request.DurationMs && Elapsed < Request.DurationMs &&
             Request.DurationMs - Elapsed < Effect.FadeMs) {
    Force = Interpolate(Effect.FadeLevel, Effect.FadeMs, Request.DurationMs - Elapsed);
  }
  const auto Scaled = static_cast<uint16_t>(std::min<uint32_t>(65535, uint32_t(Force) * Effect.Gain / 0x7fff));
  return {Scaled, Scaled};
}

class EvdevClient {
public:
  // The ring holds roundup_pow_of_two(max(64, 8 * events per packet)) events; one slot is never used.
  explicit EvdevClient(const EvdevDevice& Device) : Size {BufferSize(Device)}, Buffer(Size) {}
  EvdevClient(const EvdevClient&) = delete;
  EvdevClient& operator=(const EvdevClient&) = delete;

  static uint32_t BufferSize(const EvdevDevice& Device) {
    return std::bit_ceil(std::max<uint32_t>(64, Device.EventsPerPacket() * 8));
  }

  // Delivers one packet that ends in SYN_REPORT. Events the client's mask filters are left out, and a packet that is
  // empty after filtering sends no SYN_REPORT (evdev_pass_values). A full ring drops everything unread and keeps
  // SYN_DROPPED plus the newest event (__pass_event). The packet becomes readable at its SYN_REPORT, whole.
  void Push(std::span<const InputEvent> Packet, const ClockTimes& Now) {
    if (Revoked) return;
    LastTime = Now;
    for (const auto& Event : Packet) {
      if (Filtered(Event.Type, Event.Code)) continue;
      if (Event.Type == EV_SYN && Event.Code == SYN_REPORT && PacketHead == Head) continue; // drop an empty SYN_REPORT
      InputEvent Stamped = Event;
      Stamp(Stamped, Now);
      PassEvent(Stamped);
      if (Event.Type == EV_SYN && Event.Code == SYN_REPORT) {
        PacketHead = Head;
        ++Changes;
      }
    }
  }

  // Input lost upstream has the same resynchronization protocol as a full evdev ring.
  void Drop(const ClockTimes& Now) {
    Head = Tail = PacketHead = 0;
    constexpr InputEvent Packet[] {{0, 0, EV_SYN, SYN_DROPPED, 0}, {0, 0, EV_SYN, SYN_REPORT, 0}};
    Push(Packet, Now);
  }

  // evdev_hangup: readers and pollers wake when the device or client goes away.
  void Disconnect() {
    Revoked = true;
    ++Changes;
  }

  uint64_t Generation() const { return Changes; }

  // evdev_read. Returns the bytes copied (whole events), a negative errno, or ReadWait when a blocking read must wait.
  int64_t Read(uint8_t* Out, size_t Count, bool NonBlocking) {
    if (Revoked) return Enodev;
    if (Count != 0 && Count < InputEventSize) return Einval;
    if (PacketHead == Tail && NonBlocking) return Eagain;
    if (Count == 0) return 0;
    if (PacketHead == Tail) return ReadWait;
    size_t Done {};
    while (Done + InputEventSize <= Count && PacketHead != Tail) {
      std::memcpy(Out + Done, &Buffer[Tail], InputEventSize);
      Tail = (Tail + 1) & (Size - 1);
      Done += InputEventSize;
    }
    return static_cast<int64_t>(Done);
  }

  // evdev_poll: a live client is writable, a complete packet is readable, and a dead client reports HUP and ERR.
  uint32_t PollMask() const {
    uint32_t Mask = Revoked ? 0x10 | 0x8 : 0x4 | 0x100;
    if (PacketHead != Tail) Mask |= 0x1 | 0x40;
    return Mask;
  }
  bool Readable() const { return PacketHead != Tail; }

  // evdev_set_clk_type flushes pending events. Its SYN_DROPPED becomes readable with the next report.
  int64_t SetClock(int32_t Id, const ClockTimes& Now) {
    if (Id != 0 && Id != 1 && Id != 7) return Einval;
    if (static_cast<int>(Clock) != Id) {
      Clock = static_cast<ClientClock>(Id);
      if (Head != Tail) {
        Head = PacketHead = Tail;
        InputEvent Dropped {0, 0, EV_SYN, SYN_DROPPED, 0};
        Stamp(Dropped, Now);
        PassEvent(Dropped);
      }
    }
    return 0;
  }
  ClientClock Clock = ClientClock::Realtime;

  // evdev_release: the client's grab and force-feedback effects go away with it.
  void Close(EvdevDevice& Device) {
    if (Device.Grab == this) Device.Grab = nullptr;
    for (auto& Slot : Device.Effects) {
      if (Slot && Slot->Owner == this) Slot.reset();
    }
  }

  // The event mask of a type (EVIOCSMASK); nothing set passes everything.
  std::array<std::optional<Bits>, EV_MAX + 1> Masks {};
  bool Revoked = false;
  ClockTimes LastTime; // the time of the latest packet, which stamps a SYN_DROPPED made by a clock change

private:
  bool Filtered(uint16_t Type, uint16_t Code) const {
    // __evdev_is_filtered: the type mask (the mask of EV_SYN holds event types), then the mask of the type's codes.
    if (Type == EV_SYN || Type > EV_MAX) return false;
    if (Masks[0] && !TestBit(*Masks[0], Type)) return true;
    return Masks[Type] && !TestBit(*Masks[Type], Code);
  }
  void Stamp(InputEvent& Event, const ClockTimes& Now) const {
    const int64_t Ns = Clock == ClientClock::Realtime ? Now.RealtimeNs : Clock == ClientClock::Monotonic ? Now.MonotonicNs : Now.BoottimeNs;
    Event.Sec = Ns / 1000000000;
    Event.Usec = Ns % 1000000000 / 1000;
  }
  void PassEvent(const InputEvent& Event) {
    Buffer[Head++] = Event;
    Head &= Size - 1;
    if (Head == Tail) {
      // Drop everything unread; keep SYN_DROPPED and the newest event. The packet restarts at the drop marker.
      Tail = (Head - 2) & (Size - 1);
      Buffer[Tail] = InputEvent {Event.Sec, Event.Usec, EV_SYN, SYN_DROPPED, 0};
      PacketHead = Tail;
    }
  }

  uint32_t Size;
  std::vector<InputEvent> Buffer;
  uint32_t Head {}, Tail {}, PacketHead {};
  uint64_t Changes {};
};

// Hands one packet of a device to its clients: the device state is updated, events it does not pass are dropped, and
// a grabbing client receives it alone. Packet should end in SYN_REPORT.
inline void Deliver(EvdevDevice& Device, const std::vector<EvdevClient*>& Clients, std::span<const InputEvent> Packet, const ClockTimes& Now) {
  std::vector<InputEvent> Passed;
  for (const auto& Event : Packet) {
    if (Device.Apply(Event)) Passed.push_back(Event);
  }
  if (Passed.empty()) return;
  if (Device.Grab) {
    Device.Grab->Push(Passed, Now);
    return;
  }
  for (auto* Client : Clients) Client->Push(Passed, Now);
}

// ---- Writes (evdev_write / input_ff_event) --------------------------------------------------------------------------

// Writes input_events to the device: EV_FF events play or stop effects (the requests go to Rumble), FF_GAIN sets the gain,
// everything else is accepted and ignored (evdev forwards it to the driver). A client other than the grabber is ignored
// while the device is grabbed (input_inject_event). Returns the bytes consumed or a negative errno.
template<typename SendRumble>
int64_t Write(EvdevDevice& Device, EvdevClient& Client, const uint8_t* Data, size_t Count, SendRumble Rumble) {
  if (Client.Revoked) return Enodev;
  if (Count != 0 && Count < InputEventSize) return Einval;
  size_t Done {};
  const bool Allowed = !Device.Grab || Device.Grab == &Client;
  while (Done + InputEventSize <= Count) {
    InputEvent Event;
    std::memcpy(&Event, Data + Done, InputEventSize);
    Done += InputEventSize;
    if (!Allowed || Event.Type != EV_FF || !TestBit(Device.Ev, EV_FF)) continue;
    if (Event.Code == FF_GAIN) {
      if (TestBit(Device.Ff, FF_GAIN) && Event.Value >= 0 && Event.Value <= 0xffff) Device.Gain = static_cast<uint32_t>(Event.Value);
      continue;
    }
    if (Event.Code >= Device.Effects.size() || !Device.Effects[Event.Code] || Device.Effects[Event.Code]->Owner != &Client) continue;
    const auto& Effect = Device.Effects[Event.Code]->Effect;
    RumbleRequest Request;
    Request.EffectId = Event.Code;
    if (Event.Value <= 0) {
      Request.Stop = true;
    } else {
      Request.Strong = static_cast<uint16_t>(uint64_t {Effect.Strong()} * Device.Gain / 0xffff);
      Request.Weak = static_cast<uint16_t>(uint64_t {Effect.Weak()} * Device.Gain / 0xffff);
      Request.DurationMs = Effect.ReplayLength;
      Request.DelayMs = Effect.ReplayDelay;
      Request.PlayCount = static_cast<uint32_t>(Event.Value);
      if (Effect.Type == FF_PERIODIC) {
        const int Magnitude = Effect.Field<int16_t>(4);
        Request.Periodic = RumbleRequest::PeriodicEffect {
          Magnitude < 0 ? -Magnitude : Magnitude, Device.Gain,
          Effect.Field(10), Effect.Field(12), Effect.Field(14), Effect.Field(16)};
      }
    }
    Rumble(Request);
  }
  return static_cast<int64_t>(Done);
}

// ---- ioctl ----------------------------------------------------------------------------------------------------------

inline constexpr uint32_t IocNr(uint32_t R) { return R & 0xff; }
inline constexpr uint32_t IocType(uint32_t R) { return R >> 8 & 0xff; }
inline constexpr uint32_t IocSize(uint32_t R) { return R >> 16 & 0x3fff; }
inline constexpr uint32_t IocDir(uint32_t R) { return R >> 30; } // 1 write, 2 read
inline constexpr uint32_t Ioc(uint32_t Dir, uint32_t Nr, uint32_t Size) { return Dir << 30 | Size << 16 | 'E' << 8 | Nr; }
inline constexpr uint32_t IocRead = 2, IocWrite = 1;

inline constexpr uint32_t EVIOCGVERSION = Ioc(IocRead, 0x01, 4);
inline constexpr uint32_t EVIOCGID = Ioc(IocRead, 0x02, 8);
inline constexpr uint32_t EVIOCGREP = Ioc(IocRead, 0x03, 8);
inline constexpr uint32_t EVIOCSREP = Ioc(IocWrite, 0x03, 8);
inline constexpr uint32_t EVIOCSFF = Ioc(IocWrite, 0x80, 48);
inline constexpr uint32_t EVIOCRMFF = Ioc(IocWrite, 0x81, 4);
inline constexpr uint32_t EVIOCGEFFECTS = Ioc(IocRead, 0x84, 4);
inline constexpr uint32_t EVIOCGRAB = Ioc(IocWrite, 0x90, 4);
inline constexpr uint32_t EVIOCREVOKE = Ioc(IocWrite, 0x91, 4);
inline constexpr uint32_t EVIOCGMASK = Ioc(IocRead, 0x92, 16);
inline constexpr uint32_t EVIOCSMASK = Ioc(IocWrite, 0x92 + 1, 16);
inline constexpr uint32_t EVIOCSCLOCKID = Ioc(IocWrite, 0xa0, 4);
// The variable-length requests: pass the buffer size as Len.
inline constexpr uint32_t EVIOCGNAME(uint32_t Len) { return Ioc(IocRead, 0x06, Len); }
inline constexpr uint32_t EVIOCGPHYS(uint32_t Len) { return Ioc(IocRead, 0x07, Len); }
inline constexpr uint32_t EVIOCGUNIQ(uint32_t Len) { return Ioc(IocRead, 0x08, Len); }
inline constexpr uint32_t EVIOCGPROP(uint32_t Len) { return Ioc(IocRead, 0x09, Len); }
inline constexpr uint32_t EVIOCGKEY(uint32_t Len) { return Ioc(IocRead, 0x18, Len); }
inline constexpr uint32_t EVIOCGLED(uint32_t Len) { return Ioc(IocRead, 0x19, Len); }
inline constexpr uint32_t EVIOCGSND(uint32_t Len) { return Ioc(IocRead, 0x1a, Len); }
inline constexpr uint32_t EVIOCGSW(uint32_t Len) { return Ioc(IocRead, 0x1b, Len); }
inline constexpr uint32_t EVIOCGBIT(uint32_t Ev, uint32_t Len) { return Ioc(IocRead, 0x20 + Ev, Len); }
inline constexpr uint32_t EVIOCGABS(uint32_t Axis) { return Ioc(IocRead, 0x40 + Axis, 24); }
inline constexpr uint32_t EVIOCSABS(uint32_t Axis) { return Ioc(IocWrite, 0xc0 + Axis, 24); }

inline constexpr bool IsSetAbsIoctl(uint32_t Request) {
  return IocType(Request) == 'E' && IocDir(Request) == IocWrite &&
         (IocNr(Request) & ~uint32_t {ABS_MAX}) == IocNr(EVIOCSABS(0));
}

namespace Detail {
// check_effect_access distinguishes a missing effect from one owned by another client.
inline int CheckEffectAccess(const EvdevDevice& Device, const EvdevClient& Client, int Id) {
  if (Id < 0 || Id >= static_cast<int>(Device.Effects.size()) || !Device.Effects[Id]) return Einval;
  return Device.Effects[Id]->Owner == &Client ? 0 : Eacces;
}

// bits_to_user: BITS_TO_LONGS(MaxBit) longs, cut to the buffer. Returns the bytes copied.
inline int64_t BitsToUser(const Bits& Source, uint32_t MaxBit, std::span<uint8_t> Out) {
  const size_t Wanted = std::min<size_t>((MaxBit + 63) / 64 * 8, Out.size());
  std::memset(Out.data(), 0, Wanted);
  std::memcpy(Out.data(), Source.data(), std::min(Wanted, Source.size() * 8));
  return static_cast<int64_t>(Wanted);
}
// str_to_user: the string with its NUL, cut to the buffer. An empty string is NULL: ENOENT.
inline int64_t StringToUser(const std::string& Text, std::span<uint8_t> Out) {
  if (Text.empty()) return Enoent;
  const size_t Len = std::min(Text.size() + 1, Out.size());
  std::memcpy(Out.data(), Text.c_str(), Len);
  return static_cast<int64_t>(Len);
}
template<typename T> inline bool Store(std::span<uint8_t> Out, const T& Value) {
  if (Out.size() < sizeof(T)) return false;
  std::memcpy(Out.data(), &Value, sizeof(T));
  return true;
}
template<typename T> inline bool Load(std::span<const uint8_t> In, T& Value) {
  if (In.size() < sizeof(T)) return false;
  std::memcpy(&Value, In.data(), sizeof(T));
  return true;
}
// evdev_get_mask_cnt: the number of codes of a type's mask (0 for a type without one).
inline uint32_t MaskCount(uint32_t Type) {
  switch (Type) {
  case EV_SYN: return EV_MAX + 1;
  case EV_KEY: return KEY_MAX + 1;
  case EV_REL: return REL_MAX + 1;
  case EV_ABS: return ABS_MAX + 1;
  case EV_MSC: return MSC_MAX + 1;
  case EV_SW: return SW_MAX + 1;
  case EV_LED: return LED_MAX + 1;
  case EV_SND: return SND_MAX + 1;
  default: return 0;
  }
}
} // namespace Detail

// evdev_do_ioctl on byte buffers. The runtime sizes Arg to the request's size field, copies the guest argument in for
// every request that has a size (a read-only EVIOCGMASK also reads its argument), and afterwards copies CopyOut bytes
// back (the bytes a read request filled). Aux is the memory behind input_mask.codes_ptr, codes_size bytes: read by
// EVIOCSMASK, written by EVIOCGMASK. Returns 0, a byte count, or a negative errno.
inline int64_t EvdevIoctl(EvdevDevice& Device, EvdevClient& Client, uint32_t Request, std::span<uint8_t> Arg, size_t* CopyOut = nullptr,
                          std::span<uint8_t> Aux = {}) {
  size_t Scratch {};
  size_t& Copy = CopyOut ? *CopyOut : Scratch;
  Copy = 0;
  if (Client.Revoked) return Enodev;
  if (IocType(Request) != 'E') return Enotty;
  const uint32_t Size = IocSize(Request);
  if (Arg.size() < Size) return Einval;
  Arg = Arg.first(Size);
  const auto Done = [&](int64_t Result, size_t Bytes) {
    Copy = Result >= 0 ? Bytes : 0;
    return Result;
  };

  // Fixed requests (matched with their size, as the kernel's switch does).
  switch (Request) {
  case EVIOCGVERSION: Detail::Store(Arg, uint32_t {EvVersion}); return Done(0, 4);
  case EVIOCGID: Detail::Store(Arg, Device.Id); return Done(0, sizeof(InputId));
  case EVIOCGREP:
    if (!TestBit(Device.Ev, EV_REP)) return Enosys;
    Detail::Store(Arg, Device.Rep);
    return Done(0, 8);
  case EVIOCSREP: {
    if (!TestBit(Device.Ev, EV_REP)) return Enosys;
    std::array<int32_t, 2> Value;
    Detail::Load(Arg, Value);
    Device.Rep = Value;
    return 0;
  }
  case EVIOCSFF: {
    // input_ff_upload
    FfEffect Effect;
    Detail::Load(Arg, Effect);
    if (!TestBit(Device.Ev, EV_FF)) return Enosys;
    if ((Effect.Type != FF_RUMBLE && Effect.Type != FF_PERIODIC) || !TestBit(Device.Ff, Effect.Type)) return Einval;
    if (Effect.Type == FF_PERIODIC && (Effect.Field(0) < FF_SQUARE || Effect.Field(0) > FF_SINE ||
                                       !TestBit(Device.Ff, Effect.Field(0)))) return Einval;
    int Slot = Effect.Id;
    if (Slot == -1) {
      Slot = 0;
      while (Slot < static_cast<int>(Device.Effects.size()) && Device.Effects[Slot]) ++Slot;
      if (Slot >= static_cast<int>(Device.Effects.size())) return Enospc;
    } else {
      if (const auto Error = Detail::CheckEffectAccess(Device, Client, Slot)) return Error;
      const auto& Old = Device.Effects[Slot]->Effect;
      if (Old.Type != Effect.Type || (Effect.Type == FF_PERIODIC && Old.Field(0) != Effect.Field(0))) return Einval;
    }
    Effect.Id = static_cast<int16_t>(Slot);
    Device.Effects[Slot] = StoredEffect {Effect, &Client};
    Detail::Store(Arg, Effect);
    return Done(0, sizeof(FfEffect));
  }
  case EVIOCRMFF: {
    int32_t Id;
    Detail::Load(Arg, Id);
    if (!TestBit(Device.Ev, EV_FF)) return Enosys;
    if (const auto Error = Detail::CheckEffectAccess(Device, Client, Id)) return Error;
    Device.Effects[Id].reset();
    return 0;
  }
  case EVIOCGEFFECTS:
    Detail::Store(Arg, static_cast<int32_t>(TestBit(Device.Ev, EV_FF) ? Device.MaxEffects : 0));
    return Done(0, 4);
  case EVIOCGRAB: {
    int32_t Grab;
    Detail::Load(Arg, Grab);
    if (Grab) {
      if (Device.Grab) return Ebusy; // already grabbed (by this client too: input_grab_device)
      Device.Grab = &Client;
      return 0;
    }
    if (Device.Grab != &Client) return Einval;
    Device.Grab = nullptr;
    return 0;
  }
  case EVIOCREVOKE: {
    int32_t Value;
    Detail::Load(Arg, Value);
    if (Value) return Einval;
    if (Device.Grab == &Client) Device.Grab = nullptr;
    Client.Disconnect();
    return 0;
  }
  case EVIOCGMASK: case EVIOCSMASK: {
    InputMask Mask;
    Detail::Load(Arg, Mask);
    const uint32_t Count = Detail::MaskCount(Mask.Type);
    if (Request == EVIOCSMASK) {
      // evdev_set_mask: unknown types are ignored, no codes clears the mask, extra codes beyond the type are ignored.
      if (!Count) return 0;
      if (Mask.CodesSize == 0) {
        Client.Masks[Mask.Type].reset();
        return 0;
      }
      if (Aux.size() < Mask.CodesSize) return -14;
      Bits Fresh = MakeBits(Count - 1);
      std::memcpy(Fresh.data(), Aux.data(), std::min<size_t>({Mask.CodesSize, Fresh.size() * 8}));
      Client.Masks[Mask.Type] = std::move(Fresh);
      return 0;
    }
    // evdev_get_mask: a type without a mask reads as all bits set; the rest of the buffer is zeroed.
    if (!Count) return 0;
    if (Aux.size() < Mask.CodesSize) return -14;
    const size_t Words = (Count + 63) / 64 * 8;
    const size_t Transfer = std::min<size_t>(Mask.CodesSize, Words);
    if (Client.Masks[Mask.Type]) std::memcpy(Aux.data(), Client.Masks[Mask.Type]->data(), Transfer);
    else std::memset(Aux.data(), 0xff, Transfer);
    if (Transfer < Mask.CodesSize) std::memset(Aux.data() + Transfer, 0, Mask.CodesSize - Transfer);
    return 0;
  }
  case EVIOCSCLOCKID: {
    int32_t Id;
    Detail::Load(Arg, Id);
    return Client.SetClock(Id, Client.LastTime);
  }
  default: break;
  }

  // Variable-length reads: the request without its size field.
  const uint32_t Nr = IocNr(Request);
  if (IocDir(Request) == IocRead) {
    switch (Nr) {
    case 0x06: { const auto R = Detail::StringToUser(Device.Name, Arg); return Done(R, R); }
    case 0x07: { const auto R = Detail::StringToUser(Device.Phys, Arg); return Done(R, R); }
    case 0x08: { const auto R = Detail::StringToUser(Device.Uniq, Arg); return Done(R, R); }
    case 0x09: { const auto R = Detail::BitsToUser(Device.Prop, INPUT_PROP_MAX, Arg); return Done(R, R); }
    case 0x18: { const auto R = Detail::BitsToUser(Device.KeyState, KEY_MAX, Arg); return Done(R, R); }
    case 0x19: { const auto R = Detail::BitsToUser(Device.LedState, LED_MAX, Arg); return Done(R, R); }
    case 0x1a: { const auto R = Detail::BitsToUser(Device.SndState, SND_MAX, Arg); return Done(R, R); }
    case 0x1b: { const auto R = Detail::BitsToUser(Device.SwState, SW_MAX, Arg); return Done(R, R); }
    default: break;
    }
    if ((Nr & ~uint32_t {EV_MAX}) == 0x20) { // EVIOCGBIT(ev, len)
      const Bits* Source {};
      uint32_t MaxBit {};
      switch (Nr & EV_MAX) {
      case 0: Source = &Device.Ev; MaxBit = EV_MAX; break;
      case EV_KEY: Source = &Device.Key; MaxBit = KEY_MAX; break;
      case EV_REL: Source = &Device.Rel; MaxBit = REL_MAX; break;
      case EV_ABS: Source = &Device.Abs; MaxBit = ABS_MAX; break;
      case EV_MSC: Source = &Device.Msc; MaxBit = MSC_MAX; break;
      case EV_SW: Source = &Device.Sw; MaxBit = SW_MAX; break;
      case EV_LED: Source = &Device.Led; MaxBit = LED_MAX; break;
      case EV_SND: Source = &Device.Snd; MaxBit = SND_MAX; break;
      case EV_FF: Source = &Device.Ff; MaxBit = FF_MAX; break;
      default: return Einval;
      }
      const auto R = Detail::BitsToUser(*Source, MaxBit, Arg);
      return Done(R, R);
    }
    if ((Nr & ~uint32_t {ABS_MAX}) == 0x40) { // EVIOCGABS(axis): the size may be smaller than input_absinfo
      if (!TestBit(Device.Ev, EV_ABS)) return Einval;
      const auto Info = Device.AbsInfos[Nr & ABS_MAX];
      const size_t Bytes = std::min<size_t>(Size, sizeof(Info));
      std::memcpy(Arg.data(), &Info, Bytes);
      return Done(0, Bytes);
    }
  }
  if (IsSetAbsIoctl(Request)) { // EVIOCSABS(axis)
    if (!TestBit(Device.Ev, EV_ABS)) return Einval;
    AbsInfo Info = Device.AbsInfos[Nr & ABS_MAX];
    std::memcpy(&Info, Arg.data(), std::min<size_t>(Size, sizeof(Info)));
    Device.AbsInfos[Nr & ABS_MAX] = Info;
    return 0;
  }
  return Einval; // an unknown 'E' request
}

} // namespace Switch2Guest::Evdev
