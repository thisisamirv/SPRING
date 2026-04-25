cmake_minimum_required(VERSION 3.15)
project(qvz CXX)

add_library(qvz STATIC
    src/cluster.cpp
    src/codebook.cpp
    src/distortion.cpp
    src/lines.cpp
    src/pmf.cpp
    src/quantizer.cpp
    src/qv_compressor.cpp
    src/qvz.cpp
    src/util.cpp
    src/well.cpp
)

target_include_directories(qvz PUBLIC . .. src)
set_target_properties(qvz PROPERTIES POSITION_INDEPENDENT_CODE ON)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(src/codebook.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-result")
endif()
