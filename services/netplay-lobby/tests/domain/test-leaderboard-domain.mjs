// Property test for the leaderboard domain (no server — pure domain):
// drives a seeded random op sequence (submit / reportResult / report),
// then asserts the persistence ring is a FIXED POINT:
//   fromJSON(toJSON(x)) serializes back to the same JSON — twice.
// and the derived reads stay consistent:
//   list() ordering holds, playerCard agrees with list()+historyFor,
//   history entries only reference known match_ids.
// Run: node tests/domain/test-leaderboard-domain.mjs [iterations]
import { Leaderboard } from "../../src/domain/leaderboard.mjs";

const ITERS = parseInt(process.argv[2] || "300", 10);
let rng = (parseInt(process.env.LB_SEED || "4242", 10)) >>> 0;
const next = () => (rng = (rng * 1664525 + 1013904223) >>> 0);
const pick = (arr) => arr[next() % arr.length];
const hex = (n) =>
  Array.from({ length: n }, () => "0123456789abcdef"[next() & 15]).join("");

const NAMES = ["Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot",
               "Golf", "Hotel", "India", "Juliet", "Kilo", "Lima"];
const KEYS = Object.fromEntries(NAMES.map(() => [hex(32), true]));
const nameKey = {};   // name -> its claim key (fixed per name)
NAMES.forEach((n) => { nameKey[n] = hex(32); });

const results = [];
const ok = (n, c) => { results.push([n, c]); if (!c) console.log(`FAIL ${n}`); };

const lb = new Leaderboard();
const issuedMatchIds = [];

for (let i = 0; i < ITERS; i++) {
  const op = next() % 6;
  const name = pick(NAMES);
  if (op <= 1) {
    lb.submit({ name, key: nameKey[name], rating: 800 + next() % 2000,
                wins: next() % 40, losses: next() % 40,
                build: next() % 3 === 0 ? hex(8) : undefined });
  } else if (op === 2) {
    const opp = pick(NAMES.filter((n) => n !== name));
    const mid = next() % 4 === 0 && issuedMatchIds.length
      ? pick(issuedMatchIds)              // sometimes finish a pending match
      : hex(16);
    if (!issuedMatchIds.includes(mid)) issuedMatchIds.push(mid);
    lb.reportResult({ name, opp_name: opp, key: nameKey[name],
                      match_id: mid, won: next() % 3 - 1,
                      duration: next() % 10000 });
  } else if (op === 3 && lb.pending.size > 0) {
    /* Close a pending match with the mirrored report — exercises the
     * verified path (Elo + history) instead of only the pending/mismatch
     * branches. 2/3 of the time consistent, else contradictory. */
    const [mid, prior] = [...lb.pending.entries()][next() % lb.pending.size];
    const consistent = next() % 3 !== 0;
    const won = consistent
      ? (prior.won === -1 ? -1 : 1 - prior.won)
      : prior.won;                        // same outcome => mismatch
    lb.reportResult({ name: prior.opp, opp_name: prior.name,
                      key: nameKey[prior.opp] || hex(32),
                      match_id: mid, won, duration: next() % 10000 });
  } else if (op === 3) {
    lb.report({ name, key: nameKey[pick(NAMES)] });
  } else if (op === 4) {
    lb.report({ name, key: nameKey[pick(NAMES)] });
  } else {
    // hostile inputs must never throw
    lb.submit(pick([null, 42, "x", {}, { name }, { name, key: "z" }]));
    lb.reportResult(pick([null, [], { name }, { name, key: nameKey[name] }]));
    lb.report(pick([null, "", { name }, { name, key: "!!" }]));
  }
}

// ---- property 1: serialization round-trip is a fixed point ----
const j1 = JSON.stringify(lb.toJSON());
const lb2 = Leaderboard.fromJSON(JSON.parse(j1));
const j2 = JSON.stringify(lb2.toJSON());
ok("toJSON -> fromJSON -> toJSON is stable", j1 === j2);
const lb3 = Leaderboard.fromJSON(JSON.parse(j2));
ok("second round-trip identical", j2 === JSON.stringify(lb3.toJSON()));

// ---- property 2: derived views consistent after reload ----
const l1 = lb.list(), l2 = lb2.list();
ok("list() identical after reload", JSON.stringify(l1) === JSON.stringify(l2));
ok("historyFor identical after reload",
   JSON.stringify(lb.historyFor(null)) === JSON.stringify(lb2.historyFor(null)));
ok("list sorted by rating desc",
   l1.every((e, i) => i === 0 || l1[i - 1].rating >= e.rating));
for (const e of l1) {
  const c = lb.playerCard(e.name);
  if (!c || c.name !== e.name || c.rating !== e.rating ||
      !Array.isArray(c.matches) || typeof c.rank !== "number" || c.rank < 1) {
    ok(`playerCard consistent for ${e.name}`, false);
    break;
  }
}
ok("playerCard returns null for stranger", lb.playerCard("ZzNoSuch") === null);

// ---- property 3: history only references settled match ids ----
ok("history ids are all settled",
   lb.history.every((m) => lb.settled.has(m.id)));

// ---- property 4: moderation state survives the ring ----
for (const e of l1) {
  if (e.banned && !e.shadow) {
    // ban implies the shadow threshold was crossed first
    ok(`banned ${e.name} is also shadowed`, false);
  }
}
ok("moderation flags consistent post-reload",
   JSON.stringify(lb.fleetStats()) === JSON.stringify(lb2.fleetStats()));

const fails = results.filter(([, c]) => !c).length;
console.log(`${results.length - fails}/${results.length} property checks passed` +
            ` (iters=${ITERS}, entries=${lb.byName.size},` +
            ` history=${lb.history.length}, settled=${lb.settled.size})`);
process.exit(fails ? 1 : 0);
