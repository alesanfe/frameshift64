# ==============================================================================
# makefile.mk — the facade validates itself
#
#   makefile-docs-check    every public target must have a `##` doc comment
#   makefile-targets-check every `$(MAKE) x` / prerequisite must resolve
#   makefile-lint          checkmake if installed, portable checks otherwise
#   makefile-dry-run       `make -n` the main workflows (syntax validation)
# ==============================================================================

.PHONY: makefile-check makefile-lint makefile-docs-check \
        makefile-targets-check makefile-dry-run

# NOTE: $(MAKEFILE_LIST) is used directly inside recipes — it only contains
# every module once expansion happens at recipe-run time (a `:=` snapshot at
# parse time would miss files included after this one).

makefile-docs-check: ## [Makefile] Reject public targets without documentation
	@$(PROJECT_PYTHON) tools/analysis/lint/check_make_help.py $(MAKEFILE_LIST)

makefile-targets-check: ## [Makefile] Reject references to undefined targets
	@$(PROJECT_PYTHON) tools/analysis/lint/check_make_targets.py \
	  --makefiles $(MAKEFILE_LIST)

makefile-lint: ## [Makefile] checkmake when available; portable checks otherwise
	@if command -v checkmake >/dev/null 2>&1; then \
	  checkmake $(MAKEFILE_LIST); \
	else \
	  printf "$(YELLOW)[WARN]$(RESET) checkmake not installed — running portable checks\n"; \
	  $(PROJECT_PYTHON) tools/analysis/lint/check_makefiles.py $(MAKEFILE_LIST); \
	fi

# NOTE: only workflows that do NOT reach makefile-check can be dry-run here.
# Under `make -n`, $(MAKE) lines still execute — dry-running a workflow that
# contains makefile-check would recurse into makefile-dry-run forever.
makefile-dry-run: ## [Makefile] Dry-run leaf workflows (catches undefined vars/rules)
	@$(MAKE) -n pre-commit >/dev/null
	@$(MAKE) -n verify-fast >/dev/null
	@$(MAKE) -n netplay-matrix >/dev/null
	@$(MAKE) -n help-all >/dev/null
	@$(call success,workflow dry-runs parsed)

makefile-check: makefile-lint makefile-docs-check makefile-targets-check \
                makefile-dry-run ## [Makefile] Validate the whole Make facade
