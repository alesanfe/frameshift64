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
export const LB_BAN_REPORTS = 3;          // distinct reporter keys to ban
export const LB_SHADOW_REPORTS = 2;       // reports → shadow (pre-ban probation)
export const LB_MUTE_DISPUTES = 3;        // disputes → muted (report rights lost)
export const LB_REPORT_WINDOW_MS = 24 * 60 * 60 * 1000;
export const LB_HISTORY_MAX = 50;         // recent verified matches kept
export const LB_BUILD_MAX = 40;           // build-hash field cap
export const LB_MATCH_TTL_MS = 30 * 60 * 1000;  // issued-match token expiry
export const LB_MATCH_MAX = 512;          // live issued matches before eviction

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
  constructor(entries = [], pending = {}, settled = [], reports = {},
              history = [], matches = {}) {
    this.byName = new Map();   // displayName -> {rating,wins,losses,key,updated}
    for (const e of entries) this.byName.set(e.name, e);
    this.pending = new Map(Object.entries(pending));   // match_id -> report
    this.settled = new Set(settled);                   // verified match_ids
    /* Server-issued matches: match_id -> {at, slots: {key -> {name,token}}}.
     * A match_id minted HERE — not by the client — is the only id that can
     * verify through the token path (see reportResult). */
    this.matches = new Map(Object.entries(matches));
    // name -> Map(reporterKey -> ts) — moderation signal for /report
    this.reports = new Map();
    for (const [n, rs] of Object.entries(reports)) {
      this.reports.set(n, new Map(Object.entries(rs)));
    }
    /* Verified match records, oldest-first, capped at LB_HISTORY_MAX —
     * powers the public /history endpoint (match history per name is a
     * filter on top of this ring). */
    this.history = Array.isArray(history) ? history.slice(-LB_HISTORY_MAX) : [];
  }

  #bannedKey(key) {
    for (const e of this.byName.values()) {
      if (e.key === key && e.banned) return true;
    }
    return false;
  }

  /* Keys whose entries accumulated LB_MUTE_DISPUTES contradictory result
   * reports lose report/result rights — a reporter who can't keep their
   * outcomes straight is ignored, not banned (disputes already flag the
   * row as suspicious in list()). */
  #mutedKey(key) {
    for (const e of this.byName.values()) {
      if (e.key === key && (e.disputes || 0) >= LB_MUTE_DISPUTES) {
        return true;
      }
    }
    return false;
  }

  /* Optional build hash the client stamps on submits — /^[0-9a-f]{4,40}$.
   * Recorded verbatim; /stats surfaces the fleet's distinct-build count.
   * It's attestation, not enforcement: with no server-secured build
   * pipeline a claimed hash is self-reported like everything else. */
  #buildHash(v) {
    const s = typeof v === "string" ? v.trim().toLowerCase() : "";
    return (s.length >= 4 && s.length <= LB_BUILD_MAX && /^[0-9a-f]+$/.test(s))
      ? s : null;
  }

  /* True when `key` is the claim key for `name` — adapters use it to
   * authorize owner-scoped ops (tournament create/join/start). */
  keyOwns(name, key) {
    const e = this.byName.get(name);
    return !!e && e.key === key;
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
    if ((prev && prev.banned) || this.#bannedKey(key)) {
      return { kind: "reject", reason: "banned" };
    }
    /* Preserve firstSeen/disputes/shadow across re-submits: resetting
     * firstSeen would flag veterans as "new" (false-positive suspicious),
     * and dropping disputes/shadow would let a flagged player clear the
     * flag by re-submitting. */
    const build = this.#buildHash(body.build);
    this.byName.set(name, {
      name, key, rating, wins, losses,
      updated: new Date().toISOString(),
      firstSeen: (prev && prev.firstSeen) || new Date().toISOString(),
      disputes: (prev && prev.disputes) || 0,
      shadow: !!(prev && prev.shadow),
      banned: !!(prev && prev.banned),
      bannedAt: prev && prev.bannedAt,
      build: build || (prev && prev.build) || null,
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

  /* Server-issued match: POST /match/begin {name, key, opp_name}. The
   * caller must own `name` (claim key) and `opp_name` must be a claimed
   * board entry — so issued matches always bind two real participants.
   * Returns the caller's token only; the opponent fetches theirs via
   * claimMatch with their own key. The match_id is server-minted, so a
   * fabricated id can't reach the token path. */
  beginMatch(body) {
    if (body == null || typeof body !== "object") {
      return { kind: "reject", reason: "bad-request" };
    }
    const me = sanitizeName(body.name);
    const opp = sanitizeName(body.opp_name);
    const key = typeof body.key === "string" ? body.key : "";
    if (me === "" || opp === "") return { kind: "reject", reason: "bad-name" };
    if (!this.keyOwns(me, key)) return { kind: "reject", reason: "bad-key" };
    const oe = this.byName.get(opp);
    if (!oe) return { kind: "reject", reason: "unknown-opp" };
    if (oe.key === key) return { kind: "reject", reason: "self-match" };
    const now = Date.now();
    this.#expireMatches(now);
    const id = crypto.randomUUID().replaceAll("-", "");
    const meTok = crypto.randomUUID().replaceAll("-", "");
    const oppTok = crypto.randomUUID().replaceAll("-", "");
    this.matches.set(id, {
      at: now,
      slots: {
        [key]: { name: me, token: meTok },
        [oe.key]: { name: opp, token: oppTok },
      },
    });
    while (this.matches.size > LB_MATCH_MAX) {
      this.matches.delete(this.matches.keys().next().value);
    }
    return { kind: "ok", id, token: meTok };
  }

  /* POST /match/claim {id, name, key} — a participant retrieves their own
   * token. The key must be the issued slot key for that name, so only the
   * real opponent can read their token. */
  claimMatch(body) {
    if (body == null || typeof body !== "object") {
      return { kind: "reject", reason: "bad-request" };
    }
    const id = typeof body.id === "string" ? body.id : "";
    const name = sanitizeName(body.name);
    const key = typeof body.key === "string" ? body.key : "";
    const m = this.matches.get(id);
    if (!m) return { kind: "reject", reason: "no-match" };
    const slot = m.slots[key];
    if (!slot || slot.name !== name) return { kind: "reject", reason: "bad-key" };
    return { kind: "ok", token: slot.token };
  }

  #expireMatches(now) {
    for (const [id, m] of this.matches) {
      if (now - m.at > LB_MATCH_TTL_MS) this.matches.delete(id);
    }
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
    if (this.#bannedKey(key)) {
      return { kind: "reject", reason: "banned" };
    }
    if (this.#mutedKey(key)) {
      return { kind: "reject", reason: "muted" };
    }
    const now = Date.now();
    this.#expirePending(now);
    this.#expireMatches(now);
    /* Token path: when the match_id was server-issued, the report must
     * present the participant's issued token — the key alone isn't
     * enough (a leaked key could fabricate results on someone else's
     * issued match). Both reports must come from issued slot keys. */
    const issued = this.matches.get(matchId);
    if (issued) {
      const slot = issued.slots[key];
      const token = typeof body.token === "string" ? body.token : "";
      if (!slot || slot.name !== name || slot.token !== token) {
        return { kind: "reject", reason: "bad-token" };
      }
    }
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
    /* a/b = both participants (w/l alone can't name a draw — both null). */
    this.history.push({ id: matchId, a: prior.name, b: name, w: wName,
                        l: lName, draw: prior.won === -1,
                        trusted: issued ? "token" : "mirror",
                        at: new Date(now).toISOString() });
    while (this.history.length > LB_HISTORY_MAX) this.history.shift();
    /* An issued match verifies once — consume it so the id can't be
     * replayed for a second fabricated result. */
    if (issued) this.matches.delete(matchId);
    return { kind: "verified", winner: wName, loser: lName,
             draw: prior.won === -1, trusted: issued ? "token" : "mirror" };
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

  /* Player moderation report: {name, key, reporter}. `name` is the
   * reported entry, `key` the REPORTER's own claim key — the key must
   * already back some entry on the board, so a fresh throwaway can't
   * mint reports. LB_BAN_REPORTS distinct keys within LB_REPORT_WINDOW_MS
   * flag the target `banned`: banned keys are refused at submit/result
   * time and render flagged on the board. This is account-less community
   * moderation — a determined attacker with many claimed keys can still
   * force a ban; documented as the trust ceiling, same as /result. */
  report(body) {
    if (body == null || typeof body !== "object") {
      return { kind: "reject", reason: "bad-request" };
    }
    const name = sanitizeName(body.name);
    const key = typeof body.key === "string" ? body.key : "";
    if (name === "") return { kind: "reject", reason: "bad-name" };
    if (key.length < LB_KEY_MIN || !/^[0-9a-f]+$/i.test(key)) {
      return { kind: "reject", reason: "bad-key" };
    }
    const target = this.byName.get(name);
    if (!target) return { kind: "reject", reason: "no-such-name" };
    /* Reporter must own a claimed entry — reports aren't free. */
    let reporterKnown = false;
    for (const e of this.byName.values()) {
      if (e.key === key) { reporterKnown = true; break; }
    }
    if (!reporterKnown) return { kind: "reject", reason: "unknown-reporter" };
    if (target.key === key) {
      return { kind: "reject", reason: "self-report" };
    }
    if (this.#mutedKey(key)) {
      return { kind: "reject", reason: "muted" };
    }

    const now = Date.now();
    let rs = this.reports.get(name);
    if (!rs) { rs = new Map(); this.reports.set(name, rs); }
    for (const [k, ts] of rs) {
      if (now - ts > LB_REPORT_WINDOW_MS) rs.delete(k);
    }
    rs.set(key, now);
    /* Shadow at LB_SHADOW_REPORTS, ban at LB_BAN_REPORTS: shadow is the
     * pre-ban probation state — the entry stays playable but list()
     * flags it so matchmaking UIs can pool shadowed players separately
     * (the "shadow queue"). Ban adds the hard submit/result rejection. */
    if (rs.size >= LB_SHADOW_REPORTS && !target.shadow) {
      target.shadow = true;
    }
    if (rs.size >= LB_BAN_REPORTS && !target.banned) {
      target.banned = true;
      target.bannedAt = new Date(now).toISOString();
    }
    return { kind: "ok", reports: rs.size, shadow: !!target.shadow,
             banned: !!target.banned };
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
      .map(({ name, rating, wins, losses, updated, disputes, firstSeen,
              banned, shadow, build }) => {
        const games = wins + losses;
        const ageMs = firstSeen ? now - Date.parse(firstSeen) : Infinity;
        const suspicious =
          (ageMs < weekMs && games >= 10 && wins / games > 0.80) ||
          (disputes || 0) >= 2;
        return { name, rating, wins, losses, updated,
                 disputes: disputes || 0, suspicious, banned: !!banned,
                 shadow: !!shadow, muted: (disputes || 0) >= LB_MUTE_DISPUTES,
                 build: build || null };
      });
  }

  /* Public player card — everything the API can say about one claimed
   * name: board fields plus rank and that player's recent verified
   * matches. Returns null for unclaimed names. */
  playerCard(name) {
    const rows = this.list();
    const rank = rows.findIndex((e) => e.name === name) + 1;
    const e = rows[rank - 1];
    if (!e) return null;
    return { ...e, rank, matches: this.historyFor(name) };
  }

  /* Aggregate counters for /stats — distinct-build count exposes how
   * fragmented the deployed fleet is (build is self-reported, recorded
   * only when the client opts in by sending it). */
  fleetStats() {
    const builds = new Set();
    let shadowed = 0, muted = 0, banned = 0;
    for (const e of this.list()) {
      if (e.build) builds.add(e.build);
      if (e.shadow) shadowed++;
      if (e.muted) muted++;
      if (e.banned) banned++;
    }
    return { distinct_builds: builds.size, shadowed, muted, banned };
  }

  /* Recent verified matches, newest first. `name` (already sanitized by
   * the caller path) filters to matches that name played. */
  historyFor(name) {
    const rows = name == null
      ? this.history
      : this.history.filter((m) => m.a === name || m.b === name);
    return [...rows].reverse();
  }

  toJSON() {
    /* Persist the raw entries — going through list() would drop
     * firstSeen (killing the smurf heuristic across DO hibernation)
     * and recompute disputes fine but leak the computed flag. */
    return {
      entries: [...this.byName.values()],
      pending: Object.fromEntries(this.pending),
      settled: [...this.settled],
      reports: Object.fromEntries(
        [...this.reports].map(([n, rs]) => [n, Object.fromEntries(rs)])),
      history: this.history,
      matches: Object.fromEntries(this.matches),
    };
  }
  static fromJSON(data) {
    const o = Array.isArray(data) ? { entries: data } : (data || {});
    return new Leaderboard(
      Array.isArray(o.entries) ? o.entries : [],
      (o.pending && typeof o.pending === "object") ? o.pending : {},
      Array.isArray(o.settled) ? o.settled : [],
      (o.reports && typeof o.reports === "object") ? o.reports : {},
      Array.isArray(o.history) ? o.history : [],
      (o.matches && typeof o.matches === "object") ? o.matches : {});
  }
}
