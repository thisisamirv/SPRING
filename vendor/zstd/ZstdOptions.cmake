# ################################################################
# ZSTD Build Options Configuration
# ################################################################

# Legacy support configuration (disabled by default)
option(ZSTD_LEGACY_SUPPORT "Enable legacy format support" OFF)

if(ZSTD_LEGACY_SUPPORT)
    message(STATUS "ZSTD_LEGACY_SUPPORT enabled")
    set(ZSTD_LEGACY_LEVEL 5 CACHE STRING "Legacy support level")
    add_definitions(-DZSTD_LEGACY_SUPPORT=${ZSTD_LEGACY_LEVEL})
else()
    message(STATUS "ZSTD_LEGACY_SUPPORT disabled")
    add_definitions(-DZSTD_LEGACY_SUPPORT=0)
endif()

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

# Multi-threading support
option(ZSTD_MULTITHREAD_SUPPORT "Enable multi-threading support" ${ZSTD_MULTITHREAD_SUPPORT_DEFAULT})

if(ZSTD_MULTITHREAD_SUPPORT)
    message(STATUS "Multi-threading support enabled")
else()
    message(STATUS "Multi-threading support disabled")
endif()

# Build component options
# MSVC-specific options
if(MSVC)
    option(ZSTD_USE_STATIC_RUNTIME "Link to static runtime libraries" OFF)
endif()

# C++ support is disabled in the lean vendored build.
set(ZSTD_ENABLE_CXX OFF)
if(ZSTD_ENABLE_CXX)
    enable_language(CXX)
endif()

# Set global definitions
add_definitions(-DXXH_NAMESPACE=ZSTD_)
