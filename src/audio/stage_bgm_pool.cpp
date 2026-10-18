#include "stage_bgm_pool.h"

#include <chrono>
#include <random>

// Stage BGM integer mappings derived from gmMusicID
constexpr uint32_t nSYAudioBGMPupupu = 0;   // Dream Land
constexpr uint32_t nSYAudioBGMZebes = 1;    // Planet Zebes
constexpr uint32_t nSYAudioBGMInishie = 2;  // Mushroom Kingdom
constexpr uint32_t nSYAudioBGMSector = 4;   // Sector Z
constexpr uint32_t nSYAudioBGMJungle = 5;   // Kongo Jungle
constexpr uint32_t nSYAudioBGMCastle = 6;   // Peach's Castle
constexpr uint32_t nSYAudioBGMYamabuki = 7; // Saffron City
constexpr uint32_t nSYAudioBGMYoster = 8;   // Yoshi's Island
constexpr uint32_t nSYAudioBGMHyrule = 9;   // Hyrule Castle
constexpr uint32_t nSYAudioBGMLast = 25;    // Final Destination
constexpr uint32_t nSYAudioBGMZako = 36;    // Battlefield
constexpr uint32_t nSYAudioBGMMetal = 37;   // Meta Crystal

namespace ssb64 {
namespace audio {

const uint32_t kStageBgmPool[] = {
    nSYAudioBGMPupupu,
    nSYAudioBGMZebes,
    nSYAudioBGMInishie,
    nSYAudioBGMSector,
    nSYAudioBGMJungle,
    nSYAudioBGMCastle,
    nSYAudioBGMYamabuki,
    nSYAudioBGMYoster,
    nSYAudioBGMHyrule,
    nSYAudioBGMZako,
    nSYAudioBGMMetal,
    nSYAudioBGMLast
};

const size_t kStageBgmPoolSize = sizeof(kStageBgmPool) / sizeof(kStageBgmPool[0]);

bool IsStageBgm(uint32_t bgm) {
    for (size_t i = 0; i < kStageBgmPoolSize; i++) {
        if (kStageBgmPool[i] == bgm) {
            return true;
        }
    }
    return false;
}

uint32_t RandomStageBgm() {
    static std::mt19937 sRng([] {
        // random_device alone may be deterministic on some libstdc++
        // builds; mix it with the clock.
        std::random_device rd;
        const auto ticks = static_cast<uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return rd() ^ ticks;
    }());
    static std::uniform_int_distribution<size_t> sDist(0, kStageBgmPoolSize - 1);
    return kStageBgmPool[sDist(sRng)];
}

} // namespace audio
} // namespace ssb64
