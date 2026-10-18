include(FetchContent)

# Emscripten dependency set. Mirrors android.cmake except SDL2, which comes
# from the compiler port (-sUSE_SDL=2) rather than a built library: the flag
# supplies both the include paths at compile time and the wasm-side port
# object at link time. Wrapping it in an INTERFACE target lets the existing
# `target_link_libraries(... SDL2::SDL2)` call sites work unchanged.
if (NOT TARGET SDL2::SDL2)
    add_library(_lus_emscripten_sdl2 INTERFACE)
    target_compile_options(_lus_emscripten_sdl2 INTERFACE -sUSE_SDL=2)
    target_link_options(_lus_emscripten_sdl2 INTERFACE -sUSE_SDL=2)
    add_library(SDL2::SDL2 ALIAS _lus_emscripten_sdl2)
endif()

#=================== zlib (for libzip) ===================
# libzip's REQUIRED dep; there is no system zlib to find under the
# Emscripten sysroot, so build it from source. ZLIB::ZLIB is aliased
# manually because upstream zlib's own CMake does not always export it.
find_package(ZLIB QUIET)
if (NOT ZLIB_FOUND)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.1
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(zlib)
    if (NOT TARGET ZLIB::ZLIB)
        if (TARGET zlibstatic)
            add_library(ZLIB::ZLIB ALIAS zlibstatic)
        elseif (TARGET zlib)
            add_library(ZLIB::ZLIB ALIAS zlib)
        endif()
    endif()
endif()

#=================== nlohmann-json ===================
find_package(nlohmann_json QUIET)
if (NOT ${nlohmann_json_FOUND})
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

#=================== tinyxml2 ===================
find_package(tinyxml2 QUIET)
if (NOT ${tinyxml2_FOUND})
    set(tinyxml2_BUILD_TESTING OFF)
    FetchContent_Declare(
        tinyxml2
        GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
        GIT_TAG 11.0.0
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(tinyxml2)
endif()

#=================== spdlog ===================
find_package(spdlog QUIET)
if (NOT ${spdlog_FOUND})
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.16.0
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(spdlog)
endif()

#=================== libzip ===================
find_package(libzip QUIET)
if (NOT ${libzip_FOUND})
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_TOOLS OFF)
    set(BUILD_REGRESS OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_DOC OFF)
    set(BUILD_OSSFUZZ OFF)
    set(BUILD_SHARED_LIBS OFF)
    # Embedded build: no install rules, and the export set would otherwise
    # fail at generate time on the FetchContent'd zlibstatic dependency.
    set(LIBZIP_DO_INSTALL OFF)
    # No crypto/compression backends under wasm — zlib only. Keeps the
    # port lean and avoids accidental pick-up of host libraries.
    set(ENABLE_BZIP2 OFF)
    set(ENABLE_LZMA OFF)
    set(ENABLE_ZSTD OFF)
    set(ENABLE_OPENSSL OFF)
    set(ENABLE_MBEDTLS OFF)
    set(ENABLE_GNUTLS OFF)
    FetchContent_Declare(
        libzip
        GIT_REPOSITORY https://github.com/nih-at/libzip.git
        GIT_TAG v1.11.4
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(libzip)
    list(APPEND ADDITIONAL_LIB_INCLUDES ${libzip_SOURCE_DIR}/lib ${libzip_BINARY_DIR})
endif()

target_link_libraries(ImGui PUBLIC SDL2::SDL2)
