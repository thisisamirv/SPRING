# Top-level CMake build for the Spring executable and its bundled third-party
# dependencies.

cmake_minimum_required (VERSION 4.2)

set(CMAKE_MESSAGE_LOG_LEVEL NOTICE)

project (SPRING2)

option(SPRING_ENABLE_COMPILER_CACHE "Use sccache/ccache as compiler launcher when available" ON)
if (SPRING_ENABLE_COMPILER_CACHE)
    if (WIN32)
        find_program(SPRING_COMPILER_CACHE_PROGRAM NAMES sccache)
    else ()
        find_program(SPRING_COMPILER_CACHE_PROGRAM NAMES ccache sccache)
    endif ()

    if (SPRING_COMPILER_CACHE_PROGRAM)
        message(NOTICE "Using compiler cache launcher: ${SPRING_COMPILER_CACHE_PROGRAM}")
        set(CMAKE_C_COMPILER_LAUNCHER "${SPRING_COMPILER_CACHE_PROGRAM}" CACHE STRING "C compiler launcher" FORCE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${SPRING_COMPILER_CACHE_PROGRAM}" CACHE STRING "CXX compiler launcher" FORCE)
    else ()
        message(NOTICE "Compiler cache requested, but no sccache/ccache executable was found on PATH.")
    endif ()
endif ()

# The project is validated against GCC and Clang toolchains.

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(NOTICE "Setting build type to 'Release' as none was specified.")
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

option(LIBRAPIDARCHIVE_WITH_ISAL "Build rapidgzip with Intel ISA-L support" ON)
if (APPLE)
    # ISA-L support is generally omitted on macOS to ensure build success across architectures.
    set(LIBRAPIDARCHIVE_WITH_ISAL OFF CACHE BOOL "" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(CheckLinkerFlag)
include(CheckIPOSupported)

function(extract_archive_if_needed archive_path destination_parent expected_path)
    file(SHA256 "${archive_path}" archive_hash)
    get_filename_component(archive_name "${archive_path}" NAME)
    set(stamp_path "${build_vendor_dir}/.${archive_name}.sha256")

    set(should_extract TRUE)
    if (EXISTS "${expected_path}" AND EXISTS "${stamp_path}")
        file(READ "${stamp_path}" recorded_hash)
        string(STRIP "${recorded_hash}" recorded_hash)
        if (recorded_hash STREQUAL archive_hash)
            set(should_extract FALSE)
        endif ()
    endif ()

    if (should_extract)
        file(MAKE_DIRECTORY "${destination_parent}")
        message(NOTICE "Extracting ${archive_name}...")
        # On Windows/MSYS2, GNU tar treats "C:" as a remote host. --force-local fixes this.
        execute_process(COMMAND tar --version OUTPUT_VARIABLE tar_version ERROR_QUIET)
        if (tar_version MATCHES "GNU tar")
            set(force_local "--force-local")
        endif()

        execute_process(
            COMMAND tar ${force_local} -xf "${archive_path}" -C "${destination_parent}" ${ARGN}
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            RESULT_VARIABLE tar_result
        )
        if (NOT tar_result EQUAL 0)
            message(FATAL_ERROR "Failed to extract ${archive_path}")
        endif()
        file(WRITE "${stamp_path}" "${archive_hash}")
    endif()
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
	message(NOTICE "Detected compiler: GNU Compiler Collection")
elseif ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
	message(NOTICE "Detected compiler: LLVM Clang or Apple Clang")
else ()
	message(FATAL_ERROR "Compiler currently not supported by CMake build: ${CMAKE_CXX_COMPILER_ID}")
endif()

option(SPRING_ENABLE_FAST_LINKER "Use a faster linker when supported (mold/lld)" ON)
if (SPRING_ENABLE_FAST_LINKER)
    set(SPRING_FAST_LINKER_FLAG "")

    if (NOT APPLE AND NOT WIN32)
        check_linker_flag(CXX "-fuse-ld=mold" SPRING_LINKER_SUPPORTS_MOLD)
        if (SPRING_LINKER_SUPPORTS_MOLD)
            set(SPRING_FAST_LINKER_FLAG "-fuse-ld=mold")
        endif ()
    endif ()

    if (NOT SPRING_FAST_LINKER_FLAG)
        check_linker_flag(CXX "-fuse-ld=lld" SPRING_LINKER_SUPPORTS_LLD)
        if (SPRING_LINKER_SUPPORTS_LLD)
            set(SPRING_FAST_LINKER_FLAG "-fuse-ld=lld")
        endif ()
    endif ()

    if (SPRING_FAST_LINKER_FLAG)
        message(NOTICE "Using faster linker flag: ${SPRING_FAST_LINKER_FLAG}")
        add_link_options("${SPRING_FAST_LINKER_FLAG}")
    else ()
        message(NOTICE "Fast linker requested, but no supported linker flag was detected.")
    endif ()
endif ()

option(SPRING_ENABLE_IPO "Enable Interprocedural Optimization if supported" ON)
if (NOT WIN32 AND SPRING_ENABLE_IPO AND NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION)
        check_ipo_supported(RESULT result)
        if(result)
            message(NOTICE "Support for Interprocedural Optimization detected.")
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()
endif ()

# Native tuning is the default; portability mode restricts x86-family code
# generation to SSE4.1-compatible instructions.
option(spring_optimize_for_native "Build with -march=native" OFF)
option(spring_optimize_for_portability "Build with -msse4.1 (Maximum portability)" ON)

set(SPRING_X86_FAMILY FALSE)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
    set(SPRING_X86_FAMILY TRUE)
endif ()

message(NOTICE "Setting compiler flags")
set(FLAGS "${FLAGS} -O3")
if (spring_optimize_for_portability)
    if (SPRING_X86_FAMILY)
        set(FLAGS "${FLAGS} -msse4.1")
    else ()
        message(NOTICE "spring_optimize_for_portability requested on non-x86 target ${CMAKE_SYSTEM_PROCESSOR}; skipping -msse4.1")
    endif ()
elseif (spring_optimize_for_native)
    set(FLAGS "${FLAGS} -march=native")
endif ()
set(FLAGS "${FLAGS} -fomit-frame-pointer")

if (CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo)$")
    set(FLAGS "${FLAGS} -g")
endif ()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${FLAGS}")

# OpenMP is required across Spring and several bundled compression components.
if (APPLE)
    # Automatically locate Homebrew libomp if not already specified
    if (NOT OpenMP_FOUND AND NOT DEFINED OpenMP_ROOT)
        set(BREW_LIBOMP_DIR "")
        if (EXISTS "/opt/homebrew/opt/libomp")
            set(BREW_LIBOMP_DIR "/opt/homebrew/opt/libomp")
        elseif (EXISTS "/usr/local/opt/libomp")
            set(BREW_LIBOMP_DIR "/usr/local/opt/libomp")
        endif()

        if (BREW_LIBOMP_DIR)
            message(NOTICE "Found Homebrew libomp at ${BREW_LIBOMP_DIR}, configuring OpenMP automatically")
            set(OpenMP_ROOT "${BREW_LIBOMP_DIR}" CACHE PATH "Path to OpenMP root")
            set(OpenMP_C_FLAGS "-Xpreprocessor -fopenmp" CACHE STRING "C flags for OpenMP")
            set(OpenMP_CXX_FLAGS "-Xpreprocessor -fopenmp" CACHE STRING "CXX flags for OpenMP")
            set(OpenMP_C_LIB_NAMES "omp" CACHE STRING "C library names for OpenMP")
            set(OpenMP_CXX_LIB_NAMES "omp" CACHE STRING "CXX library names for OpenMP")
            set(OpenMP_omp_LIBRARY "${BREW_LIBOMP_DIR}/lib/libomp.dylib" CACHE FILEPATH "Path to omp library")
        endif()
    endif()
endif()

FIND_PACKAGE( OpenMP REQUIRED)
message(NOTICE "OPENMP FOUND")
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
string(APPEND SPRING_CLANGD_FLAGS ", \"-I${build_vendor_dir}/pthash/include\"")
string(APPEND SPRING_CLANGD_FLAGS ", \"-I${build_vendor_dir}/pthash/external/bits/include\"")
string(APPEND SPRING_CLANGD_FLAGS ", \"-I${build_vendor_dir}/pthash/external/mm_file/include\"")
string(APPEND SPRING_CLANGD_FLAGS ", \"-I${build_vendor_dir}/pthash/external/xxHash\"")
string(APPEND SPRING_CLANGD_FLAGS ", \"-I${build_vendor_dir}/qvz/include\"")
string(APPEND SPRING_CLANGD_FLAGS ", \"-I${build_vendor_dir}/libbsc\"")
string(APPEND SPRING_CLANGD_FLAGS ", \"-I${build_vendor_dir}/libdeflate\"")
foreach(openmp_include_dir IN LISTS SPRING_OPENMP_INCLUDE_DIRS)
    string(APPEND SPRING_CLANGD_FLAGS ", \"-I${openmp_include_dir}\"")
endforeach ()
set(CLANGD_CONTENT "CompileFlags:\n  Add: [${SPRING_CLANGD_FLAGS}]\n")
if (EXISTS "${CMAKE_SOURCE_DIR}/.clangd")
    file(READ "${CMAKE_SOURCE_DIR}/.clangd" EXISTING_CLANGD_CONTENT)
endif ()
if (NOT CLANGD_CONTENT STREQUAL EXISTING_CLANGD_CONTENT)
    file(WRITE "${CMAKE_SOURCE_DIR}/.clangd" "${CLANGD_CONTENT}")
endif ()
# Keep compile_commands.json in the source tree as a regular file copy.
# This avoids symlink permission/portability issues across platforms.
add_custom_target(copy_compile_commands ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/compile_commands.json" "${CMAKE_SOURCE_DIR}/compile_commands.json"
    COMMENT "Copying compile_commands.json into source tree"
)

# message(NOTICE "** CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
# message(NOTICE "** CMAKE_C_FLAGS: ${CMAKE_C_FLAGS}")

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
set(LIBARCHIVE_DIR ${build_vendor_dir}/libarchive)
set(include_dir ${CMAKE_SOURCE_DIR}/src)

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/cloudflare_zlib.tar.xz"
    "${build_vendor_dir}"
    "${CLOUDFLARE_ZLIB_DIR}/zlib.h"
)

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/pthash.tar.xz"
    "${build_vendor_dir}"
    "${PTHASH_DIR}/CMakeLists.txt"
)
add_subdirectory("${PTHASH_DIR}" "${CMAKE_BINARY_DIR}/pthash-build")

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/zstd.tar.xz"
    "${build_vendor_dir}"
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
    "${build_vendor_dir}"
    "${LIBBSC_DIR}/CMakeLists.txt"
)
add_subdirectory("${LIBBSC_DIR}" "${CMAKE_BINARY_DIR}/libbsc-build")
if (TARGET libbsc AND SPRING_OPENMP_INCLUDE_DIRS)
    target_include_directories(libbsc SYSTEM PRIVATE ${SPRING_OPENMP_INCLUDE_DIRS})
endif()

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/qvz.tar.xz"
    "${build_vendor_dir}"
    "${QVZ_DIR}/CMakeLists.txt"
)
add_subdirectory("${QVZ_DIR}" "${CMAKE_BINARY_DIR}/qvz-build")
if (TARGET qvz AND SPRING_OPENMP_INCLUDE_DIRS)
    target_include_directories(qvz SYSTEM PRIVATE ${SPRING_OPENMP_INCLUDE_DIRS})
endif()

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/libdeflate.tar.xz"
    "${build_vendor_dir}"
    "${LIBDEFLATE_DIR}/libdeflate.h"
)

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/indexed_bzip2.tar.xz"
    "${build_vendor_dir}"
    "${INDEXED_BZIP2_DIR}/CMakeLists.txt"
)

extract_archive_if_needed(
    "${CMAKE_SOURCE_DIR}/vendor/libarchive.tar.xz"
    "${build_vendor_dir}"
    "${LIBARCHIVE_DIR}/CMakeLists.txt"
)

set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${LIBDEFLATE_DIR}" "${CMAKE_BINARY_DIR}/libdeflate-build")

add_subdirectory("${CLOUDFLARE_ZLIB_DIR}" "${CMAKE_BINARY_DIR}/cloudflare_zlib-build")

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
    if (SPRING_OPENMP_INCLUDE_DIRS)
        target_include_directories(rapidgzip SYSTEM PRIVATE ${SPRING_OPENMP_INCLUDE_DIRS})
    endif()
endif ()

# Standard archive support via vendored libarchive.
set(ENABLE_MBEDTLS OFF CACHE BOOL "" FORCE)
set(ENABLE_NETTLE OFF CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBB2 OFF CACHE BOOL "" FORCE)
set(ENABLE_LZ4 OFF CACHE BOOL "" FORCE)
set(ENABLE_LZO OFF CACHE BOOL "" FORCE)
set(ENABLE_LZMA OFF CACHE BOOL "" FORCE)
set(ENABLE_ZSTD OFF CACHE BOOL "" FORCE)
set(ENABLE_ZLIB OFF CACHE BOOL "" FORCE)
set(ENABLE_BZip2 OFF CACHE BOOL "" FORCE)
set(ENABLE_LIBXML2 OFF CACHE BOOL "" FORCE)
set(ENABLE_EXPAT OFF CACHE BOOL "" FORCE)
set(ENABLE_PCREPOSIX OFF CACHE BOOL "" FORCE)
set(ENABLE_PCRE2POSIX OFF CACHE BOOL "" FORCE)
set(ENABLE_ICONV OFF CACHE BOOL "" FORCE)
set(ENABLE_ACL OFF CACHE BOOL "" FORCE)
set(ENABLE_XATTR OFF CACHE BOOL "" FORCE)
set(ENABLE_CNG OFF CACHE BOOL "" FORCE)
set(ENABLE_TAR OFF CACHE BOOL "" FORCE)
set(ENABLE_CPIO OFF CACHE BOOL "" FORCE)
set(ENABLE_CAT OFF CACHE BOOL "" FORCE)
set(ENABLE_UNZIP OFF CACHE BOOL "" FORCE)
set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${LIBARCHIVE_DIR}" "${CMAKE_BINARY_DIR}/libarchive-build" EXCLUDE_FROM_ALL)

# Core source files shared between spring2 and spring2-preview.
set(core_source_files
    ${source_dir}/spring.cpp
    ${source_dir}/params.cpp
    ${source_dir}/parse_utils.cpp
    ${source_dir}/dna_utils.cpp
    ${source_dir}/fs_utils.cpp
    ${source_dir}/io_utils.cpp
    ${source_dir}/bitset_dictionary.cpp
    ${source_dir}/preprocess.cpp
    ${source_dir}/encoder.cpp
    ${source_dir}/reordered_streams.cpp
    ${source_dir}/paired_end_order.cpp
    ${source_dir}/reordered_quality_id.cpp
    ${source_dir}/decompress.cpp
    ${source_dir}/template_dispatch.cpp
    ${source_dir}/spring_reader.cpp
    ${source_dir}/progress.cpp
    ${source_dir}/bgzf.cpp
)


# Convenience targets for repository development checks.
function(add_dev_script_target target_name script_name)
    add_custom_target(${target_name}
            COMMAND ${CMAKE_SOURCE_DIR}/dev/${script_name}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            USES_TERMINAL
    )
endfunction()

set(win_resources "")
if (WIN32)
    if (EXISTS "${CMAKE_SOURCE_DIR}/assets/icons/logo.ico")
        set(win_resources ${source_dir}/spring2.rc)
    else()
        message(NOTICE "logo.ico not found in assets/icons, skipping Windows icon resource")
    endif()
endif()

add_executable (spring2 ${source_dir}/main.cpp ${core_source_files} ${win_resources})
add_executable (spring2-preview ${source_dir}/preview_main.cpp ${core_source_files} ${win_resources})

# Spring headers live in src/, while vendor/ exposes bundled libraries such
# as BBHash, libbsc, id_compression, qvz, and the vendored libdeflate sources.
target_include_directories(spring2 PRIVATE ${include_dir})
target_include_directories(spring2-preview PRIVATE ${include_dir})

if (WIN32)
    target_include_directories(spring2 PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(spring2-preview PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif ()

if (SPRING_OPENMP_INCLUDE_DIRS)
    target_include_directories(spring2 SYSTEM PRIVATE ${SPRING_OPENMP_INCLUDE_DIRS})
    target_include_directories(spring2-preview SYSTEM PRIVATE ${SPRING_OPENMP_INCLUDE_DIRS})
endif ()
 
target_include_directories(spring2 SYSTEM PRIVATE ${LIBARCHIVE_DIR}/libarchive)
target_include_directories(spring2-preview SYSTEM PRIVATE ${LIBARCHIVE_DIR}/libarchive)

set(spring_libs
    libdeflate::libdeflate_static
    libzstd_static
    ZLIB::ZLIB
    libbsc
    qvz
    pthash
    archive_static
)

target_link_libraries(spring2 PUBLIC ${spring_libs})
target_link_libraries(spring2-preview PUBLIC ${spring_libs})
if (TARGET OpenMP::OpenMP_CXX)
    target_link_libraries(spring2 PUBLIC OpenMP::OpenMP_CXX)
endif ()

if (WIN32)
    target_link_libraries(spring2 PRIVATE ws2_32 psapi)
    target_link_libraries(spring2-preview PRIVATE ws2_32 psapi)
endif ()

target_compile_definitions(spring2 PRIVATE SPRING_RAPIDGZIP_EXECUTABLE=\"$<TARGET_FILE:rapidgzip>\")
target_compile_definitions(spring2-preview PRIVATE SPRING_RAPIDGZIP_EXECUTABLE=\"$<TARGET_FILE:rapidgzip>\")

if (WIN32)
    target_compile_definitions(spring2 PRIVATE IDOFILE_PATH_ROOT=\".spring-idtmp/idoFile.\")
    target_compile_definitions(spring2-preview PRIVATE IDOFILE_PATH_ROOT=\".spring-idtmp/idoFile.\")
endif ()

add_dependencies(spring2 rapidgzip)
add_dependencies(spring2-preview rapidgzip)

# On Windows with GNU toolchain: support static linking of GCC runtimes
if (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # Base GCC runtime set. OpenMP runtime is additionally resolved below from
    # compiler/OpenMP metadata to support toolchain variations.
    set(SPRING_RUNTIME_DLLS
        libstdc++-6.dll
        libgcc_s_seh-1.dll
        libwinpthread-1.dll
    )

    function(resolve_gcc_runtime_dll dll_name out_path)
        get_filename_component(_gcc_bin_dir "${CMAKE_C_COMPILER}" DIRECTORY)
        set(_candidate "${_gcc_bin_dir}/${dll_name}")
        if (EXISTS "${_candidate}")
            set(${out_path} "${_candidate}" PARENT_SCOPE)
            return()
        endif()

        execute_process(
            COMMAND "${CMAKE_C_COMPILER}" -print-file-name=${dll_name}
            OUTPUT_VARIABLE _print_path
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if (_print_path AND NOT _print_path STREQUAL dll_name AND EXISTS "${_print_path}")
            set(${out_path} "${_print_path}" PARENT_SCOPE)
            return()
        endif()

        set(${out_path} "" PARENT_SCOPE)
    endfunction()

    function(resolve_openmp_runtime_dll out_path)
        set(_openmp_candidates
            libgomp-1.dll
            libgomp.dll
            libomp.dll
            libomp-1.dll
        )

        if (DEFINED OpenMP_CXX_LIB_NAMES)
            foreach(_omp_name IN LISTS OpenMP_CXX_LIB_NAMES)
                list(APPEND _openmp_candidates "lib${_omp_name}-1.dll")
                list(APPEND _openmp_candidates "lib${_omp_name}.dll")
            endforeach()
        endif()
        list(REMOVE_DUPLICATES _openmp_candidates)

        foreach(_candidate IN LISTS _openmp_candidates)
            resolve_gcc_runtime_dll("${_candidate}" _resolved)
            if (_resolved)
                set(${out_path} "${_resolved}" PARENT_SCOPE)
                return()
            endif()
        endforeach()

        set(${out_path} "" PARENT_SCOPE)
    endfunction()

    set(SPRING_RUNTIME_DLL_PATHS)
    foreach(_dll IN LISTS SPRING_RUNTIME_DLLS)
        resolve_gcc_runtime_dll("${_dll}" _resolved_dll_path)
        if (_resolved_dll_path)
            list(APPEND SPRING_RUNTIME_DLL_PATHS "${_resolved_dll_path}")
        else()
            message(NOTICE "SPRING: runtime DLL ${_dll} not found for bundling")
        endif()
    endforeach()

    resolve_openmp_runtime_dll(_resolved_openmp_runtime)
    if (_resolved_openmp_runtime)
        list(APPEND SPRING_RUNTIME_DLL_PATHS "${_resolved_openmp_runtime}")
    else()
        message(NOTICE "SPRING: OpenMP runtime DLL not found for bundling")
    endif()
    list(REMOVE_DUPLICATES SPRING_RUNTIME_DLL_PATHS)

    if (SPRING_STATIC_RUNTIMES)
        message(NOTICE "SPRING: linking libgcc/libstdc++/libgomp statically")
        # Prefer modern CMake link options when available
        if (COMMAND target_link_options)
            target_link_options(spring2 PRIVATE -static)
            target_link_options(spring2-preview PRIVATE -static)
            target_link_options(rapidgzip PRIVATE -static)
        else()
            set_property(TARGET spring2 APPEND_STRING PROPERTY LINK_FLAGS " -static")
            set_property(TARGET spring2-preview APPEND_STRING PROPERTY LINK_FLAGS " -static")
            set_property(TARGET rapidgzip APPEND_STRING PROPERTY LINK_FLAGS " -static")
        endif()
    else()
        # Convenience target to copy commonly required GCC runtime DLLs for
        # all distributed Windows binaries.
        add_custom_target(copy_runtime_dlls ALL
            COMMENT "Copying GCC runtime DLLs next to spring2 executable"
        )
        add_dependencies(copy_runtime_dlls spring2 spring2-preview rapidgzip)
        foreach(_src IN LISTS SPRING_RUNTIME_DLL_PATHS)
            get_filename_component(_dll "${_src}" NAME)
            if (EXISTS "${_src}")
                add_custom_command(TARGET copy_runtime_dlls POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "$<TARGET_FILE_DIR:spring2>/${_dll}"
                )
                add_custom_command(TARGET copy_runtime_dlls POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "$<TARGET_FILE_DIR:spring2-preview>/${_dll}"
                )
                add_custom_command(TARGET copy_runtime_dlls POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "$<TARGET_FILE_DIR:rapidgzip>/${_dll}"
                )
            endif()
        endforeach()
    endif()
endif()


# Installation logic for a clean release footprint
if (APPLE)
    set(SPRING_INSTALL_RPATH "@loader_path/../lib")
elseif (UNIX AND NOT WIN32)
    set(SPRING_INSTALL_RPATH "$ORIGIN/../lib")
endif()

if (DEFINED SPRING_INSTALL_RPATH)
    foreach(_target IN ITEMS spring2 spring2-preview rapidgzip)
        set_target_properties(${_target} PROPERTIES
            BUILD_WITH_INSTALL_RPATH OFF
            INSTALL_RPATH "${SPRING_INSTALL_RPATH}"
        )
    endforeach()
endif()

function(resolve_unix_runtime_library out_path)
    foreach(_candidate IN LISTS ARGN)
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=${_candidate}
            OUTPUT_VARIABLE _resolved
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if (_resolved AND NOT _resolved STREQUAL _candidate AND EXISTS "${_resolved}")
            set(${out_path} "${_resolved}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_path} "" PARENT_SCOPE)
endfunction()

set(SPRING_OPENMP_RUNTIME_LIB "")
if (APPLE)
    if (DEFINED OpenMP_omp_LIBRARY AND EXISTS "${OpenMP_omp_LIBRARY}")
        set(SPRING_OPENMP_RUNTIME_LIB "${OpenMP_omp_LIBRARY}")
    endif()
    if (NOT SPRING_OPENMP_RUNTIME_LIB AND DEFINED OpenMP_CXX_LIB_NAMES)
        foreach(_omp_name IN LISTS OpenMP_CXX_LIB_NAMES)
            resolve_unix_runtime_library(_resolved_omp_dylib
                "lib${_omp_name}.dylib"
                "${_omp_name}.dylib")
            if (_resolved_omp_dylib)
                set(SPRING_OPENMP_RUNTIME_LIB "${_resolved_omp_dylib}")
                break()
            endif()
        endforeach()
    endif()
elseif (UNIX)
    # GNU toolchains generally use libgomp; Clang toolchains may use libomp.
    resolve_unix_runtime_library(SPRING_OPENMP_RUNTIME_LIB
        "libgomp.so.1"
        "libgomp.so"
        "libomp.so.1"
        "libomp.so")
endif()

set(INSTALL_TARGETS spring2 spring2-preview rapidgzip)
install(TARGETS ${INSTALL_TARGETS}
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

if ((APPLE OR (UNIX AND NOT WIN32)) AND SPRING_OPENMP_RUNTIME_LIB)
    install(FILES "${SPRING_OPENMP_RUNTIME_LIB}" DESTINATION lib)
elseif (APPLE OR (UNIX AND NOT WIN32))
    message(NOTICE "SPRING: OpenMP runtime library not found for install bundling")
endif()

if (WIN32 AND NOT SPRING_STATIC_RUNTIMES)
    foreach(_src IN LISTS SPRING_RUNTIME_DLL_PATHS)
        if (EXISTS "${_src}")
            install(FILES "${_src}" DESTINATION bin)
        endif()
    endforeach()
endif()

add_dev_script_target(dev-lint lint.sh)
add_dev_script_target(dev-cppcheck cppcheck.sh)
add_dev_script_target(dev-cbindgen-check cbindgen-check.sh)
add_dev_script_target(dev-valgrind-smoke valgrind-smoke.sh)

add_dependencies(dev-lint spring2)
add_dependencies(dev-valgrind-smoke spring2)

# Unit testing integration using doctest.
enable_testing()
add_executable(spring2-tests tests/unit_tests.cpp ${core_source_files})
target_include_directories(spring2-tests PRIVATE ${include_dir} ${vendor_dir} tests)
target_link_libraries(spring2-tests PRIVATE ${spring_libs})
if (TARGET OpenMP::OpenMP_CXX)
    target_link_libraries(spring2-tests PRIVATE OpenMP::OpenMP_CXX)
endif ()
if (SPRING_OPENMP_INCLUDE_DIRS)
    target_include_directories(spring2-tests SYSTEM PRIVATE ${SPRING_OPENMP_INCLUDE_DIRS})
endif ()
target_compile_definitions(spring2-tests PRIVATE SPRING_RAPIDGZIP_EXECUTABLE=\"$<TARGET_FILE:rapidgzip>\")
add_test(NAME unit_tests COMMAND spring2-tests)

add_executable(integration-tests tests/integration_tests.cpp ${core_source_files})
target_include_directories(integration-tests PRIVATE ${include_dir} ${vendor_dir} tests)
target_link_libraries(integration-tests PRIVATE ${spring_libs})
if (OpenMP_CXX_FOUND)
    target_link_libraries(integration-tests PRIVATE OpenMP::OpenMP_CXX)
endif()
if (SPRING_OPENMP_INCLUDE_DIRS)
    target_include_directories(integration-tests SYSTEM PRIVATE ${SPRING_OPENMP_INCLUDE_DIRS})
endif ()
target_compile_definitions(integration-tests PRIVATE SPRING_RAPIDGZIP_EXECUTABLE=\"$<TARGET_FILE:rapidgzip>\")
target_compile_definitions(integration-tests PRIVATE SPRING2_EXECUTABLE=\"$<TARGET_FILE:spring2>\")
add_test(NAME integration_tests COMMAND integration-tests)

if (WIN32)
    target_link_libraries(spring2-tests PRIVATE ws2_32 psapi)
    target_link_libraries(integration-tests PRIVATE ws2_32 psapi)
endif ()

# Integrated Smoke Tests
if (WIN32)
    # Windows: Use PowerShell version
    find_program(POWERSHELL_PATH NAMES pwsh powershell)
    if (POWERSHELL_PATH)
        add_test(NAME smoke_tests COMMAND ${POWERSHELL_PATH} -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_test.ps1)
        set_tests_properties(smoke_tests PROPERTIES 
            ENVIRONMENT "SPRING_BIN=$<TARGET_FILE:spring2>;SPRING_PREVIEW_BIN=$<TARGET_FILE:spring2-preview>"
        )
    endif()
else()
    # Linux/macOS: Use Bash version
    add_test(NAME smoke_tests COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_test.sh)
    set_tests_properties(smoke_tests PROPERTIES 
        ENVIRONMENT "SPRING_BIN=$<TARGET_FILE:spring2>;SPRING_PREVIEW_BIN=$<TARGET_FILE:spring2-preview>"
    )
endif()
