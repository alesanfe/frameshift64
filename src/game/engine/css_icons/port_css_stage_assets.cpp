/** * port_css_stage_assets.cpp — Generic CSS stage asset loader (port-only).
 *
 * Loads full-resolution CSS wallpaper PNGs and 48x36 icon PNGs that were
 * extracted from baserom.us.z64 at CMake build time by
 * tools/derive_stage_assets.py, converts them to RGBA16 BE (the format the
 * Fast3D renderer expects for N64 sprite textures), and wraps each in a
 * synthetic Sprite + Bitmap array.
 *
 * The PNGs live at runtime under:
 *   <app-data>/assets/css_icons/<name>_background.png
 *   <app-data>/assets/css_icons/<name>_small.png
 *   <app-data>/assets/css_icons/<name>_name.png   (optional — see below)
 *
 * PNGs are only present on dev builds (CMake derives them from the baserom)
 * or when a user drops in custom art — shipped release builds contain no
 * ROM-derived files, and the packaging scripts never bundle css_icons
 * (docs/bugs/desktop_extra_stage_css_icons_2026-08-31.md). So for the
 * background and icon sprites there is a second source: the wallpaper
 * Sprite inside the stage's reloc file in the user's own extracted
 * BattleShip.o2r, parsed and downscaled at runtime. Lookup order:
 *   1. <app-data>/assets/css_icons/<name><suffix>.png   (moddable override)
 *   2. same path relative to the real app bundle dir (portable builds)
 *   3. derive from the o2r wallpaper sprite (background + icon only) —
 *      but only when no PNG exists at all: a PNG that exists and fails to
 *      load is a broken override, surfaced as the question-mark fallback
 *      rather than silently papered over with the ROM-derived image.
 *
 * Nameplates have no o2r source (they are synthesized text, no ROM sprite
 * exists) — instead they fall back to nameplate pixels BAKED INTO THE
 * BINARY (generated from text strings at build time — no ROM data — see
 * tools/render_nameplates.py and the CMake nameplate commands), so release
 * plates are pixel-identical to dev-build PNG plates. Emblems stay
 * PNG-only (and are currently skipped by mnMapsMakeEmblem anyway).
 *
 * Sprite construction mirrors the old port_css_fd_background.cpp /
 * port_css_fd_icon.cpp pattern:
 *   - Background: 300x220, 44 bitmaps x 5 rows each (bmheight=5, bmHreal=6),
 *                 ndisplist=552, RGBA16.
 *   - Icon: 48x36, 1 bitmap, ndisplist=36, RGBA16.
 *   - Name: 96x10, 1 bitmap, ndisplist=10, RGBA16.
 *     Dimensions match ROM IA4 nameplate sprites (MNMaps/file 30).
 *     Only port-introduced stages with a synthesized nameplate PNG return
 *     non-NULL; ROM-resident stages return NULL so caller uses ROM sprite.
 * These constants are derived from dStageLastBackground_0x26c88 (ROM-verified
 * in the old port_css_fd_background.h header comment).
 *
 * Adding a new stage: add one entry to STAGE_TABLE in
 * css_stage_assets_internal.h.
 *
 * Split: stage table + shared types live in css_stage_assets_internal.h;
 * sprite construction lives in css_stage_build.cpp.
 */

#ifdef PORT

#include "port_css_stage_assets.h"
#include "css_stage_assets_internal.h"

using namespace ssb64::css;

// ---------------------------------------------------------------------------

extern "C" Sprite *portCSSGetStageBackgroundSprite(int gkind) {
    return getSprite(gkind, kBackground);
}

extern "C" Sprite *portCSSGetStageIconSprite(int gkind) {
    return getSprite(gkind, kIcon);
}

extern "C" Sprite *portCSSGetStageNameSprite(int gkind) {
    return getSprite(gkind, kName);
}

extern "C" Sprite *portCSSGetStageEmblemSprite(int gkind) {
    return getSprite(gkind, kEmblem);
}

#endif /* PORT */
