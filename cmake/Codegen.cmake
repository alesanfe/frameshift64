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

add_custom_target(GenerateRelocArtifacts
    BYPRODUCTS
        ${CMAKE_CURRENT_SOURCE_DIR}/generated/include/${SSB64_VERSION}/reloc_data.${SSB64_VERSION}.h
        ${CMAKE_CURRENT_SOURCE_DIR}/generated/src/${SSB64_VERSION}/RelocFileTable.${SSB64_VERSION}.cpp
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/codegen/generate_reloc_stubs.py --version ${SSB64_VERSION}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/codegen/generate_reloc_table.py --version ${SSB64_VERSION}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating reloc headers and RelocFileTable (${SSB64_VERSION})"
)

add_custom_target(GenerateCreditsAssets
    BYPRODUCTS ${SSB64_CREDITS_GENERATED_FILES}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asset-pipeline/creditsTextConverter.py ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/staff.credits.${SSB64_VERSION}.txt
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asset-pipeline/creditsTextConverter.py ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/titles.credits.${SSB64_VERSION}.txt
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asset-pipeline/creditsTextConverter.py -paragraphFont -multiline ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/info.credits.${SSB64_VERSION}.txt
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/asset-pipeline/creditsTextConverter.py -paragraphFont ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits/companies.credits.${SSB64_VERSION}.txt
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/decomp/src/credits
    COMMENT "Encoding credits text assets (${SSB64_VERSION})"
)
