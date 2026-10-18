/* schema.h — typed config schema for the port's `g*` CVars (TD-005).
 *
 * The LUS CVar store is an untyped bag: every call site carries its own
 * default, a typo'd key silently reads the default forever, and a user-
 * edited config file can smuggle in keys nothing recognizes. This module
 * is the single catalog of every first-party key:
 *
 *   - PortConfigValidate()      clamps out-of-range values at boot and
 *                               logs what it corrected.
 *   - PortConfigUnknownKeys()   warns on `g*`-prefixed keys in the store
 *                               that no schema entry declares — catches
 *                               typos and stale keys from older versions.
 *   - affects_gameplay metadata marks keys whose value a netplay peer
 *                               could legitimately care about (fairness
 *                               or determinism) — advisory today, the
 *                               source of truth when rules negotiation
 *                               moves off hardcoded fields.
 *
 * Registering defaults stays at the existing call sites — the schema
 * deliberately does NOT redeclare defaults to avoid drift between the
 * catalog and the menu/registration code.
 */
#pragma once

#include <string>
#include <vector>

namespace ssb64::config {

enum class KeyType { Bool, Int, Float, String };

struct SchemaEntry {
    const char* key;
    KeyType type;
    double lo, hi;          /* inclusive clamp range (Bool is 0..1) */
    bool affects_gameplay;  /* sim-visible or peer-negotiated value */
};

/* Run at boot, after CVarLoad(). Returns the number of values clamped. */
int PortConfigValidate();

/* Warn on `g*`-prefixed store keys not in the schema. Returns the count. */
int PortConfigUnknownKeys();

/* Stamp the current schema version after validation — future migration
 * hooks compare the stored value to detect config upgrades. */
void PortConfigStampVersion();

/* Look up a key's schema entry, or nullptr when undeclared. */
const SchemaEntry* PortConfigSchemaFind(const char* key);

/* All keys flagged affects_gameplay, for netplay compat/manifest use. */
std::vector<std::string> PortConfigGameplayKeys();

} // namespace ssb64::config
