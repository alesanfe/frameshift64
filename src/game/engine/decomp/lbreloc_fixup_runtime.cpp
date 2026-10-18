/**
 * lbreloc_fixup_runtime.cpp — lazy runtime fixups at interpreter execute time
 *
 * Split out of lbreloc_byteswap.cpp (phase-0 complexity refactor). The
 * shared tracking containers and bridge externs it uses live in
 * lbreloc_fixup_internal.h / lbreloc_fixup_state.cpp.
 */

#include "lbreloc_fixup_internal.h"
#include "lbreloc_debug.h"

// ============================================================
//  Lazy runtime fixup (Option A)
// ============================================================
//
// Called from the interpreter's gfx_vtx_handler_f3dex2 with the resolved
// vertex array address and the cmd's num_vtx.  If the address is inside a
// reloc file (i.e., not heap-built data), apply the per-Vtx byte permutation
// idempotently.
//
// Why this is correct: only data the running interpreter actually treats as
// vertices reaches this function — there's no guessing.  Each target's "type"
// is unambiguously vertex because a real G_VTX cmd just dispatched it.

// Lazy texture byte-order fixup at G_LOADBLOCK / G_LOADTLUT execute time.
// Called from libultraship's GfxDpLoadBlock and GfxDpLoadTLUT once both
// the texture address (set by G_SETTIMG) and the texel count are known.
//
// This catches textures referenced by RUNTIME-BUILT DLs that pass2 doesn't
// see (no in-file SETTIMG/LOADBLOCK pair) and that the chain walk also
// doesn't see (the slot was looked up via an MObjSub field, not a chain
// entry whose preceding cmd is SETTIMG).  Fighter material textures are
// the most common example: lbCommonAddMObjForFighterPartsDObj walks the
// fighter's MObjSub array at draw time and builds the SETTIMG/LOADBLOCK
// pairs in heap, never embedding them in any file DL.
//
// Fast3D's texel readers expect bytes in N64 BE byte order:
//   4b/8b: per-byte data, addr[0] is the first pixel
//   16b: pixels are BE u16, read via `(addr[0] << 8) | addr[1]`
//   32b: pixels are BE [R,G,B,A], read addr[0]=R, addr[1]=G, ...
// Pass1 BSWAP32 reversed the byte order within each u32 word, so we apply
// BSWAP32 again to restore the original layout.
//
// Idempotent at word granularity so overlapping runtime loads from different
// starts cannot undo each other.
extern "C" void portRelocFixupTextureAtRuntime(const void *addr, unsigned int num_bytes)
{
	if (addr == nullptr || num_bytes == 0) return;

	// Only fix data that lives inside a reloc file blob.  Heap-built textures
	// (procedurally generated, copied from elsewhere, etc.) are already in
	// correct host byte order.
	uintptr_t fileBase = 0;
	size_t    fileSize = 0;
	int in_reloc_file = portRelocFindContainingFile(addr, &fileBase, &fileSize);
	portStageAuditNoteTexDispatch(in_reloc_file);
	if (!in_reloc_file)
	{
		// Log heap textures so we can spot any geometry texture that
		// somehow lives outside any reloc file (the "third path" case).
		if (tex_log_enabled()) {
			char heap_note[40];
			std::snprintf(heap_note, sizeof(heap_note),
			              "addr=0x%016llX",
			              (unsigned long long)reinterpret_cast<uintptr_t>(addr));
			tex_log_emit("runtime.heap", -1,
			             0u, num_bytes, -1, -1, -1, addr, heap_note);
		}
		return;
	}

	uintptr_t target = reinterpret_cast<uintptr_t>(addr);
	size_t    target_offset = target - fileBase;
	if (target_offset >= fileSize)
	{
		return;
	}
	size_t available_bytes = fileSize - target_offset;
	if ((size_t)num_bytes > available_bytes)
	{
		num_bytes = (unsigned int)available_bytes;
	}

	// Clip the fix range so it can't enter a previously-registered protected
	// struct array (bitmap array, sprite, mobjsub, ...). These are already in
	// native byte order and a blind BSWAP32 pass would corrupt them. The
	// typical offender is a CI8 palette load that Fast3D treats as a fixed
	// 512-byte read even when the actual palette is smaller; the extra bytes
	// happen to overlap the bitmap/sprite struct region on sprite files.
	{
		uintptr_t clip_end = target + num_bytes;
		for (const auto &r : sProtectedStructRanges)
		{
			if (r.begin >= clip_end) continue;
			if (r.end <= target) continue;
			// Overlap. Only clip if the protected range starts *after* target,
			// so we still fix the prefix that lives outside the struct. If
			// the protected range starts at or before target, shrink num_bytes
			// to zero — the whole range overlaps a struct and we should skip it.
			if (r.begin > target)
			{
				if (r.begin < clip_end)
					clip_end = r.begin;
			}
			else
			{
				clip_end = target;
				break;
			}
		}
		num_bytes = (clip_end > target) ? (unsigned int)(clip_end - target) : 0u;
	}

	// Round UP to next 4-byte boundary, then clamp to file bounds. Issue #4:
	// a CI4 LOADTLUT with count=3 calls in with num_bytes=6, which under the
	// old `&= ~3u` round-DOWN became 4 — leaving the second 4-byte word
	// (containing palette[2]'s low byte) in pass1-BSWAP state. The N64
	// LOADTLUT only copies 6 bytes into TMEM, but pass1 mangled the whole
	// containing word, so we must un-mangle the whole word for the loaded
	// portion to read correctly. Bounds-clamped because rounding up can't
	// validly extend past file end.
	num_bytes = (unsigned int)(((size_t)num_bytes + 3u) & ~(size_t)3u);
	if ((size_t)num_bytes > available_bytes)
		num_bytes = (unsigned int)(available_bytes & ~(size_t)3u);
	if (num_bytes == 0) return;

	// If pass2 or the chain walk already fixed this exact base and runtime has
	// never seen it, keep the old conservative skip. Runtime-owned fixups below
	// are tracked per word to handle overlapping TLUT/LOADBLOCK ranges.
	if (sTexFixupExtent.find(target) == sTexFixupExtent.end() &&
	    sStructU16Fixups.count(target)) {
		if (tex_log_enabled()) {
			int rt_file_id = portRelocFindFileIdAndBase(addr, nullptr);
			tex_log_emit("runtime.skip", rt_file_id,
			             (uint32_t)target_offset, num_bytes,
			             -1, -1, -1, addr, "already-fixed");
		}
		return;
	}

	static unsigned int sRuntimeTexTraceCount = 0;
	if (sRuntimeTexTraceCount < 512)
	{
		uintptr_t descBase = 0;
		size_t descSize = 0;
		unsigned int descFileId = 0;
		const char *descPath = nullptr;
		int described = portRelocDescribePointer(addr, &descBase, &descSize, &descFileId, &descPath);
		port_log("SSB64: runtimeTexFix addr=%p req=0x%x fileBase=%p fileSize=0x%zx off=0x%zx num=0x%x desc=%d file=%u descBase=%p descSize=0x%zx path=%s\n",
		         addr, num_bytes, (void*)fileBase, fileSize, target_offset, num_bytes,
		         described, descFileId, (void*)descBase,
		         descSize, descPath ? descPath : "(unknown)");
		sRuntimeTexTraceCount++;
	}

	/* Clamp the swap at the first tokenized reloc chain slot in the range.
	 *
	 * N64 LoadBlock/LoadTLUT legitimately over-read past the true texel
	 * span (fixed block sizes); on hardware the extra bytes only landed in
	 * TMEM and were never written back. This fixup MUTATES source memory,
	 * so swapping the over-read tail byte-reverses live tokens and struct
	 * words in place — observed as fighter-file DObjDesc arrays wholesale
	 * BSWAP32'd (tokens included), cascading into textureless objects,
	 * impossible-generation token warnings, and SIGSEGV in the desc walk.
	 *
	 * Texel data never contains chain slots, so the first slot marks the
	 * end of trustworthy texture bytes: swap up to it, leave the rest.
	 * The clamped tail stays in its live (already correct) byte order and
	 * the GPU merely samples a few nonsense over-read texels the original
	 * hardware also displayed as garbage-in-TMEM. */
	{
		extern void *portRelocTryResolvePointer(uint32_t token);
		uintptr_t scan_end = target + num_bytes;
		for (uintptr_t w = target; w < scan_end; w += 4)
		{
			if (!sChainSlotAddrs.count(w))
				continue;
			/* Registry entries can outlive their file epoch: a reload at
			 * the same address (force-heap rewind, bump reuse) evicts only
			 * the NEW file's smaller range, leaving corpse addresses from
			 * the old file's tail. A live slot must hold a token that
			 * actually resolves; anything else is a corpse — drop it and
			 * keep the texture swap intact (clamping on a corpse truncates
			 * real texels and renders the texture garbled). */
			uint32_t slot_word = *reinterpret_cast<const uint32_t *>(w);
			if (slot_word == 0 || portRelocTryResolvePointer(slot_word) == nullptr)
			{
				sChainSlotAddrs.erase(w);
				continue;
			}
			static unsigned int sClampLogCount = 0;
			if (sClampLogCount < 64)
			{
				int cl_file_id = portRelocFindFileIdAndBase(addr, nullptr);
				port_log("SSB64: runtimeTexFix CLAMPED addr=%p num=0x%x->0x%x file=%d — "
				         "live reloc chain slot at %p bounds the texel span\n",
				         addr, num_bytes, (unsigned int)(w - target), cl_file_id, (void *)w);
				sClampLogCount++;
			}
			num_bytes = (unsigned int)(w - target);
			break;
		}
		if (num_bytes == 0)
			return;
	}

	uint32_t *region = reinterpret_cast<uint32_t *>(target);
	size_t num_words = num_bytes / 4;
	size_t fixed_words = 0;
	size_t skipped_words = 0;
	for (size_t i = 0; i < num_words; i++)
	{
		uintptr_t word_key = target + (uintptr_t)i * 4;
		if (sTexFixupWords.count(word_key)) {
			skipped_words++;
			continue;
		}
		sTexFixupWords.insert(word_key);
		region[i] = BSWAP32(region[i]);
		fixed_words++;
	}

	if (fixed_words == 0)
	{
		if (tex_log_enabled()) {
			int rt_file_id = portRelocFindFileIdAndBase(addr, nullptr);
			tex_log_emit("runtime.skip", rt_file_id,
			             (uint32_t)target_offset, num_bytes,
			             -1, -1, -1, addr, "already-fixed");
		}
		return;
	}

	sStructU16Fixups.insert(target);
	auto extent_it = sTexFixupExtent.find(target);
	if (extent_it == sTexFixupExtent.end() || extent_it->second < num_bytes)
		sTexFixupExtent[target] = num_bytes;

	if (tex_log_enabled()) {
		int rt_file_id = portRelocFindFileIdAndBase(addr, nullptr);
		char note[64];
		std::snprintf(note, sizeof(note), "fixed=%zu skipped=%zu",
		              fixed_words, skipped_words);
		tex_log_emit("runtime.fix", rt_file_id,
		             (uint32_t)target_offset, num_bytes,
		             -1, -1, -1, reinterpret_cast<uint32_t *>(target), note);
	}

	portStageAuditNoteRuntimeTex((unsigned int)(fixed_words * 4));
}

extern "C" void portRelocFixupVertexAtRuntime(const void *addr, unsigned int num_vtx)
{
	if (addr == nullptr || num_vtx == 0 || num_vtx > 32) return;

	// Only fix data that lives inside a reloc file blob.  Heap-built vertex
	// arrays (e.g., dynamic UI sprites) are constructed by game code with
	// already-correct byte order on the host LE side.
	uintptr_t fileBase = 0;
	size_t    fileSize = 0;
	int in_reloc_file = portRelocFindContainingFile(addr, &fileBase, &fileSize);
	portStageAuditNoteVtxDispatch(in_reloc_file);
	{
		int rt_file_id = in_reloc_file ? portRelocFindFileIdAndBase(addr, nullptr) : -1;
		portStageAuditNoteVtxDispatchFile(rt_file_id);
	}
	if (!in_reloc_file)
	{
		return;
	}

	uintptr_t target = reinterpret_cast<uintptr_t>(addr);
	size_t target_offset = target - fileBase;
	size_t total_bytes = (size_t)num_vtx * 16;
	if (target_offset + total_bytes > fileSize) return;

	// PER-VERTEX idempotency.  Game code can re-load OVERLAPPING sub-ranges
	// of the same vertex array (e.g. cmd 1 loads vertices 0..15 at addr A,
	// cmd 2 loads vertices 1..14 at addr A+16).  Address-keyed tracking
	// would treat these as distinct keys and double-fix the overlap region.
	// Instead we mark each individual 16-byte Vtx; only un-marked vertices
	// get fixed.
	uint32_t *region = reinterpret_cast<uint32_t *>(target);
	unsigned int fixed_here = 0;
	unsigned int skipped_here = 0;
	for (unsigned int i = 0; i < num_vtx; i++)
	{
		uintptr_t vtx_key = target + (uintptr_t)i * 16;
		if (sStructU16Fixups.count(vtx_key)) { skipped_here++; continue; }
		sStructU16Fixups.insert(vtx_key);
		fixed_here++;

		// word 0: s16 ob[0] | s16 ob[1] → rotate16
		region[i * 4 + 0] = (region[i * 4 + 0] << 16) | (region[i * 4 + 0] >> 16);
		// word 1: s16 ob[2] | u16 flag → rotate16
		region[i * 4 + 1] = (region[i * 4 + 1] << 16) | (region[i * 4 + 1] >> 16);
		// word 2: s16 tc[0] | s16 tc[1] → rotate16
		region[i * 4 + 2] = (region[i * 4 + 2] << 16) | (region[i * 4 + 2] >> 16);
		// word 3: u8 cn[0..3] → bswap32 to restore byte order
		region[i * 4 + 3] = BSWAP32(region[i * 4 + 3]);
	}
	if (fixed_here > 0) portStageAuditNoteRuntimeVtx(fixed_here);

	/* Focused trace on ExternDataBank104 to see who's marking its vertices. */
	if (stage_audit_enabled()) {
		int rt_file_id = portRelocFindFileIdAndBase(addr, nullptr);
		if (rt_file_id == 104) {
			static uint32_t sBank104Trace = 0;
			sBank104Trace++;
			if (sBank104Trace <= 5) {
				const uint8_t *p = (const uint8_t *)addr;
				port_log("[STAGE_AUDIT_104] call#%u addr=%p num_vtx=%u fixed=%u skipped=%u offset=0x%zx bytes=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X\n",
				         sBank104Trace, addr, num_vtx, fixed_here, skipped_here,
				         (size_t)(target - fileBase),
				         p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
				         p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
			}
		}
	}
}
