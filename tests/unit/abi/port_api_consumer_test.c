/* Real C consumer of the port_* ABI (post-audit plan item 44).
 *
 * Compiled as C — not C++ — so a C++ token leaking into the public
 * headers, or a missing `extern "C"` guard on port_abi_version(), fails
 * at compile/link time rather than at runtime. A static audit like
 * check_abi_surface.py can verify signatures; only this binary proves
 * the contract actually builds and links from the C side.
 *
 * Exit 0: headers are C-clean, the linked runtime reports the header's
 * own version, and the PortResult enum keeps its documented layout. */

#include "port_api_version.h"
#include "port_api_errors.h"

#include <stdio.h>

/* ── compile-time contract (integer constant expressions) ────────── */

/* Version packing: PORT_ABI_CURRENT == MAJOR<<8 | MINOR. The runtime
 * stamps this into replay headers and crash bundles; a drift between
 * the macro and the function would silently corrupt those stamps. */
#if PORT_ABI_CURRENT != ((PORT_ABI_VERSION_MAJOR << 8) | PORT_ABI_VERSION_MINOR)
#error "PORT_ABI_CURRENT does not match MAJOR<<8|MINOR packing"
#endif

/* PortResult layout is contractual: PORT_OK keeps the legacy 0
 * convention and every failure code is negative so `if (rc)` keeps
 * meaning "error" on call sites written before the enum existed.
 * (Enum constants are not visible to the preprocessor — these must be
 * _Static_assert, not #if.) */
_Static_assert(PORT_OK == 0, "PORT_OK must remain 0 (legacy success convention)");
_Static_assert(PORT_ERR_FAILED < 0 && PORT_ERR_INVALID < 0 &&
               PORT_ERR_UNSUPPORTED < 0 && PORT_ERR_NOT_FOUND < 0 &&
               PORT_ERR_BUSY < 0 && PORT_ERR_NO_MEM < 0 && PORT_ERR_IO < 0,
               "PortResult failure codes must stay negative");
_Static_assert(sizeof(PortResult) == sizeof(int),
               "PortResult must stay int-sized across the C/C++ boundary");

int main(void)
{
    unsigned int linked = port_abi_version();
    int failures = 0;

    /* The linked runtime must agree with the header this binary was
     * compiled against — the whole point of the stamp. */
    if (linked != PORT_ABI_CURRENT) {
        printf("FAIL: port_abi_version()=0x%04x but header says 0x%04x\n",
               linked, (unsigned int)PORT_ABI_CURRENT);
        failures++;
    }

    /* Enum width must be int-compatible on both sides of the boundary —
     * a C++ scoped enum or explicit underlying type would break this. */
    if (sizeof(PortResult) != sizeof(int)) {
        printf("FAIL: sizeof(PortResult)=%zu != sizeof(int)\n",
               sizeof(PortResult));
        failures++;
    }

    /* Distinct, stable numeric values — replay/crash parsers may
     * serialize these. */
    if (PORT_ERR_FAILED != -1 || PORT_ERR_INVALID != -2 ||
        PORT_ERR_UNSUPPORTED != -3 || PORT_ERR_NOT_FOUND != -4 ||
        PORT_ERR_BUSY != -5 || PORT_ERR_NO_MEM != -6 || PORT_ERR_IO != -7) {
        printf("FAIL: PortResult numeric values changed\n");
        failures++;
    }

    if (failures != 0) {
        printf("port_api_consumer_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("port_api_consumer_test: ABI 0x%04x, %s, OK\n",
           linked, "C consumer linked extern \"C\" symbol");
    return 0;
}
