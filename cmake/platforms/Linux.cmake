# Linux-specific settings for ${PROJECT_NAME}. Self-guarded; safe to
# include unconditionally. Requires the main target to already exist.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(${PROJECT_NAME} PRIVATE -Wl,-export-dynamic)
endif()
