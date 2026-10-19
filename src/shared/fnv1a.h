/* Shared FNV-1a-64 helpers — deterministic across builds/platforms.
 * Used by netplay compat tokens (mod content hashing) and leaderboard
 * match ids; the wire protocol's 32-bit FNV checksum lives in
 * netpacket.h and is intentionally separate (different format). */

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace ssb64 {

inline constexpr uint64_t kFnv1a64Basis = 1469598103934665603ULL;
inline constexpr uint64_t kFnv1a64Prime = 1099511628211ULL;

/* Accumulate `n` bytes into an in-progress hash (start from kFnv1a64Basis). */
inline uint64_t fnv1a64_accum(uint64_t h, const void* data, std::size_t n) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= kFnv1a64Prime;
    }
    return h;
}

inline uint64_t fnv1a64_accum(uint64_t h, const std::string& s) {
    return fnv1a64_accum(h, s.data(), s.size());
}

inline uint64_t fnv1a64(const void* data, std::size_t n) {
    return fnv1a64_accum(kFnv1a64Basis, data, n);
}

inline uint64_t fnv1a64_str(const std::string& s) {
    return fnv1a64(s.data(), s.size());
}

/* Lowercase 16-hex-digit form — used for client-consistent ids (both
 * sides hash the same payload and compare strings). */
inline std::string fnv1a64_hex(const std::string& s) {
    char out[17];
    std::snprintf(out, sizeof(out), "%016llx",
                  static_cast<unsigned long long>(fnv1a64_str(s)));
    return out;
}

} // namespace ssb64
