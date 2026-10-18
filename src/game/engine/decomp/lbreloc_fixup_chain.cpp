/**
 * lbreloc_fixup_chain.cpp — chain-walk fixups for in-file G_SETTIMG / G_VTX slots
 *
 * Split out of lbreloc_byteswap.cpp (phase-0 complexity refactor). The
 * shared tracking containers and bridge externs it uses live in
 * lbreloc_fixup_internal.h / lbreloc_fixup_state.cpp.
 */

#include "lbreloc_fixup_internal.h"
#include "lbreloc_debug.h"

#include <cstring>

// ============================================================
//  Chain-walk fixup (textures and vertices)
// ============================================================
//
// Pass2's seg==0x0E filter only catches a tiny minority of in-file textures
// and vertices.  SSB64 model DLs reference these via real-pointer slots that
// the reloc chain rewrites at load time.  Pass2 sees those slots BEFORE
// rewrite — they contain chain encoding (random-looking high bytes), not
// seg=0x0E references, so they get missed.
//
// Workaround: during the chain walk, for each chain entry whose preceding
// cmd is a G_SETTIMG or G_VTX, apply the appropriate fixup using the chain
// encoding's words_num field as the in-file target offset.  The chain knows
// the truth because it's an explicit list of "slots that need fixup" —
// no false positives possible.
//
// Args:
//   file_base       — start of the file in PC memory (post-pass1)
//   file_size       — size of the file blob
//   slot_byte_off   — byte offset of the cmd's w1 slot (the slot the
//                     chain entry refers to)
//   target_byte_off — byte offset of the target data within the same file
//                     (computed from chain entry's words_num * 4)
//
// Returns 1 if the slot was at a G_SETTIMG/G_VTX and a fixup was applied;
// 0 otherwise.

static int chain_fixup_settimg(void *file_base, size_t file_size,
                                unsigned int slot_byte_off,
                                unsigned int target_byte_off,
                                uint32_t w0)
{
	const uint8_t *file_bytes = static_cast<const uint8_t *>(file_base);

	uint32_t fmt = (w0 >> 21) & 0x07;
	uint32_t siz = (w0 >> 19) & 0x03;
	(void)fmt;

	// Walk forward up to 8 cmds (64 bytes) to find G_LOADBLOCK or G_LOADTLUT.
	uint32_t loadblock_w1 = 0;
	uint32_t loadtlut_w1  = 0;
	int      found_load   = 0;
	for (int step = 1; step <= 8; step++)
	{
		size_t walk_off = (size_t)slot_byte_off - 4 + (size_t)(step * 8);
		if (walk_off + 8 > file_size) break;
		uint32_t walk_w0 = *(const uint32_t *)(file_bytes + walk_off);
		uint32_t walk_w1 = *(const uint32_t *)(file_bytes + walk_off + 4);
		uint8_t  walk_op = (uint8_t)(walk_w0 >> 24);
		if (walk_op == GBI_LOADBLOCK)
		{
			loadblock_w1 = walk_w1;
			found_load = 1;
			break;
		}
		if (walk_op == GBI_LOADTLUT)
		{
			loadtlut_w1 = walk_w1;
			found_load = 2;
			break;
		}
		if (walk_op == GBI_SETTIMG) return 0;
	}
	if (!found_load) return 0;

	uint32_t tex_bytes = 0;
	if (found_load == 1)
	{
		uint32_t lrs = (loadblock_w1 >> 12) & 0xFFF;
		uint32_t num_texels = lrs + 1;
		uint32_t bpp = (siz == G_IM_SIZ_4b)  ? 4
		             : (siz == G_IM_SIZ_8b)  ? 8
		             : (siz == G_IM_SIZ_16b) ? 16
		             : (siz == G_IM_SIZ_32b) ? 32
		             : 0;
		if (bpp == 0) return 0;
		tex_bytes = (num_texels * bpp + 7) / 8;
	}
	else
	{
		uint32_t count = ((loadtlut_w1 >> 14) & 0x3FF) + 1;
		tex_bytes = count * 2;
	}
	tex_bytes = (tex_bytes + 3) & ~3u;
	if (tex_bytes == 0) return 0;
	if ((size_t)target_byte_off + tex_bytes > file_size)
	{
		tex_bytes = (uint32_t)(file_size - target_byte_off);
		tex_bytes &= ~3u;
		if (tex_bytes == 0) return 0;
	}

	uintptr_t fixup_key = reinterpret_cast<uintptr_t>(file_base) + (uintptr_t)target_byte_off;
	if (sStructU16Fixups.count(fixup_key)) return 1;
	sStructU16Fixups.insert(fixup_key);

	uint32_t *region = (uint32_t *)((uint8_t *)file_base + target_byte_off);
	size_t num_words = tex_bytes / 4;

	// All formats need BSWAP32 to restore N64 BE byte order:
	//  - 4b/8b: per-byte data; Fast3D reads bytes in their original order
	//  - 16b: pixels are BE u16; Fast3D reads `(addr[0] << 8) | addr[1]`
	//  - 32b: pixels are BE [R,G,B,A]; Fast3D's ImportTextureRgba32 reads
	//         addr[0]=R, addr[1]=G, addr[2]=B, addr[3]=A
	// Pass1 BSWAP32 reversed the byte order within each u32, so a second
	// BSWAP32 restores the original layout for ALL of these formats.
	//
	// Issue #4 (BTT/BTP arrow palette): we must record this fix in
	// sTexFixupExtent and sTexFixupWords too. Without those, the runtime
	// LOADTLUT path's early-skip — which fires when a target appears in
	// sStructU16Fixups but NOT sTexFixupExtent — locks out runtime fixup
	// on this target. That matters when the runtime LOADTLUT loads MORE
	// bytes than chain just fixed (e.g. a count=3 partial palette where
	// chain only saw a count=1 LOADTLUT in its 8-cmd window): bytes
	// beyond the chain-fixed range stay in pass1-BSWAP state and the
	// arrow's palette[2] reads as 0xC17B (magenta) instead of 0x0001
	// (black). With these two records, the runtime path now passes the
	// early-skip and falls through to the per-word loop, which skips
	// already-fixed words via sTexFixupWords and BSWAPs the rest.
	uintptr_t reg_base = reinterpret_cast<uintptr_t>(region);
	for (size_t i = 0; i < num_words; i++) {
		sTexFixupWords.insert(reg_base + i * 4);
		region[i] = BSWAP32(region[i]);
	}
	auto extent_it = sTexFixupExtent.find(reg_base);
	if (extent_it == sTexFixupExtent.end() || extent_it->second < tex_bytes)
		sTexFixupExtent[reg_base] = tex_bytes;

	// Diagnostic: record what was just fixed (chain-walk path).
	if (tex_log_enabled() || tex_dump_enabled()) {
		uintptr_t fb_addr = reinterpret_cast<uintptr_t>(file_base);
		int chain_file_id = portRelocFindFileIdAndBase(
			reinterpret_cast<const void *>(fb_addr), nullptr);
		uint32_t bpp_val = (siz == G_IM_SIZ_4b)  ? 4u
		                 : (siz == G_IM_SIZ_8b)  ? 8u
		                 : (siz == G_IM_SIZ_16b) ? 16u
		                 : (siz == G_IM_SIZ_32b) ? 32u
		                 : 0u;
		int fmt_val = (int)((w0 >> 21) & 0x07);
		const char *note = (found_load == 2) ? "tlut" : "loadblock";
		tex_log_emit("chain.settimg", chain_file_id,
		             target_byte_off, tex_bytes,
		             fmt_val, (int)siz, (int)bpp_val,
		             region, note);

		// Dump the texture as one or more TGA images so we can identify
		// it visually.  Skips palettes (loadtlut) since they're 1-D LUTs
		// not viewable as images.
		if (found_load == 1) {
			tex_dump_chain(chain_file_id, target_byte_off,
			               fmt_val, (int)siz, bpp_val,
			               reinterpret_cast<const uint8_t *>(region),
			               (size_t)tex_bytes);
		}
	}

	if (stage_audit_enabled()) stage_audit().chain_settimg_fixups++;
	return 1;
}

static int chain_fixup_vertex(void *file_base, size_t file_size,
                               unsigned int slot_byte_off,
                               unsigned int target_byte_off,
                               uint32_t w0)
{
	(void)slot_byte_off;
	// G_VTX layout in F3DEX2 (matches gbi_decoder.c decoding):
	//   w0[31:24] = 0x01  (G_VTX opcode)
	//   w0[23:20] = 0     (reserved)
	//   w0[19:12] = num_vtx (n), 8 bits, practically 1..32
	//   w0[11:1]  = (v0 + n) << 1, end vertex slot index in TMEM
	//   w0[0]     = 0     (must be even because of << 1)
	//   w1        = vertex array address
	//
	// Each Vtx is 16 bytes:
	//   word 0: s16 ob[0]  | s16 ob[1]   → rotate16
	//   word 1: s16 ob[2]  | u16 flag    → rotate16
	//   word 2: s16 tc[0]  | s16 tc[1]   → rotate16
	//   word 3: u8  cn[0..3]              → bswap32 (restore byte order)
	//
	// STRICT VALIDATION: opcode 0x01 is too common to trust by itself.
	// Many non-cmd u32 words start with 0x01.  Reject anything where the
	// reserved/structural bits don't match the spec.

	// bits [23:20] must be zero (reserved)
	if ((w0 & 0x00F00000) != 0) return 0;
	// bit [0] must be zero (the count is multiplied by 2)
	if ((w0 & 0x1) != 0) return 0;

	uint32_t num_vtx = (w0 >> 12) & 0xFF;
	uint32_t end_idx = (w0 & 0xFFE) >> 1;

	// Sanity-check num_vtx and the destination range.  Real game DLs use
	// 1..32 vertices per G_VTX (Vtx buffer is 32 entries on N64).
	if (num_vtx == 0 || num_vtx > 32) return 0;
	if (end_idx > 32 || end_idx < num_vtx) return 0;

	size_t total_bytes = (size_t)num_vtx * 16;
	if ((size_t)target_byte_off + total_bytes > file_size) return 0;

	uint32_t *region = (uint32_t *)((uint8_t *)file_base + target_byte_off);

	// Content-based sanity check on the FIRST vertex.
	// Post-pass1 word 0 (high 16 bits) = original BE ob[0] (s16),
	// (low 16 bits) = original BE ob[1] (s16). Word 1 likewise has
	// ob[2] and flag. Real game vertex coords observed up to ±4200,
	// flag is always 0. Reject anything with much larger coords or
	// nonzero flag — those are false-positive struct-field chain
	// entries whose preceding 4 bytes happen to look like a G_VTX cmd.
	{
		int16_t ob0 = (int16_t)((region[0] >> 16) & 0xFFFF);
		int16_t ob1 = (int16_t)(region[0] & 0xFFFF);
		int16_t ob2 = (int16_t)((region[1] >> 16) & 0xFFFF);
		uint16_t flag = (uint16_t)(region[1] & 0xFFFF);
		const int kMaxCoord = 16384;
		if (ob0 < -kMaxCoord || ob0 > kMaxCoord) return 0;
		if (ob1 < -kMaxCoord || ob1 > kMaxCoord) return 0;
		if (ob2 < -kMaxCoord || ob2 > kMaxCoord) return 0;
		if (flag != 0) return 0;
	}

	// Per-vertex idempotency (matches portRelocFixupVertexAtRuntime).
	uintptr_t target_addr = reinterpret_cast<uintptr_t>(file_base) + (uintptr_t)target_byte_off;
	if (stage_audit_enabled()) {
		int tgt_file = portRelocFindFileIdAndBase((const void *)target_addr, nullptr);
		int src_file = portRelocFindFileIdAndBase(file_base, nullptr);
		static uint32_t sChainTrace = 0;
		if (tgt_file == 104 && sChainTrace < 30) {
			sChainTrace++;
			port_log("[STAGE_AUDIT_CHAIN] chain_fixup_vertex src=file%d tgt=file%d target_off=0x%x num_vtx=%u\n",
			         src_file, tgt_file, target_byte_off, num_vtx);
		}
	}
	for (uint32_t i = 0; i < num_vtx; i++)
	{
		uintptr_t vtx_key = target_addr + (uintptr_t)i * 16;
		if (sStructU16Fixups.count(vtx_key)) continue;
		sStructU16Fixups.insert(vtx_key);

		region[i * 4 + 0] = (region[i * 4 + 0] << 16) | (region[i * 4 + 0] >> 16);
		region[i * 4 + 1] = (region[i * 4 + 1] << 16) | (region[i * 4 + 1] >> 16);
		region[i * 4 + 2] = (region[i * 4 + 2] << 16) | (region[i * 4 + 2] >> 16);
		region[i * 4 + 3] = BSWAP32(region[i * 4 + 3]);
	}
	if (stage_audit_enabled()) stage_audit().chain_vtx_fixups++;
	return 1;
}

extern "C" int portRelocFixupTextureFromChain(void *file_base, size_t file_size,
                                              unsigned int slot_byte_off,
                                              unsigned int target_byte_off)
{
	if (file_base == nullptr || slot_byte_off < 4 ||
	    (size_t)(slot_byte_off + 4) > file_size ||
	    (size_t)target_byte_off >= file_size)
	{
		return 0;
	}

	const uint8_t *file_bytes = static_cast<const uint8_t *>(file_base);

	/* The w0 candidate at slot-4 may be a TOKEN this same walk already wrote:
	 * chain entries are frequently adjacent words, so the "command" preceding
	 * this slot is the previous entry's freshly-tokenized slot. A token's top
	 * byte is its generation >> 4 — at gen 16..31 that byte is 0x01 (G_VTX),
	 * at gen 4048..4063 it's 0xFD (SETTIMG) — and the rest of the token also
	 * passes the structural checks (reserved bits zero, plausible num_vtx).
	 * Parsing such a token as a command applies vertex/texture byte
	 * permutations over live chain descriptors: observed 2026-07-31 as a
	 * gen-16 token misparse that rot16'd five descriptors in
	 * reloc_effects/EFCommonEffects2, derailing the walk into a token-feedback
	 * spiral that exhausted the slot table (abort in portRelocRegisterPointer).
	 * A live tokenized slot is never a real command word — skip. Corpse
	 * entries (address reused, word no longer a resolving token) are pruned
	 * so a stale registry can't suppress real fixups. */
	{
		uintptr_t w0_addr = (uintptr_t)(file_bytes + slot_byte_off) - 4;
		auto it = sChainSlotAddrs.find(w0_addr);
		if (it != sChainSlotAddrs.end())
		{
			extern void *portRelocTryResolvePointer(uint32_t token);
			uint32_t w0_word = *(const uint32_t *)w0_addr;
			if (w0_word != 0 && portRelocTryResolvePointer(w0_word) != nullptr)
			{
				static unsigned int sW0TokenSkips = 0;
				if (sW0TokenSkips < 16)
				{
					sW0TokenSkips++;
					port_log("SSB64: chainFixup SKIP w0-is-chain-token base=%p slot_off=0x%x w0=0x%08X\n",
					         file_base, slot_byte_off, w0_word);
				}
				return 0;
			}
			sChainSlotAddrs.erase(it);
		}
	}

	// The cmd's w0 is at slot_byte_off - 4, w1 is at slot_byte_off.
	uint32_t w0 = *(const uint32_t *)(file_bytes + slot_byte_off - 4);
	uint8_t opcode = (uint8_t)(w0 >> 24);

	if (opcode == GBI_SETTIMG)
	{
		return chain_fixup_settimg(file_base, file_size, slot_byte_off,
		                           target_byte_off, w0);
	}
	if (opcode == GBI_VTX)
	{
		// Filter: real packed cmd w1 must be at 8*N+4 alignment.
		if ((slot_byte_off & 0x7) != 4) return 0;
		return chain_fixup_vertex(file_base, file_size, slot_byte_off, target_byte_off, w0);
	}
	return 0;
}
