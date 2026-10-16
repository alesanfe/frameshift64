#include "ship/controller/physicaldevice/ConnectedPhysicalDeviceManager.h"
#include <array>
#include <spdlog/spdlog.h>

#if defined(__SWITCH__)
namespace {
constexpr uint16_t kNintendoVendorId = 0x057E;
constexpr uint16_t kJoyConLeftProductId = 0x2006;
constexpr uint16_t kJoyConRightProductId = 0x2007;

enum class JoyConSide {
    None,
    Left,
    Right,
};

JoyConSide GetJoyConSide(SDL_GameController* gamepad) {
    const auto type = SDL_GameControllerGetType(gamepad);
    if (type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT) {
        return JoyConSide::Left;
    }
    if (type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT) {
        return JoyConSide::Right;
    }

    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(gamepad);
    if (joystick != nullptr) {
        const uint16_t vendor = SDL_JoystickGetVendor(joystick);
        const uint16_t product = SDL_JoystickGetProduct(joystick);

        if (vendor == kNintendoVendorId) {
            if (product == kJoyConLeftProductId) {
                return JoyConSide::Left;
            }
            if (product == kJoyConRightProductId) {
                return JoyConSide::Right;
            }
        }
    }

    const char* name = SDL_GameControllerName(gamepad);
    if (name != nullptr) {
        if (SDL_strstr(name, "Joy-Con (L)") != nullptr || SDL_strstr(name, "Joy-Con Left") != nullptr) {
            return JoyConSide::Left;
        }
        if (SDL_strstr(name, "Joy-Con (R)") != nullptr || SDL_strstr(name, "Joy-Con Right") != nullptr) {
            return JoyConSide::Right;
        }
    }

    return JoyConSide::None;
}
} // namespace
#endif

namespace Ship {
ConnectedPhysicalDeviceManager::ConnectedPhysicalDeviceManager() {
}

ConnectedPhysicalDeviceManager::~ConnectedPhysicalDeviceManager() {
}

std::unordered_map<int32_t, SDL_GameController*>
ConnectedPhysicalDeviceManager::GetConnectedSDLGamepadsForPort(uint8_t portIndex) {
    std::unordered_map<int32_t, SDL_GameController*> result;

    for (const auto& [instanceId, gamepad] : mConnectedSDLGamepads) {
        if (!PortIsIgnoringInstanceId(portIndex, instanceId)) {
            result[instanceId] = gamepad;
        }
    }

    return result;
}

std::unordered_map<int32_t, std::string> ConnectedPhysicalDeviceManager::GetConnectedSDLGamepadNames() {
    return mConnectedSDLGamepadNames;
}

std::unordered_set<int32_t> ConnectedPhysicalDeviceManager::GetIgnoredInstanceIdsForPort(uint8_t portIndex) {
    return mIgnoredInstanceIds[portIndex];
}

bool ConnectedPhysicalDeviceManager::PortIsIgnoringInstanceId(uint8_t portIndex, int32_t instanceId) {
    return GetIgnoredInstanceIdsForPort(portIndex).contains(instanceId);
}

void ConnectedPhysicalDeviceManager::IgnoreInstanceIdForPort(uint8_t portIndex, int32_t instanceId) {
    mIgnoredInstanceIds[portIndex].insert(instanceId);
}

void ConnectedPhysicalDeviceManager::UnignoreInstanceIdForPort(uint8_t portIndex, int32_t instanceId) {
    mIgnoredInstanceIds[portIndex].erase(instanceId);
}

void ConnectedPhysicalDeviceManager::IgnoreVendorIdGlobally(uint16_t vid) {
    if (mIgnoredVendorIds.insert(vid).second) {
        SPDLOG_INFO("ConnectedPhysicalDeviceManager: globally ignoring SDL gamepads with VID 0x{:04x} "
                    "(claimed by another input backend, e.g. Raphnet native)", vid);
    }
}

void ConnectedPhysicalDeviceManager::IgnoreDeviceGlobally(uint16_t vid, uint16_t pid) {
    if (mIgnoredDevices.insert((static_cast<uint32_t>(vid) << 16) | pid).second) {
        SPDLOG_INFO("ConnectedPhysicalDeviceManager: globally ignoring SDL gamepads {:04x}:{:04x} "
                    "(claimed by another input backend)",
                    vid, pid);
    }
}

bool ConnectedPhysicalDeviceManager::IsDeviceIgnoredGlobally(uint16_t vid, uint16_t pid) const {
    return mIgnoredDevices.contains((static_cast<uint32_t>(vid) << 16) | pid);
}

void ConnectedPhysicalDeviceManager::UnignoreVendorIdGlobally(uint16_t vid) {
    if (mIgnoredVendorIds.erase(vid) > 0) {
        SPDLOG_INFO("ConnectedPhysicalDeviceManager: no longer ignoring SDL gamepads with VID 0x{:04x}", vid);
    }
}

bool ConnectedPhysicalDeviceManager::IsVendorIdIgnoredGlobally(uint16_t vid) const {
    return mIgnoredVendorIds.contains(vid);
}

void ConnectedPhysicalDeviceManager::HandlePhysicalDeviceConnect(int32_t sdlDeviceIndex) {
    RefreshConnectedSDLGamepads();
}

void ConnectedPhysicalDeviceManager::HandlePhysicalDeviceDisconnect(int32_t sdlJoystickInstanceId) {
    RefreshConnectedSDLGamepads();
}

void ConnectedPhysicalDeviceManager::RefreshConnectedSDLGamepads() {
    // Snapshot the instance ids we already knew about. A refresh fires on
    // EVERY controller add/remove event, and the per-port routing (ignore
    // sets) must survive for pads that were already connected — previously
    // each refresh re-ignored every pad for ports 2-4, silently reverting
    // the user's port assignments whenever any device event arrived.
    auto previouslyConnected = std::move(mConnectedSDLGamepads);
    std::vector<int32_t> newInstanceIds;

    mConnectedSDLGamepads.clear();
    mConnectedSDLGamepadNames.clear();
    mIgnoredInstanceIds.clear();
    static SDL_JoystickGUID sZeroGuid;

    for (int32_t i = 0; i < SDL_NumJoysticks(); i++) {

        SDL_JoystickGUID deviceGUID = SDL_JoystickGetDeviceGUID(i);
        if (SDL_memcmp(&deviceGUID, &sZeroGuid, sizeof(deviceGUID)) == 0) {
            SPDLOG_WARN(
                "Calling SDL JoystickGetDeviceGUID with index ({:d}) returned zero GUID. This is likely due to an "
                "invalid index. Refer to https://wiki.libsdl.org/SDL2/SDL_JoystickGetDeviceGUID for more information.",
                i);
            continue;
        }

        char deviceGuidCStr[33] = "";
        SDL_JoystickGetGUIDString(deviceGUID, deviceGuidCStr, sizeof(deviceGuidCStr));

        if (!SDL_IsGameController(i)) {
            SPDLOG_WARN("SDL Joystick (GUID: {}) not recognized as gamepad."
                        "This is likely due to a missing mapping string in gamecontrollerdb.txt."
                        "Refer to https://github.com/mdqinc/SDL_GameControllerDB for more information.",
                        deviceGuidCStr);
            continue;
        }

        // Globally ignored vendor (e.g. Raphnet adapter already claimed via
        // hidapi by RaphnetPhysicalDeviceManager). Skip BEFORE SDL_GameController-
        // Open so SDL never gets a handle to the device — opening here would
        // compete with our raw-SI commands and on Windows DirectInput tends to
        // grab first, breaking native polling.
        uint16_t devVid = SDL_JoystickGetDeviceVendor(i);
        if (devVid != 0 && IsVendorIdIgnoredGlobally(devVid)) {
            SPDLOG_INFO("ConnectedPhysicalDeviceManager: skipping SDL gamepad index={} VID=0x{:04x} (GUID: {}) "
                        "— globally ignored by another input backend",
                        i, devVid, deviceGuidCStr);
            continue;
        }

        if (devVid != 0 && IsDeviceIgnoredGlobally(devVid, SDL_JoystickGetDeviceProduct(i))) {
            SPDLOG_INFO("ConnectedPhysicalDeviceManager: skipping SDL gamepad index={} {:04x}:{:04x} — claimed by "
                        "another input backend",
                        i, devVid, SDL_JoystickGetDeviceProduct(i));
            continue;
        }

        auto gamepad = SDL_GameControllerOpen(i);
        if (gamepad == nullptr) {
            SPDLOG_ERROR("SDL GameControllerOpen error (GUID: {}): {}", deviceGuidCStr, SDL_GetError());
            continue;
        }

        auto instanceId = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamepad));
        if (instanceId < 0) {
            SPDLOG_ERROR("SDL JoystickInstanceID error (GUID: {}): {}", deviceGuidCStr, SDL_GetError());
            continue;
        }

        std::string gamepadName;
        auto name = SDL_GameControllerName(gamepad);
        if (name == nullptr) {
            gamepadName = deviceGuidCStr;
            SPDLOG_WARN("SDL_GameControllerName returned null. Setting name to GUID \"{}\" instead.", gamepadName);
        } else {
            gamepadName = name;
        }

        mConnectedSDLGamepads[instanceId] = gamepad;
        mConnectedSDLGamepadNames[instanceId] = gamepadName;

        if (!previouslyConnected.contains(instanceId)) {
            newInstanceIds.push_back(instanceId);
        }
    }

    // Default routing for newly connected pads: each goes to the first port
    // that doesn't have a pad yet (1st pad → port 1, 2nd pad → port 2, ...)
    // so local multiplayer works out of the box with any mix of controller
    // types. Insert into every port's ignore set first so the occupancy
    // scan below doesn't see the pad we're currently placing.
    for (int32_t newId : newInstanceIds) {
        for (uint8_t port = 0; port < 4; port++) {
            mIgnoredInstanceIds[port].insert(newId);
        }
    }
    for (int32_t newId : newInstanceIds) {
        uint8_t freePort = UINT8_MAX;
#if defined(__SWITCH__)
        // Joy-Con (L)+(R) halves map to one port: if the sibling half is the
        // sole occupant of a port, join it there. Otherwise honor SDL's
        // player index when that port is free, then fall through to
        // first-free.
        const JoyConSide newSide = GetJoyConSide(mConnectedSDLGamepads[newId]);
        if (newSide != JoyConSide::None) {
            const JoyConSide siblingSide = (newSide == JoyConSide::Left) ? JoyConSide::Right : JoyConSide::Left;
            for (const auto& [instanceId, gamepad] : mConnectedSDLGamepads) {
                if (instanceId == newId || GetJoyConSide(gamepad) != siblingSide) {
                    continue;
                }
                for (uint8_t port = 0; port < 4; port++) {
                    size_t unignored = 0;
                    bool siblingHere = false;
                    for (const auto& [otherId, otherPad] : mConnectedSDLGamepads) {
                        if (!mIgnoredInstanceIds[port].contains(otherId)) {
                            unignored++;
                            siblingHere = (otherId == instanceId);
                        }
                    }
                    if (siblingHere && unignored == 1) {
                        freePort = port;
                        break;
                    }
                }
                if (freePort != UINT8_MAX) {
                    break;
                }
            }
        }
        if (freePort == UINT8_MAX) {
            const int32_t playerIndex = SDL_GameControllerGetPlayerIndex(mConnectedSDLGamepads[newId]);
            if (playerIndex >= 0 && playerIndex < 4) {
                bool occupied = false;
                for (const auto& [instanceId, gamepad] : mConnectedSDLGamepads) {
                    if (!mIgnoredInstanceIds[playerIndex].contains(instanceId)) {
                        occupied = true;
                        break;
                    }
                }
                if (!occupied) {
                    freePort = static_cast<uint8_t>(playerIndex);
                }
            }
        }
#endif
        if (freePort == UINT8_MAX) {
            for (uint8_t port = 0; port < 4; port++) {
                bool occupied = false;
                for (const auto& [instanceId, gamepad] : mConnectedSDLGamepads) {
                    if (!mIgnoredInstanceIds[port].contains(instanceId)) {
                        occupied = true;
                        break;
                    }
                }
                if (!occupied) {
                    freePort = port;
                    break;
                }
            }
        }
        if (freePort == UINT8_MAX) {
            SPDLOG_INFO("ConnectedPhysicalDeviceManager: all 4 ports already have a gamepad; "
                        "leaving '{}' (instanceId {}) unassigned",
                        mConnectedSDLGamepadNames[newId], newId);
            continue;
        }
        mIgnoredInstanceIds[freePort].erase(newId);
        SPDLOG_INFO("ConnectedPhysicalDeviceManager: assigned new gamepad '{}' (instanceId {}) to port {}",
                    mConnectedSDLGamepadNames[newId], newId, freePort + 1);
    }
}
} // namespace Ship
