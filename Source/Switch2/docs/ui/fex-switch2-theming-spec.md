# FEX for Switch 2 — Theming Spec (Dark + Light)

Nov 23, 2026 · @Julian Vogel

> **For Claude Code:** this extends `fex-switch2-ui-spec.md`, which stays the source of truth for geometry and input. Light mockups sit on the canvas below the dark ones: https://claude.ai/artifact/7pCfQfj1AbmsKUK68p897V.

## Approach

Ship one `Theme` struct per mode (dark, light) holding every color as a semantic token; screens and components read tokens only, so adding light mode changes no layout, size, font or behaviour. This spec extends the main UI design spec, which stays the source of truth for geometry and input; the light mockups sit on the canvas next to the dark ones.

- **Changes per theme:** every color, the accent value, scrim opacity, shell (terminal) palette.
- **Never changes:** sizes, radii, spacing, fonts and weights, icon shapes, animation timing, focus behaviour.
- **Rule:** a hex literal outside `theme_dark.cpp` / `theme_light.cpp` fails the build (grep check in CI).
- **Inversions to watch:** button-hint glyphs swap (dark disc with light letter in light mode); check marks on the accent disc use `on_accent`, not `bg`; the Shell's close icon and inactive tab text use their own shell tokens.
- **Contrast floor (both themes):** text 4.5:1 against the surface it sits on, focus ring and toggle tracks 3:1 against the background. The dark accent #00C3E3 fails on light surfaces, so light mode uses its own accent.

## Launcher tokens

Every launcher color resolves through this table. Light values are tuned to the contrast floor above; ratios are against the surface each token is used on.

| Token | Dark | Light | Light contrast / note |
| --- | --- | --- | --- |
| `bg` | #1B1B1F | #EBEBEF | Screen background |
| `surface` | #2A2A30 | #FFFFFF | Cards, modal, selected sidebar item |
| `surface_raised` | #2C2C32 | #FFFFFF | Pills, gear button, chips (sit on `bg`) |
| `surface_inset` | #34343B | #E4E4EA | Card icon tiles, selected modal option, tags |
| `field` | #26262B | #E4E4EA | Path value chip |
| `log_bg` | #121215 | #F7F7F9 | Installation log, license source box |
| `divider` | #34343A | #D2D2D8 | Rules, row separators, progress track |
| `text` | #F2F2F4 | #1B1B1F | 14.4:1 on `bg` |
| `text_muted` | #A6A6AE | #5E5E66 | 5.4:1 on `bg`, 6.4:1 on `surface` |
| `text_field` | #DCDCE2 | #2E2E34 | 10.7:1 on `field` |
| `text_dim` | #8A8A94 | #6E6E78 | 4.7:1 on `log_bg` |
| `accent` (default) | #00C3E3 | #006A84 | 5.2:1 on `bg`, 6.2:1 on `surface` |
| `on_accent` | #1B1B1F | #FFFFFF | Check marks on accent discs |
| `hint_disc` / `hint_letter` | #F2F2F4 / #1B1B1F | #1B1B1F / #EBEBEF | Button glyphs invert |
| `toggle_off_track` | #4A4A52 | #7C7C86 | 3:1 non-text on `bg` |
| `toggle_off_knob` | #C9C9D0 | #FFFFFF | |
| `toggle_on_knob` | #FFFFFF | #FFFFFF | |
| `radio_off` | #6E6E78 | #8A8A94 | 3.4:1 on `surface` |
| `button_secondary` | #3A3A42 | #DCDCE2 | Cancel / Close pills in modals |
| `scrim` | #000000 @ 62% | #000000 @ 35% | Modal backdrop |
| `ok` | #4ADE80 | #15803D | 4.7:1 on `log_bg` |
| `warn` | #FBBF24 | #B45309 | 4.7:1 on `log_bg` |
| `error` | #F87171 | #B91C1C | 6.1:1 on `log_bg` |

**Accent presets.** The user picks a hue, and each theme maps it to its own value.

| Preset | Dark | Light |
| --- | --- | --- |
| Cyan (default) | #00C3E3 | #006A84 |
| Red | #E60012 | #C8102E |
| Violet | #7C5CFF | #5B3FE0 |
| Blue | #3DAEE9 | #1F6FB2 |

No shadows or gradients in either theme: light-mode cards separate from `bg` by value alone (white on #EBEBEF), matching the flat dark look.

## Shell palette

The Shell follows the launcher theme: Breeze Dark in dark mode, a Breeze Light–style palette in light mode. Its HintBar uses launcher tokens. Values are design choices tuned for contrast, not copied from KDE's files.

| Token | Dark | Light | Use |
| --- | --- | --- | --- |
| `kde_window` | #202326 | #EFF0F1 | Screen base |
| `kde_header` | #2A2E32 | #DEE0E2 | Tab strip |
| `kde_border` | #141618 | #C4C9CD | Tab strip bottom border |
| `kde_view` | #232627 | #FCFCFC | Terminal, active tab |
| `kde_text` | #FCFCFC | #232629 | Terminal foreground, active tab label |
| `kde_text_inactive` | #BDC3C7 | #57626D | Inactive tab, close icon |
| `kde_focus` | #3DAEE9 | #1E80BD | Active tab top bar, banner line |
| ANSI green | #1CDC9A | #0B7558 | Prompt user, ok |
| ANSI blue | #1D99F3 | #1A73C2 | Directories, cwd |
| ANSI yellow | #FDBC4B | #A15C00 | Strings, warnings |
| ANSI red | #ED1515 | #C0392B | Errors |

- Apps that set their own 256-color or truecolor values are passed through unchanged in both themes.
- The cursor uses `kde_text`; the selection highlight uses `kde_focus` at 30% over `kde_view`.

## Theme setting and switching

A new **Appearance** section in Settings (between Desktop and CPU emulation) holds two rows; both are already drawn in the dark and light Settings mockups.

| Label | Type | Options | Default | Stored as |
| --- | --- | --- | --- | --- |
| Theme | enum | Match console, Dark, Light | Match console | app `theme` |
| Accent color | enum | Cyan, Red, Violet, Blue | Cyan | app `accent` |

- **Match console:** read the console's system theme through the platform layer at boot and on resume from sleep. If the platform exposes no theme, fall back to Dark. Treat this as an open question until the Switch 2 homebrew SDK confirms an API.
- **Applying:** load the theme from `ui.json` before the first frame, so there is no dark flash on a light console.
- **Switching live:** snapshot the current frame, swap the `Theme` pointer, then cross-fade snapshot → new frame over 200 ms. Focus, scroll and modal state are kept. Glyph atlases are color-independent (coverage only), so nothing is re-rasterized.
- **Accent:** stored as a preset name, not a hex, so switching theme picks that preset's value for the new theme.
- **Running sessions** (Steam, desktop) keep their own themes; only the launcher and the Shell chrome follow this setting.

## Migration and acceptance

If the dark build follows the main spec (tokens only), adding light mode takes about a day.

1. Replace `theme.h` constants with a `struct Theme` and a `const Theme* g_theme`; add `on_accent`, `hint_disc`, `hint_letter`, `button_secondary`, `error` and the toggle knob tokens.
2. Move dark values into `theme_dark.cpp`; add `theme_light.cpp` from the tables above.
3. Add the CI grep that rejects hex literals outside the two theme files.
4. Add the Appearance rows to the settings schema; wire `theme` and `accent` to `ui.json`.
5. Implement boot-time loading, live switching with the 200 ms cross-fade and console-theme detection.
6. Add a theme toggle to the component gallery screen so every component can be checked in both modes.

### Acceptance checklist

- [ ] Light screenshots of Home, Settings, About, RootFS setup (first run, reinstall and its confirmation), wipe confirmation, FTP server, File picker, Shell and Installation match the light mockups within 2 du and every token.
- [ ] Every text/background pair in both themes meets 4.5:1; focus ring, toggle tracks and radio rings meet 3:1.
- [ ] Switching theme on any screen, including with a modal open, keeps focus and scroll and shows no unthemed frame.
- [ ] Booting with Light saved, or Match console on a light console, shows light from the first frame.
- [ ] No hex literal exists outside `theme_dark.cpp` / `theme_light.cpp`.
