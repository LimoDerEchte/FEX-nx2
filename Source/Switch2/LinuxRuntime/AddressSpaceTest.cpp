// SPDX-License-Identifier: MIT
#include "../GuestAddressSpace.h"

#include <cassert>
#include <elf.h>

using Switch2Guest::GuestAddressSpace;
using Switch2Guest::PageSize;

int main() {
  GuestAddressSpace Space;
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

  GuestAddressSpace Sender, Receiver;
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

  GuestAddressSpace Parent;
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
}
