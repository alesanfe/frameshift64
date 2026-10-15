# Windows-specific settings for ${PROJECT_NAME}. Self-guarded; safe to
# include unconditionally. Requires the main target to already exist.
if (CMAKE_SYSTEM_NAME STREQUAL "Windows" AND NOT BATTLESHIP_UWP)
    target_link_options(${PROJECT_NAME} PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    target_link_libraries(${PROJECT_NAME} PRIVATE shell32)
    # DMA functions (osEPiStartDma, osPiStartDma) are provided by LUS.
    # Game DMA code is no-opped via #ifdef PORT in dma.c, so no /FORCE needed.
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
    # Embed the application icon (Explorer / taskbar / window). The .rc
    # is generated per-region from platforms/windows/resources/ssb64.rc.in: US embeds
    # assets/branding/icons/icon.ico, JP embeds assets/branding/icons/icon-jp.ico. The icon also
    # surfaces as the .exe's Explorer thumbnail because RC resources
    # carry over to the PE's icon group.
    set(SSB64_RC_ICON_PATH "${CMAKE_SOURCE_DIR}/assets/branding/icons/icon.ico")
    if(SSB64_VERSION STREQUAL "jp")
        set(SSB64_RC_ICON_PATH "${CMAKE_SOURCE_DIR}/assets/branding/icons/icon-jp.ico")
    endif()
    configure_file(
        ${CMAKE_SOURCE_DIR}/platforms/windows/resources/ssb64.rc.in
        ${CMAKE_BINARY_DIR}/src/platform/ssb64.rc
        @ONLY)
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_BINARY_DIR}/src/platform/ssb64.rc)
endif()
