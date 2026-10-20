// BattleShip netplay lobby — Cloudflare Worker entrypoint.
//
// Thin adapter over the shared lobby surface: HTTP routes live in
// src/interfaces/http/app.mjs (the same Hono app the local node mirror
// serves), room semantics in src/domain/. This file owns ONLY the
// Workers/DO plumbing — WebSocketPair 101 responses, Blob→ArrayBuffer
// normalization with a per-socket order queue, setAlarm idle sweeps, and
// ctx.storage persistence behind the app's deps seam.

import { DurableObject } from "cloudflare:workers";
import { Hono } from "hono";

import {
  CODE_RE, VALID_ROLES, IDLE_SWEEP_MS, makeJoinLimiter,
} from "../../domain/protocol.mjs";
import { LobbyRoom } from "../../domain/room.mjs";
import { Leaderboard } from "../../domain/leaderboard.mjs";
import { Bracket } from "../../domain/bracket.mjs";
import { CRASH_RING } from "../../domain/crash.mjs";
import { BOARD_PAGE_HTML } from "../../domain/board_page.mjs";
import { createLobbyApp } from "../../interfaces/http/app.mjs";

/* Paths served by the leaderboard Durable Object (HTTP only — no ws).
 * Same whitelist the raw dispatcher used. */
const isLbPath = (p) =>
  p === "/leaderboard" || p === "/history" || p === "/player" ||
  p === "/submit" || p === "/result" || p === "/report" ||
  p === "/crash" || p === "/crashes" || p === "/stats" ||
  p.startsWith("/match/") ||
  p.startsWith("/replay") || p.startsWith("/tournament");

// Per-isolate IP join accounting (approximate — isolates aren't shared,
// and that's fine: this bounds scanning cost, it isn't a security
// boundary).
const allowIpJoin = makeJoinLimiter();

/* Worker-level dispatcher: portal + health locally, leaderboard-family
 * paths to the global DO, /ws to the per-room DO. */
const app = new Hono();

app.get("/", (c) =>
  c.body(BOARD_PAGE_HTML, 200, { "content-type": "text/html; charset=utf-8" }));

app.get("/health", (c) => c.json({ ok: true, service: "smash-netplay" }));

app.all("*", (c) => {
  const url = new URL(c.req.raw.url);
  if (isLbPath(url.pathname)) {
    const stub = c.env.ROOMS.get(c.env.ROOMS.idFromName("leaderboard-global"));
    return stub.fetch(c.req.raw);
  }
  if (url.pathname !== "/ws") {
    return c.text("not found", 404);
  }
  if (c.req.header("Upgrade") !== "websocket") {
    return c.text("expected websocket", 426);
  }
  const room = (url.searchParams.get("room") || "").toUpperCase();
  const role = url.searchParams.get("role");
  if (!CODE_RE.test(room) || !VALID_ROLES.has(role)) {
    return c.text("bad room or role", 400);
  }
  // Cross-code scan throttle: room-existence replies differ
  // ("no-such-room" vs "room-full"), so joining is the only oracle a
  // code guesser has — make it cost connections, not requests (AUD-25).
  const ip = c.req.header("cf-connecting-ip") || "unknown";
  if (!allowIpJoin(ip)) {
    return c.text("rate-limited", 429);
  }
  const stub = c.env.ROOMS.get(c.env.ROOMS.idFromName(room));
  return stub.fetch(c.req.raw);
});

export default { fetch: (request, env, ctx) => app.fetch(request, env, ctx) };

/* Persistence + telemetry deps for the shared HTTP app, backed by this
 * DO's transactional storage. The leaderboard DO gets one of these; room
 * DOs never see lb paths (the worker only forwards them to the global
 * stub), so per-room instances never exercise it. */
function doDeps(ctx, env) {
  const store = ctx.storage;
  return {
    loadLb: async () =>
      Leaderboard.fromJSON((await store.get("lb")) || []),
    saveLb: (lb) => store.put("lb", lb.toJSON()),
    loadBrackets: async () => new Map(
      Object.entries((await store.get("brackets")) || {})
        .map(([id, o]) => [id, Bracket.fromJSON(o)])),
    saveBrackets: (map) => store.put("brackets",
      Object.fromEntries([...map].map(([k, b]) => [k, b.toJSON()]))),
    replayIndex: {
      get: async () => (await store.get("replays-idx")) || [],
      save: (idx) => store.put("replays-idx", idx),
    },
    replayBlobs: {
      get: (id) => store.get(`rp:${id}`),
      put: (id, rec) => store.put(`rp:${id}`, rec),
      delete: (id) => store.delete(`rp:${id}`),
    },
    crashes: {
      append: async (rec) => {
        const ring = (await store.get("crashes")) || [];
        ring.push(rec);
        while (ring.length > CRASH_RING) ring.shift();
        await store.put("crashes", ring);
      },
      list: async () => (await store.get("crashes")) || [],
      count: async () => ((await store.get("crashes")) || []).length,
    },
    analytics: (point) => {
      try { env.LOBBY_ANALYTICS?.writeDataPoint(point); } catch { /* telemetry never blocks */ }
    },
    statsExtra: () => ({}),
    healthExtra: () => ({}),
    /* metrics: undefined — the DO doesn't expose Prometheus text;
     * Cloudflare telemetry goes through Analytics Engine. */
  };
}

export class NetplayRoom extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    // Non-hibernating sockets: every open ws lives in room.sockets and
    // keeps the DO resident, so in-memory state is authoritative.
    this.room = null;   // LobbyRoom, created lazily on the first admitted fetch
    this.sweepArmed = false;
    /* The whole HTTP surface the worker forwards here — shared with the
     * local adapter so endpoint semantics can't drift. */
    this.http = createLobbyApp(doDeps(ctx, env));
  }

  /* ---- socket plumbing (adapter-owned) ---- */

  reject(reason) {
    const pair = new WebSocketPair();
    pair[1].accept();
    pair[1].send(JSON.stringify({ type: "error", reason }));
    pair[1].close(4000, reason);
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  attach(ws, info) {
    this.room.attach(ws, info);
    ws.addEventListener("message", (event) => this.onMessage(ws, event.data));
    ws.addEventListener("close", () => this.room.onClose(ws));
    ws.addEventListener("error", () => this.room.onClose(ws));
    this.armSweep();
  }

  /* Idle reaper (AUD-23): arm a DO alarm once sockets exist; the alarm
   * closes silent sockets and re-arms while any remain. A dead TCP peer
   * never fires "close" on its own, so without this a zombie holds its
   * slot (and the DO's billable residency) forever. */
  armSweep() {
    if (this.sweepArmed) return;
    this.sweepArmed = true;
    this.ctx.storage.setAlarm(Date.now() + IDLE_SWEEP_MS)
      .catch(() => { this.sweepArmed = false; });
  }

  async alarm() {
    this.sweepArmed = false;
    this.room.sweepIdle();
    if (this.room.sockets.size > 0) this.armSweep();
  }

  /* ---- connection ---- */

  fetch(request) {
    const url = new URL(request.url);
    /* HTTP-only leaderboard paths — delegated to the shared app (these
     * requests carry no ?room= param). */
    if (isLbPath(url.pathname)) {
      return this.http.fetch(request);
    }
    const code = (url.searchParams.get("room") || "").toUpperCase();
    const role = url.searchParams.get("role");
    const version = url.searchParams.get("v") === "2" ? 2 : 1;

    if (!this.room) {
      this.room = new LobbyRoom(code);
      this.room.onSocketActivity = () => this.armSweep();
      // maybeDestroy intentionally unset: the isolate evicts the DO.
    }

    const action = this.room.admit({
      role, version,
      isUplink: url.searchParams.get("up") === "1",
      uplinkSlot: parseInt(url.searchParams.get("slot") || "-1", 10),
      uplinkKey: url.searchParams.get("key") || "",
    });

    /* Optional Analytics Engine telemetry (LOBBY_ANALYTICS dataset binding):
     * one datapoint per admission — Grafana reads it via the Cloudflare
     * datasource plugin. No binding = no-op; failure never blocks a join.
     * doubles: [protocol version, lobby RTT ms hint (queue only, -1 absent)]
     * — the ms column is instrumentation for latency-aware matchmaking
     * once population data justifies partitioning. */
    try {
      const msHint = role === "queue"
        ? parseInt(url.searchParams.get("ms") || "-1", 10) : -1;
      this.env.LOBBY_ANALYTICS?.writeDataPoint({
        blobs: [role, action.kind],
        doubles: [version, Number.isFinite(msHint) ? msHint : -1],
        indexes: [action.kind === "reject" ? action.reason : ""],
      });
    } catch { /* telemetry must never break admission */ }

    if (action.kind === "reject") {
      return this.reject(action.reason);
    }

    const pair = new WebSocketPair();
    pair[1].accept();

    if (action.kind === "uplink") {
      this.attach(pair[1], action.info);
      return new Response(null, { status: 101, webSocket: pair[0] });
    }

    // kind === "attach": optional room-reset, then welcome + follow-up.
    if (action.closeAll) {
      for (const [ws] of this.room.sockets) {
        try { ws.close(4002, "room-reset"); } catch { /* gone */ }
      }
      this.room.sockets.clear();
    }
    this.attach(pair[1], action.info);
    pair[1].send(JSON.stringify(action.welcome));
    if (action.after === "introduceV1") this.room.introduceV1();
    else if (action.after === "broadcastRoster") this.room.broadcastRoster();
    return new Response(null, { status: 101, webSocket: pair[0] });
  }

  /* ---- websocket events ---- */

  onMessage(ws, message) {
    const info = this.room.info(ws);
    if (!info) return;

    if (typeof message !== "string") {
      // Relayed game packet. Production workerd delivers ws binary frames
      // as Blob (wrangler dev delivered ArrayBuffer); normalize through a
      // per-socket queue so frames keep arrival order.
      info.bin = (info.bin || Promise.resolve())
        .then(async () => this.room.handleBinary(ws,
          new Uint8Array(message instanceof Blob ? await message.arrayBuffer() : message)))
        .catch(() => {});
      return;
    }
    this.room.handleText(ws, message);
  }
}
