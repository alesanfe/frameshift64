# ==============================================================================
# netplay.mk — netplay / lobby / websocket test flows
#
# Backing reality:
#   * CTest labels (tests/CMakeLists.txt): netplay, transport, leak, loopback,
#     curl, tls, fallback, protocol, concurrency, deterministic, fuzz, golden
#   * services/netplay-lobby npm scripts: test, test:mutation, test:workerd
#   * e2e drivers under tests/e2e/netplay (targets in test.mk)
# ==============================================================================

.PHONY: test-netplay test-lobby test-lobby-contract test-lobby-chaos \
        test-lobby-fuzz test-workerd \
        test-websocket test-websocket-leaks test-websocket-tls \
        test-websocket-loopback test-netplay-mutation test-netplay-soak \
        netplay-server netplay-matrix

test-netplay: ## [Netplay] Native netplay suite (protocol + transport + loopback)
	@$(CTEST) --test-dir "$(BUILD_DIR)" -L netplay $(CTEST_PARALLEL) \
	  $(CTEST_VERBOSE)

test-lobby: lobby-test test-workerd ## [Netplay] Lobby domain/security/fuzz + workerd

test-lobby-contract: ## [Netplay] Adapter contract tests (common suite)
	@cd "$(LOBBY_DIR)" && $(NODE) tests/contract/test-adapter-contract.mjs

test-lobby-chaos: ## [Netplay] Lobby chaos suite (kill, timeout, hostile peers)
	@cd "$(LOBBY_DIR)" && $(NODE) tests/chaos/test-lobby-chaos.js

test-lobby-fuzz: ## [Netplay] Lobby protocol fuzz suite
	@cd "$(LOBBY_DIR)" && $(NODE) tests/fuzz/test-lobby-fuzz.js

test-workerd: ## [Netplay] Worker durable-object tests under real workerd
	@cd "$(LOBBY_DIR)" && $(NPM) run test:workerd

test-websocket: ## [Netplay] All websocket transport backends (loopback, curl, fallback)
	@$(CTEST) --test-dir "$(BUILD_DIR)" -L transport $(CTEST_PARALLEL) \
	  $(CTEST_VERBOSE)

test-websocket-leaks: ## [Netplay] WebSocket backend leak tests
	@$(CTEST) --test-dir "$(BUILD_DIR)" -L "leak" $(CTEST_VERBOSE)

test-websocket-tls: ## [Netplay] wss:// TLS transport tests (curl backend)
	@$(CTEST) --test-dir "$(BUILD_DIR)" -L "tls" $(CTEST_VERBOSE)

test-websocket-loopback: ## [Netplay] In-process loopback transport tests
	@$(CTEST) --test-dir "$(BUILD_DIR)" -L "loopback" $(CTEST_VERBOSE)

test-netplay-mutation: ## [Netplay] Mutation testing of the lobby domain
	@cd "$(LOBBY_DIR)" && $(NPM) run test:mutation

test-netplay-soak: ## [Netplay] Long-running netplay soak (DURATION=seconds)
	@$(MAKE) e2e-soak E2E_SECONDS="$(or $(DURATION),1800)"

netplay-server: ## [Netplay] Run the local lobby service
	@cd "$(LOBBY_DIR)" && $(NODE) src/infrastructure/local/node-lobby.mjs

netplay-matrix: test-netplay test-lobby test-websocket test-websocket-leaks \
                test-lobby-chaos test-netplay-mutation e2e-netplay ## [Netplay] Full netplay validation matrix
