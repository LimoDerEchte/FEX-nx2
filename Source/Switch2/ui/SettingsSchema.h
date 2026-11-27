// SPDX-License-Identifier: MIT
// The settings schema (rows come from this table; adding a row needs no layout code) and its persistence: keys in
// sdmc:/fex/config/ui.json only (FEX runtime config is set in code, never read from a Config.json), atomic temp+rename writes,
// unknown keys preserved. The schema lists real project features only.
#pragma once

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Storage.h"
#include "theme.h"

namespace ui {

enum class RowType : uint8_t {
  Path,    // chip + Browse; A opens the file picker
  Enum,    // chip; Left/Right cycle, A opens the picker modal
  Toggle,
  Slider,
  Link,    // a row that opens another place (A)
  Action,  // a pill button; Destructive draws it in the error color
};
enum class StoreKind : uint8_t { App, None };

struct EnumOption {
  std::string Id;     // stored in ui.json for app keys
  std::string Label;
};

struct SettingDef {
  std::string Key;      // unique; for StoreKind::App also the ui.json key
  std::string Section;
  std::string Label;
  std::string Description;
  RowType Type = RowType::Toggle;
  StoreKind Store = StoreKind::App;
  std::string Default;  // enum option id, "true"/"false", a number, or a path
  std::vector<EnumOption> Options;  // Enum
  float Min = 0, Max = 0, Step = 1;  // Slider
  // Path.
  bool Folder = false;                 // choose a folder instead of a file
  std::vector<std::string> Accept;     // lower-case extensions with the dot (".sqsh"); empty = any file
  std::string PickerTitle;
  std::string PickerStart = "fex";     // start location when the value is unusable: "sd", "fex", "rootfs" or "home"
  // Action / Link.
  std::string ActionText;
  std::string ActionId;                // "reinstall_rootfs", "wipe_rootfs", "open_about", the developer actions (Backend.h DevAction)
  bool Destructive = false;
  // Availability.
  bool Honored = false;                // the runtime really uses it today
  std::string DisabledReason = "Not available yet";
  std::string EnabledWhenKey, EnabledWhenValue;  // enabled only while that setting has that value
};

const std::vector<SettingDef>& DefaultSchema();
std::vector<std::string> SectionNames(const std::vector<SettingDef>& Schema);  // in schema order, once each

struct SettingsPaths {
  std::string UiJson = "sdmc:/fex/config/ui.json";
  // The text launcher's file ("<level 0-2>\n<start folder>\n"); read once to migrate when ui.json does not exist. The old
  // /switch/fex location is legacy input only (the launcher renames it to /fex at start).
  std::vector<std::string> Legacy = {"sdmc:/fex/fex-switch2.cfg", "sdmc:/switch/fex/fex-switch2.cfg"};
};

class SettingsModel {
public:
  explicit SettingsModel(const std::vector<SettingDef>& Schema = DefaultSchema()) : Defs(&Schema) {}
  const std::vector<SettingDef>& Schema() const { return *Defs; }
  const SettingDef* Find(const std::string& Key) const;

  // Current value as stored text (enum id, "true"/"false", number, path). The default when never set.
  std::string Get(const std::string& Key) const;
  bool IsSet(const std::string& Key) const { return Values.count(Key) != 0; }
  void Set(const std::string& Key, const std::string& Value);
  bool GetBool(const std::string& Key) const { return Get(Key) == "true"; }
  int GetInt(const std::string& Key) const { return std::atoi(Get(Key).c_str()); }
  // Enum: index of the current option, 0 when the stored id is not an option.
  int EnumIndex(const SettingDef& D) const;
  // The row is usable: honored and its dependency holds. Reason is the text for a disabled row.
  bool Enabled(const SettingDef& D) const;
  std::string Reason(const SettingDef& D) const;

  // Back to the defaults. The next Save also removes the keys this schema owns from ui.json.
  void RestoreDefaults();
  bool Changed() const { return Modified; }

  // Reads ui.json (migrating the legacy file when it is absent). A missing file is fine (defaults). A file that is not valid
  // JSON leaves the defaults and sets Warning. Returns false only for that case.
  bool Load(Storage& S, const SettingsPaths& P, std::string& Warning);
  // Writes the settings: re-reads the files, changes only keys of this schema, keeps the rest, atomic writes. false and Error when a write failed.
  bool Save(Storage& S, const SettingsPaths& P, std::string& Error);

private:
  const std::vector<SettingDef>* Defs;
  std::map<std::string, std::string> Values;
  std::set<std::string> Cleared;  // schema keys to remove from the files on the next Save
  bool Modified = false;
};

// Theme setting -> mode: "match" follows the console (ConsoleMode), "dark" and "light" are explicit.
ThemeMode ResolveThemeMode(const SettingsModel& M, ThemeMode ConsoleMode);
AccentPreset ResolveAccent(const SettingsModel& M);

} // namespace ui
