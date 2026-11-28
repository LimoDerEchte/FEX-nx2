// SPDX-License-Identifier: MIT
#include "GuestSysfs.h"

#include <cstdio>
#include <cstdlib>
#include <set>

#define CHECK(Cond) do { if (!(Cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #Cond); std::exit(1); } } while (0)

using namespace Switch2Guest::Sysfs;

static std::string InputAttr(const std::vector<TopologyFile>& Files, std::string_view Name) {
  for (const auto& File : Files) {
    if (File.Name == Name) return File.Text;
  }
  CHECK(false);
  return {};
}

static void CheckInputFiles() {
  using namespace Switch2Guest;
  CHECK(InputBitmap({}) == "0");
  CHECK(InputBitmap({0, 0, 0}) == "0");
  CHECK(InputBitmap({0x10, 0, 0x8000000000000000ULL, 0}) == "8000000000000000 0 10");
  CHECK(EventUevent(2) == "MAJOR=13\nMINOR=66\nDEVNAME=input/event2\n");

  // A wired xpad from Debian bug #922465. The guest models fewer force-feedback effects.
  auto Xpad = Evdev::MakeGamepad(0);
  const auto Pad = InputFiles(Xpad, InputClass::Pad);
  CHECK(InputAttr(Pad, "phys") == "switch2/npad0\n");
  CHECK(InputAttr(Pad, "id/bustype") == "0003\n");
  CHECK(InputAttr(Pad, "id/vendor") == "045e\n");
  CHECK(InputAttr(Pad, "id/product") == "028e\n");
  CHECK(InputAttr(Pad, "id/version") == "0114\n");
  CHECK(InputAttr(Pad, "capabilities/ff") == "107030000 0\n");
  CHECK(InputAttr(Pad, "modalias") ==
        "input:b0003v045Ep028Ee0114-e0,1,3,15,k130,131,133,134,136,137,13A,13B,13C,13D,13E,"
        "ra0,1,2,3,4,5,10,11,mlsf50,51,58,59,5A,60,w\n");
  Xpad.Ff = {0, 0x107030000ULL};
  Xpad.Phys = "usb-0000:00:1a.0-1/input0";
  const auto RealPad = InputFiles(Xpad, InputClass::Pad);
  const std::string XpadAlias =
    "input:b0003v045Ep028Ee0114-e0,1,3,15,k130,131,133,134,136,137,13A,13B,13C,13D,13E,"
    "ra0,1,2,3,4,5,10,11,mlsf50,51,58,59,5A,60,w\n";
  CHECK(InputAttr(RealPad, "modalias") == XpadAlias);
  CHECK(InputAttr(RealPad, "uevent") ==
        "PRODUCT=3/45e/28e/114\nNAME=\"Microsoft X-Box 360 pad\"\nPHYS=\"usb-0000:00:1a.0-1/input0\"\n"
        "PROP=0\nEV=20000b\nKEY=7cdb000000000000 0 0 0 0\nABS=3003f\nFF=107030000 0\nMODALIAS=" + XpadAlias);
  CHECK(InputAttr(RealPad, "capabilities/sw") == "0\n");
  CHECK(InputAttr(RealPad, "uniq") == "\n");

  // Keychron USB keyboard, copied from Linux sysfs. Its key bitmap spans ten 64-bit words.
  Evdev::EvdevDevice Keyboard;
  Keyboard.Id = {3, 0x3434, 0xd031, 0x111};
  Keyboard.Name = "Keychron  Keychron Link  Keyboard";
  Keyboard.Phys = "usb-0000:0f:00.3-1/input2";
  Keyboard.Ev = {0x12001fULL};
  Keyboard.Key = {0xfffffffffffffffeULL, 0xffbeffdfffefffffULL, 0xffff7bfad941dfffULL, 0x130ff38b17d007ULL, 0x1ULL,
                  0xbfd4444600000000ULL, 0x483ffff17aff32dULL, 0x0ULL, 0x0ULL, 0x3f00733fffULL};
  Keyboard.Rel = {0x1040ULL};
  Keyboard.Abs = {0x100000000ULL};
  Keyboard.Msc = {0x10ULL};
  Keyboard.Led = {0x1fULL};
  Keyboard.Snd = {0x0ULL};
  Keyboard.Ff = {0x0ULL};
  Keyboard.Sw = {0x0ULL};
  const auto Keys = InputFiles(Keyboard, InputClass::Keyboard);
  const std::string KeyboardAlias =
        "input:b0003v3434pD031e0111-e0,1,2,3,4,11,14,k71,72,73,74,75,77,78,79,7A,7B,7C,7D,7E,7F,80,81"
        ",82,83,84,85,86,87,88,89,8A,8B,8C,8E,8F,90,96,98,9B,9C,9E,9F,A1,A3,A4,A5,A6,A7,A8,A9,AB,AC,A"
        "D,AE,B0,B1,B2,B3,B4,B5,B6,B7,B8,B9,BA,BB,BC,BD,BE,BF,C0,C1,C2,CC,CE,CF,D0,D1,D2,D4,D8,D9,DB,"
        "DF,E0,E1,E4,E5,E6,E7,E8,E9,EA,EB,F0,F1,F4,100,161,162,166,16A,16E,172,174,176,177,178,179,17"
        "A,17B,17C,17D,17F,180,182,183,185,188,189,18C,18D,18E,18F,190,191,192,193,195,197,198,199,19"
        "A,19C,1A0,1A1,1A2,1A3,1A4,1A5,1A6,1A7,1A8,1A9,1AA,1AB,1AC,1AD,1AE,1AF,1B0,1B1,1B7,1BA,240,24"
        "1,242,243,244,245,246,247,248,249,24A,24B,24C,24D,250,251,254,255,256,260,261,262,263,264,26"
        "5,r6,C,a20,m4,l0,1,2,3,4,sfw\n";
  CHECK(InputAttr(Keys, "modalias") == KeyboardAlias);
  CHECK(InputAttr(Keys, "uevent") ==
        "PRODUCT=3/3434/d031/111\n"
        "NAME=\"Keychron  Keychron Link  Keyboard\"\n"
        "PHYS=\"usb-0000:0f:00.3-1/input2\"\n"
        "UNIQ=\"\"\n"
        "PROP=0\n"
        "EV=12001f\n"
        "KEY=3f00733fff 0 0 483ffff17aff32d bfd4444600000000 1 130ff38b17d007 ffff7bfad941dfff ffbeff"
        "dfffefffff fffffffffffffffe\n"
        "REL=1040\n"
        "ABS=100000000\n"
        "MSC=10\n"
        "LED=1f\n"
        "MODALIAS=" + KeyboardAlias);
  CHECK(InputAttr(Keys, "capabilities/led") == "1f\n");
  CHECK(InputAttr(Keys, "properties") == "0\n");

  const auto UdevPad = InputUdev(Evdev::MakeGamepad(0), InputClass::Pad, 12345);
  CHECK(UdevPad.ById == "input/by-id/usb-045e_028e_npad0-event-joystick");
  CHECK(UdevPad.ByPath == "input/by-path/platform-switch2-npad0-event-joystick");
  CHECK(UdevPad.ById != InputUdev(Evdev::MakeGamepad(1), InputClass::Pad, 12345).ById);
  CHECK(UdevPad.Database ==
        "S:input/by-id/usb-045e_028e_npad0-event-joystick\n"
        "S:input/by-path/platform-switch2-npad0-event-joystick\nI:12345\n"
        "E:ID_INPUT=1\nE:ID_INPUT_JOYSTICK=1\nE:ID_BUS=usb\nE:ID_VENDOR_ID=045e\nE:ID_MODEL_ID=028e\n"
        "E:ID_SERIAL=045e_028e_npad0\nE:ID_PATH=platform-switch2-npad0\nG:seat\nG:uaccess\nQ:seat\nQ:uaccess\nV:1\n");
  const auto UdevKeyboard = InputUdev(Evdev::MakeKeyboard(), InputClass::Keyboard, 23);
  CHECK(UdevKeyboard.Database ==
        "S:input/by-id/usb-057e_3001-event-kbd\nS:input/by-path/platform-switch2-keyboard0-event-kbd\nI:23\n"
        "E:ID_INPUT=1\nE:ID_INPUT_KEY=1\nE:ID_INPUT_KEYBOARD=1\nE:ID_BUS=usb\n"
        "E:ID_VENDOR_ID=057e\nE:ID_MODEL_ID=3001\nE:ID_SERIAL=057e_3001\nE:ID_PATH=platform-switch2-keyboard0\nV:1\n");
  const auto UdevMouse = InputUdev(Evdev::MakeMouse(), InputClass::Mouse, 45);
  CHECK(UdevMouse.Database ==
        "S:input/by-id/usb-057e_3002-event-mouse\nS:input/by-path/platform-switch2-mouse0-event-mouse\nI:45\n"
        "E:ID_INPUT=1\nE:ID_INPUT_MOUSE=1\nE:ID_BUS=usb\nE:ID_VENDOR_ID=057e\nE:ID_MODEL_ID=3002\n"
        "E:ID_SERIAL=057e_3002\nE:ID_PATH=platform-switch2-mouse0\nV:1\n");
}

int main() {
  CheckInputFiles();

  // cpulist (%*pbl)
  CHECK(FormatCpuList({}) == "\n");
  CHECK(FormatCpuList({0}) == "0\n");
  CHECK(FormatCpuList({0, 1}) == "0-1\n");
  CHECK(FormatCpuList({0, 1, 2, 3, 4, 5}) == "0-5\n");
  CHECK(FormatCpuList({0, 1, 4}) == "0-1,4\n");
  CHECK(FormatCpuList({1, 3, 5}) == "1,3,5\n");
  CHECK(FormatCpuRange(1, 5) == "1-5\n");
  CHECK(CpuMaskAll(6) == "0-5\n");
  CHECK(KernelMax(6) == "5\n");

  // cpumask (%*pb): six cpus fit in one word with two digits; 40 cpus use two words.
  CHECK(FormatCpuMask({0}, 6) == "01\n");
  CHECK(FormatCpuMask({5}, 6) == "20\n");
  CHECK(FormatCpuMask({0, 1, 2, 3, 4, 5}, 6) == "3f\n");
  CHECK(FormatCpuMask({0}, 32) == "00000001\n");
  CHECK(FormatCpuMask({0, 33}, 40) == "02,00000001\n");
  CHECK(FormatCpuMask({7}, 8) == "80\n");

  // Topology of cpu 2 of 6.
  const auto Topology = TopologyFiles(2, 6);
  const auto Get = [&](std::string_view Name) -> std::string {
    for (const auto& File : Topology) {
      if (File.Name == Name) return File.Text;
    }
    std::fprintf(stderr, "missing %s\n", std::string(Name).c_str());
    std::exit(1);
  };
  CHECK(Get("core_id") == "2\n");
  CHECK(Get("physical_package_id") == "0\n");
  CHECK(Get("die_id") == "0\n");
  CHECK(Get("core_cpus_list") == "2\n");
  CHECK(Get("thread_siblings_list") == "2\n");
  CHECK(Get("core_siblings_list") == "0-5\n");
  CHECK(Get("package_cpus_list") == "0-5\n");
  CHECK(Get("thread_siblings") == "04\n");
  CHECK(Get("core_cpus") == "04\n");
  CHECK(Get("core_siblings") == "3f\n");
  CHECK(Get("package_cpus") == "3f\n");

  // Power supply text.
  CHECK(std::string(BatteryStatus({50, false, false})) == "Discharging");
  CHECK(std::string(BatteryStatus({50, true, true})) == "Charging");
  CHECK(std::string(BatteryStatus({50, true, false})) == "Not charging");
  CHECK(std::string(BatteryStatus({100, true, true})) == "Full");
  CHECK(std::string(BatteryStatus({100, false, false})) == "Discharging");
  CHECK(std::string(CapacityLevel(100)) == "Full");
  CHECK(std::string(CapacityLevel(85)) == "High");
  CHECK(std::string(CapacityLevel(50)) == "Normal");
  CHECK(std::string(CapacityLevel(10)) == "Low");
  CHECK(std::string(CapacityLevel(3)) == "Critical");
  CHECK(BatteryCapacity({77, false, false}) == "77\n");
  CHECK(BatteryCapacity({250, false, false}) == "100\n");
  CHECK(BatteryUevent({42, true, true}) ==
        "POWER_SUPPLY_NAME=BAT0\nPOWER_SUPPLY_TYPE=Battery\nPOWER_SUPPLY_STATUS=Charging\nPOWER_SUPPLY_PRESENT=1\n"
        "POWER_SUPPLY_TECHNOLOGY=Li-ion\nPOWER_SUPPLY_CAPACITY=42\nPOWER_SUPPLY_CAPACITY_LEVEL=Normal\n");
  CHECK(AdapterOnline({42, true, true}) == "1\n");
  CHECK(AdapterOnline({42, false, false}) == "0\n");
  CHECK(AdapterUevent({1, false, false}) == "POWER_SUPPLY_NAME=ADP1\nPOWER_SUPPLY_TYPE=Mains\nPOWER_SUPPLY_ONLINE=0\n");

  // cgroup2
  CHECK(FormatIdList({7, 3, 5}) == "3\n5\n7\n");
  CHECK(FormatIdList({}).empty());
  CHECK(CgroupStat == "nr_descendants 0\nnr_dying_descendants 0\n");

  // Fixed files: unique, newline terminated, below /sys.
  std::set<std::string_view> Seen;
  for (const auto& Entry : FixedFiles) {
    CHECK(Entry.Path.starts_with("/sys/"));
    CHECK(Seen.insert(Entry.Path).second);
    CHECK(Entry.Text.back() == '\n');
  }
  const auto Find = [&](std::string_view Path) -> const FixedFile& {
    for (const auto& Entry : FixedFiles) {
      if (Entry.Path == Path) return Entry;
    }
    std::fprintf(stderr, "missing %s\n", std::string(Path).c_str());
    std::exit(1);
  };
  CHECK(Find("/sys/devices/virtual/dmi/id/chassis_type").Text == "30\n");
  CHECK(Find("/sys/devices/virtual/dmi/id/product_uuid").Mode == 0400);
  CHECK(Find("/sys/kernel/mm/transparent_hugepage/enabled").Text == "always [madvise] never\n");
  CHECK(Find("/sys/fs/cgroup/cgroup.max.depth").Text == "max\n");
  CHECK(Find("/sys/fs/cgroup/cgroup.controllers").Text == "\n");

  std::puts("guest-sysfs: ok");
  return 0;
}
