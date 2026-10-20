// Shared between port.cpp (composition root) and port_init_phases.cpp
// (one-shot init phases). Not installed; internal to the application module.
//
// Umbrella for the init-implementation includes the two TUs carried as an
// identical ~60-line block — hoisted here so neither file drifts. port.cpp
// keeps only its extra private includes (Console.h, CoreFoundation).
#pragma once

#include <libultraship/libultraship.h>
#include <SDL2/SDL.h>
#include <math.h>  /* _set_FMA3_enable */
#include <libultraship/controller/controldeck/ControlDeck.h>
#include <fast/Fast3dWindow.h>
#include <ship/resource/File.h>
#include <chrono>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <functional>
#include <system_error>
#include <typeinfo>
#include <unordered_set>

#include "persistence/port_save.h"
#include "game/engine/relocation/ResourceType.h"
#include "game/engine/relocation/RelocFileFactory.h"
#include "game/engine/relocation/SyntheticReloc.h"
#include <ship/resource/factory/BlobFactory.h>
#include <ship/resource/archive/Archive.h>
#include <nlohmann/json.hpp>

#include <ship/resource/ResourceType.h>
#include <fast/resource/ResourceType.h>
#include <fast/resource/factory/DisplayListFactory.h>
#include <fast/resource/factory/LightFactory.h>
#include <fast/resource/factory/MatrixFactory.h>
#include <fast/resource/factory/TextureFactory.h>
#include <fast/resource/factory/VertexFactory.h>

#include "app_paths.h"
#include "config/schema.h"
#include "observability/crash_bundle.h"
#include "application/update/integrity/ReleaseSig.h"
#include "game/engine/decomp/audio_bridge.h"
#include "game/engine/decomp/framebuffer_capture.h"
#include "game/gameplay/enhancements.h"
#include "first_run.h"
#include "presentation/UpdatePrompt.h"
#include "presentation/OnlineStatus.h"
#include "presentation/Toasts.h"
#include "presentation/PortMenu.h"
#ifdef PORT_HIRES_ENABLED
#include "rendering/textures/hires/HiResHook.h"
#include "rendering/textures/hires/HiResPack.h"
#endif
#if !defined(__ANDROID__)
#include "port_window_icon.h"
#endif
#ifdef __SWITCH__
#include "platform/switch/SwitchImpl.h"
#include <sys/stat.h>
#endif
#if defined(__ANDROID__)
#include <android/api-level.h>  // android_get_device_api_level (audio-driver gate)
#include <SDL2/SDL_system.h>    // SDL_AndroidGetExternalStoragePath (test env shim)
#endif
#ifndef DISABLE_SCRIPTING
#include "game/modding/HookManager.h"
#include "game/modding/ModCapabilities.h"
#include "game/modding/SymbolResolver.h"
#endif
#include "renderdoc_trigger.h"
#include "port_log.h"
#include "fighter_registry.h"
#include "focus.h"
#include "rendering/shaders/fast3d_shader_manifest.h"

#ifndef DISABLE_SCRIPTING
#include <ship/scripting/ScriptLoader.h>
#include <libultraship/bridge/scriptingbridge.h>
#endif

namespace Ship { class Context; class Console; }

std::shared_ptr<Ship::Context> PortContext();
std::string PortLocateFile(const std::string& basename);
int32_t ResetCommandHandler(std::shared_ptr<Ship::Console> console,
                            std::vector<std::string> args, std::string* output);

// The init phases keep the C linkage they had inside port.cpp's extern "C"
// block — PortInitImpl calls them from that block.
extern "C" {
void port_setenv(const char* name, const char* value);
void PortApplyCliEnvArgs(int argc, char* argv[]);
int  syNetReplayLoadDebugFile(const char* path);
int  PortInitTestHooks(int argc, char* argv[]);
void PortInitFeatureLatches(void);
int  PortInitCoreRuntime(void);
int  PortInitAssetsAndServices(void);
int  PortInitMods(void);
}
