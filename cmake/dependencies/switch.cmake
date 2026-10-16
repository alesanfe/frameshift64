#
# Switch (Nintendo Switch Homebrew) platform dependencies
#
# Portlibs (devkitPro) ship SDL2, tinyxml2, zlib and GLESv2/EGL with CMake
# configs; spdlog, nlohmann_json and libzip have no portlib and come from
# FetchContent. Modeled on android.cmake with the bits PR #185's LUS
# branch needed for newlib (spdlog fwrite_unlocked, _POSIX_C_SOURCE).

message(STATUS "Configuring dependencies for Nintendo Switch")

include(FetchContent)

# Switch renders through GLES2 (mesa/libdrm_nouveau portlibs), not desktop GL.
set(USE_OPENGLES ON CACHE BOOL "" FORCE)
add_compile_definitions(IMGUI_IMPL_OPENGL_ES3)

# The dkp toolchain only injects libnx's include dir as a standard include —
# portlibs headers (GLES3/gl3.h, glad, EGL, SDL2) need an explicit path.
# ImGui is already created by common.cmake before this file is included, so
# directory-level include_directories() wouldn't reach it — target it.
# DEVKITPRO is a CMake variable set by the toolchain (with /opt/devkitpro
# fallback); don't gate on the env var — it is absent on reconfigures that
# weren't spawned by build-switch.sh.
if(DEFINED DEVKITPRO)
    include_directories(SYSTEM ${DEVKITPRO}/portlibs/switch/include)
    target_include_directories(ImGui SYSTEM PRIVATE ${DEVKITPRO}/portlibs/switch/include)
endif()

# .nro is a single static binary.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

#=================== SDL2 ===================
# devkitPro portlibs ships libSDL2.a + sdl2-config.cmake under
# portlibs/switch; the toolchain prepends that prefix to
# CMAKE_PREFIX_PATH so find_package resolves it.
find_package(SDL2 REQUIRED)

#=================== OpenGL (GLES2 via portlibs) ===================
find_package(OpenGL REQUIRED)

#=================== zlib ===================
find_package(ZLIB QUIET)
if (NOT ${ZLIB_FOUND})
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    FetchContent_Declare(
        ZLIB
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.1
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(ZLIB)
endif()

if (NOT TARGET ZLIB::ZLIB)
    if (TARGET zlibstatic)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
    elseif(TARGET zlib)
        add_library(ZLIB::ZLIB ALIAS zlib)
    endif()
endif()

#=================== nlohmann-json ===================
# No devkitPro portlib — always comes from FetchContent.
find_package(nlohmann_json QUIET)
if (NOT ${nlohmann_json_FOUND})
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

if (NOT TARGET nlohmann_json::nlohmann_json AND TARGET nlohmann_json)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
endif()

#=================== tinyxml2 ===================
# devkitPro portlibs ships switch-tinyxml2 with a CMake config.
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

if (NOT TARGET tinyxml2::tinyxml2 AND TARGET tinyxml2)
    add_library(tinyxml2::tinyxml2 ALIAS tinyxml2)
elseif (NOT TARGET tinyxml2::tinyxml2 AND TARGET tinyxml2_static)
    add_library(tinyxml2::tinyxml2 ALIAS tinyxml2_static)
endif()

#=================== spdlog ===================
# No devkitPro portlib — always comes from FetchContent.
find_package(spdlog QUIET)
if (NOT ${spdlog_FOUND})
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    # newlib on Switch lacks some unlocked stdio APIs upstream expects.
    set(SPDLOG_FWRITE_UNLOCKED OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.16.0
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(spdlog)
endif()

if (TARGET spdlog)
    target_compile_definitions(spdlog PUBLIC _POSIX_C_SOURCE=200809L)
endif()
if (TARGET spdlog_header_only)
    target_compile_definitions(spdlog_header_only INTERFACE _POSIX_C_SOURCE=200809L)
endif()

#=================== libzip ===================
# No devkitPro portlib. Optional compression backends (bzip2/lzma/zstd)
# are absent from the Switch sysroot; libzip configures without them,
# which is fine — our .o2r archives are store/zlib only.
find_package(libzip QUIET)
if (NOT ${libzip_FOUND})
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_TOOLS OFF)
    set(BUILD_REGRESS OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_DOC OFF)
    set(BUILD_OSSFUZZ OFF)
    set(LIBZIP_DO_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_Declare(
        libzip
        GIT_REPOSITORY https://github.com/nih-at/libzip.git
        GIT_TAG v1.11.4
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(libzip)
    list(APPEND ADDITIONAL_LIB_INCLUDES ${libzip_SOURCE_DIR}/lib ${libzip_BINARY_DIR})
endif()

if (NOT TARGET libzip::zip AND TARGET zip)
    add_library(libzip::zip ALIAS zip)
endif()

target_link_libraries(ImGui PUBLIC SDL2::SDL2)
