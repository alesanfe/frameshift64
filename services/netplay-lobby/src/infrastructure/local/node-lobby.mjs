// node-lobby — local mirror of the Cloudflare Worker lobby, for tests
// and LAN rigs. This file is ONLY the Node adapter: all room semantics
// (admission, routing, rate limits, roster, teardown) live in
// src/domain/, and every HTTP route lives in src/interfaces/http/app.mjs
// — the same Hono app the deployed worker's Durable Object runs.
//
// Usage: node src/infrastructure/local/node-lobby.mjs [port]
//        PORT=8799 LOBBY_DATA_DIR=/var/lib/lobby npm start

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { serve } from "@hono/node-server";
import { createNodeWebSocket } from "@hono/node-ws";

import {
  CODE_RE, VALID_ROLES, IDLE_SWEEP_MS, makeJoinLimiter,
} from "../../domain/protocol.mjs";
import { LobbyRoom } from "../../domain/room.mjs";
import { Leaderboard } from "../../domain/leaderboard.mjs";
import { Bracket } from "../../domain/bracket.mjs";
import { CRASH_RING } from "../../domain/crash.mjs";
import { createLobbyApp } from "../../interfaces/http/app.mjs";

const rooms = new Map();    // code -> LobbyRoom

/* Runtime state root: LOBBY_DATA_DIR for deployments (a Docker volume
 * lands wherever the image puts it); default keeps the beside-the-script
 * location the tests rely on. */
const dataDir = process.env.LOBBY_DATA_DIR ||
  path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "..", "..");

/* temp+fsync+rename+dirsync: writeFileSync alone leaves bytes in the page
 * cache, and rename alone leaves the directory entry unflushed — a power
 * loss between the two could drop both. fsync the temp before the rename
 * and the parent directory after it. */
const writeJsonAtomic = (dest, obj) => {
  const tmp = `${dest}.tmp-${process.pid}`;
  const fd = fs.openSync(tmp, "w");
  try {
    fs.writeSync(fd, JSON.stringify(obj));
    fs.fsyncSync(fd);
  } finally {
    fs.closeSync(fd);
  }
  fs.renameSync(tmp, dest);
  try {
    const dirFd = fs.openSync(path.dirname(dest), "r");
    try { fs.fsyncSync(dirFd); } finally { fs.closeSync(dirFd); }
  } catch { /* Windows can't fsync a dir handle — rename is enough there */ }
};

/* Leaderboard persisted under dataDir — runtime state, not repo data.
 * A torn or corrupt store must not crash-loop the service: recover to an
 * empty board rather than dying at module init (chaos test phase 3). */
const lbPath = path.join(dataDir, "leaderboard.json");
let lb = new Leaderboard();
try {
  lb = Leaderboard.fromJSON(
    JSON.parse(fs.existsSync(lbPath) ? fs.readFileSync(lbPath, "utf8") : "[]"));
} catch { /* corrupt store — start fresh */ }

/* Brackets persist beside the leaderboard — one file, id -> Bracket JSON.
 * Cap live brackets so a troll can't mint unbounded state (the DO mirrors
 * this cap via domain BR_MAX_LIVE). */
const brPath = path.join(dataDir, "brackets.json");
const brackets = new Map();
try {
  for (const [id, o] of Object.entries(JSON.parse(fs.readFileSync(brPath, "utf8")))) {
    brackets.set(id, Bracket.fromJSON(o));
  }
} catch { /* no bracket store yet */ }

/* Shared replays land under dataDir — runtime artifacts, not repo data.
 * In-memory index mirrors the worker DO's replays-idx. */
const replayDir = path.join(dataDir, "replays");
const crashPath = path.join(dataDir, "crashes.jsonl");
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

const allowIpJoin = makeJoinLimiter();  // cross-code scan throttle (AUD-25)

/* Live-room census shared by /stats and /metrics. */
function roomCensus() {
  let sockets = 0, players = 0, specs = 0;
  for (const r of rooms.values()) {
    for (const [, i] of r.sockets) {
      if (i.kind === "up") continue;
      sockets++;
      if (i.slot >= 4) specs++; else players++;
    }
  }
  return { sockets, players, specs };
}

/* Persistence + telemetry deps for the shared HTTP app — this adapter
 * stores everything as files under dataDir (atomic temp+rename for the
 * JSON stores, JSONL append for crash reports). */
const deps = {
  loadLb: async () => lb,
  saveLb: async () => {
    try { writeJsonAtomic(lbPath, lb.toJSON()); } catch { /* ro fs */ }
  },
  loadBrackets: async () => brackets,
  saveBrackets: async (map) => {
    try {
      writeJsonAtomic(brPath,
        Object.fromEntries([...map].map(([k, b]) => [k, b.toJSON()])));
    } catch { /* ro fs */ }
  },
  replayIndex: {
    get: async () => replayIdx,
    /* The file tree IS the durable index — rebuilt on boot; nothing to
     * write here. */
    save: async () => {},
  },
  replayBlobs: {
    get: async (id) => {
      try {
        return JSON.parse(
          fs.readFileSync(path.join(replayDir, `${id}.json`), "utf8"));
      } catch { return null; }
    },
    put: async (id, rec) => {
      try {
        fs.mkdirSync(replayDir, { recursive: true });
        writeJsonAtomic(path.join(replayDir, `${id}.json`), rec);
      } catch { /* ro fs */ }
    },
    delete: async (id) => {
      try { fs.unlinkSync(path.join(replayDir, `${id}.json`)); } catch { /* gone */ }
    },
  },
  crashes: {
    append: async (rec) => {
      try {
        fs.appendFileSync(crashPath, JSON.stringify(rec) + "\n");
      } catch { /* ro fs */ }
      stats.crashes++;
    },
    /* Reads only a 256 KiB tail window so a long-lived jsonl can't blow
     * the response budget; malformed tails skip per-line. */
    list: async () => {
      const ring = [];
      try {
        const fd = fs.openSync(crashPath, "r");
        try {
          const size = fs.fstatSync(fd).size;
          const off = Math.max(0, size - 256 * 1024);
          const buf = Buffer.alloc(size - off);
          fs.readSync(fd, buf, 0, buf.length, off);
          for (const line of buf.toString("utf8").split("\n")) {
            if (!line.trim()) continue;
            try { ring.push(JSON.parse(line)); } catch { /* torn tail */ }
          }
        } finally { fs.closeSync(fd); }
      } catch { /* no reports yet — empty ring */ }
      return ring.slice(-CRASH_RING);
    },
    count: async () => stats.crashes,
  },
  statsExtra: () => {
    const { sockets, players, specs } = roomCensus();
    return {
      rooms: rooms.size, sockets, players, spectators: specs,
      admits: stats.admits, rejects: stats.rejects,
    };
  },
  healthExtra: () => ({ rooms: rooms.size }),
  metrics: () => {
    const { sockets, players, specs } = roomCensus();
    return [
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
    ].join("\n");
  },
};

/* One app carries the whole HTTP surface; the WS route is registered on
 * it too so the framework owns every inbound request. */
const app = createLobbyApp(deps);
const { injectWebSocket, upgradeWebSocket } = createNodeWebSocket({ app });

/* Wire a socket into a room: registration in domain + socket lookup here.
 * node-ws hands the same WSContext to every callback, so it doubles as
 * the domain socket token — the room map key for send/close/readyState. */
const roomOf = new Map();   // WSContext -> LobbyRoom

function attach(room, ws, info) {
  room.attach(ws, info);
  roomOf.set(ws, room);
}

/* Delete the room once it holds no sockets — failed joins and closed
 * sessions must not accumulate empty LobbyRoom objects. */
function gc(room) {
  if (room.sockets.size === 0) rooms.delete(room.code);
}

function detach(ws) {
  const room = roomOf.get(ws);
  roomOf.delete(ws);
  if (room) {
    room.onClose(ws);
    gc(room);
  }
}

app.get("/ws", upgradeWebSocket((c) => {
  const url = new URL(c.req.url);
  const ip = c.env?.incoming?.socket?.remoteAddress || "unknown";
  const code = (url.searchParams.get("room") || "").toUpperCase();
  const role = url.searchParams.get("role");
  const version = url.searchParams.get("v") === "2" ? 2 : 1;
  const send = (ws, obj) => {
    try { ws.send(JSON.stringify(obj)); } catch { /* gone */ }
  };
  const fail = (ws, closeCode, reason) => {
    send(ws, { type: "error", reason });
    ws.close(closeCode, reason);
  };

  return {
    onOpen(evt, ws) {
      if (!CODE_RE.test(code) || !VALID_ROLES.has(role)) {
        fail(ws, 4000, "bad room or role");
        return;
      }
      // The no-such-room / room-full distinction is a room-existence
      // oracle — bound attempts per client IP.
      if (!allowIpJoin(ip)) {
        fail(ws, 4029, "rate-limited");
        return;
      }
      let room = rooms.get(code);
      // Only a host or a queuer mints a room: joins and uplinks to a
      // nonexistent room fail fast instead of accumulating empty objects
      // in the map. A queue connection on a fresh room lands as its host.
      const isUplink = url.searchParams.get("up") === "1";
      if (!room && ((role !== "host" && role !== "queue") || isUplink)) {
        fail(ws, 4000, "no-such-room");
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
        fail(ws, action.reason === "rate-limited" ? 4029 : 4000, action.reason);
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
        if (Number.isFinite(ms) && ms > 0 && ms < 10000) {
          stats.rttBuckets[rttBucket(ms)]++;
        }
      }
      send(ws, action.welcome);
      if (action.after === "introduceV1") room.introduceV1();
      else if (action.after === "broadcastRoster") room.broadcastRoster();
    },
    onMessage(evt, ws) {
      const room = roomOf.get(ws);
      if (!room) return;
      // node-ws delivers text as a utf-8 string and binary as ArrayBuffer.
      if (typeof evt.data === "string") {
        room.handleText(ws, evt.data);
      } else {
        room.handleBinary(ws, new Uint8Array(evt.data));
      }
    },
    onClose(evt, ws) { detach(ws); },
    onError(evt, ws) { detach(ws); },
  };
}));

const port = parseInt(process.argv[2] || process.env.PORT || "8799", 10);
const server = serve({ fetch: app.fetch, port });
injectWebSocket(server);

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
