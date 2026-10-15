# ==============================================================================
# Smash Classic / FrameShift64 — developer command facade
#
# CMake (CMakeLists.txt + CMakePresets.json + cmake/) remains the source of
# truth for compilation. This Makefile only provides short, consistent
# commands over CMake, ctest, pytest (tools/testing/run_tests.py), the
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
SHELL := /usr/bin/env bash
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
LINT_SCRIPT          := tools/lint/run_project_lint.py
MODULE_DEPS_SCRIPT   := tools/lint/check_module_deps.py
ENV_VARS_SCRIPT      := tools/lint/check_env_vars.py
DOCS_CHECK_SCRIPT    := tools/lint/check_docs.py
RETIRED_PATHS_SCRIPT := tools/lint/check_retired_paths.py
RELEASE_BIN_SCRIPT   := tools/lint/check_release_binary.py
CVAR_SCHEMA_SCRIPT   := tools/lint/check_cvar_schema.py
GEN_HEADERS_SCRIPT   := tools/lint/check_generated_headers.py
SBOM_SCRIPT          := tools/lint/gen_sbom.py
TEST_RUNNER          := tools/testing/run_tests.py
COVERAGE_TOOL        := tools/testing/coverage.py
PARSER_FUZZ          := tools/fuzz/run_parser_fuzz.py
LOBBY_DIR            := services/netplay-lobby
LOBBY_SECURITY       := $(LOBBY_DIR)/tests/test-lobby-security.js
LOBBY_FUZZ           := $(LOBBY_DIR)/tests/test-lobby-fuzz.js
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
        fuzz coverage coverage-python \
        check quick-check ci \
        lint lint-skip-configure format format-check \
        module-deps env-check docs-check retired-paths-check cvar-check gen-headers-check \
        security semgrep dependency-audit sbom release-binary-check \
        lobby-install lobby-test lobby-dev \
        mods-test \
        package package-windows package-linux package-macos package-android package-uwp \
        release-check release-dry-run \
        android android-install android-test uwp \
        docs docs-serve \
        clean clean-build clean-logs clean-reports clean-tests distclean reset \
        print-% guard-region guard-preset guard-configured guard-rom guard-exe

# ------------------------------------------------------------------------------
# Help
# ------------------------------------------------------------------------------
help: ## Show common commands
	@printf "$(BLUE)%s$(RESET) development commands\n\n" "$(PROJECT_NAME)"
	@printf "  make setup        doctor + submodules + lobby deps + configure\n"
	@printf "  make build        compile the selected preset\n"
	@printf "  make test         quick pytest suite\n"
	@printf "  make check        local quality gate\n"
	@printf "  make package      package for host platform\n\n"
	@printf "Config: PRESET=$(PRESET) REGION=$(REGION) JOBS=auto\n"
	@printf "        BUILD_DIR=$(BUILD_DIR)\n\n"
	@printf "Targets:\n"
	@grep -E '^[a-zA-Z0-9_.-]+:.*?## ' "$(firstword $(MAKEFILE_LIST))" \
	  | awk 'BEGIN {FS = ":.*?## "}; {printf "  %-26s %s\n", $$1, $$2}' | sort

help-advanced: ## Show advanced variables and examples
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

info: ## Print the active configuration
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
REQUIRED_DIRS  := src include decomp libultraship torch cmake tools \
	assets assets/recipes assets/controllers services platforms tests docs \
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

# ------------------------------------------------------------------------------
# Setup
# ------------------------------------------------------------------------------
setup: layout-check doctor install generate-sources configure ## Full developer checkout setup
	@$(call success,Development checkout ready)

bootstrap: dependencies ## Install project dependencies
	@$(call success,Dependencies initialized)

dependencies: install

install: submodules install-python install-node ## Install all dependencies

install-python: ## Create .venv + install locked deps (requirements.lock)
	@$(call heading,Installing python dependencies)
	@if [[ ! -x "$(VENV_PYTHON)" ]]; then \
	  $(PYTHON) -m venv .venv; \
	fi
	@"$(VENV_PYTHON)" -m pip install -r requirements.lock
	@"$(VENV_PYTHON)" -m pip install -e . --no-deps
	@$(call success,Python deps installed (.venv))

lock-python: ## Regenerate requirements.lock from pyproject [test] (needs uv)
	@uv pip compile pyproject.toml --extra test -o requirements.lock
	@$(call success,requirements.lock regenerated)

install-node: lobby-install ## npm ci for services/netplay-lobby

submodules: ## git submodule update --init --recursive
	@$(call heading,Initializing submodules)
	@$(GIT) submodule sync --recursive
	@$(GIT) submodule update --init --recursive
	@$(call success,Submodules initialized)

submodules-check: ## Verify submodule state is clean
	@$(GIT) submodule status --recursive
	@if $(GIT) submodule status --recursive | grep -Eq '^[-+U]'; then \
	  $(call failure,Submodule state not clean); exit 1; fi
	@$(call success,Submodule state clean)

submodules-update: ## Update submodules to remote tracking branches
	@$(GIT) submodule update --init --recursive --remote
	@$(call success,Submodules updated)

# ------------------------------------------------------------------------------
# Configure / build / run
# ------------------------------------------------------------------------------
generate-sources: generate-yamls ## Regenerate committed codegen inputs (recipes)

configure: guard-region guard-preset generate-sources ## cmake --preset <PRESET>
	@$(call heading,Configuring $(PRESET))
	@mkdir -p "$(OUT_DIR)" "$(LOG_DIR)" "$(REPORT_DIR)"
	@$(CMAKE) --preset "$(PRESET)" $(CMAKE_FEATURE_ARGS) $(CMAKE_ARGS)
	@$(call success,Configured $(PRESET))

reconfigure: configure
configure-clean: clean-build configure

build: guard-configured ## Build the main application (needs configure first)
	@$(call heading,Building $(APP_TARGET) @ $(PRESET))
	@$(CMAKE) --build --preset "$(PRESET)" --target "$(APP_TARGET)" \
	  $(BUILD_PARALLEL) $(BUILD_VERBOSE) $(BUILD_ARGS)
	@$(call success,Build completed)

build-all: guard-configured ## Build every target
	@$(CMAKE) --build --preset "$(PRESET)" \
	  $(BUILD_PARALLEL) $(BUILD_VERBOSE) $(BUILD_ARGS)

dev-build: configure codegen build ## configure + codegen + build (comfort path)

rebuild: clean-build configure codegen build

run: build guard-rom guard-exe ## Build and launch the game
	@$(call heading,Running $(EXECUTABLE_NAME))
	@exe_dir="$(dir $(EXE_DIR))"; \
	printf "Executable: %s\n" "$$exe_dir$(EXECUTABLE_NAME)$(EXE_SUFFIX)"; \
	cd "$$exe_dir" && "./$(EXECUTABLE_NAME)$(EXE_SUFFIX)" $(RUN_ARGS)

# ------------------------------------------------------------------------------
# Codegen / assets
# ------------------------------------------------------------------------------
codegen: guard-configured ## Run the CMake-managed generators (reloc, asset headers)
	@$(call heading,Running codegen targets)
	@$(CMAKE) --build --preset "$(PRESET)" --target PrepareBuildInputs \
	  --target "$(ASSET_HEADERS_TARGET)" $(BUILD_PARALLEL)
	@$(call success,Codegen completed)

generate-yamls: ## Regenerate extraction recipes + reloc yaml tables
	@$(PYTHON) tools/codegen/generate_yamls.py

codegen-check: ## Fail if codegen changes the working tree
	@before="$$($(GIT) status --porcelain)"; \
	$(MAKE) --no-print-directory codegen; \
	after="$$($(GIT) status --porcelain)"; \
	if [[ "$$before" != "$$after" ]]; then \
	  $(call failure,Codegen changed the working tree); \
	  git status --short; exit 1; fi
	@$(call success,Generated sources are current)

assets: configure guard-rom ## Extract runtime assets from the ROM (Torch)
	@$(call heading,Extracting assets ($(REGION)))
	@$(CMAKE) --build --preset "$(PRESET)" --target "$(ASSETS_TARGET)" \
	  $(BUILD_PARALLEL) $(BUILD_VERBOSE)
	@$(call success,Assets extracted)

synth-verify: configure ## Build the synthetic-reloc verification target
	@$(CMAKE) --build --preset "$(PRESET)" --target "$(SYNTH_VERIFY_TARGET)"

# ------------------------------------------------------------------------------
# Testing — pytest is the orchestrator; ctest for native binaries
# ------------------------------------------------------------------------------
test: ## Run the quick suite (unit+smoke, no rom/net/gpu)
	@$(call heading,pytest suite: quick)
	@$(PYTHON) "$(TEST_RUNNER)" --suite quick $(TEST_ARGS)

test-suite: ## Run TEST_SUITE from tests/config/test-suites.yml
	@$(call heading,pytest suite: $(TEST_SUITE))
	@args=""; \
	if [[ -n "$(EXE_DIR)" ]]; then args="--build-dir $$(dirname '$(EXE_DIR)')"; fi; \
	$(PYTHON) "$(TEST_RUNNER)" --suite "$(TEST_SUITE)" --repeat "$(REPEAT)" \
	  $$args $(TEST_ARGS)

test-unit: ## pytest -m unit
	@$(PYTHON) -m pytest -m unit $(TEST_ARGS)
test-integration: ## pytest -m integration
	@$(PYTHON) -m pytest -m integration $(TEST_ARGS)
test-contract: ## pytest -m contract
	@$(PYTHON) -m pytest -m contract $(TEST_ARGS)

ctest: guard-configured ## Run CTest-registered native tests
	@$(CTEST) --test-dir "$(BUILD_DIR)" $(CTEST_PARALLEL) $(CTEST_VERBOSE) \
	  $(CTEST_REGEX) $(CTEST_LABEL) $(TEST_ARGS)

test-list: guard-configured ## List ctest registrations
	@$(CTEST) --test-dir "$(BUILD_DIR)" --show-only

test-all: test-suite lobby-test ## pytest full + lobby suites
	@$(MAKE) --no-print-directory TEST_SUITE=full test-suite

# ------------------------------------------------------------------------------
# E2E netplay — drives the Python drivers in tests/e2e/netplay/
# (the scripts/testing/*.ps1 wrappers call the same code)
# ------------------------------------------------------------------------------
e2e: ## Run E2E_SCENARIO (all|lobby|lan|compat|spectate|replay|mutation|soak|local)
	@case "$(E2E_SCENARIO)" in \
	  all)            $(MAKE) --no-print-directory e2e-local e2e-lobby ;; \
	  lobby)          $(MAKE) --no-print-directory e2e-lobby ;; \
	  lobby-multi)    $(MAKE) --no-print-directory e2e-lobby ;; \
	  lan)            $(MAKE) --no-print-directory e2e-lan ;; \
	  compat)         $(MAKE) --no-print-directory e2e-compat ;; \
	  spectate)       $(MAKE) --no-print-directory e2e-spectate ;; \
	  replay-takeover|replay) $(MAKE) --no-print-directory e2e-replay-takeover ;; \
	  mutation)       $(MAKE) --no-print-directory e2e-mutation ;; \
	  soak)           $(MAKE) --no-print-directory e2e-soak ;; \
	  local|localtest) $(MAKE) --no-print-directory e2e-local ;; \
	  *) $(call failure,Unknown E2E_SCENARIO '$(E2E_SCENARIO)'); exit 1 ;; \
	esac

e2e-local: guard-exe ## Two localhost peers, trace-diffed (rollback)
	@out="$(LOG_DIR)/netplay-local"; mkdir -p "$$out"; \
	bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) "$(E2E_DIR)/localtest_test.py" --seconds "$(E2E_SECONDS)" \
	  --build-dir "$$bd" --out-dir "$$out" $(TEST_ARGS)

e2e-lobby: guard-exe ## Host + N-1 clients over the local lobby
	@out="$(LOG_DIR)/lobby-multi"; mkdir -p "$$out"; \
	bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) "$(E2E_DIR)/lobbytest_multi_test.py" \
	  --players "$(NETPLAY_PLAYERS)" --seconds "$(E2E_SECONDS)" \
	  --build-dir "$$bd" --out-dir "$$out" $(TEST_ARGS)

e2e-netplay: e2e-lobby

e2e-lan: guard-exe ## Direct-IP LAN session, no lobby
	@out="$(LOG_DIR)/lan"; mkdir -p "$$out"; \
	bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) "$(E2E_DIR)/lan_test.py" \
	  --players "$(NETPLAY_PLAYERS)" --seconds "$(E2E_SECONDS)" \
	  --build-dir "$$bd" --out-dir "$$out" $(TEST_ARGS)

e2e-compat: guard-exe ## Compat-handshake rejection test
	@out="$(LOG_DIR)/compat"; mkdir -p "$$out"; \
	bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) "$(E2E_DIR)/compat_test.py" \
	  --build-dir "$$bd" --out-dir "$$out" $(TEST_ARGS)

e2e-spectate: guard-exe ## Players + spectator trace-diffed
	@out="$(LOG_DIR)/spectate"; mkdir -p "$$out"; \
	bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) "$(E2E_DIR)/spectate_test.py" \
	  --players "$(NETPLAY_PLAYERS)" --seconds "$(E2E_SECONDS)" \
	  --build-dir "$$bd" --out-dir "$$out" $(TEST_ARGS)

e2e-replay-takeover: guard-exe ## Replay playback / takeover (needs REPLAY=<file>)
	@if [[ -z "$(REPLAY)" ]]; then \
	  $(call failure,Set REPLAY=<path-to.ssr>); exit 1; fi
	@out="$(LOG_DIR)/takeover"; mkdir -p "$$out"; \
	bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) "$(E2E_DIR)/replay_takeover_test.py" \
	  --replay "$(REPLAY)" --seconds "$(E2E_SECONDS)" \
	  --build-dir "$$bd" --out-dir "$$out" $(TEST_ARGS)

e2e-mutation: guard-exe ## Mutation suite (Windows: drives rollback_synctest.ps1)
	@out="$(LOG_DIR)/mutation"; mkdir -p "$$out"; \
	bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) "$(E2E_DIR)/mutation_suite_test.py" \
	  --build-dir "$$bd" --out-dir "$$out" $(TEST_ARGS)

e2e-soak: guard-exe ## Rematch rounds + optional long synctest
	@out="$(LOG_DIR)/soak"; mkdir -p "$$out"; \
	bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) "$(E2E_DIR)/soak_test.py" --seconds "$(E2E_SECONDS)" \
	  --build-dir "$$bd" --out-dir "$$out" $(TEST_ARGS)

# ------------------------------------------------------------------------------
# Fuzz + coverage
# ------------------------------------------------------------------------------
PARSER ?= manifest
ITERS  ?= 300
fuzz: guard-exe ## Run a parser fuzz driver (PARSER=manifest|json|replay)
	@bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) "$(PARSER_FUZZ)" --parser "$(PARSER)" --iters "$(ITERS)" \
	  --binary "$$bd/$(EXECUTABLE_NAME)$(EXE_SUFFIX)" $(TEST_ARGS)

coverage-python: ## Python coverage report (pytest-cov)
	@$(PYTHON) -m pytest -m "unit or integration" \
	  --cov --cov-branch --cov-context=test \
	  --cov-report=term-missing \
	  --cov-report="xml:$(OUT_DIR)/coverage/python/coverage.xml" \
	  --cov-report="html:$(OUT_DIR)/coverage/python/html"

coverage: ## Full coverage: instrumented build + run + merge (clang/gcc only)
	@$(call warning,Native coverage needs a Clang/GCC toolchain build)
	@$(MAKE) --no-print-directory ENABLE_COVERAGE=1 configure build
	@LLVM_PROFILE_FILE="$(CURDIR)/$(OUT_DIR)/coverage/native/raw/%p-%m.profraw" \
	  $(MAKE) --no-print-directory TEST_SUITE=pull-request test-suite
	@bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) "$(COVERAGE_TOOL)" \
	  --binary "$$bd/$(EXECUTABLE_NAME)$(EXE_SUFFIX)" \
	  --output-dir "$(OUT_DIR)/coverage/native"

# ------------------------------------------------------------------------------
# Quality gates
# ------------------------------------------------------------------------------
quick-check: module-deps retired-paths-check test ## Fast local gate
check: lint module-deps env-check docs-check retired-paths-check \
       cvar-check gen-headers-check test-suite ## Standard local gate
	@$(MAKE) --no-print-directory TEST_SUITE=pull-request test-suite
	@$(call success,Quality gate passed)

ci: submodules-check check security lobby-test codegen-check ## Local CI replay
	@$(call success,CI-equivalent checks passed)

lint: ## tools/lint/run_project_lint.py (configures its own build dir)
	@mkdir -p "$(REPORT_DIR)/lint"
	@$(PYTHON) "$(LINT_SCRIPT)" $(LINT_ARGS)
	@$(call success,Lint passed)

lint-skip-configure: ## Lint without the cmake-configure phase
	@$(PYTHON) "$(LINT_SCRIPT)" --skip-configure $(LINT_ARGS)

format: ## clang-format src/ (excludes submodules)
	@$(call heading,Formatting)
	@if command -v clang-format >/dev/null 2>&1; then \
	  git ls-files 'src/*.c' 'src/*.cc' 'src/*.cpp' 'src/*.h' 'src/*.hpp' \
	    ':(exclude)src/integration/decomp/*' -z \
	    | xargs -0 -r clang-format -i; \
	else \
	  $(call warning,clang-format not found); \
	fi
	@$(call success,Formatting completed)

format-check: ## Verify formatting (clang-format --dry-run)
	@if command -v clang-format >/dev/null 2>&1; then \
	  git ls-files 'src/*.c' 'src/*.cc' 'src/*.cpp' 'src/*.h' 'src/*.hpp' \
	    ':(exclude)src/integration/decomp/*' -z \
	    | xargs -0 -r clang-format --dry-run --Werror; \
	else \
	  $(call warning,clang-format not found — skipped); \
	fi

static-analysis: configure ## CMake static-analysis target, if registered
	@$(CMAKE) --build --preset "$(PRESET)" --target static-analysis \
	  $(BUILD_PARALLEL) || \
	  $(call warning,no static-analysis target registered in this preset)

module-deps: ## Architectural dependency rules vs baseline
	@$(PYTHON) "$(MODULE_DEPS_SCRIPT)"

env-check: ## env_vars.md catalog coverage
	@$(PYTHON) "$(ENV_VARS_SCRIPT)"

docs-check: ## Internal doc/link conventions
	@$(PYTHON) "$(DOCS_CHECK_SCRIPT)"

retired-paths-check: ## Reject retired paths in src/ + cmake/
	@$(PYTHON) "$(RETIRED_PATHS_SCRIPT)"

cvar-check: ## CVar schema coverage
	@$(PYTHON) "$(CVAR_SCHEMA_SCRIPT)"

gen-headers-check: ## Generated-file banner check
	@$(PYTHON) "$(GEN_HEADERS_SCRIPT)"

# ------------------------------------------------------------------------------
# Security / supply chain
# ------------------------------------------------------------------------------
security: semgrep dependency-audit release-binary-check sbom
	@$(call success,Security checks passed)

semgrep: ## Semgrep rules in config/analysis/semgrep
	@if ! command -v semgrep >/dev/null 2>&1; then \
	  $(call failure,semgrep required); exit 1; fi
	@mkdir -p "$(REPORT_DIR)/security"
	@semgrep --config config/analysis/semgrep --error \
	  --json-output "$(REPORT_DIR)/security/semgrep.json" .

dependency-audit: ## npm audit (lobby) + submodule pins
	@if [[ -f "$(LOBBY_DIR)/package-lock.json" ]]; then \
	  (cd "$(LOBBY_DIR)" && $(NPM) audit); \
	else \
	  $(call warning,no lobby lockfile); \
	fi

sbom: ## CycloneDX SBOM -> out/reports/sbom/
	@mkdir -p "$(REPORT_DIR)/sbom"
	@$(PYTHON) "$(SBOM_SCRIPT)" --out "$(REPORT_DIR)/sbom/$(PROJECT_NAME).cdx.json"

release-binary-check: ## Scan a built binary for test-hook strings
	@bd="$$(dirname '$(EXE_DIR)')"; \
	if [[ -n "$$bd" ]]; then \
	  $(PYTHON) "$(RELEASE_BIN_SCRIPT)" "$$bd/$(EXECUTABLE_NAME)$(EXE_SUFFIX)"; \
	else \
	  $(call warning,no binary — skipping release-binary check); \
	fi

# ------------------------------------------------------------------------------
# Lobby service (services/netplay-lobby)
# ------------------------------------------------------------------------------
lobby-install: ## npm ci for the lobby service
	@$(call heading,Installing lobby dependencies)
	@if [[ -f "$(LOBBY_DIR)/package-lock.json" ]]; then \
	  (cd "$(LOBBY_DIR)" && $(NPM) ci); \
	else \
	  (cd "$(LOBBY_DIR)" && $(NPM) install); \
	fi

lobby-test: ## Lobby security (21 checks) + fuzz (400 iters)
	@$(call heading,Lobby security + fuzz)
	@$(NODE) "$(LOBBY_SECURITY)"
	@$(NODE) "$(LOBBY_FUZZ)" 400
	@$(call success,Lobby tests passed)

lobby-dev: ## wrangler dev on :8799
	@cd "$(LOBBY_DIR)" && $(NPM) run dev

# ------------------------------------------------------------------------------
# Mods
# ------------------------------------------------------------------------------
mods-test: ## pytest -m modding
	@$(PYTHON) -m pytest -m modding $(TEST_ARGS)

# ------------------------------------------------------------------------------
# Packaging — platforms/<os>/packaging/* self-configure into build-bundle-*
# and drop artifacts in dist/. Region + scripting via env vars.
# ------------------------------------------------------------------------------
package: ## Package for the host platform -> dist/
	@case "$(HOST_PLATFORM)" in \
	  windows) $(MAKE) --no-print-directory package-windows ;; \
	  linux)   $(MAKE) --no-print-directory package-linux ;; \
	  macos)   $(MAKE) --no-print-directory package-macos ;; \
	  *) $(call failure,Unsupported packaging host: $(HOST_PLATFORM)); exit 1 ;; \
	esac

package-windows: guard-region ## platforms/windows/packaging/package-windows.ps1 -> dist/
	@$(call heading,Packaging windows ($(REGION)))
	@mkdir -p "$(DIST_DIR)"
	@SSB64_VERSION="$(REGION)" $(POWERSHELL) -NoProfile \
	  -ExecutionPolicy Bypass -File platforms/windows/packaging/package-windows.ps1 \
	  $(PACKAGE_ARGS)
	@$(call success,Windows package in $(DIST_DIR)/)

package-linux: guard-region ## platforms/linux/packaging/package-linux.sh -> dist/
	@$(call heading,Packaging linux ($(REGION)))
	@mkdir -p "$(DIST_DIR)"
	@SSB64_VERSION="$(REGION)" bash platforms/linux/packaging/package-linux.sh \
	  $(PACKAGE_ARGS)
	@$(call success,Linux package in $(DIST_DIR)/)

package-macos: guard-region ## platforms/macos/packaging/package-macos.sh -> dist/
	@$(call heading,Packaging macos ($(REGION)))
	@mkdir -p "$(DIST_DIR)"
	@SSB64_VERSION="$(REGION)" bash platforms/macos/packaging/package-macos.sh \
	  $(PACKAGE_ARGS)
	@$(call success,macOS package in $(DIST_DIR)/)

package-android: android ## Collect APK/AAB into dist/android
	@mkdir -p "$(DIST_DIR)/android"
	@find "$(ANDROID_DIR)" -type f \( -name '*.apk' -o -name '*.aab' \) \
	  -exec cp -f {} "$(DIST_DIR)/android/" \;

package-uwp: uwp ## Collect MSIX/APPX into dist/uwp
	@mkdir -p "$(DIST_DIR)/uwp"
	@find "$(UWP_DIR)" -type f \
	  \( -name '*.msix' -o -name '*.msixbundle' -o -name '*.appx' \) \
	  -exec cp -f {} "$(DIST_DIR)/uwp/" \;

release-check: layout-check package security release-binary-check codegen-check ## Validate a release candidate
	@$(call success,Release candidate validated)

release-dry-run: ## release-check on the <host>-release preset
	@$(MAKE) --no-print-directory \
	  BUILD_TYPE=release PRESET="$(HOST_PLATFORM)-release" release-check

# ------------------------------------------------------------------------------
# Platform builds
# ------------------------------------------------------------------------------
android: guard-region ## Gradle assembleDebug (platforms/android)
	@$(call heading,Android build ($(REGION)))
	@cd "$(ANDROID_DIR)" && \
	  if [[ -x ./gradlew ]]; then \
	    ./gradlew assembleDebug $(BUILD_ARGS); \
	  elif [[ -f ./gradlew.bat ]]; then \
	    ./gradlew.bat assembleDebug; \
	  else \
	    $(call failure,Gradle wrapper not found); exit 1; \
	  fi

android-install: android ## gradlew installDebug
	@cd "$(ANDROID_DIR)" && ./gradlew installDebug

android-test: ## gradlew testDebugUnitTest (+ connected tests if adb device)
	@cd "$(ANDROID_DIR)" && ./gradlew testDebugUnitTest && \
	  if command -v adb >/dev/null 2>&1 && adb get-state >/dev/null 2>&1; then \
	    ./gradlew connectedDebugAndroidTest; \
	  else \
	    printf "$(YELLOW)[WARN]$(RESET) no device — instrumentation skipped\n"; \
	  fi

uwp: ## platforms/uwp build entrypoint
	@$(call heading,UWP build)
	@if [[ -f "$(UWP_DIR)/build.ps1" ]]; then \
	  $(POWERSHELL) -NoProfile -File "$(UWP_DIR)/build.ps1" $(BUILD_ARGS); \
	else \
	  $(call failure,UWP build entrypoint not found); exit 1; \
	fi

# ------------------------------------------------------------------------------
# Docs
# ------------------------------------------------------------------------------
docs: docs-check

docs-serve: ## Serve docs/ on :8000
	@$(PYTHON) -m http.server 8000 --directory docs

# ------------------------------------------------------------------------------
# Cleanup
# ------------------------------------------------------------------------------
clean: clean-logs clean-reports clean-tests ## Remove generated local outputs
	@$(call success,Local outputs removed)

clean-build: ## rm -rf the selected preset build dir
	@rm -rf -- "$(BUILD_DIR)"

clean-logs:
	@rm -rf -- "$(LOG_DIR)"
clean-reports:
	@rm -rf -- "$(REPORT_DIR)"
clean-tests:
	@rm -rf -- "$(TEST_OUTPUT_DIR)" "$(OUT_DIR)/coverage"

distclean: ## rm -rf out/ + lobby node_modules + dist/
	@rm -rf -- "$(OUT_DIR)" "$(DIST_DIR)"
	@rm -rf -- "$(LOBBY_DIR)/node_modules"

# Safe-delete policy: paths must be non-empty, repo-relative, and resolved
# inside the repo before rm -rf ever runs. NEVER delete: src include
# assets config docs specs tests/support/fixtures roms decomp libultraship torch.
define safe_rmrf
	@p="$$(cd "$(1)" 2>/dev/null && pwd)"; \
	root="$$(pwd)"; \
	if [[ -z "$$p" ]]; then \
	  printf "$(YELLOW)[SKIP]$(RESET) $(1) (absent)\n"; \
	elif [[ "$$p" != "$$root"/* ]]; then \
	  $(call failure,Refusing to delete outside repo: $$p); exit 1; \
	else \
	  rm -rf -- "$(1)" && printf "$(GREEN)[RM]$(RESET)   $(1)\n"; \
	fi
endef

clean-generated: ## Remove regenerable outputs (out/ dist/)
	$(call safe_rmrf,$(OUT_DIR))
	$(call safe_rmrf,$(DIST_DIR))
	@$(call success,Generated outputs removed)

clean-dependencies: ## Remove installed deps (.venv, node_modules)
	$(call safe_rmrf,$(VENV_DIR))
	$(call safe_rmrf,.venv-lint)
	$(call safe_rmrf,$(LOBBY_DIR)/node_modules)
	@$(call success,Installed dependencies removed)

regenerate: clean-generated install generate-sources configure codegen ## Rebuild all derived state

generated-check: codegen-check ## Alias — regen must not change the tree

prune: clean-generated clean-dependencies ## Remove outputs AND installed deps
	@$(call success,Generated state removed)

reset: distclean ## Reset submodules to recorded commits
	@$(GIT) submodule foreach --recursive 'git reset --hard && git clean -fdx'
	@$(GIT) submodule update --init --recursive

# ------------------------------------------------------------------------------
# Debugging
# ------------------------------------------------------------------------------
print-%: ## Print a Make variable: make print-PRESET
	@printf '%s = %s\n' '$*' '$($*)'
