// SPDX-License-Identifier: MIT
#include "GuestInputFeed.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using namespace Switch2Guest;
using namespace Switch2Guest::Evdev;

#define CHECK(Cond) do { if (!(Cond)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #Cond); std::exit(1); } } while (0)

void Expect(const std::vector<InputEvent>& Actual, std::initializer_list<InputEvent> Expected) {
  CHECK(Actual.size() == Expected.size());
  CHECK(std::equal(Actual.begin(), Actual.end(), Expected.begin(), [](const auto& A, const auto& B) {
    return A.Sec == B.Sec && A.Usec == B.Usec && A.Type == B.Type && A.Code == B.Code && A.Value == B.Value;
  }));
}

void TestPad() {
  const PadReport Neutral {.Connected = true};
  const PadReport Held {
    PadA | PadB | PadX | PadY | PadZL | PadZR | PadUp | PadRight, -32768, 32768, 32767, -32768, 1, true,
  };
  const auto Packet = ToEvdevPacket(PadChange {0, Neutral, Held}, GamepadIdentity::Xbox360);
  Expect(Packet, {
    {0, 0, EV_KEY, BTN_EAST, 1}, {0, 0, EV_KEY, BTN_SOUTH, 1},
    {0, 0, EV_KEY, BTN_NORTH, 1}, {0, 0, EV_KEY, BTN_WEST, 1},
    {0, 0, EV_ABS, ABS_Z, 255}, {0, 0, EV_ABS, ABS_RZ, 255},
    {0, 0, EV_ABS, ABS_X, -32768}, {0, 0, EV_ABS, ABS_Y, -32768},
    {0, 0, EV_ABS, ABS_RX, 32767}, {0, 0, EV_ABS, ABS_RY, 32767},
    {0, 0, EV_ABS, ABS_HAT0X, 1}, {0, 0, EV_ABS, ABS_HAT0Y, -1},
    {0, 0, EV_SYN, SYN_REPORT, 0},
  });
  const auto Pro = ToEvdevPacket(PadChange {0, Neutral, Held}, GamepadIdentity::SwitchPro);
  Expect(Pro, {
    {0, 0, EV_KEY, BTN_EAST, 1}, {0, 0, EV_KEY, BTN_SOUTH, 1},
    {0, 0, EV_KEY, BTN_NORTH, 1}, {0, 0, EV_KEY, BTN_WEST, 1},
    {0, 0, EV_KEY, BTN_TL2, 1}, {0, 0, EV_KEY, BTN_TR2, 1},
    {0, 0, EV_ABS, ABS_X, -32767}, {0, 0, EV_ABS, ABS_Y, -32767},
    {0, 0, EV_ABS, ABS_RX, 32767}, {0, 0, EV_ABS, ABS_RY, 32767},
    {0, 0, EV_ABS, ABS_HAT0X, 1}, {0, 0, EV_ABS, ABS_HAT0Y, -1},
    {0, 0, EV_SYN, SYN_REPORT, 0},
  });
  const PadReport Opposite {.Buttons = PadLeft | PadDown, .Connected = true};
  Expect(ToEvdevPacket(PadChange {0, Neutral, Opposite}, GamepadIdentity::Xbox360), {
    {0, 0, EV_ABS, ABS_HAT0X, -1}, {0, 0, EV_ABS, ABS_HAT0Y, 1}, {0, 0, EV_SYN, SYN_REPORT, 0},
  });
  Expect(ToEvdevPacket(PadChange {0, Opposite, {}}, GamepadIdentity::Xbox360), {
    {0, 0, EV_ABS, ABS_HAT0X, 0}, {0, 0, EV_ABS, ABS_HAT0Y, 0}, {0, 0, EV_SYN, SYN_REPORT, 0},
  });
  for (const auto Identity : {GamepadIdentity::Xbox360, GamepadIdentity::SwitchPro}) {
    const auto Device = MakeGamepad(0, Identity);
    for (const auto& Event : Identity == GamepadIdentity::Xbox360 ? Packet : Pro) {
      if (Event.Type == EV_ABS) {
        CHECK(TestBit(Device.Abs, Event.Code));
        CHECK(Event.Value >= Device.AbsInfos[Event.Code].Minimum && Event.Value <= Device.AbsInfos[Event.Code].Maximum);
      } else if (Event.Type == EV_KEY) {
        CHECK(TestBit(Device.Key, Event.Code));
      }
    }
  }
}

void TestKeyboardAndMouse() {
  KeyboardReport Before {.Modifiers = 2, .Usages = {}}, After {};
  Before.Usages.set(0x04);
  After.Usages.set(0x28);
  After.Usages.set(0x01); // HID rollover has no Linux key code.
  Expect(ToEvdevPacket(KeyboardChange {Before, After}), {
    {0, 0, EV_MSC, MSC_SCAN, 0x70004}, {0, 0, EV_KEY, 30, 0},
    {0, 0, EV_MSC, MSC_SCAN, 0x70028}, {0, 0, EV_KEY, 28, 1},
    {0, 0, EV_MSC, MSC_SCAN, 0x700e1}, {0, 0, EV_KEY, 42, 0},
    {0, 0, EV_SYN, SYN_REPORT, 0},
  });
  Expect(ToEvdevPacket(KeyboardChange {{}, KeyboardReport {.Modifiers = 2, .Usages = {}}}), {
    {0, 0, EV_MSC, MSC_SCAN, 0x700e1}, {0, 0, EV_KEY, 42, 1}, {0, 0, EV_SYN, SYN_REPORT, 0},
  });
  Expect(ToEvdevPacket(MouseChange {1, {-4, 5, 2, -3, 6}}), {
    {0, 0, EV_KEY, BTN_LEFT, 0}, {0, 0, EV_KEY, BTN_RIGHT, 1}, {0, 0, EV_KEY, BTN_MIDDLE, 1},
    {0, 0, EV_REL, REL_X, -4}, {0, 0, EV_REL, REL_Y, 5},
    {0, 0, EV_REL, REL_WHEEL, 2}, {0, 0, EV_REL, REL_HWHEEL, -3},
    {0, 0, EV_SYN, SYN_REPORT, 0},
  });
  Expect(ToEvdevPacket(MouseChange {2, {.Buttons = 2}}), {});
}

void TestFeed() {
  GuestInputFeed Feed;
  const PadReport Connected {.Style = 1, .Connected = true};
  Feed.PublishPad(7, Connected, 10);
  auto Batch = Feed.Take();
  CHECK(Batch.Changes.empty() && !Batch.Dropped && Batch.Snapshot);
  CHECK(Batch.Snapshot->TimeNs == 10 && Batch.Snapshot->Pads[7] == Connected);
  const auto First = Batch.Snapshot->Connections[7];
  CHECK(First.Generation != 0 && First.Identity == GamepadIdentity::Xbox360);
  KeyboardReport Keyboard {};
  Keyboard.Usages.set(4);
  Feed.PublishPad(7, Connected, 11);
  Feed.PublishPad(8, Connected, 11);
  Feed.PublishKeyboard(Keyboard, 12);
  Feed.PublishMouse({.X = 3, .Buttons = 1}, 13);
  Feed.PublishPad(7, {}, 14);
  Feed.PublishKeyboard({}, 15);
  Feed.PublishMouse({.X = 3, .Buttons = 1}, 16);
  Batch = Feed.Take();
  CHECK(Batch.Changes.size() == 5 && !Batch.Snapshot && !Batch.Dropped);
  const auto& Changes = Batch.Changes;
  for (size_t I = 0; I < Changes.size(); ++I)
    CHECK(Changes[I].TimeNs == 12 + I);
  CHECK(std::get<KeyboardChange>(Changes[0].Change).After == Keyboard);
  CHECK(std::get<MouseChange>(Changes[1].Change).After.X == 3);
  CHECK(std::get<PadChange>(Changes[2].Change).Before == Connected);
  CHECK(!std::get<PadChange>(Changes[2].Change).After.Connected);
  CHECK(std::get<KeyboardChange>(Changes[3].Change).Before == Keyboard);
  CHECK(std::get<MouseChange>(Changes[4].Change).BeforeButtons == 1);
  CHECK(Feed.Take().Changes.empty());
  Feed.StopTaking();
  Feed.SetGamepadIdentity(GamepadIdentity::SwitchPro);
  Feed.PublishMouse({.X = 42, .Buttons = 2}, 17);
  Feed.PublishPad(7, Connected, 18);
  Batch = Feed.Take();
  CHECK(Batch.Changes.empty() && Batch.Snapshot);
  CHECK(Batch.Snapshot->TimeNs == 18 && Batch.Snapshot->MouseButtons == 2);
  const auto Second = Batch.Snapshot->Connections[7];
  CHECK(Second.Generation > First.Generation && Second.Identity == GamepadIdentity::SwitchPro);
  Feed.PushRumble(7, {.Strong = 123, .PlayCount = 1}, Second.Generation);
  Feed.PushRumble(7, {.EffectId = 1, .Stop = true}, Second.Generation);
  Feed.PushRumble(7, {.Strong = 456, .EffectId = 1}, First.Generation);
  Feed.PushRumble(8, {.Stop = true}, Second.Generation);
  const auto Rumble = Feed.TakeRumble();
  CHECK(Rumble.size() == 2 && Rumble[0].Index == 7 && Rumble[0].Request.Strong == 123 && Rumble[1].Request.Stop);
  Feed.PushRumble(7, {.Strong = 123}, Second.Generation);
  Feed.PushRumble(7, {.Stop = true}, Second.Generation);
  const auto Latest = Feed.TakeRumble();
  CHECK(Latest.size() == 1 && Latest[0].Request.Stop);
  CHECK(Feed.TakeRumble().empty());
}

void TestReconnect() {
  GuestInputFeed Feed;
  const PadReport Held {.Buttons = PadB, .LX = 123, .Style = 1, .Connected = true};
  const auto First = Feed.PublishPad(0, Held, 10);
  const auto Second = Feed.PublishPad(7, Held, 20);
  CHECK(Feed.Take().Snapshot);
  Feed.PushRumble(0, {.Strong = 123}, First);
  Feed.SetGamepadIdentity(GamepadIdentity::SwitchPro);
  const auto Snapshot = Feed.Snapshot();
  const auto Batch = Feed.Take();
  CHECK(Batch.Changes.size() == 2 && !Batch.Snapshot && !Batch.Dropped);
  for (size_t I = 0; I < Batch.Changes.size(); ++I) {
    const auto& Change = std::get<PadChange>(Batch.Changes[I].Change);
    CHECK(Change.Index == (I == 0 ? 0u : 7u) && Batch.Changes[I].TimeNs == 20);
    CHECK(Change.Before == Held && Change.After == Held);
    CHECK(Change.PreviousConnection.Generation == (I == 0 ? First : Second));
    CHECK(Change.PreviousConnection.Identity == GamepadIdentity::Xbox360);
    CHECK(Change.Connection.Identity == GamepadIdentity::SwitchPro && Change.Connection.Generation > Second);
    CHECK(Snapshot.Connections[Change.Index].Generation == Change.Connection.Generation);
  }
  CHECK(!Snapshot.Pads[1].Connected && Snapshot.Connections[1].Generation == 0);
  Feed.PushRumble(0, {.Strong = 456}, First);
  CHECK(Feed.TakeRumble().empty());
  Feed.SetGamepadIdentity(GamepadIdentity::SwitchPro);
  CHECK(Feed.PublishPad(0, Held, 30) == Snapshot.Connections[0].Generation);
  CHECK(Feed.Take().Changes.empty());

  auto OtherStyle = Held;
  OtherStyle.Style = 2;
  const auto NewGeneration = Feed.PublishPad(0, OtherStyle, 40);
  const auto Style = Feed.Take();
  CHECK(Style.Changes.size() == 1 && NewGeneration > Snapshot.Connections[0].Generation);
  const auto& Change = std::get<PadChange>(Style.Changes[0].Change);
  CHECK(Change.Before == Held && Change.After == OtherStyle && Change.Connection.Generation == NewGeneration);
  CHECK(Change.PreviousConnection.Generation == Snapshot.Connections[0].Generation);
  CHECK(Change.Connection.Identity == GamepadIdentity::SwitchPro);

  Feed.StopTaking();
  Feed.SetGamepadIdentity(GamepadIdentity::Xbox360);
  const auto Idle = Feed.Take();
  CHECK(Idle.Changes.empty() && Idle.Snapshot && Idle.Snapshot->Connections[0].Generation > NewGeneration);
  CHECK(Idle.Snapshot->Connections[0].Identity == GamepadIdentity::Xbox360 && Idle.Snapshot->Pads[0] == OtherStyle);
}

void TestOverflow() {
  GuestInputFeed Feed;
  CHECK(Feed.Take().Snapshot);
  Feed.PublishPad(7, {.Style = 1, .Connected = true}, 1);
  for (size_t I = 0; I <= GuestInputFeed::ChangeLimit; ++I)
    Feed.PublishMouse({.X = static_cast<int32_t>(I + 1), .Buttons = 1}, I + 2);
  const auto Batch = Feed.Take();
  CHECK(Batch.Dropped && Batch.Snapshot && Batch.Snapshot->Pads[7].Connected && Batch.Snapshot->MouseButtons == 1);
  CHECK(Batch.Changes.size() == GuestInputFeed::ChangeLimit);
  CHECK(Batch.Changes.front().TimeNs == 3);
  CHECK(std::get<MouseChange>(Batch.Changes.front().Change).After.X == 2);
  CHECK(std::get<MouseChange>(Batch.Changes.back().Change).After.X == GuestInputFeed::ChangeLimit + 1);
  CHECK(!Feed.Take().Dropped);

  // Lost disconnects appear in the snapshot before a retained connection can claim the free event number.
  Feed.PublishPad(7, {}, 500);
  Feed.PublishMouse({.X = 1, .Buttons = 0}, 501);
  Feed.SetGamepadIdentity(GamepadIdentity::SwitchPro);
  Feed.PublishPad(6, {.Style = 1, .Connected = true}, 502);
  for (size_t I = 1; I < GuestInputFeed::ChangeLimit; ++I)
    Feed.PublishMouse({.X = 1, .Buttons = 2}, I + 502);
  const auto Reconnect = Feed.Take();
  CHECK(Reconnect.Dropped && Reconnect.Snapshot && !Reconnect.Snapshot->Pads[7].Connected);
  CHECK(!Reconnect.Snapshot->Pads[6].Connected && Reconnect.Snapshot->MouseButtons == 0);
  const auto& NewPad = std::get<PadChange>(Reconnect.Changes.front().Change);
  CHECK(NewPad.Index == 6 && NewPad.After.Connected && NewPad.Connection.Identity == GamepadIdentity::SwitchPro);

  auto Mouse = MakeMouse();
  EvdevClient Client(Mouse);
  Client.Masks[0] = MakeBits(EV_MAX); // Loss notification and packet framing bypass user masks.
  Client.Drop({2'000'003'000, 1, 1});
  std::array<InputEvent, 2> Events {};
  CHECK(Client.Read(reinterpret_cast<uint8_t*>(Events.data()), sizeof(Events), true) == sizeof(Events));
  Expect({Events.begin(), Events.end()}, {{2, 3, EV_SYN, SYN_DROPPED, 0}, {2, 3, EV_SYN, SYN_REPORT, 0}});
  const auto Generation = Client.Generation();
  CHECK(Client.PollMask() == (0x4 | 0x100));
  Client.Disconnect();
  CHECK(Client.PollMask() == (0x10 | 0x8) && Client.Generation() == Generation + 1);
  CHECK(Client.Read(nullptr, 0, true) == Enodev);
}

void TestOwnership() {
  const auto Hold = AdvanceOwnershipHold({}, true, true, 0);
  CHECK(Hold.SinceNs == 0);
  CHECK(!AdvanceOwnershipHold(Hold, true, true, 999'999'999).Released);
  CHECK(AdvanceOwnershipHold(Hold, true, true, 1'000'000'000).Released);
  CHECK(!AdvanceOwnershipHold(Hold, false, true, 1'000'000'000).SinceNs);
  CHECK(!AdvanceOwnershipHold(Hold, true, false, 1'000'000'000).SinceNs);

  GuestInputFeed Feed;
  const PadReport Combo {.Buttons = PadPlus | PadMinus, .Connected = true};
  const auto Generation = Feed.ReleasedGeneration();
  Feed.SetOwned(InputClass::Pad, true, Generation);
  Feed.SetOwned(InputClass::Keyboard, true, Generation);
  Feed.SetOwned(InputClass::Mouse, true, Generation);
  Feed.PublishPad(0, Combo, 0);
  Feed.PublishPad(0, {}, 1);
  Feed.PublishPad(1, Combo, 2);
  Feed.PublishPad(1, Combo, 1'000'000'001);
  CHECK(Feed.IsOwned(InputClass::Pad) && Feed.IsOwned(InputClass::Keyboard) && Feed.IsOwned(InputClass::Mouse));
  Feed.PublishPad(1, Combo, 1'000'000'002);
  CHECK(!Feed.IsOwned(InputClass::Pad) && !Feed.IsOwned(InputClass::Keyboard) && !Feed.IsOwned(InputClass::Mouse));
  CHECK(Feed.ReleasedGeneration() == Generation + 1);
  Feed.SetOwned(InputClass::Pad, true, Generation);
  CHECK(!Feed.IsOwned(InputClass::Pad));
  Feed.SetOwned(InputClass::Pad, true, Feed.ReleasedGeneration());
  Feed.PublishPad(1, Combo, 2'000'000'002);
  CHECK(Feed.IsOwned(InputClass::Pad) && Feed.ReleasedGeneration() == Generation + 1);
  Feed.PublishPad(1, {}, 2'000'000'003);
  Feed.PublishPad(1, Combo, 2'000'000'004);
  Feed.PublishPad(1, Combo, 3'000'000'004);
  CHECK(!Feed.IsOwned(InputClass::Pad) && Feed.ReleasedGeneration() == Generation + 2);
}

int main() {
  TestPad();
  TestKeyboardAndMouse();
  TestFeed();
  TestReconnect();
  TestOverflow();
  TestOwnership();
  std::puts("guest input feed: OK");
}
