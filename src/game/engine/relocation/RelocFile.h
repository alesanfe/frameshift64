#pragma once

#include <ship/resource/Resource.h>
#include <vector>
#include <cstdint>

/**
 * SSB64 relocatable file resource.
 *
 * Loaded from .o2r archives at runtime. Contains pre-decompressed file data
 * plus relocation metadata needed for pointer patching.
 *
 * The resource data is in its original big-endian N64 format.
 * Pointer relocation happens at load time in the lbReloc bridge layer,
 * not in the factory.
 */
// Explicit relocation slot lists for synthesized (deblobbed) bundles.
// A synthesized bundle's chain descriptor words were overwritten with real
// content at extraction time, so the linked-list walk is impossible; the
// loader instead applies these flat lists (docs/deblob.md). Offsets are
// BYTE offsets in the bundle's CURRENT layout (u32: u16 word offsets would
// cap bundles at 256 KB, which relayouted mod bundles may exceed) and are
// emitted in chain order so fixups apply in the same sequence the chain
// walk would have used.
struct RelocExplicitInternSlot {
    uint32_t SlotByteOff;     // where the pointer word lives
    uint32_t TargetByteOff;   // what it points at (within this bundle)
};

struct RelocExplicitExternSlot {
    uint32_t SlotByteOff = 0;
    uint16_t DepFileId = 0;   // dependency file (explicit — no positional list)
    uint32_t DepWordOff = 0;  // u32-word offset into the dependency's data
};

class RelocFile final : public Ship::Resource<void> {
public:
    using Resource::Resource;

    void* GetPointer() override;
    size_t GetPointerSize() override;

    uint32_t FileId = 0;
    uint16_t RelocInternOffset = 0xFFFF; // internal reloc chain start (in u32 words), 0xFFFF = none
    uint16_t RelocExternOffset = 0xFFFF; // external reloc chain start (in u32 words), 0xFFFF = none
    std::vector<uint16_t> ExternFileIds;   // file IDs referenced by external relocations
    std::vector<uint8_t> Data;             // decompressed file data (big-endian)

    // Synthesized bundles only (see above): when true, the loader uses the
    // explicit lists below and never walks the in-data chains.
    bool HasExplicitRelocation = false;
    std::vector<RelocExplicitInternSlot> ExplicitInternSlots;
    std::vector<RelocExplicitExternSlot> ExplicitExternSlots;
};
