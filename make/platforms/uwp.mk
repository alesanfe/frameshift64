# ------------------------------------------------------------------------------
# UWP
# ------------------------------------------------------------------------------
uwp: ## platforms/uwp build entrypoint
	@$(call heading,UWP build)
	@if [[ -f "$(UWP_DIR)/build.ps1" ]]; then \
	  $(POWERSHELL) -NoProfile -File "$(UWP_DIR)/build.ps1" $(BUILD_ARGS); \
	else \
	  $(call failure,UWP build entrypoint not found); exit 1; \
	fi

