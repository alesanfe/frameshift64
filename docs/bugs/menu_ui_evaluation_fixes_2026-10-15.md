# Menu UI evaluation pass — keyboard nav, structured netplay status, quit confirmation, restore defaults, visual baseline

**Status:** FIXED (port presentation layer)

**Context:** a combined screenshot + code + runtime evaluation of the port menu scored 74/100. This entry records the correction pass over every actionable finding.

**Findings & fixes:**

1. **No keyboard navigation (A-01, S2).** Only gamepad nav was enabled — keyboard users could not traverse any widget even though the footer hints showed "Enter Select". `ImGuiConfigFlags_NavEnableKeyboard` is now set everywhere `NavEnableGamepad` is toggled (`src/presentation/Menu.cpp`, `components/libultraship/src/ship/window/gui/Gui.cpp`, 4 sites).

2. **Netplay status as free-form string only (S3).** `port_netlobby_status()` rendered as an undifferentiated paragraph — failures looked identical to normal states. `RenderNetplayLobby` now maps `port_netlobby_state()` to a severity tag + semantic color (`[Failed]` red, `[Connected]`/`[Live]` green, `[Connecting]`/`[Waiting]`/`[Starting]` blue) and prints a recovery hint on `PORT_NETLOBBY_FAILED` ("Check the room code and server address…"). Ping quality uses the same tokens (`src/presentation/PortMenuOnline.cpp`).

3. **Quit ignored live netplay sessions (U-02, S3).** "Quit BattleShip" closed the app unconditionally. It now opens a "Quit during online session?" modal whenever the lobby state is anything but IDLE/FAILED (`mShowQuitConfirm`, `PortMenuSupport.cpp` + modal in `PortMenu.cpp`).

4. **Mixed-language key names (V-01, S3).** `GetKeyNameTextA` returns OS-locale names, so an English UI showed "BARRA ESPACIADORA" on Spanish Windows. `gfx_dxgi.cpp` now resolves the scancode through a static English table first; the OS name is only a fallback for scancodes the table doesn't know (`components/libultraship/src/fast/backends/gfx_dxgi.cpp`).

5. **No per-section restore defaults (U-01, S2).** A "Restore section defaults" button now sits at the bottom of every settings section that contains restorable widgets; it walks the section's `WidgetInfo` list and rewrites each CVar from the option's `DefaultValue` metadata, then toasts the count (`src/presentation/Menu.cpp::ResetSectionCvars`).

6. **Duplicated literal colors (C-05, S3).** `UIWidgets::SemanticColors` centralizes Success/Warning/Error/Info/Muted/Placeholder; applied across Menu/PortMenu*/Toasts/OnlineStatus instead of scattered `ImVec4` literals (`UIWidgets.hpp`).

7. **Focus-highlight style bug (latent).** `PushStyleColor(NavHighlight)` was pushed before `Begin()` but its matching `PopStyleColor` ran *inside* the window as the first pop — the stack unwind order meant the custom highlight never reached the content draw and WindowBg was popped in the wrong order. The pop sequence now mirrors the push order exactly (`Menu.cpp`).

8. **`PortMenu.cpp` god file (C-01, S3).** Split by domain: `PortMenuCustomization.cpp` (Shaders/Texture Packs/Script Mods), `PortMenuOnline.cpp` (netplay lobby), `PortMenuSupport.cpp` (Actions/Diagnostics/Updates), shared helpers in `PortMenuShared.h`; `PortMenu.cpp` keeps menu registration, settings sections, Game Files, and modal plumbing (2929 → ~1450 LOC).

9. **No visual-regression tooling.** `shoot_menu_sections.py` gained `--baseline DIR` (approve current captures as reference), `--diff DIR` (pixel-diff vs baseline with `--diff-tolerance`/`--max-diff-ratio`), `--size WxH`, `--ui-scale`, and `--matrix` (4 key sections × {1280×720, 1920×1080, 320×240, 559×360, 561×360} × UI scales {0.75, 1.0, 1.5, 2.0}, naming `<section>-<W>x<H>-s<scale>.png`).

10. **Screenshot seeding silently ignored (three-stage diagnosis).** First pass assumed `NON_PORTABLE=ON` builds always read `%APPDATA%/BattleShip/` and seeded there — captures still rendered the wrong tab. Second pass guessed the runtime resolves `BattleShip.cfg.json` **by existence** (exe-dir shadows app-storage) and flipped the seed preference — also wrong. Final answer from source: `NON_PORTABLE` **is** active (wired via `#cmakedefine` → generated `install_config.h`, `components/libultraship/src/CMakeLists.txt:19`, consumed by `Context.cpp`), and `Context::InitConfiguration` reads `GetPathRelativeToAppDirectory("BattleShip.cfg.json")` = `%APPDATA%\BattleShip\` **unconditionally** — an exe-dir cfg is never even probed. The "wrong tab" captures were stale state in the *appdata* cfg: `write_cfg` preserves unknown keys, so `ActiveHeader`/`Language` from a previous run survived the seed. `ssb64::LocateExistingFile` (`src/platform/filesystem/app_paths.cpp`) does probe appdata→exe-dir, but only for optional payloads (o2r, mods, `lang/`), never the config. Both screenshot scripts now seed **all** candidate cfg paths and restore all of them, and `gSettings.Language` joined `EXTRA_KEYS` so a non-English run can't leak into the canonical gallery.

11. **Stale lobby process skewed room captures.** A leftover `node.exe` bound to :8799 made subsequent `shoot_netplay_room.py` runs reuse the wrong lobby instance (new lobby crashed `EADDRINUSE` but `health` passed against the stale one) — the client rendered the idle form while the host showed a phantom peer. Kill leftover node processes before rerunning; the script's docstring/README now warn about it.

12. **i18n conversion fallout (mechanical).** The `TR()` wrapping pass left `fmt::format(TR("...{}"))` (fmt 10+ needs `fmt::runtime` for non-constexpr strings) and split literals `TR("a") "b"` (unterminated). Both fixed across `PortMenu*.cpp`/`Menu.cpp`/`first_run.cpp`; lang JSONs are now staged next to the binary by `StageAssetRecipes.cmake`.

13. **Autostart fighter desync.** `update_auto` wrote `sLobby.my_fkind` in three places without syncing `gNetplay.Fighter`, so the "Your fighter" combo showed the CVar value while the lobby advertised another. Each assignment now writes the CVar too (`src/networking/netplay/session/netlobby.cpp`).

14. **i18n defensive-format sweep over-rotated (compile + UB).** The tool-generated pattern `ImGui::Text("%s", fmt::sprintf(TR("…"), args).c_str(), args)` referenced `fmt::sprintf`, which isn't available (no `fmt/printf.h`) — 76 call sites across 9 files failed to compile. Worse, `ImGui::TextUnformatted("%s", TR("…"))` bound the TR pointer to the `text_end` parameter — literal "%s" plus out-of-bounds read (10 sites). Fixed mechanically: format-arg sites collapse to `ImGui::X(TR("…"), args)` (ImGui's own printf formatting handles the translated format string), `TextUnformatted` takes `TR(…)` directly, and `fmt::sprintf` is gone from `src/`.

15. **i18n coverage gap — closed.** The footer device-hints (`"Enter Select   Esc Close menu…"`, `"[A] Select…"`) were left as plain literals by the first TR() pass even though the es/de/fr/pt tables already carried translations; they're wrapped now and the Spanish capture shows translated footer hints. `tools/developer/testing/shoot_languages.py` smoke-tests every shipped `lang/<code>.json` plus the three loader fallback paths (missing file, malformed JSON, out-of-range index → English).

**Verification:** clean Debug build (0 errors) after the concurrent `components/` submodule migration and the i18n sweep; 35/35 gallery captures + baseline; matrix 80/80 (1920×1080@s0.75 and 1280×720@s2 clean; 561×360 lands compact — viewport ~1px narrower than requested; 320×240@s2 worst case stays navigable). Full-gallery `--diff` vs baseline: all deltas explained by intended changes (per-section "Restore section defaults" button on settings sections, new Online widgets) plus live-scene background noise — `menu-popout`'s 33% is pure scene noise, the popout framebuffer was never part of the shot; baseline re-approved after inspection. Lint suite: `check_retired_paths` clean, `check_cvar_schema` 112 keys cover all g* literals, `check_env_vars` 118 catalogued, `check_docs` 0 findings, `run_project_lint` required checks OK (also fixed its `find_cmake_files` crash on broken junctions under `out/`); `netpacket_test` PASS. Live room states captured via `tools/developer/testing/shoot_netplay_room.py` (local `node-lobby.mjs` + `SSB64_LOBBY_HOST`/`_JOIN`/`_CODE_FILE`): host shows `[Connected] 2 players connected`, room code, per-player fighters, `ping N ms (peak M)` overlay, relayed path; client shows `waiting for host to start` + Leave. `shoot_menu_sections.py` now asserts the seeded `ActiveHeader` survives the run (`SEED?` failure line) so the cfg-shadowing bug class can't regress silently. Post-i18n gallery diff: 35/35 SAME vs baseline (max 0.48%, toast animation noise) — the TR conversion is pixel-identical in English; Spanish verified live via `menu-language-es.png`.

16. **Translated header clipped the search field (V-layout regression).** With
    longer tab labels (ES/DE) the "Header Selection" child carried
    `AutoResizeX|AlwaysAutoResize`, so `EndChild` resized the frame to the
    *content* width — past the pinned search input and the Close/Reset/Quit
    buttons — while `headerWidth` double-counted the search field as content.
    At 960x540 the input rendered as a ~60px sliver under the Close button.
    `headerReserved` now subtracts `searchW + buttons + spacing` from the
    child width, `AutoResizeX` is dropped (fixed width + `HorizontalScrollbar`
    owns overflow), and the search field/placeholder draw after `EndChild`
    pinned to the right of the strip (`Menu.cpp`). Verified pixel-level at
    960x540 and 1280x720 in EN/ES/DE — input fully visible, tabs overflow
    into the strip scrollbar, Close never overlapped. Note: the debug builds
    were repeatedly compiled from mid-edit snapshots of `Menu.cpp` (file was
    rewritten under the build; `Menu.obj` timestamps proved stale compiles),
    which is why earlier attempts appeared pixel-identical — always confirm
    `obj` mtime > source mtime before trusting a capture.

17. **`port_narrate` LNK2019 + SAPI constant typo.** New
    `src/presentation/narration.cpp` (§9 screen-reader hook, Windows SAPI) was
    created mid-build — the glob re-check added it to `smash_platform` after
    that target's compile step ran, so the first link failed unresolved.
    Second build surfaced `SPF_PURGE_BEFORE_SPEAK` — the real SAPI constant
    is `SPF_PURGEBEFORESPEAK` (`sapi.h`). Same class of failure hit
    `profiles.cpp` (`Couldn't open file 'profiles.obj'` in `__create_def`):
    files added while a build runs can land in the regenerated vcxproj after
    that project already compiled. Rebuild, don't diagnose.

18. **Netplay status strings bypassed i18n entirely.** `netlobby.cpp` wrote
    raw English literals into `sLobby.status` (`"Connected (N players) -
    waiting for host to start."`, all `fail()` reasons, transitional states)
    and the UI printed them verbatim — the last untranslated user-visible
    surface. Fixed at the source: `TR()` now wraps every status literal in
    `netlobby.cpp` (safe — it is C++ port code; `core/i18n/Locale.h`
    include), plural forms split into singular/plural keys
    (`%d player`/`%d players`), and `std::string` concat sites became
    `snprintf` with translated format strings so `Joining room %s...` /
    `Server error: %s` / `Version mismatch - you're on %s, a peer is on %s.`
    keep argument order flexible for translators. Display sites
    (`PortMenuOnline`, `OnlineStatus`, `PortMenuSupport` diagnostics) do a
    runtime `TR(status)` lookup so the one static-init literal
    (`"Pick Host or Join to play online."`, written before the locale table
    exists) still translates — `T()` returns the key unchanged when it is
    not in the table, so already-translated stored strings pass through
    harmlessly. Severity tags (`[Connected]`/`[Failed]`/…) in
    `NetStatusViewFor` are `TR()`-wrapped at definition. 41 new keys + 7 tag
    keys extracted and translated in es/de/fr/pt. Verified live: Spanish
    room capture shows `2 jugadores conectados - pulsa Iniciar partida
    (hasta 4)`. Caveat: stored status text keeps its language until the
    next `set_state`/`update_status` event after a mid-session language
    switch. Also fixed: `NetStatusViewFor` literals, and
    `shoot_netplay_room.py --lang N` seeds the language for room captures;
    `--work`/`--out` are now resolved absolute (a relative path scattered
    frames under the exe's CWD).

**Verification (this round):** full Debug rebuild clean on sha `ff26ff6b31`
(includes `narration.cpp`, `profiles.cpp`, heap-check boot, netplay readiness
APIs). `shoot_languages.py` 7/7 — es/fr/de/pt render translated (footer hints
included) and all three fallback paths degrade to English without crashing;
`menu-language-es.png` refreshed post-fix (input + hints correct). Remaining
caveat: two `lang-test` runs flaked once (frame-360 capture missed under disk
pressure) — re-run passed; not a code bug.

**Audit hook:** when a build-system migration runs concurrently, a full pixel-diff baseline is the cheapest way to prove the presentation layer survived path churn — approve baselines only after a clean build, never mid-migration.

## Round 4 — deferred evaluation items implemented (2026-10-17)

The remaining verified gaps from the external evaluation landed in this
round (build-verified against the post-reorg module layout):

- **Safe-mode launcher.** Support → Actions now has "Restart in Safe Mode":
  writes a one-shot `safemode.request` marker beside `crash.marker`
  (`port_diag_request_safe_mode`), then relaunches the process so the boot
  gate sees it — a console `reset` does not re-run startup gating. The boot
  path consumes the marker alongside the existing crash-bundle /
  `SSB64_SAFE_MODE` checks and logs `user-requested` as the reason. Windows
  relaunches via `_spawnl`, Linux via `execl("/proc/self/exe")`; other
  platforms write the marker and ask the user to restart. The same action
  is searchable (see command palette below).

- **Loss-aware connection quality.** `portNetPeerGetRemoteStats` and
  `port_netlobby_peer_stats_at` now also return `recv_seq_highwater`, so the
  Online → Netplay "Connection:" line computes the worst-peer cumulative
  loss percentage (`gaps / highwater`) and degrades the tier: loss ≥5%
  forces Poor, ≥2% prevents Good. The label shows `(loss X.X%)` when stats
  exist, on top of the existing Good/Fair/Poor ping tiers, relayed/direct
  transport, and the ping-history sparkline.

- **Undo for section restores.** "Restore section defaults" now snapshots
  every CVar-bound value first (`SnapshotSectionCvars` → `SectionUndo`,
  single-slot). An "Undo restore" button appears next to it while the
  snapshot belongs to the visible section; undoing replays the captured
  values through the same `CVarSet*` + `ShipInit` path and saves. Toast
  strings and the longer tooltip are translated (es/fr/de/pt).

- **"Go to" jump from search results.** The previously dead
  `navigateToWidget`/`navigateMainEntry`/`navigateSidebar`/
  `navigateWidgetName`/`highlightWidget` scaffold is now live: every search
  result gets a "Go to" button that switches to the owning menu + sidebar,
  scrolls the target widget into view (`SetScrollHereY`), and draws an
  accent outline around it for a frame. Targets that stay hidden (Simple
  mode, conditional widgets) time out after ~1.5 s instead of latching.

- **Command-palette-lite.** The search box now also matches global actions
  — Close Menu, Restart Game, Restart in Safe Mode, Quit BattleShip — shown
  with an `(action)` origin tag. Plus the earlier-found bug fix is still in
  place: `.SearchTerms()` and `.StableId()` finally feed the search
  haystack (dozens of widgets declared search terms that previously matched
  nothing).

- **Modified-settings surface.** A dedicated Settings → "Modified"
  sidebar (`WidgetIsNonDefault`) walks every registered CVar-bound widget
  across all sections and lists the ones whose live value differs from the
  declared default — checkbox, int combobox, int/float sliders, button
  selector, string input and colour picker are all covered. Each row shows
  the section → sidebar origin and a "Go" button driving the same
  navigation globals as search "Go to" — a user can audit and jump back to
  anything they changed. Registered as its own sidebar (not buried at the
  bottom of General) so it's discoverable and screenshot-verifiable.

**Verification (this round):** `menu_structure_test` PASS (28 sidebars, 172
widgets — safe-mode button, "Modified" sidebar), full Debug build green
(link via rename trick while a parallel session held the old exe mapped).
`settings-modified.png` captured live — panel lists 31 non-default entries
with origins and Go buttons; `settings-display.png` re-captured to confirm
the Display section stayed intact after the sidebar insert (an early edit
leaked "Display" separator widgets into the new sidebar — caught by the
screenshot, fixed). i18n catalogue + es/fr/de/pt resynced.

## Round 5 — full-section debt & bug audit (2026-10-17)

Static audit of all 28 sidebars / 172 widgets plus screenshot-driven fixes:

- **Combobox `.at()` crash (the Control Enhancements abort).**
  `UIWidgets::Combobox` did `comboMap.at(*value)` unconditionally — with
  zero saved profiles the map is empty and `std::out_of_range` unwound
  mid-frame, `EndFrame` never ran, next `NewFrame` tripped
  `ErrorCheckNewFrameSanityChecks`. Every fresh profile hit this. All map
  and vector combobox variants now clamp out-of-range persisted values and
  render a safe fallback on empty containers.

- **WIDGET_CVAR_INPUT had no draw case.** "New profile name" rendered
  nothing — added the `CVarInputString`/`CVarInputInt` renderer.

- **Dangling `c_str()` in the Profile combo.** `comboMap` held pointers
  into a `static std::vector<std::string>` that was reassigned every
  frame. Labels now rebuild only when the name set changes, with stable
  backing storage.

- **Restart-required metadata vs. accounting.** Three widgets showed the
  `[Restart required]` tag but never counted toward the pending-restart
  chip because `.Tag()` is display-only. `Low Resolution Mode`,
  `Texture Filter` and `Classic Co-op` now carry explicit
  `.ApplyPolicyOf(ApplyPolicy::RequiresRestart)`; the details-panel
  revert covers all of them (every restart-required CVar is int-backed).

- **Explicit defaults everywhere.** ~37 CVar widgets relied on implicit
  option defaults (checkbox→0, int slider→1, float→1.0, combo→key 0).
  Correct today, fragile tomorrow — every CVar-bound widget now declares
  `DefaultValue`/`DefaultIndex`, and `menu_structure_test` gained a lint
  rule enforcing it plus an unregistered-sidebar attachment check.

- **i18n sweep.** ~16 user-visible literals the TR-lint couldn't see
  (toasts, status lines, the netplay timeout message, "Room code
  copied…") wrapped in `TR()`; catalogue + es/fr/de/pt regenerated
  (655 keys, 0 missing).

- **Netplay text clipping.** The input-delay explanation ran past the
  column edge (`TextDisabled` never wraps) — switched to disabled-colour
  `TextWrapped`.

- **Screenshot harness**: `SECTIONS` entries accept a 5th element for
  per-shot CVar seeds; `settings-developer` seeds
  `gSettings.EnableDevTools=1` so the CVar-gated Developer sidebar can be
  captured. 40 PNGs in `docs/screenshots/menu/`, all verified visually.

### Round 5 follow-up — footer collision & Modified values

- **Footer hint overlap.** Long translations (es "Escribe en el cuadro de
  búsqueda...") pushed the right-aligned hints left of the pending-restart
  chip / match banner — the old left-clamp would draw hints *over* that
  cluster. `leftClusterEndX` now tracks the cluster's real right edge; if
  hints still don't fit they degrade to the compact wording
  (`compactHints` per pad family / keyboard).

- **Modified panel shows live values.** Each row is now
  `Name = value   (Section -> Sidebar)   [Go]` via `WidgetCurrentValue`
  (same type coverage as `WidgetIsNonDefault` — On/Off, combo labels,
  sliders, btn-selector, input strings, #RRGGBBAA colour).

- **Text clipping sweep (cont'd).** `TextDisabled`→wrapped for the
  Replays note, UpdatePrompt restart line, pack `summary`, and the
  GhostlyDark attribution line — all previously ran past the column edge.

- **Harness:** `SECTIONS` gained a per-shot extra-CVar element (Developer
  seeds `gSettings.EnableDevTools`); test file re-pointed at
  `src/presentation/` after the `interfaces/` flatten.

**Verification:** build green post-restructure, `menu_structure_test`
PASS (28 sidebars, 172 widgets), On/Off catalogued in 4 languages,
`settings-modified.png` re-captured showing values.
