/* ModPackInstall.cpp - the content-pack install worker pipeline:
 * zip validation/extraction, payload classification, and the staged
 * commit into mods/. Runs on a detached thread launched by
 * ModPackDownloader.cpp's LaunchWorker; reports progress through the
 * mpd:: flow-state accessors in ModPackDownloader_internal.h. */
#include "ModPackDownloader_internal.h"

#include "FileHash.h"
#include "port_log.h"

#include <spdlog/fmt/fmt.h>

#include <chrono>
#include <thread>

namespace ssb64 {
namespace enhancements {
namespace mpd {
namespace {
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Payload classification
// ---------------------------------------------------------------------------

/* Classify a zip: root manifest.json -> script mod; else by which PNG
 * grammar its members use. Also enforces the traversal audit — every
 * caller handles the archive's members by name, so unsafe names are
 * rejected regardless of kind. `pngCount` reports grammar-matching
 * members for status text. */
PayloadKind ClassifyZip(zip_t* z, std::string& error) {
    int native = 0, gliden = 0;
    const zip_int64_t n = zip_get_num_entries(z, 0);
    for (zip_int64_t i = 0; i < n; i++) {
        const char* raw = zip_get_name(z, i, 0);
        if (raw == nullptr) continue;
        const std::string name(raw);
        if (HasTraversalOrAbsolute(name)) {
            error = "archive contains an unsafe path: " + name;
            return PayloadKind::Unknown;
        }
        if (name.back() == '/') continue;
        if (name == "manifest.json") {
            return PayloadKind::ScriptMod;
        }
        const std::string base = BasenameOf(name);
        if (LowerExt(base) != ".png") continue;
        if (LooksLikePackPng(base)) { ++native; continue; }
        std::string key;
        if (ParseGlidenName(base, &key)) {
            ++gliden;
            continue;
        }
    }
    if (native > 0 && gliden == 0) return PayloadKind::NativePack;
    if (gliden > 0) return PayloadKind::GlidenPack; // mixed packs convert too
    if (native > 0) return PayloadKind::NativePack;
    return PayloadKind::Unknown;
}

/* The strict audit the curated (pinned) path keeps: PNG pack members or
 * text metadata only — a pinned digest already proves content, this
 * catches a structurally-malformed mirror of it. */
bool ValidateCuratedPackZip(zip_t* z, std::string& error) {
    int packPngs = 0;
    const zip_int64_t n = zip_get_num_entries(z, 0);
    for (zip_int64_t i = 0; i < n; i++) {
        const char* raw = zip_get_name(z, i, 0);
        if (raw == nullptr) continue;
        const std::string name(raw);
        /* Defense in depth: the pinned digest already proves content, but
         * the member audit should catch unsafe names even if a caller
         * ever bypasses verification. */
        if (HasTraversalOrAbsolute(name)) {
            error = "archive contains an unsafe path: " + name;
            return false;
        }
        if (name.back() == '/') continue;
        const std::string base = BasenameOf(name);
        const std::string ext = LowerExt(base);
        if (ext == ".png") {
            if (LooksLikePackPng(base)) ++packPngs;
            continue;
        }
        if (ext == ".txt" || ext == ".md") continue;
        error = "archive contains an unexpected file: " + name;
        return false;
    }
    if (packPngs == 0) {
        error = "archive has no pack textures (#<hash>#<fmt>#<siz>.png)";
        return false;
    }
    return true;
}

zip_t* OpenZipChecked(const std::filesystem::path& zipPath, std::string& error) {
    int zerr = 0;
    zip_t* z = zip_open(zipPath.generic_string().c_str(), ZIP_RDONLY, &zerr);
    if (z == nullptr) {
        zip_error_t ze;
        zip_error_init_with_code(&ze, zerr);
        error = std::string("file is not a valid zip (") + zip_error_strerror(&ze) + ")";
        zip_error_fini(&ze);
    }
    return z;
}


/* Move/copy a finished payload into mods/ atomically: stage under a
 * dot-name in the same directory, then rename. */
bool CommitFile(const std::filesystem::path& src, const std::filesystem::path& dst,
                std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path tmp = dst.parent_path() / ("." + dst.filename().string() + ".tmp");
    fs::remove(tmp, ec); ec.clear();
    if (src != tmp) {
        fs::rename(src, tmp, ec);
        if (ec) { // cross-fs (local file elsewhere) — copy instead
            ec.clear();
            fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                error = "could not stage file into mods/: " + ec.message();
                return false;
            }
        }
    }
    fs::remove(dst, ec); ec.clear();
    fs::rename(tmp, dst, ec);
    if (ec) {
        error = "could not install into mods/: " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

/* Materialize a directory payload the same way: build under a hidden
 * staging dir inside mods/, then rename to the final name. Existing
 * installs are replaced wholesale (delete + rename is not atomic, but
 * reinstalling a dir over itself while mounted is worse). */
bool CommitDir(const std::filesystem::path& staged, const std::filesystem::path& dst,
               std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all(dst, ec); ec.clear();
    fs::rename(staged, dst, ec);
    if (ec) {
        error = "could not install into mods/: " + ec.message();
        fs::remove_all(staged, ec);
        return false;
    }
    return true;
}

bool PrepareInstallDirs(InstallCtx& c) {
    try {
        c.modsDir = ModsRoot();
    } catch (...) {
        c.fail("app directory unavailable");
        return false;
    }
    fs::create_directories(c.modsDir, c.ec);
    if (c.ec) {
        c.fail("could not create mods/ folder: " + c.ec.message());
        return false;
    }
    c.baseName = c.job.installName.empty()
                     ? DeriveBaseName(c.job.url.empty() ? c.job.localPath : c.job.url)
                     : SanitizeInstallName(c.job.installName);
    return true;
}

/* URL jobs download into mods/.<base>.part; local jobs read the source in
 * place. Returns false on failure or cancellation (silent). */
bool AcquirePayload(InstallCtx& c) {
    if (c.job.url.empty()) {
        c.srcFile = c.job.localPath;
        return true;
    }
    c.partPath = c.modsDir / ("." + c.baseName + ".part");
    SetPartPath(c.partPath.string());
    fs::remove(c.partPath, c.ec);
    c.ec.clear();

    SetPhase(ModPackInstallPhase::Downloading);
    SetStatus("Downloading " + c.job.label + "...");
    SPDLOG_INFO("Mod pack: downloading {} -> {}", c.job.url, c.partPath.string());

    Download* dl = DownloadStart(c.job.url, c.partPath);
    if (dl == nullptr) {
        c.fail("could not start the download");
        return false;
    }
    SetActiveDownload(dl);
    DownloadState dlState;
    while ((dlState = DownloadPoll(dl)) == DownloadState::Running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    SetActiveDownload(nullptr);
    dlState = DownloadFinish(dl);
    if (c.cancelled() || dlState == DownloadState::Cancelled) {
        c.dropPart();
        return false;
    }
    if (dlState != DownloadState::Done) {
        c.dropPart();
        c.fail("download failed");
        return false;
    }
    c.srcFile = c.partPath;
    return true;
}

/* Pinned size/sha256 verification when the manifest carries them; otherwise
 * hash anyway so the user can check the digest against the link source. */
bool VerifyPayload(InstallCtx& c) {
    if (!c.job.sha256.empty() || c.job.expectedSize > 0) {
        SetPhase(ModPackInstallPhase::Verifying);
        SetStatus("Verifying download...");
        if (!DownloadedFileMatches(c.srcFile.string(), c.job.expectedSize,
                                   c.job.sha256.empty() ? "" : std::string("sha256:") + c.job.sha256,
                                   c.err)) {
            c.dropPart();
            c.fail(c.err);
            return false;
        }
    } else if (!c.srcFile.empty()) {
        // Unverified source - compute the digest anyway so the user can
        // check it against wherever they got the link.
        SetPhase(ModPackInstallPhase::Verifying);
        SetStatus("Hashing download...");
        const std::string hex = Sha256File(c.srcFile.string());
        if (!hex.empty()) {
            SetStatus("sha256: " + hex);
        }
    }
    if (c.cancelled()) {
        c.dropPart();
        return false;
    }
    return true;
}

/* Directory scan used for folder payloads: counts PNG basenames matching
 * the native grammar vs the GLideN64/Rice grammar. */
PayloadKind ClassifyDir(const fs::path& dir, std::error_code& ec) {
    if (fs::exists(dir / "manifest.json", ec)) {
        return PayloadKind::ScriptMod;
    }
    int native = 0, gliden = 0;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const std::string base = it->path().filename().string();
        if (LowerExt(base) != ".png") continue;
        if (LooksLikePackPng(base)) ++native;
        else { std::string k; if (ParseGlidenName(base, &k)) ++gliden; }
    }
    if (gliden > 0) return PayloadKind::GlidenPack;
    if (native > 0) return PayloadKind::NativePack;
    return PayloadKind::Unknown;
}

/* Determines payload shape (dir / png / archive) and pack kind. On a fatal
 * validation error it reports via c.fail() and returns false. */
bool ClassifyPayload(InstallCtx& c, PayloadKind* kind) {
    SetPhase(ModPackInstallPhase::Validating);
    SetStatus("Checking contents...");

    *kind = PayloadKind::Unknown;
    if (c.isDir) {
        *kind = ClassifyDir(c.localPath, c.ec);
    } else if (c.isPng) {
        *kind = PayloadKind::NativePack; // single texture: grammar checked at convert time
    } else if (c.isArchiveFile) {
        c.z = OpenZipChecked(c.srcFile, c.err);
        if (c.z == nullptr) {
            c.dropPart();
            c.fail(c.err);
            return false;
        }
        if (c.job.curated) {
            // Pinned content still gets the strict member audit.
            if (!ValidateCuratedPackZip(c.z, c.err)) {
                c.abort(c.err);
                return false;
            }
            *kind = PayloadKind::NativePack;
        } else if (c.srcExt == ".o2r" || c.srcExt == ".otr") {
            // An .o2r/.otr is mounted by MountModsDir whether or not it
            // carries a manifest (pure asset-swap archives exist).
            *kind = PayloadKind::ScriptMod;
        } else {
            *kind = ClassifyZip(c.z, c.err);
        }
    } else {
        c.fail("unsupported input - expected a .zip, an image pack folder, or a PNG");
        return false;
    }
    if (c.cancelled()) {
        c.abortSilent();
        return false;
    }
    return true;
}

/* Stage a directory payload through mods/.<base>.tmp then atomically commit
 * it into mods/<base>. */
void InstallDirPayload(InstallCtx& c, ModPackInstalledKind result, const char* copyErr) {
    const fs::path dst = c.modsDir / c.baseName;
    const fs::path stage = c.modsDir / ("." + c.baseName + ".tmp");
    fs::remove_all(stage, c.ec); c.ec.clear();
    fs::copy(c.localPath, stage,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, c.ec);
    if (c.ec) {
        fs::remove_all(stage, c.ec);
        c.fail(std::string(copyErr) + c.ec.message());
        return;
    }
    if (!CommitDir(stage, dst, c.err)) {
        c.fail(c.err);
        return;
    }
    c.done(result, c.baseName);
}

/* Zip payload committed as mods/<base>.zip. */
void InstallZipPayload(InstallCtx& c, ModPackInstalledKind result) {
    const fs::path dst = c.modsDir / (c.baseName + ".zip");
    if (!CommitFile(c.srcFile, dst, c.err)) {
        c.abort(c.err);
        return;
    }
    c.done(result, c.baseName + ".zip");
}

/* Single loose PNG: must already carry the native grammar name, unless the
 * GLideN64 map can rename it. */
void InstallPngPayload(InstallCtx& c) {
    std::string key;
    if (!LooksLikePackPng(c.srcBase)) {
        if (ParseGlidenName(c.srcBase, &key) && GlidenMapReady()) {
            if (const std::string* ourHash = GlidenMapLookup(key)) {
                const std::string outName = NativePackName(*ourHash, key);
                const fs::path dst = c.modsDir / outName;
                if (!CommitFile(c.srcFile, dst, c.err)) {
                    c.fail(c.err);
                    return;
                }
                c.done(ModPackInstalledKind::TexturePack, outName);
                return;
            }
        }
        c.fail("PNG is not in the pack grammar (\"#<hash>#<fmt>#<siz>.png\") "
               "or the GLideN64/Rice grammar");
        return;
    }
    const fs::path dst = c.modsDir / c.srcBase;
    if (!CommitFile(c.srcFile, dst, c.err)) {
        c.fail(c.err);
        return;
    }
    c.done(ModPackInstalledKind::TexturePack, c.srcBase);
}

/* GLideN64/Rice pack: convert member names through the bundled map into a
 * staging dir, then commit the converted tree. */
void InstallGlidenPayload(InstallCtx& c) {
    if (!GlidenMapReady()) {
        c.abort("this build has no GLideN64 conversion table "
                "(bundled-data/gliden64-map.json missing)");
        return;
    }
    SetPhase(ModPackInstallPhase::Converting);
    SetStatus("Converting texture names...");
    const fs::path stage = c.modsDir / ("." + c.baseName + ".tmp");
    fs::remove_all(stage, c.ec); c.ec.clear();
    fs::create_directories(stage, c.ec);
    ConvertStats st;
    const bool ok = c.z ? ConvertGlidenZip(c.z, stage, st, c.err)
                        : ConvertGlidenDir(c.localPath, stage, st, c.err);
    c.closeZip();
    if (!ok) {
        fs::remove_all(stage, c.ec);
        c.dropPart();
        c.fail(c.err);
        return;
    }
    if (c.cancelled()) {
        fs::remove_all(stage, c.ec);
        c.dropPart();
        return;
    }
    SetPhase(ModPackInstallPhase::Installing);
    SetStatus("Installing converted pack...");
    const fs::path dst = c.modsDir / c.baseName;
    if (!CommitDir(stage, dst, c.err)) {
        c.dropPart();
        c.fail(c.err);
        return;
    }
    c.dropPart();
    c.done(ModPackInstalledKind::TexturePack,
           c.baseName + fmt::format(" ({} textures converted, {} skipped)",
                                    st.matched, st.unmatched));
}

void InstallPayload(InstallCtx& c, PayloadKind kind) {
    switch (kind) {
    case PayloadKind::ScriptMod:
        SetPhase(ModPackInstallPhase::Installing);
        SetStatus("Installing mod into mods/...");
        if (c.isDir) {
            InstallDirPayload(c, ModPackInstalledKind::ScriptMod,
                              "could not copy mod folder: ");
        } else {
            InstallZipPayload(c, ModPackInstalledKind::ScriptMod);
        }
        break;
    case PayloadKind::NativePack:
        SetPhase(ModPackInstallPhase::Installing);
        SetStatus("Installing pack into mods/...");
        if (c.isDir) {
            InstallDirPayload(c, ModPackInstalledKind::TexturePack,
                              "could not copy pack folder: ");
        } else if (c.isPng) {
            InstallPngPayload(c);
        } else {
            InstallZipPayload(c, ModPackInstalledKind::TexturePack);
        }
        break;
    case PayloadKind::GlidenPack:
        InstallGlidenPayload(c);
        break;
    case PayloadKind::Unknown:
    default:
        c.abort("unrecognized content - expected a mod (manifest.json), a "
                "native texture pack, or a GLideN64/Rice pack");
        return;
    }
}


} // namespace

/* Extract one zip member into destPath (creating parents). Caller has
 * already run the name audit. */
bool ExtractZipMember(zip_t* z, zip_int64_t index, const std::filesystem::path& destPath) {
    std::error_code ec;
    std::filesystem::create_directories(destPath.parent_path(), ec);
    zip_file_t* f = zip_fopen_index(z, index, 0);
    if (f == nullptr) return false;
    FILE* out = std::fopen(destPath.string().c_str(), "wb");
    if (out == nullptr) {
        zip_fclose(f);
        return false;
    }
    char buf[64 * 1024];
    zip_int64_t got;
    bool ok = true;
    while ((got = zip_fread(f, buf, sizeof(buf))) > 0) {
        if (std::fwrite(buf, 1, static_cast<size_t>(got), out) != static_cast<size_t>(got)) {
            ok = false;
            break;
        }
    }
    ok = ok && got == 0;
    std::fclose(out);
    zip_fclose(f);
    return ok;
}

void InstallWorker(InstallJob job, uint64_t gen) {
    namespace fs = std::filesystem;
    InstallCtx c{ std::move(job), gen };

    if (!PrepareInstallDirs(c)) return;
    if (!AcquirePayload(c)) return;
    if (!VerifyPayload(c)) return;

    // Payload-shape facts shared by classification and install.
    c.localPath = c.job.localPath.empty() ? fs::path() : fs::path(c.job.localPath);
    c.isDir = !c.localPath.empty() && fs::is_directory(c.localPath, c.ec);
    c.ec.clear();
    // The payload's own extension comes from the source name (the URL's
    // last segment or the local path), not the .part staging name.
    c.srcBase = !c.localPath.empty()
                    ? c.localPath.filename().string()
                    : BasenameOf(c.job.url.substr(0, c.job.url.find_first_of("?#")));
    c.srcExt = LowerExt(c.srcBase);
    c.isPng = c.srcExt == ".png";
    c.isArchiveFile = !c.srcFile.empty() && !c.isDir && !c.isPng;

    PayloadKind kind;
    if (!ClassifyPayload(c, &kind)) return;
    InstallPayload(c, kind);
    c.closeZip();
}

} // namespace mpd
} // namespace enhancements
} // namespace ssb64
