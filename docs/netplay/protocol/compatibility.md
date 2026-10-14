# Protocol compatibility rules

1. **Explicit IDs.** `SYNETPEER_PACKET_*` values are assigned constants.
   Never reorder or rely on implicit enum numbering.
2. **No field reinterpretation.** A field's wire meaning is fixed at its
   offset forever. New semantics → new packet type or new version.
3. **Size is part of the contract.** Families are discriminated by size;
   appending fields to an existing family requires a new size (i.e. a new
   packet type or version bump).
4. **Additive lobby changes only.** New `reason` strings, new JSON keys,
   new `type` values are fine; renaming/removing is breaking.
5. **Version bump = hard cut.** `SYNETPEER_VERSION` 3↔4 peers cannot
   parse each other at all; the lobby's `version-mismatch` reject prevents
   them from ever sharing a room.
6. **Determinism is a compat surface.** Two clients with identical
   protocol versions but different simulation behavior desync. The
   session manifest (spec §Session manifest, TD-008) is the designated
   gate once implemented.

## Test coverage mapping

| Rule | Verified by |
|------|-------------|
| Bad magic/version/session/checksum dropped | `scripts/testing/netplay_localtest.ps1 -PacketFuzz` |
| Every validation removable = caught | `scripts/testing/netplay_mutation_suite.ps1` |
| Release ≡ Debug traces | `determinism.yml` cross-build job |
| Lobby security invariants | `services/netplay-lobby/tests/test-lobby-security.js` |
