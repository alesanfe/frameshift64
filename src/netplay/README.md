# src/netplay — online play coordination layer

Owns everything above raw sockets and below the game simulation: room
rules, lobby session, packet transports, rollback handshake.

| Subdir | Contents |
|--------|----------|
| `domain/`    | `netrules` — match rules negotiated in the PICK handshake (stage, stocks, items, team/FFA, simulation-affecting CVars). |
| `session/`   | `netlobby` — room lifecycle, host-star topology, peer forwarding, compat token handshake, spectators, rematch. |
| `transport/` | `udp_transport` (direct UDP + LAN + relay hooks + network sim) and `websocket_transport` (WebSocket fallback path). |
| `rollback/`  | `netrollback_seg.h` — forced-included header isolating the rollback segment's decomp TUs (`netrollback.c`, `netinput.c`, ...). |

## Allowed dependencies

`observability` (port_log), `persistence`, `platform` (io, coroutine),
`integration` (modding for compat tokens), `game`. Never `presentation`,
`features`, `distribution`, `application`. Enforced by
`tools/lint/check_module_deps.py`.

## The real boundary

The simulation itself lives in `decomp/src/sys/net*.c` — rollback,
input sync, replay. This layer only *coordinates* it: transports feed
`port_net_*` hooks the decomp declares. Determinism bugs belong in
decomp; session/transport bugs belong here.

## Tests

`scripts/testing/*.ps1` e2e rigs (lobbytest, lan, compat, spectate,
replay_takeover, mutation_suite, soak). Lobby protocol:
`services/netplay-lobby/tests/`.
