# Netplay observability

The lobby service exposes three telemetry surfaces — pick per deployment.

## `/health` (both adapters)

`GET /health` → `{"ok": true, "service": "smash-netplay", "rooms": N}`.
Point an uptime check at it; `rooms` is the live Durable-Objects-in-flight
count on the local adapter.

## `/metrics` — Prometheus exposition (local adapter)

`node-lobby.mjs` serves `GET /metrics` in Prometheus text format:

| Metric | Type | Meaning |
|---|---|---|
| `ssb64_lobby_rooms` | gauge | open rooms |
| `ssb64_lobby_sockets` | gauge | open player/spectator sockets |
| `ssb64_lobby_players` / `ssb64_lobby_spectators` | gauge | occupancy split |
| `ssb64_lobby_admits_total` | counter | admissions, all roles |
| `ssb64_lobby_admits_role_total{role=...}` | counter | per-role admissions (`host`/`join`/`spec`/`queue`) |
| `ssb64_lobby_rejects_total` | counter | rejected admissions |

Scrape it with any Prometheus-compatible collector (Grafana Agent,
Prometheus, VictoriaMetrics) and graph in Grafana.

## Analytics Engine — deployed Cloudflare Worker

`worker.js` emits one datapoint per room admission when the
`LOBBY_ANALYTICS` binding is present (declared in `wrangler.jsonc`,
dataset `ssb64_netplay_lobby`):

- `blobs[0]` — requested role (`host`/`join`/`spec`/`queue`)
- `blobs[1]` — outcome (`attach`/`reject`/`uplink`)
- `doubles[0]` — protocol version (1/2)
- `indexes[0]` — reject reason (empty on success)

Query it from Grafana with the Cloudflare datasource plugin, or directly:

```sql
SELECT blob1 AS role, blob2 AS outcome, COUNT() AS n
FROM ssb64_netplay_lobby
WHERE timestamp > NOW() - INTERVAL '30' DAY
GROUP BY role, outcome
```

Suggested panels: admissions/min by role, reject ratio, queue-vs-room-code
mix, spectator share.

## Match-level metrics (CI artifact, not live telemetry)

`tools/analysis/netplay_metrics.py` parses the structured rollback logs the
engine emits (`rollbacks=`, `resim_ticks=`, `max_rollback=`, `desync=`) and
the confirmed-state trace diff into `metrics.json` + an append-only JSONL
history (`--history`). `.github/workflows/determinism.yml` uploads
`metrics-history.jsonl` as a 30-day artifact — that is the "last 30 days"
panel for desync rate, worst rollback depth, and resimulated-frames-per-tick.
`--summarize <history.jsonl>` reduces the window to P50/P95/P99 per metric
(the dashboard feed); `--days N` changes the window (default 30).

## `/leaderboard` + `/submit` + `/result` — rating board (both adapters)

`GET /leaderboard` → `{"entries": [{name, rating, wins, losses, updated,
disputes}]}` sorted by rating (top 100). Two write paths:

- `POST /submit` — self-reported upsert `{"name","key","rating","wins",
  "losses"}`. `key` is a random hex token persisted in `netstats.json`;
  first submitter claims the name, later submits must present the same
  key (`400 name-taken`). Anti-hijack only.
- `POST /result` — **verified** path (the one that moves server-side Elo).
  Both clients report `{name,opp_name,key,match_id,won,duration}` for the
  same match; the second report from a *different* key either verifies
  (server applies Elo K=32 itself) or mismatches (`disputes++` on both
  names). See `protocol/messages.md` § Verified results.

- Worker adapter persists it in the dedicated `leaderboard-global` DO;
  node-lobby persists `leaderboard.json` next to the script.
- Client: `gNetplay.SubmitStats` (default **off** — sharing a name/rating
  on a public board is opt-in, toggled from the Online menu) controls the
  post-match submit + result report. LAN matches never report.
