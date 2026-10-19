// BattleShip netplay lobby — Cloudflare Worker entrypoint.
//
// Thin adapter over the shared room domain (src/domain/room.mjs): this
// file owns ONLY the Workers/DO plumbing — WebSocketPair 101 responses,
// addEventListener wiring, Blob→ArrayBuffer normalization with a
// per-socket order queue, and setAlarm idle sweeps. All room semantics
// live in the domain so the local mirror (node-lobby.mjs) can't drift.

import {
  CODE_RE, VALID_ROLES, IDLE_SWEEP_MS, makeJoinLimiter,
} from "../../domain/protocol.mjs";
import { LobbyRoom } from "../../domain/room.mjs";
import { Leaderboard } from "../../domain/leaderboard.mjs";
import {
  REPLAY_ID_RE, mintReplayId, validateReplayBody, evictOldest,
} from "../../domain/replay.mjs";

// Per-isolate IP join accounting (approximate — isolates aren't shared,
// and that's fine: this bounds scanning cost, it isn't a security
// boundary).
const allowIpJoin = makeJoinLimiter();

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (url.pathname === "/health") {
      return Response.json({ ok: true, service: "smash-netplay" });
    }
    // Self-reported rating board lives on a dedicated DO (HTTP only — no
    // ws). Same stub mechanism as rooms, fixed name.
    if (url.pathname === "/leaderboard" || url.pathname === "/submit" ||
        url.pathname === "/result" || url.pathname === "/report" ||
        url.pathname === "/crash" || url.pathname === "/crashes" ||
        url.pathname === "/stats" || url.pathname.startsWith("/replay")) {
      const stub = env.ROOMS.get(env.ROOMS.idFromName("leaderboard-global"));
      return stub.fetch(request);
    }
    if (url.pathname !== "/ws") {
      return new Response("not found", { status: 404 });
    }
    if (request.headers.get("Upgrade") !== "websocket") {
      return new Response("expected websocket", { status: 426 });
    }
    const room = (url.searchParams.get("room") || "").toUpperCase();
    const role = url.searchParams.get("role");
    if (!CODE_RE.test(room) || !VALID_ROLES.has(role)) {
      return new Response("bad room or role", { status: 400 });
    }
    // Cross-code scan throttle: room-existence replies differ
    // ("no-such-room" vs "room-full"), so joining is the only oracle a
    // code guesser has — make it cost connections, not requests (AUD-25).
    const ip = request.headers.get("cf-connecting-ip") || "unknown";
    if (!allowIpJoin(ip)) {
      return new Response("rate-limited", { status: 429 });
    }
    const stub = env.ROOMS.get(env.ROOMS.idFromName(room));
    return stub.fetch(request);
  },
};

export class NetplayRoom extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    // Non-hibernating sockets: every open ws lives in room.sockets and
    // keeps the DO resident, so in-memory state is authoritative.
    this.room = null;   // LobbyRoom, created lazily on the first admitted fetch
    this.sweepArmed = false;
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

  /* HTTP-only leaderboard paths — intercepted before any ws/room logic
   * (these requests carry no ?room= param). Persisted via DO storage so
   * the board survives isolate eviction. */
  async fetchLeaderboard(request, url) {
    if (url.pathname === "/leaderboard") {
      const rows = (await this.ctx.storage.get("lb")) || [];
      return Response.json({ entries: Leaderboard.fromJSON(rows).list() });
    }
    if (url.pathname === "/submit" && request.method === "POST") {
      let body = null;
      try { body = await request.json(); } catch { /* bad json */ }
      const rows = await this.ctx.storage.get("lb");
      const lb = Leaderboard.fromJSON(rows);
      const result = lb.submit(body);
      if (result.kind !== "ok") {
        return Response.json({ error: result.reason }, { status: 400 });
      }
      await this.ctx.storage.put("lb", lb.toJSON());
      return Response.json({ ok: true, rank: result.rank });
    }
    /* Corroborated match result — the rating-moving path. Both clients
     * report the same match_id; the second report either verifies (Elo
     * applied) or mismatches (dispute counted). */
    if (url.pathname === "/result" && request.method === "POST") {
      let body = null;
      try { body = await request.json(); } catch { /* bad json */ }
      const rows = await this.ctx.storage.get("lb");
      const lb = Leaderboard.fromJSON(rows);
      const result = lb.reportResult(body);
      if (result.kind === "reject") {
        return Response.json({ error: result.reason }, { status: 400 });
      }
      await this.ctx.storage.put("lb", lb.toJSON());
      return Response.json(result);
    }
    /* Opt-in crash telemetry (gCrashReports.Enabled): {kind:"crash",
     * info, log_tail} — ring of the last 50 in DO storage, counted via
     * analytics for the Grafana board. */
    if (url.pathname === "/crash" && request.method === "POST") {
      let body = null;
      try { body = await request.json(); } catch { /* bad json */ }
      if (body && body.kind === "crash" &&
          typeof body.info === "string" && body.info.length <= 8192 &&
          typeof body.log_tail === "string" && body.log_tail.length <= 8192 &&
          (body.lus_tail === undefined ||
           (typeof body.lus_tail === "string" && body.lus_tail.length <= 8192))) {
        const ring = (await this.ctx.storage.get("crashes")) || [];
        ring.push({ at: Date.now(), info: body.info, log_tail: body.log_tail,
                    lus_tail: body.lus_tail || "" });
        while (ring.length > 50) ring.shift();
        await this.ctx.storage.put("crashes", ring);
        try {
          this.env.LOBBY_ANALYTICS?.writeDataPoint({
            blobs: ["crash", "report"], doubles: [0, 0], indexes: [""],
          });
        } catch { /* telemetry never blocks */ }
      }
      return new Response(null, { status: 204 });
    }
    /* Recent crash reports (operator debugging; same trust level as the
     * leaderboard — the reports are opt-in and carry no auth). */
    if (url.pathname === "/crashes" && request.method === "GET") {
      const ring = (await this.ctx.storage.get("crashes")) || [];
      return Response.json({ crashes: ring });
    }
    /* Community moderation: {name, key} — `key` is the reporter's own
     * claim key. BAN_REPORTS distinct keys in 24h flag the name banned;
     * banned keys stop mutating the board. */
    if (url.pathname === "/report" && request.method === "POST") {
      let body = null;
      try { body = await request.json(); } catch { /* bad json */ }
      const lb = Leaderboard.fromJSON(await this.ctx.storage.get("lb"));
      const result = lb.report(body);
      if (result.kind !== "ok") {
        return Response.json({ error: result.reason }, { status: 400 });
      }
      await this.ctx.storage.put("lb", lb.toJSON());
      try {
        this.env.LOBBY_ANALYTICS?.writeDataPoint({
          blobs: ["report", result.banned ? "banned" : "counted"],
          doubles: [result.reports, 0], indexes: [""],
        });
      } catch { /* telemetry never blocks */ }
      return Response.json(result);
    }
    /* Public service stats — stable unauthenticated surface for the
     * dashboard and any external API consumer. */
    if (url.pathname === "/stats") {
      const lb = Leaderboard.fromJSON(await this.ctx.storage.get("lb"));
      const crashes = (await this.ctx.storage.get("crashes")) || [];
      const replays = (await this.ctx.storage.get("replays-idx")) || [];
      return Response.json({
        leaderboard_entries: lb.byName.size,
        pending_results: lb.pending.size,
        verified_matches: lb.settled.size,
        crash_reports: crashes.length,
        shared_replays: replays.length,
      });
    }
    /* Replay sharing — POST {data: <base64 .ssr>} mints a short id and
     * stores it; GET /replay/<id> serves it back. Replays are a few KB;
     * cap the index at 200 and evict oldest first. */
    if (url.pathname === "/replay" && request.method === "POST") {
      let body = null;
      try { body = await request.json(); } catch { /* bad json */ }
      const rec0 = validateReplayBody(body);
      if (!rec0) {
        return Response.json({ error: "bad-request" }, { status: 400 });
      }
      const idx = (await this.ctx.storage.get("replays-idx")) || [];
      const id = mintReplayId();
      const rec = { id, at: Date.now(), by: rec0.by };
      await this.ctx.storage.put(`rp:${id}`, { ...rec, data: rec0.data });
      idx.push(rec);
      for (const old of evictOldest(idx)) {
        await this.ctx.storage.delete(`rp:${old.id}`);
      }
      await this.ctx.storage.put("replays-idx", idx);
      return Response.json({ ok: true, id });
    }
    if (url.pathname.startsWith("/replay/") && request.method === "GET") {
      const id = url.pathname.slice("/replay/".length);
      if (!REPLAY_ID_RE.test(id)) {
        return Response.json({ error: "bad-id" }, { status: 400 });
      }
      const rec = await this.ctx.storage.get(`rp:${id}`);
      if (!rec) return Response.json({ error: "no-such-replay" }, { status: 404 });
      return Response.json(rec);
    }
    return new Response("not found", { status: 404 });
  }

  fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/leaderboard" || url.pathname === "/submit" ||
        url.pathname === "/result" || url.pathname === "/report" ||
        url.pathname === "/crash" || url.pathname === "/crashes" ||
        url.pathname === "/stats" || url.pathname.startsWith("/replay")) {
      return this.fetchLeaderboard(request, url);
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
