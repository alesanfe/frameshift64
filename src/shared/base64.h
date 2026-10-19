#pragma once

/* RFC 4648 base64 encoder — standard alphabet, '=' padded. Header-only;
 * shared by the websocket handshake and the replay uploader. */

#include <cstddef>
#include <string>

namespace ssb64 {

inline std::string Base64Encode(const void* data, size_t size) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::string out;
    out.reserve(size * 4 / 3 + 4);
    for (size_t i = 0; i < size; i += 3) {
        const unsigned int v = (static_cast<unsigned int>(bytes[i]) << 16) |
                               (i + 1 < size ? static_cast<unsigned int>(bytes[i + 1]) << 8 : 0) |
                               (i + 2 < size ? static_cast<unsigned int>(bytes[i + 2]) : 0);
        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += (i + 1 < size) ? kTable[(v >> 6) & 63] : '=';
        out += (i + 2 < size) ? kTable[v & 63] : '=';
    }
    return out;
}

} // namespace ssb64
