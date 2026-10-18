/* atomic_write.h — write a whole file atomically.
 *
 * Persistence sinks (netstats.json, netfriends.json, saves) must never
 * leave a half-written file behind: a crash or kill mid-write would
 * otherwise corrupt the store and the next load() silently discards it.
 * This helper serializes to <path>.tmp first and renames over the
 * target, so a failure can only strand a stale .tmp — never a torn
 * <path>. Note: Windows rename() refuses to replace an existing target,
 * so the fallback remove+rename runs on that platform; the window is
 * microseconds versus the whole-write window it replaces. */
#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace ssb64 {

inline bool WriteFileAtomic(const std::filesystem::path& path,
                            const std::string& data) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
        out << data;
        out.close();
        if (!out) {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {                               // target exists (Windows)
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
    }
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace ssb64
