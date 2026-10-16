#include "ship/controller/controldevice/controller/mapping/sdl/SDLRumbleMapping.h"

#include "ship/config/ConsoleVariable.h"
#include "ship/utils/StringHelper.h"
#include "ship/Context.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include <array>
#include <unordered_map>
#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace {
#if defined(__SWITCH__)
constexpr uint32_t kRumbleDurationMs = 30000;
constexpr float kSwitchRumbleLowFrequencyHz = 160.0f;
constexpr float kSwitchRumbleHighFrequencyHz = 320.0f;
#else
constexpr uint32_t kRumbleDurationMs = 0;
#endif

void StartGamepadRumble(SDL_GameController* gamepad, uint16_t lowFrequency, uint16_t highFrequency) {
    if (SDL_GameControllerRumble(gamepad, lowFrequency, highFrequency, kRumbleDurationMs) == 0) {
        return;
    }

    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(gamepad);
    if (joystick != nullptr) {
        SDL_JoystickRumble(joystick, lowFrequency, highFrequency, kRumbleDurationMs);
    }
}

void StopGamepadRumble(SDL_GameController* gamepad) {
    if (SDL_GameControllerRumble(gamepad, 0, 0, 0) == 0) {
        return;
    }

    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(gamepad);
    if (joystick != nullptr) {
        SDL_JoystickRumble(joystick, 0, 0, 0);
    }
}

#if defined(__SWITCH__)
constexpr uint16_t kNintendoVendorId = 0x057E;
constexpr uint16_t kSwitchProProductId = 0x2009;

HidNpadIdType GetNpadIdForGamepad(SDL_GameController* gamepad, uint8_t portIndex) {
    const int32_t playerIndex = SDL_GameControllerGetPlayerIndex(gamepad);
    if (playerIndex >= 0 && playerIndex <= 7) {
        return static_cast<HidNpadIdType>(HidNpadIdType_No1 + playerIndex);
    }

    // Detached Joy-Cons may not report a player index; fall back to port index.
    // This ensures Port 0 → No1, Port 1 → No2, etc., preventing cross-port bleed.
    if (portIndex <= 7) {
        return static_cast<HidNpadIdType>(HidNpadIdType_No1 + portIndex);
    }

    return HidNpadIdType_No1;
}

struct SwitchVibrationRouting {
    HidNpadStyleTag styleTag;
    int32_t handleCount;
};

struct SwitchVibrationRoutingCandidates {
    std::array<SwitchVibrationRouting, 3> routings;
    size_t count;
};

SwitchVibrationRoutingCandidates MakeRoutingCandidates(const SwitchVibrationRouting& first,
                                                      const SwitchVibrationRouting& second,
                                                      const SwitchVibrationRouting& third) {
    SwitchVibrationRoutingCandidates candidates;
    candidates.routings = { first, second, third };
    candidates.count = 3;
    return candidates;
}

bool IsLikelySwitchProController(SDL_GameController* gamepad) {
    if (SDL_GameControllerGetType(gamepad) == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO) {
        return true;
    }

    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(gamepad);
    if (joystick != nullptr) {
        const uint16_t vendor = SDL_JoystickGetVendor(joystick);
        const uint16_t product = SDL_JoystickGetProduct(joystick);
        if (vendor == kNintendoVendorId && product == kSwitchProProductId) {
            return true;
        }
    }

    const char* name = SDL_GameControllerName(gamepad);
    return name != nullptr && SDL_strstr(name, "Pro Controller") != nullptr;
}

const char* GetSwitchControllerTypeName(SDL_GameController* gamepad) {
    switch (SDL_GameControllerGetType(gamepad)) {
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
        return "SwitchPro";
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        return "JoyConLeft";
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        return "JoyConRight";
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return "JoyConPair";
    default:
        return "Other";
    }
}

SwitchVibrationRoutingCandidates GetSwitchVibrationRoutingCandidates(SDL_GameController* gamepad, uint8_t portIndex) {
    SwitchVibrationRoutingCandidates candidates =
        MakeRoutingCandidates({ HidNpadStyleTag_NpadJoyDual, 2 }, { HidNpadStyleTag_NpadHandheld, 2 },
                              { HidNpadStyleTag_NpadFullKey, 2 });

    const auto type = SDL_GameControllerGetType(gamepad);

    if (type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT) {
        candidates = MakeRoutingCandidates({ HidNpadStyleTag_NpadJoyLeft, 1 }, { HidNpadStyleTag_NpadHandheld, 2 },
                                           { HidNpadStyleTag_NpadFullKey, 2 });
        return candidates;
    }
    if (type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT) {
        candidates = MakeRoutingCandidates({ HidNpadStyleTag_NpadJoyRight, 1 },
                                           { HidNpadStyleTag_NpadHandheld, 2 },
                                           { HidNpadStyleTag_NpadFullKey, 2 });
        return candidates;
    }
    if (type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO) {
        candidates = MakeRoutingCandidates({ HidNpadStyleTag_NpadFullKey, 2 }, { HidNpadStyleTag_NpadJoyDual, 2 },
                                           { HidNpadStyleTag_NpadHandheld, 2 });
        return candidates;
    }
    if (type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) {
        if (portIndex == 0) {
            // Keep handheld-first for P1 attached mode compatibility.
            candidates = MakeRoutingCandidates({ HidNpadStyleTag_NpadHandheld, 2 }, { HidNpadStyleTag_NpadJoyDual, 2 },
                                               { HidNpadStyleTag_NpadFullKey, 2 });
        } else {
            // Detached multiplayer should target JoyDual first so both Joy-Con motors run.
            candidates = MakeRoutingCandidates({ HidNpadStyleTag_NpadJoyDual, 2 }, { HidNpadStyleTag_NpadHandheld, 2 },
                                               { HidNpadStyleTag_NpadFullKey, 2 });
        }
        return candidates;
    }

    const char* name = SDL_GameControllerName(gamepad);
    if (name != nullptr && SDL_strstr(name, "Combined Joy-Cons") != nullptr) {
        if (portIndex == 0) {
            // Keep handheld-first for P1 attached mode compatibility.
            candidates = MakeRoutingCandidates({ HidNpadStyleTag_NpadHandheld, 2 }, { HidNpadStyleTag_NpadJoyDual, 2 },
                                               { HidNpadStyleTag_NpadFullKey, 2 });
        } else {
            // Detached multiplayer should target JoyDual first so both Joy-Con motors run.
            candidates = MakeRoutingCandidates({ HidNpadStyleTag_NpadJoyDual, 2 }, { HidNpadStyleTag_NpadHandheld, 2 },
                                               { HidNpadStyleTag_NpadFullKey, 2 });
        }
    } else if (name != nullptr && SDL_strstr(name, "Switch Controller") != nullptr) {
        // SDL type detection is incomplete on Switch. Route based on what motors are
        // actually available. Default to handheld-first on port 0 for backward compat
        // with attached Joy-Cons, but the runtime styleSet check will redirect Pro to FullKey.
        if (portIndex == 0) {
            candidates = MakeRoutingCandidates({ HidNpadStyleTag_NpadHandheld, 2 }, { HidNpadStyleTag_NpadFullKey, 2 },
                                               { HidNpadStyleTag_NpadJoyDual, 2 });
        } else {
            candidates = MakeRoutingCandidates({ HidNpadStyleTag_NpadJoyDual, 2 }, { HidNpadStyleTag_NpadFullKey, 2 },
                                               { HidNpadStyleTag_NpadHandheld, 2 });
        }
    }

    return candidates;
}

bool SendSwitchHidVibration(SDL_GameController* gamepad, uint8_t portIndex, float lowAmplitude, float highAmplitude) {
    HidVibrationDeviceHandle handles[2];
    const HidNpadIdType primaryNpadId = GetNpadIdForGamepad(gamepad, portIndex);
    const char* controllerName = SDL_GameControllerName(gamepad);
    const int32_t playerIndex = SDL_GameControllerGetPlayerIndex(gamepad);
    const auto controllerType = SDL_GameControllerGetType(gamepad);
    const auto* controllerTypeName = GetSwitchControllerTypeName(gamepad);
    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(gamepad);
    const uint16_t vendor = (joystick != nullptr) ? SDL_JoystickGetVendor(joystick) : 0;
    const uint16_t product = (joystick != nullptr) ? SDL_JoystickGetProduct(joystick) : 0;
    const uint32_t primaryStyleSet = hidGetNpadStyleSet(primaryNpadId);
    const uint32_t handheldStyleSet = hidGetNpadStyleSet(HidNpadIdType_Handheld);

    // On Switch, SDL type detection is incomplete. Runtime check: if the primary npad
    // doesn't have handheld motors but does have FullKey/JoyDual, and we're on port 0,
    // skip handheld-first fallback and go straight to what the controller has.
    auto candidates = GetSwitchVibrationRoutingCandidates(gamepad, portIndex);
    if (portIndex == 0 && primaryNpadId != HidNpadIdType_Handheld) {
        const bool primaryHasHandheld = (primaryStyleSet & static_cast<uint32_t>(HidNpadStyleTag_NpadHandheld)) != 0;
        const bool primaryHasFullKey = (primaryStyleSet & static_cast<uint32_t>(HidNpadStyleTag_NpadFullKey)) != 0;
        const bool primaryHasJoyDual = (primaryStyleSet & static_cast<uint32_t>(HidNpadStyleTag_NpadJoyDual)) != 0;

        if (!primaryHasHandheld && (primaryHasFullKey || primaryHasJoyDual)) {
            // Primary npad has controller-local motors but not handheld dock motors.
            // Swap handheld to be last priority, not first.
            if (candidates.routings[0].styleTag == HidNpadStyleTag_NpadHandheld) {
                SwitchVibrationRouting tmp = candidates.routings[0];
                candidates.routings[0] = candidates.routings[1];
                candidates.routings[1] = tmp;
            }
        }
    }

    SPDLOG_INFO(
        "Switch rumble begin: port={} name='{}' type={} playerIndex={} vid=0x{:04X} pid=0x{:04X} primaryNpadId={} primaryStyleSet=0x{:X} handheldStyleSet=0x{:X} lowAmp={:.3f} highAmp={:.3f}",
        static_cast<uint32_t>(portIndex), (controllerName != nullptr) ? controllerName : "(unknown)",
        controllerTypeName, playerIndex, vendor, product, static_cast<uint32_t>(primaryNpadId), primaryStyleSet,
        handheldStyleSet, lowAmplitude, highAmplitude);

    for (size_t i = 0; i < candidates.count; i++) {
        const SwitchVibrationRouting& routing = candidates.routings[i];
        HidNpadIdType npadCandidates[2] = { primaryNpadId, primaryNpadId };
        size_t npadCandidateCount = 1;
        if (routing.styleTag == HidNpadStyleTag_NpadHandheld && primaryNpadId != HidNpadIdType_Handheld) {
            // Port 0 can legitimately map to handheld motors when Joy-Cons are attached.
            // For non-P1 ports, prefer the player's own npad to avoid routing detached
            // multiplayer rumble to Handheld and starving P2+ vibration.
            if (portIndex == 0) {
                npadCandidates[0] = HidNpadIdType_Handheld;
                npadCandidates[1] = primaryNpadId;
                npadCandidateCount = 2;
            } else {
                npadCandidates[0] = primaryNpadId;
                npadCandidateCount = 1;
            }
        }

        for (size_t npadAttempt = 0; npadAttempt < npadCandidateCount; npadAttempt++) {
            const HidNpadIdType npadId = npadCandidates[npadAttempt];
            const uint32_t targetStyleSet = (npadId == HidNpadIdType_Handheld) ? handheldStyleSet : primaryStyleSet;

            if ((targetStyleSet & static_cast<uint32_t>(routing.styleTag)) == 0) {
                SPDLOG_INFO(
                    "Switch rumble skip style: port={} npadId={} style={} targetStyleSet=0x{:X}",
                    static_cast<uint32_t>(portIndex), static_cast<uint32_t>(npadId),
                    static_cast<uint32_t>(routing.styleTag), targetStyleSet);
                continue;
            }

            Result rc = hidInitializeVibrationDevices(handles, routing.handleCount, npadId, routing.styleTag);
            if (R_FAILED(rc)) {
                SPDLOG_WARN(
                    "Switch rumble init failed: port={} npadId={} style={} handles={} rc=0x{:08X}",
                    static_cast<uint32_t>(portIndex), static_cast<uint32_t>(npadId),
                    static_cast<uint32_t>(routing.styleTag), static_cast<uint32_t>(routing.handleCount),
                    static_cast<uint32_t>(rc));
                continue;
            }

            HidVibrationValue values[2];
            for (int32_t j = 0; j < routing.handleCount; j++) {
                HidVibrationValue& value = values[j];
                value.amp_low = lowAmplitude;
                value.freq_low = kSwitchRumbleLowFrequencyHz;
                value.amp_high = highAmplitude;
                value.freq_high = kSwitchRumbleHighFrequencyHz;
            }

            rc = hidSendVibrationValues(handles, values, routing.handleCount);
            if (R_FAILED(rc)) {
                SPDLOG_WARN(
                    "Switch rumble send failed: port={} npadId={} style={} handles={} rc=0x{:08X} lowAmp={:.3f} highAmp={:.3f}",
                    static_cast<uint32_t>(portIndex), static_cast<uint32_t>(npadId),
                    static_cast<uint32_t>(routing.styleTag), static_cast<uint32_t>(routing.handleCount),
                    static_cast<uint32_t>(rc), lowAmplitude, highAmplitude);
                continue;
            }

            SPDLOG_INFO(
                "Switch rumble sent: port={} npadId={} style={} handles={} lowAmp={:.3f} highAmp={:.3f}",
                static_cast<uint32_t>(portIndex), static_cast<uint32_t>(npadId),
                static_cast<uint32_t>(routing.styleTag), static_cast<uint32_t>(routing.handleCount), lowAmplitude,
                highAmplitude);

            return true;
        }
    }

    SPDLOG_WARN("Switch rumble fallback to SDL: port={} name='{}'", static_cast<uint32_t>(portIndex),
                (controllerName != nullptr) ? controllerName : "(unknown)");

    return false;
}

bool StartSwitchGamepadRumble(SDL_GameController* gamepad, uint8_t portIndex, uint16_t lowFrequency, uint16_t highFrequency) {
    const float lowAmplitude = static_cast<float>(lowFrequency) / static_cast<float>(UINT16_MAX);
    const float highAmplitude = static_cast<float>(highFrequency) / static_cast<float>(UINT16_MAX);
    return SendSwitchHidVibration(gamepad, portIndex, lowAmplitude, highAmplitude);
}

bool StopSwitchGamepadRumble(SDL_GameController* gamepad, uint8_t portIndex) {
    return SendSwitchHidVibration(gamepad, portIndex, 0.0f, 0.0f);
}

template <typename RumbleFunc>
void ApplySwitchRumbleForPort(uint8_t portIndex, RumbleFunc&& applyRumble) {
    auto* manager = Ship::Context::GetInstance()->GetControlDeck()->GetConnectedPhysicalDeviceManager().get();
    const auto& gamepads = manager->GetConnectedSDLGamepadsForPort(portIndex);

    SPDLOG_INFO("Switch rumble apply: port={} mappedGamepads={}", static_cast<uint32_t>(portIndex), gamepads.size());

    for (const auto& [instanceId, gamepad] : gamepads) {
        const char* controllerName = SDL_GameControllerName(gamepad);
        const int32_t playerIndex = SDL_GameControllerGetPlayerIndex(gamepad);
        SPDLOG_INFO("Switch rumble gamepad: port={} instanceId={} name='{}' playerIndex={}",
                    static_cast<uint32_t>(portIndex), static_cast<int32_t>(instanceId),
                    (controllerName != nullptr) ? controllerName : "(unknown)", playerIndex);
        applyRumble(gamepad);
    }
}
#endif
} // namespace

namespace Ship {
SDLRumbleMapping::SDLRumbleMapping(uint8_t portIndex, uint8_t lowFrequencyIntensityPercentage,
                                   uint8_t highFrequencyIntensityPercentage)
    : ControllerRumbleMapping(PhysicalDeviceType::SDLGamepad, portIndex, lowFrequencyIntensityPercentage,
                              highFrequencyIntensityPercentage) {
    SetLowFrequencyIntensity(lowFrequencyIntensityPercentage);
    SetHighFrequencyIntensity(highFrequencyIntensityPercentage);
}

void SDLRumbleMapping::StartRumble() {
#if defined(__SWITCH__)
    ApplySwitchRumbleForPort(mPortIndex, [this](SDL_GameController* gamepad) {
        if (!StartSwitchGamepadRumble(gamepad, mPortIndex, mLowFrequencyIntensity, mHighFrequencyIntensity)) {
            StartGamepadRumble(gamepad, mLowFrequencyIntensity, mHighFrequencyIntensity);
        }
    });
#else
    for (const auto& [instanceId, gamepad] :
         Context::GetInstance()->GetControlDeck()->GetConnectedPhysicalDeviceManager()->GetConnectedSDLGamepadsForPort(
             mPortIndex)) {
        StartGamepadRumble(gamepad, mLowFrequencyIntensity, mHighFrequencyIntensity);
    }
#endif
}

void SDLRumbleMapping::StopRumble() {
#if defined(__SWITCH__)
    ApplySwitchRumbleForPort(mPortIndex, [this](SDL_GameController* gamepad) {
        if (!StopSwitchGamepadRumble(gamepad, mPortIndex)) {
            StopGamepadRumble(gamepad);
        }
    });
#else
    for (const auto& [instanceId, gamepad] :
         Context::GetInstance()->GetControlDeck()->GetConnectedPhysicalDeviceManager()->GetConnectedSDLGamepadsForPort(
             mPortIndex)) {
        StopGamepadRumble(gamepad);
    }
#endif
}

void SDLRumbleMapping::SetLowFrequencyIntensity(uint8_t intensityPercentage) {
    mLowFrequencyIntensityPercentage = intensityPercentage;
    mLowFrequencyIntensity = UINT16_MAX * (intensityPercentage / 100.0f);
}

void SDLRumbleMapping::SetHighFrequencyIntensity(uint8_t intensityPercentage) {
    mHighFrequencyIntensityPercentage = intensityPercentage;
    mHighFrequencyIntensity = UINT16_MAX * (intensityPercentage / 100.0f);
}

std::string SDLRumbleMapping::GetRumbleMappingId() {
    return StringHelper::Sprintf("P%d", mPortIndex);
}

void SDLRumbleMapping::SaveToConfig() {
    const std::string mappingCvarKey = CVAR_PREFIX_CONTROLLERS ".RumbleMappings." + GetRumbleMappingId();
    Ship::Context::GetInstance()->GetConsoleVariables()->SetString(
        StringHelper::Sprintf("%s.RumbleMappingClass", mappingCvarKey.c_str()).c_str(), "SDLRumbleMapping");
    Ship::Context::GetInstance()->GetConsoleVariables()->SetInteger(
        StringHelper::Sprintf("%s.LowFrequencyIntensity", mappingCvarKey.c_str()).c_str(),
        mLowFrequencyIntensityPercentage);
    Ship::Context::GetInstance()->GetConsoleVariables()->SetInteger(
        StringHelper::Sprintf("%s.HighFrequencyIntensity", mappingCvarKey.c_str()).c_str(),
        mHighFrequencyIntensityPercentage);
    Ship::Context::GetInstance()->GetConsoleVariables()->Save();
}

void SDLRumbleMapping::EraseFromConfig() {
    const std::string mappingCvarKey = CVAR_PREFIX_CONTROLLERS ".RumbleMappings." + GetRumbleMappingId();

    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.RumbleMappingClass", mappingCvarKey.c_str()).c_str());
    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.LowFrequencyIntensity", mappingCvarKey.c_str()).c_str());
    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.HighFrequencyIntensity", mappingCvarKey.c_str()).c_str());

    Ship::Context::GetInstance()->GetConsoleVariables()->Save();
}

std::string SDLRumbleMapping::GetPhysicalDeviceName() {
    return "SDL Gamepad";
}
} // namespace Ship
