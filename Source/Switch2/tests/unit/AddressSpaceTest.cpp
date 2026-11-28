// SPDX-License-Identifier: MIT
#include "GuestAddressSpace.h"

#include <cassert>
#include <elf.h>
#include <sys/mman.h>

using Switch2Guest::ArenaBackend;
using Switch2Guest::GuestAddressSpace;
using Switch2Guest::GuestArena;
using Switch2Guest::PageSize;

// Uncommitted arena memory is PROT_NONE on the host, so any stray access to
// it kills the test the way it would end the app on the console.
struct HostBackend : ArenaBackend {
  bool Commit(uint64_t Address, size_t Size) override {
    return mprotect(reinterpret_cast<void*>(Address), Size, PROT_READ | PROT_WRITE) == 0;
  }
  bool Decommit(uint64_t Address, size_t Size) override {
    // The kernel returns the pages and they read zero on the next commit.
    return madvise(reinterpret_cast<void*>(Address), Size, MADV_DONTNEED) == 0 &&
           mprotect(reinterpret_cast<void*>(Address), Size, PROT_NONE) == 0;
  }
};

static constexpr size_t G = GuestArena::Granule;
static constexpr size_t ArenaBytes = 128 * G;

int main() {
  void* Map = mmap(nullptr, ArenaBytes + G, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  assert(Map != MAP_FAILED);
  const uint64_t ArenaBase = (reinterpret_cast<uint64_t>(Map) + G - 1) & ~(G - 1);
  HostBackend Backend;
  GuestArena Arena {Backend, ArenaBase, ArenaBytes, 16 * G};
  GuestArena::Current() = &Arena;
  GuestAddressSpace Space {Arena};
  const auto Start = Space.Allocate(5 * PageSize, PF_R | PF_W);
  assert(Start && Start % PageSize == 0);
  assert(Space.Owns(Start, 5 * PageSize));
  assert(!Space.Owns(Start - PageSize, 2 * PageSize));
  assert(!Space.Owns(Start + 4 * PageSize, 2 * PageSize));
  assert(!Space.Set(Start + 1, PageSize, PF_X, true));
  assert(Space.Set(Start + PageSize, 2 * PageSize, PF_R | PF_X, true));
  assert(Space.Query(Start).Size == PageSize);
  assert(Space.Query(Start + PageSize + 1).Flags == (PF_R | PF_X));
  assert(Space.Query(Start + PageSize + 1).Size == 2 * PageSize);
  assert(Space.Set(Start + 2 * PageSize, PageSize, Space.Unmapped, true));
  assert(!Space.Query(Start + 2 * PageSize).Size);
  assert(!Space.IsMapped(Start, 5 * PageSize));
  assert(!Space.Set(Start, 5 * PageSize, PF_R));
  assert(Space.Set(Start + 2 * PageSize, PageSize, PF_W, true));
  assert(Space.IsMapped(Start, 5 * PageSize));
  assert(Space.Set(Start, 5 * PageSize, Space.Unmapped, true));
  assert(!Space.Owns(Start, PageSize));

  const auto Mixed = Space.Allocate(3 * PageSize, 0);
  const auto Middle = Mixed + PageSize;
  assert(Mixed && Space.CanReplace(Middle, PageSize, true));
  assert(Space.Set(Middle, PageSize, PF_R | PF_W, true));
  Space.ChangeAttribute(Middle, PageSize, Space.SharedPage, true);
  assert(!Space.IsShared(Mixed, PageSize) && Space.IsShared(Middle, PageSize));
  assert(Space.IsShared(Middle + 4, sizeof(uint32_t)) && !Space.IsShared(Mixed + 4, sizeof(uint32_t)));
  assert(!Space.IsShared(Middle + PageSize, PageSize));
  assert(Space.Set(Mixed, PageSize, PF_R | PF_W));
  auto* Private = reinterpret_cast<uint8_t*>(Mixed);
  auto* Shared = reinterpret_cast<uint8_t*>(Middle);
  assert(*Shared == 0);
  *Private = 12;
  *Shared = 34;
  assert(Space.SavePrivatePages());
  {
    auto Child = Space.ForkChild();
    assert(!Child.CanReplace(Mixed, PageSize, true) && !Child.CanReplace(Middle, PageSize, false));
    *Private = 56;
    *Shared = 78;
    Child.RestorePrivatePages();
    assert(*Private == 12 && *Shared == 78);
  }
  assert(Space.CanReplace(Middle, PageSize, false));
  assert(Space.Set(Mixed, 3 * PageSize, Space.Unmapped, true));

  alignas(PageSize) static char Image[3 * PageSize];
  const auto Base = reinterpret_cast<uint64_t>(Image);
  Space.Register(Base, 3 * PageSize, PF_R);
  Space.Register(Base + PageSize + 64, 32, PF_R | PF_X);
  assert(!Space.Owns(Base, PageSize));
  assert(Space.Query(Base).Size == PageSize);
  assert(Space.Query(Base + PageSize).Flags == (PF_R | PF_X));
  assert(Space.Query(Base + 2 * PageSize).Start == Base + 2 * PageSize);
  assert(Space.Set(Base + PageSize, PageSize, PF_R));
  assert(Space.Query(Base + PageSize).Flags == PF_R);

  GuestAddressSpace Sender {Arena}, Receiver {Arena};
  auto Backing = std::shared_ptr<void> {std::aligned_alloc(PageSize, PageSize), &std::free};
  assert(Backing);
  const auto SharedFile = reinterpret_cast<uint64_t>(Backing.get());
  assert(Sender.RegisterSharedFile(SharedFile, PageSize, PF_R | PF_W, Backing));
  assert(Sender.HasWritableFileMapping(Backing));
  assert(Sender.AllowsFileMappingWrite(SharedFile, PageSize));
  assert(!Sender.RegisterSharedFile(SharedFile, PageSize, PF_R, Backing));
  assert(Sender.Owns(SharedFile, PageSize) && !Sender.Owns(SharedFile, PageSize, true));
  assert(Receiver.RegisterSharedFile(SharedFile, PageSize, PF_R, Backing, false));
  assert(!Receiver.HasWritableFileMapping(Backing));
  assert(!Receiver.AllowsFileMappingWrite(SharedFile, PageSize));
  assert(Receiver.AllowsFileMappingWrite(SharedFile + PageSize, PageSize));
  auto& SharedFileByte = *reinterpret_cast<char*>(SharedFile);
  SharedFileByte = 'a';
  assert(Sender.SavePrivatePages());
  SharedFileByte = 'b';
  Sender.RestorePrivatePages();
  assert(SharedFileByte == 'b');
  Backing.reset();
  assert(Sender.Set(SharedFile, PageSize, Sender.Unmapped, true));
  assert(Receiver.Query(SharedFile).Size == PageSize && SharedFileByte == 'b');
  assert(Receiver.Set(SharedFile, PageSize, Receiver.Unmapped, true));

  GuestAddressSpace Parent {Arena};
  const auto PrivateVA = Parent.Allocate(PageSize, PF_R | PF_W);
  const auto SharedVA = Parent.Allocate(PageSize, PF_R | PF_W, true);
  assert(PrivateVA && SharedVA);
  auto& PrivateByte = *reinterpret_cast<char*>(PrivateVA);
  auto& SharedByte = *reinterpret_cast<char*>(SharedVA);
  PrivateByte = 'p';
  SharedByte = 's';
  assert(Parent.SavePrivatePages());
  auto Child = Parent;
  PrivateByte = 'c';
  SharedByte = 'X';
  assert(Child.SavePrivatePages());
  Parent.RestorePrivatePages();
  assert(PrivateByte == 'p' && SharedByte == 'X');
  Child.RestorePrivatePages();
  assert(PrivateByte == 'c' && SharedByte == 'X');
  assert(Child.Set(PrivateVA, PageSize, Child.Unmapped, true));
  Parent.RestorePrivatePages();
  assert(PrivateByte == 'p' && !Child.Owns(PrivateVA, PageSize));

  // The arena is committed lazily: PROT_NONE mappings cost nothing.
  {
    GuestAddressSpace Memory {Arena};
    const auto Before = Arena.Committed();
    const auto Reserve = Memory.Allocate(64 * PageSize, 0, false, G);
    assert(Reserve && Arena.Committed() == Before);
    assert(!Memory.IsCommitted(Reserve, PageSize) && !Memory.PageCommitted(Reserve));
    assert(Memory.Available(Reserve, 0) > 0 && Memory.Available(Reserve, PF_R) == 0);
    assert(Memory.IsMapped(Reserve, 64 * PageSize));
    // Never touches the uncommitted pages.
    assert(Memory.SavePrivatePages());
    assert(Memory.SetPages(Reserve + PageSize, 2 * PageSize, PF_R | PF_W, true) == GuestAddressSpace::SetStatus::Ok);
    assert(Arena.Committed() == Before + G);
    assert(Memory.IsCommitted(Reserve + PageSize, 2 * PageSize) && !Memory.PageCommitted(Reserve));
    assert(Memory.Available(Reserve + PageSize, PF_R | PF_W) == 2 * PageSize);
    assert(Memory.Available(Reserve + PageSize + 100, PF_R) == 2 * PageSize - 100);
    assert(Memory.Available(Reserve, PF_R) == 0 && Memory.Available(Reserve + 3 * PageSize, PF_R) == 0);
    *reinterpret_cast<char*>(Reserve + PageSize) = 'q';
    // Discarding returns the page; the next commit reads zero.
    Memory.Discard(Reserve + PageSize, PageSize);
    assert(!Memory.PageCommitted(Reserve + PageSize));
    assert(Memory.SetPages(Reserve + PageSize, PageSize, PF_R | PF_W, true) == GuestAddressSpace::SetStatus::Ok);
    assert(*reinterpret_cast<char*>(Reserve + PageSize) == 0);
    // Unmapping the whole range gives the granule back.
    assert(Memory.SetPages(Reserve, 64 * PageSize, Memory.Unmapped, true) == GuestAddressSpace::SetStatus::Ok);
    assert(Arena.Committed() == Before && !Memory.IsMapped(Reserve, PageSize));
  }

  // A failed commit changes nothing and Available stays zero.
  {
    HostBackend Small;
    GuestArena Tiny {Small, ArenaBase + 100 * G, 20 * G, G};
    GuestAddressSpace Memory {Tiny};
    assert(!Memory.Allocate(2 * G, PF_R | PF_W));
    const auto Reserve = Memory.Allocate(4 * G, 0, false, G);
    assert(Reserve);
    assert(Memory.SetPages(Reserve, 2 * G, PF_R | PF_W, true) == GuestAddressSpace::SetStatus::NoMemory);
    assert(Memory.Query(Reserve).Flags == 0 && Memory.Available(Reserve, PF_R) == 0 && Tiny.Committed() == 0);
    assert(Memory.SetPages(Reserve, PageSize, PF_R, true) == GuestAddressSpace::SetStatus::Ok);
    assert(Memory.Available(Reserve, PF_R) == PageSize);
    assert(Memory.MaxMapping() == G);
  }

  // Adjacent mappings with equal protection read as one run.
  {
    GuestAddressSpace Memory {Arena};
    const auto A = Memory.Allocate(2 * PageSize, PF_R | PF_W);
    assert(Memory.Allocate(2 * PageSize, PF_R | PF_W) == A + 2 * PageSize);
    assert(Memory.Query(A).Size == 4 * PageSize && Memory.Available(A + PageSize, PF_R) == 3 * PageSize);
    assert(Memory.Query(A + 3 * PageSize).Start == A);
    assert(Memory.Set(A + 2 * PageSize, PageSize, PF_R, true));
    assert(Memory.Query(A).Size == 2 * PageSize && Memory.Query(A + 3 * PageSize).Start == A + 3 * PageSize && Memory.Query(A + 2 * PageSize).Size == PageSize);
  }

  // Fork views: private pages save only when committed; the saved copy is
  // independent; DONTFORK pages vanish and WIPEONFORK pages read zero.
  {
    GuestAddressSpace Parent2 {Arena};
    const auto A = Parent2.Allocate(6 * PageSize, PF_R | PF_W);
    std::memset(reinterpret_cast<void*>(A), 'a', 6 * PageSize);
    assert(Parent2.ChangeAttribute(A + PageSize, PageSize, GuestAddressSpace::DontFork, true) == 1);
    assert(Parent2.ChangeAttribute(A + 2 * PageSize, PageSize, GuestAddressSpace::WipeOnFork, true) == 1);
    assert(Parent2.ChangeAttribute(A + 2 * PageSize, PageSize, GuestAddressSpace::WipeOnFork, true) == 0);
    assert(Parent2.AllHave(A + 2 * PageSize, PageSize, GuestAddressSpace::WipeOnFork));
    assert(!Parent2.AllHave(A, 3 * PageSize, GuestAddressSpace::WipeOnFork) && Parent2.AnyHave(A, 3 * PageSize, GuestAddressSpace::DontFork));
    Parent2.ChangeAttribute(A, 4 * PageSize, GuestAddressSpace::Locked, true);
    assert(Parent2.LockedBytes() == 4 * PageSize);
    assert(Parent2.SavePrivatePages());
    auto Kid = Parent2.ForkChild();
    assert(Kid.LockedBytes() == 0 && Parent2.LockedBytes() == 4 * PageSize);
    assert(!Kid.IsMapped(A + PageSize, PageSize) && Kid.IsMapped(A, PageSize) && Kid.IsMapped(A + 2 * PageSize, PageSize));
    // The child cannot give back pages the parent still holds.
    const auto Held = Arena.Committed();
    assert(Kid.Set(A, 6 * PageSize, Kid.Unmapped, true) && Arena.IsCommitted(A, 6 * PageSize) && Arena.Committed() == Held);
    std::memset(reinterpret_cast<void*>(A), 'b', 6 * PageSize);
    Parent2.RestorePrivatePages();
    assert(*reinterpret_cast<char*>(A) == 'a');
    // process_vm access to a non-resident view reads and writes its saved copy.
    GuestAddressSpace Sleeper {Arena};
    const auto S = Sleeper.Allocate(3 * PageSize, PF_R | PF_W);
    std::memset(reinterpret_cast<void*>(S), 's', 3 * PageSize);
    assert(Sleeper.SavePrivatePages());
    auto Twin = Sleeper.ForkChild();
    std::memset(reinterpret_cast<void*>(S), 'x', 3 * PageSize);
    char Buffer[16] {};
    assert(Sleeper.CopyFromProcess(S + 5, Buffer, 4, false) == 4 && Buffer[0] == 's');
    assert(Sleeper.CopyFromProcess(S + 5, Buffer, 4, true) == 4 && Buffer[0] == 'x');
    assert(Twin.CopyToProcess(S + 3 * PageSize - 2, "wxyz", 4, false) == 2);
    assert(Twin.CopyFromProcess(S + 3 * PageSize - 4, Buffer, 4, false) == 4 && Buffer[2] == 'w');
    // The write went to the twin's private copy only.
    assert(Sleeper.CopyFromProcess(S + 3 * PageSize - 2, Buffer, 2, false) == 2 && Buffer[0] == 's');
    assert(Sleeper.CopyFromProcess(S + 3 * PageSize, Buffer, 2, true) == 0);
  }

  // mremap.
  {
    GuestAddressSpace Memory {Arena};
    uint64_t New {};
    const auto A = Memory.Allocate(4 * PageSize, PF_R | PF_W);
    for (size_t Page = 0; Page < 4; ++Page) std::memset(reinterpret_cast<void*>(A + Page * PageSize), '1' + Page, PageSize);
    using Status = GuestAddressSpace::RemapStatus;
    assert(Memory.Remap(A + PageSize, 8 * PageSize, 8 * PageSize, true, false, std::nullopt, New) == Status::Fault);
    assert(Memory.Remap(A, 4 * PageSize, 2 * PageSize, false, false, std::nullopt, New) == Status::Ok && New == A);
    assert(!Memory.IsMapped(A + 2 * PageSize, PageSize));
    // Grow back in place: the reservation still holds the pages.
    assert(Memory.Remap(A, 2 * PageSize, 4 * PageSize, false, false, std::nullopt, New) == Status::Ok && New == A);
    assert(*reinterpret_cast<char*>(A + 3 * PageSize) == 0 && *reinterpret_cast<char*>(A) == '1');
    // Grow beyond the reservation: extend into free address space.
    assert(Memory.Remap(A, 4 * PageSize, 6 * PageSize, false, false, std::nullopt, New) == Status::Ok && New == A);
    assert(Memory.Query(A).Size == 6 * PageSize);
    // Blocked growth moves with MAYMOVE only.
    assert(Arena.ReserveAt(A + 6 * PageSize, PageSize));
    assert(Memory.Remap(A, 6 * PageSize, 8 * PageSize, false, false, std::nullopt, New) == Status::NoMemory);
    assert(Memory.Remap(A, 6 * PageSize, 8 * PageSize, true, false, std::nullopt, New) == Status::Ok && New != A);
    assert(*reinterpret_cast<char*>(New) == '1' && *reinterpret_cast<char*>(New + PageSize) == '2' && Memory.Available(New, PF_R) == 8 * PageSize);
    assert(!Memory.IsMapped(A, PageSize));
    // DONTUNMAP leaves a zero-filled mapping behind.
    const auto B = Memory.Allocate(2 * PageSize, PF_R | PF_W);
    std::memset(reinterpret_cast<void*>(B), 'm', 2 * PageSize);
    assert(Memory.Remap(B, 2 * PageSize, 2 * PageSize, true, true, std::nullopt, New) == Status::Ok && New != B);
    assert(*reinterpret_cast<char*>(New) == 'm' && *reinterpret_cast<char*>(B) == 0 && Memory.IsMapped(B, 2 * PageSize));
    // Shared mappings shrink in place but never move.
    const auto Shared = Memory.Allocate(2 * PageSize, PF_R | PF_W, true, G);
    assert(Arena.ReserveAt(Shared + 2 * PageSize, PageSize));
    assert(Memory.Remap(Shared, 2 * PageSize, 4 * PageSize, true, false, std::nullopt, New) == Status::NoMemory);
    assert(Memory.Remap(Shared, 2 * PageSize, PageSize, true, false, std::nullopt, New) == Status::Ok && New == Shared);
    // MREMAP_FIXED moves onto owned address space.
    const auto Dest = Memory.Allocate(2 * PageSize, 0);
    assert(Memory.Remap(B, 2 * PageSize, 2 * PageSize, true, false, Dest, New) == Status::Ok && New == Dest);
    assert(*reinterpret_cast<char*>(Dest) == 0 && Memory.Available(Dest, PF_W) == 2 * PageSize);
    // MREMAP_FIXED onto free arena address space claims it.
    const auto Free = Arena.Reserve(4 * PageSize, G);
    assert(Free && Arena.Release(Free, 4 * PageSize));
    std::memset(reinterpret_cast<void*>(Dest), 'f', PageSize);
    assert(Memory.Remap(Dest, 2 * PageSize, 2 * PageSize, true, false, Free, New) == Status::Ok && New == Free);
    assert(*reinterpret_cast<char*>(Free) == 'f' && Memory.Available(Free, PF_W) == 2 * PageSize && !Memory.IsMapped(Dest, PageSize));
    // An overlapping FIXED target is EINVAL before anything else.
    assert(Memory.Remap(Free, 2 * PageSize, 2 * PageSize, true, false, Free + PageSize, New) == Status::Invalid);
    // MREMAP_DONTUNMAP of a shared mapping is EINVAL (Linux 7.3).
    assert(Memory.Remap(Shared, PageSize, PageSize, true, true, std::nullopt, New) == Status::Invalid);
  }

  // brk-style reservation: pages start unmapped, the region survives being
  // emptied, and pages inside it can be mapped and unmapped again.
  {
    GuestAddressSpace Memory {Arena};
    const auto Heap = Memory.ReserveUnmapped(64 * PageSize);
    assert(Heap && Memory.Owns(Heap, 64 * PageSize) && !Memory.AnyMapped(Heap, 64 * PageSize));
    assert(Memory.MappedBytes() == 0 && Memory.Available(Heap, PF_R) == 0);
    assert(Memory.SetPages(Heap, 2 * PageSize, PF_R | PF_W, true) == GuestAddressSpace::SetStatus::Ok);
    assert(Memory.MappedBytes() == 2 * PageSize && Memory.Available(Heap, PF_W) == 2 * PageSize);
    std::memset(reinterpret_cast<void*>(Heap), 'h', 2 * PageSize);
    assert(Memory.Set(Heap, 2 * PageSize, GuestAddressSpace::Unmapped, true));
    assert(Memory.Owns(Heap, 64 * PageSize) && Memory.MappedBytes() == 0);
    assert(Memory.SetPages(Heap, PageSize, PF_R | PF_W, true) == GuestAddressSpace::SetStatus::Ok);
    assert(*reinterpret_cast<char*>(Heap) == 0);
    // MappedRuns skips holes.
    assert(Memory.SetPages(Heap + 3 * PageSize, 2 * PageSize, PF_R, true) == GuestAddressSpace::SetStatus::Ok);
    const auto Runs = Memory.MappedRuns(Heap, 64 * PageSize);
    assert(Runs.size() == 2 && Runs[0].first == Heap && Runs[0].second == PageSize);
    assert(Runs[1].first == Heap + 3 * PageSize && Runs[1].second == 2 * PageSize);
    assert(Memory.MappedRuns(Heap + PageSize, 2 * PageSize).empty());
  }
}
