/* Internal shared declarations for the lbreloc bridge translation units —
 * the pipeline overview lives in lbreloc_bridge.cpp. Only the
 * lbreloc_*.cpp files include this header; the decomp-facing ABI keeps
 * its C linkage per function.
 *
 * Split layout:
 *   lbreloc_status.cpp   file-range registry, cache eviction, figatree
 *                        fixup, ResourceManager fetch, status buffers
 *   lbreloc_loader.cpp   token-based relocate pipeline + mod-private loads
 *   lbreloc_query.cpp    buffer/heap getters (extern/intern/force tables)
 *   lbreloc_bridge.cpp   globals, batch loads, pointer lookup, init
 */
#pragma once

#include <ship/Context.h>
#include <ship/resource/ResourceManager.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
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

// Bridge-local type definitions.
// These MUST be ABI-compatible with the decomp definitions in lbtypes.h.
// We define them here to avoid including the decomp's include/ directory
// (which shadows system headers and breaks C++ standard library includes).
#include "port_types.h"
#include "port_log.h"

#define LBRELOC_CACHE_ALIGN(x) (((x) + 0xF) & ~0xF)

enum {
	nLBFileLocationExtern  = 0,
	nLBFileLocationDefault = 1,
	nLBFileLocationForce   = 2,
};

struct LBFileNode
{
	u32 id;
	void *addr;
};

struct LBRelocSetup
{
	uintptr_t table_addr;
	u32 table_files_num;
	void *file_heap;
	size_t file_heap_size;
	LBFileNode *status_buffer;
	size_t status_buffer_size;
	LBFileNode *force_status_buffer;
	size_t force_status_buffer_size;
};

struct LBInternBuffer
{
	uintptr_t rom_table_lo;
	u32 total_files_num;
	uintptr_t rom_table_hi;
	void *heap_start;
	void *heap_ptr;
	void *heap_end;
	s32 status_buffer_num;
	s32 status_buffer_max;
	LBFileNode *status_buffer;
	s32 force_status_buffer_num;
	s32 force_status_buffer_max;
	LBFileNode *force_status_buffer;
};

// Same size as the N64 LBTableEntry (12 bytes) — used for size calculation
struct LBTableEntry
{
	ub32 is_compressed : 1;
	u32 data_offset : 31;
	u16 reloc_intern_offset;
	u16 compressed_size;
	u16 reloc_extern_offset;
	u16 decompressed_size;
};

struct PortRelocFileRange
{
	uintptr_t base;
	size_t size;
	u32 file_id;
	const char *path;
};

/* Explicit relocation input for synthesized (deblobbed) bundles: their
 * chain descriptor words were replaced with real content at extraction, so
 * the loader applies these flat, chain-ordered lists instead of walking
 * the in-data chains (docs/deblob.md, RelocFile.h). */
struct PortRelocExplicitRelocation
{
	const RelocExplicitInternSlot *intern_slots;
	size_t intern_count;
	const RelocExplicitExternSlot *extern_slots;
	size_t extern_count;
};

/* ---- shared state (defined in lbreloc_bridge.cpp) ---- */
extern LBInternBuffer sLBRelocInternBuffer;
extern u32 *sLBRelocExternFileIDs;
extern s32 sLBRelocExternFileIDsNum;
extern s32 sLBRelocExternFileIDsMax;
extern void *sLBRelocExternFileHeap;
extern std::vector<PortRelocFileRange> sPortRelocFileRanges;

extern "C" {
extern void syDebugPrintf(const char *fmt, ...);
extern void scManagerRunPrintGObjStatus(void);
extern void portResetPackedDisplayListCache(void);
extern void gmColScriptsLinkRelocTargets(void);
extern void scSubsysMotionLinkRelocTargets(void);
extern void syNetRollbackNoteFileLoad(void *dst, size_t size, int is_figatree);
extern void port_aobj_register_halfswapped_range(void *base, unsigned long size);

// DL-source range registry (see port/port_dl_ranges.h).
void port_dl_range_register(const void *base, size_t size, const char *label);
void port_dl_range_unregister(const void *base);
}

/* ---- lbreloc_status.cpp ---- */
void portRelocEvictFileRangesInRange(void *base, size_t size);
extern "C" void port_taskman_evict_arena_caches(const void *base, size_t size);
bool portRelocIsFighterFigatreeFile(u32 file_id);
void portRelocFixupFighterFigatree(void *ram_dst, size_t copy_size,
                                   const std::vector<uint8_t> &reloc_words);
std::shared_ptr<RelocFile> portLoadRelocResource(u32 file_id);
extern "C" void* lbRelocFindStatusBufferFile(u32 id);
extern "C" void* lbRelocFindForceStatusBufferFile(u32 id);
extern "C" void lbRelocAddStatusBufferFile(u32 id, void *addr);
extern "C" void lbRelocAddForceStatusBufferFile(u32 id, void *addr);

/* ---- lbreloc_loader.cpp ---- */
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
	int             force_figatree_fixup);
extern "C" void lbRelocLoadAndRelocFile(u32 file_id, void *ram_dst, u32 bytes_num, s32 loc);

/* ---- lbreloc_query.cpp ---- */
extern "C" size_t lbRelocGetExternBytesNum(u32 file_id);
extern "C" void* lbRelocGetExternBufferFile(u32 id);
extern "C" void* lbRelocGetInternBufferFile(u32 id);
extern "C" void* lbRelocGetForceExternBufferFile(u32 id);
