#ifndef LBRELOC_FIXUP_INTERNAL_H
#define LBRELOC_FIXUP_INTERNAL_H

/* Internal interface shared by the lbreloc fixup translation units
 * (lbreloc_byteswap.cpp, lbreloc_fixup_state.cpp, lbreloc_fixup_chain.cpp,
 * lbreloc_fixup_runtime.cpp, lbreloc_fixup_sprite.cpp).
 *
 * Holds the cross-TU pieces that used to live file-static in
 * lbreloc_byteswap.cpp: the idempotency/protection tracking containers
 * (defined in lbreloc_fixup_state.cpp), the shared bridge externs, and the
 * GBI command constants used by both the blob scan and the chain/runtime
 * fixup paths. Not installed for external consumers — the public ABI stays
 * in lbreloc_byteswap.h. */

#include <stddef.h>
#include <stdint.h>

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <ship/utils/binarytools/endianness.h>

extern "C" void port_log(const char *fmt, ...);
extern "C" void portStageAuditNoteRuntimeVtx(unsigned int num_vtx);
extern "C" void portStageAuditNoteRuntimeTex(unsigned int num_bytes);
extern "C" void portStageAuditNoteVtxDispatch(int in_reloc_file);
extern "C" void portStageAuditNoteVtxDispatchFile(int file_id);
extern "C" void portStageAuditNoteTexDispatch(int in_reloc_file);
extern "C" void portRelocTexFixupLog(const char *fmt, ...);
extern "C" int  portRelocFindContainingFile(const void *ptr, uintptr_t *out_base, size_t *out_size);
extern "C" int  portRelocFindFileIdAndBase(const void *ptr, uintptr_t *out_base);
extern "C" size_t portRelocFileExtentFrom(const void *ptr);
extern "C" bool portRelocDescribePointer(const void *ptr, uintptr_t *out_base, size_t *out_size,
                                          unsigned int *out_file_id, const char **out_path);

/* Defined in RelocPointerTable.cpp — resolves a tokenized slot back to its
 * pointer without registering it (declared extern "C" in RelocPointerTable.h). */
extern "C" void *portRelocTryResolvePointer(uint32_t token);

/* ---------------------------------------------------------------
 * Shared fixup tracking state — definitions in lbreloc_fixup_state.cpp.
 * All fixup paths (pass2 regions, struct fixups, chain-walk fixups, lazy
 * runtime fixups) share these so they don't undo each other's work.
 * Cleared at scene change via portResetStructFixups.
 * --------------------------------------------------------------- */

// Tracks which (base + offset) regions have been fixed to prevent
// double-fixup. Used by per-struct fixups (Sprite, Bitmap, MObjSub, etc.),
// pass2 vertex fixups, the chain-walk texture fixup, and the runtime
// lazy vertex fixup.
extern std::unordered_set<uintptr_t> sStructU16Fixups;

// Tracks runtime texture bases that own at least one fixed word, plus the
// largest request size seen from that base. Preserves the old pass2/chain
// skip behavior for exact-base matches while sTexFixupWords handles the
// actual per-word idempotency.
extern std::unordered_map<uintptr_t, unsigned int> sTexFixupExtent;

// Runtime texture/TLUT fixups can overlap even when they start at different
// addresses. CSS gate palettes are adjacent 0x28-byte blocks, but their Sprite
// requests a 512-byte TLUT load from each block. Track each u32 word so the
// second overlapping load skips words fixed by the first instead of BSWAPing
// them back to the wrong byte order.
extern std::unordered_set<uintptr_t> sTexFixupWords;

// Memory ranges of decoded struct arrays that runtime texture/palette
// BSWAPs must NOT touch. Certain N64 sprite files intentionally overlap the
// tail of a CI8 palette (which Fast3D loads as a fixed 512-byte block even if
// the actual palette is smaller) with the start of the bitmap array — on
// hardware the unused palette entries don't matter, but on the port the
// runtime-path BSWAP would corrupt the bitmap/sprite structs we already
// byte-swap-fixed up. Entries are (begin, end) pairs.
struct ProtectedRange { uintptr_t begin; uintptr_t end; };
extern std::vector<ProtectedRange> sProtectedStructRanges;

// Separate tracking for post-decode 4c sprite deswizzle
// (portDeswizzleDecodedSprite4c) — its own set because
// portFixupSpriteBitmapData already inserted the same buf addresses during
// the pre-decode BSWAP32 pass.
extern std::unordered_set<uintptr_t> sDeswizzle4cFixups;

/* Absolute addresses of every tokenized reloc chain slot currently live.
 * Texel data never contains chain slots, so a runtime texture fixup whose
 * range covers one is aimed at non-texture data (a stale or garbage
 * SETTIMG operand) and must be refused — applying it BSWAP32s live
 * descriptor and token words in place. */
extern std::unordered_set<uintptr_t> sChainSlotAddrs;

/* Register [begin, begin+size) as a protected struct range. Defined in
 * lbreloc_fixup_state.cpp; called by the sprite/bitmap fixups. */
void portRegisterProtectedStructRange(const void *begin, size_t size);

// F3DEX2 GBI opcodes (from gbi.h with F3DEX_GBI_2 defined)
#define GBI_VTX         0x01
#define GBI_DL          0xDE
#define GBI_ENDDL       0xDF
#define GBI_SETTIMG     0xFD
#define GBI_LOADBLOCK   0xF3
#define GBI_LOADTLUT    0xF0

// Texture size constants (bits per pixel)
#define G_IM_SIZ_4b     0
#define G_IM_SIZ_8b     1
#define G_IM_SIZ_16b    2
#define G_IM_SIZ_32b    3

#endif // LBRELOC_FIXUP_INTERNAL_H
