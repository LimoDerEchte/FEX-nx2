// SPDX-License-Identifier: MIT
#include "SettingsSchema.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "Json.h"
#include "PathUtil.h"

namespace ui {

namespace {
SettingDef Enum(const char* Key, const char* Section, const char* Label, const char* Desc, std::vector<EnumOption> Options, const char* Default) {
  SettingDef D;
  D.Key = Key;
  D.Section = Section;
  D.Label = Label;
  D.Description = Desc;
  D.Type = RowType::Enum;
  D.Options = std::move(Options);
  D.Default = Default;
  return D;
}
SettingDef PathRow(const char* Key, const char* Section, const char* Label, const char* Desc, const char* Default, bool Folder, std::vector<std::string> Accept,
                   const char* Title) {
  SettingDef D;
  D.Key = Key;
  D.Section = Section;
  D.Label = Label;
  D.Description = Desc;
  D.Type = RowType::Path;
  D.Default = Default;
  D.Folder = Folder;
  D.Accept = std::move(Accept);
  D.PickerTitle = Title;
  return D;
}
std::vector<EnumOption> Opts(std::initializer_list<std::pair<const char*, const char*>> L) {
  std::vector<EnumOption> Out;
  for (const auto& P : L)
    Out.push_back({P.first, P.second});
  return Out;
}

std::vector<SettingDef> BuildSchema() {
  std::vector<SettingDef> S;

  // Paths: the two RootFS actions, then the file browser's start folder (the launcher's StartFolder setting).
  {
    SettingDef D;
    D.Key = "reinstall_rootfs";
    D.Section = "Paths";
    D.Label = "Reinstall RootFS";
    D.Description = "Choose a base system again; your home folder is kept";
    D.Type = RowType::Action;
    D.Store = StoreKind::None;
    D.ActionText = "Reinstall \xE2\x80\xBA";
    D.ActionId = "reinstall_rootfs";
    D.Honored = true;
    S.push_back(D);
    D.Key = "wipe_rootfs";
    D.Label = "Wipe RootFS";
    D.Description = "Deletes the RootFS, desktops, game stores and the home folder";
    D.ActionText = "Wipe";
    D.ActionId = "wipe_rootfs";
    D.Destructive = true;
    S.push_back(D);
  }
  {
    SettingDef D = PathRow("start_folder", "Paths", "Start folder", "The folder the file browser opens first (a path inside the RootFS)", "/fex/games", true, {}, "Choose the start folder");
    D.PickerStart = "rootfs";
    D.Honored = true;
    S.push_back(D);
  }

  // Appearance (theming spec): both are honored by the UI itself.
  {
    SettingDef D = Enum("theme", "Appearance", "Theme", "Match console needs a console theme API that is not available yet; Dark is used",
                        Opts({{"match", "Match console"}, {"dark", "Dark"}, {"light", "Light"}}), "match");
    D.Honored = true;
    S.push_back(D);
    D = Enum("accent", "Appearance", "Accent color", "Focus ring, toggles and progress", Opts({{"cyan", "Cyan"}, {"red", "Red"}, {"violet", "Violet"}, {"blue", "Blue"}}), "cyan");
    D.Honored = true;
    S.push_back(D);
  }

  // Logging: the launcher's log level is the one setting the runtime honors today.
  {
    SettingDef D = Enum("log_level", "Logging", "Log level", "How much the launcher and the guest runtime write to the SD log", Opts({{"standard", "Standard"}, {"detailed", "Detailed"}, {"trace", "Trace"}}), "standard");
    D.Honored = true;
    S.push_back(D);
  }

  // Developer: the launcher's old diagnostic and test-bundle entries. Each runs in a program tab of the Shell screen.
  {
    auto Dev = [&](const char* Key, const char* Label, const char* Desc, const char* Text) {
      SettingDef D;
      D.Key = Key;
      D.Section = "Developer";
      D.Label = Label;
      D.Description = Desc;
      D.Type = RowType::Action;
      D.Store = StoreKind::None;
      D.ActionText = Text;
      D.ActionId = Key;
      D.Honored = true;
      return D;
    };
    S.push_back(Dev("root_shell", "Root shell", "Opens a shell logged in as uid 0, for administration such as dpkg", "Open \xE2\x80\xBA"));
    SettingDef D = Dev("run_elf", "Run ELF file", "Runs an x86-64 Linux program; its .args, .user and .recovery-test files next to it apply", "Choose \xE2\x80\xBA");
    D.PickerTitle = "Choose an ELF file";
    D.PickerStart = "fex";
    S.push_back(D);
    D = Dev("run_elf_single", "Run ELF file, single-block JIT", "Same as above with the JIT limited to one block at a time", "Choose \xE2\x80\xBA");
    D.PickerTitle = "Choose an ELF file";
    D.PickerStart = "fex";
    S.push_back(D);
    D = Dev("final_pass", "Final Pass Suite", "Runs every suite a milestone's final pass needs and adds up the results", "Run");
    D.ShownWhenFile = "sdmc:/fex/games/final-pass-suite.elf";
    S.push_back(D);
  }
  return S;
}

} // namespace

const std::vector<SettingDef>& DefaultSchema() {
  static const std::vector<SettingDef> S = BuildSchema();
  return S;
}

std::vector<std::string> SectionNames(const std::vector<SettingDef>& Schema) {
  std::vector<std::string> Out;
  for (const SettingDef& D : Schema)
    if (std::find(Out.begin(), Out.end(), D.Section) == Out.end())
      Out.push_back(D.Section);
  return Out;
}

const SettingDef* SettingsModel::Find(const std::string& Key) const {
  for (const SettingDef& D : *Defs)
    if (D.Key == Key)
      return &D;
  return nullptr;
}

std::string SettingsModel::Get(const std::string& Key) const {
  auto It = Values.find(Key);
  if (It != Values.end())
    return It->second;
  const SettingDef* D = Find(Key);
  return D ? D->Default : std::string();
}

void SettingsModel::Set(const std::string& Key, const std::string& Value) {
  const SettingDef* D = Find(Key);
  if (!D)
    return;
  std::string V = Value;
  if (D->Type == RowType::Slider) {
    float F = float(std::atof(V.c_str()));
    F = std::clamp(F, D->Min, D->Max);
    if (D->Step > 0)
      F = D->Min + std::round((F - D->Min) / D->Step) * D->Step;
    V = std::to_string(int(std::lround(F)));
  }
  Values[Key] = V;
  Cleared.erase(Key);
  Modified = true;
}

int SettingsModel::EnumIndex(const SettingDef& D) const {
  const std::string V = Get(D.Key);
  for (size_t I = 0; I < D.Options.size(); ++I)
    if (D.Options[I].Id == V)
      return int(I);
  return 0;
}

bool SettingsModel::Enabled(const SettingDef& D) const {
  if (!D.Honored)
    return false;
  if (!D.EnabledWhenKey.empty() && Get(D.EnabledWhenKey) != D.EnabledWhenValue)
    return false;
  return true;
}

std::string SettingsModel::Reason(const SettingDef& D) const {
  if (D.Honored && !D.EnabledWhenKey.empty() && Get(D.EnabledWhenKey) != D.EnabledWhenValue) {
    const SettingDef* Dep = Find(D.EnabledWhenKey);
    if (Dep)
      for (const EnumOption& O : Dep->Options)
        if (O.Id == D.EnabledWhenValue)
          return "Choose \"" + O.Label + "\" for " + Dep->Label;
  }
  return D.DisabledReason;
}

void SettingsModel::RestoreDefaults() {
  for (const SettingDef& D : *Defs)
    if (D.Store != StoreKind::None)
      Cleared.insert(D.Key);
  Values.clear();
  Modified = true;
}

// ---- Persistence --------------------------------------------------------------------------------------------------------

namespace {
std::string JsonText(const Json& J) {
  if (J.IsBool())
    return J.AsBool() ? "true" : "false";
  return J.AsString();
}

bool ReadJson(Storage& S, const std::string& Path, Json& Out, bool& Exists, std::string& Error) {
  std::string Text;
  Exists = S.ReadFile(Path, Text);
  if (!Exists) {
    Out = Json::Object();
    return true;
  }
  if (!Json::Parse(Text, Out, Error) || !Out.IsObject()) {
    if (Error.empty())
      Error = "the top level is not an object";
    return false;
  }
  return true;
}
} // namespace

bool SettingsModel::Load(Storage& S, const SettingsPaths& P, std::string& Warning) {
  Values.clear();
  Cleared.clear();
  Modified = false;
  Warning.clear();
  bool Ok = true;

  Json Ui;
  bool UiExists = false;
  std::string Err;
  if (!ReadJson(S, P.UiJson, Ui, UiExists, Err)) {
    Warning = "ui.json is not valid JSON (" + Err + "); using the defaults";
    Ok = false;
    Ui = Json::Object();
    UiExists = false;
  }
  bool Migrated = false;
  if (!UiExists && Ok) {
    // One-time migration of the text launcher's settings file: "<log level 0-2>\n<start folder>\n".
    for (const std::string& L : P.Legacy) {
      std::string Text;
      if (!S.ReadFile(L, Text) || Text.empty())
        continue;
      static const char* const Levels[] = {"standard", "detailed", "trace"};
      const int Lv = Text[0] >= '0' && Text[0] <= '2' ? Text[0] - '0' : 0;
      Values["log_level"] = Levels[Lv];
      const size_t Nl = Text.find('\n');
      if (Nl != std::string::npos) {
        std::string Folder = Text.substr(Nl + 1, Text.find_first_of("\r\n", Nl + 1) - Nl - 1);
        // Same rule as the launcher: the old /switch/fex root is /fex now; only absolute guest paths count.
        if (Folder == "/switch/fex" || Folder.rfind("/switch/fex/", 0) == 0)
          Folder = Folder.substr(sizeof("/switch") - 1);
        if (!Folder.empty() && Folder[0] == '/')
          Values["start_folder"] = Folder;
      }
      Migrated = true;
      Modified = true;
      break;
    }
  }
  for (const SettingDef& D : *Defs) {
    if (D.Store != StoreKind::App)
      continue;
    const Json* J = Ui.Find(D.Key);
    if (!J)
      continue;
    std::string V = JsonText(*J);
    if (D.Type == RowType::Enum) {
      bool Known = false;
      for (const EnumOption& O : D.Options)
        Known = Known || O.Id == V;
      if (!Known)
        continue;
    } else if (D.Type == RowType::Toggle) {
      if (!J->IsBool())
        continue;
    } else if (D.Type == RowType::Slider) {
      if (!J->IsNumber())
        continue;
    }
    Values[D.Key] = V;
  }

  if (Migrated) {
    std::string E;
    if (!Save(S, P, E))
      Warning += std::string(Warning.empty() ? "" : "; ") + "could not write the migrated ui.json: " + E;
  }
  return Ok;
}

bool SettingsModel::Save(Storage& S, const SettingsPaths& P, std::string& Error) {
  Error.clear();
  if (!Modified)
    return true;
  // ui.json: re-read so unknown keys written by someone else survive. A corrupt file is kept as ui.json.bad.
  Json Ui;
  bool UiExists = false;
  std::string Err;
  if (!ReadJson(S, P.UiJson, Ui, UiExists, Err)) {
    std::string Bad;
    if (S.ReadFile(P.UiJson, Bad)) {
      std::string E2;
      S.WriteFileAtomic(P.UiJson + ".bad", Bad, E2);
    }
    Ui = Json::Object();
  }
  if (!Ui.Find("version"))
    Ui.Set("version", Json::Int(1));
  for (const SettingDef& D : *Defs) {
    if (D.Store == StoreKind::None)
      continue;
    const bool Clear = Cleared.count(D.Key) != 0;
    auto It = Values.find(D.Key);
    if (!Clear && It == Values.end())
      continue;
    if (D.Store == StoreKind::App) {
      if (Clear) {
        Ui.Erase(D.Key);
      } else if (D.Type == RowType::Toggle) {
        Ui.Set(D.Key, Json::Bool(It->second == "true"));
      } else if (D.Type == RowType::Slider) {
        Ui.Set(D.Key, Json::Int(std::atoll(It->second.c_str())));
      } else {
        Ui.Set(D.Key, Json::String(It->second));
      }
    }
  }
  std::string Text = Ui.Dump();
  if (!S.WriteFileAtomic(P.UiJson, Text, Err)) {
    Error = "ui.json: " + Err;
    return false;
  }
  Cleared.clear();
  Modified = false;
  return true;
}

ThemeMode ResolveThemeMode(const SettingsModel& M, ThemeMode ConsoleMode) {
  const std::string V = M.Get("theme");
  if (V == "dark")
    return ThemeMode::Dark;
  if (V == "light")
    return ThemeMode::Light;
  return ConsoleMode;
}

AccentPreset ResolveAccent(const SettingsModel& M) {
  AccentPreset A = AccentPreset::Cyan;
  ParseAccent(M.Get("accent").c_str(), A);
  return A;
}

} // namespace ui
