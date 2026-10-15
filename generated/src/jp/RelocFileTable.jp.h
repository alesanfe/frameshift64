#pragma once

#define RELOC_FILE_COUNT 2107

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Maps file_id (0–2106) to the resource path in the .o2r archive.
 * Auto-generated from assets/game/jp/reloc_*.yml by tools/code-generation/generate_reloc_table.py.
 */
extern const char* const gRelocFileTable[RELOC_FILE_COUNT];

#ifdef __cplusplus
}
#endif
