// SPDX-License-Identifier: MIT
#include "GuestFilePunch.h"

#include <cassert>

int main() {
  using Switch2Guest::PlanFilePunch;
  constexpr uint64_t Page = 4096;

  const auto Interior = PlanFilePunch(5 * Page, Page + 8, 4 * Page + 7, Page);
  assert(Interior.ClearBefore.Begin == Page + 8 && Interior.ClearBefore.End == 2 * Page);
  assert(Interior.Reclaim.Begin == 2 * Page && Interior.Reclaim.End == 4 * Page);
  assert(Interior.ClearAfter.Begin == 4 * Page && Interior.ClearAfter.End == 4 * Page + 7);

  const auto Tail = PlanFilePunch(3 * Page + 1, Page, 4 * Page, Page);
  assert(Tail.ClearBefore.Empty() && Tail.ClearAfter.Empty());
  assert(Tail.Reclaim.Begin == Page && Tail.Reclaim.End == 3 * Page + 1);

  const auto UncoveredTail = PlanFilePunch(3 * Page + 1, Page, 3 * Page + 1, Page);
  assert(UncoveredTail.Reclaim.End == 3 * Page);
  assert(UncoveredTail.ClearAfter.End == 3 * Page + 1);

  const auto Unaligned = PlanFilePunch(4 * Page, Page - 1, Page + 1, Page);
  assert(Unaligned.Reclaim.Empty() && Unaligned.ClearAfter.Empty());
  assert(Unaligned.ClearBefore.Begin == Page - 1 && Unaligned.ClearBefore.End == Page + 1);

  const auto Beyond = PlanFilePunch(Page, 2 * Page, 3 * Page, Page);
  assert(Beyond.ClearBefore.Empty() && Beyond.Reclaim.Empty() && Beyond.ClearAfter.Empty());
}
