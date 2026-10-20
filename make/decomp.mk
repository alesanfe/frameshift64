# ==============================================================================
# decomp.mk — facade over components/decomp's own Makefile
#
# The decomp submodule is authoritative for matching-ROM work (init, extract,
# validate, report, format). These targets delegate so users don't need to
# `cd components/decomp`. When the submodule is not fully checked out the
# targets fail with an actionable message instead of "No rule to make target".
# ==============================================================================

DECOMP_DIR := components/decomp
DECOMP_MK  := $(DECOMP_DIR)/Makefile

.PHONY: decomp-init decomp-extract decomp-validate decomp-report \
        decomp-format decomp-clean decomp-status guard-decomp

guard-decomp:
	@if [[ ! -f "$(DECOMP_MK)" ]]; then \
	  $(call failure,decomp submodule has no Makefile ($(DECOMP_MK) missing)); \
	  printf "  the checkout may be sparse — run: git -C %s submodule update --init\n" \
	    "$(DECOMP_DIR)" >&2; \
	  exit 1; fi

decomp-init: guard-decomp ## [Decomp] Full extract + build of the decomp (fresh checkout)
	@$(MAKE) -C "$(DECOMP_DIR)" init VERSION="$(REGION)"

decomp-extract: guard-decomp guard-rom ## [Decomp] Re-extract assembly/assets from the baserom
	@$(MAKE) -C "$(DECOMP_DIR)" extract VERSION="$(REGION)"

decomp-validate: guard-decomp ## [Decomp] Verify the built ROM matches expected
	@$(MAKE) -C "$(DECOMP_DIR)" validate VERSION="$(REGION)"

decomp-report: guard-decomp ## [Decomp] Generate the decomp.dev progress report
	@$(MAKE) -C "$(DECOMP_DIR)" report VERSION="$(REGION)"

decomp-format: guard-decomp ## [Decomp] clang-format the decomp sources
	@$(MAKE) -C "$(DECOMP_DIR)" format

decomp-clean: guard-decomp ## [Decomp] Clean the active version's build artifacts
	@$(MAKE) -C "$(DECOMP_DIR)" clean VERSION="$(REGION)"

decomp-status: ## [Decomp] Show the submodule's SHA and local state
	@$(GIT) -C "$(DECOMP_DIR)" rev-parse --short HEAD 2>/dev/null \
	  || { $(call failure,decomp submodule not initialized); exit 1; }
	@$(GIT) -C "$(DECOMP_DIR)" status --short
