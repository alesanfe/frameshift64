release-check: layout-check package security release-binary-check codegen-check ## Validate a release candidate
	@$(call success,Release candidate validated)

release-dry-run: ## release-check on the <host>-release preset
	@$(MAKE) --no-print-directory \
	  BUILD_TYPE=release PRESET="$(HOST_PLATFORM)-release" release-check

