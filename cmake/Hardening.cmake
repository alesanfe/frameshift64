# Production hardening for the shipped binary. Defaults ON
# (SSB64_ENABLE_HARDENING); sanitizer/dev presets may layer extra flags.
# Applied to the ssb64_port_options interface → port code only; linker
# flags land on the final binary via the same interface.

function(ssb64_apply_hardening _iface)
    if(NOT SSB64_ENABLE_HARDENING)
        return()
    endif()
    if(MSVC)
        # /guard:cf at compile time emits CFG instrumentation; /sdl is
        # intentionally absent — it promotes C4996 (e.g. getenv) to errors
        # and this codebase uses those APIs pervasively.
        target_compile_options(${_iface} INTERFACE /guard:cf)
        target_link_options(${_iface} INTERFACE
            /guard:cf /DYNAMICBASE /NXCOMPAT
            $<$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},AMD64>:/CETCOMPAT>
            $<$<STREQUAL:${CMAKE_SYSTEM_PROCESSOR},x64>:/CETCOMPAT>)
    elseif(APPLE)
        target_compile_options(${_iface} INTERFACE -fstack-protector-strong)
        target_link_options(${_iface} INTERFACE -Wl,-pie)
    elseif(EMSCRIPTEN)
        # wasm has no PIE/relro concept — the GNU link flags harden ELF
        # binaries but here they make wasm-ld treat the decomp's K&R
        # signature mismatches as fatal errors.
        target_compile_options(${_iface} INTERFACE -fstack-protector-strong)
    else() # ELF (linux / switch homebrew devkitA64 ignores unknown -Wl)
        target_compile_options(${_iface} INTERFACE
            -fstack-protector-strong
            $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>)
        target_link_options(${_iface} INTERFACE
            -Wl,-z,relro -Wl,-z,now -pie)
    endif()
endfunction()
