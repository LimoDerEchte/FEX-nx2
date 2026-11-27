# Launcher UI: implementation plan

> **Historical.** The milestone is complete. Task texts describe the tree as it was
> when each task started: `LinuxRuntime/`, `RootfsSetupHost.cpp`, `RootfsManifest.py`
> and `CheckRootfsSetup.py` were removed later in this milestone, and the specs moved
> from the repository root to `Source/Switch2/docs/ui/`. The README is the current map.

> **For agentic workers:** follow `AGENTS.md` and the process below. Each
> task is one Sonnet agent, one after another. The task text and the
> "Global constraints" are your requirements; the two spec files are the
> source of truth for every number.

**Goal:** replace the text-console launcher (`consoleInit` + `Launcher`
class in `GuestProbe.cpp`) with the GPU-rendered UI described in
`docs/ui/fex-switch2-ui-spec.md` and `docs/ui/fex-switch2-theming-spec.md`
(copied into the repo from `~/Downloads` by Task 1).

**Architecture:** the UI draws with **Vulkan through OunceVK**, as
[ounce-hbmenu](https://github.com/ouncebrew/ounce-hbmenu) does in its `switch2/`
folder (`graphics_vulkan.c`, `ouncevk_platform.c`). It uses the default
NWindow (`nwindowSetModeDimensions(nwindowGetDefault(), 0, 0, 0, 0)`), a
`vkCreateViSurfaceNN` surface and a FIFO swapchain that is recreated when the
output size changes: 1920×1080 handheld, and up to 3840×2160 docked. SDL2 was tried
(2026-11-25 console probe 1, log `.superpowers/sdd/ui-probe1.log`): its
Switch video driver does not start under libounce, so SDL fell back to
`offscreen` and failed with "Could not find a valid EGL device". SDL2 is dropped.

A small **platform abstraction** replaces what SDL2 would have given:
- `ui/platform/Display.h`: output size, scale `s`, size-change events,
  frame begin and present, vsync.
- `ui/platform/Input.h`: one event stream of buttons, sticks, touch,
  mouse and wheel.
- `ui/platform/System.h`: software keyboard, network address, free
  space, battery and clock.
- `ui/render/Renderer.h`: rects, rounded rects, rings, lines, textured
  quads, clip, and textures.

Fonts and icons are built on the Renderer's textures. Each interface has a
console backend (Vulkan + libnx) and a Linux host backend (the same Vulkan
renderer, on RADV on the dev PC). The host backend renders headless to PNG for tests
and mockup comparison, with an optional host-only window. Tests also use fake
Display and Input backends. **Only `ui/platform/` and `ui/render/` include
Vulkan or libnx headers**; `make check-ui` fails on any other include.
Backends are chosen at link time, never with `#ifdef` in screens or
components. Real launcher actions (FTP server, RootFS setup, guest shell,
logs) are called through one `LauncherBackend` interface, so screens never
call runtime code directly.

**Tech stack:** C++20, Vulkan 1.x on OunceVK (`/opt/devkitpro/portlibs/switch2`
`libvulkan.a`, `libouncevk.a`; host: system Vulkan loader), GLSL shaders
compiled to SPIR-V headers with `glslangValidator` (as hbmenu does), libnx hid
on libounce, FreeType + HarfBuzz (the `/opt/devkitpro/portlibs/switch` static
libs, which hbmenu also links on libounce), nanosvg, Lucide SVG icons, Nunito
Sans / JetBrains Mono / Noto Sans (OFL TTFs). Code adapted from ounce-hbmenu
(ISC) keeps its license header and is listed in `licenses.json`.

**Start condition:** milestone 4 is committed on `nx2`. Work on `nx2`;
commit once after console acceptance of this plan.

## Process

1. **One Sonnet implementation agent** does Tasks 1, 1.5 and 2–8 in order. After each
   task it writes `.superpowers/sdd/ui-taskN-report.md` and returns
   (status, one-line test summary, concerns); the orchestrator reports to
   the user and sends it on to the next task. No commits by the agent. No
   background processes left running.
2. **Console-gated tasks:** Task 1 (Vulkan UI probe) and Task 5 (download
   probe) end with a console run by the user; the next task starts only
   after it passes.
3. After Task 8, **one fresh Sonnet agent reviews the whole plan's diff**
   and writes one findings file ranked by severity.
4. The **Opus orchestrator** fixes the findings, runs the host screenshot
   set, compares it with the mockup, builds the console NRO and hands the
   user console runs until the acceptance checklist passes. Then it
   updates `NextMilestone.md` and commits once.

## Global constraints

- **Only the NRO is copied to the console.** Normal use (launcher, RootFS
  setup, FEX) must work with nothing copied into `sdmc:/fex/` or
  anywhere else on the SD card. Fonts, icons, `licenses.json`, license
  texts, the RootFS index and any manifest are built into the NRO (romfs
  or linked data). Everything else is created at runtime by the launcher
  (config, license files written out for the About page footnote) or
  downloaded (RootFS images). Staged SD files are allowed only for
  developer test bundles (`make filesystem-runtime` acceptance programs),
  never required for normal use.
- Spec rules hold verbatim: design units (du) on 1280×720,
  `s = output_height / 720`, edges and hairlines snapped to device pixels
  (1 du line = `max(1, round(s))` px), glyphs rasterized at `size × s`
  (atlas per size, never scaled bitmaps), AA rounded rects/rings/circles,
  icons rasterized from SVG at target size, vsync 60 fps, time-based
  animations (ms).
- Theming spec: one `Theme` struct per mode; **a hex color literal outside
  `ui/theme_dark.cpp` / `ui/theme_light.cpp` fails the build** (grep check
  in the Makefile `check-ui` target). Accent presets per theme as listed.
- Screens use tokens, components and the 2D layer only. No Vulkan or
  libnx include outside `ui/platform/` and `ui/render/` (checked by
  `make check-ui`).
- One NRO; per-launch test behavior in sidecar files, not build options
  (AGENTS.md). Single build tree `build/guest`, SD bundle `build/guest/sd`.
- **No faked behavior.** A feature without a working backend is drawn in
  the spec's disabled state (40% opacity, skipped by D-pad) with a muted
  reason line, for example: Launch Steam and Launch Desktop → subtitle
  "Needs graphics support · not available yet"; RootFS setup cards for
  Ubuntu 26.04, desktop environments and game stores → disabled
  with "Not available yet" (only Ubuntu 24.04 from the FEX RootFS SquashFS,
  Task 5, is selectable). The FTP "N clients" list shows only data `GuestFtp.h`
  really has; missing fields (speeds, per-client activity) are added to
  `GuestFtp.h` in Task 5, not invented.
- Never block the main thread; long work (install, FTP) keeps the existing
  cooperative model and reports progress to the UI per frame.
- Keep the existing SD log (`WriteLog`) and every existing launcher
  function working: FTP server, RootFS setup, guest shell, log level.
- SD root is `sdmc:/fex` everywhere in the UI (the launcher renames the
  legacy `/switch/fex` to `/fex` at start; the old location is legacy
  input only).
- Settings: keys in `sdmc:/fex/config/ui.json` only (unknown keys
  preserved, atomic temp+rename writes on leaving Settings). The runtime
  never reads FEX's `Config.json` (it sets its config in code), so the UI
  neither reads nor writes it. The settings list in the spec is examples
  only: rows mirror real project features only and come from a schema
  table; adding a row needs no layout code. No disabled placeholder rows
  for absent features (Steam, thunks, desktop, graphics, CPU emulation
  options are not listed until the project has them).
- No Nintendo, Steam, Valve, Epic, distro or KDE logos; the UI never says
  "Konsole". Bundled license texts are embedded in the NRO, written to
  `sdmc:/fex/licenses/` by the launcher, and the About list is
  generated from the embedded `ui/assets/licenses.json`.
- Every task adds host tests to `make check-ui` (Linux, headless), keeps all
  `check-guest-*` targets and `guest_nro` passing with 0 warnings, and
  does not bump the launcher version (the orchestrator does).

## File structure

```
Source/Switch2/ui/
  platform/Display.h, Input.h, System.h       platform interfaces (no Vulkan/libnx types in them)
  platform/switch2/          console backends: VI NWindow + Vulkan swapchain, libnx hid, swkbd/nifm/fs/psm/time
  platform/host/             Linux backends: headless Vulkan to PNG, optional window, scripted input, fixed System values
  platform/fake/             test backends (fake clock, fake display size, event scripts)
  render/Renderer.h          2D layer interface: FillRect, RoundRect, Ring, Line, Image, Clip, textures; scale s; pixel snapping
  render/vulkan/             Vulkan renderer (pipelines, SDF shaders, texture upload; adapted from hbmenu graphics_vulkan.c, ISC)
  render/shaders/*.vert/.frag  GLSL, compiled to SPIR-V headers at build time
  render/Fonts.h/.cpp        TTF load, glyph atlases per (font, size×s), text measure, ellipsis (end + middle)
  render/Icons.h/.cpp        nanosvg raster cache per (icon, size×s, color)
  theme.h, theme_dark.cpp, theme_light.cpp   tokens, accent presets, shell palettes
  input/InputModes.h/.cpp    over platform/Input.h: modes (controller/mouse/touch), repeat 400/90 ms, stick deadzone 0.5, gestures
  input/Focus.h/.cpp         focus tree, spatial nav (±45° cone), overrides, scroll-into-view
  components/*.h/.cpp        FocusRing, VersionBlock, IconButton, LaunchCard, PillButton, HintBar,
                             SettingsRow, Toggle, EnumChip, PathField, Slider, PickerModal,
                             ProgressBar, LogView, LicenseModal, ConfirmDialog
  screens/{home,settings,about,filepicker,ftp,rootfs,install,shell,gallery,swatch}.cpp
  App.h/.cpp                 screen stack, 150 ms cross-fade, per-screen state, dock/undock rescale
  Backend.h                  LauncherBackend interface (FTP, RootFS, install, shell, storage, versions)
  SettingsSchema.h/.cpp      schema table, ui.json persistence
  HostMain.cpp               Linux host: window or --headless --screen X --theme dark|light --out file.png
  assets/fonts/, assets/icons/, assets/licenses.json, assets/licenses/
  tests/*.cpp                host tests for make check-ui
Source/Switch2/probes/UiProbe.cpp             Task 1 console probe (EXPERIMENT, `make ui-probe`)
Source/Switch2/launcher/LauncherBackend.cpp   console implementation of Backend.h over existing runtime code
```

## Tasks

### Task 1: platform abstraction, Vulkan renderer and the console probe

The first attempt (SDL2) failed on the console; see Architecture. Remove
`ThirdParty/sdl2-switch2/`, the `.gitignore` negation for its `build.sh`
(repo root), the SDL2 parts of `probes/UiProbe.cpp` and of the Makefile
`ui-probe`/`ui-probe-host` targets, and the SDL2 prefixes under
`build/third-party/`. Keep `docs/ui/` (the specs), and the fetched fonts,
nanosvg and Lucide assets if they are still useful.

- Read ounce-hbmenu's `switch2/` first: `ouncevk_platform.c`,
  `graphics_vulkan.c`, `render.c`, `render_backend.h`, `render_scale.h`,
  `Makefile` (link line `-lvulkan ... -lnx`, shader compile rule), and
  `nx_main/nx_touch.c` / `nx_main/main.c` for input. Copied or adapted code
  keeps the ISC header.
- `ui/platform/Display.h`, `Input.h` and `System.h` as in "File structure",
  plus `ui/render/Renderer.h`. The headers contain no Vulkan or libnx types.
  Input events: buttons (with the controller id), both sticks, touch
  points (id, x, y, down/move/up), mouse (Joy-Con mouse: position,
  buttons, wheel if the SDK has it), and size changes.
- Console backends: Display over the default NWindow and a FIFO swapchain
  (recreated on a size change, as hbmenu does); Input over libnx hid
  (`padConfigureInput`, `hidGetTouchScreenStates`, the hid mouse state);
  System as far as the probe needs it (the clock). Renderer: Vulkan
  pipelines for solid quads, SDF rounded rects, rings and circles (AA in the
  shader), lines snapped to device pixels, textured quads (alpha and RGBA),
  scissor clip, texture create/update/destroy.
- Host backends: the same Vulkan Renderer on a headless VkImage, read back
  to PNG (`--headless --out file.png`); scripted Input; fixed System values.
  Fake backends for tests.
- `probes/UiProbe.cpp` draws only through the interfaces, so the probe
  tests the real abstraction: clear `#1B1B1F`, a rounded rect, a ring,
  a 1 du hairline, one line of Nunito Sans at 24×s (a temporary FreeType
  glyph upload is fine; the real Fonts module is Task 2), and an SVG icon
  via nanosvg. An on-screen readout shows the output size and `s`, the last
  button, touch points, mouse position, buttons and wheel, size changes and
  frame time. + exits cleanly. Log every step to `sdmc:/switch/fex-uiprobe.log`
  (append, run header, flushed per line, `ok`/`FAIL <step>: <VkResult or
  rc>`). `make ui-probe` gives `build/fex-switch2-uiprobe.nro` (EXPERIMENT);
  `make ui-probe-host` builds the host probe and writes a PNG.
- `make check-ui` (new): builds the host probe, renders it headless and
  checks a few pixels (background color, the rect edge snapped at s = 1 and 1.5);
  include check: Vulkan and libnx headers only in `ui/platform/` and
  `ui/render/`.
- Report: what works on the host; the console checklist for the probe.

**Task 1 console result (2026-11-25, probe 2, log `.superpowers/sdd/ui-probe2.log`):
passed.** The GPU is `Switch 2 GPU (NVK model 0x808)` with Vulkan 1.4. Docked output is 2560×1440 (s=2.0),
handheld is 1920×1080 (s=1.5), and dock and undock rescale without errors. Buttons, sticks, touch,
FreeType and the wall clock work, and Plus exits with rc=0. **The Joy-Con 2 mouse is not
reported** through the libnx hid mouse state (`mouse not seen yet` for the whole run).
Cause: the Joy-Con 2 mouse is reported through the Switch 2 **Nmouse** service,
not through the old `HidMouseState`. libounce has it: `switch2/services/nmouse.h`
(`hidInitializeNmouse`, `hidGetNmouseStatesWithDelta`, `hidGetNmouseTypeSet`, the types
JoyLeft, JoyRight and StandardMouse, `wheel_delta_y` from the Joy-Con stick, and flags bit 1 clear
when the sensor is not tracking) and the `PadPointer` helper in `switch/runtime/pad.h`
(`padPointerInitialize`, `padPointerUpdate`). Reference: kaizhar
`src/citra_switch/switch_platform.c` (github.com/wowilookgood/kaizhar, branch
kaizhar): it follows one Joy-Con until it has been still for 3 s, sums the deltas itself, skips
non-tracking samples, and reads the click from the pad's own ZL/ZR, because the mouse
buttons cannot tell L from ZL. Task 3 implements this in the console Input backend. Until then, it was listed as "not available yet", and the UI must be
fully usable with controller and touch alone. The acceptance checklist drops
"Joy-Con mouse only" until a working mouse source exists.

### Task 1.5: restructure the Switch2 source folder (no behavior change)

Runs after Task 1 (it touches the same Makefile and CMake files) and before
Task 2, so all UI work lands in the new layout. Today runtime headers,
host code, probes, tests, tool scripts and plans all sit in
`Source/Switch2/` or in the mixed `LinuxRuntime/` folder.

Target layout (move with `git mv` so history follows; file names stay
unless listed):

```
Source/Switch2/
  README.md, NextMilestone.md, CMakeLists.txt, Makefile, Toolchain.cmake, .gitignore
  launcher/     GuestProbe.cpp (the NRO main and text launcher), RootfsSetupHost.cpp, FexIcon.jpg
  runtime/      GuestRuntime.cpp/.h, GuestElf.cpp/.h and every Guest*.h, grouped:
    runtime/fs/       GuestRootFS, GuestInodeStore, GuestFileExtents, GuestFilePunch, GuestFileWrite,
                      GuestDirectoryRecovery, GuestPosixAcl, GuestInotify, GuestMounts, GuestRootfsSetup
    runtime/process/  GuestCredentials, GuestJobControl, GuestSignals, GuestTimers, GuestClocks,
                      GuestPreempt, GuestSched, GuestSeccomp, GuestNamespaces, GuestScript, GuestTerminal
    runtime/memory/   GuestAddressSpace, GuestArena, GuestMemoryFile
    runtime/ipc/      GuestFutex, GuestEpoll, GuestEventFd, GuestSysvIpc, GuestMqueue, GuestSocket, GuestInet
    runtime/          GuestRuntime.*, GuestElf.*, GuestLogBuffer, GuestFtp, GuestVideo
  host/         HostArena.cpp, HostDiskCache.cpp, HostMemory.cpp, HostNet.cpp/.h
  probes/       JitProbe.cpp, BindProbe.cpp, Milestone3Probe.cpp, UiProbe.cpp, HelloGuest.S/.ld (all EXPERIMENT probes)
  demos/        Dynamic/, Graphics/, Maze/, SDL2/ (unchanged contents)
  tests/unit/   the host unit tests (`*Test.cpp`, FtpServerHost.cpp, FtpTest.py)
  tests/guest/  guest-side suites and acceptance: *Suite.c, *Acceptance.c, *Probe.c, *Tools.sh,
                CoreutilsFilesystemProbe.sh, NameLimitProbe.c, ThreadIpcProbe.c
  tools/        FetchDebian.py, StageDebian.py, StageRootFS.py, RootfsManifest.py, CheckRootfsSetup.py,
                RunPackagedNative.sh
  docs/         every plan and inventory (*Plan.md, *Inventory.md, Milestone3Sdk.md, RemainingSyscalls.md)
  ThirdParty/   unchanged; ui/ is created by Task 2 in this layout
```

The groups are a guide. A header that clearly belongs elsewhere may move,
with the reason given in the report. Put an include path per folder in CMake and
the Makefile, or use relative includes; choose one way and use it
everywhere.

Done (Task 1.5): include paths. The Makefile (`INC`, `vpath %.h`) and
CMake (`target_include_directories`) put `runtime`, `runtime/{fs,process,memory,ipc}`,
`host` and the Switch2 root on one include path; sources include headers by bare
name (`"GuestArena.h"`) and `ThirdParty/...` from the root. New runtime headers go
in the matching `runtime/` subfolder; UI code in `ui/` joins the same path.

Rules:
- Pure move: no code or behavior change except include lines, build
  paths, script paths and doc links. Do not split `GuestRuntime.cpp`; that
  is not part of this task.
- Update every reference: `CMakeLists.txt`, `Makefile` (every target,
  including `filesystem-runtime`, the `check-*` targets and the probes),
  scripts that name other scripts or sources, `README.md`, the links in
  `NextMilestone.md` and the plans, `/home/limo/Documents/FEX/AGENTS.md`
  (it links `Source/Switch2/NextMilestone.md`, which stays where it is), and the
  "File structure" section of this plan.
- Delete stray generated files (`LinuxRuntime/__pycache__`) and add them to
  `.gitignore`.
- Done when: `cmake --build Source/Switch2/build/guest --target guest_nro`
  gives 0 warnings; the NRO is byte-identical in behavior (same launcher
  version, same sizes ±1%); every `check-guest-*`, `check-*-native` and
  `check-ui` target that existed before passes; `make filesystem-runtime`
  stages the same SD bundle (compare the file lists before and after);
  `make ui-probe`, `make bind-probe` and the JIT probe still build;
  `git status` shows renames (R) for the moved files, not delete and add.
  No console run is needed for this task.

### Task 2: fonts, icons, theme (on the Task 1 Renderer)

- `ui/render/Fonts` (FreeType + HarfBuzz shaping, glyph atlases per
  (font, size×s) as Renderer textures, measure, end and middle ellipsis) and
  `ui/render/Icons` (nanosvg raster cache per (icon, size×s, color)) use
  only `Renderer.h`; `ui/theme*`, fonts (download current OFL releases of
  Nunito Sans 400/600/700/800, JetBrains Mono 400/600/700, Noto Sans
  400/500; never older Apache-2.0 releases), Lucide icons (gamepad,
  monitor, terminal-square, gear/settings, chevron-right, chevron-down,
  plus, x, info, hard-drive, package, shopping-bag, folder, house,
  sd-card, arrow-up, pencil, eye, eye-off, trash, circle-slash,
  arrow-up-down transfer), nanosvg, their license texts.
- Spec text styles as named constants (`display`, `title`, … `kde_tab`),
  letter-spacing/tracking, line heights.
- `HostMain.cpp` headless PNG output; `screens/swatch.cpp` shows every token,
  every text style and every icon, dark and light.
- Tests: pixel snapping at s = 1, 1.5, 2, 3 (1 du line width, rect edges);
  text measure stable across scales (width × s within 1 px); middle
  ellipsis; theme token completeness (every token set in both themes);
  hex-literal grep check.

### Task 3: input, focus and shared components

- Console mouse in `ui/platform/switch2` Input: Joy-Con 2 mouse and USB mouse
  through Nmouse / `PadPointer` as described in "Task 1 console result" (deltas
  summed into a pointer position in output pixels, wheel from `wheel_delta_y`,
  click from the tracked Joy-Con's ZL/ZR; while it is the pointer, that trigger does not
  also act as a button). Log the Nmouse init result. The probe's Mouse line must show
  it; console checklist item: slide a Joy-Con as a mouse, click, scroll.
- `ui/input/`: mode switching rules (controller: any button or stick > 0.5;
  mouse: motion > 2 du or click, pointer 24 du, hidden after 3 s; touch:
  touch down, ring hidden), repeat 400 ms then 90 ms, spatial navigation
  with ±45° cone and distance ties, explicit overrides, scroll-into-view
  24 du margin 150 ms ease-out, tap < 12 du, long-press 500 ms, inertia
  0.92 per 16 ms, hit targets ≥ 44×44 du, hover focuses, left click = A,
  right click = B.
- Every component from the spec's "Shared components" table with exact
  geometry, states (normal, focused, pressed, disabled) and animations
  (ring fade 100 ms, card press 0.97 for 80 ms, toggle 120 ms, modal fade
  120 ms, progress ease 200 ms never backwards, cursor blink 530 ms,
  LogView 2,000-line ring buffer). Plus `ConfirmDialog` (PickerModal
  frame, 560–580 wide, Cancel focused by default, `accent` or `error` fill).
- `screens/gallery.cpp` shows every component in every state.
- Tests: spatial nav cases (grid, overrides, disabled skipped), repeat
  timing with a fake clock, mode switching, hit-test min size, tap vs
  drag threshold, progress never backwards, LogView ring buffer.

### Task 4: Home, Settings, About, File picker

- `Backend.h` with a fake host implementation for tests and headless
  screenshots (clearly named `FakeBackend`, host only).
- Home exactly as specified (header, title block, three cards at x = 164 /
  500 / 836 gap 56, FTP pill, HintBar, focus memory, Up → gear, gear
  Down → last card). Steam and Desktop cards disabled with the reason
  line. Shell card launches the guest shell.
- Settings: header, sidebar 300, scroll content, scroll-spy, smooth scroll
  200 ms, L/R sections, ZL/ZR page, right stick scroll, Restore defaults
  confirm, schema-driven rows (path, enum with Left/Right cycling, toggle,
  slider, link, destructive action; the extra row types are exercised by a
  test schema). Settings rows mirror real project features only: Paths
  (Reinstall RootFS, Wipe RootFS, Start folder = the launcher's
  `StartFolder`, folder picker), Appearance (theme, accent), Logging (log
  level Standard/Detailed/Trace). Sections shrink to what exists. Legacy
  `sdmc:/fex/fex-switch2.cfg` (log level, start folder) migrates into
  `ui.json` once (old `/switch/fex` location read as legacy input).
  Persistence: `ui.json` only, atomic writes, unknown keys kept. Home: with
  Steam and Desktop disabled, Left/Right skip disabled cards and focus
  stays on Shell (it never jumps to the FTP pill).
- About: app block, bundled and runtime-download sections from
  `licenses.json`, LicenseModal, full-screen license text view, footnote;
  license texts are embedded in the NRO and written to `sdmc:/fex/licenses/` by the launcher at first run (not staged).
- File picker: locations (microSD `sdmc:/`, FEX folder, RootFS `/`, Home),
  path bar with focusable breadcrumbs, sorted list (folders first, A–Z
  case-insensitive), filtering at 40%, hidden dot-files, L/R locations,
  ZL/ZR page, virtualized list, spinner after 150 ms, folder mode with
  + Use this folder, `sdmc:/…` vs `/…` path forms.
- Tests: schema → rows; persistence round trip (unknown keys preserved,
  atomic write); picker sorting/filtering/path forms; Home geometry (card
  x positions, pill position) from the layout code; headless PNGs of each
  screen in both themes.

### Task 5: RootFS images and installer (FEX RootFS squashfs)

User decisions (2026-11-25): RootFS images come from the
[FEX-Emu/RootFS](https://github.com/FEX-Emu/RootFS) project. **Ubuntu 24.04
is the only selectable RootFS.** The FEX project has no Debian image, so there
is no Debian option (the disabled Debian 12 card was removed 2026-11-26), and
the in-app Debian package setup is dropped from the setup flow. Audio and graphics
libraries are not added: they will come through the FEX layer.

Facts checked on 2026-11-25:
- Index: `http://rootfs.fex-emu.gg/RootFS_links.json`, format `{"v1": {"<name>":
  {DistroMatch, DistroVersion, URL, Hash, Type}}}`, `Type` `squashfs` or `erofs`.
  `Hash` is the hex XXH3-64 of the whole file (see
  `Source/Tools/FEXRootFSFetcher/XXFileHash.cpp`; xxhash is in `External/xxhash`).
- Ubuntu 24.04 SquashFS: `https://rootfs.fex-emu.gg/Ubuntu_24_04/2026-08-11/Ubuntu_24_04.sqsh`,
  Hash `3517e0e5ea25a473`, 525615104 bytes. **Plain HTTP works** (`http://...`, 200,
  no redirect), so the console's existing HTTP client (`RootfsSetupHost.cpp`, no TLS)
  can fetch it. The image is squashfs, **zstd** compressed (FEXRootFSFetcher
  requires zstd in unsquashfs).
- FEX images ship as a "broken chroot" (`Scripts/Ubuntu/break_chroot.sh` and
  `Scripts/chroot.py break` in that repo): `/etc/{hosts,resolv.conf,timezone,localtime,
  passwd,passwd-,group,group-,shadow,shadow-,gshadow,gshadow-,fstab,hostname,mtab,
  subuid,subgid,machine-id}`, the folders `boot home media mnt root srv tmp run`
  (and `opt` when empty), `var/cache/apt`, `var/lib/apt`, `var/tmp`, `var/run`,
  `var/lock` and `var/lib/dbus/machine-id` are moved into `/chroot/`; `/proc`, `/sys`
  and `/dev` are removed; `/usr/share/fex-emu` is removed; setuid/setgid files and
  sudo are deleted; apt lists are emptied. `unbreak_chroot.sh` is the reference for
  everyday use. Read both scripts and `Configs/Ubuntu_24_04.json` before coding.

Work:
- **Squashfs reader (no mount on the console):** `GuestSquashfs.h` reads a
  squashfs 4.0 image (superblock, id table, inode table, directory table,
  fragments, data blocks, xattrs as far as the inode store keeps them) with
  **zstd** decompression (vendor the BSD zstd decoder, `zstddeclib.c` single file, under
  `ThirdParty/zstd/` with its license; gzip/xz/lz4 images fail with a clear
  error). It streams entries (path, type, mode, uid, gid, mtime, size,
  symlink target, device numbers, hard links) into the existing rootfs
  extraction path, so files, symlinks, owners, modes and device nodes land
  through the inode store exactly as the tar path does. Reads the image
  from the SD card in bounded chunks (never the whole image in memory).
- **Everyday setup after extraction (the unbreak step, done by the
  launcher, no chroot):** move every `/chroot/` entry back to its place and
  remove `/chroot`; `chmod 1777 /tmp`; create `proc`, `sys`, `dev`, `dev/pts`
  mount points; create `/etc/passwd`, `group`, `shadow`, `gshadow` with `root`
  (0) and the default user (1000, home `/home/<user>`, shell `/bin/bash`, in
  `sudo`-less form) if the image has none; `/etc/hostname`, `/etc/hosts`
  (localhost entries), `/etc/resolv.conf` (the console's DNS, falling back to
  a public resolver), `/etc/machine-id`; do **not** add FEX's aarch64 host
  library mounts (no host libraries on the console). Record each step and
  any difference from `unbreak_chroot.sh` in `docs/ui/rootfs-image-format.md`.
- **Extra packages (only what our tests and tools need):** after the
  unbreak step, restore apt lists from `http://archive.ubuntu.com/ubuntu`
  (noble, noble-updates, noble-security; main, universe) over HTTP, and
  install the packages that the acceptance programs and tool scripts use
  but the image lacks (check `ContainerTools.sh`, `CommTools.sh`,
  `ProcessTools`/`ProcSuite` scripts, the filesystem acceptance script and the
  `FetchDebian.py` list against the image's dpkg status, for example
  `bubblewrap`, `util-linux` extras, `xz-utils`, `netcat-openbsd`), with
  `.deb` SHA-256 checked against the signed-by-hash `Packages` index, and
  dpkg status updated, reusing the package code of `RootfsSetupHost.cpp` /
  `GuestRootfsSetup.h`. The list lives in one table with a reason per package.
- **Index built into the NRO:** `ui/assets/rootfs-index.json` in the
  `RootFS_links.json` v1 format plus the fields the UI needs (download size,
  installed size, extra-package list), with the Ubuntu 24.04 SquashFS entry
  above. Optional
  "Check for newer images": fetch `http://rootfs.fex-emu.gg/RootFS_links.json`
  and offer a newer Ubuntu 24.04 SquashFS entry (same DistroMatch and
  DistroVersion); the download is verified with that entry's XXH3 hash. Over
  plain HTTP this checks integrity, not authenticity; record that.
- **Sources on the console:** download (resumable with HTTP Range, the image
  is stored under `/fex/.download/`, removed after a verified install) or a
  local `.sqsh` on the SD card picked with the File picker. The download
  probe (`make download-probe`, EXPERIMENT, log on SD) measures HTTP fetch,
  Range resume and throughput against the real image URL.
- **Installer pipeline** behind `Backend.h`: free-space check (download size
  + installed size), download, verify XXH3, extract, everyday setup, extra
  packages, write config (`ui.json`,
  `rootfs.json`). Progress and log lines feed the Installation screen;
  cancel at any step removes everything it created; a failed or cancelled
  install never leaves a RootFS the launcher treats as valid (marker file
  written last).
- Keep `make filesystem-runtime`, the prepackaged Debian developer bundle
  and every acceptance program working (developer test bundle only; the
  acceptance programs must also run from the Ubuntu RootFS).
- Tests (host): squashfs reader against images made by host `mksquashfs
  -comp zstd` (files, hard links, symlinks, device nodes, owners, modes,
  sparse and fragment files, long names) compared with `unsquashfs`;
  XXH3 of a file matches `FEXRootFSFetcher`'s; index parsing; resume logic;
  cancel cleanup; the everyday setup on a tree from the real Ubuntu 24.04
  image (downloaded to `build/`, not committed) gives a tree where
  `RunPackagedNative.sh` runs the acceptance programs.

### Task 6: FTP, RootFS setup, Installation, Shell screens

- FTP screen over the existing `GuestFtp.h` server: header toggle, address
  `ftp://<ip>:<port>`, five credential tiles (software keyboard via the
  swkbd applet for user/password, number pad for port 1024–65535, folder
  picker), password eye (Y on the focused tile), clients list with real
  data; add per-client activity and byte counters to `GuestFtp.h` if
  missing (1 Hz refresh, 3 s smoothing), disconnect confirm, Y start/stop
  anywhere, keep-running-in-apps toggle (honored only for launcher
  screens and the shell session unless more exists), random 8-character
  password on first run, keys in `ui.json`. The FTP server must keep
  running while launcher screens draw (cooperative, never blocking).
- RootFS setup (first-run when no RootFS, reinstall mode from Settings):
  sections and cards as specified; only the installer's real options are
  selectable, others disabled; free-space pill and footer sizes from real
  data; + Install disabled with `warn` text when space is short;
  selections in `rootfs.json`; cards and sizes come from the Task 5 index.
- Installation over the Task 5 installer pipeline: steps, weighted percentage, ETA
  (10 s rolling, "estimating…" for the first 3 s), LogView with the
  colored line rules, cancel confirm at every step leaving no partial
  RootFS, done and failed states. If the installer has fewer or different
  steps than the spec's five, show its real steps and record the difference.
- Shell: KDE-style tab strip (only tabs, close buttons, new-tab button),
  terminal surface drawing the existing `GuestTerminal.h` cell grid in
  `mono_term` with the Breeze palettes, OSC 0/2 tab titles, session
  banner, HintBar, L/R tabs, Y software keyboard, − exit (confirm while a
  process runs), last tab closed → Home. Multiple tabs only if the guest
  runtime can run several shell sessions; otherwise the new-tab button is
  disabled and this is recorded.
- Tests: FTP state model (start/stop, credentials validation, port range),
  install progress weighting and ETA, cancel cleanup with a fake backend,
  terminal cell grid size (≈ 138×25 at 1280×720), tab strip layout.

### Task 7: app shell, console integration, staging

- `App`: screen stack and flow from the spec's navigation diagram, 150 ms
  cross-fade, per-screen state kept, dock/undock rescale within one frame
  (recompute `s`, rebuild atlases, keep focus and scroll), external
  session hand-off hook (unused until graphics exists).
- `LauncherBackend.cpp` implements `Backend.h` over the existing runtime;
  `GuestProbe.cpp` `main` creates the App instead of `consoleInit` and the
  `Launcher` class; remove the text menu only once every function it had
  is reachable from the UI. Log lines that `PrintStage` showed on screen
  go to the LogView of the active install/shell screen and the SD log.
- CMake/Makefile: link the Task 1 platform and Vulkan renderer (`-lvulkan` from the switch2 portlibs, shaders as SPIR-V headers) into `guest_nro`; embed
  fonts, icons, licenses and the RootFS index in the NRO (romfs via
  `elf2nro --romfsdir` or linked data); a test proves the launcher starts
  and reaches RootFS setup with an empty SD card; add
  `make check-ui` to the documented verification commands.
- Tests: headless flow test with `FakeBackend` (first run → setup →
  install → Home → Settings → back, focus restored); every screen PNG in
  both themes at 1280×720, 1920×1080 and 3840×2160 under
  `build/ui-shots/`.

### Task 8: pseudo-terminals and shell tabs (user request 2026-11-25)

Goal: the Shell screen is a real terminal: several tabs, each a Bash
session on its own pseudo-terminal, all in one guest runtime, as on Linux.
Task 8a added devpts and the pty syscall path. Task 8b wires the launcher
tabs to one guest worker with an independent host-held master per tab.
Task 8c added terminal emulation and keyboard mappings (see
`TerminalInventory.md`); 8d/8e fixed the console findings. **Status:
complete, console-accepted on 0.17.15 (2026-11-26):** shell tabs, sudo,
job control, Root tab, games test programs beside shells, pty-suite and
pty-suite-root 43/0/0, X overlay, tab controls, footer and R3 help.
Open in the backlog: apt-get update closing the app (not reproduced on
0.17.15), Tab-completion and apt speed (performance work).

- **Runtime ptys (Linux semantics, `runtime/`):** `/dev/ptmx` opens a
  master and allocates index N; `/dev/pts/N` is the slave (listed in
  `/dev/pts` while it exists, mode 0620, owner the opener, group tty 5);
  `/dev/pts/ptmx` too. ioctls: `TIOCGPTN`, `TIOCSPTLCK`/`TIOCGPTLCK`
  (slave open EIO while locked), `TIOCGPTPEER`, `TCGETS`/`TCSETS`/
  `TCSETSW`/`TCSETSF` (and the termios2 forms), `TIOCGWINSZ`/`TIOCSWINSZ`
  (SIGWINCH to the foreground process group on change), `TIOCSCTTY`,
  `TIOCNOTTY`, `TIOCGPGRP`/`TIOCSPGRP` (SIGTTOU rules), `TIOCGSID`,
  `FIONREAD`, `TCFLSH`, `TCXONC`, `TIOCOUTQ`. One line discipline per pty,
  reusing the console's (ICANON editing, ECHO/ECHOE/ECHOK/ECHOCTL, ISIG:
  VINTR/VQUIT/VSUSP to the foreground group, ICRNL/INLCR/IGNCR,
  OPOST/ONLCR). Controlling terminal per session (setsid, first open
  without O_NOCTTY), SIGHUP + SIGCONT to the session when the master
  closes, slave reads EIO after hangup, master reads EIO when no slave is
  open. poll/select/epoll readiness on both ends. Background reads/writes:
  SIGTTIN/SIGTTOU with Linux's orphaned-group and ignored/blocked rules.
  Document every syscall/ioctl in ContainerInventory.md (or a new
  TerminalInventory.md) with Partial/Complete.
- **Concurrent runtime and UI:** while the Shell screen exists, the guest
  runtime runs on its worker thread and the UI keeps drawing on the main
  thread. The launcher owns each tab's master end; bytes cross between
  threads through locked queues (no other runtime state is touched from
  the UI thread). New tab: the runtime, at a safe point between scheduler
  passes, starts `/bin/bash -l` as a new session (setsid, slave as stdin/
  stdout/stderr and controlling terminal, `TERM=xterm-256color`, size
  from the tab's TerminalGrid). Closing a tab closes its master (hangup).
  The last tab closed ends the runtime and returns to Home. Remove the
  "one shell at a time" limit: `MoreShells()` true, new-tab button
  enabled, OSC 0/2 titles per tab. Use + Settings from Shell to open
  a root shell or a developer ELF in the current tab strip. Single-block
  JIT and recovery probes require a dedicated runtime. FTP resumes after
  the guest worker finishes and releases the inode store.
- **Terminal emulation:** TerminalGrid handles what Bash, readline, less,
  nano/vim and apt's progress bar use: CSI cursor movement, erase,
  insert/delete line and char, scroll regions, SGR 16/256/truecolor,
  alternate screen (1049), bracketed paste, application cursor keys,
  DECTCEM; resize sends TIOCSWINSZ. Controller/software keyboard and the
  hardware keyboard (if the platform reports one) produce the xterm byte
  sequences, including Ctrl+C/Z/D.
- **Tests:** a guest pty suite (`tests/guest/PtySuite.c`, built like the
  other suites, run natively on Linux first, then on the console) covering
  openpty/posix_openpt, termios round trips, canonical and raw reads,
  echo, signals from VINTR/VSUSP to the foreground group, window size and
  SIGWINCH, controlling terminal rules, hangup on master close, poll.
  A shell script run from the Shell: job control (`sleep 30 &`, `jobs`,
  `fg`, Ctrl+Z, `bg`), `apt-get update` without the `/dev/pts` error,
  `script -qc true /dev/null`. Host `check-ui`: TerminalGrid escape
  handling and multi-tab Shell screen with FakeBackend.
- Open question recorded, not part of this task: guest threads stay on
  the runtime's one cooperative scheduler (no host thread per guest
  thread).

## Acceptance (orchestrator)

- Host: `make check-ui` passes; `build/ui-shots/` matches the mockup
  within 2 du per edge and per color token (orchestrator compares); crisp
  at 1080p and 4K.
- Console checklist: banner version; Home renders crisp handheld and
  docked; dock/undock on every screen keeps state; every screen usable
  with controller only, Joy-Con mouse only (Nmouse, Task 3), touch only; settings survive
  restart and reach the launcher (log level, start folder); FTP start/stop and a client transfer
  from a PC; RootFS setup + install (or reinstall) completes and cancel
  leaves no partial RootFS; Shell runs Bash in several tabs (job control, apt without the /dev/pts error, PtySuite passes); process, comm, filesystem and
  container acceptance programs still pass from the Shell.
