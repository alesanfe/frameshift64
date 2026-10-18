# smash_mod_api — stable mod-facing surface

The set of headers a mod may `#include`. Everything else in `src/`
is internal and may change without notice.

Stable surface (implemented in `src/game/engine/`):

- `integration/modding/HookManager.h` — detour + trampoline API
- `integration/modding/ModRegistry.h` — mod enumeration + manifest
  fields (`apiVersion`, `simulation`)
- `integration/hooks/Events.h` + `integration/hooks/list/` — game
  event subscription

Rules for mods:

- never `#include "src/..."` paths directly — CI rejects it;
- declare `"simulation": false` in the manifest for cosmetic-only
  mods (they are exempt from the netplay compat hash);
- pin `"apiVersion"` — the loader rejects unknown majors.

See `mods/templates/basic-mod` and `docs/specs/mod-api/`.
