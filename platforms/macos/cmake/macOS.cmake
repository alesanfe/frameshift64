# macOS-specific settings for ${PROJECT_NAME}. Self-guarded; safe to
# include unconditionally. Requires the main target to already exist.
# IMPORTANT: the re-sign POST_BUILD must be registered LAST among the
# target's post-build steps — a later binary mutation would invalidate the
# ad-hoc signature.

# The port calls CFPreferences* directly (ApplePressAndHoldEnabled fix
# in main()), so link CoreFoundation explicitly rather than relying on it
# arriving transitively through libultraship's PRIVATE framework links.
if (APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE "-framework CoreFoundation")
endif()

# Re-sign with JIT entitlements so the mod loader's hook backend
# (funchook) can patch engine __TEXT at runtime.
#
# Apple Silicon enforces W^X on code-signed text pages: funchook's inline
# detour mprotect(RWX) is denied ("Failed to unprotect memory ...
# Permission denied") on a linker-/ad-hoc-signed binary with no
# entitlements. Re-signing ad-hoc WITH the JIT entitlements lifts that —
# the cs.allow-* keys are honored for ad-hoc signatures. Must run LAST
# (any later binary mutation would invalidate the signature). No-op on
# Intel macs too, but harmless; gated to APPLE.
#
# NOTE: the mod hook backend needs __TEXT max-protection = rwx so the
# JIT-entitled process can flip an engine code page writable to patch
# a prologue. `-Wl,-segprot,__TEXT,rwx,r-x` would do it, but Apple's new
# linker (ld-prime, ld-12xx) silently ignores -segprot. It's instead done
# as a deterministic post-link Mach-O edit below (before codesign).
if(APPLE AND NOT DISABLE_SCRIPTING)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMENT "macOS: __TEXT->rwx maxprot + ad-hoc re-sign w/ JIT entitlements (mod hook backend)"
        # 1. Raise __TEXT max-protection to rwx so the hook backend can
        #    flip an engine code page writable at runtime.
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/cmake/macho_text_rwx.py
            "$<TARGET_FILE:${PROJECT_NAME}>"
        # 2. Re-sign LAST (seals the modified binary). The cs.allow-*
        #    entitlements are honored for ad-hoc signatures.
        COMMAND codesign --force --sign - --timestamp=none
            --entitlements "${CMAKE_SOURCE_DIR}/cmake/macos_entitlements.plist"
            "$<TARGET_FILE:${PROJECT_NAME}>"
        VERBATIM
    )
endif()
