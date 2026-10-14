# Threat model

Scope: the desktop/mobile port, the mod runtime, the updater, and the
netplay/lobby path. Per-surface detail lives in `network-threat-model.md`,
`mod-security.md`, `update-security.md`, `asset-trust.md`.

## Assets to protect

- Integrity + determinism of an in-progress match
- Session identity (room code, upkey, session_id)
- Lobby availability (rooms, connections, bandwidth)
- User configuration and saves
- Local assets and the extraction pipeline
- Loaded mods (native code inside the process)
- Update packages
- Logs (may carry identifiers — treat as semi-sensitive)
- Cloudflare deploy credentials; CI secrets (ROM, signing keys)

## Actors

Outdated client · modified client · malicious peer · packet-replay
attacker · flooding/oversize sender · room/connection exhauster ·
incompatible or malicious mod · compromised lobby · compromised build
dependency · compromised CI workflow.

## Threat table

| Asset | Threat | Surface | Impact | Current mitigation | Pending |
|-------|--------|---------|--------|--------------------|---------|
| Match integrity | Input injection as another player | UDP + lobby routing | Desync/griefing | upkey auth, server star routing, host-stamped FORWARD, session_id check | Session manifest (TD-008) |
| Match integrity | Replay of captured packets | UDP | Stale inputs, desync | Per-remote seq tracking; cross-session replay bounded by lobby-issued session_id | — |
| Lobby | Room-code guessing | WS upgrade | Slot theft | upkey per slot, bounded join rate | — |
| Lobby | Flooding | WS text/binary | DoS | 20 text/s, 600 bin/s, size caps | Global budget per DO |
| Parsers | Malformed/huge datagram | UDP parse | Crash/OOM | Fixed sizes, size-family check, bounds, FNV checksum; `-PacketFuzz` harness | libFuzzer targets (TD-010) |
| Config/saves | Corrupted/torn files | disk | Crash, lost progress | (pending) atomic write + versioned schema + .bak | TD-004/TD-005 |
| Mods | Malicious/incompatible mod | TCC compile + hooks | Arbitrary code in-process | Manifest gate, load states, no gameplay hooks for cosmetic packs | apiVersion + simulation class (TD-007); mods are trusted code — see mod-security.md |
| Updates | Tampered package | updater | Malware install | (pending) signature/hash verification | TD-009 |
| CI | Secret exfiltration | Actions | ROM/keys leaked | Secrets scoped to jobs; `BASEROM_B64` cleared after decode | SHA-pinned actions (done), scorecard |
| Logs | Identifier leakage | log files | Privacy | — | Redaction + diagnostic bundle policy (TD-011) |

Residual risk and the per-surface tables are in the sibling docs.
