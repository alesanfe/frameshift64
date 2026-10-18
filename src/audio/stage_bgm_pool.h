#pragma once

#include <cstddef>
#include <cstdint>

namespace ssb64 {
namespace audio {

// The 12 stage-selectable BGM ids (gmMusicID). Single source of truth —
// this pool used to be declared twice, in ShuffleMusic.cpp and
// MusicSelection.cpp, with two different literal styles.
extern const uint32_t kStageBgmPool[];
extern const size_t kStageBgmPoolSize;

bool IsStageBgm(uint32_t bgm);

// Uniform pick from the pool on a module-private PRNG seeded once.
// Both callers previously used std::rand(): MusicSelection never seeded
// it (identical "random" sequence every launch) and ShuffleMusic seeded
// lazily with time(), so the two features shared the CRT's global
// sequence — consuming each other's draws.
uint32_t RandomStageBgm();

} // namespace audio
} // namespace ssb64
