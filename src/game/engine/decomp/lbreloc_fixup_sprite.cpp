/**
 * lbreloc_fixup_sprite.cpp — Sprite / Bitmap / MObjSub / FTAttributes struct fixups
 *
 * Split out of lbreloc_byteswap.cpp (phase-0 complexity refactor). The
 * shared tracking containers and bridge externs it uses live in
 * lbreloc_fixup_internal.h / lbreloc_fixup_state.cpp.
 */

#include "lbreloc_fixup_internal.h"
#include "lbreloc_debug.h"

#include <algorithm>
#include <cstring>

// ============================================================
//  Sprite / Bitmap / MObjSub — struct-level byte-swap fixups
// ============================================================
//
// After the blanket u32 swap, u32 and f32 fields are correct but
// u16-pair words and u8-quad words are garbled.
//
// rotate16: fixes two u16 values packed in one u32 word
// bswap32:  undoes the blanket swap for a u8-quad word
//
// Each function is idempotent: tracked by the same sStructU16Fixups set.

static void fixup_rotate16(uint32_t *word)
{
	*word = (*word << 16) | (*word >> 16);
}

static void fixup_bswap32(uint32_t *word)
{
	*word = BSWAP32(*word);
}

// Fixup for a u32 word laid out as [u16 a][u8 b][u8 c] in original BE memory.
// Pass1's blanket BSWAP32 leaves the bytes as [c, b, a_lo, a_hi] which makes
// a_lo/a_hi appear in the wrong slots: reading `u16 a` from offset 0 yields
// (b << 8) | c, and reading `u8 b`/`u8 c` from offsets 2/3 yields a_lo/a_hi.
// Neither rotate16 nor a second bswap32 produces the correct LE layout for
// all three fields simultaneously, so we permute the bytes directly.
static void fixup_u16_u8u8(uint32_t *word)
{
	uint8_t *p = reinterpret_cast<uint8_t *>(word);
	uint8_t b0 = p[0];
	uint8_t b1 = p[1];
	uint8_t b2 = p[2];
	uint8_t b3 = p[3];
	// Have (post-pass1): [c, b, a_lo, a_hi] = [b0, b1, b2, b3]
	// Want (LE struct):  [a_lo, a_hi, b, c] = [b2, b3, b1, b0]
	p[0] = b2;
	p[1] = b3;
	p[2] = b1;
	p[3] = b0;
}

extern "C" void portFixupSprite(void *sprite)
{
	if (sprite == NULL)
		return;

	uintptr_t key = reinterpret_cast<uintptr_t>(sprite);
	if (sStructU16Fixups.count(key))
		return;
	sStructU16Fixups.insert(key);

	uint32_t *w = static_cast<uint32_t *>(sprite);

	// Sprite layout (17 words = 68 bytes):
	//  Word  Offset  Fields                   Fixup
	//  0     0x00    s16 x, s16 y             rotate16
	//  1     0x04    s16 width, s16 height    rotate16
	//  2     0x08    f32 scalex               (ok)
	//  3     0x0C    f32 scaley               (ok)
	//  4     0x10    s16 expx, s16 expy       rotate16
	//  5     0x14    u16 attr, s16 zdepth     rotate16
	//  6     0x18    u8 r,g,b,a               bswap32
	//  7     0x1C    s16 startTLUT, s16 nTLUT rotate16
	//  8     0x20    u32 LUT (token)          (ok)
	//  9     0x24    s16 istart, s16 istep    rotate16
	//  10    0x28    s16 nbitmaps, s16 ndisplist rotate16
	//  11    0x2C    s16 bmheight, s16 bmHreal rotate16
	//  12    0x30    u8 bmfmt, u8 bmsiz, pad  bswap32
	//  13    0x34    u32 bitmap (token)        (ok)
	//  14    0x38    u32 rsp_dl (token)        (ok)
	//  15    0x3C    u32 rsp_dl_next (token)   (ok)
	//  16    0x40    s16 frac_s, s16 frac_t   rotate16

	fixup_rotate16(&w[0]);   // x, y
	fixup_rotate16(&w[1]);   // width, height
	// w[2], w[3]: f32 scalex, scaley — ok
	fixup_rotate16(&w[4]);   // expx, expy
	fixup_rotate16(&w[5]);   // attr, zdepth
	fixup_bswap32(&w[6]);    // rgba
	fixup_rotate16(&w[7]);   // startTLUT, nTLUT
	// w[8]: u32 LUT — ok
	fixup_rotate16(&w[9]);   // istart, istep
	fixup_rotate16(&w[10]);  // nbitmaps, ndisplist
	fixup_rotate16(&w[11]);  // bmheight, bmHreal
	fixup_bswap32(&w[12]);   // bmfmt, bmsiz, pad
	// w[13..15]: u32 tokens — ok
	fixup_rotate16(&w[16]);  // frac_s, frac_t
}

extern "C" void portFixupBitmap(void *bitmap)
{
	if (bitmap == NULL)
		return;

	uintptr_t key = reinterpret_cast<uintptr_t>(bitmap);
	if (sStructU16Fixups.count(key))
		return;
	sStructU16Fixups.insert(key);

	uint32_t *w = static_cast<uint32_t *>(bitmap);

	// Bitmap layout (4 words = 16 bytes):
	//  Word  Offset  Fields                        Fixup
	//  0     0x00    s16 width, s16 width_img      rotate16
	//  1     0x04    s16 s, s16 t                  rotate16
	//  2     0x08    u32 buf (token)               (ok)
	//  3     0x0C    s16 actualHeight, s16 LUToffset rotate16

	fixup_rotate16(&w[0]);
	fixup_rotate16(&w[1]);
	// w[2]: u32 buf — ok
	fixup_rotate16(&w[3]);
}

extern "C" void portFixupBitmapArray(void *bitmaps, unsigned int count)
{
	if (bitmaps == NULL || count == 0)
		return;

	// Protect the bitmap array bytes from runtime texture/palette BSWAP
	// over-reads. N64 sprite files sometimes sit a full 512-byte CI8 palette
	// load on top of the bitmap array (the useful palette entries fit before
	// the array; the tail past the array start is effectively garbage). See
	// the wallpaper handling in mvOpeningRun for the concrete case.
	portRegisterProtectedStructRange(bitmaps, (size_t)count * 16);

	uint8_t *ptr = static_cast<uint8_t *>(bitmaps);
	for (unsigned int i = 0; i < count; i++)
	{
		portFixupBitmap(ptr + i * 16);
	}
}

// Sprite layout offsets used by the texel-data fixup. We do not include
// PR/sp.h here (to keep the bridge translation unit lean), so we hard-code
// the field offsets matching the static_assert(sizeof(Sprite) == 68).
//
//   sprite[0x28]  s16 nbitmaps
//   sprite[0x32]  u8  bmsiz                (within the u8 quad word at 0x30)
//   sprite[0x34]  u32 bitmap (token)
//
//   bitmap[0x00]  s16 width        (unused for size — see width_img)
//   bitmap[0x02]  s16 width_img
//   bitmap[0x08]  u32 buf (token)
//   bitmap[0x0C]  s16 actualHeight
//
// Sizes match decomp Sprite/Bitmap structs after portFixupSprite/portFixupBitmap
// have run (i.e. fields readable as native LE).
extern "C" void portFixupSpriteBitmapData(void *sprite_v, void *bitmaps_v)
{
	if (sprite_v == NULL || bitmaps_v == NULL)
		return;

	// Also protect the 68-byte Sprite struct from runtime-texture over-reads
	// for the same reason as the bitmap array above.
	portRegisterProtectedStructRange(sprite_v, 68);

	uint8_t *sprite = static_cast<uint8_t *>(sprite_v);
	uint8_t *bitmaps = static_cast<uint8_t *>(bitmaps_v);

	int16_t nbitmaps = *reinterpret_cast<int16_t *>(sprite + 0x28);
	uint8_t bmfmt    = *reinterpret_cast<uint8_t  *>(sprite + 0x30);
	uint8_t bmsiz    = *reinterpret_cast<uint8_t  *>(sprite + 0x31);

	if (nbitmaps <= 0)
		return;

	/* nbitmaps and every buf/dimension field below come from the file blob
	 * — a shadowed (modded) reloc file can feed us garbage. Bound the
	 * bitmap array against the extent of the file it lives in so the
	 * per-bitmap loop can't read past it. */
	size_t bm_extent = portRelocFileExtentFrom(bitmaps_v);
	if (bm_extent == 0) {
		portRelocTexFixupLog("sprite.bitmap: bitmaps %p outside known reloc files — skip\n",
		                     bitmaps_v);
		return;
	}
	if ((size_t)nbitmaps * 16 > bm_extent) {
		portRelocTexFixupLog("sprite.bitmap: nbitmaps=%d overflows file extent %zu — clamped\n",
		                     (int)nbitmaps, bm_extent);
		nbitmaps = (int16_t)(bm_extent / 16);
	}

	// G_IM_SIZ_ values: 4b=0, 8b=1, 16b=2, 32b=3, 4c=4 (compressed-4b)
	//
	// Fast3D's texel readers (ImportTextureRgba16/Rgba32/CI4/CI8/etc.) all
	// read texture bytes in N64 big-endian order — for example
	//   RGBA16: `(addr[0] << 8) | addr[1]`
	//   RGBA32: `r=addr[0], g=addr[1], b=addr[2], a=addr[3]`
	// So texel data must be left in the original BE byte order from the file.
	// Pass1 BSWAP32 destroys that order; another BSWAP32 restores it.
	//
	// 32bpp used to be skipped here on the assumption "pass1 byteswap is
	// already correct because the reader is per-channel byte-by-byte".  That
	// reasoning is wrong: pass1's per-u32 byte reverse turns each pixel's
	// `[R G B A]` into `[A B G R]` in PC memory, so the per-byte reader sees
	// channel-reversed RGBA32.  Symptom: HAL Labs dog (a 32bpp sprite) renders
	// with R↔A and G↔B swapped — brown becomes a different color entirely.
	// chain_fixup_settimg already had the same wrong assumption removed; this
	// is the matching fix on the sprite path.
	int bpp;
	switch (bmsiz)
	{
	case 0: bpp = 4;  break;   // 4b
	case 1: bpp = 8;  break;   // 8b
	case 2: bpp = 16; break;   // 16b
	case 3: bpp = 32; break;   // 32b
	case 4: bpp = 4;  break;   // 4c (compressed-4b: byte-granular like 4b)
	default:
		return;
	}

	for (int i = 0; i < nbitmaps; i++)
	{
		uint8_t *bm = bitmaps + (i * 16);
		int16_t  bm_width     = *reinterpret_cast<int16_t *>(bm + 0x00);
		int16_t  width_img    = *reinterpret_cast<int16_t *>(bm + 0x02);
		uint32_t buf_token    = *reinterpret_cast<uint32_t *>(bm + 0x08);
		int16_t  actualHeight = *reinterpret_cast<int16_t *>(bm + 0x0C);

		void *buf = portRelocTryResolvePointer(buf_token);
		if (buf == NULL || width_img <= 0 || actualHeight <= 0)
			continue;

		/* The BSWAP32 pass writes align4(w*h*bpp/8) bytes and the deswizzle
		 * touches row_bytes*actualHeight bytes — the latter can exceed the
		 * former when the per-row ceil() pads up. Bound buf by the larger
		 * of the two, or the writes escape the file image. */
		size_t buf_extent = portRelocFileExtentFrom(buf);
		size_t img_bytes  = (static_cast<size_t>(width_img) *
		                     static_cast<size_t>(actualHeight) * bpp + 7) / 8;
		img_bytes = (img_bytes + 3) & ~size_t{3};
		size_t row_bytes0 = ((size_t)width_img * (size_t)bpp + 7) / 8;
		size_t dsw_bytes  = row_bytes0 * (size_t)actualHeight;
		size_t max_touched = img_bytes > dsw_bytes ? img_bytes : dsw_bytes;
		if (buf_extent == 0 || max_touched > buf_extent)
		{
			portRelocTexFixupLog("sprite.bitmap: bm=%d %dx%d (%zuB) exceeds buf extent %zu — skip\n",
			                     i, (int)width_img, (int)actualHeight, max_touched, buf_extent);
			continue;
		}

		// Idempotency for the BSWAP32 pass: skip if pass2 (or a prior
		// sprite fixup call) already restored the byte order.
		uintptr_t key = reinterpret_cast<uintptr_t>(buf);
		bool bswap_already_done = sStructU16Fixups.count(key) != 0;
		if (!bswap_already_done)
		{
			sStructU16Fixups.insert(key);

			size_t num_texels = static_cast<size_t>(width_img) *
			                    static_cast<size_t>(actualHeight);
			size_t tex_bytes = (num_texels * bpp + 7) / 8;
			// Word-align so we can iterate as u32
			tex_bytes = (tex_bytes + 3) & ~size_t{3};

			uint32_t *words = static_cast<uint32_t *>(buf);
			size_t num_words = tex_bytes / 4;

			// Undo pass1 BSWAP32 to restore N64 BE byte order.
			for (size_t j = 0; j < num_words; j++)
				words[j] = BSWAP32(words[j]);
		}

		if (!bswap_already_done && (tex_log_enabled() || tex_dump_enabled())) {
			size_t log_tex_bytes = (static_cast<size_t>(width_img) *
			                        static_cast<size_t>(actualHeight) * bpp + 7) / 8;
			log_tex_bytes = (log_tex_bytes + 3) & ~size_t{3};
			uintptr_t bm_base = 0;
			int sprite_file_id = portRelocFindFileIdAndBase(buf, &bm_base);
			uint32_t off = (sprite_file_id >= 0)
				? (uint32_t)(reinterpret_cast<uintptr_t>(buf) - bm_base)
				: 0u;
			char note[64];
			std::snprintf(note, sizeof(note),
			              "sprite bm=%d w=%d wi=%d h=%d",
			              i, (int)bm_width, (int)width_img, (int)actualHeight);
			tex_log_emit("sprite.bitmap", sprite_file_id,
			             off, (uint32_t)log_tex_bytes,
			             -1, (int)bmsiz, bpp, static_cast<uint32_t *>(buf), note);
			tex_dump_known_dims(sprite_file_id, off,
			                    (int)bmfmt, (int)bmsiz, bpp,
			                    reinterpret_cast<const uint8_t *>(buf),
			                    (size_t)log_tex_bytes,
			                    (uint32_t)width_img,
			                    (uint32_t)actualHeight);
		}

		// N64 RDP TMEM line swizzle: textures stored in DRAM are pre-swizzled
		// to avoid TMEM bank conflicts when sampled. lbCommonDrawSObjBitmap
		// LOAD_BLOCKs the sprite and relies on the RDP's odd-row TMEM
		// address XOR to unscramble at sample time; Fast3D reads DRAM
		// linearly, so we have to un-swizzle in software.
		//
		// The swap granularity is bit-depth dependent:
		//
		//   4b/8b/16b: LoadBlock uses G_IM_SIZ_16b (per the *_LOAD_BLOCK
		//              macros in gbi.h), so the sampler's XOR-0x4 on the
		//              byte address lands as "swap the two 4-byte halves
		//              of each 8-byte qword" on odd rows.
		//
		//   32b:       LoadBlock uses G_IM_SIZ_32b. The LOAD_BLOCK with
		//              dxt=0 interleaves each 4-byte pixel across the
		//              low/high TMEM banks (2 bytes per bank), so each
		//              texel consumes 2 TMEM words instead of one. The
		//              sampler's odd-row XOR still acts at the same TMEM
		//              word granularity, which at the DRAM level means
		//              "swap the two 8-byte halves of each 16-byte group"
		//              — i.e. swap 2 RGBA32 pixels with the next 2.
		//              Empirically verified by offline comparison of the
		//              post-BSWAP32 TGA dumps (tex_dump/sprite_f167_* for
		//              SMASH, _f19_* for fighter-select portraits) against
		//              the original artwork: XOR-4 (1-pixel swap, which
		//              used to be the unconditional behaviour for all
		//              bpp) scrambled 32bpp sprites by swapping adjacent
		//              pixels on every odd row; XOR-16 (2-pixel pair swap)
		//              produces clean images.
		//
		// 4c (compressed) is excluded because the on-disk data is 2bpp
		// compressed source; the decoder runs AFTER this fixup and
		// produces 4bpp data that needs its own post-decode deswizzle
		// (portDeswizzleDecodedSprite4c below).
		//
		// Uses sDeswizzle4cFixups (separate from the BSWAP set) so that
		// textures whose byte-swap was already handled by pass2 still get
		// the deswizzle applied.
		if (bpp > 0 && bmsiz != 4 &&
		    width_img > 0 && actualHeight > 0 &&
		    !sDeswizzle4cFixups.count(key))
		{
			sDeswizzle4cFixups.insert(key);

			size_t group_size = (bpp == 32) ? 16 : 8;
			size_t half       = group_size / 2;

			// row_bytes = DRAM bytes per pixel row.
			size_t row_bytes = ((size_t)width_img * (size_t)bpp + 7) / 8;

			// Require the row to hold at least one full group. The inner
			// loop only processes complete groups (`g + group_size <=
			// row_bytes`), so any trailing bytes past the last full group
			// are left untouched.
			if (row_bytes >= group_size)
			{
				uint8_t *bytes = static_cast<uint8_t *>(buf);
				for (int row = 1; row < actualHeight; row += 2)
				{
					uint8_t *row_p = bytes + (size_t)row * row_bytes;
					for (size_t g = 0; g + group_size <= row_bytes; g += group_size)
					{
						uint8_t tmp[8]; // max half is 8 (for group_size=16)
						std::memcpy(tmp, row_p + g, half);
						std::memcpy(row_p + g, row_p + g + half, half);
						std::memcpy(row_p + g + half, tmp, half);
					}
				}
			}
		}

		// Note: the fighter-intro "MARIO" title-card smear and the
		// title-screen border right-edge slice used to be patched
		// here (ce89700 zeroed, then a later revision replicated the
		// edge pixel).  Both were workarounds for a Fast3D upload bug
		// where the GPU texture was uploaded at the TMEM stride
		// (width_img) but the UV normalization used the SetTileSize
		// clamp (width), stretching the texture slightly.  The real
		// fix is in libultraship — see ImportTexture* clamp to
		// texture_tile.lrs — so we no longer need to touch the
		// trailing cols from the port side.
	}
}

extern "C" void portDeswizzleDecodedSprite4c(void *sprite_v, void *bitmaps_v)
{
	if (sprite_v == NULL || bitmaps_v == NULL)
		return;

	uint8_t *sprite = static_cast<uint8_t *>(sprite_v);
	uint8_t *bitmaps = static_cast<uint8_t *>(bitmaps_v);

	int16_t nbitmaps = *reinterpret_cast<int16_t *>(sprite + 0x28);
	if (nbitmaps <= 0)
		return;

	for (int i = 0; i < nbitmaps; i++)
	{
		uint8_t *bm = bitmaps + (i * 16);
		int16_t  width_img    = *reinterpret_cast<int16_t *>(bm + 0x02);
		uint32_t buf_token    = *reinterpret_cast<uint32_t *>(bm + 0x08);
		int16_t  actualHeight = *reinterpret_cast<int16_t *>(bm + 0x0C);

		void *buf = portRelocTryResolvePointer(buf_token);
		if (buf == NULL || width_img <= 0 || actualHeight <= 0)
			continue;

		uintptr_t key = reinterpret_cast<uintptr_t>(buf);
		if (sDeswizzle4cFixups.count(key))
			continue;
		sDeswizzle4cFixups.insert(key);

		// After decode, data is 4bpp: row_bytes = width_img / 2
		size_t row_bytes = (size_t)width_img / 2;
		if (row_bytes < 8)
			continue;

		uint8_t *bytes = static_cast<uint8_t *>(buf);
		for (int row = 1; row < actualHeight; row += 2)
		{
			uint8_t *row_p = bytes + (size_t)row * row_bytes;
			for (size_t qw = 0; qw + 8 <= row_bytes; qw += 8)
			{
				uint8_t tmp[4];
				std::memcpy(tmp, row_p + qw, 4);
				std::memcpy(row_p + qw, row_p + qw + 4, 4);
				std::memcpy(row_p + qw + 4, tmp, 4);
			}
		}
	}
}

extern "C" void portFixupMObjSub(void *mobjsub)
{
	if (mobjsub == NULL)
		return;

	uintptr_t key = reinterpret_cast<uintptr_t>(mobjsub);
	if (sStructU16Fixups.count(key))
		return;
	sStructU16Fixups.insert(key);

	uint32_t *w = static_cast<uint32_t *>(mobjsub);

	// MObjSub layout (30 words = 0x78 bytes):
	//  Word  Offset  Fields                           Fixup
	//  0     0x00    u16 pad00, u8 fmt, u8 siz        bswap32 (mixed u16+u8)
	//  1     0x04    u32 sprites (token)               (ok)
	//  2     0x08    u16 unk08, u16 unk0A              rotate16
	//  3     0x0C    u16 unk0C, u16 unk0E              rotate16
	//  4     0x10    s32 unk10                          (ok)
	//  5-10  0x14    f32 trau..unk28                    (ok)
	//  11    0x2C    u32 palettes (token)               (ok)
	//  12    0x30    u16 flags, u8 block_fmt, u8 block_siz  bswap32
	//  13    0x34    u16 block_dxt, u16 unk36           rotate16
	//  14    0x38    u16 unk38, u16 unk3A               rotate16
	//  15-18 0x3C    f32 scrollu..unk48                 (ok)
	//  19    0x4C    u32 unk4C                          (ok)
	//  20    0x50    SYColorPack primcolor (u8 rgba)    bswap32
	//  21    0x54    u8 prim_l, u8 prim_m, u8[2] pad   bswap32
	//  22    0x58    SYColorPack envcolor               bswap32
	//  23    0x5C    SYColorPack blendcolor             bswap32
	//  24    0x60    SYColorPack light1color            bswap32
	//  25    0x64    SYColorPack light2color            bswap32
	//  26-29 0x68    s32 unk68..unk74                   (ok)

	fixup_bswap32(&w[0]);    // pad00 + fmt + siz (pad16 unused, u8 fields ok)
	// w[1]: sprites token — ok
	fixup_rotate16(&w[2]);   // unk08, unk0A
	fixup_rotate16(&w[3]);   // unk0C, unk0E
	// w[4..11]: s32/f32/u32 — ok
	fixup_u16_u8u8(&w[12]);  // u16 flags + u8 block_fmt + u8 block_siz
	fixup_rotate16(&w[13]);  // block_dxt, unk36
	fixup_rotate16(&w[14]);  // unk38, unk3A
	// w[15..19]: f32/u32 — ok
	fixup_bswap32(&w[20]);   // primcolor
	fixup_bswap32(&w[21]);   // prim_l, prim_m, pad
	fixup_bswap32(&w[22]);   // envcolor
	fixup_bswap32(&w[23]);   // blendcolor
	fixup_bswap32(&w[24]);   // light1color
	fixup_bswap32(&w[25]);   // light2color
	// w[26..29]: s32 — ok
}

extern "C" void *portFixupFTTexturePartContainer(void *container)
{
	/* FTTexturePartContainer = `FTTexturePart textureparts[2]` where each
	 * entry is `{u8 joint_id; u8 detail[2];}` = 3 bytes; total sizeof = 6.
	 * After pass1 BSWAP32 the u8 fields read as the wrong byte (post-pass1
	 * byte 0 == original byte 3, etc.).
	 *
	 * Earlier attempts at a fix wrote *into* the file image. That broke
	 * for fighters whose `attr->textureparts_container` points at memory
	 * shared with adjacent reloc tokens (the C struct claim of always-6
	 * container bytes does NOT hold for every fighter — some have token-
	 * shaped data at offsets 4-7 of the resolved address). Writing the
	 * "fixed" bytes back over those tokens crashed `ftMainSetStatus`
	 * downstream when `PORT_RESOLVE` was called on the now-mutated token.
	 *
	 * Non-destructive approach: return a static-storage corrected COPY of
	 * the 6 container bytes. The original file image is never modified, so
	 * adjacent token data is untouched and downstream `PORT_RESOLVE` calls
	 * on those tokens still work. Caller dereferences the returned pointer
	 * for `textureparts[0..1]` reads — that data is correct.
	 *
	 * The static copy is single-instance: callers must not interleave
	 * lookups for two different containers without consuming the result of
	 * the first. ftparam.c's three call sites each take a single snapshot
	 * inside one function — safe. */
	if (container == NULL)
		return NULL;

	static uint8_t sFixed[8];   /* 8 bytes for container alignment safety */
	uint8_t *src = static_cast<uint8_t *>(container);
	sFixed[0] = src[3];   /* bswap of word 0 (bytes 0..3) */
	sFixed[1] = src[2];
	sFixed[2] = src[1];
	sFixed[3] = src[0];
	sFixed[4] = src[7];   /* pass1 moved original byte 4 → position 7 */
	sFixed[5] = src[6];   /* pass1 moved original byte 5 → position 6 */
	sFixed[6] = 0;        /* trailing pad (not read by C struct) */
	sFixed[7] = 0;
	return sFixed;
}

extern "C" void portFixupFTAttributes(void *attr)
{
	if (attr == NULL)
		return;

	uintptr_t key = reinterpret_cast<uintptr_t>(attr);
	if (sStructU16Fixups.count(key))
		return;
	sStructU16Fixups.insert(key);

	uint32_t *w = static_cast<uint32_t *>(attr);

	// FTAttributes layout (0x348 bytes = 210 words):
	// Words 0x00..0x2C: f32/s32 physics fields — ok
	// Words 0x27..0x2B: MPObjectColl (4 f32) + Vec2f (2 f32) — ok
	//
	//  Word  Offset  Fields                               Fixup
	//  0x2D  0x0B4   u16 dead_fgm_ids[0], [1]            rotate16
	//  0x2E  0x0B8   u16 deadup_sfx, u16 damage_sfx      rotate16
	//  0x2F  0x0BC   u16 smash_sfx[0], smash_sfx[1]      rotate16
	//  0x30  0x0C0   u16 smash_sfx[2], pad                rotate16
	// Words 0x31..0x38: FTItemPickup (8 f32) — ok
	//  0x39  0x0E4   u16 itemthrow_vel_scale, damage_scale rotate16
	//  0x3A  0x0E8   u16 heavyget_sfx, pad                rotate16
	// Word 0x3B: f32 halo_size — ok
	//  0x3C  0x0F0   SYColorRGBA shade_color[0]           bswap32
	//  0x3D  0x0F4   SYColorRGBA shade_color[1]           bswap32
	//  0x3E  0x0F8   SYColorRGBA shade_color[2]           bswap32
	//  0x3F  0x0FC   SYColorRGBA fog_color                bswap32
	// Word 0x40: is_have_* bitfields — handled by #if IS_BIG_ENDIAN in FTAttributes decl
	// Words 0x41..end: DamageCollDescs (s32/f32), pointers (u32 tokens) — ok

	fixup_rotate16(&w[0x2D]);  // dead_fgm_ids[0], [1]
	fixup_rotate16(&w[0x2E]);  // deadup_sfx, damage_sfx
	fixup_rotate16(&w[0x2F]);  // smash_sfx[0], smash_sfx[1]
	fixup_rotate16(&w[0x30]);  // smash_sfx[2], pad
	fixup_rotate16(&w[0x39]);  // itemthrow_vel_scale, itemthrow_damage_scale
	fixup_rotate16(&w[0x3A]);  // heavyget_sfx, pad
	fixup_bswap32(&w[0x3C]);   // shade_color[0] rgba
	fixup_bswap32(&w[0x3D]);   // shade_color[1] rgba
	fixup_bswap32(&w[0x3E]);   // shade_color[2] rgba
	fixup_bswap32(&w[0x3F]);   // fog_color rgba
}

/* =========================================================================
 * portMarkSyntheticSprite — pre-register a port-built Sprite so that all
 * byteswap/fixup passes become no-ops when the game calls
 * lbCommonMakeSObjForGObj → portFixupSprite / portFixupBitmapArray /
 * portFixupSpriteBitmapData on it.
 *
 * The caller must have already set all struct fields in native LE order and
 * stored pixel buffers in Big-endian RGBA16 linear (no TMEM odd-row swizzle).
 * ========================================================================= */
extern "C" void portMarkSyntheticSprite(void *sprite, void *bitmaps,
                                         unsigned int nbitmaps,
                                         void *const *buf_ptrs)
{
	if (sprite)
		sStructU16Fixups.insert(reinterpret_cast<uintptr_t>(sprite));

	if (bitmaps)
	{
		// Register each Bitmap's base address so portFixupBitmapArray's
		// per-entry portFixupBitmap calls are no-ops.
		// portFixupBitmapArray's portRegisterProtectedStructRange call will
		// still execute — it adds the bitmap array to the BSWAP-protection
		// list, which is harmless for synthetic sprites.
		uint8_t *bm = static_cast<uint8_t *>(bitmaps);
		for (unsigned int i = 0; i < nbitmaps; i++)
			sStructU16Fixups.insert(reinterpret_cast<uintptr_t>(bm + i * 16));
	}

	// Register each pixel buffer in both tracking sets so:
	//   sStructU16Fixups → portFixupSpriteBitmapData skips BSWAP32 pass
	//   sDeswizzle4cFixups → portFixupSpriteBitmapData skips deswizzle pass
	if (buf_ptrs)
	{
		for (unsigned int i = 0; i < nbitmaps; i++)
		{
			if (buf_ptrs[i] == nullptr) continue;
			uintptr_t key = reinterpret_cast<uintptr_t>(buf_ptrs[i]);
			sStructU16Fixups.insert(key);
			sDeswizzle4cFixups.insert(key);
		}
	}
}
