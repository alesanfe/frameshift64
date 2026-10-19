/* Internal shared declarations for the synthetic-reloc translation units —
 * the architecture overview lives in SyntheticReloc.cpp and docs/deblob.md.
 * Only the synth_reloc_*.cpp / SyntheticReloc.cpp files include this.
 *
 * Split layout:
 *   synth_unotr.cpp    spec lookup + display-list un-OTR (reference capture)
 *   synth_build.cpp    relayout build, CRC/BE writers, dumps, cache
 *   SyntheticReloc.cpp self-test + the extern "C" intern-pairs query
 */
#pragma once

#include "SyntheticReloc.h"
#include "RelocFile.h"
#include "RelocFileTable.h"
#include "ssb64_reloc_rebuild.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <ship/resource/archive/Archive.h>
#include <fast/resource/type/DisplayList.h>

constexpr uint8_t kOTRGSetTImgHash = 0x20;
constexpr uint8_t kOTRGDlHash = 0x31;
constexpr uint8_t kOTRGVtxHash = 0x32;
constexpr uint8_t kOTRGMarker = 0x33;
constexpr uint8_t kOTRGMoveMemHash = 0x42;

constexpr uint8_t kSeg0E = 0x0E;
constexpr int32_t kPatchLiteral = -1;

struct SynthDlPatch
{
	uint32_t word_index = 0;   // index into SynthDl::words (the w1 word)
	int32_t target_slice = 0;  // slice index, or kPatchLiteral
	uint32_t addend = 0;       // byte offset added to the target slice's layout offset
	bool seg0e = false;        // reconstruct with the 0x0E segment prefix
	uint32_t read_size = 1; // full referenced range, not just its first byte
};

struct SynthDl
{
	std::vector<uint32_t> words;   // real F3DEX2 words; patched w1s hold 0
	std::vector<SynthDlPatch> patches;
};

struct SynthCacheEntry
{
	std::shared_ptr<RelocFile> reloc;
	std::vector<uint32_t> layout_offsets;   // per-slice layout offsets
	std::vector<uint32_t> intern_pairs;     // flat (slot,target) pairs, layout coords
	std::vector<uint32_t> actual_sizes;
};

/* ---- shared state (defined in synth_build.cpp) ---- */
// Intentionally leaked: the cache holds Ship::IResource shared_ptrs, and
// running their destructors during static teardown (after spdlog's own
// statics are gone) SIGABRTs — same pattern PortShutdown documents for
// the context. Process exit reclaims the memory.
extern std::unordered_map<uint32_t, SynthCacheEntry> &sSynthCache;
extern std::mutex sSynthCacheMutex;
extern std::weak_ptr<Ship::Archive> sBaseArchive;

/* ---- synth_unotr.cpp ---- */
const SSB64SyntheticRelocSpec *portGetSyntheticRelocSpec(uint32_t file_id);
const SSB64RelocExternSlot *portFindExternalDescriptor(
    const SSB64SyntheticRelocSpec &spec, const char *path, uint32_t value);
int32_t portFindVanillaSlice(const SSB64SyntheticRelocSpec &spec, uint32_t vanilla_off);
bool portUnOtrDisplayList(const Fast::DisplayList &dl,
                          const SSB64SyntheticRelocSpec &spec,
                          const std::function<bool(uint64_t, int32_t &)> &resolveReference,
                          const char *slice_path, uint32_t vanilla_size,
                          bool vanilla,
                          SynthDl &out);
