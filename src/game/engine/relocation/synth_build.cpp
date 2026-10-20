// ---------------------------------------------------------------------------
// synth_build.cpp — relayout build: CRC/BE writers, dumpers, the synth
// cache, and portBuildSyntheticRelocResource. Split from SyntheticReloc.cpp.
// ---------------------------------------------------------------------------

#include "synth_reloc_internal.h"


// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------


// Intentionally leaked: the cache holds Ship::IResource shared_ptrs, and
// running their destructors during static teardown (after spdlog's own
// statics are gone) SIGABRTs — same pattern PortShutdown documents for
// the context. Process exit reclaims the memory.
auto &sSynthCache = *new std::unordered_map<uint32_t, SynthCacheEntry>();
std::mutex sSynthCacheMutex;
std::weak_ptr<Ship::Archive> sBaseArchive;

void portSyntheticRelocSetBaseArchive(std::shared_ptr<Ship::Archive> archive)
{
	std::lock_guard<std::mutex> lock(sSynthCacheMutex);
	sBaseArchive = archive;
	sSynthCache.clear();
}

void portSyntheticRelocEvictAll()
{
	std::lock_guard<std::mutex> lock(sSynthCacheMutex);
	sSynthCache.clear();
}

// CRC32-IEEE (matches zlib's crc32(), which the generator's zlib.crc32
// uses for spec.relocated_view_crc32). Self-contained to keep the ssb64
// link line free of a direct zlib dependency.
static uint32_t portCrc32(const uint8_t *data, size_t size)
{
	static uint32_t table[256];
	static std::once_flag once;
	std::call_once(once, [] {
		for (uint32_t i = 0; i < 256; i++)
		{
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
			{
				c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			}
			table[i] = c;
		}
	});
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < size; i++)
	{
		crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	}
	return crc ^ 0xFFFFFFFFu;
}

static void portWriteBE32(std::vector<uint8_t> &out, size_t offset, uint32_t value)
{
	out[offset + 0] = static_cast<uint8_t>((value >> 24) & 0xFF);
	out[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFF);
	out[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
	out[offset + 3] = static_cast<uint8_t>(value & 0xFF);
}

static void portWriteBE16(std::vector<uint8_t> &out, size_t offset, uint16_t value)
{
	out[offset + 0] = static_cast<uint8_t>((value >> 8) & 0xFF);
	out[offset + 1] = static_cast<uint8_t>(value & 0xFF);
}

static void portDumpSynthInspect(const SSB64SyntheticRelocSpec &spec,
                                 const SynthCacheEntry &entry,
                                 const std::vector<uint32_t> &actual_sizes,
                                 const std::vector<SynthDl> &dl_scratch)
{
	const char *inspect = getenv("SSB64_SYNTH_INSPECT");
	if (inspect == nullptr || strtoul(inspect, nullptr, 0) != spec.file_id)
	{
		return;
	}

	char path[256];
	snprintf(path, sizeof(path), "debug_traces/synth_inspect_%u.json", spec.file_id);
	FILE *f = fopen(path, "wb");
	if (f == nullptr)
	{
		return;
	}
	fprintf(f, "{\n \"file_id\": %u,\n \"parent_path\": \"%s\",\n \"data_size\": %zu,\n \"slices\": [\n",
	        spec.file_id, spec.parent_path, entry.reloc->Data.size());
	for (uint32_t i = 0; i < spec.slice_count; i++)
	{
		const auto &s = spec.slices[i];
		fprintf(f, "  {\"path\": \"%s\", \"vanilla_offset\": %u, \"vanilla_size\": %u,"
		           " \"layout_offset\": %u, \"actual_size\": %u}%s\n",
		        s.path, s.vanilla_offset, s.vanilla_size,
		        entry.layout_offsets[i], actual_sizes[i],
		        (i + 1 < spec.slice_count) ? "," : "");
	}
	fprintf(f, " ],\n \"dl_patches\": [\n");
	bool first = true;
	for (uint32_t i = 0; i < spec.slice_count; i++)
	{
		for (const auto &p : dl_scratch[i].patches)
		{
			fprintf(f, "%s  {\"dl\": \"%s\", \"word\": %u, \"target_slice\": %d,"
			           " \"addend\": %u, \"seg0e\": %s}",
			        first ? "" : ",\n", spec.slices[i].path, p.word_index,
			        p.target_slice, p.addend, p.seg0e ? "true" : "false");
			first = false;
		}
	}
	fprintf(f, "\n ],\n \"intern_slots\": [");
	first = true;
	for (const auto &slot : entry.reloc->ExplicitInternSlots)
	{
		fprintf(f, "%s{\"slot\": %u, \"target\": %u}", first ? "" : ",",
		        slot.SlotByteOff, slot.TargetByteOff);
		first = false;
	}
	fprintf(f, "],\n \"extern_slots\": [");
	first = true;
	for (const auto &slot : entry.reloc->ExplicitExternSlots)
	{
		fprintf(f, "%s{\"slot\": %u, \"file\": %u, \"word\": %u}", first ? "" : ",",
		        slot.SlotByteOff, slot.DepFileId, slot.DepWordOff);
		first = false;
	}
	fprintf(f, "],\n \"intern_slot_count\": %zu,\n \"extern_slot_count\": %zu\n}\n",
	        entry.reloc->ExplicitInternSlots.size(), entry.reloc->ExplicitExternSlots.size());
	fclose(f);
	spdlog::info("[deblob] wrote synthesis inspect JSON to {}", path);
}

static void portDumpSynthBytesIfRequested(const SSB64SyntheticRelocSpec &spec, const RelocFile &relocFile)
{
	const char *dumpAll = getenv("SSB64_DUMP_SYNTH_RELOC");
	const char *dumpOne = getenv("SSB64_DUMP_SYNTH_RELOC_FILE_ID");
	bool shouldDump = (dumpAll != nullptr && dumpAll[0] == '1');
	if (dumpOne != nullptr && strtoul(dumpOne, nullptr, 0) == spec.file_id)
	{
		shouldDump = true;
	}
	if (!shouldDump)
	{
		return;
	}
	char path[256];
	snprintf(path, sizeof(path), "debug_traces/synth_reloc_%u.bin", spec.file_id);
	FILE *df = fopen(path, "wb");
	if (df != nullptr)
	{
		fwrite(relocFile.Data.data(), 1, relocFile.Data.size(), df);
		fclose(df);
		spdlog::info("[deblob] wrote synthesized bundle for '{}' to {}", spec.parent_path, path);
	}
}

// Remap a vanilla u32-word offset into dependency file `dep_file_id` to
// that file's CURRENT layout. Identity unless the dep is a deblobbed
// bundle that relayouted (a mod resized one of its slices) — then the
// vanilla offset is located in the dep's vanilla slice map and re-based
// on the slice's new layout offset. Builds the dep first so its layout
// exists (fighter dep graphs are acyclic: Main -> Model -> extern banks).
static thread_local std::vector<uint32_t> sSynthBuildStack;

static bool portRemapDepWordOffset(uint16_t dep_file_id, uint32_t dep_word_off, uint32_t &remapped)
{
	remapped = dep_word_off;
	const auto *depSpec = portGetSyntheticRelocSpec(dep_file_id);
	if (depSpec == nullptr)
	{
		return true;
	}
	for (uint32_t in_progress : sSynthBuildStack)
	{
		if (in_progress == dep_file_id)
		{
			spdlog::error("[deblob] cyclic bundle dependency at file_id {}", dep_file_id);
			return false;
		}
	}
	if (!portBuildSyntheticRelocResource(dep_file_id))
	{
		spdlog::error("[deblob] dependency file_id {} failed synthesis", dep_file_id);
		return false;
	}
	std::lock_guard<std::mutex> lock(sSynthCacheMutex);
	auto it = sSynthCache.find(dep_file_id);
	if (it == sSynthCache.end())
	{
		return false;
	}
	const uint32_t vanilla_byte = dep_word_off * 4;
	int32_t slice = portFindVanillaSlice(*depSpec, vanilla_byte);
	if (slice == kPatchLiteral)
	{
		spdlog::error("[deblob] dependency reference has no slice in file_id {}", dep_file_id);
		return false;
	}
	const uint32_t delta = vanilla_byte - depSpec->slices[slice].vanilla_offset;
	if (delta >= it->second.actual_sizes[slice])
	{
		spdlog::error("[deblob] dependency reference +0x{:X} exceeds replacement '{}'", delta, depSpec->slices[slice].path);
		return false;
	}
	remapped = (it->second.layout_offsets[slice] + delta) / 4;
	return true;
}

// Build state shared across the passes of portBuildSyntheticRelocResource.
// The builder was split into per-phase helpers over this context without
// changing any arithmetic, logging, or early-exit order.
struct SynthBuildCtx
{
	explicit SynthBuildCtx(const SSB64SyntheticRelocSpec &base)
	    : baseSpec(&base), spec(base),
	      slices(base.slices, base.slices + base.slice_count),
	      resources(base.slice_count), actual_sizes(base.slice_count, 0),
	      dl_scratch(base.slice_count), overridden(base.slice_count, false)
	{
		// Extend the original slices with the transitive closure of new mod
		// assets. These have no vanilla address and are packed after the base.
		spec.slices = slices.data();
		// hash -> slice index map for DL reference resolution
		hashToSlice.reserve(spec.slice_count);
		for (uint32_t i = 0; i < spec.slice_count; i++)
		{
			hashToSlice.emplace(CRC64(spec.slices[i].path), i);
		}
	}

	const SSB64SyntheticRelocSpec *baseSpec;
	SSB64SyntheticRelocSpec spec;
	std::vector<SSB64SyntheticSlice> slices;
	std::deque<std::string> extra_paths; // c_str pointers survive append
	std::unordered_map<uint64_t, uint32_t> hashToSlice;
	std::vector<std::shared_ptr<Ship::IResource>> resources;
	std::vector<uint32_t> actual_sizes;
	std::vector<SynthDl> dl_scratch;
	std::vector<bool> overridden;
	bool any_override = false;
	bool relayout = false;
	SynthCacheEntry entry;
	std::shared_ptr<Ship::ResourceManager> rm;
	std::shared_ptr<Ship::ArchiveManager> am;
	std::shared_ptr<Ship::Archive> baseArchive;
};

// Recursion guard for cross-file remaps (portRemapDepWordOffset may
// build a dependency bundle mid-build).
struct BuildStackGuard
{
	BuildStackGuard(uint32_t id) { sSynthBuildStack.push_back(id); }
	~BuildStackGuard() { sSynthBuildStack.pop_back(); }
};

static bool SynthResolveReference(SynthBuildCtx &B, uint64_t hash, int32_t &target)
{
	const auto found = B.hashToSlice.find(hash);
	if (found != B.hashToSlice.end())
	{
		target = static_cast<int32_t>(found->second);
		return true;
	}
	const auto path = B.am->HashToString(hash);
	if (!path || B.slices.size() >= B.baseSpec->slice_count + 4096) return false;
	std::shared_ptr<Ship::IResource> resource;
	try { resource = B.rm->LoadResource(*path); }
	catch (const std::exception &e)
	{
		spdlog::error("[deblob] new asset '{}' failed to parse: {}", *path, e.what());
		return false;
	}
	SSB64SyntheticSliceKind kind;
	if (std::dynamic_pointer_cast<Fast::DisplayList>(resource)) kind = SSB64SyntheticSliceKind::DisplayList;
	else if (std::dynamic_pointer_cast<Fast::Vertex>(resource)) kind = SSB64SyntheticSliceKind::Vertex;
	else if (std::dynamic_pointer_cast<Fast::Texture>(resource)) kind = SSB64SyntheticSliceKind::Texture;
	else if (std::dynamic_pointer_cast<Ship::Blob>(resource)) kind = SSB64SyntheticSliceKind::Blob;
	else return false;
	target = static_cast<int32_t>(B.slices.size());
	B.extra_paths.push_back(*path);
	B.slices.push_back({B.extra_paths.back().c_str(), B.spec.vanilla_data_size, 0, kind});
	B.spec.slices = B.slices.data();
	B.spec.slice_count = static_cast<uint32_t>(B.slices.size());
	B.hashToSlice.emplace(hash, target);
	B.resources.push_back(resource);
	B.actual_sizes.push_back(0);
	B.dl_scratch.emplace_back();
	B.overridden.push_back(true);
	B.any_override = true;
	return true;
}

// Derive the byte size of one loaded slice resource and, for display
// lists, the un-OTR'd scratch program. Fails on a kind/resource mismatch.
static bool SynthMeasureSlice(SynthBuildCtx &B, uint32_t i,
                              const SSB64SyntheticSlice &slice,
                              const std::shared_ptr<Ship::IResource> &resource)
{
	switch (slice.kind)
	{
	case SSB64SyntheticSliceKind::Blob:
	{
		auto blob = std::dynamic_pointer_cast<Ship::Blob>(resource);
		if (!blob)
		{
			spdlog::error("[deblob] slice '{}' is not a Blob", slice.path);
			return false;
		}
		// LUS's Blob factory appends kBlobPadding (16) zero bytes for
		// N64 overread tolerance (ship BlobFactory.cpp) — the payload
		// is Data.size() minus that pad.
		constexpr uint32_t kLusBlobPadding = 16;
		B.actual_sizes[i] = (uint32_t)blob->Data.size() >= kLusBlobPadding
		                        ? (uint32_t)blob->Data.size() - kLusBlobPadding
		                        : (uint32_t)blob->Data.size();
		break;
	}
	case SSB64SyntheticSliceKind::Texture:
	{
		auto texture = std::dynamic_pointer_cast<Fast::Texture>(resource);
		if (!texture)
		{
			spdlog::error("[deblob] slice '{}' is not a Texture", slice.path);
			return false;
		}
		B.actual_sizes[i] = texture->ImageDataSize;
		break;
	}
	case SSB64SyntheticSliceKind::Vertex:
	{
		auto vertex = std::dynamic_pointer_cast<Fast::Vertex>(resource);
		if (!vertex)
		{
			spdlog::error("[deblob] slice '{}' is not a Vertex", slice.path);
			return false;
		}
		B.actual_sizes[i] = (uint32_t)(vertex->VertexList.size() * sizeof(Vtx));
		break;
	}
	case SSB64SyntheticSliceKind::DisplayList:
	{
		auto dl = std::dynamic_pointer_cast<Fast::DisplayList>(resource);
		if (!dl)
		{
			spdlog::error("[deblob] slice '{}' is not a DisplayList", slice.path);
			return false;
		}
		SynthDl scratch;
		auto resolveReference = [&B](uint64_t hash, int32_t &target) -> bool {
			return SynthResolveReference(B, hash, target);
		};
		if (!portUnOtrDisplayList(*dl, B.spec, resolveReference, slice.path,
		                          slice.vanilla_size, !B.overridden[i], scratch))
		{
			return false;
		}
		B.dl_scratch[i] = std::move(scratch);
		B.actual_sizes[i] = (uint32_t)(B.dl_scratch[i].words.size() * sizeof(uint32_t));
		break;
	}
	}
	return true;
}

// --- pass 0: load slices, derive actual sizes, un-OTR display lists ---
static bool SynthLoadSlices(SynthBuildCtx &B)
{
	for (uint32_t i = 0; i < B.spec.slice_count; i++)
	{
		const auto slice = B.spec.slices[i];
		std::shared_ptr<Ship::IResource> resource = B.resources[i];
		try
		{
			if (!resource) resource = B.rm->LoadResource(slice.path);
		}
		catch (const std::exception &e)
		{
			// A factory parse failure surfaces here through the async
			// loader's future; one bad slice must degrade this bundle,
			// never kill the process.
			spdlog::error("[deblob] slice '{}' failed to parse: {} (parent '{}')",
			              slice.path, e.what(), B.spec.parent_path);
			return false;
		}
		if (!resource)
		{
			spdlog::error("[deblob] missing slice '{}' for parent '{}'"
			              " — diagnose: unzip -l BattleShip.o2r | grep {}",
			              slice.path, B.spec.parent_path, B.spec.parent_path);
			return false;
		}
		B.resources[i] = resource;
		// Pinned LUS does not populate ResourceInitData::Parent. Resolve the
		// actual factory path (including alt/ and .meta redirects) in the VFS.
		const auto init = resource->GetInitData();
		const std::string resource_path = init ? init->Path : slice.path;
		const auto meta_archive = B.am->GetArchiveFromFile(std::string(slice.path) + ".meta");
		B.overridden[i] = i >= B.baseSpec->slice_count || B.am->GetArchiveFromFile(resource_path) != B.baseArchive ||
		                  (meta_archive && meta_archive != B.baseArchive);
		B.any_override = B.any_override || B.overridden[i];

		if (!SynthMeasureSlice(B, i, slice, resource))
		{
			return false;
		}
	}
	return true;
}

// --- layout ---
// Fast path: every actual size matches the vanilla layout -> vanilla
// offsets verbatim (I5 byte-exact). Otherwise RELAYOUT: keep vanilla
// offsets up to the first resized slice, then pack forward with
// 16-byte alignment (satisfies Gfx=8, Vtx=16, TMEM DMA sources, and
// word-granular chain targets). Determinism note: vanilla inter-slice
// padding before the first resize is preserved so the forced-relayout
// gate (I6) can compare against the fast path.
static uint32_t SynthComputeLayout(SynthBuildCtx &B)
{
	B.entry.layout_offsets.resize(B.spec.slice_count);
	B.entry.actual_sizes = B.actual_sizes;
	uint32_t cursor = 0;
	for (uint32_t i = 0; i < B.spec.slice_count; i++)
	{
		if (!B.relayout)
		{
			B.entry.layout_offsets[i] = B.spec.slices[i].vanilla_offset;
			if (B.actual_sizes[i] != B.spec.slices[i].vanilla_size)
			{
				B.relayout = true;
				spdlog::info("[deblob] relayout: '{}' size 0x{:X} != vanilla 0x{:X} — "
				             "repacking '{}' from slice {}",
				             B.spec.slices[i].path, B.actual_sizes[i],
				             B.spec.slices[i].vanilla_size, B.spec.parent_path, i);
			}
			cursor = B.entry.layout_offsets[i] + B.actual_sizes[i];
		}
		else
		{
			cursor = (cursor + 15u) & ~15u;
			B.entry.layout_offsets[i] = cursor;
			cursor += B.actual_sizes[i];
		}
	}
	return B.relayout ? ((cursor + 7u) & ~7u)
	                  : B.spec.vanilla_data_size;
}

// I9 structural policy for resized slices: a reference or slot that
// lands at delta > 0 inside a slice is only meaningful when the
// replacement preserved that prefix — allow with a warning while the
// delta still fits, fail loudly when it does not.
static bool SynthCheckDelta(const SynthBuildCtx &B, uint32_t slice_idx, uint32_t delta,
                            const char *what, uint32_t width = 1)
{
	if (delta < B.actual_sizes[slice_idx] && width <= B.actual_sizes[slice_idx] - delta)
	{
		if (B.relayout && delta != 0 && B.spec.slices[slice_idx].vanilla_size != 0 &&
		    B.actual_sizes[slice_idx] != B.spec.slices[slice_idx].vanilla_size)
		{
			spdlog::warn("[deblob] {} at +0x{:X} into resized slice '{}' — "
			             "only valid for prefix-preserving replacements",
			             what, delta, B.spec.slices[slice_idx].path);
		}
		return true;
	}
	spdlog::error("[deblob] I9 violated: {} at +0x{:X} exceeds resized slice"
	              " '{}' (new size 0x{:X}) — diagnose: SSB64_SYNTH_INSPECT={}",
	              what, delta, B.spec.slices[slice_idx].path,
	              B.actual_sizes[slice_idx], B.spec.file_id);
	return false;
}

// Emit one slice's payload into the packed image at its layout offset.
static void SynthBlitSlice(SynthBuildCtx &B, RelocFile &out, uint32_t i,
                         const SSB64SyntheticSlice &slice)
{
	const uint32_t off = B.entry.layout_offsets[i];
	switch (slice.kind)
	{
	case SSB64SyntheticSliceKind::Blob:
	{
		auto blob = std::static_pointer_cast<Ship::Blob>(B.resources[i]);
		std::memcpy(out.Data.data() + off, blob->Data.data(), B.actual_sizes[i]);
		break;
	}
	case SSB64SyntheticSliceKind::Texture:
	{
		auto texture = std::static_pointer_cast<Fast::Texture>(B.resources[i]);
		std::memcpy(out.Data.data() + off, texture->ImageData, B.actual_sizes[i]);
		break;
	}
	case SSB64SyntheticSliceKind::Vertex:
	{
		auto vertex = std::static_pointer_cast<Fast::Vertex>(B.resources[i]);
		size_t writeCursor = off;
		for (const auto &v : vertex->VertexList)
		{
			portWriteBE16(out.Data, writeCursor + 0x0, (uint16_t)v.v.ob[0]);
			portWriteBE16(out.Data, writeCursor + 0x2, (uint16_t)v.v.ob[1]);
			portWriteBE16(out.Data, writeCursor + 0x4, (uint16_t)v.v.ob[2]);
			portWriteBE16(out.Data, writeCursor + 0x6, v.v.flag);
			portWriteBE16(out.Data, writeCursor + 0x8, (uint16_t)v.v.tc[0]);
			portWriteBE16(out.Data, writeCursor + 0xA, (uint16_t)v.v.tc[1]);
			out.Data[writeCursor + 0xC] = v.v.cn[0];
			out.Data[writeCursor + 0xD] = v.v.cn[1];
			out.Data[writeCursor + 0xE] = v.v.cn[2];
			out.Data[writeCursor + 0xF] = v.v.cn[3];
			writeCursor += sizeof(Vtx);
		}
		break;
	}
	case SSB64SyntheticSliceKind::DisplayList:
		break; // handled by SynthPatchAndBlitDisplayList
	}
}

// apply patches: target slice layout offset (+ addend), with
// the seg-0x0E prefix restored for literal segment refs
static bool SynthPatchDisplayList(SynthBuildCtx &B, uint32_t i)
{
	auto &scratch = B.dl_scratch[i];
	const auto &slice = B.spec.slices[i];
	for (const auto &p : scratch.patches)
	{
		const uint8_t opcode = scratch.words[p.word_index - 1] >> 24;
		const auto kind = B.spec.slices[p.target_slice].kind;
		if (B.overridden[i] &&
		    ((opcode == 0x01 && kind != SSB64SyntheticSliceKind::Vertex) ||
		     (opcode == 0xDE && kind != SSB64SyntheticSliceKind::DisplayList) ||
		     (opcode == 0xFD && kind != SSB64SyntheticSliceKind::Texture && kind != SSB64SyntheticSliceKind::Blob)))
		{
			spdlog::error("[deblob] wrong resource type for command 0x{:02X} in '{}'", opcode, slice.path);
			return false;
		}
		if (!SynthCheckDelta(B, (uint32_t)p.target_slice, p.addend, "DL reference",
		                     opcode == 0xDE ? 8 : p.read_size))
		{
			return false;
		}
		uint32_t value = B.entry.layout_offsets[p.target_slice] + p.addend;
		if (p.seg0e)
		{
			value = ((uint32_t)kSeg0E << 24) | (value & 0x00FFFFFF);
		}
		scratch.words[p.word_index] = value;
	}
	return true;
}

// --- pass 2: blit + patch ---
static bool SynthBlitAndPatch(SynthBuildCtx &B, RelocFile &out)
{
	for (uint32_t i = 0; i < B.spec.slice_count; i++)
	{
		const auto &slice = B.spec.slices[i];
		if (slice.kind == SSB64SyntheticSliceKind::DisplayList)
		{
			auto &scratch = B.dl_scratch[i];
			if (!SynthPatchDisplayList(B, i))
			{
				return false;
			}
			const uint32_t off = B.entry.layout_offsets[i];
			for (size_t w = 0; w < scratch.words.size(); w++)
			{
				portWriteBE32(out.Data, off + w * sizeof(uint32_t), scratch.words[w]);
			}
		}
		else
		{
			SynthBlitSlice(B, out, i, slice);
		}
	}
	return true;
}

// A named DL can reference another new DL. Reject cycles before those
// references become an endless renderer call/branch chain.
static bool SynthValidateDlAcyclic(const SynthBuildCtx &B)
{
	std::vector<uint8_t> visiting(B.spec.slice_count, 0);
	std::function<bool(uint32_t)> visit = [&](uint32_t i) {
		if (visiting[i] == 1) return false;
		if (visiting[i] == 2) return true;
		visiting[i] = 1;
		for (const auto &p : B.dl_scratch[i].patches)
		{
			if (!p.seg0e && B.dl_scratch[i].words[p.word_index - 1] >> 24 == 0xDE &&
			    !visit(static_cast<uint32_t>(p.target_slice))) return false;
		}
		visiting[i] = 2;
		return true;
	};
	for (uint32_t i = 0; i < B.spec.slice_count; i++)
	{
		if (!visit(i))
		{
			spdlog::error("[deblob] cyclic display-list references in '{}'", B.spec.parent_path);
			return false;
		}
	}
	return true;
}

// explicit slot lists in layout coordinates
static bool SynthEmitInternSlots(SynthBuildCtx &B, RelocFile &out)
{
	out.ExplicitInternSlots.reserve(B.spec.intern_slot_count);
	for (uint32_t i = 0; i < B.spec.intern_slot_count; i++)
	{
		const auto &s = B.spec.intern_slots[i];
		// A replacement DL owns its command stream. Vanilla slot positions
		// describe the old program and must never be applied to the new one.
		if (B.overridden[s.slot_slice] &&
		    B.spec.slices[s.slot_slice].kind == SSB64SyntheticSliceKind::DisplayList) continue;
		if (!SynthCheckDelta(B, s.slot_slice, s.slot_offset_in_slice, "intern slot position", 4) ||
		    !SynthCheckDelta(B, s.target_slice, s.target_offset_in_slice, "intern slot target"))
		{
			return false;
		}
		RelocExplicitInternSlot slot;
		slot.SlotByteOff = B.entry.layout_offsets[s.slot_slice] + s.slot_offset_in_slice;
		slot.TargetByteOff = B.entry.layout_offsets[s.target_slice] + s.target_offset_in_slice;
		out.ExplicitInternSlots.push_back(slot);
		B.entry.intern_pairs.push_back(slot.SlotByteOff);
		B.entry.intern_pairs.push_back(slot.TargetByteOff);
	}
	for (uint32_t i = 0; i < B.spec.slice_count; i++)
	{
		if (!B.overridden[i]) continue;
		for (const auto &p : B.dl_scratch[i].patches)
		{
			if (p.seg0e) continue; // segmented references are resolved by the renderer
			RelocExplicitInternSlot slot{
				B.entry.layout_offsets[i] + p.word_index * 4,
				B.entry.layout_offsets[p.target_slice] + p.addend};
			out.ExplicitInternSlots.push_back(slot);
			B.entry.intern_pairs.push_back(slot.SlotByteOff);
			B.entry.intern_pairs.push_back(slot.TargetByteOff);
		}
	}
	return true;
}

static bool SynthEmitExternSlots(SynthBuildCtx &B, RelocFile &out)
{
	out.ExplicitExternSlots.reserve(B.spec.extern_slot_count);
	out.ExternFileIds.reserve(B.spec.extern_slot_count);
	for (uint32_t i = 0; i < B.spec.extern_slot_count; i++)
	{
		const auto &s = B.spec.extern_slots[i];
		if (B.overridden[s.slot_slice] &&
		    B.spec.slices[s.slot_slice].kind == SSB64SyntheticSliceKind::DisplayList) continue;
		if (!SynthCheckDelta(B, s.slot_slice, s.slot_offset_in_slice, "extern slot position", 4))
		{
			return false;
		}
		RelocExplicitExternSlot slot;
		slot.SlotByteOff = B.entry.layout_offsets[s.slot_slice] + s.slot_offset_in_slice;
		slot.DepFileId = s.dep_file_id;
		// Cross-file fidelity: a dependency that is itself a relayouted
		// deblobbed bundle moved its contents — remap the vanilla word
		// offset through the dep's layout. Identity for unmodded deps.
		if (!portRemapDepWordOffset(s.dep_file_id, s.dep_word_offset, slot.DepWordOff)) return false;
		out.ExplicitExternSlots.push_back(slot);
		// chain order == extern id order: lbRelocGetExternBytesNum walks
		// this list to predict dependency allocation sizes
		out.ExternFileIds.push_back(s.dep_file_id);
	}
	return true;
}

// Overridden DLs can address extern-file words through unpatchable
// segmented encodings; each becomes an explicit extern slot.
static bool SynthEmitDlExternSlots(SynthBuildCtx &B, RelocFile &out)
{
	for (uint32_t i = 0; i < B.spec.slice_count; i++)
	{
		if (!B.overridden[i] || B.spec.slices[i].kind != SSB64SyntheticSliceKind::DisplayList) continue;
		const auto &scratch = B.dl_scratch[i];
		for (uint32_t w = 1; w < scratch.words.size(); w += 2)
		{
			const uint32_t op = scratch.words[w - 1] >> 24;
			if (op != 0x01 && op != 0xDE && op != 0xFD && op != 0xDC) continue;
			if (std::any_of(scratch.patches.begin(), scratch.patches.end(),
			                [w](const auto &p) { return p.word_index == w; })) continue;
			const auto *external = portFindExternalDescriptor(B.spec, B.spec.slices[i].path, scratch.words[w]);
			if (!external) continue; // validated segmented references need no relocation
			RelocExplicitExternSlot slot;
			slot.SlotByteOff = B.entry.layout_offsets[i] + w * 4;
			slot.DepFileId = external->dep_file_id;
			if (!portRemapDepWordOffset(slot.DepFileId, external->dep_word_offset, slot.DepWordOff)) return false;
			out.ExplicitExternSlots.push_back(slot);
			out.ExternFileIds.push_back(slot.DepFileId);
		}
	}
	return true;
}

// I5 applies to resources from the extracted archive. An override can
// legitimately change content while preserving every size and offset.
static std::shared_ptr<RelocFile> SynthFinalize(SynthBuildCtx &B,
                                                const std::shared_ptr<RelocFile> &relocFile,
                                                uint32_t file_id, uint32_t data_size)
{
	B.entry.reloc = relocFile;
	portDumpSynthInspect(B.spec, B.entry, B.actual_sizes, B.dl_scratch);
	portDumpSynthBytesIfRequested(B.spec, *relocFile);

	if (!B.any_override)
	{
		// Overridden bundles are validated structurally above.
		uint32_t crc = portCrc32(relocFile->Data.data(), relocFile->Data.size());
		if (crc != B.spec.relocated_view_crc32)
		{
			// Wrong bytes corrupt rendering and gameplay downstream; fail
			// the build so the caller's fallback (archived parent) runs.
			spdlog::error("[deblob] I5 violated: synthesized '{}' crc 0x{:08X} != spec 0x{:08X}"
			              " — diagnose: SSB64_DUMP_SYNTH_RELOC_FILE_ID={} then compare with"
			              " tools/generate_fighter_slices.py --only <Symbol> --check",
			              B.spec.parent_path, crc, B.spec.relocated_view_crc32, B.spec.file_id);
			return nullptr;
		}
	}

	{
		std::lock_guard<std::mutex> lock(sSynthCacheMutex);
		sSynthCache.emplace(file_id, std::move(B.entry));
	}
	spdlog::info("[deblob] synthesized '{}' from {} slices ({} bytes{})",
	             B.spec.parent_path, B.spec.slice_count, data_size,
	             B.any_override ? ", override" : ", crc ok");
	return relocFile;
}

std::shared_ptr<RelocFile> portBuildSyntheticRelocResource(uint32_t file_id)
{
	const auto *specPtr = portGetSyntheticRelocSpec(file_id);
	if (specPtr == nullptr)
	{
		return nullptr;
	}
	{
		std::lock_guard<std::mutex> lock(sSynthCacheMutex);
		auto it = sSynthCache.find(file_id);
		if (it != sSynthCache.end())
		{
			return it->second.reloc;
		}
	}

	auto ctx = Ship::Context::GetInstance();
	if (!ctx)
	{
		spdlog::error("[deblob] no Ship::Context for synthetic file_id {}", file_id);
		return nullptr;
	}

	SynthBuildCtx B(*specPtr);
	B.rm = ctx->GetResourceManager();
	B.am = B.rm->GetArchiveManager();
	B.baseArchive = sBaseArchive.lock();
	if (!B.baseArchive)
	{
		spdlog::error("[deblob] base archive provenance was not registered");
		return nullptr;
	}
	BuildStackGuard build_guard(file_id);

	// --- pass 0: load slices, derive actual sizes, un-OTR display lists ---
	if (!SynthLoadSlices(B))
	{
		return nullptr;
	}
	// Bound the packed image before offset arithmetic or allocation. The
	// original game uses 24-bit segmented addresses for file-local data.
	uint64_t total_size = B.spec.vanilla_data_size;
	for (uint32_t size : B.actual_sizes) total_size += static_cast<uint64_t>(size) + 16;
	if (total_size > 0x01000000)
	{
		spdlog::error("[deblob] bundle '{}' exceeds the 24-bit address space", B.spec.parent_path);
		return nullptr;
	}

	const uint32_t data_size = SynthComputeLayout(B);

	// --- pass 2: blit + patch ---
	auto relocFile = std::make_shared<RelocFile>(std::shared_ptr<Ship::ResourceInitData>());
	relocFile->FileId = B.spec.file_id;
	relocFile->RelocInternOffset = 0xFFFF;  // chains are gone; explicit lists only
	relocFile->RelocExternOffset = 0xFFFF;
	relocFile->Data.assign(data_size, 0);
	relocFile->HasExplicitRelocation = true;

	if (!SynthBlitAndPatch(B, *relocFile) ||
	    (B.any_override && !SynthValidateDlAcyclic(B)) ||
	    !SynthEmitInternSlots(B, *relocFile) ||
	    !SynthEmitExternSlots(B, *relocFile) ||
	    !SynthEmitDlExternSlots(B, *relocFile))
	{
		return nullptr;
	}

	return SynthFinalize(B, relocFile, file_id, data_size);
}
