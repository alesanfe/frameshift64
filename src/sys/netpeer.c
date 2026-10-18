#include <sys/netpeer.h>

#include <ft/fighter.h>
#include <gr/ground.h>
#include <sc/scmanager.h>
#include <sys/netinput.h>
#include <sys/netreplay.h>
#include <sys/netrollback.h>
#include <sys/netsync.h>
#include <sys/utils.h>

#ifdef PORT
extern char *getenv(const char *name);
extern int atoi(const char *s);
extern unsigned long strtoul(const char *s, char **end, int base);
extern int snprintf(char *s, unsigned long size, const char *fmt, ...);
extern void port_log(const char *fmt, ...);
#endif

#ifdef PORT
#include <string.h>
#include <networking/netplay/transport/udp_transport.h>
#include <networking/netplay/protocol/netpacket.h>

/* The codec provides the wire primitives; keep the historical syNetPeer*
 * names as aliases so the call sites below stay in decomp style. */
#define syNetPeerWriteU8  nbNetPacketWriteU8
#define syNetPeerWriteU16 nbNetPacketWriteU16
#define syNetPeerWriteU32 nbNetPacketWriteU32
#define syNetPeerReadU8   nbNetPacketReadU8
#define syNetPeerReadU16  nbNetPacketReadU16
#define syNetPeerReadU32  nbNetPacketReadU32
#define syNetPeerChecksumBytes nbNetPacketChecksumBytes
#define syNetPeerChecksumAccumulateU32 nbNetPacketChecksumAccumulateU32
#define syNetPeerChecksumAccumulateFrame(c, f) \
	nbNetPacketChecksumAccumulateFrame((c), (const NBNetPacketFrame *)(f))
#endif

/* Wire format lives in the pure codec — src/netplay/protocol/netpacket.*.
 * The SYNETPEER_* names stay as aliases so this file keeps its decomp
 * naming while the codec (and its unit tests) share one source of truth. */
#define SYNETPEER_MAGIC NB_NETPACKET_MAGIC
#define SYNETPEER_VERSION NB_NETPACKET_VERSION
#define SYNETPEER_MAX_PACKET_FRAMES NB_NETPACKET_MAX_PACKET_FRAMES
#define SYNETPEER_FRAME_BYTES NB_NETPACKET_FRAME_BYTES
#define SYNETPEER_INPUT_HEADER_BYTES NB_NETPACKET_INPUT_HEADER_BYTES
#define SYNETPEER_SYNC_CHECK_BYTES NB_NETPACKET_SYNC_CHECK_BYTES
#define SYNETPEER_PACKET_BYTES NB_NETPACKET_PACKET_BYTES
#define SYNETPEER_VALIDATION_INPUT_WINDOW 120
#define SYNETPEER_METADATA_BYTES (11 * 4 + 8 + (MAXCONTROLLERS * 7))
#define SYNETPEER_BOOTSTRAP_PACKET_BYTES (4 + 2 + 2 + 4 + SYNETPEER_METADATA_BYTES + 4)
#define SYNETPEER_CONTROL_PACKET_BYTES (4 + 2 + 2 + 4 + 4)
#define SYNETPEER_DEFAULT_INPUT_DELAY 2
#define SYNETPEER_DEFAULT_SESSION_ID 1
#define SYNETPEER_DEFAULT_BOOTSTRAP_SEED 12345
#define SYNETPEER_LOG_INTERVAL 120
#define SYNETPEER_BOOTSTRAP_RETRY_COUNT 300
#define SYNETPEER_BOOTSTRAP_RETRY_USECS 16666
#define SYNETPEER_BARRIER_LOG_INTERVAL 30
#define SYNETPEER_BATTLE_START_REPEAT_FRAMES 30

/* Rollback pacing: how many ticks a peer may simulate past the newest
 * contiguously confirmed remote input. Resimulation must reach back to the
 * first mispredicted tick through the snapshot ring, so this stays below
 * SYNETROLLBACK_SLOT_NUM. */
#define SYNETPEER_MAX_PREDICTION 6

/* Time sync: each packet carries the sender's frame advantage (ticks it runs
 * ahead of the newest remote input it has confirmed). The peer that is
 * further ahead skips one frame at a time, at most every N frames, so both
 * sides share the rollback load instead of one peer predicting at the cap. */
#define SYNETPEER_TIMESYNC_THRESHOLD 2
#define SYNETPEER_TIMESYNC_INTERVAL 10

/* Adaptive input delay (any peer may propose): when late remote inputs
 * accumulate — each is a prediction that may roll back — the observing
 * peer broadcasts a DELAY bump and everyone applies max(local, received),
 * giving remote inputs more wire time and breaking the
 * stall->resim->stall treadmill. Delay stays below the prediction cap
 * and is monotonic within a match; bumps re-send for a few frames so a
 * lost packet converges instead of leaving peers asymmetric. */
#define SYNETPEER_MAX_INPUT_DELAY 6
#define SYNETPEER_DELAY_CHECK_FRAMES 60
#define SYNETPEER_DELAY_STALL_TRIGGER 5
#define SYNETPEER_DELAY_REPEAT_FRAMES 10

void syNetPeerReceiveRemoteInput(void);
void syNetPeerUpdateStartBarrier(void);
void syNetPeerSendLocalInput(void);
static void syNetPeerSendForwardPacket(s32 link, s32 src_player, const u8 *buffer, s32 size);

#define SYNETPEER_PACKET_INPUT NB_NETPACKET_PACKET_INPUT
#define SYNETPEER_PACKET_MATCH_CONFIG NB_NETPACKET_PACKET_MATCH_CONFIG
#define SYNETPEER_PACKET_READY NB_NETPACKET_PACKET_READY
#define SYNETPEER_PACKET_START NB_NETPACKET_PACKET_START
#define SYNETPEER_PACKET_BATTLE_READY NB_NETPACKET_PACKET_BATTLE_READY
#define SYNETPEER_PACKET_BATTLE_START NB_NETPACKET_PACKET_BATTLE_START
#define SYNETPEER_PACKET_FORWARD NB_NETPACKET_PACKET_FORWARD
#define SYNETPEER_PACKET_DELAY NB_NETPACKET_PACKET_DELAY
#define SYNETPEER_DELAY_PACKET_BYTES NB_NETPACKET_DELAY_PACKET_BYTES

/* FORWARD packets let the star-topology host relay one client's input
 * datagram to the other clients: [magic][version][type][session]
 * [src_player][inner packet][checksum]. */
#define SYNETPEER_FORWARD_HEADER_BYTES NB_NETPACKET_FORWARD_HEADER_BYTES
#define SYNETPEER_FORWARD_PACKET_BYTES NB_NETPACKET_FORWARD_PACKET_BYTES

/* A remote that goes silent mid-match stops pacing the session after this
 * many milliseconds: its fighter freezes on the last input but the other
 * players keep playing. */
#define SYNETPEER_REMOTE_DROP_MS 10000

/* Test rigs may need a longer leash: several same-machine peers can stall
 * a process past the default 10s (rollback resims blocking the netloop on
 * starved CPU) without the peer actually being dead. Overridable via
 * SSB64_NETPLAY_DROP_MS in test-hook builds. */
static u32 syNetPeerRemoteDropMs(void)
{
#ifdef SSB64_TEST_HOOKS
	const char *env = getenv("SSB64_NETPLAY_DROP_MS");

	if ((env != NULL) && (atoi(env) > 0))
	{
		return (u32)atoi(env);
	}
#endif
	return SYNETPEER_REMOTE_DROP_MS;
}

typedef NBNetPacketFrame SYNetPeerPacketFrame;

/* One per remote participant, indexed by game slot. link is the port_udp
 * link the remote is reached on (equal to its slot in lobby matches);
 * link == -1 marks a remote the star host forwards to us (clients) or the
 * legacy single-peer socket (env-driven sessions). */
typedef struct SYNetPeerRemote
{
	s32 player;
	s32 link;
	u32 recv_seq_highwater;
	sb32 recv_seq_initialized;
	u32 seq_gaps;
	u32 seq_duplicates;
	u32 seq_out_of_order;
	u32 last_peer_ack_tick;
	s32 remote_advantage;
	u32 highest_remote_tick;
	u32 last_heard_ms;
	sb32 dropped;
	sb32 battle_ready;
	sb32 battle_start_received;
	u32 last_packet_oldest_tick;
	u32 last_packet_newest_tick;
	sb32 last_packet_ticks_valid;

} SYNetPeerRemote;

SYNetPeerRemote sSYNetPeerRemotes[MAXCONTROLLERS];
s32 sSYNetPeerRemoteCount;

sb32 sSYNetPeerIsEnabled;
sb32 sSYNetPeerIsConfigured;
sb32 sSYNetPeerIsActive;
s32 sSYNetPeerLocalPlayer;
s32 sSYNetPeerRemotePlayer;
u32 sSYNetPeerInputDelay;
u32 sSYNetPeerSessionID;
u32 sSYNetPeerHighestRemoteTick;
u32 sSYNetPeerPacketsSent;
u32 sSYNetPeerPacketsReceived;
u32 sSYNetPeerPacketsDropped;
u32 sSYNetPeerFramesStaged;
u32 sSYNetPeerLateFrames;
u32 sSYNetPeerInputChecksum;
u32 sSYNetPeerDelayLastStallCheck;
u32 sSYNetPeerDelayCheckStallBase;
u32 sSYNetPeerDelayCheckLateBase;
s32 sSYNetPeerDelayRepeatLeft;
u32 sSYNetPeerLastLogTick;
u32 sSYNetPeerSendSeq;
u32 sSYNetPeerRecvSeqHighWater;
sb32 sSYNetPeerRecvSeqInitialized;
u32 sSYNetPeerSeqGaps;
u32 sSYNetPeerSeqDuplicates;
u32 sSYNetPeerSeqOutOfOrder;
u32 sSYNetPeerLastPeerAckTick;
u32 sSYNetPeerLastPacketOldestTick;
u32 sSYNetPeerLastPacketNewestTick;
sb32 sSYNetPeerLastPacketTicksValid;
sb32 sSYNetPeerBootstrapIsEnabled;
sb32 sSYNetPeerBootstrapIsHost;
sb32 sSYNetPeerBootstrapMetadataApplied;
sb32 sSYNetPeerBootstrapPeerReady;
sb32 sSYNetPeerBootstrapStartReceived;
u32 sSYNetPeerBootstrapSeed;
/* Mutation testing hooks (SSB64_NETPLAY_MUTANT=<name>): each knob removes
 * one validation so the suite can prove the detector catches the failure.
 * accept_bad_session | accept_bad_checksum | no_forward_guard. */
sb32 sSYNetPeerMutantBadSession;
sb32 sSYNetPeerMutantBadChecksum;
sb32 sSYNetPeerMutantNoForwardGuard;
/* SSB64_NETPLAY_PACKETFUZZ=<count>: seeded random datagrams injected into
 * the receive path each call. See syNetPeerReceiveRemoteInput. */
s32 sSYNetPeerPacketFuzzLeft = -2;
SYNetInputReplayMetadata sSYNetPeerBootstrapMetadata;
sb32 sSYNetPeerBattleBarrierEnabled;
sb32 sSYNetPeerBattleLocalReady;
sb32 sSYNetPeerBattlePeerReady;
sb32 sSYNetPeerBattleStartSent;
sb32 sSYNetPeerBattleStartReceived;
sb32 sSYNetPeerBattleBarrierReleased;
u32 sSYNetPeerBattleBarrierWaitFrames;
u32 sSYNetPeerBattleStartRepeatFrames;
u32 sSYNetPeerExecutionHoldFrames;
sb32 sSYNetPeerExecutionBeginLogged;

sb32 sSYNetPeerIsRollback;           // SSB64_NETPLAY_ROLLBACK (default on)
u32 sSYNetPeerFuzzSeed;              // SSB64_NETPLAY_FUZZ

/* Spectator mode (PORT lobby "spectate" role): this instance only watches a
 * live match. Every player slot is a remote stream, nothing is published or
 * sent, the start barrier is skipped, and pacing is strict - a tick may run
 * only once every player's input for it has arrived (never predicted). The
 * star host pushes each packet it receives to the spectator's relay links,
 * FORWARD-wrapped. */
sb32 sSYNetPeerIsSpectator;
s32 sSYNetPeerSpecLinks[8];
s32 sSYNetPeerSpecLinkCount;

sb32 sSYNetPeerFrameReady;           // decided once per frame in syNetPeerBeginFrame
u32 sSYNetPeerStallFrames;
u32 sSYNetPeerLockstepStallFrames;
s32 sSYNetPeerRemoteAdvantage;
u32 sSYNetPeerTimeSyncStallFrames;
u32 sSYNetPeerFramesSinceTimeSync;
sb32 sSYNetPeerIsSocketExternal;     // the lobby owns the UDP endpoint; don't close it

#ifdef PORT
const char *sSYNetPeerBindSpec;
const char *sSYNetPeerPeerSpec;
#endif

SYNetPeerRemote *syNetPeerGetRemoteByPlayer(s32 player)
{
	if ((player >= 0) && (player < MAXCONTROLLERS) && (sSYNetPeerRemotes[player].player == player))
	{
		return &sSYNetPeerRemotes[player];
	}
	return NULL;
}

SYNetPeerRemote *syNetPeerGetRemoteByLink(s32 link)
{
	s32 player;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		if ((sSYNetPeerRemotes[player].player == player) && (sSYNetPeerRemotes[player].link == link))
		{
			return &sSYNetPeerRemotes[player];
		}
	}
	return NULL;
}

/* Tick all live remotes have confirmed up to: the session may only run ahead
 * of the *slowest* remote input stream, so pacing uses the minimum. */
u32 syNetPeerGetRemoteConfirmedTick(void)
{
	u32 confirmed = 0xFFFFFFFFU;
	s32 player;
	sb32 any = FALSE;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		SYNetPeerRemote *remote = &sSYNetPeerRemotes[player];

		if ((remote->player == player) && (remote->dropped == FALSE))
		{
			u32 next = syNetInputGetRemoteNextUnconfirmed(player);

			if (next < confirmed)
			{
				confirmed = next;
			}
			any = TRUE;
		}
	}
	/* no live remotes: nothing left to wait on, free-run */
	return (any != FALSE) ? confirmed : 0xFFFFFFFFU;
}

/* Worst (largest) frame advantage reported by any live remote: if one peer
 * runs far behind, everyone eases off so it can catch up. */
s32 syNetPeerGetWorstRemoteAdvantage(void)
{
	s32 worst = 0;
	s32 player;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		SYNetPeerRemote *remote = &sSYNetPeerRemotes[player];

		if ((remote->player == player) && (remote->dropped == FALSE) &&
			(remote->remote_advantage > worst))
		{
			worst = remote->remote_advantage;
		}
	}
	return worst;
}

/* Highest input tick seen so far, per remote (or the minimum across live
 * remotes when for_all != FALSE): goes into the packet's ack field. */
u32 syNetPeerGetAckTick(const SYNetPeerRemote *remote)
{
	return (remote != NULL) ? remote->highest_remote_tick : 0;
}

void syNetPeerResetRemote(SYNetPeerRemote *remote, s32 player, s32 link)
{
	memset(remote, 0, sizeof(*remote));
	remote->player = player;
	remote->link = link;
	remote->last_heard_ms = 0;
	remote->remote_advantage = 0;
	remote->highest_remote_tick = 0;
}

void syNetPeerResetRemotes(void)
{
	s32 player;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		sSYNetPeerRemotes[player].player = -1;
		sSYNetPeerRemotes[player].link = -1;
	}
	sSYNetPeerRemoteCount = 0;
}

/* A packet just arrived from this remote: it is alive. If it had been
 * dropped for silence it rejoins pacing; its fighter unfreezes as its
 * real inputs replace the predictions. */
void syNetPeerTouchRemote(SYNetPeerRemote *remote)
{
#ifdef PORT
	remote->last_heard_ms = port_udp_now_ms();
	if (remote->dropped != FALSE)
	{
		remote->dropped = FALSE;
		port_log("SSB64 NetPeer: remote player %d resumed - back in pacing\n", remote->player);
	}
#endif
}

sb32 syNetPeerAddRemote(s32 player, s32 link)
{
	if ((player < 0) || (player >= MAXCONTROLLERS) || (player == sSYNetPeerLocalPlayer))
	{
		return FALSE;
	}
	syNetPeerResetRemote(&sSYNetPeerRemotes[player], player, link);
#ifdef PORT
	/* The drop clock starts now: a remote that never sends a single packet
	 * stops gating the session after SYNETPEER_REMOTE_DROP_MS instead of
	 * blocking the start barrier forever. */
	sSYNetPeerRemotes[player].last_heard_ms = port_udp_now_ms();
#endif
	sSYNetPeerRemoteCount++;
	sSYNetPeerRemotePlayer = player;    // last added; keeps single-remote callers/logs sane
	return TRUE;
}

#ifdef PORT
/* Spectator relay links (the lobby registers one per watcher): the star host
 * FORWARD-wraps every input packet it handles - its own stream included -
 * onto these links so a spectator sees exactly the streams the players do
 * without ever joining pacing. */
void syNetPeerAddSpecLink(s32 link)
{
	s32 i;

	if (link < 0)
	{
		return;
	}
	for (i = 0; i < sSYNetPeerSpecLinkCount; i++)
	{
		if (sSYNetPeerSpecLinks[i] == link)
		{
			return;
		}
	}
	if (sSYNetPeerSpecLinkCount < (s32)(sizeof(sSYNetPeerSpecLinks) / sizeof(sSYNetPeerSpecLinks[0])))
	{
		sSYNetPeerSpecLinks[sSYNetPeerSpecLinkCount++] = link;
	}
}

extern u32 sSYNetPeerSpecHistoryCursor[8];
extern u32 sSYNetPeerSpecProgress[8];
extern sb32 sSYNetPeerSpecCatchUp[8];
extern u32 sSYNetPeerSpecStallFrames[8];

void syNetPeerRemoveSpecLink(s32 link)
{
	s32 i;

	for (i = 0; i < sSYNetPeerSpecLinkCount; i++)
	{
		if (sSYNetPeerSpecLinks[i] == link)
		{
			sSYNetPeerSpecLinks[i] = sSYNetPeerSpecLinks[--sSYNetPeerSpecLinkCount];
			sSYNetPeerSpecHistoryCursor[i] = sSYNetPeerSpecHistoryCursor[sSYNetPeerSpecLinkCount];
			sSYNetPeerSpecProgress[i] = sSYNetPeerSpecProgress[sSYNetPeerSpecLinkCount];
			sSYNetPeerSpecCatchUp[i] = sSYNetPeerSpecCatchUp[sSYNetPeerSpecLinkCount];
			sSYNetPeerSpecStallFrames[i] = sSYNetPeerSpecStallFrames[sSYNetPeerSpecLinkCount];
			return;
		}
	}
}

/* ── Mid-match spectator catch-up ───────────────────────────────────────
 * The host auto-records every online match (syNetReplayArmAutoSave), so a
 * spectator joining mid-match doesn't need a state snapshot: the spec
 * starts at tick 0, the host streams the recorded input log FORWARD-wrapped
 * like live traffic, and the spec runs several sim ticks per display frame
 * (syNetPeerSpectateCatchUp) until it lands on the live edge.
 *
 * Flow control: the spec reports its consumed tick in SPEC_PROGRESS packets
 * and the host never streams past spec_tick + SYNETPEER_SPEC_WINDOW - the
 * spec's remote-input ring is only 720 deep and stores by (tick % 720), so
 * overrunning it would overwrite unconsumed frames and wedge the spec on a
 * permanent gap. If progress stops (a stream packet was lost) the host
 * rewinds the cursor and resends the whole unacked window. */

#define SYNETPEER_SPEC_WINDOW 512                 /* ring is 720; stay clear */
#define SYNETPEER_SPEC_HISTORY_PKTS 4             /* INPUT packets per player per call */
#define SYNETPEER_SPEC_STALL_FRAMES 120           /* resend window after ~2s no progress */
#define SYNETPEER_SPEC_PROGRESS_INTERVAL 15       /* spec reports every N frames */
#define SYNETPEER_SPEC_CATCHUP_TICKS 16           /* max sim ticks per display frame */
/* A spec this close to the live edge switches to the live FORWARD stream:
 * every live INPUT packet already repeats the last MAX_PACKET_FRAMES ticks,
 * so frames newer than live - SYNETPEER_SPEC_LIVE_MARGIN arrive either way. */
#define SYNETPEER_SPEC_LIVE_MARGIN NB_NETPACKET_MAX_PACKET_FRAMES
/* Re-arm the history stream when the spec falls this far behind again
 * (hysteresis vs SYNETPEER_SPEC_LIVE_MARGIN so the mode can't flap). */
#define SYNETPEER_SPEC_REARM_MARGIN (4 * NB_NETPACKET_MAX_PACKET_FRAMES)

u32 sSYNetPeerSpecHistoryCursor[8];
u32 sSYNetPeerSpecProgress[8];
sb32 sSYNetPeerSpecCatchUp[8];
u32 sSYNetPeerSpecStallFrames[8];
u32 sSYNetPeerSpecReportCounter;

/* Drops every spec link AND its catch-up state: a stale TRUE in
 * sSYNetPeerSpecCatchUp at index i would otherwise leak into the next
 * match's spec at the same index. */
static void syNetPeerResetSpecLinks(void)
{
	sSYNetPeerSpecLinkCount = 0;
	memset(sSYNetPeerSpecHistoryCursor, 0, sizeof(sSYNetPeerSpecHistoryCursor));
	memset(sSYNetPeerSpecProgress, 0, sizeof(sSYNetPeerSpecProgress));
	memset(sSYNetPeerSpecCatchUp, 0, sizeof(sSYNetPeerSpecCatchUp));
	memset(sSYNetPeerSpecStallFrames, 0, sizeof(sSYNetPeerSpecStallFrames));
}

static s32 syNetPeerGetSpecIndex(s32 link)
{
	s32 i;

	for (i = 0; i < sSYNetPeerSpecLinkCount; i++)
	{
		if (sSYNetPeerSpecLinks[i] == link)
		{
			return i;
		}
	}
	return -1;
}

/* Host: arm the history stream for a spec link that joined mid-match.
 * Safe to call again for an already-armed link (idempotent). */
void syNetPeerBeginSpecCatchUp(s32 link)
{
	s32 i;

	if ((sSYNetPeerIsActive == FALSE) || (sSYNetPeerBootstrapIsHost == FALSE))
	{
		return;
	}
	i = syNetPeerGetSpecIndex(link);
	if (i < 0)
	{
		return;
	}
	sSYNetPeerSpecCatchUp[i] = TRUE;
	sSYNetPeerSpecHistoryCursor[i] = 0;
	sSYNetPeerSpecProgress[i] = 0;
	sSYNetPeerSpecStallFrames[i] = 0;
	port_log("SSB64 NetPeer: spec link %d mid-match - streaming input history\n", link);
}

/* Host: a progress report arrived on a spec link. Specs ride the relay,
 * which only lets SSBL text payloads through spec→host - so the report is
 * the text line "SSBL SPECTICK <tick>". (The SPEC_PROGRESS binary packet
 * is also accepted for direct/non-relay links.) */
static void syNetPeerHandleSpecProgress(const u8 *buffer, s32 size, s32 link)
{
	s32 i;
	u32 tick = 0;

	if ((size > 13) && (memcmp(buffer, "SSBL SPECTICK ", 14) == 0))
	{
		tick = (u32)strtoul((const char *)buffer + 14, NULL, 10);
	}
	else if (nbNetPacketParseSpecProgress(buffer, size, sSYNetPeerSessionID, &tick) != NB_NETPACKET_OK)
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	i = syNetPeerGetSpecIndex(link);
	if (i < 0)
	{
		return;
	}
	if (tick > sSYNetPeerSpecProgress[i])
	{
		sSYNetPeerSpecProgress[i] = tick;
		sSYNetPeerSpecStallFrames[i] = 0;
	}
}

/* Host: each frame, push the next slice of recorded inputs to catching-up
 * spec links. One cursor per spec covers all players - every slot was
 * recorded for the same ticks. */
void syNetPeerServeSpecHistory(void)
{
#ifdef PORT
	u8 buffer[SYNETPEER_PACKET_BYTES];
	u32 live;
	u32 servable;
	s32 i;
	s32 player;

	if ((sSYNetPeerBootstrapIsHost == FALSE) || (sSYNetPeerSpecLinkCount == 0))
	{
		return;
	}
	live = syNetInputGetTick();
	/* The replay record for a remote player's tick is final only once that
	 * tick is confirmed - below it the record can still hold a prediction
	 * that a rollback rewrites. The spec's remote store is first-write-wins,
	 * so streaming a predicted record would pin the wrong input on the spec
	 * forever (resends of the corrected frame are dropped as redundant).
	 * Live FORWARDs don't need this gate: they carry the sender's own
	 * scheduled frames, which are final by definition. */
	servable = live;
	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		if ((player != sSYNetPeerLocalPlayer) && (sSYNetPeerRemotes[player].player == player))
		{
			u32 edge = syNetInputGetRemoteNextUnconfirmed(player);

			if (edge < servable)
			{
				servable = edge;
			}
		}
	}
	for (i = 0; i < sSYNetPeerSpecLinkCount; i++)
	{
		u32 progress;
		u32 cursor;
		u32 ceiling;
		u32 sent;

		if (sSYNetPeerSpecCatchUp[i] == FALSE)
		{
			/* Live-mode spec fell far enough behind that the repeated frame
			 * window can't reach its pending tick (loss burst) - re-arm the
			 * stream from its consume point. The spec keeps reporting
			 * SPECTICK unconditionally, so this recovers on its own. */
			if (live >= sSYNetPeerSpecProgress[i] + SYNETPEER_SPEC_REARM_MARGIN)
			{
				sSYNetPeerSpecCatchUp[i] = TRUE;
				sSYNetPeerSpecHistoryCursor[i] = sSYNetPeerSpecProgress[i];
				sSYNetPeerSpecStallFrames[i] = 0;
				port_log("SSB64 NetPeer: spec link %d fell behind (progress=%u live=%u) - resuming history stream\n",
						 sSYNetPeerSpecLinks[i], sSYNetPeerSpecProgress[i], live);
			}
			continue;
		}
		progress = sSYNetPeerSpecProgress[i];
		cursor = sSYNetPeerSpecHistoryCursor[i];
		ceiling = progress + SYNETPEER_SPEC_WINDOW;
		if (ceiling > servable)
		{
			ceiling = servable;
		}
		if (cursor >= ceiling)
		{
			if (progress + SYNETPEER_SPEC_LIVE_MARGIN >= live)
			{
				/* The spec consumed up to the live edge (within the repeated
				 * frame window of the live stream): it rides the normal live
				 * FORWARDs from here. */
				sSYNetPeerSpecCatchUp[i] = FALSE;
				port_log("SSB64 NetPeer: spec link %d caught up at tick %u\n",
						 sSYNetPeerSpecLinks[i], live);
				continue;
			}
			/* Window exhausted without the spec reaching the live edge: if
			 * its progress hasn't moved, packets were lost - rewind and
			 * resend the unacked window. (Not gated on cursor < live: a
			 * cursor that already raced past the live edge still has to
			 * resend while the spec's consumed tick lags behind it.) */
			if (++sSYNetPeerSpecStallFrames[i] >= SYNETPEER_SPEC_STALL_FRAMES)
			{
				sSYNetPeerSpecStallFrames[i] = 0;
				sSYNetPeerSpecHistoryCursor[i] = progress;
			}
			continue;
		}
		sent = 0;
		while ((cursor < ceiling) && (sent < SYNETPEER_SPEC_HISTORY_PKTS * NB_NETPACKET_MAX_PACKET_FRAMES))
		{
			sb32 any_sent = FALSE;
			u32 chunk = ceiling - cursor;

			if (chunk > NB_NETPACKET_MAX_PACKET_FRAMES)
			{
				chunk = NB_NETPACKET_MAX_PACKET_FRAMES;
			}
			/* Cover every slot that was in the match: remotes AND the host's
			 * own stream (the spec gets those live via send-forwards too). */
			for (player = 0; player < MAXCONTROLLERS; player++)
			{
				NBNetPacketInput pkt;
				u32 f;

				if ((sSYNetPeerRemotes[player].player != player) && (player != sSYNetPeerLocalPlayer))
				{
					continue;
				}
				memset(&pkt, 0, sizeof(pkt));
				pkt.remote_advantage = 0;
				pkt.session_id = sSYNetPeerSessionID;
				pkt.ack_tick = 0;
				pkt.seq = cursor / NB_NETPACKET_MAX_PACKET_FRAMES;
				pkt.player = (u8)player;
				pkt.local_player = (u8)player;
				pkt.remote_player = 0;
				pkt.check_tick = NB_NETPACKET_NO_CHECK_TICK;
				pkt.frame_count = 0;
				for (f = 0; f < chunk; f++)
				{
					SYNetInputFrame frame;

					if (syNetInputGetReplayFrame(player, cursor + f, &frame) == FALSE)
					{
						break;   /* not recorded (past the 21600-frame cap) */
					}
					pkt.frames[f].tick = frame.tick;
					pkt.frames[f].buttons = frame.buttons;
					pkt.frames[f].stick_x = frame.stick_x;
					pkt.frames[f].stick_y = frame.stick_y;
					pkt.frame_count++;
				}
				if (pkt.frame_count == 0)
				{
					continue;
				}
				nbNetPacketBuildInput(buffer, &pkt);
				syNetPeerSendForwardPacket(sSYNetPeerSpecLinks[i], player, buffer, SYNETPEER_PACKET_BYTES);
				any_sent = TRUE;
			}
			if (any_sent == FALSE)
			{
				break;
			}
			cursor += chunk;
			sent += chunk;
		}
		sSYNetPeerSpecHistoryCursor[i] = cursor;
	}
#endif
}

/* Spectator: report our consumed sim tick to the host so it can pace the
 * catch-up stream. Sent on the remote link the host's FORWARDs arrive on;
 * the host maps it back to our spec link via the relay slot. */
void syNetPeerSendSpecProgress(void)
{
#ifdef PORT
	char buffer[40];
	s32 size;

	/* A spec's player remotes all have link -1 - its only channel is the
	 * relay link to the host's room slot (0), the same link the FORWARD
	 * datagrams arrive on. The relay only forwards SSBL-prefixed payloads
	 * spec→host, so the report rides the text channel. */
	size = snprintf(buffer, sizeof(buffer), "SSBL SPECTICK %u", syNetInputGetTick());
	if (size > 0)
	{
		port_udp_link_send(0, buffer, size + 1);
	}
#endif
}

/* Spectator-side catch-up: while the confirmed input lead exceeds our tick,
 * run extra sim ticks in this display frame. Each iteration re-drains the
 * socket via syNetInputFuncRead, so the stream keeps flowing while we
 * consume; at the live edge the tick holds and the loop exits. */
void syNetPeerSpectateCatchUp(void (*update)(void))
{
#ifdef PORT
	s32 budget;
	const char *env;

	if (sSYNetPeerIsSpectator == FALSE)
	{
		return;
	}
	budget = SYNETPEER_SPEC_CATCHUP_TICKS;
	env = getenv("SSB64_SPEC_CATCHUP_TPF");
	if (env != NULL)
	{
		budget = atoi(env);
		if (budget < 0)
		{
			budget = SYNETPEER_SPEC_CATCHUP_TICKS;
		}
	}
	/* Report our consumed tick every N display frames so the host can pace
	 * (and resume) the history stream. Kept unconditional: falling behind
	 * again after a loss burst resumes the stream by itself. */
	if (++sSYNetPeerSpecReportCounter >= SYNETPEER_SPEC_PROGRESS_INTERVAL)
	{
		sSYNetPeerSpecReportCounter = 0;
		syNetPeerSendSpecProgress();
	}
	while ((budget-- > 0) &&
		   (syNetInputGetTick() < syNetPeerGetRemoteConfirmedTick()))
	{
		syNetInputFuncRead();
		if (syNetPeerCheckBattleExecutionReady() == FALSE)
		{
			break;
		}
		/* through RunBattleUpdate so catch-up ticks store their live hash and
		 * emit the confirmed-state trace like any other simulated tick */
		syNetRollbackRunBattleUpdate(update);
	}
#endif
}
#endif

void syNetPeerWriteMetadata(u8 **cursor, const SYNetInputReplayMetadata *metadata)
{
	s32 player;

	syNetPeerWriteU32(cursor, metadata->magic);
	syNetPeerWriteU32(cursor, metadata->version);
	syNetPeerWriteU32(cursor, metadata->scene_kind);
	syNetPeerWriteU32(cursor, metadata->player_count);
	syNetPeerWriteU32(cursor, metadata->stage_kind);
	syNetPeerWriteU32(cursor, metadata->stocks);
	syNetPeerWriteU32(cursor, metadata->time_limit);
	syNetPeerWriteU32(cursor, metadata->item_switch);
	syNetPeerWriteU32(cursor, metadata->item_toggles);
	syNetPeerWriteU32(cursor, metadata->rng_seed);
	syNetPeerWriteU32(cursor, metadata->game_type);
	syNetPeerWriteU8(cursor, metadata->game_rules);
	syNetPeerWriteU8(cursor, metadata->is_team_battle);
	syNetPeerWriteU8(cursor, metadata->handicap);
	syNetPeerWriteU8(cursor, metadata->is_team_attack);
	syNetPeerWriteU8(cursor, metadata->is_stage_select);
	syNetPeerWriteU8(cursor, metadata->damage_ratio);
	syNetPeerWriteU8(cursor, metadata->item_appearance_rate);
	syNetPeerWriteU8(cursor, metadata->is_not_teamshadows);

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		syNetPeerWriteU8(cursor, metadata->player_kinds[player]);
		syNetPeerWriteU8(cursor, metadata->fighter_kinds[player]);
		syNetPeerWriteU8(cursor, metadata->costumes[player]);
		syNetPeerWriteU8(cursor, metadata->teams[player]);
		syNetPeerWriteU8(cursor, metadata->handicaps[player]);
		syNetPeerWriteU8(cursor, metadata->levels[player]);
		syNetPeerWriteU8(cursor, metadata->shades[player]);
	}
}

void syNetPeerReadMetadata(const u8 **cursor, SYNetInputReplayMetadata *metadata)
{
	s32 player;

	metadata->magic = syNetPeerReadU32(cursor);
	metadata->version = syNetPeerReadU32(cursor);
	metadata->scene_kind = syNetPeerReadU32(cursor);
	metadata->player_count = syNetPeerReadU32(cursor);
	metadata->stage_kind = syNetPeerReadU32(cursor);
	metadata->stocks = syNetPeerReadU32(cursor);
	metadata->time_limit = syNetPeerReadU32(cursor);
	metadata->item_switch = syNetPeerReadU32(cursor);
	metadata->item_toggles = syNetPeerReadU32(cursor);
	metadata->rng_seed = syNetPeerReadU32(cursor);
	metadata->game_type = syNetPeerReadU32(cursor);
	metadata->game_rules = syNetPeerReadU8(cursor);
	metadata->is_team_battle = syNetPeerReadU8(cursor);
	metadata->handicap = syNetPeerReadU8(cursor);
	metadata->is_team_attack = syNetPeerReadU8(cursor);
	metadata->is_stage_select = syNetPeerReadU8(cursor);
	metadata->damage_ratio = syNetPeerReadU8(cursor);
	metadata->item_appearance_rate = syNetPeerReadU8(cursor);
	metadata->is_not_teamshadows = syNetPeerReadU8(cursor);

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		metadata->player_kinds[player] = syNetPeerReadU8(cursor);
		metadata->fighter_kinds[player] = syNetPeerReadU8(cursor);
		metadata->costumes[player] = syNetPeerReadU8(cursor);
		metadata->teams[player] = syNetPeerReadU8(cursor);
		metadata->handicaps[player] = syNetPeerReadU8(cursor);
		metadata->levels[player] = syNetPeerReadU8(cursor);
		metadata->shades[player] = syNetPeerReadU8(cursor);
	}
}

sb32 syNetPeerCheckMetadata(const SYNetInputReplayMetadata *metadata)
{
	if ((metadata->magic != SYNETINPUT_REPLAY_MAGIC) ||
		(metadata->version != SYNETINPUT_REPLAY_VERSION) ||
		(metadata->scene_kind != nSCKindVSBattle) ||
		(metadata->player_count == 0) ||
		(metadata->player_count > MAXCONTROLLERS))
	{
		return FALSE;
	}
	return TRUE;
}

#ifdef PORT
void syNetPeerSleepBootstrapRetry(void)
{
	port_udp_sleep_us(SYNETPEER_BOOTSTRAP_RETRY_USECS);
}

void syNetPeerCloseSocket(void)
{
	port_udp_close();
}

sb32 syNetPeerOpenSocket(void)
{
	if (port_udp_is_open())
	{
		return TRUE;
	}
	if ((port_udp_open(sSYNetPeerBindSpec) == FALSE) || (port_udp_set_peer(sSYNetPeerPeerSpec) == FALSE))
	{
		syNetPeerCloseSocket();
		return FALSE;
	}
	return TRUE;
}
#endif

void syNetPeerMakeBootstrapMetadata(SYNetInputReplayMetadata *metadata)
{
	s32 player;

	memset(metadata, 0, sizeof(*metadata));
	metadata->magic = SYNETINPUT_REPLAY_MAGIC;
	metadata->version = SYNETINPUT_REPLAY_VERSION;
	metadata->scene_kind = nSCKindVSBattle;
	metadata->player_count = 2;
	metadata->stage_kind = nGRKindCastle;
	metadata->stocks = 3;
	metadata->time_limit = SCBATTLE_TIMELIMIT_INFINITE;
	metadata->item_switch = nSCBattleItemSwitchNone;
	metadata->item_toggles = 0;
	metadata->rng_seed = sSYNetPeerBootstrapSeed;
	metadata->game_type = nSCBattleGameTypeRoyal;
	metadata->game_rules = SCBATTLE_GAMERULE_STOCK;
	metadata->is_team_battle = FALSE;
	metadata->handicap = nSCBattleHandicapOff;
	metadata->is_team_attack = FALSE;
	metadata->is_stage_select = FALSE;
	metadata->damage_ratio = 100;
	metadata->item_appearance_rate = nSCBattleItemSwitchNone;
	metadata->is_not_teamshadows = TRUE;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		metadata->player_kinds[player] = nFTPlayerKindNot;
		metadata->fighter_kinds[player] = nFTKindNull;
		metadata->costumes[player] = 0;
		metadata->teams[player] = player;
		metadata->handicaps[player] = 9;
		metadata->levels[player] = 3;
		metadata->shades[player] = 0;
	}
	metadata->player_kinds[0] = nFTPlayerKindMan;
	metadata->fighter_kinds[0] = nFTKindMario;
	metadata->player_kinds[1] = nFTPlayerKindMan;
	metadata->fighter_kinds[1] = nFTKindFox;
}

void syNetPeerApplyBootstrapMetadata(const SYNetInputReplayMetadata *metadata)
{
	sSYNetPeerBootstrapMetadata = *metadata;
	syNetReplayApplyBattleMetadata(&sSYNetPeerBootstrapMetadata);
	syUtilsSetRandomSeed(sSYNetPeerBootstrapMetadata.rng_seed);
	gSCManagerSceneData.scene_prev = nSCKindVSMode;
	gSCManagerSceneData.scene_curr = nSCKindVSBattle;
	sSYNetPeerBootstrapMetadataApplied = TRUE;

#ifdef PORT
	port_log("SSB64 NetPeer: bootstrap metadata applied host=%d stage=%u seed=%u players=%u p0=%u/%u p1=%u/%u\n",
	         sSYNetPeerBootstrapIsHost, sSYNetPeerBootstrapMetadata.stage_kind,
	         sSYNetPeerBootstrapMetadata.rng_seed, sSYNetPeerBootstrapMetadata.player_count,
	         sSYNetPeerBootstrapMetadata.player_kinds[0], sSYNetPeerBootstrapMetadata.fighter_kinds[0],
	         sSYNetPeerBootstrapMetadata.player_kinds[1], sSYNetPeerBootstrapMetadata.fighter_kinds[1]);
#endif
}

#ifdef PORT
void syNetPeerSendBytes(const u8 *buffer, u32 size)
{
	port_udp_send(buffer, (s32)size);
}

/* Lobby (external-socket) matches send each datagram on the remote's link;
 * env-driven sessions share the legacy single-peer socket. */
void syNetPeerSendBytesTo(const SYNetPeerRemote *remote, const u8 *buffer, u32 size)
{
	if (sSYNetPeerIsSocketExternal != FALSE)
	{
		/* link == -1 remotes are reached through the star host's FORWARD
		 * wrapper: there is no direct path to send on. */
		if ((remote != NULL) && (remote->link >= 0))
		{
			port_udp_link_send(remote->link, buffer, (s32)size);
		}
		return;
	}
	syNetPeerSendBytes(buffer, size);
}
#endif

void syNetPeerSendControlPacket(u16 packet_type)
{
#ifdef PORT
	u8 buffer[SYNETPEER_CONTROL_PACKET_BYTES];
	s32 player;

	nbNetPacketBuildControl(buffer, packet_type, sSYNetPeerSessionID);
	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		SYNetPeerRemote *remote = &sSYNetPeerRemotes[player];

		if ((remote->player == player) && (remote->dropped == FALSE))
		{
			syNetPeerSendBytesTo(remote, buffer, SYNETPEER_CONTROL_PACKET_BYTES);
		}
	}
#endif
}

#ifdef PORT
/* Symmetric input-delay update: the host bumps it when stalls accumulate,
 * clients apply what the DELAY packet carries. Bounded both ways so a
 * forged or stale value can't widen the window past the rollback cap. */
void syNetPeerApplyInputDelay(u32 delay)
{
	if (delay < SYNETPEER_DEFAULT_INPUT_DELAY)
	{
		delay = SYNETPEER_DEFAULT_INPUT_DELAY;
	}
	if (delay > SYNETPEER_MAX_INPUT_DELAY)
	{
		delay = SYNETPEER_MAX_INPUT_DELAY;
	}
	if (delay == sSYNetPeerInputDelay)
	{
		return;
	}
	sSYNetPeerInputDelay = delay;
	syNetInputSetLocalDelay(delay);
	port_log("SSB64 NetPeer: input delay -> %u\n", delay);
}

void syNetPeerSendDelayPacket(u32 delay)
{
	u8 buffer[SYNETPEER_DELAY_PACKET_BYTES];
	s32 player;

	nbNetPacketBuildDelay(buffer, sSYNetPeerSessionID, delay);
	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		SYNetPeerRemote *remote = &sSYNetPeerRemotes[player];

		if ((remote->player == player) && (remote->dropped == FALSE))
		{
			syNetPeerSendBytesTo(remote, buffer, SYNETPEER_DELAY_PACKET_BYTES);
		}
	}
}

void syNetPeerHandleDelayPacket(const u8 *buffer, s32 size, SYNetPeerRemote *remote)
{
	u32 delay;

	if (nbNetPacketParseDelay(buffer, size, sSYNetPeerSessionID, &delay) != NB_NETPACKET_OK)
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	if (remote != NULL)
	{
		syNetPeerTouchRemote(remote);
	}
	/* Monotonic within a match: a lower value is a stale/stray packet and
	 * must not shrink the local delay. */
	if (delay > sSYNetPeerInputDelay)
	{
		syNetPeerApplyInputDelay(delay);
		/* Star topology: clients only link the host, so a bump must be
		 * re-flooded through it for every other client to converge. */
		if (sSYNetPeerDelayRepeatLeft < SYNETPEER_DELAY_REPEAT_FRAMES)
		{
			sSYNetPeerDelayRepeatLeft = SYNETPEER_DELAY_REPEAT_FRAMES;
		}
	}
}

/* Escalation, one step per check window: late remote inputs are the
 * mispredict source (each forces a prediction that may roll back), so the
 * peer that observes them broadcasts a DELAY bump — every peer applies
 * max(local, received), converging to the same value without a dedicated
 * authority. Delay is per-sender scheduling (inputs are tick-addressed),
 * so temporary asymmetry can't desync; the uniform value keeps input-lag
 * feel identical for everyone. Re-sent for a few frames to converge
 * through loss. */
void syNetPeerUpdateAdaptiveDelay(void)
{
	u32 tick;
	u32 late_delta;

	if (sSYNetPeerIsRollback == FALSE)
	{
		return;
	}
	tick = syNetInputGetTick();

	if (sSYNetPeerDelayRepeatLeft > 0)
	{
		syNetPeerSendDelayPacket(sSYNetPeerInputDelay);
		sSYNetPeerDelayRepeatLeft--;
	}
	if ((u32)(tick - sSYNetPeerDelayLastStallCheck) < SYNETPEER_DELAY_CHECK_FRAMES)
	{
		return;
	}
	sSYNetPeerDelayLastStallCheck = tick;

	late_delta = sSYNetPeerLateFrames - sSYNetPeerDelayCheckLateBase;
	if (((late_delta >= SYNETPEER_DELAY_STALL_TRIGGER) ||
	     ((sSYNetPeerStallFrames - sSYNetPeerDelayCheckStallBase) >= SYNETPEER_DELAY_STALL_TRIGGER)) &&
	    (sSYNetPeerInputDelay < SYNETPEER_MAX_INPUT_DELAY))
	{
		syNetPeerApplyInputDelay(sSYNetPeerInputDelay + 1);
		sSYNetPeerDelayRepeatLeft = SYNETPEER_DELAY_REPEAT_FRAMES;
		port_log("SSB64 NetPeer: sustained late inputs (%u in %u frames) — bumping delay to %u\n",
		         late_delta, SYNETPEER_DELAY_CHECK_FRAMES, sSYNetPeerInputDelay);
	}
	sSYNetPeerDelayCheckLateBase = sSYNetPeerLateFrames;
	sSYNetPeerDelayCheckStallBase = sSYNetPeerStallFrames;
}
#endif

#ifdef PORT
void syNetPeerSendMatchConfigPacket(void)
{
	u8 buffer[SYNETPEER_BOOTSTRAP_PACKET_BYTES];
	u8 *cursor = buffer;
	u32 checksum;

	nbNetPacketWritePrefix(&cursor, SYNETPEER_PACKET_MATCH_CONFIG, sSYNetPeerSessionID);
	syNetPeerWriteMetadata(&cursor, &sSYNetPeerBootstrapMetadata);
	checksum = syNetPeerChecksumBytes(buffer, cursor - buffer);
	syNetPeerWriteU32(&cursor, checksum);
	syNetPeerSendBytes(buffer, SYNETPEER_BOOTSTRAP_PACKET_BYTES);
}

void syNetPeerHandleControlPacket(const u8 *buffer, s32 size, SYNetPeerRemote *remote)
{
	s32 remote_player = (remote != NULL) ? remote->player : sSYNetPeerRemotePlayer;
	s32 packet_type;

	if (size != SYNETPEER_CONTROL_PACKET_BYTES)
	{
		return;
	}
	packet_type = nbNetPacketParseControl(buffer, size, sSYNetPeerSessionID);
	if (packet_type < 0)
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	if (remote != NULL)
	{
		syNetPeerTouchRemote(remote);
	}
	if (packet_type == SYNETPEER_PACKET_READY)
	{
		sSYNetPeerBootstrapPeerReady = TRUE;
	}
	else if (packet_type == SYNETPEER_PACKET_START)
	{
		sSYNetPeerBootstrapStartReceived = TRUE;
	}
	else if (packet_type == SYNETPEER_PACKET_BATTLE_READY)
	{
		if ((remote == NULL) || (remote->battle_ready == FALSE))
		{
			port_log("SSB64 NetPeer: received BATTLE_READY role=%s tick=%u local=%d remote=%d\n",
			         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
			         syNetInputGetTick(), sSYNetPeerLocalPlayer, remote_player);
		}
		if (remote != NULL)
		{
			remote->battle_ready = TRUE;
		}
		sSYNetPeerBattlePeerReady = TRUE;
	}
	else if (packet_type == SYNETPEER_PACKET_BATTLE_START)
	{
		if ((remote == NULL) || (remote->battle_start_received == FALSE))
		{
			port_log("SSB64 NetPeer: received BATTLE_START role=%s tick=%u local=%d remote=%d\n",
			         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
			         syNetInputGetTick(), sSYNetPeerLocalPlayer, remote_player);
		}
		if (remote != NULL)
		{
			remote->battle_start_received = TRUE;
		}
		sSYNetPeerBattleStartReceived = TRUE;
	}
}

void syNetPeerHandleMatchConfigPacket(const u8 *buffer, s32 size)
{
	const u8 *cursor = buffer;
	SYNetInputReplayMetadata metadata;
	u32 magic;
	u32 session_id;
	u32 checksum;
	u32 expected_checksum;
	u16 version;
	u16 packet_type;

	if (size != SYNETPEER_BOOTSTRAP_PACKET_BYTES)
	{
		return;
	}
	expected_checksum = syNetPeerChecksumBytes(buffer, SYNETPEER_BOOTSTRAP_PACKET_BYTES - 4);

	magic = syNetPeerReadU32(&cursor);
	version = syNetPeerReadU16(&cursor);
	packet_type = syNetPeerReadU16(&cursor);
	session_id = syNetPeerReadU32(&cursor);
	syNetPeerReadMetadata(&cursor, &metadata);
	checksum = syNetPeerReadU32(&cursor);

	if ((magic != SYNETPEER_MAGIC) || (version != SYNETPEER_VERSION) ||
		(packet_type != SYNETPEER_PACKET_MATCH_CONFIG) ||
		(session_id != sSYNetPeerSessionID) || (checksum != expected_checksum) ||
		(syNetPeerCheckMetadata(&metadata) == FALSE))
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	if (sSYNetPeerBootstrapIsHost == FALSE)
	{
		syNetPeerApplyBootstrapMetadata(&metadata);
	}
}

void syNetPeerHandleBootstrapPacket(const u8 *buffer, s32 size)
{
	if (size == SYNETPEER_CONTROL_PACKET_BYTES)
	{
		syNetPeerHandleControlPacket(buffer, size, syNetPeerGetRemoteByPlayer(sSYNetPeerRemotePlayer));
	}
	else if (size == SYNETPEER_BOOTSTRAP_PACKET_BYTES)
	{
		syNetPeerHandleMatchConfigPacket(buffer, size);
	}
	else sSYNetPeerPacketsDropped++;
}

void syNetPeerReceiveBootstrapPackets(void)
{
	u8 buffer[SYNETPEER_BOOTSTRAP_PACKET_BYTES];

	while (TRUE)
	{
		s32 size = port_udp_recv(buffer, sizeof(buffer));

		if (size <= 0)
		{
			if (size < 0)
			{
				sSYNetPeerPacketsDropped++;
			}
			break;
		}
		syNetPeerHandleBootstrapPacket(buffer, (s32)size);
	}
}

void syNetPeerRunBootstrap(void)
{
	s32 i;

	if ((sSYNetPeerBootstrapIsEnabled == FALSE) || (sSYNetPeerIsConfigured == FALSE))
	{
		return;
	}
	if (syNetPeerOpenSocket() == FALSE)
	{
		return;
	}
	sSYNetPeerIsActive = TRUE;

	if (sSYNetPeerBootstrapIsHost != FALSE)
	{
		syNetPeerMakeBootstrapMetadata(&sSYNetPeerBootstrapMetadata);
		syNetPeerApplyBootstrapMetadata(&sSYNetPeerBootstrapMetadata);

		for (i = 0; i < SYNETPEER_BOOTSTRAP_RETRY_COUNT; i++)
		{
			syNetPeerSendMatchConfigPacket();
			syNetPeerReceiveBootstrapPackets();

			if (sSYNetPeerBootstrapPeerReady != FALSE)
			{
				break;
			}
			syNetPeerSleepBootstrapRetry();
		}
		if (sSYNetPeerBootstrapPeerReady == FALSE)
		{
			port_log("SSB64 NetPeer: bootstrap host timed out waiting for READY\n");
			return;
		}
		for (i = 0; i < 30; i++)
		{
			syNetPeerSendControlPacket(SYNETPEER_PACKET_START);
			syNetPeerSleepBootstrapRetry();
		}
		port_log("SSB64 NetPeer: bootstrap host sent START stage=%u seed=%u\n",
		         sSYNetPeerBootstrapMetadata.stage_kind, sSYNetPeerBootstrapMetadata.rng_seed);
	}
	else
	{
		for (i = 0; i < SYNETPEER_BOOTSTRAP_RETRY_COUNT; i++)
		{
			syNetPeerReceiveBootstrapPackets();

			if (sSYNetPeerBootstrapMetadataApplied != FALSE)
			{
				syNetPeerSendControlPacket(SYNETPEER_PACKET_READY);
				break;
			}
			syNetPeerSleepBootstrapRetry();
		}
		if (sSYNetPeerBootstrapMetadataApplied == FALSE)
		{
			port_log("SSB64 NetPeer: bootstrap client timed out waiting for MATCH_CONFIG\n");
			return;
		}
		for (i = 0; i < SYNETPEER_BOOTSTRAP_RETRY_COUNT; i++)
		{
			syNetPeerSendControlPacket(SYNETPEER_PACKET_READY);
			syNetPeerReceiveBootstrapPackets();

			if (sSYNetPeerBootstrapStartReceived != FALSE)
			{
				break;
			}
			syNetPeerSleepBootstrapRetry();
		}
		if (sSYNetPeerBootstrapStartReceived == FALSE)
		{
			port_log("SSB64 NetPeer: bootstrap client timed out waiting for START\n");
			return;
		}
		port_log("SSB64 NetPeer: bootstrap client received START stage=%u seed=%u\n",
		         sSYNetPeerBootstrapMetadata.stage_kind, sSYNetPeerBootstrapMetadata.rng_seed);
	}
}
#endif

void syNetPeerInitDebugEnv(void)
{
#ifdef PORT
	char *netplay_env = getenv("SSB64_NETPLAY");
	char *local_player_env;
	char *remote_player_env;
	char *delay_env;
	char *session_env;
	char *bind_env;
	char *peer_env;
	char *bootstrap_env;
	char *bootstrap_host_env;
	char *bootstrap_seed_env;

	if ((netplay_env == NULL) || (atoi(netplay_env) == 0))
	{
		/* No env-driven session: leave state alone — a lobby match (or a
		 * spectator start) may have configured netplay before the run loop
		 * even reached this init. */
		return;
	}
	sSYNetPeerIsEnabled = FALSE;
	sSYNetPeerIsConfigured = FALSE;
	sSYNetPeerLocalPlayer = 0;
	sSYNetPeerRemotePlayer = 1;
	syNetPeerResetRemotes();
	sSYNetPeerInputDelay = SYNETPEER_DEFAULT_INPUT_DELAY;
	sSYNetPeerSessionID = SYNETPEER_DEFAULT_SESSION_ID;
	sSYNetPeerBootstrapIsEnabled = FALSE;
	sSYNetPeerBootstrapIsHost = FALSE;
	sSYNetPeerBootstrapMetadataApplied = FALSE;
	sSYNetPeerBootstrapPeerReady = FALSE;
	sSYNetPeerBootstrapStartReceived = FALSE;
	sSYNetPeerBootstrapSeed = SYNETPEER_DEFAULT_BOOTSTRAP_SEED;
	sSYNetPeerBattleBarrierEnabled = FALSE;
	sSYNetPeerBattleLocalReady = FALSE;
	sSYNetPeerBattlePeerReady = FALSE;
	sSYNetPeerBattleStartSent = FALSE;
	sSYNetPeerBattleStartReceived = FALSE;
	sSYNetPeerBattleBarrierReleased = TRUE;
	sSYNetPeerBattleBarrierWaitFrames = 0;
	sSYNetPeerBattleStartRepeatFrames = 0;
	sSYNetPeerExecutionHoldFrames = 0;
	sSYNetPeerExecutionBeginLogged = FALSE;

	sSYNetPeerIsEnabled = TRUE;

	local_player_env = getenv("SSB64_NETPLAY_LOCAL_PLAYER");
	remote_player_env = getenv("SSB64_NETPLAY_REMOTE_PLAYER");
	delay_env = getenv("SSB64_NETPLAY_DELAY");
	session_env = getenv("SSB64_NETPLAY_SESSION");
	bind_env = getenv("SSB64_NETPLAY_BIND");
	peer_env = getenv("SSB64_NETPLAY_PEER");
	bootstrap_env = getenv("SSB64_NETPLAY_BOOTSTRAP");
	bootstrap_host_env = getenv("SSB64_NETPLAY_HOST");
	bootstrap_seed_env = getenv("SSB64_NETPLAY_SEED");
	{
		char *rollback_env = getenv("SSB64_NETPLAY_ROLLBACK");

		sSYNetPeerIsRollback = ((rollback_env == NULL) || (atoi(rollback_env) != 0)) ? TRUE : FALSE;
#ifdef SSB64_TEST_HOOKS
		/* Packet fuzzing and mutants weaken validation on purpose; they are
		 * compiled in only for test builds (-DSSB64_TEST_HOOKS=ON) and must
		 * never ship. */
		{
			char *fuzz_env = getenv("SSB64_NETPLAY_FUZZ");
			const char *mutant_env = getenv("SSB64_NETPLAY_MUTANT");

			sSYNetPeerFuzzSeed = (fuzz_env != NULL) ? (u32)atoi(fuzz_env) : 0;

			if (mutant_env != NULL)
			{
				sSYNetPeerMutantBadSession = (strcmp(mutant_env, "accept_bad_session") == 0) ? TRUE : FALSE;
				sSYNetPeerMutantBadChecksum = (strcmp(mutant_env, "accept_bad_checksum") == 0) ? TRUE : FALSE;
				sSYNetPeerMutantNoForwardGuard = (strcmp(mutant_env, "no_forward_guard") == 0) ? TRUE : FALSE;
				port_log("SSB64 NetPeer: MUTANT %s active (test harness only)\n", mutant_env);
			}
		}
#endif
	}

	if (local_player_env != NULL)
	{
		sSYNetPeerLocalPlayer = atoi(local_player_env);
	}
	if (remote_player_env != NULL)
	{
		sSYNetPeerRemotePlayer = atoi(remote_player_env);
	}
	if (delay_env != NULL)
	{
		s32 delay = atoi(delay_env);

		if (delay >= 0)
		{
			sSYNetPeerInputDelay = delay;
		}
	}
	if (session_env != NULL)
	{
		s32 session_id = atoi(session_env);

		if (session_id > 0)
		{
			sSYNetPeerSessionID = session_id;
		}
	}
	if ((bootstrap_env != NULL) && (atoi(bootstrap_env) != 0))
	{
		sSYNetPeerBootstrapIsEnabled = TRUE;
	}
	if ((bootstrap_host_env != NULL) && (atoi(bootstrap_host_env) != 0))
	{
		sSYNetPeerBootstrapIsHost = TRUE;
	}
	if (bootstrap_seed_env != NULL)
	{
		s32 seed = atoi(bootstrap_seed_env);

		if (seed > 0)
		{
			sSYNetPeerBootstrapSeed = seed;
		}
	}
	if ((sSYNetPeerLocalPlayer < 0) || (sSYNetPeerLocalPlayer >= MAXCONTROLLERS) ||
		(sSYNetPeerRemotePlayer < 0) || (sSYNetPeerRemotePlayer >= MAXCONTROLLERS) ||
		(sSYNetPeerLocalPlayer == sSYNetPeerRemotePlayer))
	{
		port_log("SSB64 NetPeer: invalid players local=%d remote=%d\n",
		         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer);
		return;
	}
	if ((bind_env == NULL) || (peer_env == NULL))
	{
		port_log("SSB64 NetPeer: SSB64_NETPLAY_BIND and SSB64_NETPLAY_PEER (host:port) are required\n");
		return;
	}
	sSYNetPeerBindSpec = bind_env;
	sSYNetPeerPeerSpec = peer_env;
	syNetPeerAddRemote(sSYNetPeerRemotePlayer, -1);   // legacy single-peer socket
	sSYNetPeerIsConfigured = TRUE;
	port_log("SSB64 NetPeer: configured local=%d remote=%d delay=%u session=%u bootstrap=%d host=%d seed=%u bind=%s peer=%s\n",
	         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer, sSYNetPeerInputDelay,
	         sSYNetPeerSessionID, sSYNetPeerBootstrapIsEnabled, sSYNetPeerBootstrapIsHost,
	         sSYNetPeerBootstrapSeed, bind_env, peer_env);
	syNetPeerRunBootstrap();
#endif
}

void syNetPeerStartVSSession(void)
{
#ifdef PORT
	if ((sSYNetPeerIsEnabled == FALSE) || (sSYNetPeerIsConfigured == FALSE))
	{
		port_log("SSB64 NetPeer: VS session start SKIPPED enabled=%d configured=%d spec=%d\n",
		         sSYNetPeerIsEnabled, sSYNetPeerIsConfigured, sSYNetPeerIsSpectator);
		return;
	}
	if (syNetPeerOpenSocket() == FALSE)
	{
		port_log("SSB64 NetPeer: VS session start SKIPPED socket open failed spec=%d\n",
		         sSYNetPeerIsSpectator);
		return;
	}
	sSYNetPeerHighestRemoteTick = 0;
	sSYNetPeerPacketsSent = 0;
	sSYNetPeerPacketsReceived = 0;
	sSYNetPeerPacketsDropped = 0;
	sSYNetPeerFramesStaged = 0;
	sSYNetPeerLateFrames = 0;
	sSYNetPeerInputChecksum = 2166136261U;
	sSYNetPeerLastLogTick = 0;
	sSYNetPeerSendSeq = 0;
	sSYNetPeerRecvSeqInitialized = FALSE;
	sSYNetPeerRecvSeqHighWater = 0;
	sSYNetPeerSeqGaps = 0;
	sSYNetPeerSeqDuplicates = 0;
	sSYNetPeerSeqOutOfOrder = 0;
	sSYNetPeerLastPeerAckTick = 0;
	sSYNetPeerLastPacketTicksValid = FALSE;
	sSYNetPeerIsActive = TRUE;
	sSYNetPeerBattleBarrierEnabled = sSYNetPeerBootstrapIsEnabled;
	sSYNetPeerBattleLocalReady = sSYNetPeerBattleBarrierEnabled;
	sSYNetPeerBattlePeerReady = FALSE;
	sSYNetPeerBattleStartSent = FALSE;
	sSYNetPeerBattleStartReceived = FALSE;
	sSYNetPeerBattleBarrierReleased = (sSYNetPeerBattleBarrierEnabled == FALSE) ? TRUE : FALSE;
	sSYNetPeerBattleBarrierWaitFrames = 0;
	sSYNetPeerBattleStartRepeatFrames = 0;
	sSYNetPeerExecutionHoldFrames = 0;
	sSYNetPeerExecutionBeginLogged = (sSYNetPeerBattleBarrierEnabled == FALSE) ? TRUE : FALSE;

	{
		s32 slot;

		/* Slots nobody plays must read identically on every peer: the
		 * Saved source with no saved frames resolves to neutral input.
		 * Left as Local they'd read whatever extra controllers happen to
		 * be plugged into each PC (a pause or reset combo on one peer only). */
		for (slot = 0; slot < MAXCONTROLLERS; slot++)
		{
			syNetInputSetSlotSource(slot, nSYNetInputSourceSaved);
		}
		syNetInputSetSlotSource(sSYNetPeerLocalPlayer, nSYNetInputSourceLocal);
		for (slot = 0; slot < MAXCONTROLLERS; slot++)
		{
			if (sSYNetPeerRemotes[slot].player == slot)
			{
				syNetInputSetSlotSource(slot, nSYNetInputSourceRemotePredicted);
				syNetInputPreconfirmRemote(slot, sSYNetPeerInputDelay);
			}
		}
	}
	syNetInputSetLocalDelay(sSYNetPeerInputDelay);
	syNetInputSetFuzzSeed(sSYNetPeerFuzzSeed);
	syNetInputSetLocalDevice(sSYNetPeerLocalPlayer, (sSYNetPeerIsSocketExternal != FALSE) ? 0 : sSYNetPeerLocalPlayer);
	sSYNetPeerFrameReady = FALSE;
	sSYNetPeerStallFrames = 0;
	sSYNetPeerDelayCheckStallBase = 0;
	sSYNetPeerDelayCheckLateBase = 0;
	sSYNetPeerDelayLastStallCheck = syNetInputGetTick();
	sSYNetPeerDelayRepeatLeft = 0;
	sSYNetPeerLockstepStallFrames = 0;
	sSYNetPeerRemoteAdvantage = 0;
	sSYNetPeerTimeSyncStallFrames = 0;
	sSYNetPeerFramesSinceTimeSync = 0;

	port_log("SSB64 NetPeer: VS session start role=%s local=%d remote=%d delay=%u barrier=%d rollback=%d fuzz=%u tick=%u\n",
	         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
	         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer, sSYNetPeerInputDelay,
	         sSYNetPeerBattleBarrierEnabled, sSYNetPeerIsRollback, sSYNetPeerFuzzSeed, syNetInputGetTick());
#endif
}

static sb32 syNetPeerCheckBarrierReleased(void)
{
	if ((sSYNetPeerIsEnabled == FALSE) || (sSYNetPeerBootstrapIsEnabled == FALSE) ||
		(sSYNetPeerBattleBarrierEnabled == FALSE))
	{
		return TRUE;
	}
	return sSYNetPeerBattleBarrierReleased;
}

/* May the tick about to be published run now? Rollback-safe ticks may run
 * up to SYNETPEER_MAX_PREDICTION ticks ahead of the confirmed remote input;
 * ticks a snapshot can't capture (countdown/announcer threads) run in
 * lockstep, i.e. only with the remote input for that tick confirmed. */
static sb32 syNetPeerCheckPacing(void)
{
	u32 tick = syNetInputGetTick();
	u32 confirmed = syNetPeerGetRemoteConfirmedTick();
	s32 remote_advantage = syNetPeerGetWorstRemoteAdvantage();

	if (sSYNetPeerIsSpectator != FALSE)
	{
		/* A spectator never predicts: a tick may run only once every
		 * player's input for it has actually arrived. */
		if (tick < confirmed)
		{
			return TRUE;
		}
		sSYNetPeerLockstepStallFrames++;
		return FALSE;
	}
	if (sSYNetPeerIsRollback == FALSE)
	{
		return TRUE;
	}
	if (syNetRollbackCheckCanSave() != FALSE)
	{
		s32 advantage = (s32)tick - (s32)confirmed;

		if (tick >= confirmed + SYNETPEER_MAX_PREDICTION)
		{
			sSYNetPeerStallFrames++;
			return FALSE;
		}
		if (((advantage - remote_advantage) >= SYNETPEER_TIMESYNC_THRESHOLD) &&
		    (sSYNetPeerFramesSinceTimeSync >= SYNETPEER_TIMESYNC_INTERVAL))
		{
			sSYNetPeerFramesSinceTimeSync = 0;
			sSYNetPeerTimeSyncStallFrames++;
			return FALSE;
		}
		return TRUE;
	}
	if (tick < confirmed)
	{
		return TRUE;
	}
	sSYNetPeerLockstepStallFrames++;
	return FALSE;
}

/* Called by syNetInputFuncRead before it resolves the tick's inputs: drain
 * the socket, then decide - once for the whole frame - whether this tick
 * runs. Input publishing, the tick advance and the battle update all read
 * the same answer, so they can never disagree within a frame. */
void syNetPeerBeginFrame(void)
{
	if (sSYNetPeerIsActive == FALSE)
	{
		sSYNetPeerFrameReady = TRUE;
		return;
	}
	syNetPeerReceiveRemoteInput();
	syNetPeerUpdateStartBarrier();
	sSYNetPeerFramesSinceTimeSync++;

	sSYNetPeerFrameReady = ((syNetPeerCheckBarrierReleased() != FALSE) && (syNetPeerCheckPacing() != FALSE)) ? TRUE : FALSE;
}

sb32 syNetPeerCheckBattleExecutionReady(void)
{
	if (sSYNetPeerIsActive == FALSE)
	{
		return TRUE;
	}
	return sSYNetPeerFrameReady;
}

sb32 syNetPeerIsRollbackActive(void)
{
	return ((sSYNetPeerIsActive != FALSE) && (sSYNetPeerIsRollback != FALSE)) ? TRUE : FALSE;
}

s32 syNetPeerGetRemotePlayer(void)
{
	return sSYNetPeerRemotePlayer;
}

sb32 syNetPeerCheckStartBarrierReleased(void)
{
	return syNetPeerCheckBattleExecutionReady();
}

void syNetPeerBuildPacket(u8 *buffer, u32 *out_size, const SYNetPeerRemote *remote)
{
	NBNetPacketInput pkt;
	SYNetInputFrame history_frame;
	u32 latest_tick;
	u32 ack_tick = syNetPeerGetAckTick((SYNetPeerRemote *)remote);
	s32 frame_count = 0;
	s32 back;

	memset(&pkt, 0, sizeof(pkt));

	/* The newest sample was scheduled for (tick being published) + delay; the
	 * packet repeats the last 16 scheduled frames for loss tolerance. */
	latest_tick = syNetInputGetTick() + sSYNetPeerInputDelay;

	for (back = SYNETPEER_MAX_PACKET_FRAMES - 1; back >= 0; back--)
	{
		if ((latest_tick >= (u32)back) &&
			(syNetInputGetLocalScheduledFrame(sSYNetPeerLocalPlayer, latest_tick - back, &history_frame) != FALSE))
		{
			pkt.frames[frame_count].tick = history_frame.tick;
			pkt.frames[frame_count].buttons = history_frame.buttons;
			pkt.frames[frame_count].stick_x = history_frame.stick_x;
			pkt.frames[frame_count].stick_y = history_frame.stick_y;
			frame_count++;
		}
	}
	{
		s32 remote_slot = (remote != NULL) ? remote->player : sSYNetPeerRemotePlayer;
		s32 advantage = (s32)syNetInputGetTick() - (s32)syNetInputGetRemoteNextUnconfirmed(remote_slot);

		advantage = (advantage < -32768) ? -32768 : (advantage > 32767) ? 32767 : advantage;
		pkt.remote_advantage = (s16)advantage;
	}
	pkt.session_id = sSYNetPeerSessionID;
	pkt.ack_tick = ack_tick;
	pkt.seq = sSYNetPeerSendSeq;
	pkt.player = (u8)sSYNetPeerLocalPlayer;
	pkt.frame_count = (u8)frame_count;
	pkt.local_player = (u8)sSYNetPeerLocalPlayer;
	pkt.remote_player = (u8)((remote != NULL) ? remote->player : sSYNetPeerRemotePlayer);
	if (syNetRollbackGetLatestConfirmedHash(&pkt.check_tick, &pkt.check_hash) == FALSE)
	{
		pkt.check_tick = NB_NETPACKET_NO_CHECK_TICK;
		pkt.check_hash = 0;
	}
	/* Byte-wise over the whole datagram so the advantage and sync-check
	 * trailer are authenticated too. */
	*out_size = nbNetPacketBuildInput(buffer, &pkt);
}

void syNetPeerSendLocalInput(void)
{
#ifdef PORT
	u8 buffer[SYNETPEER_PACKET_BYTES];
	u32 size;
	s32 player;
	sb32 sent = FALSE;

	/* A spectator watches only: it never publishes an input stream. */
	if (sSYNetPeerIsSpectator != FALSE)
	{
		return;
	}
	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		SYNetPeerRemote *remote = &sSYNetPeerRemotes[player];

		/* link == -1 remotes are reached through the star host's forwarding;
		 * nothing is sent to them directly. */
		if ((remote->player != player) || (remote->dropped != FALSE) ||
			((sSYNetPeerIsSocketExternal != FALSE) && (remote->link < 0)))
		{
			continue;
		}
		syNetPeerBuildPacket(buffer, &size, remote);

		if (size == 0)
		{
			continue;
		}
		if (((sSYNetPeerIsSocketExternal != FALSE) &&
		     (port_udp_link_send(remote->link, buffer, (s32)size) == (s32)size)) ||
		    ((sSYNetPeerIsSocketExternal == FALSE) &&
		     (port_udp_send(buffer, (s32)size) == (s32)size)))
		{
			sent = TRUE;
		}
	}
	/* The host's own stream also goes to every spectator; client packets
	 * reach watchers through syNetPeerForwardPacket. */
	if ((sSYNetPeerBootstrapIsHost != FALSE) && (sSYNetPeerSpecLinkCount > 0))
	{
		s32 spec;

		syNetPeerBuildPacket(buffer, &size, NULL);
		if (size != 0)
		{
			for (spec = 0; spec < sSYNetPeerSpecLinkCount; spec++)
			{
				syNetPeerSendForwardPacket(sSYNetPeerSpecLinks[spec], sSYNetPeerLocalPlayer, buffer, (s32)size);
			}
		}
	}
	if (sent != FALSE)
	{
		sSYNetPeerPacketsSent++;
		sSYNetPeerSendSeq++;
	}
#endif
}

/* FORWARD-wrap an INPUT packet authored by <src_player> and send it on
 * <link> - used to reach star clients (which have no link to the sender)
 * and to feed spectators (which have no player remotes at all). */
static void syNetPeerSendForwardPacket(s32 link, s32 src_player, const u8 *buffer, s32 size)
{
#ifdef PORT
	u8 wrapper[SYNETPEER_FORWARD_PACKET_BYTES];
	u32 wrapper_size;
	static s32 sFwdSendDiag = 20;

	if (sFwdSendDiag-- > 0)
	{
		port_log("SSB64 NetPeer: fwd send link=%d src=%d size=%d speclinks=%d remotes=%d tick=%u\n",
		         link, src_player, size, sSYNetPeerSpecLinkCount, sSYNetPeerRemoteCount,
		         syNetInputGetTick());
	}
	wrapper_size = nbNetPacketBuildForward(wrapper, sizeof(wrapper), buffer, (u32)size,
										 (u8)src_player, sSYNetPeerSessionID);
	if (wrapper_size == 0)
	{
		return;
	}
	port_udp_link_send(link, wrapper, (s32)wrapper_size);
#endif
}

/* The star host relays a client's datagram to the other clients so every
 * participant sees every input stream without client<->client NAT punching.
 * Spectator links get the same treatment - they just are not remotes. */
void syNetPeerForwardPacket(const SYNetPeerRemote *from, const u8 *buffer, s32 size)
{
#ifdef PORT
	s32 player;
	s32 spec;

	if ((sSYNetPeerIsSocketExternal == FALSE) || (sSYNetPeerBootstrapIsHost == FALSE) ||
		(size <= 0))
	{
		return;
	}
	/* RemoteCount < 2 skips only the player fan-out: with a single client
	 * there is nobody to relay to, but spec links still need the stream. */
	if (sSYNetPeerRemoteCount >= 2)
	{
		for (player = 0; player < MAXCONTROLLERS; player++)
		{
			SYNetPeerRemote *remote = &sSYNetPeerRemotes[player];

			if ((remote->player == player) && (remote->dropped == FALSE) &&
				(remote->link >= 0) && (remote != from))
			{
				syNetPeerSendForwardPacket(remote->link, from->player, buffer, size);
			}
		}
	}
	for (spec = 0; spec < sSYNetPeerSpecLinkCount; spec++)
	{
		syNetPeerSendForwardPacket(sSYNetPeerSpecLinks[spec], from->player, buffer, size);
	}
#endif
}

/* src is the remote this datagram came from (or, for forwarded packets, the
 * client the star host relays). forwarded packets skip the remote_player
 * check because the inner datagram was addressed to the host. */
void syNetPeerHandlePacket(const u8 *buffer, s32 size, SYNetPeerRemote *remote, sb32 forwarded)
{
	NBNetPacketInput pkt;
	u32 ack_tick;
	u32 packet_seq;
	u8 frame_count;
	u32 check_tick;
	u32 check_hash;
	u32 current_tick = syNetInputGetTick();
	s32 src_player = (remote != NULL) ? remote->player : sSYNetPeerRemotePlayer;
	s32 i;

#ifdef PORT
	if (size == SYNETPEER_CONTROL_PACKET_BYTES)
	{
		syNetPeerHandleControlPacket(buffer, size, remote);
		return;
	}
	if (size == SYNETPEER_DELAY_PACKET_BYTES)
	{
		syNetPeerHandleDelayPacket(buffer, size, remote);
		return;
	}
#endif
	if (size != SYNETPEER_PACKET_BYTES)
	{
		static s32 sSizeDropDiag = 8;

		sSYNetPeerPacketsDropped++;
		if (sSizeDropDiag-- > 0)
		{
			port_log("SSB64 NetPeer: drop size=%d fwd=%d head=%02X%02X tick=%u\n",
					 size, forwarded, buffer[0], buffer[1], syNetInputGetTick());
		}
		return;
	}
	{
		NBNetPacketContext ctx;
		NBNetPacketStatus parsed;

		ctx.session_id = sSYNetPeerSessionID;
		ctx.src_player = src_player;
		ctx.local_player = sSYNetPeerLocalPlayer;
		ctx.forwarded = (forwarded != FALSE) ? 1 : 0;
		ctx.ignore_session = (sSYNetPeerMutantBadSession != FALSE) ? 1 : 0;
		ctx.ignore_checksum = (sSYNetPeerMutantBadChecksum != FALSE) ? 1 : 0;
		parsed = nbNetPacketParseInput(buffer, size, &ctx, &pkt);
		if (parsed != NB_NETPACKET_OK)
		{
			static s32 sParseDropDiag = 8;

			sSYNetPeerPacketsDropped++;
			if (sParseDropDiag-- > 0)
			{
				port_log("SSB64 NetPeer: drop parse=%d src=%d fwd=%d tick=%u head=%02X%02X\n",
						 (s32)parsed, src_player, forwarded, syNetInputGetTick(),
						 buffer[0], buffer[1]);
			}
			return;
		}
	}
	ack_tick = pkt.ack_tick;
	packet_seq = pkt.seq;
	frame_count = pkt.frame_count;
	check_tick = pkt.check_tick;
	check_hash = pkt.check_hash;

	sSYNetPeerPacketsReceived++;
	if (remote != NULL)
	{
		remote->remote_advantage = pkt.remote_advantage;
		syNetPeerTouchRemote(remote);
	}
	sSYNetPeerRemoteAdvantage = pkt.remote_advantage;

	if (check_tick != 0xFFFFFFFFU)
	{
		syNetRollbackCheckRemoteConfirmedHash(check_tick, check_hash);
	}

	if (remote != NULL)
	{
		if (remote->recv_seq_initialized == FALSE)
		{
			remote->recv_seq_highwater = packet_seq;
			remote->recv_seq_initialized = TRUE;
		}
		else
		{
			if (packet_seq > remote->recv_seq_highwater)
			{
				remote->seq_gaps += packet_seq - remote->recv_seq_highwater - 1U;
				sSYNetPeerSeqGaps += packet_seq - remote->recv_seq_highwater - 1U;
				remote->recv_seq_highwater = packet_seq;
			}
			else if (packet_seq == remote->recv_seq_highwater)
			{
				remote->seq_duplicates++;
				sSYNetPeerSeqDuplicates++;
			}
			else
			{
				remote->seq_out_of_order++;
				sSYNetPeerSeqOutOfOrder++;
			}
		}
		remote->last_peer_ack_tick = ack_tick;
	}
	sSYNetPeerLastPeerAckTick = ack_tick;

	if (frame_count > 0)
	{
		/* -1 = unchecked; 0 = off; set SSB64_NETPLAY_PKTDIAG to dump frame windows. */
		static s32 sFrameWindowDiag = -1;
		u32 oldest_tick_bundle = pkt.frames[0].tick;
		u32 newest_tick_bundle = pkt.frames[0].tick;

		for (i = 1; i < frame_count; i++)
		{
			if (pkt.frames[i].tick < oldest_tick_bundle)
			{
				oldest_tick_bundle = pkt.frames[i].tick;
			}
			if (pkt.frames[i].tick > newest_tick_bundle)
			{
				newest_tick_bundle = pkt.frames[i].tick;
			}
		}
		if (remote != NULL)
		{
			remote->last_packet_oldest_tick = oldest_tick_bundle;
			remote->last_packet_newest_tick = newest_tick_bundle;
			remote->last_packet_ticks_valid = TRUE;
		}
		sSYNetPeerLastPacketOldestTick = oldest_tick_bundle;
		sSYNetPeerLastPacketNewestTick = newest_tick_bundle;
		sSYNetPeerLastPacketTicksValid = TRUE;
		if (sFrameWindowDiag < 0)
		{
			sFrameWindowDiag = (getenv("SSB64_NETPLAY_PKTDIAG") != NULL) ? 400 : 0;
		}
		if ((sFrameWindowDiag != 0) && (sFrameWindowDiag-- <= 0))
		{
			sFrameWindowDiag = 400;
			port_log("SSB64 NetPeer: pktdiag src=%d fwd=%d frames=[%u,%u] unconf=%u tick=%u\n",
					 src_player, forwarded, oldest_tick_bundle, newest_tick_bundle,
					 syNetInputGetRemoteNextUnconfirmed(src_player), syNetInputGetTick());
		}
	}
	else
	{
		if (remote != NULL)
		{
			remote->last_packet_ticks_valid = FALSE;
		}
		sSYNetPeerLastPacketTicksValid = FALSE;
	}

	for (i = 0; i < frame_count; i++)
	{
		u32 *highest = (remote != NULL) ? &remote->highest_remote_tick : &sSYNetPeerHighestRemoteTick;
		sb32 is_new_remote_tick = (pkt.frames[i].tick > *highest) ? TRUE : FALSE;

		if ((is_new_remote_tick != FALSE) && (pkt.frames[i].tick < current_tick))
		{
			sSYNetPeerLateFrames++;
		}
		if (is_new_remote_tick != FALSE)
		{
			*highest = pkt.frames[i].tick;
		}
		syNetInputSetRemoteInput
		(
			src_player,
			pkt.frames[i].tick,
			pkt.frames[i].buttons,
			pkt.frames[i].stick_x,
			pkt.frames[i].stick_y
		);
		sSYNetPeerFramesStaged++;
		sSYNetPeerInputChecksum = syNetPeerChecksumAccumulateU32(sSYNetPeerInputChecksum, src_player);
		sSYNetPeerInputChecksum = syNetPeerChecksumAccumulateFrame(sSYNetPeerInputChecksum, &pkt.frames[i]);
	}
	if ((remote != NULL) && (forwarded == FALSE))
	{
		syNetPeerForwardPacket(remote, buffer, size);
	}
}

/* Unwrap a FORWARD datagram (clients only): the inner packet is handled as
 * if it had arrived from its original sender. `link` is the transport link
 * it arrived on, which must be our link to the star host — a FORWARD on any
 * other link, or any FORWARD at all on the host itself, is an injection
 * attempt (the host receives client datagrams directly, never wrapped). */
void syNetPeerHandleForwardPacket(const u8 *buffer, s32 size, s32 link)
{
#ifdef PORT
	const u8 *inner;
	u32 inner_size;
	u8 src_player;
	SYNetPeerRemote *remote;
	SYNetPeerRemote *host;

	if (size < SYNETPEER_FORWARD_HEADER_BYTES + 4)
	{
		return;
	}
	if ((sSYNetPeerBootstrapIsHost != FALSE) && (sSYNetPeerMutantNoForwardGuard == FALSE))
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	host = syNetPeerGetRemoteByPlayer(0);
	/* A spectator has no direct link to the host's player remote (all its
	 * remotes are link -1 and every datagram arrives FORWARD-wrapped on its
	 * own relay link), so the host-link check does not apply. */
	if ((sSYNetPeerMutantNoForwardGuard == FALSE) && (sSYNetPeerIsSpectator == FALSE) &&
		((host == NULL) || (link != host->link)))
	{
		sSYNetPeerPacketsDropped++;
		return;
	}
	{
		NBNetPacketContext ctx;

		memset(&ctx, 0, sizeof(ctx));
		ctx.session_id = sSYNetPeerSessionID;
		ctx.local_player = sSYNetPeerLocalPlayer;
		if (nbNetPacketParseForward(buffer, size, &ctx, &src_player,
								  &inner, &inner_size) != NB_NETPACKET_OK)
		{
			sSYNetPeerPacketsDropped++;
			return;
		}
	}
	remote = syNetPeerGetRemoteByPlayer(src_player);
	/* The wrapped sender must be a remote we have no direct link to — on a
	 * client that is every non-host participant (link == -1). A FORWARD
	 * claiming to be the host, or anyone we could hear from directly, is
	 * spoofed. */
	if ((remote == NULL) || ((remote->link != -1) && (sSYNetPeerMutantNoForwardGuard == FALSE)))
	{
		static s32 sFwdDropDiag = 8;

		sSYNetPeerPacketsDropped++;
		if (sFwdDropDiag-- > 0)
		{
			port_log("SSB64 NetPeer: drop fwd-guard src=%d remote=%p rlink=%d link=%d isize=%u tick=%u env=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
					 src_player, (void *)remote,
					 (remote != NULL) ? remote->link : -9, link, inner_size,
					 syNetInputGetTick(),
					 buffer[0], buffer[1], buffer[2], buffer[3],
					 buffer[4], buffer[5], buffer[6], buffer[7],
					 buffer[8], buffer[9], buffer[10], buffer[11],
					 buffer[12], buffer[13], buffer[14], buffer[15],
					 buffer[16], buffer[17], buffer[18], buffer[19]);
		}
		return;
	}
	syNetPeerHandlePacket(inner, (s32)inner_size, remote, TRUE);
#endif
}

/* FORWARD datagrams are INPUT packets wrapped by the star host; they are
 * bigger than a bare INPUT packet, so size alone doesn't identify them.
 * Check the type field of the fixed header instead. */
sb32 syNetPeerCheckForwardPacket(const u8 *buffer, s32 size); /* fwd */

#ifdef PORT
static sb32 sSYNetPeerFuzzIsMutantProbe;

/* Packet-fuzz helpers (SSB64_NETPLAY_PACKETFUZZ / mutant probes). */

static SYNetPeerRemote *syNetPeerFuzzPickRemote(s32 iter)
{
	s32 fsrc;

	if (sSYNetPeerIsSocketExternal == FALSE)
	{
		return syNetPeerGetRemoteByPlayer(sSYNetPeerRemotePlayer);
	}
	fsrc = iter % MAXCONTROLLERS;
	if (fsrc == sSYNetPeerLocalPlayer)
	{
		fsrc = (fsrc + 1) % MAXCONTROLLERS;
	}
	return syNetPeerGetRemoteByPlayer(fsrc);
}

/* Writes a wire-valid INPUT packet for <remote>: real header, plausible
 * future frame ticks (so an accepted packet lands in the staging window and
 * diverges), correct trailing checksum. Passing a session other than
 * sSYNetPeerSessionID produces the cross-session-replay vector. */
static void syNetPeerFuzzFillInputPacket(u8 *buf, u32 session, SYNetPeerRemote *remote, s32 iter, u32 seed)
{
	u8 *w = buf;
	SYNetPeerPacketFrame frames[SYNETPEER_MAX_PACKET_FRAMES];
	u8 fcount = (u8)(1 + (iter % SYNETPEER_MAX_PACKET_FRAMES));
	u32 ftick = syNetInputGetTick() + 1;
	u32 fsum;
	s32 i;

	syNetPeerWriteU32(&w, SYNETPEER_MAGIC);
	syNetPeerWriteU16(&w, SYNETPEER_VERSION);
	syNetPeerWriteU16(&w, (u16)(seed & 0x7FFF));
	syNetPeerWriteU32(&w, session);
	syNetPeerWriteU32(&w, ftick);
	syNetPeerWriteU32(&w, (u32)iter);
	syNetPeerWriteU8(&w, (u8)remote->player);
	syNetPeerWriteU8(&w, fcount);
	syNetPeerWriteU8(&w, (u8)remote->player);
	syNetPeerWriteU8(&w, (u8)sSYNetPeerLocalPlayer);
	memset(frames, 0, sizeof(frames));
	for (i = 0; i < SYNETPEER_MAX_PACKET_FRAMES; i++)
	{
		frames[i].tick = ftick + (u32)i;
		frames[i].buttons = (u16)(seed >> (i % 16));
		frames[i].stick_x = (s8)(iter + i);
		frames[i].stick_y = (s8)(iter - i);
		syNetPeerWriteU32(&w, frames[i].tick);
		syNetPeerWriteU16(&w, frames[i].buttons);
		syNetPeerWriteU8(&w, (u8)frames[i].stick_x);
		syNetPeerWriteU8(&w, (u8)frames[i].stick_y);
	}
	syNetPeerWriteU32(&w, 0xFFFFFFFFU);
	syNetPeerWriteU32(&w, 0);
	fsum = syNetPeerChecksumBytes(buf, SYNETPEER_PACKET_BYTES - 4);
	syNetPeerWriteU32(&w, fsum);
}
#endif

sb32 syNetPeerCheckForwardPacket(const u8 *buffer, s32 size)
{
	return (nbNetPacketIsForward(buffer, size) != 0) ? TRUE : FALSE;
}

void syNetPeerReceiveRemoteInput(void)
{
#ifdef PORT
	u8 buffer[SYNETPEER_FORWARD_PACKET_BYTES];

	/* SSB64_NETPLAY_PACKETFUZZ=<count>: inject <count> seeded pseudo-random
	 * datagrams through the real handlers before each drain. Tiered
	 * validity so every defense depth is exercised:
	 *   tier 0: fully random              — magic gate
	 *   tier 1: + magic/version           — header gate
	 *   tier 2: + session                 — session gate
	 *   tier 3: + player fields + checksum — full accept path, frames decode
	 *   tier 4: like 3 but wrong session (checksum signed with it) — this is
	 *           exactly the cross-session replay an attacker would send;
	 *           must be dropped unless MUTANT accept_bad_session is active
	 *   tier 5: like 3 but checksum field off by one — must be dropped
	 *           unless MUTANT accept_bad_checksum is active
	 * Odd iterations are wrapped as FORWARD datagrams (relay path).
	 * When a MUTANT is active and no fuzz count was given, a small probe
	 * stream of the matching vector runs anyway so the mutation has an
	 * observable effect (desync) rather than silently passing.
	 * Oracle: no crash/OOB; accepted garbage must trip the desync
	 * detector via the tick-hash check. Test harness only. */
	if (sSYNetPeerPacketFuzzLeft == -2)
	{
#ifdef SSB64_TEST_HOOKS
		const char *fuzz_env = getenv("SSB64_NETPLAY_PACKETFUZZ");
		sSYNetPeerPacketFuzzLeft = (fuzz_env != NULL) ? atoi(fuzz_env) : 0;
#else
		sSYNetPeerPacketFuzzLeft = 0;
#endif
		if ((sSYNetPeerPacketFuzzLeft == 0) &&
			((sSYNetPeerMutantBadSession != FALSE) || (sSYNetPeerMutantBadChecksum != FALSE) ||
			 (sSYNetPeerMutantNoForwardGuard != FALSE)))
		{
			sSYNetPeerPacketFuzzLeft = 300;   /* probe packets, mutant-shaped */
			sSYNetPeerFuzzIsMutantProbe = TRUE;
		}
	}
	while (sSYNetPeerPacketFuzzLeft > 0)
	{
		u8 fuzzbuf[SYNETPEER_FORWARD_PACKET_BYTES];
		u32 fseed = sSYNetPeerFuzzSeed ^ (0x9E3779B9U * (u32)sSYNetPeerPacketFuzzLeft);
		s32 fiter = sSYNetPeerPacketFuzzLeft;
		s32 tier;
		s32 fsize;
		s32 fi;
		SYNetPeerRemote *fremote;

		sSYNetPeerPacketFuzzLeft--;
		if (sSYNetPeerFuzzIsMutantProbe != FALSE)
		{
			/* Mutant probes: the exact datagram class the removed check is
			 * supposed to reject — every iteration. */
			tier = (sSYNetPeerMutantBadSession != FALSE) ? 4 :
			       (sSYNetPeerMutantBadChecksum != FALSE) ? 5 : 6;
		}
		else
		{
			tier = fiter % 6;
		}
		for (fi = 0; fi < (s32)sizeof(fuzzbuf); fi++)
		{
			fseed = fseed * 1664525u + 1013904223u;
			fuzzbuf[fi] = (u8)(fseed >> 24);
		}
		if (tier == 6)
		{
			/* no_forward_guard probe: a FORWARD datagram that did NOT arrive
			 * via the host link, wrapping a fully valid INPUT packet. Must
			 * be dropped unless the mutant removed the topology check. */
			u8 *w;
			SYNetPeerRemote *host = syNetPeerGetRemoteByPlayer(0);
			SYNetPeerRemote *victim;
			s32 badlink = (host != NULL) ? host->link + 1 : 0;
			u32 fout;

			victim = (sSYNetPeerLocalPlayer != 1) ? syNetPeerGetRemoteByPlayer(1)
			                                    : syNetPeerGetRemoteByPlayer(2);
			if (victim == NULL)
			{
				continue;
			}
			w = fuzzbuf;
			syNetPeerWriteU32(&w, SYNETPEER_MAGIC);
			syNetPeerWriteU16(&w, SYNETPEER_VERSION);
			syNetPeerWriteU16(&w, SYNETPEER_PACKET_FORWARD);
			syNetPeerWriteU32(&w, sSYNetPeerSessionID);
			syNetPeerWriteU8(&w, (u8)victim->player);
			syNetPeerFuzzFillInputPacket(w, sSYNetPeerSessionID, victim, fiter, fseed);
			fout = syNetPeerChecksumBytes(fuzzbuf, SYNETPEER_FORWARD_HEADER_BYTES + SYNETPEER_PACKET_BYTES);
			w = fuzzbuf + SYNETPEER_FORWARD_HEADER_BYTES + SYNETPEER_PACKET_BYTES;
			syNetPeerWriteU32(&w, fout);
			syNetPeerHandleForwardPacket(fuzzbuf, SYNETPEER_FORWARD_PACKET_BYTES, badlink);
			continue;
		}
		if ((sSYNetPeerIsSocketExternal != FALSE) && ((fiter & 1) != 0))
		{
			/* FORWARD wrapper: real outer fields (passes CheckForward),
			 * random inner packet. Tier >= 3 also fixes the trailing
			 * checksum so the inner parse runs. */
			u8 *w = fuzzbuf;
			SYNetPeerRemote *host = syNetPeerGetRemoteByPlayer(0);

			syNetPeerWriteU32(&w, SYNETPEER_MAGIC);
			syNetPeerWriteU16(&w, SYNETPEER_VERSION);
			syNetPeerWriteU16(&w, SYNETPEER_PACKET_FORWARD);
			syNetPeerWriteU32(&w, (tier >= 2) ? sSYNetPeerSessionID : fseed);
			syNetPeerWriteU8(&w, (u8)(fiter % MAXCONTROLLERS));
			fsize = SYNETPEER_FORWARD_HEADER_BYTES + 4 + ((fiter * 37) % (s32)(sizeof(fuzzbuf) - SYNETPEER_FORWARD_HEADER_BYTES - 8)) + 4;
			if (tier >= 3)
			{
				u32 fsum = syNetPeerChecksumBytes(fuzzbuf, fsize - 4);
				w = fuzzbuf + fsize - 4;
				syNetPeerWriteU32(&w, fsum);
			}
			syNetPeerHandleForwardPacket(fuzzbuf, fsize, (host != NULL) ? host->link : -1);
			continue;
		}
		fsize = SYNETPEER_PACKET_BYTES;
		if (tier >= 1)
		{
			u8 *w = fuzzbuf;

			syNetPeerWriteU32(&w, SYNETPEER_MAGIC);
			syNetPeerWriteU16(&w, SYNETPEER_VERSION);
		}
		if (tier == 2)
		{
			u8 *w = fuzzbuf + 8;

			syNetPeerWriteU32(&w, sSYNetPeerSessionID);
		}
		if (tier >= 3)
		{
			fremote = syNetPeerFuzzPickRemote(fiter);
			if (fremote == NULL)
			{
				continue;
			}
			syNetPeerFuzzFillInputPacket(fuzzbuf,
				(tier == 4) ? (sSYNetPeerSessionID ^ 0x5AF00D5AU) : sSYNetPeerSessionID,
				fremote, fiter, fseed);
			if (tier == 5)
			{
				fuzzbuf[fsize - 1] ^= 0xFFu;   /* break the trailing checksum */
			}
		}
		else if (sSYNetPeerIsSocketExternal != FALSE)
		{
			fremote = NULL;
			for (fi = 0; fi < MAXCONTROLLERS; fi++)
			{
				fremote = syNetPeerGetRemoteByPlayer(fi);
				if ((fremote != NULL) && (fremote->player != sSYNetPeerLocalPlayer))
				{
					break;
				}
				fremote = NULL;
			}
			if (fremote == NULL)
			{
				continue;
			}
		}
		else
		{
			fremote = syNetPeerGetRemoteByPlayer(sSYNetPeerRemotePlayer);
			if (fremote == NULL)
			{
				continue;
			}
		}
		syNetPeerHandlePacket(fuzzbuf, fsize, fremote, FALSE);
	}

	while (TRUE)
	{
		s32 size;
		SYNetPeerRemote *remote;

		if (sSYNetPeerIsSocketExternal != FALSE)
		{
			s32 link = -1;

			size = port_udp_link_recv(&link, buffer, sizeof(buffer));
			if (size <= 0)
			{
				break;
			}
			if (syNetPeerCheckForwardPacket(buffer, size) != FALSE)
			{
				syNetPeerHandleForwardPacket(buffer, size, link);
				continue;
			}
			remote = syNetPeerGetRemoteByLink(link);
			if (remote == NULL)
			{
				/* Spec links aren't remotes: a catching-up spectator sends
				 * SPEC_PROGRESS on its link so the host can pace the history
				 * stream. Anything else on a spec link is noise. */
				if (syNetPeerGetSpecIndex(link) >= 0)
				{
					syNetPeerHandleSpecProgress(buffer, size, link);
				}
				else
				{
					static s32 sDropDiagBudget = 8;

					sSYNetPeerPacketsDropped++;
					if (sDropDiagBudget-- > 0)
					{
						port_log("SSB64 NetPeer: unrouteable datagram link=%d size=%d head=%02X%02X tick=%u\n",
								 link, size,
								 (size > 0) ? buffer[0] : 0, (size > 1) ? buffer[1] : 0,
								 syNetInputGetTick());
					}
				}
				continue;
			}
		}
		else
		{
			size = port_udp_recv(buffer, sizeof(buffer));
			if (size <= 0)
			{
				if (size < 0)
				{
					sSYNetPeerPacketsDropped++;
				}
				break;
			}
			remote = syNetPeerGetRemoteByPlayer(sSYNetPeerRemotePlayer);
		}
		syNetPeerHandlePacket(buffer, (s32)size, remote, FALSE);
	}
#endif
}

#ifdef PORT
void syNetPeerLogNetSyncValidation(u32 tick)
{
	u32 checksums[MAXCONTROLLERS];
	u32 inp_all = 0;
	u32 fighter_hash;
	u32 win_begin = 0;
	u32 win_length = tick;

	if ((sSYNetPeerIsActive == FALSE) || (syNetPeerCheckBattleExecutionReady() == FALSE))
	{
		return;
	}
	if (tick >= SYNETPEER_VALIDATION_INPUT_WINDOW)
	{
		win_begin = tick - SYNETPEER_VALIDATION_INPUT_WINDOW;
		win_length = SYNETPEER_VALIDATION_INPUT_WINDOW;
	}
	syNetInputGetHistoryInputValueChecksumWindow(win_begin, win_length, checksums, &inp_all);

	fighter_hash = syNetSyncHashBattleFighters();

	port_log(
		"SSB64 NetSync: role=%s lp=%d rp=%d tick=%u hist_win=[%u,%u) all=0x%08X p0=0x%08X p1=0x%08X p2=0x%08X p3=0x%08X figh=0x%08X snd_next=%u rcv_hw=%u gap=%u dup=%u ooo=%u puck=%u pko=%u pkn=%u sent=%u recv=%u dropped=%u stg=%u hr=%u late=%u inpchk=0x%08X pkt_valid=%d\n",
		(sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
		sSYNetPeerLocalPlayer,
		sSYNetPeerRemotePlayer,
		tick,
		win_begin,
		win_begin + win_length,
		inp_all,
		checksums[0],
		checksums[1],
		checksums[2],
		checksums[3],
		fighter_hash,
		sSYNetPeerSendSeq,
		sSYNetPeerRecvSeqHighWater,
		sSYNetPeerSeqGaps,
		sSYNetPeerSeqDuplicates,
		sSYNetPeerSeqOutOfOrder,
		sSYNetPeerLastPeerAckTick,
		(sSYNetPeerLastPacketTicksValid != FALSE) ? sSYNetPeerLastPacketOldestTick : (~(u32)0),
		(sSYNetPeerLastPacketTicksValid != FALSE) ? sSYNetPeerLastPacketNewestTick : (~(u32)0),
		sSYNetPeerPacketsSent,
		sSYNetPeerPacketsReceived,
		sSYNetPeerPacketsDropped,
		sSYNetPeerFramesStaged,
		sSYNetPeerHighestRemoteTick,
		sSYNetPeerLateFrames,
		sSYNetPeerInputChecksum,
		sSYNetPeerLastPacketTicksValid);
}
#endif

void syNetPeerLogStats(void)
{
#ifdef PORT
	u32 tick = syNetInputGetTick();

	if ((tick == 0) || ((tick - sSYNetPeerLastLogTick) < SYNETPEER_LOG_INTERVAL))
	{
		return;
	}
	sSYNetPeerLastLogTick = tick;

	port_log("SSB64 NetPeer: role=%s local=%d remote=%d barrier=%d execution_ready=%d tick=%u sent=%u recv=%u dropped=%u staged=%u highest_remote=%u late=%u snd_next=%u rcv_hw=%u seq_gap=%u seq_dup=%u seq_ooo=%u peer_ack=%u inpchk=0x%08X\n",
	         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
	         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer,
	         sSYNetPeerBattleBarrierReleased, syNetPeerCheckBattleExecutionReady(), tick,
	         sSYNetPeerPacketsSent, sSYNetPeerPacketsReceived, sSYNetPeerPacketsDropped,
	         sSYNetPeerFramesStaged, sSYNetPeerHighestRemoteTick, sSYNetPeerLateFrames,
	         sSYNetPeerSendSeq, sSYNetPeerRecvSeqHighWater, sSYNetPeerSeqGaps,
	         sSYNetPeerSeqDuplicates, sSYNetPeerSeqOutOfOrder,
	         sSYNetPeerLastPeerAckTick,
	         sSYNetPeerInputChecksum);

	syNetPeerLogNetSyncValidation(tick);
#endif
}

void syNetPeerLogExecutionHold(void)
{
#ifdef PORT
	if ((sSYNetPeerExecutionHoldFrames == 1) ||
		((sSYNetPeerExecutionHoldFrames % SYNETPEER_BARRIER_LOG_INTERVAL) == 0))
	{
		port_log("SSB64 NetPeer: execution hold role=%s local=%d remote=%d tick=%u hold=%u barrier_wait=%u peer_ready=%d start_sent=%d start_recv=%d highest_remote=%u late=%u recv=%u dropped=%u\n",
		         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
		         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer, syNetInputGetTick(),
		         sSYNetPeerExecutionHoldFrames, sSYNetPeerBattleBarrierWaitFrames,
		         sSYNetPeerBattlePeerReady, sSYNetPeerBattleStartSent,
		         sSYNetPeerBattleStartReceived, sSYNetPeerHighestRemoteTick,
		         sSYNetPeerLateFrames, sSYNetPeerPacketsReceived, sSYNetPeerPacketsDropped);
	}
#endif
}

void syNetPeerLogExecutionBegin(void)
{
#ifdef PORT
	if (sSYNetPeerExecutionBeginLogged == FALSE)
	{
		sSYNetPeerExecutionBeginLogged = TRUE;
		port_log("SSB64 NetPeer: execution begin role=%s local=%d remote=%d tick=%u hold=%u barrier_wait=%u highest_remote=%u late=%u\n",
		         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
		         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer, syNetInputGetTick(),
		         sSYNetPeerExecutionHoldFrames, sSYNetPeerBattleBarrierWaitFrames,
		         sSYNetPeerHighestRemoteTick, sSYNetPeerLateFrames);
	}
#endif
}

void syNetPeerLogBarrierWait(void)
{
#ifdef PORT
	if ((sSYNetPeerBattleBarrierWaitFrames % SYNETPEER_BARRIER_LOG_INTERVAL) == 0)
	{
		port_log("SSB64 NetPeer: barrier wait role=%s local=%d remote=%d tick=%u local_ready=%d peer_ready=%d start_sent=%d start_recv=%d sent=%u recv=%u dropped=%u staged=%u highest_remote=%u late=%u\n",
		         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client",
		         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer, syNetInputGetTick(),
		         sSYNetPeerBattleLocalReady, sSYNetPeerBattlePeerReady,
		         sSYNetPeerBattleStartSent, sSYNetPeerBattleStartReceived, sSYNetPeerPacketsSent,
		         sSYNetPeerPacketsReceived, sSYNetPeerPacketsDropped,
		         sSYNetPeerFramesStaged, sSYNetPeerHighestRemoteTick, sSYNetPeerLateFrames);
	}
#endif
}

void syNetPeerReleaseBattleBarrier(const char *reason)
{
	if (sSYNetPeerBattleBarrierReleased == FALSE)
	{
		sSYNetPeerBattleBarrierReleased = TRUE;

#ifdef PORT
		port_log("SSB64 NetPeer: barrier release role=%s reason=%s local=%d remote=%d tick=%u wait=%u sent=%u recv=%u dropped=%u staged=%u highest_remote=%u late=%u\n",
		         (sSYNetPeerBootstrapIsHost != FALSE) ? "host" : "client", reason,
		         sSYNetPeerLocalPlayer, sSYNetPeerRemotePlayer, syNetInputGetTick(),
		         sSYNetPeerBattleBarrierWaitFrames, sSYNetPeerPacketsSent,
		         sSYNetPeerPacketsReceived, sSYNetPeerPacketsDropped,
		         sSYNetPeerFramesStaged, sSYNetPeerHighestRemoteTick, sSYNetPeerLateFrames);
#endif
	}
}

/* Only remotes we talk to directly gate the barrier: on a client, the other
 * clients arrive through the star host's forwarding (link == -1) and the
 * host's BATTLE_START already implies they were ready. */
sb32 syNetPeerCheckAllBattleReady(void)
{
	s32 player;
	sb32 any = FALSE;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		SYNetPeerRemote *remote = &sSYNetPeerRemotes[player];

		if ((remote->player == player) && (remote->link >= 0) && (remote->dropped == FALSE))
		{
			if (remote->battle_ready == FALSE)
			{
				return FALSE;
			}
			any = TRUE;
		}
	}
	if ((any == FALSE) && (sSYNetPeerIsSocketExternal == FALSE))
	{
		/* legacy single-peer socket: the remote has link == -1 */
		for (player = 0; player < MAXCONTROLLERS; player++)
		{
			SYNetPeerRemote *remote = &sSYNetPeerRemotes[player];

			if ((remote->player == player) && (remote->dropped == FALSE))
			{
				return remote->battle_ready;
			}
		}
	}
	return any;
}

sb32 syNetPeerCheckAllBattleStartReceived(void)
{
	s32 player;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		SYNetPeerRemote *remote = &sSYNetPeerRemotes[player];

		if ((remote->player == player) && (remote->dropped == FALSE) &&
			(remote->battle_start_received == FALSE))
		{
			return FALSE;
		}
	}
	return TRUE;
}

void syNetPeerUpdateStartBarrier(void)
{
#ifdef PORT
	if ((sSYNetPeerBattleBarrierEnabled == FALSE) || (sSYNetPeerBattleBarrierReleased != FALSE))
	{
		return;
	}
	sSYNetPeerBattleBarrierWaitFrames++;
	syNetPeerSendControlPacket(SYNETPEER_PACKET_BATTLE_READY);

	if (syNetPeerCheckAllBattleReady() != FALSE)
	{
		if (sSYNetPeerBootstrapIsHost != FALSE)
		{
			syNetPeerSendControlPacket(SYNETPEER_PACKET_BATTLE_START);
			sSYNetPeerBattleStartSent = TRUE;

			if (syNetPeerCheckAllBattleStartReceived() != FALSE)
			{
				sSYNetPeerBattleStartRepeatFrames = SYNETPEER_BATTLE_START_REPEAT_FRAMES;
				syNetPeerReleaseBattleBarrier("client-ack");
			}
		}
		else if (sSYNetPeerBattleStartReceived != FALSE)
		{
			syNetPeerSendControlPacket(SYNETPEER_PACKET_BATTLE_START);
			sSYNetPeerBattleStartSent = TRUE;
			sSYNetPeerBattleStartRepeatFrames = SYNETPEER_BATTLE_START_REPEAT_FRAMES;
			syNetPeerReleaseBattleBarrier("host-start");
		}
	}
	syNetPeerLogBarrierWait();
#endif
}

/* A remote that has gone silent for syNetPeerRemoteDropMs() is excluded
 * from pacing so one lost player can't freeze everyone else's match. */
void syNetPeerUpdateRemoteDrops(void)
{
#ifdef PORT
	u32 now = port_udp_now_ms();
	u32 drop_ms = syNetPeerRemoteDropMs();
	s32 player;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		SYNetPeerRemote *remote = &sSYNetPeerRemotes[player];

		if ((remote->player == player) && (remote->dropped == FALSE) &&
			(remote->last_heard_ms != 0) &&
			(now - remote->last_heard_ms > drop_ms))
		{
			remote->dropped = TRUE;
			port_log("SSB64 NetPeer: remote player %d silent for %ums - dropping from pacing\n",
			         player, now - remote->last_heard_ms);
		}
	}
#endif
}

void syNetPeerUpdateBattleGate(void)
{
	if (sSYNetPeerIsActive == FALSE)
	{
		return;
	}
	sb32 dbg = (getenv("SSB64_NETPLAY_DEBUG_STAGES") != NULL) ? TRUE : FALSE;

	if (dbg != FALSE) port_log("SSB64 DBG: gate:recv enter\n");
	syNetPeerReceiveRemoteInput();
	if (dbg != FALSE) port_log("SSB64 DBG: gate:drops\n");
	syNetPeerUpdateRemoteDrops();
	if (dbg != FALSE) port_log("SSB64 DBG: gate:barrier\n");
	syNetPeerUpdateStartBarrier();

	if (syNetPeerCheckBattleExecutionReady() == FALSE)
	{
		sb32 dbg = (getenv("SSB64_NETPLAY_DEBUG_STAGES") != NULL) ? TRUE : FALSE;

		sSYNetPeerExecutionHoldFrames++;
		syNetPeerLogExecutionHold();

		/* A stalled peer must keep sending, or two peers waiting on each
		 * other's input would never resume. */
		if (syNetPeerCheckBarrierReleased() != FALSE)
		{
			if (dbg != FALSE) port_log("SSB64 DBG: hold:send enter\n");
			syNetPeerSendLocalInput();
			if (dbg != FALSE) port_log("SSB64 DBG: hold:send exit\n");
		}
	}
	else syNetPeerLogExecutionBegin();
}

void syNetPeerUpdate(void)
{
	if (sSYNetPeerIsActive == FALSE)
	{
		return;
	}
	syNetPeerUpdateBattleGate();

#ifdef PORT
	/* History streaming is host-only and independent of the execution gate:
	 * a stalled host must still feed catching-up spectators. */
	syNetPeerServeSpecHistory();
#endif
	if (syNetPeerCheckBattleExecutionReady() == FALSE)
	{
		return;
	}
	if (sSYNetPeerBattleStartRepeatFrames != 0)
	{
		syNetPeerSendControlPacket(SYNETPEER_PACKET_BATTLE_START);
		sSYNetPeerBattleStartRepeatFrames--;
	}
	syNetPeerSendLocalInput();
#ifdef PORT
	syNetPeerUpdateAdaptiveDelay();
#endif
	syNetPeerLogStats();
}

/* Online match from the lobby (port/net/netlobby.cpp). The UDP endpoint is
 * already open with the peer (or relay) set; this configures the session the
 * same way the env-driven bootstrap does, applies the agreed match and asks
 * the scene manager to enter VS battle. Both peers call it with mirrored
 * player slots and identical metadata; the in-battle start barrier then
 * aligns tick 0.
 *
 * wire_slots maps each game slot (0..player_count-1, always contiguous) to
 * the room slot the lobby transport links are keyed by. The lobby assigns
 * room slots sparsely when a middle player leaves, so game slot != room
 * slot in that case; NULL means the identity map. */
void syNetPeerStartOnline(s32 local_player, s32 player_count, u32 delay, sb32 is_host,
                          const SYNetInputReplayMetadata *metadata, const s32 *wire_slots,
                          u32 session_id)
{
#ifdef PORT
	extern void portSCManagerRequestScene(s32 target);
	s32 player;

	sSYNetPeerIsEnabled = TRUE;
	sSYNetPeerIsConfigured = TRUE;
	sSYNetPeerIsSocketExternal = TRUE;
	sSYNetPeerIsSpectator = FALSE;
	syNetPeerResetSpecLinks();            /* the lobby re-registers watchers each match */
	sSYNetPeerLocalPlayer = local_player;
	syNetPeerResetRemotes();
	if (is_host != FALSE)
	{
		/* star topology: the host keeps one link per client, keyed by the
		 * remote's room slot */
		for (player = 1; player < player_count; player++)
		{
			syNetPeerAddRemote(player, (wire_slots != NULL) ? wire_slots[player] : player);
		}
	}
	else
	{
		/* clients link only to the host (always room slot 0); the other
		 * clients' input streams arrive inside FORWARD packets on it */
		syNetPeerAddRemote(0, 0);
		for (player = 1; player < player_count; player++)
		{
			if (player != local_player)
			{
				syNetPeerAddRemote(player, -1);
			}
		}
	}
	sSYNetPeerRemotePlayer = (local_player == 0) ? 1 : 0;
	sSYNetPeerInputDelay = delay;
	sSYNetPeerDelayLastStallCheck = 0;
	sSYNetPeerDelayCheckStallBase = 0;
	sSYNetPeerDelayCheckLateBase = 0;
	sSYNetPeerDelayRepeatLeft = 0;
	/* session id comes from the lobby, generated independently of the
	 * broadcast rng_seed; 0 keeps the legacy seed-derived behavior for
	 * start packets that predate the field */
	sSYNetPeerSessionID = (session_id != 0) ? session_id : metadata->rng_seed;
	sSYNetPeerIsRollback = TRUE;
#ifdef SSB64_TEST_HOOKS
	sSYNetPeerFuzzSeed = (getenv("SSB64_NETPLAY_FUZZ") != NULL) ? (u32)atoi(getenv("SSB64_NETPLAY_FUZZ")) : 0;
#endif
	sSYNetPeerBootstrapIsEnabled = TRUE;    // enables the in-battle start barrier
	sSYNetPeerBootstrapIsHost = is_host;
	sSYNetPeerBootstrapSeed = metadata->rng_seed;

	syNetRollbackSetTickTimer(TRUE);
	syNetPeerApplyBootstrapMetadata(metadata);
	portSCManagerRequestScene(nSCKindVSBattle);

	port_log("SSB64 NetPeer: online match local=%d players=%d remotes=%d delay=%u host=%d stage=%u seed=%u\n",
	         local_player, player_count, sSYNetPeerRemoteCount, delay, is_host, metadata->stage_kind, metadata->rng_seed);
#endif
}

/* Online match rules: player_count humans (slots 0..player_count-1), stock battle. */
void syNetPeerMakeOnlineMetadata(SYNetInputReplayMetadata *metadata, u32 stage, u32 stocks, sb32 is_items, u32 seed,
                                 u32 player_count, const u8 *fighter_kinds, const u8 *costumes)
{
	u32 player;

	syNetPeerMakeBootstrapMetadata(metadata);
	metadata->stage_kind = stage;
	metadata->stocks = (stocks > 0) ? stocks - 1 : 0;   // the engine stores lives - 1 (3 = "4 Stocks")
	metadata->rng_seed = seed;
	metadata->player_count = player_count;

	if (is_items != FALSE)
	{
		metadata->item_toggles = ~0;
		metadata->item_switch = metadata->item_appearance_rate = nSCBattleItemSwitchMiddle;
	}
	for (player = 0; (player < player_count) && (player < MAXCONTROLLERS); player++)
	{
		metadata->player_kinds[player] = nFTPlayerKindMan;
		metadata->fighter_kinds[player] = fighter_kinds[player];
		metadata->costumes[player] = costumes[player];
	}
}

/* Plain-int entry for the C++ lobby (keeps decomp headers out of port C++).
 * local_slot is this instance's game slot (0 = host); every participant
 * occupies one contiguous game slot 0..player_count-1. wire_slots maps each
 * game slot back to its room slot (NULL = identity). */
void syNetPeerStartOnlineMatch(s32 local_slot, s32 player_count, u32 delay, u32 stage, u32 stocks, s32 is_items, u32 seed,
                               u32 session_id, const s32 *fkinds, const s32 *costumes, const s32 *wire_slots)
{
	SYNetInputReplayMetadata metadata;
	u8 fighter_kinds[MAXCONTROLLERS] = { 0 };
	u8 costume_ids[MAXCONTROLLERS] = { 0 };
	s32 player;

	for (player = 0; (player < player_count) && (player < MAXCONTROLLERS); player++)
	{
		fighter_kinds[player] = (u8)fkinds[player];
		costume_ids[player] = (u8)costumes[player];
	}
	if (player_count < 2)
	{
		player_count = 2;
	}
	if (player_count > MAXCONTROLLERS)
	{
		player_count = MAXCONTROLLERS;
	}

	syNetPeerMakeOnlineMetadata(&metadata, stage, stocks, is_items, seed, player_count, fighter_kinds, costume_ids);
	/* every online match auto-records its inputs; the file lands under
	 * <save dir>/replays/ when the VS session ends (and doubles as the
	 * payload of a desync package if the sync detector fires) */
	syNetReplayArmAutoSave();
	syNetPeerStartOnline(local_slot, player_count, delay, (local_slot == 0) ? TRUE : FALSE, &metadata, wire_slots,
	                     session_id);
}

/* Spectator entry point for the lobby: same match parameters as
 * syNetPeerStartOnlineMatch but with no local player. Every slot is a
 * remote stream fed exclusively by FORWARD datagrams pushed by the host
 * over the spectator's relay link; nothing is sent, the start barrier is
 * skipped and pacing is strict (no predictions), so the watched match runs
 * slightly behind live play whenever the relay adds latency. */
void syNetPeerStartOnlineSpectate(s32 player_count, u32 delay, u32 stage, u32 stocks, s32 is_items, u32 seed,
                                  u32 session_id, const s32 *fkinds, const s32 *costumes)
{
#ifdef PORT
	extern void portSCManagerRequestScene(s32 target);
	SYNetInputReplayMetadata metadata;
	u8 fighter_kinds[MAXCONTROLLERS] = { 0 };
	u8 costume_ids[MAXCONTROLLERS] = { 0 };
	s32 player;

	if (player_count < 2)
	{
		player_count = 2;
	}
	if (player_count > MAXCONTROLLERS)
	{
		player_count = MAXCONTROLLERS;
	}
	for (player = 0; player < player_count; player++)
	{
		fighter_kinds[player] = (u8)fkinds[player];
		costume_ids[player] = (u8)costumes[player];
	}
	syNetPeerMakeOnlineMetadata(&metadata, stage, stocks, is_items, seed, player_count, fighter_kinds, costume_ids);
	syNetReplayArmAutoSave();

	sSYNetPeerIsEnabled = TRUE;
	sSYNetPeerIsConfigured = TRUE;
	sSYNetPeerIsSocketExternal = TRUE;
	sSYNetPeerIsSpectator = TRUE;
	syNetPeerResetSpecLinks();
	sSYNetPeerLocalPlayer = -1;
	syNetPeerResetRemotes();
	/* every player is a remote with no direct link: their packets all
	 * arrive FORWARD-wrapped from the host */
	for (player = 0; player < player_count; player++)
	{
		syNetPeerAddRemote(player, -1);
	}
	sSYNetPeerRemotePlayer = 0;
	sSYNetPeerInputDelay = delay;
	sSYNetPeerSessionID = (session_id != 0) ? session_id : metadata.rng_seed;
	sSYNetPeerIsRollback = FALSE;         /* spectators never predict, so snapshots are never needed */
	sSYNetPeerFuzzSeed = 0;
	sSYNetPeerBootstrapIsEnabled = FALSE; /* no start barrier for watchers */
	sSYNetPeerBootstrapIsHost = FALSE;
	sSYNetPeerBootstrapSeed = metadata.rng_seed;

	syNetRollbackSetTickTimer(TRUE);
	syNetPeerApplyBootstrapMetadata(&metadata);
	portSCManagerRequestScene(nSCKindVSBattle);

	port_log("SSB64 NetPeer: spectating match players=%d delay=%u stage=%u seed=%u\n",
	         player_count, delay, metadata.stage_kind, metadata.rng_seed);
#endif
}

void syNetPeerStopVSSession(void)
{
#ifdef PORT
	if (sSYNetPeerIsActive != FALSE)
	{
		port_log("SSB64 NetPeer: VS session stop sent=%u recv=%u dropped=%u staged=%u late=%u stalls=%u lockstep_stalls=%u timesync_stalls=%u checksum=0x%08X\n",
		         sSYNetPeerPacketsSent, sSYNetPeerPacketsReceived, sSYNetPeerPacketsDropped,
		         sSYNetPeerFramesStaged, sSYNetPeerLateFrames, sSYNetPeerStallFrames, sSYNetPeerLockstepStallFrames,
		         sSYNetPeerTimeSyncStallFrames, sSYNetPeerInputChecksum);
	}
	if (sSYNetPeerIsSocketExternal != FALSE)
	{
		/* Online match over: keep the lobby's socket (rematch), and drop back
		 * to offline so the next local VS match isn't netplay. */
		extern void port_netlobby_on_match_end(void);
		extern void port_netlobby_set_match_result(s32 winner_slot, u32 duration_ticks);
		s32 i, winner = -1, best_stocks = -1, best_score = -1;
		sb8 dead_heat = FALSE;

		/* Winner = the game slot with the most remaining stocks, tie-broken
		 * by the game's own score (the results-screen ranking metric), while
		 * the battle state is still readable (called before the scene
		 * switch). A dead heat on both records a draw — sudden death
		 * resolves true ties in the sim before this point anyway. This also
		 * makes timed matches work: all slots sit at equal stocks, so the
		 * score picks the real winner instead of an automatic draw. */
		/* Slots past player_count are empty — they must not enter the
		 * comparison or a -1/0 seat could win a timed match on score 0. */
		for (i = 0; (gSCManagerBattleState != NULL) &&
		            (i < sSYNetPeerBootstrapMetadata.player_count) &&
		            (i < MAXCONTROLLERS); i++)
		{
			s32 stocks = gSCManagerBattleState->players[i].stock_count;
			s32 score = gSCManagerBattleState->players[i].score;
			if (stocks > best_stocks || (stocks == best_stocks && score > best_score))
			{
				best_stocks = stocks;
				best_score = score;
				winner = i;
				dead_heat = FALSE;
			}
			else if (stocks == best_stocks && score == best_score)
			{
				dead_heat = TRUE;
			}
		}
		if (dead_heat)
		{
			winner = -1;
		}
		/* the loop above already tolerates a NULL battle state (quit before
		 * the scene fully came up) — keep the duration read behind the same
		 * guard or this dereferences the pointer it just skipped */
		port_netlobby_set_match_result(winner,
			gSCManagerBattleState != NULL ? (u32)gSCManagerBattleState->time_passed : 0);

		sSYNetPeerIsEnabled = FALSE;
		sSYNetPeerIsConfigured = FALSE;
		sSYNetPeerBootstrapIsEnabled = FALSE;
		sSYNetPeerIsSocketExternal = FALSE;
		sSYNetPeerIsSpectator = FALSE;
		syNetPeerResetSpecLinks();
		syNetRollbackRestoreTickTimer();
		port_netlobby_on_match_end();
	}
	else syNetPeerCloseSocket();
#endif
	sSYNetPeerIsActive = FALSE;
}

#ifdef PORT
/* Connection-quality snapshot for the lobby UI: per-remote packet
 * accounting plus the remote's declared advantage (our rollback depth for
 * its inputs). player is the game slot (0..MAXCONTROLLERS-1). */
void portNetPeerGetRemoteStats(s32 player, u32 *gaps, u32 *dups, u32 *ooo, s32 *advantage,
                               u32 *recvHighwater)
{
	SYNetPeerRemote *remote = syNetPeerGetRemoteByPlayer(player);

	if (remote == NULL)
	{
		return;
	}
	if (gaps != NULL)
	{
		*gaps = remote->seq_gaps;
	}
	if (dups != NULL)
	{
		*dups = remote->seq_duplicates;
	}
	if (ooo != NULL)
	{
		*ooo = remote->seq_out_of_order;
	}
	if (advantage != NULL)
	{
		*advantage = remote->remote_advantage;
	}
	if (recvHighwater != NULL)
	{
		*recvHighwater = remote->recv_seq_highwater;
	}
}
#endif
