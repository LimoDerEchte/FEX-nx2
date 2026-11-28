// SPDX-License-Identifier: MIT
// The pure part of /sys: cpu lists and masks (drivers/base/cpu.c, drivers/base/topology.c, cpumask %*pb), the power
// supply text of drivers/power/supply/power_supply_sysfs.c, the cgroup2 root files and the fixed DMI strings. The
// runtime gathers the plain values (cpu count, battery percentage, charger, process ids) and puts these strings into
// its kernel tree.
#pragma once

#include "GuestInputFeed.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace Switch2Guest::Sysfs {

// A cpulist of "%*pbl": "0-5", "3", "0-1,4". Cpus is a sorted list of cpu numbers.
inline std::string FormatCpuList(const std::vector<uint32_t>& Cpus) {
  std::string Out;
  for (size_t I = 0; I < Cpus.size();) {
    size_t End = I;
    while (End + 1 < Cpus.size() && Cpus[End + 1] == Cpus[End] + 1) ++End;
    if (!Out.empty()) Out += ',';
    Out += std::to_string(Cpus[I]);
    if (End > I) Out += "-" + std::to_string(Cpus[End]);
    I = End + 1;
  }
  return Out + "\n";
}

// The cpus first..first+Count-1 as a list.
inline std::string FormatCpuRange(uint32_t First, uint32_t Count) {
  std::vector<uint32_t> Cpus;
  for (uint32_t I = 0; I < Count; ++I) Cpus.push_back(First + I);
  return FormatCpuList(Cpus);
}

// A cpumask of "%*pb" for a machine of TotalCpus cpus: 32-bit words in hex, highest first, separated by commas. The
// highest word has only as many digits as its bits need ("3f" for six cpus, "01" for cpu 0 of six).
inline std::string FormatCpuMask(const std::vector<uint32_t>& Cpus, uint32_t TotalCpus) {
  const uint32_t Words = (TotalCpus + 31) / 32;
  std::vector<uint32_t> Value(Words ? Words : 1, 0);
  for (const auto Cpu : Cpus) {
    if (Cpu < TotalCpus) Value[Cpu / 32] |= 1u << (Cpu % 32);
  }
  std::string Out;
  for (uint32_t Word = Words ? Words : 1; Word-- > 0;) {
    const uint32_t Bits = Word == Words - 1 && TotalCpus % 32 ? TotalCpus % 32 : 32;
    char Buffer[16];
    std::snprintf(Buffer, sizeof(Buffer), "%0*x", static_cast<int>((Bits + 3) / 4), Value[Word]);
    if (!Out.empty()) Out += ',';
    Out += Buffer;
  }
  return Out + "\n";
}

// "cpu_possible" style files: `online`, `possible`, `present` are the list 0..Count-1. Linux prints a single cpu as "0".
inline std::string CpuMaskAll(uint32_t Count) { return FormatCpuRange(0, Count); }
// kernel_max is the highest possible cpu number (NR_CPUS - 1); the runtime reports Count - 1 for the cpus it shows.
inline std::string KernelMax(uint32_t Count) { return std::to_string(Count ? Count - 1 : 0) + "\n"; }

// The files of cpuN/topology for cpu Cpu of Count cpus: one core per cpu, one package, no SMT (the console's
// cores are single threaded).
struct TopologyFile {
  std::string_view Name;
  std::string Text;
};
inline std::vector<TopologyFile> TopologyFiles(uint32_t Cpu, uint32_t Count) {
  const std::string Own = FormatCpuList({Cpu});
  const std::string Package = FormatCpuRange(0, Count);
  return {
      {"core_id", std::to_string(Cpu) + "\n"},
      {"physical_package_id", "0\n"},
      {"die_id", "0\n"},
      {"core_cpus", FormatCpuMask({Cpu}, Count)},
      {"core_cpus_list", Own},
      {"thread_siblings", FormatCpuMask({Cpu}, Count)},
      {"thread_siblings_list", Own},
      {"core_siblings", FormatCpuMask([&] {
         std::vector<uint32_t> All;
         for (uint32_t I = 0; I < Count; ++I) All.push_back(I);
         return All;
       }(), Count)},
      {"core_siblings_list", Package},
      {"package_cpus", FormatCpuMask([&] {
         std::vector<uint32_t> All;
         for (uint32_t I = 0; I < Count; ++I) All.push_back(I);
         return All;
       }(), Count)},
      {"package_cpus_list", Package},
  };
}

// cpufreq: fixed values in kHz, a deliberate difference (the console's clocks are not readable by a guest). They
// match the 1000 MHz /proc/cpuinfo shows.
inline constexpr uint32_t CpuFreqKHz = 1000000;
inline constexpr uint32_t CpuFreqMinKHz = 500000;
inline constexpr std::string_view CpuGovernor = "performance\n";

// ---- Power supplies (psm) -------------------------------------------------------------------------------------------

// What the console reports about its power.
struct PowerState {
  uint32_t Percent {};      // psmGetBatteryChargePercentage, 0..100
  bool ChargerPresent {};   // psmGetChargerType != unconnected
  bool ChargerEnough {};    // the charger supplies enough power (PsmChargerType_EnoughPower)
};

// power_supply_status_text: Charging, Discharging, Not charging or Full.
inline const char* BatteryStatus(const PowerState& State) {
  if (!State.ChargerPresent) return "Discharging";
  if (State.Percent >= 100) return "Full";
  return State.ChargerEnough ? "Charging" : "Not charging";
}

// power_supply_capacity_level_text. Linux drivers pick the level themselves; this is the common mapping.
inline const char* CapacityLevel(uint32_t Percent) {
  if (Percent >= 100) return "Full";
  if (Percent >= 80) return "High";
  if (Percent >= 16) return "Normal";
  if (Percent >= 6) return "Low";
  return "Critical";
}

inline std::string BatteryCapacity(const PowerState& State) { return std::to_string(std::min(State.Percent, 100u)) + "\n"; }

// The uevent file of the battery (power_supply_uevent): NAME, TYPE, STATUS, PRESENT, TECHNOLOGY, CAPACITY, LEVEL.
inline std::string BatteryUevent(const PowerState& State) {
  return std::string("POWER_SUPPLY_NAME=BAT0\nPOWER_SUPPLY_TYPE=Battery\nPOWER_SUPPLY_STATUS=") + BatteryStatus(State) +
         "\nPOWER_SUPPLY_PRESENT=1\nPOWER_SUPPLY_TECHNOLOGY=Li-ion\nPOWER_SUPPLY_CAPACITY=" + std::to_string(std::min(State.Percent, 100u)) +
         "\nPOWER_SUPPLY_CAPACITY_LEVEL=" + CapacityLevel(State.Percent) + "\n";
}
inline std::string AdapterOnline(const PowerState& State) { return State.ChargerPresent ? "1\n" : "0\n"; }
inline std::string AdapterUevent(const PowerState& State) {
  return std::string("POWER_SUPPLY_NAME=ADP1\nPOWER_SUPPLY_TYPE=Mains\nPOWER_SUPPLY_ONLINE=") + (State.ChargerPresent ? "1" : "0") + "\n";
}

// ---- cgroup2 root ---------------------------------------------------------------------------------------------------

// cgroup.procs and cgroup.threads: one id per line, ascending (cgroup_procs_show prints in the iterator order, which
// is ascending for a flat tree).
inline std::string FormatIdList(std::vector<uint32_t> Ids) {
  std::sort(Ids.begin(), Ids.end());
  std::string Out;
  for (const auto Id : Ids) Out += std::to_string(Id) + "\n";
  return Out;
}
inline constexpr std::string_view CgroupStat = "nr_descendants 0\nnr_dying_descendants 0\n";

// input_print_bitmap writes 64-bit unsigned longs on x86-64, omitting leading zero words.
inline std::string InputBitmap(const Evdev::Bits& Bits) {
  std::string Out;
  for (auto It = Bits.rbegin(); It != Bits.rend(); ++It) {
    if (!Out.empty()) Out += ' ';
    else if (!*It) continue;
    char Word[17];
    std::snprintf(Word, sizeof(Word), "%llx", static_cast<unsigned long long>(*It));
    Out += Word;
  }
  return Out.empty() ? "0" : Out;
}

inline std::string InputHex(uint16_t Value) {
  char Text[5];
  std::snprintf(Text, sizeof(Text), "%04x", static_cast<unsigned>(Value));
  return Text;
}

inline std::vector<TopologyFile> InputFiles(const Evdev::EvdevDevice& Device, InputClass Class) {
  using namespace Evdev;
  const auto& Id = Device.Id;
  const std::string Properties = InputBitmap(Device.Prop);
  std::vector<TopologyFile> Files {
    {"name", Device.Name + '\n'}, {"phys", Device.Phys + '\n'}, {"uniq", Device.Uniq + '\n'},
    {"properties", Properties + '\n'},
    {"id/bustype", InputHex(Id.Bustype) + '\n'}, {"id/vendor", InputHex(Id.Vendor) + '\n'},
    {"id/product", InputHex(Id.Product) + '\n'}, {"id/version", InputHex(Id.Version) + '\n'},
  };
  char Product[24], Alias[28];
  std::snprintf(Product, sizeof(Product), "%x/%x/%x/%x", unsigned(Id.Bustype), unsigned(Id.Vendor),
                unsigned(Id.Product), unsigned(Id.Version));
  std::snprintf(Alias, sizeof(Alias), "input:b%04Xv%04Xp%04Xe%04X-", unsigned(Id.Bustype), unsigned(Id.Vendor),
                unsigned(Id.Product), unsigned(Id.Version));
  std::string Modalias = Alias;
  std::string Uevent = std::string("PRODUCT=") + Product + '\n';
  const auto StringField = [&](std::string_view Key, const std::string& Value) {
    if (!Value.empty()) Uevent += std::string(Key) + "=\"" + Value + "\"\n";
  };
  StringField("NAME", Device.Name);
  StringField("PHYS", Device.Phys);
  // USB HID passes a string even when there is no serial number. xpad leaves uniq unset.
  if (!Device.Uniq.empty() || Class != InputClass::Pad) Uevent += "UNIQ=\"" + Device.Uniq + "\"\n";
  Uevent += "PROP=" + Properties + '\n';

  // input_dev_uevent includes only advertised types. input_print_modalias emits all groups and keys from 0x71.
  const struct {
    std::string_view Path, Field;
    char Group;
    const Bits& Bitmap;
    unsigned Type, First, End;
  } Capabilities[] {
    {"capabilities/ev", "EV", 'e', Device.Ev, EV_SYN, 0, EV_MAX},
    {"capabilities/key", "KEY", 'k', Device.Key, EV_KEY, 0x71, KEY_MAX},
    {"capabilities/rel", "REL", 'r', Device.Rel, EV_REL, 0, REL_MAX},
    {"capabilities/abs", "ABS", 'a', Device.Abs, EV_ABS, 0, ABS_MAX},
    {"capabilities/msc", "MSC", 'm', Device.Msc, EV_MSC, 0, MSC_MAX},
    {"capabilities/led", "LED", 'l', Device.Led, EV_LED, 0, LED_MAX},
    {"capabilities/snd", "SND", 's', Device.Snd, EV_SND, 0, SND_MAX},
    {"capabilities/ff", "FF", 'f', Device.Ff, EV_FF, 0, FF_MAX},
    {"capabilities/sw", "SW", 'w', Device.Sw, EV_SW, 0, SW_MAX},
  };
  for (const auto& Cap : Capabilities) {
    const auto Text = InputBitmap(Cap.Bitmap);
    Files.push_back({Cap.Path, Text + '\n'});
    if (Cap.Type == EV_SYN || TestBit(Device.Ev, Cap.Type))
      Uevent += std::string(Cap.Field) + '=' + Text + '\n';
    Modalias += Cap.Group;
    for (unsigned Code = Cap.First; Code < Cap.End; ++Code) {
      if (!TestBit(Cap.Bitmap, Code)) continue;
      char Hex[10];
      std::snprintf(Hex, sizeof(Hex), "%X,", Code);
      Modalias += Hex;
    }
  }
  Files.push_back({"modalias", Modalias + '\n'});
  Files.push_back({"uevent", Uevent + "MODALIAS=" + Modalias + '\n'});
  return Files;
}

inline std::string EventUevent(uint32_t Number) {
  return "MAJOR=13\nMINOR=" + std::to_string(64 + Number) + "\nDEVNAME=input/event" + std::to_string(Number) + '\n';
}

struct InputUdevData {
  std::string ById, ByPath, Database;
};

inline InputUdevData InputUdev(const Evdev::EvdevDevice& Device, InputClass Class, uint64_t Usec) {
  // 60-persistent-input.rules uses ID_BUS, ID_SERIAL, ID_PATH and the input class. Horizon supplies
  // a logical slot in phys instead of USB descriptors. Include the pad slot in ID_SERIAL to keep links unique.
  const auto Vendor = InputHex(Device.Id.Vendor);
  const auto Product = InputHex(Device.Id.Product);
  std::string Serial = Vendor + '_' + Product;
  if (Class == InputClass::Pad) Serial += '_' + Device.Phys.substr(Device.Phys.rfind('/') + 1);
  std::string Path = "platform-" + Device.Phys;
  std::replace(Path.begin(), Path.end(), '/', '-');
  const char* Suffix = Class == InputClass::Pad ? "-event-joystick" :
                       Class == InputClass::Keyboard ? "-event-kbd" : "-event-mouse";
  InputUdevData Data {"input/by-id/usb-" + Serial + Suffix, "input/by-path/" + Path + Suffix, {}};
  Data.Database = "S:" + Data.ById + "\nS:" + Data.ByPath + "\nI:" + std::to_string(Usec) + "\nE:ID_INPUT=1\n";
  switch (Class) {
  case InputClass::Pad: Data.Database += "E:ID_INPUT_JOYSTICK=1\n"; break;
  case InputClass::Keyboard: Data.Database += "E:ID_INPUT_KEY=1\nE:ID_INPUT_KEYBOARD=1\n"; break;
  case InputClass::Mouse: Data.Database += "E:ID_INPUT_MOUSE=1\n"; break;
  }
  Data.Database += "E:ID_BUS=usb\nE:ID_VENDOR_ID=" + Vendor + "\nE:ID_MODEL_ID=" + Product +
                   "\nE:ID_SERIAL=" + Serial + "\nE:ID_PATH=" + Path + '\n';
  // 70-uaccess and 71-seat tag joystick event nodes. The keyboard and mouse event nodes have neither tag.
  if (Class == InputClass::Pad) Data.Database += "G:seat\nG:uaccess\nQ:seat\nQ:uaccess\n";
  Data.Database += "V:1\n";
  return Data;
}

// ---- Fixed strings --------------------------------------------------------------------------------------------------

struct FixedFile {
  std::string_view Path;
  uint32_t Mode;
  std::string_view Text;
};

// DMI identity (a deliberate fixed value: the console has no firmware tables) and the transparent hugepage policy.
inline constexpr FixedFile FixedFiles[] = {
    {"/sys/devices/virtual/dmi/id/sys_vendor", 0444, "Nintendo\n"},
    {"/sys/devices/virtual/dmi/id/product_name", 0444, "Switch 2\n"},
    {"/sys/devices/virtual/dmi/id/board_vendor", 0444, "Nintendo\n"},
    {"/sys/devices/virtual/dmi/id/bios_vendor", 0444, "Nintendo\n"},
    {"/sys/devices/virtual/dmi/id/chassis_type", 0444, "30\n"}, // tablet
    {"/sys/devices/virtual/dmi/id/product_uuid", 0400, "4e494e54-454e-444f-5357-495443483200\n"},
    {"/sys/kernel/mm/transparent_hugepage/enabled", 0644, "always [madvise] never\n"},
    {"/sys/kernel/mm/transparent_hugepage/defrag", 0644, "always defer defer+madvise [madvise] never\n"},
    {"/sys/devices/system/node/online", 0444, "0\n"},
    {"/sys/fs/cgroup/cgroup.controllers", 0444, "\n"},
    {"/sys/fs/cgroup/cgroup.subtree_control", 0644, "\n"},
    {"/sys/fs/cgroup/cgroup.max.depth", 0644, "max\n"},
    {"/sys/fs/cgroup/cgroup.max.descendants", 0644, "max\n"},
};

} // namespace Switch2Guest::Sysfs
