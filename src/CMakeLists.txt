include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/cvars.cmake)

add_library(libultraship STATIC)
set_target_properties(libultraship PROPERTIES PREFIX "")
set_property(TARGET libultraship PROPERTY CXX_STANDARD 20)
# Need to set C11 for libgfxd (_Alignas)
set_property(TARGET libultraship PROPERTY C_STANDARD 11)

set(INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../include)
option(USE_OPENGLES "Enable GLES3" OFF)
option(GFX_DEBUG_DISASSEMBLER "Enable libgfxd" OFF)

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
use_props(${PROJECT_NAME} "${CMAKE_CONFIGURATION_TYPES}" "${DEFAULT_CXX_PROPS}")
endif()

#=================== Top-Level ===================

configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/ship/install_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/ship/install_config.h @ONLY)

#=================== Fast ===================

add_subdirectory("fast")

#=================== LUS ===================

add_subdirectory("libultraship")

#=================== Ship ===================

add_subdirectory("ship")

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/ship/utils/AppleFolderManager.mm PROPERTIES COMPILE_FLAGS -fno-objc-arc)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/ship/utils/macUtils.mm PROPERTIES COMPILE_FLAGS -fno-objc-arc)
endif()

#=================== Packages & Includes ===================

target_include_directories(libultraship
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${INCLUDE_DIR} ${ADDITIONAL_LIB_INCLUDES}
)

#=================== Linking ===================
if(INCLUDE_MPQ_SUPPORT)
    if (NOT CMAKE_SYSTEM_NAME STREQUAL "Windows" AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_link_libraries(libultraship PUBLIC "$<LINK_LIBRARY:WHOLE_ARCHIVE,storm>")
    else()
        target_link_libraries(libultraship PUBLIC storm)
    endif()
endif()

target_link_libraries(libultraship
    PUBLIC ImGui
)

target_link_libraries(libultraship PUBLIC stb)

if (GFX_DEBUG_DISASSEMBLER)
    target_link_libraries(libultraship PRIVATE libgfxd)
endif()

find_package(libzip REQUIRED)
target_link_libraries(libultraship PRIVATE libzip::zip)

find_package(nlohmann_json REQUIRED)
target_link_libraries(libultraship PUBLIC nlohmann_json::nlohmann_json)

find_package(tinyxml2 REQUIRED)
target_link_libraries(libultraship PUBLIC tinyxml2::tinyxml2)

find_package(spdlog REQUIRED)
target_link_libraries(libultraship PUBLIC spdlog::spdlog)

# hidapi for native Raphnet N64 USB adapter (vendored via FetchContent in cmake/dependencies/common.cmake).
target_link_libraries(libultraship PRIVATE hidapi::hidapi)

# libusb for the native GameCube adapter (WUP-028) driver. Optional: without it
# GCAdapter compiles to a stub and the adapter is left to SDL as before.
#   * Linux/BSD: use the system libusb-1.0 (pkg-config) when present.
#   * Everything else (Windows, macOS, or Linux without libusb-dev): build libusb
#     statically from libusb-cmake, so no DLL/dylib needs to ship with the game.
#   * Not available on Android, iOS, Switch or the UWP/Xbox build (no raw USB access).
# On Windows the adapter must be bound to the WinUSB driver (e.g. via Zadig or
# Dolphin's driver installer) for libusb to open it.
option(LUS_GCADAPTER "Native GameCube adapter support (libusb)" ON)
option(LUS_GCADAPTER_FETCH_LIBUSB "Always build libusb from source instead of using the system one" OFF)
if (LUS_GCADAPTER AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android" AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS"
    AND NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch" AND NOT BATTLESHIP_UWP
    AND NOT CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    set(_LUS_LIBUSB_TARGET "")
    if (NOT LUS_GCADAPTER_FETCH_LIBUSB AND (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME MATCHES "BSD"))
        find_package(PkgConfig QUIET)
        if (PkgConfig_FOUND)
            pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
        endif()
        if (TARGET PkgConfig::LIBUSB)
            set(_LUS_LIBUSB_TARGET PkgConfig::LIBUSB)
            message(STATUS "GameCube adapter: system libusb ${LIBUSB_VERSION}")
        endif()
    endif()
    if (_LUS_LIBUSB_TARGET STREQUAL "")
        include(FetchContent)
        set(LIBUSB_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(LIBUSB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(LIBUSB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(LIBUSB_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
        # netlink hotplug instead of libudev: no extra dev package, and avoids
        # the udev sysattr stalls behind issue #137.
        set(LIBUSB_ENABLE_UDEV OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            libusb_cmake
            GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
            GIT_TAG v1.0.30-0
        )
        FetchContent_MakeAvailable(libusb_cmake)
        if (TARGET usb-1.0)
            set(_LUS_LIBUSB_TARGET usb-1.0)
            message(STATUS "GameCube adapter: libusb built from source (libusb-cmake)")
        endif()
    endif()
    if (NOT _LUS_LIBUSB_TARGET STREQUAL "")
        target_link_libraries(libultraship PRIVATE ${_LUS_LIBUSB_TARGET})
        target_compile_definitions(libultraship PRIVATE LUS_HAS_GCADAPTER)
    else()
        message(STATUS "GameCube adapter: libusb unavailable, native support disabled")
    endif()
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    target_link_libraries(libultraship PRIVATE Threads::Threads)
    find_library(COCOA Cocoa)
    target_link_libraries(libultraship PRIVATE ${COCOA})

endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    find_Library(OSX_FOUNDATION Foundation)
    find_Library(OSX_AVFOUNDATION AVFoundation)
    find_library(METAL Metal)
    find_library(QUARTZCORE QuartzCore)
    find_library(COREAUDIO CoreAudio)
    find_library(AUDIOTOOLBOX AudioToolbox)
    target_link_libraries(libultraship PRIVATE ${OSX_FOUNDATION} ${OSX_AVFOUNDATION} ${METAL} ${QUARTZCORE} ${COREAUDIO} ${AUDIOTOOLBOX} ${CMAKE_DL_LIBS})
endif()

if (USE_OPENGLES)
    if (CMAKE_SYSTEM_NAME STREQUAL "Android")
        find_library(OPENGLES_LIBRARY GLESv3)
        target_link_libraries(libultraship PRIVATE GLESv3)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
        # WebGL2/GLES3 entry points are built into the emscripten runtime —
        # gl* symbols resolve at link with no library to find_library.
    elseif(CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
        target_link_libraries(libultraship PRIVATE OpenGL::GLES2)
    else()
        find_library(OPENGLES_LIBRARY GLESv2)
        target_link_libraries(libultraship PRIVATE GLESv2)
    endif()
endif()

target_link_libraries(libultraship PUBLIC prism)

# Post-process shader transpiler (glslang + SPIRV-Cross). Pulled in by
# cmake/dependencies/common.cmake when LUS_ENABLE_POSTPROCESS_TRANSPILER is
# ON (desktop default). The runtime code in src/fast/postprocess/ keys off
# the LUS_POSTPROCESS_TRANSPILER compile def to enable the transpile path.
if (LUS_ENABLE_POSTPROCESS_TRANSPILER)
    target_link_libraries(libultraship PRIVATE
        glslang
        glslang-default-resource-limits
        SPIRV
        spirv-cross-core
        spirv-cross-glsl
        spirv-cross-hlsl
        spirv-cross-msl
    )
    target_compile_definitions(libultraship PRIVATE LUS_POSTPROCESS_TRANSPILER)
endif()

# TCC source-compiled mod scripting. libtcc/libtcc1 are vendored by
# cmake/dependencies/common.cmake; DISABLE_SCRIPTING compiles the loader out.
if(NOT DISABLE_SCRIPTING)
    target_link_libraries(libultraship PUBLIC libtcc libtcc1)
else()
    target_compile_definitions(libultraship PUBLIC DISABLE_SCRIPTING)
endif()

#=================== Compile Options & Defs ===================

target_compile_definitions(libultraship PRIVATE ${GBI_UCODE})

if(BATTLESHIP_UWP)
    target_compile_definitions(libultraship PUBLIC _UWP=1 BATTLESHIP_UWP=1)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        WIN32
        _CONSOLE
        _CRT_SECURE_NO_WARNINGS
        ENABLE_DX11
        NOMINMAX
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set_target_properties(${PROJECT_NAME} PROPERTIES
        XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
    )
endif()

set(SPDLOG_LEVEL_TRACE 0)
set(SPDLOG_LEVEL_DEBUG 1)
set(SPDLOG_LEVEL_INFO 2)
set(SPDLOG_LEVEL_WARN 3)
set(SPDLOG_LEVEL_ERROR 4)
set(SPDLOG_LEVEL_CRITICAL 5)
set(SPDLOG_LEVEL_OFF 6)
set(LOG_LEVELS "SPDLOG_LEVEL_TRACE;SPDLOG_LEVEL_DEBUG;SPDLOG_LEVEL_INFO;SPDLOG_LEVEL_WARN;SPDLOG_LEVEL_ERROR;SPDLOG_LEVEL_CRITICAL;SPDLOG_LEVEL_OFF")
set(SPDLOG_MIN_CUTOFF SPDLOG_LEVEL_TRACE CACHE STRING "The minimum log level that will be available to a build. Can be overridden in cmake or in InitLogging")
set_property(CACHE SPDLOG_MIN_CUTOFF PROPERTY STRINGS ${LOG_LEVELS})
if(NOT SPDLOG_MIN_CUTOFF IN_LIST LOG_LEVELS)
    message(FATAL_ERROR "SPDLOG_MIN_CUTOFF must be one of ${LOG_LEVELS}")
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_compile_definitions(libultraship PRIVATE
        $<$<CONFIG:Debug>:_DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
        SPDLOG_ACTIVE_LEVEL=${SPDLOG_MIN_CUTOFF}
        __IOS__
    )

    set_xcode_property(${PROJECT_NAME} PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID} All)

    if(NOT SIGN_LIBRARY)
        set_target_properties(${PROJECT_NAME} PROPERTIES
            XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
            XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
        )

        if(TARGET SDL2)
            set_target_properties(SDL2 PROPERTIES
                XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
                XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
            )
        endif()

        set_target_properties(zip PROPERTIES
            XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
            XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
        )
    endif()
else()
    target_compile_definitions(libultraship PRIVATE
        ENABLE_OPENGL
        $<$<CONFIG:Debug>:_DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
        SPDLOG_ACTIVE_LEVEL=${SPDLOG_MIN_CUTOFF}
        $<$<BOOL:${USE_OPENGLES}>:USE_OPENGLES>
        $<$<BOOL:${GFX_DEBUG_DISASSEMBLER}>:GFX_DEBUG_DISASSEMBLER>
    )
endif()

if(MSVC)
    target_compile_options(libultraship PRIVATE
        $<$<CONFIG:Debug>:
            /Od;
            /Oi-
        >
        $<$<CONFIG:Release>:
            /Oi;
            /Gy
        >
        /permissive-;
        /MP;
        /sdl;
        /W3;
        ${DEFAULT_CXX_DEBUG_INFORMATION_FORMAT};
        ${DEFAULT_CXX_EXCEPTION_HANDLING};
    )
    target_link_options(libultraship PRIVATE
        $<$<CONFIG:Release>:
            /OPT:REF;
            /OPT:ICF
        >
        /SUBSYSTEM:CONSOLE
    )
endif()

if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU|Clang")
	target_compile_options(libultraship PRIVATE
		-Wall
		-Wextra
		-Wno-error
		-Wno-unused-variable
		-Wno-unused-parameter
		-Wno-unused-function
		-Wno-parentheses
		-Wno-narrowing
		-Wno-missing-field-initializers
        $<$<COMPILE_LANGUAGE:C,OBJC>:-Wno-implicit-function-declaration>
        $<$<COMPILE_LANGUAGE:C,OBJC>:-Wno-int-conversion>
	)
    if (MSVC)
    target_compile_options(libultraship PRIVATE
        -Wno-implicit-function-declaration
    )
    endif()
endif()
