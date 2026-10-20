# ------------------------------------------------------------------------------
# Security / supply chain
# ------------------------------------------------------------------------------
security: semgrep dependency-audit release-binary-check sbom ## [Security] Run the main security controls
	@$(call success,Security checks passed)

semgrep:  ## [Security] Semgrep rules in config/analysis/semgrep
	@if ! command -v semgrep >/dev/null 2>&1; then \
	  $(call failure,semgrep required); exit 1; fi
	@mkdir -p "$(REPORT_DIR)/security"
	@semgrep --config config/analysis/semgrep --error \
	  --json-output "$(REPORT_DIR)/security/semgrep.json" .

dependency-audit:  ## [Security] npm audit (lobby) + submodule pins
	@if [[ -f "$(LOBBY_DIR)/package-lock.json" ]]; then \
	  (cd "$(LOBBY_DIR)" && $(NPM) audit); \
	else \
	  $(call warning,no lobby lockfile); \
	fi

sbom:  ## [Security] CycloneDX SBOM -> out/reports/sbom/
	@mkdir -p "$(REPORT_DIR)/sbom"
	@$(PYTHON) "$(SBOM_SCRIPT)" --out "$(REPORT_DIR)/sbom/$(PROJECT_NAME).cdx.json"

release-binary-check:  ## [Security] Scan a built binary for test-hook strings
	@bd="$$(dirname '$(EXE_DIR)')"; \
	if [[ -n "$$bd" ]]; then \
	  $(PYTHON) "$(RELEASE_BIN_SCRIPT)" "$$bd/$(EXECUTABLE_NAME)$(EXE_SUFFIX)"; \
	else \
	  $(call warning,no binary — skipping release-binary check); \
	fi

