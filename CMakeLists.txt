# Top-level CMake build for the Spring executable and its bundled third-party
# dependencies.

cmake_minimum_required (VERSION 4.2)
project (SPRING)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CheckIPOSupported)

function(extract_archive_if_needed archive_path destination_dir expected_path)
    file(SHA256 "${archive_path}" archive_hash)
    set(stamp_path "${destination_dir}/.spring-archive-sha256")
    set(should_extract TRUE)

    if (EXISTS "${expected_path}" AND EXISTS "${stamp_path}")
        file(READ "${stamp_path}" recorded_hash)
        string(STRIP "${recorded_hash}" recorded_hash)
        if (recorded_hash STREQUAL archive_hash)
            set(should_extract FALSE)
        endif ()
    endif ()

    if (should_extract)
        file(REMOVE_RECURSE "${destination_dir}")
        file(MAKE_DIRECTORY "${destination_dir}")
        file(ARCHIVE_EXTRACT INPUT "${archive_path}" DESTINATION "${destination_dir}")
        file(WRITE "${stamp_path}" "${archive_hash}\n")
    endif ()
endfunction()

function(run_command_checked working_directory description)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${working_directory}"
        RESULT_VARIABLE command_result
    )
    if (NOT command_result EQUAL 0)
        message(FATAL_ERROR "${description} failed with exit code ${command_result}")
    endif ()
endfunction()

# The project is validated against GCC and Clang toolchains.
if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
	message(STATUS "Detected compiler: GNU Compiler Collection")
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
	message(STATUS "Detected compiler: LLVM Clang or Apple Clang")
else ()
	message(FATAL_ERROR "Compiler currently not supported by CMake build: ${CMAKE_CXX_COMPILER_ID}")
endif()

if (NOT WIN32)
        check_ipo_supported(RESULT result)
        if(result)
            message(STATUS "Support for Interprocedural Optimization detected.")
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()
endif ()

# Native tuning is the default; portability mode restricts x86-family code
# generation to SSE4.1-compatible instructions.
option(spring_optimize_for_native "Build spoa with -march=native" ON)
option(spring_optimize_for_portability "Build spoa with -msse4.1" OFF)

set(SPRING_X86_FAMILY FALSE)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
    set(SPRING_X86_FAMILY TRUE)
endif ()

message(STATUS "Setting compiler flags")
set(FLAGS "${FLAGS} -O3")
if (spring_optimize_for_portability)
    if (SPRING_X86_FAMILY)
        set(FLAGS "${FLAGS} -msse4.1")
    else ()
        message(STATUS "spring_optimize_for_portability requested on non-x86 target ${CMAKE_SYSTEM_PROCESSOR}; skipping -msse4.1")
    endif ()
elseif (spring_optimize_for_native)
    set(FLAGS "${FLAGS} -march=native")
endif ()
set(FLAGS "${FLAGS} -fomit-frame-pointer")
set(FLAGS "${FLAGS} -fstrict-aliasing")
set(FLAGS "${FLAGS} -ffast-math")

if (CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo)$")
    set(FLAGS "${FLAGS} -g")
endif ()

set(CMAKE_CXX_FLAGS "${FLAGS}")
set(CMAKE_C_FLAGS "${FLAGS}")

# OpenMP is required across Spring and several bundled compression components.
FIND_PACKAGE( OpenMP REQUIRED)
message(STATUS "OPENMP FOUND")
set(OpenMP_DEFINITIONS "-DLIBBSC_OPENMP_SUPPORT -DLIBSAIS_OPENMP")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS} ${OpenMP_DEFINITIONS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS} ${OpenMP_DEFINITIONS}")

execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=include
                OUTPUT_VARIABLE COMPILER_INCLUDE_DIR
                OUTPUT_STRIP_TRAILING_WHITESPACE)
# Emit a matching clangd configuration so editor diagnostics see the same
# compiler include path and OpenMP flags as the CMake build.
file(WRITE "${CMAKE_SOURCE_DIR}/.clangd" "CompileFlags:\n  Add: [\"-I${COMPILER_INCLUDE_DIR}\", \"-fopenmp\"]\n")

message(STATUS "** CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
message(STATUS "** CMAKE_C_FLAGS: ${CMAKE_C_FLAGS}")

set(source_dir ${CMAKE_SOURCE_DIR}/src)
set(vendor_dir ${CMAKE_SOURCE_DIR}/vendor)
set(build_vendor_dir ${CMAKE_BINARY_DIR}/vendor)
set(BBHASH_DIR ${build_vendor_dir}/BBHash)
set(CLOUDFLARE_ZLIB_DIR ${build_vendor_dir}/cloudflare_zlib)
set(ID_COMPRESSION_DIR ${build_vendor_dir}/id_compression)
set(INDEXED_BZIP2_DIR ${build_vendor_dir}/indexed_bzip2)
set(LIBBSC_DIR ${build_vendor_dir}/libbsc)
set(LIBDEFLATE_DIR ${build_vendor_dir}/libdeflate)
set(QVZ_DIR ${build_vendor_dir}/qvz)
set(include_dir ${CMAKE_SOURCE_DIR}/src)

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/cloudflare_zlib.tar.xz"
    "${CLOUDFLARE_ZLIB_DIR}"
    "${CLOUDFLARE_ZLIB_DIR}/zlib.h"
)

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/BBHash.tar.xz"
    "${BBHASH_DIR}"
    "${BBHASH_DIR}/BooPHF.h"
)

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/id_compression.tar.xz"
    "${ID_COMPRESSION_DIR}"
    "${ID_COMPRESSION_DIR}/include/sam_block.h"
)

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/libbsc.tar.xz"
    "${LIBBSC_DIR}"
    "${LIBBSC_DIR}/bsc.cpp"
)

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/qvz.tar.xz"
    "${QVZ_DIR}"
    "${QVZ_DIR}/src/qvz.cpp"
)

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/libdeflate.tar.xz"
    "${LIBDEFLATE_DIR}"
    "${LIBDEFLATE_DIR}/libdeflate.h"
)

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/indexed_bzip2.tar.xz"
    "${INDEXED_BZIP2_DIR}"
    "${INDEXED_BZIP2_DIR}/CMakeLists.txt"
)

set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${LIBDEFLATE_DIR}" "${CMAKE_BINARY_DIR}/libdeflate-build")

add_library(spring_cloudflare_zlib STATIC
    ${CLOUDFLARE_ZLIB_DIR}/adler32.c
    ${CLOUDFLARE_ZLIB_DIR}/compress.c
    ${CLOUDFLARE_ZLIB_DIR}/crc32.c
    ${CLOUDFLARE_ZLIB_DIR}/deflate.c
    ${CLOUDFLARE_ZLIB_DIR}/gzclose.c
    ${CLOUDFLARE_ZLIB_DIR}/gzlib.c
    ${CLOUDFLARE_ZLIB_DIR}/gzread.c
    ${CLOUDFLARE_ZLIB_DIR}/gzwrite.c
    ${CLOUDFLARE_ZLIB_DIR}/inffast.c
    ${CLOUDFLARE_ZLIB_DIR}/inflate.c
    ${CLOUDFLARE_ZLIB_DIR}/inftrees.c
    ${CLOUDFLARE_ZLIB_DIR}/trees.c
    ${CLOUDFLARE_ZLIB_DIR}/uncompr.c
    ${CLOUDFLARE_ZLIB_DIR}/zutil.c
)
target_include_directories(spring_cloudflare_zlib PUBLIC ${CLOUDFLARE_ZLIB_DIR})
add_library(ZLIB::ZLIB ALIAS spring_cloudflare_zlib)

set(LIBRAPIDARCHIVE_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_BUILD_TOOLS ON CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_RUN_CLANG_TIDY OFF CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_USE_SYSTEM_ZLIB ON CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_USE_ZLIB_NG OFF CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_WITH_RPMALLOC OFF CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_WITH_ISAL ON CACHE BOOL "" FORCE)
add_subdirectory("${INDEXED_BZIP2_DIR}" "${CMAKE_BINARY_DIR}/indexed_bzip2-build" EXCLUDE_FROM_ALL)

# The Spring executable is built from the core pipeline plus bundled
# third-party compression libraries.
list(APPEND source_files
    # spring
    ${source_dir}/main.cpp
    ${source_dir}/spring.cpp
    ${source_dir}/util.cpp
    ${source_dir}/bitset_dictionary.cpp
    ${source_dir}/preprocess.cpp
    ${source_dir}/encoder.cpp
    ${source_dir}/reordered_streams.cpp
    ${source_dir}/paired_end_order.cpp
    ${source_dir}/reordered_quality_id.cpp
    ${source_dir}/decompress.cpp
    ${source_dir}/template_dispatch.cpp

    # id compression
    ${ID_COMPRESSION_DIR}/src/Arithmetic_stream.cpp
    ${ID_COMPRESSION_DIR}/src/compression.cpp
    ${ID_COMPRESSION_DIR}/src/id_compression.cpp
    ${ID_COMPRESSION_DIR}/src/io_functions.cpp
    ${ID_COMPRESSION_DIR}/src/sam_file_allocation.cpp
    ${ID_COMPRESSION_DIR}/src/sam_models.cpp
    ${ID_COMPRESSION_DIR}/src/stream_model.cpp

    # bsc
    ${LIBBSC_DIR}/bsc.cpp
    ${LIBBSC_DIR}/bsc_str_array.cpp
    ${LIBBSC_DIR}/libbsc/adler32/adler32.cpp
    ${LIBBSC_DIR}/libbsc/bwt/libsais/libsais.c
    ${LIBBSC_DIR}/libbsc/bwt/bwt.cpp
    ${LIBBSC_DIR}/libbsc/coder/coder.cpp
    ${LIBBSC_DIR}/libbsc/coder/qlfc/qlfc.cpp
    ${LIBBSC_DIR}/libbsc/coder/qlfc/qlfc_model.cpp
    ${LIBBSC_DIR}/libbsc/filters/detectors.cpp
    ${LIBBSC_DIR}/libbsc/filters/preprocessing.cpp
    ${LIBBSC_DIR}/libbsc/libbsc/libbsc.cpp
    ${LIBBSC_DIR}/libbsc/lzp/lzp.cpp
    ${LIBBSC_DIR}/libbsc/platform/platform.cpp
    ${LIBBSC_DIR}/libbsc/st/st.cpp

    # qvz
    ${QVZ_DIR}/src/cluster.cpp
    ${QVZ_DIR}/src/codebook.cpp
    ${QVZ_DIR}/src/distortion.cpp
    ${QVZ_DIR}/src/lines.cpp
    ${QVZ_DIR}/src/pmf.cpp
    ${QVZ_DIR}/src/quantizer.cpp
    ${QVZ_DIR}/src/qv_compressor.cpp
    ${QVZ_DIR}/src/qvz.cpp
    ${QVZ_DIR}/src/util.cpp
    ${QVZ_DIR}/src/well.cpp
)

if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU|Clang")
    set(vendored_unused_result_warning_sources
        ${ID_COMPRESSION_DIR}/src/Arithmetic_stream.cpp
        ${ID_COMPRESSION_DIR}/src/io_functions.cpp
        ${QVZ_DIR}/src/codebook.cpp
    )
    set_source_files_properties(
        ${vendored_unused_result_warning_sources}
        PROPERTIES COMPILE_OPTIONS "-Wno-unused-result"
    )
endif ()

# Convenience targets for repository development checks.
function(add_dev_script_target target_name script_name)
    add_custom_target(${target_name}
            COMMAND ${CMAKE_SOURCE_DIR}/dev/${script_name}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            USES_TERMINAL
    )
endfunction()

add_executable (spring ${source_files})

# Spring headers live in src/, while vendor/ exposes bundled libraries such
# as BBHash, libbsc, id_compression, qvz, and the vendored libdeflate sources.
target_include_directories(spring PRIVATE ${include_dir} ${vendor_dir} ${build_vendor_dir})
target_link_libraries(spring PUBLIC
    libdeflate::libdeflate_static
    ZLIB::ZLIB
)
target_compile_definitions(spring PRIVATE SPRING_RAPIDGZIP_EXECUTABLE=\"$<TARGET_FILE:rapidgzip>\")

add_dependencies(spring rapidgzip)

add_dev_script_target(dev-lint lint.sh)
add_dev_script_target(dev-cppcheck cppcheck.sh)
add_dev_script_target(dev-cbindgen-check cbindgen-check.sh)
add_dev_script_target(dev-valgrind-smoke valgrind-smoke.sh)

add_dependencies(dev-lint spring)
add_dependencies(dev-valgrind-smoke spring)
