# Fast3D Backend Audit: Unimplemented / Stubbed RDP/RSP Features
**Date:** 2026-04-28  
**Scope:** `libultraship/src/fast/` — N64 RDP/RSP features that are stubbed or incomplete  
**Impact Focus:** Misrendering bugs specific to SSB64 (vs. crashes)

## Executive Summary

Fast3D bundles Emill's emulator-grade re-implementation, which stubs several N64 RDP/RSP features that most ROMs don't use. SSB64 exercises several of these:

1. **✓ FIXED (2026-04-25):** `gDPSetPrimDepth` (G_ZS_PRIM) — sprites rendered at z=0 instead of intended depth
2. **✓ FIXED (2026-04-20):** Color-image-to-Z-buffer redirect in `TextureRectangle` and `FillRectangle`  
3. **⚠ PARTIALLY GUARDED:** Color-image redirect in `GfxSpTri1` — NOW FIXED, includes depth-test remapping
4. **✓ FIXED (2026-10-14):** YUV texture format — `ImportTextureYuv` decodes YUV16 4:2:2 → RGBA8 with the BT.601 matrix
5. **✓ FIXED (2026-10-14):** Background scaling — `Gfxs2dexBg1cyc` now uses `scaleW`/`scaleH` as `dsdx`/`dtdy` and draws `frameX..frameX+frameW`
6. **✓ ALREADY FIXED:** Direct3D11 `MIRROR|CLAMP` → `D3D11_TEXTURE_ADDRESS_MIRROR_ONCE` (present in `gfx_cm_to_d3d11`; the audit excerpted an older revision)
7. **⚠ LOW-RISK STUBS (correct no-ops):** RDP sync commands (`RDPLOADSYNC`, `RDPPIPESYNC`, `RDPTILESYNC`, `RDPFULLSYNC`) — the interpreter is synchronous, no pipeline to drain. `G_SETCONVERT` now stores K0-K5 in `mRdp->convert_k`
8. **✓ COMPLETE:** Fog, color combiners (TEXEL0/TEXEL1/COMBINED/ENVIRONMENT/NOISE), render modes, Z-depth

## Detailed Findings

### 1. YUV Texture Format — FIXED (2026-10-14)

**File:** `libultraship/src/fast/interpreter.cpp` — `Interpreter::ImportTextureYuv`

**Status:** Implemented. `G_IM_FMT_YUV`/`G_IM_SIZ_16b` decodes YUV16 4:2:2
(even texels `{Y,Cb}`, odd texels `{Y,Cr}`) to RGBA8 with the BT.601 matrix —
the hardware default that corresponds to the RDP's TF-step color conversion.
This matches what the color combiner receives on console: SSB64's sprite path
(`lbcommon.c:2881+`) selects `G_CC_DECALRGBA` for `bmfmt == G_IM_FMT_YUV` and
never issues `gDPSetConvert`, so TEXEL0 arrives at the CC already RGB.
Decoding in the importer (rather than carrying YUV through the shader) is
therefore correct — the `G_CC_1CYUV2RGB`/`G_CC_YUV2RGB` combiner macros exist
in gbi.h but no SSB64 call site uses them.

The importer mirrors `ImportTextureRgba16`: same effective-line-size,
naturalWidth stride reassignment, tile `lrs/lrt` clamping, masks/maskt wrap
bounds, hi-res hook (`G_IM_FMT_YUV`/`G_IM_SIZ_16b` args), and upload path.
Chroma for the odd-width row edge falls back to neutral (`Cr = 0`) rather than
reading the next row.

**Remaining limitation:** custom `G_SETCONVERT` K0-K5 matrices are parsed and
stored (`mRdp->convert_k`) but not applied — the decode always uses BT.601.
No known SSB64 content sets custom constants.

---

### 2. Color-Image Redirect Guard in GfxSpTri1 — NOW PROPERLY IMPLEMENTED

**File:** `/Users/jackrickey/Dev/ssb64-port/libultraship/src/fast/interpreter.cpp:2381-2385`

**Code:**
```cpp
bool redirect_active = mRdp->color_image_address == mRdp->z_buf_address && mRdp->color_image_address != nullptr;
if (redirect_active) {
    depth_test = (mRdp->other_mode_l & Z_CMP) == Z_CMP;
    depth_mask = (mRdp->other_mode_l & Z_UPD) == Z_UPD;
}
```

**Status:** FIXED (but worth confirming the fix is working as intended)

**GBI Feature:** `gDPSetColorImage(gSYVideoZBuffer)` + triangle draw redirection

**Risk:** SHIPPED-BUG-LIKELY (but now guarded)

**SSB64 Callsites:**
- `decomp/src/mv/mvopening/mvopeningroom.c:1095+1100` — `mvOpeningRoomTransitionOverlayProcDisplay` (now PORT-gated, never executes)
- `decomp/src/mv/mvopening/mvopeningroom.c:1114+1118+1124` — `mvOpeningRoomTransitionOutlineProcDisplay` (redirects, draws triangle + fill)

**Context:** The opening-scene transition uses this N64 trick to write depth-mask values into the Z-buffer via triangle draws, which Fast3D cannot emulate because it doesn't actually redirect the color target. The guard remaps depth-test logic when the redirect is active: instead of reading depth-test from `G_ZBUFFER` geometry mode, it reads from `Z_CMP` in the render mode — matching real N64 semantics where Z-buffer writes happen but Z-compare is separate.

**Current Status:**
- **Overlay draw (line 1100):** Now PORT-gated to return early; not exercised.
- **Outline draw (line 1124):** Attempts to draw a silhouette mesh to Z-buffer. The guard exists but the symptom (if it regresses) would be: **the outlined character silhouette becomes invisible or renders as an opaque shape** because depth-test incorrectly rejects the tris against stale Z-buffer content.

**Verification:** The guard is present and the depth-test remapping logic is correct. However, the motivating comment (lines 2370-2379) is worth re-reading to confirm the intent matches the implementation.

---

### 3. Background Scaling — FIXED (2026-10-14)

**File:** `libultraship/src/fast/interpreter.cpp` — `Interpreter::Gfxs2dexBg1cyc`

**Status:** Implemented. `scaleW`/`scaleH` (u5.10 texels-per-pixel in
`F3DuObjScaleBg_t`) now feed `dsdx`/`dtdy` directly — same fixed-point units
the texture rectangle expects (s5.10). `scaleW == 0` falls back to identity
per the S2DEX "unscaled" convention. The frame rect was also corrected:
`lrx/lry` are `frameX + frameW`/`frameY + frameH` (previously `frameW`/`frameH`
as absolute coords — identical only when `frameX == frameY == 0`, which is
why full-screen BGs rendered fine). `G_BG_FLAG_FLIPS` still negates `dsdx`.

**Remaining limitation:** the rolling `imageYorig` window (s20.5) is not used;
`ult` still derives from `imageY`. Correct for non-scrolling backgrounds;
a game using the S2DEX scroll-window feature would need per-row `ult`.

---

### 4. Direct3D11 Texture Wrap Mode — ALREADY FIXED

**File:** `libultraship/src/fast/backends/gfx_direct3d11.cpp` — `gfx_cm_to_d3d11` (~line 591)

**Status:** Verified fixed — the audit excerpted an older revision. Current code:
```cpp
static D3D11_TEXTURE_ADDRESS_MODE gfx_cm_to_d3d11(uint32_t val) {
    if ((val & G_TX_MIRROR) && (val & G_TX_CLAMP)) {
        return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    }
    if (val & G_TX_CLAMP) {
        return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
    return (val & G_TX_MIRROR) ? D3D11_TEXTURE_ADDRESS_MIRROR : D3D11_TEXTURE_ADDRESS_WRAP;
}
```

`G_TX_MIRROR | G_TX_CLAMP` → `MIRROR_ONCE` matches `GL_MIRROR_CLAMP_TO_EDGE`
(OpenGL) and `MTLSamplerAddressModeMirrorOnce` (Metal). Backend parity
confirmed; no remaining work.

---

### 5. RDP Sync Command Stubs — LOW RISK

**File:** `/Users/jackrickey/Dev/ssb64-port/libultraship/src/fast/interpreter.cpp:5383-5386, 5389`

**Commands:** `RDP_G_RDPLOADSYNC`, `RDP_G_RDPPIPESYNC`, `RDP_G_RDPTILESYNC`, `RDP_G_RDPFULLSYNC`, `RDP_G_SETCONVERT`

**Code:**
```cpp
{ RDP_G_RDPLOADSYNC, { "mRdpLOADSYNC", gfx_stubbed_command_handler } },
{ RDP_G_RDPPIPESYNC, { "mRdpPIPESYNC", gfx_stubbed_command_handler } },
{ RDP_G_RDPTILESYNC, { "mRdpTILESYNC", gfx_stubbed_command_handler } },
{ RDP_G_RDPFULLSYNC, { "mRdpFULLSYNC", gfx_stubbed_command_handler } },
{ RDP_G_SETCONVERT, { "G_SETCONVERT", gfx_stubbed_command_handler } },
```

**GBI Feature:** RDP pipeline synchronization; YUV→RGB color-space conversion setup

**Risk:** LOW (these are mostly pipeline management; emulators treat them as no-ops safely)

**SSB64 Usage:** Not confirmed in codebase search; unlikely to affect visible output if SSB64 doesn't rely on precise RDP pipeline stalls.

**Status (2026-10-14):** The four `*SYNC` commands remain intentional no-ops — Fast3D processes commands in order on a synchronous interpreter, so there is no RDP pipeline to drain; this is correct behavior, not a gap. `G_SETCONVERT` now has a real handler (`gfx_set_convert_handler_rdp`) that decodes and stores the six s9 constants into `mRdp->convert_k` / `mRdp->convert_constants_set`. They are not yet consumed by the YUV importer, which always uses the BT.601 default matrix — acceptable because no known SSB64 content issues `gDPSetConvert`.

---

### 6. Culling Display List Handler — FIXED (2026-10-14)

**File:** `libultraship/src/fast/interpreter.cpp` — `gfx_cull_dl_handler_f3dex2`

**Status:** Implemented. `G_CULLDL` (`gSPCullDisplayList(v0, vn)`) is a
conditional call: the sub-display-list at `w1` executes only when the vertex
range `[v0, vn]` is not trivially rejected. Rejection reuses the per-vertex
`clip_rej` flags already computed in `GfxSpVertex` — the AND across the range
is non-zero iff every vertex lies outside the same clip half-space, matching
RSP semantics. Range indices are stored doubled in `w0`; both are clamped to
`MAX_VERTICES + 4` before use. Culled = `SegAddr`'d sub-DL is never pushed
and execution continues at the next command.

**SSB64 Usage:** No grep hits for `gSPCullDisplayList` in `decomp/src/`; asset
DLs may still emit it. This is primarily a correctness/perf improvement for
modded or uncommon content.

---

### 7. G_ZS_PRIM Depth Source (Primitive Depth) — ✓ FIXED

**File:** `/Users/jackrickey/Dev/ssb64-port/libultraship/src/fast/interpreter.cpp:2647-2658, 4639-4640`

**Status:** COMPLETE and VERIFIED (2026-04-25 fix)

**GBI Feature:** `gDPSetPrimDepth(z, dz)` + `G_ZS_PRIM` depth source

**SSB64 Callsites:**
- Every 2D sprite using `SP_Z` flag in decomp/src/libultra/sp/sprite.c:207
- Portrait/card overlays in fighter description scenes
- mvOpeningRoom wallpaper depth setup

**Verification:** The fix properly stores `prim_depth_z` in the handler and applies it in `GfxSpTri1` lines 2654-2658, with correct NDC mapping for both Metal ([0,1]) and OpenGL ([-1,1]).

---

### 8. Render Modes & Z-Buffer Modes — ✓ COMPLETE

**Files:** `interpreter.cpp` lines 2369, 2383-2384, 2393

**Status:** Fully implemented

**Features Verified:**
- `Z_UPD` (Z-buffer write enable)
- `Z_CMP` (Z-buffer compare enable)  
- `ZMODE_DEC` (Z-decal mode)
- `G_RM_*` render-mode combinations (blend src/dst, coverage)

The color-image redirect logic in `GfxSpTri1` correctly remaps `depth_test` from geometry-mode `G_ZBUFFER` to render-mode `Z_CMP` when redirect is active, which matches N64 hardware semantics.

---

### 9. Color Combiners — ✓ COMPLETE

**File:** `interpreter.cpp` lines 479-624 (color combiner shader generation)

**Status:** Fully implemented

**Features Verified:**
- All primary sources: `G_CCMUX_TEXEL0`, `G_CCMUX_TEXEL1`, `G_CCMUX_COMBINED`, `G_CCMUX_ENVIRONMENT`
- All alpha sources: `G_ACMUX_*` equivalents
- Special modes: `G_CCMUX_NOISE`, `G_CC_1CYUV2RGB`, `G_CC_YUV2RGB`
- Two-cycle mode (`G_CYC_2CYCLE`)

The shader generation correctly maps combiner inputs to shader constants. YUV modes are recognized but will fail if YUV textures are loaded (since the texture itself isn't decoded).

---

### 10. Fog Modes — ✓ COMPLETE

**File:** `interpreter.cpp` lines 2285-2298, 2422-2443, 2805-2816

**Status:** Fully implemented

**Features Verified:**
- `G_FOG` geometry-mode flag
- Fog color + factor computation
- Two blend modes: standard fog and blend-color shroud mode
- Fog factors packed into vertex alpha

---

### 11. TLUT (Texture Lookup Table) Modes — ✓ COMPLETE

**File:** `interpreter.cpp` lines 1757-1765

**Status:** Fully implemented

**Features Verified:**
- TLUT mode detection from `other_mode_h`
- Format override for 4-bit and 8-bit CI (color-indexed) textures when TLUT mode is active
- Proper palette slot selection based on `paletteIndex`

---

### 12. Texture Wrapping & Masking — ✓ COMPLETE (with noted wrap limitation)

**File:** `interpreter.cpp` lines 1116-1124, 1498-1515, 1601-1609

**Status:** Largely complete; OpenGL/Metal full, Direct3D11 missing one combo (see section 4)

**Features Verified:**
- `masks` / `maskt` wrap boundaries properly clamped in texture decode
- Mirror and clamp modes distinguished
- Per-tile wrap state correctly maintained

---

## Summary Table

| Feature | File | Line(s) | Status | Risk | Notes |
|---------|------|---------|--------|------|-------|
| YUV Textures | interpreter.cpp | ~2468 (ImportTextureYuv) | ✓ FIXED 2026-10-14 | — | YUV16→RGBA8 BT.601 at import |
| Color-Image Redirect (Tri) | interpreter.cpp | 2381-2385 | ✓ FIXED | — | Now includes depth-test remap |
| Prim Depth (G_ZS_PRIM) | interpreter.cpp | 4639-4640, 2654-2658 | ✓ FIXED | — | Complete; both Z and NDC mapping |
| BG Scaling | interpreter.cpp | Gfxs2dexBg1cyc | ✓ FIXED 2026-10-14 | — | scaleW/H → dsdx/dtdy |
| D3D11 Mirror\|Clamp | gfx_direct3d11.cpp | ~591 | ✓ FIXED | — | MIRROR_ONCE; parity with GL/Metal |
| RDP Sync Stubs | interpreter.cpp | ~6724 | ✓ N/A | — | Correct no-ops; synchronous interpreter |
| G_SETCONVERT | interpreter.cpp | ~5977 | ✓ PARSED 2026-10-14 | — | K0-K5 stored; not applied to YUV |
| Cull DL | interpreter.cpp | ~5268 | ✓ FIXED 2026-10-14 | — | clip_rej AND over vertex range |
| Fog | interpreter.cpp | 2285+ | ✓ IMPL | — | Full support |
| Color Combiners | interpreter.cpp | 479+ | ✓ IMPL | — | All modes, including YUV-aware |
| Z-Buffer Modes | interpreter.cpp | 2369+ | ✓ IMPL | — | Complete; redirect-aware |
| TLUT | interpreter.cpp | 1757+ | ✓ IMPL | — | Full support |
| Texture Wrap | interpreter.cpp | 1116+ | ✓ IMPL* | — | *D3D11 missing 1 combo |

---

## Risk Assessment for SSB64 Gameplay

### Resolved (2026-10-14)
- **YUV Textures** — decoded at import (BT.601); previously silent load-fail.
- **Background Scaling** — scaleW/H now drive the texture rect step.
- **D3D11 Mirror|Clamp** — MIRROR_ONCE; confirmed parity (was already fixed).
- **Cull DL** — implemented; conditional call via clip_rej intersection.

### Medium (Regression Risk)
- **Color-Image Redirect in GfxSpTri1** — The guard is present and correct, but the mvOpeningRoom transition silhouette is the regression vector if depth-test logic changes.

### Low
- **RDP Sync Stubs** — Correct no-ops; synchronous interpreter.
- **G_SETCONVERT custom matrices** — parsed but not applied; YUV decode is fixed BT.601.

---

## Files Involved

### Primary
- `/Users/jackrickey/Dev/ssb64-port/libultraship/src/fast/interpreter.cpp` — Main RDP/RSP handlers
- `/Users/jackrickey/Dev/ssb64-port/libultraship/src/fast/backends/gfx_direct3d11.cpp` — D3D11 wrap-mode handler
- `/Users/jackrickey/Dev/ssb64-port/libultraship/src/fast/backends/gfx_opengl.cpp` — Reference (correct impl.)
- `/Users/jackrickey/Dev/ssb64-port/libultraship/src/fast/backends/gfx_metal.cpp` — Reference (correct impl.)

### Related Bug Docs
- `docs/bugs/primdepth_unimplemented_2026-04-25.md` — G_ZS_PRIM fix (now resolved)
- `docs/bugs/color_image_to_zbuffer_draws_2026-04-20.md` — Color-image redirect context
- `docs/bugs/whispy_canopy_metal_fallback_texture_2026-04-25.md` — Metal texture binding fix

### Game Source (SSB64 Usage)
- decomp/src/libultra/sp/sprite.c — YUV sprite paths
- `decomp/src/mv/mvopening/mvopeningroom.c` — Color-image redirect + transition tris
- `decomp/src/if/ifcommon.c` — Magnifier bubble (color-image redirect)
- `decomp/src/sys/objdisplay.c` — Z-buffer clear idiom

