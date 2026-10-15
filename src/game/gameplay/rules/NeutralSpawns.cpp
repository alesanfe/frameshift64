#include "enhancements.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include "networking/netplay/domain/netrules.h"

namespace ssb64 {
    namespace enhancements {

        const char* NeutralSpawnsCVarName() {
            return "gEnhancements.NeutralSpawns";
        }

    } // namespace enhancements
} // namespace ssb64

extern "C" int port_enhancement_neutral_spawns(void) {
    int online;
    if (port_netrules_override(PORT_NETRULE_NEUTRAL_SPAWNS, 0, &online)) {
        return online;
    }
    return CVarGetInteger(ssb64::enhancements::NeutralSpawnsCVarName(), 0) != 0;
}
