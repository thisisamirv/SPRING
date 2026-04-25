cmake_minimum_required(VERSION 3.15)
project(pthash LANGUAGES CXX)

add_library(pthash INTERFACE)

target_include_directories(pthash INTERFACE
    .
    ..
    include
    external/xxHash
    external/bits/include
    external/bits/external/essentials/include
    external/mm_file/include
    ${CMAKE_SOURCE_DIR}/src
)
