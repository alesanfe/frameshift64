# ------------------------------------------------------------------------------
# Release flow
#
#   make release VERSION=X.Y.Z   validate + build + verify locally (no publish)
#   release-tag / release-publish arrive in Entrega 3 behind CONFIRM_RELEASE=1
# ------------------------------------------------------------------------------

release-check: layout-check package security release-binary-check codegen-check  ## [Release] Validate a release candidate
	@$(call success,Release candidate validated)

release-dry-run:  ## [Release] release-check on the <host>-release preset
	@$(MAKE) --no-print-directory \
	  BUILD_TYPE=release PRESET="$(HOST_PLATFORM)-release" release-check

# -- Local validated release -------------------------------------------------
release: release-prepare release-build release-verify ## [Release] Build+verify a local release (VERSION=X.Y.Z, no publish)
	@$(call success,Release v$(VERSION) prepared under $(ARTIFACT_DIR))
	@printf "  publish is a separate, guarded step (Entrega 3)\n"

release-prepare: ## [Release] Pre-flight: semver, clean tree, branch, no dup tag
	@test -n "$(VERSION)" || { \
	  $(call failure,use: make release VERSION=X.Y.Z); exit 2; }
	@$(PROJECT_PYTHON) tools/release/prepare_release.py \
	  --version "$(VERSION)" --require-clean --require-submodules-clean

release-build: ## [Release] Build release artifacts into $(ARTIFACT_DIR)
	@$(MAKE) --no-print-directory PRESET="$(HOST_PLATFORM)-release" \
	  package-current package-checksums package-sbom artifact-manifest
	@$(MAKE) --no-print-directory release-notes VERSION="$(VERSION)"

release-verify: ## [Release] Gates a release must pass before publish
	@$(MAKE) --no-print-directory verify-fast
	@$(MAKE) --no-print-directory abi-check PRESET="$(HOST_PLATFORM)-release" \
	  || $(call warning,abi-check needs a built release binary)
	@$(MAKE) --no-print-directory package-verify

release-notes: ## [Release] Draft release notes -> $(ARTIFACT_DIR)/RELEASE_NOTES.md
	@test -n "$(VERSION)" || { \
	  $(call failure,use: make release-notes VERSION=X.Y.Z); exit 2; }
	@$(PROJECT_PYTHON) tools/release/generate_release_notes.py \
	  --version "$(VERSION)" \
	  --output "$(ARTIFACT_DIR)/RELEASE_NOTES.md"
