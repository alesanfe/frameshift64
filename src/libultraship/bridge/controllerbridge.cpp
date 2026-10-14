#include "libultraship/bridge/controllerbridge.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/controller/physicaldevice/ConnectedPhysicalDeviceManager.h"
#include "ship/controller/physicaldevice/PhysicalDeviceType.h"
#include "ship/controller/controldevice/controller/Controller.h"
#include "ship/controller/gcadapter/GCAdapter.h"
#include "ship/controller/raphnet/RaphnetPhysicalDeviceManager.h"
#include "libultraship/libultra/controller.h"
#include "ship/Context.h"

extern "C" {

void ControllerBlockGameInput(uint16_t inputBlockId) {
    Ship::Context::GetInstance()->GetControlDeck()->BlockGameInput(static_cast<int32_t>(inputBlockId));
}

void ControllerUnblockGameInput(uint16_t inputBlockId) {
    Ship::Context::GetInstance()->GetControlDeck()->UnblockGameInput(static_cast<int32_t>(inputBlockId));
}

uint8_t ControllerPortHasPhysicalDevice(int32_t portIndex) {
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return 0;
    }
    auto deck = ctx->GetControlDeck();
    if (deck == nullptr || portIndex < 0 || portIndex >= MAXCONTROLLERS) {
        return 0;
    }

    // SDL gamepads routed to this port.
    if (auto mgr = deck->GetConnectedPhysicalDeviceManager()) {
        if (!mgr->GetConnectedSDLGamepadsForPort(static_cast<uint8_t>(portIndex)).empty()) {
            return 1;
        }
    }

    // Native GC adapter: any adapter port routed here with a controller in it.
    if (auto gc = deck->GetGCAdapter()) {
        const uint8_t mask = gc->GetPortRouting(static_cast<uint8_t>(portIndex));
        for (int ap = 0; ap < Ship::gGCAdapterPorts; ap++) {
            if ((mask & (1u << ap)) && gc->GetPortType(static_cast<uint8_t>(ap)) != Ship::GCControllerType::None) {
                return 1;
            }
        }
    }

    // Raphnet adapter channel bound to this port — a port is only claimed
    // after the pad answers GetControllerType as CTL_TYPE_N64.
    if (auto raph = deck->GetRaphnetPhysicalDeviceManager()) {
        if (raph->IsPortClaimed(static_cast<uint8_t>(portIndex))) {
            return 1;
        }
    }

    // Keyboard/mouse mappings — the PC's built-in input (port 1 by default,
    // but report honestly wherever the user mapped them).
    if (auto controller = deck->GetControllerByPort(static_cast<uint8_t>(portIndex))) {
        if (controller->HasMappingsForPhysicalDeviceType(Ship::PhysicalDeviceType::Keyboard) ||
            controller->HasMappingsForPhysicalDeviceType(Ship::PhysicalDeviceType::Mouse)) {
            return 1;
        }
    }

    return 0;
}
}
