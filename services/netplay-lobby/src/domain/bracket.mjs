// bracket.mjs — single-elimination tournament domain, shared by the
// Worker DO and the local node-lobby adapter. Pure domain: persistence
// is injected by the adapter (same pattern as leaderboard.mjs).
//
// A bracket is created by any claimed key, players join with their
// leaderboard claim key (prevents name-squatting inside a bracket), the
// owner seeds+starts it, and each pairing settles when the corresponding
// VERIFIED match_id lands on the leaderboard — bracket results reuse the
// two-party corroboration already proven for /result rather than
// trusting a single reporter.
//
// Trust ceiling: same account-less model as the leaderboard — colluding
// keys can still fake a win, so this is convenience infrastructure for
// community brackets, not anti-cheat.

export const BR_MAX_PLAYERS = 32;
export const BR_MIN_PLAYERS = 2;
export const BR_NAME_MAX = 32;
export const BR_ID_LEN = 10;
export const BR_MAX_LIVE = 8;      // live brackets cap per lobby instance

/* Shared ID mint — crypto.randomUUID() exists on Workers and Node ≥20
 * (globalThis.crypto), so both adapters mint from the same space. */
export function mintBracketId() {
  return crypto.randomUUID().replaceAll("-", "").slice(0, BR_ID_LEN);
}

/* Settle every live bracket waiting on this verified match_id. `brackets`
 * is any iterable of Bracket instances; returns the ones that changed so
 * the adapter can persist/write-back just those. */
export function settleLiveBrackets(brackets, matchId, winner, loser) {
  const changed = [];
  for (const b of brackets) {
    if (b.status === "live" &&
        b.settle(String(matchId || ""), winner, loser).kind !== "reject") {
      changed.push(b);
    }
  }
  return changed;
}

export function sanitizeBracketName(s) {
  return typeof s === "string"
    ? s.trim().replace(/[^\p{L}\p{N} _.\-]/gu, "").slice(0, BR_NAME_MAX)
    : "";
}

const pow2 = (n) => 2 ** Math.ceil(Math.log2(Math.max(n, BR_MIN_PLAYERS)));

export class Bracket {
  /* state = { id, name, owner, players: [{name,seed}], size, status,
   *           rounds: [[{a,b,matchId,winner}]], cursor, created, winner } */
  constructor(state) {
    Object.assign(this, state);
    this.players = this.players || [];
    this.rounds = this.rounds || [];
    this.status = this.status || "open";   // open | live | done
    this.winner = this.winner || null;
  }

  static create(id, name, ownerName, size) {
    const n = pow2(Math.min(BR_MAX_PLAYERS,
                            Math.max(BR_MIN_PLAYERS, size | 0 || 8)));
    return new Bracket({
      id, name, owner: ownerName, size: n, status: "open",
      players: [], rounds: [], winner: null,
      created: new Date().toISOString(),
    });
  }

  /* Join requires the name already claimed on the leaderboard — the
   * caller (adapter) verifies the key/name binding before calling. */
  join(name) {
    if (this.status !== "open") return { kind: "reject", reason: "closed" };
    if (this.players.some((p) => p.name === name)) {
      return { kind: "reject", reason: "already-joined" };
    }
    if (this.players.length >= this.size) {
      return { kind: "reject", reason: "full" };
    }
    this.players.push({ name, seed: this.players.length + 1 });
    return { kind: "ok", count: this.players.length };
  }

  /* Owner-only; pads with byes, seeds by leaderboard rating (highest
   * seed = highest rating), pairs classic 1-vs-N style. */
  start(seedOrder) {
    if (this.status !== "open") return { kind: "reject", reason: "not-open" };
    if (this.players.length < BR_MIN_PLAYERS) {
      return { kind: "reject", reason: "too-few" };
    }
    // seedOrder: caller-supplied rating sort (names, best first). Any
    // joined player missing from it lands at the bottom in join order.
    const ranked = [...seedOrder.filter((n) =>
                       this.players.some((p) => p.name === n)),
                    ...this.players.map((p) => p.name)
                      .filter((n) => !seedOrder.includes(n))];
    while (ranked.length < this.size) ranked.push(null);   // byes
    const r1 = [];
    for (let i = 0; i < this.size / 2; i++) {
      r1.push({ a: ranked[i], b: ranked[this.size - 1 - i],
                matchId: null, winner: ranked[i] && !ranked[this.size - 1 - i]
                                   ? ranked[i] : null });
    }
    this.rounds = [r1];
    this.status = "live";
    return { kind: "ok" };
  }

  /* Settle an open pairing from a verified result. Both parties of the
   * verified match must be this pairing's players — without the loser
   * check, any verified win by `a` over an outsider would settle a-b.
   * Called by the adapter after lb.reportResult() returns kind:"verified". */
  settle(matchId, winnerName, loserName) {
    if (this.status !== "live") return { kind: "reject", reason: "not-live" };
    for (const round of this.rounds) {
      for (const m of round) {
        if (m.winner == null && m.a != null && m.b != null &&
            ((m.a === winnerName && m.b === loserName) ||
             (m.b === winnerName && m.a === loserName)) &&
            (m.matchId == null || m.matchId === matchId)) {
          m.matchId = matchId;
          m.winner = winnerName;
          return this.#advance();
        }
      }
    }
    return { kind: "reject", reason: "no-open-pairing" };
  }

  /* Explicit pairing→match binding (either player may bind before the
   * result lands; settle() then accepts that match_id for the pairing). */
  bindMatch(aName, bName, matchId) {
    if (this.status !== "live") return { kind: "reject", reason: "not-live" };
    for (const round of this.rounds) {
      for (const m of round) {
        if (m.winner == null &&
            ((m.a === aName && m.b === bName) ||
             (m.a === bName && m.b === aName))) {
          m.matchId = matchId;
          return { kind: "ok" };
        }
      }
    }
    return { kind: "reject", reason: "no-such-pairing" };
  }

  #advance() {
    const cur = this.rounds[this.rounds.length - 1];
    /* Double-bye pairings (size > joined players leaves null-vs-null
     * slots) can never settle — they count as already-advanced and their
     * null propagates as a bye into the next round. */
    if (cur.some((m) => m.winner == null && (m.a != null || m.b != null))) {
      return { kind: "ok" };
    }
    const winners = cur.map((m) => m.winner);
    if (winners.length === 1) {
      this.status = "done";
      this.winner = winners[0];
      return { kind: "done", winner: this.winner };
    }
    const next = [];
    for (let i = 0; i < winners.length; i += 2) {
      next.push({ a: winners[i], b: winners[i + 1],
                  matchId: null, winner: winners[i + 1] == null
                                         ? winners[i] : null });
    }
    this.rounds.push(next);
    if (next.every((m) => m.winner != null)) return this.#advance();
    return { kind: "ok" };
  }

  /* Public view — everything a bracket page needs. */
  view() {
    return {
      id: this.id, name: this.name, owner: this.owner, size: this.size,
      status: this.status, winner: this.winner, created: this.created,
      players: this.players.map((p) => p.name),
      rounds: this.rounds,
    };
  }

  toJSON() {
    const { id, name, owner, size, status, winner, created, players,
            rounds } = this;
    return { id, name, owner, size, status, winner, created, players,
             rounds };
  }
  static fromJSON(o) { return new Bracket(o || {}); }
}
