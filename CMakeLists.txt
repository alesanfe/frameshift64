cmake_minimum_required(VERSION 3.24.0 FATAL_ERROR)

project(ssb64 LANGUAGES C CXX ASM)

option(BATTLESHIP_UWP "Build the native DLL used by the Xbox/UWP wrapper" OFF)

if(BATTLESHIP_UWP)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        message(FATAL_ERROR "BATTLESHIP_UWP requires a Windows x64 host build")
    endif()
    set(NON_PORTABLE ON CACHE BOOL "Store writable data in platform app storage" FORCE)
    set(DISABLE_SCRIPTING ON CACHE BOOL "UWP cannot provide executable-memory scripting" FORCE)
    set(LUS_ENABLE_POSTPROCESS_TRANSPILER OFF CACHE BOOL "UWP uses packaged shaders" FORCE)
    add_compile_definitions(_UWP=1 BATTLESHIP_UWP=1)
endif()

# Android needs ASM for the aarch64 coroutine context-switch
# (src/platform/coroutine/coroutine_aarch64.S). Enable it before any add_library/add_executable
# call sees an .S source. ASM is preprocessed by clang on the way through.
if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    enable_language(ASM)
endif()

# Mod system master switch (TinyCC source scripting + funchook hook backend +
# src/game/modding). DISABLE_SCRIPTING gates it everywhere — funchook FetchContent,
# src/game/modding sources, the no-inline linkage tweaks, the post-build .tcc/ staging,
# and the in-engine call sites (#ifndef DISABLE_SCRIPTING). Forced ON for
# Android: funchook/TinyCC have no Android target and the loader is desktop-only.
# Set BEFORE add_subdirectory(libultraship) so the engine half sees it too.
if (CMAKE_SYSTEM_NAME STREQUAL "Android" OR CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    # funchook/TinyCC have no Switch target either — the mod loader is
    # desktop-only on both platforms.
    set(DISABLE_SCRIPTING ON CACHE BOOL "Disable the mod system (scripting + hooks)" FORCE)
endif()

if (DISABLE_SCRIPTING)
    # Define for every superproject target (ssb64_game + the exe + port glue)
    # so the #ifndef DISABLE_SCRIPTING guards in src/*.cpp engage. libultraship
    # additionally defines it PUBLIC from its own CMake when the var is set.
    add_compile_definitions(DISABLE_SCRIPTING)
endif()

include(ExternalProject)
include(FetchContent) # pull discord-rpc from GitHub (desktop only)

# Version string for the built-in updater. String-compared exactly against
# the name returned by GitHub's /tags API, so it must match the release tag
# character-for-character (suffix-and-all). Resolution order:
#   1. -DBATTLESHIP_VERSION=... passed by the caller (release CI sets this
#      to $GITHUB_REF_NAME).
#   2. `git describe --tags --abbrev=0` for local builds at a tagged commit.
#   3. "dev" fallback for source tarballs / shallow CI checkouts — those
#      builds always show "update available", which is the right signal.
# Smash Classic: the VERSION file is the release version (vX.Y.Z, matching the
# GitHub release tag). It comes before `git describe`, which would pick up
# upstream BattleShip's tags and make the updater compare against those.
if(NOT BATTLESHIP_VERSION AND EXISTS "${CMAKE_SOURCE_DIR}/VERSION")
    file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" BATTLESHIP_VERSION LIMIT_COUNT 1)
    string(STRIP "${BATTLESHIP_VERSION}" BATTLESHIP_VERSION)
endif()
if(NOT BATTLESHIP_VERSION)
    find_package(Git QUIET)
    if(Git_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE BATTLESHIP_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
endif()
if(NOT BATTLESHIP_VERSION)
    set(BATTLESHIP_VERSION "dev")
endif()
message(STATUS "BattleShip updater version: ${BATTLESHIP_VERSION}")

# Build identity for the netplay compatibility handshake: the git SHA of the
# exact tree compiled catches same-version builds that differ (dev builds,
# local patches) where the release tag alone would silently desync.
find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=10 HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE BATTLESHIP_BUILD_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(NOT BATTLESHIP_BUILD_SHA)
    set(BATTLESHIP_BUILD_SHA "dev")
endif()
message(STATUS "BattleShip build sha: ${BATTLESHIP_BUILD_SHA}")

# Pulled-in BEFORE the libultraship subdirectory so its
# find_package(tinyxml2) sees our pkg-config-fallback shim. Required
# for Ubuntu 22.04 jammy, where libtinyxml2-dev ships no CMake config.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -fobjc-arc")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build." FORCE)
endif()

# Disable MSVC's vectorized STL helpers (added in 14.40 / VS 17.10).
# Some VS installs ship the headers without the matching vcruntime
# symbols, which produces unresolved external errors at link
# (__std_find_first_of_trivial_pos_1 etc). The fallback non-vectorized
# variants are functionally equivalent and always work.
if(MSVC)
    add_compile_definitions(_USE_STD_VECTOR_ALGORITHMS=0)
endif()

################################################################################
# Options
################################################################################

option(NON_MATCHING "Build without matching decomp constraints" ON)
option(NON_EQUIVALENT "Build without equivalent decomp constraints" ON)
option(USE_AUTO_VCPKG "Use automated vcpkg for dependency management" ON)
option(SSB64_STAGE_CYCLE_DEMO "Boot directly into attract-demo stage cycle for rendering regression testing" OFF)
option(SSB64_ENABLE_COVERAGE "Instrument the build for LLVM source-based coverage" OFF)
option(SSB64_ENABLE_HARDENING "Production hardening (CFI/stack-protector/RELRO)" ON)
option(SSB64_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" OFF)
option(SSB64_ENABLE_ASAN "AddressSanitizer (dev only — never ship)" OFF)
option(SSB64_ENABLE_UBSAN "UndefinedBehaviorSanitizer (dev only, GCC/Clang)" OFF)
option(SSB64_ENABLE_CLANG_TIDY "clang-tidy on first-party targets" OFF)

# LLVM source-based coverage (clang/gcc only — MSVC has no
# -fcoverage-mapping equivalent; use a clang-cl or Unix toolchain build
# for coverage numbers). Run the suite with
# LLVM_PROFILE_FILE=<dir>/%p-%m.profraw, then merge with
# tools/developer/testing/coverage.py.
if(SSB64_ENABLE_COVERAGE)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU" OR
       NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "SSB64_ENABLE_COVERAGE requires a Clang or GCC toolchain")
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
        add_link_options(-fprofile-instr-generate -fcoverage-mapping)
    else()
        add_compile_options(--coverage)
        add_link_options(--coverage)
    endif()
endif()

# ROM version to build. The decomp game code is region-conditionally
# compiled (#if REGION_US / REGION_JP), so US and JP are separate builds —
# use a dedicated build dir per version. The user still supplies their own
# ROM (baserom.<version>.{z64,n64,v64}); no assets are shipped.
set(SSB64_VERSION "us" CACHE STRING "ROM version to build: us or jp")
set_property(CACHE SSB64_VERSION PROPERTY STRINGS us jp)
if(NOT SSB64_VERSION MATCHES "^(us|jp)$")
    message(FATAL_ERROR "SSB64_VERSION must be 'us' or 'jp' (got '${SSB64_VERSION}')")
endif()
# Android ships US-only. The Android build pipeline (Gradle asset staging,
# touch overlay, JNI bridges) has only ever been tested against the US
# decomp; bifurcating the APK / app-data dir per region the way desktop
# does (PR #191) would also need a JP-specific BootActivity/AssetExtractor.
# Until someone signs up for the JP Android effort, force US.
if (CMAKE_SYSTEM_NAME STREQUAL "Android" AND NOT SSB64_VERSION STREQUAL "us")
    message(FATAL_ERROR
        "Android build is US-only (got SSB64_VERSION=${SSB64_VERSION}). "
        "Drop -DSSB64_VERSION=jp from the Gradle externalNativeBuild args.")
endif()
if (BATTLESHIP_UWP AND NOT SSB64_VERSION STREQUAL "us")
    message(FATAL_ERROR "The UWP package supports the US ROM only")
endif()
string(TOUPPER "${SSB64_VERSION}" SSB64_VERSION_UC)
message(STATUS "SSB64 ROM version: ${SSB64_VERSION}")

find_package(Python3 COMPONENTS Interpreter REQUIRED)

################################################################################
# libultraship configuration (set before add_subdirectory)
################################################################################

# O2R (ZIP) only, no MPQ/StormLib
set(INCLUDE_MPQ_SUPPORT OFF CACHE BOOL "" FORCE)
set(GBI_UCODE "F3DEX_GBI_2" CACHE STRING "" FORCE)

# Android needs GLES3 instead of desktop GL + GLEW. Pin USE_OPENGLES ON
# before the libultraship subdirectory so its src/CMakeLists.txt picks the
# GLESv3 branch and the ENABLE_OPENGL/USE_OPENGLES compile defs are set.
if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(USE_OPENGLES ON CACHE BOOL "" FORCE)
endif()

# Build Torch as the original standalone sidecar executable on desktop.
# Android ships Torch as a SHARED library (libtorch_runner.so) inside the
# APK so the first-run UI can extract the user's ROM on-device via JNI —
# see the torch_runner target at the bottom of this file.
if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(USE_STANDALONE OFF CACHE BOOL "" FORCE)
else()
    set(USE_STANDALONE ON CACHE BOOL "" FORCE)
endif()
set(BUILD_STORMLIB OFF CACHE BOOL "" FORCE)
set(BUILD_SM64 OFF CACHE BOOL "" FORCE)
set(BUILD_MK64 OFF CACHE BOOL "" FORCE)
set(BUILD_SF64 OFF CACHE BOOL "" FORCE)
set(BUILD_PM64 OFF CACHE BOOL "" FORCE)
set(BUILD_FZERO OFF CACHE BOOL "" FORCE)
set(BUILD_MARIO_ARTIST OFF CACHE BOOL "" FORCE)
set(BUILD_NAUDIO ON CACHE BOOL "" FORCE)
set(BUILD_SSB64 ON CACHE BOOL "" FORCE)

if(BATTLESHIP_UWP)
    FetchContent_Declare(UwpLibs
        GIT_REPOSITORY https://github.com/worleydl/uwp-dep.git
        GIT_TAG 0ec87d2406b47743772e2787654275a6f6c0e284
    )
    FetchContent_MakeAvailable(UwpLibs)
endif()

################################################################################
# Subdirectories: libultraship
################################################################################

add_subdirectory(components/libultraship ${CMAKE_BINARY_DIR}/libultraship)

# LUS's embedded-repo helpers (cmake/DefaultCXX.cmake) aim the lib at
# ${CMAKE_SOURCE_DIR}/x64/<config>/ — correct for a standalone LUS checkout,
# but it leaks a build-artifact dir into our source root and splits the PDB
# from the .lib (LNK4099). Redirect everything back under the build tree.
if (CMAKE_CONFIGURATION_TYPES AND TARGET libultraship)
    foreach(_lus_cfg IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${_lus_cfg}" _lus_cfg_upper)
        set_target_properties(libultraship PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY_${_lus_cfg_upper} "${CMAKE_BINARY_DIR}/libultraship/${_lus_cfg}"
            LIBRARY_OUTPUT_DIRECTORY_${_lus_cfg_upper} "${CMAKE_BINARY_DIR}/libultraship/${_lus_cfg}"
            RUNTIME_OUTPUT_DIRECTORY_${_lus_cfg_upper} "${CMAKE_BINARY_DIR}/libultraship/${_lus_cfg}"
            COMPILE_PDB_OUTPUT_DIRECTORY_${_lus_cfg_upper} "${CMAKE_BINARY_DIR}/libultraship/${_lus_cfg}"
            PDB_OUTPUT_DIRECTORY_${_lus_cfg_upper} "${CMAKE_BINARY_DIR}/libultraship/${_lus_cfg}")
    endforeach()
endif()

# Vendored engine: upstream ships without warning hygiene for MSVC's level-3
# diagnostics (int/size_t conversions, precedence parens, narrowing). These
# are upstream's debt — silence them on the target instead of patching
# vendored sources we re-pull on upgrades.
if (MSVC AND TARGET libultraship)
    target_compile_options(libultraship PRIVATE
        /wd4005 /wd4018 /wd4068 /wd4099 /wd4101 /wd4244 /wd4267
        /wd4305 /wd4309 /wd4312 /wd4554 /wd4805 /wd4834 /wd4838
        /wd4201 /wd4310)
    # LUS objects reference libultraship.pdb but Release never emits one
    # (LNK4099); /Zi produces it so /DEBUG linking finds real LUS symbols.
    target_compile_options(libultraship PRIVATE /Zi)
endif()

# LUS public headers (f3dex.h, color.h, types.h) emit MSVC-extension
# diagnostics (C4201 nameless unions, C4310 constant truncation) in every
# port TU that includes them. Consume them as SYSTEM headers so upstream
# noise stays out of our targets without patching the submodule.
if (TARGET libultraship)
    get_target_property(_lus_iface_includes libultraship INTERFACE_INCLUDE_DIRECTORIES)
    if (_lus_iface_includes)
        set_target_properties(libultraship PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_lus_iface_includes}")
    endif()
    unset(_lus_iface_includes)
endif()

################################################################################
# funchook - cross-platform runtime function-detouring lib used by the mod
# loader. Replaces MinHook (which is x86/x64-Windows-only) so the hook system
# works on every release target: Windows x64, Linux x64/arm64, and
# macOS arm64 (Apple Silicon). funchook does the prologue rewrite +
# trampoline emit; HookManager layers the per-target hot-reload chain on top.
#
# funchook pulls its disassembler backend itself (distorm on x86, capstone
# on arm64). Its hardcoded capstone (aquynh/capstone @ 4.0.2) is patched up
# to capstone-engine/capstone @ 5.0.1 via the PATCH_COMMAND below — 4.0.2's
# `cmake_policy(CMP0048 OLD)` is rejected outright by CMake >=4.
# Pinned to v1.1.3 (latest release; the project is archived but the
# instruction sets it rewrites are stable). GPLv2 + linking exception —
# the exception means it does NOT impose GPL on the rest of the codebase.
#
# Desktop-only: the mod system is excluded on Android, where DISABLE_SCRIPTING
# is forced ON, so funchook is not fetched there.
################################################################################
if (NOT DISABLE_SCRIPTING)
    include(FetchContent)
    set(FUNCHOOK_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(FUNCHOOK_BUILD_STATIC ON  CACHE BOOL "" FORCE)
    set(FUNCHOOK_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
    set(FUNCHOOK_INSTALL      OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        funchook
        GIT_REPOSITORY https://github.com/kubo/funchook.git
        GIT_TAG        88388db3c69e16c1560fee65c6857d75f5ce6fd5 # v1.1.3
        PATCH_COMMAND  ${CMAKE_COMMAND}
                       -DCAPSTONE_TMPL=cmake/capstone.cmake.in
                       -DFUNCHOOK_CML=CMakeLists.txt
                       -DFUNCHOOK_UNIX=src/funchook_unix.c
                       -P ${CMAKE_SOURCE_DIR}/cmake/funchook_capstone_bump.cmake
    )
    FetchContent_MakeAvailable(funchook)

    # funchook compiles with the toolchain default CRT (/MD); mixing its
    # objects into the static-CRT (/MT) binary produces LNK4217 __imp_*
    # imports and the LNK4098 MSVCRT conflict — and two different CRT heaps
    # in one process is a real bug class, not noise.
    if (MSVC)
        foreach(_fh_target funchook-static distorm)
            if (TARGET ${_fh_target})
                set_target_properties(${_fh_target} PROPERTIES
                    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
            endif()
        endforeach()
        unset(_fh_target)
    endif()

    # Stale-cache guard. FetchContent runs PATCH_COMMAND only on the FIRST
    # populate of funchook-src; an already-populated _deps/funchook-src (e.g.
    # from an older/partial patch) is reused silently. If the macOS arm64
    # mach_vm_protect rewrite isn't wired into its call site, inline detours
    # fail at runtime with mprotect(RWX) EINVAL — and nothing catches it until
    # a mod tries to hook. Fail loudly at configure time instead. See
    # docs/bugs/funchook_macos_arm64_patch_anchor_2026-06-15.md.
    if (APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_fh_unix "${funchook_SOURCE_DIR}/src/funchook_unix.c")
        if (EXISTS "${_fh_unix}")
            file(READ "${_fh_unix}" _fh_unix_contents)
            if (NOT _fh_unix_contents MATCHES "funchook_mac_arm64_set_rw\\(mstate->addr")
                message(FATAL_ERROR
                    "funchook_unix.c lacks the Apple Silicon mach_vm_protect patch "
                    "(stale FetchContent cache). Remove ${CMAKE_BINARY_DIR}/_deps/funchook-* "
                    "and re-configure. See docs/bugs/funchook_macos_arm64_patch_anchor_2026-06-15.md")
            endif()
            unset(_fh_unix_contents)
        endif()
        unset(_fh_unix)
    endif()
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    # Android: build Torch in-tree as a STATIC lib (USE_STANDALONE=OFF
    # was forced above). The libtorch_runner.so SHARED library at the
    # bottom of this file wraps it for JNI.
    add_subdirectory(components/game-runtime ${CMAKE_BINARY_DIR}/torch)
elseif (NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    # Desktop: build Torch as a host executable via ExternalProject so it
    # can run at our build time to extract ROM assets. Switch has no host
    # Torch — assets are pre-extracted on a PC and bundled onto the SD card.
    set(SSB64_TORCH_BUILD_DIR ${CMAKE_BINARY_DIR}/TorchExternal/src/TorchExternal-build)
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(TORCH_SIDECAR_NAME torch.exe)
        if(CMAKE_CONFIGURATION_TYPES)
            set(TORCH_EXECUTABLE ${SSB64_TORCH_BUILD_DIR}/$<CONFIG>/torch.exe)
        else()
            set(TORCH_EXECUTABLE ${SSB64_TORCH_BUILD_DIR}/torch.exe)
        endif()
    else()
        set(TORCH_SIDECAR_NAME torch)
        set(TORCH_EXECUTABLE ${SSB64_TORCH_BUILD_DIR}/torch)
    endif()
    ExternalProject_Add(TorchExternal
        PREFIX TorchExternal
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/components/game-runtime
        BINARY_DIR ${SSB64_TORCH_BUILD_DIR}
        # Run the nested dependency check every time. Its own build system
        # recompiles only changed files; a completed ExternalProject stamp
        # must not hide subsequent edits to this local submodule.
        BUILD_ALWAYS TRUE
        BUILD_BYPRODUCTS ${TORCH_EXECUTABLE}
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/torch-install
            -DUSE_STANDALONE=ON
            -DBUILD_STORMLIB=OFF
            -DBUILD_SM64=OFF
            -DBUILD_MK64=OFF
            -DBUILD_SF64=OFF
            -DBUILD_PM64=OFF
            -DBUILD_FZERO=OFF
            -DBUILD_MARIO_ARTIST=OFF
            -DBUILD_NAUDIO=ON
            -DBUILD_SSB64=ON
    )

endif()

# Reloc YAML codegen, reloc stub/table generation, credits encoding.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Codegen.cmake)


add_custom_target(PrepareBuildInputs
    DEPENDS GenerateRelocArtifacts GenerateCreditsAssets
)

# ── Window icon embed (US/JP) ─────────────────────────────────────────────
# Linux SDL only renders an app icon on the WM if SDL_SetWindowIcon() is
# called at runtime — there's no .rc-equivalent that the WM reads at
# launch. The PNG bytes are baked into the binary via tools/assets/pipeline/embed_icon.py
# so src/platform/window/port_window_icon.cpp doesn't have to chase the icon across
# build/dev-tree/AppImage layouts. Region-aware: US uses assets/branding/icons/icon.png,
# JP uses assets/branding/icons/icon-jp.png.
set(SSB64_ICON_PNG ${CMAKE_CURRENT_SOURCE_DIR}/assets/branding/icons/icon.png)
if(SSB64_VERSION STREQUAL "jp")
    set(SSB64_ICON_PNG ${CMAKE_CURRENT_SOURCE_DIR}/assets/branding/icons/icon-jp.png)
endif()
set(SSB64_ICON_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(SSB64_ICON_GEN ${SSB64_ICON_GEN_DIR}/port_icon_data.h)
add_custom_command(
    OUTPUT ${SSB64_ICON_GEN}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/assets/pipeline/embed_icon.py
            ${SSB64_ICON_PNG} ${SSB64_ICON_GEN} kSSB64IconPng
    DEPENDS
        ${SSB64_ICON_PNG}
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/assets/pipeline/embed_icon.py
    COMMENT "Embedding window icon (${SSB64_VERSION}) -> ${SSB64_ICON_GEN}"
    VERBATIM
)

# ── CSS icon pipeline ─────────────────────────────────────────────────────
# Stage-specific assets (background wallpapers + CSS thumbnails) are now
# extracted from baserom.us.z64 at build time by tools/assets/pipeline/derive_stage_assets.py
# and loaded at runtime as PNG files. No Nintendo-derived bytes are baked
# into the binary.
#
# The arrow sprite remains baked (it is original port artwork).
set(SSB64_CSS_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)

# Stage asset PNGs — extracted from baserom.us.z64 by derive_stage_assets.py.
# Desktop-only: Android packaging of these assets is a follow-up task.
# The add_custom_command that invokes derive_stage_assets.py is declared later
# (after SSB64_BASEROM is resolved) in the "Asset extraction targets" section.
# The output paths are set here so the POST_BUILD copy can reference them.
set(SSB64_CSS_STAGE_ASSET_DIR ${CMAKE_CURRENT_BINARY_DIR}/assets/css_icons)
set(SSB64_CSS_FD_BG_PNG      ${SSB64_CSS_STAGE_ASSET_DIR}/final_destination_background.png)
set(SSB64_CSS_FD_SMALL_PNG   ${SSB64_CSS_STAGE_ASSET_DIR}/final_destination_small.png)
set(SSB64_CSS_FD_NAME_PNG    ${SSB64_CSS_STAGE_ASSET_DIR}/final_destination_name.png)
set(SSB64_CSS_FD_EMBLEM_PNG  ${SSB64_CSS_STAGE_ASSET_DIR}/final_destination_emblem.png)
# Metal Cavern (nGRKindMetal) — no emblem PNG (mnMapsMakeEmblem skips port
# stages until the IA4 render path is debugged; matches FD).
set(SSB64_CSS_MC_BG_PNG      ${SSB64_CSS_STAGE_ASSET_DIR}/metal_cavern_background.png)
set(SSB64_CSS_MC_SMALL_PNG   ${SSB64_CSS_STAGE_ASSET_DIR}/metal_cavern_small.png)
set(SSB64_CSS_MC_NAME_PNG    ${SSB64_CSS_STAGE_ASSET_DIR}/metal_cavern_name.png)
# Battlefield (nGRKindZako) — same shape as Metal Cavern.
set(SSB64_CSS_BF_BG_PNG      ${SSB64_CSS_STAGE_ASSET_DIR}/battlefield_background.png)
set(SSB64_CSS_BF_SMALL_PNG   ${SSB64_CSS_STAGE_ASSET_DIR}/battlefield_small.png)
set(SSB64_CSS_BF_NAME_PNG    ${SSB64_CSS_STAGE_ASSET_DIR}/battlefield_name.png)

# Scroll arrow — generated procedurally with apex angle ~149°. Canvas width is
# padded to a multiple of 4 px inside the script so RGBA16 row stride stays
# 8-byte-aligned (W=6 visual → W=8 canvas, transparent padding centered).
# Two variants: right-pointing (default) and left-pointing (--mirror) so the
# second-page indicator doesn't depend on negative SObj scalex.
set(SSB64_CSS_ARROW_PNG       ${CMAKE_CURRENT_SOURCE_DIR}/assets/source/css-icons/arrow.png)
set(SSB64_CSS_ARROW_LEFT_PNG  ${CMAKE_CURRENT_SOURCE_DIR}/assets/source/css-icons/arrow_left.png)
set(SSB64_CSS_ARROW_H         ${SSB64_CSS_GEN_DIR}/arrow_data.h)
set(SSB64_CSS_ARROW_LEFT_H    ${SSB64_CSS_GEN_DIR}/arrow_left_data.h)

# Baked nameplate headers for the port-introduced stages. Rendered from text
# strings only (tools/assets/pipeline/render_nameplates.py — NO ROM data), so unlike the
# stage icons these are legal to compile into shipped binaries. They are the
# release fallback for portCSSGetStageNameSprite when no
# assets/css_icons/<name>_name.png override exists; keep the strings in sync
# with STAGES in tools/assets/pipeline/derive_stage_assets.py.
set(SSB64_CSS_NAMEPLATES
    "final_destination|FINAL DESTINATION|nameFinalDestination"
    "metal_cavern|METAL CAVERN|nameMetalCavern"
    "battlefield|BATTLEFIELD|nameBattlefield")
set(SSB64_CSS_NAMEPLATE_HEADERS "")
foreach(entry IN LISTS SSB64_CSS_NAMEPLATES)
    string(REPLACE "|" ";" fields "${entry}")
    list(GET fields 0 np_stem)
    list(GET fields 1 np_text)
    list(GET fields 2 np_symbol)
    set(np_png ${SSB64_CSS_GEN_DIR}/${np_stem}_name.png)
    set(np_h   ${SSB64_CSS_GEN_DIR}/${np_stem}_name_data.h)
    add_custom_command(
        OUTPUT ${np_png}
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/assets/pipeline/render_nameplates.py
                ${np_text}
                ${np_png}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/assets/pipeline/render_nameplates.py
        COMMENT "Rendering nameplate PNG (${np_text})"
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${np_h}
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/assets/pipeline/png_to_c_array.py
                ${np_png}
                ${np_h}
                ${np_symbol}
        DEPENDS ${np_png}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/assets/pipeline/png_to_c_array.py
        COMMENT "Baking nameplate -> ${np_h}"
        VERBATIM
    )
    list(APPEND SSB64_CSS_NAMEPLATE_HEADERS ${np_h})
endforeach()
add_custom_command(
    OUTPUT ${SSB64_CSS_ARROW_PNG}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/assets/pipeline/generate_arrow_svg.py
            ${SSB64_CSS_ARROW_PNG}
            6 44
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/assets/pipeline/generate_arrow_svg.py
    COMMENT "Generating CSS scroll-arrow PNG (visual 6x44, canvas 8x44 padded)"
    VERBATIM
)
add_custom_command(
    OUTPUT ${SSB64_CSS_ARROW_LEFT_PNG}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/assets/pipeline/generate_arrow_svg.py
            ${SSB64_CSS_ARROW_LEFT_PNG}
            6 44 --mirror
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/assets/pipeline/generate_arrow_svg.py
    COMMENT "Generating CSS scroll-arrow PNG (left-mirrored)"
    VERBATIM
)
add_custom_command(
    OUTPUT ${SSB64_CSS_ARROW_H}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/assets/pipeline/png_to_c_array.py
            ${SSB64_CSS_ARROW_PNG}
            ${SSB64_CSS_ARROW_H}
            arrow
    DEPENDS
        ${SSB64_CSS_ARROW_PNG}
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/assets/pipeline/png_to_c_array.py
    COMMENT "Baking scroll-arrow -> ${SSB64_CSS_ARROW_H}"
    VERBATIM
)
add_custom_command(
    OUTPUT ${SSB64_CSS_ARROW_LEFT_H}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/assets/pipeline/png_to_c_array.py
            ${SSB64_CSS_ARROW_LEFT_PNG}
            ${SSB64_CSS_ARROW_LEFT_H}
            arrowLeft
    DEPENDS
        ${SSB64_CSS_ARROW_LEFT_PNG}
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/assets/pipeline/png_to_c_array.py
    COMMENT "Baking scroll-arrow (left) -> ${SSB64_CSS_ARROW_LEFT_H}"
    VERBATIM
)

add_custom_target(ExtractAssetHeaders
    DEPENDS PrepareBuildInputs
    COMMENT "BattleShip asset/header generation is driven by PrepareBuildInputs"
)

################################################################################
# Collect game sources
################################################################################

# Decomp game code (exclude debug overlay and debug battle/menu)
file(GLOB_RECURSE SSB64_SRC_CREDITS "components/decomp/src/credits/*.c" "components/decomp/src/credits/*.h")
file(GLOB_RECURSE SSB64_SRC_EF      "components/decomp/src/ef/*.c"      "components/decomp/src/ef/*.h")
file(GLOB_RECURSE SSB64_SRC_FT      "components/decomp/src/ft/*.c"      "components/decomp/src/ft/*.h")
file(GLOB_RECURSE SSB64_SRC_GM      "components/decomp/src/gm/*.c"      "components/decomp/src/gm/*.h")
file(GLOB_RECURSE SSB64_SRC_GR      "components/decomp/src/gr/*.c"      "components/decomp/src/gr/*.h")
file(GLOB_RECURSE SSB64_SRC_IF      "components/decomp/src/if/*.c"      "components/decomp/src/if/*.h")
file(GLOB_RECURSE SSB64_SRC_IT      "components/decomp/src/it/*.c"      "components/decomp/src/it/*.h")
file(GLOB_RECURSE SSB64_SRC_LB      "components/decomp/src/lb/*.c"      "components/decomp/src/lb/*.h")
file(GLOB_RECURSE SSB64_SRC_MN      "components/decomp/src/mn/*.c"      "components/decomp/src/mn/*.h")
file(GLOB_RECURSE SSB64_SRC_MP      "components/decomp/src/mp/*.c"      "components/decomp/src/mp/*.h")
file(GLOB_RECURSE SSB64_SRC_MV      "components/decomp/src/mv/*.c"      "components/decomp/src/mv/*.h")
file(GLOB_RECURSE SSB64_SRC_SC      "components/decomp/src/sc/*.c"      "components/decomp/src/sc/*.h")
file(GLOB_RECURSE SSB64_SRC_SYS     "components/decomp/src/sys/*.c"     "components/decomp/src/sys/*.h")
file(GLOB_RECURSE SSB64_SRC_WP      "components/decomp/src/wp/*.c"      "components/decomp/src/wp/*.h")

# Port layer (C++ glue) — excludes src/platform/compatibility/ which is compiled as part of ssb64_game
# CONFIGURE_DEPENDS: new port TUs (e.g. a new src/game/gameplay/**/*.cpp whose
# symbol a decomp TU references) must be picked up by already-configured build
# dirs, or a plain `cmake --build` after pulling hits an undefined-reference
# link error against a stale glob.
file(GLOB_RECURSE SSB64_SRC_PORT CONFIGURE_DEPENDS "src/*.cpp" "src/*.c" "src/*.h" "src/*.S")
list(FILTER SSB64_SRC_PORT EXCLUDE REGEX "src/platform/compatibility/.*\.c$")
# RelocFileTable.{us,jp}.cpp both define gRelocFileTable — compile only the
# selected version's table (the rest are siblings for the other version).
# ROM-free generated sources live in the build tree (generated by
# GenerateRelocArtifacts), so the src glob never sees them — append the
# selected region explicitly.
list(APPEND SSB64_SRC_PORT
    ${CMAKE_BINARY_DIR}/generated/src/${SSB64_VERSION}/RelocFileTable.${SSB64_VERSION}.cpp)
# Deblob rebuild specs are region-specific the same way: both
# ssb64_reloc_rebuild.{us,jp}.cpp define the same synthesis entry points.
# They stay versioned under generated/src/ because their generator needs a
# real baserom (see generated/README.md).
list(APPEND SSB64_SRC_PORT
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/src/${SSB64_VERSION}/ssb64_reloc_rebuild.${SSB64_VERSION}.cpp)
# coroutine_test.cpp has its own main(); only built standalone when
# SSB64_BUILD_COROUTINE_TEST=ON below. Never link it into the game.
list(FILTER SSB64_SRC_PORT EXCLUDE REGEX "tests/unit/platform/coroutine_test\\.cpp$")
# ARM coroutine asm shims are added explicitly to the Android target below;
# MSVC can't assemble .S and produces no .obj, which breaks the exports.def
# generation at link time. Always excluded from the glob: with per-module
# OBJECT libraries (TD-002) a file living in both smash_platform and the
# SHARED target would compile twice and produce duplicate symbols.
list(FILTER SSB64_SRC_PORT EXCLUDE REGEX "src/platform/coroutine/coroutine_.*\\.S$")
# Switch asm context-switch is AArch64-only and only appended on Switch
# below — but the case-insensitive filesystem glob also matches "*.S" on
# Windows, where MSVC can't assemble it. (The guarded .cpp files compile
# empty and are harmless.)
if (NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    list(FILTER SSB64_SRC_PORT EXCLUDE REGEX "src/platform/switch/context_switch\\.s$")
endif()
# src/game/modding/ (HookManager + SymbolResolver) wraps funchook, which isn't
# fetched when the mod system is disabled. Drop it so nothing references the
# missing backend. src/game/engine/hooks/ (the event dispatch system) is funchook-free
# and uses core libultraship events, so it stays compiled on every target.
if (DISABLE_SCRIPTING)
    list(FILTER SSB64_SRC_PORT EXCLUDE REGEX "src/game/modding/")
endif()
if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    # libtorch_runner.so wraps android_torch_bridge.cpp separately; keep
    # it out of libmain.so so the static libtorch + its FetchContent'd
    # deps don't bloat the SDL-bound game library.
    list(FILTER SSB64_SRC_PORT EXCLUDE REGEX "src/platform/android/android_torch_bridge\\.cpp$")
    # Drop features we explicitly disable on Android (no curl-driven
    # self-updater — Android updates via APK installs, not in-app binary
    # replacement — and no discord-rpc target). ShaderDownloader and
    # ModPackDownloader are NOT excluded: both go through the platform
    # download service (HttpURLConnection via JNI on Android) and
    # in-process libzip/SHA-256, so they run on Android too.
    # The corresponding menu entries are gated with #if !defined(__ANDROID__)
    # in src/interfaces/presentation/PortMenu.cpp so nothing references the removed symbols.
    # (src/interfaces/rendering/textures/hires/ is NOT dropped here — it compiles on Android but runs
    # opt-in with a reduced LRU budget; see the PORT_HIRES_ENABLED block.)
    list(FILTER SSB64_SRC_PORT EXCLUDE REGEX
        "src/(application/update/updater/Updater|networking/social/DiscordRichPresence)\\.cpp$")
    list(FILTER SSB64_SRC_PORT EXCLUDE REGEX "src/platform/window/port_window_icon\\.cpp$")
endif()
if(BATTLESHIP_UWP)
    # Xbox packages are updated as MSIX and cannot use these Win32
    # shell-out/IPC integrations.
    list(FILTER SSB64_SRC_PORT EXCLUDE REGEX
        "src/(application/update/(updater/Updater|downloads/(ShaderDownloader|ModPackDownloader))|networking/social/DiscordRichPresence)\\.cpp$")
    list(FILTER SSB64_SRC_PORT EXCLUDE REGEX "src/platform/window/port_window_icon\\.cpp$")
    list(FILTER SSB64_SRC_PORT EXCLUDE REGEX "src/application/first_run\\.cpp$")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    # Switch: no Discord SDK, no curl/HTTP self-update or download services,
    # no Win32 window icon, no Torch first-run wizard — assets ship
    # pre-extracted on the SD card (same exclusions as UWP).
    list(FILTER SSB64_SRC_PORT EXCLUDE REGEX
        "src/(application/update/(updater/Updater|downloads/(ShaderDownloader|ModPackDownloader))|networking/social/DiscordRichPresence)\\.cpp$")
    list(FILTER SSB64_SRC_PORT EXCLUDE REGEX "src/platform/window/port_window_icon\\.cpp$")
    list(FILTER SSB64_SRC_PORT EXCLUDE REGEX "src/application/first_run\\.cpp$")
endif()
# Treat the generated icon header as a source so CMake schedules it ahead
# of any port TU compilation (port_window_icon.cpp #includes it). The
# headers are ALSO tracked in SSB64_GEN_HEADER_SRCS: TD-002 compiles
# src/<module>/ trees as separate OBJECT libraries, and a file-level
# codegen edge only applies to the target listing the generated file —
# every module lib gets them as sources so no TU can compile before the
# headers exist.
set(SSB64_GEN_HEADER_SRCS "")
if (NOT CMAKE_SYSTEM_NAME STREQUAL "Android" AND NOT BATTLESHIP_UWP)
    list(APPEND SSB64_GEN_HEADER_SRCS ${SSB64_ICON_GEN})
endif()
# Arrow + nameplate baked headers — added as sources so CMake schedules their
# generation before any port TU compilation.  Stage background/icon PNGs are
# ROM-derived and therefore disk-loaded/o2r-derived at runtime; no baked
# headers for those.
list(APPEND SSB64_GEN_HEADER_SRCS
    ${SSB64_CSS_ARROW_H}
    ${SSB64_CSS_ARROW_LEFT_H}
    ${SSB64_CSS_NAMEPLATE_HEADERS}
)
list(APPEND SSB64_SRC_PORT ${SSB64_GEN_HEADER_SRCS})

# JP drops the hi-res texture pack pipeline entirely (US-ROM CRC32 keying,
# no JP packs authored); PORT_HIRES_ENABLED is appended to the compile defs
# below where the rationale comment lives. The source filter must run
# BEFORE the per-module partition — module OBJECT libraries snapshoot
# SSB64_SRC_PORT at partition time.
if(NOT SSB64_VERSION STREQUAL "us")
    list(FILTER SSB64_SRC_PORT EXCLUDE REGEX "src/interfaces/rendering/textures/hires/")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    list(APPEND SSB64_SRC_PORT "src/platform/switch/context_switch.s")
endif()

################################################################################
# TD-002 — per-module component libraries
#
# Compile each src/<module>/ subtree as its own OBJECT library
# (smash_<module>). Compile-time boundary only: every object still links
# into the single ${PROJECT_NAME} binary, so no link-order work is needed
# and cyclic module includes behave exactly as they did with the flat
# glob. What the split buys: per-module warning/flag scoping, a real
# target graph mirroring check_module_deps.py's rules, and codegen
# ordering edges that no longer depend on one shared source list.
#
# ssb64_port_options carries the definitions/include paths/link usage
# requirements every port TU needs; the module libs and the executable
# all consume it.
################################################################################

add_library(ssb64_port_options INTERFACE)
# ${PROJECT_NAME} is SHARED on Android/UWP — propagate PIC so the module
# objects are built correctly on those targets too.
set_target_properties(ssb64_port_options PROPERTIES
    INTERFACE_POSITION_INDEPENDENT_CODE ON)

# First-party quality flags — attached ONCE to the options interface so
# every smash_* object lib and the executable inherit them. External code
# (components/decomp/, components/libultraship/, components/game-runtime/, FetchContent) never consumes it.
include(cmake/CompilerWarnings.cmake)
include(cmake/Hardening.cmake)
include(cmake/Sanitizers.cmake)
include(cmake/StaticAnalysis.cmake)
ssb64_apply_warnings(ssb64_port_options)
ssb64_apply_hardening(ssb64_port_options)
ssb64_apply_sanitizers(ssb64_port_options)

# Module name -> canonical source root. Order matters: a module nested
# under another's directory must be listed FIRST so it is carved out of
# the pool before the parent claims it (update<application,
# decomp/hooks<engine, compatibility<platform). Roots are explicit —
# name-matched filters would let e.g. "audio" steal
# src/interfaces/audio/n64/.
set(SSB64_MODULE_DIRS
    update application
    config i18n observability persistence
    decomp hooks engine gameplay modding
    audio input presentation rendering
    netplay social
    compatibility platform)
set(SSB64_MODULE_ROOT_update        src/application/update)
set(SSB64_MODULE_ROOT_application   src/application)
set(SSB64_MODULE_ROOT_config        src/config)
set(SSB64_MODULE_ROOT_i18n          src/i18n)
set(SSB64_MODULE_ROOT_observability src/observability)
set(SSB64_MODULE_ROOT_persistence   src/persistence)
set(SSB64_MODULE_ROOT_decomp        src/game/engine/decomp)
set(SSB64_MODULE_ROOT_hooks         src/game/engine/hooks)
set(SSB64_MODULE_ROOT_engine        src/game/engine)
set(SSB64_MODULE_ROOT_gameplay      src/game/gameplay)
set(SSB64_MODULE_ROOT_modding       src/game/modding)
set(SSB64_MODULE_ROOT_audio         src/interfaces/audio)
set(SSB64_MODULE_ROOT_input         src/interfaces/input)
set(SSB64_MODULE_ROOT_presentation  src/interfaces/presentation)
set(SSB64_MODULE_ROOT_rendering     src/interfaces/rendering)
set(SSB64_MODULE_ROOT_netplay       src/networking/netplay)
set(SSB64_MODULE_ROOT_social        src/networking/social)
set(SSB64_MODULE_ROOT_compatibility src/platform/compatibility)
set(SSB64_MODULE_ROOT_platform      src/platform)

# Compiler-enforced module boundaries. Mirrors the FORBIDDEN matrix in
# tools/analysis/lint/check_module_deps.py at submodule granularity: a
# scoped smash_<mod> receives ONLY these include dirs PRIVATE, so a
# cross-module #include outside the list fails at compile time instead of
# waiting for the lint. Basename-style includes are what this guards;
# qualified `src/<module>/...` includes resolve via the src/ root entry,
# which scoped modules do not get (the lint still covers that channel).
set(SSB64_MODULE_SCOPE_observability
    src/observability)
set(SSB64_MODULE_SCOPE_config
    src/config
    src/observability)            # port_log.h — shared log sink
set(SSB64_MODULE_SCOPE_persistence
    src/persistence
    src/observability)
set(SSB64_MODULE_SCOPE_i18n
    src/i18n
    src/observability             # port_log.h
    src/platform/filesystem)           # app_paths.h — lang/ lookup
set(SSB64_MODULE_SCOPE_compatibility
    src/platform/compatibility src/platform src/platform/coroutine
    src/platform/http src/platform/filesystem src/platform/window
    src/platform/android
    src/config src/observability src/persistence
    src/interfaces/rendering src/interfaces/rendering/interpolation
    src/game/engine/relocation
    src/interfaces/rendering/shaders
    src/interfaces/rendering/textures/hires)
set(SSB64_MODULE_SCOPE_platform
    src/platform src/platform/coroutine src/platform/http
    src/platform/filesystem src/platform/window src/platform/android
    src/platform/compatibility
    src/observability)            # port_log.h — shared log sink
set(SSB64_MODULE_SCOPE_engine
    src/game/engine src/game/engine/decomp src/game/engine/hooks
    src/game/engine/relocation         # RelocFile/RelocPointerTable — asset reloc ABI
    src/game/engine/css_icons          # CSS stage/arrow assets served to decomp
    src/platform/filesystem            # app_paths.h — css_icons asset lookup
    src/observability)
set(SSB64_MODULE_SCOPE_gameplay
    src/game/gameplay src/game/gameplay/assists src/game/gameplay/modes
    src/game/gameplay/rules src/game/gameplay/training
    src/game/gameplay/unlocks
    src/networking/netplay/domain      # netrules.h — rules shared with netplay
    src/observability)
set(SSB64_MODULE_SCOPE_modding
    src/game/modding
    src/observability)
set(SSB64_MODULE_SCOPE_netplay
    src/networking/netplay src/networking/netplay/domain
    src/networking/netplay/protocol src/networking/netplay/session
    src/networking/netplay/transport src/networking/netplay/rollback
    src/networking/netplay/diagnostics
    src/game/modding                   # ModRegistry.h — mod-loaded check
    src/i18n src/observability)
set(SSB64_MODULE_SCOPE_social
    src/networking/social
    src/game/gameplay                  # enhancements.h — presence reads game state
    src/observability)
set(SSB64_MODULE_SCOPE_audio
    src/interfaces/audio src/interfaces/audio/n64
    src/game/gameplay                  # enhancements.h
    src/observability)
set(SSB64_MODULE_SCOPE_input
    src/interfaces/input src/interfaces/input/internal
    src/networking/netplay/domain      # netrules.h — input sync rules
    src/game/gameplay
    src/observability)
set(SSB64_MODULE_SCOPE_rendering
    src/interfaces/rendering src/interfaces/rendering/widescreen
    src/interfaces/rendering/cosmetics
    src/interfaces/rendering/interpolation
    src/interfaces/rendering/shaders src/interfaces/rendering/textures/hires
    src/game/gameplay                  # enhancements.h
    src/observability)
set(SSB64_MODULE_SCOPE_presentation
    src/interfaces/presentation
    src/application                    # app_mods.h — mods panel
    src/config src/i18n src/persistence
    src/observability
    src/game/gameplay                  # enhancements.h
    src/networking/netplay/session src/networking/netplay/transport
    src/platform src/platform/filesystem
    src/interfaces/rendering/interpolation
    src/interfaces/rendering/textures/hires)
set(SSB64_MODULE_SCOPE_decomp
    src/game/engine/decomp
    src/game/engine/relocation)          # RelocFile/RelocPointerTable
set(SSB64_MODULE_SCOPE_hooks
    src/game/engine/hooks)             # list/*.h resolves via this dir
set(SSB64_MODULE_SCOPE_update
    src/application/update src/application/update/updater
    src/application/update/downloads src/application/update/integrity
    src/application/update/manifests
    src/game/gameplay                  # enhancements.h — update check entry
    src/platform/filesystem src/platform/http
    src/observability)

set(SSB64_MODULE_OBJECTS "")
set(SSB64_SRC_PORT_TOPLEVEL ${SSB64_SRC_PORT})
foreach(mod IN LISTS SSB64_MODULE_DIRS)
    set(_mod_srcs ${SSB64_SRC_PORT_TOPLEVEL})
    list(FILTER _mod_srcs INCLUDE REGEX "/${SSB64_MODULE_ROOT_${mod}}/")
    if(_mod_srcs)
        add_library(smash_${mod} OBJECT ${_mod_srcs})
        target_sources(smash_${mod} PRIVATE ${SSB64_GEN_HEADER_SRCS})
        target_link_libraries(smash_${mod} PRIVATE ssb64_port_options)
        if(DEFINED SSB64_MODULE_SCOPE_${mod})
            # Scoped: only the module's own subtree + declared deps.
            target_include_directories(smash_${mod} PRIVATE
                ${SSB64_MODULE_SCOPE_${mod}})
        else()
            # Unscoped (migration pending): legacy flat src/ include set.
            target_link_libraries(smash_${mod} PRIVATE ssb64_src_flat_includes)
        endif()
        ssb64_apply_static_analysis(smash_${mod})
        add_dependencies(smash_${mod} ExtractAssetHeaders)
        list(APPEND SSB64_MODULE_OBJECTS $<TARGET_OBJECTS:smash_${mod}>)
        list(REMOVE_ITEM SSB64_SRC_PORT_TOPLEVEL ${_mod_srcs})
    endif()
endforeach()

# Debug tools (GBI trace + Acmd trace systems)
file(GLOB SSB64_DEBUG_TOOLS
    "tools/debugging/gbi_trace/*.c" "tools/debugging/gbi_trace/*.h"
    "tools/debugging/acmd_trace/*.c" "tools/debugging/acmd_trace/*.h"
)

set(SSB64_DECOMP_SOURCES
    ${SSB64_SRC_CREDITS}
    ${SSB64_SRC_EF}
    ${SSB64_SRC_FT}
    ${SSB64_SRC_GM}
    ${SSB64_SRC_GR}
    ${SSB64_SRC_IF}
    ${SSB64_SRC_IT}
    ${SSB64_SRC_LB}
    ${SSB64_SRC_MN}
    ${SSB64_SRC_MP}
    ${SSB64_SRC_MV}
    ${SSB64_SRC_SC}
    ${SSB64_SRC_SYS}
    ${SSB64_SRC_WP}
)

# Exclude .inc.c files (inline data included by other .c files)
list(FILTER SSB64_DECOMP_SOURCES EXCLUDE REGEX "\\.inc\\.c$")

# JP build excludes the US-only startup/congratulations menu sources
# (matches the upstream decomp Makefile's VERSION=jp C_FILES filter-out).
if(SSB64_VERSION STREQUAL "jp")
    list(FILTER SSB64_DECOMP_SOURCES EXCLUDE REGEX
        "components/decomp/src/mn/mncommon/(mncongra|mnstartup)\\.c$")
endif()

################################################################################
# Excluded directories (not compiled):
#   src/ovl8/   - Debug overlay
#   src/db/     - Debug battle/menu testing
#   src/libultra/ - Replaced by libultraship
################################################################################

# User-facing application identity. The JP build is a SEPARATE application:
# distinct binary name, distinct app-data dir (saves/config/logs/o2r), so a
# user can have both installed and they never touch each other's ROM/o2r.
# US keeps the historical "BattleShip" so existing installs / the in-app
# updater / release links are unaffected. This single value drives the
# binary OUTPUT_NAME, the libultraship Context name/shortName (which scopes
# the app-data directory), and the per-platform package scripts.
if(SSB64_VERSION STREQUAL "jp")
    set(SSB64_APP_NAME "BattleShip-JP")
else()
    set(SSB64_APP_NAME "BattleShip")
endif()
message(STATUS "SSB64 app identity: ${SSB64_APP_NAME}")

# Asset-recipe fingerprint: identifies the extraction pipeline (Torch source +
# config.yml + recipe yamls) that a BattleShip.o2r was produced by. The
# port stamps it into a "<o2r>.recipe" sidecar after extraction and
# re-extracts automatically when a binary update changes the recipe —
# stale archives from older builds caused reloc-token storms and menu
# crashes (issues #217/#221) until users manually re-extracted.
file(GLOB_RECURSE SSB64_TORCH_RECIPE_INPUTS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/components/game-runtime/src/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/components/game-runtime/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/components/game-runtime/src/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/components/game-runtime/src/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/components/game-runtime/lib/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/components/game-runtime/lib/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/components/game-runtime/lib/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/components/game-runtime/lib/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/components/game-runtime/cmake/*.cmake)
list(APPEND SSB64_TORCH_RECIPE_INPUTS ${CMAKE_CURRENT_SOURCE_DIR}/components/game-runtime/CMakeLists.txt)
# GLOB_RECURSE: the deblob child yamls live in per-category subdirectories
# (assets/game/<v>/reloc_fighters_main/*.yml,
# docs/deblob.md) and must invalidate the recipe hash — a stale archive
# missing slices would fail synthesis.
file(GLOB_RECURSE SSB64_RECIPE_YAMLS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/game/${SSB64_VERSION}/*.yml
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/game/${SSB64_VERSION}/*.yaml)
set(SSB64_RECIPE_INPUTS ${SSB64_TORCH_RECIPE_INPUTS} ${SSB64_RECIPE_YAMLS}
    ${CMAKE_CURRENT_SOURCE_DIR}/config/project/config.yml)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SSB64_RECIPE_INPUTS})
list(SORT SSB64_RECIPE_INPUTS)
set(SSB64_RECIPE_CONCAT "")
foreach(_recipe_input IN LISTS SSB64_RECIPE_INPUTS)
    file(SHA1 ${_recipe_input} _recipe_input_sha1)
    file(RELATIVE_PATH _recipe_path ${CMAKE_CURRENT_SOURCE_DIR} ${_recipe_input})
    string(APPEND SSB64_RECIPE_CONCAT "${_recipe_path}:${_recipe_input_sha1}\n")
endforeach()
string(SHA1 SSB64_ASSET_RECIPE_HASH "${SSB64_RECIPE_CONCAT}")
message(STATUS "SSB64 asset recipe hash: ${SSB64_ASSET_RECIPE_HASH}")
# Track uncommitted extractor edits too, and compile the hash only into its
# consumer: changing a recipe must not rebuild every game translation unit.
set_property(SOURCE src/application/first_run.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    SSB64_ASSET_RECIPE_HASH=\"${SSB64_ASSET_RECIPE_HASH}\")
set(SSB64_RECIPE_DIR ${CMAKE_BINARY_DIR}/asset-recipes)
include(cmake/StageAssetRecipes.cmake)
ssb64_stage_asset_recipes("${CMAKE_CURRENT_SOURCE_DIR}" "${SSB64_RECIPE_DIR}" "${SSB64_VERSION}")
file(GENERATE OUTPUT ${SSB64_RECIPE_DIR}/recipe.sha1 CONTENT "${SSB64_ASSET_RECIPE_HASH}\n")

# Test hooks (SSB64_NETPLAY_MUTANT, SSB64_NETPLAY_PACKETFUZZ,
# SSB64_ROLLBACK_MUTANT, SSB64_NETPLAY_TEST_DESYNC, SSB64_COMPAT_TOKEN)
# weaken packet validation and forge identity for the determinism/mutation
# harnesses. They must NEVER ship in a release binary — pass
# -DSSB64_TEST_HOOKS=OFF when packaging (release.yml does). Default ON so
# local test scripts keep working on dev builds.
option(SSB64_TEST_HOOKS "Compile in env-driven test/mutation hooks" ON)

# Shared compile definitions used by both the decomp object library and the
# main executable.
set(SSB64_COMPILE_DEFS
    # Region (REGION_US/VERSION_US or REGION_JP/VERSION_JP per SSB64_VERSION)
    REGION_${SSB64_VERSION_UC}=1
    VERSION_${SSB64_VERSION_UC}=1
    # App identity (binary name + libultraship app-data scope). Quoted so
    # it expands to a C string literal at the use site.
    SSB64_APP_NAME=\"${SSB64_APP_NAME}\"
    # Per-region archive filename: US writes/reads "BattleShip.o2r"
    # (preserves the historical name so existing installs keep working);
    # JP writes/reads "BattleShip-JP.o2r" so a stray archive in CWD or an
    # accidental shared install directory can't be picked up by the wrong
    # build (PortLocateFile falls back to CWD on miss, and the bifurcated
    # app-data dirs from issue #190 don't catch that case).
    SSB64_O2R_NAME=\"${SSB64_APP_NAME}.o2r\"

    # Port mode
    NON_MATCHING=1
    NON_EQUIVALENT=1
    AVOID_UB=1
    PORT=1

    # GBI / graphics
    F3DEX_GBI_2=1
    _LANGUAGE_C

    # Audio — SSB64 uses the compact "n_audio" microcode (N_MICRO).
    # This controls buffer layout and command set in the synthesis pull chain.
    N_MICRO=1

    # Platform compat
    _USE_MATH_DEFINES
    # Port code uses portable getenv/fopen/strdup and POSIX names (fileno
    # etc.); MSVC's *_s / _-prefixed replacements are Windows-only. Silence
    # both deprecation classes once, globally.
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_WARNINGS

    # ImGui
    CIMGUI_DEFINE_ENUMS_AND_STRUCTS

    # Updater
    BATTLESHIP_CURRENT_VERSION=\"${BATTLESHIP_VERSION}\"

    # Netplay handshake (see src/networking/netplay/session/netlobby.cpp)
    BATTLESHIP_BUILD_SHA=\"${BATTLESHIP_BUILD_SHA}\"
)

if(SSB64_STAGE_CYCLE_DEMO)
    list(APPEND SSB64_COMPILE_DEFS PORT_STAGE_CYCLE_DEMO=1)
endif()

# Hi-res texture pack support is US-only (desktop AND Android). On Android
# it compiles but defaults to OFF and runs with a smaller decoded-RGBA8 LRU
# budget (see src/interfaces/rendering/textures/hires/HiResPack.h: kHiResEnabledDefault / kDefaultLruBudgetMB)
# plus a per-texture upscale cap, so a pack can't blow the mobile memory
# budget and trip the low-memory killer; the dev dump-tooling menu items are
# still gated off touch UI in PortMenu.cpp.
# Pack PNGs are addressed by CRC32-IEEE of the decoded RGBA8 buffer that
# came out of the US ROM fast-path; JP rendering hits different hash inputs
# (different glyph art for VS-records, JP-only menus, plus palette/sprite
# nuances on shared assets), so a US-built pack would silently miss on JP
# and no JP-only pack has been authored. Drop src/interfaces/rendering/textures/hires/ from the JP build
# entirely and gate the port.cpp / PortMenu.cpp integration on PORT_HIRES_ENABLED.
if(SSB64_VERSION STREQUAL "us")
    list(APPEND SSB64_COMPILE_DEFS PORT_HIRES_ENABLED=1)
endif()
# (the JP-side src filter moved above the TD-002 module partition — it has
#  to run before SSB64_SRC_PORT is carved into smash_* OBJECT libraries)

if(SSB64_TEST_HOOKS)
    list(APPEND SSB64_COMPILE_DEFS SSB64_TEST_HOOKS=1)
endif()

# TD-009 — embed the release pubkey (raw 32-byte Ed25519 key, hex) when the
# maintainer has provisioned one. Generate with `minisign -G` and derive via
# tools/release/make_pubkey_hex.py — see docs/development/release_signing.md.
# Builds without the file ship the updater in sha256-integrity-only mode.
if(EXISTS "${CMAKE_SOURCE_DIR}/config/release_pubkey.hex")
    file(READ "${CMAKE_SOURCE_DIR}/config/release_pubkey.hex" _release_pubkey)
    string(STRIP "${_release_pubkey}" _release_pubkey)
    if(_release_pubkey MATCHES "^[0-9a-fA-F]{64}$")
        list(APPEND SSB64_COMPILE_DEFS BATTLESHIP_RELEASE_PUBKEY=\"${_release_pubkey}\")
        message(STATUS "Release signature verification ENABLED (pubkey embedded)")
    else()
        message(WARNING "config/release_pubkey.hex is not 64 hex chars — "
                        "release signature verification DISABLED for this build")
    endif()
    unset(_release_pubkey)
endif()

################################################################################
# Decomp C code — OBJECT library with custom include/ shadowing system headers
################################################################################

add_library(ssb64_game OBJECT ${SSB64_DECOMP_SOURCES})
# Sanitizers/static-analysis cover the game objects too (they are ours to
# patch); strict warnings do NOT — decomp source would not survive /W4.
if(SSB64_ENABLE_ASAN OR SSB64_ENABLE_UBSAN)
    add_library(ssb64_game_sanitizers INTERFACE)
    ssb64_apply_sanitizers(ssb64_game_sanitizers)
    target_link_libraries(ssb64_game PRIVATE ssb64_game_sanitizers)
endif()
ssb64_apply_static_analysis(ssb64_game)

# Port stub definitions (C code that satisfies N64 linker symbols)
file(GLOB SSB64_STUBS "src/platform/compatibility/*.c")
target_sources(ssb64_game PRIVATE ${SSB64_STUBS})

# Selectively include decomp libultra files that provide needed functionality.
# These are pure C implementations that work on any platform.
set(SSB64_LIBULTRA_PORT
    components/decomp/src/libultra/gu/mtxcatf.c       # guMtxCatF
    components/decomp/src/libultra/gu/mtxutil.c        # guMtxF2L, guMtxL2F, guMtxIdentF
    components/decomp/src/libultra/gu/mtxxfmf.c        # guMtxXFMF
    components/decomp/src/libultra/gu/normalize.c      # guNormalize
    components/decomp/src/libultra/gu/sinf.c           # __sinf — SGI polynomial + Cody-Waite reduction
    components/decomp/src/libultra/gu/cosf.c           # __cosf — same (all game trig routes here, not host libm)
    # NOTE: sprite.c is NOT included — drawbitmap is assembly-only (MIPS).
    # Sprite functions are stubbed in src/platform/compatibility/n64_stubs.c instead.
)
target_sources(ssb64_game PRIVATE ${SSB64_LIBULTRA_PORT})

# The SGI trig sources type-pun floats through int* (guint.h du/fu idiom) and
# their double-precision polynomials must evaluate identically on every host:
# AArch64 contracts a*b+c into fmadd by default, which perturbs the last ulp
# vs. the N64's separate mul/add. IDO's #pragma weak sinf/cosf aliases are
# ignored by MSVC, so PR/gu.h also #defines sinf/cosf -> __sinf/__cosf under
# PORT to route every bare call site to the SGI implementation on all hosts.
# -fno-builtin keeps GCC/Clang from constant-folding or sincos-fusing bare
# calls past the aliases.
if (NOT MSVC)
    set_source_files_properties(
        components/decomp/src/libultra/gu/sinf.c
        components/decomp/src/libultra/gu/cosf.c
        PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-fno-strict-aliasing")
    # Contraction folds a*b+c into fmadd on AArch64, perturbing last-ulp
    # results vs. x86-64 SSE2 and the N64's separate mul.s/add.s — a silent
    # cross-platform desync vector for netplay (PC <-> Android). MSVC's
    # default /fp:precise already does not contract.
    target_compile_options(ssb64_game PRIVATE -ffp-contract=off)
    target_compile_options(ssb64_game PRIVATE -fno-builtin-sinf -fno-builtin-cosf)
endif()

# Rollback snapshots (docs/rollback_design.md): force a section-pragma header
# into every gameplay-module TU so all their globals are contiguous and
# sys/netrollback.c can save/restore them with one memcpy. Engine plumbing
# (net*, audio, scheduler, controller, taskman, reloc loader) stays outside.
if (MSVC)
    set(SSB64_ROLLBACK_SEG_SOURCES
        ${SSB64_SRC_EF} ${SSB64_SRC_FT} ${SSB64_SRC_GM} ${SSB64_SRC_GR}
        ${SSB64_SRC_IF} ${SSB64_SRC_IT} ${SSB64_SRC_MP} ${SSB64_SRC_SC}
        ${SSB64_SRC_WP}
        ${CMAKE_CURRENT_SOURCE_DIR}/components/decomp/src/lb/lbcommon.c
        ${CMAKE_CURRENT_SOURCE_DIR}/components/decomp/src/lb/lbparticle.c
        ${CMAKE_CURRENT_SOURCE_DIR}/components/decomp/src/sys/objanim.c
        ${CMAKE_CURRENT_SOURCE_DIR}/components/decomp/src/sys/objhelper.c
        ${CMAKE_CURRENT_SOURCE_DIR}/components/decomp/src/sys/objman.c
        ${CMAKE_CURRENT_SOURCE_DIR}/components/decomp/src/sys/objscript.c
        ${CMAKE_CURRENT_SOURCE_DIR}/components/decomp/src/sys/utils.c)
    list(FILTER SSB64_ROLLBACK_SEG_SOURCES INCLUDE REGEX "\\.c$")
    list(FILTER SSB64_ROLLBACK_SEG_SOURCES EXCLUDE REGEX "\\.inc\\.c$")
    set_source_files_properties(${SSB64_ROLLBACK_SEG_SOURCES}
        PROPERTIES COMPILE_OPTIONS "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/networking/netplay/rollback/netrollback_seg.h")
endif()

# N64 audio library — synthesis driver, sequence player, voice management.
# These decomp files are compiled with PORT defines; the CPU Acmd interpreter
# (src/interfaces/audio/n64/mixer.h) replaces RSP microcode via macro substitution.
set(SSB64_NAUDIO_PORT
    components/decomp/src/libultra/n_audio/n_env.c             # Core: synth driver, audio frame, pull chain, CSP, FX, event queue
    components/decomp/src/libultra/n_audio/n_synaddplayer.c     # Add player to synth driver
    components/decomp/src/libultra/n_audio/n_synallocfx.c       # FX bus allocation
    components/decomp/src/libultra/n_audio/n_synallocvoice.c    # Voice allocation + stealing
    components/decomp/src/libultra/n_audio/n_synfreevoice.c     # Voice free
    components/decomp/src/libultra/n_audio/n_syndelete.c        # Synth delete
    components/decomp/src/libultra/n_audio/n_synsetfxmix.c      # Voice FX mix
    components/decomp/src/libultra/n_audio/n_synsetpan.c        # Voice pan
    components/decomp/src/libultra/n_audio/n_synsetpitch.c      # Voice pitch
    components/decomp/src/libultra/n_audio/n_synsetpriority.c   # Voice priority
    components/decomp/src/libultra/n_audio/n_synsetvol.c        # Voice volume
    components/decomp/src/libultra/n_audio/n_synstartvoiceparam.c # Voice start with params
    components/decomp/src/libultra/n_audio/n_synstopvoice.c     # Voice stop
    components/decomp/src/libultra/n_audio/n_cspplay.c          # CSP play
    components/decomp/src/libultra/n_audio/n_cspsetbank.c       # CSP set bank
    components/decomp/src/libultra/n_audio/n_cspsetfxmix.c      # CSP set FX mix
    components/decomp/src/libultra/n_audio/n_cspsetpriority.c   # CSP set priority
    components/decomp/src/libultra/n_audio/n_cspsetseq.c        # CSP set sequence
    components/decomp/src/libultra/n_audio/n_cspsetvol.c        # CSP set volume
    components/decomp/src/libultra/n_audio/n_cspstop.c          # CSP stop
    components/decomp/src/libultra/n_audio/n_seq.c              # Sequence data reader
    components/decomp/src/libultra/n_audio/n_seqplayer.c        # Sequence player voice handler
    components/decomp/src/libultra/n_audio/n_seqpgetchlvol.c    # Get channel volume
    components/decomp/src/libultra/n_audio/n_seqpgetvol.c       # Get master volume
    components/decomp/src/libultra/audio/cents2ratio.c          # alCents2Ratio — pitch conversion
)
target_sources(ssb64_game PRIVATE ${SSB64_NAUDIO_PORT})

target_compile_definitions(ssb64_game PRIVATE ${SSB64_COMPILE_DEFS})

target_include_directories(ssb64_game PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include          # Port-tree generated headers (reloc_data.h)
    ${CMAKE_CURRENT_SOURCE_DIR}/components/decomp/include   # Decomp C headers (shadows system headers)
    ${CMAKE_CURRENT_SOURCE_DIR}/components/decomp/src       # Decomp game module headers
    ${CMAKE_CURRENT_SOURCE_DIR}/src            # src/<module>/x.h prefixed includes
    ${CMAKE_CURRENT_SOURCE_DIR}/src/application
    ${CMAKE_CURRENT_SOURCE_DIR}/src/application/update
    ${CMAKE_CURRENT_SOURCE_DIR}/src/application/update/updater
    ${CMAKE_CURRENT_SOURCE_DIR}/src/application/update/downloads
    ${CMAKE_CURRENT_SOURCE_DIR}/src/application/update/integrity
    ${CMAKE_CURRENT_SOURCE_DIR}/src/application/update/manifests
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config
    ${CMAKE_CURRENT_SOURCE_DIR}/src/i18n
    ${CMAKE_CURRENT_SOURCE_DIR}/src/observability
    ${CMAKE_CURRENT_SOURCE_DIR}/src/persistence
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/engine/decomp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/engine/hooks
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/modding
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/gameplay
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/input
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/presentation
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/rendering
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/rendering/widescreen
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/rendering/cosmetics
    ${CMAKE_CURRENT_SOURCE_DIR}/src/networking
    ${CMAKE_CURRENT_SOURCE_DIR}/src/networking/netplay
    ${CMAKE_CURRENT_SOURCE_DIR}/src/networking/netplay/domain
    ${CMAKE_CURRENT_SOURCE_DIR}/src/networking/netplay/session
    ${CMAKE_CURRENT_SOURCE_DIR}/src/networking/netplay/transport
    ${CMAKE_CURRENT_SOURCE_DIR}/src/networking/netplay/rollback
    ${CMAKE_CURRENT_SOURCE_DIR}/src/networking/social
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/engine/relocation
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/rendering/interpolation
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/audio/n64
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/rendering/shaders
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/engine/css_icons
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/rendering/textures/hires
    ${CMAKE_BINARY_DIR}/generated/include/us   # reloc_data.us.h selector target
    ${CMAKE_BINARY_DIR}/generated/include/jp
    ${CMAKE_BINARY_DIR}/generated/src/us      # RelocFileTable.us.h selector target
    ${CMAKE_BINARY_DIR}/generated/src/jp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/coroutine
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/http
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/filesystem
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/window
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/android
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/compatibility
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/debugging      # Acmd trace (mixer.h calls acmd_trace_log_cmd)
    # Event system headers: decomp PORT hooks fire engine events directly
    # (hooks/Events.h -> ship/events/EventTypes.h + bridge/eventsbridge.h).
    ${CMAKE_CURRENT_SOURCE_DIR}/components/libultraship/src
    ${CMAKE_CURRENT_SOURCE_DIR}/components/libultraship/include
)

# Clang 16+ promoted several historically-warning diagnostics to errors by
# default, which breaks the decomp C code on macOS/Linux. The decomp was
# written against IDO 7.1 and relies on implicit function declarations, loose
# int/pointer conversions, and incompatible library redeclarations — all of
# which MSVC merely warns about. Downgrade those here so the port can build
# without modifying every decomp source file.
if (NOT MSVC)
    # Common warnings that both Clang and GCC accept.
    target_compile_options(ssb64_game PRIVATE
        -Werror=implicit-function-declaration
        -Wno-implicit-int
        -Werror=int-conversion
        -Werror=incompatible-pointer-types
        -Wno-shift-negative-value
        -Wno-pointer-sign
        -Wno-unused-value
        -Wno-unused-variable
        -Wno-unused-but-set-variable
        -Wno-unused-function
        -Werror=return-type
    )
    # Clang-only diagnostics. -Wincompatible-library-redeclaration is
    # Clang-specific; -Wreturn-mismatch / -Wconstant-conversion /
    # -Wtautological-constant-out-of-range-compare exist in modern Clang
    # but only landed in GCC 14+ (or never).
    if (CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(ssb64_game PRIVATE
            -Werror=incompatible-library-redeclaration
            -Werror=return-mismatch
            -Wno-parentheses-equality
            -Wno-constant-conversion
            -Wno-tautological-constant-out-of-range-compare
        )
    endif()
endif()

################################################################################
# Fetch Discord RPC (desktop only — Android and Switch drop Discord Rich
# Presence along with the rest of the network-dependent features).
################################################################################
if (NOT CMAKE_SYSTEM_NAME STREQUAL "Android" AND NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch" AND NOT BATTLESHIP_UWP)
    # discord-rpc v3.4.0's CMakeLists declares cmake_minimum_required(VERSION 2.x),
    # which CMake 3.30+ refuses to honor. Force a baseline policy version for the
    # fetched subdir so the build still configures with current CMake.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE INTERNAL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    # BattleShip links against the static CRT on Windows (see the
    # MSVC_RUNTIME_LIBRARY setting on the ssb64 / ssb64_game targets below).
    # discord-rpc's own CMakeLists guards the static-CRT switch behind
    # USE_STATIC_CRT — opt in so the produced discord-rpc.lib uses the same
    # /MT[d] runtime, otherwise the final link fails with LNK2038.
    set(USE_STATIC_CRT ON CACHE BOOL "" FORCE)
    # The bundled rapidjson-1.1.0 declares an ill-formed GenericStringRef
    # operator= (assigns to a const member); clang 22+ rejects it outright.
    # Upstream rapidjson removed the operator (rapidjson#1364) — drop the
    # line at fetch time the same way.
    # NOTE: the rapidjson patch script must NOT be a PATCH_COMMAND here.
    # It would be ineffective anyway (rapidjson is downloaded later by
    # discord-rpc's own configure — see the execute_process below), and
    # FetchContent serializes PATCH_COMMAND args into a generated CMake
    # file where a backslashed Windows ${Python3_EXECUTABLE} path becomes
    # an invalid escape sequence, failing the whole configure on MSVC CI.
    FetchContent_Declare(
        discord_rpc
        GIT_REPOSITORY https://github.com/discordapp/discord-rpc.git
        GIT_TAG        e6390c8c4190b59f1cc98bcec688e0037b84974d # v3.4.0
        PATCH_COMMAND  ${CMAKE_COMMAND} -E rm -f .clang-format
    )
    FetchContent_MakeAvailable(discord_rpc)
    # rapidjson is downloaded by discord-rpc's own configure (inside
    # MakeAvailable), after the FetchContent patch step ran — apply the
    # clang-22 compat patch now that the file exists.
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build/patch-discord-rapidjson.py
        WORKING_DIRECTORY ${discord_rpc_SOURCE_DIR}
        COMMAND_ERROR_IS_FATAL ANY)
    # Belt and suspenders: enforce the runtime-library property on the
    # produced target directly so it survives a future cmake_policy(CMP0091)
    # propagation behavior change.
    if(MSVC AND TARGET discord-rpc)
        set_property(TARGET discord-rpc PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    # Suppress GCC 14 strict template errors in discord-rpc's bundled RapidJSON
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "14.0")
        target_compile_options(discord-rpc PRIVATE -Wno-template-body)
    endif()
endif()

################################################################################
# Executable target — port C++ code + decomp object library
################################################################################

if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    # SDLActivity (Java) loads a shared library and calls SDL_main from it
    # via dlsym. Output name `main` matches SDL2's default `libmain.so`
    # lookup. Both ARM coroutine asm shims are pulled in here so the SHARED
    # target builds with the .S preprocessed by clang; each file's body is
    # #if-guarded on the target arch (__aarch64__ / __arm__), so only the
    # one matching ANDROID_ABI emits code.
    add_library(${PROJECT_NAME} SHARED
        ${SSB64_SRC_PORT_TOPLEVEL}
        ${SSB64_MODULE_OBJECTS}
        ${SSB64_DEBUG_TOOLS}
        $<TARGET_OBJECTS:ssb64_game>
        src/platform/coroutine/coroutine_aarch64.S
        src/platform/coroutine/coroutine_armv7.S
    )
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "main")
elseif(BATTLESHIP_UWP)
    # The UWP CoreApplication executable is a tiny wrapper in uwp/. Keep the
    # existing game entrypoint in a DLL and export SDL_main for that wrapper.
    add_library(${PROJECT_NAME} SHARED
        ${SSB64_SRC_PORT_TOPLEVEL}
        ${SSB64_MODULE_OBJECTS}
        ${SSB64_DEBUG_TOOLS}
        $<TARGET_OBJECTS:ssb64_game>
    )
    set_target_properties(${PROJECT_NAME} PROPERTIES
        OUTPUT_NAME "BattleShip"
        WINDOWS_EXPORT_ALL_SYMBOLS TRUE
    )
else()
    add_executable(${PROJECT_NAME} ${SSB64_SRC_PORT_TOPLEVEL} ${SSB64_MODULE_OBJECTS} ${SSB64_DEBUG_TOOLS} $<TARGET_OBJECTS:ssb64_game>)

    # User-facing binary name. The CMake target keeps the historical
    # `ssb64` identifier (matches the repo, build dirs, internal symbols),
    # but the produced executable is `${SSB64_APP_NAME}` — `BattleShip`
    # (US) or `BattleShip-JP` (JP), so the two are distinct applications.
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${SSB64_APP_NAME}")
endif()
ssb64_apply_static_analysis(${PROJECT_NAME})

# Share the runtime destination with the staging prerequisite. Referencing
# TARGET_FILE_DIR here creates a back-edge with Visual Studio even under
# CMP0112 NEW. Explicit paths keep both generators' dependency graphs acyclic.
# Preserve the normal <build>/<config> multi-config output layout and any
# output-directory overrides supplied by the caller.
set(SSB64_RUNTIME_DIR "${CMAKE_CURRENT_BINARY_DIR}")
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(_ssb64_output_property LIBRARY_OUTPUT_DIRECTORY)
else()
    set(_ssb64_output_property RUNTIME_OUTPUT_DIRECTORY)
endif()
get_property(_ssb64_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
get_target_property(_ssb64_output_dir ${PROJECT_NAME} ${_ssb64_output_property})
if(_ssb64_output_dir)
    set(SSB64_RUNTIME_DIR "${_ssb64_output_dir}")
endif()
if(_ssb64_multi_config)
    if(NOT SSB64_RUNTIME_DIR MATCHES "\\$<")
        string(APPEND SSB64_RUNTIME_DIR "/$<CONFIG>")
    endif()
    foreach(_ssb64_config IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${_ssb64_config}" _ssb64_config_upper)
        get_target_property(_ssb64_config_dir ${PROJECT_NAME}
            ${_ssb64_output_property}_${_ssb64_config_upper})
        if(_ssb64_config_dir)
            set(SSB64_RUNTIME_DIR
                "$<IF:$<CONFIG:${_ssb64_config}>,${_ssb64_config_dir},${SSB64_RUNTIME_DIR}>")
        endif()
    endforeach()
endif()
# Normalize relative user overrides the same way as the target properties.
set(SSB64_RUNTIME_DIR "$<PATH:ABSOLUTE_PATH,${SSB64_RUNTIME_DIR},${CMAKE_CURRENT_BINARY_DIR}>")

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set_target_properties(ssb64_game PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    # The smash_* OBJECT libraries compile with the toolchain default (/MD)
    # unless told otherwise; mixing them into the static-CRT binary leaves
    # __imp_ externals unresolved (LNK2019). Same runtime for every module.
    # MSBuild runs several cl.exe instances in parallel inside a project;
    # without /FS they all write the project PDB unserialized and lose the
    # race intermittently (C1041 "cannot open program database").
    if(MSVC)
        target_compile_options(ssb64_game PRIVATE /FS)
        target_compile_options(${PROJECT_NAME} PRIVATE /FS)
        foreach(mod IN LISTS SSB64_MODULE_DIRS)
            if(TARGET smash_${mod})
                set_target_properties(smash_${mod} PROPERTIES
                    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
                target_compile_options(smash_${mod} PRIVATE /FS)
            endif()
        endforeach()
        # Vendored deps default to /MD too — one CRT for everything that
        # links into the exe, or we get LNK4098 and split heaps.
        foreach(vendored IN ITEMS
            libultraship funchook-static distorm discord-rpc prism
            glslang glslang-default-resource-limits
            GenericCodeGen MachineIndependent OSDependent SPIRV
            spirv-cross-core spirv-cross-glsl spirv-cross-hlsl spirv-cross-msl
            hidapi hidapi_winapi ed25519_verify
            tcc tcc1 libtcc libtcc1 ImGui stb usb-1.0)
            if(TARGET ${vendored})
                get_target_property(_vendored_aliased ${vendored} ALIASED_TARGET)
                if(NOT _vendored_aliased)
                    set_target_properties(${vendored} PROPERTIES
                        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
                    target_compile_options(${vendored} PRIVATE /FS)
                endif()
                unset(_vendored_aliased)
            endif()
        endforeach()
    else()
        foreach(mod IN LISTS SSB64_MODULE_DIRS)
            if(TARGET smash_${mod})
                set_target_properties(smash_${mod} PROPERTIES
                    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
            endif()
        endforeach()
    endif()
endif()

################################################################################
# Compile definitions
################################################################################

target_compile_definitions(ssb64_port_options INTERFACE ${SSB64_COMPILE_DEFS})
target_link_libraries(${PROJECT_NAME} PRIVATE ssb64_port_options)

################################################################################
# Include paths (no include/ here — port code uses system headers)
################################################################################

# Every src/ subtree dir, in one flat set — the legacy include surface for
# modules not yet covered by an SSB64_MODULE_SCOPE_* entry. Scoped modules
# do NOT link this: a forbidden cross-module include is then a compile
# error in the module lib itself (see SSB64_MODULE_SCOPE_* above).
add_library(ssb64_src_flat_includes INTERFACE)
target_include_directories(ssb64_src_flat_includes INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/application
    ${CMAKE_CURRENT_SOURCE_DIR}/src/application/update
    ${CMAKE_CURRENT_SOURCE_DIR}/src/application/update/updater
    ${CMAKE_CURRENT_SOURCE_DIR}/src/application/update/downloads
    ${CMAKE_CURRENT_SOURCE_DIR}/src/application/update/integrity
    ${CMAKE_CURRENT_SOURCE_DIR}/src/application/update/manifests
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config
    ${CMAKE_CURRENT_SOURCE_DIR}/src/i18n
    ${CMAKE_CURRENT_SOURCE_DIR}/src/observability
    ${CMAKE_CURRENT_SOURCE_DIR}/src/persistence
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/engine/decomp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/engine/hooks
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/modding
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/gameplay
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/audio
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/input
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/presentation
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/rendering
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/rendering/widescreen
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/rendering/cosmetics
    ${CMAKE_CURRENT_SOURCE_DIR}/src/networking
    ${CMAKE_CURRENT_SOURCE_DIR}/src/networking/netplay
    ${CMAKE_CURRENT_SOURCE_DIR}/src/networking/netplay/domain
    ${CMAKE_CURRENT_SOURCE_DIR}/src/networking/netplay/session
    ${CMAKE_CURRENT_SOURCE_DIR}/src/networking/netplay/transport
    ${CMAKE_CURRENT_SOURCE_DIR}/src/networking/netplay/rollback
    ${CMAKE_CURRENT_SOURCE_DIR}/src/networking/social
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/engine/relocation
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/rendering/interpolation
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/audio/n64
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/rendering/shaders
    ${CMAKE_CURRENT_SOURCE_DIR}/src/game/engine/css_icons
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interfaces/rendering/textures/hires
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/coroutine
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/http
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/filesystem
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/window
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/android
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/compatibility
)
# Non-module sources (files sitting directly under src/ or its group
# roots) compile inside the executable target — it keeps the flat set.
target_link_libraries(${PROJECT_NAME} PRIVATE ssb64_src_flat_includes)

target_include_directories(ssb64_port_options INTERFACE
    # NOTE: Do NOT add components/decomp/include here. The C decomp shim headers (stdlib.h,
    # stddef.h, ...) shadow the C++ standard library when picked up by src/*.cpp,
    # producing libc++ <cstdlib> errors. Only ssb64_game (C decomp) consumes them.
    ${CMAKE_CURRENT_SOURCE_DIR}/components/decomp/src       # Decomp game module headers (sys/, ft/, sc/, ...)
    ${CMAKE_CURRENT_SOURCE_DIR}/include          # Public port headers (smash_classic/)
    ${CMAKE_BINARY_DIR}/generated/include/us   # reloc_data.us.h selector target
    ${CMAKE_BINARY_DIR}/generated/include/jp
    ${CMAKE_BINARY_DIR}/generated/src/us      # RelocFileTable.us.h selector target
    ${CMAKE_BINARY_DIR}/generated/src/jp
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/debugging
    ${SSB64_CSS_GEN_DIR}                         # CSS icon baked headers (arrow_data.h)
)
if (NOT CMAKE_SYSTEM_NAME STREQUAL "Android" AND NOT BATTLESHIP_UWP)
    target_include_directories(ssb64_port_options INTERFACE
        ${SSB64_ICON_GEN_DIR}                         # Generated icon header (port_icon_data.h)
        ${discord_rpc_SOURCE_DIR}/include             # Discord Rich Presence
    )
else()
    # Android: include the generated dir for CSS icon baked headers.
    # (SSB64_ICON_GEN_DIR is already the same dir; listed explicitly for clarity.)
    target_include_directories(ssb64_port_options INTERFACE
        ${SSB64_CSS_GEN_DIR}                          # CSS icon baked headers
    )
endif()

################################################################################
# Link libultraship and Torch
################################################################################

if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    add_dependencies(${PROJECT_NAME} libultraship)
    target_link_libraries(${PROJECT_NAME} PRIVATE libultraship)
elseif (CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    # Switch has no host-side TorchExternal — assets ship pre-extracted.
    add_dependencies(${PROJECT_NAME} libultraship)
    target_link_libraries(${PROJECT_NAME} PRIVATE libultraship)
elseif(BATTLESHIP_UWP)
    add_dependencies(${PROJECT_NAME} libultraship TorchExternal)
    target_link_libraries(${PROJECT_NAME} PRIVATE libultraship SDL2 LIBUWP GLFW3)
else()
    add_dependencies(${PROJECT_NAME} libultraship TorchExternal discord-rpc)
    target_link_libraries(${PROJECT_NAME} PRIVATE libultraship discord-rpc)
    target_link_libraries(ssb64_port_options INTERFACE discord-rpc)
endif()
# Engine/dep usage requirements for every port TU (OBJECT libraries get no
# include dirs from the consuming executable — they inherit them through
# the shared options interface).
target_link_libraries(ssb64_port_options INTERFACE libultraship)
# Codegen ordering: reloc tables/credits headers exist before any module TU.
foreach(mod IN LISTS SSB64_MODULE_DIRS)
    if(TARGET smash_${mod})
        add_dependencies(smash_${mod} libultraship)
    endif()
endforeach()
if (WIN32)
    # src/observability/port_symbolize.cpp (SyncTest diagnostics), src/networking/netplay/transport/udp_transport.cpp, src/networking/netplay/transport/websocket_transport.cpp
    # sapi: §9 menu narration (src/interfaces/presentation/narration.cpp)
    target_link_libraries(${PROJECT_NAME} PRIVATE dbghelp ws2_32 winhttp bcrypt sapi ole32)
endif()

# In-process zip extraction (src/platform/filesystem/zip_extract.cpp). libzip is
# already REQUIRED by libultraship on every target — vcpkg/system on
# desktop, FetchContent'd on Android — so this only adds a direct link
# for the executable, no new dependency.
find_package(libzip REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE libzip::zip)
target_link_libraries(ssb64_port_options INTERFACE libzip::zip)

# TD-009 — offline-signed release manifest verification. orlp/ed25519 is the
# small public-domain RFC 8032 implementation; only the verify path is
# compiled (releases are signed with `minisign -l`, raw Ed25519 — see
# docs/development/release_signing.md). The pubkey comes from
# config/release_pubkey.hex via BATTLESHIP_RELEASE_PUBKEY.
include(FetchContent)
FetchContent_Declare(ed25519
    GIT_REPOSITORY https://github.com/orlp/ed25519.git
    GIT_TAG        b1f19fab4aebe607805620d25a5e42566ce46a0e)
FetchContent_GetProperties(ed25519)
if(NOT ed25519_POPULATED)
    FetchContent_Populate(ed25519)
endif()
add_library(ed25519_verify STATIC
    ${ed25519_SOURCE_DIR}/src/fe.c
    ${ed25519_SOURCE_DIR}/src/ge.c
    ${ed25519_SOURCE_DIR}/src/sc.c
    ${ed25519_SOURCE_DIR}/src/sha512.c
    ${ed25519_SOURCE_DIR}/src/verify.c)
target_include_directories(ed25519_verify PUBLIC ${ed25519_SOURCE_DIR}/src)
if (MSVC)
    # Created after the vendored-CRT foreach above — same static runtime.
    set_target_properties(ed25519_verify PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE ed25519_verify)
target_link_libraries(ssb64_port_options INTERFACE ed25519_verify)

add_dependencies(ssb64_game GenerateCreditsAssets)
if (NOT DISABLE_SCRIPTING)
    # funchook hook backend (libultraship is already linked above).
    target_link_libraries(${PROJECT_NAME} PRIVATE funchook-static)
    target_link_libraries(ssb64_port_options INTERFACE funchook-static)

    ############################################################################
    # Mod-system linkage requirements
    #
    # 1. Disable function inlining on the main executable so engine
    #    functions remain resolvable + hookable by name. Without this, the
    #    optimizer can collapse a hookable function into its caller and
    #    eliminate the address mods would target. /Ob0 (MSVC) and
    #    -fno-inline-functions (GCC/Clang) preserve every function as a
    #    standalone symbol.
    #
    #    Cost: marginal. The decomp is already non-matching for the port
    #    build and the renderer is the perf bottleneck, not game logic.
    #
    # 2. Ensure PDB (Windows) / debug symbols (Linux/macOS) are produced
    #    for Release builds too, since symbol resolution depends on them.
    ############################################################################
    if (MSVC)
        target_compile_options(ssb64_port_options INTERFACE /Ob0)
        target_compile_options(ssb64_game         PRIVATE /Ob0)
        # NOTE: do NOT add /OPT:REF or /OPT:ICF here. /OPT:REF strips
        # unreferenced symbols (mods reach in by-name even when nothing
        # in the binary references the function); /OPT:ICF folds
        # identical code sections (two functions with the same body
        # collapse to one symbol, breaking hook-by-name when two ports
        # of the same function exist). Plain /DEBUG is enough; it just
        # tells the linker to emit a full PDB.
    else()
        target_compile_options(ssb64_port_options INTERFACE -fno-inline-functions)
        target_compile_options(ssb64_game         PRIVATE -fno-inline-functions)
    endif()
endif()

# Debug info for EVERY build — crash-bundle symbolization (TD-015) must
# work on script-disabled builds too (the JP Windows release disables
# scripting). /Zi + /DEBUG emits a PDB per config; -g carries DWARF.
# The shipped binary is unaffected — packaging splits the debug payload
# into the per-release symbols archive.
if (MSVC)
    target_compile_options(ssb64_port_options INTERFACE /Zi)
    target_compile_options(ssb64_game         PRIVATE /Zi)
    target_link_options(${PROJECT_NAME}    PRIVATE /DEBUG)
else()
    target_compile_options(ssb64_port_options INTERFACE -g)
    target_compile_options(ssb64_game         PRIVATE -g)
endif()

################################################################################
# Platform-specific settings — per-platform files under platforms/<os>/cmake/,
# each internally guarded (safe to include on every host).
#   Windows.cmake — subsystem/entry point, .rc icon embed, shell32
#   Linux.cmake   — -Wl,-export-dynamic
#   Switch.cmake  — devkitPro toolchain, .nro packaging, BundleSwitch
#   macOS.cmake   — CoreFoundation link, Mach-O rwx + ad-hoc re-sign
################################################################################
include(${CMAKE_CURRENT_SOURCE_DIR}/platforms/windows/cmake/Windows.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/platforms/linux/cmake/Linux.cmake)

################################################################################
# TCC mod scripting: export the engine's full symbol table so libtcc-compiled
# mods can resolve any engine function by name. On Windows this is what makes
# `tcc.exe -impdef BattleShip.exe -o BattleShip.def` produce a useful export
# list for ScriptLoader's memory-mode symbol registration; on Unix the linker
# default-visibility + -Wl,-export-dynamic above handles it.
################################################################################
if(NOT DISABLE_SCRIPTING)
    # ENABLE_EXPORTS makes the EXE produce an import library (.lib) and
    # surface its symbols to runtime-loaded modules. Without it
    # WINDOWS_EXPORT_ALL_SYMBOLS only generates a .def file but the linker
    # doesn't actually emit an export table on the EXE - the .exe ends up
    # without an .edata section and tcc -impdef returns "no symbols found".
    set_target_properties(${PROJECT_NAME} PROPERTIES
        ENABLE_EXPORTS TRUE
        WINDOWS_EXPORT_ALL_SYMBOLS TRUE
    )
endif()

# Runtime data staging (f3d.o2r, gamecontrollerdb, fonts, config.yml, extraction yamls).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/RuntimeAssets.cmake)

################################################################################
# TCC mod scripting: post-build .tcc/ population + BattleShip.def generation
#
# At runtime the engine's ScriptLoader feeds mod source through libtcc with
# include paths under .tcc/include/ and library paths under .tcc/lib/. We
# populate both from the FetchContent'd TinyCC source tree so the binary is
# self-contained; modders don't have to install TinyCC.
#
# On Windows we additionally run `tcc.exe -impdef BattleShip.exe -o
# BattleShip.def` to generate the export-name list that ScriptLoader resolves
# against the running process before TCC relocates mod code. WINDOWS_EXPORT_ALL_SYMBOLS
# (set above) makes the EXE's export table contain every engine function so
# the .def covers them all.
################################################################################
if(NOT DISABLE_SCRIPTING)
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMENT "Copying libtcc headers and runtime libraries..."
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/"
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/lib/"
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/include/"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/_deps/tinycc-src/include/"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/include/"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/_deps/tinycc-src/win32/include/"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/include/"
            # libtcc1.a built by MSVC is COFF format which TCC can't
            # parse. Use tcc.exe itself to recompile the runtime source
            # into a TCC-readable archive. -B points tcc at .tcc/ for
            # tccdefs.h (auto-included via the command line).
            COMMAND "$<TARGET_FILE:tcc>"
                -B "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc"
                -c "${CMAKE_BINARY_DIR}/_deps/tinycc-src/lib/libtcc1.c"
                -o "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/lib/libtcc1.o"
            COMMAND "$<TARGET_FILE:tcc>" -ar rcs
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/lib/libtcc1.a"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/lib/libtcc1.o"
            COMMAND ${CMAKE_COMMAND} -E rm -f
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/lib/libtcc1.o"
            VERBATIM
        )
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMENT "Generating BattleShip.def via tcc -impdef..."
            COMMAND "$<TARGET_FILE:tcc>"
                    -impdef "$<TARGET_FILE:${PROJECT_NAME}>"
                    -o "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/lib/BattleShip.def"
            VERBATIM
        )
        add_dependencies(${PROJECT_NAME} tcc)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMENT "Copying libtcc headers and libs..."
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/"
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/lib/"
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/include/"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/_deps/tinycc-src/include/"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/include/"
            COMMAND ${CMAKE_COMMAND} -E copy
                "$<TARGET_FILE:libtcc1>"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/lib/libtcc1.a"
            VERBATIM
        )
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMENT "Copying libtcc headers and libs..."
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/"
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/lib/"
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/include/"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/_deps/tinycc-src/include/"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/include/"
            COMMAND ${CMAKE_COMMAND} -E copy
                "$<TARGET_FILE:libtcc1>"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/.tcc/lib/libtcc1.a"
            VERBATIM
        )
    endif()
endif()

################################################################################
# Windows: stage the vcpkg runtime DLLs next to the binary. Under the VS
# generator vcpkg does this via applocal.ps1; Ninja (and other non-VS
# generators) get a bare executable that fails to launch with
# STATUS_DLL_NOT_FOUND (0xC0000135), so copy them explicitly here.
################################################################################
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMENT "Staging vcpkg runtime DLLs next to the executable..."
        COMMAND ${CMAKE_COMMAND}
            "-DSRC=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<$<CONFIG:Debug>:debug/>bin"
            "-DDST=$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyDirIfExists.cmake"
        VERBATIM
    )
endif()

################################################################################
# macOS: JIT-entitlement re-sign for the mod hook backend. Defined in
# platforms/macos/cmake/macOS.cmake; registered here so it remains the LAST
# POST_BUILD step (any later binary mutation would invalidate the
# signature).
################################################################################
include(${CMAKE_CURRENT_SOURCE_DIR}/platforms/macos/cmake/macOS.cmake)

################################################################################
# Asset extraction targets (desktop only — Android extracts on-device from
# the user's ROM via libtorch_runner.so at first launch; see
# src/platform/android/android_torch_bridge.cpp + RomImporter.java).
# Switch likewise has no host-side Torch here — the .o2r is pre-extracted
# on a PC and bundled onto the SD card (BundleSwitch target below).
################################################################################

if (NOT CMAKE_SYSTEM_NAME STREQUAL "Android" AND NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
# Accept .z64 (big-endian), .n64 (little-endian), or .v64 (byteswapped).
# Torch's Cartridge::Normalize converts non-z64 byte orders in-memory.
# Resolution order: -DSSB64_BASEROM=<path> cache var, $SMASH_BASEROM env,
# then roms/baserom.<version>.{z64,n64,v64}, then the same names at the
# repo root (legacy location, still supported).
set(SSB64_BASEROM "")
if(DEFINED ENV{SMASH_BASEROM} AND EXISTS "$ENV{SMASH_BASEROM}")
    set(SSB64_BASEROM "$ENV{SMASH_BASEROM}")
endif()
if(NOT SSB64_BASEROM)
    foreach(dir "${CMAKE_SOURCE_DIR}/roms" "${CMAKE_SOURCE_DIR}")
        foreach(ext z64 n64 v64)
            if(EXISTS "${dir}/baserom.${SSB64_VERSION}.${ext}")
                set(SSB64_BASEROM "${dir}/baserom.${SSB64_VERSION}.${ext}")
                break()
            endif()
        endforeach()
        if(SSB64_BASEROM)
            break()
        endif()
    endforeach()
endif()
if(NOT SSB64_BASEROM)
    set(SSB64_BASEROM "${CMAKE_SOURCE_DIR}/baserom.${SSB64_VERSION}.z64")
endif()

# BattleShip.o2r is per-ROM-version. The decomp is region-split, so US and
# JP use separate build dirs — extracting into the SHARED source root made a
# US build and a JP build clobber each other's archive. Extract into a
# per-build-dir sink (${CMAKE_BINARY_DIR}/extracted) instead, then stage it
# next to the binary. Torch writes config.yml's `output.binary`
# (BattleShip.o2r, identical for both regions) into its `-d` dir,
# independent of `-s` (where it reads config/yamls). The staged runtime
# name follows SSB64_APP_NAME (BattleShip.o2r or BattleShip-JP.o2r), while
# the extracted build artifact stays fixed. Packaged-release first-run
# extraction (sidecar torch + config.yml, its own -d) is unaffected.
set(SSB64_O2R_DIR ${CMAKE_BINARY_DIR}/extracted)
set(SSB64_O2R     ${SSB64_O2R_DIR}/BattleShip.o2r)

# Two configurations:
#
#   (a) Baserom present (typical dev / Torch-extract first-run): declare the
#       o2r as an add_custom_command(OUTPUT ...) so downstream rules that
#       DEPENDS on it as a file connect to this producer through CMake's
#       file-level dependency graph. The wrapping custom_target keeps
#       `cmake --build . --target ExtractAssets` working.
#
#   (b) Baserom absent (CI release pipeline never ships the copyrighted ROM):
#       provide a stub ExtractAssets target that fails with a clear error if
#       invoked, but does NOT register the file-level rule. Any caller that
#       genuinely needs the o2r will hit ExtractAssets and get the
#       explanatory error.
# Torch resolves config.yml's `path: yamls/<region>` relative to its `-s`
# dir. ssb64_stage_asset_recipes() (called during configure) stages
# config.yml + the recursive yamls tree into ${SSB64_RECIPE_DIR} so torch
# reads a self-contained recipe dir that mirrors what first_run.cpp stages
# at runtime, and recipe.sha1 (file(GENERATE) above) records the content
# hash the runtime compares against SSB64_ASSET_RECIPE_HASH.

if(EXISTS "${SSB64_BASEROM}")
    add_custom_command(
        OUTPUT ${SSB64_O2R}
        BYPRODUCTS ${SSB64_O2R}.recipe
        # SSB64_RECIPE_YAMLS + config.yml: editing any extraction recipe
        # (child slice maps included) re-extracts — previously a yaml edit
        # silently left a stale archive.
        DEPENDS TorchExternal ${TORCH_EXECUTABLE} ${SSB64_BASEROM}
                ${SSB64_RECIPE_INPUTS} ${SSB64_RECIPE_DIR}/recipe.sha1
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SSB64_O2R_DIR}
        COMMAND ${TORCH_EXECUTABLE} o2r ${SSB64_BASEROM} -s ${SSB64_RECIPE_DIR} -d ${SSB64_O2R_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SSB64_RECIPE_DIR}/recipe.sha1 ${SSB64_O2R}.recipe
        COMMENT "Extracting ${SSB64_VERSION} assets from ${SSB64_BASEROM} -> ${SSB64_O2R}"
        VERBATIM
    )
    add_custom_target(ExtractAssets DEPENDS ${SSB64_O2R})
    # Deblob validation gate (docs/deblob.md): builds every deblobbed
    # bundle through the real archive/factory/synthesis path and fails on
    # any I5/I7/I9 violation. Mandatory local pre-merge for changes to
    # components/game-runtime/, the extraction recipes, or the synthesis code (CI has no
    # staged archive). Runs headless-ish (exits before the game boots; a
    # window may flash).
    add_custom_target(synth_verify
        COMMAND ${CMAKE_COMMAND} -E env SSB64_SYNTH_SELFTEST=1 $<TARGET_FILE:${PROJECT_NAME}>
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DEPENDS ${PROJECT_NAME}
        COMMENT "deblob synth_verify: synthesizing every deblobbed bundle (I5 gate)"
        USES_TERMINAL
    )
    # Baserom present (dev build): make the normal build also stage the
    # o2r next to the binary, so the flow is a true 3 commands —
    # configure, build, run — with no separate ExtractAssets step. The
    # o2r is an OUTPUT custom command, so it only re-extracts when its
    # inputs (torch / recipes / baserom) change; cached otherwise.
    # The release pipeline ships no baserom and hits the else() branch,
    # so packaged builds are unaffected (runtime first-run extracts).
    add_dependencies(${PROJECT_NAME} ExtractAssets)
else()
    add_custom_target(ExtractAssets
        COMMAND ${CMAKE_COMMAND} -E echo
            "ERROR: ExtractAssets requires baserom.${SSB64_VERSION}.{z64,n64,v64} in source root (${CMAKE_SOURCE_DIR})"
        COMMAND ${CMAKE_COMMAND} -E false
        COMMENT "ExtractAssets stub — baserom missing"
    )
    message(STATUS "BattleShip.o2r: baserom absent — ExtractAssets stub installed")
endif()

# Staging must run even when only Torch/recipes changed and ssb64 did not
# relink. Use the shared explicit runtime directory, without a reference
# back to the target that depends on this staging step.
add_custom_target(StageRuntimeAssets
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DDESTINATION_DIR=${SSB64_RUNTIME_DIR}
        -DVERSION=${SSB64_VERSION}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageAssetRecipes.cmake
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${TORCH_EXECUTABLE} ${SSB64_RUNTIME_DIR}/${TORCH_SIDECAR_NAME}
    DEPENDS TorchExternal
    COMMENT "Staging extraction recipes and Torch next to the game"
    VERBATIM)
if(EXISTS "${SSB64_BASEROM}")
    add_dependencies(StageRuntimeAssets ExtractAssets)
    add_custom_command(TARGET StageRuntimeAssets POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SSB64_O2R} ${SSB64_RUNTIME_DIR}/${SSB64_APP_NAME}.o2r
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SSB64_O2R}.recipe ${SSB64_RUNTIME_DIR}/${SSB64_APP_NAME}.o2r.recipe
        VERBATIM)
endif()
add_dependencies(${PROJECT_NAME} StageRuntimeAssets)

add_custom_target(GenerateBattleShipO2R
    DEPENDS ExtractAssets
    COMMENT "Generating BattleShip.o2r"
)

# ── CSS stage asset extraction ─────────────────────────────────────────────
# derive_stage_assets.py extracts the FD background + icon PNGs from the ROM,
# and synthesizes the FD nameplate PNG (96x10, PIL bitmap font, no ROM data).
# These PNGs are then copied to the run directory so the game can load them
# at runtime without any Nintendo-derived bytes baked into the binary.
#
# Desktop-only.  Android is a follow-up task (stage PNGs need to be packaged
# into the APK assets/ and extracted to externalFilesDir on first launch).
# derive_stage_assets.py currently understands the US ROM layout only.
# This affects three additional CSS stage images, not the main game assets.
# When building US, honor the resolved ROM path (incl. SMASH_BASEROM) if it
# is .z64 — the script reads big-endian fields and does not byteswap.
# JP builds fall back to a US baserom (roms/ first, then the legacy root).
if(SSB64_VERSION STREQUAL "us" AND SSB64_BASEROM MATCHES "\\.z64$"
   AND EXISTS "${SSB64_BASEROM}")
    set(SSB64_CSS_ASSET_ROM "${SSB64_BASEROM}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/roms/baserom.us.z64")
    set(SSB64_CSS_ASSET_ROM "${CMAKE_CURRENT_SOURCE_DIR}/roms/baserom.us.z64")
else()
    set(SSB64_CSS_ASSET_ROM "${CMAKE_CURRENT_SOURCE_DIR}/baserom.us.z64")
endif()
if(EXISTS "${SSB64_CSS_ASSET_ROM}")
    add_custom_command(
        OUTPUT
            ${SSB64_CSS_FD_BG_PNG}
            ${SSB64_CSS_FD_SMALL_PNG}
            ${SSB64_CSS_FD_NAME_PNG}
            ${SSB64_CSS_FD_EMBLEM_PNG}
            ${SSB64_CSS_MC_BG_PNG}
            ${SSB64_CSS_MC_SMALL_PNG}
            ${SSB64_CSS_MC_NAME_PNG}
            ${SSB64_CSS_BF_BG_PNG}
            ${SSB64_CSS_BF_SMALL_PNG}
            ${SSB64_CSS_BF_NAME_PNG}
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/assets/pipeline/derive_stage_assets.py
                ${SSB64_CSS_ASSET_ROM}
                ${SSB64_CSS_STAGE_ASSET_DIR}
        DEPENDS
            ${SSB64_CSS_ASSET_ROM}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/assets/pipeline/derive_stage_assets.py
        COMMENT "Extracting CSS stage assets from ${SSB64_CSS_ASSET_ROM}"
        VERBATIM
    )
    add_custom_target(DeriveStageAssets
        DEPENDS
            ${SSB64_CSS_FD_BG_PNG}
            ${SSB64_CSS_FD_SMALL_PNG}
            ${SSB64_CSS_FD_NAME_PNG}
            ${SSB64_CSS_FD_EMBLEM_PNG}
            ${SSB64_CSS_MC_BG_PNG}
            ${SSB64_CSS_MC_SMALL_PNG}
            ${SSB64_CSS_MC_NAME_PNG}
            ${SSB64_CSS_BF_BG_PNG}
            ${SSB64_CSS_BF_SMALL_PNG}
            ${SSB64_CSS_BF_NAME_PNG}
    )
    add_dependencies(${PROJECT_NAME} DeriveStageAssets)
    # Copy extracted PNGs into the run-directory asset subtree so the game
    # can find them at  <run-dir>/assets/css_icons/<name>_*.png .
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/css_icons
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SSB64_CSS_FD_BG_PNG}
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/css_icons/final_destination_background.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SSB64_CSS_FD_SMALL_PNG}
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/css_icons/final_destination_small.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SSB64_CSS_FD_NAME_PNG}
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/css_icons/final_destination_name.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SSB64_CSS_FD_EMBLEM_PNG}
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/css_icons/final_destination_emblem.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SSB64_CSS_MC_BG_PNG}
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/css_icons/metal_cavern_background.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SSB64_CSS_MC_SMALL_PNG}
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/css_icons/metal_cavern_small.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SSB64_CSS_MC_NAME_PNG}
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/css_icons/metal_cavern_name.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SSB64_CSS_BF_BG_PNG}
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/css_icons/battlefield_background.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SSB64_CSS_BF_SMALL_PNG}
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/css_icons/battlefield_small.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SSB64_CSS_BF_NAME_PNG}
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/css_icons/battlefield_name.png
        COMMENT "Staging CSS stage asset PNGs next to ssb64 binary"
    )
else()
    add_custom_target(DeriveStageAssets
        COMMENT "DeriveStageAssets: skipped (baserom absent)"
    )
    message(STATUS "CSS stage assets: baserom absent — DeriveStageAssets stub installed")
endif()

# NOTE: there is intentionally no port-asset o2r target. `ssb64.o2r` was an
# early-development artifact; the runtime loads only BattleShip.o2r
# (ROM-derived, user-extracted at first run) and f3d.o2r (shaders). No
# ROM-derived data is ever produced by the build.

endif() # desktop-only — ExtractAssets / GenerateBattleShipO2R block

################################################################################
# Android-only targets: torch_runner SHARED library + coroutine_test harness
################################################################################
if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    # libtorch_runner.so — wraps the static libtorch.a (from add_subdirectory
    # at the top) with a small extern "C" entry point that the first-run
    # Activity calls via JNI to extract the user's ROM into BattleShip.o2r.
    # Lives outside libmain.so so Java can load it BEFORE SDLActivity/libSDL2
    # are initialized — the ROM picker is a regular Activity that runs before
    # the SDL game loop starts.
    add_library(torch_runner SHARED src/platform/android/android_torch_bridge.cpp)
    target_include_directories(torch_runner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/components/game-runtime/src
    )
    # The extractor + its FetchContent'd deps (yaml-cpp, tinyxml2, libgfxd,
    # BinaryTools, N64Graphics, spdlog, GSL, ...). All are static libs from
    # torch's CMakeLists.txt under USE_STANDALONE=OFF.
    target_link_libraries(torch_runner PRIVATE torch)
    set_target_properties(torch_runner PROPERTIES OUTPUT_NAME "torch_runner")

    # Standalone ARM coroutine test (off by default — opt in for asm
    # regression validation, see docs/android_port_status_*.md). Pushes a
    # tiny binary to /data/local/tmp/ to verify the asm context-switch
    # without needing the full game. Both .S shims are arch-#if-guarded, so
    # the same target builds for arm64-v8a and armeabi-v7a.
    option(SSB64_BUILD_COROUTINE_TEST
        "Build the ARM coroutine standalone test binary (Android)" OFF)
    if (SSB64_BUILD_COROUTINE_TEST)
        add_executable(coroutine_test
            tests/unit/platform/coroutine_test.cpp
            src/platform/coroutine/coroutine_android.cpp
            src/platform/coroutine/coroutine_aarch64.S
            src/platform/coroutine/coroutine_armv7.S
        )
        target_include_directories(coroutine_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/coroutine
        )
    endif()
endif()

################################################################################
# Mod TCC mods (desktop only — the mod system is excluded on Android)
#
# Each mod under mods/examples/ or mods/templates/ has its own CMakeLists.txt that bundles the
# mod's source + assets into output/ and stages a copy under
# build/<config>/mods/<ModName>/ so the runtime loader picks it up.
# ModO2R provides the optional ssb64_add_mod_o2r_target helper for
# packaging into a single .o2r distributable.
################################################################################
if (NOT DISABLE_SCRIPTING)
    include(ModO2R)

    # Stage bundled example mods next to the binary so the in-game Mods
    # panel can offer one-click install (PortMenu copies
    # bundled-mods/<name>/ into mods/ then hot-reloads). package_mod.py
    # replicates each mod's own CMakeLists output (build.gen + dist/*.c +
    # manifest + headers). POST_BUILD keeps the staged copy in sync with
    # source edits; packagers pick bundled-mods/ up with the rest of the
    # exe directory.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/developer/modding/package_mod.py
            --mod-dir ${CMAKE_CURRENT_SOURCE_DIR}/mods/examples/playertint
            --out-dir "$<TARGET_FILE_DIR:${PROJECT_NAME}>/bundled-mods/playertint"
            --repo-root ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Staging bundled mod: playertint"
        VERBATIM)
endif()

# GLideN64->BattleShip texture-name mapping (vendored from GhostlyDark's
# SSB-Reloaded, MIT — see tools/developer/modding/vendor/README.md). The in-game
# converter (src/application/update/downloads/ModPackDownloader.cpp) renames
# GLideN64/Rice-format PNG packs to the native "#hash#fmt#siz.png"
# grammar using this table. Staged as bundled-data/gliden64-map.json;
# independent of DISABLE_SCRIPTING since texture packs aren't script mods.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/bundled-data"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/developer/modding/vendor/battleship.json
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/bundled-data/gliden64-map.json"
    COMMENT "Staging GLideN64 conversion map"
    VERBATIM)

################################################################################
# Switch toolchain settings + SD-card bundling (platforms/switch/cmake/Switch.cmake).
# Included late so BundleSwitch can depend on GenerateF3DO2R, which is
# defined by cmake/RuntimeAssets.cmake above.
################################################################################
include(${CMAKE_CURRENT_SOURCE_DIR}/platforms/switch/cmake/Switch.cmake)

################################################################################
# Test harness (tests/). Off by default — enables CTest + smash_add_test
# registrations. See tests/README.md for the level/domain layout and
# tests/support/config/test-suites.yml for suite presets.
################################################################################
option(SSB64_BUILD_TESTING "Build the CTest-registered test harness (tests/)" OFF)
if (SSB64_BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
