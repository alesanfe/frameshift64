#include <sys/netreplay.h>
#include <sys/netsync.h>

#include <ft/fighter.h>
#include <if/ifcommon.h>
#include <sc/scmanager.h>
#include <sys/netinput.h>
#include <sys/utils.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef PORT
#include "port_log.h"
extern void port_exit_process(int code);
extern char *getenv(const char *name);
extern int atoi(const char *s);
extern const char *port_save_get_dir(void);
extern int port_mkdir_p(const char *path);
#endif

#define SYNETREPLAY_DEFAULT_RECORD_FRAMES 1800

typedef struct SYNetReplayFileHeader
{
	u32 magic;
	u32 version;
	u32 metadata_size;
	u32 frame_size;
	u32 frame_count;
	u32 player_count;
	u32 input_checksum;

} SYNetReplayFileHeader;

const char *sSYNetReplayRecordPath;
const char *sSYNetReplayPlayPath;
/* Online matches record automatically (netpeer arms this): the replay lands
 * in <save dir>/replays/ at match end unless SSB64_REPLAY_RECORD already
 * named an explicit destination. */
sb32 sSYNetReplayAutoSave;
sb32 sSYNetReplayIsAutoRecording;

char *syNetReplayMakeAutoPath(void);  /* malloc'd — caller frees */
u32 sSYNetReplayRecordFrameLimit = SYNETREPLAY_DEFAULT_RECORD_FRAMES;
u32 sSYNetReplayLoadedFrameCount;
u32 sSYNetReplayLoadedInputChecksum;
sb32 sSYNetReplayIsRecording;
sb32 sSYNetReplayIsRecordWritten;
sb32 sSYNetReplayIsPlaybackLoaded;
sb32 sSYNetReplayIsPlaybackActive;
sb32 sSYNetReplayIsPlaybackVerified;
/* SSB64_RIG_EXIT=1: turn the playback verify verdict into the process exit
 * code (0 PASS, 1 FAIL, 2 playback ended before the replay did). */
sb32 sSYNetReplayRigExit;
/* SSB64_REPLAY_TAKEOVER=<player>:<tick>: replay runs the recorded inputs
 * until <tick>, then that slot's source flips to the live controller - the
 * classic "train from this exact moment" workflow. The other slots keep
 * replaying their recorded inputs. Verification is disabled once the
 * takeover fires: diverging from the recording is the point. */
s32 sSYNetReplayTakeoverPlayer = -1;
u32 sSYNetReplayTakeoverTick;
SYNetInputReplayMetadata sSYNetReplayLoadedMetadata;
SYNetInputFrame sSYNetReplayLoadedFrames[MAXCONTROLLERS][SYNETINPUT_REPLAY_MAX_FRAMES];

void syNetReplayClearLoadedFrames(void)
{
	s32 player;
	s32 tick;

	sSYNetReplayLoadedFrameCount = 0;
	sSYNetReplayLoadedInputChecksum = 0;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		for (tick = 0; tick < SYNETINPUT_REPLAY_MAX_FRAMES; tick++)
		{
			memset(&sSYNetReplayLoadedFrames[player][tick], 0, sizeof(SYNetInputFrame));
		}
	}
}

void syNetReplayCaptureBattleMetadata(SCBattleState *battle_state, SYNetInputReplayMetadata *metadata)
{
	s32 player;

	memset(metadata, 0, sizeof(SYNetInputReplayMetadata));

	metadata->magic = SYNETINPUT_REPLAY_MAGIC;
	metadata->version = SYNETINPUT_REPLAY_VERSION;
	metadata->scene_kind = nSCKindVSBattle;
	metadata->rng_seed = syUtilsRandSeed();

	if (battle_state == NULL)
	{
		return;
	}
	metadata->player_count = battle_state->pl_count + battle_state->cp_count;
	metadata->stage_kind = battle_state->gkind;
	metadata->stocks = battle_state->stocks;
	metadata->time_limit = battle_state->time_limit;
	metadata->item_switch = battle_state->item_appearance_rate;
	metadata->item_toggles = battle_state->item_toggles;
	metadata->game_type = battle_state->game_type;
	metadata->game_rules = battle_state->game_rules;
	metadata->is_team_battle = battle_state->is_team_battle;
	metadata->handicap = battle_state->handicap;
	metadata->is_team_attack = battle_state->is_team_attack;
	metadata->is_stage_select = battle_state->is_stage_select;
	metadata->damage_ratio = battle_state->damage_ratio;
	metadata->item_appearance_rate = battle_state->item_appearance_rate;
	metadata->is_not_teamshadows = battle_state->is_not_teamshadows;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		metadata->player_kinds[player] = battle_state->players[player].pkind;
		metadata->fighter_kinds[player] = battle_state->players[player].fkind;
		metadata->costumes[player] = battle_state->players[player].costume;
		metadata->teams[player] = battle_state->players[player].team;
		metadata->handicaps[player] = battle_state->players[player].handicap;
		metadata->levels[player] = battle_state->players[player].level;
		metadata->shades[player] = battle_state->players[player].shade;
	}
}

void syNetReplayApplyBattleMetadata(const SYNetInputReplayMetadata *metadata)
{
	SCBattleState *battle_state = &gSCManagerTransferBattleState;
	s32 player;

	*battle_state = dSCManagerDefaultBattleState;
	battle_state->game_type = metadata->game_type;
	battle_state->gkind = metadata->stage_kind;
	battle_state->is_team_battle = metadata->is_team_battle;
	battle_state->game_rules = metadata->game_rules;
	battle_state->time_limit = metadata->time_limit;
	battle_state->stocks = metadata->stocks;
	battle_state->handicap = metadata->handicap;
	battle_state->is_team_attack = metadata->is_team_attack;
	battle_state->is_stage_select = FALSE;
	battle_state->damage_ratio = metadata->damage_ratio;
	battle_state->item_toggles = metadata->item_toggles;
	battle_state->item_appearance_rate = metadata->item_appearance_rate;
	battle_state->is_not_teamshadows = metadata->is_not_teamshadows;
	battle_state->pl_count = 0;
	battle_state->cp_count = 0;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		u8 handicap = metadata->handicaps[player];

		/* A replay file is untrusted input: the per-player handicap indexes
		 * dFTCommonDataHandicapTable[handicap - 1] in ftParamGetCommonKnockback,
		 * so 0 (a zero-filled or corrupt file) reads one row BEFORE the table —
		 * caught by ASan as a global-buffer-overflow during hit processing. The
		 * game's own paths keep it in 1..9 (CSS default 9 = neutral); clamp to
		 * the same domain here. */
		if ((handicap < 1) || (handicap > 9))
		{
			handicap = 9;
		}

		battle_state->players[player].player = (metadata->is_team_battle != FALSE) ? metadata->teams[player] : player;
		battle_state->players[player].team = metadata->teams[player];
		battle_state->players[player].pkind = metadata->player_kinds[player];
		battle_state->players[player].fkind = metadata->fighter_kinds[player];
		battle_state->players[player].costume = metadata->costumes[player];
		battle_state->players[player].shade = metadata->shades[player];
		battle_state->players[player].handicap = handicap;
		battle_state->players[player].level = metadata->levels[player];
		battle_state->players[player].tag = (metadata->player_kinds[player] == nFTPlayerKindMan) ? player : GMCOMMON_PLAYERS_MAX;
		battle_state->players[player].is_single_stockicon = (metadata->game_rules & SCBATTLE_GAMERULE_TIME) ? TRUE : FALSE;

		if (metadata->player_kinds[player] == nFTPlayerKindMan)
		{
			battle_state->players[player].color =
			(metadata->is_team_battle == FALSE) ? player : dIFCommonPlayerTeamColorIDs[metadata->teams[player]];
			battle_state->pl_count++;
		}
		else if (metadata->player_kinds[player] == nFTPlayerKindCom)
		{
			battle_state->players[player].color =
			(metadata->is_team_battle == FALSE) ? GMCOMMON_PLAYERS_MAX : dIFCommonPlayerTeamColorIDs[metadata->teams[player]];
			battle_state->cp_count++;
		}
	}
	gSCManagerSceneData.gkind = metadata->stage_kind;
}

void syNetReplayInitDebugEnv(void)
{
	/* Replay record/play/takeover are a test harness: they boot straight
	 * into scripted matches and can disable verification. Compiled in only
	 * for test builds (-DSSB64_TEST_HOOKS=ON); never shipped. */
#if defined(PORT) && defined(SSB64_TEST_HOOKS)
	const char *frame_limit_env;

	sSYNetReplayRecordPath = getenv("SSB64_REPLAY_RECORD");
	sSYNetReplayPlayPath = getenv("SSB64_REPLAY_PLAY");
	frame_limit_env = getenv("SSB64_REPLAY_RECORD_FRAMES");
	{
		const char *rig_exit_env = getenv("SSB64_RIG_EXIT");

		sSYNetReplayRigExit = ((rig_exit_env != NULL) && (atoi(rig_exit_env) != 0)) ? TRUE : FALSE;
	}

	if (frame_limit_env != NULL)
	{
		s32 frame_limit = atoi(frame_limit_env);

		if ((frame_limit > 0) && (frame_limit < SYNETINPUT_REPLAY_MAX_FRAMES))
		{
			sSYNetReplayRecordFrameLimit = frame_limit;
		}
	}
	{
		const char *takeover_env = getenv("SSB64_REPLAY_TAKEOVER");
		int player = -1;
		unsigned int tick = 0;

		if ((takeover_env != NULL) && (sscanf(takeover_env, "%d:%u", &player, &tick) == 2) &&
			(player >= 0) && (player < MAXCONTROLLERS))
		{
			sSYNetReplayTakeoverPlayer = player;
			sSYNetReplayTakeoverTick = tick;
			port_log("SSB64 Replay: takeover armed player=%d tick=%u\n", player, tick);
		}
	}
	if (sSYNetReplayPlayPath != NULL)
	{
		if (syNetReplayLoadDebugFile(sSYNetReplayPlayPath) != FALSE)
		{
			syNetReplayApplyBattleMetadata(&sSYNetReplayLoadedMetadata);
			syUtilsSetRandomSeed(sSYNetReplayLoadedMetadata.rng_seed);
			gSCManagerSceneData.scene_prev = nSCKindVSMode;
			gSCManagerSceneData.scene_curr = nSCKindVSBattle;
		}
		else
		{
			/* Nothing will arm playback now, so no verdict can ever be produced;
			 * without this a batch run would sit at the title screen forever. */
			port_log("SSB64 Replay: playback load failed path=%s result=LOADFAIL\n", sSYNetReplayPlayPath);

			if (sSYNetReplayRigExit != FALSE)
			{
				port_log("SSB64 Replay: SSB64_RIG_EXIT set, exiting with code %d\n", 3);
				syNetSyncFinishVSSession();
				port_exit_process(3);
			}
		}
	}
#endif
}

void syNetReplayArmAutoSave(void)
{
	sSYNetReplayAutoSave = TRUE;
}

void syNetReplayStartVSSession(SCBattleState *battle_state)
{
	SYNetInputReplayMetadata metadata;
	u32 tick;
	s32 player;

	if (sSYNetReplayIsPlaybackLoaded != FALSE)
	{
		syNetInputClearReplayFrames();
		syNetInputSetReplayMetadata(&sSYNetReplayLoadedMetadata);
		syUtilsSetRandomSeed(sSYNetReplayLoadedMetadata.rng_seed);

		for (tick = 0; tick < sSYNetReplayLoadedFrameCount; tick++)
		{
			for (player = 0; player < MAXCONTROLLERS; player++)
			{
				syNetInputSetReplayFrame(player, tick, &sSYNetReplayLoadedFrames[player][tick]);
			}
		}
		for (player = 0; player < MAXCONTROLLERS; player++)
		{
			syNetInputSetSlotSource(player, nSYNetInputSourceSaved);
		}
		sSYNetReplayIsPlaybackActive = TRUE;
		sSYNetReplayIsPlaybackVerified = FALSE;
		syNetInputSetPublishedChecksumLimit(sSYNetReplayLoadedFrameCount);

#ifdef PORT
		port_log("SSB64 Replay: playback start path=%s frames=%u checksum=0x%08X stage=%u seed=%u\n",
		         sSYNetReplayPlayPath, sSYNetReplayLoadedFrameCount, sSYNetReplayLoadedInputChecksum,
		         sSYNetReplayLoadedMetadata.stage_kind, sSYNetReplayLoadedMetadata.rng_seed);
#endif
		return;
	}
	if ((sSYNetReplayRecordPath != NULL) || (sSYNetReplayAutoSave != FALSE))
	{
		syNetReplayCaptureBattleMetadata(battle_state, &metadata);
		syNetInputClearReplayFrames();
		syNetInputSetReplayMetadata(&metadata);
		syNetInputSetRecordingEnabled(TRUE);
		sSYNetReplayIsRecording = TRUE;
		sSYNetReplayIsRecordWritten = FALSE;
		sSYNetReplayIsAutoRecording = (sSYNetReplayRecordPath == NULL) ? TRUE : FALSE;

#ifdef PORT
		port_log("SSB64 Replay: recording start path=%s limit=%u stage=%u seed=%u players=%u\n",
		         (sSYNetReplayRecordPath != NULL) ? sSYNetReplayRecordPath : "<auto>",
		         sSYNetReplayRecordFrameLimit, metadata.stage_kind,
		         metadata.rng_seed, metadata.player_count);
#endif
	}
}

void syNetReplayUpdate(void)
{
#ifdef PORT
	/* syNetReplayUpdate runs after the tick's inputs were resolved, so when
	 * N ticks are published the next simulated tick is N itself - switching
	 * the slot's source here hands control over exactly at takeover_tick. */
	if ((sSYNetReplayTakeoverPlayer >= 0) && (sSYNetReplayIsPlaybackActive != FALSE) &&
		(syNetInputGetPublishedTickCount() >= sSYNetReplayTakeoverTick))
	{
		port_log("SSB64 Replay: takeover player=%d at tick=%u - slot is live input now\n",
		         sSYNetReplayTakeoverPlayer, sSYNetReplayTakeoverTick);
		syNetInputSetSlotSource(sSYNetReplayTakeoverPlayer, nSYNetInputSourceLocal);
		sSYNetReplayTakeoverPlayer = -1;
		/* the input stream intentionally diverges from here - the end-of-
		 * replay checksum verdict would be a false FAIL */
		sSYNetReplayIsPlaybackVerified = TRUE;
	}
#endif
	if ((sSYNetReplayIsRecording != FALSE) && (sSYNetReplayIsRecordWritten == FALSE) &&
		(syNetInputGetRecordedFrameCount() >= sSYNetReplayRecordFrameLimit))
	{
		syNetReplayFinishVSSession();
	}
	if ((sSYNetReplayIsPlaybackActive != FALSE) && (sSYNetReplayIsPlaybackVerified == FALSE) &&
		(syNetInputGetPublishedTickCount() >= sSYNetReplayLoadedFrameCount))
	{
		/* netinput froze the published-input checksum at exactly frame_count
		 * advanced ticks (see syNetInputSetPublishedChecksumLimit), so this
		 * compares the same tick range the recorder hashed. */
		u32 checksum = syNetInputGetPublishedInputChecksum();
		sb32 is_pass = (checksum == sSYNetReplayLoadedInputChecksum) ? TRUE : FALSE;

#ifdef PORT
		port_log("SSB64 Replay: playback verify frames=%u expected=0x%08X actual=0x%08X result=%s\n",
		         sSYNetReplayLoadedFrameCount, sSYNetReplayLoadedInputChecksum, checksum,
		         (is_pass != FALSE) ? "PASS" : "FAIL");
#endif
		sSYNetReplayIsPlaybackVerified = TRUE;
		sSYNetReplayIsPlaybackActive = FALSE;

#if defined(PORT) && defined(SSB64_TEST_HOOKS)
		if (sSYNetReplayRigExit != FALSE)
		{
			/* Batch/rig mode: report through the exit code. port_exit_process()
			 * terminates immediately - normal teardown from the game coroutine
			 * is not safe (render/audio threads are mid-frame). Inputs that
			 * replayed identically but a gameplay state trace that diverged
			 * (SSB64_SYNC_VERIFY) is its own code: the sim is nondeterministic. */
			s32 exit_code = (is_pass != FALSE) ? 0 : 1;

			if (is_pass != FALSE)
			{
				switch (syNetSyncGetVerifyResult())
				{
				case nSYNetSyncVerifyNotLoaded:
					/* an absent oracle must never read as PASS */
					port_log("SSB64 Replay: inputs PASS but the state trace to verify against did not load result=LOADFAIL\n");
					exit_code = 3;
					break;
				case nSYNetSyncVerifyDiverged:
					port_log("SSB64 Replay: inputs PASS but state trace diverged result=DESYNC\n");
					exit_code = 4;
					break;
				case nSYNetSyncVerifyShort:
					port_log("SSB64 Replay: inputs PASS but the state trace ended before the replay did result=DESYNC\n");
					exit_code = 4;
					break;
				default:
					break;
				}
			}
			port_log("SSB64 Replay: SSB64_RIG_EXIT set, exiting with code %d\n", exit_code);
			syNetSyncFinishVSSession();
			port_exit_process(exit_code);
		}
#endif
	}
}

void syNetReplayFinishVSSession(void)
{
	if ((sSYNetReplayIsPlaybackActive != FALSE) && (sSYNetReplayIsPlaybackVerified == FALSE))
	{
		/* The match ended before the replay stream did (stocks/time ran out
		 * earlier than the recording expected), so there is nothing to verify
		 * against - report it rather than leaving a batch run waiting. */
#ifdef PORT
		port_log("SSB64 Replay: playback ended early ticks=%u of %u result=INCOMPLETE\n",
		         syNetInputGetPublishedTickCount(), sSYNetReplayLoadedFrameCount);
#endif
		sSYNetReplayIsPlaybackVerified = TRUE;
		sSYNetReplayIsPlaybackActive = FALSE;

#if defined(PORT) && defined(SSB64_TEST_HOOKS)
		if (sSYNetReplayRigExit != FALSE)
		{
			/* Batch/rig mode: report through the exit code. port_exit_process()
			 * terminates immediately - normal teardown from the game coroutine
			 * is not safe (render/audio threads are mid-frame). */
			/* the input stream could not be verified, but a state trace that
			 * already diverged is the stronger verdict and must not be masked */
			s32 exit_code = (syNetSyncGetVerifyResult() == nSYNetSyncVerifyDiverged) ? 4 : 2;

			if (exit_code == 4)
			{
				port_log("SSB64 Replay: match ended early and the state trace diverged result=DESYNC\n");
			}
			port_log("SSB64 Replay: SSB64_RIG_EXIT set, exiting with code %d\n", exit_code);
			syNetSyncFinishVSSession();
			port_exit_process(exit_code);
		}
#endif
	}
	if ((sSYNetReplayIsRecording != FALSE) && (sSYNetReplayIsRecordWritten == FALSE))
	{
		const char *target = sSYNetReplayRecordPath;
		char *auto_path = NULL;

		if ((target == NULL) && (sSYNetReplayIsAutoRecording != FALSE))
		{
			auto_path = syNetReplayMakeAutoPath();
			target = auto_path;
		}
		if (target != NULL)
		{
			syNetReplayWriteDebugFile(target);
		}
		free(auto_path);
		syNetInputSetRecordingEnabled(FALSE);
		sSYNetReplayIsRecording = FALSE;
		sSYNetReplayIsRecordWritten = TRUE;
	}
}

/* <save dir>/replays/<YYYYMMDD>-<HHMMSS>-<seed hex>.ssr — created under the
 * same directory convention as ssb64_save.bin. */
char *syNetReplayMakeAutoPath(void)
{
#ifdef PORT
	SYNetInputReplayMetadata metadata;
	const char *save_dir;
	char *path;
	size_t path_len;
	time_t now = time(NULL);
	struct tm *tm_info = localtime(&now);
	char stamp[32];

	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tm_info);
	save_dir = port_save_get_dir();
	/* Heap-size everything: a deep save dir (long SHIP_HOME, unicode
	 * usernames) must not silently truncate the replay dump. */
	path_len = strlen(save_dir) + sizeof("/replays/replay-") + sizeof(stamp)
	           + sizeof("-00000000.ssr") + 8;
	path = (char *)malloc(path_len);
	if (path == NULL)
	{
		port_log("SSB64 Replay: out of memory building auto path\n");
		return NULL;
	}
	snprintf(path, path_len, "%s/replays", save_dir);
	port_mkdir_p(path);
	syNetInputGetReplayMetadata(&metadata);
	snprintf(path, path_len, "%s/replays/replay-%s-%08X.ssr", save_dir,
	         stamp, metadata.rng_seed);
	return path;
#else
	return NULL;
#endif
}

/* A desync leaves each peer's simulation diverged: dump everything needed to
 * reproduce it locally into <save dir>/desync/<stamp>-<seed>/ — the recorded
 * input replay plus a human-readable info file with the diverging tick and
 * both hashes. Pair two players' packages to diff determinism bugs. */
void syNetReplayWriteDesyncPackage(u32 tick, u32 local_hash, u32 remote_hash)
{
#ifdef PORT
	SYNetInputReplayMetadata metadata;
	const char *save_dir;
	char *dir, *path;
	size_t dir_len;
	time_t now = time(NULL);
	struct tm *tm_info = localtime(&now);
	char stamp[32];
	FILE *fp;
	s32 player;

	memset(&metadata, 0, sizeof(metadata));
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tm_info);
	save_dir = port_save_get_dir();
	/* Heap-size the paths: deep save dirs must not silently drop the
	 * desync package. */
	dir_len = strlen(save_dir) + sizeof("/desync/") + sizeof(stamp) + sizeof("-00000000") + 8;
	dir = (char *)malloc(dir_len);
	path = (char *)malloc(dir_len + sizeof("/replay.ssr"));
	if ((dir == NULL) || (path == NULL))
	{
		port_log("SSB64 Replay: out of memory building desync path\n");
		free(dir);
		free(path);
		return;
	}
	snprintf(dir, dir_len, "%s/desync/%s-%08X", save_dir, stamp,
	         (syNetInputGetReplayMetadata(&metadata) != FALSE) ? metadata.rng_seed : 0);
	if (port_mkdir_p(dir) != 0)
	{
		free(dir);
		free(path);
		return;
	}
	snprintf(path, dir_len + sizeof("/replay.ssr"), "%s/replay.ssr", dir);
	syNetReplayWriteDebugFile(path);
	snprintf(path, dir_len + sizeof("/replay.ssr"), "%s/info.txt", dir);
	fp = fopen(path, "w");
	if (fp != NULL)
	{
		fprintf(fp, "desync_tick=%u\nlocal_hash=%08X\nremote_hash=%08X\n",
		        tick, local_hash, remote_hash);
		fprintf(fp, "stage=%u seed=%u players=%u stocks=%u items=%u\n",
		        metadata.stage_kind, metadata.rng_seed, metadata.player_count,
		        metadata.stocks, metadata.item_switch);
		for (player = 0; player < MAXCONTROLLERS; player++)
		{
			fprintf(fp, "p%d fkind=%u costume=%u\n", player,
			        metadata.fighter_kinds[player], metadata.costumes[player]);
		}
		fclose(fp);
	}
	port_log("SSB64 Replay: desync package written to %s\n", dir);
	free(dir);
	free(path);
#else
	(void)tick; (void)local_hash; (void)remote_hash;
#endif
}

sb32 syNetReplayWriteDebugFile(const char *path)
{
	SYNetReplayFileHeader header;
	SYNetInputReplayMetadata metadata;
	SYNetInputFrame frame;
	FILE *fp;
	u32 tick;
	s32 player;

	if ((path == NULL) || (syNetInputGetReplayMetadata(&metadata) == FALSE))
	{
		return FALSE;
	}
	fp = fopen(path, "wb");

	if (fp == NULL)
	{
#ifdef PORT
		port_log("SSB64 Replay: failed to open record path=%s\n", path);
#endif
		return FALSE;
	}
	header.magic = SYNETINPUT_REPLAY_MAGIC;
	header.version = SYNETINPUT_REPLAY_VERSION;
	header.metadata_size = sizeof(SYNetInputReplayMetadata);
	header.frame_size = sizeof(SYNetInputFrame);
	header.frame_count = syNetInputGetRecordedFrameCount();
	header.player_count = MAXCONTROLLERS;
	header.input_checksum = syNetInputGetReplayInputChecksum();

	fwrite(&header, sizeof(header), 1, fp);
	fwrite(&metadata, sizeof(metadata), 1, fp);

	for (tick = 0; tick < header.frame_count; tick++)
	{
		for (player = 0; player < MAXCONTROLLERS; player++)
		{
			if (syNetInputGetReplayFrame(player, tick, &frame) == FALSE)
			{
				memset(&frame, 0, sizeof(frame));
				frame.tick = tick;
			}
			fwrite(&frame, sizeof(frame), 1, fp);
		}
	}
	fclose(fp);

#ifdef PORT
	port_log("SSB64 Replay: wrote path=%s frames=%u checksum=0x%08X\n",
	         path, header.frame_count, header.input_checksum);
#endif
	return TRUE;
}

sb32 syNetReplayLoadDebugFile(const char *path)
{
	SYNetReplayFileHeader header;
	FILE *fp;
	u32 tick;
	s32 player;

	if (path == NULL)
	{
		return FALSE;
	}
	fp = fopen(path, "rb");

	if (fp == NULL)
	{
#ifdef PORT
		port_log("SSB64 Replay: failed to open playback path=%s\n", path);
#endif
		return FALSE;
	}
	if (fread(&header, sizeof(header), 1, fp) != 1)
	{
		fclose(fp);
		return FALSE;
	}
	if ((header.magic != SYNETINPUT_REPLAY_MAGIC) ||
		(header.version != SYNETINPUT_REPLAY_VERSION) ||
		(header.metadata_size != sizeof(SYNetInputReplayMetadata)) ||
		(header.frame_size != sizeof(SYNetInputFrame)) ||
		(header.frame_count == 0) || (header.frame_count > SYNETINPUT_REPLAY_MAX_FRAMES) ||
		(header.player_count != MAXCONTROLLERS))
	{
#ifdef PORT
		port_log("SSB64 Replay: rejected playback header path=%s magic=0x%08X version=%u frames=%u players=%u\n",
		         path, header.magic, header.version, header.frame_count, header.player_count);
#endif
		fclose(fp);
		return FALSE;
	}
	if (fread(&sSYNetReplayLoadedMetadata, sizeof(sSYNetReplayLoadedMetadata), 1, fp) != 1)
	{
		fclose(fp);
		return FALSE;
	}
	syNetReplayClearLoadedFrames();
	sSYNetReplayLoadedFrameCount = header.frame_count;
	sSYNetReplayLoadedInputChecksum = header.input_checksum;

	for (tick = 0; tick < header.frame_count; tick++)
	{
		for (player = 0; player < MAXCONTROLLERS; player++)
		{
			if (fread(&sSYNetReplayLoadedFrames[player][tick], sizeof(SYNetInputFrame), 1, fp) != 1)
			{
				fclose(fp);
				return FALSE;
			}
		}
	}
	fclose(fp);
	sSYNetReplayIsPlaybackLoaded = TRUE;

#ifdef PORT
	port_log("SSB64 Replay: loaded path=%s frames=%u checksum=0x%08X stage=%u seed=%u\n",
	         path, sSYNetReplayLoadedFrameCount, sSYNetReplayLoadedInputChecksum,
	         sSYNetReplayLoadedMetadata.stage_kind, sSYNetReplayLoadedMetadata.rng_seed);
#endif
	return TRUE;
}

#ifdef PORT
/* UI replay playback (Play -> Replays): loads the .ssr, applies the recorded
 * battle metadata the same way the boot-time SSB64_REPLAY_PLAY path does,
 * then hands off to the VS battle scene. syNetReplayStartVSSession arms the
 * saved-input source when the battle scene initializes. */
sb32 portNetReplayPlayFile(const char *path)
{
	extern void portSCManagerRequestScene(s32 target);

	if (syNetReplayLoadDebugFile(path) == FALSE)
	{
		return FALSE;
	}
	syNetReplayApplyBattleMetadata(&sSYNetReplayLoadedMetadata);
	syUtilsSetRandomSeed(sSYNetReplayLoadedMetadata.rng_seed);
	portSCManagerRequestScene(nSCKindVSBattle);
	return TRUE;
}
#endif
