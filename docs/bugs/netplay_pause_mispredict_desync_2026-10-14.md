# Netplay: mispredict during pause produced an unrecoverable desync (AUD-18)

**Status:** FIXED (decomp `netrollback.c`) — 2026-10-14
**Found by:** deep technical audit (`docs/investigations/deep_technical_audit_2026-10-14.md`, AUD-18, severity ALTA / MUY PROBABLE)

## Symptom

During netplay, if either player paused and a rollback mispredict landed on
a pause (or unpause) tick, `syNetRollbackResimulate` could not recover:
`FailedRollbacks++` and the two peers diverged permanently — typically one
peer unpausing while the other stayed frozen. The in-band sync check could
miss it entirely if divergence happened after the last checked hash.

## Root cause

`syNetRollbackCheckCanSave` only allowed snapshots while
`game_status == nSCBattleGameStatusGo`, leaving a hole in the snapshot
chain covering every pause/unpause tick. But remote inputs are *meaningful*
during pause: `ifCommonBattlePauseUpdateInterface` reads the pausing
peer's `button_tap`/`button_hold` for START (unpause), A+B+R+Z (quit to
CSS) and stick-driven pause-camera orbit. A mispredicted remote input on
one of those ticks changed the outcome (unpause vs. stay paused) yet had
no snapshot to resimulate from — the one class of tick where prediction
matters *and* rollback was impossible.

The original exclusion conflated two different guards. The
`game_status != Go` check was a proxy for "a GObjThread sequence is
running" (countdown, entry, announcer, sudden death — those keep live
state on host coroutine stacks that a snapshot cannot capture). Pause is
different: the pause menu runs as ordinary GObj *func processes* on the
game thread, no `GObjThread` is alive while paused, and all of its state
(`sIFCommonBattlePauseKindInterface`, pause-camera origins,
`CameraRestoreWait`, the `is_paused` process flags) lives in BSS/arena —
fully covered by the snapshot.

## Fix

`syNetRollbackCheckCanSave` now accepts `Go`, `Pause` and `Unpause`, and
keeps the `sGCThreadsActive != 0` rejection as the actual "a coroutine-
stacked thread is mid-flight" guard — that is the invariant snapshots
depend on, and it already excludes every genuinely unsafe sequence.

`End`, `BossDefeat`, `Set` and `Wait` still reject: those sequences spawn
GObjThreads and consume no meaningful remote inputs.

## Notes / follow-ups

- Resim across a pause tick re-publishes the pausing peer's confirmed
  inputs; the pause menu's deterministic camera/menu state replays
  identically.
- Audio from a resimulated transition (pause jingle, unpause) can repeat
  once — same class of cosmetic artifact as the existing "rewound frames
  are silent" limitation in `docs/netplay/netplay_online.md`.
- Verification pending: two instances + loss injection during pause with
  remote button presses (audit's suggested test); no build toolchain was
  available in this environment.
