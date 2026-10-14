/* net_packet_fuzzer.c — libFuzzer target for the SSNP wire codec.
 *
 * Feeds arbitrary bytes through every parse entry point of
 * src/netplay/protocol/netpacket.c. The codec is pure (no globals, no
 * I/O) so a crash or sanitizer hit here is always a real finding.
 *
 * Build:
 *   cmake -DSSB64_BUILD_TESTING=ON -DSSB64_BUILD_FUZZERS=ON -DCMAKE_C_COMPILER=clang ...
 *   ./net_packet_fuzzer tests/fuzz/corpora/packet_parser
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "netplay/protocol/netpacket.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	NBNetPacketContext ctx;
	NBNetPacketInput parsed_input;
	u8 src_player;
	const u8 *inner;
	u32 inner_size;

	/* Derive the validation context from the tail bytes so mutations
	 * flip which checks fire — session/player taken modulo range so
	 * both accept and reject paths stay reachable. */
	memset(&ctx, 0, sizeof(ctx));
	if (size >= 8)
	{
		memcpy(&ctx.session_id, data + size - 8, sizeof(ctx.session_id));
		ctx.src_player   = (s32)(data[size - 4] % 4);
		ctx.local_player = (s32)(data[size - 3] % 4);
		ctx.forwarded    = data[size - 2] & 1;
		/* ignore_session / ignore_checksum left 0 — bypassing them in
		 * the codec would hide real validation bugs from the fuzzer. */
	}

	(void)nbNetPacketParseInput(data, (s32)size, &ctx, &parsed_input);
	(void)nbNetPacketParseControl(data, (s32)size, ctx.session_id);
	{
		u32 delay;
		(void)nbNetPacketParseDelay(data, (s32)size, ctx.session_id, &delay);
	}
	(void)nbNetPacketParseForward(data, (s32)size, &ctx,
								  &src_player, &inner, &inner_size);
	(void)nbNetPacketIsForward(data, (s32)size);
	{
		u16 packet_type;
		u32 session_id;
		const u8 *payload;
		(void)nbNetPacketParsePrefix(data, (s32)size,
									 &packet_type, &session_id, &payload);
	}

	/* Exercise the checksum helpers on the raw buffer. */
	(void)nbNetPacketChecksumBytes(data, (u32)size);

	return 0;
}
