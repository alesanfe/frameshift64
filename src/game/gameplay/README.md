# src/game/gameplay — player-visible gameplay enhancements

Opt-in enhancements the player can toggle. Infrastructure that only
*delivers* features (updater, pack downloads, signing) lives in
`src/application/update/` — not here. Non-gameplay feature packs live in their
own top-level modules: `src/audio/` (music/volume), `src/networking/social/`
(Discord Rich Presence), `src/input/` (controller remap + stick
behaviors, `internal/buttons.h`), `src/rendering/` (widescreen, HUD).

| Subdir | Contents |
|--------|----------|
| `assists/`  | Z-cancel assists: `AutoZCancel`, `FailedZCancelFlash`. |
| `modes/`    | Alternate modes/boot paths: `BonusStages`, `BootToVSCSS`, `ClassicCoop`, `SkipResultsScreen`. |
| `rules/`    | Match rules: `CompRuleset`, `CpuLevel9`, `DisableStageHazards`, `NeutralSpawns`. |
| `training/` | Practice tools: `HitboxView`. |
| `unlocks/`  | `UnlockCheats`. |

`enhancements.h` at this level is the shared registry/menu header every
feature includes.

## Allowed dependencies

Leaf modules, `integration`, `netplay` (rules handshake). Never
`application`/`presentation`. New files: put them in the domain
subdir that fits — or in the matching top-level module (`audio/`,
`input/`, `social/`, `rendering/`) when the feature isn't gameplay.
