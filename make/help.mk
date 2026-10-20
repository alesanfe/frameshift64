# ==============================================================================
# help.mk — categorized self-documentation (requires tools/developer/make_help.py)
# ==============================================================================

MAKE_HELP := $(PROJECT_PYTHON) tools/developer/make_help.py

.PHONY: help help-all help-json help-build help-test help-netplay help-quality \
        help-security help-package help-release help-workflow

help: ## [Help] Show the common workflow commands
	@$(MAKE_HELP) --mode common $(MAKEFILE_LIST)

help-all: ## [Help] Show every documented target by category
	@$(MAKE_HELP) --mode all $(MAKEFILE_LIST)

help-json: ## [Help] Export the full target catalog as JSON
	@$(MAKE_HELP) --mode all --format json \
	  --output "$(REPORT_DIR)/make-targets.json" $(MAKEFILE_LIST)

help-build: ## [Help] Build and code-generation targets
	@$(MAKE_HELP) --mode all --category Build $(MAKEFILE_LIST)

help-test: ## [Help] Test targets
	@$(MAKE_HELP) --mode all --category Test $(MAKEFILE_LIST)

help-netplay: ## [Help] Netplay and lobby targets
	@$(MAKE_HELP) --mode all --category Netplay $(MAKEFILE_LIST)

help-quality: ## [Help] Lint, complexity and architecture targets
	@$(MAKE_HELP) --mode all --category Quality $(MAKEFILE_LIST)

help-security: ## [Help] Security targets
	@$(MAKE_HELP) --mode all --category Security $(MAKEFILE_LIST)

help-package: ## [Help] Packaging targets
	@$(MAKE_HELP) --mode all --category Package $(MAKEFILE_LIST)

help-release: ## [Help] Release targets
	@$(MAKE_HELP) --mode all --category Release $(MAKEFILE_LIST)

help-workflow: ## [Help] High-level workflow targets (pre-commit, verify, ...)
	@$(MAKE_HELP) --mode all --category Workflow $(MAKEFILE_LIST)
