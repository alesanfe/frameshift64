#include "port_api_version.h"
#include "port_api_errors.h"

unsigned int port_abi_version(void)
{
    return PORT_ABI_CURRENT;
}

unsigned int port_abi_check_compatibility(unsigned int consumer_version,
                                          unsigned int consumer_struct_size)
{
    (void)consumer_struct_size;
    const unsigned int cmaj = (consumer_version >> 8) & 0xFF;
    const unsigned int cmin = consumer_version & 0xFF;
    if (cmaj != PORT_ABI_VERSION_MAJOR || cmin > PORT_ABI_VERSION_MINOR) {
        return (unsigned int)PORT_ERR_UNSUPPORTED;
    }
    return (unsigned int)PORT_OK;
}
