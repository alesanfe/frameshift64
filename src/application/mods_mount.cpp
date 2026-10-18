/* Mod archive mounting — extracted from port.cpp.
 *
 * MountModsDir walks the mods roots and mounts manifest/o2r/zip mods into
 * the LUS ArchiveManager; UnmountMissingMods drops archives whose on-disk
 * source vanished. Both borrow the context via PortContext(). */
#include "port_internal.h"

#include "app_paths.h"
#include "port_log.h"
#include "SyntheticReloc.h"

#include <libultraship/libultraship.h>
#include <ship/Context.h>

#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#ifndef DISABLE_SCRIPTING
namespace ssb64 {

/* Recursively walk mods/, mounting any folder that contains a
 * manifest.json or any .o2r/.otr/.zip file as a separate archive
 * with the LUS ArchiveManager. Folders without a manifest.json are
 * treated as category dirs and traversed for nested mods.
 *
 * This means modders can organize via either layout:
 *
 *   mods/MasterBall/manifest.json            ← flat
 *   mods/items/MasterBall/manifest.json      ← categorical
 *   mods/pokemon/Celebi/manifest.json        ← categorical
 *   mods/some_pack.o2r                       ← packaged
 *
 * Idempotent: AddArchive is only called for paths not already
 * present in the manager's archive list. Safe to call repeatedly
 * (e.g. from the Mods → Reload menu, after a modder dropped a
 * brand-new mod folder in mods/ while the engine was running). */
void MountModsDir() {
	namespace fs = std::filesystem;
	auto rm = PortContext() ? PortContext()->GetResourceManager() : nullptr;
	if (!rm) return;
	auto am = rm->GetArchiveManager();
	if (!am) return;

	/* Mods can live in two roots:
	 *   - the bundle dir (RealAppBundlePath()/mods) — mods shipped with
	 *     the release or dropped next to the exe in portable installs;
	 *   - the writable app-data dir (GetPathRelativeToAppDirectory("mods"))
	 *     — where the in-game installer puts mods on packaged builds
	 *     (AppImage, .app, installed layouts) whose bundle dir is
	 *     read-only. In portable mode both resolve to the same place and
	 *     the second walk just no-ops through the dedupe set. */
	std::vector<fs::path> modsDirs;
#ifdef BATTLESHIP_UWP
	const fs::path usbMods(ssb64::ExternalDataPath("mods"));
	std::error_code rootEc;
	modsDirs.push_back(fs::is_directory(usbMods, rootEc)
	                     ? usbMods
	                     : fs::path(Ship::Context::GetAppDirectoryPath()) / "mods");
#else
	modsDirs.push_back(ssb64::RealAppBundlePath() + "/mods");
	/* In portable mode GetPathRelativeToAppDirectory("mods") resolves to
	 * the same directory as the bundle dir — possibly through a different
	 * spelling ("./mods" vs absolute). Canonicalise before comparing so
	 * the same dir is never walked twice (the dedupe set keys on the
	 * archive path string, which would differ per spelling). */
	std::error_code appDataEc;
	fs::path appDataMods =
		fs::weakly_canonical(
			Ship::Context::GetPathRelativeToAppDirectory("mods"),
			appDataEc);
	appDataEc.clear();
	const fs::path bundleMods = fs::weakly_canonical(modsDirs.front(), appDataEc);
	if (!appDataMods.empty() && appDataMods != bundleMods) {
		modsDirs.push_back(appDataMods);
	}
#endif
	{
		std::error_code ec;
		bool any = false;
		for (const auto& d : modsDirs) {
			if (fs::exists(d, ec)) { any = true; break; }
		}
		if (!any) {
			return;
		}
	}

	/* Build a set of already-mounted archive paths so we don't
	 * double-mount on a rescan. Path comparison is by exact string;
	 * the LUS manager keeps the path the caller passed to AddArchive. */
	auto existing_list = am->GetArchives();
	std::unordered_set<std::string> existing;
	if (existing_list) {
		for (const auto& a : *existing_list) {
			if (a) existing.insert(a->GetPath());
		}
	}

	bool mounted_new = false;
	auto try_mount = [&](const fs::path& p) {
		const std::string path = p.generic_string();
		if (existing.contains(path)) {
			return;
		}
		if (am->AddArchive(path)) {
			port_log("SSB64: mounted mod archive -> %s\n", path.c_str());
			existing.insert(path);
			mounted_new = true;
		} else {
			port_log("SSB64: failed to mount mod archive -> %s\n", path.c_str());
		}
	};

	std::function<void(const fs::path&)> walk = [&](const fs::path& dir) {
		std::error_code ec_local;
		for (const auto& entry : fs::directory_iterator(dir, ec_local)) {
			const fs::path p = entry.path();
			if (entry.is_regular_file(ec_local)) {
				const std::string ext = p.extension().string();
				if (ext == ".o2r" || ext == ".otr" || ext == ".zip") {
					try_mount(p);
				}
				continue;
			}
			if (!entry.is_directory(ec_local)) {
				continue;
			}
			/* A folder containing manifest.json is a mod. Otherwise
			 * treat it as a category directory and recurse. */
			if (fs::exists(p / "manifest.json", ec_local)) {
				try_mount(p);
			} else {
				walk(p);
			}
		}
	};
	for (const auto& d : modsDirs) {
		std::error_code ec;
		if (fs::is_directory(d, ec)) {
			walk(d);
		}
	}

	/* A newly mounted archive can override paths whose resources are
	 * already cached — the VFS override binds at AddArchive time, but the
	 * ResourceManager cache and the deblob synthesis cache (I8,
	 * docs/deblob.md) would keep serving the old content until the next
	 * process start. Drop both; resources reload on demand and deblobbed
	 * bundles re-synthesize with the override on the next scene load.
	 * At boot this runs before anything is cached, so it is free. */
	if (mounted_new) {
		if (auto resMgr = PortContext() ? PortContext()->GetResourceManager() : nullptr) {
			resMgr->UnloadResources("*");
		}
		portSyntheticRelocEvictAll();
	}
}

/* Unmount mod archives whose on-disk source no longer exists. MountModsDir
 * only ever ADDS archives, so a mod folder/.o2r deleted at runtime stays
 * mounted in the ArchiveManager for the rest of the session — Hot Reload would
 * recompile it from the still-mounted VFS and Rescan would still list it.
 * Removing the stale archive here lets both reflect deletions. Only archives
 * carrying a manifest.json are considered, so the base game + shader archives
 * are never touched. Call with mod scripts already unloaded (the loaded image
 * is independent of the archive, but unloading first keeps state consistent). */
void UnmountMissingMods() {
	namespace fs = std::filesystem;
	auto rm = PortContext() ? PortContext()->GetResourceManager() : nullptr;
	if (!rm) return;
	auto am = rm->GetArchiveManager();
	if (!am) return;
	auto archives = am->GetArchives();
	if (!archives) return;

	/* Collect first, then remove — don't mutate the manager's list mid-walk. */
	std::vector<std::string> stale;
	for (const auto& a : *archives) {
		if (!a) continue;
		if (!a->HasFile("manifest.json")) continue; /* not a mod */
		const std::string path = a->GetPath();
		std::error_code ec;
		if (!fs::exists(fs::path(path), ec)) {
			stale.push_back(path);
		}
	}
	for (const auto& path : stale) {
		am->RemoveArchive(path);
		port_log("SSB64: unmounted deleted mod archive -> %s\n", path.c_str());
	}
}

} // namespace ssb64
#endif
