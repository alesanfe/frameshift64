################################################################################
# Copy runtime data files to build directory (desktop only — Android stages
# these into the APK assets/ at Gradle time via android/app/build.gradle.kts).
################################################################################

# f3d.o2r is a zip of source shaders — `cmake -E tar` runs on the host in
# every configuration, including cross builds. Define it for all platforms
# so BundleSwitch (below) can consume ${CMAKE_BINARY_DIR}/f3d.o2r instead
# of a stale copy committed at the source root.
if (NOT CMAKE_SYSTEM_NAME STREQUAL "Android" AND NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    configure_file(
        ${CMAKE_SOURCE_DIR}/config/controllers/gamecontrollerdb.txt
        ${CMAKE_BINARY_DIR}/gamecontrollerdb.txt
        COPYONLY
    )

    add_custom_target(GenerateF3DO2R
        BYPRODUCTS ${CMAKE_BINARY_DIR}/f3d.o2r
        COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_BINARY_DIR}/f3d.o2r
        # add_custom_target ignores WORKING_DIRECTORY — chdir explicitly so the
        # relative `shaders` input resolves regardless of the caller's CWD.
        COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_SOURCE_DIR}/components/libultraship/src/fast
            ${CMAKE_COMMAND} -E tar cf ${CMAKE_BINARY_DIR}/f3d.o2r --format=zip shaders
        COMMENT "Packaging Fast3D shaders into f3d.o2r"
    )

    add_dependencies(${PROJECT_NAME} GenerateF3DO2R)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/fonts
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/yamls/${SSB64_VERSION}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_BINARY_DIR}/f3d.o2r
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/f3d.o2r
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/config/controllers/gamecontrollerdb.txt
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/gamecontrollerdb.txt
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/assets/fonts/Montserrat-Regular.ttf
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/fonts/Montserrat-Regular.ttf
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/assets/fonts/Inconsolata-Regular.ttf
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/assets/fonts/Inconsolata-Regular.ttf
        # config.yml, the extraction yamls (incl. deblob slice maps), and the
        # torch sidecar are staged by the StageRuntimeAssets target — a real
        # dependency edge that also runs when ssb64 did not relink.
        # CSS stage asset PNGs (final_destination_background.png, etc.) are
        # copied in a separate POST_BUILD step added by the DeriveStageAssets
        # block further below, after SSB64_BASEROM is resolved.
        # Arrow data is baked into the binary via arrow_data.h / arrow_left_data.h.
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android" OR CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    # Android: Gradle's stageGameAssets task picks the archive up at APK build
    # time; the APK asset pipeline stages it into externalFilesDir on first
    # launch, so no POST_BUILD copies.
    # Switch: BundleSwitch copies ${CMAKE_BINARY_DIR}/f3d.o2r into
    # switch_sd/switch/BattleShip/ — same story, no next-to-binary copy.
    add_custom_target(GenerateF3DO2R
        BYPRODUCTS ${CMAKE_BINARY_DIR}/f3d.o2r
        COMMAND ${CMAKE_COMMAND} -E rm -f ${CMAKE_BINARY_DIR}/f3d.o2r
        COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_SOURCE_DIR}/components/libultraship/src/fast
            ${CMAKE_COMMAND} -E tar cf ${CMAKE_BINARY_DIR}/f3d.o2r --format=zip shaders
        COMMENT "Packaging Fast3D shaders into f3d.o2r (no next-to-binary staging)"
    )
    add_dependencies(${PROJECT_NAME} GenerateF3DO2R)
endif()
