/* Minimal emscripten/console.h shim — see ../emscripten.h. */
#pragma once

#include "../emscripten.h"

#ifdef __cplusplus
extern "C" {
#endif

void emscripten_errf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
