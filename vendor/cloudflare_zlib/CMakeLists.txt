cmake_minimum_required(VERSION 3.15)
project(spring_cloudflare_zlib C)
add_library(spring_cloudflare_zlib STATIC
    adler32.c
    compress.c
    crc32.c
    deflate.c
    gzclose.c
    gzlib.c
    gzread.c
    gzwrite.c
    inffast.c
    inflate.c
    inftrees.c
    trees.c
    uncompr.c
    zutil.c
)
target_include_directories(spring_cloudflare_zlib PUBLIC .)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(spring_cloudflare_zlib PRIVATE -fno-lto -fno-strict-aliasing)
endif()
if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "Clang"
    AND (MSVC OR CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"
         OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC"))
    target_compile_options(spring_cloudflare_zlib PRIVATE -Wno-unused-variable)
endif()
set_target_properties(spring_cloudflare_zlib PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(ZLIB::ZLIB ALIAS spring_cloudflare_zlib)
