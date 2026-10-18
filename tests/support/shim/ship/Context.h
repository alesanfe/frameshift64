/* Test double for components/libultraship/include/ship/Context.h.
 *
 * Unit tests that compile production TUs referencing Ship::Context (the
 * netplay persistence stores call GetPathRelativeToAppDirectory when no
 * SSB64_SAVE_PATH override is set) link this shim instead of libultraship:
 * the real header pulls spdlog + Audio.h and the .lib carries unresolved
 * port_* hooks plus a static CRT that conflicts with test binaries.
 * Keep the declarations in sync with the methods the tested TUs call —
 * tests provide the definitions themselves.
 */
#pragma once

#include <string>

namespace Ship {

class Context {
  public:
    static std::string GetPathRelativeToAppDirectory(const std::string path,
                                                     std::string appName = "");
};

} // namespace Ship
