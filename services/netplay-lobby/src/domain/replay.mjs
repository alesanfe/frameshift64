/* Shared replay-share policy for the Cloudflare worker and the local
 * node lobby — both frontends MUST enforce identical limits or a pack
 * accepted locally gets rejected in production. Keep every magic number
 * for /replay here, never inline in the infrastructure layer. */

export const REPLAY_MAX_DATA = 400 * 1024;   // base64 payload cap (~300 KB binary)
export const REPLAY_RING = 200;              // stored replays before eviction
export const REPLAY_BY_MAX = 24;             // uploader display-name cap
export const REPLAY_ID_RE = /^[0-9a-f]{10}$/;
const B64_RE = /^[A-Za-z0-9+/=\r\n]+$/;

export function mintReplayId() {
  return crypto.randomUUID().replaceAll("-", "").slice(0, 10);
}

/* validateReplayBody(body) -> {data, by} | null. `data` is the verified
 * base64 blob; `by` is the uploader name truncated to display bounds. */
export function validateReplayBody(body) {
  if (body == null || typeof body !== "object" ||
      typeof body.data !== "string" ||
      body.data.length === 0 || body.data.length > REPLAY_MAX_DATA ||
      !B64_RE.test(body.data)) {
    return null;
  }
  const by = typeof body.by === "string" ? body.by.slice(0, REPLAY_BY_MAX) : "";
  return { data: body.data, by };
}

/* evictOldest(index) -> evicted records. Caller drops each one's
 * backing store (DO key, file, …). */
export function evictOldest(index) {
  const gone = [];
  while (index.length > REPLAY_RING) gone.push(index.shift());
  return gone;
}
