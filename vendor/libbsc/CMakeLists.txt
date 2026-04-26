cmake_minimum_required(VERSION 3.15)
project(libbsc C CXX)

add_library(libbsc STATIC
    bsc.cpp
    bsc_str_array.cpp
    adler32/adler32.cpp
    bwt/libsais/libsais.c
    bwt/bwt.cpp
    coder/coder.cpp
    coder/qlfc/qlfc.cpp
    coder/qlfc/qlfc_model.cpp
    filters/detectors.cpp
    filters/preprocessing.cpp
    libbsc/libbsc.cpp
    lzp/lzp.cpp
    platform/platform.cpp
    st/st.cpp
)

target_include_directories(libbsc PUBLIC . .. adler32 bwt coder filters libbsc lzp platform st bwt/libsais coder/qlfc ${CMAKE_SOURCE_DIR}/src)
set_target_properties(libbsc PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(TARGET OpenMP::OpenMP_CXX)
    target_link_libraries(libbsc PUBLIC OpenMP::OpenMP_CXX)
endif()
