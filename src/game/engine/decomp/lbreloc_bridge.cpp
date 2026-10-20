/**
 * lbreloc_bridge.cpp — PORT replacement for src/lb/lbreloc.c
 *
 * Replaces ROM DMA-based file loading with LUS ResourceManager calls.
 * Uses the token-based pointer indirection system: the relocation logic
 * computes real 64-bit pointers but stores 32-bit tokens in the 4-byte
 * data slots. Game code resolves tokens via RELOC_RESOLVE().
 *
 * This file is compiled as C++ (needs LUS headers) but exports C-linkage
 * functions matching the signatures in src/lb/lbreloc.h.
 *
 * Split: shared state/types live in lbreloc_bridge_internal.h; eviction,
 * status buffers, the relocate pipeline, and buffer getters live in
 * lbreloc_{status,loader,query}.cpp.
 */
#include "lbreloc_bridge_internal.h"

// // // // // // // // // // // //
//                               //
//   GLOBAL / STATIC VARIABLES   //
//                               //
// // // // // // // // // // // //

LBInternBuffer sLBRelocInternBuffer;

u32 *sLBRelocExternFileIDs;
s32 sLBRelocExternFileIDsNum;
s32 sLBRelocExternFileIDsMax;
void *sLBRelocExternFileHeap;

std::vector<PortRelocFileRange> sPortRelocFileRanges;

// All game-facing functions have C linkage
extern "C" {

// // // // // // // // // // // //
//                               //
//     BRIDGE: BATCH LOADING     //
//                               //
// // // // // // // // // // // //

size_t lbRelocLoadFilesExtern(u32 *ids, u32 len, void **files, void *heap)
{
	sLBRelocExternFileHeap = heap;

	while (len != 0)
	{
		*files = lbRelocGetExternBufferFile(*ids);
		ids++;
		files++;
		len--;
	}

	return (size_t)((uintptr_t)sLBRelocExternFileHeap - (uintptr_t)heap);
}

size_t lbRelocLoadFilesIntern(u32 *ids, u32 len, void **files)
{
	void *heap = sLBRelocInternBuffer.heap_ptr;

	while (len)
	{
		*files = lbRelocGetInternBufferFile(*ids);
		ids++;
		files++;
		len--;
	}

	return (size_t)((uintptr_t)sLBRelocInternBuffer.heap_ptr - (uintptr_t)heap);
}

size_t lbRelocGetAllocSize(u32 *ids, u32 len)
{
	u32 file_ids[50];
	size_t allocated = 0;

	sLBRelocExternFileIDs = file_ids;
	sLBRelocExternFileIDsNum = 0;
	sLBRelocExternFileIDsMax = ARRAY_COUNT(file_ids);

	while (len != 0)
	{
		allocated = LBRELOC_CACHE_ALIGN(allocated);
		allocated += lbRelocGetExternBytesNum(*ids);
		ids++;
		len--;
	}
	return allocated;
}

bool portRelocFindContainingFile(const void *ptr, uintptr_t *out_base, size_t *out_size)
{
	uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

	for (const auto &range : sPortRelocFileRanges)
	{
		uintptr_t start = range.base;
		size_t size = range.size;

		if ((addr >= start) && (size != 0) && ((addr - start) < size))
		{
			if (out_base != NULL)
			{
				*out_base = start;
			}
			if (out_size != NULL)
			{
				*out_size = size;
			}
			return TRUE;
		}
	}
	return FALSE;
}

/* C-callable helper: find both file_id and base for a pointer.
 * Returns -1 if not in any reloc file range. */
extern "C" int portRelocFindFileIdAndBase(const void *ptr, uintptr_t *out_base)
{
	uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
	for (const auto &range : sPortRelocFileRanges)
	{
		if ((addr >= range.base) && (range.size != 0) && ((addr - range.base) < range.size))
		{
			if (out_base != nullptr) *out_base = range.base;
			return (int)range.file_id;
		}
	}
	return -1;
}

/* Bytes remaining from ptr to the end of its registered reloc file range.
 * 0 when ptr is not inside any known file — callers treat that as
 * "cannot prove bounds" and refuse or clamp. Sprite/bitmap fields come
 * from the file blob and can be corrupted by a shadowed (modded) asset,
 * so the writes they drive must stay inside the image. */
extern "C" size_t portRelocFileExtentFrom(const void *ptr)
{
	uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
	for (const auto &range : sPortRelocFileRanges)
	{
		if ((addr >= range.base) && (range.size != 0) && ((addr - range.base) < range.size))
		{
			return range.size - (size_t)(addr - range.base);
		}
	}
	return 0;
}

/* Classify a pointer for diagnostic output. Writes a short human-readable
 * label into buf (e.g. "scene_arena+0x4528", "reloc[523]+0x120 path",
 * "n64_seg", "low_brk", "high_heap"). Used by the GFX diag dump
 * (libultraship interpreter.cpp diagDumpAll) to identify the upstream
 * holder behind a stale-pointer crash in gfx_step. Never derefs ptr —
 * uses only registered range metadata. */
extern void *gPortSceneHeap;
extern const size_t gPortSceneHeapSize;
extern "C" void port_classify_dl_ptr(uintptr_t addr, char *buf, size_t buf_size)
{
	if (buf == nullptr || buf_size == 0) return;
	if (addr == 0) {
		std::snprintf(buf, buf_size, "null");
		return;
	}
	if (addr <= 0x0FFFFFFFu) {
		std::snprintf(buf, buf_size, "n64_seg[%u]+0x%x",
		              (unsigned)((addr >> 24) & 0xFF),
		              (unsigned)(addr & 0x00FFFFFFu));
		return;
	}
	if (gPortSceneHeap != nullptr) {
		uintptr_t arena_base = reinterpret_cast<uintptr_t>(gPortSceneHeap);
		if (addr >= arena_base && (addr - arena_base) < gPortSceneHeapSize) {
			std::snprintf(buf, buf_size, "scene_arena+0x%lx",
			              (unsigned long)(addr - arena_base));
			return;
		}
	}
	uintptr_t reloc_base = 0;
	int file_id = portRelocFindFileIdAndBase(reinterpret_cast<const void*>(addr), &reloc_base);
	if (file_id >= 0) {
		const char *path = (file_id < (int)RELOC_FILE_COUNT && gRelocFileTable[file_id])
		                   ? gRelocFileTable[file_id] : "?";
		std::snprintf(buf, buf_size, "reloc[%d]+0x%lx %s",
		              file_id, (unsigned long)(addr - reloc_base), path);
		return;
	}
	/* No registered range. Tag by host-address heuristic. */
	if (addr < 0x100000000ull) {
		std::snprintf(buf, buf_size, "low_brk@0x%lx", (unsigned long)addr);
	} else if (addr >= 0x7f0000000000ull) {
		std::snprintf(buf, buf_size, "high_heap@0x%lx", (unsigned long)addr);
	} else {
		std::snprintf(buf, buf_size, "other@0x%lx", (unsigned long)addr);
	}
}

void *portRelocResolveArrayEntry(const void *array_ptr, unsigned int index)
{
	if (array_ptr == nullptr)
	{
		return nullptr;
	}

	uintptr_t base = 0;
	size_t size = 0;

	if (portRelocFindContainingFile(array_ptr, &base, &size))
	{
		uintptr_t addr = reinterpret_cast<uintptr_t>(array_ptr);
		size_t byte_offset = static_cast<size_t>(addr - base);
		size_t entry_offset = byte_offset + (static_cast<size_t>(index) * sizeof(uint32_t));

		if ((entry_offset > size) || ((size - entry_offset) < sizeof(uint32_t)))
		{
			return nullptr;
		}

		return portRelocResolvePointer(reinterpret_cast<const uint32_t *>(array_ptr)[index]);
	}

	return reinterpret_cast<void *const *>(array_ptr)[index];
}

bool portRelocDescribePointer(const void *ptr, uintptr_t *out_base, size_t *out_size, u32 *out_file_id, const char **out_path)
{
	uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);

	for (const auto &range : sPortRelocFileRanges)
	{
		uintptr_t start = range.base;
		size_t size = range.size;

		if ((addr >= start) && (size != 0) && ((addr - start) < size))
		{
			if (out_base != NULL)
			{
				*out_base = start;
			}
			if (out_size != NULL)
			{
				*out_size = size;
			}
			if (out_file_id != NULL)
			{
				*out_file_id = range.file_id;
			}
			if (out_path != NULL)
			{
				*out_path = range.path;
			}
			return TRUE;
		}
	}
	return FALSE;
}

// // // // // // // // // // // //
//                               //
//    BRIDGE: INITIALIZATION     //
//                               //
// // // // // // // // // // // //

void lbRelocInitSetup(LBRelocSetup *setup)
{
	/* Note: the historical portRelocResetPointerTable() call used to live
	 * here. It wholesale-invalidated every token in the table on every
	 * scene init, which is what created the variant-1/2/3 stale-data crash
	 * family — tokens pointing at INTERN-BUFFER files (mainmotion, sub-
	 * motion, model, special1-4, shieldpose; all persistent across scenes)
	 * would suddenly fail to resolve at scene N+1 even though their
	 * underlying memory was still live. The new per-slot generational
	 * model in RelocPointerTable.cpp handles invalidation surgically:
	 * port_taskman_evict_arena_caches() calls portRelocInvalidateRange()
	 * with the scene-arena range, so ONLY arena-backed tokens go stale.
	 * Persistent file tokens stay valid across scene cycles. No wholesale
	 * reset here. */
	gmColScriptsLinkRelocTargets();
	scSubsysMotionLinkRelocTargets();
	portResetPackedDisplayListCache();
	sPortRelocFileRanges.clear();

	// Clear u16 struct fixup tracking — addresses from the old heap are stale
	portResetStructFixups();

	// Clear FB-mirror registrations — see src/game/engine/decomp/framebuffer_capture.h.
	// The 1P stage-clear wallpaper buf and the lbtransition photo heap both
	// register their CPU pointer as a mirror of a snapshot FB; on scene change
	// the bump-reset heaps free those addresses and a fresh load could land
	// at the same address. Without this, the new asset would render the prior
	// scene's snapshot instead of its own pixels.
	extern void port_capture_release_all(void);
	port_capture_release_all();

	// ROM addresses (unused in port but stored for completeness)
	sLBRelocInternBuffer.rom_table_lo = setup->table_addr;
	sLBRelocInternBuffer.total_files_num = setup->table_files_num;
	sLBRelocInternBuffer.rom_table_hi = setup->table_addr +
		((setup->table_files_num + 1) * sizeof(LBTableEntry));

	// Heap management (still used — callers allocate and pass heaps)
	sLBRelocInternBuffer.heap_start = sLBRelocInternBuffer.heap_ptr = setup->file_heap;
	sLBRelocInternBuffer.heap_end = (void *)((uintptr_t)setup->file_heap + setup->file_heap_size);

	// Status buffers (file caching)
	sLBRelocInternBuffer.status_buffer_num = 0;
	sLBRelocInternBuffer.status_buffer_max = (s32)setup->status_buffer_size;
	sLBRelocInternBuffer.status_buffer = setup->status_buffer;

	sLBRelocInternBuffer.force_status_buffer_max = (s32)setup->force_status_buffer_size;
	sLBRelocInternBuffer.force_status_buffer_num = 0;
	sLBRelocInternBuffer.force_status_buffer = setup->force_status_buffer;
}

} // extern "C"
