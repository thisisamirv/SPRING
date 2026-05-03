# ################################################################
# ZSTD Build Targets Configuration
# ################################################################

# Always build the library first (this defines ZSTD_BUILD_STATIC/SHARED options)
include(ZstdLibBuild)

# Clean-all target for thorough cleanup
add_custom_target(clean-all
    COMMAND ${CMAKE_BUILD_TOOL} clean
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/
    COMMENT "Performing complete clean including build directory"
)
