/** lbreloc_status.cpp — file-range registry, arena eviction, fighter
 * figatree fixup, ResourceManager fetch, and the status-buffer tables.
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

void portRelocEvictFileRangesInRange(void *base, size_t size)
{
	if ((base == nullptr) || (size == 0))
	{
		return;
	}

	uintptr_t begin = reinterpret_cast<uintptr_t>(base);
	uintptr_t end = begin + size;

	/* Unregister matching ranges from the DL-range registry so a stale
	 * reloc-file pointer in cmd_stack is rejected on next gfx_step (rather
	 * than dereferencing into now-recycled memory). Done before the erase
	 * so we still have the base pointers. */
	for (const auto &r : sPortRelocFileRanges) {
		uintptr_t rb = r.base;
		uintptr_t re = rb + r.size;
		if ((r.size != 0) && (rb < end) && (begin < re)) {
			port_dl_range_unregister(reinterpret_cast<const void*>(rb));
		}
	}

	sPortRelocFileRanges.erase(
		std::remove_if(sPortRelocFileRanges.begin(), sPortRelocFileRanges.end(),
			[begin, end](const PortRelocFileRange &range) {
				uintptr_t range_begin = range.base;
				uintptr_t range_end = range_begin + range.size;

				return (range.size != 0) && (range_begin < end) && (begin < range_end);
			}),
		sPortRelocFileRanges.end());
}

/* Walk a status-buffer array, drop every entry whose .addr falls inside
 * [lo, hi). Uses unordered swap-with-last + decrement-count erase: lookup
 * iterates the whole list linearly so order doesn't matter. After this
 * runs, lbRelocFindStatusBufferFile(id) for an evicted file returns NULL,
 * which makes ftManagerSetupFilesKind's *p_file_* assignments produce
 * NULL globals and the existing `*file_head == NULL` defensive guard in
 * efManagerMakeEffect bail cleanly. The eventual file-reload path
 * re-allocates fresh memory and re-adds the entry on next request. */
static void portStatusBufferEvictRange(LBFileNode *entries, s32 *p_num,
                                        uintptr_t lo, uintptr_t hi)
{
	if (entries == nullptr || p_num == nullptr) return;
	s32 i = 0;
	while (i < *p_num)
	{
		uintptr_t a = reinterpret_cast<uintptr_t>(entries[i].addr);
		if (a >= lo && a < hi)
		{
			entries[i] = entries[*p_num - 1];
			(*p_num)--;
		}
		else
		{
			i++;
		}
	}
}

/* Called by syTaskmanStartTask before reusing the scene arena for the next
 * scene. Evicts every port-side cache that may hold a pointer into the old
 * arena's contents (DL widening, texture upload, struct fixup, reloc file
 * ranges) so a stale lookup can't resolve through prior-scene state. Same
 * eviction APIs run per-relocFile-load in port_reloc_lb_load_request. */
extern "C" void port_taskman_evict_arena_caches(const void *base, size_t size)
{
	if ((base == nullptr) || (size == 0)) return;
	extern void portPackedDisplayListCacheDeleteRange(const void *base, size_t size);
	extern void portTextureCacheDeleteRange(const void *base, size_t size);
	extern void portEvictStructFixupsInRange(const void *base, size_t size);
	portPackedDisplayListCacheDeleteRange(base, size);
	portTextureCacheDeleteRange(base, size);
	portEvictStructFixupsInRange(base, size);
	portRelocEvictFileRangesInRange(const_cast<void *>(base), size);
	/* Structural fix: invalidate tokens whose pointers fall in the recycled
	 * scene arena. With the per-slot generational model in RelocPointerTable
	 * .cpp, this NULLs only the affected slots (bumping their generation)
	 * and pushes their indices onto a free list for reuse. Tokens pointing
	 * at intern-buffer files (mainmotion, submotion, model, special1-4,
	 * shieldpose) — which persist across scenes — remain valid because
	 * they don't intersect this range. This is THE structural fix that
	 * eliminates the variant-1/2/3 stale-data crash family — see
	 * docs/bugs/linux_stale_scene_data_family_2026-05-11.md. */
	portRelocInvalidateRange(base, size);

	/* Variant 6 fix (silent particle/sprite corruption on cross-mode scene
	 * transitions, e.g. Training → quit → VS with overlapping fighters):
	 * sLBRelocInternBuffer.status_buffer[] is a flat decomp-side cache of
	 * {file_id → host addr} that lbRelocGetStatusBufferFile() reads. It is
	 * only reset in lbRelocInitSetup, which fires at mode entry, NOT per
	 * scene. Within a mode the cache accumulates; across modes that don't
	 * happen to traverse a mode-init path, entries from a prior mode
	 * survive — and when their .addr lives in the recycled scene arena,
	 * the cache returns a pointer to bytes the upcoming memset(0) is
	 * about to zero.
	 *
	 * Subsequent ftManagerSetupFilesPlayablesAll → ftManagerSetupFilesKind
	 * calls then assign that stale pointer into the per-character gFTData*
	 * globals (e.g. gFTDataMarioSpecial2). Downstream readers dereference
	 * them, read zero bytes, MObjSub.sprites resolves to NULL, the
	 * defensive `current_sprite != NULL` guard in gcDrawMObjForDObj skips
	 * gDPSetTextureImage, and particle quads emit without a texture bound
	 * — rendering as flat boxes. Same shape touches stage-sprite elements
	 * (clouds, bushes, foreground decoration) that use the particle draw
	 * path. Fighter mesh draws are unaffected because their model file
	 * lives in the intern buffer (out of arena range).
	 *
	 * Surgical fix: evict status-buffer entries whose .addr falls in the
	 * arena range, exactly mirroring the existing token-invalidation
	 * scope. Intern-buffer entries (out of arena range) survive untouched.
	 * After this, ftManagerSetupFilesKind's lookup for an evicted id
	 * returns NULL, the global is set NULL, and the file-load path runs
	 * fresh on next demand. */
	{
		uintptr_t lo = reinterpret_cast<uintptr_t>(base);
		uintptr_t hi = lo + size;
		portStatusBufferEvictRange(sLBRelocInternBuffer.status_buffer,
		                            &sLBRelocInternBuffer.status_buffer_num, lo, hi);
		portStatusBufferEvictRange(sLBRelocInternBuffer.force_status_buffer,
		                            &sLBRelocInternBuffer.force_status_buffer_num, lo, hi);
	}
}

bool portRelocIsFighterFigatreeFile(u32 file_id)
{
	static const char sFighterAnimPrefix[] = "reloc_animations/FT";
	static const char sFighterSubmotionPrefix[] = "reloc_submotions/FT";
	/* SCExplainMain contains arrays of FTKeyEvent u16s (per-player
	 * tutorial button inputs) and SCExplainPhase u16/u8 fields.
	 * The u16-halfswap applied by portRelocFixupFighterFigatree is
	 * what makes u16 pair reads produce the BE value on LE.
	 * Without it, u16[0] and u16[1] are swapped per u32, so the
	 * first FTKeyEvent reads from what should be the SECOND u16 of
	 * the pair — typically 0x0000, which parses as End and kills the
	 * tutorial's button scripting. */
	static const char sSCExplainMainPath[] = "reloc_scene/SCExplainMain";

	if (file_id >= RELOC_FILE_COUNT || gRelocFileTable[file_id] == NULL) return false;
	const char *path = gRelocFileTable[file_id];
	return (std::strncmp(path, sFighterAnimPrefix, sizeof(sFighterAnimPrefix) - 1) == 0) ||
	       (std::strncmp(path, sFighterSubmotionPrefix, sizeof(sFighterSubmotionPrefix) - 1) == 0) ||
	       (std::strcmp(path, sSCExplainMainPath) == 0);
}

void portRelocFixupFighterFigatree(void *ram_dst, size_t copy_size, const std::vector<uint8_t> &reloc_words)
{
	u32 *words;
	size_t word_count;
	size_t i;

	if ((ram_dst == NULL) || (copy_size < sizeof(u32)))
	{
		return;
	}
	words = (u32*)ram_dst;
	word_count = copy_size / sizeof(u32);

	if (reloc_words.size() < word_count)
	{
		word_count = reloc_words.size();
	}
	for (i = 0; i < word_count; i++)
	{
		if (reloc_words[i] == 0)
		{
			words[i] = (words[i] << 16) | (words[i] >> 16);
		}
	}
}

// // // // // // // // // // // //
//                               //
//       RESOURCE LOADING        //
//                               //
// // // // // // // // // // // //

std::shared_ptr<RelocFile> portLoadRelocResource(u32 file_id)
{
	if (file_id >= RELOC_FILE_COUNT || gRelocFileTable[file_id] == NULL)
	{
		spdlog::error("lbReloc bridge: invalid file_id {} (0x{:08X})", file_id, file_id);
		return nullptr;
	}

	auto ctx = Ship::Context::GetInstance();
	if (!ctx)
	{
		spdlog::error("lbReloc bridge: no Ship::Context");
		return nullptr;
	}

	// Deblobbed bundles are synthesized from their typed o2r slices
	// (docs/deblob.md). Parents ship with archive:false, so on synthesis
	// failure the LoadResource below misses and the stale-archive handler
	// prints its actionable delete-and-relaunch message — the synthesis
	// error above it names the exact slice and invariant.
	if (portGetSyntheticRelocSpec(file_id) != nullptr)
	{
		auto synthetic = portBuildSyntheticRelocResource(file_id);
		if (synthetic)
		{
			return synthetic;
		}
		spdlog::error("[deblob] synthesis failed for file_id {} — no archived parent"
		              " ships for deblobbed bundles; see the slice error above",
		              file_id);
	}

	std::string path(gRelocFileTable[file_id]);
	auto resource = ctx->GetResourceManager()->LoadResource(path);

	if (!resource)
	{
		/* Catch the "stale BattleShip.o2r" failure mode visibly before the
		 * caller dereferences this NULL and SIGSEGVs deep in portFixupSprite.
		 *
		 * When a user upgrades the port across an asset-format bump (e.g.
		 * factory version 0 vs 3), their cached BattleShip.o2r is silently
		 * incompatible: every RELO resource fails to load with "GetFactory
		 * failed to find an import factory" deep inside libultraship, but
		 * we only see NULL here. A healthy run loads thousands of reloc
		 * resources — the first one failing means the archive is unusable.
		 *
		 * Print an actionable message and exit cleanly rather than crash
		 * minutes later in a non-obvious place. */
		static bool sExitedOnStaleArchive = false;
		if (!sExitedOnStaleArchive)
		{
			sExitedOnStaleArchive = true;
			std::string dataDirO2r =
			    Ship::Context::GetPathRelativeToAppDirectory("BattleShip.o2r");
			fprintf(stderr,
			    "\n"
			    "============================================================\n"
			    "SSB64 PC port: failed to load asset from BattleShip.o2r\n"
			    "  resource: %s\n"
			    "  file_id:  %u\n"
			    "============================================================\n"
			    "\n"
			    "This almost always means your cached BattleShip.o2r was\n"
			    "packed by an older build whose resource format doesn't\n"
			    "match this binary's registered factory version.\n"
			    "\n"
			    "Fix: delete the stale archive and re-launch — the binary\n"
			    "will re-extract assets from your baserom on first run.\n"
			    "\n"
			    "    rm \"%s\"\n"
			    "\n"
			    "If that doesn't help, check ssb64.log for the underlying\n"
			    "ResourceLoader 'GetFactory failed' error which names the\n"
			    "exact Format/Version mismatch.\n"
			    "============================================================\n"
			    "\n",
			    path.c_str(), file_id, dataDirO2r.c_str());
			port_log("SSB64: STALE BattleShip.o2r detected — first reloc load failed "
			         "(path=%s file_id=%u). Delete '%s' and re-launch to re-extract.\n",
			         path.c_str(), file_id, dataDirO2r.c_str());
			std::fflush(stderr);
			/* _exit(0): skip C++ static destructors and spdlog flush so the
			 * Linux desktop doesn't show a "fatal error" popup. The exit
			 * code is intentionally 0 — the meaningful failure signal is
			 * the message we just printed; a non-zero code triggers
			 * crash-reporter UI on some desktops (Fedora/GNOME). Use POSIX
			 * _exit on POSIX, _Exit (stdlib) as a portable fallback. */
#if defined(__unix__) || defined(__APPLE__)
			_exit(0);
#else
			std::_Exit(0);
#endif
		}
		spdlog::error("lbReloc bridge: failed to load '{}' (file_id={})", path, file_id);
		return nullptr;
	}

	auto relocFile = std::dynamic_pointer_cast<RelocFile>(resource);
	if (!relocFile)
	{
		spdlog::error("lbReloc bridge: '{}' is not a RelocFile", path);
		return nullptr;
	}

	return relocFile;
}


// All game-facing functions have C linkage
extern "C" {

// // // // // // // // // // // //
//                               //
//      STATUS BUFFER FUNCS      //
//                               //
// // // // // // // // // // // //

void* lbRelocFindStatusBufferFile(u32 id)
{
	s32 i;

	if (sLBRelocInternBuffer.status_buffer_num == 0)
	{
		return NULL;
	}
	else for (i = 0; i < sLBRelocInternBuffer.status_buffer_num; i++)
	{
		if (id == sLBRelocInternBuffer.status_buffer[i].id)
		{
			return sLBRelocInternBuffer.status_buffer[i].addr;
		}
	}
	return NULL;
}

void* lbRelocGetStatusBufferFile(u32 id)
{
	return lbRelocFindStatusBufferFile(id);
}

void* lbRelocFindForceStatusBufferFile(u32 id)
{
	s32 i;

	if (sLBRelocInternBuffer.force_status_buffer_num != 0)
	{
		for (i = 0; i < sLBRelocInternBuffer.force_status_buffer_num; i++)
		{
			if (id == sLBRelocInternBuffer.force_status_buffer[i].id)
			{
				return sLBRelocInternBuffer.force_status_buffer[i].addr;
			}
		}
	}
	return lbRelocFindStatusBufferFile(id);
}

void* lbRelocGetForceStatusBufferFile(u32 id)
{
	return lbRelocFindForceStatusBufferFile(id);
}

void lbRelocAddStatusBufferFile(u32 id, void *addr)
{
	u32 num = sLBRelocInternBuffer.status_buffer_num;

	if (num >= (u32)sLBRelocInternBuffer.status_buffer_max)
	{
		while (TRUE)
		{
			syDebugPrintf("Relocatable Data Manager: Status Buffer is full !!\n");
			scManagerRunPrintGObjStatus();
		}
	}
	sLBRelocInternBuffer.status_buffer[num].id = id;
	sLBRelocInternBuffer.status_buffer[num].addr = addr;
	sLBRelocInternBuffer.status_buffer_num++;
}

void lbRelocAddForceStatusBufferFile(u32 id, void *addr)
{
	u32 num = sLBRelocInternBuffer.force_status_buffer_num;

	if (num >= (u32)sLBRelocInternBuffer.force_status_buffer_max)
	{
		while (TRUE)
		{
			syDebugPrintf("Relocatable Data Manager: Force Status Buffer is full !!\n");
			scManagerRunPrintGObjStatus();
		}
	}
	sLBRelocInternBuffer.force_status_buffer[num].id = id;
	sLBRelocInternBuffer.force_status_buffer[num].addr = addr;
	sLBRelocInternBuffer.force_status_buffer_num++;
}

} // extern "C"
