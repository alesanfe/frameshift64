/* Minimal emscripten.h shim for host-compiled tests of __EMSCRIPTEN__
 * code paths (tests/unit/netplay/transport/ws_web_test.cpp). ONLY on
 * that test's include path — never installed into a real emsdk build.
 * The backing fake lives in fake_ems.cpp; the clock is test-driven so
 * retry/cadence logic is deterministic. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef int EM_BOOL;
#define EM_TRUE 1
#define EM_FALSE 0

/* ms since "page load" — the fake returns the test-controlled clock. */
double emscripten_get_now(void);

/* Evaluates a JS expression and returns its string result (malloc'd —
 * caller frees). The fake either returns a canned value
 * (fake_ems::SetScriptResult) or, when FAKE_EMS_LOCATION_SEARCH is set,
 * executes the REAL expression under node with a stubbed
 * globalThis.location — exercising the production JS, not a mock. */
char *emscripten_run_script_string(const char *script);

#ifdef __cplusplus
}
#endif
