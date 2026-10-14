# Vendored data — GLideN64 → BattleShip texture-hash mapping

`battleship.json` maps third-party N64 hires-pack filename hashes to the
decoded-RGBA8 CRC32 keys this port's `HiResPack` looks up:

    "<RiceCRC8>#<fmt>#<siz>"             -> "<ourHash8>"
    "<RiceCRC8>#<fmt>#<siz>#<palCRC8>"   -> "<ourHash8>"   (CI textures)

It covers every texture in the retail US ROM (31,228 entries), so **any**
GLideN64/Rice-PNG texture pack for vanilla SSB64 — not just SSB Reloaded —
can be re-keyed for BattleShip by renaming its PNGs. The in-game installer
(`src/distribution/ModPackDownloader.cpp`) and the CLI twin
(`tools/modding/convert_gliden64_pack.py`) both consume this file; the build
stages it next to the binary as `bundled-data/gliden64-map.json`.

## Provenance

- Upstream: <https://github.com/GhostlyDark/SSB-Reloaded> —
  `SMASH BROTHERS/battleship.json`
- Vendored 2026-10-15, upstream blob sha1 `7494e51809ccb53a3a61293faf38ffdf5e8fbb61`
- File sha256 `55b243dd911497495c8e4a77f5627b8770972500b36fb69ef39151bdd752ff71`
- License: the upstream `battleship.py` (MIT) documents this file as "pure
  data ... shippable under any license" — it is a hash dictionary produced
  by a GPL-side build step and carries no copyrightable content of its own.

## Updating

Download the current file from the upstream repo and replace
`battleship.json`; bump the sha1/sha256 above. The runtime treats a missing
or malformed file as "converter unavailable" — nothing else depends on it.
