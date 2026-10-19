// node-lobby — local mirror of the Cloudflare Worker lobby, for tests
// and LAN rigs. This file is ONLY the Node adapter: all room semantics
// (admission, routing, rate limits, roster, teardown) live in
// src/domain/ so they cannot drift from the deployed worker.
//
// Usage: node src/node-lobby.mjs [port]

import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import { WebSocketServer } from "ws";
import {
  CODE_RE, VALID_ROLES, IDLE_SWEEP_MS, makeJoinLimiter,
} from "../../domain/protocol.mjs";
import { LobbyRoom } from "../../domain/room.mjs";
import {
  Leaderboard, sanitizeName, LB_MUTE_DISPUTES,
} from "../../domain/leaderboard.mjs";
import {
  REPLAY_ID_RE, mintReplayId, validateReplayBody, evictOldest,
} from "../../domain/replay.mjs";
import {
  Bracket, sanitizeBracketName, BR_MAX_LIVE,
  mintBracketId, settleLiveBrackets,
} from "../../domain/bracket.mjs";
import { BOARD_PAGE_HTML } from "../../domain/board_page.mjs";

const rooms = new Map();    // code -> LobbyRoom

/* Store writes go through temp+rename so a crash mid-write can't leave a
 * torn JSON file behind (same pattern as the client's atomic_save). */
const writeJsonAtomic = (dest, obj) => {
  const tmp = dest + ".tmp";
  fs.writeFileSync(tmp, JSON.stringify(obj));
  fs.renameSync(tmp, dest);
};

/* Runtime state root: LOBBY_DATA_DIR for deployments (a Docker volume
 * lands wherever the image puts it); default keeps the beside-the-script
 * location the tests rely on. */
const dataDir = process.env.LOBBY_DATA_DIR ||
  path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "..", "..");

/* Leaderboard persisted under dataDir — runtime state, not repo data. */
const lbPath = path.join(dataDir, "leaderboard.json");
/* A torn or corrupt store must not crash-loop the service: recover to an
 * empty board rather than dying at module init (chaos test phase 3). */
let lb = new Leaderboard();
try {
  lb = Leaderboard.fromJSON(
    JSON.parse(fs.existsSync(lbPath) ? fs.readFileSync(lbPath, "utf8") : "[]"));
} catch { /* corrupt store - start fresh */ }
const saveLb = () => {
  try { writeJsonAtomic(lbPath, lb.toJSON()); } catch { /* ro fs */ }
};

/* Brackets persist beside the leaderboard - one file, id -> Bracket JSON.
 * Cap live brackets so a troll can't mint unbounded state. */
const brPath = path.join(dataDir, "brackets.json");
const brackets = new Map();
try {
  for (const [id, o] of Object.entries(JSON.parse(fs.readFileSync(brPath, "utf8")))) {
    brackets.set(id, Bracket.fromJSON(o));
  }
} catch { /* no bracket store yet */ }
const saveBrackets = () => {
  try {
    writeJsonAtomic(brPath,
      Object.fromEntries([...brackets].map(([k, b]) => [k, b.toJSON()])));
  } catch { /* ro fs */ }
};
/* Bracket ids come from the shared domain mint (crypto.randomUUID - same alphabet as the Worker adapter). */
const allowIpJoin = makeJoinLimiter();  // cross-code scan throttle (AUD-25)

/* Shared replays land next to the leaderboard store — runtime artifacts,
 * not repo data. In-memory index mirrors the worker DO's replays-idx. */
const replayDir = path.join(dataDir, "replays");
const replayIdx = [];
try {
  for (const f of fs.readdirSync(replayDir)) {
    if (!f.endsWith(".json")) continue;
    const rec = JSON.parse(fs.readFileSync(path.join(replayDir, f), "utf8"));
    replayIdx.push({ id: rec.id, at: rec.at, by: rec.by || "" });
  }
  replayIdx.sort((a, b) => String(a.at).localeCompare(String(b.at)));
} catch { /* no replay dir yet — starts empty */ }

/* Cumulative counters for /metrics — local mirror of what the deployed
 * worker reports through Analytics Engine (LOBBY_ANALYTICS binding). */
const stats = { admits: 0, rejects: 0, byRole: { host: 0, join: 0, spec: 0, queue: 0 },
                crashes: 0,
                /* lobby-path RTT histogram (ms) reported via ?ms= on role=queue */
                rttBuckets: { lt50: 0, lt100: 0, lt150: 0, lt250: 0, rest: 0 } };

function rttBucket(ms) {
  if (ms < 50) return "lt50";
  if (ms < 100) return "lt100";
  if (ms < 150) return "lt150";
  if (ms < 250) return "lt250";
  return "rest";
}

/* Reads a JSON body with a size cap; cb(null) on parse failure or destroy. */
const readJsonBody = (req, limit, cb) => {
  let raw = "";
  req.on("data", (c) => { raw += c; if (raw.length > limit) req.destroy(); });
  req.on("end", () => {
    let body = null;
    try { body = JSON.parse(raw); } catch { /* bad json */ }
    cb(body);
  });
};

const port = parseInt(process.argv[2] || process.env.PORT || "8799", 10);
const server = http.createServer((req, res) => {
  /* Public portal page — same HTML the worker serves at GET /. */
  if (req.url === "/" && req.method === "GET") {
    res.writeHead(200, { "content-type": "text/html; charset=utf-8" });
    res.end(BOARD_PAGE_HTML);
    return;
  }
  if (req.url === "/health") {
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ ok: true, service: "smash-netplay", rooms: rooms.size }));
    return;
  }
  if (req.url === "/metrics") {
    /* Prometheus text exposition — point Grafana's Cloudflare/Prometheus
     * datasource at this on a LAN/dev rig. */
    let sockets = 0, players = 0, specs = 0;
    for (const r of rooms.values()) {
      for (const [, i] of r.sockets) {
        if (i.kind === "up") continue;
        sockets++;
        if (i.slot >= 4) specs++; else players++;
      }
    }
    const lines = [
      "# HELP ssb64_lobby_rooms Open rooms",
      "# TYPE ssb64_lobby_rooms gauge",
      `ssb64_lobby_rooms ${rooms.size}`,
      "# HELP ssb64_lobby_sockets Open player/spectator sockets",
      "# TYPE ssb64_lobby_sockets gauge",
      `ssb64_lobby_sockets ${sockets}`,
      `ssb64_lobby_players ${players}`,
      `ssb64_lobby_spectators ${specs}`,
      "# HELP ssb64_lobby_admits_total Admitted connections by role",
      "# TYPE ssb64_lobby_admits_total counter",
      `ssb64_lobby_admits_total ${stats.admits}`,
      `ssb64_lobby_admits_role_total{role="host"} ${stats.byRole.host}`,
      `ssb64_lobby_admits_role_total{role="join"} ${stats.byRole.join}`,
      `ssb64_lobby_admits_role_total{role="spec"} ${stats.byRole.spec}`,
      `ssb64_lobby_admits_role_total{role="queue"} ${stats.byRole.queue}`,
      "# HELP ssb64_lobby_rejects_total Rejected admissions",
      "# TYPE ssb64_lobby_rejects_total counter",
      `ssb64_lobby_rejects_total ${stats.rejects}`,
      "# HELP ssb64_lobby_queue_rtt_ms Reported queue-join lobby RTT (ms) histogram",
      "# TYPE ssb64_lobby_queue_rtt_ms counter",
      `ssb64_lobby_queue_rtt_ms{bucket="lt50"} ${stats.rttBuckets.lt50}`,
      `ssb64_lobby_queue_rtt_ms{bucket="lt100"} ${stats.rttBuckets.lt100}`,
      `ssb64_lobby_queue_rtt_ms{bucket="lt150"} ${stats.rttBuckets.lt150}`,
      `ssb64_lobby_queue_rtt_ms{bucket="lt250"} ${stats.rttBuckets.lt250}`,
      `ssb64_lobby_queue_rtt_ms{bucket="rest"} ${stats.rttBuckets.rest}`,
      "# HELP ssb64_lobby_crashes_total Opt-in crash reports received",
      "# TYPE ssb64_lobby_crashes_total counter",
      `ssb64_lobby_crashes_total ${stats.crashes}`,
      "",
    ];
    res.writeHead(200, { "content-type": "text/plain; version=0.0.4" });
    res.end(lines.join("\n"));
    return;
  }
  if (req.url === "/crash" && req.method === "POST") {
    /* Opt-in crash telemetry (client gCrashReports.Enabled): append one
     * JSONL record per report — build/crash fields + a capped log tail.
     * No body limit is implied by the client beyond its own caps, so cap
     * the intake here too (64 KiB). */
    let raw = "";
    req.on("data", (c) => { raw += c; if (raw.length > 65536) req.destroy(); });
    req.on("end", () => {
      try {
        const doc = JSON.parse(raw);
        if (doc && doc.kind === "crash" &&
            typeof doc.info === "string" && doc.info.length <= 8192 &&
            typeof doc.log_tail === "string" && doc.log_tail.length <= 8192 &&
            (doc.lus_tail === undefined ||
             (typeof doc.lus_tail === "string" && doc.lus_tail.length <= 8192))) {
          fs.appendFileSync(path.join(dataDir, "crashes.jsonl"),
            JSON.stringify({ at: new Date().toISOString(), ...doc }) + "\n");
          stats.crashes++;
        }
      } catch { /* malformed report — drop silently */ }
      res.writeHead(204);
      res.end();
    });
    return;
  }
  if (req.url === "/leaderboard" && req.method === "GET") {
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ entries: lb.list() }));
    return;
  }
  if (req.url === "/submit" && req.method === "POST") {
    readJsonBody(req, 4096, (body) => {
      const result = lb.submit(body);
      if (result.kind !== "ok") {
        res.writeHead(400, { "content-type": "application/json" });
        res.end(JSON.stringify({ error: result.reason }));
        return;
      }
      saveLb();
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true, rank: result.rank }));
    });
    return;
  }
  if (req.url === "/result" && req.method === "POST") {
    readJsonBody(req, 4096, (body) => {
      const result = lb.reportResult(body);
      if (result.kind === "reject") {
        res.writeHead(400, { "content-type": "application/json" });
        res.end(JSON.stringify({ error: result.reason }));
        return;
      }
      saveLb();
      /* A verified match_id settles any live bracket pairing waiting on
       * it - tournaments ride on the same two-key corroboration. */
      if (result.kind === "verified" && result.winner && !result.draw) {
        const changed = settleLiveBrackets(
          brackets.values(), body.match_id, result.winner, result.loser);
        if (changed.length) saveBrackets();
      }
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify(result));
    });
    return;
  }
  /* Community moderation report — same domain semantics as the worker. */
  if (req.url === "/report" && req.method === "POST") {
    readJsonBody(req, 4096, (body) => {
      const result = lb.report(body);
      if (result.kind !== "ok") {
        res.writeHead(400, { "content-type": "application/json" });
        res.end(JSON.stringify({ error: result.reason }));
        return;
      }
      saveLb();
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify(result));
    });
    return;
  }
  /* Public service stats — same surface as the worker DO /stats. */
  if (req.url === "/stats") {
    let sockets = 0, players = 0, specs = 0;
    for (const r of rooms.values()) {
      for (const [, i] of r.sockets) {
        if (i.kind === "up") continue;
        sockets++;
        if (i.slot >= 4) specs++; else players++;
      }
    }
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({
      rooms: rooms.size, sockets, players, spectators: specs,
      admits: stats.admits, rejects: stats.rejects,
      leaderboard_entries: lb.byName.size,
      pending_results: lb.pending.size,
      verified_matches: lb.settled.size,
      crash_reports: stats.crashes,
      shared_replays: replayIdx.length,
      ...lb.fleetStats(),
    }));
    return;
  }
  /* Replay sharing — POST {data: <base64 .ssr>, by?} mints an id; GET
   * /replay/<id> serves it back. Stored as files under replays/ next to
   * leaderboard.json; ring capped at 200. */
  if (req.url === "/replay" && req.method === "POST") {
    readJsonBody(req, 600 * 1024, (body) => {
      const rec0 = validateReplayBody(body);
      if (!rec0) {
        res.writeHead(400, { "content-type": "application/json" });
        res.end(JSON.stringify({ error: "bad-request" }));
        return;
      }
      const id = mintReplayId();
      const rec = { id, at: new Date().toISOString(), by: rec0.by };
      try {
        fs.mkdirSync(replayDir, { recursive: true });
        writeJsonAtomic(path.join(replayDir, `${id}.json`),
          { ...rec, data: rec0.data });
      } catch { /* ro fs */ }
      replayIdx.push(rec);
      for (const old of evictOldest(replayIdx)) {
        try { fs.unlinkSync(path.join(replayDir, `${old.id}.json`)); } catch { /* gone */ }
      }
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true, id }));
    });
    return;
  }
    /* Replay index - GET /replays lists shared replay metadata (no blob
   * data) newest-first so the portal can render a browse list. */
  if (req.url === "/replays" && req.method === "GET") {
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ replays: [...replayIdx].reverse() }));
    return;
  }
  if (req.url.startsWith("/replay/") && req.method === "GET") {
    const id = req.url.slice("/replay/".length).split("?")[0];
    if (!REPLAY_ID_RE.test(id)) {
      res.writeHead(400, { "content-type": "application/json" });
      res.end(JSON.stringify({ error: "bad-id" }));
      return;
    }
    let rec = null;
    try {
      rec = JSON.parse(fs.readFileSync(path.join(replayDir, `${id}.json`), "utf8"));
    } catch { /* missing */ }
    if (!rec) {
      res.writeHead(404, { "content-type": "application/json" });
      res.end(JSON.stringify({ error: "no-such-replay" }));
      return;
    }
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify(rec));
    return;
  }
    /* Recent verified matches - GET /history (all) or /history?name=X
   * filtered to that player. Same surface as the worker DO. */
  if (req.method === "GET" &&
      (req.url === "/history" || req.url.startsWith("/history?"))) {
    const name = sanitizeName(
      new URL(req.url, "http://lobby").searchParams.get("name") || "") || null;
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({ matches: lb.historyFor(name) }));
    return;
  }
  /* Player card - GET /player?name=X: board row + rank + recent verified
   * matches. 404 for unclaimed names. Same surface as the worker DO. */
  if (req.method === "GET" &&
      (req.url === "/player" || req.url.startsWith("/player?"))) {
    const name = sanitizeName(
      new URL(req.url, "http://lobby").searchParams.get("name") || "");
    const card = name ? lb.playerCard(name) : null;
    if (!card) {
      res.writeHead(404, { "content-type": "application/json" });
      res.end(JSON.stringify({ error: "no-such-player" }));
      return;
    }
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify(card));
    return;
  }
  /* Tournaments - create/join/start/view. Bracket results aren't
   * trusted to a single reporter: pairings settle only when the same
   * match_id is verified through /result by distinct claim keys. */
  if (req.url === "/tournament" && req.method === "POST") {
    readJsonBody(req, 4096, (body) => {
      if (!body || typeof body !== "object") {
        res.writeHead(400).end(JSON.stringify({ error: "bad-request" }));
        return;
      }
      const name = sanitizeBracketName(body.name);
      const by = sanitizeName(body.by);
      const key = typeof body.key === "string" ? body.key : "";
      if (!name || !by || !lb.keyOwns(by, key)) {
        res.writeHead(400).end(JSON.stringify({ error: "bad-request" }));
        return;
      }
      const live = [...brackets.values()].filter((b) => b.status !== "done").length;
      if (live >= BR_MAX_LIVE) {
        res.writeHead(400).end(JSON.stringify({ error: "too-many-live" }));
        return;
      }
      const id = mintBracketId();
      const b = Bracket.create(id, name, by, body.size);
      brackets.set(id, b);
      saveBrackets();
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ ok: true, id, size: b.size }));
    });
    return;
  }
  if (req.url === "/tournament/join" && req.method === "POST") {
    readJsonBody(req, 4096, (body) => {
      const b = brackets.get(String(body?.id || ""));
      const name = sanitizeName(body?.name);
      const key = typeof body?.key === "string" ? body.key : "";
      /* Banned/muted players can't post results — a bracket pairing with
       * them inside could never verify, so they may not join at all. */
      const ent = lb.byName.get(name);
      if (!b || !name || !lb.keyOwns(name, key) ||
          (ent && (ent.banned || (ent.disputes || 0) >= LB_MUTE_DISPUTES))) {
        res.writeHead(400).end(JSON.stringify({ error: "bad-request" }));
        return;
      }
      const r = b.join(name);
      if (r.kind !== "ok") {
        res.writeHead(400).end(JSON.stringify({ error: r.reason }));
        return;
      }
      saveBrackets();
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify(r));
    });
    return;
  }
  if (req.url === "/tournament/start" && req.method === "POST") {
    readJsonBody(req, 4096, (body) => {
      const b = brackets.get(String(body?.id || ""));
      const key = typeof body?.key === "string" ? body.key : "";
      if (!b || !lb.keyOwns(b.owner, key)) {
        res.writeHead(400).end(JSON.stringify({ error: "bad-request" }));
        return;
      }
      const seedOrder = lb.list().map((e) => e.name);
      const r = b.start(seedOrder);
      if (r.kind !== "ok") {
        res.writeHead(400).end(JSON.stringify({ error: r.reason }));
        return;
      }
      saveBrackets();
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify(b.view()));
    });
    return;
  }
  if (req.method === "GET" &&
      (req.url === "/tournament" || req.url.startsWith("/tournament?"))) {
    const id = new URL(req.url, "http://lobby").searchParams.get("id");
    if (id) {
      const b = brackets.get(id);
      if (!b) {
        res.writeHead(404).end(JSON.stringify({ error: "no-such-bracket" }));
        return;
      }
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify(b.view()));
      return;
    }
    res.writeHead(200, { "content-type": "application/json" });
    res.end(JSON.stringify({
      brackets: [...brackets.values()].map((b) => b.view()),
    }));
    return;
  }
  res.writeHead(404);
  res.end("not found");
});
const wss = new WebSocketServer({ noServer: true });
server.on("upgrade", (req, socket, head) => {
  wss.handleUpgrade(req, socket, head, (ws) => wss.emit("connection", ws, req));
});
server.listen(port);

/* Wire a socket into a room: registration in domain + event plumbing here. */
function attach(room, ws, info) {
  room.attach(ws, info);
  ws.on("message", (data, isBinary) => {
    if (isBinary) {
      // Buffer is Uint8Array-compatible — domain works on the same surface.
      room.handleBinary(ws, data);
    } else {
      room.handleText(ws, data.toString());
    }
  });
  ws.on("close", () => room.onClose(ws));
  ws.on("error", () => room.onClose(ws));
}

/* Delete the room once it holds no sockets — failed joins and closed
 * sessions must not accumulate empty LobbyRoom objects. */
function gc(room) {
  if (room.sockets.size === 0) rooms.delete(room.code);
}

wss.on("connection", (ws, req) => {
  const url = new URL(req.url, "http://localhost");
  if (url.pathname !== "/ws") { ws.close(); return; }
  const code = (url.searchParams.get("room") || "").toUpperCase();
  const role = url.searchParams.get("role");
  const version = url.searchParams.get("v") === "2" ? 2 : 1;
  const send = (obj) => { try { ws.send(JSON.stringify(obj)); } catch { /* gone */ } };
  const fail = (closeCode, reason) => {
    send({ type: "error", reason });
    ws.close(closeCode, reason);
  };

  if (!CODE_RE.test(code) || !VALID_ROLES.has(role)) {
    fail(4000, "bad room or role");
    return;
  }
  // The no-such-room / room-full distinction is a room-existence oracle —
  // bound attempts per client IP.
  if (!allowIpJoin(req.socket.remoteAddress || "unknown")) {
    fail(4029, "rate-limited");
    return;
  }

  let room = rooms.get(code);
  // Only a host or a queuer mints a room: joins and uplinks to a nonexistent
  // room fail fast instead of accumulating empty objects in the map. A
  // queue connection on a fresh room lands as its host (matchmaking).
  const isUplink = url.searchParams.get("up") === "1";
  if (!room && ((role !== "host" && role !== "queue") || isUplink)) {
    fail(4000, "no-such-room");
    return;
  }
  if (!room) {
    room = new LobbyRoom(code);
    room.maybeDestroy = () => gc(room);
    rooms.set(code, room);
  }

  const action = room.admit({
    role, version, isUplink,
    uplinkSlot: parseInt(url.searchParams.get("slot") || "-1", 10),
    uplinkKey: url.searchParams.get("key") || "",
  });

  if (action.kind === "reject") {
    stats.rejects++;
    fail(action.reason === "rate-limited" ? 4029 : 4000, action.reason);
    gc(room);
    return;
  }
  if (action.kind === "uplink") {
    attach(room, ws, action.info);
    return;
  }

  // kind === "attach": optional room-reset, then welcome + follow-up.
  if (action.closeAll) {
    for (const [s] of room.sockets) {
      try { s.close(4002, "room-reset"); } catch { /* gone */ }
    }
    room.sockets.clear();
  }
  attach(room, ws, action.info);
  stats.admits++;
  if (role in stats.byRole) stats.byRole[role]++;
  if (role === "queue") {
    const ms = parseInt(url.searchParams.get("ms") || "", 10);
    if (Number.isFinite(ms) && ms > 0 && ms < 10000) stats.rttBuckets[rttBucket(ms)]++;
  }
  send(action.welcome);
  if (action.after === "introduceV1") room.introduceV1();
  else if (action.after === "broadcastRoster") room.broadcastRoster();
});

// Idle sweep across all rooms (client keepalive cadence is 20 s; 90 s of
// silence means a dead peer that will never emit "close").
setInterval(() => {
  for (const r of rooms.values()) r.sweepIdle();
}, IDLE_SWEEP_MS).unref();

/* Graceful shutdown: state is already flushed on every mutation (atomic
 * temp+rename), so shutdown only needs to stop accepting, close live
 * sockets so clients reconnect cleanly, then exit. */
let shuttingDown = false;
function shutdown(sig) {
  if (shuttingDown) return;
  shuttingDown = true;
  console.log(`node-lobby: ${sig} — draining ${rooms.size} room(s)`);
  server.close();
  for (const r of rooms.values()) {
    for (const [s] of r.sockets) {
      try { s.close(1001, "server-shutdown"); } catch { /* gone */ }
    }
  }
  /* Don't hang on half-open sockets — force out after a short drain. */
  setTimeout(() => process.exit(0), 1500).unref();
  server.closeAllConnections?.();
  process.exitCode = 0;
}
process.on("SIGTERM", () => shutdown("SIGTERM"));
process.on("SIGINT", () => shutdown("SIGINT"));

console.log(`node-lobby listening on :${port} (data: ${dataDir})`);
