#include "netpacket.h"

#include <string.h>

/* FNV-1a over a byte range — authenticates the whole datagram. */
u32 nbNetPacketChecksumBytes(const u8 *bytes, u32 size)
{
	u32 checksum = NB_NETPACKET_FNV_BASIS;
	u32 i;

	for (i = 0; i < size; i++)
	{
		checksum ^= bytes[i];
		checksum *= NB_NETPACKET_FNV_PRIME;
	}
	return checksum;
}

u32 nbNetPacketChecksumAccumulateU32(u32 checksum, u32 value)
{
	checksum ^= value;
	checksum *= NB_NETPACKET_FNV_PRIME;
	return checksum;
}

u32 nbNetPacketChecksumAccumulateFrame(u32 checksum, const NBNetPacketFrame *frame)
{
	checksum = nbNetPacketChecksumAccumulateU32(checksum, frame->tick);
	checksum = nbNetPacketChecksumAccumulateU32(checksum, frame->buttons);
	checksum = nbNetPacketChecksumAccumulateU32(checksum, (u8)frame->stick_x);
	checksum = nbNetPacketChecksumAccumulateU32(checksum, (u8)frame->stick_y);
	return checksum;
}

void nbNetPacketWriteU8(u8 **cursor, u8 value)
{
	*(*cursor)++ = value;
}

void nbNetPacketWriteU16(u8 **cursor, u16 value)
{
	*(*cursor)++ = (value >> 8) & 0xFF;
	*(*cursor)++ = value & 0xFF;
}

void nbNetPacketWriteU32(u8 **cursor, u32 value)
{
	*(*cursor)++ = (value >> 24) & 0xFF;
	*(*cursor)++ = (value >> 16) & 0xFF;
	*(*cursor)++ = (value >> 8) & 0xFF;
	*(*cursor)++ = value & 0xFF;
}

u8 nbNetPacketReadU8(const u8 **cursor)
{
	return *(*cursor)++;
}

u16 nbNetPacketReadU16(const u8 **cursor)
{
	u16 value = (u16)(*(*cursor)++) << 8;

	value |= *(*cursor)++;
	return value;
}

u32 nbNetPacketReadU32(const u8 **cursor)
{
	u32 value = (u32)(*(*cursor)++) << 24;

	value |= (u32)(*(*cursor)++) << 16;
	value |= (u32)(*(*cursor)++) << 8;
	value |= *(*cursor)++;
	return value;
}

u32 nbNetPacketBuildInput(u8 *buffer, const NBNetPacketInput *in)
{
	static const NBNetPacketFrame zero_frame = { 0, 0, 0, 0 };
	u8 *cursor = buffer;
	u32 checksum;
	s32 i;

	nbNetPacketWriteU32(&cursor, NB_NETPACKET_MAGIC);
	nbNetPacketWriteU16(&cursor, NB_NETPACKET_VERSION);
	nbNetPacketWriteU16(&cursor, (u16)in->remote_advantage);
	nbNetPacketWriteU32(&cursor, in->session_id);
	nbNetPacketWriteU32(&cursor, in->ack_tick);
	nbNetPacketWriteU32(&cursor, in->seq);
	nbNetPacketWriteU8(&cursor, in->player);
	nbNetPacketWriteU8(&cursor, in->frame_count);
	nbNetPacketWriteU8(&cursor, in->local_player);
	nbNetPacketWriteU8(&cursor, in->remote_player);
	for (i = 0; i < NB_NETPACKET_MAX_PACKET_FRAMES; i++)
	{
		const NBNetPacketFrame *frame =
			(i < in->frame_count) ? &in->frames[i] : &zero_frame;

		nbNetPacketWriteU32(&cursor, frame->tick);
		nbNetPacketWriteU16(&cursor, frame->buttons);
		nbNetPacketWriteU8(&cursor, (u8)frame->stick_x);
		nbNetPacketWriteU8(&cursor, (u8)frame->stick_y);
	}
	nbNetPacketWriteU32(&cursor, in->check_tick);
	nbNetPacketWriteU32(&cursor, in->check_hash);
	checksum = nbNetPacketChecksumBytes(buffer, NB_NETPACKET_PACKET_BYTES - 4);
	nbNetPacketWriteU32(&cursor, checksum);
	return NB_NETPACKET_PACKET_BYTES;
}

static NBNetPacketStatus nbNetPacketReadHeader(const u8 **cursor, NBNetPacketInput *parsed)
{
	if (nbNetPacketReadU32(cursor) != NB_NETPACKET_MAGIC)
	{
		return NB_NETPACKET_BAD_MAGIC;
	}
	if (nbNetPacketReadU16(cursor) != NB_NETPACKET_VERSION)
	{
		return NB_NETPACKET_BAD_VERSION;
	}
	parsed->remote_advantage = (s16)nbNetPacketReadU16(cursor);
	parsed->session_id = nbNetPacketReadU32(cursor);
	parsed->ack_tick = nbNetPacketReadU32(cursor);
	parsed->seq = nbNetPacketReadU32(cursor);
	parsed->player = nbNetPacketReadU8(cursor);
	parsed->frame_count = nbNetPacketReadU8(cursor);
	parsed->local_player = nbNetPacketReadU8(cursor);
	parsed->remote_player = nbNetPacketReadU8(cursor);
	return NB_NETPACKET_OK;
}

static NBNetPacketStatus nbNetPacketValidateCtx(const NBNetPacketInput *parsed,
												const NBNetPacketContext *ctx)
{
	if (ctx == NULL)
	{
		return NB_NETPACKET_OK;
	}
	if ((ctx->ignore_session == 0) && (parsed->session_id != ctx->session_id))
	{
		return NB_NETPACKET_BAD_SESSION;
	}
	if ((parsed->player != (u8)ctx->src_player) ||
		(parsed->local_player != (u8)ctx->src_player))
	{
		return NB_NETPACKET_BAD_PLAYER;
	}
	if ((ctx->forwarded == 0) && (parsed->remote_player != (u8)ctx->local_player))
	{
		return NB_NETPACKET_BAD_REMOTE;
	}
	return NB_NETPACKET_OK;
}

static void nbNetPacketReadFrames(const u8 **cursor, NBNetPacketInput *parsed)
{
	s32 i;

	for (i = 0; i < NB_NETPACKET_MAX_PACKET_FRAMES; i++)
	{
		parsed->frames[i].tick = nbNetPacketReadU32(cursor);
		parsed->frames[i].buttons = nbNetPacketReadU16(cursor);
		parsed->frames[i].stick_x = (s8)nbNetPacketReadU8(cursor);
		parsed->frames[i].stick_y = (s8)nbNetPacketReadU8(cursor);
	}
	parsed->check_tick = nbNetPacketReadU32(cursor);
	parsed->check_hash = nbNetPacketReadU32(cursor);
}

NBNetPacketStatus nbNetPacketParseInput(const u8 *buffer, s32 size,
										const NBNetPacketContext *ctx,
										NBNetPacketInput *out)
{
	const u8 *cursor = buffer;
	NBNetPacketInput parsed;
	NBNetPacketStatus status;
	u32 checksum;
	u32 expected_checksum;

	if (size != NB_NETPACKET_PACKET_BYTES)
	{
		return NB_NETPACKET_BAD_SIZE;
	}
	memset(&parsed, 0, sizeof(parsed));

	status = nbNetPacketReadHeader(&cursor, &parsed);
	if (status != NB_NETPACKET_OK)
	{
		return status;
	}
	status = nbNetPacketValidateCtx(&parsed, ctx);
	if (status != NB_NETPACKET_OK)
	{
		return status;
	}
	if (parsed.frame_count > NB_NETPACKET_MAX_PACKET_FRAMES)
	{
		return NB_NETPACKET_BAD_FRAME_COUNT;
	}
	nbNetPacketReadFrames(&cursor, &parsed);

	checksum = nbNetPacketReadU32(&cursor);
	expected_checksum = nbNetPacketChecksumBytes(buffer,
											   NB_NETPACKET_PACKET_BYTES - 4);
	if (((ctx == NULL) || (ctx->ignore_checksum == 0)) &&
		(checksum != expected_checksum))
	{
		return NB_NETPACKET_BAD_CHECKSUM;
	}
	if (out != NULL)
	{
		*out = parsed;
	}
	return NB_NETPACKET_OK;
}



u32 nbNetPacketBuildControl(u8 *buffer, u16 packet_type, u32 session_id)
{
	u8 *cursor = buffer;

	nbNetPacketWritePrefix(&cursor, packet_type, session_id);
	nbNetPacketWriteU32(&cursor, nbNetPacketChecksumBytes(buffer, (u32)(cursor - buffer)));
	return NB_NETPACKET_CONTROL_PACKET_BYTES;
}

s32 nbNetPacketParseControl(const u8 *buffer, s32 size, u32 session_id)
{
	const u8 *cursor = buffer;
	const u8 *check_cursor;
	u16 packet_type;
	u32 checksum;

	if (size != NB_NETPACKET_CONTROL_PACKET_BYTES)
	{
		return -(s32)NB_NETPACKET_BAD_SIZE;
	}
	check_cursor = buffer + NB_NETPACKET_CONTROL_PACKET_BYTES - 4;
	checksum = nbNetPacketReadU32(&check_cursor);
	if (nbNetPacketChecksumBytes(buffer, NB_NETPACKET_CONTROL_PACKET_BYTES - 4)
		!= checksum)
	{
		return -(s32)NB_NETPACKET_BAD_CHECKSUM;
	}
	if (nbNetPacketReadU32(&cursor) != NB_NETPACKET_MAGIC)
	{
		return -(s32)NB_NETPACKET_BAD_MAGIC;
	}
	if (nbNetPacketReadU16(&cursor) != NB_NETPACKET_VERSION)
	{
		return -(s32)NB_NETPACKET_BAD_VERSION;
	}
	packet_type = nbNetPacketReadU16(&cursor);
	if (nbNetPacketReadU32(&cursor) != session_id)
	{
		return -(s32)NB_NETPACKET_BAD_SESSION;
	}
	return (s32)packet_type;
}

u32 nbNetPacketBuildDelay(u8 *buffer, u32 session_id, u32 delay)
{
	u8 *cursor = buffer;

	nbNetPacketWritePrefix(&cursor, NB_NETPACKET_PACKET_DELAY, session_id);
	nbNetPacketWriteU32(&cursor, delay);
	nbNetPacketWriteU32(&cursor, nbNetPacketChecksumBytes(buffer, (u32)(cursor - buffer)));
	return NB_NETPACKET_DELAY_PACKET_BYTES;
}

s32 nbNetPacketParseDelay(const u8 *buffer, s32 size, u32 session_id, u32 *out_delay)
{
	const u8 *cursor = buffer;
	const u8 *check_cursor;
	u32 checksum;

	if (size != NB_NETPACKET_DELAY_PACKET_BYTES)
	{
		return -(s32)NB_NETPACKET_BAD_SIZE;
	}
	check_cursor = buffer + NB_NETPACKET_DELAY_PACKET_BYTES - 4;
	checksum = nbNetPacketReadU32(&check_cursor);
	if (nbNetPacketChecksumBytes(buffer, NB_NETPACKET_DELAY_PACKET_BYTES - 4)
	    != checksum)
	{
		return -(s32)NB_NETPACKET_BAD_CHECKSUM;
	}
	if (nbNetPacketReadU32(&cursor) != NB_NETPACKET_MAGIC)
	{
		return -(s32)NB_NETPACKET_BAD_MAGIC;
	}
	if (nbNetPacketReadU16(&cursor) != NB_NETPACKET_VERSION)
	{
		return -(s32)NB_NETPACKET_BAD_VERSION;
	}
	if (nbNetPacketReadU16(&cursor) != NB_NETPACKET_PACKET_DELAY)
	{
		return -(s32)NB_NETPACKET_BAD_TYPE;
	}
	if (nbNetPacketReadU32(&cursor) != session_id)
	{
		return -(s32)NB_NETPACKET_BAD_SESSION;
	}
	if (out_delay != NULL)
	{
		*out_delay = nbNetPacketReadU32(&cursor);
	}
	return NB_NETPACKET_OK;
}

u32 nbNetPacketBuildSpecProgress(u8 *buffer, u32 session_id, u32 tick)
{
	u8 *cursor = buffer;

	nbNetPacketWritePrefix(&cursor, NB_NETPACKET_PACKET_SPEC_PROGRESS, session_id);
	nbNetPacketWriteU32(&cursor, tick);
	nbNetPacketWriteU32(&cursor, nbNetPacketChecksumBytes(buffer, (u32)(cursor - buffer)));
	return NB_NETPACKET_DELAY_PACKET_BYTES;
}

s32 nbNetPacketParseSpecProgress(const u8 *buffer, s32 size, u32 session_id, u32 *out_tick)
{
	const u8 *cursor = buffer;
	const u8 *check_cursor;
	u32 checksum;

	if (size != NB_NETPACKET_DELAY_PACKET_BYTES)
	{
		return -(s32)NB_NETPACKET_BAD_SIZE;
	}
	check_cursor = buffer + NB_NETPACKET_DELAY_PACKET_BYTES - 4;
	checksum = nbNetPacketReadU32(&check_cursor);
	if (nbNetPacketChecksumBytes(buffer, NB_NETPACKET_DELAY_PACKET_BYTES - 4)
	    != checksum)
	{
		return -(s32)NB_NETPACKET_BAD_CHECKSUM;
	}
	if (nbNetPacketReadU32(&cursor) != NB_NETPACKET_MAGIC)
	{
		return -(s32)NB_NETPACKET_BAD_MAGIC;
	}
	if (nbNetPacketReadU16(&cursor) != NB_NETPACKET_VERSION)
	{
		return -(s32)NB_NETPACKET_BAD_VERSION;
	}
	if (nbNetPacketReadU16(&cursor) != NB_NETPACKET_PACKET_SPEC_PROGRESS)
	{
		return -(s32)NB_NETPACKET_BAD_TYPE;
	}
	if (nbNetPacketReadU32(&cursor) != session_id)
	{
		return -(s32)NB_NETPACKET_BAD_SESSION;
	}
	if (out_tick != NULL)
	{
		*out_tick = nbNetPacketReadU32(&cursor);
	}
	return NB_NETPACKET_OK;
}

u32 nbNetPacketBuildForward(u8 *buffer, u32 buffer_size,
							const u8 *inner, u32 inner_size,
							u8 src_player, u32 session_id)
{
	u8 *cursor = buffer;

	if ((inner_size <= 0) ||
		(inner_size + NB_NETPACKET_FORWARD_HEADER_BYTES + 4 > buffer_size))
	{
		return 0;
	}
	nbNetPacketWriteU32(&cursor, NB_NETPACKET_MAGIC);
	nbNetPacketWriteU16(&cursor, NB_NETPACKET_VERSION);
	nbNetPacketWriteU16(&cursor, NB_NETPACKET_PACKET_FORWARD);
	nbNetPacketWriteU32(&cursor, session_id);
	nbNetPacketWriteU8(&cursor, src_player);
	memcpy(cursor, inner, inner_size);
	cursor += inner_size;
	nbNetPacketWriteU32(&cursor, nbNetPacketChecksumBytes(buffer, (u32)(cursor - buffer)));
	return (u32)(cursor - buffer);
}

NBNetPacketStatus nbNetPacketParseForward(const u8 *buffer, s32 size,
										 const NBNetPacketContext *ctx,
										 u8 *src_player_out,
										 const u8 **inner_out, u32 *inner_size_out)
{
	const u8 *cursor = buffer;
	const u8 *check_cursor;
	u8 src_player;
	u16 packet_type;
	u32 session_id;
	u32 checksum;

	if (size < NB_NETPACKET_FORWARD_HEADER_BYTES + 4)
	{
		return NB_NETPACKET_SHORT;
	}
	if (nbNetPacketReadU32(&cursor) != NB_NETPACKET_MAGIC)
	{
		return NB_NETPACKET_BAD_MAGIC;
	}
	if (nbNetPacketReadU16(&cursor) != NB_NETPACKET_VERSION)
	{
		return NB_NETPACKET_BAD_VERSION;
	}
	packet_type = nbNetPacketReadU16(&cursor);
	if (packet_type != NB_NETPACKET_PACKET_FORWARD)
	{
		return NB_NETPACKET_BAD_TYPE;
	}
	session_id = nbNetPacketReadU32(&cursor);
	src_player = nbNetPacketReadU8(&cursor);
	check_cursor = buffer + size - 4;
	checksum = nbNetPacketReadU32(&check_cursor);

	if ((ctx != NULL) && (session_id != ctx->session_id))
	{
		return NB_NETPACKET_BAD_SESSION;
	}
	if (checksum != nbNetPacketChecksumBytes(buffer, size - 4))
	{
		return NB_NETPACKET_BAD_CHECKSUM;
	}
	if ((ctx != NULL) && (src_player == (u8)ctx->local_player))
	{
		return NB_NETPACKET_SELF_FORWARD;
	}
	if (src_player_out != NULL)
	{
		*src_player_out = src_player;
	}
	if (inner_out != NULL)
	{
		*inner_out = cursor;
	}
	if (inner_size_out != NULL)
	{
		*inner_size_out = (u32)(size - NB_NETPACKET_FORWARD_HEADER_BYTES - 4);
	}
	return NB_NETPACKET_OK;
}

s32 nbNetPacketIsForward(const u8 *buffer, s32 size)
{
	const u8 *cursor = buffer;

	if (size < NB_NETPACKET_FORWARD_HEADER_BYTES + 4)
	{
		return 0;
	}
	if ((nbNetPacketReadU32(&cursor) != NB_NETPACKET_MAGIC) ||
		(nbNetPacketReadU16(&cursor) != NB_NETPACKET_VERSION) ||
		(nbNetPacketReadU16(&cursor) != NB_NETPACKET_PACKET_FORWARD))
	{
		return 0;
	}
	/* INPUT datagrams keep remote_advantage where FORWARD stores its type:
	 * an advantage of exactly PACKET_FORWARD would alias the whole packet
	 * as a forward and starve the peer (observed freezing matches under
	 * latency once a host's lead reached 6). Envelopes only ever wrap a
	 * whole game datagram, so require the inner magic too - a real INPUT
	 * would need ack_tick >= 0x53534Exx (weeks of match) to collide. */
	cursor = buffer + NB_NETPACKET_FORWARD_HEADER_BYTES;
	return (nbNetPacketReadU32(&cursor) == NB_NETPACKET_MAGIC) ? 1 : 0;
}

void nbNetPacketWritePrefix(u8 **cursor, u16 packet_type, u32 session_id)
{
	nbNetPacketWriteU32(cursor, NB_NETPACKET_MAGIC);
	nbNetPacketWriteU16(cursor, NB_NETPACKET_VERSION);
	nbNetPacketWriteU16(cursor, packet_type);
	nbNetPacketWriteU32(cursor, session_id);
}

NBNetPacketStatus nbNetPacketParsePrefix(const u8 *buffer, s32 size,
										u16 *packet_type_out, u32 *session_id_out,
										const u8 **payload_out)
{
	const u8 *cursor = buffer;

	if (size < 12)
	{
		return NB_NETPACKET_SHORT;
	}
	if (nbNetPacketReadU32(&cursor) != NB_NETPACKET_MAGIC)
	{
		return NB_NETPACKET_BAD_MAGIC;
	}
	if (nbNetPacketReadU16(&cursor) != NB_NETPACKET_VERSION)
	{
		return NB_NETPACKET_BAD_VERSION;
	}
	if (packet_type_out != NULL)
	{
		*packet_type_out = nbNetPacketReadU16(&cursor);
	}
	else
	{
		nbNetPacketReadU16(&cursor);
	}
	if (session_id_out != NULL)
	{
		*session_id_out = nbNetPacketReadU32(&cursor);
	}
	else
	{
		nbNetPacketReadU32(&cursor);
	}
	if (payload_out != NULL)
	{
		*payload_out = cursor;
	}
	return NB_NETPACKET_OK;
}
