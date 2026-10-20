/**
 * contracts/replay.ts — shared-replay upload and index shapes, plus the
 * crash-report record contract.
 *
 * Mirrors src/domain/replay.mjs (validateReplayBody / evictOldest) and
 * src/domain/crash.mjs (isValidCrashReport / crashReportRecord).
 * Type-only.
 */

/** POST /replay body as validated by validateReplayBody. */
export interface ReplayUploadBody {
  data?: unknown;   // base64 blob, <= REPLAY_MAX_DATA
  by?: unknown;     // uploader display name, truncated to REPLAY_BY_MAX
}

/** What validateReplayBody returns on success. */
export interface ValidatedReplayBody {
  data: string;
  by: string;
}

/** One entry in the shared replay index (id -> blob store). */
export interface ReplayIndexEntry {
  id: string;        // REPLAY_ID_RE: /^[0-9a-f]{10}$/
  at: string;        // ISO timestamp
  by: string;
}

/** Persisted replay record (index + payload stored by the adapter). */
export interface ReplayRecord extends ReplayIndexEntry {
  data: string;      // base64
}

/** POST /crash body (client crash bundle). `lus_tail` optional — older
 * clients only send info + log_tail. All strings capped at
 * CRASH_FIELD_MAX. */
export interface CrashReportBody {
  kind?: unknown;
  info?: unknown;
  log_tail?: unknown;
  lus_tail?: unknown;
}

/** The whitelisted record actually persisted — never spread the raw
 * body into storage. */
export interface CrashReportRecord {
  info: string;
  log_tail: string;
  lus_tail: string;
}
