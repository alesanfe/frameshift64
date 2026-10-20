// Mutation testing for the lobby domain. Applies one targeted mutant at a
// time to a throwaway copy of src/, then runs the real security suite
// against a lobby spawned from the mutated tree. A mutant the suite can't
// detect is a surviving mutant — a hole in the tests, not just the code.
//
// Usage:
//   node tests/mutation/run-mutations.mjs [--threshold 0.70] [--only id1,id2]
//                                        [--timeout 90000] [--json path]
//
// Exit code is 0 when the kill rate meets --threshold (default 0.70), so CI
// can gate on it or run it informational-only via continue-on-error.
const path = require("path");
const fs = require("fs");
const os = require("os");
const { spawnSync } = require("child_process");

const SERVICE_DIR = path.join(__dirname, "..", "..");
const SECURITY_TEST = path.join(__dirname, "..", "security", "test-lobby-security.js");

// Each mutant: exact source replacement in a domain file. `find` must occur
// exactly once — zero hits means the mutant drifted out of date (reported
// INVALID, excluded from the score), more than one means `find` needs more
// context. Mutants target judgment calls: validation bounds, corroboration
// requirements, moderation thresholds, token binding, Elo direction.
const MUTANTS = [
  // ---- submit validation ----
  {
    id: "submit-rating-cap-raised",
    file: "src/domain/leaderboard.mjs",
    find: "rating < 0 || rating > 4000",
    replace: "rating < 0 || rating > 999999",
    why: "accept absurd ratings (test submits 99999 expecting reject)",
  },
  {
    id: "submit-name-taken-ok",
    file: "src/domain/leaderboard.mjs",
    find: `    if (prev && prev.key !== key) {
      return { kind: "reject", reason: "name-taken" };`,
    replace: `    if (false) {
      return { kind: "reject", reason: "name-taken" };`,
    why: "a second key can hijack a claimed name",
  },
  // ---- result corroboration ----
  {
    id: "result-won-any-value",
    file: "src/domain/leaderboard.mjs",
    find: "if (won !== 1 && won !== 0 && won !== -1) {",
    replace: "if (false) {",
    why: "out-of-range won values pass validation",
  },
  {
    id: "result-matchid-any-chars",
    file: "src/domain/leaderboard.mjs",
    find: "if (matchId.length < 8 || matchId.length > 64 || !/^[0-9a-f]+$/i.test(matchId)) {",
    replace: "if (matchId.length < 8 || matchId.length > 64 || false) {",
    why: "non-hex match ids pass validation",
  },
  {
    id: "result-settled-ignored",
    file: "src/domain/leaderboard.mjs",
    find: "    if (this.settled.has(matchId)) {",
    replace: "    if (false && this.settled.has(matchId)) {",
    why: "settled match ids can be replayed",
  },
  {
    id: "result-banned-key-ok",
    file: "src/domain/leaderboard.mjs",
    find: `    if (this.#bannedKey(key)) {
      return { kind: "reject", reason: "banned" };
    }
    if (this.#mutedKey(key)) {
      return { kind: "reject", reason: "muted" };
    }
    const now = Date.now();`,
    replace: `    if (false) {
      return { kind: "reject", reason: "banned" };
    }
    if (this.#mutedKey(key)) {
      return { kind: "reject", reason: "muted" };
    }
    const now = Date.now();`,
    why: "banned keys can still report results",
  },
  {
    id: "result-mirror-not-required",
    file: "src/domain/leaderboard.mjs",
    find: "const mirrors = prior.opp === name && prior.name === opp;",
    replace: "const mirrors = true;",
    why: "cross-paired reports (A-vs-C, B-vs-D) count as corroborated",
  },
  {
    id: "result-outcomes-not-checked",
    file: "src/domain/leaderboard.mjs",
    find: "(prior.won === 1 && won === 0) || (prior.won === 0 && won === 1) ||",
    replace: "true ||",
    why: "contradictory outcomes verify instead of counting as disputes",
  },
  // ---- match tokens ----
  {
    id: "token-not-verified",
    file: "src/domain/leaderboard.mjs",
    find: "if (!slot || slot.name !== name || slot.token !== token) {",
    replace: "if (!slot || slot.name !== name) {",
    why: "issued match accepts any token / no token",
  },
  {
    id: "token-slot-name-unbound",
    file: "src/domain/leaderboard.mjs",
    find: "if (!slot || slot.name !== name || slot.token !== token) {",
    replace: "if (!slot || slot.token !== token) {",
    why: "a valid token can report for a different participant name",
  },
  {
    id: "claim-any-key-gets-slot0",
    file: "src/domain/leaderboard.mjs",
    find: "const slot = m.slots[key];",
    replace: "const slot = Object.values(m.slots).find((s) => s.name === name);",
    why: "a foreign key can claim a participant's token by naming them",
  },
  // ---- moderation ----
  {
    id: "ban-threshold-removed",
    file: "src/domain/leaderboard.mjs",
    find: "if (rs.size >= LB_BAN_REPORTS && !target.banned) {",
    replace: "if (rs.size >= 99 && !target.banned) {",
    why: "bans never trigger no matter how many distinct reporters",
  },
  {
    id: "shadow-threshold-removed",
    file: "src/domain/leaderboard.mjs",
    find: "if (rs.size >= LB_SHADOW_REPORTS && !target.shadow) {",
    replace: "if (rs.size >= 99 && !target.shadow) {",
    why: "shadow probation never triggers",
  },
  {
    id: "mute-threshold-removed",
    file: "src/domain/leaderboard.mjs",
    find: "if (e.key === key && (e.disputes || 0) >= LB_MUTE_DISPUTES) {",
    replace: "if (e.key === key && (e.disputes || 0) >= 99) {",
    why: "disputed keys keep report/result rights forever",
  },
  {
    id: "unknown-reporter-ok",
    file: "src/domain/leaderboard.mjs",
    find: "if (!reporterKnown) return { kind: \"reject\", reason: \"unknown-reporter\" };",
    replace: "if (false) return { kind: \"reject\", reason: \"unknown-reporter\" };",
    why: "unclaimed keys can mint reports",
  },
  {
    id: "self-report-ok",
    file: "src/domain/leaderboard.mjs",
    find: "    if (target.key === key) {\n      return { kind: \"reject\", reason: \"self-report\" };\n    }",
    replace: "    if (false) {\n      return { kind: \"reject\", reason: \"self-report\" };\n    }",
    why: "players can report themselves",
  },
  // ---- Elo ----
  {
    id: "elo-zero-k",
    file: "src/domain/leaderboard.mjs",
    find: "const K = 32;",
    replace: "const K = 0;",
    why: "verified matches never move ratings",
  },
  {
    id: "elo-winner-drains",
    file: "src/domain/leaderboard.mjs",
    find: "w.rating = Math.max(100, w.rating + delta);",
    replace: "w.rating = Math.max(100, w.rating - delta);",
    why: "winners lose rating and losers gain it",
  },
  // ---- room domain ----
  {
    id: "room-join-limit-removed",
    file: "src/domain/room.mjs",
    find: "return this.recentJoins.length <= MAX_JOINS_PER_WINDOW;",
    replace: "return this.recentJoins.length >= 0;",
    why: "join-rate limiting never triggers",
    // The per-room limiter shares the per-IP join budget in the security
    // suite — the domain test exercises it in-process, uncontaminated.
    test: ["tests/domain/test-room-domain.mjs"],
  },
  {
    id: "room-binary-rate-removed",
    file: "src/domain/room.mjs",
    find: "return binary ? ++r.bin <= MAX_BINARY_PER_SEC : ++r.text <= MAX_TEXT_PER_SEC;",
    replace: "return binary ? ++r.bin <= 9999 : ++r.text <= MAX_TEXT_PER_SEC;",
    why: "binary uplink flooding is never throttled",
  },
];

// ---------- harness ----------

const argv = process.argv.slice(2);
const argVal = (flag, dflt) => {
  const i = argv.indexOf(flag);
  return i >= 0 ? argv[i + 1] : dflt;
};
const threshold = parseFloat(argVal("--threshold", "0.70"));
const timeoutMs = parseInt(argVal("--timeout", "90000"), 10);
const only = argVal("--only", null);
const jsonOut = argVal("--json", null);

function stageMutant(mut, workDir) {
  // Fresh copy of src/ per mutant inside the service dir so node_modules
  // resolution (hono, @hono/*, ws) keeps working via parent lookup. When a
  // mutant pins a test file, the test is staged too so its relative
  // ../../src imports resolve to the MUTATED tree.
  const staged = path.join(workDir, "src");
  fs.cpSync(path.join(SERVICE_DIR, "src"), staged, { recursive: true });
  const target = path.join(staged, mut.file.replace(/^src\//, ""));
  const src = fs.readFileSync(target, "utf8");
  const hits = src.split(mut.find).length - 1;
  if (hits !== 1) {
    return { ok: false, hits };
  }
  fs.writeFileSync(target, src.replace(mut.find, mut.replace));
  return { ok: true, script: path.join(staged, "infrastructure", "local", "node-lobby.mjs") };
}

function stageTest(mut, workDir) {
  if (!mut.test) return null;
  for (const t of mut.test) {
    const dest = path.join(workDir, t);
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    fs.cpSync(path.join(SERVICE_DIR, t), dest);
  }
  return mut.test.map((t) => path.join(workDir, t));
}

function runSuite(mut, workDir) {
  const stage = stageMutant(mut, workDir);
  if (!stage.ok) return { verdict: "INVALID", detail: `find matched ${stage.hits} times` };

  const dataDir = path.join(workDir, "data");
  fs.mkdirSync(dataDir, { recursive: true });
  const stagedTest = stageTest(mut, workDir);
  const testCmd = stagedTest || [SECURITY_TEST];
  const r = spawnSync(process.execPath, testCmd, {
    env: { ...process.env, LOBBY_SCRIPT: stage.script, LOBBY_DATA_DIR: dataDir },
    timeout: timeoutMs,
    encoding: "utf8",
    maxBuffer: 8 * 1024 * 1024,
  });
  const out = `${r.stdout || ""}${r.stderr || ""}`;
  if (r.error && r.error.code === "ETIMEDOUT") {
    return { verdict: "KILLED", detail: "suite timeout (mutant deadlocked the lobby)" };
  }
  const fails = (out.match(/^FAIL /gm) || []);
  if (fails.length === 0 && out.includes("checks passed")) {
    return { verdict: "SURVIVED", detail: "suite green against mutant" };
  }
  return { verdict: "KILLED", detail: fails[0] || `suite exited ${r.status}` };
}

async function main() {
  const mutants = only
    ? MUTANTS.filter((m) => only.split(",").includes(m.id))
    : MUTANTS;
  if (mutants.length === 0) {
    console.error(`--only matched no mutants; known ids:\n${MUTANTS.map((m) => "  " + m.id).join("\n")}`);
    process.exit(2);
  }

  const results = [];
  for (const mut of mutants) {
    const workDir = fs.mkdtempSync(path.join(SERVICE_DIR, ".mutation-"));
    const r = runSuite(mut, workDir);
    fs.rmSync(workDir, { recursive: true, force: true });
    results.push({ id: mut.id, file: mut.file, ...r });
    const tag = { KILLED: "KILLED ", SURVIVED: "SURVIVED", INVALID: "INVALID" }[r.verdict];
    console.log(`${tag} ${mut.id}  — ${mut.why}`);
    if (r.verdict !== "SURVIVED") console.log(`         ↳ ${r.detail}`);
  }

  const scored = results.filter((r) => r.verdict !== "INVALID");
  const killed = scored.filter((r) => r.verdict === "KILLED").length;
  const survivors = scored.filter((r) => r.verdict === "SURVIVED");
  const invalid = results.filter((r) => r.verdict === "INVALID");
  const score = scored.length ? killed / scored.length : 0;

  console.log("\n" + "=".repeat(60));
  console.log(`mutation score: ${(score * 100).toFixed(1)}%  (${killed}/${scored.length} killed)`);
  if (survivors.length) {
    console.log("\nsurviving mutants — tests don't cover these semantics:");
    for (const s of survivors) console.log(`  ${s.id} (${s.file})`);
  }
  if (invalid.length) {
    console.log("\ninvalid mutants — source drifted, update `find`:");
    for (const s of invalid) console.log(`  ${s.id}: ${s.detail}`);
  }

  if (jsonOut) {
    fs.writeFileSync(jsonOut, JSON.stringify({ score, killed, total: scored.length, results }, null, 2));
  }
  process.exit(score >= threshold ? 0 : 1);
}

main().catch((e) => { console.error("mutation harness error:", e); process.exit(2); });
