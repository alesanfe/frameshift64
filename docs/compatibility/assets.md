# Asset compatibility

## What is enforced today

`BattleShip.o2r` is ROM-derived and produced by the Torch pipeline. Build
and runtime are protected by a **recipe hash**, not a version number:

- CMake bakes `SSB64_ASSET_RECIPE_HASH` from the Torch SHA + `config.yml`
  + all extraction yamls for the region.
- Extraction stamps a `<o2r>.recipe` sidecar next to the archive.
- On boot the game compares baked vs stamped hash; a mismatch triggers an
  automatic re-extract, keeping the old archive until the replacement is
  fully built. See `docs/bugs/README.md` → `stale_o2r_recipe_mismatch`.

This covers toolchain/layout drift. It does not yet cover intentional
*format* revisions of the archive itself — that is what
`SMASH_ASSET_FORMAT_VERSION` is reserved for.

## Region builds

US and JP are separate recipe sets (`assets/game/us`,
`.../jp`) producing separate per-build-dir extraction sinks; runtime
stages `assets/game/<region>` + `config.yml` next to the binary for first-run
extraction and sidecar-Torch re-extraction.

## Golden tests (target)

    tests/support/fixtures/assets/
    └── per region: { expected hash, size, resource count, load check }

Tracked as TD-006.
