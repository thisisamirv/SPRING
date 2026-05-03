/*-
 * Copyright (c) 2003-2008 Tim Kientzle
 * Copyright (c) 2016 Martin Matuska
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

#ifndef ARCHIVE_ENTRY_H_INCLUDED
#define ARCHIVE_ENTRY_H_INCLUDED

#define ARCHIVE_VERSION_NUMBER 3008006

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#if ARCHIVE_VERSION_NUMBER < 4000000

#include <time.h>
#endif

#if defined(_WIN32) && !defined(__CYGWIN__)
#include <windows.h>
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

#if ARCHIVE_VERSION_NUMBER >= 3999000

#define __LA_MODE_T int
#elif defined(_WIN32) && !defined(__CYGWIN__) && !defined(__BORLANDC__) &&     \
    !defined(__WATCOMC__)
#define __LA_MODE_T unsigned short
#else
#define __LA_MODE_T mode_t
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

#if ARCHIVE_VERSION_NUMBER < 4000000

#define __LA_INO_T la_int64_t
#else

#define __LA_INO_T la_uint64_t
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

#if defined(__GNUC__) &&                                                       \
    (__GNUC__ > 3 || (__GNUC__ == 3 && __GNUC_MINOR__ >= 1))
#define __LA_DEPRECATED __attribute__((deprecated))
#else
#define __LA_DEPRECATED
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct archive;
struct archive_entry;

#define AE_IFMT ((__LA_MODE_T)0170000)
#define AE_IFREG ((__LA_MODE_T)0100000)
#define AE_IFLNK ((__LA_MODE_T)0120000)
#define AE_IFSOCK ((__LA_MODE_T)0140000)
#define AE_IFCHR ((__LA_MODE_T)0020000)
#define AE_IFBLK ((__LA_MODE_T)0060000)
#define AE_IFDIR ((__LA_MODE_T)0040000)
#define AE_IFIFO ((__LA_MODE_T)0010000)

#define AE_SYMLINK_TYPE_UNDEFINED 0
#define AE_SYMLINK_TYPE_FILE 1
#define AE_SYMLINK_TYPE_DIRECTORY 2

__LA_DECL struct archive_entry *archive_entry_clear(struct archive_entry *);

__LA_DECL struct archive_entry *archive_entry_clone(struct archive_entry *);
__LA_DECL void archive_entry_free(struct archive_entry *);
__LA_DECL struct archive_entry *archive_entry_new(void);

__LA_DECL struct archive_entry *archive_entry_new2(struct archive *);

__LA_DECL __LA_TIME_T archive_entry_atime(struct archive_entry *);
__LA_DECL long archive_entry_atime_nsec(struct archive_entry *);
__LA_DECL int archive_entry_atime_is_set(struct archive_entry *);
__LA_DECL __LA_TIME_T archive_entry_birthtime(struct archive_entry *);
__LA_DECL long archive_entry_birthtime_nsec(struct archive_entry *);
__LA_DECL int archive_entry_birthtime_is_set(struct archive_entry *);
__LA_DECL __LA_TIME_T archive_entry_ctime(struct archive_entry *);
__LA_DECL long archive_entry_ctime_nsec(struct archive_entry *);
__LA_DECL int archive_entry_ctime_is_set(struct archive_entry *);
__LA_DECL __LA_DEV_T archive_entry_dev(struct archive_entry *);
__LA_DECL int archive_entry_dev_is_set(struct archive_entry *);
__LA_DECL __LA_DEV_T archive_entry_devmajor(struct archive_entry *);
__LA_DECL __LA_DEV_T archive_entry_devminor(struct archive_entry *);
__LA_DECL __LA_MODE_T archive_entry_filetype(struct archive_entry *);
__LA_DECL int archive_entry_filetype_is_set(struct archive_entry *);
__LA_DECL void archive_entry_fflags(struct archive_entry *, unsigned long *,
                                    unsigned long *);
__LA_DECL const char *archive_entry_fflags_text(struct archive_entry *);
__LA_DECL la_int64_t archive_entry_gid(struct archive_entry *);
__LA_DECL int archive_entry_gid_is_set(struct archive_entry *);
__LA_DECL const char *archive_entry_gname(struct archive_entry *);
__LA_DECL const char *archive_entry_gname_utf8(struct archive_entry *);
__LA_DECL const wchar_t *archive_entry_gname_w(struct archive_entry *);
__LA_DECL void archive_entry_set_link_to_hardlink(struct archive_entry *);
__LA_DECL const char *archive_entry_hardlink(struct archive_entry *);
__LA_DECL const char *archive_entry_hardlink_utf8(struct archive_entry *);
__LA_DECL const wchar_t *archive_entry_hardlink_w(struct archive_entry *);
__LA_DECL int archive_entry_hardlink_is_set(struct archive_entry *);
__LA_DECL __LA_INO_T archive_entry_ino(struct archive_entry *);
__LA_DECL __LA_INO_T archive_entry_ino64(struct archive_entry *);
__LA_DECL int archive_entry_ino_is_set(struct archive_entry *);
__LA_DECL __LA_MODE_T archive_entry_mode(struct archive_entry *);
__LA_DECL time_t archive_entry_mtime(struct archive_entry *);
__LA_DECL long archive_entry_mtime_nsec(struct archive_entry *);
__LA_DECL int archive_entry_mtime_is_set(struct archive_entry *);
__LA_DECL unsigned int archive_entry_nlink(struct archive_entry *);
__LA_DECL const char *archive_entry_pathname(struct archive_entry *);
__LA_DECL const char *archive_entry_pathname_utf8(struct archive_entry *);
__LA_DECL const wchar_t *archive_entry_pathname_w(struct archive_entry *);
__LA_DECL __LA_MODE_T archive_entry_perm(struct archive_entry *);
__LA_DECL int archive_entry_perm_is_set(struct archive_entry *);
__LA_DECL int archive_entry_rdev_is_set(struct archive_entry *);
__LA_DECL __LA_DEV_T archive_entry_rdev(struct archive_entry *);
__LA_DECL __LA_DEV_T archive_entry_rdevmajor(struct archive_entry *);
__LA_DECL __LA_DEV_T archive_entry_rdevminor(struct archive_entry *);
__LA_DECL const char *archive_entry_sourcepath(struct archive_entry *);
__LA_DECL const wchar_t *archive_entry_sourcepath_w(struct archive_entry *);
__LA_DECL la_int64_t archive_entry_size(struct archive_entry *);
__LA_DECL int archive_entry_size_is_set(struct archive_entry *);
__LA_DECL const char *archive_entry_strmode(struct archive_entry *);
__LA_DECL void archive_entry_set_link_to_symlink(struct archive_entry *);
__LA_DECL const char *archive_entry_symlink(struct archive_entry *);
__LA_DECL const char *archive_entry_symlink_utf8(struct archive_entry *);
__LA_DECL int archive_entry_symlink_type(struct archive_entry *);
__LA_DECL const wchar_t *archive_entry_symlink_w(struct archive_entry *);
__LA_DECL la_int64_t archive_entry_uid(struct archive_entry *);
__LA_DECL int archive_entry_uid_is_set(struct archive_entry *);
__LA_DECL const char *archive_entry_uname(struct archive_entry *);
__LA_DECL const char *archive_entry_uname_utf8(struct archive_entry *);
__LA_DECL const wchar_t *archive_entry_uname_w(struct archive_entry *);
__LA_DECL int archive_entry_is_data_encrypted(struct archive_entry *);
__LA_DECL int archive_entry_is_metadata_encrypted(struct archive_entry *);
__LA_DECL int archive_entry_is_encrypted(struct archive_entry *);

__LA_DECL void archive_entry_set_atime(struct archive_entry *, __LA_TIME_T,
                                       long);
__LA_DECL void archive_entry_unset_atime(struct archive_entry *);
#if defined(_WIN32) && !defined(__CYGWIN__)
__LA_DECL void archive_entry_copy_bhfi(struct archive_entry *,
                                       BY_HANDLE_FILE_INFORMATION *);
#endif
__LA_DECL void archive_entry_set_birthtime(struct archive_entry *, __LA_TIME_T,
                                           long);
__LA_DECL void archive_entry_unset_birthtime(struct archive_entry *);
__LA_DECL void archive_entry_set_ctime(struct archive_entry *, __LA_TIME_T,
                                       long);
__LA_DECL void archive_entry_unset_ctime(struct archive_entry *);
__LA_DECL void archive_entry_set_dev(struct archive_entry *, __LA_DEV_T);
__LA_DECL void archive_entry_set_devmajor(struct archive_entry *, __LA_DEV_T);
__LA_DECL void archive_entry_set_devminor(struct archive_entry *, __LA_DEV_T);
__LA_DECL void archive_entry_set_filetype(struct archive_entry *, unsigned int);
__LA_DECL void archive_entry_set_fflags(struct archive_entry *, unsigned long,
                                        unsigned long);

__LA_DECL const char *archive_entry_copy_fflags_text(struct archive_entry *,
                                                     const char *);
__LA_DECL const char *archive_entry_copy_fflags_text_len(struct archive_entry *,
                                                         const char *, size_t);
__LA_DECL const wchar_t *
archive_entry_copy_fflags_text_w(struct archive_entry *, const wchar_t *);
__LA_DECL void archive_entry_set_gid(struct archive_entry *, la_int64_t);
__LA_DECL void archive_entry_set_gname(struct archive_entry *, const char *);
__LA_DECL void archive_entry_set_gname_utf8(struct archive_entry *,
                                            const char *);
__LA_DECL void archive_entry_copy_gname(struct archive_entry *, const char *);
__LA_DECL void archive_entry_copy_gname_w(struct archive_entry *,
                                          const wchar_t *);
__LA_DECL int archive_entry_update_gname_utf8(struct archive_entry *,
                                              const char *);
__LA_DECL void archive_entry_set_hardlink(struct archive_entry *, const char *);
__LA_DECL void archive_entry_set_hardlink_utf8(struct archive_entry *,
                                               const char *);
__LA_DECL void archive_entry_copy_hardlink(struct archive_entry *,
                                           const char *);
__LA_DECL void archive_entry_copy_hardlink_w(struct archive_entry *,
                                             const wchar_t *);
__LA_DECL int archive_entry_update_hardlink_utf8(struct archive_entry *,
                                                 const char *);
__LA_DECL void archive_entry_set_ino(struct archive_entry *, __LA_INO_T);
__LA_DECL void archive_entry_set_ino64(struct archive_entry *, __LA_INO_T);
__LA_DECL void archive_entry_set_link(struct archive_entry *, const char *);
__LA_DECL void archive_entry_set_link_utf8(struct archive_entry *,
                                           const char *);
__LA_DECL void archive_entry_copy_link(struct archive_entry *, const char *);
__LA_DECL void archive_entry_copy_link_w(struct archive_entry *,
                                         const wchar_t *);
__LA_DECL int archive_entry_update_link_utf8(struct archive_entry *,
                                             const char *);
__LA_DECL void archive_entry_set_mode(struct archive_entry *, __LA_MODE_T);
__LA_DECL void archive_entry_set_mtime(struct archive_entry *, __LA_TIME_T,
                                       long);
__LA_DECL void archive_entry_unset_mtime(struct archive_entry *);
__LA_DECL void archive_entry_set_nlink(struct archive_entry *, unsigned int);
__LA_DECL void archive_entry_set_pathname(struct archive_entry *, const char *);
__LA_DECL void archive_entry_set_pathname_utf8(struct archive_entry *,
                                               const char *);
__LA_DECL void archive_entry_copy_pathname(struct archive_entry *,
                                           const char *);
__LA_DECL void archive_entry_copy_pathname_w(struct archive_entry *,
                                             const wchar_t *);
__LA_DECL int archive_entry_update_pathname_utf8(struct archive_entry *,
                                                 const char *);
__LA_DECL void archive_entry_set_perm(struct archive_entry *, __LA_MODE_T);
__LA_DECL void archive_entry_set_rdev(struct archive_entry *, __LA_DEV_T);
__LA_DECL void archive_entry_set_rdevmajor(struct archive_entry *, __LA_DEV_T);
__LA_DECL void archive_entry_set_rdevminor(struct archive_entry *, __LA_DEV_T);
__LA_DECL void archive_entry_set_size(struct archive_entry *, la_int64_t);
__LA_DECL void archive_entry_unset_size(struct archive_entry *);
__LA_DECL void archive_entry_copy_sourcepath(struct archive_entry *,
                                             const char *);
__LA_DECL void archive_entry_copy_sourcepath_w(struct archive_entry *,
                                               const wchar_t *);
__LA_DECL void archive_entry_set_symlink(struct archive_entry *, const char *);
__LA_DECL void archive_entry_set_symlink_type(struct archive_entry *, int);
__LA_DECL void archive_entry_set_symlink_utf8(struct archive_entry *,
                                              const char *);
__LA_DECL void archive_entry_copy_symlink(struct archive_entry *, const char *);
__LA_DECL void archive_entry_copy_symlink_w(struct archive_entry *,
                                            const wchar_t *);
__LA_DECL int archive_entry_update_symlink_utf8(struct archive_entry *,
                                                const char *);
__LA_DECL void archive_entry_set_uid(struct archive_entry *, la_int64_t);
__LA_DECL void archive_entry_set_uname(struct archive_entry *, const char *);
__LA_DECL void archive_entry_set_uname_utf8(struct archive_entry *,
                                            const char *);
__LA_DECL void archive_entry_copy_uname(struct archive_entry *, const char *);
__LA_DECL void archive_entry_copy_uname_w(struct archive_entry *,
                                          const wchar_t *);
__LA_DECL int archive_entry_update_uname_utf8(struct archive_entry *,
                                              const char *);
__LA_DECL void archive_entry_set_is_data_encrypted(struct archive_entry *,
                                                   char is_encrypted);
__LA_DECL void archive_entry_set_is_metadata_encrypted(struct archive_entry *,
                                                       char is_encrypted);

__LA_DECL const struct stat *archive_entry_stat(struct archive_entry *);
__LA_DECL void archive_entry_copy_stat(struct archive_entry *,
                                       const struct stat *);

__LA_DECL const void *archive_entry_mac_metadata(struct archive_entry *,
                                                 size_t *);
__LA_DECL void archive_entry_copy_mac_metadata(struct archive_entry *,
                                               const void *, size_t);

#define ARCHIVE_ENTRY_DIGEST_MD5 0x00000001
#define ARCHIVE_ENTRY_DIGEST_RMD160 0x00000002
#define ARCHIVE_ENTRY_DIGEST_SHA1 0x00000003
#define ARCHIVE_ENTRY_DIGEST_SHA256 0x00000004
#define ARCHIVE_ENTRY_DIGEST_SHA384 0x00000005
#define ARCHIVE_ENTRY_DIGEST_SHA512 0x00000006

__LA_DECL const unsigned char *archive_entry_digest(struct archive_entry *,
                                                    int);
__LA_DECL int archive_entry_set_digest(struct archive_entry *, int,
                                       const unsigned char *);

#define ARCHIVE_ENTRY_ACL_EXECUTE 0x00000001
#define ARCHIVE_ENTRY_ACL_WRITE 0x00000002
#define ARCHIVE_ENTRY_ACL_READ 0x00000004
#define ARCHIVE_ENTRY_ACL_READ_DATA 0x00000008
#define ARCHIVE_ENTRY_ACL_LIST_DIRECTORY 0x00000008
#define ARCHIVE_ENTRY_ACL_WRITE_DATA 0x00000010
#define ARCHIVE_ENTRY_ACL_ADD_FILE 0x00000010
#define ARCHIVE_ENTRY_ACL_APPEND_DATA 0x00000020
#define ARCHIVE_ENTRY_ACL_ADD_SUBDIRECTORY 0x00000020
#define ARCHIVE_ENTRY_ACL_READ_NAMED_ATTRS 0x00000040
#define ARCHIVE_ENTRY_ACL_WRITE_NAMED_ATTRS 0x00000080
#define ARCHIVE_ENTRY_ACL_DELETE_CHILD 0x00000100
#define ARCHIVE_ENTRY_ACL_READ_ATTRIBUTES 0x00000200
#define ARCHIVE_ENTRY_ACL_WRITE_ATTRIBUTES 0x00000400
#define ARCHIVE_ENTRY_ACL_DELETE 0x00000800
#define ARCHIVE_ENTRY_ACL_READ_ACL 0x00001000
#define ARCHIVE_ENTRY_ACL_WRITE_ACL 0x00002000
#define ARCHIVE_ENTRY_ACL_WRITE_OWNER 0x00004000
#define ARCHIVE_ENTRY_ACL_SYNCHRONIZE 0x00008000

#define ARCHIVE_ENTRY_ACL_PERMS_POSIX1E                                        \
  (ARCHIVE_ENTRY_ACL_EXECUTE | ARCHIVE_ENTRY_ACL_WRITE | ARCHIVE_ENTRY_ACL_READ)

#define ARCHIVE_ENTRY_ACL_PERMS_NFS4                                           \
  (ARCHIVE_ENTRY_ACL_EXECUTE | ARCHIVE_ENTRY_ACL_READ_DATA |                   \
   ARCHIVE_ENTRY_ACL_LIST_DIRECTORY | ARCHIVE_ENTRY_ACL_WRITE_DATA |           \
   ARCHIVE_ENTRY_ACL_ADD_FILE | ARCHIVE_ENTRY_ACL_APPEND_DATA |                \
   ARCHIVE_ENTRY_ACL_ADD_SUBDIRECTORY | ARCHIVE_ENTRY_ACL_READ_NAMED_ATTRS |   \
   ARCHIVE_ENTRY_ACL_WRITE_NAMED_ATTRS | ARCHIVE_ENTRY_ACL_DELETE_CHILD |      \
   ARCHIVE_ENTRY_ACL_READ_ATTRIBUTES | ARCHIVE_ENTRY_ACL_WRITE_ATTRIBUTES |    \
   ARCHIVE_ENTRY_ACL_DELETE | ARCHIVE_ENTRY_ACL_READ_ACL |                     \
   ARCHIVE_ENTRY_ACL_WRITE_ACL | ARCHIVE_ENTRY_ACL_WRITE_OWNER |               \
   ARCHIVE_ENTRY_ACL_SYNCHRONIZE)

#define ARCHIVE_ENTRY_ACL_ENTRY_INHERITED 0x01000000
#define ARCHIVE_ENTRY_ACL_ENTRY_FILE_INHERIT 0x02000000
#define ARCHIVE_ENTRY_ACL_ENTRY_DIRECTORY_INHERIT 0x04000000
#define ARCHIVE_ENTRY_ACL_ENTRY_NO_PROPAGATE_INHERIT 0x08000000
#define ARCHIVE_ENTRY_ACL_ENTRY_INHERIT_ONLY 0x10000000
#define ARCHIVE_ENTRY_ACL_ENTRY_SUCCESSFUL_ACCESS 0x20000000
#define ARCHIVE_ENTRY_ACL_ENTRY_FAILED_ACCESS 0x40000000

#define ARCHIVE_ENTRY_ACL_INHERITANCE_NFS4                                     \
  (ARCHIVE_ENTRY_ACL_ENTRY_FILE_INHERIT |                                      \
   ARCHIVE_ENTRY_ACL_ENTRY_DIRECTORY_INHERIT |                                 \
   ARCHIVE_ENTRY_ACL_ENTRY_NO_PROPAGATE_INHERIT |                              \
   ARCHIVE_ENTRY_ACL_ENTRY_INHERIT_ONLY |                                      \
   ARCHIVE_ENTRY_ACL_ENTRY_SUCCESSFUL_ACCESS |                                 \
   ARCHIVE_ENTRY_ACL_ENTRY_FAILED_ACCESS | ARCHIVE_ENTRY_ACL_ENTRY_INHERITED)

#define ARCHIVE_ENTRY_ACL_TYPE_ACCESS 0x00000100
#define ARCHIVE_ENTRY_ACL_TYPE_DEFAULT 0x00000200
#define ARCHIVE_ENTRY_ACL_TYPE_ALLOW 0x00000400
#define ARCHIVE_ENTRY_ACL_TYPE_DENY 0x00000800
#define ARCHIVE_ENTRY_ACL_TYPE_AUDIT 0x00001000
#define ARCHIVE_ENTRY_ACL_TYPE_ALARM 0x00002000
#define ARCHIVE_ENTRY_ACL_TYPE_POSIX1E                                         \
  (ARCHIVE_ENTRY_ACL_TYPE_ACCESS | ARCHIVE_ENTRY_ACL_TYPE_DEFAULT)
#define ARCHIVE_ENTRY_ACL_TYPE_NFS4                                            \
  (ARCHIVE_ENTRY_ACL_TYPE_ALLOW | ARCHIVE_ENTRY_ACL_TYPE_DENY |                \
   ARCHIVE_ENTRY_ACL_TYPE_AUDIT | ARCHIVE_ENTRY_ACL_TYPE_ALARM)

#define ARCHIVE_ENTRY_ACL_USER 10001
#define ARCHIVE_ENTRY_ACL_USER_OBJ 10002
#define ARCHIVE_ENTRY_ACL_GROUP 10003
#define ARCHIVE_ENTRY_ACL_GROUP_OBJ 10004
#define ARCHIVE_ENTRY_ACL_MASK 10005
#define ARCHIVE_ENTRY_ACL_OTHER 10006
#define ARCHIVE_ENTRY_ACL_EVERYONE 10107

__LA_DECL void archive_entry_acl_clear(struct archive_entry *);
__LA_DECL int archive_entry_acl_add_entry(struct archive_entry *, int, int, int,
                                          int, const char *);
__LA_DECL int archive_entry_acl_add_entry_w(struct archive_entry *, int, int,
                                            int, int, const wchar_t *);

__LA_DECL int archive_entry_acl_reset(struct archive_entry *, int);
__LA_DECL int archive_entry_acl_next(struct archive_entry *, int, int *, int *,
                                     int *, int *, const char **);

#define ARCHIVE_ENTRY_ACL_STYLE_EXTRA_ID 0x00000001
#define ARCHIVE_ENTRY_ACL_STYLE_MARK_DEFAULT 0x00000002
#define ARCHIVE_ENTRY_ACL_STYLE_SOLARIS 0x00000004
#define ARCHIVE_ENTRY_ACL_STYLE_SEPARATOR_COMMA 0x00000008
#define ARCHIVE_ENTRY_ACL_STYLE_COMPACT 0x00000010

__LA_DECL wchar_t *archive_entry_acl_to_text_w(struct archive_entry *,
                                               la_ssize_t *, int);
__LA_DECL char *archive_entry_acl_to_text(struct archive_entry *, la_ssize_t *,
                                          int);
__LA_DECL int archive_entry_acl_from_text_w(struct archive_entry *,
                                            const wchar_t *, int);
__LA_DECL int archive_entry_acl_from_text(struct archive_entry *, const char *,
                                          int);

#define OLD_ARCHIVE_ENTRY_ACL_STYLE_EXTRA_ID 1024
#define OLD_ARCHIVE_ENTRY_ACL_STYLE_MARK_DEFAULT 2048

__LA_DECL const wchar_t *archive_entry_acl_text_w(struct archive_entry *,
                                                  int) __LA_DEPRECATED;
__LA_DECL const char *archive_entry_acl_text(struct archive_entry *,
                                             int) __LA_DEPRECATED;

__LA_DECL int archive_entry_acl_types(struct archive_entry *);

__LA_DECL int archive_entry_acl_count(struct archive_entry *, int);

struct archive_acl;
__LA_DECL struct archive_acl *archive_entry_acl(struct archive_entry *);

__LA_DECL void archive_entry_xattr_clear(struct archive_entry *);
__LA_DECL void archive_entry_xattr_add_entry(struct archive_entry *,
                                             const char *, const void *,
                                             size_t);

__LA_DECL int archive_entry_xattr_count(struct archive_entry *);
__LA_DECL int archive_entry_xattr_reset(struct archive_entry *);
__LA_DECL int archive_entry_xattr_next(struct archive_entry *, const char **,
                                       const void **, size_t *);

__LA_DECL void archive_entry_sparse_clear(struct archive_entry *);
__LA_DECL void archive_entry_sparse_add_entry(struct archive_entry *,
                                              la_int64_t, la_int64_t);

__LA_DECL int archive_entry_sparse_count(struct archive_entry *);
__LA_DECL int archive_entry_sparse_reset(struct archive_entry *);
__LA_DECL int archive_entry_sparse_next(struct archive_entry *, la_int64_t *,
                                        la_int64_t *);

struct archive_entry_linkresolver;

__LA_DECL struct archive_entry_linkresolver *
archive_entry_linkresolver_new(void);
__LA_DECL void
archive_entry_linkresolver_set_strategy(struct archive_entry_linkresolver *,
                                        int);
__LA_DECL void
archive_entry_linkresolver_free(struct archive_entry_linkresolver *);
__LA_DECL void archive_entry_linkify(struct archive_entry_linkresolver *,
                                     struct archive_entry **,
                                     struct archive_entry **);
__LA_DECL struct archive_entry *
archive_entry_partial_links(struct archive_entry_linkresolver *res,
                            unsigned int *links);
#ifdef __cplusplus
}
#endif

#undef __LA_DECL

#endif
