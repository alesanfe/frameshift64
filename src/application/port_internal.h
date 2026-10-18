#pragma once

/* Internal composition-root plumbing shared between port.cpp and the
 * sibling TUs split out of it (crash_handler.cpp, mods_mount.cpp,
 * mod_bridge_anchors.cpp). NOT public API — nothing outside
 * src/application/ should include this. */

#include <memory>

namespace Ship { class Context; }

/* The single LUS context owned by port.cpp. Borrowed by sibling TUs that
 * need ResourceManager/ArchiveManager access; may be nullptr early in
 * boot — callers must null-check. */
std::shared_ptr<Ship::Context> PortContext();

/* Windows crash diagnostics: vectored first-chance handler, unhandled
 * filter + minidump, signal() hooks, CRT report routing, terminate /
 * invalid-param hooks, atexit log marker. Empty body on non-Windows
 * builds so main() calls it unconditionally. */
void port_install_crash_handlers();

namespace ssb64 {

/* Mod archive mounting (mods_mount.cpp). MountModsDir walks the mods
 * roots and mounts new manifest/.o2r/.zip mods; UnmountMissingMods drops
 * archives whose on-disk source vanished. No-ops when PortContext() is
 * unset or scripting is disabled. */
void MountModsDir();
void UnmountMissingMods();

} // namespace ssb64
