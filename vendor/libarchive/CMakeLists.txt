add_definitions(-DHAVE_CONFIG_H)
if(ANDROID)
    include_directories(${PROJECT_SOURCE_DIR}/contrib/android/include)
endif()
set(include_HEADERS
    archive.h
    archive_entry.h
)
set(libarchive_SOURCES
    archive_acl.c
    archive_acl_private.h
    archive_check_magic.c
    archive_crc32.h
    archive_digest.c
    archive_digest_private.h
    archive_endian.h
    archive_entry.c
    archive_entry.h
    archive_entry_copy_stat.c
    archive_entry_link_resolver.c
    archive_entry_locale.h
    archive_entry_private.h
    archive_entry_sparse.c
    archive_entry_stat.c
    archive_entry_strmode.c
    archive_entry_xattr.c
    archive_match.c
    archive_openssl_evp_private.h
    archive_openssl_hmac_private.h
    archive_options.c
    archive_options_private.h
    archive_parse_date.c
    archive_pathmatch.c
    archive_pathmatch.h
    archive_platform.h
    archive_platform_acl.h
    archive_platform_stat.h
    archive_platform_xattr.h
    archive_private.h
    archive_rb.c
    archive_rb.h
    archive_read.c
    archive_read_append_filter.c
    archive_read_open_filename.c
    archive_read_open_memory.c
    archive_read_private.h
    archive_read_set_format.c
    archive_read_set_options.c
    archive_read_support_filter_gzip.c
    archive_read_support_filter_none.c
    archive_read_support_filter_xz.c
    archive_read_support_filter_zstd.c
    archive_read_support_format_empty.c
    archive_read_support_format_tar.c
    archive_write_disk_posix.c
    archive_write_disk_private.h
    archive_write_disk_set_standard_lookup.c
    archive_string.c
    archive_string.h
    archive_string_composition.h
    archive_string_sprintf.c
    archive_time.c
    archive_time_private.h
    archive_random.c
    archive_random_private.h
    archive_util.c
    archive_version_details.c
    archive_virtual.c
    archive_write.c
    archive_write_private.h
    archive_write_open_fd.c
    archive_write_open_filename.c
    archive_write_open_memory.c
    archive_write_add_filter.c
    archive_write_add_filter_gzip.c
    archive_write_add_filter_none.c
    archive_write_add_filter_xz.c
    archive_write_add_filter_zstd.c
    archive_write_set_format.c
    archive_write_set_format_pax.c
    archive_write_set_format_private.h
    archive_write_set_format_ustar.c
    archive_write_set_options.c
    archive_xxhash.h
    xxhash.c
)
if(WIN32 AND NOT CYGWIN)
    list(APPEND libarchive_SOURCES archive_entry_copy_bhfi.c)
    list(APPEND libarchive_SOURCES archive_windows.c)
    list(APPEND libarchive_SOURCES archive_windows.h)
    list(APPEND libarchive_SOURCES archive_write_disk_windows.c)
endif(WIN32 AND NOT CYGWIN)
if(ARCHIVE_BLAKE2)
    list(APPEND libarchive_SOURCES archive_blake2sp_ref.c)
    list(APPEND libarchive_SOURCES archive_blake2s_ref.c)
endif(ARCHIVE_BLAKE2)
if(ARCHIVE_ACL_DARWIN)
    list(APPEND libarchive_SOURCES archive_disk_acl_darwin.c)
elseif(ARCHIVE_ACL_FREEBSD)
    list(APPEND libarchive_SOURCES archive_disk_acl_freebsd.c)
elseif(ARCHIVE_ACL_LIBACL)
    list(APPEND libarchive_SOURCES archive_disk_acl_linux.c)
elseif(ARCHIVE_ACL_SUNOS)
    list(APPEND libarchive_SOURCES archive_disk_acl_sunos.c)
endif()
add_library(archive_static STATIC ${libarchive_SOURCES} ${include_HEADERS})
target_include_directories(archive_static PUBLIC .)
target_link_libraries(archive_static ${ADDITIONAL_LIBS})
set_target_properties(archive_static PROPERTIES COMPILE_DEFINITIONS
    LIBARCHIVE_STATIC)
if(WIN32 AND MSVC AND MSVC_USE_STATIC_CRT)
    set_property(TARGET archive_static PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif(WIN32 AND MSVC AND MSVC_USE_STATIC_CRT)
set_target_properties(archive_static PROPERTIES OUTPUT_NAME archive)
if(CMAKE_C_COMPILER_ID MATCHES "GNU")
    if(CMAKE_C_COMPILER_ID MATCHES "GNU")
        target_compile_options(archive_static PRIVATE -Wno-discarded-qualifiers -Wno-stringop-overflow)
    elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang")
        target_compile_options(archive_static PRIVATE -Wno-ignored-qualifiers)
    endif()
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang")
    target_compile_options(archive_static PRIVATE -Wno-ignored-qualifiers)
endif()
