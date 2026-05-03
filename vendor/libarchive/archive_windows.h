/*-
 * Copyright (c) 2009-2011 Michihiro NAKAJIMA
 * Copyright (c) 2003-2006 Tim Kientzle
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
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

#ifndef LIBARCHIVE_ARCHIVE_WINDOWS_H_INCLUDED
#define LIBARCHIVE_ARCHIVE_WINDOWS_H_INCLUDED

#ifndef __LIBARCHIVE_BUILD
#if defined(__has_include) && __has_include(<sys/types.h>)
#include <sys/types.h>
#endif

#if !defined(_ARCHIVE_SSIZE_T_DEFINED) && !defined(_SSIZE_T_) &&               \
    !defined(_SSIZE_T_DEFINED) && !defined(__ssize_t_defined)
typedef long ssize_t;
#define _ARCHIVE_SSIZE_T_DEFINED
#endif

#if !defined(_ARCHIVE_PID_T_DEFINED) && !defined(_PID_T) &&                    \
    !defined(_PID_T_DEFINED) && !defined(__pid_t_defined) &&                   \
    !defined(__MINGW32__) && !defined(__MINGW64__)

typedef int pid_t;
#define _ARCHIVE_PID_T_DEFINED
#endif
#endif

#ifndef MINGW_HAS_SECURE_API
#define MINGW_HAS_SECURE_API 1
#endif

#include <errno.h>
#define set_errno(val) ((errno) = val)
#include <io.h>
#include <stdlib.h>
#if defined(HAVE_STDINT_H)
#include <stdint.h>
#endif
#include <direct.h>
#include <fcntl.h>
#include <process.h>
#include <stdio.h>
#include <sys/stat.h>
#if defined(__MINGW32__) && defined(HAVE_UNISTD_H)

#include <unistd.h>
#endif
#define NOCRYPT
#if defined(__LIBARCHIVE_BUILD)

#include <windows.h>
#if defined(__has_include)
#if __has_include(<wincrypt.h>)
#include <wincrypt.h>
#endif
#else
#include <wincrypt.h>
#endif
#else

#ifndef HANDLE
typedef void *HANDLE;
#endif
#ifndef BOOL
typedef int BOOL;
#endif
#ifndef DWORD
typedef unsigned long DWORD;
#endif
#ifndef LPCWSTR
typedef const wchar_t *LPCWSTR;
#endif
#ifndef LPWSTR
typedef wchar_t *LPWSTR;
#endif
#ifndef WINBASEAPI
#define WINBASEAPI
#endif
#ifndef WINAPI
#define WINAPI
#endif
#endif

#include "archive_platform_stat.h"

typedef unsigned int id_t;

#if defined(__BORLANDC__)
#pragma warn - 8068
#pragma warn - 8072
#endif

#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void *)0)
#endif
#endif

#define close _close
#define fcntl(fd, cmd, flg)
#ifndef fileno
#define fileno _fileno
#endif
#ifdef fstat
#undef fstat
#endif
#define fstat __la_fstat
#if !defined(__BORLANDC__)
#ifdef lseek
#undef lseek
#endif
#define lseek _lseeki64
#else
#define lseek __la_lseek
#define __LA_LSEEK_NEEDED
#endif
#define lstat __la_stat
#define open __la_open
#define _wopen __la_wopen
#define read __la_read
#if !defined(__BORLANDC__) && !defined(__WATCOMC__)
#define setmode _setmode
#endif
#define la_stat(path, stref) __la_stat(path, stref)
#if !defined(__WATCOMC__)
#if !defined(__BORLANDC__)
#define strdup _strdup
#endif
#define tzset _tzset
#if !defined(__BORLANDC__)
#define umask _umask
#endif
#endif
#define waitpid __la_waitpid
#define write __la_write

#if !defined(__WATCOMC__)

#ifndef O_RDONLY
#define O_RDONLY _O_RDONLY
#define O_WRONLY _O_WRONLY
#define O_TRUNC _O_TRUNC
#define O_CREAT _O_CREAT
#define O_EXCL _O_EXCL
#define O_BINARY _O_BINARY
#endif

#ifndef _S_IFIFO
#define _S_IFIFO 0010000
#endif
#ifndef _S_IFCHR
#define _S_IFCHR 0020000
#endif
#ifndef _S_IFDIR
#define _S_IFDIR 0040000
#endif
#ifndef _S_IFBLK
#define _S_IFBLK 0060000
#endif
#ifndef _S_IFLNK
#define _S_IFLNK 0120000
#endif
#ifndef _S_IFSOCK
#define _S_IFSOCK 0140000
#endif
#ifndef _S_IFREG
#define _S_IFREG 0100000
#endif
#ifndef _S_IFMT
#define _S_IFMT 0170000
#endif

#ifndef S_IFIFO
#define S_IFIFO _S_IFIFO
#endif

#ifndef S_IFBLK
#define S_IFBLK _S_IFBLK
#endif
#ifndef S_IFLNK
#define S_IFLNK _S_IFLNK
#endif
#ifndef S_IFSOCK
#define S_IFSOCK _S_IFSOCK
#endif

#ifndef S_ISBLK
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

#define _S_ISUID 0004000
#define _S_ISGID 0002000
#define _S_ISVTX 0001000

#define S_ISUID _S_ISUID
#define S_ISGID _S_ISGID
#define S_ISVTX _S_ISVTX

#define _S_IRWXU (_S_IREAD | _S_IWRITE | _S_IEXEC)
#define _S_IXUSR _S_IEXEC
#define _S_IWUSR _S_IWRITE
#define _S_IRUSR _S_IREAD
#define _S_IRWXG (_S_IRWXU >> 3)
#define _S_IXGRP (_S_IXUSR >> 3)
#define _S_IWGRP (_S_IWUSR >> 3)
#define _S_IRGRP (_S_IRUSR >> 3)
#define _S_IRWXO (_S_IRWXG >> 3)
#define _S_IXOTH (_S_IXGRP >> 3)
#define _S_IWOTH (_S_IWGRP >> 3)
#define _S_IROTH (_S_IRGRP >> 3)

#ifndef S_IRWXU
#define S_IRWXU _S_IRWXU
#define S_IXUSR _S_IXUSR
#define S_IWUSR _S_IWUSR
#define S_IRUSR _S_IRUSR
#endif
#ifndef S_IRWXG
#define S_IRWXG _S_IRWXG
#define S_IXGRP _S_IXGRP
#define S_IWGRP _S_IWGRP
#endif
#ifndef S_IRGRP
#define S_IRGRP _S_IRGRP
#endif
#ifndef S_IRWXO
#define S_IRWXO _S_IRWXO
#define S_IXOTH _S_IXOTH
#define S_IWOTH _S_IWOTH
#define S_IROTH _S_IROTH
#endif

#endif

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETOWN 5
#define F_SETOWN 6
#define F_GETLK 7
#define F_SETLK 8
#define F_SETLKW 9

#define F_GETLK64 7
#define F_SETLK64 8
#define F_SETLKW64 9

#define FD_CLOEXEC 1

#define O_NONBLOCK 0x0004

#if !defined(F_OK)
#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0
#endif

int __la_seek_fstat(int fd, la_seek_stat_t *st);
int __la_seek_stat(const char *path, la_seek_stat_t *st);

extern int __la_fstat(int fd, struct stat *st);
extern int __la_lstat(const char *path, struct stat *st);
#if defined(__LA_LSEEK_NEEDED)
extern __int64 __la_lseek(int fd, __int64 offset, int whence);
#endif
extern int __la_open(const char *path, int flags, ...);
extern int __la_wopen(const wchar_t *path, int flags, ...);
extern ssize_t __la_read(int fd, void *buf, size_t nbytes);
extern int __la_stat(const char *path, struct stat *st);
extern pid_t __la_waitpid(HANDLE child, int *status, int option);
extern ssize_t __la_write(int fd, const void *buf, size_t nbytes);

#define _stat64i32(path, st) __la_stat(path, st)
#define _stat64(path, st) __la_stat(path, st)

#define WIFEXITED(sts) ((sts & 0x100) == 0)
#define WEXITSTATUS(sts) (sts & 0x0FF)

extern wchar_t *__la_win_permissive_name(const char *name);
extern wchar_t *__la_win_permissive_name_w(const wchar_t *wname);
extern void __la_dosmaperr(unsigned long e);
#define la_dosmaperr(e) __la_dosmaperr(e)
extern struct archive_entry *
__la_win_entry_in_posix_pathseparator(struct archive_entry *);

#if defined(HAVE_WCRTOMB) && defined(__BORLANDC__)
typedef int mbstate_t;
size_t wcrtomb(char *, wchar_t, mbstate_t *);
#endif

#if defined(WINAPI_FAMILY_PARTITION) && defined(NTDDI_VERSION)

#if !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP) &&                      \
    (NTDDI_VERSION < NTDDI_WIN10_VB)

#define GetVolumePathNameW(f, v, c) (0)
#endif
#elif defined(_MSC_VER) && _MSC_VER < 1300
WINBASEAPI BOOL WINAPI GetVolumePathNameW(LPCWSTR lpszFileName,
                                          LPWSTR lpszVolumePathName,
                                          DWORD cchBufferLength);
#endif
#if defined(_MSC_VER) && _MSC_VER < 1300
#if _WIN32_WINNT < 0x0500
typedef struct _FILE_ALLOCATED_RANGE_BUFFER {
  LARGE_INTEGER FileOffset;
  LARGE_INTEGER Length;
} FILE_ALLOCATED_RANGE_BUFFER, *PFILE_ALLOCATED_RANGE_BUFFER;
#define FSCTL_SET_SPARSE                                                       \
  CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 49, METHOD_BUFFERED, FILE_WRITE_DATA)
#define FSCTL_QUERY_ALLOCATED_RANGES                                           \
  CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 51, METHOD_NEITHER, FILE_READ_DATA)
#endif
#endif

#endif
