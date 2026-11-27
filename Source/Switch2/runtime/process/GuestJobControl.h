// SPDX-License-Identifier: MIT
// Linux wait statuses, wait option rules, process-group and session rules
// (kernel/exit.c, kernel/sys.c) and terminal signal characters
// (drivers/tty/n_tty.c) for the Switch 2 guest runtime.
#pragma once
#include <cstdint>

namespace Switch2Guest {
constexpr uint32_t WaitExitedStatus(uint32_t Code) {
  return (Code & 0xff) << 8;
}
constexpr uint32_t WaitSignaledStatus(uint32_t Signal) {
  return Signal & 0x7f;
}
constexpr uint32_t WaitStoppedStatus(uint32_t Signal) {
  return (Signal << 8) | 0x7f;
}
constexpr uint32_t WaitContinuedStatus = 0xffff;

enum class WaitIdType : uint32_t { All = 0, Pid = 1, Pgid = 2, Pidfd = 3 };
constexpr uint32_t WNoHang = 1, WUntraced = 2, WStopped = 2, WExited = 4, WContinued = 8, WNoWait = 0x01000000,
                   WNoThread = 0x20000000, WAll = 0x40000000, WClone = 0x80000000;

struct WaitSelector {
  WaitIdType Type;
  uint32_t Id;
  bool operator==(const WaitSelector&) const = default;
};

// wait4's pid argument: > 0 one child, 0 the caller's group, -1 any child,
// < -1 the group -pid.
constexpr WaitSelector SelectorFromWait4(int32_t Pid, uint32_t OwnGroup) {
  if (Pid > 0) return {WaitIdType::Pid, static_cast<uint32_t>(Pid)};
  if (Pid == 0) return {WaitIdType::Pgid, OwnGroup};
  if (Pid == -1) return {WaitIdType::All, 0};
  return {WaitIdType::Pgid, static_cast<uint32_t>(-static_cast<int64_t>(Pid))};
}

// kernel_wait4: WNOHANG, WUNTRACED, WCONTINUED and the __W flags.
constexpr int64_t ValidateWait4Options(uint32_t Options) {
  return Options & ~(WNoHang | WUntraced | WContinued | WNoThread | WAll | WClone) ? -22 : 0;
}

// kernel_waitid: the same plus WEXITED/WNOWAIT, and at least one state.
constexpr int64_t ValidateWaitIdOptions(uint32_t Options) {
  if (Options & ~(WNoHang | WNoWait | WExited | WStopped | WContinued | WNoThread | WAll | WClone)) return -22;
  return Options & (WExited | WStopped | WContinued) ? 0 : -22;
}

struct SessionView {
  uint32_t Pid, Group, Session;
  bool IsChild, ChildExecd;
};

// setpgid(2) after pid and pgid defaults are applied (Linux kernel/sys.c).
// Target is the caller itself or a child of the caller.
constexpr int64_t CheckSetpgid(const SessionView& Caller, const SessionView& Target, bool GroupExistsInSession,
                               uint32_t NewGroup) {
  if (!Target.IsChild && Target.Pid != Caller.Pid) return -3; // ESRCH
  if (Target.IsChild) {
    if (Target.Session != Caller.Session) return -1; // EPERM
    if (Target.ChildExecd) return -13;               // EACCES
  }
  if (Target.Session == Target.Pid) return -1; // a session leader keeps its group
  if (NewGroup != Target.Pid && !GroupExistsInSession) return -1;
  return 0;
}

// TIOCSPGRP (tty_jobctl_ioctl / tiocspgrp): ESRCH when no process, group or session leader has that number (find_vpid fails),
// then EPERM when the group's session (session_of_pgrp: a member's, else that of the process of that number) is not the
// caller's. GroupSession is 0 when none was found. The runtime's virtual init is pid 1 of the root PID namespace, in
// session 1: it exists, and it is never the caller's session.
constexpr int64_t CheckTiocspgrp(bool NumberExists, uint32_t GroupSession, uint32_t CallerSession) {
  if (!NumberExists) return -3;
  return GroupSession == CallerSession && GroupSession != 0 ? 0 : -1;
}

// setsid(2): EPERM when a process group with the caller's ID exists.
constexpr int64_t CheckSetsid(const SessionView&, bool GroupIdInUse) {
  return GroupIdInUse ? -1 : 0;
}

constexpr uint32_t TerminalIsig = 1;
constexpr int TerminalVintr = 0, TerminalVquit = 1, TerminalVsusp = 10;

// The signal a byte generates with ISIG set, or 0. A control character of
// 0 (_POSIX_VDISABLE) never matches.
constexpr uint32_t TerminalSignalFor(uint8_t Byte, const uint8_t ControlChars[19], uint32_t LocalFlags) {
  if (!(LocalFlags & TerminalIsig)) return 0;
  const auto Matches = [&](int Index) { return ControlChars[Index] && Byte == ControlChars[Index]; };
  if (Matches(TerminalVintr)) return 2;  // SIGINT
  if (Matches(TerminalVquit)) return 3;  // SIGQUIT
  if (Matches(TerminalVsusp)) return 20; // SIGTSTP
  return 0;
}
} // namespace Switch2Guest
