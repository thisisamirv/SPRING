/*-
 * Copyright (c) 2003-2010 Tim Kientzle
 * Copyright (c) 2011-2012 Michihiro NAKAJIMA
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

#include "archive_platform.h"

#ifndef ARCHIVE_PLATFORM_H_INCLUDED
#error "archive_platform.h must be included first"
#endif

#if defined(_WIN32) && !defined(__CYGWIN__)

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_UTIME_H
#include <sys/utime.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <winioctl.h>

#include "archive.h"

#ifndef ARCHIVE_H_INCLUDED
#error "archive.h must be included"
#endif

#include "archive_acl_private.h"

#ifndef ARCHIVE_ACL_PRIVATE_H_INCLUDED
#error "archive_acl_private.h must be included"
#endif

#include "archive_entry.h"

#ifndef ARCHIVE_ENTRY_H_INCLUDED
#error "archive_entry.h must be included"
#endif

#include "archive_private.h"

#ifndef ARCHIVE_PRIVATE_H_INCLUDED
#error "archive_private.h must be included"
#endif

#include "archive_string.h"

#ifndef ARCHIVE_STRING_H_INCLUDED
#error "archive_string.h must be included"
#endif

#include "archive_time_private.h"

#ifndef ARCHIVE_TIME_PRIVATE_H_INCLUDED
#error "archive_time_private.h must be included"
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifndef IO_REPARSE_TAG_SYMLINK

#define IO_REPARSE_TAG_SYMLINK 0xA000000CL
#endif

static BOOL SetFilePointerEx_perso(HANDLE hFile, LARGE_INTEGER liDistanceToMove,
                                   PLARGE_INTEGER lpNewFilePointer,
                                   DWORD dwMoveMethod) {
  LARGE_INTEGER li;
  li.QuadPart = liDistanceToMove.QuadPart;
  li.LowPart = SetFilePointer(hFile, li.LowPart, &li.HighPart, dwMoveMethod);
  if (lpNewFilePointer) {
    lpNewFilePointer->QuadPart = li.QuadPart;
  }
  return li.LowPart != (DWORD)-1 || GetLastError() == NO_ERROR;
}

struct fixup_entry {
  struct fixup_entry *next;
  struct archive_acl acl;
  mode_t mode;
  int64_t atime;
  int64_t birthtime;
  int64_t mtime;
  int64_t ctime;
  unsigned long atime_nanos;
  unsigned long birthtime_nanos;
  unsigned long mtime_nanos;
  unsigned long ctime_nanos;
  unsigned long fflags_set;
  int fixup;
  wchar_t *name;
};

#define TODO_MODE_FORCE 0x40000000
#define TODO_MODE_BASE 0x20000000
#define TODO_SUID 0x10000000
#define TODO_SUID_CHECK 0x08000000
#define TODO_SGID 0x04000000
#define TODO_SGID_CHECK 0x02000000
#define TODO_MODE (TODO_MODE_BASE | TODO_SUID | TODO_SGID)
#define TODO_TIMES ARCHIVE_EXTRACT_TIME
#define TODO_OWNER ARCHIVE_EXTRACT_OWNER
#define TODO_FFLAGS ARCHIVE_EXTRACT_FFLAGS
#define TODO_ACLS ARCHIVE_EXTRACT_ACL
#define TODO_XATTR ARCHIVE_EXTRACT_XATTR
#define TODO_MAC_METADATA ARCHIVE_EXTRACT_MAC_METADATA

struct archive_write_disk {
  struct archive archive;

  mode_t user_umask;
  struct fixup_entry *fixup_list;
  struct fixup_entry *current_fixup;
  int64_t user_uid;
  int skip_file_set;
  int64_t skip_file_dev;
  int64_t skip_file_ino;
  time_t start_time;

  int64_t (*lookup_gid)(void *private, const char *gname, int64_t gid);
  void (*cleanup_gid)(void *private);
  void *lookup_gid_data;
  int64_t (*lookup_uid)(void *private, const char *uname, int64_t uid);
  void (*cleanup_uid)(void *private);
  void *lookup_uid_data;

  struct archive_wstring path_safe;

  BY_HANDLE_FILE_INFORMATION st;
  BY_HANDLE_FILE_INFORMATION *pst;

  struct archive_entry *entry;
  wchar_t *name;
  struct archive_wstring _name_data;
  wchar_t *tmpname;
  struct archive_wstring _tmpname_data;

  int todo;

  int deferred;

  int flags;

  HANDLE fh;

  int64_t offset;

  int64_t fd_offset;

  int64_t total_bytes_written;

  int64_t filesize;

  int restore_pwd;

  mode_t mode;

  int64_t uid;
  int64_t gid;
};

#define DEFAULT_DIR_MODE 0777

#define MINIMUM_DIR_MODE 0700
#define MAXIMUM_DIR_MODE 0775

static int disk_unlink(const wchar_t *);
static int disk_rmdir(const wchar_t *);
static int check_symlinks(struct archive_write_disk *);
static int create_filesystem_object(struct archive_write_disk *);
static struct fixup_entry *current_fixup(struct archive_write_disk *,
                                         const wchar_t *pathname);
static int cleanup_pathname(struct archive_write_disk *, wchar_t *);
static int create_dir(struct archive_write_disk *, wchar_t *);
static int create_parent_dir(struct archive_write_disk *, wchar_t *);
static int la_chmod(const wchar_t *, mode_t);
static int la_mktemp(struct archive_write_disk *);
static int older(BY_HANDLE_FILE_INFORMATION *, struct archive_entry *);
static int permissive_name_w(struct archive_write_disk *);
static int restore_entry(struct archive_write_disk *);
static int set_acls(struct archive_write_disk *, HANDLE h, const wchar_t *,
                    struct archive_acl *);
static int set_xattrs(struct archive_write_disk *);
static int clear_nochange_fflags(struct archive_write_disk *);
static int set_fflags(struct archive_write_disk *);
static int set_fflags_platform(const wchar_t *, unsigned long, unsigned long);
static int set_ownership(struct archive_write_disk *);
static int set_mode(struct archive_write_disk *, int mode);
static int set_times(struct archive_write_disk *, HANDLE, int, const wchar_t *,
                     time_t, long, time_t, long, time_t, long, time_t, long);
static int set_times_from_entry(struct archive_write_disk *);
static struct fixup_entry *sort_dir_list(struct fixup_entry *p);
static ssize_t write_data_block(struct archive_write_disk *, const char *,
                                size_t);

static int _archive_write_disk_close(struct archive *);
static int _archive_write_disk_free(struct archive *);
static int _archive_write_disk_header(struct archive *, struct archive_entry *);
static int64_t _archive_write_disk_filter_bytes(struct archive *, int);
static int _archive_write_disk_finish_entry(struct archive *);
static ssize_t _archive_write_disk_data(struct archive *, const void *, size_t);
static ssize_t _archive_write_disk_data_block(struct archive *, const void *,
                                              size_t, int64_t);

#define bhfi_dev(bhfi) ((bhfi)->dwVolumeSerialNumber)

#define bhfi_ino(bhfi)                                                         \
  ((((int64_t)((bhfi)->nFileIndexHigh & 0x0000FFFFUL)) << 32) |                \
   (bhfi)->nFileIndexLow)
#define bhfi_size(bhfi)                                                        \
  ((((int64_t)(bhfi)->nFileSizeHigh) << 32) | (bhfi)->nFileSizeLow)

static int file_information(struct archive_write_disk *a, wchar_t *path,
                            BY_HANDLE_FILE_INFORMATION *st, mode_t *mode,
                            int sim_lstat) {
  HANDLE h;
  int r;
  DWORD flag = FILE_FLAG_BACKUP_SEMANTICS;
  WIN32_FIND_DATAW findData;
#if _WIN32_WINNT >= 0x0602
  CREATEFILE2_EXTENDED_PARAMETERS createExParams;
#endif

  if (sim_lstat || mode != NULL) {
    h = FindFirstFileW(path, &findData);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_NAME) {
      wchar_t *full;
      full = __la_win_permissive_name_w(path);
      h = FindFirstFileW(full, &findData);
      free(full);
    }
    if (h == INVALID_HANDLE_VALUE) {
      la_dosmaperr(GetLastError());
      return (-1);
    }
    FindClose(h);
  }

  if (sim_lstat &&
      ((findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
       (findData.dwReserved0 == IO_REPARSE_TAG_SYMLINK)))
    flag |= FILE_FLAG_OPEN_REPARSE_POINT;

#if _WIN32_WINNT >= 0x0602
  ZeroMemory(&createExParams, sizeof(createExParams));
  createExParams.dwSize = sizeof(createExParams);
  createExParams.dwFileFlags = flag;
  h = CreateFile2(a->name, 0, 0, OPEN_EXISTING, &createExParams);
#else
  h = CreateFileW(a->name, 0, 0, NULL, OPEN_EXISTING, flag, NULL);
#endif
  if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_NAME) {
    wchar_t *full;
    full = __la_win_permissive_name_w(path);
#if _WIN32_WINNT >= 0x0602
    h = CreateFile2(full, 0, 0, OPEN_EXISTING, &createExParams);
#else
    h = CreateFileW(full, 0, 0, NULL, OPEN_EXISTING, flag, NULL);
#endif
    free(full);
  }
  if (h == INVALID_HANDLE_VALUE) {
    la_dosmaperr(GetLastError());
    return (-1);
  }
  r = GetFileInformationByHandle(h, st);
  CloseHandle(h);
  if (r == 0) {
    la_dosmaperr(GetLastError());
    return (-1);
  }

  if (mode == NULL)
    return (0);

  *mode = S_IRUSR | S_IRGRP | S_IROTH;
  if ((st->dwFileAttributes & FILE_ATTRIBUTE_READONLY) == 0)
    *mode |= S_IWUSR | S_IWGRP | S_IWOTH;
  if ((st->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
      findData.dwReserved0 == IO_REPARSE_TAG_SYMLINK)
    *mode |= S_IFLNK;
  else if (st->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    *mode |= S_IFDIR | S_IXUSR | S_IXGRP | S_IXOTH;
  else {
    const wchar_t *p;

    *mode |= S_IFREG;
    p = wcsrchr(path, L'.');
    if (p != NULL && wcslen(p) == 4) {
      switch (p[1]) {
      case L'B':
      case L'b':
        if ((p[2] == L'A' || p[2] == L'a') && (p[3] == L'T' || p[3] == L't'))
          *mode |= S_IXUSR | S_IXGRP | S_IXOTH;
        break;
      case L'C':
      case L'c':
        if (((p[2] == L'M' || p[2] == L'm') && (p[3] == L'D' || p[3] == L'd')))
          *mode |= S_IXUSR | S_IXGRP | S_IXOTH;
        break;
      case L'E':
      case L'e':
        if ((p[2] == L'X' || p[2] == L'x') && (p[3] == L'E' || p[3] == L'e'))
          *mode |= S_IXUSR | S_IXGRP | S_IXOTH;
        break;
      default:
        break;
      }
    }
  }
  return (0);
}

static int permissive_name_w(struct archive_write_disk *a) {
  wchar_t *wn, *wnp;
  wchar_t *ws, *wsp;
  DWORD l;

  wnp = a->name;
  if (wnp[0] == L'\\' && wnp[1] == L'\\' && wnp[2] == L'?' && wnp[3] == L'\\')

    return (0);

  if (wnp[0] == L'\\' && wnp[1] == L'\\' && wnp[2] == L'.' && wnp[3] == L'\\') {

    if (((wnp[4] >= L'a' && wnp[4] <= L'z') ||
         (wnp[4] >= L'A' && wnp[4] <= L'Z')) &&
        wnp[5] == L':' && wnp[6] == L'\\') {
      wnp[2] = L'?';
      return (0);
    }
  }

  if (((wnp[0] >= L'a' && wnp[0] <= L'z') ||
       (wnp[0] >= L'A' && wnp[0] <= L'Z')) &&
      wnp[1] == L':' && wnp[2] == L'\\') {
    wn = _wcsdup(wnp);
    if (wn == NULL)
      return (-1);
    if (archive_wstring_ensure(&(a->_name_data), 4 + wcslen(wn) + 1) == NULL) {
      free(wn);
      return (-1);
    }
    a->name = a->_name_data.s;

    archive_wstrncpy(&(a->_name_data), L"\\\\?\\", 4);
    archive_wstrcat(&(a->_name_data), wn);
    free(wn);
    return (0);
  }

  if (wnp[0] == L'\\' && wnp[1] == L'\\' && wnp[2] != L'\\') {
    const wchar_t *p = &wnp[2];

    while (*p != L'\\' && *p != L'\0')
      ++p;
    if (*p == L'\\') {
      const wchar_t *rp = ++p;

      while (*p != L'\\' && *p != L'\0')
        ++p;
      if (*p == L'\\' && p != rp) {

        wn = _wcsdup(wnp);
        if (wn == NULL)
          return (-1);
        if (archive_wstring_ensure(&(a->_name_data), 8 + wcslen(wn) + 1) ==
            NULL) {
          free(wn);
          return (-1);
        }
        a->name = a->_name_data.s;

        archive_wstrncpy(&(a->_name_data), L"\\\\?\\UNC\\", 8);
        archive_wstrcat(&(a->_name_data), wn + 2);
        free(wn);
        return (0);
      }
    }
    return (0);
  }

  l = GetCurrentDirectoryW(0, NULL);
  if (l == 0)
    return (-1);
  ws = malloc(l * sizeof(wchar_t));
  l = GetCurrentDirectoryW(l, ws);
  if (l == 0) {
    free(ws);
    return (-1);
  }
  wsp = ws;

  if (wnp[0] == L'\\') {
    wn = _wcsdup(wnp);
    if (wn == NULL) {
      free(wsp);
      return (-1);
    }
    if (archive_wstring_ensure(&(a->_name_data), 4 + 2 + wcslen(wn) + 1) ==
        NULL) {
      free(wsp);
      free(wn);
      return (-1);
    }
    a->name = a->_name_data.s;

    archive_wstrncpy(&(a->_name_data), L"\\\\?\\", 4);
    archive_wstrncat(&(a->_name_data), wsp, 2);
    archive_wstrcat(&(a->_name_data), wn);
    free(wsp);
    free(wn);
    return (0);
  }

  wn = _wcsdup(wnp);
  if (wn == NULL) {
    free(wsp);
    return (-1);
  }
  if (archive_wstring_ensure(&(a->_name_data), 4 + l + 1 + wcslen(wn) + 1) ==
      NULL) {
    free(wsp);
    free(wn);
    return (-1);
  }
  a->name = a->_name_data.s;

  if (l > 3 && wsp[0] == L'\\' && wsp[1] == L'\\' && wsp[2] == L'?' &&
      wsp[3] == L'\\') {
    archive_wstrncpy(&(a->_name_data), wsp, l);
  } else if (l > 2 && wsp[0] == L'\\' && wsp[1] == L'\\' && wsp[2] != L'\\') {
    archive_wstrncpy(&(a->_name_data), L"\\\\?\\UNC\\", 8);
    archive_wstrncat(&(a->_name_data), wsp + 2, l - 2);
  } else {
    archive_wstrncpy(&(a->_name_data), L"\\\\?\\", 4);
    archive_wstrncat(&(a->_name_data), wsp, l);
  }
  archive_wstrncat(&(a->_name_data), L"\\", 1);
  archive_wstrcat(&(a->_name_data), wn);
  a->name = a->_name_data.s;
  free(wsp);
  free(wn);
  return (0);
}

static int la_chmod(const wchar_t *path, mode_t mode) {
  DWORD attr;
  BOOL r;
  wchar_t *fullname;
  int ret = 0;

  fullname = NULL;
  attr = GetFileAttributesW(path);
  if (attr == (DWORD)-1 && GetLastError() == ERROR_INVALID_NAME) {
    fullname = __la_win_permissive_name_w(path);
    attr = GetFileAttributesW(fullname);
  }
  if (attr == (DWORD)-1) {
    la_dosmaperr(GetLastError());
    ret = -1;
    goto exit_chmode;
  }
  if (mode & _S_IWRITE)
    attr &= ~FILE_ATTRIBUTE_READONLY;
  else
    attr |= FILE_ATTRIBUTE_READONLY;
  if (fullname != NULL)
    r = SetFileAttributesW(fullname, attr);
  else
    r = SetFileAttributesW(path, attr);
  if (r == 0) {
    la_dosmaperr(GetLastError());
    ret = -1;
  }
exit_chmode:
  free(fullname);
  return (ret);
}

static int la_mktemp(struct archive_write_disk *a) {
  int fd;
  mode_t mode;

  archive_wstring_empty(&(a->_tmpname_data));
  archive_wstrcpy(&(a->_tmpname_data), a->name);
  archive_wstrcat(&(a->_tmpname_data), L".XXXXXX");
  a->tmpname = a->_tmpname_data.s;

  fd = __archive_mkstemp(a->tmpname);
  if (fd == -1)
    return -1;

  mode = a->mode & 0777 & ~a->user_umask;
  if (la_chmod(a->tmpname, mode) == -1) {
    la_dosmaperr(GetLastError());
    _close(fd);
    return -1;
  }
  return (fd);
}

#if _WIN32_WINNT < _WIN32_WINNT_VISTA
static void *la_GetFunctionKernel32(const char *name) {
  static HINSTANCE lib;
  static int set;
  if (!set) {
    set = 1;
    lib = LoadLibrary(TEXT("kernel32.dll"));
  }
  if (lib == NULL) {
    fprintf(stderr, "Can't load kernel32.dll?!\n");
    exit(1);
  }
  return (void *)GetProcAddress(lib, name);
}
#endif

static int la_CreateHardLinkW(wchar_t *linkname, wchar_t *target) {
  static BOOL(WINAPI * f)(LPCWSTR, LPCWSTR, LPSECURITY_ATTRIBUTES);
  BOOL ret;

#if _WIN32_WINNT < _WIN32_WINNT_XP
  static int set;

  if (!set) {
    set = 1;
    f = la_GetFunctionKernel32("CreateHardLinkW");
  }
#else
  f = CreateHardLinkW;
#endif
  if (!f) {
    errno = ENOTSUP;
    return (0);
  }
  ret = (*f)(linkname, target, NULL);
  if (!ret) {

#define IS_UNC(name)                                                           \
  ((name[0] == L'U' || name[0] == L'u') &&                                     \
   (name[1] == L'N' || name[1] == L'n') &&                                     \
   (name[2] == L'C' || name[2] == L'c') && name[3] == L'\\')
    if (!wcsncmp(linkname, L"\\\\?\\", 4)) {
      linkname += 4;
      if (IS_UNC(linkname))
        linkname += 4;
    }
    if (!wcsncmp(target, L"\\\\?\\", 4)) {
      target += 4;
      if (IS_UNC(target))
        target += 4;
    }
#undef IS_UNC
    ret = (*f)(linkname, target, NULL);
  }
  return (ret);
}

static int la_CreateSymbolicLinkW(const wchar_t *linkname,
                                  const wchar_t *target, int linktype) {
  static BOOLEAN(WINAPI * f)(LPCWSTR, LPCWSTR, DWORD);
  wchar_t *ttarget, *p;
  size_t len;
  DWORD attrs = 0;
  DWORD flags = 0;
  DWORD newflags = 0;
  BOOL ret = 0;

#if _WIN32_WINNT < _WIN32_WINNT_VISTA

  static int set;
  if (!set) {
    set = 1;
    f = la_GetFunctionKernel32("CreateSymbolicLinkW");
  }
#else
#if !defined(WINAPI_FAMILY_PARTITION) ||                                       \
    WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
  f = CreateSymbolicLinkW;
#else
  f = NULL;
#endif
#endif
  if (!f)
    return (0);

  len = wcslen(target);
  if (len == 0) {
    errno = EINVAL;
    return (0);
  }

  ttarget = malloc((len + 1) * sizeof(wchar_t));
  if (ttarget == NULL)
    return (0);

  p = ttarget;

  while (*target != L'\0') {
    if (*target == L'/')
      *p = L'\\';
    else
      *p = *target;
    target++;
    p++;
  }
  *p = L'\0';

  if (linktype != AE_SYMLINK_TYPE_FILE &&
      (linktype == AE_SYMLINK_TYPE_DIRECTORY || *(p - 1) == L'\\' ||
       (*(p - 1) == L'.' &&
        (len == 1 || *(p - 2) == L'\\' ||
         (*(p - 2) == L'.' && (len == 2 || *(p - 3) == L'\\')))))) {
#if defined(SYMBOLIC_LINK_FLAG_DIRECTORY)
    flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
#else
    flags |= 0x1;
#endif
  }

#if defined(SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)
  newflags = flags | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
#else
  newflags = flags | 0x2;
#endif

  attrs = GetFileAttributesW(linkname);
  if (attrs != INVALID_FILE_ATTRIBUTES) {
    if (attrs & FILE_ATTRIBUTE_DIRECTORY)
      disk_rmdir(linkname);
    else
      disk_unlink(linkname);
  }

  ret = (*f)(linkname, ttarget, newflags);

  if (!ret) {
    ret = (*f)(linkname, ttarget, flags);
  }
  free(ttarget);
  return (ret);
}

static int la_ftruncate(HANDLE handle, int64_t length) {
  LARGE_INTEGER distance;

  if (GetFileType(handle) != FILE_TYPE_DISK) {
    errno = EBADF;
    return (-1);
  }
  distance.QuadPart = length;
  if (!SetFilePointerEx_perso(handle, distance, NULL, FILE_BEGIN)) {
    la_dosmaperr(GetLastError());
    return (-1);
  }
  if (!SetEndOfFile(handle)) {
    la_dosmaperr(GetLastError());
    return (-1);
  }
  return (0);
}

static int lazy_stat(struct archive_write_disk *a) {
  if (a->pst != NULL) {

    return (ARCHIVE_OK);
  }
  if (a->fh != INVALID_HANDLE_VALUE &&
      GetFileInformationByHandle(a->fh, &a->st) == 0) {
    a->pst = &a->st;
    return (ARCHIVE_OK);
  }

  if (file_information(a, a->name, &a->st, NULL, 1) == 0) {
    a->pst = &a->st;
    return (ARCHIVE_OK);
  }
  archive_set_error(&a->archive, errno, "Couldn't stat file");
  return (ARCHIVE_WARN);
}

static const struct archive_vtable archive_write_disk_vtable = {
    .archive_close = _archive_write_disk_close,
    .archive_filter_bytes = _archive_write_disk_filter_bytes,
    .archive_free = _archive_write_disk_free,
    .archive_write_header = _archive_write_disk_header,
    .archive_write_finish_entry = _archive_write_disk_finish_entry,
    .archive_write_data = _archive_write_disk_data,
    .archive_write_data_block = _archive_write_disk_data_block,
};

static int64_t _archive_write_disk_filter_bytes(struct archive *_a, int n) {
  struct archive_write_disk *a = (struct archive_write_disk *)_a;
  (void)n;
  if (n == -1 || n == 0)
    return (a->total_bytes_written);
  return (-1);
}

int archive_write_disk_set_options(struct archive *_a, int flags) {
  struct archive_write_disk *a = (struct archive_write_disk *)_a;

  a->flags = flags;
  return (ARCHIVE_OK);
}

static int _archive_write_disk_header(struct archive *_a,
                                      struct archive_entry *entry) {
  struct archive_write_disk *a = (struct archive_write_disk *)_a;
  struct fixup_entry *fe;
  int ret, r;

  archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC,
                      ARCHIVE_STATE_HEADER | ARCHIVE_STATE_DATA,
                      "archive_write_disk_header");
  archive_clear_error(&a->archive);
  if (a->archive.state & ARCHIVE_STATE_DATA) {
    r = _archive_write_disk_finish_entry(&a->archive);
    if (r == ARCHIVE_FATAL)
      return (r);
  }

  a->pst = NULL;
  a->current_fixup = NULL;
  a->deferred = 0;
  archive_entry_free(a->entry);
  a->entry = NULL;
  a->entry = archive_entry_clone(entry);
  a->fh = INVALID_HANDLE_VALUE;
  a->fd_offset = 0;
  a->offset = 0;
  a->restore_pwd = -1;
  a->uid = a->user_uid;
  a->mode = archive_entry_mode(a->entry);
  if (archive_entry_size_is_set(a->entry))
    a->filesize = archive_entry_size(a->entry);
  else
    a->filesize = -1;
  archive_wstrcpy(&(a->_name_data), archive_entry_pathname_w(a->entry));
  a->name = a->_name_data.s;
  archive_clear_error(&a->archive);

  ret = cleanup_pathname(a, a->name);
  if (ret != ARCHIVE_OK)
    return (ret);

  if (permissive_name_w(a) < 0) {
    errno = EINVAL;
    return (ARCHIVE_FAILED);
  }

  umask(a->user_umask = umask(0));

  a->todo = TODO_MODE_BASE;
  if (a->flags & ARCHIVE_EXTRACT_PERM) {
    a->todo |= TODO_MODE_FORCE;

    if (a->mode & S_ISGID)
      a->todo |= TODO_SGID | TODO_SGID_CHECK;

    if (a->mode & S_ISUID)
      a->todo |= TODO_SUID | TODO_SUID_CHECK;
  } else {

    a->mode &= ~S_ISUID;
    a->mode &= ~S_ISGID;
    a->mode &= ~S_ISVTX;
    a->mode &= ~a->user_umask;
  }
#if 0
	if (a->flags & ARCHIVE_EXTRACT_OWNER)
		a->todo |= TODO_OWNER;
#endif
  if (a->flags & ARCHIVE_EXTRACT_TIME)
    a->todo |= TODO_TIMES;
  if (a->flags & ARCHIVE_EXTRACT_ACL) {
    if (archive_entry_filetype(a->entry) == AE_IFDIR)
      a->deferred |= TODO_ACLS;
    else
      a->todo |= TODO_ACLS;
  }
  if (a->flags & ARCHIVE_EXTRACT_XATTR)
    a->todo |= TODO_XATTR;
  if (a->flags & ARCHIVE_EXTRACT_FFLAGS)
    a->todo |= TODO_FFLAGS;
  if (a->flags & ARCHIVE_EXTRACT_SECURE_SYMLINKS) {
    ret = check_symlinks(a);
    if (ret != ARCHIVE_OK)
      return (ret);
  }

  ret = restore_entry(a);

  if (a->deferred & TODO_MODE) {
    fe = current_fixup(a, archive_entry_pathname_w(entry));
    fe->fixup |= TODO_MODE_BASE;
    fe->mode = a->mode;
  }

  if ((a->deferred & TODO_TIMES) && (archive_entry_mtime_is_set(entry) ||
                                     archive_entry_atime_is_set(entry))) {
    fe = current_fixup(a, archive_entry_pathname_w(entry));
    fe->mode = a->mode;
    fe->fixup |= TODO_TIMES;
    if (archive_entry_atime_is_set(entry)) {
      fe->atime = archive_entry_atime(entry);
      fe->atime_nanos = archive_entry_atime_nsec(entry);
    } else {

      fe->atime = a->start_time;
      fe->atime_nanos = 0;
    }
    if (archive_entry_mtime_is_set(entry)) {
      fe->mtime = archive_entry_mtime(entry);
      fe->mtime_nanos = archive_entry_mtime_nsec(entry);
    } else {

      fe->mtime = a->start_time;
      fe->mtime_nanos = 0;
    }
    if (archive_entry_birthtime_is_set(entry)) {
      fe->birthtime = archive_entry_birthtime(entry);
      fe->birthtime_nanos = archive_entry_birthtime_nsec(entry);
    } else {

      fe->birthtime = fe->mtime;
      fe->birthtime_nanos = fe->mtime_nanos;
    }
  }

  if (a->deferred & TODO_ACLS) {
    fe = current_fixup(a, archive_entry_pathname_w(entry));
    archive_acl_copy(&fe->acl, archive_entry_acl(entry));
  }

  if (a->deferred & TODO_FFLAGS) {
    unsigned long set, clear;

    fe = current_fixup(a, archive_entry_pathname_w(entry));
    archive_entry_fflags(entry, &set, &clear);
    fe->fflags_set = set;
  }

  if (a->fh != INVALID_HANDLE_VALUE && archive_entry_sparse_count(entry) > 0) {
    int64_t base = 0, offset, length;
    int i, cnt = archive_entry_sparse_reset(entry);
    int sparse = 0;

    for (i = 0; i < cnt; i++) {
      archive_entry_sparse_next(entry, &offset, &length);
      if (offset - base >= 4096) {
        sparse = 1;
        break;
      }
      base = offset + length;
    }
    if (sparse) {
      DWORD dmy;

      DeviceIoControl(a->fh, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &dmy, NULL);
    }
  }

  if (ret >= ARCHIVE_WARN)
    a->archive.state = ARCHIVE_STATE_DATA;

  if (a->fh == INVALID_HANDLE_VALUE) {
    archive_entry_set_size(entry, 0);
    a->filesize = 0;
  }

  return (ret);
}

int archive_write_disk_set_skip_file(struct archive *_a, la_int64_t d,
                                     la_int64_t i) {
  struct archive_write_disk *a = (struct archive_write_disk *)_a;
  archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC, ARCHIVE_STATE_ANY,
                      "archive_write_disk_set_skip_file");
  a->skip_file_set = 1;
  a->skip_file_dev = d;
  a->skip_file_ino = i;
  return (ARCHIVE_OK);
}

static ssize_t write_data_block(struct archive_write_disk *a, const char *buff,
                                size_t size) {
  OVERLAPPED ol;
  uint64_t start_size = size;
  DWORD bytes_written = 0;
  ssize_t block_size = 0, bytes_to_write;

  if (size == 0)
    return (ARCHIVE_OK);

  if (a->filesize == 0 || a->fh == INVALID_HANDLE_VALUE) {
    archive_set_error(&a->archive, 0, "Attempt to write to an empty file");
    return (ARCHIVE_WARN);
  }

  if (a->flags & ARCHIVE_EXTRACT_SPARSE) {

    block_size = 16 * 1024;
  }

  if (a->filesize >= 0 && (int64_t)(a->offset + size) > a->filesize)
    start_size = size = (size_t)(a->filesize - a->offset);

  while (size > 0) {
    if (block_size == 0) {
      bytes_to_write = size;
    } else {

      const char *p, *end;
      int64_t block_end;

      for (p = buff, end = buff + size; p < end; ++p) {
        if (*p != '\0')
          break;
      }
      a->offset += p - buff;
      size -= p - buff;
      buff = p;
      if (size == 0)
        break;

      block_end = (a->offset / block_size + 1) * block_size;

      bytes_to_write = size;
      if (a->offset + bytes_to_write > block_end)
        bytes_to_write = (DWORD)(block_end - a->offset);
    }
    memset(&ol, 0, sizeof(ol));
    ol.Offset = (DWORD)(a->offset & 0xFFFFFFFF);
    ol.OffsetHigh = (DWORD)(a->offset >> 32);
    if (!WriteFile(a->fh, buff, (uint32_t)bytes_to_write, &bytes_written,
                   &ol)) {
      DWORD lasterr;

      lasterr = GetLastError();
      if (lasterr == ERROR_ACCESS_DENIED)
        errno = EBADF;
      else
        la_dosmaperr(lasterr);
      archive_set_error(&a->archive, errno, "Write failed");
      return (ARCHIVE_WARN);
    }
    buff += bytes_written;
    size -= bytes_written;
    a->total_bytes_written += bytes_written;
    a->offset += bytes_written;
    a->fd_offset = a->offset;
  }
  return ((ssize_t)(start_size - size));
}

static ssize_t _archive_write_disk_data_block(struct archive *_a,
                                              const void *buff, size_t size,
                                              int64_t offset) {
  struct archive_write_disk *a = (struct archive_write_disk *)_a;
  ssize_t r;

  archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC, ARCHIVE_STATE_DATA,
                      "archive_write_data_block");

  a->offset = offset;
  r = write_data_block(a, buff, size);
  if (r < ARCHIVE_OK)
    return (r);
  if ((size_t)r < size) {
    archive_set_error(&a->archive, 0, "Write request too large");
    return (ARCHIVE_WARN);
  }
#if ARCHIVE_VERSION_NUMBER < 3999000
  return (ARCHIVE_OK);
#else
  return (size);
#endif
}

static ssize_t _archive_write_disk_data(struct archive *_a, const void *buff,
                                        size_t size) {
  struct archive_write_disk *a = (struct archive_write_disk *)_a;

  archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC, ARCHIVE_STATE_DATA,
                      "archive_write_data");

  return (write_data_block(a, buff, size));
}

static int _archive_write_disk_finish_entry(struct archive *_a) {
  struct archive_write_disk *a = (struct archive_write_disk *)_a;
  int ret = ARCHIVE_OK;

  archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC,
                      ARCHIVE_STATE_HEADER | ARCHIVE_STATE_DATA,
                      "archive_write_finish_entry");
  if (a->archive.state & ARCHIVE_STATE_HEADER)
    return (ARCHIVE_OK);
  archive_clear_error(&a->archive);

  if (a->fh == INVALID_HANDLE_VALUE) {

  } else if (a->filesize < 0) {

  } else if (a->fd_offset == a->filesize) {

  } else {
    if (la_ftruncate(a->fh, a->filesize) == -1) {
      archive_set_error(&a->archive, errno, "File size could not be restored");
      CloseHandle(a->fh);
      a->fh = INVALID_HANDLE_VALUE;
      return (ARCHIVE_FAILED);
    }
  }

  if (a->todo & (TODO_OWNER | TODO_SUID | TODO_SGID)) {
    a->uid = archive_write_disk_uid(&a->archive, archive_entry_uname(a->entry),
                                    archive_entry_uid(a->entry));
  }

  if (a->todo & (TODO_OWNER | TODO_SGID | TODO_SUID)) {
    a->gid = archive_write_disk_gid(&a->archive, archive_entry_gname(a->entry),
                                    archive_entry_gid(a->entry));
  }

  if (a->todo & TODO_OWNER)
    ret = set_ownership(a);

  if (a->todo & TODO_MODE) {
    int r2 = set_mode(a, a->mode);
    if (r2 < ret)
      ret = r2;
  }

  if (a->todo & TODO_XATTR) {
    int r2 = set_xattrs(a);
    if (r2 < ret)
      ret = r2;
  }

  if (a->todo & TODO_FFLAGS) {
    int r2 = set_fflags(a);
    if (r2 < ret)
      ret = r2;
  }

  if (a->todo & TODO_TIMES) {
    int r2 = set_times_from_entry(a);
    if (r2 < ret)
      ret = r2;
  }

  if (a->todo & TODO_ACLS) {
    int r2 = set_acls(a, a->fh, archive_entry_pathname_w(a->entry),
                      archive_entry_acl(a->entry));
    if (r2 < ret)
      ret = r2;
  }

  if (a->fh != INVALID_HANDLE_VALUE) {
    CloseHandle(a->fh);
    a->fh = INVALID_HANDLE_VALUE;
    if (a->tmpname) {

      disk_unlink(a->name);
      if (_wrename(a->tmpname, a->name) != 0) {
        la_dosmaperr(GetLastError());
        archive_set_error(&a->archive, errno,
                          "Failed to rename temporary file");
        ret = ARCHIVE_FAILED;
        disk_unlink(a->tmpname);
      }
      a->tmpname = NULL;
    }
  }

  archive_entry_free(a->entry);
  a->entry = NULL;
  a->archive.state = ARCHIVE_STATE_HEADER;
  return (ret);
}

int archive_write_disk_set_group_lookup(
    struct archive *_a, void *private_data,
    la_int64_t (*lookup_gid)(void *private, const char *gname, la_int64_t gid),
    void (*cleanup_gid)(void *private)) {
  struct archive_write_disk *a = (struct archive_write_disk *)_a;
  archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC, ARCHIVE_STATE_ANY,
                      "archive_write_disk_set_group_lookup");

  if (a->cleanup_gid != NULL && a->lookup_gid_data != NULL)
    (a->cleanup_gid)(a->lookup_gid_data);

  a->lookup_gid = lookup_gid;
  a->cleanup_gid = cleanup_gid;
  a->lookup_gid_data = private_data;
  return (ARCHIVE_OK);
}

int archive_write_disk_set_user_lookup(struct archive *_a, void *private_data,
                                       int64_t (*lookup_uid)(void *private,
                                                             const char *uname,
                                                             int64_t uid),
                                       void (*cleanup_uid)(void *private)) {
  struct archive_write_disk *a = (struct archive_write_disk *)_a;
  archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC, ARCHIVE_STATE_ANY,
                      "archive_write_disk_set_user_lookup");

  if (a->cleanup_uid != NULL && a->lookup_uid_data != NULL)
    (a->cleanup_uid)(a->lookup_uid_data);

  a->lookup_uid = lookup_uid;
  a->cleanup_uid = cleanup_uid;
  a->lookup_uid_data = private_data;
  return (ARCHIVE_OK);
}

int64_t archive_write_disk_gid(struct archive *_a, const char *name,
                               la_int64_t id) {
  struct archive_write_disk *a = (struct archive_write_disk *)_a;
  archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC, ARCHIVE_STATE_ANY,
                      "archive_write_disk_gid");
  if (a->lookup_gid)
    return (a->lookup_gid)(a->lookup_gid_data, name, id);
  return (id);
}

int64_t archive_write_disk_uid(struct archive *_a, const char *name,
                               la_int64_t id) {
  struct archive_write_disk *a = (struct archive_write_disk *)_a;
  archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC, ARCHIVE_STATE_ANY,
                      "archive_write_disk_uid");
  if (a->lookup_uid)
    return (a->lookup_uid)(a->lookup_uid_data, name, id);
  return (id);
}

struct archive *archive_write_disk_new(void) {
  struct archive_write_disk *a;

  a = calloc(1, sizeof(*a));
  if (a == NULL)
    return (NULL);
  a->archive.magic = ARCHIVE_WRITE_DISK_MAGIC;

  a->archive.state = ARCHIVE_STATE_HEADER;
  a->archive.vtable = &archive_write_disk_vtable;
  a->start_time = time(NULL);

  umask(a->user_umask = umask(0));
  if (archive_wstring_ensure(&a->path_safe, 512) == NULL) {
    free(a);
    return (NULL);
  }
  a->path_safe.s[0] = 0;
  return (&a->archive);
}

static int disk_unlink(const wchar_t *path) {
  wchar_t *fullname;
  int r;

  r = _wunlink(path);
  if (r != 0 && GetLastError() == ERROR_INVALID_NAME) {
    fullname = __la_win_permissive_name_w(path);
    r = _wunlink(fullname);
    free(fullname);
  }
  return (r);
}

static int disk_rmdir(const wchar_t *path) {
  wchar_t *fullname;
  int r;

  r = _wrmdir(path);
  if (r != 0 && GetLastError() == ERROR_INVALID_NAME) {
    fullname = __la_win_permissive_name_w(path);
    r = _wrmdir(fullname);
    free(fullname);
  }
  return (r);
}

static int restore_entry(struct archive_write_disk *a) {
  int ret = ARCHIVE_OK, en;

  if (a->flags & ARCHIVE_EXTRACT_UNLINK && !S_ISDIR(a->mode)) {

    if (a->flags & ARCHIVE_EXTRACT_CLEAR_NOCHANGE_FFLAGS)
      (void)clear_nochange_fflags(a);
    if (disk_unlink(a->name) == 0) {

      a->pst = NULL;
    } else if (errno == ENOENT) {

    } else if (disk_rmdir(a->name) == 0) {

      a->pst = NULL;
    } else {

      archive_set_error(&a->archive, errno, "Could not unlink");
      return (ARCHIVE_FAILED);
    }
  }

  en = create_filesystem_object(a);

  if ((en == ENOTDIR || en == ENOENT) &&
      !(a->flags & ARCHIVE_EXTRACT_NO_AUTODIR)) {
    wchar_t *full;

    create_parent_dir(a, a->name);

    full = __la_win_permissive_name_w(a->name);
    if (full == NULL) {
      en = EINVAL;
    } else {

      archive_wstrcpy(&(a->_name_data), full);
      a->name = a->_name_data.s;
      free(full);
      en = create_filesystem_object(a);
    }
  }

  if ((en == ENOENT) && (archive_entry_hardlink(a->entry) != NULL)) {
    archive_set_error(&a->archive, en, "Hard-link target '%s' does not exist.",
                      archive_entry_hardlink(a->entry));
    return (ARCHIVE_FAILED);
  }

  if ((en == EISDIR || en == EEXIST) &&
      (a->flags & ARCHIVE_EXTRACT_NO_OVERWRITE)) {

    if (S_ISDIR(a->mode)) {

      a->todo = 0;
    }
    archive_entry_unset_size(a->entry);
    return (ARCHIVE_OK);
  }

  if (en == EISDIR) {

    if (disk_rmdir(a->name) != 0) {
      archive_set_error(&a->archive, errno,
                        "Can't remove already-existing dir");
      return (ARCHIVE_FAILED);
    }
    a->pst = NULL;

    en = create_filesystem_object(a);
  } else if (en == EEXIST) {
    mode_t st_mode;
    mode_t lst_mode;
    BY_HANDLE_FILE_INFORMATION lst;

    int r = 0;
    int dirlnk = 0;

    r = file_information(a, a->name, &lst, &lst_mode, 1);
    if (r != 0) {
      archive_set_error(&a->archive, errno, "Can't stat existing object");
      return (ARCHIVE_FAILED);
    } else if (S_ISLNK(lst_mode)) {
      if (lst.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        dirlnk = 1;

      r = file_information(a, a->name, &a->st, &st_mode, 0);
      if (r != 0) {
        a->st = lst;
        st_mode = lst_mode;
      }
    } else {
      a->st = lst;
      st_mode = lst_mode;
    }

    if ((a->flags & ARCHIVE_EXTRACT_NO_OVERWRITE_NEWER) && !S_ISDIR(st_mode)) {
      if (!older(&(a->st), a->entry)) {
        archive_entry_unset_size(a->entry);
        return (ARCHIVE_OK);
      }
    }

    if (a->skip_file_set && bhfi_dev(&a->st) == a->skip_file_dev &&
        bhfi_ino(&a->st) == a->skip_file_ino) {
      archive_set_error(&a->archive, 0, "Refusing to overwrite archive");
      return (ARCHIVE_FAILED);
    }

    if (!S_ISDIR(st_mode)) {
      if (a->flags & ARCHIVE_EXTRACT_CLEAR_NOCHANGE_FFLAGS) {
        (void)clear_nochange_fflags(a);
      }
      if ((a->flags & ARCHIVE_EXTRACT_SAFE_WRITES) && S_ISREG(a->mode)) {
        int fd = la_mktemp(a);

        if (fd == -1) {
          la_dosmaperr(GetLastError());
          archive_set_error(&a->archive, errno, "Can't create temporary file");
          return (ARCHIVE_FAILED);
        }
        a->fh = (HANDLE)_get_osfhandle(fd);
        if (a->fh == INVALID_HANDLE_VALUE) {
          la_dosmaperr(GetLastError());
          return (ARCHIVE_FAILED);
        }
        a->pst = NULL;
        en = 0;
      } else {
        if (dirlnk) {

          if (disk_rmdir(a->name) != 0) {
            archive_set_error(&a->archive, errno,
                              "Can't unlink "
                              "directory symlink");
            return (ARCHIVE_FAILED);
          }
        } else {
          if (disk_unlink(a->name) != 0) {

            archive_set_error(&a->archive, errno,
                              "Can't unlink "
                              "already-existing object");
            return (ARCHIVE_FAILED);
          }
        }
        a->pst = NULL;

        en = create_filesystem_object(a);
      }
    } else if (!S_ISDIR(a->mode)) {

      if (a->flags & ARCHIVE_EXTRACT_CLEAR_NOCHANGE_FFLAGS)
        (void)clear_nochange_fflags(a);
      if (disk_rmdir(a->name) != 0) {
        archive_set_error(&a->archive, errno,
                          "Can't remove already-existing dir");
        return (ARCHIVE_FAILED);
      }

      en = create_filesystem_object(a);
    } else {

      if ((a->mode != st_mode) && (a->todo & TODO_MODE_FORCE))
        a->deferred |= (a->todo & TODO_MODE);

      en = 0;
    }
  }

  if (en) {

    archive_set_error(&a->archive, en, "Can't create '%ls'", a->name);
    return (ARCHIVE_FAILED);
  }

  a->pst = NULL;
  return (ret);
}

static int create_filesystem_object(struct archive_write_disk *a) {

  const wchar_t *linkname;
  wchar_t *fullname;
  mode_t final_mode, mode;
  int r;
  DWORD attrs = 0;
#if _WIN32_WINNT >= 0x0602
  CREATEFILE2_EXTENDED_PARAMETERS createExParams;
#endif

  linkname = archive_entry_hardlink_w(a->entry);
  if (linkname != NULL) {
    wchar_t *linksanitized, *linkfull, *namefull;
    size_t l = (wcslen(linkname) + 1) * sizeof(wchar_t);
    linksanitized = malloc(l);
    if (linksanitized == NULL) {
      archive_set_error(&a->archive, ENOMEM,
                        "Can't allocate memory for hardlink target");
      return (-1);
    }
    memcpy(linksanitized, linkname, l);
    r = cleanup_pathname(a, linksanitized);
    if (r != ARCHIVE_OK) {
      free(linksanitized);
      return (r);
    }
    linkfull = __la_win_permissive_name_w(linksanitized);
    free(linksanitized);
    namefull = __la_win_permissive_name_w(a->name);
    if (linkfull == NULL || namefull == NULL) {
      errno = EINVAL;
      r = -1;
    } else {

      if (a->flags & ARCHIVE_EXTRACT_SAFE_WRITES) {
        attrs = GetFileAttributesW(namefull);
        if (attrs != INVALID_FILE_ATTRIBUTES) {
          if (attrs & FILE_ATTRIBUTE_DIRECTORY)
            disk_rmdir(namefull);
          else
            disk_unlink(namefull);
        }
      }
      r = la_CreateHardLinkW(namefull, linkfull);
      if (r == 0) {
        la_dosmaperr(GetLastError());
        r = errno;
      } else
        r = 0;
    }

    if (r == 0 && a->filesize <= 0) {
      a->todo = 0;
      a->deferred = 0;
    } else if (r == 0 && a->filesize > 0) {
#if _WIN32_WINNT >= 0x0602
      ZeroMemory(&createExParams, sizeof(createExParams));
      createExParams.dwSize = sizeof(createExParams);
      createExParams.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
      a->fh = CreateFile2(namefull, GENERIC_WRITE, 0, TRUNCATE_EXISTING,
                          &createExParams);
#else
      a->fh = CreateFileW(namefull, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, NULL);
#endif
      if (a->fh == INVALID_HANDLE_VALUE) {
        la_dosmaperr(GetLastError());
        r = errno;
      }
    }
    free(linkfull);
    free(namefull);
    return (r);
  }
  linkname = archive_entry_symlink_w(a->entry);
  if (linkname != NULL) {

    attrs = GetFileAttributesW(a->name);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
      if (attrs & FILE_ATTRIBUTE_DIRECTORY)
        disk_rmdir(a->name);
      else
        disk_unlink(a->name);
    }
#if HAVE_SYMLINK
    return symlink(linkname, a->name) ? errno : 0;
#else
    errno = 0;
    r = la_CreateSymbolicLinkW((const wchar_t *)a->name, linkname,
                               archive_entry_symlink_type(a->entry));
    if (r == 0) {
      if (errno == 0)
        la_dosmaperr(GetLastError());
      r = errno;
    } else
      r = 0;
    return (r);
#endif
  }

  final_mode = a->mode & 07777;

  mode = final_mode & 0777 & ~a->user_umask;

  switch (a->mode & AE_IFMT) {
  default:

  case AE_IFREG:
    a->tmpname = NULL;
    fullname = a->name;

#if _WIN32_WINNT >= 0x0602
    ZeroMemory(&createExParams, sizeof(createExParams));
    createExParams.dwSize = sizeof(createExParams);
    createExParams.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    a->fh =
        CreateFile2(fullname, GENERIC_WRITE, 0, CREATE_NEW, &createExParams);
#else
    a->fh = CreateFileW(fullname, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL, NULL);
#endif
    if (a->fh == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_NAME &&
        fullname == a->name) {
      fullname = __la_win_permissive_name_w(a->name);
#if _WIN32_WINNT >= 0x0602
      a->fh =
          CreateFile2(fullname, GENERIC_WRITE, 0, CREATE_NEW, &createExParams);
#else
      a->fh = CreateFileW(fullname, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL, NULL);
#endif
    }
    if (a->fh == INVALID_HANDLE_VALUE) {
      if (GetLastError() == ERROR_ACCESS_DENIED) {
        DWORD attr;

        attr = GetFileAttributesW(fullname);
        if (attr == (DWORD)-1)
          la_dosmaperr(GetLastError());
        else if (attr & FILE_ATTRIBUTE_DIRECTORY)
          errno = EISDIR;
        else
          errno = EACCES;
      } else
        la_dosmaperr(GetLastError());
      r = 1;
    } else
      r = 0;
    if (fullname != a->name)
      free(fullname);
    break;
  case AE_IFCHR:
  case AE_IFBLK:

    return (EINVAL);
  case AE_IFDIR:
    mode = (mode | MINIMUM_DIR_MODE) & MAXIMUM_DIR_MODE;
    fullname = a->name;
    r = CreateDirectoryW(fullname, NULL);
    if (r == 0 && GetLastError() == ERROR_INVALID_NAME && fullname == a->name) {
      fullname = __la_win_permissive_name_w(a->name);
      r = CreateDirectoryW(fullname, NULL);
    }
    if (r != 0) {
      r = 0;

      a->deferred |= (a->todo & TODO_TIMES);
      a->todo &= ~TODO_TIMES;

      if ((mode != final_mode) || (a->flags & ARCHIVE_EXTRACT_PERM))
        a->deferred |= (a->todo & TODO_MODE);
      a->todo &= ~TODO_MODE;
    } else {
      la_dosmaperr(GetLastError());
      r = -1;
    }
    if (fullname != a->name)
      free(fullname);
    break;
  case AE_IFIFO:

    return (EINVAL);
  }

  if (r)
    return (errno);

  if (mode == final_mode)
    a->todo &= ~TODO_MODE;
  return (0);
}

static int _archive_write_disk_close(struct archive *_a) {
  struct archive_write_disk *a = (struct archive_write_disk *)_a;
  struct fixup_entry *next, *p;
  int ret;

  archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC,
                      ARCHIVE_STATE_HEADER | ARCHIVE_STATE_DATA,
                      "archive_write_disk_close");
  ret = _archive_write_disk_finish_entry(&a->archive);

  p = sort_dir_list(a->fixup_list);

  while (p != NULL) {
    a->pst = NULL;
    if (p->fixup & TODO_TIMES) {
      set_times(a, INVALID_HANDLE_VALUE, p->mode, p->name, p->atime,
                p->atime_nanos, p->birthtime, p->birthtime_nanos, p->mtime,
                p->mtime_nanos, p->ctime, p->ctime_nanos);
    }
    if (p->fixup & TODO_MODE_BASE)
      la_chmod(p->name, p->mode);
    if (p->fixup & TODO_ACLS)
      set_acls(a, INVALID_HANDLE_VALUE, p->name, &p->acl);
    if (p->fixup & TODO_FFLAGS)
      set_fflags_platform(p->name, p->fflags_set, 0);
    next = p->next;
    archive_acl_clear(&p->acl);
    free(p->name);
    free(p);
    p = next;
  }
  a->fixup_list = NULL;
  return (ret);
}

static int _archive_write_disk_free(struct archive *_a) {
  struct archive_write_disk *a;
  int ret;
  if (_a == NULL)
    return (ARCHIVE_OK);
  archive_check_magic(_a, ARCHIVE_WRITE_DISK_MAGIC,
                      ARCHIVE_STATE_ANY | ARCHIVE_STATE_FATAL,
                      "archive_write_disk_free");
  a = (struct archive_write_disk *)_a;
  ret = _archive_write_disk_close(&a->archive);
  archive_write_disk_set_group_lookup(&a->archive, NULL, NULL, NULL);
  archive_write_disk_set_user_lookup(&a->archive, NULL, NULL, NULL);
  archive_entry_free(a->entry);
  archive_wstring_free(&a->_name_data);
  archive_wstring_free(&a->_tmpname_data);
  archive_string_free(&a->archive.error_string);
  archive_wstring_free(&a->path_safe);
  a->archive.magic = 0;
  __archive_clean(&a->archive);
  free(a);
  return (ret);
}

static struct fixup_entry *sort_dir_list(struct fixup_entry *p) {
  struct fixup_entry *a, *b, *t;

  if (p == NULL)
    return (NULL);

  if (p->next == NULL)
    return (p);

  t = p;
  a = p->next->next;
  while (a != NULL) {

    a = a->next;
    if (a != NULL)
      a = a->next;
    t = t->next;
  }

  b = t->next;
  t->next = NULL;
  a = p;

  a = sort_dir_list(a);
  b = sort_dir_list(b);

  if (wcscmp(a->name, b->name) > 0) {
    t = p = a;
    a = a->next;
  } else {
    t = p = b;
    b = b->next;
  }

  while (a != NULL && b != NULL) {
    if (wcscmp(a->name, b->name) > 0) {
      t->next = a;
      a = a->next;
    } else {
      t->next = b;
      b = b->next;
    }
    t = t->next;
  }

  if (a != NULL)
    t->next = a;
  if (b != NULL)
    t->next = b;

  return (p);
}

static struct fixup_entry *new_fixup(struct archive_write_disk *a,
                                     const wchar_t *pathname) {
  struct fixup_entry *fe;

  fe = calloc(1, sizeof(struct fixup_entry));
  if (fe == NULL)
    return (NULL);
  fe->next = a->fixup_list;
  a->fixup_list = fe;
  fe->fixup = 0;
  fe->name = _wcsdup(pathname);
  fe->fflags_set = 0;
  return (fe);
}

static struct fixup_entry *current_fixup(struct archive_write_disk *a,
                                         const wchar_t *pathname) {
  if (a->current_fixup == NULL)
    a->current_fixup = new_fixup(a, pathname);
  return (a->current_fixup);
}

static int check_symlinks(struct archive_write_disk *a) {
  wchar_t *pn, *p;
  wchar_t c;
  int r;
  BY_HANDLE_FILE_INFORMATION st;
  mode_t st_mode;

  pn = a->name;
  p = a->path_safe.s;
  while ((*pn != '\0') && (*p == *pn))
    ++p, ++pn;

  while (*pn == '\\')
    ++pn;
  c = pn[0];

  while (pn[0] != '\0' && (pn[0] != '\\' || pn[1] != '\0')) {

    while (*pn != '\0' && *pn != '\\')
      ++pn;
    c = pn[0];
    pn[0] = '\0';

    r = file_information(a, a->name, &st, &st_mode, 1);
    if (r != 0) {

      if (errno == ENOENT)
        break;
    } else if (S_ISLNK(st_mode)) {
      if (c == '\0') {

        if (a->flags & ARCHIVE_EXTRACT_CLEAR_NOCHANGE_FFLAGS) {
          (void)clear_nochange_fflags(a);
        }
        if (st.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          r = disk_rmdir(a->name);
        } else {
          r = disk_unlink(a->name);
        }
        if (r) {
          archive_set_error(&a->archive, errno, "Could not remove symlink %ls",
                            a->name);
          pn[0] = c;
          return (ARCHIVE_FAILED);
        }
        a->pst = NULL;

        if (!S_ISLNK(a->mode)) {
          archive_set_error(&a->archive, 0, "Removing symlink %ls", a->name);
        }

        pn[0] = c;
        return (0);
      } else if (a->flags & ARCHIVE_EXTRACT_UNLINK) {

        if (a->flags & ARCHIVE_EXTRACT_CLEAR_NOCHANGE_FFLAGS) {
          (void)clear_nochange_fflags(a);
        }
        if (st.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          r = disk_rmdir(a->name);
        } else {
          r = disk_unlink(a->name);
        }
        if (r != 0) {
          archive_set_error(&a->archive, 0,
                            "Cannot remove intervening "
                            "symlink %ls",
                            a->name);
          pn[0] = c;
          return (ARCHIVE_FAILED);
        }
        a->pst = NULL;
      } else {
        archive_set_error(&a->archive, 0, "Cannot extract through symlink %ls",
                          a->name);
        pn[0] = c;
        return (ARCHIVE_FAILED);
      }
    }
    if (!c)
      break;
    pn[0] = c;
    pn++;
  }
  pn[0] = c;

  archive_wstrcpy(&a->path_safe, a->name);
  return (ARCHIVE_OK);
}

static int guidword(wchar_t *p, int n) {
  int i;

  for (i = 0; i < n; i++) {
    if ((*p >= L'0' && *p <= L'9') || (*p >= L'a' && *p <= L'f') ||
        (*p >= L'A' && *p <= L'F'))
      p++;
    else
      return (-1);
  }
  return (0);
}

static int cleanup_pathname(struct archive_write_disk *a, wchar_t *name) {
  wchar_t *dest, *src, *p, *top;
  wchar_t separator = L'\0';
  BOOL absolute_path = 0;

  p = name;
  if (*p == L'\0') {
    archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                      "Invalid empty pathname");
    return (ARCHIVE_FAILED);
  }

  for (; *p != L'\0'; p++) {
    if (*p == L'/')
      *p = L'\\';
  }
  p = name;

  if (p[0] == L'\\' && p[1] == L'\\' && (p[2] == L'.' || p[2] == L'?') &&
      p[3] == L'\\') {
    absolute_path = 1;

    if (p[2] == L'?' && (p[4] == L'U' || p[4] == L'u') &&
        (p[5] == L'N' || p[5] == L'n') && (p[6] == L'C' || p[6] == L'c') &&
        p[7] == L'\\')
      p += 8;

    else if (p[2] == L'?' && (p[4] == L'V' || p[4] == L'v') &&
             (p[5] == L'O' || p[5] == L'o') && (p[6] == L'L' || p[6] == L'l') &&
             (p[7] == L'U' || p[7] == L'u') && (p[8] == L'M' || p[8] == L'm') &&
             (p[9] == L'E' || p[9] == L'e') && p[10] == L'{') {
      if (guidword(p + 11, 8) == 0 && p[19] == L'-' &&
          guidword(p + 20, 4) == 0 && p[24] == L'-' &&
          guidword(p + 25, 4) == 0 && p[29] == L'-' &&
          guidword(p + 30, 4) == 0 && p[34] == L'-' &&
          guidword(p + 35, 12) == 0 && p[47] == L'}' && p[48] == L'\\')
        p += 49;
      else
        p += 4;

    } else if (p[2] == L'.' && (p[4] == L'P' || p[4] == L'p') &&
               (p[5] == L'H' || p[5] == L'h') &&
               (p[6] == L'Y' || p[6] == L'y') &&
               (p[7] == L'S' || p[7] == L's') &&
               (p[8] == L'I' || p[8] == L'i') &&
               (p[9] == L'C' || p[9] == L'c') &&
               (p[9] == L'A' || p[9] == L'a') &&
               (p[9] == L'L' || p[9] == L'l') &&
               (p[9] == L'D' || p[9] == L'd') &&
               (p[9] == L'R' || p[9] == L'r') &&
               (p[9] == L'I' || p[9] == L'i') &&
               (p[9] == L'V' || p[9] == L'v') &&
               (p[9] == L'E' || p[9] == L'e') &&
               (p[10] >= L'0' && p[10] <= L'9') && p[11] == L'\0') {
      archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                        "Path is a physical drive name");
      return (ARCHIVE_FAILED);
    } else
      p += 4;

  } else if (p[0] == L'\\' && p[1] == L'\\') {
    absolute_path = 1;
    p += 2;
  }

  if (((p[0] >= L'a' && p[0] <= L'z') || (p[0] >= L'A' && p[0] <= L'Z')) &&
      p[1] == L':') {
    if (p[2] == L'\0') {
      archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                        "Path is a drive name");
      return (ARCHIVE_FAILED);
    }

    absolute_path = 1;

    if (p[2] == L'\\')
      p += 2;
  }

  if (absolute_path && (a->flags & ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS)) {
    archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC, "Path is absolute");
    return (ARCHIVE_FAILED);
  }

  top = dest = src = p;

  for (; *p != L'\0'; p++) {
    if (*p == L':' || *p == L'*' || *p == L'?' || *p == L'"' || *p == L'<' ||
        *p == L'>' || *p == L'|')
      *p = L'_';
  }

  if (*src == L'\\')
    separator = *src++;

  for (;;) {

    if (src[0] == L'\0') {
      break;
    } else if (src[0] == L'\\') {

      src++;
      continue;
    } else if (src[0] == L'.') {
      if (src[1] == L'\0') {

        break;
      } else if (src[1] == L'\\') {

        src += 2;
        continue;
      } else if (src[1] == L'.') {
        if (src[2] == L'\\' || src[2] == L'\0') {

          if (a->flags & ARCHIVE_EXTRACT_SECURE_NODOTDOT) {
            archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                              "Path contains '..'");
            return (ARCHIVE_FAILED);
          }
        }
      }
    }

    if (separator)
      *dest++ = L'\\';
    while (*src != L'\0' && *src != L'\\') {
      *dest++ = *src++;
    }

    if (*src == L'\0')
      break;

    separator = *src++;
  }

  if (dest == top) {

    if (separator)
      *dest++ = L'\\';
    else
      *dest++ = L'.';
  }

  *dest = L'\0';
  return (ARCHIVE_OK);
}

static int create_parent_dir(struct archive_write_disk *a, wchar_t *path) {
  wchar_t *slash;
  int r;

  slash = wcsrchr(path, L'\\');
  if (slash == NULL)
    return (ARCHIVE_OK);
  *slash = L'\0';
  r = create_dir(a, path);
  *slash = L'\\';
  return (r);
}

static int create_dir(struct archive_write_disk *a, wchar_t *path) {
  BY_HANDLE_FILE_INFORMATION st;
  struct fixup_entry *le;
  wchar_t *slash, *base, *full;
  mode_t mode_final, mode, st_mode;
  int r;

  slash = wcsrchr(path, L'\\');
  if (slash == NULL)
    base = path;
  else
    base = slash + 1;

  if (base[0] == L'\0' || (base[0] == L'.' && base[1] == L'\0') ||
      (base[0] == L'.' && base[1] == L'.' && base[2] == L'\0')) {

    if (slash != NULL) {
      *slash = L'\0';
      r = create_dir(a, path);
      *slash = L'\\';
      return (r);
    }
    return (ARCHIVE_OK);
  }

  if (file_information(a, path, &st, &st_mode, 0) == 0) {
    if (S_ISDIR(st_mode))
      return (ARCHIVE_OK);
    if ((a->flags & ARCHIVE_EXTRACT_NO_OVERWRITE)) {
      archive_set_error(&a->archive, EEXIST, "Can't create directory '%ls'",
                        path);
      return (ARCHIVE_FAILED);
    }
    if (disk_unlink(path) != 0) {
      archive_set_error(&a->archive, errno,
                        "Can't create directory '%ls': "
                        "Conflicting file cannot be removed",
                        path);
      return (ARCHIVE_FAILED);
    }
  } else if (errno != ENOENT && errno != ENOTDIR) {

    archive_set_error(&a->archive, errno, "Can't test directory '%ls'", path);
    return (ARCHIVE_FAILED);
  } else if (slash != NULL) {
    *slash = '\0';
    r = create_dir(a, path);
    *slash = '\\';
    if (r != ARCHIVE_OK)
      return (r);
  }

  mode_final = DEFAULT_DIR_MODE & ~a->user_umask;

  mode = mode_final;
  mode |= MINIMUM_DIR_MODE;
  mode &= MAXIMUM_DIR_MODE;

  full = __la_win_permissive_name_w(path);
  if (full == NULL)
    errno = EINVAL;
  else if (CreateDirectoryW(full, NULL) != 0) {
    if (mode != mode_final) {
      le = new_fixup(a, path);
      le->fixup |= TODO_MODE_BASE;
      le->mode = mode_final;
    }
    free(full);
    return (ARCHIVE_OK);
  } else {
    la_dosmaperr(GetLastError());
  }
  free(full);

  if (file_information(a, path, &st, &st_mode, 0) == 0 && S_ISDIR(st_mode))
    return (ARCHIVE_OK);

  archive_set_error(&a->archive, errno, "Failed to create dir '%ls'", path);
  return (ARCHIVE_FAILED);
}

static int set_ownership(struct archive_write_disk *a) {

  if (a->user_uid != 0 && a->user_uid != a->uid) {
    archive_set_error(&a->archive, errno, "Can't set UID=%jd",
                      (intmax_t)a->uid);
    return (ARCHIVE_WARN);
  }

  archive_set_error(&a->archive, errno, "Can't set user=%jd/group=%jd for %ls",
                    (intmax_t)a->uid, (intmax_t)a->gid, a->name);
  return (ARCHIVE_WARN);
}

static int set_times(struct archive_write_disk *a, HANDLE h, int mode,
                     const wchar_t *name, time_t atime, long atime_nanos,
                     time_t birthtime, long birthtime_nanos, time_t mtime,
                     long mtime_nanos, time_t ctime_sec, long ctime_nanos) {
  HANDLE hw = 0;
  ULARGE_INTEGER wintm;
  FILETIME *pfbtime;
  FILETIME fatime, fbtime, fmtime;

  (void)ctime_sec;
  (void)ctime_nanos;

  if (h != INVALID_HANDLE_VALUE) {
    hw = NULL;
  } else {
    wchar_t *ws;
#if _WIN32_WINNT >= 0x0602
    CREATEFILE2_EXTENDED_PARAMETERS createExParams;
#endif

    if (S_ISLNK(mode))
      return (ARCHIVE_OK);
    ws = __la_win_permissive_name_w(name);
    if (ws == NULL)
      goto settimes_failed;
#if _WIN32_WINNT >= 0x0602
    ZeroMemory(&createExParams, sizeof(createExParams));
    createExParams.dwSize = sizeof(createExParams);
    createExParams.dwFileFlags = FILE_FLAG_BACKUP_SEMANTICS;
    hw = CreateFile2(ws, FILE_WRITE_ATTRIBUTES, 0, OPEN_EXISTING,
                     &createExParams);
#else
    hw = CreateFileW(ws, FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING,
                     FILE_FLAG_BACKUP_SEMANTICS, NULL);
#endif
    free(ws);
    if (hw == INVALID_HANDLE_VALUE)
      goto settimes_failed;
    h = hw;
  }

  wintm.QuadPart = unix_to_ntfs(atime, atime_nanos);
  fatime.dwLowDateTime = wintm.LowPart;
  fatime.dwHighDateTime = wintm.HighPart;
  wintm.QuadPart = unix_to_ntfs(mtime, mtime_nanos);
  fmtime.dwLowDateTime = wintm.LowPart;
  fmtime.dwHighDateTime = wintm.HighPart;

  if (birthtime > 0 || birthtime_nanos > 0) {
    wintm.QuadPart = unix_to_ntfs(birthtime, birthtime_nanos);
    fbtime.dwLowDateTime = wintm.LowPart;
    fbtime.dwHighDateTime = wintm.HighPart;
    pfbtime = &fbtime;
  } else
    pfbtime = NULL;
  if (SetFileTime(h, pfbtime, &fatime, &fmtime) == 0)
    goto settimes_failed;
  CloseHandle(hw);
  return (ARCHIVE_OK);

settimes_failed:
  CloseHandle(hw);
  archive_set_error(&a->archive, EINVAL, "Can't restore time");
  return (ARCHIVE_WARN);
}

static int set_times_from_entry(struct archive_write_disk *a) {
  time_t atime, birthtime, mtime, ctime_sec;
  long atime_nsec, birthtime_nsec, mtime_nsec, ctime_nsec;

  atime = birthtime = mtime = ctime_sec = a->start_time;
  atime_nsec = birthtime_nsec = mtime_nsec = ctime_nsec = 0;

  if (!archive_entry_atime_is_set(a->entry) &&
      !archive_entry_birthtime_is_set(a->entry) &&
      !archive_entry_mtime_is_set(a->entry))
    return (ARCHIVE_OK);

  if (archive_entry_atime_is_set(a->entry)) {
    atime = archive_entry_atime(a->entry);
    atime_nsec = archive_entry_atime_nsec(a->entry);
  }
  if (archive_entry_birthtime_is_set(a->entry)) {
    birthtime = archive_entry_birthtime(a->entry);
    birthtime_nsec = archive_entry_birthtime_nsec(a->entry);
  }
  if (archive_entry_mtime_is_set(a->entry)) {
    mtime = archive_entry_mtime(a->entry);
    mtime_nsec = archive_entry_mtime_nsec(a->entry);
  }
  if (archive_entry_ctime_is_set(a->entry)) {
    ctime_sec = archive_entry_ctime(a->entry);
    ctime_nsec = archive_entry_ctime_nsec(a->entry);
  }

  return set_times(a, a->fh, a->mode, a->name, atime, atime_nsec, birthtime,
                   birthtime_nsec, mtime, mtime_nsec, ctime_sec, ctime_nsec);
}

static int set_mode(struct archive_write_disk *a, int mode) {
  int r = ARCHIVE_OK;
  mode &= 07777;

  if (a->todo & TODO_SGID_CHECK) {

    if ((r = lazy_stat(a)) != ARCHIVE_OK)
      return (r);
    if (0 != a->gid) {
      mode &= ~S_ISGID;
    }

    if (0 != a->uid && (a->todo & TODO_SUID)) {
      mode &= ~S_ISUID;
    }
    a->todo &= ~TODO_SGID_CHECK;
    a->todo &= ~TODO_SUID_CHECK;
  } else if (a->todo & TODO_SUID_CHECK) {

    if (a->user_uid != a->uid) {
      mode &= ~S_ISUID;
    }
    a->todo &= ~TODO_SUID_CHECK;
  }

  if (S_ISLNK(a->mode)) {
#ifdef HAVE_LCHMOD

    if (lchmod(a->name, mode) != 0) {
      archive_set_error(&a->archive, errno, "Can't set permissions to 0%o",
                        (int)mode);
      r = ARCHIVE_WARN;
    }
#endif
  } else if (!S_ISDIR(a->mode)) {

#ifdef HAVE_FCHMOD
    if (a->fd >= 0) {
      if (fchmod(a->fd, mode) != 0) {
        archive_set_error(&a->archive, errno, "Can't set permissions to 0%o",
                          (int)mode);
        r = ARCHIVE_WARN;
      }
    } else
#endif

        if (la_chmod(a->name, mode) != 0) {
      archive_set_error(&a->archive, errno, "Can't set permissions to 0%o",
                        (int)mode);
      r = ARCHIVE_WARN;
    }
  }
  return (r);
}

static int set_fflags_platform(const wchar_t *name, unsigned long fflags_set,
                               unsigned long fflags_clear) {
  DWORD oldflags, newflags;
  wchar_t *fullname;

  const DWORD settable_flags =
      FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NORMAL |
      FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_OFFLINE |
      FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM |
      FILE_ATTRIBUTE_TEMPORARY;

  oldflags = GetFileAttributesW(name);
  if (oldflags == (DWORD)-1 && GetLastError() == ERROR_INVALID_NAME) {
    fullname = __la_win_permissive_name_w(name);
    oldflags = GetFileAttributesW(fullname);
  }
  if (oldflags == (DWORD)-1) {
    la_dosmaperr(GetLastError());
    return (ARCHIVE_WARN);
  }
  newflags = ((oldflags & ~fflags_clear) | fflags_set) & settable_flags;
  if (SetFileAttributesW(name, newflags) == 0)
    return (ARCHIVE_WARN);
  return (ARCHIVE_OK);
}

static int clear_nochange_fflags(struct archive_write_disk *a) {
  return (set_fflags_platform(a->name, 0, FILE_ATTRIBUTE_READONLY));
}

static int set_fflags(struct archive_write_disk *a) {
  unsigned long set, clear;

  if (a->todo & TODO_FFLAGS) {
    archive_entry_fflags(a->entry, &set, &clear);
    if (set == 0 && clear == 0)
      return (ARCHIVE_OK);
    return (set_fflags_platform(a->name, set, clear));
  }
  return (ARCHIVE_OK);
}

static int set_acls(struct archive_write_disk *a, HANDLE h, const wchar_t *name,
                    struct archive_acl *acl) {
  (void)a;
  (void)h;
  (void)name;
  (void)acl;
  return (ARCHIVE_OK);
}

static int set_xattrs(struct archive_write_disk *a) {
  static int warning_done = 0;

  if (archive_entry_xattr_count(a->entry) != 0 && !warning_done) {
    warning_done = 1;
    archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
                      "Cannot restore extended attributes on this system");
    return (ARCHIVE_WARN);
  }

  return (ARCHIVE_OK);
}

static int older(BY_HANDLE_FILE_INFORMATION *st, struct archive_entry *entry) {
  int64_t sec;
  uint32_t nsec;

  ntfs_to_unix(FILETIME_to_ntfs(&st->ftLastWriteTime), &sec, &nsec);

  if (sec < archive_entry_mtime(entry))
    return (1);

  if (sec > archive_entry_mtime(entry))
    return (0);
  if ((long)nsec < archive_entry_mtime_nsec(entry))
    return (1);

  return (0);
}

#endif
