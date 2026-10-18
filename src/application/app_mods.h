/*
 * app_mods.h — presentation-facing facade over the modding subsystem.
 *
 * src/presentation must not include src/game/engine directly (module-dep
 * rule), so the Mods menu talks to this thin API instead. Everything here
 * is a plain value type or a synchronous call; the ImGui layer keeps no
 * knowledge of ModRegistry/HookManager internals.
 *
 * Only meaningful when scripting is enabled — the integration/modding tree
 * is compiled out under DISABLE_SCRIPTING and callers are #ifdef-guarded
 * the same way.
 */
#pragma once

#include <string>
#include <vector>

namespace ssb64 {

/* Forwarded here so presentation never sees the integration header. */
void MountModsDir();
void UnmountMissingMods();

namespace appmods {

/* Lifecycle state of an installed mod, as the user should see it. */
enum class ModStatus {
    Loaded,          /* compiled + ModInit ran: live in the engine        */
    NotLoaded,       /* mounted/installed but not currently loaded         */
    InvalidManifest, /* mounted, but manifest.json is unnamed/unparseable  */
};

/* Signature status of the archive payload. */
enum class SignatureStatus {
    None,    /* no signature fields in the manifest          */
    Valid,   /* checksum recomputes AND signature verifies   */
    Invalid, /* signed in name only — bad checksum/verify    */
};

/* A single installed mod, flattened for display. Plain value type. */
struct ModEntry {
    std::string name;        /* manifest "name" (or a path-derived fallback) */
    std::string version;     /* manifest "version"                           */
    std::string author;      /* manifest "author"                            */
    std::string description; /* manifest "description"                       */
    std::string archivePath; /* absolute path of the mounted folder / .o2r   */
    ModStatus state = ModStatus::NotLoaded;
    std::string note; /* human-readable detail; set for InvalidManifest */
    bool simulation = true; /* manifest "simulation"; false = cosmetic-only */
    SignatureStatus signature = SignatureStatus::None;
};

/*
 * Refresh the mounted set from disk (drop missing archives, pick up new
 * folders) and return the flattened inventory. Ordered deterministically:
 * loaded mods first, then case-insensitively by name.
 */
std::vector<ModEntry> Snapshot();

/*
 * Unload every script mod (ModExit + per-owner hook cleanup), rescan the
 * mods/ folder, recompile and reload (with hook ownership tagging).
 * Returns false if the reload threw — error is already logged.
 */
bool HotReload();

} // namespace appmods
} // namespace ssb64
