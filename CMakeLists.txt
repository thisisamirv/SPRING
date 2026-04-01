cmake_minimum_required (VERSION 4.2)
project (SPRING)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CheckIPOSupported)
include(FetchContent)

if (NOT UNIX)
	message(FATAL_ERROR "Non Unix platforms not supported currently.")
endif()

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

option(spring_optimize_for_native "Build spoa with -march=native" ON)
option(spring_optimize_for_portability "Build spoa with -msse4.1" OFF)

message(STATUS "Setting compiler flags")
set(FLAGS "${FLAGS} -g")
##set(FLAGS "${FLAGS} -Wextra")
#set(FLAGS "${FLAGS} -Wpedantic")
#set(FLAGS "${FLAGS} -Wall")
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

FIND_PACKAGE( OpenMP REQUIRED)
message(STATUS "OPENMP FOUND")
set(OpenMP_DEFINITIONS "-DLIBBSC_OPENMP_SUPPORT -DLIBSAIS_OPENMP")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS} ${OpenMP_DEFINITIONS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS} ${OpenMP_DEFINITIONS}")

execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=include
                OUTPUT_VARIABLE COMPILER_INCLUDE_DIR
                OUTPUT_STRIP_TRAILING_WHITESPACE)
file(WRITE "${CMAKE_SOURCE_DIR}/.clangd" "CompileFlags:\n  Add: [\"-I${COMPILER_INCLUDE_DIR}\", \"-fopenmp\"]\n")

message(STATUS "** CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
message(STATUS "** CMAKE_C_FLAGS: ${CMAKE_C_FLAGS}")

#######################################################
# build cloudflare zlib and set environment variable ZLIB_ROOT
# the build runs during configuration time so boost can use the
# correct zlib

message(STATUS "Building zlib")
message(STATUS "CMAKE_SOURCE_DIR: ${CMAKE_SOURCE_DIR}")
execute_process(
    COMMAND ./configure
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/cloudflare_zlib/
)
execute_process(
    COMMAND make clean
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/cloudflare_zlib/
)
execute_process(
    COMMAND make static
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/cloudflare_zlib/
)

set( ENV{ZLIB_ROOT} ${CMAKE_SOURCE_DIR}/third_party/cloudflare_zlib/ )

#######################################################

set(BUILD_TESTING OFF CACHE BOOL "Disable third-party tests" FORCE)
set(BOOST_INCLUDE_LIBRARIES filesystem iostreams program_options system)

FetchContent_Declare(
    boost_local
    URL ${CMAKE_SOURCE_DIR}/third_party/boost/boost-1.90.0-cmake.tar.xz
    URL_HASH SHA256=aca59f889f0f32028ad88ba6764582b63c916ce5f77b31289ad19421a96c555f
)
FetchContent_MakeAvailable(boost_local)

set(source_dir ${CMAKE_SOURCE_DIR}/src)
set(third_party_dir ${CMAKE_SOURCE_DIR}/third_party)
set(include_dir ${CMAKE_SOURCE_DIR}/src)

list(APPEND source_files
    # spring
    ${source_dir}/main.cpp
    ${source_dir}/spring.cpp
    ${source_dir}/util.cpp
    ${source_dir}/bitset_util.cpp
    ${source_dir}/preprocess.cpp
    ${source_dir}/encoder.cpp
    ${source_dir}/reorder_compress_streams.cpp
    ${source_dir}/pe_encode.cpp
    ${source_dir}/reorder_compress_quality_id.cpp
    ${source_dir}/decompress.cpp
    ${source_dir}/call_template_functions.cpp

    # id compression
    ${third_party_dir}/id_compression/src/Arithmetic_stream.cpp
    ${third_party_dir}/id_compression/src/compression.cpp
    ${third_party_dir}/id_compression/src/id_compression.cpp
    ${third_party_dir}/id_compression/src/io_functions.cpp
    ${third_party_dir}/id_compression/src/sam_file_allocation.cpp
    ${third_party_dir}/id_compression/src/sam_models.cpp
    ${third_party_dir}/id_compression/src/stream_model.cpp

    # bsc
    ${third_party_dir}/libbsc/bsc.cpp
    ${third_party_dir}/libbsc/bsc_str_array.cpp
    ${third_party_dir}/libbsc/libbsc/adler32/adler32.cpp
    ${third_party_dir}/libbsc/libbsc/bwt/libsais/libsais.c
    ${third_party_dir}/libbsc/libbsc/bwt/bwt.cpp
    ${third_party_dir}/libbsc/libbsc/coder/coder.cpp
    ${third_party_dir}/libbsc/libbsc/coder/qlfc/qlfc.cpp
    ${third_party_dir}/libbsc/libbsc/coder/qlfc/qlfc_model.cpp
    ${third_party_dir}/libbsc/libbsc/filters/detectors.cpp
    ${third_party_dir}/libbsc/libbsc/filters/preprocessing.cpp
    ${third_party_dir}/libbsc/libbsc/libbsc/libbsc.cpp
    ${third_party_dir}/libbsc/libbsc/lzp/lzp.cpp
    ${third_party_dir}/libbsc/libbsc/platform/platform.cpp
    ${third_party_dir}/libbsc/libbsc/st/st.cpp

    # qvz
    ${third_party_dir}/qvz/src/cluster.cpp
    ${third_party_dir}/qvz/src/codebook.cpp
    ${third_party_dir}/qvz/src/distortion.cpp
    ${third_party_dir}/qvz/src/lines.cpp
    ${third_party_dir}/qvz/src/pmf.cpp
    ${third_party_dir}/qvz/src/quantizer.cpp
    ${third_party_dir}/qvz/src/qv_compressor.cpp
    ${third_party_dir}/qvz/src/qvz.cpp
    ${third_party_dir}/qvz/src/util.cpp
    ${third_party_dir}/qvz/src/well.cpp
)

function(add_dev_script_target target_name script_name)
    add_custom_target(${target_name}
            COMMAND ${CMAKE_SOURCE_DIR}/dev/${script_name}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            USES_TERMINAL
    )
endfunction()

add_executable (spring ${source_files})

target_include_directories(spring PRIVATE ${include_dir} ${third_party_dir})
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
