#pragma once

/* RFC 4648 base64 encoder — standard alphabet, '=' padded. Header-only;
 * shared by the websocket handshake and the replay uploader. */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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

/* RFC 4648 base64 decoder — ignores ASCII whitespace, stops at the '='
 * pad, rejects any other out-of-alphabet byte. Mirrors Base64Encode. */
namespace detail {
inline int B64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
} // namespace detail

inline bool Base64Decode(const std::string& in, std::vector<uint8_t>& out) {
    out.clear();
    int acc = 0, bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = detail::B64Value(c);
        if (v < 0) {
            if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
            return false;
        }
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

} // namespace ssb64
