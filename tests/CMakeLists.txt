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
# Pure wire codec (src/netplay/protocol/) — the first self-contained
# native suite (test-catalog §D.1 NET-PROTO). Compiles the codec directly;
# no game state, no ROM, no network.
smash_add_test(NAME netpacket_test TYPE unit TIMEOUT 15
    SOURCES
        unit/netplay/protocol/netpacket_test.c
        ${CMAKE_SOURCE_DIR}/src/netplay/protocol/netpacket.c
    LABELS netplay protocol deterministic)
target_include_directories(netpacket_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
