# Nintendo Switch (devkitPro / libnx) settings for ${PROJECT_NAME} plus the
# BundleSwitch SD-card deployment target. Self-guarded; safe to include
# unconditionally. Requires the main target and GenerateF3DO2R to exist —
# include this AFTER cmake/RuntimeAssets.cmake.
if (CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    # SW-08: libultraship must actually carry a Switch backend. The pinned
    # fork (JRickey/libultraship, branch ssb64) currently has no __SWITCH__
    # code — configuring anyway produces an .nro that fails at link time
    # with hundreds of undefined SDL/OpenGL symbols. Scan the vendored LUS
    # sources and fail at configure time with an actionable message instead.
    file(GLOB_RECURSE SSB64_LUS_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/libultraship/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/libultraship/src/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/libultraship/src/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/libultraship/include/*.h"
    )
    set(SSB64_LUS_HAS_SWITCH FALSE)
    foreach(lus_src IN LISTS SSB64_LUS_SOURCES)
        file(STRINGS "${lus_src}" lus_switch_hit REGEX "__SWITCH__" LIMIT_COUNT 1)
        if(lus_switch_hit)
            set(SSB64_LUS_HAS_SWITCH TRUE)
            break()
        endif()
    endforeach()
    if(NOT SSB64_LUS_HAS_SWITCH)
        message(FATAL_ERROR
            "Nintendo Switch target requested, but the vendored libultraship "
            "(${CMAKE_CURRENT_SOURCE_DIR}/libultraship) contains no __SWITCH__ "
            "backend (no libnx windowing, no Switch graphics/input/audio "
            "implementations). The .nro would not link. Either check out a "
            "libultraship revision with Switch support (fork branch with a "
            "libnx backend) or do not configure with -DCMAKE_SYSTEM_NAME=NintendoSwitch."
        )
    endif()

    target_compile_definitions(ssb64_game PRIVATE __SWITCH__ _GNU_SOURCE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE __SWITCH__ _GNU_SOURCE)

    find_program(NACPTOOL_EXECUTABLE
        NAMES nacptool
        HINTS
            "$ENV{DEVKITPRO}/tools/bin"
            "${DEVKITPRO}/tools/bin"
        REQUIRED
    )

    find_program(ELF2NRO_EXECUTABLE
        NAMES elf2nro
        HINTS
            "$ENV{DEVKITPRO}/tools/bin"
            "${DEVKITPRO}/tools/bin"
        REQUIRED
    )

    # Aggressive optimization for Tegra X1 (Cortex-A57).
    # -march is already set by the devkitPro toolchain; we only add -mtune and -O3.
    # NO -ffast-math: it re-enables -ffp-contract=fast plus finite-math-only /
    # no-signed-zeros, which diverges the deterministic simulation from every
    # other platform (netplay desync) and from N64 FP semantics.
    set(SWITCH_OPT_FLAGS -O3 -mtune=cortex-a57)
    target_compile_options(ssb64_game PRIVATE ${SWITCH_OPT_FLAGS})
    target_compile_options(${PROJECT_NAME} PRIVATE ${SWITCH_OPT_FLAGS})
    target_link_options(${PROJECT_NAME} PRIVATE -Wl,--gc-sections)

    # elf2nro icon: resolve via DEVKITPRO instead of a hardcoded /opt/devkitpro
    # so non-default installs (Windows devkitPro, custom prefixes) still work.
    set(SWITCH_NRO_ICON_ARG "")
    if(DEFINED ENV{DEVKITPRO} AND EXISTS "$ENV{DEVKITPRO}/libnx/default_icon.jpg")
        set(SWITCH_NRO_ICON_ARG "--icon=$ENV{DEVKITPRO}/libnx/default_icon.jpg")
    endif()

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${PROJECT_NAME}>
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/BattleShip.elf.keep
        COMMAND ${NACPTOOL_EXECUTABLE}
            --create
            BattleShip
            JRickey
            ${BATTLESHIP_VERSION}
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/control.nacp
        COMMAND ${ELF2NRO_EXECUTABLE}
            $<TARGET_FILE:${PROJECT_NAME}>
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/BattleShip.nro
            --alignedheader
            ${SWITCH_NRO_ICON_ARG}
            --nacp=$<TARGET_FILE_DIR:${PROJECT_NAME}>/control.nacp
        COMMENT "Packaging BattleShip.nro from BattleShip.elf"
    )

    if(TARGET TorchExternal)
        add_dependencies(${PROJECT_NAME} TorchExternal)
    endif()

    # ── SD-card bundling (extract assets on PC, copy alongside .nro) ──────
    set(SWITCH_SD_DIR "${CMAKE_BINARY_DIR}/switch_sd")
    set(SWITCH_GAME_DIR "${SWITCH_SD_DIR}/switch/BattleShip")

    file(MAKE_DIRECTORY "${SWITCH_GAME_DIR}")

    add_custom_target(BundleSwitch
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/BattleShip.nro"
            "${SWITCH_GAME_DIR}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/BattleShip.o2r"
            "${SWITCH_GAME_DIR}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/f3d.o2r"
            "${SWITCH_GAME_DIR}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/assets/configuration/controllers/gamecontrollerdb.txt"
            "${SWITCH_GAME_DIR}/gamecontrollerdb.txt"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${SWITCH_GAME_DIR}/shader_cache"
        COMMENT "Bundling Switch deployment files into ${SWITCH_GAME_DIR}"
    )
    # BattleShip.o2r is user-extracted (pre-built on a PC); f3d.o2r is
    # generated by the host-side GenerateF3DO2R target.
    add_dependencies(BundleSwitch GenerateF3DO2R)
endif()
