/* Shared CSS stage-preview icon scaler. The build-time icon pipeline
 * (game/engine/css_icons/css_stage_build.cpp) and the Android runtime
 * deriver (platform/android/android_torch_bridge.cpp) must produce
 * pixel-identical icons — both call this instead of keeping their own
 * copies of the filter. */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ssb64 {

/* Decomp contract: CSS stage preview icons are 48x36 RGBA16 sprites. */
inline constexpr int kCssStageIconW = 48;
inline constexpr int kCssStageIconH = 36;

/* Center-crop `src` (packed RGBA8, src_w x src_h, 4 bytes/px) to the
 * destination aspect, then bilinear-downsample to dst_w x dst_h.
 * Returns the packed RGBA8 result (dst_w*dst_h*4 bytes). */
inline std::vector<uint8_t> IconDownscaleBilinear(const uint8_t *src,
                                                  int src_w, int src_h,
                                                  int dst_w, int dst_h) {
    std::vector<uint8_t> dst((size_t)dst_w * dst_h * 4);
    int crop_x = 0, crop_y = 0, crop_w = src_w, crop_h = src_h;
    const float src_aspect = (float)src_w / (float)src_h;
    const float icon_aspect = (float)dst_w / (float)dst_h;

    if (src_aspect > icon_aspect) {
        crop_w = (int)((float)src_h * icon_aspect);
        crop_x = (src_w - crop_w) / 2;
    } else if (src_aspect < icon_aspect) {
        crop_h = (int)((float)src_w / icon_aspect);
        crop_y = (src_h - crop_h) / 2;
    }

    const float scale_x = (float)crop_w / (float)dst_w;
    const float scale_y = (float)crop_h / (float)dst_h;

    for (int y = 0; y < dst_h; ++y) {
        const float sy = (float)crop_y + ((float)y + 0.5f) * scale_y - 0.5f;
        const int y0 = std::max(0, std::min(src_h - 1, (int)sy));
        const int y1 = std::max(0, std::min(src_h - 1, y0 + 1));
        const float fy = std::max(0.0f, std::min(1.0f, sy - (float)y0));

        for (int x = 0; x < dst_w; ++x) {
            const float sx = (float)crop_x + ((float)x + 0.5f) * scale_x - 0.5f;
            const int x0 = std::max(0, std::min(src_w - 1, (int)sx));
            const int x1 = std::max(0, std::min(src_w - 1, x0 + 1));
            const float fx = std::max(0.0f, std::min(1.0f, sx - (float)x0));

            for (int c = 0; c < 4; ++c) {
                const float p00 = src[((size_t)y0 * src_w + x0) * 4 + c];
                const float p10 = src[((size_t)y0 * src_w + x1) * 4 + c];
                const float p01 = src[((size_t)y1 * src_w + x0) * 4 + c];
                const float p11 = src[((size_t)y1 * src_w + x1) * 4 + c];
                const float top = p00 * (1.0f - fx) + p10 * fx;
                const float bot = p01 * (1.0f - fx) + p11 * fx;
                const float v = top * (1.0f - fy) + bot * fy;
                dst[((size_t)y * dst_w + x) * 4 + c] =
                    (uint8_t)std::max(0.0f, std::min(255.0f, v + 0.5f));
            }
        }
    }
    return dst;
}

} // namespace ssb64
