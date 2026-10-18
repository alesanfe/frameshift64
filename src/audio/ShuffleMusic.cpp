#include "enhancements.h"
#include "stage_bgm_pool.h"
#include <libultraship/bridge/consolevariablebridge.h>

#include <cstdint>

constexpr const char* kShuffleMusicCVar = "gEnhancements.ShuffleMusic";

// -1 means the user has not locked in a manual track
extern "C" {
    int32_t gManualMusicSelection = -1;
}

extern "C" uint32_t port_enhancement_shuffle_music(uint32_t requested_bgm) {
    // 1. highest priority: Manual Selection via Double-Tap UI
    if (gManualMusicSelection != -1 && ssb64::audio::IsStageBgm(requested_bgm)) {
        return (uint32_t)gManualMusicSelection;
    }

    // 2. middle priority: Shuffle Music
    if (CVarGetInteger(kShuffleMusicCVar, 0) != 0 && ssb64::audio::IsStageBgm(requested_bgm)) {
        return ssb64::audio::RandomStageBgm();
    }

    // 3. lowest priority: Default Stage BGM
    return requested_bgm;
}

namespace ssb64 {
    namespace enhancements {
        const char* ShuffleMusicCVarName() {
            return kShuffleMusicCVar;
        }
    } // namespace enhancements
} // namespace ssb64
