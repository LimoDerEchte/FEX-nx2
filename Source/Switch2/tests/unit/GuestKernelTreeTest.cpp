// SPDX-License-Identifier: MIT
#include "GuestKernelTree.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(Cond) do { if (!(Cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #Cond); std::exit(1); } } while (0)

using namespace Switch2Guest::KernelTree;

int main() {
  Tree<int> T;
  T.AddRoot("/sys");
  T.AddRoot("/proc/sys");

  // Coverage: roots and below, not siblings with the same prefix.
  CHECK(T.Covers("/sys"));
  CHECK(T.Covers("/sys/anything"));
  CHECK(!T.Covers("/system"));
  CHECK(!T.Covers("/proc"));
  CHECK(!T.Covers("/proc/self"));
  CHECK(T.Covers("/proc/sys/kernel"));

  // Adding creates parents; nothing outside a root.
  CHECK(T.Add("/sys/kernel/mm/transparent_hugepage/enabled", Kind::File, 0444, 7));
  CHECK(!T.Add("/proc/self/x", Kind::File, 0444, 1));
  CHECK(!T.Add("/sys", Kind::File, 0444, 1));
  CHECK(T.IsDirectory("/sys/kernel"));
  CHECK(T.IsDirectory("/sys/kernel/mm/transparent_hugepage"));
  CHECK(T.Find("/sys/kernel")->Mode == 0555);
  const auto* File = T.Find("/sys/kernel/mm/transparent_hugepage/enabled");
  CHECK(File && File->Type == Kind::File && File->Mode == 0444 && File->Data == 7);
  CHECK(!T.IsDirectory("/sys/kernel/mm/transparent_hugepage/enabled"));
  CHECK(!T.Find("/sys/nothing"));
  CHECK(!T.Find("/proc"));

  // Children: direct only, sorted.
  CHECK(T.Add("/sys/devices/system/cpu/online", Kind::File, 0444, 1));
  CHECK(T.Add("/sys/class/dmi", Kind::Link, 0777, 2));
  const auto Top = T.Children("/sys");
  CHECK(Top.size() == 3);
  CHECK(Top[0].first == "class" && Top[0].second == Kind::Directory);
  CHECK(Top[1].first == "devices");
  CHECK(Top[2].first == "kernel");
  CHECK(T.Children("/sys/class").size() == 1 && T.Children("/sys/class")[0].second == Kind::Link);
  CHECK(T.Children("/sys/kernel/mm/transparent_hugepage/enabled").empty()); // not a directory

  // A name that only shares a prefix is not a child.
  CHECK(T.Add("/sys/devices2", Kind::File, 0444, 3));
  CHECK(T.Children("/sys/devices").size() == 1);

  // Replace and remove (with everything below).
  CHECK(T.Add("/sys/devices2", Kind::File, 0644, 4));
  CHECK(T.Find("/sys/devices2")->Mode == 0644 && T.Find("/sys/devices2")->Data == 4);
  T.Remove("/sys/devices");
  CHECK(!T.Find("/sys/devices"));
  CHECK(!T.Find("/sys/devices/system/cpu/online"));
  CHECK(T.Find("/sys/devices2"));
  T.Remove("/sys");
  CHECK(T.IsDirectory("/sys")); // a root stays

  // Nested roots: the longer one decides.
  CHECK(T.Add("/proc/sys/kernel/ostype", Kind::File, 0444, 5));
  CHECK(T.Children("/proc/sys").size() == 1);

  std::puts("guest-kernel-tree: ok");
  return 0;
}
