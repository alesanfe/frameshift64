// ---------------------------------------------------------------------------
// synth_unotr.cpp — spec lookup + display-list un-OTR (reference capture).
// Split from SyntheticReloc.cpp; the architecture overview lives there.
// ---------------------------------------------------------------------------

#include "synth_reloc_internal.h"

// Spec lookup: direct-index table over the generated file_id-sorted array.
// ---------------------------------------------------------------------------

static const SSB64SyntheticRelocSpec **sSpecByFileId = nullptr;
static std::once_flag sSpecTableOnce;

const SSB64SyntheticRelocSpec *portGetSyntheticRelocSpec(uint32_t file_id)
{
	std::call_once(sSpecTableOnce, [] {
		sSpecByFileId = new const SSB64SyntheticRelocSpec *[RELOC_FILE_COUNT]();
		for (uint32_t i = 0; i < gSyntheticRelocSpecCount; i++)
		{
			const auto &spec = gSyntheticRelocSpecs[i];
			if (spec.file_id < RELOC_FILE_COUNT)
			{
				sSpecByFileId[spec.file_id] = &spec;
			}
		}
	});
	if (file_id >= RELOC_FILE_COUNT)
	{
		return nullptr;
	}
	return sSpecByFileId[file_id];
}

// ---------------------------------------------------------------------------
// OTR display-list decoding (pass 0)
// ---------------------------------------------------------------------------

// Torch's binary DL exporter replaces pointer-carrying commands with OTR
// pseudo-opcodes followed by one CRC64-of-path word pair. An "injected"
// pair (both words equal to the command's own) marks an unresolved literal
// — the original word is preserved verbatim (this is how raw seg-0x0E refs
// and extern-slot descriptor words survive byte-exactly).
static uint64_t portReadHash64(const Gfx &cmd)
{
	return (static_cast<uint64_t>(static_cast<uint32_t>(cmd.words.w0)) << 32) |
	       static_cast<uint32_t>(cmd.words.w1);
}

// Torch's unresolved-reference encoding (DisplayListFactory export): the
// OTR pseudo-command is followed by the ORIGINAL command pair verbatim
// (its top byte is the real opcode — 0xFD/0xDE/0xDC), not a CRC64 hash.
// A real hash's top byte colliding with the expected opcode is the
// residual I7 hazard documented in docs/deblob.md.
static bool portIsLiteralPair(const Gfx &pair, uint8_t real_opcode)
{
	return static_cast<uint8_t>(static_cast<uint32_t>(pair.words.w0) >> 24) == real_opcode;
}

// PR #271: a binary mod may move a command while retaining its exact
// external-chain descriptor. Recover its identity, never its old slot.
// Match before interpreting segment prefixes: a chain-next word can start
// with 0x0E too. Ambiguous descriptors are rejected.
const SSB64RelocExternSlot *portFindExternalDescriptor(
    const SSB64SyntheticRelocSpec &spec, const char *path, uint32_t value)
{
	const SSB64RelocExternSlot *match = nullptr;
	for (uint32_t e = 0; e < spec.extern_slot_count; e++)
	{
		const auto &slot = spec.extern_slots[e];
		if (std::strcmp(spec.slices[slot.slot_slice].path, path) != 0) continue;
		uint32_t next = 0xFFFF;
		if (e + 1 < spec.extern_slot_count)
		{
			const auto &n = spec.extern_slots[e + 1];
			next = (spec.slices[n.slot_slice].vanilla_offset + n.slot_offset_in_slice) / 4;
		}
		if (value != ((next << 16) | slot.dep_word_offset)) continue;
		if (match && (match->dep_file_id != slot.dep_file_id || match->dep_word_offset != slot.dep_word_offset))
			return nullptr;
		match = &slot;
	}
	return match;
}

// Map a vanilla byte offset to its containing slice (binary search over the
// spec's sorted vanilla layout). Used to make literal seg-0x0E references
// relayout-safe: they become slice+delta patches instead of raw offsets.
int32_t portFindVanillaSlice(const SSB64SyntheticRelocSpec &spec, uint32_t vanilla_off)
{
	uint32_t lo = 0, hi = spec.slice_count;
	while (lo < hi)
	{
		uint32_t mid = (lo + hi) / 2;
		const auto &s = spec.slices[mid];
		if (vanilla_off < s.vanilla_offset)
		{
			hi = mid;
		}
		else if (vanilla_off >= s.vanilla_offset + s.vanilla_size)
		{
			lo = mid + 1;
		}
		else
		{
			return (int32_t)mid;
		}
	}
	return kPatchLiteral;
}

// Push a literal (unresolved-at-extraction) w1. Seg-0x0E words become
// slice+delta patches so relayout can remap them; everything else (extern
// descriptors, plain literals) passes through untouched.
static void portPushLiteralRef(const SSB64SyntheticRelocSpec &spec, SynthDl &out, uint32_t w1)
{
	if ((w1 >> 24) == kSeg0E)
	{
		int32_t slice = portFindVanillaSlice(spec, w1 & 0x00FFFFFF);
		if (slice != kPatchLiteral)
		{
			out.patches.push_back({(uint32_t)out.words.size(), slice,
			                       (w1 & 0x00FFFFFF) - spec.slices[slice].vanilla_offset,
			                       /* seg0e */ true});
			out.words.push_back(0);
			return;
		}
	}
	out.words.push_back(w1);
}

// Per-decode state for portUnOtrDisplayList — the per-opcode handlers
// below were split out of the original monolithic loop without changing
// any emitted word, patch, or error path.
struct UnotrCtx
{
	const Fast::DisplayList &dl;
	const SSB64SyntheticRelocSpec &spec;
	const std::function<bool(uint64_t, int32_t &)> &resolveReference;
	const char *slice_path;
	bool vanilla;
	SynthDl &out;
	size_t i = 0;
	uint32_t w0 = 0;
	uint32_t w1 = 0;
	uint8_t opcode = 0;
	bool skipArtificialBranchEnd = false;
};

static bool UnotrResolveHash(const UnotrCtx &C, const Gfx &hashCmd, int32_t &slice_out)
{
	if (!C.resolveReference(portReadHash64(hashCmd), slice_out))
	{
		// I7: resolution must be total — an unresolved non-injected
		// hash is a fatal load error, never a silent passthrough.
		spdlog::error("[deblob] I7 violated: unresolved reference hash 0x{:016X} in '{}'"
		              " — diagnose: compare SSB64_SYNTH_INSPECT output with the manifest",
		              portReadHash64(hashCmd), C.slice_path);
		return false;
	}
	return true;
}

// Fast64 XML uses LUS-owned path strings, unlike Torch's binary
// hash pairs. Never truncate a host string pointer into the N64 image.
static bool UnotrDecodeXmlPathRef(UnotrCtx &C)
{
	const auto &cmd = C.dl.Instructions[C.i];
	const char *path = reinterpret_cast<const char *>(cmd.words.w1);
	if (std::find(C.dl.Strings.begin(), C.dl.Strings.end(), path) == C.dl.Strings.end())
	{
		spdlog::error("[deblob] invalid filepath command in '{}'", C.slice_path);
		return false;
	}
	int32_t target;
	if (!C.resolveReference(CRC64(path), target))
	{
		spdlog::error("[deblob] missing or unsupported asset '{}' referenced by '{}'", path, C.slice_path);
		return false;
	}
	uint32_t addend = 0, width = 1;
	if (C.opcode == 0x24)
	{
		if (++C.i >= C.dl.Instructions.size()) return false;
		const auto &args = C.dl.Instructions[C.i];
		const uint32_t count = static_cast<uint32_t>(args.words.w0);
		const uint32_t index = static_cast<uint32_t>(args.words.w1) >> 16;
		if (count == 0 || count > 32 || index > 32 - count) return false;
		addend = (static_cast<uint32_t>(args.words.w1) & 0xFFFF) * sizeof(Vtx);
		width = count * sizeof(Vtx);
		C.out.words.push_back(0x01000000u | (count << 12) | ((index + count) << 1));
	}
	else
	{
		C.out.words.push_back(((C.opcode == 0x25 ? 0xFDu : 0xDEu) << 24) | (C.w0 & 0x00FFFFFFu));
	}
	C.out.patches.push_back({(uint32_t)C.out.words.size(), target, addend, false, width});
	C.out.words.push_back(0);
	return true;
}

// XML Triangle1 uses LUS's wide-index encoding
static bool UnotrDecodeXmlTriangle1(UnotrCtx &C)
{
	const uint32_t a = C.w0 & 0xFFFFFF, b = C.w1 >> 16, c = C.w1 & 0xFFFF;
	if (a >= 32 || b >= 32 || c >= 32) return false;
	C.out.words.push_back(0x05000000u | (a << 17) | (b << 9) | (c << 1));
	C.out.words.push_back(0);
	return true;
}

static bool UnotrDecodeVtxHash(UnotrCtx &C)
{
	// Unresolved G_VTX refs never get the OTR opcode (torch emits
	// the plain original command instead), so 0x32 is always
	// followed by a real hash pair.
	if ((C.i + 1) >= C.dl.Instructions.size())
	{
		spdlog::error("[deblob] truncated VTX OTR command in '{}'", C.slice_path);
		return false;
	}
	const auto &hashCmd = C.dl.Instructions[++C.i];
	const uint32_t nvtx = (C.w0 >> 12) & 0xFF;
	const uint32_t end = (C.w0 >> 1) & 0x7F;
	if (nvtx == 0 || nvtx > 32 || end < nvtx || end > 32 || C.w1 % sizeof(Vtx) != 0)
	{
		spdlog::error("[deblob] invalid vertex count, buffer index or alignment in '{}'", C.slice_path);
		return false;
	}
	C.out.words.push_back((0x01u << 24) | (nvtx << 12) | (end << 1));
	int32_t slice;
	if (!UnotrResolveHash(C, hashCmd, slice))
	{
		return false;
	}
	// w1 carries the byte delta within the vertex slice
	C.out.patches.push_back({(uint32_t)C.out.words.size(), slice, C.w1, false,
	                         nvtx * static_cast<uint32_t>(sizeof(Vtx))});
	C.out.words.push_back(0);
	return true;
}

// The pair following an unresolved OTR command is a literal: either a
// duplicate of the OTR word (G_DL/G_MOVEMEM) or the original command
// verbatim (G_SETTIMG). Rebuild the real command from it.
static bool UnotrEmitLiteralRef(UnotrCtx &C, const Gfx &pairCmd, uint8_t real_opcode, bool duplicated)
{
	const uint32_t literal = duplicated ? C.w1 : static_cast<uint32_t>(pairCmd.words.w1);
	const bool external = !C.vanilla && portFindExternalDescriptor(C.spec, C.slice_path, literal);
	if (!C.vanilla && !external && ((literal >> 24) == 0 || (literal >> 24) > 15))
	{
		spdlog::error("[deblob] unresolved vanilla relocation descriptor in override '{}'; export a named asset reference", C.slice_path);
		return false;
	}
	if (C.opcode == kOTRGMoveMemHash)
	{
		// The OTR MOVEMEM rewrite destroys the original
		// pointer word — an unresolved one cannot be
		// reconstructed. Generator-side node coverage must
		// prevent this (I7).
		spdlog::error("[deblob] I7 violated: unresolved MOVEMEM in '{}'"
		              " (pointer lost at extraction) — diagnose:"
		              " compare SSB64_SYNTH_INSPECT output with the manifest",
		              C.slice_path);
		return false;
	}
	if (duplicated)
	{
		// OTR words duplicated: reconstruct the real command
		// (branch bits were zeroed by the OTR macro; the I5
		// gate catches any vanilla DL that actually used them)
		C.out.words.push_back(((uint32_t)real_opcode << 24) |
		                      (C.opcode == kOTRGSetTImgHash ? (C.w0 & 0x00FFFFFFu) : 0u));
		if (external) C.out.words.push_back(C.w1);
		else portPushLiteralRef(C.spec, C.out, C.w1);
	}
	else
	{
		// pair IS the original command — emit verbatim
		C.out.words.push_back(static_cast<uint32_t>(pairCmd.words.w0));
		if (external) C.out.words.push_back(literal);
		else portPushLiteralRef(C.spec, C.out, literal);
	}
	return true;
}

// The hash pair resolved to a named slice: emit the real opcode plus a
// patch that pass-2 rewrites to the target's layout offset.
static bool UnotrEmitNamedRef(UnotrCtx &C, const Gfx &pairCmd, bool named_reference, int32_t slice)
{
	if (!named_reference)
	{
		spdlog::error("[deblob] I7 violated: unresolved reference hash 0x{:016X} in '{}'",
		              portReadHash64(pairCmd), C.slice_path);
		return false;
	}
	if (C.opcode == kOTRGDlHash)
	{
		const uint32_t branch = (C.w0 >> 16) & 0xFF;
		C.out.words.push_back((0xDEu << 24) | (branch << 16));
		C.out.patches.push_back({(uint32_t)C.out.words.size(), slice, 0, false});
	}
	else if (C.opcode == kOTRGSetTImgHash)
	{
		C.out.words.push_back((0xFDu << 24) | (C.w0 & 0x00FFFFFFu));
		C.out.patches.push_back({(uint32_t)C.out.words.size(), slice, 0, false});
	}
	else
	{
		const bool hasOffset = ((C.w1 >> 8) & 0xFF) != 0;
		C.out.words.push_back((0xDCu << 24) | (C.w0 & 0x00FFFFFFu));
		C.out.patches.push_back({(uint32_t)C.out.words.size(), slice,
		                         hasOffset ? 8u : 0u, false});
	}
	C.out.words.push_back(0);
	return true;
}

static bool UnotrDecodeRefHash(UnotrCtx &C)
{
	if ((C.i + 1) >= C.dl.Instructions.size())
	{
		spdlog::error("[deblob] truncated OTR ref command 0x{:02X} in '{}'",
		              C.opcode, C.slice_path);
		return false;
	}
	const uint8_t real_opcode =
		(C.opcode == kOTRGDlHash) ? 0xDE :
		(C.opcode == kOTRGSetTImgHash) ? 0xFD : 0xDC;
	const auto &pairCmd = C.dl.Instructions[++C.i];

	// Torch's unresolved encodings differ per opcode: G_DL and
	// G_MOVEMEM duplicate the OTR command as the pair; G_SETTIMG
	// writes the ORIGINAL command as the pair (real opcode in the
	// top byte). Cover both shapes.
	const auto &otrCmd = C.dl.Instructions[C.i - 1];
	const bool duplicated =
		(pairCmd.words.w0 == otrCmd.words.w0 && pairCmd.words.w1 == otrCmd.words.w1);
	int32_t resolved_slice;
	const bool named_reference = C.resolveReference(portReadHash64(pairCmd), resolved_slice);
	if (!named_reference && (duplicated || portIsLiteralPair(pairCmd, real_opcode)))
	{
		if (!UnotrEmitLiteralRef(C, pairCmd, real_opcode, duplicated))
		{
			return false;
		}
	}
	else if (!UnotrEmitNamedRef(C, pairCmd, named_reference, resolved_slice))
	{
		return false;
	}
	if (C.opcode == kOTRGDlHash)
	{
		C.skipArtificialBranchEnd = (((C.w0 >> 16) & 0xFF) & G_DL_NOPUSH) != 0;
	}
	return true;
}

// Overrides must not smuggle raw pointer commands past the OTR decoding:
// OTR-range opcodes or unresolved pointer-bearing w1s are rejected.
static bool UnotrCheckOverrideCommand(const UnotrCtx &C)
{
	if (C.vanilla ||
	    !((C.opcode >= 0x20 && C.opcode <= 0x45) ||
	      ((C.opcode == 0x01 || C.opcode == 0xDA || C.opcode == 0xDC || C.opcode == 0xDE || C.opcode == 0xFD) &&
	       ((C.w1 >> 24) == 0 || (C.w1 >> 24) > 15) &&
	       !portFindExternalDescriptor(C.spec, C.slice_path, C.w1))))
	{
		return true;
	}
	spdlog::error("[deblob] unsupported or unresolved pointer command 0x{:02X} in override '{}'", C.opcode, C.slice_path);
	return false;
}

// Torch appends a G_ENDDL terminator to count-delimited joint DLs so
// LUS's factory can read them; drop it when the reconstruction lands
// exactly one command past the vanilla slice (byte-exact fast path).
static bool UnotrFinalize(UnotrCtx &C, uint32_t vanilla_size)
{
	if (C.vanilla && C.out.words.size() >= 2 &&
	    C.out.words.size() * sizeof(uint32_t) == (size_t)vanilla_size + 8 &&
	    C.out.words[C.out.words.size() - 2] == 0xDF000000u &&
	    C.out.words.back() == 0)
	{
		C.out.words.resize(C.out.words.size() - 2);
	}
	if (!C.vanilla && (C.out.words.size() < 2 ||
	    (C.out.words[C.out.words.size() - 2] != 0xDF000000u &&
	     C.out.words[C.out.words.size() - 2] != 0xDE010000u)))
	{
		spdlog::error("[deblob] replacement DL '{}' must end with EndDisplayList or JumpToDisplayList", C.slice_path);
		return false;
	}

	return true;
}

enum class UnotrOtrKind { Marker, Vtx, Ref, Plain };

static UnotrOtrKind UnotrClassifyOtr(uint8_t opcode)
{
	if (opcode == kOTRGMarker) return UnotrOtrKind::Marker;
	if (opcode == kOTRGVtxHash) return UnotrOtrKind::Vtx;
	if (opcode == kOTRGDlHash || opcode == kOTRGSetTImgHash || opcode == kOTRGMoveMemHash)
		return UnotrOtrKind::Ref;
	return UnotrOtrKind::Plain;
}

static bool UnotrDecodeCommand(UnotrCtx &C)
{
	// Fast64 XML escapes decode before the branch-end skip logic; the
	// skip flag deliberately survives them.
	if (C.opcode == 0x24 || C.opcode == 0x25 || C.opcode == 0x27)
	{
		return UnotrDecodeXmlPathRef(C);
	}
	if (C.opcode == 0x26)
	{
		return UnotrDecodeXmlTriangle1(C);
	}

	if (C.skipArtificialBranchEnd && C.opcode == 0xDF)
	{
		C.skipArtificialBranchEnd = false;
		return true;
	}
	C.skipArtificialBranchEnd = false;

	switch (UnotrClassifyOtr(C.opcode))
	{
	case UnotrOtrKind::Marker:
		if (C.i + 1 >= C.dl.Instructions.size()) return false;
		C.i++;
		return true;
	case UnotrOtrKind::Vtx:
		return UnotrDecodeVtxHash(C);
	case UnotrOtrKind::Ref:
		return UnotrDecodeRefHash(C);
	case UnotrOtrKind::Plain:
		break;
	}
	if (!UnotrCheckOverrideCommand(C)) return false;
	C.out.words.push_back(C.w0);
	C.out.words.push_back(C.w1);
	return true;
}

bool portUnOtrDisplayList(const Fast::DisplayList &dl,
                                 const SSB64SyntheticRelocSpec &spec,
                                 const std::function<bool(uint64_t, int32_t &)> &resolveReference,
                                 const char *slice_path, uint32_t vanilla_size,
                                 bool vanilla,
                                 SynthDl &out)
{
	UnotrCtx C{dl, spec, resolveReference, slice_path, vanilla, out};
	out.words.reserve(dl.Instructions.size() * 2);

	for (C.i = 0; C.i < dl.Instructions.size(); C.i++)
	{
		const auto &cmd = dl.Instructions[C.i];
		C.w0 = static_cast<uint32_t>(cmd.words.w0);
		C.w1 = static_cast<uint32_t>(cmd.words.w1);
		C.opcode = static_cast<uint8_t>(C.w0 >> 24);
		if (!UnotrDecodeCommand(C))
		{
			return false;
		}
	}

	return UnotrFinalize(C, vanilla_size);
}
