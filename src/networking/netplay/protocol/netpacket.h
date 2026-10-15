#ifndef NB_NETPACKET_H
#define NB_NETPACKET_H

/* Pure wire codec for the ssb64 netplay datagrams (SSNP v4). Big-endian on
 * the wire; FNV-1a checksum authenticates the whole datagram. No globals,
 * no session state — every function takes explicit context so the codec is
 * callable from the game (components/decomp/src/sys/netpeer.c) and from unit tests.
 *
 * Layouts (see docs/netplay/protocol/specification.md):
 *   INPUT:    [magic][ver][advantage][session][ack][seq][player][count]
 *             [local][remote][16×frame][check_tick][check_hash][checksum]
 *   CONTROL:  [magic][ver][type][session][checksum]
 *   BOOTSTRAP:[magic][ver][type][session][metadata][checksum]
 *   FORWARD:  [magic][ver][type][session][src][inner][checksum]
 */

/* N64 primitive types: reuse decomp's ultratypes when its include dir is
 * on the path (the decomp TUs); fall back to stdint so the header is
 * self-contained for port sources and unit tests. _LANGUAGE_C alone is
 * not a reliable probe — port TUs get it as a global define without the
 * decomp include dir. */
#if defined(__has_include)
#if __has_include("PR/ultratypes.h")
#include "PR/ultratypes.h"
#define NB_NETPACKET_HAVE_ULTRATYPES
#endif
#endif
#ifndef NB_NETPACKET_HAVE_ULTRATYPES
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
#endif

#define NB_NETPACKET_MAGIC 0x53534E50 /* 'SSNP' */
#define NB_NETPACKET_VERSION 4
#define NB_NETPACKET_FNV_BASIS 2166136261U
#define NB_NETPACKET_FNV_PRIME 16777619U

#define NB_NETPACKET_MAX_PACKET_FRAMES 16
#define NB_NETPACKET_FRAME_BYTES 8
#define NB_NETPACKET_INPUT_HEADER_BYTES (4 + 2 + 2 + 4 + 4 + 4 + 1 + 1 + 1 + 1)
#define NB_NETPACKET_SYNC_CHECK_BYTES (4 + 4)
#define NB_NETPACKET_PACKET_BYTES \
	(NB_NETPACKET_INPUT_HEADER_BYTES + \
	 (NB_NETPACKET_MAX_PACKET_FRAMES * NB_NETPACKET_FRAME_BYTES) + \
	 NB_NETPACKET_SYNC_CHECK_BYTES + 4)
#define NB_NETPACKET_CONTROL_PACKET_BYTES (4 + 2 + 2 + 4 + 4)
#define NB_NETPACKET_DELAY_PACKET_BYTES   (4 + 2 + 2 + 4 + 4 + 4)
#define NB_NETPACKET_FORWARD_HEADER_BYTES (4 + 2 + 2 + 4 + 1)
#define NB_NETPACKET_FORWARD_PACKET_BYTES \
	(NB_NETPACKET_FORWARD_HEADER_BYTES + NB_NETPACKET_PACKET_BYTES + 4)

#define NB_NETPACKET_PACKET_INPUT 0
#define NB_NETPACKET_PACKET_MATCH_CONFIG 1
#define NB_NETPACKET_PACKET_READY 2
#define NB_NETPACKET_PACKET_START 3
#define NB_NETPACKET_PACKET_BATTLE_READY 4
#define NB_NETPACKET_PACKET_BATTLE_START 5
#define NB_NETPACKET_PACKET_FORWARD 6
/* Host → players: raise the input delay symmetric mid-match when the host
 * measures sustained prediction stalls (the "late" treadmill). Applied on
 * receipt; repeated sends converge after loss. Asymmetric delay while a
 * packet is in flight only costs prediction budget, never determinism —
 * inputs are addressed by absolute tick. */
#define NB_NETPACKET_PACKET_DELAY 7
/* Spectator → host: reports the spec's consumed sim tick so the host can
 * pace the mid-match catch-up history stream inside the 720-deep remote
 * ring. Loss only slows the stream — the host resends the gap window. */
#define NB_NETPACKET_PACKET_SPEC_PROGRESS 8

/* Sentinel for "no confirmed-state hash carried" in the sync trailer. */
#define NB_NETPACKET_NO_CHECK_TICK 0xFFFFFFFFU

typedef struct NBNetPacketFrame
{
	u32 tick;
	u16 buttons;
	s8 stick_x;
	s8 stick_y;

} NBNetPacketFrame;

typedef struct NBNetPacketInput
{
	s16 remote_advantage;
	u32 session_id;
	u32 ack_tick;
	u32 seq;
	u8 player;          /* sender game slot */
	u8 frame_count;     /* valid entries in frames[] (0..16) */
	u8 local_player;    /* sender-side local slot (== player) */
	u8 remote_player;   /* addressed slot (== receiver's local) */
	NBNetPacketFrame frames[NB_NETPACKET_MAX_PACKET_FRAMES];
	u32 check_tick;     /* NB_NETPACKET_NO_CHECK_TICK when none */
	u32 check_hash;

} NBNetPacketInput;

typedef enum NBNetPacketStatus
{
	NB_NETPACKET_OK = 0,
	NB_NETPACKET_SHORT,        /* buffer smaller than the minimum header */
	NB_NETPACKET_BAD_SIZE,     /* not a valid size for the packet kind */
	NB_NETPACKET_BAD_MAGIC,
	NB_NETPACKET_BAD_VERSION,
	NB_NETPACKET_BAD_TYPE,
	NB_NETPACKET_BAD_SESSION,
	NB_NETPACKET_BAD_PLAYER,      /* player/local_player != src */
	NB_NETPACKET_BAD_REMOTE,      /* remote_player != local (non-forwarded) */
	NB_NETPACKET_BAD_FRAME_COUNT,
	NB_NETPACKET_BAD_CHECKSUM,
	NB_NETPACKET_SELF_FORWARD,    /* FORWARD claiming our own slot */

} NBNetPacketStatus;

/* Context the caller supplies to validate a received INPUT datagram. */
typedef struct NBNetPacketContext
{
	u32 session_id;
	s32 src_player;    /* remote slot the datagram must come from */
	s32 local_player;  /* our slot (remote_player must match unless forwarded) */
	s32 forwarded;     /* nonzero when unwrapped from a FORWARD envelope */
	s32 ignore_session;   /* fuzz mutants: skip the session-id check */
	s32 ignore_checksum;  /* fuzz mutants: skip the checksum check */

} NBNetPacketContext;

#ifdef __cplusplus
extern "C" {
#endif

/* ── FNV-1a ──────────────────────────────────────────────────────────── */
u32 nbNetPacketChecksumBytes(const u8 *bytes, u32 size);
u32 nbNetPacketChecksumAccumulateU32(u32 checksum, u32 value);
u32 nbNetPacketChecksumAccumulateFrame(u32 checksum, const NBNetPacketFrame *frame);

/* ── Big-endian cursor I/O ───────────────────────────────────────────── */
void nbNetPacketWriteU8(u8 **cursor, u8 value);
void nbNetPacketWriteU16(u8 **cursor, u16 value);
void nbNetPacketWriteU32(u8 **cursor, u32 value);
u8 nbNetPacketReadU8(const u8 **cursor);
u16 nbNetPacketReadU16(const u8 **cursor);
u32 nbNetPacketReadU32(const u8 **cursor);

/* ── INPUT packet ────────────────────────────────────────────────────── */

/* Serialize `in` into buffer (must hold NB_NETPACKET_PACKET_BYTES) and
 * append the checksum. Returns the datagram size. */
u32 nbNetPacketBuildInput(u8 *buffer, const NBNetPacketInput *in);

/* Validate + parse an INPUT datagram. On NB_NETPACKET_OK the parsed fields
 * land in `out` (may be NULL for a validation-only call). */
NBNetPacketStatus nbNetPacketParseInput(const u8 *buffer, s32 size,
										const NBNetPacketContext *ctx,
										NBNetPacketInput *out);

/* ── CONTROL packet ──────────────────────────────────────────────────── */

u32 nbNetPacketBuildControl(u8 *buffer, u16 packet_type, u32 session_id);

/* Validate + return the control type (READY/START/BATTLE_READY/...) or a
 * negative NBNetPacketStatus. */
s32 nbNetPacketParseControl(const u8 *buffer, s32 size, u32 session_id);

/* ── DELAY packet ────────────────────────────────────────────────────── */

u32 nbNetPacketBuildDelay(u8 *buffer, u32 session_id, u32 delay);

/* Validate + return NB_NETPACKET_OK with *out_delay set, or a negative
 * NBNetPacketStatus. */
s32 nbNetPacketParseDelay(const u8 *buffer, s32 size, u32 session_id, u32 *out_delay);

/* ── SPEC_PROGRESS packet ────────────────────────────────────────────── */

u32 nbNetPacketBuildSpecProgress(u8 *buffer, u32 session_id, u32 tick);

/* Validate + return NB_NETPACKET_OK with *out_tick set, or a negative
 * NBNetPacketStatus. */
s32 nbNetPacketParseSpecProgress(const u8 *buffer, s32 size, u32 session_id, u32 *out_tick);

/* ── FORWARD envelope ────────────────────────────────────────────────── */

/* Wrap `inner` (an INPUT datagram authored by src_player) for relay.
 * Returns the datagram size, or 0 when the inner buffer cannot fit. */
u32 nbNetPacketBuildForward(u8 *buffer, u32 buffer_size,
							const u8 *inner, u32 inner_size,
							u8 src_player, u32 session_id);

/* Validate the envelope. On OK: *src_player_out gets the claimed sender
 * and *inner_out/*inner_size_out point into `buffer` at the wrapped
 * datagram. Self-forwards (src == ctx->local_player) are rejected. */
NBNetPacketStatus nbNetPacketParseForward(const u8 *buffer, s32 size,
										 const NBNetPacketContext *ctx,
										 u8 *src_player_out,
										 const u8 **inner_out, u32 *inner_size_out);

/* Cheap type sniff: is this a FORWARD datagram? (magic+version+type only).
 * Returns 1/0. */
s32 nbNetPacketIsForward(const u8 *buffer, s32 size);

/* ── Shared 12-byte prefix used by BOOTSTRAP/MATCH_CONFIG ────────────── */
void nbNetPacketWritePrefix(u8 **cursor, u16 packet_type, u32 session_id);
NBNetPacketStatus nbNetPacketParsePrefix(const u8 *buffer, s32 size,
										u16 *packet_type_out, u32 *session_id_out,
										const u8 **payload_out);

#ifdef __cplusplus
}
#endif

#endif /* NB_NETPACKET_H */
