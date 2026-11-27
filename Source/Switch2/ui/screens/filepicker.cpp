// SPDX-License-Identifier: MIT
#include "screens/filepicker.h"
#include "components/Chrome.h"

#include <algorithm>
#include <cmath>

namespace ui {

using namespace picker_layout;

namespace {
IconId LocationIcon(const std::string& Id) {
  if (Id == "sd") return IconId::SdCard;
  if (Id == "fex") return IconId::Folder;
  if (Id == "rootfs") return IconId::Package;
  return IconId::House;
}
} // namespace

FilePickerScreen::FilePickerScreen(LauncherBackend& B, PickerSpec Spec, std::function<void(const std::string&)> OnPicked)
    : Be(B), Pk(std::move(Spec)), Result(std::move(OnPicked)) {
  Locs = DefaultLocations(Be.HomeFolder());
  Opt.Accept = Pk.Accept;
  Opt.FolderMode = Pk.Folder;
  for (const Location& L : Locs) {
    auto It = std::make_unique<SidebarItem>();
    It->Text = L.Name;
    It->HasIcon = true;
    It->Icon = LocationIcon(L.Id);
    LocItems.push_back(std::move(It));
  }
  // Group labels (13/700 uppercase): Console at y 96, Linux below; the items start 6 below their label.
  LocItems[0]->Bounds = Rect {32, 120, 252, 52};
  LocItems[1]->Bounds = Rect {32, 180, 252, 52};
  LocItems[2]->Bounds = Rect {32, 272, 252, 52};
  LocItems[3]->Bounds = Rect {32, 332, 252, 52};
  for (int K = 0; K < 8; ++K)
    Crumbs.push_back(std::make_unique<Crumb>());
  for (int K = 0; K < PoolSize; ++K) {
    auto R = std::make_unique<FileRow>();
    R->RingClip = Rect {SidebarW + 1, ListTop - 8, 1280 - SidebarW - 1, ListBottom - (ListTop - 8)};  // never over the footer
    FileRow* Raw = R.get();
    R->OnNavigate = [this](Dir D, uint64_t Now) {
      if (D == Dir::Up)
        return MoveCursor(-1, Now, true);
      if (D == Dir::Down)
        return MoveCursor(1, Now, true);
      return false;
    };
    R->OnActivate = [this, Raw]() {
      if (Raw->Bound)
        ActivateEntry(Raw->Index, Now_);
    };
    Pool.push_back(std::move(R));
  }
  UpBtn.OnActivate = [this]() { GoParent(Now_); };
  Bar.OnHint = [this](Button Btn) {
    UiEvent E;
    E.Kind = UiEvent::Type::Button;
    E.Btn = Btn;
    Interact.Dispatch(E, Now_);
  };

  std::vector<Widget*> Root;
  for (auto& L : LocItems)
    Root.push_back(L.get());
  Root.push_back(&UpBtn);
  for (auto& C : Crumbs)
    Root.push_back(C.get());
  for (auto& R : Pool)
    Root.push_back(R.get());
  Root.push_back(&Bar);
  Interact.SetRoot(Root);
  for (int K = 0; K < int(Locs.size()); ++K) {
    LocItems[size_t(K)]->OnActivate = [this, K]() { GoLocation(K, Now_); };
  }
  Interact.Scroll = &Scroll;
  // B goes up one folder (at a location root it closes the picker like X), X cancels.
  Interact.OnBack = [this]() {
    if (IsRootPath(Path))
      Finish({});
    else
      GoParent(Now_);
  };
  Interact.OnButton = [this](Button Btn) {
    switch (Btn) {
    case Button::X: Finish({}); break;
    case Button::L: GoLocation(std::max(0, Loc - 1), Now_); break;
    case Button::R: GoLocation(std::min(int(Locs.size()) - 1, Loc + 1), Now_); break;
    case Button::ZL: Page(-1, Now_); break;
    case Button::ZR: Page(1, Now_); break;
    case Button::Plus:
      if (Pk.Folder)
        Finish(Path);
      break;
    default: break;
    }
  };

  Path = StartFolder(Be.GetStorage(), Locs, Pk.Current, Pk.Folder, Pk.FallbackLocation);
  // The current value (a file in that folder, or the subfolder it names) starts selected.
  Open(Path, 0, IsValidPath(Pk.Current) && ParentPath(NormalizePath(Pk.Current)) == Path ? BaseName(Pk.Current) : std::string());
  UpdateHints();
  Interact.SetFocus(LocItems[size_t(Loc)]->Id, 0);
}

FilePickerScreen::~FilePickerScreen() = default;

int FilePickerScreen::VisibleRows() const {
  int N = 0;
  for (const auto& R : Pool)
    if (R->Bound)
      ++N;
  return N;
}

std::string FilePickerScreen::Subtitle() const {
  if (Pk.Folder)
    return "Choose a folder";
  return {};
}

void FilePickerScreen::Open(const std::string& Folder, uint64_t Now, const std::string& PreferName) {
  Path = NormalizePath(Folder);
  Prefer = PreferName;
  Loc = LocationOf(Locs, Path);
  {
    Widget* F = Interact.FocusedWidget();
    ReturnToList = std::any_of(Pool.begin(), Pool.end(), [F](const auto& R) { return R.get() == F; });
  }
  Loader.Begin(Be.GetStorage(), Path, Opt, Now);
  Loaded = false;
  // Unbind every row: a folder that loads within one frame must not keep the previous folder's rows (same index, stale name
  // and enabled state).
  for (auto& R : Pool) {
    R->Bound = false;
    R->Index = -1;
  }
  Cursor = 0;
  Scroll.SetViewport(ListTop, ListBottom - ListTop, 0);
  Scroll.To(0, Now, 0);
  for (int K = 0; K < int(LocItems.size()); ++K)
    LocItems[size_t(K)]->Selected = K == Loc;
  Rebuild();
}

void FilePickerScreen::GoLocation(int I, uint64_t Now) {
  if (I < 0 || I >= int(Locs.size()))
    return;
  Open(Locs[size_t(I)].Root, Now);
}

void FilePickerScreen::GoParent(uint64_t Now) {
  if (IsRootPath(Path))
    return;
  const std::string Name = BaseName(Path);
  Open(ParentPath(Path), Now, Name);
}

void FilePickerScreen::Finish(const std::string& Chosen) {
  if (!Chosen.empty() && Result)
    Result(Chosen);
  Send(Request::Kind::Back);
}

bool FilePickerScreen::Selectable(int I) const {
  const auto& E = Loader.Entries();
  return I >= 0 && I < int(E.size()) && E[size_t(I)].Selectable;
}

void FilePickerScreen::ActivateEntry(int Index, uint64_t Now) {
  const auto& E = Loader.Entries();
  if (Index < 0 || Index >= int(E.size()) || !E[size_t(Index)].Selectable)
    return;
  const BrowseEntry& B = E[size_t(Index)];
  const std::string Full = NormalizePath(JoinPath(Path, B.Name));
  if (B.IsDir)
    Open(Full, Now);
  else
    Finish(Full);
}

bool FilePickerScreen::MoveCursor(int Delta, uint64_t Now, bool Wrap) {
  const int N = int(Loader.Entries().size());
  int I = Cursor;
  do {
    I += Delta;
  } while (I >= 0 && I < N && !Selectable(I));
  if ((I < 0 || I >= N) && Wrap && N > 0) {  // Up on the top entry goes to the bottom one, Down on the bottom entry to the top one
    I = Delta > 0 ? -1 : N;
    do {
      I += Delta;
    } while (I >= 0 && I < N && !Selectable(I));
    if (I < 0 || I >= N || I == Cursor)
      return false;
    Scroll.To(Delta > 0 ? 0.0f : 1e9f, Now, 0);  // a jump across the list: the rows must be bound under the cursor at once
  }
  if (I < 0 || I >= N)
    return false;
  Cursor = I;
  CursorLed = true;
  Scroll.EnsureVisible(Rect {ListX, ListTop + float(Cursor) * RowStep, ListW, RowH}, Now);
  Relayout(Now);
  UpdateHints();
  return true;
}

void FilePickerScreen::Page(int Dir, uint64_t Now) {
  const int N = int(Loader.Entries().size());
  if (N == 0)
    return;
  const int Step = std::max(1, int((ListBottom - ListTop) / RowStep) - 1);
  int I = std::clamp(Cursor + Dir * Step, 0, N - 1);
  while (I >= 0 && I < N && !Selectable(I))
    I += Dir > 0 ? -1 : 1;  // settle on the nearest selectable entry toward where we came from
  if (I < 0 || I >= N)
    return;
  Cursor = I;
  CursorLed = true;
  Scroll.EnsureVisible(Rect {ListX, ListTop + float(Cursor) * RowStep, ListW, RowH}, Now);
  Relayout(Now);
  UpdateHints();
}

FileRow* FilePickerScreen::SlotOf(int Index) {
  for (auto& R : Pool)
    if (R->Bound && R->Index == Index)
      return R.get();
  return nullptr;
}

void FilePickerScreen::Relayout(uint64_t Now) {
  const auto& E = Loader.Entries();
  const bool Show = Loader.Done() && !E.empty();
  const float Off = Scroll.Offset(Now);
  const int First = std::max(0, int(std::floor(Off / RowStep)));
  // Entry Idx always uses slot Idx % PoolSize: while an entry stays on screen it keeps its widget (and the focus), even
  // while the list scrolls under it.
  for (int Idx = First; Idx < First + PoolSize; ++Idx) {
    FileRow& R = *Pool[size_t(Idx % PoolSize)];
    if (!Show || Idx >= int(E.size())) {
      R.Bound = false;
      R.Visible = false;
      R.Index = -1;
      continue;
    }
    R.Visible = true;
    if (R.Index != Idx || !R.Bound) {
      const BrowseEntry& B = E[size_t(Idx)];
      R.Index = Idx;
      R.Bound = true;
      R.Name = B.Name;
      R.IsDir = B.IsDir;
      R.SizeText = B.IsDir ? std::string() : FormatBytes(B.Size);
      R.DateText = FormatDate(B.Mtime);
      R.Enabled = B.Selectable;
    }
    R.Bounds = Rect {ListX, ListTop + float(Idx) * RowStep - Off, ListW, RowH};
  }
  // Explicit moves out of and into the list: Left -> the current location; the sidebar, up button and crumbs go Right/Down
  // to the cursor row.
  FileRow* Cur = SlotOf(Cursor);
  const int Target = Cur ? Cur->Id : NoFocus;
  for (auto& R : Pool)
    R->Over[int(Dir::Left)].Id = LocItems[size_t(Loc)]->Id;
  for (auto& L : LocItems)
    L->Over[int(Dir::Right)].Id = Target;
  UpBtn.Over[int(Dir::Down)].Id = Target;
  for (auto& C : Crumbs)
    C->Over[int(Dir::Down)].Id = Target;
  UpBtn.Over[int(Dir::Left)].Id = LocItems[size_t(Loc)]->Id;
  UpBtn.Enabled = !IsRootPath(Path);
}

void FilePickerScreen::SyncFocus(uint64_t Now) {
  if (!Loader.Done())
    return;
  Widget* F = Interact.FocusedWidget();
  FileRow* FocusedSlot = nullptr;
  for (auto& R : Pool)
    if (R.get() == F)
      FocusedSlot = R.get();
  FileRow* Cur = SlotOf(Cursor);
  if (CursorLed && Cur) {
    Interact.SetFocus(Cur->Id, Now);  // keyboard led: the focus follows the cursor row
  } else if (FocusedSlot && FocusedSlot->Bound && FocusedSlot->Index != Cursor) {
    Cursor = FocusedSlot->Index;  // pointer or touch led (hover, tap) or the cursor scrolled away
    UpdateHints();
  }
  if (FocusedSlot && FocusedSlot->Bound && FocusedSlot->Index == Cursor)
    CursorLed = false;
  if (CursorLed && !Cur)
    CursorLed = false;
}

void FilePickerScreen::Rebuild() {
  const std::vector<PathCrumb> Cs = Breadcrumbs(Path);
  CrumbPaths.clear();
  // Fit the crumbs between the up button and the right edge; drop the first ones (a leading "..." opens the first dropped folder's parent).
  CrumbsShown = 0;
  std::vector<std::pair<std::string, std::string>> Items;
  for (const PathCrumb& C : Cs)
    Items.push_back({C.Label, C.Path});
  size_t Start = 0;
  const float Avail = ListX + ListW - (ListX + 40 + 12);
  auto Width = [&](size_t From) {
    float W = From > 0 ? 44.0f + 24 : 0.0f;
    for (size_t I = From; I < Items.size(); ++I)
      W += float(Items[I].first.size()) * 9.5f + 16 + (I + 1 < Items.size() ? 24 : 0);
    return W;
  };
  while (Start + 1 < Items.size() && Width(Start) > Avail)
    ++Start;
  std::vector<std::pair<std::string, std::string>> Shown;
  if (Start > 0)
    Shown.push_back({"\xE2\x80\xA6", Items[Start - 1].second});
  for (size_t I = Start; I < Items.size(); ++I)
    Shown.push_back(Items[I]);
  for (size_t K = 0; K < Crumbs.size(); ++K) {
    Crumb& C = *Crumbs[K];
    if (K < Shown.size()) {
      C.Text = Shown[K].first;
      C.Current = K + 1 == Shown.size();
      C.Visible = true;
      CrumbPaths.push_back(Shown[K].second);
      const std::string Target = Shown[K].second;
      C.OnActivate = [this, Target]() {
        if (NormalizePath(Target) != Path)
          Open(Target, Now_);
      };
    } else {
      C.Visible = false;
      CrumbPaths.emplace_back();
    }
  }
  CrumbsShown = int(std::min(Shown.size(), Crumbs.size()));
  UpdateHints();
}

std::string FilePickerScreen::FocusedPath() const {
  const auto& E = Loader.Entries();
  if (Loader.Done() && Cursor >= 0 && Cursor < int(E.size())) {
    const std::string P = JoinPath(Path, E[size_t(Cursor)].Name);
    return E[size_t(Cursor)].IsDir ? P + "/" : P;
  }
  return Path;
}

void FilePickerScreen::UpdateHints() {
  const auto& E = Loader.Entries();
  const bool OnFile = Loader.Done() && Cursor >= 0 && Cursor < int(E.size()) && !E[size_t(Cursor)].IsDir;
  std::vector<Hint> H = {{Button::X, "Cancel"}, {Button::B, IsRootPath(Path) ? "Close" : "Parent folder"}, {Button::A, OnFile ? "Select" : "Open"}};
  if (Pk.Folder)
    H.push_back({Button::Plus, "Use this folder"});
  const bool AtRoot = IsRootPath(Path);
  if (Bar.HintCount() != H.size() || HintsOnFile != OnFile || HintsAtRoot != AtRoot) {
    Bar.SetHints(H);
    HintsOnFile = OnFile;
    HintsAtRoot = AtRoot;
  }
  Bar.Segs = {{HintBar::Seg::Kind::Path, FocusedPath()}};
}

void FilePickerScreen::Update(uint64_t Now, float Scale, const std::vector<Event>& Raw) {
  Now_ = Now;
  if (!ClockSet) {
    ClockSet = true;
    Loader.Rebase(Now);
  }
  Relayout(Now);
  Interact.Update(Now, Scale, Raw);
  Loader.Step(256);  // up to 256 entries per frame: thousands of entries load over several frames
  // Loaded (not "was not done before this frame"): Open() may run inside Interact.Update and finish in the same frame.
  if (!Loaded && Loader.Done()) {
    Loaded = true;
    const int N = int(Loader.Entries().size());
    Cursor = 0;
    if (!Prefer.empty())
      for (int I = 0; I < N; ++I)
        if (Loader.Entries()[size_t(I)].Name == Prefer && Selectable(I))
          Cursor = I;
    if (!Selectable(Cursor)) {
      Cursor = -1;
      MoveCursor(1, Now);
      if (Cursor < 0)
        Cursor = 0;
    }
    Scroll.SetViewport(ListTop, ListBottom - ListTop, float(N) * RowStep + 8);
    Scroll.To(0, Now, 0);
    Scroll.EnsureVisible(Rect {ListX, ListTop + float(Cursor) * RowStep, ListW, RowH}, Now);
    CursorLed = ReturnToList;  // a folder opened from the list lands the focus on the list; from the sidebar or a crumb it stays
    ReturnToList = false;
    UpdateHints();
  }
  Relayout(Now);
  SyncFocus(Now);
}

void FilePickerScreen::Draw(const DrawContext& C, uint64_t Now) {
  Now_ = Now;
  const Theme& T = C.T;
  const float S = C.S;
  const int Hair = HairlinePx(S);
  const Frame F {C, Now, 0};
  Relayout(Now);

  // Header: folder icon 32 + title 28/800 over the subtitle; free space at the right.
  const HeaderMetrics Hm = DrawScreenHeader(F, IconId::Folder, Pk.Title, {}, true);
  {
    const float Sy = Hm.SubY, Sh = Hm.SubH;
    float X = Hm.SubX;
    if (Pk.Folder) {
      F.Text(cstyles::Meta14, "Choose a folder", X, Sy, Sh, T.TextMuted);
    } else if (Pk.Accept.empty()) {
      F.Text(cstyles::Meta14, "Showing all files", X, Sy, Sh, T.TextMuted);
    } else {
      std::string Types;
      for (size_t I = 0; I < Pk.Accept.size(); ++I)
        Types += (I ? (I + 1 == Pk.Accept.size() ? " and " : ", ") : "") + Pk.Accept[I];
      X += F.Text(cstyles::Meta14, "Showing ", X, Sy, Sh, T.TextMuted) / S;
      X += F.Text(styles::MonoUiSmall, Types, X, Sy, Sh, T.TextField) / S;
      F.Text(cstyles::Meta14, " files", X, Sy, Sh, T.TextMuted);
    }
  }
  {
    uint64_t Free = 0, Total = 0;
    if (Be.GetStorage().FreeSpace(Path, Free, Total)) {
      static constexpr TextStyle FreeStyle {"free_15", FontId::NunitoSemiBold, 15};
      F.Text(FreeStyle, FormatBytes(Free) + (IsSdPath(Path) ? " free on microSD" : " free in RootFS"), chrome::HeaderRight, 0, HeaderH, T.TextMuted, Align::Right);
    }
  }
  C.R.Line(std::round(SidebarW * S), std::round(HeaderH * S), std::round(SidebarW * S), std::round(656 * S), float(Hair), T.Divider);

  // Sidebar: group labels and the locations.
  F.Text(cstyles::Group13, "Console", 32 + 18, 96, 18, T.TextMuted);
  F.Text(cstyles::Group13, "Linux", 32 + 18, 248, 18, T.TextMuted);
  for (auto& L : LocItems)
    L->Draw(F);

  // Path bar: up button, breadcrumbs separated by chevrons.
  UpBtn.Bounds.X = ListX;
  UpBtn.Bounds.Y = PathBarY;
  UpBtn.Draw(F);
  float X = ListX + 40 + 12;
  for (int K = 0; K < CrumbsShown; ++K) {
    Crumb& Cr = *Crumbs[size_t(K)];
    Cr.Bounds.X = X;
    Cr.Bounds.Y = PathBarY;
    Cr.Bounds.W = Cr.PreferredWidth(F);
    Cr.Draw(F);
    X += Cr.Bounds.W;
    if (K + 1 < CrumbsShown) {
      C.I.Draw(IconId::ChevronRight, C.Du(X + 3, PathBarY + 13, 14, 14), T.TextMuted, stroke::Chevron);
      X += 20;
    }
  }

  // List.
  const float ListH = ListBottom - ListTop;
  C.R.PushClip(C.Du(SidebarW + 1, ListTop, 1280 - SidebarW - 1, ListH));
  if (!Loader.Done()) {
    if (Loader.SpinnerVisible(Now))
      DrawSpinner(F, ListX + ListW / 2, ListTop + ListH / 2 - 20, 18, Now);
  } else if (Loader.Entries().empty()) {
    F.Text(styles::Body, Loader.Failed() ? "This folder could not be opened" : "This folder is empty", ListX + ListW / 2, ListTop + ListH / 2 - 20, 24, T.TextMuted, Align::Center);
  } else {
    for (auto& R : Pool)
      if (R->Bound)
        R->Draw(F);
  }
  C.R.PopClip();
  Bar.Draw(F);
  Interact.DrawOverlay(F);
}

} // namespace ui
