# port_* ABI contract

The `port_*` extern "C" surface is the boundary between the port runtime
(`src/`) and the decomp game code (`components/decomp/`). This directory
holds the shared contract that all `port_*` headers follow.

## Versioning — `port_api_version.h`

`PORT_ABI_CURRENT` packs `MAJOR << 8 | MINOR`. Call
`port_abi_version()` for the *linked* runtime's version.

- **MAJOR** — a signature changed or a symbol was removed. Old callers
  must not link.
- **MINOR** — additive only: new symbols, new enum flags, new trailing
  fields in size-tagged structs. Old callers keep working.

Stamps that cross trust boundaries (replay headers, crash bundles,
match handshakes) must record `PORT_ABI_CURRENT` so mismatched builds
are rejected instead of misbehaving.

## Result codes — `port_api_errors.h`

`PortResult` negative-on-failure codes supersede ad-hoc `0`/`-1`
returns. Legacy signatures keep their old conventions until a MAJOR
bump; new functions return `PortResult`.

## Conventions every `port_*` header follows

1. **`extern "C"` everywhere** — no C++ types or exceptions cross the
   boundary. Decomp is C; the port side is C++.
2. **Caller-allocated out-structs** — functions that fill a caller
   buffer take a `size`/`capacity` argument; they never return pointers
   into mutable port-side state unless documented as borrowed for the
   call's duration.
3. **Size-tagged extension** — a struct expected to grow carries a
   `struct_size` field set by the caller to `sizeof(struct)`. New
   trailing fields are MINOR bumps; the callee must not read past
   `struct_size`.
4. **Opaque handles over internals** — callers hold `uint32_t`/pointer
   tokens, never `Peer`, `Lobby`, or other port-internal types.
5. **Thread discipline is stated per family** — functions callable off
   the game thread say so in their doc comment (e.g. logging is
   any-thread; save is game-thread only).
6. **No hidden global side-effects** — a `port_*` call that mutates
   lobby/save/renderer state documents it; read-only queries are marked
   `/* const */` in the header.

## Adding to the ABI

New symbol → MINOR bump in `port_api_version.h` + entry in the table
below. Signature change → MAJOR bump and update every caller; never
change a signature silently.

| Area | Header | Status |
|------|--------|--------|
| Version | `port_api_version.h` | v1.0 |
| Errors | `port_api_errors.h` | v1.0 |
| Save/load | `src/persistence/port_save.h` | legacy (0/-1 codes) |
| Engine init | `src/game/engine/port.h` | legacy |
