/**
 * css_stage_build.cpp — sprite/bitmap construction for the CSS stage
 * assets: put_* emitters, PNG loaders, o2r wallpaper derivation,
 * per-kind builders, and the getSprite dispatcher + sprite cache.
 * Split from port_css_stage_assets.cpp; the rationale lives there.
 */

#ifdef PORT

/* This TU's include chain reaches windows.h (via libultraship.h) without
 * NOMINMAX; the min()/max() macros mangle the std::min/std::max calls in
 * the derivation and bilinear code into MSVC C2589. Same guard as
 * libultraship's interpreter.cpp. */
#define NOMINMAX

#include "port_css_stage_assets.h"
#include "css_stage_assets_internal.h"
#include "css_le_put.h"

#include "app_paths.h"
#include "lbreloc_byteswap.h"
#include "icon_scale.h"
#include "rgba16_swizzle.h"
#include "RelocFile.h"
#include "RelocFileTable.h"
#include "RelocPointerTable.h"
#include "port_log.h"

#include <ship/resource/ResourceManager.h>

// stb_image's implementation lives in libultraship's stb_impl.c.
// Only include the header to pick up declarations.
#include <stb_image.h>

#include <libultraship/libultraship.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace ssb64::css;

namespace {

// ---------------------------------------------------------------------------
// RGBA8888 -> RGBA16 BE converter.
//
// stb_image returns 4-byte RGBA8888. The Fast3D renderer expects RGBA16 BE
// (R5G5B5A1) with no TMEM odd-row swizzle (synthetic sprites bypass the
// portFixupSpriteBitmapData swizzle via portMarkSyntheticSprite).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// RGBA8888 -> IA4 silhouette packer.
//
// IA4 is **3 bits of intensity + 1 bit of alpha** per pixel (NOT 4-bit
// intensity — see components/libultraship/src/fast/interpreter.cpp ImportTextureIA4).
// 2 pixels per byte; high nibble = even pixel, low nibble = odd pixel.
//
// For a silhouette emblem source (RGB ignored, alpha encodes the mask):
//   Visible pixel (alpha >= 128): pack 0xF = intensity 7 + alpha 1.
//     Intensity 7 = max, so SObj-side colour modulation (0x5C/0x22/0x00 brown)
//     tints the visible pixels to their authored colour, exactly like every
//     ROM FT emblem.
//   Transparent pixel: pack 0x0 = intensity 0 + alpha 0.
//
// IA4's 1-bit alpha can't represent partial transparency — LANCZOS-upscaled
// edges threshold to fully visible or fully invisible. That's the cost of
// matching the ROM IA4 format exactly; if anti-aliased edges become important,
// CI4 (4-bit indexed) is the next-format-up that fits TMEM at 64x48.
// Width must be even (always true for the emblem dims we target).
// ---------------------------------------------------------------------------

static uint8_t *convertAlphaToIA4(const uint8_t *rgba8, int w, int h) {
    if (w % 2 != 0) return nullptr; // IA4 packs 2 pixels per byte
    const size_t out_bytes = (size_t)w * (size_t)h / 2;
    uint8_t *out = static_cast<uint8_t *>(std::malloc(out_bytes));
    if (!out) return nullptr;

    const uint8_t *src = rgba8;
    uint8_t *dst = out;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; x += 2) {
            uint8_t a0 = src[(y * w + x + 0) * 4 + 3];
            uint8_t a1 = src[(y * w + x + 1) * 4 + 3];
            // Threshold each pixel to a 1-bit alpha mask; visible pixels get
            // full intensity (nibble = 0xF = 0b1111 = I=7 + A=1).
            uint8_t n0 = (a0 >= 128) ? 0xF : 0x0;
            uint8_t n1 = (a1 >= 128) ? 0xF : 0x0;
            *dst++ = (uint8_t)((n0 << 4) | n1);
        }
    }
    return out;
}

static uint8_t *convertToRGBA16BE(const uint8_t *rgba8, int w, int h) {
    if (w <= 0 || h <= 0) return nullptr;
    const size_t npixels = (size_t)w * (size_t)h;
    uint8_t *out = static_cast<uint8_t *>(std::malloc(npixels * 2));
    if (!out) return nullptr;

    for (size_t i = 0; i < npixels; ++i) {
        uint8_t r = rgba8[i * 4 + 0];
        uint8_t g = rgba8[i * 4 + 1];
        uint8_t b = rgba8[i * 4 + 2];
        uint8_t a = rgba8[i * 4 + 3];
        uint16_t r5 = r >> 3;
        uint16_t g5 = g >> 3;
        uint16_t b5 = b >> 3;
        uint16_t a1 = a >= 128 ? 1 : 0;
        uint16_t word = (r5 << 11) | (g5 << 6) | (b5 << 1) | a1;
        // Store big-endian.
        out[i * 2 + 0] = (uint8_t)(word >> 8);
        out[i * 2 + 1] = (uint8_t)(word & 0xFF);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Build a multi-bitmap background Sprite from heap-allocated RGBA16 data.
//
// Layout matches dStageLastBackground_0x26c88: w x h image split into
// nbitmaps strips of bm_h rendered rows each.
// ---------------------------------------------------------------------------

static Sprite *buildBackgroundSprite(const StageEntry &se,
                                     uint8_t *rgba16_buf,
                                     CacheEntry *entry)
{
    const int w           = se.bg_w;
    const int h           = se.bg_h;
    const int nbitmaps    = se.bg_nbitmaps;
    const int bm_h        = se.bg_bm_h;
    const int bm_hreal    = se.bg_bm_hreal;
    const int ndisplist   = se.bg_ndisplist;
    const int bm_bytes    = w * bm_h * 2; // bytes per bitmap strip (rendered rows only)

    uint8_t *sp_raw = static_cast<uint8_t *>(std::malloc(68));
    uint8_t *bm_raw = static_cast<uint8_t *>(std::malloc((size_t)nbitmaps * 16));
    void   **buf_ptrs = static_cast<void **>(std::malloc((size_t)nbitmaps * sizeof(void *)));
    if (!sp_raw || !bm_raw || !buf_ptrs) {
        std::free(sp_raw); std::free(bm_raw); std::free(buf_ptrs);
        return nullptr;
    }
    std::memset(sp_raw, 0, 68);
    std::memset(bm_raw, 0, (size_t)nbitmaps * 16);

    for (int i = 0; i < nbitmaps; ++i) {
        uint8_t *bm       = bm_raw + i * 16;
        uint8_t *strip    = rgba16_buf + (size_t)i * bm_bytes;
        put_s16(bm, 0x00, (int16_t)w);
        put_s16(bm, 0x02, (int16_t)w);
        put_s16(bm, 0x04, 0);
        put_s16(bm, 0x06, 0);
        put_u32(bm, 0x08, portRelocRegisterPointer(strip));
        put_s16(bm, 0x0C, (int16_t)bm_h);
        put_s16(bm, 0x0E, 0);
        buf_ptrs[i] = strip;
    }

    Bitmap *bm0 = reinterpret_cast<Bitmap *>(bm_raw);

    put_sprite_head(sp_raw, w, h,
                        portRelocRegisterPointer(nullptr));
    put_s16(sp_raw, 0x28, (int16_t)nbitmaps);
    put_s16(sp_raw, 0x2A, (int16_t)ndisplist);
    put_s16(sp_raw, 0x2C, (int16_t)bm_h);
    put_s16(sp_raw, 0x2E, (int16_t)bm_hreal);
    sp_raw[0x30] = kFmtRGBA16; sp_raw[0x31] = kSiz16b;
    sp_raw[0x32] = 0;          sp_raw[0x33] = 0;
    put_u32(sp_raw, 0x34, portRelocRegisterPointer(bm0));
    put_u32(sp_raw, 0x38, portRelocRegisterPointer(nullptr));
    put_u32(sp_raw, 0x3C, portRelocRegisterPointer(nullptr));
    put_s16(sp_raw, 0x40, 0); put_s16(sp_raw, 0x42, 0);

    Sprite *sp = reinterpret_cast<Sprite *>(sp_raw);
    portMarkSyntheticSprite(sp, bm0, (unsigned int)nbitmaps, buf_ptrs);
    std::free(buf_ptrs);

    gfxRegisterTrustedLowVARange(rgba16_buf, (size_t)w * h * 2);

    entry->sprite     = sp;
    entry->bitmaps    = bm0;
    entry->rgba16_buf = rgba16_buf;
    entry->nbitmaps   = nbitmaps;
    return sp;
}

// ---------------------------------------------------------------------------
// Build a synthetic single-bitmap Sprite. Shared body for the icon (48x36
// RGBA16), nameplate (96x10 RGBA16), and emblem (64x48 IA4) sprites — they
// differ only in dims, ndisplist, pixel format, and texel byte size.
// ---------------------------------------------------------------------------

static Sprite *buildSingleBitmapSprite(int w, int h, int ndisplist,
                                       uint8_t fmt, uint8_t siz, size_t buf_bytes,
                                       uint8_t *buf, CacheEntry *entry)
{
    const int nbitmaps  = 1;

    uint8_t *sp_raw = static_cast<uint8_t *>(std::malloc(68));
    uint8_t *bm_raw = static_cast<uint8_t *>(std::malloc(16));
    if (!sp_raw || !bm_raw) { std::free(sp_raw); std::free(bm_raw); return nullptr; }
    std::memset(sp_raw, 0, 68);
    std::memset(bm_raw, 0, 16);

    put_s16(bm_raw, 0x00, (int16_t)w);
    put_s16(bm_raw, 0x02, (int16_t)w);
    put_s16(bm_raw, 0x04, 0);
    put_s16(bm_raw, 0x06, 0);
    put_u32(bm_raw, 0x08, portRelocRegisterPointer(buf));
    put_s16(bm_raw, 0x0C, (int16_t)h);
    put_s16(bm_raw, 0x0E, 0);

    Bitmap *bm = reinterpret_cast<Bitmap *>(bm_raw);

    put_sprite_head(sp_raw, w, h,
                        portRelocRegisterPointer(nullptr));
    put_s16(sp_raw, 0x28, (int16_t)nbitmaps);
    put_s16(sp_raw, 0x2A, (int16_t)ndisplist);
    put_s16(sp_raw, 0x2C, (int16_t)h);
    put_s16(sp_raw, 0x2E, (int16_t)h);
    sp_raw[0x30] = fmt; sp_raw[0x31] = siz;
    sp_raw[0x32] = 0;   sp_raw[0x33] = 0;
    put_u32(sp_raw, 0x34, portRelocRegisterPointer(bm));
    put_u32(sp_raw, 0x38, portRelocRegisterPointer(nullptr));
    put_u32(sp_raw, 0x3C, portRelocRegisterPointer(nullptr));
    put_s16(sp_raw, 0x40, 0); put_s16(sp_raw, 0x42, 0);

    Sprite *sp = reinterpret_cast<Sprite *>(sp_raw);

    void *buf_ptrs[1] = { buf };
    portMarkSyntheticSprite(sp, bm, (unsigned int)nbitmaps, buf_ptrs);

    gfxRegisterTrustedLowVARange(buf, buf_bytes);

    entry->sprite     = sp;
    entry->bitmaps    = bm;
    entry->rgba16_buf = buf;
    entry->nbitmaps   = nbitmaps;
    return sp;
}

static Sprite *buildIconSprite(uint8_t *rgba16_buf, CacheEntry *entry)
{
    // Icon ndisplist = height (one strip per row).
    return buildSingleBitmapSprite(kIconW, kIconH, kIconH,
                                   kFmtRGBA16, kSiz16b, (size_t)kIconW * kIconH * 2,
                                   rgba16_buf, entry);
}

// ---------------------------------------------------------------------------
// Build a single-bitmap nameplate Sprite (96x10).
//
// Matches the ROM IA4 nameplate sprite dimensions from reloc file 30 / MNMaps:
//   width=96, height=10, 1 bitmap, bmheight=10, bmHreal=10, ndisplist=36.
// Format is stored as RGBA16 (same as background/icon) because convertToRGBA16BE
// is applied at load time and portMarkSyntheticSprite bypasses the TMEM swizzle.
// The caller (mnmaps.c) must force red=green=blue=0x00 on the SObj so the
// sprite renders as black text on transparent — identical to ROM nameplate style.
// ---------------------------------------------------------------------------

static constexpr int kNameW  = 96;
static constexpr int kNameH  = 10;

static_assert(kNameFinalDestinationWidth == kNameW && kNameFinalDestinationHeight == kNameH,
              "baked FD nameplate dims drifted from the 96x10 sprite contract");
static_assert(kNameMetalCavernWidth == kNameW && kNameMetalCavernHeight == kNameH,
              "baked Metal Cavern nameplate dims drifted from the 96x10 sprite contract");
static_assert(kNameBattlefieldWidth == kNameW && kNameBattlefieldHeight == kNameH,
              "baked Battlefield nameplate dims drifted from the 96x10 sprite contract");

static Sprite *buildNameSprite(uint8_t *rgba16_buf, CacheEntry *entry)
{
    // ndisplist=36 matches ROM nameplate sprites (PeachsCastleText etc.)
    return buildSingleBitmapSprite(kNameW, kNameH, 36,
                                   kFmtRGBA16, kSiz16b, (size_t)kNameW * kNameH * 2,
                                   rgba16_buf, entry);
}

// ---------------------------------------------------------------------------
// Build the FD-style emblem Sprite — IA4 single-bitmap, 64x48.
//
// Every ROM FT emblem in reloc file 20 (llFTEmblemSprites*) uses the same
// fields, byte-for-byte:
//   width=64 height=48 nbitmaps=1 ndisplist=36
//   bmheight=48 bmHreal=48 bmfmt=4 (IA) bmsiz=0 (IA4)
//   attr=0x0220 scalex=scaley=1.0
//
// IA4 = 4 bits intensity per pixel, 2 pixels per byte. 64*48/2 = 1536 bytes —
// fits TMEM (4 KB) as a single bitmap, so no multi-strip layout needed. The
// caller sets SObj red/green/blue to the franchise brown (0x5C/0x22/0x00) and
// the combiner tints the intensity texels — identical to how the ROM emblems
// render.
//
// This deliberately matches the ROM Sprite struct field-for-field. Don't
// invent a new layout; if the 9 ROM emblems render correctly, ours does too.
// ---------------------------------------------------------------------------

static constexpr int kEmblemW  = 64;
static constexpr int kEmblemH  = 48;

static Sprite *buildEmblemSprite(uint8_t *ia4_buf, CacheEntry *entry)
{
    // ndisplist=36 matches ROM FT emblems exactly. entry->rgba16_buf keeps its
    // historical name; it holds the packed IA4 buffer here.
    return buildSingleBitmapSprite(kEmblemW, kEmblemH, 36,
                                   kFmtIA, kSiz4b, (size_t)kEmblemW * kEmblemH / 2,
                                   ia4_buf, entry);
}

// ---------------------------------------------------------------------------
// Re-register fixup tracking on every call (scene-boundary safety).
// portResetStructFixups() clears sStructU16Fixups / sDeswizzle4cFixups at
// scene boundaries, so portMarkSyntheticSprite must be called again each time
// the CSS is entered to keep portFixup* as no-ops for these synthetic sprites.
// ---------------------------------------------------------------------------

static void reregisterEntry(const CacheEntry &e) {
    if (!e.sprite || !e.bitmaps || e.nbitmaps == 0) return;
    void **buf_ptrs = static_cast<void **>(std::malloc((size_t)e.nbitmaps * sizeof(void *)));
    if (!buf_ptrs) return;
    // Walk the Bitmap array, reading each bitmap's buf reloc token (LE u32 at
    // offset +0x08 within each 16-byte Bitmap struct) and resolving it back to
    // the actual pointer, so portMarkSyntheticSprite can re-register all entries.
    const uint8_t *bm_base = reinterpret_cast<const uint8_t *>(e.bitmaps);
    for (int i = 0; i < e.nbitmaps; ++i) {
        uint32_t token;
        std::memcpy(&token, bm_base + (size_t)i * 16 + 0x08, 4);
        buf_ptrs[i] = portRelocResolvePointer(token);
    }
    portMarkSyntheticSprite(e.sprite, e.bitmaps, (unsigned int)e.nbitmaps, buf_ptrs);
    std::free(buf_ptrs);
}

// ---------------------------------------------------------------------------
// PNG loader — loads a PNG and converts to RGBA16 BE.
// Expected dimensions are validated; returns nullptr on mismatch/error.
// ---------------------------------------------------------------------------

static uint8_t *loadPNGAsRGBA16BE(const std::string &path, int expected_w, int expected_h) {
    int w = 0, h = 0, channels = 0;
    uint8_t *rgba8 = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!rgba8) {
        port_log("CSS stage assets: stbi_load failed: %s (%s)\n",
                 path.c_str(),
                 stbi_failure_reason() ? stbi_failure_reason() : "?");
        return nullptr;
    }

    if (w != expected_w || h != expected_h) {
        port_log("CSS stage assets: %s — expected %dx%d, got %dx%d; skipping.\n",
                 path.c_str(), expected_w, expected_h, w, h);
        stbi_image_free(rgba8);
        return nullptr;
    }

    uint8_t *rgba16 = convertToRGBA16BE(rgba8, w, h);
    stbi_image_free(rgba8);

    if (!rgba16) {
        port_log("CSS stage assets: convertToRGBA16BE OOM for %s\n", path.c_str());
    }
    return rgba16;
}

// Loads a PNG and returns the alpha channel packed as IA4 (2 px/byte, w*h/2 bytes).
// Used for emblem sprites where matching the ROM's IA4 single-bitmap format
// avoids TMEM-overflow / multi-bitmap-strip rendering bugs entirely.
static uint8_t *loadPNGAsIA4(const std::string &path, int expected_w, int expected_h) {
    int w = 0, h = 0, channels = 0;
    uint8_t *rgba8 = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!rgba8) {
        port_log("CSS stage assets: stbi_load failed: %s (%s)\n",
                 path.c_str(),
                 stbi_failure_reason() ? stbi_failure_reason() : "?");
        return nullptr;
    }
    if (w != expected_w || h != expected_h) {
        port_log("CSS stage assets: %s — expected %dx%d, got %dx%d; skipping.\n",
                 path.c_str(), expected_w, expected_h, w, h);
        stbi_image_free(rgba8);
        return nullptr;
    }
    uint8_t *ia4 = convertAlphaToIA4(rgba8, w, h);
    stbi_image_free(rgba8);
    if (!ia4) {
        port_log("CSS stage assets: convertAlphaToIA4 OOM for %s\n", path.c_str());
    }
    return ia4;
}

// ---------------------------------------------------------------------------
// o2r wallpaper derivation — the release-build source for background + icon.
//
// Shipped builds contain no css_icons PNGs (they are ROM-derived and cannot
// be distributed), but the user's own extracted BattleShip.o2r carries each
// stage's 300x220 RGBA16 CSS wallpaper Sprite inside the reloc file named
// by STAGE_TABLE.wp_file_id. RelocFile::Data is the pristine decompressed
// big-endian file image (the lbreloc bridge memcpy's it into game RAM
// before patching, so the cached resource is never mutated) — parse the
// Sprite exactly the way tools/derive_stage_assets.py does at build time:
// resolve the bitmap array via the RELOC pointer encoding
// ((raw & 0xFFFF) * 4), un-swizzle the TMEM odd-row XOR4 pattern per strip,
// and keep only the bmheight rendered rows of each strip.
//
// Every parse step is bounds-checked and geometry-validated against
// STAGE_TABLE; any mismatch (e.g. a JP archive where the sprite moved)
// returns nullptr and the caller falls back to the question-mark sprite —
// never a garbage image.
// ---------------------------------------------------------------------------

static inline uint16_t be_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}
static inline int16_t be_s16(const uint8_t *p) { return (int16_t)be_u16(p); }
static inline uint32_t be_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// SSB64 RELOC internal-pointer slots encode a file-relative byte offset in
// the low 16 bits, in u32-word units (high 16 bits are the reloc chain link).
static inline uint32_t relocPtrToFileOff(const uint8_t *p) {
    return (be_u32(p) & 0xFFFFu) * 4u;
}

// TMEM odd-row un-swizzle for 16bpp strips lives in src/shared/rgba16_swizzle.h
// (UnswizzleRgba16Strip) — see docs/bugs/sprite_texel_tmem_swizzle_2026-04-10.md.

// Decode the stage's o2r wallpaper Sprite into the final full-image RGBA16 BE
// buffer (se.bg_w x se.bg_h, malloc'd — same contract as loadPNGAsRGBA16BE).
// The unswizzled strip rows already ARE the target format, so the background
// is a straight memcpy stitch of each strip's rendered rows — byte-identity
// with the source texels is structural, not a property of a conversion
// round-trip. Returns nullptr on any validation failure.
// Load the stage's reloc file resource and return its decoded bytes.
// Returns nullptr on any failure (missing ctx/resource, wrong resource type).
static std::shared_ptr<RelocFile> loadWallpaperRelocFile(const StageEntry &se) {
    auto ctx = Ship::Context::GetInstance();
    if (!ctx) {
        return nullptr;
    }
    auto resource = ctx->GetResourceManager()->LoadResource(
        std::string(gRelocFileTable[se.wp_file_id]));
    if (!resource) {
        port_log("CSS stage assets: %s — reloc file %u (%s) not in archive\n",
                 se.name, se.wp_file_id, gRelocFileTable[se.wp_file_id]);
        return nullptr;
    }
    // Guarded downcast, matching lbreloc_bridge.cpp: a shadowing archive (mod
    // o2r, fromsource o2r) could serve this path as a different resource type.
    auto relocFile = std::dynamic_pointer_cast<RelocFile>(resource);
    if (!relocFile) {
        port_log("CSS stage assets: %s — %s is not a RelocFile resource; skipping derivation\n",
                 se.name, gRelocFileTable[se.wp_file_id]);
        return nullptr;
    }
    return relocFile;
}

struct WallpaperSpriteHeader {
    int w;
    int h;
    int nbitmaps;
    int bm_h;
    uint8_t fmt;
    uint8_t siz;
};

// Parse + validate the Sprite header at wp_sprite_off against the stage's
// expected geometry. Returns false (and logs) on any mismatch.
static bool parseWallpaperHeader(const StageEntry &se, const std::vector<uint8_t> &data,
                                 WallpaperSpriteHeader *hdr, const uint8_t **sp) {
    const size_t so = se.wp_sprite_off;
    if (so + 68 > data.size()) {
        port_log("CSS stage assets: %s — sprite offset 0x%zX out of range (file %zu bytes)\n",
                 se.name, so, data.size());
        return false;
    }
    *sp = data.data() + so;
    hdr->w        = be_s16(*sp + 0x04);
    hdr->h        = be_s16(*sp + 0x06);
    hdr->nbitmaps = be_s16(*sp + 0x28);
    hdr->bm_h     = be_s16(*sp + 0x2C);
    hdr->fmt      = (*sp)[0x30];
    hdr->siz      = (*sp)[0x31];

    if (hdr->w != se.bg_w || hdr->h != se.bg_h || hdr->nbitmaps != se.bg_nbitmaps ||
        hdr->bm_h != se.bg_bm_h || hdr->fmt != kFmtRGBA16 || hdr->siz != kSiz16b) {
        port_log("CSS stage assets: %s — wallpaper sprite geometry mismatch "
                 "(%dx%d nbm=%d bmh=%d fmt=%d siz=%d); skipping derivation\n",
                 se.name, hdr->w, hdr->h, hdr->nbitmaps, hdr->bm_h, hdr->fmt, hdr->siz);
        return false;
    }
    return true;
}

// Unswizzle + copy one bitmap strip's rendered rows into out[*out_off].
// Returns false (and logs) on any range/geometry error.
static bool stitchWallpaperStrip(const StageEntry &se, const std::vector<uint8_t> &data,
                                 int i, uint32_t bm_array, const WallpaperSpriteHeader &hdr,
                                 uint8_t *out, size_t out_bytes, size_t *out_off) {
    const size_t bm_off = (size_t)bm_array + (size_t)i * 16;
    if (bm_off + 16 > data.size()) {
        return false;
    }
    const uint8_t *bm = data.data() + bm_off;
    const int bm_w    = be_s16(bm + 0x00);
    const int bm_rows = be_s16(bm + 0x0C); // actualHeight (rendered + fringe)
    const uint32_t buf_off = relocPtrToFileOff(bm + 0x08);

    const size_t strip_bytes = (size_t)bm_w * (size_t)bm_rows * 2u;
    const int rendered = std::min(hdr.bm_h, bm_rows);
    const size_t rendered_bytes = (size_t)bm_w * (size_t)rendered * 2u;
    if (bm_w != hdr.w || bm_rows <= 0 ||
        (size_t)buf_off + strip_bytes > data.size() ||
        *out_off + rendered_bytes > out_bytes) {
        port_log("CSS stage assets: %s — bitmap[%d] out of range/mismatch\n",
                 se.name, i);
        return false;
    }

    std::vector<uint8_t> strip(data.data() + buf_off, data.data() + buf_off + strip_bytes);
    UnswizzleRgba16Strip(strip.data(), bm_w, bm_rows);

    std::memcpy(out + *out_off, strip.data(), rendered_bytes);
    *out_off += rendered_bytes;
    return true;
}

// Decode the stage's o2r wallpaper Sprite into the final full-image RGBA16 BE
// buffer (se.bg_w x se.bg_h, malloc'd — same contract as loadPNGAsRGBA16BE).
// The unswizzled strip rows already ARE the target format, so the background
// is a straight memcpy stitch of each strip's rendered rows — byte-identity
// with the source texels is structural, not a property of a conversion
// round-trip. Returns nullptr on any validation failure.
static uint8_t *deriveWallpaperRGBA16(const StageEntry &se) {
    if (se.wp_file_id >= RELOC_FILE_COUNT) {
        return nullptr;
    }
    auto relocFile = loadWallpaperRelocFile(se);
    if (!relocFile) {
        return nullptr;
    }
    const std::vector<uint8_t> &data = relocFile->Data;

    WallpaperSpriteHeader hdr;
    const uint8_t *sp = nullptr;
    if (!parseWallpaperHeader(se, data, &hdr, &sp)) {
        return nullptr;
    }

    const uint32_t bm_array = relocPtrToFileOff(sp + 0x34);
    const size_t out_bytes = (size_t)hdr.w * hdr.h * 2u;
    uint8_t *out = static_cast<uint8_t *>(std::malloc(out_bytes));
    if (!out) {
        return nullptr;
    }
    size_t out_off = 0;

    for (int i = 0; i < hdr.nbitmaps; ++i) {
        if (!stitchWallpaperStrip(se, data, i, bm_array, hdr, out, out_bytes, &out_off)) {
            std::free(out);
            return nullptr;
        }
    }

    if (out_off != out_bytes) {
        port_log("CSS stage assets: %s — assembled %zu bytes, expected %zu\n",
                 se.name, out_off, out_bytes);
        std::free(out);
        return nullptr;
    }
    return out;
}

// Expand an RGBA16 BE buffer to RGBA8888 (for the bilinear icon filter).
static std::vector<uint8_t> expandRGBA16BEToRGBA8888(const uint8_t *rgba16, size_t npixels) {
    std::vector<uint8_t> out;
    out.reserve(npixels * 4);
    for (size_t i = 0; i < npixels; ++i) {
        const uint16_t word =
            (uint16_t)((uint16_t)rgba16[i * 2] << 8 | rgba16[i * 2 + 1]);
        const uint8_t r5 = (uint8_t)((word >> 11) & 0x1F);
        const uint8_t g5 = (uint8_t)((word >> 6) & 0x1F);
        const uint8_t b5 = (uint8_t)((word >> 1) & 0x1F);
        out.push_back((uint8_t)((r5 << 3) | (r5 >> 2)));
        out.push_back((uint8_t)((g5 << 3) | (g5 >> 2)));
        out.push_back((uint8_t)((b5 << 3) | (b5 >> 2)));
        out.push_back((word & 1) ? 255 : 0);
    }
    return out;
}

// Center-crop to the icon aspect ratio, then bilinear-downscale to 48x36.
// The filter lives in src/shared/icon_scale.h so the Android runtime
// deriver (android_torch_bridge.cpp) produces identical pixels.
static std::vector<uint8_t> downscaleIconBilinear(const std::vector<uint8_t> &src,
                                                  int src_w, int src_h) {
    return ssb64::IconDownscaleBilinear(src.data(), src_w, src_h, kIconW, kIconH);
}

// Derive the background or icon pixel buffer (RGBA16 BE, malloc'd — same
// contract as loadPNGAsRGBA16BE) from the o2r wallpaper sprite.
static uint8_t *deriveFromArchive(const StageEntry &se, AssetKind asset_kind) {
    uint8_t *rgba16 = deriveWallpaperRGBA16(se);
    if (!rgba16 || asset_kind == kBackground) {
        return rgba16;
    }
    const std::vector<uint8_t> rgba8 =
        expandRGBA16BEToRGBA8888(rgba16, (size_t)se.bg_w * se.bg_h);
    std::free(rgba16);
    const std::vector<uint8_t> icon8 = downscaleIconBilinear(rgba8, se.bg_w, se.bg_h);
    return convertToRGBA16BE(icon8.data(), kIconW, kIconH);
}

// ---------------------------------------------------------------------------
// PNG override lookup.
//
// ssb64::LocateExistingFile probes the LUS app directory first (SDL pref
// path on NON_PORTABLE release builds, cwd on portable ones), then the real
// executable/bundle directory (where the CMake POST_BUILD step stages
// dev-build PNGs, and where a portable build launched from another cwd keeps
// its files). Returns "" when the PNG exists nowhere, which routes
// background/icon requests to the o2r derivation above.
// ---------------------------------------------------------------------------

static std::string locateCssAssetPNG(const std::string &rel_path) {
    return ssb64::LocateExistingFile(rel_path);
}

} // namespace

namespace ssb64::css {

CacheEntry  sCache[kStageCount][kAssetKindCount];
std::mutex  sCacheMutex;

// ---------------------------------------------------------------------------
// Core getter implementation.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// getSprite helpers
// ---------------------------------------------------------------------------

struct SpritePlan {
    const char *suffix;
    int expected_w;
    int expected_h;
};

static SpritePlan spritePlanFor(AssetKind asset_kind, const StageEntry &se) {
    if (asset_kind == kBackground) {
        return { "_background.png", se.bg_w, se.bg_h };
    }
    if (asset_kind == kName) {
        return { "_name.png", kNameW, kNameH };
    }
    if (asset_kind == kEmblem) {
        return { "_emblem.png", kEmblemW, kEmblemH };
    }
    return { "_small.png", kIconW, kIconH };
}

// A PNG override exists on disk: it is authoritative. If it fails to load
// (corrupt, wrong size — already logged above), return NULL so the question
// mark surfaces the broken override instead of silently substituting the
// ROM-derived image under a modder's art.
static uint8_t *pixelsFromPng(AssetKind asset_kind, const std::string &full_path,
                              const SpritePlan &plan) {
    return (asset_kind == kEmblem)
               ? loadPNGAsIA4(full_path, plan.expected_w, plan.expected_h)
               : loadPNGAsRGBA16BE(full_path, plan.expected_w, plan.expected_h);
}

// No PNG anywhere (the normal case on release builds, which cannot ship
// ROM-derived files) — derive from the o2r wallpaper sprite, or fall back to
// the compiled-in nameplate. Returns nullptr when nothing can be produced.
static uint8_t *pixelsFromFallback(const StageEntry &se, AssetKind asset_kind,
                                   CacheEntry &entry, std::string *full_path) {
    if ((asset_kind == kBackground || asset_kind == kIcon) &&
        !entry.derive_failed) {
        uint8_t *pixels = deriveFromArchive(se, asset_kind);
        if (pixels) {
            *full_path = std::string(gRelocFileTable[se.wp_file_id]) + " (o2r-derived)";
            return pixels;
        }
        entry.derive_failed = true;
        return nullptr;
    }
    if (asset_kind == kName && se.baked_name != nullptr) {
        // No PNG override — use the compiled-in nameplate (rendered from a
        // text string at build time; the normal case on release builds).
        // Heap copy so the cache owns a mutable, high-VA buffer like every
        // other pixel source.
        const size_t nbytes = (size_t)kNameW * kNameH * 2;
        uint8_t *pixels = static_cast<uint8_t *>(std::malloc(nbytes));
        if (pixels) {
            std::memcpy(pixels, se.baked_name, nbytes);
            *full_path = std::string(se.name) + "_name (baked)";
        }
        return pixels;
    }
    return nullptr;
}

static Sprite *buildSpriteFor(AssetKind asset_kind, const StageEntry &se,
                              uint8_t *pixels, CacheEntry *entry) {
    if (asset_kind == kBackground) {
        return buildBackgroundSprite(se, pixels, entry);
    }
    if (asset_kind == kName) {
        return buildNameSprite(pixels, entry);
    }
    if (asset_kind == kEmblem) {
        return buildEmblemSprite(pixels, entry);
    }
    return buildIconSprite(pixels, entry);
}

Sprite *getSprite(int gkind, AssetKind asset_kind) {
    // Find the stage table entry.
    int stage_idx = -1;
    for (int i = 0; i < kStageCount; ++i) {
        if (STAGE_TABLE[i].gkind == gkind) {
            stage_idx = i;
            break;
        }
    }
    if (stage_idx < 0) return nullptr;  // gkind not in table

    std::lock_guard<std::mutex> guard(sCacheMutex);

    CacheEntry &entry = sCache[stage_idx][asset_kind];

    if (entry.sprite) {
        // Cache hit: re-register for scene-boundary safety.
        reregisterEntry(entry);
        return entry.sprite;
    }

    // Cache miss: load the PNG.
    const StageEntry &se = STAGE_TABLE[stage_idx];

    // For nameplate/emblem sprites: return NULL immediately if this stage has no PNG.
    if (asset_kind == kName && !se.has_name_png) {
        return nullptr;
    }
    if (asset_kind == kEmblem && !se.has_emblem_png) {
        return nullptr;
    }

    const SpritePlan plan = spritePlanFor(asset_kind, se);

    std::string rel_path = std::string("assets/css_icons/") + se.name + plan.suffix;
    std::string full_path = locateCssAssetPNG(rel_path);

    // Emblems use the ROM's IA4 single-bitmap format (alpha-mask source); all
    // other CSS sprites use RGBA16. Pick the converter accordingly.
    uint8_t *pixels = nullptr;
    if (!full_path.empty()) {
        pixels = pixelsFromPng(asset_kind, full_path, plan);
    } else {
        pixels = pixelsFromFallback(se, asset_kind, entry, &full_path);
    }
    if (!pixels) {
        // Nothing to load or derive — return NULL so caller falls back to
        // the ROM sprite / question mark / subtitle-font text.
        return nullptr;
    }

    Sprite *sp = buildSpriteFor(asset_kind, se, pixels, &entry);

    if (!sp) {
        std::free(pixels);
        port_log("CSS stage assets: sprite build failed for %s\n", full_path.c_str());
        return nullptr;
    }

    port_log("CSS stage assets: loaded %s (%dx%d, %d bitmaps)\n",
             full_path.c_str(), plan.expected_w, plan.expected_h, entry.nbitmaps);
    return sp;
}

} // namespace ssb64::css

#endif /* PORT */
