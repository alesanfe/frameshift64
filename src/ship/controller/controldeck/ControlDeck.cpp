#include "ship/controller/controldeck/ControlDeck.h"

#include "ship/Context.h"
#include "ship/controller/controldevice/controller/Controller.h"
#include "ship/controller/controldevice/controller/mapping/raphnet/RaphnetRumbleMapping.h"
#include "ship/controller/raphnet/RaphnetPhysicalDeviceManager.h"
#include "ship/controller/gcadapter/GCAdapter.h"
#include "ship/controller/controldevice/controller/mapping/gcadapter/GCAdapterRumbleMapping.h"
#include "ship/utils/StringHelper.h"
#include "ship/config/ConsoleVariable.h"
#include <imgui.h>
#include "ship/controller/controldevice/controller/mapping/mouse/WheelHandler.h"

namespace Ship {

ControlDeck::ControlDeck(std::vector<CONTROLLERBUTTONS_T> additionalBitmasks,
                         std::shared_ptr<ControllerDefaultMappings> controllerDefaultMappings,
                         std::unordered_map<CONTROLLERBUTTONS_T, std::string> buttonNames) {
    mConnectedPhysicalDeviceManager = std::make_shared<ConnectedPhysicalDeviceManager>();
    mGlobalSDLDeviceSettings = std::make_shared<GlobalSDLDeviceSettings>();
    mControllerDefaultMappings = controllerDefaultMappings == nullptr ? std::make_shared<ControllerDefaultMappings>()
                                                                      : controllerDefaultMappings;
}

ControlDeck::~ControlDeck() {
    SPDLOG_TRACE("destruct control deck");
}

void ControlDeck::PreInitRaphnet() {
    if (mRaphnetPhysicalDeviceManager != nullptr) {
        SPDLOG_WARN("ControlDeck::PreInitRaphnet called twice; ignoring");
        return;
    }

    // Master kill switch — lets users force-disable native mode without
    // unplugging the adapter (it then appears as a plain SDL HID joystick).
    int32_t enabled = Ship::Context::GetInstance()->GetConsoleVariables()->GetInteger(
        CVAR_PREFIX_CONTROLLERS ".Raphnet.Enabled", 1);
    if (enabled == 0) {
        SPDLOG_INFO("ControlDeck::PreInitRaphnet: gControllers.Raphnet.Enabled=0; native adapter "
                    "support disabled by CVAR (adapter will fall back to SDL HID joystick)");
        return;
    }

    mRaphnetPhysicalDeviceManager = std::make_shared<RaphnetPhysicalDeviceManager>();
    if (!mRaphnetPhysicalDeviceManager->Init()) {
        SPDLOG_WARN("Raphnet manager Init failed; native adapter support disabled this session");
        // Keep the manager around so accessors return a non-null but empty
        // instance — simpler than nullptr-checking everywhere.
        return;
    }
    // Tell the SDL device manager to skip every adapter VID we just claimed,
    // BEFORE SDL_Init(SDL_INIT_GAMECONTROLLER) (which is the next thing the
    // caller does). This wins the Windows DirectInput grab race against the
    // raphnet HID joystick surface.
    for (uint16_t vid : mRaphnetPhysicalDeviceManager->GetClaimedVids()) {
        mConnectedPhysicalDeviceManager->IgnoreVendorIdGlobally(vid);
    }
    SPDLOG_INFO("ControlDeck::PreInitRaphnet: {} adapter(s), {} port(s) claimed",
                mRaphnetPhysicalDeviceManager->GetOpenTransports().size(),
                mRaphnetPhysicalDeviceManager->ClaimedPortCount());

    // One-shot diagnostic dump for remote testers. Runs after enumeration
    // but before normal game startup; the game still launches normally
    // afterward so the user can close the window when satisfied.
    int32_t selfTest = Ship::Context::GetInstance()->GetConsoleVariables()->GetInteger(
        CVAR_PREFIX_CONTROLLERS ".Raphnet.SelfTest", 0);
    if (selfTest != 0) {
        SPDLOG_INFO("ControlDeck::PreInitRaphnet: gControllers.Raphnet.SelfTest=1; running diagnostic dump");
        mRaphnetPhysicalDeviceManager->RunSelfTest();
    }
}

void ControlDeck::ShutdownRaphnet() {
    if (mRaphnetPhysicalDeviceManager == nullptr) {
        return;
    }
    SPDLOG_INFO("ControlDeck::ShutdownRaphnet");
    mRaphnetPhysicalDeviceManager->Shutdown();
    mRaphnetPhysicalDeviceManager.reset();
}

void ControlDeck::PreInitGCAdapter() {
    if (mGCAdapter != nullptr) {
        return;
    }
    if (Ship::Context::GetInstance()->GetConsoleVariables()->GetInteger(CVAR_PREFIX_CONTROLLERS ".GCAdapter.Enabled",
                                                                        1) == 0) {
        SPDLOG_INFO("ControlDeck::PreInitGCAdapter: gControllers.GCAdapter.Enabled=0; native GC adapter disabled");
        return;
    }
    auto adapter = std::make_shared<GCAdapter>();
    if (!adapter->Start()) {
        return;
    }
    mGCAdapter = std::move(adapter);

    // On macOS/Windows the adapter can also surface as a plain HID joystick, so
    // hide it from SDL to stop the two backends fighting over the interface --
    // but ONLY once we have actually claimed it. Hiding it unconditionally
    // would make the adapter unusable by *either* backend on a machine where
    // libusb cannot open it (no WinUSB driver, no udev rule, or the OS HID
    // driver refusing to let go), which is strictly worse than leaving it to
    // SDL. Start() has already made one synchronous attempt by this point.
    //
    // Known gap: an adapter plugged in *after* startup is claimed by the reader
    // thread without SDL being told, so on macOS both backends can end up
    // reading it. Startup is the common case and the one that can regress.
    if (mGCAdapter->IsConnected()) {
        mConnectedPhysicalDeviceManager->IgnoreDeviceGlobally(gGCAdapterVid, gGCAdapterPid);
    } else {
        SPDLOG_INFO("ControlDeck::PreInitGCAdapter: no adapter claimed at startup; leaving {:04x}:{:04x} to SDL",
                    gGCAdapterVid, gGCAdapterPid);
    }
}

void ControlDeck::ShutdownGCAdapter() {
    if (mGCAdapter == nullptr) {
        return;
    }
    mGCAdapter->Stop();
    mGCAdapter.reset();
}

std::shared_ptr<GCAdapter> ControlDeck::GetGCAdapter() {
    return mGCAdapter;
}

void ControlDeck::ApplyGCAdapterRouting() {
    if (mGCAdapter == nullptr) {
        return;
    }
    auto cvars = Ship::Context::GetInstance()->GetConsoleVariables();

    // -1 is out of range for a 4-bit mask, so it doubles as "no saved route".
    // The distinction matters: a saved 0 means the user deliberately cleared
    // the port and must not be given the default back.
    std::array<int32_t, gGCAdapterPorts> saved{};
    uint8_t claimed = 0;
    for (uint8_t port = 0; port < gGCAdapterPorts; ++port) {
        saved[port] = cvars->GetInteger(
            StringHelper::Sprintf(CVAR_PREFIX_CONTROLLERS ".Port%d.GCAdapter.AdapterPorts", port + 1).c_str(), -1);
        if (saved[port] >= 0) {
            claimed |= static_cast<uint8_t>(saved[port]) & 0x0F;
        }
    }

    for (uint8_t port = 0; port < gGCAdapterPorts; ++port) {
        uint8_t mask;
        if (saved[port] >= 0) {
            mask = static_cast<uint8_t>(saved[port]) & 0x0F;
        } else {
            // Implicit default: adapter port N feeds game port N, unless some
            // other port has explicitly claimed that adapter port. Yielding is
            // what keeps one controller from driving two players at once.
            const uint8_t bit = static_cast<uint8_t>(1 << port);
            mask = (claimed & bit) ? 0 : bit;
        }
        mGCAdapter->SetPortRouting(port, mask);
    }
}

void ControlDeck::Init(uint8_t* controllerBits) {
    mControllerBits = controllerBits;
    *mControllerBits |= 1 << 0;

    for (auto port : mPorts) {
        if (port->GetConnectedController()->HasConfig()) {
            port->GetConnectedController()->ReloadAllMappingsFromConfig();
        }
    }

#if defined(__SWITCH__)
    // On Switch, seed default SDL mappings for every unconfigured port so
    // auto-assigned pads can drive P2-P4 without manual setup.
    for (size_t i = 0; i < mPorts.size(); ++i) {
        auto controller = mPorts[i]->GetConnectedController();
        if (controller == nullptr || controller->HasConfig()) {
            continue;
        }

        if (i == 0) {
            controller->AddDefaultMappings(PhysicalDeviceType::Keyboard);
            controller->AddDefaultMappings(PhysicalDeviceType::Mouse);
        }

        controller->AddDefaultMappings(PhysicalDeviceType::SDLGamepad);
    }
#else
    // if we don't have a config for controller 1, set default bindings
    if (!mPorts[0]->GetConnectedController()->HasConfig()) {
        mPorts[0]->GetConnectedController()->AddDefaultMappings(PhysicalDeviceType::Keyboard);
        mPorts[0]->GetConnectedController()->AddDefaultMappings(PhysicalDeviceType::Mouse);
        mPorts[0]->GetConnectedController()->AddDefaultMappings(PhysicalDeviceType::SDLGamepad);
    }
#endif

    // Gamepad defaults for ports 2-4 as well: SDL mappings only fire for
    // pads routed to their port, so these stay dormant until a pad is
    // assigned there (second pad connected, or the user toggles routing in
    // the input editor) — at which point the pad just works instead of
    // requiring a manual "Set Defaults" per port. Keyboard/mouse stay
    // port-1-only.
    for (size_t i = 1; i < mPorts.size(); i++) {
        if (!mPorts[i]->GetConnectedController()->HasConfig()) {
            mPorts[i]->GetConnectedController()->AddDefaultMappings(PhysicalDeviceType::SDLGamepad);
        }
    }

    // Native GameCube adapter: restore each game port's adapter routing, seed
    // the default GC mappings once per port (so an existing config gets them
    // without a manual "Set Defaults"), and install the rumble mapping.
    if (mGCAdapter != nullptr) {
        auto cvars = Ship::Context::GetInstance()->GetConsoleVariables();
        for (size_t i = 0; i < mPorts.size() && i < gGCAdapterPorts; ++i) {
            const uint8_t port = static_cast<uint8_t>(i);
            auto controller = mPorts[i]->GetConnectedController();
            if (controller == nullptr) {
                continue;
            }
            const std::string appliedKey =
                StringHelper::Sprintf(CVAR_PREFIX_CONTROLLERS ".Port%d.GCAdapter.DefaultsApplied", port + 1);
            if (cvars->GetInteger(appliedKey.c_str(), 0) == 0) {
                controller->AddDefaultMappings(PhysicalDeviceType::GameCubeAdapter);
                cvars->SetInteger(appliedKey.c_str(), 1);
                cvars->Save();
            }

            if (auto rumble = controller->GetRumble()) {
                rumble->AddRumbleMapping(std::make_shared<GCAdapterRumbleMapping>(
                    port, DEFAULT_LOW_FREQUENCY_RUMBLE_PERCENTAGE, DEFAULT_HIGH_FREQUENCY_RUMBLE_PERCENTAGE,
                    std::weak_ptr<GCAdapter>(mGCAdapter), port));
            }
        }
        ApplyGCAdapterRouting();
    }

    // Install Raphnet rumble mappings on any port the RaphnetPhysicalDeviceManager
    // has claimed. Polling (the input read path) wires up in L7 — this commit
    // only handles rumble wiring, which can land independently because
    // ControllerRumble already supports adding mappings via AddRumbleMapping.
    if (mRaphnetPhysicalDeviceManager != nullptr) {
        for (size_t i = 0; i < mPorts.size(); ++i) {
            const uint8_t portIndex = static_cast<uint8_t>(i);
            auto transport = mRaphnetPhysicalDeviceManager->GetTransportForPort(portIndex);
            if (transport == nullptr) {
                continue;
            }
            const int channel = mRaphnetPhysicalDeviceManager->GetChannelForPort(portIndex);
            if (channel < 0) {
                continue;
            }
            auto controller = mPorts[i]->GetConnectedController();
            if (controller == nullptr) {
                continue;
            }
            auto rumble = controller->GetRumble();
            if (rumble == nullptr) {
                continue;
            }
            auto mapping = std::make_shared<RaphnetRumbleMapping>(
                portIndex, DEFAULT_LOW_FREQUENCY_RUMBLE_PERCENTAGE,
                DEFAULT_HIGH_FREQUENCY_RUMBLE_PERCENTAGE, std::weak_ptr<RaphnetTransport>(transport),
                static_cast<uint8_t>(channel));
            rumble->AddRumbleMapping(mapping);
            // Switch the controller's read path to native raw-SI polling.
            // No-op on bases that don't override; LUS::Controller overrides.
            controller->SetRaphnetBinding(std::weak_ptr<RaphnetTransport>(transport),
                                          static_cast<uint8_t>(channel));
            SPDLOG_INFO("ControlDeck::Init: port {} raphnet binding installed "
                        "(chn={}, rumble + native polling active)",
                        portIndex, channel);
        }
    }
}

bool ControlDeck::ProcessKeyboardEvent(KbEventType eventType, KbScancode scancode) {
    bool result = false;
    for (auto port : mPorts) {
        auto controller = port->GetConnectedController();

        if (controller != nullptr) {
            result = controller->ProcessKeyboardEvent(eventType, scancode) || result;
        }
    }

    return result;
}

bool ControlDeck::ProcessMouseButtonEvent(bool isPressed, MouseBtn button) {
    bool result = false;
    for (auto port : mPorts) {
        auto controller = port->GetConnectedController();

        if (controller != nullptr) {
            result = controller->ProcessMouseButtonEvent(isPressed, button) || result;
        }
    }

    return result;
}

bool ControlDeck::AllGameInputBlocked() {
    return !mGameInputBlockers.empty();
}

bool ControlDeck::GamepadGameInputBlocked() {
    // block controller input when using the controller to navigate imgui menus
    return AllGameInputBlocked() ||
           Context::GetInstance()->GetWindow()->GetGui()->GetMenuOrMenubarVisible() &&
               Ship::Context::GetInstance()->GetConsoleVariables()->GetInteger(CVAR_IMGUI_CONTROLLER_NAV, 0);
}

bool ControlDeck::KeyboardGameInputBlocked() {
    // block keyboard input when typing in imgui
    ImGuiWindow* activeIDWindow = ImGui::GetCurrentContext()->ActiveIdWindow;
    return AllGameInputBlocked() ||
           (activeIDWindow != NULL &&
            activeIDWindow->ID != Context::GetInstance()->GetWindow()->GetGui()->GetMainGameWindowID()) ||
           ImGui::GetTopMostPopupModal() != NULL; // ImGui::GetIO().WantCaptureKeyboard, but ActiveId check altered
}

bool ControlDeck::MouseGameInputBlocked() {
    // block mouse input when user interacting with gui
    ImGuiWindow* window = ImGui::GetCurrentContext()->HoveredWindow;
    if (window == NULL) {
        return true;
    }
    return AllGameInputBlocked() ||
           (window->ID != Context::GetInstance()->GetWindow()->GetGui()->GetMainGameWindowID());
}

std::shared_ptr<Controller> ControlDeck::GetControllerByPort(uint8_t port) {
    return mPorts[port]->GetConnectedController();
}

// Send "motors off" to every connected gamepad. Must be called while Context,
// ControlDeck, and SDL are all still alive — i.e. before sContext.reset() at
// shutdown. Without this, a clean app exit leaves the last in-flight FF effect
// (uploaded with SDL_MAX_RUMBLE_DURATION_MS ≈ 32s on the Linux evdev backend)
// running on the device until the kernel timer expires.
void ControlDeck::StopAllRumble() {
    for (auto& port : mPorts) {
        auto controller = port->GetConnectedController();
        if (controller == nullptr) {
            continue;
        }
        auto rumble = controller->GetRumble();
        if (rumble != nullptr) {
            rumble->StopRumble();
        }
    }
}

void ControlDeck::BlockGameInput(int32_t blockId) {
    mGameInputBlockers[blockId] = true;
}

void ControlDeck::UnblockGameInput(int32_t blockId) {
    mGameInputBlockers.erase(blockId);
}

std::shared_ptr<ConnectedPhysicalDeviceManager> ControlDeck::GetConnectedPhysicalDeviceManager() {
    return mConnectedPhysicalDeviceManager;
}

std::shared_ptr<RaphnetPhysicalDeviceManager> ControlDeck::GetRaphnetPhysicalDeviceManager() {
    return mRaphnetPhysicalDeviceManager;
}

std::shared_ptr<GlobalSDLDeviceSettings> ControlDeck::GetGlobalSDLDeviceSettings() {
    return mGlobalSDLDeviceSettings;
}

std::shared_ptr<ControllerDefaultMappings> ControlDeck::GetControllerDefaultMappings() {
    return mControllerDefaultMappings;
}

const std::unordered_map<CONTROLLERBUTTONS_T, std::string>& ControlDeck::GetAllButtonNames() const {
    return mButtonNames;
}

std::string ControlDeck::GetButtonNameForBitmask(CONTROLLERBUTTONS_T bitmask) {
    // if we don't have a name for this bitmask,
    // return the stringified bitmask
    if (!mButtonNames.contains(bitmask)) {
        return std::to_string(bitmask);
    }

    return mButtonNames[bitmask];
}
} // namespace Ship
