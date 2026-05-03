/*-
 * Copyright (c) 2003-2010 Tim Kientzle
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef ARCHIVE_H_INCLUDED
#define ARCHIVE_H_INCLUDED

#define ARCHIVE_VERSION_NUMBER 3008006

#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>
#if ARCHIVE_VERSION_NUMBER < 4000000

#include <time.h>
#endif

#if defined(__BORLANDC__) && __BORLANDC__ >= 0x560
#include <stdint.h>
#elif !defined(__WATCOMC__) && !defined(_MSC_VER) && !defined(__INTERIX) &&    \
    !defined(__BORLANDC__) && !defined(_SCO_DS) && !defined(__osf__) &&        \
    !defined(__CLANG_INTTYPES_H)
#include <inttypes.h>
#endif

#if !defined(__LA_INT64_T_DEFINED)

#if ARCHIVE_VERSION_NUMBER < 4000000
#define __LA_INT64_T la_int64_t
#endif
#define __LA_INT64_T_DEFINED
#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__WATCOMC__)
typedef __int64 la_int64_t;
typedef unsigned __int64 la_uint64_t;
#else
#include <unistd.h>
#if defined(_SCO_DS) || defined(__osf__)
typedef long long la_int64_t;
typedef unsigned long long la_uint64_t;
#else
typedef int64_t la_int64_t;
typedef uint64_t la_uint64_t;
#endif
#endif
#endif

#if !defined(__LA_SSIZE_T_DEFINED)

#if ARCHIVE_VERSION_NUMBER < 4000000
#define __LA_SSIZE_T la_ssize_t
#endif
#define __LA_SSIZE_T_DEFINED
#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__WATCOMC__)
#if defined(_SSIZE_T_DEFINED) || defined(_SSIZE_T_)
typedef ssize_t la_ssize_t;
#elif defined(_WIN64)
typedef __int64 la_ssize_t;
#else
typedef long la_ssize_t;
#endif
#else
#include <unistd.h>
typedef ssize_t la_ssize_t;
#endif
#endif

#if ARCHIVE_VERSION_NUMBER < 4000000

#define __LA_TIME_T time_t
#else

#define __LA_TIME_T la_int64_t
#endif

#if ARCHIVE_VERSION_NUMBER < 4000000

#define __LA_DEV_T dev_t
#else

#define __LA_DEV_T la_int64_t
#endif

#if defined(__LIBARCHIVE_BUILD) && defined(__ANDROID__)
#include "android_lf.h"
#endif

#if ((defined __WIN32__) || (defined _WIN32) || defined(__CYGWIN__)) &&        \
    (!defined LIBARCHIVE_STATIC)
#ifdef __LIBARCHIVE_BUILD
#ifdef __GNUC__
#define __LA_DECL __attribute__((dllexport)) extern
#else
#define __LA_DECL __declspec(dllexport)
#endif
#else
#ifdef __GNUC__
#define __LA_DECL
#else
#define __LA_DECL __declspec(dllimport)
#endif
#endif
#elif defined __LIBARCHIVE_ENABLE_VISIBILITY
#define __LA_DECL __attribute__((visibility("default")))
#else

#define __LA_DECL
#endif

#undef __LA_PRINTF
#if defined(__GNUC__) && __GNUC__ >= 3 && !defined(__MINGW32__)
#define __LA_PRINTF(fmtarg, firstvararg)                                       \
  __attribute__((__format__(__printf__, fmtarg, firstvararg)))
#else
#define __LA_PRINTF(fmtarg, firstvararg)
#endif

#if defined(__GNUC__) &&                                                       \
    (__GNUC__ > 3 || (__GNUC__ == 3 && __GNUC_MINOR__ >= 1))
#define __LA_DEPRECATED __attribute__((deprecated))
#else
#define __LA_DEPRECATED
#endif

#ifdef __cplusplus
extern "C" {
#endif

__LA_DECL int archive_version_number(void);

#define ARCHIVE_VERSION_ONLY_STRING "3.8.6"
#define ARCHIVE_VERSION_STRING "libarchive " ARCHIVE_VERSION_ONLY_STRING
__LA_DECL const char *archive_version_string(void);

__LA_DECL const char *archive_version_details(void);

__LA_DECL const char *archive_zlib_version(void);
__LA_DECL const char *archive_liblzma_version(void);
__LA_DECL const char *archive_bzlib_version(void);
__LA_DECL const char *archive_liblz4_version(void);
__LA_DECL const char *archive_libzstd_version(void);
__LA_DECL const char *archive_liblzo2_version(void);
__LA_DECL const char *archive_libexpat_version(void);
__LA_DECL const char *archive_libbsdxml_version(void);
__LA_DECL const char *archive_libxml2_version(void);
__LA_DECL const char *archive_mbedtls_version(void);
__LA_DECL const char *archive_nettle_version(void);
__LA_DECL const char *archive_openssl_version(void);
__LA_DECL const char *archive_libmd_version(void);
__LA_DECL const char *archive_commoncrypto_version(void);
__LA_DECL const char *archive_cng_version(void);
__LA_DECL const char *archive_wincrypt_version(void);
__LA_DECL const char *archive_librichacl_version(void);
__LA_DECL const char *archive_libacl_version(void);
__LA_DECL const char *archive_libattr_version(void);
__LA_DECL const char *archive_libiconv_version(void);
__LA_DECL const char *archive_libpcre_version(void);
__LA_DECL const char *archive_libpcre2_version(void);

struct archive;
struct archive_entry;

#define ARCHIVE_EOF 1
#define ARCHIVE_OK 0
#define ARCHIVE_RETRY (-10)
#define ARCHIVE_WARN (-20)

#define ARCHIVE_FAILED (-25)

#define ARCHIVE_FATAL (-30)

typedef la_ssize_t archive_read_callback(struct archive *, void *_client_data,
                                         const void **_buffer);

typedef la_int64_t archive_skip_callback(struct archive *, void *_client_data,
                                         la_int64_t request);

typedef la_int64_t archive_seek_callback(struct archive *, void *_client_data,
                                         la_int64_t offset, int whence);

typedef la_ssize_t archive_write_callback(struct archive *, void *_client_data,
                                          const void *_buffer, size_t _length);

typedef int archive_open_callback(struct archive *, void *_client_data);

typedef int archive_close_callback(struct archive *, void *_client_data);

typedef int archive_free_callback(struct archive *, void *_client_data);

typedef int archive_switch_callback(struct archive *, void *_client_data1,
                                    void *_client_data2);

typedef const char *archive_passphrase_callback(struct archive *,
                                                void *_client_data);

#define ARCHIVE_FILTER_NONE 0
#define ARCHIVE_FILTER_GZIP 1
#define ARCHIVE_FILTER_BZIP2 2
#define ARCHIVE_FILTER_COMPRESS 3
#define ARCHIVE_FILTER_PROGRAM 4
#define ARCHIVE_FILTER_LZMA 5
#define ARCHIVE_FILTER_XZ 6
#define ARCHIVE_FILTER_UU 7
#define ARCHIVE_FILTER_RPM 8
#define ARCHIVE_FILTER_LZIP 9
#define ARCHIVE_FILTER_LRZIP 10
#define ARCHIVE_FILTER_LZOP 11
#define ARCHIVE_FILTER_GRZIP 12
#define ARCHIVE_FILTER_LZ4 13
#define ARCHIVE_FILTER_ZSTD 14

#if ARCHIVE_VERSION_NUMBER < 4000000
#define ARCHIVE_COMPRESSION_NONE ARCHIVE_FILTER_NONE
#define ARCHIVE_COMPRESSION_GZIP ARCHIVE_FILTER_GZIP
#define ARCHIVE_COMPRESSION_BZIP2 ARCHIVE_FILTER_BZIP2
#define ARCHIVE_COMPRESSION_COMPRESS ARCHIVE_FILTER_COMPRESS
#define ARCHIVE_COMPRESSION_PROGRAM ARCHIVE_FILTER_PROGRAM
#define ARCHIVE_COMPRESSION_LZMA ARCHIVE_FILTER_LZMA
#define ARCHIVE_COMPRESSION_XZ ARCHIVE_FILTER_XZ
#define ARCHIVE_COMPRESSION_UU ARCHIVE_FILTER_UU
#define ARCHIVE_COMPRESSION_RPM ARCHIVE_FILTER_RPM
#define ARCHIVE_COMPRESSION_LZIP ARCHIVE_FILTER_LZIP
#define ARCHIVE_COMPRESSION_LRZIP ARCHIVE_FILTER_LRZIP
#endif

#define ARCHIVE_FORMAT_BASE_MASK 0xff0000
#define ARCHIVE_FORMAT_CPIO 0x10000
#define ARCHIVE_FORMAT_CPIO_POSIX (ARCHIVE_FORMAT_CPIO | 1)
#define ARCHIVE_FORMAT_CPIO_BIN_LE (ARCHIVE_FORMAT_CPIO | 2)
#define ARCHIVE_FORMAT_CPIO_BIN_BE (ARCHIVE_FORMAT_CPIO | 3)
#define ARCHIVE_FORMAT_CPIO_SVR4_NOCRC (ARCHIVE_FORMAT_CPIO | 4)
#define ARCHIVE_FORMAT_CPIO_SVR4_CRC (ARCHIVE_FORMAT_CPIO | 5)
#define ARCHIVE_FORMAT_CPIO_AFIO_LARGE (ARCHIVE_FORMAT_CPIO | 6)
#define ARCHIVE_FORMAT_CPIO_PWB (ARCHIVE_FORMAT_CPIO | 7)
#define ARCHIVE_FORMAT_SHAR 0x20000
#define ARCHIVE_FORMAT_SHAR_BASE (ARCHIVE_FORMAT_SHAR | 1)
#define ARCHIVE_FORMAT_SHAR_DUMP (ARCHIVE_FORMAT_SHAR | 2)
#define ARCHIVE_FORMAT_TAR 0x30000
#define ARCHIVE_FORMAT_TAR_USTAR (ARCHIVE_FORMAT_TAR | 1)
#define ARCHIVE_FORMAT_TAR_PAX_INTERCHANGE (ARCHIVE_FORMAT_TAR | 2)
#define ARCHIVE_FORMAT_TAR_PAX_RESTRICTED (ARCHIVE_FORMAT_TAR | 3)
#define ARCHIVE_FORMAT_TAR_GNUTAR (ARCHIVE_FORMAT_TAR | 4)
#define ARCHIVE_FORMAT_ISO9660 0x40000
#define ARCHIVE_FORMAT_ISO9660_ROCKRIDGE (ARCHIVE_FORMAT_ISO9660 | 1)
#define ARCHIVE_FORMAT_ZIP 0x50000
#define ARCHIVE_FORMAT_EMPTY 0x60000
#define ARCHIVE_FORMAT_AR 0x70000
#define ARCHIVE_FORMAT_AR_GNU (ARCHIVE_FORMAT_AR | 1)
#define ARCHIVE_FORMAT_AR_BSD (ARCHIVE_FORMAT_AR | 2)
#define ARCHIVE_FORMAT_MTREE 0x80000
#define ARCHIVE_FORMAT_RAW 0x90000
#define ARCHIVE_FORMAT_XAR 0xA0000
#define ARCHIVE_FORMAT_LHA 0xB0000
#define ARCHIVE_FORMAT_CAB 0xC0000
#define ARCHIVE_FORMAT_RAR 0xD0000
#define ARCHIVE_FORMAT_7ZIP 0xE0000
#define ARCHIVE_FORMAT_WARC 0xF0000
#define ARCHIVE_FORMAT_RAR_V5 0x100000

#define ARCHIVE_READ_FORMAT_CAPS_NONE (0)
#define ARCHIVE_READ_FORMAT_CAPS_ENCRYPT_DATA (1 << 0)
#define ARCHIVE_READ_FORMAT_CAPS_ENCRYPT_METADATA (1 << 1)

#define ARCHIVE_READ_FORMAT_ENCRYPTION_UNSUPPORTED -2
#define ARCHIVE_READ_FORMAT_ENCRYPTION_DONT_KNOW -1

__LA_DECL struct archive *archive_read_new(void);

#if ARCHIVE_VERSION_NUMBER < 4000000
__LA_DECL int
archive_read_support_compression_all(struct archive *) __LA_DEPRECATED;
__LA_DECL int
archive_read_support_compression_bzip2(struct archive *) __LA_DEPRECATED;
__LA_DECL int
archive_read_support_compression_compress(struct archive *) __LA_DEPRECATED;
__LA_DECL int
archive_read_support_compression_gzip(struct archive *) __LA_DEPRECATED;
__LA_DECL int
archive_read_support_compression_lzip(struct archive *) __LA_DEPRECATED;
__LA_DECL int
archive_read_support_compression_lzma(struct archive *) __LA_DEPRECATED;
__LA_DECL int
archive_read_support_compression_none(struct archive *) __LA_DEPRECATED;
__LA_DECL int
archive_read_support_compression_program(struct archive *,
                                         const char *command) __LA_DEPRECATED;
__LA_DECL int archive_read_support_compression_program_signature(
    struct archive *, const char *, const void *, size_t) __LA_DEPRECATED;

__LA_DECL int
archive_read_support_compression_rpm(struct archive *) __LA_DEPRECATED;
__LA_DECL int
archive_read_support_compression_uu(struct archive *) __LA_DEPRECATED;
__LA_DECL int
archive_read_support_compression_xz(struct archive *) __LA_DEPRECATED;
#endif

__LA_DECL int archive_read_support_filter_all(struct archive *);
__LA_DECL int archive_read_support_filter_by_code(struct archive *, int);
__LA_DECL int archive_read_support_filter_bzip2(struct archive *);
__LA_DECL int archive_read_support_filter_compress(struct archive *);
__LA_DECL int archive_read_support_filter_gzip(struct archive *);
__LA_DECL int archive_read_support_filter_grzip(struct archive *);
__LA_DECL int archive_read_support_filter_lrzip(struct archive *);
__LA_DECL int archive_read_support_filter_lz4(struct archive *);
__LA_DECL int archive_read_support_filter_lzip(struct archive *);
__LA_DECL int archive_read_support_filter_lzma(struct archive *);
__LA_DECL int archive_read_support_filter_lzop(struct archive *);
__LA_DECL int archive_read_support_filter_none(struct archive *);
__LA_DECL int archive_read_support_filter_program(struct archive *,
                                                  const char *command);
__LA_DECL int archive_read_support_filter_program_signature(struct archive *,
                                                            const char *,
                                                            const void *,
                                                            size_t);
__LA_DECL int archive_read_support_filter_rpm(struct archive *);
__LA_DECL int archive_read_support_filter_uu(struct archive *);
__LA_DECL int archive_read_support_filter_xz(struct archive *);
__LA_DECL int archive_read_support_filter_zstd(struct archive *);

__LA_DECL int archive_read_support_format_7zip(struct archive *);
__LA_DECL int archive_read_support_format_all(struct archive *);
__LA_DECL int archive_read_support_format_ar(struct archive *);
__LA_DECL int archive_read_support_format_by_code(struct archive *, int);
__LA_DECL int archive_read_support_format_cab(struct archive *);
__LA_DECL int archive_read_support_format_cpio(struct archive *);
__LA_DECL int archive_read_support_format_empty(struct archive *);

__LA_DECL int archive_read_support_format_gnutar(struct archive *);
__LA_DECL int archive_read_support_format_iso9660(struct archive *);
__LA_DECL int archive_read_support_format_lha(struct archive *);
__LA_DECL int archive_read_support_format_mtree(struct archive *);
__LA_DECL int archive_read_support_format_rar(struct archive *);
__LA_DECL int archive_read_support_format_rar5(struct archive *);
__LA_DECL int archive_read_support_format_raw(struct archive *);
__LA_DECL int archive_read_support_format_tar(struct archive *);
__LA_DECL int archive_read_support_format_warc(struct archive *);
__LA_DECL int archive_read_support_format_xar(struct archive *);

__LA_DECL int archive_read_support_format_zip(struct archive *);

__LA_DECL int archive_read_support_format_zip_streamable(struct archive *);

__LA_DECL int archive_read_support_format_zip_seekable(struct archive *);

__LA_DECL int archive_read_set_format(struct archive *, int);
__LA_DECL int archive_read_append_filter(struct archive *, int);
__LA_DECL int archive_read_append_filter_program(struct archive *,
                                                 const char *);
__LA_DECL int archive_read_append_filter_program_signature(struct archive *,
                                                           const char *,
                                                           const void *,
                                                           size_t);

__LA_DECL int archive_read_set_open_callback(struct archive *,
                                             archive_open_callback *);
__LA_DECL int archive_read_set_read_callback(struct archive *,
                                             archive_read_callback *);
__LA_DECL int archive_read_set_seek_callback(struct archive *,
                                             archive_seek_callback *);
__LA_DECL int archive_read_set_skip_callback(struct archive *,
                                             archive_skip_callback *);
__LA_DECL int archive_read_set_close_callback(struct archive *,
                                              archive_close_callback *);

__LA_DECL int archive_read_set_switch_callback(struct archive *,
                                               archive_switch_callback *);

__LA_DECL int archive_read_set_callback_data(struct archive *, void *);

__LA_DECL int archive_read_set_callback_data2(struct archive *, void *,
                                              unsigned int);

__LA_DECL int archive_read_add_callback_data(struct archive *, void *,
                                             unsigned int);

__LA_DECL int archive_read_append_callback_data(struct archive *, void *);

__LA_DECL int archive_read_prepend_callback_data(struct archive *, void *);

__LA_DECL int archive_read_open1(struct archive *);

__LA_DECL int archive_read_open(struct archive *, void *_client_data,
                                archive_open_callback *,
                                archive_read_callback *,
                                archive_close_callback *);
__LA_DECL int archive_read_open2(struct archive *, void *_client_data,
                                 archive_open_callback *,
                                 archive_read_callback *,
                                 archive_skip_callback *,
                                 archive_close_callback *);

__LA_DECL int archive_read_open_filename(struct archive *,
                                         const char *_filename,
                                         size_t _block_size);

__LA_DECL int archive_read_open_filenames(struct archive *,
                                          const char **_filenames,
                                          size_t _block_size);
__LA_DECL int archive_read_open_filename_w(struct archive *,
                                           const wchar_t *_filename,
                                           size_t _block_size);
#if defined(_WIN32) && !defined(__CYGWIN__)
__LA_DECL int archive_read_open_filenames_w(struct archive *,
                                            const wchar_t **_filenames,
                                            size_t _block_size);
#endif

__LA_DECL int archive_read_open_file(struct archive *, const char *_filename,
                                     size_t _block_size) __LA_DEPRECATED;

__LA_DECL int archive_read_open_memory(struct archive *, const void *buff,
                                       size_t size);

__LA_DECL int archive_read_open_memory2(struct archive *a, const void *buff,
                                        size_t size, size_t read_size);

__LA_DECL int archive_read_open_fd(struct archive *, int _fd,
                                   size_t _block_size);

__LA_DECL int archive_read_open_FILE(struct archive *, FILE *_file);

__LA_DECL int archive_read_next_header(struct archive *,
                                       struct archive_entry **);

__LA_DECL int archive_read_next_header2(struct archive *,
                                        struct archive_entry *);

__LA_DECL la_int64_t archive_read_header_position(struct archive *);

__LA_DECL int archive_read_has_encrypted_entries(struct archive *);

__LA_DECL int archive_read_format_capabilities(struct archive *);

__LA_DECL la_ssize_t archive_read_data(struct archive *, void *, size_t);

__LA_DECL la_int64_t archive_seek_data(struct archive *, la_int64_t, int);

__LA_DECL int archive_read_data_block(struct archive *a, const void **buff,
                                      size_t *size, la_int64_t *offset);

__LA_DECL int archive_read_data_skip(struct archive *);
__LA_DECL int archive_read_data_into_fd(struct archive *, int fd);

__LA_DECL int archive_read_set_format_option(struct archive *_a, const char *m,
                                             const char *o, const char *v);

__LA_DECL int archive_read_set_filter_option(struct archive *_a, const char *m,
                                             const char *o, const char *v);

__LA_DECL int archive_read_set_option(struct archive *_a, const char *m,
                                      const char *o, const char *v);

__LA_DECL int archive_read_set_options(struct archive *_a, const char *opts);

__LA_DECL int archive_read_add_passphrase(struct archive *, const char *);
__LA_DECL int
archive_read_set_passphrase_callback(struct archive *, void *client_data,
                                     archive_passphrase_callback *);

#define ARCHIVE_EXTRACT_OWNER (0x0001)

#define ARCHIVE_EXTRACT_PERM (0x0002)

#define ARCHIVE_EXTRACT_TIME (0x0004)

#define ARCHIVE_EXTRACT_NO_OVERWRITE (0x0008)

#define ARCHIVE_EXTRACT_UNLINK (0x0010)

#define ARCHIVE_EXTRACT_ACL (0x0020)

#define ARCHIVE_EXTRACT_FFLAGS (0x0040)

#define ARCHIVE_EXTRACT_XATTR (0x0080)

#define ARCHIVE_EXTRACT_SECURE_SYMLINKS (0x0100)

#define ARCHIVE_EXTRACT_SECURE_NODOTDOT (0x0200)

#define ARCHIVE_EXTRACT_NO_AUTODIR (0x0400)

#define ARCHIVE_EXTRACT_NO_OVERWRITE_NEWER (0x0800)

#define ARCHIVE_EXTRACT_SPARSE (0x1000)

#define ARCHIVE_EXTRACT_MAC_METADATA (0x2000)

#define ARCHIVE_EXTRACT_NO_HFS_COMPRESSION (0x4000)

#define ARCHIVE_EXTRACT_HFS_COMPRESSION_FORCED (0x8000)

#define ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS (0x10000)

#define ARCHIVE_EXTRACT_CLEAR_NOCHANGE_FFLAGS (0x20000)

#define ARCHIVE_EXTRACT_SAFE_WRITES (0x40000)

__LA_DECL int archive_read_extract(struct archive *, struct archive_entry *,
                                   int flags);
__LA_DECL int archive_read_extract2(struct archive *, struct archive_entry *,
                                    struct archive *);
__LA_DECL void archive_read_extract_set_progress_callback(
    struct archive *, void (*_progress_func)(void *), void *_user_data);

__LA_DECL void archive_read_extract_set_skip_file(struct archive *, la_int64_t,
                                                  la_int64_t);

__LA_DECL int archive_read_close(struct archive *);

__LA_DECL int archive_read_free(struct archive *);
#if ARCHIVE_VERSION_NUMBER < 4000000

__LA_DECL int archive_read_finish(struct archive *) __LA_DEPRECATED;
#endif

__LA_DECL struct archive *archive_write_new(void);
__LA_DECL int archive_write_set_bytes_per_block(struct archive *,
                                                int bytes_per_block);
__LA_DECL int archive_write_get_bytes_per_block(struct archive *);

__LA_DECL int archive_write_set_bytes_in_last_block(struct archive *,
                                                    int bytes_in_last_block);
__LA_DECL int archive_write_get_bytes_in_last_block(struct archive *);

__LA_DECL int archive_write_set_skip_file(struct archive *, la_int64_t,
                                          la_int64_t);

#if ARCHIVE_VERSION_NUMBER < 4000000
__LA_DECL int
archive_write_set_compression_bzip2(struct archive *) __LA_DEPRECATED;
__LA_DECL int
archive_write_set_compression_compress(struct archive *) __LA_DEPRECATED;
__LA_DECL int
archive_write_set_compression_gzip(struct archive *) __LA_DEPRECATED;
__LA_DECL int
archive_write_set_compression_lzip(struct archive *) __LA_DEPRECATED;
__LA_DECL int
archive_write_set_compression_lzma(struct archive *) __LA_DEPRECATED;
__LA_DECL int
archive_write_set_compression_none(struct archive *) __LA_DEPRECATED;
__LA_DECL int
archive_write_set_compression_program(struct archive *,
                                      const char *cmd) __LA_DEPRECATED;
__LA_DECL int
archive_write_set_compression_xz(struct archive *) __LA_DEPRECATED;
#endif

__LA_DECL int archive_write_add_filter(struct archive *, int filter_code);
__LA_DECL int archive_write_add_filter_by_name(struct archive *,
                                               const char *name);
__LA_DECL int archive_write_add_filter_b64encode(struct archive *);
__LA_DECL int archive_write_add_filter_bzip2(struct archive *);
__LA_DECL int archive_write_add_filter_compress(struct archive *);
__LA_DECL int archive_write_add_filter_grzip(struct archive *);
__LA_DECL int archive_write_add_filter_gzip(struct archive *);
__LA_DECL int archive_write_add_filter_lrzip(struct archive *);
__LA_DECL int archive_write_add_filter_lz4(struct archive *);
__LA_DECL int archive_write_add_filter_lzip(struct archive *);
__LA_DECL int archive_write_add_filter_lzma(struct archive *);
__LA_DECL int archive_write_add_filter_lzop(struct archive *);
__LA_DECL int archive_write_add_filter_none(struct archive *);
__LA_DECL int archive_write_add_filter_program(struct archive *,
                                               const char *cmd);
__LA_DECL int archive_write_add_filter_uuencode(struct archive *);
__LA_DECL int archive_write_add_filter_xz(struct archive *);
__LA_DECL int archive_write_add_filter_zstd(struct archive *);

__LA_DECL int archive_write_set_format(struct archive *, int format_code);
__LA_DECL int archive_write_set_format_by_name(struct archive *,
                                               const char *name);

__LA_DECL int archive_write_set_format_7zip(struct archive *);
__LA_DECL int archive_write_set_format_ar_bsd(struct archive *);
__LA_DECL int archive_write_set_format_ar_svr4(struct archive *);
__LA_DECL int archive_write_set_format_cpio(struct archive *);
__LA_DECL int archive_write_set_format_cpio_bin(struct archive *);
__LA_DECL int archive_write_set_format_cpio_newc(struct archive *);
__LA_DECL int archive_write_set_format_cpio_odc(struct archive *);
__LA_DECL int archive_write_set_format_cpio_pwb(struct archive *);
__LA_DECL int archive_write_set_format_gnutar(struct archive *);
__LA_DECL int archive_write_set_format_iso9660(struct archive *);
__LA_DECL int archive_write_set_format_mtree(struct archive *);
__LA_DECL int archive_write_set_format_mtree_classic(struct archive *);

__LA_DECL int archive_write_set_format_pax(struct archive *);
__LA_DECL int archive_write_set_format_pax_restricted(struct archive *);
__LA_DECL int archive_write_set_format_raw(struct archive *);
__LA_DECL int archive_write_set_format_shar(struct archive *);
__LA_DECL int archive_write_set_format_shar_dump(struct archive *);
__LA_DECL int archive_write_set_format_ustar(struct archive *);
__LA_DECL int archive_write_set_format_v7tar(struct archive *);
__LA_DECL int archive_write_set_format_warc(struct archive *);
__LA_DECL int archive_write_set_format_xar(struct archive *);
__LA_DECL int archive_write_set_format_zip(struct archive *);
__LA_DECL int archive_write_set_format_filter_by_ext(struct archive *a,
                                                     const char *filename);
__LA_DECL int archive_write_set_format_filter_by_ext_def(struct archive *a,
                                                         const char *filename,
                                                         const char *def_ext);
__LA_DECL int archive_write_zip_set_compression_deflate(struct archive *);
__LA_DECL int archive_write_zip_set_compression_store(struct archive *);
__LA_DECL int archive_write_zip_set_compression_lzma(struct archive *);
__LA_DECL int archive_write_zip_set_compression_xz(struct archive *);
__LA_DECL int archive_write_zip_set_compression_bzip2(struct archive *);
__LA_DECL int archive_write_zip_set_compression_zstd(struct archive *);

__LA_DECL int archive_write_open(struct archive *, void *,
                                 archive_open_callback *,
                                 archive_write_callback *,
                                 archive_close_callback *);
__LA_DECL int archive_write_open2(struct archive *, void *,
                                  archive_open_callback *,
                                  archive_write_callback *,
                                  archive_close_callback *,
                                  archive_free_callback *);
__LA_DECL int archive_write_open_fd(struct archive *, int _fd);
__LA_DECL int archive_write_open_filename(struct archive *, const char *_file);
__LA_DECL int archive_write_open_filename_w(struct archive *,
                                            const wchar_t *_file);

__LA_DECL int archive_write_open_file(struct archive *,
                                      const char *_file) __LA_DEPRECATED;
__LA_DECL int archive_write_open_FILE(struct archive *, FILE *);

__LA_DECL int archive_write_open_memory(struct archive *, void *_buffer,
                                        size_t _buffSize, size_t *_used);

__LA_DECL int archive_write_header(struct archive *, struct archive_entry *);
__LA_DECL la_ssize_t archive_write_data(struct archive *, const void *, size_t);

__LA_DECL la_ssize_t archive_write_data_block(struct archive *, const void *,
                                              size_t, la_int64_t);

__LA_DECL int archive_write_finish_entry(struct archive *);
__LA_DECL int archive_write_close(struct archive *);

__LA_DECL int archive_write_fail(struct archive *);

__LA_DECL int archive_write_free(struct archive *);
#if ARCHIVE_VERSION_NUMBER < 4000000

__LA_DECL int archive_write_finish(struct archive *) __LA_DEPRECATED;
#endif

__LA_DECL int archive_write_set_format_option(struct archive *_a, const char *m,
                                              const char *o, const char *v);

__LA_DECL int archive_write_set_filter_option(struct archive *_a, const char *m,
                                              const char *o, const char *v);

__LA_DECL int archive_write_set_option(struct archive *_a, const char *m,
                                       const char *o, const char *v);

__LA_DECL int archive_write_set_options(struct archive *_a, const char *opts);

__LA_DECL int archive_write_set_passphrase(struct archive *_a, const char *p);
__LA_DECL int
archive_write_set_passphrase_callback(struct archive *, void *client_data,
                                      archive_passphrase_callback *);

__LA_DECL struct archive *archive_write_disk_new(void);

__LA_DECL int archive_write_disk_set_skip_file(struct archive *, la_int64_t,
                                               la_int64_t);

__LA_DECL int archive_write_disk_set_options(struct archive *, int flags);

__LA_DECL int archive_write_disk_set_standard_lookup(struct archive *);

__LA_DECL int archive_write_disk_set_group_lookup(
    struct archive *, void *, la_int64_t (*)(void *, const char *, la_int64_t),
    void (*)(void *));
__LA_DECL int archive_write_disk_set_user_lookup(
    struct archive *, void *, la_int64_t (*)(void *, const char *, la_int64_t),
    void (*)(void *));
__LA_DECL la_int64_t archive_write_disk_gid(struct archive *, const char *,
                                            la_int64_t);
__LA_DECL la_int64_t archive_write_disk_uid(struct archive *, const char *,
                                            la_int64_t);

__LA_DECL struct archive *archive_read_disk_new(void);

__LA_DECL int archive_read_disk_set_symlink_logical(struct archive *);

__LA_DECL int archive_read_disk_set_symlink_physical(struct archive *);

__LA_DECL int archive_read_disk_set_symlink_hybrid(struct archive *);

__LA_DECL int archive_read_disk_entry_from_file(struct archive *,
                                                struct archive_entry *, int,
                                                const struct stat *);

__LA_DECL const char *archive_read_disk_gname(struct archive *, la_int64_t);
__LA_DECL const char *archive_read_disk_uname(struct archive *, la_int64_t);

__LA_DECL int archive_read_disk_set_standard_lookup(struct archive *);

__LA_DECL int archive_read_disk_set_gname_lookup(struct archive *, void *,
                                                 const char *(*)(void *,
                                                                 la_int64_t),
                                                 void (*)(void *));
__LA_DECL int archive_read_disk_set_uname_lookup(struct archive *, void *,
                                                 const char *(*)(void *,
                                                                 la_int64_t),
                                                 void (*)(void *));

__LA_DECL int archive_read_disk_open(struct archive *, const char *);
__LA_DECL int archive_read_disk_open_w(struct archive *, const wchar_t *);

__LA_DECL int archive_read_disk_descend(struct archive *);
__LA_DECL int archive_read_disk_can_descend(struct archive *);
__LA_DECL int archive_read_disk_current_filesystem(struct archive *);
__LA_DECL int
archive_read_disk_current_filesystem_is_synthetic(struct archive *);
__LA_DECL int archive_read_disk_current_filesystem_is_remote(struct archive *);

__LA_DECL int archive_read_disk_set_atime_restored(struct archive *);

#define ARCHIVE_READDISK_RESTORE_ATIME (0x0001)

#define ARCHIVE_READDISK_HONOR_NODUMP (0x0002)

#define ARCHIVE_READDISK_MAC_COPYFILE (0x0004)

#define ARCHIVE_READDISK_NO_TRAVERSE_MOUNTS (0x0008)

#define ARCHIVE_READDISK_NO_XATTR (0x0010)

#define ARCHIVE_READDISK_NO_ACL (0x0020)

#define ARCHIVE_READDISK_NO_FFLAGS (0x0040)

#define ARCHIVE_READDISK_NO_SPARSE (0x0080)

__LA_DECL int archive_read_disk_set_behavior(struct archive *, int flags);

__LA_DECL int archive_read_disk_set_matching(
    struct archive *, struct archive *_matching,
    void (*_excluded_func)(struct archive *, void *, struct archive_entry *),
    void *_client_data);
__LA_DECL int archive_read_disk_set_metadata_filter_callback(
    struct archive *,
    int (*_metadata_filter_func)(struct archive *, void *,
                                 struct archive_entry *),
    void *_client_data);

__LA_DECL int archive_free(struct archive *);

__LA_DECL int archive_filter_count(struct archive *);
__LA_DECL la_int64_t archive_filter_bytes(struct archive *, int);
__LA_DECL int archive_filter_code(struct archive *, int);
__LA_DECL const char *archive_filter_name(struct archive *, int);

#if ARCHIVE_VERSION_NUMBER < 4000000

__LA_DECL
    la_int64_t archive_position_compressed(struct archive *) __LA_DEPRECATED;

__LA_DECL la_int64_t archive_position_uncompressed(struct archive *)
    __LA_DEPRECATED;

__LA_DECL const char *
archive_compression_name(struct archive *) __LA_DEPRECATED;

__LA_DECL int archive_compression(struct archive *) __LA_DEPRECATED;
#endif

__LA_DECL time_t archive_parse_date(time_t now, const char *datestr);

__LA_DECL int archive_errno(struct archive *);
__LA_DECL const char *archive_error_string(struct archive *);
__LA_DECL const char *archive_format_name(struct archive *);
__LA_DECL int archive_format(struct archive *);
__LA_DECL void archive_clear_error(struct archive *);
__LA_DECL void archive_set_error(struct archive *, int _err, const char *fmt,
                                 ...) __LA_PRINTF(3, 4);
__LA_DECL void archive_copy_error(struct archive *dest, struct archive *src);
__LA_DECL int archive_file_count(struct archive *);

__LA_DECL struct archive *archive_match_new(void);
__LA_DECL int archive_match_free(struct archive *);

__LA_DECL int archive_match_excluded(struct archive *, struct archive_entry *);

__LA_DECL int archive_match_path_excluded(struct archive *,
                                          struct archive_entry *);

__LA_DECL int archive_match_set_inclusion_recursion(struct archive *, int);

__LA_DECL int archive_match_exclude_pattern(struct archive *, const char *);
__LA_DECL int archive_match_exclude_pattern_w(struct archive *,
                                              const wchar_t *);

__LA_DECL int archive_match_exclude_pattern_from_file(struct archive *,
                                                      const char *,
                                                      int _nullSeparator);
__LA_DECL int archive_match_exclude_pattern_from_file_w(struct archive *,
                                                        const wchar_t *,
                                                        int _nullSeparator);

__LA_DECL int archive_match_include_pattern(struct archive *, const char *);
__LA_DECL int archive_match_include_pattern_w(struct archive *,
                                              const wchar_t *);

__LA_DECL int archive_match_include_pattern_from_file(struct archive *,
                                                      const char *,
                                                      int _nullSeparator);
__LA_DECL int archive_match_include_pattern_from_file_w(struct archive *,
                                                        const wchar_t *,
                                                        int _nullSeparator);

__LA_DECL int archive_match_path_unmatched_inclusions(struct archive *);

__LA_DECL int archive_match_path_unmatched_inclusions_next(struct archive *,
                                                           const char **);
__LA_DECL int archive_match_path_unmatched_inclusions_next_w(struct archive *,
                                                             const wchar_t **);

__LA_DECL int archive_match_time_excluded(struct archive *,
                                          struct archive_entry *);

#define ARCHIVE_MATCH_MTIME (0x0100)

#define ARCHIVE_MATCH_CTIME (0x0200)

#define ARCHIVE_MATCH_NEWER (0x0001)

#define ARCHIVE_MATCH_OLDER (0x0002)

#define ARCHIVE_MATCH_EQUAL (0x0010)

__LA_DECL int archive_match_include_time(struct archive *, int _flag,
                                         time_t _sec, long _nsec);

__LA_DECL int archive_match_include_date(struct archive *, int _flag,
                                         const char *_datestr);
__LA_DECL int archive_match_include_date_w(struct archive *, int _flag,
                                           const wchar_t *_datestr);

__LA_DECL int archive_match_include_file_time(struct archive *, int _flag,
                                              const char *_pathname);
__LA_DECL int archive_match_include_file_time_w(struct archive *, int _flag,
                                                const wchar_t *_pathname);

__LA_DECL int archive_match_exclude_entry(struct archive *, int _flag,
                                          struct archive_entry *);

__LA_DECL int archive_match_owner_excluded(struct archive *,
                                           struct archive_entry *);

__LA_DECL int archive_match_include_uid(struct archive *, la_int64_t);
__LA_DECL int archive_match_include_gid(struct archive *, la_int64_t);
__LA_DECL int archive_match_include_uname(struct archive *, const char *);
__LA_DECL int archive_match_include_uname_w(struct archive *, const wchar_t *);
__LA_DECL int archive_match_include_gname(struct archive *, const char *);
__LA_DECL int archive_match_include_gname_w(struct archive *, const wchar_t *);

#if ARCHIVE_VERSION_NUMBER < 4000000

__LA_DECL int archive_utility_string_sort(char **);
#endif

#ifdef __cplusplus
}
#endif

#undef __LA_DECL

#endif
