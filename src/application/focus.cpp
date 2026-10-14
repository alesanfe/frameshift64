#include "focus.h"

#include <cstdlib>

#include "libultraship/bridge/consolevariablebridge.h"
#include "bridge/hooks/Events.h"

namespace ssb64 {

static float sSavedMasterVolume = -1.0f;

static void OnWindowFocus(IEvent* raw) {
    auto* ev = reinterpret_cast<WindowFocusEvent*>(raw);

    if (!ev->Focused) {
        if (sSavedMasterVolume < 0.0f && CVarGetInteger("gSettings.FocusControl.MuteOnFocusLoss", 0)) {
            sSavedMasterVolume = CVarGetFloat("gSettings.Audio.MasterVolume", 1.0f);
            CVarSetFloat("gSettings.Audio.MasterVolume", 0.0f);
        }
    } else {
        if (sSavedMasterVolume >= 0.0f) {
            CVarSetFloat("gSettings.Audio.MasterVolume", sSavedMasterVolume);
            sSavedMasterVolume = -1.0f;
        }
    }
}

/* Quitting while unfocused leaves MasterVolume=0 in the CVar set, and
 * Config::Save() (run later, in ~Context) would persist it — next launch
 * boots muted with no indication why. atexit handlers run in reverse
 * registration order, and this one is registered during PortInit (after
 * Context exists), so it fires before ~Context's save. */
static void RestoreMutedVolumeAtExit() {
    if (sSavedMasterVolume >= 0.0f) {
        CVarSetFloat("gSettings.Audio.MasterVolume", sSavedMasterVolume);
        sSavedMasterVolume = -1.0f;
    }
}

void RegisterFocusListener() {
    REGISTER_LISTENER(WindowFocusEvent, EVENT_PRIORITY_NORMAL, OnWindowFocus);
    std::atexit(RestoreMutedVolumeAtExit);
}

} // namespace ssb64
