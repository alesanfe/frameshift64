# Strict warnings for FIRST-PARTY port code only (src/). Attach to the
# ssb64_port_options interface so smash_* modules + exe inherit them;
# external code (decomp/, libultraship/, torch/, FetchContent deps) never
# sees these flags — their warnings must not gate our build.
#
# SSB64_WARNINGS_AS_ERRORS=ON promotes warnings to errors for the same
# first-party scope only.

function(ssb64_apply_warnings _iface)
    if(MSVC)
        target_compile_options(${_iface} INTERFACE
            /W4 /permissive- /utf-8
            $<$<BOOL:${SSB64_WARNINGS_AS_ERRORS}>:/WX>)
    else()
        target_compile_options(${_iface} INTERFACE
            -Wall -Wextra -Wpedantic
            $<$<BOOL:${SSB64_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()
endfunction()
