/*
 * app_mods.cpp — see app_mods.h for the layering rationale.
 */
#include "app_mods.h"

#ifndef DISABLE_SCRIPTING

#include "integration/modding/HookManager.h"
#include "integration/modding/ModRegistry.h"

#include <libultraship/libultraship.h>
#include <ship/scripting/ScriptLoader.h>

#include <exception>

namespace ssb64::appmods {

static ModStatus ToStatus(mods::ModState s) {
    switch (s) {
        case mods::ModState::Loaded:          return ModStatus::Loaded;
        case mods::ModState::InvalidManifest: return ModStatus::InvalidManifest;
        case mods::ModState::NotLoaded:
        default:                              return ModStatus::NotLoaded;
    }
}

static SignatureStatus ToSignature(mods::ModSignature s) {
    switch (s) {
        case mods::ModSignature::Valid:   return SignatureStatus::Valid;
        case mods::ModSignature::Invalid: return SignatureStatus::Invalid;
        case mods::ModSignature::None:
        default:                          return SignatureStatus::None;
    }
}

std::vector<ModEntry> Snapshot() {
    /* Sync the mounted set to what's actually on disk so the list reflects
     * mods added or deleted at runtime. New mods appear as NotLoaded until
     * a Hot Reload compiles them; this only refreshes the inventory. */
    ssb64::UnmountMissingMods();
    ssb64::MountModsDir();

    std::vector<ModEntry> out;
    for (const mods::ModInfo& m : mods::ModRegistry::Snapshot()) {
        ModEntry e;
        e.name = m.name;
        e.version = m.version;
        e.author = m.author;
        e.description = m.description;
        e.archivePath = m.archivePath;
        e.state = ToStatus(m.state);
        e.note = m.note;
        e.simulation = m.simulation;
        e.signature = ToSignature(m.signature);
        out.push_back(std::move(e));
    }
    return out;
}

bool HotReload() {
    auto scripting = Ship::Context::GetInstance()->GetScriptLoader();
    if (!scripting) {
        return false;
    }
    try {
        /* Unload: ModExit fires, then HookManager uninstalls every
         * hook owned by that mod (postExit callback runs BEFORE
         * FreeLibrary so trampolines into the about-to-be-freed mod
         * code get cleanly removed). Then recompile + reload with the
         * SetCurrentOwner wrapper so new Install calls re-tag with
         * the right owner. */
        scripting->UnloadAll(
            /*preExit=*/std::nullopt,
            /*postExit=*/[](const std::string& mod) {
                mods::HookManager::UninstallHooksForOwner(mod.c_str());
            });
        /* Now that every script is unloaded, drop archives whose mod folder
         * was deleted at runtime — otherwise it stays mounted and gets
         * recompiled below as if it were still installed. Then pick up any
         * new folders/archives that landed in mods/ since the last load
         * (idempotent: already-mounted archives are skipped). */
        ssb64::UnmountMissingMods();
        ssb64::MountModsDir();
        scripting->CompileAll();
        scripting->LoadAll(
            /*preInit=*/[](const std::string& mod) {
                mods::HookManager::SetCurrentOwner(mod.c_str());
            },
            /*postInit=*/[](const std::string&) {
                mods::HookManager::ClearCurrentOwner();
            });
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Mod reload failed: {}", e.what());
        return false;
    }
    return true;
}

} // namespace ssb64::appmods

#endif // !DISABLE_SCRIPTING
