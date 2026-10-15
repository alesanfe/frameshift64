# ------------------------------------------------------------------------------
# UWP — Xbox Developer Mode sideload package. The entrypoint lives in
# platforms/uwp/ci/build-uwp.ps1; it configures a native Release build with
# -DBATTLESHIP_UWP=ON, then the packaging project under platforms/uwp/, and
# drops a signed MSIX into -Output. Pass UWP_PACKAGE_VERSION / UWP_SIGNING_PFX /
# UWP_SIGNING_PASSWORD to override the defaults (a disposable dev cert is used
# when no PFX is given). See platforms/uwp/ci/README.md for the container flow.
# ------------------------------------------------------------------------------
UWP_PACKAGE_VERSION  ?= 1.0.0.0
UWP_SIGNING_PFX      ?=
UWP_SIGNING_PASSWORD ?=

uwp: ## platforms/uwp/ci/build-uwp.ps1 -> signed MSIX in $(DIST_DIR)
	@$(call heading,UWP build)
	@mkdir -p "$(DIST_DIR)"
	@args="-Source \"$(CURDIR)\" -Output \"$(CURDIR)/$(DIST_DIR)\" \
		-PackageVersion \"$(UWP_PACKAGE_VERSION)\""; \
	if [[ -n "$(UWP_SIGNING_PFX)" ]]; then \
	  args="$$args -SigningPfx \"$(UWP_SIGNING_PFX)\""; \
	fi; \
	if [[ -n "$(UWP_SIGNING_PASSWORD)" ]]; then \
	  args="$$args -SigningPassword \"$(UWP_SIGNING_PASSWORD)\""; \
	fi; \
	eval $(POWERSHELL) -NoProfile -ExecutionPolicy Bypass \
	  -File "\"$(UWP_DIR)/ci/build-uwp.ps1\"" $$args
