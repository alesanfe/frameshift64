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

# ------------------------------------------------------------------------------
# Protected publication — pushing a v* tag fires .github/workflows/release.yml,
# which builds every platform and creates the GitHub Release. Both steps
# require CONFIRM_RELEASE=1; release-publish also needs REMOTE=<name>.
# ------------------------------------------------------------------------------
guard-confirm-release:
	@test "$(CONFIRM_RELEASE)" = "1" || { \
	  $(call failure,this mutates git state and can trigger CI publication); \
	  printf "  run intentionally, e.g. %s\n" \
	    "CONFIRM_RELEASE=1 make release-tag VERSION=X.Y.Z" >&2; exit 2; }

release-tag: guard-confirm-release ## [Release] Create annotated v<VERSION> tag (needs CONFIRM_RELEASE=1)
	@test -n "$(VERSION)" || { \
	  $(call failure,use: make release-tag VERSION=X.Y.Z); exit 2; }
	@$(GIT) rev-parse "refs/tags/v$(VERSION)" >/dev/null 2>&1 && { \
	  $(call failure,tag v$(VERSION) already exists); exit 1; } || true
	@$(GIT) tag -a "v$(VERSION)" -m "Release v$(VERSION)"
	@$(call success,created tag v$(VERSION) — publish: \
	  CONFIRM_RELEASE=1 make release-publish VERSION=$(VERSION) REMOTE=<name>)

release-publish: guard-confirm-release ## [Release] Push the tag -> fires release.yml (CONFIRM_RELEASE=1 REMOTE=…)
	@test -n "$(VERSION)" || { \
	  $(call failure,use: make release-publish VERSION=X.Y.Z); exit 2; }
	@test -n "$(REMOTE)" || { \
	  $(call failure,REMOTE required — choose deliberately); \
	  printf "  remotes: %s\n" "$$($(GIT) remote | tr '\n' ' ')" >&2; exit 2; }
	@$(GIT) rev-parse "refs/tags/v$(VERSION)" >/dev/null 2>&1 || { \
	  $(call failure,tag v$(VERSION) missing — run release-tag first); exit 1; }
	@$(call warning,pushing v$(VERSION) to $(REMOTE) starts the release pipeline)
	@$(GIT) push "$(REMOTE)" "refs/tags/v$(VERSION)"
