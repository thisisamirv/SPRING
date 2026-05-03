/*-
 * Copyright (c) 2003-2007 Tim Kientzle
 * Copyright (c) 2010-2012 Michihiro NAKAJIMA
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

#include "archive_platform.h"

#ifndef ARCHIVE_PLATFORM_H_INCLUDED
#error "archive_platform.h must be included first"
#endif

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include "archive.h"

#ifndef ARCHIVE_H_INCLUDED
#error "archive.h must be included"
#endif

#include "archive_entry.h"

#ifndef ARCHIVE_ENTRY_H_INCLUDED
#error "archive_entry.h must be included"
#endif

#include "archive_entry_locale.h"

#ifndef ARCHIVE_ENTRY_LOCALE_H_INCLUDED
#error "archive_entry_locale.h must be included"
#endif

#include "archive_private.h"

#ifndef ARCHIVE_PRIVATE_H_INCLUDED
#error "archive_private.h must be included"
#endif

#include "archive_write_private.h"

#ifndef ARCHIVE_WRITE_PRIVATE_H_INCLUDED
#error "archive_write_private.h must be included"
#endif

#include "archive_write_set_format_private.h"

#ifndef ARCHIVE_WRITE_SET_FORMAT_PRIVATE_H_INCLUDED
#error "archive_write_set_format_private.h must be included"
#endif

struct sparse_block {
  struct sparse_block *next;
  int is_hole;
  uint64_t offset;
  uint64_t remaining;
};

struct pax {
  uint64_t entry_bytes_remaining;
  uint64_t entry_padding;
  struct archive_string l_url_encoded_name;
  struct archive_string pax_header;
  struct archive_string sparse_map;
  size_t sparse_map_padding;
  struct sparse_block *sparse_list;
  struct sparse_block *sparse_tail;
  struct archive_string_conv *sconv_utf8;
  int opt_binary;

  unsigned flags;
#define WRITE_SCHILY_XATTR (1 << 0)
#define WRITE_LIBARCHIVE_XATTR (1 << 1)
};

static void add_pax_attr(struct archive_string *, const char *key,
                         const char *value);
static void add_pax_attr_binary(struct archive_string *, const char *key,
                                const char *value, size_t value_len);
static void add_pax_attr_int(struct archive_string *, const char *key,
                             int64_t value);
static void add_pax_attr_time(struct archive_string *, const char *key,
                              int64_t sec, unsigned long nanos);
static int add_pax_acl(struct archive_write *, struct archive_entry *,
                       struct pax *, int);
static ssize_t archive_write_pax_data(struct archive_write *, const void *,
                                      size_t);
static int archive_write_pax_close(struct archive_write *);
static int archive_write_pax_free(struct archive_write *);
static int archive_write_pax_finish_entry(struct archive_write *);
static int archive_write_pax_header(struct archive_write *,
                                    struct archive_entry *);
static int archive_write_pax_options(struct archive_write *, const char *,
                                     const char *);
static char *base64_encode(const char *src, size_t len);
static char *build_gnu_sparse_name(char *dest, const char *src);
static char *build_pax_attribute_name(char *dest, const char *src);
static char *build_ustar_entry_name(char *dest, const char *src,
                                    size_t src_length, const char *insert);
static char *format_int(char *dest, int64_t);
static int has_non_ASCII(const char *);
static void sparse_list_clear(struct pax *);
static int sparse_list_add(struct pax *, int64_t, int64_t);
static char *url_encode(const char *in);
static time_t get_ustar_max_mtime(void);

int archive_write_set_format_pax_restricted(struct archive *_a) {
  struct archive_write *a = (struct archive_write *)_a;
  int r;

  archive_check_magic(_a, ARCHIVE_WRITE_MAGIC, ARCHIVE_STATE_NEW,
                      "archive_write_set_format_pax_restricted");

  r = archive_write_set_format_pax(&a->archive);
  a->archive.archive_format = ARCHIVE_FORMAT_TAR_PAX_RESTRICTED;
  a->archive.archive_format_name = "restricted POSIX pax interchange";
  return (r);
}

int archive_write_set_format_pax(struct archive *_a) {
  struct archive_write *a = (struct archive_write *)_a;
  struct pax *pax;

  archive_check_magic(_a, ARCHIVE_WRITE_MAGIC, ARCHIVE_STATE_NEW,
                      "archive_write_set_format_pax");

  if (a->format_free != NULL)
    (a->format_free)(a);

  pax = calloc(1, sizeof(*pax));
  if (pax == NULL) {
    archive_set_error(&a->archive, ENOMEM, "Can't allocate pax data");
    return (ARCHIVE_FATAL);
  }
  pax->flags = WRITE_LIBARCHIVE_XATTR | WRITE_SCHILY_XATTR;

  a->format_data = pax;
  a->format_name = "pax";
  a->format_options = archive_write_pax_options;
  a->format_write_header = archive_write_pax_header;
  a->format_write_data = archive_write_pax_data;
  a->format_close = archive_write_pax_close;
  a->format_free = archive_write_pax_free;
  a->format_finish_entry = archive_write_pax_finish_entry;
  a->archive.archive_format = ARCHIVE_FORMAT_TAR_PAX_INTERCHANGE;
  a->archive.archive_format_name = "POSIX pax interchange";
  return (ARCHIVE_OK);
}

static int archive_write_pax_options(struct archive_write *a, const char *key,
                                     const char *val) {
  struct pax *pax = (struct pax *)a->format_data;
  int ret = ARCHIVE_FAILED;

  if (strcmp(key, "hdrcharset") == 0) {

    if (val == NULL || val[0] == 0)
      archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                        "pax: hdrcharset option needs a character-set name");
    else if (strcmp(val, "BINARY") == 0 || strcmp(val, "binary") == 0) {

      pax->opt_binary = 1;
      ret = ARCHIVE_OK;
    } else if (strcmp(val, "UTF-8") == 0) {

      pax->sconv_utf8 =
          archive_string_conversion_to_charset(&(a->archive), "UTF-8", 0);
      if (pax->sconv_utf8 == NULL)
        ret = ARCHIVE_FATAL;
      else
        ret = ARCHIVE_OK;
    } else
      archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                        "pax: invalid charset name");
    return (ret);
  } else if (strcmp(key, "xattrheader") == 0) {
    if (val == NULL || val[0] == 0) {
      archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                        "pax: xattrheader requires a value");
    } else if (strcmp(val, "ALL") == 0 || strcmp(val, "all") == 0) {
      pax->flags |= WRITE_LIBARCHIVE_XATTR | WRITE_SCHILY_XATTR;
      ret = ARCHIVE_OK;
    } else if (strcmp(val, "SCHILY") == 0 || strcmp(val, "schily") == 0) {
      pax->flags |= WRITE_SCHILY_XATTR;
      pax->flags &= ~WRITE_LIBARCHIVE_XATTR;
      ret = ARCHIVE_OK;
    } else if (strcmp(val, "LIBARCHIVE") == 0 ||
               strcmp(val, "libarchive") == 0) {
      pax->flags |= WRITE_LIBARCHIVE_XATTR;
      pax->flags &= ~WRITE_SCHILY_XATTR;
      ret = ARCHIVE_OK;
    } else
      archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                        "pax: invalid xattr header name");
    return (ret);
  }

  return (ARCHIVE_WARN);
}

static void add_pax_attr_time(struct archive_string *as, const char *key,
                              int64_t sec, unsigned long nanos) {
  int digit, i;
  char *t;

  char tmp[1 + 3 * sizeof(sec) + 1 + 3 * sizeof(nanos)];

  tmp[sizeof(tmp) - 1] = 0;
  t = tmp + sizeof(tmp) - 1;

  for (digit = 0, i = 10; i > 0 && digit == 0; i--) {
    digit = nanos % 10;
    nanos /= 10;
  }

  if (i > 0) {
    while (i > 0) {
      *--t = "0123456789"[digit];
      digit = nanos % 10;
      nanos /= 10;
      i--;
    }
    *--t = '.';
  }
  t = format_int(t, sec);

  add_pax_attr(as, key, t);
}

static char *format_int(char *t, int64_t i) {
  uint64_t ui;

  if (i < 0)
    ui = (i == INT64_MIN) ? (uint64_t)(INT64_MAX) + 1 : (uint64_t)(-i);
  else
    ui = i;

  do {
    *--t = "0123456789"[ui % 10];
  } while (ui /= 10);
  if (i < 0)
    *--t = '-';
  return (t);
}

static void add_pax_attr_int(struct archive_string *as, const char *key,
                             int64_t value) {
  char tmp[1 + 3 * sizeof(value)];

  tmp[sizeof(tmp) - 1] = 0;
  add_pax_attr(as, key, format_int(tmp + sizeof(tmp) - 1, value));
}

static void add_pax_attr(struct archive_string *as, const char *key,
                         const char *value) {
  add_pax_attr_binary(as, key, value, strlen(value));
}

static void add_pax_attr_binary(struct archive_string *as, const char *key,
                                const char *value, size_t value_len) {
  int digits, i, len, next_ten;
  char tmp[1 + 3 * sizeof(int)];

  len = 1 + (int)strlen(key) + 1 + (int)value_len + 1;

  next_ten = 1;
  digits = 0;
  i = len;
  while (i > 0) {
    i = i / 10;
    digits++;
    next_ten = next_ten * 10;
  }

  if (len + digits >= next_ten)
    digits++;

  tmp[sizeof(tmp) - 1] = 0;
  archive_strcat(as, format_int(tmp + sizeof(tmp) - 1, len + digits));
  archive_strappend_char(as, ' ');
  archive_strcat(as, key);
  archive_strappend_char(as, '=');
  archive_array_append(as, value, value_len);
  archive_strappend_char(as, '\n');
}

static void archive_write_pax_header_xattr(struct pax *pax,
                                           const char *encoded_name,
                                           const void *value,
                                           size_t value_len) {
  struct archive_string s;
  char *encoded_value;

  if (encoded_name == NULL)
    return;

  if (pax->flags & WRITE_LIBARCHIVE_XATTR) {
    encoded_value = base64_encode((const char *)value, value_len);
    if (encoded_value != NULL) {
      archive_string_init(&s);
      archive_strcpy(&s, "LIBARCHIVE.xattr.");
      archive_strcat(&s, encoded_name);
      add_pax_attr(&(pax->pax_header), s.s, encoded_value);
      archive_string_free(&s);
    }
    free(encoded_value);
  }
  if (pax->flags & WRITE_SCHILY_XATTR) {
    archive_string_init(&s);
    archive_strcpy(&s, "SCHILY.xattr.");
    archive_strcat(&s, encoded_name);
    add_pax_attr_binary(&(pax->pax_header), s.s, value, value_len);
    archive_string_free(&s);
  }
}

static int archive_write_pax_header_xattrs(struct archive_write *a,
                                           struct pax *pax,
                                           struct archive_entry *entry) {
  int i = archive_entry_xattr_reset(entry);

  while (i--) {
    const char *name;
    const void *value;
    char *url_encoded_name = NULL, *encoded_name = NULL;
    size_t size;
    int r;

    archive_entry_xattr_next(entry, &name, &value, &size);
    url_encoded_name = url_encode(name);
    if (url_encoded_name == NULL)
      goto malloc_error;
    else {

      r = archive_strcpy_l(&(pax->l_url_encoded_name), url_encoded_name,
                           pax->sconv_utf8);
      free(url_encoded_name);
      if (r == 0)
        encoded_name = pax->l_url_encoded_name.s;
      else if (r == -1)
        goto malloc_error;
      else {
        archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                          "Error encoding pax extended attribute");
        return (ARCHIVE_FAILED);
      }
    }

    archive_write_pax_header_xattr(pax, encoded_name, value, size);
  }
  return (ARCHIVE_OK);
malloc_error:
  archive_set_error(&a->archive, ENOMEM, "Can't allocate memory");
  return (ARCHIVE_FATAL);
}

static int get_entry_hardlink(struct archive_write *a,
                              struct archive_entry *entry, const char **name,
                              size_t *length, struct archive_string_conv *sc) {
  int r;

  r = archive_entry_hardlink_l(entry, name, length, sc);
  if (r != 0) {
    if (errno == ENOMEM) {
      archive_set_error(&a->archive, ENOMEM,
                        "Can't allocate memory for Linkname");
      return (ARCHIVE_FATAL);
    }
    return (ARCHIVE_WARN);
  }
  return (ARCHIVE_OK);
}

static int get_entry_pathname(struct archive_write *a,
                              struct archive_entry *entry, const char **name,
                              size_t *length, struct archive_string_conv *sc) {
  int r;

  r = archive_entry_pathname_l(entry, name, length, sc);
  if (r != 0) {
    if (errno == ENOMEM) {
      archive_set_error(&a->archive, ENOMEM,
                        "Can't allocate memory for Pathname");
      return (ARCHIVE_FATAL);
    }
    return (ARCHIVE_WARN);
  }
  return (ARCHIVE_OK);
}

static int get_entry_uname(struct archive_write *a, struct archive_entry *entry,
                           const char **name, size_t *length,
                           struct archive_string_conv *sc) {
  int r;

  r = archive_entry_uname_l(entry, name, length, sc);
  if (r != 0) {
    if (errno == ENOMEM) {
      archive_set_error(&a->archive, ENOMEM, "Can't allocate memory for Uname");
      return (ARCHIVE_FATAL);
    }
    return (ARCHIVE_WARN);
  }
  return (ARCHIVE_OK);
}

static int get_entry_gname(struct archive_write *a, struct archive_entry *entry,
                           const char **name, size_t *length,
                           struct archive_string_conv *sc) {
  int r;

  r = archive_entry_gname_l(entry, name, length, sc);
  if (r != 0) {
    if (errno == ENOMEM) {
      archive_set_error(&a->archive, ENOMEM, "Can't allocate memory for Gname");
      return (ARCHIVE_FATAL);
    }
    return (ARCHIVE_WARN);
  }
  return (ARCHIVE_OK);
}

static int get_entry_symlink(struct archive_write *a,
                             struct archive_entry *entry, const char **name,
                             size_t *length, struct archive_string_conv *sc) {
  int r;

  r = archive_entry_symlink_l(entry, name, length, sc);
  if (r != 0) {
    if (errno == ENOMEM) {
      archive_set_error(&a->archive, ENOMEM,
                        "Can't allocate memory for Linkname");
      return (ARCHIVE_FATAL);
    }
    return (ARCHIVE_WARN);
  }
  return (ARCHIVE_OK);
}

static int add_pax_acl(struct archive_write *a, struct archive_entry *entry,
                       struct pax *pax, int flags) {
  char *p;
  const char *attr;
  int acl_types;

  acl_types = archive_entry_acl_types(entry);

  if ((acl_types & ARCHIVE_ENTRY_ACL_TYPE_NFS4) != 0)
    attr = "SCHILY.acl.ace";
  else if ((flags & ARCHIVE_ENTRY_ACL_TYPE_ACCESS) != 0)
    attr = "SCHILY.acl.access";
  else if ((flags & ARCHIVE_ENTRY_ACL_TYPE_DEFAULT) != 0)
    attr = "SCHILY.acl.default";
  else
    return (ARCHIVE_FATAL);

  p = archive_entry_acl_to_text_l(entry, NULL, flags, pax->sconv_utf8);
  if (p == NULL) {
    if (errno == ENOMEM) {
      archive_set_error(&a->archive, ENOMEM, "%s %s",
                        "Can't allocate memory for ", attr);
      return (ARCHIVE_FATAL);
    }
    archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT, "%s %s %s",
                      "Can't translate ", attr, " to UTF-8");
    return (ARCHIVE_WARN);
  }

  if (*p != '\0') {
    add_pax_attr(&(pax->pax_header), attr, p);
  }
  free(p);
  return (ARCHIVE_OK);
}

static int archive_write_pax_header(struct archive_write *a,
                                    struct archive_entry *entry_original) {
  struct archive_entry *entry_main;
  const char *p;
  const char *suffix;
  int need_extension, r, ret;
  int acl_types;
  int sparse_count;
  uint64_t sparse_total, real_size;
  struct pax *pax;
  const char *hardlink;
  const char *path = NULL, *linkpath = NULL;
  const char *uname = NULL, *gname = NULL;
  const void *mac_metadata;
  size_t mac_metadata_size;
  struct archive_string_conv *sconv;
  size_t hardlink_length, path_length, linkpath_length;
  size_t uname_length, gname_length;

  char paxbuff[512];
  char ustarbuff[512];
  char ustar_entry_name[256];
  char pax_entry_name[256];
  char gnu_sparse_name[256];
  struct archive_string entry_name;

  ret = ARCHIVE_OK;
  need_extension = 0;
  pax = (struct pax *)a->format_data;

  const time_t ustar_max_mtime = get_ustar_max_mtime();

#if defined(_WIN32) && !defined(__CYGWIN__)

  if ((archive_entry_pathname_w(entry_original) == NULL) &&
      (archive_entry_pathname(entry_original) == NULL)) {
#else
  if (archive_entry_pathname(entry_original) == NULL) {
#endif
    archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                      "Can't record entry in tar file without pathname");
    return (ARCHIVE_FAILED);
  }

  if (pax->opt_binary)
    sconv = NULL;
  else {

    if (pax->sconv_utf8 == NULL) {

      pax->sconv_utf8 =
          archive_string_conversion_to_charset(&(a->archive), "UTF-8", 1);
      if (pax->sconv_utf8 == NULL)

        return (ARCHIVE_FAILED);
    }
    sconv = pax->sconv_utf8;
  }

  r = get_entry_hardlink(a, entry_original, &hardlink, &hardlink_length, sconv);
  if (r == ARCHIVE_FATAL)
    return (r);
  else if (r != ARCHIVE_OK) {
    r = get_entry_hardlink(a, entry_original, &hardlink, &hardlink_length,
                           NULL);
    if (r == ARCHIVE_FATAL)
      return (r);
    archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
                      "Can't translate linkname '%s' to %s", hardlink,
                      archive_string_conversion_charset_name(sconv));
    ret = ARCHIVE_WARN;
    sconv = NULL;
  }

  if (hardlink == NULL) {
    switch (archive_entry_filetype(entry_original)) {
    case AE_IFBLK:
    case AE_IFCHR:
    case AE_IFIFO:
    case AE_IFLNK:
    case AE_IFREG:
      break;
    case AE_IFDIR: {

#if defined(_WIN32) && !defined(__CYGWIN__)
      const wchar_t *wp;

      wp = archive_entry_pathname_w(entry_original);
      if (wp != NULL && wp[wcslen(wp) - 1] != L'/') {
        struct archive_wstring ws;

        archive_string_init(&ws);
        path_length = wcslen(wp);
        if (archive_wstring_ensure(&ws, path_length + 2) == NULL) {
          archive_set_error(&a->archive, ENOMEM, "Can't allocate pax data");
          archive_wstring_free(&ws);
          return (ARCHIVE_FATAL);
        }

        if (wp[path_length - 1] == L'\\')
          path_length--;
        archive_wstrncpy(&ws, wp, path_length);
        archive_wstrappend_wchar(&ws, L'/');
        archive_entry_copy_pathname_w(entry_original, ws.s);
        archive_wstring_free(&ws);
        p = NULL;
      } else
#endif
        p = archive_entry_pathname(entry_original);

      if (p != NULL && p[0] != '\0' && p[strlen(p) - 1] != '/') {
        struct archive_string as;

        archive_string_init(&as);
        path_length = strlen(p);
        if (archive_string_ensure(&as, path_length + 2) == NULL) {
          archive_set_error(&a->archive, ENOMEM, "Can't allocate pax data");
          archive_string_free(&as);
          return (ARCHIVE_FATAL);
        }
#if defined(_WIN32) && !defined(__CYGWIN__)

        if (p[strlen(p) - 1] == '\\')
          path_length--;
        else
#endif
          archive_strncpy(&as, p, path_length);
        archive_strappend_char(&as, '/');
        archive_entry_copy_pathname(entry_original, as.s);
        archive_string_free(&as);
      }
      break;
    }
    default:
      __archive_write_entry_filetype_unsupported(&a->archive, entry_original,
                                                 "pax");
      return (ARCHIVE_FAILED);
    }
  }

  mac_metadata = archive_entry_mac_metadata(entry_original, &mac_metadata_size);
  if (mac_metadata != NULL) {
    const char *oname;
    char *name, *bname;
    size_t name_length;
    struct archive_entry *extra = archive_entry_new2(&a->archive);

    oname = archive_entry_pathname(entry_original);
    name_length = strlen(oname);
    name = malloc(name_length + 3);
    if (name == NULL || extra == NULL) {

      archive_entry_free(extra);
      free(name);
      return (ARCHIVE_FAILED);
    }
    strcpy(name, oname);

    bname = strrchr(name, '/');
    while (bname != NULL && bname[1] == '\0') {
      *bname = '\0';
      bname = strrchr(name, '/');
    }
    if (bname == NULL) {
      memmove(name + 2, name, name_length + 1);
      memmove(name, "._", 2);
    } else {
      bname += 1;
      memmove(bname + 2, bname, strlen(bname) + 1);
      memmove(bname, "._", 2);
    }
    archive_entry_copy_pathname(extra, name);
    free(name);

    archive_entry_set_size(extra, mac_metadata_size);
    archive_entry_set_filetype(extra, AE_IFREG);
    archive_entry_set_perm(extra, archive_entry_perm(entry_original));
    archive_entry_set_mtime(extra, archive_entry_mtime(entry_original),
                            archive_entry_mtime_nsec(entry_original));
    archive_entry_set_gid(extra, archive_entry_gid(entry_original));
    archive_entry_set_gname(extra, archive_entry_gname(entry_original));
    archive_entry_set_uid(extra, archive_entry_uid(entry_original));
    archive_entry_set_uname(extra, archive_entry_uname(entry_original));

    r = archive_write_pax_header(a, extra);
    archive_entry_free(extra);
    if (r < ARCHIVE_WARN)
      return (r);
    if (r < ret)
      ret = r;
    r = (int)archive_write_pax_data(a, mac_metadata, mac_metadata_size);
    if (r < ARCHIVE_WARN)
      return (r);
    if (r < ret)
      ret = r;
    r = archive_write_pax_finish_entry(a);
    if (r < ARCHIVE_WARN)
      return (r);
    if (r < ret)
      ret = r;
  }

#if defined(_WIN32) && !defined(__CYGWIN__)

  entry_main = __la_win_entry_in_posix_pathseparator(entry_original);
  if (entry_main == entry_original)
    entry_main = archive_entry_clone(entry_original);
#else
  entry_main = archive_entry_clone(entry_original);
#endif
  if (entry_main == NULL) {
    archive_set_error(&a->archive, ENOMEM, "Can't allocate pax data");
    return (ARCHIVE_FATAL);
  }
  archive_string_empty(&(pax->pax_header));
  archive_string_empty(&(pax->sparse_map));
  sparse_total = 0;
  sparse_list_clear(pax);

  if (hardlink == NULL && archive_entry_filetype(entry_main) == AE_IFREG)
    sparse_count = archive_entry_sparse_reset(entry_main);
  else
    sparse_count = 0;
  if (sparse_count) {
    int64_t offset, length, last_offset = 0;

    while (archive_entry_sparse_next(entry_main, &offset, &length) ==
           ARCHIVE_OK)
      last_offset = offset + length;

    if (last_offset < archive_entry_size(entry_main))
      archive_entry_sparse_add_entry(entry_main, archive_entry_size(entry_main),
                                     0);
    sparse_count = archive_entry_sparse_reset(entry_main);
  }

  r = get_entry_pathname(a, entry_main, &path, &path_length, sconv);
  if (r == ARCHIVE_FATAL) {
    archive_entry_free(entry_main);
    return (r);
  } else if (r != ARCHIVE_OK) {
    r = get_entry_pathname(a, entry_main, &path, &path_length, NULL);
    if (r == ARCHIVE_FATAL) {
      archive_entry_free(entry_main);
      return (r);
    }
    archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
                      "Can't translate pathname '%s' to %s", path,
                      archive_string_conversion_charset_name(sconv));
    ret = ARCHIVE_WARN;
    sconv = NULL;
  }
  r = get_entry_uname(a, entry_main, &uname, &uname_length, sconv);
  if (r == ARCHIVE_FATAL) {
    archive_entry_free(entry_main);
    return (r);
  } else if (r != ARCHIVE_OK) {
    r = get_entry_uname(a, entry_main, &uname, &uname_length, NULL);
    if (r == ARCHIVE_FATAL) {
      archive_entry_free(entry_main);
      return (r);
    }
    archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
                      "Can't translate uname '%s' to %s", uname,
                      archive_string_conversion_charset_name(sconv));
    ret = ARCHIVE_WARN;
    sconv = NULL;
  }
  r = get_entry_gname(a, entry_main, &gname, &gname_length, sconv);
  if (r == ARCHIVE_FATAL) {
    archive_entry_free(entry_main);
    return (r);
  } else if (r != ARCHIVE_OK) {
    r = get_entry_gname(a, entry_main, &gname, &gname_length, NULL);
    if (r == ARCHIVE_FATAL) {
      archive_entry_free(entry_main);
      return (r);
    }
    archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
                      "Can't translate gname '%s' to %s", gname,
                      archive_string_conversion_charset_name(sconv));
    ret = ARCHIVE_WARN;
    sconv = NULL;
  }
  linkpath = hardlink;
  linkpath_length = hardlink_length;
  if (linkpath == NULL) {
    r = get_entry_symlink(a, entry_main, &linkpath, &linkpath_length, sconv);
    if (r == ARCHIVE_FATAL) {
      archive_entry_free(entry_main);
      return (r);
    } else if (r != ARCHIVE_OK) {
      r = get_entry_symlink(a, entry_main, &linkpath, &linkpath_length, NULL);
      if (r == ARCHIVE_FATAL) {
        archive_entry_free(entry_main);
        return (r);
      }
      archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
                        "Can't translate linkname '%s' to %s", linkpath,
                        archive_string_conversion_charset_name(sconv));
      ret = ARCHIVE_WARN;
      sconv = NULL;
    }
  }

  if (sconv == NULL && !pax->opt_binary) {
    if (hardlink != NULL) {
      r = get_entry_hardlink(a, entry_main, &hardlink, &hardlink_length, NULL);
      if (r == ARCHIVE_FATAL) {
        archive_entry_free(entry_main);
        return (r);
      }
      linkpath = hardlink;
      linkpath_length = hardlink_length;
    }
    r = get_entry_pathname(a, entry_main, &path, &path_length, NULL);
    if (r == ARCHIVE_FATAL) {
      archive_entry_free(entry_main);
      return (r);
    }
    r = get_entry_uname(a, entry_main, &uname, &uname_length, NULL);
    if (r == ARCHIVE_FATAL) {
      archive_entry_free(entry_main);
      return (r);
    }
    r = get_entry_gname(a, entry_main, &gname, &gname_length, NULL);
    if (r == ARCHIVE_FATAL) {
      archive_entry_free(entry_main);
      return (r);
    }
  }

  if (sconv == NULL)
    add_pax_attr(&(pax->pax_header), "hdrcharset", "BINARY");

  if (has_non_ASCII(path)) {

    add_pax_attr(&(pax->pax_header), "path", path);
    archive_entry_set_pathname(
        entry_main,
        build_ustar_entry_name(ustar_entry_name, path, path_length, NULL));
    need_extension = 1;
  } else {

    if (path_length <= 100) {

    } else {

      suffix = strchr(path + path_length - 100 - 1, '/');

      if (suffix == path)
        suffix = strchr(suffix + 1, '/');

      if (suffix == NULL || suffix[1] == '\0' || suffix - path > 155) {
        add_pax_attr(&(pax->pax_header), "path", path);
        archive_entry_set_pathname(
            entry_main,
            build_ustar_entry_name(ustar_entry_name, path, path_length, NULL));
        need_extension = 1;
      }
    }
  }

  if (linkpath != NULL) {

    if (linkpath_length > 100 || has_non_ASCII(linkpath)) {
      add_pax_attr(&(pax->pax_header), "linkpath", linkpath);
      if (linkpath_length > 100) {
        if (hardlink != NULL)
          archive_entry_set_hardlink(entry_main, "././@LongHardLink");
        else
          archive_entry_set_symlink(entry_main, "././@LongSymLink");
      } else {

        if (hardlink != NULL)
          archive_entry_set_hardlink(entry_main, linkpath);
        else
          archive_entry_set_symlink(entry_main, linkpath);
      }
      need_extension = 1;
    }
  }

  archive_string_init(&entry_name);
  archive_strcpy(&entry_name, archive_entry_pathname(entry_main));

  if (archive_entry_size(entry_main) >= (((int64_t)1) << 33)) {
    need_extension = 1;
  }

  if ((unsigned int)archive_entry_gid(entry_main) >= (1 << 18)) {
    add_pax_attr_int(&(pax->pax_header), "gid", archive_entry_gid(entry_main));
    need_extension = 1;
  }

  if (gname != NULL) {
    if (gname_length > 31 || has_non_ASCII(gname)) {
      add_pax_attr(&(pax->pax_header), "gname", gname);
      need_extension = 1;
    }
  }

  if ((unsigned int)archive_entry_uid(entry_main) >= (1 << 18)) {
    add_pax_attr_int(&(pax->pax_header), "uid", archive_entry_uid(entry_main));
    need_extension = 1;
  }

  if (uname != NULL) {
    if (uname_length > 31 || has_non_ASCII(uname)) {
      add_pax_attr(&(pax->pax_header), "uname", uname);
      need_extension = 1;
    }
  }

  if (archive_entry_filetype(entry_main) == AE_IFBLK ||
      archive_entry_filetype(entry_main) == AE_IFCHR) {

    int rdevmajor, rdevminor;
    rdevmajor = archive_entry_rdevmajor(entry_main);
    rdevminor = archive_entry_rdevminor(entry_main);
    if (rdevmajor >= (1 << 18)) {
      add_pax_attr_int(&(pax->pax_header), "SCHILY.devmajor", rdevmajor);

      need_extension = 1;
    }

    if (rdevminor >= (1 << 18)) {
      add_pax_attr_int(&(pax->pax_header), "SCHILY.devminor", rdevminor);

      need_extension = 1;
    }
  }

  if (!need_extension && ((archive_entry_mtime(entry_main) < 0) ||
                          (archive_entry_mtime(entry_main) >= ustar_max_mtime)))
    need_extension = 1;

  p = archive_entry_fflags_text(entry_main);
  if (!need_extension && p != NULL && *p != '\0')
    need_extension = 1;

  if (!need_extension && archive_entry_xattr_count(entry_original) > 0)
    need_extension = 1;

  if (!need_extension && sparse_count > 0)
    need_extension = 1;

  acl_types = archive_entry_acl_types(entry_original);

  if (!need_extension && acl_types != 0)
    need_extension = 1;

  if (!need_extension && archive_entry_symlink_type(entry_main) > 0)
    need_extension = 1;

  if (a->archive.archive_format != ARCHIVE_FORMAT_TAR_PAX_RESTRICTED) {
    if (archive_entry_ctime(entry_main) != 0 ||
        archive_entry_ctime_nsec(entry_main) != 0)
      add_pax_attr_time(&(pax->pax_header), "ctime",
                        archive_entry_ctime(entry_main),
                        archive_entry_ctime_nsec(entry_main));

    if (archive_entry_atime(entry_main) != 0 ||
        archive_entry_atime_nsec(entry_main) != 0)
      add_pax_attr_time(&(pax->pax_header), "atime",
                        archive_entry_atime(entry_main),
                        archive_entry_atime_nsec(entry_main));

    if (archive_entry_birthtime_is_set(entry_main) &&
        archive_entry_birthtime(entry_main) < archive_entry_mtime(entry_main))
      add_pax_attr_time(&(pax->pax_header), "LIBARCHIVE.creationtime",
                        archive_entry_birthtime(entry_main),
                        archive_entry_birthtime_nsec(entry_main));
  }

  if (a->archive.archive_format != ARCHIVE_FORMAT_TAR_PAX_RESTRICTED ||
      need_extension) {
    if (archive_entry_mtime(entry_main) < 0 ||
        archive_entry_mtime(entry_main) >= ustar_max_mtime ||
        archive_entry_mtime_nsec(entry_main) != 0)
      add_pax_attr_time(&(pax->pax_header), "mtime",
                        archive_entry_mtime(entry_main),
                        archive_entry_mtime_nsec(entry_main));

    p = archive_entry_fflags_text(entry_main);
    if (p != NULL && *p != '\0')
      add_pax_attr(&(pax->pax_header), "SCHILY.fflags", p);

    if ((acl_types & ARCHIVE_ENTRY_ACL_TYPE_NFS4) != 0) {
      ret = add_pax_acl(a, entry_original, pax,
                        ARCHIVE_ENTRY_ACL_STYLE_EXTRA_ID |
                            ARCHIVE_ENTRY_ACL_STYLE_SEPARATOR_COMMA |
                            ARCHIVE_ENTRY_ACL_STYLE_COMPACT);
      if (ret == ARCHIVE_FATAL) {
        archive_entry_free(entry_main);
        archive_string_free(&entry_name);
        return (ARCHIVE_FATAL);
      }
    }
    if (acl_types & ARCHIVE_ENTRY_ACL_TYPE_ACCESS) {
      ret = add_pax_acl(a, entry_original, pax,
                        ARCHIVE_ENTRY_ACL_TYPE_ACCESS |
                            ARCHIVE_ENTRY_ACL_STYLE_EXTRA_ID |
                            ARCHIVE_ENTRY_ACL_STYLE_SEPARATOR_COMMA);
      if (ret == ARCHIVE_FATAL) {
        archive_entry_free(entry_main);
        archive_string_free(&entry_name);
        return (ARCHIVE_FATAL);
      }
    }
    if (acl_types & ARCHIVE_ENTRY_ACL_TYPE_DEFAULT) {
      ret = add_pax_acl(a, entry_original, pax,
                        ARCHIVE_ENTRY_ACL_TYPE_DEFAULT |
                            ARCHIVE_ENTRY_ACL_STYLE_EXTRA_ID |
                            ARCHIVE_ENTRY_ACL_STYLE_SEPARATOR_COMMA);
      if (ret == ARCHIVE_FATAL) {
        archive_entry_free(entry_main);
        archive_string_free(&entry_name);
        return (ARCHIVE_FATAL);
      }
    }

    if (sparse_count > 0) {
      int64_t soffset, slength;

      add_pax_attr_int(&(pax->pax_header), "GNU.sparse.major", 1);
      add_pax_attr_int(&(pax->pax_header), "GNU.sparse.minor", 0);

      add_pax_attr(&(pax->pax_header), "GNU.sparse.name", path);
      add_pax_attr_int(&(pax->pax_header), "GNU.sparse.realsize",
                       archive_entry_size(entry_main));

      archive_entry_set_pathname(
          entry_main, build_gnu_sparse_name(gnu_sparse_name, entry_name.s));

      archive_string_sprintf(&(pax->sparse_map), "%d\n", sparse_count);
      while (archive_entry_sparse_next(entry_main, &soffset, &slength) ==
             ARCHIVE_OK) {
        archive_string_sprintf(&(pax->sparse_map), "%jd\n%jd\n",
                               (intmax_t)soffset, (intmax_t)slength);
        sparse_total += slength;
        if (sparse_list_add(pax, soffset, slength) != ARCHIVE_OK) {
          archive_set_error(&a->archive, ENOMEM, "Can't allocate memory");
          archive_entry_free(entry_main);
          archive_string_free(&entry_name);
          return (ARCHIVE_FATAL);
        }
      }
    }

    if (archive_write_pax_header_xattrs(a, pax, entry_original) ==
        ARCHIVE_FATAL) {
      archive_entry_free(entry_main);
      archive_string_free(&entry_name);
      return (ARCHIVE_FATAL);
    }

    if (archive_entry_symlink_type(entry_main) == AE_SYMLINK_TYPE_FILE) {
      add_pax_attr(&(pax->pax_header), "LIBARCHIVE.symlinktype", "file");
    } else if (archive_entry_symlink_type(entry_main) ==
               AE_SYMLINK_TYPE_DIRECTORY) {
      add_pax_attr(&(pax->pax_header), "LIBARCHIVE.symlinktype", "dir");
    }
  }

  if (archive_entry_filetype(entry_main) != AE_IFREG)
    archive_entry_set_size(entry_main, 0);

  if (a->archive.archive_format != ARCHIVE_FORMAT_TAR_PAX_INTERCHANGE &&
      hardlink != NULL)
    archive_entry_set_size(entry_main, 0);

  if (hardlink != NULL)
    archive_entry_set_size(entry_main, 0);

  real_size = archive_entry_size(entry_main);

  if (archive_strlen(&(pax->sparse_map))) {
    size_t mapsize = archive_strlen(&(pax->sparse_map));
    pax->sparse_map_padding = 0x1ff & (-(ssize_t)mapsize);
    archive_entry_set_size(entry_main,
                           mapsize + pax->sparse_map_padding + sparse_total);
  }

  if (archive_entry_size(entry_main) >= (((int64_t)1) << 33)) {
    add_pax_attr_int(&(pax->pax_header), "size",
                     archive_entry_size(entry_main));
  }

  if (__archive_write_format_header_ustar(a, ustarbuff, entry_main, -1, 0,
                                          NULL) == ARCHIVE_FATAL) {
    archive_entry_free(entry_main);
    archive_string_free(&entry_name);
    return (ARCHIVE_FATAL);
  }

  if (archive_strlen(&(pax->pax_header)) > 0) {
    struct archive_entry *pax_attr_entry;
    time_t s;
    int64_t uid, gid;
    __LA_MODE_T mode;

    pax_attr_entry = archive_entry_new2(&a->archive);
    p = entry_name.s;
    archive_entry_set_pathname(pax_attr_entry,
                               build_pax_attribute_name(pax_entry_name, p));
    archive_entry_set_size(pax_attr_entry, archive_strlen(&(pax->pax_header)));

    uid = archive_entry_uid(entry_main);
    if (uid >= 1 << 18)
      uid = (1 << 18) - 1;
    archive_entry_set_uid(pax_attr_entry, uid);
    gid = archive_entry_gid(entry_main);
    if (gid >= 1 << 18)
      gid = (1 << 18) - 1;
    archive_entry_set_gid(pax_attr_entry, gid);

    mode = archive_entry_mode(entry_main);
#ifdef S_ISUID
    mode &= ~S_ISUID;
#endif
#ifdef S_ISGID
    mode &= ~S_ISGID;
#endif
#ifdef S_ISVTX
    mode &= ~S_ISVTX;
#endif
    archive_entry_set_mode(pax_attr_entry, mode);

    archive_entry_set_uname(pax_attr_entry, archive_entry_uname(entry_main));
    archive_entry_set_gname(pax_attr_entry, archive_entry_gname(entry_main));

    s = archive_entry_mtime(entry_main);
    if (s < 0) {
      s = 0;
    }
    if (s > ustar_max_mtime) {
      s = ustar_max_mtime;
    }
    archive_entry_set_mtime(pax_attr_entry, s, 0);

    archive_entry_set_atime(pax_attr_entry, 0, 0);

    archive_entry_set_ctime(pax_attr_entry, 0, 0);

    r = __archive_write_format_header_ustar(a, paxbuff, pax_attr_entry, 'x', 1,
                                            NULL);

    archive_entry_free(pax_attr_entry);

    if (r < ARCHIVE_WARN) {
      archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
                        "archive_write_pax_header: "
                        "'x' header failed?!  This can't happen.\n");
      archive_entry_free(entry_main);
      archive_string_free(&entry_name);
      return (ARCHIVE_FATAL);
    } else if (r < ret)
      ret = r;
    r = __archive_write_output(a, paxbuff, 512);
    if (r != ARCHIVE_OK) {
      sparse_list_clear(pax);
      pax->entry_bytes_remaining = 0;
      pax->entry_padding = 0;
      archive_entry_free(entry_main);
      archive_string_free(&entry_name);
      return (ARCHIVE_FATAL);
    }

    pax->entry_bytes_remaining = archive_strlen(&(pax->pax_header));
    pax->entry_padding = 0x1ff & (-(int64_t)pax->entry_bytes_remaining);

    r = __archive_write_output(a, pax->pax_header.s,
                               archive_strlen(&(pax->pax_header)));
    if (r != ARCHIVE_OK) {

      archive_entry_free(entry_main);
      archive_string_free(&entry_name);
      return (ARCHIVE_FATAL);
    }

    r = __archive_write_nulls(a, (size_t)pax->entry_padding);
    if (r != ARCHIVE_OK) {

      archive_entry_free(entry_main);
      archive_string_free(&entry_name);
      return (ARCHIVE_FATAL);
    }
    pax->entry_bytes_remaining = pax->entry_padding = 0;
  }

  r = __archive_write_output(a, ustarbuff, 512);
  if (r != ARCHIVE_OK) {
    archive_entry_free(entry_main);
    archive_string_free(&entry_name);
    return (r);
  }

  archive_entry_set_size(entry_original, real_size);
  if (pax->sparse_list == NULL && real_size > 0) {

    sparse_list_add(pax, 0, real_size);
    sparse_total = real_size;
  }
  pax->entry_padding = 0x1ff & (-(int64_t)sparse_total);
  archive_entry_free(entry_main);
  archive_string_free(&entry_name);

  return (ret);
}

static char *build_ustar_entry_name(char *dest, const char *src,
                                    size_t src_length, const char *insert) {
  const char *prefix, *prefix_end;
  const char *suffix, *suffix_end;
  const char *filename, *filename_end;
  char *p;
  int need_slash = 0;
  size_t suffix_length = 98;
  size_t insert_length;

  if (insert == NULL)
    insert_length = 0;
  else

    insert_length = strlen(insert) + 2;

  if (src_length < 100 && insert == NULL) {
    strncpy(dest, src, src_length);
    dest[src_length] = '\0';
    return (dest);
  }

  filename_end = src + src_length;

  for (;;) {
    if (filename_end > src && filename_end[-1] == '/') {
      filename_end--;
      need_slash = 1;
      continue;
    }
    if (filename_end > src + 1 && filename_end[-1] == '.' &&
        filename_end[-2] == '/') {
      filename_end -= 2;
      need_slash = 1;
      continue;
    }
    break;
  }
  if (need_slash)
    suffix_length--;

  filename = filename_end - 1;
  while ((filename > src) && (*filename != '/'))
    filename--;
  if ((*filename == '/') && (filename < filename_end - 1))
    filename++;

  suffix_length -= insert_length;
  if (filename_end > filename + suffix_length)
    filename_end = filename + suffix_length;

  suffix_length -= filename_end - filename;

  prefix = src;
  prefix_end = prefix + 154;
  if (prefix_end > filename)
    prefix_end = filename;
  while (prefix_end > prefix && *prefix_end != '/')
    prefix_end--;
  if ((prefix_end < filename) && (*prefix_end == '/'))
    prefix_end++;

  suffix = prefix_end;
  suffix_end = suffix + suffix_length;
  if (suffix_end > filename)
    suffix_end = filename;
  if (suffix_end < suffix)
    suffix_end = suffix;
  while (suffix_end > suffix && *suffix_end != '/')
    suffix_end--;
  if ((suffix_end < filename) && (*suffix_end == '/'))
    suffix_end++;

  p = dest;
  if (prefix_end > prefix) {
    strncpy(p, prefix, prefix_end - prefix);
    p += prefix_end - prefix;
  }
  if (suffix_end > suffix) {
    strncpy(p, suffix, suffix_end - suffix);
    p += suffix_end - suffix;
  }
  if (insert != NULL) {

    strcpy(p, insert);
    p += strlen(insert);
    *p++ = '/';
  }
  strncpy(p, filename, filename_end - filename);
  p += filename_end - filename;
  if (need_slash)
    *p++ = '/';
  *p = '\0';

  return (dest);
}

static char *build_pax_attribute_name(char *dest, const char *src) {
  char buff[64];
  const char *p;

  if (src == NULL || *src == '\0') {
    strcpy(dest, "PaxHeader/blank");
    return (dest);
  }

  p = src + strlen(src);
  for (;;) {

    if (p > src && p[-1] == '/') {
      --p;
      continue;
    }

    if (p > src + 1 && p[-1] == '.' && p[-2] == '/') {
      --p;
      continue;
    }
    break;
  }

  if (p == src) {
    strcpy(dest, "/PaxHeader/rootdir");
    return (dest);
  }

  if (*src == '.' && p == src + 1) {
    strcpy(dest, "PaxHeader/currentdir");
    return (dest);
  }

#if HAVE_GETPID && 0
  snprintf(buff, sizeof(buff), "PaxHeader.%d", getpid());
#else

  strcpy(buff, "PaxHeader");
#endif

  build_ustar_entry_name(dest, src, p - src, buff);

  return (dest);
}

static char *build_gnu_sparse_name(char *dest, const char *src) {
  const char *p;

  if (src == NULL || *src == '\0') {
    strcpy(dest, "GNUSparseFile/blank");
    return (dest);
  }

  p = src + strlen(src);
  for (;;) {

    if (p > src && p[-1] == '/') {
      --p;
      continue;
    }

    if (p > src + 1 && p[-1] == '.' && p[-2] == '/') {
      --p;
      continue;
    }
    break;
  }

  build_ustar_entry_name(dest, src, p - src, "GNUSparseFile.0");

  return (dest);
}

static int archive_write_pax_close(struct archive_write *a) {
  return (__archive_write_nulls(a, 512 * 2));
}

static int archive_write_pax_free(struct archive_write *a) {
  struct pax *pax;

  pax = (struct pax *)a->format_data;
  if (pax == NULL)
    return (ARCHIVE_OK);

  archive_string_free(&pax->pax_header);
  archive_string_free(&pax->sparse_map);
  archive_string_free(&pax->l_url_encoded_name);
  sparse_list_clear(pax);
  free(pax);
  a->format_data = NULL;
  return (ARCHIVE_OK);
}

static int archive_write_pax_finish_entry(struct archive_write *a) {
  struct pax *pax;
  uint64_t remaining;
  int ret;

  pax = (struct pax *)a->format_data;
  remaining = pax->entry_bytes_remaining;
  if (remaining == 0) {
    while (pax->sparse_list) {
      struct sparse_block *sb;
      if (!pax->sparse_list->is_hole)
        remaining += pax->sparse_list->remaining;
      sb = pax->sparse_list->next;
      free(pax->sparse_list);
      pax->sparse_list = sb;
    }
  }
  ret = __archive_write_nulls(a, (size_t)(remaining + pax->entry_padding));
  pax->entry_bytes_remaining = pax->entry_padding = 0;
  return (ret);
}

static ssize_t archive_write_pax_data(struct archive_write *a, const void *buff,
                                      size_t s) {
  struct pax *pax;
  size_t ws;
  size_t total;
  int ret;

  pax = (struct pax *)a->format_data;

  if (archive_strlen(&(pax->sparse_map))) {
    ret = __archive_write_output(a, pax->sparse_map.s,
                                 archive_strlen(&(pax->sparse_map)));
    if (ret != ARCHIVE_OK)
      return (ret);
    ret = __archive_write_nulls(a, pax->sparse_map_padding);
    if (ret != ARCHIVE_OK)
      return (ret);
    archive_string_empty(&(pax->sparse_map));
  }

  total = 0;
  while (total < s) {
    const unsigned char *p;

    while (pax->sparse_list != NULL && pax->sparse_list->remaining == 0) {
      struct sparse_block *sb = pax->sparse_list->next;
      free(pax->sparse_list);
      pax->sparse_list = sb;
    }

    if (pax->sparse_list == NULL)
      return (total);

    p = ((const unsigned char *)buff) + total;
    ws = s - total;
    if (ws > pax->sparse_list->remaining)
      ws = (size_t)pax->sparse_list->remaining;

    if (pax->sparse_list->is_hole) {

      pax->sparse_list->remaining -= ws;
      total += ws;
      continue;
    }

    ret = __archive_write_output(a, p, ws);
    pax->sparse_list->remaining -= ws;
    total += ws;
    if (ret != ARCHIVE_OK)
      return (ret);
  }
  return (total);
}

static int has_non_ASCII(const char *_p) {
  const unsigned char *p = (const unsigned char *)_p;

  if (p == NULL)
    return (1);
  while (*p != '\0' && *p < 128)
    p++;
  return (*p != '\0');
}

static char *url_encode(const char *in) {
  const char *s;
  char *d;
  size_t out_len = 0;
  char *out;

  for (s = in; *s != '\0'; s++) {
    if (*s < 33 || *s > 126 || *s == '%' || *s == '=') {
      if (SIZE_MAX - out_len < 4)
        return (NULL);
      out_len += 3;
    } else {
      if (SIZE_MAX - out_len < 2)
        return (NULL);
      out_len++;
    }
  }

  out = malloc(out_len + 1);
  if (out == NULL)
    return (NULL);

  for (s = in, d = out; *s != '\0'; s++) {

    if (*s < 33 || *s > 126 || *s == '%' || *s == '=') {

      *d++ = '%';
      *d++ = "0123456789ABCDEF"[0x0f & (*s >> 4)];
      *d++ = "0123456789ABCDEF"[0x0f & *s];
    } else {
      *d++ = *s;
    }
  }
  *d = '\0';
  return (out);
}

static char *base64_encode(const char *s, size_t len) {
  static const char digits[64] = {
      'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
      'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
      'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
      'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
      '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'};
  int v;
  char *d, *out;

  out = malloc((len * 4 + 2) / 3 + 1);
  if (out == NULL)
    return (NULL);
  d = out;

  while (len >= 3) {
    v = (((int)s[0] << 16) & 0xff0000) | (((int)s[1] << 8) & 0xff00) |
        (((int)s[2]) & 0x00ff);
    s += 3;
    len -= 3;
    *d++ = digits[(v >> 18) & 0x3f];
    *d++ = digits[(v >> 12) & 0x3f];
    *d++ = digits[(v >> 6) & 0x3f];
    *d++ = digits[(v) & 0x3f];
  }

  switch (len) {
  case 0:
    break;
  case 1:
    v = (((int)s[0] << 16) & 0xff0000);
    *d++ = digits[(v >> 18) & 0x3f];
    *d++ = digits[(v >> 12) & 0x3f];
    break;
  case 2:
    v = (((int)s[0] << 16) & 0xff0000) | (((int)s[1] << 8) & 0xff00);
    *d++ = digits[(v >> 18) & 0x3f];
    *d++ = digits[(v >> 12) & 0x3f];
    *d++ = digits[(v >> 6) & 0x3f];
    break;
  }

  *d = '\0';
  return (out);
}

static void sparse_list_clear(struct pax *pax) {
  while (pax->sparse_list != NULL) {
    struct sparse_block *sb = pax->sparse_list;
    pax->sparse_list = sb->next;
    free(sb);
  }
  pax->sparse_tail = NULL;
}

static int _sparse_list_add_block(struct pax *pax, int64_t offset,
                                  int64_t length, int is_hole) {
  struct sparse_block *sb;

  sb = malloc(sizeof(*sb));
  if (sb == NULL)
    return (ARCHIVE_FATAL);
  sb->next = NULL;
  sb->is_hole = is_hole;
  sb->offset = offset;
  sb->remaining = length;
  if (pax->sparse_list == NULL || pax->sparse_tail == NULL)
    pax->sparse_list = pax->sparse_tail = sb;
  else {
    pax->sparse_tail->next = sb;
    pax->sparse_tail = sb;
  }
  return (ARCHIVE_OK);
}

static int sparse_list_add(struct pax *pax, int64_t offset, int64_t length) {
  int64_t last_offset;
  int r;

  if (pax->sparse_tail == NULL)
    last_offset = 0;
  else {
    last_offset = pax->sparse_tail->offset + pax->sparse_tail->remaining;
  }
  if (last_offset < offset) {

    r = _sparse_list_add_block(pax, last_offset, offset - last_offset, 1);
    if (r != ARCHIVE_OK)
      return (r);
  }

  return (_sparse_list_add_block(pax, offset, length, 0));
}

static time_t get_ustar_max_mtime(void) {

  if (sizeof(time_t) > sizeof(int32_t))
    return (time_t)0x1ffffffff;
  else
    return (time_t)0x7fffffff;
}
