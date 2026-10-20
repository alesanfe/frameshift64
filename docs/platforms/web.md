# BattleShip — Web (WASM) build

Experimental browser port. The full game (decomp + libultraship + port
layer) compiles to WebAssembly; rendering goes through WebGL2 (`USE_OPENGLES`
is forced), coroutines run on Emscripten fibers (`-sASYNCIFY`), and netplay
rides the WebSocket/relay transports only (browsers have no UDP).

## Requirements

- Emscripten SDK. A local checkout lives at `out/emsdk/` (created via
  `git clone https://github.com/emscripten-core/emsdk + emsdk install latest`
  — gitignored). Any emsdk install works; the preset just needs a toolchain
  file path.
- `ninja` on PATH.

## Build

```bash
cmake --preset web-wasm
cmake --build out/build/web-wasm --target ssb64   # BattleShip.{html,js,wasm}
cmake --build out/build/web-wasm --target torch   # torch/torch.{js,wasm} — in-browser extractor
```

Outputs land in `out/build/web-wasm/`:

- `BattleShip.html` — the game page (built from `platforms/web/shell.html`)
- `BattleShip.js` + `BattleShip.wasm` — engine + game code
- `torch/torch.js` + `torch/torch.wasm` — in-browser ROM→o2r extractor
  (embind `Companion` API + the `Extract` entry point — wired into the
  page's ROM picker, see below)

## Run locally

Browsers refuse wasm from `file://`, and this is a **threaded build**:
pthreads need `SharedArrayBuffer`, which browsers only expose to
cross-origin-isolated pages — so the server MUST send COOP/COEP headers
or the module stalls forever on `loading-workers`. Use the bundled dev
server (fails fast if the port is already bound):

```bash
python platforms/web/serve.py out/build/web-wasm 8931
# → http://localhost:8931/  (redirects to /BattleShip.html)
```

`serve.py` also sends the security headers the product needs:
`Cross-Origin-Resource-Policy: same-origin`, `X-Content-Type-Options:
nosniff`, `Referrer-Policy: no-referrer`, a restrictive
`Permissions-Policy`, and a Content-Security-Policy
(`script-src 'self' 'unsafe-inline' 'unsafe-eval' blob:` —
the shell's inline loader plus `unsafe-eval`, required by the two
eval-ing runtime paths: EM_ASM blocks (kbproxy flag, lobby-code publish)
and `emscripten_run_script_string` (the URL→env bridge)).

The loader gates on capabilities (WebAssembly, `crossOriginIsolated`,
WebGL2, IndexedDB) and reports an actionable typed error code
(`WASM_UNSUPPORTED`, `CROSS_ORIGIN_ISOLATION_MISSING`, …) instead of
hanging when something is missing.

## Asset flow (first run on web)

The page defers `main()` until `BattleShip.o2r` exists in MEMFS:

1. IDBFS is mounted at `/persist` and synced in, then the process cwd is
   `chdir`'d into it — LUS resolves its app dir to `.`, so the derived o2r
   **and every save/config/mods/crash file the game writes** land in
   persisted storage automatically.
2. Otherwise the page tries `fetch('BattleShip.o2r')` — drop a pre-extracted
   archive next to the html when hosting to skip the picker.
3. Otherwise an upload button (or drag-and-drop anywhere on the page)
   accepts either a desktop-extracted `BattleShip.o2r` **or the ROM itself**
   (`.z64`/`.n64`/`.v64`). The picker shows a client-side ROM header sniff
   (name / cartridge code / region / size — first 64 bytes only) before
   extracting, and the result is flushed to IDBFS immediately, not on
   pagehide.

In-browser ROM → o2r extraction runs in **`extract_worker.js`** — a module
Web Worker that imports the ES6 `torch.js` module and runs
`Companion::Process` off the page thread, so the tab stays responsive
during the multi-second extract and the job is cancellable via
`worker.terminate()` (a fresh worker per attempt leaves no half-state).
The worker stages the recipe tree into Torch's own MEMFS from
`asset-recipes/` (the `files.txt` manifest is emitted at configure time),
receives the ROM bytes as a transferred `ArrayBuffer`, calls the embind
`Extract(romPath, srcDir, destDir)` entry point — paths, not data, cross
embind — and transfers the produced `BattleShip.o2r` back.
Companion validates the ROM hash against `config.yml`, so a foreign file
fails with a typed error code (`EXTRACT_FAILED`, `NO_O2R`,
`RECIPE_MANIFEST_MISSING`, …) and the picker stays open. The ROM never
leaves the browser — every step is local, and the picker screen says so.

Once assets are staged the page shows a **Start BattleShip** button:
`callMain` runs inside that user gesture so the `AudioContext` boots
running instead of suspended (autoplay policy). `?autostart=1` skips the
gate for CI rigs (`pw_lobby_smoke.js` uses it).

## PWA / offline / storage

The page is an installable PWA:

- `manifest.webmanifest` + `icons/` (generated from
  `assets/branding/icons/icon.png`) — standalone, landscape, dark theme.
- `sw.js` is stamped at link time (`stamp_sw.cmake`) with the wasm
  SHA-256, so any real rebuild yields a byte-different service worker and
  wakes the browser's update check. It precaches the shell, wasm, torch
  module, icons and the `asset-recipes/` tree (offline first-run
  extraction works). **User-derived files (`*.o2r`/`*.z64`/…) never enter
  the Cache API** — they live in IDBFS only.
- Updates are user-gated: a new SW stays `waiting`, the page offers
  "Restart to apply", and `SKIP_WAITING` is posted only on that gesture —
  never mid-game.

The gear button (top-right) opens the storage panel:
`navigator.storage.estimate()` usage/quota bar, a "request persistence"
action (`navigator.storage.persist()`), "clear game assets" which deletes
only the regenerable `.o2r` from `/persist` (saves/config/mods survive),
**save/settings export & import** (JSON `.bssave` bundle covering every
`/persist` file except derived `*.o2r`/ROM formats — path-traversal-safe
import that flushes IDBFS), and a diagnostics copy button producing a
JSON bundle (codes, UA, capabilities, storage estimate — no ROM/save
content).

Error screens carry typed codes plus a "Restart in safe mode" action that
reloads with `?SSB64_SAFE_MODE=1` (consumed by `port.cpp`, skips mods via
the existing env bridge).

WebGL context loss is handled at the canvas level:
`webglcontextlost` → `preventDefault()` (required for any restore
attempt) + visible "recovering" status; `webglcontextrestored` →
"restored". The loss count lands in the diagnostics bundle.

## Threaded vs single-thread builds (docs §4)

Two official wasm variants exist because Emscripten cannot degrade a
pthreads binary at runtime:

- `BattleShip.html` — threaded (preset `web-wasm`): needs
  `SharedArrayBuffer` + `crossOriginIsolated` (the COOP/COEP headers
  `serve.py` sends). Default when capabilities allow.
- `BattleShipST.html` — single-thread (preset `web-wasm-st`, CMake option
  `SSB64_WEB_SINGLE_THREAD=ON`): boots **without** cross-origin
  isolation. Selection is automatic — the threaded shell redirects to
  `BattleShipST.html` when SAB/COI are missing — and manual via `?st=1`
  (force ST), `?st=0` (force threaded) or the "Try single-thread build"
  recovery link on error screens.

Serial fallbacks under `SSB64_WEB_SINGLE_THREAD` (defined globally via
`CMAKE_*_FLAGS` so the macro reaches libultraship too):
`spdlog` sync logger instead of the async pool
(`Context::InitLogging`), `SerialThreadPool` running resource loads
inline (`ResourceManager`), watchdog disabled (signal handlers still
install), first-run extraction deferred-inline (`std::launch::deferred`),
the in-game Updater sidebar hidden (web updates are the service worker's
job — same reasoning as Android), and the `osContInit`
`gamecontrollerdb.txt` probe skipped entirely (SDL's built-in table +
runtime-injected mappings still apply). Netplay leaderboard code is
already a no-op on web (`SSB64_HAVE_CURL` unset); the SDL transports and
GCAdapter paths are empty TUs under Emscripten.

Known ST quirk (root-caused 2026-10-20): `stat()` /
`std::filesystem::exists`, `SDL_GameControllerAddMappingsFromFile` and
the spdlog stdio sink **deadlock when called inside an Asyncify fiber**
in single-thread builds — the same calls are fine threaded and fine on
the main fiber. Any future port code that touches the filesystem or
locked stdio inside an N64 coroutine must stay on the main fiber or use
`fopen`-style probing; this is why the controllerdb probe is skipped
rather than converted in ST.

## Verified

- `platforms/web/pw_rom_picker.py` — picker → Torch worker → typed
  `NO_O2R` rejection, picker stays usable (Chromium).
- `platforms/web/pw_rom_privacy.py` — request capture: every request
  same-origin GET, zero ROM bytes in any request; Cache API + IndexedDB
  audited for ROM-marker leaks (none).
- `platforms/web/pw_browser_matrix.py` — boot smoke on
  chromium/firefox/webkit: all reach a sized canvas with the SW active.
- Variant selection matrix (merged deploy dir, Chromium): non-COI
  `BattleShip.html` auto-redirects to `BattleShipST.html` and the game
  boots to `scManagerRunLoop`/scene 28; COI `BattleShip.html` stays
  threaded and boots; `?st=1` forces ST under COI; `?st=0` on a non-COI
  server stays threaded and shows the typed
  `CROSS_ORIGIN_ISOLATION_MISSING` capability error.

## Canvas / resolution
the element's CSS box tracks the viewport and SDL's Emscripten backend
syncs the GL drawing buffer to it (honoring devicePixelRatio), so this
is a true resolution scale with correct mouse mapping — not a blurry
stretch. A `MutationObserver` refits when the game flips aspect
(widescreen CVar).

## Input: keyboard, gamepad, touch, mobile text

**Keyboard** — physical typing works through SDL-Emscripten's window
listeners → `GfxWindowBackendSDL2::HandleSingleEvent` → LUS keyboard
device → `osContGetReadData` (default LUS mapping: A=X, B=C, Z=Z, L=E,
R=R, Start=Space, C=arrow keys, stick=WASD).

**Gamepad** — SDL-Emscripten's gamepad driver is compiled in; LUS polls
`SDL_GameController` state every frame and `gamecontrollerdb.txt` is
served next to the page.

**Touch overlay** — on touch-capable/coarse-pointer devices the shell
shows a DOM overlay (floating stick + A/B/Z/L/R/Start + C cluster + menu
button). Controls synthesize `KeyboardEvent`s carrying the `code` of the
default LUS mapping, so remaps apply transparently. `keyup` is delayed
~50 ms so at least one N64 pad sample sees the press — see
`docs/bugs/web_touch_samepump_key_collapse_2026-10-20.md`.
`?touch=1`/`?touch=0` force it on/off; it auto-hides while a browser
gamepad is connected or the ImGui menu is open.

**Mobile text input** — SDL-Emscripten has no `SDL_StartTextInput`
implementation, so ImGui text fields cannot summon the OS keyboard on
their own. The web build reuses `Ship::Mobile::ImGuiProcessEvent`
(`MobileImpl.cpp`, now compiled for Emscripten): `Gui::EndFrame` polls
`io.WantTextInput` every frame — not only per SDL event, because after a
tap activates an `InputText` no further events may arrive — and on
transitions publishes `window.__ssb64_wants_keyboard`. The shell watches
the flag and focuses `#kbproxy`, a bottom-bar `<input>`; that opens the
mobile OS keyboard and the proxy's real keystrokes feed SDL exactly like
physical typing (keydown/keypress → SDL key + textinput). On deactivate
the flag clears and the proxy blurs/hides. The viewport meta carries
`interactive-widget=resizes-content` so the keyboard shrinks the visual
viewport and the bottom-fixed proxy stays visible *above* it instead of
being covered.

**Debug/test observables** — `port_web_touch_poll` publishes two extra
globals each frame: `window.__ssb64_menu_open` (ImGui menu visibility)
and `window.__ssb64_p1_buttons` (`gSYControllerDevices[0]` —
hold low 16 bits, tap high 16 bits), letting page code and test rigs
verify input reached the *game-consumed* N64 pad state without peeking
at wasm memory.

## Netplay: public lobby

The default server (`PORT_NETLOBBY_DEFAULT_SERVER` in
`src/shared/netplay_server.h`) is the deployed Cloudflare lobby
`wss://smash-netplay.jumpcircuit.workers.dev`, so the Online menu
(Host / Join / Quickplay / Spectate) works out of the box — no URL
params needed. Verified E2E:
`platforms/web/pw_lobby_public_smoke.js` hosts on tab A, mints the room
code (published to `window.__ssb64_lobby_code` by `auto_code_write` —
browser tabs can't share the `lobby_code.txt` file), joins tab B by
code, and rides autostart into a running rollback match: identical
seeds both sides, `desync=0`, frames progressing on both.

Web sessions are **relay-only end-to-end** (browsers have no UDP):
`send_stun` short-circuits to the give-up outcome on `__EMSCRIPTEN__`,
so the endpoint publish doesn't stall for seconds retrying unreachable
STUN servers, and every peer link is a relay link over the lobby
WebSocket pair (control + uplink).

The local variant `pw_lobby_smoke.js` runs the same handshake against a
dev `node-lobby.mjs` (`ws://localhost:8799`). Both scripts need
`?autostart=1` (skips the Start gate) and block `sw.js` for
determinism; run them from a directory where `playwright` resolves
(e.g. `NODE_PATH=<path-to>/node_modules node pw_lobby_public_smoke.js`).

## Simulated input validation (no hardware)

**`pw_gamepad_sim.js`** — installs a synthetic standard-mapping Gamepad
behind `navigator.getGamepads()` before load, fires
`gamepadconnected`, and asserts: touch overlay hides while the pad is
connected, a scripted START press shows up in `__ssb64_p1_buttons`
(game-consumed state), and `gamepaddisconnected` restores the overlay.
Runs in a mobile-emulated context so the overlay-replacement rule is
actually exercised.

**`SSB64_VJOY` (native, `SSB64_TEST_HOOKS`)** —
`src/application/vjoy_testhook.cpp` attaches an SDL virtual joystick,
registers a standard mapping for its GUID so `SDL_IsGameController()`
accepts it, and lets LUS bind it via the real `CONTROLLERDEVICEADDED`
path. `SSB64_VJOY_SCRIPT="F:B[:H],…"` presses SDL button B at frame F
for H frames (default 15); `SSB64_VJOY_EXIT=N` exits cleanly. Each tick
the hook logs `gSYControllerDevices[0]` hold/tap changes — e.g.
`180:9,400:0:30` logs `tap=0x1000` (START) then `tap=0x8000` (A),
proving the full SDL → ControlDeck → decomp chain on a real desktop
build with zero hardware.

## What works / what doesn't

| Area | State |
|------|-------|
| Build | full link: decomp + LUS + port → `BattleShip.wasm` (~15 MB) |
| SDL2 | Emscripten port (`-sUSE_SDL=2`) |
| GL | WebGL2 via `USE_OPENGLES` (no GLEW) |
| Coroutines | `emscripten_fiber_*` backend + ASYNCIFY |
| Netplay | `websocket_web.cpp` (control+uplink) + `udp_transport_web.cpp` (relay-only facade); public-lobby match verified E2E |
| Saves/config/mods | IDBFS (`/persist` is the process cwd; synced on pagehide, visibility-change and after asset writes) |
| Canvas | fills tab, aspect-preserved, SDL-synced real resolution |
| Input | keyboard, gamepad, on-screen touch overlay, mobile kbproxy |
| Mods/scripting | disabled (`DISABLE_SCRIPTING` forced) |
| Updater/downloads/Discord/leaderboard | inert stubs (`port_web_stubs.cpp`) or self-gated (SSB64_HAVE_CURL absent) |
| First-run wizard | replaced by the page loader (o2r upload/fetch/ROM extract) |
| Watchdog/renderdoc/hidapi/libusb | excluded or stubbed |

## Known issues

- wasm-ld reports K&R-style **signature mismatches** across decomp TUs
  (e.g. `itmain.c` declares a symbol with more args than `itlinkbomb.c`).
  They link as warnings today; any indirect (function-pointer) call through
  a wrong signature traps at runtime. These are real ABI hazards worth a
  dedicated cleanup pass.
- `SSB64_TEST_HOOKS` boot paths (replay/state-trace env vars) compile in —
  `std::getenv` reads the emulated env, which `port_web_stubs.cpp`'s
  pre-main constructor populates from URL params (`SSB64_*` + lobby
  aliases). Determinism harness on web is future work.
- Audio latency/sync and 60 fps pacing are unverified (the Start-gate
  unlocks the AudioContext, but latency under browser scheduling is
  unmeasured).
- The coroutine/ASYNCIFY combination works at link time; deep call chains
  through Asyncify-listed frames may need `ASYNCIFY_STACK_SIZE` tuning or
  an `ASYNCIFY_ADD` allowlist if the scheduler misbehaves.

## Files

- `shell.html` — page + loader (pre-main FS staging, o2r/ROM picker,
  capability gate, storage panel, SW registration, IDBFS cwd)
- `extract_worker.js` — module worker running in-browser torch extraction
- `sw.js.in` / `stamp_sw.cmake` — service worker (offline shell,
  wasm-hash-versioned updates)
- `manifest.webmanifest` / `icons/` — PWA install metadata
- `src/platform/coroutine/coroutine_emscripten.cpp` — fiber backend
- `src/networking/netplay/transport/websocket_web.cpp` — `port_ws_*` over
  `emscripten_websocket_*`
- `src/networking/netplay/transport/udp_transport_web.cpp` — relay-only
  `port_udp_*`
- `src/application/port_web_stubs.cpp` — inert implementations of the
  desktop-only APIs the menus still reference
- `components/libultraship/cmake/dependencies/emscripten.cmake` —
  FetchContent dep set (zlib/libzip/spdlog/tinyxml2/nlohmann_json) + SDL2
  port interface
