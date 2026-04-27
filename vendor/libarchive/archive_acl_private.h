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

#ifndef ARCHIVE_ACL_PRIVATE_H_INCLUDED
#define ARCHIVE_ACL_PRIVATE_H_INCLUDED

#ifndef __LIBARCHIVE_BUILD
/* For editor/LSP parsing, prefer system types when available. Only provide
 * a guarded fallback typedef if a system header did not supply them. */
#if defined(__has_include) && __has_include(<sys/types.h>)
#include <sys/types.h>
#endif

/* Fallback: only define these typedefs if no common guard macro is present.
 * We avoid testing 'mode_t' or 'ssize_t' directly (they are typedefs, not
 * macros), and instead set and test our own guard macros when providing
 * replacements. This prevents accidental redefinition when system headers
 * already define these types. */
#if !defined(_ARCHIVE_MODE_T_DEFINED) && !defined(_MODE_T) &&                  \
    !defined(_MODE_T_DEFINED) && !defined(__mode_t_defined) &&                 \
    !defined(__MINGW32__) && !defined(__MINGW64__)
/* On MinGW/MSYS the C runtime already defines `mode_t` (typedef _mode_t),
 * but it typically doesn't set one of the common guard macros we check.
 * Avoid providing our fallback typedef when compiling under MinGW to
 * prevent a conflicting redefinition. */
typedef int mode_t;
#define _ARCHIVE_MODE_T_DEFINED
#endif

#if !defined(_ARCHIVE_SSIZE_T_DEFINED) && !defined(_SSIZE_T_) &&               \
    !defined(_SSIZE_T_DEFINED) && !defined(__ssize_t_defined)
typedef long ssize_t;
#define _ARCHIVE_SSIZE_T_DEFINED
#endif

#endif

#include "archive_string.h"

struct archive_acl_entry {
  struct archive_acl_entry *next;
  int type;                    /* E.g., access or default */
  int tag;                     /* E.g., user/group/other/mask */
  int permset;                 /* r/w/x bits */
  int id;                      /* uid/gid for user/group */
  struct archive_mstring name; /* uname/gname */
};

struct archive_acl {
  mode_t mode;
  struct archive_acl_entry *acl_head;
  struct archive_acl_entry *acl_p;
  int acl_state; /* See acl_next for details. */
  wchar_t *acl_text_w;
  char *acl_text;
  int acl_types;
};

void archive_acl_clear(struct archive_acl *);
void archive_acl_copy(struct archive_acl *, struct archive_acl *);
int archive_acl_count(struct archive_acl *, int);
int archive_acl_types(struct archive_acl *);
int archive_acl_reset(struct archive_acl *, int);
int archive_acl_next(struct archive *, struct archive_acl *, int, int *, int *,
                     int *, int *, const char **);

int archive_acl_add_entry(struct archive_acl *, int, int, int, int,
                          const char *);
int archive_acl_add_entry_w_len(struct archive_acl *, int, int, int, int,
                                const wchar_t *, size_t);
int archive_acl_add_entry_len(struct archive_acl *, int, int, int, int,
                              const char *, size_t);

wchar_t *archive_acl_to_text_w(struct archive_acl *, ssize_t *, int,
                               struct archive *);
char *archive_acl_to_text_l(struct archive_acl *, ssize_t *, int,
                            struct archive_string_conv *);

/*
 * ACL text parser.
 */
int archive_acl_from_text_w(struct archive_acl *, const wchar_t * /* wtext */,
                            int /* type */);
int archive_acl_from_text_l(struct archive_acl *, const char * /* text */,
                            int /* type */, struct archive_string_conv *);
int archive_acl_from_text_nl(struct archive_acl *, const char * /* text */,
                             size_t /* size of text */, int /* type */,
                             struct archive_string_conv *);

#endif /* !ARCHIVE_ACL_PRIVATE_H_INCLUDED */
