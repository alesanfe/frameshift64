# mods/

Mod SDK and content for the BattleShip mod system (TCC-compiled C hooks).

| Dir | Contents |
|-----|----------|
| `sdk/` | Public headers + cmake helpers consumed by mod projects (WIP — see `docs/modding/modding.md`). |
| `examples/` | Working example mods (`hooktest`, `playertint`) — was `workspace/`. Each has its own `CMakeLists.txt`. |
| `templates/` | `basic-mod/` — copy this to start a new mod (was `workspace/template/`). |
| `tests/` | Mod-loading checks. |

The in-engine runtime (hook manager, registry, symbol resolver, mod bridge) lives in `src/bridge/modding/`; the event-dispatch hook list in `src/bridge/hooks/`.

## One-click installs (in-game Mods panel)

- **Bundled mods**: the build packages `mods/examples/playertint` into
  `<exe>/bundled-mods/playertint/` (`tools/developer/modding/package_mod.py`, run as
  a `POST_BUILD` step). *Assets → Script Mods* shows an **Install & Enable**
  button that copies it into `mods/` and hot-reloads (compiles + runs
  `ModInit` immediately).
- **Curated texture packs**: *Assets → Mods* offers **Download & Install**
  for SSB Reloaded 2.0.1 (GhostlyDark's HD pack,
  [upstream](https://github.com/GhostlyDark/SSB-Reloaded)). The installer
  (`src/application/update/downloads/ModPackDownloader.cpp`) downloads the release zip into
  `mods/` over a `.part` staging file, verifies a compile-time-pinned
  SHA-256, audits the archive (PNG-only, no traversal/manifest), then
  installs atomically. The pack is downloaded from upstream — it is not
  redistributed by this repo.
- **Install your own**: the same panel accepts a pasted URL, and files or
  folders can be dropped straight onto the game window. The installer
  sniffs the payload and does the right thing per kind:
  - zip/dir with a `manifest.json` (or an `.o2r`/`.otr`) → script mod,
    installed into `mods/` and activated on the next Hot Reload.
  - PNGs in the native `#<hash>#<fmt>#<siz>.png` grammar → hi-res texture
    pack, installed into `mods/` (zips stay zipped — HiResPack indexes
    members without extracting).
  - PNGs in the GLideN64/Rice grammar
    (`<prefix>#<crc8>#<fmt>#<siz>[#<palCRC8>]_<tag>.png`) → converted
    file-by-file through the vendored hash map
    (`tools/developer/modding/vendor/battleship.json`, staged as
    `bundled-data/gliden64-map.json`) into a `mods/` subdir. This makes
    virtually every emulator-era SSB64 texture pack installable as-is.
  The same conversion is available offline as a CLI:
  `python tools/developer/modding/convert_gliden64_pack.py --pack <dir|zip> --out mods/<name>`.

**Note:** the repo root `mods/` dir is also where HiResPack scans for user texture packs on dev launches — everything except `sdk/`, `examples/`, `templates/`, `tests/` and this file is gitignored (see `.gitignore`).

## Localization policy

Mod-authored content is **not** part of the game's translation table:
manifest fields (`name`, `description`, `author`) and any strings a mod
emits are rendered verbatim in the author's language. This applies to
`examples/` and `templates/` as well — they are reference content, not UI
chrome. The surrounding menu strings (buttons, tooltips, panel labels)
are translated in `assets/lang/` like the rest of the UI; the structural
test `tests/unit/presentation/menu_structure_test.py` enforces that
coverage for `src/` only.
