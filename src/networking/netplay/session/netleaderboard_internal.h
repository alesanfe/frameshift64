#pragma once

/* Internal helpers shared by the netleaderboard TUs (netleaderboard.cpp
 * + netleaderboard_social.cpp). NOT public API — the extern "C" surface
 * lives in netleaderboard.h. Everything here is header-inline so both
 * TUs see the same implementation; the shared lock/state stay single-
 * instance via function-local leaked statics (same lifetime contract as
 * the file's other intentionally-leaked slots — detached worker threads
 * can outlive static destruction). */

#include "netleaderboard.h"
#include "http_curl.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace port_lb {

inline std::mutex& SharedLock()
{
    static std::mutex& m = *new std::mutex;
    return m;
}

/* Mirror of sanitizeName() in services/netplay-lobby/src/domain/
 * leaderboard.mjs — the server strips anything outside its alphabet
 * (letters/digits + " _.-[]()", 24 chars, trimmed) before storing, so
 * we must compare ranks against the *sanitized* name, not the raw
 * profile string (a name with e.g. "?" or emojis would never match a
 * row and "Your rank" would stay hidden). ASCII-only isalnum is the
 * pragmatic subset of the server's \p{L}\p{N} class. */
inline bool lb_name_char(unsigned char u)
{
    return std::isalnum(u) != 0 || std::strchr(" _.-[]()", static_cast<char>(u)) != nullptr;
}

inline std::string sanitize_lb_name(const char *raw)
{
    std::string out;
    for (const char *p = raw != nullptr ? raw : ""; *p != '\0' && out.size() < 24; ++p) {
        const unsigned char u = static_cast<unsigned char>(*p);
        if (lb_name_char(u)) {
            out += static_cast<char>(u);
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

/* "2026-10-19T12:34:56.789Z" -> "10-19 12:34" (UTC, trimmed for the row). */
inline void hist_stamp(const std::string &iso, char *out, size_t cap)
{
    out[0] = '\0';
    if (iso.size() >= 16 && iso[4] == '-' && iso[7] == '-' &&
        (iso[10] == 'T' || iso[10] == ' ')) {
        std::snprintf(out, cap, "%.5s %.5s",
                      iso.c_str() + 5, iso.c_str() + 11);
    }
}

/* Shared JSON member readers — server payloads are treated as hostile,
 * so every read is shape-checked before touching it. */
inline bool json_str(const nlohmann::json &j, const char *k, std::string &out)
{
    const auto it = j.find(k);
    if (it == j.end() || !it->is_string()) {
        return false;
    }
    out = it->get<std::string>();
    return true;
}

inline void json_str_to(const nlohmann::json &j, const char *k,
                        char *dst, size_t cap, size_t max_len)
{
    std::string v;
    if (json_str(j, k, v)) {
        std::snprintf(dst, cap, "%s", v.substr(0, max_len).c_str());
    }
}

inline void json_str_copy(const nlohmann::json &j, const char *k,
                          std::string &dst, size_t cap)
{
    std::string v;
    if (json_str(j, k, v)) {
        dst = v.substr(0, cap);
    }
}

inline int json_int_or(const nlohmann::json &j, const char *k, int dflt)
{
    const auto it = j.find(k);
    return (it != j.end() && it->is_number()) ? it->get<int>() : dflt;
}

/* One verified match row — /history entries and /player.matches share
 * this exact wire shape. */
inline bool parse_match_row(const nlohmann::json &m, PortLbMatchRow &r)
{
    if (!m.is_object()) {
        return false;
    }
    json_str_to(m, "a", r.a, sizeof(r.a), 96);
    json_str_to(m, "b", r.b, sizeof(r.b), 96);
    json_str_to(m, "w", r.w, sizeof(r.w), 96);
    json_str_to(m, "l", r.l, sizeof(r.l), 96);
    r.draw = (m.contains("draw") && m["draw"].is_boolean() &&
              m["draw"].get<bool>()) ? 1 : 0;
    std::string at;
    json_str(m, "at", at);
    hist_stamp(at, r.stamp, sizeof(r.stamp));
    return true;
}

/* Detached-GET skeleton shared by every read-only fetcher: worker
 * thread -> GET -> shape-checked parse -> lock -> store -> state.
 * `parse` fills `out` and returns false to mark the fetch failed. The
 * slots are leaked on purpose (see SharedLock), so references captured
 * by the detached thread stay valid. */
template <typename T, typename Parse>
inline void lb_fetch(const std::string &url, std::atomic<int> &state,
                     T &slot, Parse &&parse)
{
    std::thread([url, &state, &slot, parse] {
        std::string body, error;
        T result;
        int st = 2;
        const nlohmann::json doc = ssb64::CurlGetToString(
            url, &body, &error, {}, 15)
                ? nlohmann::json::parse(body, nullptr, /*exceptions*/ false)
                : nlohmann::json();
        if (doc.is_discarded() || !parse(doc, result)) {
            st = -1;
        }
        std::lock_guard<std::mutex> lk(SharedLock());
        if (st == 2) {
            slot = std::move(result);
        }
        state = st;
    }).detach();
}

} // namespace port_lb
