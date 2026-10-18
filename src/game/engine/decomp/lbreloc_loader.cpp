/** lbreloc_loader.cpp — the token-based relocate pipeline: byte-swap,
 * chain walks, intern/extern slot tokenization, mod-private loads.
 * Split from lbreloc_bridge.cpp; the pipeline overview lives there. */
#include <ship/Context.h>
#include <ship/resource/ResourceManager.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>  // _exit
#endif

#include "RelocFile.h"
#include "RelocFileTable.h"
#include "RelocPointerTable.h"
#include "SyntheticReloc.h"
#include "ssb64_reloc_rebuild.h"
#include "lbreloc_byteswap.h"
#include "lbreloc_bridge_internal.h"

extern "C" {

// // // // // // // // // // // //
//                               //
//   BRIDGE: LOAD & RELOCATE     //
//                               //
// // // // // // // // // // // //

/**
 * Copy `src_bytes` into `ram_dst` and perform the full reloc-fixup
 * pipeline against it: cache evictions, byte-swap, status-buffer
 * registration, internal/external pointer chain walks (token-based),
 * figatree-specific cleanup, audit emission.
 *
 * Exposed for mod-loader use: a mod that ships its own reloc-format
 * file bytes (e.g., new-character assets bundled inside a TCC mod)
 * calls this directly instead of routing through portLoadRelocResource,
 * which only sees files inside BattleShip.o2r. Mod file_ids may sit
 * outside the vanilla 0..RELOC_FILE_COUNT-1 range; we gate operations
 * that need a valid gRelocFileTable[] entry on the usual bounds check.
 *
 * Token-based relocation: instead of writing 64-bit void* into 4-byte
 * slots (which would corrupt adjacent data on LP64), each target
 * pointer is registered in the token table and the 32-bit token is
 * written into the 4-byte slot.
 *
 * PortRelocExplicitRelocation (deblobbed flat-lists input) lives in
 * lbreloc_bridge_internal.h.
 */

/* One intern relocation slot, shared by the chain walk, the explicit flat
 * loop, and the mod-private path so all three tokenize identically:
 * optional SETTIMG/G_VTX texture fixup, target pointer -> token, optional
 * figatree slot marking, token stamped into the slot, optional chain-slot
 * note for the generational eviction bookkeeping. */
static inline void portRelocApplyInternSlot(
	void *ram_dst, size_t copySize,
	uint32_t slot_byte_off, uint32_t target_byte_off,
	bool texture_fixup, bool note_chain,
	std::vector<uint8_t> *figatree_words)
{
	if (texture_fixup)
	{
		portRelocFixupTextureFromChain(ram_dst, copySize,
		                               slot_byte_off, target_byte_off);
	}

	void *target = (void *)((uintptr_t)ram_dst + target_byte_off);
	u32 token = portRelocRegisterPointer(target);

	uint32_t slot_word = slot_byte_off / (uint32_t)sizeof(u32);
	if (figatree_words != nullptr && slot_word < figatree_words->size())
	{
		(*figatree_words)[slot_word] = 1;
	}

	u32 *slot = (u32 *)((uintptr_t)ram_dst + slot_byte_off);
	*slot = token;
	if (note_chain)
	{
		portRelocNoteChainSlot(slot);
	}
}

static void portRelocLoadFileFromBytesImpl(
	unsigned int    file_id,
	void           *ram_dst,
	unsigned int    bytes_num,
	int             loc,
	const void     *src_bytes,
	unsigned int    src_size,
	unsigned short  reloc_intern_offset,
	unsigned short  reloc_extern_offset,
	const unsigned short *extern_file_ids,
	unsigned int    extern_count,
	int             force_figatree_fixup,
	const PortRelocExplicitRelocation *explicit_reloc)
{
	/* portRelocIsFighterFigatreeFile looks up gRelocFileTable[file_id]
	 * to match the "reloc_animations/" path prefix. Mod-registered file
	 * IDs sit outside the vanilla table range so the path lookup fails
	 * and the halfswap step is skipped, leaving anim-joint tokens in
	 * chain-entry form. The mod can override via force_figatree_fixup
	 * for its own animation files. */
	bool is_fighter_figatree =
		force_figatree_fixup || portRelocIsFighterFigatreeFile(file_id);
	std::vector<uint8_t> figatree_reloc_words;
	const char *table_path = (file_id < RELOC_FILE_COUNT) ? gRelocFileTable[file_id] : nullptr;

	// Gated: SSB64_LOG_LBRELOC_LOAD=1 logs every file load. Helpful when
	// tracing which reloc files are loaded per scene.
	if (getenv("SSB64_LOG_LBRELOC_LOAD") != nullptr) {
		static int s_load_log_count = 0;
		if (s_load_log_count < 512) {
			s_load_log_count++;
			port_log("SSB64: lbReloc LOAD file_id=%u loc=%d path=%s fig=%d\n",
				file_id, loc,
				table_path ? table_path : "(null)",
				(int)is_fighter_figatree);
		}
	}

	if (src_bytes == nullptr || src_size == 0)
	{
		spdlog::error("portRelocLoadFileFromBytes: NULL/empty src for file_id {}", file_id);
		return;
	}

	// Copy decompressed data into the game's heap allocation
	size_t copySize = src_size;
	if (copySize > bytes_num && bytes_num > 0)
	{
		spdlog::warn("lbReloc bridge: file_id {} data ({} bytes) exceeds "
		             "buffer ({} bytes), truncating", file_id, copySize, bytes_num);
		copySize = bytes_num;
	}
	if (is_fighter_figatree)
	{
		figatree_reloc_words.resize(copySize / sizeof(u32), 0);
	}
	// Invalidate fixup idempotency state that keyed on addresses inside the
	// region we're about to overwrite. Needed because bump-reset heaps (e.g.
	// the stage-select wallpaper heaps rewound by lbRelocGetForceExternHeapFile
	// on every cursor tick) reuse the same addresses across stages; without
	// this, portFixupSprite/Bitmap/SpriteBitmapData wrongly skip the new load
	// and the BSWAP texel loop later walks past the texture on bogus sizes.
	portEvictStructFixupsInRange(ram_dst, copySize);
	// Evict any libultraship texture-cache entries whose origAddr falls in
	// the heap range we're about to overwrite. The Fast3D cache key is
	// {addr, fmt, siz, sizeBytes, masks, maskt, w, h} — same-shape textures
	// (e.g. the 300x6 RGBA16 wallpaper tile rows used by every stage) at a
	// reused heap address would otherwise hit a stale cached upload from
	// the prior file. Symptom: scene 45 (DK+Samus Kongo Jungle) renders
	// the prior scene's wallpaper. See docs/dk_intro_wallpaper_*.md
	extern void portTextureCacheDeleteRange(const void *base, size_t size);
	portTextureCacheDeleteRange(ram_dst, copySize);
	// Evict cached packed-DL widenings whose source pointer falls in the
	// range we're about to overwrite. Without this, the widening cache
	// hands back a vector with stale fileBase/fileSize, segment-0E sub-DL
	// references resolve to the prior file's address window, and the
	// interpreter walks garbage — fingerprint of issue #103/#128.
	extern void portPackedDisplayListCacheDeleteRange(const void *base, size_t size);
	portPackedDisplayListCacheDeleteRange(ram_dst, copySize);
	portRelocEvictFileRangesInRange(ram_dst, copySize);
	memcpy(ram_dst, src_bytes, copySize);
	syNetRollbackNoteFileLoad(ram_dst, copySize, is_fighter_figatree ? 1 : 0);

	/* lbRelocGetExternHeapFile sets sLBRelocExternFileHeap = heap so that
	 * subsequent dep loads (triggered by the chain walk via
	 * lbRelocGetExternBufferFile) bump-allocate sequentially past this
	 * file's buffer. Mod-loaded files bypass that entry point entirely,
	 * leaving the bump pointer at whatever the last vanilla load left it
	 * at -- often pointing *into* the buffer we just memcpy'd. When a
	 * cache-miss dep load fires inside the chain walk it then writes its
	 * bytes through this file's region, corrupting slots before the walk
	 * has a chance to tokenize them. Mirror the vanilla setup by
	 * advancing the bump pointer past this file's end so dep allocations
	 * land in clean memory. Done for Extern and Force; Default uses the
	 * intern-buffer bump heap which mod paths never enter. */
	if (loc == nLBFileLocationExtern || loc == nLBFileLocationForce) {
		sLBRelocExternFileHeap = (void *)LBRELOC_CACHE_ALIGN(
			(uintptr_t)ram_dst + copySize);
	}

	// One-shot raw dump for verification against ROM extraction.
	// Set SSB64_DUMP_FILE_ID env var to a file_id; this writes the post-memcpy,
	// pre-byteswap bytes to debug_traces/port_file_<id>.bin.  Compare against
	// tools/debugging/reloc_extract/reloc_extract.py output for the same id.
	//
	// If SSB64_DUMP_ALL_FIGATREE=1, dump every fighter figatree file loaded
	// (pre-byteswap) for bulk comparison.
	{
		const char *dump_id_env = getenv("SSB64_DUMP_FILE_ID");
		const char *dump_all_env = getenv("SSB64_DUMP_ALL_FIGATREE");
		bool do_dump = false;
		if (dump_id_env != nullptr) {
			unsigned long target_id = strtoul(dump_id_env, nullptr, 0);
			if ((unsigned long)file_id == target_id) do_dump = true;
		}
		if (dump_all_env != nullptr && dump_all_env[0] == '1' && is_fighter_figatree) {
			do_dump = true;
		}
		if (do_dump) {
			char path[256];
			snprintf(path, sizeof(path), "debug_traces/port_file_%u.bin", file_id);
			FILE *df = fopen(path, "wb");
			if (df != nullptr) {
				fwrite(ram_dst, 1, copySize, df);
				fclose(df);
				spdlog::info("[port-dump] wrote {} bytes for file_id={} ({}) to {}",
				             copySize, file_id, table_path ? table_path : "(mod)", path);
			}
		}
	}

	// Byte-swap from N64 big-endian to native little-endian.
	// Must happen BEFORE the reloc chain walk (which reads u16 fields
	// from u32 words using bit shifts that assume native byte order).
	portRelocByteSwapBlob(ram_dst, copySize, (unsigned int)file_id);

	// Register in status buffer
	if (loc == nLBFileLocationForce)
	{
		lbRelocAddForceStatusBufferFile(file_id, ram_dst);
	}
	else
	{
		lbRelocAddStatusBufferFile(file_id, ram_dst);
	}

	sPortRelocFileRanges.push_back({ reinterpret_cast<uintptr_t>(ram_dst), copySize, file_id, table_path });

	/* Mirror this range into the DL-range registry so gfx_step's bounds
	 * check accepts DLs resolved through reloc files. Path string from
	 * gRelocFileTable is static (linker-emitted), safe to retain. */
	port_dl_range_register(ram_dst, copySize, table_path ? table_path : "reloc?");

	// --- Internal pointer relocation (token-based) ---
	//
	// Each reloc descriptor is a 4-byte word in the data:
	//   bits [31:16] = next descriptor offset (in words), 0xFFFF = end
	//   bits [15:0]  = target offset within this file (in words)
	//
	// On N64, the code overwrites this 4-byte word with a void* pointer.
	// In the port, we compute the pointer, register it as a token, and
	// write the 32-bit token into the 4-byte word.

	if (explicit_reloc != nullptr)
	{
		// --- Explicit relocation (synthesized/deblobbed bundles) ---
		//
		// The chain descriptor words were replaced with real content at
		// extraction, so the in-data chains are unwalkable; apply the
		// spec's flat, chain-ordered slot lists instead (docs/deblob.md).
		// Offsets arrive in the bundle's CURRENT layout, so this path is
		// also what makes relayouted (resized-mod) bundles relocatable.
		for (size_t i = 0; i < explicit_reloc->intern_count; i++)
		{
			const RelocExplicitInternSlot &s = explicit_reloc->intern_slots[i];
			if ((size_t)s.SlotByteOff + sizeof(u32) > copySize ||
			    (size_t)s.TargetByteOff >= copySize)
			{
				spdlog::error("[deblob] I9 violated: explicit intern slot {} OOB "
				              "(file_id {}, slot=0x{:X} target=0x{:X} size=0x{:X})"
				              " — diagnose: SSB64_SYNTH_INSPECT={}",
				              i, file_id, s.SlotByteOff, s.TargetByteOff, copySize, file_id);
				break;
			}
			portRelocApplyInternSlot(ram_dst, copySize,
			                         s.SlotByteOff, s.TargetByteOff,
			                         /* texture_fixup */ true, /* note_chain */ true,
			                         is_fighter_figatree ? &figatree_reloc_words : nullptr);
		}

		for (size_t i = 0; i < explicit_reloc->extern_count; i++)
		{
			const RelocExplicitExternSlot &s = explicit_reloc->extern_slots[i];
			if ((size_t)s.SlotByteOff + sizeof(u32) > copySize)
			{
				spdlog::error("[deblob] I9 violated: explicit extern slot {} OOB "
				              "(file_id {}, slot=0x{:X} size=0x{:X})"
				              " — diagnose: SSB64_SYNTH_INSPECT={}",
				              i, file_id, s.SlotByteOff, copySize, file_id);
				break;
			}

			// Same dependency resolution as the chain walk below, but the
			// dep file id comes from the slot itself instead of the
			// positional extern id list.
			void *vaddr_extern;
			if (loc == nLBFileLocationForce)
			{
				vaddr_extern = lbRelocFindForceStatusBufferFile(s.DepFileId);
			}
			else
			{
				vaddr_extern = lbRelocFindStatusBufferFile(s.DepFileId);
			}
			if (vaddr_extern == NULL)
			{
				switch (loc)
				{
				case nLBFileLocationExtern:
					vaddr_extern = lbRelocGetExternBufferFile(s.DepFileId);
					break;
				case nLBFileLocationDefault:
					vaddr_extern = lbRelocGetInternBufferFile(s.DepFileId);
					break;
				case nLBFileLocationForce:
					vaddr_extern = lbRelocGetForceExternBufferFile(s.DepFileId);
					break;
				}
			}

			void *target = (void *)((uintptr_t)vaddr_extern +
			                        ((size_t)s.DepWordOff * sizeof(u32)));
			u32 token = portRelocRegisterPointer(target);

			uint32_t slot_word = s.SlotByteOff / (uint32_t)sizeof(u32);
			if (is_fighter_figatree && slot_word < figatree_reloc_words.size())
			{
				figatree_reloc_words[slot_word] = 1;
			}
			u32 *slot = (u32 *)((uintptr_t)ram_dst + s.SlotByteOff);
			*slot = token;
			portRelocNoteChainSlot(slot);
		}
	}
	else
	{

	u16 reloc_intern = reloc_intern_offset;
	u32 intern_steps = 0;

	while (reloc_intern != 0xFFFF)
	{
		/* Containment: a pristine chain only references word offsets inside
		 * its own file and visits each slot once, so a legit walk can never
		 * take more steps than the file has words nor step outside it. A
		 * corrupted chain word (observed 2026-07-31: gen-16 token misparsed
		 * as G_VTX rot16'd live descriptors) used to send this walk on an
		 * unbounded wander that exhausted the token table — abort() in
		 * portRelocRegisterPointer. Log + stop instead: the file keeps every
		 * fixup applied up to the bad word, and the scene keeps running. */
		if ((size_t)reloc_intern * sizeof(u32) + sizeof(u32) > copySize ||
		    ++intern_steps > (u32)(copySize / sizeof(u32)))
		{
			spdlog::error("lbReloc bridge: file_id {} intern chain walk STOPPED "
			              "(off={} steps={} copySize={}) — corrupt chain word",
			              file_id, reloc_intern, intern_steps, copySize);
			port_log("SSB64: chainWalk STOP intern file=%u off=0x%x steps=%u size=0x%zx\n",
			         file_id, (unsigned)reloc_intern, (unsigned)intern_steps, copySize);
			break;
		}
		u32 *slot = (u32 *)((uintptr_t)ram_dst + (reloc_intern * sizeof(u32)));

		// Read the reloc descriptor before we overwrite the slot.
		// After the blanket u32 byte-swap, native u32 reads produce the
		// same values as on the N64 (big-endian).  The struct layout is:
		//   bits [31:16] = next descriptor offset (word index), 0xFFFF = end
		//   bits [15:0]  = target offset within this file (word index)
		u16 next_reloc = (u16)(*slot >> 16);
		u16 words_num  = (u16)(*slot & 0xFFFF);

		/* An intern target outside the file means this chain word is not a
		 * chain word (corruption, or the walk already left the rails). */
		if ((size_t)words_num * sizeof(u32) >= copySize)
		{
			spdlog::error("lbReloc bridge: file_id {} intern chain target OOB "
			              "(slot_off={} target_words={} copySize={}) — stopping walk",
			              file_id, reloc_intern, words_num, copySize);
			port_log("SSB64: chainWalk STOP intern-target-oob file=%u slot=0x%x tgt=0x%x size=0x%zx\n",
			         file_id, (unsigned)reloc_intern, (unsigned)words_num, copySize);
			break;
		}

		// All reloc chain entries are intra-file pointers.  Tokenize them
		// normally so the resource system can resolve them to PC addresses.
		//
		// Note: G_DL commands that reference segment 0x0E are NOT in the reloc
		// chain — they exist as raw 0x0Exxxxxx values in the ROM data.
		// These are intra-file sub-DL references resolved to absolute
		// addresses by portNormalizeDisplayListPointer at widening time.
		//
		// Texture fixup happens inside the helper: if this slot is the w1
		// of a G_SETTIMG cmd, the chain encoding gives us the in-file
		// target offset where the texture bytes live; pass2's seg==0x0E
		// scan can't see these. Idempotent.
		portRelocApplyInternSlot(ram_dst, copySize,
		                         (uint32_t)(reloc_intern * sizeof(u32)),
		                         (uint32_t)(words_num * sizeof(u32)),
		                         /* texture_fixup */ true, /* note_chain */ true,
		                         is_fighter_figatree ? &figatree_reloc_words : nullptr);

		reloc_intern = next_reloc;
	}

	// --- External pointer relocation (token-based) ---
	//
	// Same chain structure, but the target is in a DIFFERENT file.
	// The extern file IDs come from the RelocFile metadata (extracted
	// by Torch at ROM-extraction time), not from ROM DMA.

	u16 reloc_extern = reloc_extern_offset;
	u32 extern_idx = 0;
	u32 extern_steps = 0;

	while (reloc_extern != 0xFFFF)
	{
		/* Same containment as the intern walk: slots must lie inside this
		 * file and a legit chain can't have more entries than file words.
		 * (words_num indexes the DEPENDENCY file, so it can't be bounds-
		 * checked here; extern_idx >= extern_count below covers the rest.) */
		if ((size_t)reloc_extern * sizeof(u32) + sizeof(u32) > copySize ||
		    ++extern_steps > (u32)(copySize / sizeof(u32)))
		{
			spdlog::error("lbReloc bridge: file_id {} extern chain walk STOPPED "
			              "(off={} steps={} copySize={}) — corrupt chain word",
			              file_id, reloc_extern, extern_steps, copySize);
			port_log("SSB64: chainWalk STOP extern file=%u off=0x%x steps=%u size=0x%zx\n",
			         file_id, (unsigned)reloc_extern, (unsigned)extern_steps, copySize);
			break;
		}
		u32 *slot = (u32 *)((uintptr_t)ram_dst + (reloc_extern * sizeof(u32)));

		u16 next_reloc = (u16)(*slot >> 16);
		u16 words_num  = (u16)(*slot & 0xFFFF);

		if (extern_idx >= extern_count)
		{
			spdlog::error("lbReloc bridge: file_id {} extern reloc overrun "
			              "(idx={}, count={})", file_id, extern_idx,
			              extern_count);
			break;
		}

		u16 dep_file_id = extern_file_ids[extern_idx];
		void *vaddr_extern;

		// Check if dependency is already loaded
		if (loc == nLBFileLocationForce)
		{
			vaddr_extern = lbRelocFindForceStatusBufferFile(dep_file_id);
		}
		else
		{
			vaddr_extern = lbRelocFindStatusBufferFile(dep_file_id);
		}

		// Load dependency if not cached
		if (vaddr_extern == NULL)
		{
			switch (loc)
			{
			case nLBFileLocationExtern:
				vaddr_extern = lbRelocGetExternBufferFile(dep_file_id);
				break;
			case nLBFileLocationDefault:
				vaddr_extern = lbRelocGetInternBufferFile(dep_file_id);
				break;
			case nLBFileLocationForce:
				vaddr_extern = lbRelocGetForceExternBufferFile(dep_file_id);
				break;
			}
		}

		// Compute target pointer (offset into the dependency file's data)
		void *target = (void *)((uintptr_t)vaddr_extern + (words_num * sizeof(u32)));

		u32 token = portRelocRegisterPointer(target);

		if (is_fighter_figatree && (reloc_extern < figatree_reloc_words.size()))
		{
			figatree_reloc_words[reloc_extern] = 1;
		}
		*slot = token;
		portRelocNoteChainSlot(slot);

		extern_idx++;
		reloc_extern = next_reloc;
	}

	} // explicit_reloc == nullptr (chain walks)

	if (is_fighter_figatree)
	{
		portRelocFixupFighterFigatree(ram_dst, copySize, figatree_reloc_words);
		/* Register the halfswapped range so port_aobj_event32_unhalfswap_stream
		 * knows to only touch EVENT32 streams inside this file's memory. */
		port_aobj_register_halfswapped_range(ram_dst, (unsigned long)copySize);
	}

	{
		extern void portStageAuditEmitLoadSummary(unsigned int file_id, const char *path, size_t size);
		extern void portStageAuditEmitOpcodeCensus(unsigned int file_id, const char *path, const void *data, size_t size);
		portStageAuditEmitLoadSummary(file_id, table_path, copySize);
		portStageAuditEmitOpcodeCensus(file_id, table_path, ram_dst, copySize);
	}
}

/* Stable C ABI (mod-facing, see doc above): chain-walk relocation only. */
extern "C" void portRelocLoadFileFromBytes(
	unsigned int    file_id,
	void           *ram_dst,
	unsigned int    bytes_num,
	int             loc,
	const void     *src_bytes,
	unsigned int    src_size,
	unsigned short  reloc_intern_offset,
	unsigned short  reloc_extern_offset,
	const unsigned short *extern_file_ids,
	unsigned int    extern_count,
	int             force_figatree_fixup)
{
	portRelocLoadFileFromBytesImpl(file_id, ram_dst, bytes_num, loc,
	                               src_bytes, src_size,
	                               reloc_intern_offset, reloc_extern_offset,
	                               extern_file_ids, extern_count,
	                               force_figatree_fixup,
	                               /* explicit_reloc */ nullptr);
}

/**
 * Mod-private variant of portRelocLoadFileFromBytes: copies src_bytes
 * into a mod-owned buffer, byte-swaps to native endian, and walks the
 * intern reloc chain to register pointer tokens. Skips status-buffer
 * registration, extern chain walks, cache evictions, and audit emission
 * because the caller's buffer isn't shared with the engine's per-scene
 * heap (so engine cache invariants don't apply, and consuming a slot in
 * the per-scene status buffer would steal it from the engine's own
 * file loads). Used by mods (via ce_load_reloc_blob) to host long-lived
 * reloc files (e.g., SR-extended menu sprite blobs) that need to flow
 * through the engine's reloc pipeline so internal pointer tokens
 * resolve, but don't belong in the engine's per-scene file table.
 *
 * Re-entrant safety: callable from inside lbRelocInitSetup's tail (e.g.,
 * via the CharacterEngine post-reset callback) because nothing here
 * touches lbRelocInternBuffer or other per-scene engine state.
 */
/* Extended form: `explicit_intern_pairs` is a flat u32 array of
 * (slot_byte_off, target_byte_off) pairs (chain order) for mod bytes whose
 * chain was pre-relocated away — i.e. a mod re-serving a deblobbed
 * bundle's synthesized data (docs/deblob.md). Pass nullptr/0 for ordinary
 * chain-intact reloc bytes; C-flat layout so libtcc mods can call it. */
extern "C" void portRelocLoadFileFromBytesPrivateEx(
	void           *ram_dst,
	unsigned int    dst_size,
	const void     *src_bytes,
	unsigned int    src_size,
	unsigned short  reloc_intern_offset,
	const unsigned int *explicit_intern_pairs,
	unsigned int    explicit_pair_count)
{
	if (src_bytes == nullptr || src_size == 0 || ram_dst == nullptr) {
		return;
	}

	size_t copySize = src_size;
	if (copySize > dst_size && dst_size > 0) {
		spdlog::warn("portRelocLoadFileFromBytesPrivate: src ({} bytes) > dst ({} bytes), truncating",
		             copySize, dst_size);
		copySize = dst_size;
	}

	// Invalidate any prior fixup state / cached uploads that still reference
	// the buffer we're about to overwrite. Each call must redo
	// portFixupSprite / portFixupBitmapArray / portFixupSpriteBitmapData on
	// the freshly-loaded data; without these evictions the fixup tracker
	// thinks "already done" and skips the BE-restore + TMEM swizzle, leaving
	// the texel data in the wrong byte order for the RDP.
	portEvictStructFixupsInRange(ram_dst, copySize);
	extern void portTextureCacheDeleteRange(const void *base, size_t size);
	portTextureCacheDeleteRange(ram_dst, copySize);
	extern void portPackedDisplayListCacheDeleteRange(const void *base, size_t size);
	portPackedDisplayListCacheDeleteRange(ram_dst, copySize);

	memcpy(ram_dst, src_bytes, copySize);
	syNetRollbackNoteFileLoad(ram_dst, copySize, 0);

	// Byte-swap from N64 BE to native LE; same call portRelocLoadFileFromBytes
	// makes inside its public path. file_id is unused by the swap logic for
	// non-figatree files but the API requires a value; pass 0 since this is
	// a mod-private buffer that has no engine file_id.
	portRelocByteSwapBlob(ram_dst, copySize, /* file_id = */ 0u);

	// Register the buffer's address range so portRelocFindContainingFile can
	// see it. Display lists inside a reloc file encode their vertex / matrix /
	// sub-DL pointers as segment-0x0E offsets (0x0E0xxxxx). Those only get
	// rewritten to fileBase+offset by portNormalizeDisplayListPointer, which
	// bails immediately unless the DL pointer resolves to a registered range.
	// The public loader pushes the range above; the private path used
	// to skip it, so a mod-private DL (e.g. KirbyHatEngine's 0xA8D custom-hat
	// models) was never widened — the interpreter read the packed 8-byte
	// commands as native 16-byte and fed the raw 0x0Exxxxxx vertex pointer to
	// GfxSpVertex, AV'ing on a garbage host address. Re-loads reuse the same
	// buffer, so evict any prior range for this region before re-registering.
	portRelocEvictFileRangesInRange(ram_dst, copySize);
	sPortRelocFileRanges.push_back({ reinterpret_cast<uintptr_t>(ram_dst),
	                                 copySize, /* file_id = */ 0xFFFFFFFFu,
	                                 "(mod-private)" });

	/* Mirror into the DL-range registry as the public path does, so
	 * gfx_step's runaway-walker bounds check accepts mod-private DLs
	 * (portRelocEvictFileRangesInRange above also unregisters any prior
	 * DL range for this buffer, so re-loads must re-register). */
	port_dl_range_register(ram_dst, copySize, "(mod-private)");

	// Intern relocation. Same per-slot logic as the public loader (shared
	// helper) but without texture-fixup (no SETTIMG-driven texture cache
	// for the mod-private buffer), figatree halfswap, or chain-slot notes.
	if (explicit_intern_pairs != nullptr)
	{
		// Explicit flat list: (slot, target) byte-offset pairs, for
		// pre-relocated (deblobbed-bundle) bytes whose chain is gone.
		for (unsigned int i = 0; i < explicit_pair_count; i++)
		{
			uint32_t slot_off = explicit_intern_pairs[i * 2];
			uint32_t target_off = explicit_intern_pairs[i * 2 + 1];
			if ((size_t)slot_off + sizeof(u32) > copySize ||
			    (size_t)target_off >= copySize)
			{
				spdlog::error("portRelocLoadFileFromBytesPrivateEx: explicit slot {} OOB "
				              "(slot=0x{:X} target=0x{:X} size=0x{:X})",
				              i, slot_off, target_off, copySize);
				break;
			}
			portRelocApplyInternSlot(ram_dst, copySize, slot_off, target_off,
			                         /* texture_fixup */ false,
			                         /* note_chain */ false, nullptr);
		}
		return;
	}

	u16 reloc_intern = reloc_intern_offset;
	u32 intern_steps = 0;
	while (reloc_intern != 0xFFFF) {
		/* Same containment as the public intern walk (see there). */
		if ((size_t)reloc_intern * sizeof(u32) + sizeof(u32) > copySize ||
		    ++intern_steps > (u32)(copySize / sizeof(u32))) {
			spdlog::error("portRelocLoadFileFromBytesPrivate: chain walk STOPPED "
			              "(off={} steps={} copySize={}) — corrupt chain word",
			              reloc_intern, intern_steps, copySize);
			break;
		}
		u32 *slot = (u32 *)((uintptr_t)ram_dst + (reloc_intern * sizeof(u32)));
		u16 next_reloc = (u16)(*slot >> 16);
		u16 words_num  = (u16)(*slot & 0xFFFF);

		if ((size_t)words_num * sizeof(u32) >= copySize) {
			spdlog::error("portRelocLoadFileFromBytesPrivate: chain target OOB "
			              "(slot_off={} target_words={} copySize={}) — stopping walk",
			              reloc_intern, words_num, copySize);
			break;
		}

		portRelocApplyInternSlot(ram_dst, copySize,
		                         (uint32_t)(reloc_intern * sizeof(u32)),
		                         (uint32_t)(words_num * sizeof(u32)),
		                         /* texture_fixup */ false,
		                         /* note_chain */ false, nullptr);

		reloc_intern = next_reloc;
	}
}

/* Stable C ABI kept for already-shipped TCC mods (ce_load_reloc_blob). */
extern "C" void portRelocLoadFileFromBytesPrivate(
	void           *ram_dst,
	unsigned int    dst_size,
	const void     *src_bytes,
	unsigned int    src_size,
	unsigned short  reloc_intern_offset)
{
	portRelocLoadFileFromBytesPrivateEx(ram_dst, dst_size, src_bytes, src_size,
	                                    reloc_intern_offset, nullptr, 0);
}

/**
 * Load a file from the .o2r archive, copy into ram_dst, and perform
 * token-based internal + external pointer relocation. Thin wrapper
 * around portRelocLoadFileFromBytes that sources its bytes via the
 * libultraship ResourceManager (i.e., the BattleShip.o2r /
 * BattleShip.fromsource.o2r pipeline).
 */
void lbRelocLoadAndRelocFile(u32 file_id, void *ram_dst, u32 bytes_num, s32 loc)
{
	auto relocFile = portLoadRelocResource(file_id);
	if (!relocFile)
	{
		spdlog::error("lbReloc bridge: cannot load file_id {} — halting", file_id);
		return;
	}

	// Synthesized (deblobbed) bundles carry explicit slot lists instead of
	// walkable in-data chains (docs/deblob.md).
	PortRelocExplicitRelocation explicit_reloc;
	const PortRelocExplicitRelocation *explicit_ptr = nullptr;
	if (relocFile->HasExplicitRelocation)
	{
		explicit_reloc.intern_slots = relocFile->ExplicitInternSlots.data();
		explicit_reloc.intern_count = relocFile->ExplicitInternSlots.size();
		explicit_reloc.extern_slots = relocFile->ExplicitExternSlots.data();
		explicit_reloc.extern_count = relocFile->ExplicitExternSlots.size();
		explicit_ptr = &explicit_reloc;
	}

	portRelocLoadFileFromBytesImpl(
		(unsigned int)file_id,
		ram_dst,
		bytes_num,
		(int)loc,
		relocFile->Data.data(),
		(unsigned int)relocFile->Data.size(),
		relocFile->RelocInternOffset,
		relocFile->RelocExternOffset,
		relocFile->ExternFileIds.data(),
		(unsigned int)relocFile->ExternFileIds.size(),
		/* force_figatree_fixup = */ 0,
		explicit_ptr);
}

// // // // // // // // // // // //
//                               //
//    BRIDGE: SIZE CALCULATION   //
//                               //
// // // // // // // // // // // //

} // extern "C"
