# src/resources — relocatable game assets

Loads and serves ROM-derived reloc files: `RelocFile`, `RelocFileFactory`,
`RelocPointerTable`, `SyntheticReloc`, and the per-region
`RelocFileTable.*` + `include/reloc_data.*.h` — **generated** by
`tools/codegen/` (marked `linguist-generated`, never edit).

Subdirs: `audio/` (banks/SFX), `css_icons/`, `interpolation/` (frame
interpolation), `shaders/`, `textures/hires/` (HiResPack).

## Allowed dependencies

`integration/decomp` (relocation adapters), `platform`, `observability`,
LUS resource manager. No `features`, `presentation`, `netplay`.

## Tests

`tests/unit/resources/` (VPK0, extern-ids), `tests/integration/assets/`.
