// SPDX-License-Identifier: MIT
// Widgets of Home, Settings, About and the file picker: SidebarItem, ActionPill (a row control), LicenseRow, FtpPill, FileRow,
// Crumb, UpButton and the loading spinner. They use Widget/Frame like the shared components and read theme tokens only.
#pragma once

#include "components/Controls.h"

namespace ui {

// 52 high sidebar entry (Settings sections, About, picker locations): optional icon 20, label row_label, selected = `surface`
// bg + accent label. The FocusRing is outset 4, stroke 3.
class SidebarItem final : public Widget {
public:
  SidebarItem() { Fixed = true; Bounds.H = 52; }
  std::string Text;
  bool HasIcon = false;
  IconId Icon = IconId::Info;
  float IconStroke = stroke::Default;
  bool Selected = false;
  void Draw(const Frame& F) override;
  float RingOutset() const override { return 4; }
  float RingStroke() const override { return 3; }
  float RingRadius() const override { return 11; }  // 12 - 1: the spec's ring radius 15 for sidebar items
};

// A pill inside a SettingsRow ("Reinstall >" or a trash icon + "Wipe" in the error color). A on the row presses it.
class ActionPill final : public Control {
public:
  ActionPill() { Bounds.H = 44; }
  std::string Text;
  bool HasIcon = false;
  IconId Icon = IconId::Trash;
  bool Destructive = false;
  float PreferredWidth(const Frame& F) override;
  void Draw(const Frame& F) override;
};

// A row of the About lists (min height 60, bottom rule): name 17/700 + role 14 muted at the left; license chip + chevron at the right.
class LicenseRow final : public Widget {
public:
  LicenseRow() { Bounds.H = 60; }
  std::string Name, Role, License;
  void Draw(const Frame& F) override;
  float RingOutset() const override { return 14; }
  float RingOutsetY() const override { return -4; }
  float RingStroke() const override { return 3; }
  float RingRadius() const override { return 12; }
};

// The FTP status pill of Home: 52 high, transfer icon, "FTP server", a rule, a status dot and the status text.
class FtpPill final : public Widget {
public:
  FtpPill() { Bounds.H = 52; }
  bool Running = false;
  int Clients = 0;
  std::string StatusText() const;
  float PreferredWidth(const Frame& F) const;
  void Draw(const Frame& F) override;
  float RingOutset() const override { return 10; }
  float RingStroke() const override { return 4; }
  float RingRadius() const override { return 26; }
};

// One row of the virtualized file list (min 56, r12): icon, name, size, date. Bound to an entry by the picker.
class FileRow final : public Widget {
public:
  FileRow() { Fixed = true; Bounds.H = 56; }
  std::string Name, SizeText, DateText;
  bool IsDir = false;
  bool Bound = false;  // shows an entry
  int Index = -1;      // the entry it shows (set by the picker)
  std::function<bool(Dir, uint64_t)> OnNavigate;  // Up/Down inside the list; true = the picker moved its cursor
  bool OnNav(Dir D, uint64_t Now) override { return OnNavigate && OnNavigate(D, Now); }
  void Draw(const Frame& F) override;
  float RingOutset() const override { return 4; }
  float RingStroke() const override { return 3; }
  float RingRadius() const override { return 11; }
};

// A breadcrumb segment (16/600; earlier segments muted, the current folder in `text`). Focusable; opens that folder.
class Crumb final : public Widget {
public:
  Crumb() { Fixed = true; Bounds.H = 40; }
  std::string Text;
  bool Current = false;
  float PreferredWidth(const Frame& F) const { return std::max(44.0f, F.Measure(cstyles::Crumb, Text) + 16); }
  void Draw(const Frame& F) override;
  float RingOutset() const override { return 4; }
  float RingStroke() const override { return 3; }
  float RingRadius() const override { return 8; }
};

// The round 40 "parent folder" button (arrow-up 18).
class UpButton final : public Widget {
public:
  UpButton() { Fixed = true; Bounds.W = Bounds.H = 40; }
  void Draw(const Frame& F) override;
  float RingOutset() const override { return 4; }
  float RingStroke() const override { return 3; }
  float RingRadius() const override { return 20; }
};

// Eight dots chasing each other around a circle, centred at (Cx, Cy) du, one turn per 900 ms.
void DrawSpinner(const Frame& F, float Cx, float Cy, float Radius, uint64_t Now);

} // namespace ui
