/*-
 * Copyright (c) 2003-2007 Tim Kientzle
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

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_WCHAR_H
#include <wchar.h>
#endif

#include "archive_pathmatch.h"

#ifndef ARCHIVE_PATHMATCH_H_INCLUDED
#error "archive_pathmatch.h must be included"
#endif

static int pm_list(const char *start, const char *end, const char c,
                   int flags) {
  const char *p = start;
  char rangeStart = '\0', nextRangeStart;
  int match = 1, nomatch = 0;

  (void)flags;

  if ((*p == '!' || *p == '^') && p < end) {
    match = 0;
    nomatch = 1;
    ++p;
  }

  while (p < end) {
    nextRangeStart = '\0';
    switch (*p) {
    case '-':

      if ((rangeStart == '\0') || (p == end - 1)) {
        if (*p == c)
          return (match);
      } else {
        char rangeEnd = *++p;
        if (rangeEnd == '\\')
          rangeEnd = *++p;
        if ((rangeStart <= c) && (c <= rangeEnd))
          return (match);
      }
      break;
    case '\\':
      ++p;

    default:
      if (*p == c)
        return (match);
      nextRangeStart = *p;
    }
    rangeStart = nextRangeStart;
    ++p;
  }
  return (nomatch);
}

static int pm_list_w(const wchar_t *start, const wchar_t *end, const wchar_t c,
                     int flags) {
  const wchar_t *p = start;
  wchar_t rangeStart = L'\0', nextRangeStart;
  int match = 1, nomatch = 0;

  (void)flags;

  if ((*p == L'!' || *p == L'^') && p < end) {
    match = 0;
    nomatch = 1;
    ++p;
  }

  while (p < end) {
    nextRangeStart = L'\0';
    switch (*p) {
    case L'-':

      if ((rangeStart == L'\0') || (p == end - 1)) {
        if (*p == c)
          return (match);
      } else {
        wchar_t rangeEnd = *++p;
        if (rangeEnd == L'\\')
          rangeEnd = *++p;
        if ((rangeStart <= c) && (c <= rangeEnd))
          return (match);
      }
      break;
    case L'\\':
      ++p;

    default:
      if (*p == c)
        return (match);
      nextRangeStart = *p;
    }
    rangeStart = nextRangeStart;
    ++p;
  }
  return (nomatch);
}

static const char *pm_slashskip(const char *s) {
  while ((*s == '/') || (s[0] == '.' && s[1] == '/') ||
         (s[0] == '.' && s[1] == '\0'))
    ++s;
  return (s);
}

static const wchar_t *pm_slashskip_w(const wchar_t *s) {
  while ((*s == L'/') || (s[0] == L'.' && s[1] == L'/') ||
         (s[0] == L'.' && s[1] == L'\0'))
    ++s;
  return (s);
}

static int pm(const char *p, const char *s, int flags) {
  const char *end;

  if (s[0] == '.' && s[1] == '/')
    s = pm_slashskip(s + 1);
  if (p[0] == '.' && p[1] == '/')
    p = pm_slashskip(p + 1);

  for (;;) {
    switch (*p) {
    case '\0':
      if (s[0] == '/') {
        if (flags & PATHMATCH_NO_ANCHOR_END)
          return (1);

        s = pm_slashskip(s);
      }
      return (*s == '\0');
    case '?':

      if (*s == '\0')
        return (0);
      break;
    case '*':

      while (*p == '*')
        ++p;

      if (*p == '\0')
        return (1);
      while (*s) {
        if (archive_pathmatch(p, s, flags))
          return (1);
        ++s;
      }
      return (0);
    case '[':

      end = p + 1;
      while (*end != '\0' && *end != ']') {
        if (*end == '\\' && end[1] != '\0')
          ++end;
        ++end;
      }
      if (*end == ']') {

        if (!pm_list(p + 1, end, *s, flags))
          return (0);
        p = end;
        break;
      } else

          if (*p != *s)
        return (0);
      break;
    case '\\':

      if (p[1] == '\0') {
        if (*s != '\\')
          return (0);
      } else {
        ++p;
        if (*p != *s)
          return (0);
      }
      break;
    case '/':
      if (*s != '/' && *s != '\0')
        return (0);

      p = pm_slashskip(p);
      s = pm_slashskip(s);
      if (*p == '\0' && (flags & PATHMATCH_NO_ANCHOR_END))
        return (1);
      --p;
      --s;
      break;
    case '$':

      if (p[1] == '\0' && (flags & PATHMATCH_NO_ANCHOR_END)) {

        return (*pm_slashskip(s) == '\0');
      }

    default:
      if (*p != *s)
        return (0);
      break;
    }
    ++p;
    ++s;
  }
}

static int pm_w(const wchar_t *p, const wchar_t *s, int flags) {
  const wchar_t *end;

  if (s[0] == L'.' && s[1] == L'/')
    s = pm_slashskip_w(s + 1);
  if (p[0] == L'.' && p[1] == L'/')
    p = pm_slashskip_w(p + 1);

  for (;;) {
    switch (*p) {
    case L'\0':
      if (s[0] == L'/') {
        if (flags & PATHMATCH_NO_ANCHOR_END)
          return (1);

        s = pm_slashskip_w(s);
      }
      return (*s == L'\0');
    case L'?':

      if (*s == L'\0')
        return (0);
      break;
    case L'*':

      while (*p == L'*')
        ++p;

      if (*p == L'\0')
        return (1);
      while (*s) {
        if (archive_pathmatch_w(p, s, flags))
          return (1);
        ++s;
      }
      return (0);
    case L'[':

      end = p + 1;
      while (*end != L'\0' && *end != L']') {
        if (*end == L'\\' && end[1] != L'\0')
          ++end;
        ++end;
      }
      if (*end == L']') {

        if (!pm_list_w(p + 1, end, *s, flags))
          return (0);
        p = end;
        break;
      } else

          if (*p != *s)
        return (0);
      break;
    case L'\\':

      if (p[1] == L'\0') {
        if (*s != L'\\')
          return (0);
      } else {
        ++p;
        if (*p != *s)
          return (0);
      }
      break;
    case L'/':
      if (*s != L'/' && *s != L'\0')
        return (0);

      p = pm_slashskip_w(p);
      s = pm_slashskip_w(s);
      if (*p == L'\0' && (flags & PATHMATCH_NO_ANCHOR_END))
        return (1);
      --p;
      --s;
      break;
    case L'$':

      if (p[1] == L'\0' && (flags & PATHMATCH_NO_ANCHOR_END)) {

        return (*pm_slashskip_w(s) == L'\0');
      }

    default:
      if (*p != *s)
        return (0);
      break;
    }
    ++p;
    ++s;
  }
}

int __archive_pathmatch(const char *p, const char *s, int flags) {

  if (p == NULL || *p == '\0')
    return (s == NULL || *s == '\0');
  else if (s == NULL)
    return (0);

  if (*p == '^') {
    ++p;
    flags &= ~PATHMATCH_NO_ANCHOR_START;
  }

  if (*p == '/' && *s != '/')
    return (0);

  if (*p == '*' || *p == '/') {
    while (*p == '/')
      ++p;
    while (*s == '/')
      ++s;
    return (pm(p, s, flags));
  }

  if (flags & PATHMATCH_NO_ANCHOR_START) {
    for (; s != NULL; s = strchr(s, '/')) {
      if (*s == '/')
        s++;
      if (pm(p, s, flags))
        return (1);
    }
    return (0);
  }

  return (pm(p, s, flags));
}

int __archive_pathmatch_w(const wchar_t *p, const wchar_t *s, int flags) {

  if (p == NULL || *p == L'\0')
    return (s == NULL || *s == L'\0');
  else if (s == NULL)
    return (0);

  if (*p == L'^') {
    ++p;
    flags &= ~PATHMATCH_NO_ANCHOR_START;
  }

  if (*p == L'/' && *s != L'/')
    return (0);

  if (*p == L'*' || *p == L'/') {
    while (*p == L'/')
      ++p;
    while (*s == L'/')
      ++s;
    return (pm_w(p, s, flags));
  }

  if (flags & PATHMATCH_NO_ANCHOR_START) {
    for (; s != NULL; s = wcschr(s, L'/')) {
      if (*s == L'/')
        s++;
      if (pm_w(p, s, flags))
        return (1);
    }
    return (0);
  }

  return (pm_w(p, s, flags));
}
