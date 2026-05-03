cmake_minimum_required(VERSION 3.15)
project(qvz CXX)
add_library(qvz STATIC
    cluster.cpp
    codebook.cpp
    distortion.cpp
    lines.cpp
    pmf.cpp
    quantizer.cpp
    qv_compressor.cpp
    qvz.cpp
    util.cpp
    well.cpp
)
target_include_directories(qvz PUBLIC . ..)
set_target_properties(qvz PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(codebook.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-result")
endif()
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
    AND (MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"
         OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
    target_compile_options(qvz PRIVATE -Wno-unused-but-set-variable)
endif()
