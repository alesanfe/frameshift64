#ifndef PORT_ERROR_CODE_H
#define PORT_ERROR_CODE_H

/* Unified error taxonomy — the stable categories every module reports
 * through. See docs/architecture/error-model.md for the disposition
 * policy (which categories toast, log, bundle, or force safe mode).
 *
 * C-compatible on purpose: the decomp-facing port_* ABI and C shims
 * can return category codes directly; C++ modules wrap them in
 * ssb64::Result<T> (result.h). */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PortErrorCategory {
    PORT_ERR_OK = 0,           /* not an error — used by Result for success */

    PORT_ERR_CONFIG,           /* malformed/unsupported configuration value */
    PORT_ERR_FILESYSTEM,       /* path, permission, ENOSPC, removable media */
    PORT_ERR_ASSET,            /* o2r/pack missing, truncated, bad layout */
    PORT_ERR_ROM_VALIDATION,   /* ROM rejected: wrong region/hash/size */
    PORT_ERR_NETWORK,          /* connect/dns/tls/timeout — transport-level */
    PORT_ERR_PROTOCOL,         /* well-formed transport, bad/unsupported msg */
    PORT_ERR_COMPATIBILITY,    /* platform or save-format version mismatch */
    PORT_ERR_UPDATE,           /* updater: manifest/signature/download state */
    PORT_ERR_PLATFORM,         /* OS call failed with no better category */
    PORT_ERR_INTERNAL,         /* invariant violation — bug, not environment */

    PORT_ERR__COUNT            /* sentinel — keep last, never assign */
} PortErrorCategory;

/* Stable lowercase name for logs/manifests ("config", "protocol", ...).
 * Returns "unknown" for out-of-range input — never NULL. */
const char* port_error_category_name(PortErrorCategory cat);

/* Default disposition (policy table in error-model.md): whether the
 * category is normally user-recoverable. Internal/rom errors are not —
 * they mean fix the build or supply a valid dump. */
int port_error_is_recoverable(PortErrorCategory cat);

#ifdef __cplusplus
}
#endif

#endif /* PORT_ERROR_CODE_H */
