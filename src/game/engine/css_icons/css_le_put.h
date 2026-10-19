#pragma once

/* Little-endian field writers for synthesized N64 structures (reloc
 * assets carry their scalar fields in host order post-relocation — the
 * runtime byteswapper handles the N64 view). These used to be identical
 * file-local copies in css_stage_build.cpp and port_css_arrow.cpp. */

#include <cstdint>
#include <cstring>

namespace ssb64::css {

inline void put_s16(uint8_t *b, unsigned off, int16_t v) {
    uint16_t u; std::memcpy(&u, &v, 2);
    b[off+0] = (uint8_t)(u & 0xFF); b[off+1] = (uint8_t)(u >> 8);
}
inline void put_u16(uint8_t *b, unsigned off, uint16_t v) {
    b[off+0] = (uint8_t)(v & 0xFF); b[off+1] = (uint8_t)(v >> 8);
}
inline void put_u32(uint8_t *b, unsigned off, uint32_t v) {
    b[off+0] = (uint8_t)(v & 0xFF);
    b[off+1] = (uint8_t)((v >> 8) & 0xFF);
    b[off+2] = (uint8_t)((v >> 16) & 0xFF);
    b[off+3] = (uint8_t)(v >> 24);
}
inline void put_f32(uint8_t *b, unsigned off, float v) {
    uint32_t bits; std::memcpy(&bits, &v, 4); put_u32(b, off, bits);
}

/* Sprite-record head shared by every synthesized css sprite: x/y=0,
 * w×h in texels, 1.0 scale, uv 0, RGBA FF, no tlut, reloc-registered
 * bitmap pointer (callers pass portRelocRegisterPointer(bm) so this
 * header stays reloc-free). Bytes 0x00-0x26. */
inline void put_sprite_head(uint8_t *b, int w, int h, uint32_t bm_reloc) {
    put_s16(b, 0x00, 0);
    put_s16(b, 0x02, 0);
    put_s16(b, 0x04, (int16_t)w);
    put_s16(b, 0x06, (int16_t)h);
    put_f32(b, 0x08, 1.0f);
    put_f32(b, 0x0C, 1.0f);
    put_s16(b, 0x10, 0); put_s16(b, 0x12, 0);
    put_u16(b, 0x14, 0); put_s16(b, 0x16, 0);
    b[0x18] = 0xFF; b[0x19] = 0xFF;
    b[0x1A] = 0xFF; b[0x1B] = 0xFF;
    put_s16(b, 0x1C, 0); put_s16(b, 0x1E, 0);
    put_u32(b, 0x20, bm_reloc);
    put_s16(b, 0x24, 0); put_s16(b, 0x26, 1);
}

} // namespace ssb64::css
