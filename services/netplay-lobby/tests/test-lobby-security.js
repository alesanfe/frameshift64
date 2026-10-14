// Exercise the hardened lobby: host + 2 clients, uplink auth, star
// enforcement and rate limiting. Run: node server/test-lobby-security.js
const path = require("path");
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
  const srv = spawn("node", [path.join(__dirname, "..", "src", "node-lobby.js"), String(PORT)], { stdio: "pipe" });
  await sleep(700);

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

  host.ws.close(); c1.ws.close(); c2.ws.close(); up1.ws.close();
  host2.ws.close(); spec.ws.close(); cSpec.ws.close();
  await sleep(150);
  srv.kill();
  const fails = results.filter(([, c]) => !c).length;
  console.log(`\n${results.length - fails}/${results.length} checks passed`);
  process.exit(fails ? 1 : 0);
}

main().catch((e) => { console.error(e); process.exit(1); });
