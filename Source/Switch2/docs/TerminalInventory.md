# Terminal and pseudo-terminal inventory (UI Task 8a)

Linux x86-64 behavior of the terminal devices of the guest runtime: the
launcher's console terminal and pseudo-terminals. `Complete` means the rules
named in Notes match Linux 7.3, checked by `tests/guest/PtySuite.c` natively
(`make check-pty-suite-native`, as uid 1000 and as `unshare -r` root, 43
cases). `Partial` names every deviation. Format follows
[ContainerInventory.md](ContainerInventory.md). Console verification: not run
yet (see the checkpoint in [NextMilestone.md](../NextMilestone.md)).

State and byte rules are in `runtime/process/GuestPty.h` (`GuestTty`,
`PtyTable`, `TtyMaster`; host test `make check-guest-pty`) and the n_tty
editing and echo rules in `GuestTerminal.h` (`GuestTerminalInput::Linux`).
The runtime wiring is in `GuestRuntime.cpp` (`TtyIoctl`, `ReadTerminal`,
`ReadPtyMaster`, `WritePty`, `PtyEvents`, `OpenPtyMaster`, `OpenPtySlave`,
`HangupTty`, `DisassociateTty`).

## Design

- **One tty object per terminal.** `GuestTty` holds the line discipline and
  termios (`GuestTerminalInput`), the window size, the session and foreground
  group, the hang-up flag, the flow state and, for a pty, the devpts identity
  (index, lock, owner, mode 0620, group tty 5, open master and slave
  descriptions). The console is one instance (`ConsoleTty`) whose behavior is
  unchanged: its output is not post-processed, it does not act on XON/XOFF and
  it echoes control characters raw (`FullDiscipline` and
  `GuestTerminalInput::Linux` are off). A pty has both on.
- **Controlling terminal per process.** `GuestProcess::Ctty` replaces the old
  `HasTerminal` flag and the console's `TerminalSession`/`ForegroundGroup`
  globals. Job control (`TerminalAccess`: SIGTTIN, SIGTTOU, the ignored,
  blocked and orphaned rules), `TIOCSCTTY`, `TIOCNOTTY`, the exit of a session
  leader and `setsid` work on whichever tty the process holds.
- **Master side behind an interface.** `TtyMaster` has three calls: `Space()`
  (room for slave output), `Output()` (slave output after output flags and
  echo) and `TakeInput()` (bytes the master typed that the discipline has not
  received). The guest's `/dev/ptmx` descriptors use `PtyQueueMaster` (a 64 KiB
  queue). A host-side master (UI Task 8b) implements the interface with locked
  queues and installs it in `GuestTty::Master`. Nothing in the runtime pumps
  `TakeInput` yet (no host master exists); the interface and the queue are
  tested by `GuestPtyTest.cpp` only.
- **Wake-ups.** Blocked pty calls wait with `WaitKind::IO` and rerun when
  `IOEpoch` changes; every pty read, write, close and ioctl that changes
  state bumps it (poll, select and epoll use the same epoch).
- **Close and hang-up.** A pty description that is destroyed is queued
  (`ClosedPtys`) and handled when the syscall ends (`DrainClosedFiles`): the
  last master description hangs the slave up (`HangupTty`).

## Syscalls

| Number | Syscall | Status | Notes |
| ---: | --- | --- | --- |
| 2, 257, 437 | `open`, `openat`, `openat2` | Complete | `/dev/ptmx` and `/dev/pts/ptmx` allocate a master and the lowest free index (limit 4096, then ENOSPC); the flags (`O_NONBLOCK`, `O_CLOEXEC`, `O_RDONLY`) apply, a master never becomes a controlling terminal. `/dev/pts/<n>`: ENOENT unless a master is open; permission as the node's mode 0620 owner and group allow (EACCES), then a locked slave is EIO. A session leader without a controlling terminal acquires the slave on a read-open without `O_NOCTTY` when the terminal has no session (a write-only open does not). `/dev/tty` opens the process's controlling terminal (console or pty slave), ENXIO without one. |
| 0 | `read` | Complete | Slave: canonical lines (one line per read), raw reads under VMIN/VTIME, EOF (`^D`), SIGTTIN for a background group (EIO when ignored, blocked or orphaned), 0 after a hang-up, EAGAIN with `O_NONBLOCK`. Master: the slave's output in order (raw); EIO once a slave was opened and every slave description is closed, but only after the pending data; EAGAIN or a wait otherwise. `readv`, `preadv` (ESPIPE) and `pread64` (ESPIPE) share the paths. |
| 1 | `write` | Partial | Slave: output flags, then the master's queue; SIGTTOU with TOSTOP for a background group; EIO after a hang-up; blocks while output is stopped (`TCOOFF`, `^S`). Master: bytes go through the slave's discipline (signals, echo, editing); accepted before any slave exists. **Deviation:** a blocking write larger than the room (slave input buffer 4096 bytes, master queue 65536 bytes) returns the short count instead of blocking until all is written. |
| 8 | `lseek` | Complete | ESPIPE on both ends. |
| 16 | `ioctl` | Partial | See [ioctl requests](#ioctl-requests). |
| 7, 23, 271, 270, 232, 281 | `poll`, `select`, `ppoll`, `pselect6`, `epoll_wait`, `epoll_pwait` | Complete | n_tty poll masks (below). |
| 4, 5, 6, 262 | `stat`, `fstat`, `lstat`, `newfstatat` | Complete | `/dev/pts/<n>`: character device 136:`n` (`PtsRdev`), mode 0620, owner the master opener's fsuid, group 5; `/dev/ptmx`, `/dev/pts/ptmx`: 5:2 mode 0666 root; the same values by path and by descriptor (st_dev, st_ino, st_rdev). The group is 65534 in a user namespace that does not map group 5, as natively. |
| 21, 269, 439 | `access`, `faccessat`, `faccessat2` | Complete | Pty slaves are checked against their own owner and mode, not the inode store's default owner. |
| 78, 217 | `getdents`, `getdents64` | Complete | `/dev/pts` lists `ptmx` and the slaves whose master is open (DT_CHR); `/dev` lists `ptmx`. |
| 89 | `readlink` (`/proc/<pid>/fd/<n>`) | Complete | `/dev/pts/<n>`, `/dev/ptmx` or `/dev/pts/ptmx` as opened; `ttyname(3)` and `ttyname_r(3)` work. |
| 138 | `fstatfs` | Partial | A slave opened by path reports devpts (0x1cd1); `/dev/ptmx` reports tmpfs like the other `/dev` nodes. |
| 90, 91, 92, 93, 94, 268, 260 | `chmod`, `fchmod`, `chown`, `fchown`, `lchown`, `fchmodat`, `fchownat` | Complete | `/dev/pts/<n>` only: the owner (or CAP_FOWNER) sets the mode, CAP_CHOWN or the owner's own-group rule sets the owner and group (as `grantpt(3)` and sudo do); the node's mode and owner govern later opens and `access`. `/dev/ptmx` keeps the generic device answer (EOPNOTSUPP). |
| 112, 109, 121, 124 | `setsid`, `setpgid`, `getpgid`, `getsid` | Complete | `setsid` drops the controlling terminal; the terminal's session and group follow Linux rules. |
| 3 | `close` | Complete | The last description of a master hangs the slave up (below); the index is free once the master, every slave description and every process holding the terminal as controlling terminal are gone. |
| 60 | `exit`, `exit_group` | Complete | A session leader's exit sends SIGHUP to the foreground group (SIGCONT only for TIOCNOTTY) and every process of the session loses the terminal. |

Not done for the pty paths: `utimensat` of `/dev/pts/<n>`, `O_ASYNC`/`SIGIO`,
`splice`, `sendfile`, `fcntl(F_SETOWN)` on a tty, a second devpts instance (a
guest `mount -t devpts` is still an empty file system, so `bwrap --dev` cannot
offer ptys), `kernel.pty.max` and the devpts mount options (`mode`, `gid`,
`ptmxmode`, `max`).

## ioctl requests

On a master descriptor the requests act on its slave tty (`tty_pair_get_tty`)
except where noted.

| Request | Status | Notes |
| --- | --- | --- |
| `TCGETS`, `TCSETS`, `TCSETSW`, `TCSETSF` | Complete | The 36-byte kernel termios. SIGTTOU for a background group (ignored or blocked: allowed; orphaned: EIO). A pty keeps CS8 and CREAD and clears PARENB as `pty_set_termios` does. `TCSETSF` flushes input. EFAULT for a bad pointer. |
| `TCGETS2`, `TCSETS2`, `TCSETSW2`, `TCSETSF2` | Complete | The 44-byte `termios2`. Rates come from c_cflag (CBAUD, CBAUDEX, CIBAUD); an explicit speed counts only with BOTHER in the field. |
| `TCSBRK` | Partial | Job-control check, then 0. No break is generated and no output is queued to drain. |
| `TCXONC` | Complete | `TCOOFF`/`TCOON` stop and restart the slave's output (writes block, poll shows no POLLOUT); `TCIOFF`/`TCION` put the STOP or START character into the master's output (nothing when it is disabled); other values EINVAL. IXON `^S`/`^Q` (and IXANY) do the same; a `TCOOFF` is not undone by `^Q`. |
| `TCFLSH` | Partial | 0, 1, 2 (else EINVAL). Input flush clears the unread input and the unfinished line. **Deviation:** `TCOFLUSH` and the flush done by a signal character keep the master's unread output (Linux drops only bytes still in the driver's buffers, which this queue never holds). |
| `TIOCGPGRP`, `TIOCSPGRP`, `TIOCGSID` | Complete | Slave: ENOTTY unless the caller's controlling terminal. Master: `TIOCGPGRP`/`TIOCGSID` answer for the slave, `TIOCSPGRP` needs the caller to control the slave (ENOTTY). `TIOCSPGRP`: SIGTTOU check, EINVAL for a negative group, ESRCH when no process has that number, EPERM when the group's session is not the caller's (also pgid 1: the runtime's virtual init exists in session 1; 0.17.13, the check order of `tiocspgrp`, `CheckTiocspgrp` in `GuestJobControl.h`), EFAULT. Numbers are in the caller's PID namespace. |
| `TIOCSCTTY` | Complete | Leader only (EPERM), not when it has a terminal (EPERM); success when it is already this terminal's session; a terminal of another session needs argument 1 and CAP_SYS_ADMIN (EPERM) and then moves. Also through a master descriptor. |
| `TIOCNOTTY` | Complete | ENOTTY unless the slave is the controlling terminal (a master descriptor never is). A session leader hangs up its foreground group (SIGHUP, SIGCONT) and the session loses the terminal. |
| `TIOCGWINSZ`, `TIOCSWINSZ` | Complete | Pty: both ends share the size (0x0 at first); a changed size sends SIGWINCH to the foreground group, the same size sends nothing. Console: `TIOCGWINSZ` is the launcher's terminal size, `TIOCSWINSZ` is accepted and ignored (fixed geometry, unchanged). |
| `FIONREAD` (`TIOCINQ`) | Complete | Slave: bytes of finished lines (canonical) or queued bytes (raw). Master: unread output. |
| `TIOCOUTQ` | Partial | Always 0: output reaches the master queue at once. |
| `TIOCGETD`, `TIOCSETD` | Partial | N_TTY only: 0 is returned and accepted, other lines EINVAL. |
| `TIOCPKT` (master) | Partial | Packet mode: each master read starts with a status byte (0 with data); status 1 (`TIOCPKT_FLUSHREAD`), 2 (`FLUSHWRITE`), 4 (`STOP`) and 8 (`START`) are reported alone. Not generated: `TIOCPKT_NOSTOP`, `DOSTOP`, `IOCTL`. Slave: ENOTTY. |
| `TIOCGPTN` (master) | Complete | The index; ENOTTY on a slave. |
| `TIOCSPTLCK`, `TIOCGPTLCK` (master) | Complete | New masters are locked; `unlockpt(3)` is `TIOCSPTLCK` 0. ENOTTY on a slave. |
| `TIOCGPTPEER` (master) | Complete | Opens the slave with the given flags (access mode, `O_NOCTTY`, `O_CLOEXEC`, `O_NONBLOCK`; others EINVAL), no permission check, EIO while locked. On a slave: EIO. |
| `FIONBIO`, `FIOCLEX`, `FIONCLEX` | Complete | Generic. |
| After a hang-up | Complete | Every request on a hung-up slave descriptor is EIO except `TIOCSPGRP` (ENOTTY), `FIONBIO` and `FIOCLEX`. |
| `TIOCEXCL`, `TIOCNXCL`, `TIOCGEXCL`, `TIOCSTI`, `TIOCCONS`, `TIOCVHANGUP`, `TIOCSIG`, `TIOCMGET`/`TIOCMSET`, `TIOCSERGETLSR`, `TIOCGICOUNT`, `TCGETA`/`TCSETA*`, `TIOCLINUX` | Unrouted | ENOTTY. |

## Line discipline (pty)

| Rule | Status | Notes |
| --- | --- | --- |
| Canonical input | Complete | Lines end at newline, VEOL (and VEOL2 with IEXTEN); `^D` ends a line or is end of file when the line is empty; at most 4095 characters of a line, 4096 buffered bytes in all (more are dropped for a raw slave, a master write waits). |
| Editing | Partial | VERASE, VKILL, VWERASE (alphanumerics and `_` are words), VREPRINT, VLNEXT (the next byte is data, signals and flow characters included). Not done: VDISCARD (`^O`), VSWTC, EXTPROC, parity and break handling (INPCK, IGNBRK, BRKINT, PARMRK), IUCLC, IMAXBEL bell. |
| Input flags | Complete | ISTRIP, IGNCR, ICRNL, INLCR, IXON, IXANY. |
| Echo | Partial | ECHO, ECHOE (`\b \b`, twice for a control character shown as `^X`), ECHOK and ECHOKE (kill erases the line, or shows the kill character and a newline), ECHONL, ECHOCTL (`^X`, also for ESC), ECHO off. Verified byte for byte against Linux 7.3. Not done: ECHOPRT, and the column of an erased TAB assumes the line started at column 0 (Linux tracks the output column). |
| Signals | Complete | ISIG with VINTR, VQUIT, VSUSP to the foreground group, echo `^C` with ECHO, flush of the unread input unless NOFLSH (see `TCFLSH` deviation). No signal without a foreground group. |
| Output flags | Partial | OPOST with ONLCR and OCRNL, applied to slave output and echo. Not done: OLCUC, ONOCR, ONLRET, delays, tab expansion. |
| Raw reads | Complete | VMIN/VTIME in all four combinations, including the inter-byte timer; poll reports POLLIN as `n_tty_poll` does (VMIN bytes without VTIME). |
| Flow control | Complete | See `TCXONC`. |

## Poll masks

| End | Mask |
| --- | --- |
| Slave | POLLIN with readable input; POLLOUT unless output is stopped or the queue is full. After a hang-up: POLLIN, POLLOUT, POLLERR and POLLHUP. |
| Master | POLLIN with unread output (and POLLPRI with a packet status); POLLHUP once a slave was opened and every slave description is closed (not before the first open); POLLOUT while the slave's input has room. |

## Hang-up and lifetime

- Master close (last description): the slave is hung up for good. Slave
  descriptors read 0, write EIO, poll `POLLIN|POLLOUT|POLLERR|POLLHUP`, and
  ioctls fail (above). The node `/dev/pts/<n>` disappears at once (ENOENT, not
  listed). The **session leader** gets SIGHUP then SIGCONT and every process
  of the session loses the terminal. As on Linux the foreground group is not
  signalled here (the shell forwards it); it is signalled when the leader exits
  or calls `TIOCNOTTY`.
- A master read after all slaves closed: pending data first, then EIO; a new
  slave open makes the pair live again. A master write before a slave exists
  succeeds and the bytes wait in the slave's input.
- The index is reusable when the master, every slave description and every
  controlling-terminal reference are gone.

## Console terminal

Unchanged except for sharing the new object: the standard streams and
`/dev/tty` of an interactive run use `ConsoleTty`; its job control, TIOCSCTTY
and TIOCNOTTY checks are the same code as a pty's; the poll rule for a raw
console read now also honors VMIN (a read needs VMIN bytes). A session leader's
exit still sends SIGHUP only.


## Launcher tabs

The first tab starts one `GuestSyscalls` worker. Later tabs start new processes
in that runtime. Each process leads its own session and process group. Its
pty slave supplies descriptors 0–2 and its controlling terminal. The launcher
holds the master and exchanges input, output, and window size with the worker.
Only the worker applies terminal discipline, sends signals, or uses the live
inode store. The first resize publishes the initial window size. Later changes
send SIGWINCH to the foreground group.

Closing a tab releases its master and hangs up the terminal. The guest worker
sends SIGHUP and SIGCONT to the session leader. A leader that survives a
user-requested close triggers cleanup of the session after two seconds. A
normal Bash exit closes its tab. A developer program's exit status or a launch
failure stays visible until the user closes its tab. The worker saves inode
metadata when the last session ends or the app closes. FTP is suspended until
then.

The launcher tab path has no runtime HID or controller polling. The earlier
`ConsoleTty` path is still available through the legacy interactive entry point.

## Terminal emulator and keys (UI Task 8c)

`ui/TerminalGrid.cpp` is the emulator behind each tab (`TERM=xterm-256color`).
`ui/TerminalKeys.cpp` turns keys into bytes. Status: Partial (listed gaps
below); everything else named here is implemented and covered by
`ui/tests/TerminalTest.cpp` (`make check-ui`).

Complete:
- C0: BS, HT with tab stops (HTS/TBC), LF/VT/FF, CR, BEL. UTF-8 decoding with
  U+FFFD for invalid bytes, also across split feeds. Wide glyphs take two cells
  (table `ui/TerminalWide.inc`). Pending wrap (DECAWM, mode 7).
- ESC: 7/8, D, E, H, M, c, =, >, `(0`/`(B` (DEC line drawing to Unicode).
- CSI: CUU CUD CUF CUB CNL CPL CHA CUP HVP VPA HPA, ED 0-3, EL, ECH, ICH, DCH,
  IL, DL, SU, SD, DECSTBM, REP, SCOSC/SCORC, TBC, SGR (bold, dim, italic,
  underline, blink, inverse, hidden, strike, 16 and bright, 256-color,
  truecolor with `;` and `:`), DSR 5/6 and DA1/DA2 replies (written to the
  pty master), DECSCUSR accepted (the cursor stays a block).
- Modes: 1, 7, 12, 25, 47, 66, 1047, 1048, 1049 (alternate screen, cursor
  save), 2004. Unknown private modes are ignored.
- OSC 0/2 set the tab title. OSC 8, 52 and others, and DCS strings, are
  consumed silently; BEL and ST end them. CAN/SUB abort a sequence.
- Scrollback (2000 lines) on the main screen only. Resize keeps content
  clipped or padded, the cursor in range, and the tab sends TIOCSWINSZ.

Partial / not implemented: no reflow on resize; no mouse reporting modes
(1000-1006); no focus events (1004); no sixel/ReGIS; no OSC 52 clipboard;
DECSCUSR shapes ignored; no combining-character composition (a combining mark
takes a cell); only G0 designation (no G1/SO/SI).

Keys (`EncodeTerminalKey`): printable text as UTF-8; Enter CR; Backspace DEL
(Ctrl+Backspace BS); Tab; Shift+Tab `ESC [ Z`; Esc; arrows/Home/End as CSI,
or SS3 in application cursor mode; Insert `ESC [ 2 ~`, Delete `3~`, PgUp `5~`, PgDn `6~` (0.17.12 sent 4 and 5: PgUp acted as End and PgDn as PgUp; `4~` is End);
F1-F4 SS3, F5-F12 `ESC [ n ~`; modifiers as `;2`..`;8`; Ctrl+letter and
Ctrl+ `@ [ \ ] ^ _ space ?`; Alt as ESC prefix; keypad per DECKPAM; bracketed
paste wrapping when mode 2004 is set (software keyboard text is sent as paste).

Sources: the hardware USB keyboard when the platform reports one
(`Switch2Platform.cpp`), the software keyboard (Y), and the controller.
Controller on the Shell screen: D-pad and left stick = arrows, A = Enter, B = Esc, Y =
software keyboard, X then D-pad up/down/left/right = Ctrl+C / Ctrl+Z /
Ctrl+D / Tab (X again or any other button cancels; the footer shows the
mapping), L/R switch tabs, ZR opens a new tab, ZL closes the current tab (a shell at its prompt hangs up at once,
a running program asks first), the right stick scrolls the back buffer (Shift+PgUp/PgDn on a keyboard), + Settings,
- exits, R3 (click of the right stick) opens the list of all bindings and closes it again (B too). The footer shows
only ZR, ZL, B, X, Y and R3 Help, as button discs (the UI font has no arrow glyphs: 0.17.12 spelled the D-pad
directions with arrow characters, which drew as missing glyphs). BEL does nothing (0.17.12 flashed the terminal
for 120 ms: bash rings it on Tab with several completions and on unbound keys).
Ctrl+C and Ctrl+Z reach the foreground group through the runtime line
discipline (VINTR/VSUSP).
