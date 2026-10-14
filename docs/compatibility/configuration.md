# Configuration compatibility

Configuration is currently untyped key/value (LUS CVars persisted by the
engine). The target design — schema, migrations, local-vs-synchronized
split — is documented here as normative; implementation is tracked as
TD-005 in the technical-debt register.

## Local vs simulation-affecting

Two disjoint categories:

- **Local-only**: resolution, volume, language, overlays, device bindings,
  touch layout. Free to differ between peers; never part of handshake.
- **Simulation-affecting**: ruleset, gameplay mods, competitive options,
  anything read by game code during a match. Must be exchanged and
  compared in the session manifest before a netplay match starts
  (`docs/netplay/protocol/specification.md` §Session manifest).

A config key must declare its category; the default is local-only, and
marking a key simulation-affecting requires a compatibility review.

## Target schema

    {
      "schemaVersion": 3,
      "video": {}, "audio": {}, "input": {},
      "accessibility": {}, "netplay": {}, "mods": {}
    }

Requirements when the config module lands under `src/` (planned):

- `schemaVersion` field, checked on load.
- Centralized defaults; validation of ranges and enum values.
- Unknown keys: warn + preserve (forward compatibility).
- Atomic write; backup before migration; corrupt file → defaults + warn.
