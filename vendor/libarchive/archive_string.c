/*-
 * Copyright (c) 2003-2011 Tim Kientzle
 * Copyright (c) 2011-2012 Michihiro NAKAJIMA
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

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_ICONV_H
#include <iconv.h>
#endif
#ifdef HAVE_LANGINFO_H
#include <langinfo.h>
#endif
#ifdef HAVE_LOCALCHARSET_H
#include <localcharset.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_WCHAR_H
#include <wchar.h>
#endif
#if defined(_WIN32) && !defined(__CYGWIN__)
#include <locale.h>
#include <windows.h>
#endif

#include "archive_endian.h"

#ifndef ARCHIVE_ENDIAN_H_INCLUDED
#error "archive_endian.h must be included"
#endif

#include "archive_private.h"

#ifndef ARCHIVE_PRIVATE_H_INCLUDED
#error "archive_private.h must be included"
#endif

#include "archive_string.h"

#ifndef ARCHIVE_STRING_H_INCLUDED
#error "archive_string.h must be included"
#endif

#include "archive_string_composition.h"

#ifndef ARCHIVE_STRING_COMPOSITION_H_INCLUDED
#error "archive_string_composition.h must be included"
#endif

#if !defined(HAVE_WMEMCPY) && !defined(wmemcpy)
#define wmemcpy(a, b, i) (wchar_t *)memcpy((a), (b), (i) * sizeof(wchar_t))
#endif

#if !defined(HAVE_WMEMMOVE) && !defined(wmemmove)
#define wmemmove(a, b, i) (wchar_t *)memmove((a), (b), (i) * sizeof(wchar_t))
#endif

#undef max
#define max(a, b) ((a) > (b) ? (a) : (b))

struct archive_string_conv {
  struct archive_string_conv *next;
  char *from_charset;
  char *to_charset;
  unsigned from_cp;
  unsigned to_cp;

  int same;
  int flag;
#define SCONV_TO_CHARSET 1

#define SCONV_FROM_CHARSET (1 << 1)

#define SCONV_BEST_EFFORT (1 << 2)
#define SCONV_WIN_CP (1 << 3)

#define SCONV_UTF8_LIBARCHIVE_2 (1 << 4)

#define SCONV_NORMALIZATION_C (1 << 6)

#define SCONV_NORMALIZATION_D (1 << 7)

#define SCONV_TO_UTF8 (1 << 8)
#define SCONV_FROM_UTF8 (1 << 9)
#define SCONV_TO_UTF16BE (1 << 10)
#define SCONV_FROM_UTF16BE (1 << 11)
#define SCONV_TO_UTF16LE (1 << 12)
#define SCONV_FROM_UTF16LE (1 << 13)
#define SCONV_TO_UTF16 (SCONV_TO_UTF16BE | SCONV_TO_UTF16LE)
#define SCONV_FROM_UTF16 (SCONV_FROM_UTF16BE | SCONV_FROM_UTF16LE)

#if HAVE_ICONV
  iconv_t cd;
  iconv_t cd_w;

#endif

  struct archive_string utftmp;
  int (*converter[2])(struct archive_string *, const void *, size_t,
                      struct archive_string_conv *);
  int nconverter;
};

#define CP_C_LOCALE 0
#define CP_UTF16LE 1200
#define CP_UTF16BE 1201

#define IS_HIGH_SURROGATE_LA(uc) ((uc) >= 0xD800 && (uc) <= 0xDBFF)
#define IS_LOW_SURROGATE_LA(uc) ((uc) >= 0xDC00 && (uc) <= 0xDFFF)
#define IS_SURROGATE_PAIR_LA(uc) ((uc) >= 0xD800 && (uc) <= 0xDFFF)
#define UNICODE_MAX 0x10FFFF
#define UNICODE_R_CHAR 0xFFFD

static const char utf8_replacement_char[] = {0xef, 0xbf, 0xbd};

static struct archive_string_conv *
find_sconv_object(struct archive *, const char *, const char *);
static void add_sconv_object(struct archive *, struct archive_string_conv *);
static struct archive_string_conv *
create_sconv_object(const char *, const char *, unsigned, int);
static void free_sconv_object(struct archive_string_conv *);
static struct archive_string_conv *
get_sconv_object(struct archive *, const char *, const char *, int);
static unsigned make_codepage_from_charset(const char *);
static unsigned get_current_codepage(void);
static unsigned get_current_oemcp(void);
static size_t mbsnbytes(const void *, size_t);
static size_t utf16nbytes(const void *, size_t);
#if defined(_WIN32) && !defined(__CYGWIN__)
static int
archive_wstring_append_from_mbs_in_codepage(struct archive_wstring *,
                                            const char *, size_t,
                                            struct archive_string_conv *);
static int
archive_string_append_from_wcs_in_codepage(struct archive_string *,
                                           const wchar_t *, size_t,
                                           struct archive_string_conv *);
static int strncat_in_codepage(struct archive_string *, const void *, size_t,
                               struct archive_string_conv *);
static int win_strncat_from_utf16be(struct archive_string *, const void *,
                                    size_t, struct archive_string_conv *);
static int win_strncat_from_utf16le(struct archive_string *, const void *,
                                    size_t, struct archive_string_conv *);
static int win_strncat_to_utf16be(struct archive_string *, const void *, size_t,
                                  struct archive_string_conv *);
static int win_strncat_to_utf16le(struct archive_string *, const void *, size_t,
                                  struct archive_string_conv *);
#endif
static int best_effort_strncat_from_utf16be(struct archive_string *,
                                            const void *, size_t,
                                            struct archive_string_conv *);
static int best_effort_strncat_from_utf16le(struct archive_string *,
                                            const void *, size_t,
                                            struct archive_string_conv *);
static int best_effort_strncat_to_utf16be(struct archive_string *, const void *,
                                          size_t, struct archive_string_conv *);
static int best_effort_strncat_to_utf16le(struct archive_string *, const void *,
                                          size_t, struct archive_string_conv *);
#if defined(HAVE_ICONV)
static int iconv_strncat_in_locale(struct archive_string *, const void *,
                                   size_t, struct archive_string_conv *);
#endif
static int best_effort_strncat_in_locale(struct archive_string *, const void *,
                                         size_t, struct archive_string_conv *);
static int _utf8_to_unicode(uint32_t *, const char *, size_t);
static int utf8_to_unicode(uint32_t *, const char *, size_t);
static inline uint32_t combine_surrogate_pair(uint32_t, uint32_t);
static int cesu8_to_unicode(uint32_t *, const char *, size_t);
static size_t unicode_to_utf8(char *, size_t, uint32_t);
static int utf16_to_unicode(uint32_t *, const char *, size_t, int);
static size_t unicode_to_utf16be(char *, size_t, uint32_t);
static size_t unicode_to_utf16le(char *, size_t, uint32_t);
static int strncat_from_utf8_libarchive2(struct archive_string *, const void *,
                                         size_t, struct archive_string_conv *);
static int strncat_from_utf8_to_utf8(struct archive_string *, const void *,
                                     size_t, struct archive_string_conv *);
static int archive_string_normalize_C(struct archive_string *, const void *,
                                      size_t, struct archive_string_conv *);
static int archive_string_normalize_D(struct archive_string *, const void *,
                                      size_t, struct archive_string_conv *);
static int archive_string_append_unicode(struct archive_string *, const void *,
                                         size_t, struct archive_string_conv *);

#if defined __LITTLE_ENDIAN__
#define IS_BIG_ENDIAN 0
#elif defined __BIG_ENDIAN__
#define IS_BIG_ENDIAN 1
#elif defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) &&           \
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define IS_BIG_ENDIAN 0
#elif defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) &&              \
    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define IS_BIG_ENDIAN 1
#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_AMD64) ||           \
                            defined(_M_X64) || defined(_M_ARM64))
#define IS_BIG_ENDIAN 0
#else

static int is_big_endian(void) {
  uint16_t d = 1;

  return (archive_be16dec(&d) == 1);
}

#define IS_BIG_ENDIAN is_big_endian()
#endif

static struct archive_string *archive_string_append(struct archive_string *as,
                                                    const char *p, size_t s) {
  if (archive_string_ensure(as, as->length + s + 1) == NULL)
    return (NULL);
  if (s)
    memmove(as->s + as->length, p, s);
  as->length += s;
  as->s[as->length] = 0;
  return (as);
}

static struct archive_wstring *
archive_wstring_append(struct archive_wstring *as, const wchar_t *p, size_t s) {
  if (archive_wstring_ensure(as, as->length + s + 1) == NULL)
    return (NULL);
  if (s)
    wmemmove(as->s + as->length, p, s);
  as->length += s;
  as->s[as->length] = 0;
  return (as);
}

struct archive_string *archive_array_append(struct archive_string *as,
                                            const char *p, size_t s) {
  return archive_string_append(as, p, s);
}

void archive_string_concat(struct archive_string *dest,
                           struct archive_string *src) {
  if (archive_string_append(dest, src->s, src->length) == NULL)
    __archive_errx(1, "Out of memory");
}

void archive_wstring_concat(struct archive_wstring *dest,
                            struct archive_wstring *src) {
  if (archive_wstring_append(dest, src->s, src->length) == NULL)
    __archive_errx(1, "Out of memory");
}

void archive_string_free(struct archive_string *as) {
  as->length = 0;
  as->buffer_length = 0;
  free(as->s);
  as->s = NULL;
}

void archive_wstring_free(struct archive_wstring *as) {
  as->length = 0;
  as->buffer_length = 0;
  free(as->s);
  as->s = NULL;
}

struct archive_wstring *archive_wstring_ensure(struct archive_wstring *as,
                                               size_t s) {
  return (struct archive_wstring *)archive_string_ensure(
      (struct archive_string *)as, s * sizeof(wchar_t));
}

struct archive_string *archive_string_ensure(struct archive_string *as,
                                             size_t s) {
  char *p;
  size_t new_length;

  if (as->s && (s <= as->buffer_length))
    return (as);

  if (as->buffer_length < 32)

    new_length = 32;
  else if (as->buffer_length < 8192)

    new_length = as->buffer_length + as->buffer_length;
  else {

    new_length = as->buffer_length + as->buffer_length / 4;

    if (new_length < as->buffer_length) {

      archive_string_free(as);
      errno = ENOMEM;
      return (NULL);
    }
  }

  if (new_length < s)
    new_length = s;

  p = realloc(as->s, new_length);
  if (p == NULL) {

    archive_string_free(as);
    errno = ENOMEM;
    return (NULL);
  }

  as->s = p;
  as->buffer_length = new_length;
  return (as);
}

struct archive_string *archive_strncat(struct archive_string *as,
                                       const void *_p, size_t n) {
  size_t s;
  const char *p, *pp;

  p = (const char *)_p;

  s = 0;
  pp = p;
  while (s < n && *pp) {
    pp++;
    s++;
  }
  if ((as = archive_string_append(as, p, s)) == NULL)
    __archive_errx(1, "Out of memory");
  return (as);
}

struct archive_wstring *archive_wstrncat(struct archive_wstring *as,
                                         const wchar_t *p, size_t n) {
  size_t s;
  const wchar_t *pp;

  s = 0;
  pp = p;
  while (s < n && *pp) {
    pp++;
    s++;
  }
  if ((as = archive_wstring_append(as, p, s)) == NULL)
    __archive_errx(1, "Out of memory");
  return (as);
}

struct archive_string *archive_strcat(struct archive_string *as,
                                      const void *p) {

  return archive_strncat(as, p, 0x1000000);
}

struct archive_wstring *archive_wstrcat(struct archive_wstring *as,
                                        const wchar_t *p) {

  return archive_wstrncat(as, p, 0x1000000);
}

struct archive_string *archive_strappend_char(struct archive_string *as,
                                              char c) {
  if ((as = archive_string_append(as, &c, 1)) == NULL)
    __archive_errx(1, "Out of memory");
  return (as);
}

struct archive_wstring *archive_wstrappend_wchar(struct archive_wstring *as,
                                                 wchar_t c) {
  if ((as = archive_wstring_append(as, &c, 1)) == NULL)
    __archive_errx(1, "Out of memory");
  return (as);
}

static const char *default_iconv_charset(const char *charset) {
  if (charset != NULL && charset[0] != '\0')
    return charset;
#if HAVE_LOCALE_CHARSET && !defined(__APPLE__)

  return locale_charset();
#elif HAVE_NL_LANGINFO
  return nl_langinfo(CODESET);
#else
  return "";
#endif
}

#if defined(_WIN32) && !defined(__CYGWIN__)

int archive_wstring_append_from_mbs(struct archive_wstring *dest, const char *p,
                                    size_t len) {
  return archive_wstring_append_from_mbs_in_codepage(dest, p, len, NULL);
}

static int
archive_wstring_append_from_mbs_in_codepage(struct archive_wstring *dest,
                                            const char *s, size_t length,
                                            struct archive_string_conv *sc) {
  int ret = 0;
  size_t count;
  UINT from_cp;

  if (sc != NULL)
    from_cp = sc->from_cp;
  else
    from_cp = get_current_codepage();

  if (from_cp == CP_C_LOCALE) {

    wchar_t *ws;
    const unsigned char *mp;

    if (NULL == archive_wstring_ensure(dest, dest->length + length + 1))
      return (-1);

    ws = dest->s + dest->length;
    mp = (const unsigned char *)s;
    count = 0;
    while (count < length && *mp) {
      *ws++ = (wchar_t)*mp++;
      count++;
    }
  } else if (sc != NULL &&
             (sc->flag & (SCONV_NORMALIZATION_C | SCONV_NORMALIZATION_D))) {

    struct archive_string u16;
    int saved_flag = sc->flag;

    if (IS_BIG_ENDIAN)
      sc->flag |= SCONV_TO_UTF16BE;
    else
      sc->flag |= SCONV_TO_UTF16LE;

    if (sc->flag & SCONV_FROM_UTF16) {

      count = utf16nbytes(s, length);
    } else {

      count = mbsnbytes(s, length);
    }
    u16.s = (char *)dest->s;
    u16.length = dest->length << 1;
    u16.buffer_length = dest->buffer_length;
    if (sc->flag & SCONV_NORMALIZATION_C)
      ret = archive_string_normalize_C(&u16, s, count, sc);
    else
      ret = archive_string_normalize_D(&u16, s, count, sc);
    dest->s = (wchar_t *)u16.s;
    dest->length = u16.length >> 1;
    dest->buffer_length = u16.buffer_length;
    sc->flag = saved_flag;
    return (ret);
  } else if (sc != NULL && (sc->flag & SCONV_FROM_UTF16)) {
    count = utf16nbytes(s, length);
    count >>= 1;

    if (NULL == archive_wstring_ensure(dest, dest->length + count + 1))
      return (-1);
    wmemcpy(dest->s + dest->length, (const wchar_t *)s, count);
    if ((sc->flag & SCONV_FROM_UTF16BE) && !IS_BIG_ENDIAN) {
      uint16_t *u16 = (uint16_t *)(dest->s + dest->length);
      size_t b;
      for (b = 0; b < count; b++) {
        uint16_t val = archive_le16dec(u16 + b);
        archive_be16enc(u16 + b, val);
      }
    } else if ((sc->flag & SCONV_FROM_UTF16LE) && IS_BIG_ENDIAN) {
      uint16_t *u16 = (uint16_t *)(dest->s + dest->length);
      size_t b;
      for (b = 0; b < count; b++) {
        uint16_t val = archive_be16dec(u16 + b);
        archive_le16enc(u16 + b, val);
      }
    }
  } else {
    DWORD mbflag;
    size_t buffsize;

    if (sc == NULL)
      mbflag = 0;
    else if (sc->flag & SCONV_FROM_CHARSET) {

      length = mbsnbytes(s, length);
      mbflag = 0;
    } else
      mbflag = MB_PRECOMPOSED;

    mbflag |= MB_ERR_INVALID_CHARS;

    buffsize = dest->length + length + 1;
    do {
      int r;

      if (length > (size_t)INT_MAX ||
          (dest->buffer_length >> 1) > (size_t)INT_MAX)
        return (-1);

      if (NULL == archive_wstring_ensure(dest, buffsize))
        return (-1);

      r = MultiByteToWideChar(from_cp, mbflag, s, (int)length,
                              dest->s + dest->length,
                              (int)(dest->buffer_length >> 1) - 1);
      if (r == 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {

        buffsize = dest->buffer_length << 1;
        continue;
      }
      if (r == 0 && length != 0)
        ret = -1;
      count = (size_t)r;
      break;
    } while (1);
  }
  dest->length += count;
  dest->s[dest->length] = L'\0';
  return (ret);
}

#else

int archive_wstring_append_from_mbs(struct archive_wstring *dest, const char *p,
                                    size_t len) {
  size_t r;
  int ret_val = 0;

  size_t mbs_length = len;
  const char *mbs = p;
  wchar_t *wcs;
#if HAVE_MBRTOWC
  mbstate_t shift_state;

  memset(&shift_state, 0, sizeof(shift_state));
#endif

  if (NULL == archive_wstring_ensure(dest, dest->length + len + 1))
    return (-1);
  wcs = dest->s + dest->length;

  while (*mbs && mbs_length > 0) {

#if HAVE_MBRTOWC
    r = mbrtowc(wcs, mbs, mbs_length, &shift_state);
#else
    r = mbtowc(wcs, mbs, mbs_length);
#endif
    if (r == (size_t)-1 || r == (size_t)-2) {
      ret_val = -1;
      break;
    }
    if (r == 0 || r > mbs_length)
      break;
    wcs++;

    mbs += r;
    mbs_length -= r;
  }
  dest->length = wcs - dest->s;
  dest->s[dest->length] = L'\0';
  return (ret_val);
}

#endif

#if defined(_WIN32) && !defined(__CYGWIN__)

int archive_string_append_from_wcs(struct archive_string *as, const wchar_t *w,
                                   size_t len) {
  return archive_string_append_from_wcs_in_codepage(as, w, len, NULL);
}

static int
archive_string_append_from_wcs_in_codepage(struct archive_string *as,
                                           const wchar_t *ws, size_t len,
                                           struct archive_string_conv *sc) {
  BOOL defchar_used, *dp;
  int ret = 0;
  UINT to_cp;
  size_t count, wslen = len;

  if (sc != NULL)
    to_cp = sc->to_cp;
  else
    to_cp = get_current_codepage();

  if (to_cp == CP_C_LOCALE) {

    const wchar_t *wp = ws;
    char *p;

    if (NULL == archive_string_ensure(as, as->length + wslen + 1))
      return (-1);
    p = as->s + as->length;
    count = 0;
    defchar_used = 0;
    while (count < wslen && *wp) {
      if (*wp > 255) {
        *p++ = '?';
        wp++;
        defchar_used = 1;
      } else
        *p++ = (char)*wp++;
      count++;
    }
  } else if (sc != NULL && (sc->flag & SCONV_TO_UTF16)) {
    uint16_t *u16;

    if (NULL == archive_string_ensure(as, as->length + len * 2 + 2))
      return (-1);
    u16 = (uint16_t *)(as->s + as->length);
    count = 0;
    defchar_used = 0;
    if (sc->flag & SCONV_TO_UTF16BE) {
      while (count < len && *ws) {
        archive_be16enc(u16 + count, *ws);
        ws++;
        count++;
      }
    } else {
      while (count < len && *ws) {
        archive_le16enc(u16 + count, *ws);
        ws++;
        count++;
      }
    }
    count <<= 1;
  } else {

    if (NULL == archive_string_ensure(as, as->length + len * 2 + 1))
      return (-1);
    do {
      int r;

      defchar_used = 0;
      if (to_cp == CP_UTF8)
        dp = NULL;
      else
        dp = &defchar_used;

      if (as->buffer_length - as->length - 1 > (size_t)INT_MAX ||
          wslen > (size_t)INT_MAX)
        return (-1);
      r = WideCharToMultiByte(to_cp, 0, ws, (int)wslen, as->s + as->length,
                              (int)(as->buffer_length - as->length - 1), NULL,
                              dp);
      if (r == 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {

        if (NULL == archive_string_ensure(as, as->buffer_length + len))
          return (-1);
        continue;
      }
      if (r == 0)
        ret = -1;
      count = (size_t)r;
      break;
    } while (1);
  }
  as->length += count;
  as->s[as->length] = '\0';
  return (defchar_used ? -1 : ret);
}

#elif defined(HAVE_WCTOMB) || defined(HAVE_WCRTOMB)

int archive_string_append_from_wcs(struct archive_string *as, const wchar_t *w,
                                   size_t len) {

  int n, ret_val = 0;
  char *p;
  char *end;
#if HAVE_WCRTOMB
  mbstate_t shift_state;

  memset(&shift_state, 0, sizeof(shift_state));
#else

  if (wctomb(NULL, L'\0') < 0) {
  }
#endif

  if (archive_string_ensure(as, as->length + len + 1) == NULL)
    return (-1);

  p = as->s + as->length;
  end = as->s + as->buffer_length - MB_CUR_MAX - 1;
  while (*w != L'\0' && len > 0) {
    if (p >= end) {
      as->length = p - as->s;
      as->s[as->length] = '\0';

      if (archive_string_ensure(
              as, as->length + max(len * 2, (size_t)MB_CUR_MAX) + 1) == NULL)
        return (-1);
      p = as->s + as->length;
      end = as->s + as->buffer_length - MB_CUR_MAX - 1;
    }
#if HAVE_WCRTOMB
    n = wcrtomb(p, *w++, &shift_state);
#else
    n = wctomb(p, *w++);
#endif
    if (n == -1) {
      if (errno == EILSEQ) {

        *p++ = '?';
        ret_val = -1;
      } else {
        ret_val = -1;
        break;
      }
    } else
      p += n;
    len--;
  }
  as->length = p - as->s;
  as->s[as->length] = '\0';
  return (ret_val);
}

#else

int archive_string_append_from_wcs(struct archive_string *as, const wchar_t *w,
                                   size_t len) {
  (void)as;
  (void)w;
  (void)len;
  errno = ENOSYS;
  return (-1);
}

#endif

static struct archive_string_conv *
find_sconv_object(struct archive *a, const char *fc, const char *tc) {
  struct archive_string_conv *sc;

  if (a == NULL)
    return (NULL);

  for (sc = a->sconv; sc != NULL; sc = sc->next) {
    if (strcmp(sc->from_charset, fc) == 0 && strcmp(sc->to_charset, tc) == 0)
      break;
  }
  return (sc);
}

static void add_sconv_object(struct archive *a,
                             struct archive_string_conv *sc) {
  struct archive_string_conv **psc;

  psc = &(a->sconv);
  while (*psc != NULL)
    psc = &((*psc)->next);
  *psc = sc;
}

static void add_converter(struct archive_string_conv *sc,
                          int (*converter)(struct archive_string *,
                                           const void *, size_t,
                                           struct archive_string_conv *)) {
  if (sc == NULL || sc->nconverter >= 2)
    __archive_errx(1, "Programming error");
  sc->converter[sc->nconverter++] = converter;
}

static void setup_converter(struct archive_string_conv *sc) {

  sc->nconverter = 0;

  if (sc->flag & SCONV_UTF8_LIBARCHIVE_2) {
    add_converter(sc, strncat_from_utf8_libarchive2);
    return;
  }

  if (sc->flag & SCONV_TO_UTF16) {

    if (sc->flag & SCONV_FROM_UTF8) {
      add_converter(sc, archive_string_append_unicode);
      return;
    }

#if defined(_WIN32) && !defined(__CYGWIN__)
    if (sc->flag & SCONV_WIN_CP) {
      if (sc->flag & SCONV_TO_UTF16BE)
        add_converter(sc, win_strncat_to_utf16be);
      else
        add_converter(sc, win_strncat_to_utf16le);
      return;
    }
#endif

#if defined(HAVE_ICONV)
    if (sc->cd != (iconv_t)-1) {
      add_converter(sc, iconv_strncat_in_locale);
      return;
    }
#endif

    if (sc->flag & SCONV_BEST_EFFORT) {
      if (sc->flag & SCONV_TO_UTF16BE)
        add_converter(sc, best_effort_strncat_to_utf16be);
      else
        add_converter(sc, best_effort_strncat_to_utf16le);
    } else

      sc->nconverter = 0;
    return;
  }

  if (sc->flag & SCONV_FROM_UTF16) {

    if (sc->flag & SCONV_NORMALIZATION_D)
      add_converter(sc, archive_string_normalize_D);
    else if (sc->flag & SCONV_NORMALIZATION_C)
      add_converter(sc, archive_string_normalize_C);

    if (sc->flag & SCONV_TO_UTF8) {

      if (!(sc->flag & (SCONV_NORMALIZATION_D | SCONV_NORMALIZATION_C)))
        add_converter(sc, archive_string_append_unicode);
      return;
    }

#if defined(_WIN32) && !defined(__CYGWIN__)
    if (sc->flag & SCONV_WIN_CP) {
      if (sc->flag & SCONV_FROM_UTF16BE)
        add_converter(sc, win_strncat_from_utf16be);
      else
        add_converter(sc, win_strncat_from_utf16le);
      return;
    }
#endif

#if defined(HAVE_ICONV)
    if (sc->cd != (iconv_t)-1) {
      add_converter(sc, iconv_strncat_in_locale);
      return;
    }
#endif

    if ((sc->flag & (SCONV_BEST_EFFORT | SCONV_FROM_UTF16BE)) ==
        (SCONV_BEST_EFFORT | SCONV_FROM_UTF16BE))
      add_converter(sc, best_effort_strncat_from_utf16be);
    else if ((sc->flag & (SCONV_BEST_EFFORT | SCONV_FROM_UTF16LE)) ==
             (SCONV_BEST_EFFORT | SCONV_FROM_UTF16LE))
      add_converter(sc, best_effort_strncat_from_utf16le);
    else

      sc->nconverter = 0;
    return;
  }

  if (sc->flag & SCONV_FROM_UTF8) {

    if (sc->flag & SCONV_NORMALIZATION_D)
      add_converter(sc, archive_string_normalize_D);
    else if (sc->flag & SCONV_NORMALIZATION_C)
      add_converter(sc, archive_string_normalize_C);

    if (sc->flag & SCONV_TO_UTF8) {

      if (!(sc->flag & (SCONV_NORMALIZATION_D | SCONV_NORMALIZATION_C)))
        add_converter(sc, strncat_from_utf8_to_utf8);
      return;
    }
  }

#if defined(_WIN32) && !defined(__CYGWIN__)

  if (sc->flag & SCONV_WIN_CP) {
    add_converter(sc, strncat_in_codepage);
    return;
  }
#endif

#if HAVE_ICONV
  if (sc->cd != (iconv_t)-1) {
    add_converter(sc, iconv_strncat_in_locale);

    if ((sc->flag & SCONV_FROM_CHARSET) && (sc->flag & SCONV_TO_UTF8)) {
      if (sc->flag & SCONV_NORMALIZATION_D)
        add_converter(sc, archive_string_normalize_D);
    }
    return;
  }
#endif

  if ((sc->flag & SCONV_BEST_EFFORT) || sc->same)
    add_converter(sc, best_effort_strncat_in_locale);
  else

    sc->nconverter = 0;
}

static const char *canonical_charset_name(const char *charset) {
  char cs[16];
  char *p;
  const char *s;

  if (charset == NULL || charset[0] == '\0' || strlen(charset) > 15)
    return (charset);

  p = cs;
  s = charset;
  while (*s) {
    char c = *s++;
    if (c >= 'a' && c <= 'z')
      c -= 'a' - 'A';
    *p++ = c;
  }
  *p++ = '\0';

  if (strcmp(cs, "UTF-8") == 0 || strcmp(cs, "UTF8") == 0)
    return ("UTF-8");
  if (strcmp(cs, "UTF-16BE") == 0 || strcmp(cs, "UTF16BE") == 0)
    return ("UTF-16BE");
  if (strcmp(cs, "UTF-16LE") == 0 || strcmp(cs, "UTF16LE") == 0)
    return ("UTF-16LE");
  if (strcmp(cs, "CP932") == 0)
    return ("CP932");
  return (charset);
}

static struct archive_string_conv *
create_sconv_object(const char *fc, const char *tc, unsigned current_codepage,
                    int flag) {
  struct archive_string_conv *sc;

  sc = calloc(1, sizeof(*sc));
  if (sc == NULL)
    return (NULL);
  sc->next = NULL;
  sc->from_charset = strdup(fc);
  if (sc->from_charset == NULL) {
    free(sc);
    return (NULL);
  }
  sc->to_charset = strdup(tc);
  if (sc->to_charset == NULL) {
    free(sc->from_charset);
    free(sc);
    return (NULL);
  }
  archive_string_init(&sc->utftmp);

  if (flag & SCONV_TO_CHARSET) {

    sc->from_cp = current_codepage;
    sc->to_cp = make_codepage_from_charset(tc);
#if defined(_WIN32) && !defined(__CYGWIN__)
    if (IsValidCodePage(sc->to_cp))
      flag |= SCONV_WIN_CP;
#endif
  } else if (flag & SCONV_FROM_CHARSET) {

    sc->to_cp = current_codepage;
    sc->from_cp = make_codepage_from_charset(fc);
#if defined(_WIN32) && !defined(__CYGWIN__)
    if (IsValidCodePage(sc->from_cp))
      flag |= SCONV_WIN_CP;
#endif
  }

  if (strcmp(fc, tc) == 0 ||
      (sc->from_cp != (unsigned)-1 && sc->from_cp == sc->to_cp))
    sc->same = 1;
  else
    sc->same = 0;

  if (strcmp(tc, "UTF-8") == 0)
    flag |= SCONV_TO_UTF8;
  else if (strcmp(tc, "UTF-16BE") == 0)
    flag |= SCONV_TO_UTF16BE;
  else if (strcmp(tc, "UTF-16LE") == 0)
    flag |= SCONV_TO_UTF16LE;
  if (strcmp(fc, "UTF-8") == 0)
    flag |= SCONV_FROM_UTF8;
  else if (strcmp(fc, "UTF-16BE") == 0)
    flag |= SCONV_FROM_UTF16BE;
  else if (strcmp(fc, "UTF-16LE") == 0)
    flag |= SCONV_FROM_UTF16LE;
#if defined(_WIN32) && !defined(__CYGWIN__)
  if (sc->to_cp == CP_UTF8)
    flag |= SCONV_TO_UTF8;
  else if (sc->to_cp == CP_UTF16BE)
    flag |= SCONV_TO_UTF16BE | SCONV_WIN_CP;
  else if (sc->to_cp == CP_UTF16LE)
    flag |= SCONV_TO_UTF16LE | SCONV_WIN_CP;
  if (sc->from_cp == CP_UTF8)
    flag |= SCONV_FROM_UTF8;
  else if (sc->from_cp == CP_UTF16BE)
    flag |= SCONV_FROM_UTF16BE | SCONV_WIN_CP;
  else if (sc->from_cp == CP_UTF16LE)
    flag |= SCONV_FROM_UTF16LE | SCONV_WIN_CP;
#endif

  if ((flag & SCONV_FROM_CHARSET) &&
      (flag & (SCONV_FROM_UTF16 | SCONV_FROM_UTF8))) {
#if defined(__APPLE__)
    if (flag & SCONV_TO_UTF8)
      flag |= SCONV_NORMALIZATION_D;
    else
#endif
      flag |= SCONV_NORMALIZATION_C;
  }
#if defined(__APPLE__)

  if ((flag & SCONV_TO_CHARSET) &&
      (flag & (SCONV_FROM_UTF16 | SCONV_FROM_UTF8)) &&
      !(flag & (SCONV_TO_UTF16 | SCONV_TO_UTF8)))
    flag |= SCONV_NORMALIZATION_C;

  if ((flag & SCONV_FROM_CHARSET) &&
      !(flag & (SCONV_FROM_UTF16 | SCONV_FROM_UTF8)) && (flag & SCONV_TO_UTF8))
    flag |= SCONV_NORMALIZATION_D;
#endif

#if defined(HAVE_ICONV)
  sc->cd_w = (iconv_t)-1;

  if (((flag & (SCONV_TO_UTF8 | SCONV_TO_UTF16)) &&
       (flag & (SCONV_FROM_UTF8 | SCONV_FROM_UTF16))) ||
      (flag & SCONV_WIN_CP)) {

    sc->cd = (iconv_t)-1;
  } else {
    sc->cd = iconv_open(tc, fc);
    if (sc->cd == (iconv_t)-1 && (sc->flag & SCONV_BEST_EFFORT)) {

      if (strcmp(tc, "CP932") == 0)
        sc->cd = iconv_open("SJIS", fc);
      else if (strcmp(fc, "CP932") == 0)
        sc->cd = iconv_open(tc, "SJIS");
    }
#if defined(_WIN32) && !defined(__CYGWIN__)

    if (flag & SCONV_FROM_CHARSET) {
      sc->cd_w = iconv_open("UTF-8", fc);
      if (sc->cd_w == (iconv_t)-1 && (sc->flag & SCONV_BEST_EFFORT)) {
        if (strcmp(fc, "CP932") == 0)
          sc->cd_w = iconv_open("UTF-8", "SJIS");
      }
    }
#endif
  }
#endif

  sc->flag = flag;

  setup_converter(sc);

  return (sc);
}

static void free_sconv_object(struct archive_string_conv *sc) {
  free(sc->from_charset);
  free(sc->to_charset);
  archive_string_free(&sc->utftmp);
#if HAVE_ICONV
  if (sc->cd != (iconv_t)-1)
    iconv_close(sc->cd);
  if (sc->cd_w != (iconv_t)-1)
    iconv_close(sc->cd_w);
#endif
  free(sc);
}

#if defined(_WIN32) && !defined(__CYGWIN__)
#if defined(WINAPI_FAMILY_PARTITION) &&                                        \
    !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#define GetOEMCP() CP_OEMCP
#endif

static unsigned my_atoi(const char *p) {
  unsigned cp;

  cp = 0;
  while (*p) {
    if (*p >= '0' && *p <= '9')
      cp = cp * 10 + (*p - '0');
    else
      return (-1);
    p++;
  }
  return (cp);
}

static struct charset {
  const char *name;
  unsigned cp;
} charsets[] = {

    {"ASCII", 1252},
    {"ASMO-708", 708},
    {"BIG5", 950},
    {"CHINESE", 936},
    {"CP367", 1252},
    {"CP819", 1252},
    {"CP1025", 21025},
    {"DOS-720", 720},
    {"DOS-862", 862},
    {"EUC-CN", 51936},
    {"EUC-JP", 51932},
    {"EUC-KR", 949},
    {"EUCCN", 51936},
    {"EUCJP", 51932},
    {"EUCKR", 949},
    {"GB18030", 54936},
    {"GB2312", 936},
    {"HEBREW", 1255},
    {"HZ-GB-2312", 52936},
    {"IBM273", 20273},
    {"IBM277", 20277},
    {"IBM278", 20278},
    {"IBM280", 20280},
    {"IBM284", 20284},
    {"IBM285", 20285},
    {"IBM290", 20290},
    {"IBM297", 20297},
    {"IBM367", 1252},
    {"IBM420", 20420},
    {"IBM423", 20423},
    {"IBM424", 20424},
    {"IBM819", 1252},
    {"IBM871", 20871},
    {"IBM880", 20880},
    {"IBM905", 20905},
    {"IBM924", 20924},
    {"ISO-8859-1", 28591},
    {"ISO-8859-13", 28603},
    {"ISO-8859-15", 28605},
    {"ISO-8859-2", 28592},
    {"ISO-8859-3", 28593},
    {"ISO-8859-4", 28594},
    {"ISO-8859-5", 28595},
    {"ISO-8859-6", 28596},
    {"ISO-8859-7", 28597},
    {"ISO-8859-8", 28598},
    {"ISO-8859-9", 28599},
    {"ISO8859-1", 28591},
    {"ISO8859-13", 28603},
    {"ISO8859-15", 28605},
    {"ISO8859-2", 28592},
    {"ISO8859-3", 28593},
    {"ISO8859-4", 28594},
    {"ISO8859-5", 28595},
    {"ISO8859-6", 28596},
    {"ISO8859-7", 28597},
    {"ISO8859-8", 28598},
    {"ISO8859-9", 28599},
    {"JOHAB", 1361},
    {"KOI8-R", 20866},
    {"KOI8-U", 21866},
    {"KS_C_5601-1987", 949},
    {"LATIN1", 1252},
    {"LATIN2", 28592},
    {"MACINTOSH", 10000},
    {"SHIFT-JIS", 932},
    {"SHIFT_JIS", 932},
    {"SJIS", 932},
    {"US", 1252},
    {"US-ASCII", 1252},
    {"UTF-16", 1200},
    {"UTF-16BE", 1201},
    {"UTF-16LE", 1200},
    {"UTF-8", CP_UTF8},
    {"X-EUROPA", 29001},
    {"X-MAC-ARABIC", 10004},
    {"X-MAC-CE", 10029},
    {"X-MAC-CHINESEIMP", 10008},
    {"X-MAC-CHINESETRAD", 10002},
    {"X-MAC-CROATIAN", 10082},
    {"X-MAC-CYRILLIC", 10007},
    {"X-MAC-GREEK", 10006},
    {"X-MAC-HEBREW", 10005},
    {"X-MAC-ICELANDIC", 10079},
    {"X-MAC-JAPANESE", 10001},
    {"X-MAC-KOREAN", 10003},
    {"X-MAC-ROMANIAN", 10010},
    {"X-MAC-THAI", 10021},
    {"X-MAC-TURKISH", 10081},
    {"X-MAC-UKRAINIAN", 10017},
};
static unsigned make_codepage_from_charset(const char *charset) {
  char cs[16];
  char *p;
  unsigned cp;
  int a, b;

  if (charset == NULL || strlen(charset) > 15)
    return -1;

  p = cs;
  while (*charset) {
    char c = *charset++;
    if (c >= 'a' && c <= 'z')
      c -= 'a' - 'A';
    *p++ = c;
  }
  *p++ = '\0';
  cp = -1;

  a = 0;
  b = sizeof(charsets) / sizeof(charsets[0]);
  while (b > a) {
    int c = (b + a) / 2;
    int r = strcmp(charsets[c].name, cs);
    if (r < 0)
      a = c + 1;
    else if (r > 0)
      b = c;
    else
      return charsets[c].cp;
  }

  switch (*cs) {
  case 'C':
    if (cs[1] == 'P' && cs[2] >= '0' && cs[2] <= '9') {
      cp = my_atoi(cs + 2);
    } else if (strcmp(cs, "CP_ACP") == 0)
      cp = get_current_codepage();
    else if (strcmp(cs, "CP_OEMCP") == 0)
      cp = get_current_oemcp();
    break;
  case 'I':
    if (cs[1] == 'B' && cs[2] == 'M' && cs[3] >= '0' && cs[3] <= '9') {
      cp = my_atoi(cs + 3);
    }
    break;
  case 'W':
    if (strncmp(cs, "WINDOWS-", 8) == 0) {
      cp = my_atoi(cs + 8);
      if (cp != 874 && (cp < 1250 || cp > 1258))
        cp = -1;
    }
    break;
  }
  return (cp);
}

static unsigned get_current_codepage(void) {
  char *locale, *p;
  unsigned cp;

  locale = setlocale(LC_CTYPE, NULL);
  if (locale == NULL)
    return (GetACP());
  if (locale[0] == 'C' && locale[1] == '\0')
    return (CP_C_LOCALE);
  p = strrchr(locale, '.');
  if (p == NULL)
    return (GetACP());
  if ((strcmp(p + 1, "utf8") == 0) || (strcmp(p + 1, "UTF-8") == 0))
    return CP_UTF8;
  cp = my_atoi(p + 1);
  if ((int)cp <= 0)
    return (GetACP());
  return (cp);
}

static struct {
  unsigned acp;
  unsigned ocp;
  const char *locale;
} acp_ocp_map[] = {{950, 950, "Chinese_Taiwan"},
                   {936, 936, "Chinese_People's Republic of China"},
                   {950, 950, "Chinese_Taiwan"},
                   {1250, 852, "Czech_Czech Republic"},
                   {1252, 850, "Danish_Denmark"},
                   {1252, 850, "Dutch_Netherlands"},
                   {1252, 850, "Dutch_Belgium"},
                   {1252, 437, "English_United States"},
                   {1252, 850, "English_Australia"},
                   {1252, 850, "English_Canada"},
                   {1252, 850, "English_New Zealand"},
                   {1252, 850, "English_United Kingdom"},
                   {1252, 437, "English_United States"},
                   {1252, 850, "Finnish_Finland"},
                   {1252, 850, "French_France"},
                   {1252, 850, "French_Belgium"},
                   {1252, 850, "French_Canada"},
                   {1252, 850, "French_Switzerland"},
                   {1252, 850, "German_Germany"},
                   {1252, 850, "German_Austria"},
                   {1252, 850, "German_Switzerland"},
                   {1253, 737, "Greek_Greece"},
                   {1250, 852, "Hungarian_Hungary"},
                   {1252, 850, "Icelandic_Iceland"},
                   {1252, 850, "Italian_Italy"},
                   {1252, 850, "Italian_Switzerland"},
                   {932, 932, "Japanese_Japan"},
                   {949, 949, "Korean_Korea"},
                   {1252, 850, "Norwegian (BokmOl)_Norway"},
                   {1252, 850, "Norwegian (BokmOl)_Norway"},
                   {1252, 850, "Norwegian-Nynorsk_Norway"},
                   {1250, 852, "Polish_Poland"},
                   {1252, 850, "Portuguese_Portugal"},
                   {1252, 850, "Portuguese_Brazil"},
                   {1251, 866, "Russian_Russia"},
                   {1250, 852, "Slovak_Slovakia"},
                   {1252, 850, "Spanish_Spain"},
                   {1252, 850, "Spanish_Mexico"},
                   {1252, 850, "Spanish_Spain"},
                   {1252, 850, "Swedish_Sweden"},
                   {1254, 857, "Turkish_Turkey"},
                   {0, 0, NULL}};

static unsigned get_current_oemcp(void) {
  int i;
  char *locale, *p;
  size_t len;

  locale = setlocale(LC_CTYPE, NULL);
  if (locale == NULL)
    return (GetOEMCP());
  if (locale[0] == 'C' && locale[1] == '\0')
    return (CP_C_LOCALE);

  p = strrchr(locale, '.');
  if (p == NULL)
    return (GetOEMCP());
  len = p - locale;
  for (i = 0; acp_ocp_map[i].acp; i++) {
    if (strncmp(acp_ocp_map[i].locale, locale, len) == 0)
      return (acp_ocp_map[i].ocp);
  }
  return (GetOEMCP());
}
#else

static unsigned get_current_codepage(void) { return (-1); }
static unsigned make_codepage_from_charset(const char *charset) {
  (void)charset;
  return (-1);
}
static unsigned get_current_oemcp(void) { return (-1); }

#endif

static struct archive_string_conv *
get_sconv_object(struct archive *a, const char *fc, const char *tc, int flag) {
  struct archive_string_conv *sc;
  unsigned current_codepage;

  sc = find_sconv_object(a, fc, tc);
  if (sc != NULL)
    return (sc);

  if (a == NULL)
    current_codepage = get_current_codepage();
  else
    current_codepage = a->current_codepage;

  sc = create_sconv_object(canonical_charset_name(fc),
                           canonical_charset_name(tc), current_codepage, flag);
  if (sc == NULL) {
    if (a != NULL)
      archive_set_error(a, ENOMEM,
                        "Could not allocate memory for "
                        "a string conversion object");
    return (NULL);
  }

  if (sc->nconverter == 0) {
    if (a != NULL) {
#if HAVE_ICONV
      archive_set_error(a, ARCHIVE_ERRNO_MISC,
                        "iconv_open failed : Cannot handle ``%s''",
                        (flag & SCONV_TO_CHARSET) ? tc : fc);
#else
      archive_set_error(a, ARCHIVE_ERRNO_MISC,
                        "A character-set conversion not fully supported "
                        "on this platform");
#endif
    }

    free_sconv_object(sc);
    return (NULL);
  }

  if (a != NULL)
    add_sconv_object(a, sc);
  return (sc);
}

static const char *get_current_charset(struct archive *a) {
  const char *cur_charset;

  if (a == NULL)
    cur_charset = default_iconv_charset("");
  else {
    cur_charset = default_iconv_charset(a->current_code);
    if (a->current_code == NULL) {
      a->current_code = strdup(cur_charset);
      a->current_codepage = get_current_codepage();
      a->current_oemcp = get_current_oemcp();
    }
  }
  return (cur_charset);
}

struct archive_string_conv *
archive_string_conversion_to_charset(struct archive *a, const char *charset,
                                     int best_effort) {
  int flag = SCONV_TO_CHARSET;

  if (best_effort)
    flag |= SCONV_BEST_EFFORT;
  return (get_sconv_object(a, get_current_charset(a), charset, flag));
}

struct archive_string_conv *
archive_string_conversion_from_charset(struct archive *a, const char *charset,
                                       int best_effort) {
  int flag = SCONV_FROM_CHARSET;

  if (best_effort)
    flag |= SCONV_BEST_EFFORT;
  return (get_sconv_object(a, charset, get_current_charset(a), flag));
}

#if defined(_WIN32) && !defined(__CYGWIN__)
struct archive_string_conv *
archive_string_default_conversion_for_read(struct archive *a) {
  const char *cur_charset = get_current_charset(a);
  char oemcp[16];

  if (cur_charset != NULL && (a->current_codepage == CP_C_LOCALE ||
                              a->current_codepage == a->current_oemcp))
    return (NULL);

  _snprintf(oemcp, sizeof(oemcp) - 1, "CP%d", a->current_oemcp);

  oemcp[sizeof(oemcp) - 1] = '\0';
  return (get_sconv_object(a, oemcp, cur_charset, SCONV_FROM_CHARSET));
}

struct archive_string_conv *
archive_string_default_conversion_for_write(struct archive *a) {
  const char *cur_charset = get_current_charset(a);
  char oemcp[16];

  if (cur_charset != NULL && (a->current_codepage == CP_C_LOCALE ||
                              a->current_codepage == a->current_oemcp))
    return (NULL);

  _snprintf(oemcp, sizeof(oemcp) - 1, "CP%d", a->current_oemcp);

  oemcp[sizeof(oemcp) - 1] = '\0';
  return (get_sconv_object(a, cur_charset, oemcp, SCONV_TO_CHARSET));
}
#else
struct archive_string_conv *
archive_string_default_conversion_for_read(struct archive *a) {
  (void)a;
  return (NULL);
}

struct archive_string_conv *
archive_string_default_conversion_for_write(struct archive *a) {
  (void)a;
  return (NULL);
}
#endif

void archive_string_conversion_free(struct archive *a) {
  struct archive_string_conv *sc;
  struct archive_string_conv *sc_next;

  for (sc = a->sconv; sc != NULL; sc = sc_next) {
    sc_next = sc->next;
    free_sconv_object(sc);
  }
  a->sconv = NULL;
  free(a->current_code);
  a->current_code = NULL;
}

const char *
archive_string_conversion_charset_name(struct archive_string_conv *sc) {
  if (sc == NULL) {
    return "current locale";
  }
  if (sc->flag & SCONV_TO_CHARSET)
    return (sc->to_charset);
  else
    return (sc->from_charset);
}

void archive_string_conversion_set_opt(struct archive_string_conv *sc,
                                       int opt) {
  switch (opt) {

  case SCONV_SET_OPT_UTF8_LIBARCHIVE2X:
#if (defined(_WIN32) && !defined(__CYGWIN__)) ||                               \
    defined(__STDC_ISO_10646__) || defined(__APPLE__)

    (void)sc;
#else
    if ((sc->flag & SCONV_UTF8_LIBARCHIVE_2) == 0) {
      sc->flag |= SCONV_UTF8_LIBARCHIVE_2;

      setup_converter(sc);
    }
#endif
    break;
  case SCONV_SET_OPT_NORMALIZATION_C:
    if ((sc->flag & SCONV_NORMALIZATION_C) == 0) {
      sc->flag |= SCONV_NORMALIZATION_C;
      sc->flag &= ~SCONV_NORMALIZATION_D;

      setup_converter(sc);
    }
    break;
  case SCONV_SET_OPT_NORMALIZATION_D:
#if defined(HAVE_ICONV)

    if (!(sc->flag & SCONV_WIN_CP) &&
        (sc->flag & (SCONV_FROM_UTF16 | SCONV_FROM_UTF8)) &&
        !(sc->flag & (SCONV_TO_UTF16 | SCONV_TO_UTF8)))
      break;
#endif
    if ((sc->flag & SCONV_NORMALIZATION_D) == 0) {
      sc->flag |= SCONV_NORMALIZATION_D;
      sc->flag &= ~SCONV_NORMALIZATION_C;

      setup_converter(sc);
    }
    break;
  default:
    break;
  }
}

static size_t mbsnbytes(const void *_p, size_t n) {
  size_t s;
  const char *p, *pp;

  if (_p == NULL)
    return (0);
  p = (const char *)_p;

  s = 0;
  pp = p;
  while (s < n && *pp) {
    pp++;
    s++;
  }
  return (s);
}

static size_t utf16nbytes(const void *_p, size_t n) {
  size_t s;
  const char *p, *pp;

  if (_p == NULL)
    return (0);
  p = (const char *)_p;

  s = 0;
  pp = p;
  n >>= 1;
  while (s < n && (pp[0] || pp[1])) {
    pp += 2;
    s++;
  }
  return (s << 1);
}

int archive_strncpy_l(struct archive_string *as, const void *_p, size_t n,
                      struct archive_string_conv *sc) {
  as->length = 0;
  return (archive_strncat_l(as, _p, n, sc));
}

int archive_strncat_l(struct archive_string *as, const void *_p, size_t n,
                      struct archive_string_conv *sc) {
  const void *s;
  size_t length = 0;
  int i, r = 0, r2;

  if (_p != NULL && n > 0) {
    if (sc != NULL && (sc->flag & SCONV_FROM_UTF16))
      length = utf16nbytes(_p, n);
    else
      length = mbsnbytes(_p, n);
  }

  if (length == 0) {
    size_t tn = 1;
    if (sc != NULL && (sc->flag & SCONV_TO_UTF16))
      tn = 2;
    if (archive_string_ensure(as, as->length + tn) == NULL)
      return (-1);
    as->s[as->length] = 0;
    if (tn == 2)
      as->s[as->length + 1] = 0;
    return (0);
  }

  if (sc == NULL) {
    if (archive_string_append(as, _p, length) == NULL)
      return (-1);
    return (0);
  }

  s = _p;
  i = 0;
  if (sc->nconverter > 1) {
    sc->utftmp.length = 0;
    r2 = sc->converter[0](&(sc->utftmp), s, length, sc);
    if (r2 != 0 && errno == ENOMEM)
      return (r2);
    if (r > r2)
      r = r2;
    s = sc->utftmp.s;
    length = sc->utftmp.length;
    ++i;
  }
  r2 = sc->converter[i](as, s, length, sc);
  if (r > r2)
    r = r2;
  return (r);
}

struct archive_string *archive_string_dirname(struct archive_string *as) {

  while (as->length > 1 && as->s[as->length - 1] == '/')
    as->length--;

  while (as->length > 0 && as->s[as->length - 1] != '/')
    as->length--;

  if (as->length == 0)
    return (archive_strcat(as, "."));

  while (as->length > 1 && as->s[as->length - 1] == '/')
    as->length--;

  as->s[as->length] = '\0';
  return (as);
}

#if HAVE_ICONV

static int iconv_strncat_in_locale(struct archive_string *as, const void *_p,
                                   size_t length,
                                   struct archive_string_conv *sc) {
  ICONV_CONST char *itp;
  size_t remaining;
  iconv_t cd;
  char *outp;
  size_t avail, bs;
  int return_value = 0;
  size_t to_size, from_size;

  if (sc->flag & SCONV_TO_UTF16)
    to_size = 2;
  else
    to_size = 1;
  if (sc->flag & SCONV_FROM_UTF16)
    from_size = 2;
  else
    from_size = 1;

  if (archive_string_ensure(as, as->length + length * 2 + to_size) == NULL)
    return (-1);

  cd = sc->cd;
  itp = (char *)(uintptr_t)_p;
  remaining = length;
  outp = as->s + as->length;
  avail = as->buffer_length - as->length - to_size;
  while (remaining >= from_size) {
    size_t result = iconv(cd, &itp, &remaining, &outp, &avail);

    if (result != (size_t)-1)
      break;

    if (errno == EILSEQ || errno == EINVAL) {

      if (sc->flag & (SCONV_TO_UTF8 | SCONV_TO_UTF16)) {
        size_t rbytes;
        if (sc->flag & SCONV_TO_UTF8)
          rbytes = sizeof(utf8_replacement_char);
        else
          rbytes = 2;

        if (avail < rbytes) {
          as->length = outp - as->s;
          bs = as->buffer_length + (remaining * to_size) + rbytes;
          if (NULL == archive_string_ensure(as, bs))
            return (-1);
          outp = as->s + as->length;
          avail = as->buffer_length - as->length - to_size;
        }
        if (sc->flag & SCONV_TO_UTF8)
          memcpy(outp, utf8_replacement_char, sizeof(utf8_replacement_char));
        else if (sc->flag & SCONV_TO_UTF16BE)
          archive_be16enc(outp, UNICODE_R_CHAR);
        else
          archive_le16enc(outp, UNICODE_R_CHAR);
        outp += rbytes;
        avail -= rbytes;
      } else {

        *outp++ = '?';
        avail--;
      }
      itp += from_size;
      remaining -= from_size;
      return_value = -1;
    } else {

      as->length = outp - as->s;
      bs = as->buffer_length + remaining * 2;
      if (NULL == archive_string_ensure(as, bs))
        return (-1);
      outp = as->s + as->length;
      avail = as->buffer_length - as->length - to_size;
    }
  }
  as->length = outp - as->s;
  as->s[as->length] = 0;
  if (to_size == 2)
    as->s[as->length + 1] = 0;
  return (return_value);
}

#endif

#if defined(_WIN32) && !defined(__CYGWIN__)

static int strncat_in_codepage(struct archive_string *as, const void *_p,
                               size_t length, struct archive_string_conv *sc) {
  const char *s = (const char *)_p;
  struct archive_wstring aws;
  size_t l;
  int r, saved_flag;

  archive_string_init(&aws);
  saved_flag = sc->flag;
  sc->flag &= ~(SCONV_NORMALIZATION_D | SCONV_NORMALIZATION_C);
  r = archive_wstring_append_from_mbs_in_codepage(&aws, s, length, sc);
  sc->flag = saved_flag;
  if (r != 0) {
    archive_wstring_free(&aws);
    if (errno != ENOMEM)
      archive_string_append(as, s, length);
    return (-1);
  }

  l = as->length;
  r = archive_string_append_from_wcs_in_codepage(as, aws.s, aws.length, sc);
  if (r != 0 && errno != ENOMEM && l == as->length)
    archive_string_append(as, s, length);
  archive_wstring_free(&aws);
  return (r);
}

static int invalid_mbs(const void *_p, size_t n,
                       struct archive_string_conv *sc) {
  const char *p = (const char *)_p;
  unsigned codepage;
  DWORD mbflag = MB_ERR_INVALID_CHARS;

  if (sc->flag & SCONV_FROM_CHARSET)
    codepage = sc->to_cp;
  else
    codepage = sc->from_cp;

  if (codepage == CP_C_LOCALE)
    return (0);
  if (codepage != CP_UTF8)
    mbflag |= MB_PRECOMPOSED;

  if (n > (size_t)INT_MAX)
    return (-1);
  if (MultiByteToWideChar(codepage, mbflag, p, (int)n, NULL, 0) == 0)
    return (-1);
  return (0);
}

#else

static int invalid_mbs(const void *_p, size_t n,
                       struct archive_string_conv *sc) {
  const char *p = (const char *)_p;
  size_t r;

#if HAVE_MBRTOWC
  mbstate_t shift_state;

  memset(&shift_state, 0, sizeof(shift_state));
#else

  mbtowc(NULL, NULL, 0);
#endif
  while (n) {
    wchar_t wc;

#if HAVE_MBRTOWC
    r = mbrtowc(&wc, p, n, &shift_state);
#else
    r = mbtowc(&wc, p, n);
#endif
    if (r == (size_t)-1 || r == (size_t)-2)
      return (-1);
    if (r == 0)
      break;
    p += r;
    n -= r;
  }
  (void)sc;
  return (0);
}

#endif

static int best_effort_strncat_in_locale(struct archive_string *as,
                                         const void *_p, size_t length,
                                         struct archive_string_conv *sc) {
  size_t remaining;
  const uint8_t *itp;
  int return_value = 0;

  if (sc->same) {
    if (archive_string_append(as, _p, length) == NULL)
      return (-1);
    return (invalid_mbs(_p, length, sc));
  }

  remaining = length;
  itp = (const uint8_t *)_p;
  while (*itp && remaining > 0) {
    if (*itp > 127) {

      if (sc->flag & SCONV_TO_UTF8) {
        if (archive_string_append(as, utf8_replacement_char,
                                  sizeof(utf8_replacement_char)) == NULL) {
          __archive_errx(1, "Out of memory");
        }
      } else {
        archive_strappend_char(as, '?');
      }
      return_value = -1;
    } else {
      archive_strappend_char(as, *itp);
    }
    ++itp;
  }
  return (return_value);
}

static int _utf8_to_unicode(uint32_t *pwc, const char *s, size_t n) {
  static const char utf8_count[256] = {
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
      1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
      2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
      4, 4, 4, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  int ch, i;
  int cnt;
  uint32_t wc;

  if (n == 0)
    return (0);

  ch = (unsigned char)*s;
  if (ch == 0)
    return (0);
  cnt = utf8_count[ch];

  if (n < (size_t)cnt) {
    cnt = (int)n;
    for (i = 1; i < cnt; i++) {
      if ((s[i] & 0xc0) != 0x80) {
        cnt = i;
        break;
      }
    }
    goto invalid_sequence;
  }

  switch (cnt) {
  case 1:
    *pwc = ch & 0x7f;
    return (cnt);
  case 2:
    if ((s[1] & 0xc0) != 0x80) {
      cnt = 1;
      goto invalid_sequence;
    }
    *pwc = ((ch & 0x1f) << 6) | (s[1] & 0x3f);
    return (cnt);
  case 3:
    if ((s[1] & 0xc0) != 0x80) {
      cnt = 1;
      goto invalid_sequence;
    }
    if ((s[2] & 0xc0) != 0x80) {
      cnt = 2;
      goto invalid_sequence;
    }
    wc = ((ch & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f);
    if (wc < 0x800)
      goto invalid_sequence;
    break;
  case 4:
    if ((s[1] & 0xc0) != 0x80) {
      cnt = 1;
      goto invalid_sequence;
    }
    if ((s[2] & 0xc0) != 0x80) {
      cnt = 2;
      goto invalid_sequence;
    }
    if ((s[3] & 0xc0) != 0x80) {
      cnt = 3;
      goto invalid_sequence;
    }
    wc = ((ch & 0x07) << 18) | ((s[1] & 0x3f) << 12) | ((s[2] & 0x3f) << 6) |
         (s[3] & 0x3f);
    if (wc < 0x10000)
      goto invalid_sequence;
    break;
  default:
    if (ch == 0xc0 || ch == 0xc1)
      cnt = 2;
    else if (ch >= 0xf5 && ch <= 0xf7)
      cnt = 4;
    else if (ch >= 0xf8 && ch <= 0xfb)
      cnt = 5;
    else if (ch == 0xfc || ch == 0xfd)
      cnt = 6;
    else
      cnt = 1;
    if (n < (size_t)cnt)
      cnt = (int)n;
    for (i = 1; i < cnt; i++) {
      if ((s[i] & 0xc0) != 0x80) {
        cnt = i;
        break;
      }
    }
    goto invalid_sequence;
  }

  if (wc > UNICODE_MAX)
    goto invalid_sequence;

  *pwc = wc;
  return (cnt);
invalid_sequence:
  *pwc = UNICODE_R_CHAR;
  return (cnt * -1);
}

static int utf8_to_unicode(uint32_t *pwc, const char *s, size_t n) {
  int cnt;

  cnt = _utf8_to_unicode(pwc, s, n);

  if (cnt == 3 && IS_SURROGATE_PAIR_LA(*pwc))
    return (-3);
  return (cnt);
}

static inline uint32_t combine_surrogate_pair(uint32_t uc, uint32_t uc2) {
  uc -= 0xD800;
  uc *= 0x400;
  uc += uc2 - 0xDC00;
  uc += 0x10000;
  return (uc);
}

static int cesu8_to_unicode(uint32_t *pwc, const char *s, size_t n) {
  uint32_t wc = 0;
  int cnt;

  cnt = _utf8_to_unicode(&wc, s, n);
  if (cnt == 3 && IS_HIGH_SURROGATE_LA(wc)) {
    uint32_t wc2 = 0;
    if (n - 3 < 3) {

      goto invalid_sequence;
    }
    cnt = _utf8_to_unicode(&wc2, s + 3, n - 3);
    if (cnt != 3 || !IS_LOW_SURROGATE_LA(wc2)) {

      goto invalid_sequence;
    }
    wc = combine_surrogate_pair(wc, wc2);
    cnt = 6;
  } else if (cnt == 3 && IS_LOW_SURROGATE_LA(wc)) {

    goto invalid_sequence;
  }
  *pwc = wc;
  return (cnt);
invalid_sequence:
  *pwc = UNICODE_R_CHAR;
  if (cnt > 0)
    cnt *= -1;
  return (cnt);
}

static size_t unicode_to_utf8(char *p, size_t remaining, uint32_t uc) {
  char *_p = p;

  if (uc > UNICODE_MAX)
    uc = UNICODE_R_CHAR;

  if (uc <= 0x7f) {
    if (remaining == 0)
      return (0);
    *p++ = (char)uc;
  } else if (uc <= 0x7ff) {
    if (remaining < 2)
      return (0);
    *p++ = 0xc0 | ((uc >> 6) & 0x1f);
    *p++ = 0x80 | (uc & 0x3f);
  } else if (uc <= 0xffff) {
    if (remaining < 3)
      return (0);
    *p++ = 0xe0 | ((uc >> 12) & 0x0f);
    *p++ = 0x80 | ((uc >> 6) & 0x3f);
    *p++ = 0x80 | (uc & 0x3f);
  } else {
    if (remaining < 4)
      return (0);
    *p++ = 0xf0 | ((uc >> 18) & 0x07);
    *p++ = 0x80 | ((uc >> 12) & 0x3f);
    *p++ = 0x80 | ((uc >> 6) & 0x3f);
    *p++ = 0x80 | (uc & 0x3f);
  }
  return (p - _p);
}

static int utf16be_to_unicode(uint32_t *pwc, const char *s, size_t n) {
  return (utf16_to_unicode(pwc, s, n, 1));
}

static int utf16le_to_unicode(uint32_t *pwc, const char *s, size_t n) {
  return (utf16_to_unicode(pwc, s, n, 0));
}

static int utf16_to_unicode(uint32_t *pwc, const char *s, size_t n, int be) {
  const char *utf16 = s;
  unsigned uc;

  if (n == 0)
    return (0);
  if (n == 1) {

    *pwc = UNICODE_R_CHAR;
    return (-1);
  }

  if (be)
    uc = archive_be16dec(utf16);
  else
    uc = archive_le16dec(utf16);
  utf16 += 2;

  if (IS_HIGH_SURROGATE_LA(uc)) {
    unsigned uc2;

    if (n >= 4) {
      if (be)
        uc2 = archive_be16dec(utf16);
      else
        uc2 = archive_le16dec(utf16);
    } else
      uc2 = 0;
    if (IS_LOW_SURROGATE_LA(uc2)) {
      uc = combine_surrogate_pair(uc, uc2);
      utf16 += 2;
    } else {

      *pwc = UNICODE_R_CHAR;
      return (-2);
    }
  }

  if (IS_SURROGATE_PAIR_LA(uc) || uc > UNICODE_MAX) {

    *pwc = UNICODE_R_CHAR;
    return (((int)(utf16 - s)) * -1);
  }
  *pwc = uc;
  return ((int)(utf16 - s));
}

static size_t unicode_to_utf16be(char *p, size_t remaining, uint32_t uc) {
  char *utf16 = p;

  if (uc > 0xffff) {

    if (remaining < 4)
      return (0);
    uc -= 0x10000;
    archive_be16enc(utf16, ((uc >> 10) & 0x3ff) + 0xD800);
    archive_be16enc(utf16 + 2, (uc & 0x3ff) + 0xDC00);
    return (4);
  } else {
    if (remaining < 2)
      return (0);
    archive_be16enc(utf16, (uint16_t)uc);
    return (2);
  }
}

static size_t unicode_to_utf16le(char *p, size_t remaining, uint32_t uc) {
  char *utf16 = p;

  if (uc > 0xffff) {

    if (remaining < 4)
      return (0);
    uc -= 0x10000;
    archive_le16enc(utf16, ((uc >> 10) & 0x3ff) + 0xD800);
    archive_le16enc(utf16 + 2, (uc & 0x3ff) + 0xDC00);
    return (4);
  } else {
    if (remaining < 2)
      return (0);
    archive_le16enc(utf16, (uint16_t)uc);
    return (2);
  }
}

static int strncat_from_utf8_to_utf8(struct archive_string *as,
                                     const void *_src, size_t len,
                                     struct archive_string_conv *sc) {
  int ret = 0;
  const char *src = _src;
  (void)sc;

  if (archive_string_ensure(as, as->length + len + 1) == NULL)
    return (-1);

  for (;;) {
    int n;
    uint32_t uc;
    const char *e = src;

    while ((n = utf8_to_unicode(&uc, e, len)) > 0) {
      e += n;
      len -= n;
    }

    if (e > src) {
      if (archive_string_append(as, src, e - src) == NULL)
        return (-1);
      src = e;
    }

    if (n == 0) {

      return (ret);
    } else {

      char t[4];
      size_t w;

      if (n == -3 && IS_SURROGATE_PAIR_LA(uc)) {
        n = cesu8_to_unicode(&uc, src, len);
      }

      if (n < 0) {
        ret = -1;
        n *= -1;
      }

      src += n;
      len -= n;

      w = unicode_to_utf8(t, sizeof(t), uc);
      if (archive_string_append(as, t, w) == NULL)
        return (-1);
    }
  }
}

static int archive_string_append_unicode(struct archive_string *as,
                                         const void *_p, size_t len,
                                         struct archive_string_conv *sc) {
  const char *s;
  char *p, *endp;
  uint32_t uc;
  size_t w;
  size_t ts, tm;
  int n, ret = 0;
  int (*parse)(uint32_t *, const char *, size_t);
  size_t (*unparse)(char *, size_t, uint32_t);

  if (sc->flag & SCONV_TO_UTF16BE) {
    unparse = unicode_to_utf16be;
    ts = 2;
  } else if (sc->flag & SCONV_TO_UTF16LE) {
    unparse = unicode_to_utf16le;
    ts = 2;
  } else if (sc->flag & SCONV_TO_UTF8) {
    unparse = unicode_to_utf8;
    ts = 1;
  } else {

    if (sc->flag & SCONV_FROM_UTF16BE) {
      unparse = unicode_to_utf16be;
      ts = 2;
    } else if (sc->flag & SCONV_FROM_UTF16LE) {
      unparse = unicode_to_utf16le;
      ts = 2;
    } else {
      unparse = unicode_to_utf8;
      ts = 1;
    }
  }

  if (sc->flag & SCONV_FROM_UTF16BE) {
    parse = utf16be_to_unicode;
    tm = 1;
  } else if (sc->flag & SCONV_FROM_UTF16LE) {
    parse = utf16le_to_unicode;
    tm = 1;
  } else {
    parse = cesu8_to_unicode;
    tm = ts;
  }

  if (archive_string_ensure(as, as->length + len * tm + ts) == NULL)
    return (-1);

  s = (const char *)_p;
  p = as->s + as->length;
  endp = as->s + as->buffer_length - ts;
  while ((n = parse(&uc, s, len)) != 0) {
    if (n < 0) {

      n *= -1;
      ret = -1;
    }
    s += n;
    len -= n;
    while ((w = unparse(p, endp - p, uc)) == 0) {

      as->length = p - as->s;
      if (archive_string_ensure(as, as->buffer_length + len * tm + ts) == NULL)
        return (-1);
      p = as->s + as->length;
      endp = as->s + as->buffer_length - ts;
    }
    p += w;
  }
  as->length = p - as->s;
  as->s[as->length] = '\0';
  if (ts == 2)
    as->s[as->length + 1] = '\0';
  return (ret);
}

#define HC_SBASE 0xAC00
#define HC_LBASE 0x1100
#define HC_VBASE 0x1161
#define HC_TBASE 0x11A7
#define HC_LCOUNT 19
#define HC_VCOUNT 21
#define HC_TCOUNT 28
#define HC_NCOUNT (HC_VCOUNT * HC_TCOUNT)
#define HC_SCOUNT (HC_LCOUNT * HC_NCOUNT)

static uint32_t get_nfc(uint32_t uc, uint32_t uc2) {
  int t, b;

  t = 0;
  b = sizeof(u_composition_table) / sizeof(u_composition_table[0]) - 1;
  while (b >= t) {
    int m = (t + b) / 2;
    if (u_composition_table[m].cp1 < uc)
      t = m + 1;
    else if (u_composition_table[m].cp1 > uc)
      b = m - 1;
    else if (u_composition_table[m].cp2 < uc2)
      t = m + 1;
    else if (u_composition_table[m].cp2 > uc2)
      b = m - 1;
    else
      return (u_composition_table[m].nfc);
  }
  return (0);
}

#define FDC_MAX 10

#define UPDATE_UC(new_uc)                                                      \
  do {                                                                         \
    uc = new_uc;                                                               \
    ucptr = NULL;                                                              \
  } while (0)

#define REPLACE_UC_WITH_UC2()                                                  \
  do {                                                                         \
    uc = uc2;                                                                  \
    ucptr = uc2ptr;                                                            \
    n = n2;                                                                    \
  } while (0)

#define EXPAND_BUFFER()                                                        \
  do {                                                                         \
    as->length = p - as->s;                                                    \
    if (archive_string_ensure(as, as->buffer_length + len * tm + ts) == NULL)  \
      return (-1);                                                             \
    p = as->s + as->length;                                                    \
    endp = as->s + as->buffer_length - ts;                                     \
  } while (0)

#define UNPARSE(p, endp, uc)                                                   \
  do {                                                                         \
    while ((w = unparse(p, (endp) - (p), uc)) == 0) {                          \
      EXPAND_BUFFER();                                                         \
    }                                                                          \
    p += w;                                                                    \
  } while (0)

#define WRITE_UC()                                                             \
  do {                                                                         \
    if (ucptr) {                                                               \
      if (p + n > endp)                                                        \
        EXPAND_BUFFER();                                                       \
      switch (n) {                                                             \
      case 4:                                                                  \
        *p++ = *ucptr++;                                                       \
                                                                               \
      case 3:                                                                  \
        *p++ = *ucptr++;                                                       \
                                                                               \
      case 2:                                                                  \
        *p++ = *ucptr++;                                                       \
                                                                               \
      case 1:                                                                  \
        *p++ = *ucptr;                                                         \
        break;                                                                 \
      }                                                                        \
      ucptr = NULL;                                                            \
    } else {                                                                   \
      UNPARSE(p, endp, uc);                                                    \
    }                                                                          \
  } while (0)

#define COLLECT_CPS(start)                                                     \
  do {                                                                         \
    int _i;                                                                    \
    for (_i = start; _i < FDC_MAX; _i++) {                                     \
      nx = parse(&ucx[_i], s, len);                                            \
      if (nx <= 0)                                                             \
        break;                                                                 \
      cx = CCC(ucx[_i]);                                                       \
      if (cl >= cx && cl != 228 && cx != 228)                                  \
        break;                                                                 \
      s += nx;                                                                 \
      len -= nx;                                                               \
      cl = cx;                                                                 \
      ccx[_i] = cx;                                                            \
    }                                                                          \
    if (_i >= FDC_MAX) {                                                       \
      ret = -1;                                                                \
      ucx_size = FDC_MAX;                                                      \
    } else                                                                     \
      ucx_size = _i;                                                           \
  } while (0)

static int archive_string_normalize_C(struct archive_string *as, const void *_p,
                                      size_t len,
                                      struct archive_string_conv *sc) {
  const char *s = (const char *)_p;
  char *p, *endp;
  uint32_t uc, uc2;
  size_t w;
  int always_replace, n, n2, ret = 0, spair, ts, tm;
  int (*parse)(uint32_t *, const char *, size_t);
  size_t (*unparse)(char *, size_t, uint32_t);

  always_replace = 1;
  ts = 1;
  if (sc->flag & SCONV_TO_UTF16BE) {
    unparse = unicode_to_utf16be;
    ts = 2;
    if (sc->flag & SCONV_FROM_UTF16BE)
      always_replace = 0;
  } else if (sc->flag & SCONV_TO_UTF16LE) {
    unparse = unicode_to_utf16le;
    ts = 2;
    if (sc->flag & SCONV_FROM_UTF16LE)
      always_replace = 0;
  } else if (sc->flag & SCONV_TO_UTF8) {
    unparse = unicode_to_utf8;
    if (sc->flag & SCONV_FROM_UTF8)
      always_replace = 0;
  } else {

    always_replace = 0;
    if (sc->flag & SCONV_FROM_UTF16BE) {
      unparse = unicode_to_utf16be;
      ts = 2;
    } else if (sc->flag & SCONV_FROM_UTF16LE) {
      unparse = unicode_to_utf16le;
      ts = 2;
    } else {
      unparse = unicode_to_utf8;
    }
  }

  if (sc->flag & SCONV_FROM_UTF16BE) {
    parse = utf16be_to_unicode;
    tm = 1;
    spair = 4;
  } else if (sc->flag & SCONV_FROM_UTF16LE) {
    parse = utf16le_to_unicode;
    tm = 1;
    spair = 4;
  } else {
    parse = cesu8_to_unicode;
    tm = ts;
    spair = 6;
  }

  if (archive_string_ensure(as, as->length + len * tm + ts) == NULL)
    return (-1);

  p = as->s + as->length;
  endp = as->s + as->buffer_length - ts;
  while ((n = parse(&uc, s, len)) != 0) {
    const char *ucptr, *uc2ptr;

    if (n < 0) {

      UNPARSE(p, endp, uc);
      s += n * -1;
      len -= n * -1;
      ret = -1;
      continue;
    } else if (n == spair || always_replace)

      ucptr = NULL;
    else
      ucptr = s;
    s += n;
    len -= n;

    while ((n2 = parse(&uc2, s, len)) > 0) {
      uint32_t ucx[FDC_MAX];
      int ccx[FDC_MAX];
      int cl, cx, i, nx, ucx_size;
      int LIndex, SIndex;
      uint32_t nfc;

      if (n2 == spair || always_replace)

        uc2ptr = NULL;
      else
        uc2ptr = s;
      s += n2;
      len -= n2;

      if (!IS_DECOMPOSABLE_BLOCK(uc2)) {
        WRITE_UC();
        REPLACE_UC_WITH_UC2();
        continue;
      }

      if (0 <= (LIndex = uc - HC_LBASE) && LIndex < HC_LCOUNT) {

        int VIndex = uc2 - HC_VBASE;
        if (0 <= VIndex && VIndex < HC_VCOUNT) {

          UPDATE_UC(HC_SBASE + (LIndex * HC_VCOUNT + VIndex) * HC_TCOUNT);
        } else {
          WRITE_UC();
          REPLACE_UC_WITH_UC2();
        }
        continue;
      } else if (0 <= (SIndex = uc - HC_SBASE) && SIndex < HC_SCOUNT &&
                 (SIndex % HC_TCOUNT) == 0) {

        int TIndex = uc2 - HC_TBASE;
        if (0 < TIndex && TIndex < HC_TCOUNT) {

          UPDATE_UC(uc + TIndex);
        } else {
          WRITE_UC();
          REPLACE_UC_WITH_UC2();
        }
        continue;
      } else if ((nfc = get_nfc(uc, uc2)) != 0) {

        UPDATE_UC(nfc);
        continue;
      } else if ((cl = CCC(uc2)) == 0) {

        WRITE_UC();
        REPLACE_UC_WITH_UC2();
        continue;
      }

      cx = 0;
      ucx[0] = uc2;
      ccx[0] = cl;
      COLLECT_CPS(1);

      i = 1;
      while (i < ucx_size) {
        int j;

        if ((nfc = get_nfc(uc, ucx[i])) == 0) {
          i++;
          continue;
        }

        UPDATE_UC(nfc);

        for (j = i; j + 1 < ucx_size; j++) {
          ucx[j] = ucx[j + 1];
          ccx[j] = ccx[j + 1];
        }
        ucx_size--;

        if (ucx_size > 0 && i == ucx_size && nx > 0 && cx == cl) {
          cl = ccx[ucx_size - 1];
          COLLECT_CPS(ucx_size);
        }

        i = 0;
      }

      WRITE_UC();
      for (i = 0; i < ucx_size; i++)
        UNPARSE(p, endp, ucx[i]);

      if (nx > 0 && cx == cl && len > 0) {
        while ((nx = parse(&ucx[0], s, len)) > 0) {
          cx = CCC(ucx[0]);
          if (cl > cx)
            break;
          s += nx;
          len -= nx;
          cl = cx;
          UNPARSE(p, endp, ucx[0]);
        }
      }
      break;
    }
    if (n2 < 0) {
      WRITE_UC();

      UNPARSE(p, endp, uc2);
      s += n2 * -1;
      len -= n2 * -1;
      ret = -1;
      continue;
    } else if (n2 == 0) {
      WRITE_UC();
      break;
    }
  }
  as->length = p - as->s;
  as->s[as->length] = '\0';
  if (ts == 2)
    as->s[as->length + 1] = '\0';
  return (ret);
}

static int get_nfd(uint32_t *cp1, uint32_t *cp2, uint32_t uc) {
  int t, b;

  if ((uc >= 0x2000 && uc <= 0x2FFF) || (uc >= 0xF900 && uc <= 0xFAFF) ||
      (uc >= 0x2F800 && uc <= 0x2FAFF))
    return (0);

  if (uc == 0x1109A || uc == 0x1109C || uc == 0x110AB)
    return (0);

  t = 0;
  b = sizeof(u_decomposition_table) / sizeof(u_decomposition_table[0]) - 1;
  while (b >= t) {
    int m = (t + b) / 2;
    if (u_decomposition_table[m].nfc < uc)
      t = m + 1;
    else if (u_decomposition_table[m].nfc > uc)
      b = m - 1;
    else {
      *cp1 = u_decomposition_table[m].cp1;
      *cp2 = u_decomposition_table[m].cp2;
      return (1);
    }
  }
  return (0);
}

#define REPLACE_UC_WITH(cp)                                                    \
  do {                                                                         \
    uc = cp;                                                                   \
    ucptr = NULL;                                                              \
  } while (0)

static int archive_string_normalize_D(struct archive_string *as, const void *_p,
                                      size_t len,
                                      struct archive_string_conv *sc) {
  const char *s = (const char *)_p;
  char *p, *endp;
  uint32_t uc, uc2;
  size_t w;
  int always_replace, n, n2, ret = 0, spair, ts, tm;
  int (*parse)(uint32_t *, const char *, size_t);
  size_t (*unparse)(char *, size_t, uint32_t);

  always_replace = 1;
  ts = 1;
  if (sc->flag & SCONV_TO_UTF16BE) {
    unparse = unicode_to_utf16be;
    ts = 2;
    if (sc->flag & SCONV_FROM_UTF16BE)
      always_replace = 0;
  } else if (sc->flag & SCONV_TO_UTF16LE) {
    unparse = unicode_to_utf16le;
    ts = 2;
    if (sc->flag & SCONV_FROM_UTF16LE)
      always_replace = 0;
  } else if (sc->flag & SCONV_TO_UTF8) {
    unparse = unicode_to_utf8;
    if (sc->flag & SCONV_FROM_UTF8)
      always_replace = 0;
  } else {

    always_replace = 0;
    if (sc->flag & SCONV_FROM_UTF16BE) {
      unparse = unicode_to_utf16be;
      ts = 2;
    } else if (sc->flag & SCONV_FROM_UTF16LE) {
      unparse = unicode_to_utf16le;
      ts = 2;
    } else {
      unparse = unicode_to_utf8;
    }
  }

  if (sc->flag & SCONV_FROM_UTF16BE) {
    parse = utf16be_to_unicode;
    tm = 1;
    spair = 4;
  } else if (sc->flag & SCONV_FROM_UTF16LE) {
    parse = utf16le_to_unicode;
    tm = 1;
    spair = 4;
  } else {
    parse = cesu8_to_unicode;
    tm = ts;
    spair = 6;
  }

  if (archive_string_ensure(as, as->length + len * tm + ts) == NULL)
    return (-1);

  p = as->s + as->length;
  endp = as->s + as->buffer_length - ts;
  while ((n = parse(&uc, s, len)) != 0) {
    const char *ucptr;
    uint32_t cp1, cp2;
    int SIndex;
    struct {
      uint32_t uc;
      int ccc;
    } fdc[FDC_MAX];
    int fdi, fdj;
    int ccc;

  check_first_code:
    if (n < 0) {

      UNPARSE(p, endp, uc);
      s += n * -1;
      len -= n * -1;
      ret = -1;
      continue;
    } else if (n == spair || always_replace)

      ucptr = NULL;
    else
      ucptr = s;
    s += n;
    len -= n;

    if ((SIndex = uc - HC_SBASE) >= 0 && SIndex < HC_SCOUNT) {
      int L = HC_LBASE + SIndex / HC_NCOUNT;
      int V = HC_VBASE + (SIndex % HC_NCOUNT) / HC_TCOUNT;
      int T = HC_TBASE + SIndex % HC_TCOUNT;

      REPLACE_UC_WITH(L);
      WRITE_UC();
      REPLACE_UC_WITH(V);
      WRITE_UC();
      if (T != HC_TBASE) {
        REPLACE_UC_WITH(T);
        WRITE_UC();
      }
      continue;
    }
    if (IS_DECOMPOSABLE_BLOCK(uc) && CCC(uc) != 0) {
      WRITE_UC();
      continue;
    }

    fdi = 0;
    while (get_nfd(&cp1, &cp2, uc) && fdi < FDC_MAX) {
      int k;

      for (k = fdi; k > 0; k--)
        fdc[k] = fdc[k - 1];
      fdc[0].ccc = CCC(cp2);
      fdc[0].uc = cp2;
      fdi++;
      REPLACE_UC_WITH(cp1);
    }

    while ((n2 = parse(&uc2, s, len)) > 0 && (ccc = CCC(uc2)) != 0 &&
           fdi < FDC_MAX) {
      int j, k;

      s += n2;
      len -= n2;
      for (j = 0; j < fdi; j++) {
        if (fdc[j].ccc > ccc)
          break;
      }
      if (j < fdi) {
        for (k = fdi; k > j; k--)
          fdc[k] = fdc[k - 1];
        fdc[j].ccc = ccc;
        fdc[j].uc = uc2;
      } else {
        fdc[fdi].ccc = ccc;
        fdc[fdi].uc = uc2;
      }
      fdi++;
    }

    WRITE_UC();
    for (fdj = 0; fdj < fdi; fdj++) {
      REPLACE_UC_WITH(fdc[fdj].uc);
      WRITE_UC();
    }

    if (n2 == 0)
      break;
    REPLACE_UC_WITH(uc2);
    n = n2;
    goto check_first_code;
  }
  as->length = p - as->s;
  as->s[as->length] = '\0';
  if (ts == 2)
    as->s[as->length + 1] = '\0';
  return (ret);
}

static int strncat_from_utf8_libarchive2(struct archive_string *as,
                                         const void *_p, size_t len,
                                         struct archive_string_conv *sc) {
  const char *s;
  int n;
  char *p;
  char *end;
  uint32_t unicode;
#if HAVE_WCRTOMB
  mbstate_t shift_state;

  memset(&shift_state, 0, sizeof(shift_state));
#else

  if (wctomb(NULL, L'\0') < 0) {
  }
#endif
  (void)sc;

  if (archive_string_ensure(as, as->length + len + 1) == NULL)
    return (-1);

  s = (const char *)_p;
  p = as->s + as->length;
  end = as->s + as->buffer_length - MB_CUR_MAX - 1;
  while ((n = _utf8_to_unicode(&unicode, s, len)) != 0) {
    wchar_t wc;

    if (p >= end) {
      as->length = p - as->s;

      if (archive_string_ensure(
              as, as->length + max(len * 2, (size_t)MB_CUR_MAX) + 1) == NULL)
        return (-1);
      p = as->s + as->length;
      end = as->s + as->buffer_length - MB_CUR_MAX - 1;
    }

    if (n < 0) {
      n *= -1;
      wc = L'?';
    } else
      wc = (wchar_t)unicode;

    s += n;
    len -= n;

#if HAVE_WCRTOMB
    n = (int)wcrtomb(p, wc, &shift_state);
#else
    n = (int)wctomb(p, wc);
#endif
    if (n == -1)
      return (-1);
    p += n;
  }
  as->length = p - as->s;
  as->s[as->length] = '\0';
  return (0);
}

#if defined(_WIN32) && !defined(__CYGWIN__)

static int win_strncat_from_utf16(struct archive_string *as, const void *_p,
                                  size_t bytes, struct archive_string_conv *sc,
                                  int be) {
  struct archive_string tmp;
  const char *u16;
  BOOL defchar;
  char *mbs;
  size_t mbs_size, b, ll;
  int ret = 0;

  bytes &= ~1;
  if (archive_string_ensure(as, as->length + bytes + 1) == NULL)
    return (-1);

  mbs = as->s + as->length;
  mbs_size = as->buffer_length - as->length - 1;

  if (sc->to_cp == CP_C_LOCALE) {

    u16 = _p;
    ll = 0;
    for (b = 0; b < bytes; b += 2) {
      uint16_t val;
      if (be)
        val = archive_be16dec(u16 + b);
      else
        val = archive_le16dec(u16 + b);
      if (val > 255) {
        *mbs++ = '?';
        ret = -1;
      } else
        *mbs++ = (char)(val & 0xff);
      ll++;
    }
    as->length += ll;
    as->s[as->length] = '\0';
    return (ret);
  }

  archive_string_init(&tmp);
  if (be) {
    if (IS_BIG_ENDIAN) {
      u16 = _p;
    } else {
      if (archive_string_ensure(&tmp, bytes + 2) == NULL)
        return (-1);
      memcpy(tmp.s, _p, bytes);
      for (b = 0; b < bytes; b += 2) {
        uint16_t val = archive_be16dec(tmp.s + b);
        archive_le16enc(tmp.s + b, val);
      }
      u16 = tmp.s;
    }
  } else {
    if (!IS_BIG_ENDIAN) {
      u16 = _p;
    } else {
      if (archive_string_ensure(&tmp, bytes + 2) == NULL)
        return (-1);
      memcpy(tmp.s, _p, bytes);
      for (b = 0; b < bytes; b += 2) {
        uint16_t val = archive_le16dec(tmp.s + b);
        archive_be16enc(tmp.s + b, val);
      }
      u16 = tmp.s;
    }
  }

  do {
    int r;
    defchar = 0;

    if (bytes > (size_t)INT_MAX || mbs_size > (size_t)INT_MAX)
      return (-1);
    r = WideCharToMultiByte(sc->to_cp, 0, (LPCWSTR)u16, (int)bytes >> 1, mbs,
                            (int)mbs_size, NULL, &defchar);

    if (r != 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      ll = (size_t)r;
      break;
    }

    r = WideCharToMultiByte(sc->to_cp, 0, (LPCWSTR)u16, (int)bytes, NULL, 0,
                            NULL, NULL);
    ll = (size_t)r;
    if (archive_string_ensure(as, ll + 1) == NULL)
      return (-1);
    mbs = as->s + as->length;
    mbs_size = as->buffer_length - as->length - 1;
  } while (1);
  archive_string_free(&tmp);
  as->length += ll;
  as->s[as->length] = '\0';
  if (ll == 0 || defchar)
    ret = -1;
  return (ret);
}

static int win_strncat_from_utf16be(struct archive_string *as, const void *_p,
                                    size_t bytes,
                                    struct archive_string_conv *sc) {
  return (win_strncat_from_utf16(as, _p, bytes, sc, 1));
}

static int win_strncat_from_utf16le(struct archive_string *as, const void *_p,
                                    size_t bytes,
                                    struct archive_string_conv *sc) {
  return (win_strncat_from_utf16(as, _p, bytes, sc, 0));
}

static int win_strncat_to_utf16(struct archive_string *as16, const void *_p,
                                size_t length, struct archive_string_conv *sc,
                                int bigendian) {
  const char *s = (const char *)_p;
  char *u16;
  size_t count, avail;

  if (archive_string_ensure(as16, as16->length + (length + 1) * 2) == NULL)
    return (-1);

  u16 = as16->s + as16->length;
  avail = as16->buffer_length - 2;
  if (sc->from_cp == CP_C_LOCALE) {

    count = 0;
    while (count < length && *s) {
      if (bigendian)
        archive_be16enc(u16, *s);
      else
        archive_le16enc(u16, *s);
      u16 += 2;
      s++;
      count++;
    }
    as16->length += count << 1;
    as16->s[as16->length] = 0;
    as16->s[as16->length + 1] = 0;
    return (0);
  }
  do {
    int r;
    if (length > (size_t)INT_MAX || (avail >> 1) > (size_t)INT_MAX)
      return (-1);
    r = MultiByteToWideChar(sc->from_cp, MB_PRECOMPOSED, s, (int)length,
                            (LPWSTR)u16, (int)avail >> 1);

    if (r != 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      count = (size_t)r;
      break;
    }

    r = MultiByteToWideChar(sc->from_cp, MB_PRECOMPOSED, s, (int)length, NULL,
                            0);
    count = (size_t)r;
    if (archive_string_ensure(as16, (count + 1) * 2) == NULL)
      return (-1);
    u16 = as16->s + as16->length;
    avail = as16->buffer_length - 2;
  } while (1);
  as16->length += count * 2;
  as16->s[as16->length] = 0;
  as16->s[as16->length + 1] = 0;
  if (count == 0)
    return (-1);

  if (IS_BIG_ENDIAN) {
    if (!bigendian) {
      while (count > 0) {
        uint16_t v = archive_be16dec(u16);
        archive_le16enc(u16, v);
        u16 += 2;
        count--;
      }
    }
  } else {
    if (bigendian) {
      while (count > 0) {
        uint16_t v = archive_le16dec(u16);
        archive_be16enc(u16, v);
        u16 += 2;
        count--;
      }
    }
  }
  return (0);
}

static int win_strncat_to_utf16be(struct archive_string *as16, const void *_p,
                                  size_t length,
                                  struct archive_string_conv *sc) {
  return (win_strncat_to_utf16(as16, _p, length, sc, 1));
}

static int win_strncat_to_utf16le(struct archive_string *as16, const void *_p,
                                  size_t length,
                                  struct archive_string_conv *sc) {
  return (win_strncat_to_utf16(as16, _p, length, sc, 0));
}

#endif

static int best_effort_strncat_from_utf16(struct archive_string *as,
                                          const void *_p, size_t bytes,
                                          struct archive_string_conv *sc,
                                          int be) {
  const char *utf16 = (const char *)_p;
  char *mbs;
  uint32_t uc;
  int n, ret;

  (void)sc;

  ret = 0;
  if (archive_string_ensure(as, as->length + bytes + 1) == NULL)
    return (-1);
  mbs = as->s + as->length;

  while ((n = utf16_to_unicode(&uc, utf16, bytes, be)) != 0) {
    if (n < 0) {
      n *= -1;
      ret = -1;
    }
    bytes -= n;
    utf16 += n;

    if (uc > 127) {

      *mbs++ = '?';
      ret = -1;
    } else
      *mbs++ = (char)uc;
  }
  as->length = mbs - as->s;
  as->s[as->length] = '\0';
  return (ret);
}

static int best_effort_strncat_from_utf16be(struct archive_string *as,
                                            const void *_p, size_t bytes,
                                            struct archive_string_conv *sc) {
  return (best_effort_strncat_from_utf16(as, _p, bytes, sc, 1));
}

static int best_effort_strncat_from_utf16le(struct archive_string *as,
                                            const void *_p, size_t bytes,
                                            struct archive_string_conv *sc) {
  return (best_effort_strncat_from_utf16(as, _p, bytes, sc, 0));
}

static int best_effort_strncat_to_utf16(struct archive_string *as16,
                                        const void *_p, size_t length,
                                        struct archive_string_conv *sc,
                                        int bigendian) {
  const char *s = (const char *)_p;
  char *utf16;
  size_t remaining;
  int ret;

  (void)sc;

  ret = 0;
  remaining = length;

  if (archive_string_ensure(as16, as16->length + (length + 1) * 2) == NULL)
    return (-1);

  utf16 = as16->s + as16->length;
  while (remaining--) {
    unsigned c = *s++;
    if (c > 127) {

      c = UNICODE_R_CHAR;
      ret = -1;
    }
    if (bigendian)
      archive_be16enc(utf16, (uint16_t)c);
    else
      archive_le16enc(utf16, (uint16_t)c);
    utf16 += 2;
  }
  as16->length = utf16 - as16->s;
  as16->s[as16->length] = 0;
  as16->s[as16->length + 1] = 0;
  return (ret);
}

static int best_effort_strncat_to_utf16be(struct archive_string *as16,
                                          const void *_p, size_t length,
                                          struct archive_string_conv *sc) {
  return (best_effort_strncat_to_utf16(as16, _p, length, sc, 1));
}

static int best_effort_strncat_to_utf16le(struct archive_string *as16,
                                          const void *_p, size_t length,
                                          struct archive_string_conv *sc) {
  return (best_effort_strncat_to_utf16(as16, _p, length, sc, 0));
}

void archive_mstring_clean(struct archive_mstring *aes) {
  archive_wstring_free(&(aes->aes_wcs));
  archive_string_free(&(aes->aes_mbs));
  archive_string_free(&(aes->aes_utf8));
  archive_string_free(&(aes->aes_mbs_in_locale));
  aes->aes_set = 0;
}

void archive_mstring_copy(struct archive_mstring *dest,
                          struct archive_mstring *src) {
  dest->aes_set = src->aes_set;
  archive_string_copy(&(dest->aes_mbs), &(src->aes_mbs));
  archive_string_copy(&(dest->aes_utf8), &(src->aes_utf8));
  archive_wstring_copy(&(dest->aes_wcs), &(src->aes_wcs));
}

int archive_mstring_get_utf8(struct archive *a, struct archive_mstring *aes,
                             const char **p) {
  struct archive_string_conv *sc;
  int r;

  if (aes->aes_set & AES_SET_UTF8) {
    *p = aes->aes_utf8.s;
    return (0);
  }

  *p = NULL;
#if defined(_WIN32) && !defined(__CYGWIN__)

  if ((aes->aes_set & AES_SET_WCS) != 0) {
    sc = archive_string_conversion_to_charset(a, "UTF-8", 1);
    if (sc == NULL)
      return (-1);
    archive_string_empty(&(aes->aes_utf8));
    r = archive_string_append_from_wcs_in_codepage(
        &(aes->aes_utf8), aes->aes_wcs.s, aes->aes_wcs.length, sc);
    if (a == NULL)
      free_sconv_object(sc);
    if (r == 0) {
      aes->aes_set |= AES_SET_UTF8;
      *p = aes->aes_utf8.s;
      return (0);
    } else
      return (-1);
  }
#endif

  if ((aes->aes_set & AES_SET_MBS) == 0) {
    const char *pm;
    archive_mstring_get_mbs(a, aes, &pm);
  }
  if (aes->aes_set & AES_SET_MBS) {
    sc = archive_string_conversion_to_charset(a, "UTF-8", 1);
    if (sc == NULL)
      return (-1);
    r = archive_strncpy_l(&(aes->aes_utf8), aes->aes_mbs.s, aes->aes_mbs.length,
                          sc);
    if (a == NULL)
      free_sconv_object(sc);
    if (r == 0) {
      aes->aes_set |= AES_SET_UTF8;
      *p = aes->aes_utf8.s;
      return (0);
    } else
      return (-1);
  }
  return (0);
}

int archive_mstring_get_mbs(struct archive *a, struct archive_mstring *aes,
                            const char **p) {
  struct archive_string_conv *sc;
  int r, ret = 0;

  if (aes->aes_set & AES_SET_MBS) {
    *p = aes->aes_mbs.s;
    return (ret);
  }

  *p = NULL;

  if (aes->aes_set & AES_SET_WCS) {
    archive_string_empty(&(aes->aes_mbs));
    r = archive_string_append_from_wcs(&(aes->aes_mbs), aes->aes_wcs.s,
                                       aes->aes_wcs.length);
    *p = aes->aes_mbs.s;
    if (r == 0) {
      aes->aes_set |= AES_SET_MBS;
      return (ret);
    } else
      ret = -1;
  }

  if (aes->aes_set & AES_SET_UTF8) {
    archive_string_empty(&(aes->aes_mbs));
    sc = archive_string_conversion_from_charset(a, "UTF-8", 1);
    if (sc == NULL)
      return (-1);
    r = archive_strncpy_l(&(aes->aes_mbs), aes->aes_utf8.s,
                          aes->aes_utf8.length, sc);
    if (a == NULL)
      free_sconv_object(sc);
    *p = aes->aes_mbs.s;
    if (r == 0) {
      aes->aes_set |= AES_SET_MBS;
      ret = 0;
    } else
      ret = -1;
  }
  return (ret);
}

int archive_mstring_get_wcs(struct archive *a, struct archive_mstring *aes,
                            const wchar_t **wp) {
  int r, ret = 0;

  (void)a;

  if (aes->aes_set & AES_SET_WCS) {
    *wp = aes->aes_wcs.s;
    return (ret);
  }

  *wp = NULL;
#if defined(_WIN32) && !defined(__CYGWIN__)

  if (aes->aes_set & AES_SET_UTF8) {
    struct archive_string_conv *sc;

    sc = archive_string_conversion_from_charset(a, "UTF-8", 1);
    if (sc != NULL) {
      archive_wstring_empty((&aes->aes_wcs));
      r = archive_wstring_append_from_mbs_in_codepage(
          &(aes->aes_wcs), aes->aes_utf8.s, aes->aes_utf8.length, sc);
      if (a == NULL)
        free_sconv_object(sc);
      if (r == 0) {
        aes->aes_set |= AES_SET_WCS;
        *wp = aes->aes_wcs.s;
        return (0);
      }
    }
  }
#endif

  if ((aes->aes_set & AES_SET_MBS) == 0) {
    const char *p;
    archive_mstring_get_mbs(a, aes, &p);
  }

  if (aes->aes_set & AES_SET_MBS) {
    archive_wstring_empty(&(aes->aes_wcs));
    r = archive_wstring_append_from_mbs(&(aes->aes_wcs), aes->aes_mbs.s,
                                        aes->aes_mbs.length);
    if (r == 0) {
      aes->aes_set |= AES_SET_WCS;
      *wp = aes->aes_wcs.s;
    } else
      ret = -1;
  }
  return (ret);
}

int archive_mstring_get_mbs_l(struct archive *a, struct archive_mstring *aes,
                              const char **p, size_t *length,
                              struct archive_string_conv *sc) {
  int ret = 0;
#if defined(_WIN32) && !defined(__CYGWIN__)
  int r;

  if (sc != NULL && (aes->aes_set & AES_SET_WCS) != 0) {
    archive_string_empty(&(aes->aes_mbs_in_locale));
    r = archive_string_append_from_wcs_in_codepage(
        &(aes->aes_mbs_in_locale), aes->aes_wcs.s, aes->aes_wcs.length, sc);
    if (r == 0) {
      *p = aes->aes_mbs_in_locale.s;
      if (length != NULL)
        *length = aes->aes_mbs_in_locale.length;
      return (0);
    } else if (errno == ENOMEM)
      return (-1);
    else
      ret = -1;
  }
#endif

  if ((aes->aes_set & AES_SET_MBS) == 0) {
    const char *pm;
    if (archive_mstring_get_mbs(a, aes, &pm) != 0) {

      ret = -1;
    }
  }

  if (aes->aes_set & AES_SET_MBS) {
    if (sc == NULL) {

      *p = aes->aes_mbs.s;
      if (length != NULL)
        *length = aes->aes_mbs.length;
      return (0);
    }
    ret = archive_strncpy_l(&(aes->aes_mbs_in_locale), aes->aes_mbs.s,
                            aes->aes_mbs.length, sc);
    *p = aes->aes_mbs_in_locale.s;
    if (length != NULL)
      *length = aes->aes_mbs_in_locale.length;
  } else {

    *p = NULL;
    if (length != NULL)
      *length = 0;
  }
  return (ret);
}

int archive_mstring_copy_mbs(struct archive_mstring *aes, const char *mbs) {
  if (mbs == NULL) {
    aes->aes_set = 0;
    return (0);
  }
  return (archive_mstring_copy_mbs_len(aes, mbs, strlen(mbs)));
}

int archive_mstring_copy_mbs_len(struct archive_mstring *aes, const char *mbs,
                                 size_t len) {
  if (mbs == NULL) {
    aes->aes_set = 0;
    return (0);
  }
  aes->aes_set = AES_SET_MBS;
  archive_strncpy(&(aes->aes_mbs), mbs, len);
  archive_string_empty(&(aes->aes_utf8));
  archive_wstring_empty(&(aes->aes_wcs));
  return (0);
}

int archive_mstring_copy_wcs(struct archive_mstring *aes, const wchar_t *wcs) {
  return archive_mstring_copy_wcs_len(aes, wcs, wcs == NULL ? 0 : wcslen(wcs));
}

int archive_mstring_copy_utf8(struct archive_mstring *aes, const char *utf8) {
  if (utf8 == NULL) {
    aes->aes_set = 0;
    return (0);
  }
  aes->aes_set = AES_SET_UTF8;
  archive_string_empty(&(aes->aes_mbs));
  archive_string_empty(&(aes->aes_wcs));
  archive_strncpy(&(aes->aes_utf8), utf8, strlen(utf8));
  return (int)strlen(utf8);
}

int archive_mstring_copy_wcs_len(struct archive_mstring *aes,
                                 const wchar_t *wcs, size_t len) {
  if (wcs == NULL) {
    aes->aes_set = 0;
    return (0);
  }
  aes->aes_set = AES_SET_WCS;
  archive_string_empty(&(aes->aes_mbs));
  archive_string_empty(&(aes->aes_utf8));
  archive_wstrncpy(&(aes->aes_wcs), wcs, len);
  return (0);
}

int archive_mstring_copy_mbs_len_l(struct archive_mstring *aes, const char *mbs,
                                   size_t len, struct archive_string_conv *sc) {
  int r;

  if (mbs == NULL) {
    aes->aes_set = 0;
    return (0);
  }
  archive_string_empty(&(aes->aes_mbs));
  archive_wstring_empty(&(aes->aes_wcs));
  archive_string_empty(&(aes->aes_utf8));
#if defined(_WIN32) && !defined(__CYGWIN__)

  if (sc == NULL) {
    if (archive_string_append(&(aes->aes_mbs), mbs, mbsnbytes(mbs, len)) ==
        NULL) {
      aes->aes_set = 0;
      r = -1;
    } else {
      aes->aes_set = AES_SET_MBS;
      r = 0;
    }
#if defined(HAVE_ICONV)
  } else if (sc != NULL && sc->cd_w != (iconv_t)-1) {

    iconv_t cd = sc->cd;
    unsigned from_cp;
    int flag;

    sc->cd = sc->cd_w;
    r = archive_strncpy_l(&(aes->aes_utf8), mbs, len, sc);
    sc->cd = cd;
    if (r != 0) {
      aes->aes_set = 0;
      return (r);
    }
    aes->aes_set = AES_SET_UTF8;

    flag = sc->flag;
    sc->flag &= ~(SCONV_NORMALIZATION_C | SCONV_TO_UTF16 | SCONV_FROM_UTF16);
    from_cp = sc->from_cp;
    sc->from_cp = CP_UTF8;
    r = archive_wstring_append_from_mbs_in_codepage(
        &(aes->aes_wcs), aes->aes_utf8.s, aes->aes_utf8.length, sc);
    sc->flag = flag;
    sc->from_cp = from_cp;
    if (r == 0)
      aes->aes_set |= AES_SET_WCS;
#endif
  } else {
    r = archive_wstring_append_from_mbs_in_codepage(&(aes->aes_wcs), mbs, len,
                                                    sc);
    if (r == 0)
      aes->aes_set = AES_SET_WCS;
    else
      aes->aes_set = 0;
  }
#else
  r = archive_strncpy_l(&(aes->aes_mbs), mbs, len, sc);
  if (r == 0)
    aes->aes_set = AES_SET_MBS;
  else
    aes->aes_set = 0;
#endif
  return (r);
}

int archive_mstring_update_utf8(struct archive *a, struct archive_mstring *aes,
                                const char *utf8) {
  struct archive_string_conv *sc;
  int r;

  if (utf8 == NULL) {
    aes->aes_set = 0;
    return (0);
  }

  archive_strcpy(&(aes->aes_utf8), utf8);

  archive_string_empty(&(aes->aes_mbs));
  archive_wstring_empty(&(aes->aes_wcs));

  aes->aes_set = AES_SET_UTF8;

  sc = archive_string_conversion_from_charset(a, "UTF-8", 1);
  if (sc == NULL)
    return (-1);

#if defined(_WIN32) && !defined(__CYGWIN__)

  r = archive_wstring_append_from_mbs_in_codepage(
      &(aes->aes_wcs), aes->aes_utf8.s, aes->aes_utf8.length, sc);

  if (a == NULL)
    free_sconv_object(sc);
  if (r != 0)
    return (-1);
  aes->aes_set = AES_SET_UTF8 | AES_SET_WCS;

  if (archive_string_append_from_wcs(&(aes->aes_mbs), aes->aes_wcs.s,
                                     aes->aes_wcs.length))
    return (-1);
#else

  r = archive_strcpy_l(&(aes->aes_mbs), utf8, sc);

  if (a == NULL)
    free_sconv_object(sc);
  if (r != 0)
    return (-1);
  aes->aes_set = AES_SET_UTF8 | AES_SET_MBS;

  if (archive_wstring_append_from_mbs(&(aes->aes_wcs), aes->aes_mbs.s,
                                      aes->aes_mbs.length))
    return (-1);
#endif

  aes->aes_set = AES_SET_UTF8 | AES_SET_WCS | AES_SET_MBS;

  return (0);
}
