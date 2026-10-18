/**
 * lbreloc_fixup_state.cpp — shared fixup tracking state + struct-level tracking API
 *
 * Split out of lbreloc_byteswap.cpp (phase-0 complexity refactor). The
 * shared tracking containers and bridge externs it uses live in
 * lbreloc_fixup_internal.h / lbreloc_fixup_state.cpp.
 */

#include "lbreloc_fixup_internal.h"

#include <algorithm>

// Tracks which (base + offset) regions have been fixed to prevent
// double-fixup. Used by per-struct fixups (Sprite, Bitmap, MObjSub, etc.),
// pass2 vertex fixups, the chain-walk texture fixup, and the runtime
// lazy vertex fixup. All paths share this set so they don't undo each
// other's work. Cleared at scene change via portResetStructFixups.
std::unordered_set<uintptr_t> sStructU16Fixups;

// Tracks runtime texture bases that own at least one fixed word, plus the
// largest request size seen from that base. This preserves the old pass2/chain
// skip behavior for exact-base matches while sTexFixupWords handles the actual
// per-word idempotency.
std::unordered_map<uintptr_t, unsigned int> sTexFixupExtent;

// Runtime texture/TLUT fixups can overlap even when they start at different
// addresses. CSS gate palettes are adjacent 0x28-byte blocks, but their Sprite
// requests a 512-byte TLUT load from each block. Track each u32 word so the
// second overlapping load skips words fixed by the first instead of BSWAPing
// them back to the wrong byte order.
std::unordered_set<uintptr_t> sTexFixupWords;

// Tracks memory ranges of decoded struct arrays that runtime texture/palette
// BSWAPs must NOT touch. Certain N64 sprite files intentionally overlap the
// tail of a CI8 palette (which Fast3D loads as a fixed 512-byte block even if
// the actual palette is smaller) with the start of the bitmap array — on
// hardware the unused palette entries don't matter, but on the port the
// runtime-path BSWAP would corrupt the bitmap/sprite structs we already
// byte-swap-fixed up.
//
// Entries are (begin, end) pairs. Cleared at scene change via
// portResetStructFixups.
std::vector<ProtectedRange> sProtectedStructRanges;

// Separate tracking for post-decode 4c sprite deswizzle (portDeswizzleDecodedSprite4c).
// Needs its own set because portFixupSpriteBitmapData already inserted the same buf
// addresses during the pre-decode BSWAP32 pass.
std::unordered_set<uintptr_t> sDeswizzle4cFixups;

/* Absolute addresses of every tokenized reloc chain slot currently live.
 * Texel data never contains chain slots, so a runtime texture fixup whose
 * range covers one is aimed at non-texture data (a stale or garbage
 * SETTIMG operand) and must be refused — applying it BSWAP32s live
 * descriptor and token words in place. Observed cascade: a DObjDesc array
 * in a fighter main file wholesale byte-reversed mid-scene, its sentinel
 * unrecognizable, item spawn walks running off the array into float data
 * → textureless objects, "impossible" token warnings (byte-swapped
 * tokens), and SIGSEGV in gcSetupCustomDObjsWithMObj. */
std::unordered_set<uintptr_t> sChainSlotAddrs;

extern "C" void portRelocNoteChainSlot(const void *slot)
{
	sChainSlotAddrs.insert(reinterpret_cast<uintptr_t>(slot));
}

void portRegisterProtectedStructRange(const void *begin, size_t size)
{
	if (begin == nullptr || size == 0) return;
	uintptr_t b = reinterpret_cast<uintptr_t>(begin);
	sProtectedStructRanges.push_back({ b, b + size });
}

// ============================================================
//  Struct u16 fixup — rotate16 for u16 fields in ROM structs
// ============================================================

extern "C" void portFixupStructU16(void *base, unsigned int byte_offset, unsigned int num_words)
{
	uintptr_t key = reinterpret_cast<uintptr_t>(base) + byte_offset;
	if (sStructU16Fixups.count(key))
		return;
	sStructU16Fixups.insert(key);

	uint32_t *words = reinterpret_cast<uint32_t *>(
		static_cast<uint8_t *>(base) + byte_offset);
	for (unsigned int i = 0; i < num_words; i++)
	{
		words[i] = (words[i] << 16) | (words[i] >> 16);
	}
}

// Undo the blanket pass1 u32 byteswap for a single-u8-inside-a-u32 field.
// After pass1, a BE ROM u8 at struct offset N lives at native offset (N|3);
// reversing the containing u32 word puts the u8 back at offset N where the
// C struct definition expects to read it.  Idempotent via sStructU16Fixups.
extern "C" void portFixupStructU32(void *base, unsigned int byte_offset, unsigned int num_words)
{
	uintptr_t key = reinterpret_cast<uintptr_t>(base) + byte_offset;
	if (sStructU16Fixups.count(key))
		return;
	sStructU16Fixups.insert(key);

	uint32_t *words = reinterpret_cast<uint32_t *>(
		static_cast<uint8_t *>(base) + byte_offset);
	for (unsigned int i = 0; i < num_words; i++)
	{
		words[i] = BSWAP32(words[i]);
	}
}

extern "C" void portResetStructFixups(void)
{
	sStructU16Fixups.clear();
	sTexFixupExtent.clear();
	sTexFixupWords.clear();
	sProtectedStructRanges.clear();
	sDeswizzle4cFixups.clear();
	sChainSlotAddrs.clear();
}

extern "C" void portEvictStructFixupsInRange(const void *begin, size_t size)
{
	if (begin == nullptr || size == 0)
		return;

	uintptr_t lo = reinterpret_cast<uintptr_t>(begin);
	uintptr_t hi = lo + size;

	auto evict_set = [&](std::unordered_set<uintptr_t> &s) {
		for (auto it = s.begin(); it != s.end(); ) {
			if (*it >= lo && *it < hi) it = s.erase(it);
			else ++it;
		}
	};
	evict_set(sStructU16Fixups);
	evict_set(sTexFixupWords);
	evict_set(sDeswizzle4cFixups);
	evict_set(sChainSlotAddrs);

	for (auto it = sTexFixupExtent.begin(); it != sTexFixupExtent.end(); ) {
		if (it->first >= lo && it->first < hi) it = sTexFixupExtent.erase(it);
		else ++it;
	}

	sProtectedStructRanges.erase(
		std::remove_if(sProtectedStructRanges.begin(), sProtectedStructRanges.end(),
		               [&](const ProtectedRange &r) {
		                   return r.begin < hi && r.end > lo;
		               }),
		sProtectedStructRanges.end());
}

// For raw texel blobs loaded via a runtime-built SETTIMG (where pass2's
// DL scan never saw the SETTIMG inside a stored display list) — apply
// BSWAP32 again to restore N64 BE byte order that pass1 destroyed.
// Tracked by base so repeat calls on the same blob are no-ops.
extern "C" void portFixupRawTextureBSWAP32(void *base, size_t bytes)
{
	if (base == nullptr || bytes == 0)
		return;

	uintptr_t key = reinterpret_cast<uintptr_t>(base);
	if (sStructU16Fixups.count(key))
		return;
	sStructU16Fixups.insert(key);

	// Round down to full u32s — pass1 only touched 4-byte-aligned words,
	// so any trailing tail bytes were never swapped and shouldn't be now.
	size_t num_words = bytes / 4;
	uint32_t *words = static_cast<uint32_t *>(base);
	for (size_t i = 0; i < num_words; i++)
	{
		words[i] = BSWAP32(words[i]);
	}
}
