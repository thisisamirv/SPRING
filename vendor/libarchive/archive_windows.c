/*-
 * Copyright (c) 2009-2011 Michihiro NAKAJIMA
 * Copyright (c) 2003-2007 Kees Zeelenberg
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

#if defined(_WIN32) && !defined(__CYGWIN__)

#include "archive_entry.h"

#ifndef ARCHIVE_ENTRY_H_INCLUDED
#error "archive_entry.h must be included"
#endif

#include "archive_platform.h"

#ifndef ARCHIVE_PLATFORM_H_INCLUDED
#error "archive_platform.h must be included first"
#endif

#include "archive_platform_stat.h"

#ifndef ARCHIVE_PLATFORM_STAT_H_INCLUDED
#error "archive_platform_stat.h must be included"
#endif

#include "archive_private.h"

#ifndef ARCHIVE_PRIVATE_H_INCLUDED
#error "archive_private.h must be included"
#endif

#include "archive_time_private.h"

#ifndef ARCHIVE_TIME_PRIVATE_H_INCLUDED
#error "archive_time_private.h must be included"
#endif
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#ifdef HAVE_SYS_UTIME_H
#include <sys/utime.h>
#endif
#include <process.h>
#include <share.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <wchar.h>
#include <windows.h>

#if defined(__LA_LSEEK_NEEDED)
static BOOL SetFilePointerEx_perso(HANDLE hFile, LARGE_INTEGER liDistanceToMove,
                                   PLARGE_INTEGER lpNewFilePointer,
                                   DWORD dwMoveMethod) {
  LARGE_INTEGER li;
  li.QuadPart = liDistanceToMove.QuadPart;
  li.LowPart = SetFilePointer(hFile, li.LowPart, &li.HighPart, dwMoveMethod);
  if (lpNewFilePointer) {
    lpNewFilePointer->QuadPart = li.QuadPart;
  }
  return li.LowPart != -1 || GetLastError() == NO_ERROR;
}
#endif

struct ustat {
  int64_t st_atime;
  uint32_t st_atime_nsec;
  int64_t st_ctime;
  uint32_t st_ctime_nsec;
  int64_t st_mtime;
  uint32_t st_mtime_nsec;
  gid_t st_gid;

  int64_t st_ino;
  mode_t st_mode;
  uint32_t st_nlink;
  uint64_t st_size;
  uid_t st_uid;
  dev_t st_dev;
  dev_t st_rdev;
};

#define INOSIZE (8 * sizeof(ino_t))
static __inline ino_t getino(struct ustat *ub) {
  ULARGE_INTEGER ino64;
  ino64.QuadPart = ub->st_ino;

  return ((ino_t)(ino64.LowPart ^ (ino64.LowPart >> INOSIZE)));
}

wchar_t *__la_win_permissive_name(const char *name) {
  wchar_t *wn;
  wchar_t *ws;
  size_t ll;

  ll = strlen(name);
  wn = malloc((ll + 1) * sizeof(wchar_t));
  if (wn == NULL)
    return (NULL);
  ll = mbstowcs(wn, name, ll);
  if (ll == (size_t)-1) {
    free(wn);
    return (NULL);
  }
  wn[ll] = L'\0';
  ws = __la_win_permissive_name_w(wn);
  free(wn);
  return (ws);
}

wchar_t *__la_win_permissive_name_w(const wchar_t *wname) {
  wchar_t *wn, *wnp;
  wchar_t *ws, *wsp;
  DWORD l, len, slen;
  int unc;

  l = GetFullPathNameW(wname, 0, NULL, NULL);
  if (l == 0)
    return (NULL);

  l += 3;
  wnp = malloc(l * sizeof(wchar_t));
  if (wnp == NULL)
    return (NULL);
  len = GetFullPathNameW(wname, l, wnp, NULL);
  wn = wnp;

  if (wnp[0] == L'\\' && wnp[1] == L'\\' && wnp[2] == L'?' && wnp[3] == L'\\')

    return (wn);

  if (wnp[0] == L'\\' && wnp[1] == L'\\' && wnp[2] == L'.' && wnp[3] == L'\\') {

    if (((wnp[4] >= L'a' && wnp[4] <= L'z') ||
         (wnp[4] >= L'A' && wnp[4] <= L'Z')) &&
        wnp[5] == L':' && wnp[6] == L'\\')
      wnp[2] = L'?';
    return (wn);
  }

  unc = 0;
  if (wnp[0] == L'\\' && wnp[1] == L'\\' && wnp[2] != L'\\') {
    wchar_t *p = &wnp[2];

    while (*p != L'\\' && *p != L'\0')
      ++p;
    if (*p == L'\\') {
      wchar_t *rp = ++p;

      while (*p != L'\\' && *p != L'\0')
        ++p;
      if (*p == L'\\' && p != rp) {

        wnp += 2;
        len -= 2;
        unc = 1;
      }
    }
  }

  slen = 4 + (unc * 4) + len + 1;
  ws = wsp = malloc(slen * sizeof(wchar_t));
  if (ws == NULL) {
    free(wn);
    return (NULL);
  }

  wcsncpy(wsp, L"\\\\?\\", 4);
  wsp += 4;
  slen -= 4;
  if (unc) {

    wcsncpy(wsp, L"UNC\\", 4);
    wsp += 4;
    slen -= 4;
  }
  wcsncpy(wsp, wnp, slen);
  wsp[slen - 1] = L'\0';
  free(wn);
  return (ws);
}

static HANDLE la_CreateFile(const char *path, DWORD dwDesiredAccess,
                            DWORD dwShareMode,
                            LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                            DWORD dwCreationDisposition,
                            DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
  wchar_t *wpath;
  HANDLE handle;
#if _WIN32_WINNT >= 0x0602
  CREATEFILE2_EXTENDED_PARAMETERS createExParams;
#endif

#if !defined(WINAPI_FAMILY_PARTITION) ||                                       \
    WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
  handle =
      CreateFileA(path, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                  dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
  if (handle != INVALID_HANDLE_VALUE)
    return (handle);
  if (GetLastError() != ERROR_PATH_NOT_FOUND)
    return (handle);
#endif
  wpath = __la_win_permissive_name(path);
  if (wpath == NULL)
    return INVALID_HANDLE_VALUE;
#if _WIN32_WINNT >= 0x0602
  ZeroMemory(&createExParams, sizeof(createExParams));
  createExParams.dwSize = sizeof(createExParams);
  createExParams.dwFileAttributes = dwFlagsAndAttributes & 0xFFFF;
  createExParams.dwFileFlags = dwFlagsAndAttributes & 0xFFF00000;
  createExParams.dwSecurityQosFlags = dwFlagsAndAttributes & 0x000F0000;
  createExParams.lpSecurityAttributes = lpSecurityAttributes;
  createExParams.hTemplateFile = hTemplateFile;
  handle = CreateFile2(wpath, dwDesiredAccess, dwShareMode,
                       dwCreationDisposition, &createExParams);
#else
  handle =
      CreateFileW(wpath, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                  dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
#endif
  free(wpath);
  return (handle);
}

#if defined(__LA_LSEEK_NEEDED)
__int64 __la_lseek(int fd, __int64 offset, int whence) {
  LARGE_INTEGER distance;
  LARGE_INTEGER newpointer;
  HANDLE handle;

  if (fd < 0) {
    errno = EBADF;
    return (-1);
  }
  handle = (HANDLE)_get_osfhandle(fd);
  if (GetFileType(handle) != FILE_TYPE_DISK) {
    errno = EBADF;
    return (-1);
  }
  distance.QuadPart = offset;
  if (!SetFilePointerEx_perso(handle, distance, &newpointer, whence)) {
    DWORD lasterr;

    lasterr = GetLastError();
    if (lasterr == ERROR_BROKEN_PIPE)
      return (0);
    if (lasterr == ERROR_ACCESS_DENIED)
      errno = EBADF;
    else
      la_dosmaperr(lasterr);
    return (-1);
  }
  return (newpointer.QuadPart);
}
#endif

int __la_open(const char *path, int flags, ...) {
  va_list ap;
  wchar_t *ws;
  int r, pmode;
  DWORD attr;

  va_start(ap, flags);
  pmode = va_arg(ap, int);
  va_end(ap);
  ws = NULL;

  pmode = pmode & (_S_IREAD | _S_IWRITE);

  if ((flags & ~O_BINARY) == O_RDONLY) {

    attr = GetFileAttributesA(path);
#if !defined(WINAPI_FAMILY_PARTITION) ||                                       \
    WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    if (attr == (DWORD)-1 && GetLastError() == ERROR_PATH_NOT_FOUND)
#endif
    {
      ws = __la_win_permissive_name(path);
      if (ws == NULL) {
        errno = EINVAL;
        return (-1);
      }
      attr = GetFileAttributesW(ws);
    }
    if (attr == (DWORD)-1) {
      la_dosmaperr(GetLastError());
      free(ws);
      return (-1);
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
      HANDLE handle;
#if !defined(WINAPI_FAMILY_PARTITION) ||                                       \
    WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
      if (ws != NULL)
        handle = CreateFileW(
            ws, 0, 0, NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_ATTRIBUTE_READONLY, NULL);
      else
        handle = CreateFileA(
            path, 0, 0, NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_ATTRIBUTE_READONLY, NULL);
#else
      CREATEFILE2_EXTENDED_PARAMETERS createExParams;
      ZeroMemory(&createExParams, sizeof(createExParams));
      createExParams.dwSize = sizeof(createExParams);
      createExParams.dwFileAttributes = FILE_ATTRIBUTE_READONLY;
      createExParams.dwFileFlags = FILE_FLAG_BACKUP_SEMANTICS;
      handle = CreateFile2(ws, 0, 0, OPEN_EXISTING, &createExParams);
#endif
      free(ws);
      if (handle == INVALID_HANDLE_VALUE) {
        la_dosmaperr(GetLastError());
        return (-1);
      }
      r = _open_osfhandle((intptr_t)handle, _O_RDONLY);
      return (r);
    }
  }
  if (ws == NULL) {
#if defined(__BORLANDC__)

    r = _open(path, flags);
#else
    _sopen_s(&r, path, flags, _SH_DENYNO, pmode);
#endif
    if (r < 0 && errno == EACCES && (flags & O_CREAT) != 0) {

      attr = GetFileAttributesA(path);
      if (attr == (DWORD)-1)
        la_dosmaperr(GetLastError());
      else if (attr & FILE_ATTRIBUTE_DIRECTORY)
        errno = EISDIR;
      else
        errno = EACCES;
      return (-1);
    }
    if (r >= 0 || errno != ENOENT)
      return (r);
    ws = __la_win_permissive_name(path);
    if (ws == NULL) {
      errno = EINVAL;
      return (-1);
    }
  }
  _wsopen_s(&r, ws, flags, _SH_DENYNO, pmode);
  if (r < 0 && errno == EACCES && (flags & O_CREAT) != 0) {

    attr = GetFileAttributesW(ws);
    if (attr == (DWORD)-1)
      la_dosmaperr(GetLastError());
    else if (attr & FILE_ATTRIBUTE_DIRECTORY)
      errno = EISDIR;
    else
      errno = EACCES;
  }
  free(ws);
  return (r);
}

int __la_wopen(const wchar_t *path, int flags, ...) {
  va_list ap;
  wchar_t *fullpath;
  int r, pmode;
  DWORD attr;

  va_start(ap, flags);
  pmode = va_arg(ap, int);
  va_end(ap);
  fullpath = NULL;

  pmode = pmode & (_S_IREAD | _S_IWRITE);

  if ((flags & ~O_BINARY) == O_RDONLY) {

    attr = GetFileAttributesW(path);
#if !defined(WINAPI_FAMILY_PARTITION) ||                                       \
    WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    if (attr == (DWORD)-1 && GetLastError() == ERROR_PATH_NOT_FOUND)
#endif
    {
      fullpath = __la_win_permissive_name_w(path);
      if (fullpath == NULL) {
        errno = EINVAL;
        return (-1);
      }
      path = fullpath;
      attr = GetFileAttributesW(fullpath);
    }
    if (attr == (DWORD)-1) {
      la_dosmaperr(GetLastError());
      free(fullpath);
      return (-1);
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
      HANDLE handle;
#if !defined(WINAPI_FAMILY_PARTITION) ||                                       \
    WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
      if (fullpath != NULL)
        handle = CreateFileW(
            fullpath, 0, 0, NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_ATTRIBUTE_READONLY, NULL);
      else
        handle = CreateFileW(
            path, 0, 0, NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_ATTRIBUTE_READONLY, NULL);
#else
      CREATEFILE2_EXTENDED_PARAMETERS createExParams;
      ZeroMemory(&createExParams, sizeof(createExParams));
      createExParams.dwSize = sizeof(createExParams);
      createExParams.dwFileAttributes = FILE_ATTRIBUTE_READONLY;
      createExParams.dwFileFlags = FILE_FLAG_BACKUP_SEMANTICS;
      handle = CreateFile2(fullpath, 0, 0, OPEN_EXISTING, &createExParams);
#endif
      free(fullpath);
      if (handle == INVALID_HANDLE_VALUE) {
        la_dosmaperr(GetLastError());
        return (-1);
      }
      r = _open_osfhandle((intptr_t)handle, _O_RDONLY);
      return (r);
    }
  }
  _wsopen_s(&r, path, flags, _SH_DENYNO, pmode);
  if (r < 0 && errno == EACCES && (flags & O_CREAT) != 0) {

    attr = GetFileAttributesW(path);
    if (attr == (DWORD)-1)
      la_dosmaperr(GetLastError());
    else if (attr & FILE_ATTRIBUTE_DIRECTORY)
      errno = EISDIR;
    else
      errno = EACCES;
  }
  free(fullpath);
  return (r);
}

ssize_t __la_read(int fd, void *buf, size_t nbytes) {
  HANDLE handle;
  DWORD bytes_read, lasterr;
  int r;

#ifdef _WIN64
  if (nbytes > UINT32_MAX)
    nbytes = UINT32_MAX;
#endif
  if (fd < 0) {
    errno = EBADF;
    return (-1);
  }

  if (nbytes == 0)
    return (0);
  handle = (HANDLE)_get_osfhandle(fd);
  r = ReadFile(handle, buf, (uint32_t)nbytes, &bytes_read, NULL);
  if (r == 0) {
    lasterr = GetLastError();
    if (lasterr == ERROR_NO_DATA) {
      errno = EAGAIN;
      return (-1);
    }
    if (lasterr == ERROR_BROKEN_PIPE)
      return (0);
    if (lasterr == ERROR_ACCESS_DENIED)
      errno = EBADF;
    else
      la_dosmaperr(lasterr);
    return (-1);
  }
  return ((ssize_t)bytes_read);
}

static int __hstat(HANDLE handle, struct ustat *st) {
  BY_HANDLE_FILE_INFORMATION info;
  ULARGE_INTEGER ino64;
  DWORD ftype;
  mode_t mode;

  switch (ftype = GetFileType(handle)) {
  case FILE_TYPE_UNKNOWN:
    errno = EBADF;
    return (-1);
  case FILE_TYPE_CHAR:
  case FILE_TYPE_PIPE:
    if (ftype == FILE_TYPE_CHAR) {
      st->st_mode = S_IFCHR;
      st->st_size = 0;
    } else {
      DWORD avail;

      st->st_mode = S_IFIFO;
      if (PeekNamedPipe(handle, NULL, 0, NULL, &avail, NULL))
        st->st_size = avail;
      else
        st->st_size = 0;
    }
    st->st_atime = 0;
    st->st_atime_nsec = 0;
    st->st_mtime = 0;
    st->st_mtime_nsec = 0;
    st->st_ctime = 0;
    st->st_ctime_nsec = 0;
    st->st_ino = 0;
    st->st_nlink = 1;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_rdev = 0;
    st->st_dev = 0;
    return (0);
  case FILE_TYPE_DISK:
    break;
  default:

    la_dosmaperr(GetLastError());
    return (-1);
  }

  ZeroMemory(&info, sizeof(info));
  if (!GetFileInformationByHandle(handle, &info)) {
    la_dosmaperr(GetLastError());
    return (-1);
  }

  mode = S_IRUSR | S_IRGRP | S_IROTH;
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) == 0)
    mode |= S_IWUSR | S_IWGRP | S_IWOTH;
  if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    mode |= S_IFDIR | S_IXUSR | S_IXGRP | S_IXOTH;
  else
    mode |= S_IFREG;
  st->st_mode = mode;

  ntfs_to_unix(FILETIME_to_ntfs(&info.ftLastAccessTime), &st->st_atime,
               &st->st_atime_nsec);
  ntfs_to_unix(FILETIME_to_ntfs(&info.ftLastWriteTime), &st->st_mtime,
               &st->st_mtime_nsec);
  ntfs_to_unix(FILETIME_to_ntfs(&info.ftCreationTime), &st->st_ctime,
               &st->st_ctime_nsec);
  st->st_size = ((int64_t)(info.nFileSizeHigh) * ((int64_t)MAXDWORD + 1)) +
                (int64_t)(info.nFileSizeLow);
#ifdef SIMULATE_WIN_STAT
  st->st_ino = 0;
  st->st_nlink = 1;
  st->st_dev = 0;
#else

  ino64.HighPart = info.nFileIndexHigh & 0x0000FFFFUL;
  ino64.LowPart = info.nFileIndexLow;
  st->st_ino = ino64.QuadPart;
  st->st_nlink = info.nNumberOfLinks;
  if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    ++st->st_nlink;
  st->st_dev = info.dwVolumeSerialNumber;
#endif
  st->st_uid = 0;
  st->st_gid = 0;
  st->st_rdev = 0;
  return (0);
}

static void copy_stat(struct stat *st, struct ustat *us) {
  st->st_atime = us->st_atime;
  st->st_ctime = us->st_ctime;
  st->st_mtime = us->st_mtime;
  st->st_gid = us->st_gid;
  st->st_ino = getino(us);
  st->st_mode = us->st_mode;
  st->st_nlink = us->st_nlink;
  st->st_size = (off_t)us->st_size;
  if (st->st_size < 0 || (uint64_t)st->st_size != us->st_size)
    st->st_size = 0;
  st->st_uid = us->st_uid;
  st->st_dev = us->st_dev;
  st->st_rdev = us->st_rdev;
}

int __la_fstat(int fd, struct stat *st) {
  struct ustat u;
  int ret;

  if (fd < 0) {
    errno = EBADF;
    return (-1);
  }
  ret = __hstat((HANDLE)_get_osfhandle(fd), &u);
  if (ret >= 0) {
    copy_stat(st, &u);
    if (u.st_mode & (S_IFCHR | S_IFIFO)) {
      st->st_dev = fd;
      st->st_rdev = fd;
    }
  }
  return (ret);
}

int __la_stat(const char *path, struct stat *st) {
  HANDLE handle;
  struct ustat u;
  int ret;

  handle = la_CreateFile(path, 0, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    la_dosmaperr(GetLastError());
    return (-1);
  }
  ret = __hstat(handle, &u);
  CloseHandle(handle);
  if (ret >= 0) {
    char *p;

    copy_stat(st, &u);
    p = strrchr(path, '.');
    if (p != NULL && strlen(p) == 4) {
      char exttype[4];

      ++p;
      exttype[0] = toupper(*p++);
      exttype[1] = toupper(*p++);
      exttype[2] = toupper(*p++);
      exttype[3] = '\0';
      if (!strcmp(exttype, "EXE") || !strcmp(exttype, "CMD") ||
          !strcmp(exttype, "BAT") || !strcmp(exttype, "COM"))
        st->st_mode |= S_IXUSR | S_IXGRP | S_IXOTH;
    }
  }
  return (ret);
}

static void copy_seek_stat(la_seek_stat_t *st, struct ustat *us) {
  st->st_mtime = us->st_mtime;
  st->st_gid = us->st_gid;
  st->st_ino = getino(us);
  st->st_mode = us->st_mode;
  st->st_nlink = us->st_nlink;
  st->st_size = (la_seek_t)us->st_size;
  if (st->st_size < 0 || (uint64_t)st->st_size != us->st_size)
    st->st_size = -1;
  st->st_uid = us->st_uid;
  st->st_dev = us->st_dev;
  st->st_rdev = us->st_rdev;
}

int __la_seek_fstat(int fd, la_seek_stat_t *st) {
  struct ustat u;
  int ret;

  ret = __hstat((HANDLE)_get_osfhandle(fd), &u);
  if (ret >= 0)
    copy_seek_stat(st, &u);
  return (ret);
}

int __la_seek_stat(const char *path, la_seek_stat_t *st) {
  HANDLE handle;
  struct ustat u;
  int ret;

  handle = la_CreateFile(path, 0, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    la_dosmaperr(GetLastError());
    return (-1);
  }
  ret = __hstat(handle, &u);
  CloseHandle(handle);
  copy_seek_stat(st, &u);
  return (ret);
}

pid_t __la_waitpid(HANDLE child, int *status, int option) {
  DWORD cs;

  (void)option;
  do {
    if (GetExitCodeProcess(child, &cs) == 0) {
      la_dosmaperr(GetLastError());
      CloseHandle(child);
      *status = 0;
      return (-1);
    }
  } while (cs == STILL_ACTIVE);

  CloseHandle(child);
  *status = (int)(cs & 0xff);
  return (0);
}

ssize_t __la_write(int fd, const void *buf, size_t nbytes) {
  DWORD bytes_written;

#ifdef _WIN64
  if (nbytes > UINT32_MAX)
    nbytes = UINT32_MAX;
#endif
  if (fd < 0) {
    errno = EBADF;
    return (-1);
  }
  if (!WriteFile((HANDLE)_get_osfhandle(fd), buf, (uint32_t)nbytes,
                 &bytes_written, NULL)) {
    DWORD lasterr;

    lasterr = GetLastError();
    if (lasterr == ERROR_ACCESS_DENIED)
      errno = EBADF;
    else
      la_dosmaperr(lasterr);
    return (-1);
  }
  return (bytes_written);
}

static int replace_pathseparator(struct archive_wstring *ws,
                                 const wchar_t *wp) {
  wchar_t *w;
  size_t path_length;

  if (wp == NULL)
    return (0);
  if (wcschr(wp, L'\\') == NULL)
    return (0);
  path_length = wcslen(wp);
  if (archive_wstring_ensure(ws, path_length) == NULL)
    return (-1);
  archive_wstrncpy(ws, wp, path_length);
  for (w = ws->s; *w; w++) {
    if (*w == L'\\')
      *w = L'/';
  }
  return (1);
}

static int fix_pathseparator(struct archive_entry *entry) {
  struct archive_wstring ws;
  const wchar_t *wp;
  int ret = ARCHIVE_OK;

  archive_string_init(&ws);
  wp = archive_entry_pathname_w(entry);
  switch (replace_pathseparator(&ws, wp)) {
  case 0:
    break;
  case 1:
    archive_entry_copy_pathname_w(entry, ws.s);
    break;
  default:
    ret = ARCHIVE_FAILED;
  }
  wp = archive_entry_hardlink_w(entry);
  switch (replace_pathseparator(&ws, wp)) {
  case 0:
    break;
  case 1:
    archive_entry_copy_hardlink_w(entry, ws.s);
    break;
  default:
    ret = ARCHIVE_FAILED;
  }
  wp = archive_entry_symlink_w(entry);
  switch (replace_pathseparator(&ws, wp)) {
  case 0:
    break;
  case 1:
    archive_entry_copy_symlink_w(entry, ws.s);
    break;
  default:
    ret = ARCHIVE_FAILED;
  }
  archive_wstring_free(&ws);
  return (ret);
}

struct archive_entry *
__la_win_entry_in_posix_pathseparator(struct archive_entry *entry) {
  struct archive_entry *entry_main;
  const wchar_t *wp;
  int has_backslash = 0;
  int ret;

  wp = archive_entry_pathname_w(entry);
  if (wp != NULL && wcschr(wp, L'\\') != NULL)
    has_backslash = 1;
  if (!has_backslash) {
    wp = archive_entry_hardlink_w(entry);
    if (wp != NULL && wcschr(wp, L'\\') != NULL)
      has_backslash = 1;
  }
  if (!has_backslash) {
    wp = archive_entry_symlink_w(entry);
    if (wp != NULL && wcschr(wp, L'\\') != NULL)
      has_backslash = 1;
  }

  if (!has_backslash)
    return (entry);

  entry_main = archive_entry_clone(entry);
  if (entry_main == NULL)
    return (NULL);

  ret = fix_pathseparator(entry_main);
  if (ret < ARCHIVE_WARN) {
    archive_entry_free(entry_main);
    return (NULL);
  }
  return (entry_main);
}

static const struct {
  DWORD winerr;
  int doserr;
} doserrors[] = {{ERROR_INVALID_FUNCTION, EINVAL},
                 {ERROR_FILE_NOT_FOUND, ENOENT},
                 {ERROR_PATH_NOT_FOUND, ENOENT},
                 {ERROR_TOO_MANY_OPEN_FILES, EMFILE},
                 {ERROR_ACCESS_DENIED, EACCES},
                 {ERROR_INVALID_HANDLE, EBADF},
                 {ERROR_ARENA_TRASHED, ENOMEM},
                 {ERROR_NOT_ENOUGH_MEMORY, ENOMEM},
                 {ERROR_INVALID_BLOCK, ENOMEM},
                 {ERROR_BAD_ENVIRONMENT, E2BIG},
                 {ERROR_BAD_FORMAT, ENOEXEC},
                 {ERROR_INVALID_ACCESS, EINVAL},
                 {ERROR_INVALID_DATA, EINVAL},
                 {ERROR_INVALID_DRIVE, ENOENT},
                 {ERROR_CURRENT_DIRECTORY, EACCES},
                 {ERROR_NOT_SAME_DEVICE, EXDEV},
                 {ERROR_NO_MORE_FILES, ENOENT},
                 {ERROR_LOCK_VIOLATION, EACCES},
                 {ERROR_SHARING_VIOLATION, EACCES},
                 {ERROR_BAD_NETPATH, ENOENT},
                 {ERROR_NETWORK_ACCESS_DENIED, EACCES},
                 {ERROR_BAD_NET_NAME, ENOENT},
                 {ERROR_FILE_EXISTS, EEXIST},
                 {ERROR_CANNOT_MAKE, EACCES},
                 {ERROR_FAIL_I24, EACCES},
                 {ERROR_INVALID_PARAMETER, EINVAL},
                 {ERROR_NO_PROC_SLOTS, EAGAIN},
                 {ERROR_DRIVE_LOCKED, EACCES},
                 {ERROR_BROKEN_PIPE, EPIPE},
                 {ERROR_DISK_FULL, ENOSPC},
                 {ERROR_INVALID_TARGET_HANDLE, EBADF},
                 {ERROR_INVALID_HANDLE, EINVAL},
                 {ERROR_WAIT_NO_CHILDREN, ECHILD},
                 {ERROR_CHILD_NOT_COMPLETE, ECHILD},
                 {ERROR_DIRECT_ACCESS_HANDLE, EBADF},
                 {ERROR_NEGATIVE_SEEK, EINVAL},
                 {ERROR_SEEK_ON_DEVICE, EACCES},
                 {ERROR_DIR_NOT_EMPTY, ENOTEMPTY},
                 {ERROR_NOT_LOCKED, EACCES},
                 {ERROR_BAD_PATHNAME, ENOENT},
                 {ERROR_MAX_THRDS_REACHED, EAGAIN},
                 {ERROR_LOCK_FAILED, EACCES},
                 {ERROR_ALREADY_EXISTS, EEXIST},
                 {ERROR_FILENAME_EXCED_RANGE, ENOENT},
                 {ERROR_NESTING_NOT_ALLOWED, EAGAIN},
                 {ERROR_NOT_ENOUGH_QUOTA, ENOMEM}};

void __la_dosmaperr(unsigned long e) {
  int i;

  if (e == 0) {
    errno = 0;
    return;
  }

  for (i = 0; i < (int)(sizeof(doserrors) / sizeof(doserrors[0])); i++) {
    if (doserrors[i].winerr == e) {
      errno = doserrors[i].doserr;
      return;
    }
  }

  errno = EINVAL;
  return;
}

#endif
