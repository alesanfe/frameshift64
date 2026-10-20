release-check: layout-check package security release-binary-check codegen-check  ## [Release] Validate a release candidate
	@$(call success,Release candidate validated)

release-dry-run:  ## [Release] release-check on the <host>-release preset
	@$(MAKE) --no-print-directory \
	  BUILD_TYPE=release PRESET="$(HOST_PLATFORM)-release" release-check

