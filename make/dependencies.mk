# ------------------------------------------------------------------------------
# Setup
# ------------------------------------------------------------------------------
setup: layout-check doctor install generate-sources configure  ## [Setup] Full developer checkout setup
	@$(call success,Development checkout ready)

bootstrap: dependencies  ## [Setup] Install project dependencies
	@$(call success,Dependencies initialized)

dependencies: install ## [Setup] Install or update all dependencies

install: submodules install-python install-node  ## [Setup] Install all dependencies

install-python:  ## [Setup] Create .venv + install locked deps (requirements.lock)
	@$(call heading,Installing python dependencies)
	@if [[ ! -x "$(VENV_PYTHON)" ]]; then \
	  $(PYTHON) -m venv .venv; \
	fi
	@"$(VENV_PYTHON)" -m pip install -r requirements.lock
	@"$(VENV_PYTHON)" -m pip install -e . --no-deps
	@$(call success,Python deps installed (.venv))

lock-python:  ## [Setup] Regenerate requirements.lock from pyproject [test] (needs uv)
	@uv pip compile pyproject.toml --extra test -o requirements.lock
	@$(call success,requirements.lock regenerated)

install-node: lobby-install  ## [Setup] npm ci for services/netplay-lobby

submodules:  ## [Setup] git submodule update --init --recursive
	@$(call heading,Initializing submodules)
	@$(GIT) submodule sync --recursive
	@$(GIT) submodule update --init --recursive
	@$(call success,Submodules initialized)

submodules-check:  ## [Setup] Verify submodule state is clean
	@$(GIT) submodule status --recursive
	@if $(GIT) submodule status --recursive | grep -Eq '^[-+U]'; then \
	  $(call failure,Submodule state not clean); exit 1; fi
	@$(call success,Submodule state clean)

# ------------------------------------------------------------------------------
# Worktree helpers — parallel sessions each get an isolated checkout under
# .claude/worktrees/ (see AGENTS.md worktree workflow). new-worktree.sh
# clones submodules from the local object stores (unpushed pins survive).
# ------------------------------------------------------------------------------
worktree-new:  ## [Setup] Create an isolated worktree (SLUG=name [BASE=branch] [BUILD=1] [CONFIG=release])
	@test -n "$(SLUG)" || { \
	  $(call failure,use: make worktree-new SLUG=<name>); exit 2; }
	@bash scripts/development/new-worktree.sh "$(SLUG)" \
	  $(if $(BASE),--base "$(BASE)",) \
	  $(if $(filter 1 true yes,$(BUILD)),--build,) \
	  $(if $(filter release,$(CONFIG)),--release,)

worktree-list:  ## [Setup] List git worktrees (main + agent checkouts)
	@$(GIT) worktree list

worktree-prune:  ## [Setup] Prune stale worktree metadata (does not delete files)
	@$(GIT) worktree prune -v
	@$(GIT) worktree list

submodules-update:  ## [Setup] Update submodules to remote tracking branches
	@$(GIT) submodule update --init --recursive --remote
	@$(call success,Submodules updated)

# ------------------------------------------------------------------------------
# Lobby service (services/netplay-lobby)
# ------------------------------------------------------------------------------
lobby-install:  ## [Netplay] npm ci for the lobby service
	@$(call heading,Installing lobby dependencies)
	@if [[ -f "$(LOBBY_DIR)/package-lock.json" ]]; then \
	  (cd "$(LOBBY_DIR)" && $(NPM) ci); \
	else \
	  (cd "$(LOBBY_DIR)" && $(NPM) install); \
	fi

lobby-test:  ## [Netplay] Lobby security (21 checks) + fuzz (400 iters)
	@$(call heading,Lobby security + fuzz)
	@$(NODE) "$(LOBBY_SECURITY)"
	@$(NODE) "$(LOBBY_FUZZ)" 400
	@$(call success,Lobby tests passed)

lobby-dev:  ## [Netplay] wrangler dev on :8799
	@cd "$(LOBBY_DIR)" && $(NPM) run dev

