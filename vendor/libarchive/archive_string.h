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

#ifndef ARCHIVE_STRING_H_INCLUDED
#define ARCHIVE_STRING_H_INCLUDED

#ifndef __LIBARCHIVE_BUILD
#ifndef __LIBARCHIVE_TEST

#if __has_include(<sys/types.h>)
#include <sys/types.h>
#endif
#endif
#endif

#include <stdarg.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_WCHAR_H
#include <wchar.h>
#endif

#include "archive.h"

struct archive_string {
  char *s;
  size_t length;
  size_t buffer_length;
};

struct archive_wstring {
  wchar_t *s;
  size_t length;
  size_t buffer_length;
};

struct archive_string_conv;

#define archive_string_init(a)                                                 \
  do {                                                                         \
    (a)->s = NULL;                                                             \
    (a)->length = 0;                                                           \
    (a)->buffer_length = 0;                                                    \
  } while (0)

struct archive_string *archive_strappend_char(struct archive_string *, char);

struct archive_wstring *archive_wstrappend_wchar(struct archive_wstring *,
                                                 wchar_t);

struct archive_string *archive_array_append(struct archive_string *,
                                            const char *, size_t);

int archive_string_append_from_wcs(struct archive_string *, const wchar_t *,
                                   size_t);

struct archive_string_conv *
archive_string_conversion_to_charset(struct archive *, const char *, int);
struct archive_string_conv *
archive_string_conversion_from_charset(struct archive *, const char *, int);

struct archive_string_conv *
archive_string_default_conversion_for_read(struct archive *);
struct archive_string_conv *
archive_string_default_conversion_for_write(struct archive *);

void archive_string_conversion_free(struct archive *);
const char *
archive_string_conversion_charset_name(struct archive_string_conv *);
void archive_string_conversion_set_opt(struct archive_string_conv *, int);
#define SCONV_SET_OPT_UTF8_LIBARCHIVE2X 1
#define SCONV_SET_OPT_NORMALIZATION_C 2
#define SCONV_SET_OPT_NORMALIZATION_D 4

int archive_strncpy_l(struct archive_string *, const void *, size_t,
                      struct archive_string_conv *);

int archive_strncat_l(struct archive_string *, const void *, size_t,
                      struct archive_string_conv *);

#define archive_string_copy(dest, src)                                         \
  ((dest)->length = 0, archive_string_concat((dest), (src)))
#define archive_wstring_copy(dest, src)                                        \
  ((dest)->length = 0, archive_wstring_concat((dest), (src)))

void archive_string_concat(struct archive_string *dest,
                           struct archive_string *src);
void archive_wstring_concat(struct archive_wstring *dest,
                            struct archive_wstring *src);

struct archive_string *archive_string_ensure(struct archive_string *, size_t);
struct archive_wstring *archive_wstring_ensure(struct archive_wstring *,
                                               size_t);

struct archive_string *archive_strncat(struct archive_string *, const void *,
                                       size_t);
struct archive_wstring *archive_wstrncat(struct archive_wstring *,
                                         const wchar_t *, size_t);

struct archive_string *archive_strcat(struct archive_string *, const void *);
struct archive_wstring *archive_wstrcat(struct archive_wstring *,
                                        const wchar_t *);

#define archive_strcpy(as, p)                                                  \
  archive_strncpy((as), (p), ((p) == NULL ? 0 : strlen(p)))
#define archive_wstrcpy(as, p)                                                 \
  archive_wstrncpy((as), (p), ((p) == NULL ? 0 : wcslen(p)))
#define archive_strcpy_l(as, p, lo)                                            \
  archive_strncpy_l((as), (p), ((p) == NULL ? 0 : strlen(p)), (lo))

#define archive_strncpy(as, p, l)                                              \
  ((as)->length = 0, archive_strncat((as), (p), (l)))
#define archive_wstrncpy(as, p, l)                                             \
  ((as)->length = 0, archive_wstrncat((as), (p), (l)))

#define archive_strlen(a) ((a)->length)

#define archive_string_empty(a) ((a)->length = 0)
#define archive_wstring_empty(a) ((a)->length = 0)

void archive_string_free(struct archive_string *);
void archive_wstring_free(struct archive_wstring *);

void archive_string_vsprintf(struct archive_string *, const char *, va_list)
    __LA_PRINTF(2, 0);
void archive_string_sprintf(struct archive_string *, const char *, ...)
    __LA_PRINTF(2, 3);

struct archive_string *archive_string_dirname(struct archive_string *);

int archive_wstring_append_from_mbs(struct archive_wstring *dest, const char *,
                                    size_t);

struct archive_mstring {
  struct archive_string aes_mbs;
  struct archive_string aes_utf8;
  struct archive_wstring aes_wcs;
  struct archive_string aes_mbs_in_locale;

  int aes_set;
#define AES_SET_MBS 1
#define AES_SET_UTF8 2
#define AES_SET_WCS 4
};

void archive_mstring_clean(struct archive_mstring *);
void archive_mstring_copy(struct archive_mstring *dest,
                          struct archive_mstring *src);
int archive_mstring_get_mbs(struct archive *, struct archive_mstring *,
                            const char **);
int archive_mstring_get_utf8(struct archive *, struct archive_mstring *,
                             const char **);
int archive_mstring_get_wcs(struct archive *, struct archive_mstring *,
                            const wchar_t **);
int archive_mstring_get_mbs_l(struct archive *, struct archive_mstring *,
                              const char **, size_t *,
                              struct archive_string_conv *);
int archive_mstring_copy_mbs(struct archive_mstring *, const char *mbs);
int archive_mstring_copy_mbs_len(struct archive_mstring *, const char *mbs,
                                 size_t);
int archive_mstring_copy_utf8(struct archive_mstring *, const char *utf8);
int archive_mstring_copy_wcs(struct archive_mstring *, const wchar_t *wcs);
int archive_mstring_copy_wcs_len(struct archive_mstring *, const wchar_t *wcs,
                                 size_t);
int archive_mstring_copy_mbs_len_l(struct archive_mstring *, const char *mbs,
                                   size_t, struct archive_string_conv *);
int archive_mstring_update_utf8(struct archive *, struct archive_mstring *aes,
                                const char *utf8);

#endif
