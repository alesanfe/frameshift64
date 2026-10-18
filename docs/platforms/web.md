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
  (embind `Companion` API; not yet wired into the page — see below)

## Run locally

Browsers refuse wasm from `file://`; serve the build dir:

```bash
cd out/build/web-wasm && python -m http.server 8080
# → http://localhost:8080/BattleShip.html
```

## Asset flow (first run on web)

The page defers `main()` until `BattleShip.o2r` exists in MEMFS:

1. IDBFS is mounted at `/persist` — an o2r cached from a previous session is
   restored automatically.
2. Otherwise the page tries `fetch('BattleShip.o2r')` — drop a pre-extracted
   archive next to the html when hosting to skip the picker.
3. Otherwise an upload button accepts a desktop-extracted `BattleShip.o2r`,
   stores it in MEMFS **and** persists it into IDBFS for next time.

In-browser ROM → o2r extraction via wasm-Torch is compiled (the `torch`
target) but the page does not offer a ROM picker yet — that's the remaining
wiring (load `torch.js` in a Worker, feed it the ROM bytes + extraction
recipes, read `BattleShip.o2r` out of its FS, hand it to the game FS).

## What works / what doesn't

| Area | State |
|------|-------|
| Build | full link: decomp + LUS + port → `BattleShip.wasm` (~15 MB) |
| SDL2 | Emscripten port (`-sUSE_SDL=2`) |
| GL | WebGL2 via `USE_OPENGLES` (no GLEW) |
| Coroutines | `emscripten_fiber_*` backend + ASYNCIFY |
| Netplay | `websocket_web.cpp` (control+uplink) + `udp_transport_web.cpp` (relay-only facade) |
| Saves/config | MEMFS + IDBFS mirror (`/persist`, synced on pagehide) |
| Mods/scripting | disabled (`DISABLE_SCRIPTING` forced) |
| Updater/downloads/Discord/leaderboard | inert stubs (`port_web_stubs.cpp`) or self-gated (SSB64_HAVE_CURL absent) |
| First-run wizard | replaced by the page loader (o2r upload/fetch) |
| Watchdog/renderdoc/hidapi/libusb | excluded or stubbed |

## Known issues

- wasm-ld reports K&R-style **signature mismatches** across decomp TUs
  (e.g. `itmain.c` declares a symbol with more args than `itlinkbomb.c`).
  They link as warnings today; any indirect (function-pointer) call through
  a wrong signature traps at runtime. These are real ABI hazards worth a
  dedicated cleanup pass.
- `SSB64_TEST_HOOKS` boot paths (replay/state-trace env vars) compile in —
  `std::getenv` is emulated; whether emscripten's env is wired to URL
  params is unverified. Determinism harness on web is future work.
- Audio latency/sync and 60 fps pacing are unverified (needs a browser run
  with real assets).
- The coroutine/ASYNCIFY combination works at link time; deep call chains
  through Asyncify-listed frames may need `ASYNCIFY_STACK_SIZE` tuning or
  an `ASYNCIFY_ADD` allowlist if the scheduler misbehaves.

## Files

- `shell.html` — page + loader (pre-main FS staging, o2r picker, IDBFS)
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
