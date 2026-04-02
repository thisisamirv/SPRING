# Top-level CMake build for the Spring executable and its bundled third-party
# dependencies.

cmake_minimum_required (VERSION 4.2)
project (SPRING)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CheckIPOSupported)
include(FetchContent)

# Spring currently targets Unix-like environments only.
if (NOT UNIX)
	message(FATAL_ERROR "Non Unix platforms not supported currently.")
endif()

# The project is validated against GCC and Clang toolchains.
if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
	message(STATUS "Detected compiler: GNU Compiler Collection")
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
	message(STATUS "Detected compiler: LLVM Clang or Apple Clang")
else ()
	message(FATAL_ERROR "Compiler currently not supported by CMake build: ${CMAKE_CXX_COMPILER_ID}")
endif()

check_ipo_supported(RESULT result)
if(result)
  message(STATUS "Support for Interprocedural Optimization detected.")
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Native tuning is the default; portability mode restricts code generation to
# SSE4.1-compatible instructions.
option(spring_optimize_for_native "Build spoa with -march=native" ON)
option(spring_optimize_for_portability "Build spoa with -msse4.1" OFF)

message(STATUS "Setting compiler flags")
set(FLAGS "${FLAGS} -g")
set(FLAGS "${FLAGS} -O3")
if (spring_optimize_for_portability)
    set(FLAGS "${FLAGS} -msse4.1")
elseif (spring_optimize_for_native)
    set(FLAGS "${FLAGS} -march=native")
endif ()
set(FLAGS "${FLAGS} -fomit-frame-pointer")
set(FLAGS "${FLAGS} -fstrict-aliasing")
set(FLAGS "${FLAGS} -ffast-math")

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

#######################################################
# Build Cloudflare zlib during configuration so the bundled Boost build can
# discover and link against the same zlib implementation.

set(CLOUDFLARE_ZLIB_DIR ${CMAKE_BINARY_DIR}/cloudflare_zlib-src)

message(STATUS "Building zlib")
message(STATUS "CMAKE_SOURCE_DIR: ${CMAKE_SOURCE_DIR}")
file(REMOVE_RECURSE ${CLOUDFLARE_ZLIB_DIR})
file(MAKE_DIRECTORY ${CLOUDFLARE_ZLIB_DIR})
execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar xJf ${CMAKE_SOURCE_DIR}/vendor/cloudflare_zlib.tar.xz
    WORKING_DIRECTORY ${CLOUDFLARE_ZLIB_DIR}
)
execute_process(
    COMMAND ./configure
    WORKING_DIRECTORY ${CLOUDFLARE_ZLIB_DIR}
)
execute_process(
    COMMAND make clean
    WORKING_DIRECTORY ${CLOUDFLARE_ZLIB_DIR}
)
execute_process(
    COMMAND make libz.a
    WORKING_DIRECTORY ${CLOUDFLARE_ZLIB_DIR}
)

set( ENV{ZLIB_ROOT} ${CLOUDFLARE_ZLIB_DIR} )
set(ZLIB_ROOT ${CLOUDFLARE_ZLIB_DIR} CACHE PATH "Path to the vendored zlib build" FORCE)
set(ZLIB_INCLUDE_DIR ${CLOUDFLARE_ZLIB_DIR} CACHE PATH "Path to the vendored zlib headers" FORCE)
set(ZLIB_LIBRARY ${CLOUDFLARE_ZLIB_DIR}/libz.a CACHE FILEPATH "Path to the vendored zlib library" FORCE)
set(ZLIB_LIBRARY_RELEASE ${CLOUDFLARE_ZLIB_DIR}/libz.a CACHE FILEPATH "Path to the vendored zlib release library" FORCE)

#######################################################

# Fetch only the Boost components Spring links directly.
set(BUILD_TESTING OFF CACHE BOOL "Disable third-party tests" FORCE)
set(BOOST_INCLUDE_LIBRARIES filesystem iostreams program_options system)

FetchContent_Declare(
    boost_local
    URL ${CMAKE_SOURCE_DIR}/vendor/boost.tar.xz
    URL_HASH SHA256=4f5880384f4aac2a0d5400043038fb4cea07f8495ea49575e300b357e5dd1b9f
)
FetchContent_MakeAvailable(boost_local)

set(source_dir ${CMAKE_SOURCE_DIR}/src)
set(vendor_dir ${CMAKE_SOURCE_DIR}/vendor)
set(build_vendor_dir ${CMAKE_BINARY_DIR}/vendor)
set(BBHASH_DIR ${build_vendor_dir}/BBHash)
set(ID_COMPRESSION_DIR ${build_vendor_dir}/id_compression)
set(LIBBSC_DIR ${build_vendor_dir}/libbsc)
set(QVZ_DIR ${build_vendor_dir}/qvz)
set(include_dir ${CMAKE_SOURCE_DIR}/src)

file(REMOVE_RECURSE ${BBHASH_DIR})
file(MAKE_DIRECTORY ${BBHASH_DIR})
execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar xJf ${CMAKE_SOURCE_DIR}/vendor/BBHash.tar.xz
    WORKING_DIRECTORY ${BBHASH_DIR}
)

file(REMOVE_RECURSE ${ID_COMPRESSION_DIR})
file(MAKE_DIRECTORY ${ID_COMPRESSION_DIR})
execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar xJf ${CMAKE_SOURCE_DIR}/vendor/id_compression.tar.xz
    WORKING_DIRECTORY ${ID_COMPRESSION_DIR}
)

file(REMOVE_RECURSE ${LIBBSC_DIR})
file(MAKE_DIRECTORY ${LIBBSC_DIR})
execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar xJf ${CMAKE_SOURCE_DIR}/vendor/libbsc.tar.xz
    WORKING_DIRECTORY ${LIBBSC_DIR}
)

file(REMOVE_RECURSE ${QVZ_DIR})
file(MAKE_DIRECTORY ${QVZ_DIR})
execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar xJf ${CMAKE_SOURCE_DIR}/vendor/qvz.tar.xz
    WORKING_DIRECTORY ${QVZ_DIR}
)

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
# as BBHash, libbsc, id_compression, qvz, and Cloudflare zlib.
target_include_directories(spring PRIVATE ${include_dir} ${vendor_dir} ${build_vendor_dir})
target_link_libraries(spring PUBLIC
    Boost::filesystem
    Boost::system
    Boost::program_options
    Boost::iostreams
)

add_dev_script_target(dev-lint lint.sh)
add_dev_script_target(dev-cppcheck cppcheck.sh)
add_dev_script_target(dev-cbindgen-check cbindgen-check.sh)
add_dev_script_target(dev-valgrind-smoke valgrind-smoke.sh)

add_dependencies(dev-lint spring)
add_dependencies(dev-valgrind-smoke spring)
