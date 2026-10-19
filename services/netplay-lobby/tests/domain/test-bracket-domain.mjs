// Property test for the bracket domain (no server — pure domain):
// drives a seeded random op sequence (create / join / start / bindMatch /
// settle across several brackets), then asserts structural invariants:
//   toJSON->fromJSON->toJSON is a FIXED POINT,
//   every pairing's players are joined names or bye-nulls,
//   a settled pairing's winner is one of its two players,
//   rounds halve correctly, done brackets name exactly one joined winner.
// Run: node tests/domain/test-bracket-domain.mjs [iterations]
import { Bracket, mintBracketId, settleLiveBrackets,
         sanitizeBracketName } from "../../src/domain/bracket.mjs";

const ITERS = parseInt(process.argv[2] || "300", 10);
let rng = (parseInt(process.env.BR_SEED || "99173", 10)) >>> 0;
const next = () => (rng = (rng * 1664525 + 1013904223) >>> 0);
const pick = (arr) => arr[next() % arr.length];
const hex = (n) =>
  Array.from({ length: n }, () => "0123456789abcdef"[next() & 15]).join("");

const NAMES = ["Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot",
               "Golf", "Hotel", "India", "Juliet", "Kilo", "Lima"];

const results = [];
const ok = (n, c) => { results.push([n, c]); if (!c) console.log(`FAIL ${n}`); };

const brackets = new Map();
let settles = 0;

for (let i = 0; i < ITERS; i++) {
  const op = next() % 7;
  if (op === 0 || brackets.size === 0) {
    const id = mintBracketId();
    brackets.set(id, Bracket.create(id, "Cup" + brackets.size,
                                    pick(NAMES), pick([2, 3, 4, 5, 8, 9, 16, 33, 0])));
  } else {
    const b = pick([...brackets.values()]);
    const name = pick(NAMES);
    if (op === 1) {
      b.join(name);
    } else if (op === 2) {
      b.join(next() % 4 === 0 ? name : pick([null, 42, "", {}, name]));
    } else if (op === 3) {
      b.start(next() % 2 === 0
        ? [...NAMES].sort(() => (next() % 3) - 1)   // shuffled seed order
        : []);                                       // no ratings known
    } else if (op === 4 && b.status === "live") {
      // settle a random open pairing — sometimes with the real pairing,
      // sometimes a winner/loser that should NOT settle it.
      const open = b.rounds.flat().filter(
        (m) => m.winner == null && m.a != null && m.b != null);
      if (open.length) {
        const m = pick(open);
        const honest = next() % 3 !== 0;
        const w = honest ? (next() % 2 ? m.a : m.b) : pick(NAMES);
        const l = honest ? (w === m.a ? m.b : m.a) : pick(NAMES);
        const mid = m.matchId || hex(16);
        const r = b.settle(mid, w, l);
        if (r.kind !== "reject") settles++;
      }
    } else if (op === 5 && b.status === "live") {
      const open = b.rounds.flat().filter(
        (m) => m.winner == null && m.a != null && m.b != null);
      if (open.length) {
        const m = pick(open);
        b.bindMatch(m.a, m.b, hex(16));
      }
    } else {
      // hostile inputs must never throw
      b.join(pick([null, 7, {}, []]));
      b.settle(pick([null, hex(4)]), pick(NAMES), pick(NAMES));
      b.start(pick([null, 12, "x"]));
    }
  }
}

// Shared live-settle helper: only live brackets change.
const changed = settleLiveBrackets(brackets.values(), hex(16),
                                   pick(NAMES), pick(NAMES));
ok("settleLiveBrackets only touches live brackets",
   changed.every((b) => b.status === "live"));

// ---- per-bracket structural invariants ----
let seenDone = 0, seenLive = 0, byes = 0;
for (const b of brackets.values()) {
  const tag = `bracket ${b.id} (${b.name})`;
  ok(`${tag}: players unique and within size`,
     new Set(b.players.map((p) => p.name)).size === b.players.length &&
     b.players.length <= b.size);
  ok(`${tag}: size is a power of two >= 2`,
     (b.size & (b.size - 1)) === 0 && b.size >= 2);
  const pnames = new Set(b.players.map((p) => p.name));
  for (const [ri, round] of b.rounds.entries()) {
    ok(`${tag}: round ${ri} has size/${2 ** (ri + 1)} pairings`,
       round.length === b.size / (2 ** (ri + 1)));
    for (const m of round) {
      ok(`${tag}: pairing players are joined or null`,
         (m.a == null || pnames.has(m.a)) && (m.b == null || pnames.has(m.b)));
      if (m.winner != null) {
        ok(`${tag}: settled winner is one of the pairing`,
           m.winner === m.a || m.winner === m.b);
      }
      if (m.a == null && m.b == null) byes++;
    }
  }
  if (b.status === "done") {
    seenDone++;
    ok(`${tag}: done bracket has a joined winner`,
       b.winner != null && pnames.has(b.winner));
    // every live pairing must be settled — no open holes in a done bracket
    ok(`${tag}: done bracket has no unsettled real pairings`,
       b.rounds.flat().every((m) => m.winner != null ||
                              (m.a == null && m.b == null)));
  }
  if (b.status === "live") seenLive++;
  // serialization fixed point per bracket
  const j1 = JSON.stringify(b.toJSON());
  const b2 = Bracket.fromJSON(JSON.parse(j1));
  ok(`${tag}: toJSON round-trip stable`, j1 === JSON.stringify(b2.toJSON()));
}

// ---- aggregate: the random walk actually exercised the interesting paths ----
ok("at least one bracket reached live", seenLive > 0);
ok("verified settles happened", settles > 0);
ok("sanitizeBracketName bounds hostile input",
   sanitizeBracketName("  <script>alert(1)</script>  ").length <= 32 &&
   !sanitizeBracketName("<x>").includes("<"));

const fails = results.filter(([, c]) => !c).length;
console.log(`${results.length - fails}/${results.length} property checks passed` +
            ` (iters=${ITERS}, brackets=${brackets.size}, live=${seenLive},` +
            ` done=${seenDone}, settles=${settles}, byes=${byes})`);
process.exit(fails ? 1 : 0);
