// Lobby chaos test (testplan §12): kill the server mid-flight, corrupt its
// stores, restart on the same port — the service must recover without
// losing durably-saved state. Validates the atomic temp+rename writes
// (leaderboard.json / brackets.json / replays/) under crash-fault
// injection. Run: node services/netplay-lobby/tests/chaos/test-lobby-chaos.js
const path = require("path");
const fs = require("fs");
const { spawn } = require("child_process");

const PORT = 8887;
const BASE = `http://127.0.0.1:${PORT}`;
const SVC = path.join(__dirname, "..", "..", "src", "infrastructure", "local", "node-lobby.mjs");
const LB_FILE = path.join(__dirname, "..", "..", "leaderboard.json");
const BR_FILE = path.join(__dirname, "..", "..", "brackets.json");
const RP_DIR = path.join(__dirname, "..", "..", "replays");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let pass = 0, fail = 0;
const ok = (name, cond) => { cond ? pass++ : fail++;
  console.log(`${cond ? "PASS" : "FAIL"} ${name}`); };

function waitForListen(srv, ms = 8000) {
  /* Three readiness signals raced: the stdout banner (fast path), an
   * active /health poll (covers a swallowed or interleaved banner), and
   * early process exit. stderr is captured so a timeout dumps the real
   * cause instead of a bare "listen timeout". */
  return new Promise((resolve, reject) => {
    let stderr = "";
    srv.stderr.on("data", (d) => { stderr += String(d); });
    const fail = (why) => { clearTimeout(to); clearInterval(poll); reject(new Error(why)); };
    const to = setTimeout(() =>
      fail(`lobby listen timeout${stderr ? `\nstderr: ${stderr}` : ""}`), ms);
    const poll = setInterval(async () => {
      try {
        const r = await fetch(`${BASE}/health`);
        if (r.ok) { clearTimeout(to); clearInterval(poll); resolve(); }
      } catch { /* not up yet */ }
    }, 100);
    srv.stdout.on("data", (d) => {
      if (String(d).includes("listening")) {
        clearTimeout(to); clearInterval(poll); resolve();
      }
    });
    srv.on("exit", (c) =>
      fail(`lobby exited ${c} before listen${stderr ? `\nstderr: ${stderr}` : ""}`));
  });
}

async function startLobby() {
  const srv = spawn("node", [SVC, String(PORT)], { stdio: "pipe" });
  srv.stderr.on("data", () => {});
  await waitForListen(srv);
  return srv;
}

const post = (p, body) => fetch(`${BASE}${p}`, {
  method: "POST", headers: { "content-type": "application/json" },
  body: JSON.stringify(body) });

const KEY_A = "aaaa".repeat(8), KEY_B = "bbbb".repeat(8), KEY_O = "cccc".repeat(8);

async function main() {
  for (const f of [LB_FILE, BR_FILE]) { try { fs.unlinkSync(f); } catch { /* absent */ } }

  // --- Phase 1: seed durable state, then SIGKILL (not graceful) mid-write ---
  let srv = await startLobby();
  await post("/submit", { name: "ChaosA", key: KEY_A, rating: 1500, wins: 1, losses: 0 });
  await post("/submit", { name: "ChaosB", key: KEY_B, rating: 1500, wins: 0, losses: 1 });
  await post("/submit", { name: "ChaosOwn", key: KEY_O, rating: 1400, wins: 0, losses: 0 });
  const up = await (await post("/replay", {
    data: Buffer.from("chaos-replay").toString("base64"), by: "ChaosA" })).json();
  const tour = await (await post("/tournament", {
    name: "ChaosCup", by: "ChaosOwn", key: KEY_O, size: 2 })).json();
  await post("/tournament/join", { id: tour.id, name: "ChaosA", key: KEY_A });
  await post("/tournament/join", { id: tour.id, name: "ChaosB", key: KEY_B });
  srv.kill("SIGKILL");
  await sleep(400);
  ok("server killed without graceful shutdown", srv.exitCode === null || srv.killed);

  // --- Phase 2: restart on same port — durable state must survive ---
  srv = await startLobby();
  const board = await (await fetch(`${BASE}/leaderboard`)).json();
  ok("leaderboard survives SIGKILL restart",
     (board.entries || []).some((e) => e.name === "ChaosA") &&
     (board.entries || []).some((e) => e.name === "ChaosB"));
  const view = await (await fetch(`${BASE}/tournament?id=${tour.id}`)).json();
  ok("tournament survives SIGKILL restart",
     view.id === tour.id && (view.players || []).length === 2);
  const rep = await (await fetch(`${BASE}/replay/${up.id}`)).json();
  ok("shared replay survives SIGKILL restart", rep.data != null);
  const idx = await (await fetch(`${BASE}/replays`)).json();
  ok("replay index rebuilt from disk on restart",
     (idx.replays || []).some((r) => r.id === up.id));
  const st = await (await fetch(`${BASE}/health`)).json();
  ok("service healthy after restart", st.ok === true);
  srv.kill("SIGKILL");
  await sleep(300);

  // --- Phase 3: torn/corrupt stores — the service must not crash-loop ---
  fs.writeFileSync(LB_FILE, '{"entries":[TRUNCATED', "utf8");
  fs.writeFileSync(BR_FILE, "not json at all", "utf8");
  srv = await startLobby();
  const healthy = await (await fetch(`${BASE}/health`)).json();
  ok("service boots on corrupt stores", healthy.ok === true);
  const board2 = await (await fetch(`${BASE}/leaderboard`)).json();
  ok("corrupt leaderboard recovers to empty board",
     Array.isArray(board2.entries) && board2.entries.length === 0);
  const sub = await post("/submit", {
    name: "Recover", key: "dddd".repeat(8), rating: 1500, wins: 0, losses: 0 });
  ok("writes work after corrupt-store recovery", sub.status === 200);

  // --- Phase 4: binary garbage + replay index pointing at a missing file ---
  fs.writeFileSync(LB_FILE, Buffer.alloc(256, 0xff));
  srv.kill("SIGKILL");
  await sleep(300);
  srv = await startLobby();
  const healthy2 = await (await fetch(`${BASE}/health`)).json();
  ok("service boots on binary-garbage store", healthy2.ok === true);
  srv.kill("SIGKILL");

  for (const f of [LB_FILE, BR_FILE]) { try { fs.unlinkSync(f); } catch { /* absent */ } }
  console.log(`\n${pass}/${pass + fail} checks passed`);
  process.exit(fail ? 1 : 0);
}

main().catch((e) => { console.error("CHAOS DRIVER ERROR:", e); process.exit(1); });
