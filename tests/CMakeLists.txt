# Test harness entry point. Enabled with -DSSB64_BUILD_TESTING=ON.
#
# Levels map to CTest labels, not just paths — run subsets with:
#   ctest --test-dir <build> --label-regex unit
#   ctest --test-dir <build> --label-regex "unit.*netplay"
#   ctest --test-dir <build> --label-exclude "slow|requires-rom|requires-network"
# Suite presets that translate to label sets live in config/test-suites.yml.

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
    LABELS netplay protocol deterministic)
target_include_directories(netpacket_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

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
