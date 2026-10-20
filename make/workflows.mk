# ==============================================================================
# workflows.mk — memorable high-level flows composed from existing targets
#
#   pre-commit  fast local gate before committing
#   pre-push    fuller local gate before pushing
#   verify      PR-equivalent validation (defined in quality.mk)
#   verify-all  verify + security + ABI + reproducibility (exhaustive)
# ==============================================================================

.PHONY: dev dev-clean dev-run pre-commit pre-push ci-local \
        all default doctor-full verify-all

dev: setup configure dev-build ## [Workflow] Prepare and compile the dev environment
	@$(call success,Development environment ready — run: make run)

dev-clean: clean-build configure dev-build ## [Workflow] Rebuild the dev environment from clean

dev-run: dev-build run ## [Workflow] Build and launch the game

pre-commit: format-check quick-check test-unit ## [Workflow] Fast validation before committing

pre-push: check test static-analysis ## [Workflow] Full validation before pushing

ci-local: verify ## [Workflow] Run the main CI validation locally

all: build ## [Workflow] Alias for build
	@:

default: help ## [Workflow] Show help
	@:

doctor-full: doctor env-check submodules-check worktree-check ## [Workflow] Full environment diagnostic

verify-all: verify security abi-check worktree-check ## [Workflow] Exhaustive verification (verify + security + ABI + worktree)
