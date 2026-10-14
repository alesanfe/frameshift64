/* zip_extract.cpp — see header for the contract. libzip is pulled in
 * by libultraship on every platform; linking `zip` here adds no new
 * dependency, it just stops relying on `unzip`/`tar` being on PATH. */
#include "platform/filesystem/zip_extract.h"

#include <zip.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <spdlog/spdlog.h>

namespace ssb64 {
namespace {

/* Zip-slip guard: reject absolute paths and any entry whose normalized
 * relative path escapes the destination root. libzip hands us raw
 * archive names — a hostile archive could otherwise write outside
 * destDir (classic `../../etc/...` traversal). */
bool IsSafeRelPath(const std::string& name) {
    if (name.empty() || name.front() == '/' || name.front() == '\\') {
        return false;
    }
    // Drive-letter or UNC prefixes (Windows-side absolute paths).
    if (name.size() >= 2 && name[1] == ':') {
        return false;
    }
    std::filesystem::path rel(name);
    for (const auto& part : rel) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

} // namespace

bool ZipExtractToDir(const std::filesystem::path& zipPath,
                     const std::filesystem::path& destDir) {
    int err = 0;
    zip_t* za = zip_open(zipPath.string().c_str(), ZIP_RDONLY, &err);
    if (za == nullptr) {
        zip_error_t ze;
        zip_error_init_with_code(&ze, err);
        SPDLOG_ERROR("zip extract: cannot open {}: {}", zipPath.string(),
                     zip_error_strerror(&ze));
        zip_error_fini(&ze);
        return false;
    }

    bool ok = true;
    const zip_int64_t count = zip_get_num_entries(za, 0);
    for (zip_int64_t i = 0; i < count && ok; ++i) {
        zip_stat_t st;
        if (zip_stat_index(za, static_cast<zip_uint64_t>(i), 0, &st) != 0) {
            SPDLOG_ERROR("zip extract: stat failed for entry {}", i);
            ok = false;
            break;
        }
        const std::string name = st.name ? st.name : "";
        // Directory entries end in '/'.
        if (name.empty() || name.back() == '/') {
            continue;
        }
        if (!IsSafeRelPath(name)) {
            SPDLOG_WARN("zip extract: skipping unsafe entry `{}`", name);
            continue;
        }
        zip_file_t* zf = zip_fopen_index(za, static_cast<zip_uint64_t>(i), 0);
        if (zf == nullptr) {
            SPDLOG_ERROR("zip extract: cannot open entry `{}`", name);
            ok = false;
            break;
        }
        const std::filesystem::path outPath = destDir / std::filesystem::path(name);
        std::error_code ec;
        std::filesystem::create_directories(outPath.parent_path(), ec);
        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            SPDLOG_ERROR("zip extract: cannot write {}", outPath.string());
            zip_fclose(zf);
            ok = false;
            break;
        }
        char buf[64 * 1024];
        zip_int64_t n;
        while ((n = zip_fread(zf, buf, sizeof(buf))) > 0) {
            out.write(buf, n);
        }
        if (n < 0 || !out.good()) {
            SPDLOG_ERROR("zip extract: read/write failed on `{}`", name);
            ok = false;
        }
        out.close();
        zip_fclose(zf);
    }
    zip_close(za);
    return ok;
}

} // namespace ssb64
