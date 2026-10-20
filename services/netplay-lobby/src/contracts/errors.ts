/**
 * contracts/errors.ts — the domain's result vocabulary.
 *
 * Every domain operation returns a discriminated union instead of
 * throwing or returning null; the `kind` tag is the contract the HTTP
 * presenter switches on. Type-only — no runtime exports; .mjs
 * consumers reference these via JSDoc `import("./x.js")` types.
 *
 * Reject reasons are additive-only: clients may dispatch on them.
 */

/** Reasons a domain call may reject — union of every literal the
 * domain actually returns (keep in sync; checkJs surfaces drift when a
 * file opts in). */
export type RejectReason =
  // shared
  | "bad-request" | "bad-name" | "bad-key" | "banned" | "muted"
  // submit / board
  | "bad-rating" | "name-taken"
  // match lifecycle
  | "unknown-opp" | "self-match" | "no-match" | "bad-match"
  | "bad-result" | "bad-token" | "already-settled" | "mirror-disabled"
  // room admission
  | "rate-limited" | "no-such-room" | "room-full" | "room-in-use"
  | "version-mismatch" | "no-host";

export interface Reject {
  kind: "reject";
  reason: RejectReason;
}

/** A domain operation's outcome: T on success, Reject on failure. */
export type Result<T> = T | Reject;
