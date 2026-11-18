# Next milestone: a multi-process graphical Linux app

## Starting point

Branch: `nx2`. FEX 0.8.4 is verified on a physical Switch 2 with the
Debian 12 amd64 glibc 2.36 rootfs. Stock `bookworm-true.elf` returned 0.
The baseline `bookworm-glibc-baseline.elf`, linked with Debian's startup
objects, passed its TLS and `dlopen("libm.so.6")` test and returned 42.
Star Catch and the custom dynamic ELF demo also passed on hardware in
their respective milestones.

Keep the Debian rootfs in `/fex/rootfs/bookworm-baseline-236` and select
it with the executable's `.rootfs` sidecar. The previous CachyOS loader
requires guest AVX instructions during startup. Its startup objects
also marked the test executable as x86-64-v4. Use the Debian toolchain
inputs for the next test. See [the glibc bring-up notes](LinuxRuntimePlan.md).

## Goal

Run two dynamically linked x86-64 Linux guest programs in one NRO
session. One program is a graphical UI. It sends a request to a helper
over a Unix-domain socket and presents the helper's reply on screen.
Controller input changes the request. When the UI exits, the launcher
reaps the helper, closes the channel, and returns to its browser.

Start with a purpose-built harness using normal Linux socket calls and
the existing frame-presentation frontend. The harness gives us a
controlled process and IPC test before bringing in a general display
server. Steam Big Picture remains the long-term target.

## Implementation order

1. Give each guest process explicit ownership of its FEX context, CPU
   state, ELF images, stack, VMA table, descriptor table, rootfs, and
   exit state. Lift the shared guest stack and jump buffer out of the
   launcher path before running concurrent guests. Keep console, display,
   and log operations on an appropriate host owner.
2. Start two separate guest images from one launcher test session. Use
   launcher-managed startup first to verify process lifetimes without
   depending on guest fork. Add Unix-domain stream transport with
   blocking reads, close and EOF behavior, and polling. Verify message
   ordering, partial transfers, and cleanup when either guest exits.
3. Move helper startup into the guest-facing process path. Choose and
   trace one standard Linux launch API, preferably glibc
   `posix_spawn` plus `waitpid`. Implement only the required
   `clone` or `clone3`, `execve`, descriptor inheritance, and wait
   behavior. If the observed launch path shares the caller's address
   space, honor its lifetime and suspension rules. Treat general fork
   and copy-on-write as a separate compatibility problem.
4. Wire the graphical test to the IPC helper. Reuse the frame bridge and
   controller polling. Keep a plain-text version of the test to pinpoint
   process failures independently of display problems.

## Acceptance on hardware

- A glibc-linked parent starts a glibc-linked helper through the guest
  launch API. The parent receives the helper's reply over the socket
  and displays a recognizable change after controller input.
- The parent can handle helper EOF and observe its exit status. Quitting
  returns to the browser. Running the test again starts with fresh state.
- Stock `true`, the baseline glibc probe, the custom dynamic demo, and
  Star Catch still pass. Capture the test NRO version and console result
  in this file or the README before committing the milestone.

Horizon currently hosts guest code in the NRO's address space. Per-guest
ownership and syscall checks are needed for correctness. They do not
provide kernel-enforced process isolation. The first test can use
trusted fixture programs while evaluating what a real desktop client
needs from memory isolation, threading, signals, networking, and display.
