# Public API boundary

The flat include model means "public" is a **declared contract**, not a
directory — `config/architecture/public-api.yml` is the list, and
`tools/analysis/lint/check_public_api.py` enforces it in CI.

## What's public

| Surface | Consumers | Stability rule |
|---|---|---|
| `port_*` C ABI (`src/game/engine/`, watchdog, save) | `components/decomp` | append-only; signature changes need a coordinated decomp commit |
| `shared/result/` error taxonomy | all port modules + tools | additive categories only |
| `schema.h` CVar schema | menu, tools, profiles | keys retire, never renumber |

Everything else is **module-private** even if another module legitimately
includes it today — module scopes (`SSB64_MODULE_SCOPE_*`) expose headers
for compilation, but only `public-api.yml` entries carry a stability
promise.

## Rules

1. A public header may only include other public headers, siblings in its
   own directory, or system headers — never another module's private
   header (the checker verifies this; `#include "foo.h"` that resolves
   outside `public:` fails CI).
2. Public headers must be self-contained — they compile standalone in any
   include order. Add `smash_test`-style TU coverage when one is added.
3. Removing/renaming a public header requires moving it to `retired:`
   with a note, not deletion.
4. New public headers need: entry in `public-api.yml`, doc comment naming
   the consumer, and no downward includes.
5. Decomp must never `#include` outside the `port_*` surface — that is
   the one boundary byte-matching never relaxes.

## Why a manifest, not an `include/` dir

Moving ~50 headers into `public/` breaks every flat include site for
cosmetic gain. The manifest gives the same guarantee (a nameable,
CI-checked contract) at zero churn. If a future phase does create
`include/smash_classic/`, this file becomes its index.
