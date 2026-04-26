/*-
 * Copyright (c) 2003-2007 Tim Kientzle
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

/* !!ONLY FOR USE INTERNALLY TO LIBARCHIVE!! */

/*
 * This header is the first thing included in any of the libarchive
 * source files.  As far as possible, platform-specific issues should
 * be dealt with here and not within individual source files.  I'm
 * actively trying to minimize #if blocks within the main source,
 * since they obfuscate the code.
 */

#ifndef ARCHIVE_PLATFORM_H_INCLUDED
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_SIZE_T 8
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#if defined(_WIN32) && !defined(__CYGWIN__)
#define HAVE_DECL_SSIZE_MAX 0
#else
#define HAVE_DECL_SSIZE_MAX 1
#endif
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define __LIBARCHIVE_CONFIG_H_INCLUDED 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCSCHR 1
#define HAVE_WCSDUP 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_MEMMOVE 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_ERRNO_H 1
#define HAVE_LIMITS_H 1
#define HAVE_WCHAR_H 1
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#if defined(__clang__)
/* Prevent clangd/include-cleaner from removing stdlib.h: reference a symbol
 * in an unevaluated context so this has no runtime effect. */
static inline void __la_mark_stdlib_used(void) { (void)sizeof(malloc(0)); }
#endif
#include <string.h>
#if defined(__clang__)
/* Prevent clangd/include-cleaner from removing string.h. */
static inline void __la_mark_string_used(void) { (void)sizeof(strlen("")); }
#endif
#include <sys/stat.h>
#include <sys/types.h>
#if !defined(uid_t) && !defined(_UID_T_DECLARED)
/* Ensure basic POSIX types exist for parsing in environments lacking
 * full system headers. Real builds provide these via <sys/types.h>. */
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#if !defined(ULONG_PTR) && !defined(_WIN32)
/* Minimal fallback for pointer-sized unsigned type when not on Windows.
 * On Windows, system headers (e.g., basetsd.h) provide ULONG_PTR with the
 * appropriate width; avoid redefining it to prevent type mismatch errors. */
typedef unsigned long ULONG_PTR;
#endif
#if !defined(uid_t)
typedef unsigned int uid_t;
#define _UID_T_DECLARED
#endif
#if !defined(gid_t)
typedef unsigned int gid_t;
#define _GID_T_DECLARED
#endif
#if !defined(pid_t) && !defined(_PID_T_) && !defined(_PID_T_DECLARED)
typedef int pid_t;
#define _PID_T_DECLARED
#endif
#if !defined(ssize_t)
#if defined(_WIN64)
typedef __int64 ssize_t;
#else
typedef long ssize_t;
#endif
#define _SSIZE_T_DECLARED
#endif
#if !defined(mode_t)
typedef unsigned short mode_t;
#define _MODE_T_DECLARED
#endif
#if !defined(dev_t)
typedef unsigned int dev_t;
#define _DEV_T_DECLARED
#endif
#ifdef __cplusplus
}
#endif
#endif
#include <wchar.h>
#if defined(__clang__)
/* Prevent clangd/include-cleaner from removing wchar.h. */
static inline void __la_mark_wchar_used(void) {
  (void)sizeof(wcschr(L"", L'a'));
}
#endif
#if !defined(_WIN32)
#define HAVE_UNISTD_H 1
#define HAVE_DIRENT_H 1
#define HAVE_PWD_H 1
#define HAVE_GRP_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_GETTIMEOFDAY 1
#include <dirent.h>
#include <grp.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>
#ifndef HAVE_SYS_IOCTL_H
#define HAVE_SYS_IOCTL_H 1
#endif
#endif
#ifndef __LA_FALLTHROUGH
#if defined(__GNUC__) && (__GNUC__ >= 7)
#define __LA_FALLTHROUGH __attribute__((fallthrough))
#else
#define __LA_FALLTHROUGH
#endif
#endif
#endif

#if defined(__linux__) || defined(__APPLE__)
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_SIZE_T 8
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define __LIBARCHIVE_CONFIG_H_INCLUDED 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCSCHR 1
#define HAVE_WCSDUP 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_MEMMOVE 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_DIRENT_H 1
#define HAVE_FCHDIR 1
#define HAVE_FSTATAT 1
#define HAVE_OPENAT 1
#define HAVE_FCHMOD 1
#define HAVE_FCHOWN 1
#define HAVE_LUTIMES 1
#define HAVE_PWD_H 1
#define HAVE_GRP_H 1
#define HAVE_GETPWUID 1
#define HAVE_GETGRGID 1
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <pwd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <wchar.h>
#ifndef HAVE_SYS_IOCTL_H
#define HAVE_SYS_IOCTL_H 1
#endif
#ifndef __LA_FALLTHROUGH
#if defined(__GNUC__) && (__GNUC__ >= 7)
#define __LA_FALLTHROUGH __attribute__((fallthrough))
#else
#define __LA_FALLTHROUGH
#endif
#endif
#endif

#if defined(__linux__) || defined(__APPLE__)
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_SIZE_T 8
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define __LIBARCHIVE_CONFIG_H_INCLUDED 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCSCHR 1
#define HAVE_WCSDUP 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_MEMMOVE 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_DIRENT_H 1
#define HAVE_FCHDIR 1
#define HAVE_FSTATAT 1
#define HAVE_OPENAT 1
#define HAVE_FCHMOD 1
#define HAVE_FCHOWN 1
#define HAVE_LUTIMES 1
#define HAVE_PWD_H 1
#define HAVE_GRP_H 1
#define HAVE_GETPWUID 1
#define HAVE_GETGRGID 1
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <pwd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <wchar.h>
#ifndef HAVE_SYS_IOCTL_H
#define HAVE_SYS_IOCTL_H 1
#endif
#ifndef __LA_FALLTHROUGH
#if defined(__GNUC__) && (__GNUC__ >= 7)
#define __LA_FALLTHROUGH __attribute__((fallthrough))
#else
#define __LA_FALLTHROUGH
#endif
#endif
#endif

#if defined(__linux__) || defined(__APPLE__)
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_SIZE_T 8
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define __LIBARCHIVE_CONFIG_H_INCLUDED 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCSCHR 1
#define HAVE_WCSDUP 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_MEMMOVE 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_DIRENT_H 1
#define HAVE_FCHDIR 1
#define HAVE_FSTATAT 1
#define HAVE_OPENAT 1
#define HAVE_FCHMOD 1
#define HAVE_FCHOWN 1
#define HAVE_LUTIMES 1
#define HAVE_PWD_H 1
#define HAVE_GRP_H 1
#define HAVE_GETPWUID 1
#define HAVE_GETGRGID 1
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <pwd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <wchar.h>
#ifndef HAVE_SYS_IOCTL_H
#define HAVE_SYS_IOCTL_H 1
#endif
#ifndef __LA_FALLTHROUGH
#if defined(__GNUC__) && (__GNUC__ >= 7)
#define __LA_FALLTHROUGH __attribute__((fallthrough))
#else
#define __LA_FALLTHROUGH
#endif
#endif
#endif

#if defined(__linux__) || defined(__APPLE__)
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_SIZE_T 8
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define __LIBARCHIVE_CONFIG_H_INCLUDED 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCSCHR 1
#define HAVE_WCSDUP 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_MEMMOVE 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_DIRENT_H 1
#define HAVE_FCHDIR 1
#define HAVE_FSTATAT 1
#define HAVE_OPENAT 1
#define HAVE_FCHMOD 1
#define HAVE_FCHOWN 1
#define HAVE_LUTIMES 1
#define HAVE_PWD_H 1
#define HAVE_GRP_H 1
#define HAVE_GETPWUID 1
#define HAVE_GETGRGID 1
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <pwd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <wchar.h>
#ifndef HAVE_SYS_IOCTL_H
#define HAVE_SYS_IOCTL_H 1
#endif
#ifndef __LA_FALLTHROUGH
#if defined(__GNUC__) && (__GNUC__ >= 7)
#define __LA_FALLTHROUGH __attribute__((fallthrough))
#else
#define __LA_FALLTHROUGH
#endif
#endif
#endif

#if defined(__linux__) || defined(__APPLE__)
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_SIZE_T 8
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define __LIBARCHIVE_CONFIG_H_INCLUDED 1
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <wchar.h>
#ifndef HAVE_SYS_IOCTL_H
#define HAVE_SYS_IOCTL_H 1
#endif
#ifndef __LA_FALLTHROUGH
#if defined(__GNUC__) && (__GNUC__ >= 7)
#define __LA_FALLTHROUGH __attribute__((fallthrough))
#else
#define __LA_FALLTHROUGH
#endif
#endif
#endif

#if defined(__linux__) || defined(__APPLE__)
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_SIZE_T 8
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define __LIBARCHIVE_CONFIG_H_INCLUDED 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCSCHR 1
#define HAVE_WCSDUP 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_MEMMOVE 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_DIRENT_H 1
#define HAVE_FCHDIR 1
#define HAVE_FSTATAT 1
#define HAVE_OPENAT 1
#define HAVE_FCHMOD 1
#define HAVE_FCHOWN 1
#define HAVE_LUTIMES 1
#define HAVE_PWD_H 1
#define HAVE_GRP_H 1
#define HAVE_GETPWUID 1
#define HAVE_GETGRGID 1
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <pwd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <wchar.h>
#ifndef HAVE_SYS_IOCTL_H
#define HAVE_SYS_IOCTL_H 1
#endif
#ifndef __LA_FALLTHROUGH
#if defined(__GNUC__) && (__GNUC__ >= 7)
#define __LA_FALLTHROUGH __attribute__((fallthrough))
#else
#define __LA_FALLTHROUGH
#endif
#endif
#endif

#if defined(__linux__) || defined(__APPLE__)
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_SIZE_T 8
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define __LIBARCHIVE_CONFIG_H_INCLUDED 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCSCHR 1
#define HAVE_WCSDUP 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_MEMMOVE 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_DIRENT_H 1
#define HAVE_FCHDIR 1
#define HAVE_FSTATAT 1
#define HAVE_OPENAT 1
#define HAVE_FCHMOD 1
#define HAVE_FCHOWN 1
#define HAVE_LUTIMES 1
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <wchar.h>
#ifndef HAVE_SYS_IOCTL_H
#define HAVE_SYS_IOCTL_H 1
#endif
#ifndef __LA_FALLTHROUGH
#if defined(__GNUC__) && (__GNUC__ >= 7)
#define __LA_FALLTHROUGH __attribute__((fallthrough))
#else
#define __LA_FALLTHROUGH
#endif
#endif
#endif

#if defined(__linux__) || defined(__APPLE__)
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_SIZE_T 8
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define __LIBARCHIVE_CONFIG_H_INCLUDED 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCSCHR 1
#define HAVE_WCSDUP 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_MEMMOVE 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <wchar.h>
#ifndef HAVE_SYS_IOCTL_H
#define HAVE_SYS_IOCTL_H 1
#endif
#ifndef __LA_FALLTHROUGH
#if defined(__GNUC__) && (__GNUC__ >= 7)
#define __LA_FALLTHROUGH __attribute__((fallthrough))
#else
#define __LA_FALLTHROUGH
#endif
#endif
#endif

#if defined(__linux__) || defined(__APPLE__)
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_SIZE_T 8
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define __LIBARCHIVE_CONFIG_H_INCLUDED 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCSCHR 1
#define HAVE_WCSDUP 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_MEMMOVE 1
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <wchar.h>
#ifndef HAVE_SYS_IOCTL_H
#define HAVE_SYS_IOCTL_H 1
#endif
#ifndef __LA_FALLTHROUGH
#if defined(__GNUC__) && (__GNUC__ >= 7)
#define __LA_FALLTHROUGH __attribute__((fallthrough))
#else
#define __LA_FALLTHROUGH
#endif
#endif
#endif

#if defined(__linux__) || defined(__APPLE__)
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_SIZE_T 8
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define __LIBARCHIVE_CONFIG_H_INCLUDED 1
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <wchar.h>
#ifndef HAVE_SYS_IOCTL_H
#define HAVE_SYS_IOCTL_H 1
#endif
#ifndef __LA_FALLTHROUGH
#if defined(__GNUC__) && (__GNUC__ >= 7)
#define __LA_FALLTHROUGH __attribute__((fallthrough))
#else
#define __LA_FALLTHROUGH
#endif
#endif
#endif

#if defined(__linux__) || defined(__APPLE__)
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_SIZE_T 8
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <wchar.h>
#ifndef HAVE_SYS_IOCTL_H
#define HAVE_SYS_IOCTL_H 1
#endif
#ifndef __LA_FALLTHROUGH
#if defined(__GNUC__) && (__GNUC__ >= 7)
#define __LA_FALLTHROUGH __attribute__((fallthrough))
#else
#define __LA_FALLTHROUGH
#endif
#endif
#endif

#define ARCHIVE_PLATFORM_H_INCLUDED

/* archive.h and archive_entry.h require this.
 * Define `__LIBARCHIVE_BUILD` only when a real config/header is present
 * to avoid disabling parsing-time fallbacks in other headers. */
#if defined(PLATFORM_CONFIG_H) || defined(HAVE_CONFIG_H) ||                    \
    defined(__LIBARCHIVE_CONFIG_H_INCLUDED)
#define __LIBARCHIVE_BUILD 1
#endif

#if defined(PLATFORM_CONFIG_H)
/* Use hand-built config.h in environments that need it. */
#include PLATFORM_CONFIG_H
#elif defined(HAVE_CONFIG_H)
/* Most POSIX platforms use the 'configure' script to build config.h */
// #include "config.h"
#else
/* No config.h available. Provide a minimal parse-time fallback so editors
 * and clangd can parse this header without the project configure step.
 * This block is intentionally conservative and only affects editor
 * diagnostics; real builds should provide a proper config.h. */
#ifndef __LIBARCHIVE_PARSER_FALLBACK
#define __LIBARCHIVE_PARSER_FALLBACK 1
/* Minimal feature macros to keep downstream code happy during parsing. */
#ifndef HAVE_INTTYPES_H
#define HAVE_INTTYPES_H 1
#endif
#ifndef HAVE_STDINT_H
#define HAVE_STDINT_H 1
#endif
#ifndef SIZEOF_INT
#define SIZEOF_INT 4
#endif
#ifndef SIZEOF_LONG
#define SIZEOF_LONG 8
#endif
#endif /* __LIBARCHIVE_PARSER_FALLBACK */
#endif

/* On macOS check for some symbols based on the deployment target version.  */
#if defined(__APPLE__)
#undef HAVE_FUTIMENS
#undef HAVE_UTIMENSAT
#include <AvailabilityMacros.h>
#if MAC_OS_X_VERSION_MIN_REQUIRED >= 101300
#define HAVE_FUTIMENS 1
#define HAVE_UTIMENSAT 1
#endif
#endif

/* For cygwin, to avoid missing LONG, ULONG, PUCHAR, ... definitions */
#ifdef __CYGWIN__
#include <windef.h>
#endif

/* It should be possible to get rid of this by extending the feature-test
 * macros to cover Windows API functions, probably along with non-trivial
 * refactoring of code to find structures that sit more cleanly on top of
 * either Windows or Posix APIs. */
#if (defined(__WIN32__) || defined(_WIN32) || defined(__WIN32)) &&             \
    !defined(__CYGWIN__)
#include "archive_windows.h"
#if defined(LIBARCHIVE_ARCHIVE_WINDOWS_H_INCLUDED)
/* Keep archive_windows.h from being removed by include-cleaner when it's
 * actually included. */
static inline void __la_mark_archive_windows_used(void) { (void)sizeof(LONG); }
#endif
/* The C library on Windows specifies a calling convention for callback
 * functions and exports; when we interact with them (capture pointers,
 * call and pass function pointers) we need to match their calling
 * convention.
 * This only matters when libarchive is built with /Gr, /Gz or /Gv
 * (which change the default calling convention.) */
#define __LA_LIBC_CC __cdecl
#else
#define la_stat(path, stref) stat(path, stref)
#define __LA_LIBC_CC
#endif

/*
 * The config files define a lot of feature macros.  The following
 * uses those macros to select/define replacements and include key
 * headers as required.
 */

/* Try to get standard C99-style integer type definitions. */
#if HAVE_INTTYPES_H
#include <inttypes.h>
#endif
#if HAVE_STDINT_H
#include <stdint.h>
#endif

/* Borland warns about its own constants!  */
#if defined(__BORLANDC__)
#if HAVE_DECL_UINT64_MAX
#undef UINT64_MAX
#undef HAVE_DECL_UINT64_MAX
#endif
#if HAVE_DECL_UINT64_MIN
#undef UINT64_MIN
#undef HAVE_DECL_UINT64_MIN
#endif
#if HAVE_DECL_INT64_MAX
#undef INT64_MAX
#undef HAVE_DECL_INT64_MAX
#endif
#if HAVE_DECL_INT64_MIN
#undef INT64_MIN
#undef HAVE_DECL_INT64_MIN
#endif
#endif

/* Some platforms lack the standard *_MAX definitions. */
#if !HAVE_DECL_SIZE_MAX
#define SIZE_MAX (~(size_t)0)
#endif
#if !HAVE_DECL_SSIZE_MAX && !defined(SSIZE_MAX)
#define SSIZE_MAX ((ssize_t)(SIZE_MAX >> 1))
#endif
#if !HAVE_DECL_UINT32_MAX
#define UINT32_MAX (~(uint32_t)0)
#endif
#if !HAVE_DECL_INT32_MAX
#define INT32_MAX ((int32_t)(UINT32_MAX >> 1))
#endif
#if !HAVE_DECL_INT32_MIN
#define INT32_MIN ((int32_t)(~INT32_MAX))
#endif
#if !HAVE_DECL_UINT64_MAX
#define UINT64_MAX (~(uint64_t)0)
#endif
#if !HAVE_DECL_INT64_MAX
#define INT64_MAX ((int64_t)(UINT64_MAX >> 1))
#endif
#if !HAVE_DECL_INT64_MIN
#define INT64_MIN ((int64_t)(~INT64_MAX))
#endif
#if !HAVE_DECL_UINTMAX_MAX
#define UINTMAX_MAX (~(uintmax_t)0)
#endif
#if !HAVE_DECL_INTMAX_MAX
#define INTMAX_MAX ((intmax_t)(UINTMAX_MAX >> 1))
#endif
#if !HAVE_DECL_INTMAX_MIN
#define INTMAX_MIN ((intmax_t)(~INTMAX_MAX))
#endif

/* Some platforms lack the standard PRIxN/PRIdN definitions. */
#if !HAVE_INTTYPES_H || !defined(PRIx32) || !defined(PRId32)
#ifndef PRIx32
#if SIZEOF_INT == 4
#define PRIx32 "x"
#elif SIZEOF_LONG == 4
#define PRIx32 "lx"
#else
#error No suitable 32-bit unsigned integer type found for this platform
#endif
#endif // PRIx32
#ifndef PRId32
#if SIZEOF_INT == 4
#define PRId32 "d"
#elif SIZEOF_LONG == 4
#define PRId32 "ld"
#else
#error No suitable 32-bit signed integer type found for this platform
#endif
#endif // PRId32
#endif // !HAVE_INTTYPES_H || !defined(PRIx32) || !defined(PRId32)

/*
 * If we can't restore metadata using a file descriptor, then
 * for compatibility's sake, close files before trying to restore metadata.
 */
#if defined(HAVE_FCHMOD) || defined(HAVE_FUTIMES) ||                           \
    defined(HAVE_ACL_SET_FD) || defined(HAVE_ACL_SET_FD_NP) ||                 \
    defined(HAVE_FCHOWN)
#define CAN_RESTORE_METADATA_FD
#endif

/* Set up defaults for internal error codes. */
#ifndef ARCHIVE_ERRNO_FILE_FORMAT
#if HAVE_EFTYPE
#define ARCHIVE_ERRNO_FILE_FORMAT EFTYPE
#else
#if HAVE_EILSEQ
#define ARCHIVE_ERRNO_FILE_FORMAT EILSEQ
#else
#define ARCHIVE_ERRNO_FILE_FORMAT EINVAL
#endif
#endif
#endif

#ifndef ARCHIVE_ERRNO_PROGRAMMER
#define ARCHIVE_ERRNO_PROGRAMMER EINVAL
#endif

#ifndef ARCHIVE_ERRNO_MISC
#define ARCHIVE_ERRNO_MISC (-1)
#endif

#if defined(__linux__) || defined(__APPLE__)
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <wchar.h>
#ifndef HAVE_SYS_IOCTL_H
#define HAVE_SYS_IOCTL_H 1
#endif
#ifndef __LA_FALLTHROUGH
#if defined(__GNUC__) && (__GNUC__ >= 7)
#define __LA_FALLTHROUGH __attribute__((fallthrough))
#else
#define __LA_FALLTHROUGH
#endif
#endif
#endif

#ifndef __LA_DEPRECATED
#if defined(__GNUC__) &&                                                       \
    (__GNUC__ > 3 || (__GNUC__ == 3 && __GNUC_MINOR__ >= 1))
#define __LA_DEPRECATED __attribute__((deprecated))
#else
#define __LA_DEPRECATED
#endif
#endif
#ifndef __LA_DECL
#define __LA_DECL
#endif
#ifndef __LA_PRINTF
#define __LA_PRINTF(fmtarg, firstvararg)
#endif

#endif /* !ARCHIVE_PLATFORM_H_INCLUDED */
