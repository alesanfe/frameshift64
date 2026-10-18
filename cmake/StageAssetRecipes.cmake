cmake_minimum_required(VERSION 3.24)

# Copy extraction inputs only into an owned build/package directory.
# configure_file preserves timestamps when bytes have not changed. Remove
# obsolete staged recipes too, so renamed assets cannot be extracted twice.
function(ssb64_stage_asset_recipes source destination version)
    if(NOT version MATCHES "^(us|jp)$")
        message(FATAL_ERROR "Unsupported asset recipe region: ${version}")
    endif()
    if(source STREQUAL destination)
        message(FATAL_ERROR "Asset recipes must be staged outside the source directory")
    endif()
    file(GLOB_RECURSE recipes RELATIVE "${source}/assets/game"
        "${source}/assets/game/${version}/*.yml"
        "${source}/assets/game/${version}/*.yaml")
    if(NOT recipes)
        message(FATAL_ERROR "No ${version} asset recipes found in ${source}/assets/game")
    endif()
    file(GLOB_RECURSE staged RELATIVE "${destination}"
        "${destination}/yamls/${version}/*.yml" "${destination}/yamls/${version}/*.yaml")
    foreach(path IN LISTS staged)
        if(NOT path IN_LIST recipes)
            file(REMOVE "${destination}/${path}")
        endif()
    endforeach()
    foreach(path IN LISTS recipes)
        # Staged layout keeps torch's expected "yamls/<v>/..." shape under
        # DESTINATION_DIR; the source tree stores them under
        # assets/game/.
        configure_file("${source}/assets/game/${path}"
                       "${destination}/yamls/${path}" COPYONLY)
    endforeach()
    # Torch reads "config.yml" from its CWD at runtime — the staged name is
    # part of its contract even though the source lives in config/project/.
    configure_file("${source}/config/project/config.yml" "${destination}/config.yml" COPYONLY)
    # i18n string tables (src/i18n/Locale.cpp loads lang/<code>.json
    # next to the binary; missing files simply keep the English source
    # literals).
    file(GLOB langs "${source}/assets/lang/*.json")
    # catalog.json is a dev-time extraction artifact, not a runtime table.
    list(FILTER langs EXCLUDE REGEX "catalog\\.json$")
    file(MAKE_DIRECTORY "${destination}/lang")
    # Drop stale staged tables too (renamed/removed languages, catalog.json
    # left behind by builds from before the exclusion above).
    file(GLOB staged_langs RELATIVE "${destination}/lang" "${destination}/lang/*.json")
    foreach(lang_name IN LISTS staged_langs)
        if(NOT EXISTS "${source}/assets/lang/${lang_name}"
           OR lang_name STREQUAL "catalog.json")
            file(REMOVE "${destination}/lang/${lang_name}")
        endif()
    endforeach()
    foreach(path IN LISTS langs)
        get_filename_component(lang_name "${path}" NAME)
        configure_file("${path}" "${destination}/lang/${lang_name}" COPYONLY)
    endforeach()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
    ssb64_stage_asset_recipes("${SOURCE_DIR}" "${DESTINATION_DIR}" "${VERSION}")
endif()
