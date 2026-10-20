/*
 * ModRegistry.cpp — see ModRegistry.h.
 *
 * Walks the archives the engine already mounted (ArchiveManager) rather
 * than re-scanning the filesystem: that way a "mod" is whatever the engine
 * actually has, manifest parsing is reused from the Archive layer
 * (GetManifest), and per-archive reads avoid the VFS path collision you'd
 * hit reading "manifest.json" globally when several mods each ship one.
 */
#include "ModRegistry.h"

#include <libultraship/libultraship.h> // Ship::Context
#include <ship/resource/ResourceManager.h>
#include <ship/resource/archive/ArchiveManager.h>
#include <ship/resource/archive/Archive.h> // Archive, ArchiveManifest, GetManifest
#include <ship/scripting/ScriptLoader.h>   // GetLoadersInDependencyOrder

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_set>
#include <utility>

namespace ssb64::mods {
namespace {

bool CaseInsensitiveLess(const std::string& a, const std::string& b) {
    return std::lexicographical_compare(
        a.begin(), a.end(), b.begin(), b.end(),
        [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
}

/* Folder name, or archive filename without extension — a readable fallback
 * label when a mod's manifest carries no usable "name". */
std::string PathLabel(const std::string& archivePath) {
    const std::filesystem::path p(archivePath);
    std::string stem = p.stem().string();
    if (!stem.empty()) {
        return stem;
    }
    std::string fname = p.filename().string();
    return fname.empty() ? archivePath : fname;
}

} // namespace

/* Names of mods that compiled and ran ModInit successfully. Keyed by
 * manifest name — the same key ScriptLoader uses internally. If the
 * loader is not up, every mod simply reports NotLoaded. */
static std::unordered_set<std::string> CollectLoadedModNames(Ship::Context* ctx) {
    std::unordered_set<std::string> loaded;
    if (auto scripting = ctx->GetScriptLoader()) {
        for (const auto& name : scripting->GetLoadersInDependencyOrder()) {
            loaded.insert(name);
        }
    }
    return loaded;
}

/* Build the ModInfo for one mounted archive (manifest fields + load state).
 * A mounted archive is a managed mod iff it carries a manifest.json.
 * Excludes the base game + shader archives, which carry none. */
static bool ArchiveToModInfo(const std::shared_ptr<Ship::Archive>& archive,
                             const std::unordered_set<std::string>& loaded,
                             ModInfo* info) {
    if (!archive || !archive->HasFile("manifest.json")) {
        return false;
    }
    const Ship::ArchiveManifest& manifest = archive->GetManifest();

    info->archivePath = archive->GetPath();
    info->version = manifest.Version;
    info->author = manifest.Author;
    info->description = manifest.Description;
    info->simulation = manifest.Simulation;

    if (manifest.Name.empty()) {
        /* manifest.json exists but parsed to no name: malformed JSON or
         * a missing "name". Surface it instead of hiding it. */
        info->name = PathLabel(info->archivePath);
        info->state = ModState::InvalidManifest;
        info->note = "manifest.json is missing a \"name\" or could not be parsed";
    } else {
        info->name = manifest.Name;
        info->state = loaded.count(manifest.Name) ? ModState::Loaded : ModState::NotLoaded;
    }
    return true;
}

std::vector<ModInfo> ModRegistry::Snapshot() {
    std::vector<ModInfo> mods;

    auto ctx = Ship::Context::GetInstance();
    if (!ctx) {
        return mods;
    }
    auto resourceManager = ctx->GetResourceManager();
    if (!resourceManager) {
        return mods;
    }
    auto archiveManager = resourceManager->GetArchiveManager();
    if (!archiveManager) {
        return mods;
    }
    auto archives = archiveManager->GetArchives();
    if (!archives) {
        return mods;
    }

    const std::unordered_set<std::string> loaded = CollectLoadedModNames(ctx.get());

    for (const auto& archive : *archives) {
        ModInfo info;
        if (!ArchiveToModInfo(archive, loaded, &info)) {
            continue;
        }
        mods.push_back(std::move(info));
    }

    /* Loaded mods first, then case-insensitive by name — stable across
     * frames so the rendered list does not jump around. */
    std::sort(mods.begin(), mods.end(), [](const ModInfo& a, const ModInfo& b) {
        const int rankA = (a.state == ModState::Loaded) ? 0 : 1;
        const int rankB = (b.state == ModState::Loaded) ? 0 : 1;
        if (rankA != rankB) {
            return rankA < rankB;
        }
        return CaseInsensitiveLess(a.name, b.name);
    });

    return mods;
}

} // namespace ssb64::mods
