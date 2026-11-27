// SPDX-License-Identifier: MIT
#include "GuestJobControl.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      std::exit(1);                                           \
    }                                                         \
  } while (0)

using namespace Switch2Guest;

int main() {
  CHECK(WaitExitedStatus(3) == 0x300);
  CHECK(WaitExitedStatus(0x1ff) == 0xff00);
  CHECK(WaitSignaledStatus(9) == 9);
  CHECK(WaitStoppedStatus(20) == 0x147f);
  CHECK(WaitContinuedStatus == 0xffff);

  CHECK((SelectorFromWait4(0, 77) == WaitSelector {WaitIdType::Pgid, 77}));
  CHECK((SelectorFromWait4(-1, 77) == WaitSelector {WaitIdType::All, 0}));
  CHECK((SelectorFromWait4(-5, 77) == WaitSelector {WaitIdType::Pgid, 5}));
  CHECK((SelectorFromWait4(12, 77) == WaitSelector {WaitIdType::Pid, 12}));

  CHECK(ValidateWait4Options(WExited) == -22);
  CHECK(ValidateWait4Options(WNoWait) == -22);
  CHECK(ValidateWait4Options(WNoHang | WUntraced | WContinued | WAll) == 0);
  CHECK(ValidateWaitIdOptions(WNoHang) == -22);
  CHECK(ValidateWaitIdOptions(0) == -22);
  CHECK(ValidateWaitIdOptions(WExited | WNoWait) == 0);
  CHECK(ValidateWaitIdOptions(WStopped | WNoHang) == 0);
  CHECK(ValidateWaitIdOptions(WExited | 0x10) == -22);

  const SessionView Shell {100, 100, 100, false, false};
  const SessionView Child {101, 100, 100, true, false};
  const SessionView Stranger {102, 102, 100, false, false};
  CHECK(CheckSetpgid(Shell, Child, false, 101) == 0);       // new group named after the child
  CHECK(CheckSetpgid(Shell, Child, true, 100) == 0);        // join an existing group
  CHECK(CheckSetpgid(Shell, Child, false, 55) == -1);       // group not in the session
  CHECK(CheckSetpgid(Shell, Stranger, true, 102) == -3);    // neither self nor child
  CHECK(CheckSetpgid(Shell, {101, 100, 100, true, true}, false, 101) == -13);  // child exec'd
  CHECK(CheckSetpgid(Shell, {101, 101, 101, true, false}, false, 101) == -1);  // other session
  CHECK(CheckSetpgid(Shell, Shell, true, 100) == -1);       // session leader
  const SessionView Member {103, 100, 100, false, false};
  CHECK(CheckSetpgid(Member, Member, false, 103) == 0);

  // TIOCSPGRP: pgid 1 (init) exists in another session: EPERM, not ESRCH (the console's virtual init is not a process).
  CHECK(CheckTiocspgrp(false, 0, 100) == -3);      // no such number
  CHECK(CheckTiocspgrp(true, 1, 100) == -1);       // init: another session
  CHECK(CheckTiocspgrp(true, 100, 100) == 0);      // a group of the caller's session
  CHECK(CheckTiocspgrp(true, 0, 100) == -1);       // exists but no session found: never a match

  CHECK(CheckSetsid(Shell, true) == -1);
  CHECK(CheckSetsid(Member, false) == 0);

  const uint8_t Chars[19] {3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22};
  CHECK(TerminalSignalFor(0x03, Chars, TerminalIsig) == 2);
  CHECK(TerminalSignalFor(0x1c, Chars, TerminalIsig) == 3);
  CHECK(TerminalSignalFor(0x1a, Chars, TerminalIsig) == 20);
  CHECK(TerminalSignalFor(0x03, Chars, 0) == 0);
  CHECK(TerminalSignalFor('a', Chars, TerminalIsig) == 0);
  uint8_t Disabled[19] {};
  CHECK(TerminalSignalFor(0, Disabled, TerminalIsig) == 0);
  std::puts("guest job control: ok");
}
