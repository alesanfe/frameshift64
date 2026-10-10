#include <sys/netrollback.h>
#include <sys/netsync.h>

#include <ft/fighter.h>
#include <it/item.h>
#include <wp/weapon.h>
#include <ef/effect.h>
#include <sc/sctypes.h>
#include <sc/scmanager.h>
#include <sys/malloc.h>
#include <sys/netinput.h>
#include <sys/netreplay.h>
#include <sys/taskman.h>
#include <sys/utils.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PORT
extern void port_log(const char *fmt, ...);
extern const char *port_symbolize(const void *addr, u32 *offset);
extern void port_exit_process(int code);
extern size_t port_aobj_fixup_snapshot_size(void);
extern size_t port_aobj_fixup_snapshot_save(void *dst, size_t capacity);
extern void port_aobj_fixup_snapshot_load(const void *src, size_t size);
extern char *getenv(const char *name);
extern int atoi(const char *s);
#endif

extern u32 sGCThreadsActive;
extern void *gPortSceneHeap;
extern const size_t gPortSceneHeapSize;

// // // // // // // // // // // //
//                               //
//        SECTION MARKERS        //
//                               //
// // // // // // // // // // // //

/* Bounds of the gameplay-global sections built by port/netrollback_seg.h.
 * The $a/$z subsections sort before/after every $m contribution. */
#if defined(_MSC_VER)
#pragma bss_seg(".ssbb$a")
u8 gSYNetRollbackBssStart;
#pragma bss_seg(".ssbb$z")
u8 gSYNetRollbackBssEnd;
#pragma data_seg(".ssbd$a")
u8 gSYNetRollbackDataStart = 1;
#pragma data_seg(".ssbd$z")
u8 gSYNetRollbackDataEnd = 1;
#pragma data_seg()
#pragma bss_seg()
#define SYNETROLLBACK_HAS_SECTIONS TRUE
#else
#define SYNETROLLBACK_HAS_SECTIONS FALSE
#endif

// // // // // // // // // // // //
//                               //
//   GLOBAL / STATIC VARIABLES   //
//                               //
// // // // // // // // // // // //

/* Slot SYNETROLLBACK_SLOT_NUM is private to SyncTest: the post-update state
 * of the first pass, compared byte-for-byte against the second pass. */
#define SYNETROLLBACK_SLOT_COMPARE SYNETROLLBACK_SLOT_NUM
#define SYNETROLLBACK_DIFF_REPORT_MAX 64

/* Reloc files loaded into the arena during the session. Gameplay never writes
 * file data; only the port's lazy endian fixups do, and those are tracked in
 * host-side "already fixed" sets. Restoring file bytes would desync the bytes
 * from those sets, so file ranges are excluded from snapshots entirely.
 *
 * Fighter animation (figatree) heaps are the exception: gameplay reloads
 * them on status changes, so their bytes ARE state. They stay in the
 * snapshot, and their fixup verdicts (port_aobj_fixup bitmaps) are saved
 * and restored alongside so bytes and verdicts always agree. */
#define SYNETROLLBACK_FILE_RANGE_MAX 512

typedef struct SYNetRollbackState
{
	sb32 is_valid;
	u8 *data;
	u8 *bss;
	u8 *arena;
	size_t arena_size;
	SYMallocRegion heap;
	u32 spawn_serial;
	u8 *aobj_fixup;
	size_t aobj_fixup_size;
	size_t aobj_fixup_capacity;

} SYNetRollbackState;

typedef struct SYNetRollbackRange
{
	u8 *lo;
	u8 *hi;

} SYNetRollbackRange;

static SYNetRollbackState sSYNetRollbackStates[SYNETROLLBACK_SLOT_NUM + 1];

static sb32 sSYNetRollbackIsSyncTest;
static sb32 sSYNetRollbackIsTickTimer;
static sb32 sSYNetRollbackIsSessionActive;
static sb32 sSYNetRollbackIsResimulating;
static sb32 sSYNetRollbackIsRigExit;
static u32 sSYNetRollbackTraceTick;
static u8 *sSYNetRollbackArenaWatermark;

static SYNetRollbackRange sSYNetRollbackFileRanges[SYNETROLLBACK_FILE_RANGE_MAX];
static s32 sSYNetRollbackFileRangesNum;
static sb32 sSYNetRollbackIsFileRangeOverflow;

static u32 sSYNetRollbackTotalTestedTicks;
static u32 sSYNetRollbackTotalSkippedTicks;
static u32 sSYNetRollbackTotalFailedTicks;
static u32 sSYNetRollbackTestedTicks;
static u32 sSYNetRollbackSkippedTicks;
static u32 sSYNetRollbackFailedTicks;
static u32 sSYNetRollbackColumnReported;
static u32 sSYNetRollbackDiffsReported;
static size_t sSYNetRollbackMaxArenaBytes;

// // // // // // // // // // // //
//                               //
//           FUNCTIONS           //
//                               //
// // // // // // // // // // // //

static size_t syNetRollbackGetDataSize(void)
{
#if SYNETROLLBACK_HAS_SECTIONS
	return (size_t)(&gSYNetRollbackDataEnd - &gSYNetRollbackDataStart);
#else
	return 0;
#endif
}

static size_t syNetRollbackGetBssSize(void)
{
#if SYNETROLLBACK_HAS_SECTIONS
	return (size_t)(&gSYNetRollbackBssEnd - &gSYNetRollbackBssStart);
#else
	return 0;
#endif
}

#ifdef PORT
static s32 syNetRollbackGetEnvInt(const char *name, s32 fallback)
{
	const char *env = getenv(name);

	return (env != NULL) ? atoi(env) : fallback;
}

/* SSB64_ROLLBACK_AUTOVS=1: skip the menus into a 4-CPU (level 9) VS match so
 * SyncTest runs unattended. Optional: SSB64_ROLLBACK_STAGE (gkind),
 * SSB64_ROLLBACK_FIGHTERS ("fkind,fkind,fkind,fkind"), SSB64_ROLLBACK_SEED,
 * SSB64_ROLLBACK_MINUTES (default 1). */
static void syNetRollbackSetupAutoVS(void)
{
	SCBattleState battle_state = dSCManagerDefaultBattleState;
	SYNetInputReplayMetadata metadata;
	const char *fighters_env = getenv("SSB64_ROLLBACK_FIGHTERS");
	u32 seed = (u32)syNetRollbackGetEnvInt("SSB64_ROLLBACK_SEED", 12345);
	s32 player;

	syNetReplayCaptureBattleMetadata(&battle_state, &metadata);

	metadata.rng_seed = seed;
	metadata.game_type = nSCBattleGameTypeRoyal;
	metadata.game_rules = SCBATTLE_GAMERULE_TIME;
	metadata.time_limit = syNetRollbackGetEnvInt("SSB64_ROLLBACK_MINUTES", 1);
	metadata.stage_kind = syNetRollbackGetEnvInt("SSB64_ROLLBACK_STAGE", nGRKindBattleStart + (seed % (nGRKindBattleEnd - nGRKindBattleStart + 1)));
	metadata.item_toggles = ~0;
	metadata.item_appearance_rate = nSCBattleItemSwitchHigh;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		s32 fkind = nFTKindPlayableStart + ((seed + player * 3) % (nFTKindPlayableEnd - nFTKindPlayableStart + 1));

		if (fighters_env != NULL)
		{
			fkind = atoi(fighters_env);

			while ((*fighters_env != '\0') && (*fighters_env != ','))
			{
				fighters_env++;
			}
			fighters_env = (*fighters_env == ',') ? fighters_env + 1 : NULL;
		}
		metadata.player_kinds[player] = nFTPlayerKindCom;
		metadata.fighter_kinds[player] = fkind;
		metadata.costumes[player] = 0;
		metadata.teams[player] = player;
		metadata.handicaps[player] = 9;
		metadata.levels[player] = 9;
		metadata.shades[player] = 0;
	}
	syNetReplayApplyBattleMetadata(&metadata);
	syUtilsSetRandomSeed(seed);
	gSCManagerSceneData.scene_prev = nSCKindVSMode;
	gSCManagerSceneData.scene_curr = nSCKindVSBattle;

	port_log("SSB64 Rollback: AutoVS stage=%u fighters=%u,%u,%u,%u seed=%u minutes=%u\n",
	         metadata.stage_kind, metadata.fighter_kinds[0], metadata.fighter_kinds[1],
	         metadata.fighter_kinds[2], metadata.fighter_kinds[3], seed, metadata.time_limit);
}
#endif

void syNetRollbackInitDebugEnv(void)
{
#ifdef PORT
	const char *synctest_env = getenv("SSB64_ROLLBACK_SYNCTEST");
	const char *netplay_env = getenv("SSB64_NETPLAY");

	sSYNetRollbackIsSyncTest = ((synctest_env != NULL) && (atoi(synctest_env) != 0)) ? TRUE : FALSE;

	if ((sSYNetRollbackIsSyncTest != FALSE) && (SYNETROLLBACK_HAS_SECTIONS == FALSE))
	{
		port_log("SSB64 Rollback: SyncTest needs MSVC section grouping; disabled on this toolchain\n");
		sSYNetRollbackIsSyncTest = FALSE;
	}
	/* The original match timer counts VI retraces. Resimulation runs several
	 * ticks inside one VI, so any rollback-capable session counts sim ticks
	 * instead. Offline play keeps the retrace-driven timer (1:1 with N64). */
	sSYNetRollbackIsTickTimer = ((sSYNetRollbackIsSyncTest != FALSE) || ((netplay_env != NULL) && (atoi(netplay_env) != 0))) ? TRUE : FALSE;

	sSYNetRollbackIsRigExit = (syNetRollbackGetEnvInt("SSB64_RIG_EXIT", 0) != 0) ? TRUE : FALSE;
	sSYNetRollbackTraceTick = (u32)syNetRollbackGetEnvInt("SSB64_ROLLBACK_TRACE_TICK", 0);

	if (syNetRollbackGetEnvInt("SSB64_ROLLBACK_AUTOVS", 0) != 0)
	{
		syNetRollbackSetupAutoVS();
	}
	if (sSYNetRollbackIsSyncTest != FALSE)
	{
		port_log("SSB64 Rollback: SyncTest enabled data=%u bss=%u bytes\n",
		         (unsigned)syNetRollbackGetDataSize(), (unsigned)syNetRollbackGetBssSize());
		{
			u32 sym_offset;
			const char *sym = port_symbolize(&sSYNetRollbackTestedTicks, &sym_offset);

			port_log("SSB64 Rollback: symbolizer probe -> %s\n", (sym != NULL) ? sym : "(unavailable)");
		}
	}
#endif
}

/* Called first thing in the VS task start function: everything the arena
 * holds below this point is the scene's render buffers, never rolled back. */
/* One VS scene can hold two sessions (timed tie -> sudden death); each is
 * reported on its own and folded into the scene total used by the verdict. */
static void syNetRollbackReportSession(void)
{
#ifdef PORT
	if ((sSYNetRollbackIsSyncTest == FALSE) || (sSYNetRollbackIsSessionActive == FALSE))
	{
		return;
	}
	port_log("SSB64 Rollback: session end status=%d time_passed=%u time_remain=%u is_reset=%d stocks=%d,%d,%d,%d\n",
	         gSCManagerBattleState->game_status, gSCManagerBattleState->time_passed, gSCManagerBattleState->time_remain,
	         gSCManagerSceneData.is_reset, gSCManagerBattleState->players[0].stock_count, gSCManagerBattleState->players[1].stock_count,
	         gSCManagerBattleState->players[2].stock_count, gSCManagerBattleState->players[3].stock_count);
	port_log("SSB64 Rollback: session tested=%u failed=%u skipped=%u max_arena=%u bytes\n",
	         sSYNetRollbackTestedTicks, sSYNetRollbackFailedTicks, sSYNetRollbackSkippedTicks,
	         (unsigned)sSYNetRollbackMaxArenaBytes);
#endif
	sSYNetRollbackTotalTestedTicks += sSYNetRollbackTestedTicks;
	sSYNetRollbackTotalSkippedTicks += sSYNetRollbackSkippedTicks;
	sSYNetRollbackTotalFailedTicks += sSYNetRollbackFailedTicks;
}

void syNetRollbackStartVSSession(void)
{
	s32 i;

	syNetRollbackReportSession();

	for (i = 0; i < ARRAY_COUNT(sSYNetRollbackStates); i++)
	{
		sSYNetRollbackStates[i].is_valid = FALSE;
	}
	sSYNetRollbackArenaWatermark = (u8*)gSYTaskmanGeneralHeap.ptr;
	sSYNetRollbackFileRangesNum = 0;
	sSYNetRollbackIsFileRangeOverflow = FALSE;
	sSYNetRollbackIsSessionActive = TRUE;
	sSYNetRollbackIsResimulating = FALSE;
	sSYNetRollbackTestedTicks = 0;
	sSYNetRollbackSkippedTicks = 0;
	sSYNetRollbackFailedTicks = 0;
	sSYNetRollbackColumnReported = 0;
	sSYNetRollbackDiffsReported = 0;
	sSYNetRollbackMaxArenaBytes = 0;
}

void syNetRollbackFinishVSSession(void)
{
#ifdef PORT
	if ((sSYNetRollbackIsSyncTest != FALSE) && (sSYNetRollbackIsSessionActive != FALSE))
	{
		syNetRollbackReportSession();

		port_log("SSB64 Rollback: SyncTest %s tested=%u failed=%u skipped=%u\n",
		         (sSYNetRollbackTotalFailedTicks == 0) ? "PASS" : "FAIL",
		         sSYNetRollbackTotalTestedTicks, sSYNetRollbackTotalFailedTicks, sSYNetRollbackTotalSkippedTicks);

		if (sSYNetRollbackIsRigExit != FALSE)
		{
			port_exit_process((sSYNetRollbackTotalFailedTicks == 0) ? 0 : 1);
		}
		sSYNetRollbackTotalTestedTicks = 0;
		sSYNetRollbackTotalSkippedTicks = 0;
		sSYNetRollbackTotalFailedTicks = 0;
	}
#endif
	sSYNetRollbackIsSessionActive = FALSE;
}

sb32 syNetRollbackIsTickTimer(void)
{
	return sSYNetRollbackIsTickTimer;
}

sb32 syNetRollbackIsResimulating(void)
{
	return sSYNetRollbackIsResimulating;
}

/* Called by the reloc loader for every file it copies into memory. Ranges are
 * kept sorted and merged; only arena memory above the setup watermark is
 * tracked (everything below it is never snapshotted anyway). */
void syNetRollbackNoteFileLoad(void *dst, size_t size, sb32 is_figatree)
{
	u8 *lo = (u8*)dst;
	u8 *hi = lo + size;
	u8 *arena_end;
	s32 i;
	s32 j;

	if ((sSYNetRollbackIsSessionActive == FALSE) || (is_figatree != FALSE) || (size == 0) || (gPortSceneHeap == NULL))
	{
		return;
	}
	arena_end = (u8*)gPortSceneHeap + gPortSceneHeapSize;

	if ((hi <= sSYNetRollbackArenaWatermark) || (lo >= arena_end))
	{
		return;
	}
	if (lo < sSYNetRollbackArenaWatermark)
	{
		lo = sSYNetRollbackArenaWatermark;
	}
	if (hi > arena_end)
	{
		hi = arena_end;
	}
	/* Absorb every range overlapping or touching [lo, hi), then insert. */
	for (i = 0; i < sSYNetRollbackFileRangesNum; )
	{
		SYNetRollbackRange *range = &sSYNetRollbackFileRanges[i];

		if ((range->hi >= lo) && (range->lo <= hi))
		{
			if (range->lo < lo)
			{
				lo = range->lo;
			}
			if (range->hi > hi)
			{
				hi = range->hi;
			}
			for (j = i; j < sSYNetRollbackFileRangesNum - 1; j++)
			{
				sSYNetRollbackFileRanges[j] = sSYNetRollbackFileRanges[j + 1];
			}
			sSYNetRollbackFileRangesNum--;
		}
		else i++;
	}
	if (sSYNetRollbackFileRangesNum >= SYNETROLLBACK_FILE_RANGE_MAX)
	{
		if (sSYNetRollbackIsFileRangeOverflow == FALSE)
		{
#ifdef PORT
			port_log("SSB64 Rollback: file range table full; rollback disabled for this session\n");
#endif
			sSYNetRollbackIsFileRangeOverflow = TRUE;
		}
		return;
	}
	for (i = sSYNetRollbackFileRangesNum; (i > 0) && (sSYNetRollbackFileRanges[i - 1].lo > lo); i--)
	{
		sSYNetRollbackFileRanges[i] = sSYNetRollbackFileRanges[i - 1];
	}
	sSYNetRollbackFileRanges[i].lo = lo;
	sSYNetRollbackFileRanges[i].hi = hi;
	sSYNetRollbackFileRangesNum++;
}

/* Copies [watermark, watermark + size) between the live arena and a snapshot
 * image laid out at the same offsets, skipping the file ranges. */
static void syNetRollbackCopyArena(u8 *image, size_t size, sb32 is_save)
{
	u8 *live = sSYNetRollbackArenaWatermark;
	size_t cursor = 0;
	s32 i;

	for (i = 0; (i < sSYNetRollbackFileRangesNum) && (cursor < size); i++)
	{
		size_t lo = (size_t)(sSYNetRollbackFileRanges[i].lo - live);
		size_t hi = (size_t)(sSYNetRollbackFileRanges[i].hi - live);

		if (lo > size)
		{
			lo = size;
		}
		if (lo > cursor)
		{
			if (is_save != FALSE)
			{
				memcpy(image + cursor, live + cursor, lo - cursor);
			}
			else memcpy(live + cursor, image + cursor, lo - cursor);
		}
		if (hi > cursor)
		{
			cursor = hi;
		}
	}
	if (cursor < size)
	{
		if (is_save != FALSE)
		{
			memcpy(image + cursor, live + cursor, size - cursor);
		}
		else memcpy(live + cursor, image + cursor, size - cursor);
	}
}

/* GObj thread processes keep live state on host coroutine stacks, which a
 * snapshot cannot capture; only snapshot while none exist (they run during
 * the countdown, entry, announcer and sudden-death sequences). */
sb32 syNetRollbackCheckCanSave(void)
{
	if ((SYNETROLLBACK_HAS_SECTIONS == FALSE) || (sSYNetRollbackIsSessionActive == FALSE))
	{
		return FALSE;
	}
	if ((gSCManagerBattleState == NULL) || (gSCManagerBattleState->game_status != nSCBattleGameStatusGo))
	{
		return FALSE;
	}
	if (sGCThreadsActive != 0)
	{
		return FALSE;
	}
	if (sSYNetRollbackIsFileRangeOverflow != FALSE)
	{
		return FALSE;
	}
	return TRUE;
}

static sb32 syNetRollbackAllocState(SYNetRollbackState *state)
{
	if (state->data == NULL)
	{
		state->data = malloc(syNetRollbackGetDataSize() + 1);
		state->bss = malloc(syNetRollbackGetBssSize() + 1);
		state->arena = malloc(gPortSceneHeapSize);
	}
	return ((state->data != NULL) && (state->bss != NULL) && (state->arena != NULL)) ? TRUE : FALSE;
}

static sb32 syNetRollbackSaveState(SYNetRollbackState *state)
{
#if SYNETROLLBACK_HAS_SECTIONS
	size_t arena_size = (size_t)((u8*)gSYTaskmanGeneralHeap.ptr - sSYNetRollbackArenaWatermark);
	s32 i;

	if (syNetRollbackAllocState(state) == FALSE)
	{
		return FALSE;
	}
	memcpy(state->data, &gSYNetRollbackDataStart, syNetRollbackGetDataSize());
	memcpy(state->bss, &gSYNetRollbackBssStart, syNetRollbackGetBssSize());
	syNetRollbackCopyArena(state->arena, arena_size, TRUE);

#ifdef PORT
	{
		size_t aobj_size = port_aobj_fixup_snapshot_size();

		if (aobj_size > state->aobj_fixup_capacity)
		{
			u8 *grown = realloc(state->aobj_fixup, aobj_size);

			if (grown == NULL)
			{
				return FALSE;
			}
			state->aobj_fixup = grown;
			state->aobj_fixup_capacity = aobj_size;
		}
		state->aobj_fixup_size = port_aobj_fixup_snapshot_save(state->aobj_fixup, state->aobj_fixup_capacity);
	}
#endif
	state->arena_size = arena_size;
	state->heap = gSYTaskmanGeneralHeap;
	state->spawn_serial = syNetSyncGetSpawnSerial();
	state->is_valid = TRUE;

	if (arena_size > sSYNetRollbackMaxArenaBytes)
	{
		sSYNetRollbackMaxArenaBytes = arena_size;
	}
	return TRUE;
#else
	return FALSE;
#endif
}

sb32 syNetRollbackSave(s32 slot)
{
	if ((slot < 0) || (slot > SYNETROLLBACK_SLOT_NUM) || (syNetRollbackCheckCanSave() == FALSE))
	{
		return FALSE;
	}
	return syNetRollbackSaveState(&sSYNetRollbackStates[slot]);
}

void syNetRollbackLoad(s32 slot)
{
#if SYNETROLLBACK_HAS_SECTIONS
	SYNetRollbackState *state;
	s32 i;

	if ((slot < 0) || (slot > SYNETROLLBACK_SLOT_NUM))
	{
		return;
	}
	state = &sSYNetRollbackStates[slot];

	if (state->is_valid == FALSE)
	{
		return;
	}
	memcpy(&gSYNetRollbackDataStart, state->data, syNetRollbackGetDataSize());
	memcpy(&gSYNetRollbackBssStart, state->bss, syNetRollbackGetBssSize());
	syNetRollbackCopyArena(state->arena, state->arena_size, FALSE);

	gSYTaskmanGeneralHeap = state->heap;
	syNetSyncSetSpawnSerial(state->spawn_serial);

#ifdef PORT
	port_aobj_fixup_snapshot_load(state->aobj_fixup, state->aobj_fixup_size);
#endif
#endif
}

#ifdef PORT
static char sSYNetRollbackDescribeBuf[96];

static sb32 syNetRollbackDescribeIn(const u8 *addr, const void *base, size_t size, const char *what, s32 link)
{
	if ((base != NULL) && (addr >= (const u8*)base) && (addr < (const u8*)base + size))
	{
		snprintf(sSYNetRollbackDescribeBuf, sizeof(sSYNetRollbackDescribeBuf), "%s[link%d]+0x%X",
		         what, link, (unsigned)(addr - (const u8*)base));
		return TRUE;
	}
	return FALSE;
}

static sb32 syNetRollbackDescribeDObj(const u8 *addr, DObj *dobj, s32 link, s32 depth)
{
	for (; (dobj != NULL) && (depth < 64); dobj = dobj->sib_next)
	{
		AObj *aobj;

		if (syNetRollbackDescribeIn(addr, dobj, sizeof(DObj), "DObj", link) != FALSE)
		{
			return TRUE;
		}
		for (aobj = dobj->aobj; aobj != NULL; aobj = aobj->next)
		{
			if (syNetRollbackDescribeIn(addr, aobj, sizeof(AObj), "AObj", link) != FALSE)
			{
				return TRUE;
			}
		}
		if (syNetRollbackDescribeDObj(addr, dobj->child, link, depth + 1) != FALSE)
		{
			return TRUE;
		}
	}
	return FALSE;
}

/* Names the live game object an arena byte belongs to (GObj, its user struct,
 * or a DObj/AObj in its tree). Returns NULL when nothing live owns it. */
static const char *syNetRollbackDescribeArena(const u8 *addr)
{
	s32 link;

	for (link = 0; link < GC_COMMON_MAX_LINKS; link++)
	{
		GObj *gobj;

		for (gobj = gGCCommonLinks[link]; gobj != NULL; gobj = gobj->link_next)
		{
			size_t user_size = 0;
			const char *user_name = NULL;

			if (syNetRollbackDescribeIn(addr, gobj, sizeof(GObj), "GObj", link) != FALSE)
			{
				return sSYNetRollbackDescribeBuf;
			}
			switch (link)
			{
			case nGCCommonLinkIDFighter: user_size = sizeof(FTStruct); user_name = "FTStruct"; break;
			case nGCCommonLinkIDItem: user_size = sizeof(ITStruct); user_name = "ITStruct"; break;
			case nGCCommonLinkIDWeapon: user_size = sizeof(WPStruct); user_name = "WPStruct"; break;
			case nGCCommonLinkIDEffect:
			case nGCCommonLinkIDSpecialEffect: user_size = sizeof(EFStruct); user_name = "EFStruct"; break;
			default: break;
			}
			if ((user_name != NULL) && (syNetRollbackDescribeIn(addr, gobj->user_data.p, user_size, user_name, link) != FALSE))
			{
				return sSYNetRollbackDescribeBuf;
			}
			if ((gobj->obj_kind == 1) && (syNetRollbackDescribeDObj(addr, (DObj*)gobj->obj, link, 0) != FALSE))
			{
				return sSYNetRollbackDescribeBuf;
			}
		}
	}
	return NULL;
}

static void syNetRollbackReportDiff(const char *region, size_t region_offset, const u8 *live, const u8 *saved, size_t size)
{
	size_t i;

	for (i = 0; (i < size) && (sSYNetRollbackDiffsReported < SYNETROLLBACK_DIFF_REPORT_MAX); i++)
	{
		if (live[i] != saved[i])
		{
			u32 sym_offset = 0;
			const char *sym = (region[0] == 'a') ? syNetRollbackDescribeArena(&live[i]) : port_symbolize(&live[i], &sym_offset);
			s32 range_id;

			for (range_id = 0; range_id < sSYNetRollbackFileRangesNum; range_id++)
			{
				if ((&live[i] >= sSYNetRollbackFileRanges[range_id].lo) && (&live[i] < sSYNetRollbackFileRanges[range_id].hi))
				{
					break;
				}
			}
			port_log("SSB64 Rollback:   tick=%u %s+0x%X %s+0x%X%s pass1=%02X pass2=%02X\n",
			         sSYNetRollbackTestedTicks, region, (unsigned)(region_offset + i), (sym != NULL) ? sym : "?", sym_offset,
			         (range_id < sSYNetRollbackFileRangesNum) ? " (file)" : "", saved[i], live[i]);
			sSYNetRollbackDiffsReported++;

			/* Skip the rest of this word/struct run so one field is one line. */
			while ((i + 1 < size) && (live[i + 1] != saved[i + 1]))
			{
				i++;
			}
		}
	}
}

/* Byte-compare the live state (second pass) against the first pass's
 * post-update state to name the exact globals/arena bytes that diverged. */
static void syNetRollbackReportMemoryDiffs(void)
{
#if SYNETROLLBACK_HAS_SECTIONS
	SYNetRollbackState *first = &sSYNetRollbackStates[SYNETROLLBACK_SLOT_COMPARE];
	size_t arena_size = (size_t)((u8*)gSYTaskmanGeneralHeap.ptr - sSYNetRollbackArenaWatermark);
	size_t cursor = 0;
	s32 i;

	if (first->is_valid == FALSE)
	{
		return;
	}
	if ((arena_size != first->arena_size) && (sSYNetRollbackDiffsReported < SYNETROLLBACK_DIFF_REPORT_MAX))
	{
		sSYNetRollbackDiffsReported++;
		port_log("SSB64 Rollback:   arena bump differs pass1=%u pass2=%u\n",
		         (unsigned)first->arena_size, (unsigned)arena_size);
	}
	syNetRollbackReportDiff("data", 0, &gSYNetRollbackDataStart, first->data, syNetRollbackGetDataSize());
	syNetRollbackReportDiff("bss", 0, &gSYNetRollbackBssStart, first->bss, syNetRollbackGetBssSize());

	/* Same spans as syNetRollbackCopyArena: file ranges are not in the image. */
	if (arena_size > first->arena_size)
	{
		arena_size = first->arena_size;
	}
	for (i = 0; (i <= sSYNetRollbackFileRangesNum) && (cursor < arena_size); i++)
	{
		size_t lo = arena_size;
		size_t hi = arena_size;

		if (i < sSYNetRollbackFileRangesNum)
		{
			lo = (size_t)(sSYNetRollbackFileRanges[i].lo - sSYNetRollbackArenaWatermark);
			hi = (size_t)(sSYNetRollbackFileRanges[i].hi - sSYNetRollbackArenaWatermark);

			if (lo > arena_size)
			{
				lo = arena_size;
			}
		}
		if (lo > cursor)
		{
			syNetRollbackReportDiff("arena", cursor, sSYNetRollbackArenaWatermark + cursor, first->arena + cursor, lo - cursor);
		}
		if (hi > cursor)
		{
			cursor = hi;
		}
	}
#endif
}
#endif

/* SyncTest: save, run the tick, hash; restore, run the same tick again, hash.
 * Any divergence means the snapshot misses state (or the tick is not a pure
 * function of snapshot + published input). Inputs for the tick are already
 * published into gSYControllerDevices, which lives outside the snapshot. */
#ifdef PORT
static void syNetRollbackTraceDObjs(DObj *dobj, s32 depth, const char *phase)
{
	for (; (dobj != NULL) && (depth < 64); dobj = dobj->sib_next)
	{
		if (dobj->anim_joint.event16 != NULL)
		{
			u16 *ev = (u16*)dobj->anim_joint.event16;

			port_log("SSB64 RollbackTrace: %s dobj=+0x%X ev=%p wait=%f speed=%f frame=%f words=%04X %04X %04X %04X\n",
			         phase, (unsigned)((u8*)dobj - sSYNetRollbackArenaWatermark), (void*)ev,
			         dobj->anim_wait, dobj->anim_speed, dobj->anim_frame, ev[0], ev[1], ev[2], ev[3]);
		}
		syNetRollbackTraceDObjs(dobj->child, depth + 1, phase);
	}
}

/* SSB64_ROLLBACK_TRACE_TICK=N: dump every fighter joint's animation cursor
 * around SyncTest tick N, to localize a pass1/pass2 divergence. */
static void syNetRollbackTraceTick(const char *phase)
{
	GObj *gobj;

	if ((sSYNetRollbackTraceTick == 0) || (sSYNetRollbackTestedTicks + 1 != sSYNetRollbackTraceTick))
	{
		return;
	}
	for (gobj = gGCCommonLinks[nGCCommonLinkIDFighter]; gobj != NULL; gobj = gobj->link_next)
	{
		if (gobj->obj_kind == 1)
		{
			syNetRollbackTraceDObjs((DObj*)gobj->obj, 0, phase);
		}
	}
}
#else
#define syNetRollbackTraceTick(phase)
#endif

void syNetRollbackRunBattleUpdate(void (*update)(void))
{
	SYNetSyncTickHash first;
	SYNetSyncTickHash second;
	u32 diff_mask;
	s32 column;

	if (sSYNetRollbackIsSyncTest == FALSE)
	{
		update();
		return;
	}
	if (syNetRollbackSave(0) == FALSE)
	{
		sSYNetRollbackSkippedTicks++;
		update();
		return;
	}
	syNetRollbackTraceTick("pre1");
	update();
	syNetRollbackTraceTick("post1");
	syNetSyncHashTick(&first);
	syNetRollbackSaveState(&sSYNetRollbackStates[SYNETROLLBACK_SLOT_COMPARE]);

	syNetRollbackLoad(0);
	syNetRollbackTraceTick("pre2");

	sSYNetRollbackIsResimulating = TRUE;
	update();
	sSYNetRollbackIsResimulating = FALSE;
	syNetRollbackTraceTick("post2");

	syNetSyncHashTick(&second);
	sSYNetRollbackTestedTicks++;

	diff_mask = 0;

	for (column = 0; column < SYNETSYNC_COLUMN_NUM; column++)
	{
		if (first.column[column] != second.column[column])
		{
			diff_mask |= (1U << column);
		}
	}
	if (diff_mask & SYNETSYNC_GATED_COLUMN_MASK)
	{
		sSYNetRollbackFailedTicks++;
	}
#ifdef PORT
	if (diff_mask & ~sSYNetRollbackColumnReported)
	{
		port_log("SSB64 Rollback: SyncTest divergence tested_tick=%u game_tic=%u mask=0x%X\n",
		         sSYNetRollbackTestedTicks, (unsigned)gSCManagerBattleState->time_passed, diff_mask);

		for (column = 0; column < SYNETSYNC_COLUMN_NUM; column++)
		{
			if ((diff_mask & ~sSYNetRollbackColumnReported) & (1U << column))
			{
				port_log("SSB64 Rollback:   column %s pass1=%08X pass2=%08X\n",
				         syNetSyncGetColumnName(column), first.column[column], second.column[column]);
			}
		}
		sSYNetRollbackColumnReported |= diff_mask;
	}
	/* The hashes cover a subset of state; the raw compare catches the rest. */
	if (sSYNetRollbackDiffsReported < SYNETROLLBACK_DIFF_REPORT_MAX)
	{
		syNetRollbackReportMemoryDiffs();
	}
#endif
}
