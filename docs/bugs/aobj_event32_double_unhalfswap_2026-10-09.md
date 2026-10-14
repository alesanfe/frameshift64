# AObj event32 un-halfswap was order-dependent and could double-swap (2026-10-09)

**Status:** FIXED (`src/compatibility/port_aobj_fixup.cpp`). Found by the rollback SyncTest (`docs/netplay/rollback_design.md`).

## Symptom

SyncTest runs each VS tick twice from the same restored state. On a status change it found fighter joints whose `DObj.anim_joint` cursor and AObj interpolation state differed between the passes, for example at tested tick 298 on Peach's Castle with Mario/Fox/DK/Samus at seed 12345. The `joints` hash column diverged later in the match.

The trace (`SSB64_ROLLBACK_TRACE_TICK=298`) showed stream bytes under a joint changing during the first pass, for example `0000 200E` → `200E 0000`. So the second pass parsed different data from the same cursor.

## Root cause

Fighter figatree files are u16-halfswapped at load. EVENT32 streams inside them are converted lazily by `port_aobj_event32_unhalfswap_stream`, but `gcParseDObjAnimJoint` calls it with the stream's current position, not its head. The walker's bookkeeping was a set of head addresses:

- A later call at a position inside an already-converted stream wasn't in the set. The walker re-ran its raw-vs-unswapped heuristic on native data.
- That heuristic accepts the unswapped reading whenever the raw reading validates in fewer than 2 commands. A native 1-command tail (for example just before `End`) got swapped back to corrupt form.
- Whether that happened depended on which joint reached which address first and when, so the result was order-dependent.

Rollback also exposed a second problem. The verdict sets were global host state keyed by address, while the figatree heap bytes are gameplay state that rollback restores. Restored bytes and un-restored verdicts disagreed.

## Fix

- **Per-word idempotency.** A `kFixed` verdict records every u32 the walker converted. A walk that lands on a native word stops (`scan`), and a call whose position is already converted returns immediately (`walk`).
- **Per-range bitmaps.** Inside registered figatree ranges, all verdicts (`kHeads`, `kRejected`, `kFixed`, `kVisited`) live in per-range bitmaps with one bit per byte. Out-of-range keys fall back to sets.
- **Snapshot API.** `port_aobj_fixup_snapshot_{size,save,load}` serializes the bitmaps, so rollback restores heap bytes and verdicts together.

## Offline impact

The double-swap path exists without rollback. Whether it produced visible animation glitches in normal play has not been verified. It needs a 1-command native tail to be re-walked, which the SyncTest's re-execution made routine.

## Audit hook

A lazy in-place fixup that is keyed on where the reader happens to be, rather than on what has been converted, is order-dependent. Make in-place conversions idempotent per unit of data.
