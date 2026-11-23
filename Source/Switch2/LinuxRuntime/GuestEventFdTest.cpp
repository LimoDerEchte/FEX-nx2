// SPDX-License-Identifier: MIT
#include "../GuestEventFd.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(Cond) do { if (!(Cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #Cond); std::exit(1); } } while (0)

using namespace Switch2Guest;

int main() {
  EventCounter E;
  uint64_t Out = 99;
  CHECK(!E.Readable() && E.Writable());
  CHECK(E.Read(Out) == -11 && Out == 99);          // empty: EAGAIN
  CHECK(E.Write(5) == 0 && E.Write(2) == 0 && E.Value == 7 && E.Readable());
  CHECK(E.Read(Out) == 0 && Out == 7 && E.Value == 0 && !E.Readable());
  CHECK(E.Write(0) == 0 && E.Value == 0);

  EventCounter S {3, true};
  CHECK(S.Read(Out) == 0 && Out == 1 && S.Value == 2);
  CHECK(S.Read(Out) == 0 && Out == 1 && S.Read(Out) == 0 && Out == 1 && S.Value == 0);
  CHECK(S.Read(Out) == -11);

  EventCounter W;
  CHECK(W.Write(UINT64_MAX) == -22 && W.Value == 0);
  CHECK(W.Write(EventFdMax) == 0 && W.Value == EventFdMax);
  CHECK(!W.Writable() && W.Readable());
  CHECK(W.Write(1) == -11 && W.Value == EventFdMax);   // overflow: EAGAIN
  CHECK(W.Write(0) == 0);                                // adding 0 at max is fine
  CHECK(W.Read(Out) == 0 && Out == EventFdMax && W.Value == 0 && W.Writable());

  EventCounter O {EventFdMax - 3, false};
  CHECK(O.Write(4) == -11 && O.Value == EventFdMax - 3);
  CHECK(O.Write(3) == 0 && O.Value == EventFdMax);
  std::puts("GuestEventFd: OK");
}
