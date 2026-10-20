# ==============================================================================
# diagnostics.mk — inspect the project, its tools, and its outputs
# ==============================================================================

.PHONY: status versions environment graph-deps graph-targets logs-tail report

status: ## [Diagnostics] Worktree + submodule state
	@$(GIT) status --short
	@$(GIT) submodule status --recursive

versions: ## [Diagnostics] Tool versions (cmake/python/node/git/ninja)
	@$(CMAKE) --version | head -1
	@$(PROJECT_PYTHON) --version
	@$(GIT) --version
	@-$(NODE) --version 2>/dev/null || printf "node: not installed\n"
	@-ninja --version 2>/dev/null || printf "ninja: not installed\n"

environment: ## [Diagnostics] JSON environment report -> $(REPORT_DIR)/environment.json
	@$(PROJECT_PYTHON) tools/developer/environment_report.py \
	  --output "$(REPORT_DIR)/environment.json"

graph-deps: ## [Diagnostics] Module dependency graph -> $(REPORT_DIR)/module-deps.dot
	@mkdir -p "$(REPORT_DIR)"
	@$(PROJECT_PYTHON) tools/analysis/architecture/generate_graph.py \
	  --format dot --output "$(REPORT_DIR)/module-deps.dot"
	@$(call success,$(REPORT_DIR)/module-deps.dot — render: dot -Tsvg -o out.svg)

graph-targets: guard-configured ## [Diagnostics] CMake target graph -> $(REPORT_DIR)/cmake-targets.dot
	@mkdir -p "$(REPORT_DIR)"
	@$(CMAKE) --graphviz="$(REPORT_DIR)/cmake-targets.dot" "$(BUILD_DIR)"
	@$(call success,$(REPORT_DIR)/cmake-targets.dot)

logs-tail: ## [Diagnostics] Tail the game log (ssb64.log next to the exe)
	@log="$$(dirname '$(EXE_DIR)')/ssb64.log"; \
	if [[ ! -f "$$log" ]]; then log="$$(ls -t $(LOG_DIR)/*.log 2>/dev/null | head -1 || true)"; fi; \
	if [[ -z "$$log" || ! -f "$$log" ]]; then \
	  $(call failure,no log file found); exit 1; fi; \
	printf "tailing %s\n" "$$log"; tail -f "$$log"

report: environment quality-report abi-report artifact-manifest ## [Diagnostics] Consolidated technical report under $(REPORT_DIR)
	@$(call success,reports under $(REPORT_DIR))
