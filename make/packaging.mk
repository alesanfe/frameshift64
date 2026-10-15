# ------------------------------------------------------------------------------
# Packaging — platforms/<os>/packaging/* self-configure into build-bundle-*
# and drop artifacts in dist/. Region + scripting via env vars.
# ------------------------------------------------------------------------------
package: ## Package for the host platform -> dist/
	@case "$(HOST_PLATFORM)" in \
	  windows) $(MAKE) --no-print-directory package-windows ;; \
	  linux)   $(MAKE) --no-print-directory package-linux ;; \
	  macos)   $(MAKE) --no-print-directory package-macos ;; \
	  *) $(call failure,Unsupported packaging host: $(HOST_PLATFORM)); exit 1 ;; \
	esac

package-windows: guard-region ## platforms/windows/packaging/package-windows.ps1 -> dist/
	@$(call heading,Packaging windows ($(REGION)))
	@mkdir -p "$(DIST_DIR)"
	@SSB64_VERSION="$(REGION)" $(POWERSHELL) -NoProfile \
	  -ExecutionPolicy Bypass -File platforms/windows/packaging/package-windows.ps1 \
	  $(PACKAGE_ARGS)
	@$(call success,Windows package in $(DIST_DIR)/)

package-linux: guard-region ## platforms/linux/packaging/package-linux.sh -> dist/
	@$(call heading,Packaging linux ($(REGION)))
	@mkdir -p "$(DIST_DIR)"
	@SSB64_VERSION="$(REGION)" bash platforms/linux/packaging/package-linux.sh \
	  $(PACKAGE_ARGS)
	@$(call success,Linux package in $(DIST_DIR)/)

package-macos: guard-region ## platforms/macos/packaging/package-macos.sh -> dist/
	@$(call heading,Packaging macos ($(REGION)))
	@mkdir -p "$(DIST_DIR)"
	@SSB64_VERSION="$(REGION)" bash platforms/macos/packaging/package-macos.sh \
	  $(PACKAGE_ARGS)
	@$(call success,macOS package in $(DIST_DIR)/)

package-android: android ## Collect APK/AAB into dist/android
	@mkdir -p "$(DIST_DIR)/android"
	@find "$(ANDROID_DIR)" -type f \( -name '*.apk' -o -name '*.aab' \) \
	  -exec cp -f {} "$(DIST_DIR)/android/" \;

package-uwp: uwp ## Collect MSIX/APPX into dist/uwp
	@mkdir -p "$(DIST_DIR)/uwp"
	@find "$(UWP_DIR)" -type f \
	  \( -name '*.msix' -o -name '*.msixbundle' -o -name '*.appx' \) \
	  -exec cp -f {} "$(DIST_DIR)/uwp/" \;

