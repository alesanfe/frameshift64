# ASan / UBSan for first-party code. Sanitized binaries are DEV-ONLY —
# release packaging must never ship them (release-binary-check scans for
# the runtime markers too).
#
# MSVC supports AddressSanitizer (/fsanitize=address, VS2019 16.9+); UBSan
# is Clang/GCC-only and warns + skips on MSVC.

function(ssb64_apply_sanitizers _iface)
    set(_copts "")
    set(_lopts "")
    if(SSB64_ENABLE_ASAN)
        if(MSVC)
            list(APPEND _copts /fsanitize=address)
        else()
            list(APPEND _copts -fsanitize=address -fno-omit-frame-pointer)
            list(APPEND _lopts -fsanitize=address)
        endif()
    endif()
    if(SSB64_ENABLE_UBSAN)
        if(MSVC)
            message(WARNING "SSB64_ENABLE_UBSAN: not supported on MSVC — skipped")
        else()
            list(APPEND _copts -fsanitize=undefined -fno-omit-frame-pointer)
            list(APPEND _lopts -fsanitize=undefined)
        endif()
    endif()
    if(_copts)
        target_compile_options(${_iface} INTERFACE ${_copts})
    endif()
    if(_lopts)
        target_link_options(${_iface} INTERFACE ${_lopts})
    endif()
    if(SSB64_ENABLE_ASAN OR SSB64_ENABLE_UBSAN)
        message(STATUS "Sanitizers: ASan=${SSB64_ENABLE_ASAN} UBSan=${SSB64_ENABLE_UBSAN} (dev build — do not ship)")
    endif()
endfunction()
