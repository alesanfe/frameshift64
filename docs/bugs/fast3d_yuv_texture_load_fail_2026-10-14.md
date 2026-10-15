# Fast3D YUV Texture Silent Load Fail

**Status:** FIXED — 2026-10-14
**Files:** `components/libultraship/src/fast/interpreter.cpp` (`ImportTextureYuv`, `ImportTexture`), `components/libultraship/include/fast/interpreter.h`
**Source:** `docs/investigations/audit_fast3d_gaps_2026-04-28.md` item 1 (HIGH)

## Symptom

Any sprite whose `Bitmap` data is `G_IM_FMT_YUV` never uploaded a texture:
`ImportTexture` hit `case G_IM_FMT_YUV: SPDLOG_ERROR("YUV Textures not supported")`
and returned, so the draw sampled whatever was previously bound (stale texture,
black quad, or nothing). `bmfmt` arrives from ROM sprite assets, not C code —
no grep hits for the format in `components/decomp/src/` — so the failure surface was
content-driven and only visible as misrendered sprites (wallpapers, menu
imagery, and any `SP_FASTCOPY` YUV bitmap).

## Root cause

Upstream Fast3D never implemented the YUV importer. The correct fix is *not*
a shader-side `YUV2RGB` combiner: on hardware the Texture Filter converts
YUV→RGB during sampling, so by the time the Color Combiner runs, `TEXEL0` is
already RGB. SSB64 confirms this — the sprite path
(`components/decomp/src/lb/lbcommon.c:2881+`) selects `G_CC_DECALRGBA` for
`bmfmt == G_IM_FMT_YUV` (the `default:` case) and never emits
`gDPSetConvert`. The `G_CC_1CYUV2RGB`/`G_CC_YUV2RGB` macros exist in `gbi.h`
but have no SSB64 call sites.

## Fix

`Interpreter::ImportTextureYuv` decodes N64 YUV16 (4:2:2 — even texels are
`{Y,Cb}`, odd texels `{Y,Cr}`) to RGBA8 with the hardware-default conversion,
equivalent to BT.601:

```
R = Y + 1.402·Cr
G = Y - 0.344·Cb - 0.714·Cr
B = Y + 1.772·Cb
```

(in 8.8 fixed point; alpha = 255). The function mirrors `ImportTextureRgba16`:
same effective-line-size logic, naturalWidth DRAM-stride reassignment (the
`sprite_decode_stride_mismatch` class of bug — clamped width must not redefine
the source row stride), tile `lrs/lrt` clamping, `masks`/`maskt` wrap bounds,
hi-res texture-pack hook, and final `UploadTexture`. For the last column of an
odd-width image the missing chroma partner decodes as neutral (`Cr = 0`) rather
than reading into the next row.

`G_SETCONVERT` now also has a real handler (`gfx_set_convert_handler_rdp`) that
parses the six s9 K-constants into `mRdp->convert_k` — parsed but not applied;
the decode always uses the default matrix. No known SSB64 content sets custom
constants.

## Related fixes in the same pass

- **`Gfxs2dexBg1cyc` background scaling** — `dsdx`/`dtdy` were hardcoded to
  `1 << 10` (identity); now driven by `F3DuObjScaleBg_t.scaleW/scaleH` (u5.10,
  the same units the texture rect expects). The rect extent was also fixed to
  `frameX + frameW` / `frameY + frameH` — previously `frameW`/`frameH` were
  used as absolute corners, correct only when `frameX == frameY == 0`.
- **`G_CULLDL`** — implemented as a conditional call: the sub-DL executes only
  when the vertex range `[v0, vn]` is not trivially rejected (AND of
  per-vertex `clip_rej` — nonzero intersection means every vertex shares a
  clip half-space). Indices are stored doubled in `w0`; clamped to
  `MAX_VERTICES + 4`.
- **D3D11 `MIRROR|CLAMP`** — already mapped to `D3D11_TEXTURE_ADDRESS_MIRROR_ONCE`
  (`gfx_cm_to_d3d11`); the audit excerpted a pre-fix revision. GL/Metal/D3D11
  are at parity.

## Audit hook

When a hardware unit (TF) transforms data before the unit a stub reports to
(CC), the fix belongs at the earlier stage — check which combiner the game's
asset actually selects before writing shader-side conversions.
