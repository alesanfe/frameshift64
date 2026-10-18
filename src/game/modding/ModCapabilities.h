#pragma once
/*
 * ModCapabilities.h — manifest-declared capability isolation for script mods.
 *
 * TCC mods run in-process and can call any exported engine symbol; true
 * memory sandboxing is impossible without an out-of-process VM. What we
 * CAN enforce is the resolution boundary: every symbol a mod obtains goes
 * through mod_resolve_symbol() / mod_install_hook(), so gating there
 * prevents a mod from silently reaching sensitive subsystems it never
 * declared.
 *
 * A mod's manifest may declare:
 *
 *     "capabilities": ["network", "persist", "debug", "system"]
 *
 *   - "network" — netplay transport/session/state symbols
 *                 (port_netlobby*, port_netstats*, port_netleaderboard*,
 *                  port_netfriends*, port_ws_*, uplink_*, syNet*, netpeer*,
 *                  port_netrollback*).
 *   - "persist" — save/profile persistence (port_save*, port_profile*).
 *   - "debug"   — diagnostics (port_diag*, port_watchdog*, port_crash*,
 *                 port_symbolize*, port_heap*).
 *   - "system"  — updater/download/process control (port_update*,
 *                 port_download*, port_exit*).
 *
 * Ungated prefixes (gameplay `ft*`, `it*`, `gm*`, `sc*`, `ef*`, `gr*`,
 * `wp*`, `mn*`, `mp*`, `os*`, `lb*`, rendering, audio) resolve for every
 * mod — they are the reason mods exist.
 *
 * A manifest WITHOUT "capabilities" is unrestricted (backward compat) and
 * logs one warning encouraging authors to declare them. A manifest WITH
 * the field gets exactly what it asks for: resolving a gated symbol whose
 * capability was not declared fails the resolve and logs the denial.
 *
 * Enforcement window: the set of the mod currently inside ModInit().
 * ScriptLoader::LoadAll wraps each ModInit in pre/postInit callbacks;
 * app_mods.cpp and port.cpp push/pop the current mod's capabilities
 * through BeginMod/EndMod alongside the HookManager owner tagging.
 * Symbols resolved outside an init window are unrestricted — mods
 * overwhelmingly resolve during ModInit, and runtime resolution by a
 * misbehaving mod is the documented residual of in-process native mods.
 */
#include <string>
#include <vector>

namespace ssb64::mods::ModCapabilities {

/* Push the capability context for the mod about to run ModInit. Looks up
 * the archive whose manifest.Name == modName and stashes its declared
 * capabilities; unknown names get the unrestricted-legacy treatment. */
void BeginMod(const char* modName);

/* Clear the current capability context (post-ModInit). */
void EndMod();

/* May the current mod resolve `symbolName`? Consulted by
 * SymbolResolver::Resolve and HookManager::InstallHook. */
bool IsSymbolAllowed(const char* symbolName);

/* Read a manifest's capability list into a normalized set (lowercase,
 * unknown entries logged and dropped). Exposed for tests/UI display. */
std::vector<std::string> Normalize(const std::vector<std::string>& declared);

} // namespace ssb64::mods::ModCapabilities
