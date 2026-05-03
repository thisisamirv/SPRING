/*-
 * Copyright (c) 2003-2010 Tim Kientzle
 * Copyright (c) 2012 Michihiro NAKAJIMA
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

#if !defined(_WIN32) || defined(__CYGWIN__)

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_ACL_H
#include <sys/acl.h>
#endif
#ifdef HAVE_SYS_EXTATTR_H
#include <sys/extattr.h>
#endif
#if HAVE_SYS_XATTR_H
#include <sys/xattr.h>
#elif HAVE_ATTR_XATTR_H
#include <attr/xattr.h>
#endif
#ifdef HAVE_SYS_EA_H
#include <sys/ea.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_SYS_UTIME_H
#include <sys/utime.h>
#endif
#ifdef HAVE_COPYFILE_H
#include <copyfile.h>
#endif
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_GRP_H
#include <grp.h>
#endif
#ifdef HAVE_LANGINFO_H
#include <langinfo.h>
#endif
#ifdef HAVE_LINUX_FS_H
#include <linux/fs.h>
#endif

#ifdef HAVE_LINUX_EXT2_FS_H
#include <linux/ext2_fs.h>
#endif
#if defined(HAVE_EXT2FS_EXT2_FS_H) && !defined(__CYGWIN__)
#include <ext2fs/ext2_fs.h>
#endif
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_UTIME_H
#include <utime.h>
#endif
#ifdef F_GETTIMES
#include <sys/fcntl1.h>
#endif

#define to_int64_time(t)                                                       \
  ((t) < 0                               ? (int64_t)(t)                        \
   : (uint64_t)(t) > (uint64_t)INT64_MAX ? INT64_MAX                           \
                                         : (int64_t)(t))

#if __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_MAC && !TARGET_OS_EMBEDDED && HAVE_QUARANTINE_H
#include <quarantine.h>
#define HAVE_QUARANTINE 1
#endif
#endif

#ifdef HAVE_ZLIB_H
#include <zlib.h>
#endif

#include "archive.h"

#ifndef ARCHIVE_H_INCLUDED
#error "archive.h must be included"
#endif

#include "archive_acl_private.h"

#ifndef ARCHIVE_ACL_PRIVATE_H_INCLUDED
#error "archive_acl_private.h must be included"
#endif

#include "archive_endian.h"

#ifndef ARCHIVE_ENDIAN_H_INCLUDED
#error "archive_endian.h must be included"
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

#include "archive_write_disk_private.h"

#ifndef ARCHIVE_WRITE_DISK_PRIVATE_H_INCLUDED
#error "archive_write_disk_private.h must be included"
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#if defined O_NOFOLLOW && !(INT_MIN <= O_NOFOLLOW && O_NOFOLLOW <= INT_MAX)
#undef O_NOFOLLOW
#endif

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#ifndef AT_FDCWD
#define AT_FDCWD -100
#endif

struct fixup_entry {
  struct fixup_entry *next;
  struct archive_acl acl;
  mode_t mode;
  __LA_MODE_T filetype;
  int64_t atime;
  int64_t birthtime;
  int64_t mtime;
  int64_t ctime;
  unsigned long atime_nanos;
  unsigned long birthtime_nanos;
  unsigned long mtime_nanos;
  unsigned long ctime_nanos;
  unsigned long fflags_set;
  size_t mac_metadata_size;
  void *mac_metadata;
  int fixup;
  char *name;
};

#define TODO_MODE_FORCE 0x40000000
#define TODO_MODE_BASE 0x20000000
#define TODO_SUID 0x10000000
#define TODO_SUID_CHECK 0x08000000
#define TODO_SGID 0x04000000
#define TODO_SGID_CHECK 0x02000000
#define TODO_APPLEDOUBLE 0x01000000
#define TODO_MODE (TODO_MODE_BASE | TODO_SUID | TODO_SGID)
#define TODO_TIMES ARCHIVE_EXTRACT_TIME
#define TODO_OWNER ARCHIVE_EXTRACT_OWNER
#define TODO_FFLAGS ARCHIVE_EXTRACT_FFLAGS
#define TODO_ACLS ARCHIVE_EXTRACT_ACL
#define TODO_XATTR ARCHIVE_EXTRACT_XATTR
#define TODO_MAC_METADATA ARCHIVE_EXTRACT_MAC_METADATA
#define TODO_HFS_COMPRESSION ARCHIVE_EXTRACT_HFS_COMPRESSION_FORCED

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

  struct archive_string path_safe;

  struct stat st;
  struct stat *pst;

  struct archive_entry *entry;
  char *name;
  struct archive_string _name_data;
  char *tmpname;
  struct archive_string _tmpname_data;

  int todo;

  int deferred;

  int flags;

  int fd;

  int64_t offset;

  int64_t fd_offset;

  int64_t total_bytes_written;

  int64_t filesize;

  int restore_pwd;

  mode_t mode;

  int64_t uid;
  int64_t gid;

  uint32_t decmpfs_attr_size;
  unsigned char *decmpfs_header_p;

  int rsrc_xattr_options;

  unsigned char *resource_fork;
  size_t resource_fork_allocated_size;
  unsigned int decmpfs_block_count;
  uint32_t *decmpfs_block_info;

  unsigned char *compressed_buffer;
  size_t compressed_buffer_size;
  size_t compressed_buffer_remaining;

  uint32_t compressed_rsrc_position;
  uint32_t compressed_rsrc_position_v;

  char *uncompressed_buffer;
  size_t block_remaining_bytes;
  size_t file_remaining_bytes;
#ifdef HAVE_ZLIB_H
  z_stream stream;
  int stream_valid;
  int decmpfs_compression_level;
#endif
};

#define DEFAULT_DIR_MODE 0777

#define MINIMUM_DIR_MODE 0700
#define MAXIMUM_DIR_MODE 0775

#define MAX_DECMPFS_BLOCK_SIZE (64 * 1024)

#define CMP_XATTR 3
#define CMP_RESOURCE_FORK 4

#define RSRC_H_SIZE 260
#define RSRC_F_SIZE 50

#define COMPRESSED_W_SIZE (64 * 1024)

#define MAX_DECMPFS_XATTR_SIZE 3802
#ifndef DECMPFS_XATTR_NAME
#define DECMPFS_XATTR_NAME "com.apple.decmpfs"
#endif
#define DECMPFS_MAGIC 0x636d7066
#define DECMPFS_COMPRESSION_MAGIC 0
#define DECMPFS_COMPRESSION_TYPE 4
#define DECMPFS_UNCOMPRESSED_SIZE 8
#define DECMPFS_HEADER_SIZE 16

#define HFS_BLOCKS(s) ((s) >> 12)

static int la_opendirat(int, const char *);
static int la_mktemp(struct archive_write_disk *);
static int la_verify_filetype(mode_t, __LA_MODE_T);
static void fsobj_error(int *, struct archive_string *, int, const char *,
                        const char *);
static int check_symlinks_fsobj(char *, int *, struct archive_string *, int,
                                int);
static int check_symlinks(struct archive_write_disk *);
static int create_filesystem_object(struct archive_write_disk *);
static struct fixup_entry *current_fixup(struct archive_write_disk *,
                                         const char *pathname);
#if defined(HAVE_FCHDIR) && defined(PATH_MAX)
static void edit_deep_directories(struct archive_write_disk *ad);
#endif
static int cleanup_pathname_fsobj(char *, int *, struct archive_string *, int);
static int cleanup_pathname(struct archive_write_disk *);
static int create_dir(struct archive_write_disk *, char *);
static int create_parent_dir(struct archive_write_disk *, char *);
static ssize_t hfs_write_data_block(struct archive_write_disk *, const char *,
                                    size_t);
static int fixup_appledouble(struct archive_write_disk *, const char *);
static int older(struct stat *, struct archive_entry *);
static int restore_entry(struct archive_write_disk *);
static int set_mac_metadata(struct archive_write_disk *, const char *,
                            const void *, size_t);
static int set_xattrs(struct archive_write_disk *);
static int clear_nochange_fflags(struct archive_write_disk *);
static int set_fflags(struct archive_write_disk *);
static int set_fflags_platform(struct archive_write_disk *, int fd,
                               const char *name, mode_t mode,
                               unsigned long fflags_set,
                               unsigned long fflags_clear);
static int set_ownership(struct archive_write_disk *);
static int set_mode(struct archive_write_disk *, int mode);
static int set_time(int, int, const char *, time_t, long, time_t, long);
static int set_times(struct archive_write_disk *, int, int, const char *,
                     time_t, long, time_t, long, time_t, long, time_t, long);
static int set_times_from_entry(struct archive_write_disk *);
static struct fixup_entry *sort_dir_list(struct fixup_entry *p);
static ssize_t write_data_block(struct archive_write_disk *, const char *,
                                size_t);
static void close_file_descriptor(struct archive_write_disk *);

static int _archive_write_disk_close(struct archive *);
static int _archive_write_disk_free(struct archive *);
static int _archive_write_disk_header(struct archive *, struct archive_entry *);
static int64_t _archive_write_disk_filter_bytes(struct archive *, int);
static int _archive_write_disk_finish_entry(struct archive *);
static ssize_t _archive_write_disk_data(struct archive *, const void *, size_t);
static ssize_t _archive_write_disk_data_block(struct archive *, const void *,
                                              size_t, int64_t);

static int la_mktemp(struct archive_write_disk *a) {
  struct archive_string *tmp = &a->_tmpname_data;
  int oerrno, fd;
  mode_t mode;

  archive_strcpy(tmp, a->name);
  archive_string_dirname(tmp);
  archive_strcat(tmp, "/tar.XXXXXXXX");
  a->tmpname = tmp->s;

  fd = __archive_mkstemp(a->tmpname);
  if (fd == -1)
    return -1;

  mode = a->mode & 0777 & ~a->user_umask;
  if (fchmod(fd, mode) == -1) {
    oerrno = errno;
    close(fd);
    errno = oerrno;
    return -1;
  }
  return fd;
}

static int la_opendirat(int fd, const char *path) {
  const int flags = O_CLOEXEC
#if defined(O_BINARY)
                    | O_BINARY
#endif
#if defined(O_DIRECTORY)
                    | O_DIRECTORY
#endif
#if defined(O_PATH)
                    | O_PATH
#elif defined(O_SEARCH)
                    | O_SEARCH
#elif defined(__FreeBSD__) && defined(O_EXEC)
                    | O_EXEC
#else
                    | O_RDONLY
#endif
      ;

#if !defined(HAVE_OPENAT)
  if (fd != AT_FDCWD) {
    errno = ENOTSUP;
    return (-1);
  } else
    return (open(path, flags));
#else
  return (openat(fd, path, flags));
#endif
}

static int la_verify_filetype(mode_t mode, __LA_MODE_T filetype) {
  int ret = 0;

  switch (filetype) {
  case AE_IFREG:
    ret = (S_ISREG(mode));
    break;
  case AE_IFDIR:
    ret = (S_ISDIR(mode));
    break;
  case AE_IFLNK:
    ret = (S_ISLNK(mode));
    break;
#ifdef S_ISSOCK
  case AE_IFSOCK:
    ret = (S_ISSOCK(mode));
    break;
#endif
  case AE_IFCHR:
    ret = (S_ISCHR(mode));
    break;
  case AE_IFBLK:
    ret = (S_ISBLK(mode));
    break;
  case AE_IFIFO:
    ret = (S_ISFIFO(mode));
    break;
  default:
    break;
  }

  return (ret);
}

static int lazy_stat(struct archive_write_disk *a) {
  if (a->pst != NULL) {

    return (ARCHIVE_OK);
  }
#ifdef HAVE_FSTAT
  if (a->fd >= 0 && fstat(a->fd, &a->st) == 0) {
    a->pst = &a->st;
    return (ARCHIVE_OK);
  }
#endif

#ifdef HAVE_LSTAT
  if (lstat(a->name, &a->st) == 0)
#else
  if (la_stat(a->name, &a->st) == 0)
#endif
  {
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
  const char *linkname;
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
  if (a->entry) {
    archive_entry_free(a->entry);
    a->entry = NULL;
  }
  a->entry = archive_entry_clone(entry);
  a->fd = -1;
  a->fd_offset = 0;
  a->offset = 0;
  a->restore_pwd = -1;
  a->uid = a->user_uid;
  a->mode = archive_entry_mode(a->entry);
  if (archive_entry_size_is_set(a->entry))
    a->filesize = archive_entry_size(a->entry);
  else
    a->filesize = -1;
  archive_strcpy(&(a->_name_data), archive_entry_pathname(a->entry));
  a->name = a->_name_data.s;
  archive_clear_error(&a->archive);

  ret = cleanup_pathname(a);
  if (ret != ARCHIVE_OK)
    return (ret);

  linkname = archive_entry_hardlink(a->entry);
  if (linkname != NULL && strcmp(a->name, linkname) == 0) {
    archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                      "Skipping hardlink pointing to itself: %s", a->name);
    return (ARCHIVE_WARN);
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
  if (a->flags & ARCHIVE_EXTRACT_OWNER)
    a->todo |= TODO_OWNER;
  if (a->flags & ARCHIVE_EXTRACT_TIME)
    a->todo |= TODO_TIMES;
  if (a->flags & ARCHIVE_EXTRACT_ACL) {
#if ARCHIVE_ACL_DARWIN

    size_t metadata_size;

    if ((a->flags & ARCHIVE_EXTRACT_MAC_METADATA) == 0 ||
        archive_entry_mac_metadata(a->entry, &metadata_size) == NULL ||
        metadata_size == 0)
#endif
#if ARCHIVE_ACL_LIBRICHACL

      short extract_acls = 1;
    if (a->flags & ARCHIVE_EXTRACT_XATTR &&
        (archive_entry_acl_types(a->entry) & ARCHIVE_ENTRY_ACL_TYPE_NFS4)) {
      const char *attr_name;
      const void *attr_value;
      size_t attr_size;
      int i = archive_entry_xattr_reset(a->entry);
      while (i--) {
        archive_entry_xattr_next(a->entry, &attr_name, &attr_value, &attr_size);
        if (attr_name != NULL && attr_value != NULL && attr_size > 0 &&
            strcmp(attr_name, "trusted.richacl") == 0) {
          extract_acls = 0;
          break;
        }
      }
    }
    if (extract_acls)
#endif
#if ARCHIVE_ACL_DARWIN || ARCHIVE_ACL_LIBRICHACL
    {
#endif
      if (archive_entry_filetype(a->entry) == AE_IFDIR)
        a->deferred |= TODO_ACLS;
      else
        a->todo |= TODO_ACLS;
#if ARCHIVE_ACL_DARWIN || ARCHIVE_ACL_LIBRICHACL
    }
#endif
  }
  if (a->flags & ARCHIVE_EXTRACT_MAC_METADATA) {
    if (archive_entry_filetype(a->entry) == AE_IFDIR)
      a->deferred |= TODO_MAC_METADATA;
    else
      a->todo |= TODO_MAC_METADATA;
  }
#if defined(__APPLE__) && defined(UF_COMPRESSED) && defined(HAVE_ZLIB_H)
  if ((a->flags & ARCHIVE_EXTRACT_NO_HFS_COMPRESSION) == 0) {
    unsigned long set, clear;
    archive_entry_fflags(a->entry, &set, &clear);
    if ((set & ~clear) & UF_COMPRESSED) {
      a->todo |= TODO_HFS_COMPRESSION;
      a->decmpfs_block_count = (unsigned)-1;
    }
  }
  if ((a->flags & ARCHIVE_EXTRACT_HFS_COMPRESSION_FORCED) != 0 &&
      (a->mode & AE_IFMT) == AE_IFREG && a->filesize > 0) {
    a->todo |= TODO_HFS_COMPRESSION;
    a->decmpfs_block_count = (unsigned)-1;
  }
  {
    const char *p;

    p = strrchr(a->name, '/');
    if (p == NULL)
      p = a->name;
    else
      p++;
    if (p[0] == '.' && p[1] == '_') {

      a->todo &= ~TODO_HFS_COMPRESSION;
      if (a->filesize > 0)
        a->todo |= TODO_APPLEDOUBLE;
    }
  }
#endif

  if (a->flags & ARCHIVE_EXTRACT_XATTR) {
#if ARCHIVE_XATTR_DARWIN

    size_t metadata_size;

    if ((a->flags & ARCHIVE_EXTRACT_MAC_METADATA) == 0 ||
        archive_entry_mac_metadata(a->entry, &metadata_size) == NULL ||
        metadata_size == 0)
#endif
      a->todo |= TODO_XATTR;
  }
  if (a->flags & ARCHIVE_EXTRACT_FFLAGS)
    a->todo |= TODO_FFLAGS;
  if (a->flags & ARCHIVE_EXTRACT_SECURE_SYMLINKS) {
    ret = check_symlinks(a);
    if (ret != ARCHIVE_OK)
      return (ret);
  }
#if defined(HAVE_FCHDIR) && defined(PATH_MAX)

  edit_deep_directories(a);
#endif

  ret = restore_entry(a);

#if defined(__APPLE__) && defined(UF_COMPRESSED) && defined(HAVE_ZLIB_H)

  if (a->todo | TODO_HFS_COMPRESSION) {

    if (a->fd < 0 || fchflags(a->fd, UF_COMPRESSED) != 0)
      a->todo &= ~TODO_HFS_COMPRESSION;
  }
#endif

#ifdef HAVE_FCHDIR

  if (a->restore_pwd >= 0) {
    r = fchdir(a->restore_pwd);
    if (r != 0) {
      archive_set_error(&a->archive, errno, "chdir() failure");
      ret = ARCHIVE_FATAL;
    }
    close(a->restore_pwd);
    a->restore_pwd = -1;
  }
#endif

  if (a->deferred & TODO_MODE) {
    fe = current_fixup(a, archive_entry_pathname(entry));
    if (fe == NULL)
      return (ARCHIVE_FATAL);
    fe->filetype = archive_entry_filetype(entry);
    fe->fixup |= TODO_MODE_BASE;
    fe->mode = a->mode;
  }

  if ((a->deferred & TODO_TIMES) && (archive_entry_mtime_is_set(entry) ||
                                     archive_entry_atime_is_set(entry))) {
    fe = current_fixup(a, archive_entry_pathname(entry));
    if (fe == NULL)
      return (ARCHIVE_FATAL);
    fe->filetype = archive_entry_filetype(entry);
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
    fe = current_fixup(a, archive_entry_pathname(entry));
    if (fe == NULL)
      return (ARCHIVE_FATAL);
    fe->filetype = archive_entry_filetype(entry);
    fe->fixup |= TODO_ACLS;
    archive_acl_copy(&fe->acl, archive_entry_acl(entry));
  }

  if (a->deferred & TODO_MAC_METADATA) {
    const void *metadata;
    size_t metadata_size;
    metadata = archive_entry_mac_metadata(a->entry, &metadata_size);
    if (metadata != NULL && metadata_size > 0) {
      fe = current_fixup(a, archive_entry_pathname(entry));
      if (fe == NULL)
        return (ARCHIVE_FATAL);
      fe->filetype = archive_entry_filetype(entry);
      fe->mac_metadata = malloc(metadata_size);
      if (fe->mac_metadata != NULL) {
        memcpy(fe->mac_metadata, metadata, metadata_size);
        fe->mac_metadata_size = metadata_size;
        fe->fixup |= TODO_MAC_METADATA;
      }
    }
  }

  if (a->deferred & TODO_FFLAGS) {
    fe = current_fixup(a, archive_entry_pathname(entry));
    if (fe == NULL)
      return (ARCHIVE_FATAL);
    fe->filetype = archive_entry_filetype(entry);
    fe->fixup |= TODO_FFLAGS;
  }

  if (ret >= ARCHIVE_WARN)
    a->archive.state = ARCHIVE_STATE_DATA;

  if (a->fd < 0) {
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
  uint64_t start_size = size;
  ssize_t bytes_written = 0;
  ssize_t block_size = 0, bytes_to_write;

  if (size == 0)
    return (ARCHIVE_OK);

  if (a->filesize == 0 || a->fd < 0) {
    archive_set_error(&a->archive, 0, "Attempt to write to an empty file");
    return (ARCHIVE_WARN);
  }

  if (a->flags & ARCHIVE_EXTRACT_SPARSE) {
#if HAVE_STRUCT_STAT_ST_BLKSIZE
    int r;
    if ((r = lazy_stat(a)) != ARCHIVE_OK)
      return (r);
    block_size = a->pst->st_blksize;
#else

    block_size = 16 * 1024;
#endif
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
        bytes_to_write = block_end - a->offset;
    }

    if (a->offset != a->fd_offset) {
      if (lseek(a->fd, a->offset, SEEK_SET) < 0) {
        archive_set_error(&a->archive, errno, "Seek failed");
        return (ARCHIVE_FATAL);
      }
      a->fd_offset = a->offset;
    }
    bytes_written = write(a->fd, buff, bytes_to_write);
    if (bytes_written < 0) {
      archive_set_error(&a->archive, errno, "Write failed");
      return (ARCHIVE_WARN);
    }
    buff += bytes_written;
    size -= bytes_written;
    a->total_bytes_written += bytes_written;
    a->offset += bytes_written;
    a->fd_offset = a->offset;
  }
  return (start_size - size);
}

#if defined(__APPLE__) && defined(UF_COMPRESSED) &&                            \
    defined(HAVE_SYS_XATTR_H) && defined(HAVE_ZLIB_H)

static int hfs_set_compressed_fflag(struct archive_write_disk *a) {
  int r;

  if ((r = lazy_stat(a)) != ARCHIVE_OK)
    return (r);

  a->st.st_flags |= UF_COMPRESSED;
  if (fchflags(a->fd, a->st.st_flags) != 0) {
    archive_set_error(&a->archive, errno,
                      "Failed to set UF_COMPRESSED file flag");
    return (ARCHIVE_WARN);
  }
  return (ARCHIVE_OK);
}

static int hfs_write_decmpfs(struct archive_write_disk *a) {
  int r;
  uint32_t compression_type;

  r = fsetxattr(a->fd, DECMPFS_XATTR_NAME, a->decmpfs_header_p,
                a->decmpfs_attr_size, 0, 0);
  if (r < 0) {
    archive_set_error(&a->archive, errno, "Cannot restore xattr:%s",
                      DECMPFS_XATTR_NAME);
    compression_type =
        archive_le32dec(&a->decmpfs_header_p[DECMPFS_COMPRESSION_TYPE]);
    if (compression_type == CMP_RESOURCE_FORK)
      fremovexattr(a->fd, XATTR_RESOURCEFORK_NAME, XATTR_SHOWCOMPRESSION);
    return (ARCHIVE_WARN);
  }
  return (ARCHIVE_OK);
}

static int hfs_write_resource_fork(struct archive_write_disk *a,
                                   unsigned char *buff, size_t bytes,
                                   uint32_t position) {
  int ret;

  ret = fsetxattr(a->fd, XATTR_RESOURCEFORK_NAME, buff, bytes, position,
                  a->rsrc_xattr_options);
  if (ret < 0) {
    archive_set_error(
        &a->archive, errno, "Cannot restore xattr: %s at %u pos %u bytes",
        XATTR_RESOURCEFORK_NAME, (unsigned)position, (unsigned)bytes);
    return (ARCHIVE_WARN);
  }
  a->rsrc_xattr_options &= ~XATTR_CREATE;
  return (ARCHIVE_OK);
}

static int hfs_write_compressed_data(struct archive_write_disk *a,
                                     size_t bytes_compressed) {
  int ret;

  ret = hfs_write_resource_fork(a, a->compressed_buffer, bytes_compressed,
                                a->compressed_rsrc_position);
  if (ret == ARCHIVE_OK)
    a->compressed_rsrc_position += bytes_compressed;
  return (ret);
}

static int hfs_write_resource_fork_header(struct archive_write_disk *a) {
  unsigned char *buff;
  uint32_t rsrc_bytes;
  uint32_t rsrc_header_bytes;

  buff = a->resource_fork;
  rsrc_bytes = a->compressed_rsrc_position - RSRC_F_SIZE;
  rsrc_header_bytes = RSRC_H_SIZE + 4 + (a->decmpfs_block_count * 8);
  archive_be32enc(buff, 0x100);
  archive_be32enc(buff + 4, rsrc_bytes);
  archive_be32enc(buff + 8, rsrc_bytes - 256);
  archive_be32enc(buff + 12, 0x32);
  memset(buff + 16, 0, 240);
  archive_be32enc(buff + 256, rsrc_bytes - 260);
  return hfs_write_resource_fork(a, buff, rsrc_header_bytes, 0);
}

static size_t hfs_set_resource_fork_footer(unsigned char *buff,
                                           size_t buff_size) {
  static const char rsrc_footer[RSRC_F_SIZE] = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x32, 0x00, 0x00,
      'c',  'm',  'p',  'f',  0x00, 0x00, 0x00, 0x0a, 0x00, 0x01,
      0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  if (buff_size < sizeof(rsrc_footer))
    return (0);
  memcpy(buff, rsrc_footer, sizeof(rsrc_footer));
  return (sizeof(rsrc_footer));
}

static int hfs_reset_compressor(struct archive_write_disk *a) {
  int ret;

  if (a->stream_valid)
    ret = deflateReset(&a->stream);
  else
    ret = deflateInit(&a->stream, a->decmpfs_compression_level);

  if (ret != Z_OK) {
    archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                      "Failed to initialize compressor");
    return (ARCHIVE_FATAL);
  } else
    a->stream_valid = 1;

  return (ARCHIVE_OK);
}

static int hfs_decompress(struct archive_write_disk *a) {
  uint32_t *block_info;
  unsigned int block_count;
  uint32_t data_pos, data_size;
  ssize_t r;
  ssize_t bytes_written, bytes_to_write;
  unsigned char *b;

  block_info = (uint32_t *)(a->resource_fork + RSRC_H_SIZE);
  block_count = archive_le32dec(block_info++);
  while (block_count--) {
    data_pos = RSRC_H_SIZE + archive_le32dec(block_info++);
    data_size = archive_le32dec(block_info++);
    r = fgetxattr(a->fd, XATTR_RESOURCEFORK_NAME, a->compressed_buffer,
                  data_size, data_pos, 0);
    if (r != data_size) {
      archive_set_error(&a->archive, (r < 0) ? errno : ARCHIVE_ERRNO_MISC,
                        "Failed to read resource fork");
      return (ARCHIVE_WARN);
    }
    if (a->compressed_buffer[0] == 0xff) {
      bytes_to_write = data_size - 1;
      b = a->compressed_buffer + 1;
    } else {
      uLong dest_len = MAX_DECMPFS_BLOCK_SIZE;
      int zr;

      zr = uncompress((Bytef *)a->uncompressed_buffer, &dest_len,
                      a->compressed_buffer, data_size);
      if (zr != Z_OK) {
        archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                          "Failed to decompress resource fork");
        return (ARCHIVE_WARN);
      }
      bytes_to_write = dest_len;
      b = (unsigned char *)a->uncompressed_buffer;
    }
    do {
      bytes_written = write(a->fd, b, bytes_to_write);
      if (bytes_written < 0) {
        archive_set_error(&a->archive, errno, "Write failed");
        return (ARCHIVE_WARN);
      }
      bytes_to_write -= bytes_written;
      b += bytes_written;
    } while (bytes_to_write > 0);
  }
  r = fremovexattr(a->fd, XATTR_RESOURCEFORK_NAME, 0);
  if (r == -1) {
    archive_set_error(&a->archive, errno, "Failed to remove resource fork");
    return (ARCHIVE_WARN);
  }
  return (ARCHIVE_OK);
}

static int hfs_drive_compressor(struct archive_write_disk *a, const char *buff,
                                size_t size) {
  unsigned char *buffer_compressed;
  size_t bytes_compressed;
  size_t bytes_used;
  int ret;

  ret = hfs_reset_compressor(a);
  if (ret != ARCHIVE_OK)
    return (ret);

  if (a->compressed_buffer == NULL) {
    size_t block_size;

    block_size = COMPRESSED_W_SIZE + RSRC_F_SIZE +
                 +compressBound(MAX_DECMPFS_BLOCK_SIZE);
    a->compressed_buffer = malloc(block_size);
    if (a->compressed_buffer == NULL) {
      archive_set_error(&a->archive, ENOMEM,
                        "Can't allocate memory for Resource Fork");
      return (ARCHIVE_FATAL);
    }
    a->compressed_buffer_size = block_size;
    a->compressed_buffer_remaining = block_size;
  }

  buffer_compressed = a->compressed_buffer + a->compressed_buffer_size -
                      a->compressed_buffer_remaining;
  a->stream.next_in = (Bytef *)(uintptr_t)(const void *)buff;
  a->stream.avail_in = size;
  a->stream.next_out = buffer_compressed;
  a->stream.avail_out = a->compressed_buffer_remaining;
  do {
    ret = deflate(&a->stream, Z_FINISH);
    switch (ret) {
    case Z_OK:
    case Z_STREAM_END:
      break;
    default:
      archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                        "Failed to compress data");
      return (ARCHIVE_FAILED);
    }
  } while (ret == Z_OK);
  bytes_compressed = a->compressed_buffer_remaining - a->stream.avail_out;

  if (bytes_compressed > size) {
    buffer_compressed[0] = 0xFF;
    memcpy(buffer_compressed + 1, buff, size);
    bytes_compressed = size + 1;
  }
  a->compressed_buffer_remaining -= bytes_compressed;

  if (a->decmpfs_block_count == 1 &&
      (a->decmpfs_attr_size + bytes_compressed) <= MAX_DECMPFS_XATTR_SIZE) {
    archive_le32enc(&a->decmpfs_header_p[DECMPFS_COMPRESSION_TYPE], CMP_XATTR);
    memcpy(a->decmpfs_header_p + DECMPFS_HEADER_SIZE, buffer_compressed,
           bytes_compressed);
    a->decmpfs_attr_size += bytes_compressed;
    a->compressed_buffer_remaining = a->compressed_buffer_size;

    ret = hfs_write_decmpfs(a);
    if (ret == ARCHIVE_OK)
      ret = hfs_set_compressed_fflag(a);
    return (ret);
  }

  archive_le32enc(a->decmpfs_block_info++,
                  a->compressed_rsrc_position_v - RSRC_H_SIZE);
  archive_le32enc(a->decmpfs_block_info++, bytes_compressed);
  a->compressed_rsrc_position_v += bytes_compressed;

  bytes_used = a->compressed_buffer_size - a->compressed_buffer_remaining;
  while (bytes_used >= COMPRESSED_W_SIZE) {
    ret = hfs_write_compressed_data(a, COMPRESSED_W_SIZE);
    if (ret != ARCHIVE_OK)
      return (ret);
    bytes_used -= COMPRESSED_W_SIZE;
    if (bytes_used > COMPRESSED_W_SIZE)
      memmove(a->compressed_buffer, a->compressed_buffer + COMPRESSED_W_SIZE,
              bytes_used);
    else
      memcpy(a->compressed_buffer, a->compressed_buffer + COMPRESSED_W_SIZE,
             bytes_used);
  }
  a->compressed_buffer_remaining = a->compressed_buffer_size - bytes_used;

  if (a->file_remaining_bytes == 0) {
    size_t rsrc_size;
    int64_t bk;

    rsrc_size = hfs_set_resource_fork_footer(a->compressed_buffer + bytes_used,
                                             a->compressed_buffer_remaining);
    ret = hfs_write_compressed_data(a, bytes_used + rsrc_size);
    a->compressed_buffer_remaining = a->compressed_buffer_size;

    bk = HFS_BLOCKS(a->compressed_rsrc_position);
    bk += bk >> 7;
    if (bk > HFS_BLOCKS(a->filesize))
      return hfs_decompress(a);

    if (ret == ARCHIVE_OK)
      ret = hfs_write_resource_fork_header(a);

    if (ret == ARCHIVE_OK)
      ret = hfs_write_decmpfs(a);
    if (ret == ARCHIVE_OK)
      ret = hfs_set_compressed_fflag(a);
  }
  return (ret);
}

static ssize_t hfs_write_decmpfs_block(struct archive_write_disk *a,
                                       const char *buff, size_t size) {
  const char *buffer_to_write;
  size_t bytes_to_write;
  int ret;

  if (a->decmpfs_block_count == (unsigned)-1) {
    void *new_block;
    size_t new_size;
    unsigned int block_count;

    if (a->decmpfs_header_p == NULL) {
      new_block = malloc(MAX_DECMPFS_XATTR_SIZE + sizeof(uint32_t));
      if (new_block == NULL) {
        archive_set_error(&a->archive, ENOMEM,
                          "Can't allocate memory for decmpfs");
        return (ARCHIVE_FATAL);
      }
      a->decmpfs_header_p = new_block;
    }
    a->decmpfs_attr_size = DECMPFS_HEADER_SIZE;
    archive_le32enc(&a->decmpfs_header_p[DECMPFS_COMPRESSION_MAGIC],
                    DECMPFS_MAGIC);
    archive_le32enc(&a->decmpfs_header_p[DECMPFS_COMPRESSION_TYPE],
                    CMP_RESOURCE_FORK);
    archive_le64enc(&a->decmpfs_header_p[DECMPFS_UNCOMPRESSED_SIZE],
                    a->filesize);

    block_count =
        (a->filesize + MAX_DECMPFS_BLOCK_SIZE - 1) / MAX_DECMPFS_BLOCK_SIZE;

    new_size =
        RSRC_H_SIZE + 4 + (block_count * sizeof(uint32_t) * 2) + RSRC_F_SIZE;
    if (new_size > a->resource_fork_allocated_size) {
      new_block = realloc(a->resource_fork, new_size);
      if (new_block == NULL) {
        archive_set_error(&a->archive, ENOMEM,
                          "Can't allocate memory for ResourceFork");
        return (ARCHIVE_FATAL);
      }
      a->resource_fork_allocated_size = new_size;
      a->resource_fork = new_block;
    }

    if (a->uncompressed_buffer == NULL) {
      new_block = malloc(MAX_DECMPFS_BLOCK_SIZE);
      if (new_block == NULL) {
        archive_set_error(&a->archive, ENOMEM,
                          "Can't allocate memory for decmpfs");
        return (ARCHIVE_FATAL);
      }
      a->uncompressed_buffer = new_block;
    }
    a->block_remaining_bytes = MAX_DECMPFS_BLOCK_SIZE;
    a->file_remaining_bytes = a->filesize;
    a->compressed_buffer_remaining = a->compressed_buffer_size;

    a->rsrc_xattr_options = XATTR_CREATE;

    a->decmpfs_block_info = (uint32_t *)(a->resource_fork + RSRC_H_SIZE);

    archive_le32enc(a->decmpfs_block_info++, block_count);

    a->compressed_rsrc_position = RSRC_H_SIZE + 4 + (block_count * 8);
    a->compressed_rsrc_position_v = a->compressed_rsrc_position;
    a->decmpfs_block_count = block_count;
  }

  if (a->file_remaining_bytes == 0)
    return ((ssize_t)size);

  if (size > a->block_remaining_bytes)
    bytes_to_write = a->block_remaining_bytes;
  else
    bytes_to_write = size;

  if (bytes_to_write > a->file_remaining_bytes)
    bytes_to_write = a->file_remaining_bytes;

  if (bytes_to_write == MAX_DECMPFS_BLOCK_SIZE)
    buffer_to_write = buff;
  else {
    memcpy(a->uncompressed_buffer + MAX_DECMPFS_BLOCK_SIZE -
               a->block_remaining_bytes,
           buff, bytes_to_write);
    buffer_to_write = a->uncompressed_buffer;
  }
  a->block_remaining_bytes -= bytes_to_write;
  a->file_remaining_bytes -= bytes_to_write;

  if (a->block_remaining_bytes == 0 || a->file_remaining_bytes == 0) {
    ret = hfs_drive_compressor(
        a, buffer_to_write, MAX_DECMPFS_BLOCK_SIZE - a->block_remaining_bytes);
    if (ret < 0)
      return (ret);
    a->block_remaining_bytes = MAX_DECMPFS_BLOCK_SIZE;
  }

  if (a->file_remaining_bytes == 0)
    return ((ssize_t)size);
  return (bytes_to_write);
}

static ssize_t hfs_write_data_block(struct archive_write_disk *a,
                                    const char *buff, size_t size) {
  uint64_t start_size = size;
  ssize_t bytes_written = 0;
  ssize_t bytes_to_write;

  if (size == 0)
    return (ARCHIVE_OK);

  if (a->filesize == 0 || a->fd < 0) {
    archive_set_error(&a->archive, 0, "Attempt to write to an empty file");
    return (ARCHIVE_WARN);
  }

  if (a->filesize >= 0 && (int64_t)(a->offset + size) > a->filesize)
    start_size = size = (size_t)(a->filesize - a->offset);

  while (size > 0) {
    bytes_to_write = size;

    if (a->offset < a->fd_offset) {

      archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC, "Seek failed");
      return (ARCHIVE_FATAL);
    } else if (a->offset > a->fd_offset) {
      uint64_t skip = a->offset - a->fd_offset;
      char nullblock[1024];

      memset(nullblock, 0, sizeof(nullblock));
      while (skip > 0) {
        if (skip > sizeof(nullblock))
          bytes_written =
              hfs_write_decmpfs_block(a, nullblock, sizeof(nullblock));
        else
          bytes_written = hfs_write_decmpfs_block(a, nullblock, skip);
        if (bytes_written < 0) {
          archive_set_error(&a->archive, errno, "Write failed");
          return (ARCHIVE_WARN);
        }
        skip -= bytes_written;
      }

      a->fd_offset = a->offset;
    }
    bytes_written = hfs_write_decmpfs_block(a, buff, bytes_to_write);
    if (bytes_written < 0)
      return (bytes_written);
    buff += bytes_written;
    size -= bytes_written;
    a->total_bytes_written += bytes_written;
    a->offset += bytes_written;
    a->fd_offset = a->offset;
  }
  return (start_size - size);
}
#else
static ssize_t hfs_write_data_block(struct archive_write_disk *a,
                                    const char *buff, size_t size) {
  return (write_data_block(a, buff, size));
}
#endif

static ssize_t _archive_write_disk_data_block(struct archive *_a,
                                              const void *buff, size_t size,
                                              int64_t offset) {
  struct archive_write_disk *a = (struct archive_write_disk *)_a;
  ssize_t r;

  archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC, ARCHIVE_STATE_DATA,
                      "archive_write_data_block");

  a->offset = offset;
  if (a->todo & TODO_HFS_COMPRESSION)
    r = hfs_write_data_block(a, buff, size);
  else
    r = write_data_block(a, buff, size);
  if (r < ARCHIVE_OK)
    return (r);
  if ((size_t)r < size) {
    archive_set_error(&a->archive, 0,
                      "Too much data: Truncating file at %ju bytes",
                      (uintmax_t)a->filesize);
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

  if (a->todo & TODO_HFS_COMPRESSION)
    return (hfs_write_data_block(a, buff, size));
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

  if (a->fd < 0) {

  } else if (a->filesize < 0) {

  } else if (a->fd_offset == a->filesize) {

#if defined(__APPLE__) && defined(UF_COMPRESSED) && defined(HAVE_ZLIB_H)
  } else if (a->todo & TODO_HFS_COMPRESSION) {
    char null_d[1024];
    ssize_t r;

    if (a->file_remaining_bytes)
      memset(null_d, 0, sizeof(null_d));
    while (a->file_remaining_bytes) {
      if (a->file_remaining_bytes > sizeof(null_d))
        r = hfs_write_data_block(a, null_d, sizeof(null_d));
      else
        r = hfs_write_data_block(a, null_d, a->file_remaining_bytes);
      if (r < 0) {
        close_file_descriptor(a);
        return ((int)r);
      }
    }
#endif
  } else {
#if HAVE_FTRUNCATE
    if (ftruncate(a->fd, a->filesize) == -1 && a->filesize == 0) {
      archive_set_error(&a->archive, errno, "File size could not be restored");
      close_file_descriptor(a);
      return (ARCHIVE_FAILED);
    }
#endif

    a->pst = NULL;
    if ((ret = lazy_stat(a)) != ARCHIVE_OK) {
      close_file_descriptor(a);
      return (ret);
    }

    if (a->st.st_size < a->filesize) {
      const char nul = '\0';
      if (lseek(a->fd, a->filesize - 1, SEEK_SET) < 0) {
        archive_set_error(&a->archive, errno, "Seek failed");
        close_file_descriptor(a);
        return (ARCHIVE_FATAL);
      }
      if (write(a->fd, &nul, 1) < 0) {
        archive_set_error(&a->archive, errno, "Write to restore size failed");
        close_file_descriptor(a);
        return (ARCHIVE_FATAL);
      }
      a->pst = NULL;
    }
  }

  if (a->todo & TODO_APPLEDOUBLE) {
    int r2 = fixup_appledouble(a, a->name);
    if (r2 == ARCHIVE_EOF) {

      goto finish_metadata;
    }
    if (r2 < ret)
      ret = r2;
  }

  if (a->todo & (TODO_OWNER | TODO_SUID | TODO_SGID)) {
    a->uid = archive_write_disk_uid(&a->archive, archive_entry_uname(a->entry),
                                    archive_entry_uid(a->entry));
  }

  if (a->todo & (TODO_OWNER | TODO_SGID | TODO_SUID)) {
    a->gid = archive_write_disk_gid(&a->archive, archive_entry_gname(a->entry),
                                    archive_entry_gid(a->entry));
  }

  if (a->todo & TODO_OWNER) {
    int r2 = set_ownership(a);
    if (r2 < ret)
      ret = r2;
  }

  if (a->user_uid != 0 && (a->todo & TODO_XATTR)) {
    int r2 = set_xattrs(a);
    if (r2 < ret)
      ret = r2;
  }

  if (a->todo & TODO_MODE) {
    int r2 = set_mode(a, a->mode);
    if (r2 < ret)
      ret = r2;
  }

  if (a->user_uid == 0 && (a->todo & TODO_XATTR)) {
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

  if (a->todo & TODO_MAC_METADATA) {
    const void *metadata;
    size_t metadata_size;
    metadata = archive_entry_mac_metadata(a->entry, &metadata_size);
    if (metadata != NULL && metadata_size > 0) {
      int r2 = set_mac_metadata(a, archive_entry_pathname(a->entry), metadata,
                                metadata_size);
      if (r2 < ret)
        ret = r2;
    }
  }

  if (a->todo & TODO_ACLS) {
    int r2;
    r2 = archive_write_disk_set_acls(
        &a->archive, a->fd, archive_entry_pathname(a->entry),
        archive_entry_acl(a->entry), archive_entry_mode(a->entry));
    if (r2 < ret)
      ret = r2;
  }

finish_metadata:

  if (a->fd >= 0) {
    close(a->fd);
    a->fd = -1;
    if (a->tmpname) {
      if (rename(a->tmpname, a->name) == -1) {
        archive_set_error(&a->archive, errno,
                          "Failed to rename temporary file");
        ret = ARCHIVE_FAILED;
        unlink(a->tmpname);
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
#ifdef HAVE_GETEUID
  a->user_uid = geteuid();
#endif
  if (archive_string_ensure(&a->path_safe, 512) == NULL) {
    free(a);
    return (NULL);
  }
  a->path_safe.s[0] = 0;

#ifdef HAVE_ZLIB_H
  a->decmpfs_compression_level = 5;
#endif
  return (&a->archive);
}

#if defined(HAVE_FCHDIR) && defined(PATH_MAX)
static void edit_deep_directories(struct archive_write_disk *a) {
  int ret;
  char *tail = a->name;

  if (strlen(tail) < PATH_MAX)
    return;

  a->restore_pwd = la_opendirat(AT_FDCWD, ".");
  __archive_ensure_cloexec_flag(a->restore_pwd);
  if (a->restore_pwd < 0)
    return;

  while (strlen(tail) >= PATH_MAX) {

    tail += PATH_MAX - 8;
    while (tail > a->name && *tail != '/')
      tail--;

    if (tail <= a->name)
      return;

    *tail = '\0';
    ret = create_dir(a, a->name);
    if (ret == ARCHIVE_OK && chdir(a->name) != 0)
      ret = ARCHIVE_FAILED;
    *tail = '/';
    if (ret != ARCHIVE_OK)
      return;
    tail++;

    a->name = tail;
  }
  return;
}
#endif

static int restore_entry(struct archive_write_disk *a) {
  int ret = ARCHIVE_OK, en;

  if (a->flags & ARCHIVE_EXTRACT_UNLINK && !S_ISDIR(a->mode)) {

    if (a->flags & ARCHIVE_EXTRACT_CLEAR_NOCHANGE_FFLAGS)
      (void)clear_nochange_fflags(a);
    if (unlink(a->name) == 0) {

      a->pst = NULL;
    } else if (errno == ENOENT) {

    } else if (rmdir(a->name) == 0) {

      a->pst = NULL;
    } else {

      archive_set_error(&a->archive, errno, "Could not unlink");
      return (ARCHIVE_FAILED);
    }
  }

  en = create_filesystem_object(a);

  if ((en == ENOTDIR || en == ENOENT) &&
      !(a->flags & ARCHIVE_EXTRACT_NO_AUTODIR)) {

    create_parent_dir(a, a->name);

    en = create_filesystem_object(a);
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

    if (rmdir(a->name) != 0) {
      archive_set_error(&a->archive, errno,
                        "Can't remove already-existing dir");
      return (ARCHIVE_FAILED);
    }
    a->pst = NULL;

    en = create_filesystem_object(a);
  } else if (en == EEXIST) {

    int r = 0;

    if (S_ISDIR(a->mode))
      r = la_stat(a->name, &a->st);

    if (r != 0 || !S_ISDIR(a->mode))
#ifdef HAVE_LSTAT
      r = lstat(a->name, &a->st);
#else
      r = la_stat(a->name, &a->st);
#endif
    if (r != 0) {
      archive_set_error(&a->archive, errno, "Can't stat existing object");
      return (ARCHIVE_FAILED);
    }

    if ((a->flags & ARCHIVE_EXTRACT_NO_OVERWRITE_NEWER) &&
        !S_ISDIR(a->st.st_mode)) {
      if (!older(&(a->st), a->entry)) {
        archive_entry_unset_size(a->entry);
        return (ARCHIVE_OK);
      }
    }

    if (a->skip_file_set && a->st.st_dev == (dev_t)a->skip_file_dev &&
        a->st.st_ino == (ino_t)a->skip_file_ino) {
      archive_set_error(&a->archive, 0, "Refusing to overwrite archive");
      return (ARCHIVE_FAILED);
    }

    if (!S_ISDIR(a->st.st_mode)) {
      if (a->flags & ARCHIVE_EXTRACT_CLEAR_NOCHANGE_FFLAGS)
        (void)clear_nochange_fflags(a);

      if ((a->flags & ARCHIVE_EXTRACT_SAFE_WRITES) && S_ISREG(a->mode)) {

        if ((a->fd = la_mktemp(a)) == -1) {
          archive_set_error(&a->archive, errno, "Can't create temporary file");
          return ARCHIVE_FAILED;
        }
        a->pst = NULL;
        en = 0;
      } else {

        if (unlink(a->name) != 0) {
          archive_set_error(&a->archive, errno,
                            "Can't unlink already-existing "
                            "object");
          return (ARCHIVE_FAILED);
        }
        a->pst = NULL;

        en = create_filesystem_object(a);
      }
    } else if (!S_ISDIR(a->mode)) {

      if (a->flags & ARCHIVE_EXTRACT_CLEAR_NOCHANGE_FFLAGS)
        (void)clear_nochange_fflags(a);
      if (rmdir(a->name) != 0) {
        archive_set_error(
            &a->archive, errno,
            "Can't replace existing directory with non-directory");
        return (ARCHIVE_FAILED);
      }

      en = create_filesystem_object(a);
    } else {

      if ((a->mode != a->st.st_mode) && (a->todo & TODO_MODE_FORCE))
        a->deferred |= (a->todo & TODO_MODE);

      en = 0;
    }
  }

  if (en) {

    if ((&a->archive)->error == NULL)
      archive_set_error(&a->archive, en, "Can't create '%s'", a->name);
    return (ARCHIVE_FAILED);
  }

  a->pst = NULL;
  return (ret);
}

static int create_filesystem_object(struct archive_write_disk *a) {

  const char *linkname;
  mode_t final_mode, mode;
  int r;

  char *linkname_copy;
  struct stat st;
  struct archive_string error_string;
  int error_number;

  linkname = archive_entry_hardlink(a->entry);
  if (linkname != NULL) {
#if !HAVE_LINK
    return (EPERM);
#else
    archive_string_init(&error_string);
    linkname_copy = strdup(linkname);
    if (linkname_copy == NULL) {
      return (EPERM);
    }

    r = cleanup_pathname_fsobj(linkname_copy, &error_number, &error_string,
                               a->flags);
    if (r != ARCHIVE_OK) {
      archive_set_error(&a->archive, error_number, "%s", error_string.s);
      free(linkname_copy);
      archive_string_free(&error_string);

      return (EPERM);
    }
    r = check_symlinks_fsobj(linkname_copy, &error_number, &error_string,
                             a->flags, 1);
    if (r != ARCHIVE_OK) {
      archive_set_error(&a->archive, error_number, "%s", error_string.s);
      free(linkname_copy);
      archive_string_free(&error_string);

      return (EPERM);
    }
    free(linkname_copy);
    archive_string_free(&error_string);

    if (a->flags & ARCHIVE_EXTRACT_SAFE_WRITES)
      unlink(a->name);
#ifdef HAVE_LINKAT
    r = linkat(AT_FDCWD, linkname, AT_FDCWD, a->name, 0) ? errno : 0;
#else
    r = link(linkname, a->name) ? errno : 0;
#endif

    if (r == 0 && a->filesize <= 0) {
      a->todo = 0;
      a->deferred = 0;
    } else if (r == 0 && a->filesize > 0) {
#ifdef HAVE_LSTAT
      r = lstat(a->name, &st);
#else
      r = la_stat(a->name, &st);
#endif
      if (r != 0)
        r = errno;
      else if ((st.st_mode & AE_IFMT) == AE_IFREG) {
        a->fd = open(a->name,
                     O_WRONLY | O_TRUNC | O_BINARY | O_CLOEXEC | O_NOFOLLOW);
        __archive_ensure_cloexec_flag(a->fd);
        if (a->fd < 0)
          r = errno;
      }
    }
    return (r);
#endif
  }
  linkname = archive_entry_symlink(a->entry);
  if (linkname != NULL) {
#if HAVE_SYMLINK

    if (a->flags & ARCHIVE_EXTRACT_SAFE_WRITES)
      unlink(a->name);
    return symlink(linkname, a->name) ? errno : 0;
#else
    return (EPERM);
#endif
  }

  final_mode = a->mode & 07777;

  mode = final_mode & 0777 & ~a->user_umask;

  if (a->user_uid != 0 && a->todo & (TODO_HFS_COMPRESSION | TODO_XATTR)) {
    mode |= 0200;
  }

  switch (a->mode & AE_IFMT) {
  default:

  case AE_IFREG:
    a->tmpname = NULL;
    a->fd =
        open(a->name, O_WRONLY | O_CREAT | O_EXCL | O_BINARY | O_CLOEXEC, mode);
    __archive_ensure_cloexec_flag(a->fd);
    r = (a->fd < 0);
    break;
  case AE_IFCHR:
#ifdef HAVE_MKNOD

    r = mknod(a->name, mode | S_IFCHR, archive_entry_rdev(a->entry));
    break;
#else

    return (EINVAL);
#endif
  case AE_IFBLK:
#ifdef HAVE_MKNOD
    r = mknod(a->name, mode | S_IFBLK, archive_entry_rdev(a->entry));
    break;
#else

    return (EINVAL);
#endif
  case AE_IFDIR:
    mode = (mode | MINIMUM_DIR_MODE) & MAXIMUM_DIR_MODE;
    r = mkdir(a->name, mode);
    if (r == 0) {

      a->deferred |= (a->todo & TODO_TIMES);
      a->todo &= ~TODO_TIMES;

      if ((mode != final_mode) || (a->flags & ARCHIVE_EXTRACT_PERM))
        a->deferred |= (a->todo & TODO_MODE);
      a->todo &= ~TODO_MODE;
    }
    break;
  case AE_IFIFO:
#ifdef HAVE_MKFIFO
    r = mkfifo(a->name, mode);
    break;
#else

    return (EINVAL);
#endif
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
  struct stat st;
  char *c;
  int fd, ret, openflags;

  archive_check_magic(&a->archive, ARCHIVE_WRITE_DISK_MAGIC,
                      ARCHIVE_STATE_HEADER | ARCHIVE_STATE_DATA,
                      "archive_write_disk_close");
  ret = _archive_write_disk_finish_entry(&a->archive);

  p = sort_dir_list(a->fixup_list);

  while (p != NULL) {
    fd = -1;
    a->pst = NULL;

    c = p->name;
    while (*c != '\0')
      c++;
    while (c != p->name && *(c - 1) == '/') {
      c--;
      *c = '\0';
    }

    if (p->fixup == 0)
      goto skip_fixup_entry;
    else {

      openflags = O_BINARY | O_NOFOLLOW | O_RDONLY | O_CLOEXEC;
#if defined(O_DIRECTORY)
      if (p->filetype == AE_IFDIR)
        openflags |= O_DIRECTORY;
#endif
      fd = open(p->name, openflags);

#if defined(O_DIRECTORY)

      if (fd == -1 || p->filetype != AE_IFDIR) {
#if HAVE_FSTAT
        if (fd > 0 && (fstat(fd, &st) != 0 ||
                       la_verify_filetype(st.st_mode, p->filetype) == 0)) {
          goto skip_fixup_entry;
        } else
#endif
            if (
#ifdef HAVE_LSTAT
                lstat(p->name, &st) != 0 ||
#else
            la_stat(p->name, &st) != 0 ||
#endif
                la_verify_filetype(st.st_mode, p->filetype) == 0) {
          goto skip_fixup_entry;
        }
      }
#else
#if HAVE_FSTAT
      if (fd > 0 && (fstat(fd, &st) != 0 ||
                     la_verify_filetype(st.st_mode, p->filetype) == 0)) {
        goto skip_fixup_entry;
      } else
#endif
          if (
#ifdef HAVE_LSTAT
              lstat(p->name, &st) != 0 ||
#else
          la_stat(p->name, &st) != 0 ||
#endif
              la_verify_filetype(st.st_mode, p->filetype) == 0) {
        goto skip_fixup_entry;
      }
#endif
    }
    if (p->fixup & TODO_TIMES) {
      set_times(a, fd, p->mode, p->name, p->atime, p->atime_nanos, p->birthtime,
                p->birthtime_nanos, p->mtime, p->mtime_nanos, p->ctime,
                p->ctime_nanos);
    }
    if (p->fixup & TODO_MODE_BASE) {
#ifdef HAVE_FCHMOD
      if (fd >= 0)
        fchmod(fd, p->mode & 07777);
      else
#endif
#ifdef HAVE_LCHMOD
        lchmod(p->name, p->mode & 07777);
#else
      chmod(p->name, p->mode & 07777);
#endif
    }
    if (p->fixup & TODO_ACLS)
      archive_write_disk_set_acls(&a->archive, fd, p->name, &p->acl, p->mode);
    if (p->fixup & TODO_FFLAGS)
      set_fflags_platform(a, fd, p->name, p->mode, p->fflags_set, 0);
    if (p->fixup & TODO_MAC_METADATA)
      set_mac_metadata(a, p->name, p->mac_metadata, p->mac_metadata_size);
  skip_fixup_entry:
    next = p->next;
    archive_acl_clear(&p->acl);
    free(p->mac_metadata);
    free(p->name);
    if (fd >= 0)
      close(fd);
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
  archive_string_free(&a->_name_data);
  archive_string_free(&a->_tmpname_data);
  archive_string_free(&a->archive.error_string);
  archive_string_free(&a->path_safe);
  a->archive.magic = 0;
  __archive_clean(&a->archive);
  free(a->decmpfs_header_p);
  free(a->resource_fork);
  free(a->compressed_buffer);
  free(a->uncompressed_buffer);
#if defined(__APPLE__) && defined(UF_COMPRESSED) &&                            \
    defined(HAVE_SYS_XATTR_H) && defined(HAVE_ZLIB_H)
  if (a->stream_valid) {
    switch (deflateEnd(&a->stream)) {
    case Z_OK:
      break;
    default:
      archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                        "Failed to clean up compressor");
      ret = ARCHIVE_FATAL;
      break;
    }
  }
#endif
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

  if (strcmp(a->name, b->name) > 0) {
    t = p = a;
    a = a->next;
  } else {
    t = p = b;
    b = b->next;
  }

  while (a != NULL && b != NULL) {
    if (strcmp(a->name, b->name) > 0) {
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
                                     const char *pathname) {
  struct fixup_entry *fe;

  fe = calloc(1, sizeof(struct fixup_entry));
  if (fe == NULL) {
    archive_set_error(&a->archive, ENOMEM, "Can't allocate memory for a fixup");
    return (NULL);
  }
  fe->next = a->fixup_list;
  a->fixup_list = fe;
  fe->fixup = 0;
  fe->filetype = 0;
  fe->name = strdup(pathname);
  return (fe);
}

static struct fixup_entry *current_fixup(struct archive_write_disk *a,
                                         const char *pathname) {
  if (a->current_fixup == NULL)
    a->current_fixup = new_fixup(a, pathname);
  return (a->current_fixup);
}

static void fsobj_error(int *a_eno, struct archive_string *a_estr, int err,
                        const char *errstr, const char *path) {
  if (a_eno)
    *a_eno = err;
  if (a_estr)
    archive_string_sprintf(a_estr, "%s%s", errstr, path);
}

static int check_symlinks_fsobj(char *path, int *a_eno,
                                struct archive_string *a_estr, int flags,
                                int checking_linkname) {
#if !defined(HAVE_LSTAT) &&                                                    \
    !(defined(HAVE_OPENAT) && defined(HAVE_FSTATAT) && defined(HAVE_UNLINKAT))

  (void)path;
  (void)a_eno;
  (void)a_estr;
  (void)flags;
  (void)checking_linkname;
  return (ARCHIVE_OK);
#else
  int res = ARCHIVE_OK;
  char *tail;
  char *head;
  int last;
  char c = '\0';
  int r;
  struct stat st;
  int chdir_fd;
#if defined(HAVE_OPENAT) && defined(HAVE_FSTATAT) && defined(HAVE_UNLINKAT)
  int fd;
#endif

  if (path[0] == '\0')
    return (ARCHIVE_OK);

  chdir_fd = la_opendirat(AT_FDCWD, ".");
  __archive_ensure_cloexec_flag(chdir_fd);
  if (chdir_fd < 0) {
    fsobj_error(a_eno, a_estr, errno, "Could not open ", path);
    return (ARCHIVE_FATAL);
  }
  head = path;
  tail = path;
  last = 0;

  if (tail == path && tail[0] == '/')
    ++tail;

  while (!last) {

    while (*tail == '/')
      ++tail;

    while (*tail != '\0' && *tail != '/')
      ++tail;

    last = (tail[0] == '\0') || (tail[0] == '/' && tail[1] == '\0');

    c = tail[0];
    tail[0] = '\0';

#if defined(HAVE_OPENAT) && defined(HAVE_FSTATAT) && defined(HAVE_UNLINKAT)
    r = fstatat(chdir_fd, head, &st, AT_SYMLINK_NOFOLLOW);
#elif defined(HAVE_LSTAT)
    r = lstat(head, &st);
#else
    r = la_stat(head, &st);
#endif
    if (r != 0) {
      tail[0] = c;

      if (errno == ENOENT) {
        break;
      } else {

        fsobj_error(a_eno, a_estr, errno, "Could not stat ", path);
        res = ARCHIVE_FAILED;
        break;
      }
    } else if (S_ISDIR(st.st_mode)) {
      if (!last) {
#if defined(HAVE_OPENAT) && defined(HAVE_FSTATAT) && defined(HAVE_UNLINKAT)
        fd = la_opendirat(chdir_fd, head);
        if (fd < 0)
          r = -1;
        else {
          r = 0;
          close(chdir_fd);
          chdir_fd = fd;
        }
#else
        r = chdir(head);
#endif
        if (r != 0) {
          tail[0] = c;
          fsobj_error(a_eno, a_estr, errno, "Could not chdir ", path);
          res = (ARCHIVE_FATAL);
          break;
        }

        head = tail + 1;
      }
    } else if (S_ISLNK(st.st_mode)) {
      if (last && checking_linkname) {
#ifdef HAVE_LINKAT

        res = ARCHIVE_OK;
#else

        tail[0] = c;
        fsobj_error(a_eno, a_estr, errno, "Cannot write hardlink to symlink ",
                    path);
        res = ARCHIVE_FAILED;
#endif
        break;
      } else if (last) {

#if defined(HAVE_OPENAT) && defined(HAVE_FSTATAT) && defined(HAVE_UNLINKAT)
        r = unlinkat(chdir_fd, head, 0);
#else
        r = unlink(head);
#endif
        if (r != 0) {
          tail[0] = c;
          fsobj_error(a_eno, a_estr, errno, "Could not remove symlink ", path);
          res = ARCHIVE_FAILED;
          break;
        }

        tail[0] = c;

        res = ARCHIVE_OK;
        break;
      } else if (flags & ARCHIVE_EXTRACT_UNLINK) {

#if defined(HAVE_OPENAT) && defined(HAVE_FSTATAT) && defined(HAVE_UNLINKAT)
        r = unlinkat(chdir_fd, head, 0);
#else
        r = unlink(head);
#endif
        if (r != 0) {
          tail[0] = c;
          fsobj_error(a_eno, a_estr, 0,
                      "Cannot remove intervening "
                      "symlink ",
                      path);
          res = ARCHIVE_FAILED;
          break;
        }
        tail[0] = c;
      } else if ((flags & ARCHIVE_EXTRACT_SECURE_SYMLINKS) == 0) {

#if defined(HAVE_OPENAT) && defined(HAVE_FSTATAT) && defined(HAVE_UNLINKAT)
        r = fstatat(chdir_fd, head, &st, 0);
#else
        r = la_stat(head, &st);
#endif
        if (r != 0) {
          tail[0] = c;
          if (errno == ENOENT) {
            break;
          } else {
            fsobj_error(a_eno, a_estr, errno, "Could not stat ", path);
            res = (ARCHIVE_FAILED);
            break;
          }
        } else if (S_ISDIR(st.st_mode)) {
#if defined(HAVE_OPENAT) && defined(HAVE_FSTATAT) && defined(HAVE_UNLINKAT)
          fd = la_opendirat(chdir_fd, head);
          if (fd < 0)
            r = -1;
          else {
            r = 0;
            close(chdir_fd);
            chdir_fd = fd;
          }
#else
          r = chdir(head);
#endif
          if (r != 0) {
            tail[0] = c;
            fsobj_error(a_eno, a_estr, errno, "Could not chdir ", path);
            res = (ARCHIVE_FATAL);
            break;
          }

          head = tail + 1;
        } else {
          tail[0] = c;
          fsobj_error(a_eno, a_estr, 0,
                      "Cannot extract through "
                      "symlink ",
                      path);
          res = ARCHIVE_FAILED;
          break;
        }
      } else {
        tail[0] = c;
        fsobj_error(a_eno, a_estr, 0, "Cannot extract through symlink ", path);
        res = ARCHIVE_FAILED;
        break;
      }
    }

    tail[0] = c;
    if (tail[0] != '\0')
      tail++;
  }

  tail[0] = c;
#if defined(HAVE_OPENAT) && defined(HAVE_FSTATAT) && defined(HAVE_UNLINKAT)

  if (chdir_fd >= 0)
    close(chdir_fd);
#elif HAVE_FCHDIR

  if (chdir_fd >= 0) {
    r = fchdir(chdir_fd);
    if (r != 0) {
      fsobj_error(a_eno, a_estr, errno, "chdir() failure", "");
    }
    close(chdir_fd);
    chdir_fd = -1;
    if (r != 0) {
      res = (ARCHIVE_FATAL);
    }
  }
#endif

  return res;
#endif
}

static int check_symlinks(struct archive_write_disk *a) {
  struct archive_string error_string;
  int error_number;
  int rc;
  archive_string_init(&error_string);
  rc = check_symlinks_fsobj(a->name, &error_number, &error_string, a->flags, 0);
  if (rc != ARCHIVE_OK) {
    archive_set_error(&a->archive, error_number, "%s", error_string.s);
  }
  archive_string_free(&error_string);
  a->pst = NULL;
  return rc;
}

#if defined(__CYGWIN__)

static void cleanup_pathname_win(char *path) {
  wchar_t wc;
  char *p;
  size_t alen, l;
  int mb, complete, utf8;

  alen = 0;
  mb = 0;
  complete = 1;
  utf8 = (strcmp(nl_langinfo(CODESET), "UTF-8") == 0) ? 1 : 0;
  for (p = path; *p != '\0'; p++) {
    ++alen;
    if (*p == '\\') {

      if (utf8 || !mb)
        *p = '/';
      else
        complete = 0;
    } else if (*(unsigned char *)p > 127)
      mb = 1;
    else
      mb = 0;

    if (*p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' ||
        *p == '>' || *p == '|')
      *p = '_';
  }
  if (complete)
    return;

  p = path;
  while (*p != '\0' && alen) {
    l = mbtowc(&wc, p, alen);
    if (l == (size_t)-1) {
      while (*p != '\0') {
        if (*p == '\\')
          *p = '/';
        ++p;
      }
      break;
    }
    if (l == 1 && wc == L'\\')
      *p = '/';
    p += l;
    alen -= l;
  }
}
#endif

static int cleanup_pathname_fsobj(char *path, int *a_eno,
                                  struct archive_string *a_estr, int flags) {
  char *dest, *src;
  char separator = '\0';

  dest = src = path;
  if (*src == '\0') {
    fsobj_error(a_eno, a_estr, ARCHIVE_ERRNO_MISC, "Invalid empty ",
                "pathname");
    return (ARCHIVE_FAILED);
  }

#if defined(__CYGWIN__)
  cleanup_pathname_win(path);
#endif

  if (*src == '/') {
    if (flags & ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS) {
      fsobj_error(a_eno, a_estr, ARCHIVE_ERRNO_MISC, "Path is ", "absolute");
      return (ARCHIVE_FAILED);
    }

    separator = *src++;
  }

  for (;;) {

    if (src[0] == '\0') {
      break;
    } else if (src[0] == '/') {

      src++;
      continue;
    } else if (src[0] == '.') {
      if (src[1] == '\0') {

        break;
      } else if (src[1] == '/') {

        src += 2;
        continue;
      } else if (src[1] == '.') {
        if (src[2] == '/' || src[2] == '\0') {

          if (flags & ARCHIVE_EXTRACT_SECURE_NODOTDOT) {
            fsobj_error(a_eno, a_estr, ARCHIVE_ERRNO_MISC, "Path contains ",
                        "'..'");
            return (ARCHIVE_FAILED);
          }
        }
      }
    }

    if (separator)
      *dest++ = '/';
    while (*src != '\0' && *src != '/') {
      *dest++ = *src++;
    }

    if (*src == '\0')
      break;

    separator = *src++;
  }

  if (dest == path) {

    if (separator)
      *dest++ = '/';
    else
      *dest++ = '.';
  }

  *dest = '\0';
  return (ARCHIVE_OK);
}

static int cleanup_pathname(struct archive_write_disk *a) {
  struct archive_string error_string;
  int error_number;
  int rc;
  archive_string_init(&error_string);
  rc = cleanup_pathname_fsobj(a->name, &error_number, &error_string, a->flags);
  if (rc != ARCHIVE_OK) {
    archive_set_error(&a->archive, error_number, "%s", error_string.s);
  }
  archive_string_free(&error_string);
  return rc;
}

static int create_parent_dir(struct archive_write_disk *a, char *path) {
  char *slash;
  int r;

  slash = strrchr(path, '/');
  if (slash == NULL)
    return (ARCHIVE_OK);
  *slash = '\0';
  r = create_dir(a, path);
  *slash = '/';
  return (r);
}

static int create_dir(struct archive_write_disk *a, char *path) {
  struct stat st;
  struct fixup_entry *le;
  char *slash, *base;
  mode_t mode_final, mode;
  int r;

  slash = strrchr(path, '/');
  if (slash == NULL)
    base = path;
  else
    base = slash + 1;

  if (base[0] == '\0' || (base[0] == '.' && base[1] == '\0') ||
      (base[0] == '.' && base[1] == '.' && base[2] == '\0')) {

    if (slash != NULL) {
      *slash = '\0';
      r = create_dir(a, path);
      *slash = '/';
      return (r);
    }
    return (ARCHIVE_OK);
  }

  if (la_stat(path, &st) == 0) {
    if (S_ISDIR(st.st_mode))
      return (ARCHIVE_OK);
    if ((a->flags & ARCHIVE_EXTRACT_NO_OVERWRITE)) {
      archive_set_error(&a->archive, EEXIST, "Can't create directory '%s'",
                        path);
      return (ARCHIVE_FAILED);
    }
    if (unlink(path) != 0) {
      archive_set_error(&a->archive, errno,
                        "Can't create directory '%s': "
                        "Conflicting file cannot be removed",
                        path);
      return (ARCHIVE_FAILED);
    }
  } else if (errno != ENOENT && errno != ENOTDIR) {

    archive_set_error(&a->archive, errno, "Can't test directory '%s'", path);
    return (ARCHIVE_FAILED);
  } else if (slash != NULL) {
    *slash = '\0';
    r = create_dir(a, path);
    *slash = '/';
    if (r != ARCHIVE_OK)
      return (r);
  }

  mode_final = DEFAULT_DIR_MODE & ~a->user_umask;

  mode = mode_final;
  mode |= MINIMUM_DIR_MODE;
  mode &= MAXIMUM_DIR_MODE;
  if (mkdir(path, mode) == 0) {
    if (mode != mode_final) {
      le = new_fixup(a, path);
      if (le == NULL)
        return (ARCHIVE_FATAL);
      le->fixup |= TODO_MODE_BASE;
      le->mode = mode_final;
    }
    return (ARCHIVE_OK);
  }

  if (errno == EEXIST) {
    if (la_stat(path, &st) == 0) {
      if (S_ISDIR(st.st_mode))
        return (ARCHIVE_OK);

      errno = ENOTDIR;
    } else {

      errno = EEXIST;
    }
  }

  archive_set_error(&a->archive, errno, "Failed to create dir '%s'", path);
  return (ARCHIVE_FAILED);
}

static int set_ownership(struct archive_write_disk *a) {
#if !defined(__CYGWIN__) && !defined(__linux__)

  if (a->user_uid != 0 && a->user_uid != a->uid) {
    archive_set_error(&a->archive, errno, "Can't set UID=%jd",
                      (intmax_t)a->uid);
    return (ARCHIVE_WARN);
  }
#endif

#ifdef HAVE_FCHOWN

  if (a->fd >= 0 && fchown(a->fd, a->uid, a->gid) == 0) {

    a->todo &= ~(TODO_OWNER | TODO_SGID_CHECK | TODO_SUID_CHECK);
    return (ARCHIVE_OK);
  }
#endif

#ifdef HAVE_LCHOWN
  if (lchown(a->name, a->uid, a->gid) == 0) {

    a->todo &= ~(TODO_OWNER | TODO_SGID_CHECK | TODO_SUID_CHECK);
    return (ARCHIVE_OK);
  }
#elif HAVE_CHOWN
  if (!S_ISLNK(a->mode) && chown(a->name, a->uid, a->gid) == 0) {

    a->todo &= ~(TODO_OWNER | TODO_SGID_CHECK | TODO_SUID_CHECK);
    return (ARCHIVE_OK);
  }
#endif

  archive_set_error(&a->archive, errno, "Can't set user=%jd/group=%jd for %s",
                    (intmax_t)a->uid, (intmax_t)a->gid, a->name);
  return (ARCHIVE_WARN);
}

static int set_time(int fd, int mode, const char *name, time_t atime,
                    long atime_nsec, time_t mtime, long mtime_nsec) {

#if defined(HAVE_UTIMENSAT) && defined(HAVE_FUTIMENS)

  struct timespec ts[2];
  (void)mode;
  ts[0].tv_sec = atime;
  ts[0].tv_nsec = atime_nsec;
  ts[1].tv_sec = mtime;
  ts[1].tv_nsec = mtime_nsec;
  if (fd >= 0)
    return futimens(fd, ts);
  return utimensat(AT_FDCWD, name, ts, AT_SYMLINK_NOFOLLOW);

#elif HAVE_UTIMES

  struct timeval times[2];

  times[0].tv_sec = atime;
  times[0].tv_usec = atime_nsec / 1000;
  times[1].tv_sec = mtime;
  times[1].tv_usec = mtime_nsec / 1000;

#ifdef HAVE_FUTIMES
  if (fd >= 0)
    return (futimes(fd, times));
#else
  (void)fd;
#endif
#ifdef HAVE_LUTIMES
  (void)mode;
  return (lutimes(name, times));
#else
  if (S_ISLNK(mode))
    return (0);
  return (utimes(name, times));
#endif

#elif defined(HAVE_UTIME)

  struct utimbuf times;
  (void)fd;
  (void)name;
  (void)atime_nsec;
  (void)mtime_nsec;
  times.actime = atime;
  times.modtime = mtime;
  if (S_ISLNK(mode))
    return (ARCHIVE_OK);
  return (utime(name, &times));

#else

  (void)fd;
  (void)mode;
  (void)name;
  (void)atime;
  (void)atime_nsec;
  (void)mtime;
  (void)mtime_nsec;
  return (ARCHIVE_WARN);
#endif
}

#ifdef F_SETTIMES
static int set_time_tru64(int fd, int mode, const char *name, time_t atime,
                          long atime_nsec, time_t mtime, long mtime_nsec,
                          time_t ctime, long ctime_nsec) {
  struct attr_timbuf tstamp;
  tstamp.atime.tv_sec = atime;
  tstamp.mtime.tv_sec = mtime;
  tstamp.ctime.tv_sec = ctime;
#if defined(__hpux) && (defined(__ia64) || defined(__hppa))
  tstamp.atime.tv_nsec = atime_nsec;
  tstamp.mtime.tv_nsec = mtime_nsec;
  tstamp.ctime.tv_nsec = ctime_nsec;
#else
  tstamp.atime.tv_usec = atime_nsec / 1000;
  tstamp.mtime.tv_usec = mtime_nsec / 1000;
  tstamp.ctime.tv_usec = ctime_nsec / 1000;
#endif
  return (fcntl(fd, F_SETTIMES, &tstamp));
}
#endif

static int set_times(struct archive_write_disk *a, int fd, int mode,
                     const char *name, time_t atime, long atime_nanos,
                     time_t birthtime, long birthtime_nanos, time_t mtime,
                     long mtime_nanos, time_t cctime, long ctime_nanos) {

  int r1 = 0, r2 = 0;

#ifdef F_SETTIMES

  if (a->user_uid == 0 &&
      set_time_tru64(fd, mode, name, atime, atime_nanos, mtime, mtime_nanos,
                     cctime, ctime_nanos) == 0) {
    return (ARCHIVE_OK);
  }
#else
  (void)cctime;
  (void)ctime_nanos;
#endif

#ifdef HAVE_STRUCT_STAT_ST_BIRTHTIME

  if (birthtime < mtime ||
      (birthtime == mtime && birthtime_nanos < mtime_nanos))
    r1 = set_time(fd, mode, name, atime, atime_nanos, birthtime,
                  birthtime_nanos);
#else
  (void)birthtime;
  (void)birthtime_nanos;
#endif
  r2 = set_time(fd, mode, name, atime, atime_nanos, mtime, mtime_nanos);
  if (r1 != 0 || r2 != 0) {
    archive_set_error(&a->archive, errno, "Can't restore time");
    return (ARCHIVE_WARN);
  }
  return (ARCHIVE_OK);
}

static int set_times_from_entry(struct archive_write_disk *a) {
  time_t atime, birthtime, mtime, cctime;
  long atime_nsec, birthtime_nsec, mtime_nsec, ctime_nsec;

  atime = birthtime = mtime = cctime = a->start_time;
  atime_nsec = birthtime_nsec = mtime_nsec = ctime_nsec = 0;

  if (!archive_entry_atime_is_set(a->entry)
#if HAVE_STRUCT_STAT_ST_BIRTHTIME
      && !archive_entry_birthtime_is_set(a->entry)
#endif
      && !archive_entry_mtime_is_set(a->entry))
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
    cctime = archive_entry_ctime(a->entry);
    ctime_nsec = archive_entry_ctime_nsec(a->entry);
  }

  return set_times(a, a->fd, a->mode, a->name, atime, atime_nsec, birthtime,
                   birthtime_nsec, mtime, mtime_nsec, cctime, ctime_nsec);
}

static int set_mode(struct archive_write_disk *a, int mode) {
  int r = ARCHIVE_OK;
  int r2;
  mode &= 07777;

  if (a->todo & TODO_SGID_CHECK) {

    if ((r = lazy_stat(a)) != ARCHIVE_OK)
      return (r);
    if (a->pst->st_gid != a->gid) {
      mode &= ~S_ISGID;
      if (a->flags & ARCHIVE_EXTRACT_OWNER) {

        archive_set_error(&a->archive, -1, "Can't restore SGID bit");
        r = ARCHIVE_WARN;
      }
    }

    if (a->pst->st_uid != a->uid && (a->todo & TODO_SUID)) {
      mode &= ~S_ISUID;
      if (a->flags & ARCHIVE_EXTRACT_OWNER) {
        archive_set_error(&a->archive, -1, "Can't restore SUID bit");
        r = ARCHIVE_WARN;
      }
    }
    a->todo &= ~TODO_SGID_CHECK;
    a->todo &= ~TODO_SUID_CHECK;
  } else if (a->todo & TODO_SUID_CHECK) {

    if (a->user_uid != a->uid) {
      mode &= ~S_ISUID;
      if (a->flags & ARCHIVE_EXTRACT_OWNER) {
        archive_set_error(&a->archive, -1, "Can't make file SUID");
        r = ARCHIVE_WARN;
      }
    }
    a->todo &= ~TODO_SUID_CHECK;
  }

  if (S_ISLNK(a->mode)) {
#ifdef HAVE_LCHMOD

    if (lchmod(a->name, (mode_t)mode) != 0) {
      switch (errno) {
      case ENOTSUP:
      case ENOSYS:
#if ENOTSUP != EOPNOTSUPP
      case EOPNOTSUPP:
#endif

        break;
      default:
        archive_set_error(&a->archive, errno, "Can't set permissions to 0%o",
                          (unsigned int)mode);
        r = ARCHIVE_WARN;
      }
    }
#endif
  } else if (!S_ISDIR(a->mode)) {

#ifdef HAVE_FCHMOD
    if (a->fd >= 0)
      r2 = fchmod(a->fd, (mode_t)mode);
    else
#endif

      r2 = chmod(a->name, (mode_t)mode);

    if (r2 != 0) {
      archive_set_error(&a->archive, errno, "Can't set permissions to 0%o",
                        (unsigned int)mode);
      r = ARCHIVE_WARN;
    }
  }
  return (r);
}

static int set_fflags(struct archive_write_disk *a) {
  struct fixup_entry *le;
  unsigned long set, clear;
  int r;
  mode_t mode = archive_entry_mode(a->entry);

  const int critical_flags = 0
#ifdef SF_IMMUTABLE
                             | SF_IMMUTABLE
#endif
#ifdef UF_IMMUTABLE
                             | UF_IMMUTABLE
#endif
#ifdef SF_APPEND
                             | SF_APPEND
#endif
#ifdef UF_APPEND
                             | UF_APPEND
#endif
#if defined(FS_APPEND_FL)
                             | FS_APPEND_FL
#elif defined(EXT2_APPEND_FL)
                             | EXT2_APPEND_FL
#endif
#if defined(FS_IMMUTABLE_FL)
                             | FS_IMMUTABLE_FL
#elif defined(EXT2_IMMUTABLE_FL)
                             | EXT2_IMMUTABLE_FL
#endif
#ifdef FS_JOURNAL_DATA_FL
                             | FS_JOURNAL_DATA_FL
#endif
      ;

  if (a->todo & TODO_FFLAGS) {
    archive_entry_fflags(a->entry, &set, &clear);

    if ((critical_flags != 0) && (set & critical_flags)) {
      le = current_fixup(a, a->name);
      if (le == NULL)
        return (ARCHIVE_FATAL);
      le->filetype = archive_entry_filetype(a->entry);
      le->fixup |= TODO_FFLAGS;
      le->fflags_set = set;

      if ((le->fixup & TODO_MODE) == 0)
        le->mode = mode;
    } else {
      r = set_fflags_platform(a, a->fd, a->name, mode, set, clear);
      if (r != ARCHIVE_OK)
        return (r);
    }
  }
  return (ARCHIVE_OK);
}

static int clear_nochange_fflags(struct archive_write_disk *a) {
  mode_t mode = archive_entry_mode(a->entry);
  const int nochange_flags = 0
#ifdef SF_IMMUTABLE
                             | SF_IMMUTABLE
#endif
#ifdef UF_IMMUTABLE
                             | UF_IMMUTABLE
#endif
#ifdef SF_APPEND
                             | SF_APPEND
#endif
#ifdef UF_APPEND
                             | UF_APPEND
#endif
#if defined(FS_APPEND_FL)
                             | FS_APPEND_FL
#elif defined(EXT2_APPEND_FL)
                             | EXT2_APPEND_FL
#endif
#if defined(FS_IMMUTABLE_FL)
                             | FS_IMMUTABLE_FL
#elif defined(EXT2_IMMUTABLE_FL)
                             | EXT2_IMMUTABLE_FL
#endif
      ;

  return (set_fflags_platform(a, a->fd, a->name, mode, 0, nochange_flags));
}

#if (defined(HAVE_LCHFLAGS) || defined(HAVE_CHFLAGS) ||                        \
     defined(HAVE_FCHFLAGS)) &&                                                \
    defined(HAVE_STRUCT_STAT_ST_FLAGS)

static int set_fflags_platform(struct archive_write_disk *a, int fd,
                               const char *name, mode_t mode, unsigned long set,
                               unsigned long clear) {
  int r;
  const int sf_mask = 0
#ifdef SF_APPEND
                      | SF_APPEND
#endif
#ifdef SF_ARCHIVED
                      | SF_ARCHIVED
#endif
#ifdef SF_IMMUTABLE
                      | SF_IMMUTABLE
#endif
#ifdef SF_NOUNLINK
                      | SF_NOUNLINK
#endif
      ;
  (void)mode;

  if (set == 0 && clear == 0)
    return (ARCHIVE_OK);

  if ((r = lazy_stat(a)) != ARCHIVE_OK)
    return (r);

  a->st.st_flags &= ~clear;
  a->st.st_flags |= set;

  if (a->user_uid != 0)
    a->st.st_flags &= ~sf_mask;

#ifdef HAVE_FCHFLAGS

  if (fd >= 0 && fchflags(fd, a->st.st_flags) == 0)
    return (ARCHIVE_OK);
#endif

#ifdef HAVE_LCHFLAGS
  if (lchflags(name, a->st.st_flags) == 0)
    return (ARCHIVE_OK);
#elif defined(HAVE_CHFLAGS)
  if (S_ISLNK(a->st.st_mode)) {
    archive_set_error(&a->archive, errno, "Can't set file flags on symlink.");
    return (ARCHIVE_WARN);
  }
  if (chflags(name, a->st.st_flags) == 0)
    return (ARCHIVE_OK);
#endif
  archive_set_error(&a->archive, errno, "Failed to set file flags");
  return (ARCHIVE_WARN);
}

#elif (defined(FS_IOC_GETFLAGS) && defined(FS_IOC_SETFLAGS) &&                 \
       defined(HAVE_WORKING_FS_IOC_GETFLAGS)) ||                               \
    (defined(EXT2_IOC_GETFLAGS) && defined(EXT2_IOC_SETFLAGS) &&               \
     defined(HAVE_WORKING_EXT2_IOC_GETFLAGS))

static int set_fflags_platform(struct archive_write_disk *a, int fd,
                               const char *name, mode_t mode, unsigned long set,
                               unsigned long clear) {
  int ret;
  int myfd = fd;
  int newflags, oldflags;

  const int sf_mask = 0
#if defined(FS_IMMUTABLE_FL)
                      | FS_IMMUTABLE_FL
#elif defined(EXT2_IMMUTABLE_FL)
                      | EXT2_IMMUTABLE_FL
#endif
#if defined(FS_APPEND_FL)
                      | FS_APPEND_FL
#elif defined(EXT2_APPEND_FL)
                      | EXT2_APPEND_FL
#endif
#if defined(FS_JOURNAL_DATA_FL)
                      | FS_JOURNAL_DATA_FL
#endif
      ;

  if (set == 0 && clear == 0)
    return (ARCHIVE_OK);

  if (!S_ISREG(mode) && !S_ISDIR(mode))
    return (ARCHIVE_OK);

  if (myfd < 0) {
    myfd =
        open(name, O_RDONLY | O_NONBLOCK | O_BINARY | O_CLOEXEC | O_NOFOLLOW);
    __archive_ensure_cloexec_flag(myfd);
  }
  if (myfd < 0)
    return (ARCHIVE_OK);

  ret = ARCHIVE_OK;

  if (ioctl(myfd,
#ifdef FS_IOC_GETFLAGS
            FS_IOC_GETFLAGS,
#else
            EXT2_IOC_GETFLAGS,
#endif
            &oldflags) < 0)
    goto fail;

  newflags = (oldflags & ~clear) | set;
  if (ioctl(myfd,
#ifdef FS_IOC_SETFLAGS
            FS_IOC_SETFLAGS,
#else
            EXT2_IOC_SETFLAGS,
#endif
            &newflags) >= 0)
    goto cleanup;
  if (errno != EPERM)
    goto fail;

  newflags &= ~sf_mask;
  oldflags &= sf_mask;
  newflags |= oldflags;
  if (ioctl(myfd,
#ifdef FS_IOC_SETFLAGS
            FS_IOC_SETFLAGS,
#else
            EXT2_IOC_SETFLAGS,
#endif
            &newflags) >= 0)
    goto cleanup;

fail:
  archive_set_error(&a->archive, errno, "Failed to set file flags");
  ret = ARCHIVE_WARN;
cleanup:
  if (fd < 0)
    close(myfd);
  return (ret);
}

#else

static int set_fflags_platform(struct archive_write_disk *a, int fd,
                               const char *name, mode_t mode, unsigned long set,
                               unsigned long clear) {
  (void)a;
  (void)fd;
  (void)name;
  (void)mode;
  (void)set;
  (void)clear;
  return (ARCHIVE_OK);
}

#endif

#ifndef HAVE_COPYFILE_H

static int set_mac_metadata(struct archive_write_disk *a, const char *pathname,
                            const void *metadata, size_t metadata_size) {
  (void)a;
  (void)pathname;
  (void)metadata;
  (void)metadata_size;
  return (ARCHIVE_OK);
}

static int fixup_appledouble(struct archive_write_disk *a,
                             const char *pathname) {
  (void)a;
  (void)pathname;
  return (ARCHIVE_OK);
}
#else

#if defined(HAVE_SYS_XATTR_H)
static int copy_xattrs(struct archive_write_disk *a, int tmpfd, int dffd) {
  ssize_t xattr_size;
  char *xattr_names = NULL, *xattr_val = NULL;
  int ret = ARCHIVE_OK, xattr_i;

  xattr_size = flistxattr(tmpfd, NULL, 0, 0);
  if (xattr_size == -1) {
    archive_set_error(&a->archive, errno, "Failed to read metadata(xattr)");
    ret = ARCHIVE_WARN;
    goto exit_xattr;
  }
  xattr_names = malloc(xattr_size);
  if (xattr_names == NULL) {
    archive_set_error(&a->archive, ENOMEM,
                      "Can't allocate memory for metadata(xattr)");
    ret = ARCHIVE_FATAL;
    goto exit_xattr;
  }
  xattr_size = flistxattr(tmpfd, xattr_names, xattr_size, 0);
  if (xattr_size == -1) {
    archive_set_error(&a->archive, errno, "Failed to read metadata(xattr)");
    ret = ARCHIVE_WARN;
    goto exit_xattr;
  }
  for (xattr_i = 0; xattr_i < xattr_size;
       xattr_i += strlen(xattr_names + xattr_i) + 1) {
    char *p;
    ssize_t s;
    int f;

    s = fgetxattr(tmpfd, xattr_names + xattr_i, NULL, 0, 0, 0);
    if (s == -1) {
      archive_set_error(&a->archive, errno, "Failed to get metadata(xattr)");
      ret = ARCHIVE_WARN;
      goto exit_xattr;
    }
    p = realloc(xattr_val, s);
    if (p == NULL) {
      archive_set_error(&a->archive, ENOMEM, "Failed to get metadata(xattr)");
      ret = ARCHIVE_WARN;
      goto exit_xattr;
    }
    xattr_val = p;
    s = fgetxattr(tmpfd, xattr_names + xattr_i, xattr_val, s, 0, 0);
    if (s == -1) {
      archive_set_error(&a->archive, errno, "Failed to get metadata(xattr)");
      ret = ARCHIVE_WARN;
      goto exit_xattr;
    }
    f = fsetxattr(dffd, xattr_names + xattr_i, xattr_val, s, 0, 0);
    if (f == -1) {
      archive_set_error(&a->archive, errno, "Failed to get metadata(xattr)");
      ret = ARCHIVE_WARN;
      goto exit_xattr;
    }
  }
exit_xattr:
  free(xattr_names);
  free(xattr_val);
  return (ret);
}
#endif

static int copy_acls(struct archive_write_disk *a, int tmpfd, int dffd) {
#ifndef HAVE_SYS_ACL_H
  return 0;
#else
  acl_t acl, dfacl = NULL;
  int acl_r, ret = ARCHIVE_OK;

  acl = acl_get_fd(tmpfd);
  if (acl == NULL) {
    if (errno == ENOENT)

      return (ret);
    archive_set_error(&a->archive, errno, "Failed to get metadata(acl)");
    ret = ARCHIVE_WARN;
    goto exit_acl;
  }
  dfacl = acl_dup(acl);
  acl_r = acl_set_fd(dffd, dfacl);
  if (acl_r == -1) {
    archive_set_error(&a->archive, errno, "Failed to get metadata(acl)");
    ret = ARCHIVE_WARN;
    goto exit_acl;
  }
exit_acl:
  if (acl)
    acl_free(acl);
  if (dfacl)
    acl_free(dfacl);
  return (ret);
#endif
}

static int create_tempdatafork(struct archive_write_disk *a,
                               const char *pathname) {
  struct archive_string tmpdatafork;
  int tmpfd;

  archive_string_init(&tmpdatafork);
  archive_strcpy(&tmpdatafork, pathname);
  archive_string_dirname(&tmpdatafork);
  archive_strcat(&tmpdatafork, "/tar.XXXXXXXX");
  tmpfd = __archive_mkstemp(tmpdatafork.s);
  if (tmpfd < 0) {
    archive_set_error(&a->archive, errno, "Failed to mkstemp");
    archive_string_free(&tmpdatafork);
    return (-1);
  }
  if (copyfile(pathname, tmpdatafork.s, 0,
               COPYFILE_UNPACK | COPYFILE_NOFOLLOW | COPYFILE_ACL |
                   COPYFILE_XATTR) < 0) {
    archive_set_error(&a->archive, errno, "Failed to restore metadata");
    close(tmpfd);
    tmpfd = -1;
  }
  unlink(tmpdatafork.s);
  archive_string_free(&tmpdatafork);
  return (tmpfd);
}

static int copy_metadata(struct archive_write_disk *a, const char *metadata,
                         const char *datafork, int datafork_compressed) {
  int ret = ARCHIVE_OK;

  if (datafork_compressed) {
    int dffd, tmpfd;

    tmpfd = create_tempdatafork(a, metadata);
    if (tmpfd == -1)
      return (ARCHIVE_WARN);

    dffd = open(datafork, 0);
    if (dffd == -1) {
      archive_set_error(&a->archive, errno,
                        "Failed to open the data fork for metadata");
      close(tmpfd);
      return (ARCHIVE_WARN);
    }

#if defined(HAVE_SYS_XATTR_H)
    ret = copy_xattrs(a, tmpfd, dffd);
    if (ret == ARCHIVE_OK)
#endif
      ret = copy_acls(a, tmpfd, dffd);
    close(tmpfd);
    close(dffd);
  } else {
    if (copyfile(metadata, datafork, 0,
                 COPYFILE_UNPACK | COPYFILE_NOFOLLOW | COPYFILE_ACL |
                     COPYFILE_XATTR) < 0) {
      archive_set_error(&a->archive, errno, "Failed to restore metadata");
      ret = ARCHIVE_WARN;
    }
  }
  return (ret);
}

static int set_mac_metadata(struct archive_write_disk *a, const char *pathname,
                            const void *metadata, size_t metadata_size) {
  struct archive_string tmp;
  ssize_t written;
  int fd;
  int ret = ARCHIVE_OK;

  archive_string_init(&tmp);
  archive_strcpy(&tmp, pathname);
  archive_string_dirname(&tmp);
  archive_strcat(&tmp, "/tar.XXXXXXXX");
  fd = __archive_mkstemp(tmp.s);

  if (fd < 0) {
    archive_set_error(&a->archive, errno, "Failed to restore metadata");
    archive_string_free(&tmp);
    return (ARCHIVE_WARN);
  }
  written = write(fd, metadata, metadata_size);
  close(fd);
  if ((size_t)written != metadata_size) {
    archive_set_error(&a->archive, errno, "Failed to restore metadata");
    ret = ARCHIVE_WARN;
  } else {
    int compressed;

#if defined(UF_COMPRESSED)
    if ((a->todo & TODO_HFS_COMPRESSION) != 0 &&
        (ret = lazy_stat(a)) == ARCHIVE_OK)
      compressed = a->st.st_flags & UF_COMPRESSED;
    else
#endif
      compressed = 0;
    ret = copy_metadata(a, tmp.s, pathname, compressed);
  }
  unlink(tmp.s);
  archive_string_free(&tmp);
  return (ret);
}

static int fixup_appledouble(struct archive_write_disk *a,
                             const char *pathname) {
  char buff[8];
  struct stat st;
  const char *p;
  struct archive_string datafork;
  int fd = -1, ret = ARCHIVE_OK;

  archive_string_init(&datafork);

  p = strrchr(pathname, '/');
  if (p == NULL)
    p = pathname;
  else
    p++;
  if (p[0] != '.' || p[1] != '_')
    goto skip_appledouble;

  archive_strncpy(&datafork, pathname, p - pathname);
  archive_strcat(&datafork, p + 2);
  if (
#ifdef HAVE_LSTAT
      lstat(datafork.s, &st) == -1 ||
#else
      la_stat(datafork.s, &st) == -1 ||
#endif
      (((st.st_mode & AE_IFMT) != AE_IFREG) &&
       ((st.st_mode & AE_IFMT) != AE_IFDIR)))
    goto skip_appledouble;

  fd = open(pathname, O_RDONLY | O_BINARY | O_CLOEXEC);
  __archive_ensure_cloexec_flag(fd);
  if (fd == -1) {
    archive_set_error(&a->archive, errno, "Failed to open a restoring file");
    ret = ARCHIVE_WARN;
    goto skip_appledouble;
  }
  if (read(fd, buff, 8) == -1) {
    archive_set_error(&a->archive, errno, "Failed to read a restoring file");
    close(fd);
    ret = ARCHIVE_WARN;
    goto skip_appledouble;
  }
  close(fd);

  if (archive_be32dec(buff) != 0x00051607)
    goto skip_appledouble;

  if (archive_be32dec(buff + 4) != 0x00020000)
    goto skip_appledouble;

  ret = copy_metadata(a, pathname, datafork.s,
#if defined(UF_COMPRESSED)
                      st.st_flags & UF_COMPRESSED);
#else
                      0);
#endif
  if (ret == ARCHIVE_OK) {
    unlink(pathname);
    ret = ARCHIVE_EOF;
  }
skip_appledouble:
  archive_string_free(&datafork);
  return (ret);
}
#endif

#if ARCHIVE_XATTR_LINUX || ARCHIVE_XATTR_DARWIN || ARCHIVE_XATTR_AIX

static int set_xattrs(struct archive_write_disk *a) {
  struct archive_entry *entry = a->entry;
  struct archive_string errlist;
  int ret = ARCHIVE_OK;
  int i = archive_entry_xattr_reset(entry);
  short fail = 0;

  archive_string_init(&errlist);

  while (i--) {
    const char *name;
    const void *value;
    size_t size;
    int e;

    archive_entry_xattr_next(entry, &name, &value, &size);

    if (name == NULL)
      continue;
#if ARCHIVE_XATTR_LINUX

    if (strncmp(name, "system.", 7) == 0 &&
        (strcmp(name + 7, "posix_acl_access") == 0 ||
         strcmp(name + 7, "posix_acl_default") == 0))
      continue;
    if (strncmp(name, "trusted.SGI_", 12) == 0 &&
        (strcmp(name + 12, "ACL_DEFAULT") == 0 ||
         strcmp(name + 12, "ACL_FILE") == 0))
      continue;

    if (strncmp(name, "xfsroot.", 8) == 0) {
      fail = 1;
      archive_strcat(&errlist, name);
      archive_strappend_char(&errlist, ' ');
      continue;
    }
#endif

    if (a->fd >= 0) {
#if ARCHIVE_XATTR_LINUX
      e = fsetxattr(a->fd, name, value, size, 0);
#elif ARCHIVE_XATTR_DARWIN
      e = fsetxattr(a->fd, name, value, size, 0, 0);
#elif ARCHIVE_XATTR_AIX
      e = fsetea(a->fd, name, value, size, 0);
#endif
    } else {
#if ARCHIVE_XATTR_LINUX
      e = lsetxattr(archive_entry_pathname(entry), name, value, size, 0);
#elif ARCHIVE_XATTR_DARWIN
      e = setxattr(archive_entry_pathname(entry), name, value, size, 0,
                   XATTR_NOFOLLOW);
#elif ARCHIVE_XATTR_AIX
      e = lsetea(archive_entry_pathname(entry), name, value, size, 0);
#endif
    }
    if (e == -1) {
      ret = ARCHIVE_WARN;
      archive_strcat(&errlist, name);
      archive_strappend_char(&errlist, ' ');
      if (errno != ENOTSUP && errno != ENOSYS)
        fail = 1;
    }
  }

  if (ret == ARCHIVE_WARN) {
    if (fail && errlist.length > 0) {
      errlist.length--;
      errlist.s[errlist.length] = '\0';
      archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                        "Cannot restore extended attributes: %s", errlist.s);
    } else
      archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                        "Cannot restore extended "
                        "attributes on this file system.");
  }

  archive_string_free(&errlist);
  return (ret);
}
#elif ARCHIVE_XATTR_FREEBSD

static int set_xattrs(struct archive_write_disk *a) {
  struct archive_entry *entry = a->entry;
  struct archive_string errlist;
  int ret = ARCHIVE_OK;
  int i = archive_entry_xattr_reset(entry);
  short fail = 0;

  archive_string_init(&errlist);

  while (i--) {
    const char *name;
    const void *value;
    size_t size;
    archive_entry_xattr_next(entry, &name, &value, &size);
    if (name != NULL) {
      int e;
      int namespace;

      namespace = EXTATTR_NAMESPACE_USER;

      if (strncmp(name, "user.", 5) == 0) {

        name += 5;
        namespace = EXTATTR_NAMESPACE_USER;
      } else if (strncmp(name, "system.", 7) == 0) {
        name += 7;
        namespace = EXTATTR_NAMESPACE_SYSTEM;
        if (!strcmp(name, "nfs4.acl") || !strcmp(name, "posix1e.acl_access") ||
            !strcmp(name, "posix1e.acl_default"))
          continue;
      } else {

        archive_strcat(&errlist, name);
        archive_strappend_char(&errlist, ' ');
        fail = 1;
        ret = ARCHIVE_WARN;
        continue;
      }

      if (a->fd >= 0) {

        errno = 0;
        e = extattr_set_fd(a->fd, namespace, name, value, size);
        if (e == 0 && errno == 0) {
          e = size;
        }
      } else {
        e = extattr_set_link(archive_entry_pathname(entry), namespace, name,
                             value, size);
      }
      if (e != (int)size) {
        archive_strcat(&errlist, name);
        archive_strappend_char(&errlist, ' ');
        ret = ARCHIVE_WARN;
        if (errno != ENOTSUP && errno != ENOSYS)
          fail = 1;
      }
    }
  }

  if (ret == ARCHIVE_WARN) {
    if (fail && errlist.length > 0) {
      errlist.length--;
      errlist.s[errlist.length] = '\0';

      archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                        "Cannot restore extended attributes: %s", errlist.s);
    } else
      archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                        "Cannot restore extended "
                        "attributes on this file system.");
  }

  archive_string_free(&errlist);
  return (ret);
}
#else

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
#endif

static int older(struct stat *st, struct archive_entry *entry) {

  if (to_int64_time(st->st_mtime) < to_int64_time(archive_entry_mtime(entry)))
    return (1);

  if (to_int64_time(st->st_mtime) > to_int64_time(archive_entry_mtime(entry)))
    return (0);

#if HAVE_STRUCT_STAT_ST_MTIMESPEC_TV_NSEC

  if (st->st_mtimespec.tv_nsec < archive_entry_mtime_nsec(entry))
    return (1);
#elif HAVE_STRUCT_STAT_ST_MTIM_TV_NSEC

  if (st->st_mtim.tv_nsec < archive_entry_mtime_nsec(entry))
    return (1);
#elif HAVE_STRUCT_STAT_ST_MTIME_N

  if (st->st_mtime_n < archive_entry_mtime_nsec(entry))
    return (1);
#elif HAVE_STRUCT_STAT_ST_UMTIME

  if (st->st_umtime * 1000 < archive_entry_mtime_nsec(entry))
    return (1);
#elif HAVE_STRUCT_STAT_ST_MTIME_USEC

  if (st->st_mtime_usec * 1000 < archive_entry_mtime_nsec(entry))
    return (1);
#else

#endif

  return (0);
}

#ifndef ARCHIVE_ACL_SUPPORT
int archive_write_disk_set_acls(struct archive *a, int fd, const char *name,
                                struct archive_acl *abstract_acl,
                                __LA_MODE_T mode) {
  (void)a;
  (void)fd;
  (void)name;
  (void)abstract_acl;
  (void)mode;
  return (ARCHIVE_OK);
}
#endif

static void close_file_descriptor(struct archive_write_disk *a) {
  if (a->fd >= 0) {
    close(a->fd);
    a->fd = -1;
  }
}

#endif
