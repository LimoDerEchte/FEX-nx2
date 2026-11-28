// SPDX-License-Identifier: MIT
#include "GuestEvdev.h"
#include "GuestPosixAcl.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(Cond) do { if (!(Cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #Cond); std::exit(1); } } while (0)

using namespace Switch2Guest::Evdev;

static const ClockTimes Time {5'000'000'123'000, 7'000'000'456'000, 9'000'000'789'000};

static InputEvent Ev(uint16_t Type, uint16_t Code, int32_t Value) { return InputEvent {0, 0, Type, Code, Value}; }
static InputEvent Syn() { return Ev(EV_SYN, SYN_REPORT, 0); }

static void TestSeatAccessAcl() {
  using namespace Switch2Guest;
  constexpr char Expected[] =
    "\x02\x00\x00\x00"
    "\x01\x00\x06\x00\xff\xff\xff\xff"
    "\x02\x00\x06\x00\x78\x56\x34\x12"
    "\x04\x00\x06\x00\xff\xff\xff\xff"
    "\x10\x00\x06\x00\xff\xff\xff\xff"
    "\x20\x00\x00\x00\xff\xff\xff\xff";
  const auto Bytes = EncodeSeatAccessAcl(0x12345678);
  CHECK(Bytes == std::string_view(Expected, sizeof(Expected) - 1));
  CHECK(EncodeSeatAccessAcl(0).substr(16, 4) == std::string(4, '\0'));
  const auto Layout = ParsePosixAcl(Bytes);
  CHECK(Layout && Layout->Extended && Layout->Mode(Bytes) == 0660);
  const auto NoGroup = [](uint32_t) { return false; };
  CHECK(Layout->Allows(Bytes, 0, 100, 0x12345678, NoGroup, 6));
  CHECK(!Layout->Allows(Bytes, 0, 100, 0x12345678, NoGroup, 1));
  CHECK(!Layout->Allows(Bytes, 0, 100, 1000, NoGroup, 4));
}

// Runs an ioctl with a buffer of the request's size.
static int64_t Ioctl(EvdevDevice& D, EvdevClient& C, uint32_t Request, std::vector<uint8_t>& Buffer, size_t* Copy = nullptr, std::span<uint8_t> Aux = {}) {
  Buffer.resize(IocSize(Request));
  return EvdevIoctl(D, C, Request, Buffer, Copy, Aux);
}

static void TestLayouts() {
  CHECK(sizeof(InputEvent) == 24 && sizeof(InputId) == 8 && sizeof(AbsInfo) == 24 && sizeof(FfEffect) == 48);
  CHECK(offsetof(FfEffect, Union) == 16);
  CHECK(EVIOCGVERSION == 0x80044501);
  CHECK(EVIOCGID == 0x80084502);
  CHECK(EVIOCSFF == 0x40304580);
  CHECK(EVIOCRMFF == 0x40044581);
  CHECK(EVIOCGEFFECTS == 0x80044584);
  CHECK(EVIOCGRAB == 0x40044590);
  CHECK(EVIOCREVOKE == 0x40044591);
  CHECK(EVIOCGMASK == 0x80104592);
  CHECK(EVIOCSMASK == 0x40104593);
  CHECK(EVIOCSCLOCKID == 0x400445a0);
  CHECK(EVIOCGABS(ABS_X) == 0x80184540);
  CHECK(EVIOCGBIT(EV_KEY, 96) == 0x80604521);
}

static void TestHid() {
  CHECK(HidUsageToKey(0x04) == 30);
  CHECK(HidUsageToKey(0x28) == 28);
  CHECK(HidUsageToKey(0xe1) == 42);
  CHECK(HidUsageToKey(0x1d) == 44);  // Z
  CHECK(HidUsageToKey(0x27) == 11);  // 0
  CHECK(HidUsageToKey(0x2c) == 57);  // space
  CHECK(HidUsageToKey(0x39) == 58);  // caps lock
  CHECK(HidUsageToKey(0x3a) == 59);  // F1
  CHECK(HidUsageToKey(0x45) == 88);  // F12
  CHECK(HidUsageToKey(0x4f) == 106); // right arrow
  CHECK(HidUsageToKey(0x52) == 103); // up arrow
  CHECK(HidUsageToKey(0x62) == 82);  // keypad 0
  CHECK(HidUsageToKey(0xe0) == 29 && HidUsageToKey(0xe7) == 126);
  CHECK(HidUsageToKey(0x00) == 0 && HidUsageToKey(0x01) == 0);
}

static void TestFactories() {
  const auto Xbox = MakeGamepad(2);
  CHECK(Xbox.Name == "Microsoft X-Box 360 pad" && Xbox.Phys == "switch2/npad2");
  CHECK(Xbox.Id.Bustype == 3 && Xbox.Id.Vendor == 0x045e && Xbox.Id.Product == 0x028e && Xbox.Id.Version == 0x0114);
  CHECK(TestBit(Xbox.Key, BTN_SOUTH) && TestBit(Xbox.Key, BTN_EAST) && TestBit(Xbox.Key, BTN_NORTH) && TestBit(Xbox.Key, BTN_WEST));
  CHECK(!TestBit(Xbox.Key, BTN_TL2) && TestBit(Xbox.Key, BTN_THUMBR) && TestBit(Xbox.Key, BTN_MODE));
  CHECK(Xbox.AbsInfos[ABS_X].Minimum == -32768 && Xbox.AbsInfos[ABS_X].Maximum == 32767 && Xbox.AbsInfos[ABS_X].Fuzz == 16 &&
        Xbox.AbsInfos[ABS_X].Flat == 128);
  CHECK(Xbox.AbsInfos[ABS_Z].Minimum == 0 && Xbox.AbsInfos[ABS_RZ].Maximum == 255);
  CHECK(Xbox.AbsInfos[ABS_HAT0X].Minimum == -1 && Xbox.AbsInfos[ABS_HAT0Y].Maximum == 1);
  CHECK(Xbox.MaxEffects == 16 && TestBit(Xbox.Ev, EV_FF) && TestBit(Xbox.Ff, FF_RUMBLE) && TestBit(Xbox.Ff, FF_GAIN));
  CHECK(BitCount(Xbox.Abs) == 8);
  const auto Pro = MakeGamepad(0, GamepadIdentity::SwitchPro);
  CHECK(Pro.Name == "Nintendo Switch Pro Controller" && Pro.Id.Vendor == 0x057e && Pro.Id.Product == 0x2009);
  CHECK(TestBit(Pro.Key, BTN_SOUTH) && TestBit(Pro.Key, BTN_EAST) && TestBit(Pro.Key, BTN_NORTH) && TestBit(Pro.Key, BTN_WEST));
  CHECK(TestBit(Pro.Key, BTN_TL2) && TestBit(Pro.Key, BTN_TR2) && TestBit(Pro.Key, BTN_Z));
  CHECK(!TestBit(Pro.Abs, ABS_Z) && TestBit(Pro.Abs, ABS_HAT0X) && Pro.AbsInfos[ABS_X].Maximum == 32767 && Pro.AbsInfos[ABS_X].Fuzz == 250);

  const auto Keyboard = MakeKeyboard();
  CHECK(TestBit(Keyboard.Key, 30) && TestBit(Keyboard.Key, 28) && TestBit(Keyboard.Key, 42) && !TestBit(Keyboard.Key, BTN_SOUTH));
  CHECK(TestBit(Keyboard.Led, LED_CAPSL) && TestBit(Keyboard.Ev, EV_REP) && TestBit(Keyboard.Msc, MSC_SCAN) && Keyboard.MaxEffects == 0);
  CHECK(Keyboard.Rep[0] == 250 && Keyboard.Rep[1] == 33);
  const auto Mouse = MakeMouse();
  CHECK(TestBit(Mouse.Rel, REL_X) && TestBit(Mouse.Rel, REL_WHEEL) && TestBit(Mouse.Rel, REL_HWHEEL) && TestBit(Mouse.Key, BTN_MIDDLE));
  CHECK(!TestBit(Mouse.Ev, EV_ABS) && Mouse.MaxEffects == 0);
}

static void TestFramingAndState() {
  auto Pad = MakeGamepad(0);
  EvdevClient Client(Pad);
  std::vector<EvdevClient*> Clients {&Client};
  const InputEvent Packet[] = {Ev(EV_KEY, BTN_SOUTH, 1), Ev(EV_ABS, ABS_X, 1000), Syn()};
  Deliver(Pad, Clients, Packet, Time);
  uint8_t Out[24 * 8];
  CHECK(Client.Read(Out, 23, true) == Einval);
  CHECK(Client.Read(Out, 0, true) == 0);
  // A buffer for two events reads two; the packet is delivered whole, so a third read gets the SYN_REPORT.
  CHECK(Client.Read(Out, 48, true) == 48);
  InputEvent Got[2];
  std::memcpy(Got, Out, 48);
  CHECK(Got[0].Type == EV_KEY && Got[0].Code == BTN_SOUTH && Got[0].Value == 1);
  CHECK(Got[1].Type == EV_ABS && Got[1].Value == 1000);
  CHECK(Got[0].Sec == 5000 && Got[0].Usec == 123);
  CHECK(Client.Read(Out, 100, true) == 24); // whole events only
  CHECK(Client.Read(Out, 24, true) == Eagain);
  CHECK(Client.Read(Out, 24, false) == ReadWait);
  CHECK(!Client.Readable());
  // State.
  std::vector<uint8_t> Buffer;
  size_t Copy {};
  CHECK(Ioctl(Pad, Client, EVIOCGKEY(96), Buffer, &Copy) == 96 && Copy == 96);
  CHECK(Buffer[BTN_SOUTH / 8] >> (BTN_SOUTH % 8) & 1);
  CHECK(Ioctl(Pad, Client, EVIOCGABS(ABS_X), Buffer, &Copy) == 0 && Copy == 24);
  AbsInfo Info;
  std::memcpy(&Info, Buffer.data(), 24);
  CHECK(Info.Value == 1000 && Info.Maximum == 32767);
  // Duplicates and unsupported events are dropped; a packet without changes sends nothing.
  const InputEvent Same[] = {Ev(EV_KEY, BTN_SOUTH, 1), Ev(EV_ABS, ABS_X, 1000), Ev(EV_KEY, BTN_TL2, 1), Syn()};
  Deliver(Pad, Clients, Same, Time);
  CHECK(!Client.Readable());
  // A release is delivered and clears the state.
  const InputEvent Release[] = {Ev(EV_KEY, BTN_SOUTH, 0), Syn()};
  Deliver(Pad, Clients, Release, Time);
  CHECK(Client.Read(Out, sizeof(Out), true) == 48);
  CHECK(Client.Read(Out, sizeof(Out), true) == Eagain);
  CHECK(Ioctl(Pad, Client, EVIOCGKEY(96), Buffer) == 96 && !(Buffer[BTN_SOUTH / 8] >> (BTN_SOUTH % 8) & 1));
}

static void TestOverflow() {
  auto Pad = MakeGamepad(0);
  EvdevClient Client(Pad);
  // 100 packets of two events (key then SYN): the ring (127 usable slots) overflows.
  for (int I = 0; I < 100; ++I) {
    const InputEvent Packet[] = {Ev(EV_ABS, ABS_X, I), Syn()};
    Client.Push(Packet, Time);
  }
  uint8_t Out[24 * 256];
  const auto Bytes = Client.Read(Out, sizeof(Out), true);
  CHECK(Bytes > 0 && Bytes % 24 == 0);
  InputEvent First;
  std::memcpy(&First, Out, 24);
  // The queue holds a SYN_DROPPED first and ends in a whole packet.
  CHECK(First.Type == EV_SYN && First.Code == SYN_DROPPED);
  InputEvent Last;
  std::memcpy(&Last, Out + Bytes - 24, 24);
  CHECK(Last.Type == EV_SYN && Last.Code == SYN_REPORT);
  CHECK(Bytes / 24 < 128);
  // The newest packet survives.
  InputEvent Newest;
  std::memcpy(&Newest, Out + Bytes - 48, 24);
  CHECK(Newest.Type == EV_ABS && Newest.Value == 99);
  // Overflow in a packet whose event arrives when the ring is exactly full leaves SYN_DROPPED, then the SYN_REPORT.
  EvdevClient Small(Pad);
  for (int I = 0; I < 63; ++I) {
    const InputEvent One[] = {Ev(EV_ABS, ABS_X, I), Syn()};
    Small.Push(One, Time);
  }
  const InputEvent Full[] = {Ev(EV_ABS, ABS_X, 63), Syn()};
  Small.Push(Full, Time);
  CHECK(Small.Read(Out, sizeof(Out), true) == 48);
  InputEvent Boundary[2];
  std::memcpy(Boundary, Out, sizeof(Boundary));
  CHECK(Boundary[0].Type == EV_SYN && Boundary[0].Code == SYN_DROPPED);
  CHECK(Boundary[1].Type == EV_SYN && Boundary[1].Code == SYN_REPORT);
  CHECK(Small.Read(Out, sizeof(Out), true) == Eagain);
}

static void TestMasks() {
  auto Pad = MakeGamepad(0);
  EvdevClient Client(Pad);
  std::vector<EvdevClient*> Clients {&Client};
  std::vector<uint8_t> Buffer;
  // Allow only EV_SYN and EV_ABS events (the type mask bits 0 and 3).
  InputMask Mask {0, 4, 0};
  std::memcpy(Buffer.data(), &Mask, 0);
  uint8_t Bits[4] = {0x09, 0, 0, 0};
  Buffer.resize(16);
  std::memcpy(Buffer.data(), &Mask, 16);
  CHECK(EvdevIoctl(Pad, Client, EVIOCSMASK, Buffer, nullptr, Bits) == 0);
  const InputEvent Packet[] = {Ev(EV_KEY, BTN_EAST, 1), Ev(EV_ABS, ABS_Y, 5), Syn()};
  Deliver(Pad, Clients, Packet, Time);
  uint8_t Out[24 * 4];
  CHECK(Client.Read(Out, sizeof(Out), true) == 48); // ABS and SYN; the key was masked
  InputEvent First;
  std::memcpy(&First, Out, 24);
  CHECK(First.Type == EV_ABS);
  // A packet that is empty after masking sends no SYN_REPORT.
  const InputEvent KeyOnly[] = {Ev(EV_KEY, BTN_EAST, 0), Syn()};
  Deliver(Pad, Clients, KeyOnly, Time);
  CHECK(!Client.Readable());
  // Read the mask back: the types set, then the unset KEY mask reads all ones.
  uint8_t Back[4] {};
  CHECK(EvdevIoctl(Pad, Client, EVIOCGMASK, Buffer, nullptr, Back) == 0);
  CHECK(Back[0] == 0x09 && Back[1] == 0);
  InputMask KeyMask {EV_KEY, 8, 0};
  std::memcpy(Buffer.data(), &KeyMask, 16);
  uint8_t KeyBack[8] {};
  CHECK(EvdevIoctl(Pad, Client, EVIOCGMASK, Buffer, nullptr, KeyBack) == 0);
  CHECK(KeyBack[0] == 0xff && KeyBack[7] == 0xff);
  // codes_size larger than the mask zero-fills the rest.
  InputMask Wide {0, 16, 0};
  std::memcpy(Buffer.data(), &Wide, 16);
  uint8_t WideBack[16];
  std::memset(WideBack, 0x55, sizeof(WideBack));
  CHECK(EvdevIoctl(Pad, Client, EVIOCGMASK, Buffer, nullptr, WideBack) == 0);
  CHECK(WideBack[0] == 0x09 && WideBack[8] == 0 && WideBack[15] == 0);
  // codes_size 0 clears the mask; an unknown type is accepted and ignored.
  InputMask Clear {0, 0, 0};
  std::memcpy(Buffer.data(), &Clear, 16);
  CHECK(EvdevIoctl(Pad, Client, EVIOCSMASK, Buffer) == 0);
  CHECK(!Client.Masks[0]);
  InputMask Unknown {EV_FF, 4, 0};
  std::memcpy(Buffer.data(), &Unknown, 16);
  CHECK(EvdevIoctl(Pad, Client, EVIOCSMASK, Buffer, nullptr, Bits) == 0);
}

static void TestClocks() {
  auto Pad = MakeGamepad(0);
  EvdevClient Realtime(Pad), Monotonic(Pad), Boottime(Pad);
  std::vector<uint8_t> Buffer(4);
  const auto SetClock = [&](EvdevClient& C, int32_t Id) {
    std::memcpy(Buffer.data(), &Id, 4);
    return EvdevIoctl(Pad, C, EVIOCSCLOCKID, Buffer);
  };
  CHECK(SetClock(Monotonic, 1) == 0 && SetClock(Boottime, 7) == 0);
  CHECK(!Monotonic.Readable() && !Boottime.Readable());
  CHECK(Monotonic.Read(nullptr, 0, true) == Eagain);
  CHECK(SetClock(Realtime, 2) == Einval && SetClock(Realtime, 3) == Einval);
  std::vector<EvdevClient*> Clients {&Realtime, &Monotonic, &Boottime};
  const InputEvent Packet[] = {Ev(EV_ABS, ABS_X, 1), Syn()};
  Deliver(Pad, Clients, Packet, Time);
  const auto SecondsOf = [&](EvdevClient& C) {
    uint8_t Out[24 * 8];
    const auto Bytes = C.Read(Out, sizeof(Out), true);
    InputEvent Last;
    std::memcpy(&Last, Out + Bytes - 24, 24);
    return Last.Sec;
  };
  // Each client receives the same packet in its own clock domain.
  CHECK(SecondsOf(Realtime) == 5000);
  CHECK(SecondsOf(Monotonic) == 7000);
  CHECK(SecondsOf(Boottime) == 9000);

  Realtime.Push(Packet, Time);
  CHECK(SetClock(Realtime, 1) == 0 && !Realtime.Readable());
  Realtime.Push(Packet, Time);
  InputEvent Events[3];
  CHECK(Realtime.Read(reinterpret_cast<uint8_t*>(Events), sizeof(Events), true) == sizeof(Events));
  CHECK(Events[0].Type == EV_SYN && Events[0].Code == SYN_DROPPED && Events[0].Sec == 7000);
  CHECK(Events[2].Type == EV_SYN && Events[2].Code == SYN_REPORT);
}

static void TestIoctls() {
  auto Pad = MakeGamepad(1);
  EvdevClient Client(Pad);
  std::vector<uint8_t> Buffer;
  size_t Copy {};
  CHECK(Ioctl(Pad, Client, EVIOCGVERSION, Buffer, &Copy) == 0 && Copy == 4);
  uint32_t Version;
  std::memcpy(&Version, Buffer.data(), 4);
  CHECK(Version == 0x010001);
  CHECK(Ioctl(Pad, Client, EVIOCGID, Buffer) == 0);
  InputId Id;
  std::memcpy(&Id, Buffer.data(), 8);
  CHECK(Id.Vendor == 0x045e);
  // Strings: with the NUL, cut to the length.
  CHECK(Ioctl(Pad, Client, EVIOCGNAME(256), Buffer, &Copy) == 24 && Copy == 24 && std::string(reinterpret_cast<char*>(Buffer.data())) == Pad.Name);
  CHECK(Ioctl(Pad, Client, EVIOCGNAME(8), Buffer, &Copy) == 8 && Copy == 8);
  CHECK(Ioctl(Pad, Client, EVIOCGPHYS(64), Buffer) == 14);
  CHECK(Ioctl(Pad, Client, EVIOCGUNIQ(64), Buffer) == Enoent);
  CHECK(Ioctl(Pad, Client, EVIOCGPROP(8), Buffer) == 8);
  // Bitmaps: min(len, bytes in longs).
  CHECK(Ioctl(Pad, Client, EVIOCGBIT(0, 4), Buffer) == 4);
  CHECK(Ioctl(Pad, Client, EVIOCGBIT(0, 64), Buffer) == 8);
  CHECK(Ioctl(Pad, Client, EVIOCGBIT(EV_KEY, 200), Buffer) == 96);
  CHECK(Ioctl(Pad, Client, EVIOCGBIT(EV_KEY, 16), Buffer) == 16);
  CHECK(Ioctl(Pad, Client, EVIOCGBIT(EV_ABS, 8), Buffer) == 8);
  CHECK(Buffer[0] == 0x3f && Buffer[2] == 0x03); // X Y Z RX RY RZ, HAT0X HAT0Y
  CHECK(Ioctl(Pad, Client, EVIOCGBIT(EV_FF, 32), Buffer) == 16);
  CHECK(Ioctl(Pad, Client, EVIOCGBIT(EV_REP, 8), Buffer) == Einval);
  CHECK(Ioctl(Pad, Client, EVIOCGLED(8), Buffer) == 8 && Ioctl(Pad, Client, EVIOCGSND(8), Buffer) == 8 && Ioctl(Pad, Client, EVIOCGSW(8), Buffer) == 8);
  // Absinfo get and set (a smaller buffer is cut).
  CHECK(Ioctl(Pad, Client, EVIOCGABS(ABS_RX), Buffer) == 0);
  AbsInfo Info;
  std::memcpy(&Info, Buffer.data(), 24);
  CHECK(Info.Flat == 128);
  Info.Flat = 300;
  std::memcpy(Buffer.data(), &Info, 24);
  CHECK(EvdevIoctl(Pad, Client, EVIOCSABS(ABS_RX), Buffer) == 0 && Pad.AbsInfos[ABS_RX].Flat == 300);
  for (auto Device : {MakeKeyboard(), MakeMouse()}) {
    EvdevClient NoAbs(Device);
    size_t Copy = sizeof(AbsInfo);
    CHECK(Ioctl(Device, NoAbs, EVIOCGABS(ABS_RX), Buffer, &Copy) == Einval && Copy == 0);
    CHECK(Ioctl(Device, NoAbs, EVIOCSABS(ABS_RX), Buffer, &Copy) == Einval && Copy == 0);
    CHECK(Device.AbsInfos[ABS_RX].Flat == 0);
  }
  // Repeat: the gamepad has no EV_REP, the keyboard does.
  CHECK(Ioctl(Pad, Client, EVIOCGREP, Buffer) == Enosys);
  auto Keyboard = MakeKeyboard();
  EvdevClient KeyClient(Keyboard);
  CHECK(Ioctl(Keyboard, KeyClient, EVIOCGREP, Buffer) == 0);
  int32_t Rep[2];
  std::memcpy(Rep, Buffer.data(), 8);
  CHECK(Rep[0] == 250 && Rep[1] == 33);
  Rep[0] = 400;
  std::memcpy(Buffer.data(), Rep, 8);
  CHECK(EvdevIoctl(Keyboard, KeyClient, EVIOCSREP, Buffer) == 0 && Keyboard.Rep[0] == 400);
  // Unknown 'E' requests are EINVAL, other types ENOTTY.
  CHECK(Ioctl(Pad, Client, Ioc(IocRead, 0x10, 4), Buffer) == Einval);
  CHECK(EvdevIoctl(Pad, Client, 0x5401, Buffer) == Enotty);
  // A short argument buffer is refused rather than overrun.
  std::vector<uint8_t> Short(2);
  CHECK(EvdevIoctl(Pad, Client, EVIOCGVERSION, Short) == Einval);
}

static void TestEffects() {
  auto Pad = MakeGamepad(0);
  EvdevClient A(Pad), B(Pad);
  std::vector<uint8_t> Buffer;
  size_t Copy {};
  CHECK(Ioctl(Pad, A, EVIOCGEFFECTS, Buffer) == 0);
  int32_t Max;
  std::memcpy(&Max, Buffer.data(), 4);
  CHECK(Max == 16);
  const auto Upload = [&](EvdevClient& C, int16_t Id, uint16_t Type, uint16_t Strong, uint16_t Weak, uint16_t Length,
                          int16_t Magnitude = 0) {
    FfEffect Effect {};
    Effect.Type = Type;
    Effect.Id = Id;
    Effect.ReplayLength = Length;
    std::memcpy(Effect.Union, &Strong, 2);
    std::memcpy(Effect.Union + 2, &Weak, 2);
    std::memcpy(Effect.Union + 4, &Magnitude, 2);
    Buffer.resize(48);
    std::memcpy(Buffer.data(), &Effect, 48);
    return EvdevIoctl(Pad, C, EVIOCSFF, Buffer, &Copy);
  };
  const auto IdOf = [&] { FfEffect E; std::memcpy(&E, Buffer.data(), 48); return E.Id; };
  CHECK(Upload(A, -1, FF_RUMBLE, 0x8000, 0x4000, 250) == 0 && IdOf() == 0 && Copy == 48);
  CHECK(Upload(A, -1, FF_RUMBLE, 1, 1, 10) == 0 && IdOf() == 1);
  CHECK(Upload(A, -1, FF_PERIODIC, 1, 1, 10) == Einval);
  CHECK(Upload(A, -1, 0x99, 1, 1, 10) == Einval);
  CHECK(Upload(B, 0, FF_RUMBLE, 1, 1, 1) == Eacces);     // another client's effect
  CHECK(Upload(A, 0, FF_RUMBLE, 0x8000, 0x4000, 500) == 0 && IdOf() == 0); // replace
  CHECK(Upload(A, 99, FF_RUMBLE, 1, 1, 1) == Einval);
  CHECK(Upload(A, -2, FF_RUMBLE, 1, 1, 1) == Einval);
  CHECK(Upload(A, 2, FF_RUMBLE, 1, 1, 1) == Einval && Copy == 0);
  // Fill the table.
  for (int I = 2; I < 16; ++I) CHECK(Upload(A, -1, FF_RUMBLE, 1, 1, 1) == 0);
  CHECK(Upload(A, -1, FF_RUMBLE, 1, 1, 1) == Enospc);
  // Remove.
  Buffer.resize(4);
  int32_t Id = 1;
  std::memcpy(Buffer.data(), &Id, 4);
  CHECK(EvdevIoctl(Pad, B, EVIOCRMFF, Buffer) == Eacces); // not the owner
  CHECK(EvdevIoctl(Pad, A, EVIOCRMFF, Buffer) == 0);
  CHECK(EvdevIoctl(Pad, A, EVIOCRMFF, Buffer) == Einval);
  CHECK(Upload(A, -1, FF_RUMBLE, 7, 8, 9) == 0 && IdOf() == 1);
  // Writes: play, stop, gain.
  std::vector<RumbleRequest> Rumble;
  const auto SendRumble = [&](RumbleRequest Request) { Rumble.push_back(Request); };
  InputEvent Play = Ev(EV_FF, 0, 3);
  CHECK(Write(Pad, A, reinterpret_cast<uint8_t*>(&Play), 24, SendRumble) == 24);
  CHECK(Rumble.size() == 1 && Rumble[0].Strong == 0x8000 && Rumble[0].Weak == 0x4000 && Rumble[0].DurationMs == 500 && Rumble[0].PlayCount == 3 &&
        !Rumble[0].Stop);
  InputEvent Stop = Ev(EV_FF, 0, 0);
  CHECK(Write(Pad, A, reinterpret_cast<uint8_t*>(&Stop), 24, SendRumble) == 24 && Rumble.size() == 2 && Rumble[1].Stop);
  InputEvent Gain = Ev(EV_FF, FF_GAIN, 0x7fff);
  CHECK(Write(Pad, A, reinterpret_cast<uint8_t*>(&Gain), 24, SendRumble) == 24 && Pad.Gain == 0x7fff && Rumble.size() == 2);
  CHECK(Write(Pad, A, reinterpret_cast<uint8_t*>(&Play), 24, SendRumble) == 24 && Rumble[2].Strong == 0x8000 * 0x7fff / 0xffff);
  // Unknown effect ids and other events are accepted and ignored; another client's effect does not play.
  InputEvent Other[2] = {Ev(EV_FF, 20, 1), Ev(EV_KEY, BTN_SOUTH, 1)};
  CHECK(Write(Pad, A, reinterpret_cast<uint8_t*>(Other), 48, SendRumble) == 48 && Rumble.size() == 3);
  CHECK(Write(Pad, B, reinterpret_cast<uint8_t*>(&Play), 24, SendRumble) == 24 && Rumble.size() == 3);
  CHECK(Write(Pad, A, reinterpret_cast<uint8_t*>(&Play), 10, SendRumble) == Einval);
  for (uint16_t Wave : {FF_SINE, FF_TRIANGLE, FF_SQUARE}) {
    CHECK(TestBit(Pad.Ff, Wave));
    CHECK(Upload(A, -1, FF_PERIODIC, Wave, 50, 100, -0x7fff) == Enospc);
  }
  CHECK(Upload(A, 0, FF_PERIODIC, FF_SINE, 50, 100, 0x7fff) == Einval); // existing rumble slot
  Buffer.resize(4);
  Id = 1;
  std::memcpy(Buffer.data(), &Id, 4);
  CHECK(EvdevIoctl(Pad, A, EVIOCRMFF, Buffer) == 0);
  CHECK(Upload(A, -1, FF_PERIODIC, FF_SINE, 50, 100, -0x7fff) == 0 && IdOf() == 1);
  CHECK(Upload(A, 1, FF_PERIODIC, FF_SQUARE, 50, 100, -0x7fff) == Einval);
  InputEvent Periodic = Ev(EV_FF, 1, 2);
  CHECK(Write(Pad, A, reinterpret_cast<uint8_t*>(&Periodic), 24, SendRumble) == 24 && Rumble.size() == 4);
  auto Request = Rumble.back();
  const auto [Strong, Weak] = RumbleAt(Request, 0);
  CHECK(Request.DurationMs == 100 && Request.PlayCount == 2 && Strong == 0x7fff && Weak == Strong);
  Request.DelayMs = 10;
  Request.Periodic->AttackMs = Request.Periodic->FadeMs = 20;
  Request.Periodic->AttackLevel = Request.Periodic->FadeLevel = 0;
  CHECK(RumbleAt(Request, 9).first == 0 && RumbleAt(Request, 20).first == 0x3fff &&
        RumbleAt(Request, 30).first == 0x7fff && RumbleAt(Request, 100).first == 0x3fff);
  // Closing a client erases its effects and its grab.
  Buffer.resize(4);
  int32_t One = 1;
  std::memcpy(Buffer.data(), &One, 4);
  CHECK(EvdevIoctl(Pad, A, EVIOCGRAB, Buffer) == 0 && Pad.Grab == &A);
  A.Close(Pad);
  CHECK(!Pad.Grab);
  for (const auto& Slot : Pad.Effects) CHECK(!Slot);
  // Devices without force feedback.
  auto Keyboard = MakeKeyboard();
  EvdevClient KeyClient(Keyboard);
  CHECK(Ioctl(Keyboard, KeyClient, EVIOCGEFFECTS, Buffer) == 0);
  std::memcpy(&Max, Buffer.data(), 4);
  CHECK(Max == 0);
  FfEffect Rumble1 {};
  Rumble1.Type = FF_RUMBLE;
  Rumble1.Id = -1;
  Buffer.resize(48);
  std::memcpy(Buffer.data(), &Rumble1, 48);
  CHECK(EvdevIoctl(Keyboard, KeyClient, EVIOCSFF, Buffer) == Enosys);
}

static void TestGrabAndRevoke() {
  auto Pad = MakeGamepad(0);
  EvdevClient A(Pad), B(Pad);
  std::vector<EvdevClient*> Clients {&A, &B};
  std::vector<uint8_t> Buffer(4);
  int32_t One = 1, Zero = 0;
  std::memcpy(Buffer.data(), &One, 4);
  CHECK(EvdevIoctl(Pad, A, EVIOCGRAB, Buffer) == 0);
  CHECK(EvdevIoctl(Pad, B, EVIOCGRAB, Buffer) == Ebusy);
  std::memcpy(Buffer.data(), &Zero, 4);
  CHECK(EvdevIoctl(Pad, B, EVIOCGRAB, Buffer) == Einval); // not the grabber
  const InputEvent Packet[] = {Ev(EV_ABS, ABS_X, 4), Syn()};
  Deliver(Pad, Clients, Packet, Time);
  CHECK(A.Readable() && !B.Readable()); // the grabber only
  CHECK(EvdevIoctl(Pad, A, EVIOCGRAB, Buffer) == 0 && !Pad.Grab);
  const InputEvent Next[] = {Ev(EV_ABS, ABS_X, 5), Syn()};
  Deliver(Pad, Clients, Next, Time);
  CHECK(B.Readable());
  // Revoke: reads, polls and ioctls fail with ENODEV; events stop.
  CHECK(EvdevIoctl(Pad, B, EVIOCREVOKE, Buffer) == 0);
  uint8_t Out[48];
  CHECK(B.Read(Out, 24, true) == Enodev);
  CHECK((B.PollMask() & 0x18) == 0x18);
  CHECK(EvdevIoctl(Pad, B, EVIOCGVERSION, Buffer) == Enodev);
  std::vector<RumbleRequest> Rumble;
  const auto SendRumble = [&](RumbleRequest Request) { Rumble.push_back(Request); };
  InputEvent Gain = Ev(EV_FF, FF_GAIN, 1);
  CHECK(Write(Pad, B, reinterpret_cast<uint8_t*>(&Gain), 24, SendRumble) == Enodev);
  const InputEvent Later[] = {Ev(EV_ABS, ABS_X, 6), Syn()};
  Deliver(Pad, Clients, Later, Time);
  uint8_t Readback[8 * InputEventSize];
  CHECK(A.Read(Readback, sizeof(Readback), true) == 6 * InputEventSize);
  // REVOKE with a non-zero argument is invalid.
  std::memcpy(Buffer.data(), &One, 4);
  CHECK(EvdevIoctl(Pad, A, EVIOCREVOKE, Buffer) == Einval);
}

int main() {
  TestSeatAccessAcl();
  TestLayouts();
  TestHid();
  TestFactories();
  TestFramingAndState();
  TestOverflow();
  TestMasks();
  TestClocks();
  TestIoctls();
  TestEffects();
  TestGrabAndRevoke();
  std::puts("guest-evdev: ok");
  return 0;
}
