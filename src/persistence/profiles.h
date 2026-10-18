#ifndef PORT_PROFILES_H
#define PORT_PROFILES_H

/* §35 — named player profiles for controls & gameplay mods.
 *
 * A profile snapshots the CVar trees that define "how I play":
 *   gControllers.*  — per-port button/stick/rumble mappings (LUS)
 *   gEnhancements.* — gameplay assists/rules modifiers
 *   gMods.*         — per-mod settings (script mods may register CVars here)
 * Global presentation/system CVars (language, theme, narration, video,
 * audio, netplay) deliberately stay out — a profile is about playstyle,
 * not app configuration.
 *
 * Profiles live as JSON files in <save_dir>/profiles/<name>.json. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Persist the current controls/mods CVars under `name`.
 * Name: [A-Za-z0-9 _-], 1-32 chars. Returns 0 on success, -1 on error. */
int port_profile_save(const char* name);

/* Restore a saved profile: writes the CVars back, reloads the four
 * controller ports' mappings from config, and saves the CVar store.
 * Returns 0 on success, -1 if the file is missing/corrupt. */
int port_profile_load(const char* name);

/* Delete a profile file. Returns 0 on success, -1 if missing. */
int port_profile_delete(const char* name);

/* List profile names as a '\n'-separated list into buf (NUL-terminated).
 * Returns the number of profiles (buf may be truncated if cap is small). */
size_t port_profile_list(char* buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PORT_PROFILES_H */
