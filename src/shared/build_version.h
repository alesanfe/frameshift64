#ifndef SSB64_SHARED_BUILD_VERSION_H
#define SSB64_SHARED_BUILD_VERSION_H

/* Compile-time build identity, injected by CMake SSB64_COMPILE_DEFS.
 * The fallbacks cover non-CMake consumers (test stubs, external tools
 * compiling a TU directly) so they never see an undefined macro — keep
 * both fallbacks identical everywhere by including this header rather
 * than re-declaring them per file. */

#ifndef BATTLESHIP_CURRENT_VERSION
#define BATTLESHIP_CURRENT_VERSION "dev"
#endif
#ifndef BATTLESHIP_BUILD_SHA
#define BATTLESHIP_BUILD_SHA "dev"
#endif

#endif /* SSB64_SHARED_BUILD_VERSION_H */
