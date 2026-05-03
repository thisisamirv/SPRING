# ################################################################
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under both the BSD-style license (found in the
# LICENSE file in the root directory of this source tree) and the GPLv2 (found
# in the COPYING file in the root directory of this source tree).
# ################################################################
option(ZSTD_BUILD_STATIC "BUILD STATIC LIBRARIES" ON)
option(ZSTD_BUILD_SHARED "BUILD SHARED LIBRARIES" ON)
option(ZSTD_BUILD_COMPRESSION "BUILD COMPRESSION MODULE" ON)
option(ZSTD_BUILD_DECOMPRESSION "BUILD DECOMPRESSION MODULE" ON)
option(ZSTD_BUILD_DEPRECATED "BUILD DEPRECATED MODULE" OFF)
set(ZSTDLIB_VISIBLE "" CACHE STRING "Visibility for ZSTDLIB API")
set(ZSTDERRORLIB_VISIBLE "" CACHE STRING "Visibility for ZSTDERRORLIB_VISIBLE API")
set(ZDICTLIB_VISIBLE "" CACHE STRING "Visibility for ZDICTLIB_VISIBLE API")
set(ZSTDLIB_STATIC_API "" CACHE STRING "Visibility for ZSTDLIB_STATIC_API API")
set(ZDICTLIB_STATIC_API "" CACHE STRING "Visibility for ZDICTLIB_STATIC_API API")
set_property(CACHE ZSTDLIB_VISIBLE PROPERTY STRINGS "" "hidden" "default" "protected" "internal")
set_property(CACHE ZSTDERRORLIB_VISIBLE PROPERTY STRINGS "" "hidden" "default" "protected" "internal")
set_property(CACHE ZDICTLIB_VISIBLE PROPERTY STRINGS "" "hidden" "default" "protected" "internal")
set_property(CACHE ZSTDLIB_STATIC_API PROPERTY STRINGS "" "hidden" "default" "protected" "internal")
set_property(CACHE ZDICTLIB_STATIC_API PROPERTY STRINGS "" "hidden" "default" "protected" "internal")
if(NOT ZSTD_BUILD_SHARED AND NOT ZSTD_BUILD_STATIC)
    message(SEND_ERROR "You need to build at least one flavor of libzstd")
endif()
set(CommonSources
    ${LIBRARY_DIR}/debug.c
    ${LIBRARY_DIR}/entropy_common.c
    ${LIBRARY_DIR}/error_private.c
    ${LIBRARY_DIR}/fse_decompress.c
    ${LIBRARY_DIR}/threading.c
    ${LIBRARY_DIR}/xxhash.c
    ${LIBRARY_DIR}/zstd_common.c)
set(CompressSources
    ${LIBRARY_DIR}/fse_compress.c
    ${LIBRARY_DIR}/hist.c
    ${LIBRARY_DIR}/huf_compress.c
    ${LIBRARY_DIR}/zstd_compress.c
    ${LIBRARY_DIR}/zstd_compress_literals.c
    ${LIBRARY_DIR}/zstd_compress_sequences.c
    ${LIBRARY_DIR}/zstd_compress_superblock.c
    ${LIBRARY_DIR}/zstd_double_fast.c
    ${LIBRARY_DIR}/zstd_fast.c
    ${LIBRARY_DIR}/zstd_lazy.c
    ${LIBRARY_DIR}/zstd_ldm.c
    ${LIBRARY_DIR}/zstd_opt.c
    ${LIBRARY_DIR}/zstd_preSplit.c)
set(DecompressSources
    ${LIBRARY_DIR}/huf_decompress.c
    ${LIBRARY_DIR}/zstd_ddict.c
    ${LIBRARY_DIR}/zstd_decompress.c
    ${LIBRARY_DIR}/zstd_decompress_block.c)
if(MSVC)
    add_compile_options(-DZSTD_DISABLE_ASM)
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "amd64.*|AMD64.*|x86_64.*|X86_64.*" AND ${ZSTD_HAS_NOEXECSTACK})
        enable_language(ASM)
        set(DecompressSources ${DecompressSources} ${LIBRARY_DIR}/huf_decompress_amd64.S)
    else()
        add_compile_options(-DZSTD_DISABLE_ASM)
    endif()
endif()
file(GLOB PublicHeaders ${LIBRARY_DIR}/*.h)
set(CommonHeaders "")
set(CompressHeaders "")
set(DecompressHeaders "")
set(Sources ${CommonSources})
set(Headers ${PublicHeaders} ${CommonHeaders})
if(ZSTD_BUILD_COMPRESSION)
    set(Sources ${Sources} ${CompressSources})
    set(Headers ${Headers} ${CompressHeaders})
endif()
if(ZSTD_BUILD_DECOMPRESSION)
    set(Sources ${Sources} ${DecompressSources})
    set(Headers ${Headers} ${DecompressHeaders})
endif()
if(ZSTD_BUILD_DEPRECATED)
    set(Sources ${Sources} ${DeprecatedSources})
    set(Headers ${Headers} ${DeprecatedHeaders})
endif()
if(ZSTD_LEGACY_SUPPORT)
    set(LIBRARY_LEGACY_DIR ${LIBRARY_DIR}/legacy)
    set(Sources ${Sources}
        ${LIBRARY_LEGACY_DIR}/zstd_v01.c
        ${LIBRARY_LEGACY_DIR}/zstd_v02.c
        ${LIBRARY_LEGACY_DIR}/zstd_v03.c
        ${LIBRARY_LEGACY_DIR}/zstd_v04.c
        ${LIBRARY_LEGACY_DIR}/zstd_v05.c
        ${LIBRARY_LEGACY_DIR}/zstd_v06.c
        ${LIBRARY_LEGACY_DIR}/zstd_v07.c)
    set(Headers ${Headers}
        ${LIBRARY_LEGACY_DIR}/zstd_legacy.h
        ${LIBRARY_LEGACY_DIR}/zstd_v01.h
        ${LIBRARY_LEGACY_DIR}/zstd_v02.h
        ${LIBRARY_LEGACY_DIR}/zstd_v03.h
        ${LIBRARY_LEGACY_DIR}/zstd_v04.h
        ${LIBRARY_LEGACY_DIR}/zstd_v05.h
        ${LIBRARY_LEGACY_DIR}/zstd_v06.h
        ${LIBRARY_LEGACY_DIR}/zstd_v07.h)
endif()
if(MSVC AND NOT (CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
    set(MSVC_RESOURCE_DIR ${ZSTD_SOURCE_DIR}/build/VS2010/libzstd-dll)
    set(PlatformDependResources ${MSVC_RESOURCE_DIR}/libzstd-dll.rc)
else()
    set(PlatformDependResources)
endif()
if(NOT CMAKE_ASM_COMPILER STREQUAL CMAKE_C_COMPILER)
    set_source_files_properties(${Sources} PROPERTIES LANGUAGE C)
endif()
macro(add_definition target var)
    if(NOT ("${${var}}" STREQUAL ""))
        target_compile_definitions(${target} PUBLIC "${var}=__attribute__((visibility(\"${${var}}\")))")
    endif()
endmacro()
set(PUBLIC_INCLUDE_DIRS ${LIBRARY_DIR})
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /I \"${LIBRARY_DIR}\"")
add_library(libzstd_static STATIC ${Sources} ${Headers})
target_include_directories(libzstd_static INTERFACE $<BUILD_INTERFACE:${PUBLIC_INCLUDE_DIRS}>)
add_definition(libzstd_static ZSTDLIB_VISIBLE)
add_definition(libzstd_static ZSTDERRORLIB_VISIBLE)
add_definition(libzstd_static ZDICTLIB_VISIBLE)
add_definition(libzstd_static ZSTDLIB_STATIC_API)
add_definition(libzstd_static ZDICTLIB_STATIC_API)
add_library(libzstd INTERFACE)
target_link_libraries(libzstd INTERFACE libzstd_static)
if(MSVC)
    set_property(TARGET libzstd_static APPEND PROPERTY COMPILE_DEFINITIONS "ZSTD_HEAPMODE=0;_CRT_SECURE_NO_WARNINGS")
    target_compile_options(libzstd_static PRIVATE /wd4505)
endif()
if(MSVC OR (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT MINGW))
    set(STATIC_LIBRARY_BASE_NAME zstd_static)
else()
    set(STATIC_LIBRARY_BASE_NAME zstd)
endif()
set_target_properties(
    libzstd_static
    PROPERTIES
    POSITION_INDEPENDENT_CODE On
    OUTPUT_NAME ${STATIC_LIBRARY_BASE_NAME})
if(ZSTD_FRAMEWORK)
    set_target_properties(
        libzstd_static
        PROPERTIES
        FRAMEWORK TRUE
        FRAMEWORK_VERSION "${ZSTD_FULL_VERSION}"
        PRODUCT_BUNDLE_IDENTIFIER "github.com/facebook/zstd/${STATIC_LIBRARY_BASE_NAME}"
        XCODE_ATTRIBUTE_INSTALL_PATH "@rpath"
        PUBLIC_HEADER "${PublicHeaders}"
        OUTPUT_NAME "${STATIC_LIBRARY_BASE_NAME}"
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
        MACOSX_FRAMEWORK_IDENTIFIER "github.com/facebook/zstd/${STATIC_LIBRARY_BASE_NAME}"
        MACOSX_FRAMEWORK_BUNDLE_VERSION "${ZSTD_FULL_VERSION}"
        MACOSX_FRAMEWORK_SHORT_VERSION_STRING "${ZSTD_SHORT_VERSION}"
        MACOSX_RPATH TRUE
        RESOURCE ${PublicHeaders})
endif()
