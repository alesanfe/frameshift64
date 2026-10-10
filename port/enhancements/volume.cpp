#include <libultraship/bridge/consolevariablebridge.h>

#include <cstdlib>

// Wrap everything in extern "C" so the C-based engine can link to these functions
extern "C" {

    // Volume level getters pulled from the UI cvars
    float port_get_master_volume() {
        // SSB64_MUTE=1: silent run (automated tests) without touching the
        // player's saved volume settings.
        static const bool sMuted = std::getenv("SSB64_MUTE") != nullptr;
        if (sMuted) {
            return 0.0f;
        }
        return CVarGetFloat("gSettings.Audio.MasterVolume", 1.0f);
    }

    float port_get_music_volume() {
        return CVarGetFloat("gSettings.Audio.MusicVolume", 1.0f);
    }

    float port_get_sfx_volume() {
        return CVarGetFloat("gSettings.Audio.SFXVolume", 1.0f);
    }

    float port_get_voice_volume() {
        return CVarGetFloat("gSettings.Audio.VoiceVolume", 1.0f);
    }

}
