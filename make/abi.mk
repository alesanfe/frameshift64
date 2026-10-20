# ==============================================================================
# abi.mk — port_* ABI as a first-class gate
#
# The decomp<->port boundary is the port_* extern "C" surface. These targets
# make ABI drift a CI failure rather than a review catch:
#   abi-version   sanity-check PORT_ABI_* constants + port_abi_version()
#   abi-exports   diff exported port_* symbols vs config/abi/exports.json
#   abi-compat    semver policy vs the last released version
#   abi-baseline  regenerate the baseline — requires ALLOW_UPDATE=1
# ==============================================================================

ABI_TOOLS   := tools/analysis/abi
ABI_BASELINE := config/abi/exports.json
ABI_DIR     := config/abi

.PHONY: abi-check abi-version abi-exports abi-baseline abi-compat abi-report \
        abi-consumer-c abi-consumer-cpp guard-allow-update

abi-check: abi-version abi-exports abi-compat ## [ABI] Run every ABI check

abi-version: ## [ABI] Validate PORT_ABI_* constants and public headers
	@$(PROJECT_PYTHON) $(ABI_TOOLS)/check_abi_version.py

abi-exports: guard-exe ## [ABI] Diff exported port_* symbols against the baseline
	@$(PROJECT_PYTHON) $(ABI_TOOLS)/check_exports.py \
	  --binary "$(EXE_DIR)" \
	  --baseline "$(ABI_BASELINE)" \
	  --report "$(REPORT_DIR)/abi-exports.json"

abi-baseline: guard-allow-update guard-exe ## [ABI] Rewrite the baseline (needs ALLOW_UPDATE=1)
	@$(PROJECT_PYTHON) $(ABI_TOOLS)/check_exports.py \
	  --binary "$(EXE_DIR)" \
	  --baseline "$(ABI_BASELINE)" --update

abi-compat: guard-exe ## [ABI] Enforce semver vs the last released ABI
	@$(PROJECT_PYTHON) $(ABI_TOOLS)/check_compatibility.py \
	  --baseline "$(ABI_DIR)" \
	  --binary "$(EXE_DIR)"

abi-report: ## [ABI] Markdown report of the public port_* surface
	@$(PROJECT_PYTHON) $(ABI_TOOLS)/generate_report.py \
	  --output "$(REPORT_DIR)/abi-report.md"

# These compile dedicated consumer TUs when the CMake targets are registered
# (see tests/abi/). Until they exist the targets fail informatively.
abi-consumer-c: guard-configured ## [ABI] Compile a C consumer of the public ABI
	@if ! $(CMAKE) --build --preset "$(PRESET)" --target port_api_consumer_c \
	    2>/dev/null; then \
	  $(call failure,port_api_consumer_c target not registered); \
	  printf "  add it under tests/abi/ (see docs/architecture/public-api.md)\n" >&2; \
	  exit 1; fi

abi-consumer-cpp: guard-configured ## [ABI] Compile a C++ consumer of the public ABI
	@if ! $(CMAKE) --build --preset "$(PRESET)" --target port_api_consumer_cpp \
	    2>/dev/null; then \
	  $(call failure,port_api_consumer_cpp target not registered); \
	  printf "  add it under tests/abi/ (see docs/architecture/public-api.md)\n" >&2; \
	  exit 1; fi

guard-allow-update:
	@test "$(ALLOW_UPDATE)" = "1" || { \
	  $(call failure,this operation modifies a committed baseline); \
	  printf "  run intentionally: ALLOW_UPDATE=1 make %s\n" \
	    "$(@F)" >&2; exit 2; }
