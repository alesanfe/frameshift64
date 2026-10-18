/**
 * lbreloc_byteswap.cpp — Big-endian to native byte-swap for reloc file blobs
 *
 * N64 reloc files are stored in big-endian byte order. On little-endian PC,
 * every multi-byte field is read incorrectly. This module performs a two-pass
 * byte-swap:
 *
 *   Pass 1: Blanket u32 swap of the entire blob. This correctly handles
 *           display list commands, float/int struct fields, reloc chain
 *           descriptors, 32bpp texture pixels, and zeros.
 *
 *   Pass 2: Parse the now-native-endian display list commands to identify
 *           vertex buffers and texture/palette regions, then apply targeted
 *           fixups for data types that u32 swap got wrong.
 *
 * Called from lbRelocLoadAndRelocFile() between memcpy and the reloc chain walk.
 *
 * The fixup paths that share this machinery live in sibling TUs:
 *   lbreloc_fixup_state.cpp   — idempotency/protection tracking containers
 *   lbreloc_fixup_chain.cpp   — G_SETTIMG / G_VTX fixups during the chain walk
 *   lbreloc_fixup_runtime.cpp — lazy fixups at interpreter execute time
 *   lbreloc_fixup_sprite.cpp  — Sprite / Bitmap / MObjSub / FT struct fixups
 */

#include "lbreloc_byteswap.h"
#include "lbreloc_fixup_internal.h"
#include "lbreloc_debug.h"

#include <algorithm>
#include <cstdint>
#include <vector>

// Segment ID used for intra-file display list references
#define FILE_SEGMENT_ID 0x0E

// Fixup types for marked regions
enum RegionFixup
{
	FIXUP_VERTEX,       // Per-Vtx: rotate16 words 0-2, bswap32 word 3
	FIXUP_TEX_BYTES,    // 4bpp/8bpp texture: undo u32 swap (bswap32 back)
	FIXUP_TEX_U16,      // 16bpp texture or palette: rotate16 each word
	// 32bpp: no fixup needed (u32 swap was correct)
};

struct FixupRegion
{
	uint32_t byte_offset;
	uint32_t byte_size;
	RegionFixup type;
};

// ============================================================
//  Pass 1: Blanket u32 swap
// ============================================================

static void pass1_swap_u32(void *data, size_t size)
{
	uint32_t *words = static_cast<uint32_t *>(data);
	size_t count = size / 4;

	for (size_t i = 0; i < count; i++)
	{
		words[i] = BSWAP32(words[i]);
	}
}

// ============================================================
//  Pass 2: DL-guided fixup scan
// ============================================================

static void scan_display_lists(const uint32_t *words, size_t num_words,
                               size_t file_size, std::vector<FixupRegion> &regions)
{
	// Pending texture image state from the most recent gDPSetTextureImage
	bool has_pending_tex = false;
	uint32_t pending_tex_offset = 0;
	uint32_t pending_tex_siz = 0;

	for (size_t i = 0; i + 1 < num_words; i += 2)
	{
		uint32_t w0 = words[i];
		uint32_t w1 = words[i + 1];
		uint8_t opcode = (w0 >> 24) & 0xFF;

		switch (opcode)
		{
		case GBI_VTX:
		{
			uint32_t num_vtx = (w0 >> 12) & 0xFF;
			uint8_t seg = (w1 >> 24) & 0xFF;
			uint32_t offset = w1 & 0x00FFFFFF;
			size_t offset_sz = static_cast<size_t>(offset);
			size_t vtx_bytes = static_cast<size_t>(num_vtx) * 16;

			if (seg == FILE_SEGMENT_ID && num_vtx > 0 &&
			    offset_sz <= file_size && vtx_bytes <= (file_size - offset_sz))
			{
				regions.push_back({offset, num_vtx * 16, FIXUP_VERTEX});
				if (stage_audit_enabled()) stage_audit().vtx_scan_caught++;
			}
			else if (stage_audit_enabled() && num_vtx > 0)
			{
				stage_audit().vtx_scan_skipped_seg++;
			}
			break;
		}

		case GBI_SETTIMG:
		{
			uint8_t seg = (w1 >> 24) & 0xFF;
			uint32_t siz = (w0 >> 19) & 0x03;

			if (seg == FILE_SEGMENT_ID)
			{
				pending_tex_offset = w1 & 0x00FFFFFF;
				pending_tex_siz = siz;
				has_pending_tex = true;
				if (stage_audit_enabled()) stage_audit().settimg_scan_caught++;
			}
			else
			{
				has_pending_tex = false;
				if (stage_audit_enabled()) stage_audit().settimg_scan_skipped_seg++;
			}
			break;
		}

		case GBI_LOADBLOCK:
		{
			if (!has_pending_tex)
				break;

			uint32_t lrs = (w1 >> 12) & 0xFFF;
			uint32_t num_texels = lrs + 1;
			uint32_t bpp = 0;

			switch (pending_tex_siz)
			{
			case G_IM_SIZ_4b:  bpp = 4;  break;
			case G_IM_SIZ_8b:  bpp = 8;  break;
			case G_IM_SIZ_16b: bpp = 16; break;
			case G_IM_SIZ_32b: bpp = 32; break;
			}

			if (bpp == 0)
				break;

			uint32_t tex_bytes = (num_texels * bpp + 7) / 8;
			// Align to 4 bytes for u32 word processing
			tex_bytes = (tex_bytes + 3) & ~3u;

			size_t pending_offset_sz = static_cast<size_t>(pending_tex_offset);
			if (pending_offset_sz > file_size)
			{
				has_pending_tex = false;
				break;
			}
			if (static_cast<size_t>(tex_bytes) > (file_size - pending_offset_sz))
				tex_bytes = static_cast<uint32_t>(file_size - pending_offset_sz);

			RegionFixup fixup;
			switch (pending_tex_siz)
			{
			case G_IM_SIZ_4b:
			case G_IM_SIZ_8b:
				fixup = FIXUP_TEX_BYTES;
				break;
			case G_IM_SIZ_16b:
				fixup = FIXUP_TEX_U16;
				break;
			default:
				// 32b: u32 swap was correct, no fixup
				goto skip_tex;
			}

			regions.push_back({pending_tex_offset, tex_bytes, fixup});
		skip_tex:
			has_pending_tex = false;
			break;
		}

		case GBI_LOADTLUT:
		{
			if (!has_pending_tex)
				break;

			uint32_t count = ((w1 >> 14) & 0x3FF) + 1;
			uint32_t palette_bytes = count * 2;
			// Align to 4 bytes
			palette_bytes = (palette_bytes + 3) & ~3u;

			size_t pending_offset_sz = static_cast<size_t>(pending_tex_offset);
			if (pending_offset_sz > file_size)
			{
				has_pending_tex = false;
				break;
			}
			if (static_cast<size_t>(palette_bytes) > (file_size - pending_offset_sz))
				palette_bytes = static_cast<uint32_t>(file_size - pending_offset_sz);

			regions.push_back({pending_tex_offset, palette_bytes, FIXUP_TEX_U16});
			has_pending_tex = false;
			break;
		}

		default:
			break;
		}
	}
}

// ============================================================
//  Fixup application
// ============================================================

// Rotate each u32 word by 16 bits: converts u32-swapped u16 pairs
// to correctly u16-swapped data.
// After u32 swap, BE bytes [A B C D] became [D C B A].
// rotate16 produces [B A D C] which is correct u16 LE for (AB, CD).
static inline uint32_t rotate16(uint32_t w)
{
	return (w << 16) | (w >> 16);
}

static void apply_fixup_vertex(uint32_t *words, size_t num_words)
{
	// Vtx is 4 u32 words (16 bytes):
	//   Word 0: s16 ob[0] | s16 ob[1]   -> rotate16
	//   Word 1: s16 ob[2] | u16 flag     -> rotate16
	//   Word 2: s16 tc[0] | s16 tc[1]    -> rotate16
	//   Word 3: u8 cn[0-3]               -> bswap32 (restore byte order)
	for (size_t i = 0; i + 3 < num_words; i += 4)
	{
		words[i + 0] = rotate16(words[i + 0]);
		words[i + 1] = rotate16(words[i + 1]);
		words[i + 2] = rotate16(words[i + 2]);
		words[i + 3] = BSWAP32(words[i + 3]);
	}
}

static void apply_fixup_tex_bytes(uint32_t *words, size_t num_words)
{
	// 4bpp/8bpp texture data: byte-granular pixels.
	// u32 swap reversed byte order within each 4-byte group.
	// Undo by swapping back to original byte order.
	for (size_t i = 0; i < num_words; i++)
	{
		words[i] = BSWAP32(words[i]);
	}
}

static void apply_fixup_tex_u16(uint32_t *words, size_t num_words)
{
	// 16bpp texture/palette data: u16 pixel values stored in N64 BE byte order.
	// Pass1 BSWAP32 reversed the byte order within each u32; Fast3D's
	// ImportTextureRgba16 reads texels as `(addr[0] << 8) | addr[1]` (BE u16),
	// so we must restore the original BE byte layout.  BSWAP32 undoes pass1.
	//
	// Earlier this used rotate16 which only swapped the two 16-bit halves —
	// that's correct for `[s16][s16]` vertex pair fields but wrong for a stream
	// of BE-read u16 texels.  Most non-sprite RGBA16 textures (room walls, etc.)
	// surfaced as all-near-black pixels until this was switched to BSWAP32.
	for (size_t i = 0; i < num_words; i++)
	{
		words[i] = BSWAP32(words[i]);
	}
}

static void apply_fixups(void *data, size_t file_size,
                         const std::vector<FixupRegion> &regions,
                         unsigned int file_id)
{
	uint8_t *bytes = static_cast<uint8_t *>(data);

	for (const auto &region : regions)
	{
		size_t start = static_cast<size_t>(region.byte_offset);
		size_t len = static_cast<size_t>(region.byte_size);

		// Bounds check using size_t math to prevent 32-bit overflow.
		if (start > file_size || len > (file_size - start))
			continue;

		// We reinterpret as u32 words, so both start and length must be word-aligned.
		if ((start & 3) != 0)
			continue;
		len &= ~static_cast<size_t>(3);
		if (len == 0)
			continue;

		uint32_t *region_words = reinterpret_cast<uint32_t *>(
			bytes + start);
		size_t num_words = len / 4;

		switch (region.type)
		{
		case FIXUP_VERTEX:
			apply_fixup_vertex(region_words, num_words);
			// Per-vertex registration so the runtime lazy fixup
			// (portRelocFixupVertexAtRuntime) skips each Vtx
			// individually — handles overlapping sub-loads.
			{
				uintptr_t base = reinterpret_cast<uintptr_t>(region_words);
				size_t n_vtx = num_words / 4;  // 4 u32 words per Vtx
				for (size_t v = 0; v < n_vtx; v++) {
					sStructU16Fixups.insert(base + v * 16);
				}
			}
			break;
		case FIXUP_TEX_BYTES:
			apply_fixup_tex_bytes(region_words, num_words);
			tex_log_emit("pass2.bytes", (int)file_id,
			             (uint32_t)start, (uint32_t)len,
			             -1, -1, -1, region_words, "4b/8b");
			break;
		case FIXUP_TEX_U16:
			apply_fixup_tex_u16(region_words, num_words);
			tex_log_emit("pass2.u16", (int)file_id,
			             (uint32_t)start, (uint32_t)len,
			             -1, -1, 16, region_words, "16b/tlut");
			break;
		}
	}
}

// ============================================================
//  Public API
// ============================================================

extern "C" void portRelocByteSwapBlob(void *data, size_t size, unsigned int file_id)
{
	if (data == nullptr || size < 4)
		return;

	if (stage_audit_enabled()) stage_audit_reset_per_file();

	if (stage_audit_enabled() && file_id == 104) {
		const uint8_t *p = static_cast<const uint8_t *>(data);
		port_log("[STAGE_AUDIT_104_LOAD] pre_pass1 bytes=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
		         p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
		         p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
	}

	// Pass 1: blanket u32 swap
	pass1_swap_u32(data, size);

	if (stage_audit_enabled() && file_id == 104) {
		const uint8_t *p = static_cast<const uint8_t *>(data);
		port_log("[STAGE_AUDIT_104_LOAD] post_pass1 bytes=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
		         p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
		         p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
	}

	// Pass 2: DL-guided fixup
	size_t num_words = size / 4;
	const uint32_t *words = static_cast<const uint32_t *>(data);

	std::vector<FixupRegion> regions;
	scan_display_lists(words, num_words, size, regions);

	if (!regions.empty())
	{
		apply_fixups(data, size, regions, file_id);
	}
}
