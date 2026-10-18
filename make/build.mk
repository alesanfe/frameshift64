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

# Sanitizer presets — dev-only instrumented builds (never ship these)
asan: ## Configure+build+test with AddressSanitizer (<host>-asan preset)
	@$(MAKE) --no-print-directory PRESET="$(HOST_PLATFORM)-asan" configure build test-suite

ubsan: ## Configure+build+test with UBSan (<host>-ubsan preset; Clang/GCC only)
ifeq ($(HOST_PLATFORM),windows)
	@$(call failure,UBSan is not supported on MSVC — use 'make asan' or a clang toolchain)
	@exit 1
else
	@$(MAKE) --no-print-directory PRESET="$(HOST_PLATFORM)-ubsan" configure build test-suite
endif

sanitizers: asan ubsan ## ASan + UBSan passes

run: build guard-rom guard-exe ## Build and launch the game
	@$(call heading,Running $(EXECUTABLE_NAME))
	@exe_dir="$(dir $(EXE_DIR))"; \
	printf "Executable: %s\n" "$$exe_dir$(EXECUTABLE_NAME)$(EXE_SUFFIX)"; \
	cd "$$exe_dir" && "./$(EXECUTABLE_NAME)$(EXE_SUFFIX)" $(RUN_ARGS)

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
# assets config docs specs tests/support/fixtures roms components/*.
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

