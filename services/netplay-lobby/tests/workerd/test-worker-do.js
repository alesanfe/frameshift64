// Real-runtime contract test: runs the actual Cloudflare worker inside
// workerd via `wrangler dev --local`, so requests flow through a real
// Durable Object with transactional storage — not a Map simulation like
// tests/contract/test-adapter-contract.mjs.
//
//   node tests/workerd/test-worker-do.js [port]
//
// Covers the DO-routed surface end to end: health, name claims, the
// server-issued match-token flow (begin → claim → token-bearing result
// mirror), replay persistence in ctx.storage, and a WebSocket room
// admission through the per-room DO.
const path = require("path");
const { spawn } = require("child_process");
const WebSocket = require("ws");

const PORT = parseInt(process.argv[2] || "8890", 10);
const BASE = `http://127.0.0.1:${PORT}`;
const SERVICE_DIR = path.join(__dirname, "..", "..");

const results = [];
const ok = (name, cond) => { results.push([name, cond]); console.log(`${cond ? "PASS" : "FAIL"} ${name}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const json = (r) => r.json();
const post = (p, body) => fetch(`${BASE}${p}`, {
  method: "POST",
  headers: { "content-type": "application/json" },
  body: JSON.stringify(body),
});

function wsConnect(qs) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws?${qs}`);
    const msgs = [];
    ws.on("message", (d) => msgs.push(d));
    ws.on("open", () => resolve({ ws, msgs }));
    ws.on("error", reject);
  });
}
async function firstMsg(c, ms = 5000) {
  for (let t = 0; t < ms; t += 25) {
    if (c.msgs.length) return JSON.parse(c.msgs[0].toString());
    await sleep(25);
  }
  return null;
}

async function main() {
  /* wrangler dev runs the real wrangler.jsonc: bundles worker.js (hono
   * import resolved), applies the NetplayRoom DO migration, and serves it
   * on local workerd — the deployed runtime, not a shim. */
  const wranglerCli = path.join(SERVICE_DIR, "node_modules", "wrangler",
    "wrangler-dist", "cli.js");
  const srv = spawn(process.execPath, [wranglerCli, "dev", "--local",
    "--port", String(PORT), "--show-interactive-dev-session=false",
    "--log-level", "warn"],
    { cwd: SERVICE_DIR, stdio: "pipe" });
  srv.stderr.on("data", () => {});

  try {
    /* Wait for a live bind: /health is served by the worker itself, so a
     * 200 means bundling + the worker entrypoint both survived. */
    let up = false;
    for (let t = 0; t < 600 && !up; t++) {
      try {
        const r = await fetch(`${BASE}/health`);
        if (r.ok) up = true;
      } catch { /* still booting */ }
      if (!up) await sleep(100);
    }
    if (!up) throw new Error("wrangler dev never bound");

    const health = await json(await fetch(`${BASE}/health`));
    ok("health through real workerd", health.ok === true && health.service === "smash-netplay");
    const notFound = await fetch(`${BASE}/nope`);
    ok("unknown path is 404", notFound.status === 404);

    // --- DO-routed leaderboard flow ---
    const K1 = "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6";
    const K2 = "f6e5d4c3b2a19087f6e5d4c3b2a19087";
    const s1 = await post("/submit", { name: "WkA", key: K1, rating: 1500, wins: 0, losses: 0 });
    const s2 = await post("/submit", { name: "WkB", key: K2, rating: 1500, wins: 0, losses: 0 });
    ok("claims accepted through DO", s1.status === 200 && s2.status === 200);
    const dup = await post("/submit", { name: "WkA", key: K2, rating: 1500, wins: 0, losses: 0 });
    ok("name-taken rejected through DO", dup.status === 400);

    // --- server-issued match token flow (the /match/begin path) ---
    const begin = await json(await post("/match/begin",
      { name: "WkA", key: K1, opp_name: "WkB" }));
    ok("match begin mints id + caller token",
       begin.kind === "ok" && typeof begin.id === "string" &&
       /^[0-9a-f]{32}$/.test(begin.token));
    const claim = await json(await post("/match/claim",
      { id: begin.id, name: "WkB", key: K2 }));
    ok("opponent claims own token", claim.kind === "ok" && claim.token !== begin.token);

    const noTok = await post("/result",
      { name: "WkA", opp_name: "WkB", key: K1, match_id: begin.id, won: 1 });
    ok("missing token rejected", noTok.status === 400);

    const r1 = await json(await post("/result",
      { name: "WkA", opp_name: "WkB", key: K1, match_id: begin.id, won: 1, token: begin.token }));
    ok("first token report pending", r1.kind === "pending");
    const r2 = await json(await post("/result",
      { name: "WkB", opp_name: "WkA", key: K2, match_id: begin.id, won: 0, token: claim.token }));
    ok("token mirror verifies trusted=token",
       r2.kind === "verified" && r2.trusted === "token");

    const replayId = await post("/result",
      { name: "WkA", opp_name: "WkB", key: K1, match_id: begin.id, won: 1, token: begin.token });
    ok("settled issued match rejected", replayId.status === 400);

    const lb = await json(await fetch(`${BASE}/leaderboard`));
    const rowA = (lb.entries || []).find((e) => e.name === "WkA");
    ok("DO-persisted board shows Elo moved", rowA && rowA.wins === 1 && rowA.rating > 1500);

    // --- replay storage through ctx.storage ---
    const blob = Buffer.from([0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4]).toString("base64");
    const rp = await json(await post("/replay", { data: blob, by: "WkA" }));
    ok("replay upload accepted", rp.ok === true && typeof rp.id === "string");
    const dl = await fetch(`${BASE}/replay/${rp.id}`);
    ok("replay downloads from DO storage", dl.status === 200);

    // --- per-room DO: WebSocket admission ---
    const c = await wsConnect("room=TESTAB&role=host&v=2");
    const welcome = await firstMsg(c);
    ok("host welcome via per-room DO",
       welcome && welcome.type === "welcome" && welcome.slot === 0 &&
       /^[0-9a-f]{32}$/.test(welcome.upkey || ""));
    c.ws.close();

    // --- join checks that never reach the DO ---
    const badWs = await wsConnect("room=!!&role=join&v=2").catch(() => null);
    ok("bad room code rejected", badWs === null ||
       ((await firstMsg(badWs)) || {}).reason === "bad room or role");
  } finally {
    srv.kill();
  }

  const failed = results.filter(([, x]) => !x);
  console.log(`\n${results.length - failed.length}/${results.length} workerd checks passed`);
  process.exit(failed.length ? 1 : 0);
}

main().catch((e) => { console.error("workerd driver error:", e); process.exit(1); });
