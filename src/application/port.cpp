// On desktop we own `int main` directly. On Android, SDLActivity calls into
// the .so via dlsym("SDL_main"), so we let SDL_main.h's `#define main SDL_main`
// rename the entry point during preprocessing — which is exactly what
// SDL_MAIN_HANDLED would suppress.
#if !defined(__ANDROID__) && !defined(BATTLESHIP_UWP)
#define SDL_MAIN_HANDLED
#endif
#include "port.h"
#include "gameloop.h"
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
static std::string PortLocateFile(const std::string& basename) {
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
extern "C" int syNetReplayLoadDebugFile(const char* path);

static int32_t ResetCommandHandler(std::shared_ptr<Ship::Console> /*console*/, std::vector<std::string> /*args*/,
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
static void port_setenv(const char* name, const char* value) {
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
static void PortApplyCliEnvArgs(int argc, char* argv[]) {
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

static int PortInitImpl(int argc, char* argv[]) {
	port_log("SSB64: PortInit entered\n");

#ifdef SSB64_TEST_HOOKS
	PortApplyCliEnvArgs(argc, argv);

	/* TD-010 — parser fuzz entry point. `tools/analysis/fuzz/run_parser_fuzz.py`
	 * drives this: SSB64_FUZZ_PARSER=<manifest|replay|json|save> +
	 * SSB64_FUZZ_INPUT=<path> parses the file through the real parser and
	 * exits. Placed before every other init so a fuzz iteration is a bare
	 * parse+exit with no SDL/archive/game boot in between. */
	if (const char* fuzz = std::getenv("SSB64_FUZZ_PARSER")) {
		const char* input = std::getenv("SSB64_FUZZ_INPUT");
		if (input == nullptr) {
			port_log("SSB64 Fuzz: SSB64_FUZZ_INPUT unset\n");
			port_exit_process(2);
		}
		auto read_all = [](const char* path) {
			std::vector<char> data;
			if (FILE* f = std::fopen(path, "rb")) {
				char chunk[1 << 16];
				size_t n;
				while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
					data.insert(data.end(), chunk, chunk + n);
				}
				std::fclose(f);
			}
			return data;
		};
		if (std::strcmp(fuzz, "manifest") == 0) {
			const std::vector<char> data = read_all(input);
			Ship::ArchiveManifest manifest;
			(void)Ship::Archive::ParseManifestBuffer(
			    data.empty() ? nullptr : data.data(), data.size(), &manifest);
			port_log("SSB64 Fuzz: manifest parsed name=\"%s\"\n",
			         manifest.Name.c_str());
		} else if (std::strcmp(fuzz, "replay") == 0) {
			const int ok = syNetReplayLoadDebugFile(input);
			port_log("SSB64 Fuzz: replay load -> %d\n", ok);
		} else if (std::strcmp(fuzz, "json") == 0) {
			/* Raw nlohmann coverage — exercises the JSON parser the config
			 * file and mod manifests share. */
			const std::vector<char> data = read_all(input);
			try {
				auto j = nlohmann::json::parse(data.begin(), data.end());
				port_log("SSB64 Fuzz: json parsed type=%d\n", (int)j.type());
			} catch (const std::exception& e) {
				port_log("SSB64 Fuzz: json rejected (%s)\n", e.what());
			}
		} else if (std::strcmp(fuzz, "save") == 0) {
			/* File-load coverage: point the SRAM override at the fuzz input
			 * and mirror it into the .meta sidecar, so checkSaveFormat, the
			 * sidecar sscanf and the image read path all run on fuzz bytes. */
			port_setenv("SSB64_SAVE_PATH", input);
			{
				std::string meta = input;
				const size_t dot = meta.find_last_of('.');
				const size_t sep = meta.find_last_of("/\\");
				if (dot != std::string::npos && (sep == std::string::npos || dot > sep)) {
					meta.replace(dot, std::string::npos, ".meta");
				} else {
					meta += ".meta";
				}
				const std::vector<char> data = read_all(input);
				if (FILE* f = std::fopen(meta.c_str(), "wb")) {
					if (!data.empty()) {
						(void)std::fwrite(data.data(), 1, data.size(), f);
					}
					std::fclose(f);
				}
			}
			{
				char buf[4096];
				for (uintptr_t off = 0; off < 0x8000; off += sizeof(buf)) {
					(void)port_save_read(off, buf, sizeof(buf));
				}
			}
			port_log("SSB64 Fuzz: save swept 32KiB\n");
		} else {
			port_log("SSB64 Fuzz: unknown parser \"%s\" "
			         "(manifest|replay|json|save)\n", fuzz);
			port_exit_process(2);
		}
		port_exit_process(0);
	}
#endif /* SSB64_TEST_HOOKS */

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

	/* TD-005: typed-schema pass over the loaded CVars — clamp out-of-range
	 * values and warn on `g*` keys nothing declares (typos / stale entries
	 * left by older builds). Runs before any feature latches CVar state. */
	{
		const int clamped = ssb64::config::PortConfigValidate();
		const int unknown = ssb64::config::PortConfigUnknownKeys();
		ssb64::config::PortConfigStampVersion();
		if (clamped || unknown) {
			port_log("SSB64 Config: %d value(s) clamped, %d unknown key(s)\n",
			         clamped, unknown);
		}
	}

#ifdef BATTLESHIP_UWP
	// Xbox is controller-first. Set this before the first ImGui frame so
	// View/Back can open the menu and A/B + D-pad/stick can operate it.
	sContext->GetConsoleVariables()->SetInteger("gControlNav", 1);
	// Use the CoreWindow swap-chain path validated by the Xbox UWP wrapper.
	sContext->GetConfig()->SetInt("Window.Backend.Id",
	                            static_cast<int>(Ship::WindowBackend::FAST3D_DXGI_DX11));
	sContext->GetConfig()->SetString("Window.Backend.Name", "DirectX 11");
#endif

	/* Latch the Classic Co-op menu choice for this launch — the toggle
	 * swaps which CSS overlay Classic mode enters, so it only applies on
	 * the next boot ("(Needs reload)" on the menu widget). */
	port_classic_coop_latch();

#ifdef PORT_HIRES_ENABLED
	// Hi-res texture pack: scan <app-data>/mods/ for GLideN64-named PNGs
	// and register the Fast3D substitution hook. Master enable lives in
	// the gHiResTextures.Enabled CVar (default 1) — toggled from the
	// Assets → Mods menu, takes effect next cache miss. US-only (see
	// CMakeLists.txt — JP builds drop port/hires/ entirely).
	if (port_diag_safe_mode_active()) {
		port_log("SSB64: safe mode — skipping hi-res texture pack scan\n");
	} else {
	ssb64::hires::HiResPack::Get().Init();
	// Warm the decoded-texture cache on background threads (issue #215) so
	// first use of a pack texture doesn't stall the game/render thread on a
	// synchronous PNG decode. Gated by gHiResTextures.Preload (default on
	// for desktop, off on Android).
	ssb64::hires::HiResPack::Get().StartPreload();
	ssb64_hires_register();
	}
#endif

	/* Pillarbox the framebuffer to 4:3 every launch. The game emits 4:3
	 * GBI only — when LUS's viewport runs at the window aspect (LUS
	 * default: gAdvancedResolution.Enabled=0, viewport == content region),
	 * fullscreen on a 16:9 monitor draws into a 16:9 framebuffer that the
	 * game only fills 4:3 of, leaving the side strips as un-cleared
	 * garbage. Force LUS's aspect-correction path on with a 4:3 target so
	 * mCurDimensions and DrawGame()'s sPosX pillarbox the game image
	 * inside black bars.
	 *
	 * Skip the 4:3 force when the widescreen feature is enabled — its
	 * clip-x compression in Interpreter::AdjXForAspectRatio expands the 4:3
	 * GBI to fill the widened FB during battles, and outside battles we
	 * accept the 4:3-stretched menu look as a Phase 1 trade-off. Toggling
	 * the widescreen CVar takes effect on the next launch (the 4:3 force is
	 * latched here); changing it mid-session triggers an mCurDimensions FB
	 * resize that's racy and crashes. Document this on the menu tooltip. */
	if (auto cv = sContext->GetConsoleVariables()) {
		const bool widescreen_on = cv->GetInteger("gEnhancements.Widescreen", 1) != 0;
		cv->SetFloat("gAdvancedResolution.AspectRatioX", 4.0f);
		cv->SetFloat("gAdvancedResolution.AspectRatioY", 3.0f);

#if defined(__ANDROID__)
		/* Android has no useful exclusive-display-mode fullscreen for this
		 * port. Use SDL_WINDOW_FULLSCREEN_DESKTOP so SDL keeps the native
		 * surface size reported by the OS instead of asking for a fixed
		 * fullscreen mode that can collapse ultrawide devices back to a
		 * compatibility aspect on relaunch. */
		cv->SetInteger("gSdlWindowedFullscreen", 1);
#endif

		/* Latch the Low Resolution Mode menu choice at boot. libultraship's
		 * Gui::CalculateGameViewport / Gui::DrawGame read gLowResMode and the
		 * gAdvancedResolution.* cvars every frame and rewrite mCurDimensions,
		 * which forces a Fast3D framebuffer reallocation. Toggling these
		 * mid-session races the ImGui Metal backend: the per-frame
		 * ImGui::Image cmd captures the old MTLTexture pointer at submit,
		 * but the FB resource releases it on dim change before Metal's
		 * encoder retains the texture, so the next setFragmentTexture:
		 * objc_retain hits a freed object and SIGSEGVs (same hazard
		 * documented above for the widescreen toggle). The menu writes the
		 * user's choice to gLowResModePending; we translate it into the
		 * right set of LUS cvars here, once per launch.
		 *
		 *   0  Off — window-resolution framebuffer
		 *   1  N64 stretched 4:3       (gLowResMode=1)
		 *   2  240p window aspect      (gLowResMode=2)
		 *   3  480p window aspect      (gLowResMode=3)
		 *   4..7  N64 pixel-perfect integer scale (PixelPerfectMode path,
		 *        fixed 320x240 framebuffer via VerticalResolutionToggle=1
		 *        + VerticalPixelCount=240, factor selected by sub-key) */
		if (cv->Get("gLowResModePending") == nullptr) {
			cv->SetInteger("gLowResModePending", cv->GetInteger("gLowResMode", 0));
		}
		const int32_t mode = cv->GetInteger("gLowResModePending", 0);

		int32_t low_res_mode = 0;
		bool integer_scale = false;
		int32_t integer_factor = 1;
		bool integer_auto_fit = false;
		switch (mode) {
			case 1: low_res_mode = 1; break;
			case 2: low_res_mode = 2; break;
			case 3: low_res_mode = 3; break;
			case 4: integer_scale = true; integer_auto_fit = true; break;
			case 5: integer_scale = true; integer_factor = 2; break;
			case 6: integer_scale = true; integer_factor = 3; break;
			case 7: integer_scale = true; integer_factor = 4; break;
			default: break;
		}

		cv->SetInteger("gLowResMode", low_res_mode);
		if (integer_scale) {
			/* PixelPerfect path needs Enabled=1 regardless of widescreen so
			 * ApplyResolutionChanges runs; with VerticalResolutionToggle=1
			 * and a 4:3 AspectRatio, mCurDimensions resolves to 320x240, then
			 * DrawGame's pixel-perfect branch draws it at integer factor. */
			cv->SetInteger("gAdvancedResolution.Enabled", 1);
			cv->SetInteger("gAdvancedResolution.VerticalResolutionToggle", 1);
			cv->SetInteger("gAdvancedResolution.VerticalPixelCount", 240);
			cv->SetInteger("gAdvancedResolution.PixelPerfectMode", 1);
			cv->SetInteger("gAdvancedResolution.IntegerScale.FitAutomatically", integer_auto_fit ? 1 : 0);
			cv->SetInteger("gAdvancedResolution.IntegerScale.Factor", integer_factor);
			cv->SetInteger("gAdvancedResolution.IntegerScale.NeverExceedBounds", 1);
		} else {
			/* Restore the widescreen-driven 4:3 pillarbox default when no
			 * integer-scale mode is selected. */
			cv->SetInteger("gAdvancedResolution.Enabled", widescreen_on ? 0 : 1);
			cv->SetInteger("gAdvancedResolution.VerticalResolutionToggle", 0);
			cv->SetInteger("gAdvancedResolution.PixelPerfectMode", 0);
		}

		/* Issue #96 migration. v0.7-beta stored the per-player NRage
		 * analog-remap enable flag at gEnhancements.AnalogRemap.PX —
		 * the same JSON path that PX.Deadzone / PX.Range hang off of.
		 * That made PX both a scalar and a parent, and Config::Save's
		 * unflatten() threw json::type_error.313 the first time the
		 * Deadzone or Range slider moved, truncating the file to
		 * empty before the throw. The enable cvar is now stored at
		 * gEnhancements.AnalogRemap.PX.Enabled (sibling of Deadzone
		 * / Range). Migrate any legacy scalar by copying its value
		 * to the new key and clearing the old key, otherwise the
		 * legacy entry would re-introduce the conflict on next save. */
		for (int p = 0; p < 4; ++p) {
			char legacy[64];
			char modern[64];
			std::snprintf(legacy, sizeof(legacy), "gEnhancements.AnalogRemap.P%d", p + 1);
			std::snprintf(modern, sizeof(modern), "gEnhancements.AnalogRemap.P%d.Enabled", p + 1);
			if (auto var = cv->Get(legacy); var && var->Type == Ship::ConsoleVariableType::Integer) {
				cv->SetInteger(modern, var->Integer);
				cv->ClearVariable(legacy);
			}
		}
	}

#ifdef __APPLE__
	/* Force the Metal backend on macOS.  The OpenGL backend works but
	 * Apple's GLD driver emits a one-shot
	 *   "GLD_TEXTURE_INDEX_2D is unloadable and bound to sampler type
	 *    (Float) - using zero texture because texture unloadable"
	 * the first time Fast3D draws with the TEXEL1 sampler declared but
	 * unbound (combine doesn't reference TEXEL1).  Metal-cpp accepts
	 * the same shader without complaint and avoids the GL deprecation
	 * path on Apple Silicon.  If Metal is somehow unavailable on the
	 * host, libultraship's Config::GetWindowBackend() fallback
	 * downgrades to OpenGL automatically — this write only changes the
	 * preferred default. */
	sContext->GetConfig()->SetInt(
		"Window.Backend.Id",
		static_cast<int>(Ship::WindowBackend::FAST3D_SDL_METAL));
	sContext->GetConfig()->SetString("Window.Backend.Name", "Metal");
#endif

	/* New init order:
	 *   1. CrashHandler / Console / ControlDeck     — resource-agnostic
	 *   2. ResourceManager bootstrapped with f3d.o2r only — the renderer
	 *      backend (Metal / OpenGL / D3D11) compiles shaders out of
	 *      f3d.o2r during InitWindow's first frame setup, so the
	 *      ResourceManager has to exist by then.  f3d.o2r is shipped
	 *      with the binary (always present, no ROM needed).
	 *   3. Window + Port Menu
	 *   4. First-run flow: silent in-process extraction, then ImGui wizard if needed.
	 *      Once BattleShip.o2r is on disk we add it via ArchiveManager.
	 *   5. Audio / GfxDebugger / FileDropMgr / factory registration. */
	if (!sContext->InitCrashHandler()) { port_log("SSB64: InitCrashHandler failed\n"); return 1; }
	if (!sContext->InitConsole()) { port_log("SSB64: InitConsole failed\n"); return 1; }
	port_log("SSB64: CrashHandler + Console OK\n");

	/* Back the ESC-menu Reset button / Ctrl+Cmd-R Dispatch("reset") with a
	 * real command — see the reset block above PortInit for the mechanism. */
	sContext->GetConsole()->AddCommand("reset", { ResetCommandHandler, "Resets the game to the opening scene" });

	// ControlDeck MUST be initialized before Window — the DXGI window proc
	// calls ControllerUnblockGameInput on WM_SETFOCUS during window creation.
	//
	// Scoped so the local shared_ptr drops its ref inside the block: sContext
	// retains a copy via InitControlDeck, so when PortShutdown later calls
	// sContext.reset() it's the sole owner. Otherwise PortInit's locals
	// outlive sContext and their destructors run after spdlog::shutdown,
	// crashing in SPDLOG_DEBUG calls inside libultraship destructors.
	{
		auto controlDeck = std::make_shared<LUS::ControlDeck>();
		if (!sContext->InitControlDeck(controlDeck)) { port_log("SSB64: InitControlDeck failed\n"); return 1; }
	}
	port_log("SSB64: ControlDeck OK\n");

	{
		// Bootstrap ResourceManager with f3d.o2r only. Allow empty paths
		// so a missing f3d.o2r logs but doesn't fatal — the Window init
		// would still partially work for the wizard, which is enough.
		//
		// PortLocateFile replaces Ship::Context::LocateFileAcrossAppDirs
		// for issue #58 — see the helper's comment for why.
		port_log("SSB64: locating f3d.o2r ...\n");
		const std::string f3d = PortLocateFile("f3d.o2r");
		port_log("SSB64: bootstrap archive (shaders) -> %s\n", f3d.c_str());

		std::vector<std::string> bootstrapPaths = {f3d};
		port_log("SSB64: calling InitResourceManager (bootstrap) ...\n");
		if (!sContext->InitResourceManager(bootstrapPaths, {}, 0,
		                                   /*allowEmptyPaths=*/true)) {
			port_log("SSB64: bootstrap InitResourceManager failed\n");
			return 1;
		}
		port_log("SSB64: bootstrap ResourceManager OK\n");
	}

	// See controlDeck note above re: scoping.
	{
		port_log("SSB64: constructing Fast3dWindow ...\n");
		auto window = std::make_shared<Fast::Fast3dWindow>();
		port_log("SSB64: calling InitWindow ...\n");
		if (!sContext->InitWindow(window)) { port_log("SSB64: InitWindow failed\n"); return 1; }
		port_log("SSB64: Window OK\n");

		// Esc Menu screen, Toggle with Esc.
		if (auto gui = window->GetGui()) {
			port_log("SSB64: attaching Port menu ...\n");
			gui->SetMenu(std::make_shared<ssb64::PortMenu>());
			port_log("SSB64: Port menu attached\n");
#if !defined(__ANDROID__) && !defined(BATTLESHIP_UWP)
			// Startup "update available" prompt; Init kicks off the check.
			gui->AddGuiWindow(std::make_shared<ssb64::UpdatePromptWindow>());
			gui->AddGuiWindow(std::make_shared<ssb64::OnlineStatusWindow>());
#endif
			gui->AddGuiWindow(std::make_shared<ssb64::ToastsWindow>());
		}

#if !defined(__ANDROID__) && !defined(BATTLESHIP_UWP)
		// Linux: WMs only show the app icon if SDL_SetWindowIcon is called
		// on the live window. .ico/.icns paths are baked into the .exe /
		// .app on Windows / macOS so this is a no-op there. Android pulls
		// its launcher icon from the APK resources at install time, so
		// the runtime SDL_SetWindowIcon path is skipped entirely there
		// (and port_window_icon.cpp isn't compiled into libmain.so).
		ssb64::SetWindowIcon();
#endif
	}

	// Pin LUS to off-screen rendering so mGameFb is populated during
	// gameplay and the GPU readback at scene transitions captures the
	// prior frame rather than the post-Present swap-chain back buffer
	// (undefined contents under DXGI FLIP_DISCARD on D3D11). Required by
	// the 1P stage-clear frozen-wallpaper capture (issue #57) and the VS
	// match -> results-screen photo wipe (issue #81). Cost is one extra
	// full-screen blit per frame (sub-millisecond on any modern GPU).
	port_capture_set_force_render_to_fb(1);

	// FileDropMgr must come up before the first-run wizard so SDL_DROPFILE
	// events landing on the window during the wizard frame loop can be
	// polled and used to fill the ROM path field.
	if (!sContext->InitFileDropMgr()) { port_log("SSB64: InitFileDropMgr failed\n"); return 1; }
	port_log("SSB64: FileDropMgr OK\n");
	/* Mod/pack drop consumer registered at boot (not on first
	 * Texture Packs menu render) so a .zip/.o2r/.png dropped
	 * before the user ever opens the menu still installs. */
	ssb64::enhancements::RegisterModPackDropHandler();

	/* First-run flow:
	 *   1. Silent extraction: if a ROM sits at app-data / bundle / cwd we
	 *      just extract without bothering the user.
	 *   2. If still missing, drive an ImGui wizard modal in a pre-gameloop
	 *      render loop until the user provides a ROM and extraction
	 *      succeeds — or quits the window. */
	{
#ifdef BATTLESHIP_UWP
		// A UWP app cannot launch the desktop Torch sidecar or browse arbitrary
		// files. Assets are prepared on a PC and copied to E:/BattleShip.
		std::error_code ec;
		const std::string archive = PortLocateFile(SSB64_O2R_NAME);
		if (!std::filesystem::exists(archive, ec)) {
			port_log("SSB64: missing E:/BattleShip/%s; run the packaged US asset tool and copy its output to USB\n",
			         SSB64_O2R_NAME);
			PortShutdown();
			return 1;
		}
#else
		const std::string targetO2r =
			Ship::Context::GetPathRelativeToAppDirectory(SSB64_O2R_NAME);
		// silent=true: any failure during this auto-attempt should land in
		// the wizard's status text, not a native popup that races the
		// ImGui modal.
		ssb64::ExtractAssetsIfNeeded(targetO2r, /*silent=*/true);
		std::error_code ec;
		// noexcept exists / PortLocateFile rather than the throwing LUS
		// form — issue #58.
		if (!std::filesystem::exists(targetO2r, ec) &&
		    !std::filesystem::exists(PortLocateFile(SSB64_O2R_NAME), ec)) {
			if (!ssb64::RunFirstRunWizard(targetO2r)) {
				port_log("SSB64: first-run wizard cancelled — exiting\n");
				// PortShutdown drops audio bridge refs + resets sContext
				// before main returns. Without it, IResource destructors
				// run during static teardown after spdlog has already
				// closed, raising "mutex lock failed: Invalid argument"
				// from the Fast3dWindow destructor's SPDLOG_DEBUG and
				// terminating with SIGABRT.
				PortShutdown();
				return 1;
			}
		}
#endif
	}

	{
		// Add the per-region game archive to the running ResourceManager
		// now that it exists (SSB64_O2R_NAME = BattleShip.o2r on US,
		// BattleShip-JP.o2r on JP — see CMakeLists.txt).
		auto am = sContext->GetResourceManager()->GetArchiveManager();

		// Optional: BattleShip.fromsource.o2r contains relocData resources
		// produced by compiling components/decomp/src/relocData/*.c via the source-
		// compile pipeline (tools/build_reloc_resource.py). Adding it BEFORE
		// the Torch-extracted BattleShip.o2r means the LUS ArchiveManager's
		// FIFO-wins lookup serves source-compiled entries for matching
		// resource paths. The file is gitignored / produced by the
		// BuildBattleShipFromSource CMake target, which is gated on clang
		// availability — when missing the load is silently skipped and
		// runtime behaves exactly as the pre-M2 Torch-only path.
		if (const char *fromsource = std::getenv("SSB64_RELOC_FROMSOURCE");
			fromsource && fromsource[0] == '1') {
			const std::string fs = PortLocateFile("BattleShip.fromsource.o2r");
			if (!fs.empty()) {
				port_log("SSB64: SSB64_RELOC_FROMSOURCE=1 -> adding %s ahead of BattleShip.o2r\n",
				         fs.c_str());
				if (!am->AddArchive(fs)) {
					port_log("SSB64: AddArchive failed for %s (continuing with Torch-extracted reloc data)\n",
					         fs.c_str());
				}
			} else {
				port_log("SSB64: SSB64_RELOC_FROMSOURCE=1 set but BattleShip.fromsource.o2r not found\n");
			}
		}

		const std::string ssb64o2r = PortLocateFile(SSB64_O2R_NAME);
		port_log("SSB64: adding game archive -> %s\n", ssb64o2r.c_str());
		auto baseArchive = am->AddArchive(ssb64o2r);
		if (!baseArchive) {
			port_log("SSB64: AddArchive failed for %s\n", ssb64o2r.c_str());
			return 1;
		}
		portSyntheticRelocSetBaseArchive(baseArchive);
		port_log("SSB64: game archive registered\n");

		/* TD-009: register the engine-agnostic Ed25519 verifier so mod
		 * archives can validate manifest signatures. The callback keeps
		 * crypto out of libultraship; mods without signature fields are
		 * unaffected. */
		Ship::Archive::SetSignatureVerifier(
			[](const std::string& message, const std::string& signature_b64,
			   const std::string& pubkey_b64) -> bool {
				std::string err;
				return ssb64::enhancements::VerifyEd25519MinisigFields(
					message, signature_b64, pubkey_b64, err);
			});
	}

	{
		auto window = std::dynamic_pointer_cast<Fast::Fast3dWindow>(sContext->GetWindow());
		if (window != nullptr) {
			const auto start = std::chrono::steady_clock::now();
			const auto progress = window->PrewarmShaders(ssb64::GetFast3dShaderManifest());
			const double elapsedMs = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - start).count();
			port_log("SSB64: Fast3D warmup complete=%d compiled=%zu cached=%zu failed=%zu skipped=%zu ms=%.2f\n",
			         progress.complete ? 1 : 0, progress.compiled, progress.alreadyCached,
			         progress.failed, progress.skipped, elapsedMs);
		}
	}

	{
		/* SSB64's audio synthesis path produces interleaved s16 stereo PCM at
		 * 32 kHz (sSYAudioFrequency, see src/sys/audio.c).  LUS's default
		 * AudioSettings::SampleRate is 44100 Hz — passing an empty {} settings
		 * struct makes the host audio device expect 44.1 kHz samples while we
		 * feed it 32 kHz, causing pitch-shift / time-stretch / aliasing that
		 * shows up as broadband noise in the output. */
		Ship::AudioSettings audio;
		audio.SampleRate = 32000;
		if (!sContext->InitAudio(audio)) { port_log("SSB64: InitAudio failed\n"); return 1; }
		port_log("SSB64: Audio initialized at %d Hz\n", (int)audio.SampleRate);
	}
	if (!sContext->InitGfxDebugger()) { port_log("SSB64: InitGfxDebugger failed\n"); return 1; }

	if (!sContext->InitEventSystem()) { port_log("SSB64: InitEventSystem failed\n"); return 1; }
	port_log("SSB64: EventSystem initialized\n");

	// Allocate runtime EventIDs for every event declared in src/game/engine/hooks/list/*.
	// Must happen AFTER InitEventSystem (PortRegisterEvents calls
	// EventSystemRegisterEvent, which dereferences Context::GetEventSystem()).
	PortRegisterEvents();
	ssb64::RegisterFocusListener();
	port_log("SSB64: Engine events registered\n");

#ifndef DISABLE_SCRIPTING
	// TCC mod scripting: configure the include paths + library paths under
	// .tcc/ that the engine populates post-build (see CMakeLists.txt). On
	// Windows BattleShip.def is an export-name list that ScriptLoader resolves
	// against the running EXE before TCC relocates mods into memory; on Unix
	// mods resolve symbols dynamically via the host process's exported symbols.
	{
		std::unordered_map<std::string, std::string> defines = {
			{ "PORT", "1" },
			{ "REGION_US", "1" },
			{ "VERSION_US", "1" },
			{ "F3DEX_GBI_2", "1" },
			{ "_LANGUAGE_C", "1" },
			{ "_USE_MATH_DEFINES", "1" },
			{ "NON_MATCHING", "1" },
			{ "NON_EQUIVALENT", "1" },
			{ "AVOID_UB", "1" },
		};
		std::vector<std::string> includePaths = {
			Ship::Context::GetPathRelativeToAppDirectory(".tcc/include"),
			Ship::Context::GetPathRelativeToAppDirectory(".tcc/include/tcc"),
			Ship::Context::GetPathRelativeToAppDirectory(".tcc/include/winapi"),
			Ship::Context::GetPathRelativeToAppDirectory(".tcc/include/sys"),
			Ship::Context::GetPathRelativeToAppDirectory(".tcc/include/sec_api"),
		};
		std::vector<std::string> libraryPaths = {
			Ship::Context::GetPathRelativeToAppDirectory(".tcc/lib"),
		};
#ifdef _WIN32
		std::vector<std::string> libraries = { "BattleShip.def", "tcc1" };
#else
		std::vector<std::string> libraries = { "tcc1" };
#endif
		constexpr int kCodeVersion = 1;
		/* -mms-bitfields makes TCC pack bitfields the way MSVC does
		 * (start a new storage unit on type changes, e.g. between
		 * `ub32 X : 1` and `s32 Y : 2`). The engine is built with MSVC
		 * and several decomp structs (notably FTStruct ~lines
		 * 1507-1551) mix `ub32`/`s32`/`u32` bitfield types — without
		 * this flag, TCC packs them all into a single 32-bit unit
		 * while MSVC splits them across multiple units, shifting every
		 * field afterwards (e.g. `attr`, `joints`) and causing mod
		 * reads to land on adjacent function-pointer fields. */
		if (!sContext->InitScriptLoader(defines, kCodeVersion, "-mms-bitfields",
		                                includePaths, libraryPaths, libraries)) {
			port_log("SSB64: InitScriptLoader failed\n");
			return 1;
		}
		port_log("SSB64: ScriptLoader initialized (codeVersion=%d)\n", kCodeVersion);
	}
#endif

	// InitFileDropMgr already happened earlier — see the wizard plumbing.
	port_log("SSB64: All subsystems initialized\n");

	// Register resource factories
	auto loader = sContext->GetResourceManager()->GetResourceLoader();
	loader->RegisterResourceFactory(
		std::make_shared<ResourceFactoryBinaryRelocFileV0>(),
		RESOURCE_FORMAT_BINARY,
		"SSB64Reloc",
		static_cast<uint32_t>(SSB64::ResourceType::SSB64Reloc),
		0
	);

	/* Fast3D typed-asset factories for deblobbed reloc slices
	 * (docs/deblob.md): the synthesis engine loads these resource types
	 * and re-serializes them into the bundle's big-endian image. */
	loader->RegisterResourceFactory(
		std::make_shared<Fast::ResourceFactoryBinaryDisplayListV0>(),
		RESOURCE_FORMAT_BINARY, "DisplayList",
		static_cast<uint32_t>(Fast::ResourceType::DisplayList), 0);
	loader->RegisterResourceFactory(
		std::make_shared<Fast::ResourceFactoryXMLDisplayListV0>(),
		RESOURCE_FORMAT_XML, "DisplayList",
		static_cast<uint32_t>(Fast::ResourceType::DisplayList), 0);
	loader->RegisterResourceFactory(
		std::make_shared<Fast::ResourceFactoryBinaryLightV0>(),
		RESOURCE_FORMAT_BINARY, "Light",
		static_cast<uint32_t>(Fast::ResourceType::Light), 0);
	loader->RegisterResourceFactory(
		std::make_shared<Fast::ResourceFactoryBinaryMatrixV0>(),
		RESOURCE_FORMAT_BINARY, "Matrix",
		static_cast<uint32_t>(Fast::ResourceType::Matrix), 0);
	loader->RegisterResourceFactory(
		std::make_shared<Fast::ResourceFactoryBinaryTextureV0>(),
		RESOURCE_FORMAT_BINARY, "Texture",
		static_cast<uint32_t>(Fast::ResourceType::Texture), 0);
	loader->RegisterResourceFactory(
		std::make_shared<Fast::ResourceFactoryBinaryTextureV1>(),
		RESOURCE_FORMAT_BINARY, "Texture",
		static_cast<uint32_t>(Fast::ResourceType::Texture), 1);
	loader->RegisterResourceFactory(
		std::make_shared<Fast::ResourceFactoryBinaryVertexV0>(),
		RESOURCE_FORMAT_BINARY, "Vertex",
		static_cast<uint32_t>(Fast::ResourceType::Vertex), 0);
	loader->RegisterResourceFactory(
		std::make_shared<Fast::ResourceFactoryXMLVertexV0>(),
		RESOURCE_FORMAT_XML, "Vertex",
		static_cast<uint32_t>(Fast::ResourceType::Vertex), 0);
	loader->RegisterResourceFactory(
		std::make_shared<Ship::ResourceFactoryBinaryBlobV0>(),
		RESOURCE_FORMAT_BINARY,
		"Blob",
		static_cast<uint32_t>(Ship::ResourceType::Blob),
		0
	);

	port_log("SSB64: Resource factories registered\n");

#ifndef DISABLE_SCRIPTING
	/* Mod runtime: bring up symbol resolution + the hook table so TCC
	 * mods can install hooks during their MOD_INIT below. Hooks land on
	 * engine functions in memory; the game coroutine started later via
	 * PortGameInit() picks them up automatically because the patches
	 * are in the running .text. */
	if (!ssb64::mods::SymbolResolver::Init()) {
		port_log("SSB64: SymbolResolver::Init failed - mods will not load\n");
	}
	if (!ssb64::mods::HookManager::Init()) {
		port_log("SSB64: HookManager::Init failed - mods will not load\n");
	}
#endif

	/* Seed the per-fkind dispatch registry from the vanilla decomp arrays
	 * BEFORE any mod runs. Mods can overwrite vanilla rows or add synth
	 * rows past nFTKindEnumCount via port_fighter_register at MOD_INIT;
	 * the engine reads through registry accessors instead of fixed-size
	 * vanilla arrays, so synth fkinds never OOB those tables. */
	port_fighter_seed_vanilla();
	port_log("SSB64: fighter registry seeded\n");

#ifndef DISABLE_SCRIPTING
	/* Mount mods/ entries (folders, .o2r, .zip) into the LUS
	 * ArchiveManager so ScriptLoader can iterate them. */
	if (port_diag_safe_mode_active()) {
		port_log("SSB64: safe mode — skipping mods/ mount and scripted mods\n");
	} else {
	ssb64::MountModsDir();

	// TCC scripting: compile + load any .o2r / folder mod under mods/ that
	// declares a `main` entry in its manifest.json. Each mod's source files
	// are amalgamated by the ScriptLoader, compiled and relocated into memory
	// via libtcc, and ModInit is called by name. The pre/post-init callbacks
	// tag every HookManager::InstallHook call with the current mod name so
	// hot-reload can selectively uninstall hooks per mod without touching others.
	if (auto scripting = sContext->GetScriptLoader()) {
		try {
			scripting->CompileAll();
			scripting->LoadAll(
				/*preInit=*/[](const std::string& mod) {
					ssb64::mods::HookManager::SetCurrentOwner(mod.c_str());
				},
				/*postInit=*/[](const std::string&) {
					ssb64::mods::HookManager::ClearCurrentOwner();
				});
			port_log("SSB64: TCC scripted mods compiled + loaded\n");
		} catch (const std::exception& e) {
			port_log("SSB64: TCC ScriptLoader threw — continuing without scripted mods: %s\n", e.what());
		}
	}
	}
#endif

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
			const bool requested = std::filesystem::exists(smReq) &&
				(std::filesystem::remove(smReq), true);
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
		_exit(failures == 0 ? 0 : 1);
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
