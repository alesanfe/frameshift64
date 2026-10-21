/**
 * contracts/bracket.ts — single-elimination tournament shapes.
 *
 * Mirrors src/domain/bracket.mjs (the Bracket class' persisted state and
 * its discriminated results). Type-only — the .mjs implementation stays
 * the runtime authority; drift is caught when a file opts into
 * `// @ts-check`.
 */

import type { Reject } from "./errors.js";

/** Bracket lifecycle. open = accepting players, live = pairings are
 * being settled, done = a winner exists. */
export type BracketStatus = "open" | "live" | "done";

/** A joined player; `seed` is 1-based join order until start() reseeds. */
export interface BracketPlayer {
  name: string;
  seed: number;
}

/** One pairing inside a round. `a`/`b` are player names or null (bye or
 * not-yet-decided); matchId binds the pairing to a verified leaderboard
 * match once known; winner is null until settled. */
export interface BracketPairing {
  a: string | null;
  b: string | null;
  matchId: string | null;
  winner: string | null;
}

/** The persisted/restored shape — adapters serialize this via
 * Bracket.toJSON()/fromJSON(). */
export interface BracketState {
  id: string;
  name: string;
  owner: string;
  size: number;                    // power of two, BR_MIN_PLAYERS..BR_MAX_PLAYERS
  status: BracketStatus;
  players: BracketPlayer[];
  rounds: BracketPairing[][];      // rounds[0] is round 1
  winner: string | null;
  created: string;                 // ISO timestamp
}

/** join() result. */
export type BracketJoinResult =
  | Reject                          // closed | already-joined | full
  | { kind: "ok"; count: number };  // count = players now in

/** start() result — `done` is set when round 1 auto-resolved on byes and
 * the bracket finished immediately. */
export type BracketStartResult =
  | Reject                          // not-open | too-few
  | { kind: "ok"; done?: boolean };

/** settle()/advance() result — "done" carries the champion. */
export type BracketSettleResult =
  | Reject                          // not-live | no-open-pairing
  | { kind: "ok" }
  | { kind: "done"; winner: string | null };

/** bindMatch() result. */
export type BracketBindResult =
  | Reject                          // not-live | no-such-pairing
  | { kind: "ok" };

/** The public view served to bracket pages. */
export interface BracketView {
  id: string;
  name: string;
  owner: string;
  size: number;
  status: BracketStatus;
  winner: string | null;
  created: string;
  players: string[];               // names only
  rounds: BracketPairing[][];
}
