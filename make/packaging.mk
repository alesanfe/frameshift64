# ------------------------------------------------------------------------------
# Packaging — platforms/<os>/packaging/* self-configure into build-bundle-*
# and drop artifacts in dist/. Region + scripting via env vars.
# ------------------------------------------------------------------------------
package:  ## [Package] Package for the host platform -> dist/
	@case "$(HOST_PLATFORM)" in \
	  windows) $(MAKE) --no-print-directory package-windows ;; \
	  linux)   $(MAKE) --no-print-directory package-linux ;; \
	  macos)   $(MAKE) --no-print-directory package-macos ;; \
	  *) $(call failure,Unsupported packaging host: $(HOST_PLATFORM)); exit 1 ;; \
	esac

package-windows: guard-region  ## [Package] packaging/windows/package-windows.ps1 -> dist/
	@$(call heading,Packaging windows ($(REGION)))
	@mkdir -p "$(DIST_DIR)"
	@SSB64_VERSION="$(REGION)" $(POWERSHELL) -NoProfile \
	  -ExecutionPolicy Bypass -File packaging/windows/package-windows.ps1 \
	  $(PACKAGE_ARGS)
	@$(call success,Windows package in $(DIST_DIR)/)

package-linux: guard-region  ## [Package] packaging/linux/package-linux.sh -> dist/
	@$(call heading,Packaging linux ($(REGION)))
	@mkdir -p "$(DIST_DIR)"
	@SSB64_VERSION="$(REGION)" bash packaging/linux/package-linux.sh \
	  $(PACKAGE_ARGS)
	@$(call success,Linux package in $(DIST_DIR)/)

package-macos: guard-region  ## [Package] packaging/macos/package-macos.sh -> dist/
	@$(call heading,Packaging macos ($(REGION)))
	@mkdir -p "$(DIST_DIR)"
	@SSB64_VERSION="$(REGION)" bash packaging/macos/package-macos.sh \
	  $(PACKAGE_ARGS)
	@$(call success,macOS package in $(DIST_DIR)/)

package-android: android  ## [Package] Collect APK/AAB into dist/android
	@mkdir -p "$(DIST_DIR)/android"
	@find "$(ANDROID_DIR)" -type f \( -name '*.apk' -o -name '*.aab' \) \
	  -exec cp -f {} "$(DIST_DIR)/android/" \;

package-uwp: uwp  ## [Package] Collect MSIX/APPX into dist/uwp
	@mkdir -p "$(DIST_DIR)/uwp"
	@find "$(UWP_DIR)" -type f \
	  \( -name '*.msix' -o -name '*.msixbundle' -o -name '*.appx' \) \
	  -exec cp -f {} "$(DIST_DIR)/uwp/" \;

# ------------------------------------------------------------------------------
# Unified artifacts — same dist/ + SHA256SUMS.txt layout the release workflow
# publishes, produced locally so local and CI outputs are interchangeable.
# ------------------------------------------------------------------------------
artifact-manifest:  ## [Package] (Re)generate dist/SHA256SUMS.txt over top-level artifacts
	@$(PYTHON) -c "\
import hashlib, pathlib, sys; \
d = pathlib.Path('$(DIST_DIR)'); \
files = sorted(p for p in d.iterdir() if p.is_file() and p.name != 'SHA256SUMS.txt'); \
(d / 'SHA256SUMS.txt').write_text(''.join( \
    hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + p.name + '\n' for p in files)); \
sys.exit(0 if files else (print('artifact-manifest: no artifacts in ' + str(d)), 1)[1])"

artifacts: package artifact-manifest  ## [Package] Package host platform + checksum manifest -> dist/
	@$(call success,Artifacts + SHA256SUMS.txt in $(DIST_DIR)/)

# ------------------------------------------------------------------------------
# Unified package layout — stage every artifact under $(PACKAGE_DIR)
# (out/packages) so `make package-all` yields a symmetric tree:
#   out/packages/<platform>/...
# The packaging scripts still emit into dist/; these targets collect.
# ------------------------------------------------------------------------------
package-current: package ## [Package] Stage host artifacts into $(PACKAGE_DIR)/$(HOST_PLATFORM)
	@mkdir -p "$(PACKAGE_DIR)/$(HOST_PLATFORM)"
	@cp -rf "$(DIST_DIR)/." "$(PACKAGE_DIR)/$(HOST_PLATFORM)/" 2>/dev/null || true
	@$(call success,$(PACKAGE_DIR)/$(HOST_PLATFORM))

package-all: package-windows package-linux package-macos package-android \
             package-uwp ## [Package] Build every platform package (heavy)
	@$(call success,all platform packages in $(DIST_DIR))

package-checksums: ## [Package] SHA256SUMS over $(PACKAGE_DIR) -> $(ARTIFACT_DIR)
	@$(PROJECT_PYTHON) tools/release/generate_checksums.py \
	  --input "$(PACKAGE_DIR)" \
	  --output "$(ARTIFACT_DIR)/SHA256SUMS"

package-sbom: sbom ## [Package] Copy the SBOM into the artifact dir
	@mkdir -p "$(ARTIFACT_DIR)"
	@cp -f "$(REPORT_DIR)/sbom/"*.json "$(ARTIFACT_DIR)/" 2>/dev/null || true

package-verify: package-checksums artifact-manifest release-binary-check ## [Package] Verify staged packages (hashes + manifest + binary scan)
	@$(call success,package verification passed)

# Structural smoke: the newest dist/ artifact must contain the exe +
# runtime payloads. Does not launch the GUI binary.
package-smoke-test: ## [Package] Smoke-test the newest dist/ artifact (structure, not launch)
	@pkg="$$(ls -t $(DIST_DIR)/*.zip $(DIST_DIR)/*.AppImage $(DIST_DIR)/*.dmg \
	  2>/dev/null | head -1 || true)"; \
	if [[ -z "$$pkg" ]]; then \
	  $(call failure,no package artifact in $(DIST_DIR)); \
	  printf "  run: make package\n" >&2; exit 1; fi; \
	$(PROJECT_PYTHON) tools/release/smoke_test_package.py \
	  --package "$$pkg" --platform "$(HOST_PLATFORM)"

package-clean: ## [Package] Remove staged packages and manifests
	@$(CMAKE) -E remove_directory "$(PACKAGE_DIR)"
	@$(CMAKE) -E remove "$(ARTIFACT_DIR)/SHA256SUMS"

