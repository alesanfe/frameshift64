# ==============================================================================
# reproducibility.mk — clean-input + deterministic-output gates
#
# worktree-check       the tree is a known input state (needed by release)
# source-repro-check   committed generated files match their generators
# asset-repro-check    ROM + recipes extract byte-identically twice
# package-repro-check  two package runs produce identical file hashes
# clean-room-check     pristine worktree: checkout -> configure -> build -> ctest
# reproducibility-check  the full gate
# ==============================================================================

.PHONY: worktree-check source-repro-check asset-repro-check \
        package-repro-check clean-room-check reproducibility-check reproduce

worktree-check: ## [Reproducibility] Require a clean Git worktree and submodules
	@$(PROJECT_PYTHON) tools/analysis/repro/check_worktree.py \
	  --recursive-submodules

source-repro-check: generated-check codegen-check ## [Reproducibility] Generated sources must match the tree
	@$(call success,generated sources reproducible)

asset-repro-check: guard-rom ## [Reproducibility] Extract the .o2r twice and compare hashes
	@$(PROJECT_PYTHON) tools/analysis/repro/check_o2r_reproducibility.py \
	  --rom "$(ROM_PATH)" --region "$(REGION)" \
	  --output "$(REPORT_DIR)/o2r-repro.json"

package-repro-check: ## [Reproducibility] Package the host platform twice and compare
	@$(PROJECT_PYTHON) tools/analysis/repro/check_package_reproducibility.py \
	  --platform "$(HOST_PLATFORM)" --preset "$(PRESET)" \
	  --output "$(REPORT_DIR)/package-repro.json"

clean-room-check: ## [Reproducibility] Verify HEAD in a pristine worktree (checkout->build->ctest)
	@$(PROJECT_PYTHON) tools/analysis/repro/run_clean_room.py \
	  --commit "$(or $(COMMIT),HEAD)" --preset "$(PRESET)" \
	  --report "$(REPORT_DIR)/clean-room.json" $(CLEAN_ROOM_ARGS)

reproducibility-check: worktree-check source-repro-check asset-repro-check \
                       package-repro-check ## [Reproducibility] Full reproducibility gate
	@$(call success,reproducibility gate passed)

reproduce: reproducibility-check ## [Reproducibility] Alias — verify HEAD reproduces
