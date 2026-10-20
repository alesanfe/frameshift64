// Exercise the hardened lobby: host + 2 clients, uplink auth, star
// enforcement and rate limiting. Run: node server/test-lobby-security.js
const path = require("path");
const fs = require("fs");
const { spawn } = require("child_process");
const WebSocket = require("ws");

const PORT = 8877;
const BASE = `ws://127.0.0.1:${PORT}/ws`;
const results = [];
const ok = (name, cond) => { results.push([name, cond]); console.log(`${cond ? "PASS" : "FAIL"} ${name}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function connect(qs) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`${BASE}?${qs}`);
    const msgs = [];
    const bins = [];
    ws.on("message", (d, isBin) => (isBin ? bins : msgs).push(d));
    ws.on("open", () => resolve({ ws, msgs, bins }));
    ws.on("error", reject);
  });
}
const jmsg = (c, type) => c.msgs.map((m) => JSON.parse(m.toString())).find((m) => m.type === type);
async function jmsgWait(c, type, ms = 2000) {
  for (let t = 0; t < ms; t += 25) {
    const m = jmsg(c, type);
    if (m) return m;
    await sleep(25);
  }
  return null;
}
const closed = (ws) => Promise.race([
  new Promise((r) => ws.on("close", r)),
  sleep(3000).then(() => -1),
]);

async function main() {
  // node-lobby persists the leaderboard under its data dir — wipe it
  // first so rating assertions aren't order-dependent across runs.
  // LOBBY_SCRIPT/LOBBY_DATA_DIR let mutation harnesses point this suite
  // at a mutated source tree.
  try {
    fs.unlinkSync(path.join(process.env.LOBBY_DATA_DIR ||
      path.join(__dirname, "..", ".."), "leaderboard.json"));
  } catch { /* first run or already clean */ }
  const lobbyScript = process.env.LOBBY_SCRIPT ||
    path.join(__dirname, "..", "..", "src", "infrastructure", "local", "node-lobby.mjs");
  const srv = spawn("node", [lobbyScript, String(PORT)], { stdio: "pipe" });
  /* Wait for a real bind — race the stdout banner against an active
   * /health poll (a swallowed banner must not stall the suite) and
   * early exit; stderr rides along so a timeout reports the cause. */
  await new Promise((resolve, reject) => {
    let stderr = "";
    srv.stderr.on("data", (d) => { stderr += String(d); });
    const fail = (why) => { clearTimeout(to); clearInterval(poll); reject(new Error(why)); };
    const to = setTimeout(() =>
      fail(`lobby listen timeout${stderr ? `\nstderr: ${stderr}` : ""}`), 8000);
    const poll = setInterval(async () => {
      try {
        const r = await fetch(`http://127.0.0.1:${PORT}/health`);
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

  // --- setup: host + 2 clients on a v2 room ---
  const host = await connect("room=ABCDEF&role=host&v=2");
  const hw = await jmsgWait(host, "welcome");
  ok("host welcome carries slot 0 + upkey", hw && hw.slot === 0 && /^[0-9a-f]{32}$/.test(hw.upkey || ""));

  const c1 = await connect("room=ABCDEF&role=join&v=2");
  const c2 = await connect("room=ABCDEF&role=join&v=2");
  const w1 = await jmsgWait(c1, "welcome");
  const w2 = await jmsgWait(c2, "welcome");
  ok("clients get slots 1,2 + distinct upkeys",
     w1.slot === 1 && w2.slot === 2 && w1.upkey !== w2.upkey && /^[0-9a-f]{32}$/.test(w1.upkey));

  // --- SEC-3: uplink requires the slot's upkey ---
  const bad = await connect("room=ABCDEF&role=join&v=2&up=1&slot=1&key=deadbeefdeadbeefdeadbeefdeadbeef");
  const badErr = await jmsgWait(bad, "error");
  ok("uplink with wrong key rejected", badErr && badErr.reason === "no-such-room");
  bad.ws.close();

  const noKey = await connect("room=ABCDEF&role=join&v=2&up=1&slot=1");
  ok("uplink with no key rejected", (await jmsgWait(noKey, "error"))?.reason === "no-such-room");
  noKey.ws.close();

  const up1 = await connect(`room=ABCDEF&role=join&v=2&up=1&slot=1&key=${w1.upkey}`);
  await sleep(100);
  ok("uplink with correct key accepted", up1.ws.readyState === WebSocket.OPEN);

  // uplink can't steal another slot (c2's key for slot 1 must fail)
  const steal = await connect(`room=ABCDEF&role=join&v=2&up=1&slot=1&key=${w2.upkey}`);
  ok("uplink with another slot's key rejected", (await jmsgWait(steal, "error"))?.reason === "no-such-room");
  steal.ws.close();

  // --- SEC-1/SEC-2 server side: star + relay-flag enforcement ---
  // nobody asked for relay yet: nothing should be routed at all
  const ping = Buffer.from([0, 9, 9, 9]);                 // [to=0][payload]
  const p2p = Buffer.from([2, 7, 7]);                     // client -> client attempt
  c1.ws.send(ping);
  c1.ws.send(p2p);
  await sleep(200);
  ok("no relay before 'relay' msg (client->host)", host.bins.length === 0);
  ok("client->client binary always dropped", c2.bins.length === 0);

  // c1 marks its link as relayed
  c1.ws.send(JSON.stringify({ type: "relay" }));
  await sleep(150);
  const pr = await jmsgWait(host, "peer_relay");
  ok("host told c1 is relayed", pr && pr.slot === 1);

  c1.ws.send(ping);          // now legit: c1 -> host
  up1.ws.send(ping);         // uplink carries owner's identity
  c1.ws.send(p2p);           // still illegal: client -> client
  await sleep(200);
  const fromC1 = host.bins.filter((b) => b[0] === 1).length;
  ok("relayed c1->host frames arrive with from_slot=1", fromC1 === 2);
  ok("c1->c2 still dropped after relay", c2.bins.length === 0);

  // host -> c1 works (target relayed), host -> c2 doesn't (c2 direct)
  host.ws.send(Buffer.from([1, 5, 5]));
  host.ws.send(Buffer.from([2, 6, 6]));
  await sleep(200);
  ok("host->relayed-client routed", c1.bins.length === 1 && c1.bins[0][0] === 0);
  ok("host->non-relayed client dropped", c2.bins.length === 0);

  // --- ready-check: {type:"ready"} rides the roster to players and specs ---
  const specReady = await connect("room=ABCDEF&role=spec&v=2");
  await jmsgWait(specReady, "welcome");
  host.msgs.length = 0;   // discard stale rosters; find() returns the oldest
  specReady.msgs.length = 0;
  c1.ws.send(JSON.stringify({ type: "ready", ready: 1 }));
  const roster = await jmsgWait(host, "peers");
  ok("ready flag reaches the host's roster",
     roster && roster.players.some((p) => p.slot === 1 && p.ready === true));
  const specRoster = await jmsgWait(specReady, "peers");
  const specSlot1 = specRoster && specRoster.players.find((p) => p.slot === 1);
  ok("spec's redacted roster still hides endpoints but shows ready",
     specSlot1 && specSlot1.ready === true && specSlot1.public === "-");
  c1.ws.send(JSON.stringify({ type: "ready", ready: 0 }));
  host.msgs.length = 0;
  const unready = await jmsgWait(host, "peers");
  ok("unready propagates",
     unready && unready.players.some((p) => p.slot === 1 && p.ready === false));

  // --- SEC-4: rate limits ---
  const joinWait = closed((await connect("room=ABCDEF&role=join&v=2")).ws);
  // room now has host+2 clients; one more join fits (slot 3)…
  // …but hammering joins trips the 12/10s window first:
  const attempts = [];
  for (let i = 0; i < 8; i++) attempts.push(await connect("room=ABCDEF&role=join&v=2"));
  const errs = [];
  for (const c of attempts) errs.push(await jmsgWait(c, "error"));
  const limited = errs.some((e) => e && (e.reason === "rate-limited" || e.reason === "room-full"));
  ok("join flood throttled (rate-limit or room-full)", limited);
  attempts.forEach((c) => c.ws.close());

  // text flood -> socket closed
  const flood = await connect("room=WXYZ22&role=host&v=2");
  await jmsgWait(flood, "welcome");
  const fclosed = closed(flood.ws);
  for (let i = 0; i < 40; i++) flood.ws.send(JSON.stringify({ type: "ping" }));
  const code = await fclosed;
  ok("text flood closed with 4029", code === 4029);

  // --- SEC-5: spectators (fresh room — ABCDEF spent its join window) ---
  const host2 = await connect("room=SPECAA&role=host&v=2");
  await jmsgWait(host2, "welcome");
  const spec = await connect("room=SPECAA&role=spec&v=2");
  const specW = await jmsgWait(spec, "welcome");
  ok("spec accepted on slot 4", specW && specW.slot === 4 && specW.role === "spec");

  // spec -> host must carry "SSBL " text; raw binary is dropped
  spec.ws.send(Buffer.concat([Buffer.from([0]), Buffer.from("SSBL HELLO")]));  // legit control
  spec.ws.send(Buffer.concat([Buffer.from([0]), Buffer.from([0xde, 0xad, 0xbe, 0xef, 0x01])])); // smuggled binary
  await sleep(200);
  ok("spec SSBL control reaches host",
     host2.bins.length === 1 && host2.bins[0][0] === 4 &&
     host2.bins[0].subarray(1).toString() === "SSBL HELLO");
  // spec -> player slot is always dropped (no player to observe, but the
  // route must refuse: connect a client and confirm nothing arrives)
  const cSpec = await connect("room=SPECAA&role=join&v=2");
  await jmsgWait(cSpec, "welcome");
  spec.ws.send(Buffer.concat([Buffer.from([cSpec ? 1 : 1]), Buffer.from("SSBL HI")]));
  await sleep(200);
  ok("spec->client route dropped", cSpec.bins.length === 0);

  // --- SEC-6: cross-version host must not reset a live room ---
  const killer = await connect("room=SPECAA&role=host");   // no v=2 -> v1
  const kerr = await jmsgWait(killer, "error");
  ok("cross-version host rejected", kerr && kerr.reason === "version-mismatch");
  await sleep(100);
  ok("live room survives (host still open)", host2.ws.readyState === WebSocket.OPEN);
  killer.ws.close();

  // --- SEC-7: oversized frames still spend rate budget ---
  const big = await connect("room=ZZZZ99&role=host&v=2");
  await jmsgWait(big, "welcome");
  const bclosed = closed(big.ws);
  const blob = Buffer.alloc(1025, 0x41);   // > MAX_BINARY_BYTES (1024)
  for (let i = 0; i < 620; i++) big.ws.send(blob);   // exceeds 600/s binary budget
  const bcode = await bclosed;
  ok("oversize flood closed with 4029", bcode === 4029);

  // --- matchmaking: role=queue hosts an empty room, joins an occupied one ---
  const q1 = await connect("room=QUEUE2&role=queue&v=2");
  const q1w = await jmsgWait(q1, "welcome");
  ok("queue on empty room lands as host", q1w && q1w.role === "host" && q1w.slot === 0);
  const q2 = await connect("room=QUEUE2&role=queue&v=2");
  const q2w = await jmsgWait(q2, "welcome");
  ok("queue on hosted room lands as join", q2w && q2w.role === "join" && q2w.slot === 1);
  const q1bad = await connect("room=QUEUE3&role=queue");   // v1 can't queue
  const q1err = await jmsgWait(q1bad, "error");
  ok("queue on v1 rejected", q1err && q1err.reason === "version-mismatch");
  q1.ws.close(); q2.ws.close(); q1bad.ws.close();

  // --- leaderboard: claim-key binds a name; bad payloads rejected ---
  const lbBase = `http://127.0.0.1:${PORT}`;
  const KEY_A = "0123456789abcdef0123456789abcdef";
  const KEY_B = "ffffffffffffffffffffffffffffffff";
  const post = (body) => fetch(`${lbBase}/submit`, {
    method: "POST", headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  });
  const r1 = await post({ name: "SecTest", key: KEY_A, rating: 1500, wins: 4, losses: 1 });
  ok("leaderboard submit accepted", r1.status === 200);
  const r2 = await post({ name: "SecTest", key: KEY_B, rating: 2000, wins: 9, losses: 0 });
  ok("leaderboard wrong-key rejected", r2.status === 400);
  const r3 = await post({ name: "SecTest", key: KEY_A, rating: 99999, wins: 0, losses: 0 });
  ok("leaderboard absurd rating rejected", r3.status === 400);
  const r4 = await post({ name: '"><script>', key: KEY_A, rating: 1300, wins: 1, losses: 0 });
  ok("leaderboard hostile name sanitized or rejected", r4.status !== 500);
  const lb = await (await fetch(`${lbBase}/leaderboard`)).json();
  const row = lb.entries.find((e) => e.name === "SecTest");
  ok("leaderboard lists claimed name at first rating",
     row && row.rating === 1500 && row.wins === 4);

  // --- verified results: /result only moves rating when two distinct
  // claim keys report the mirrored outcome for the same match_id ---
  const KEY_C = "cccccccccccccccccccccccccccccccc";
  const KEY_D = "dddddddddddddddddddddddddddddddd";
  const postRes = (body) => fetch(`${lbBase}/result`, {
    method: "POST", headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  });
  await post({ name: "AliceT", key: KEY_C, rating: 1500, wins: 0, losses: 0 });
  await post({ name: "BobT", key: KEY_D, rating: 1500, wins: 0, losses: 0 });

  const p1 = await (await postRes({ name: "AliceT", opp_name: "BobT", key: KEY_C,
    match_id: "aa11bb22cc33dd44", won: 1, duration: 6000 })).json();
  ok("first result report is pending", p1.kind === "pending");
  const v1 = await (await postRes({ name: "BobT", opp_name: "AliceT", key: KEY_D,
    match_id: "aa11bb22cc33dd44", won: 0, duration: 6000 })).json();
  ok("mirrored report verifies", v1.kind === "verified" && v1.winner === "AliceT");
  const lb2 = await (await fetch(`${lbBase}/leaderboard`)).json();
  const aRow = lb2.entries.find((e) => e.name === "AliceT");
  const bRow = lb2.entries.find((e) => e.name === "BobT");
  ok("verified result applied server-side Elo",
     aRow && bRow && aRow.rating === 1516 && bRow.rating === 1484 &&
     aRow.wins === 1 && bRow.losses === 1);

  const s1 = await postRes({ name: "AliceT", opp_name: "BobT", key: KEY_C,
    match_id: "aa11bb22cc33dd44", won: 1, duration: 6000 });
  ok("re-report of settled match rejected", s1.status === 400);

  await postRes({ name: "AliceT", opp_name: "BobT", key: KEY_C,
    match_id: "ee55ff66aa77bb88", won: 1, duration: 100 });
  const m1 = await (await postRes({ name: "BobT", opp_name: "AliceT", key: KEY_D,
    match_id: "ee55ff66aa77bb88", won: 1, duration: 100 })).json();
  ok("contradictory reports are a mismatch", m1.kind === "mismatch");

  const badId = await postRes({ name: "AliceT", opp_name: "BobT", key: KEY_C,
    match_id: "nope!", won: 1, duration: 10 });
  ok("hostile match_id rejected", badId.status === 400);
  const nonHexId = await postRes({ name: "AliceT", opp_name: "BobT", key: KEY_C,
    match_id: "zzzzzzzzzzzzzzzz", won: 1, duration: 10 });
  ok("non-hex match_id (8+ chars) rejected", nonHexId.status === 400);
  /* Cross-paired reports: A reports "beat C", D reports "beat B" under the
   * same match_id — names don't mirror, so it's a mismatch, not a verify. */
  const cp1 = await (await postRes({ name: "AliceT", opp_name: "CarolT", key: KEY_C,
    match_id: "cc00dd11ee22ff33", won: 1, duration: 50 })).json();
  ok("cross-pair first report pending", cp1.kind === "pending");
  /* Second report has the mirrored OUTCOME (loss vs prior's win) but names
   * that don't mirror the pairing — must mismatch, never verify. */
  const cp2 = await (await postRes({ name: "DaveT", opp_name: "BobT",
    key: "1234abcd1234abcd1234abcd1234abcd",
    match_id: "cc00dd11ee22ff33", won: 0, duration: 50 })).json();
  ok("cross-paired reports mismatch, not verify", cp2.kind === "mismatch");
  const badKey = await postRes({ name: "AliceT", opp_name: "BobT", key: "x",
    match_id: "1122334455667788", won: 1, duration: 10 });
  ok("weak claim key rejected", badKey.status === 400);
  const badWon = await postRes({ name: "AliceT", opp_name: "BobT", key: KEY_C,
    match_id: "1122334455667788", won: 7, duration: 10 });
  ok("out-of-range result rejected", badWon.status === 400);
  ok("mirror-verified reports are trusted=mirror", v1.trusted === "mirror");

  // --- server-issued match tokens: /match/begin mints the match_id and a
  // per-participant token, so a fabricated id can't reach the stronger
  // verification path; issued ids are consumed on settle. Dedicated
  // players keep this off AliceT's history/count assertions. ---
  const KEY_G1 = "12121212121212121212121212121212";
  const KEY_G2 = "34343434343434343434343434343434";
  await post({ name: "GwenT", key: KEY_G1, rating: 1500, wins: 0, losses: 0 });
  await post({ name: "HankT", key: KEY_G2, rating: 1500, wins: 0, losses: 0 });
  const postMatch = (p, body) => fetch(`${lbBase}${p}`, {
    method: "POST", headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  });
  const begin = await (await postMatch("/match/begin", {
    name: "GwenT", key: KEY_G1, opp_name: "HankT" })).json();
  ok("match begin mints id + caller token",
     begin.kind === "ok" && typeof begin.id === "string" &&
     typeof begin.token === "string");
  const badBegin = await postMatch("/match/begin", {
    name: "CarolT", key: "badbadbadbadbadb", opp_name: "HankT" });
  ok("match begin requires claim key", badBegin.status === 400);
  const beginStranger = await postMatch("/match/begin", {
    name: "GwenT", key: KEY_G1, opp_name: "Nobody" });
  ok("match begin requires claimed opponent", beginStranger.status === 400);
  const badClaim = await postMatch("/match/claim", {
    id: begin.id, name: "HankT", key: "badbadbadbadbadb" });
  ok("claim rejects foreign key", badClaim.status === 400);
  const claim = await (await postMatch("/match/claim", {
    id: begin.id, name: "HankT", key: KEY_G2 })).json();
  ok("opponent claims own token",
     claim.kind === "ok" && typeof claim.token === "string");

  const noTok = await postRes({ name: "GwenT", opp_name: "HankT", key: KEY_G1,
    match_id: begin.id, won: 1, duration: 5000 });
  ok("issued match requires token", noTok.status === 400);
  const wrongTok = await postRes({ name: "GwenT", opp_name: "HankT", key: KEY_G1,
    match_id: begin.id, won: 1, duration: 5000, token: "f".repeat(32) });
  ok("wrong token rejected", wrongTok.status === 400);
  const foreignKey = await postRes({ name: "GwenT", opp_name: "HankT", key: KEY_G2,
    match_id: begin.id, won: 1, duration: 5000, token: claim.token });
  ok("token must name its own slot", foreignKey.status === 400);
  const tp1 = await (await postRes({ name: "GwenT", opp_name: "HankT", key: KEY_G1,
    match_id: begin.id, won: 1, duration: 5000, token: begin.token })).json();
  ok("token report is pending", tp1.kind === "pending");
  const tv1 = await (await postRes({ name: "HankT", opp_name: "GwenT", key: KEY_G2,
    match_id: begin.id, won: 0, duration: 5000, token: claim.token })).json();
  ok("token path verifies trusted=token",
     tv1.kind === "verified" && tv1.trusted === "token");
  const reuse = await postRes({ name: "GwenT", opp_name: "HankT", key: KEY_G1,
    match_id: begin.id, won: 1, duration: 5000, token: begin.token });
  ok("issued id consumed after settle", reuse.status === 400);

  // --- moderation: /report needs a claimed reporter key; three distinct
  // reporters inside the window flag the target banned, and a banned key
  // can no longer submit or report results ---
  const postReport = (body) => fetch(`${lbBase}/report`, {
    method: "POST", headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  });
  const KEY_E = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
  const KEY_F = "ffffffffffffffffff00000000000000ff";
  await post({ name: "CarolT", key: KEY_E, rating: 1200, wins: 0, losses: 0 });
  const rep0 = await postReport({ name: "AliceT", key: "1".repeat(32) });
  ok("report from unknown reporter rejected", rep0.status === 400);
  const repSelf = await postReport({ name: "AliceT", key: KEY_C });
  ok("self-report rejected", repSelf.status === 400);
  const rep1 = await (await postReport({ name: "AliceT", key: KEY_A })).json();
  ok("first report counted, not banned", rep1.kind === "ok" && rep1.banned === false);
  const rep2 = await (await postReport({ name: "AliceT", key: KEY_D })).json();
  ok("second report counted", rep2.kind === "ok" && rep2.reports === 2);
  const rep3 = await (await postReport({ name: "AliceT", key: KEY_E })).json();
  ok("third distinct report bans", rep3.banned === true);
  ok("shadow flag set before ban", rep3.shadow === true);
  const repDup = await (await postReport({ name: "AliceT", key: KEY_E })).json();
  ok("repeat reporter doesn't double-count", repDup.reports === 3);
  const subBan = await post({ name: "AliceT", key: KEY_C, rating: 1600, wins: 5, losses: 0 });
  ok("banned key rejected on submit", subBan.status === 400);
  const resBan = await postRes({ name: "AliceT", opp_name: "BobT", key: KEY_C,
    match_id: "9988776655443322", won: 1, duration: 10 });
  ok("banned key rejected on result", resBan.status === 400);
  const repNoName = await postReport({ name: "NobodyHere", key: KEY_B });
  ok("report of unknown name rejected", repNoName.status === 400);
  const lbB = await (await fetch(`${lbBase}/leaderboard`)).json();
  ok("banned flag visible on board",
     lbB.entries.find((e) => e.name === "AliceT")?.banned === true);
  ok("shadow flag visible on board",
     lbB.entries.find((e) => e.name === "AliceT")?.shadow === true);

  // --- mute: a key with >=3 disputes loses report/result rights ---
  const KEY_M1 = "1111aaaa1111aaaa1111aaaa1111aaaa";
  const KEY_M2 = "2222bbbb2222bbbb2222bbbb2222bbbb";
  const KEY_M3 = "3333cccc3333cccc3333cccc3333cccc";
  await post({ name: "MuteT", key: KEY_M1, rating: 1200, wins: 0, losses: 0 });
  await post({ name: "VicT1", key: KEY_M2, rating: 1200, wins: 0, losses: 0 });
  await post({ name: "VicT2", key: KEY_M3, rating: 1200, wins: 0, losses: 0 });
  // 3 contradictory results: MuteT claims wins, opponents also claim wins
  for (const [mid, opp, okey] of [
      ["aaaa000011112222", "VicT1", KEY_M2],
      ["bbbb000011112222", "VicT2", KEY_M3],
      ["cccc000011112222", "VicT1", KEY_M2]]) {
    await postRes({ name: "MuteT", opp_name: opp, key: KEY_M1,
      match_id: mid, won: 1, duration: 10 });
    await postRes({ name: opp, opp_name: "MuteT", key: okey,
      match_id: mid, won: 1, duration: 10 });
  }
  const lbM = await (await fetch(`${lbBase}/leaderboard`)).json();
  ok("disputes reach mute threshold",
     (lbM.entries.find((e) => e.name === "MuteT")?.disputes || 0) >= 3);
  const muteRes = await postRes({ name: "MuteT", opp_name: "VicT1",
    key: KEY_M1, match_id: "dddd000011112222", won: 1, duration: 10 });
  ok("muted key rejected on result", muteRes.status === 400);
  const muteRep = await postReport({ name: "VicT1", key: KEY_M1 });
  ok("muted key rejected on report", muteRep.status === 400);
  const muteSub = await post({ name: "MuteT", key: KEY_M1, rating: 1200,
    wins: 0, losses: 0 });
  ok("muted key still may submit (mute != ban)", muteSub.status === 200);

  // --- build hash: optional self-reported attestation, recorded verbatim ---
  const KEY_H = "4444eeee4444eeee4444eeee4444eeee";
  await post({ name: "HashT", key: KEY_H, rating: 1300, wins: 0, losses: 0,
    build: "ff26ff6b31" });
  const lbH = await (await fetch(`${lbBase}/leaderboard`)).json();
  ok("build hash recorded on board",
     lbH.entries.find((e) => e.name === "HashT")?.build === "ff26ff6b31");
  const badHash = await post({ name: "HashT", key: KEY_H, rating: 1300,
    wins: 0, losses: 0, build: "not!hex!but!long!enough!to!pass!len" });
  ok("bad build hash ignored, prior kept",
     badHash.status === 200 &&
     (await (await fetch(`${lbBase}/leaderboard`)).json())
       .entries.find((e) => e.name === "HashT")?.build === "ff26ff6b31");

  // --- replay sharing: POST mints an id, GET serves the bytes back ---
  const repB64 = Buffer.from("fake-ssr-replay-bytes").toString("base64");
  const up = await (await fetch(`${lbBase}/replay`, {
    method: "POST", headers: { "content-type": "application/json" },
    body: JSON.stringify({ data: repB64, by: "SecTest" }),
  })).json();
  ok("replay upload mints id", up.ok === true && /^[0-9a-f]{10}$/.test(up.id));
  const got = await (await fetch(`${lbBase}/replay/${up.id}`)).json();
  ok("replay fetch returns bytes", got.data === repB64 && got.by === "SecTest");
  const miss = await fetch(`${lbBase}/replay/deadbeef00`);
  ok("unknown replay id 404s", miss.status === 404);
  const idx = await (await fetch(`${lbBase}/replays`)).json();
  ok("replay index lists the share newest-first",
     Array.isArray(idx.replays) && idx.replays[0]?.id === up.id);
  const badRp = await fetch(`${lbBase}/replay`, {
    method: "POST", headers: { "content-type": "application/json" },
    body: JSON.stringify({ data: "not!base64!" }),
  });
  ok("non-base64 replay rejected", badRp.status === 400);

  // --- public stats endpoint ---
  const st = await (await fetch(`${lbBase}/stats`)).json();
  ok("stats exposes counts",
     typeof st.leaderboard_entries === "number" &&
     typeof st.verified_matches === "number");
  ok("stats exposes moderation counters",
     typeof st.shadowed === "number" && typeof st.muted === "number" &&
     typeof st.banned === "number" && typeof st.distinct_builds === "number");
  ok("stats counts this run's moderation state",
     st.shadowed >= 1 && st.banned >= 1 && st.distinct_builds >= 1);

  // --- player card: /player?name=X ---
  const pc = await (await fetch(`${lbBase}/player?name=SecTest`)).json();
  ok("player card has rank + matches",
     pc.name === "SecTest" && typeof pc.rank === "number" &&
     Array.isArray(pc.matches));
  const pcNone = await fetch(`${lbBase}/player?name=NobodyHere`);
  ok("player card 404 for stranger", pcNone.status === 404);
  const pcBad = await fetch(`${lbBase}/player`);
  ok("player card 400-ish for missing name", pcBad.status === 404);

  // --- tournament bracket: create -> join -> start -> verified settle ---
  // Dedicated clean players — AliceT is banned and MuteT muted by the
  // moderation checks above, so neither can report results.
  const KEY_TA = "5555aaaa5555aaaa5555aaaa5555aaaa";
  const KEY_TB = "6666bbbb6666bbbb6666bbbb6666bbbb";
  await post({ name: "TourA", key: KEY_TA, rating: 1500, wins: 0, losses: 0 });
  await post({ name: "TourB", key: KEY_TB, rating: 1500, wins: 0, losses: 0 });
  const postTour = (p, body) => fetch(`${lbBase}${p}`, {
    method: "POST", headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  });
  const tourBad = await postTour("/tournament",
    { name: "Cup", by: "Nobody", key: "1".repeat(32), size: 4 });
  ok("tournament create requires claimed owner", tourBad.status === 400);
  const tour = await (await postTour("/tournament",
    { name: "Cup", by: "SecTest", key: KEY_A, size: 2 })).json();
  ok("tournament create mints id", tour.ok === true && tour.size === 2);
  const joinBad = await postTour("/tournament/join",
    { id: tour.id, name: "TourA", key: "1".repeat(32) });
  ok("tournament join requires own claim key", joinBad.status === 400);
  const join1 = await postTour("/tournament/join",
    { id: tour.id, name: "TourA", key: KEY_TA });
  const join2 = await postTour("/tournament/join",
    { id: tour.id, name: "TourB", key: KEY_TB });
  ok("tournament joins accepted", join1.status === 200 && join2.status === 200);
  // Banned/muted players can't post results — they may not join a bracket.
  const joinBanned = await postTour("/tournament/join",
    { id: tour.id, name: "AliceT", key: KEY_C });
  ok("banned player can't join tournament", joinBanned.status === 400);
  const joinMuted = await postTour("/tournament/join",
    { id: tour.id, name: "MuteT", key: KEY_M1 });
  ok("muted player can't join tournament", joinMuted.status === 400);
  const startBad = await postTour("/tournament/start",
    { id: tour.id, key: KEY_TB });
  ok("tournament start is owner-only", startBad.status === 400);
  const start = await postTour("/tournament/start", { id: tour.id, key: KEY_A });
  ok("tournament starts seeded", start.status === 200);
  const view1 = await (await fetch(`${lbBase}/tournament?id=${tour.id}`)).json();
  ok("bracket view shows live pairing",
     view1.status === "live" && view1.rounds.length === 1 &&
     view1.rounds[0].some((m) => m.a && m.b));
  // Report the bracket match through /result — same two-key corroboration
  const tid = "f00dcafe1234abcd";
  const pairing = view1.rounds[0].find((m) => m.a && m.b);
  const [tA, tB] = [pairing.a, pairing.b];
  const keyOf = (n) => n === "TourA" ? KEY_TA : KEY_TB;
  await postRes({ name: tA, opp_name: tB, key: keyOf(tA),
    match_id: tid, won: 1, duration: 120 });
  const vres = await (await postRes({ name: tB, opp_name: tA,
    key: keyOf(tB), match_id: tid, won: 0, duration: 120 })).json();
  ok("bracket match verified via /result", vres.kind === "verified");
  const view2 = await (await fetch(`${lbBase}/tournament?id=${tour.id}`)).json();
  ok("bracket settled on verified result",
     view2.status === "done" && view2.winner === tA);
  const joinDone = await postTour("/tournament/join",
    { id: tour.id, name: "SecTest", key: KEY_A });
  ok("join after done rejected", joinDone.status === 400);
  const viewAll = await (await fetch(`${lbBase}/tournament`)).json();
  ok("tournament list returns array", Array.isArray(viewAll.brackets));
  const viewMiss = await fetch(`${lbBase}/tournament?id=deadbeef00`);
  ok("unknown bracket 404s", viewMiss.status === 404);

  // --- public portal page: GET / serves the same API surface ---
  const portal = await fetch(`${lbBase}/`);
  const portalHtml = await portal.text();
  ok("portal page served as HTML",
     portal.status === 200 && portalHtml.includes("/leaderboard") &&
     portal.headers.get("content-type").includes("text/html"));

  // --- match history: verified results are listed, ?name= filters ---
  const hist = await (await fetch(`${lbBase}/history`)).json();
  const hRec = (hist.matches || []).find(
    (m) => m.w === "AliceT" && m.l === "BobT" && m.draw === false);
  ok("history lists the verified match", !!hRec && typeof hRec.at === "string");
  const histAlice = await (await fetch(`${lbBase}/history?name=AliceT`)).json();
  ok("history ?name= filters to played matches",
     (histAlice.matches || []).length === 1 && histAlice.matches[0].w === "AliceT");
  const histNobody = await (await fetch(`${lbBase}/history?name=NobodyHere`)).json();
  ok("history ?name= for stranger is empty",
     Array.isArray(histNobody.matches) && histNobody.matches.length === 0);

  // --- per-room join flood: upgrade attempts to one room are bounded at
  // MAX_JOINS_PER_WINDOW (24/10s) — the room code is the only secret, so
  // guessing costs connections. 26 joins to a live room → rate-limited. ---
  {
    const rlHost = await connect("room=RATELM&role=host&v=2");
    await jmsgWait(rlHost, "welcome");
    let roomRejects = 0;
    const roomAttempts = [];
    for (let i = 0; i < 26; i++) {
      roomAttempts.push(connect("room=RATELM&role=join&v=2")
        .then(async (c) => {
          const m = await jmsgWait(c, "error", 1500);
          c.ws.close();
          return m && m.reason === "rate-limited" ? "rate-limited" : "open";
        })
        .catch(() => "rate-limited"));
    }
    for (const r of await Promise.all(roomAttempts)) {
      if (r === "rate-limited") roomRejects++;
    }
    ok("per-room join flood rate-limited", roomRejects >= 1);
    rlHost.ws.close();
  }

  // --- per-IP join flood: the cross-code scan throttle caps join
  // attempts per IP per window (AUD-25). 40 rapid joins to distinct
  // rooms from this host must see at least one "rate-limited" close. ---
  {
    let joinRejects = 0;
    const attempts = [];
    for (let i = 0; i < 40; i++) {
      // CODE_RE charset excludes 0/1/I/O — derive codes from A–H, J–N, P–Z, 2–9.
      const alpha = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
      const room = "FL" + alpha[(i * 7) % alpha.length] + alpha[(i * 13) % alpha.length]
        + alpha[(i * 5) % alpha.length] + alpha[i % alpha.length];
      attempts.push(connect(`room=${room}&role=join&v=2`)
        .then(async (c) => {
          const m = await jmsgWait(c, "error", 1500);
          c.ws.close();
          return m && m.reason === "rate-limited" ? "rate-limited" : "open";
        })
        .catch(() => "rate-limited"));
    }
    for (const r of await Promise.all(attempts)) {
      if (r === "rate-limited") joinRejects++;
    }
    ok("join flood rate-limited", joinRejects >= 1);
  }

  host.ws.close(); c1.ws.close(); c2.ws.close(); up1.ws.close();
  host2.ws.close(); spec.ws.close(); cSpec.ws.close();
  await sleep(150);
  srv.kill();
  const fails = results.filter(([, c]) => !c).length;
  console.log(`\n${results.length - fails}/${results.length} checks passed`);
  process.exit(fails ? 1 : 0);
}

main().catch((e) => { console.error(e); process.exit(1); });
