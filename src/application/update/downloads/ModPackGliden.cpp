/* ModPackGliden.cpp - GLideN64/Rice texture-name handling for the mod
 * installer: the "#<riceCRC>#<fmt>#<siz>" name grammar, the vendored
 * hash map (bundled-data/gliden64-map.json, staged next to the binary),
 * and the zip/dir -> native-grammar materialization. Shared plumbing
 * (status reporting, path hygiene, ExtractZipMember) lives in
 * ModPackDownloader_internal.h's mpd:: namespace. */
#include "ModPackDownloader_internal.h"

#include "exe_dir.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace ssb64 {
namespace enhancements {
namespace mpd {
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Pack-content validation + grammar detection
// ---------------------------------------------------------------------------

/* Native pack grammar: basename contains "#<8 hex>#<n>#<n>" — same shape
 * HiResPack::ParseFilename requires. Only needs to prove the archive is
 * really a texture pack; the loader itself re-parses per member. */
bool LooksLikePackPng(const std::string& basename) {
    if (LowerExt(basename) != ".png") return false;
    const size_t first = basename.find('#');
    if (first == std::string::npos) return false;
    const size_t hexStart = first + 1;
    size_t i = hexStart;
    while (i < basename.size() && std::isxdigit(static_cast<unsigned char>(basename[i]))) ++i;
    return i > hexStart && i < basename.size() && basename[i] == '#';
}

namespace {

/* Split "a#b#c" on '#' bounded at `us` — the free-form prefix itself
 * may carry '#'/'_'. */
std::vector<std::string> SplitHashFields(const std::string& s, size_t us) {
    std::vector<std::string> fields;
    for (size_t pos = 0; pos < us;) {
        const size_t next = s.find('#', pos);
        fields.push_back(s.substr(pos, (next == std::string::npos || next > us
                                            ? us : next) - pos));
        if (next == std::string::npos || next > us) break;
        pos = next + 1;
    }
    return fields;
}

bool IsHex8(const std::string& f) {
    if (f.size() != 8) return false;
    for (char c : f) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool IsDigit1(const std::string& f) {
    return f.size() == 1 &&
           std::isxdigit(static_cast<unsigned char>(f[0]));
}

std::string UpperHex(std::string f) {
    for (char& c : f) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return f;
}

/* Candidate trailing tuple: either [crc, fmt, siz] or
 * [crc, fmt, siz, pal] — false when neither shape fits. */
bool GlidenTuple(const std::vector<std::string>& f,
                 std::string& crc, std::string& fmt,
                 std::string& siz, std::string& pal) {
    const size_t n = f.size();
    if (n >= 5 && IsHex8(f[n - 1]) && IsDigit1(f[n - 2]) &&
        IsDigit1(f[n - 3]) && IsHex8(f[n - 4])) {
        crc = f[n - 4]; fmt = f[n - 3]; siz = f[n - 2]; pal = f[n - 1];
        return true;
    }
    if (n >= 4 && IsDigit1(f[n - 1]) && IsDigit1(f[n - 2]) && IsHex8(f[n - 3])) {
        crc = f[n - 3]; fmt = f[n - 2]; siz = f[n - 1];
        return true;
    }
    return false;
}

} // namespace

/* GLideN64/Rice-PNG grammar:
 *     "<prefix>#<crc8>#<fmt>#<siz>[#<palCRC8>]_<tag>.png"
 * e.g. "SMASH BROTHERS#3A8B8290#2#0#41B9B4CE_ciByRGBA.png" or
 * "...#ABCDEF12#0#0_all.png". On match, `outKey` receives the
 * battleship.json lookup key ("CRC#F#S" or "CRC#F#S#PAL", uppercase). */
bool ParseGlidenName(const std::string& basename, std::string* outKey) {
    if (LowerExt(basename) != ".png") return false;
    // "_<tag>.png" must terminate the name.
    const size_t stemEnd = basename.size() - 4;
    const size_t us = basename.rfind('_', stemEnd);
    if (us == std::string::npos || us == 0 || us + 1 >= stemEnd) return false;
    // Split everything before the '_' on '#'; the last 3-4 fields are
    // the hash tuple, the leading part is the free-form prefix (it may
    // itself contain '#' or '_').
    const std::vector<std::string> fields = SplitHashFields(basename, us);
    if (fields.size() < 4) return false; // prefix + crc + fmt + siz minimum
    // Candidate trailing tuple: either [crc, fmt, siz] or
    // [crc, fmt, siz, pal].
    std::string crc, fmt, siz, pal;
    if (!GlidenTuple(fields, crc, fmt, siz, pal)) return false;
    std::string key = UpperHex(crc) + "#" + fmt + "#" + siz;
    if (!pal.empty()) key += "#" + UpperHex(pal);
    if (outKey) *outKey = key;
    return true;
}

/* "#<ourHash>#<fmt>#<siz>.png" from a mapped hash + a parse key. */
std::string NativePackName(const std::string& ourHash, const std::string& key) {
    const size_t f1 = key.find('#');
    const size_t f2 = key.find('#', f1 + 1);
    return "#" + ourHash + key.substr(f1, f2 - f1 + 2) + ".png";
}

namespace {

// ---------------------------------------------------------------------------
// GLideN64 -> native hash mapping (vendored data)
// ---------------------------------------------------------------------------

/* tools/developer/modding/vendor/battleship.json, staged next to the binary as
 * bundled-data/gliden64-map.json. Maps "RiceCRC#FMT#SIZ[#PAL]" (uppercase
 * hex) -> our decoded-RGBA8 hash (8 hex). Pure rename data — no image
 * decoding. Loaded lazily on the first GLideN64 pack seen. */
std::once_flag s_mapOnce;
bool s_mapOk = false;
std::unordered_map<std::string, std::string> s_glidenMap;

std::filesystem::path GlidenMapPath() {
    namespace fs = std::filesystem;
    const std::string dir = ExecutableDirPath();
    if (dir.empty()) return {};
    const fs::path exeDir(dir);
    // Packagers relocate data relative to the binary: Windows keeps it
    // next to the exe, the macOS .app moves it to Contents/Resources,
    // the Linux AppImage to usr/share/<app>.
    const fs::path candidates[] = {
        exeDir / "bundled-data" / "gliden64-map.json",
        exeDir.parent_path() / "Resources" / "bundled-data" / "gliden64-map.json",
        exeDir.parent_path() / "share" / "BattleShip" / "bundled-data" / "gliden64-map.json",
    };
    std::error_code ec;
    for (const fs::path& c : candidates) {
        if (fs::is_regular_file(c, ec)) return c;
        ec.clear();
    }
    return candidates[0];
}

void LoadGlidenMap() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path path = GlidenMapPath();
    if (path.empty() || !fs::is_regular_file(path, ec)) {
        SPDLOG_WARN("Mod pack: GLideN64 map not found at {}", path.string());
        return;
    }
    try {
        std::ifstream in(path, std::ios::binary);
        const nlohmann::json j = nlohmann::json::parse(in);
        if (!j.is_object()) {
            SPDLOG_ERROR("Mod pack: gliden64-map.json is not an object");
            return;
        }
        s_glidenMap.reserve(j.size());
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_string()) {
                s_glidenMap.emplace(it.key(), it.value().get<std::string>());
            }
        }
        s_mapOk = !s_glidenMap.empty();
        SPDLOG_INFO("Mod pack: GLideN64 map loaded ({} entries)", s_glidenMap.size());
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Mod pack: gliden64-map.json parse failed: {}", e.what());
    }
}

} // namespace

bool GlidenMapReady() {
    std::call_once(s_mapOnce, LoadGlidenMap);
    return s_mapOk;
}

const std::string* GlidenMapLookup(const std::string& key) {
    const auto it = s_glidenMap.find(key);
    return it == s_glidenMap.end() ? nullptr : &it->second;
}

/* Convert a GLideN64 zip/dir into a native-grammar directory at outDir.
 * Matched PNGs land as "<reldir>/#<hash>#<fmt>#<siz>.png"; unmatched or
 * unparseable PNGs are skipped (the mapping only covers vanilla-ROM
 * textures — Smash Remix additions have no target); .txt/.md metadata is
 * copied through for browsability. Returns counts for the status line. */
bool ConvertGlidenZip(zip_t* z, const std::filesystem::path& outDir,
                      ConvertStats& st, std::string& error) {
    namespace fs = std::filesystem;
    const zip_int64_t n = zip_get_num_entries(z, 0);
    for (zip_int64_t i = 0; i < n; i++) {
        const char* raw = zip_get_name(z, i, 0);
        if (raw == nullptr) continue;
        const std::string name(raw);
        if (name.back() == '/') continue;
        const std::string base = BasenameOf(name);
        const std::string ext = LowerExt(base);
        const std::string relDir = name.size() > base.size()
                                       ? name.substr(0, name.size() - base.size() - 1)
                                       : std::string();
        const fs::path dstDir = outDir / fs::path(relDir);

        if (ext == ".png") {
            if (LooksLikePackPng(base)) {
                // Already native — keep verbatim.
                if (!ExtractZipMember(z, i, dstDir / base)) {
                    error = "could not extract " + name;
                    return false;
                }
                ++st.matched;
                continue;
            }
            std::string key;
            if (!ParseGlidenName(base, &key)) {
                ++st.other;
                continue;
            }
            const auto it = s_glidenMap.find(key);
            if (it == s_glidenMap.end()) {
                ++st.unmatched;
                continue;
            }
            if (!ExtractZipMember(z, i, dstDir / NativePackName(it->second, key))) {
                error = "could not extract " + name;
                return false;
            }
            ++st.matched;
        } else if (ext == ".txt" || ext == ".md") {
            ExtractZipMember(z, i, dstDir / base); // metadata: best-effort
        } else {
            ++st.other;
        }
    }
    if (st.matched == 0) {
        error = "no pack textures matched the ROM's texture set";
        return false;
    }
    return true;
}

bool ConvertGlidenDir(const std::filesystem::path& srcDir,
                      const std::filesystem::path& outDir,
                      ConvertStats& st, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(srcDir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const fs::path src = it->path();
        const std::string base = src.filename().string();
        const std::string ext = LowerExt(base);
        const fs::path relDir = src.parent_path().lexically_relative(srcDir);
        const fs::path dstDir = outDir / relDir;

        if (ext == ".png") {
            std::string outName;
            if (LooksLikePackPng(base)) {
                outName = base;
            } else {
                std::string key;
                if (!ParseGlidenName(base, &key)) { ++st.other; continue; }
                const auto mit = s_glidenMap.find(key);
                if (mit == s_glidenMap.end()) { ++st.unmatched; continue; }
                outName = NativePackName(mit->second, key);
            }
            std::error_code cec;
            fs::create_directories(dstDir, cec);
            fs::copy_file(src, dstDir / outName, fs::copy_options::overwrite_existing, cec);
            if (cec) {
                error = "could not copy " + base + ": " + cec.message();
                return false;
            }
            ++st.matched;
        } else if (ext == ".txt" || ext == ".md") {
            std::error_code cec;
            fs::create_directories(dstDir, cec);
            fs::copy_file(src, dstDir / base, fs::copy_options::overwrite_existing, cec);
        } else {
            ++st.other;
        }
    }
    if (st.matched == 0) {
        error = "no pack textures matched the ROM's texture set";
        return false;
    }
    return true;
}

} // namespace mpd
} // namespace enhancements
} // namespace ssb64
