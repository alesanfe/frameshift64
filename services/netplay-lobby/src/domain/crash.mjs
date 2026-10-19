/* Shared crash-report policy for the Cloudflare worker and the local
 * node lobby — both frontends MUST enforce identical field validation or
 * a report accepted locally gets dropped in production. Keep the field
 * list and every magic number for /crash here, never inline in the
 * infrastructure layer. */

export const CRASH_FIELD_MAX = 8192;   // per-field byte cap (info, log_tail, lus_tail)
export const CRASH_RING = 50;          // stored reports before eviction (worker)

/* isValidCrashReport(body) -> bool. `lus_tail` is optional (older clients
 * only sent info + log_tail); everything else is required. */
export function isValidCrashReport(body) {
  return body != null && typeof body === "object" &&
    body.kind === "crash" &&
    typeof body.info === "string" && body.info.length <= CRASH_FIELD_MAX &&
    typeof body.log_tail === "string" && body.log_tail.length <= CRASH_FIELD_MAX &&
    (body.lus_tail === undefined ||
     (typeof body.lus_tail === "string" && body.lus_tail.length <= CRASH_FIELD_MAX));
}

/* Fields to persist — never spread the raw client doc into storage:
 * unknown keys could smuggle unbounded junk past the validation above. */
export function crashReportRecord(body) {
  return {
    info: body.info,
    log_tail: body.log_tail,
    lus_tail: body.lus_tail || "",
  };
}
