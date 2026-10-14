# Save compatibility

Save data is user property. A release must never silently destroy or
downgrade it.

## Rules

- `SMASH_SAVE_FORMAT_VERSION` bumps on any layout change.
- Loading an older version must run a **recorded migration** or produce a
  clear error — never a partial import with wrong offsets.
- Before any migration, the existing file is copied to a `.bak` sibling.
- Writes must be atomic: write to temp file, fsync, rename over.
- A corrupted save degrades to defaults with a visible warning; the corrupt
  file is preserved alongside for debugging.

## Test fixtures (target layout — see tests/fixtures/)

    tests/fixtures/saves/
    ├── original-n64/         # controller pak / flash dumps we support
    ├── previous-releases/    # saves produced by each shipped version
    ├── corrupted/            # truncated, bad CRC, bad magic
    ├── partially-written/    # torn writes
    └── migrations/           # vN -> vN+1 golden pairs

Each fixture records: source version, expected migration result, and the
field-level assertions a unit test runs after load.

## Current state (2026-10-14)

`src/persistence/port_save.cpp` keeps the SRAM image as a flat 32 KiB chip mirror —
no in-image header, because every game offset is absolute. Format metadata
lives in a `ssb64_save.meta` sidecar written on first save:

    format_version=<SMASH_SAVE_FORMAT_VERSION>
    game_version=<SMASH_CLASSIC_APP_VERSION_STR>
    sram_size=0x<PORT_SAVE_SIZE>

Read path (`checkSaveFormat`, once per process):

- File size != `PORT_SAVE_SIZE` → loud log; reads still proceed (offsets
  past EOF return zero, i.e. fresh SRAM) — corruption is diagnosable but
  never destroys user data.
- Sidecar `format_version` > `SMASH_SAVE_FORMAT_VERSION` → loud log
  ("written by newer build"); read still attempted. This is deliberate
  warn-and-read, per the "never silently destroy" rule — rejection only
  becomes correct once a migration framework exists.

Still open (TD-004): atomic write (temp + fsync + rename), `.bak` copy
before migration, in-image magic/version, and the fixture suite above.
Missing file → zero-filled fresh SRAM (unchanged behavior).
