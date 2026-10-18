# specs — consumable contracts

Machine-checkable and implementation-neutral contracts shared between
the native client, the lobby service, and mods. The *schemas* live here
under `docs/specs/`; narrative docs live in the neighbouring sections.

| Dir | Contract |
|-----|----------|
| `netplay/`      | lobby message vocabulary + handshake order (worker.js / node-lobby.mjs / netlobby.cpp all implement it) |
| `mod-api/`      | `manifest.schema.json` — mod manifest fields incl. `apiVersion` / `simulation` |
| `save-format/`  | save image + `.meta` sidecar layout |
| `updates/`      | release manifest fields consumed by `ReleaseSig`/`Updater` |
