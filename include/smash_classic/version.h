/*
 * smash_classic/version.h — independent compatibility-surface versions.
 *
 * Every external contract of this project has its own version. They are NOT
 * required to move together: a netplay message change must not force a save
 * bump, and a save-format bump must not break the lobby. See
 * docs/compatibility/policy.md for the rules each surface follows.
 *
 * Current values mirror what the code actually enforces today. A constant
 * marked "specification target" is reserved: the enforcement lands with the
 * feature, but the number space is claimed now so it can never be reused.
 */
#ifndef SMASH_CLASSIC_VERSION_H
#define SMASH_CLASSIC_VERSION_H

#include <stdint.h>

/* Application release (semver tag, e.g. "v1.4.1" in VERSION). Not a wire
 * value — every wire surface below has its own field. */
#define SMASH_CLASSIC_APP_VERSION_STR "1.4.1"

/* Peer-to-peer input/rollback packet format — decomp/src/sys/netpeer.c
 * `SYNETPEER_VERSION`. Wire field: u16 after the magic. Both peers must
 * match exactly or packets are dropped at the parser. */
#define SMASH_NETPLAY_PROTOCOL_VERSION 3u

/* Lobby signaling protocol — services/netplay-lobby `?v=` query param.
 * 1 = legacy host/join pair, 2 = 4-player slots + star routing + spectators.
 * A room's version is fixed by the host; mismatched joiners get
 * {type:"error", reason:"version-mismatch"}. */
#define SMASH_LOBBY_PROTOCOL_VERSION 2u

/* Save file format — src/core/port_save.cpp. Bumped when the on-disk layout
 * changes; the loader must migrate or reject older versions. */
#define SMASH_SAVE_FORMAT_VERSION 1u

/* Local configuration format — LUS CVar/config storage. Bumped when key
 * semantics change; migrations land with the schema work
 * (docs/engineering/migration-status.md). */
#define SMASH_CONFIG_FORMAT_VERSION 1u

/* Mod API — manifest.json + hook/symbol surface consumed by mods/. Bumped
 * when the SDK contract changes; ModRegistry must refuse mods requesting a
 * newer apiVersion than it implements. */
#define SMASH_MOD_API_VERSION 1u

/* Extracted-asset archive contract — guarded today by the baked recipe hash
 * (torch SHA + config.yml + extraction yamls → .recipe sidecar) which auto
 * re-extracts on mismatch. The numeric version is reserved for when the o2r
 * schema itself is versioned. */
#define SMASH_ASSET_FORMAT_VERSION 1u

#endif /* SMASH_CLASSIC_VERSION_H */
