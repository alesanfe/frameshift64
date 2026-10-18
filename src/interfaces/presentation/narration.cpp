#include "narration.h"

#include <libultraship/bridge/consolevariablebridge.h>

#if defined(_WIN32) && !defined(BATTLESHIP_UWP)

#include <windows.h>
#include <sapi.h>
#include <cwchar>

namespace {

/* SAPI voice, created lazily on first use. Speak() calls are async and
 * purge the pending queue so fast focus moves announce only the latest
 * widget instead of stuttering through a backlog. */
ISpVoice* GetVoice() {
    static ISpVoice* sVoice = nullptr;
    static bool sTried = false;
    if (!sTried) {
        sTried = true;
        /* The process may already hold a COM apartment (SDL, updater);
         * CoInitializeEx is idempotent — S_FALSE/RPC_E_CHANGED_MODE just
         * means we ride the existing one. */
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&sVoice)))) {
            sVoice = nullptr;
        }
    }
    return sVoice;
}

} // namespace

extern "C" int port_narration_available(void) {
    return GetVoice() != nullptr ? 1 : 0;
}

extern "C" void port_narrate(const char* text) {
    if (text == nullptr || text[0] == '\0' ||
        CVarGetInteger("gSettings.Narration", 0) == 0) {
        return;
    }
    ISpVoice* voice = GetVoice();
    if (voice == nullptr) {
        return;
    }
    wchar_t w[512];
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 512) <= 0) {
        return;
    }
    voice->Speak(w, SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
}

#else

extern "C" int port_narration_available(void) {
    return 0;
}

extern "C" void port_narrate(const char*) {
    /* no voice backend on this platform yet */
}

#endif
