#pragma once

/*
 * port_api_errors.h — common result codes for the port_* ABI.
 *
 * Historical port_* functions return ad-hoc codes (0/-1/NULL). New code
 * and any signature touched at a MAJOR bump uses these. Codes are
 * negative on failure so existing `if (rc)` checks keep working.
 */

typedef enum PortResult {
    PORT_OK = 0,            /* success; also the legacy "0" convention */
    PORT_ERR_FAILED = -1,   /* generic failure (legacy "-1" convention) */
    PORT_ERR_INVALID = -2,  /* null/bad argument or magic/size mismatch */
    PORT_ERR_UNSUPPORTED = -3, /* valid input, feature not available */
    PORT_ERR_NOT_FOUND = -4, /* requested object/file/record absent */
    PORT_ERR_BUSY = -5,     /* object in use; caller may retry */
    PORT_ERR_NO_MEM = -6,   /* allocation failed */
    PORT_ERR_IO = -7        /* filesystem/transport failure */
} PortResult;
