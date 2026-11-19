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
