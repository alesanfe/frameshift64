#pragma once

/*
 * port_api_version.h — versioned contract for the port_* ABI.
 *
 * Every public port_* header exposes C symbols shared between the port
 * runtime (src/) and the decomp side (components/decomp/). Symbols are
 * stable: existing function signatures never change in place; changes
 * come as a version bump below or a new symbol alongside the old one.
 *
 * Versioning rules:
 *  - MAJOR: an existing signature changed or a symbol was removed.
 *    Callers built against an older MAJOR must not link.
 *  - MINOR: additive only (new symbols, new flags in existing enums,
 *    new trailing fields in size-tagged structs). Old callers keep
 *    working without recompilation.
 *
 * ABI_VERSION is embedded in the build via port_abi_version() so tools
 * (replays, crash bundles, mod loaders) can refuse mismatched builds.
 */

#define PORT_ABI_VERSION_MAJOR 1
#define PORT_ABI_VERSION_MINOR 0

/* 0xMMNN packed form for cheap comparisons and wire stamps. */
#define PORT_ABI_CURRENT \
    ((PORT_ABI_VERSION_MAJOR << 8) | PORT_ABI_VERSION_MINOR)

#ifdef __cplusplus
extern "C" {
#endif

/* Returns PORT_ABI_CURRENT of the *linked* runtime — never trust a
 * caller-supplied version for safety checks, always call this. */
unsigned int port_abi_version(void);

/* Compile-time guard a consumer can use to refuse an older header:
 *     static_assert(PORT_ABI_CURRENT >= 0x0100, "port ABI too old"); */

#ifdef __cplusplus
}
#endif
