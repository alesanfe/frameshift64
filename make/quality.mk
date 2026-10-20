# ------------------------------------------------------------------------------
# Quality gates
# ------------------------------------------------------------------------------
# verify-fast — daily-driver gate (structure + format + arch + quick tests)
verify-fast: layout-check format-check module-deps retired-paths-check test ## [Workflow] Fast verification for iteration
	@$(call success,Fast verification passed)

quick-check: module-deps retired-paths-check test  ## [Quality] Fast local gate
check: layout-check lint module-deps env-check docs-check retired-paths-check \
       cvar-check gen-headers-check complexity-check file-size-check \
       encoding-check makefile-check schemas-check \
       test-suite ## [Quality] Main static quality gate
	@$(MAKE) --no-print-directory TEST_SUITE=pull-request test-suite
	@$(call success,Quality gate passed)

ci: submodules-check check security lobby-test codegen-check  ## [Workflow] Local CI replay
	@$(call success,CI-equivalent checks passed)

# verify — full validation chain (what a release-candidate must pass)
verify: layout-check doctor check security generated-check build ## [Workflow] Full project verification (PR-equivalent)
	@$(call success,Full verification passed)

lint:  ## [Quality] tools/analysis/lint/run_project_lint.py (configures its own build dir)
	@mkdir -p "$(REPORT_DIR)/lint"
	@$(PYTHON) "$(LINT_SCRIPT)" $(LINT_ARGS)
	@$(call success,Lint passed)

lint-skip-configure:  ## [Quality] Lint without the cmake-configure phase
	@$(PYTHON) "$(LINT_SCRIPT)" --skip-configure $(LINT_ARGS)

format:  ## [Quality] clang-format src/ (excludes submodules)
	@$(call heading,Formatting)
	@if command -v clang-format >/dev/null 2>&1; then \
	  git ls-files 'src/*.c' 'src/*.cc' 'src/*.cpp' 'src/*.h' 'src/*.hpp' \
	    ':(exclude)src/game/engine/decomp/*' -z \
	    | xargs -0 -r clang-format -i; \
	else \
	  $(call warning,clang-format not found); \
	fi
	@$(call success,Formatting completed)

format-check:  ## [Quality] Verify formatting (clang-format --dry-run)
	@if command -v clang-format >/dev/null 2>&1; then \
	  git ls-files 'src/*.c' 'src/*.cc' 'src/*.cpp' 'src/*.h' 'src/*.hpp' \
	    ':(exclude)src/game/engine/decomp/*' -z \
	    | xargs -0 -r clang-format --dry-run --Werror; \
	else \
	  $(call warning,clang-format not found — skipped); \
	fi

static-analysis: configure  ## [Quality] CMake static-analysis target, if registered
	@$(CMAKE) --build --preset "$(PRESET)" --target static-analysis \
	  $(BUILD_PARALLEL) || \
	  $(call warning,no static-analysis target registered in this preset)

module-deps:  ## [Quality] Architectural dependency rules vs baseline
	@$(PYTHON) "$(MODULE_DEPS_SCRIPT)"

schemas-check:  ## [Quality] Validate specs/schemas/*.schema.json + fixtures
	@$(PYTHON) tools/analysis/lint/check_json_schemas.py

env-check:  ## [Quality] env_vars.md catalog coverage
	@$(PYTHON) "$(ENV_VARS_SCRIPT)"

docs-check:  ## [Quality] Internal doc/link conventions
	@$(PYTHON) "$(DOCS_CHECK_SCRIPT)"

retired-paths-check:  ## [Quality] Reject retired paths in src/ + cmake/
	@$(PYTHON) "$(RETIRED_PATHS_SCRIPT)"

cvar-check:  ## [Quality] CVar schema coverage
	@$(PYTHON) "$(CVAR_SCHEMA_SCRIPT)"

gen-headers-check:  ## [Quality] Generated-file banner check
	@$(PYTHON) "$(GEN_HEADERS_SCRIPT)"

# ------------------------------------------------------------------------------
# Ratchets / structural budgets
# ------------------------------------------------------------------------------
architecture-check: module-deps ## [Quality] Validate module boundaries (alias of module-deps)

complexity-check:  ## [Quality] CCN/NLOC ratchet vs baseline (fails on new violations)
	@$(PROJECT_PYTHON) tools/analysis/lint/check_complexity.py

complexity-report:  ## [Quality] Print every complexity violation
	@$(PROJECT_PYTHON) tools/analysis/lint/check_complexity.py --report

file-size-check:  ## [Quality] Per-file line-count caps vs baseline
	@$(PROJECT_PYTHON) tools/analysis/lint/check_file_size.py

encoding-check:  ## [Quality] Reject NUL bytes, invalid UTF-8, mixed EOL
	@$(PROJECT_PYTHON) tools/analysis/lint/check_source_encoding.py

baseline-check: complexity-check file-size-check ## [Quality] All baselines must not regress

quality-report:  ## [Quality] Write quality reports under $(REPORT_DIR)
	@mkdir -p "$(REPORT_DIR)"
	@$(PROJECT_PYTHON) tools/analysis/lint/check_complexity.py --report \
	  > "$(REPORT_DIR)/complexity.txt" || true
	@$(PROJECT_PYTHON) tools/analysis/lint/check_file_size.py --report \
	  > "$(REPORT_DIR)/file-size.txt" || true
	@$(call success,Reports written to $(REPORT_DIR))

# ------------------------------------------------------------------------------
# Docs
# ------------------------------------------------------------------------------
docs: docs-check ## [Quality] Check documentation coverage

docs-serve:  ## [Quality] Serve docs/ on :8000
	@$(PYTHON) -m http.server 8000 --directory docs

