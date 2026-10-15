/*
 * netpacket_test.c — pure codec suite for the SSNP v4 wire format
 * (src/networking/netplay/protocol/netpacket.*). Covers the NET-PROTO block of
 * docs/testing/test-catalog.md: length/short-buffer rejection, frame-count
 * boundaries, magic/version/session/checksum rejection, round trips for
 * INPUT/CONTROL/FORWARD, and FORWARD-specific guards.
 *
 * Self-contained: links only netpacket.c, no game state.
 */

#include "networking/netplay/protocol/netpacket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT(cond) \
	do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); exit(1); } } while (0)

static NBNetPacketContext ctx(u32 session, s32 src, s32 local, s32 forwarded)
{
	NBNetPacketContext c;

	c.session_id = session;
	c.src_player = src;
	c.local_player = local;
	c.forwarded = forwarded;
	c.ignore_session = 0;
	c.ignore_checksum = 0;
	return c;
}

static NBNetPacketInput make_input(void)
{
	NBNetPacketInput in;
	s32 i;

	memset(&in, 0, sizeof(in));
	in.remote_advantage = -3;
	in.session_id = 0xC0FFEE;
	in.ack_tick = 999;
	in.seq = 42;
	in.player = 1;
	in.local_player = 1;
	in.remote_player = 0;
	in.frame_count = 4;
	for (i = 0; i < in.frame_count; i++)
	{
		in.frames[i].tick = 1000 + (u32)i;
		in.frames[i].buttons = (u16)(0x8000 >> i);
		in.frames[i].stick_x = (s8)(i - 2);
		in.frames[i].stick_y = (s8)(2 - i);
	}
	in.check_tick = 777;
	in.check_hash = 0xDEADBEEF;
	return in;
}

/* ── FNV-1a ─────────────────────────────────────────────────────────── */

static void test_checksum_vectors(void)
{
	/* FNV-1a("a") is a fixed published value. */
	EXPECT(nbNetPacketChecksumBytes(NULL, 0) == 2166136261U);
	EXPECT(nbNetPacketChecksumBytes((const u8 *)"a", 1) == 0xE40C292CU);
}

/* ── INPUT round trip + rejections ──────────────────────────────────── */

static void test_input_round_trip(void)
{
	u8 buf[NB_NETPACKET_PACKET_BYTES];
	NBNetPacketInput out;
	NBNetPacketInput in = make_input();
	NBNetPacketContext c = ctx(in.session_id, 1, 0, 0);

	EXPECT(nbNetPacketBuildInput(buf, &in) == NB_NETPACKET_PACKET_BYTES);
	EXPECT(nbNetPacketParseInput(buf, sizeof(buf), &c, &out) == NB_NETPACKET_OK);
	EXPECT(out.session_id == in.session_id);
	EXPECT(out.ack_tick == in.ack_tick);
	EXPECT(out.seq == in.seq);
	EXPECT(out.player == in.player);
	EXPECT(out.frame_count == in.frame_count);
	EXPECT(out.local_player == in.local_player);
	EXPECT(out.remote_player == in.remote_player);
	EXPECT(out.remote_advantage == in.remote_advantage);
	EXPECT(out.check_tick == in.check_tick);
	EXPECT(out.check_hash == in.check_hash);
	EXPECT(memcmp(out.frames, in.frames, sizeof(in.frames)) == 0);
}

static void test_input_size_gates(void)
{
	u8 buf[NB_NETPACKET_PACKET_BYTES];
	NBNetPacketInput in = make_input();
	NBNetPacketContext c = ctx(in.session_id, 1, 0, 0);

	nbNetPacketBuildInput(buf, &in);
	EXPECT(nbNetPacketParseInput(buf, 0, &c, NULL) == NB_NETPACKET_BAD_SIZE);
	EXPECT(nbNetPacketParseInput(buf, NB_NETPACKET_PACKET_BYTES - 1, &c, NULL)
		   == NB_NETPACKET_BAD_SIZE);
	EXPECT(nbNetPacketParseInput(buf, NB_NETPACKET_PACKET_BYTES + 1, &c, NULL)
		   == NB_NETPACKET_BAD_SIZE);
	/* Validation-only call: NULL out pointer is accepted. */
	EXPECT(nbNetPacketParseInput(buf, sizeof(buf), NULL, NULL) == NB_NETPACKET_OK);
}

static void test_input_field_rejection(void)
{
	u8 buf[NB_NETPACKET_PACKET_BYTES];
	NBNetPacketInput in = make_input();
	NBNetPacketContext c = ctx(in.session_id, 1, 0, 0);

	nbNetPacketBuildInput(buf, &in);

	buf[0] ^= 0xFF;   /* magic lives at offset 0, big-endian */
	EXPECT(nbNetPacketParseInput(buf, sizeof(buf), &c, NULL) == NB_NETPACKET_BAD_MAGIC);
	nbNetPacketBuildInput(buf, &in);

	buf[5] = 3;       /* version low byte (BE u16 at 4..5) */
	EXPECT(nbNetPacketParseInput(buf, sizeof(buf), &c, NULL) == NB_NETPACKET_BAD_VERSION);
	nbNetPacketBuildInput(buf, &in);

	c = ctx(in.session_id + 1, 1, 0, 0);
	EXPECT(nbNetPacketParseInput(buf, sizeof(buf), &c, NULL) == NB_NETPACKET_BAD_SESSION);

	c = ctx(in.session_id, 2, 0, 0);   /* wrong src slot */
	EXPECT(nbNetPacketParseInput(buf, sizeof(buf), &c, NULL) == NB_NETPACKET_BAD_PLAYER);

	c = ctx(in.session_id, 1, 3, 0);   /* packet not addressed to us */
	EXPECT(nbNetPacketParseInput(buf, sizeof(buf), &c, NULL) == NB_NETPACKET_BAD_REMOTE);

	/* Forwarded datagrams skip the remote_player binding. */
	c = ctx(in.session_id, 1, 3, 1);
	EXPECT(nbNetPacketParseInput(buf, sizeof(buf), &c, NULL) == NB_NETPACKET_OK);
}

static void test_input_frame_count_boundary(void)
{
	u8 buf[NB_NETPACKET_PACKET_BYTES];
	NBNetPacketInput in = make_input();
	NBNetPacketContext c;

	in.frame_count = 0;
	nbNetPacketBuildInput(buf, &in);
	c = ctx(in.session_id, 1, 0, 0);
	EXPECT(nbNetPacketParseInput(buf, sizeof(buf), &c, NULL) == NB_NETPACKET_OK);

	in.frame_count = 16;
	nbNetPacketBuildInput(buf, &in);
	EXPECT(nbNetPacketParseInput(buf, sizeof(buf), &c, NULL) == NB_NETPACKET_OK);

	/* 17 and 255 don't round-trip through the builder (u8 field, builder
	 * writes it verbatim) — mutate the count byte and re-checksum so only
	 * the bound is violated. */
	buf[21] = 17;   /* frame_count byte: player@20, count@21 */
	{
		u32 sum = nbNetPacketChecksumBytes(buf, sizeof(buf) - 4);
		u8 *tail = buf + sizeof(buf) - 4;
		nbNetPacketWriteU32(&tail, sum);
	}
	EXPECT(nbNetPacketParseInput(buf, sizeof(buf), NULL, NULL) == NB_NETPACKET_BAD_FRAME_COUNT);

	buf[21] = 255;
	{
		u32 sum = nbNetPacketChecksumBytes(buf, sizeof(buf) - 4);
		u8 *tail = buf + sizeof(buf) - 4;
		nbNetPacketWriteU32(&tail, sum);
	}
	EXPECT(nbNetPacketParseInput(buf, sizeof(buf), NULL, NULL) == NB_NETPACKET_BAD_FRAME_COUNT);
}

static void test_input_checksum_rejection(void)
{
	u8 buf[NB_NETPACKET_PACKET_BYTES];
	NBNetPacketInput in = make_input();
	NBNetPacketContext c = ctx(in.session_id, 1, 0, 0);
	s32 i;

	nbNetPacketBuildInput(buf, &in);
	/* Flip every byte one at a time: the datagram must be rejected — by a
	 * dedicated field status or by the trailing checksum. */
	for (i = 0; i < NB_NETPACKET_PACKET_BYTES - 4; i++)
	{
		u8 saved = buf[i];

		buf[i] ^= 0x01;
		EXPECT(nbNetPacketParseInput(buf, sizeof(buf), &c, NULL) != NB_NETPACKET_OK);
		buf[i] = saved;
	}
}

static void test_input_mutant_flags(void)
{
	u8 buf[NB_NETPACKET_PACKET_BYTES];
	NBNetPacketInput in = make_input();
	NBNetPacketContext c = ctx(in.session_id, 1, 0, 0);

	nbNetPacketBuildInput(buf, &in);
	c.ignore_session = 1;
	c.session_id = 0;   /* mismatch that must now pass */
	EXPECT(nbNetPacketParseInput(buf, sizeof(buf), &c, NULL) == NB_NETPACKET_OK);

	buf[30] ^= 0xFF;
	c = ctx(in.session_id, 1, 0, 0);
	c.ignore_checksum = 1;
	EXPECT(nbNetPacketParseInput(buf, sizeof(buf), &c, NULL) == NB_NETPACKET_OK);
}

/* ── CONTROL ────────────────────────────────────────────────────────── */

static void test_control_round_trip(void)
{
	u8 buf[NB_NETPACKET_CONTROL_PACKET_BYTES];

	EXPECT(nbNetPacketBuildControl(buf, NB_NETPACKET_PACKET_BATTLE_START, 0xABCD)
		   == NB_NETPACKET_CONTROL_PACKET_BYTES);
	EXPECT(nbNetPacketParseControl(buf, sizeof(buf), 0xABCD)
		   == NB_NETPACKET_PACKET_BATTLE_START);
}

static void test_control_rejection(void)
{
	u8 buf[NB_NETPACKET_CONTROL_PACKET_BYTES];

	nbNetPacketBuildControl(buf, NB_NETPACKET_PACKET_READY, 0xABCD);
	EXPECT(nbNetPacketParseControl(buf, sizeof(buf) - 1, 0xABCD)
		   == -NB_NETPACKET_BAD_SIZE);
	EXPECT(nbNetPacketParseControl(buf, sizeof(buf), 0xABCD + 1)
		   == -NB_NETPACKET_BAD_SESSION);
	buf[3] ^= 0xFF;
	EXPECT(nbNetPacketParseControl(buf, sizeof(buf), 0xABCD)
		   == -NB_NETPACKET_BAD_CHECKSUM);
	nbNetPacketBuildControl(buf, NB_NETPACKET_PACKET_READY, 0xABCD);
	buf[0] ^= 0xFF;
	EXPECT(nbNetPacketParseControl(buf, sizeof(buf), 0xABCD)
		   == -NB_NETPACKET_BAD_CHECKSUM); /* checksum first */
}

/* ── DELAY ──────────────────────────────────────────────────────────── */

static void test_delay_round_trip(void)
{
	u8 buf[NB_NETPACKET_DELAY_PACKET_BYTES];
	u32 delay = 0;

	EXPECT(nbNetPacketBuildDelay(buf, 0xABCD, 3) == NB_NETPACKET_DELAY_PACKET_BYTES);
	EXPECT(nbNetPacketParseDelay(buf, sizeof(buf), 0xABCD, &delay) == NB_NETPACKET_OK);
	EXPECT(delay == 3);
	/* NULL out-param is a validation-only call. */
	EXPECT(nbNetPacketParseDelay(buf, sizeof(buf), 0xABCD, NULL) == NB_NETPACKET_OK);
}

static void test_delay_rejection(void)
{
	u8 buf[NB_NETPACKET_DELAY_PACKET_BYTES];
	u32 delay;

	nbNetPacketBuildDelay(buf, 0xABCD, 2);
	EXPECT(nbNetPacketParseDelay(buf, sizeof(buf) - 1, 0xABCD, &delay)
		   == -NB_NETPACKET_BAD_SIZE);
	EXPECT(nbNetPacketParseDelay(buf, sizeof(buf) + 1, 0xABCD, &delay)
		   == -NB_NETPACKET_BAD_SIZE);
	EXPECT(nbNetPacketParseDelay(buf, sizeof(buf), 0xABCD + 1, &delay)
		   == -NB_NETPACKET_BAD_SESSION);
	buf[3] ^= 0xFF;
	EXPECT(nbNetPacketParseDelay(buf, sizeof(buf), 0xABCD, &delay)
		   == -NB_NETPACKET_BAD_CHECKSUM);
	nbNetPacketBuildDelay(buf, 0xABCD, 2);
	buf[4] ^= 0xFF; /* version byte — checksum still fails first */
	EXPECT(nbNetPacketParseDelay(buf, sizeof(buf), 0xABCD, &delay)
		   == -NB_NETPACKET_BAD_CHECKSUM);
}

/* ── SPEC_PROGRESS ──────────────────────────────────────────────────── */

static void test_spec_progress_round_trip(void)
{
	u8 buf[NB_NETPACKET_DELAY_PACKET_BYTES];
	u32 tick = 0;

	EXPECT(nbNetPacketBuildSpecProgress(buf, 0xABCD, 12345) == NB_NETPACKET_DELAY_PACKET_BYTES);
	EXPECT(nbNetPacketParseSpecProgress(buf, sizeof(buf), 0xABCD, &tick) == NB_NETPACKET_OK);
	EXPECT(tick == 12345);
	EXPECT(nbNetPacketParseSpecProgress(buf, sizeof(buf), 0xABCD, NULL) == NB_NETPACKET_OK);
	/* a SPEC_PROGRESS datagram must not validate as DELAY and vice versa */
	EXPECT(nbNetPacketParseDelay(buf, sizeof(buf), 0xABCD, &tick)
		   == -NB_NETPACKET_BAD_TYPE);
}

static void test_spec_progress_rejection(void)
{
	u8 buf[NB_NETPACKET_DELAY_PACKET_BYTES];
	u32 tick;

	nbNetPacketBuildSpecProgress(buf, 0xABCD, 7);
	EXPECT(nbNetPacketParseSpecProgress(buf, sizeof(buf) - 1, 0xABCD, &tick)
		   == -NB_NETPACKET_BAD_SIZE);
	EXPECT(nbNetPacketParseSpecProgress(buf, sizeof(buf) + 1, 0xABCD, &tick)
		   == -NB_NETPACKET_BAD_SIZE);
	EXPECT(nbNetPacketParseSpecProgress(buf, sizeof(buf), 0xABCD + 1, &tick)
		   == -NB_NETPACKET_BAD_SESSION);
	buf[3] ^= 0xFF;
	EXPECT(nbNetPacketParseSpecProgress(buf, sizeof(buf), 0xABCD, &tick)
		   == -NB_NETPACKET_BAD_CHECKSUM);
}

/* ── FORWARD ────────────────────────────────────────────────────────── */

static void test_forward_round_trip(void)
{
	u8 inner[NB_NETPACKET_PACKET_BYTES];
	u8 wrap[NB_NETPACKET_FORWARD_PACKET_BYTES];
	NBNetPacketInput in = make_input();
	NBNetPacketContext c = ctx(in.session_id, -1, 0, 1);
	const u8 *inner_out = NULL;
	u32 inner_size = 0;
	u8 src = 0xFF;
	u32 wrap_size;

	nbNetPacketBuildInput(inner, &in);
	wrap_size = nbNetPacketBuildForward(wrap, sizeof(wrap), inner, sizeof(inner),
										(u8)in.player, in.session_id);
	EXPECT(wrap_size == sizeof(inner) + NB_NETPACKET_FORWARD_HEADER_BYTES + 4);
	EXPECT(nbNetPacketIsForward(wrap, (s32)wrap_size) == 1);
	EXPECT(nbNetPacketIsForward(inner, sizeof(inner)) == 0);

	EXPECT(nbNetPacketParseForward(wrap, (s32)wrap_size, &c, &src,
								   &inner_out, &inner_size) == NB_NETPACKET_OK);
	EXPECT(src == in.player);
	EXPECT(inner_size == sizeof(inner));
	EXPECT(memcmp(inner_out, inner, inner_size) == 0);
}

static void test_forward_rejection(void)
{
	u8 inner[NB_NETPACKET_PACKET_BYTES];
	u8 wrap[NB_NETPACKET_FORWARD_PACKET_BYTES];
	NBNetPacketInput in = make_input();
	NBNetPacketContext c;
	const u8 *inner_out;
	u32 inner_size;
	u8 src;
	u32 wrap_size;

	nbNetPacketBuildInput(inner, &in);
	wrap_size = nbNetPacketBuildForward(wrap, sizeof(wrap), inner, sizeof(inner),
										(u8)in.player, in.session_id);
	inner_out = NULL;
	inner_size = 0;
	src = 0;

	/* Self-forward: claimed sender == our slot. */
	c = ctx(in.session_id, -1, (s32)in.player, 1);
	EXPECT(nbNetPacketParseForward(wrap, (s32)wrap_size, &c, &src,
								   &inner_out, &inner_size)
		   == NB_NETPACKET_SELF_FORWARD);

	c = ctx(in.session_id + 1, -1, 0, 1);
	EXPECT(nbNetPacketParseForward(wrap, (s32)wrap_size, &c, NULL, NULL, NULL)
		   == NB_NETPACKET_BAD_SESSION);

	wrap[0] ^= 0xFF;
	c = ctx(in.session_id, -1, 0, 1);
	EXPECT(nbNetPacketParseForward(wrap, (s32)wrap_size, &c, NULL, NULL, NULL)
		   == NB_NETPACKET_BAD_MAGIC);

	nbNetPacketBuildForward(wrap, sizeof(wrap), inner, sizeof(inner),
							(u8)in.player, in.session_id);
	wrap[NB_NETPACKET_FORWARD_HEADER_BYTES] ^= 0xFF; /* corrupt inner body */
	EXPECT(nbNetPacketParseForward(wrap, (s32)wrap_size, &c, NULL, NULL, NULL)
		   == NB_NETPACKET_BAD_CHECKSUM);

	/* Truncated envelope and oversized inner are both refused. */
	EXPECT(nbNetPacketParseForward(wrap, NB_NETPACKET_FORWARD_HEADER_BYTES, &c,
								   NULL, NULL, NULL) == NB_NETPACKET_SHORT);
	EXPECT(nbNetPacketBuildForward(wrap, 8, inner, sizeof(inner), 1, 1) == 0);
	EXPECT(nbNetPacketBuildForward(wrap, sizeof(wrap), inner, 0, 1, 1) == 0);
}

/* ── BOOTSTRAP prefix (RT-001, MATCH_CONFIG-shaped) ─────────────────── */

static void test_bootstrap_prefix(void)
{
	/* BOOTSTRAP = 12B prefix + metadata payload + checksum. The codec owns
	 * the prefix; the metadata payload belongs to netpeer. Round-trip the
	 * codec half: prefix out must parse back and expose the payload. */
	u8 buf[12 + 64 + 4];
	u8 *w = buf;
	const u8 *payload = NULL;
	u16 type = 0;
	u32 session = 0;
	u32 sum;
	s32 i;

	for (i = 0; i < 64; i++)
	{
		buf[12 + i] = (u8)(i * 7 + 1);
	}
	nbNetPacketWritePrefix(&w, NB_NETPACKET_PACKET_MATCH_CONFIG, 0x5E55);
	w += 64;
	sum = nbNetPacketChecksumBytes(buf, (u32)(w - buf));
	nbNetPacketWriteU32(&w, sum);
	EXPECT((u32)(w - buf) == sizeof(buf));

	EXPECT(nbNetPacketParsePrefix(buf, (s32)sizeof(buf), &type, &session,
								  &payload) == NB_NETPACKET_OK);
	EXPECT(type == NB_NETPACKET_PACKET_MATCH_CONFIG);
	EXPECT(session == 0x5E55);
	EXPECT(payload == buf + 12);
	for (i = 0; i < 64; i++)
	{
		EXPECT(payload[i] == (u8)(i * 7 + 1));
	}

	EXPECT(nbNetPacketParsePrefix(buf, 11, NULL, NULL, NULL) == NB_NETPACKET_SHORT);
	buf[0] ^= 0xFF;
	EXPECT(nbNetPacketParsePrefix(buf, (s32)sizeof(buf), NULL, NULL, NULL)
		   == NB_NETPACKET_BAD_MAGIC);
}

/* ── TYPE-001: every control-type byte 0..255 ───────────────────────── */

static void test_type_enumeration(void)
{
	u8 buf[NB_NETPACKET_CONTROL_PACKET_BYTES];
	s32 t;

	/* The codec is type-agnostic (dispatch lives in netpeer): every
	 * well-formed CONTROL packet parses to its declared type, known or
	 * not. Malformed envelopes still reject at every type value. */
	for (t = 0; t <= 255; t++)
	{
		nbNetPacketBuildControl(buf, (u16)t, 0xABCD);
		EXPECT(nbNetPacketParseControl(buf, sizeof(buf), 0xABCD) == t);
		buf[3] ^= 0x01;
		EXPECT(nbNetPacketParseControl(buf, sizeof(buf), 0xABCD)
			   == -NB_NETPACKET_BAD_CHECKSUM);
	}
	/* IsForward only fires on the real FORWARD type: mutate the type
	 * field of a real forward envelope through every value. */
	{
		u8 inner[NB_NETPACKET_PACKET_BYTES];
		u8 wrap[NB_NETPACKET_FORWARD_PACKET_BYTES];
		NBNetPacketInput in = make_input();
		u32 wrap_size;

		nbNetPacketBuildInput(inner, &in);
		wrap_size = nbNetPacketBuildForward(wrap, sizeof(wrap), inner,
											sizeof(inner), 1, in.session_id);
		for (t = 0; t <= 255; t++)
		{
			wrap[6] = (u8)(t >> 8);
			wrap[7] = (u8)(t & 0xFF);
			EXPECT(nbNetPacketIsForward(wrap, (s32)wrap_size)
				   == (t == NB_NETPACKET_PACKET_FORWARD ? 1 : 0));
		}
	}
	/* Regression: INPUT keeps remote_advantage at the FORWARD type
	 * offset — advantage == PACKET_FORWARD must not alias the packet
	 * as a forward (it starved peers once a host lead reached 6). */
	{
		u8 buf[NB_NETPACKET_PACKET_BYTES];
		NBNetPacketInput in = make_input();

		in.remote_advantage = NB_NETPACKET_PACKET_FORWARD;
		nbNetPacketBuildInput(buf, &in);
		EXPECT(nbNetPacketIsForward(buf, (s32)sizeof(buf)) == 0);
	}
}

/* ── CUR-001: corpus of valid messages + invalid siblings ───────────── */

static void test_corpus(void)
{
	static const u16 k_control_types[] =
	{
		NB_NETPACKET_PACKET_MATCH_CONFIG,
		NB_NETPACKET_PACKET_READY,
		NB_NETPACKET_PACKET_START,
		NB_NETPACKET_PACKET_BATTLE_READY,
		NB_NETPACKET_PACKET_BATTLE_START,
	};
	u8 buf[NB_NETPACKET_FORWARD_PACKET_BYTES];
	u8 inner[NB_NETPACKET_PACKET_BYTES];
	NBNetPacketInput in = make_input();
	NBNetPacketContext c;
	u32 i;

	/* Valid siblings: every control type, INPUT, FORWARD. */
	for (i = 0; i < sizeof(k_control_types) / sizeof(k_control_types[0]); i++)
	{
		nbNetPacketBuildControl(buf, k_control_types[i], in.session_id);
		EXPECT(nbNetPacketParseControl(buf, NB_NETPACKET_CONTROL_PACKET_BYTES,
									   in.session_id)
			   == (s32)k_control_types[i]);
	}
	nbNetPacketBuildInput(inner, &in);
	c = ctx(in.session_id, 1, 0, 0);
	EXPECT(nbNetPacketParseInput(inner, sizeof(inner), &c, NULL) == NB_NETPACKET_OK);

	/* Invalid siblings of the valid INPUT: magic, version, session,
	 * checksum — each rejected with its dedicated status. */
	memcpy(buf, inner, sizeof(inner));
	buf[0] ^= 0xFF;
	EXPECT(nbNetPacketParseInput(buf, sizeof(inner), &c, NULL) == NB_NETPACKET_BAD_MAGIC);
	memcpy(buf, inner, sizeof(inner));
	buf[5] = 0;
	EXPECT(nbNetPacketParseInput(buf, sizeof(inner), &c, NULL) == NB_NETPACKET_BAD_VERSION);
	memcpy(buf, inner, sizeof(inner));
	buf[8] ^= 0xFF;
	EXPECT(nbNetPacketParseInput(buf, sizeof(inner), &c, NULL) == NB_NETPACKET_BAD_SESSION);
	memcpy(buf, inner, sizeof(inner));
	buf[sizeof(inner) - 1] ^= 0xFF;
	EXPECT(nbNetPacketParseInput(buf, sizeof(inner), &c, NULL) == NB_NETPACKET_BAD_CHECKSUM);
}

/* ── FUZZ-001: seeded arbitrary datagrams — no crash, clean rejects ─── */

static u32 s_rng = 0x243F6A88U;

static u32 rng(void)
{
	/* xorshift32 — deterministic across platforms. */
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

static void test_fuzz_arbitrary(void)
{
	u8 buf[NB_NETPACKET_FORWARD_PACKET_BYTES + 64];
	u8 inner[NB_NETPACKET_PACKET_BYTES];
	NBNetPacketInput in = make_input();
	NBNetPacketContext c = ctx(in.session_id, 1, 0, 0);
	u32 i;
	u32 size;
	u32 accepted = 0;
	s32 it;

	nbNetPacketBuildInput(inner, &in);

	/* Pass 1: 10k fully arbitrary datagrams (empty → oversized). Every
	 * parser must reject cleanly — the contract is "no crash, no OOB". */
	for (i = 0; i < 10000; i++)
	{
		size = rng() % sizeof(buf);
		for (it = 0; it < (s32)size; it++)
		{
			buf[it] = (u8)rng();
		}
		if (nbNetPacketParseInput(buf, (s32)size, &c, NULL) == NB_NETPACKET_OK) accepted++;
		if (nbNetPacketParseControl(buf, (s32)size, in.session_id) >= 0) accepted++;
		if (nbNetPacketParseForward(buf, (s32)size, &c, NULL, NULL, NULL)
			== NB_NETPACKET_OK) accepted++;
		(void)nbNetPacketIsForward(buf, (s32)size);
		(void)nbNetPacketChecksumBytes(buf, size);
	}
	/* Random garbage almost never survives magic+version+checksum; a
	 * non-zero accept count isn't a bug, but a huge one would be. */
	EXPECT(accepted < 1000);

	/* Pass 2: 10k mutants of valid datagrams — 1..8 random byte edits at
	 * random offsets. Parse must terminate with a status, never crash. */
	for (i = 0; i < 10000; i++)
	{
		u32 edits = 1 + (rng() % 8);
		u32 e;

		memcpy(buf, inner, sizeof(inner));
		for (e = 0; e < edits; e++)
		{
			buf[rng() % sizeof(inner)] ^= (u8)(rng() | 1);
		}
		(void)nbNetPacketParseInput(buf, sizeof(inner), &c, NULL);
		(void)nbNetPacketParseInput(buf, sizeof(inner), NULL, NULL);
	}

	/* Pass 3: truncated prefixes of a valid INPUT — every length. */
	for (size = 0; size <= sizeof(inner); size++)
	{
		(void)nbNetPacketParseInput(inner, (s32)size, &c, NULL);
		(void)nbNetPacketParseControl(inner, (s32)size, in.session_id);
		(void)nbNetPacketParseForward(inner, (s32)size, &c, NULL, NULL, NULL);
	}
}

int main(void)
{
	test_checksum_vectors();
	test_input_round_trip();
	test_input_size_gates();
	test_input_field_rejection();
	test_input_frame_count_boundary();
	test_input_checksum_rejection();
	test_input_mutant_flags();
	test_control_round_trip();
	test_control_rejection();
	test_delay_round_trip();
	test_delay_rejection();
	test_spec_progress_round_trip();
	test_spec_progress_rejection();
	test_forward_round_trip();
	test_forward_rejection();
	test_bootstrap_prefix();
	test_type_enumeration();
	test_corpus();
	test_fuzz_arbitrary();

	puts("PASS netpacket_test");
	return 0;
}
