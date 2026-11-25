# Launcher UI: implementation plan

> **For agentic workers:** follow `AGENTS.md` and the process below. Each
> task is one Sonnet agent, one after another. The task text and the
> "Global constraints" are your requirements; the two spec files are the
> source of truth for every number.

**Goal:** replace the text-console launcher (`consoleInit` + `Launcher`
class in `GuestProbe.cpp`) with the GPU-rendered UI described in
`docs/ui/fex-switch2-ui-spec.md` and `docs/ui/fex-switch2-theming-spec.md`
(copied into the repo from `~/Downloads` by Task 1).

**Architecture:** a thin 2D layer (`ui/render/`) over SDL2's GLES2
renderer draws everything; screens use only components and theme tokens.
The same UI code builds for the console (SDL2 rebuilt for libounce and the
Switch 2 EGL/GLES2 portlibs) and for the Linux host (system SDL2), where a
headless mode renders any screen to PNG for tests and mockup comparison.
Real launcher actions (FTP server, RootFS setup, guest shell, logs) are
called through one `LauncherBackend` interface so screens never call
runtime code directly.

**Tech stack:** C++20, SDL2 2.28.5 (devkitPro switch-sdl2 port sources),
FreeType + HarfBuzz (through SDL2_ttf 2.20+), nanosvg, Lucide SVG icons,
Nunito Sans / JetBrains Mono / Noto Sans (OFL TTFs), libounce.

**Start condition:** milestone 4 is committed on `nx2`. Work on `nx2`;
commit once after console acceptance of this plan.

## Process

1. **One Sonnet implementation agent** does Tasks 1–7 in order. After each
   task it writes `.superpowers/sdd/ui-taskN-report.md` and returns
   (status, one-line test summary, concerns); the orchestrator reports to
   the user and sends it on to the next task. No commits by the agent. No
   background processes left running.
2. **Console-gated tasks:** Task 1 (SDL2 probe) and Task 5 (download
   probe) end with a console run by the user; the next task starts only
   after it passes.
3. After Task 7, **one fresh Sonnet agent reviews the whole plan's diff**
   and writes one findings file ranked by severity.
4. The **Opus orchestrator** fixes the findings, runs the host screenshot
   set, compares it with the mockup, builds the console NRO and hands the
   user console runs until the acceptance checklist passes. Then it
   updates `NextMilestone.md` and commits once.

## Global constraints

- **Only the NRO is copied to the console.** Normal use (launcher, RootFS
  setup, FEX) must work with nothing copied into `sdmc:/switch/fex/` or
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
- Screens use tokens, components and the 2D layer only. No SDL call
  outside `ui/render/` and `ui/input/`.
- One NRO; per-launch test behavior in sidecar files, not build options
  (AGENTS.md). Single build tree `build/guest`, SD bundle `build/guest/sd`.
- **No faked behavior.** A feature without a working backend is drawn in
  the spec's disabled state (40% opacity, skipped by D-pad) with a muted
  reason line, for example: Launch Steam and Launch Desktop → subtitle
  "Needs graphics support · not available yet"; RootFS setup cards for
  Ubuntu 26.04, desktop environments and game stores → disabled with
  "Not available yet" (only what `GuestRootfsSetup.h` really installs is
  selectable). The FTP "N clients" list shows only data `GuestFtp.h`
  really has; missing fields (speeds, per-client activity) are added to
  `GuestFtp.h` in Task 5, not invented.
- Never block the main thread; long work (install, FTP) keeps the existing
  cooperative model and reports progress to the UI per frame.
- Keep the existing SD log (`WriteLog`) and every existing launcher
  function working: FTP server, RootFS setup, guest shell, log level.
- Settings: app keys in `sdmc:/switch/fex/config/ui.json`, FEX keys merged
  into FEX's `Config.json` (unknown keys preserved), atomic temp+rename
  writes on leaving Settings. The settings list in the spec is examples
  only: rows come from a schema table; adding a row needs no layout code.
  Only rows whose setting the runtime really honors are enabled; the rest
  render disabled with "Not available yet".
- No Nintendo, Steam, Valve, Epic, distro or KDE logos; the UI never says
  "Konsole". Bundled license texts are embedded in the NRO, written to
  `sdmc:/switch/fex/licenses/` by the launcher, and the About list is
  generated from the embedded `ui/assets/licenses.json`.
- Every task adds host tests to `make check-ui` (Linux, headless), keeps all
  `check-guest-*` targets and `guest_nro` passing with 0 warnings, and
  does not bump the launcher version (the orchestrator does).

## File structure

```
Source/Switch2/ui/
  render/Renderer.h/.cpp     2D layer: FillRect, RoundRect, Ring, Line, Text, Image, Clip; scale s; pixel snapping
  render/Fonts.h/.cpp        TTF load, glyph atlases per (font, size×s), text measure, ellipsis (end + middle)
  render/Icons.h/.cpp        nanosvg raster cache per (icon, size×s, color)
  theme.h, theme_dark.cpp, theme_light.cpp   tokens, accent presets, shell palettes
  input/Input.h/.cpp         modes (controller/mouse/touch), repeat 400/90 ms, stick deadzone 0.5, gestures
  input/Focus.h/.cpp         focus tree, spatial nav (±45° cone), overrides, scroll-into-view
  components/*.h/.cpp        FocusRing, VersionBlock, IconButton, LaunchCard, PillButton, HintBar,
                             SettingsRow, Toggle, EnumChip, PathField, Slider, PickerModal,
                             ProgressBar, LogView, LicenseModal, ConfirmDialog
  screens/{home,settings,about,filepicker,ftp,rootfs,install,shell,gallery,swatch}.cpp
  App.h/.cpp                 screen stack, 150 ms cross-fade, per-screen state, dock/undock rescale
  Backend.h                  LauncherBackend interface (FTP, RootFS, install, shell, storage, versions)
  SettingsSchema.h/.cpp      schema table, ui.json + Config.json persistence
  HostMain.cpp               Linux host: window or --headless --screen X --theme dark|light --out file.png
  assets/fonts/, assets/icons/, assets/licenses.json, assets/licenses/
  tests/*.cpp                host tests for make check-ui
Source/Switch2/ThirdParty/sdl2-switch2/      build script + patches for SDL2/SDL2_ttf on libounce
Source/Switch2/UiProbe.cpp                    Task 1 console probe (EXPERIMENT, `make ui-probe`)
Source/Switch2/LauncherBackend.cpp            console implementation of Backend.h over existing runtime code
```

## Tasks

### Task 1: SDL2 on Switch 2 and the console probe

- Copy both spec files into `docs/ui/`.
- Build SDL2 2.28.5 from the devkitPro switch-sdl2 sources (the switch
  video, input, audio and thread drivers) against **libounce** and the
  **switch2** portlibs (`/opt/devkitpro/portlibs/switch2`: libEGL,
  libGLESv2, libglapi, libdrm_nouveau), not against libnx and the Switch 1
  Mesa. Also build FreeType, HarfBuzz and SDL2_ttf for switch2 (or prove
  the switch1 static libs link and run, and record why). Script it in
  `ThirdParty/sdl2-switch2/build.sh`; install into a prefix under
  `build/` (never into `/opt/devkitpro` silently; document if the user must
  install). Record every patch needed and why.
- `UiProbe.cpp` → `build/fex-switch2-uiprobe.nro` (`make ui-probe`,
  labeled EXPERIMENT), logging every step to `sdmc:/switch/fex-uiprobe.log`
  (append, run header): SDL_Init video+gamecontroller+events, window at
  the output size, GLES2 renderer, clear to `#1B1B1F`, a rounded rect, one
  line of Nunito Sans rendered through SDL2_ttf at 24×s, an SVG icon via
  nanosvg, and an on-screen readout of: output size and `s`, last
  controller button, touch points, mouse position and buttons (Joy-Con
  mouse), mouse wheel events (record whether the SDK exposes them),
  dock/undock size changes, frame time. + exits cleanly.
- Host: the same probe builds for Linux with system SDL2.
- Report: what works on the host; the console checklist for the probe.

### Task 2: render layer, theme, fonts, icons

- `ui/render/`, `ui/theme*`, fonts (download current OFL releases of
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
  slider, link, destructive action), disabled rows. Paths section starts
  with Reinstall RootFS and Wipe RootFS rows (confirm dialogs as specified).
  Wire only settings the runtime honors today (at least log level, which
  `Launcher::LoadSettings` stores now; migrate its file into `ui.json`);
  other schema rows disabled. Persistence: `ui.json` + FEX `Config.json`
  merge, atomic writes, unknown keys kept.
- About: app block, bundled and runtime-download sections from
  `licenses.json`, LicenseModal, full-screen license text view, footnote;
  license texts are embedded in the NRO and written to `sdmc:/switch/fex/licenses/` by the launcher at first run (not staged).
- File picker: locations (microSD `sdmc:/`, FEX folder, RootFS `/`, Home),
  path bar with focusable breadcrumbs, sorted list (folders first, A–Z
  case-insensitive), filtering at 40%, hidden dot-files, L/R locations,
  ZL/ZR page, virtualized list, spinner after 150 ms, folder mode with
  + Use this folder, `sdmc:/…` vs `/…` path forms.
- Tests: schema → rows; persistence round trip (unknown keys preserved,
  atomic write); picker sorting/filtering/path forms; Home geometry (card
  x positions, pill position) from the layout code; headless PNGs of each
  screen in both themes.

### Task 5: RootFS images and installer (after the FEX RootFS project)

Redesign the Debian setup after [FEX-Emu/RootFS](https://github.com/FEX-Emu/RootFS)
(read its builder configs, `RootFS_links.json` and FEXRootFSFetcher in
`Source/Tools/FEXRootFSFetcher` of this repo first) instead of staging
loose `.deb` files from a PC.

- **Image builder (Linux host):** `LinuxRuntime/RootfsBuilder/` with one
  config per image (distro, release, architecture amd64, package list,
  post-install steps: users `root` and the default user, locale,
  `/etc/hostname`, apt sources, dpkg database, FEX thunk config,
  removal of docs/man/locales not needed). First image: **Debian 13
  "trixie"** base; packages that the current `FetchDebian.py` list and the
  acceptance tools need stay available. Build without root (mmdebstrap
  `--mode=unshare` or the existing fetch + extract path), reproducible
  (fixed snapshot date, sorted tar, fixed mtimes).
- **Image format the console can install:** the console cannot mount
  squashfs/EROFS, so the image is a chunked `tar.xz` (or `tar.zst` if a
  decoder is added) with a per-chunk SHA-256, which the existing
  `GuestRootfsSetup.h` tar/xz code extracts into the SD rootfs with the
  inode store. Record the format in `docs/ui/rootfs-image-format.md`.
- **Index:** `rootfs-index.json` like `RootFS_links.json`, **built into
  the NRO** (an optional newer index may be downloaded, but only one signed
  or hash-pinned by the built-in index is trusted): id, name,
  version, base, download size, installed size, chunk URLs, SHA-256,
  minimum launcher version, optional components (desktops, stores) with
  their sizes. RootFS setup and the "≈ X / ≈ Y" footer read sizes from it;
  cards appear only for entries in the index (Ubuntu, desktops, stores
  stay disabled until an image provides them).
- **Sources on the console:** download from the URLs in the built-in
  index (default; no files copied by the user). Optional: a local image on
  the SD card chosen with the File picker, for offline installs. Add a download probe
  (`make download-probe`, EXPERIMENT, logs to SD) that tests HTTP and
  HTTPS fetch through the console's network and ssl services, resume of
  partial downloads and throughput; if HTTPS is not available, record it
  and support only HTTP + SHA-256 verification against an index pinned in
  the NRO.
- **Installer pipeline** behind `Backend.h`: check free space, download
  (resumable, verified per chunk), extract, install thunk libraries, write
  config (FEX `Config.json` `RootFS`, `ui.json`, `rootfs.json`). Progress
  and log lines feed the Installation screen; cancel at any step removes
  everything it created; a failed or cancelled install never leaves a
  RootFS the launcher treats as valid (marker file written last).
- Keep `make filesystem-runtime` and every acceptance program working
  (they may move into the image or stay as an add-on staged on top).
- Tests: index parsing and validation, chunk verification failure, resume
  logic, cancel cleanup, builder reproducibility (two builds, same hash),
  native run of the acceptance programs from the built image via
  `RunPackagedNative.sh`.

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
- CMake/Makefile: link the Task 1 SDL2 build into `guest_nro`; embed
  fonts, icons, licenses and the RootFS index in the NRO (romfs via
  `elf2nro --romfsdir` or linked data); a test proves the launcher starts
  and reaches RootFS setup with an empty SD card; add
  `make check-ui` to the documented verification commands.
- Tests: headless flow test with `FakeBackend` (first run → setup →
  install → Home → Settings → back, focus restored); every screen PNG in
  both themes at 1280×720, 1920×1080 and 3840×2160 under
  `build/ui-shots/`.

## Acceptance (orchestrator)

- Host: `make check-ui` passes; `build/ui-shots/` matches the mockup
  within 2 du per edge and per color token (orchestrator compares); crisp
  at 1080p and 4K.
- Console checklist: banner version; Home renders crisp handheld and
  docked; dock/undock on every screen keeps state; every screen usable
  with controller only, Joy-Con mouse only, touch only; settings survive
  restart and reach `Config.json`; FTP start/stop and a client transfer
  from a PC; RootFS setup + install (or reinstall) completes and cancel
  leaves no partial RootFS; Shell runs Bash; process, comm, filesystem and
  container acceptance programs still pass from the Shell.
