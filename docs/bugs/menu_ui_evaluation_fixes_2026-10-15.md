# Menu UI evaluation pass — keyboard nav, structured netplay status, quit confirmation, restore defaults, visual baseline

**Status:** FIXED (port presentation layer)

**Context:** a combined screenshot + code + runtime evaluation of the port menu scored 74/100. This entry records the correction pass over every actionable finding.

**Findings & fixes:**

1. **No keyboard navigation (A-01, S2).** Only gamepad nav was enabled — keyboard users could not traverse any widget even though the footer hints showed "Enter Select". `ImGuiConfigFlags_NavEnableKeyboard` is now set everywhere `NavEnableGamepad` is toggled (`src/interfaces/presentation/Menu.cpp`, `components/libultraship/src/ship/window/gui/Gui.cpp`, 4 sites).

2. **Netplay status as free-form string only (S3).** `port_netlobby_status()` rendered as an undifferentiated paragraph — failures looked identical to normal states. `RenderNetplayLobby` now maps `port_netlobby_state()` to a severity tag + semantic color (`[Failed]` red, `[Connected]`/`[Live]` green, `[Connecting]`/`[Waiting]`/`[Starting]` blue) and prints a recovery hint on `PORT_NETLOBBY_FAILED` ("Check the room code and server address…"). Ping quality uses the same tokens (`src/interfaces/presentation/PortMenuOnline.cpp`).

3. **Quit ignored live netplay sessions (U-02, S3).** "Quit BattleShip" closed the app unconditionally. It now opens a "Quit during online session?" modal whenever the lobby state is anything but IDLE/FAILED (`mShowQuitConfirm`, `PortMenuSupport.cpp` + modal in `PortMenu.cpp`).

4. **Mixed-language key names (V-01, S3).** `GetKeyNameTextA` returns OS-locale names, so an English UI showed "BARRA ESPACIADORA" on Spanish Windows. `gfx_dxgi.cpp` now resolves the scancode through a static English table first; the OS name is only a fallback for scancodes the table doesn't know (`components/libultraship/src/fast/backends/gfx_dxgi.cpp`).

5. **No per-section restore defaults (U-01, S2).** A "Restore section defaults" button now sits at the bottom of every settings section that contains restorable widgets; it walks the section's `WidgetInfo` list and rewrites each CVar from the option's `DefaultValue` metadata, then toasts the count (`src/interfaces/presentation/Menu.cpp::ResetSectionCvars`).

6. **Duplicated literal colors (C-05, S3).** `UIWidgets::SemanticColors` centralizes Success/Warning/Error/Info/Muted/Placeholder; applied across Menu/PortMenu*/Toasts/OnlineStatus instead of scattered `ImVec4` literals (`UIWidgets.hpp`).

7. **Focus-highlight style bug (latent).** `PushStyleColor(NavHighlight)` was pushed before `Begin()` but its matching `PopStyleColor` ran *inside* the window as the first pop — the stack unwind order meant the custom highlight never reached the content draw and WindowBg was popped in the wrong order. The pop sequence now mirrors the push order exactly (`Menu.cpp`).

8. **`PortMenu.cpp` god file (C-01, S3).** Split by domain: `PortMenuCustomization.cpp` (Shaders/Texture Packs/Script Mods), `PortMenuOnline.cpp` (netplay lobby), `PortMenuSupport.cpp` (Actions/Diagnostics/Updates), shared helpers in `PortMenuShared.h`; `PortMenu.cpp` keeps menu registration, settings sections, Game Files, and modal plumbing (2929 → ~1450 LOC).

9. **No visual-regression tooling.** `shoot_menu_sections.py` gained `--baseline DIR` (approve current captures as reference), `--diff DIR` (pixel-diff vs baseline with `--diff-tolerance`/`--max-diff-ratio`), `--size WxH`, `--ui-scale`, and `--matrix` (4 key sections × {1280×720, 1920×1080, 320×240, 559×360, 561×360} × UI scales {0.75, 1.0, 1.5, 2.0}, naming `<section>-<W>x<H>-s<scale>.png`).

10. **Screenshot script wrote the wrong config again (follow-up to `menu_ui_visual_qa_fixes`).** `app_cfg_path` preferred `<exe>/BattleShip.cfg.json` whenever the file existed, but the verify preset builds `NON_PORTABLE=ON`, so the app actually reads `%APPDATA%/BattleShip/`. Captures silently showed the user's last interactive section instead of the seeded one. The script now reads `NON_PORTABLE:BOOL` from the build's `CMakeCache.txt` to pick the real config path.

**Verification:** clean Debug build (0 errors) after the concurrent `components/` submodule migration; 35/35 gallery captures regenerated under `docs/screenshots/menu/` + baseline set in `docs/screenshots/baseline/`; matrix captures in `docs/screenshots/matrix/`.

**Audit hook:** when a build-system migration runs concurrently, a full pixel-diff baseline is the cheapest way to prove the presentation layer survived path churn — approve baselines only after a clean build, never mid-migration.
