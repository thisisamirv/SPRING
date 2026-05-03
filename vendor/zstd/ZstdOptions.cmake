# ################################################################
# ZSTD Build Options Configuration
# ################################################################

# Legacy support is disabled in the lean vendored build.
add_definitions(-DZSTD_LEGACY_SUPPORT=0)

# Platform-specific options
if(APPLE)
    option(ZSTD_FRAMEWORK "Build as Apple Framework" OFF)
endif()

# Android-specific configuration
if(ANDROID)
    set(ZSTD_MULTITHREAD_SUPPORT_DEFAULT OFF)
    # Handle old Android API levels
    if((NOT ANDROID_PLATFORM_LEVEL) OR (ANDROID_PLATFORM_LEVEL VERSION_LESS 24))
        message(STATUS "Configuring for old Android API - disabling fseeko/ftello")
        add_compile_definitions(LIBC_NO_FSEEKO)
    endif()
else()
    set(ZSTD_MULTITHREAD_SUPPORT_DEFAULT ON)
endif()

# Multi-threading support is disabled in the lean vendored build.
message(STATUS "Multi-threading support disabled")

# Build component options
# MSVC-specific options
if(MSVC)
    option(ZSTD_USE_STATIC_RUNTIME "Link to static runtime libraries" OFF)
endif()

# Set global definitions
add_definitions(-DXXH_NAMESPACE=ZSTD_)
