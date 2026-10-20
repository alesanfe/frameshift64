# ------------------------------------------------------------------------------
# Testing — pytest is the orchestrator; ctest for native binaries
# ------------------------------------------------------------------------------
test:  ## [Test] Run the quick suite (unit+smoke, no rom/net/gpu)
	@$(call heading,pytest suite: quick)
	@$(PYTHON) "$(TEST_RUNNER)" --suite quick $(TEST_ARGS)

test-suite:  ## [Test] Run TEST_SUITE from tests/support/config/test-suites.yml
	@$(call heading,pytest suite: $(TEST_SUITE))
	@args=""; \
	if [[ -n "$(EXE_DIR)" ]]; then args="--build-dir $$(dirname '$(EXE_DIR)')"; fi; \
	$(PYTHON) "$(TEST_RUNNER)" --suite "$(TEST_SUITE)" --repeat "$(REPEAT)" \
	  $$args $(TEST_ARGS)

test-unit:  ## [Test] pytest -m unit
	@$(PYTHON) -m pytest -m unit $(TEST_ARGS)
test-integration:  ## [Test] pytest -m integration
	@$(PYTHON) -m pytest -m integration $(TEST_ARGS)
test-contract:  ## [Test] pytest -m contract
	@$(PYTHON) -m pytest -m contract $(TEST_ARGS)

ctest: guard-configured  ## [Test] Run CTest-registered native tests
	@$(CTEST) --test-dir "$(BUILD_DIR)" $(CTEST_PARALLEL) $(CTEST_VERBOSE) \
	  $(CTEST_REGEX) $(CTEST_LABEL) $(TEST_ARGS)

test-list: guard-configured  ## [Test] List ctest registrations
	@$(CTEST) --test-dir "$(BUILD_DIR)" --show-only

test-all: test-suite lobby-test  ## [Test] pytest full + lobby suites
	@$(MAKE) --no-print-directory TEST_SUITE=full test-suite

# ------------------------------------------------------------------------------
# E2E netplay — drives the Python drivers in tests/e2e/netplay/
# (the scripts/testing/*.ps1 wrappers call the same code)
# ------------------------------------------------------------------------------
e2e:  ## [Netplay] Run E2E_SCENARIO (all|lobby|lobby-1v1|lan|compat|spectate|replay|mutation|soak|local)
	@case "$(E2E_SCENARIO)" in \
	  all)            $(MAKE) --no-print-directory e2e-local e2e-lobby ;; \
	  lobby)          $(MAKE) --no-print-directory e2e-lobby ;; \
	  lobby-multi)    $(MAKE) --no-print-directory e2e-lobby ;; \
	  lobby-1v1|lobby-2p) $(MAKE) --no-print-directory e2e-lobby-1v1 ;; \
	  lan)            $(MAKE) --no-print-directory e2e-lan ;; \
	  compat)         $(MAKE) --no-print-directory e2e-compat ;; \
	  spectate)       $(MAKE) --no-print-directory e2e-spectate ;; \
	  replay-takeover|replay) $(MAKE) --no-print-directory e2e-replay-takeover ;; \
	  mutation)       $(MAKE) --no-print-directory e2e-mutation ;; \
	  soak)           $(MAKE) --no-print-directory e2e-soak ;; \
	  local|localtest) $(MAKE) --no-print-directory e2e-local ;; \
	  *) $(call failure,Unknown E2E_SCENARIO '$(E2E_SCENARIO)'); exit 1 ;; \
	esac

e2e-local: guard-exe  ## [Netplay] Two localhost peers, trace-diffed (rollback)
	@out="$(LOG_DIR)/netplay-local"; mkdir -p "$$out"; \
	bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) "$(E2E_DIR)/localtest_test.py" --seconds "$(E2E_SECONDS)" \
	  --build-dir "$$bd" --out-dir "$$out" $(TEST_ARGS)

e2e-lobby: guard-exe  ## [Netplay] Host + N-1 clients over the local lobby (NET_PROFILE=<name>)
	@out="$(LOG_DIR)/lobby-multi"; mkdir -p "$$out"; \
	bd="$$(dirname '$(EXE_DIR)')"; \
	prof=""; [[ -n "$(NET_PROFILE)" ]] && prof="--profile $(NET_PROFILE)"; \
	$(PYTHON) "$(E2E_DIR)/lobbytest_multi_test.py" \
	  --players "$(NETPLAY_PLAYERS)" --seconds "$(E2E_SECONDS)" $$prof \
	  --build-dir "$$bd" --out-dir "$$out" $(TEST_ARGS)

e2e-netplay: e2e-lobby ## [Netplay] Run the netplay E2E scenarios

e2e-lobby-1v1: guard-exe  ## [Netplay] 1v1 lobby room (lobbytest driver)
	@out="$(LOG_DIR)/lobby-1v1"; mkdir -p "$$out"; \
	bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) "$(E2E_DIR)/lobbytest_test.py" --seconds "$(E2E_SECONDS)" \
	  --build-dir "$$bd" --out-dir "$$out" $(TEST_ARGS)

e2e-lan: guard-exe  ## [Netplay] Direct-IP LAN session, no lobby (NET_PROFILE=<name> for impairment)
	@out="$(LOG_DIR)/lan"; mkdir -p "$$out"; \
	bd="$$(dirname '$(EXE_DIR)')"; \
	prof=""; [[ -n "$(NET_PROFILE)" ]] && prof="--profile $(NET_PROFILE)"; \
	$(PYTHON) "$(E2E_DIR)/lan_test.py" \
	  --players "$(NETPLAY_PLAYERS)" --seconds "$(E2E_SECONDS)" $$prof \
	  --build-dir "$$bd" --out-dir "$$out" $(TEST_ARGS)

e2e-compat: guard-exe  ## [Netplay] Compat-handshake rejection test
	@out="$(LOG_DIR)/compat"; mkdir -p "$$out"; \
	bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) "$(E2E_DIR)/compat_test.py" \
	  --build-dir "$$bd" --out-dir "$$out" $(TEST_ARGS)

e2e-spectate: guard-exe  ## [Netplay] Players + spectator trace-diffed (NET_PROFILE=<name>)
	@out="$(LOG_DIR)/spectate"; mkdir -p "$$out"; \
	bd="$$(dirname '$(EXE_DIR)')"; \
	prof=""; [[ -n "$(NET_PROFILE)" ]] && prof="--profile $(NET_PROFILE)"; \
	$(PYTHON) "$(E2E_DIR)/spectate_test.py" \
	  --players "$(NETPLAY_PLAYERS)" --seconds "$(E2E_SECONDS)" $$prof \
	  --build-dir "$$bd" --out-dir "$$out" $(TEST_ARGS)

e2e-replay-takeover: guard-exe  ## [Netplay] Replay playback / takeover (needs REPLAY=<file>)
	@if [[ -z "$(REPLAY)" ]]; then \
	  $(call failure,Set REPLAY=<path-to.ssr>); exit 1; fi
	@out="$(LOG_DIR)/takeover"; mkdir -p "$$out"; \
	bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) "$(E2E_DIR)/replay_takeover_test.py" \
	  --replay "$(REPLAY)" --seconds "$(E2E_SECONDS)" \
	  --build-dir "$$bd" --out-dir "$$out" $(TEST_ARGS)

e2e-impaired: guard-exe  ## [Netplay] Lobby e2e under NET_PROFILE (default degraded)
	@$(MAKE) --no-print-directory NET_PROFILE="$(or $(NET_PROFILE),degraded)" e2e-lobby

test-determinism: guard-exe  ## [Test] Replay determinism: N runs, first-divergent-tick report (needs REPLAY=<file>)
	@if [[ -z "$(REPLAY)" ]]; then \
	  $(call failure,Set REPLAY=<path-to.ssr>); exit 1; fi
	@bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) tools/developer/testing/check_determinism.py \
	  --binary "$$bd/$(EXECUTABLE_NAME)$(EXE_SUFFIX)" \
	  --replay "$(REPLAY)" --runs "$(DETERMINISM_RUNS)" \
	  --output "$(TEST_OUTPUT_DIR)/determinism" $(TEST_ARGS)

e2e-mutation: guard-exe  ## [Netplay] Mutation suite (Windows: drives rollback_synctest.ps1)
	@out="$(LOG_DIR)/mutation"; mkdir -p "$$out"; \
	bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) "$(E2E_DIR)/mutation_suite_test.py" \
	  --build-dir "$$bd" --out-dir "$$out" $(TEST_ARGS)

e2e-soak: guard-exe  ## [Netplay] Rematch rounds + optional long synctest
	@out="$(LOG_DIR)/soak"; mkdir -p "$$out"; \
	bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) "$(E2E_DIR)/soak_test.py" --seconds "$(E2E_SECONDS)" \
	  --build-dir "$$bd" --out-dir "$$out" $(TEST_ARGS)

# ------------------------------------------------------------------------------
# Fuzz + coverage
# ------------------------------------------------------------------------------
PARSER ?= manifest
ITERS  ?= 300
fuzz: guard-exe  ## [Test] Run a parser fuzz driver (PARSER=manifest|json|replay|save)
	@bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) "$(PARSER_FUZZ)" --parser "$(PARSER)" --iters "$(ITERS)" \
	  --binary "$$bd/$(EXECUTABLE_NAME)$(EXE_SUFFIX)" $(TEST_ARGS)

fuzz-all: guard-exe  ## [Test] Fuzz every parser (manifest, json, replay, save)
	@for p in manifest json replay save; do \
		$(MAKE) --no-print-directory fuzz PARSER="$$p" || exit 1; \
	done

coverage-python:  ## [Test] Python coverage report (pytest-cov)
	@$(PYTHON) -m pytest -m "unit or integration" \
	  --cov --cov-branch --cov-context=test \
	  --cov-report=term-missing \
	  --cov-report="xml:$(OUT_DIR)/coverage/python/coverage.xml" \
	  --cov-report="html:$(OUT_DIR)/coverage/python/html"

coverage:  ## [Test] Full coverage: instrumented build + run + merge (clang/gcc only)
	@$(call warning,Native coverage needs a Clang/GCC toolchain build)
	@$(MAKE) --no-print-directory ENABLE_COVERAGE=1 configure build
	@LLVM_PROFILE_FILE="$(CURDIR)/$(OUT_DIR)/coverage/native/raw/%p-%m.profraw" \
	  $(MAKE) --no-print-directory TEST_SUITE=pull-request test-suite
	@bd="$$(dirname '$(EXE_DIR)')"; \
	$(PYTHON) "$(COVERAGE_TOOL)" \
	  --binary "$$bd/$(EXECUTABLE_NAME)$(EXE_SUFFIX)" \
	  --output-dir "$(OUT_DIR)/coverage/native"

# ------------------------------------------------------------------------------
# Mods
# ------------------------------------------------------------------------------
mods-test:  ## [Test] pytest -m modding
	@$(PYTHON) -m pytest -m modding $(TEST_ARGS)

