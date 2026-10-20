/* C11 consumer of the port_* ABI contract headers — compiled as C, not
 * C++, so any C++ construct leaking into the decomp-facing contract
 * fails the build outright. See src/game/engine/port_api/README.md. */
#include "port_api_version.h"
#include "port_api_errors.h"

#include <assert.h>
#include <stdint.h>

/* Version macros must be integer expressions usable in static asserts. */
_Static_assert(PORT_ABI_VERSION_MAJOR >= 1, "major must be >= 1");
_Static_assert((PORT_ABI_CURRENT >> 8) == PORT_ABI_VERSION_MAJOR,
               "packed major mismatch");
_Static_assert((PORT_ABI_CURRENT & 0xFF) == PORT_ABI_VERSION_MINOR,
               "packed minor mismatch");

/* PortResult layout is contractual: PORT_OK keeps the legacy 0
 * convention and every failure code is negative so `if (rc)` keeps
 * meaning "error" on call sites written before the enum existed.
 * (Enum constants are invisible to the preprocessor — _Static_assert,
 * not #if.) */
_Static_assert(PORT_OK == 0, "PORT_OK must remain 0");
_Static_assert(PORT_ERR_FAILED < 0 && PORT_ERR_INVALID < 0 &&
               PORT_ERR_UNSUPPORTED < 0 && PORT_ERR_NOT_FOUND < 0 &&
               PORT_ERR_BUSY < 0 && PORT_ERR_NO_MEM < 0 && PORT_ERR_IO < 0,
               "error codes must stay negative");
/* Replay/crash parsers may serialize these — pin the numeric values. */
_Static_assert(PORT_ERR_FAILED == -1 && PORT_ERR_INVALID == -2 &&
               PORT_ERR_UNSUPPORTED == -3 && PORT_ERR_NOT_FOUND == -4 &&
               PORT_ERR_BUSY == -5 && PORT_ERR_NO_MEM == -6 &&
               PORT_ERR_IO == -7,
               "PortResult numeric values changed — that is a MAJOR bump");
/* Enum width must be int-compatible on both sides of the boundary —
 * a C++ scoped enum or explicit underlying type would break this. */
_Static_assert(sizeof(PortResult) == sizeof(int),
               "PortResult must stay int-sized");

/* The prototypes must be valid C declarations (not C++ overloads). */
extern unsigned int port_abi_version(void);
extern unsigned int port_abi_check_compatibility(unsigned int, unsigned int);

int main(void) {
    /* Header-vs-linked-runtime agreement — the point of the stamp. */
    assert(port_abi_version() == PORT_ABI_CURRENT);
    /* Negotiation: same version OK; wrong MAJOR or newer MINOR rejected. */
    assert(port_abi_check_compatibility(PORT_ABI_CURRENT, 0) == 0);
    assert(port_abi_check_compatibility(
               (PORT_ABI_VERSION_MAJOR + 1) << 8, 0) != 0);
    assert(port_abi_check_compatibility(
               PORT_ABI_CURRENT + 1, 0) != 0);
    return 0;
}
