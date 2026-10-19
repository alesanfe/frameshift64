#pragma once

/* N64 TMEM 4-byte half swap for RGBA16 texture strips: within every
 * 8-byte qword of each odd row, the two 4-byte halves swap. Used by the
 * CSS wallpaper decoder (css_icons) and the Android torch bridge —
 * same artifact of how the RDP lays out 16bpp rows in TMEM.
 * Reference: docs/bugs/sprite_texel_tmem_swizzle_2026-04-10.md */

#include <cstdint>
#include <cstddef>
#include <utility>

inline void UnswizzleRgba16Strip(uint8_t* strip, int width, int height)
{
    const int row_bytes = width * 2;
    for (int row = 1; row < height; row += 2) {
        uint8_t* row_data = strip + (size_t)row * (size_t)row_bytes;
        for (int off = 0; off + 7 < row_bytes; off += 8) {
            std::swap(row_data[off + 0], row_data[off + 4]);
            std::swap(row_data[off + 1], row_data[off + 5]);
            std::swap(row_data[off + 2], row_data[off + 6]);
            std::swap(row_data[off + 3], row_data[off + 7]);
        }
    }
}
