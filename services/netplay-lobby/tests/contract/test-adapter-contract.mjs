// test-adapter-contract.mjs — differential contract between the two
// adapters' dependency shapes. Fires an identical request battery at
// createLobbyApp wired with:
//
//   * node-shaped deps  — fs-backed stores + the SAME live domain
//                         instances every loadLb (mirrors
//                         infrastructure/local/node-lobby.mjs)
//   * DO-shaped deps    — Map-backed storage + a FRESH fromJSON object
//                         per loadLb (mirrors worker.js doDeps /
//                         ctx.storage semantics)
//
// The shared app must produce identical HTTP semantics under both
// persistence models — status codes, normalized bodies, and final
// observable state. The asymmetry it guards: node's shared instance
// persists un-saved mutations (the object IS the store), while the DO
// only persists what saveLb writes. Any route that mutates without
// saveLb then fails would drift silently.
//
// Volatile output (minted ids, ISO timestamps, per-adapter extras) is
// normalized before comparison. Run: node tests/contract/test-adapter-contract.mjs

import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { createLobbyApp } from "../../src/interfaces/http/app.mjs";
import { Leaderboard } from "../../src/domain/leaderboard.mjs";
import { Bracket, BR_MAX_LIVE } from "../../src/domain/bracket.mjs";

const results = [];
const ok = (name, cond) => {
  results.push([name, cond]);
  console.log(`${cond ? "PASS" : "FAIL"} ${name}`);
};

/* ── dependency shapes ─────────────────────────────────────────────── */

/* Mirrors worker.js doDeps(): ctx.storage becomes a Map; every load
 * re-inflates a fresh domain object — nothing persists unless saved. */
function doDeps() {
  const store = new Map();
  const storage = {
    get: async (k) => store.get(k),
    put: async (k, v) => void store.set(k, v),
    delete: async (k) => store.delete(k),
  };
  const CRASH_RING = 64;
  return {
    loadLb: async () =>
      Leaderboard.fromJSON((await storage.get("lb")) || []),
    saveLb: (lb) => storage.put("lb", lb.toJSON()),
    loadBrackets: async () =>
      new Map(Object.entries((await storage.get("brackets")) || {})
        .map(([id, o]) => [id, Bracket.fromJSON(o)])),
    saveBrackets: (map) =>
      storage.put("brackets",
        Object.fromEntries([...map].map(([k, b]) => [k, b.toJSON()]))),
    replayIndex: {
      get: async () => (await storage.get("replays-idx")) || [],
      save: (idx) => storage.put("replays-idx", idx),
    },
    replayBlobs: {
      get: (id) => storage.get(`rp:${id}`),
      put: (id, rec) => storage.put(`rp:${id}`, rec),
      delete: (id) => storage.delete(`rp:${id}`),
    },
    crashes: {
      append: async (rec) => {
        const ring = (await storage.get("crashes")) || [];
        ring.push(rec);
        while (ring.length > CRASH_RING) ring.shift();
        await storage.put("crashes", ring);
      },
      list: async () => (await storage.get("crashes")) || [],
      count: async () => ((await storage.get("crashes")) || []).length,
    },
    statsExtra: () => ({}),
    healthExtra: () => ({}),
    _dump: async () => ({
      lb: (await storage.get("lb")) || [],
      brackets: (await storage.get("brackets")) || {},
      replays: (await storage.get("replays-idx")) || [],
      crashes: (await storage.get("crashes")) || [],
    }),
  };
}

/* Mirrors node-lobby.mjs deps: live lb/brackets/replayIdx objects kept
 * per process (loadLb returns THE instance — unsaved mutations linger),
 * replay blobs + crash ring live under a temp data dir. */
function nodeDeps() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "lobby-contract-"));
  const lb = new Leaderboard();
  const brackets = new Map();
  const replayIdx = [];
  const replayDir = path.join(dir, "replays");
  const crashPath = path.join(dir, "crashes.jsonl");
  const lbPath = path.join(dir, "leaderboard.json");
  const brPath = path.join(dir, "brackets.json");
  let crashCount = 0;
  const CRASH_RING = 64;

  const writeJsonAtomic = (dest, obj) => {
    const tmp = dest + ".tmp";
    fs.writeFileSync(tmp, JSON.stringify(obj));
    fs.renameSync(tmp, dest);
  };

  return {
    loadLb: async () => lb,                    // shared instance!
    saveLb: async () => {
      try { writeJsonAtomic(lbPath, lb.toJSON()); } catch { /* ro fs */ }
    },
    loadBrackets: async () => brackets,        // shared Map!
    saveBrackets: async (map) => {
      try {
        writeJsonAtomic(brPath,
          Object.fromEntries([...map].map(([k, b]) => [k, b.toJSON()])));
      } catch { /* ro fs */ }
    },
    replayIndex: {
      get: async () => replayIdx,
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
        try { fs.appendFileSync(crashPath, JSON.stringify(rec) + "\n"); }
        catch { /* ro fs */ }
        crashCount++;
      },
      list: async () => {
        const ring = [];
        try {
          for (const line of fs.readFileSync(crashPath, "utf8").split("\n")) {
            if (!line.trim()) continue;
            try { ring.push(JSON.parse(line)); } catch { /* torn tail */ }
          }
        } catch { /* no reports yet */ }
        return ring.slice(-CRASH_RING);
      },
      count: async () => crashCount,
    },
    statsExtra: () => ({}),
    healthExtra: () => ({}),
    _dump: async () => ({
      lb: lb.toJSON(),
      brackets: Object.fromEntries([...brackets].map(([k, b]) => [k, b.toJSON()])),
      replays: replayIdx,
      crashes: JSON.parse(
        fs.existsSync(crashPath)
          ? `[${fs.readFileSync(crashPath, "utf8").trim().split("\n").join(",")}]`
          : "[]"),
    }),
  };
}

/* ── normalization ─────────────────────────────────────────────────── */

/* Volatile values -> placeholders so the two adapters' outputs compare:
 * minted ids (uuid / br- / rp- style), ISO timestamps, `at` fields. */
function normalize(v) {
  if (typeof v === "string") {
    return v
      .replace(/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}/gi, "<UUID>")
      .replace(/\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+Z/g, "<TS>");
  }
  /* epoch-millis timestamps (moderation report times, replay clocks) */
  if (typeof v === "number" && v > 1e12) return "<EPOCH>";
  if (Array.isArray(v)) return v.map(normalize);
  if (v !== null && typeof v === "object") {
    const out = {};
    for (const k of Object.keys(v).sort()) {
      /* Minted ids appear as map keys (brackets, replays) — normalize
       * the key itself so storage shape compares, not random values. */
      const nk = (/^[0-9a-f]{8,}$/i.test(k) ||
                  /^[0-9a-f-]{20,}$/i.test(k)) ? "<KEY>" : k;
      out[nk] = (k === "at" || k === "id") ? "<VOL>" : normalize(v[k]);
    }
    return out;
  }
  return v;
}
const normStr = (v) => JSON.stringify(normalize(v));

/* ── request battery ───────────────────────────────────────────────── */

const K_A = "0123456789abcdef0123456789abcdef";
const K_B = "ffffffffffffffffffffffffffffffff";
const K_C = "cccccccccccccccccccccccccccccccc";
const K_D = "dddddddddddddddddddddddddddddddd";

const J = (o) => JSON.stringify(o);
const H = { "content-type": "application/json" };

async function battery(app, label) {
  const out = [];
  const req = async (method, p, body, raw) => {
    const r = await app.fetch(new Request(`http://t${p}`, {
      method,
      headers: body !== undefined ? H : undefined,
      body: raw !== undefined ? raw : (body !== undefined ? J(body) : undefined),
    }));
    let parsed = null;
    const text = await r.text();
    try { parsed = JSON.parse(text); } catch { parsed = text.length > 120 ? text.slice(0, 120) : text; }
    out.push({ m: method, p, status: r.status, body: parsed });
    return { status: r.status, body: parsed };
  };

  /* root + health + stats (extras normalized to {} in both dep sets) */
  await req("GET", "/health");
  await req("GET", "/");
  await req("GET", "/stats");
  await req("GET", "/nonexistent-route");

  /* leaderboard claim flow — valid, wrong key, absurd rating, hostile name */
  await req("POST", "/submit", { name: "AliceT", key: K_A, rating: 1500, wins: 0, losses: 0 });
  await req("POST", "/submit", { name: "BobT", key: K_B, rating: 1500, wins: 0, losses: 0 });
  await req("POST", "/submit", { name: "CarolT", key: K_C, rating: 1400, wins: 2, losses: 1 });
  await req("POST", "/submit", { name: "AliceT", key: K_B, rating: 9999 });
  await req("POST", "/submit", { name: "AliceT", key: K_A, rating: 99999 });
  await req("POST", "/submit", { name: '"><script>', key: K_A, rating: 1300 });
  await req("POST", "/submit", null, "{ not json");
  await req("GET", "/leaderboard");
  await req("GET", "/player?name=AliceT");
  await req("GET", "/player?name=NoSuchPlayer");
  await req("GET", "/history");
  await req("GET", "/history?name=AliceT");
  await req("GET", "/history?name=");

  /* corroborated results — pending, verified, settled-reject, mismatch */
  await req("POST", "/result", { name: "AliceT", opp_name: "BobT", key: K_A, match_id: "aa11bb22cc33dd44", won: 1, duration: 6000 });
  await req("POST", "/result", { name: "BobT", opp_name: "AliceT", key: K_B, match_id: "aa11bb22cc33dd44", won: 0, duration: 6000 });
  await req("POST", "/result", { name: "AliceT", opp_name: "BobT", key: K_A, match_id: "aa11bb22cc33dd44", won: 1, duration: 6000 });
  await req("POST", "/result", { name: "AliceT", opp_name: "BobT", key: K_A, match_id: "ee55ff66aa77bb88", won: 1, duration: 100 });
  await req("POST", "/result", { name: "BobT", opp_name: "AliceT", key: K_B, match_id: "ee55ff66aa77bb88", won: 1, duration: 100 });
  await req("POST", "/result", { name: "AliceT", opp_name: "BobT", key: K_A, match_id: "nope!", won: 1, duration: 10 });
  await req("POST", "/result", { name: "AliceT", opp_name: "BobT", key: "x", match_id: "1122334455667788", won: 1, duration: 10 });
  await req("POST", "/result", { name: "AliceT", opp_name: "BobT", key: K_A, match_id: "1122334455667788", won: 7, duration: 10 });
  await req("GET", "/history?name=AliceT");

  /* moderation report */
  await req("POST", "/report", { name: "CarolT", key: K_A });
  await req("POST", "/report", { name: "NoSuchPlayer", key: K_A });
  await req("POST", "/report", null, "garbage");
  await req("GET", "/leaderboard");

  /* crash reports — valid, invalid kind, oversized reject */
  await req("POST", "/crash", { kind: "crash", info: "segv", log_tail: "tail", lus_tail: "lus" });
  await req("POST", "/crash", { kind: "notacrash", info: "x", log_tail: "y" });
  await req("POST", "/crash", { kind: "crash", info: "x".repeat(300 * 1024), log_tail: "" });
  await req("GET", "/crashes");

  /* replays — valid post, index newest-first, fetch by id, bad id, 404 */
  await req("POST", "/replay", { data: "QUJDQUJD", by: "AliceT" });
  await req("POST", "/replay", { data: "REVGREVG", by: "BobT" });
  await req("POST", "/replay", { data: "", by: "x" });
  await req("POST", "/replay", { data: "!!!not-b64!!!", by: "x" });
  const reps = await req("GET", "/replays");
  const rid = reps.body?.replays?.[0]?.id;
  if (rid) {
    await req("GET", `/replay/${encodeURIComponent(rid)}`);
  }
  await req("GET", "/replay/not a valid id!");
  await req("GET", "/replay/aaaaaaaaaaaaaaaa");

  /* tournaments — create needs a claimed name+key, join, start, view */
  await req("POST", "/tournament", { name: "Cup One", by: "AliceT", key: K_A, size: 4 });
  await req("POST", "/tournament", { name: "Cup Two", by: "AliceT", key: K_B, size: 4 });
  await req("POST", "/tournament", { name: "Cup Three", by: "Ghost", key: K_A, size: 4 });
  const tour = await req("GET", "/tournament");
  const bid = tour.body?.brackets?.[0]?.id;
  if (bid) {
    await req("POST", "/tournament/join", { id: bid, name: "BobT", key: K_B });
    await req("POST", "/tournament/join", { id: bid, name: "CarolT", key: K_C });
    await req("POST", "/tournament/join", { id: bid, name: "CarolT", key: K_C });
    await req("POST", "/tournament/join", { id: bid, name: "BobT", key: K_A });
    await req("POST", "/tournament/join", { id: "br-nope", name: "BobT", key: K_B });
    await req("GET", `/tournament?id=${encodeURIComponent(bid)}`);
    await req("POST", "/tournament/start", { id: bid, key: K_A });
    await req("POST", "/tournament/start", { id: bid, key: K_A });
    await req("GET", `/tournament?id=${encodeURIComponent(bid)}`);
  }
  await req("GET", "/tournament?id=br-missing");
  await req("GET", "/tournament");

  /* stats + health re-read after the whole mutation sequence */
  await req("GET", "/stats");
  await req("GET", "/health");

  return out;
}

/* ── compare ───────────────────────────────────────────────────────── */

const doD = doDeps();
const nodeD = nodeDeps();
const doApp = createLobbyApp(doD);
const nodeApp = createLobbyApp(nodeD);

const [doOut, nodeOut] = [
  await battery(doApp, "do"),
  await battery(nodeApp, "node"),
];

ok("same response count", doOut.length === nodeOut.length);

let diffs = 0;
for (let i = 0; i < Math.min(doOut.length, nodeOut.length); i++) {
  const a = doOut[i], b = nodeOut[i];
  if (a.status !== b.status || normStr(a.body) !== normStr(b.body)) {
    diffs++;
    console.log(`  DIFF #${i} ${a.m} ${a.p}: status ${a.status}/${b.status}`);
    console.log(`    do:   ${normStr(a.body)}`);
    console.log(`    node: ${normStr(b.body)}`);
  }
}
ok(`0/${doOut.length} response diffs`, diffs === 0);

/* Final observable state must be identical too — not just responses. */
const doState = normalize(await doD._dump());
const nodeState = normalize(await nodeD._dump());
ok("identical persisted state",
   JSON.stringify(doState) === JSON.stringify(nodeState));
if (JSON.stringify(doState) !== JSON.stringify(nodeState)) {
  const a = JSON.stringify(doState), b = JSON.stringify(nodeState);
  let i = 0;
  while (i < Math.min(a.length, b.length) && a[i] === b[i]) i++;
  console.log(`  first diff at char ${i}:`);
  console.log(`    do:   ...${a.slice(Math.max(0, i - 60), i + 120)}`);
  console.log(`    node: ...${b.slice(Math.max(0, i - 60), i + 120)}`);
}

const fails = results.filter(([, c]) => !c).length;
console.log(`${results.length - fails}/${results.length} checks passed`);
process.exit(fails ? 1 : 0);
