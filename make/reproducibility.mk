# ==============================================================================
# reproducibility.mk — clean-input gates
#
# Entrega 1 ships worktree-check (needed by doctor-full / verify-all).
# asset/package/clean-room checks land with their tools in Entrega 2.
# ==============================================================================

.PHONY: worktree-check source-repro-check

worktree-check: ## [Reproducibility] Require a clean Git worktree and submodules
	@$(PROJECT_PYTHON) tools/analysis/repro/check_worktree.py \
	  --recursive-submodules

source-repro-check: generated-check codegen-check ## [Reproducibility] Generated sources must match the tree
	@$(call success,generated sources reproducible)
