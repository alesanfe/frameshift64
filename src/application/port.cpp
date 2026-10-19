// On desktop we own `int main` directly. On Android, SDLActivity calls into
// the .so via dlsym("SDL_main"), so we let SDL_main.h's `#define main SDL_main`
// rename the entry point during preprocessing — which is exactly what
// SDL_MAIN_HANDLED would suppress.
#if !defined(__ANDROID__) && !defined(BATTLESHIP_UWP)
#define SDL_MAIN_HANDLED
#endif
#include "port.h"
#include "gameloop.h"
#include "port_init_internal.h"
#include "port_internal.h"

#include <libultraship/libultraship.h>
#include <SDL2/SDL.h>
#include <math.h>  /* _set_FMA3_enable */
#include <libultraship/controller/controldeck/ControlDeck.h>
#include <fast/Fast3dWindow.h>
#include <ship/resource/File.h>
#include <chrono>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <typeinfo>
#include <unordered_set>

#include "persistence/port_save.h"
#include "game/engine/relocation/ResourceType.h"
#include "game/engine/relocation/RelocFileFactory.h"
#include "game/engine/relocation/SyntheticReloc.h"
#include <ship/resource/factory/BlobFactory.h>
#include <ship/resource/archive/Archive.h>
#ifdef SSB64_TEST_HOOKS
#include <nlohmann/json.hpp>
#endif
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

extern "C" void PortRegisterEvents(void);
#if defined(SSB64_TEST_HOOKS) && defined(_MSC_VER) && defined(_DEBUG)
/* Defined in core/observability/heap_check_boot.cpp — point-in-time probe of
 * the shared (ucrtbased) debug heap; no-op unless SSB64_HEAP_CHECK is set. */
extern "C" int portSharedCrtHeapValidate(const char* tag);
#endif



#include <filesystem>
#include <system_error>

#include <ship/debug/Console.h>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

static std::shared_ptr<Ship::Context> sContext;

std::shared_ptr<Ship::Context> PortContext() { return sContext; }



// Port-side replacement for Ship::Context::LocateFileAcrossAppDirs.
//
// LUS's version uses the throwing form of std::filesystem::exists, which
// raises filesystem_error if the underlying GetFileAttributesExW returns
// anything other than an ENOENT-equivalent. On Win10 19042 the
// NON_PORTABLE app-data probe path (SDL_GetPrefPath returns a
// backslash-terminated path, joined with a literal "/" — yielding
// e.g. "C:\\Users\\u\\AppData\\Roaming\\BattleShip\\/f3d.o2r")
// reportedly trips that error and the unwind from the throw fast-fails
// the process via the heap-corruption detector before any user-level
// catch can run (BattleShip issue #58).
//
// Two differences from the LUS version:
//   1. Use the noexcept exists(p, ec) overload — false is the right
//      answer for any failure, and probing a path should never throw.
//   2. Probe the bundle dir via ssb64::RealAppBundlePath(), which
//      returns the actual exe directory on Windows portable-zip
//      distros. Ship::Context::GetAppBundlePath() under NON_PORTABLE
//      returns the literal CMAKE_INSTALL_PREFIX ("BattleShip"), which
//      is wrong for any user that doesn't unzip into a "BattleShip"
//      subdir matching their cwd.
std::string PortLocateFile(const std::string& basename) {
	// Shared probe order lives in ssb64::LocateExistingFile (app_paths.cpp)
	// so every optional-file lookup walks the same directories; this wrapper
	// keeps the historical "./<name>" miss value so callers that open the
	// path get a readable error message.
	std::string found = ssb64::LocateExistingFile(basename);
	return found.empty() ? "./" + basename : found;
}

/* ── Console "reset" command ─────────────────────────────────────────────
 * The ESC-menu Reset button (src/presentation/Menu.cpp) and the Ctrl/Cmd-R shortcut
 * (libultraship Gui.cpp) both Dispatch("reset") at the LUS console, but the
 * command was never registered, so both fell through to "[LUS] Command not
 * found" and did nothing.
 *
 * The handler performs an in-game reset back to the boot scene — the same
 * "return to title" semantics as a console reset — via the scene manager's
 * normal transition path. The mechanics live decomp-side in
 * portSCManagerRequestReset() (components/decomp/src/sc/scmanager.c) because the port
 * layer can't include decomp headers (the C shim stdlib shadows libc++ —
 * see the include-path note in CMakeLists.txt) and mirroring the scene
 * struct layout here would invite exactly the layout-drift bugs
 * docs/debug_ido_bitfield_layout.md warns about. */
extern "C" void portSCManagerRequestReset(void);

int32_t ResetCommandHandler(std::shared_ptr<Ship::Console> /*console*/, std::vector<std::string> /*args*/,
                                   std::string* output) {
	portSCManagerRequestReset();
	if (output) {
		*output = "Resetting to the opening scene...";
	}
	return 0;
}

extern "C" {

static int PortInitImpl(int argc, char* argv[]);

int PortInit(int argc, char* argv[]) {
	// Top-level catch so unhandled C++ exceptions during init land in
	// ssb64.log with their type and what() instead of bubbling up as an
	// opaque MSVC 0xE06D7363 throw that the user sees as "the app just
	// crashed". Issue #58 reported a Win10 19042 crash with no usable
	// signal beyond "ControlDeck OK"; the caught e.what() narrows it.
	try {
		return PortInitImpl(argc, argv);
	} catch (const std::exception& e) {
		port_log("\n*** PortInit: unhandled C++ exception ***\n"
		         "    type: %s\n    what: %s\n",
		         typeid(e).name(), e.what());
		port_log_close();
		return 1;
	} catch (...) {
		port_log("\n*** PortInit: unhandled non-std exception ***\n");
		port_log_close();
		return 1;
	}
}

#ifdef SSB64_TEST_HOOKS
/* Test-hook env shims - only compiled into test builds. Shipped binaries
 * ignore these flags entirely. */
void port_setenv(const char* name, const char* value) {
#ifdef _WIN32
	_putenv_s(name, value);
#else
	setenv(name, value, 1);
#endif
}

/* Translate argv into SSB64_* env vars before any lazy getenv() static
 * initializes, so test rigs can drive hooks without platform env syntax:
 *
 *   --test-mode          -> SSB64_TEST_MODE=1 + SSB64_NO_UPDATE_CHECK=1
 *                           (both only when unset in the real env - CI
 *                           marks the run automated and the updater
 *                           never touches the network).
 *   --ssb64-<name>=<val> -> SSB64_<NAME>=<val>; <name> accepts
 *                           [A-Za-z0-9_] and is uppercased, so the flag
 *                           can never set PATH, HOME, or any non-SSB64
 *                           variable regardless of case.
 *
 * Documented in docs/development/env_vars.md ("Command-line equivalents"). */
void PortApplyCliEnvArgs(int argc, char* argv[]) {
	for (int i = 1; i < argc; i++) {
		const char* a = argv[i];
		if (a == nullptr || a[0] != '-' || a[1] != '-') {
			continue;
		}
		if (std::strcmp(a, "--test-mode") == 0) {
			if (std::getenv("SSB64_TEST_MODE") == nullptr) {
				port_setenv("SSB64_TEST_MODE", "1");
			}
			if (std::getenv("SSB64_NO_UPDATE_CHECK") == nullptr) {
				port_setenv("SSB64_NO_UPDATE_CHECK", "1");
			}
			port_log("SSB64: --test-mode (SSB64_TEST_MODE=1, "
			         "SSB64_NO_UPDATE_CHECK=1)\n");
			continue;
		}
		const char* body = a + 2;
		const char* eq = nullptr;
		size_t nlen = 0;
		if (std::strncmp(body, "ssb64-", 6) == 0) {
			body += 6;
			eq = std::strchr(body, '=');
			nlen = eq != nullptr ? (size_t)(eq - body) : 0;
		} else {
			continue;
		}
		bool valid = eq != nullptr && nlen > 0 && nlen < 80;
		char name[96];
		std::memcpy(name, "SSB64_", 6);
		for (size_t j = 0; valid && j < nlen; j++) {
			const char c = body[j];
			valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			        (c >= '0' && c <= '9') || c == '_';
			name[6 + j] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
		}
		if (!valid) {
			port_log("SSB64: ignoring malformed flag '%s'\n", a);
			continue;
		}
		name[6 + nlen] = '\0';
		port_setenv(name, eq + 1);
		port_log("SSB64: %s=%s (from argv)\n", name, eq + 1);
	}
}
#endif /* SSB64_TEST_HOOKS */

/* Test-hook env shims + parser-fuzz early entry. Returns 0 to continue
 * normal boot; exits the process itself for fuzz/test-mode paths. */

/* DL-range hooks, context instance, logging, config + CVars. */
static int PortInitContext() {

	/* Wire the SSB64-specific DL-range bounds-check and diag classifier
	 * into libultraship's GFX walker (see port/port_dl_ranges.h). Done
	 * before any GFX activity. libultraship has no compile-time symbol
	 * dependency on these — both are optional callbacks. */
	{
		extern void port_dl_ranges_init(void);
		port_dl_ranges_init();
	}

	/* App identity comes from CMake (SSB64_APP_NAME = "BattleShip" for US,
	 * "BattleShip-JP" for JP). The shortName scopes libultraship's
	 * app-data directory (Context::GetAppDirectoryPath →
	 * ~/Library/Application Support/<shortName>, $XDG_DATA_HOME/<shortName>,
	 * %APPDATA%\<shortName>), so US and JP get fully separate
	 * saves/config/logs/BattleShip.o2r and can never read each other's
	 * ROM-derived data. The config filename stays fixed — it already
	 * lives inside the per-app (bifurcated) directory. */
#ifndef SSB64_APP_NAME
#define SSB64_APP_NAME "BattleShip"
#endif
	sContext = Ship::Context::CreateUninitializedInstance(
		SSB64_APP_NAME,
		SSB64_APP_NAME,
		"BattleShip.cfg.json"
	);

	if (!sContext) {
		port_log("SSB64: Failed to create context instance\n");
		return 1;
	}

	port_log("SSB64: Context instance created\n");

	if (!sContext->InitLogging()) { port_log("SSB64: InitLogging failed\n"); return 1; }
	port_log("SSB64: Logging OK\n");

	if (!sContext->InitConfiguration()) { port_log("SSB64: InitConfiguration failed\n"); return 1; }
	if (!sContext->InitConsoleVariables()) { port_log("SSB64: InitConsoleVariables failed\n"); return 1; }
	port_log("SSB64: Config + CVars OK\n");
	/* CVars are readable now - an opted-in crash bundle uploads on a
	 * detached thread (gCrashReports.Enabled, default off). */
	ssb64::diag::MaybeUploadCrashReport();
	return 0;
}

/* Feature latches: schema validate, UWP backend, co-op latch, hi-res
 * packs, pillarbox, platform backend overrides. No failure paths. */

/* Core runtime bring-up: crash handler, console, control deck,
 * bootstrap ResourceManager (f3d.o2r), window + Port menu. */

/* Assets + services: framebuffer pin, file-drop, first-run wizard,
 * game archive registration, shader prewarm, audio, events. */

/* Mods + factories: script loader, resource factories, symbol
 * resolver, fighter registry seed, mods/ mount + TCC compile. */
static int PortInitImpl(int argc, char* argv[]) {
	port_log("SSB64: PortInit entered\n");

	if (const int rc = PortInitTestHooks(argc, argv)) { return rc; }
	if (const int rc = PortInitContext()) { return rc; }
	PortInitFeatureLatches();
	if (const int rc = PortInitCoreRuntime()) { return rc; }
	if (const int rc = PortInitAssetsAndServices()) { return rc; }
	if (const int rc = PortInitMods()) { return rc; }

	port_log("SSB64: init complete\n");
	return 0;
}

void PortShutdown(void) {
#ifndef DISABLE_SCRIPTING
	// Tear down mod hooks first so no replacement function fires during
	// engine shutdown (replacement code might call into ssb64_game state
	// that's about to be torn down).
	ssb64::mods::HookManager::Shutdown();
	ssb64::mods::SymbolResolver::Shutdown();

	// Unload mod scripts now, while the Context's EventSystem is still fully
	// alive, so each mod's ModExit (which calls UNREGISTER_LISTENER) can reach
	// it. Otherwise the only UnloadAll happens inside ~Context via the
	// sContext.reset() below — by which point the EventSystem is being torn
	// down, so EventSystemUnregisterListener -> Context::GetEventSystem
	// dereferences a dead shared_ptr<EventSystem> and crashes on exit whenever
	// a listener-registering mod (e.g. one that REGISTER_LISTENERs in ModInit)
	// is loaded. The later ~Context UnloadAll then finds nothing loaded.
	if (sContext) {
		if (auto scripting = sContext->GetScriptLoader()) {
			scripting->UnloadAll();
		}
	}
#endif

	// Drop audio bridge resource references before Ship::Context goes away.
	// Otherwise their shared_ptrs survive into __cxa_finalize_ranges and
	// Ship::IResource::~IResource() lands on a shut-down spdlog.
	portAudioShutdownAssets();
	// Stop any in-flight controller rumble while Context + ControlDeck +
	// gamepads + SDL are all still alive. SDLRumbleMapping::StopRumble walks
	// back through Context::GetInstance(), so this MUST run before
	// sContext.reset() — destructor re-entry through the Context singleton
	// has its own SIGSEGV trap. Issue #82: on Linux/evdev, the last
	// SDL_GameControllerRumble call uploads an FF effect with
	// SDL_MAX_RUMBLE_DURATION_MS (~32s); without an explicit stop, the
	// kernel runs the effect to completion after the process exits.
	if (sContext) {
		if (auto cd = sContext->GetControlDeck()) {
			// Shut down the raphnet pipeline FIRST. Each transport's Close
			// sends RQ_RNT_SUSPEND_POLLING(0) so the adapter is usable as a
			// plain HID joystick again after process exit (the firmware does
			// NOT auto-resume on hid_close). Order: ShutdownRaphnet → Stop-
			// AllRumble → sContext.reset() — same singleton-reentry caveat
			// applies as the rumble cleanup below.
			cd->ShutdownRaphnet();
			cd->ShutdownGCAdapter();
			cd->StopAllRumble();
		}
	}
#if defined(__ANDROID__)
	// Detach the touch-overlay virtual joystick and reset its statics while
	// SDL is still up. The process survives Activity relaunches on Android,
	// so stale handles here become use-after-free in the next SDL session.
	extern void port_touch_overlay_shutdown(void);
	port_touch_overlay_shutdown();
#endif
	sContext.reset();
	port_log_close();
}

int PortIsRunning(void) {
	return WindowIsRunning() ? 1 : 0;
}

} // extern "C"

int main(int argc, char* argv[]) {
#if defined(_MSC_VER) && defined(_M_X64)
	/* Netplay determinism: the MSVC CRT picks FMA3 or SSE2 implementations of
	 * sinf/cosf/atan2f/... by CPU, and the two round differently. Two peers
	 * on different CPUs would then simulate slightly different matches. Pin
	 * every machine to the same (SSE2) path. */
	_set_FMA3_enable(0);
#endif
	/* Use an absolute path for ssb64.log so it lands in a predictable
	 * place regardless of how the binary was launched (Finder / open /
	 * shell from any cwd). SDL_GetPrefPath returns the OS app-data dir
	 * (~/Library/Application Support/BattleShip/, %APPDATA%\BattleShip\,
	 * $XDG_DATA_HOME/BattleShip/) and creates it on demand — same dir
	 * Ship::Context will later use for the user's saves and o2r. */
	{
		std::string logPath;
		/* SSB64_LOG_PATH: per-instance log, e.g. two local netplay peers. */
		if (const char* override_path = std::getenv("SSB64_LOG_PATH")) {
			logPath = override_path;
		}
#ifdef __SWITCH__
		// On Switch, write ssb64.log to the SD card alongside the .nro
		// so it's accessible without a PC tool. The directory may not exist
		// yet — without it fopen fails and all logging is silently lost.
		else {
			mkdir("sdmc:/switch/BattleShip", 0777);
			logPath = "sdmc:/switch/BattleShip/ssb64.log";
		}
#else
#ifdef _WIN32
		/* Portable Windows build: keep the log next to the game, in logs\,
		 * where players can find it to send with a bug report. */
		else if (char* base = SDL_GetBasePath()) {
			std::string dir = std::string(base) + "logs";
			SDL_free(base);
			CreateDirectoryA(dir.c_str(), NULL);
			logPath = dir + "\\ssb64.log";
		}
#endif
		else if (char* p = SDL_GetPrefPath(NULL, "BattleShip")) {
			logPath = std::string(p) + "ssb64.log";
			SDL_free(p);
		} else {
			logPath = "ssb64.log";  // last-resort cwd fallback
		}
#endif
		/* TD-015: if the previous run crashed, its handler left a
		 * crash.marker beside the log. Bundle it (marker + the old log,
		 * which holds the backtrace) into logs/crashes/ BEFORE
		 * port_log_init truncates ssb64.log. */
		const std::string bundle = ssb64::diag::CollectPendingCrashBundle(logPath);
		ssb64::diag::QueueCrashReport(bundle);
		port_log_init(logPath.c_str());
		if (!bundle.empty()) {
			port_log("SSB64: previous run crashed — bundle at %s\n", bundle.c_str());
		}
		/* Safe mode: a collected crash bundle means the last run died, so
		 * boot without scripted mods or hi-res packs this launch — a bad
		 * mod can't crash-loop the game. SSB64_SAFE_MODE=1 forces the same
		 * without a crash. Nothing here touches persisted CVars. */
		{
			const char* safeEnv = std::getenv("SSB64_SAFE_MODE");
			const bool forced = safeEnv != nullptr && safeEnv[0] != '\0' &&
				!(safeEnv[0] == '0' && safeEnv[1] == '\0');
			/* One-shot request from Support -> "Restart in Safe Mode": the
			 * marker sits next to crash.marker and is consumed here. */
			const size_t smSep = logPath.find_last_of("/\\");
			const std::string smDir = (smSep == std::string::npos) ? "." : logPath.substr(0, smSep);
			const std::string smReq = smDir + "/safemode.request";
			/* error_code overloads — a read-only log dir must not throw
			 * inside the crash-recovery path itself. */
			std::error_code smEc;
			const bool requested = std::filesystem::exists(smReq, smEc) &&
				(std::filesystem::remove(smReq, smEc), true);
			port_diag_set_safe_mode((!bundle.empty() || forced || requested) ? 1 : 0);
			if (port_diag_safe_mode_active()) {
				port_log("SSB64: safe mode active (%s) — mods and hi-res packs skipped this launch\n",
				         !bundle.empty() ? "previous run crashed" :
				         (forced ? "SSB64_SAFE_MODE set" : "user-requested"));
			}
		}

		/* Tell the crash handlers where to drop crash.marker for the NEXT
		 * run: the same directory the log lives in. */
		const size_t sep = logPath.find_last_of("/\\");
		const std::string logDir = (sep == std::string::npos) ? "." : logPath.substr(0, sep);
		port_diag_set_crash_dir(logDir.c_str());
	}
	port_log("SSB64: main entered\n");
#ifdef __SWITCH__
	/* Mount romfs and bring up the platform services (pl/apm/pcv) before any
	 * asset or applet work — without this nothing under src/platform/switch
	 * ever runs. */
	Ship::Switch::Init();
	port_log("SSB64: Switch platform init done\n");
#endif
	/* SSB64_ADDR_JITTER=<n>: shift this process's heap layout (leaked
	 * allocations of varying size) so two local netplay peers don't share
	 * addresses the way two different PCs never do. Test-only —
	 * compiled out unless -DSSB64_TEST_HOOKS=ON. */
#ifdef SSB64_TEST_HOOKS
	if (const char* jitter = std::getenv("SSB64_ADDR_JITTER")) {
		int n = std::atoi(jitter);
		for (int i = 0; i < n; i++) {
			volatile char* leak = (volatile char*)std::malloc((size_t)(i + 1) * 4099 + 24);
			if (leak != nullptr) leak[0] = 1;
		}
#ifdef _WIN32
		for (int i = 0; i < n; i++) {
			VirtualAlloc(NULL, (SIZE_T)(i + 1) * 65536, MEM_RESERVE, PAGE_NOACCESS);
		}
#endif
	}
#endif /* SSB64_TEST_HOOKS */

#ifdef __APPLE__
	/* Disable the macOS press-and-hold accent/diacritic popup for this app.
	 * SDL keeps a Cocoa text-input context alive for the game window, so
	 * holding a movement key (e.g. WASD) makes AppKit pop the accent picker
	 * instead of delivering key-repeat — the held key stops registering as
	 * down. AppKit reads ApplePressAndHoldEnabled from the app's user
	 * defaults when the text-input context is first created, so this must
	 * run before SDL creates the window (i.e. before PortInit). Writing it
	 * per-app (kCFPreferencesCurrentApplication) leaves the global/system
	 * default untouched. Key repeat still works. */
	CFPreferencesSetAppValue(CFSTR("ApplePressAndHoldEnabled"), kCFBooleanFalse,
	                         kCFPreferencesCurrentApplication);
	CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication);
#endif

	port_install_crash_handlers();


	// Initialize RenderDoc trigger BEFORE PortInit so the RenderDoc DLL
	// can hook D3D11 before LUS creates the device.
	portRenderDocInit();
	port_log("SSB64: RenderDoc hook initialized\n");

	if (PortInit(argc, argv) != 0) {
		return 1;
	}

#if defined(__ANDROID__)
	// === Android JNI cache warm-up ===
	//
	// Our cooperative scheduler runs as port_coroutines (aarch64 fibers)
	// stack-switched on the SDL_main thread. ART tracks JNI transition
	// frames per OS thread via a ManagedStack list whose head lives on
	// the native stack — when port_coroutine_swap moves SP to a different
	// fiber, the head pointer dangles and any JNI call from the fiber
	// aborts with "invalid JNI transition frame reference".
	//
	// SDL2's Android backend lazy-initializes a few caches via JNI on
	// first use; if the first use happens from a coroutine, we crash.
	// Force-warm them here on the real thread so subsequent reads from
	// inside coroutines hit the in-process cache without re-entering JNI.

	// 1. SDL_INIT_GAMECONTROLLER → HIDDeviceManager.initialize.
	SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
	if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
		port_log("SSB64: pre-init SDL_INIT_GAMECONTROLLER failed: %s\n",
		         SDL_GetError());
	}

	// Prefer AAudio (low-latency) only where it exists — API 26+. AAudio's
	// libaaudio.so is absent below that, and SDL_AudioInit does NOT fall back
	// once a driver name is pinned (it fails with "Audio target not
	// available"), so forcing it on older devices kills audio outright. Below
	// 26 we leave the hint unset and let SDL auto-select OpenSL ES.
	if (android_get_device_api_level() >= 26) {
		SDL_SetHint(SDL_HINT_AUDIODRIVER, "aaudio");
	}

	// 2. Suppress ImGui's per-frame SDL_GetDisplayUsableBounds JNI path.
	//    ImGui_ImplSDL2_UpdateMonitors runs on the SSB64 GFX coroutine
	//    every frame; it calls SDL_GetDisplayUsableBounds →
	//    ParseDisplayUsableBoundsHint → SDL_GetHint(SDL_HINT_DISPLAY_USABLE_BOUNDS).
	//    On Android SDL_GetHint falls through to SDL_getenv, which
	//    Binder-IPCs into Java's PackageManager.getApplicationInfo —
	//    that fails CheckJNI ("invalid JNI transition frame reference")
	//    when called from inside a port_coroutine fiber.
	//
	//    Setting the hint here populates SDL2's per-program hint store;
	//    SDL_GetHint then returns from the local cache without env
	//    lookup, never re-entering JNI from the coroutine. Query the real
	//    display bounds while we're still on the JVM-attached SDL_main
	//    thread (the hint is unset yet, so this hits the actual display,
	//    not the hint) — a hardcoded 1080p clamp misplaces floating ImGui
	//    windows on ultrawide/high-res devices. Fall back to 1080p only
	//    if the query fails.
	{
		char bounds[64] = "0,0,1920,1080";
		if (SDL_InitSubSystem(SDL_INIT_VIDEO) == 0) {
			SDL_Rect r;
			if (SDL_GetDisplayUsableBounds(0, &r) == 0 && r.w > 0 && r.h > 0) {
				SDL_snprintf(bounds, sizeof(bounds), "%d,%d,%d,%d", r.x, r.y, r.w, r.h);
			}
		}
		SDL_SetHint(SDL_HINT_DISPLAY_USABLE_BOUNDS, bounds);
	}
	// Warm bHasEnvironmentVariables so any other SDL_getenv that we
	// missed is also cached. The SDL_main thread is JVM-attached at
	// this point, so the JNI roundtrip succeeds.
	(void)SDL_getenv("__ssb64_jni_warmup__");
#endif

	/* Deblob validation harness (docs/architecture/deblob.md, the
	 * synth_verify gate):
	 * SSB64_SYNTH_SELFTEST=1 builds every spec'd bundle through the real
	 * archive + factories + synthesis path and exits pass/fail before the
	 * game boots. Each build enforces I5 (relocated-view CRC) and I7/I9
	 * internally; results land in debug_traces/synth_verify_results.json.
	 * ROM-independent: the spec CRCs are the ROM-anchored ground truth.
	 * Boot-hijacking test hook — compiled only with -DSSB64_TEST_HOOKS=ON. */
#ifdef SSB64_TEST_HOOKS
	if (const char *selftest = std::getenv("SSB64_SYNTH_SELFTEST");
	    selftest && selftest[0] == '1') {
		extern int portSyntheticRelocSelfTest();
		int failures = portSyntheticRelocSelfTest();
		PortShutdown();
		port_exit_process(failures == 0 ? 0 : 1);   /* no _exit on wasm */
	}
#endif

	// Wrap post-init (game boot + main loop + shutdown) in a top-level
	// catch so uncaught C++ exceptions get logged as ssb64.log entries
	// with type and what() before the process exits, instead of bubbling
	// up as an opaque MSVC 0xE06D7363 throw.
	try {

	// Initialize the game boot sequence (coroutines, thread init, etc.)
	PortGameInit();

	// Main frame loop — each iteration runs one frame of game logic
	// and rendering through the coroutine system. PortPushFrame posts
	// a VI tick, resumes the game coroutine, and display lists are
	// rendered via DrawAndRunGraphicsCommands inside the coroutine.
	//
	// SSB64_MAX_FRAMES=N — debug aid that forces a clean shutdown
	// after N frames. Goes through the same code path as the user
	// closing the window (Window::Close() sets mIsRunning=false).
	// Compiled out unless -DSSB64_TEST_HOOKS=ON.
#ifdef SSB64_TEST_HOOKS
	int maxFrames = 0;
	if (const char* env = std::getenv("SSB64_MAX_FRAMES")) {
		maxFrames = std::atoi(env);
	}
#endif
	int frame = 0;
	bool firstRunHintShown = false;
	while (WindowIsRunning()) {
		PortPushFrame();
		frame++;
#if defined(SSB64_TEST_HOOKS) && defined(_MSC_VER) && defined(_DEBUG)
		/* Heap-corruption bisect: validate the shared (ucrtbased) debug heap
		 * once per frame when SSB64_HEAP_CHECK is set; heap_probe.log records
		 * frame-N ENTER/DONE so the first DONE-missing line brackets the
		 * corrupting frame. */
		if (std::getenv("SSB64_HEAP_CHECK") != nullptr) {
			char tag[40];
			snprintf(tag, sizeof(tag), "frame-%d", frame);
			portSharedCrtHeapValidate(tag);
		}
#endif

		if (!firstRunHintShown && frame == 60) {
			auto cv = sContext->GetConsoleVariables();
			if (cv && cv->GetInteger("gFirstRunHintShown", 0) == 0) {
				cv->SetInteger("gFirstRunHintShown", 1);
				if (auto gui = sContext->GetWindow()->GetGui()) {
					gui->SaveConsoleVariablesNextFrame();
				}
			}
			firstRunHintShown = true;
		}
#ifdef SSB64_TEST_HOOKS
		if (maxFrames > 0 && frame >= maxFrames) {
			port_log("SSB64: SSB64_MAX_FRAMES=%d reached — triggering clean shutdown\n", maxFrames);
			if (auto ctx = Ship::Context::GetInstance()) {
				if (auto win = ctx->GetWindow()) {
					win->Close();
				}
			}
			break;
		}
#endif
	}

	port_log("SSB64: main loop exited cleanly at frame=%d (WindowIsRunning=%d)\n",
	         frame, WindowIsRunning());

	PortGameShutdown();
	port_log("SSB64: PortGameShutdown returned\n");

#if defined(SSB64_TEST_HOOKS) && defined(_MSC_VER) && defined(_DEBUG)
	portSharedCrtHeapValidate("pre-PortShutdown");
#endif
	PortShutdown();
	portRenderDocShutdown();
#ifdef __SWITCH__
	Ship::Switch::Exit();
#endif
	return 0;

	} catch (const std::exception& e) {
		port_log("\n*** main: unhandled C++ exception ***\n"
		         "    type: %s\n    what: %s\n",
		         typeid(e).name(), e.what());
		port_log_close();
		return 1;
	} catch (...) {
		port_log("\n*** main: unhandled non-std exception ***\n");
		port_log_close();
		return 1;
	}
}
