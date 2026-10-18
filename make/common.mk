# ==============================================================================
# Smash Classic / FrameShift64 — developer command facade
#
# CMake (CMakeLists.txt + CMakePresets.json + cmake/) remains the source of
# truth for compilation. This Makefile only provides short, consistent
# commands over CMake, ctest, pytest (tools/developer/testing/run_tests.py), the
# standalone drivers in tests/, and the platform scripts in scripts/.
#
#   make setup                       doctor + submodules + lobby-install + configure
#   make build                       build the main app (preset windows-debug)
#   make test                        run the quick pytest suite
#   make check                       local quality gate (lint + tests)
#   make e2e E2E_SCENARIO=lan        run a netplay e2e driver
#   make package                     package for the host platform
#
#   make PRESET=windows-release build
#   make REGION=jp PRESET=jp-release build
#   make JOBS=8 build
#   make TEST_SUITE=netplay test-suite
# ==============================================================================
.DEFAULT_GOAL := help
.DELETE_ON_ERROR:
.SUFFIXES:
.ONESHELL:
# GNU make invokes $(SHELL) $(.SHELLFLAGS) literally — "/usr/bin/env bash"
# is a path-with-spaces it cannot exec (Error 127 on Linux CI). Plain "bash"
# resolves through PATH on every runner and on Git Bash for Windows.
SHELL := bash
.SHELLFLAGS := -eu -o pipefail -c

# ------------------------------------------------------------------------------
# User-configurable variables
# ------------------------------------------------------------------------------
PROJECT_NAME       ?= ssb64
APP_TARGET         ?= ssb64
REGION             ?= us
# Executable name tracks SSB64_APP_NAME (BattleShip us / BattleShip-JP jp).
EXECUTABLE_NAME    ?= $(if $(filter jp,$(REGION)),BattleShip-JP,BattleShip)
BUILD_TYPE         ?= debug
JOBS               ?=
VERBOSE            ?= 0
COLOR              ?= 1
OUT_DIR            ?= out
LOG_DIR            ?= $(OUT_DIR)/logs
REPORT_DIR         ?= $(OUT_DIR)/reports
TEST_OUTPUT_DIR    ?= $(OUT_DIR)/test-results
DIST_DIR           ?= dist

CMAKE              ?= cmake
CTEST              ?= ctest
PYTHON             ?= python
GIT                ?= git
NODE               ?= node
NPM                ?= npm

CMAKE_ARGS         ?=
BUILD_ARGS         ?=
TEST_ARGS          ?=
RUN_ARGS           ?=
LINT_ARGS          ?=
PACKAGE_ARGS       ?=

TEST_REGEX         ?=
TEST_LABEL         ?=
TEST_SUITE         ?= quick
E2E_SCENARIO       ?= all
NETPLAY_PLAYERS    ?= 3
E2E_SECONDS        ?= 45
REPEAT             ?= 1
NET_PROFILE        ?=
DETERMINISM_RUNS   ?= 10

ENABLE_COVERAGE    ?= 0
ENABLE_MODDING     ?= 1
ENABLE_TEST_HOOKS  ?= 1
ENABLE_TESTING     ?= 0
NON_PORTABLE       ?= 0

# ROMs live at the repo root, gitignored (see docs).
ROM_US             ?= baserom.us.z64
ROM_JP             ?= baserom.jp.z64

# Real CMake target names (see root CMakeLists.txt).
ASSETS_TARGET          ?= ExtractAssets
ASSET_HEADERS_TARGET   ?= ExtractAssetHeaders
RUNTIME_ASSETS_TARGET  ?= StageRuntimeAssets
O2R_TARGET             ?= GenerateBattleShipO2R
STAGE_ASSETS_TARGET    ?= DeriveStageAssets
SYNTH_VERIFY_TARGET    ?= synth_verify

# ------------------------------------------------------------------------------
# Host / preset detection
# ------------------------------------------------------------------------------
UNAME_S := $(shell uname -s 2>/dev/null || echo unknown)
ifneq (,$(findstring MINGW,$(UNAME_S)))
HOST_PLATFORM := windows
EXE_SUFFIX    := .exe
POWERSHELL    ?= powershell
else ifneq (,$(findstring MSYS,$(UNAME_S)))
HOST_PLATFORM := windows
EXE_SUFFIX    := .exe
POWERSHELL    ?= powershell
else ifneq (,$(findstring CYGWIN,$(UNAME_S)))
HOST_PLATFORM := windows
EXE_SUFFIX    := .exe
POWERSHELL    ?= powershell
else ifeq ($(UNAME_S),Linux)
HOST_PLATFORM := linux
EXE_SUFFIX    :=
POWERSHELL    ?= pwsh
else ifeq ($(UNAME_S),Darwin)
HOST_PLATFORM := macos
EXE_SUFFIX    :=
POWERSHELL    ?= pwsh
else
HOST_PLATFORM := unknown
EXE_SUFFIX    :=
POWERSHELL    ?= pwsh
endif

BUILD_TYPE_LOWER := $(shell printf '%s' "$(BUILD_TYPE)" | tr '[:upper:]' '[:lower:]')
PRESET           ?= $(HOST_PLATFORM)-$(BUILD_TYPE_LOWER)
BUILD_DIR        ?= $(OUT_DIR)/build/$(PRESET)

# Multi-config generators nest the binary under <config>/. EXE_DIR is the
# directory that actually contains the executable (for e2e --build-dir).
EXE_DIR ?= $(firstword $(wildcard $(BUILD_DIR)/$(EXECUTABLE_NAME)$(EXE_SUFFIX) \
              $(BUILD_DIR)/Release/$(EXECUTABLE_NAME)$(EXE_SUFFIX) \
              $(BUILD_DIR)/Debug/$(EXECUTABLE_NAME)$(EXE_SUFFIX) \
              $(BUILD_DIR)/bin/$(EXECUTABLE_NAME)$(EXE_SUFFIX)))

ifeq ($(REGION),us)
ROM_PATH := $(ROM_US)
else ifeq ($(REGION),jp)
ROM_PATH := $(ROM_JP)
else
ROM_PATH := baserom.$(REGION).z64
endif
# Prefer the roms/ drop dir; fall back to repo-root legacy location.
ifeq ($(wildcard $(ROM_PATH)),)
ifneq ($(wildcard roms/$(notdir $(ROM_PATH))),)
ROM_PATH := roms/$(notdir $(ROM_PATH))
endif
endif

# ------------------------------------------------------------------------------
# Real scripts / dirs (verified against the tree — keep in sync)
# ------------------------------------------------------------------------------
LINT_SCRIPT          := tools/analysis/lint/run_project_lint.py
MODULE_DEPS_SCRIPT   := tools/analysis/lint/check_module_deps.py
ENV_VARS_SCRIPT      := tools/analysis/lint/check_env_vars.py
DOCS_CHECK_SCRIPT    := tools/analysis/lint/check_docs.py
RETIRED_PATHS_SCRIPT := tools/analysis/lint/check_retired_paths.py
RELEASE_BIN_SCRIPT   := tools/analysis/lint/check_release_binary.py
CVAR_SCHEMA_SCRIPT   := tools/analysis/lint/check_cvar_schema.py
GEN_HEADERS_SCRIPT   := tools/analysis/lint/check_generated_headers.py
SBOM_SCRIPT          := tools/analysis/lint/gen_sbom.py
TEST_RUNNER          := tools/developer/testing/run_tests.py
COVERAGE_TOOL        := tools/developer/testing/coverage.py
PARSER_FUZZ          := tools/analysis/fuzz/run_parser_fuzz.py
LOBBY_DIR            := services/netplay-lobby
LOBBY_SECURITY       := $(LOBBY_DIR)/tests/security/test-lobby-security.js
LOBBY_FUZZ           := $(LOBBY_DIR)/tests/fuzz/test-lobby-fuzz.js
E2E_DIR              := tests/e2e/netplay
ANDROID_DIR          := platforms/android
UWP_DIR              := platforms/uwp

# Project python — prefer .venv when it exists (reproducible deps)
VENV_DIR             := .venv
ifeq ($(HOST_PLATFORM),windows)
VENV_PYTHON          := $(VENV_DIR)/Scripts/python.exe
else
VENV_PYTHON          := $(VENV_DIR)/bin/python
endif
PROJECT_PYTHON       := $(if $(wildcard $(VENV_PYTHON)),$(VENV_PYTHON),$(PYTHON))

# ------------------------------------------------------------------------------
# Derived flags
# ------------------------------------------------------------------------------
ifneq ($(strip $(JOBS)),)
BUILD_PARALLEL := --parallel $(JOBS)
CTEST_PARALLEL := --parallel $(JOBS)
endif
ifeq ($(VERBOSE),1)
BUILD_VERBOSE := --verbose
CTEST_VERBOSE := --verbose
else
CTEST_VERBOSE := --output-on-failure
endif
ifneq ($(strip $(TEST_REGEX)),)
CTEST_REGEX := --tests-regex "$(TEST_REGEX)"
endif
ifneq ($(strip $(TEST_LABEL)),)
CTEST_LABEL := --label-regex "$(TEST_LABEL)"
endif

# Only real root-CMakeLists options — no invented SMASH_* switches.
# DISABLE_SCRIPTING is the mod-system master switch (inverted semantic).
DISABLE_SCRIPTING := $(if $(filter 1,$(ENABLE_MODDING)),OFF,ON)
CMAKE_FEATURE_ARGS := \
	-DSSB64_VERSION="$(REGION)" \
	-DSSB64_TEST_HOOKS="$(ENABLE_TEST_HOOKS)" \
	-DSSB64_BUILD_TESTING="$(ENABLE_TESTING)" \
	-DSSB64_ENABLE_COVERAGE="$(ENABLE_COVERAGE)" \
	-DDISABLE_SCRIPTING="$(DISABLE_SCRIPTING)" \
	-DNON_PORTABLE="$(NON_PORTABLE)"

# ------------------------------------------------------------------------------
# Presentation helpers
# ------------------------------------------------------------------------------
BLUE   := \033[1;34m
GREEN  := \033[1;32m
YELLOW := \033[1;33m
RED    := \033[1;31m
RESET  := \033[0m
ifeq ($(COLOR),0)
BLUE   :=
GREEN  :=
YELLOW :=
RED    :=
RESET  :=
endif

define heading
printf "\n$(BLUE)==> %s$(RESET)\n" "$(1)"
endef
define success
printf "$(GREEN)[OK]$(RESET) %s\n" "$(1)"
endef
define warning
printf "$(YELLOW)[WARN]$(RESET) %s\n" "$(1)"
endef
define failure
printf "$(RED)[ERROR]$(RESET) %s\n" "$(1)" >&2
endef

.PHONY: help help-advanced info doctor \
        setup bootstrap dependencies submodules submodules-check submodules-update \
        configure reconfigure configure-clean build build-all rebuild run \
        codegen codegen-check assets generate-yamls synth-verify \
        test test-unit test-integration test-contract test-suite test-list test-all ctest \
        e2e e2e-netplay e2e-lobby e2e-lan e2e-compat e2e-spectate \
        e2e-replay-takeover e2e-mutation e2e-soak e2e-local \
        fuzz fuzz-all coverage coverage-python \
        check quick-check ci \
        lint lint-skip-configure format format-check \
        module-deps env-check docs-check retired-paths-check cvar-check gen-headers-check \
        security semgrep dependency-audit sbom release-binary-check \
        lobby-install lobby-test lobby-dev \
        mods-test \
        package package-windows package-linux package-macos package-android package-uwp \
        artifacts artifact-manifest \
        release-check release-dry-run \
        android android-install android-test uwp \
        docs docs-serve \
        clean clean-build clean-logs clean-reports clean-tests distclean reset \
        print-% guard-region guard-preset guard-configured guard-rom guard-exe

# ------------------------------------------------------------------------------
# Guards
# ------------------------------------------------------------------------------
guard-region:
	@if [[ "$(REGION)" != "us" && "$(REGION)" != "jp" ]]; then \
	  $(call failure,REGION must be us or jp — got '$(REGION)'); exit 1; fi

guard-preset:
	@if [[ "$(HOST_PLATFORM)" == "unknown" ]]; then \
	  $(call failure,Host platform unknown — set PRESET explicitly); exit 1; fi
	@if [[ ! -f CMakePresets.json ]]; then \
	  $(call failure,CMakePresets.json not found); exit 1; fi

guard-configured:
	@if [[ ! -d "$(BUILD_DIR)" ]]; then \
	  $(call failure,Build dir missing: $(BUILD_DIR)); \
	  printf "  run: make PRESET=%s configure\n" "$(PRESET)" >&2; exit 1; fi

guard-rom: guard-region
	@if [[ ! -f "$(ROM_PATH)" ]]; then \
	  $(call failure,ROM not found: $(ROM_PATH)); \
	  printf "  supply a legally obtained ROM or override ROM_US/ROM_JP\n" >&2; \
	  exit 1; fi

guard-exe:
	@if [[ -z "$(EXE_DIR)" ]]; then \
	  $(call failure,$(EXECUTABLE_NAME)$(EXE_SUFFIX) not found under $(BUILD_DIR)); \
	  printf "  run: make PRESET=%s build\n" "$(PRESET)" >&2; exit 1; fi

# Files/dirs every checkout must have — first line of defense against a
# partial clone, a forgotten submodule init, or a stale path after a move.
REQUIRED_FILES := CMakeLists.txt CMakePresets.json Makefile pyproject.toml \
	$(TEST_RUNNER) $(LINT_SCRIPT) $(MODULE_DEPS_SCRIPT) \
	$(LOBBY_DIR)/package.json $(LOBBY_DIR)/package-lock.json \
	roms/README.md
REQUIRED_DIRS  := src include components/decomp components/libultraship \
	components/game-runtime cmake tools \
	assets assets/game config/controllers services platforms tests docs \
	config generated specs roms

layout-check: ## Verify required files/dirs exist
	@$(call heading,Validating repository layout)
	@status=0; \
	for path in $(REQUIRED_FILES); do \
	  if [[ ! -f "$$path" ]]; then \
	    printf "$(RED)[MISSING FILE]$(RESET) %s\n" "$$path"; status=1; \
	  fi; \
	done; \
	for path in $(REQUIRED_DIRS); do \
	  if [[ ! -d "$$path" ]]; then \
	    printf "$(RED)[MISSING DIR]$(RESET)  %s\n" "$$path"; status=1; \
	  fi; \
	done; \
	if [[ $$status -eq 0 ]]; then $(call success,Layout OK); fi; \
	exit $$status

doctor: ## Validate the local development environment
	@$(call heading,Checking development environment)
	@[[ -f tools/versions.env ]] && . tools/versions.env; \
	status=0; \
	ver_ge() { \
	  printf '%s\n%s\n' "$$2" "$$1" | sort -V -C 2>/dev/null \
	  || { IFS=. read -r a b c <<<"$$1"; IFS=. read -r x y z <<<"$$2"; \
	       [[ $$((a*1000000 + b*1000 + c)) -ge $$((x*1000000 + y*1000 + z)) ]]; }; \
	}; \
	for cmd in "$(GIT)" "$(CMAKE)" "$(PYTHON)"; do \
	  if command -v "$$cmd" >/dev/null 2>&1; then \
	    printf "$(GREEN)[OK]$(RESET)       %-10s %s\n" "$$cmd" "$$(command -v "$$cmd")"; \
	  else \
	    printf "$(RED)[MISSING]$(RESET)   %s\n" "$$cmd"; status=1; \
	  fi; \
	done; \
	for cmd in "$(NODE)" ninja; do \
	  if command -v "$$cmd" >/dev/null 2>&1; then \
	    printf "$(GREEN)[OK]$(RESET)       %-10s %s\n" "$$cmd" "$$(command -v "$$cmd")"; \
	  else \
	    printf "$(YELLOW)[OPTIONAL]$(RESET)  %s\n" "$$cmd"; \
	  fi; \
	done; \
	cv="$$($(CMAKE) --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1 || true)"; \
	pv="$$($(PYTHON) --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' || true)"; \
	nv="$$($(NODE) --version 2>/dev/null | tr -d v || true)"; \
	if [[ -n "$$cv" && -n "$${MIN_CMAKE:-}" ]]; then \
	  if ver_ge "$$cv" "$$MIN_CMAKE"; then \
	    printf "$(GREEN)[OK]$(RESET)       cmake %s (>= %s)\n" "$$cv" "$$MIN_CMAKE"; \
	  else \
	    printf "$(RED)[OLD]$(RESET)      cmake %s < %s\n" "$$cv" "$$MIN_CMAKE"; status=1; \
	  fi; \
	fi; \
	if [[ -n "$$pv" && -n "$${MIN_PYTHON:-}" ]]; then \
	  if ver_ge "$$pv" "$$MIN_PYTHON"; then \
	    printf "$(GREEN)[OK]$(RESET)       python %s (>= %s)\n" "$$pv" "$$MIN_PYTHON"; \
	  else \
	    printf "$(RED)[OLD]$(RESET)      python %s < %s\n" "$$pv" "$$MIN_PYTHON"; status=1; \
	  fi; \
	fi; \
	if [[ -n "$$nv" && -n "$${MIN_NODE:-}" ]]; then \
	  if ver_ge "$$nv" "$$MIN_NODE"; then \
	    printf "$(GREEN)[OK]$(RESET)       node %s (>= %s)\n" "$$nv" "$$MIN_NODE"; \
	  else \
	    printf "$(YELLOW)[OLD]$(RESET)   node %s < %s\n" "$$nv" "$$MIN_NODE"; \
	  fi; \
	fi; \
	if [[ -f "$(ROM_PATH)" ]]; then \
	  printf "$(GREEN)[OK]$(RESET)       ROM        %s\n" "$(ROM_PATH)"; \
	else \
	  printf "$(YELLOW)[OPTIONAL]$(RESET)  ROM not found at %s\n" "$(ROM_PATH)"; \
	fi; \
	if git submodule status --recursive 2>/dev/null | grep -q '^-'; then \
	  printf "$(YELLOW)[WARN]$(RESET)      submodules not initialized (make submodules)\n"; \
	else \
	  printf "$(GREEN)[OK]$(RESET)       submodules initialized\n"; \
	fi; \
	exit $$status

