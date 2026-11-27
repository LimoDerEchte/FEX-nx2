// SPDX-License-Identifier: MIT
// Focus tree and scrolling: exactly one focused node, spatial navigation (nearest centre inside the +-45 degree cone of the
// pressed direction, ties broken by the smaller sideways offset, then by id), explicit overrides, 44x44 du hit rects, and
// scroll-into-view (24 du margin, 150 ms ease-out). All rects are design units. No Vulkan or libnx types.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Anim.h"
#include "input/Dir.h"
#include "render/Renderer.h"

namespace ui {

constexpr int NoFocus = -1;

// Explicit override of one direction: a node id, or "the last focused node of a group".
struct Override {
  int Id = NoFocus;
  int Group = NoFocus;
};

struct FocusNode {
  int Id = NoFocus;
  Rect R;                 // du
  bool Enabled = true;    // disabled nodes are skipped by navigation
  int Group = NoFocus;    // for "last focused of group" overrides
  Override Over[4];       // indexed by Dir
  // The ring drawn around the node (FocusRing): outset, stroke and the target's own corner radius.
  float Outset = 10, Stroke = 4, Radius = 0;
};

// The rect hit-tested for a node: at least 44x44 du, centred on the visual.
Rect HitRect(Rect R);

class FocusTree {
public:
  // Rebuilds the node list; the focused id and the last focused of every group survive.
  void Begin() { Nodes.clear(); }
  void Add(const FocusNode& N) { Nodes.push_back(N); }
  // After the nodes are added: if the focused node is gone or disabled, the first enabled node is focused.
  void End();

  int Focused() const { return Cur; }
  const FocusNode* Find(int Id) const;
  const FocusNode* FocusedNode() const { return Find(Cur); }
  // Focuses Id if it exists and is enabled. Returns true when the focus changed.
  bool SetFocus(int Id);
  // Moves the focus one step. Returns the new id, or NoFocus when nothing moved.
  int Navigate(Dir D);
  // The node under a point, last added wins; disabled nodes do not count. NoFocus when none.
  int HitTest(float X, float Y) const;
  int Count() const { return int(Nodes.size()); }

private:
  int Best(const FocusNode& From, Dir D) const;
  std::vector<FocusNode> Nodes;
  int Cur = NoFocus;
  std::unordered_map<int, int> LastInGroup;
};

// Vertical scrolling with animated scroll-into-view.
class ScrollView {
public:
  void SetViewport(float Top, float Height, float ContentHeight);  // du: the viewport's screen rect (offset 0) and the content height
  float Offset(uint64_t Now) const;                                // the animated offset
  float Target() const { return Pos.To; }
  // Smallest scroll that shows R (unscrolled layout coordinates) with a 24 du margin, 150 ms ease-out.
  void EnsureVisible(Rect R, uint64_t Now);
  // Direct change (wheel, drag, inertia): immediate, no animation, clamped.
  void By(float Dy, uint64_t Now);
  void To(float Offset, uint64_t Now, uint32_t DurMs);
  float MaxOffset() const { return std::max(0.0f, Content - Height); }
  float ViewTop() const { return Top; }
  float ViewHeight() const { return Height; }

private:
  float Top = 0, Height = 0, Content = 0;
  Tween Pos;
};

} // namespace ui
