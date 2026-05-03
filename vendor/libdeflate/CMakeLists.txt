cmake_minimum_required(VERSION 3.10)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "No build type selected; defaulting to Release")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build" FORCE)
endif()

if(DEFINED CMAKE_C_FLAGS_RELEASE)
    set(LIBDEFLATE_USER_SET_RELEASE_FLAGS ON)
endif()

if(POLICY CMP0092)
    cmake_policy(SET CMP0092 NEW)
endif()

set(VERSION_REGEX "#define LIBDEFLATE_VERSION_STRING[ \t]+\"([0-9\\.]+)\"")
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/libdeflate.h VERSION_STRING REGEX ${VERSION_REGEX})
string(REGEX REPLACE ${VERSION_REGEX} "\\1" VERSION_STRING "${VERSION_STRING}")

project(libdeflate
    LANGUAGES C
    VERSION ${VERSION_STRING})

include(CheckCCompilerFlag)

option(LIBDEFLATE_BUILD_STATIC_LIB "Build the static library" ON)
option(LIBDEFLATE_BUILD_SHARED_LIB "Build the shared library" ON)
option(LIBDEFLATE_COMPRESSION_SUPPORT "Support compression" ON)
option(LIBDEFLATE_DECOMPRESSION_SUPPORT "Support decompression" ON)
option(LIBDEFLATE_ZLIB_SUPPORT "Support the zlib format" ON)
option(LIBDEFLATE_GZIP_SUPPORT "Support the gzip format" ON)
option(LIBDEFLATE_FREESTANDING "Build a freestanding library" OFF)

if(NOT LIBDEFLATE_USER_SET_RELEASE_FLAGS)
    set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG"
        CACHE STRING "C compiler flags for release builds" FORCE)
endif()
set(CMAKE_C_STANDARD 99)
if(NOT MSVC)
    check_c_compiler_flag(-Wdeclaration-after-statement HAVE_WDECLARATION_AFTER_STATEMENT)
    check_c_compiler_flag(-Wimplicit-fallthrough HAVE_WIMPLICIT_FALLTHROUGH)
    check_c_compiler_flag(-Wmissing-field-initializers HAVE_WMISSING_FIELD_INITIALIZERS)
    check_c_compiler_flag(-Wmissing-prototypes HAVE_WMISSING_PROTOTYPES)
    check_c_compiler_flag(-Wpedantic HAVE_WPEDANTIC)
    check_c_compiler_flag(-Wshadow HAVE_WSHADOW)
    check_c_compiler_flag(-Wstrict-prototypes HAVE_WSTRICT_PROTOTYPES)
    check_c_compiler_flag(-Wundef HAVE_WUNDEF)
    check_c_compiler_flag(-Wvla HAVE_WVLA)
    add_compile_options(
        -Wall
        $<$<BOOL:${HAVE_WDECLARATION_AFTER_STATEMENT}>:-Wdeclaration-after-statement>
        $<$<BOOL:${HAVE_WIMPLICIT_FALLTHROUGH}>:-Wimplicit-fallthrough>
        $<$<BOOL:${HAVE_WMISSING_FIELD_INITIALIZERS}>:-Wmissing-field-initializers>
        $<$<BOOL:${HAVE_WMISSING_PROTOTYPES}>:-Wmissing-prototypes>
        $<$<BOOL:${HAVE_WPEDANTIC}>:-Wpedantic>
        $<$<BOOL:${HAVE_WSHADOW}>:-Wshadow>
        $<$<BOOL:${HAVE_WSTRICT_PROTOTYPES}>:-Wstrict-prototypes>
        $<$<BOOL:${HAVE_WUNDEF}>:-Wundef>
        $<$<BOOL:${HAVE_WVLA}>:-Wvla>
    )
endif()
if(LIBDEFLATE_FREESTANDING)
    add_definitions(-DFREESTANDING)
    list(APPEND LIB_COMPILE_OPTIONS -ffreestanding -nostdlib)
    list(APPEND LIB_LINK_LIBRARIES -ffreestanding -nostdlib)
endif()

set(LIB_SOURCES
    common_defs.h
    libdeflate.h
    cpu_features_arm.c
    cpu_features_arm.h
    cpu_features_common.h
    deflate_constants.h
    lib_common.h
    utils.c
    cpu_features_x86.c
    cpu_features_x86.h
)
if(LIBDEFLATE_COMPRESSION_SUPPORT)
    list(APPEND LIB_SOURCES
        matchfinder_impl_arm.h
        bt_matchfinder.h
        deflate_compress.c
        deflate_compress.h
        hc_matchfinder.h
        ht_matchfinder.h
        matchfinder_common.h
        matchfinder_impl_riscv.h
        matchfinder_impl_x86.h
    )
endif()
if(LIBDEFLATE_DECOMPRESSION_SUPPORT)
    list(APPEND LIB_SOURCES
        decompress_template.h
        deflate_decompress.c
        decompress_impl.h
    )
endif()
if(LIBDEFLATE_ZLIB_SUPPORT)
    list(APPEND LIB_SOURCES
        adler32.c
        adler32_impl_arm.h
        adler32_impl_x86.h
        adler32_template.h
        zlib_constants.h
    )
    if(LIBDEFLATE_COMPRESSION_SUPPORT)
        list(APPEND LIB_SOURCES zlib_compress.c)
    endif()
    if(LIBDEFLATE_DECOMPRESSION_SUPPORT)
        list(APPEND LIB_SOURCES zlib_decompress.c)
    endif()
endif()
if(LIBDEFLATE_GZIP_SUPPORT)
    list(APPEND LIB_SOURCES
        crc32_impl_arm.h
        crc32_pmull_helpers.h
        crc32_pmull_wide.h
        crc32.c
        crc32_multipliers.h
        crc32_tables.h
        gzip_constants.h
        crc32_impl_x86.h
        crc32_pclmul_template.h
    )
    if(LIBDEFLATE_COMPRESSION_SUPPORT)
        list(APPEND LIB_SOURCES gzip_compress.c)
    endif()
    if(LIBDEFLATE_DECOMPRESSION_SUPPORT)
        list(APPEND LIB_SOURCES gzip_decompress.c)
    endif()
endif()

set(LIB_INCLUDE_DIRS
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

if(LIBDEFLATE_BUILD_STATIC_LIB)
    add_library(libdeflate_static STATIC ${LIB_SOURCES})
    add_library(libdeflate::libdeflate_static ALIAS libdeflate_static)

    if(WIN32 AND NOT MINGW)
        set(STATIC_LIB_NAME deflatestatic)
    else()
        set(STATIC_LIB_NAME deflate)
    endif()
    set_target_properties(libdeflate_static PROPERTIES
        OUTPUT_NAME ${STATIC_LIB_NAME}
        PUBLIC_HEADER libdeflate.h)
    target_include_directories(libdeflate_static PUBLIC ${LIB_INCLUDE_DIRS})
    target_compile_definitions(libdeflate_static PRIVATE ${LIB_COMPILE_DEFINITIONS})
    target_compile_options(libdeflate_static PRIVATE ${LIB_COMPILE_OPTIONS})
endif()

