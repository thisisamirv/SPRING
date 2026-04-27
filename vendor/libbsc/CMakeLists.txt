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

target_include_directories(libbsc PUBLIC . .. ${CMAKE_SOURCE_DIR}/src)
set_target_properties(libbsc PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(TARGET OpenMP::OpenMP_CXX)
    target_link_libraries(libbsc PUBLIC OpenMP::OpenMP_CXX)
endif()
