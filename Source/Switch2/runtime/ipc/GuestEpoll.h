// SPDX-License-Identifier: MIT
#pragma once
// epoll interest lists, as Linux fs/eventpoll.c reports them.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Switch2Guest {
constexpr uint32_t EpollIn = 0x1, EpollPri = 0x2, EpollOut = 0x4, EpollErr = 0x8, EpollHup = 0x10, EpollRdNorm = 0x40,
                   EpollRdBand = 0x80, EpollWrNorm = 0x100, EpollWrBand = 0x200, EpollMsg = 0x400, EpollRdHup = 0x2000,
                   EpollExclusive = 1u << 28, EpollWakeup = 1u << 29, EpollOneShot = 1u << 30, EpollEt = 1u << 31;
constexpr uint32_t EpollCtlAdd = 1, EpollCtlDel = 2, EpollCtlMod = 3;
// Bits that may accompany EPOLLEXCLUSIVE (EPOLLEXCLUSIVE_OK_BITS).
constexpr uint32_t EpollExclusiveOk = EpollIn | EpollOut | EpollRdNorm | EpollRdBand | EpollWrNorm | EpollWrBand |
                                      EpollErr | EpollHup | EpollWakeup | EpollEt | EpollExclusive;
// Flag bits that never appear in reported events (EP_PRIVATE_BITS).
constexpr uint32_t EpollPrivate = EpollWakeup | EpollOneShot | EpollEt | EpollExclusive;

struct __attribute__((packed)) EpollEvent {
  uint32_t Events;
  uint64_t Data;
};
static_assert(sizeof(EpollEvent) == 12);

// The EPOLLEXCLUSIVE checks of epoll_ctl, after the descriptors were found.
inline int64_t CheckEpollExclusive(uint32_t Op, uint32_t Events, bool TargetIsEpoll) {
  if ((Op != EpollCtlAdd && Op != EpollCtlMod) || !(Events & EpollExclusive)) return 0;
  if (Op == EpollCtlMod) return -22;
  if (TargetIsEpoll || (Events & ~EpollExclusiveOk)) return -22;
  return 0;
}

template<typename Key>
class EpollSet {
public:
  int64_t Add(const Key& K, uint32_t Events, uint64_t Data) {
    if (Find(K) != Items.end()) return -17;
    Items.push_back({K, Events, Data, NotSeen, false});
    return 0;
  }

  int64_t Modify(const Key& K, uint32_t Events, uint64_t Data) {
    const auto Item = Find(K);
    if (Item == Items.end()) return -2;
    if (Item->Events & EpollExclusive) return -22;
    // A modified item is checked again, so an ET item that is ready reports.
    *Item = {K, Events, Data, NotSeen, false};
    return 0;
  }

  int64_t Remove(const Key& K) {
    const auto Item = Find(K);
    if (Item == Items.end()) return -2;
    Erase(Item);
    return 0;
  }

  bool Contains(const Key& K) const {
    return std::any_of(Items.begin(), Items.end(), [&](const auto& Item) { return Item.K == K; });
  }

  // The watched description is gone: drop its item without an error.
  void Forget(const Key& K) {
    if (const auto Item = Find(K); Item != Items.end()) Erase(Item);
  }

  template<typename Predicate>
  void ForgetIf(Predicate Gone) {
    for (size_t Index = 0; Index < Items.size();) {
      if (Gone(Items[Index].K)) Erase(Items.begin() + Index);
      else ++Index;
    }
  }

  size_t Size() const { return Items.size(); }

  // Every interest with its event mask and data (/proc/<pid>/fdinfo of an epoll descriptor).
  template<typename Visit>
  void ForEachItem(Visit Function) const {
    for (const auto& Item : Items) Function(Item.K, Item.Events, Item.Data);
  }

  template<typename Visit>
  void ForEachKey(Visit Function) const {
    for (const auto& Item : Items) Function(Item.K);
  }

  // Reports up to Max ready items. Readiness(K) gives the current poll mask;
  // Generation(K) changes whenever the file's state changes. Level-triggered
  // items report while ready; edge-triggered ones once per change; one-shot
  // ones once until modified. EPOLLERR and EPOLLHUP are always reported.
  // Scanning resumes after the last reported item, so with a small Max
  // ready items take turns as on Linux's ready list.
  template<typename ReadyFn, typename GenerationFn>
  size_t Collect(EpollEvent* Out, size_t Max, ReadyFn Readiness, GenerationFn Generation) {
    size_t Count = 0;
    const size_t Total = Items.size();
    if (!Total || !Max) return 0;
    size_t Start = Cursor % Total, Last = Start;
    for (size_t Step = 0; Step < Total && Count < Max; ++Step) {
      const size_t Index = (Start + Step) % Total;
      auto& Item = Items[Index];
      if (Item.Disabled) continue;
      const uint32_t Ready = static_cast<uint32_t>(Readiness(Item.K)) & ((Item.Events & ~EpollPrivate) | EpollErr | EpollHup);
      if (!Ready) continue;
      if (Item.Events & EpollEt) {
        const uint64_t Now = Generation(Item.K);
        if (Now == Item.SeenGeneration) continue;
        Item.SeenGeneration = Now;
      }
      if (Item.Events & EpollOneShot) Item.Disabled = true;
      Out[Count++] = {Ready, Item.Data};
      Last = Index;
    }
    if (Count) Cursor = Last + 1;
    return Count;
  }

  // Whether Collect would report anything (an epoll file's own readiness).
  template<typename ReadyFn, typename GenerationFn>
  bool AnyReady(ReadyFn Readiness, GenerationFn Generation) const {
    return std::any_of(Items.begin(), Items.end(), [&](const auto& Item) {
      if (Item.Disabled) return false;
      const uint32_t Ready = static_cast<uint32_t>(Readiness(Item.K)) & ((Item.Events & ~EpollPrivate) | EpollErr | EpollHup);
      return Ready && (!(Item.Events & EpollEt) || Generation(Item.K) != Item.SeenGeneration);
    });
  }

private:
  static constexpr uint64_t NotSeen = UINT64_MAX;
  struct Item {
    Key K;
    uint32_t Events;
    uint64_t Data;
    uint64_t SeenGeneration;
    bool Disabled;
  };
  std::vector<Item> Items;
  size_t Cursor {};

  auto Find(const Key& K) {
    return std::find_if(Items.begin(), Items.end(), [&](const auto& Item) { return Item.K == K; });
  }

  void Erase(typename std::vector<Item>::iterator It) {
    const size_t Index = It - Items.begin();
    Items.erase(It);
    if (Cursor > Index) --Cursor;
  }
};
} // namespace Switch2Guest
