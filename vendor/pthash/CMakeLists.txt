cmake_minimum_required(VERSION 3.15)
project(pthash LANGUAGES CXX)

add_library(pthash INTERFACE)

target_include_directories(pthash INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
