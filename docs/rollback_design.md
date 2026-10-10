# Rollback Snapshot/Restore Design

Status: snapshot/restore implemented, and SyncTest passes full VS matches. No networking yet.

This builds on `docs/netplay_architecture.md` (input boundary, debug P2P) and `docs/state_trace.md` (the verification oracle).

Code:
- `decomp/src/sys/netrollback.{c,h}`
- `port/netrollback_seg.h`
- `port/port_symbolize.cpp`
- `scripts/rollback_synctest.ps1`

## Tick boundary

VS runs `syTaskmanRunTask` → `syTaskmanCommonTaskUpdate`:
1. `syNetInputFuncRead` publishes input for tick T into `gSYControllerDevices`.
2. `scVSBattleFuncUpdate` runs the match logic.

`scVSBattleFuncUpdate` calls `syNetRollbackRunBattleUpdate(ifCommonBattleUpdateInterfaceAll)`. With SyncTest off, that is a plain call.

**Snapshots happen at that boundary.** The game coroutine is parked between ticks, so only loop locals are live.

## What a snapshot contains

| Region | How |
|---|---|
| Gameplay-module globals | MSVC `/FI port/netrollback_seg.h` puts every global of `ef ft gm gr if it mp sc wp` and `lb/lbcommon lb/lbparticle sys/objanim sys/objhelper sys/objman sys/objscript sys/utils` into the `.ssbd`/`.ssbb` sections. Markers in `netrollback.c` bound them, and save/restore is one `memcpy` each (about 280 KB of data plus 35 KB of BSS). The list lives in `CMakeLists.txt` as `SSB64_ROLLBACK_SEG_SOURCES`. |
| Scene arena | `[watermark, gSYTaskmanGeneralHeap.ptr)`. The watermark is taken at the top of `scVSBattleStartBattle`/`scVSBattleStartSuddenDeath`, so the render buffers below it are excluded. The allocator struct is saved too. This came to about 1.2 MB per snapshot in testing. |
| Excluded file ranges | Every reloc file copied into the arena is reported via `syNetRollbackNoteFileLoad` and skipped. Gameplay never writes file data. The port's lazy endian fixups do, and those are tracked in host-side "already fixed" sets that are not rolled back, so restoring file bytes would desync the two. |
| Fighter figatree heaps | Not excluded: status changes reload them, so their bytes are gameplay state. Their fixup verdicts live in per-range bitmaps in `port_aobj_fixup.cpp`, which are snapshotted alongside (`port_aobj_fixup_snapshot_*`). |
| Other | The netsync spawn serial and the match-timer tick counter (`sIFCommonTimerTicCount`, which is in the section). |
| Never rolled back | Netinput history rings, netpeer transport, audio, the controller globals (re-published every tick), and render state. |

## Fixes this required

1. **Match timer.** It counted VI retraces. Under `SSB64_NETPLAY` or SyncTest it now counts sim ticks. Offline play is unchanged and stays 1:1 with the N64.
2. **Reloc token churn.** `portRelocRegisterPointer` minted a fresh token on every registration, so every figatree reload produced new bytes and grew the token table for the whole match. Registration is now idempotent per live pointer.
3. **AObj event32 double un-halfswap (also affects offline play).** The parser calls the walker at the stream's current position. Two things went wrong:
   - Head-keyed sets couldn't see that the position sat inside an already-converted stream.
   - The raw-vs-unswapped heuristic accepted 1-command tails, which got swapped back to corrupt form.

   Conversion is now tracked per word (`kFixed`), and a walk stops at the first native word.

## Known divergences (non-gameplay)

- `FTStruct.p_sfx/sfx_id/p_voice/voice_id/p_loop_sfx/loop_sfx_id`. Audio handles get re-issued when the second pass replays the sound. Gameplay never branches on them.
- **To do:** suppress audio during resimulation and treat the handles as presentation state.

## Not rollback-able yet

Ticks where any GObj thread process is alive are skipped. That covers:
- the countdown and entry sequence
- the announcer
- time-up and sudden-death intros

For now netplay should run delay-based through those sequences. The longer-term fix is to convert those threads to func processes with explicit state.

## SyncTest

```powershell
.\scripts\rollback_synctest.ps1 -Stage 0 -Fighters "0,1,2,3" -Seed 12345
```

Environment variables:
- `SSB64_ROLLBACK_SYNCTEST=1`
- `SSB64_ROLLBACK_AUTOVS=1`: a 4-CPU level-9 match with no menus.
- `SSB64_ROLLBACK_STAGE`, `SSB64_ROLLBACK_FIGHTERS`, `SSB64_ROLLBACK_SEED`, `SSB64_ROLLBACK_MINUTES`
- `SSB64_RIG_EXIT=1`: exit 0 on PASS, 1 on FAIL.
- `SSB64_ROLLBACK_TRACE_TICK=N`: dumps every fighter joint's animation cursor around tested tick N.

How a tick is tested:
1. Save, run the tick, and hash it with `syNetSyncHashTick`.
2. Restore, run the tick again, and hash it again.
3. A gated-column mismatch is a FAIL.
4. A raw byte compare of section and arena names the diverging symbol (DbgHelp) or arena object (`FTStruct[link3]+0x…`, `DObj`, `AObj`, …).

## Next

1. Multi-frame rollback. Keep a ring of `SYNETROLLBACK_SLOT_NUM` snapshots, re-publish historic inputs from `netinput`, and resimulate N ticks per frame.
2. Suppress audio during resimulation.
3. Integrate GekkoNet over the `netpeer` transport.
4. Lobby, room codes and NAT traversal.
