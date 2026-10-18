// Deblob synthesis engine — see SyntheticReloc.h and docs/deblob.md.
//
// Architecture (patch-list, relayout-ready from day one):
//   pass 0  load every slice resource; derive its ACTUAL byte length from
//           the parsed payload (never the o2r entry size); un-OTR display
//           lists into per-slice scratch words, recording every reference
//           as {word_index, target_slice, addend} instead of writing an
//           offset — resolution to a concrete pointer value is deferred.
//   layout  fast path: every actual size == spec vanilla size -> vanilla
//           offsets verbatim (I5: output equals the relocated view
//           byte-for-byte). Relayout path (mods, Phase 6): reassign.
//   pass 2  blit slices at their layout offsets; apply DL patches with the
//           TARGET slice's layout offset; emit explicit intern/extern slot
//           lists (RelocFile) in layout coordinates for the loader.
//
// Every failure cites its invariant (docs/deblob.md) and the diagnosis
// command. SSB64_SYNTH_INSPECT=<id> dumps the build's decisions as JSON
// shaped to diff against the generator manifest (the L2<->L5 gate).
#include "SyntheticReloc.h"
#include "synth_reloc_internal.h"

#include <cstring>
#include <algorithm>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include <ship/Context.h>
#include <ship/resource/ResourceManager.h>
#include <ship/resource/archive/ArchiveManager.h>
#include <ship/resource/type/Blob.h>
#include <ship/utils/StrHash64.h>
#include <fast/resource/type/DisplayList.h>
#include <fast/resource/type/Texture.h>
#include <fast/resource/type/Vertex.h>

int portSyntheticRelocSelfTest()
{
	uint32_t pass = 0, fail = 0;
	std::vector<std::pair<const char *, bool>> verdicts;
	verdicts.reserve(gSyntheticRelocSpecCount);

	for (uint32_t i = 0; i < gSyntheticRelocSpecCount; i++)
	{
		const auto &spec = gSyntheticRelocSpecs[i];
		bool ok = portBuildSyntheticRelocResource(spec.file_id) != nullptr;
		verdicts.emplace_back(spec.parent_path, ok);
		ok ? pass++ : fail++;
	}

	FILE *f = fopen("debug_traces/synth_verify_results.json", "wb");
	if (f == nullptr)
	{
		// debug_traces/ may not exist in a fresh working dir
		std::error_code ec;
		std::filesystem::create_directories("debug_traces", ec);
		f = fopen("debug_traces/synth_verify_results.json", "wb");
	}
	if (f != nullptr)
	{
		fprintf(f, "{\n \"pass\": %u,\n \"fail\": %u,\n \"files\": [\n", pass, fail);
		for (size_t i = 0; i < verdicts.size(); i++)
		{
			fprintf(f, "  {\"path\": \"%s\", \"ok\": %s}%s\n",
			        verdicts[i].first, verdicts[i].second ? "true" : "false",
			        (i + 1 < verdicts.size()) ? "," : "");
		}
		fprintf(f, " ]\n}\n");
		fclose(f);
	}

	spdlog::info("[deblob] synth_verify: {} pass, {} fail of {} specs",
	             pass, fail, gSyntheticRelocSpecCount);
	fprintf(stderr, "[deblob] synth_verify: %u pass, %u fail of %u specs\n",
	        pass, fail, gSyntheticRelocSpecCount);
	for (const auto &[path, ok] : verdicts)
	{
		if (!ok)
		{
			fprintf(stderr, "[deblob]   FAIL %s\n", path);
		}
	}
	return (int)fail;
}

extern "C" int port_synth_reloc_get_intern_pairs(uint32_t file_id,
                                                 const uint32_t **pairs_out,
                                                 uint32_t *pair_count_out)
{
	if (pairs_out == nullptr || pair_count_out == nullptr)
	{
		return 1;
	}
	// ensure the bundle is built so the layout is current
	if (!portBuildSyntheticRelocResource(file_id))
	{
		return 1;
	}
	std::lock_guard<std::mutex> lock(sSynthCacheMutex);
	auto it = sSynthCache.find(file_id);
	if (it == sSynthCache.end())
	{
		return 1;
	}
	*pairs_out = it->second.intern_pairs.data();
	*pair_count_out = (uint32_t)(it->second.intern_pairs.size() / 2);
	return 0;
}
