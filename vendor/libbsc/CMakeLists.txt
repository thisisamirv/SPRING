cmake_minimum_required(VERSION 3.15)
project(libbsc C CXX)
add_library(libbsc STATIC
    adler32.cpp
    bsc.cpp
    bsc_str_array.cpp
    bwt.cpp
    coder.cpp
    detectors.cpp
    libbsc.cpp
    libsais.c
    lzp.cpp
    platform.cpp
    preprocessing.cpp
    qlfc.cpp
    qlfc_model.cpp
    st.cpp
)
target_include_directories(libbsc PUBLIC . .. ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/common)
set_target_properties(libbsc PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(TARGET OpenMP::OpenMP_CXX)
    target_link_libraries(libbsc PUBLIC OpenMP::OpenMP_CXX)
endif()
if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "Clang"
    AND (MSVC OR CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"
         OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC"))
    target_compile_options(libbsc PRIVATE -Wno-unused-variable)
endif()

# Workaround for MSVC ARM64 internal compiler error in libsais.c
# Use /Od to avoid compiler bug (OpenMP is disabled globally for MSVC ARM64)
if(MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64)$")
    set_source_files_properties(libsais.c PROPERTIES COMPILE_OPTIONS "/Od")
endif()
