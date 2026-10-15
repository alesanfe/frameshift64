# Build-time static analysis for first-party targets.
# SSB64_ENABLE_CLANG_TIDY=ON wires clang-tidy into compile steps for
# port code (smash_* modules + exe) — NOT external code. Slow; use on
# dedicated lint builds, not the daily driver.

if(SSB64_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(CLANG_TIDY_EXE)
        set(SSB64_CLANG_TIDY_CMD "${CLANG_TIDY_EXE}"
            "-checks=bugprone-*,performance-*,portability-*")
        message(STATUS "clang-tidy enabled for first-party targets: ${CLANG_TIDY_EXE}")
    else()
        message(WARNING "SSB64_ENABLE_CLANG_TIDY=ON but clang-tidy not found — skipped")
    endif()
endif()

function(ssb64_apply_static_analysis _tgt)
    if(SSB64_ENABLE_CLANG_TIDY AND SSB64_CLANG_TIDY_CMD)
        set_target_properties(${_tgt} PROPERTIES
            CXX_CLANG_TIDY "${SSB64_CLANG_TIDY_CMD}"
            C_CLANG_TIDY "${SSB64_CLANG_TIDY_CMD}")
    endif()
endfunction()
