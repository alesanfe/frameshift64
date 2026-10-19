# Test harness entry point. Enabled with -DSSB64_BUILD_TESTING=ON.
#
# Levels map to CTest labels, not just paths — run subsets with:
#   ctest --test-dir <build> --label-regex unit
#   ctest --test-dir <build> --label-regex "unit.*netplay"
#   ctest --test-dir <build> --label-exclude "slow|requires-rom|requires-network"
# Suite presets that translate to label sets live in support/config/test-suites.yml.

enable_testing()

# smash_add_test(<name>
#   TYPE unit|integration|contract|e2e|smoke|fuzz|performance|security|regression
#   SOURCES <files...>
#   [LIBRARIES <targets...>]
#   [LABELS <extra labels...>]
#   [TIMEOUT <seconds>]
#   [REQUIRES_ROM] [REQUIRES_NETWORK] [REQUIRES_GPU] [SERIAL])
function(smash_add_test)
    set(options REQUIRES_ROM REQUIRES_NETWORK REQUIRES_GPU SERIAL)
    set(oneValueArgs NAME TYPE TIMEOUT)
    set(multiValueArgs SOURCES LIBRARIES LABELS)
    cmake_parse_arguments(TEST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if (NOT TEST_NAME OR NOT TEST_SOURCES OR NOT TEST_TYPE)
        message(FATAL_ERROR "smash_add_test needs NAME, TYPE and SOURCES")
    endif()
    if (NOT TEST_TIMEOUT)
        set(TEST_TIMEOUT 30)
    endif()

    add_executable(${TEST_NAME} ${TEST_SOURCES})
    target_link_libraries(${TEST_NAME} PRIVATE ${TEST_LIBRARIES})
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

    set(_labels "${TEST_TYPE}")
    if (TEST_REQUIRES_ROM)
        string(APPEND _labels ";requires-rom")
    endif()
    if (TEST_REQUIRES_NETWORK)
        string(APPEND _labels ";requires-network")
    endif()
    if (TEST_REQUIRES_GPU)
        string(APPEND _labels ";requires-gpu")
    endif()
    if (TEST_LABELS)
        string(APPEND _labels ";${TEST_LABELS}")
    endif()
    set_tests_properties(${TEST_NAME} PROPERTIES
        LABELS "${_labels}"
        TIMEOUT "${TEST_TIMEOUT}"
        RUN_SERIAL ${TEST_SERIAL})
endfunction()

# smash_test_support — shared helpers for native suites (tests/support/):
#   smash_test/check.h  counted + fatal assertions with a final report
#   smash_test/env.h    portable setenv/unsetenv for SSB64_* hook tests
add_library(smash_test_support STATIC support/src/env.c)
target_include_directories(smash_test_support PUBLIC support/include)

# Test registrations land here per domain. Example:
#   smash_add_test(NAME netplay_protocol_test TYPE unit TIMEOUT 15
#       SOURCES unit/netplay/protocol/message_codec_test.cpp
#       LIBRARIES smash_test_support
#       LABELS netplay protocol deterministic)

# ── unit/netplay ───────────────────────────────────────────────────────
# Pure wire codec (src/networking/netplay/protocol/) — the first self-contained
# native suite (test-catalog §D.1 NET-PROTO). Compiles the codec directly;
# no game state, no ROM, no network.
smash_add_test(NAME netpacket_test TYPE unit TIMEOUT 15
    SOURCES
        unit/netplay/protocol/netpacket_test.c
        ${CMAKE_SOURCE_DIR}/src/networking/netplay/protocol/netpacket.c
    LIBRARIES smash_test_support
    LABELS netplay protocol deterministic)
target_include_directories(netpacket_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Lobby wire-JSON helpers — self-contained TU, compiles standalone like
# netpacket.c above (no game state, no network).
smash_add_test(NAME lobby_json_test TYPE unit TIMEOUT 15
    SOURCES
        unit/netplay/session/lobby_json_test.cpp
        ${CMAKE_SOURCE_DIR}/src/networking/netplay/session/lobby_json.cpp
    LIBRARIES smash_test_support nlohmann_json::nlohmann_json
    LABELS netplay protocol deterministic)
target_include_directories(lobby_json_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Persistence stores — SSB64_SAVE_PATH redirects them into a per-process
# temp dir, so load/save runs fully offline. Each binary runs twice:
# once against a crafted fixture (default argv) and once against a
# corrupt file (argv "corrupt") since the lazy load is one-shot per
# process. port_save_get_dir is the test seam — each test TU provides a
# double honoring SSB64_SAVE_PATH.
foreach(store netstats netfriends)
    smash_add_test(NAME ${store}_test TYPE unit TIMEOUT 15
        SOURCES
            unit/netplay/domain/${store}_test.cpp
            ${CMAKE_SOURCE_DIR}/src/networking/netplay/domain/${store}.cpp
        LIBRARIES smash_test_support nlohmann_json::nlohmann_json
        LABELS netplay persistence deterministic)
    target_include_directories(${store}_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src/networking/netplay/domain
        ${CMAKE_SOURCE_DIR}/src/observability
        ${CMAKE_SOURCE_DIR}/src/persistence
        ${CMAKE_SOURCE_DIR}/src/shared)
    add_test(NAME ${store}_corrupt_test COMMAND ${store}_test corrupt)
    set_tests_properties(${store}_corrupt_test PROPERTIES
        LABELS "unit;netplay;persistence;deterministic" TIMEOUT 15)
endforeach()

# ── unit/presentation ──────────────────────────────────────────────────
# Static structural check over the PortMenu widget registry (§52/§53):
# no empty sidebars, no ImGui label collisions, every interactive widget
# self-documents via tooltip, buttons always wire a callback, widgets
# never bind to undeclared columns. Pure Python — no ROM/GPU needed.
add_test(NAME menu_structure_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/unit/presentation/menu_structure_test.py)
set_tests_properties(menu_structure_test PROPERTIES
    LABELS "unit;presentation;usability;deterministic" TIMEOUT 30)

# ── unit/relocation ────────────────────────────────────────────────────
# Golden structural invariants of the committed synthetic-reloc spec table
# (generated/src/<ver>/ssb64_reloc_rebuild.<ver>.cpp): file_id ordering,
# slice containment, intern/extern slot bounds — the exact contract the
# deblob runtime trusts (docs/deblob.md). Pure Python, no ROM needed.
add_test(NAME reloc_spec_golden_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/unit/relocation/reloc_spec_golden_test.py)
set_tests_properties(reloc_spec_golden_test PROPERTIES
    LABELS "unit;relocation;deterministic" TIMEOUT 30)

# ── e2e/golden ─────────────────────────────────────────────────────────
# Golden gameplay verification: replays the committed .ssr input tapes
# (tests/support/fixtures/replays/, produced by
# tools/developer/testing/make_replay.py) and diffs the per-tick state
# trace against the committed goldens (tests/support/golden/*.trace).
# Needs BattleShip.o2r next to the binary — the driver exits 77 when the
# extracted assets are absent so ROM-less environments SKIP, not FAIL.
add_test(NAME golden_gameplay_test
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/e2e/golden/golden_gameplay_test.py
            --build-dir $<TARGET_FILE_DIR:ssb64>)
set_tests_properties(golden_gameplay_test PROPERTIES
    LABELS "e2e;golden;deterministic;requires-rom"
    SKIP_RETURN_CODE 77
    TIMEOUT 600)

# ── tests/fuzz (libFuzzer) ─────────────────────────────────────────────
# Coverage-guided parser fuzzing (TD-010). Opt-in: requires clang's
# -fsanitize=fuzzer, so it's gated on its own option rather than riding
# SSB64_BUILD_TESTING. The mutation-driven in-binary driver
# (tools/analysis/fuzz/run_parser_fuzz.py + SSB64_FUZZ_PARSER hook) covers the
# file-loading parsers that need real game state; these targets cover
# the pure codecs.
#
#   cmake -DSSB64_BUILD_FUZZERS=ON -DCMAKE_C_COMPILER=clang ...
option(SSB64_BUILD_FUZZERS "Build libFuzzer parser targets (tests/fuzz/)" OFF)
if (SSB64_BUILD_FUZZERS)
    if (NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "SSB64_BUILD_FUZZERS needs clang (-fsanitize=fuzzer)")
    endif()

    set(SSB64_FUZZ_FLAGS "-fsanitize=fuzzer,address,undefined" "-g")
    set(SSB64_FUZZ_CORPUS_DIR "${CMAKE_SOURCE_DIR}/tests/fuzz/corpora")

    # SSNP wire codec — self-contained C, no game state.
    add_executable(net_packet_fuzzer
        fuzz/net_packet_fuzzer.c
        ${CMAKE_SOURCE_DIR}/src/networking/netplay/protocol/netpacket.c)
    target_include_directories(net_packet_fuzzer PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(net_packet_fuzzer PRIVATE ${SSB64_FUZZ_FLAGS})
    target_link_options(net_packet_fuzzer PRIVATE ${SSB64_FUZZ_FLAGS})
    add_test(NAME net_packet_fuzzer
        COMMAND net_packet_fuzzer ${SSB64_FUZZ_CORPUS_DIR}/packet_parser -runs=10000)
    set_tests_properties(net_packet_fuzzer PROPERTIES LABELS "fuzz" TIMEOUT 120)

    # Mod manifest parser — pure static method on Ship::Archive; linking
    # the whole libultraship target pulls Archive.cpp's real code path.
    add_executable(mod_manifest_fuzzer fuzz/mod_manifest_fuzzer.cpp)
    target_link_libraries(mod_manifest_fuzzer PRIVATE libultraship)
    target_compile_options(mod_manifest_fuzzer PRIVATE ${SSB64_FUZZ_FLAGS})
    target_link_options(mod_manifest_fuzzer PRIVATE ${SSB64_FUZZ_FLAGS})
    add_test(NAME mod_manifest_fuzzer
        COMMAND mod_manifest_fuzzer ${SSB64_FUZZ_CORPUS_DIR}/manifest -runs=10000)
    set_tests_properties(mod_manifest_fuzzer PROPERTIES LABELS "fuzz" TIMEOUT 120)

    # Shared JSON parser (config file, manifests, lobby messages).
    add_executable(config_json_fuzzer fuzz/config_json_fuzzer.cpp)
    target_link_libraries(config_json_fuzzer PRIVATE libultraship)
    target_compile_options(config_json_fuzzer PRIVATE ${SSB64_FUZZ_FLAGS})
    target_link_options(config_json_fuzzer PRIVATE ${SSB64_FUZZ_FLAGS})
    add_test(NAME config_json_fuzzer
        COMMAND config_json_fuzzer ${SSB64_FUZZ_CORPUS_DIR}/json -runs=10000)
    set_tests_properties(config_json_fuzzer PROPERTIES LABELS "fuzz" TIMEOUT 120)
endif()
