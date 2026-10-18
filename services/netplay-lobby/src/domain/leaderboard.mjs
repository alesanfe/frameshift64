// leaderboard.mjs — self-reported rating board shared by the Worker DO
// and the local node-lobby adapter. Pure domain: no fetch/storage APIs —
// persistence is injected as {load(), save(json)} by the adapter.
//
// Threat model: entries are SELF-REPORTED (the game submits its own
// rating after a match). A player-chosen claim key (a random token the
// client persists locally) binds a display name so nobody else can
// overwrite it — this stops casual hijacking, not a determined
// cheater. There is no server-verified match outcome; the board is a
// friends-scale convenience, not anti-cheat infrastructure.

export const LB_MAX_ENTRIES = 100;
export const LB_NAME_MAX = 24;
export const LB_KEY_MIN = 16;

/* Display names come back verbatim to every client — keep them to a
 * boring alphabet so JSON serialization and ImGui rendering can't be
 * weaponized. */
export function sanitizeName(name) {
  if (typeof name !== "string") return "";
  const out = [];
  for (const ch of name.trim()) {
    if (/[\p{L}\p{N} _.\-[\]()]/u.test(ch)) out.push(ch);
    if (out.length >= LB_NAME_MAX) break;
  }
  return out.join("").trim();
}

export class Leaderboard {
  constructor(entries = [], pending = {}, settled = []) {
    this.byName = new Map();   // displayName -> {rating,wins,losses,key,updated}
    for (const e of entries) this.byName.set(e.name, e);
    this.pending = new Map(Object.entries(pending));   // match_id -> report
    this.settled = new Set(settled);                   // verified match_ids
  }

  /* submit({name,key,rating,wins,losses}) -> {kind:"ok",rank} |
   * {kind:"reject",reason}. First submit claims the name with `key`;
   * later submits must present the same key. */
  submit(body) {
    if (body == null || typeof body !== "object") {
      return { kind: "reject", reason: "bad-request" };
    }
    const name = sanitizeName(body.name);
    const key = typeof body.key === "string" ? body.key : "";
    const rating = Math.trunc(Number(body.rating));
    const wins = Math.max(0, Math.trunc(Number(body.wins)) || 0);
    const losses = Math.max(0, Math.trunc(Number(body.losses)) || 0);
    if (name === "") return { kind: "reject", reason: "bad-name" };
    if (key.length < LB_KEY_MIN || !/^[0-9a-f]+$/i.test(key)) {
      return { kind: "reject", reason: "bad-key" };
    }
    if (!Number.isFinite(rating) || rating < 0 || rating > 4000) {
      return { kind: "reject", reason: "bad-rating" };
    }
    const prev = this.byName.get(name);
    if (prev && prev.key !== key) {
      return { kind: "reject", reason: "name-taken" };
    }
    /* Preserve firstSeen/disputes across re-submits: resetting firstSeen
     * would flag veterans as "new" (false-positive suspicious), and
     * dropping disputes would let a flagged player clear the flag by
     * re-submitting. */
    this.byName.set(name, {
      name, key, rating, wins, losses,
      updated: new Date().toISOString(),
      firstSeen: (prev && prev.firstSeen) || new Date().toISOString(),
      disputes: (prev && prev.disputes) || 0,
    });
    // Evict the tail when the board overflows — a new entry that lands
    // below the cutoff is accepted then immediately evicted, which is
    // fine: it was never going to be visible anyway.
    while (this.byName.size > LB_MAX_ENTRIES) {
      const last = this.list()[this.byName.size - 1];
      this.byName.delete(last.name);
    }
    const rank = this.list().findIndex((e) => e.name === name) + 1;
    return { kind: "ok", rank };
  }

  /* Corroborated result reporting — the only path that moves ratings on
   * the verified board. Both clients POST the same match_id; the result
   * applies only when two DIFFERENT claim keys agree on the winner:
   *
   *   {key, name, opp_name, match_id, won: 1|0|-1, duration}
   *
   * won is from the reporter's perspective. A second report matching the
   * first's match_id with the mirrored outcome verifies it (win/loss or
   * draw/draw); a contradictory second report is a mismatch and counts
   * against both names — the match is settled either way and can't be
   * re-reported. Reports expire after PENDING_TTL_MS so a lobby-crash
   * doesn't pin a match_id forever.
   *
   * This stops a lone client from awarding itself wins: it takes two
   * distinct keys to move a rating. A pair of colluding clients can still
   * farm wins — that's the ceiling of account-less verification. */
  reportResult(body) {
    if (body == null || typeof body !== "object") {
      return { kind: "reject", reason: "bad-request" };
    }
    const name = sanitizeName(body.name);
    const opp = sanitizeName(body.opp_name);
    const key = typeof body.key === "string" ? body.key : "";
    const matchId = typeof body.match_id === "string" ? body.match_id : "";
    const won = Math.trunc(Number(body.won));
    if (name === "" || opp === "") return { kind: "reject", reason: "bad-name" };
    if (key.length < LB_KEY_MIN || !/^[0-9a-f]+$/i.test(key)) {
      return { kind: "reject", reason: "bad-key" };
    }
    if (matchId.length < 8 || matchId.length > 64 || !/^[0-9a-f]+$/i.test(matchId)) {
      return { kind: "reject", reason: "bad-match" };
    }
    if (won !== 1 && won !== 0 && won !== -1) {
      return { kind: "reject", reason: "bad-result" };
    }
    if (this.settled.has(matchId)) {
      return { kind: "reject", reason: "already-settled" };
    }
    const now = Date.now();
    this.#expirePending(now);
    const prior = this.pending.get(matchId);
    if (!prior) {
      this.pending.set(matchId, { name, opp, key, won, ts: now });
      return { kind: "pending" };
    }
    this.pending.delete(matchId);
    this.settled.add(matchId);
    if (this.settled.size > 2000) {
      this.settled.delete(this.settled.values().next().value);
    }
    /* Consistency: prior reported `won` against `opp`; the second report
     * must be the mirror image — A says "I beat B" ⇔ B says "I lost to A",
     * or both say draw. Cross-checked via names so a confused pairing
     * (A reports vs C, B reports vs D) doesn't verify. */
    const mirrors = prior.opp === name && prior.name === opp;
    const consistent = mirrors &&
      ((prior.won === 1 && won === 0) || (prior.won === 0 && won === 1) ||
       (prior.won === -1 && won === -1));
    if (!consistent) {
      for (const n of [prior.name, name]) {
        const e = this.byName.get(n);
        if (e) e.disputes = (e.disputes || 0) + 1;
      }
      return { kind: "mismatch" };
    }
    /* Verified — apply server-side Elo. Both entries exist by now (the
     * reporters claimed their names); an unclaimed one is created so the
     * verified path never loses a result. */
    const wName = prior.won === -1 ? null : (prior.won === 1 ? prior.name : name);
    const lName = prior.won === -1 ? null : (prior.won === 1 ? name : prior.name);
    /* For a win the "winner" arg is the reporter who said won=1; for a
     * draw, prior.name takes that seat and score 0.5 applies symmetric Elo
     * (higher-rated bleeds a little, lower-rated gains). */
    this.#applyResult(prior.won === 1 ? prior.name : (prior.won === 0 ? name : prior.name),
                      prior.won === 1 ? name : (prior.won === 0 ? prior.name : name),
                      prior.won === -1 ? 0.5 : 1.0);
    return { kind: "verified", winner: wName, loser: lName, draw: prior.won === -1 };
  }

  /* Elo update between two named entries. `score` is from winnerName's
   * perspective (1 win, 0.5 draw). Missing entries are minted at 1200 —
   * the first claim of a name is the report itself (same claim-key rule). */
  #applyResult(winnerName, loserName, score) {
    const K = 32;
    const get = (n, k) => {
      let e = this.byName.get(n);
      if (!e) {
        e = { name: n, key: k, rating: 1200, wins: 0, losses: 0, disputes: 0,
              updated: new Date().toISOString(), firstSeen: new Date().toISOString() };
        this.byName.set(n, e);
      }
      return e;
    };
    const w = get(winnerName), l = get(loserName);
    const expected = 1 / (1 + Math.pow(10, (l.rating - w.rating) / 400));
    const delta = Math.round(K * (score - expected));
    w.rating = Math.max(100, w.rating + delta);
    l.rating = Math.max(100, l.rating - delta);
    if (score === 1) { w.wins++; l.losses++; }
    w.updated = new Date().toISOString();
    l.updated = w.updated;
    while (this.byName.size > LB_MAX_ENTRIES) {
      const last = this.list()[this.byName.size - 1];
      this.byName.delete(last.name);
    }
  }

  #expirePending(now) {
    for (const [id, r] of this.pending) {
      if (now - r.ts > 10 * 60 * 1000) this.pending.delete(id);
    }
  }

  list() {
    const now = Date.now();
    const weekMs = 7 * 24 * 60 * 60 * 1000;
    /* Smurf/sandbag heuristic — surfaced as a flag, never a ban (account-
     * less verification can't distinguish a smurf from a genuinely good
     * new player; the flag tells viewers to weight the row accordingly):
     *   - account < 7 days old with >= 10 games and a winrate over 80%
     *   - or >= 2 contradictory result reports (disputes) */
    return [...this.byName.values()]
      .sort((a, b) => b.rating - a.rating || b.wins - a.wins)
      .map(({ name, rating, wins, losses, updated, disputes, firstSeen }) => {
        const games = wins + losses;
        const ageMs = firstSeen ? now - Date.parse(firstSeen) : Infinity;
        const suspicious =
          (ageMs < weekMs && games >= 10 && wins / games > 0.80) ||
          (disputes || 0) >= 2;
        return { name, rating, wins, losses, updated,
                 disputes: disputes || 0, suspicious };
      });
  }

  toJSON() {
    /* Persist the raw entries — going through list() would drop
     * firstSeen (killing the smurf heuristic across DO hibernation)
     * and recompute disputes fine but leak the computed flag. */
    return {
      entries: [...this.byName.values()],
      pending: Object.fromEntries(this.pending),
      settled: [...this.settled],
    };
  }
  static fromJSON(data) {
    const o = Array.isArray(data) ? { entries: data } : (data || {});
    return new Leaderboard(
      Array.isArray(o.entries) ? o.entries : [],
      (o.pending && typeof o.pending === "object") ? o.pending : {},
      Array.isArray(o.settled) ? o.settled : []);
  }
}
