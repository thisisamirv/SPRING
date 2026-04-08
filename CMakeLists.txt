# Top-level CMake build for the Spring executable and its bundled third-party
# dependencies.

cmake_minimum_required (VERSION 4.2)
project (SPRING2)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CheckIPOSupported)

function(extract_archive_if_needed archive_path destination_dir expected_path)
    file(SHA256 "${archive_path}" archive_hash)
    set(stamp_path "${destination_dir}/.spring-archive-sha256")
    set(should_extract TRUE)

    if (EXISTS "${expected_path}" AND EXISTS "${stamp_path}")
        file(READ "${stamp_path}" recorded_hash)
        string(STRIP "${recorded_hash}" recorded_hash)
        if (recorded_hash STREQUAL archive_hash)
            set(should_extract FALSE)
        endif ()
    endif ()

    if (should_extract)
        file(REMOVE_RECURSE "${destination_dir}")
        file(MAKE_DIRECTORY "${destination_dir}")
        file(ARCHIVE_EXTRACT INPUT "${archive_path}" DESTINATION "${destination_dir}")
        # Some archives contain a single top-level directory (e.g. "zstd/")
        # Normalize that case by moving the inner contents up one level
        if (NOT EXISTS "${expected_path}")
            file(RELATIVE_PATH rel_expected "${expected_path}" "${destination_dir}")
            file(GLOB children RELATIVE "${destination_dir}" "${destination_dir}/*")
            list(LENGTH children children_count)
            if (children_count EQUAL 1)
                list(GET children 0 only_child)
                if (IS_DIRECTORY "${destination_dir}/${only_child}")
                    if (EXISTS "${destination_dir}/${only_child}/${rel_expected}")
                        file(GLOB inner_items RELATIVE "${destination_dir}/${only_child}" "${destination_dir}/${only_child}/*")
                        foreach(inner_item IN LISTS inner_items)
                            file(RENAME "${destination_dir}/${only_child}/${inner_item}" "${destination_dir}/${inner_item}")
                        endforeach()
                        file(REMOVE_RECURSE "${destination_dir}/${only_child}")
                    endif()
                endif()
            endif()
        endif()

        file(WRITE "${stamp_path}" "${archive_hash}\n")
    endif ()
endfunction()

function(replace_in_file_if_needed file_path old_text new_text)
    if (NOT EXISTS "${file_path}")
        message(FATAL_ERROR "Cannot patch missing file: ${file_path}")
    endif ()

    file(READ "${file_path}" file_contents)
    string(FIND "${file_contents}" "${new_text}" replacement_index)
    if (NOT replacement_index EQUAL -1)
        return()
    endif ()

    string(REPLACE "${old_text}" "${new_text}" updated_contents "${file_contents}")
    if (file_contents STREQUAL updated_contents)
        message(FATAL_ERROR "Expected text not found while patching ${file_path}")
    endif ()

    file(WRITE "${file_path}" "${updated_contents}")
endfunction()

function(run_command_checked working_directory description)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${working_directory}"
        RESULT_VARIABLE command_result
    )
    if (NOT command_result EQUAL 0)
        message(FATAL_ERROR "${description} failed with exit code ${command_result}")
    endif ()
endfunction()

# The project is validated against GCC and Clang toolchains.
if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
	message(STATUS "Detected compiler: GNU Compiler Collection")
elseif ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
	message(STATUS "Detected compiler: LLVM Clang or Apple Clang")
else ()
	message(FATAL_ERROR "Compiler currently not supported by CMake build: ${CMAKE_CXX_COMPILER_ID}")
endif()

if (NOT WIN32)
        check_ipo_supported(RESULT result)
        if(result)
            message(STATUS "Support for Interprocedural Optimization detected.")
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()
endif ()

# Native tuning is the default; portability mode restricts x86-family code
# generation to SSE4.1-compatible instructions.
option(spring_optimize_for_native "Build spoa with -march=native" ON)
option(spring_optimize_for_portability "Build spoa with -msse4.1" OFF)

set(SPRING_X86_FAMILY FALSE)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
    set(SPRING_X86_FAMILY TRUE)
endif ()

message(STATUS "Setting compiler flags")
set(FLAGS "${FLAGS} -O3")
if (spring_optimize_for_portability)
    if (SPRING_X86_FAMILY)
        set(FLAGS "${FLAGS} -msse4.1")
    else ()
        message(STATUS "spring_optimize_for_portability requested on non-x86 target ${CMAKE_SYSTEM_PROCESSOR}; skipping -msse4.1")
    endif ()
elseif (spring_optimize_for_native)
    set(FLAGS "${FLAGS} -march=native")
endif ()
set(FLAGS "${FLAGS} -fomit-frame-pointer")
set(FLAGS "${FLAGS} -fstrict-aliasing")
set(FLAGS "${FLAGS} -ffast-math")

if (CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo)$")
    set(FLAGS "${FLAGS} -g")
endif ()

set(CMAKE_CXX_FLAGS "${FLAGS}")
set(CMAKE_C_FLAGS "${FLAGS}")

# OpenMP is required across Spring and several bundled compression components.
FIND_PACKAGE( OpenMP REQUIRED)
message(STATUS "OPENMP FOUND")
set(OpenMP_DEFINITIONS "-DLIBBSC_OPENMP_SUPPORT -DLIBSAIS_OPENMP")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS} ${OpenMP_DEFINITIONS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS} ${OpenMP_DEFINITIONS}")

set(SPRING_OPENMP_INCLUDE_DIRS)
if (OpenMP_C_INCLUDE_DIRS)
    list(APPEND SPRING_OPENMP_INCLUDE_DIRS ${OpenMP_C_INCLUDE_DIRS})
endif ()
if (OpenMP_CXX_INCLUDE_DIRS)
    list(APPEND SPRING_OPENMP_INCLUDE_DIRS ${OpenMP_CXX_INCLUDE_DIRS})
endif ()
if (NOT SPRING_OPENMP_INCLUDE_DIRS AND DEFINED OpenMP_ROOT)
    set(SPRING_OPENMP_ROOT_INCLUDE_DIR "${OpenMP_ROOT}/include")
    if (EXISTS "${SPRING_OPENMP_ROOT_INCLUDE_DIR}/omp.h")
        list(APPEND SPRING_OPENMP_INCLUDE_DIRS "${SPRING_OPENMP_ROOT_INCLUDE_DIR}")
    endif ()
endif ()
list(REMOVE_DUPLICATES SPRING_OPENMP_INCLUDE_DIRS)

execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=include
                OUTPUT_VARIABLE COMPILER_INCLUDE_DIR
                OUTPUT_STRIP_TRAILING_WHITESPACE)
set(source_dir ${CMAKE_SOURCE_DIR}/src)
set(vendor_dir ${CMAKE_SOURCE_DIR}/vendor)
set(build_vendor_dir ${CMAKE_BINARY_DIR}/vendor)
set(include_dir ${CMAKE_SOURCE_DIR}/src)

# Emit a matching clangd configuration so editor diagnostics see the same
# compiler include path and OpenMP flags as the CMake build.
set(SPRING_CLANGD_FLAGS "\"-fopenmp\", \"-I${include_dir}\", \"-I${vendor_dir}\", \"-I${build_vendor_dir}\", \"-I${vendor_dir}/zstd/lib\"")
foreach(openmp_include_dir IN LISTS SPRING_OPENMP_INCLUDE_DIRS)
    string(APPEND SPRING_CLANGD_FLAGS ", \"-I${openmp_include_dir}\"")
endforeach ()
file(WRITE "${CMAKE_SOURCE_DIR}/.clangd" "CompileFlags:\n  Add: [${SPRING_CLANGD_FLAGS}]\n")
if (WIN32)
    # On Windows creating symbolic links often requires elevated privileges.
    # Instead of creating a symbolic link during configure, add a build
    # target that copies compile_commands.json from the build directory
    # into the source tree after it is generated.
    add_custom_target(copy_compile_commands ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/compile_commands.json" "${CMAKE_SOURCE_DIR}/compile_commands.json"
        COMMENT "Copying compile_commands.json into source tree"
    )
else()
    file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json" "${CMAKE_SOURCE_DIR}/compile_commands.json" SYMBOLIC)
endif()

message(STATUS "** CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
message(STATUS "** CMAKE_C_FLAGS: ${CMAKE_C_FLAGS}")

# Option to statically link common GCC runtimes so the executable can run
# on Windows `cmd.exe` without shipping DLLs next to the binary.
option(SPRING_STATIC_RUNTIMES "Statically link libgcc/libstdc++/libgomp on supported toolchains" OFF)

set(PTHASH_DIR "${build_vendor_dir}/pthash")
set(CLOUDFLARE_ZLIB_DIR ${build_vendor_dir}/cloudflare_zlib)
set(ZSTD_DIR ${build_vendor_dir}/zstd)
set(INDEXED_BZIP2_DIR ${build_vendor_dir}/indexed_bzip2)
set(LIBBSC_DIR ${build_vendor_dir}/libbsc)
set(LIBDEFLATE_DIR ${build_vendor_dir}/libdeflate)
set(QVZ_DIR ${build_vendor_dir}/qvz)
set(include_dir ${CMAKE_SOURCE_DIR}/src)

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/cloudflare_zlib.tar.xz"
    "${CLOUDFLARE_ZLIB_DIR}"
    "${CLOUDFLARE_ZLIB_DIR}/zlib.h"
)

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/pthash.tar.xz"
    "${PTHASH_DIR}"
    "${PTHASH_DIR}/vendor/pthash/include/pthash.hpp"
)

# add_subdirectory("${PTHASH_DIR}" "${CMAKE_BINARY_DIR}/pthash-build" EXCLUDE_FROM_ALL)

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/zstd.tar.xz"
    "${ZSTD_DIR}"
    "${ZSTD_DIR}/lib/zstd.h"
)

# Zstd build configuration
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_DICTBUILDER OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
add_compile_definitions(ZSTD_NO_TRACE)
add_subdirectory("${ZSTD_DIR}/cmake" "${CMAKE_BINARY_DIR}/zstd-build")

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/libbsc.tar.xz"
    "${LIBBSC_DIR}"
    "${LIBBSC_DIR}/bsc.cpp"
)

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/qvz.tar.xz"
    "${QVZ_DIR}"
    "${QVZ_DIR}/src/qvz.cpp"
)

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/libdeflate.tar.xz"
    "${LIBDEFLATE_DIR}"
    "${LIBDEFLATE_DIR}/libdeflate.h"
)

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/indexed_bzip2.tar.xz"
    "${INDEXED_BZIP2_DIR}"
    "${INDEXED_BZIP2_DIR}/CMakeLists.txt"
)

set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${LIBDEFLATE_DIR}" "${CMAKE_BINARY_DIR}/libdeflate-build")

add_library(spring_cloudflare_zlib STATIC
    ${CLOUDFLARE_ZLIB_DIR}/adler32.c
    ${CLOUDFLARE_ZLIB_DIR}/compress.c
    ${CLOUDFLARE_ZLIB_DIR}/crc32.c
    ${CLOUDFLARE_ZLIB_DIR}/deflate.c
    ${CLOUDFLARE_ZLIB_DIR}/gzclose.c
    ${CLOUDFLARE_ZLIB_DIR}/gzlib.c
    ${CLOUDFLARE_ZLIB_DIR}/gzread.c
    ${CLOUDFLARE_ZLIB_DIR}/gzwrite.c
    ${CLOUDFLARE_ZLIB_DIR}/inffast.c
    ${CLOUDFLARE_ZLIB_DIR}/inflate.c
    ${CLOUDFLARE_ZLIB_DIR}/inftrees.c
    ${CLOUDFLARE_ZLIB_DIR}/trees.c
    ${CLOUDFLARE_ZLIB_DIR}/uncompr.c
    ${CLOUDFLARE_ZLIB_DIR}/zutil.c
)
target_include_directories(spring_cloudflare_zlib PUBLIC ${CLOUDFLARE_ZLIB_DIR})
add_library(ZLIB::ZLIB ALIAS spring_cloudflare_zlib)

set(LIBRAPIDARCHIVE_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_BUILD_TOOLS ON CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_RUN_CLANG_TIDY OFF CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_USE_SYSTEM_ZLIB ON CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_USE_ZLIB_NG OFF CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_WITH_RPMALLOC OFF CACHE BOOL "" FORCE)
set(LIBRAPIDARCHIVE_WITH_ISAL ON CACHE BOOL "Build with ISA-L support")
add_subdirectory("${INDEXED_BZIP2_DIR}" "${CMAKE_BINARY_DIR}/indexed_bzip2-build" EXCLUDE_FROM_ALL)
if (TARGET rapidgzip)
    target_compile_options(rapidgzip PRIVATE -fno-fast-math)
endif ()

# The Spring executable is built from the core pipeline plus bundled
# third-party compression libraries.
list(APPEND source_files
    # spring
    ${source_dir}/main.cpp
    ${source_dir}/spring.cpp
    ${source_dir}/util.cpp
    ${source_dir}/bitset_dictionary.cpp
    ${source_dir}/preprocess.cpp
    ${source_dir}/encoder.cpp
    ${source_dir}/reordered_streams.cpp
    ${source_dir}/paired_end_order.cpp
    ${source_dir}/reordered_quality_id.cpp
    ${source_dir}/decompress.cpp
    ${source_dir}/template_dispatch.cpp

    # bsc
    ${LIBBSC_DIR}/bsc.cpp
    ${LIBBSC_DIR}/bsc_str_array.cpp
    ${LIBBSC_DIR}/libbsc/adler32/adler32.cpp
    ${LIBBSC_DIR}/libbsc/bwt/libsais/libsais.c
    ${LIBBSC_DIR}/libbsc/bwt/bwt.cpp
    ${LIBBSC_DIR}/libbsc/coder/coder.cpp
    ${LIBBSC_DIR}/libbsc/coder/qlfc/qlfc.cpp
    ${LIBBSC_DIR}/libbsc/coder/qlfc/qlfc_model.cpp
    ${LIBBSC_DIR}/libbsc/filters/detectors.cpp
    ${LIBBSC_DIR}/libbsc/filters/preprocessing.cpp
    ${LIBBSC_DIR}/libbsc/libbsc/libbsc.cpp
    ${LIBBSC_DIR}/libbsc/lzp/lzp.cpp
    ${LIBBSC_DIR}/libbsc/platform/platform.cpp
    ${LIBBSC_DIR}/libbsc/st/st.cpp

    # qvz
    ${QVZ_DIR}/src/cluster.cpp
    ${QVZ_DIR}/src/codebook.cpp
    ${QVZ_DIR}/src/distortion.cpp
    ${QVZ_DIR}/src/lines.cpp
    ${QVZ_DIR}/src/pmf.cpp
    ${QVZ_DIR}/src/quantizer.cpp
    ${QVZ_DIR}/src/qv_compressor.cpp
    ${QVZ_DIR}/src/qvz.cpp
    ${QVZ_DIR}/src/util.cpp
    ${QVZ_DIR}/src/well.cpp
)

if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU|Clang")
    set(vendored_unused_result_warning_sources
        ${QVZ_DIR}/src/codebook.cpp
    )
    set_source_files_properties(
        ${vendored_unused_result_warning_sources}
        PROPERTIES COMPILE_OPTIONS "-Wno-unused-result"
    )
endif ()

# Convenience targets for repository development checks.
function(add_dev_script_target target_name script_name)
    add_custom_target(${target_name}
            COMMAND ${CMAKE_SOURCE_DIR}/dev/${script_name}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            USES_TERMINAL
    )
endfunction()

add_executable (spring2 ${source_files})

# Spring headers live in src/, while vendor/ exposes bundled libraries such
# as BBHash, libbsc, id_compression, qvz, and the vendored libdeflate sources.
target_include_directories(spring2 PRIVATE ${include_dir} ${vendor_dir} ${build_vendor_dir} ${ZSTD_DIR}/lib)
target_include_directories(spring2 SYSTEM PRIVATE
    ${PTHASH_DIR}/vendor/pthash/include
    ${PTHASH_DIR}/vendor/pthash/external/xxHash
    ${PTHASH_DIR}/vendor/pthash/external/bits/include
    ${PTHASH_DIR}/vendor/pthash/external/bits/external/essentials/include
    ${PTHASH_DIR}/vendor/pthash/external/mm_file/include
)
if (WIN32)
    target_include_directories(spring2 PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif ()
if (SPRING_OPENMP_INCLUDE_DIRS)
    target_include_directories(spring2 SYSTEM PRIVATE ${SPRING_OPENMP_INCLUDE_DIRS})
endif ()
target_link_libraries(spring2 PUBLIC
    libdeflate::libdeflate_static
    libzstd_static
    ZLIB::ZLIB
    # PTHASH (target removed as standalone Build system is gone; using headers directly)
)
if (TARGET OpenMP::OpenMP_CXX)
    target_link_libraries(spring2 PUBLIC OpenMP::OpenMP_CXX)
endif ()

if (WIN32)
    target_link_libraries(spring2 PRIVATE ws2_32 psapi)
endif ()

target_compile_definitions(spring2 PRIVATE SPRING_RAPIDGZIP_EXECUTABLE=\"$<TARGET_FILE:rapidgzip>\")
if (WIN32)
    target_compile_definitions(spring2 PRIVATE IDOFILE_PATH_ROOT=\".spring-idtmp/idoFile.\")
endif ()

add_dependencies(spring2 rapidgzip)

# On Windows with GNU toolchain: support static linking of GCC runtimes
if (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if (SPRING_STATIC_RUNTIMES)
        message(STATUS "SPRING: linking libgcc/libstdc++/libgomp statically")
        # Prefer modern CMake link options when available
        if (COMMAND target_link_options)
            target_link_options(spring2 PRIVATE -static-libgcc -static-libstdc++ -static-libgomp)
        else()
            set_property(TARGET spring2 APPEND_STRING PROPERTY LINK_FLAGS " -static-libgcc -static-libstdc++ -static-libgomp")
        endif()
    else()
        # Add a post-build step and a convenience target to copy commonly required
        # GCC runtime DLLs (if present near the compiler) into the executable dir.
        get_filename_component(SPRING_GCC_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
        set(SPRING_RUNTIME_DLLS
            libgomp-1.dll
            libstdc++-6.dll
            libgcc_s_seh-1.dll
            libwinpthread-1.dll
        )

        foreach(_dll IN LISTS SPRING_RUNTIME_DLLS)
            set(_src "${SPRING_GCC_BIN_DIR}/${_dll}")
            if (EXISTS "${_src}")
                add_custom_command(TARGET spring2 POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "$<TARGET_FILE_DIR:spring2>/${_dll}"
                    COMMENT "Copying runtime ${_dll} to target directory"
                )
            endif()
        endforeach()

        # Convenience target to explicitly copy runtime DLLs (useful for installers)
        add_custom_target(copy_runtime_dlls
            COMMENT "Copy GCC runtime DLLs next to spring2 executable"
        )
        add_dependencies(copy_runtime_dlls spring2)
        foreach(_dll IN LISTS SPRING_RUNTIME_DLLS)
            set(_src "${SPRING_GCC_BIN_DIR}/${_dll}")
            if (EXISTS "${_src}")
                add_custom_command(TARGET copy_runtime_dlls POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "$<TARGET_FILE_DIR:spring2>/${_dll}"
                )
            endif()
        endforeach()
    endif()
endif()

add_dev_script_target(dev-lint lint.sh)
add_dev_script_target(dev-cppcheck cppcheck.sh)
add_dev_script_target(dev-cbindgen-check cbindgen-check.sh)
add_dev_script_target(dev-valgrind-smoke valgrind-smoke.sh)

add_dependencies(dev-lint spring2)
add_dependencies(dev-valgrind-smoke spring2)
