// SPDX-License-Identifier: MIT
#include "screens/settings.h"
#include "components/Chrome.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "PathUtil.h"

namespace ui {

using namespace settings_layout;

namespace {
constexpr TextStyle AboutName {"about_name", FontId::NunitoExtraBold, 24};
constexpr TextStyle FxTile {"fx_tile", FontId::MonoSemiBold, 22};
const char* const FootText =
    "Full license texts are saved on the SD card in sdmc:/fex/licenses/. This software is based in part on the work of the FreeType Team. "
    "Fonts are licensed under the SIL Open Font License 1.1. FEX for Switch 2 is not affiliated with Nintendo, Valve, Canonical, KDE or Epic Games; "
    "all trademarks belong to their owners.";
} // namespace

SettingsScreen::SettingsScreen(LauncherBackend& B, SettingsModel& M, SettingsPaths P, const std::vector<LicenseEntry>* L)
    : Be(B), Model(M), Paths(std::move(P)), Licenses(L ? *L : EmbeddedLicenses()) {
  Sections = SectionNames(Model.Schema());
  BuildRows();
  BuildAbout();
  Layout();

  Restore.Text = "Restore defaults";
  Restore.Fixed = true;
  Restore.OnActivate = [this]() { OpenRestoreConfirm(); };

  Bar.Status = "Changes apply on next launch";
  Bar.SetHints({{Button::B, "Back"}, {Button::A, "OK"}});
  Bar.OnHint = [this](Button Btn) {
    UiEvent E;
    E.Kind = UiEvent::Type::Button;
    E.Btn = Btn;
    Interact.Dispatch(E, Now_);
  };

  PickerM.OnSelect = [this](int I) {
    if (PickerRow && PickerRow->Def && I >= 0 && I < int(PickerRow->Def->Options.size())) {
      Model.Set(PickerRow->Def->Key, PickerRow->Def->Options[size_t(I)].Id);
      RowsDirty = true;
      ApplyTheme();
    }
    Host.Close();
  };
  ConfirmM.OnConfirm = [this]() {
    const std::string What = PendingConfirm;
    Host.Close();
    if (What == "restore") {
      Model.RestoreDefaults();
      RowsDirty = true;
      ApplyTheme();
    } else if (What == "wipe") {
      // The Installation screen runs the wipe with its progress (step 0 counts the files, step 1 removes them).
      Request R;
      R.K = Request::Kind::StartInstall;
      R.Install.WipeOnly = true;
      if (OnRequest)
        OnRequest(R);
    }
  };
  LicM.OnReadText = [this]() {
    TextM.Title = LicM.Entry.Name;
    TextM.Subtitle = LicM.Entry.License + " \xC2\xB7 " + LicenseTextPath(LicM.Entry);
    TextM.Text = LicenseText(LicM.Entry);
    TextM.Open(Now_);
    Host.Open(TextM);
  };
  TextM.OnHint = [this](Button Btn) {
    if (Btn == Button::B)
      Host.Close();
    else
      TextM.Page(Btn == Button::ZR ? 1 : -1);
  };

  // One root with everything first so every widget has an id, then the explicit navigation overrides.
  std::vector<Widget*> All;
  for (auto& It : Items)
    All.push_back(It.get());
  All.push_back(&Restore);
  for (auto& R : Rows)
    All.push_back(R.Row.get());
  for (auto& L2 : LicRows)
    All.push_back(L2.get());
  All.push_back(&Bar);
  Interact.SetRoot(All);
  const int N = int(Sections.size());
  std::vector<int> FirstRow(size_t(N + 1), NoFocus);
  for (auto& R : Rows)
    if (FirstRow[size_t(R.Section)] == NoFocus && R.Row->Enabled)
      FirstRow[size_t(R.Section)] = R.Row->Id;
  for (auto& R : Rows)
    if (FirstRow[size_t(R.Section)] == NoFocus)
      FirstRow[size_t(R.Section)] = R.Row->Id;
  for (int I = 0; I < N; ++I)
    Items[size_t(I)]->Over[int(Dir::Right)].Id = FirstRow[size_t(I)];
  if (!LicRows.empty())
    Items[size_t(N)]->Over[int(Dir::Right)].Id = LicRows[0]->Id;
  for (auto& R : Rows) {
    R.Row->Over[int(Dir::Left)].Id = Items[size_t(R.Section)]->Id;
  }
  for (auto& L2 : LicRows)
    L2->Over[int(Dir::Left)].Id = Items[size_t(N)]->Id;
  // Header: Up from the first sidebar item or the first row reaches Restore defaults; Down from it returns.
  if (!Items.empty())
    Items[0]->Over[int(Dir::Up)].Id = Restore.Id;
  if (!Rows.empty())
    Rows[0].Row->Over[int(Dir::Up)].Id = Restore.Id;
  if (!LicRows.empty())
    LicRows[0]->Over[int(Dir::Up)].Id = Restore.Id;
  Restore.Over[int(Dir::Down)].Id = Rows.empty() ? Items[0]->Id : Rows[0].Row->Id;
  Restore.Over[int(Dir::Left)].Id = Items[0]->Id;

  for (int I = 0; I <= N; ++I)
    Items[size_t(I)]->OnActivate = [this, I]() {
      if (I < SectionCount())
        FocusFirstRowOf(I);
      else if (!LicRows.empty())
        Interact.SetFocus(LicRows[0]->Id, Now_);
    };

  Interact.Scroll = &Scroll;
  Interact.OnBack = [this]() { Leave(); };
  Interact.OnButton = [this](Button Btn) {
    if (Host.Top() == &TextM) {
      if (Btn == Button::ZL)
        TextM.Page(-1);
      else if (Btn == Button::ZR)
        TextM.Page(1);
      return;
    }
    if (Host.IsOpen())
      return;
    if (Btn == Button::L)
      SectionStep(-1, Now_);
    else if (Btn == Button::R)
      SectionStep(1, Now_);
    else if (Btn == Button::ZL)
      PageStep(-1, Now_);
    else if (Btn == Button::ZR)
      PageStep(1, Now_);
  };
  Interact.ScrollFilter = [this](float, float, float Dy) {
    if (Host.Top() == &TextM) {
      TextM.ScrollBy(Dy);
      return true;
    }
    return false;
  };

  Sel = 0;
  Items[0]->Selected = true;
  AboutPane = false;
  SetRoot();
  Scroll.SetViewport(ViewTop, ViewH, SettingsEnd - ViewTop);
  RefreshRows();
  Interact.SetFocus(Items[0]->Id, 0);
  PrevFocus = Items[0].get();
}

SettingsScreen::~SettingsScreen() = default;

// ---- Build --------------------------------------------------------------------------------------------------------------

void SettingsScreen::BuildRows() {
  const auto& Schema = Model.Schema();
  Rows.reserve(Schema.size());
  for (size_t Si = 0; Si < Sections.size(); ++Si) {
    Items.push_back(std::make_unique<SidebarItem>());
    Items.back()->Text = Sections[Si];
  }
  Items.push_back(std::make_unique<SidebarItem>());
  Items.back()->Text = "About";
  Items.back()->HasIcon = true;
  Items.back()->Icon = IconId::Info;
  Items.back()->IconStroke = 2.0f;

  for (const SettingDef& D : Schema) {
    if (!D.ShownWhenFile.empty() && !Be.GetStorage().Exists(D.ShownWhenFile))
      continue;
    RowInfo Info;
    Info.Def = &D;
    Info.Section = int(std::find(Sections.begin(), Sections.end(), D.Section) - Sections.begin());
    Info.Row = std::make_unique<SettingsRow>();
    Info.Row->Label = D.Label;
    Info.Row->Description = D.Description;
    const size_t Idx = Rows.size();
    std::unique_ptr<Control> Ctl;
    switch (D.Type) {
    case RowType::Toggle: {
      auto T = std::make_unique<Toggle>();
      T->OnChange = [this, Idx](bool On) {
        Model.Set(Rows[Idx].Def->Key, On ? "true" : "false");
        RowsDirty = true;
      };
      Ctl = std::move(T);
      break;
    }
    case RowType::Enum: {
      auto C = std::make_unique<EnumChip>();
      C->Wrap = false;  // Left on the first value leaves the row for the sidebar
      for (const EnumOption& O : D.Options)
        C->Values.push_back(O.Label);
      C->OnChange = [this, Idx](int V) {
        Model.Set(Rows[Idx].Def->Key, Rows[Idx].Def->Options[size_t(V)].Id);
        RowsDirty = true;
        ApplyTheme();
      };
      C->OnActivate = [this, Idx]() { OpenPickerFor(Rows[Idx]); };
      Ctl = std::move(C);
      break;
    }
    case RowType::Slider: {
      auto Sl = std::make_unique<Slider>();
      Sl->LeaveAtEdge = true;
      Sl->Min = D.Min;
      Sl->Max = D.Max;
      Sl->Step = D.Step;
      Sl->Format = [](float V) { char B2[24]; std::snprintf(B2, sizeof(B2), "%d", int(std::lround(V))); return std::string(B2); };
      Sl->OnChange = [this, Idx](float V) {
        Model.Set(Rows[Idx].Def->Key, std::to_string(int(std::lround(V))));
        RowsDirty = true;
      };
      Ctl = std::move(Sl);
      break;
    }
    case RowType::Path: {
      auto P = std::make_unique<PathField>();
      P->OnActivate = [this, Idx]() { RequestPath(Rows[Idx]); };
      Ctl = std::move(P);
      break;
    }
    case RowType::Link:
    case RowType::Action: {
      auto A = std::make_unique<ActionPill>();
      A->Text = D.ActionText;
      A->Destructive = D.Destructive;
      A->HasIcon = D.Destructive;
      A->Icon = IconId::Trash;
      A->OnActivate = [this, Idx]() { Press(Rows[Idx]); };
      Ctl = std::move(A);
      break;
    }
    }
    Info.Ctl = Ctl.get();
    Info.Row->SetControl(std::move(Ctl));
    Rows.push_back(std::move(Info));
  }
}

void SettingsScreen::BuildAbout() {
  for (const LicenseEntry& E : Licenses) {
    auto R = std::make_unique<LicenseRow>();
    R->Name = E.Name;
    R->Role = E.Role;
    R->License = E.License;
    const LicenseEntry* P = &E;
    R->OnActivate = [this, P]() {
      if (Host.IsOpen())
        return;
      LicM.Open(*P, Now_);
      Host.Open(LicM);
    };
    LicRows.push_back(std::move(R));
  }
}

void SettingsScreen::Layout() {
  const int N = int(Sections.size());
  for (int I = 0; I <= N; ++I) {
    SidebarItem& It = *Items[size_t(I)];
    It.Bounds.X = SidebarX;
    It.Bounds.W = SidebarItemW;
    It.Bounds.Y = I < N ? ViewTop + 24 + float(I) * (SidebarItemH + SidebarGap) : 632 - SidebarItemH;
  }
  SectionStart.assign(size_t(N), 0);
  SectionHeaderY.assign(size_t(N), 0);
  float Y = ViewTop + TopPad;
  for (int S = 0; S < N; ++S) {
    SectionStart[size_t(S)] = Y;
    SectionHeaderY[size_t(S)] = Y + SectionPadTop;
    float RowY = SectionHeaderY[size_t(S)] + SectionHeaderH + 6;
    for (auto& R : Rows)
      if (R.Section == S) {
        R.Row->Bounds = Rect {ContentX, RowY, ContentW, RowH};
        R.Row->RingClip = Rect {SidebarW + 1, ViewTop, 1280 - SidebarW - 1, ViewH};
        RowY += RowH;
      }
    Y = RowY;
  }
  SettingsEnd = Y + BottomPad;

  // About: app block, bundled list, runtime list, footnote.
  AboutBlockY = ViewTop + TopPad + 16;
  float Ay = AboutBlockY + 72 + 20 + 1 + SectionPadTop;
  FirstBundled = FirstRuntime = -1;
  for (int Pass = 0; Pass < 2; ++Pass) {
    AboutHeaderY[Pass] = Ay;
    Ay += SectionHeaderH;
    for (size_t I = 0; I < Licenses.size(); ++I)
      if (Licenses[I].Bundled == (Pass == 0)) {
        LicRows[I]->Bounds = Rect {ContentX, Ay, ContentW, 60};
        Ay += 60;
        if (Pass == 0 && FirstBundled < 0)
          FirstBundled = int(I);
        if (Pass == 1 && FirstRuntime < 0)
          FirstRuntime = int(I);
      }
    Ay += SectionPadTop;
  }
  AboutFootY = Ay;
  AboutEnd = AboutFootY + 3 * 20 + BottomPad;  // refined once fonts exist (UpdateAboutLayout)
}

// ---- Rows ---------------------------------------------------------------------------------------------------------------

void SettingsScreen::RefreshRows() {
  RowsDirty = false;
  for (RowInfo& R : Rows) {
    const SettingDef& D = *R.Def;
    bool On = Model.Enabled(D);
    std::string Reason = Model.Reason(D);
    if (D.Type == RowType::Action || D.Type == RowType::Link) {
      Capability Cap;
      if (D.ActionId == "reinstall_rootfs")
        Cap = Be.Reinstall();
      else if (D.ActionId == "wipe_rootfs")
        Cap = Be.Wipe();
      else if (D.ActionId != "open_about")
        Cap = Be.DevAction(D.ActionId);
      if (!Cap.Available) {
        On = false;
        Reason = Cap.Reason.empty() ? D.DisabledReason : Cap.Reason;
      }
    }
    R.Row->Enabled = On;
    R.Row->Description = On ? D.Description : Reason;
    switch (D.Type) {
    case RowType::Toggle: static_cast<Toggle*>(R.Ctl)->Set(Model.GetBool(D.Key), Now_, true); break;
    case RowType::Enum: static_cast<EnumChip*>(R.Ctl)->Index = Model.EnumIndex(D); break;
    case RowType::Slider: static_cast<Slider*>(R.Ctl)->SetValue(float(Model.GetInt(D.Key))); break;
    case RowType::Path: static_cast<PathField*>(R.Ctl)->Path = Model.Get(D.Key); break;
    default: break;
    }
  }
}

SettingsRow* SettingsScreen::RowFor(const std::string& Key) {
  for (RowInfo& R : Rows)
    if (R.Def->Key == Key)
      return R.Row.get();
  return nullptr;
}

void SettingsScreen::ApplyTheme() { SetTheme(ResolveThemeMode(Model, ThemeMode::Dark), ResolveAccent(Model)); }

void SettingsScreen::OpenPickerFor(RowInfo& R) {
  if (Host.IsOpen())
    return;
  PickerRow = &R;
  PickerM.Title = R.Def->Label;
  PickerM.Options.clear();
  for (const EnumOption& O : R.Def->Options)
    PickerM.Options.push_back(O.Label);
  PickerM.Selected = Model.EnumIndex(*R.Def);
  PickerM.Open(Now_);
  Host.Open(PickerM);
}

void SettingsScreen::RequestPath(RowInfo& R) {
  Request Rq;
  Rq.K = Request::Kind::OpenFilePicker;
  Rq.Picker.Title = R.Def->PickerTitle.empty() ? "Choose " + R.Def->Label : R.Def->PickerTitle;
  Rq.Picker.Accept = R.Def->Accept;
  Rq.Picker.Folder = R.Def->Folder;
  Rq.Picker.Current = Model.Get(R.Def->Key);
  Rq.Picker.FallbackLocation = R.Def->PickerStart;
  const std::string Key = R.Def->Key;
  Rq.OnPicked = [this, Key](const std::string& Path) {
    Model.Set(Key, Path);
    RowsDirty = true;
  };
  if (OnRequest)
    OnRequest(Rq);
}

void SettingsScreen::Press(RowInfo& R) {
  const std::string& Id = R.Def->ActionId;
  if (Id == "reinstall_rootfs") {
    Request Rq;
    Rq.K = Request::Kind::OpenRootfsSetup;
    Rq.Reinstall = true;
    if (OnRequest)
      OnRequest(Rq);
  } else if (Id == "wipe_rootfs") {
    if (Host.IsOpen())
      return;
    PendingConfirm = "wipe";
    ConfirmM.Title = "Wipe RootFS?";
    ConfirmM.Message = "This deletes " + Be.WipeSummary() + ". This can't be undone. The next launch starts first-run setup.";
    ConfirmM.ConfirmText = "Wipe RootFS";
    ConfirmM.Destructive = true;
    ConfirmM.Open(Now_);
    Host.Open(ConfirmM);
  } else if (Id == "open_about") {
    Send(Request::Kind::OpenAbout);
    SelectSection(SectionCount(), Now_);
  } else if (!R.Def->PickerTitle.empty()) {
    // A developer action on a chosen file: the picker comes first. The choice is sent from Update, once this screen is on top again.
    Request Rq;
    Rq.K = Request::Kind::OpenFilePicker;
    Rq.Picker.Title = R.Def->PickerTitle;
    Rq.Picker.Accept = R.Def->Accept;
    Rq.Picker.Current = "sdmc:/fex/games";  // test programs live there; a missing folder falls back to sdmc:/fex
    Rq.Picker.FallbackLocation = R.Def->PickerStart;
    const std::string Action = Id;
    Rq.OnPicked = [this, Action](const std::string& Path) { DevPending = {Action, Path}; };
    if (OnRequest)
      OnRequest(Rq);
  } else {
    Request Rq;
    Rq.K = Request::Kind::DevAction;
    Rq.Action = Id;
    if (OnRequest)
      OnRequest(Rq);
  }
}

void SettingsScreen::OpenRestoreConfirm() {
  if (Host.IsOpen())
    return;
  PendingConfirm = "restore";
  ConfirmM.Title = "Restore all settings?";
  ConfirmM.Message = "Every setting returns to its default. Your RootFS stays installed.";
  ConfirmM.ConfirmText = "Restore";
  ConfirmM.Destructive = false;
  ConfirmM.Open(Now_);
  Host.Open(ConfirmM);
}

// ---- Sections, scrolling ------------------------------------------------------------------------------------------------

float SettingsScreen::SectionOffset(int I) const {
  if (I < 0 || I >= int(SectionStart.size()))
    return 0;
  return SectionStart[size_t(I)] - ViewTop - TopPad;
}

float SettingsScreen::ContentHeight() const { return (AboutPane ? AboutEnd : SettingsEnd) - ViewTop; }

void SettingsScreen::SetRoot() {
  std::vector<Widget*> L;
  for (auto& It : Items)
    L.push_back(It.get());
  L.push_back(&Restore);
  if (AboutPane) {
    for (auto& R : LicRows)
      L.push_back(R.get());
  } else {
    for (auto& R : Rows)
      L.push_back(R.Row.get());
  }
  L.push_back(&Bar);
  Interact.SetRoot(L);
  for (auto& R : Rows)
    R.Row->Visible = !AboutPane;
  for (auto& R : LicRows)
    R->Visible = AboutPane;
}

void SettingsScreen::SwitchPane(bool About) {
  if (About == AboutPane)
    return;
  PaneOffset[AboutPane ? 1 : 0] = Scroll.Target();
  AboutPane = About;
  SetRoot();
  Scroll.SetViewport(ViewTop, ViewH, ContentHeight());
  Scroll.To(PaneOffset[About ? 1 : 0], Now_, 0);
  Holding = false;
}

void SettingsScreen::SelectSection(int I, uint64_t Now, bool Animate) {
  if (I < 0 || I > SectionCount())
    return;
  Now_ = Now;
  SwitchPane(I == SectionCount());
  Sel = I;
  for (int K = 0; K <= SectionCount(); ++K)
    Items[size_t(K)]->Selected = K == Sel;
  if (I < SectionCount() && Animate) {
    Scroll.To(std::clamp(SectionOffset(I), 0.0f, Scroll.MaxOffset()), Now, 200);
    Holding = true;
    HoldTarget = Scroll.Target();
  }
}

void SettingsScreen::UpdateSpy() {
  if (AboutPane)
    return;
  const float Off = Scroll.Target();
  if (Holding) {
    if (Off == HoldTarget)
      return;
    Holding = false;
  }
  int Idx = 0;
  for (int I = 0; I < SectionCount(); ++I)
    if (Off + 1.0f >= SectionOffset(I))
      Idx = I;
  if (Scroll.MaxOffset() > 0 && Off >= Scroll.MaxOffset() - 0.5f)
    Idx = SectionCount() - 1;
  if (Idx != Sel) {
    Sel = Idx;
    for (int K = 0; K <= SectionCount(); ++K)
      Items[size_t(K)]->Selected = K == Sel;
  }
}

void SettingsScreen::FocusFirstRowOf(int S) {
  RowInfo* First = nullptr;
  for (RowInfo& R : Rows)
    if (R.Section == S && (!First || (R.Row->Enabled && !First->Row->Enabled)))
      First = &R;
  if (First)
    Interact.SetFocus(First->Row->Id, Now_);
}

void SettingsScreen::SectionStep(int Dir, uint64_t Now) {
  const int To = std::clamp(Sel + Dir, 0, SectionCount());
  if (To == Sel)
    return;
  Widget* F = Interact.FocusedWidget();
  const bool OnSidebar = !F || std::any_of(Items.begin(), Items.end(), [F](const auto& I) { return I.get() == F; });
  SelectSection(To, Now);
  if (OnSidebar)
    Interact.SetFocus(Items[size_t(To)]->Id, Now);
  else if (To < SectionCount())
    FocusFirstRowOf(To);
  else if (!LicRows.empty())
    Interact.SetFocus(LicRows[0]->Id, Now);
}

void SettingsScreen::PageStep(int Dir, uint64_t Now) {
  const float Target = std::clamp(Scroll.Target() + float(Dir) * (ViewH - 48), 0.0f, Scroll.MaxOffset());
  Scroll.To(Target, Now, 200);
  Holding = false;
  Widget* F = Interact.FocusedWidget();
  const bool OnRow = F && !F->Fixed;
  if (!OnRow)
    return;
  std::vector<Widget*> Cands;
  if (AboutPane)
    for (auto& R : LicRows)
      Cands.push_back(R.get());
  else
    for (auto& R : Rows)
      if (R.Row->Enabled)
        Cands.push_back(R.Row.get());
  Widget* Pick = nullptr;
  for (Widget* W : Cands)
    if (W->Bounds.Y >= ViewTop + Target + 8) {
      Pick = W;
      break;
    }
  if (!Pick && !Cands.empty())
    Pick = Cands.back();
  if (Pick)
    Interact.SetFocus(Pick->Id, Now);
}

// ---- Persistence --------------------------------------------------------------------------------------------------------

bool SettingsScreen::Commit() {
  std::string Err;
  if (Model.Save(Be.GetStorage(), Paths, Err)) {
    Bar.Status = "Changes apply on next launch";
    SaveFailedOnce = false;
    return true;
  }
  Bar.Status = "Could not save settings: " + Err;
  return false;
}

void SettingsScreen::Leave() {
  if (!Commit() && !SaveFailedOnce) {
    SaveFailedOnce = true;  // stay once so the error is read; B again leaves without saving
    return;
  }
  Send(Request::Kind::Back);
}

// ---- Update / Draw ------------------------------------------------------------------------------------------------------

void SettingsScreen::Update(uint64_t Now, float Scale, const std::vector<Event>& Raw) {
  Now_ = Now;
  ++Frames;
  Host.Tick(Now);
  if (!DevPending.first.empty()) {
    Request Rq;
    Rq.K = Request::Kind::DevAction;
    Rq.Action = DevPending.first;
    Rq.Path = DevPending.second;
    DevPending = {};
    if (OnRequest)
      OnRequest(Rq);
  }
  if (RowsDirty)
    RefreshRows();
  Interact.Update(Now, Scale, Raw);
  if (!Host.IsOpen()) {
    // The selected section follows the sidebar focus (controller, hover, tap) when the focus moves onto another item.
    Widget* F = Interact.FocusedWidget();
    if (F != PrevFocus)
      for (int I = 0; I <= SectionCount(); ++I)
        if (Items[size_t(I)].get() == F && I != Sel)
          SelectSection(I, Now);
    PrevFocus = F;
  }
  UpdateSpy();
  if (RowsDirty)
    RefreshRows();
}

void SettingsScreen::UpdateAboutLayout(const Frame& F) {
  if (FootScale == F.S() && !FootLines.empty())
    return;
  FootScale = F.S();
  FootLines = WrapLines(F, styles::Meta, FootText, ContentW);
  const float LineH = F.Dc.F.LineHeightPx(styles::Meta, F.S()) / F.S();
  AboutEnd = AboutFootY + float(FootLines.size()) * (LineH + 2) + BottomPad;
  if (AboutPane)
    Scroll.SetViewport(ViewTop, ViewH, ContentHeight());
}

void SettingsScreen::DrawAbout(const Frame& Fc) {
  const Theme& T = Fc.T();
  const float S = Fc.S();
  const int Hair = HairlinePx(S);
  // App block: tile 72 r16 with "FX", name 24/800, version, one line 14 muted.
  Fc.Dc.R.FillRoundRect(Fc.Box(ContentX, AboutBlockY, 72, 72), 16 * S, T.SurfaceRaised);
  Fc.Text(FxTile, "FX", ContentX + 36, AboutBlockY, 72, T.Accent, Align::Center);
  const float H1 = Fc.Dc.F.LineHeightPx(AboutName, S) / S, H2 = Fc.Dc.F.LineHeightPx(styles::MonoUi, S) / S, H3 = Fc.Dc.F.LineHeightPx(cstyles::Meta14, S) / S;
  const float Top = AboutBlockY + (72 - (H1 + 4 + H2 + 4 + H3)) / 2;
  const float Tx = ContentX + 72 + 20;
  Fc.Text(AboutName, "FEX for Switch 2", Tx, Top, H1, T.Text);
  Fc.Text(styles::MonoUi, Be.VersionText(), Tx, Top + H1 + 4, H2, T.Text);
  Fc.Text(cstyles::Meta14, "Homebrew launcher for FEX-Emu. Not affiliated with Nintendo, Valve, Canonical or KDE.", Tx, Top + H1 + 4 + H2 + 4, H3, T.TextMuted);
  const float RuleY = std::round((AboutBlockY + 72 + 20 - Fc.ScrollY) * S);
  Fc.Dc.R.Line(std::round((SidebarW + 1) * S), RuleY, std::round(1280 * S), RuleY, float(Hair), T.Divider);
  Fc.Text(styles::Section, "Bundled open-source components", ContentX, AboutHeaderY[0], 20, T.TextMuted);
  Fc.Text(styles::Section, "Downloaded at runtime \xC2\xB7 not bundled", ContentX, AboutHeaderY[1], 20, T.TextMuted);
  for (auto& R : LicRows)
    if (R->Bounds.Y + R->Bounds.H - Fc.ScrollY > ViewTop && R->Bounds.Y - Fc.ScrollY < ViewTop + ViewH)
      R->Draw(Fc);
  const float LineH = Fc.Dc.F.LineHeightPx(styles::Meta, S) / S;
  for (size_t I = 0; I < FootLines.size(); ++I)
    Fc.Text(styles::Meta, FootLines[I], ContentX, AboutFootY + float(I) * (LineH + 2), LineH, T.TextMuted);
}

void SettingsScreen::Draw(const DrawContext& C, uint64_t Now) {
  Now_ = Now;
  const Theme& T = C.T;
  const float S = C.S;
  const int Hair = HairlinePx(S);
  const Frame F {C, Now, 0};
  const Frame Fc {C, Now, Interact.ScrollOffset(Now)};
  UpdateAboutLayout(F);

  // Content (clipped to the viewport).
  C.R.PushClip(C.Du(SidebarW + 1, ViewTop, 1280 - SidebarW - 1, ViewH));
  if (AboutPane) {
    DrawAbout(Fc);
  } else {
    for (size_t Si = 0; Si < Sections.size(); ++Si)
      Fc.Text(styles::Section, Sections[Si], ContentX, SectionHeaderY[Si], 20, T.TextMuted);
    for (RowInfo& R : Rows) {
      const float Y = R.Row->Bounds.Y - Fc.ScrollY;
      if (Y + R.Row->Bounds.H > ViewTop && Y < ViewTop + ViewH)
        R.Row->Draw(Fc);
    }
  }
  C.R.PopClip();

  // Header: gear icon 32 + 14 gap + "Settings" 28/800, Restore defaults at the right, rule at the bottom.
  DrawScreenHeader(F, IconId::Settings, "Settings");
  Restore.Layout(F);
  Restore.Bounds.X = chrome::HeaderRight - Restore.Bounds.W;
  Restore.Bounds.Y = chrome::HeaderCentre(44);
  Restore.Draw(F);
  // Sidebar and the 1 du rule between it and the content.
  C.R.Line(std::round(SidebarW * S), std::round(HeaderH * S), std::round(SidebarW * S), std::round(656 * S), float(Hair), T.Divider);
  for (auto& It : Items)
    It->Draw(F);
  Bar.Draw(F);
  Host.Draw(F, {&PickerM, &ConfirmM, &LicM, &TextM});
  Interact.DrawOverlay(F);
}

} // namespace ui
