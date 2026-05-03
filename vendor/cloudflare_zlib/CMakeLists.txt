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
target_compile_options(spring_cloudflare_zlib PRIVATE -fno-lto -fno-strict-aliasing)
set_target_properties(spring_cloudflare_zlib PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(ZLIB::ZLIB ALIAS spring_cloudflare_zlib)
