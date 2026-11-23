// SPDX-License-Identifier: MIT
#pragma once
// inotify instances, as Linux fs/notify/inotify reports events.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace Switch2Guest {
constexpr uint32_t InAccess = 0x1, InModify = 0x2, InAttrib = 0x4, InCloseWrite = 0x8, InCloseNowrite = 0x10,
                   InOpen = 0x20, InMovedFrom = 0x40, InMovedTo = 0x80, InCreate = 0x100, InDelete = 0x200,
                   InDeleteSelf = 0x400, InMoveSelf = 0x800, InUnmount = 0x2000, InQOverflow = 0x4000,
                   InIgnored = 0x8000, InOnlyDir = 0x01000000, InDontFollow = 0x02000000, InExclUnlink = 0x04000000,
                   InMaskCreate = 0x10000000, InMaskAdd = 0x20000000, InIsDir = 0x40000000, InOneShot = 0x80000000,
                   InAllEvents = 0xfff;
// Bits a watch keeps (ALL_INOTIFY_BITS without the add/create modifiers).
constexpr uint32_t InWatchBits = InAllEvents | InOnlyDir | InDontFollow | InExclUnlink | InOneShot;
constexpr size_t InotifyMaxQueued = 16384, InotifyMaxWatches = 8192, InotifyMaxInstances = 128;
constexpr size_t InotifyHeaderSize = 16;

struct InotifyEvent {
  int32_t Wd;
  uint32_t Mask, Cookie;
  std::string Name;
  bool operator==(const InotifyEvent&) const = default;
};

// struct inotify_event plus the name padded with NULs so that at least one
// NUL follows it, to a multiple of the header size.
inline size_t EncodedSize(const InotifyEvent& Event) {
  if (Event.Name.empty()) return InotifyHeaderSize;
  return InotifyHeaderSize + (Event.Name.size() + InotifyHeaderSize) / InotifyHeaderSize * InotifyHeaderSize;
}

inline size_t Encode(const InotifyEvent& Event, uint8_t* Out) {
  const auto Size = EncodedSize(Event);
  const uint32_t Length = static_cast<uint32_t>(Size - InotifyHeaderSize);
  std::memset(Out, 0, Size);
  std::memcpy(Out, &Event.Wd, 4);
  std::memcpy(Out + 4, &Event.Mask, 4);
  std::memcpy(Out + 8, &Event.Cookie, 4);
  std::memcpy(Out + 12, &Length, 4);
  std::memcpy(Out + InotifyHeaderSize, Event.Name.data(), Event.Name.size());
  return Size;
}

class InotifyInstance {
public:
  struct Watch {
    int32_t Wd;
    uint64_t Inode;
    uint32_t Mask;
    bool Directory;
  };

  // Returns the watch descriptor or -errno. The same inode keeps its wd; the
  // mask is replaced, or OR'd with IN_MASK_ADD; IN_MASK_CREATE fails with
  // EEXIST. Argument checks (EINVAL, ENOTDIR, EACCES) happen in the caller.
  int64_t AddWatch(uint64_t Inode, uint32_t Mask, bool IsDirectory) {
    const auto Kept = Mask & InWatchBits;
    if (auto* Existing = FindInode(Inode)) {
      if (Mask & InMaskCreate) return -17;
      Existing->Mask = (Mask & InMaskAdd) ? Existing->Mask | Kept : Kept;
      return Existing->Wd;
    }
    if (Watches.size() >= InotifyMaxWatches) return -28;
    Watches.push_back({NextWd++, Inode, Kept, IsDirectory});
    return Watches.back().Wd;
  }

  int64_t RemoveWatch(int32_t Wd) {
    const auto It = std::find_if(Watches.begin(), Watches.end(), [Wd](const auto& W) { return W.Wd == Wd; });
    if (It == Watches.end()) return -22;
    Watches.erase(It);
    Queue(InotifyEvent {Wd, InIgnored, 0, {}});
    return 0;
  }

  bool Watching(uint64_t Inode) const { return FindInode(Inode) != nullptr; }
  const Watch* WatchFor(uint64_t Inode) const { return FindInode(Inode); }

  // An event on a watched inode. Name is set for an event about a child of a
  // watched directory and empty for an event about the inode itself.
  // ExclUnlinked marks a child that is already unlinked (IN_EXCL_UNLINK).
  void Emit(uint64_t Inode, uint32_t Mask, uint32_t Cookie, std::string_view Name, bool IsDirectory,
            bool ExclUnlinked = false) {
    auto* Target = FindInode(Inode);
    if (!Target || !(Target->Mask & Mask & InAllEvents)) return;
    if (ExclUnlinked && (Target->Mask & InExclUnlink)) return;
    const int32_t Wd = Target->Wd;
    const bool OneShot = Target->Mask & InOneShot;
    Queue(InotifyEvent {Wd, (Mask & InAllEvents) | (IsDirectory ? InIsDir : 0), Cookie, std::string(Name)});
    if (OneShot) {
      Watches.erase(std::find_if(Watches.begin(), Watches.end(), [Wd](const auto& W) { return W.Wd == Wd; }));
      Queue(InotifyEvent {Wd, InIgnored, 0, {}});
    }
  }

  // The last link of a watched inode is gone (and, for files, its last
  // descriptor closed): IN_DELETE_SELF, then IN_IGNORED, and the watch ends.
  void InodeGone(uint64_t Inode) {
    auto* Target = FindInode(Inode);
    if (!Target) return;
    const int32_t Wd = Target->Wd;
    if (Target->Mask & InDeleteSelf) Queue(InotifyEvent {Wd, InDeleteSelf, 0, {}});
    Watches.erase(std::find_if(Watches.begin(), Watches.end(), [Wd](const auto& W) { return W.Wd == Wd; }));
    Queue(InotifyEvent {Wd, InIgnored, 0, {}});
  }

  // Ends the watch on Inode with IN_IGNORED, after the caller queued
  // IN_DELETE_SELF through Emit (Linux order: DELETE_SELF, the parent's
  // IN_DELETE, then IGNORED when the inode is evicted).
  void DropInode(uint64_t Inode) {
    auto* Target = FindInode(Inode);
    if (!Target) return;
    const int32_t Wd = Target->Wd;
    Watches.erase(std::find_if(Watches.begin(), Watches.end(), [Wd](const auto& W) { return W.Wd == Wd; }));
    Queue(InotifyEvent {Wd, InIgnored, 0, {}});
  }

  // Copies whole events; EINVAL when the first does not fit, EAGAIN when
  // nothing is queued (the caller blocks or returns it).
  int64_t Read(uint8_t* Out, size_t Count) {
    if (Events.empty()) return -11;
    if (EncodedSize(Events.front()) > Count) return -22;
    size_t Used = 0;
    while (!Events.empty() && Used + EncodedSize(Events.front()) <= Count) {
      Used += Encode(Events.front(), Out + Used);
      Events.pop_front();
    }
    ++ChangeCount;
    return static_cast<int64_t>(Used);
  }

  size_t PendingBytes() const {
    size_t Total = 0;
    for (const auto& Event : Events) Total += EncodedSize(Event);
    return Total;
  }

  bool Readable() const { return !Events.empty(); }
  size_t Queued() const { return Events.size(); }
  bool HasWatches() const { return !Watches.empty(); }
  // Counts queue changes, for edge-triggered epoll.
  uint64_t Changes() const { return ChangeCount; }

private:
  std::vector<Watch> Watches;
  std::deque<InotifyEvent> Events;
  int32_t NextWd {1};
  uint64_t ChangeCount {};

  Watch* FindInode(uint64_t Inode) {
    const auto It = std::find_if(Watches.begin(), Watches.end(), [Inode](const auto& W) { return W.Inode == Inode; });
    return It == Watches.end() ? nullptr : &*It;
  }
  const Watch* FindInode(uint64_t Inode) const {
    const auto It = std::find_if(Watches.begin(), Watches.end(), [Inode](const auto& W) { return W.Inode == Inode; });
    return It == Watches.end() ? nullptr : &*It;
  }

  // inotify_merge and the overflow event of fsnotify_insert_event: an event
  // equal to the tail is dropped; a full queue takes one IN_Q_OVERFLOW.
  void Queue(InotifyEvent Event) {
    if (!Events.empty() && Events.back() == Event) return;
    if (Events.size() >= InotifyMaxQueued) {
      const InotifyEvent Overflow {-1, InQOverflow, 0, {}};
      if (Events.back() != Overflow) {
        Events.push_back(Overflow);
        ++ChangeCount;
      }
      return;
    }
    Events.push_back(std::move(Event));
    ++ChangeCount;
  }
};
} // namespace Switch2Guest
