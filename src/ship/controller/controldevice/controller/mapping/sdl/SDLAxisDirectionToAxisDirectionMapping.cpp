#include "ship/controller/controldevice/controller/mapping/sdl/SDLAxisDirectionToAxisDirectionMapping.h"
#include <spdlog/spdlog.h>
#include "ship/utils/StringHelper.h"
#include "ship/window/gui/IconsFontAwesome4.h"
#include "ship/config/ConsoleVariable.h"
#include "ship/Context.h"
#include "ship/controller/controldeck/ControlDeck.h"

#define MAX_SDL_RANGE (float)INT16_MAX

namespace {
#if defined(__SWITCH__)
constexpr uint16_t kNintendoVendorId = 0x057E;
constexpr uint16_t kJoyConLeftProductId = 0x2006;
constexpr uint16_t kJoyConRightProductId = 0x2007;

bool IsSingleJoyCon(SDL_GameController* gamepad) {
    const auto type = SDL_GameControllerGetType(gamepad);
    if (type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
        type == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT) {
        return true;
    }

    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(gamepad);
    if (joystick != nullptr) {
        const uint16_t vendor = SDL_JoystickGetVendor(joystick);
        const uint16_t product = SDL_JoystickGetProduct(joystick);
        if (vendor == kNintendoVendorId && (product == kJoyConLeftProductId || product == kJoyConRightProductId)) {
            return true;
        }
    }

    const char* name = SDL_GameControllerName(gamepad);
    return name != nullptr && SDL_strstr(name, "Joy-Con") != nullptr;
}

int16_t SynthesizeAxisFromDpad(SDL_GameController* gamepad, int32_t axis) {
    const bool up = SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
    const bool down = SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
    const bool left = SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0;
    const bool right = SDL_GameControllerGetButton(gamepad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;

    switch (axis) {
        case SDL_CONTROLLER_AXIS_LEFTX:
            return static_cast<int16_t>((right ? INT16_MAX : 0) - (left ? INT16_MAX : 0));
        case SDL_CONTROLLER_AXIS_LEFTY:
            return static_cast<int16_t>((down ? INT16_MAX : 0) - (up ? INT16_MAX : 0));
        default:
            return 0;
    }
}
#endif
} // namespace

namespace Ship {
SDLAxisDirectionToAxisDirectionMapping::SDLAxisDirectionToAxisDirectionMapping(uint8_t portIndex, StickIndex stickIndex,
                                                                               Direction direction,
                                                                               int32_t sdlControllerAxis,
                                                                               int32_t axisDirection)
    : ControllerInputMapping(PhysicalDeviceType::SDLGamepad),
      ControllerAxisDirectionMapping(PhysicalDeviceType::SDLGamepad, portIndex, stickIndex, direction),
      SDLAxisDirectionToAnyMapping(sdlControllerAxis, axisDirection) {
}

float SDLAxisDirectionToAxisDirectionMapping::GetNormalizedAxisDirectionValue() {
    if (Context::GetInstance()->GetControlDeck()->GamepadGameInputBlocked()) {
        return 0.0f;
    }

    // todo: i don't like making a vector here, not sure what a better solution is
    std::vector<float> normalizedValues = {};
    for (const auto& [instanceId, gamepad] :
         Context::GetInstance()->GetControlDeck()->GetConnectedPhysicalDeviceManager()->GetConnectedSDLGamepadsForPort(
             mPortIndex)) {
        int16_t axisValue = SDL_GameControllerGetAxis(gamepad, mControllerAxis);

#if defined(__SWITCH__)
        // On devkitPro SDL's Switch backend, detached single Joy-Cons expose
        // stick direction as dpad buttons and may leave stick axes at 0.
        // Synthesize left-stick axis from dpad only for those Joy-Con types.
        if (axisValue == 0 && IsSingleJoyCon(gamepad)) {
            axisValue = SynthesizeAxisFromDpad(gamepad, mControllerAxis);
        }
#endif

        if ((mAxisDirection == POSITIVE && axisValue < 0) || (mAxisDirection == NEGATIVE && axisValue > 0)) {
            normalizedValues.push_back(0.0f);
            continue;
        }

        // scale {-32768 ... +32767} to {-MAX_AXIS_RANGE ... +MAX_AXIS_RANGE}
        // and use the absolute value of it
        normalizedValues.push_back(fabs(axisValue * MAX_AXIS_RANGE / MAX_SDL_RANGE));
    }

    if (normalizedValues.size() == 0) {
        return 0.0f;
    }

    return *std::max_element(normalizedValues.begin(), normalizedValues.end());
}

std::string SDLAxisDirectionToAxisDirectionMapping::GetAxisDirectionMappingId() {
    return StringHelper::Sprintf("P%d-S%d-D%d-SDLA%d-AD%s", mPortIndex, mStickIndex, mDirection, mControllerAxis,
                                 mAxisDirection == 1 ? "P" : "N");
}

void SDLAxisDirectionToAxisDirectionMapping::SaveToConfig() {
    const std::string mappingCvarKey = CVAR_PREFIX_CONTROLLERS ".AxisDirectionMappings." + GetAxisDirectionMappingId();
    Ship::Context::GetInstance()->GetConsoleVariables()->SetString(
        StringHelper::Sprintf("%s.AxisDirectionMappingClass", mappingCvarKey.c_str()).c_str(),
        "SDLAxisDirectionToAxisDirectionMapping");
    Ship::Context::GetInstance()->GetConsoleVariables()->SetInteger(
        StringHelper::Sprintf("%s.Stick", mappingCvarKey.c_str()).c_str(), mStickIndex);
    Ship::Context::GetInstance()->GetConsoleVariables()->SetInteger(
        StringHelper::Sprintf("%s.Direction", mappingCvarKey.c_str()).c_str(), mDirection);
    Ship::Context::GetInstance()->GetConsoleVariables()->SetInteger(
        StringHelper::Sprintf("%s.SDLControllerAxis", mappingCvarKey.c_str()).c_str(), mControllerAxis);
    Ship::Context::GetInstance()->GetConsoleVariables()->SetInteger(
        StringHelper::Sprintf("%s.AxisDirection", mappingCvarKey.c_str()).c_str(), mAxisDirection);
    Ship::Context::GetInstance()->GetConsoleVariables()->Save();
}

void SDLAxisDirectionToAxisDirectionMapping::EraseFromConfig() {
    const std::string mappingCvarKey = CVAR_PREFIX_CONTROLLERS ".AxisDirectionMappings." + GetAxisDirectionMappingId();
    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.Stick", mappingCvarKey.c_str()).c_str());
    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.Direction", mappingCvarKey.c_str()).c_str());
    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.AxisDirectionMappingClass", mappingCvarKey.c_str()).c_str());
    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.SDLControllerAxis", mappingCvarKey.c_str()).c_str());
    Ship::Context::GetInstance()->GetConsoleVariables()->ClearVariable(
        StringHelper::Sprintf("%s.AxisDirection", mappingCvarKey.c_str()).c_str());
    Ship::Context::GetInstance()->GetConsoleVariables()->Save();
}

int8_t SDLAxisDirectionToAxisDirectionMapping::GetMappingType() {
    return MAPPING_TYPE_GAMEPAD;
}

std::string SDLAxisDirectionToAxisDirectionMapping::GetPhysicalDeviceName() {
    return SDLAxisDirectionToAnyMapping::GetPhysicalDeviceName();
}

std::string SDLAxisDirectionToAxisDirectionMapping::GetPhysicalInputName() {
    return SDLAxisDirectionToAnyMapping::GetPhysicalInputName();
}
} // namespace Ship
