# ------------------------------------------------------------------------------
# Quality gates
# ------------------------------------------------------------------------------
# verify-fast — daily-driver gate (structure + format + arch + quick tests)
verify-fast: layout-check format-check module-deps retired-paths-check test
	@$(call success,Fast verification passed)

quick-check: module-deps retired-paths-check test ## Fast local gate
check: layout-check lint module-deps env-check docs-check retired-paths-check \
       cvar-check gen-headers-check test-suite ## Standard local gate
	@$(MAKE) --no-print-directory TEST_SUITE=pull-request test-suite
	@$(call success,Quality gate passed)

ci: submodules-check check security lobby-test codegen-check ## Local CI replay
	@$(call success,CI-equivalent checks passed)

# verify — full validation chain (what a release-candidate must pass)
verify: layout-check doctor check security generated-check build
	@$(call success,Full verification passed)

lint: ## tools/analysis/lint/run_project_lint.py (configures its own build dir)
	@mkdir -p "$(REPORT_DIR)/lint"
	@$(PYTHON) "$(LINT_SCRIPT)" $(LINT_ARGS)
	@$(call success,Lint passed)

lint-skip-configure: ## Lint without the cmake-configure phase
	@$(PYTHON) "$(LINT_SCRIPT)" --skip-configure $(LINT_ARGS)

format: ## clang-format src/ (excludes submodules)
	@$(call heading,Formatting)
	@if command -v clang-format >/dev/null 2>&1; then \
	  git ls-files 'src/*.c' 'src/*.cc' 'src/*.cpp' 'src/*.h' 'src/*.hpp' \
	    ':(exclude)src/game/engine/decomp/*' -z \
	    | xargs -0 -r clang-format -i; \
	else \
	  $(call warning,clang-format not found); \
	fi
	@$(call success,Formatting completed)

format-check: ## Verify formatting (clang-format --dry-run)
	@if command -v clang-format >/dev/null 2>&1; then \
	  git ls-files 'src/*.c' 'src/*.cc' 'src/*.cpp' 'src/*.h' 'src/*.hpp' \
	    ':(exclude)src/game/engine/decomp/*' -z \
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
# Docs
# ------------------------------------------------------------------------------
docs: docs-check

docs-serve: ## Serve docs/ on :8000
	@$(PYTHON) -m http.server 8000 --directory docs

