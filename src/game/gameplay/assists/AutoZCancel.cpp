#include "enhancements.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include "netrules.h"

namespace ssb64 {
    namespace enhancements {

        const char* AutoZCancelCVarName() {
            return "gEnhancements.CasualRules.AutoZCancel";
        }

    } // namespace enhancements
} // namespace ssb64

extern "C" int port_enhancement_IsAutoZCancelEnabled(void) {
    int online;
    if (port_netrules_override(PORT_NETRULE_AUTO_Z_CANCEL, 0, &online)) {
        return online;
    }
    return CVarGetInteger(ssb64::enhancements::AutoZCancelCVarName(), 0) != 0;
}
