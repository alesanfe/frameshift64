# ==============================================================================
# Smash Classic / BattleShip — task facade
#
# Domain-specific targets live in make/*.mk — this file only wires them up.
# Edit the domain file, not the facade. docs/development/make-workflow.md
# documents the lifecycle; docs/development/repository-layout.md the paths.
# ==============================================================================

.DEFAULT_GOAL := help

include make/common.mk        # shell/vars/host detection/presentation/guards/doctor
include make/dependencies.mk  # setup/install/submodules/venv/lobby deps
include make/generation.mk    # codegen/assets/synth-verify
include make/build.mk         # configure/build/run/cleanup/prune
include make/test.mk          # pytest/ctest/e2e/fuzz/coverage/mods-test
include make/netplay.mk       # netplay/lobby/websocket test flows
include make/quality.mk       # lint/module-deps/env/docs/cvar/golden + docs-serve
include make/security.mk      # semgrep/dependency-audit/sbom/release-binary-check
include make/abi.mk           # port_* ABI version/exports/compat gates
include make/decomp.mk        # facade over components/decomp Makefile
include make/reproducibility.mk # clean-worktree / generated-source / repro gates
include make/platforms.mk     # platform-info/emscripten/switch/tsan/config builds
include make/packaging.mk     # package/package-<os>/package-verify
include make/release.mk       # release/release-dry-run/release-notes
include make/workflows.mk     # dev/pre-commit/pre-push/verify-all composites
include make/diagnostics.mk   # status/versions/environment/graph/report
include make/developer.mk     # mod-package/i18n-extract/save-editor/shots-*
include make/makefile.mk      # self-validation of the facade
include make/help.mk          # categorized help (help/help-all/help-*)
include make/platforms/android.mk
include make/platforms/uwp.mk
# -include make/platforms/$(HOST_PLATFORM).mk   # per-host overrides when added

# ------------------------------------------------------------------------------
# Help (target catalog itself lives in make/help.mk → tools/developer/make_help.py)
# ------------------------------------------------------------------------------

help-advanced: ## [Help] Show advanced variables and examples
	@printf "$(BLUE)Advanced examples$(RESET)\n\n"
	@printf "  make PRESET=windows-release build\n"
	@printf "  make ENABLE_COVERAGE=1 build test-suite TEST_SUITE=pull-request\n"
	@printf "  make TEST_REGEX=rollback ctest\n"
	@printf "  make E2E_SCENARIO=soak E2E_SECONDS=300 e2e\n"
	@printf "  make TEST_SUITE=netplay REPEAT=3 test-suite\n"
	@printf "  make PACKAGE_ARGS='-Verbose' package\n\n"
	@printf "CMake feature flags forwarded:\n"
	@printf "  SSB64_VERSION SSB64_TEST_HOOKS SSB64_BUILD_TESTING\n"
	@printf "  SSB64_ENABLE_COVERAGE DISABLE_SCRIPTING NON_PORTABLE\n"
	@printf "  (anything else: CMAKE_ARGS='-DFOO=bar')\n"

info: ## [Diagnostics] Print the active configuration
	@printf "Project:            %s\n" "$(PROJECT_NAME)"
	@printf "Host platform:      %s\n" "$(HOST_PLATFORM)"
	@printf "Preset:             %s\n" "$(PRESET)"
	@printf "Region:             %s\n" "$(REGION)"
	@printf "ROM:                %s\n" "$(ROM_PATH)"
	@printf "Build dir:          %s\n" "$(BUILD_DIR)"
	@printf "Executable:         %s\n" "$(or $(EXE_DIR),<not built yet>)"
	@printf "App target:         %s\n" "$(APP_TARGET)"
	@printf "Coverage:           %s\n" "$(ENABLE_COVERAGE)"
	@printf "Modding:            %s\n" "$(ENABLE_MODDING)"
	@printf "Test hooks:         %s\n" "$(ENABLE_TEST_HOOKS)"
	@printf "CTest harness:      %s\n" "$(ENABLE_TESTING)"

# ------------------------------------------------------------------------------
# Debugging
# ------------------------------------------------------------------------------
print-%: ## [Diagnostics] Print a Make variable: make print-PRESET
	@printf '%s = %s\n' '$*' '$($*)'
