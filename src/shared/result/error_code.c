#include "error_code.h"

static const char* const sNames[PORT_ERR__COUNT] = {
    "ok",
    "config",
    "filesystem",
    "asset",
    "rom_validation",
    "network",
    "protocol",
    "compatibility",
    "update",
    "platform",
    "internal",
};

const char* port_error_category_name(PortErrorCategory cat) {
    if (cat < 0 || cat >= PORT_ERR__COUNT) {
        return "unknown";
    }
    return sNames[cat];
}

int port_error_is_recoverable(PortErrorCategory cat) {
    switch (cat) {
        case PORT_ERR_CONFIG:
        case PORT_ERR_FILESYSTEM:
        case PORT_ERR_ASSET:
        case PORT_ERR_NETWORK:
        case PORT_ERR_UPDATE:
        case PORT_ERR_PLATFORM:
            return 1;
        default:
            /* OK isn't an error; rom/protocol/compatibility/internal need a
             * fix (valid dump, version bump, code change), not a retry. */
            return 0;
    }
}
