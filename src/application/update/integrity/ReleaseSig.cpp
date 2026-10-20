/* See ReleaseSig.h. */

#include "ReleaseSig.h"
#include "base64.h" // ssb64::Base64Decode — shared RFC 4648 decoder

#include <array>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <ed25519.h>
}

namespace ssb64::enhancements {
namespace {

bool HexDecode(const char* hex, uint8_t* out, size_t out_len) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < out_len; i++) {
        int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* Extract the first base64 body line from a .minisig / .pub file: the line
 * that follows the "untrusted comment:" header. */
bool FirstSigLine(const std::string& text, std::string& line) {
    size_t pos = 0;
    bool comment_seen = false;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        std::string l = text.substr(pos, end == std::string::npos ? end : end - pos);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (comment_seen && !l.empty()) {
            line = l;
            return true;
        }
        if (l.rfind("untrusted comment:", 0) == 0) comment_seen = true;
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return false;
}

} // namespace

bool ReleaseSignatureSupported() {
#ifdef BATTLESHIP_RELEASE_PUBKEY
    return true;
#else
    return false;
#endif
}

bool VerifyReleaseSignature(const std::string& message, const std::string& minisig_text,
                            std::string& error) {
#ifndef BATTLESHIP_RELEASE_PUBKEY
    (void)message; (void)minisig_text;
    error = "this build has no release pubkey embedded";
    return false;
#else
    std::string line;
    if (!FirstSigLine(minisig_text, line)) {
        error = "malformed signature file";
        return false;
    }
    /* minisign SigStruct: sig_alg(2) || keynum(8) || signature(64) = 74
     * bytes. 'Ed' = raw Ed25519 over the content (minisign -l); 'ED' =
     * prehashed (Blake2b) — we sign releases with -l precisely so the
     * verifier only needs plain RFC 8032 Ed25519. */
    std::vector<uint8_t> blob;
    if (!Base64Decode(line, blob) || blob.size() != 74) {
        error = "malformed signature file";
        return false;
    }
    if (blob[0] != 'E' || blob[1] != 'd') {
        error = "unsupported signature mode (release must sign with minisign -l)";
        return false;
    }
    uint8_t pubkey[32];
    if (!HexDecode(BATTLESHIP_RELEASE_PUBKEY, pubkey, sizeof(pubkey))) {
        error = "embedded release pubkey is invalid";
        return false;
    }
    if (ed25519_verify(blob.data() + 10,
                       reinterpret_cast<const uint8_t*>(message.data()), message.size(),
                       pubkey) != 1) {
        error = "release signature verification failed";
        return false;
    }
    return true;
#endif
}

bool VerifyEd25519MinisigFields(const std::string& message,
                                const std::string& signature_b64,
                                const std::string& pubkey_b64,
                                std::string& error) {
    std::vector<uint8_t> sig;
    if (!Base64Decode(signature_b64, sig) || sig.size() != 74) {
        error = "malformed signature (expected base64 SigStruct)";
        return false;
    }
    if (sig[0] != 'E' || sig[1] != 'd') {
        error = "unsupported signature mode (must be minisign -l raw)";
        return false;
    }
    std::vector<uint8_t> pub;
    if (!Base64Decode(pubkey_b64, pub) || pub.size() != 42 ||
        pub[0] != 'E' || pub[1] != 'd') {
        error = "malformed public key (expected base64 PubkeyStruct)";
        return false;
    }
    if (ed25519_verify(sig.data() + 10,
                       reinterpret_cast<const uint8_t*>(message.data()), message.size(),
                       pub.data() + 10) != 1) {
        error = "signature verification failed";
        return false;
    }
    return true;
}

/* First 64 chars are hex and char 64 is a space/tab separator. */
static bool SumsHexPrefix(const std::string& l) {
    size_t sp = l.find_first_of(" \t", 64);
    if (sp != 64) {
        return false;
    }
    for (size_t i = 0; i < 64; i++) {
        char c = l[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

/* Parse one sha256sum-format line ("<64-hex><sp><sp-or-*><name>");
 * return "sha256:<lower-hex>" when its name matches `asset_name`, else "". */
static std::string SumsLineDigest(const std::string& l, const std::string& asset_name) {
    if (l.size() <= 65) {
        return "";
    }
    if (!SumsHexPrefix(l)) {
        return "";
    }
    std::string name = l.substr(65);
    while (!name.empty() && (name[0] == ' ' || name[0] == '\t' || name[0] == '*')) {
        name.erase(0, 1);
    }
    if (name != asset_name) {
        return "";
    }
    std::string dg = l.substr(0, 64);
    for (char& c : dg) c = (char)tolower((unsigned char)c);
    return "sha256:" + dg;
}

std::string SumsDigestForAsset(const std::string& sums_text, const std::string& asset_name) {
    size_t pos = 0;
    while (pos < sums_text.size()) {
        size_t end = sums_text.find('\n', pos);
        std::string l = sums_text.substr(pos, end == std::string::npos ? end : end - pos);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        std::string dg = SumsLineDigest(l, asset_name);
        if (!dg.empty()) {
            return dg;
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return "";
}

} // namespace ssb64::enhancements
