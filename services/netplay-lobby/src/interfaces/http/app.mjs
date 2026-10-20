// Shared HTTP surface for the netplay lobby — one Hono app used by both
// adapters:
//
//   * infrastructure/local/node-lobby.mjs   — @hono/node-server serve()
//   * infrastructure/cloudflare/worker.js   — NetplayRoom DO app.fetch()
//
// Adapters inject persistence + telemetry through `deps`; every route here
// is adapter-agnostic so HTTP semantics cannot drift between the local
// mirror and the deployed worker.
//
// deps = {
//   loadLb()      -> Promise<Leaderboard>   (fresh domain object per call)
//   saveLb(lb)    -> Promise<void>
//   loadBrackets()-> Promise<Map<id, Bracket>>
//   saveBrackets(map) -> Promise<void>
//   replayIndex:  { get()->Promise<[{id,at,by}]>, save(idx)->Promise<void> }
//   replayBlobs:  { get(id)->Promise<rec|null>, put(id,rec), delete(id) }
//   crashes:      { append(rec)->Promise<void>, list()->Promise<[]>,
//                   count()->Promise<number> }
//   analytics?(point)                         — optional datapoint sink
//   statsExtra?()  -> object                  — adapter counters for /stats
//   healthExtra?() -> object                  — extra /health fields
//   metrics?()     -> string                  — Prometheus text; route
//                                               registered only when set
// }

import { Hono } from "hono";

import {
  sanitizeName, LB_MUTE_DISPUTES,
} from "../../domain/leaderboard.mjs";
import {
  Bracket, sanitizeBracketName, BR_MAX_LIVE,
  mintBracketId, settleLiveBrackets,
} from "../../domain/bracket.mjs";
import {
  REPLAY_ID_RE, mintReplayId, validateReplayBody, evictOldest,
} from "../../domain/replay.mjs";
import {
  isValidCrashReport, crashReportRecord,
} from "../../domain/crash.mjs";
import { BOARD_PAGE_HTML } from "../../interfaces/web/board_page.mjs";

const json = (c, obj, status = 200) => c.json(obj, status);

/* Reads and JSON-parses the body with a hard size cap — equivalent to the
 * old raw-handler req.destroy() contract but answered gracefully. */
async function readBody(c, limit) {
  const buf = await c.req.arrayBuffer().catch(() => null);
  if (!buf || buf.byteLength > limit) return null;
  try { return JSON.parse(new TextDecoder().decode(buf)); }
  catch { return null; }
}

export function createLobbyApp(deps) {
  const app = new Hono();

  /* Public portal page — same HTML on both adapters. */
  app.get("/", (c) =>
    c.body(BOARD_PAGE_HTML, 200, { "content-type": "text/html; charset=utf-8" }));

  app.get("/health", (c) =>
    json(c, { ok: true, service: "smash-netplay", ...(deps.healthExtra?.() || {}) }));

  /* Prometheus text exposition — local/LAN adapter only (the deployed
   * worker reports through Analytics Engine instead). */
  if (deps.metrics) {
    app.get("/metrics", (c) =>
      c.body(deps.metrics(), 200,
        { "content-type": "text/plain; version=0.0.4" }));
  }

  /* Opt-in crash telemetry (client gCrashReports.Enabled). */
  app.post("/crash", async (c) => {
    const doc = await readBody(c, 64 * 1024);
    if (isValidCrashReport(doc)) {
      await deps.crashes.append(
        { at: new Date().toISOString(), ...crashReportRecord(doc) });
      deps.analytics?.({ blobs: ["crash", "report"], doubles: [0, 0], indexes: [""] });
    }
    return c.body(null, 204);
  });

  /* Recent crash reports — operator debugging surface. */
  app.get("/crashes", async (c) =>
    json(c, { crashes: await deps.crashes.list() }));

  /* Self-reported rating board. */
  app.get("/leaderboard", async (c) =>
    json(c, { entries: (await deps.loadLb()).list() }));

  app.post("/submit", async (c) => {
    const lb = await deps.loadLb();
    const result = lb.submit(await readBody(c, 4096));
    if (result.kind !== "ok") return json(c, { error: result.reason }, 400);
    await deps.saveLb(lb);
    return json(c, { ok: true, rank: result.rank });
  });

  /* Corroborated match result — the rating-moving path. Both clients
   * report the same match_id; the second report either verifies (Elo
   * applied) or mismatches (dispute counted). A verified match also
   * settles any live bracket pairing waiting on it. */
  app.post("/result", async (c) => {
    const body = await readBody(c, 4096);
    const lb = await deps.loadLb();
    const result = lb.reportResult(body);
    if (result.kind === "reject") return json(c, { error: result.reason }, 400);
    await deps.saveLb(lb);
    if (result.kind === "verified" && result.winner && !result.draw) {
      const brackets = await deps.loadBrackets();
      const changed = settleLiveBrackets(
        brackets.values(), body.match_id, result.winner, result.loser);
      if (changed.length) await deps.saveBrackets(brackets);
    }
    return json(c, result);
  });

  /* Server-issued match: mints match_id + per-participant tokens. The
   * begin caller proves their own claim key; the opponent retrieves their
   * token via /match/claim. Reports on an issued match must present the
   * participant's token — so a fabricated match_id can't move Elo. */
  app.post("/match/begin", async (c) => {
    const lb = await deps.loadLb();
    const result = lb.beginMatch(await readBody(c, 4096));
    if (result.kind !== "ok") return json(c, { error: result.reason }, 400);
    await deps.saveLb(lb);
    return json(c, result);
  });

  app.post("/match/claim", async (c) => {
    const lb = await deps.loadLb();
    const result = lb.claimMatch(await readBody(c, 4096));
    if (result.kind !== "ok") return json(c, { error: result.reason }, 400);
    return json(c, result);
  });

  /* Community moderation report — BAN_REPORTS distinct keys in 24h flag
   * the name banned; banned keys stop mutating the board. */
  app.post("/report", async (c) => {
    const lb = await deps.loadLb();
    const result = lb.report(await readBody(c, 4096));
    if (result.kind !== "ok") return json(c, { error: result.reason }, 400);
    await deps.saveLb(lb);
    deps.analytics?.({
      blobs: ["report", result.banned ? "banned" : "counted"],
      doubles: [result.reports, 0], indexes: [""],
    });
    return json(c, result);
  });

  /* Recent verified matches — all, or filtered to ?name=X. */
  app.get("/history", async (c) => {
    const lb = await deps.loadLb();
    const name = sanitizeName(c.req.query("name") || "") || null;
    return json(c, { matches: lb.historyFor(name) });
  });

  /* Player card — board row + rank + recent verified matches. */
  app.get("/player", async (c) => {
    const lb = await deps.loadLb();
    const name = sanitizeName(c.req.query("name") || "");
    const card = name ? lb.playerCard(name) : null;
    if (!card) return json(c, { error: "no-such-player" }, 404);
    return json(c, card);
  });

  /* Public service stats — stable unauthenticated surface for the
   * portal, the dashboard, and external API consumers. */
  app.get("/stats", async (c) => {
    const lb = await deps.loadLb();
    return json(c, {
      ...(deps.statsExtra?.() || {}),
      leaderboard_entries: lb.byName.size,
      pending_results: lb.pending.size,
      verified_matches: lb.settled.size,
      crash_reports: await deps.crashes.count(),
      shared_replays: (await deps.replayIndex.get()).length,
      ...lb.fleetStats(),
    });
  });

  /* Replay sharing — POST {data: <base64 .ssr>, by?} mints an id;
   * GET /replay/<id> serves it back. Index is metadata only, newest
   * first, ring-capped via evictOldest. */
  app.post("/replay", async (c) => {
    const rec0 = validateReplayBody(await readBody(c, 600 * 1024));
    if (!rec0) return json(c, { error: "bad-request" }, 400);
    const id = mintReplayId();
    const rec = { id, at: new Date().toISOString(), by: rec0.by };
    const idx = await deps.replayIndex.get();
    await deps.replayBlobs.put(id, { ...rec, data: rec0.data });
    idx.push(rec);
    for (const old of evictOldest(idx)) await deps.replayBlobs.delete(old.id);
    await deps.replayIndex.save(idx);
    return json(c, { ok: true, id });
  });

  app.get("/replays", async (c) =>
    json(c, { replays: [...(await deps.replayIndex.get())].reverse() }));

  app.get("/replay/:id", async (c) => {
    const id = c.req.param("id");
    if (!REPLAY_ID_RE.test(id)) return json(c, { error: "bad-id" }, 400);
    const rec = await deps.replayBlobs.get(id);
    if (!rec) return json(c, { error: "no-such-replay" }, 404);
    return json(c, rec);
  });

  /* Tournaments — create/join/start/view. Bracket results aren't trusted
   * to a single reporter: pairings settle only when the same match_id is
   * verified through /result by both players' distinct claim keys. */
  app.post("/tournament", async (c) => {
    const body = await readBody(c, 4096);
    if (!body || typeof body !== "object") {
      return json(c, { error: "bad-request" }, 400);
    }
    const lb = await deps.loadLb();
    const name = sanitizeBracketName(body.name);
    const by = sanitizeName(body.by);
    const key = typeof body.key === "string" ? body.key : "";
    if (!name || !by || !lb.keyOwns(by, key)) {
      return json(c, { error: "bad-request" }, 400);
    }
    const brackets = await deps.loadBrackets();
    const live = [...brackets.values()].filter((b) => b.status !== "done").length;
    if (live >= BR_MAX_LIVE) return json(c, { error: "too-many-live" }, 400);
    const id = mintBracketId();
    const b = Bracket.create(id, name, by, body.size);
    brackets.set(id, b);
    await deps.saveBrackets(brackets);
    return json(c, { ok: true, id, size: b.size });
  });

  app.post("/tournament/join", async (c) => {
    const body = await readBody(c, 4096);
    const brackets = await deps.loadBrackets();
    const b = brackets.get(String(body?.id || ""));
    const lb = await deps.loadLb();
    const name = sanitizeName(body?.name);
    const key = typeof body?.key === "string" ? body.key : "";
    /* Banned/muted players can't post results — a bracket pairing with
     * them inside could never verify, so they may not join at all. */
    const ent = lb.byName.get(name);
    if (!b || !name || !lb.keyOwns(name, key) ||
        (ent && (ent.banned || (ent.disputes || 0) >= LB_MUTE_DISPUTES))) {
      return json(c, { error: "bad-request" }, 400);
    }
    const r = b.join(name);
    if (r.kind !== "ok") return json(c, { error: r.reason }, 400);
    await deps.saveBrackets(brackets);
    return json(c, r);
  });

  app.post("/tournament/start", async (c) => {
    const body = await readBody(c, 4096);
    const brackets = await deps.loadBrackets();
    const b = brackets.get(String(body?.id || ""));
    const lb = await deps.loadLb();
    const key = typeof body?.key === "string" ? body.key : "";
    if (!b || !lb.keyOwns(b.owner, key)) {
      return json(c, { error: "bad-request" }, 400);
    }
    // Seed by board rating — the strongest player takes seed 1.
    const r = b.start(lb.list().map((e) => e.name));
    if (r.kind !== "ok") return json(c, { error: r.reason }, 400);
    await deps.saveBrackets(brackets);
    return json(c, b.view());
  });

  app.get("/tournament", async (c) => {
    const brackets = await deps.loadBrackets();
    const id = c.req.query("id");
    if (id) {
      const b = brackets.get(id);
      if (!b) return json(c, { error: "no-such-bracket" }, 404);
      return json(c, b.view());
    }
    return json(c, { brackets: [...brackets.values()].map((b) => b.view()) });
  });

  app.notFound((c) => c.text("not found", 404));

  return app;
}
