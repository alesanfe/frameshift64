# ==============================================================================
# platforms.mk — homogeneous per-platform facade
#
# Preset reality (CMakePresets.json): <host>-debug / <host>-release,
# jp-release, lint, <host>-asan, linux-ubsan, macos-ubsan, web-wasm.
# Gated/absent toolchains fail with an actionable message, never
# "No rule to make target".
# ==============================================================================

EMSDK_DIR := out/emsdk

.PHONY: platform-info build-host package-host test-host \
        debug build-release relwithdebinfo minsizerel \
        emscripten emscripten-st emscripten-test emscripten-serve \
        guard-web-build \
        test-web test-web-picker test-web-privacy test-web-matrix \
        test-web-gamepad test-web-kb test-web-lobby test-web-lobby-public \
        switch switch-check switch-sdk-install package-switch \
        tsan test-asan test-ubsan test-tsan sanitizer-matrix test-sanitizers

platform-info: ## [Platform] Show detected host platform and tooling
	@printf "Host:        %s\n" "$(HOST_PLATFORM)"
	@printf "Preset:      %s\n" "$(PRESET)"
	@printf "Build dir:   %s\n" "$(BUILD_DIR)"
	@printf "Executable:  %s\n" "$(or $(EXE_DIR),<not built>)"
	@printf "Emscripten:  %s\n" "$(if $(wildcard $(EMSDK_DIR)/emsdk),found,missing → docs/platforms/web.md)"

build-host: ## [Platform] Build for the host platform
	@$(MAKE) build

package-host: ## [Platform] Package for the host platform
	@$(MAKE) package

test-host: ## [Platform] Test for the host platform
	@$(MAKE) test

# Config-flavored builds — map to the real <host>-<config> presets.
debug: ## [Build] Configure+build the <host>-debug preset
	@$(MAKE) PRESET="$(HOST_PLATFORM)-debug" configure dev-build

# Named build-release — `release` belongs to the validated release flow
# in release.mk.
build-release: ## [Build] Configure+build the <host>-release preset
	@$(MAKE) PRESET="$(HOST_PLATFORM)-release" configure dev-build

relwithdebinfo: ## [Build] No dedicated preset — RelWithDebInfo via CMAKE_ARGS
	@$(MAKE) PRESET="$(HOST_PLATFORM)-release" \
	  CMAKE_ARGS="-DCMAKE_BUILD_TYPE=RelWithDebInfo" configure build

minsizerel: ## [Build] No dedicated preset — MinSizeRel via CMAKE_ARGS
	@$(MAKE) PRESET="$(HOST_PLATFORM)-release" \
	  CMAKE_ARGS="-DCMAKE_BUILD_TYPE=MinSizeRel" configure build

build-target: ## [Build] Build one CMake target: make build-target TARGET=ExtractAssets
	@test -n "$(TARGET)" || { \
	  $(call failure,use: make build-target TARGET=<name>); exit 2; }
	@$(CMAKE) --build --preset "$(PRESET)" --target "$(TARGET)" \
	  $(BUILD_PARALLEL)

list-targets: ## [Build] List targets the configured generator offers
	@$(CMAKE) --build --preset "$(PRESET)" --target help 2>/dev/null \
	  || $(CMAKE) --build "$(BUILD_DIR)" --target help

build-clean: ## [Build] Clean-first rebuild of the preset
	@$(CMAKE) --build --preset "$(PRESET)" --clean-first $(BUILD_PARALLEL)

compile-commands: configure ## [Build] Symlink compile_commands.json to the root
	@if [[ -f "$(BUILD_DIR)/compile_commands.json" ]]; then \
	  $(CMAKE) -E create_symlink \
	    "$(BUILD_DIR)/compile_commands.json" \
	    "$(PROJECT_ROOT)/compile_commands.json"; \
	  $(call success,compile_commands.json linked); \
	else \
	  $(call failure,no compile_commands.json in $(BUILD_DIR)); \
	  printf "  configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON\n" >&2; \
	  exit 1; fi

# ------------------------------------------------------------------------------
# Emscripten — web-wasm preset, gated on a local emsdk (docs/platforms/web.md)
# ------------------------------------------------------------------------------
guard-emsdk:
	@if [[ ! -x "$(EMSDK_DIR)/emsdk" && ! -f "$(EMSDK_DIR)/emsdk_env.sh" \
	      && ! -f "$(EMSDK_DIR)/emsdk.bat" ]]; then \
	  $(call failure,emsdk checkout missing at $(EMSDK_DIR)); \
	  printf "  see docs/platforms/web.md — git clone emsdk + emsdk install latest\n" >&2; \
	  exit 1; fi

emscripten: guard-emsdk ## [Platform] Build the WebAssembly port (web-wasm)
	@$(CMAKE) --preset web-wasm
	@$(CMAKE) --build --preset web-wasm $(BUILD_PARALLEL)

emscripten-st: guard-emsdk ## [Platform] Build the single-thread WASM port (web-wasm-st)
	@$(CMAKE) --preset web-wasm-st
	@$(CMAKE) --build --preset web-wasm-st $(BUILD_PARALLEL)

emscripten-test: emscripten ## [Platform] Run tests on the web build
	@$(CTEST) --test-dir "$(OUT_DIR)/build/web-wasm" $(CTEST_VERBOSE)

# serve.py adds the COOP/COEP headers SharedArrayBuffer (pthread build)
# needs plus the shell CSP — a bare http.server cannot run the game.
emscripten-serve: emscripten ## [Platform] Serve the web build on :8931 (PORT= BUILD=web-wasm-st)
	@$(PROJECT_PYTHON) platforms/web/serve.py \
	  "$(OUT_DIR)/build/$(or $(BUILD),web-wasm)" $(or $(PORT),8931)

# ------------------------------------------------------------------------------
# Web browser tests (playwright). The .py drivers self-serve; the .js ones
# need serve.py running on :8931 — the recipes spin it up and kill it.
# playwright must resolve for node (NODE_PATH=<dir>/node_modules if it is
# not global) and for python (the .venv).
# ------------------------------------------------------------------------------
WEB_BUILD_DIR := $(OUT_DIR)/build/web-wasm
WEB_PORT      ?= 8931
WEB_DIR       := platforms/web

guard-web-build:
	@if [[ ! -f "$(WEB_BUILD_DIR)/BattleShip.html" ]]; then \
	  $(call failure,web build missing — run: make emscripten); exit 1; fi

# serve a build dir, run a node playwright script against it, tear down.
# Usage: $(call web-served-test,<script>,<build-dir>)
define web-served-test
	@rv=0; \
	$(PROJECT_PYTHON) $(WEB_DIR)/serve.py "$(2)" "$(WEB_PORT)" >/dev/null 2>&1 & \
	srv=$$!; sleep 2; \
	$(NODE) "$(WEB_DIR)/$(1)" || rv=$$?; \
	kill $$srv 2>/dev/null; wait $$srv 2>/dev/null; exit $$rv
endef

test-web: test-web-picker test-web-privacy test-web-matrix \
          test-web-gamepad test-web-kb ## [Platform] All ROM-free web E2Es (needs playwright)

test-web-picker: guard-web-build ## [Platform] ROM-picker pipeline E2E (fake ROM, no extraction)
	@$(PROJECT_PYTHON) $(WEB_DIR)/pw_rom_picker.py "$(WEB_BUILD_DIR)"

test-web-privacy: guard-web-build ## [Platform] ROM-privacy E2E (no bytes leave the page)
	@$(PROJECT_PYTHON) $(WEB_DIR)/pw_rom_privacy.py "$(WEB_BUILD_DIR)"

test-web-matrix: guard-web-build ## [Platform] Browser-matrix boot smoke (chromium/firefox/webkit)
	@$(PROJECT_PYTHON) $(WEB_DIR)/pw_browser_matrix.py "$(WEB_BUILD_DIR)"

test-web-gamepad: guard-web-build ## [Platform] Simulated-gamepad E2E (consumed pad state)
	$(call web-served-test,pw_gamepad_sim.js,$(WEB_BUILD_DIR))

test-web-kb: guard-web-build ## [Platform] Soft-keyboard viewport E2E
	$(call web-served-test,pw_kb_viewport.js,$(WEB_BUILD_DIR))

# The lobby smokes need a running lobby too — test-web-lobby composes
# serve + node-lobby + the two-tab ws:// flow.
test-web-lobby: guard-web-build ## [Platform] Two-tab lobby smoke over local lobby (:8799)
	@rv=0; \
	( cd "$(LOBBY_DIR)" && $(NODE) src/infrastructure/local/node-lobby.mjs \
	    >/dev/null 2>&1 ) & lobby=$$!; \
	$(PROJECT_PYTHON) $(WEB_DIR)/serve.py "$(WEB_BUILD_DIR)" "$(WEB_PORT)" \
	    >/dev/null 2>&1 & srv=$$!; sleep 3; \
	$(NODE) "$(WEB_DIR)/pw_lobby_smoke.js" || rv=$$?; \
	kill $$srv $$lobby 2>/dev/null; wait 2>/dev/null; exit $$rv

test-web-lobby-public: guard-web-build ## [Platform] Public-lobby match smoke
	$(call web-served-test,pw_lobby_public_smoke.js,$(WEB_BUILD_DIR))

# ------------------------------------------------------------------------------
# Nintendo Switch — gated (SW-08). Always fails informatively today.
# ------------------------------------------------------------------------------
switch-check: ## [Platform] Check the Switch SDK gate (devkitPro + preset)
	@$(PROJECT_PYTHON) tools/platform/check_switch_sdk.py

switch-sdk-install: ## [Platform] Install the devkitPro Switch toolchain (scripts/development)
	@bash scripts/development/install-devkitpro-pacman

# build-switch.sh owns the real flow (devkitPro toolchain file, asset
# extraction flags, BundleSwitch packaging) — no 'switch' preset exists.
switch: switch-check ## [Platform] Build the Switch port (gated SW-08; ARGS=--debug)
	@bash scripts/build/build-switch.sh $(ARGS)

package-switch: switch ## [Package] Package the Switch build (gated SW-08)
	@$(CMAKE) --build "$(OUT_DIR)/build/switch" --target BundleSwitch \
	  $(BUILD_PARALLEL)

# ------------------------------------------------------------------------------
# Sanitizers — real <host>-asan / <host>-ubsan presets; tsan needs Linux
# ------------------------------------------------------------------------------
# TSan: no preset is registered in CMakePresets.json today. The check is a
# gate, not a stub — it verifies the preset exists, then delegates to WSL
# (Windows) or runs directly (Linux). Never a silent skip.
guard-tsan-preset:
	@if ! grep -q '"linux-tsan"' CMakePresets.json; then \
	  $(call failure,no linux-tsan preset in CMakePresets.json); \
	  printf "  register one, then: make tsan (delegates to WSL on Windows)\n" >&2; \
	  exit 1; fi

tsan: guard-tsan-preset ## [Sanitizer] ThreadSanitizer build (Linux/WSL only)
	@$(PROJECT_PYTHON) tools/developer/run_wsl_preset.py \
	  --preset linux-tsan --targets "configure build"

test-asan: ## [Sanitizer] Configure+build+test under AddressSanitizer
	@$(MAKE) PRESET="$(HOST_PLATFORM)-asan" configure build ctest

test-ubsan: ## [Sanitizer] Configure+build+test under UBSan (Clang/GCC)
	@if [[ "$(HOST_PLATFORM)" == "windows" ]]; then \
	  $(call failure,no windows-ubsan preset — MSVC lacks UBSan); \
	  printf "  use WSL: make test-ubsan (linux-ubsan preset exists)\n" >&2; \
	  exit 1; fi
	@$(MAKE) PRESET="$(HOST_PLATFORM)-ubsan" configure build ctest

test-tsan: guard-tsan-preset ## [Sanitizer] Configure+build+test under ThreadSanitizer (Linux)
	@$(PROJECT_PYTHON) tools/developer/run_wsl_preset.py \
	  --preset linux-tsan --targets "configure build ctest" --label tsan

sanitizer-matrix: test-asan test-ubsan ## [Sanitizer] Run every available sanitizer
	@$(call warning,test-tsan runs only on Linux — invoke it explicitly)

test-sanitizers: sanitizer-matrix ## [Sanitizer] Alias for sanitizer-matrix
