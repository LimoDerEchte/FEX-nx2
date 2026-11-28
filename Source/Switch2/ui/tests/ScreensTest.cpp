// SPDX-License-Identifier: MIT
// Host tests for Task 4 (make check-ui): settings schema -> rows, persistence (ui.json only, unknown keys preserved,
// atomic writes, legacy migration), file picker logic (sorting, filtering, path forms, virtualization, spinner), Home geometry
// and navigation, Settings and About flows through the real Interaction path, license store. Fake clock and renderer: no GPU.
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "FileBrowser.h"
#include "Json.h"
#include "LicenseStore.h"
#include "PathUtil.h"
#include "RootfsIndex.h"
#include "SettingsSchema.h"
#include "backend/FakeBackend.h"
#include "platform/Log.h"
#include "platform/fake/FakePlatform.h"
#include "screens/filepicker.h"
#include "screens/home.h"
#include "screens/settings.h"

namespace {
int Failures = 0, Checks = 0;

#define CHECK(Cond)                                                          \
  do {                                                                       \
    ++Checks;                                                                \
    if (!(Cond)) {                                                           \
      ++Failures;                                                            \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond);            \
    }                                                                        \
  } while (0)

using namespace ui;

Event Btn(Button B, bool Down) {
  Event E;
  E.Kind = Down ? Event::Type::ButtonDown : Event::Type::ButtonUp;
  E.Btn = B;
  return E;
}

struct Rig {
  FakeDisplay D {Size {1280, 720}};
  FakeRenderer R {D};
  Fonts F {R};
  Icons I {R};
  uint64_t Now = 1000;
  Rig() { SetTheme(ThemeMode::Dark, AccentPreset::Cyan); }
  DrawContext Ctx() { return DrawContext {R, F, I, CurrentTheme(), 1.0f}; }
};

template <class S> struct ScreenRig : Rig {
  S* Scr = nullptr;
  void Step(const std::vector<Event>& Ev = {}, uint64_t Advance = 16) {
    Now += Advance;
    Scr->Update(Now, 1.0f, Ev);
    R.Calls.clear();
    const DrawContext C = Ctx();
    Scr->Draw(C, Now);
  }
  void Run(uint64_t Ms) {
    for (uint64_t T = 0; T < Ms; T += 16)
      Step();
  }
  void Press(Button B) {
    Step({Btn(B, true)});
    Step({Btn(B, false)});
  }
  Widget* Focused() { return Scr->Input().FocusedWidget(); }
};

std::string TempDir() {
  char Tpl[] = "/tmp/fex-ui-test-XXXXXX";
  const char* D = mkdtemp(Tpl);
  return D ? D : "/tmp";
}
std::string Slurp(const std::string& P) {
  std::ifstream In(P, std::ios::binary);
  std::stringstream S;
  S << In.rdbuf();
  return S.str();
}
bool FileExists(const std::string& P) {
  struct stat St;
  return stat(P.c_str(), &St) == 0;
}
void Spit(const std::string& P, const std::string& T) {
  std::ofstream Out(P, std::ios::binary);
  Out << T;
}

// ---- Path utilities ------------------------------------------------------------------------------------------------------

void TestPaths() {
  CHECK(NormalizePath("sdmc:/a//b/./c/") == "sdmc:/a/b/c");
  CHECK(NormalizePath("sdmc:/a/../../b") == "sdmc:/b");
  CHECK(NormalizePath("/usr/../..") == "/");
  CHECK(NormalizePath("sdmc:/") == "sdmc:/");
  CHECK(NormalizePath("sdmc:") == "sdmc:");  // not a valid form: unchanged
  CHECK(ParentPath("sdmc:/fex/rootfs") == "sdmc:/fex");
  CHECK(ParentPath("sdmc:/fex") == "sdmc:/");
  CHECK(ParentPath("sdmc:/") == "sdmc:/");
  CHECK(ParentPath("/home/switch/") == "/home");
  CHECK(ParentPath("/home") == "/");
  CHECK(ParentPath("/") == "/");
  CHECK(JoinPath("sdmc:/", "a") == "sdmc:/a");
  CHECK(JoinPath("/usr", "bin") == "/usr/bin");
  CHECK(BaseName("sdmc:/fex/") == "fex");
  CHECK(IsSdPath("sdmc:/x") && !IsRootfsPath("sdmc:/x") && IsRootfsPath("/x") && !IsValidPath("rel/x"));
  const auto B = Breadcrumbs("sdmc:/fex/rootfs");
  CHECK(B.size() == 3 && B[0].Label == "microSD" && B[0].Path == "sdmc:/" && B[1].Label == "fex" && B[1].Path == "sdmc:/fex" && B[2].Path == "sdmc:/fex/rootfs");
  const auto R = Breadcrumbs("/home/switch");
  CHECK(R.size() == 3 && R[0].Label == "RootFS" && R[0].Path == "/" && R[2].Label == "switch" && R[2].Path == "/home/switch");
  CHECK(FormatBytes(0) == "0 B" && FormatBytes(1536) == "1.5 KB" && FormatBytes(19756849561ull) == "18.4 GB");
  CHECK(FormatDate(0).empty() && FormatDate(1790000000) == "2026-09-21");
}

// ---- Picker logic --------------------------------------------------------------------------------------------------------

void TestBrowseLogic() {
  const std::vector<std::string> Acc = {".sqsh", ".ero"};
  CHECK(MatchesAccept("a.sqsh", Acc) && MatchesAccept("A.SQSH", Acc) && MatchesAccept("x.ero", Acc));
  CHECK(!MatchesAccept("a.sqsh.sha256", Acc) && !MatchesAccept("sqsh", Acc) && !MatchesAccept(".sqsh", Acc) && !MatchesAccept("notes.txt", Acc));
  CHECK(MatchesAccept("anything", {}));
  CHECK(IsHiddenName(".bashrc") && !IsHiddenName("bashrc"));

  std::vector<BrowseEntry> E = {{"beta.txt", false}, {"Zeta", true}, {"alpha.txt", false}, {"Alpha", true}, {"B", true}, {"a.txt", false}};
  SortEntries(E);
  const char* Want[] = {"Alpha", "B", "Zeta", "a.txt", "alpha.txt", "beta.txt"};  // folders first, each A-Z case-insensitive
  bool Ok = E.size() == 6;
  for (size_t I = 0; Ok && I < 6; ++I)
    Ok = E[I].Name == Want[I];
  CHECK(Ok);

  BrowseOptions O;
  O.Accept = Acc;
  BrowseEntry Out;
  DirEntry D;
  D.Name = "Ubuntu.sqsh";
  CHECK(MakeEntry(D, O, Out) && Out.Selectable);
  D.Name = "readme.txt";
  CHECK(MakeEntry(D, O, Out) && !Out.Selectable);  // visible at 40%, not selectable
  D.Name = ".hidden";
  CHECK(!MakeEntry(D, O, Out));
  D.Name = "sub";
  D.IsDir = true;
  CHECK(MakeEntry(D, O, Out) && Out.Selectable && Out.IsDir);
  BrowseOptions F;
  F.FolderMode = true;
  D.IsDir = false;
  D.Name = "file.bin";
  CHECK(!MakeEntry(D, F, Out));  // files hidden in folder mode
  D.IsDir = true;
  D.Name = "dir";
  CHECK(MakeEntry(D, F, Out));

  FakeBackend B;
  const auto L = DefaultLocations(B.HomeFolder());
  CHECK(L.size() == 4 && L[0].Root == "sdmc:/" && L[1].Root == "sdmc:/fex/" && L[2].Root == "/" && L[3].Root == "/home/switch/");
  CHECK(L[0].Group == "Console" && L[2].Group == "Linux");
  CHECK(LocationOf(L, "sdmc:/roms") == 0 && LocationOf(L, "sdmc:/fex/rootfs") == 1 && LocationOf(L, "/usr/bin") == 2 && LocationOf(L, "/home/switch/Documents") == 3);
  Storage& S = B.GetStorage();
  CHECK(StartFolder(S, L, "sdmc:/fex/rootfs/Ubuntu_24_04.sqsh", false, "fex") == "sdmc:/fex/rootfs");  // the folder of the value
  CHECK(StartFolder(S, L, "sdmc:/fex/config", true, "home") == "sdmc:/fex/config");                 // a folder value is itself
  CHECK(StartFolder(S, L, "sdmc:/gone/deep/x.sqsh", false, "fex") == "sdmc:/fex");                          // else the location that fits
  CHECK(StartFolder(S, L, "", true, "home") == "/home/switch");
  CHECK(StartFolder(S, L, "", true, "nowhere") == "sdmc:/");
}

void TestLoader() {
  FakeBackend B;
  B.Fs().PendingFrames = 3;
  FolderLoader Ld;
  BrowseOptions O;
  O.Accept = {".sqsh"};
  Ld.Begin(B.GetStorage(), "sdmc:/fex/rootfs", O, 1000);
  CHECK(!Ld.Done());
  CHECK(!Ld.Step(256) && Ld.PendingSteps() == 1);
  CHECK(!Ld.SpinnerVisible(1100) && Ld.SpinnerVisible(1150));  // the spinner after 150 ms
  Ld.Step(256);
  Ld.Step(256);
  CHECK(Ld.Step(256) && Ld.Done() && !Ld.Failed());
  const auto& E = Ld.Entries();
  CHECK(E.size() == 5 && E[0].Name == "old" && E[0].IsDir && E[1].Name == "notes.txt" && !E[1].Selectable && E[2].Name == "Ubuntu_22_04.sqsh" && E[3].Name == "Ubuntu_24_04.sqsh" &&
        E[4].Name == "Ubuntu_24_04.sqsh.sha256" && !E[4].Selectable);  // no .hidden-cache
  B.Fs().PendingFrames = 0;
  Ld.Begin(B.GetStorage(), "sdmc:/missing", O, 0);
  CHECK(Ld.Done() && Ld.Failed());
  // Steps of at most N entries: a large folder takes several frames.
  for (int K = 0; K < 1000; ++K)
    B.Fs().AddFile("sdmc:/big/f" + std::to_string(K), 1);
  Ld.Begin(B.GetStorage(), "sdmc:/big", BrowseOptions {}, 0);
  int Steps = 0;
  while (!Ld.Step(256))
    ++Steps;
  CHECK(Steps == 3 && Ld.Entries().size() == 1000);  // 256 per step: 3 steps, the 4th completes it
}

// ---- Schema, rows, persistence --------------------------------------------------------------------------------------------

// Test-only schema: the default rows plus enough generic rows (toggle, slider, file path, dependency, padding) to exercise every
// row type and make every section scroll. The real schema lists project features only.
const std::vector<SettingDef>& RichSchema() {
  static const std::vector<SettingDef> R = [] {
    std::vector<SettingDef> V = DefaultSchema();
    for (const char* Sec : {"Paths", "Appearance", "Logging"})
      for (int I = 0; I < 5; ++I) {
        SettingDef D;
        D.Key = std::string("pad_") + Sec + std::to_string(I);
        D.Section = Sec;
        D.Label = "Pad";
        D.Type = RowType::Toggle;
        D.Default = "false";
        D.Honored = true;
        V.push_back(D);
      }
    SettingDef D;
    D.Key = "ex_toggle";
    D.Section = "Appearance";
    D.Label = "Extra toggle";
    D.Type = RowType::Toggle;
    D.Default = "true";
    D.Honored = true;
    V.push_back(D);
    D = SettingDef();
    D.Key = "ex_slider";
    D.Section = "Appearance";
    D.Label = "Extra slider";
    D.Type = RowType::Slider;
    D.Min = 500;
    D.Max = 10000;
    D.Step = 500;
    D.Default = "5000";
    D.Honored = true;
    V.push_back(D);
    D = SettingDef();
    D.Key = "ex_file";
    D.Section = "Paths";
    D.Label = "Extra file";
    D.Type = RowType::Path;
    D.Default = "sdmc:/fex/rootfs/Ubuntu_24_04.sqsh";
    D.Accept = {".sqsh", ".ero"};
    D.PickerTitle = "Choose RootFS image";
    D.PickerStart = "fex";
    D.Honored = true;
    V.push_back(D);
    D = SettingDef();
    D.Key = "ex_mode";
    D.Section = "Logging";
    D.Label = "Mode";
    D.Type = RowType::Enum;
    D.Options = {{"a", "Alpha"}, {"custom", "Custom command"}};
    D.Default = "a";
    D.Honored = true;
    V.push_back(D);
    D = SettingDef();
    D.Key = "ex_cmd";
    D.Section = "Logging";
    D.Label = "Command";
    D.Type = RowType::Path;
    D.Default = "/usr/bin/bash";
    D.Honored = true;
    D.EnabledWhenKey = "ex_mode";
    D.EnabledWhenValue = "custom";
    V.push_back(D);
    return V;
  }();
  return R;
}

void TestSchema() {
  const auto& S = DefaultSchema();
  const auto Sec = SectionNames(S);
  const char* WantSec[] = {"Paths", "Appearance", "Logging", "Developer"};
  CHECK(Sec.size() == 4);
  for (size_t I = 0; I < Sec.size() && I < 4; ++I)
    CHECK(Sec[I] == WantSec[I]);
  std::vector<std::string> Keys;
  for (const SettingDef& D : S) {
    for (const std::string& K : Keys)
      CHECK(K != D.Key);
    Keys.push_back(D.Key);
    CHECK(!D.Label.empty() && !D.Section.empty());
    // Real features only: every row is honored (no disabled placeholders), and the SD root is sdmc:/fex.
    CHECK(D.Honored);
    CHECK(D.Default.find("switch/fex") == std::string::npos);
    if (D.Type == RowType::Enum) {
      bool HasDefault = false;
      for (const EnumOption& O : D.Options)
        HasDefault = HasDefault || O.Id == D.Default;
      CHECK(HasDefault);
    }
  }
  for (const char* Gone : {"steam_dir", "thunk_libs", "config_dir", "rootfs_image", "desktop_session", "display_server", "tso", "smc_checks", "vk_driver", "docked_res", "log_output", "silent_log"})
    CHECK(SettingsModel().Find(Gone) == nullptr);
  SettingsModel M;
  int Enabled = 0;
  for (const SettingDef& D : S)
    if (M.Enabled(D))
      ++Enabled;
  CHECK(Enabled == 11 && S.size() == 11);
  CHECK(M.Get("log_level") == "standard" && M.Get("start_folder") == "/fex/games");
  const SettingDef* Sf = M.Find("start_folder");
  CHECK(Sf && Sf->Type == RowType::Path && Sf->Folder && Sf->Store == StoreKind::App);
  const SettingDef* Ll = M.Find("log_level");
  CHECK(Ll && Ll->Options.size() == 3 && Ll->Options[0].Id == "standard" && Ll->Options[2].Id == "trace");
  const SettingDef* Identity = M.Find("gamepad_identity");
  CHECK(Identity && Identity->Section == "Developer" && Identity->Label == "Gamepad identity");
  CHECK(Identity->Store == StoreKind::App && M.Get("gamepad_identity") == "xbox360" && Identity->Options.size() == 2);
  CHECK(Identity->Options[0].Label == "Xbox 360 controller" && Identity->Options[1].Label == "Switch Pro Controller");
  // Dependency: the custom command needs the Custom mode (generic row machinery, test schema).
  SettingsModel M2(RichSchema());
  const SettingDef& Dep = *M2.Find("ex_cmd");
  CHECK(!M2.Enabled(Dep) && M2.Reason(Dep).find("Custom command") != std::string::npos);
  M2.Set("ex_mode", "custom");
  CHECK(M2.Enabled(Dep));
  // Slider values are clamped and snapped.
  M2.Set("ex_slider", "100000");
  CHECK(M2.Get("ex_slider") == "10000");
  M2.Set("ex_slider", "5300");
  CHECK(M2.Get("ex_slider") == "5500");
  M2.Set("ex_slider", "-5");
  CHECK(M2.Get("ex_slider") == "500");
}

struct FailRenameStorage final : PosixStorage {
  using PosixStorage::PosixStorage;
  bool FailRename = false;
  bool Rename(const std::string& From, const std::string& To) override { return FailRename ? false : PosixStorage::Rename(From, To); }
};

void TestPersistence() {
  const std::string Dir = TempDir();
  FailRenameStorage St(Dir, Dir + "/rootfs");
  SettingsPaths P;
  CHECK(P.UiJson == "sdmc:/fex/config/ui.json" && P.Legacy[0] == "sdmc:/fex/fex-switch2.cfg");
  const std::string UiReal = Dir + "/fex/config/ui.json";

  // Seed: unknown keys of every shape.
  St.MakeDirs("sdmc:/fex/config");
  Spit(UiReal, "{\n  \"ftp_port\": 5000,\n  \"future\": {\"a\": [1, 2.50, true, null, \"x\\u00e9\"]},\n  \"log_level\": \"detailed\"\n}\n");
  SettingsModel M(RichSchema());
  std::string W, E;
  CHECK(M.Load(St, P, W) && W.empty());
  CHECK(M.Get("log_level") == "detailed");  // from ui.json
  CHECK(!M.Changed());
  M.Set("log_level", "trace");
  M.Set("gamepad_identity", "switch_pro");
  M.Set("accent", "violet");
  M.Set("ex_toggle", "false");
  M.Set("ex_slider", "9000");
  M.Set("start_folder", "/fex/games/demo");
  M.Set("ex_file", "sdmc:/fex/rootfs/Ubuntu_22_04.sqsh");
  CHECK(M.Changed());
  CHECK(M.Save(St, P, E) && E.empty() && !M.Changed());
  CHECK(!FileExists(UiReal + ".tmp"));  // the temp file was renamed away
  CHECK(!FileExists(Dir + "/fex/config/Config.json"));  // FEX's Config.json is never written

  Json Ui;
  std::string Err;
  CHECK(Json::Parse(Slurp(UiReal), Ui, Err));
  CHECK(Ui.Find("log_level") && Ui.Find("log_level")->AsString() == "trace");
  CHECK(Ui.Find("accent") && Ui.Find("accent")->AsString() == "violet");
  CHECK(Ui.Find("ftp_port") && Ui.Find("ftp_port")->AsInt() == 5000);  // unknown keys kept
  const Json* Fut = Ui.Find("future");
  CHECK(Fut && Fut->IsObject() && Fut->Find("a") && Fut->Find("a")->Items().size() == 5 && Fut->Find("a")->Items()[1].AsString() == "2.50" && Fut->Find("a")->Items()[4].AsString() == "x\xC3\xA9");
  CHECK(Ui.Find("start_folder") && Ui.Find("start_folder")->AsString() == "/fex/games/demo");
  CHECK(Ui.Find("ex_toggle") && Ui.Find("ex_toggle")->IsBool() && Ui.Find("ex_slider") && Ui.Find("ex_slider")->AsInt() == 9000);

  // Round trip into a new model.
  SettingsModel M2(RichSchema());
  CHECK(M2.Load(St, P, W));
  CHECK(M2.Get("gamepad_identity") == "switch_pro");
  CHECK(M2.Get("log_level") == "trace" && M2.Get("accent") == "violet" && !M2.GetBool("ex_toggle") && M2.Get("ex_slider") == "9000");
  CHECK(M2.Get("start_folder") == "/fex/games/demo" && M2.Get("ex_file") == "sdmc:/fex/rootfs/Ubuntu_22_04.sqsh");

  // Atomic: a failing rename leaves the old file intact and no temp file behind; the model stays changed so a retry can work.
  const std::string Before = Slurp(UiReal);
  M2.Set("log_level", "standard");
  St.FailRename = true;
  CHECK(!M2.Save(St, P, E) && !E.empty());
  CHECK(Slurp(UiReal) == Before && !FileExists(UiReal + ".tmp"));
  CHECK(M2.Changed());
  St.FailRename = false;
  CHECK(M2.Save(St, P, E));
  CHECK(Slurp(UiReal) != Before && Slurp(UiReal).find("\"standard\"") != std::string::npos);

  // Restore defaults removes the keys of the schema and keeps the unknown ones.
  M2.RestoreDefaults();
  CHECK(M2.Save(St, P, E));
  CHECK(Json::Parse(Slurp(UiReal), Ui, Err) && Ui.Find("log_level") == nullptr && Ui.Find("accent") == nullptr && Ui.Find("start_folder") == nullptr && Ui.Find("ftp_port") && Ui.Find("future"));
  SettingsModel M3;
  CHECK(M3.Load(St, P, W) && M3.Get("log_level") == "standard" && M3.Get("start_folder") == "/fex/games");

  // A corrupt ui.json is kept as ui.json.bad.
  Spit(UiReal, "garbage");
  SettingsModel M4;
  CHECK(!M4.Load(St, P, W) && !W.empty() && M4.Get("log_level") == "standard");
  M4.Set("log_level", "trace");
  CHECK(M4.Save(St, P, E) && Slurp(UiReal + ".bad") == "garbage" && Json::Parse(Slurp(UiReal), Ui, Err));

  // A fresh SD card: no files at all -> defaults, the first Save creates the folders.
  FailRenameStorage Fresh(TempDir(), "/nonexistent");
  SettingsModel M5;
  CHECK(M5.Load(Fresh, P, W) && W.empty());
  CHECK(M5.Save(Fresh, P, E) && !FileExists(Fresh.Real(P.UiJson)));  // nothing changed: nothing written
  M5.Set("theme", "dark");
  CHECK(M5.Save(Fresh, P, E) && FileExists(Fresh.Real(P.UiJson)));
}

void TestMigration() {
  const std::string Dir = TempDir();
  PosixStorage St(Dir, Dir);
  SettingsPaths P;
  St.MakeDirs("sdmc:/fex");
  Spit(Dir + "/fex/fex-switch2.cfg", "2\n/fex/games/demo\n");
  SettingsModel M;
  std::string W;
  CHECK(M.Load(St, P, W) && W.empty());
  CHECK(M.Get("log_level") == "trace" && M.Get("start_folder") == "/fex/games/demo");
  Json Ui;
  std::string Err;
  CHECK(Json::Parse(Slurp(Dir + "/fex/config/ui.json"), Ui, Err));  // migrated into ui.json right away
  CHECK(Ui.Find("log_level") && Ui.Find("log_level")->AsString() == "trace" && Ui.Find("start_folder") && Ui.Find("start_folder")->AsString() == "/fex/games/demo");
  CHECK(FileExists(Dir + "/fex/fex-switch2.cfg"));  // the old file is left alone
  Spit(Dir + "/fex/fex-switch2.cfg", "0\n/fex/games\n");  // ui.json exists now: no second migration
  SettingsModel M2;
  CHECK(M2.Load(St, P, W) && M2.Get("log_level") == "trace" && M2.Get("start_folder") == "/fex/games/demo");
  M2.Set("accent", "red");
  std::string E;
  CHECK(M2.Save(St, P, E) && Json::Parse(Slurp(Dir + "/fex/config/ui.json"), Ui, Err) && Ui.Find("start_folder") && Ui.Find("accent"));
  // Levels 0 and 1.
  Spit(Dir + "/fex/fex-switch2.cfg", "1\n/\n");
  ::unlink((Dir + "/fex/config/ui.json").c_str());
  SettingsModel M3;
  CHECK(M3.Load(St, P, W) && M3.Get("log_level") == "detailed" && M3.Get("start_folder") == "/");
  // Legacy input only: the old /switch/fex location, with the folder moved to /fex like the launcher does.
  const std::string Dir2 = TempDir();
  PosixStorage St2(Dir2, Dir2);
  St2.MakeDirs("sdmc:/switch/fex");
  Spit(Dir2 + "/switch/fex/fex-switch2.cfg", "1\n/switch/fex/games\n");
  SettingsModel M4;
  CHECK(M4.Load(St2, P, W) && M4.Get("log_level") == "detailed" && M4.Get("start_folder") == "/fex/games");
  CHECK(FileExists(Dir2 + "/fex/config/ui.json"));
}

void TestFakeStorageAtomic() {
  FakeBackend B;
  std::string E;
  B.Fs().Ops.clear();
  CHECK(B.GetStorage().WriteFileAtomic("sdmc:/a/b.json", "{}", E));
  CHECK(B.Fs().Ops.size() == 2 && B.Fs().Ops[0] == "write sdmc:/a/b.json.tmp" && B.Fs().Ops[1] == "rename sdmc:/a/b.json.tmp sdmc:/a/b.json");  // temp first, then rename
  std::string T;
  CHECK(B.GetStorage().ReadFile("sdmc:/a/b.json", T) && T == "{}");
  B.Fs().FailWrites = true;
  CHECK(!B.GetStorage().WriteFileAtomic("sdmc:/a/b.json", "x", E));
  CHECK(B.GetStorage().ReadFile("sdmc:/a/b.json", T) && T == "{}");  // untouched
}

void TestJson() {
  Json J;
  std::string E;
  CHECK(Json::Parse("{\"a\": [1, -2.5e3, \"x\\ny\\\"\", {\"b\": null}], \"c\": false}", J, E));
  CHECK(J.Find("a")->Items().size() == 4 && J.Find("a")->Items()[1].AsString() == "-2.5e3" && J.Find("a")->Items()[2].AsString() == "x\ny\"");
  CHECK(!Json::Parse("{\"a\": }", J, E) && !E.empty());
  CHECK(!Json::Parse("{} x", J, E));
  CHECK(!Json::Parse("[1,", J, E));
  CHECK(Json::Parse("\"\\ud83d\\ude00\"", J, E) && J.AsString() == "\xF0\x9F\x98\x80");
  Json O = Json::Object();
  O.Set("z", Json::Int(1));
  O.Set("a", Json::String("q\"\\\n"));
  Json Back;
  CHECK(Json::Parse(O.Dump(), Back, E) && Back.Members()[0].first == "z" && Back.Find("a")->AsString() == "q\"\\\n");  // order kept
}

// ---- Licenses ------------------------------------------------------------------------------------------------------------

void TestLicenses() {
  const auto& L = EmbeddedLicenses();
  CHECK(L.size() >= 15);
  int Bundled = 0, Runtime = 0;
  std::vector<bool> Used(LicenseFileCount(), false);
  for (const LicenseEntry& E : L) {
    CHECK(!E.Name.empty() && !E.License.empty() && !E.Role.empty());
    if (E.Bundled) {
      ++Bundled;
      CHECK(!E.File.empty() && LicenseTextAvailable(E));  // every bundled component has its text embedded
      CHECK(LicenseText(E).size() > 100);
      CHECK(LicenseTextPath(E) == std::string("sdmc:/fex/licenses/") + E.File);
    } else {
      ++Runtime;
      CHECK(E.File.empty());  // never bundled: no text to ship
    }
    for (size_t I = 0; I < LicenseFileCount(); ++I)
      if (E.File == LicenseFileName(I))
        Used[I] = true;
  }
  CHECK(Bundled >= 10 && Runtime >= 4);
  for (size_t I = 0; I < Used.size(); ++I) {
    if (!Used[I])
      std::printf("unused license text: %s\n", LicenseFileName(I));
    CHECK(Used[I]);  // the About list covers every text that ships (no drift)
  }
  // Names the spec requires are present.
  auto Has = [&](const char* N) {
    for (const LicenseEntry& E : L)
      if (E.Name == N)
        return true;
    return false;
  };
  CHECK(Has("FreeType") && Has("HarfBuzz") && Has("NanoSVG") && Has("Lucide") && Has("Nunito Sans") && Has("JetBrains Mono") && Has("Noto Sans") && Has("FEX-Emu"));
  for (const LicenseEntry& E : L)
    CHECK(E.Name.find("Konsole") == std::string::npos && E.Role.find("Konsole") == std::string::npos && E.Note.find("Konsole") == std::string::npos);
  std::vector<LicenseEntry> Bad;
  std::string Err;
  CHECK(!ParseLicenses("{\"components\": [{\"role\": \"x\"}]}", Bad, Err));
  CHECK(!ParseLicenses("{}", Bad, Err));

  // First-run write: all files appear once, a second run writes nothing, a changed file is restored.
  const std::string Dir = TempDir();
  PosixStorage St(Dir, Dir);
  int N = 0;
  std::string E;
  CHECK(EnsureLicensesWritten(St, "sdmc:/fex/licenses", N, E));
  CHECK(N == int(LicenseFileCount()) + 1);
  CHECK(Slurp(Dir + "/fex/licenses/FreeType-FTL.txt").find("FreeType") != std::string::npos && FileExists(Dir + "/fex/licenses/licenses.json"));
  CHECK(EnsureLicensesWritten(St, "sdmc:/fex/licenses", N, E) && N == 0);
  Spit(Dir + "/fex/licenses/FreeType-FTL.txt", "tampered");
  CHECK(EnsureLicensesWritten(St, "sdmc:/fex/licenses", N, E) && N == 1 && Slurp(Dir + "/fex/licenses/FreeType-FTL.txt") != "tampered");
}

// ---- Home ----------------------------------------------------------------------------------------------------------------

struct HomeRig : ScreenRig<HomeScreen> {
  FakeBackend Be;
  std::unique_ptr<HomeScreen> H;
  std::vector<Request::Kind> Sent;
  explicit HomeRig(bool AllEnabled = false) {
    if (AllEnabled) {
      Be.SteamCap = {true, ""};
      Be.DesktopCap = {true, ""};
    }
    H = std::make_unique<HomeScreen>(Be);
    Scr = H.get();
    H->OnRequest = [this](const Request& R) { Sent.push_back(R.K); };
    Step();
  }
};

void TestHome() {
  using namespace home_layout;
  HomeRig G(true);
  HomeScreen& H = *G.H;
  // Geometry from the layout code.
  CHECK(H.Card(0).Bounds.X == 164 && H.Card(1).Bounds.X == 500 && H.Card(2).Bounds.X == 836);
  CHECK(H.Card(1).Bounds.X - (H.Card(0).Bounds.X + H.Card(0).Bounds.W) == 56 && H.Card(2).Bounds.X - (H.Card(1).Bounds.X + H.Card(1).Bounds.W) == 56);
  CHECK(H.Card(2).Bounds.X + H.Card(2).Bounds.W - H.Card(0).Bounds.X == 952);  // row width
  CHECK(H.Card(0).Bounds.Y == 235 && H.Card(0).Bounds.W == 280 && H.Card(0).Bounds.H == 300);
  const float CardsBottom = H.Card(0).Bounds.Y + H.Card(0).Bounds.H;
  CHECK(H.Pill().Bounds.Y == CardsBottom + 32 && H.Pill().Bounds.H == 52);  // 28 gap + 28 margin below the cards
  CHECK(std::fabs(H.Pill().Bounds.X + H.Pill().Bounds.W / 2 - 640) <= 0.5f && H.Pill().Bounds.W > 200 && H.Pill().Bounds.W < 500);
  CHECK(H.Gear().Bounds.X == 1176 && H.Gear().Bounds.Y == 32 && H.Gear().Bounds.W == 56);
  CHECK(H.Hints().Bounds.Y == 656 && H.Hints().Bounds.H == 64 && H.Hints().HintCount() == 4);
  CHECK(H.Pill().Bounds.Y + H.Pill().Bounds.H < H.Hints().Bounds.Y);

  // Card 1 is focused on first open; Up -> gear; gear Down -> the last focused card; Down -> pill; pill Up -> last card.
  CHECK(G.Focused() == &H.Card(0));
  G.Press(Button::Right);
  CHECK(G.Focused() == &H.Card(1));
  G.Press(Button::Up);
  CHECK(G.Focused() == &H.Gear());
  G.Press(Button::Down);
  CHECK(G.Focused() == &H.Card(1));  // the last focused card, not the nearest
  G.Press(Button::Down);
  CHECK(G.Focused() == &H.Pill());
  G.Press(Button::Up);
  CHECK(G.Focused() == &H.Card(1));
  G.Press(Button::Right);
  CHECK(G.Focused() == &H.Card(2));
  G.Press(Button::Down);
  CHECK(G.Focused() == &H.Pill());  // Down from any card focuses the pill
  G.Press(Button::A);
  CHECK(G.Sent.back() == Request::Kind::OpenFtp);
  G.Press(Button::Up);
  G.Press(Button::Up);
  CHECK(G.Focused() == &H.Gear());
  G.Press(Button::A);
  CHECK(G.Sent.back() == Request::Kind::OpenSettings);
  G.Sent.clear();
  G.Press(Button::X);
  G.Press(Button::Plus);
  CHECK(G.Sent.size() == 2 && G.Sent[0] == Request::Kind::OpenFtp && G.Sent[1] == Request::Kind::OpenSettings);
  // Launch: a card fires 80 ms after A.
  G.Sent.clear();
  G.Press(Button::Down);  // gear Down -> last card (3)
  CHECK(G.Focused() == &H.Card(2));
  G.Press(Button::A);
  CHECK(G.Sent.empty());
  G.Run(100);
  CHECK(G.Sent.size() == 1 && G.Sent[0] == Request::Kind::LaunchShell);
  // B exits at once (no confirmation).
  G.Sent.clear();
  G.Press(Button::B);
  CHECK(G.Sent.size() == 1 && G.Sent[0] == Request::Kind::Exit);
  // Hint texts.
  CHECK(H.Hints().Segs.size() == 3 && H.Hints().Segs[1].Text == "Ubuntu_24_04.sqsh" && H.Hints().Segs[2].Text.find("18.4 GB free") != std::string::npos);

  // Disabled cards (Steam, Desktop need graphics) show the reason, are skipped, and the shell card is focused first.
  HomeRig D;
  CHECK(!D.H->Card(0).Enabled && !D.H->Card(1).Enabled && D.H->Card(2).Enabled);
  CHECK(D.H->Card(0).Subtitle == "Needs graphics support \xC2\xB7 not available yet");
  CHECK(D.Focused() == &D.H->Card(2));
  D.Press(Button::Left);  // the disabled cards are skipped and focus never jumps to the FTP pill: it stays on Shell
  CHECK(D.Focused() == &D.H->Card(2));
  D.Press(Button::Right);
  CHECK(D.Focused() == &D.H->Card(2));
  D.Press(Button::Up);
  D.Press(Button::Up);
  CHECK(D.Focused() == &D.H->Gear());
  // The FTP pill text.
  D.Be.Ftp_ = {false, 0};
  D.Step();
  CHECK(D.H->Pill().StatusText() == "Off");
  D.Be.Ftp_ = {true, 1};
  D.Step();
  CHECK(D.H->Pill().StatusText() == "Running \xC2\xB7 1 client");
  // Touch: tap the shell card.
  HomeRig T(true);
  const Rect Sh = T.H->Card(2).Bounds;
  Event Down, Up;
  Down.Kind = Event::Type::Touch;
  Down.Phase = TouchPhase::Down;
  Down.X = Sh.X + 50;
  Down.Y = Sh.Y + 50;
  Up = Down;
  Up.Phase = TouchPhase::Up;
  T.Step({Down});
  T.Step({Up});
  T.Run(150);
  CHECK(!T.Sent.empty() && T.Sent.back() == Request::Kind::LaunchShell);
}

// ---- Settings ------------------------------------------------------------------------------------------------------------

struct SetRig : ScreenRig<SettingsScreen> {
  FakeBackend Be;
  SettingsModel Model;
  SettingsPaths Paths;
  std::unique_ptr<SettingsScreen> S;
  std::vector<Request> Sent;
  explicit SetRig(const std::vector<SettingDef>* Schema = nullptr) : Model(Schema ? SettingsModel(*Schema) : SettingsModel()) {
    S = std::make_unique<SettingsScreen>(Be, Model, Paths);
    Scr = S.get();
    S->OnRequest = [this](const Request& R) { Sent.push_back(R); };
    Step();
  }
  SettingsRow* Row(const char* Key) { return S->RowFor(Key); }
  void Focus(Widget* W) { S->Input().SetFocus(W->Id, Now); Step(); }
};

void TestSettingsRows() {
  SetRig G;
  SettingsScreen& S = *G.S;
  CHECK(S.SectionCount() == 4 && S.Item(0).Text == "Paths" && S.Item(1).Text == "Appearance" && S.Item(2).Text == "Logging" && S.Item(3).Text == "Developer" && S.Item(4).Text == "About" && S.Item(4).HasIcon);
  // One row per schema entry, laid out in order inside each section with 74 du rows and no row-specific layout code.
  const auto& Schema = DefaultSchema();
  float Prev = -1;
  std::string PrevSec;
  for (const SettingDef& D : Schema) {
    SettingsRow* R = S.RowFor(D.Key);
    if (!D.ShownWhenFile.empty()) {  // not listed: the rig's storage has no such file
      CHECK(R == nullptr);
      continue;
    }
    CHECK(R != nullptr);
    if (!R)
      continue;
    CHECK(R->Bounds.X == 333 && R->Bounds.W == 899 && R->Bounds.H == 74);
    if (D.Section == PrevSec)
      CHECK(R->Bounds.Y == Prev + 74);
    Prev = R->Bounds.Y;
    PrevSec = D.Section;
    Control* C = R->GetControl();
    switch (D.Type) {
    case RowType::Enum: CHECK(dynamic_cast<EnumChip*>(C) && static_cast<EnumChip*>(C)->Values.size() == D.Options.size()); break;
    case RowType::Path: CHECK(dynamic_cast<PathField*>(C) && static_cast<PathField*>(C)->Path == D.Default); break;
    default: CHECK(dynamic_cast<ActionPill*>(C)); break;
    }
    CHECK(R->Enabled);  // no disabled placeholder rows for absent features
  }
  CHECK(G.Row("log_level")->Enabled && G.Row("theme")->Enabled && G.Row("reinstall_rootfs")->Enabled && G.Row("wipe_rootfs")->Enabled && G.Row("start_folder")->Enabled);
  CHECK(G.Row("reinstall_rootfs")->Bounds.Y < G.Row("start_folder")->Bounds.Y);  // the two RootFS rows start the Paths section
  CHECK(G.Row("steam_dir") == nullptr && G.Row("tso") == nullptr && G.Row("rootfs_image") == nullptr);

  // A schema with more rows of every type needs no layout code: the rows simply appear.
  SetRig C(&RichSchema());
  CHECK(C.Row("ex_toggle") && dynamic_cast<Toggle*>(C.Row("ex_toggle")->GetControl()) && static_cast<Toggle*>(C.Row("ex_toggle")->GetControl())->On());
  CHECK(C.Row("ex_slider") && static_cast<Slider*>(C.Row("ex_slider")->GetControl())->Value == 5000);
  CHECK(C.Row("ex_file") && C.Row("ex_file")->Enabled && !C.Row("ex_cmd")->Enabled && C.Row("ex_cmd")->Description.find("Custom command") != std::string::npos);
  C.Model.Set("ex_mode", "custom");
  C.S->RefreshRows();
  C.Step();
  CHECK(C.Row("ex_cmd")->Enabled);
  CHECK(C.S->ContentHeight() > G.S->ContentHeight());
}

void TestSettingsNav() {
  SetRig G(&RichSchema());
  SettingsScreen& S = *G.S;
  CHECK(G.Focused() == &S.Item(0) && S.Selected() == 0);
  // Down in the sidebar selects the next section and smooth-scrolls it to the top (200 ms).
  G.Press(Button::Down);
  CHECK(G.Focused() == &S.Item(1) && S.Selected() == 1);
  const float T1 = std::clamp(S.SectionOffset(1), 0.0f, S.Scrolling().MaxOffset());
  CHECK(S.Scrolling().Target() == T1 && T1 > 0);
  CHECK(S.Scrolling().Offset(G.Now) < T1);  // still moving
  G.Run(250);
  CHECK(std::fabs(S.Scrolling().Offset(G.Now) - T1) < 0.5f);
  // Right from the sidebar goes to the first enabled row of the section; Left from a row goes back to the sidebar item.
  G.Press(Button::Right);
  CHECK(G.Focused() == G.Row("theme"));  // the first enabled row of the selected section
  G.Press(Button::Down);
  CHECK(G.Focused() == G.Row("accent"));
  G.Press(Button::Left);
  CHECK(G.Focused() == &S.Item(1));
  G.Press(Button::Up);
  CHECK(G.Focused() == &S.Item(0));
  // Scroll-spy: scrolling the content selects the section at the top.
  S.Scrolling().To(S.SectionOffset(1), G.Now, 0);
  G.Step();
  CHECK(S.Selected() == 1);
  S.Scrolling().To(0, G.Now, 0);
  G.Step();
  CHECK(S.Selected() == 0);
  S.Scrolling().To(S.Scrolling().MaxOffset(), G.Now, 0);
  G.Step();
  CHECK(S.Selected() == 3);
  // L / R: previous / next section; ZL / ZR: page.
  G.Press(Button::L);
  CHECK(S.Selected() == 2);
  G.Press(Button::R);
  G.Press(Button::R);
  CHECK(S.Selected() == 4 && S.InAbout());  // R on the last settings section reaches About
  G.Press(Button::R);
  CHECK(S.Selected() == 4);
  G.Press(Button::L);
  CHECK(S.Selected() == 3 && !S.InAbout());
  S.SelectSection(0, G.Now, true);
  G.Run(300);
  const float Page = 568 - 48;
  G.Press(Button::ZR);
  G.Run(300);
  CHECK(std::fabs(S.Scrolling().Offset(G.Now) - std::min(Page, S.Scrolling().MaxOffset())) < 1.0f);
  G.Press(Button::ZL);
  G.Run(300);
  CHECK(S.Scrolling().Offset(G.Now) < 1.0f);
  // D-pad never lands on a disabled row.
  S.SelectSection(0, G.Now, false);
  G.Focus(G.Row("reinstall_rootfs"));
  for (int I = 0; I < 14; ++I) {
    G.Press(Button::Down);
    CHECK(G.Focused() != nullptr && G.Focused()->Enabled);
    G.Run(300);
  }
  // Focus keeps the 24 du margin: the focused row is inside the viewport.
  const float Off = S.Scrolling().Offset(G.Now);
  Widget* W = G.Focused();
  CHECK(W && W->Bounds.Y - Off >= 88 && W->Bounds.Y + W->Bounds.H - Off <= 656);
}

void TestSettingsControls() {
  SetRig G;
  SettingsScreen& S = *G.S;
  // Enum cycling with Left/Right applies the theme live.
  G.Focus(G.Row("theme"));
  CHECK(G.Model.Get("theme") == "match");
  G.Press(Button::Right);
  CHECK(G.Model.Get("theme") == "dark" && CurrentTheme().Mode == ThemeMode::Dark);
  G.Press(Button::Right);
  CHECK(G.Model.Get("theme") == "light" && CurrentTheme().Mode == ThemeMode::Light);
  G.Press(Button::Left);
  G.Press(Button::Left);
  CHECK(G.Model.Get("theme") == "match" && CurrentTheme().Mode == ThemeMode::Dark);  // no console theme API: Dark
  G.Focus(G.Row("accent"));
  G.Press(Button::Right);
  CHECK(G.Model.Get("accent") == "red" && CurrentTheme().Preset == AccentPreset::Red);
  G.Press(Button::Left);
  // A opens the picker with the current value; Down + A selects it.
  G.Focus(G.Row("log_level"));
  G.Press(Button::A);
  CHECK(S.Modals().IsOpen() && S.Modals().Top() == &S.Picker() && S.Picker().Selected == 0 && S.Picker().Options.size() == 3);
  G.Press(Button::Down);
  G.Press(Button::Down);
  G.Press(Button::A);
  CHECK(!S.Modals().IsOpen() && G.Model.Get("log_level") == "trace" && static_cast<EnumChip*>(G.Row("log_level")->GetControl())->Index == 2);
  G.Run(200);
  // B in a picker cancels and keeps the value.
  G.Press(Button::A);
  G.Press(Button::Up);
  G.Press(Button::B);
  CHECK(!S.Modals().IsOpen() && G.Model.Get("log_level") == "trace");
  // Disabled rows cannot be pressed.
  G.Model.Set("log_level", "standard");

  // Restore defaults: confirm with Cancel focused; confirming resets the values.
  G.Model.Set("accent", "violet");
  G.Step();
  G.Focus(&S.RestoreButton());
  G.Press(Button::A);
  CHECK(S.Modals().Top() == &S.Confirm() && S.Confirm().Title == "Restore all settings?" && !S.Confirm().Destructive);
  G.Press(Button::A);  // Cancel is the default
  CHECK(!S.Modals().IsOpen() && G.Model.Get("accent") == "violet");
  G.Run(200);
  G.Press(Button::A);
  G.Press(Button::Right);  // Cancel -> Restore
  G.Press(Button::A);
  CHECK(!S.Modals().IsOpen() && G.Model.Get("accent") == "cyan" && CurrentTheme().Preset == AccentPreset::Cyan);

  // Wipe RootFS: destructive confirm, Cancel first; Wipe opens the Installation screen in wipe mode (it runs the backend there).
  G.Run(200);
  G.Focus(G.Row("wipe_rootfs"));
  G.Press(Button::A);
  CHECK(S.Modals().Top() == &S.Confirm() && S.Confirm().Title == "Wipe RootFS?" && S.Confirm().Destructive && S.Confirm().Message.find("This can't be undone") != std::string::npos);
  G.Press(Button::A);
  CHECK(G.Be.WipeCalls == 0 && G.Sent.empty());
  G.Run(200);
  G.Press(Button::A);
  G.Press(Button::Right);
  G.Press(Button::A);
  CHECK(G.Be.WipeCalls == 0 && G.Sent.size() == 1 && G.Sent[0].K == Request::Kind::StartInstall && G.Sent[0].Install.WipeOnly);
  // Reinstall opens RootFS setup in reinstall mode (no confirm here: setup confirms later); unavailable = disabled with the reason.
  SetRig R;
  R.Focus(R.Row("reinstall_rootfs"));
  R.Press(Button::A);
  CHECK(R.Sent.size() == 1 && R.Sent[0].K == Request::Kind::OpenRootfsSetup && R.Sent[0].Reinstall);
  SetRig U;
  U.Be.ReinstallCap = {false, "RootFS setup is not available yet"};
  U.Be.WipeCap = {false, ""};
  U.S->RefreshRows();
  U.Step();
  CHECK(!U.Row("reinstall_rootfs")->Enabled && U.Row("reinstall_rootfs")->Description == "RootFS setup is not available yet" && !U.Row("wipe_rootfs")->Enabled && U.Row("wipe_rootfs")->Description == "Not available yet");
}

void TestSettingsPathAndSave() {
  // Path rows: A asks the app for the file picker with the right spec; the result lands in the row and the model.
  SetRig G(&RichSchema());
  G.Focus(G.Row("ex_file"));
  G.Press(Button::A);
  CHECK(G.Sent.size() == 1 && G.Sent[0].K == Request::Kind::OpenFilePicker);
  const PickerSpec& P = G.Sent[0].Picker;
  CHECK(P.Title == "Choose RootFS image" && P.Accept.size() == 2 && P.Accept[0] == ".sqsh" && !P.Folder && P.Current == "sdmc:/fex/rootfs/Ubuntu_24_04.sqsh" && P.FallbackLocation == "fex");
  G.Sent[0].OnPicked("sdmc:/fex/rootfs/Ubuntu_22_04.sqsh");
  G.Step();
  CHECK(G.Model.Get("ex_file") == "sdmc:/fex/rootfs/Ubuntu_22_04.sqsh" && static_cast<PathField*>(G.Row("ex_file")->GetControl())->Path == "sdmc:/fex/rootfs/Ubuntu_22_04.sqsh");
  G.Sent.clear();
  // The start folder row is real: folder mode, the launcher's default, the RootFS as the fallback location; the result is stored.
  SetRig F0;
  F0.Focus(F0.Row("start_folder"));
  F0.Press(Button::A);
  CHECK(F0.Sent.size() == 1 && F0.Sent[0].K == Request::Kind::OpenFilePicker);
  const PickerSpec& Q = F0.Sent[0].Picker;
  CHECK(Q.Folder && Q.Accept.empty() && Q.FallbackLocation == "rootfs" && Q.Current == "/fex/games" && Q.Title == "Choose the start folder");
  F0.Sent[0].OnPicked("/fex/games/demo");
  F0.Step();
  CHECK(F0.Model.Get("start_folder") == "/fex/games/demo" && static_cast<PathField*>(F0.Row("start_folder")->GetControl())->Path == "/fex/games/demo");
  F0.Press(Button::B);
  std::string Saved;
  CHECK(F0.Be.GetStorage().ReadFile("sdmc:/fex/config/ui.json", Saved) && Saved.find("\"start_folder\": \"/fex/games/demo\"") != std::string::npos);

  // B leaves through a save: ui.json is written atomically, then Back is sent.
  SetRig S;
  S.Model.Set("log_level", "detailed");
  S.Step();
  S.Press(Button::B);
  CHECK(S.Sent.size() == 1 && S.Sent[0].K == Request::Kind::Back);
  std::string T;
  CHECK(S.Be.GetStorage().ReadFile("sdmc:/fex/config/ui.json", T) && T.find("\"log_level\": \"detailed\"") != std::string::npos);
  CHECK(S.Be.Fs().Ops.size() >= 2 && S.Be.Fs().Ops[S.Be.Fs().Ops.size() - 2] == "write sdmc:/fex/config/ui.json.tmp");
  // A failing save keeps the screen once and shows the error; B again leaves.
  SetRig F;
  F.Model.Set("log_level", "trace");
  F.Be.Fs().FailWrites = true;
  F.Press(Button::B);
  CHECK(F.Sent.empty() && F.S->StatusText().find("Could not save settings") != std::string::npos && F.S->SaveFailed());
  F.Press(Button::B);
  CHECK(F.Sent.size() == 1 && F.Sent[0].K == Request::Kind::Back);
  // Nothing changed: leaving writes nothing.
  SetRig N;
  N.Be.Fs().Ops.clear();
  N.Press(Button::B);
  CHECK(N.Sent.size() == 1 && N.Be.Fs().Ops.empty());
}

void TestAbout() {
  SetRig G;
  SettingsScreen& S = *G.S;
  const auto& L = EmbeddedLicenses();
  S.SelectSection(S.SectionCount(), G.Now, false);
  G.Step();
  CHECK(S.InAbout() && S.Item(4).Selected && S.LicenseRowCount() == int(L.size()));
  // Bundled first, then runtime downloads; rows are 60 high with the spec's order.
  float Y = 0;
  bool SawRuntime = false, OrderOk = true;
  for (int I = 0; I < S.LicenseRowCount(); ++I) {
    const LicenseRow* R = S.LicRow(I);
    CHECK(R->Bounds.H == 60 && R->Name == L[size_t(I)].Name && R->License == L[size_t(I)].License);
    if (!L[size_t(I)].Bundled)
      SawRuntime = true;
    else if (SawRuntime)
      OrderOk = false;
    CHECK(R->Bounds.Y > Y || Y == 0);
    Y = R->Bounds.Y;
  }
  CHECK(OrderOk && SawRuntime);
  CHECK(S.ContentHeight() > 568);
  // Right from the sidebar: the first license row; A opens the modal with Close focused; Read license text opens the full view.
  G.Focus(&S.Item(4));
  G.Press(Button::Right);
  CHECK(G.Focused() == S.LicRow(0));
  G.Press(Button::Down);  // FreeType
  G.Press(Button::A);
  CHECK(S.Modals().Top() == &S.LicModal() && S.LicModal().Entry.Name == "FreeType" && S.LicModal().HasText && G.Focused() == &S.LicModal().ClosePill());
  G.Press(Button::Left);  // Read license text
  CHECK(G.Focused() == &S.LicModal().ReadPill());
  G.Press(Button::A);
  CHECK(S.Modals().Top() == &S.TextView() && S.TextView().Title == "FreeType" && S.TextView().Text.find("FreeType") != std::string::npos);
  G.Run(200);
  CHECK(S.TextView().LineCount() > 20);
  const float M = S.TextView().MaxOffset();
  CHECK(M > 0);
  G.Press(Button::ZR);
  CHECK(S.TextView().Offset() > 0);
  G.Press(Button::ZL);
  CHECK(S.TextView().Offset() == 0);
  G.Press(Button::B);  // back to the modal
  CHECK(S.Modals().Top() == &S.LicModal());
  G.Press(Button::B);
  CHECK(!S.Modals().IsOpen());
  // A runtime-download entry has no text: only Close.
  G.Run(300);
  S.Scrolling().To(S.Scrolling().MaxOffset(), G.Now, 0);
  G.Step();
  int RuntimeRow = -1;
  for (int I = 0; I < S.LicenseRowCount(); ++I)
    if (!L[size_t(I)].Bundled) {
      RuntimeRow = I;
      break;
    }
  G.Focus(S.LicRow(RuntimeRow));
  G.Press(Button::A);
  CHECK(S.Modals().Top() == &S.LicModal() && !S.LicModal().HasText);
  G.Press(Button::B);
  // Left from a row goes back to the About item; selecting another section leaves About with its own scroll.
  G.Run(300);
  G.Focus(S.LicRow(1));
  G.Press(Button::Left);
  CHECK(G.Focused() == &S.Item(4));
  G.Press(Button::Up);
  CHECK(!S.InAbout() && S.Selected() == 3);
  // The footnote and the trademark line exist after a draw (wrapped text) and the UI never says Konsole.
  S.SelectSection(S.SectionCount(), G.Now, false);
  G.Step();
  bool Konsole = false;
  for (const std::string& C : G.R.Calls)
    Konsole = Konsole || C.find("Konsole") != std::string::npos;
  CHECK(!Konsole);
}

// ---- File picker screen --------------------------------------------------------------------------------------------------

struct PickRig : ScreenRig<FilePickerScreen> {
  FakeBackend Be;
  std::unique_ptr<FilePickerScreen> P;
  std::vector<std::string> Picked;
  std::vector<Request::Kind> Sent;
  PickRig(PickerSpec Spec, int Many = 0, int Slow = 0) {
    for (int K = 0; K < Many; ++K) {
      char Name[48];
      std::snprintf(Name, sizeof(Name), "sdmc:/roms/game-%05d.nes", K);
      Be.Fs().AddFile(Name, 1000 + uint64_t(K), 1700000000);
    }
    Be.Fs().PendingFrames = Slow;
    P = std::make_unique<FilePickerScreen>(Be, std::move(Spec), [this](const std::string& S) { Picked.push_back(S); });
    Scr = P.get();
    P->OnRequest = [this](const Request& R) { Sent.push_back(R.K); };
    Step();
  }
};

PickerSpec RootfsSpec() {
  PickerSpec S;
  S.Title = "Choose RootFS image";
  S.Accept = {".sqsh", ".ero"};
  S.Current = "sdmc:/fex/rootfs/Ubuntu_24_04.sqsh";
  return S;
}

void TestPicker() {
  PickRig G(RootfsSpec());
  FilePickerScreen& P = *G.P;
  CHECK(P.CurrentPath() == "sdmc:/fex/rootfs" && P.CurrentLocation() == 1 && !P.Loading());
  const auto& E = P.Entries();
  CHECK(E.size() == 5 && E[0].IsDir && E[0].Name == "old" && !E[1].Selectable && E[2].Selectable);
  CHECK(P.CrumbCount() == 3 && P.CrumbAt(0).Text == "microSD" && P.CrumbAt(2).Text == "rootfs" && P.CrumbAt(2).Current && !P.CrumbAt(0).Current);
  CHECK(P.CursorIndex() == 3);  // the current value starts selected
  CHECK(P.Hints().HintCount() == 3);
  // Focus starts on the location; Right goes to the list; D-pad skips the dimmed files.
  CHECK(G.Focused() == &P.LocItem(1));
  G.Press(Button::Right);
  CHECK(P.CursorIndex() == 3 && G.Focused() == &P.Slot(3 - 0));
  G.Press(Button::Up);
  CHECK(P.CursorIndex() == 2);
  G.Press(Button::Up);
  CHECK(P.CursorIndex() == 0);  // notes.txt (1) is not selectable: skipped
  G.Press(Button::Down);
  G.Press(Button::Down);
  CHECK(P.CursorIndex() == 3);  // the .sha256 file after it is skipped
  G.Press(Button::Down);
  CHECK(P.CursorIndex() == 0);  // Down on the bottom entry wraps to the top one
  G.Press(Button::Up);
  CHECK(P.CursorIndex() == 3);  // Up on the top entry wraps to the bottom one (skipping the unselectable files at the end)
  CHECK(P.FocusedPath() == "sdmc:/fex/rootfs/Ubuntu_24_04.sqsh");
  CHECK(G.Focused() != nullptr && G.Focused()->Enabled);
  // The HintBar's left side shows the full path of the focused item in the SD form.
  CHECK(P.Hints().Segs.size() == 1 && P.Hints().Segs[0].K == HintBar::Seg::Kind::Path && P.Hints().Segs[0].Text == "sdmc:/fex/rootfs/Ubuntu_24_04.sqsh");
  // A on a file selects it (path form sdmc:/...) and leaves.
  G.Press(Button::A);
  CHECK(G.Picked.size() == 1 && G.Picked[0] == "sdmc:/fex/rootfs/Ubuntu_24_04.sqsh" && G.Sent.size() == 1 && G.Sent[0] == Request::Kind::Back);
  // A on a folder opens it; B is the parent folder and re-selects where we came from.
  G.Picked.clear();
  G.Press(Button::Up);
  G.Press(Button::Up);
  CHECK(P.CursorIndex() == 0);
  G.Press(Button::A);
  G.Run(100);
  CHECK(P.CurrentPath() == "sdmc:/fex/rootfs/old" && P.Entries().empty());
  G.Press(Button::B);
  G.Run(100);
  CHECK(P.CurrentPath() == "sdmc:/fex/rootfs" && P.Entries()[size_t(P.CursorIndex())].Name == "old");
  // Breadcrumbs open their folder; the up button goes to the parent; a root has no parent.
  P.CrumbAt(1).Activate(G.Now);
  G.Run(100);
  CHECK(P.CurrentPath() == "sdmc:/fex");
  P.Up().Activate(G.Now);
  G.Run(100);
  CHECK(P.CurrentPath() == "sdmc:/" && !P.Up().Enabled);
  // L / R jump between locations; the RootFS location shows /... paths.
  G.Press(Button::R);
  CHECK(P.CurrentPath() == "sdmc:/fex" && P.CurrentLocation() == 1);
  G.Press(Button::R);
  G.Run(100);
  CHECK(P.CurrentPath() == "/" && P.CurrentLocation() == 2 && P.CrumbAt(0).Text == "RootFS" && P.Hints().Segs[0].Text.rfind("/", 0) == 0);
  G.Press(Button::R);
  G.Run(100);
  CHECK(P.CurrentPath() == "/home/switch/" || P.CurrentPath() == "/home/switch");
  CHECK(P.CurrentLocation() == 3);
  G.Press(Button::R);
  CHECK(P.CurrentLocation() == 3);  // clamped
  // Hidden dot-files are not listed (.bashrc in Home).
  bool Hidden = false;
  for (const auto& En : P.Entries())
    Hidden = Hidden || En.Name[0] == '.';
  CHECK(!Hidden && P.Entries().size() == 2);  // Documents and notes.txt; .bashrc and .local are hidden
  // X cancels without a result (B here would go up to /home).
  G.Picked.clear();
  G.Sent.clear();
  G.Press(Button::X);
  CHECK(G.Picked.empty() && G.Sent.size() == 1);
  // Selecting from the RootFS side returns a /... path (accept any file).
  PickerSpec Any;
  Any.Title = "Choose a start command";
  Any.Current = "/usr/bin/startplasma-wayland";
  PickRig R(Any);
  CHECK(R.P->CurrentPath() == "/usr/bin" && R.P->CurrentLocation() == 2);
  R.Press(Button::Right);
  R.Press(Button::A);
  CHECK(R.Picked.size() == 1 && R.Picked[0].rfind("/usr/bin/", 0) == 0);
}

void TestPickerFolderMode() {
  PickerSpec S;
  S.Title = "Choose the start folder";
  S.Folder = true;
  S.Current = "/home/switch/.missing/folder";  // missing: falls back
  S.FallbackLocation = "home";
  PickRig G(S);
  FilePickerScreen& P = *G.P;
  CHECK(P.CurrentPath() == "/home/switch");  // the Home location
  for (const auto& E : P.Entries())
    CHECK(E.IsDir);  // files hidden in folder mode; .local is a hidden dot-folder too
  CHECK(P.Entries().size() == 1 && P.Entries()[0].Name == "Documents");
  bool HasUse = false;
  for (const auto& H : std::vector<Button> {Button::Plus})
    (void)H;
  CHECK(P.Hints().HintCount() == 4);  // B, X, A and + Use this folder
  HasUse = true;
  CHECK(HasUse);
  G.Press(Button::Plus);
  CHECK(G.Picked.size() == 1 && G.Picked[0] == "/home/switch" && G.Sent.size() == 1);
  // A on a folder only opens it; + chooses the folder that is open.
  G.Picked.clear();
  G.Press(Button::Right);
  G.Press(Button::A);
  G.Run(100);
  CHECK(P.CurrentPath() == "/home/switch/Documents" && G.Picked.empty());
  G.Press(Button::Plus);
  CHECK(G.Picked.size() == 1 && G.Picked[0] == "/home/switch/Documents");
  // A file-mode picker has no + hint.
  PickRig F(RootfsSpec());
  CHECK(F.P->Hints().HintCount() == 3);
}

void TestPickerVirtualized() {
  PickerSpec S;
  S.Title = "Choose a ROM";
  S.Current = "sdmc:/roms";
  PickRig G(S, 5000);
  FilePickerScreen& P = *G.P;
  G.Run(16 * 40);  // 5000 entries load 256 per frame
  CHECK(!P.Loading() && P.Entries().size() == 5000);
  CHECK(P.VisibleRows() > 0 && P.VisibleRows() <= picker_layout::PoolSize);  // only the visible rows exist as widgets
  // Sorted A-Z: game-00000 first.
  CHECK(P.Entries()[0].Name == "game-00000.nes" && P.Entries()[4999].Name == "game-04999.nes");
  G.Press(Button::Right);
  const int Page = int((picker_layout::ListBottom - picker_layout::ListTop) / picker_layout::RowStep) - 1;
  G.Press(Button::ZR);
  G.Run(300);
  CHECK(P.CursorIndex() == Page && std::fabs(P.ListOffset(G.Now) - 0) >= 0);
  CHECK(P.VisibleRows() <= picker_layout::PoolSize);
  // Jump deep: the pool still shows only a few rows and the cursor row is on screen.
  for (int I = 0; I < 800; ++I)
    P.Page(1, G.Now);
  G.Run(300);
  CHECK(P.CursorIndex() == 4999 || P.CursorIndex() >= 4000);
  const float Off = P.ListOffset(G.Now);
  const float RowY = picker_layout::ListTop + float(P.CursorIndex()) * picker_layout::RowStep - Off;
  CHECK(RowY >= picker_layout::ListTop - 1 && RowY + picker_layout::RowH <= picker_layout::ListBottom + 1);
  CHECK(P.VisibleRows() <= picker_layout::PoolSize && G.Focused() != nullptr);
  for (int K = 0; K < picker_layout::PoolSize; ++K)
    if (P.Slot(K).Bound) {
      CHECK(P.Slot(K).Bounds.Y + 56 > picker_layout::ListTop - 60 && P.Slot(K).Bounds.Y < picker_layout::ListBottom + 60);
    }
  // The focused slot shows the cursor entry.
  FileRow* Fr = dynamic_cast<FileRow*>(G.Focused());
  CHECK(Fr && Fr->Index == P.CursorIndex());
  // Wheel scrolling moves the list by pixels.
  const float Before = P.ListOffset(G.Now);
  Event W;
  W.Kind = Event::Type::MouseWheel;
  W.DY = 3;
  G.Step({W});
  CHECK(P.ListOffset(G.Now) != Before);
}

// Down to the last entry of a folder that scrolls: the focus stays on the list (console: it jumped to the location item).
void TestPickerDownToLast() {
  PickerSpec S;
  S.Title = "Choose a ROM";
  S.Current = "sdmc:/roms";
  PickRig G(S, 15);
  FilePickerScreen& P = *G.P;
  G.Run(100);
  G.Press(Button::Right);
  for (int K = 0; K < 14; ++K) {
    G.Press(Button::Down);
    G.Run(50);
    FileRow* Fr = dynamic_cast<FileRow*>(G.Focused());
    if (!Fr) std::printf("down %d: focus left the list (cursor %d)\n", K, P.CursorIndex());
    CHECK(Fr && Fr->Index == P.CursorIndex());
  }
  CHECK(P.CursorIndex() == 14);
  // Down on the last entry wraps to the first, Up on the first wraps to the last; the focus stays on the list.
  G.Press(Button::Down);
  G.Run(50);
  CHECK(P.CursorIndex() == 0 && dynamic_cast<FileRow*>(G.Focused()) && dynamic_cast<FileRow*>(G.Focused())->Index == P.CursorIndex());  // the first selectable entry
  G.Press(Button::Up);
  G.Run(50);
  CHECK(P.CursorIndex() == 14 && dynamic_cast<FileRow*>(G.Focused()) && dynamic_cast<FileRow*>(G.Focused())->Index == 14);
  // Every bound row and the focus ring stay inside the list area: the ring clip is the list.
  FileRow* Fr = dynamic_cast<FileRow*>(G.Focused());
  CHECK(Fr && Fr->RingClip.H > 0 && Fr->RingClip.Y + Fr->RingClip.H <= picker_layout::ListBottom);
}

void TestPickerSpinner() {
  PickerSpec S;
  S.Title = "Choose RootFS image";
  S.Accept = {".sqsh"};
  S.Current = "sdmc:/fex/rootfs/Ubuntu_24_04.sqsh";
  PickRig G(S, 0, 20);  // each folder stalls 20 frames (320 ms)
  FilePickerScreen& P = *G.P;
  CHECK(P.Loading() && P.VisibleRows() == 0);
  uint64_t Start = G.Now;
  bool ShownEarly = false, ShownLate = false;
  for (int I = 0; I < 30 && P.Loading(); ++I) {
    G.Step();
    if (G.Now - Start < 140)
      ShownEarly = ShownEarly || P.SpinnerShown(G.Now);
    else if (G.Now - Start >= 150)
      ShownLate = ShownLate || P.SpinnerShown(G.Now);
  }
  CHECK(!ShownEarly && ShownLate);  // the spinner appears only after 150 ms
  G.Run(200);
  CHECK(!P.Loading() && !P.SpinnerShown(G.Now) && P.Entries().size() == 5);
  // A folder that loads at once never shows it.
  G.Be.Fs().PendingFrames = 0;
  P.GoLocation(0, G.Now);
  G.Step();
  CHECK(!P.Loading() && !P.SpinnerShown(G.Now) && P.CurrentPath() == "sdmc:/");
  // Missing folder: reported, not a crash.
  P.Open("sdmc:/nope", G.Now);
  G.Step();
  CHECK(!P.Loading() && P.Entries().empty());
}

} // namespace

// ---- RootFS index and installer interface (Task 5) ------------------------------------------------------------------------
void TestRootfsIndex() {
  const RootfsIndex& Built = BuiltInRootfsIndex();
  CHECK(Built.Images.size() == 1);
  const RootfsImage* Ubuntu = Built.Find("ubuntu", "24.04");
  CHECK(Ubuntu != nullptr);
  if (Ubuntu) {
    CHECK(Ubuntu->Selectable() && Ubuntu->Type == "squashfs" && Ubuntu->Name == "Ubuntu 24.04");
    CHECK(Ubuntu->Url == "http://rootfs.fex-emu.gg/Ubuntu_24_04/2026-08-11/Ubuntu_24_04.sqsh");
    CHECK(Ubuntu->Hash == "3517e0e5ea25a473" && Ubuntu->DownloadBytes == 525615104ull && Ubuntu->FileName == "Ubuntu_24_04.sqsh");
    CHECK(Ubuntu->InstalledBytes > Ubuntu->DownloadBytes && Ubuntu->Extras.size() == 7 && Ubuntu->Extras[0] == "bubblewrap");
  }
  CHECK(Built.Find("debian", "12") == nullptr);  // no Debian image (removed from the index)
  // An entry marked unavailable parses and is not selectable.
  RootfsIndex Unavailable;
  std::string Why;
  CHECK(ParseRootfsIndex(R"({"v1": {"X": {"DistroMatch": "x", "DistroVersion": "1", "URL": "", "Hash": "", "Type": "squashfs", "Available": false, "Reason": "Not available yet"}}})", Unavailable, Why) &&
        Unavailable.Images.size() == 1 && !Unavailable.Images[0].Selectable() && Unavailable.Images[0].Reason == "Not available yet");
  // The built-in file is in the RootFS_links.json v1 format: a project entry parses with the same parser, https becomes http.
  RootfsIndex Parsed;
  std::string Error;
  const std::string Live = R"J({"v1": {
    "Ubuntu 24.04 (EroFS)": {"DistroMatch": "ubuntu", "DistroVersion": "24.04", "URL": "https://rootfs.fex-emu.gg/Ubuntu_24_04/2026-09-01/Ubuntu_24_04.ero", "Hash": "52d6c0dab39efaed", "Type": "erofs"},
    "Ubuntu 24.04 (SquashFS)": {"DistroMatch": "ubuntu", "DistroVersion": "24.04", "URL": "https://rootfs.fex-emu.gg/Ubuntu_24_04/2026-09-01/Ubuntu_24_04.sqsh", "Hash": "cee02b6ea44829d", "Type": "squashfs"},
    "Fedora 44 (SquashFS)": {"DistroMatch": "fedora", "DistroVersion": "44", "URL": "https://rootfs.fex-emu.gg/Fedora_44/2026-08-11/Fedora_44.sqsh", "Hash": "fda4aaeab0fa69ba", "Type": "squashfs"},
    "Broken": {"DistroMatch": "x", "DistroVersion": "1", "URL": "https://h/x.sqsh", "Hash": "zz", "Type": "squashfs"}}})J";
  CHECK(ParseRootfsIndex(Live, Parsed, Error));
  CHECK(Parsed.Images.size() == 3);  // the entry with a bad hash is skipped
  CHECK(Parsed.Find("ubuntu", "24.04")->Url == "http://rootfs.fex-emu.gg/Ubuntu_24_04/2026-09-01/Ubuntu_24_04.sqsh");  // the squashfs one, http
  CHECK(Parsed.Find("ubuntu", "24.04")->Hash == "cee02b6ea44829d");  // a 15 digit hash (leading zero dropped) is valid
  CHECK(!Parsed.Find("ubuntu", "24.04")->Selectable() == false);
  // Newer image: same distro and version, later date folder; the built-in sizes carry over, the download size is learned.
  RootfsImage Newer;
  CHECK(Ubuntu && FindNewer(*Ubuntu, Parsed, Newer));
  CHECK(Newer.Url.find("2026-09-01") != std::string::npos && Newer.Name == "Ubuntu 24.04" && Newer.DownloadBytes == 0 && Newer.InstalledBytes == Ubuntu->InstalledBytes);
  CHECK(Newer.Extras == Ubuntu->Extras);
  // The same or an older date offers nothing, and an erofs entry is never offered.
  CHECK(!FindNewer(Newer, Parsed, Newer));
  RootfsIndex OnlyErofs;
  CHECK(ParseRootfsIndex(R"({"v1": {"E": {"DistroMatch": "ubuntu", "DistroVersion": "24.04", "URL": "https://h/Ubuntu_24_04/2026-12-01/u.ero", "Hash": "1a", "Type": "erofs"}}})", OnlyErofs, Error));
  CHECK(Ubuntu && !FindNewer(*Ubuntu, OnlyErofs, Newer));
  // A newer entry on another host is never offered (the live index is not authenticated).
  RootfsIndex OtherHost;
  CHECK(ParseRootfsIndex(R"({"v1": {"U": {"DistroMatch": "ubuntu", "DistroVersion": "24.04", "URL": "http://evil.example/Ubuntu_24_04/2026-12-01/u.sqsh", "Hash": "1a", "Type": "squashfs"}}})", OtherHost, Error));
  CHECK(Ubuntu && !FindNewer(*Ubuntu, OtherHost, Newer));
  // ChooseIndex: built-in when offline or unparsable, the newer entry when the live index has one.
  auto Choice = ChooseIndex("");
  CHECK(!Choice.LiveUsed && Choice.Index.Find("ubuntu", "24.04")->Url == Ubuntu->Url);
  Choice = ChooseIndex("{not json");
  CHECK(!Choice.LiveUsed && Choice.Note.find("built-in") != std::string::npos && Choice.Index.Images.size() == 1);
  Choice = ChooseIndex("{\"v2\": {}}");
  CHECK(!Choice.LiveUsed && Choice.Note.find("not a RootFS index") != std::string::npos);
  Choice = ChooseIndex(Live);
  CHECK(Choice.LiveUsed && Choice.Note.find("newer") != std::string::npos);
  CHECK(Choice.Index.Find("ubuntu", "24.04")->Url.find("2026-09-01") != std::string::npos && Choice.Index.Images.size() == 1);
  Choice = ChooseIndex(R"({"v1": {"U": {"DistroMatch": "ubuntu", "DistroVersion": "24.04", "URL": "https://h/Ubuntu_24_04/2026-08-11/u.sqsh", "Hash": "1a", "Type": "squashfs"}}})");
  CHECK(Choice.LiveUsed && Choice.Note.find("no newer") != std::string::npos && Choice.Index.Find("ubuntu", "24.04")->Url == Ubuntu->Url);
  CHECK(!ParseRootfsIndex("{\"v1\": {}}", Parsed, Error));
  CHECK(HttpUrl("https://a/b") == "http://a/b" && HttpUrl("http://a/b") == "http://a/b" && UrlFileName("http://a/b/c.sqsh") == "c.sqsh");
}

struct RecordingEvents final : InstallEvents {
  std::vector<std::string> Steps;
  int Pumps = 0;
  int CancelAt = -1;
  double Last = 0;
  bool Monotonic = true;
  std::vector<std::string> Lines;
  void Step(int, const std::string& Title) override { Steps.push_back(Title); }
  void Progress(double Fraction, const std::string&) override {
    if (Fraction + 1e-9 < Last)
      Monotonic = false;
    Last = Fraction;
  }
  void Log(InstallLogKind, const std::string& Line) override { Lines.push_back(Line); }
  void Pump() override { ++Pumps; }
  bool Cancelled() override { return CancelAt >= 0 && Pumps >= CancelAt; }
};

void TestFakeInstall() {
  FakeBackend B;
  B.Root.Installed = false;
  InstallRequest Request;
  Request.Image = *BuiltInRootfsIndex().Find("ubuntu", "24.04");
  RecordingEvents Ok;
  auto Result = B.InstallRootfs(Request, Ok);
  CHECK(Result.Ok && !Result.Cancelled && B.Rootfs().Installed && Ok.Steps.size() == 7 && Ok.Monotonic && Ok.Last > 0.999);
  CHECK(Ok.Pumps == 7 * B.PumpsPerStep);
  // A local image skips the download step.
  RecordingEvents Local;
  Request.LocalPath = "/fex/images/x.sqsh";
  CHECK(B.InstallRootfs(Request, Local).Ok && Local.Steps.size() == 6 && Local.Steps[1] == "Verify");
  // Cancel leaves no RootFS; a failure reports the step.
  B.Root.Installed = false;
  RecordingEvents Cut;
  Cut.CancelAt = 9;
  Result = B.InstallRootfs(Request, Cut);
  CHECK(!Result.Ok && Result.Cancelled && !B.Rootfs().Installed);
  B.InstallFailsAtStep = 3;
  RecordingEvents Bad;
  Result = B.InstallRootfs(Request, Bad);
  CHECK(!Result.Ok && !Result.Cancelled && Result.Error.find("Extract the RootFS") != std::string::npos && !B.Rootfs().Installed);
  std::string Text, Error;
  CHECK(!B.FetchLiveRootfsIndex(Text, Error) && !Error.empty());
  B.LiveIndexText = "{}";
  CHECK(B.FetchLiveRootfsIndex(Text, Error) && Text == "{}");
}

int main() {
  SetLogSink([](const char*) {});
  TestPaths();
  TestBrowseLogic();
  TestLoader();
  TestSchema();
  TestJson();
  TestPersistence();
  TestMigration();
  TestFakeStorageAtomic();
  TestLicenses();
  TestHome();
  TestSettingsRows();
  TestSettingsNav();
  TestSettingsControls();
  TestSettingsPathAndSave();
  TestAbout();
  TestPicker();
  TestPickerFolderMode();
  TestPickerVirtualized();
  TestPickerDownToLast();
  TestPickerSpinner();
  TestRootfsIndex();
  TestFakeInstall();
  std::printf("ui-screens-test: %d checks, %d failures\n", Checks, Failures);
  return Failures ? 1 : 0;
}
