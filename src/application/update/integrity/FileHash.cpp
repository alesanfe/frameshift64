/* See FileHash.h. Implementations moved verbatim out of Updater.cpp so the
 * curated mod-pack installer can share the same integrity gate. */
#include "FileHash.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#endif

namespace ssb64 {
namespace enhancements {

namespace {

#if !defined(_WIN32)
/* In-process SHA-256 (public-domain-style, FIPS 180-4). Replaces the
 * sha256sum/shasum shell-out: the OS tools don't exist on Android and
 * the AppImage loader-override strip was fragile on Linux anyway. */
struct Sha256Ctx {
    uint32_t h[8];
    uint8_t  buf[64];
    uint64_t totalLen = 0;
    size_t   bufLen = 0;
};

constexpr uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t ShaRor(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void Sha256Init(Sha256Ctx& c) {
    static const uint32_t kInit[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    std::memcpy(c.h, kInit, sizeof(kInit));
    c.totalLen = 0;
    c.bufLen = 0;
}

void Sha256Block(Sha256Ctx& c, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) |
               (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(p[i * 4 + 2]) << 8) |
               static_cast<uint32_t>(p[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = ShaRor(w[i - 15], 7) ^ ShaRor(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ShaRor(w[i - 2], 17) ^ ShaRor(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
    uint32_t e = c.h[4], f = c.h[5], g = c.h[6], h = c.h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = ShaRor(e, 6) ^ ShaRor(e, 11) ^ ShaRor(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + S1 + ch + kSha256K[i] + w[i];
        const uint32_t S0 = ShaRor(a, 2) ^ ShaRor(a, 13) ^ ShaRor(a, 22);
        const uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        const uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d;
    c.h[4] += e; c.h[5] += f; c.h[6] += g; c.h[7] += h;
}

void Sha256Update(Sha256Ctx& c, const uint8_t* data, size_t len) {
    c.totalLen += len;
    while (len > 0) {
        const size_t take = std::min(len, sizeof(c.buf) - c.bufLen);
        std::memcpy(c.buf + c.bufLen, data, take);
        c.bufLen += take;
        data += take;
        len -= take;
        if (c.bufLen == sizeof(c.buf)) {
            Sha256Block(c, c.buf);
            c.bufLen = 0;
        }
    }
}

void Sha256Final(Sha256Ctx& c, uint8_t out[32]) {
    const uint64_t bitLen = c.totalLen * 8;
    uint8_t pad = 0x80;
    Sha256Update(c, &pad, 1);
    const uint8_t zero = 0;
    while (c.bufLen != 56) {
        Sha256Update(c, &zero, 1);
    }
    uint8_t lenBytes[8];
    for (int i = 0; i < 8; ++i) {
        lenBytes[i] = static_cast<uint8_t>(bitLen >> (56 - i * 8));
    }
    Sha256Update(c, lenBytes, 8);
    for (int i = 0; i < 8; ++i) {
        out[i * 4]     = static_cast<uint8_t>(c.h[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(c.h[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(c.h[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(c.h[i]);
    }
}

#endif // !_WIN32

} // namespace

#if defined(_WIN32)
/* Lowercase hex SHA-256 of a file (Windows CNG), or "" on error. */
std::string Sha256File(const std::string& path) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return hex;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::ifstream in(path, std::ios::binary);
        std::vector<char> buf(1 << 16);
        bool ok = static_cast<bool>(in);
        while (ok && in) {
            in.read(buf.data(), buf.size());
            std::streamsize n = in.gcount();
            if (n > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(n), 0) != 0) {
                ok = false;
            }
        }
        unsigned char digest[32];
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            static const char* kHex = "0123456789abcdef";
            for (unsigned char b : digest) {
                hex += kHex[b >> 4];
                hex += kHex[b & 15];
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}
#else
/* Lowercase hex SHA-256 of a file via the in-process implementation
 * above — same code on Linux, macOS, and Android (no OS tool needed). */
std::string Sha256File(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return {};
    Sha256Ctx ctx;
    Sha256Init(ctx);
    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), buf.size());
        const std::streamsize n = in.gcount();
        if (n > 0) {
            Sha256Update(ctx, reinterpret_cast<const uint8_t*>(buf.data()),
                         static_cast<size_t>(n));
        }
    }
    uint8_t digest[32];
    Sha256Final(ctx, digest);
    std::string hex;
    hex.reserve(64);
    static const char* kHex = "0123456789abcdef";
    for (uint8_t b : digest) {
        hex += kHex[b >> 4];
        hex += kHex[b & 15];
    }
    return hex;
}
#endif // _WIN32

/* AUD-32: size + SHA-256 gate shared by every platform. Before this, only
 * Windows verified the payload — Linux trusted curl's exit code and macOS
 * opened the DMG unchecked, so a truncated/tampered download was installed
 * (Linux) or mounted (macOS). Digest source caveat unchanged: it comes from
 * the same release metadata as the URL, so it checks integrity, not
 * publisher authenticity (see TD-009 for Sigstore/attestation). */
bool DownloadedFileMatches(const std::string& path, long long expectedSize,
                           const std::string& expectedDigest, std::string& error) {
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        const long long actual = in ? static_cast<long long>(in.tellg()) : -1;
        if (actual != expectedSize) {
            error = "download was incomplete.";
            return false;
        }
    }
    if (expectedDigest.rfind("sha256:", 0) == 0 &&
        Sha256File(path) != expectedDigest.substr(7)) {
        error = "download failed verification.";
        return false;
    }
    return true;
}

} // namespace enhancements
} // namespace ssb64
