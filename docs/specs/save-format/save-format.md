# Save format contract

- `ssb64.sav` — flat SRAM chip mirror; offsets are absolute and
  versioned implicitly by the game layout. No in-image header by
  design (see docs/engineering/technical-debt-register.md TD-004).
- `ssb64.sav.meta` — sidecar JSON: `format_version`, timestamps.
  Bumping `format_version` is the migration hook.
- Writes: temp + fsync + atomic rename (never a torn image).
