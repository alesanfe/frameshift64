################################################################################
# Codegen targets — reloc YAML regeneration, reloc stub headers, RelocFileTable,
# credits asset encoding. Needs: SSB64_VERSION, Python3_EXECUTABLE.
#
# NOTE: add_custom_target ignores WORKING_DIRECTORY — every tool invoked here
# must anchor its own paths (tools/codegen/* resolve the repo root via
# __file__.parents[2]; creditsTextConverter gets absolute args).
################################################################################
# Reloc YAML category files (same category set for every version; the
# per-version ROM offsets differ inside them). assets/configuration/extraction/${SSB64_VERSION}/ is
# produced by tools/codegen/generate_yamls.py --version ${SSB64_VERSION}.
set(SSB64_RELOC_YAML_CATEGORIES
    animations bonus effects extern_data fighters_common fighters_main
    interface items menus misc_named movies scene stages submotions
    transitions
)
set(SSB64_RELOC_YAML_FILES "")
foreach(_cat ${SSB64_RELOC_YAML_CATEGORIES})
    list(APPEND SSB64_RELOC_YAML_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/configuration/extraction/${SSB64_VERSION}/reloc_${_cat}.yml)
endforeach()

set(SSB64_CREDITS_GENERATED_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/staff.credits.encoded
    ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/staff.credits.metadata
    ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/titles.credits.encoded
    ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/titles.credits.metadata
    ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/info.credits.encoded
    ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/info.credits.metadata
    ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/companies.credits.encoded
    ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/companies.credits.metadata
)

add_custom_target(RegenerateRelocYamls
    # These are tracked authoring inputs. Declaring them as BYPRODUCTS
    # pulls this always-run target into ordinary extraction builds, and
    # even lets Ninja clean delete them. Regeneration is opt-in only.
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/codegen/generate_yamls.py --version ${SSB64_VERSION}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/codegen/generate_fighter_slices.py --version ${SSB64_VERSION}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Regenerating reloc YAML configs + deblob slice maps (${SSB64_VERSION})"
)

# One command per intent (docs/deblob.md): regenerate every deblob
# artifact — parent yamls, child slice maps, manifests, rebuild specs —
# for BOTH regions from their baseroms.
add_custom_target(DeblobRegen
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/codegen/generate_yamls.py --version us
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/codegen/generate_fighter_slices.py --version us
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/codegen/generate_yamls.py --version jp
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/codegen/generate_fighter_slices.py --version jp
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Regenerating all deblob artifacts (both regions)"
)

# AUD-29: these used to be always-run phony targets — every build rewrote
# the outputs, bumped timestamps, and forced a recompile+relink of the
# port layer. As add_custom_command OUTPUTs they now only re-run when an
# input is newer; the same-named custom targets wrap them so the
# documented entry points (`--target GenerateRelocArtifacts`) keep working.
set(SSB64_RELOC_GENERATED_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/include/${SSB64_VERSION}/reloc_data.${SSB64_VERSION}.h
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/src/${SSB64_VERSION}/RelocFileTable.${SSB64_VERSION}.cpp
)
add_custom_command(
    OUTPUT ${SSB64_RELOC_GENERATED_FILES}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/codegen/generate_reloc_stubs.py --version ${SSB64_VERSION}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/codegen/generate_reloc_table.py --version ${SSB64_VERSION}
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/codegen/generate_reloc_stubs.py
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/codegen/generate_reloc_table.py
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/codegen/reloc_data_symbols.${SSB64_VERSION}.txt
        ${SSB64_RELOC_YAML_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating reloc headers and RelocFileTable (${SSB64_VERSION})"
    VERBATIM
)
add_custom_target(GenerateRelocArtifacts DEPENDS ${SSB64_RELOC_GENERATED_FILES})

set(SSB64_CREDITS_INPUT_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/staff.credits.${SSB64_VERSION}.txt
    ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/titles.credits.${SSB64_VERSION}.txt
    ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/info.credits.${SSB64_VERSION}.txt
    ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/companies.credits.${SSB64_VERSION}.txt
)
add_custom_command(
    OUTPUT ${SSB64_CREDITS_GENERATED_FILES}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asset-pipeline/creditsTextConverter.py ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/staff.credits.${SSB64_VERSION}.txt
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asset-pipeline/creditsTextConverter.py ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/titles.credits.${SSB64_VERSION}.txt
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asset-pipeline/creditsTextConverter.py -paragraphFont -multiline ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/info.credits.${SSB64_VERSION}.txt
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asset-pipeline/creditsTextConverter.py -paragraphFont ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/companies.credits.${SSB64_VERSION}.txt
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/asset-pipeline/creditsTextConverter.py
        ${SSB64_CREDITS_INPUT_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits
    COMMENT "Encoding credits text assets (${SSB64_VERSION})"
    VERBATIM
)
add_custom_target(GenerateCreditsAssets DEPENDS ${SSB64_CREDITS_GENERATED_FILES})
