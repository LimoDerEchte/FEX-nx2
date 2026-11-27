// SPDX-License-Identifier: MIT
// Widgets of the FTP and RootFS setup screens: CredentialTile (FTP, 84 high), ClientRow (FTP clients list), OptionCard (RootFS
// setup cards with a radio or checkbox badge) and the section header drawing helper.
#pragma once

#include <memory>

#include "components/Controls.h"

namespace ui {

// The eye button inside the Password tile: 22 du icon, hit area 44x44, no background or outline.
class EyeButton final : public Widget {
public:
  EyeButton() { Focusable = false; Bounds.W = Bounds.H = 44; }
  bool Shown = false;  // true: the password is visible (crossed eye)
  void Draw(const Frame& F) override;
};

// 84 high, r16, `surface`, padding 14 18: label 14/600 muted at the top, value (mono_ui 18/600, one line, ellipsis) at the bottom.
// Kinds: Text (pencil top right), Password (pencil + eye), Switch (On/Off 18/700 + a small 48x28 toggle with a 22 knob).
class CredentialTile final : public Widget {
public:
  enum class Kind : uint8_t { Text, Password, Switch };
  explicit CredentialTile(Kind K = Kind::Text) : K(K) {
    Bounds.H = 84;
    if (K == Kind::Password)
      Eye.OnActivate = [this]() {
        if (OnEye)
          OnEye();
      };
  }
  Kind K;
  std::string Label, Value;
  bool On = false;                       // Switch kind
  bool ShowSecret = false;               // Password kind: the value is shown (drawn as is); the screen masks it otherwise
  std::function<void()> OnEye;
  void SetOn(bool V, uint64_t Now);
  void Children(std::vector<Widget*>& Out) override {
    if (K == Kind::Password)
      Out.push_back(&Eye);
  }
  void Layout() { Eye.Bounds = Rect {Bounds.X + Bounds.W - 18 - 22 - 11, Bounds.Y + Bounds.H - 14 - 22 - 11, 44, 44}; }
  EyeButton& EyeBtn() { return Eye; }
  void Draw(const Frame& F) override;
  float RingOutset() const override { return 8; }
  float RingStroke() const override { return 4; }
  float RingRadius() const override { return 16; }

private:
  EyeButton Eye;
  Tween Knob;
  bool KnobInit = false;
};

// min 72 high, bottom rule, grid 2fr 3fr 140 140, gap 24: name 18/700 over the address (mono_ui 14 muted); activity 16; down and
// up speeds mono 16 right-aligned (the active direction 600 in `text`, a zero direction muted).
class ClientRow final : public Widget {
public:
  ClientRow() { Bounds.H = 72; }
  uint64_t ClientId = 0;
  std::string Name, Address, Activity, Down, Up;
  bool DownActive = false, UpActive = false;
  void Draw(const Frame& F) override;
  float RingOutset() const override { return 4; }
  float RingStroke() const override { return 3; }
  float RingRadius() const override { return 12; }
  // The four column rects (du) for a row of width W at X: name, activity, down, up.
  static void Columns(float X, float W, float Out[4][2]);
};

// A RootFS setup card. Wide (88 high): icon tile 52 r12, name 18/700, meta 14 muted, badge at the right. Tall (120 high): icon tile
// 44 top left, badge top right, name 17/700 and meta 13 muted at the bottom. Disabled cards show the reason as their meta line.
class OptionCard final : public Widget {
public:
  enum class Badge : uint8_t { Radio, Check };
  OptionCard(bool Tall, Badge B) : Tall(Tall), B(B) { Bounds.H = Tall ? 120 : 88; }
  bool Tall;
  Badge B;
  IconId Icon = IconId::Package;
  std::string Name, Meta, Tag;  // Tag: "Recommended"
  bool Selected = false;
  void Draw(const Frame& F) override;
  float RingOutset() const override { return 8; }
  float RingStroke() const override { return 4; }
  float RingRadius() const override { return 16; }
};

// "PICK ONE" style section header at (X, Y): the section label on the left and a 13 muted note at the right edge of W.
void DrawSectionHeader(const Frame& F, float X, float Y, float W, const std::string& Label, const std::string& Note);

} // namespace ui
