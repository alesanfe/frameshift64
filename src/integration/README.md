# src/integration — adapter layer between decomp, engine, and mods

The only module allowed to know both N64-decomp internals and
libultraship/mod APIs. Everything here translates across a boundary.

| Subdir | Contents |
|--------|----------|
| `decomp/`   | Adapters that serve N64 semantics over modern machinery: `audio_bridge` (seq/bank/fgm decoding + playback), `lbreloc_bridge` + `lbreloc_byteswap` (relocatable asset fixup, big-endian→native), `particle_bank_bridge` (validated particle bank loading), `framebuffer_capture`, `ssb64_reloc_rebuild.*` (generated deblob spec). |
| `hooks/`    | `Events` — the engine event bus + `list/` catalog of game events decomp call-sites fire. funchook-free; always compiled. |
| `modding/`  | `HookManager`, `ModRegistry`, `SymbolResolver`, `mod_bridge` — the TCC-compiled mod runtime (funchook detours). Dropped under `DISABLE_SCRIPTING`. |

## Allowed dependencies

decomp headers, libultraship, leaf modules (`observability`, `game`,
`rendering`), `resources`. Never `application`/`presentation`/
`features`/`netplay`. Enforced by `check_module_deps.py`.

## Rules of the layer

- A decomp `.c` file may `#include "integration/..."` headers — that is
  the sanctioned call-out path (see `decomp/src/sys/audio.c` →
  `audio_bridge.h`).
- Mod-facing surface is `modding/` + `hooks/list/`; `include/smash_mod_api/`
  documents the stable subset. Internal headers are not part of the API.
