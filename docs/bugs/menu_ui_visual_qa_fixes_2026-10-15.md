# Menu UI visual QA pass — checkbox fill, missing glyphs, compact layout overflow, screenshot config path

**Status:** FIXED (port presentation layer)

**Symptoms (found by screenshot-based QA of `docs/screenshots/menu/`):**

- Unchecked checkboxes rendered as solid accent-colored squares — indistinguishable from a "selected" state at a glance.
- Em dashes (`—`), curly quotes, and ellipses in labels/tooltips rendered as `?` boxes.
- Several labels carried redundant "(Needs reload)" text next to the `[Restart required]` tag.
- Float sliders showed raw `printf("%f")` output (`10.000000`).
- The Accessibility sidebar opened on a group separator misleadingly titled "Interface".
- At small window sizes the header tabs collided with the Close/Reset/Quit buttons, the sidebar left no room for content, and the 3-line footer strip ate half of a 240px-high window.
- The screenshot capture script silently wrote the wrong config file: every "section" shot showed Settings → General at the default 640×480.

**Root causes & fixes:**

1. **Checkbox frame.** `UIWidgets::PushStyleCheckbox` pushed the accent color at alpha 1.0 into `ImGuiCol_FrameBg` unconditionally, so the off state filled solid. The unchecked frame now uses a subdued fill with an accent border; the checkmark still only draws when the value is true (`src/interfaces/presentation/UIWidgets.cpp`).

2. **Glyph coverage.** Menu fonts were built with the default Latin-1 atlas. The standard font now bakes General Punctuation (0x2010–0x205F: dashes, quotes, ellipsis, bullets) alongside the default ranges (`src/interfaces/presentation/Menu.cpp`).

3. **Label/formatting cleanup.** Dropped "(Needs reload)" suffixes wherever a `.Tag("Restart required")` already exists (`PortMenu.cpp`, `Menu.cpp`); `FloatSliderOptions::format` default changed `"%f"` → `"%.4g"` (`UIWidgets.hpp`) — `10`, `0.85`, `10.05` instead of trailing-zero noise; explicit `.Format()` overrides still win.

4. **Accessibility grouping.** Re-sectioned as Visual / Text & Help / Motion instead of a stray "Interface" separator (`PortMenu.cpp`).

5. **Compact layout.** Below ~560px of menu width the vertical sidebar becomes a horizontal scrolling strip above a full-width content column, the header row is constrained to the space actually left by the trailing buttons and scrolls to the active tab, and the footer collapses to a single hint line (the 3-line help strip was consuming ~150px of a 240px window). Active sidebar/strip entries scroll into view for the first few frames after open — `SetScrollHere*` can't resolve on frame 1 because the child's `ScrollMax` isn't computed yet (`Menu.cpp`).

6. **Extraction status card.** Game Files → Extraction now renders a bordered status card (Ready/Missing, archive name + size, region, `.bak` backup presence) via `LocateExistingFile` — probing app-dir first had it reporting "Missing" while the archive sat next to the exe (`PortMenu.cpp`).

7. **Screenshot config path.** `tools/developer/testing/shoot_menu_sections.py` wrote seeds to `%APPDATA%/BattleShip/BattleShip.cfg.json`, but this build is portable (`NON_PORTABLE` unset → `GetAppDirectoryPath` returns `"."`) and reads `<exe dir>/BattleShip.cfg.json`. The script now prefers the exe-dir config — section selection, window size, and per-shot CVars actually apply. Bonus fix: `Window.Width`/`Height`/`Fullscreen.Enabled` are written for a uniform 960×540 (320×240 for `menu-compact`), and `window-online-status` was dropped from auto-capture (only meaningful mid-session).

**Audit hook:** For capture tooling on LUS-derived ports, verify *where* the config actually resolves (`GetAppDirectoryPath`) before trusting seeded state — a silent portable/NON_PORTABLE mismatch produces screenshots of the default state while every shot "succeeds". Visual diffs (or here, uniform 640×480 PNG sizes) are the tell.
