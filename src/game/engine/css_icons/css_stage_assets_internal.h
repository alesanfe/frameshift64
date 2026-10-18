/* Internal shared declarations for the CSS stage-asset translation units —
 * the full rationale lives in port_css_stage_assets.cpp. Only the
 * css_stage_*.cpp / port_css_stage_assets.cpp files include this header;
 * the public ABI stays in port_css_stage_assets.h.
 *
 * Split layout:
 *   css_stage_build.cpp        sprite/bitmap construction, PNG loads,
 *                              o2r derivation, the getSprite dispatcher
 *   port_css_stage_assets.cpp  stage table docs + the 4 public C wrappers
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

/* PR/sp.h uses _Static_assert (C11) and cannot be included from C++.
 * Opaque type tags. */
struct sprite;
struct bitmap;
typedef struct sprite Sprite;
typedef struct bitmap Bitmap;

// Baked 96x10 RGBA16 nameplate data, generated at build time from text
// strings only (tools/render_nameplates.py + png_to_c_array.py — no ROM
// data, so legal to ship). Release fallback when no PNG override exists.
#include "final_destination_name_data.h"
#include "metal_cavern_name_data.h"
#include "battlefield_name_data.h"

// libultraship fast/interpreter.cpp. The interpreter's SETTIMG low-VA guard
// drops texture-set commands for addresses <= 0x0FFFFFFF outside a loaded
// module — a heuristic against stale N64-segment tokens. Our pixel buffers
// are plain mallocs, and on a session whose brk heap hasn't grown past
// 256 MB they land BELOW that threshold: the guard then silently drops
// their SETTIMGs and the CSS renders stale garbage (and a stale bound
// texture can later crash the signature probe). Whether a given run is
// affected depends purely on how much the heap grew before the CSS opened
// (an asset re-extraction at boot pushes it well past the threshold, a
// warm start does not) — so EVERY synthetic pixel buffer must be
// registered as trusted at creation, not just the baked .rodata arrays.
extern "C" void gfxRegisterTrustedLowVARange(const void *base, size_t size);

namespace ssb64::css {

// ---------------------------------------------------------------------------
// Stage table.
//
// To add a new port-introduced stage:
//   1. Append a StageEntry here.
//   2. Add a matching entry to STAGES in tools/derive_stage_assets.py.
//   3. Add the CMake OUTPUT + DEPENDS + POST_BUILD copy for the new PNGs.
// ---------------------------------------------------------------------------

struct StageEntry {
    int         gkind;       // GRKind enum value
    const char *name;        // filename stem (e.g. "final_destination")

    // Background sprite geometry (ROM-verified from dStageLastBackground_0x26c88):
    int         bg_w;        // full image width  (pixels)
    int         bg_h;        // full image height (pixels)
    int         bg_nbitmaps; // number of TMEM-slice bitmaps
    int         bg_bm_h;    // rendered rows per bitmap (bmheight)
    int         bg_bm_hreal;// N64 bmHreal (rows in the TMEM load including fringe)
    int         bg_ndisplist;// ndisplist from ROM sprite

    // Nameplate sprite: set to true when a synthesized <name>_name.png exists.
    // False for stages that ship a ROM-resident IA4 nameplate (the 9 starters).
    bool        has_name_png; // if false, portCSSGetStageNameSprite returns NULL

    // Emblem sprite: set to true when a synthesized <name>_emblem.png exists.
    // False for stages that use a ROM-resident 64x48 IA4 emblem (the 9 starters).
    // The PNG is loaded as a silhouette mask (alpha channel) and packed to IA4
    // single-bitmap at runtime — matches the ROM FT emblem format byte-for-byte
    // so no multi-bitmap strip geometry is needed.
    bool        has_emblem_png; // if false, portCSSGetStageEmblemSprite returns NULL

    // o2r wallpaper source — reloc file id + byte offset of the 300x220
    // RGBA16 wallpaper Sprite inside that file's (big-endian) data. Used to
    // derive the background/icon at runtime when no PNG exists (release
    // builds). Mirrors STAGES in tools/derive_stage_assets.py, which reads
    // the same sprite out of the same file at build time.
    uint32_t    wp_file_id;
    uint32_t    wp_sprite_off;

    // Baked nameplate pixels (96x10 RGBA16 BE, compiled in from the
    // generated <stem>_name_data.h). Release fallback for kName when no
    // PNG override exists; NULL for a stage without a baked plate.
    const uint8_t *baked_name;
};

inline constexpr StageEntry STAGE_TABLE[] = {
    {
        /* nGRKindLast = 16 */
        16, "final_destination",
        300, 220,
        44,  /* bg_nbitmaps: 44 * 5 rendered rows = 220 total */
        5,   /* bg_bm_h:    rows rendered per bitmap */
        6,   /* bg_bm_hreal: rows in the TMEM load   */
        552, /* bg_ndisplist from dStageLastBackground_0x26c88 */
        true,/* has_name_png: synthesized 96x10 nameplate */
        true,/* has_emblem_png: upscaled 64x48 emblem from MasterHand icon */
        96, 0x26c88, /* wallpaper: reloc file 96 (StageLastFile1), dStageLastBackground */
        kNameFinalDestinationRGBA16,
    },
    {
        /* nGRKindMetal = 13 — Meta Crystal (Metal Cavern in the port CSS).
         * Stage data file 0x62 ships the same 300x220/44-bitmap RGBA16
         * wallpaper format as FD at offset 0x26c88 — geometry matches FD
         * byte-for-byte so the same buildBackgroundSprite parameters work. */
        13, "metal_cavern",
        300, 220,
        44, 5, 6, 552,
        true,  /* synthesized "METAL CAVERN" nameplate */
        false, /* no emblem — same skip path as FD until IA4 render is fixed */
        0x62, 0x26c88, /* wallpaper: reloc file 0x62 — Meta Crystal stage data */
        kNameMetalCavernRGBA16,
    },
    {
        /* nGRKindZako = 14 — Duel Zone (Battlefield in the port CSS).
         * Same wallpaper geometry as FD/Metal. */
        14, "battlefield",
        300, 220,
        44, 5, 6, 552,
        true,  /* synthesized "BATTLEFIELD" nameplate */
        false, /* no emblem */
        0x61, 0x26c88, /* wallpaper: reloc file 0x61 — Duel Zone stage data */
        kNameBattlefieldRGBA16,
    },
    // { next_gkind, "next_name", bg_w, bg_h, bg_nbitmaps, bg_bm_h, bg_bm_hreal,
    //   bg_ndisplist, has_name_png, has_emblem_png, wp_file_id, wp_sprite_off,
    //   baked_name },
};

inline constexpr int kStageCount = (int)(sizeof(STAGE_TABLE) / sizeof(STAGE_TABLE[0]));

// Icon dimensions — same for all stages (matches N64 CSS format).
inline constexpr int      kIconW  = 48;
inline constexpr int      kIconH  = 36;
inline constexpr uint8_t  kFmtRGBA16 = 0; /* G_IM_FMT_RGBA */
inline constexpr uint8_t  kFmtIA     = 4; /* G_IM_FMT_IA   — emblems, nameplates */
inline constexpr uint8_t  kSiz16b    = 2; /* G_IM_SIZ_16b  */
inline constexpr uint8_t  kSiz4b     = 0; /* G_IM_SIZ_4b   — IA4: 2 pixels/byte */

// ---------------------------------------------------------------------------
// Cache entries.
// ---------------------------------------------------------------------------

enum AssetKind { kBackground = 0, kIcon = 1, kName = 2, kEmblem = 3, kAssetKindCount = 4 };

struct CacheEntry {
    Sprite   *sprite       = nullptr;
    Bitmap   *bitmaps      = nullptr;  // pointer to first Bitmap in the array
    uint8_t  *rgba16_buf   = nullptr;  // heap-allocated RGBA16 BE pixel data
    int       nbitmaps     = 0;
    bool      derive_failed = false;   // o2r derivation failed once — don't
                                       // re-parse/re-log on every CSS rebuild
                                       // (the archive can't change mid-session;
                                       // a PNG dropped in later is still probed)
};

// Flat 2D array: cache[gkind_index][asset_kind] — defined in css_stage_build.cpp
extern CacheEntry  sCache[kStageCount][kAssetKindCount];
extern std::mutex  sCacheMutex;

/* ---- css_stage_build.cpp ---- */
Sprite *getSprite(int gkind, AssetKind asset_kind);

} // namespace ssb64::css
