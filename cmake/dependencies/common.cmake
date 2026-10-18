include(FetchContent)

find_package(OpenGL QUIET)

#=================== ImGui ===================
set(imgui_fixes_and_config_patch_file ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dependencies/patches/imgui-fixes-and-config.patch)
set(imgui_apply_patch_command ${CMAKE_COMMAND} -Dpatch_file=${imgui_fixes_and_config_patch_file} -Dwith_reset=TRUE -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dependencies/git-patch.cmake)
if(BATTLESHIP_UWP)
    set(imgui_uwp_patch_file ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dependencies/patches/imgui-uwp.patch)
    set(imgui_apply_patch_command
        ${imgui_apply_patch_command}
        COMMAND ${CMAKE_COMMAND} -Dpatch_file=${imgui_uwp_patch_file} -Dwith_reset=FALSE
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dependencies/git-patch.cmake)
endif()

FetchContent_Declare(
    ImGui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b-docking
    PATCH_COMMAND ${imgui_apply_patch_command}
)
FetchContent_MakeAvailable(ImGui)
list(APPEND ADDITIONAL_LIB_INCLUDES ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

add_library(ImGui STATIC)
set_property(TARGET ImGui PROPERTY CXX_STANDARD 20)

target_sources(ImGui
    PRIVATE
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
)

target_sources(ImGui
    PRIVATE
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
)

target_include_directories(ImGui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends PRIVATE ${SDL2_INCLUDE_DIRS})

# ========= StormLib =============
if(INCLUDE_MPQ_SUPPORT)
    set(stormlib_patch_file ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dependencies/patches/stormlib-optimizations.patch)
    set(stormlib_apply_patch_command ${CMAKE_COMMAND} -Dpatch_file=${stormlib_patch_file} -Dwith_reset=TRUE -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/dependencies/git-patch.cmake)

    FetchContent_Declare(
        StormLib
        GIT_REPOSITORY https://github.com/ladislav-zezula/StormLib.git
        GIT_TAG v9.25
        PATCH_COMMAND ${stormlib_apply_patch_command}
    )
    FetchContent_MakeAvailable(StormLib)
    list(APPEND ADDITIONAL_LIB_INCLUDES ${stormlib_SOURCE_DIR}/src)
endif()

#=================== STB ===================
set(STB_DIR ${CMAKE_BINARY_DIR}/_deps/stb)
file(DOWNLOAD "https://github.com/nothings/stb/raw/0bc88af4de5fb022db643c2d8e549a0927749354/stb_image.h" "${STB_DIR}/stb_image.h")
# If network download failed (sandbox/proxy/offline), fall back to vendored copy used by Torch.
if (EXISTS "${STB_DIR}/stb_image.h")
    file(SIZE "${STB_DIR}/stb_image.h" STB_IMAGE_SIZE)
else()
    set(STB_IMAGE_SIZE 0)
endif()
if (STB_IMAGE_SIZE EQUAL 0)
    set(STB_FALLBACK "${CMAKE_SOURCE_DIR}/components/game-runtime/lib/n64graphics/stb_image.h")
    if (EXISTS "${STB_FALLBACK}")
        message(WARNING "stb_image.h download failed/empty; using fallback at ${STB_FALLBACK}")
        file(COPY "${STB_FALLBACK}" DESTINATION "${STB_DIR}")
    else()
        message(FATAL_ERROR "stb_image.h download failed and fallback file is missing: ${STB_FALLBACK}")
    endif()
endif()
file(WRITE "${STB_DIR}/stb_impl.c" "#define STB_IMAGE_IMPLEMENTATION\n#include \"stb_image.h\"")

add_library(stb STATIC)

target_sources(stb PRIVATE
    ${STB_DIR}/stb_image.h
    ${STB_DIR}/stb_impl.c
)

target_include_directories(stb PUBLIC ${STB_DIR})
list(APPEND ADDITIONAL_LIB_INCLUDES ${STB_DIR})

#=================== libgfxd ===================
if (GFX_DEBUG_DISASSEMBLER)
    FetchContent_Declare(
        libgfxd
        GIT_REPOSITORY https://github.com/glankk/libgfxd.git
        GIT_TAG 008f73dca8ebc9151b205959b17773a19c5bd0da
    )
    FetchContent_MakeAvailable(libgfxd)

    add_library(libgfxd STATIC)
    set_property(TARGET libgfxd PROPERTY C_STANDARD 11)

    target_sources(libgfxd PRIVATE
        ${libgfxd_SOURCE_DIR}/gbi.h
        ${libgfxd_SOURCE_DIR}/gfxd.h
        ${libgfxd_SOURCE_DIR}/priv.h
        ${libgfxd_SOURCE_DIR}/gfxd.c
        ${libgfxd_SOURCE_DIR}/uc.c
        ${libgfxd_SOURCE_DIR}/uc_f3d.c
        ${libgfxd_SOURCE_DIR}/uc_f3db.c
        ${libgfxd_SOURCE_DIR}/uc_f3dex.c
        ${libgfxd_SOURCE_DIR}/uc_f3dex2.c
        ${libgfxd_SOURCE_DIR}/uc_f3dexb.c
    )

    target_include_directories(libgfxd PUBLIC ${libgfxd_SOURCE_DIR})
endif()

#======== Post-process shader transpiler (glslang + SPIRV-Cross) ========
#
# Pulls in glslang (BSD-3 + Apache 2.0) and SPIRV-Cross (Apache 2.0 + MIT)
# so user-supplied .glsl post-process shaders can be transpiled to HLSL and
# MSL at load time. Both licenses are MIT-compatible; nothing GPL ships
# inside libultraship as a result of this addition. See
# docs/crt_shader_plan_2026-05-11.md §3.2 / §8.3.
#
# Heavy first-build cost (~10-20s extra link), so default OFF on the mobile
# targets that don't yet have a CRT-shader UI. Desktops get it ON so user
# .glsl drops Just Work cross-backend.
if (CMAKE_SYSTEM_NAME STREQUAL "iOS" OR CMAKE_SYSTEM_NAME STREQUAL "Android" OR CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    set(_LUS_PP_TRANSPILER_DEFAULT OFF)
else()
    set(_LUS_PP_TRANSPILER_DEFAULT ON)
endif()
option(LUS_ENABLE_POSTPROCESS_TRANSPILER
       "Build the GLSL->HLSL/MSL transpiler used by the post-process runtime"
       ${_LUS_PP_TRANSPILER_DEFAULT})
unset(_LUS_PP_TRANSPILER_DEFAULT)

if (LUS_ENABLE_POSTPROCESS_TRANSPILER)
    # glslang. We disable everything except the bits we link against.
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE) # avoids the SPIRV-Tools dep
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    set(_LUS_BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_Declare(
        glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG 15.4.0
    )
    FetchContent_MakeAvailable(glslang)
    set(BUILD_SHARED_LIBS ${_LUS_BUILD_SHARED_LIBS_SAVED})
    unset(_LUS_BUILD_SHARED_LIBS_SAVED)

    # SPIRV-Cross. We only need core + glsl + hlsl + msl static libs.
    set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_FORCE_PIC ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        spirv_cross
        GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
        GIT_TAG vulkan-sdk-1.4.304.0
    )
    FetchContent_MakeAvailable(spirv_cross)
endif()

#======== thread-pool ========
FetchContent_Declare(
    ThreadPool
    GIT_REPOSITORY https://github.com/bshoshany/thread-pool.git
    GIT_TAG v4.1.0
)
FetchContent_MakeAvailable(ThreadPool)

list(APPEND ADDITIONAL_LIB_INCLUDES ${threadpool_SOURCE_DIR}/include)

#=========== prism ===========
option(PRISM_STANDALONE "Build prism as a standalone library" OFF)
FetchContent_Declare(
    prism
    GIT_REPOSITORY https://github.com/KiritoDv/prism-processor.git
    GIT_TAG 1de054450e7b3c5f777d2e3dfcb228ad120c329d
)
FetchContent_MakeAvailable(prism)

#=========== hidapi (native Raphnet N64 USB adapter support) ===========
# Static-only, hidraw backend on Linux (no libusb dep). 0.14.0 fixes a
# macOS hid_close deadlock present in 0.13.x.
#
# On Android, hidapi-0.14.0 only ships a libusb backend, and the NDK
# sysroot has no libusb-1.0 — so we provide a tiny stub library that
# exposes the same hidapi C symbols as no-ops. The Raphnet code paths
# in libultraship still link cleanly; at runtime hid_init() reports
# success but hid_enumerate() returns nullptr, so RaphnetPhysicalDeviceManager
# decides "no adapter found" and the SDL2 / Java HIDDeviceManager handles
# everything that's actually present. Switch is in the same boat: no USB
# host HID access from a retail .nro, so the same stub applies.
# Emscripten joins the stub set: browsers have no raw USB HID without the
# WebHID API, so Raphnet enumeration must report "no adapter" and SDL's
# GameController path handles pads.
if (CMAKE_SYSTEM_NAME STREQUAL "Android" OR CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch" OR CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    set(_LUS_HIDAPI_STUB_DIR ${CMAKE_CURRENT_BINARY_DIR}/hidapi_stub)
    file(MAKE_DIRECTORY ${_LUS_HIDAPI_STUB_DIR})
    file(WRITE ${_LUS_HIDAPI_STUB_DIR}/hidapi.h [=[
/* hidapi.h — Android stub shim. Minimal subset used by libultraship's
 * Raphnet support paths. All operations report failure / no devices. */
#ifndef HIDAPI_STUB_H
#define HIDAPI_STUB_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hid_device_ hid_device;

struct hid_device_info {
    char            *path;
    unsigned short   vendor_id;
    unsigned short   product_id;
    wchar_t         *serial_number;
    unsigned short   release_number;
    wchar_t         *manufacturer_string;
    wchar_t         *product_string;
    unsigned short   usage_page;
    unsigned short   usage;
    int              interface_number;
    struct hid_device_info *next;
};

static inline int                       hid_init(void) { return 0; }
static inline int                       hid_exit(void) { return 0; }
static inline struct hid_device_info   *hid_enumerate(unsigned short vid, unsigned short pid) { (void)vid; (void)pid; return NULL; }
static inline void                      hid_free_enumeration(struct hid_device_info *devs) { (void)devs; }
static inline hid_device               *hid_open_path(const char *path) { (void)path; return NULL; }
static inline void                      hid_close(hid_device *dev) { (void)dev; }
static inline int                       hid_read(hid_device *dev, unsigned char *data, size_t length) { (void)dev; (void)data; (void)length; return -1; }
static inline int                       hid_send_feature_report(hid_device *dev, const unsigned char *data, size_t length) { (void)dev; (void)data; (void)length; return -1; }
static inline int                       hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length) { (void)dev; (void)data; (void)length; return -1; }
static inline int                       hid_set_nonblocking(hid_device *dev, int nonblock) { (void)dev; (void)nonblock; return 0; }
static inline const wchar_t            *hid_error(hid_device *dev) { (void)dev; return L"hidapi-stub: this platform has no native HID backend"; }

#ifdef __cplusplus
}
#endif

#endif /* HIDAPI_STUB_H */
]=])
    add_library(hidapi_stub INTERFACE)
    target_include_directories(hidapi_stub INTERFACE ${_LUS_HIDAPI_STUB_DIR})
    add_library(hidapi::hidapi ALIAS hidapi_stub)
else()
    set(HIDAPI_BUILD_HIDTEST OFF CACHE BOOL "" FORCE)
    set(HIDAPI_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(HIDAPI_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
        set(HIDAPI_WITH_HIDRAW ON CACHE BOOL "" FORCE)
        set(HIDAPI_WITH_LIBUSB OFF CACHE BOOL "" FORCE)
    endif()
    # Force static while pulling hidapi in, then restore caller's preference.
    set(_LUS_BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    # hidapi-0.14.0 declares cmake_minimum_required(3.4.3); CMake 4.x removed
    # < 3.5 compatibility, so allow it via policy minimum.
    set(_LUS_CMP_MIN_SAVED ${CMAKE_POLICY_VERSION_MINIMUM})
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_Declare(
        hidapi
        GIT_REPOSITORY https://github.com/libusb/hidapi.git
        GIT_TAG hidapi-0.14.0
    )
    FetchContent_MakeAvailable(hidapi)
    set(CMAKE_POLICY_VERSION_MINIMUM ${_LUS_CMP_MIN_SAVED})
    unset(_LUS_CMP_MIN_SAVED)
    set(BUILD_SHARED_LIBS ${_LUS_BUILD_SHARED_LIBS_SAVED})
    unset(_LUS_BUILD_SHARED_LIBS_SAVED)
endif()

#=========== libtcc (TinyCC — runtime C compiler for mod scripting) ===========
# Ported from upstream Kenix3/libultraship at SHA ecb0867. Builds libtcc and
# libtcc1 as STATIC. Game-side post-build steps (.def gen, .tcc/
# include+lib copy) live in the outer project's CMakeLists.txt.
if(NOT DISABLE_SCRIPTING)

FetchContent_Declare(
    tinycc
    GIT_REPOSITORY https://github.com/TinyCC/tinycc.git
    GIT_TAG        a338258d309c888bde96b2d1f206299231a54ddf
)

FetchContent_MakeAvailable(tinycc)
if(NOT TARGET libtcc)
    if(NOT EXISTS "${tinycc_SOURCE_DIR}/config.h")
        message(STATUS "Configuring TinyCC to generate config.h...")
        if(WIN32)
            # Some Windows shell configs (NoDefaultCurrentDirectoryInExePath)
            # prevent cmd from finding the .bat in cwd without a .\ prefix.
            execute_process(
                COMMAND cmd /c ".\\build-tcc.bat" -c cl
                WORKING_DIRECTORY "${tinycc_SOURCE_DIR}/win32"
                RESULT_VARIABLE tcc_config_result
            )
        else()
            execute_process(
                COMMAND ./configure
                WORKING_DIRECTORY "${tinycc_SOURCE_DIR}"
                RESULT_VARIABLE tcc_config_result
            )
        endif()

        if(NOT tcc_config_result EQUAL 0)
            # build-tcc.bat / configure failed (typically: cl.exe not on PATH
            # because cmake was run from a shell without vcvars set, e.g. plain
            # bash instead of a VS Developer Prompt). config.h is trivial — read
            # the VERSION file and synthesize it ourselves so the libtcc compile
            # can proceed regardless.
            message(WARNING "TinyCC configuration script returned non-zero; falling back to direct config.h generation.")
            file(READ "${tinycc_SOURCE_DIR}/VERSION" TCC_VERSION_RAW)
            string(STRIP "${TCC_VERSION_RAW}" TCC_VERSION_STRIPPED)
            file(WRITE "${tinycc_SOURCE_DIR}/config.h"
                "#define TCC_VERSION \"${TCC_VERSION_STRIPPED}\"\n")
        endif()

        if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
            message(STATUS "iOS target detected: Disabling CONFIG_CODESIGN...")
            file(APPEND "${tinycc_SOURCE_DIR}/config.h" "\n/* Force disable code signing for iOS cross-compilation */\n#undef CONFIG_CODESIGN\n")
        endif()
    endif()

    if(CMAKE_CROSSCOMPILING)
        find_program(HOST_C_COMPILER NAMES cc clang gcc REQUIRED)
        set(C2STR_EXE "${tinycc_BINARY_DIR}/tcc_c2str_host")
        if(CMAKE_HOST_WIN32)
            set(C2STR_EXE "${C2STR_EXE}.exe")
        endif()

        set(SIGN_COMMAND "")
        if(CMAKE_HOST_APPLE)
            set(SIGN_COMMAND COMMAND codesign -f -s - "${C2STR_EXE}")
        endif()

        add_custom_command(
            OUTPUT "${C2STR_EXE}"
            COMMAND ${CMAKE_COMMAND} -E env --unset=SDKROOT --unset=IPHONEOS_DEPLOYMENT_TARGET --unset=TVOS_DEPLOYMENT_TARGET
                    ${HOST_C_COMPILER} -DC2STR -o "${C2STR_EXE}" "${tinycc_SOURCE_DIR}/conftest.c"
            ${SIGN_COMMAND}
            DEPENDS "${tinycc_SOURCE_DIR}/conftest.c"
            COMMENT "Compiling host tool c2str natively..."
        )

        add_custom_command(
            OUTPUT "${tinycc_BINARY_DIR}/tccdefs_.h"
            COMMAND "${C2STR_EXE}" "${tinycc_SOURCE_DIR}/include/tccdefs.h" "${tinycc_BINARY_DIR}/tccdefs_.h"
            DEPENDS "${tinycc_SOURCE_DIR}/include/tccdefs.h" "${C2STR_EXE}"
            COMMENT "Generating tccdefs_.h for TinyCC (Cross-compiling)..."
        )
    else()
        add_executable(tcc_c2str "${tinycc_SOURCE_DIR}/conftest.c")
        target_compile_definitions(tcc_c2str PRIVATE C2STR)
        target_include_directories(tcc_c2str PRIVATE "${tinycc_SOURCE_DIR}")

        if(APPLE)
            set_target_properties(tcc_c2str PROPERTIES
                CODE_SIGNING_ALLOWED NO
                CODE_SIGNING_REQUIRED NO
                XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
                XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
                XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
                XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
            )
        endif()

        add_custom_command(
            OUTPUT "${tinycc_BINARY_DIR}/tccdefs_.h"
            COMMAND tcc_c2str "${tinycc_SOURCE_DIR}/include/tccdefs.h" "${tinycc_BINARY_DIR}/tccdefs_.h"
            DEPENDS "${tinycc_SOURCE_DIR}/include/tccdefs.h" tcc_c2str
            COMMENT "Generating tccdefs_.h for TinyCC..."
        )
    endif()

    add_library(libtcc STATIC
        "${tinycc_SOURCE_DIR}/libtcc.c"
        "${tinycc_BINARY_DIR}/tccdefs_.h"
    )

    add_library(libtcc1 STATIC
        "${tinycc_SOURCE_DIR}/lib/libtcc1.c"
    )
    # Standalone tcc.exe — the CLI driver. Used post-build by the outer
    # project to run `tcc.exe -impdef BattleShip.exe -o BattleShip.def`,
    # producing the export-name list that memory-mode mod source resolves
    # against the running process.
    # tcc.c #includes libtcc.c via ONE_SOURCE so we don't link against
    # libtcc here — it's a self-contained translation unit.
    add_executable(tcc
        "${tinycc_SOURCE_DIR}/tcc.c"
        "${tinycc_BINARY_DIR}/tccdefs_.h"
    )
    target_include_directories(tcc PRIVATE
        "${tinycc_SOURCE_DIR}"
        "${tinycc_BINARY_DIR}"
    )

    target_include_directories(libtcc1 PRIVATE
        "${tinycc_SOURCE_DIR}"
        "${tinycc_BINARY_DIR}"
    )

    if(MSVC)
        if(CMAKE_GENERATOR_PLATFORM MATCHES "ARM64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
            target_compile_definitions(libtcc1 PRIVATE __aarch64__ _WIN64)
            target_compile_definitions(libtcc  PRIVATE __aarch64__ TCC_TARGET_ARM64 _WIN64)
            target_compile_definitions(tcc     PRIVATE __aarch64__ TCC_TARGET_ARM64 _WIN64)
        else()
            target_compile_definitions(libtcc1 PRIVATE __x86_64__ _WIN64)
            target_compile_definitions(libtcc  PRIVATE __x86_64__ TCC_TARGET_X86_64 _WIN64)
            target_compile_definitions(tcc     PRIVATE __x86_64__ TCC_TARGET_X86_64 _WIN64)
        endif()
        # TCC_TARGET_PE: generate Windows PE/COFF code and use Windows
        # runtime conventions instead of Linux/ELF. Required on libtcc for
        # memory-mode mod relocation and on tcc.exe so `-impdef` is available.
        target_compile_definitions(libtcc PRIVATE TCC_TARGET_PE)
        target_compile_definitions(tcc    PRIVATE TCC_TARGET_PE)
        target_compile_definitions(libtcc1 PRIVATE "__faststorefence=__faststorefence_tcc_unused")
    endif()

    set(TCC_SAFE_INCLUDE_DIR "${tinycc_BINARY_DIR}/safe_include")
    configure_file(
        "${tinycc_SOURCE_DIR}/libtcc.h"
        "${TCC_SAFE_INCLUDE_DIR}/libtcc.h"
        COPYONLY
    )

    target_include_directories(libtcc PRIVATE
        "${tinycc_SOURCE_DIR}"
        "${tinycc_BINARY_DIR}"
    )
    target_include_directories(libtcc PUBLIC
        $<BUILD_INTERFACE:${TCC_SAFE_INCLUDE_DIR}>
    )

    if(ANDROID)
        target_link_libraries(libtcc PRIVATE dl m)
    elseif(UNIX AND NOT APPLE)
        target_link_libraries(libtcc PRIVATE dl m pthread)
    endif()

    set_target_properties(libtcc  PROPERTIES OUTPUT_NAME "tcc")
    set_target_properties(libtcc1 PROPERTIES OUTPUT_NAME "tcc1")

    if(APPLE)
        set_target_properties(libtcc libtcc1 PROPERTIES
            CODE_SIGNING_ALLOWED NO
            CODE_SIGNING_REQUIRED NO
            XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
            XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
            XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
            XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
        )
    endif()
endif()

endif() # NOT DISABLE_SCRIPTING
