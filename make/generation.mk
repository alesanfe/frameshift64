# ------------------------------------------------------------------------------
# Codegen / assets
# ------------------------------------------------------------------------------
codegen: guard-configured  ## [Build] Run the CMake-managed generators (reloc, asset headers)
	@$(call heading,Running codegen targets)
	@$(CMAKE) --build --preset "$(PRESET)" --target PrepareBuildInputs \
	  --target "$(ASSET_HEADERS_TARGET)" $(BUILD_PARALLEL)
	@$(call success,Codegen completed)

generate-yamls:  ## [Build] Regenerate extraction recipes + reloc yaml tables
	@$(PYTHON) tools/code-generation/generate_yamls.py

codegen-check:  ## [Build] Fail if codegen changes the working tree
	@before="$$($(GIT) status --porcelain)"; \
	$(MAKE) --no-print-directory codegen; \
	after="$$($(GIT) status --porcelain)"; \
	if [[ "$$before" != "$$after" ]]; then \
	  $(call failure,Codegen changed the working tree); \
	  git status --short; exit 1; fi
	@$(call success,Generated sources are current)

assets: configure guard-rom  ## [Build] Extract runtime assets from the ROM (Torch)
	@$(call heading,Extracting assets ($(REGION)))
	@$(CMAKE) --build --preset "$(PRESET)" --target "$(ASSETS_TARGET)" \
	  $(BUILD_PARALLEL) $(BUILD_VERBOSE)
	@$(call success,Assets extracted)

synth-verify: configure  ## [Build] Build the synthetic-reloc verification target
	@$(CMAKE) --build --preset "$(PRESET)" --target "$(SYNTH_VERIFY_TARGET)"

