#pragma once

/* §9 accessibility — screen-reader narration for the port menu. Speaks a
 * short announcement through the OS voice (Windows SAPI). A no-op when the
 * gSettings.Narration CVar is off or on platforms without a voice backend. */

#ifdef __cplusplus
extern "C" {
#endif

void port_narrate(const char* text); /* announce `text` (UTF-8) if narration is on */
int port_narration_available(void);  /* 1 when a voice backend exists on this platform */

#ifdef __cplusplus
}
#endif
