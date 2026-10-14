# src/features — player-visible gameplay enhancements

Opt-in enhancements the player can toggle. Infrastructure that only
*delivers* features (updater, pack downloads, signing) lives in
`src/distribution/` — not here.

| Subdir | Contents |
|--------|----------|
| `gameplay/` | Input & rules: Z-cancel assists, C-stick/DPad/tap-jump, comp ruleset, neutral spawns, stage hazards, HUD, classic co-op, CPU L9, unlocks, hitbox view, boot/skip shortcuts. |
| `audio/`    | Music selection, shuffle, volume. |
| `social/`   | Discord Rich Presence. |
| `cosmetics/` | widescreen (has its own include dir). |
| `internal/` | `buttons.h` shared button definitions. |

`enhancements.h` at this level is the shared registry/menu header every
feature includes.

## Allowed dependencies

Leaf modules, `integration`, `netplay` (rules handshake). Never
`application`/`presentation`. New files: put them in the domain
subdir that fits; do not add new top-level `features/` dumping ground.
