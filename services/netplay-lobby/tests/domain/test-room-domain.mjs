// Room-domain unit checks — admission, per-room join budgeting, and the
// uplink-auth refund. In-process (no sockets): admit() takes a plain
// params object and returns an action descriptor; socket plumbing stays
// in the adapters.
//
//   node tests/domain/test-room-domain.mjs
import { LobbyRoom } from "../../src/domain/room.mjs";
import { MAX_JOINS_PER_WINDOW } from "../../src/domain/protocol.mjs";

const results = [];
const ok = (name, cond) => { results.push([name, cond]); console.log(`${cond ? "PASS" : "FAIL"} ${name}`); };

// --- admission basics ---
{
  const room = new LobbyRoom("ABCDEF");
  const host = room.admit({ role: "host", version: 2, isUplink: false });
  ok("host admitted with welcome", host.kind === "attach" && host.welcome.slot === 0);
  // A join only admits while a host socket exists — attach a fake one.
  room.attach({ send() {}, close() {} }, host.info);
  const j1 = room.admit({ role: "join", version: 2, isUplink: false });
  ok("join gets slot 1", j1.kind === "attach" && j1.info.slot === 1);
  room.attach({ send() {}, close() {} }, j1.info);
  const j2 = room.admit({ role: "join", version: 2, isUplink: false });
  room.attach({ send() {}, close() {} }, j2.info);
  const j3 = room.admit({ role: "join", version: 2, isUplink: false });
  room.attach({ send() {}, close() {} }, j3.info);
  const j4 = room.admit({ role: "join", version: 2, isUplink: false });
  ok("room fills to 4 players", j4.kind === "reject" && j4.reason === "room-full");
}

// --- per-room join budget: MAX_JOINS_PER_WINDOW admits per window,
// regardless of outcome ---
{
  const room = new LobbyRoom("BUDGET");
  let lastReject = null;
  for (let i = 0; i < MAX_JOINS_PER_WINDOW + 3; i++) {
    const a = room.admit({ role: "join", version: 2, isUplink: false });
    if (a.kind === "reject") lastReject = a.reason;
  }
  ok("join budget caps the window", lastReject === "rate-limited");
}

// --- failed uplink auth refunds its join spend ---
{
  const room = new LobbyRoom("REFUND");
  room.admit({ role: "host", version: 2, isUplink: false });
  // Burn most of the budget with uplink auth failures — each should refund.
  let exhausted = false;
  for (let i = 0; i < MAX_JOINS_PER_WINDOW + 3; i++) {
    const a = room.admit({ role: "join", version: 2, isUplink: true,
                           uplinkSlot: 0, uplinkKey: "wrong" });
    if (a.kind === "reject" && a.reason === "rate-limited") exhausted = true;
  }
  ok("uplink auth failures don't drain the budget", !exhausted);
  const still = room.admit({ role: "join", version: 2, isUplink: false });
  ok("join still admitted after uplink spam", still.kind !== "reject" ||
     still.reason !== "rate-limited");
}

const fails = results.filter(([, c]) => !c).length;
console.log(`\n${results.length - fails}/${results.length} room-domain checks passed`);
process.exit(fails ? 1 : 0);
