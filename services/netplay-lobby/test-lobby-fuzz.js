// Lobby protocol fuzz driver (testplan §11): hammers node-lobby with
// malformed text/binary frames — the server must never crash, hang, or
// corrupt unrelated rooms. Run: node services/netplay-lobby/test-lobby-fuzz.js [iterations]
const path = require("path");
const http = require("http");
const crypto = require("crypto");
const { spawn } = require("child_process");
const WebSocket = require(path.join(__dirname, "cloudflare", "node_modules", "ws"));

const PORT = 8878;
const BASE = `ws://127.0.0.1:${PORT}/ws`;
const ITERS = parseInt(process.argv[2] || "400", 10);
const SEED = parseInt(process.env.LOBBY_FUZZ_SEED || "777", 10);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Deterministic PRNG so a crash seed reproduces.
let rng = SEED >>> 0;
const next = () => (rng = (rng * 1664525 + 1013904223) >>> 0);
const pick = (arr) => arr[next() % arr.length];
const rbytes = (n) => Buffer.from(Array.from({ length: n }, () => next() & 0xff));

const TEXT_VECTORS = [
  () => rbytes(next() % 256).toString("latin1"),           // not JSON
  () => "{", () => "{}", () => "null", () => "[]",
  () => JSON.stringify({ type: pick(["endpoint", "relay", "ping", "keepalive", "bogus", next() & 0xffff]) }),
  () => JSON.stringify({ type: "endpoint", public: rbytes(next() % 300).toString("hex"), local: next() }),
  () => JSON.stringify({ type: "endpoint", public: 12345, local: { x: 1 } }),
  () => rbytes(3000).toString("hex"),                      // over MAX_TEXT_BYTES
];

async function main() {
  const srv = spawn("node", [path.join(__dirname, "node-lobby.js"), String(PORT)], { stdio: "pipe" });
  let srvDead = false;
  srv.on("exit", (code) => { srvDead = true; console.log(`lobby exited code=${code}`); });
  await sleep(700);

  // Control room: must stay healthy through the whole fuzz.
  const ctl = new WebSocket(`${BASE}?room=CTRL42&role=host&v=2`);
  await new Promise((r) => ctl.on("open", r));

  const sockets = [];
  for (let i = 0; i < ITERS && !srvDead; i++) {
    const which = next() % 5;
    try {
      if (which <= 1) {
        // Random connection params: bad room codes, weird roles, uplink probes.
        // NB: CTRL42 is off-limits — a fuzzed host join legitimately resets
        // the room, which would kill the control socket by design.
        const q = `room=${pick(["", "AAAAAA", "!!!!!!", "A".repeat(next() % 20), "ZZZZZ9"])}` +
                  `&role=${pick(["host", "join", "x", String(next())])}` +
                  `&v=${pick(["1", "2", "x"])}` +
                  (next() % 4 === 0 ? `&up=1&slot=${next() % 5}&key=${rbytes(8).toString("hex")}` : "");
        const ws = new WebSocket(`${BASE}?${q}`);
        ws.on("error", () => {});
        sockets.push(ws);
      } else if (sockets.length > 0) {
        // Fuzz a live socket.
        const ws = pick(sockets.filter((s) => s.readyState === 1));
        if (ws) {
          if (next() % 2) ws.send(pick(TEXT_VECTORS)());
          else ws.send(rbytes(1 + (next() % 1200)));
        }
      }
      if (sockets.length > 40) {
        const ws = sockets.shift();
        try { ws.terminate(); } catch { /* gone */ }
      }
    } catch { /* keep going — a client-side throw is not a server fault */ }
    if (i % 50 === 49) await sleep(20);   // let the event loop breathe
  }

  for (const ws of sockets) { try { ws.terminate(); } catch { /* gone */ } }
  await sleep(300);

  // Oracle 1: server still alive.
  if (srvDead) {
    console.log(`FAIL lobby process died during fuzz (seed=${SEED})`);
    srv.kill();
    process.exit(1);
  }
  // Oracle 2: control room still functional (send a valid message, get ack).
  const ack = new Promise((r) => {
    ctl.on("message", (d) => { try { if (JSON.parse(d.toString()).type === "keepalive_ack") r(true); } catch {} });
    setTimeout(() => r(false), 2000);
  });
  ctl.send(JSON.stringify({ type: "ping" }));
  const alive = await ack;
  // Oracle 3: /health answers.
  const health = await new Promise((r) => {
    http.get(`http://127.0.0.1:${PORT}/health`, (res) => r(res.statusCode === 200)).on("error", () => r(false));
  });
  ctl.terminate();
  srv.kill();

  const pass = alive && health;
  console.log(`${pass ? "PASS" : "FAIL"} lobby fuzz iters=${ITERS} seed=${SEED} ctl_alive=${alive} health=${health}`);
  process.exit(pass ? 0 : 1);
}

main().catch((e) => { console.log("FAIL harness error:", e.message); process.exit(2); });
