# ==============================================================================
# developer.mk — facades for the user-facing tools under tools/developer/
#
#   mod-package      package a C-mod source dir into a loadable mod
#   i18n-extract     rebuild the translation catalog (assets/lang/)
#   save-editor      inspect/patch the binary save file
#   shots-*          screenshot rigs (docs/marketing assets) — need a build
# ==============================================================================

TOOLS_DEV := tools/developer

.PHONY: mod-package i18n-extract save-editor \
        shots-menu shots-lang shots-netplay

mod-package: ## [Tools] Package a C-mod dir: make mod-package MOD=mods/examples/foo [OUT=dir]
	@test -n "$(MOD)" || { \
	  $(call failure,use: make mod-package MOD=<mod-source-dir>); exit 2; }
	@$(PROJECT_PYTHON) $(TOOLS_DEV)/modding/package_mod.py \
	  --mod-dir "$(MOD)" \
	  --out-dir "$(or $(OUT),$(OUT_DIR)/mods/$(notdir $(MOD)))" \
	  --repo-root "$(PROJECT_ROOT)" $(ARGS)

# --write rebuilds assets/lang/catalog.json; SYNC=<code> also merges new
# keys into assets/lang/<code>.json (English placeholders).
i18n-extract: ## [Tools] Rebuild translation catalog (SYNC=<lang-code> to sync a table)
	@$(PROJECT_PYTHON) $(TOOLS_DEV)/i18n/extract_strings.py --write \
	  $(if $(SYNC),--sync=$(SYNC),)

save-editor: ## [Tools] Save-file editor: make save-editor ARGS="dump --all" (see --help)
	@$(PROJECT_PYTHON) $(TOOLS_DEV)/save-editor/save_editor.py $(ARGS)

# Screenshot rigs — they drive the built binary with SSB64_SCREENSHOT_FRAMES.
shots-menu: guard-exe ## [Tools] Screenshot every menu section -> docs/screenshots/menu
	@$(PROJECT_PYTHON) $(TOOLS_DEV)/testing/shoot_menu_sections.py \
	  --bin "$$(dirname '$(EXE_DIR)')" $(ARGS)

shots-lang: guard-exe ## [Tools] Screenshot the game in every language
	@$(PROJECT_PYTHON) $(TOOLS_DEV)/testing/shoot_languages.py \
	  --bin "$$(dirname '$(EXE_DIR)')" $(ARGS)

shots-netplay: guard-exe ## [Tools] Screenshot a netplay room (two local clients)
	@$(PROJECT_PYTHON) $(TOOLS_DEV)/testing/shoot_netplay_room.py \
	  --bin "$$(dirname '$(EXE_DIR)')" $(ARGS)
