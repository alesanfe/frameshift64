/**
 * port_diag_stubs.c — tiny accessors that let port/ layer code read game
 * state (currently the active scene) without pulling the entire sc/ header
 * chain into port/*.cpp files. Compiled as part of the decomp target so it
 * has full visibility of SCCommonData and the nSCKind* enum.
 */

#include <ssb_types.h>
#include <sc/scene.h>
#include <sc/scmanager.h>

u8 port_diag_get_scene_curr(void)
{
	return gSCManagerSceneData.scene_curr;
}

u8 port_diag_get_scene_prev(void)
{
	return gSCManagerSceneData.scene_prev;
}

/* id -> name table; SCKind is contiguous so a direct index replaces the
 * case-per-scene switch. Unlisted ids (or future enum additions) read as
 * NULL and report "?". */
static const char *const kPortSceneNames[] = {
	[nSCKindNoController]        = "NoController",
	[nSCKindTitle]               = "Title",
	[nSCKindDebugMaps]           = "DebugMaps",
	[nSCKindDebugCube]           = "DebugCube",
	[nSCKindDebugBattle]         = "DebugBattle",
	[nSCKindDebugFalls]          = "DebugFalls",
	[nSCKindDebugUnknown]        = "DebugUnknown",
	[nSCKindModeSelect]          = "ModeSelect",
	[nSCKind1PMode]              = "1PMode",
	[nSCKindVSMode]              = "VSMode",
	[nSCKindVSOptions]           = "VSOptions",
	[nSCKindVSItemSwitch]        = "VSItemSwitch",
	[nSCKindMessage]             = "Message",
	[nSCKind1PChallenger]        = "1PChallenger",
	[nSCKind1PIntro]             = "1PIntro",
	[nSCKindScreenAdjust]        = "ScreenAdjust",
	[nSCKindPlayersVS]           = "PlayersVS",
	[nSCKind1PGamePlayers]       = "1PGamePlayers",
	[nSCKindPlayers1PTraining]   = "Players1PTraining",
	[nSCKind1PBonus1Players]     = "1PBonus1Players",
	[nSCKind1PBonus2Players]     = "1PBonus2Players",
	[nSCKindMaps]                = "Maps",
	[nSCKindVSBattle]            = "VSBattle",
	[nSCKindUnknownMario]        = "UnknownMario",
	[nSCKindVSResults]           = "VSResults",
	[nSCKindVSRecord]            = "VSRecord",
	[nSCKindCharacters]          = "Characters",
#if defined(REGION_US)
	[nSCKindStartup]             = "Startup",
#endif
	[nSCKindOpeningRoom]         = "OpeningRoom",
	[nSCKindOpeningPortraits]    = "OpeningPortraits",
	[nSCKindOpeningMario]        = "OpeningMario",
	[nSCKindOpeningDonkey]       = "OpeningDonkey",
	[nSCKindOpeningSamus]        = "OpeningSamus",
	[nSCKindOpeningFox]          = "OpeningFox",
	[nSCKindOpeningLink]         = "OpeningLink",
	[nSCKindOpeningYoshi]        = "OpeningYoshi",
	[nSCKindOpeningPikachu]      = "OpeningPikachu",
	[nSCKindOpeningKirby]        = "OpeningKirby",
	[nSCKindOpeningRun]          = "OpeningRun",
	[nSCKindOpeningYoster]       = "OpeningYoster",
	[nSCKindOpeningCliff]        = "OpeningCliff",
	[nSCKindOpeningStandoff]     = "OpeningStandoff",
	[nSCKindOpeningYamabuki]     = "OpeningYamabuki",
	[nSCKindOpeningClash]        = "OpeningClash",
	[nSCKindOpeningSector]       = "OpeningSector",
	[nSCKindOpeningJungle]       = "OpeningJungle",
	[nSCKindOpeningNewcomers]    = "OpeningNewcomers",
	[nSCKindBackupClear]         = "BackupClear",
	[nSCKindEnding]              = "Ending",
	[nSCKind1PContinue]          = "1PContinue",
	[nSCKind1PScoreUnk]          = "1PScoreUnk",
	[nSCKind1PStageClear]        = "1PStageClear",
	[nSCKind1PGame]              = "1PGame",
	[nSCKind1PBonusStage]        = "1PBonusStage",
	[nSCKind1PTrainingMode]      = "1PTrainingMode",
#if defined(REGION_US)
	[nSCKindCongra]              = "Congra",
#endif
	[nSCKindStaffroll]           = "Staffroll",
	[nSCKindOption]              = "Option",
	[nSCKindData]                = "Data",
	[nSCKindSoundTest]           = "SoundTest",
	[nSCKindExplain]             = "Explain",
	[nSCKindAutoDemo]            = "AutoDemo",
};

const char *port_diag_get_scene_name(u8 id)
{
	const char *name = (id < ARRAY_COUNT(kPortSceneNames)) ? kPortSceneNames[id] : NULL;
	return (name != NULL) ? name : "?";
}

/* Whether the current scene has authored climax-freeze frames whose timing
 * is preserved by the RCP cost-model in port/gameloop.cpp.
 *
 * The cost model treats DLs that exceed budget+rect_gate as "authored freezes"
 * and stretches them across multiple VIs to mimic real-N64 RDP overrun. That
 * fidelity is only needed in scenes that play the pre-rendered character
 * intro sequences whose climaxes (Yoshi tongue, Samus grapple, DK smash, etc.)
 * the game authors timed around the natural overrun:
 *   - AutoDemo (attract loop cycles through them)
 *   - the various Opening scenes (first-boot intros, mvOpening*)
 *   - Ending cutscene
 *
 * Everywhere else (CSS, menus, normal battle, 1P game, bonus stages, training,
 * stage clear, etc.) the cost+rect heuristic just mis-fires on UI-fillrate-
 * heavy DLs — the 4-fighter VS CSS panels cluster at cost ≈420k, rect_px ≈236k,
 * which trips both gates every frame and halves the game-tick rate. Issue #78.
 */
sb32 port_scene_wants_freeze_simulation(u8 scene_id)
{
	/* Intro-scene range is contiguous (OpeningRoom..Newcomers);
	 * Ending and AutoDemo sit outside it. */
	if (scene_id >= nSCKindOpeningRoom && scene_id <= nSCKindOpeningNewcomers) {
		return TRUE;
	}
	return scene_id == nSCKindEnding || scene_id == nSCKindAutoDemo;
}
