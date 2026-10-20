/**
 * contracts/match.ts — server-issued match lifecycle and result reports.
 *
 * Mirrors src/domain/leaderboard.mjs (Leaderboard.beginMatch /
 * claimMatch / reportResult). Type-only.
 *
 * The issued-match model: /match/begin mints the id + a token per
 * participant; each side redeems its own token via /match/claim; a
 * /result report on an issued id must present the slot key AND its
 * token. The legacy "mirror" path (client-chosen match_id, two
 * corroborating reports) is the deprecated fallback the
 * `LOBBY_DISABLE_MIRROR` switch retires — see docs/decisions/.
 */

import type { Reject } from "./errors.js";

/** A participant's issued credentials inside a match record. */
export interface MatchSlot {
  name: string;
  token: string;
}

/** Server-issued match record — keyed by the server-minted id. */
export interface IssuedMatch {
  at: number;                    // mint timestamp ms
  slots: Record<string, MatchSlot>; // claim key -> participant
}

/** Match lifecycle as a discriminated union — the states the room and
 * the leaderboard may hold a match in. `settled` is terminal: the id is
 * consumed on verify (issued matches delete on settlement). */
export type MatchState =
  | { kind: "pending"; at: number }
  | { kind: "claimed"; playerA: string; playerB: string }
  | { kind: "running"; startedAt: number }
  | { kind: "settled"; winner: string; loser: string; draw: boolean }
  | { kind: "expired"; expiredAt: number };

/** POST /match/begin body. `key` proves board-name ownership. */
export interface BeginMatchBody {
  name?: unknown;
  key?: unknown;
  opp_name?: unknown;
}

export type BeginMatchResult =
  | Reject
  | { kind: "ok"; id: string; token: string };

/** POST /match/claim body — a participant retrieves their own token. */
export interface ClaimMatchBody {
  id?: unknown;
  name?: unknown;
  key?: unknown;
}

export type ClaimMatchResult =
  | Reject
  | { kind: "ok"; token: string };

/** POST /result body. `won`: 1 win, 0 loss, -1 draw. `token` is
 * required on issued matches, ignored on the mirror path. */
export interface ReportBody {
  key?: unknown;
  name?: unknown;
  opp_name?: unknown;
  match_id?: unknown;
  won?: unknown;
  duration?: unknown;
  token?: unknown;
}

/** How a verified result was proven — surfaced for telemetry and the
 * mirror-retirement dashboard (see fleetStats.mirror_reports). */
export type VerificationMode = "token" | "mirror";

export type ReportResult =
  | Reject
  | { kind: "pending" }                       // first report stored, awaiting mirror
  | { kind: "mismatch" }                      // two reports disagree — both dropped
  | {
      kind: "verified";
      winner: string;
      loser: string;
      draw: boolean;
      trusted: VerificationMode;
    };

/** Board entry as stored/persisted (Leaderboard.byName values). */
export interface BoardEntry {
  name: string;
  rating: number;
  wins: number;
  losses: number;
  key: string;
  updated: number;
}
