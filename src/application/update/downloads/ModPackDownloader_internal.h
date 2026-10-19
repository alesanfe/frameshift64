/* ModPackDownloader_internal.h - shared internals of the content-pack
 * installer, split across:
 *
 *   ModPackDownloader.cpp  catalog, flow state, path/name hygiene, public API
 *   ModPackGliden.cpp      GLideN64/Rice name grammar + hash-map conversion
 *   ModPackInstall.cpp     zip validation/extraction + install worker pipeline
 *
 * Everything under mpd:: is cross-TU plumbing, not public API - the public
 * surface is declared in enhancements.h. Types shared by the pipeline
 * (InstallJob/InstallCtx/PayloadKind/ConvertStats) live here so all three
 * TUs agree on them. */
#pragma once

#include "enhancements.h"
#include "download_service.h"

#include <zip.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace ssb64 {
namespace enhancements {
namespace mpd {

// ---------------------------------------------------------------------------
// Flow state (storage lives in ModPackDownloader.cpp)
// ---------------------------------------------------------------------------

void     SetStatus(std::string s);
void     SetPartPath(std::string p);
void     SetLastName(std::string n);
void     SetPhase(ModPackInstallPhase p);
void     SetLastKind(ModPackInstalledKind k);
uint64_t Generation();
/* Publish/clear the in-flight transfer under s_childMutex so
 * CancelModPackInstall() can kill it (nullptr = no active transfer). */
void     SetActiveDownload(Download* d);
void     KillActiveDownload();

// ---------------------------------------------------------------------------
// Path/name hygiene (ModPackDownloader.cpp)
// ---------------------------------------------------------------------------

bool        HasTraversalOrAbsolute(const std::string& name);
std::string BasenameOf(const std::string& name);
std::string LowerExt(const std::string& name);
std::string SanitizeInstallName(std::string in);
std::string DeriveBaseName(const std::string& source);

/* mods/ root - the dir HiResPack scans and "Open Mods Folder" opens. */
std::filesystem::path ModsRoot();

// ---------------------------------------------------------------------------
// GLideN64 grammar + conversion (ModPackGliden.cpp)
// ---------------------------------------------------------------------------

bool LooksLikePackPng(const std::string& basename);
bool ParseGlidenName(const std::string& basename, std::string* outKey);
std::string NativePackName(const std::string& ourHash, const std::string& key);
bool GlidenMapReady();
/* nullptr when the key has no native hash in the vendored map. Only
 * meaningful after GlidenMapReady(). */
const std::string* GlidenMapLookup(const std::string& key);

struct ConvertStats { int matched = 0, unmatched = 0, other = 0; };

bool ConvertGlidenZip(zip_t* z, const std::filesystem::path& outDir,
                      ConvertStats& st, std::string& error);
bool ConvertGlidenDir(const std::filesystem::path& srcDir,
                      const std::filesystem::path& outDir,
                      ConvertStats& st, std::string& error);

// ---------------------------------------------------------------------------
// Zip extraction (ModPackInstall.cpp) - shared with the GLideN64 converter
// ---------------------------------------------------------------------------

bool ExtractZipMember(zip_t* z, zip_int64_t index, const std::filesystem::path& destPath);

// ---------------------------------------------------------------------------
// Install pipeline (ModPackInstall.cpp)
// ---------------------------------------------------------------------------

enum class PayloadKind { ScriptMod, NativePack, GlidenPack, Unknown };

struct InstallJob {
    std::string label;        // status text ("SSB Reloaded 2.0.1", URL, file name)
    std::string version;
    std::string url;          // "" => local source
    std::string localPath;    // file or directory source
    std::string sha256;       // "" => unverified (reported, not enforced)
    uint64_t    expectedSize = 0; // 0 => unknown
    std::string installName;  // fixed output base name; "" => derive from source
    bool        curated = false;
};

/* Shared state for one install job - the worker is a linear pipeline
 * (prepare -> acquire -> verify -> classify -> install), each phase a
 * function taking this context. Cleanup helpers centralize the recurring
 * "close the zip / drop the .part staging file" tails. */
struct InstallCtx {
    InstallJob job;
    uint64_t gen = 0;
    std::error_code ec;
    std::string err;
    std::filesystem::path modsDir;
    std::string baseName;
    std::filesystem::path srcFile;   // file payload (downloaded .part or local file)
    std::filesystem::path partPath;  // mods/.<base>.part staging for URL payloads
    std::filesystem::path localPath;
    zip_t* z = nullptr;
    bool isDir = false;
    bool isPng = false;
    bool isArchiveFile = false;
    std::string srcBase;
    std::string srcExt;

    bool cancelled() const { return Generation() != gen; }
    void fail(const std::string& why) {
        SPDLOG_ERROR("Mod install of {} failed: {}", job.label, why);
        SetStatus("Error: " + why);
        SetPartPath({});
        SetPhase(ModPackInstallPhase::Error);
    }
    void done(ModPackInstalledKind kind, const std::string& name) {
        SetPartPath({});
        SetLastName(name);
        SetLastKind(kind);
        SetStatus("Installed " + name);
        SPDLOG_INFO("Mod install: {} -> {}", job.label, name);
        SetPhase(ModPackInstallPhase::Done);
    }
    void dropPart() {
        if (!partPath.empty()) {
            std::filesystem::remove(partPath, ec);
            ec.clear();
        }
    }
    void closeZip() {
        if (z != nullptr) {
            zip_close(z);
            z = nullptr;
        }
    }
    /* Frequent tail: release transient artifacts then report the failure. */
    void abort(const std::string& why) {
        closeZip();
        dropPart();
        fail(why);
    }
    void abortSilent() { // cancelled: transient cleanup, no error
        closeZip();
        dropPart();
    }
};

void InstallWorker(InstallJob job, uint64_t gen);

} // namespace mpd
} // namespace enhancements
} // namespace ssb64
